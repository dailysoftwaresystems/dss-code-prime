#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace dss {

// Config-driven import-resolution declaration (schema v4 `imports` block).
//
// The single language-agnostic import engine (ConfigDrivenImportResolver)
// reads this struct instead of branching on the language name — per-language
// behavior comes entirely from the `.lang.json` config. Two generic
// strategies ship:
//
//   IncludeFollowing — directive-driven: each `directiveRule` node carries a
//     `pathToken` whose quoted literal is resolved against the including
//     file's directory + declared include dirs; the target is LOADED into the
//     CU (recursively) and the directive becomes a CrossTreeRef.
//   NameMatching — name-driven: a `nameRule` in one of `referenceParents`
//     positions is matched to a `nameRule` under a `definitionRule` in ANOTHER
//     tree, keyed on the last `nameToken` text (case-folded unless
//     `caseSensitive`).
//
// Rule/token names are stored as strings and resolved to RuleId/SchemaTokenId
// at resolve-time via the schema's interners (`rules().find()` /
// `schemaTokens().find()`). The LOADER validates every referenced name at load
// time (`C_UnknownShape` / `C_UnknownToken`), so a loaded schema is guaranteed
// resolvable; the resolver's `.find()`-miss tolerance is defensive only.
enum class ImportStrategy : std::uint8_t { None, IncludeFollowing, NameMatching };

struct DSS_EXPORT ImportConfig {
    ImportStrategy strategy = ImportStrategy::None;

    // include-following:
    std::string directiveRule;   // e.g. "includeDirective"
    std::string pathToken;       // e.g. "StringStart" — the LOCAL (quote)
                                 // form's path-opener token. Searched on
                                 // the including file's dir + includeDirs.
    // Optional SYSTEM (angle) form's path-opener token (e.g. "HeaderStart"
    // for c-subset's `#include <h>`). When a directive carries THIS token
    // instead of `pathToken`, the resolver searches the SYSTEM path
    // (SemanticConfig.shippedLibDirs, the analogue of C's /usr/include)
    // and HARD-FAILS (F_ShippedHeaderNotFound) on a miss — a missing
    // system header is a fatal error in C, not the soft D_UnresolvedImport
    // used for a missing quote-include. Empty ⇒ the language declares no
    // angle form (only the quote form resolves). Additive: a language may
    // declare both, one, or neither.
    std::string systemPathToken; // e.g. "HeaderStart"

    // name-matching:
    std::string nameRule;        // e.g. "qualifiedName"
    std::string definitionRule;  // e.g. "createTableStmt"
    std::vector<std::string> referenceParents;  // e.g. tableRef/insertStmt/updateStmt/deleteStmt
    std::string nameToken;       // e.g. "Identifier"
    bool caseSensitive = true;   // tsql sets false (SQL identifiers fold case)
};

} // namespace dss
