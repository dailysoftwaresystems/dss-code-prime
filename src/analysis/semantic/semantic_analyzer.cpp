#include "analysis/semantic/semantic_analyzer.hpp"

#include "analysis/compilation_unit/unit_attribute.hpp"
#include "analysis/semantic/scope_tree.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/symbol_table.hpp"
#include "analysis/semantic/type_rules.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/number_decode.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/string_style.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/type_lattice/type_lattice.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
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

// HR11: the per-SCHEMA index caches + config pointers. A `RuleId`/`SchemaTokenId`
// is only meaningful within its own schema, so these maps (keyed by such ids)
// are built once PER distinct schema in the CU and selected per tree by
// `EngineState::activate(tree.schema())`. A homogeneous CU has exactly one
// bundle. No field here is CU-global; the CU-global state stays on EngineState.
struct SchemaIndexes {
    SemanticConfig const* cfg         = nullptr;  // the owning schema's semantics
    NumberStyle const*    numberStyle = nullptr;  // its numeric-literal style (may be null)
    std::unordered_map<std::uint32_t, std::size_t> declByRule;
    std::unordered_map<std::uint32_t, std::size_t> refByRule;
    // D5.1: member-access rules (`obj.field` and `ptr->field`). Multi-entry
    // per rule when multiple shapes share the same rule (e.g. c-subset's
    // single `postfixExpr` covers BOTH `.` and `->`); each is distinguished
    // by `MemberAccessRule.operatorToken`, just like `assignByRule`.
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> memberAccessByRule;
    std::unordered_map<std::uint32_t, std::size_t> typeShapeByRule;
    std::unordered_map<std::uint32_t, bool>        scopeByRule;
    // Multiple assignment entries may share one rule (e.g. c-subset's
    // operator-table `binaryExpr` reused for `=` AND every compound-assign
    // operator `+=`/`<<=`/…). Each entry is distinguished by its
    // `operatorToken`, so the index maps a rule to ALL its entries and the
    // check picks the one whose operator token is present in the node.
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> assignByRule;
    std::unordered_map<std::uint32_t, std::size_t> callByRule;
    std::unordered_map<std::uint32_t, std::size_t> returnByRule;   // GAP A
    std::unordered_map<std::uint32_t, bool>        loopByRule;      // GAP C loop contexts
    std::unordered_map<std::uint32_t, bool>        loopControlByRule; // GAP C break/continue
    // Built-in type name → TypeId (interned once per schema, into the CU lattice).
    std::unordered_map<std::string, TypeId>        builtinTypeIds;
    // Literal token-kind → TypeId.
    std::unordered_map<std::uint32_t, TypeId>      literalTypeIds;
};

// One transient per `analyze()` call. Consumed into the returned model.
struct EngineState {
    explicit EngineState(CompilationUnit const& cu)
        : lattice{cu.id(), cu.compositeSourceLanguage()},
          nodeToSymbol{cu},
          nodeToType{cu} {}

    DiagnosticReporter         reporter;
    TypeLattice                lattice;
    ScopeTree                  scopes;
    SymbolTable                symbols;
    UnitAttribute<SymbolId>    nodeToSymbol;
    UnitAttribute<TypeId>      nodeToType;
    // HR11: per-schema index bundles, keyed by SchemaId.v; `active_` is the
    // bundle for the tree currently being processed (set via `activate`).
    std::unordered_map<std::uint32_t, SchemaIndexes> schemaIndexes;
    SchemaIndexes const*                             active_ = nullptr;
    // GAP A: scope opened by a function's BODY → that function's result
    // type. A `return` statement walks its scope parent chain to find the
    // nearest enclosing function result type for assignability checking.
    std::unordered_map<std::uint32_t /*ScopeId.v*/, TypeId> fnResultByScope;
    // SE7 reverse use-index: SymbolId.v → its use-site NodeIds.
    std::unordered_map<std::uint32_t, std::vector<NodeId>> usesBySymbol;
    // D5.1: composite type → the inner scope holding its fields, populated in
    // Pass 1.5 when a `fieldChildren`-bearing decl composes its struct type.
    // Pass 2's member-access resolution reads this to find the field-name's
    // scope from the LHS expression's TypeId (no type-name string lookup).
    std::unordered_map<std::uint32_t /*TypeId.v*/, ScopeId> compositeScopeByType;

