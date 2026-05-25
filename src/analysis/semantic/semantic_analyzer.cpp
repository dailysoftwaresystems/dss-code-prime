#include "analysis/semantic/semantic_analyzer.hpp"

#include "analysis/compilation_unit/unit_attribute.hpp"
#include "analysis/semantic/scope_tree.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/symbol_table.hpp"
#include "analysis/semantic/type_rules.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/type_lattice/type_lattice.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The single, language-agnostic SchemaDrivenSemantics engine. Reads the
// schema's `SemanticConfig` and:
//
//   Pass 1 (pre-order, per tree): identify declarations, push/pop scopes,
//     mint SymbolIds, bind names in the scope tree, emit
//     S_RedeclaredSymbol on same-scope duplicates. Forward references
//     (G-209) fall out because all decls are minted before any use is
//     resolved.
//
//   Pass 1.5 (post-order, per tree): resolve declaration types from
//     their `type` subtree per `typeShapes`/`builtinTypes`.
//
//   Pass 2 (post-order, per tree): resolve identifier USES (the
//     `references` rules) against the scope chain, propagate symbol
//     types to use sites, type literal leaves per `literalTypes`, and
//     emit S_UndeclaredIdentifier on misses. Initializer-based type
//     inference fills in untyped declarations.
//
// Per-tree-root scopes: each tree gets its OWN root scope (a child of
// the shared CU root). Pass 1 declares a tree's top-level symbols into
// THAT tree's root scope — so two unrelated files each declaring
// top-level `var a` do not collide and a symbol in file A is NOT
// visible from file B without an explicit import edge.
//
// Cross-tree symbol visibility: after Pass 1 on EVERY tree (so all
// target symbols exist), for each CrossTreeRef in cu.crossRefs() we
// inject every binding of the EDGE-TARGET tree's root scope into the
// EDGE-SOURCE tree's root scope. CU4's ImportResolver sets source =
// the referencing/including tree and target = the defining/included
// tree (verified against import_resolver.cpp for both IncludeFollowing
// and NameMatching strategies), so the referencing file sees the
// defining file's symbols. This is exactly what `injectBinding` was
// built for.

namespace dss {

namespace {

// One transient per `analyze()` call. Consumed into the returned model.
struct EngineState {
    explicit EngineState(CompilationUnit const& cu)
        : lattice{cu.id(), std::string{cu.schema().name()}},
          nodeToSymbol{cu},
          nodeToType{cu} {}

    DiagnosticReporter         reporter;
    TypeLattice                lattice;
    ScopeTree                  scopes;
    SymbolTable                symbols;
    UnitAttribute<SymbolId>    nodeToSymbol;
    UnitAttribute<TypeId>      nodeToType;
    // Per-rule index caches.
    std::unordered_map<std::uint32_t, std::size_t> declByRule;
    std::unordered_map<std::uint32_t, std::size_t> refByRule;
    std::unordered_map<std::uint32_t, std::size_t> typeShapeByRule;
    std::unordered_map<std::uint32_t, bool>        scopeByRule;
    // Built-in type name → TypeId (interned once per CU).
    std::unordered_map<std::string, TypeId>        builtinTypeIds;
    // Literal token-kind → TypeId.
    std::unordered_map<std::uint32_t, TypeId>      literalTypeIds;

