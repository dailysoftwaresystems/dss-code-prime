# The configure-time reader of one count of the config tree (cmake/DssConfigureTimeCount.cmake), run alone in script
# mode (cycle P69 f26d, lane `cs`):
#   cmake -D DSS_REPO_ROOT=<repo> -D DOC=<document> -D KEY=<key> -D SUBJECT=<subject> -D UNIT=<unit> -P <this file>
# Prints `<key>=<value>` or stops with the reader's own refusal. Driven by test_configure_time_count_refusals.cmake,
# once per planted document.
foreach(_required IN ITEMS DSS_REPO_ROOT DOC KEY SUBJECT UNIT)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "configure-time count reader: -D ${_required} was not supplied.")
    endif()
endforeach()
include("${DSS_REPO_ROOT}/cmake/DssConfigureTimeCount.cmake")
dss_read_configure_time_count("${DOC}" "${KEY}" "${SUBJECT}"
    "The build cannot be configured without its `${KEY}`." "${UNIT}" _value)
message(STATUS "${KEY}=${_value}")
