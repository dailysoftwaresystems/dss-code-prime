add_library(lsp OBJECT
    diagnostic_translator.cpp
    document_store.cpp
    json_rpc.cpp
    lsp_coordinates.cpp
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

# Aggregated into dsscp-lib via $<TARGET_OBJECTS:lsp> at
# src/CMakeLists.txt — same pattern as core/ and tokenizer/.
# DSS_BUILDING_DLL must live here (not on the SHARED target) so
# DSS_EXPORT decorations resolve to dllexport when these .obj files
# are produced on MSVC.
target_compile_definitions(lsp PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# nlohmann/json is PRIVATE — kept out of every lsp/*.hpp per the
# project convention (mirrors core/grammar_schema_json.cpp's
# isolation). ✔MEASURED 2026-08-13, and the header half of that claim
# HOLDS: `grep -rn '^#include <nlohmann' src/lsp/` finds no .hpp.
# ⚠ The second half had drifted. This said "linked in only because the
# json_rpc.cpp TU needs json::parse/dump"; it is THREE TUs —
# json_rpc.cpp, diagnostic_translator.cpp, lsp_server.cpp. Harmless to
# the build (the edge is needed either way), which is exactly why it
# went unnoticed: a comment that overstates isolation reads as a
# stronger invariant than the code actually keeps, and the next author
# to add a json include here would believe they were the first.
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
#
# ✅ THE PROJECT-MANIFEST HALF IS FIXED (D-LSP-PROJECT-CONFIG-LIVES-ABOVE-ITS-CONSUMERS,
# closed 2026-08-13). `workspace_project.cpp` used to include
# `program/project_config.hpp`; the parser now lives at
# `core/types/project_config.hpp` and is reached through the `core` edge already
# declared above. Same ONE parser, reused rather than re-implemented — only the
# direction changed, from sideways-and-up to straight down.
#
# ✅ AND SO IS THE TARGET-SPEC HALF
# (D-LSP-TARGET-SPEC-SPLITTER-LIVES-ABOVE-ITS-CONSUMERS, closed 2026-08-27).
# `workspace_project.cpp` used to include `program/target_spec.hpp` for
# `TargetSpec::parse`, the ONE `<targetName>:<formatName>` splitter — the last
# cross-tier include in this directory. The type could not follow
# `project_config` down while it carried
# `outputExtension(ObjectFormatSchema const&)`, which drags `link/` in.
#
# ★ WHAT MADE THE SPLIT CHEAP RATHER THAN A DESIGN COMPROMISE: ✔MEASURED
# against the body, `outputExtension` referenced NEITHER `targetName` NOR
# `formatName`. It was a `const` member that used nothing of `*this` — a free
# function about the FORMAT, wearing a member's clothes, and the disguise is
# what pinned the splitter to this tier. `TargetSpec` + `TargetSpecError` are
# now `core/types/target_spec.hpp`; the extension rule is the driver-tier free
# function `outputExtensionFor(ObjectFormatSchema const&)` in
# `program/target_spec.hpp`, which still includes the core header so every
# existing driver include compiles unchanged.
#
# `lsp` STILL gets NO link edge to `program`, and that is deliberate rather than
# an oversight:
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
#     `dsscp-lib` (src/CMakeLists.txt), so the symbols are already in
#     one image. Test binaries link that lib, not the object libs.
#
# Duplicating either reader here to avoid the smell would be strictly worse: an
# editor and a compiler that disagree about what a manifest MEANS is a bug class
# this tree keeps deleting. The pin that reds if anyone tries is named
# `LspAndDriverParseAManifestThroughTheSameLoader`
# (tests/lsp/test_workspace_project.cpp).
