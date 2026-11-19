# The configure-time reader of the config documents' nesting limit, run alone in script mode
# (cycle P69 L2, lane `lm`): `cmake -D DSS_REPO_ROOT=<repo> -D DOC=<document> -P <this file>`.
# Prints `maxNesting=<value>` or stops with the reader's own refusal. Driven by
# test_config_nesting_limit_refusals.cmake, once per planted document.
foreach(_required IN ITEMS DSS_REPO_ROOT DOC)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "config nesting limit reader: -D ${_required} was not supplied.")
    endif()
endforeach()
include("${DSS_REPO_ROOT}/cmake/DssConfigNestingLimit.cmake")
dss_read_config_nesting_limit("${DOC}" _levels)
message(STATUS "maxNesting=${_levels}")
