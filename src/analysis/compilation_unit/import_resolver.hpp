#pragma once

// Per-language import resolution (CU4). Bridges parser output and the semantic
// phase by populating a CompilationUnit's `crossRefs` — the cross-tree edges
// linking a use/reference in one file to its definition in another.
//
// Two resolution styles ship, dispatched by language NAME (plan Q5 — a
// schema-declared `imports.syntax` field is a v3 candidate):
//   - c-subset: include-following. `#include "x.h"` directives are resolved
//     against the including file's directory + declared include dirs; missing
//     files are LOADED into the CU (recursively), and each directive becomes a
//     CrossTreeRef (filename token -> included tree root, importSpan = directive).
//   - tsql-subset: name-matching. A table-position `qualifiedName` is matched to
//     a `CREATE TABLE` of the same name in ANOTHER tree (importSpan = nullopt).
//   - everything else (toy): identity — no cross-refs.
//
// Unresolved references are surfaced as driver diagnostics (D_UnresolvedImport
// / D_UnresolvedReference), never silently dropped.
//
// Diagnostics of a LOADED include live on that file's own Tree (consistent
// with CU2 C2-L1: lexer+parser diagnostics are per-Tree). An auto-loaded
// header is a full member of `cu.trees()` and carries its own
// `diagnostics()` — a consumer checking "did the whole CU parse" must walk
// every tree's diagnostics, exactly as it would for an explicitly-added file.
// `driverDiagnostics()` holds only the D_* driver codes.
//
// Known v1 limitations (documented, not bugs):
//   - c-subset `#include` is a single `"#include"` lexeme, so `# include`
//     (whitespace between `#` and `include`) is not recognized.
//   - `addInMemory` sources are never deduplicated against include targets;
//     if an in-memory `label` happens to equal a real on-disk path that
//     another file `#include`s, the file is loaded a second time from disk.
//   - tsql table names composed only of bracketed identifiers (`[Name]`) are
//     not matched (the matcher keys on the last `Identifier` token).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace dss {

// Everything a resolver needs from the in-progress CompilationUnit build, plus
// the outputs it produces. Lives only for the duration of one resolve() call.
struct DSS_EXPORT ResolutionContext {
    // The trees built so far. An include-following resolver appends to this via
    // `loadFile`; index-based access stays valid across that growth, but a
    // `Tree const&` / span into an element does NOT (see loadFile).
    std::vector<Tree>&                     trees;
    GrammarSchema const&                   schema;
    DiagnosticReporter&                    diagnostics;
    std::span<std::filesystem::path const> includeDirs;

    // Load + tokenize + parse `path`, append its Tree to `trees` (deduplicating
    // by weakly-canonical path), and return the resulting tree's TreeId. Sets
    // `ok` false (and returns InvalidTree) when the file can't be read.
    // MUST NOT be called while holding a reference/span into `trees` — it may
    // reallocate the vector.
    std::function<TreeId(std::filesystem::path const&, bool& ok)> loadFile;

    // Output: the resolved cross-tree edges. Resolvers append; never cleared.
    std::vector<CrossTreeRef>&             crossRefs;
};

class DSS_EXPORT ImportResolver {
public:
    virtual ~ImportResolver();
    ImportResolver()                                 = default;
    ImportResolver(ImportResolver const&)            = delete;
    ImportResolver& operator=(ImportResolver const&) = delete;

    virtual void resolve(ResolutionContext& context) const = 0;
};

// Pick the resolver for a language by name. Always returns a non-null resolver
// (unknown languages get the identity resolver).
[[nodiscard]] DSS_EXPORT std::unique_ptr<ImportResolver>
chooseResolver(std::string_view languageName);

} // namespace dss
