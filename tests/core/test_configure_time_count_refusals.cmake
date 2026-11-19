# ═══ A COUNT THE CONFIG TREE STATES FOR THE BUILD IS REFUSED BY NAME WHEN IT IS NOT A POSITIVE WHOLE NUMBER ═══
# (cycle P69 f26d, lane `cs`) — the ctest entry `core/diagnostic_quote_limit_configure_refusals`, and any other count
# read through cmake/DssConfigureTimeCount.cmake.
#
# A configure without a usable value must STOP, saying why: there is no default. Each case plants one document under
# a scratch directory and runs the reader alone, in script mode (configure_time_count_reader.cmake), reading its exit
# and its sentence: a missing document, bytes that are not JSON, a top level that is not an object, no <key>, and a
# <key> that is zero, negative, fractional or a string are each refused; a positive whole number and the SHIPPED
# document are read (the CONTROLS: a reader that refused everything would fail them).
#
#   cmake -D DSS_REPO_ROOT=<repo> -D DSS_SCRATCH=<dir> -D KEY=<key> -D SUBJECT=<subject> -D UNIT=<unit>
#         -D SHIPPED=<the shipped document, relative to the repo> -P test_configure_time_count_refusals.cmake

foreach(_required IN ITEMS DSS_REPO_ROOT DSS_SCRATCH KEY SUBJECT UNIT SHIPPED)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "configure-time count refusals: -D ${_required} was not supplied.")
    endif()
endforeach()

set(_reader "${CMAKE_CURRENT_LIST_DIR}/configure_time_count_reader.cmake")
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
        COMMAND "${CMAKE_COMMAND}" -D "DSS_REPO_ROOT=${DSS_REPO_ROOT}" -D "DOC=${_doc}" -D "KEY=${KEY}"
                -D "SUBJECT=${SUBJECT}" -D "UNIT=${UNIT}" -P "${_reader}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    # CMake wraps a message to its console width and indents each continuation, so a sentence may break at any of
    # its spaces (MEASURED: under a long scratch path every fragment was split): each run of white space is read
    # as one space before a fragment is looked for.
    string(REGEX REPLACE "[ \t\r\n]+" " " _said "${_out} ${_err}")
    set(_ok FALSE)
    if(_wantFail AND NOT _rc EQUAL 0)
        set(_ok TRUE)
    elseif(NOT _wantFail AND _rc EQUAL 0)
        set(_ok TRUE)
    endif()
    string(FIND "${_said}" "${_fragment}" _at)
    if(NOT _ok OR _at EQUAL -1)
        set(_failures "${_failures}\n  ${_name}: exit ${_rc}, wanted it to say '${_fragment}'; it said: ${_said}"
            PARENT_SCOPE)
    endif()
endfunction()

_case(missing    "MISSING"                1 "no such document")
_case(not_json   "{\"${KEY}\": 64"        1 "not JSON")
_case(top_array  "[64]"                   1 "its top level is ARRAY, not an object")
_case(no_key     "{\"other\": 1}"         1 "declares no `${KEY}`")
_case(zero       "{\"${KEY}\": 0}"        1 "must be a positive whole number of ${UNIT}")
_case(negative   "{\"${KEY}\": -3}"       1 "must be a positive whole number of ${UNIT}")
_case(fractional "{\"${KEY}\": 2.5}"      1 "must be a positive whole number of ${UNIT}")
_case(a_string   "{\"${KEY}\": \"64\"}"   1 "must be a positive whole number of ${UNIT}")
_case(seven      "{\"${KEY}\": 7}"        0 "${KEY}=7")
# Every refusal names its subject first, so the operator knows WHICH count stopped the configure.
_case(named      "{\"${KEY}\": 0}"        1 "${SUBJECT} (")

# The shipped document itself is read, and says what the forward compiles in.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -D "DSS_REPO_ROOT=${DSS_REPO_ROOT}" -D "DOC=${DSS_REPO_ROOT}/${SHIPPED}" -D "KEY=${KEY}"
            -D "SUBJECT=${SUBJECT}" -D "UNIT=${UNIT}" -P "${_reader}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "${KEY}=[1-9][0-9]*")
    set(_failures "${_failures}\n  the shipped document: exit ${_rc}; it said: ${_out} ${_err}")
endif()

if(_failures)
    message(FATAL_ERROR "the configure-time read of `${KEY}` did not refuse as it must:${_failures}")
endif()
message(STATUS "configure-time count refusals (${KEY}): 9 refused by name, 2 read")
