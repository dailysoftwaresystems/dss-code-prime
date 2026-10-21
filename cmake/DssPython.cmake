# DssPython.cmake -- THE ONE PYTHON EVERY CTEST ENTRY RUNS: AT OR ABOVE THE REPOSITORY'S FLOOR, AND NEVER
# AN APP EXECUTION ALIAS.
#
# Included wherever a directory needs `${Python3_EXECUTABLE}`, in place of a bare find_package; every
# inclusion asks the same question and gets the same, cached, answer.
#
# ★ THE FLOOR IS STATED ONCE: `.harness-config/config.json`, the `tools` entry named `python3`, its
#   `minVersion` -- the value DssHarness holds every leg to, runner steps included, and installs a host's
#   tools by. This module reads that
#   value (CMake's JSON reader takes the file's comments) and asks FindPython3 for an interpreter at or
#   above it. FindPython3 looks past an older interpreter wherever it looks, so a host whose default
#   `python3` is older still configures when a newer one is installed, and a host with none is REFUSED
#   HERE, AT CONFIGURE, BY NAME -- never a self-test failing later, on one leg, on a construct its
#   interpreter is too old for (✔MEASURED 2026-10-16: the macOS leg ran every self-test under Apple's
#   3.9.6 and `Path.write_text(newline=)`, a 3.10 keyword, failed there alone).
# ★ NEVER AN APP EXECUTION ALIAS. On Windows, `%LOCALAPPDATA%\Microsoft\WindowsApps\python3.exe` is a
#   zero-byte reparse point (tag IO_REPARSE_TAG_APPEXECLINK, ✔MEASURED 2026-10-16) that the system
#   resolves when it starts a process: it can launch a Store installer instead of an interpreter, the
#   interpreter behind it can change under a configured build, and a spawner that asks the file system
#   what the path is -- not the process loader -- finds no program there (a leg's cache naming that
#   alias turned two self-test entries red; ✔MEASURED 2026-10-20 on a build whose cache held
#   `_Python3_EXECUTABLE` = that alias, seven self-test entries red on windows-x86_64-release, every one
#   "was not found as an executable"). That directory is left out of the search, and an interpreter named
#   inside it -- by a cache from before this rule, or by hand -- is not used: a cached one is searched for
#   again, one given by hand is refused by name.
# ★ THE LEG'S PATH FIRST: the interpreter is the first on the PATH a leg's processes see that meets the
#   floor -- the one a runner step starts when that one meets it -- and the Windows registry and the macOS
#   frameworks are asked only after it. FindPython3 asks both FIRST by default, and a registry entry is
#   whatever last registered itself: ✔MEASURED 2026-10-16 on the Windows host, the registry answered with an
#   interpreter inside another session's scratch directory, ahead of the one the host's PATH names.
# ⚠ A config.json that cannot be read, or a python3 entry with no `minVersion` of the form
#   `major.minor.patch` (the one form DssHarness reads a minVersion in), is a FATAL_ERROR naming what is
#   missing: a floor guessed here would be a second statement of it.
#
# ★ THE SELF-TEST (`-DDSS_PYTHON_MODE=selftest`, ctest `build/python_interpreter`). Run with `cmake -P`,
#   this file configures fixture PROJECTS that include it -- with the build's own generator, in throwaway
#   directories -- and proves what it chooses: on a clean configure, on a floor no interpreter meets, and,
#   on a Windows host, on a cache that already names an App Execution Alias (the cache a build configured
#   before this rule holds) and on an alias named by hand. Each proof is an ARM and prints one verdict line;
#   the test binary reads each into a case of its own (tests/program/test_python_interpreter.cpp).

