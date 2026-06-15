#include "analysis/semantic/semantic_analyzer.hpp"

#include "analysis/compilation_unit/unit_attribute.hpp"
#include "analysis/semantic/scope_tree.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/symbol_table.hpp"
#include "analysis/semantic/type_rules.hpp"
#include "core/types/data_model.hpp"
#include "core/types/decl_prefix_strip.hpp"   // declRoleChildren / descendVisibleDecl / specifierPrefixChild
#include "core/types/declarator_walk.hpp"     // FC4: declaratorNameNode / collectDeclarators
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/integer_literal_ladder.hpp"
#include "core/types/number_decode.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/string_style.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "core/types/type_lattice/type_layout.hpp"  // FC6: computeLayout (sizeof in array dims)
#include "ffi/shipped_lib_descriptor.hpp"   // FF11 neutral-JSON descriptor reader
#include "hir/cst_const_eval.hpp"
#include "hir/hir_op.hpp"   // FC6 c-subtreeType: HirOpKind / opName / isComparison (the per-verb laws cst_to_hir uses)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
    std::unordered_map<std::uint32_t, std::size_t> castByRule;      // FC2 explicit casts
    // FC3.5 sweep-c3: compound-literal type stamping
    // (D-CSUBSET-COMPOUND-LITERAL-TYPEDEF)
    std::unordered_map<std::uint32_t, std::size_t> compoundLiteralByRule;
    std::unordered_map<std::uint32_t, std::size_t> returnByRule;   // GAP A
    std::unordered_map<std::uint32_t, bool>        loopByRule;      // GAP C loop contexts
    std::unordered_map<std::uint32_t, bool>        loopControlByRule; // GAP C break/continue
    // Built-in type name → TypeId (interned once per schema, into the CU
    // lattice; FC3 c1 — the per-row `coreByDataModel` override for the
    // ACTIVE data model is applied here, so every consumer below sees
    // the model-correct width).
    std::unordered_map<std::string, TypeId>        builtinTypeIds;
    // Literal token-kind → TypeId.
    std::unordered_map<std::uint32_t, TypeId>      literalTypeIds;
    // FC3 c1: type-specifier multiset resolution (C 6.7.2). The
    // VOCABULARY is every token kind appearing in any `typeSpecifiers`
    // row — `resolveTypeNode` treats an Internal node whose visible
    // children are ALL vocabulary tokens as a specifier run. The SETS
    // map keys the canonical (sorted, comma-joined kind-id) multiset to
    // its interned TypeId (data model already applied). Empty for
    // languages without the table (toy / tsql) — the arm never fires.
    std::unordered_set<std::uint32_t>              typeSpecifierVocabulary;
    std::unordered_map<std::string, TypeId>        typeSpecifierSets;
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
    // FC3 c1: the analysis-time data model (`analyze()`'s parameter —
    // the active format's width triple). Read by `buildIndexes` (the
    // `coreByDataModel` overrides), the integer-literal ladder, and the
    // shipped-lib descriptor reader. Set ONCE before any index is built.
    DataModel                  dataModel = DataModel::Lp64;
    // FC6 deferral-close: the active target's aggregate-layout params
    // (`analyze()`'s parameter — target.aggregateLayout()). `nullopt` ⇒ the
    // target declared no `aggregateLayout` block, so a `sizeof` in an array
    // dimension fails loud rather than folding a wrong size. Read by
    // `constIntExpr`'s sizeof-folding closure (array-dimension const-context).
    std::optional<AggregateLayoutParams> aggregateLayout;
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

// D-DECL-SPECIFIER-PREFIX-SUBSTRATE: `declRoleChildren` (a declaration's
// visible children with a leading specifier prefix stripped — what positional
// name/type/params/body/kindByChild indices resolve against) and
// `descendVisibleDecl` (strip-aware first step, raw later steps) come from the
// SHARED `core/types/decl_prefix_strip.hpp` (D-DECL-PREFIX-STRIP-SHARED-HELPER
// — one source of truth across the analyzer, cst_const_eval, and CST→HIR).

// Forward declarations (these helpers are referenced by the passes
// before their definitions appear). `CallRule` comes from
// semantic_config.hpp (dss namespace).
//
// FC4 c1: `collectParamTypes` collects (param NODE, resolved type) pairs —
// the node lets the `(void)` normalization decide named-vs-unnamed and
// position its diagnostic. `emitOnMiss=false` suppresses re-diagnosis when
// a harvest re-resolves types a param row's own visit already reported.
void collectParamTypes(EngineState& s, SemanticConfig const& cfg,
                       Tree const& tree, NodeId cur, ScopeId scope,
                       std::vector<std::pair<NodeId, TypeId>>& out,
                       bool emitOnMiss = true);
void normalizeSoleVoidParams(EngineState& s, SemanticConfig const& cfg,
                             Tree const& tree,
                             std::vector<std::pair<NodeId, TypeId>>& params,
                             bool emitOnMiss);
void checkCall(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
               NodeId node, ScopeId scope, CallRule const& call);
void checkReturn(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                 NodeId node, ScopeId scope, ReturnRule const& ret);
void gatherArgExpressions(Tree const& tree, NodeId argsNode,
                          std::vector<NodeId>& out);
[[nodiscard]] TypeId subtreeType(EngineState const& s, Tree const& tree,
                                 NodeId node, ScopeId scope = {});

// FC6 (C99 §6.7.2.1p18): does `t` CONTAIN a flexible array member — i.e. is it a
// struct whose LAST member is an incomplete array (so `t` is itself a FAM
// struct), or an aggregate that recursively holds one (a struct field, an array
// element)? Such a type may not be embedded as a struct member or array element.
// Terminates: the interned operand DAG is finite + acyclic (C forbids a struct
// containing itself by value). A BARE incomplete array (`t` itself a FAM) is NOT
// "containing" one — that case is the struct's own valid trailing member,
// checked directly at the composition site.
[[nodiscard]] bool typeContainsFlexibleArray(TypeInterner const& in, TypeId t) {
    if (!t.valid()) return false;
    TypeKind const k = in.kind(t);
    if (k == TypeKind::Struct) {
        auto const ops = in.operands(t);
        if (!ops.empty() && in.isIncompleteArray(ops[ops.size() - 1])) return true;
        for (TypeId f : ops)
            if (typeContainsFlexibleArray(in, f)) return true;
        return false;
    }
    if (k == TypeKind::Array) {
        if (in.isIncompleteArray(t)) return false;  // bare FAM — not "containing"
        auto const ops = in.operands(t);
        return !ops.empty() && typeContainsFlexibleArray(in, ops[0]);
    }
    return false;
}
[[nodiscard]] bool admitsNullPointerConstant(
    EngineState const& s, Tree const& tree,
    TypeId lhsTy, NodeId rhsExpr,
    SemanticConfig::PointerConversionRules const& rules);