    [[nodiscard]] TypeId typeAt(NodeId id) const {
        auto const* p = nodeToType.tryGet(id);
        return p ? *p : InvalidType;
    }
};

// Visible (non-EmptySpace) children of `parent` — the indexing convention
// the v4 `semantics` block uses for declaration/typeShape child positions.
[[nodiscard]] std::vector<NodeId>
visibleChildren(Tree const& tree, NodeId parent) {
    std::vector<NodeId> out;
    for (auto const& child : tree.children(parent)) {
        if (!isEmptySpace(tree.flags(child))) out.push_back(child);
    }
    return out;
}

// Extract identifier text + the bound NodeId per the requested matching mode.
//   Self           — the node IS the name (or wraps a single visible
//                    identifier).
//   LastIdentifier — depth-first scan; the LAST Identifier token wins.
struct ResolvedName {
    std::string name;
    NodeId      node;
};

[[nodiscard]] ResolvedName
extractNameNode(Tree const& tree, NodeId node, NameMatchMode mode,
                SchemaTokenId idKind) {
    if (!node.valid()) return {};
    if (mode == NameMatchMode::Self) {
        if (tree.kind(node) == NodeKind::Token) {
            return {std::string{tree.text(node)}, node};
        }
        auto kids = visibleChildren(tree, node);
        if (kids.size() == 1) {
            // Recurse one level so an `expression[Identifier]` wrapper
            // bottoms out at the identifier token (not the wrapper).
            return extractNameNode(tree, kids[0], NameMatchMode::Self, idKind);
        }
        return {std::string{tree.text(node)}, node};
    }
    // LastIdentifier — DFS for the last identifier token (the kind the
    // schema's `semantics.identifierToken` names; resolved by the loader).
    NodeId found{};
    std::vector<NodeId> stack{node};
    while (!stack.empty()) {
        NodeId cur = stack.back();
        stack.pop_back();
        if (tree.kind(cur) == NodeKind::Token && idKind.valid()
            && tree.tokenKind(cur) == idKind) {
            found = cur;
        }
        auto kids = tree.children(cur);
        // Reverse-push so visit order is left-to-right.
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            if (!isEmptySpace(tree.flags(*it))) stack.push_back(*it);
        }
    }
    if (found.valid()) return {std::string{tree.text(found)}, found};
    return {};
}

void buildIndexes(EngineState& s, SemanticConfig const& cfg) {
    for (std::size_t i = 0; i < cfg.declarations.size(); ++i) {
        s.declByRule[cfg.declarations[i].rule.v] = i;
    }
    for (std::size_t i = 0; i < cfg.references.size(); ++i) {
        s.refByRule[cfg.references[i].rule.v] = i;
    }
    for (std::size_t i = 0; i < cfg.typeShapes.size(); ++i) {
        s.typeShapeByRule[cfg.typeShapes[i].rule.v] = i;
    }
    for (auto const& sc : cfg.scopes) s.scopeByRule[sc.rule.v] = true;
    for (auto const& bt : cfg.builtinTypes) {
        s.builtinTypeIds[bt.name] = s.lattice.interner().primitive(bt.core);
    }
    for (auto const& lt : cfg.literalTypes) {
        s.literalTypeIds[lt.literal.v] = s.lattice.interner().primitive(lt.core);
    }
}

// Resolve a type-position subtree to a TypeId. Walks `typeShapes`
// recursively (e.g. pointerType[innerType] → Ptr<innerType>) and looks
// the leaf up in `builtinTypes`. Emits `S_UnknownType` when no matching
// mapping was found in the subtree's leaf.
[[nodiscard]] TypeId
resolveTypeNode(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                NodeId node, bool emitOnMiss = true) {
    if (!node.valid()) return InvalidType;
    auto const k = tree.kind(node);
    if (k == NodeKind::Internal) {
        auto const rule = tree.rule(node);
        auto it = s.typeShapeByRule.find(rule.v);
        if (it != s.typeShapeByRule.end()) {
            auto const& shape = cfg.typeShapes[it->second];
            auto kids = visibleChildren(tree, node);
            if (shape.operandChild < 0
                || static_cast<std::size_t>(shape.operandChild) >= kids.size()) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::S_UnknownType;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = tree.source().id();
                d.span     = tree.span(node);
                d.actual   = std::string{tree.text(node)};
                s.reporter.report(std::move(d));
                return InvalidType;
            }
            TypeId inner = resolveTypeNode(s, cfg, tree, kids[shape.operandChild], emitOnMiss);
            switch (shape.constructor) {
                case TypeConstructor::Pointer:
                    return s.lattice.interner().pointer(inner);
                case TypeConstructor::Reference:
                    return s.lattice.interner().reference(inner);
                case TypeConstructor::Nullable:
                    return s.lattice.interner().nullable(inner);
                case TypeConstructor::Optional:
                    return s.lattice.interner().optional(inner);
                case TypeConstructor::Slice:
                    return s.lattice.interner().slice(inner);
            }
            return InvalidType;
        }
        auto kids = visibleChildren(tree, node);
        for (auto child : kids) {
            auto inner = resolveTypeNode(s, cfg, tree, child, /*emitOnMiss=*/false);
            if (inner.valid()) return inner;
        }
        if (emitOnMiss) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::S_UnknownType;
            d.severity = DiagnosticSeverity::Error;
            d.buffer   = tree.source().id();
            d.span     = tree.span(node);
            d.actual   = std::string{tree.text(node)};
            s.reporter.report(std::move(d));
        }
        return InvalidType;
    }
    if (k == NodeKind::Token) {
        std::string const text{tree.text(node)};
        auto it = s.builtinTypeIds.find(text);
        if (it != s.builtinTypeIds.end()) return it->second;
        return InvalidType;
    }
    return InvalidType;
}

