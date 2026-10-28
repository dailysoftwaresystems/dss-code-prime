# ═══ THE CONFIG DOCUMENTS' NESTING LIMIT, READ AT CONFIGURE TIME (cycle P69 L2, lane `lm`) ═══
#
# Every reader of a DSS config document takes one limit on how deep the document may
# nest (`parseConfigDocument`, src/core/types/config_document_parse.hpp): a deeper one is
# refused by name before any loader walks it. The limit governs the reading of EVERY
# config document, so it cannot be one the compiler reads at run time; it is config data
# all the same, so it lives in the config tree — `configure-time/config-documents.json`,
# whose `$comment` gives the value's reason — and is read HERE, when the build is
# configured, then compiled in by src/core/CMakeLists.txt. That document is never
# installed (cmake/DssInstall.cmake), and the install smoke test compiles with the
# installed tree to prove nothing reads it at run time.
#
# dss_read_config_nesting_limit(<document> <out variable>)
#   Sets the variable to the document's `maxNesting`, a positive whole number, and makes
#   the document a configure dependency (an edit to it re-configures). REFUSES the
#   configure, by name, for a document that is missing, bytes that are not JSON, a top
#   level that is not an object, no `maxNesting`, or a `maxNesting` that is not a
#   positive whole number. There is no default: a build without the value does not
#   configure, and the library does not compile without its forward.

function(dss_read_config_nesting_limit _dss_cnl_doc _dss_cnl_out)
    set(_dss_cnl_where "the config documents' nesting limit ('${_dss_cnl_doc}')")
    if(NOT EXISTS "${_dss_cnl_doc}" OR IS_DIRECTORY "${_dss_cnl_doc}")
        message(FATAL_ERROR
            "${_dss_cnl_where}: no such document. Every config document's reader takes "
            "its `maxNesting`, so the build cannot be configured without it.")
    endif()
    file(READ "${_dss_cnl_doc}" _dss_cnl_text)
    string(JSON _dss_cnl_type ERROR_VARIABLE _dss_cnl_error TYPE "${_dss_cnl_text}")
    if(_dss_cnl_error)
        message(FATAL_ERROR "${_dss_cnl_where}: not JSON (${_dss_cnl_error}).")
    endif()
    if(NOT _dss_cnl_type STREQUAL "OBJECT")
        message(FATAL_ERROR
            "${_dss_cnl_where}: its top level is ${_dss_cnl_type}, not an object.")
    endif()
    string(JSON _dss_cnl_kind ERROR_VARIABLE _dss_cnl_error TYPE "${_dss_cnl_text}" maxNesting)
    if(_dss_cnl_error)
        message(FATAL_ERROR "${_dss_cnl_where}: it declares no `maxNesting`.")
    endif()
    string(JSON _dss_cnl_value GET "${_dss_cnl_text}" maxNesting)
    if(NOT _dss_cnl_kind STREQUAL "NUMBER" OR NOT _dss_cnl_value MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "${_dss_cnl_where}: `maxNesting` is '${_dss_cnl_value}' (${_dss_cnl_kind}); "
            "it must be a positive whole number of levels.")
    endif()
    if(NOT CMAKE_SCRIPT_MODE_FILE)   # a script (the refusals' test) has no build to re-configure
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dss_cnl_doc}")
    endif()
    set(${_dss_cnl_out} "${_dss_cnl_value}" PARENT_SCOPE)
endfunction()
