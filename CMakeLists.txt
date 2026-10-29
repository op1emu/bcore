cmake_minimum_required(VERSION 3.10)
project(bfin-recompiler CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Include directories
# (no global include_directories — per-target PRIVATE/PUBLIC below)

# ---- LLVM ----
# LLVM 15, or 23 (the version the WebAssembly build has). A given LLVM_DIR
# is the one used, whichever of the two it is: were 15 looked for first, it
# would replace a cache's LLVM_DIR of 23 with 15's.
set(LLVM_DIR "" CACHE PATH
    "Path to LLVM CMake config directory (LLVMConfig.cmake). \
Leave empty to search standard system paths for LLVM 15. \
Example: /usr/lib/llvm-15/lib/cmake/llvm")
if(LLVM_DIR)
    find_package(LLVM REQUIRED CONFIG NO_DEFAULT_PATH PATHS ${LLVM_DIR})
else()
    find_package(LLVM 15 REQUIRED CONFIG)
endif()
if(NOT LLVM_VERSION_MAJOR EQUAL 15 AND NOT LLVM_VERSION_MAJOR EQUAL 23)
    message(FATAL_ERROR "LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}: bcore builds with LLVM 15 or 23")
endif()
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

if(EMSCRIPTEN)
    # WebAssembly (tools/wasm/README.md): LLVM's WebAssembly backend,
    # which emits each block's object, and its Object library, which reads
    # it back (wasm_object.h); no JIT.
    llvm_map_components_to_libnames(LLVM_LIBS core support passes object
        webassemblycodegen webassemblydesc webassemblyinfo)
else()
    llvm_map_components_to_libnames(LLVM_LIBS core support orcjit native passes)
endif()

if(EMSCRIPTEN)
    # Exceptions as WebAssembly's (a helper's, caught in host_bridge), the
    # same RTTI as LLVM's (its classes are derived from here), and a link
    # at -O1 whatever the build type: wasm-opt's -O2 and up took 75 s for
    # each executable, and its larger functions started three times slower
    # (V8 compiled them again); the code runs as fast either way.
    add_compile_options(-fwasm-exceptions)
    if(NOT LLVM_ENABLE_RTTI)
        add_compile_options(-fno-rtti)
    endif()
    add_link_options(-fwasm-exceptions -O1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB
        -sALLOW_TABLE_GROWTH=1 -sSTACK_SIZE=8MB)
    # The executables built here (the tools, the tests) run in Node, on the
    # host's files. Node 24 or newer (CMAKE_CROSSCOMPILING_EMULATOR, which
    # runs the tests): Node 20's V8 copied the whole table into every
    # instance that imports it, a block's module each, memory as the square
    # of the blocks (V8 13.3 has one for all, Chrome 133 on).
    add_link_options(-sNODERAWFS=1 -sEXIT_RUNTIME=1)
    if(NOT CMAKE_CROSSCOMPILING_EMULATOR)
        message(FATAL_ERROR "-DCMAKE_CROSSCOMPILING_EMULATOR=<Node 24 or newer> runs the WebAssembly build's executables")
    endif()
    execute_process(COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} --version OUTPUT_VARIABLE BCORE_NODE_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT BCORE_NODE_VERSION MATCHES "^v([0-9]+)\\." OR CMAKE_MATCH_1 LESS 24)
        message(FATAL_ERROR "${CMAKE_CROSSCOMPILING_EMULATOR} is Node ${BCORE_NODE_VERSION}: the WebAssembly build's executables need 24 or newer")
    endif()
endif()

# ---- Profiling / observability ----
# Standalone default ON: the code map, event sink and counters cost a null
# check and a few increments per dispatch. Hosts can compile them out (see
# bcore_profile.h).
# Off on WebAssembly, where there is no code to map (the blocks are
# modules) nor perf.
if(EMSCRIPTEN)
    set(BCORE_ENABLE_PROFILE_DEFAULT OFF)
else()
    set(BCORE_ENABLE_PROFILE_DEFAULT ON)
endif()
option(BCORE_ENABLE_PROFILE
    "JIT observability API: code map, typed event sink, counters" ${BCORE_ENABLE_PROFILE_DEFAULT})
