# ═══ THE CONFIGURE-TIME READ OF THE CONFIG DOCUMENTS' NESTING LIMIT REFUSES BY NAME ═══
# (cycle P69 L2, lane `lm`) — the ctest entry `core/config_nesting_limit_configure_refusals`.
#
# cmake/DssConfigNestingLimit.cmake is what makes the limit config data with no default: a
# configure without a usable `maxNesting` must STOP, saying why. Each case plants one document
# under a scratch directory and runs the reader alone, in script mode
# (config_nesting_limit_reader.cmake), reading its exit and its sentence: a missing document,
# bytes that are not JSON, a top level that is not an object, no `maxNesting`, and a
# `maxNesting` that is zero, negative, fractional or a string are each refused; a positive
# whole number and the shipped document are read (the CONTROLS: a reader that refused
# everything would fail them).
#
#   cmake -D DSS_REPO_ROOT=<repo> -D DSS_SCRATCH=<dir> -P test_config_nesting_limit_refusals.cmake

foreach(_required IN ITEMS DSS_REPO_ROOT DSS_SCRATCH)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "config nesting limit refusals: -D ${_required} was not supplied.")
    endif()
endforeach()

set(_reader "${CMAKE_CURRENT_LIST_DIR}/config_nesting_limit_reader.cmake")
file(REMOVE_RECURSE "${DSS_SCRATCH}")
file(MAKE_DIRECTORY "${DSS_SCRATCH}")
set(_failures "")

# _case(<name> <document text, or MISSING> <expected exit: 0 or 1> <a sentence fragment the output must hold>)
function(_case _name _text _wantFail _fragment)
    set(_doc "${DSS_SCRATCH}/${_name}.json")
    if(NOT _text STREQUAL "MISSING")
        file(WRITE "${_doc}" "${_text}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -D "DSS_REPO_ROOT=${DSS_REPO_ROOT}" -D "DOC=${_doc}" -P "${_reader}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    string(REPLACE "\n" " " _said "${_out} ${_err}")
    if(_wantFail)
        set(_ok FALSE)
        if(NOT _rc EQUAL 0)
            set(_ok TRUE)
        endif()
    else()
        set(_ok FALSE)
        if(_rc EQUAL 0)
            set(_ok TRUE)
        endif()
    endif()
    string(FIND "${_said}" "${_fragment}" _at)
    if(NOT _ok OR _at EQUAL -1)
        set(_failures "${_failures}\n  ${_name}: exit ${_rc}, wanted it to say '${_fragment}'; it said: ${_said}"
            PARENT_SCOPE)
    endif()
endfunction()

_case(missing        "MISSING"                    1 "no such document")
_case(not_json       "{\"maxNesting\": 64"        1 "not JSON")
_case(top_is_array   "[64]"                       1 "its top level is ARRAY, not an object")
_case(no_max_nesting "{\"other\": 1}"             1 "declares no `maxNesting`")
_case(zero           "{\"maxNesting\": 0}"        1 "must be a positive whole number")
_case(negative       "{\"maxNesting\": -3}"       1 "must be a positive whole number")
_case(fractional     "{\"maxNesting\": 2.5}"      1 "must be a positive whole number")
_case(a_string       "{\"maxNesting\": \"64\"}"   1 "must be a positive whole number")
_case(seven          "{\"maxNesting\": 7}"        0 "maxNesting=7")

# The shipped document itself is read, and says what the forward compiles in.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -D "DSS_REPO_ROOT=${DSS_REPO_ROOT}"
            -D "DOC=${DSS_REPO_ROOT}/src/dss-config/configure-time/config-documents.json" -P "${_reader}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "maxNesting=[1-9][0-9]*")
    set(_failures "${_failures}\n  the shipped document: exit ${_rc}; it said: ${_out} ${_err}")
endif()

if(_failures)
    message(FATAL_ERROR "the configure-time read of the nesting limit did not refuse as it must:${_failures}")
endif()
message(STATUS "config nesting limit refusals: 8 refused by name, 2 read")