// True iff a token of kind `kind` appears anywhere in `node`'s subtree,
// stopping descent at any NESTED declaration-rule node (other than the
// root `node` itself). Used by SE4 const-marker detection (walk a decl's
// `typeChild` for the language's const keyword) AND by D-LANG-VARIADIC
// variadic-marker detection (walk a decl's `paramsChild` for the
// language's `EllipsisOp` marker). Generic substrate — any future
// "marker token within a decl subtree" scan (async/inline/noexcept
// markers etc.) reuses this same walker with the same decl-rule stop.
//
// `declByRule` is the map from RuleId.v → declarations[] index that
// EngineState builds at activate-time. Pass `&s.idx().declByRule` for
// safe-bounded descent (the audit-fold default — closes silent-failure
// HIGH-2 step 13.4: without it, a future grammar that nested a
// default-value expression INSIDE a `param` could false-match a marker
// in the inner expression and silently lower every fixed-arity callee
// as variadic). Pass `nullptr` ONLY when the scan MUST cross nested
// declaration-boundaries by design — a deliberate choice, not an
// oversight or a copy-paste from the legacy unbounded shape.
[[nodiscard]] bool
subtreeContainsToken(Tree const& tree, NodeId node, SchemaTokenId kind,
                     std::unordered_map<std::uint32_t, std::size_t> const*
                         declByRule = nullptr) {
    if (!node.valid() || !kind.valid()) return false;
    std::vector<NodeId> stack{node};
    bool firstPop = true;
    while (!stack.empty()) {
        NodeId cur = stack.back();
        stack.pop_back();
        if (tree.kind(cur) == NodeKind::Token && tree.tokenKind(cur) == kind) {
            return true;
        }
        // Stop descent at any nested declaration-rule node (the root
        // itself IS a decl subtree by contract and stays in scope).
        if (!firstPop && declByRule != nullptr
            && tree.kind(cur) == NodeKind::Internal
            && declByRule->contains(tree.rule(cur).v)) {
            continue;
        }
        firstPop = false;
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
    for (std::size_t i = 0; i < cfg.castRules.size(); ++i) {
        idx.castByRule[cfg.castRules[i].rule.v] = i;
    }
    for (std::size_t i = 0; i < cfg.compoundLiteralRules.size(); ++i) {
        idx.compoundLiteralByRule[cfg.compoundLiteralRules[i].rule.v] = i;
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
        // FC3 c1: the ACTIVE data model's override (if declared) wins —
        // c-subset's `long` is I64 base / I32 under LLP64.
        idx.builtinTypeIds[bt.name] =
            s.lattice.interner().primitive(bt.resolveCore(s.dataModel));
    }
    for (auto const& lt : cfg.literalTypes) {
        idx.literalTypeIds[lt.literal.v] = s.lattice.interner().primitive(lt.core);
    }
    // FC3 c1: type-specifier multiset table (C 6.7.2). Vocabulary = every
    // token kind any row names; sets = canonical sorted-kind key → the
    // interned TypeId under the ACTIVE data model. The loader already
    // sorted each row's `tokens` and rejected duplicate multisets.
    for (auto const& ts : cfg.typeSpecifiers) {
        std::string key;
        for (auto const& t : ts.tokens) {
            idx.typeSpecifierVocabulary.insert(t.v);
            key += std::to_string(t.v);
            key += ',';
        }
        idx.typeSpecifierSets[std::move(key)] =
            s.lattice.interner().primitive(ts.resolveCore(s.dataModel));
    }
}

// Resolve a type-position subtree to a TypeId. Walks `typeShapes`
// recursively (e.g. pointerType[innerType] → Ptr<innerType>) and looks
// the leaf up in `builtinTypes`. A leaf that is not a built-in type but
// resolves (via scope-chain lookup from `scope`) to a `Type`-kind symbol
// with a valid type yields that aliased type (SE5 typedef resolution).
// Emits `S_UnknownType` when no matching mapping was found in the
// subtree's leaf.
//
// FC3 c1: `specifierDiagnosed` threads the "an invalid type-specifier
// COMBINATION was already reported inside this resolution" fact up the
// recursion so the outer-level miss arm does not pile a generic
// S_UnknownType on top of the precise S_InvalidTypeSpecifierCombination
// (one definitive diagnostic per bad type, the house one-diag bar).
// The public wrapper below owns the flag; external callers are
// signature-unchanged.
[[nodiscard]] TypeId
resolveTypeNodeImpl(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                    NodeId node, ScopeId scope, bool emitOnMiss,
                    bool& specifierDiagnosed) {
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
            TypeId inner = resolveTypeNodeImpl(s, cfg, tree, kids[shape.operandChild],
                                               scope, emitOnMiss, specifierDiagnosed);
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
        // FC4 c1: an inline TYPE-DECLARATION in type position — C's
        // `typedef struct {...} T;` / `typedef struct P {...} MyP;`,
        // where the head subtree IS a composite-minting declaration
        // (a `declarations` row of kind Type). Resolve to the type the
        // declaration's own (post-order-earlier) Pass 1.5 visit minted
        // for its symbol — named rows anchor the symbol on the name
        // node; anonymous rows (anonymousNameAllowed) on the decl node
        // itself. WITHOUT this arm the generic child-descent below
        // would recurse into the struct BODY and silently resolve the
        // FIRST FIELD's type as the head type. An unresolved minted
        // type returns InvalidType and the outer miss arm reports.
        {
            auto declIt = s.idx().declByRule.find(rule.v);
            if (declIt != s.idx().declByRule.end()
                && cfg.declarations[declIt->second].kind
                       == DeclarationKind::Type) {
                auto const& drow = cfg.declarations[declIt->second];
                SymbolId sym{};
                auto dkids = declRoleChildren(tree, node, drow);
                if (drow.nameChild.has_value()
                    && *drow.nameChild < dkids.size()) {
                    auto rn = extractNameNode(
                        tree, dkids[*drow.nameChild], drow.nameMatch,
                        cfg.identifierToken, cfg.bracketIdentifierToken);
                    if (rn.node.valid()) sym = s.symbolAtOr(rn.node);
                }
                if (!sym.valid()) sym = s.symbolAtOr(node);
                if (sym.valid()) {
                    auto const& rec = s.symbols.at(sym);
                    if (rec.type.valid()) return rec.type;
                }
                return InvalidType;
            }
        }
        // FC3 c1: type-specifier keyword run (C 6.7.2 — `unsigned long
        // long int` in any order). An Internal node whose visible
        // children are ALL tokens of the language's declared specifier
        // VOCABULARY resolves by canonical (sorted) multiset lookup in
        // the `typeSpecifiers` table. An undeclared combination
        // (`unsigned float`, `short long`) fails loud HERE — it is a
        // definitive semantic reject (the run is structurally a
        // specifier set; no alternative resolution exists), so the
        // diagnostic fires regardless of `emitOnMiss` (which only
        // suppresses the "maybe another child resolves" miss path).
        // Languages without the table have an empty vocabulary — the
        // arm never fires and resolution is byte-identical to pre-FC3.
        if (!s.idx().typeSpecifierSets.empty()) {
            auto specKids = visibleChildren(tree, node);
            bool allSpecifierTokens = !specKids.empty();
            std::vector<std::uint32_t> kinds;
            kinds.reserve(specKids.size());
            for (auto child : specKids) {
                if (tree.kind(child) == NodeKind::Token
                    && s.idx().typeSpecifierVocabulary.contains(
                           tree.tokenKind(child).v)) {
                    kinds.push_back(tree.tokenKind(child).v);
                    continue;
                }
                allSpecifierTokens = false;
                break;
            }
            if (allSpecifierTokens) {
                std::sort(kinds.begin(), kinds.end());
                std::string key;
                for (auto kv : kinds) {
                    key += std::to_string(kv);
                    key += ',';
                }
                auto setIt = s.idx().typeSpecifierSets.find(key);
                if (setIt != s.idx().typeSpecifierSets.end()) {
                    return setIt->second;
                }
                ParseDiagnostic d;
                d.code     = DiagnosticCode::S_InvalidTypeSpecifierCombination;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = tree.source().id();
                d.span     = tree.span(node);
                d.actual   = std::string{tree.text(node)};
                s.reporter.report(std::move(d));
                specifierDiagnosed = true;   // outer S_UnknownType suppressed
                return InvalidType;
            }
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
                auto t = resolveTypeNodeImpl(s, cfg, tree, child, scope,
                                             /*emitOnMiss=*/false,
                                             specifierDiagnosed);
                if (t.valid()) inner = t;
            }
        }
        if (inner.valid()) {
            for (std::uint32_t i = 0; i < ptrDepth; ++i)
                inner = s.lattice.interner().pointer(inner);
            return inner;
        }
        if (emitOnMiss && !specifierDiagnosed) {
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

// The public type-position resolver — every external call site uses this
// signature; the wrapper owns the FC3 c1 specifier-diagnosed flag (see
// resolveTypeNodeImpl).
[[nodiscard]] TypeId
resolveTypeNode(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                NodeId node, ScopeId scope, bool emitOnMiss = true) {
    bool specifierDiagnosed = false;
    return resolveTypeNodeImpl(s, cfg, tree, node, scope, emitOnMiss,
                               specifierDiagnosed);
}

// True for the core integer kinds (signed + unsigned). Array lengths must be
// an integer literal; a float/char literal in length position is rejected.
[[nodiscard]] constexpr bool isIntegerKind(TypeKind k) {
    return k >= TypeKind::I8 && k <= TypeKind::U128;
}

// Build the int-literal-token set from this schema's `literalTypeIds`
// filtered to integer cores. Small (≤5 entries per language); built
// lazily per `constIntExpr` / enum / etc. call. Filtering at use-time
// means a future schema adding a new integer literal kind is picked up
// without touching this code.
[[nodiscard]] std::unordered_set<std::uint32_t>
integerLiteralTokenSet(EngineState const& s) {
    std::unordered_set<std::uint32_t> out;
    for (auto const& [tok, ty] : s.idx().literalTypeIds) {
        if (isIntegerKind(s.lattice.interner().kind(ty))) out.insert(tok);
    }
    return out;
}

// SE-arrays / D5.5-enum / D5.3-designator: evaluate a CST expression to
// a compile-time integer constant via the shared CST const-eval engine
// (plan 12.5 §0.2 D6). Replaces the hand-rolled "linear single-child
// peel + literal leaf" check that previously sat here and at two other
// sites. Now folds `[1+1]`, `(2*4)`, `1 << 3`, `cond ? a : b`, etc.
// Returns nullopt for any expression that doesn't fold; the caller maps
// to its language-specific `S_NonConstant*` diagnostic.
// Resolve a constant symbol's defining init-expression CST. Walks the
// scope chain from `fromScope` looking for `name`; returns the init
// expression CST ONLY when the bound symbol is `isConst` (mutable
// symbols can change at runtime — not foldable). The DeclarationRule's
// `initChild` (already config-driven) gives the visible-child index
// where the init expression lives.
[[nodiscard]] std::optional<CstResolvedSymbol>
resolveConstSymbolInit(EngineState const& s, Tree const& tree,
                       SemanticConfig const& cfg,
                       ScopeId fromScope, std::string_view name) {
    SymbolId const sym = s.scopes.lookup(fromScope, name);
    if (!sym.valid()) return std::nullopt;
    auto const& rec = s.symbols.at(sym);
    if (!rec.isConst) return std::nullopt;
    if (rec.tree.v != tree.id().v) return std::nullopt;
    auto declIt = s.idx().declByRule.find(tree.rule(rec.declRuleNode).v);
    if (declIt == s.idx().declByRule.end()) return std::nullopt;
    auto initExpr = findInitExprInDecl(tree, cfg.declarations[declIt->second],
                                       rec.declRuleNode, rec.declNode);
    if (!initExpr.has_value()) return std::nullopt;
    // D7: return the SYMBOL's defining scope so the engine evaluates
    // its init expression in the scope where the symbol was declared
    // — not the original use-site scope. This is what makes shadowing
    // work correctly across const ref chains.
    return CstResolvedSymbol{*initExpr, rec.scope.v};
}

[[nodiscard]] std::optional<std::int64_t>
constIntExpr(EngineState& s, Tree const& tree, NodeId node,
             ScopeId fromScope = {}, SemanticConfig const* cfg = nullptr) {
    if (!node.valid()) return std::nullopt;
    auto intLits = integerLiteralTokenSet(s);
    CstEvalContext ctx{tree, tree.schema(), intLits, s.idx().numberStyle};
    CstEvalEnvironment env;
    if (fromScope.valid() && cfg != nullptr) {
        // The resolver receives the CURRENT scope context (initially
        // `fromScope`, but updated as the engine recurses into other
        // symbols' init expressions). It looks up the identifier
        // from that scope, returning the resolved symbol's init AND
        // its declaring scope so the engine carries the right
        // context into the recursive evaluation.
        env.resolveSymbolInit = [&s, &tree, cfg](NodeId identTok, std::uint32_t curScopeOpaque)
            -> std::optional<CstResolvedSymbol> {
            return resolveConstSymbolInit(s, tree, *cfg, ScopeId{curScopeOpaque},
                                          tree.text(identTok));
        };
        // FC6: fold `sizeof(T)` in an array dimension (`int a[sizeof(T)]`). The
        // engine dispatches the `sizeofRule` node here (ahead of its wrapper-
        // peel); this closure resolves the SIZED type and sizes it through the
        // same `computeLayout` engine MIR uses. The TYPE form (`sizeof(T)`)
        // resolves its `castTypeRef` directly (type resolution does not depend on
        // expression typing, so it works at this Pass-1.5 declaration-type
        // stage); the VALUE form (`sizeof e`) reads the operand's resolved type,
        // which is NOT yet available here (expression types are stamped in Pass
        // 2) → `nullopt` → the array length fails loud (S_NonConstantArrayLength)
        // rather than a wrong size. `nullopt` when the target declared no layout
        // params, too — never a guessed size.
        env.resolveSizeof = [&s, &tree, cfg, fromScope](NodeId sizeofNode)
            -> std::optional<std::uint64_t> {
            if (!s.aggregateLayout.has_value()) return std::nullopt;
            TypeId sized{};
            for (NodeId form : visibleChildren(tree, sizeofNode)) {
                if (tree.kind(form) != NodeKind::Internal) continue;
                RuleId const fr = tree.rule(form);
                if (cfg->sizeofTypeRule.valid() && fr.v == cfg->sizeofTypeRule.v) {
                    auto fk = visibleChildren(tree, form);
                    if (cfg->sizeofTypeChild < fk.size())
                        // emitOnMiss=false: an unknown sized-type yields nullopt
                        // → the array length fails loud with ONE positioned
                        // S_NonConstantArrayLength (the caller owns it), not a
                        // redundant S_UnknownType + S_NonConstantArrayLength pair.
                        sized = resolveTypeNode(s, *cfg, tree,
                                                fk[cfg->sizeofTypeChild], fromScope,
                                                /*emitOnMiss=*/false);
                } else if (cfg->sizeofValueRule.valid()
                           && fr.v == cfg->sizeofValueRule.v) {
                    for (NodeId opnd : visibleChildren(tree, form)) {
                        if (tree.kind(opnd) == NodeKind::Internal) {
                            // FC6 c-subtreeType: pass the const-context scope so
                            // a Pass-1.5 `sizeof e` whose operand is an
                            // identifier (`sizeof b` / `sizeof(b[0])`) resolves
                            // its leaf type by scope-lookup (leaves are not yet
                            // Pass-2-stamped at this declaration-type stage).
                            sized = subtreeType(s, tree, opnd, fromScope);
                            break;
                        }
                    }
                }
                break;  // the single sizeof form
            }
            if (!sized.valid()) return std::nullopt;
            auto const layout = computeLayout(sized, s.lattice.interner(),
                                              *s.aggregateLayout, s.dataModel);
            if (!layout) return std::nullopt;
            return layout->size;
        };
    }
    ConstEvalResult const r = evaluateConstantCst(node, ctx, env, {}, fromScope.v);
    if (!r.value.has_value()) return std::nullopt;
    return asInt64Bridge(*r.value);
}

// SE-arrays: if `decl` configures an array declarator suffix and a node of that
// suffix rule appears among the declaration's visible children, wrap `base` as
// Array<base, length>. A present-but-non-constant (or absent) length emits
// S_NonConstantArrayLength and returns InvalidType so the symbol's type stays
// unresolved (downstream fails loud rather than silently using the element
// type or decaying to a pointer). No suffix present ⇒ returns `base` unchanged.
[[nodiscard]] TypeId
applyArraySuffix(EngineState& s, Tree const& tree, DeclarationRule const& decl,
                 NodeId declNode, TypeId base,
                 ScopeId fromScope = {}, SemanticConfig const* cfg = nullptr) {
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
    // FC6 (FAM): an ABSENT length (`T x[]` — the length slot is empty, so it
    // resolves to the closing-bracket token, not an expression) on a declaration
    // form that `allowFlexibleArray` (a struct field) is a flexible/incomplete
    // array (C99 §6.7.2.1). It is an INCOMPLETE type, legal only as a struct's
    // LAST field; the type_layout engine lays a FAM struct out correctly and
    // fails loud on a non-last FAM. A standalone `T x[]` (a local/global — NOT
    // `allowFlexibleArray`) keeps the `S_NonConstantArrayLength` error below.
    // Distinct from a present-but-non-constant length (`T x[n]`), always an error.
    bool const absentLength =
        !lenNode.valid() || tree.kind(lenNode) != NodeKind::Internal;
    if (absentLength && decl.allowFlexibleArray) {
        return s.lattice.interner().incompleteArray(base);
    }
    auto len = constIntExpr(s, tree, lenNode, fromScope, cfg);
    if (!len.has_value()) { emit(DiagnosticCode::S_NonConstantArrayLength); return InvalidType; }
    // C99 §6.7.5.2: array length is a positive integer constant
    // expression. Negative folds (e.g. `int a[-1]`) and zero are
    // out-of-range; fail loud rather than wrap to a giant unsigned.
    if (*len <= 0) {
        emit(DiagnosticCode::S_ArrayLengthOutOfRange);
        return InvalidType;
    }
    // FC6 (C99 §6.7.2.1p18): a FAM-bearing struct may not be an array element
    // (`struct F arr[3];` where F has a flexible array member). A bare FAM
    // struct as a complete object (`struct F f;`) stays valid — only the array
    // ELEMENT is rejected. Mirrors the struct-composition in-aggregate check.
    if (typeContainsFlexibleArray(s.lattice.interner(), base)) {
        emit(DiagnosticCode::S_FlexibleArrayInAggregate);
        return InvalidType;
    }
    return s.lattice.interner().array(base, *len);
}

// ── FC4 c1: the declarator-inversion engine (M3) ────────────────────────────
//
// A declarator-mode DeclarationRule splits a declaration's type across the
// type-specifier HEAD (base type only) and a recursive DECLARATOR wrapping
// the name (C 6.7.6). The verified inversion, for one declarator `d` over
// the head type `T`:
//
//   declared(d, T):
//     T1 = Ptr^k(T)          — one Ptr per pointerLayer child of `d`;
//     T2 = fold the direct's suffixes RIGHT-to-LEFT over T1 — the FIRST
//          suffix in source order is the OUTERMOST type constructor:
//            `x[2][3]`   → Array2<Array3<T>>   (s2 applied first, s1 wraps)
//            `*fp(int)`  → FnSig([int] → Ptr<T>)  (stars bind first, the
//                          suffix wraps the star-applied type)
//     then bind: a nameToken direct binds the name : T2; a group direct
//     RECURSES into the parenthesized declarator with T2 as its base —
//            `(*fp)(int)` → the fn suffix builds FnSig BEFORE the descent,
//                          the inner star then yields Ptr<FnSig> at `fp`.
//     An ABSTRACT direct (no name, no group) — or a star-only declarator
//     with no direct — declares T2/T1 itself (a parameter type).
//
// Suffix meanings (config-resolved roles, never rule names):
//   fnSuffixRule    → FnSig(paramTypes, T) — params harvested from the
//                     suffix's `fnSuffixParamsRule` child: each child that
//                     IS a declaration-rule node yields its computed type
//                     via the SAME machinery (recursive; abstract params
//                     have no name but their type still computes; order =
//                     source order, deterministic).
//   arraySuffixRule → Array<T, n> via the existing array semantics — the
//                     length is the suffix's first visible Internal child
//                     folded by the shared CST const-eval; a missing /
//                     non-constant length is S_NonConstantArrayLength and
//                     a non-positive one S_ArrayLengthOutOfRange, exactly
//                     like the legacy ArraySuffix facet (never a silent
//                     decay).
//
// `emitOnMiss` keeps diagnostics exactly-once: a declaration row's OWN
// Pass 1.5 visit folds with emission ON (named AND abstract declarators —
// the definitive diagnostic site); the fn-suffix param HARVEST re-computes
// types with emission OFF (the param rows already diagnosed themselves,
// post-order runs them first).

[[nodiscard]] TypeId
declaratorDeclaredType(EngineState& s, SemanticConfig const& cfg,
                       Tree const& tree, NodeId node, TypeId base,
                       ScopeId scope, bool emitOnMiss);

// The declared type of ONE declaration-row node — the fn-suffix param
// harvest's per-param resolution. Legacy rows resolve their `typeChild`
// (+ legacy array suffix, mirroring `collectParamTypes`); declarator-mode
// rows resolve the head then fold the single `declaratorChild` (absent in
// the tree ⇒ a type-only param — the head itself). A list-mode row has no
// single type and yields InvalidType (a config-shape mismatch the FnSig
// then surfaces as an unresolved param type — never a guessed one).
[[nodiscard]] TypeId
declRowDeclaredType(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                    NodeId declNode, ScopeId scope, bool emitOnMiss) {
    auto declIt = s.idx().declByRule.find(tree.rule(declNode).v);
    if (declIt == s.idx().declByRule.end()) return InvalidType;
    auto const& decl = cfg.declarations[declIt->second];
    auto kids = declRoleChildren(tree, declNode, decl);
    if (decl.isDeclaratorMode()) {
        if (!cfg.declarators.has_value()) return InvalidType;  // loader invariant
        TypeId head = InvalidType;
        if (decl.headChild.has_value() && *decl.headChild < kids.size()) {
            head = resolveTypeNode(s, cfg, tree, kids[*decl.headChild], scope,
                                   emitOnMiss);
        }
        if (decl.declaratorChild.has_value()) {
            if (*decl.declaratorChild < kids.size()) {
                return declaratorDeclaredType(
                    s, cfg, tree, kids[*decl.declaratorChild], head, scope,
                    emitOnMiss);
            }
            return head;   // declarator structurally absent — type-only param
        }
        return InvalidType;   // a LIST row has no single param type
    }
    if (decl.typeChild.has_value() && *decl.typeChild < kids.size()) {
        TypeId pty = resolveTypeNode(s, cfg, tree, kids[*decl.typeChild], scope,
                                     emitOnMiss);
        pty = applyArraySuffix(s, tree, decl, declNode, pty, scope, &cfg);
        return pty;
    }
    return InvalidType;
}

// Apply ONE declarator suffix (fn / array per the config roles) to the
// type accumulated so far. See the inversion comment above for ordering.
[[nodiscard]] TypeId
applyDeclaratorSuffix(EngineState& s, SemanticConfig const& cfg,
                      Tree const& tree, NodeId suffix, TypeId inner,
                      ScopeId scope, bool emitOnMiss) {
    DeclaratorConfig const& dc = *cfg.declarators;
    RuleId const r = tree.rule(suffix);
    if (r == dc.fnSuffixRule) {
        // The param harvest is the SHARED `collectParamTypes` walker (one
        // chokepoint with the legacy function-decl path): it descends
        // wrapper rules (c-subset's `paramOrEllipsis` alt wrapper),
        // resolves each declaration-rule node via `declRowDeclaredType`
        // (declarator-mode aware), and stops at each param (a nested
        // fn-ptr param's inner params never leak into THIS signature).
        // emitOnMiss threads through so a harvest re-resolution never
        // re-reports what the param row's own visit already diagnosed.
        std::vector<std::pair<NodeId, TypeId>> params;
        if (dc.fnSuffixParamsRule.has_value()) {
            NodeId paramsList{};
            for (NodeId c : visibleChildren(tree, suffix)) {
                if (tree.kind(c) == NodeKind::Internal
                    && tree.rule(c) == *dc.fnSuffixParamsRule) {
                    paramsList = c;
                    break;
                }
            }
            if (paramsList.valid()) {
                collectParamTypes(s, cfg, tree, paramsList, scope, params,
                                  emitOnMiss);
            }
        }
        // C 6.7.6.3p10 `(void)` normalization — the same call the legacy
        // function-decl FnSig build applies (ONE convention, two paths).
        normalizeSoleVoidParams(s, cfg, tree, params, emitOnMiss);
        std::vector<TypeId> paramTypes;
        paramTypes.reserve(params.size());
        for (auto const& [pNode, pTy] : params) paramTypes.push_back(pTy);
        // CcSysV is the canonical MIR-tier placeholder — the SAME cc source
        // every interner `fnSig()` call site uses pre-ML7 (the function-decl
        // path above, builtins, moduleInit); ML7's calling-convention pass
        // applies the target's real convention at materialize time.
        return s.lattice.interner().fnSig(paramTypes, inner, CallConv::CcSysV);
    }
    if (r == dc.arraySuffixRule) {
        auto const emit = [&](DiagnosticCode code) {
            if (!emitOnMiss) return;
            ParseDiagnostic d;
            d.code     = code;
            d.severity = DiagnosticSeverity::Error;
            d.buffer   = tree.source().id();
            d.span     = tree.span(suffix);
            d.actual   = std::string{tree.text(suffix)};
            s.reporter.report(std::move(d));
        };
        // The length is whichever of the suffix's visible children
        // CONSTANT-FOLDS (`[ n ]` — a bare literal TOKEN or an expression
        // rule; the bracket tokens fold to nothing, so the first foldable
        // child IS the length — no per-language child index needed). Same
        // validation as the legacy ArraySuffix facet: missing /
        // non-constant fails loud, non-positive is out of range — never a
        // silent pointer decay.
        std::optional<std::int64_t> len;
        for (NodeId c : visibleChildren(tree, suffix)) {
            len = constIntExpr(s, tree, c, scope, &cfg);
            if (len.has_value()) break;
        }
        if (!len.has_value()) {
            emit(DiagnosticCode::S_NonConstantArrayLength);
            return InvalidType;
        }
        if (*len <= 0) {
            emit(DiagnosticCode::S_ArrayLengthOutOfRange);
            return InvalidType;
        }
        // FC6 (C99 §6.7.2.1p18): a FAM-bearing element type may not be an
        // array element (the declarator-mode twin of the `applyArraySuffix`
        // check — both array-forming sites reject it, by construction).
        if (typeContainsFlexibleArray(s.lattice.interner(), inner)) {
            emit(DiagnosticCode::S_FlexibleArrayInAggregate);
            return InvalidType;
        }
        return s.lattice.interner().array(inner, *len);
    }
    return InvalidType;   // caller filters to the two suffix roles
}

TypeId declaratorDeclaredType(EngineState& s, SemanticConfig const& cfg,
                              Tree const& tree, NodeId node, TypeId base,
                              ScopeId scope, bool emitOnMiss) {
    if (!cfg.declarators.has_value()) return InvalidType;
    DeclaratorConfig const& dc = *cfg.declarators;
    if (!node.valid() || !base.valid()) return InvalidType;
    if (tree.kind(node) != NodeKind::Internal) return InvalidType;
    RuleId const r = tree.rule(node);
    if (r == dc.initDeclaratorRule) {
        NodeId const inner = declarator_walk_detail::firstChildOfRule(
            TreeDeclaratorView{tree}, node, dc.declaratorRule);
        if (!inner.valid()) return InvalidType;
        return declaratorDeclaredType(s, cfg, tree, inner, base, scope,
                                      emitOnMiss);
    }
    if (r != dc.declaratorRule) return InvalidType;

    // Pointer layers bind FIRST (innermost): T1 = Ptr^k(base).
    TypeId t = base;
    NodeId direct{};
    for (NodeId c : visibleChildren(tree, node)) {
        if (tree.kind(c) != NodeKind::Internal) continue;
        RuleId const cr = tree.rule(c);
        if (cr == dc.pointerLayerRule) {
            t = s.lattice.interner().pointer(t);
            continue;
        }
        if (cr == dc.directRule && !direct.valid()) direct = c;
    }
    if (!direct.valid()) return t;   // star-only abstract declarator

    // The direct's base (name token or group) + suffixes, one scan.
    NodeId nameTok{};
    NodeId group{};
    std::vector<NodeId> suffixes;
    for (NodeId c : visibleChildren(tree, direct)) {
        if (tree.kind(c) == NodeKind::Token) {
            if (tree.tokenKind(c) == dc.nameToken && !nameTok.valid()) {
                nameTok = c;
            }
            continue;
        }
        RuleId const cr = tree.rule(c);
        if (cr == dc.fnSuffixRule || cr == dc.arraySuffixRule) {
            suffixes.push_back(c);
            continue;
        }
        if (cr == dc.groupRule && !group.valid()) group = c;
    }

    // Suffixes fold RIGHT-to-LEFT (source-first suffix = outermost type).
    for (std::size_t i = suffixes.size(); i-- > 0;) {
        t = applyDeclaratorSuffix(s, cfg, tree, suffixes[i], t, scope,
                                  emitOnMiss);
        if (!t.valid()) return InvalidType;
    }

    if (nameTok.valid()) return t;   // bound at the name
    if (group.valid()) {
        NodeId const inner = declarator_walk_detail::firstChildOfRule(
            TreeDeclaratorView{tree}, group, dc.declaratorRule);
        if (!inner.valid()) return InvalidType;   // malformed group
        return declaratorDeclaredType(s, cfg, tree, inner, t, scope,
                                      emitOnMiss);
    }
    return t;   // abstract direct — the type itself
}

// FC4 c1 (M5): first token of `kind` in `node`'s subtree in SOURCE order —
// the positioned gate site. Same nested-decl-rule descent stop as
// `subtreeContainsToken` (each nested decl row runs its own gate scan, so
// stopping prevents a double-fire on the same token).
[[nodiscard]] NodeId
findTokenInSubtree(Tree const& tree, NodeId node, SchemaTokenId kind,
                   std::unordered_map<std::uint32_t, std::size_t> const*
                       declByRule) {
    if (!node.valid() || !kind.valid()) return {};
    std::vector<NodeId> stack{node};
    bool firstPop = true;
    while (!stack.empty()) {
        NodeId cur = stack.back();
        stack.pop_back();
        if (tree.kind(cur) == NodeKind::Token && tree.tokenKind(cur) == kind) {
            return cur;
        }
        if (!firstPop && declByRule != nullptr
            && tree.kind(cur) == NodeKind::Internal
            && declByRule->contains(tree.rule(cur).v)) {
            continue;
        }
        firstPop = false;
        auto const cs = tree.children(cur);
        // Reverse-push so pop order is left-to-right (source order — the
        // FIRST occurrence positions the diagnostic).
        for (auto it = cs.rbegin(); it != cs.rend(); ++it) {
            if (!isEmptySpace(tree.flags(*it))) stack.push_back(*it);
        }
    }
    return {};
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
            auto kids = declRoleChildren(tree, node, decl);

            // FC4 c1 (M5): config-driven fail-loud marker gates — each
            // declared (token → diagnostic) pair fires when the token
            // appears in THIS declaration's subtree, positioned at its
            // first occurrence. Runs for declarator-mode AND legacy rows
            // alike; the scan stops at nested decl rows (they run their
            // own gates), so one use site fires exactly once.
            for (auto const& gate : decl.gatedMarkers) {
                NodeId const hit = findTokenInSubtree(
                    tree, node, gate.token, &s.idx().declByRule);
                if (!hit.valid()) continue;
                ParseDiagnostic d;
                d.code     = gate.code;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = tree.source().id();
                d.span     = tree.span(hit);
                d.actual   = std::string{tree.text(hit)};
                s.reporter.report(std::move(d));
            }

            // Evaluate the kindByChild discriminator (if any) BEFORE
            // minting the symbol so its `kind` reflects the structural
            // choice in the tree (e.g. c-subset's topLevelDecl with a
            // funcDefTail descendant mints a Function, not a Variable).
            DeclarationKind effectiveKind = decl.kind;
            if (decl.kindByChild.has_value()) {
                auto const& disc = *decl.kindByChild;
                NodeId const disChild = descendVisibleDecl(tree, node, disc.childPath, decl);
                if (disChild.valid()
                    && tree.kind(disChild) == NodeKind::Internal
                    && tree.rule(disChild) == disc.whenRule) {
                    effectiveKind = disc.whenKind;
                }
            }

            // FC4 c1 (M3): declarator-mode rows mint ONE symbol PER named
            // declarator below the list/single carrier child — `int *p, q;`
            // mints p and q (the SHARED walk extracts each name; abstract
            // declarators mint nothing, a legal outcome). Each symbol binds
            // into the ENCLOSING scope exactly like the legacy path; the
            // const marker has no typeChild home in this mode (it lives in
            // the head / pointer layers), so the scan covers the whole decl
            // subtree — the legacy no-typeChild fallback scope.
            if (decl.isDeclaratorMode() && cfg.declarators.has_value()) {
                auto const carrierIdx = decl.declaratorListChild.has_value()
                                            ? decl.declaratorListChild
                                            : decl.declaratorChild;
                if (carrierIdx.has_value() && *carrierIdx < kids.size()) {
                    std::vector<NodeId> declarators;
                    collectDeclarators(tree, kids[*carrierIdx],
                                       *cfg.declarators, declarators);
                    for (NodeId dNode : declarators) {
                        NodeId const nameNode = declaratorNameNode(
                            tree, dNode, *cfg.declarators);
                        if (!nameNode.valid()) {
                            // FC4 c1 stage 2a: an ABSTRACT declarator. Legal
                            // in parameter-like positions; a named position
                            // (requireNamedDeclarators — C locals/globals/
                            // typedefs) rejects it LOUD: `int *;` declares
                            // nothing and must not silently no-op.
                            if (decl.requireNamedDeclarators) {
                                ParseDiagnostic d;
                                d.code = DiagnosticCode::
                                    S_DeclarationDeclaresNothing;
                                d.severity = DiagnosticSeverity::Error;
                                d.buffer   = tree.source().id();
                                d.span     = tree.span(dNode);
                                d.actual   = std::string{tree.text(dNode)};
                                s.reporter.report(std::move(d));
                            }
                            continue;   // abstract — mints nothing
                        }
                        std::string name{tree.text(nameNode)};
                        if (name.empty()) continue;
                        ScopeId const bindScope = current;
                        SymbolRecord rec;
                        rec.name         = name;
                        rec.scope        = bindScope;
                        rec.declNode     = nameNode;
                        rec.declRuleNode = node;
                        rec.tree         = tree.id();
                        rec.kind         = effectiveKind;
                        rec.warnIfUnused = decl.warnIfUnused;
                        rec.fieldIndex   = static_cast<std::uint32_t>(
                            s.scopes.scopes()[bindScope.v].bindings.size());
                        if (decl.constMarker.has_value()) {
                            rec.isConst = subtreeContainsToken(
                                tree, node, *decl.constMarker,
                                &s.idx().declByRule);
                        }
                        SymbolId const newId = s.symbols.mint(rec);
                        SymbolId const prior =
                            s.scopes.bind(bindScope, name, newId);
                        if (prior.valid()) {
                            ParseDiagnostic d;
                            d.code     = DiagnosticCode::S_RedeclaredSymbol;
                            d.severity = DiagnosticSeverity::Error;
                            d.buffer   = tree.source().id();
                            d.span     = tree.span(nameNode);
                            d.actual   = name;
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
                            s.nodeToSymbol.set(nameNode, newId);
                        }
                    }
                }
            } else if (decl.nameChild.has_value() && *decl.nameChild < kids.size()) {
                NodeId const nameContainer = kids[*decl.nameChild];
                auto resolved = extractNameNode(
                    tree, nameContainer, decl.nameMatch, cfg.identifierToken,
                    cfg.bracketIdentifierToken);
                // FC4 c1 stage 2a (anonymousNameAllowed): an OPTIONAL name in
                // the grammar shifts the positional name child onto whatever
                // follows (C's anonymous `typedef struct { ... } T;` puts the
                // body's `{` at the tag's index). For flagged rows, a resolved
                // node that is NOT an identifier leaf means ANONYMOUS: mint
                // under a synthesized unique name, anchored on the DECL node
                // itself (the type-position resolver looks the symbol up
                // there). Unflagged rows keep the legacy behavior exactly.
                if (decl.anonymousNameAllowed) {
                    std::string leafText;
                    bool const isIdentLeaf = resolved.node.valid()
                        && nameLeafText(tree, resolved.node,
                                        cfg.identifierToken,
                                        cfg.bracketIdentifierToken, leafText);
                    if (!isIdentLeaf) {
                        resolved.name = std::format("<anon:{}:{}>",
                                                    decl.ruleName, node.v);
                        resolved.node = node;
                    }
                }
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
                            tree, scanRoot, *decl.constMarker,
                            &s.idx().declByRule);
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
            auto kids = declRoleChildren(tree, node, decl);
            // FC4 c1 (M3): declarator-mode rows — resolve the HEAD once via
            // the standard type-position resolver (it carries NO stars; the
            // declarator owns all type structure), then fold EVERY
            // declarator's declared type via the inversion (see
            // declaratorDeclaredType). Named declarators get their Pass-1
            // symbol's type set; abstract ones still fold so their
            // diagnostics (bad array length, unresolvable nested param
            // head) fire here, at the row's own definitive visit.
            if (decl.isDeclaratorMode() && cfg.declarators.has_value()) {
                TypeId headTy = InvalidType;
                if (decl.headChild.has_value() && *decl.headChild < kids.size()) {
                    headTy = resolveTypeNode(
                        s, cfg, tree, kids[*decl.headChild], here);
                }
                // kindByChild discriminator (mirrors the legacy arm): a
                // matched Function discriminator makes this a function
                // DEFINITION — the body is the matched node itself when
                // `bodyPath` is empty (the declarator-mode convention:
                // params live inside the declarator's fn suffix, the
                // matched block IS the body).
                bool isFunctionForm = false;
                NodeId bodyNode{};
                if (decl.kindByChild.has_value()) {
                    auto const& disc = *decl.kindByChild;
                    NodeId const disChild =
                        descendVisibleDecl(tree, node, disc.childPath, decl);
                    if (disChild.valid()
                        && tree.kind(disChild) == NodeKind::Internal
                        && tree.rule(disChild) == disc.whenRule
                        && disc.whenKind == DeclarationKind::Function) {
                        isFunctionForm = true;
                        bodyNode = disc.bodyPath.empty()
                            ? disChild
                            : descendVisible(tree, disChild, disc.bodyPath);
                    }
                }
                auto const carrierIdx = decl.declaratorListChild.has_value()
                                            ? decl.declaratorListChild
                                            : decl.declaratorChild;
                // SINGLE-declarator rows (param-like) with the declarator
                // structurally ABSENT — a type-only parameter (`int f(int)`):
                // the head IS the declared type; stamp it on the row node so
                // the HIR lowering can type the (nameless) param slot.
                if (decl.declaratorChild.has_value()
                    && (!carrierIdx.has_value() || *carrierIdx >= kids.size())
                    && headTy.valid()) {
                    s.nodeToType.set(node, headTy);
                }
                if (carrierIdx.has_value() && *carrierIdx < kids.size()) {
                    std::vector<NodeId> declarators;
                    collectDeclarators(tree, kids[*carrierIdx],
                                       *cfg.declarators, declarators);
                    auto const emitInvalidFn = [&](NodeId at,
                                                   std::string detail) {
                        ParseDiagnostic d;
                        d.code = DiagnosticCode::S_InvalidFunctionDeclarator;
                        d.severity = DiagnosticSeverity::Error;
                        d.buffer   = tree.source().id();
                        d.span     = tree.span(at);
                        d.actual   = std::move(detail);
                        s.reporter.report(std::move(d));
                    };
                    // C 6.9.1: a function DEFINITION has exactly ONE
                    // declarator (`int a, main() { }` is ill-formed).
                    if (isFunctionForm && declarators.size() != 1) {
                        emitInvalidFn(node,
                                      "a function definition declares "
                                      "exactly one declarator");
                    }
                    for (NodeId dNode : declarators) {
                        TypeId const declTy = declaratorDeclaredType(
                            s, cfg, tree, dNode, headTy, here,
                            /*emitOnMiss=*/true);
                        // SINGLE-declarator rows (param-like): also stamp
                        // the row node — an ABSTRACT param has no name
                        // node to carry the type, but its slot still
                        // exists and the HIR lowering needs its type.
                        if (decl.declaratorChild.has_value()
                            && declTy.valid()) {
                            s.nodeToType.set(node, declTy);
                        }
                        NodeId const nameNode = declaratorNameNode(
                            tree, dNode, *cfg.declarators);
                        if (!nameNode.valid()) continue;   // abstract
                        SymbolId const sym = s.symbolAtOr(nameNode);
                        if (!sym.valid()) continue;   // redeclared-error path
                        if (declTy.valid()) {
                            s.symbols.at(sym).type = declTy;
                            s.nodeToType.set(nameNode, declTy);
                        }
                        bool const isFnSig = declTy.valid()
                            && s.lattice.interner().kind(declTy)
                                   == TypeKind::FnSig;
                        if (isFunctionForm && declarators.size() == 1) {
                            // C 6.9.1 definition constraints, checked LOUD:
                            //  * the named direct-declarator must carry a
                            //    function suffix (`int (*fp)(int) { }` and
                            //    `int x { }` are not function declarators);
                            //  * no initializer on the init-declarator
                            //    (`int main() = 5 { }` parses; reject here).
                            NodeId const direct = tree.parent(nameNode);
                            bool fnSuffixOnName = false;
                            if (direct.valid()
                                && tree.kind(direct) == NodeKind::Internal
                                && tree.rule(direct)
                                       == cfg.declarators->directRule) {
                                for (NodeId c : visibleChildren(tree, direct)) {
                                    if (tree.kind(c) == NodeKind::Internal
                                        && tree.rule(c)
                                               == cfg.declarators->fnSuffixRule) {
                                        fnSuffixOnName = true;
                                        break;
                                    }
                                }
                            }
                            if (!fnSuffixOnName) {
                                emitInvalidFn(nameNode,
                                              "a function definition's "
                                              "declarator must be a function "
                                              "declarator (name followed by "
                                              "a parameter list)");
                            }
                            if (tree.rule(dNode)
                                    == cfg.declarators->initDeclaratorRule) {
                                std::size_t internals = 0;
                                for (NodeId c : visibleChildren(tree, dNode)) {
                                    if (tree.kind(c) == NodeKind::Internal)
                                        ++internals;
                                }
                                if (internals > 1) {
                                    emitInvalidFn(
                                        dNode,
                                        "a function definition cannot carry "
                                        "an initializer");
                                }
                            }
                            // GAP A: the function's RESULT type, keyed on
                            // the scope its body opens (the return-check
                            // walks the scope chain to here).
                            if (isFnSig && bodyNode.valid()) {
                                ScopeId const bodyScope =
                                    scopeAnchoredAt(s, tree, bodyNode);
                                if (bodyScope.valid()) {
                                    s.fnResultByScope[bodyScope.v] =
                                        s.lattice.interner().fnResult(declTy);
                                }
                            }
                        } else if (isFnSig
                                   && s.symbols.at(sym).kind
                                          == DeclarationKind::Variable) {
                            // A bare function-TYPED object declaration — a C
                            // prototype (`int f();`). Declaration-without-
                            // definition is NOT wired (externs carry that
                            // role); a silent FnSig-typed data global would
                            // miscompile, so fail loud. Pinned residue:
                            // D-CSUBSET-FN-PROTOTYPE (stage 2b registry).
                            emitInvalidFn(nameNode,
                                          "function prototype declarations "
                                          "are not supported here; use "
                                          "'extern' for cross-unit "
                                          "declarations");
                        }
                    }
                }
            } else if (decl.nameChild.has_value() && *decl.nameChild < kids.size()) {
                auto resolved = extractNameNode(
                    tree, kids[*decl.nameChild], decl.nameMatch, cfg.identifierToken,
                    cfg.bracketIdentifierToken);
                // FC4 c1 (anonymousNameAllowed): mirror Pass 1's anonymous
                // re-anchor — the symbol lives on the DECL node when the
                // name child resolved to a non-identifier leaf.
                if (decl.anonymousNameAllowed) {
                    std::string leafText;
                    bool const isIdentLeaf = resolved.node.valid()
                        && nameLeafText(tree, resolved.node,
                                        cfg.identifierToken,
                                        cfg.bracketIdentifierToken, leafText);
                    if (!isIdentLeaf) resolved.node = node;
                }
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
                            descendVisibleDecl(tree, node, disc.childPath, decl);
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
                            // FC6: carry each field's decl node (its span) — the
                            // FAM-constraint check positions diagnostics at the
                            // offending field, and `scope.bindings` is unordered
                            // (so the post-sort type list alone can't locate it).
                            struct FieldEntry {
                                std::uint32_t index; TypeId type; NodeId declNode;
                            };
                            std::vector<FieldEntry> fields;
                            auto const& scope = s.scopes.scopes()[srec.structScope.v];
                            bool anyInvalid = false;
                            // D5.5: enum members have NO declared type
                            // (they're typed BY the enum type Pass 1.5
                            // is about to mint); skip the "field's type
                            // must be resolved" gate for enum.
                            bool const requireFieldTypes =
                                decl.fieldChildren->compositeKind
                                != CompositeKind::Enum;
                            for (auto const& [_name, fieldSymId] : scope.bindings) {
                                SymbolRecord const& frec = s.symbols.at(fieldSymId);
                                if (!frec.declRuleNode.valid()
                                    || frec.tree.v != tree.id().v) continue;
                                if (tree.rule(frec.declRuleNode)
                                    != decl.fieldChildren->rule) continue;
                                if (requireFieldTypes && !frec.type.valid()) {
                                    anyInvalid = true;
                                    break;
                                }
                                fields.push_back({frec.fieldIndex, frec.type,
                                                  frec.declRuleNode});
                            }
                            if (!anyInvalid) {
                                std::sort(fields.begin(), fields.end(),
                                          [](FieldEntry const& a, FieldEntry const& b) {
                                              return a.index < b.index;
                                          });
                                std::vector<TypeId> fieldTypes;
                                fieldTypes.reserve(fields.size());
                                for (auto const& fe : fields)
                                    fieldTypes.push_back(fe.type);
                                // D5.4 / D5.5: struct vs union vs enum
                                // dispatch is config-driven via
                                // FieldChildrenDescriptor::compositeKind.
                                CompositeKind const ck =
                                    decl.fieldChildren->compositeKind;
                                // FC6: flexible-array-member constraints (C99
                                // §6.7.2.1), positioned at the offending field.
                                // The not-last / sole-member checks are
                                // struct-only (a bare FAM in a union is rejected
                                // earlier — the `allowFlexibleArray=false` gate on
                                // `unionField` → S_NonConstantArrayLength); but the
                                // EMBEDDED-FAM-struct check (p18) runs for unions
                                // too, since a FAM-bearing struct may not be a
                                // member of a struct OR a union (gcc/clang reject
                                // both). Enums carry no field types. This is the
                                // positioned SEMANTIC surface; the layout engine's
                                // non-last-FAM nullopt is the backstop.
                                if (ck == CompositeKind::Struct
                                    || ck == CompositeKind::Union) {
                                    TypeInterner const& in = s.lattice.interner();
                                    for (std::size_t i = 0; i < fields.size(); ++i) {
                                        TypeId const ft = fields[i].type;
                                        NodeId const fn = fields[i].declNode;
                                        auto famDiag = [&](DiagnosticCode code) {
                                            ParseDiagnostic d;
                                            d.code     = code;
                                            d.severity = DiagnosticSeverity::Error;
                                            d.buffer   = tree.source().id();
                                            d.span     = tree.span(fn);
                                            d.actual   = std::string{tree.text(fn)};
                                            s.reporter.report(std::move(d));
                                        };
                                        if (in.isIncompleteArray(ft)) {
                                            // A direct FAM (struct only — a bare
                                            // union FAM never reaches here): legal
                                            // ONLY as the last AND non-sole member.
                                            if (i + 1 != fields.size())
                                                famDiag(DiagnosticCode::S_FlexibleArrayNotLast);
                                            else if (fields.size() == 1)
                                                famDiag(DiagnosticCode::S_FlexibleArraySoleMember);
                                        } else if (typeContainsFlexibleArray(in, ft)) {
                                            // A field whose TYPE embeds a FAM (a
                                            // nested FAM struct / array of one) —
                                            // forbidden in a struct OR a union.
                                            famDiag(DiagnosticCode::S_FlexibleArrayInAggregate);
                                        }
                                    }
                                }
                                TypeId compositeTy;
                                if (ck == CompositeKind::Union) {
                                    compositeTy = s.lattice.interner().unionType(
                                        srec.name, fieldTypes);
                                } else if (ck == CompositeKind::Enum) {
                                    // D5.5: enum type carries no field-
                                    // operands — only its nominal name +
                                    // underlying TypeKind (I32 in v1).
                                    // Pass 1 created each enumerator's
                                    // SymbolRecord with type still invalid;
                                    // we now SET each enumerator's type to
                                    // the enum type, COMPUTE its integer
                                    // value (C99 §6.7.2.2: explicit `= N`
                                    // overrides the running counter; missing
                                    // initializer = previous + 1), AND
                                    // also-bind the enumerator name to the
                                    // enclosing scope (NOT a lift — original
                                    // binding stays in the inner scope; this
                                    // is a republication) so C-classic
                                    // `enum E { A } ... A` works.
                                    compositeTy = s.lattice.interner().enumType(
                                        srec.name, TypeKind::I32);
                                    if (srec.structScope.valid()) {
                                        auto const enclosingId =
                                            s.scopes.scopes()[srec.structScope.v].parent;
                                        // Collect enumerators in fieldIndex
                                        // order so the running counter
                                        // matches source declaration order.
                                        std::vector<std::pair<std::uint32_t, SymbolId>> ordered;
                                        for (auto const& [_n, eSymId]
                                             : s.scopes.scopes()[srec.structScope.v].bindings) {
                                            SymbolRecord const& er = s.symbols.at(eSymId);
                                            if (!er.declRuleNode.valid()
                                                || er.tree.v != tree.id().v)
                                                continue;
                                            if (tree.rule(er.declRuleNode)
                                                != decl.fieldChildren->rule)
                                                continue;
                                            ordered.emplace_back(er.fieldIndex, eSymId);
                                        }
                                        std::sort(ordered.begin(), ordered.end());
                                        std::int64_t nextValue = 0;
                                        for (auto const& [_idx, eSymId] : ordered) {
                                            SymbolRecord& erec =
                                                s.symbols.at(eSymId);
                                            erec.type = compositeTy;
                                            // Optional `= expr`: pass through
                                            // the shared CST const-eval engine
                                            // (plan 12.5 §0.2 D6). Folds
                                            // literal arithmetic (`A = 1+1`),
                                            // bitops, comparisons, ternary;
                                            // identifier refs are anchored as
                                            // a real-blocker followup.
                                            std::int64_t value = nextValue;
                                            bool hadExplicit = false;
                                            bool explicitFailed = false;
                                            // D-DECL-SPECIFIER-PREFIX-SUBSTRATE (cycle-13 audit fix):
                                            // resolve the enumerator's `= expr` value through the SHARED
                                            // `findInitExprInDecl` chokepoint (mirrors
                                            // `resolveConstSymbolInit` ~:598) instead of an open-coded
                                            // "first Internal child" scan. The chokepoint STRIPS a leading
                                            // specifier prefix before selecting the value child; the old
                                            // raw scan bypassed it, so an enumerator decl carrying a future
                                            // `static`/`__attribute__` prefix would select the specifier
                                            // subtree as its value (silent-wrong value / phantom
                                            // S_NonConstantEnumeratorValue). `enumerator` is a registered
                                            // decl rule, so the lookup resolves; the defensive branch keeps
                                            // the prior behavior for any enumerator minted by a rule with
                                            // no DeclarationRule (⇒ no specifierPrefixRule possible).
                                            std::optional<NodeId> valueExpr;
                                            if (auto eDeclIt = s.idx().declByRule.find(
                                                    tree.rule(erec.declRuleNode).v);
                                                eDeclIt != s.idx().declByRule.end()) {
                                                valueExpr = findInitExprInDecl(
                                                    tree, cfg.declarations[eDeclIt->second],
                                                    erec.declRuleNode);
                                            } else {
                                                for (NodeId c :
                                                     visibleChildren(tree, erec.declRuleNode))
                                                    if (tree.kind(c) == NodeKind::Internal) {
                                                        valueExpr = c;
                                                        break;
                                                    }
                                            }
                                            if (valueExpr.has_value()) {
                                                hadExplicit = true;
                                                if (auto iv = constIntExpr(
                                                        s, tree, *valueExpr, erec.scope, &cfg);
                                                    iv.has_value()) {
                                                    value = *iv;
                                                } else {
                                                    explicitFailed = true;
                                                }
                                            }
                                            if (hadExplicit && explicitFailed) {
                                                ParseDiagnostic d2;
                                                d2.code = DiagnosticCode::S_NonConstantEnumeratorValue;
                                                d2.severity = DiagnosticSeverity::Error;
                                                d2.buffer = tree.source().id();
                                                d2.span = tree.span(erec.declRuleNode);
                                                d2.actual = erec.name;
                                                s.reporter.report(std::move(d2));
                                            }
                                            erec.enumValue = value;
                                            nextValue = value + 1;
                                            // D5.5-FU2: only also-bind to the
                                            // enclosing scope when the config
                                            // opts in (`liftToEnclosingScope:
                                            // true`). C-style enums opt in;
                                            // future Rust-style `E.A`-only
                                            // schemas would leave it false.
                                            if (enclosingId.valid()
                                             && decl.fieldChildren->liftToEnclosingScope) {
                                                SymbolId const prior =
                                                    s.scopes.bind(
                                                        enclosingId, erec.name, eSymId);
                                                if (prior.valid()) {
                                                    ParseDiagnostic d2;
                                                    d2.code     = DiagnosticCode::S_RedeclaredSymbol;
                                                    d2.severity = DiagnosticSeverity::Error;
                                                    d2.buffer   = tree.source().id();
                                                    d2.span     = tree.span(erec.declRuleNode);
                                                    d2.actual   = erec.name;
                                                    s.reporter.report(std::move(d2));
                                                }
                                            }
                                        }
                                    }
                                } else {
                                    compositeTy = s.lattice.interner().structType(
                                        srec.name, fieldTypes);
                                }
                                srec.type = compositeTy;
                                s.nodeToType.set(resolved.node, compositeTy);
                                s.compositeScopeByType[compositeTy.v] = srec.structScope;
                            }
                        }
                    } else if (effectiveKind == DeclarationKind::Function) {
                        // SE6: build a FnSig over the param types. Each
                        // param row's own visit is the definitive
                        // diagnostic site, so this harvest re-resolution
                        // runs with emitOnMiss=false (one diagnostic per
                        // bad param type, not two). The `(void)`
                        // convention applies at the same chokepoint the
                        // declarator fn-suffix fold uses.
                        std::vector<std::pair<NodeId, TypeId>> params;
                        if (paramsNode.valid()) {
                            collectParamTypes(s, cfg, tree, paramsNode, here,
                                              params, /*emitOnMiss=*/false);
                        }
                        normalizeSoleVoidParams(s, cfg, tree, params,
                                                /*emitOnMiss=*/true);
                        std::vector<TypeId> paramTypes;
                        paramTypes.reserve(params.size());
                        for (auto const& [pNode, pTy] : params) {
                            paramTypes.push_back(pTy);
                        }
                        // D-LANG-VARIADIC (step 13.4): scan the params
                        // subtree for the declaration's configured
                        // variadic-marker token (e.g. `EllipsisOp` for
                        // c-subset). When present, build a variadic
                        // FnSig (scalars=[cc, 1]); otherwise build the
                        // standard non-variadic FnSig (scalars=[cc]).
                        // The walker stops descent at any nested decl-
                        // rule node — a future grammar that nests a
                        // default-value expression inside a `param`
                        // cannot false-match the marker. Closes silent-
                        // failure HIGH-2 (step 13.4 post-fold).
                        bool const isVariadic =
                            paramsNode.valid() && decl.variadicMarker.has_value()
                            && subtreeContainsToken(
                                tree, paramsNode, *decl.variadicMarker,
                                &s.idx().declByRule);
                        // CcSysV is the canonical MIR-tier placeholder
                        // (mirrors `hir_to_mir.cpp:lowerModuleInit`'s
                        // moduleInit FnSig): the target's real
                        // convention is applied by ML7 (`lir_callconv`)
                        // via `cc.name` lookup at materialize time.
                        // Do NOT inspect this CallConv field at MIR
                        // tier — it's a semantic placeholder, not the
                        // load-bearing CC. Anchored as the same
                        // placeholder convention every interner
                        // `fnSig()` callsite uses pre-ML7.
                        TypeId const fnTy = s.lattice.interner().fnSig(
                            paramTypes, returnTy, CallConv::CcSysV,
                            isVariadic);
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
                            applyArraySuffix(s, tree, decl, node, returnTy, here, &cfg);
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
            TypeId litTy = litIt->second;
            // FC3 c1: the integer-literal ladder (C 6.4.4.1). When the
            // language declares `integerLiteralTyping` AND this token is
            // the numberStyle's INTEGER literal kind, the literal's type
            // is the first (suffix-class × radix-class) candidate whose
            // range fits the decoded magnitude — overriding the
            // `literalTypes` base core. Languages without the block (or
            // other literal kinds — float, bool keywords) keep the
            // token-kind map exactly (toy / tsql — pinned).
            if (!cfg.integerLiteralTyping.empty()
                && s.idx().numberStyle != nullptr
                && s.idx().numberStyle->emitKind.integer.valid()
                && tk == s.idx().numberStyle->emitKind.integer) {
                auto const text = tree.text(node);
                auto const magnitude = decodeInteger(text, s.idx().numberStyle);
                bool fits = magnitude.has_value();
                if (fits) {
                    auto const r = typeIntegerLiteral(
                        text, s.idx().numberStyle, cfg.integerLiteralTyping,
                        s.dataModel, *magnitude);
                    switch (r.status) {
                        case IntegerLadderStatus::Typed:
                            litTy = s.lattice.interner().primitive(r.kind);
                            break;
                        case IntegerLadderStatus::TooLarge:
                            fits = false;
                            break;
                        case IntegerLadderStatus::NoRule:
                            // Loader invariant: every numberStyle suffix is
                            // covered by exactly one rule and an unsuffixed
                            // rule exists. A miss here is substrate drift —
                            // fail loud, never silently keep the base core.
                            std::fputs("dss::analyze fatal: integer literal "
                                       "matched no integerLiteralTyping rule "
                                       "(loader cross-check invariant "
                                       "violated)\n", stderr);
                            std::abort();
                    }
                }
                if (!fits) {
                    // Decode-tier overflow (> the u64 accumulator) and
                    // ladder exhaustion (decodable but beyond the last
                    // candidate's range) are the same user-facing fact:
                    // no declared type can hold this literal.
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::S_IntegerLiteralTooLarge;
                    d.severity = DiagnosticSeverity::Error;
                    d.buffer   = tree.source().id();
                    d.span     = tree.span(node);
                    d.actual   = std::string{text};
                    s.reporter.report(std::move(d));
                    // Leave the node untyped (cascade suppression
                    // downstream; the error already fails the compile).
                    return;
                }
            }
            // FC3.5 sweep-c2: float-literal suffix typing (C 6.4.4.2).
            // When the language declares `floatLiteralTyping` AND this
            // token is the numberStyle's FLOAT literal kind, the
            // suffix selects the type (c-subset: `1.5f` → F32, `1.5`
            // → F64) — the SAME shared rule the CST→HIR tier runs.
            // Languages without the block keep the token-kind map
            // exactly (toy / tsql — pinned).
            if (!cfg.floatLiteralTyping.empty()
                && s.idx().numberStyle != nullptr
                && s.idx().numberStyle->emitKind.floating.valid()
                && tk == s.idx().numberStyle->emitKind.floating) {
                auto const fk = typeFloatLiteral(
                    tree.text(node), s.idx().numberStyle,
                    cfg.floatLiteralTyping, s.dataModel);
                if (!fk.has_value()) {
                    // Loader invariant: every numberStyle float suffix
                    // is covered and an unsuffixed rule exists. A miss
                    // here is substrate drift — fail loud, never
                    // silently keep the base core.
                    std::fputs("dss::analyze fatal: float literal "
                               "matched no floatLiteralTyping rule "
                               "(loader cross-check invariant "
                               "violated)\n", stderr);
                    std::abort();
                }
                litTy = s.lattice.interner().primitive(*fk);
            }
            s.nodeToType.set(node, litTy);
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
                    auto kids = declRoleChildren(tree, parent, parentDecl);
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

        // FC2: explicit-cast typing + legality (`semantics.casts`).
        // Post-order means the operand subtree is already typed. The
        // type-position child resolves through the SAME resolver a
        // declaration's typeChild uses (builtins + pointer stars +
        // struct refs + typedef aliases — incl. cross-tree-injected
        // typedefs; an unresolvable name emits S_UnknownType there).
        // The resolved target is stamped on BOTH the type child (the
        // HIR lowering probes for a stamped type below the cast node —
        // the compound-literal precedent) and the cast node itself
        // (the expression's RESULT type for enclosing checks). The
        // (target, operand) pair is then validated against the
        // explicit-cast matrix — S_InvalidCast on illegal pairs
        // (struct/union values, void, arrays).
        auto castIt = s.idx().castByRule.find(rule.v);
        if (castIt != s.idx().castByRule.end()) {
            auto const& castRule = cfg.castRules[castIt->second];
            auto kids = visibleChildren(tree, node);
            if (castRule.typeChild < kids.size()
                && castRule.operandChild < kids.size()) {
                NodeId const typeNode    = kids[castRule.typeChild];
                NodeId const operandNode = kids[castRule.operandChild];
                TypeId const target =
                    resolveTypeNode(s, cfg, tree, typeNode, here);
                if (target.valid()) {
                    s.nodeToType.set(typeNode, target);
                    s.nodeToType.set(node, target);
                    TypeId const operandTy =
                        subtreeType(s, tree, operandNode);
                    // FC3.5 sweep-c3 (D-CSUBSET-CAST-VOID-DISCARD): the
                    // `(void)expr` discard idiom is legal for EVERY
                    // operand type (C 6.5.4p2 / 6.3.2.2) — checked
                    // BEFORE the castability matrix, whose entries must
                    // all be mapCast-lowerable (the discard emits no
                    // Cast node at all; see lowerCast).
                    if (operandTy.valid()
                        && !isVoidDiscardCast(s.lattice.interner(), target)
                        && !isExplicitCastable(s.lattice.interner(),
                                               target, operandTy)) {
                        ParseDiagnostic d;
                        d.code     = DiagnosticCode::S_InvalidCast;
                        d.severity = DiagnosticSeverity::Error;
                        d.buffer   = tree.source().id();
                        d.span     = tree.span(node);
                        d.actual   = std::string{tree.text(node)};
                        s.reporter.report(std::move(d));
                    }
                }
                // target invalid ⇒ resolveTypeNode already emitted
                // S_UnknownType (fail loud); the cast node stays
                // untyped and enclosing checks cascade-suppress.
            }
        }

        // FC6: sizeof typing. The TYPE form (`sizeof(T)`) resolves + stamps its
        // castTypeRef child through the SAME type resolver casts use (so the HIR
        // lowering's `resolveStampedTypeBelow` recovers the SIZED type); the VALUE
        // form (`sizeof e`) leaves its operand typed normally. BOTH forms stamp the
        // node `size_t` (U64) — the result type for enclosing checks. The operand
        // is UNEVALUATED (C 6.5.3.4); only its type matters. NOTE: U64 is correct
        // for LP64 + LLP64 (both 64-bit `size_t`); a future ILP32 target wants a
        // 32-bit `size_t` here — track under D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-
        // WIDTH (the FOLDED VALUE is already per-target-correct: the MIR layout
        // engine reads `dataModel`; and `(int)sizeof(...)` is the idiom anyway).
        if (cfg.sizeofTypeRule.valid() && rule.v == cfg.sizeofTypeRule.v) {
            auto kids = visibleChildren(tree, node);
            if (cfg.sizeofTypeChild < kids.size()) {
                NodeId const typeNode = kids[cfg.sizeofTypeChild];
                TypeId const sized = resolveTypeNode(s, cfg, tree, typeNode, here);
                if (sized.valid()) s.nodeToType.set(typeNode, sized);
                // sized invalid ⇒ resolveTypeNode emitted S_UnknownType (fail loud).
            }
            s.nodeToType.set(node, s.lattice.interner().primitive(TypeKind::U64));
        }
        if (cfg.sizeofValueRule.valid() && rule.v == cfg.sizeofValueRule.v) {
            s.nodeToType.set(node, s.lattice.interner().primitive(TypeKind::U64));
        }

        // FC3.5 sweep-c3 (D-CSUBSET-COMPOUND-LITERAL-TYPEDEF):
        // compound-literal typing (`semantics.compoundLiterals`). The
        // type-position child resolves through the SAME resolver the
        // cast block above uses (builtins + pointer stars + struct
        // refs + typedef aliases); the resolved type is stamped on
        // BOTH the type child (the HIR lowering's
        // `resolveStampedTypeBelow` probe) and the node itself (the
        // literal's RESULT type for enclosing checks). NO cast-matrix
        // validation — a compound literal is C 6.5.2.5 postfix
        // syntax, not a conversion; per-element legality lives in the
        // HIR brace-init lowering. Pre-sweep only struct-ref type
        // children resolved (via the struct-name machinery); builtin
        // keywords and typedef names stamped NOTHING and the HIR
        // lowering fail-louded on every scalar compound literal.
        auto clIt = s.idx().compoundLiteralByRule.find(rule.v);
        if (clIt != s.idx().compoundLiteralByRule.end()) {
            auto const& clRule = cfg.compoundLiteralRules[clIt->second];
            auto kids = visibleChildren(tree, node);
            if (clRule.typeChild < kids.size()) {
                NodeId const typeNode = kids[clRule.typeChild];
                TypeId const target =
                    resolveTypeNode(s, cfg, tree, typeNode, here);
                if (target.valid()) {
                    s.nodeToType.set(typeNode, target);
                    s.nodeToType.set(node, target);
                }
                // target invalid ⇒ resolveTypeNode already emitted
                // S_UnknownType (fail loud); the literal stays untyped
                // and enclosing checks cascade-suppress.
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
                // subtreeType (not raw typeAt) — the LHS of `p->x` /
                // `s.x` is the expression-wrapper node, not the leaf;
                // pass 2 sets the type on the inner identifier-leaf
                // for bare references and on the leaf literal for
                // immediates. typeAt() against the wrapper returns
                // InvalidType for both, silently bypassing
                // S_NotAPointer / S_NotAComposite / field-type
                // write-back. The DFS-descent helper handles both
                // cases. Mirrors the same fix applied to checkCall
                // (D-LANG-POINTER-VOID-CONVERT closure, step 13.2 —
                // the void-pointer negative pin surfaced the
                // checkCall variant of this gap; the silent-failure
                // hunt review surfaced the parallel sites here +
                // initNode below).
                TypeId lhsType = subtreeType(s, tree, lhsNode);
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
            auto kids = declRoleChildren(tree, node, decl);
            // FC4 c1: declarator-mode rows — each init-declarator carries
            // its OWN optional initializer (`int x = 1, *p = q;`); check
            // every (declared type, init type) pair with the SAME
            // assignability + null-pointer-constant rules the legacy
            // positional path applies below.
            if (decl.isDeclaratorMode() && cfg.declarators.has_value()) {
                auto const carrierIdx = decl.declaratorListChild.has_value()
                                            ? decl.declaratorListChild
                                            : decl.declaratorChild;
                if (carrierIdx.has_value() && *carrierIdx < kids.size()) {
                    std::vector<NodeId> declarators;
                    collectDeclarators(tree, kids[*carrierIdx],
                                       *cfg.declarators, declarators);
                    for (NodeId dNode : declarators) {
                        if (tree.rule(dNode)
                                != cfg.declarators->initDeclaratorRule) {
                            continue;   // no init slot on a bare declarator
                        }
                        // The init subtree = the Internal child that is
                        // NOT the declarator (`[declarator, '=', init]`).
                        NodeId initNode{};
                        for (NodeId c : visibleChildren(tree, dNode)) {
                            if (tree.kind(c) != NodeKind::Internal) continue;
                            if (tree.rule(c) == cfg.declarators->declaratorRule)
                                continue;
                            initNode = c;
                            break;
                        }
                        if (!initNode.valid()) continue;
                        NodeId const nameNode = declaratorNameNode(
                            tree, dNode, *cfg.declarators);
                        if (!nameNode.valid()) continue;
                        SymbolId const sym = s.symbolAtOr(nameNode);
                        if (!sym.valid()) continue;
                        auto& rec = s.symbols.at(sym);
                        // The init's type = the first stamped type along
                        // the SINGLE-child wrapper chain (initValue →
                        // expression → operand/binaryExpr/...). An
                        // aggregate brace-init list (multi-child) yields
                        // none and is deliberately SKIPPED here — its
                        // per-element checks live in the HIR brace-init
                        // lowering, contextually typed by the declared
                        // type; a DFS into the braces would surface a
                        // member literal's type and false-fire
                        // S_TypeMismatch against the aggregate.
                        TypeId initTy = InvalidType;
                        for (NodeId walk = initNode; walk.valid();) {
                            initTy = s.typeAt(walk);
                            if (initTy.valid()) break;
                            auto wk = visibleChildren(tree, walk);
                            if (wk.size() != 1) break;
                            walk = wk[0];
                        }
                        if (!rec.type.valid()) {
                            if (initTy.valid()) {
                                rec.type = initTy;
                                s.nodeToType.set(nameNode, initTy);
                            }
                        } else if (initTy.valid()
                                   && !isAssignable(s.lattice.interner(),
                                                    rec.type, initTy,
                                                    tree.schema().semantics()
                                                        .pointerConversions,
                                                    /*boolWidensToArith=*/true)
                                   && !admitsNullPointerConstant(
                                          s, tree, rec.type, initNode,
                                          tree.schema().semantics()
                                              .pointerConversions)) {
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
            if (decl.initChild.has_value() && *decl.initChild < kids.size()
                && decl.nameChild.has_value() && *decl.nameChild < kids.size()) {
                NodeId initNode = kids[*decl.initChild];
                // subtreeType — bare identifier initializers like
                // `int *p = q;` carry their type on the leaf, not on
                // the expression wrapper. typeAt(initNode) returns
                // InvalidType for the wrapper, the inference branch
                // silently skips (rec.type stays Invalid), AND the
                // brand-new pointerConversions-gated isAssignable
                // check below is bypassed. The DFS-descent helper
                // closes both. Mirrors the checkCall + member-access
                // fix (D-LANG-POINTER-VOID-CONVERT closure +
                // silent-failure hunt fold).
                TypeId initTy = subtreeType(s, tree, initNode);
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
                                                    rec.type, initTy,
                                                    tree.schema().semantics()
                                                        .pointerConversions,
                                                    /*boolWidensToArith=*/true)
                                   // D-LANG-NULL-POINTER-CONSTANT (step
                                   // 13.3): admit `T* p = 0;` initializer
                                   // per C §6.3.2.3.3.
                                   && !admitsNullPointerConstant(
                                          s, tree, rec.type, initNode,
                                          tree.schema().semantics()
                                              .pointerConversions)) {
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

// Collect (param node, resolved type) pairs from a params subtree, in
// source (left-to-right) order, by recursively walking it for declaration
// rules and resolving each via `declRowDeclaredType` (legacy typeChild AND
// FC4 declarator-mode rows alike — ONE per-param resolution). A param-decl
// node is NOT descended into (its own children are the param's type/name —
// and a declarator-mode fn-ptr param's NESTED params must never leak into
// the enclosing signature). SE6 + FC4 c1.
void collectParamTypes(EngineState& s, SemanticConfig const& cfg,
                       Tree const& tree, NodeId cur, ScopeId scope,
                       std::vector<std::pair<NodeId, TypeId>>& out,
                       bool emitOnMiss) {
    if (!cur.valid() || isEmptySpace(tree.flags(cur))) return;
    if (tree.kind(cur) == NodeKind::Internal) {
        auto declIt = s.idx().declByRule.find(tree.rule(cur).v);
        if (declIt != s.idx().declByRule.end()) {
            auto const& decl = cfg.declarations[declIt->second];
            if (decl.isDeclaratorMode()
                || decl.typeChild.has_value()) {
                out.emplace_back(cur, declRowDeclaredType(
                    s, cfg, tree, cur, scope, emitOnMiss));
            }
            return;  // a param decl is a leaf for parameter-collection
        }
    }
    for (auto const& child : tree.children(cur)) {
        collectParamTypes(s, cfg, tree, child, scope, out, emitOnMiss);
    }
}

// FC4 c1: the name (if any) declared by one PARAM-row node — drives the
// `(void)` normalization's named-vs-unnamed distinction across BOTH row
// shapes (declarator-mode walk / legacy positional nameChild).
[[nodiscard]] bool paramRowIsNamed(EngineState& s, SemanticConfig const& cfg,
                                   Tree const& tree, NodeId declNode) {
    auto declIt = s.idx().declByRule.find(tree.rule(declNode).v);
    if (declIt == s.idx().declByRule.end()) return false;
    auto const& decl = cfg.declarations[declIt->second];
    auto kids = declRoleChildren(tree, declNode, decl);
    if (decl.isDeclaratorMode() && cfg.declarators.has_value()) {
        auto const carrier = decl.declaratorListChild.has_value()
                                 ? decl.declaratorListChild
                                 : decl.declaratorChild;
        if (!carrier.has_value() || *carrier >= kids.size()) return false;
        std::vector<NodeId> ds;
        collectDeclarators(tree, kids[*carrier], *cfg.declarators, ds);
        for (NodeId dn : ds) {
            if (declaratorNameNode(tree, dn, *cfg.declarators).valid()) {
                return true;
            }
        }
        return false;
    }
    if (!decl.nameChild.has_value() || *decl.nameChild >= kids.size()) {
        return false;
    }
    auto rn = extractNameNode(tree, kids[*decl.nameChild], decl.nameMatch,
                              cfg.identifierToken, cfg.bracketIdentifierToken);
    return rn.node.valid() && !rn.name.empty();
}

// FC4 c1: C 6.7.6.3p10 — the `(void)` parameter-list convention, applied at
// the ONE chokepoint both FnSig builders share (the legacy function-decl
// arm and the declarator fn-suffix fold). When the language declares
// `parameters.soleVoidMeansEmpty`:
//   * exactly ONE param, resolved type Void, UNNAMED  → zero params;
//   * any OTHER Void param (named, or void-among-others) → ill-formed,
//     S_InvalidVoidParam positioned at the param (suppressed when
//     `emitOnMiss` is false — a harvest re-resolution must not re-report
//     what the definitive visit already did; the normalization itself
//     still applies so the two computations agree on the type).
void normalizeSoleVoidParams(EngineState& s, SemanticConfig const& cfg,
                             Tree const& tree,
                             std::vector<std::pair<NodeId, TypeId>>& params,
                             bool emitOnMiss) {
    if (!cfg.parameters.soleVoidMeansEmpty) return;
    auto const isVoid = [&](TypeId t) {
        return t.valid()
            && s.lattice.interner().kind(t) == TypeKind::Void;
    };
    bool anyVoid = false;
    for (auto const& [n, t] : params) anyVoid = anyVoid || isVoid(t);
    if (!anyVoid) return;
    if (params.size() == 1 && isVoid(params[0].second)
        && !paramRowIsNamed(s, cfg, tree, params[0].first)) {
        params.clear();
        return;
    }
    if (!emitOnMiss) return;
    for (auto const& [n, t] : params) {
        if (!isVoid(t)) continue;
        ParseDiagnostic d;
        d.code     = DiagnosticCode::S_InvalidVoidParam;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(n);
        d.actual   = std::string{tree.text(n)};
        s.reporter.report(std::move(d));
    }
}

// SE6: verify a call node against the callee's signature. Resolves the
// callee child to a symbol (or peels a non-identifier callee expression
// to its designator core); emits S_NotCallable / S_ArgCountMismatch /
// S_TypeMismatch / S_UndeclaredIdentifier as appropriate. Indirect
// calls through a Ptr<FnSig> value route through the SAME signature
// checking as direct calls (FC4 c2 — the call-via-register encoding
// landed end-to-end, retiring the S_IndirectCallNotSupported wall).

// D-CSUBSET-FNPTR-INDIRECT-CALL (closed FC4 c2): TYPE-driven test for
// "this value is a function pointer" — lattice kinds only (Ptr whose
// sole operand is a FnSig), zero language identity. Declarators TYPE
// `int (*fp)(int)` (FC4 c1 stage 2a) and the call path encodes both
// direct symbol calls AND calls through the pointer (LIR call-reg
// opcode, x86 FF /2, arm64 BLR — FC4 c2), so a Ptr<FnSig> callee is
// unwrapped and signature-checked instead of walled.
[[nodiscard]] bool isFnPointerType(TypeInterner const& in, TypeId t) {
    return t.valid()
        && in.kind(t) == TypeKind::Ptr
        && in.kind(in.operands(t)[0]) == TypeKind::FnSig;
}

// SE6 shared tail (extracted FC4 c2): verify a call node against a
// RESOLVED callee signature — result-type stamp + variadic-aware arity
// + per-arg assignability. Shared by EVERY callee shape that lands on
// a FnSig: the direct symbol path (which threads its symbol's
// `variadicBuiltin` flag), the paren-wrapped direct designator, and
// the indirect function-pointer path (variadicBuiltin = false — that
// flag marks NAME-addressed builtins like tsql COALESCE, unreachable
// through a pointer value). The FnSig's own C-style variadic bit
// (`fnIsVariadic`) needs no symbol, so variadic signatures check
// identically through every path.
void checkCallAgainstSig(EngineState& s, SemanticConfig const& /*cfg*/,
                         Tree const& tree, NodeId node,
                         std::vector<NodeId> const& kids,
                         CallRule const& call, TypeId fnSig,
                         bool variadicBuiltin) {
    // FIX 2: the call EXPRESSION carries the callee's RESULT type — not its
    // FnSig. Without this, a `return f(args);` walk (subtreeType) would
    // surface the callee identifier's FnSig (which IS typed, below) and
    // wrongly fail `isAssignable(I32, FnSig)` → a spurious
    // S_ReturnTypeMismatch on legal `int g(){ return f(); }`. Typing the
    // call node with its result also feeds future consumers (e.g.
    // `int x = f();` initializer-type-flow). Set BEFORE arity/arg checks so
    // the result type is present even when an arg mismatch is reported (the
    // call's value type is its declared result regardless of arg errors).
    s.nodeToType.set(node, s.lattice.interner().fnResult(fnSig));

    // Collect arg expression nodes (visible children of the args subtree
    // that are NOT separators). The argsChild subtree is a comma-separated
    // list of `expression`s; count the non-token visible children.
    std::vector<NodeId> argNodes;
    if (call.argsChild < kids.size()) {
        gatherArgExpressions(tree, kids[call.argsChild], argNodes);
    }

    auto params = s.lattice.interner().fnParams(fnSig);

    // D-LANG-VARIADIC (step 13.4): a C-style variadic FnSig
    // (scalars[1] == 1) admits >= fixedParamCount args; a non-variadic
    // FnSig admits exactly fixedParamCount. The pre-existing
    // `variadicBuiltin` flag (e.g. tsql COALESCE) admits ANY arg count
    // — those builtins have no fixed prefix to require.
    bool const variadicFnSig   = s.lattice.interner().fnIsVariadic(fnSig);
    bool const tooFewForVariadic =
        variadicFnSig && argNodes.size() < params.size();
    bool const wrongCountForFixed =
        !variadicBuiltin && !variadicFnSig
        && argNodes.size() != params.size();
    if (tooFewForVariadic || wrongCountForFixed) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::S_ArgCountMismatch;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(node);
        // D-LANG-VARIADIC (step 13.4) post-fold MEDIUM-1: mirror the
        // HIR verifier's "fixed " word for variadic-too-few so users
        // can distinguish "wrong fixed-arity" from "variadic prefix
        // too short" without inspecting the FnSig.
        d.actual   = tooFewForVariadic
            ? std::format("{} (fewer than {} fixed parameter(s))",
                          argNodes.size(), params.size())
            : std::to_string(argNodes.size());
        s.reporter.report(std::move(d));
        return;
    }
    bool const variadic = variadicBuiltin || variadicFnSig;

    // Per-arg assignability (only up to the declared arity for variadics).
    std::size_t const checkCount = variadic
        ? std::min(argNodes.size(), params.size())
        : params.size();
    // Use subtreeType (not raw typeAt) so the check sees the type of a
    // bare identifier reference or literal whose type sits on a leaf
    // descendant of the expression wrapper. Pre-fix, `typeAt(argNodes[i])`
    // returned InvalidType for bare `x` references (the wrapper isn't in
    // the type side-table) and the `if (!argTy.valid()) continue;`
    // silently suppressed the diagnostic — a real silent-failure surface
    // that step-13.2 surfaced via DistinctTypedPointersRemainMismatch.
    // The same descent helper already powers checkReturn (`subtreeType`
    // is defined below).
    auto const& ptrRules = tree.schema().semantics().pointerConversions;
    for (std::size_t i = 0; i < checkCount && i < argNodes.size(); ++i) {
        TypeId argTy = subtreeType(s, tree, argNodes[i]);
        if (!argTy.valid()) continue;  // unknown arg type — suppress cascade
        if (!isAssignable(s.lattice.interner(), params[i], argTy, ptrRules,
                          /*boolWidensToArith=*/true)) {
            // D-LANG-NULL-POINTER-CONSTANT (step 13.3): admit literal-0
            // → Ptr<*> as null pointer constant per C §6.3.2.3.3. The
            // check lives here (NOT in isAssignable) because it is
            // value-aware (looks at the literal's decoded value).
            if (admitsNullPointerConstant(s, tree, params[i],
                                          argNodes[i], ptrRules)) {
                continue;
            }
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
        // Not a simple identifier callee. FC4 c2: PEEL the callee
        // expression to its designator core, then triage the LANDED
        // type. The peel walks (grammar-agnostically):
        //   (a)  single-visible-child wrappers (`expression[x]`,
        //        `operand[x]`) — the same descent extractNameNode's
        //        Self walk uses;
        //   (a') paren-style groupings — a leading NON-OPERATOR token
        //        with exactly ONE non-token child (`(fp)`); C 6.5.1p5:
        //        parentheses preserve the designator's meaning;
        //   (b)  deref wrappers — a leading prefix token whose DECLARED
        //        `hirLowering.unaryOps` target is "Deref" peels into its
        //        operand. C 6.5.3.2p4 makes `*` on a function designator
        //        or function pointer the identity for call purposes —
        //        deref(Ptr<FnSig>) = Ptr<FnSig> (designator decays right
        //        back), deref(FnSig) = FnSig — so `(*fp)(3)`,
        //        `(***fp)(x)`, and `(*helper)(40)` all land on the same
        //        designator their derefs started from.
        //
        // Single source of truth (plan-lock-flagged-acceptable): the
        // analyzer reads the schema's `hirLowering()` block here —
        // deref-ness is DECLARED once (the unaryOps "Deref" target) and
        // consumed by BOTH the CST→HIR lowering engine and this peel; a
        // second semantic-side declaration would inevitably drift.
        //
        // Triage of the landed type:
        //   * identifier token  → symbol lookup (an undeclared name
        //     mirrors the bare-callee arm's refByRule dedup below);
        //   * FnSig             → checkCallAgainstSig — paren-wrapped
        //     direct designators `(helper)(...)` get REAL arity/arg
        //     checking (upgraded from c1's deliberate silence);
        //   * Ptr<FnSig>        → unwrap one level → checkCallAgainstSig
        //     (the indirect call — encoded via the call-reg variant;
        //     D-CSUBSET-FNPTR-INDIRECT-CALL closed FC4 c2);
        //   * other valid       → S_NotCallable (typed and provably not
        //     callable);
        //   * invalid/unstamped → conservative silent return (no
        //     semantic-tier expression typing for that shape yet — do
        //     not invent types; deeper tiers keep their own fail-loud
        //     walls).
        auto const& hirCfg = tree.schema().hirLowering();
        // Deref-token set, computed once per call check (the unaryOps
        // table is a handful of entries).
        std::vector<std::uint32_t> derefTokens;
        for (auto const& e : hirCfg.unaryOps) {
            if (e.target == "Deref" && e.token.valid()) {
                derefTokens.push_back(e.token.v);
            }
        }
        auto const isDerefToken = [&](SchemaTokenId tk) {
            for (auto const v : derefTokens) {
                if (v == tk.v) return true;
            }
            return false;
        };
        auto const& opTable = tree.schema().operatorTable();
        NodeId cur = calleeNode;
        bool coveredByRefRule = false;
        // Guard exhaustion (>128-deep wrapper nest) leaves `cur` at an
        // intermediate node → subtreeType → conservative silent return.
        // Degrade-to-silent is deliberate: no miscompile is possible
        // (deeper tiers keep their own fail-loud walls).
        for (int guard = 0; guard < 128 && cur.valid(); ++guard) {
            if (tree.kind(cur) != NodeKind::Internal) break;  // landed
            if (s.idx().refByRule.contains(tree.rule(cur).v)) {
                // A `references` rule covers the walked subtree — its
                // Pass-2 visit owns any undeclared-identifier emission
                // (same dedup contract as the bare-callee arm below).
                coveredByRefRule = true;
            }
            auto peelKids = visibleChildren(tree, cur);
            if (peelKids.empty()) break;
            if (peelKids.size() == 1) {       // (a) transparent wrapper
                cur = peelKids[0];
                continue;
            }
            if (tree.kind(peelKids[0]) == NodeKind::Token) {
                SchemaTokenId const headTk = tree.tokenKind(peelKids[0]);
                // The wrapper's sole non-token child (if exactly one).
                NodeId sole{};
                bool multiple = false;
                for (auto k : peelKids) {
                    if (tree.kind(k) == NodeKind::Token) continue;
                    if (sole.valid()) { multiple = true; break; }
                    sole = k;
                }
                if (isDerefToken(headTk)) {   // (b) deref peel
                    if (!sole.valid() || multiple) break;  // malformed — land
                    cur = sole;
                    continue;
                }
                if (!opTable.lookup(headTk, OperatorArity::Prefix)
                         .has_value()
                    && sole.valid() && !multiple) {
                    cur = sole;               // (a') paren-style grouping
                    continue;
                }
                break;  // operator-headed wrapper (e.g. `-x`) — land here
            }
            break;      // multi-child shape (cast/binary/postfix) — land
        }
        auto const& in = s.lattice.interner();
        TypeId landedTy = InvalidType;
        bool   landedVariadicBuiltin = false;
        if (cur.valid() && tree.kind(cur) == NodeKind::Token
            && cfg.identifierToken.valid()
            && tree.tokenKind(cur) == cfg.identifierToken) {
            SymbolId const sym =
                s.scopes.lookup(scope, std::string{tree.text(cur)});
            if (!sym.valid()) {
                if (coveredByRefRule) {
                    return;  // ref-rule path already emitted (or will)
                }
                ParseDiagnostic d;
                d.code     = DiagnosticCode::S_UndeclaredIdentifier;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = tree.source().id();
                d.span     = tree.span(cur);
                d.actual   = std::string{tree.text(cur)};
                s.reporter.report(std::move(d));
                return;
            }
            landedTy              = s.symbols.at(sym).type;
            landedVariadicBuiltin = s.symbols.at(sym).variadicBuiltin;
        } else {
            landedTy = subtreeType(s, tree, cur.valid() ? cur : calleeNode);
        }
        if (!landedTy.valid()) {
            return;  // unstamped callee expression — out of v1 scope
        }
        if (in.kind(landedTy) == TypeKind::FnSig) {
            checkCallAgainstSig(s, cfg, tree, node, kids, call, landedTy,
                                landedVariadicBuiltin);
            return;
        }
        if (isFnPointerType(in, landedTy)) {
            checkCallAgainstSig(s, cfg, tree, node, kids, call,
                                in.operands(landedTy)[0],
                                /*variadicBuiltin=*/false);
            return;
        }
        ParseDiagnostic d;
        d.code     = DiagnosticCode::S_NotCallable;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(calleeNode);
        d.actual   = std::string{tree.text(calleeNode)};
        s.reporter.report(std::move(d));
        return;
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
        // D-CSUBSET-FNPTR-INDIRECT-CALL (closed FC4 c2): a bare-
        // identifier callee whose symbol is typed Ptr<FnSig>
        // (`int (*fp)(int) = &helper; fp(3);`) is a well-typed INDIRECT
        // call — unwrap to the FnSig and run the SAME result-stamp +
        // arity + per-arg checking as a direct call (the call-via-
        // register encoding landed end-to-end, retiring the
        // S_IndirectCallNotSupported wall). variadicBuiltin is
        // necessarily false through a pointer — that flag marks
        // NAME-addressed builtins (e.g. tsql COALESCE); the FnSig's
        // own C-style variadic bit still applies inside the shared
        // tail. Purely lattice-kind-driven.
        if (isFnPointerType(s.lattice.interner(), fnTy)) {
            checkCallAgainstSig(s, cfg, tree, node, kids, call,
                                s.lattice.interner().operands(fnTy)[0],
                                /*variadicBuiltin=*/false);
            return;
        }
        // Genuinely non-callable value (S_NotCallable is the RIGHT code
        // here: the value is typed and provably not callable in C).
        ParseDiagnostic d;
        d.code     = DiagnosticCode::S_NotCallable;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(resolved.node);
        d.actual   = resolved.name;
        s.reporter.report(std::move(d));
        return;
    }

    // Direct symbol call: the shared tail does the result-type stamp,
    // the variadic-aware arity check, and per-arg assignability. The
    // symbol arm threads its own `variadicBuiltin` flag (e.g. tsql
    // COALESCE admits any arg count).
    checkCallAgainstSig(s, cfg, tree, node, kids, call, fnTy,
                        s.symbols.at(calleeSym).variadicBuiltin);
}

// D-LANG-NULL-POINTER-CONSTANT (step 13.3, 2026-06-02): per C §6.3.2.3.3,
// an integer constant expression with value 0 is a null pointer constant
// — convertible WITHOUT cast to any object-pointer or function-pointer
// type. This helper admits ONLY the bare integer-literal `0` form (not
// `+0` / `-0` / `0+0` / `(int)0`). Audit fold (6-agent 2nd-order F3,
// 2026-06-02): the wider unary/cast/const-eval admission was DROPPED to
// avoid a HIR/semantic divergence — the HIR `coerce()` arm matches on
// `HirKind::Literal` of the child node, and a `-0` source expression
// lowers to `UnaryOp(Neg, Literal(0))` whose top-level HIR kind is
// `UnaryOp`. Semantic admission of `-0` without HIR materialization
// would leak an un-Cast `I32` into a `Ptr<*>` slot at the verifier. The
// wider admission lives at D-LANG-NULL-PTR-CONST-EVAL, which adds a
// const-eval pre-pass producing a typed integer-zero result that both
// tiers can recognize uniformly.
//
// **Honest behavior note** (2nd-order audit comment-analyzer #3,
// 2026-06-02): the helper itself rejects `-0` (returns false on the
// MinusOp non-literal token). But callers thread through
// `subtreeType` first to compute `argTy`; with `-0` the operator-stop
// in subtreeType returns InvalidType, and the caller's
// `if (!argTy.valid()) continue;` cascade-suppression silently
// skips the assignability check — `f(-0)` neither admits nor fires
// a diagnostic. Closing this cascade-suppression false-negative
// requires pass-2 expression typing on unary wrappers (anchored at
// D-SEMANTIC-SUBTREETYPE-TRANSPARENT-WRAPPERS full closure).
//
// Implementation: walks the subtree, rejecting on any non-literal
// token (operator / delimiter / etc.) and requiring exactly one
// typed integer-literal Token leaf with decoded value 0.
[[nodiscard]] bool
isLiteralIntegerZero(EngineState const& s, Tree const& tree, NodeId node) {
    if (!node.valid()) return false;
    NodeId onlyLeaf;
    int    typedCount = 0;
    std::vector<NodeId> stack{node};
    while (!stack.empty()) {
        NodeId cur = stack.back();
        stack.pop_back();
        if (tree.kind(cur) == NodeKind::Token) {
            if (isEmptySpace(tree.flags(cur))) continue;
            auto const tk = tree.tokenKind(cur);
            auto litIt = s.idx().literalTypeIds.find(tk.v);
            if (litIt == s.idx().literalTypeIds.end()) {
                // Non-literal token (operator / delimiter / etc.) —
                // reject. This is the single gate that rules out
                // `-0` (MinusOp present), `(0)` (ParenOpen present),
                // `0 + 0` (PlusOp present), etc. — matching HIR
                // coerce()'s `HirKind::Literal`-only admit shape.
                return false;
            }
            if (!s.typeAt(cur).valid()) continue;
            if (++typedCount > 1) return false;
            onlyLeaf = cur;
            continue;
        }
        for (auto child : tree.children(cur)) {
            if (!isEmptySpace(tree.flags(child))) stack.push_back(child);
        }
    }
    if (typedCount != 1 || !onlyLeaf.valid()) return false;
    auto const tk = tree.tokenKind(onlyLeaf);
    auto litIt = s.idx().literalTypeIds.find(tk.v);
    if (litIt == s.idx().literalTypeIds.end()) return false;
    // Integer kind only (signed or unsigned). Floats / bools / chars
    // can never be a NULL pointer constant.
    auto const kind = s.lattice.interner().kind(litIt->second);
    using namespace detail::type_rules;
    if (signedIntRank(kind) == 0 && unsignedIntRank(kind) == 0) return false;
    auto const decoded =
        decodeInteger(tree.text(onlyLeaf), s.idx().numberStyle);
    return decoded.has_value() && *decoded == 0;
}

// D-LANG-NULL-POINTER-CONSTANT helper (step 13.3): returns true when
// the conversion `rhsExpr → lhsTy` should be admitted as a null-pointer
// conversion. The shape is shared between checkCall arg-check,
// checkReturn, and pass-2 decl-init — all three must read it identically
// so a NULL passed as call-arg, return-value, or initializer behaves
// the same. Source-agnostic: the rule fires only when the active
// language declares `pointerConversions.nullPointerConstantFromIntegerZero:
// true` (default false — Rust/Swift-friendly). The flag check lives
// here; `isLiteralIntegerZero` is a structural-only helper that
// makes no config check.
[[nodiscard]] bool
admitsNullPointerConstant(EngineState const& s, Tree const& tree,
                          TypeId lhsTy, NodeId rhsExpr,
                          SemanticConfig::PointerConversionRules const& rules) {
    if (!rules.nullPointerConstantFromIntegerZero) return false;
    if (!lhsTy.valid()) return false;
    if (s.lattice.interner().kind(lhsTy) != TypeKind::Ptr) return false;
    return isLiteralIntegerZero(s, tree, rhsExpr);
}

// Core operator NAME → HirOpKind (the reverse of `opName`). Mirrors the
// identical helper in cst_to_hir.cpp's anonymous namespace (that one is
// file-local there, so the semantic tier needs its own copy — the LAW it
// encodes, `opName`, is the single shared source). `std::nullopt` when the
// `target` string is not a core operator (a special tag like "AddressOf").
[[nodiscard]] std::optional<HirOpKind> coreOpFromNameSem(std::string_view s) {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(HirOpKind::Count_); ++i) {
        auto const op = static_cast<HirOpKind>(i);
        if (opName(op) == s) return op;
    }
    return std::nullopt;
}

// GAP A: the type carried by an expression subtree — a COMPLETE semantic-tier
// expression typer. A stamped type (`s.typeAt(node)`) is AUTHORITATIVE (Pass 2
// stamps refs, member/call/cast/sizeof results, literals, compound literals);
// when the node is NOT stamped it DERIVES the type per its operator verb,
// mirroring the CST→HIR lowering (`cst_to_hir.cpp`) EXACTLY — the same verb
// vocabulary (`hirLowering.{binary,unary,postfix}Ops` + `ternaryExprRule`) and
// the same result-type laws (`derefResultType`/`indexResultType`/
// `usualArithmeticCommonType`/`integerPromotedType`/`isComparison`, all the
// SHARED `type_rules.hpp` / `hir_op.hpp` sources both tiers call). So the two
// tiers cannot drift on what an expression's type is.
//
// D-SEMANTIC-SUBTREETYPE-TRANSPARENT-WRAPPERS (anchor) — ✅ CLOSED (FC6, 2026-06-15).
// The prior implementation was a cascade-SUPPRESSOR: it DFS-descended for the
// first stamped descendant and STOPPED (returning InvalidType) at any operator
// wrapper, on the premise that "there is no semantic-tier expression typing".
// That premise is now false — each wrapper is typed by its VERB here, so the
// descent-stop is gone. `&x` on `int x` correctly yields `Ptr<I32>` (not the
// false-positive InvalidType), `a + b` its common arithmetic type, `*p` the
// pointee, `a[i]` the element, `c ? t : e` the balanced type — exactly the
// values cst_to_hir computes. Source-AGNOSTIC: the verb strings live in the
// language's `hirLowering` JSON, never in this code (no rule/op identity is
// hardcoded); a language without a `hirLowering` block has empty op tables, so
// every dispatch falls through to the leaf path.
//
// Const note: the signature is `EngineState const& s` (every caller's
// EngineState is itself non-const — Pass 1.5 `resolveSizeof`, Pass 2,
// checkCall/Return — so the mutable interner obtained below never writes
// through a genuinely-const object). The read accessors (`kind`/`operands`/
// `fnResult`) are const; the INTERNING ops the derivations need (`primitive`
// for Bool, `pointer` for AddressOf, the common-type engine) mutate (they
// memoize into the interner's arena), so a mutable interner reference is taken
// once here — the same handle the lowering tier uses.
[[nodiscard]] TypeId
subtreeType(EngineState const& s, Tree const& tree, NodeId node, ScopeId scope) {
    if (!node.valid()) return InvalidType;
    // Stamped type wins — Pass 2 already computed refs, member, call, cast,
    // sizeof, literals, and compound literals onto the node itself.
    if (TypeId t = s.typeAt(node); t.valid()) return t;

    // The interning derivations memoize into the interner's arena, so they need
    // a mutable handle. Sound because every caller owns a non-const EngineState
    // (see the const note above); the const-cast never mutates a const object.
    TypeInterner& interner =
        const_cast<TypeLattice&>(s.lattice).interner();   // NOLINT — see const note
    auto const boolType = [&]() -> TypeId {
        return interner.primitive(TypeKind::Bool);
    };
    // Config-driven C 6.3.1.8 common arithmetic type — the EXACT sibling of
    // cst_to_hir's `commonArithType`: the language's resolved
    // `arithmeticConversions` engine when declared, else the legacy interner
    // rule (toy/tsql keep `TypeInterner::commonType` byte-identically).
    auto const& sem = tree.schema().semantics();
    std::optional<ResolvedArithmeticRules> arith;
    if (sem.arithmeticConversions.has_value()) {
        arith = resolveArithmeticRules(*sem.arithmeticConversions, s.dataModel);
    }
    auto const commonArithType = [&](TypeId a, TypeId b) -> TypeId {
        if (arith.has_value()) {
            return usualArithmeticCommonType(interner, a, b, *arith);
        }
        return interner.commonType(a, b);
    };

    // ── leaf typing: an identifier resolves to its symbol's type by scope ──
    // A literal token is Pass-2-stamped (handled by the `typeAt` above); a
    // Pass-1.5 `sizeof(literal)` is rare and InvalidType there is an acceptable
    // fail-loud (the array length then reports S_NonConstantArrayLength, never a
    // guessed size). An IDENTIFIER leaf, however, MUST resolve here — that is
    // the Pass-1.5 `sizeof b` / `sizeof(b[0])` case the sizeof closure feeds.
    auto const leafType = [&](NodeId tok) -> TypeId {
        if (tree.kind(tok) != NodeKind::Token) return InvalidType;
        if (!sem.identifierToken.valid()
            || tree.tokenKind(tok).v != sem.identifierToken.v) {
            return InvalidType;            // a literal/operator token — not a name
        }
        if (!scope.valid()) return InvalidType;
        SymbolId const sym = s.scopes.lookup(scope, tree.text(tok));
        if (!sym.valid()) return InvalidType;
        return s.symbols.at(sym).type;
    };

    // A token-leaf node reached directly (only at Pass 1.5; Pass 2 would have
    // stamped it). Type it as a leaf.
    if (tree.kind(node) != NodeKind::Internal) {
        return leafType(node);
    }

    auto const& hirCfg = tree.schema().hirLowering();
    RuleId const r = tree.rule(node);

    // The visible operator token + its `hirLowering` op-table entry. `verb` is
    // the entry's `target` (the HIR op NAME or special tag) the law dispatches
    // on, matching cst_to_hir's `binOp_`/`unOp_`/`postOp_` token→entry maps.
    auto const opEntryFor =
        [&](std::vector<HirOperatorEntry> const& ops) -> HirOperatorEntry const* {
        for (NodeId c : visibleChildren(tree, node)) {
            if (tree.kind(c) != NodeKind::Token) continue;
            std::uint32_t const tk = tree.tokenKind(c).v;
            for (auto const& e : ops) {
                if (e.token.v == tk) return &e;
            }
        }
        return nullptr;
    };

    // ── cast / sizeof / compound-literal: RE-TYPING wrappers ──
    // Pass 2 STAMPS these (the `typeAt` above returns them there); but at Pass
    // 1.5 — a sizeof-operand sub-expression — they are NOT stamped yet, so type
    // them HERE exactly as the Pass-2 arms do. Without this, the transparent-
    // wrapper fallthrough below would descend PAST the cast and return the
    // operand's PRE-cast type — but the cast's whole purpose is to RE-TYPE, so
    // `sizeof((char)x)` ≠ `sizeof(x)` would silently fold the WRONG size in an
    // array dimension (the divergence-from-cst_to_hir class this typer forbids).
    if (hirCfg.sizeofRule.valid() && r.v == hirCfg.sizeofRule.v) {
        return interner.primitive(TypeKind::U64);                  // size_t
    }
    if (auto const it = s.idx().castByRule.find(r.v);
        it != s.idx().castByRule.end()) {
        auto const& cr = sem.castRules[it->second];
        auto const kids = visibleChildren(tree, node);
        if (cr.typeChild >= kids.size()) return InvalidType;
        // emitOnMiss=false — the Pass-2 cast arm owns the S_UnknownType
        // diagnostic; here we only need the (re-typed) target type.
        return resolveTypeNode(const_cast<EngineState&>(s), sem, tree,
                               kids[cr.typeChild], scope, /*emitOnMiss=*/false);
    }
    if (auto const it = s.idx().compoundLiteralByRule.find(r.v);
        it != s.idx().compoundLiteralByRule.end()) {
        auto const& cl = sem.compoundLiteralRules[it->second];
        auto const kids = visibleChildren(tree, node);
        if (cl.typeChild >= kids.size()) return InvalidType;
        return resolveTypeNode(const_cast<EngineState&>(s), sem, tree,
                               kids[cl.typeChild], scope, /*emitOnMiss=*/false);
    }

    // ── binary: mirror cst_to_hir combineBinary / lowerBinary ──
    if (r.v == hirCfg.binaryExprRule.v) {
        // [lhs(Internal), OP-token, rhs(Internal)]
        NodeId lhsN{}, rhsN{};
        for (NodeId c : visibleChildren(tree, node)) {
            if (tree.kind(c) == NodeKind::Token) continue;
            if (!lhsN.valid()) lhsN = c; else if (!rhsN.valid()) rhsN = c;
        }
        HirOperatorEntry const* e = opEntryFor(hirCfg.binaryOps);
        if (e == nullptr) return InvalidType;
        TypeId const lt = subtreeType(s, tree, lhsN, scope);
        TypeId const rt = subtreeType(s, tree, rhsN, scope);
        if (e->target == "LogicalAnd" || e->target == "LogicalOr") return boolType();
        if (e->target == "Comma")  return rt;                       // value of RHS
        // Assignment / compound-assignment yield the LHS (lvalue) type.
        if (e->target == "Assign" || !e->compoundBase.empty()) return lt;
        auto const op = coreOpFromNameSem(e->target);
        if (op.has_value()) {
            if (isComparison(*op)) return boolType();
            // Shift: result is the PROMOTED LEFT operand (C 6.5.7); the count's
            // type never contributes. Only WITH the arithmetic block (exactly as
            // cst_to_hir's combineBinary gates its shift special-case); a block-
            // less language falls through to the common-type path below — keeping
            // the two tiers identical for any future shift-declaring language.
            if ((*op == HirOpKind::Shl || *op == HirOpKind::Shr)
                && arith.has_value()) {
                return integerPromotedType(interner, lt, *arith);
            }
        }
        // Arithmetic / bitwise: the usual-arithmetic common type, falling back
        // to the first valid operand (pointer arithmetic / non-arith pairs)
        // exactly as combineBinary's result rule.
        TypeId const common = commonArithType(lt, rt);
        if (common.valid()) return common;
        return lt.valid() ? lt : rt;
    }

    // ── unary: mirror cst_to_hir lowerUnary ──
    if (r.v == hirCfg.unaryExprRule.v) {
        // [OP-token, operand(Internal)]
        NodeId opTok{}, operandN{};
        for (NodeId c : visibleChildren(tree, node)) {
            if (tree.kind(c) == NodeKind::Token) { if (!opTok.valid()) opTok = c; continue; }
            if (!operandN.valid()) operandN = c;
        }
        HirOperatorEntry const* e = opEntryFor(hirCfg.unaryOps);
        if (e == nullptr) return InvalidType;
        TypeId const ot = subtreeType(s, tree, operandN, scope);
        if (e->target == "AddressOf") return ot.valid() ? interner.pointer(ot) : InvalidType;
        if (e->target == "Deref")     return derefResultType(interner, ot);
        auto const op = coreOpFromNameSem(e->target);
        if (op.has_value() && *op == HirOpKind::Not) return boolType();
        return ot;   // Neg / BitNot are type-preserving
    }

    // ── postfix: mirror cst_to_hir lowerPostfix ──
    if (r.v == hirCfg.postfixExprRule.v) {
        // [base(Internal), OP-token, rest...]
        NodeId baseN{};
        for (NodeId c : visibleChildren(tree, node)) {
            if (tree.kind(c) != NodeKind::Token) { baseN = c; break; }
        }
        HirOperatorEntry const* e = opEntryFor(hirCfg.postfixOps);
        if (e == nullptr) return InvalidType;
        TypeId const bt = subtreeType(s, tree, baseN, scope);
        if (e->target == "Index")   return indexResultType(interner, bt);
        if (e->target == "PostInc" || e->target == "PostDec") return bt;
        // Call / member access are Pass-2-stamped (the leading `typeAt` returns
        // before here). Unstamped is only reachable at Pass 1.5; a Call there
        // can still resolve its result from the callee signature, while member
        // access is exotic in a sizeof operand — InvalidType (acceptable
        // fail-loud, mirrors cst_to_hir's typeAtOr-fallback shape).
        if (e->target == "Call") {
            // Unwrap the callee type to its FnSig — a direct FnSig designator or
            // a `Ptr<FnSig>` function-pointer value (one pointer level), the same
            // `calleeSigOf` unwrap cst_to_hir uses; then the result is its
            // `fnResult`. InvalidType for any other callee shape.
            TypeId sig{};
            if (bt.valid()) {
                if (interner.kind(bt) == TypeKind::FnSig) {
                    sig = bt;
                } else if (interner.kind(bt) == TypeKind::Ptr) {
                    auto const ops = interner.operands(bt);
                    if (!ops.empty() && interner.kind(ops[0]) == TypeKind::FnSig) {
                        sig = ops[0];
                    }
                }
            }
            return sig.valid() ? interner.fnResult(sig) : InvalidType;
        }
        return InvalidType;   // MemberAccess / MemberAccessThruPtr
    }

    // ── ternary: mirror cst_to_hir lowerTernary ──
    if (hirCfg.ternaryExprRule.valid() && r.v == hirCfg.ternaryExprRule.v) {
        // operands = the 3 non-token visible children [cond, then, else]
        std::vector<NodeId> operands;
        for (NodeId c : visibleChildren(tree, node)) {
            if (tree.kind(c) != NodeKind::Token) operands.push_back(c);
        }
        if (operands.size() != 3) return InvalidType;
        TypeId const thenT = subtreeType(s, tree, operands[1], scope);
        TypeId const elseT = subtreeType(s, tree, operands[2], scope);
        TypeId const common = commonArithType(thenT, elseT);
        if (common.valid()) return common;
        return thenT.valid() ? thenT : elseT;   // non-arith arms (pointers etc.)
    }

    // Transparent / operand wrapper (paren-expression, operand alts, the
    // primary-expression that wraps a bare literal or identifier leaf): the
    // wrapper passes its inner expression's type through unchanged. Recurse via
    // `subtreeType` on each visible child in order and return the first VALID
    // result — `subtreeType` itself checks the stamped side-table first (so a
    // Pass-2-stamped literal leaf yields its type), then a nested operator
    // wrapper by its verb, then an identifier leaf by scope-lookup. This is the
    // single-source equivalent of the old "first stamped descendant wins" DFS,
    // now made operator-AWARE (a nested `&x` returns `Ptr<I32>`, not the inner
    // leaf's I32). An EmptySpace child is already filtered by visibleChildren.
    for (NodeId c : visibleChildren(tree, node)) {
        if (TypeId t = subtreeType(s, tree, c, scope); t.valid()) return t;
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
    auto const& ptrRules = tree.schema().semantics().pointerConversions;
    if (!isAssignable(s.lattice.interner(), fnResult, exprTy, ptrRules,
                      /*boolWidensToArith=*/true)) {
        // D-LANG-NULL-POINTER-CONSTANT (step 13.3): admit `return 0;`
        // from a Ptr<*>-returning function per C §6.3.2.3.3.
        if (admitsNullPointerConstant(s, tree, fnResult,
                                      valueNode, ptrRules)) {
            return;
        }
        emitMismatch(valueNode);
    }
}

} // namespace

SemanticModel analyze(std::shared_ptr<CompilationUnit const> cu,
                      DataModel dataModel,
                      std::optional<AggregateLayoutParams> aggregateLayout) {
    if (!cu) {
        std::fputs("dss::analyze fatal: null CompilationUnit\n", stderr);
        std::abort();
    }
    EngineState s{*cu};
    s.dataModel = dataModel;
    s.aggregateLayout = aggregateLayout;

    // FC3 c1: ILP32 is DECLARED-ONLY (the wasm/spirv skeleton formats
    // carry it for honesty) — no exercised 32-bit width path exists, so
    // selecting it fails loud rather than silently typing `long` /
    // pointers with untested widths. Positioned at the first tree's
    // root (the analysis covers the whole CU); analysis continues under
    // the collect-all discipline, but the error fails the compile.
    if (dataModel == DataModel::Ilp32) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::S_UnsupportedDataModel;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::string{dataModelName(dataModel)};
        for (Tree const& tree : cu->trees()) {
            if (!tree.root().valid()) continue;
            d.buffer = tree.source().id();
            d.span   = tree.span(tree.root());
            break;
        }
        s.reporter.report(std::move(d));
    }

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
            // CcSysV is the canonical MIR-tier placeholder (mirrors
            // the same convention as the user-function FnSig
            // construction earlier in this TU + `hir_to_mir.cpp:
            // lowerModuleInit`'s moduleInit FnSig): ML7's
            // calling-convention pass maps to the target's real
            // convention at materialize time via `cc.name` lookup.
            // Do NOT inspect this CallConv field at MIR tier.
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
    //
    // Conflict detection (FF11 GOAL-2): if the SOURCE tree ALREADY declares
    // a top-level symbol of the same name in its OWN root scope (a genuine
    // user declaration — `SymbolRecord.tree == sourceTree`), then including
    // a header that ALSO declares that name is a redeclaration — exactly C's
    // rule that `#include <stdio.h>` + your own `extern ... puts(...)` is a
    // duplicate. Emit S_RedeclaredSymbol at the SOURCE decl (where the user
    // wrote the conflicting line) rather than silently shadowing. We do NOT
    // flag injected-vs-injected collisions (a diamond/transitive include
    // making the same target symbol visible via two edges): those are
    // idempotent visibility, not a user conflict — gated by the
    // `tree == sourceTree` check (an injected binding carries the DEFINING
    // tree's id, never the source's).
    //
    // De-duplication: the ImportResolver emits one crossRefs edge per
    // `#include` directive, so `#include <h>` written TWICE produces two
    // edges with the same (sourceTree, targetTree). Without de-dup, one
    // logical name collision would emit S_RedeclaredSymbol once PER
    // duplicate edge (over-reporting). Track the (sourceTree, name) pairs
    // already reported across the WHOLE loop so a given colliding name in a
    // given source tree fails loud exactly once — independent of how many
    // include directives (or how many distinct headers) re-declare it. A
    // genuine single conflict still fires once; a DIFFERENT colliding name
    // (whether via the same or another header) is a distinct key and still
    // fires.
    std::unordered_set<std::string> reportedConflicts;
    auto const conflictKey = [](std::uint32_t treeV,
                                std::string_view name) {
        // EXACT (collision-free) composite key: the tree id's decimal digits,
        // a NUL separator (never present in an identifier), then the name. Two
        // trees declaring the same colliding name stay distinct. A 64-bit HASH
        // key would risk a (vanishingly rare) collision silently dropping a
        // genuinely-distinct second conflict — exact-over-probabilistic is the
        // right standard for a correctness-bearing dedup set.
        std::string key = std::to_string(treeV);
        key.push_back('\0');
        key.append(name);
        return key;
    };
    for (auto const& edge : cu->crossRefs()) {
        auto srcIt = treeRootScope.find(edge.sourceTree.v);
        auto tgtIt = treeRootScope.find(edge.targetTree.v);
        if (srcIt == treeRootScope.end() || tgtIt == treeRootScope.end()) continue;

        // The source tree's OWN top-level bindings (declared in pass 1),
        // keyed by name. An entry here whose symbol's tree is the source
        // tree is a real user declaration; a later same-name inject is a
        // conflict. Built once per edge from the source root scope.
        std::unordered_map<std::string_view, SymbolId> ownByName;
        for (auto const& [name, sym] : s.scopes.bindingsOf(srcIt->second)) {
            if (sym.valid() && s.symbols.at(sym).tree.v == edge.sourceTree.v) {
                ownByName.emplace(name, sym);
            }
        }

        for (auto const& [name, sym] : s.scopes.bindingsOf(tgtIt->second)) {
            if (auto it = ownByName.find(name); it != ownByName.end()) {
                // The source file declared `name` itself AND includes a
                // header declaring it — duplicate. Fail loud at the source,
                // but only ONCE per (sourceTree, name): a second include of
                // the same (or another) header re-declaring `name` is the
                // same logical conflict, already reported.
                if (!reportedConflicts.insert(
                        conflictKey(edge.sourceTree.v, name)).second) {
                    continue;   // already reported; still skip injection
                }
                auto const& srcTree = *std::find_if(
                    trees.begin(), trees.end(),
                    [&](Tree const& t) { return t.id().v == edge.sourceTree.v; });
                auto const& ownRec = s.symbols.at(it->second);
                ParseDiagnostic d;
                d.code     = DiagnosticCode::S_RedeclaredSymbol;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = srcTree.source().id();
                d.span     = srcTree.span(ownRec.declNode);
                d.actual   = std::string{name};
                auto const& hdrRec = s.symbols.at(sym);
                auto const tgtTreeIt = std::find_if(
                    trees.begin(), trees.end(),
                    [&](Tree const& t) { return t.id().v == edge.targetTree.v; });
                if (tgtTreeIt != trees.end()) {
                    d.related.push_back(RelatedLocation{
                        tgtTreeIt->source().id(),
                        tgtTreeIt->span(hdrRec.declNode),
                        "also declared by the included header",
                    });
                }
                s.reporter.report(std::move(d));
                continue;   // do not inject the conflicting binding
            }
            s.scopes.injectBinding(srcIt->second, std::string{name}, sym);
        }
    }

    // FF11 shipped-library descriptor injection (the builtinFunctions analogue),
    // closing D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC's semantic half. An
    // angle/system `#include <stdio.h>` resolved (in CU4) to a LANGUAGE-NEUTRAL
    // JSON descriptor whose path the CU recorded; here — AFTER every tree's
    // Pass 1 (so user decls exist for the goal-2 skip) and BEFORE Pass 2 (so a
    // user CALL like `puts("hi")` resolves against the injected symbol) — we
    // read each descriptor, intern each symbol's signature into THIS CU's
    // interner (`s.lattice.interner()`, the same interner the lowerer lowers
    // through), mint an extern function SymbolRecord, and inject it into the CU
    // ROOT scope (the parent of every language's builtin scope → visible to
    // every tree, language-blind — a neutral system import). The minted row is
    // recorded on `shippedExterns` so the CST→HIR lowerer can synthesize the
    // matching ExternFunction + HirExternRecord that FF5 binds to the library.
    //
    // GOAL-2 (a user who BOTH `#include <stdio.h>` AND writes `extern int
    // puts(...)`): the descriptor symbol is SKIPPED when ANY user declaration
    // already claims the name. The user decl wins and is the sole authority — no
    // duplicate symbol, no duplicate extern HIR node, no double-bound import.
    // This is the descriptor-model analogue of cycle-21's tree-level
    // S_RedeclaredSymbol (which no longer applies — a descriptor is not a tree).
    std::vector<ShippedExternSymbol> shippedExterns;
    {
        // Names any USER declaration (top-level, in any tree's own root scope)
        // claimed — the goal-2 skip set. A binding whose symbol's `tree` is the
        // root scope's own tree is a real user decl (injected cross-tree
        // bindings carry the DEFINING tree's id; builtins carry InvalidTree).
        std::unordered_set<std::string> userDeclaredNames;
        for (auto const& [treeV, scope] : treeRootScope) {
            for (auto const& [name, sym] : s.scopes.bindingsOf(scope)) {
                if (sym.valid() && s.symbols.at(sym).tree.v == treeV) {
                    userDeclaredNames.insert(std::string{name});
                }
            }
        }

        // Dedup descriptor paths (the same `#include <stdio.h>` in two trees, or
        // twice in one tree, records the path twice) AND symbol names already
        // minted (two descriptors both declaring `puts` → first wins) so a name
        // is injected at most once.
        std::unordered_set<std::string> readDescriptors;
        std::unordered_set<std::string> injectedNames;
        for (std::filesystem::path const& descPath :
             cu->shippedLibDescriptors()) {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(descPath, ec);
            std::string const key =
                (ec ? descPath.lexically_normal() : canonical).string();
            if (!readDescriptors.insert(key).second) continue;  // already read

            // Read + decode against THIS CU's interner/registry/reporter. The
            // reader fails loud (F_ShippedLibDescriptorMalformed /
            // F_ShippedLibUnsupportedType) and returns nullopt on any problem;
            // nothing is injected in that case (the pipeline then aborts on the
            // reporter error delta — never a silent partial import).
            auto desc = ffi::readShippedLibDescriptor(
                descPath, s.lattice.interner(), s.lattice.registry(), s.reporter,
                s.dataModel);
            if (!desc) continue;

            for (auto const& sym : desc->symbols) {
                // GOAL-2: a user decl of this name wins — skip the descriptor's.
                if (userDeclaredNames.contains(sym.name)) continue;
                if (!injectedNames.insert(sym.name).second) continue;  // first wins

                SymbolRecord rec;
                rec.name  = sym.name;
                rec.scope = cuRoot;
                rec.tree  = InvalidTree;   // not a user decl (mirrors builtins)
                rec.kind  = sym.kind == ffi::ShippedSymbolKind::Function
                                ? DeclarationKind::Function
                                : DeclarationKind::Variable;
                rec.type  = sym.signature;
                SymbolId const id = s.symbols.mint(rec);
                s.scopes.injectBinding(cuRoot, sym.name, id);

                shippedExterns.push_back(ShippedExternSymbol{
                    id, sym.name, sym.signature, desc->library,
                    sym.kind == ffi::ShippedSymbolKind::Function});
            }
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
        std::move(shippedExterns),
        dataModel,
    };
}

} // namespace dss