if(BCORE_ENABLE_PROFILE AND EMSCRIPTEN)
    message(FATAL_ERROR "BCORE_ENABLE_PROFILE: no profiling on WebAssembly")
endif()
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

# ---- Copy-and-patch (cp_format.h): x86-64, AArch64 and WebAssembly ----
# On by default where the stencils run; OFF builds the core without them
# anywhere (Core::set_copy_patch then does nothing). The native stencils are
# LLVM 15's (LLVM 23 builds without them); WebAssembly's (cp_wasm.h) LLVM
# 23's, the LLVM of its build.
if(EMSCRIPTEN)
    if(LLVM_VERSION_MAJOR EQUAL 23)
        set(BCORE_COPY_PATCH_DEFAULT ON)
    else()
        set(BCORE_COPY_PATCH_DEFAULT OFF)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|aarch64|arm64|ARM64)$" AND LLVM_VERSION_MAJOR EQUAL 15)
    set(BCORE_COPY_PATCH_DEFAULT ON)
else()
    set(BCORE_COPY_PATCH_DEFAULT OFF)
endif()
option(BCORE_COPY_PATCH "Copy-and-patch tier 0 (x86-64, AArch64 with LLVM 15; WebAssembly)" ${BCORE_COPY_PATCH_DEFAULT})
if(BCORE_COPY_PATCH AND NOT BCORE_COPY_PATCH_DEFAULT)
    message(FATAL_ERROR "BCORE_COPY_PATCH needs an x86-64 or AArch64 host with LLVM 15, or WebAssembly")
endif()
if(EMSCRIPTEN AND BCORE_COPY_PATCH)
    # The stencils are data in memory (cp_stencils.cpp), more than the
    # default 16 MiB it starts with.
    add_link_options(-sINITIAL_MEMORY=128MB)
endif()

