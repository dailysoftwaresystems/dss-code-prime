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
#
# The read is the one every configure-time count takes (cmake/DssConfigureTimeCount.cmake,
# cycle P69 f26d), so this limit and the diagnostics' quoted-source-text limit are refused in
# the same sentences by the same code.

include("${CMAKE_CURRENT_LIST_DIR}/DssConfigureTimeCount.cmake")

function(dss_read_config_nesting_limit _dss_cnl_doc _dss_cnl_out)
    dss_read_configure_time_count(
        "${_dss_cnl_doc}" maxNesting
        "the config documents' nesting limit"
        "Every config document's reader takes its `maxNesting`, so the build cannot be configured without it."
        levels _dss_cnl_value)
    set(${_dss_cnl_out} "${_dss_cnl_value}" PARENT_SCOPE)
endfunction()
