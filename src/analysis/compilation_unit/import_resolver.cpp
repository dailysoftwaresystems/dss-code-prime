#include "analysis/compilation_unit/import_resolver.hpp"

#include "core/types/include_path_resolve.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "core/types/string_style.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/tree_visitor.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace dss {

ImportResolver::~ImportResolver() = default;

namespace {

namespace fs = std::filesystem;

[[noreturn]] void resolverFatal(char const* what) {
    std::fputs("dss::ImportResolver fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// Mirror of compilation_unit.cpp's reportDriver — a driver-level diagnostic
// referencing a real source span (the import directive / reference site).
void reportDriver(DiagnosticReporter& reporter,
                  DiagnosticCode      code,
                  DiagnosticSeverity  severity,
                  BufferId            buffer,
                  SourceSpan          span,
                  std::string         actual) {
    ParseDiagnostic diagnostic;
    diagnostic.code     = code;
    diagnostic.severity = severity;
    diagnostic.buffer   = buffer;
    diagnostic.span     = span;
    diagnostic.actual   = std::move(actual);
    reporter.report(std::move(diagnostic));
}

[[nodiscard]] NodeId rootOfTree(std::vector<Tree> const& trees, TreeId id) {
    for (Tree const& tree : trees) {
        if (tree.id() == id) return tree.root();
    }
    // Only called with a TreeId that loadFile just returned with ok==true, so
    // the tree is always present. A miss is a broken invariant, not a
    // recoverable miss — fail loud rather than fabricate an InvalidNode edge.
    resolverFatal("rootOfTree: TreeId not present in the CompilationUnit");
}

// First Token DESCENDANT of `parent` whose token-kind is `kind`, or
// InvalidNode. A descendant (not direct-child) walk because the
// directive's path token may sit under an intermediate rule node — e.g.
// FF11 nests the target as `includeDirective → includeTarget →
// include{Quote,Angle}Target → <pathToken>`, so the StringStart /
// HeaderStart is a grandchild, not a direct child. Pre-order so the
// FIRST such token in source order wins (a well-formed directive has
// exactly one).
[[nodiscard]] NodeId firstDescendantOfKind(Tree const& tree, NodeId parent, SchemaTokenId kind) {
    NodeId found = InvalidNode;
    walkPreOrder(tree, parent, [&](TreeCursor const& cursor) {
        if (found.valid()) return;
        NodeId const id = cursor.current();
        if (tree.kind(id) == NodeKind::Token && tree.tokenKind(id) == kind) {
            found = id;
        }
    });
    return found;
}

// Read the quoted filename of a `#include "..."`. The StringStart token spans
// only the opening quote; the content is read from the source text between the
// quotes (the path is a filename, not a decoded string value, so the raw bytes
// are what we want — independent of how the body tokenizes). Returns empty when
// the literal isn't a well-formed "...".
[[nodiscard]] std::string extractQuotedFilename(Tree const& tree, NodeId stringStart) {
    std::string_view const src = tree.source().text();
    ByteOffset const open = tree.span(stringStart).start();
    if (open >= src.size() || src[open] != '"') return {};
    auto const close = src.find('"', open + 1);
    if (close == std::string_view::npos) return {};
    return std::string(src.substr(open + 1, close - (open + 1)));
}

// Read the angle-bracketed header name of a `#include <h.h>` (FF11). The
// HeaderStart token spans only the opening `<`; the path bytes run to
// the closing `>` (read from the source slice — the path is a filename,
// not a decoded string, mirroring the quote form). Returns empty when
// the `<...>` isn't well-formed.
[[nodiscard]] std::string extractAngleFilename(Tree const& tree, NodeId headerStart) {
    std::string_view const src = tree.source().text();
    ByteOffset const open = tree.span(headerStart).start();
    if (open >= src.size() || src[open] != '<') return {};
    auto const close = src.find('>', open + 1);
    if (close == std::string_view::npos) return {};
    return std::string(src.substr(open + 1, close - (open + 1)));
}

// `findInDirs` / `resolveIncludePath` now live in the SHARED
// `core/types/include_path_resolve.hpp` (FC15c): the import resolver and the
// preprocessor's `__has_include` operator call the SAME implementations so
// their "does this header exist" answers can never drift. The angle/system
// form goes through `resolveSystemDescriptor` (the `<stem>.json` mapping below).

// Read the inner text of a bracket-quoted identifier opener (tsql's
// `[Orders]` → "Orders", `[a]]b]` → "a]b"). The opener token spans only
// `[`; the body bytes are off-grammar default-mode tokens, so the content
// comes from the source slice. The `]]` doubled-delimiter un-escaping
// (matching the tokenizer's `EscapeKind::DoubledDelimiter` rule for `[`)
// lives in the shared `bracketInnerText` helper, which the semantic engine
// also calls — keeping both decoders byte-identical with the tokenizer.
// Returns empty on a malformed `[...]`.
[[nodiscard]] std::string bracketIdText(Tree const& tree, NodeId openerNode) {
    return bracketInnerText(tree.source().text(), tree.span(openerNode).start());
}

// Text of the last name-bearing leaf under `nameNode` — the table name in
// `db.schema.table`. A name leaf is the `nameToken` (plain identifier) OR,
// when set, the `bracketToken` (a bracket-quoted identifier opener; GAP D),
// whose inner text is read from the source slice. Case-folded ONLY when
// `!caseSensitive` (SQL folds; the config carries the policy). Returns empty
// when no name leaf is found.
[[nodiscard]] std::string lastIdentifierText(Tree const& tree, NodeId nameNode,
                                             SchemaTokenId nameToken,
                                             std::optional<SchemaTokenId> bracketToken,
                                             bool caseSensitive) {
    std::string last;
    walkPreOrder(tree, nameNode, [&](TreeCursor const& cursor) {
        NodeId const id = cursor.current();
        if (tree.kind(id) != NodeKind::Token) return;
        auto const tk = tree.tokenKind(id);
        if (nameToken.valid() && tk == nameToken) {
            last = std::string(tree.text(id));
        } else if (bracketToken.has_value() && bracketToken->valid()
                   && tk == *bracketToken) {
            if (std::string inner = bracketIdText(tree, id); !inner.empty()) {
                last = std::move(inner);
            }
        }
    });
    if (!caseSensitive) {
        for (char& ch : last) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return last;
}

// ── The single language-agnostic import engine ───────────────────────────────
//
// Branches on `config.strategy` — a config value read from the schema's
// `imports` block — NEVER on the language name. Each strategy is a private
// member function holding the corresponding generic algorithm.

class ConfigDrivenImportResolver final : public ImportResolver {
public:
    ConfigDrivenImportResolver(std::shared_ptr<GrammarSchema const> schema, ImportConfig config)
        : schema_(std::move(schema)), config_(std::move(config)) {}

    void resolve(ResolutionContext& context) const override {
        switch (config_.strategy) {
            case ImportStrategy::None:             return;
            case ImportStrategy::IncludeFollowing: resolveIncludeFollowing(context); return;
            case ImportStrategy::NameMatching:     resolveNameMatching(context);     return;
        }
    }

private:
    // True iff `tree` was built from THIS resolver's schema. In a multi-language
    // CU each resolver processes only its own language's trees (a c-subset
    // resolver's `directiveRule` RuleId means nothing in a tsql tree), so every
    // tree-iterating pass below gates on this. Homogeneous CU: always true.
    [[nodiscard]] bool owns(Tree const& tree) const {
        return tree.schema().schemaId() == schema_->schemaId();
    }

    // include-following: follow each `directiveRule` node's path literal.
    // TWO forms (FF11), distinguished by which path-opener token the
    // directive carries — NO branch on the language name:
    //   * QUOTE form (`config_.pathToken`, e.g. StringStart): a LOCAL
    //     include. Searched on the including file's dir + includeDirs; a
    //     miss is the SOFT D_UnresolvedImport (a local include may be
    //     provided by a later build step). The resolved file is LOADED as
    //     source under THIS schema and becomes a CrossTreeRef edge.
    //   * ANGLE/SYSTEM form (`config_.systemPathToken`, e.g.
    //     HeaderStart): a SYSTEM include resolving to a LANGUAGE-NEUTRAL
    //     JSON DESCRIPTOR (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC). The
    //     requested `<stdio.h>` is mapped to `<stem>.json` (`stdio.json`)
    //     and searched on `systemDirs` (shippedLibDirs); a HIT records the
    //     resolved path on `context.shippedLibDescriptors` (the semantic
    //     phase reads it + injects its extern symbols — NOT a source Tree,
    //     so no CrossTreeRef). A miss is the HARD F_ShippedHeaderNotFound
    //     (a missing system header is a fatal C error). Absent token ⇒
    //     the language declares no angle form.
    void resolveIncludeFollowing(ResolutionContext& context) const {
        RuleId const includeRule = schema_->rules().find(config_.directiveRule);
        if (!includeRule.valid()) return;
        // FC13 co-existence: when the config-SELECTED C preprocessor is enabled
        // it OWNS quote-`#include` end to end -- on success it removes the
        // directive (it never reaches the parser), and on FAILURE it leaves the
        // directive bytes AND already reported `P_PreprocessorIncludeError`. So
        // a quote include the parser still sees here is a PP-diagnosed failure;
        // re-reporting it as `D_UnresolvedImport` would double-diagnose one root
        // cause. We therefore SKIP the quote form when the PP is enabled. The
        // ANGLE form (systemKind, FF11 descriptors) is untouched -- the PP
        // passes angle includes through for THIS post-parse resolver to own.
        // Config-driven (a `preprocess().enabled` flag, never a language name).
        const bool ppEnabled = schema_->preprocess().enabled;
        SchemaTokenId const stringKind = schema_->schemaTokens().find(config_.pathToken);
        SchemaTokenId const systemKind = config_.systemPathToken.empty()
            ? SchemaTokenId{}
            : schema_->schemaTokens().find(config_.systemPathToken);

        struct Edge { TreeId source; NodeId node; SourceSpan span; TreeId target; };
        std::vector<Edge> edges;

        // Index-based loop: include-following appends to context.trees, and we
        // re-derive context.trees[i] each iteration (stable across reallocation)
        // rather than holding a Tree reference across loadFile.
        std::size_t index = 0;
        while (index < context.trees.size()) {
            struct Directive { NodeId node; SourceSpan span; std::string filename; bool isSystem; };
            std::vector<Directive> directives;
            TreeId   sourceTree;
            BufferId sourceBuffer;
            fs::path includingDir;
            {
                Tree const& tree = context.trees[index];
                if (!owns(tree)) { ++index; continue; }   // another language's tree
                sourceTree   = tree.id();
                sourceBuffer = tree.source().id();
                includingDir = fs::path(std::string(tree.source().name())).parent_path();
                walkPreOrder(tree, [&](TreeCursor const& cursor) {
                    NodeId const node = cursor.current();
                    if (tree.kind(node) != NodeKind::Internal) return;
                    if (tree.rule(node) != includeRule) return;
                    // Quote form first; fall back to the angle form. A
                    // well-formed directive carries exactly one path
                    // opener, so the order only matters for malformed
                    // input (both absent → empty filename → unresolved).
                    NodeId const strNode =
                        stringKind.valid() ? firstDescendantOfKind(tree, node, stringKind)
                                           : InvalidNode;
                    if (strNode.valid()) {
                        // PP-enabled: the preprocessor already owns + diagnosed
                        // this quote include (a failed one left its bytes here).
                        // Skipping avoids the D_UnresolvedImport double-report.
                        if (ppEnabled) return;
                        directives.push_back({node, tree.span(node),
                                              extractQuotedFilename(tree, strNode), false});
                        return;
                    }
                    NodeId const hdrNode =
                        systemKind.valid() ? firstDescendantOfKind(tree, node, systemKind)
                                           : InvalidNode;
                    if (hdrNode.valid()) {
                        directives.push_back({node, tree.span(node),
                                              extractAngleFilename(tree, hdrNode), true});
                        return;
                    }
                    // Neither opener present — a malformed `#include`.
                    directives.push_back({node, tree.span(node), std::string{}, false});
                });
            }  // Tree reference released before any loadFile call below.

            for (Directive& directive : directives) {
                // Miss handler: SYSTEM includes hard-fail (a missing
                // shipped header is fatal in C); LOCAL includes soft-fail
                // (D_UnresolvedImport Warning).
                auto const unresolved = [&] {
                    if (directive.isSystem) {
                        reportDriver(context.diagnostics, DiagnosticCode::F_ShippedHeaderNotFound,
                                     DiagnosticSeverity::Error, sourceBuffer, directive.span,
                                     directive.filename.empty() ? "<malformed system include>"
                                                                : directive.filename);
                    } else {
                        reportDriver(context.diagnostics, DiagnosticCode::D_UnresolvedImport,
                                     DiagnosticSeverity::Warning, sourceBuffer, directive.span,
                                     directive.filename.empty() ? "<malformed include>"
                                                                : directive.filename);
                    }
                };
                if (directive.filename.empty()) { unresolved(); continue; }

                // ANGLE/SYSTEM form → NEUTRAL JSON DESCRIPTOR
                // (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC). A system include
                // resolves to `<stem>.json` on the systemDirs path, NOT a
                // source header: `#include <stdio.h>` maps to `stdio.json`.
                // The descriptor is a language-neutral symbol table, so it is
                // recorded (for the semantic phase to read) rather than loaded
                // + parsed as a Tree — it has no source syntax and produces no
                // CrossTreeRef. A miss is the SAME hard F_ShippedHeaderNotFound
                // as before (a missing system header is a fatal C error). The
                // mapping drops the extension + PRESERVES any subdirectory
                // (`<stdio.h>` → `stdio.json`; `<sys/types.h>` → `sys/types.json`,
                // distinct from `<time.h>` → `time.json`) — agnostic of the
                // requested extension spelling (`<stdio.h>`, `<stdio>` → `stdio.json`).
                if (directive.isSystem) {
                    // The `<stem>.json` mapping is the SHARED
                    // `resolveSystemDescriptor` (FC15c funnel) so this resolution
                    // and `__has_include(<h>)` agree byte-for-byte.
                    auto const resolved = resolveSystemDescriptor(
                        directive.filename, context.systemDirs);
                    if (!resolved) { unresolved(); continue; }
                    // Carry the include directive's span + buffer so the per-target
                    // SEMANTIC availability gate can position its diagnostic on the
                    // `#include` line (the format isn't known until semantic time).
                    context.shippedLibDescriptors.push_back(
                        ShippedDescriptorRef{*resolved, directive.span, sourceBuffer});
                    continue;
                }

                // QUOTE form: a LOCAL include, loaded as source under THIS
                // schema (the included header is a source file in the same
                // language) and recorded as a CrossTreeRef edge.
                auto const resolved =
                    resolveIncludePath(directive.filename, includingDir, context.includeDirs);
                if (!resolved) { unresolved(); continue; }

                bool ok = false;
                TreeId const target = context.loadFile(*resolved, ok, schema_);
                if (!ok) { unresolved(); continue; }
                edges.push_back({sourceTree, directive.node, directive.span, target});
            }
            ++index;
        }

        // context.trees is stable now; resolve each edge's target root.
        for (Edge const& edge : edges) {
            context.crossRefs.push_back(CrossTreeRef{
                edge.source, edge.node, edge.target, rootOfTree(context.trees, edge.target), edge.span});
        }
    }

    // name-matching: a `nameRule` in a `referenceParents` position resolved to a
    // `nameRule` under a `definitionRule` of the same name in ANOTHER tree.
    void resolveNameMatching(ResolutionContext& context) const {
        GrammarSchema const& schema = *schema_;
        RuleId const qualifiedNameRule = schema.rules().find(config_.nameRule);
        if (!qualifiedNameRule.valid()) return;
        RuleId const createRule    = schema.rules().find(config_.definitionRule);
        SchemaTokenId const identifierKind = schema.schemaTokens().find(config_.nameToken);
        bool const caseSensitive = config_.caseSensitive;
        // GAP D: a bracket-quoted identifier (`[Orders]`) also counts as a
        // name leaf when the semantics block declares a bracketIdentifierToken.
        std::optional<SchemaTokenId> const bracketKind =
            schema.semantics().bracketIdentifierToken;

        // A reference-position name is a direct child of one of these.
        std::vector<RuleId> tablePositions;
        tablePositions.reserve(config_.referenceParents.size());
        for (std::string const& parent : config_.referenceParents) {
            tablePositions.push_back(schema.rules().find(parent));
        }
        auto const isTablePosition = [&](RuleId parentRule) {
            for (RuleId rule : tablePositions) {
                if (rule.valid() && rule == parentRule) return true;
            }
            return false;
        };

        // Visit every (name node, its Internal parent rule) pair in a tree —
        // the shared shape of both the definition and reference passes.
        auto const forEachQualifiedName = [&](Tree const& tree, auto&& onMatch) {
            walkPreOrder(tree, [&](TreeCursor const& cursor) {
                NodeId const node = cursor.current();
                if (tree.kind(node) != NodeKind::Internal || tree.rule(node) != qualifiedNameRule) return;
                NodeId const parent = tree.parent(node);
                if (!parent.valid() || tree.kind(parent) != NodeKind::Internal) return;
                onMatch(node, tree.rule(parent));
            });
        };

        // Pass 1 — definitions: each definition node's name. First wins. Only
        // this language's trees (a definition is matched against same-language
        // references; cross-language linkage is the FFI plan's job — HR11 defers).
        std::unordered_map<std::string, std::pair<TreeId, NodeId>> definitions;
        for (Tree const& tree : context.trees) {
            if (!owns(tree)) continue;
            forEachQualifiedName(tree, [&](NodeId node, RuleId parentRule) {
                if (parentRule != createRule) return;
                std::string name = lastIdentifierText(tree, node, identifierKind, bracketKind, caseSensitive);
                if (!name.empty()) definitions.emplace(std::move(name), std::pair{tree.id(), node});
            });
        }

        // Pass 2 — references: reference-position names resolved to a definition
        // in ANOTHER tree become cross-refs; no definition anywhere is a
        // D_UnresolvedReference; a same-tree match is intra-file (skip).
        for (Tree const& tree : context.trees) {
            if (!owns(tree)) continue;
            BufferId const buffer = tree.source().id();
            forEachQualifiedName(tree, [&](NodeId node, RuleId parentRule) {
                if (!isTablePosition(parentRule)) return;
                std::string name = lastIdentifierText(tree, node, identifierKind, bracketKind, caseSensitive);
                if (name.empty()) return;  // no resolvable name leaf.
                auto const it = definitions.find(name);
                if (it == definitions.end()) {
                    reportDriver(context.diagnostics, DiagnosticCode::D_UnresolvedReference,
                                 DiagnosticSeverity::Warning, buffer, tree.span(node),
                                 std::string(tree.source().slice(tree.span(node))));
                    return;
                }
                if (it->second.first == tree.id()) return;  // intra-file reference.
                context.crossRefs.push_back(CrossTreeRef{
                    tree.id(), node, it->second.first, it->second.second, std::nullopt});
            });
        }
    }

    std::shared_ptr<GrammarSchema const> schema_;
    ImportConfig                         config_;
};

} // namespace

std::unique_ptr<ImportResolver> chooseResolver(std::shared_ptr<GrammarSchema const> schema) {
    ImportConfig config = schema->imports();
    return std::make_unique<ConfigDrivenImportResolver>(std::move(schema), std::move(config));
}

} // namespace dss
