add_library(lsp OBJECT
    diagnostic_translator.cpp
    document_store.cpp
    json_rpc.cpp
    lsp_semantic_query.cpp
    lsp_server.cpp
    method_dispatcher.cpp
    protocol.cpp
    schema_cache.cpp
    transport.cpp
    utf16_column.cpp
    workspace_project.cpp
)

target_include_directories(lsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:lsp> at
# src/CMakeLists.txt — same pattern as core/ and tokenizer/.
# DSS_BUILDING_DLL must live here (not on the SHARED target) so
# DSS_EXPORT decorations resolve to dllexport when these .obj files
# are produced on MSVC.
target_compile_definitions(lsp PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# nlohmann/json is PRIVATE — kept out of every lsp/*.hpp per the
# project convention (mirrors core/grammar_schema_json.cpp's
# isolation). Linked in only because the json_rpc.cpp TU needs
# json::parse/dump.
target_link_libraries(lsp
    PRIVATE
        core
        tokenizer
        analysis_syntactic
        analysis_compilation_unit
        analysis_semantic
        nlohmann_json::nlohmann_json
)

# ── `program` IS DELIBERATELY ABSENT FROM THE LIST ABOVE ────────────────────
# `workspace_project.cpp` includes `program/project_config.hpp` +
# `program/target_spec.hpp` — the ONE project-manifest parser and the ONE
# `<target>:<format>` splitter, reused rather than re-implemented. It does NOT
# get a link edge, and that is deliberate rather than an oversight:
#
#   * `program` ALREADY declares `target_link_libraries(program PUBLIC ... lsp
#     ...)` (it owns the `--lsp` mode dispatch), so `lsp -> program` would close
#     a cycle. CMake tolerates link cycles only among STATIC libraries; these
#     are OBJECT libraries, so it would be a configure-time error.
#   * No edge is needed for COMPILATION: `target_include_directories(lsp PUBLIC
#     ${CMAKE_CURRENT_SOURCE_DIR}/..)` already puts `src/` on the include path,
#     which is what `#include "program/..."` resolves against.
#   * No edge is needed for LINKING: `$<TARGET_OBJECTS:lsp>` and
#     `$<TARGET_OBJECTS:program>` are both aggregated into the single
#     `dss-code-prime-lib` (src/CMakeLists.txt), so the symbols are already in
#     one image. Test binaries link that lib, not the object libs.
#
# ⚠ WHAT THIS COSTS, STATED: the two OBJECT libs now reference each other's
# headers, which is a layering smell even though the build graph stays acyclic.
# The real fix is to move `project_config.{hpp,cpp}` (and the `ResolveLibrarySpec`
# it borrows from `cli_args.hpp`) DOWN into `core`, where both tiers can depend
# on it in one direction. That is a `src/program/` + `src/core/` change ⇒
# `D-LSP-PROJECT-CONFIG-LIVES-ABOVE-ITS-CONSUMERS`. Duplicating the parser here
# to avoid the smell would have been strictly worse: an editor and a compiler
# that disagree about what a manifest MEANS is a bug class this tree keeps
# deleting.
