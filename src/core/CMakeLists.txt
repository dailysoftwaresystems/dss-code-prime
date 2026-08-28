add_library(core OBJECT
    compiler.cpp

    # ── substrate/ — host/OS utilities (the first non-header-only one) ──
    # The ONE checked whole-file read
    # (D-CORE-SHIPPED-CONFIG-LOADERS-DRAIN-A-STREAM-WITHOUT-CHECKING-IT).
    substrate/checked_file_read.cpp
    substrate/large_stack_call.cpp
    substrate/path_identity.cpp
    substrate/phase_timers.cpp
    substrate/process_spawn.cpp
    substrate/stack_sized_thread.cpp
    substrate/thread_pool.cpp

    # ── types/ — universal in-memory model ──
    types/source_span.cpp
    types/source_buffer.cpp
    types/rule_id.cpp
    types/scope_kind.cpp
    types/parse_diagnostic.cpp
    types/diagnostic_reporter.cpp
    types/unsuppressable_codes.cpp
    types/config_path_walk.cpp
    types/header_case_diagnostic.cpp
    types/include_path_resolve.cpp
    types/glob_match.cpp
    # The content-addressed memo every schema family loads through
    # (D-CONFIG-A-SCHEMA-DOCUMENT-IS-REBUILT-ONCE-PER-LOAD-INSIDE-ONE-PROCESS).
    types/config_document_memo.cpp
    types/grammar_schema.cpp
    types/grammar_schema_json.cpp
    types/predefined_macro_json.cpp
    # The ONE `.dss-project.json` reader. It lived in `src/program/` until
    # 2026-08-13, when the LSP tier's `workspace_project.cpp` — which reads the
    # same manifest to learn a workspace's assembly dialect — had to reach UP
    # into the driver tier to reuse it (`program` already links `lsp`, so the
    # two OBJECT libs referenced each other's headers).
    # D-LSP-PROJECT-CONFIG-LIVES-ABOVE-ITS-CONSUMERS: moved down to the tier
    # both readers already depend on, so the dependency runs one way again.
    types/project_config.cpp
    types/target_schema.cpp
    types/target_spec.cpp
    types/target_schema_json.cpp
    types/lexer_mode.cpp
    types/schema_walker.cpp
    types/string_style.cpp
    types/tree.cpp
    types/tree_builder.cpp
    types/tree_cursor.cpp
    types/type_lattice/type_lattice.cpp
    types/type_lattice/type_layout.cpp
    types/type_lattice/type_reintern.cpp
    types/aggregate_abi.cpp

    # ── crypto/ — self-contained hashing primitives (codesign foundation) ──
    crypto/sha256.cpp
)

