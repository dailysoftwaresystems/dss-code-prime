# ═══ THE BUILD STAMP'S DECLARATION, CHECKED ════════════════════════════════════
#
# `include()`d by the top-level CMakeLists right after it declares what the
# compiler is built from (`DSS_BUILD_STAMP_INPUTS`, `DSS_BUILD_STAMP_SCRIPT_DIRS`;
# `cmake/DssBuildStamp.cmake` explains both). The stamp moves only when a
# declared path does, so a declaration that MISSES a path the build reads is the
# under-invalidation the stamp exists to make impossible: the runtime object
# cache would serve an artifact a different compiler built, and nothing would
# say so. This file is what keeps the declaration honest about the one thing a
# configure can enumerate — the directories it processes.
#
# dss_build_stamp_check_declaration(<inputs variable> <script dirs variable>)
#   Called DEFERRED, at the end of the calling directory, so it sees every
#   `add_subdirectory` that directory and its descendants made. Walks that tree
#   of directories (iteratively — a work list, never a recursion) and refuses,
#   at configure time and by name, a directory that lies under no declared
#   input and no declared script directory. Both variables hold the
#   comma-separated, source-relative paths the stamp script is handed.
#   Not judged: a directory in the build tree, or outside the source tree — a
#   fetched dependency is both, and its version is pinned in the top-level
#   CMakeLists, which is an input.
#   ⓘ SCOPE, stated rather than implied: DIRECTORIES only. A script
#   `include()`d from outside the declared paths, or a file read with
#   `file(READ)` at configure time, is not followed (✔READ 2026-10-06: none
#   exists — every `include()` names `cmake/`, and the one configure-time read
#   outside the inputs is the examples' manifest glob, which registers tests).
#   Nor is a SOURCE FILE a target lists from outside the declared paths
#   (`add_library(x ../elsewhere/y.c)`), or an include directory there: the walk
#   sees where the scripts are, never what they compile (✔READ 2026-10-06: the
#   product targets — `dsscp-lib`, `dsscp` and the object libraries they link —
#   are declared under `src/` and list their sources there, and every include
#   directory they name is inside it or in the build tree, where generated
#   headers are written).
#
# Proven by `build/build_stamp_identity`, which configures fixture projects
# against this file (see THE SELF-TEST in `cmake/DssBuildStamp.cmake`).

function(dss_build_stamp_check_declaration inputs_var scripts_var)
    foreach(_dss_var IN ITEMS "${inputs_var}" "${scripts_var}")
        if(NOT DEFINED "${_dss_var}" OR "${${_dss_var}}" STREQUAL "")
            message(FATAL_ERROR "DssBuildStampDeclaration: `${_dss_var}` is not set — the build stamp has no "
                                "declaration of what the compiler is built from to check.")
        endif()
    endforeach()
    string(REPLACE "," ";" _dss_declared "${${inputs_var}};${${scripts_var}}")
    set(_dss_roots "")
    foreach(_dss_rel IN LISTS _dss_declared)
        set(_dss_abs "${CMAKE_SOURCE_DIR}/${_dss_rel}")
        cmake_path(NORMAL_PATH _dss_abs)
        list(APPEND _dss_roots "${_dss_abs}")
    endforeach()
    set(_dss_queue "${CMAKE_CURRENT_SOURCE_DIR}")
    set(_dss_judged 0)
    set(_dss_outside "")
    while(_dss_queue)
        list(POP_FRONT _dss_queue _dss_dir)
        get_property(_dss_subs DIRECTORY "${_dss_dir}" PROPERTY SUBDIRECTORIES)
        foreach(_dss_sub IN LISTS _dss_subs)
            cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${_dss_sub}" NORMALIZE _dss_in_build)
            cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${_dss_sub}" NORMALIZE _dss_in_source)
            if(_dss_in_build OR NOT _dss_in_source)
                continue()
            endif()
            math(EXPR _dss_judged "${_dss_judged} + 1")
            set(_dss_covered FALSE)
            foreach(_dss_root IN LISTS _dss_roots)
                cmake_path(IS_PREFIX _dss_root "${_dss_sub}" NORMALIZE _dss_under)
                if(_dss_under)
                    set(_dss_covered TRUE)
                    break()
                endif()
            endforeach()
            if(NOT _dss_covered)
                file(RELATIVE_PATH _dss_shown "${CMAKE_SOURCE_DIR}" "${_dss_sub}")
                list(APPEND _dss_outside "${_dss_shown}")
            endif()
            list(APPEND _dss_queue "${_dss_sub}")
        endforeach()
    endwhile()
    if(_dss_outside)
        list(JOIN _dss_outside ", " _dss_outside)
        message(FATAL_ERROR
            "DssBuildStampDeclaration: configure processed ${_dss_outside}, which lies under no declared input "
            "(${${inputs_var}}) and no declared script directory (${${scripts_var}}). The build stamp would not "
            "move when it does. Declare it in the top-level CMakeLists: in `${inputs_var}` when the compiler is "
            "built from what it holds, or in `${scripts_var}` when only its CMake scripts are.")
    endif()
    if(_dss_judged EQUAL 0)
        message(FATAL_ERROR "DssBuildStampDeclaration: the walk from ${CMAKE_CURRENT_SOURCE_DIR} judged no "
                            "directory at all — the check collapsed, which is not a pass.")
    endif()
endfunction()
