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

#include <algorithm>
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
//     mint SymbolIds, bind names in the scope tree, mark const symbols
//     (SE4), emit S_RedeclaredSymbol on same-scope duplicates. Forward
//     references (G-209) fall out because all decls are minted before any
//     use is resolved.
//
//   Pass 1.5 (post-order, per tree): resolve declaration types from
//     their `type` subtree per `typeShapes`/`builtinTypes`. Function-kind
//     symbols (SE6) build a FnSig over their resolved param/return types.
//     Type-position resolution also consults type-alias symbols (SE5).
//
//   Pass 2 (post-order, per tree): resolve identifier USES (the
//     `references` rules) against the scope chain, propagate symbol
//     types to use sites, type literal leaves per `literalTypes`, check
//     assignments against const symbols (SE4), check calls against
//     callee signatures (SE6), and emit S_* diagnostics on misses.
//     Initializer-based type inference fills in untyped declarations.
//
// Per-tree-root scopes: each tree gets its OWN root scope (a child of
// the shared CU builtins scope). Pass 1 declares a tree's top-level
// symbols into THAT tree's root scope.
//
// Built-in functions (SE6): a CU-wide "builtins" scope is the parent of
// every tree's root scope, so config-declared builtin functions (e.g.
// COALESCE) are visible everywhere yet user declarations shadow them.
//
// The engine NEVER branches on schema.name(): every per-language
// behaviour is sourced from the SemanticConfig (+ grammar shapes).

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
    std::unordered_map<std::uint32_t, std::size_t> assignByRule;
    std::unordered_map<std::uint32_t, std::size_t> callByRule;
    // Built-in type name → TypeId (interned once per CU).
    std::unordered_map<std::string, TypeId>        builtinTypeIds;
    // Literal token-kind → TypeId.
    std::unordered_map<std::uint32_t, TypeId>      literalTypeIds;
    // SE7 reverse use-index: SymbolId.v → its use-site NodeIds.
    std::unordered_map<std::uint32_t, std::vector<NodeId>> usesBySymbol;

    [[nodiscard]] TypeId typeAt(NodeId id) const {
        auto const* p = nodeToType.tryGet(id);
        return p ? *p : InvalidType;
    }

    // The SymbolId bound to a name node, or InvalidSymbol.
    [[nodiscard]] SymbolId symbolAtOr(NodeId id) const {
        auto const* p = nodeToSymbol.tryGet(id);
        return p ? *p : InvalidSymbol;
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

// Follow a path of visible-child indices from `start`. Returns
// InvalidNode if any step indexes out of range. Used by the kindByChild
// facet to resolve params/body paths through a discriminator sub-rule
// (e.g. c-subset's `funcDefTail → [funcParams, block]`).
[[nodiscard]] NodeId
descendVisible(Tree const& tree, NodeId start,
               std::vector<std::uint32_t> const& path) {
    NodeId cur = start;
    for (auto idx : path) {
        if (!cur.valid()) return {};
        auto kids = visibleChildren(tree, cur);
        if (idx >= kids.size()) return {};
        cur = kids[idx];
    }
    return cur;
}

// Forward declarations (these helpers are referenced by the passes
// before their definitions appear). `CallRule` comes from
// semantic_config.hpp (dss namespace).
void collectParamTypes(EngineState& s, SemanticConfig const& cfg,
                       Tree const& tree, NodeId cur, ScopeId scope,
                       std::vector<TypeId>& out);
void checkCall(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
               NodeId node, ScopeId scope, CallRule const& call);
void gatherArgExpressions(Tree const& tree, NodeId argsNode,
                          std::vector<NodeId>& out);

// True iff a token of kind `kind` appears anywhere in `node`'s subtree.
// Used by SE4 const-marker detection (walk the type subtree for the
// language's const keyword).
[[nodiscard]] bool
subtreeContainsToken(Tree const& tree, NodeId node, SchemaTokenId kind) {
    if (!node.valid() || !kind.valid()) return false;
    std::vector<NodeId> stack{node};
    while (!stack.empty()) {
        NodeId cur = stack.back();
        stack.pop_back();
        if (tree.kind(cur) == NodeKind::Token && tree.tokenKind(cur) == kind) {
            return true;
        }
        for (auto const& child : tree.children(cur)) {
            if (!isEmptySpace(tree.flags(child))) stack.push_back(child);
        }
    }
    return false;
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

// Gather the argument-expression nodes of a call's args subtree, in
// source order. The args list is a comma-separated sequence of
// expressions; its DIRECT visible children alternate expression / comma /
// expression — so the argument nodes are exactly the Internal (rule)
// children (the commas are Token children, skipped). A single argument
// that happens to be a bare identifier still parses to a wrapper rule
// node (expression/operand), so counting Internal children is correct for
// any arity including one. SE6.
void gatherArgExpressions(Tree const& tree, NodeId argsNode,
                          std::vector<NodeId>& out) {
    if (!argsNode.valid()) return;
    for (auto kid : visibleChildren(tree, argsNode)) {
        if (tree.kind(kid) == NodeKind::Internal) out.push_back(kid);
    }
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
    for (std::size_t i = 0; i < cfg.assignments.size(); ++i) {
        s.assignByRule[cfg.assignments[i].rule.v] = i;
    }
    for (std::size_t i = 0; i < cfg.callRules.size(); ++i) {
        s.callByRule[cfg.callRules[i].rule.v] = i;
    }
    for (auto const& bt : cfg.builtinTypes) {
        s.builtinTypeIds[bt.name] = s.lattice.interner().primitive(bt.core);
    }
    for (auto const& lt : cfg.literalTypes) {
        s.literalTypeIds[lt.literal.v] = s.lattice.interner().primitive(lt.core);
    }
}

// Resolve a type-position subtree to a TypeId. Walks `typeShapes`
// recursively (e.g. pointerType[innerType] → Ptr<innerType>) and looks
// the leaf up in `builtinTypes`. A leaf that is not a built-in type but
// resolves (via scope-chain lookup from `scope`) to a `Type`-kind symbol
// with a valid type yields that aliased type (SE5 typedef resolution).
// Emits `S_UnknownType` when no matching mapping was found in the
// subtree's leaf.
[[nodiscard]] TypeId
resolveTypeNode(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                NodeId node, ScopeId scope, bool emitOnMiss = true) {
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
            TypeId inner = resolveTypeNode(s, cfg, tree, kids[shape.operandChild],
                                           scope, emitOnMiss);
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
            auto inner = resolveTypeNode(s, cfg, tree, child, scope, /*emitOnMiss=*/false);
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
        // SE5: a non-builtin name in type position may be a type alias.
        // Resolve it through the scope chain; a Type-kind symbol with a
        // valid type contributes the aliased type.
        if (scope.valid()) {
            SymbolId const aliasSym = s.scopes.lookup(scope, text);
            if (aliasSym.valid()) {
                auto const& rec = s.symbols.at(aliasSym);
                if (rec.kind == DeclarationKind::Type && rec.type.valid()) {
                    return rec.type;
                }
            }
        }
        return InvalidType;
    }
    return InvalidType;
}

// ── Pass 1: pre-order — mint decls + push/pop scopes + const marking ───────
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

            // Evaluate the kindByChild discriminator (if any) BEFORE
            // minting the symbol so its `kind` reflects the structural
            // choice in the tree (e.g. c-subset's topLevelDecl with a
            // funcDefTail descendant mints a Function, not a Variable).
            DeclarationKind effectiveKind = decl.kind;
            if (decl.kindByChild.has_value()) {
                auto const& disc = *decl.kindByChild;
                NodeId const disChild = descendVisible(tree, node, disc.childPath);
                if (disChild.valid()
                    && tree.kind(disChild) == NodeKind::Internal
                    && tree.rule(disChild) == disc.whenRule) {
                    effectiveKind = disc.whenKind;
                }
            }

            if (decl.nameChild.has_value() && *decl.nameChild < kids.size()) {
                NodeId const nameContainer = kids[*decl.nameChild];
                auto resolved = extractNameNode(
                    tree, nameContainer, decl.nameMatch, cfg.identifierToken);
                if (!resolved.name.empty() && resolved.node.valid()) {
                    // A declaration's NAME binds into the ENCLOSING scope
                    // (`current`), NOT the scope this rule itself opens
                    // (`here`). For a rule that is both a declaration AND a
                    // scope opener (e.g. a function whose body is its own
                    // scope), this keeps the function name visible to its
                    // siblings/callers while its params/locals live inside.
                    ScopeId const bindScope = current;
                    SymbolRecord rec;
                    rec.name         = resolved.name;
                    rec.scope        = bindScope;
                    rec.declNode     = resolved.node;
                    rec.declRuleNode = node;
                    rec.tree         = tree.id();
                    rec.kind         = effectiveKind;

                    // SE4: const-marking. Scan the type subtree (or the
                    // whole decl subtree when no typeChild) for the
                    // language's const-marker token.
                    if (decl.constMarker.has_value()) {
                        NodeId scanRoot = node;
                        if (decl.typeChild.has_value()
                            && *decl.typeChild < kids.size()) {
                            scanRoot = kids[*decl.typeChild];
                        }
                        rec.isConst = subtreeContainsToken(
                            tree, scanRoot, *decl.constMarker);
                    }

                    SymbolId const newId = s.symbols.mint(rec);
                    SymbolId const prior = s.scopes.bind(bindScope, resolved.name, newId);
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

// Locate the child scope minted for an in-scope rule node (mirrors
// Pass 1's push/pop without re-walking). Returns `current` if none found.
[[nodiscard]] ScopeId
childScopeFor(EngineState& s, Tree const& tree, NodeId node, ScopeId current) {
    for (auto child : s.scopes.scopes()[current.v].children) {
        auto const& rec = s.scopes.scopes()[child.v];
        if (rec.anchor.v == node.v && rec.tree.v == tree.id().v) {
            return child;
        }
    }
    return current;
}

// ── Pass 1.5: post-order — resolve declaration types + FnSigs ──────────────
void resolveDeclTypes(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                      NodeId node, ScopeId current) {
    if (!node.valid()) return;
    if (isEmptySpace(tree.flags(node))) return;

    ScopeId here = current;
    if (tree.kind(node) == NodeKind::Internal
        && s.scopeByRule.contains(tree.rule(node).v)) {
        here = childScopeFor(s, tree, node, current);
    }

    for (auto const& child : tree.children(node)) {
        resolveDeclTypes(s, cfg, tree, child, here);
    }

    if (tree.kind(node) == NodeKind::Internal) {
        auto const rule = tree.rule(node);
        auto declIt = s.declByRule.find(rule.v);
        if (declIt != s.declByRule.end()) {
            auto const& decl = cfg.declarations[declIt->second];
            auto kids = visibleChildren(tree, node);
            if (decl.nameChild.has_value() && *decl.nameChild < kids.size()) {
                auto resolved = extractNameNode(
                    tree, kids[*decl.nameChild], decl.nameMatch, cfg.identifierToken);
                SymbolId sym = resolved.node.valid()
                    ? s.symbolAtOr(resolved.node) : InvalidSymbol;
                if (sym.valid()) {
                    // Resolve return / declared type from typeChild.
                    TypeId returnTy = InvalidType;
                    if (decl.typeChild.has_value() && *decl.typeChild < kids.size()) {
                        returnTy = resolveTypeNode(
                            s, cfg, tree, kids[*decl.typeChild], here);
                    }

                    // Re-evaluate the kindByChild discriminator (if any) to
                    // resolve the effective kind + params/body nodes via
                    // the configured paths. Mirrors pass1's evaluation; the
                    // engine does NOT cache it on the symbol because the
                    // tree itself is the source of truth (config-driven).
                    DeclarationKind effectiveKind = s.symbols.at(sym).kind;
                    NodeId paramsNode{};
                    NodeId bodyNode{};
                    bool discMatched = false;
                    if (decl.kindByChild.has_value()) {
                        auto const& disc = *decl.kindByChild;
                        NodeId const disChild =
                            descendVisible(tree, node, disc.childPath);
                        if (disChild.valid()
                            && tree.kind(disChild) == NodeKind::Internal
                            && tree.rule(disChild) == disc.whenRule) {
                            discMatched   = true;
                            effectiveKind = disc.whenKind;
                            if (!disc.paramsPath.empty()) {
                                paramsNode = descendVisible(
                                    tree, disChild, disc.paramsPath);
                            }
                            if (!disc.bodyPath.empty()) {
                                bodyNode = descendVisible(
                                    tree, disChild, disc.bodyPath);
                            }
                        }
                    }
                    // Fall back to the static paramsChild/bodyChild slots
                    // when no discriminator-resolved nodes are available.
                    if (!discMatched) {
                        if (decl.paramsChild.has_value()
                            && *decl.paramsChild < kids.size()) {
                            paramsNode = kids[*decl.paramsChild];
                        }
                        if (decl.bodyChild.has_value()
                            && *decl.bodyChild < kids.size()) {
                            bodyNode = kids[*decl.bodyChild];
                        }
                    }
                    // The body node is intentionally resolved but currently
                    // unused beyond signaling Function-ness (scope opening
                    // is handled by the `scopes` facet on the body rule
                    // itself, e.g. c-subset declares `funcDefTail` as a
                    // scope rule so the params bind there).
                    (void)bodyNode;

                    if (effectiveKind == DeclarationKind::Function) {
                        // SE6: build a FnSig over the param types.
                        std::vector<TypeId> paramTypes;
                        if (paramsNode.valid()) {
                            collectParamTypes(s, cfg, tree, paramsNode, here,
                                              paramTypes);
                        }
                        TypeId const fnTy = s.lattice.interner().fnSig(
                            paramTypes, returnTy, CallConv::CcSysV);
                        s.symbols.at(sym).type = fnTy;
                        s.nodeToType.set(resolved.node, fnTy);
                    } else if (returnTy.valid()) {
                        s.symbols.at(sym).type = returnTy;
                        s.nodeToType.set(resolved.node, returnTy);
                    }
                }
            }
        }
    }
}

// ── Pass 2: post-order — resolve uses + literal/init typing + checks ───────
void pass2(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
           NodeId node, ScopeId current) {
    if (!node.valid()) return;
    if (isEmptySpace(tree.flags(node))) return;
    auto const k = tree.kind(node);
    ScopeId here = current;

    if (k == NodeKind::Internal && s.scopeByRule.contains(tree.rule(node).v)) {
        here = childScopeFor(s, tree, node, current);
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
                        s.usesBySymbol[found.v].push_back(resolved.node);
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
                    SymbolId sym = s.symbolAtOr(resolved.node);
                    if (sym.valid()) {
                        auto& rec = s.symbols.at(sym);
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

    // SE4: const-violation check on assignment rules.
    if (k == NodeKind::Internal) {
        auto const rule = tree.rule(node);
        auto assignIt = s.assignByRule.find(rule.v);
        if (assignIt != s.assignByRule.end()) {
            auto const& assign = cfg.assignments[assignIt->second];
            auto kids = visibleChildren(tree, node);
            // The operatorToken (when set) gates the match: a binaryExpr
            // reused for every operator only counts as an assignment when
            // one of its visible children IS that operator token.
            bool gated = true;
            if (assign.operatorToken.has_value()) {
                gated = false;
                for (auto kid : kids) {
                    if (tree.kind(kid) == NodeKind::Token
                        && tree.tokenKind(kid) == *assign.operatorToken) {
                        gated = true;
                        break;
                    }
                }
            }
            if (gated && assign.lhsChild < kids.size()) {
                NodeId lhsNode = kids[assign.lhsChild];
                auto resolved = extractNameNode(
                    tree, lhsNode, NameMatchMode::Self, cfg.identifierToken);
                if (resolved.node.valid()
                    && tree.kind(resolved.node) == NodeKind::Token
                    && cfg.identifierToken.valid()
                    && tree.tokenKind(resolved.node) == cfg.identifierToken) {
                    SymbolId const lhsSym = s.scopes.lookup(here, resolved.name);
                    if (lhsSym.valid() && s.symbols.at(lhsSym).isConst) {
                        ParseDiagnostic d;
                        d.code     = DiagnosticCode::S_ConstViolation;
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

    // SE6: call checking.
    if (k == NodeKind::Internal) {
        auto const rule = tree.rule(node);
        auto callIt = s.callByRule.find(rule.v);
        if (callIt != s.callByRule.end()) {
            checkCall(s, cfg, tree, node, here, cfg.callRules[callIt->second]);
        }
    }
}

// Collect parameter types from a params subtree, in source (left-to-right)
// order, by recursively walking it for declaration rules and resolving
// each one's typeChild. A param-decl node is NOT descended into (its own
// children are the param's type/name, not nested params). SE6.
void collectParamTypes(EngineState& s, SemanticConfig const& cfg,
                       Tree const& tree, NodeId cur, ScopeId scope,
                       std::vector<TypeId>& out) {
    if (!cur.valid() || isEmptySpace(tree.flags(cur))) return;
    if (tree.kind(cur) == NodeKind::Internal) {
        auto declIt = s.declByRule.find(tree.rule(cur).v);
        if (declIt != s.declByRule.end()) {
            auto const& decl = cfg.declarations[declIt->second];
            auto kids = visibleChildren(tree, cur);
            if (decl.typeChild.has_value() && *decl.typeChild < kids.size()) {
                out.push_back(resolveTypeNode(
                    s, cfg, tree, kids[*decl.typeChild], scope));
            }
            return;  // a param decl is a leaf for parameter-collection
        }
    }
    for (auto const& child : tree.children(cur)) {
        collectParamTypes(s, cfg, tree, child, scope, out);
    }
}

// SE6: verify a call node against the callee's signature. Resolves the
// callee child to a symbol; emits S_NotCallable / S_ArgCountMismatch /
// S_TypeMismatch / S_UndeclaredIdentifier as appropriate.
void checkCall(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
               NodeId node, ScopeId scope, CallRule const& call) {
    auto kids = visibleChildren(tree, node);

    // operatorToken gate (when set): the rule only counts as a call when
    // one of its visible children is a token of that kind. Lets a single
    // postfix-expression shape cover both call and non-call forms (e.g.
    // c-subset's `postfixExpr` covers `f(...)`, `a[i]`, AND `i++`).
    if (call.operatorToken.has_value()) {
        bool gated = false;
        for (auto kid : kids) {
            if (tree.kind(kid) == NodeKind::Token
                && tree.tokenKind(kid) == *call.operatorToken) {
                gated = true;
                break;
            }
        }
        if (!gated) return;
    }

    if (call.calleeChild >= kids.size()) return;
    NodeId calleeNode = kids[call.calleeChild];

    auto resolved = extractNameNode(
        tree, calleeNode, NameMatchMode::Self, cfg.identifierToken);
    if (!resolved.node.valid()
        || tree.kind(resolved.node) != NodeKind::Token
        || !cfg.identifierToken.valid()
        || tree.tokenKind(resolved.node) != cfg.identifierToken) {
        return;  // not a simple identifier callee — out of v1 scope
    }
    SymbolId const calleeSym = s.scopes.lookup(scope, resolved.name);
    if (!calleeSym.valid()) {
        // The callee is a bare Identifier; the reference-rules path may
        // not cover this position (e.g. tsql's `callExpr` callee is the
        // raw `Identifier` token, NOT a `qualifiedName`). Emit on the
        // callee span so the unresolved call is loud, with the callee
        // text as `actual`.
        //
        // BUT: if a `references` rule structurally COVERS the callee
        // subtree (e.g. c-subset's `operand` is both the references-rule
        // and the postfixExpr's callee child), Pass 2's ref-rule visit
        // of that very same subtree has ALREADY emitted the
        // S_UndeclaredIdentifier for this identifier. Emitting again
        // here would double-fire on the wire (the reporter's sliding-
        // window dedup currently hides it, but the architecture should
        // not rely on noise filtering to hide a structural duplicate).
        //
        // Detection: walk DOWN from `calleeNode` through single-child
        // Internal wrappers (mirroring extractNameNode's Self walk) and
        // check whether ANY visited node's rule is in `refByRule`. If
        // so, the ref path owns the diagnostic; we skip emit.
        {
            NodeId probe = calleeNode;
            while (probe.valid()
                   && tree.kind(probe) == NodeKind::Internal) {
                if (s.refByRule.contains(tree.rule(probe).v)) {
                    return;  // ref-rule path already emitted (or will)
                }
                auto kids2 = visibleChildren(tree, probe);
                if (kids2.size() != 1) break;
                probe = kids2[0];
            }
        }
        ParseDiagnostic d;
        d.code     = DiagnosticCode::S_UndeclaredIdentifier;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(resolved.node);
        d.actual   = resolved.name;
        s.reporter.report(std::move(d));
        return;
    }
    TypeId const fnTy = s.symbols.at(calleeSym).type;

    if (!fnTy.valid() || s.lattice.interner().kind(fnTy) != TypeKind::FnSig) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::S_NotCallable;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(resolved.node);
        d.actual   = resolved.name;
        s.reporter.report(std::move(d));
        return;
    }

    // Collect arg expression nodes (visible children of the args subtree
    // that are NOT separators). The argsChild subtree is a comma-separated
    // list of `expression`s; count the non-token visible children.
    std::vector<NodeId> argNodes;
    if (call.argsChild < kids.size()) {
        gatherArgExpressions(tree, kids[call.argsChild], argNodes);
    }

    auto params = s.lattice.interner().fnParams(fnTy);

    bool const variadic = s.symbols.at(calleeSym).variadicBuiltin;
    if (!variadic && argNodes.size() != params.size()) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::S_ArgCountMismatch;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(node);
        d.actual   = std::to_string(argNodes.size());
        s.reporter.report(std::move(d));
        return;
    }

    // Per-arg assignability (only up to the declared arity for variadics).
    std::size_t const checkCount = variadic
        ? std::min(argNodes.size(), params.size())
        : params.size();
    for (std::size_t i = 0; i < checkCount && i < argNodes.size(); ++i) {
        TypeId argTy = s.typeAt(argNodes[i]);
        if (!argTy.valid()) continue;  // unknown arg type — suppress cascade
        if (!isAssignable(s.lattice.interner(), params[i], argTy)) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::S_TypeMismatch;
            d.severity = DiagnosticSeverity::Error;
            d.buffer   = tree.source().id();
            d.span     = tree.span(argNodes[i]);
            d.actual   = std::string{tree.text(argNodes[i])};
            s.reporter.report(std::move(d));
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

    // SE6: a CU-wide builtins scope (child of the CU root, parent of every
    // tree root) holds config-declared builtin functions. Visible
    // everywhere; user declarations in a tree root shadow it.
    ScopeId const cuRoot = s.scopes.root();
    ScopeId const builtinScope = s.scopes.pushScope(cuRoot, InvalidNode, InvalidTree);
    for (auto const& bf : cfg.builtinFunctions) {
        std::vector<TypeId> paramTypes;
        paramTypes.reserve(bf.paramCores.size());
        for (auto pc : bf.paramCores) {
            paramTypes.push_back(s.lattice.interner().primitive(pc));
        }
        TypeId const fnTy = s.lattice.interner().fnSig(
            paramTypes, s.lattice.interner().primitive(bf.resultCore),
            CallConv::CcSysV);
        SymbolRecord rec;
        rec.name            = bf.name;
        rec.scope           = builtinScope;
        rec.tree            = InvalidTree;
        rec.kind            = DeclarationKind::Function;
        rec.type            = fnTy;
        rec.variadicBuiltin = bf.variadic;
        SymbolId const id = s.symbols.mint(rec);
        s.scopes.injectBinding(builtinScope, bf.name, id);
    }

    // Each tree gets its OWN root scope (a child of the builtins scope) so
    // two unrelated files' top-level decls live in separate namespaces but
    // both still see the builtins.
    auto const trees = cu->trees();
    std::unordered_map<std::uint32_t /*TreeId.v*/, ScopeId> treeRootScope;

    // Pass 1 per tree, declaring into THAT tree's root scope.
    for (auto const& tree : trees) {
        if (!tree.root().valid()) continue;
        ScopeId const treeRoot = s.scopes.pushScope(builtinScope, tree.root(), tree.id());
        treeRootScope[tree.id().v] = treeRoot;
        pass1(s, cfg, tree, tree.root(), treeRoot);
    }

    // Cross-tree symbol visibility — runs AFTER every tree's Pass 1 so
    // all target symbols exist. CU4's ImportResolver records each edge as
    // source = the referencing/including tree, target = the
    // defining/included tree. To make the referencing file see the
    // defining file's symbols we inject every binding of the TARGET tree's
    // root scope into the SOURCE tree's root scope.
    for (auto const& edge : cu->crossRefs()) {
        auto srcIt = treeRootScope.find(edge.sourceTree.v);
        auto tgtIt = treeRootScope.find(edge.targetTree.v);
        if (srcIt == treeRootScope.end() || tgtIt == treeRootScope.end()) continue;
        for (auto const& [name, sym] : s.scopes.bindingsOf(tgtIt->second)) {
            s.scopes.injectBinding(srcIt->second, std::string{name}, sym);
        }
    }

    // Pass 1.5 per tree: resolve declaration types + function signatures.
    for (auto const& tree : trees) {
        if (!tree.root().valid()) continue;
        resolveDeclTypes(s, cfg, tree, tree.root(), treeRootScope.at(tree.id().v));
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
        std::move(s.usesBySymbol),
    };
}

} // namespace dss
