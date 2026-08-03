#pragma once

#include "core/substrate/diagnostic_collector.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/preprocess_config.hpp"

#include <nlohmann/json.hpp>

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// INTERNAL header — the ONE `predefinedMacros` array parser, shared by every
// config loader that declares predefined macros. Never included by a public
// type header (it pulls <nlohmann/json.hpp>, which the public surface keeps
// out by convention — see `grammar_schema_json.hpp`).
//
// TF-C74 (per-architecture identity predefines): predefined macros are now
// declared in TWO config families — the LANGUAGE (`preprocess.predefinedMacros`
// in `<lang>.lang.json`) and the TARGET (`predefinedMacros` in
// `<arch>.target.json`, the per-architecture identity macros). Both must obey
// the SAME grammar: the closed `kind` verb set, the Constant⇒`value` rule, the
// `params`/`isFunctionLike` function-like shape with its C 6.10.3p6
// duplicate-param + identifier checks, and the `availableObjectFormats`
// validation. Copying ~220 lines into the target loader would guarantee drift
// (the target side would silently miss the next rule the language side gains),
// so the parser is EXTRACTED here and both loaders call it.
//
// Behaviour is byte-for-byte the pre-extraction language-loader behaviour; the
// only parameterization is the diagnostic CODE and the JSON POINTER PREFIX,
// because each config family owns its own diagnostic vocabulary and its own
// path spelling (`/preprocess/predefinedMacros/3` vs `/predefinedMacros/3`).

// TF-C83: the BUILD's own version text, for the `version` predefine kind.
//
// SINGLE SOURCE OF TRUTH, NOT DUPLICATED DATA. The repo-root `VERSION` file is
// the one place the version is written. CMake ALREADY reads it (top-level
// `CMakeLists.txt` — `file(READ ... VERSION)` feeding `project(VERSION ...)`,
// with a comment saying that is exactly why); `src/core/CMakeLists.txt` merely
// forwards that same variable here. VERSION -> CMake -> this macro. Nothing
// restates the number.
//
// WHY BUILD-INJECTED AND NOT READ FROM DISK AT CONFIG-LOAD TIME. Schemas can be
// loaded from TEXT (`GrammarSchema::loadFromText`), where there is no config
// root and therefore no sibling `VERSION` to read — a disk-reading design would
// work for `loadShipped` and fail for `loadFromText`, i.e. it would fail
// exactly where it is least visible. A compile-time constant has no such seam,
// and it keeps schema parsing pure JSON->struct with no filesystem I/O.
//
// Undefined => the build is BROKEN, loudly, here and now. Defaulting to "0.0.0"
// would ship a compiler that quietly reports the wrong identity.
#ifndef DSS_PROJECT_VERSION
#    error "DSS_PROJECT_VERSION is not defined — src/core/CMakeLists.txt must forward the repo-root VERSION file (see top-level CMakeLists.txt)."
#endif

namespace dss::detail {

inline constexpr std::string_view kBuildVersionText = DSS_PROJECT_VERSION;
// NOTE: `packVersionComponents` — the pure value transform this kind uses — is
// declared in `preprocess_config.hpp`, NOT here. It has nothing to do with
// JSON, and this header pulls <nlohmann/json.hpp>, which `core` links PRIVATE;
// declaring it here would make it unreachable from tests.

// Parse a `predefinedMacros` JSON ARRAY into `out`, appending one
// `PredefinedMacroDef` per well-formed entry and emitting a diagnostic per
// malformed one (the malformed entry is SKIPPED, never half-built).
//
//   `pms`         — the array node. The CALLER must have verified `is_array()`
//                   and emitted its own family-specific message otherwise;
//                   this keeps the "must be an array" wording owned by the
//                   loader whose key vocabulary names it.
//   `arrayPath`   — the JSON pointer of the array itself, e.g.
//                   "/preprocess/predefinedMacros" or "/predefinedMacros".
//                   Per-entry paths are `{arrayPath}/{index}[/field]`.
//   `entryCode`   — the diagnostic code for a MALFORMED entry field
//                   (`C_InvalidPreprocess` for the language family,
//                   `C_MalformedJson` for the target family). A MISSING
//                   required field always emits `C_MissingField`, which is
//                   universal across both families.
//   `coll`        — the loader's diagnostic collector.
//   `out`         — appended to; never cleared.
//
// DUPLICATE NAMES within one array are rejected (`entryCode`): two entries
// spelling the same macro would make the effective value depend on which seed
// site iterated last — a silent last-writer-wins across the four preprocessor
// seed sites. Fail loud instead.
void parsePredefinedMacroArray(nlohmann::json const&           pms,
                               std::string_view                arrayPath,
                               DiagnosticCode                  entryCode,
                               substrate::DiagnosticCollector& coll,
                               std::vector<PredefinedMacroDef>& out);

} // namespace dss::detail
