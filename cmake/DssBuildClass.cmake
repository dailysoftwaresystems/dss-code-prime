# ══ cmake/DssBuildClass.cmake — the ONE owner of "what kind of build is this" ══
#
# Born of an integrated runner that hung before creating its own directory,
# with no entry timeout in place to catch it.
#
# Two consumers ask this question and must never get two answers:
#   * tests/CMakeLists.txt — the shuffle arms trim their repeat count only on an
#     INSTRUMENTED build (`DSS_SHUFFLE_SANITIZED`);
#   * cmake/DssTestBudgets.cmake — every ctest entry's TIMEOUT follows the class.
# Until 2026-09-15 each carried its OWN detection: the shuffle arm tested
# CMAKE_CXX_FLAGS and its build-type variant, the budgets tested the C, CXX and
# linker flags plus the top directory's options. Two definitions of one fact, which
# agreed only because of HOW CI happens to pass its flags. Now there is one, and
# both consumers READ it.
#
# ── THE CLASS ────────────────────────────────────────────────────────────────
#   sanitized  a `-fsanitize=` or `/fsanitize=` reaches the COMPILER: the C or CXX
#              flags — with the build type's own variant on a single-config
#              generator, and every configuration's variant on a multi-config one —
#              or the top directory's COMPILE_OPTIONS
#   release    otherwise, a single-config build of type Release, RelWithDebInfo or
#              MinSizeRel (compared case-insensitively)
#   debug      otherwise: Debug, an empty or custom type (assumed unoptimized), and
#              every multi-config generator, whose configuration is chosen only later
# ⚠ COMPILE-SIDE ON PURPOSE. A sanitizer runtime that is only LINKED instruments
# nothing: the code runs at its uninstrumented speed and catches nothing, and
# "instrumented" is the property both consumers are about.
#
# ── HOW CI'S SANITIZED LEG ARRIVES HERE ──────────────────────────────────────
# 📄 `.github/workflows/pipeline-pr.yml`: `Compose sanitizer flags` builds
# `flags="-fsanitize=<matrix.sanitizers> -fno-omit-frame-pointer -g"`, and
# `Configure` exports it as CFLAGS, CXXFLAGS and LDFLAGS before running
# `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDSS_BUILD_TESTS=ON` with
# the two ccache launchers. On a FIRST configure CMake initialises CMAKE_CXX_FLAGS
# from CXXFLAGS, CMAKE_C_FLAGS from CFLAGS — C is enabled, though this project asks
# for CXX only, because the vendored googletest's `project()` names no languages —
# and the exe, shared and module linker flags from LDFLAGS. ✔MEASURED 2026-09-15 by
# running exactly that configure line (clang-19, ccache, Ninja, Debug) into a fresh
# WSL build directory: all five cache entries hold the flags, the shuffle arm reports
# sanitized=ON, the budgets apply class `sanitized`, and `ctest/entry-budgets` passes
# on that tree. The pin also re-reads that workflow on every run and probes each
# leg's configuration through `dss_build_class_of`.
#
# ── ONE ANSWER PER CONFIGURE ─────────────────────────────────────────────────
# The first `dss_build_class()` computes the class and REMEMBERS it. Every later
# call recomputes it from the configuration as it stands THEN, and stops the
# configure if the answer moved: a sanitizer option added after the first reader
# would otherwise leave the shuffle arms configured for one kind of build and the
# budgets for another. Every reader is recorded, so the pin can prove both asked.
#
# ⚠ THIS IS THE ONLY FILE THAT MAY TEST FOR A SANITIZER. The pin refuses the text
# `fsanitize` in the code (not the comments) of every other CMake file this build
# reads.

include_guard(GLOBAL)

# The class of a build of type `build_type` whose compiler receives `compile_flags`.
function(dss_build_class_of out_var build_type multi_config compile_flags)
    if(compile_flags MATCHES "[-/]fsanitize=")
        set(_class sanitized)
    else()
        string(TOLOWER "${build_type}" _type)
        if(NOT multi_config AND _type MATCHES "^(release|relwithdebinfo|minsizerel)$")
            set(_class release)
        else()
            set(_class debug)
        endif()
    endif()
    set(${out_var} "${_class}" PARENT_SCOPE)
endfunction()

# THIS build's class, as the configuration stands now. READER names the caller.
function(dss_build_class out_var)
    cmake_parse_arguments(_dbc "" "READER" "" ${ARGN})
    if(NOT _dbc_READER)
        message(FATAL_ERROR "dss_build_class(): READER <who is asking> is required, so the pin can prove every consumer asked")
    endif()

    get_property(_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_multi)
        set(_multi 1)
        set(_types ${CMAKE_CONFIGURATION_TYPES})
    else()
        set(_multi 0)
        set(_types ${CMAKE_BUILD_TYPE})
    endif()
    set(_flags "")
    foreach(_lang IN ITEMS C CXX)
        string(APPEND _flags " ${CMAKE_${_lang}_FLAGS}")
        foreach(_type IN LISTS _types)
            string(TOUPPER "${_type}" _upper)
            string(APPEND _flags " ${CMAKE_${_lang}_FLAGS_${_upper}}")
        endforeach()
    endforeach()
    string(STRIP "${_flags}" _flags)
    get_property(_dir_options DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY COMPILE_OPTIONS)
    string(REPLACE ";" " " _dir_options "${_dir_options}")
    dss_build_class_of(_class "${CMAKE_BUILD_TYPE}" "${_multi}" "${_flags} ${_dir_options}")

    get_property(_known GLOBAL PROPERTY DSS_BUILD_CLASS SET)
    if(_known)
        get_property(_first GLOBAL PROPERTY DSS_BUILD_CLASS)
        if(NOT _first STREQUAL _class)
            get_property(_first_reader GLOBAL PROPERTY DSS_BUILD_CLASS_FIRST_READER)
            get_property(_first_flags GLOBAL PROPERTY DSS_BUILD_CLASS_COMPILE_FLAGS)
            get_property(_first_options GLOBAL PROPERTY DSS_BUILD_CLASS_DIRECTORY_OPTIONS)
            message(FATAL_ERROR
                "cmake/DssBuildClass.cmake: this build's class MOVED during the configure — "
                "'${_first}' when ${_first_reader} asked (compile flags: '${_first_flags}'; top "
                "directory options: '${_first_options}'), '${_class}' now that ${_dbc_READER} asks "
                "(compile flags: '${_flags}'; top directory options: '${_dir_options}'). Something "
                "changed the compile configuration between the two readers, so they would configure "
                "for two different kinds of build. Set it before the first reader instead.")
        endif()
    else()
        set_property(GLOBAL PROPERTY DSS_BUILD_CLASS "${_class}")
        set_property(GLOBAL PROPERTY DSS_BUILD_CLASS_FIRST_READER "${_dbc_READER}")
        set_property(GLOBAL PROPERTY DSS_BUILD_CLASS_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
        set_property(GLOBAL PROPERTY DSS_BUILD_CLASS_MULTI_CONFIG "${_multi}")
        set_property(GLOBAL PROPERTY DSS_BUILD_CLASS_COMPILE_FLAGS "${_flags}")
        set_property(GLOBAL PROPERTY DSS_BUILD_CLASS_DIRECTORY_OPTIONS "${_dir_options}")
    endif()
    set_property(GLOBAL APPEND PROPERTY DSS_BUILD_CLASS_READERS "${_dbc_READER}=${_class}")
    set(${out_var} "${_class}" PARENT_SCOPE)
endfunction()