# The floor `config` states (tools -> python3 -> minVersion) -> `out_var`; a FATAL_ERROR naming what is
# missing when there is none to read.
function(_dss_python_read_floor config out_var)
    if(NOT EXISTS "${config}")
        message(FATAL_ERROR "DssPython: ${config} does not exist, so the Python floor it states "
                            "(tools -> python3 -> minVersion) cannot be read")
    endif()
    file(READ "${config}" _dss_python_text)
    string(JSON _dss_python_tools ERROR_VARIABLE _dss_python_error LENGTH "${_dss_python_text}" tools)
    if(_dss_python_error OR NOT _dss_python_tools GREATER 0)
        message(FATAL_ERROR "DssPython: ${config} holds no readable `tools` list "
                            "(${_dss_python_error}), so the Python floor it states cannot be read")
    endif()
    set(_dss_python_floor "")
    math(EXPR _dss_python_last "${_dss_python_tools} - 1")
    foreach(_dss_python_i RANGE ${_dss_python_last})
        string(JSON _dss_python_name ERROR_VARIABLE _dss_python_error GET "${_dss_python_text}" tools
               ${_dss_python_i} name)
        if(_dss_python_name STREQUAL "python3")
            string(JSON _dss_python_floor ERROR_VARIABLE _dss_python_error GET "${_dss_python_text}" tools
                   ${_dss_python_i} minVersion)
            break()
        endif()
    endforeach()
    if(NOT _dss_python_floor MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "DssPython: ${config}'s tools entry `python3` states no minVersion of "
                            "the form major.minor.patch (read: '${_dss_python_floor}'), so there is no Python floor "
                            "to hold every leg to")
    endif()
    set(${out_var} "${_dss_python_floor}" PARENT_SCOPE)
endfunction()

# The directory of the App Execution Aliases on this host -> `out_var`; empty where there is none.
function(_dss_python_alias_directory out_var)
    set(_dss_python_dir "")
    if(CMAKE_HOST_WIN32 AND DEFINED ENV{LOCALAPPDATA})
        file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Microsoft/WindowsApps" _dss_python_dir)
    endif()
    set(${out_var} "${_dss_python_dir}" PARENT_SCOPE)
endfunction()

