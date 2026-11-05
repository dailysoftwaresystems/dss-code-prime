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
#   directories -- and proves what it chooses: on a clean configure (and that the search there was told to
#   ask the PATH before the registry and the frameworks), on a floor no interpreter meets, and, on a Windows
#   host, on a cache that already names an App Execution Alias (the cache a build configured before this rule
#   holds), on an alias named by hand, on one named in another case, and on a PATH entry inside the alias
#   directory that follows an entry ending in a separator. Each proof is an ARM and prints one verdict line;
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

# Whether `path` names the App Execution Alias directory `aliases` or lies inside it -> `out_var`, compared AS THE
# OS COMPARES: through the long name of the directory that holds it (`file(REAL_PATH)` asks the OS, which hands
# back the long name for an 8.3 short one and resolves a link) and, on Windows, without regard to case -- a cache
# or a PATH entry spelling `WindowsApps` in another case or in its short form names the same directory.
function(_dss_python_in_aliases aliases path out_var)
    set(_in FALSE)
    if(aliases AND NOT "${path}" STREQUAL "")
        file(TO_CMAKE_PATH "${path}" _p)
        if(IS_DIRECTORY "${_p}")
            set(_dir "${_p}")
            set(_leaf "")
        else()
            cmake_path(GET _p PARENT_PATH _dir)
            cmake_path(GET _p FILENAME _leaf)
        endif()
        if(IS_DIRECTORY "${_dir}")
            file(REAL_PATH "${_dir}" _dir)
        endif()
        set(_a "${aliases}")
        if(IS_DIRECTORY "${_a}")
            file(REAL_PATH "${_a}" _a)
        endif()
        set(_whole "${_dir}")
        if(NOT _leaf STREQUAL "")
            set(_whole "${_dir}/${_leaf}")
        endif()
        if(CMAKE_HOST_WIN32)
            string(TOLOWER "${_whole}" _whole)
            string(TOLOWER "${_a}" _a)
        endif()
        cmake_path(IS_PREFIX _a "${_whole}" NORMALIZE _in)
    endif()
    set(${out_var} ${_in} PARENT_SCOPE)
endfunction()

# Drop FindPython3's cached choice of interpreter, WHOLE: the interpreter, its debug sibling, the signature its
# validity is checked by, and the PROPERTIES FindPython3 reports of it (its version parts, stdlib and
# site-packages). FindPython3 drops these four together when it drops a cached interpreter itself, and it reports
# the interpreter it finds next with whatever properties are still cached -- the dropped one's (P69 H5).
function(_dss_python_forget_the_cached_choice)
    foreach(_dss_python_cached IN ITEMS _Python3_EXECUTABLE _Python3_EXECUTABLE_DEBUG
                                        _Python3_INTERPRETER_SIGNATURE _Python3_INTERPRETER_PROPERTIES)
        unset(${_dss_python_cached} CACHE)
    endforeach()
endfunction()

