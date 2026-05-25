#include "analysis/compilation_unit/import_resolver.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
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

// First Token child of `parent` whose token-kind is `kind`, or InvalidNode.
[[nodiscard]] NodeId firstChildOfKind(Tree const& tree, NodeId parent, SchemaTokenId kind) {
    for (NodeId child : tree.children(parent)) {
        if (tree.kind(child) == NodeKind::Token && tree.tokenKind(child) == kind) {
            return child;
        }
    }
    return InvalidNode;
}

// Read the quoted filename of a `#include "..."`. The StringStart token spans
// only the opening quote; the body is separate (off-grammar) StringChar tokens,
// so the content is read from the source text between the quotes rather than
// from the tree. Returns empty when the literal isn't a well-formed "...".
[[nodiscard]] std::string extractQuotedFilename(Tree const& tree, NodeId stringStart) {
    std::string_view const src = tree.source().text();
    ByteOffset const open = tree.span(stringStart).start();
    if (open >= src.size() || src[open] != '"') return {};
    auto const close = src.find('"', open + 1);
    if (close == std::string_view::npos) return {};
    return std::string(src.substr(open + 1, close - (open + 1)));
}

[[nodiscard]] std::optional<fs::path> resolveIncludePath(
    std::string_view                       filename,
    fs::path const&                        includingDir,
    std::span<fs::path const>              includeDirs) {
    fs::path const rel{filename};
    std::error_code ec;
    if (rel.is_absolute()) {
        return fs::exists(rel, ec) ? std::optional{rel} : std::nullopt;
    }
    if (!includingDir.empty()) {
        if (auto candidate = includingDir / rel; fs::exists(candidate, ec)) return candidate;
    }
    for (fs::path const& dir : includeDirs) {
        if (auto candidate = dir / rel; fs::exists(candidate, ec)) return candidate;
    }
    return std::nullopt;
}

// Text of the last `nameToken` token under `nameNode` — the table name in
// `db.schema.table`. Case-folded ONLY when `!caseSensitive` (SQL folds; the
// config carries the policy). Returns empty when the name is composed only of
// bracketed identifiers (`[Name]`), which v1 does not match (documented
// limitation: the matcher keys on the last `nameToken`).
[[nodiscard]] std::string lastIdentifierText(Tree const& tree, NodeId nameNode,
                                             SchemaTokenId nameToken, bool caseSensitive) {
    std::string last;
    walkPreOrder(tree, nameNode, [&](TreeCursor const& cursor) {
        NodeId const id = cursor.current();
        if (tree.kind(id) == NodeKind::Token && tree.tokenKind(id) == nameToken) {
            last = std::string(tree.text(id));
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
    explicit ConfigDrivenImportResolver(ImportConfig config) : config_(std::move(config)) {}

    void resolve(ResolutionContext& context) const override {
        switch (config_.strategy) {
            case ImportStrategy::None:             return;
            case ImportStrategy::IncludeFollowing: resolveIncludeFollowing(context); return;
            case ImportStrategy::NameMatching:     resolveNameMatching(context);     return;
        }
    }

private:
    // include-following: follow each `directiveRule` node's `pathToken` literal.
    void resolveIncludeFollowing(ResolutionContext& context) const {
        RuleId const includeRule = context.schema.rules().find(config_.directiveRule);
        if (!includeRule.valid()) return;
        SchemaTokenId const stringKind = context.schema.schemaTokens().find(config_.pathToken);

        struct Edge { TreeId source; NodeId node; SourceSpan span; TreeId target; };
        std::vector<Edge> edges;

        // Index-based loop: include-following appends to context.trees, and we
        // re-derive context.trees[i] each iteration (stable across reallocation)
        // rather than holding a Tree reference across loadFile.
        std::size_t index = 0;
        while (index < context.trees.size()) {
            struct Directive { NodeId node; SourceSpan span; std::string filename; };
            std::vector<Directive> directives;
            TreeId   sourceTree;
            BufferId sourceBuffer;
            fs::path includingDir;
            {
                Tree const& tree = context.trees[index];
                sourceTree   = tree.id();
                sourceBuffer = tree.source().id();
                includingDir = fs::path(std::string(tree.source().name())).parent_path();
                walkPreOrder(tree, [&](TreeCursor const& cursor) {
                    NodeId const node = cursor.current();
                    if (tree.kind(node) != NodeKind::Internal) return;
                    if (tree.rule(node) != includeRule) return;
                    NodeId const strNode = firstChildOfKind(tree, node, stringKind);
                    std::string filename =
                        strNode.valid() ? extractQuotedFilename(tree, strNode) : std::string{};
                    directives.push_back({node, tree.span(node), std::move(filename)});
                });
            }  // Tree reference released before any loadFile call below.

            for (Directive& directive : directives) {
                auto const unresolved = [&] {
                    reportDriver(context.diagnostics, DiagnosticCode::D_UnresolvedImport,
                                 DiagnosticSeverity::Warning, sourceBuffer, directive.span,
                                 directive.filename.empty() ? "<malformed include>"
                                                            : directive.filename);
                };
                if (directive.filename.empty()) { unresolved(); continue; }
                auto const resolved =
                    resolveIncludePath(directive.filename, includingDir, context.includeDirs);
                if (!resolved) { unresolved(); continue; }

                bool ok = false;
                TreeId const target = context.loadFile(*resolved, ok);
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
        GrammarSchema const& schema = context.schema;
        RuleId const qualifiedNameRule = schema.rules().find(config_.nameRule);
        if (!qualifiedNameRule.valid()) return;
        RuleId const createRule    = schema.rules().find(config_.definitionRule);
        SchemaTokenId const identifierKind = schema.schemaTokens().find(config_.nameToken);
        bool const caseSensitive = config_.caseSensitive;

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

        // Pass 1 — definitions: each definition node's name. First wins.
        std::unordered_map<std::string, std::pair<TreeId, NodeId>> definitions;
        for (Tree const& tree : context.trees) {
            forEachQualifiedName(tree, [&](NodeId node, RuleId parentRule) {
                if (parentRule != createRule) return;
                std::string name = lastIdentifierText(tree, node, identifierKind, caseSensitive);
                if (!name.empty()) definitions.emplace(std::move(name), std::pair{tree.id(), node});
            });
        }

        // Pass 2 — references: reference-position names resolved to a definition
        // in ANOTHER tree become cross-refs; no definition anywhere is a
        // D_UnresolvedReference; a same-tree match is intra-file (skip).
        for (Tree const& tree : context.trees) {
            BufferId const buffer = tree.source().id();
            forEachQualifiedName(tree, [&](NodeId node, RuleId parentRule) {
                if (!isTablePosition(parentRule)) return;
                std::string name = lastIdentifierText(tree, node, identifierKind, caseSensitive);
                if (name.empty()) return;  // bracket-id-only name — v1 doesn't match.
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

    ImportConfig config_;
};

} // namespace

std::unique_ptr<ImportResolver> chooseResolver(GrammarSchema const& schema) {
    return std::make_unique<ConfigDrivenImportResolver>(schema.imports());
}

} // namespace dss
