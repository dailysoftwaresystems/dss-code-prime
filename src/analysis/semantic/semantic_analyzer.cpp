#include "analysis/semantic/semantic_analyzer.hpp"

#include "analysis/compilation_unit/unit_attribute.hpp"
#include "analysis/semantic/scope_tree.hpp"
#include "analysis/semantic/constant_symbol_fold.hpp" // Item 1: shared enum/constant Ref->literal builder
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/symbol_table.hpp"
#include "analysis/semantic/type_rules.hpp"
#include "core/substrate/large_stack_call.hpp"  // D-PARSE-DEEP-FRONTEND-STACK: run analyze on a large stack
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
#include "core/types/char_decode.hpp"  // C 6.4.5: decodeStringLiteralBody (string-literal typing)
#include "core/types/string_literal_decode.hpp"  // C 5.1.1.2 phase 6: decodeAdjacentStringBodies (THE chokepoint, shared with HIR)
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
    // C 6.4.5: the string-literal body token kind + its element core (e.g. Char),
    // diverted here from a `literalTypes` row with `stringArray:true` — a string's
    // type is a PER-OCCURRENCE `Array<core, N+1>`, not a fixed TypeId. Unset (invalid
    // token / Void core) when the language declares no string-array row (toy/tsql).
    SchemaTokenId                                  stringLiteralBodyToken{};
    TypeKind                                       stringLiteralElementCore = TypeKind::Void;
    // C 5.1.1.2 phase 6 (D-CSUBSET-ADJACENT-STRING-CONCAT): the rule whose
    // subtree is a (possibly adjacent-concatenated) string-literal expression.
    // When valid, the string's `Array<core, N+1>` type is stamped on this RULE
    // NODE (decoding every body child via the SHARED chokepoint), NOT on each
    // body TOKEN — so `"a" "b"` types as ONE Array<Char,3>, not two separate
    // single-piece arrays. `subtreeType` checks `typeAt(node)` first, so the
    // stamped rule-node type wins and descent stops. Invalid (no such rule) ⇒
    // the per-token fallback still fires (toy/tsql, or a future grammar with no
    // stringLiteralExpr rule). Resolved by NAME at the index-build call site —
    // source-agnostic (any grammar with a `stringLiteralExpr` rule opts in).
    RuleId                                         stringLiteralExprRule{};
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
          nodeToType{cu},
          nullPointerConstantNodes{cu} {}

    DiagnosticReporter         reporter;
    TypeLattice                lattice;
    ScopeTree                  scopes;
    SymbolTable                symbols;
    UnitAttribute<SymbolId>    nodeToSymbol;
    UnitAttribute<TypeId>      nodeToType;
    // R2 (D-SEMANTIC-NULL-CONSTANT-FOLDING): source nodes admitted as a null-
    // pointer constant via the FOLDED path — a non-literal integer constant
    // expression that folds to 0 (`1-1`, `-0`). The HIR lowerer materializes a
    // synthetic Literal 0 in their place (the literal-0 path needs no marker: the
    // coerce arm admits a real Literal directly). MUST be a TREE-KEYED
    // UnitAttribute (NOT a flat NodeId.v set): NodeId is tree-LOCAL, so a
    // multi-source CU's trees each restart numbering at 1 — a flat set would alias
    // node K across files → a cross-tree false positive → a silent miscompile (an
    // unrelated expression replaced by Literal 0). UnitAttribute routes per-tree,
    // exactly like nodeToType/nodeToSymbol.
    UnitAttribute<bool>        nullPointerConstantNodes;
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
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): the active CC's va_list lowering
    // strategy (`analyze()`'s parameter). Selects the injected `va_list` TYPE AND
    // the `va_start`/`va_arg`/`va_end` `ap`-operand type check: SysVRegisterSave (or
    // nullopt) ⇒ `ap` is `__va_list_tag[1]`/`*`; HomogeneousPointer (Win64) ⇒ `ap`
    // is `char*`. `nullopt` ⇒ the SysV-family default (back-compat).
    std::optional<VaListStrategy> vaListStrategy;
    // c8: the active target's object-format (`analyze()`'s param) — gates
    // per-target shipped-header availability. `nullopt` ⇒ no gate (back-compat).
    std::optional<ObjectFormatKind> activeFormat;
    // Plan 25: the active target's ARCH NAME (`analyze()`'s param — `target.name()`).
    // The per-target shipped-struct `variants` selector: a struct's field list is
    // chosen by (activeTarget, activeFormat) so its byte layout is correct per
    // target. `nullopt` ⇒ no variant selection (back-compat: flat-`fields` structs
    // decode as before; a variants-only struct is not injected).
    std::optional<std::string> activeTarget;
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
    // D-CSUBSET-FN-PROTOTYPE: function REDECLARATIONS Pass 1 merged — one
    // {survivor, absorbed} pair per (proto→def / def→proto / proto→proto /
    // extern→proto) merge. After Pass 1.5 resolves both records' FnSig types, a
    // compatibility sweep compares each pair's `type` (interner TypeId equality =
    // structural signature equality) and emits S_IncompatibleRedeclaration on a
    // mismatch. Built in Pass 1, consumed once after Pass 1.5 (before Pass 2).
    std::vector<std::pair<SymbolId, SymbolId>> mergedFnDecls;

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

// c25 D-CSUBSET-UNIFIED-COMPOSITE-SPECIFIER: does this node have a VISIBLE CHILD
// of the given rule? The dual-mode discriminator for the unified composite
// specifier — `structSpec` is a DEFINITION when it has a `structBody` child,
// else a tag REFERENCE. Generic + agnostic: keyed on a config-resolved RuleId,
// never a keyword. Direct-children only (the body child is always a direct child
// of the specifier node — never nested), matching the `definesWhenChildRule`
// contract.
[[nodiscard]] bool
nodeHasVisibleChildOfRule(Tree const& tree, NodeId node, RuleId childRule) {
    if (!childRule.valid()) return false;
    for (NodeId c : visibleChildren(tree, node)) {
        if (tree.kind(c) == NodeKind::Internal && tree.rule(c) == childRule) {
            return true;
        }
    }
    return false;
}

// c25: is the declaration `decl` a DEFINITION at THIS node? A row without the
// dual-mode gate is ALWAYS a definition (every shipped decl before c25). A gated
// row (`definesWhenChildRule`, the unified composite specifier) is a definition
// ONLY when the body child is present; absent ⇒ the node is a pure tag reference
// (handled by the paired `references[]` row), so the declaration paths (mint,
// scope-open, tag-bind, fieldChildren) MUST all skip it.
[[nodiscard]] bool
isDefinitionAtNode(DeclarationRule const& decl, Tree const& tree, NodeId node) {
    if (!decl.definesWhenChildRule.has_value()) return true;
    return nodeHasVisibleChildOfRule(tree, node, *decl.definesWhenChildRule);
}

// D-CSUBSET-FN-PROTOTYPE: does the declarator NAME carry a function suffix —
// i.e. is `nameNode` the name of a function declarator (`int f(int)` /
// `int f(int x){…}`) rather than a function POINTER (`int (*fp)(int)`)? True
// iff the name's DIRECT declarator (its parent) is the config's `directRule`
// AND has a `fnSuffixRule` child. For a function pointer the suffix sits on the
// OUTER declarator (the name's direct declarator is the inner group's, which
// carries no suffix), so this cleanly distinguishes a prototype from a fnptr.
// Shared by Pass 1 (proto detection) and Pass 1.5 (definition's fn-declarator
// constraint check) so the two never diverge.
[[nodiscard]] bool
hasFnSuffixOnName(Tree const& tree, NodeId nameNode,
                  DeclaratorConfig const& dc) {
    NodeId const direct = tree.parent(nameNode);
    if (!direct.valid() || tree.kind(direct) != NodeKind::Internal
        || tree.rule(direct) != dc.directRule) {
        return false;
    }
    for (NodeId c : visibleChildren(tree, direct)) {
        if (tree.kind(c) == NodeKind::Internal
            && tree.rule(c) == dc.fnSuffixRule) {
            return true;
        }
    }
    return false;
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

// R1: the SINGLE source for member-access (`obj.field` / `ptr->field`) field-type
// resolution, shared by the Pass-2 member arm (which adds diagnostics + symbol
// binding) and `subtreeType` (Pass 1.5, which needs only the field TYPE — e.g.
// `sizeof(s.y)` in an array dimension). It resolves the access and reports the
// OUTCOME so each caller reacts: Pass-2 emits the matching diagnostic + binds the
// SymbolId; subtreeType takes `fieldType` on `Ok` (InvalidType otherwise — the
// array length then fails loud, never a guessed size). AGNOSTIC: drives entirely
// off the config `memberAccesses` table + `compositeScopeByType`; no rule/op
// identity is hardcoded.
struct MemberResolution {
    enum class Status {
        NotMemberAccess,  // node's rule is not a configured member-access (or op-token mismatch)
        LhsUntyped,       // LHS type could not be resolved (chained error — caller stays quiet)
        NotAPointer,      // arrow form `p->x` on a non-pointer LHS
        NotAComposite,    // effective type has no struct scope
        BadNameNode,      // nameChild did not resolve to an identifier leaf
        UndeclaredField,  // field name not found in the struct scope
        Ok,               // field binding found (fieldType may still be invalid if the field's own type is unresolved)
    };
    Status      status        = Status::NotMemberAccess;
    TypeId      fieldType      {};       // valid iff Ok AND the field's type resolved
    SymbolId    fieldSym       {};       // valid iff Ok
    NodeId      nameChildNode  {};       // raw nameChild (span for NotAPointer/NotAComposite/BadNameNode)
    NodeId      nameNode       {};       // extracted identifier leaf (span for UndeclaredField + binding target)
    std::string fieldName;               // its text (UndeclaredField diag actual)
    bool        dereferences   = false;  // arrow form (NotAComposite message disambiguation)
};
[[nodiscard]] MemberResolution
resolveMemberAccess(EngineState const& s, SemanticConfig const& cfg,
                    Tree const& tree, NodeId node, ScopeId scope);

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
    EngineState& s, Tree const& tree,
    TypeId lhsTy, NodeId rhsExpr,
    SemanticConfig::PointerConversionRules const& rules,
    ScopeId scope, SemanticConfig const& cfg);

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
        // A string-literal row carries an ELEMENT core (Char) + a per-occurrence
        // Array<core, N+1> type — divert it to the dedicated index field (NOT the
        // fixed literalTypeIds map, which holds one TypeId per token).
        if (lt.stringArray) {
            idx.stringLiteralBodyToken   = lt.literal;
            idx.stringLiteralElementCore = lt.core;
            continue;
        }
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

