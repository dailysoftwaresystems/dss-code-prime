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
# ✅ THE PROJECT-MANIFEST HALF IS FIXED (D-LSP-PROJECT-CONFIG-LIVES-ABOVE-ITS-
# CONSUMERS, closed 2026-08-13). `workspace_project.cpp` used to include
# `program/project_config.hpp`; the parser now lives at
# `core/types/project_config.hpp` and is reached through the `core` edge already
# declared above. Same ONE parser, reused rather than re-implemented — only the
# direction changed, from sideways-and-up to straight down.
#
# ⚠ ONE CROSS-TIER INCLUDE REMAINS, NAMED RATHER THAN GLOSSED:
# `workspace_project.cpp` still includes `program/target_spec.hpp` for
# `TargetSpec::parse`, the ONE `<targetName>:<formatName>` splitter. It could
# not follow `project_config` down, because `TargetSpec::outputExtension` takes
# an `ObjectFormatSchema const&` — so the type drags `link/` in, and `core` must
# not depend on `link`. Closing it means SPLITTING the type (pure `parse` in
# `core`, `outputExtension` left in the driver tier), which changes an API used
# by `program.cpp`, `dump_predefined_macros.cpp` and
# `tests/link/test_macho_dylib_writer.cpp` ⇒ an operator decision, anchored
# `D-LSP-TARGET-SPEC-SPLITTER-LIVES-ABOVE-ITS-CONSUMERS`.
#
# Until then `lsp` still gets NO link edge to `program`, and that is deliberate
# rather than an oversight:
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
# Duplicating either reader here to avoid the smell would be strictly worse: an
# editor and a compiler that disagree about what a manifest MEANS is a bug class
# this tree keeps deleting. The pin that reds if anyone tries is named
# `LspAndDriverParseAManifestThroughTheSameLoader`
# (tests/lsp/test_workspace_project.cpp).