// ── Pass 1: pre-order — mint decls + push/pop scopes ───────────────────────
void pass1(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
           NodeId node, ScopeId current) {
    if (!node.valid()) return;
    if (isEmptySpace(tree.flags(node))) return;
    auto const k = tree.kind(node);

    ScopeId here = current;

    if (k == NodeKind::Internal) {
        auto const rule = tree.rule(node);

        if (s.scopeByRule.contains(rule.v)) {
            here = s.scopes.pushScope(current, node, tree.id());
        }

        auto declIt = s.declByRule.find(rule.v);
        if (declIt != s.declByRule.end()) {
            auto const& decl = cfg.declarations[declIt->second];
            auto kids = visibleChildren(tree, node);
            if (decl.nameChild.has_value() && *decl.nameChild < kids.size()) {
                NodeId const nameContainer = kids[*decl.nameChild];
                auto resolved = extractNameNode(
                    tree, nameContainer, decl.nameMatch, cfg.identifierToken);
                if (!resolved.name.empty() && resolved.node.valid()) {
                    SymbolRecord rec;
                    rec.name         = resolved.name;
                    rec.scope        = here;
                    rec.declNode     = resolved.node;
                    rec.declRuleNode = node;
                    rec.tree         = tree.id();
                    rec.kind         = decl.kind;

                    SymbolId const newId = s.symbols.mint(rec);
                    SymbolId const prior = s.scopes.bind(here, resolved.name, newId);
                    if (prior.valid()) {
                        ParseDiagnostic d;
                        d.code     = DiagnosticCode::S_RedeclaredSymbol;
                        d.severity = DiagnosticSeverity::Error;
                        d.buffer   = tree.source().id();
                        d.span     = tree.span(resolved.node);
                        d.actual   = resolved.name;
                        auto const& priorRec = s.symbols.at(prior);
                        if (priorRec.tree.v == tree.id().v) {
                            d.related.push_back(RelatedLocation{
                                tree.source().id(),
                                tree.span(priorRec.declNode),
                                "previously declared here",
                            });
                        }
                        s.reporter.report(std::move(d));
                    } else {
                        s.nodeToSymbol.set(resolved.node, newId);
                    }
                }
            }
        }
    }

    for (auto const& child : tree.children(node)) {
        pass1(s, cfg, tree, child, here);
    }
}

// ── Pass 1.5: post-order — resolve declaration types ───────────────────────
void resolveDeclTypes(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                      NodeId node) {
    if (!node.valid()) return;
    if (isEmptySpace(tree.flags(node))) return;

    if (tree.kind(node) == NodeKind::Internal) {
        auto const rule = tree.rule(node);
        auto declIt = s.declByRule.find(rule.v);
        if (declIt != s.declByRule.end()) {
            auto const& decl = cfg.declarations[declIt->second];
            auto kids = visibleChildren(tree, node);
            if (decl.typeChild.has_value() && *decl.typeChild < kids.size()
                && decl.nameChild.has_value() && *decl.nameChild < kids.size()) {
                TypeId const ty = resolveTypeNode(s, cfg, tree, kids[*decl.typeChild]);
                auto resolved = extractNameNode(
                    tree, kids[*decl.nameChild], decl.nameMatch, cfg.identifierToken);
                if (resolved.node.valid()) {
                    auto const* sym = s.nodeToSymbol.tryGet(resolved.node);
                    if (sym && sym->valid()) {
                        s.symbols.at(*sym).type = ty;
                        if (ty.valid()) {
                            s.nodeToType.set(resolved.node, ty);
                        }
                    }
                }
            }
        }
    }

    for (auto const& child : tree.children(node)) {
        resolveDeclTypes(s, cfg, tree, child);
    }
}

