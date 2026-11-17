# ═══ A COUNT THE CONFIG TREE STATES FOR THE BUILD, READ AT CONFIGURE TIME (cycle P69 f26d, lane `cs`) ═══
#
# Some limits are config data that no run-time document can carry: the config documents' nesting
# limit governs the reading of every config document, and the diagnostics' quoted-source-text
# limit belongs to no language, target or object format. Each lives in a document of the config
# tree's `configure-time/` directory, whose `$comment` gives the value's reason, and is read HERE,
# when the build is configured, then compiled in by the CMakeLists that forwards it. Those
# documents are never installed (cmake/DssInstall.cmake).
#
# dss_read_configure_time_count(<document> <key> <subject> <why> <unit> <out variable>)
#   Sets the variable to the document's <key>, a positive whole number, and makes the document a
#   configure dependency (an edit to it re-configures). REFUSES the configure, by name — the
#   sentence starts with <subject> and the document's path — for a document that is missing (the
#   sentence then says <why>), bytes that are not JSON, a top level that is not an object, no
#   <key>, or a <key> that is not a positive whole number of <unit>. There is no default: a build
#   without the value does not configure, and the library does not compile without its forward.

function(dss_read_configure_time_count _dss_ctc_doc _dss_ctc_key _dss_ctc_subject _dss_ctc_why
         _dss_ctc_unit _dss_ctc_out)
    set(_dss_ctc_where "${_dss_ctc_subject} ('${_dss_ctc_doc}')")
    if(NOT EXISTS "${_dss_ctc_doc}" OR IS_DIRECTORY "${_dss_ctc_doc}")
        message(FATAL_ERROR "${_dss_ctc_where}: no such document. ${_dss_ctc_why}")
    endif()
    file(READ "${_dss_ctc_doc}" _dss_ctc_text)
    string(JSON _dss_ctc_type ERROR_VARIABLE _dss_ctc_error TYPE "${_dss_ctc_text}")
    if(_dss_ctc_error)
        message(FATAL_ERROR "${_dss_ctc_where}: not JSON (${_dss_ctc_error}).")
    endif()
    if(NOT _dss_ctc_type STREQUAL "OBJECT")
        message(FATAL_ERROR
            "${_dss_ctc_where}: its top level is ${_dss_ctc_type}, not an object.")
    endif()
    string(JSON _dss_ctc_kind ERROR_VARIABLE _dss_ctc_error TYPE "${_dss_ctc_text}" ${_dss_ctc_key})
    if(_dss_ctc_error)
        message(FATAL_ERROR "${_dss_ctc_where}: it declares no `${_dss_ctc_key}`.")
    endif()
    string(JSON _dss_ctc_value GET "${_dss_ctc_text}" ${_dss_ctc_key})
    if(NOT _dss_ctc_kind STREQUAL "NUMBER" OR NOT _dss_ctc_value MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "${_dss_ctc_where}: `${_dss_ctc_key}` is '${_dss_ctc_value}' (${_dss_ctc_kind}); "
            "it must be a positive whole number of ${_dss_ctc_unit}.")
    endif()
    if(NOT CMAKE_SCRIPT_MODE_FILE)   # a script (the refusals' test) has no build to re-configure
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dss_ctc_doc}")
    endif()
    set(${_dss_ctc_out} "${_dss_ctc_value}" PARENT_SCOPE)
endfunction()