if(DEFINED DSS_PYTHON_MODE)
    # ═══ THE SELF-TEST ═══
    if(NOT DSS_PYTHON_MODE STREQUAL "selftest")
        message(FATAL_ERROR "DssPython: DSS_PYTHON_MODE is `selftest` or not set, and `${DSS_PYTHON_MODE}` is neither.")
    endif()
    foreach(_dss_required IN ITEMS DSS_PYTHON_SCRATCH DSS_PYTHON_SOURCE_DIR DSS_PYTHON_GENERATOR)
        if(NOT DEFINED ${_dss_required} OR "${${_dss_required}}" STREQUAL "")
            message(FATAL_ERROR "dss-python selftest: ${_dss_required} was not passed -- a named failure, never a skip.")
        endif()
    endforeach()
    set(_dss_py_t_module "${CMAKE_CURRENT_LIST_FILE}")
    _dss_python_read_floor("${DSS_PYTHON_SOURCE_DIR}/.harness-config/config.json" _dss_py_t_floor)
    file(READ "${DSS_PYTHON_SOURCE_DIR}/.harness-config/config.json" _dss_py_t_config)
    _dss_python_alias_directory(_dss_py_t_aliases)
    # The ratchet: two arms on every host, two more where App Execution Aliases exist.
    set(_DSS_PY_SELFTEST_ARMS 2)
    if(_dss_py_t_aliases)
        set(_DSS_PY_SELFTEST_ARMS 4)
    endif()
    set(_dss_py_t_ran 0)
    set(_dss_py_t_failed 0)

    function(_dss_py_t_arm name ok detail)
        math(EXPR _n "${_dss_py_t_ran} + 1")
        set(_dss_py_t_ran ${_n} PARENT_SCOPE)
        if(ok)
            message(STATUS "dss-python selftest: ok      ${name}")
        else()
            math(EXPR _f "${_dss_py_t_failed} + 1")
            set(_dss_py_t_failed ${_f} PARENT_SCOPE)
            message(STATUS "dss-python selftest: FAILED  ${name} -- ${detail}")
        endif()
    endfunction()

    # Configures a fixture project whose `.harness-config/config.json` holds `config_text` and which includes
    # THIS file, handing the configure the arguments after `out_chosen` as they are (a `-D` cache entry) ->
    # its exit code, what it printed (every run of whitespace folded to one space: CMake wraps a message),
    # and the interpreter it chose and that interpreter's version, as a list (empty when it chose none).
    function(_dss_py_t_configure case config_text out_rc out_text out_chosen)
        set(_root "${DSS_PYTHON_SCRATCH}/${case}")
        file(REMOVE_RECURSE "${_root}")
        file(WRITE "${_root}/tree/.harness-config/config.json" "${config_text}")
        file(WRITE "${_root}/tree/CMakeLists.txt"
             "cmake_minimum_required(VERSION 4.0)\nproject(pythonfixture NONE)\n"
             "include(\"${_dss_py_t_module}\")\n"
             "file(WRITE \"\${CMAKE_BINARY_DIR}/chosen.txt\" \"\${Python3_EXECUTABLE}\\n\${Python3_VERSION}\\n\")\n")
        set(_gen -G "${DSS_PYTHON_GENERATOR}")
        if(DSS_PYTHON_MAKE_PROGRAM)
            list(APPEND _gen "-DCMAKE_MAKE_PROGRAM=${DSS_PYTHON_MAKE_PROGRAM}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -S "${_root}/tree" -B "${_root}/build" ${_gen} ${ARGN}
            RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
        string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
        set(_chosen "")
        if(_rc EQUAL 0 AND EXISTS "${_root}/build/chosen.txt")
            file(STRINGS "${_root}/build/chosen.txt" _chosen)
        endif()
        set(${out_rc} "${_rc}" PARENT_SCOPE)
        set(${out_text} "${_text}" PARENT_SCOPE)
        set(${out_chosen} "${_chosen}" PARENT_SCOPE)
    endfunction()

    # Whether `path` lies inside the App Execution Alias directory -> `out_var`.
    function(_dss_py_t_in_aliases path out_var)
        set(_in FALSE)
        if(_dss_py_t_aliases AND NOT "${path}" STREQUAL "")
            file(TO_CMAKE_PATH "${path}" _p)
            cmake_path(IS_PREFIX _dss_py_t_aliases "${_p}" NORMALIZE _in)
        endif()
        set(${out_var} ${_in} PARENT_SCOPE)
    endfunction()

    # (1) A clean configure: an interpreter at or above the floor the tree's config.json states.
    _dss_py_t_configure(clean "${_dss_py_t_config}" _rc _o _chosen)
    list(LENGTH _chosen _n)
    set(_ok FALSE)
    if(_rc EQUAL 0 AND _n EQUAL 2)
        list(GET _chosen 0 _exe)
        list(GET _chosen 1 _ver)
        _dss_py_t_in_aliases("${_exe}" _in)
        if(_ver VERSION_GREATER_EQUAL _dss_py_t_floor AND NOT _in)
            set(_ok TRUE)
        endif()
    endif()
    _dss_py_t_arm("configure: a clean configure chooses an interpreter at or above the floor config.json states"
                  ${_ok} "rc ${_rc}, chose [${_chosen}] under the floor ${_dss_py_t_floor}: ${_o}")

    # (2) A floor no interpreter meets is refused at configure, by the floor it names. A Python 3 floor: one of
    #     another major version is refused by FindPython3 for its major alone, without naming the floor
    #     (✔MEASURED 2026-10-20, cmake 4.3, a floor of 99.0.0: "Wrong major version specified is \"99\"").
    _dss_py_t_configure(unmet "{ \"tools\": [ { \"name\": \"python3\", \"minVersion\": \"3.99.0\" } ] }\n"
                        _rc _o _chosen)
    set(_ok FALSE)
    if(NOT _rc EQUAL 0 AND _o MATCHES "3\\.99\\.0")
        set(_ok TRUE)
    endif()
    _dss_py_t_arm("configure: a floor no interpreter meets is refused at configure, by the floor it names"
                  ${_ok} "rc ${_rc}: ${_o}")

    if(_dss_py_t_aliases)
        # (3) A cache that ALREADY names an App Execution Alias -- what a build configured before this rule
        #     holds -- is searched again, and the interpreter chosen lies outside the alias directory.
        _dss_py_t_configure(cached-alias "${_dss_py_t_config}" _rc _o _chosen
                            "-D_Python3_EXECUTABLE:INTERNAL=${_dss_py_t_aliases}/python3.exe")
        list(LENGTH _chosen _n)
        set(_ok FALSE)
        set(_exe "")
        if(_rc EQUAL 0 AND _n EQUAL 2)
            list(GET _chosen 0 _exe)
            _dss_py_t_in_aliases("${_exe}" _in)
            if(NOT _in AND NOT _exe STREQUAL "")
                set(_ok TRUE)
            endif()
        endif()
        _dss_py_t_arm("configure: a cache naming an App Execution Alias is searched again, and the interpreter chosen lies outside the alias directory"
                      ${_ok} "rc ${_rc}, chose [${_chosen}]: ${_o}")

        # (4) An interpreter named BY HAND inside the alias directory is refused, by name.
        _dss_py_t_configure(named-alias "${_dss_py_t_config}" _rc _o _chosen
                            "-DPython3_EXECUTABLE=${_dss_py_t_aliases}/python3.exe")
        set(_ok FALSE)
        if(NOT _rc EQUAL 0 AND _o MATCHES "Python3_EXECUTABLE names .* an App Execution Alias")
            set(_ok TRUE)
        endif()
        _dss_py_t_arm("configure: an interpreter named by hand inside the App Execution Alias directory is refused by name"
                      ${_ok} "rc ${_rc}: ${_o}")
    endif()

    file(REMOVE_RECURSE "${DSS_PYTHON_SCRATCH}")
    if(NOT _dss_py_t_ran EQUAL _DSS_PY_SELFTEST_ARMS)
        message(FATAL_ERROR "dss-python selftest: ARM COUNT ${_dss_py_t_ran}, expected ${_DSS_PY_SELFTEST_ARMS} -- "
                            "_DSS_PY_SELFTEST_ARMS is the ratchet")
    endif()
    if(_dss_py_t_failed GREATER 0)
        message(FATAL_ERROR "dss-python selftest: ${_dss_py_t_ran} arm(s), ${_dss_py_t_failed} failed")
    endif()
    message(STATUS "dss-python selftest: ${_dss_py_t_ran} arm(s), 0 failed")
    return()
endif()

set(_dss_python_config "${CMAKE_SOURCE_DIR}/.harness-config/config.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dss_python_config}")
_dss_python_read_floor("${_dss_python_config}" DSS_PYTHON_FLOOR)

# ⓘ A FLOOR THAT CHANGED ASKS THE SEARCH AGAIN: FindPython3 keeps the interpreter a cache chose under
#   the old floor whenever it also meets the new one, and the interpreter is to be the first that meets
#   the floor stated now, on every configure alike.
if(DEFINED CACHE{DSS_PYTHON_FLOOR_SEARCHED} AND NOT DSS_PYTHON_FLOOR_SEARCHED STREQUAL DSS_PYTHON_FLOOR)
    unset(_Python3_EXECUTABLE CACHE)
endif()
set(DSS_PYTHON_FLOOR_SEARCHED "${DSS_PYTHON_FLOOR}" CACHE INTERNAL
    "the Python floor (config.json tools python3 minVersion) the interpreter was searched for under")

_dss_python_alias_directory(_dss_python_aliases)
if(_dss_python_aliases)
    foreach(_dss_python_var _Python3_EXECUTABLE Python3_EXECUTABLE)
        if(DEFINED ${_dss_python_var} AND NOT "${${_dss_python_var}}" STREQUAL "")
            file(TO_CMAKE_PATH "${${_dss_python_var}}" _dss_python_named)
            cmake_path(IS_PREFIX _dss_python_aliases "${_dss_python_named}" NORMALIZE _dss_python_in_aliases)
            if(_dss_python_in_aliases AND _dss_python_var STREQUAL "_Python3_EXECUTABLE")
                unset(_Python3_EXECUTABLE CACHE)
            elseif(_dss_python_in_aliases)
                message(FATAL_ERROR "DssPython: Python3_EXECUTABLE names ${_dss_python_named}, an App "
                                    "Execution Alias: a reparse point the process loader resolves and a spawner "
                                    "that asks the file system finds no program at. Name the interpreter itself.")
            endif()
        endif()
    endforeach()
    list(APPEND CMAKE_IGNORE_PATH "${_dss_python_aliases}")
endif()
set(Python3_FIND_REGISTRY LAST)
set(Python3_FIND_FRAMEWORK LAST)
find_package(Python3 ${DSS_PYTHON_FLOOR} COMPONENTS Interpreter REQUIRED)
if(_dss_python_aliases)
    list(REMOVE_ITEM CMAKE_IGNORE_PATH "${_dss_python_aliases}")
    file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" _dss_python_named)
    cmake_path(IS_PREFIX _dss_python_aliases "${_dss_python_named}" NORMALIZE _dss_python_in_aliases)
    if(_dss_python_in_aliases)
        message(FATAL_ERROR "DssPython: FindPython3 chose ${_dss_python_named}, inside the App Execution Alias "
                            "directory this module left out of its search; no interpreter is used from there")
    endif()
endif()