target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# These OBJECT-lib sources are the actual compilation units that get folded
# into dsscp-lib (SHARED) via $<TARGET_OBJECTS:core>. Setting the
# defines on the SHARED target would not reach them — $<TARGET_OBJECTS> only
# copies finished .o files, not usage requirements — so DSS_BUILDING_DLL has
# to live here for __declspec(dllexport) in core/export.hpp to fire on MSVC.
target_compile_definitions(core PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# TF-C83 — forward the repo-root VERSION file to the `version` predefine kind
# (`core/types/predefined_macro_json.hpp`). `DSS_VERSION` is the STRIPPED
# contents of that file, already read by the top-level CMakeLists as the single
# source of truth for `project(VERSION ...)`; re-using the same variable is what
# keeps `__DSSCP__` from becoming a second place the version is written. The
# header `#error`s if this definition is missing, so a botched wireup fails the
# BUILD rather than silently predefining a wrong identity.
target_compile_definitions(core PRIVATE DSS_PROJECT_VERSION="${DSS_VERSION}")

# nlohmann/json is PRIVATE: the include path is a build requirement of `core`'s
# own sources, NOT a usage requirement re-exported to consumers.
#
# ⚠ MEASURED 2026-08-13, because the comment that stood here claimed
# "included by exactly one translation unit (grammar_schema_json.cpp) ... every
# other file in `core` sees just our public headers, never <nlohmann/json.hpp>"
# and BOTH halves had drifted false. The actual `#include <nlohmann/json.hpp>`
# set inside src/core/ is:
#     types/grammar_schema_json.cpp     types/target_schema_json.cpp
#     types/project_config.cpp          types/predefined_macro_json.hpp
#     substrate/relocation_table_json.hpp
# Keep this list honest: `grep -rn '#include <nlohmann' src/core/` is the whole
# instrument.
#
# ── THE CONVENTION, STATED CORRECTLY ─────────────────────────────────────────
# A public header does not put <nlohmann/json.hpp> on its consumers — EXCEPT a
# header whose declared job IS to accept a `nlohmann::json const&`, which names
# itself `_json` and is included only by TUs already holding a parsed document.
# The two entries above ending in `_json.hpp` are that sanctioned shape; the
# exception is mechanical, not taste, because a parser interface cannot hide the
# type it parses, and these are TEMPLATES with no `.cpp` to hide an include in.
#
# ⚠⚠ AN EARLIER VERSION OF THIS COMMENT DREW A CAUSAL CLAIM FROM THE LIST ABOVE
# THAT IS FALSE, AND IT WAS ABOUT TO DRIVE A DESIGN DECISION. It read: the two
# headers are why "`tests/`, `program`, `link`, `lsp` etc. carry their own
# `nlohmann_json::nlohmann_json` edges" — offered as evidence that this
# convention was never really enforced. ✔MEASURED 2026-08-13, all three parts
# wrong (D-CORE-JSON-LEAKS-INTO-TWO-PUBLIC-HEADERS):
#   (1) `program` carries NO nlohmann edge at all, and src/program/CMakeLists.txt
#       documents that absence explicitly as correct.
#   (2) EVERY other edge is self-explained: ffi(1), opt(1), link(2), lsp(3) each
#       `#include <nlohmann/json.hpp>` DIRECTLY in their own translation units.
#       Not one edge traces to a leaked core header.
#       ⚠ Measure this with the pattern anchored — `^#include <nlohmann` — never
#       a bare `grep -l nlohmann/json.hpp`. The loose form counts a file whose
#       COMMENT names the header: src/lsp/protocol.hpp says "NO
#       <nlohmann/json.hpp> in this header" and the loose grep scored it a
#       violation, briefly turning a refusal into a finding.
#   (3) So the fix those premises implied — "hide the JSON and watch the edges
#       disappear" — had no valid check behind it; the edges would not have moved.
# The real defect was narrower: `substrate/relocation_table.hpp` was a JSON
# parser interface NOT named as one. It is now split — the typed half (the
# `relocation_row` concept + `validateRelocationsTable`) keeps the plain name and
# pulls no JSON, so `object_format_schema.cpp` and `target_schema.cpp` stop
# seeing nlohmann for a function that never touched it.
target_link_libraries(core PRIVATE nlohmann_json::nlohmann_json)

# Big-object assembler flag for the large auto-generated grammar-schema TU.
# grammar_schema_json.cpp is a big generated translation unit that, under the
# MinGW/binutils toolchain, overflows the WINDOWS COFF object-section count on a
# from-scratch build ("file too big"). `-Wa,-mbig-obj` is the standard binutils
# remedy (it raises the COFF section limit). CRUCIAL: `-mbig-obj` is a COFF-ONLY
# assembler option — the Linux/macOS GNU `as` REJECTS it ("unrecognized option
# -mbig-obj"). GNU-on-Linux (ELF) is the SAME CXX_COMPILER_ID:GNU as MinGW, so a
# bare GNU scope wrongly breaks the Linux-GCC legs. The flag is therefore scoped
# THREE ways: (1) per-FILE via set_source_files_properties — only this one TU,
# every other `core` source byte-identical; (2) GNU AND (3) WINDOWS, via the
# $<$<AND:$<CXX_COMPILER_ID:GNU>,$<PLATFORM_ID:Windows>>:...> generator expression —
# so it is applied ONLY to a MinGW/GNU-on-Windows (COFF) build and expands to
# NOTHING for MSVC, Clang, and Linux/macOS GCC, whose compile lines provably cannot
# change. A toolchain-object-size accommodation, not a language/CPU/format branch.
set_source_files_properties(types/grammar_schema_json.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<AND:$<CXX_COMPILER_ID:GNU>,$<PLATFORM_ID:Windows>>:-Wa,-mbig-obj>")

# D-DIAG-CODENAME-EXHAUSTIVE-WARN (2026-06-09) — the exhaustive DiagnosticCode /
# DiagnosticSeverity dispatch gate on types/parse_diagnostic.cpp is now
# PROJECT-WIDE (root CMakeLists.txt). Both of that file's switches
# (`diagnosticCodeName`, `severityName`) are no-`default` exhaustive and stay
# covered; a new DiagnosticCode enumerator added without a name arm is still a
# BUILD ERROR on local MSVC.
#
# Worth keeping because it is what the gate was born from: TWELVE codes (the
# X_* optimizer/pipeline family plus A_FunctionEncodeAborted /
# K_AbsolutePointerRelocMissing / S_NotAComposite / S_NotAPointer) had drifted
# unhandled here, all of them silently returning "Unknown", visible only as a
# `-Wswitch` on the macOS / linux-clang CI legs.
#
# The old note warned against spreading the flag to grammar_schema.cpp /
# target_schema.cpp, whose switches use subset-with-`default` forms. Spreading
# it is now exactly what happened, and it is inert for them: `-Werror=switch` /
# C4062 do not fire on a switch that has a `default:` arm. ✔MEASURED — see
# D-BUILD-CLANG-ONLY-WARNINGS-INVISIBLE-TO-THE-MSVC-AND-GCC-LEGS for the
# whole-tree probe that found zero `-Wswitch` sites after consolidation.