// c26 D-CSUBSET-ABSTRACT-DECLARATOR-TYPE-NAME: forward declaration — the shared
// `directRule`-folding engine (defined alongside `declaratorDeclaredType` below)
// is consumed early by `resolveTypeNodeImpl`'s type-name path to fold an abstract
// fn-ptr/array declarator (`(*)(void)`/`[N]`) onto a cast/sizeof base type.
[[nodiscard]] TypeId
directDeclaredType(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                   NodeId direct, TypeId base, ScopeId scope, bool emitOnMiss,
                   bool allowFlexibleArray);

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
        //
        // c25 D-CSUBSET-UNIFIED-COMPOSITE-SPECIFIER: a dual-mode specifier
        // (`structSpec`) in type position with its body ABSENT (`struct S v;`)
        // is a tag REFERENCE, NOT an inline definition — it minted no symbol, so
        // this arm must NOT fire. `isDefinitionAtNode` gates it OUT so resolution
        // falls through to the isTagReference arm below (the body-absent form's
        // SOLE resolution path), exactly as the former `structTypeRef` resolved.
        {
            auto declIt = s.idx().declByRule.find(rule.v);
            if (declIt != s.idx().declByRule.end()
                && cfg.declarations[declIt->second].kind
                       == DeclarationKind::Type
                && isDefinitionAtNode(cfg.declarations[declIt->second], tree, node)) {
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
        // C 6.2.3 tag namespace (MF-1): a TAG reference in type position —
        // `struct Foo` / `union Foo` / `enum Foo`, the node reached by
        // descending into a reference rule flagged `isTagReference`. Resolve
        // the tag identifier against the TAG namespace, NOT ordinary. WITHOUT
        // this explicit arm the generic child-descent below would bottom out
        // at the bare Identifier token and the Token-leaf arm would look it up
        // ORDINARY (line ~835) — which, now that composite tags bind into the
        // Tag namespace, would miss every tag and (worse) could resolve a
        // same-named ORDINARY typedef as the tag's type. A miss here falls
        // through to the generic miss path so `struct Nope x;` still emits
        // S_UnknownType. The bind side is namespace-routed by `fieldChildren`;
        // this is its lookup counterpart, both config-driven (no hardcoded
        // keyword). `emitOnMiss=false` recursive calls (e.g. the pointer-base
        // descent) keep their soft-miss semantics — the arm only RESOLVES, it
        // does not emit.
        {
            auto refIt = s.idx().refByRule.find(rule.v);
            if (refIt != s.idx().refByRule.end()
                && cfg.references[refIt->second].isTagReference
                && scope.valid()) {
                auto const& ref = cfg.references[refIt->second];
                auto rn = extractNameNode(tree, node, ref.nameMatch,
                                          cfg.identifierToken,
                                          cfg.bracketIdentifierToken);
                if (!rn.name.empty()) {
                    SymbolId const tagSym =
                        s.scopes.lookup(scope, rn.name, SymbolNamespace::Tag);
                    if (tagSym.valid()) {
                        auto const& rec = s.symbols.at(tagSym);
                        if (rec.kind == DeclarationKind::Type && rec.type.valid()) {
                            return rec.type;
                        }
                    }
                }
                // Tag miss: fall through to the generic miss arm (below) so an
                // undeclared `struct Nope` fails loud with S_UnknownType. Do
                // NOT descend into the StructKeyword/Identifier children (the
                // Identifier would resolve ORDINARY and cross the namespaces).
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
        }
        auto kids = visibleChildren(tree, node);
        // SE-pointers (G5): count `pointerToken` children at THIS node (C
        // declarator stars: `int *p`, `int **p`) and wrap the resolved base type
        // that many times in Ptr. The stars are a flat token run in `typeRef`;
        // the base type comes from the non-star child (typeBase).
        //
        // c21 (D-CSUBSET-VOLATILE-QUALIFIER) fix #1b: this is the CO-LOCATED arm —
        // `typeRefAllowingStruct` / `castTypeRef` where a head qualifier and the
        // stars are FLAT siblings of THIS node (NOT split across a head + a
        // sibling declarator subtree, the form fix #1a covers). Position-aware
        // pointee-volatile reject: a `volatileMarker` token appearing BEFORE the
        // first `pointerToken` (`volatile int *`) makes the POINTEE volatile —
        // REJECT (model B cannot express it; the volatility would ride the deref).
        // A volatile token AFTER the last star (`int * volatile`, east) makes the
        // POINTER OBJECT volatile — ACCEPT (threaded at the ref site). The walk
        // below tracks star position so the two are distinguished structurally,
        // config-driven on `cfg.volatileMarker` + `cfg.pointerToken` (no hardcoded
        // keyword). Done as a first pass so the reject fires whether or not the
        // base type resolves.
        if (cfg.volatileMarker.has_value() && cfg.pointerToken.has_value()) {
            // Position-aware: walk the FLAT child run left-to-right. The first
            // `pointerToken` direct child marks the star run; a `volatileMarker`
            // appearing in ANY child BEFORE it (the qualifier may be wrapped in a
            // `headQualifier`-style sub-rule, so scan each child's SUBTREE, not
            // just direct Token children) means the POINTEE is volatile → reject.
            // A volatile in a child AFTER the first star (east) is the pointer
            // OBJECT's — left for the declarator/object path to thread.
            bool sawStar = false;
            bool volBeforeStar = false;
            for (auto child : kids) {
                bool const isStarTok = tree.kind(child) == NodeKind::Token
                    && tree.tokenKind(child) == *cfg.pointerToken;
                if (isStarTok) { sawStar = true; continue; }
                if (!sawStar
                    && subtreeContainsToken(tree, child, *cfg.volatileMarker,
                                            &s.idx().declByRule)) {
                    volBeforeStar = true;
                }
            }
            if (volBeforeStar && sawStar) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::S_VolatilePointeeNotSupported;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = tree.source().id();
                d.span     = tree.span(node);
                d.actual   = std::string{tree.text(node)};
                s.reporter.report(std::move(d));
                return InvalidType;
            }
        }
        std::uint32_t ptrDepth = 0;
        TypeId inner = InvalidType;
        NodeId absDirect{};   // c26: an abstract-declarator type-name tail
        for (auto child : kids) {
            if (cfg.pointerToken.has_value()
                && tree.kind(child) == NodeKind::Token
                && tree.tokenKind(child) == *cfg.pointerToken) {
                ++ptrDepth;
                continue;
            }
            // c26 D-CSUBSET-ABSTRACT-DECLARATOR-TYPE-NAME: a `directAbstractRule`
            // child of a type-resolved node is an ABSTRACT declarator in a
            // type-name position (cast / sizeof / compound-literal / va_arg — C
            // 6.7.7). It is the dedicated abstract twin (Identifier base excluded)
            // so a parenthesized multiplication `(c * c)` never reaches here; and
            // NO declaration path produces one (declarations fold a `directRule`
            // declarator SEPARATELY via `declaratorDeclaredType`). Captured now,
            // folded after the base resolves (base + stars = the element type).
            if (cfg.declarators.has_value()
                && cfg.declarators->directAbstractRule.has_value()
                && tree.kind(child) == NodeKind::Internal
                && tree.rule(child) == *cfg.declarators->directAbstractRule
                && !absDirect.valid()) {
                absDirect = child;
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
            // c26: fold the abstract declarator (fn-ptr / array type-name) onto the
            // base+stars via the SHARED `directDeclaredType` engine — the SAME path
            // a declaration's declarator takes, so `(int(*)(void))` yields exactly
            // the param-position type `Ptr<Fn(void)->int>`. A type-name MUST be
            // abstract (C 6.7.7): a NAME on the declarator (`(int x)expr`) is a
            // constraint violation — fail LOUD (never silently drop the name and
            // mistype as the bare base). `emitOnMiss` threads the diagnostic gate.
            if (absDirect.valid()) {
                if (declaratorNameNode(tree, absDirect, *cfg.declarators)
                        .valid()) {
                    if (emitOnMiss) {
                        ParseDiagnostic d;
                        d.code     = DiagnosticCode::
                            S_TypeNameDeclaratorNotAbstract;
                        d.severity = DiagnosticSeverity::Error;
                        d.buffer   = tree.source().id();
                        d.span     = tree.span(absDirect);
                        d.actual   = std::string{tree.text(absDirect)};
                        s.reporter.report(std::move(d));
                    }
                    return InvalidType;
                }
                return directDeclaredType(s, cfg, tree, absDirect, inner, scope,
                                          emitOnMiss,
                                          /*allowFlexibleArray=*/false);
            }
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
        // Item 1: an inline-valued named constant (enum enumerator / shipped-
        // descriptor constant) resolves DIRECTLY to its literal — no init-CST.
        // Tried before resolveSymbolInit so `int a[CHAR_BIT]` / `int b[ENUM_VAL]`
        // fold at this Pass-1.5 array-dimension stage. Shares the ONE
        // `constantLiteralForSymbol` builder with the HIR Ref fold.
        env.resolveSymbolValue = [&s, &tree](NodeId identTok, std::uint32_t curScopeOpaque)
            -> std::optional<HirLiteralValue> {
            SymbolId const sym =
                s.scopes.lookup(ScopeId{curScopeOpaque}, tree.text(identTok));
            if (!sym.valid()) return std::nullopt;
            return constantLiteralForSymbol(s.symbols.at(sym), s.lattice.interner());
        };
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

// FC8 D-CSUBSET-BITFIELD: if `decl` configures a bit-field suffix and a node of
// that rule appears in the field subtree, resolve the field's declared bit-width.
// Returns `present` (a `: W` suffix exists on this field) + `width` (the
// VALIDATED width, set only when valid). A bit-field's TYPE is UNCHANGED (it
// stays its declared integer type — the width is a layout/codegen property
// carried on the struct type's scalars, not a type wrapper like an array). Fails
// loud (leaving `width` unset) on: a non-integer base (C 6.7.2.1p5), a width
// that is negative / exceeds the base type's bit-size (p4), or a zero width on a
// NAMED field (p3 — a zero-width bit-field must be anonymous).
struct BitfieldResolution {
    bool                         present = false;   // a `: W` suffix exists
    std::optional<std::uint32_t> width;             // validated width (0 = anon zero-width)
};

[[nodiscard]] BitfieldResolution
resolveBitfieldSuffix(EngineState& s, Tree const& tree, DeclarationRule const& decl,
                      NodeId declNode, TypeId fieldType, bool hasName,
                      ScopeId fromScope, SemanticConfig const* cfg) {
    BitfieldResolution out;
    if (!decl.bitfieldSuffix.has_value()) return out;
    BitfieldSuffix const& bs = *decl.bitfieldSuffix;
    // Bounded descendant search for the suffix rule (mirror applyArraySuffix).
    NodeId suffix{};
    std::vector<NodeId> stack{declNode};
    for (int guard = 0; guard < 8192 && !stack.empty(); ++guard) {
        NodeId c = stack.back(); stack.pop_back();
        if (tree.kind(c) != NodeKind::Internal) continue;
        if (tree.rule(c) == bs.rule) { suffix = c; break; }
        for (NodeId g : visibleChildren(tree, c)) stack.push_back(g);
    }
    if (!suffix.valid()) return out;   // no `: W` on this field
    out.present = true;
    auto emit = [&](DiagnosticCode code) {
        ParseDiagnostic d;
        d.code     = code;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(suffix);
        d.actual   = std::string{tree.text(suffix)};
        s.reporter.report(std::move(d));
    };
    if (!fieldType.valid()) return out;   // unresolved base — upstream already loud
    // FC8 D-CSUBSET-ENUM-BITFIELD: an enum-typed bit-field (`enum E e : 3;`) is
    // permitted (C 6.7.2.1) — an enum behaves AS its underlying integer
    // (D-CSUBSET-ENUM-INT-CONVERSION), so validate the width against the
    // UNDERLYING's bit-size (a >32-bit underlying still hits the wide-unit
    // reject below, consistent with a plain `long` bit-field). Non-enum types
    // pass through unchanged.
    using namespace detail::type_rules;
    TypeId const reprType =
        enumUnderlyingOrSelf(s.lattice.interner(), fieldType);
    TypeKind const k = s.lattice.interner().kind(reprType);
    // The base type's bit-size (fixed-width integer kinds only; 0 ⇒ non-integer).
    auto intBits = [](TypeKind kk) -> std::uint32_t {
        switch (kk) {
            case TypeKind::Bool: case TypeKind::Char:
            case TypeKind::I8:   case TypeKind::U8:   return 8;
            case TypeKind::I16:  case TypeKind::U16:  return 16;
            case TypeKind::I32:  case TypeKind::U32:  return 32;
            case TypeKind::I64:  case TypeKind::U64:  return 64;
            case TypeKind::I128: case TypeKind::U128: return 128;
            default:                                  return 0;
        }
    };
    std::uint32_t const typeBits = intBits(k);
    if (typeBits == 0) { emit(DiagnosticCode::S_BitFieldNonIntegerType); return out; }
    // A bit-field on a >32-bit base (`long`/`long long`/I64/U64) needs a 64-bit
    // allocation-unit access. D-CSUBSET-BITFIELD-WIDE-UNIT (v0.0.2 FC8) closed the
    // last codegen gap — materializing a 64-bit constant > int32 (the wide-mask
    // dead-end): the x86 backend now emits `mov r64, imm64` (REX.W B8+rd io) and
    // arm64 the MOVZ/MOVK ladder, both capability-probed in MIR→LIR. The
    // extract/insert shapes (Load/Store/And/Or/Shl/LShr/AShr @64) already
    // encoded, so 64-bit-base bit-fields now compile + run end-to-end. I128/U128
    // bit-fields stay rejected — there is no 128-bit allocation-unit codegen (no
    // 128-bit `mov`/ALU forms). 8/16/32/64-bit integer bases are the supported set.
    if (typeBits > 64) { emit(DiagnosticCode::S_BitFieldWidthOutOfRange); return out; }
    NodeId widthNode{};
    if (bs.widthChild.has_value()) {
        auto sufKids = visibleChildren(tree, suffix);
        if (*bs.widthChild < sufKids.size()) widthNode = sufKids[*bs.widthChild];
    }
    auto w = constIntExpr(s, tree, widthNode, fromScope, cfg);
    if (!w.has_value() || *w < 0
        || static_cast<std::uint64_t>(*w) > typeBits) {
        emit(DiagnosticCode::S_BitFieldWidthOutOfRange); return out;
    }
    if (*w == 0 && hasName) {   // C 6.7.2.1p3: a zero-width bit-field has no name
        emit(DiagnosticCode::S_BitFieldWidthOutOfRange); return out;
    }
    out.width = static_cast<std::uint32_t>(*w);
    return out;
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
                       ScopeId scope, bool emitOnMiss,
                       bool allowFlexibleArray = false);

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
                      ScopeId scope, bool emitOnMiss,
                      bool allowFlexibleArray = false) {
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
            // FC12a-core (D-FC12A-VARIADIC-CALLEE): a `...` in this param list makes
            // the FnSig C-style variadic. This SHARED suffix resolver is the path a
            // function DEFINITION (`T f(int n, ...) {...}`) + a fn-pointer type take —
            // the legacy decl-only path scanned its per-rule marker but this one did
            // NOT, so a variadic DEFINITION built a non-variadic FnSig and every call
            // to it tripped S_ArgCountMismatch. Scan here (stopping at nested decl
            // rules, like the legacy path) so the def + decl agree on variadic-ness.
            if (paramsList.valid() && dc.variadicMarker.has_value()
                && subtreeContainsToken(tree, paramsList, *dc.variadicMarker,
                                        &s.idx().declByRule)) {
                normalizeSoleVoidParams(s, cfg, tree, params, emitOnMiss);
                std::vector<TypeId> vt;
                vt.reserve(params.size());
                for (auto const& [pNode, pTy] : params) vt.push_back(pTy);
                return s.lattice.interner().fnSig(vt, inner, CallConv::CcSysV,
                                                  /*isVariadic=*/true);
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
            // c10 D-CSUBSET-STRUCT-MEMBER-DECLARATOR (FAM): an ABSENT length on
            // a declaration form that may bear a flexible array member (a
            // struct field) is an INCOMPLETE array (C99 §6.7.2.1), NOT an
            // error — the declarator-mode twin of the legacy `applyArraySuffix`
            // FAM branch. Only the field's OWN array suffix inherits this; a
            // nested fn-ptr param's array never does (the caller passes false
            // into the group recursion).
            if (allowFlexibleArray)
                return s.lattice.interner().incompleteArray(inner);
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

// Fold ONE `directRule` node (`directDeclarator` — the name/group base + its
// fn/array suffixes) onto the accumulated `base` type. Factored out of
// `declaratorDeclaredType` (the part AFTER its pointer-layer loop) so it can be
// reused VERBATIM by the cast/sizeof/compound-literal/va_arg type-name resolver
// (c26 D-CSUBSET-ABSTRACT-DECLARATOR-TYPE-NAME): there the abstract declarator
// IS a `directDeclarator` (the leading stars are consumed by the type-name's own
// `{repeat StarOp}` and folded into `base` before this is called), so there is no
// `declarator` wrapper node to hand `declaratorDeclaredType`. ONE engine, two
// entry points — the abstract type-name fold and the declaration-position fold
// can never drift. `base` already carries the pointer-star depth.
[[nodiscard]] TypeId
directDeclaredType(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                   NodeId direct, TypeId base, ScopeId scope, bool emitOnMiss,
                   bool allowFlexibleArray) {
    if (!cfg.declarators.has_value()) return InvalidType;
    DeclaratorConfig const& dc = *cfg.declarators;
    if (!direct.valid() || !base.valid()) return InvalidType;
    if (tree.kind(direct) != NodeKind::Internal) return InvalidType;
    // Accept the concrete `directRule` (declaration position) OR its abstract
    // twin `directAbstractRule` (c26 type-name position) — both expose the SAME
    // group/fnSuffix/arraySuffix child rules, so the fold below is identical. The
    // abstract twin simply cannot carry a top-level name token (Identifier base
    // excluded by grammar), so the `nameTok` scan below stays empty for it and a
    // name only appears nested in its group (caught by the caller's reject).
    RuleId const directR = tree.rule(direct);
    bool const isDirect = directR == dc.directRule
        || (dc.directAbstractRule.has_value()
            && directR == *dc.directAbstractRule);
    if (!isDirect) return InvalidType;
    TypeId t = base;

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
                                  emitOnMiss, allowFlexibleArray);
        if (!t.valid()) return InvalidType;
    }

    if (nameTok.valid()) return t;   // bound at the name
    if (group.valid()) {
        NodeId const inner = declarator_walk_detail::firstChildOfRule(
            TreeDeclaratorView{tree}, group, dc.declaratorRule);
        if (!inner.valid()) return InvalidType;   // malformed group
        // GOTCHA (c10 plan-lock): do NOT propagate the field's FAM-ness into a
        // grouped/parenthesized inner declarator — a nested fn-ptr param's
        // array (`int (*f)(int x[]))`) is its OWN declarator and must keep the
        // ordinary `S_NonConstantArrayLength` behavior; only the field's
        // top-level array suffix is a flexible array member.
        return declaratorDeclaredType(s, cfg, tree, inner, t, scope,
                                      emitOnMiss, /*allowFlexibleArray=*/false);
    }
    return t;   // abstract direct — the type itself
}

TypeId declaratorDeclaredType(EngineState& s, SemanticConfig const& cfg,
                              Tree const& tree, NodeId node, TypeId base,
                              ScopeId scope, bool emitOnMiss,
                              bool allowFlexibleArray) {
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
                                      emitOnMiss, allowFlexibleArray);
    }
    // c23 (D-CSUBSET-STRUCT-MULTI-DECLARATOR) FIX 1: a struct/union member-list
    // slot wraps ONE declarator (+ its own bitfield suffix). Descend to the
    // inner declaratorRule and recurse — identical to the initDeclaratorRule arm
    // above. WITHOUT this arm a `structMemberDeclarator` node falls through to
    // the `r != dc.declaratorRule` reject below → InvalidType for EVERY field →
    // the struct never composes (H_TypeUnresolved); the feature (and every
    // single-declarator struct now routed through the member list) is dead.
    // Each slot takes the head `base` TypeId BY VALUE into the append-only
    // interner, so a per-slot star (`int *a, b;` → Ptr<int> then int) cannot
    // leak across slots — the crux is correct by construction. An ABSENT inner
    // declarator (the anonymous bit-field `int : 3;`) yields InvalidType here,
    // which is fine: the anonymous-field path (~2435) types it from `headTy`,
    // never from this declTy.
    if (dc.memberDeclaratorRule.has_value()
        && r == *dc.memberDeclaratorRule) {
        NodeId const inner = declarator_walk_detail::firstChildOfRule(
            TreeDeclaratorView{tree}, node, dc.declaratorRule);
        if (!inner.valid()) return InvalidType;
        return declaratorDeclaredType(s, cfg, tree, inner, base, scope,
                                      emitOnMiss, allowFlexibleArray);
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

    // The direct's base (name token or group) + suffixes — folded by the
    // SHARED `directDeclaredType` engine (the same one the abstract type-name
    // resolver uses; see its comment). `t` already carries the pointer-star
    // depth from the loop above.
    return directDeclaredType(s, cfg, tree, direct, t, scope, emitOnMiss,
                              allowFlexibleArray);
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

// Float a scope reference up to the nearest enclosing NAMESPACE scope (a block
// or the file/CU root), past any DECLARATOR-DOMINATOR scope — a `declarations`-
// rule scope that declares no members of its own (no fieldChildren). C11 6.2.1:
// a struct/union/enum TAG, and an enum's enumerators, declared as a type
// specifier belong to the enclosing block or file scope, NOT a transient
// interior declaration scope. Some declaration rules open such a scope purely to
// dominate a later sibling (c-subset's topLevelDecl opens one so a function's
// params — living inside its declarator's fnSuffix — reach the body block); a
// tag/enumerator minted from a specifier in that scope would VANISH when it pops
// (invisible to the next declaration). Driven by declByRule/fieldChildren
// membership — no rule-name identity branch. STOPS at a composite-body scope (a
// member namespace), a block, or the file/CU root.
ScopeId floatToNamespaceScope(EngineState const& s, SemanticConfig const& cfg,
                              Tree const& tree, ScopeId scope) {
    auto const& scs = s.scopes.scopes();
    while (scope.valid() && scope.v < scs.size()) {
        NodeId const anchorN = scs[scope.v].anchor;
        if (!anchorN.valid() || scs[scope.v].tree.v != tree.id().v)
            break;  // file/CU root — the namespace
        auto dIt = s.idx().declByRule.find(tree.rule(anchorN).v);
        if (dIt == s.idx().declByRule.end())
            break;  // a block / non-declaration scope
        if (cfg.declarations[dIt->second].fieldChildren.has_value())
            break;  // a composite body — a member namespace
        scope = scs[scope.v].parent;  // skip the declarator dominator
    }
    return scope;
}

// D-CSUBSET-BLOCK-SCOPE-PROTOTYPE: the FILE (translation-unit) scope of `tree` —
// the scope whose anchor IS this tree's root node. Each tree's Pass 1 runs under
// a per-tree root scope (`pushScope(builtinScope, tree.root(), tree.id())`), so
// walking the parent chain from any interior scope until the anchor equals the
// tree root reaches that file scope. Used to RE-HOME a block-scope function
// declaration onto the file scope (C 6.2.2p4 — a no-linkage block-scope
// declaration of a function refers to the file-scope function with external
// linkage). Returns `scope` unchanged if it is already the file scope, or if the
// chain is exhausted without a match (a malformed scope graph — fail-safe: bind
// where we are, the prior behavior).
[[nodiscard]] ScopeId fileScopeOf(EngineState const& s, Tree const& tree,
                                  ScopeId scope) {
    auto const& scs = s.scopes.scopes();
    NodeId const treeRoot = tree.root();
    ScopeId cur = scope;
    while (cur.valid() && cur.v < scs.size()) {
        auto const& rec = scs[cur.v];
        if (rec.tree.v == tree.id().v && rec.anchor.v == treeRoot.v) return cur;
        cur = rec.parent;
    }
    return scope;
}

// D-CSUBSET-FN-PROTOTYPE + D-CSUBSET-EXTERN-DEFINITION-MERGE: resolve a same-scope
// REDECLARATION (`prior` already bound `name` in `bindScope`; `newId` is the new
// symbol). A redeclaration MERGES instead of colliding when both sides name the
// SAME entity (both functions OR both objects) and are NOT both definitions. A
// NON-DEFINING declaration — a bare prototype OR an `extern` declaration — names a
// body/storage that lives elsewhere (or later). The cases, on whether each side is
// non-defining:
//   nonDefining → definition : the DEFINITION wins the binding; the non-defining
//                 decl is absorbed (proto→def, extern→def).
//   definition → nonDefining / nonDefining → nonDefining : a redundant declaration;
//                 KEEP the prior binding, absorb the new one (def→proto, def→extern,
//                 proto→proto, extern→extern, proto↔extern).
//   definition → definition : two bodies / two storage definitions (incl. two
//                 `int g;` tentative defs) → S_RedeclaredSymbol.
// CATEGORY GUARD: a non-defining declaration MERGES only with a declaration of the
// SAME declaration category — Function, Variable, Type, or Table. A function and an
// OBJECT (Variable), a typedef (Type) and an object/function, or any future Table vs
// object of the same name are DIFFERENT categories → a genuine collision, never a
// merge. A signature/type mismatch on a merged pair fails loud AFTER Pass 1.5
// (S_IncompatibleRedeclaration) once both types resolve.
//
// The category is the PRECISE DeclarationKind, with one normalization: a bare
// prototype is minted Variable-kind + isProtoDeclaration in Pass 1 (Pass 1.5 later
// upgrades its KIND to Function), so the lambda maps either signal — Function-kind
// or isProtoDeclaration — to Function. Variable / Type / Table stay DISTINCT — a coarse
// function-vs-non-function split would lump a typedef (Type) and a same-named extern
// object (Variable) into one category and silently absorb the extern into the typedef
// (`typedef int g; extern int g;`), losing the C 6.7p4 fail-loud.
//
// Shared by BOTH Pass-1 minting paths (declarator-mode and legacy positional) so a
// proto/extern and its definition merge regardless of which path mints each — e.g.
// c-subset's `extern` (positional) and its definition (declarator-mode topLevelDecl).
// Both paths mint `newId` with its final `kind`/`isProtoDeclaration` BEFORE this call,
// so the category is read directly from each record. `newNonDef` = the new decl is
// non-defining (proto or extern).
void mergeOrCollideRedeclaration(EngineState& s, Tree const& tree,
                                 ScopeId bindScope, std::string const& name,
                                 NodeId nameNode, SymbolId prior, SymbolId newId,
                                 bool newNonDef) {
    auto& priorRec = s.symbols.at(prior);
    bool const priorNonDef =
        priorRec.isProtoDeclaration || priorRec.isExternDeclaration;
    // Precise declaration category: a proto (kind Variable + isProtoDeclaration,
    // pre-upgrade) counts as Function; Variable / Type / Table stay distinct.
    auto category = [](SymbolRecord const& r) {
        if (r.kind == DeclarationKind::Function || r.isProtoDeclaration)
            return DeclarationKind::Function;
        return r.kind;
    };
    bool const sameCategory = category(priorRec) == category(s.symbols.at(newId));
    bool const bothDefinitions = !priorNonDef && !newNonDef;
    if (sameCategory && !bothDefinitions) {
        if (priorNonDef && !newNonDef) {
            // nonDefining → definition: the DEFINITION wins the binding; the prior
            // non-defining decl is absorbed (a proto/extern declarator emits no HIR
            // node — see the topLevelDecl proto-skip and lowerExternDecl's
            // absorbed-skip).
            s.scopes.injectBinding(bindScope, name, newId);
            priorRec.isAbsorbedProto = true;
            s.nodeToSymbol.set(nameNode, newId);
            s.mergedFnDecls.push_back({newId, prior});
        } else {
            // definition → nonDefining / nonDefining → nonDefining: a redundant
            // declaration. Keep the PRIOR binding; absorb the NEW one. Bind the
            // absorbed declarator's NAME node to its OWN record (newId), NOT the
            // survivor — newId carries the proto/extern flag the HIR skip reads, and
            // Pass 1.5 resolves THIS declarator's signature into newId so the post-1.5
            // sweep compares survivor.type vs absorbed.type with BOTH valid (catches
            // an incompatible def→proto / def→extern). Aiming at `prior` would clobber
            // the survivor's resolved type and hide the mismatch.
            s.symbols.at(newId).isAbsorbedProto = true;
            s.nodeToSymbol.set(nameNode, newId);
            s.mergedFnDecls.push_back({prior, newId});
        }
    } else {
        // Two real definitions (incl. two tentative object defs), or a cross-category
        // (function vs object) redeclaration — a genuine collision.
        ParseDiagnostic d;
        d.code     = DiagnosticCode::S_RedeclaredSymbol;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(nameNode);
        d.actual   = name;
        if (priorRec.tree.v == tree.id().v) {
            d.related.push_back(RelatedLocation{
                tree.source().id(),
                tree.span(priorRec.declNode),
                "previously declared here",
            });
        }
        s.reporter.report(std::move(d));
    }
}

// ── Pass 1: pre-order — mint decls + push/pop scopes + const marking ───────
// pass1 PER-NODE work for ONE node — extracted for the iterative driver below
// (D-PARSE-DEEP-NEST-RECURSION-MEMORY plan 24 Stage 2). PRE-order: resolves this
// node's child scope `here` (a pushScope for a scope-rule node, else `current`)
// and binds its declarations; the driver then walks the children under the
// returned `here`. Byte-identical to the prior pre-child body of `pass1`.
[[nodiscard]] ScopeId
pass1Node(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
          NodeId node, ScopeId current) {
    auto const k = tree.kind(node);

    ScopeId here = current;

    if (k == NodeKind::Internal) {
        auto const rule = tree.rule(node);

        // c25 D-CSUBSET-UNIFIED-COMPOSITE-SPECIFIER: a dual-mode declaration row
        // (`definesWhenChildRule`) is a DEFINITION at this node ONLY when its body
        // child is present; absent ⇒ the node is a pure tag reference and the
        // declaration paths (scope-open, mint, tag-bind) ALL skip it. Computed
        // BEFORE the scope-open so a body-absent specifier (`struct S v;`) opens
        // NO scope — the post-order passes re-derive scopes by anchor lookup, so
        // they agree (no scope minted here ⇒ childScopeFor returns `current`).
        bool declIsDefiningHere = true;
        if (auto gIt = s.idx().declByRule.find(rule.v);
            gIt != s.idx().declByRule.end()) {
            declIsDefiningHere =
                isDefinitionAtNode(cfg.declarations[gIt->second], tree, node);
        }

        if (declIsDefiningHere && s.idx().scopeByRule.contains(rule.v)) {
            here = s.scopes.pushScope(current, node, tree.id());
        }

        auto declIt = s.idx().declByRule.find(rule.v);
        if (declIt != s.idx().declByRule.end() && declIsDefiningHere) {
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
                    // c10 D-CSUBSET-STRUCT-MEMBER-DECLARATOR (anonymous fields):
                    // flips true when this declaration binds at least one NAMED
                    // symbol. A field row that binds NONE (every declarator
                    // abstract — `int *;` — OR an absent declarator — `int :3;`
                    // / `int ;`, where `declarators` is empty) re-anchors a
                    // synthetic anonymous symbol below, mirroring the legacy
                    // positional anonymous-field path. Other declarator-mode
                    // rows (params/locals/globals) never set anonymousNameAllowed
                    // so the anon block is inert for them.
                    bool boundNamed = false;
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
                        // D-CSUBSET-FN-PROTOTYPE: a bare function prototype is a
                        // Variable-kind declarator (the kindByChild definition
                        // discriminator did NOT match — no body) whose NAME
                        // carries a function suffix. A fnptr (`int (*fp)(int)`)
                        // is NOT one (its suffix sits on the outer declarator).
                        // Computed BEFORE the bind scope is chosen because a
                        // prototype re-homes onto the file scope (below).
                        bool const isProto =
                            (effectiveKind == DeclarationKind::Variable)
                            && nameNode.valid()
                            && hasFnSuffixOnName(tree, nameNode, *cfg.declarators);
                        // D-CSUBSET-BLOCK-SCOPE-PROTOTYPE: a block-scope function
                        // declaration (`int f(int);` inside a body) has EXTERNAL
                        // linkage and REFERS to the file-scope function of the
                        // same name (C 6.2.2p4 / 6.7.6.3), it does NOT introduce a
                        // distinct block-local function. Re-home the prototype onto
                        // the file (translation-unit) scope so the existing
                        // file-scope proto/definition MERGE (the 4-case direction
                        // table below) resolves it — a later/earlier file-scope
                        // definition WINS the binding and the block proto is
                        // absorbed (a call resolves to the definition). When there
                        // is NO file-scope definition the re-homed proto is still
                        // an unabsorbed file-scope declaration → a call fails loud
                        // at HIR→MIR exactly like a file-scope undefined proto
                        // (consistent fail-loud, not a block-local shadow). A
                        // non-proto declarator binds in `current` unchanged.
                        ScopeId bindScope = current;
                        if (isProto) bindScope = fileScopeOf(s, tree, current);
                        SymbolRecord rec;
                        rec.name         = name;
                        rec.scope        = bindScope;
                        rec.declNode     = nameNode;
                        rec.declRuleNode = node;
                        rec.tree         = tree.id();
                        rec.kind         = effectiveKind;
                        // A function PROTOTYPE is a function DECLARATION, never an
                        // "unused variable" — suppress warnIfUnused for it. This
                        // matters for a BLOCK-scope proto (D-CSUBSET-BLOCK-SCOPE-
                        // PROTOTYPE), whose local declaration rule sets
                        // warnIfUnused; re-homed to file scope and absorbed by the
                        // definition (its own use-set stays empty — the call
                        // resolves to the definition), it would otherwise emit a
                        // spurious S_UnusedVariable. A file-scope proto is
                        // unaffected (topLevelDecl already declares warnIfUnused
                        // false), so this is byte-identical there.
                        rec.warnIfUnused = decl.warnIfUnused && !isProto;
                        rec.fieldIndex   = static_cast<std::uint32_t>(
                            s.scopes.scopes()[bindScope.v].bindings.size());
                        if (decl.constMarker.has_value()) {
                            rec.isConst = subtreeContainsToken(
                                tree, node, *decl.constMarker,
                                &s.idx().declByRule);
                        }
                        // c21 (D-CSUBSET-VOLATILE-QUALIFIER): parallel volatile
                        // scan — INDEPENDENT of isConst (`const volatile` ⇒ both).
                        if (decl.volatileMarker.has_value()) {
                            rec.isVolatile = subtreeContainsToken(
                                tree, node, *decl.volatileMarker,
                                &s.idx().declByRule);
                        }
                        rec.isProtoDeclaration = isProto;
                        // D-CSUBSET-EXTERN-DEFINITION-MERGE: a non-defining
                        // declaration (c-subset's `extern`) — config-driven, no
                        // rule-name identity. Like a prototype it is a non-defining
                        // declaration that MERGES with an in-TU definition.
                        bool const isExtern = decl.nonDefiningDeclaration;
                        rec.isExternDeclaration = isExtern;
                        SymbolId const newId = s.symbols.mint(rec);
                        boundNamed = true;
                        SymbolId const prior =
                            s.scopes.bind(bindScope, name, newId);
                        if (prior.valid()) {
                            // The new decl's category (Function via Function-kind or a
                            // bare proto, else Variable/Type/Table) is read from its
                            // record inside the helper. A real function definition is
                            // Function-kind and non-`isExtern`.
                            mergeOrCollideRedeclaration(
                                s, tree, bindScope, name, nameNode, prior, newId,
                                /*newNonDef=*/isProto || isExtern);
                        } else {
                            s.nodeToSymbol.set(nameNode, newId);
                        }
                    }
                    // c10 D-CSUBSET-STRUCT-MEMBER-DECLARATOR: an ANONYMOUS field
                    // (`int :3;` / `int ;` / over-admitted `int *;`) binds no
                    // named symbol. For an `anonymousNameAllowed` row, mint a
                    // synthetic anonymous field symbol anchored on the DECL node
                    // (`node`) — mirroring the legacy positional anonymous path
                    // (the `<anon:rule:node.v>` re-anchor): an anonymous bit-field
                    // occupies a layout slot, and Pass 1.5 fails loud
                    // (S_DeclarationDeclaresNothing) for a NON-bitfield anonymous
                    // field. Same SymbolRecord shape as the named declarator
                    // above, minus the proto/extern axes (an anonymous field is
                    // never a prototype). Binds into the ENCLOSING struct scope
                    // (`current`) under the Ordinary namespace, exactly like a
                    // named field.
                    if (!boundNamed && decl.anonymousNameAllowed) {
                        SymbolRecord rec;
                        rec.name         = std::format("<anon:{}:{}>",
                                                       decl.ruleName, node.v);
                        rec.scope        = current;
                        rec.declNode     = node;
                        rec.declRuleNode = node;
                        rec.tree         = tree.id();
                        rec.kind         = DeclarationKind::Variable;
                        rec.warnIfUnused = decl.warnIfUnused;
                        rec.fieldIndex   = static_cast<std::uint32_t>(
                            s.scopes.scopes()[current.v].bindings.size());
                        if (decl.constMarker.has_value()) {
                            rec.isConst = subtreeContainsToken(
                                tree, node, *decl.constMarker,
                                &s.idx().declByRule);
                        }
                        // c21 (D-CSUBSET-VOLATILE-QUALIFIER): anonymous-field
                        // volatile scan — parallel to isConst (independent).
                        if (decl.volatileMarker.has_value()) {
                            rec.isVolatile = subtreeContainsToken(
                                tree, node, *decl.volatileMarker,
                                &s.idx().declByRule);
                        }
                        SymbolId const newId = s.symbols.mint(rec);
                        s.scopes.bind(current, rec.name, newId);
                        s.nodeToSymbol.set(node, newId);
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
                    // C11 6.2.1 tag scope: a struct/union/enum TAG declared as
                    // a type specifier belongs to the nearest enclosing block
                    // or file scope — never an interior DECLARATOR-DOMINATOR
                    // scope (e.g. c-subset's topLevelDecl, which opens a scope
                    // only to dominate a function's params). A tag minted from a
                    // file-scope `struct P {…} v;` / `struct Q g;` specifier
                    // would otherwise bind into that transient scope and VANISH
                    // when it pops, invisible to the next declaration. Float
                    // past it via the shared helper; gated on fieldChildren so
                    // only composite TAGS float (a plain typedef-name keeps the
                    // enclosing-scope binding).
                    ScopeId const bindScope =
                        decl.fieldChildren.has_value()
                            ? floatToNamespaceScope(s, cfg, tree, current)
                            : current;
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

                    // D-CSUBSET-SELF-REFERENTIAL-STRUCT: FORWARD-MINT the nominal
                    // composite TypeId NOW (Pass 1, before the body is walked) so a
                    // SELF-REFERENTIAL field inside the body (`struct N *next;`) can
                    // resolve its tag to a VALID `rec.type` at Pass 1.5 field-type
                    // resolution (the tag-reference arm requires `rec.type.valid()`).
                    // Identity = (kind, name, decl-site): the decl-site key packs the
                    // tree id + this declaration's rule node so two distinct
                    // definitions (incl. block-scoped same-name structs) never share
                    // a TypeId. Pass 1.5 ATTACHES the fields via `completeComposite`
                    // on this same TypeId. Struct/Union only — Enum keeps its
                    // value-typed `enumType` path (no self-reference). The forward
                    // type stays INCOMPLETE until Pass 1.5 completes it; a direct
                    // non-pointer member of it then fails loud (incomplete guard).
                    if (rec.structScope.valid()
                        && decl.fieldChildren->compositeKind != CompositeKind::Enum) {
                        TypeKind const compKind =
                            decl.fieldChildren->compositeKind == CompositeKind::Union
                                ? TypeKind::Union
                                : TypeKind::Struct;
                        std::uint64_t const declSiteKey =
                            (static_cast<std::uint64_t>(tree.id().v) << 32)
                            | static_cast<std::uint64_t>(node.v);
                        rec.type = s.lattice.interner().forwardComposite(
                            compKind, resolved.name, declSiteKey);
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
                    // c21 (D-CSUBSET-VOLATILE-QUALIFIER): same type-subtree scan
                    // for volatile — independent of isConst (`const volatile`).
                    if (decl.volatileMarker.has_value()) {
                        NodeId scanRoot = node;
                        if (decl.typeChild.has_value()
                            && *decl.typeChild < kids.size()) {
                            scanRoot = kids[*decl.typeChild];
                        }
                        rec.isVolatile = subtreeContainsToken(
                            tree, scanRoot, *decl.volatileMarker,
                            &s.idx().declByRule);
                    }

                    // D-CSUBSET-EXTERN-DEFINITION-MERGE: c-subset's `extern`
                    // declaration mints through THIS legacy positional path (it
                    // declares a positional `name`, not declarator-mode). Mark it
                    // non-defining so it merges with an in-TU definition of the same
                    // name (handled below via the shared mergeOrCollideRedeclaration).
                    rec.isExternDeclaration = decl.nonDefiningDeclaration;

                    SymbolId const newId = s.symbols.mint(rec);
                    // C 6.2.3 tag namespace: a composite-type declaration
                    // (carries `fieldChildren`, the same gate that floats the
                    // tag past the declarator-dominator scope) binds its tag
                    // into the TAG namespace; a plain typedef-name / object /
                    // function binds Ordinary. This is the ONLY tag-BIND site;
                    // it lets `typedef struct Pair {…} Pair;` mint the tag and
                    // the alias under the same name without collision.
                    SymbolNamespace const bindNs =
                        decl.fieldChildren.has_value()
                            ? SymbolNamespace::Tag
                            : SymbolNamespace::Ordinary;
                    SymbolId const prior =
                        s.scopes.bind(bindScope, resolved.name, newId, bindNs);
                    if (prior.valid()) {
                        // D-CSUBSET-EXTERN-DEFINITION-MERGE: in the ORDINARY
                        // namespace, route a redeclaration through the shared
                        // merge-or-collide so an `extern` minted here merges with a
                        // definition (or another extern) — identical to the
                        // declarator-mode path. A Variable/Type redeclaration with
                        // neither side non-defining still collides (the helper's
                        // bothDefinitions arm), byte-identical to the prior behavior.
                        // The TAG namespace keeps the plain collision (tags have no
                        // proto/extern axis).
                        if (bindNs == SymbolNamespace::Ordinary) {
                            mergeOrCollideRedeclaration(
                                s, tree, bindScope, resolved.name, resolved.node,
                                prior, newId,
                                /*newNonDef=*/decl.nonDefiningDeclaration);
                        } else {
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
                        }
                    } else {
                        s.nodeToSymbol.set(resolved.node, newId);
                    }
                }
            }
        }
    }

    return here;
}

// pass1 — iterative whole-tree PRE-ORDER walk (D-PARSE-DEEP-NEST-RECURSION-
// MEMORY plan 24 Stage 2): an explicit heap work-stack replaces host recursion.
// Each node: skip if invalid / empty-space, else run `pass1Node` (scope push +
// declaration binding) and push its children under the returned scope `here`, in
// SOURCE order. OUTPUT-IDENTICAL — pre-order scope/bind, left-to-right children.
void pass1(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
           NodeId rootNode, ScopeId rootCurrent) {
    std::vector<std::pair<NodeId, ScopeId>> stack;
    stack.push_back({rootNode, rootCurrent});
    while (!stack.empty()) {
        // Copy the frame out BEFORE any push_back (which can realloc the stack).
        auto const [node, current] = stack.back();
        stack.pop_back();
        if (!node.valid() || isEmptySpace(tree.flags(node))) continue;
        ScopeId const here = pass1Node(s, cfg, tree, node, current);
        // Push children so they POP in source (left-to-right) order.
        std::span<NodeId const> const kids = tree.children(node);
        for (std::size_t i = kids.size(); i-- > 0;) {
            stack.push_back({kids[i], here});
        }
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
        if (declIt != s.idx().declByRule.end()
            && isDefinitionAtNode(cfg.declarations[declIt->second], tree, node)) {
            // c25 D-CSUBSET-UNIFIED-COMPOSITE-SPECIFIER: a dual-mode specifier with
            // its body ABSENT is a tag reference, not a definition — it minted no
            // symbol and opened no scope in Pass 1, so it has no declaration type
            // to resolve here. Its type-position resolution happens via the paired
            // tag-reference path (resolveTypeNode's isTagReference arm).
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
                    // c10 D-CSUBSET-STRUCT-MEMBER-DECLARATOR (anonymous fields):
                    // tracks whether any NAMED declarator resolved — mirrors the
                    // Pass-1 binding loop. A field row that resolved NONE handles
                    // its anonymous symbol after the loop (bit-field width +
                    // declares-nothing diagnostic).
                    bool resolvedNamed = false;
                    // c21 (D-CSUBSET-VOLATILE-QUALIFIER) fix #1a: head-position
                    // volatile token, hoisted out of the per-declarator loop (a
                    // head scan is declarator-invariant). With a pointer star in a
                    // given declarator it forms a pointer-to-volatile-POINTEE — see
                    // the reject inside the loop below.
                    bool const headIsVolatile =
                        decl.volatileMarker.has_value()
                        && decl.headChild.has_value()
                        && *decl.headChild < kids.size()
                        && subtreeContainsToken(
                               tree, kids[*decl.headChild], *decl.volatileMarker,
                               &s.idx().declByRule);
                    // c21 (D-CSUBSET-VOLATILE-QUALIFIER) fix #1a, typedef arm: a
                    // TYPEDEF whose aliased type carries `volatile` (head-position)
                    // cannot be expressed by model B AT ALL — model B records
                    // volatility per-SYMBOL (`isVolatile` on the declared object/
                    // member), but a typedef carries volatility through the TYPE,
                    // which users of the alias inherit; threading that needs
                    // type-level cv-qualification (model A). Both `typedef volatile
                    // int vint;` (the alias would silently drop volatility on every
                    // `vint x;`) AND `typedef volatile int *vip;` (a laundered
                    // pointer-to-volatile-pointee) are rejected loud here, so no
                    // volatile typedef can silently mislead. Positioned at the row.
                    if (headIsVolatile && decl.kind == DeclarationKind::Type) {
                        ParseDiagnostic d;
                        d.code     = DiagnosticCode::S_VolatilePointeeNotSupported;
                        d.severity = DiagnosticSeverity::Error;
                        d.buffer   = tree.source().id();
                        d.span     = tree.span(node);
                        d.actual   = std::string{tree.text(node)};
                        s.reporter.report(std::move(d));
                    }
                    for (NodeId dNode : declarators) {
                        // c21 (D-CSUBSET-VOLATILE-QUALIFIER) fix #1a: reject a
                        // pointer-to-volatile-POINTEE in DECLARATION forms (local /
                        // global / param / struct-member / typedef) — a HEAD volatile
                        // together with a pointer star in THIS declarator forms
                        // `volatile <base> *p`, which model B cannot express (the
                        // volatility would ride the deref, needing type-level
                        // cv-tracking — model A / c22). Head-scoped: the head holds
                        // NO stars (they live in the declarator's pointerLayer), so
                        // this does NOT fire on east `int * volatile p` (that
                        // volatile is a `ptrQualifier` INSIDE `dNode`, after the
                        // star — the POINTER OBJECT is volatile, which IS
                        // expressible and is threaded at the ref site). Config-
                        // driven: keys off `decl.volatileMarker` + the declarator
                        // role's `pointerToken`, never a hardcoded keyword/`*`.
                        // Per-declarator so `volatile int x, *p;` rejects only `*p`.
                        // Positioned at the offending declarator. (A TYPEDEF is
                        // already rejected wholesale by the typedef arm above — skip
                        // here to avoid a double diagnostic.)
                        if (headIsVolatile && decl.kind != DeclarationKind::Type
                            && subtreeContainsToken(
                                   tree, dNode, cfg.declarators->pointerToken,
                                   &s.idx().declByRule)) {
                            ParseDiagnostic d;
                            d.code     = DiagnosticCode::S_VolatilePointeeNotSupported;
                            d.severity = DiagnosticSeverity::Error;
                            d.buffer   = tree.source().id();
                            d.span     = tree.span(dNode);
                            d.actual   = std::string{tree.text(dNode)};
                            s.reporter.report(std::move(d));
                        }
                        TypeId const declTy = declaratorDeclaredType(
                            s, cfg, tree, dNode, headTy, here,
                            /*emitOnMiss=*/true, decl.allowFlexibleArray);
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
                        resolvedNamed = true;
                        if (declTy.valid()) {
                            s.symbols.at(sym).type = declTy;
                            s.nodeToType.set(nameNode, declTy);
                        }
                        // c23 D-CSUBSET-STRUCT-MULTI-DECLARATOR: a NAMED
                        // struct/union bit-field (`int x : 3;` / per-slot
                        // `int a:3, b:5;`) in declarator mode. Gated on
                        // `decl.bitfieldSuffix` so it fires ONLY for a field
                        // rule (params/locals/globals carry no bitfieldSuffix).
                        // SEARCH ROOT = `dNode` (the per-slot
                        // `structMemberDeclarator`), NOT `node` (the whole
                        // structField): c23 moved the `bitfieldDeclSuffix`
                        // INSIDE each member-list slot, so each slot's width
                        // resolves independently — a DFS from `node` would find
                        // the FIRST slot's suffix for every declarator
                        // (`int a:3, b:5;` → both 3). The slot's own bitfield
                        // suffix is a sibling of its inner declarator, so the
                        // bounded descendant search from `dNode` reaches it.
                        if (decl.bitfieldSuffix.has_value()) {
                            BitfieldResolution const bf = resolveBitfieldSuffix(
                                s, tree, decl, dNode, declTy, nameNode.valid(),
                                here, &cfg);
                            if (bf.width.has_value())
                                s.symbols.at(sym).bitFieldWidth = bf.width;
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
                            bool const fnSuffixOnName = hasFnSuffixOnName(
                                tree, nameNode, *cfg.declarators);
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
                            // function PROTOTYPE (`int f(int);`). D-CSUBSET-FN-
                            // PROTOTYPE: a prototype IS a function declaration —
                            // it is callable (forward / mutual recursion) and a
                            // later definition MERGES with it (Pass 1 recorded
                            // the merge). UPGRADE its kind to Function so Pass 2
                            // resolves a call through it, and emit NOTHING. A
                            // function-TYPED Variable that is NOT a prototype
                            // (`isProtoDeclaration` false — e.g. a malformed
                            // function-pointer form whose suffix landed on the
                            // name) still fails loud: a silent FnSig-typed data
                            // global would miscompile.
                            if (s.symbols.at(sym).isProtoDeclaration) {
                                s.symbols.at(sym).kind =
                                    DeclarationKind::Function;
                            } else {
                                emitInvalidFn(nameNode,
                                              "function prototype declarations "
                                              "are not supported here; use "
                                              "'extern' for cross-unit "
                                              "declarations");
                            }
                        }
                    }
                    // c10 D-CSUBSET-STRUCT-MEMBER-DECLARATOR (anonymous fields):
                    // an ANONYMOUS field bound no named symbol — resolve its
                    // anonymous symbol (Pass 1 minted it on `node`). Mirrors the
                    // legacy positional anonymous-field Pass 1.5 (`fieldHasName=
                    // false`): stamp the head as its type, resolve a `: W`
                    // bit-field width, and — since an anonymous member is legal
                    // ONLY as a bit-field (C 6.7.2.1) — fail loud with
                    // S_DeclarationDeclaresNothing on `node` for an anonymous
                    // NON-bit-field (`int ;` / `int *;`). Gated on
                    // `bitfieldSuffix` so it fires only for a field form.
                    if (!resolvedNamed && decl.anonymousNameAllowed
                        && decl.bitfieldSuffix.has_value()) {
                        SymbolId const sym = s.symbolAtOr(node);
                        if (sym.valid() && headTy.valid()) {
                            s.symbols.at(sym).type = headTy;
                            s.nodeToType.set(node, headTy);
                        }
                        BitfieldResolution const bf = resolveBitfieldSuffix(
                            s, tree, decl, node, headTy, /*hasName=*/false, here,
                            &cfg);
                        if (sym.valid() && bf.width.has_value())
                            s.symbols.at(sym).bitFieldWidth = bf.width;
                        if (!bf.present) {
                            ParseDiagnostic d;
                            d.code     = DiagnosticCode::S_DeclarationDeclaresNothing;
                            d.severity = DiagnosticSeverity::Error;
                            d.buffer   = tree.source().id();
                            d.span     = tree.span(node);
                            d.actual   = std::string{tree.text(node)};
                            s.reporter.report(std::move(d));
                        }
                    }
                }
            } else if (decl.nameChild.has_value() && *decl.nameChild < kids.size()) {
                auto resolved = extractNameNode(
                    tree, kids[*decl.nameChild], decl.nameMatch, cfg.identifierToken,
                    cfg.bracketIdentifierToken);
                // FC4 c1 (anonymousNameAllowed): mirror Pass 1's anonymous
                // re-anchor — the symbol lives on the DECL node when the
                // name child resolved to a non-identifier leaf. FC8 bitfields:
                // capture whether the field has a real name (`fieldHasName`) for
                // the bit-field width/anonymity validation below.
                bool fieldHasName = true;
                if (decl.anonymousNameAllowed) {
                    std::string leafText;
                    bool const isIdentLeaf = resolved.node.valid()
                        && nameLeafText(tree, resolved.node,
                                        cfg.identifierToken,
                                        cfg.bracketIdentifierToken, leafText);
                    if (!isIdentLeaf) resolved.node = node;
                    fieldHasName = isIdentLeaf;
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
                                SymbolId sym;
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
                                                  frec.declRuleNode, fieldSymId});
                            }
                            if (!anyInvalid) {
                                std::sort(fields.begin(), fields.end(),
                                          [](FieldEntry const& a, FieldEntry const& b) {
                                              return a.index < b.index;
                                          });
                                // Densify each field's ordinal to its position in
                                // the interned composite (0..n-1). Pass 1 stamps
                                // `fieldIndex` as the *total* binding count of the
                                // scope at bind time — but a nested type-tag (an
                                // inline `struct Inner {…}` defined in field
                                // position, or any non-field binding interleaved
                                // among the fields) also consumes a binding slot,
                                // leaving the field ordinals SPARSE (e.g. in=0,
                                // z=2 with `Inner` bound between them). The struct
                                // type packs fields DENSELY (sorted order →
                                // positions 0,1), and three consumers read
                                // `fieldIndex` as an absolute index into that
                                // packed type: the HIR MemberAccess payload, the
                                // union-variant index, and the designated-init
                                // path. Writing the dense position back here — the
                                // single chokepoint where the composite's field
                                // layout is canonically established — keeps
                                // `symbol.fieldIndex == type field position` BY
                                // CONSTRUCTION (idempotent for a gap-free struct;
                                // a no-op for enum enumerators, already dense).
                                for (std::size_t i = 0; i < fields.size(); ++i)
                                    s.symbols.at(fields[i].sym).fieldIndex =
                                        static_cast<std::uint32_t>(i);
                                std::vector<TypeId> fieldTypes;
                                fieldTypes.reserve(fields.size());
                                for (auto const& fe : fields)
                                    fieldTypes.push_back(fe.type);
                                // FC8 D-CSUBSET-BITFIELD: gather each field's
                                // declared bit-width (set at the field's Pass 1.5
                                // resolution) into the parallel array passed to
                                // structType — `kNotBitfield` for an ordinary
                                // field. All-ordinary ⇒ structType emits empty
                                // scalars (the struct interns bit-identically to a
                                // pre-bitfield struct). The interned type is then
                                // the authoritative bit-width source.
                                std::vector<std::int64_t> fieldBitWidths;
                                fieldBitWidths.reserve(fields.size());
                                for (auto const& fe : fields) {
                                    auto const& w =
                                        s.symbols.at(fe.sym).bitFieldWidth;
                                    fieldBitWidths.push_back(
                                        w.has_value()
                                            ? static_cast<std::int64_t>(*w)
                                            : kNotBitfield);
                                }
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
                                // D-CSUBSET-SELF-REFERENTIAL-STRUCT: set when a DIRECT
                                // member is an incomplete composite (self-by-value /
                                // forward-only-by-value). The composite is then NOT
                                // completed (left incomplete), which keeps
                                // `computeLayout` returning nullopt — avoiding the
                                // infinite recursion a complete self-by-value type
                                // would cause (layout(N)→layout(field N)→…).
                                bool anyIncompleteMember = false;
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
                                        if (in.isIncompleteComposite(ft)) {
                                            // D-CSUBSET-SELF-REFERENTIAL-STRUCT: a
                                            // DIRECT (non-pointer) member whose type
                                            // is an INCOMPLETE composite — a struct
                                            // that contains ITSELF by value
                                            // (`struct N { struct N n; }`; its own
                                            // type is still the incomplete forward
                                            // type here) or a member of a forward-
                                            // declared-but-undefined `struct B b;`.
                                            // Its size is unknowable → fail loud. A
                                            // POINTER member (`struct N *next;`) is a
                                            // Ptr (never an incomplete composite) and
                                            // is correctly NOT flagged.
                                            famDiag(DiagnosticCode::S_IncompleteTypeMember);
                                            anyIncompleteMember = true;
                                        } else if (in.isIncompleteArray(ft)) {
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
                                    // D-CSUBSET-SELF-REFERENTIAL-STRUCT: COMPLETE the
                                    // nominal TypeId Pass 1 forward-minted (attach
                                    // the variant fields into the immutable side-
                                    // table) — do NOT mint a fresh one, or a self-ref
                                    // variant's `Ptr<U>` would point at a different
                                    // TypeId than the completed union.
                                    compositeTy = srec.type;
                                    // Leave INCOMPLETE if a direct member is itself an
                                    // incomplete composite (the error was emitted
                                    // above) — completing would let `computeLayout`
                                    // recurse infinitely on the self-by-value cycle.
                                    if (!anyIncompleteMember)
                                        s.lattice.interner().completeComposite(
                                            compositeTy, fieldTypes, fieldBitWidths);
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
                                        // Republish enumerators into the SAME
                                        // namespace scope the enum TAG floats to
                                        // (C11 6.2.1) — past any declarator-
                                        // dominator (topLevelDecl) so a file-
                                        // scope `enum E { A } … A` resolves `A`.
                                        auto const enclosingId =
                                            floatToNamespaceScope(
                                                s, cfg, tree,
                                                s.scopes.scopes()[srec.structScope.v].parent);
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
                                            erec.isEnumerator = true;
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
                                    // D-CSUBSET-SELF-REFERENTIAL-STRUCT: COMPLETE the
                                    // Pass-1 forward-minted nominal TypeId (see the
                                    // Union arm) rather than minting a fresh one.
                                    // Left incomplete on a self-by-value member (see
                                    // the Union arm's recursion note).
                                    compositeTy = srec.type;
                                    if (!anyIncompleteMember)
                                        s.lattice.interner().completeComposite(
                                            compositeTy, fieldTypes, fieldBitWidths);
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
                        // FC8 D-CSUBSET-BITFIELD: resolve a `: W` bit-field width
                        // (the field TYPE is unchanged — the width rides on the
                        // record, then the struct type's scalars at composition).
                        BitfieldResolution const bf = resolveBitfieldSuffix(
                            s, tree, decl, node, declTy, fieldHasName, here, &cfg);
                        if (bf.width.has_value())
                            s.symbols.at(sym).bitFieldWidth = bf.width;
                        // An anonymous field is legal ONLY as a bit-field
                        // (C 6.7.2.1) — `int;` / anonymous `int[3];` declare
                        // nothing. Gated on `bitfieldSuffix` so it only fires for
                        // a field form (an anonymous struct/union tag is a
                        // separate, unbuilt feature, never reached here).
                        if (!fieldHasName && !bf.present
                            && decl.bitfieldSuffix.has_value()) {
                            ParseDiagnostic d;
                            d.code     = DiagnosticCode::S_DeclarationDeclaresNothing;
                            d.severity = DiagnosticSeverity::Error;
                            d.buffer   = tree.source().id();
                            d.span     = tree.span(node);
                            d.actual   = std::string{tree.text(node)};
                            s.reporter.report(std::move(d));
                        }
                    }
                }
            }
        }
    }
}

