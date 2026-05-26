#pragma once

// Config-driven import resolution (CU4). Bridges parser output and the
// semantic phase by populating a CompilationUnit's `crossRefs` — the
// cross-tree edges linking a use/reference in one file to its definition in
// another.
//
// ONE language-agnostic engine (ConfigDrivenImportResolver) reads the schema's
// `imports` block (schema v4) — NO code branches on the source language name.
// The block's `strategy` selects one of two generic strategies (or none):
//   - include-following. Each `directiveRule` node carries a `pathToken` whose
//     quoted literal is resolved against the including file's directory +
//     declared include dirs; missing files are LOADED into the CU (recursively),
//     and each directive becomes a CrossTreeRef (path token -> included tree
//     root, importSpan = directive). (c-subset uses this.)
//   - name-matching. A `nameRule` in a `referenceParents` position is matched
//     to a `nameRule` under a `definitionRule` of the same name in ANOTHER tree
//     (importSpan = nullopt), keyed on the last `nameToken` text (case-folded
//     unless `caseSensitive`). (tsql-subset uses this.)
//   - none (or no `imports` block): identity — no cross-refs. (toy uses this.)
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
//   - include-following: a directive whose path lexeme is a single token (e.g.
//     c-subset's `"#include"`) won't recognize a split spelling (`# include`).
//   - `addInMemory` sources are never deduplicated against include targets;
//     if an in-memory `label` happens to equal a real on-disk path that
//     another file follows, the file is loaded a second time from disk.
//   - name-matching: a name composed only of bracketed identifiers (`[Name]`)
//     is not matched (the matcher keys on the last `nameToken`).

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

// Build the resolver from a schema's config-driven `imports` block. Always
// returns a non-null resolver — a schema with no `imports` block (or
// `strategy: "none"`) gets one that produces no cross-refs. NO dispatch on the
// language name: behavior comes entirely from `schema.imports()`.
[[nodiscard]] DSS_EXPORT std::unique_ptr<ImportResolver>
chooseResolver(GrammarSchema const& schema);

} // namespace dss
