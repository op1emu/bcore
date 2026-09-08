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
# Standalone default ON: owner-thread counters and probe/sink branches remain
# when unarmed. Hosts can compile instrumentation out (see bcore_profile.h).
option(BCORE_ENABLE_PROFILE
    "JIT observability API: code map, typed event sink, counters, block cost probe" ON)
# OFF by default: emits jitdump files on disk whenever blocks are compiled.
option(BCORE_PERF_JIT_EVENTS
    "Register LLVM's PerfJITEventListener (enables 'perf inject --jit')" OFF)

if(BCORE_ENABLE_PROFILE)
    # computeSymbolSizes() lives in the Object library.
    llvm_map_components_to_libnames(LLVM_PROFILE_LIBS object)
    list(APPEND LLVM_LIBS ${LLVM_PROFILE_LIBS})
endif()
if(BCORE_PERF_JIT_EVENTS)
    llvm_map_components_to_libnames(LLVM_PERFJIT_LIBS perfjitevents)
    list(APPEND LLVM_LIBS ${LLVM_PERFJIT_LIBS})
endif()

# ---- Core library (disassembler + emulator) ----
add_library(bfin-core STATIC
    src/core.cpp
    src/disasm_visitor.cpp
    src/lift_visitor.cpp
    src/jit_engine.cpp
    src/bb_translator.cpp
    src/syscall_emu.cpp
    src/cec.cpp
    src/evt.cpp
    src/memory.cpp
    src/cpu_state.cpp
    src/profile.cpp
)
target_include_directories(bfin-core
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LLVM_INCLUDE_DIRS}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(bfin-core PUBLIC
    ${LLVM_DEFINITIONS}
    BCORE_ENABLE_PROFILE=$<BOOL:${BCORE_ENABLE_PROFILE}>
    BCORE_PERF_JIT_EVENTS=$<BOOL:${BCORE_PERF_JIT_EVENTS}>
)
target_link_libraries(bfin-core PUBLIC ${LLVM_LIBS})
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
option(BCORE_BUILD_PROFILE_TESTS "Build focused observability regression/benchmark" OFF)
if(BCORE_BUILD_PROFILE_TESTS)
    add_executable(bcore-profile-test tests/profile_test.cpp)
    target_link_libraries(bcore-profile-test PRIVATE bfin-core)
    add_test(NAME bcore-profile-test COMMAND bcore-profile-test)
endif()