// C 5.1.1.2 phase 6 (D-CSUBSET-ADJACENT-STRING-CONCAT): is `node`'s parent the
// stringLiteralExpr rule node? A body token under that rule is typed as part of
// the WHOLE concatenated literal on the rule node (see pass2Post's Internal
// branch), so its per-token typing is skipped. False when the grammar has no
// stringLiteralExpr rule — the per-token fallback then fires (toy/tsql).
[[nodiscard]] bool parentIsStringLiteralExprRule(EngineState const& s, Tree const& tree,
                                                 NodeId node) {
    if (!s.idx().stringLiteralExprRule.valid()) return false;
    NodeId const p = tree.parent(node);
    if (!p.valid() || tree.kind(p) != NodeKind::Internal) return false;
    return tree.rule(p).v == s.idx().stringLiteralExprRule.v;
}

// ── Pass 2: post-order — resolve uses + literal/init typing + checks ───────
// `loopDepth` (GAP C) is the count of enclosing `loopRules` subtrees the
// walk is currently inside; a `loopControls` node at depth 0 is outside
// any loop and emits S_ControlOutsideLoop.
// pass2 POST-CHILD work for ONE node — extracted for the iterative driver below
// (D-PARSE-DEEP-NEST-RECURSION-MEMORY plan 24 Stage 2). Runs AFTER all
// descendants are walked; `here` is this node's OWN resolved scope and
// `loopDepth` its OWN enclosing-loop depth (both threaded by the driver). A
// `return;` here ends THIS node's post-work and the driver loop continues —
// exactly as the recursive `return;` did. The body is byte-identical to the
// prior post-child portion of `pass2`.
void pass2Post(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
               NodeId node, ScopeId here, int loopDepth) {
    auto const k = tree.kind(node);

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
        // C 6.4.5: a string literal has type `Array<elementCore, N+1>` (N = decoded
        // body length, +1 for the NUL). When the grammar has a stringLiteralExpr
        // rule (C 5.1.1.2 phase 6, D-CSUBSET-ADJACENT-STRING-CONCAT), the WHOLE
        // (possibly adjacent-concatenated) literal is typed on that RULE NODE in
        // the Internal branch below — so a body TOKEN whose parent is that rule is
        // SKIPPED here (typing it per-token would mis-size `"a" "b"` as two
        // separate Array<Char,2>s and the HIR/semantic tiers would disagree on N).
        // For a grammar with NO stringLiteralExpr rule (toy/tsql, or a body token
        // not under that rule) the per-token fallback still fires. A malformed
        // escape leaves the node InvalidType → a `sizeof` of it fails loud.
        else if (s.idx().stringLiteralBodyToken.valid()
                 && tk == s.idx().stringLiteralBodyToken
                 && !parentIsStringLiteralExprRule(s, tree, node)) {
            if (auto decoded = decodeStringLiteralBody(tree.text(node))) {
                TypeId const elem = s.lattice.interner()
                                        .primitive(s.idx().stringLiteralElementCore);
                TypeId const arr  = s.lattice.interner().array(
                    elem, static_cast<std::int64_t>(decoded->size() + 1));
                s.nodeToType.set(node, arr);
            }
        }
    }

    // C 5.1.1.2 phase 6 (D-CSUBSET-ADJACENT-STRING-CONCAT): type the WHOLE
    // (possibly adjacent-concatenated) string-literal expression on its RULE
    // NODE. Every body child is decoded INDEPENDENTLY then byte-joined via the
    // SHARED `decodeAdjacentStringBodies` chokepoint — the exact path the HIR
    // tier uses, so both produce the IDENTICAL `Array<core, N+1>` (N = total
    // decoded bytes). `subtreeType` short-circuits on `typeAt(node)`, so this
    // rule-node type is what consumers (e.g. `sizeof`, array-dim fold) observe;
    // descent into the body tokens stops. A malformed escape leaves the node
    // untyped (InvalidType) → a `sizeof` of it fails loud, never a guessed size.
    if (k == NodeKind::Internal
        && s.idx().stringLiteralExprRule.valid()
        && s.idx().stringLiteralBodyToken.valid()
        && tree.rule(node).v == s.idx().stringLiteralExprRule.v) {
        if (auto decoded = decodeAdjacentStringBodies(
                tree, node, s.idx().stringLiteralBodyToken)) {
            TypeId const elem = s.lattice.interner()
                                    .primitive(s.idx().stringLiteralElementCore);
            TypeId const arr  = s.lattice.interner().array(
                elem, static_cast<std::int64_t>(decoded->size() + 1));
            s.nodeToType.set(node, arr);
        }
    }

    // Reference resolution. Skip if the node is a declaration's own name
    // slot (declaration and reference shapes can structurally overlap).
    if (k == NodeKind::Internal) {
        auto const rule = tree.rule(node);
        auto refIt = s.idx().refByRule.find(rule.v);
        // c25 D-CSUBSET-UNIFIED-COMPOSITE-SPECIFIER: the unified specifier rule
        // (`structSpec`) is BOTH a declaration row and a reference row. When its
        // body is PRESENT it is a DEFINITION (already minted + tag-bound in Pass
        // 1) — NOT a use of its own tag, so the reference walker must skip it
        // (else it would resolve the tag to the symbol it just defined and record
        // a phantom self-use, suppressing the unused check / inflating use-counts).
        // The body-ABSENT form IS a genuine tag reference and resolves normally.
        bool refIsDefinitionHere = false;
        if (refIt != s.idx().refByRule.end()) {
            if (auto dIt = s.idx().declByRule.find(rule.v);
                dIt != s.idx().declByRule.end()) {
                auto const& drow = cfg.declarations[dIt->second];
                refIsDefinitionHere =
                    drow.definesWhenChildRule.has_value()
                    && isDefinitionAtNode(drow, tree, node);
            }
        }
        if (refIt != s.idx().refByRule.end() && !refIsDefinitionHere) {
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
                    // C 6.2.3 tag namespace (MF-3): a tag reference rule
                    // (`struct Foo`) resolves against the Tag namespace; every
                    // other reference rule resolves Ordinary. This is the
                    // Pass-2 USE-resolution counterpart of the type-position
                    // tag arm (MF-1) — it covers a tag reference reached
                    // through Pass-2 reference resolution rather than
                    // type-position resolution.
                    SymbolNamespace const lookupNs =
                        ref.isTagReference ? SymbolNamespace::Tag
                                           : SymbolNamespace::Ordinary;
                    SymbolId const found =
                        s.scopes.lookup(here, resolved.name, lookupNs);
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
                    // Cast node at all; see combineCast in cst_to_hir).
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

        // FC12a-core (D-FC12A-VARIADIC-CALLEE) + FC12b (D-FC12B-WIN64-VARIADIC-CALLEE):
        // variadic-intrinsic typing. va_arg(ap,T) resolves+stamps its castTypeRef type
        // child (so the HIR lowering recovers T — a VALUE in the type position fails
        // loud at resolveTypeNode's S_UnknownType) + stamps the node T; va_start/va_end
        // stamp the node `void`. ALL THREE check the `ap` operand is a va_list. The
        // SHAPE of a va_list is STRATEGY-dependent (matching the injected type):
        //   * SysVRegisterSave (or nullopt): Array<__va_list_tag> or Ptr<__va_list_tag>
        //     (array-decay makes both shapes legitimate).
        //   * HomogeneousPointer (Win64 + Apple arm64): `char*` = Ptr<I8> (the injected
        //     `va_list`).
        //   * Aapcs64DualCursor (AAPCS64 ARM64-ELF): the `__va_list` STRUCT directly
        //     (NOT array-decayed, NOT a pointer) — the ap operand is the struct lvalue.
        // A wrong `ap` (e.g. an int) is S_TypeMismatch. The check shares ONE helper.
        {
            auto& in = s.lattice.interner();
            bool const homogeneousVaList =
                s.vaListStrategy.has_value()
                && *s.vaListStrategy == VaListStrategy::HomogeneousPointer;
            bool const aapcs64VaList =
                s.vaListStrategy.has_value()
                && *s.vaListStrategy == VaListStrategy::Aapcs64DualCursor;
            // True iff `ty` is a va_list for the active strategy.
            auto isVaList = [&](TypeId ty) -> bool {
                if (!ty.valid()) return false;
                TypeKind const k = in.kind(ty);
                if (homogeneousVaList) {
                    // Win64 / Apple arm64: `va_list` is `char*` (Ptr<I8>). Accept any
                    // Ptr-to-char (the injected type) — its element width is what the
                    // linear va_arg walk reads, so a Ptr<I8> is the precise shape.
                    if (k != TypeKind::Ptr) return false;
                    auto const ops = in.operands(ty);
                    if (ops.empty()) return false;
                    TypeId const inner = ops[0];
                    return inner.valid() && in.kind(inner) == TypeKind::I8;
                }
                if (aapcs64VaList) {
                    // AAPCS64: `va_list` IS the `__va_list` struct (no decay). The ap
                    // operand is the struct lvalue; its address is the cursor base.
                    return k == TypeKind::Struct && in.name(ty) == "__va_list";
                }
                // SysV family: Array<__va_list_tag> or Ptr<__va_list_tag>.
                if (k != TypeKind::Array && k != TypeKind::Ptr) return false;
                auto const ops = in.operands(ty);
                if (ops.empty()) return false;
                TypeId const inner = ops[0];
                return inner.valid() && in.kind(inner) == TypeKind::Struct
                    && in.name(inner) == "__va_list_tag";
            };
            // Emit S_TypeMismatch at `apNode` when its type is not a va_list.
            auto checkAp = [&](NodeId apNode) {
                TypeId const apTy = subtreeType(s, tree, apNode);
                if (isVaList(apTy)) return;
                ParseDiagnostic d;
                d.code     = DiagnosticCode::S_TypeMismatch;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = tree.source().id();
                d.span     = tree.span(apNode);
                d.actual   = std::string{tree.text(apNode)};
                s.reporter.report(std::move(d));
            };
            if (cfg.vaArgRule.valid() && rule.v == cfg.vaArgRule.v) {
                auto kids = visibleChildren(tree, node);
                TypeId argTy = InvalidType;
                if (cfg.vaArgTypeChild < kids.size()) {
                    NodeId const typeNode = kids[cfg.vaArgTypeChild];
                    argTy = resolveTypeNode(s, cfg, tree, typeNode, here);
                    if (argTy.valid()) s.nodeToType.set(typeNode, argTy);
                    // argTy invalid ⇒ resolveTypeNode emitted S_UnknownType (the
                    // `va_arg(ap, x)` for a VALUE x fail-loud).
                }
                if (cfg.vaArgApChild < kids.size()) checkAp(kids[cfg.vaArgApChild]);
                // The node's RESULT type is the read type T (invalid cascades-suppress).
                if (argTy.valid()) s.nodeToType.set(node, argTy);
            }
            if (cfg.vaStartRule.valid() && rule.v == cfg.vaStartRule.v) {
                auto kids = visibleChildren(tree, node);
                if (cfg.vaStartApChild < kids.size()) checkAp(kids[cfg.vaStartApChild]);
                s.nodeToType.set(node, in.primitive(TypeKind::Void));
            }
            if (cfg.vaEndRule.valid() && rule.v == cfg.vaEndRule.v) {
                auto kids = visibleChildren(tree, node);
                if (cfg.vaEndApChild < kids.size()) checkAp(kids[cfg.vaEndApChild]);
                s.nodeToType.set(node, in.primitive(TypeKind::Void));
            }
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

        // D5.1: member-access resolution (`obj.field` / `ptr->field`). The shared
        // `resolveMemberAccess` (R1) performs the resolution — LHS type → one-Ptr
        // unwrap for the arrow form → struct scope via `compositeScopeByType` →
        // field lookup — and this arm reacts to the OUTCOME: emit the matching
        // diagnostic, and on success bind the field SymbolId + propagate the field
        // type onto both the name leaf and the member-access node (so a chained
        // `f.x.y` resolves the next layer in the same post-order step). No conflict
        // with the reference-resolution block above: that resolves identifier USES
        // against the LEXICAL scope chain; this resolves field-names against a
        // struct's STRUCT scope — orthogonal. subtreeType (Pass 1.5) calls the SAME
        // helper for the field TYPE only (e.g. `sizeof(s.y)` in an array dim).
        {
            auto const mr = resolveMemberAccess(s, cfg, tree, node, here);
            using St = MemberResolution::Status;
            switch (mr.status) {
                case St::NotMemberAccess:  // non-member postfix verb — not our node
                case St::LhsUntyped:       // chained error — stay quiet
                    break;
                case St::NotAPointer: {
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::S_NotAPointer;
                    d.severity = DiagnosticSeverity::Error;
                    d.buffer   = tree.source().id();
                    d.span     = tree.span(mr.nameChildNode);
                    d.actual   = "arrow operator '->' requires a pointer operand";
                    s.reporter.report(std::move(d));
                    break;
                }
                case St::NotAComposite: {
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::S_NotAComposite;
                    d.severity = DiagnosticSeverity::Error;
                    d.buffer   = tree.source().id();
                    d.span     = tree.span(mr.nameChildNode);
                    // Disambiguate the two failure shapes in the message text: the
                    // LHS itself was non-composite (`.x`) vs the LHS was Ptr<T> but
                    // T is non-composite (`->x`).
                    d.actual   = mr.dereferences
                        ? "arrow operator '->' pointee is not a composite type"
                        : "member access '.' requires a composite-typed operand";
                    s.reporter.report(std::move(d));
                    break;
                }
                case St::BadNameNode: {
                    // A non-identifier nameChild is a schema-shape bug (the
                    // nameChild is positionally fixed by the rule) — emit
                    // C_InvalidSemantics, not a phantom S_UndeclaredIdentifier.
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::C_InvalidSemantics;
                    d.severity = DiagnosticSeverity::Error;
                    d.buffer   = tree.source().id();
                    d.span     = tree.span(mr.nameChildNode);
                    d.actual   = "member-access nameChild did not "
                                 "resolve to an identifier leaf";
                    s.reporter.report(std::move(d));
                    break;
                }
                case St::UndeclaredField: {
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::S_UndeclaredIdentifier;
                    d.severity = DiagnosticSeverity::Error;
                    d.buffer   = tree.source().id();
                    d.span     = tree.span(mr.nameNode);
                    d.actual   = mr.fieldName;
                    s.reporter.report(std::move(d));
                    break;
                }
                case St::Ok: {
                    s.nodeToSymbol.set(mr.nameNode, mr.fieldSym);
                    s.usesBySymbol[mr.fieldSym.v].push_back(mr.nameNode);
                    if (mr.fieldType.valid()) {
                        s.nodeToType.set(mr.nameNode, mr.fieldType);
                        s.nodeToType.set(node, mr.fieldType);
                    }
                    break;
                }
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
                                                    /*boolWidensToArith=*/true,
                                                    /*charConvertsToArith=*/cfg.charConvertsToArith, /*enumConvertsToArith=*/cfg.enumConvertsToArith, /*intCrossSignednessConverts=*/cfg.intCrossSignednessConverts, /*intSameSignednessNarrows=*/cfg.intSameSignednessNarrows)
                                   && !admitsNullPointerConstant(
                                          s, tree, rec.type, initNode,
                                          tree.schema().semantics()
                                              .pointerConversions,
                                          here, cfg)) {
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
                                                    /*boolWidensToArith=*/true,
                                                    /*charConvertsToArith=*/cfg.charConvertsToArith, /*enumConvertsToArith=*/cfg.enumConvertsToArith, /*intCrossSignednessConverts=*/cfg.intCrossSignednessConverts, /*intSameSignednessNarrows=*/cfg.intSameSignednessNarrows)
                                   // D-LANG-NULL-POINTER-CONSTANT (step
                                   // 13.3): admit `T* p = 0;` initializer
                                   // per C §6.3.2.3.3.
                                   && !admitsNullPointerConstant(
                                          s, tree, rec.type, initNode,
                                          tree.schema().semantics()
                                              .pointerConversions,
                                          here, cfg)) {
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
                // D-SEMANTIC-ASSIGN-STMT-ASSIGNABILITY-BYPASS: an assignment
                // (`a = b`, statement OR nested expression) must run the SAME
                // assignability check the init / call-arg / return sites use —
                // `subtreeType(lhs)` ←? `subtreeType(rhs)` through the shared
                // `isAssignable` chokepoint, with the SAME conversion gates and
                // the SAME null-pointer-constant admission. Pre-fix the
                // assignment carried NO semantic type check (only the const
                // check above), so `int x; float f; x = f;` was silently
                // accepted while `int x = f;` (init) rejected S_TypeMismatch — a
                // strictness asymmetry (the downstream HIR `coerce()` materialized
                // a correct-but-implicit Cast). This restores parity: an invalid
                // assignment now fails loud here, positioned at the RHS.
                //
                // ONLY the PLAIN assignment is checked — a COMPOUND assignment
                // (`x += y`) is `x = x op y` whose result is the arithmetic
                // common type converted back to `x` (the usual-arithmetic path,
                // NOT assignability: `int x; double y; x += y;` is legal). The
                // operator-table entry's `target == "Assign"` (with an empty
                // `compoundBase`) is the SAME plain-vs-compound discriminator
                // `subtreeType`'s combineBinary uses — fully config-driven, no
                // operator-token identity hardcoded here.
                if (assign.operatorToken.has_value()
                    && assign.lhsChild < kids.size()
                    && assign.rhsChild < kids.size()) {
                    auto const& hirCfg = tree.schema().hirLowering();
                    bool isPlainAssign = false;
                    for (auto const& e : hirCfg.binaryOps) {
                        if (e.token.v == assign.operatorToken->v) {
                            isPlainAssign =
                                (e.target == "Assign" && e.compoundBase.empty());
                            break;
                        }
                    }
                    if (isPlainAssign) {
                        NodeId const lhsN = kids[assign.lhsChild];
                        NodeId const rhsN = kids[assign.rhsChild];
                        TypeId const lhsTy = subtreeType(s, tree, lhsN, here);
                        TypeId const rhsTy = subtreeType(s, tree, rhsN, here);
                        // Skip on either side unknown (cascade suppression — an
                        // upstream error already fired). The conversion gates +
                        // the null-pointer admission MIRROR the init-site call
                        // EXACTLY so the two positions agree by construction.
                        auto const& ptrRules =
                            tree.schema().semantics().pointerConversions;
                        if (lhsTy.valid() && rhsTy.valid()
                            && !isAssignable(s.lattice.interner(), lhsTy, rhsTy,
                                             ptrRules,
                                             /*boolWidensToArith=*/true,
                                             /*charConvertsToArith=*/cfg.charConvertsToArith,
                                             /*enumConvertsToArith=*/cfg.enumConvertsToArith,
                                             /*intCrossSignednessConverts=*/cfg.intCrossSignednessConverts,
                                             /*intSameSignednessNarrows=*/cfg.intSameSignednessNarrows)
                            && !admitsNullPointerConstant(
                                   s, tree, lhsTy, rhsN, ptrRules, here, cfg)) {
                            ParseDiagnostic d;
                            d.code     = DiagnosticCode::S_TypeMismatch;
                            d.severity = DiagnosticSeverity::Error;
                            d.buffer   = tree.source().id();
                            d.span     = tree.span(rhsN);
                            d.actual   = std::string{tree.text(rhsN)};
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

// pass2 — iterative whole-tree POST-ORDER walk (D-PARSE-DEEP-NEST-RECURSION-
// MEMORY plan 24 Stage 2): an explicit heap work-stack replaces host recursion
// so a deeply-nested tree (statements OR expressions) carries flat O(N)
// host-stack cost. Two phases per node — phase 0 resolves its scope `here` +
// `childLoopDepth` (the prior pre-child setup) and pushes its children in SOURCE
// order; phase 1 runs `pass2Post` (the prior post-child handling). OUTPUT-
// IDENTICAL to the recursion: same pre-order scope resolution, same left-to-
// right child order, same post-order node handling.
void pass2(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
           NodeId rootNode, ScopeId rootCurrent, int rootLoopDepth) {
    struct Frame {
        NodeId       node;
        ScopeId      current;
        int          loopDepth;
        ScopeId      here;       // resolved at phase 0, consumed at phase 1
        std::uint8_t phase;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{rootNode, rootCurrent, rootLoopDepth, {}, 0});
    while (!stack.empty()) {
        // Copy out the fields we need BEFORE any push_back (which can realloc
        // and dangle a reference into `stack`).
        Frame const f = stack.back();
        if (f.phase == 0) {
            if (!f.node.valid() || isEmptySpace(tree.flags(f.node))) {
                stack.pop_back();
                continue;
            }
            auto const k = tree.kind(f.node);
            ScopeId here = f.current;
            if (k == NodeKind::Internal
                && s.idx().scopeByRule.contains(tree.rule(f.node).v)) {
                here = childScopeFor(s, tree, f.node, f.current);
            }
            // GAP C: entering a loop-context subtree raises the depth for the
            // children walk (a break/continue is valid anywhere inside it).
            int const childLoopDepth =
                (k == NodeKind::Internal
                 && s.idx().loopByRule.contains(tree.rule(f.node).v))
                    ? f.loopDepth + 1
                    : f.loopDepth;
            // Record `here` + advance to the post phase BEFORE pushing children.
            stack.back().here  = here;
            stack.back().phase = 1;
            // Push children so they POP in source (left-to-right) order: a LIFO
            // stack needs reverse insertion. Each child inherits `here` as its
            // `current` and `childLoopDepth` as its `loopDepth`, exactly as the
            // recursive `pass2(child, here, childLoopDepth)` passed.
            std::span<NodeId const> const kids = tree.children(f.node);
            for (std::size_t i = kids.size(); i-- > 0;) {
                stack.push_back(Frame{kids[i], here, childLoopDepth, {}, 0});
            }
        } else {
            stack.pop_back();
            pass2Post(s, cfg, tree, f.node, f.here, f.loopDepth);
        }
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
void checkCallAgainstSig(EngineState& s, SemanticConfig const& cfg,
                         Tree const& tree, NodeId node,
                         std::vector<NodeId> const& kids,
                         CallRule const& call, TypeId fnSig,
                         bool variadicBuiltin, ScopeId scope) {
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

    // D-TYPEINTERNER-OPERAND-SPAN-LIFETIME-GUARD: COPY the param-type span into a
    // stable owned vector. The per-arg assignability loop below calls subtreeType()
    // (~line 3467), which can INTERN a fresh type mid-loop — e.g. an `&b` arg
    // materializes pointer<int> on first use — mutating the interner pool and
    // dangling a retained fnParams() view. That is a heap-use-after-free masked in
    // Release (the guard is compiled out) and only caught on Debug: the ffi_memcpy
    // multi-param shipped-FnSig case (`memcpy(&b,&a,4)`). Single-param libc fns
    // (malloc/free) don't trip it — a literal `4` / an existing pointer arg interns
    // nothing. Owning the params makes the loop robust against ANY downstream intern.
    std::vector<TypeId> const params = [&] {
        auto const sp = s.lattice.interner().fnParams(fnSig);
        return std::vector<TypeId>(sp.begin(), sp.end());
    }();

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
                          /*boolWidensToArith=*/true,
                                                    /*charConvertsToArith=*/cfg.charConvertsToArith, /*enumConvertsToArith=*/cfg.enumConvertsToArith, /*intCrossSignednessConverts=*/cfg.intCrossSignednessConverts, /*intSameSignednessNarrows=*/cfg.intSameSignednessNarrows)) {
            // D-LANG-NULL-POINTER-CONSTANT (step 13.3): admit literal-0
            // → Ptr<*> as null pointer constant per C §6.3.2.3.3. The
            // check lives here (NOT in isAssignable) because it is
            // value-aware (looks at the literal's decoded value).
            if (admitsNullPointerConstant(s, tree, params[i],
                                          argNodes[i], ptrRules, scope, cfg)) {
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
                                landedVariadicBuiltin, scope);
            return;
        }
        if (isFnPointerType(in, landedTy)) {
            checkCallAgainstSig(s, cfg, tree, node, kids, call,
                                in.operands(landedTy)[0],
                                /*variadicBuiltin=*/false, scope);
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
                                /*variadicBuiltin=*/false, scope);
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
                        s.symbols.at(calleeSym).variadicBuiltin, scope);
}

// D-LANG-NULL-POINTER-CONSTANT (step 13.3, 2026-06-02): per C §6.3.2.3.3,
// an integer constant expression with value 0 is a null pointer constant
// — convertible WITHOUT cast to any object-pointer or function-pointer
// type. This helper admits ONLY the bare integer-literal `0` form (not
// `+0` / `-0` / `0+0` / `(int)0`). This is the structural FAST PATH of
// `admitsNullPointerConstant`; the WIDER folded admission (`-0`, `1-1`,
// `(int)(2-2)` — any integer constant expression with value 0) lives in that
// caller's R2 folded path (D-SEMANTIC-NULL-CONSTANT-FOLDING ✅ CLOSED).
//
// History (resolved by R2, 2026-06-15): an earlier audit DROPPED the wider
// admission to avoid a HIR/semantic divergence — the HIR `coerce()` arm matches
// `HirKind::Literal`, and `-0` lowers to `UnaryOp(Neg, Literal 0)` / `1-1` to a
// `BinaryOp`, so semantic admission WITHOUT HIR materialization would leak an
// un-Cast `I32` into a `Ptr<*>` slot at the verifier. R2 closes exactly that: the
// folded path MARKS the admitted node (`EngineState::nullPointerConstantNodes` →
// `SemanticModel::isNullPointerConstant`), and the CST→HIR lowerer materializes a
// synthetic `Literal 0` in its place, so the existing literal-0 coerce arm emits
// the `Cast(0 → Ptr)` uniformly. So `f(-0)` / `f(1-1)` now ADMIT (and lower to
// NULL) at every conversion site — no divergence, no leaked I32.
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
admitsNullPointerConstant(EngineState& s, Tree const& tree,
                          TypeId lhsTy, NodeId rhsExpr,
                          SemanticConfig::PointerConversionRules const& rules,
                          ScopeId scope, SemanticConfig const& cfg) {
    if (!rules.nullPointerConstantFromIntegerZero) return false;
    if (!lhsTy.valid()) return false;
    if (s.lattice.interner().kind(lhsTy) != TypeKind::Ptr) return false;
    // Fast path: a STRUCTURAL integer literal 0 (`0`). The HIR coerce arm admits a
    // real Literal directly, so this path needs NO marker — byte-identical to the
    // pre-R2 behavior.
    if (isLiteralIntegerZero(s, tree, rhsExpr)) return true;
    // R2 (D-SEMANTIC-NULL-CONSTANT-FOLDING) folded path — C §6.3.2.3p3: ANY integer
    // constant expression with value 0 (`1-1`, `-0`, `(int)(2-2)`) is a null pointer
    // constant. The operand must be (a) INTEGER-typed — excludes a float `1.5-1.5`;
    // a cast `(int)x` makes it integer-typed — and (b) const-fold to 0. On admit,
    // MARK the node: the HIR lowerer materializes a synthetic Literal 0 in its place
    // (the operator tree would otherwise lower to a BinaryOp the literal-only coerce
    // arm cannot admit → a silent tier divergence). The integer-kind gate mirrors
    // `isLiteralIntegerZero` (signed/unsigned int ranks only, not Char/Bool) so the
    // two admit forms stay consistent.
    TypeId const ot = subtreeType(s, tree, rhsExpr, scope);
    if (!ot.valid()) return false;
    {
        using namespace detail::type_rules;
        TypeKind const ok = s.lattice.interner().kind(ot);
        if (signedIntRank(ok) == 0 && unsignedIntRank(ok) == 0) return false;
    }
    auto const folded = constIntExpr(s, tree, rhsExpr, scope, &cfg);
    if (folded.has_value() && *folded == 0) {
        s.nullPointerConstantNodes.set(rhsExpr, true);
        return true;
    }
    return false;
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
subtreeType(EngineState const& s, Tree const& tree, NodeId rootNode, ScopeId scope) {
    // D-PARSE-DEEP-NEST-RECURSION-MEMORY (plan 24 Stage 1): this typer is an
    // EXPLICIT HEAP WORK-STACK driver — NOT host recursion — so a deeply-nested
    // expression carries flat O(N) host-stack cost. The five recursive arms
    // (binary lhs/rhs, unary operand, postfix base, ternary then/else, the
    // transparent-wrapper children) become phase-machine frames; every terminal
    // arm and combine rule below is byte-identical to the prior recursive form
    // (OUTPUT-IDENTITY is THE gate — the full ctest suite is the oracle). `scope`
    // is INVARIANT across the whole traversal (every prior recursive call passed
    // the same `scope`), so it is captured once, not carried per frame. The
    // member-access chain (`a.b.c`) still recurses through `resolveMemberAccess`
    // → `subtreeType` (a separate, shallow-in-practice residual).

    // Stamped type wins — short-circuit on the HOT PATH (Pass-2 calls on an
    // already-stamped node) BEFORE building the per-call closures below (notably
    // `resolveArithmeticRules`). The driver's `enter` repeats this check for
    // every child, so this is purely the root's fast exit (output-identical).
    if (!rootNode.valid()) return InvalidType;
    if (TypeId t = s.typeAt(rootNode); t.valid()) return t;

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
    auto const& hirCfg = tree.schema().hirLowering();

    // ── leaf typing: an identifier resolves to its symbol's type by scope ──
    // A literal token is Pass-2-stamped (handled by the `typeAt` short-circuit in
    // `enter`); a Pass-1.5 `sizeof(literal)` is rare and InvalidType there is an
    // acceptable fail-loud. An IDENTIFIER leaf MUST resolve here — the Pass-1.5
    // `sizeof b` / `sizeof(b[0])` case the sizeof closure feeds.
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
    // The visible operator token + its `hirLowering` op-table entry — the entry's
    // `target` (HIR op NAME / special tag) the law dispatches on, matching
    // cst_to_hir's `binOp_`/`unOp_`/`postOp_` token→entry maps.
    auto const opEntryFor =
        [&](NodeId node, std::vector<HirOperatorEntry> const& ops)
        -> HirOperatorEntry const* {
        for (NodeId c : visibleChildren(tree, node)) {
            if (tree.kind(c) != NodeKind::Token) continue;
            std::uint32_t const tk = tree.tokenKind(c).v;
            for (auto const& e : ops) {
                if (e.token.v == tk) return &e;
            }
        }
        return nullptr;
    };

    // ── combine helpers (each verbatim from the prior recursive arm; operate on
    //    already-typed child results, so no recursion) ──
    auto const combineBinary =
        [&](HirOperatorEntry const* e, TypeId lt, TypeId rt) -> TypeId {
        if (e->target == "LogicalAnd" || e->target == "LogicalOr") return boolType();
        if (e->target == "Comma")  return rt;                       // value of RHS
        if (e->target == "Assign" || !e->compoundBase.empty()) return lt;
        auto const op = coreOpFromNameSem(e->target);
        if (op.has_value()) {
            if (isComparison(*op)) return boolType();
            // Shift result type follows the config verb `shiftResult` via the
            // shared `shiftResultType` chokepoint (D-UAC-SHIFT-RESULT-RULE-CONFIG)
            // — the SAME function cst_to_hir's combineBinary uses.
            if ((*op == HirOpKind::Shl || *op == HirOpKind::Shr)
                && arith.has_value()) {
                return shiftResultType(interner, lt, rt, *arith);
            }
        }
        TypeId const common = commonArithType(lt, rt);
        if (common.valid()) return common;
        return lt.valid() ? lt : rt;
    };
    auto const combineUnary = [&](HirOperatorEntry const* e, TypeId ot) -> TypeId {
        if (e->target == "AddressOf") return ot.valid() ? interner.pointer(ot) : InvalidType;
        if (e->target == "Deref")     return derefResultType(interner, ot);
        // FC-F1 (C 6.5.3.1): prefix `++x`/`--x` yields the OPERAND type (a value
        // equal to the post-increment object), exactly as postfix `combinePostfix`
        // does. Explicit here so the type does not rely on the `coreOpFromNameSem`
        // fall-through (PreInc/PreDec are not core unary ops) — the CST→HIR tier
        // also lowers prefix ++/-- to a SeqExpr whose result type is the lvalue
        // type, so the two tiers agree.
        if (e->target == "PreInc" || e->target == "PreDec") return ot;
        // c12 (C 6.5.3.3p2): unary `+` yields the INTEGER-PROMOTED operand value.
        // Like Neg/BitNot the type is operand-preserving here (sub-int values live
        // promoted in 32-bit regs — the lazy-consumer model — so the carried type
        // is the operand's; the CST→HIR tier lowers `+x` to the operand itself).
        if (e->target == "Pos") return ot;
        auto const op = coreOpFromNameSem(e->target);
        if (op.has_value() && *op == HirOpKind::Not) return boolType();
        return ot;   // Neg / BitNot are type-preserving
    };
    auto const combinePostfix =
        [&](NodeId node, HirOperatorEntry const* e, TypeId bt) -> TypeId {
        if (e->target == "Index")   return indexResultType(interner, bt);
        if (e->target == "PostInc" || e->target == "PostDec") return bt;
        if (e->target == "Call") {
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
        // Member access (`.`/`->`): the shared helper resolves the field type (it
        // re-derives the object type via subtreeType — the member-chain residual).
        auto const mr = resolveMemberAccess(s, sem, tree, node, scope);
        return mr.status == MemberResolution::Status::Ok ? mr.fieldType
                                                         : InvalidType;
    };
    auto const combineTernary = [&](TypeId thenT, TypeId elseT) -> TypeId {
        TypeId const common = commonArithType(thenT, elseT);
        if (common.valid()) return common;
        return thenT.valid() ? thenT : elseT;   // non-arith arms (pointers etc.)
    };

    // ── the explicit work-stack ──
    struct Frame {
        NodeId node;
        enum class Kind : std::uint8_t {
            Binary, Unary, Postfix, Ternary, Wrapper
        } kind;
        std::uint8_t           phase;
        NodeId                  n0;     // binary lhs / unary operand / postfix base
        NodeId                  n1;     // binary rhs
        HirOperatorEntry const* e;      // binary / unary / postfix op entry
        std::vector<NodeId>     list;   // ternary [cond,then,else] / wrapper children
        std::size_t             idx;    // wrapper child cursor
        TypeId                  c0;     // first child result (lt / thenT)
    };
    std::vector<Frame> work;
    TypeId result{};

    // Classify `node`: either set `result` to its type (TERMINAL — typeAt /
    // leaf / sizeof / label / cast / compound-literal / no-op-entry), or push a
    // Frame for one of the five recursive arms. Mirrors the prior dispatch order
    // EXACTLY so the result is byte-identical.
    auto const enter = [&](NodeId node) {
        if (!node.valid()) { result = InvalidType; return; }
        // Stamped type wins — Pass 2 already computed refs/member/call/cast/
        // sizeof/literals/compound-literals onto the node itself.
        if (TypeId t = s.typeAt(node); t.valid()) { result = t; return; }
        if (tree.kind(node) != NodeKind::Internal) { result = leafType(node); return; }
        RuleId const r = tree.rule(node);
        // ── cast / sizeof / compound-literal: RE-TYPING wrappers (Pass-1.5; Pass 2
        //    stamps them so the typeAt above wins there). Without this the wrapper
        //    fallthrough would return the PRE-cast type — `sizeof((char)x)` would
        //    fold the wrong size. ──
        if (hirCfg.sizeofRule.valid() && r.v == hirCfg.sizeofRule.v) {
            result = interner.primitive(TypeKind::U64); return;        // size_t
        }
        // D-CSUBSET-COMPUTED-GOTO: `&&label` is `void*` (a dedicated operand rule
        // whose Identifier child is a LABEL name, never typed as an expression).
        if (hirCfg.labelAddressRule.valid() && r.v == hirCfg.labelAddressRule.v) {
            result = interner.pointer(interner.primitive(TypeKind::Void)); return;
        }
        if (auto const it = s.idx().castByRule.find(r.v);
            it != s.idx().castByRule.end()) {
            auto const& cr = sem.castRules[it->second];
            auto const kids = visibleChildren(tree, node);
            // emitOnMiss=false — the Pass-2 cast arm owns the S_UnknownType diag.
            result = (cr.typeChild >= kids.size())
                ? InvalidType
                : resolveTypeNode(const_cast<EngineState&>(s), sem, tree,
                                  kids[cr.typeChild], scope, /*emitOnMiss=*/false);
            return;
        }
        if (auto const it = s.idx().compoundLiteralByRule.find(r.v);
            it != s.idx().compoundLiteralByRule.end()) {
            auto const& cl = sem.compoundLiteralRules[it->second];
            auto const kids = visibleChildren(tree, node);
            result = (cl.typeChild >= kids.size())
                ? InvalidType
                : resolveTypeNode(const_cast<EngineState&>(s), sem, tree,
                                  kids[cl.typeChild], scope, /*emitOnMiss=*/false);
            return;
        }
        // ── binary: [lhs(Internal), OP-token, rhs(Internal)] ──
        if (r.v == hirCfg.binaryExprRule.v) {
            NodeId lhsN{}, rhsN{};
            for (NodeId c : visibleChildren(tree, node)) {
                if (tree.kind(c) == NodeKind::Token) continue;
                if (!lhsN.valid()) lhsN = c; else if (!rhsN.valid()) rhsN = c;
            }
            HirOperatorEntry const* e = opEntryFor(node, hirCfg.binaryOps);
            if (e == nullptr) { result = InvalidType; return; }
            work.push_back(Frame{node, Frame::Kind::Binary, 0, lhsN, rhsN, e,
                                 {}, 0, {}});
            return;
        }
        // ── unary: [OP-token, operand(Internal)] ──
        if (r.v == hirCfg.unaryExprRule.v) {
            NodeId operandN{};
            for (NodeId c : visibleChildren(tree, node)) {
                if (tree.kind(c) == NodeKind::Token) continue;
                operandN = c; break;
            }
            HirOperatorEntry const* e = opEntryFor(node, hirCfg.unaryOps);
            if (e == nullptr) { result = InvalidType; return; }
            work.push_back(Frame{node, Frame::Kind::Unary, 0, operandN, {}, e,
                                 {}, 0, {}});
            return;
        }
        // ── postfix: [base(Internal), OP-token, rest...] ──
        if (r.v == hirCfg.postfixExprRule.v) {
            NodeId baseN{};
            for (NodeId c : visibleChildren(tree, node)) {
                if (tree.kind(c) != NodeKind::Token) { baseN = c; break; }
            }
            HirOperatorEntry const* e = opEntryFor(node, hirCfg.postfixOps);
            if (e == nullptr) { result = InvalidType; return; }
            work.push_back(Frame{node, Frame::Kind::Postfix, 0, baseN, {}, e,
                                 {}, 0, {}});
            return;
        }
        // ── ternary: operands = the 3 non-token visible children [cond,then,else] ──
        if (hirCfg.ternaryExprRule.valid() && r.v == hirCfg.ternaryExprRule.v) {
            std::vector<NodeId> operands;
            for (NodeId c : visibleChildren(tree, node)) {
                if (tree.kind(c) != NodeKind::Token) operands.push_back(c);
            }
            if (operands.size() != 3) { result = InvalidType; return; }
            work.push_back(Frame{node, Frame::Kind::Ternary, 0, {}, {}, nullptr,
                                 std::move(operands), 0, {}});
            return;
        }
        // ── transparent / operand wrapper: pass the inner expression's type
        //    through; recurse on each visible child in order, first VALID wins
        //    (operator-AWARE via the per-child `enter`). ──
        std::vector<NodeId> kids;
        for (NodeId c : visibleChildren(tree, node)) kids.push_back(c);
        work.push_back(Frame{node, Frame::Kind::Wrapper, 0, {}, {}, nullptr,
                             std::move(kids), 0, {}});
    };

    // Driver: each frame either pushes a child (`enter`, last statement of the
    // branch — `f` may be invalidated by the push, so nothing reads `f` after it)
    // or, when its children are typed, combines + pops, leaving the result in
    // `result` for the parent's next phase to consume.
    enter(rootNode);
    while (!work.empty()) {
        Frame& f = work.back();
        switch (f.kind) {
        case Frame::Kind::Binary:
            if (f.phase == 0) { f.phase = 1; NodeId n = f.n0; enter(n); }
            else if (f.phase == 1) {
                f.c0 = result; f.phase = 2; NodeId n = f.n1; enter(n);
            } else {
                HirOperatorEntry const* e = f.e; TypeId lt = f.c0;
                work.pop_back(); result = combineBinary(e, lt, result);
            }
            break;
        case Frame::Kind::Unary:
            if (f.phase == 0) { f.phase = 1; NodeId n = f.n0; enter(n); }
            else {
                HirOperatorEntry const* e = f.e;
                work.pop_back(); result = combineUnary(e, result);
            }
            break;
        case Frame::Kind::Postfix:
            if (f.phase == 0) { f.phase = 1; NodeId n = f.n0; enter(n); }
            else {
                NodeId node = f.node; HirOperatorEntry const* e = f.e;
                work.pop_back(); result = combinePostfix(node, e, result);
            }
            break;
        case Frame::Kind::Ternary:
            if (f.phase == 0) { f.phase = 1; NodeId n = f.list[1]; enter(n); }
            else if (f.phase == 1) {
                f.c0 = result; f.phase = 2; NodeId n = f.list[2]; enter(n);
            } else {
                TypeId thenT = f.c0;
                work.pop_back(); result = combineTernary(thenT, result);
            }
            break;
        case Frame::Kind::Wrapper:
            if (f.phase == 0) {
                if (f.idx >= f.list.size()) { work.pop_back(); result = InvalidType; }
                else { f.phase = 1; NodeId n = f.list[f.idx]; enter(n); }
            } else {  // phase 1: a child just delivered `result`
                if (result.valid()) { work.pop_back(); }  // first valid wins
                else { ++f.idx; f.phase = 0; }             // try the next child
            }
            break;
        }
    }
    return result;
}

// R1: shared member-access resolver (declared after subtreeType's forward decl).
// Walks `obj.field` / `ptr->field` → the field's declared type, reporting the
// outcome. NO side effects (no diagnostics, no symbol binding) — the Pass-2 arm
// owns those and reacts to `status`; subtreeType (Pass 1.5) takes `fieldType` on
// Ok. Mirrors the resolution the Pass-2 arm performed inline before R1; the spans
// it exposes (`nameChildNode` for the pre-extraction failures, `nameNode` for the
// undeclared-field case) preserve the exact diagnostic positions the corpus pins.
[[nodiscard]] MemberResolution
resolveMemberAccess(EngineState const& s, SemanticConfig const& cfg,
                    Tree const& tree, NodeId node, ScopeId scope) {
    MemberResolution out;
    // Locate the matching MemberAccessRule for this node's rule, distinguished by
    // operator-token (mirrors the assignByRule discipline: an ungated entry is the
    // sole entry; otherwise the entry whose `.`/`->` token is present wins). No
    // match ⇒ this postfix node carries a non-member operator (`++`/`(`/`[`).
    RuleId const rule = tree.rule(node);
    auto maIt = s.idx().memberAccessByRule.find(rule.v);
    if (maIt == s.idx().memberAccessByRule.end()) return out;  // NotMemberAccess
    MemberAccessRule const* ma = nullptr;
    for (std::size_t midx : maIt->second) {
        auto const& cand = cfg.memberAccesses[midx];
        if (!cand.operatorToken.has_value()) { ma = &cand; break; }  // ungated
        bool matched = false;
        for (auto kid : tree.children(node)) {
            if (tree.kind(kid) == NodeKind::Token
                && tree.tokenKind(kid) == *cand.operatorToken) { matched = true; break; }
        }
        if (matched) { ma = &cand; break; }
    }
    if (ma == nullptr) return out;  // NotMemberAccess (operator mismatch)
    out.dereferences = ma->dereferences;

    auto kids = visibleChildren(tree, node);
    if (ma->lhsChild >= kids.size() || ma->nameChild >= kids.size()) return out;
    NodeId const lhsNode = kids[ma->lhsChild];
    out.nameChildNode    = kids[ma->nameChild];

    // F5: thread `scope` so a Pass-1.5 unstamped leaf (`s` in `sizeof(s.y)`)
    // resolves by scope-lookup. In Pass 2 the leaf is already stamped, so
    // subtreeType short-circuits on `typeAt` and `scope` is unused.
    TypeId const lhsType = subtreeType(s, tree, lhsNode, scope);
    if (!lhsType.valid()) { out.status = MemberResolution::Status::LhsUntyped; return out; }

    TypeId effectiveType = lhsType;
    if (ma->dereferences) {
        // Arrow form `p->x` = `(*p).x`: the LHS must be `Ptr<Struct>` (one level).
        if (s.lattice.interner().kind(effectiveType) == TypeKind::Ptr) {
            auto const ops = s.lattice.interner().operands(effectiveType);
            if (ops.empty()) { out.status = MemberResolution::Status::NotAPointer; return out; }
            effectiveType = ops[0];
        } else {
            out.status = MemberResolution::Status::NotAPointer; return out;
        }
    }

    auto scopeIt = s.compositeScopeByType.find(effectiveType.v);
    if (scopeIt == s.compositeScopeByType.end()) {
        out.status = MemberResolution::Status::NotAComposite; return out;
    }

    auto fnRes = extractNameNode(tree, out.nameChildNode, NameMatchMode::Self,
                                 cfg.identifierToken, cfg.bracketIdentifierToken);
    out.nameNode  = fnRes.node;
    out.fieldName = fnRes.name;
    bool isIdentifier = false;
    if (fnRes.node.valid() && tree.kind(fnRes.node) == NodeKind::Token) {
        auto const rtk = tree.tokenKind(fnRes.node);
        if (cfg.identifierToken.valid() && rtk == cfg.identifierToken) {
            isIdentifier = true;
        } else if (cfg.bracketIdentifierToken.has_value()
                   && cfg.bracketIdentifierToken->valid()
                   && rtk == *cfg.bracketIdentifierToken) {
            isIdentifier = true;
        }
    }
    if (!isIdentifier || fnRes.name.empty()) {
        out.status = MemberResolution::Status::BadNameNode; return out;
    }

    ScopeId const fieldScope = scopeIt->second;
    auto const& sc = s.scopes.scopes()[fieldScope.v];
    auto bindIt = sc.bindings.find(fnRes.name);
    if (bindIt == sc.bindings.end()) {
        out.status = MemberResolution::Status::UndeclaredField; return out;
    }
    out.fieldSym  = bindIt->second;
    out.fieldType = s.symbols.at(out.fieldSym).type;   // may be invalid ⇒ still Ok (binding found)
    out.status    = MemberResolution::Status::Ok;
    return out;
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
                      /*boolWidensToArith=*/true,
                                                    /*charConvertsToArith=*/cfg.charConvertsToArith, /*enumConvertsToArith=*/cfg.enumConvertsToArith, /*intCrossSignednessConverts=*/cfg.intCrossSignednessConverts, /*intSameSignednessNarrows=*/cfg.intSameSignednessNarrows)) {
        // D-LANG-NULL-POINTER-CONSTANT (step 13.3): admit `return 0;`
        // from a Ptr<*>-returning function per C §6.3.2.3.3.
        if (admitsNullPointerConstant(s, tree, fnResult,
                                      valueNode, ptrRules, scope, cfg)) {
            return;
        }
        emitMismatch(valueNode);
    }
}

} // namespace

// The full semantic-analysis pass, factored out of the public `analyze`
// entry point so the latter can run it on a large worker-thread stack.
// File-static: the only caller is `analyze` below. The recursion over the
// expression tree (and the many tree walks it drives) overflows the host's
// ~1 MB main stack at ~25 nesting levels (`D-PARSE-DEEP-FRONTEND-STACK`),
// so `analyze` invokes this via `callOnLargeStack` on a 64 MiB stack.
static SemanticModel analyzeImpl(std::shared_ptr<CompilationUnit const> cu,
                                 DataModel dataModel,
                                 std::optional<AggregateLayoutParams> aggregateLayout,
                                 std::optional<VaListStrategy> vaListStrategy,
                                 std::optional<ObjectFormatKind> activeFormat,
                                 std::optional<std::string_view> activeTarget);

SemanticModel analyze(std::shared_ptr<CompilationUnit const> cu,
                      DataModel dataModel,
                      std::optional<AggregateLayoutParams> aggregateLayout,
                      std::optional<VaListStrategy> vaListStrategy,
                      std::optional<ObjectFormatKind> activeFormat,
                      std::optional<std::string_view> activeTarget) {
    // Run the recursive analysis on a dedicated large-stack worker thread
    // (JOIN-synchronous — no concurrency) so a deeply-nested-but-legal
    // expression tree does not overflow the host's ~1 MB main thread stack.
    // This is what lets `ParserConfig::maxExpressionDepth` be a real
    // semantic cap (config-driven: c-subset = 1024) rather than a host-stack
    // artifact (`D-PARSE-DEEP-FRONTEND-STACK`). The null-CU + every other contract
    // check lives in `analyzeImpl`, which runs on the worker; any exception
    // it throws is re-thrown here by `callOnLargeStack`.
    return dss::substrate::callOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes, [&] {
            return analyzeImpl(std::move(cu), dataModel, std::move(aggregateLayout),
                               std::move(vaListStrategy), std::move(activeFormat),
                               std::move(activeTarget));
        });
}

static SemanticModel analyzeImpl(std::shared_ptr<CompilationUnit const> cu,
                                 DataModel dataModel,
                                 std::optional<AggregateLayoutParams> aggregateLayout,
                                 std::optional<VaListStrategy> vaListStrategy,
                                 std::optional<ObjectFormatKind> activeFormat,
                                 std::optional<std::string_view> activeTarget) {
    if (!cu) {
        std::fputs("dss::analyze fatal: null CompilationUnit\n", stderr);
        std::abort();
    }
    EngineState s{*cu};
    s.dataModel = dataModel;
    s.aggregateLayout = aggregateLayout;
    s.vaListStrategy = vaListStrategy;
    s.activeFormat = activeFormat;
    // Plan 25: own the arch-name string (the caller's string_view may be
    // transient) so the shipped-struct variant selector reads a stable value.
    if (activeTarget.has_value()) s.activeTarget = std::string{*activeTarget};

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
        // C 5.1.1.2 phase 6: the (possibly adjacent-concatenated) string-literal
        // EXPR rule, resolved by name from THIS schema's rule interner — the
        // same name HIR lowering's library-override path uses. Invalid for a
        // grammar without the rule (the per-token typing fallback then fires).
        idx.stringLiteralExprRule = sch.rules().find("stringLiteralExpr");
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
        // FC12a-core (D-FC12A-VARIADIC-CALLEE) + FC12b (D-FC12B-WIN64-VARIADIC-CALLEE):
        // inject the `va_list` type iff this language declares a va_arg surface
        // (config-driven: `vaArgRule` valid). The CONCRETE va_list TYPE is selected
        // by the active CC's `vaListStrategy` (BLOCKER-2: threaded into analyze() so
        // sizeof(va_list) is right per ABI — a wrong size mis-sizes the `ap` local
        // and corrupts the stack). Switch on the closed strategy (never cc.name /
        // arch / format), with a fail-loud default for an un-realized ABI:
        //   * SysVRegisterSave (or ABSENT — direct-API/back-compat) → `__va_list_tag`
        //     {u32,u32,void*,void*} + `va_list = __va_list_tag[1]` (24B). The C ABI
        //     definition; it array-decays to `__va_list_tag*` when passed to
        //     va_start/va_arg/va_end. The per-field BYTE OFFSETS at codegen come from
        //     the target's `vaListLayout`; this struct only makes the type resolvable
        //     by name + sizes `ap`.
        //   * HomogeneousPointer (Win64 + Apple arm64) → `va_list = char*` (a pointer
        //     to I8). NO __va_list_tag struct: sizeof(va_list)==8 sizes `ap` as one
        //     pointer.
        //   * Aapcs64DualCursor (AAPCS64 ARM64-ELF, FC12c) → `va_list = __va_list`, a
        //     typedef to the 5-field STRUCT DIRECTLY (NOT an array like SysV, NOT a
        //     pointer like Win64): {void* __stack; void* __gr_top; void* __vr_top;
        //     int __gr_offs; int __vr_offs;} (32B). The va_start/va_arg/va_end ap
        //     operand is this struct lvalue; its ADDRESS is the dual-cursor base.
        // A language without the surface injects nothing — no leak, no pollution.
        if (sch->semantics().vaArgRule.valid()) {
            auto& in = s.lattice.interner();
            VaListStrategy const strat =
                vaListStrategy.value_or(VaListStrategy::SysVRegisterSave);
            if (strat == VaListStrategy::Aapcs64DualCursor) {
                // AAPCS64 §B.4: `va_list = __va_list` (the struct directly). The five
                // members in declaration order — pointers first (8B each), then the two
                // i32 cursors (4+4) — give a 32B struct under natural alignment, exactly
                // the AAPCS64 `__va_list` layout. `va_list` typedefs DIRECTLY to this
                // struct (no array decay): the va_start/va_arg/va_end ap operand IS the
                // struct lvalue and its address is the dual-cursor base.
                TypeId const voidPtr = in.pointer(in.primitive(TypeKind::Void));
                TypeId const i32     = in.primitive(TypeKind::I32);
                std::array<TypeId, 5> vaFields{
                    voidPtr,   // __stack
                    voidPtr,   // __gr_top
                    voidPtr,   // __vr_top
                    i32,       // __gr_offs
                    i32,       // __vr_offs
                };
                TypeId const vaStructTy = in.structType("__va_list", vaFields);
                SymbolRecord structRec;
                structRec.name  = "__va_list";
                structRec.scope = builtinScope;
                structRec.tree  = InvalidTree;
                structRec.kind  = DeclarationKind::Type;
                structRec.type  = vaStructTy;
                SymbolId const structId = s.symbols.mint(structRec);
                s.scopes.injectBinding(builtinScope, "__va_list", structId);

                SymbolRecord vaRec;
                vaRec.name  = "va_list";
                vaRec.scope = builtinScope;
                vaRec.tree  = InvalidTree;
                vaRec.kind  = DeclarationKind::Type;
                vaRec.type  = vaStructTy;   // typedef to the struct DIRECTLY
                SymbolId const vaId = s.symbols.mint(vaRec);
                s.scopes.injectBinding(builtinScope, "va_list", vaId);
            } else if (strat == VaListStrategy::HomogeneousPointer) {
                // Win64: `va_list` is a plain `char*` (pointer to I8) — sizeof 8.
                TypeId const charPtr = in.pointer(in.primitive(TypeKind::I8));
                SymbolRecord vaRec;
                vaRec.name  = "va_list";
                vaRec.scope = builtinScope;
                vaRec.tree  = InvalidTree;
                vaRec.kind  = DeclarationKind::Type;
                vaRec.type  = charPtr;
                SymbolId const vaId = s.symbols.mint(vaRec);
                s.scopes.injectBinding(builtinScope, "va_list", vaId);
            } else {
                // SysVRegisterSave (or absent): __va_list_tag[1].
                TypeId const voidPtr = in.pointer(in.primitive(TypeKind::Void));
                std::array<TypeId, 4> tagFields{
                    in.primitive(TypeKind::U32),  // gp_offset
                    in.primitive(TypeKind::U32),  // fp_offset
                    voidPtr,                      // overflow_arg_area
                    voidPtr,                      // reg_save_area
                };
                TypeId const tagTy = in.structType("__va_list_tag", tagFields);
                SymbolRecord tagRec;
                tagRec.name  = "__va_list_tag";
                tagRec.scope = builtinScope;
                tagRec.tree  = InvalidTree;
                tagRec.kind  = DeclarationKind::Type;
                tagRec.type  = tagTy;
                SymbolId const tagId = s.symbols.mint(tagRec);
                // MF-4 (C 6.2.3): builtins-as-`DeclarationKind::Type` —
                // `__va_list_tag` here and shipped-descriptor typedefs — are
                // injected into the ORDINARY namespace (the default param). A
                // `struct __va_list_tag` reference therefore will NOT resolve
                // them (the tag-namespace lookup misses → S_UnknownType). That
                // is an INTENTIONAL fail-loud boundary, not a tag-namespace
                // bug: these are synthesized typedef-NAMES, not C tags, and no
                // c-subset source spells `struct __va_list_tag` (the type is
                // reached only through the `va_list` typedef alias). If a real
                // tagged builtin is ever needed, bind it with
                // SymbolNamespace::Tag instead.
                s.scopes.injectBinding(builtinScope, "__va_list_tag", tagId);

                TypeId const vaListTy = in.array(tagTy, 1);
                SymbolRecord vaRec;
                vaRec.name  = "va_list";
                vaRec.scope = builtinScope;
                vaRec.tree  = InvalidTree;
                vaRec.kind  = DeclarationKind::Type;
                vaRec.type  = vaListTy;
                SymbolId const vaId = s.symbols.mint(vaRec);
                s.scopes.injectBinding(builtinScope, "va_list", vaId);
            }
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
        // keyed by (name, namespace). An entry here whose symbol's tree is the
        // source tree is a real user declaration; a later same-name inject in
        // the SAME namespace is a conflict. C 6.2.3: a header's `struct Foo`
        // tag and the including file's `typedef … Foo` are DIFFERENT
        // namespaces — distinct keys, no false conflict — while tag+tag /
        // ordinary+ordinary same-name still collide. Built once per edge from
        // the source root scope. The namespace byte is appended after a NUL
        // (never present in an identifier) so the key stays collision-free.
        auto const nsKey = [](std::string_view name, SymbolNamespace ns) {
            std::string key{name};
            key.push_back('\0');
            key.push_back(static_cast<char>(ns));
            return key;
        };
        std::unordered_map<std::string, SymbolId> ownByName;
        for (auto const& [name, ns, sym] : s.scopes.bindingsOf(srcIt->second)) {
            if (sym.valid() && s.symbols.at(sym).tree.v == edge.sourceTree.v) {
                ownByName.emplace(nsKey(name, ns), sym);
            }
        }

        for (auto const& [name, ns, sym] : s.scopes.bindingsOf(tgtIt->second)) {
            if (auto it = ownByName.find(nsKey(name, ns)); it != ownByName.end()) {
                // The source file declared `name` itself AND includes a
                // header declaring it in the SAME namespace — duplicate. Fail
                // loud at the source, but only ONCE per (sourceTree, name,
                // namespace): a second include of the same (or another) header
                // re-declaring `name` is the same logical conflict, already
                // reported. The conflict key folds the namespace byte in so a
                // tag conflict and an ordinary conflict of the same name dedup
                // independently.
                if (!reportedConflicts.insert(
                        conflictKey(edge.sourceTree.v, nsKey(name, ns))).second) {
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
            // Re-inject into the SAME namespace it came from — a tag stays a
            // tag in the target's tagBindings.
            s.scopes.injectBinding(srcIt->second, std::string{name}, sym, ns);
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
            // Across BOTH namespaces: a user declaration of `name` (object,
            // function, typedef, OR a struct/union/enum tag) blocks a
            // descriptor symbol of that name — this is a NAME-level goal-2 skip
            // (the descriptor injects ordinary names; a same-name user tag
            // still claims the spelling, preserving the pre-tag-namespace
            // behavior). `ns` is intentionally unused here.
            for (auto const& [name, ns, sym] : s.scopes.bindingsOf(scope)) {
                (void)ns;
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
        // Struct/union/enum TAGS live in a SEPARATE namespace (C 6.2.3): a
        // descriptor `struct stat` does NOT collide with the ordinary `stat`
        // function, so tag first-wins dedup uses its own set.
        std::unordered_set<std::string> injectedTags;
        for (ShippedDescriptorRef const& ref :
             cu->shippedLibDescriptors()) {
            std::filesystem::path const& descPath = ref.path;  // (ref.span/buffer: the c8 gate)
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
            // Plan 25: thread the active (arch, format) so the decoder can SELECT
            // a struct's per-target `variants` field list (a struct's byte layout
            // diverges per target — `struct stat` is 144B on x86_64-linux, 128B on
            // arm64-linux). `s.activeTarget`/`s.activeFormat` are nullopt for
            // direct-API/LSP/test callers ⇒ no variant selection (flat-`fields`
            // structs decode as before). The string_view borrows `s.activeTarget`'s
            // string, which outlives this call.
            std::optional<std::string_view> const activeTargetView =
                s.activeTarget.has_value()
                    ? std::optional<std::string_view>{*s.activeTarget}
                    : std::nullopt;
            auto desc = ffi::readShippedLibDescriptor(
                descPath, s.lattice.interner(), s.lattice.registry(), s.reporter,
                s.dataModel, activeTargetView, s.activeFormat);
            if (!desc) continue;

            // c8: per-target AVAILABILITY gate. When the active object-format is
            // KNOWN (a real per-target compile — nullopt for direct-API/LSP/test
            // callers) and the descriptor RESTRICTS its formats (non-empty
            // `availableObjectFormats`) and the active format is NOT among them, the
            // header does not exist on this target → FAIL LOUD (like MSVC C1083 for
            // a POSIX `<sys/time.h>` on windows-pe), POSITIONED on the `#include`
            // line (the carried ref.span/buffer), and inject NOTHING. AGNOSTIC: a
            // config-declared set + a generic membership test, no `if(format==…)`.
            if (s.activeFormat.has_value()
                && !ffi::objectFormatInAvailabilitySet(desc->availableObjectFormats,
                                                       *s.activeFormat)) {
                // The SHARED availability predicate (ffi) — the SAME membership
                // test the preprocessor `__has_include` + macro-splice use, so the
                // `#include` gate here and `__has_include` can never disagree.
                ParseDiagnostic d;
                d.code     = DiagnosticCode::F_ShippedHeaderUnavailableForTarget;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = ref.buffer;
                d.span     = ref.span;
                d.actual   = desc->header;
                s.reporter.report(std::move(d));
                continue;   // unavailable for this target — inject nothing
            }

            for (auto const& sym : desc->symbols) {
                // GOAL-2: a user decl of this name wins — skip the descriptor's.
                if (userDeclaredNames.contains(sym.name)) continue;
                // Per-SYMBOL availability gate (the symbol-granularity sibling of
                // the header gate above). When the active object-format is KNOWN
                // and the symbol RESTRICTS its formats and the active format is NOT
                // among them, the symbol does not EXIST on this target → inject
                // nothing (not declared → not imported → a reference fails loud as
                // an undefined name). This is load-bearing: DSS imports EVERY
                // declared shipped extern, so declaring errno's macho-only __error
                // on an elf target would plant an undefined import that breaks the
                // dynamic link at load. AGNOSTIC: the SAME config-set membership
                // predicate the header gate + __has_include use, never if(format==).
                if (s.activeFormat.has_value()
                    && !ffi::objectFormatInAvailabilitySet(sym.availableObjectFormats,
                                                           *s.activeFormat))
                    continue;
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

            // Item 1: inject the descriptor's CONSTANTS (the neutral form of a
            // header's `#define` macro-constants, e.g. CHAR_BIT) as named
            // integer-constant symbols. Goal-2 (a user decl of the name wins) +
            // first-wins dedup share the SAME sets as symbols — constants,
            // typedefs, and symbols are one name namespace. A constant has NO
            // link surface, so it is NOT added to shippedExterns; it folds to
            // its value at HIR Ref-lowering AND in constant-expression position
            // (the const-eval direct-value arm), via `isInjectedConstant`.
            for (auto const& c : desc->constants) {
                if (userDeclaredNames.contains(c.name)) continue;
                if (!injectedNames.insert(c.name).second) continue;  // first wins
                SymbolRecord rec;
                rec.name               = c.name;
                rec.scope              = cuRoot;
                rec.tree               = InvalidTree;   // not a user decl
                rec.kind               = DeclarationKind::Variable;
                rec.type               = c.type;
                rec.enumValue          = c.value;       // the int64 bit-pattern
                rec.isInjectedConstant = true;
                rec.isConst            = true;          // a macro constant is not assignable
                SymbolId const id = s.symbols.mint(rec);
                s.scopes.injectBinding(cuRoot, c.name, id);
            }

            // Item 1: inject the descriptor's TYPEDEFS as `DeclarationKind::Type`
            // symbols, so the name resolves in TYPE position (resolveTypeNode
            // walks the scope chain). This block runs BEFORE Pass 1.5 type
            // resolution, so a descriptor typedef is visible to a `T x;`
            // declaration. (A builtin type of the same name wins —
            // resolveTypeNode checks `builtinTypeIds` before the alias lookup.)
            for (auto const& td : desc->typedefs) {
                if (userDeclaredNames.contains(td.name)) continue;
                if (!injectedNames.insert(td.name).second) continue;  // first wins
                SymbolRecord rec;
                rec.name  = td.name;
                rec.scope = cuRoot;
                rec.tree  = InvalidTree;   // not a user decl
                rec.kind  = DeclarationKind::Type;
                rec.type  = td.type;
                SymbolId const id = s.symbols.mint(rec);
                s.scopes.injectBinding(cuRoot, td.name, id);
            }

            // c7: inject the descriptor's STRUCTS as a TAG (so `struct tag v;`
            // resolves) + a populated field scope (so `v.field` resolves) +
            // `compositeScopeByType` (the member-access resolver's TypeId→scope
            // index). Built by hand here — there is no CST node to tree-walk — and
            // BYTE-IDENTICAL to a user-declared struct (Pass 1.5 @ the
            // compositeScopeByType line). The layout engine DERIVES the field
            // offsets from the field sizes; the descriptor declares no offsets.
            // (The va_list builtin inject omits the field scope on purpose: its
            // fields are reached only via va_* intrinsics, never by name.)
            for (auto const& st : desc->structs) {
                if (!injectedTags.insert(st.name).second) continue;   // first wins (tag ns)
                // A node-independent field scope, parented at the CU root.
                ScopeId const fieldScope =
                    s.scopes.pushScope(cuRoot, NodeId{}, InvalidTree);
                for (std::uint32_t i = 0; i < st.fields.size(); ++i) {
                    SymbolRecord f;
                    f.name       = st.fields[i].name;
                    f.scope      = fieldScope;
                    f.tree       = InvalidTree;   // not a user decl
                    f.kind       = DeclarationKind::Variable;
                    f.type       = st.fields[i].type;
                    f.fieldIndex = i;             // == position in the interned operands
                    SymbolId const fid = s.symbols.mint(f);
                    s.scopes.injectBinding(fieldScope, st.fields[i].name, fid);
                }
                s.compositeScopeByType[st.typeId.v] = fieldScope;
                // The struct TAG, in the TAG namespace (C 6.2.3) so a `struct tag`
                // reference resolves; `structScope` links it to its field scope.
                SymbolRecord tag;
                tag.name        = st.name;
                tag.scope       = cuRoot;
                tag.tree        = InvalidTree;
                tag.kind        = DeclarationKind::Type;
                tag.type        = st.typeId;
                tag.structScope = fieldScope;
                SymbolId const tagId = s.symbols.mint(tag);
                s.scopes.injectBinding(cuRoot, st.name, tagId, SymbolNamespace::Tag);
            }
        }
    }

    // Pass 1.5 per tree: resolve declaration types + function signatures.
    for (auto const& tree : trees) {
        if (!tree.root().valid()) continue;
        s.activate(tree.schema());
        resolveDeclTypes(s, *s.idx().cfg, tree, tree.root(), treeRootScope.at(tree.id().v));
    }

    // D-CSUBSET-FN-PROTOTYPE: function-redeclaration COMPATIBILITY sweep. Pass 1
    // merged each (proto/def) pair into `mergedFnDecls` {survivor, absorbed};
    // now that Pass 1.5 has resolved both records' FnSig types, compare them.
    // Interner TypeId equality IS structural FnSig equality (return type AND the
    // full parameter list — the interner dedups identical signatures), so a
    // simple `.v` inequality means an INCOMPATIBLE redeclaration (C 6.7p4 /
    // 6.9.1: `int f(int); long f(int){…}`). Fail loud at the absorbed (later/
    // redundant) declaration with a related-location at the survivor; do NOT
    // silently pick one signature. Both `.type` must be valid (an unresolved
    // type already produced its own diagnostic — don't pile on).
    {
        std::unordered_map<std::uint32_t /*TreeId.v*/, Tree const*> treeById;
        for (auto const& tree : trees) treeById[tree.id().v] = &tree;
        for (auto const& [survivor, absorbed] : s.mergedFnDecls) {
            if (!survivor.valid() || !absorbed.valid()) continue;
            auto const& sRec = s.symbols.at(survivor);
            auto const& aRec = s.symbols.at(absorbed);
            if (!sRec.type.valid() || !aRec.type.valid()) continue;
            if (sRec.type.v == aRec.type.v) continue;   // compatible — merged
            auto aTreeIt = treeById.find(aRec.tree.v);
            if (aTreeIt == treeById.end()) continue;
            Tree const& aTree = *aTreeIt->second;
            ParseDiagnostic d;
            d.code     = DiagnosticCode::S_IncompatibleRedeclaration;
            d.severity = DiagnosticSeverity::Error;
            d.buffer   = aTree.source().id();
            d.span     = aTree.span(aRec.declNode);
            d.actual   = aRec.name;
            auto sTreeIt = treeById.find(sRec.tree.v);
            if (sTreeIt != treeById.end()) {
                Tree const& sTree = *sTreeIt->second;
                d.related.push_back(RelatedLocation{
                    sTree.source().id(),
                    sTree.span(sRec.declNode),
                    "previously declared here with a different signature",
                });
            }
            s.reporter.report(std::move(d));
        }
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
        std::move(s.nullPointerConstantNodes),
        std::move(shippedExterns),
        dataModel,
    };
}

} // namespace dss