# THE LEG'S PATH FIRST (above), stated ONCE: the CMake code that sets the search parameters FindPython3 is asked
# with. The module's search runs it, and the self-test's configure before the rule -- a project that does not
# include this module (case 3) -- is written with it: FindPython3 keeps a cached choice only under the signature
# it was made with, and these parameters are part of that signature (cmake 4.3.2 FindPython/Support.cmake), so
# the two configures are asked alike by construction, never by a second copy of the parameters (P69 H5).
set(_dss_python_search_order "set(Python3_FIND_REGISTRY LAST)\nset(Python3_FIND_FRAMEWORK LAST)\n")

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
    # The ratchet: three arms on every host, four more where App Execution Aliases exist.
    set(_DSS_PY_SELFTEST_ARMS 3)
    if(_dss_py_t_aliases)
        set(_DSS_PY_SELFTEST_ARMS 7)
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
             "file(WRITE \"\${CMAKE_BINARY_DIR}/chosen.txt\" \"\${Python3_EXECUTABLE}\\n\${Python3_VERSION}\\n\")\n"
             "file(WRITE \"\${CMAKE_BINARY_DIR}/order.txt\" \"\${Python3_FIND_REGISTRY}\\n\${Python3_FIND_FRAMEWORK}\\n\")\n")
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
        _dss_python_in_aliases("${_dss_py_t_aliases}" "${path}" _in)
        set(${out_var} ${_in} PARENT_SCOPE)
    endfunction()

    # (1) A clean configure: an interpreter at or above the floor the tree's config.json states.
    _dss_py_t_configure(clean "${_dss_py_t_config}" _rc _o _chosen)
    list(LENGTH _chosen _n)
    set(_ok FALSE)
    set(_dss_py_t_clean_exe "")
    if(_rc EQUAL 0 AND _n EQUAL 2)
        list(GET _chosen 0 _exe)
        list(GET _chosen 1 _ver)
        _dss_py_t_in_aliases("${_exe}" _in)
        if(_ver VERSION_GREATER_EQUAL _dss_py_t_floor AND NOT _in)
            set(_ok TRUE)
            set(_dss_py_t_clean_exe "${_exe}")
        endif()
    endif()
    _dss_py_t_arm("configure: a clean configure chooses an interpreter at or above the floor config.json states"
                  ${_ok} "rc ${_rc}, chose [${_chosen}] under the floor ${_dss_py_t_floor}: ${_o}")

    # (1b) THE LEG'S PATH IS ASKED FIRST: the search is told to ask the Windows registry and the macOS frameworks
    #      only after the PATH -- FindPython3 asks both FIRST by default, and ✔MEASURED 2026-10-16 a registry entry
    #      answered ahead of the interpreter the host's PATH names. What the module told the search is read back
    #      from the clean configure above, on every host (a registry this test may not write is no fixture).
    set(_order "")
    if(EXISTS "${DSS_PYTHON_SCRATCH}/clean/build/order.txt")
        file(READ "${DSS_PYTHON_SCRATCH}/clean/build/order.txt" _order)
        string(STRIP "${_order}" _order)
        string(REPLACE "\n" ";" _order "${_order}")
    endif()
    set(_ok FALSE)
    if(_order STREQUAL "LAST;LAST")
        set(_ok TRUE)
    endif()
    _dss_py_t_arm("configure: the search asks the leg's PATH before the Windows registry and the macOS frameworks"
                  ${_ok} "the clean configure told FindPython3 registry;framework [${_order}], not [LAST;LAST]")

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
        # THE FIXTURE'S OWN ALIAS DIRECTORY, holding an interpreter that STARTS. Whether the host's alias starts
        # one is the host's business (a Store stub, an install manager, a removed runtime), and a case that asks
        # the module's rule must not hang on it: the fixture points `LOCALAPPDATA` into the scratch and makes a
        # virtual environment there, with the interpreter the clean configure chose. Its PATH names that
        # environment's interpreter first and leaves the host's alias directory out -- the fixture asks about its
        # own alias directory alone. Cases (3) and (6) configure under it.
        set(_dss_py_t_lad "${DSS_PYTHON_SCRATCH}/alias-fixture/localappdata")
        set(_dss_py_t_fixture_aliases "${_dss_py_t_lad}/Microsoft/WindowsApps")
        set(_dss_py_t_fixture_exe "${_dss_py_t_fixture_aliases}/Scripts/python.exe")
        set(_dss_py_t_fixture_why "")
        if(_dss_py_t_clean_exe STREQUAL "")
            set(_dss_py_t_fixture_why "the clean configure chose no interpreter to make the fixture's alias directory with")
        else()
            execute_process(COMMAND "${_dss_py_t_clean_exe}" -m venv --without-pip "${_dss_py_t_fixture_aliases}"
                            RESULT_VARIABLE _venv_rc OUTPUT_VARIABLE _venv_out ERROR_VARIABLE _venv_err)
            if(NOT _venv_rc EQUAL 0 OR NOT EXISTS "${_dss_py_t_fixture_exe}")
                set(_dss_py_t_fixture_why "the fixture's alias directory was not made: `${_dss_py_t_clean_exe} -m venv` "
                                          "answered ${_venv_rc}: ${_venv_out}${_venv_err}")
            endif()
        endif()
        # The leg's PATH, read as the SEARCH PATH it is (an entry ending in a separator does not swallow the next),
        # without the host's alias directory.
        file(TO_CMAKE_PATH "$ENV{PATH}" _dss_py_t_leg_path)
        set(_dss_py_t_leg_rest "")
        foreach(_entry IN LISTS _dss_py_t_leg_path)
            if(NOT _entry STREQUAL "")
                _dss_py_t_in_aliases("${_entry}" _in)
                if(NOT _in)
                    file(TO_NATIVE_PATH "${_entry}" _entry)
                    list(APPEND _dss_py_t_leg_rest "${_entry}")
                endif()
            endif()
        endforeach()
        file(TO_NATIVE_PATH "${_dss_py_t_lad}" _dss_py_t_lad_native)
        file(TO_NATIVE_PATH "${_dss_py_t_fixture_aliases}/Scripts" _dss_py_t_fixture_scripts)

        # Configures the fixture project `case` under the fixture's alias directory and the PATH `path` (a list),
        # twice when `before` is TRUE: first a project WITHOUT this module -- the search parameters this module
        # sets (`_dss_python_search_order`, their one statement), nothing of its rule -- whose configure
        # FindPython3 caches its choice in, then the same build with
        # this module. -> the exit code, the text, the choice (as `_dss_py_t_configure` gives them), and what the
        # configure before the rule chose.
        function(_dss_py_t_configure_under_fixture case path before out_rc out_text out_chosen out_before)
            set(_root "${DSS_PYTHON_SCRATCH}/${case}")
            file(REMOVE_RECURSE "${_root}")
            set(_gen -G "${DSS_PYTHON_GENERATOR}")
            if(DSS_PYTHON_MAKE_PROGRAM)
                list(APPEND _gen "-DCMAKE_MAKE_PROGRAM=${DSS_PYTHON_MAKE_PROGRAM}")
            endif()
            set(_chosen_before "")
            if(before)
                file(WRITE "${_root}/tree/CMakeLists.txt"
                     "cmake_minimum_required(VERSION 4.0)\nproject(pythonfixture NONE)\n"
                     "${_dss_python_search_order}"
                     "find_package(Python3 COMPONENTS Interpreter REQUIRED)\n"
                     "file(WRITE \"\${CMAKE_BINARY_DIR}/before.txt\" \"\${Python3_EXECUTABLE}\\n\")\n")
                execute_process(
                    COMMAND "${CMAKE_COMMAND}" -E env "LOCALAPPDATA=${_dss_py_t_lad_native}" "PATH=${path}"
                            "${CMAKE_COMMAND}" -S "${_root}/tree" -B "${_root}/build" ${_gen}
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
                if(_rc EQUAL 0 AND EXISTS "${_root}/build/before.txt")
                    file(STRINGS "${_root}/build/before.txt" _chosen_before)
                endif()
            endif()
            file(WRITE "${_root}/tree/.harness-config/config.json" "${_dss_py_t_config}")
            file(WRITE "${_root}/tree/CMakeLists.txt"
                 "cmake_minimum_required(VERSION 4.0)\nproject(pythonfixture NONE)\n"
                 "include(\"${_dss_py_t_module}\")\n"
                 "file(WRITE \"\${CMAKE_BINARY_DIR}/chosen.txt\" \"\${Python3_EXECUTABLE}\\n\${Python3_VERSION}\\n\${Python3_SITELIB}\\n\")\n")
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E env "LOCALAPPDATA=${_dss_py_t_lad_native}" "PATH=${path}"
                        "${CMAKE_COMMAND}" -S "${_root}/tree" -B "${_root}/build" ${_gen}
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
            string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
            set(_chosen "")
            if(_rc EQUAL 0 AND EXISTS "${_root}/build/chosen.txt")
                file(STRINGS "${_root}/build/chosen.txt" _chosen)
            endif()
            set(${out_rc} "${_rc}" PARENT_SCOPE)
            set(${out_text} "${_text}" PARENT_SCOPE)
            set(${out_chosen} "${_chosen}" PARENT_SCOPE)
            set(${out_before} "${_chosen_before}" PARENT_SCOPE)
        endfunction()

        # Whether the configure chose an interpreter outside BOTH alias directories, the fixture's and the host's,
        # and FindPython3 reports it as itself: its site-packages lie outside them too (an interpreter reported
        # with a dropped choice's cached properties carries the dropped one's).
        function(_dss_py_t_chose_outside rc chosen out_var)
            set(_ok FALSE)
            list(LENGTH chosen _n)
            if(rc EQUAL 0 AND _n EQUAL 3)
                list(GET chosen 0 _exe)
                list(GET chosen 2 _site)
                _dss_python_in_aliases("${_dss_py_t_fixture_aliases}" "${_exe}" _in_fixture)
                _dss_py_t_in_aliases("${_exe}" _in_host)
                _dss_python_in_aliases("${_dss_py_t_fixture_aliases}" "${_site}" _site_in_fixture)
                if(NOT _exe STREQUAL "" AND NOT _site STREQUAL "" AND NOT _in_fixture AND NOT _in_host
                   AND NOT _site_in_fixture)
                    set(_ok TRUE)
                endif()
            endif()
            set(${out_var} ${_ok} PARENT_SCOPE)
        endfunction()

        # (3) A cache that ALREADY names an interpreter inside the alias directory -- what a build configured
        #     before this rule holds -- is searched again, and the interpreter chosen lies outside it. THE CACHE IS
        #     MADE AS SUCH A BUILD MADE IT, BY FINDPYTHON3: a cached `_Python3_EXECUTABLE` that its own
        #     `_Python3_INTERPRETER_SIGNATURE` does not match is discarded by FindPython3 before this module's rule
        #     decides anything (READ, cmake 4.3.2's FindPython/Support.cmake), so a `-D` of the cache alone never
        #     reached the rule: ✔MEASURED 2026-11-03, with the rule's `unset` removed that fixture stayed green
        #     (check-mutations run 20261103-181938-fd251bd3, windows-x86_64-release). The configure before the rule
        #     must have chosen the fixture's alias, or the cache this case asks about was never made.
        set(_ok FALSE)
        set(_detail "${_dss_py_t_fixture_why}")
        if(_dss_py_t_fixture_why STREQUAL "")
            set(_path "${_dss_py_t_fixture_scripts}" ${_dss_py_t_leg_rest})
            _dss_py_t_configure_under_fixture(cached-alias "${_path}" TRUE _rc _o _chosen _before)
            _dss_python_in_aliases("${_dss_py_t_fixture_aliases}" "${_before}" _cached)
            if(NOT _cached)
                set(_detail "the configure before the rule chose [${_before}], not the fixture's alias: no cache to ask about")
            else()
                _dss_py_t_chose_outside("${_rc}" "${_chosen}" _ok)
                set(_detail "rc ${_rc}, chose [${_chosen}] over the cached [${_before}]: ${_o}")
            endif()
        endif()
        _dss_py_t_arm("configure: a cache naming an App Execution Alias is searched again, and the interpreter chosen lies outside the alias directory"
                      ${_ok} "${_detail}")

        # (4) An interpreter named BY HAND inside the alias directory is refused, by name.
        _dss_py_t_configure(named-alias "${_dss_py_t_config}" _rc _o _chosen
                            "-DPython3_EXECUTABLE=${_dss_py_t_aliases}/python3.exe")
        set(_ok FALSE)
        if(NOT _rc EQUAL 0 AND _o MATCHES "Python3_EXECUTABLE names .* an App Execution Alias")
            set(_ok TRUE)
        endif()
        _dss_py_t_arm("configure: an interpreter named by hand inside the App Execution Alias directory is refused by name"
                      ${_ok} "rc ${_rc}: ${_o}")

        # (5) The same, spelled in another CASE: the OS compares names without regard to case, so the module does.
        string(TOUPPER "${_dss_py_t_aliases}" _dss_py_t_upper)
        _dss_py_t_configure(named-alias-case "${_dss_py_t_config}" _rc _o _chosen
                            "-DPython3_EXECUTABLE=${_dss_py_t_upper}/python3.exe")
        set(_ok FALSE)
        if(NOT _rc EQUAL 0 AND _o MATCHES "Python3_EXECUTABLE names .* an App Execution Alias")
            set(_ok TRUE)
        endif()
        _dss_py_t_arm("configure: an interpreter named inside the App Execution Alias directory in another case is refused by name"
                      ${_ok} "rc ${_rc}, chose [${_chosen}]: ${_o}")

        # (6) A PATH entry inside the alias directory is left out of the search when the entry before it ENDS IN A
        #     SEPARATOR. The PATH is a native search path, not a CMake list: read as a list, `C:\a\;C:\b` is ONE
        #     element (`\;` does not divide one, cmake-language "Lists"), so the entry after a `\` was never
        #     compared. ✔MEASURED 2026-11-03, this host's PATH: 14 of its 61 entries end in `\`. The fixture's PATH
        #     is one such entry (the scratch directory, which exists), then the fixture's alias interpreter, then
        #     the leg's PATH; no cache.
        set(_ok FALSE)
        set(_detail "${_dss_py_t_fixture_why}")
        if(_dss_py_t_fixture_why STREQUAL "")
            file(TO_NATIVE_PATH "${DSS_PYTHON_SCRATCH}/" _dss_py_t_ends_in_separator)
            set(_path "${_dss_py_t_ends_in_separator}" "${_dss_py_t_fixture_scripts}" ${_dss_py_t_leg_rest})
            _dss_py_t_configure_under_fixture(after-a-separator "${_path}" FALSE _rc _o _chosen _before)
            _dss_py_t_chose_outside("${_rc}" "${_chosen}" _ok)
            set(_detail "rc ${_rc}, chose [${_chosen}]: ${_o}")
        endif()
        _dss_py_t_arm("configure: a PATH entry inside the App Execution Alias directory is left out of the search after an entry ending in a separator"
                      ${_ok} "${_detail}")
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
    _dss_python_forget_the_cached_choice()
endif()
set(DSS_PYTHON_FLOOR_SEARCHED "${DSS_PYTHON_FLOOR}" CACHE INTERNAL
    "the Python floor (config.json tools python3 minVersion) the interpreter was searched for under")

_dss_python_alias_directory(_dss_python_aliases)
if(_dss_python_aliases)
    foreach(_dss_python_var _Python3_EXECUTABLE Python3_EXECUTABLE)
        if(DEFINED ${_dss_python_var} AND NOT "${${_dss_python_var}}" STREQUAL "")
            file(TO_CMAKE_PATH "${${_dss_python_var}}" _dss_python_named)
            _dss_python_in_aliases("${_dss_python_aliases}" "${_dss_python_named}" _dss_python_in_aliases)
            if(_dss_python_in_aliases AND _dss_python_var STREQUAL "_Python3_EXECUTABLE")
                _dss_python_forget_the_cached_choice()
            elseif(_dss_python_in_aliases)
                message(FATAL_ERROR "DssPython: Python3_EXECUTABLE names ${_dss_python_named}, an App "
                                    "Execution Alias: a reparse point the process loader resolves and a spawner "
                                    "that asks the file system finds no program at. Name the interpreter itself.")
            endif()
        endif()
    endforeach()
    # The search leaves the alias directory out under EVERY spelling the leg's PATH gives it: CMAKE_IGNORE_PATH
    # compares spellings, and a PATH entry naming the directory in another case or its short form would pass.
    # The PATH is read as the native SEARCH PATH it is: read as a CMake list, an entry ending in `\` escapes the
    # `;` after it and swallows the next entry, which was then never compared (P69 H5).
    set(_dss_python_ignored "${_dss_python_aliases}")
    file(TO_CMAKE_PATH "$ENV{PATH}" _dss_python_path)
    foreach(_dss_python_entry IN LISTS _dss_python_path)
        if(NOT _dss_python_entry STREQUAL "")
            _dss_python_in_aliases("${_dss_python_aliases}" "${_dss_python_entry}" _dss_python_in_aliases)
            if(_dss_python_in_aliases)
                file(TO_CMAKE_PATH "${_dss_python_entry}" _dss_python_entry)
                list(APPEND _dss_python_ignored "${_dss_python_entry}")
            endif()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _dss_python_ignored)
    list(APPEND CMAKE_IGNORE_PATH ${_dss_python_ignored})
endif()
cmake_language(EVAL CODE "${_dss_python_search_order}")
find_package(Python3 ${DSS_PYTHON_FLOOR} COMPONENTS Interpreter REQUIRED)
if(_dss_python_aliases)
    list(REMOVE_ITEM CMAKE_IGNORE_PATH ${_dss_python_ignored})
    file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" _dss_python_named)
    _dss_python_in_aliases("${_dss_python_aliases}" "${_dss_python_named}" _dss_python_in_aliases)
    if(_dss_python_in_aliases)
        message(FATAL_ERROR "DssPython: FindPython3 chose ${_dss_python_named}, inside the App Execution Alias "
                            "directory this module left out of its search; no interpreter is used from there")
    endif()
endif()