// ── Pass 2: post-order — resolve uses + literal/init typing ────────────────
//
// `current` is the active scope when entering `node`. Mirrors Pass 1's
// push/pop by locating the child scope minted for an in-scope rule.
void pass2(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
           NodeId node, ScopeId current) {
    if (!node.valid()) return;
    if (isEmptySpace(tree.flags(node))) return;
    auto const k = tree.kind(node);
    ScopeId here = current;

    if (k == NodeKind::Internal && s.scopeByRule.contains(tree.rule(node).v)) {
        for (auto child : s.scopes.scopes()[current.v].children) {
            auto const& rec = s.scopes.scopes()[child.v];
            if (rec.anchor.v == node.v && rec.tree.v == tree.id().v) {
                here = child;
                break;
            }
        }
    }

    for (auto const& child : tree.children(node)) {
        pass2(s, cfg, tree, child, here);
    }

    // Literal typing.
    if (k == NodeKind::Token) {
        auto tk = tree.tokenKind(node);
        auto litIt = s.literalTypeIds.find(tk.v);
        if (litIt != s.literalTypeIds.end()) {
            s.nodeToType.set(node, litIt->second);
        }
    }

    // Reference resolution. Skip if the node is a declaration's own name
    // slot (declaration and reference shapes can structurally overlap).
    if (k == NodeKind::Internal) {
        auto const rule = tree.rule(node);
        auto refIt = s.refByRule.find(rule.v);
        if (refIt != s.refByRule.end()) {
            bool isDeclSite = false;
            NodeId parent = tree.parent(node);
            if (parent.valid() && tree.kind(parent) == NodeKind::Internal) {
                auto parentRule = tree.rule(parent);
                auto parentDeclIt = s.declByRule.find(parentRule.v);
                if (parentDeclIt != s.declByRule.end()) {
                    auto const& parentDecl = cfg.declarations[parentDeclIt->second];
                    auto kids = visibleChildren(tree, parent);
                    if (parentDecl.nameChild.has_value()
                        && *parentDecl.nameChild < kids.size()
                        && kids[*parentDecl.nameChild].v == node.v) {
                        isDeclSite = true;
                    }
                }
            }
            if (!isDeclSite) {
                auto const& ref = cfg.references[refIt->second];
                auto resolved = extractNameNode(
                    tree, node, ref.nameMatch, cfg.identifierToken);
                // Only treat the site as an identifier USE when the resolved
                // leaf is genuinely the identifier token (per the config's
                // `identifierToken`). Reference rules can structurally cover
                // non-name leaves (e.g. c-subset's `operand` covers
                // IntLiteral too); resolving those as names would emit
                // phantom S_UndeclaredIdentifier diagnostics.
                bool isIdentifier = false;
                if (cfg.identifierToken.valid() && resolved.node.valid()
                    && tree.kind(resolved.node) == NodeKind::Token
                    && tree.tokenKind(resolved.node) == cfg.identifierToken) {
                    isIdentifier = true;
                }
                if (isIdentifier && !resolved.name.empty()) {
                    SymbolId const found = s.scopes.lookup(here, resolved.name);
                    if (found.valid()) {
                        s.nodeToSymbol.set(resolved.node, found);
                        auto const& rec = s.symbols.at(found);
                        if (rec.type.valid()) {
                            s.nodeToType.set(resolved.node, rec.type);
                            s.nodeToType.set(node, rec.type);
                        }
                    } else {
                        ParseDiagnostic d;
                        d.code     = DiagnosticCode::S_UndeclaredIdentifier;
                        d.severity = DiagnosticSeverity::Error;
                        d.buffer   = tree.source().id();
                        d.span     = tree.span(resolved.node);
                        d.actual   = resolved.name;
                        s.reporter.report(std::move(d));
                    }
                }
            }
        }
    }

    // Initializer-based type inference for declarations whose declared
    // type was absent (e.g. toy's `var x = ...;` has no `typeChild`).
    // ALSO: type-check assignment when an explicit type IS present.
    if (k == NodeKind::Internal) {
        auto const rule = tree.rule(node);
        auto declIt = s.declByRule.find(rule.v);
        if (declIt != s.declByRule.end()) {
            auto const& decl = cfg.declarations[declIt->second];
            auto kids = visibleChildren(tree, node);
            if (decl.initChild.has_value() && *decl.initChild < kids.size()
                && decl.nameChild.has_value() && *decl.nameChild < kids.size()) {
                NodeId initNode = kids[*decl.initChild];
                TypeId initTy = s.typeAt(initNode);
                auto resolved = extractNameNode(
                    tree, kids[*decl.nameChild], decl.nameMatch, cfg.identifierToken);
                if (resolved.node.valid()) {
                    auto const* symPtr = s.nodeToSymbol.tryGet(resolved.node);
                    if (symPtr && symPtr->valid()) {
                        auto& rec = s.symbols.at(*symPtr);
                        if (!rec.type.valid()) {
                            if (initTy.valid()) {
                                rec.type = initTy;
                                s.nodeToType.set(resolved.node, initTy);
                            }
                        } else if (initTy.valid()
                                   && !isAssignable(s.lattice.interner(),
                                                    rec.type, initTy)) {
                            ParseDiagnostic d;
                            d.code     = DiagnosticCode::S_TypeMismatch;
                            d.severity = DiagnosticSeverity::Error;
                            d.buffer   = tree.source().id();
                            d.span     = tree.span(initNode);
                            d.actual   = std::string{tree.text(initNode)};
                            s.reporter.report(std::move(d));
                        }
                    }
                }
            }
        }
    }
}

} // namespace

