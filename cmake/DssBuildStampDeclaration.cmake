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
# dss_build_stamp_handed_dependencies(<out variable>)
#   The dependency sources this configure was HANDED instead of fetching them,
#   as CMake's own FetchContent records a hand-over: every cache entry
#   `FETCHCONTENT_SOURCE_DIR_<NAME>` that holds a path. Sets the variable to a
#   comma-separated list of `<NAME>=<absolute directory>`, sorted by name, EMPTY
#   where nothing was handed. An entry that is EMPTY hands nothing over —
#   FetchContent writes one for every dependency it fetches itself. The ONE
#   reader of that record: the check below asks it whose a directory is, and
#   the top-level CMakeLists hands its answer to the stamp, which names every
#   handed dependency by its BYTES (`cmake/DssBuildStamp.cmake`).
#   Refused, by name: a handed path that holds one of the list's own separators
#   (`,` `=` `;`), which the stamp script could not read back.
#
# dss_build_stamp_check_declaration(<inputs variable> <script dirs variable>
#                                   <handed dependencies variable>)
#   Called DEFERRED, at the end of the calling directory, so it sees every
#   `add_subdirectory` that directory and its descendants made. Walks that tree
#   of directories (iteratively — a work list, never a recursion) and refuses,
#   at configure time and by name, a directory that lies under no declared
#   input and no declared script directory. The first two variables hold the
#   comma-separated, source-relative paths the stamp script is handed; the
#   third holds what `dss_build_stamp_handed_dependencies` answered when the
#   stamp was WIRED, which is what the stamp will digest.
#   Refused first: a record of hand-overs that is no longer the one the stamp
#   was wired with. The walk does not judge a handed dependency's directories
#   BECAUSE the stamp carries its bytes, so a hand-over recorded after the
#   wiring read them — a script that sets the cache entry itself — would be
#   excused here and missing there.
#   Not judged: a dependency's directory, because the stamp names a dependency
#   in its own way and never through the declaration. A FETCHED one lies in the
#   build tree, or outside the source tree, and its version is pinned in the
#   top-level CMakeLists, which is an input. A HANDED one
#   (`dss_build_stamp_handed_dependencies`) is not judged WHEREVER it lies, and
#   no pin holds its bytes: the stamp carries their digest instead. A copy of
#   this tree that is built with the dependency sources its original fetched
#   holds them INSIDE itself — DssHarness gives each mutation worker its own —
#   and a rule that knew a dependency only by its position refused every such
#   configure (✔MEASURED 2026-10-10: a worker's configure ended here, naming
#   the two dependencies' directories).
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

function(dss_build_stamp_handed_dependencies out_var)
    set(_dss_pairs "")
    get_cmake_property(_dss_cache CACHE_VARIABLES)
    list(SORT _dss_cache)
    foreach(_dss_entry IN LISTS _dss_cache)
        if(_dss_entry MATCHES "^FETCHCONTENT_SOURCE_DIR_(.+)$")
            set(_dss_name "${CMAKE_MATCH_1}")
            set(_dss_abs "${${_dss_entry}}")   # read as FetchContent reads it
            if(_dss_abs STREQUAL "")
                continue()                     # nothing handed: the dependency is fetched
            endif()
            cmake_path(ABSOLUTE_PATH _dss_abs BASE_DIRECTORY "${CMAKE_SOURCE_DIR}" NORMALIZE)
            if("${_dss_name}=${_dss_abs}" MATCHES "[,;]" OR _dss_abs MATCHES "=")
                message(FATAL_ERROR
                    "DssBuildStampDeclaration: the dependency '${_dss_name}' was handed from '${_dss_abs}', and that "
                    "holds a `,`, `=` or `;` — the separators of the list the build stamp reads its handed "
                    "dependencies from. Hand it from a path without one.")
            endif()
            list(APPEND _dss_pairs "${_dss_name}=${_dss_abs}")
        endif()
    endforeach()
    list(JOIN _dss_pairs "," _dss_joined)
    set(${out_var} "${_dss_joined}" PARENT_SCOPE)
endfunction()

function(dss_build_stamp_check_declaration inputs_var scripts_var handed_var)
    foreach(_dss_var IN ITEMS "${inputs_var}" "${scripts_var}")
        if(NOT DEFINED "${_dss_var}" OR "${${_dss_var}}" STREQUAL "")
            message(FATAL_ERROR "DssBuildStampDeclaration: `${_dss_var}` is not set — the build stamp has no "
                                "declaration of what the compiler is built from to check.")
        endif()
    endforeach()
    # EMPTY is an answer here ("handed none"); not set is not.
    if(NOT DEFINED "${handed_var}")
        message(FATAL_ERROR "DssBuildStampDeclaration: `${handed_var}` is not set — the build stamp was wired "
                            "without `dss_build_stamp_handed_dependencies` being asked which dependencies this "
                            "configure was handed.")
    endif()
    dss_build_stamp_handed_dependencies(_dss_pairs)
    if(NOT "${_dss_pairs}" STREQUAL "${${handed_var}}")
        message(FATAL_ERROR
            "DssBuildStampDeclaration: this configure ends with the handed dependencies '${_dss_pairs}', and the "
            "build stamp was wired with '${${handed_var}}'. A hand-over recorded after the stamp's wiring read "
            "them is one the stamp would not carry the bytes of. Hand a dependency over where CMake reads it "
            "before any script runs: on the command line (`-DFETCHCONTENT_SOURCE_DIR_<NAME>=<dir>`) or in the "
            "initial cache.")
    endif()
    string(REPLACE "," ";" _dss_declared "${${inputs_var}};${${scripts_var}}")
    set(_dss_roots "")
    foreach(_dss_rel IN LISTS _dss_declared)
        set(_dss_abs "${CMAKE_SOURCE_DIR}/${_dss_rel}")
        cmake_path(NORMAL_PATH _dss_abs)
        list(APPEND _dss_roots "${_dss_abs}")
    endforeach()
    # The directories of the dependencies this configure was handed.
    string(REPLACE "," ";" _dss_pairs "${_dss_pairs}")
    set(_dss_handed "")
    foreach(_dss_pair IN LISTS _dss_pairs)
        string(REGEX REPLACE "^[^=]*=" "" _dss_dependency "${_dss_pair}")
        list(APPEND _dss_handed "${_dss_dependency}")
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
            set(_dss_is_handed FALSE)
            foreach(_dss_dependency IN LISTS _dss_handed)
                cmake_path(IS_PREFIX _dss_dependency "${_dss_sub}" NORMALIZE _dss_under_dependency)
                if(_dss_under_dependency)
                    set(_dss_is_handed TRUE)
                    break()
                endif()
            endforeach()
            if(_dss_in_build OR NOT _dss_in_source OR _dss_is_handed)
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
