cmake_minimum_required(VERSION 3.10)
project(bfin-recompiler CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Include directories
# (no global include_directories — per-target PRIVATE/PUBLIC below)

# ---- LLVM ----
set(LLVM_DIR "" CACHE PATH
    "Path to LLVM CMake config directory (LLVMConfig.cmake). \
Leave empty to search standard system paths. \
Example: /usr/lib/llvm-15/lib/cmake/llvm")
find_package(LLVM 15 REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

llvm_map_components_to_libnames(LLVM_LIBS core support orcjit native passes)

# ---- Profiling / observability ----
# Standalone default ON: the code map, event sink and counters cost a null
# check and a few increments per dispatch. Hosts can compile them out (see
# bcore_profile.h).
option(BCORE_ENABLE_PROFILE
    "JIT observability API: code map, typed event sink, counters" ON)
# OFF by default: emits jitdump files on disk whenever blocks are compiled.
option(BCORE_PERF_JIT_EVENTS
    "Register LLVM's PerfJITEventListener (enables 'perf inject --jit')" OFF)

if(BCORE_ENABLE_PROFILE)
    # computeSymbolSizes() lives in the Object library.
    llvm_map_components_to_libnames(LLVM_PROFILE_LIBS object)
    list(APPEND LLVM_LIBS ${LLVM_PROFILE_LIBS})
endif()
if(BCORE_PERF_JIT_EVENTS AND NOT BCORE_ENABLE_PROFILE)
    # The jitdump listener is attached by the profiling code; without it
    # set_perf_jitdump() would silently do nothing.
    message(FATAL_ERROR "BCORE_PERF_JIT_EVENTS requires BCORE_ENABLE_PROFILE")
endif()
if(BCORE_PERF_JIT_EVENTS)
    llvm_map_components_to_libnames(LLVM_PERFJIT_LIBS perfjitevents)
    list(APPEND LLVM_LIBS ${LLVM_PERFJIT_LIBS})
endif()

# ---- The lifter, the compilers and the JIT: all of the core but Core ----
# bcore-stencilgen links it to build the stencils bfin-core then holds.
add_library(bcore-lift STATIC
    src/disasm_visitor.cpp
    src/lift_visitor.cpp
    src/code_arena.cpp
    src/jit_engine.cpp
    src/compiler.cpp
    src/compile_worker.cpp
    src/bb_translator.cpp
    src/bb_stencil.cpp
    src/cp_build.cpp
    src/packet_state.cpp
    src/guest_memory.cpp
    src/guest_speculation.cpp
    src/steady_skip.cpp
    src/syscall_emu.cpp
    src/cec.cpp
    src/evt.cpp
    src/memory.cpp
    src/cpu_state.cpp
    src/profile.cpp
)
target_include_directories(bcore-lift
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LLVM_INCLUDE_DIRS}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(bcore-lift PUBLIC
    ${LLVM_DEFINITIONS}
    BCORE_ENABLE_PROFILE=$<BOOL:${BCORE_ENABLE_PROFILE}>
    BCORE_PERF_JIT_EVENTS=$<BOOL:${BCORE_PERF_JIT_EVENTS}>
)
find_package(Threads REQUIRED)  # the promotion worker (Core::set_tier_background)
target_link_libraries(bcore-lift PUBLIC ${LLVM_LIBS} Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bcore-lift PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
set_target_properties(bcore-lift PROPERTIES FOLDER "Core")

# ---- Copy-and-patch stencils (cp_format.h): x86-64 hosts ----
# Built from the lifter by bcore-stencilgen whenever bcore-lift changes.
# On by default where the stencils run (x86-64); OFF builds the core
# without them anywhere (Core::set_copy_patch then does nothing).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(BCORE_COPY_PATCH_DEFAULT ON)
else()
    set(BCORE_COPY_PATCH_DEFAULT OFF)
endif()
option(BCORE_COPY_PATCH "Copy-and-patch tier 0 (x86-64 only)" ${BCORE_COPY_PATCH_DEFAULT})
if(BCORE_COPY_PATCH AND NOT BCORE_COPY_PATCH_DEFAULT)
    message(FATAL_ERROR "BCORE_COPY_PATCH needs an x86-64 host")
endif()
set(BCORE_CP_SOURCES)
if(BCORE_COPY_PATCH)
    add_executable(bcore-stencilgen tools/stencilgen.cpp)
    target_include_directories(bcore-stencilgen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${LLVM_INCLUDE_DIRS})
    target_link_libraries(bcore-stencilgen PRIVATE bcore-lift)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(bcore-stencilgen PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    set_target_properties(bcore-stencilgen PROPERTIES FOLDER "Core")
    set(BCORE_CP_GEN ${CMAKE_CURRENT_BINARY_DIR}/cp_gen)
    add_custom_command(
        OUTPUT ${BCORE_CP_GEN}/cp_stencils.inc ${BCORE_CP_GEN}/cp_stencils.bin ${BCORE_CP_GEN}/cp_tables.bin
               ${BCORE_CP_GEN}/cp_stencils.txt
        COMMAND ${CMAKE_COMMAND} -E make_directory ${BCORE_CP_GEN}
        COMMAND bcore-stencilgen ${BCORE_CP_GEN}
        DEPENDS bcore-stencilgen
        COMMENT "Building the copy-and-patch stencils"
        VERBATIM)
    set_source_files_properties(src/cp_stencils.cpp PROPERTIES
        OBJECT_DEPENDS "${BCORE_CP_GEN}/cp_stencils.inc;${BCORE_CP_GEN}/cp_stencils.bin;${BCORE_CP_GEN}/cp_tables.bin")
    set(BCORE_CP_SOURCES src/cp_stencils.cpp src/cp_translator.cpp ${BCORE_CP_GEN}/cp_stencils.inc)
endif()

# ---- Core library (disassembler + emulator) ----
add_library(bfin-core STATIC
    src/core.cpp
    ${BCORE_CP_SOURCES}
)
target_include_directories(bfin-core
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LLVM_INCLUDE_DIRS}
        ${BCORE_CP_GEN}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(bfin-core PRIVATE
    BCORE_COPY_PATCH=$<BOOL:${BCORE_COPY_PATCH}>
    CP_STENCILS_BIN="${BCORE_CP_GEN}/cp_stencils.bin"
    CP_TABLES_BIN="${BCORE_CP_GEN}/cp_tables.bin"
)
target_link_libraries(bfin-core PUBLIC bcore-lift)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bfin-core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
set_target_properties(bfin-core PROPERTIES FOLDER "Core")

# ---- Executables ----
add_executable(disasm tools/disasm.cpp)
target_link_libraries(disasm PRIVATE bfin-core)
target_include_directories(disasm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(disasm PROPERTIES FOLDER "Disassembler")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(disasm PRIVATE -Wall -Wextra)
endif()

add_executable(emu tools/emu.cpp)
target_link_libraries(emu PRIVATE bfin-core)
# Export symbols so JIT can find extern "C" functions at runtime
set_target_properties(emu PROPERTIES ENABLE_EXPORTS TRUE FOLDER "Emulator")
target_include_directories(emu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(emu PRIVATE -Wall -Wextra)
endif()

# ---- Blackfin cross-toolchain (test fixtures only) ----
# bfin-elf-gcc/ld/objdump are used only in add_custom_command — NOT as
# CMAKE_CXX_COMPILER. CMake cross-compilation toolchain files don't apply.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(BfinToolchain)   # sets BFIN_GCC, BFIN_LD, BFIN_OBJDUMP, BFIN_TOOLCHAIN_FOUND

# ---- Test fixtures: fetch from external repo (GPL-licensed, not in this tree) ----
include(FetchContent)
FetchContent_Declare(
    bfin_testsuite
    GIT_REPOSITORY https://github.com/op1emu/bfin_sim.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
    SOURCE_DIR     ${CMAKE_BINARY_DIR}/bfin_testsuite_src
)
FetchContent_MakeAvailable(bfin_testsuite)
set(TESTSUITE_DIR ${CMAKE_BINARY_DIR}/bfin_testsuite_src/testsuite)

# Find all assembly test files
# Note: file(GLOB) is evaluated at configure time; re-run cmake after the external
# repo is updated and the build tree is refreshed.
file(GLOB asm_files
    ${TESTSUITE_DIR}/*.s  ${TESTSUITE_DIR}/*.S
    tests/benchmarks/*.s  tests/benchmarks/*.S)

# Enable testing
enable_testing()

if(asm_files AND BFIN_TOOLCHAIN_FOUND)
    list(LENGTH asm_files asm_count)
    message(STATUS "Found ${asm_count} assembly test files to build")

    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test_objects)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test_linked)

    foreach(asm_file ${asm_files})
        get_filename_component(asm_name ${asm_file} NAME)
        set(obj_file "${CMAKE_CURRENT_BINARY_DIR}/test_objects/${asm_name}.o")
        set(elf_file "${CMAKE_CURRENT_BINARY_DIR}/test_linked/${asm_name}.elf")

        add_custom_command(
            OUTPUT  ${obj_file}
            COMMAND ${BFIN_GCC} -c -o ${obj_file} ${asm_file}
                    -I${TESTSUITE_DIR}
            DEPENDS ${asm_file}
            COMMENT "Assembling ${asm_name}"
            WORKING_DIRECTORY ${TESTSUITE_DIR}
            VERBATIM
        )
        add_custom_command(
            OUTPUT  ${elf_file}
            COMMAND ${BFIN_LD} -e __start -o ${elf_file} ${obj_file}
            DEPENDS ${obj_file}
            COMMENT "Linking ${asm_name}.elf"
            VERBATIM
        )
        list(APPEND obj_files ${obj_file})
        list(APPEND elf_files ${elf_file})
    endforeach()

    add_custom_target(assemble-tests ALL DEPENDS ${obj_files})
    set_target_properties(assemble-tests PROPERTIES FOLDER "Tests")

    add_custom_target(link-tests ALL DEPENDS ${elf_files})
    add_dependencies(link-tests assemble-tests)
    set_target_properties(link-tests PROPERTIES FOLDER "Tests")

    # Generate build/test_config.py so Python test scripts can find bfin-elf-objdump
    # without hardcoding the path.
    file(GENERATE
        OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/test_config.py"
        CONTENT "# Auto-generated by CMake. Do not edit.\nBFIN_OBJDUMP = r\"${BFIN_OBJDUMP}\"\n"
    )

    add_custom_target(run-comparison-tests
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_comparison_test.py
        DEPENDS disasm assemble-tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running disassembler comparison tests against bfin-elf-objdump"
    )
    set_target_properties(run-comparison-tests PROPERTIES FOLDER "Tests")

    add_test(
        NAME comparison_test
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_comparison_test.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
else()
    if(NOT BFIN_TOOLCHAIN_FOUND)
        message(STATUS "Skipping test assembly/link targets (bfin toolchain not found)")
    else()
        message(STATUS "No assembly test files found in fetched testsuite or tests/benchmarks/")
    endif()
endif()

option(BCORE_BUILD_PROFILE_TESTS "Build the observability regression test" OFF)
if(BCORE_BUILD_PROFILE_TESTS)
    add_executable(bcore-profile-test tests/profile_test.cpp)
    target_link_libraries(bcore-profile-test PRIVATE bfin-core)
    # The compile-metrics test hashes a hand-made object through the internal API.
    target_include_directories(bcore-profile-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${LLVM_INCLUDE_DIRS})
    llvm_map_components_to_libnames(LLVM_PROFILE_TEST_LIBS irreader)
    target_link_libraries(bcore-profile-test PRIVATE ${LLVM_PROFILE_TEST_LIBS})
    add_test(NAME bcore-profile-test COMMAND bcore-profile-test)
endif()

option(BCORE_BUILD_FAULT_TESTS "Build hardware-fault regression tests" OFF)
if(BCORE_BUILD_FAULT_TESTS)
    add_executable(bcore-data-alignment-test tests/data_alignment_test.cpp)
    target_link_libraries(bcore-data-alignment-test PRIVATE bfin-core)
    add_test(NAME bcore-data-alignment-test COMMAND bcore-data-alignment-test)
    add_executable(bcore-page-table-test tests/page_table_test.cpp)
    target_link_libraries(bcore-page-table-test PRIVATE bfin-core)
    add_test(NAME bcore-page-table-test COMMAND bcore-page-table-test)
    add_executable(bcore-chain-test tests/chain_test.cpp)
    target_link_libraries(bcore-chain-test PRIVATE bfin-core)
    target_compile_definitions(bcore-chain-test PRIVATE BCORE_COPY_PATCH_TEST=$<BOOL:${BCORE_COPY_PATCH}>)
    add_test(NAME bcore-chain-test COMMAND bcore-chain-test)
    add_executable(bcore-hwloop-jump-test tests/hwloop_jump_test.cpp)
    target_link_libraries(bcore-hwloop-jump-test PRIVATE bfin-core)
    target_compile_definitions(bcore-hwloop-jump-test PRIVATE BCORE_COPY_PATCH_TEST=$<BOOL:${BCORE_COPY_PATCH}>)
    add_test(NAME bcore-hwloop-jump-test COMMAND bcore-hwloop-jump-test)
    add_executable(bcore-lazy-state-test tests/lazy_state_test.cpp)
    target_link_libraries(bcore-lazy-state-test PRIVATE bfin-core)
    add_test(NAME bcore-lazy-state-test COMMAND bcore-lazy-state-test)
    add_executable(bcore-loop-block-test tests/loop_block_test.cpp)
    target_link_libraries(bcore-loop-block-test PRIVATE bfin-core)
    add_test(NAME bcore-loop-block-test COMMAND bcore-loop-block-test)
    add_executable(bcore-steady-skip-test tests/steady_skip_test.cpp)
    target_link_libraries(bcore-steady-skip-test PRIVATE bfin-core)
    add_test(NAME bcore-steady-skip-test COMMAND bcore-steady-skip-test)
    add_executable(bcore-lb-guard-test tests/lb_guard_test.cpp)
    target_link_libraries(bcore-lb-guard-test PRIVATE bfin-core)
    add_test(NAME bcore-lb-guard-test COMMAND bcore-lb-guard-test)
    add_executable(bcore-tier-test tests/tier_test.cpp)
    target_link_libraries(bcore-tier-test PRIVATE bfin-core)
    target_compile_definitions(bcore-tier-test PRIVATE BCORE_COPY_PATCH_TEST=$<BOOL:${BCORE_COPY_PATCH}>)
    target_include_directories(bcore-tier-test PRIVATE ${LLVM_INCLUDE_DIRS})
    add_test(NAME bcore-tier-test COMMAND bcore-tier-test)
    add_executable(bcore-code-arena-test tests/code_arena_test.cpp)
    target_link_libraries(bcore-code-arena-test PRIVATE bfin-core)
    # It also builds an LLJIT of its own over the arena's memory managers.
    target_include_directories(bcore-code-arena-test PRIVATE ${LLVM_INCLUDE_DIRS})
    add_test(NAME bcore-code-arena-test COMMAND bcore-code-arena-test)
    if(BCORE_COPY_PATCH)
        add_executable(bcore-cp-stencil-test tests/cp_stencil_test.cpp)
        target_link_libraries(bcore-cp-stencil-test PRIVATE bfin-core)
        # Every third of the build's stencils' samples (a 32-bit key has four:
        # each key's are all reached), and 100000 random parallel packets.
        add_test(NAME bcore-cp-stencil-test COMMAND bcore-cp-stencil-test 3)
        set_tests_properties(bcore-cp-stencil-test PROPERTIES COST 400)
        add_test(NAME bcore-cp-stencil-test-parallel COMMAND bcore-cp-stencil-test 1 parallel 100000)
        set_tests_properties(bcore-cp-stencil-test-parallel PROPERTIES COST 160)
    endif()
    # Seconds each takes on a CI runner: ctest -j starts the slowest first.
    set_tests_properties(bcore-loop-block-test PROPERTIES COST 300)
    set_tests_properties(bcore-tier-test PROPERTIES COST 100)
    set_tests_properties(bcore-steady-skip-test PROPERTIES COST 50)
    set_tests_properties(bcore-lazy-state-test PROPERTIES COST 36)
endif()