SemanticModel analyze(std::shared_ptr<CompilationUnit const> cu) {
    if (!cu) {
        std::fputs("dss::analyze fatal: null CompilationUnit\n", stderr);
        std::abort();
    }
    EngineState s{*cu};
    auto const& cfg = cu->schema().semantics();
    buildIndexes(s, cfg);

    // Register the schema's type extensions into the lattice's registry
    // (tsql's TSQL::Varchar etc.).
    registerSchemaTypeExtensions(s.lattice.registry(), cu->schema());

    // Each tree gets its OWN root scope (a child of the CU root) so two
    // unrelated files' top-level decls live in separate namespaces.
    auto const trees = cu->trees();
    ScopeId const cuRoot = s.scopes.root();
    std::unordered_map<std::uint32_t /*TreeId.v*/, ScopeId> treeRootScope;

    // Pass 1 per tree, declaring into THAT tree's root scope.
    for (auto const& tree : trees) {
        if (!tree.root().valid()) continue;
        ScopeId const treeRoot = s.scopes.pushScope(cuRoot, tree.root(), tree.id());
        treeRootScope[tree.id().v] = treeRoot;
        pass1(s, cfg, tree, tree.root(), treeRoot);
    }

    // Cross-tree symbol visibility — runs AFTER every tree's Pass 1 so
    // all target symbols exist. CU4's ImportResolver records each edge as
    // source = the referencing/including tree, target = the
    // defining/included tree (true for BOTH the IncludeFollowing and
    // NameMatching strategies in import_resolver.cpp). To make the
    // referencing file see the defining file's symbols we inject every
    // binding of the TARGET tree's root scope into the SOURCE tree's
    // root scope.
    for (auto const& edge : cu->crossRefs()) {
        auto srcIt = treeRootScope.find(edge.sourceTree.v);
        auto tgtIt = treeRootScope.find(edge.targetTree.v);
        if (srcIt == treeRootScope.end() || tgtIt == treeRootScope.end()) continue;
        for (auto const& [name, sym] : s.scopes.bindingsOf(tgtIt->second)) {
            s.scopes.injectBinding(srcIt->second, std::string{name}, sym);
        }
    }

    // Pass 1.5 per tree: resolve declaration types.
    for (auto const& tree : trees) {
        if (!tree.root().valid()) continue;
        resolveDeclTypes(s, cfg, tree, tree.root());
    }

    // Pass 2 per tree, against that tree's root scope.
    for (auto const& tree : trees) {
        if (!tree.root().valid()) continue;
        pass2(s, cfg, tree, tree.root(), treeRootScope.at(tree.id().v));
    }

    return SemanticModel{
        std::move(cu),
        std::move(s.lattice),
        std::move(s.scopes).release(),
        std::move(s.symbols).release(),
        std::move(s.nodeToSymbol),
        std::move(s.nodeToType),
        std::move(s.reporter),
    };
}

} // namespace dss