    // Select the index bundle for `schema` before processing one of its trees.
    void activate(GrammarSchema const& schema) {
        active_ = &schemaIndexes.at(schema.schemaId().v);
    }
    // The active per-schema bundle. Valid only during a per-tree pass (after
    // `activate`); the passes never cross a tree boundary mid-walk. Asserts
    // rather than silently null-deref if a caller reads it without activating.
    [[nodiscard]] SchemaIndexes const& idx() const {
        if (active_ == nullptr) {
            std::fputs("dss::analyze fatal: idx() read before activate() — a "
                       "per-schema index was used outside a per-tree pass\n", stderr);
            std::abort();
        }
        return *active_;
    }

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
void checkReturn(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                 NodeId node, ScopeId scope, ReturnRule const& ret);
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

// GAP D: read the inner text of a bracket-quoted identifier opener (tsql's
// `[Orders]` → "Orders", `[a]]b]` → "a]b"). The opener token
// (`BracketIdStart`) spans only the `[`; the body bytes are off-grammar
// default-mode tokens, so the content is read from the source slice. The
// `]]` doubled-delimiter un-escaping (matching the tokenizer's
// `EscapeKind::DoubledDelimiter` rule) lives in the shared `bracketInnerText`
// helper, which the import resolver also calls — keeping both decoders
// byte-identical with the tokenizer. Returns empty on a malformed `[...]`.
[[nodiscard]] std::string
bracketIdText(Tree const& tree, NodeId openerNode) {
    return bracketInnerText(tree.source().text(), tree.span(openerNode).start());
}

// True when `node` is a name-bearing leaf per the config: either the plain
// identifier token (`idKind`) or — when set — the bracket-id opener
// (`bracketKind`). Out param `text` receives the resolved name (bracketed
// text stripped of its brackets for the bracket-id case).
[[nodiscard]] bool
nameLeafText(Tree const& tree, NodeId node, SchemaTokenId idKind,
             std::optional<SchemaTokenId> bracketKind, std::string& text) {
    if (tree.kind(node) != NodeKind::Token) return false;
    auto const tk = tree.tokenKind(node);
    if (idKind.valid() && tk == idKind) {
        text = std::string{tree.text(node)};
        return true;
    }
    if (bracketKind.has_value() && bracketKind->valid() && tk == *bracketKind) {
        text = bracketIdText(tree, node);
        return !text.empty();
    }
    return false;
}

[[nodiscard]] ResolvedName
extractNameNode(Tree const& tree, NodeId node, NameMatchMode mode,
                SchemaTokenId idKind,
                std::optional<SchemaTokenId> bracketKind = std::nullopt) {
    if (!node.valid()) return {};
    if (mode == NameMatchMode::Self) {
        if (tree.kind(node) == NodeKind::Token) {
            return {std::string{tree.text(node)}, node};
        }
        auto kids = visibleChildren(tree, node);
        if (kids.size() == 1) {
            // Recurse one level so an `expression[Identifier]` wrapper
            // bottoms out at the identifier token (not the wrapper).
            return extractNameNode(tree, kids[0], NameMatchMode::Self, idKind,
                                   bracketKind);
        }
        return {std::string{tree.text(node)}, node};
    }
    // LastIdentifier — DFS for the last name-bearing leaf (the kind the
    // schema's `semantics.identifierToken` names — OR, when configured, a
    // `bracketIdentifierToken` leaf; both resolved by the loader).
    NodeId found{};
    std::string foundText;
    std::vector<NodeId> stack{node};
    while (!stack.empty()) {
        NodeId cur = stack.back();
        stack.pop_back();
        if (std::string leaf; nameLeafText(tree, cur, idKind, bracketKind, leaf)) {
            found     = cur;
            foundText = std::move(leaf);
        }
        auto kids = tree.children(cur);
        // Reverse-push so visit order is left-to-right.
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            if (!isEmptySpace(tree.flags(*it))) stack.push_back(*it);
        }
    }
    if (found.valid()) return {std::move(foundText), found};
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

// Fill `idx` from `cfg` (the owning schema's semantics). Interns the schema's
// builtin types into the shared CU lattice (idempotent). Called once per
// distinct schema in the CU (HR11).
void buildIndexes(EngineState& s, SchemaIndexes& idx, SemanticConfig const& cfg) {
    idx.cfg = &cfg;
    for (std::size_t i = 0; i < cfg.declarations.size(); ++i) {
        idx.declByRule[cfg.declarations[i].rule.v] = i;
    }
    for (std::size_t i = 0; i < cfg.references.size(); ++i) {
        idx.refByRule[cfg.references[i].rule.v] = i;
    }
    for (std::size_t i = 0; i < cfg.memberAccesses.size(); ++i) {
        idx.memberAccessByRule[cfg.memberAccesses[i].rule.v].push_back(i);
    }
    for (std::size_t i = 0; i < cfg.typeShapes.size(); ++i) {
        idx.typeShapeByRule[cfg.typeShapes[i].rule.v] = i;
    }
    for (auto const& sc : cfg.scopes) idx.scopeByRule[sc.rule.v] = true;
    for (std::size_t i = 0; i < cfg.assignments.size(); ++i) {
        idx.assignByRule[cfg.assignments[i].rule.v].push_back(i);
    }
    for (std::size_t i = 0; i < cfg.callRules.size(); ++i) {
        idx.callByRule[cfg.callRules[i].rule.v] = i;
    }
    for (std::size_t i = 0; i < cfg.returnRules.size(); ++i) {
        idx.returnByRule[cfg.returnRules[i].rule.v] = i;
    }
    for (auto const& lr : cfg.loopRules)    idx.loopByRule[lr.rule.v] = true;
    for (auto const& lc : cfg.loopControls) idx.loopControlByRule[lc.rule.v] = true;
    for (auto const& bt : cfg.builtinTypes) {
        if (bt.extension.has_value()) {
            // The mapping names a registered type-extension (e.g. T-SQL's
            // VARCHAR → "TSQL::Varchar"). Extensions are registered into the
            // CU registry BEFORE buildIndexes runs (see analyze()), so the
            // kindId is available here. The extension type is built with no
            // type/scalar args — the parameterized form (e.g. Varchar<N>) is
            // resolved later if/when the grammar parses the parameter; a bare
            // type-position name resolves to the un-parameterized extension so
            // the type position does not spuriously emit S_UnknownType.
            auto kindId = s.lattice.registry().findExtension(*bt.extension);
            if (!kindId) {
                // Loader invariant: a builtinType naming an `extension` is
                // validated against typeExtensions[] at load
                // (C_UnknownTypeExtension), and registerSchemaTypeExtensions
                // runs before buildIndexes (see analyze()). An unregistered
                // extension here means that invariant was violated — fail loud
                // rather than silently degrade the type to S_UnknownType.
                std::fputs("dss::analyze fatal: builtinType names an "
                           "unregistered type-extension (loader invariant "
                           "violated)\n", stderr);
                std::abort();
            }
            idx.builtinTypeIds[bt.name] =
                s.lattice.interner().extension(*kindId, *bt.extension, {});
            continue;
        }
        idx.builtinTypeIds[bt.name] = s.lattice.interner().primitive(bt.core);
    }
    for (auto const& lt : cfg.literalTypes) {
        idx.literalTypeIds[lt.literal.v] = s.lattice.interner().primitive(lt.core);
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
        auto it = s.idx().typeShapeByRule.find(rule.v);
        if (it != s.idx().typeShapeByRule.end()) {
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
        // SE-pointers (G5): count `pointerToken` children at THIS node (C
        // declarator stars: `int *p`, `int **p`) and wrap the resolved base type
        // that many times in Ptr. The stars are a flat token run in `typeRef`;
        // the base type comes from the non-star child (typeBase).
        std::uint32_t ptrDepth = 0;
        TypeId inner = InvalidType;
        for (auto child : kids) {
            if (cfg.pointerToken.has_value()
                && tree.kind(child) == NodeKind::Token
                && tree.tokenKind(child) == *cfg.pointerToken) {
                ++ptrDepth;
                continue;
            }
            if (!inner.valid()) {
                auto t = resolveTypeNode(s, cfg, tree, child, scope, /*emitOnMiss=*/false);
                if (t.valid()) inner = t;
            }
        }
        if (inner.valid()) {
            for (std::uint32_t i = 0; i < ptrDepth; ++i)
                inner = s.lattice.interner().pointer(inner);
            return inner;
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
        auto it = s.idx().builtinTypeIds.find(text);
        if (it != s.idx().builtinTypeIds.end()) return it->second;
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

// True for the core integer kinds (signed + unsigned). Array lengths must be
// an integer literal; a float/char literal in length position is rejected.
[[nodiscard]] constexpr bool isIntegerKind(TypeKind k) {
    return k >= TypeKind::I8 && k <= TypeKind::U128;
}

// SE-arrays: evaluate a declarator-suffix length expression to a compile-time
// constant. Succeeds ONLY when the expression peels to a single integer
// literal — a wrapper rule (expression/operand) or a parenthesized literal is
// fine, but any branching (a binary op, a call, an identifier) is non-constant
// and returns nullopt so the caller fails loud. decodeInteger overflow also
// surfaces as nullopt.
[[nodiscard]] std::optional<std::uint64_t>
constIntLength(EngineState& s, Tree const& tree, NodeId node) {
    NodeId cur = node;
    for (int guard = 0; guard < 64 && cur.valid(); ++guard) {
        if (tree.kind(cur) == NodeKind::Token) {
            auto it = s.idx().literalTypeIds.find(tree.tokenKind(cur).v);
            if (it == s.idx().literalTypeIds.end()) return std::nullopt;
            if (!isIntegerKind(s.lattice.interner().kind(it->second))) return std::nullopt;
            return decodeInteger(tree.text(cur), s.idx().numberStyle);
        }
        // Count meaningful children: exactly one internal child ⇒ a wrapper
        // (expression → operand, operand → `( expr )`) to descend through;
        // zero internal + one literal token ⇒ the leaf literal.
        NodeId onlyInternal{};
        int internalCount = 0;
        NodeId onlyLiteral{};
        int literalCount = 0;
        for (NodeId c : visibleChildren(tree, cur)) {
            if (tree.kind(c) == NodeKind::Internal) { ++internalCount; onlyInternal = c; }
            else if (s.idx().literalTypeIds.count(tree.tokenKind(c).v) != 0) {
                ++literalCount; onlyLiteral = c;
            }
        }
        if (internalCount == 1) { cur = onlyInternal; continue; }
        if (internalCount == 0 && literalCount == 1) { cur = onlyLiteral; continue; }
        return std::nullopt;  // branching / no literal ⇒ non-constant
    }
    return std::nullopt;
}

// SE-arrays: if `decl` configures an array declarator suffix and a node of that
// suffix rule appears among the declaration's visible children, wrap `base` as
// Array<base, length>. A present-but-non-constant (or absent) length emits
// S_NonConstantArrayLength and returns InvalidType so the symbol's type stays
// unresolved (downstream fails loud rather than silently using the element
// type or decaying to a pointer). No suffix present ⇒ returns `base` unchanged.
[[nodiscard]] TypeId
applyArraySuffix(EngineState& s, Tree const& tree, DeclarationRule const& decl,
                 NodeId declNode, TypeId base) {
    if (!decl.arraySuffix.has_value() || !base.valid()) return base;
    ArraySuffix const& as = *decl.arraySuffix;
    // Bounded descendant search for the suffix rule. The suffix is a direct
    // child for a local/param declarator (`varDeclHead`, `param`) but nested
    // under a tail rule for a global (`topLevelDecl` → `varDeclTail`), so a
    // descendant scan handles every form uniformly. It cannot false-match an
    // array SUBSCRIPT — that is a distinct postfix `Index` rule, never the
    // declarator-suffix rule — so descending into an initializer is harmless.
    NodeId suffix{};
    std::vector<NodeId> stack{declNode};
    for (int guard = 0; guard < 8192 && !stack.empty(); ++guard) {
        NodeId c = stack.back(); stack.pop_back();
        if (tree.kind(c) != NodeKind::Internal) continue;
        if (tree.rule(c) == as.rule) { suffix = c; break; }
        for (NodeId g : visibleChildren(tree, c)) stack.push_back(g);
    }
    if (!suffix.valid()) return base;  // this declarator has no `[..]`

    auto emit = [&](DiagnosticCode code) {
        ParseDiagnostic d;
        d.code     = code;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(suffix);
        d.actual   = std::string{tree.text(suffix)};
        s.reporter.report(std::move(d));
    };

    NodeId lenNode{};
    if (as.lengthChild.has_value()) {
        auto sufKids = visibleChildren(tree, suffix);
        if (*as.lengthChild < sufKids.size())
            lenNode = sufKids[*as.lengthChild];
    }
    auto len = constIntLength(s, tree, lenNode);
    if (!len.has_value()) { emit(DiagnosticCode::S_NonConstantArrayLength); return InvalidType; }
    // A constant that decodes but exceeds the signed length the interner stores
    // (int64) must NOT wrap to a negative length — fail loud instead.
    if (*len > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        emit(DiagnosticCode::S_ArrayLengthOutOfRange);
        return InvalidType;
    }
    return s.lattice.interner().array(base, static_cast<std::int64_t>(*len));
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

        if (s.idx().scopeByRule.contains(rule.v)) {
            here = s.scopes.pushScope(current, node, tree.id());
        }

        auto declIt = s.idx().declByRule.find(rule.v);
        if (declIt != s.idx().declByRule.end()) {
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
                    tree, nameContainer, decl.nameMatch, cfg.identifierToken,
                    cfg.bracketIdentifierToken);
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
                    rec.warnIfUnused = decl.warnIfUnused;
                    // D5.1: stamp the declaration-order index of this symbol in
                    // its declaring scope. The scope's `bindings` count BEFORE
                    // we bind this symbol IS its 0-based ordinal. Meaningful for
                    // field symbols of a composite-type decl (`obj.field` index
                    // resolution); harmless positional metadata for everything
                    // else (Variable/Function/Type/Table outside a composite).
                    rec.fieldIndex = static_cast<std::uint32_t>(
                        s.scopes.scopes()[bindScope.v].bindings.size());
                    // D5.1: if THIS declaration introduces a composite type
                    // (carries `fieldChildren`), record the inner scope it just
                    // opened so Pass 2's member-access resolution can find the
                    // struct's fields. `here != current` iff this rule is in
                    // `scopeByRule` (which the loader/`fieldChildren` contract
                    // requires for any composite-type decl).
                    if (decl.fieldChildren.has_value() && here.v != current.v) {
                        rec.structScope = here;
                    }

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

// GAP A: find the scope (anywhere in the tree) whose anchor IS `node` —
// used to key a function's result type on the scope its body opens. The
// body scope can be nested under an intermediate scope (e.g. c-subset's
// `block` sits inside the `funcDefTail` scope), so a parent-children scan
// (as in `childScopeFor`) is insufficient; this scans all scopes for the
// matching anchor. Returns InvalidScope if none.
[[nodiscard]] ScopeId
scopeAnchoredAt(EngineState& s, Tree const& tree, NodeId node) {
    auto const& scopes = s.scopes.scopes();
    for (std::size_t i = 1; i < scopes.size(); ++i) {
        if (scopes[i].anchor.v == node.v && scopes[i].tree.v == tree.id().v) {
            return ScopeId{static_cast<std::uint32_t>(i)};
        }
    }
    return InvalidScope;
}

// ── Pass 1.5: post-order — resolve declaration types + FnSigs ──────────────
void resolveDeclTypes(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                      NodeId node, ScopeId current) {
    if (!node.valid()) return;
    if (isEmptySpace(tree.flags(node))) return;

    ScopeId here = current;
    if (tree.kind(node) == NodeKind::Internal
        && s.idx().scopeByRule.contains(tree.rule(node).v)) {
        here = childScopeFor(s, tree, node, current);
    }

    for (auto const& child : tree.children(node)) {
        resolveDeclTypes(s, cfg, tree, child, here);
    }

    if (tree.kind(node) == NodeKind::Internal) {
        auto const rule = tree.rule(node);
        auto declIt = s.idx().declByRule.find(rule.v);
        if (declIt != s.idx().declByRule.end()) {
            auto const& decl = cfg.declarations[declIt->second];
            auto kids = visibleChildren(tree, node);
            if (decl.nameChild.has_value() && *decl.nameChild < kids.size()) {
                auto resolved = extractNameNode(
                    tree, kids[*decl.nameChild], decl.nameMatch, cfg.identifierToken,
                    cfg.bracketIdentifierToken);
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
                    if (decl.fieldChildren.has_value()) {
                        // D5.1: compose the composite (struct/union) type from
                        // its field symbols. Post-order means every field's
                        // type is already resolved (each field is itself a
                        // declaration, processed earlier in the post-order
                        // walk). We iterate the struct's inner scope, collect
                        // (fieldIndex, type) pairs for symbols whose declaring
                        // rule matches `fieldChildren.rule`, sort by ordinal,
                        // and intern the structType. If any field type is
                        // unresolved the struct type stays InvalidType — the
                        // HIR verifier's requiresValidType then surfaces it
                        // through TypeDecl as H_TypeUnresolved (no silent gap).
                        SymbolRecord& srec = s.symbols.at(sym);
                        if (srec.structScope.valid()) {
                            std::vector<std::pair<std::uint32_t, TypeId>> fields;
                            auto const& scope = s.scopes.scopes()[srec.structScope.v];
                            bool anyInvalid = false;
                            for (auto const& [_name, fieldSymId] : scope.bindings) {
                                SymbolRecord const& frec = s.symbols.at(fieldSymId);
                                if (!frec.declRuleNode.valid()
                                    || frec.tree.v != tree.id().v) continue;
                                if (tree.rule(frec.declRuleNode)
                                    != decl.fieldChildren->rule) continue;
                                if (!frec.type.valid()) {
                                    anyInvalid = true;
                                    break;
                                }
                                fields.emplace_back(frec.fieldIndex, frec.type);
                            }
                            if (!anyInvalid) {
                                std::sort(fields.begin(), fields.end());
                                std::vector<TypeId> fieldTypes;
                                fieldTypes.reserve(fields.size());
                                for (auto const& [_idx, t] : fields)
                                    fieldTypes.push_back(t);
                                // D5.4: struct vs union dispatch is config-
                                // driven via FieldChildrenDescriptor::compositeKind.
                                TypeId const compositeTy =
                                    (decl.fieldChildren->compositeKind
                                     == CompositeKind::Union)
                                    ? s.lattice.interner().unionType(
                                        srec.name, fieldTypes)
                                    : s.lattice.interner().structType(
                                        srec.name, fieldTypes);
                                srec.type = compositeTy;
                                s.nodeToType.set(resolved.node, compositeTy);
                                s.compositeScopeByType[compositeTy.v] = srec.structScope;
                            }
                        }
                    } else if (effectiveKind == DeclarationKind::Function) {
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
                        // GAP A: record the function's RESULT type keyed on
                        // the scope its body opens. A `return` inside the body
                        // walks its scope parent chain to find the nearest
                        // enclosing function result. The body rule is itself a
                        // `scopes` rule (returns must live in a body scope), so
                        // we look up the scope anchored at the body node.
                        if (bodyNode.valid()) {
                            ScopeId const bodyScope =
                                scopeAnchoredAt(s, tree, bodyNode);
                            if (bodyScope.valid()) {
                                s.fnResultByScope[bodyScope.v] = returnTy;
                            }
                        }
                    } else if (returnTy.valid()) {
                        // SE-arrays: a `[N]` declarator suffix wraps the
                        // element type as Array<elem, N>. A non-constant length
                        // leaves the type unresolved (applyArraySuffix already
                        // emitted S_NonConstantArrayLength) so we fail loud.
                        TypeId const declTy =
                            applyArraySuffix(s, tree, decl, node, returnTy);
                        if (declTy.valid()) {
                            s.symbols.at(sym).type = declTy;
                            s.nodeToType.set(resolved.node, declTy);
                        }
                    }
                }
            }
        }
    }
}

// ── Pass 2: post-order — resolve uses + literal/init typing + checks ───────
// `loopDepth` (GAP C) is the count of enclosing `loopRules` subtrees the
// walk is currently inside; a `loopControls` node at depth 0 is outside
// any loop and emits S_ControlOutsideLoop.
void pass2(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
           NodeId node, ScopeId current, int loopDepth) {
    if (!node.valid()) return;
    if (isEmptySpace(tree.flags(node))) return;
    auto const k = tree.kind(node);
    ScopeId here = current;

    if (k == NodeKind::Internal && s.idx().scopeByRule.contains(tree.rule(node).v)) {
        here = childScopeFor(s, tree, node, current);
    }

    // GAP C: entering a loop-context subtree raises the depth for the
    // children walk. Detected on the node itself; the increment applies to
    // descendants (a break/continue is valid anywhere inside the subtree).
    int const childLoopDepth =
        (k == NodeKind::Internal && s.idx().loopByRule.contains(tree.rule(node).v))
            ? loopDepth + 1
            : loopDepth;

    for (auto const& child : tree.children(node)) {
        pass2(s, cfg, tree, child, here, childLoopDepth);
    }

    // Literal typing.
    if (k == NodeKind::Token) {
        auto tk = tree.tokenKind(node);
        auto litIt = s.idx().literalTypeIds.find(tk.v);
        if (litIt != s.idx().literalTypeIds.end()) {
            s.nodeToType.set(node, litIt->second);
        }
    }

    // Reference resolution. Skip if the node is a declaration's own name
    // slot (declaration and reference shapes can structurally overlap).
    if (k == NodeKind::Internal) {
        auto const rule = tree.rule(node);
        auto refIt = s.idx().refByRule.find(rule.v);
        if (refIt != s.idx().refByRule.end()) {
            bool isDeclSite = false;
            NodeId parent = tree.parent(node);
            if (parent.valid() && tree.kind(parent) == NodeKind::Internal) {
                auto parentRule = tree.rule(parent);
                auto parentDeclIt = s.idx().declByRule.find(parentRule.v);
                if (parentDeclIt != s.idx().declByRule.end()) {
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
                    tree, node, ref.nameMatch, cfg.identifierToken,
                    cfg.bracketIdentifierToken);
                // Only treat the site as an identifier USE when the resolved
                // leaf is genuinely a name-bearing leaf (the config's
                // `identifierToken` OR, when set, its `bracketIdentifierToken`
                // — GAP D). Reference rules can structurally cover non-name
                // leaves (e.g. c-subset's `operand` covers IntLiteral too);
                // resolving those as names would emit phantom
                // S_UndeclaredIdentifier diagnostics.
                bool isIdentifier = false;
                if (resolved.node.valid()
                    && tree.kind(resolved.node) == NodeKind::Token) {
                    auto const rtk = tree.tokenKind(resolved.node);
                    if (cfg.identifierToken.valid() && rtk == cfg.identifierToken) {
                        isIdentifier = true;
                    } else if (cfg.bracketIdentifierToken.has_value()
                               && cfg.bracketIdentifierToken->valid()
                               && rtk == *cfg.bracketIdentifierToken) {
                        isIdentifier = true;
                    }
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
                        // Hard (must-resolve) iff `hardParents` is empty (the
                        // lexical default) OR this reference's parent rule is one
                        // of the configured must-resolve positions. A soft
                        // position (e.g. a SQL column in an expression) leaves the
                        // name unresolved (sym 0) without an error — the lowering
                        // recovers it from source provenance.
                        bool hard = ref.hardParents.empty();
                        if (!hard) {
                            NodeId refParent = tree.parent(node);
                            if (refParent.valid()
                                && tree.kind(refParent) == NodeKind::Internal) {
                                RuleId const pr = tree.rule(refParent);
                                for (RuleId hp : ref.hardParents)
                                    if (hp.v == pr.v) { hard = true; break; }
                            }
                        }
                        if (hard) {
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

        // D5.1: member-access resolution (`obj.field` and `ptr->field`).
        // Pass 2 visits post-order so the LHS's type is already in
        // `nodeToType` by the time we reach the member-access internal
        // node. We:
        //   1. read LHS type via `typeAt(kids[lhsChild])`,
        //   2. unwrap one Ptr layer if the access dereferences (arrow form),
        //   3. find the resolved struct's inner scope via
        //      `compositeScopeByType` (populated by Pass 1.5),
        //   4. extract the field-name identifier text from `kids[nameChild]`,
        //   5. look it up in the struct's scope → bind the field SymbolId on
        //      the name node and propagate the field type to the
        //      member-access node (so chained access `f.x.y` resolves the
        //      next layer in the same post-order step).
        // No conflict with the reference-resolution block above: that one
        // resolves identifier USES against the LEXICAL scope chain; this one
        // resolves field-names against a struct's STRUCT scope — orthogonal.
        auto maIt = s.idx().memberAccessByRule.find(rule.v);
        // Pick the matching entry by operator-token (mirrors `assignByRule`'s
        // discipline: multiple entries per rule are distinguished by which
        // operator token — `.` vs `->` — is present in the node). Ungated
        // entries match unconditionally and must be the sole entry for the
        // rule. Returns null when no entry's operator-token matches: the node
        // is structurally a member-access-rule node but carries a different
        // operator (e.g. c-subset's `postfixExpr` with `++`/`(`/`[`).
        MemberAccessRule const* const ma = [&]() -> MemberAccessRule const* {
            if (maIt == s.idx().memberAccessByRule.end()) return nullptr;
            for (std::size_t midx : maIt->second) {
                auto const& cand = cfg.memberAccesses[midx];
                if (!cand.operatorToken.has_value()) return &cand;  // ungated
                for (auto kid : tree.children(node)) {
                    if (tree.kind(kid) == NodeKind::Token
                        && tree.tokenKind(kid) == *cand.operatorToken) {
                        return &cand;
                    }
                }
            }
            return nullptr;
        }();
        if (ma != nullptr) {
            auto kids = visibleChildren(tree, node);
            if (ma->lhsChild < kids.size() && ma->nameChild < kids.size()) {
                NodeId const lhsNode  = kids[ma->lhsChild];
                NodeId const nameNode = kids[ma->nameChild];
                TypeId lhsType = s.typeAt(lhsNode);
                if (lhsType.valid()) {
                    TypeId effectiveType = lhsType;
                    bool   typeOk = true;
                    if (ma->dereferences) {
                        // Arrow form: `p->x` is sugar for `(*p).x`. The LHS
                        // must be `Ptr<Struct>`; one indirection only. If the
                        // LHS isn't even a pointer, emit S_NotAPointer; the
                        // post-unwrap "non-composite" case (Ptr<int>->x) falls
                        // through to S_NotAComposite below with arrow-aware
                        // wording.
                        if (s.lattice.interner().kind(effectiveType)
                            == TypeKind::Ptr) {
                            auto ops = s.lattice.interner()
                                       .operands(effectiveType);
                            if (!ops.empty()) effectiveType = ops[0];
                            else typeOk = false;
                        } else {
                            typeOk = false;
                        }
                        if (!typeOk) {
                            ParseDiagnostic d;
                            d.code     = DiagnosticCode::S_NotAPointer;
                            d.severity = DiagnosticSeverity::Error;
                            d.buffer   = tree.source().id();
                            d.span     = tree.span(nameNode);
                            d.actual   = "arrow operator '->' requires a "
                                         "pointer operand";
                            s.reporter.report(std::move(d));
                        }
                    }
                    if (typeOk) {
                        auto scopeIt =
                            s.compositeScopeByType.find(effectiveType.v);
                        if (scopeIt == s.compositeScopeByType.end()) {
                            ParseDiagnostic d;
                            d.code     = DiagnosticCode::S_NotAComposite;
                            d.severity = DiagnosticSeverity::Error;
                            d.buffer   = tree.source().id();
                            d.span     = tree.span(nameNode);
                            // Disambiguate the two arrow-form failure shapes
                            // in the message text so the reader knows whether
                            // the LHS itself was non-composite (`.x` form) or
                            // the LHS was Ptr<T> but T is non-composite (`->x`
                            // form — review F5).
                            d.actual   = ma->dereferences
                                ? "arrow operator '->' pointee is not a "
                                  "composite type"
                                : "member access '.' requires a composite-"
                                  "typed operand";
                            s.reporter.report(std::move(d));
                        } else {
                            auto fnRes = extractNameNode(
                                tree, nameNode, NameMatchMode::Self,
                                cfg.identifierToken,
                                cfg.bracketIdentifierToken);
                            // Gate to a real identifier leaf, mirroring the
                            // reference-resolution discipline above. A non-
                            // identifier nameChild is a schema-shape bug
                            // (member-access's nameChild is positionally
                            // fixed by the rule) — emit C_InvalidSemantics
                            // rather than silently failing or producing a
                            // phantom S_UndeclaredIdentifier on the token's
                            // raw source text (reviews F3 + F7).
                            bool isIdentifier = false;
                            if (fnRes.node.valid()
                                && tree.kind(fnRes.node) == NodeKind::Token) {
                                auto const rtk = tree.tokenKind(fnRes.node);
                                if (cfg.identifierToken.valid()
                                    && rtk == cfg.identifierToken) {
                                    isIdentifier = true;
                                } else if (cfg.bracketIdentifierToken
                                               .has_value()
                                           && cfg.bracketIdentifierToken
                                                  ->valid()
                                           && rtk == *cfg.bracketIdentifierToken) {
                                    isIdentifier = true;
                                }
                            }
                            if (!isIdentifier || fnRes.name.empty()) {
                                ParseDiagnostic d;
                                d.code     = DiagnosticCode::C_InvalidSemantics;
                                d.severity = DiagnosticSeverity::Error;
                                d.buffer   = tree.source().id();
                                d.span     = tree.span(nameNode);
                                d.actual   = "member-access nameChild did not "
                                             "resolve to an identifier leaf";
                                s.reporter.report(std::move(d));
                            } else {
                                ScopeId const fieldScope = scopeIt->second;
                                auto const& sc =
                                    s.scopes.scopes()[fieldScope.v];
                                auto bindIt = sc.bindings.find(fnRes.name);
                                if (bindIt != sc.bindings.end()) {
                                    SymbolId const fieldSym = bindIt->second;
                                    SymbolRecord const& frec =
                                        s.symbols.at(fieldSym);
                                    s.nodeToSymbol.set(fnRes.node, fieldSym);
                                    s.usesBySymbol[fieldSym.v].push_back(
                                        fnRes.node);
                                    if (frec.type.valid()) {
                                        s.nodeToType.set(fnRes.node, frec.type);
                                        s.nodeToType.set(node, frec.type);
                                    }
                                } else {
                                    ParseDiagnostic d;
                                    d.code     = DiagnosticCode::S_UndeclaredIdentifier;
                                    d.severity = DiagnosticSeverity::Error;
                                    d.buffer   = tree.source().id();
                                    d.span     = tree.span(fnRes.node);
                                    d.actual   = fnRes.name;
                                    s.reporter.report(std::move(d));
                                }
                            }
                        }
                    }
                }
                // lhsType invalid ⇒ likely a chained error; stay quiet.
            }
        }
    }

    // Initializer-based type inference for declarations whose declared
    // type was absent (e.g. toy's `var x = ...;` has no `typeChild`).
    // ALSO: type-check assignment when an explicit type IS present.
    if (k == NodeKind::Internal) {
        auto const rule = tree.rule(node);
        auto declIt = s.idx().declByRule.find(rule.v);
        if (declIt != s.idx().declByRule.end()) {
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
        auto assignIt = s.idx().assignByRule.find(rule.v);
        if (assignIt != s.idx().assignByRule.end()) {
            auto kids = visibleChildren(tree, node);
            // Several assignment entries may share this rule (operator-table
            // `binaryExpr` reused for `=` and every compound-assign op). Each
            // entry's `operatorToken` (when set) gates the match: a binaryExpr
            // only counts as that assignment when one of its visible children
            // IS that operator token. Apply the FIRST entry whose gate passes
            // (a binaryExpr carries exactly one operator, so at most one entry
            // can fire) — then stop.
            for (auto assignIdx : assignIt->second) {
                auto const& assign = cfg.assignments[assignIdx];
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
                if (!gated) continue;
                if (assign.lhsChild < kids.size()) {
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
                break;  // operator matched — no further entry can apply
            }
        }
    }

    // SE6: call checking.
    if (k == NodeKind::Internal) {
        auto const rule = tree.rule(node);
        auto callIt = s.idx().callByRule.find(rule.v);
        if (callIt != s.idx().callByRule.end()) {
            checkCall(s, cfg, tree, node, here, cfg.callRules[callIt->second]);
        }
    }

    // GAP A: return-type checking. When this node is a `returnRules` rule,
    // find the nearest enclosing function result type (walk the scope parent
    // chain via `fnResultByScope`) and check the returned expression against
    // it. A bare `return;` is valid only in a Void function; a `return expr;`
    // in a Void function — or a bare `return;` in a non-Void function — is a
    // mismatch. An unknown/Invalid enclosing result is skipped (cascade
    // suppression).
    if (k == NodeKind::Internal) {
        auto retIt = s.idx().returnByRule.find(tree.rule(node).v);
        if (retIt != s.idx().returnByRule.end()) {
            checkReturn(s, cfg, tree, node, here, cfg.returnRules[retIt->second]);
        }
    }

    // GAP C: break/continue outside any loop. A `loopControls` node at
    // depth 0 is outside every loop-context subtree.
    if (k == NodeKind::Internal && loopDepth == 0
        && s.idx().loopControlByRule.contains(tree.rule(node).v)) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::S_ControlOutsideLoop;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(node);
        d.actual   = std::string{tree.text(node)};
        s.reporter.report(std::move(d));
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
        auto declIt = s.idx().declByRule.find(tree.rule(cur).v);
        if (declIt != s.idx().declByRule.end()) {
            auto const& decl = cfg.declarations[declIt->second];
            auto kids = visibleChildren(tree, cur);
            if (decl.typeChild.has_value() && *decl.typeChild < kids.size()) {
                TypeId pty = resolveTypeNode(
                    s, cfg, tree, kids[*decl.typeChild], scope);
                // SE-arrays: an array-declarator param (`int a[10]`) carries
                // the Array type into the signature.
                pty = applyArraySuffix(s, tree, decl, cur, pty);
                out.push_back(pty);
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
                if (s.idx().refByRule.contains(tree.rule(probe).v)) {
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

    // FIX 2: the call EXPRESSION carries the callee's RESULT type — not its
    // FnSig. Without this, a `return f(args);` walk (subtreeType) would
    // surface the callee identifier's FnSig (which IS typed, below) and
    // wrongly fail `isAssignable(I32, FnSig)` → a spurious
    // S_ReturnTypeMismatch on legal `int g(){ return f(); }`. Typing the
    // call node with its result also feeds future consumers (e.g.
    // `int x = f();` initializer-type-flow). Set BEFORE arity/arg checks so
    // the result type is present even when an arg mismatch is reported (the
    // call's value type is its declared result regardless of arg errors).
    s.nodeToType.set(node, s.lattice.interner().fnResult(fnTy));

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

// GAP A: the type carried by an expression subtree. Pass 2 types literal
// leaves and propagates resolved-symbol types to reference wrapper nodes,
// but a literal's type lives on the leaf, not the enclosing expression
// wrapper. So check `node` first, then DFS for the first typed descendant.
// Generic — no rule names, just the side-table.
[[nodiscard]] TypeId
subtreeType(EngineState const& s, Tree const& tree, NodeId node) {
    if (!node.valid()) return InvalidType;
    if (TypeId t = s.typeAt(node); t.valid()) return t;
    std::vector<NodeId> stack{node};
    while (!stack.empty()) {
        NodeId cur = stack.back();
        stack.pop_back();
        if (TypeId t = s.typeAt(cur); t.valid()) return t;
        for (auto child : tree.children(cur)) {
            if (!isEmptySpace(tree.flags(child))) stack.push_back(child);
        }
    }
    return InvalidType;
}

// GAP A: check a return statement against the nearest enclosing function
// result type. See the call site in pass2 for the policy summary.
void checkReturn(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                 NodeId node, ScopeId scope, ReturnRule const& ret) {
    (void)cfg;
    // Walk the scope parent chain for the nearest enclosing function result.
    TypeId fnResult = InvalidType;
    bool   foundFn  = false;
    auto const& scopes = s.scopes.scopes();
    for (ScopeId cur = scope; cur.valid() && cur.v < scopes.size();
         cur = scopes[cur.v].parent) {
        auto it = s.fnResultByScope.find(cur.v);
        if (it != s.fnResultByScope.end()) {
            fnResult = it->second;
            foundFn  = true;
            break;
        }
    }
    // A return outside any function we tracked → nothing to check against.
    if (!foundFn) return;

    // Resolve the returned-expression node. The `value` child (when
    // configured) names the expression slot; but an OPTIONAL expression
    // grammar means a bare `return;` puts the statement terminator (a
    // Token) at that slot — treat only an Internal node as a real returned
    // expression.
    NodeId valueNode{};
    if (ret.valueChild.has_value()) {
        auto kids = visibleChildren(tree, node);
        if (*ret.valueChild < kids.size()
            && tree.kind(kids[*ret.valueChild]) == NodeKind::Internal) {
            valueNode = kids[*ret.valueChild];
        }
    }

    bool const isVoid =
        fnResult.valid()
        && s.lattice.interner().kind(fnResult) == TypeKind::Void;

    auto emitMismatch = [&](NodeId span) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::S_ReturnTypeMismatch;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(span);
        d.actual   = std::string{tree.text(span)};
        s.reporter.report(std::move(d));
    };

    if (!valueNode.valid()) {
        // Bare `return;` — valid ONLY in a Void function. If the enclosing
        // result is unknown/Invalid, skip (cascade suppression).
        if (fnResult.valid() && !isVoid) emitMismatch(node);
        return;
    }

    // `return expr;` in a Void function is always a mismatch.
    if (isVoid) { emitMismatch(valueNode); return; }

    // Otherwise compare the returned expression's type to the result type.
    // An unknown result OR unknown expr type → skip (cascade suppression).
    TypeId const exprTy = subtreeType(s, tree, valueNode);
    if (!fnResult.valid() || !exprTy.valid()) return;
    if (!isAssignable(s.lattice.interner(), fnResult, exprTy)) {
        emitMismatch(valueNode);
    }
}

} // namespace

SemanticModel analyze(std::shared_ptr<CompilationUnit const> cu) {
    if (!cu) {
        std::fputs("dss::analyze fatal: null CompilationUnit\n", stderr);
        std::abort();
    }
    EngineState s{*cu};

    // HR11: build one index bundle per DISTINCT schema in the CU (keyed by
    // SchemaId), each with its type extensions registered into the shared CU
    // lattice FIRST (so a builtinType naming an extension resolves its kindId).
    // A homogeneous CU has exactly one bundle. `tree.schema()` is the
    // authoritative per-file language; the CU-level `schema()` is unused here.
    std::vector<GrammarSchema const*> distinctSchemas;
    for (Tree const& tree : cu->trees()) {
        GrammarSchema const& sch = tree.schema();
        if (s.schemaIndexes.contains(sch.schemaId().v)) continue;
        registerSchemaTypeExtensions(s.lattice.registry(), sch);
        SchemaIndexes& idx = s.schemaIndexes[sch.schemaId().v];
        idx.numberStyle = sch.numberStyle();           // for constant array-length decode
        buildIndexes(s, idx, sch.semantics());
        distinctSchemas.push_back(&sch);
    }

    // SE6 + HR11: a PER-LANGUAGE builtins scope (child of the CU root) holds that
    // schema's config-declared builtin functions, and a tree's root scope parents
    // to ITS language's builtin scope. So builtins are visible within their own
    // language (and shadowable by user decls) but DON'T leak across languages — a
    // c-subset file must not resolve tsql's COALESCE; a genuine cross-language
    // call is the FFI plan's job, not a silent builtin hit. Homogeneous CU: one
    // builtin scope, identical to CU1-CU4.
    ScopeId const cuRoot = s.scopes.root();
    std::unordered_map<std::uint32_t /*SchemaId.v*/, ScopeId> builtinScopeBySchema;
    for (GrammarSchema const* sch : distinctSchemas) {
        ScopeId const builtinScope = s.scopes.pushScope(cuRoot, InvalidNode, InvalidTree);
        builtinScopeBySchema[sch->schemaId().v] = builtinScope;
        for (auto const& bf : sch->semantics().builtinFunctions) {
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
    }

    // Each tree gets its OWN root scope (a child of ITS language's builtins
    // scope) so two unrelated files' top-level decls live in separate namespaces
    // but each still sees its own language's builtins.
    auto const trees = cu->trees();
    std::unordered_map<std::uint32_t /*TreeId.v*/, ScopeId> treeRootScope;

    // Pass 1 per tree, declaring into THAT tree's root scope. `activate` selects
    // the tree's own schema bundle (HR11) so its rule-id index lookups + cfg are
    // its own language's — a tree never sees another schema's config.
    for (auto const& tree : trees) {
        if (!tree.root().valid()) continue;
        s.activate(tree.schema());
        ScopeId const builtinScope = builtinScopeBySchema.at(tree.schema().schemaId().v);
        ScopeId const treeRoot = s.scopes.pushScope(builtinScope, tree.root(), tree.id());
        treeRootScope[tree.id().v] = treeRoot;
        pass1(s, *s.idx().cfg, tree, tree.root(), treeRoot);
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
        s.activate(tree.schema());
        resolveDeclTypes(s, *s.idx().cfg, tree, tree.root(), treeRootScope.at(tree.id().v));
    }

    // Pass 2 per tree, against that tree's root scope. Loop-context depth
    // starts at 0 (GAP C).
    for (auto const& tree : trees) {
        if (!tree.root().valid()) continue;
        s.activate(tree.schema());
        pass2(s, *s.idx().cfg, tree, tree.root(), treeRootScope.at(tree.id().v), 0);
    }

    // D8: unused-variable warnings. After Pass 2 has fully populated the
    // SE7 reverse use-index (`usesBySymbol`), any symbol whose minting
    // declaration opted in (`warnIfUnused`) AND whose use-set is EMPTY is
    // never referenced — emit S_UnusedVariable (a WARNING) at the
    // declaration's rule-node span. No CFG needed: never-referenced is a
    // direct read of the reverse index. Config-driven and per-declaration:
    // only symbols from a `warnIfUnused: true` declaration are candidates.
    {
        std::unordered_map<std::uint32_t /*TreeId.v*/, Tree const*> treeById;
        for (auto const& tree : trees) treeById[tree.id().v] = &tree;
        auto const& records = s.symbols.records();
        for (std::size_t i = 1; i < records.size(); ++i) {
            auto const& rec = records[i];
            if (!rec.warnIfUnused) continue;
            auto useIt = s.usesBySymbol.find(static_cast<std::uint32_t>(i));
            if (useIt != s.usesBySymbol.end() && !useIt->second.empty()) continue;
            auto treeIt = treeById.find(rec.tree.v);
            if (treeIt == treeById.end()) {
                // Every minted user symbol carries a valid tree id (set at
                // pass 1). A warnIfUnused symbol whose tree is absent from the
                // CU is an internal invariant violation — fail loud.
                std::fputs("dss::analyze fatal: unused-variable symbol "
                           "references a tree absent from the compilation "
                           "unit\n", stderr);
                std::abort();
            }
            Tree const& tree = *treeIt->second;
            ParseDiagnostic d;
            d.code     = DiagnosticCode::S_UnusedVariable;
            d.severity = DiagnosticSeverity::Warning;
            d.buffer   = tree.source().id();
            d.span     = tree.span(rec.declRuleNode);
            d.actual   = rec.name;
            s.reporter.report(std::move(d));
        }
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
        std::move(s.compositeScopeByType),
    };
}

} // namespace dss