# ---- The core in three libraries ----
# bcore-runtime: what blocks run on and the disassembler, without LLVM (the
# CPU state, the CEC and event controller, the helpers' C functions, the
# syscalls). bcore-lift: guest code to object files (the lifter, its IR
# passes, Compiler). bcore-engine: the objects loaded and run (JitEngine
# over the host's CodeLoader, the promotion worker, profiling). bfin-core,
# below, is Core over the three. bcore-stencilgen links bcore-lift alone.
add_library(bcore-runtime STATIC
    src/disasm_visitor.cpp
    src/host_bridge.cpp
    src/syscall_emu.cpp
    src/cec.cpp
    src/evt.cpp
    src/memory.cpp
    src/cpu_state.cpp
)
target_include_directories(bcore-runtime
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(bcore-runtime PUBLIC
    BCORE_ENABLE_PROFILE=$<BOOL:${BCORE_ENABLE_PROFILE}>
    BCORE_PERF_JIT_EVENTS=$<BOOL:${BCORE_PERF_JIT_EVENTS}>
)
set_target_properties(bcore-runtime PROPERTIES FOLDER "Core")

# The stencils' pieces of the lifter only with copy-and-patch (below).
set(BCORE_LIFT_CP_SOURCES)
if(BCORE_COPY_PATCH)
    set(BCORE_LIFT_CP_SOURCES src/bb_stencil.cpp src/cp_build.cpp)
endif()
add_library(bcore-lift STATIC
    src/lift_visitor.cpp
    src/compiler.cpp
    src/bb_translator.cpp
    src/packet_state.cpp
    src/guest_memory.cpp
    src/guest_speculation.cpp
    src/steady_skip.cpp
    ${BCORE_LIFT_CP_SOURCES}
)
target_include_directories(bcore-lift
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(bcore-lift PUBLIC ${LLVM_DEFINITIONS})
target_link_libraries(bcore-lift PUBLIC bcore-runtime ${LLVM_LIBS})
set_target_properties(bcore-lift PROPERTIES FOLDER "Core")

# The host's CodeLoader (code_loader.h).
if(EMSCRIPTEN)
    set(BCORE_LOADER_SOURCES src/code_loader_wasm.cpp src/wasm_object.cpp)
else()
    set(BCORE_LOADER_SOURCES src/code_loader_native.cpp src/code_arena.cpp)
endif()
add_library(bcore-engine STATIC
    src/jit_engine.cpp
    ${BCORE_LOADER_SOURCES}
    src/helper_functions.cpp
    src/compile_worker.cpp
    src/profile.cpp
)
target_include_directories(bcore-engine
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LLVM_INCLUDE_DIRS}
)
find_package(Threads REQUIRED)  # the promotion worker (Core::set_tier_background)
target_link_libraries(bcore-engine PUBLIC bcore-lift Threads::Threads)
set_target_properties(bcore-engine PROPERTIES FOLDER "Core")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(library bcore-runtime bcore-lift bcore-engine)
        target_compile_options(${library} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endforeach()
endif()

# ---- Copy-and-patch stencils ----
# Built from the lifter by bcore-stencilgen whenever bcore-lift changes.
set(BCORE_CP_SOURCES)
if(BCORE_COPY_PATCH)
    add_executable(bcore-stencilgen tools/stencilgen.cpp)
    target_include_directories(bcore-stencilgen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${LLVM_INCLUDE_DIRS})
    target_link_libraries(bcore-stencilgen PRIVATE bcore-lift Threads::Threads)  # it lifts on threads
    # Each architecture's stencils it can make (--target), with the assembler
    # parser AArch64's hole materialization needs; WebAssembly's only its
    # own (they are lifted with its offsets).
    set(BCORE_GEN_ARCHS X86 AArch64)
    if(EMSCRIPTEN)
        set(BCORE_GEN_ARCHS)
        llvm_map_components_to_libnames(BCORE_GEN_WASM_LIBS webassemblycodegen webassemblydesc webassemblyinfo)
        target_link_libraries(bcore-stencilgen PRIVATE ${BCORE_GEN_WASM_LIBS})
        target_compile_definitions(bcore-stencilgen PRIVATE BCORE_STENCILGEN_WEBASSEMBLY=1)
    endif()
    foreach(arch ${BCORE_GEN_ARCHS})
        if(arch IN_LIST LLVM_TARGETS_TO_BUILD)
            string(TOLOWER ${arch} lower)
            llvm_map_components_to_libnames(BCORE_GEN_${arch}_LIBS
                ${lower}codegen ${lower}asmparser ${lower}desc ${lower}info)
            target_link_libraries(bcore-stencilgen PRIVATE ${BCORE_GEN_${arch}_LIBS})
            string(TOUPPER ${arch} upper)
            target_compile_definitions(bcore-stencilgen PRIVATE BCORE_STENCILGEN_${upper}=1)
        endif()
    endforeach()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(bcore-stencilgen PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    set_target_properties(bcore-stencilgen PROPERTIES FOLDER "Core")
    set(BCORE_CP_GEN ${CMAKE_CURRENT_BINARY_DIR}/cp_gen)
    # A directory that keeps the stencils of the last bcore-stencilgen binary
    # (cmake/StencilCache.cmake): a rebuild that links the same generator,
    # as a CI job with ccache does, copies them instead of taking minutes.
    set(BCORE_STENCIL_CACHE_DIR "" CACHE PATH "Reuse the stencils of an identical bcore-stencilgen")
    # Off: stencils without the splitter's cold parts, a baseline to measure
    # the split against.
    option(BCORE_STENCIL_SPLIT "Split the stencils' cold paths apart" ON)
    set(BCORE_STENCILGEN_ARGS "")
    if(NOT BCORE_STENCIL_SPLIT)
        list(APPEND BCORE_STENCILGEN_ARGS --no-split)
    endif()
    # WebAssembly's generator runs in Node, in a process per CPU (it has no
    # threads): tools/wasm/stencilgen.mjs.
    set(BCORE_STENCILGEN_RUN "")
    set(BCORE_STENCILGEN_WITH "")
    if(EMSCRIPTEN)
        set(BCORE_STENCILGEN_RUN ${CMAKE_CROSSCOMPILING_EMULATOR} ${CMAKE_CURRENT_SOURCE_DIR}/tools/wasm/stencilgen.mjs)
        set(BCORE_STENCILGEN_WITH $<TARGET_FILE_DIR:bcore-stencilgen>/bcore-stencilgen.wasm)
    endif()
    if(BCORE_STENCIL_CACHE_DIR)
        set(BCORE_STENCILGEN_COMMAND ${CMAKE_COMMAND} -DGEN=$<TARGET_FILE:bcore-stencilgen>
            -DOUT=${BCORE_CP_GEN} -DCACHE=${BCORE_STENCIL_CACHE_DIR} "-DARGS=${BCORE_STENCILGEN_ARGS}"
            "-DRUN=${BCORE_STENCILGEN_RUN}" "-DWITH=${BCORE_STENCILGEN_WITH}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/StencilCache.cmake)
    elseif(EMSCRIPTEN)
        set(BCORE_STENCILGEN_COMMAND ${BCORE_STENCILGEN_RUN} $<TARGET_FILE:bcore-stencilgen> ${BCORE_CP_GEN}
            ${BCORE_STENCILGEN_ARGS})
    else()
        set(BCORE_STENCILGEN_COMMAND bcore-stencilgen ${BCORE_CP_GEN} ${BCORE_STENCILGEN_ARGS})
    endif()
    add_custom_command(
        OUTPUT ${BCORE_CP_GEN}/cp_stencils.inc ${BCORE_CP_GEN}/cp_stencils.bin ${BCORE_CP_GEN}/cp_tables.bin
               ${BCORE_CP_GEN}/cp_stencils.txt
        COMMAND ${CMAKE_COMMAND} -E make_directory ${BCORE_CP_GEN}
        COMMAND ${BCORE_STENCILGEN_COMMAND}
        DEPENDS bcore-stencilgen ${CMAKE_CURRENT_SOURCE_DIR}/cmake/StencilCache.cmake
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/wasm/stencilgen.mjs
        COMMENT "Building the copy-and-patch stencils"
        VERBATIM)
    set_source_files_properties(src/cp_stencils.cpp PROPERTIES
        OBJECT_DEPENDS "${BCORE_CP_GEN}/cp_stencils.inc;${BCORE_CP_GEN}/cp_stencils.bin;${BCORE_CP_GEN}/cp_tables.bin")
    set(BCORE_CP_SOURCES src/cp_stencils.cpp src/cp_translator.cpp ${BCORE_CP_GEN}/cp_stencils.inc)
    if(EMSCRIPTEN)
        list(APPEND BCORE_CP_SOURCES src/cp_emit_wasm.cpp)
    endif()
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
target_link_libraries(bfin-core PUBLIC bcore-engine)
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
if(EMSCRIPTEN)
    # emu.js under Node as an executable, for the test scripts
    # (run_diff_test.py --emu build/emu-node).
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/emu-node
         "#!/bin/sh\nexec \"${CMAKE_CROSSCOMPILING_EMULATOR}\" \"${CMAKE_CURRENT_BINARY_DIR}/emu.js\" \"$@\"\n")
    execute_process(COMMAND chmod +x ${CMAKE_CURRENT_BINARY_DIR}/emu-node)
    # The same emu for a browser's Worker (tools/wasm/web/): a module that
    # createEmu() makes, its files in memory (FS), main run by callMain.
    add_executable(emu-web tools/emu.cpp)
    target_link_libraries(emu-web PRIVATE bfin-core)
    target_include_directories(emu-web PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_target_properties(emu-web PROPERTIES LINK_OPTIONS
        "-fwasm-exceptions;-O1;-sALLOW_MEMORY_GROWTH=1;-sINITIAL_MEMORY=128MB;-sMAXIMUM_MEMORY=4GB;-sALLOW_TABLE_GROWTH=1;-sSTACK_SIZE=8MB;-sENVIRONMENT=worker;-sMODULARIZE=1;-sEXPORT_NAME=createEmu;-sINVOKE_RUN=0;-sEXIT_RUNTIME=0;-sEXPORTED_RUNTIME_METHODS=FS,callMain")
endif()
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

    # bcore's own: a host that adds bcore as a subdirectory runs its ctest
    # without it.
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        add_test(
            NAME comparison_test
            COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_comparison_test.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
    endif()
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
    target_compile_definitions(bcore-data-alignment-test PRIVATE BCORE_COPY_PATCH_TEST=$<BOOL:${BCORE_COPY_PATCH}>)
    add_test(NAME bcore-data-alignment-test COMMAND bcore-data-alignment-test)
    add_executable(bcore-page-table-test tests/page_table_test.cpp)
    target_link_libraries(bcore-page-table-test PRIVATE bfin-core)
    target_compile_definitions(bcore-page-table-test PRIVATE BCORE_COPY_PATCH_TEST=$<BOOL:${BCORE_COPY_PATCH}>)
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
    # In two shards (its argument): alone it was the longest test by far.
    foreach(shard 0 1)
        add_test(NAME bcore-loop-block-test-${shard} COMMAND bcore-loop-block-test ${shard} 2)
        set_tests_properties(bcore-loop-block-test-${shard} PROPERTIES COST 150 LABELS slow)
    endforeach()
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
    # The native loader's code arenas (none on WebAssembly). It also builds
    # an LLJIT of its own over the arena's memory managers.
    if(NOT EMSCRIPTEN)
        add_executable(bcore-code-arena-test tests/code_arena_test.cpp)
        target_link_libraries(bcore-code-arena-test PRIVATE bfin-core)
        target_include_directories(bcore-code-arena-test PRIVATE ${LLVM_INCLUDE_DIRS})
        add_test(NAME bcore-code-arena-test COMMAND bcore-code-arena-test)
    endif()
    # count_module's vectorized loops, from fixed IR; and, on x86-64, a MAC
    # loop block compiled (not run) for Zen 4's AVX-512 and for x86-64-v3.
    add_executable(bcore-vectorize-count-test tests/vectorize_count_test.cpp)
    target_link_libraries(bcore-vectorize-count-test PRIVATE bcore-lift)
    target_include_directories(bcore-vectorize-count-test PRIVATE ${LLVM_INCLUDE_DIRS})
    llvm_map_components_to_libnames(LLVM_VECTORIZE_TEST_LIBS irreader)
    target_link_libraries(bcore-vectorize-count-test PRIVATE ${LLVM_VECTORIZE_TEST_LIBS})
    add_test(NAME bcore-vectorize-count-test-rules COMMAND bcore-vectorize-count-test rules)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        add_test(NAME bcore-vectorize-count-test-pipeline
                 COMMAND bcore-vectorize-count-test pipeline ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/mac_loop_block.ll)
    endif()
    # A host with global types and C functions named as bcore's are (Access,
    # BBInfo, Memory, ..., mem_read8) and laid out otherwise: it links, a
    # loop block still optimizes and the JIT finds its blocks.
    add_executable(bcore-host-types-test tests/host_types_test.cpp tests/host_types_bridge.cpp)
    set_source_files_properties(tests/host_types_test.cpp PROPERTIES COMPILE_OPTIONS -O0)
    target_link_libraries(bcore-host-types-test PRIVATE bcore-engine)
    target_include_directories(bcore-host-types-test PRIVATE ${LLVM_INCLUDE_DIRS})
    target_link_libraries(bcore-host-types-test PRIVATE ${LLVM_VECTORIZE_TEST_LIBS})
    add_test(NAME bcore-host-types-test
             COMMAND bcore-host-types-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/mac_loop_block.ll)
    # Every symbol the libraries export names only bcore's, the standard
    # library's and LLVM's names: a host may have any global of its own.
    find_program(BCORE_LLVM_NM llvm-nm HINTS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
    if(BCORE_LLVM_NM)
        add_test(NAME bcore-exported-symbols-test
                 COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_exported_symbols.py --nm ${BCORE_LLVM_NM}
                         $<TARGET_FILE:bcore-runtime> $<TARGET_FILE:bcore-lift> $<TARGET_FILE:bcore-engine>
                         $<TARGET_FILE:bfin-core>)
    endif()
    add_test(NAME bcore-exported-symbols-unittest
             COMMAND python3 -m unittest tests/test_check_exported_symbols.py
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    # Blocks compiled for WebAssembly (not run) on any host whose LLVM has
    # the target: in the blocks' convention there, and objects the
    # WebAssembly loader runs as they are (wasm_object.h).
    if("WebAssembly" IN_LIST LLVM_TARGETS_TO_BUILD)
        if(EMSCRIPTEN)
            add_executable(bcore-wasm-object-test tests/wasm_object_test.cpp)
        else()
            add_executable(bcore-wasm-object-test tests/wasm_object_test.cpp src/wasm_object.cpp)
        endif()
        target_link_libraries(bcore-wasm-object-test PRIVATE bfin-core)
        target_include_directories(bcore-wasm-object-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${LLVM_INCLUDE_DIRS})
        llvm_map_components_to_libnames(LLVM_WASM_TEST_LIBS
            webassemblycodegen webassemblydesc webassemblyinfo object)
        target_link_libraries(bcore-wasm-object-test PRIVATE ${LLVM_WASM_TEST_LIBS})
        add_test(NAME bcore-wasm-object-test COMMAND bcore-wasm-object-test)
    endif()
    # AArch64 stencils compiled (not run) on any host whose LLVM has the
    # target: their relocations, their holes' widths, how they are filled in.
    # (The stencils are LLVM 15's, and their pieces of the lifter are built
    # with copy-and-patch.)
    if("AArch64" IN_LIST LLVM_TARGETS_TO_BUILD AND BCORE_COPY_PATCH AND NOT EMSCRIPTEN)
        add_executable(bcore-cp-a64-test tests/cp_a64_test.cpp)
        target_link_libraries(bcore-cp-a64-test PRIVATE bcore-lift)
        target_include_directories(bcore-cp-a64-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${LLVM_INCLUDE_DIRS})
        llvm_map_components_to_libnames(LLVM_A64_TEST_LIBS
            aarch64codegen aarch64asmparser aarch64desc aarch64info)
        target_link_libraries(bcore-cp-a64-test PRIVATE ${LLVM_A64_TEST_LIBS})
        add_test(NAME bcore-cp-a64-test COMMAND bcore-cp-a64-test)
    endif()
    if(BCORE_COPY_PATCH)
        add_executable(bcore-cp-stencil-test tests/cp_stencil_test.cpp)
        target_link_libraries(bcore-cp-stencil-test PRIVATE bfin-core)
        # Every third of the build's stencils' samples (a 32-bit key has four:
        # each key's are all reached), in four shards that run in parallel,
        # and 100000 random parallel packets.
        foreach(offset 0 3 6 9)
            add_test(NAME bcore-cp-stencil-test-${offset} COMMAND bcore-cp-stencil-test 12 ${offset})
            set_tests_properties(bcore-cp-stencil-test-${offset} PROPERTIES COST 200 LABELS slow)
        endforeach()
        add_test(NAME bcore-cp-stencil-test-parallel COMMAND bcore-cp-stencil-test 1 parallel 100000)
        set_tests_properties(bcore-cp-stencil-test-parallel PROPERTIES COST 300 LABELS slow)
        # A Memory whose callbacks change registers: every parallel packet
        # compiled with LLVM, the rest still stencils.
        add_test(NAME bcore-cp-stencil-test-clobber COMMAND bcore-cp-stencil-test 32 clobber 20000)
        set_tests_properties(bcore-cp-stencil-test-clobber PROPERTIES COST 200 LABELS slow)
        # Every 24th at a high PC (L1 instruction SRAM, op1emu's).
        add_test(NAME bcore-cp-stencil-test-high COMMAND bcore-cp-stencil-test 24 5 high)
        set_tests_properties(bcore-cp-stencil-test-high PROPERTIES COST 100 LABELS slow)
        # Not on WebAssembly, where LLVM's tier 0 compiles no block that long:
        # its -O0 code has more locals than V8 takes in a function (50000).
        if(NOT EMSCRIPTEN)
            add_test(NAME bcore-cp-stencil-test-long COMMAND bcore-cp-stencil-test long)
        endif()
    endif()
    # Seconds each takes on a CI runner: ctest -j starts the slowest first.
    # A test that takes a minute or more is also labelled slow; CI runs every
    # test in every configuration but one (ctest -LE slow there).
    set_tests_properties(bcore-tier-test PROPERTIES COST 100 LABELS slow)
    set_tests_properties(bcore-steady-skip-test PROPERTIES COST 50 LABELS slow)
    set_tests_properties(bcore-lazy-state-test PROPERTIES COST 36 LABELS slow)
endif()
