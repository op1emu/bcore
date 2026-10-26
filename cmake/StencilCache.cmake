# Runs bcore-stencilgen, or copies what the same generator built before.
#
#   cmake -DGEN=<bcore-stencilgen> -DOUT=<cp_gen> -DCACHE=<dir> [-DARGS=<its arguments>] -P StencilCache.cmake
#
# The key is the generator's own SHA-256 (with its arguments, if any):
# anything that changes the stencils (the lifter's sources, a macro, LLVM)
# changes the binary, so an entry is never stale. A binary that changes for no reason only costs a regeneration.
# CACHE keeps the newest entry alone.

cmake_minimum_required(VERSION 3.21)  # file(COPY_FILE ... RESULT)

foreach(var GEN OUT CACHE)
    if(NOT ${var})
        message(FATAL_ERROR "StencilCache.cmake: -D${var}= is required")
    endif()
endforeach()

set(files cp_stencils.inc cp_stencils.bin cp_tables.bin cp_stencils.txt)
file(SHA256 "${GEN}" key)
if(ARGS)
    string(SHA256 key "${key};${ARGS}")
endif()
string(SUBSTRING "${key}" 0 12 short)
set(entry "${CACHE}/${key}")

set(hit TRUE)
foreach(f ${files})
    if(NOT EXISTS "${entry}/${f}")
        set(hit FALSE)
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUT}")
if(hit)
    foreach(f ${files})
        file(COPY_FILE "${entry}/${f}" "${OUT}/${f}" RESULT copied)
        if(NOT copied EQUAL 0)
            message(WARNING "stencils: ${copied}; building them")
            set(hit FALSE)
            break()
        endif()
        # Newer than GEN, or the build would come back here every time.
        file(TOUCH "${OUT}/${f}")
    endforeach()
endif()
if(hit)
    message(STATUS "stencils: cache hit ${short}")
    return()
endif()

message(STATUS "stencils: cache miss ${short}")
execute_process(COMMAND "${GEN}" "${OUT}" ${ARGS} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "bcore-stencilgen failed: ${rc}")
endif()

# The stencils are built: a cache that cannot keep them (full, read-only,
# another build clearing it) only warns.
file(GLOB old LIST_DIRECTORIES true "${CACHE}/*")
if(old)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -rf ${old})
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E make_directory "${entry}.tmp"
                RESULT_VARIABLE stored ERROR_VARIABLE why)
if(stored EQUAL 0)
    foreach(f ${files})
        file(COPY_FILE "${OUT}/${f}" "${entry}.tmp/${f}" RESULT stored)
        if(NOT stored EQUAL 0)
            set(why "${stored}")
            break()
        endif()
    endforeach()
endif()
# Complete or absent: a build stopped while copying leaves only .tmp, which
# the next miss removes.
if(stored EQUAL 0)
    file(RENAME "${entry}.tmp" "${entry}" RESULT stored)
    set(why "${stored}")
endif()
if(NOT stored EQUAL 0)
    message(WARNING "stencils: not cached: ${why}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -rf "${entry}.tmp")
endif()
