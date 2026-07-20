#include "analysis/semantic/semantic_analyzer.hpp"

#include "analysis/compilation_unit/unit_attribute.hpp"
#include "analysis/semantic/scope_tree.hpp"
#include "analysis/semantic/constant_symbol_fold.hpp" // Item 1: shared enum/constant Ref->literal builder
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/symbol_table.hpp"
#include "analysis/semantic/type_rules.hpp"
#include "core/substrate/large_stack_call.hpp"  // D-PARSE-DEEP-FRONTEND-STACK: run analyze on a large stack
#include "core/types/attribute_naming.hpp"   // D-CSUBSET-PACKED: stripDunder (shared with the preprocessor)
#include "core/types/data_model.hpp"
#include "core/types/decl_prefix_strip.hpp"   // declRoleChildren / descendVisibleDecl / specifierPrefixChild
#include "core/types/declarator_walk.hpp"     // FC4: declaratorNameNode / collectDeclarators
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/bit_int_value.hpp"
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
#include "core/types/wide_string_encode.hpp"  // C 6.4.5: encodeWideString (wide/UTF code-unit count, shared with HIR)
#include "ffi/shipped_lib_descriptor.hpp"   // FF11 neutral-JSON descriptor reader
// D-LANG-TYPE-IDENTITY-VOCABULARY: the CROSS-descriptor identity invariant. The
// per-file reader sees ONE descriptor and cannot know a sibling spells the same
// tag differently; this CU-wide accumulator can, and fails loud.
#include "ffi/shipped_type_consistency.hpp"
#include "hir/cst_const_eval.hpp"
#include "hir/hir_text.hpp"   // c104: parseTypeFromText (builtin signatureText decode)
#include "hir/hir_op.hpp"   // FC6 c-subtreeType: HirOpKind / opName / isComparison (the per-verb laws cst_to_hir uses)

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <array>
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
    // C11/C23 6.7.10 (D-CSUBSET-STATIC-ASSERT): true for the static-assertion
    // declaration rule. Pass 2 (`pass2Post`) const-evaluates its condition + emits
    // S_StaticAssertFailed on a zero / non-constant fold. Empty ⇒ no surface.
    std::unordered_map<std::uint32_t, bool>        staticAssertByRule;
    // FC17.9(i) (D-CSUBSET-INLINE-ASM): true for the inline-asm statement rule
    // (asmStmt). pass2Post decodes its template child and emits
    // S_InlineAsmNonEmptyTemplate unless it decodes to strictly zero bytes. Empty
    // ⇒ no surface.
    std::unordered_map<std::uint32_t, bool>        inlineAsmByRule;
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
    // C11/C23 6.4.5: a prefixed string literal (`L"…"`/`u"…"`/`U"…"`/`u8"…"`) types
    // as `Array<elementCore, N+1>` where the element core is keyed by the literal's
    // ACTUAL opener token kind. Built from the language's `hirLowering`
    // `stringLiteralPrefixes` at index-build time; WideStringStart (wchar_t) is
    // FORMAT-resolved here (pe→U16, elf/macho→I32, D-FFI-STDDEF-WCHAR-PE-WIDTH)
    // because THIS tier has `activeFormat`. Empty ⇒ the language declares no
    // prefixes (the scalar `stringLiteralElementCore` narrow default applies). The
    // HIR tier reads the element core BACK off the stamped node type (it lacks
    // format), so this map is the single format-aware resolution point.
    std::unordered_map<std::uint32_t /*opener SchemaTokenId.v*/, TypeKind>
                                                   stringLiteralElementCoreByStart;
    // C11/C23 6.4.5p5 (Cycle D): the SET of NON-narrow string-opener token kinds
    // (`u"`/`U"`/`u8"`/`L"`). An opener is non-narrow when its base `elementCore` OR
    // ANY `elementCoreByFormat` value is non-narrow (not Char/Byte) — a FORMAT-
    // AGNOSTIC classification keyed on the token KIND, MIRRORING the HIR tier's
    // `isWideStringOpenerKind`. The adjacent-concat effective-prefix fold uses THIS
    // (never the format-resolved `byStart` core) to detect a run mixing two DIFFERENT
    // non-narrow prefixes: on pe `u"`/`L"` both resolve to U16, so a core-keyed
    // conflict would silently accept `u"a" L"b"` on Windows while rejecting it on
    // Linux. Empty ⇒ the language declares no wide prefixes.
    std::unordered_set<std::uint32_t /*opener SchemaTokenId.v*/>
                                                   nonNarrowStringOpeners;
    // C11/C23 6.4.4.4: the CHARACTER-constant body token kind (`CharLiteral`) + the
    // WIDE openers' format-resolved element cores. `charLiteralBodyToken` gates the
    // char-typing override (only a char body token is a candidate); the map holds
    // ONLY the wide/UTF openers (`L'`/`u'`/`U'`/`u8'`) → their C23 scalar core
    // (WideCharStart format-resolved here, pe→U16 / elf,macho→I32). The NARROW
    // `CharStart` is DELIBERATELY absent — the unprefixed `'x'` stays `int` via the
    // flat `literalTypeIds` path (other integer-literal consumers key on that entry),
    // so a wide opener OVERRIDES the flat int type and the narrow one never does.
    // Empty ⇒ the language declares no wide char prefixes (byte-identical). Like the
    // string map, this is the single format-aware resolution point (the HIR tier
    // reads the resolved core back off the stamped body token; it lacks format).
    SchemaTokenId                                  charLiteralBodyToken{};
    std::unordered_map<std::uint32_t /*opener SchemaTokenId.v*/, TypeKind>
                                                   charLiteralElementCoreByStart;
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
    // c34 (D-CSUBSET-ARRAY-SIZE-INFERENCE): the brace-init-list rule + its
    // top-level element rule, resolved by NAME (like `stringLiteralExprRule`).
    // Used ONLY to count the top-level `initElement` children of a `[]` array's
    // brace initializer so an `int a[] = {e0,e1,…}` infers `Array<elem, count>`
    // (C 6.7.9p22). Invalid (no such rule) ⇒ the inference never fires for a
    // brace init (toy/tsql, or a grammar without these rules). Source-agnostic.
    RuleId                                         braceInitListRule{};
    RuleId                                         initElementRule{};
    // FC3 c1: type-specifier multiset resolution (C 6.7.2). The
    // VOCABULARY is every token kind appearing in any `typeSpecifiers`
    // row — `resolveTypeNode` treats an Internal node whose visible
    // children are ALL vocabulary tokens as a specifier run. The SETS
    // map keys the canonical (sorted, comma-joined kind-id) multiset to
    // its interned TypeId (data model already applied). Empty for
    // languages without the table (toy / tsql) — the arm never fires.
    std::unordered_set<std::uint32_t>              typeSpecifierVocabulary;
    std::unordered_map<std::string, TypeId>        typeSpecifierSets;
    // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): multiset keys of rows carrying a
    // `coreByLongDoubleFormat` map that the ACTIVE format leaves UNREALIZED
    // (axis None — wasm/spirv/direct-API). Diverted here at index build
    // instead of interned (a base-core intern would silently bind the F64
    // meaning under an undeclared representation); the multiset-lookup miss
    // path consults this set to emit the precise S_LongDoubleFormatUndeclared
    // instead of the generic S_InvalidTypeSpecifierCombination.
    std::unordered_set<std::string>                unrealizedLongDoubleSets;
};

// One transient per `analyze()` call. Consumed into the returned model.
struct EngineState {
    explicit EngineState(CompilationUnit const& cu)
        : lattice{cu.id(), cu.compositeSourceLanguage()},
          nodeToSymbol{cu},
          nodeToType{cu},
          nodeToSelectedExpr{cu},
          nullPointerConstantNodes{cu} {}

    DiagnosticReporter         reporter;
    TypeLattice                lattice;
    ScopeTree                  scopes;
    SymbolTable                symbols;
    UnitAttribute<SymbolId>    nodeToSymbol;
    UnitAttribute<TypeId>      nodeToType;
    // FC16 (D-CSUBSET-GENERIC-SELECTION): for each `_Generic` node, the NodeId of
    // the SELECTED association's result-expression (the winner of the compile-time
    // type match). Written by Pass 2's generic-selection arm; read by the CST→HIR
    // `lowerGeneric`, which lowers ONLY that recorded sub-expression (its type +
    // value), discarding the non-selected associations (UNEVALUATED per 6.5.1.1p3).
    // MUST be a TREE-KEYED UnitAttribute (NOT a flat NodeId.v map): NodeId is tree-
    // LOCAL, so a multi-source CU restarts numbering per tree — a flat map would
    // alias node K across files. Routes per-tree exactly like nodeToType/nodeToSymbol.
    UnitAttribute<NodeId>      nodeToSelectedExpr;
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
    // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): the analysis-time `long double` axis
    // (`analyze()`'s parameter — effectiveLongDoubleFormat(target, format)).
    // Read by `buildIndexes` (the `coreByLongDoubleFormat` typeSpecifiers
    // overrides — a map-carrying row under `None` is diverted UNREALIZED,
    // never base-core-resolved) and the float-literal ladder. Set ONCE before
    // any index is built, like `dataModel`.
    LongDoubleFormat           longDoubleFormat = LongDoubleFormat::None;
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
    // c82 (D-CSUBSET-PARAM-ARRAY-ADJUSTMENT): the builtin `va_list` TypeId the
    // per-CC injection minted (set alongside the builtin scope build). TWO
    // consumers: the FF11 shipped-descriptor read binds it as the `va_list`
    // named-type alias, and `adjustArrayToPointer` EXCLUDES it — a va_list
    // param keeps its per-CC form (SysV: the `__va_list_tag[1]` array) because
    // the va_* machinery (c63) owns va_list parameter passing END-TO-END
    // (param slot + decay-at-call + va_arg addressing, validated per-ABI);
    // C 7.16 makes va_list observable ONLY through va_*/forwarding, so the
    // 6.7.6.3p7 adjustment has no program-visible effect on it, while
    // adjusting it would silently add an indirection level under va_arg
    // (a SysV SIGSEGV, witnessed mid-c82). `nullopt` ⇒ no exclusion.
    std::optional<TypeId> vaListType;
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

// FC17 (D-CSUBSET-ENUM-UNDERLYING-TYPE): the FIRST visible child of `node` whose
// rule is `childRule`, or an invalid NodeId if none. The value-returning sibling
// of `nodeHasVisibleChildOfRule` — used to locate the enum underlying-type clause
// (`enumTypeSpecifier`) inside an enum specifier so the semantic tier can resolve
// its type child. Generic + agnostic: keyed on a config-resolved RuleId, never a
// keyword. Direct-children only (the clause is always a direct child).
[[nodiscard]] NodeId
findVisibleChildOfRule(Tree const& tree, NodeId node, RuleId childRule) {
    if (!childRule.valid()) return NodeId{};
    for (NodeId c : visibleChildren(tree, node)) {
        if (tree.kind(c) == NodeKind::Internal && tree.rule(c) == childRule) {
            return c;
        }
    }
    return NodeId{};
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
            && isFnSuffixRule(tree.rule(c), dc)) {
            return true;
        }
    }
    return false;
}

// c32 (D-CSUBSET-FNPTR-PARAM-SCOPE): is `directNode` (a `directRule` node) a
// NAME-bearing direct declarator — i.e. does it have a direct visible child that
// is the declarator NAME token (`int f(int)` / `int f(int){…}`)? A function
// POINTER's direct declarator (`int (*fp)(int)`) instead carries a
// `groupRule`/`parenDeclarator` base (the name is nested INSIDE it), so this is
// false for it. This is the same name-vs-fnptr distinction `hasFnSuffixOnName`
// draws, expressed at the direct-declarator node (where the fnSuffix-suffix walk
// lands) rather than at the name node.
[[nodiscard]] bool
directDeclaratorIsNameBearing(Tree const& tree, NodeId directNode,
                              DeclaratorConfig const& dc) {
    if (!directNode.valid() || tree.kind(directNode) != NodeKind::Internal
        || tree.rule(directNode) != dc.directRule) {
        return false;
    }
    for (NodeId c : visibleChildren(tree, directNode)) {
        if (tree.kind(c) == NodeKind::Token
            && tree.tokenKind(c) == dc.nameToken) {
            return true;
        }
    }
    return false;
}

// c32 (D-CSUBSET-FNPTR-PARAM-SCOPE): is the declaration `decl` at `node` a
// FUNCTION DEFINITION (a body present) — NOT a prototype/variable/typedef? True
// iff the kindByChild discriminator resolves to `Function` (the body-block
// discriminator matched). Factored from the Pass-1.5 isFunctionForm test
// (resolveDeclTypes) so the two can't drift. A row with no kindByChild can never
// be a function definition through this path (returns false).
[[nodiscard]] bool
declNodeIsFunctionDefinition(DeclarationRule const& decl, Tree const& tree,
                             NodeId node) {
    if (!decl.kindByChild.has_value()) return false;
    auto const& disc = *decl.kindByChild;
    NodeId const disChild = descendVisibleDecl(tree, node, disc.childPath, decl);
    return disChild.valid()
        && tree.kind(disChild) == NodeKind::Internal
        && tree.rule(disChild) == disc.whenRule
        && disc.whenKind == DeclarationKind::Function;
}

[[nodiscard]] bool
nodeOpensChildScope(EngineState const& s, SemanticConfig const& cfg,
                    Tree const& tree, NodeId node);

// c32 (D-CSUBSET-FNPTR-PARAM-SCOPE): does `node` (a `prototypeParamScopeRule`
// param-list node) open a per-declarator FUNCTION-PROTOTYPE scope (C 6.2.1p4)?
// TRUE for every fn-declarator's param list EXCEPT a function DEFINITION's OWN
// params — a definition's params must keep binding into the definition's scope so
// they reach the body. A param list is a definition's own iff its owning fnSuffix
// sits on a NAME-bearing direct declarator (NOT a function pointer, whose suffix
// rides a `parenDeclarator` base) AND the enclosing declaration is a function
// DEFINITION (a body present). So:
//   * fn-ptr member / typedef / param (`int (*a)(int x)`): base is a
//     parenDeclarator ⇒ not name-bearing ⇒ OPEN an isolated scope.
//   * a bare prototype (`int f(int x);`): name-bearing BUT no body ⇒ OPEN an
//     isolated scope (the proto's params have function-prototype scope; the proto
//     name itself re-homes to file scope via the separate `isProto` path).
//   * a function definition (`int add(int x){…}`): name-bearing AND a body ⇒ do
//     NOT open — its params bind into the topLevelDecl scope, body-visible.
// Reuses the config-driven `hasFnSuffixOnName`/`directDeclaratorIsNameBearing`
// shape tests and `declNodeIsFunctionDefinition`; no rule/keyword hardcoded.
[[nodiscard]] bool
isPrototypeParamScopeNode(EngineState const& s, SemanticConfig const& cfg,
                          Tree const& tree, NodeId node) {
    if (!cfg.declarators.has_value()
        || !cfg.declarators->prototypeParamScopeRule.has_value()) {
        return false;
    }
    auto const& dc = *cfg.declarators;
    if (tree.kind(node) != NodeKind::Internal
        || tree.rule(node) != *dc.prototypeParamScopeRule) {
        return false;
    }
    // paramList → fnSuffix (its direct parent must be the fn-suffix rule).
    NodeId const fnSuffix = tree.parent(node);
    if (!fnSuffix.valid() || tree.kind(fnSuffix) != NodeKind::Internal
        || !isFnSuffixRule(tree.rule(fnSuffix), dc)) {
        // Not a function-suffix param list (defensive — the role is the
        // fnSuffix's param list by construction). Treat as a prototype scope
        // (the safe direction: isolate the names) only when it IS a fnSuffix;
        // otherwise leave scoping unchanged.
        return false;
    }
    // fnSuffix → direct declarator. A DEFINITION's own params require the suffix
    // to sit on a NAME-bearing direct declarator AND the enclosing declaration to
    // be a function definition (a body). Anything else is a non-definition
    // declarator's param list → an isolated prototype scope.
    NodeId const direct = tree.parent(fnSuffix);
    bool const nameBearing = directDeclaratorIsNameBearing(tree, direct, dc);
    if (!nameBearing) return true;   // fn-pointer suffix ⇒ isolate
    // Walk up to the nearest enclosing declaration-row node; it is a function
    // definition iff its kindByChild resolves to Function (a body present).
    for (NodeId cur = tree.parent(direct); cur.valid(); cur = tree.parent(cur)) {
        if (tree.kind(cur) != NodeKind::Internal) continue;
        auto const it = s.idx().declByRule.find(tree.rule(cur).v);
        if (it == s.idx().declByRule.end()) continue;
        // The nearest enclosing declaration row. A function definition's own
        // params bind into ITS scope (not isolated); a non-definition (a bare
        // prototype, a typedef, a struct field, a param) isolates.
        return !declNodeIsFunctionDefinition(cfg.declarations[it->second],
                                             tree, cur);
    }
    // No enclosing declaration row found (defensive) ⇒ isolate (safe direction:
    // a stray fn-declarator's params never leak into the file scope).
    return true;
}

// c32 (D-CSUBSET-FNPTR-PARAM-SCOPE): the SINGLE source of the "does this node
// open a child scope" decision, shared by ALL THREE scope-deriving passes —
// Pass 1 (`pass1Node`, which pushes the scope), Pass 1.5 (`resolveDeclTypes`)
// and Pass 2 (the `pass2` driver), the latter two re-deriving the SAME scope by
// anchor lookup. Folding the decision here means a param can never bind in one
// scope (Pass 1) yet be looked up in another (Pass 1.5 / Pass 2). Two ways a
// node opens a child scope:
//   (1) it is a `scopes`-rule node that is a DEFINITION here (the c25 dual-mode
//       `definesWhenChildRule` gate — a body-absent composite specifier opens
//       none), OR
//   (2) it is a per-declarator function-prototype param list (c32).
[[nodiscard]] bool
nodeOpensChildScope(EngineState const& s, SemanticConfig const& cfg,
                    Tree const& tree, NodeId node) {
    if (tree.kind(node) != NodeKind::Internal) return false;
    auto const rule = tree.rule(node);
    if (s.idx().scopeByRule.contains(rule.v)) {
        bool defining = true;
        if (auto gIt = s.idx().declByRule.find(rule.v);
            gIt != s.idx().declByRule.end()) {
            defining = isDefinitionAtNode(cfg.declarations[gIt->second], tree, node);
        }
        if (defining) return true;
    }
    return isPrototypeParamScopeNode(s, cfg, tree, node);
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
// C11/C23 6.5.1.1 (D-CSUBSET-GENERIC-SELECTION / -GENERIC-RESULT-TYPE-DEDUCTION):
// the ONE generic-selection chokepoint. Given a `_Generic` node and its ALREADY-
// COMPUTED controlling type, decide which association WINS from the typed/
// `default` associations, so BOTH tiers type the selection from its winner rather
// than its controlling expression: `subtreeType` (Pass 1.5 — `auto` inference /
// array-dim `sizeof`) and `pass2Post` (Pass 2). It does NOT call `subtreeType`
// for the controlling type — the caller supplies it (subtreeType types it on its
// own work-stack, pass2Post at top level) so a controlling-nested `_Generic`
// never host-recurses (D-PARSE-DEEP-NEST-RECURSION-MEMORY). FULLY SILENT: it
// resolves each association type-name only to match, then rolls the reporter back
// (the `_BitInt`/typeof-bitfield constraint diagnostics fire UNCONDITIONALLY and
// bypass the dedup window, so a bare resolve would multi-emit) — pass2Post's
// stamp-loop resolve is the SOLE emitter. Returns the winner + the typed
// associations' type-nodes (pass2Post re-resolves them LOUD to stamp + fail loud).
// Defined below subtreeType (mutual recursion; both are forward-declared here).
struct GenericSelection {
    NodeId selected{};                // matched-or-default result-expr, or Invalid
    int    matchCount = 0;            // typed-association matches (ambiguity check)
    NodeId defaultExpr{};             // first `default:` result-expr, or Invalid
    bool   anyBadType = false;        // a value-in-type-position was rejected
    bool   controllingResolved = false;   // the controlling type resolved (no-match guard)
    std::vector<NodeId> typedAssocTypeNodes;   // pass2Post re-resolves (loud) + stamps
};
[[nodiscard]] GenericSelection
selectGenericAssociation(EngineState const& s, SemanticConfig const& cfg,
                         Tree const& tree, NodeId node, ScopeId scope,
                         TypeId controllingType);
// FC17.5 (D-CSUBSET-AUTO-TYPE-INFERENCE): the ONE literal-typing chokepoint
// (extracted from pass2Post — defined alongside it below), forward-declared so
// the Pass-1.5 inference arm can PRE-STAMP an initializer's literal leaves
// before `subtreeType` reads them (subtreeType types identifier leaves by
// scope lookup but literal leaves only via the typeAt stamps Pass 2 writes).
void typeLiteralIfAny(EngineState& s, SemanticConfig const& cfg,
                      Tree const& tree, NodeId node);
// Core operator NAME → HirOpKind (reverse of opName()); std::nullopt if the
// `target` string is a special tag (AddressOf/…), not a core op. Defined below;
// forward-declared so pass2Post's nullptr operator gate (D-CSUBSET-NULLPTR) can
// classify a binary/unary operator by its shared HIR verb.
[[nodiscard]] std::optional<HirOpKind> coreOpFromNameSem(std::string_view s);

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
        AmbiguousField,   // FC16: name matches ≥2 sibling anonymous members (C 6.7.2.1 ¶13) — fail loud
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
//
// `opaqueRules` (D-CSUBSET-TYPEOF): rule ids whose subtree is OPAQUE to this
// qualifier scan — descent stops at such a node WITHOUT inspecting it, EVEN when
// it is the scan ROOT (unlike `declByRule`, which keeps the root in scope). This
// makes a `typeof`/`typeof_unqual` OPERAND invisible to the coarse volatile/const
// scans: the typeof arm already resolves the operand's own qualifiers (preserve
// for typeof, strip for typeof_unqual), so re-finding a literal `volatile`/`const`
// INSIDE the operand would silently re-apply what typeof_unqual just stripped —
// the CRITICAL silent-miscompile this opacity closes. Root-inclusive because these
// scans are frequently rooted DIRECTLY at a typeof node (the typeofSpecifier
// wrapper's own generic descent scans its `typeofType`/`typeofValue` child as the
// scan root). Empty span (the default) ⇒ behaviour is EXACTLY the legacy scan.
[[nodiscard]] bool
subtreeContainsToken(Tree const& tree, NodeId node, SchemaTokenId kind,
                     std::unordered_map<std::uint32_t, std::size_t> const*
                         declByRule = nullptr,
                     std::span<RuleId const> opaqueRules = {}) {
    if (!node.valid() || !kind.valid()) return false;
    std::vector<NodeId> stack{node};
    bool firstPop = true;
    while (!stack.empty()) {
        NodeId cur = stack.back();
        stack.pop_back();
        if (tree.kind(cur) == NodeKind::Token && tree.tokenKind(cur) == kind) {
            return true;
        }
        bool stop = false;
        if (tree.kind(cur) == NodeKind::Internal) {
            std::uint32_t const rv = tree.rule(cur).v;
            // Opaque rules (typeof operands) stop descent ALWAYS — even at the
            // root (firstPop) — so a stripped-then-re-scanned qualifier can never
            // leak back out of a typeof operand.
            for (RuleId const& opq : opaqueRules) {
                if (opq.valid() && opq.v == rv) { stop = true; break; }
            }
            // Stop descent at any nested declaration-rule node (the root itself
            // IS a decl subtree by contract and stays in scope — hence !firstPop).
            if (!stop && !firstPop && declByRule != nullptr
                && declByRule->contains(rv)) {
                stop = true;
            }
        }
        firstPop = false;
        if (stop) continue;
        for (auto const& child : tree.children(cur)) {
            if (!isEmptySpace(tree.flags(child))) stack.push_back(child);
        }
    }
    return false;
}

// c36 (D-CSUBSET-MUTABLE-POINTER-TO-CONST): does THIS declarator declare a
// const OBJECT? `const` qualifies the type it directly modifies (C 6.7.3):
// `const char *p` qualifies the POINTEE — the pointer OBJECT `p` is MUTABLE
// (`p += 1` is legal); only a POST-star `* const` (inside the OUTERMOST pointer
// layer) qualifies the object itself (`char * const p`). The legacy coarse
// `subtreeContainsToken(const)` over the whole declaration mis-fires for a
// pointer-to-const — it sees the head/pointee const and wrongly marks the
// pointer object const. This is the SAME bug c27 fixed for `volatile` (which
// became a type qualifier because it affects CODEGEN; const affects only
// assignability, so it stays a symbol flag — computed correctly HERE).
//
// Rule: descend to the inner declaratorRule; if it has >=1 pointer layer, the
// object is const iff the LAST (outermost, source-order) pointer layer carries
// the const marker. With NO pointer layer (a scalar, or a pointer hidden in a
// typedef) the head/east const applies to the object directly — the legacy
// whole-declaration scan is exactly right. Mirrors the per-layer walk in
// `declaratorDeclaredType` (the c27 volatile path) — one structural model, the
// const verdict cannot drift from the type the declarator actually forms.
//
// SCOPE: a GROUPED pointer declarator `char (* const p)` hides its layer inside
// the group, so it falls to the whole-decl scan (STATUS QUO — no regression; it
// was coarse before too). Grouped-with-head-const stays a pre-existing
// over-approximation (anchor D-CSUBSET-GROUPED-DECLARATOR-CONST).
[[nodiscard]] bool
declaratorObjectIsConst(Tree const& tree, NodeId declNode, NodeId dNode,
                        DeclaratorConfig const& dc, SchemaTokenId constMarker,
                        std::unordered_map<std::uint32_t, std::size_t> const*
                            declByRule,
                        NodeId headNode,
                        std::span<RuleId const> opaqueRules = {}) {
    // Descend a per-slot wrapper (initDeclarator / memberDeclarator) to the
    // inner declaratorRule — the same descent declaratorDeclaredType uses.
    NodeId inner = dNode;
    if (dNode.valid() && tree.kind(dNode) == NodeKind::Internal) {
        RuleId const dr = tree.rule(dNode);
        if (dr == dc.initDeclaratorRule
            || (dc.memberDeclaratorRule.has_value()
                && dr == *dc.memberDeclaratorRule)) {
            NodeId const d = declarator_walk_detail::firstChildOfRule(
                TreeDeclaratorView{tree}, dNode, dc.declaratorRule);
            if (d.valid()) inner = d;
        }
    }
    // The LAST pointer layer in source order = the outermost pointer, whose
    // qualifier governs the OBJECT. visibleChildren is source-ordered.
    NodeId lastLayer{};
    if (inner.valid() && tree.kind(inner) == NodeKind::Internal) {
        for (NodeId c : visibleChildren(tree, inner)) {
            if (tree.kind(c) == NodeKind::Internal
                && tree.rule(c) == dc.pointerLayerRule) {
                lastLayer = c;
            }
        }
    }
    if (lastLayer.valid())
        return subtreeContainsToken(tree, lastLayer, constMarker, declByRule,
                                    opaqueRules);
    // c58 (D-CSUBSET-INITIALIZER-CONST-TOKEN-LEAK): a SCALAR object's const comes
    // ONLY from its type-specifier HEAD, never a `const` TOKEN inside its
    // INITIALIZER (e.g. `int r = f((const char*)p)` — the cast's const must NOT mark
    // `r` const; sqlite nocaseCollatingFunc). The old fallback scanned the whole
    // `declNode`, which spans the initializer → a false S_ConstViolation on a later
    // `r = …`. Scan the HEAD (base type, no pointer stars) — mirrors the legacy
    // positional path's `kids[*decl.typeChild]` scoping; a genuine `const int r`
    // keeps its const in the head, so it stays correctly rejected.
    return subtreeContainsToken(tree, headNode.valid() ? headNode : declNode,
                                constMarker, declByRule, opaqueRules);
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

// D-LANG-TYPE-IDENTITY-VOCABULARY: the ONE seam every ENGINE-SYNTHESIZED
// standard type mints through (`sizeof`/`_Alignof` → C's `size_t`; a
// same-pointee `p - q` → `ptrdiff_t`). The language config declares WHICH
// vocabulary entry serves the role under the active data model; the engine only
// carries the resolved (core, tag) pair here — it never sees a name to compare,
// so no site branches on a spelling. An UNDECLARED role falls back to
// `historicCore` as an ANONYMOUS primitive, which is byte-for-byte what these
// sites did before the block existed (and what every non-C schema still gets).
[[nodiscard]] TypeId synthesizedType(TypeInterner& interner,
                                     SynthesizedTypeRule const& rule,
                                     DataModel dm, TypeKind historicCore) {
    if (auto const r = rule.resolve(dm)) {
        return interner.primitive(r->first, r->second);
    }
    return interner.primitive(historicCore);
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
    if (cfg.staticAssertRule.valid()) idx.staticAssertByRule[cfg.staticAssertRule.v] = true;
    if (cfg.inlineAsmRule.valid())    idx.inlineAsmByRule[cfg.inlineAsmRule.v] = true;
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
        // c-subset's `long` is I64 base / I32 under LLP64. The vocabulary tag
        // (loader-DERIVED from the matching typeSpecifiers row) supplies the
        // IDENTITY, so the text-keyed path and the keyword path intern the
        // SAME TypeId for `long` (D-LANG-TYPE-IDENTITY-VOCABULARY).
        idx.builtinTypeIds[bt.name] =
            s.lattice.interner().primitive(bt.resolveCore(s.dataModel),
                                           bt.vocabularyName);
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
        // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): axis-aware resolution. nullopt ⇔
        // the row depends on the long-double axis but the active format
        // declared none — divert the key UNREALIZED (never intern the base
        // core, which is the F64-axis meaning; see the SchemaIndexes field).
        auto const resolved = ts.resolveCore(s.dataModel, s.longDoubleFormat);
        if (!resolved.has_value()) {
            idx.unrealizedLongDoubleSets.insert(std::move(key));
            continue;
        }
        // C99 _Complex (D-CSUBSET-COMPLEX §6.2.5, MINOR-8): a `complex` row's
        // resolved core is the ELEMENT float — wrap it in interner.complex() so the
        // multiset `double _Complex`/`float _Complex`/`long double _Complex` binds a
        // genuine Complex TypeId at EVERY type position (decl/param/member/cast/
        // sizeof funnel through this table). The element rode the same resolveCore
        // axis, so `long double _Complex`'s element is F80/F128/F64 for free.
        // D-LANG-TYPE-IDENTITY-VOCABULARY: identity comes from the row's
        // VOCABULARY name, representation from the resolved core. The two axes
        // are independent, so a target that gives `long` and `int` (LLP64) or
        // `long double` and `double` (f64 axis) the same core still yields two
        // DISTINCT TypeIds. A `complex` row's name rides its ELEMENT (the
        // Complex wrapper is structural), which is exactly what keeps
        // `long double _Complex` distinct from `double _Complex`.
        TypeId const elemTy =
            s.lattice.interner().primitive(*resolved, ts.name);
        idx.typeSpecifierSets[std::move(key)] =
            ts.complex ? s.lattice.interner().complex(elemTy) : elemTy;
    }
    // The C 6.3.1.1 conversion rank of each NAMED vocabulary entry — the
    // usual-arithmetic-conversions tie-break for two operands that share a core
    // but not an identity. Declared once per index build; the loader's cross-row
    // consistency check guarantees every row spelling a name agrees on its rank.
    for (auto const& ts : cfg.typeSpecifiers) {
        if (ts.name.empty()) continue;
        s.lattice.interner().declareVocabularyRank(ts.name, ts.rank);
    }
}

// c26 D-CSUBSET-ABSTRACT-DECLARATOR-TYPE-NAME: forward declaration — the shared
// `directRule`-folding engine (defined alongside `declaratorDeclaredType` below)
// is consumed early by `resolveTypeNodeImpl`'s type-name path to fold an abstract
// fn-ptr/array declarator (`(*)(void)`/`[N]`) onto a cast/sizeof base type.
[[nodiscard]] TypeId
directDeclaredType(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                   NodeId direct, TypeId base, ScopeId scope, bool emitOnMiss,
                   bool allowFlexibleArray,
                   bool allowInitInferredArray = false,
                   bool paramDecay = false);

// c35 D-CSUBSET-FORWARD-STRUCT-DECLARATION: forward declaration — the
// tag-namespace-scope floater (defined below) is consumed early by
// `resolveTypeNodeImpl`'s isTagReference arm to bind an opaque forward-minted
// composite tag into the nearest enclosing namespace scope (C11 6.2.1).
[[nodiscard]] ScopeId
floatToNamespaceScope(EngineState const& s, SemanticConfig const& cfg,
                      Tree const& tree, ScopeId scope);

// D-CSUBSET-BITINT: `resolveTypeNodeImpl` (below) folds a `_BitInt(N)` width
// through `constIntExpr`, which is DEFINED later in this TU — forward-declare it
// here (no default args; the definition supplies them). Same const-expr evaluator
// the array-dimension / alignas / static_assert consumers use.
[[nodiscard]] std::optional<std::int64_t>
constIntExpr(EngineState& s, Tree const& tree, NodeId node, ScopeId fromScope,
             SemanticConfig const* cfg);

// VLA C1a (D-CSUBSET-VLA): the two array-suffix arms (`applyArraySuffix`,
// `applyDeclaratorSuffix` — both DEFINED above the definition below) gate the VLA
// accept on BLOCK scope via `fileScopeOf` (the agnostic scope-chain walk the
// thread_local validator uses). Forward-declare it here so those arms can call it.
[[nodiscard]] ScopeId fileScopeOf(EngineState const& s, Tree const& tree,
                                  ScopeId scope);

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
        // C23 6.2.5/6.7.2 (D-CSUBSET-BITINT): `_BitInt(N)` — a bit-precise integer
        // type-specifier. Handled BEFORE the multiset arm (a run containing a
        // `bitIntSpecifier` node is NOT all-tokens, so that arm skips it). Two
        // shapes: `node` IS the bitIntSpecifier (a bare `_BitInt(N)` reached
        // directly), OR `node` is a `typeSpecifierSeq` run holding the
        // bitIntSpecifier NODE plus sibling `unsigned`/`signed` TOKENS (order-
        // independent; DEFAULT signed). The arm resolves the width const-expr and
        // fail-loud-gates it. Fires only for a language declaring `semantics.bitInt`
        // (never a keyword identity).
        if (cfg.bitIntSpecRule.valid()) {
            NodeId bitSpec{};
            bool sawUnsigned = false;
            bool sawSigned   = false;
            bool sawOther    = false;   // any specifier other than signedness → invalid
            if (rule.v == cfg.bitIntSpecRule.v) {
                bitSpec = node;   // bare `_BitInt(N)` — default signed, no siblings
            } else {
                for (NodeId c : visibleChildren(tree, node)) {
                    if (tree.kind(c) == NodeKind::Internal
                        && tree.rule(c).v == cfg.bitIntSpecRule.v) {
                        if (bitSpec.valid()) sawOther = true;   // two `_BitInt` → invalid
                        else bitSpec = c;
                    } else if (tree.kind(c) == NodeKind::Token) {
                        auto const ck = tree.tokenKind(c);
                        if (cfg.bitIntUnsignedToken && ck == *cfg.bitIntUnsignedToken)
                            sawUnsigned = true;
                        else if (cfg.bitIntSignedToken && ck == *cfg.bitIntSignedToken)
                            sawSigned = true;
                        else
                            sawOther = true;   // int/long/short/… mixed with `_BitInt`
                    } else {
                        sawOther = true;
                    }
                }
            }
            if (bitSpec.valid()) {
                // `unsigned _BitInt` → unsigned; `signed _BitInt` / bare → signed.
                bool const isSigned = !sawUnsigned;
                if (sawUnsigned && sawSigned) sawOther = true;   // contradictory pair
                if (sawOther) {
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::S_InvalidTypeSpecifierCombination;
                    d.severity = DiagnosticSeverity::Error;
                    d.buffer   = tree.source().id();
                    d.span     = tree.span(node);
                    d.actual   = std::string{tree.text(node)};
                    s.reporter.report(std::move(d));
                    specifierDiagnosed = true;
                    return InvalidType;
                }
                auto bitKids = visibleChildren(tree, bitSpec);
                if (cfg.bitIntWidthChild >= bitKids.size()) {
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::S_UnknownType;
                    d.severity = DiagnosticSeverity::Error;
                    d.buffer   = tree.source().id();
                    d.span     = tree.span(bitSpec);
                    d.actual   = std::string{tree.text(bitSpec)};
                    s.reporter.report(std::move(d));
                    specifierDiagnosed = true;
                    return InvalidType;
                }
                // The width is a shared-CST const-expr (the array-dim / alignas /
                // static_assert evaluator). A width diagnostic is UNSUPPRESSABLE and
                // is emitted regardless of `emitOnMiss` (a global's head resolves
                // that way) so the fail-loud is never silently dropped. Because
                // unsuppressable codes BYPASS the reporter dedup window, a re-typing
                // caller that resolves this type-name in Pass 1.5 AND Pass 2 (cast /
                // compound-literal / `_Generic` association) must roll its Pass-1.5
                // resolve back or this fires twice — see those chokepoints.
                auto const emitWidth = [&](DiagnosticCode code, std::string msg) {
                    ParseDiagnostic d;
                    d.code     = code;
                    d.severity = DiagnosticSeverity::Error;
                    d.buffer   = tree.source().id();
                    d.span     = tree.span(bitSpec);
                    d.actual   = std::move(msg);
                    s.reporter.report(std::move(d));
                    specifierDiagnosed = true;
                };
                auto const width =
                    constIntExpr(s, tree, bitKids[cfg.bitIntWidthChild], scope, &cfg);
                if (!width.has_value()) {
                    emitWidth(DiagnosticCode::S_BitIntWidthNotConstant,
                              "`_BitInt(N)` requires an integer constant-expression "
                              "width: " + std::string{tree.text(bitSpec)});
                    return InvalidType;
                }
                std::int64_t const n = *width;
                if (n <= 0) {
                    emitWidth(DiagnosticCode::S_BitIntWidthNotPositive,
                              std::format("`_BitInt` width must be positive (got {})",
                                          n));
                    return InvalidType;
                }
                if (isSigned && n < 2) {
                    emitWidth(DiagnosticCode::S_BitIntSignedWidthTooSmall,
                              "a signed `_BitInt` needs at least 2 bits (1 sign + 1 "
                              "value bit); use `unsigned _BitInt(1)` for a 1-bit type");
                    return InvalidType;
                }
                // __BITINT_MAXWIDTH__ (C23 6.2.5) — the same 8388608 the predefined
                // macro carries; the two encode ONE ABI constant.
                constexpr std::int64_t kBitIntMaxWidth = 8388608;
                if (n > kBitIntMaxWidth) {
                    emitWidth(DiagnosticCode::S_BitIntWidthExceedsMax,
                              std::format("`_BitInt` width {} exceeds "
                                          "__BITINT_MAXWIDTH__ ({})",
                                          n, kBitIntMaxWidth));
                    return InvalidType;
                }
                // D-CSUBSET-BITINT-C2-WIDE: N>64 is now a runnable MULTI-LIMB type
                // (ceil(N/64) i64 limbs) — the C1 `S_BitIntWidthAboveC1Limit` cycle
                // gate is RETIRED here. `bitInt(n, ...)` mints any width; the wrap /
                // storage / ABI / ops for N>64 live in the MIR-lowering tier. As of C3
                // (D-CSUBSET-BITINT-C3-MULDIV) `* / %` also lower (multi-limb multiply
                // + binary long division); the remaining wide gaps are float<->wide
                // conversion and wide literals, which fail loud at their own sites.
                TypeId const result = s.lattice.interner().bitInt(n, isSigned);
                s.nodeToType.set(node, result);
                if (bitSpec.v != node.v) s.nodeToType.set(bitSpec, result);
                return result;
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
                // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): a VALID multiset whose
                // row the active format leaves UNREALIZED (no longDoubleFormat
                // axis) — the precise diagnostic, BEFORE the generic
                // invalid-combination miss below (the combination is not
                // invalid; its representation is undeclared).
                if (s.idx().unrealizedLongDoubleSets.contains(key)) {
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::S_LongDoubleFormatUndeclared;
                    d.severity = DiagnosticSeverity::Error;
                    d.buffer   = tree.source().id();
                    d.span     = tree.span(node);
                    d.actual   = std::format(
                        "'{}' is not realized on this object format: the "
                        "format declares no longDoubleFormat axis",
                        tree.text(node));
                    s.reporter.report(std::move(d));
                    specifierDiagnosed = true;   // outer S_UnknownType suppressed
                    return InvalidType;
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
                    // c35 D-CSUBSET-FORWARD-STRUCT-DECLARATION: the tag is NOT
                    // bound — an OPAQUE / forward-declared composite (`struct S *`,
                    // `typedef struct S S;` where S is never defined; the
                    // sqlite3_stmt handle). For a Struct/Union tag reference,
                    // FORWARD-MINT an INCOMPLETE composite NOW and bind it into the
                    // Tag namespace of the nearest enclosing namespace scope (C11
                    // 6.2.1 — a tag belongs to the enclosing block/file scope, never
                    // a transient declarator-dominator), so this reference and every
                    // later reference to the same tag resolve to ONE TypeId (the
                    // Tag binding deduplicates — only the first miss mints). The
                    // result is INCOMPLETE: a `Ptr<incomplete>` is sizeable and
                    // usable (the opaque-handle case), while a VALUE / by-value
                    // member / sizeof of it fails loud through the UNCHANGED
                    // computeLayout incomplete guard (the no-silent-miscompile
                    // backstop — never a guessed/zero size). A same-TU DEFINITION
                    // of the tag needs NO merge step here: Pass 1's whole-tree
                    // PRE-ORDER binds a definition's COMPLETE tag BEFORE any type
                    // reference resolves, so this forward-mint fires ONLY for a
                    // genuinely-never-defined tag — a forward-then-define resolves
                    // every reference to the definition's complete TypeId. Enum is
                    // value-typed (no incomplete representation), so an Enum tag-miss
                    // falls through to the fail-loud arm below — `compositeKind` is
                    // config, never a hardcoded keyword. The decl-site key packs the
                    // reference node so the mint is unique-but-deterministic; the
                    // Tag binding (not this key) is what unifies repeat references.
                    auto const& tagRef = cfg.references[refIt->second];
                    if (tagRef.compositeKind != CompositeKind::Enum) {
                        TypeKind const compKind =
                            tagRef.compositeKind == CompositeKind::Union
                                ? TypeKind::Union
                                : TypeKind::Struct;
                        std::uint64_t const declSiteKey =
                            (static_cast<std::uint64_t>(tree.id().v) << 32)
                            | static_cast<std::uint64_t>(node.v);
                        TypeId const incomplete =
                            s.lattice.interner().forwardComposite(
                                compKind, rn.name, declSiteKey);
                        ScopeId const bindScope =
                            floatToNamespaceScope(s, cfg, tree, scope);
                        SymbolRecord rec;
                        rec.name         = rn.name;
                        rec.scope        = bindScope;
                        rec.declNode     = node;
                        rec.declRuleNode = node;
                        rec.tree         = tree.id();
                        rec.kind         = DeclarationKind::Type;
                        rec.type         = incomplete;
                        SymbolId const newId = s.symbols.mint(rec);
                        s.scopes.bind(bindScope, rn.name, newId,
                                      SymbolNamespace::Tag);
                        return incomplete;
                    }
                }
                // Tag miss: fall through to the generic miss arm (below) so an
                // undeclared `struct Nope` / `enum Nope` fails loud with
                // S_UnknownType. Do NOT descend into the StructKeyword/Identifier
                // children (the Identifier would resolve ORDINARY and cross the
                // namespaces).
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
        // C23 6.7.2.5 (D-CSUBSET-TYPEOF): `typeof(T)` / `typeof(expr)` /
        // `typeof_unqual(...)` — resolve to the OPERAND's type. Handled ATOMICALLY
        // here (after the tag-reference block, BEFORE the generic kids-descent) so
        // the operand's own literal qualifiers are governed by THIS arm — PRESERVED
        // for `typeof`, STRIPPED for `typeof_unqual` — and never re-applied by the
        // descent's coarse base-volatile scan (the operand subtree is opaque to that
        // scan; see subtreeContainsToken's `opaqueRules`). Only VolatileQual is
        // interned, so "strip top-level qualifiers" == stripVolatile. The result is
        // STAMPED on the typeof node so the HIR lowering's resolveStampedTypeBelow
        // recovers the typeof result (not an inner operand's type). Config-driven on
        // `cfg.typeof*Rule`; never a keyword identity.
        {
            bool const isTypeofType =
                cfg.typeofTypeRule.valid() && rule.v == cfg.typeofTypeRule.v;
            bool const isTypeofValue =
                cfg.typeofValueRule.valid() && rule.v == cfg.typeofValueRule.v;
            if (isTypeofType || isTypeofValue) {
                auto tkids = visibleChildren(tree, node);
                if (cfg.typeofOperandChild >= tkids.size()) {
                    if (emitOnMiss && !specifierDiagnosed) {
                        ParseDiagnostic d;
                        d.code     = DiagnosticCode::S_UnknownType;
                        d.severity = DiagnosticSeverity::Error;
                        d.buffer   = tree.source().id();
                        d.span     = tree.span(node);
                        d.actual   = std::string{tree.text(node)};
                        s.reporter.report(std::move(d));
                        specifierDiagnosed = true;
                    }
                    return InvalidType;
                }
                NodeId const operandNode = tkids[cfg.typeofOperandChild];
                // The strip spelling (`typeof_unqual`): child-0 keyword == the
                // configured strip token. Absent token ⇒ never strips.
                bool const strip =
                    cfg.typeofStripQualifiersToken.has_value()
                    && !tkids.empty()
                    && tree.kind(tkids[0]) == NodeKind::Token
                    && tree.tokenKind(tkids[0]) == *cfg.typeofStripQualifiersToken;
                TypeId raw = InvalidType;
                if (isTypeofType) {
                    // TYPE-NAME operand: recurse the SAME type resolver (an
                    // unknown type-name fails loud S_UnknownType for free).
                    raw = resolveTypeNodeImpl(s, cfg, tree, operandNode, scope,
                                              emitOnMiss, specifierDiagnosed);
                } else {
                    // EXPRESSION operand (UNEVALUATED, 6.7.2.5p2). C 6.7.2.5
                    // constraint: a bit-field member has no nameable type. Bounded
                    // transparent descent to the innermost member-access node
                    // (mirrors the nullptr operator-gate walk); resolveMemberAccess
                    // exposes a bit-field field via `bitFieldWidth`.
                    NodeId probe = operandNode;
                    for (int guard = 0; probe.valid() && guard < 64; ++guard) {
                        if (tree.kind(probe) == NodeKind::Internal
                            && s.idx().memberAccessByRule.contains(
                                   tree.rule(probe).v)) {
                            MemberResolution const mr =
                                resolveMemberAccess(s, cfg, tree, probe, scope);
                            if (mr.status == MemberResolution::Status::Ok
                                && mr.fieldSym.valid()
                                && s.symbols.at(mr.fieldSym)
                                       .bitFieldWidth.has_value()) {
                                // UNCONDITIONAL (C 6.7.2.5 constraint): a bit-field
                                // operand is always ill-formed. Emitted even under
                                // emitOnMiss=false (a global's head resolves that
                                // way) so the fail-loud is never silently dropped.
                                // Unsuppressable codes BYPASS the reporter dedup
                                // window, so a Pass-1.5+Pass-2 re-typing caller
                                // (cast / compound-literal / `_Generic` association)
                                // must roll its Pass-1.5 resolve back to avoid a
                                // double emit — see those chokepoints.
                                ParseDiagnostic d;
                                d.code     =
                                    DiagnosticCode::S_TypeofBitfieldOperand;
                                d.severity = DiagnosticSeverity::Error;
                                d.buffer   = tree.source().id();
                                d.span     = tree.span(operandNode);
                                d.actual   =
                                    std::string{tree.text(operandNode)};
                                s.reporter.report(std::move(d));
                                specifierDiagnosed = true;
                                return InvalidType;
                            }
                            break;   // resolved (bit-field or not) — stop descent
                        }
                        // Descend ONE transparent wrapper (a single Internal child);
                        // a real operator/comma (≥2 Internal children) is not a
                        // transparent member operand — stop.
                        NodeId next{};
                        int internals = 0;
                        for (NodeId c : visibleChildren(tree, probe)) {
                            if (tree.kind(c) == NodeKind::Internal) {
                                ++internals; next = c;
                            }
                        }
                        if (internals == 1) { probe = next; continue; }
                        break;
                    }
                    // The operand subtree was already typed by the blanket Pass-2
                    // walk (which emitted any S_Undeclared/S_TypeMismatch); read its
                    // type on-demand — InvalidType cascades, no double-diagnostic.
                    raw = subtreeType(s, tree, operandNode, scope);
                }
                if (!raw.valid()) {
                    // The operand did not resolve. The TYPE form's recursion already
                    // emitted S_UnknownType (and set specifierDiagnosed); the VALUE
                    // form's operand was diagnosed by the blanket Pass-2 typing
                    // (S_Undeclared/S_TypeMismatch). Mark specifierDiagnosed so the
                    // outer generic-descent miss arm does NOT stack a REDUNDANT
                    // S_UnknownType on the typeof node (design: cascade, no double-diag).
                    specifierDiagnosed = true;
                    return InvalidType;
                }
                TypeId const result =
                    strip ? s.lattice.interner().stripVolatile(raw) : raw;
                s.nodeToType.set(node, result);
                return result;
            }
        }
        auto kids = visibleChildren(tree, node);
        // SE-pointers (G5): count `pointerToken` children at THIS node (C
        // declarator stars: `int *p`, `int **p`) and wrap the resolved base type
        // that many times in Ptr. The stars are a flat token run in `typeRef`;
        // the base type comes from the non-star child (typeBase).
        //
        // c27 (D-CSUBSET-VOLATILE-POINTEE): this is the CO-LOCATED arm —
        // `typeRefAllowingStruct` / `castTypeRef` where a head qualifier and the
        // stars are FLAT siblings of THIS node, AND the split-form HEAD subtree
        // (`volatile u32` with the stars living in a sibling declarator the
        // caller folds). Position-aware: a `volatileMarker` appearing in any child
        // BEFORE the first `pointerToken` qualifies the BASE (the eventual
        // innermost pointee, C 6.7.3) → wrap the resolved base in VolatileQual so
        // `volatile u32 *` builds Ptr<VolatileQual(U32)> and the deref/access of it
        // is volatile by TYPE. A volatile AFTER the last star (east `int *
        // volatile`) is the pointer OBJECT's — NOT a base qualifier; the
        // declarator/symbol path threads it (c21 isVolatile), so it is excluded
        // here. With ZERO stars at this node a leading volatile makes the whole
        // base volatile (a bare `volatile int` scalar, or the head of a
        // split-form pointer decl) → VolatileQual(base); the declarator's pointer
        // layers then wrap on top. Config-driven on `cfg.volatileMarker` +
        // `cfg.pointerToken` (no hardcoded keyword). REPLACES the c21 pointee
        // REJECT — volatile pointees now compile (model B → the type system).
        // The pointer-star run at THIS node takes one of TWO shapes, by rule:
        //   • a flat `pointerToken` (StarOp) TOKEN run — `typeRef` / `sizeofType`
        //     (`int **` where the stars are bare `{repeat StarOp}`); and
        //   • c29 (D-CSUBSET-POST-STAR-CAST-QUALIFIER): `castTypeRef`'s stars are
        //     `pointerLayer` CHILD nodes (`StarOp {repeat ptrQualifier}`) so a
        //     POST-star qualifier (`u32 * volatile` / `int * const`) rides INSIDE
        //     the layer. A volatile inside a pointerLayer is the POINTER OBJECT's
        //     east qualifier; a cast yields an RVALUE with NO top-level cv (C
        //     6.5.4), so it is STRIPPED — never folded into the base.
        // The star-run boundary for the pre-stars-volatile scan is therefore the
        // FIRST child that is EITHER a bare star token OR a pointerLayer node. A
        // pointerLayer's subtree is never handed to `subtreeContainsToken` (the
        // break fires first), so a post-star volatile can NEVER set
        // `baseIsVolatile` — only the c27 PRE-stars volatile pointee path
        // (`volatile u32 *`→Ptr<VolatileQual(u32)>) wraps the base, as before.
        // Config-driven on `cfg.pointerToken` + the declarators'
        // `pointerLayerRule` (no hardcoded keyword/rule name).
        bool const haveLayerRule = cfg.declarators.has_value();
        RuleId const layerRule =
            haveLayerRule ? cfg.declarators->pointerLayerRule : RuleId{};
        auto isPointerStar = [&](NodeId child) {
            if (cfg.pointerToken.has_value()
                && tree.kind(child) == NodeKind::Token
                && tree.tokenKind(child) == *cfg.pointerToken) {
                return true;   // typeRef / sizeofType bare `*`
            }
            return haveLayerRule
                && tree.kind(child) == NodeKind::Internal
                && tree.rule(child) == layerRule;   // castTypeRef pointerLayer
        };
        // D-CSUBSET-TYPEOF: a `typeof(...)` child's operand subtree is OPAQUE to
        // this base-volatile scan (the typeof arm resolves the operand's own
        // qualifiers — preserved for typeof, stripped for typeof_unqual — so a
        // literal `volatile` inside the operand must NOT re-qualify the base here).
        // Config-driven; empty/invalid for a language without typeof (no effect).
        std::array<RuleId, 2> const typeofOpaqueRules{cfg.typeofTypeRule,
                                                      cfg.typeofValueRule};
        // D-CSUBSET-ATOMIC (FC17.9(d) 1b): `_Atomic` is scanned in EXACT PARALLEL to
        // `volatile` (the `atomicMarker` semantics token) — a base-position `_Atomic`
        // BEFORE the first star qualifies the base pointee (`_Atomic int *` =>
        // Ptr<atomicQualified(int)>). The scan is position-aware identically (an
        // `_Atomic` after the last star is the pointer object's, threaded by the
        // declarator's pointer-layer loop, so it breaks at the star run just like
        // volatile). Both bits compose in the shared qualifier skin (cycle 1a).
        bool baseIsVolatile = false;
        bool baseIsAtomic   = false;
        if (cfg.volatileMarker.has_value() || cfg.atomicMarker.has_value()) {
            for (auto child : kids) {
                if (isPointerStar(child)) {
                    break;   // reached the star run — a later qualifier is the pointer object's
                }
                if (cfg.volatileMarker.has_value()
                    && subtreeContainsToken(tree, child, *cfg.volatileMarker,
                                            &s.idx().declByRule, typeofOpaqueRules)) {
                    baseIsVolatile = true;
                }
                if (cfg.atomicMarker.has_value()
                    && subtreeContainsToken(tree, child, *cfg.atomicMarker,
                                            &s.idx().declByRule, typeofOpaqueRules)) {
                    baseIsAtomic = true;
                }
            }
        }
        std::uint32_t ptrDepth = 0;
        TypeId inner = InvalidType;
        NodeId absDirect{};   // c26: an abstract-declarator type-name tail
        for (auto child : kids) {
            // Each bare star OR pointerLayer child = ONE pointer level. c29: a
            // pointerLayer's ptrQualifiers (const/volatile/restrict, after the
            // star) are STRIPPED — a cast pointer is a top-level-cv-less rvalue (C
            // 6.5.4), so `(int * const)p` and `(int *)p` both yield Ptr<int>, and
            // `(u32 * volatile)p` builds Ptr<u32> with NO VolatileQual on the
            // pointer. (The declaration path threads an east volatile into
            // VolatileQual(Ptr<...>) via `declaratorDeclaredType`; a cast does NOT.)
            if (isPointerStar(child)) {
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
            // c27 (D-CSUBSET-VOLATILE-POINTEE): a leading volatile qualifies the
            // BASE (innermost pointee) — wrap BEFORE the pointer layers so
            // `volatile u32 **` = Ptr<Ptr<VolatileQual(U32)>> (C 6.7.3). Idempotent
            // in the interner. For zero stars this is the bare `volatile T` (the
            // scalar / split-form head); the caller's declarator adds any pointers.
            if (baseIsVolatile)
                inner = s.lattice.interner().volatileQualified(inner);
            // D-CSUBSET-ATOMIC (FC17.9(d) 1b): wrap the base in the Atomic bit too.
            // `qualified` merges bits, so `_Atomic volatile int` becomes ONE {V,A}
            // skin regardless of which wrap runs first (order-independent).
            if (baseIsAtomic) {
                // D-CSUBSET-ATOMIC-NONLOCKFREE: `_Atomic` is supported this cycle ONLY
                // on a naturally-aligned lock-free SCALAR. On an aggregate or a wide
                // scalar (`isByValueClass`), a copy decomposes to plain field/byte
                // Load/Store AFTER `computeLayout` strips this TRANSPARENT skin — the
                // type-based atomic-access belt then sees only plain types, so it would
                // be a SILENT non-atomic access (C11 7.17.5). FAIL LOUD + do NOT wrap
                // (never let an atomic-qualified non-lock-free type reach codegen); the
                // lock-table / large-atomic path is deferred beyond atomic cycle-1.
                if (isByValueClass(s.lattice.interner(), inner)) {
                    if (emitOnMiss) {
                        ParseDiagnostic d;
                        d.code     = DiagnosticCode::S_AtomicNonLockFree;
                        d.severity = DiagnosticSeverity::Error;
                        d.buffer   = tree.source().id();
                        d.span     = tree.span(node);
                        d.actual   = std::string{tree.text(node)};
                        s.reporter.report(std::move(d));
                    }
                } else {
                    inner = s.lattice.interner().atomicQualified(inner);
                }
            }
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

// FC17 (D-CSUBSET-CONSTEXPR): the FLOAT sibling of `integerLiteralTokenSet` —
// this schema's `literalTypeIds` filtered to float cores. Populated ONLY into
// the float-capable `constExprValue` consumer's context (the constexpr
// initializer check); every integer-required consumer (array dims / enums /
// static_assert / alignas / case labels) never passes it, so a float literal
// stays non-foldable there (`int a[1.5+1.5]` / `_Static_assert(1.5>1.0,"")`
// keep failing loud — the F3 no-leak wall).
[[nodiscard]] std::unordered_set<std::uint32_t>
floatLiteralTokenSet(EngineState const& s) {
    std::unordered_set<std::uint32_t> out;
    auto const isFloatKind = [](TypeKind k) noexcept {
        return k == TypeKind::F16 || k == TypeKind::F32
            || k == TypeKind::F64 || k == TypeKind::F80 || k == TypeKind::F128;
    };
    for (auto const& [tok, ty] : s.idx().literalTypeIds) {
        if (isFloatKind(s.lattice.interner().kind(ty))) out.insert(tok);
    }
    return out;
}

// FC17 F2 (D-CSUBSET-CONSTEXPR / the pre-existing `_Static_assert(true)` gap):
// token → ready-made literal for this language's FIXED-VALUE keyword literals —
// the `literalTypes` rows carrying `value:` (C23 `true` → Bool 1 / `false` →
// Bool 0), mirroring the CST→HIR tier's `litFixed_` build (grammar-declared
// value, NEVER a text decode) with the SAME signedness discipline
// (`unsignedIntRank`-keyed arm selection, the `constantLiteralForSymbol`
// precedent). Filtered to INTEGER-VALUED cores (bool/char/integer kinds) so a
// NullptrT-cored row (`nullptr`, value 0) is structurally EXCLUDED — `nullptr`
// is a null pointer constant, NOT an integer constant expression, and must not
// fold in `int a[...]` / `_Static_assert(...)` position. Reads the schema's
// semantics directly (always present on the tree), so consumers with a null
// `cfg` still fold keyword literals.
[[nodiscard]] std::unordered_map<std::uint32_t, HirLiteralValue>
fixedValueTokenMap(Tree const& tree) {
    std::unordered_map<std::uint32_t, HirLiteralValue> out;
    for (auto const& lt : tree.schema().semantics().literalTypes) {
        if (!lt.fixedValue.has_value()) continue;
        bool const integerValued = lt.core == TypeKind::Bool
                                || lt.core == TypeKind::Char
                                || isIntegerKind(lt.core);
        if (!integerValued) continue;
        HirLiteralValue v;
        v.core = lt.core;
        if (detail::type_rules::unsignedIntRank(lt.core) == 0) {
            v.value = *lt.fixedValue;
        } else {
            v.value = static_cast<std::uint64_t>(*lt.fixedValue);
        }
        out.emplace(lt.literal.v, std::move(v));
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
    CstResolvedSymbol out{*initExpr, rec.scope.v, std::nullopt};
    // C4b (I1): a const `_BitInt(N)` symbol's value is its initializer CONVERTED
    // to `_BitInt(N)` (C 6.7.9 / 6.3.1.3). Hand the engine the declared (width,
    // signed) so it applies the mod-2^N wrap — else `const _BitInt(4) k = 20wb;
    // k` folds to 20 (the initializer's `_BitInt(6)` value) instead of 4.
    auto const& in = s.lattice.interner();
    if (rec.type.valid() && in.kind(rec.type) == TypeKind::BitInt) {
        out.declaredBitPrecise = CstBitPreciseDeclType{
            static_cast<std::uint32_t>(in.bitIntWidth(rec.type)),
            in.bitIntIsSigned(rec.type)};
    }
    return out;
}

// FC17 (D-CSUBSET-CONSTEXPR): the ONE resolver-environment builder shared by
// `constIntExpr` (the integer-required consumers) and `constExprValue` (the
// float-capable constexpr-initializer fold) — extracted VERBATIM from
// constIntExpr's body so the two evaluators can never drift on what a symbol /
// sizeof / alignof / cast / member-offset resolves to in const-expr position.
// THE 3 MUST-KEEP INVARIANTS (audit-pinned):
//   (1) the `fromScope.valid() && cfg != nullptr` gate — an invalid scope or a
//       null config yields an EMPTY environment (leaf-only folding), exactly as
//       before;
//   (2) the scope-capture ASYMMETRY — resolveSymbolValue / resolveSymbolInit
//       receive the DYNAMIC current scope (`curScopeOpaque`, threaded by the
//       engine as it recurses into other symbols' init expressions), while
//       resolveSizeof / resolveAlignof / resolveCastTarget capture the STATIC
//       `fromScope` (a sizeof/alignof/cast TYPE resolves in the scope of the
//       const-expr USE SITE, not of a referenced symbol's declaration);
//   (3) the returned closures capture `s` / `tree` by reference and
//       `cfg` / `fromScope` by value — the env is consumed within the caller's
//       frame (both callers build it, run `evaluateConstantCst`, and return).
[[nodiscard]] CstEvalEnvironment
buildConstEvalEnv(EngineState& s, Tree const& tree,
                  ScopeId fromScope, SemanticConfig const* cfg) {
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
        // C11/C23 6.5.3.4: fold `_Alignof(T)` in a const-expr position (an array
        // dimension `int a[_Alignof(T)]`, `_Static_assert(_Alignof(T)==N,...)`).
        // An ADDITIVE mirror of `resolveSizeof` reading ALIGNMENT instead of size.
        // The engine dispatches the `alignofRule` node here (ahead of its wrapper-
        // peel); this closure resolves the queried type (the castTypeRef child)
        // and reads its alignment through the same `computeLayout` engine MIR
        // uses. Type-name form ONLY (no value form). `nullopt` when un-alignable
        // or the target declared no layout params → the caller fails loud, never a
        // guessed alignment.
        env.resolveAlignof = [&s, &tree, cfg, fromScope](NodeId alignofNode)
            -> std::optional<std::uint64_t> {
            if (!s.aggregateLayout.has_value()) return std::nullopt;
            TypeId queried{};
            if (cfg->alignofTypeRule.valid()
                && tree.rule(alignofNode).v == cfg->alignofTypeRule.v) {
                auto ak = visibleChildren(tree, alignofNode);
                if (cfg->alignofTypeChild < ak.size())
                    // emitOnMiss=false: an unknown queried type yields nullopt →
                    // the caller fails loud with ONE positioned diagnostic (it
                    // owns it), not a redundant S_UnknownType pair.
                    queried = resolveTypeNode(s, *cfg, tree,
                                              ak[cfg->alignofTypeChild], fromScope,
                                              /*emitOnMiss=*/false);
            }
            if (!queried.valid()) return std::nullopt;
            auto const layout = computeLayout(queried, s.lattice.interner(),
                                              *s.aggregateLayout, s.dataModel);
            if (!layout) return std::nullopt;
            return layout->align.bytes();
        };
        // c43 (D-CSUBSET-ADDRESS-CONSTANT-FOLD / Option A): classify a cast's
        // TARGET type for the offsetof spine — `(T*)0` (pointer), `(char*)x`
        // (pointer, retype), `(size_t)int` (integer width/signedness). The engine
        // is interner-free, so this closure (which owns the interner) hands back a
        // CstCastTarget descriptor. The cast's type-ref is the first Internal child
        // (`( typeRef ) operand`); resolveTypeNode (c26 handles abstract cast
        // declarators) folds it. emitOnMiss=false: an unresolved cast type ⇒
        // nullopt ⇒ the fold fails loud (one positioned diagnostic from the caller).
        env.resolveCastTarget = [&s, &tree, cfg, fromScope](NodeId castNode)
            -> std::optional<CstCastTarget> {
            NodeId typeRefN{};
            for (NodeId c : visibleChildren(tree, castNode)) {
                if (tree.kind(c) == NodeKind::Internal) { typeRefN = c; break; }
            }
            if (!typeRefN.valid()) return std::nullopt;
            TypeId const ty = resolveTypeNode(s, *cfg, tree, typeRefN, fromScope,
                                              /*emitOnMiss=*/false);
            if (!ty.valid()) return std::nullopt;
            TypeInterner const& in = s.lattice.interner();
            CstCastTarget t;
            TypeKind const k = in.kind(ty);
            if (k == TypeKind::Ptr) {
                t.isPointer = true;
                auto const ops = in.operands(ty);
                if (!ops.empty()) t.pointeeType = ops[0];
                return t;
            }
            TypeKind ik = k;   // an enum casts as its underlying integer
            if (k == TypeKind::Enum) {
                auto const sc = in.scalars(ty);
                if (!sc.empty()) ik = static_cast<TypeKind>(sc[0]);
            }
            switch (ik) {
                case TypeKind::Bool: t.isInteger=true; t.intBits=1;  t.intSigned=false; break;
                case TypeKind::Char:
                case TypeKind::I8:   t.isInteger=true; t.intBits=8;  t.intSigned=true;  break;
                case TypeKind::U8:   t.isInteger=true; t.intBits=8;  t.intSigned=false; break;
                case TypeKind::I16:  t.isInteger=true; t.intBits=16; t.intSigned=true;  break;
                case TypeKind::U16:  t.isInteger=true; t.intBits=16; t.intSigned=false; break;
                case TypeKind::I32:  t.isInteger=true; t.intBits=32; t.intSigned=true;  break;
                case TypeKind::U32:  t.isInteger=true; t.intBits=32; t.intSigned=false; break;
                case TypeKind::I64:  t.isInteger=true; t.intBits=64; t.intSigned=true;  break;
                case TypeKind::U64:  t.isInteger=true; t.intBits=64; t.intSigned=false; break;
                // C23 6.3.1.3 (D-CSUBSET-BITINT-CONSTFOLD-LARGE, C4b): a cast TO
                // `_BitInt(N)` folds via the wrap-aware bignum at width N (mod-2^N),
                // for ANY N (narrow AND wide) — `(_BitInt(4))15 + 1 == 0` /
                // `(_BitInt(40))2000000 * ...`. Carried as the bit-precise descriptor
                // (NOT `isInteger`, whose `intBits` maxes at 64 and whose narrow
                // fold cannot express a wide `_BitInt`).
                case TypeKind::BitInt:
                    t.isBitPrecise = true;
                    t.bitWidth = static_cast<std::uint32_t>(in.bitIntWidth(ty));
                    t.bitSigned = in.bitIntIsSigned(ty);
                    return t;
                default: return std::nullopt;   // float / aggregate — non-foldable cast
            }
            return t;
        };
        // c43: resolve a struct/union field's byte offset + type for `&((T*)0)->M`.
        // Looks the field name up in the container's MEMBER SCOPE (Pass-1 binds
        // fields there with their declaration-order `fieldIndex`), guards bit-fields
        // (no byte offset — taking their address is illegal C), and reads the offset
        // from the SAME computeLayout the sizeof / codegen paths use (per-target via
        // aggregateLayout — no target branch here). nullopt ⇒ the member fold fails
        // loud (unknown field / not-yet-completed composite / no layout params).
        env.resolveFieldOffset = [&s, &tree](TypeId container, NodeId fieldTok)
            -> std::optional<CstFieldResolution> {
            if (!s.aggregateLayout.has_value()) return std::nullopt;
            TypeInterner const& in = s.lattice.interner();
            TypeKind const ck = in.kind(container);
            if (ck != TypeKind::Struct && ck != TypeKind::Union) return std::nullopt;
            auto const scopeIt = s.compositeScopeByType.find(container.v);
            if (scopeIt == s.compositeScopeByType.end()) return std::nullopt;
            SymbolId const fsym = s.scopes.lookup(scopeIt->second, tree.text(fieldTok));
            if (!fsym.valid()) return std::nullopt;
            SymbolRecord const& frec = s.symbols.at(fsym);
            std::uint32_t const idx = frec.fieldIndex;
            if (in.fieldBitWidth(container, idx).has_value()) return std::nullopt;  // bit-field
            auto const layout = computeLayout(container, in, *s.aggregateLayout, s.dataModel);
            if (!layout || idx >= layout->fieldOffsets.size()) return std::nullopt;
            return CstFieldResolution{layout->fieldOffsets[idx], frec.type};
        };
    }
    return env;
}

[[nodiscard]] std::optional<std::int64_t>
constIntExpr(EngineState& s, Tree const& tree, NodeId node,
             ScopeId fromScope = {}, SemanticConfig const* cfg = nullptr) {
    if (!node.valid()) return std::nullopt;
    auto intLits = integerLiteralTokenSet(s);
    // FC17 F2: the fixed-value keyword literals (`true`/`false`) fold in EVERY
    // integer const-expr context — they are integer constant expressions per
    // C23 6.6 (closes the pre-existing `_Static_assert(true)` gap alongside the
    // shared evaluator's narrow-char arm). The map excludes NullptrT rows by
    // construction. NO float set here — integer-required consumers keep floats
    // non-foldable (the F3 wall).
    auto fixedVals = fixedValueTokenMap(tree);
    CstEvalContext ctx{tree, tree.schema(), intLits, s.idx().numberStyle};
    ctx.fixedValueTokens = &fixedVals;
    // C4b (Fork-2c): supply the `integerLiteralTyping` rules so a `wb`/`uwb`
    // bit-precise literal folds to a BitIntValue leaf (`_Static_assert(15wb+1==16)`,
    // an ICE `_BitInt` array dim). Absent cfg ⇒ empty ⇒ off (unchanged behavior).
    if (cfg != nullptr) ctx.integerLiteralTyping = cfg->integerLiteralTyping;
    ctx.dataModel = s.dataModel;
    CstEvalEnvironment env = buildConstEvalEnv(s, tree, fromScope, cfg);
    ConstEvalResult const r = evaluateConstantCst(node, ctx, env, {}, fromScope.v);
    if (!r.value.has_value()) return std::nullopt;
    return asInt64Bridge(*r.value);
}

// FC17 (D-CSUBSET-CONSTEXPR): the FLOAT-CAPABLE full-value sibling of
// `constIntExpr` — folds a constexpr object's initializer to its complete
// compile-time `HirLiteralValue` (int / uint / bool / float arm), or nullopt
// when it is not a compile-time constant. Differences from constIntExpr,
// each deliberate:
//   * `floatLiteralTokens` is populated (float literals fold at the leaf) and
//     `EvalOptions.allowFloat` is on (float arithmetic folds — the SAME CE5
//     engine walls the HIR-side evaluator uses), so `constexpr double PI2 =
//     3.5 * 2;` validates — while every integer-required consumer keeps both
//     off (the F3 no-leak walls);
//   * returns the full `HirLiteralValue` (no asInt64Bridge) — the constexpr
//     check needs "is it a compile-time constant", not an int64;
//   * shares `buildConstEvalEnv` (the 6 resolvers) VERBATIM with constIntExpr,
//     so `constexpr int N = M + 1;` resolves M exactly as `int a[M + 1]` does.
[[nodiscard]] std::optional<HirLiteralValue>
constExprValue(EngineState& s, Tree const& tree, NodeId node,
               ScopeId fromScope, SemanticConfig const* cfg) {
    if (!node.valid()) return std::nullopt;
    auto intLits   = integerLiteralTokenSet(s);
    auto floatLits = floatLiteralTokenSet(s);
    auto fixedVals = fixedValueTokenMap(tree);
    CstEvalContext ctx{tree, tree.schema(), intLits, s.idx().numberStyle};
    ctx.floatLiteralTokens = &floatLits;
    ctx.fixedValueTokens   = &fixedVals;
    // C4b (Fork-2c): a `wb`/`uwb` bit-precise constexpr initializer folds to a
    // BitIntValue (`constexpr _BitInt(8) k = 15wb;`).
    if (cfg != nullptr) ctx.integerLiteralTyping = cfg->integerLiteralTyping;
    ctx.dataModel = s.dataModel;
    // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): float-literal ladder + axis so a
    // `20.0L` leaf carries its TRUE core (F80/F128 on a walled axis) and the
    // hostBacked fold gate refuses to fold it at binary64 — this is the ONLY
    // ctx that admits float leaves, so it is the only one that threads them.
    if (cfg != nullptr) ctx.floatLiteralTyping = cfg->floatLiteralTyping;
    ctx.longDoubleFormat = s.longDoubleFormat;
    CstEvalEnvironment env = buildConstEvalEnv(s, tree, fromScope, cfg);
    EvalOptions options;
    options.allowFloat = true;
    ConstEvalResult const r =
        evaluateConstantCst(node, ctx, env, options, fromScope.v);
    if (!r.value.has_value()) return std::nullopt;
    return r.value;
}

// FC17 (D-CSUBSET-CONSTEXPR): enforce the C23 6.7.1 constexpr-object
// constraints for ONE declarator whose symbol was Pass-1-marked `isConstexpr`.
// Runs from pass2Post's per-declarator loop — Pass 2, NOT Pass 1.5, because a
// bare `nullptr` initializer's type is only stamped by Pass 2's literal typing
// (the NullptrT admission below reads it). THE EMPIRICAL DELTA vs `const`:
// `const int x = argc;` compiles clean (const-ness is initializer-blind; the
// fold is lazy — only an ICE consumer errors), while `constexpr int x = argc;`
// must fail AT ITS OWN DECLARATION — this function IS that check. Every arm
// fails loud; a constexpr declaration NEVER silently degrades to plain const.
//   * unresolved declared type   → return (cascade — already diagnosed);
//   * FnSig / Function symbol    → S_ConstexprFunctionNotSupported (BOTH the
//     proto and the definition form — C23 constexpr is objects-only, and the
//     file-scope linkage row must never silently apply to a function);
//   * volatile-qualified OBJECT  → S_ConstexprInvalidQualifier (6.7.1p11;
//     checked BEFORE the kind classification because `kind()` sees through
//     VolatileQual. A volatile POINTEE — `constexpr volatile int *p` — is
//     Ptr at the top level and correctly stays legal; an east
//     `int * volatile p` IS a volatile object and is rejected);
//   * Array / Struct / Union     → S_ConstexprUnsupportedType — the NAMED loud
//     aggregate deferral (D-CSUBSET-CONSTEXPR-AGGREGATE-TYPE; a UNIFORM
//     boundary — `constexpr char s[] = "hi"` is deliberately NOT carved out);
//   * missing initializer        → S_ConstexprMissingInitializer, fired PER
//     DECLARATOR (`constexpr int a = 1, b;` errors on `b`);
//   * Ptr                        → the initializer must be a null pointer
//     constant: the shared `admitsNullPointerConstant` (structural `0` + the
//     R2 folded integer-0 path, which also MARKS the node for the HIR
//     literal-0 materialization) OR a Pass-2-stamped NullptrT (`nullptr` —
//     mirroring the decl-init isAssignable site's admission). Anything else
//     (`&g`; the `(T*)0` cast form = D-CSUBSET-CONSTEXPR-POINTER-CAST-NULL,
//     a named loud deferral) → S_ConstexprNonConstantInitializer;
//   * arithmetic scalar (integer / float / bool / char / Enum — F5: an
//     enumerator initializer folds via the shared resolveSymbolValue arm) →
//     the initializer must fold through `constExprValue` (float-capable,
//     full-value) → else S_ConstexprNonConstantInitializer;
//   * any OTHER kind             → S_ConstexprUnsupportedType (the fail-loud
//     catch-all — never silent).
void validateConstexprDeclarator(EngineState& s, SemanticConfig const& cfg,
                                 Tree const& tree, NodeId dNode,
                                 NodeId nameNode, SymbolId sym, ScopeId here) {
    SymbolRecord const& rec = s.symbols.at(sym);
    TypeId const declTy = rec.type;
    auto const emit = [&](DiagnosticCode code, NodeId at) {
        ParseDiagnostic d;
        d.code     = code;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(at);
        d.actual   = std::string{tree.text(at)};
        s.reporter.report(std::move(d));
    };
    if (!declTy.valid()) return;   // cascade — the type failure already reported
    TypeInterner const& in = s.lattice.interner();
    if (in.kind(declTy) == TypeKind::FnSig
        || rec.kind == DeclarationKind::Function) {
        emit(DiagnosticCode::S_ConstexprFunctionNotSupported, nameNode);
        return;
    }
    if (in.isVolatileQualified(declTy)) {
        emit(DiagnosticCode::S_ConstexprInvalidQualifier, nameNode);
        return;
    }
    TypeKind const k = in.kind(declTy);
    if (k == TypeKind::Array || k == TypeKind::Struct || k == TypeKind::Union) {
        emit(DiagnosticCode::S_ConstexprUnsupportedType, nameNode);
        return;
    }
    // The initializer subtree — the Internal child of the init-declarator that
    // is NOT the declarator (`[declarator, '=', initValue]`; the loop's own
    // discovery pattern). A bare declarator (rule != initDeclaratorRule — the
    // very carriers the F1 hook-hoist exists for) has no init slot at all.
    NodeId initNode{};
    if (cfg.declarators.has_value()
        && tree.rule(dNode) == cfg.declarators->initDeclaratorRule) {
        for (NodeId c : visibleChildren(tree, dNode)) {
            if (tree.kind(c) != NodeKind::Internal) continue;
            if (tree.rule(c) == cfg.declarators->declaratorRule) continue;
            initNode = c;
            break;
        }
    }
    if (!initNode.valid()) {
        emit(DiagnosticCode::S_ConstexprMissingInitializer, nameNode);
        return;
    }
    // FC17.5 F3 (D-CSUBSET-EMPTY-INITIALIZER, C23 6.7.10): an EMPTY brace
    // initializer `constexpr int x = {};` / `constexpr int *p = {};` IS a
    // compile-time constant — it zero-initializes (6.7.10p11), and zero is a
    // valid value for every scalar constexpr object (incl. the pointer arm:
    // zero = the null pointer constant). `constExprValue` cannot fold a brace
    // list (it is not an expression), so without this arm the HIR-side scalar
    // `{}` lift would stay S_ConstexprNonConstantInitializer-rejected under
    // constexpr. ONLY the truly-empty list is admitted here; `{5}` folds via
    // the normal single-child-descent path below, and a malformed multi-element
    // `{1,2}` falls through to the arithmetic/pointer arms' loud rejection
    // (the HIR lowering also rejects it — S_InvalidScalarInitializer). The
    // descent skips TOKEN children (`{`/`}` are the brace list's own tokens)
    // and stops at the first node with 0 or 2+ Internal children, so a
    // compound literal `(int){}` (two Internal children) is NOT admitted — a
    // compound literal is not a C constant expression.
    if (s.idx().braceInitListRule.valid()) {
        NodeId walk = initNode;
        for (int guard = 0; guard < 64 && walk.valid(); ++guard) {
            if (tree.kind(walk) != NodeKind::Internal) { walk = NodeId{}; break; }
            if (tree.rule(walk).v == s.idx().braceInitListRule.v) break;
            NodeId sole{};
            bool   multiple = false;
            for (NodeId c : visibleChildren(tree, walk)) {
                if (tree.kind(c) != NodeKind::Internal) continue;
                if (sole.valid()) { multiple = true; break; }
                sole = c;
            }
            if (multiple || !sole.valid()) { walk = NodeId{}; break; }
            walk = sole;
        }
        if (walk.valid()
            && tree.rule(walk).v == s.idx().braceInitListRule.v) {
            bool anyElement = false;
            for (NodeId c : visibleChildren(tree, walk)) {
                if (tree.kind(c) != NodeKind::Internal) continue;
                if (!s.idx().initElementRule.valid()
                    || tree.rule(c).v == s.idx().initElementRule.v) {
                    anyElement = true;
                    break;
                }
            }
            if (!anyElement) return;   // `= {}` — zero, a valid constexpr value
        }
    }
    if (k == TypeKind::Ptr) {
        if (admitsNullPointerConstant(s, tree, declTy, initNode,
                                      tree.schema().semantics()
                                          .pointerConversions,
                                      here, cfg)) {
            return;
        }
        TypeId const initTy = subtreeType(s, tree, initNode, here);
        if (initTy.valid() && in.kind(initTy) == TypeKind::NullptrT) return;
        emit(DiagnosticCode::S_ConstexprNonConstantInitializer, initNode);
        return;
    }
    bool const arithmetic =
        k == TypeKind::Bool || k == TypeKind::Char || k == TypeKind::Byte
        || isIntegerKind(k) || k == TypeKind::Enum
        || k == TypeKind::F16 || k == TypeKind::F32 || k == TypeKind::F64
        || k == TypeKind::F80 || k == TypeKind::F128;
    if (arithmetic) {
        if (constExprValue(s, tree, initNode, here, &cfg).has_value()) return;
        emit(DiagnosticCode::S_ConstexprNonConstantInitializer, initNode);
        return;
    }
    emit(DiagnosticCode::S_ConstexprUnsupportedType, nameNode);
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

    // VLA C4c (D-CSUBSET-VLA, C99 §6.7.6.2/6.7.6.3): the array-parameter
    // decorations `static` / cv-qualifiers / `*` are legal ONLY in a
    // function-parameter declarator. This legacy suffix facet is externDecl's,
    // and an extern object declaration is NEVER a parameter — so ANY decoration
    // is a constraint violation. Reject BEFORE the fixed-index `lengthChild`
    // lookup below: with a leading decoration the bound is no longer at
    // `lengthChild`, so a widened `extern int arr[static 5];` would otherwise
    // SILENTLY drop the `static 5` → a bogus incompleteArray. (The declarator-
    // mode twin gates the same construct in applyDeclaratorSuffix via paramDecay.)
    if (cfg != nullptr && cfg->declarators.has_value()
        && arraySuffixHasModifier(tree, suffix,
                                  cfg->declarators->arraySuffixModifierTokens)) {
        emit(DiagnosticCode::S_ArrayParamQualifierNonParameter);
        return InvalidType;
    }

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
    // c82: `arrayToPointer` (C 6.7.6.3p7 parameter adjustment) admits the
    // absent length through the SAME incomplete-array path — the caller's
    // `adjustArrayToPointer` rewrites the result to Ptr<element>, so the
    // incomplete array never escapes as a param's final type.
    if (absentLength && (decl.allowFlexibleArray || decl.arrayToPointer)) {
        return s.lattice.interner().incompleteArray(base);
    }
    auto len = constIntExpr(s, tree, lenNode, fromScope, cfg);
    if (!len.has_value()) {
        // VLA C1a (D-CSUBSET-VLA): the legacy-facet twin of the declarator-mode VLA
        // arm — a PRESENT-but-non-constant length on a NON-FAM, NON-param declarator
        // is a variable-length array. Build the vlaArray here; the block-vs-file +
        // automatic-storage constraints are enforced by the Pass-2
        // `validateVlaDeclarator` (reading the symbol's binding scope), NOT by the
        // type-construction `fromScope` (a descendant of the file scope even for a
        // file-scope decl). Params (`arrayToPointer`) + struct fields
        // (`allowFlexibleArray`) already took their absent-length paths above.
        // `visibleChildren(suffix).size() > 2` mirrors the twin's `hasPresentLength`:
        // a PRESENT length (`[ expr ]`, 3 children) is a VLA; an ABSENT one (`[]`, 2)
        // stays S_NonConstantArrayLength (defense-in-depth — the only legacy array-
        // suffix row today is `externDecl`/allowFlexibleArray, skipped above).
        if (!decl.allowFlexibleArray && !decl.arrayToPointer
            && visibleChildren(tree, suffix).size() > 2) {
            TypeInterner& in = s.lattice.interner();
            // VLA C3 (D-CSUBSET-VLA): a VLA whose element is itself an array or a VLA
            // (`int a[n][m]`, `int a[n][5]`) is a MULTI-DIMENSIONAL VLA — build the
            // nested `vlaArray(element)`; HIR→MIR sizes the runtime row stride. The
            // right-to-left suffix fold already produced `base` as the inner type.
            return in.vlaArray(base);
        }
        emit(DiagnosticCode::S_NonConstantArrayLength);
        return InvalidType;
    }
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
    // VLA C3 (D-CSUBSET-VLA): a CONSTANT outer bound over a VLA element (`int a[5][n]`
    // — the `[n]` folded first into a vlaArray, now `[5]` folds over it) is a fixed-
    // outer multi-dimensional VLA. Build `array(vlaArray, N)`; the transitive
    // `typeContainsVla` routes it to the runtime alloca/stride paths downstream. The
    // FAM guard above still rejects an incomplete (-1) element; only the -2 VLA
    // element flows through here.
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
    // D-CSUBSET-BITINT-BITFIELD (C23 6.7.2.1): a `_BitInt(N)` bit-field's base
    // bit-size is N (reprType is the BitInt itself — enumUnderlyingOrSelf passes a
    // non-enum through). The >64 cap below then excludes wide (N>64) BitInt
    // bit-fields for free (same as I128/U128 — no >64 allocation-unit codegen), and
    // the `M <= typeBits` check enforces M <= N with no extra code.
    std::uint32_t const typeBits =
        (k == TypeKind::BitInt)
            ? static_cast<std::uint32_t>(s.lattice.interner().bitIntWidth(reprType))
            : intBits(k);
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

// C11/C23 6.7.5 (D-CSUBSET-ALIGNAS): the FIRST `alignasSpec` node in a
// declaration's specifier prefix, or InvalidNode when the declaration carries no
// alignas (or the language declares no alignas surface / prefix rule). Used for
// PRESENCE detection at a call site that owns the CONTEXT decision (a typedef /
// function / parameter alignas is a constraint violation the caller emits). The
// prefix subtree is reached via `specifierPrefixChild` — the same accessor the
// linkage scan uses — so this sees exactly the STRIPPED specifier prefix
// (structMemberDeclSpecifiers / declSpecifiers / localDeclSpecifiers).
[[nodiscard]] NodeId
firstAlignasSpecInPrefix(EngineState& s, SemanticConfig const& cfg,
                         Tree const& tree, NodeId declNode,
                         DeclarationRule const& decl) {
    if (!cfg.alignasSpecRule.valid()) return {};
    NodeId const prefix = specifierPrefixChild(tree, declNode, decl);
    if (!prefix.valid()) return {};
    // Bounded descendant search for the specifier rule (mirror
    // resolveBitfieldSuffix's suffix search); the prefix subtree is tiny.
    std::vector<NodeId> stack{prefix};
    for (int guard = 0; guard < 8192 && !stack.empty(); ++guard) {
        NodeId c = stack.back(); stack.pop_back();
        if (tree.kind(c) != NodeKind::Internal) continue;
        if (tree.rule(c).v == cfg.alignasSpecRule.v) return c;
        for (NodeId g : visibleChildren(tree, c)) stack.push_back(g);
    }
    return {};
}

// FC16 (D-CSUBSET-NORETURN): true iff a declaration's specifier prefix names the
// `noreturn` function attribute — in EITHER the `_Noreturn` KEYWORD form (a token
// of `cfg.noreturnKeywordToken`, C11 6.7.4) OR an ATTRIBUTE form (`[[noreturn]]` /
// `__attribute__((noreturn))` / `__attribute__((__noreturn__))` / `[[gnu::noreturn]]`,
// C23 6.7.12.7 / GNU), matched by an attribute IDENTIFIER leaf (dunder-normalized
// via the shared `stripDunder`, so `__noreturn__` ≡ `noreturn` and `[[gnu::noreturn]]`'s
// final segment matches) against `cfg.noreturnAttributeNames`. A bounded descendant
// search over the STRIPPED specifier prefix (the same `specifierPrefixChild` accessor
// the linkage + alignas scans use).
//
// CRITICAL: it matches BOTH forms — the `scanCompositePacked` precedent matches
// IDENTIFIERS only, which would MISS a bare `_Noreturn` KEYWORD (a distinct token
// kind, not an identifier). Emits NOTHING: detection that drops a flag is a SAFE
// miss (a spurious H_VerifierFailure downstream is fail-loud), never a silent
// miscompile — so no diagnostic surface is needed here.
[[nodiscard]] bool
specifierPrefixNamesNoreturn(SemanticConfig const& cfg, Tree const& tree,
                             NodeId declNode, DeclarationRule const& decl) {
    bool const haveKeyword = cfg.noreturnKeywordToken.has_value()
                          && cfg.noreturnKeywordToken->valid();
    if (!haveKeyword && cfg.noreturnAttributeNames.empty()) return false;
    NodeId const prefix = specifierPrefixChild(tree, declNode, decl);
    if (!prefix.valid()) return false;
    std::vector<NodeId> stack{prefix};
    for (int guard = 0; guard < 8192 && !stack.empty(); ++guard) {
        NodeId c = stack.back(); stack.pop_back();
        if (tree.kind(c) == NodeKind::Internal) {
            for (NodeId g : visibleChildren(tree, c)) stack.push_back(g);
            continue;
        }
        // (a) the `_Noreturn` KEYWORD token.
        if (haveKeyword && tree.tokenKind(c).v == cfg.noreturnKeywordToken->v)
            return true;
        // (b) an attribute IDENTIFIER naming `noreturn` (dunder-normalized).
        if (cfg.identifierToken.valid()
            && tree.tokenKind(c) == cfg.identifierToken) {
            std::string_view const id = stripDunder(tree.text(c));
            for (std::string const& nm : cfg.noreturnAttributeNames)
                if (id == nm) return true;
        }
    }
    return false;
}

// FC17 (D-CSUBSET-CONSTEXPR): true iff a declaration's specifier prefix carries
// the C23 6.7.1 `constexpr` KEYWORD (a token of `cfg.constexprKeywordToken`).
// The `specifierPrefixNamesNoreturn` mirror, KEYWORD-form only (constexpr has
// no attribute spelling): a bounded descendant search over the STRIPPED
// specifier prefix (the same `specifierPrefixChild` accessor the linkage /
// alignas / noreturn scans use), matching by token-kind equality. No
// typeof/alignas operand-opacity hazard — the keyword cannot parse inside
// those operands (it is a declaration specifier, not an expression or
// type-name token), so a prefix hit is always a REAL constexpr declaration.
// Emits NOTHING: Pass 2's validateConstexprDeclarator owns every diagnostic.
[[nodiscard]] bool
specifierPrefixHasConstexpr(SemanticConfig const& cfg, Tree const& tree,
                            NodeId declNode, DeclarationRule const& decl) {
    if (!cfg.constexprKeywordToken.has_value()
        || !cfg.constexprKeywordToken->valid()) {
        return false;
    }
    NodeId const prefix = specifierPrefixChild(tree, declNode, decl);
    if (!prefix.valid()) return false;
    std::vector<NodeId> stack{prefix};
    for (int guard = 0; guard < 8192 && !stack.empty(); ++guard) {
        NodeId c = stack.back(); stack.pop_back();
        if (tree.kind(c) == NodeKind::Internal) {
            for (NodeId g : visibleChildren(tree, c)) stack.push_back(g);
            continue;
        }
        if (tree.tokenKind(c).v == cfg.constexprKeywordToken->v) return true;
    }
    return false;
}

// TLS C1 (D-CSUBSET-THREAD-LOCAL): the storage-duration facts folded from ONE
// declaration's specifier prefix. Keyed on the SAME per-row `linkageSpecifiers`
// facet CST→HIR's `linkageFrom` folds (token SOURCE TEXT → effect), so the
// semantic mark and the HIR linkage fold can never disagree on WHICH tokens
// confer thread/static storage — one config facet, two consumers. Skips
// `linkageSpecifierIgnoredRules` subtrees wholesale (the linkageFrom skip
// mirrored: a `[[...]]`/alignas identifier that happens to spell a specifier
// text must not mark the record when the linkage scan would not fold it). A
// row with NO linkageSpecifiers (forDecl — deliberately, so a for-init static
// errors instead of routing) yields all-false; the row's gatedMarkers own the
// loud reject there. Emits NOTHING: Pass 2's validateThreadLocalDeclarator
// and the D-CSUBSET-LOCAL-STATIC lowering own the consequences.
struct SpecifierStorageFacts {
    bool threadStorage = false;   // a {threadStorage:true} entry matched
    bool staticStorage = false;   // a {staticStorage:true} entry matched
};
[[nodiscard]] SpecifierStorageFacts
scanSpecifierPrefixStorage(Tree const& tree, NodeId declNode,
                           DeclarationRule const& decl) {
    SpecifierStorageFacts out;
    if (decl.linkageSpecifiers.empty()) return out;
    NodeId const prefix = specifierPrefixChild(tree, declNode, decl);
    if (!prefix.valid()) return out;
    std::vector<NodeId> stack{prefix};
    for (int guard = 0; guard < 8192 && !stack.empty(); ++guard) {
        NodeId c = stack.back(); stack.pop_back();
        if (tree.kind(c) == NodeKind::Internal) {
            bool skip = false;
            for (RuleId rid : decl.linkageSpecifierIgnoredRules) {
                if (tree.rule(c).v == rid.v) { skip = true; break; }
            }
            if (skip) continue;
            for (NodeId g : visibleChildren(tree, c)) stack.push_back(g);
            continue;
        }
        auto it = decl.linkageSpecifiers.find(std::string{tree.text(c)});
        if (it == decl.linkageSpecifiers.end()) continue;
        if (it->second.threadStorage) out.threadStorage = true;
        if (it->second.staticStorage) out.staticStorage = true;
    }
    return out;
}

// TLS C1 (D-CSUBSET-THREAD-LOCAL): the first specifier-prefix TOKEN whose kind
// is in `kinds` (the language's `threadLocal.incompatibleSpecifierTokens` —
// c-subset: RegisterKeyword), or InvalidNode. Anchors the
// S_ThreadLocalInvalidCombination diagnostic at the offending specifier.
[[nodiscard]] NodeId
firstPrefixTokenOfKinds(Tree const& tree, NodeId declNode,
                        DeclarationRule const& decl,
                        std::vector<SchemaTokenId> const& kinds) {
    if (kinds.empty()) return {};
    NodeId const prefix = specifierPrefixChild(tree, declNode, decl);
    if (!prefix.valid()) return {};
    std::vector<NodeId> stack{prefix};
    for (int guard = 0; guard < 8192 && !stack.empty(); ++guard) {
        NodeId c = stack.back(); stack.pop_back();
        if (tree.kind(c) == NodeKind::Internal) {
            for (NodeId g : visibleChildren(tree, c)) stack.push_back(g);
            continue;
        }
        for (SchemaTokenId k : kinds) {
            if (tree.tokenKind(c).v == k.v) return c;
        }
    }
    return {};
}

// FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS, C23 6.7.13): the standard-attribute
// facts folded from ONE declaration's specifier prefix (or one bare attribute-
// declaration statement) by `scanAttributeSemantics` below. Computed once per
// declaration (the declAlignasSpec/declHasNoreturn precedent) and applied to
// every named declarator's SymbolRecord. Messages merge first-non-empty-wins
// across clauses/specifiers (design note F7).
struct AttributeSemanticsFacts {
    bool        maybeUnused = false;   // suppressUnused row matched
    bool        deprecated  = false;   // warnOnUse row matched
    std::string deprecatedMessage;
    bool        nodiscard   = false;   // warnOnDiscard row matched
    std::string nodiscardMessage;
};

// FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS): extract ONE attribute clause from a
// clause node — a whole `attrSpec` (`__attribute__((deprecated("m")))` is ONE
// clause) or one `stdAttrItem` (`deprecated("m")` / `gnu::packed` inside a
// `[[...]]`). The NAME is the LAST identifier among the node's DIRECT visible
// children (the `::`-namespaced form's final segment — `gnu::packed` → `packed`;
// both shapes inline their sequences, so the identifier(s) land flat),
// dunder-normalized via the shared `stripDunder`. The OPTIONAL string argument
// is the sole Internal direct child (a `stringLiteralExpr`), decoded via the
// SHARED `decodeAdjacentStringBodies` chokepoint (the static_assert message
// precedent — escapes + adjacent concat decode identically everywhere). A
// clause with no identifier (an empty `__attribute__(())`) yields nullopt.
struct AttributeClause {
    std::string_view name;      // dunder-normalized final name segment
    std::string      message;   // decoded string argument ("" = none/undecodable)
};
[[nodiscard]] std::optional<AttributeClause>
extractOneAttrClause(EngineState& s, SemanticConfig const& cfg,
                     Tree const& tree, NodeId clauseNode) {
    NodeId nameTok{};
    NodeId msgNode{};
    for (NodeId c : visibleChildren(tree, clauseNode)) {
        if (tree.kind(c) == NodeKind::Token) {
            if (cfg.identifierToken.valid()
                && tree.tokenKind(c) == cfg.identifierToken) {
                nameTok = c;   // LAST identifier wins → the ::-final segment
            }
            continue;
        }
        // The only Internal child either shape admits is the parenthesized
        // string-literal argument (`stringLiteralExpr`).
        if (!msgNode.valid()) msgNode = c;
    }
    if (!nameTok.valid()) return std::nullopt;
    AttributeClause out;
    out.name = stripDunder(tree.text(nameTok));
    if (msgNode.valid() && s.idx().stringLiteralBodyToken.valid()) {
        if (auto decoded = decodeAdjacentStringBodies(
                tree, msgNode, s.idx().stringLiteralBodyToken)) {
            out.message = std::move(*decoded);
        }
    }
    return out;
}

// FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS): fold the attribute-semantics effects
// over every attribute specifier in `startNode`'s subtree (a declaration's
// STRIPPED specifier prefix, or a bare attribute-declaration statement). A
// bounded descent that STOPS AT each attribute node: a `stdAttrRule` node
// (C23 `[[...]]`) contributes one clause per Internal child (`stdAttrItem` —
// `[[a, b]]` is two clauses); an `attrSpecRule` node (GNU `__attribute__`) is
// ONE clause. Each clause's name is matched (dunder-normalized) against the
// config's `attributeEffects` rows and the matched row's effect folds into
// `out` (messages first-non-empty-wins).
//
// UNKNOWN names: a C23 `[[...]]` clause matching NO row emits the SUPPRESSIBLE
// Warning S_UnknownAttribute — but ONLY when `emitUnknown` (the once-per-
// declaration Pass-1.5 site + the bare-statement Pass-2 arm; a re-scan must
// not double-fire). The GNU form NEVER warns here: its unknown names keep the
// pre-existing loud gates (file-scope H_UnknownLinkageSpecifier at the HIR
// linkage scan; the block-scope wholesale-ignore is the named deferral
// D-CSUBSET-ATTRIBUTE-GNU-BLOCK-SCOPE-UNKNOWN-NAME).
void scanAttributeSemantics(EngineState& s, SemanticConfig const& cfg,
                            Tree const& tree, NodeId startNode,
                            bool emitUnknown, AttributeSemanticsFacts& out) {
    if (!startNode.valid()) return;
    if (cfg.attributeEffects.empty()) return;
    if (!cfg.attrSpecRule.valid() && !cfg.stdAttrRule.valid()) return;
    auto const foldClause = [&](NodeId clauseNode, bool fromStdForm) {
        auto clause = extractOneAttrClause(s, cfg, tree, clauseNode);
        if (!clause.has_value()) return;
        AttributeSemanticsRow const* row = nullptr;
        for (auto const& r : cfg.attributeEffects) {
            for (auto const& nm : r.names) {
                if (clause->name == nm) { row = &r; break; }
            }
            if (row != nullptr) break;
        }
        if (row == nullptr) {
            if (fromStdForm && emitUnknown) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::S_UnknownAttribute;
                d.severity = DiagnosticSeverity::Warning;
                d.buffer   = tree.source().id();
                d.span     = tree.span(clauseNode);
                d.actual   = std::string{tree.text(clauseNode)};
                s.reporter.report(std::move(d));
            }
            return;
        }
        switch (row->effect) {
            case AttributeEffect::SuppressUnused:
                out.maybeUnused = true;
                break;
            case AttributeEffect::WarnOnUse:
                out.deprecated = true;
                if (out.deprecatedMessage.empty())
                    out.deprecatedMessage = std::move(clause->message);
                break;
            case AttributeEffect::WarnOnDiscard:
                out.nodiscard = true;
                if (out.nodiscardMessage.empty())
                    out.nodiscardMessage = std::move(clause->message);
                break;
            case AttributeEffect::None:
                break;   // known vocabulary, consumed elsewhere / inert
        }
    };
    std::vector<NodeId> stack{startNode};
    for (int guard = 0; guard < 8192 && !stack.empty(); ++guard) {
        NodeId c = stack.back(); stack.pop_back();
        if (tree.kind(c) != NodeKind::Internal) continue;
        RuleId const r = tree.rule(c);
        if (cfg.stdAttrRule.valid() && r.v == cfg.stdAttrRule.v) {
            for (NodeId item : visibleChildren(tree, c)) {
                if (tree.kind(item) == NodeKind::Internal)
                    foldClause(item, /*fromStdForm=*/true);
            }
            continue;   // stop AT the attribute node
        }
        if (cfg.attrSpecRule.valid() && r.v == cfg.attrSpecRule.v) {
            foldClause(c, /*fromStdForm=*/false);
            continue;   // stop AT the attribute node
        }
        for (NodeId g : visibleChildren(tree, c)) stack.push_back(g);
    }
}

// C11/C23 6.7.5 (D-CSUBSET-ALIGNAS): compute + validate the alignment an
// `alignasSpec` node requests. Reads the `alignasArg` operand (visible-child
// `alignasArgChild`): a `castTypeRef` (TYPE form) resolves the type + reads its
// alignment via `computeLayout(...)->align` (== _Alignof(T)); anything else
// (VALUE form) const-evaluates the constant-expression via the SAME `constIntExpr`
// static_assert / array-dimension folding uses. Validates:
//   • 0 ⇒ nullopt, NO error (6.7.5p3: "an alignment specification of zero has no
//     effect" — a NO-OP, treated as "no override" by the caller);
//   • not a power of two ⇒ S_AlignasNotPowerOfTwo, nullopt;
//   • > 256 ⇒ S_AlignasExceedsMax, nullopt (the `Alignment` newtype cap);
//   • non-constant value ⇒ S_AlignasNonConstant, nullopt.
// The WEAKER-than-natural check (6.7.5p4) is the CALLER's (it owns the declared
// type). Returns the validated alignment in bytes, or nullopt on 0/error.
[[nodiscard]] std::optional<std::uint32_t>
evalOneAlignasSpec(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                   NodeId alignasSpecNode, ScopeId fromScope) {
    auto emit = [&](DiagnosticCode code) {
        ParseDiagnostic d;
        d.code     = code;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(alignasSpecNode);
        d.actual   = std::string{tree.text(alignasSpecNode)};
        s.reporter.report(std::move(d));
    };
    auto kids = visibleChildren(tree, alignasSpecNode);
    if (cfg.alignasArgChild >= kids.size()) return std::nullopt;   // malformed
    NodeId const argNode = kids[cfg.alignasArgChild];
    // The `alignasArg` speculative alt commits EITHER the `alignasTypeName` wrapper
    // (TYPE form) OR a value expression (VALUE form); discriminate by the committed
    // child's rule. Descend to the sole visible child of the `alignasArg` alt
    // wrapper (it holds one reading).
    NodeId inner = argNode;
    if (tree.kind(argNode) == NodeKind::Internal) {
        auto argKids = visibleChildren(tree, argNode);
        if (argKids.size() == 1) inner = argKids.front();
    }
    bool const isTypeForm =
        cfg.alignasArgTypeRule.valid()
        && tree.kind(inner) == NodeKind::Internal
        && tree.rule(inner).v == cfg.alignasArgTypeRule.v;
    std::int64_t value = 0;
    if (isTypeForm) {
        // The type form is `alignasTypeName [ castTypeRef ]` — resolve the SOLE
        // castTypeRef child inside the wrapper (the wrapper exists only to carry
        // the commitRequiresTypeName guard on the probed branch).
        NodeId typeChild = inner;
        {
            auto wrapKids = visibleChildren(tree, inner);
            if (!wrapKids.empty()) typeChild = wrapKids.front();
        }
        // emitOnMiss=false: an unknown queried type yields nullopt → we fail loud
        // with ONE positioned diagnostic below (never a redundant S_UnknownType).
        // This runs BEFORE the layout-params check so `alignas(<not-a-type>)` — a
        // value that the parser optimistically committed as a type-name — fails
        // loud (S_AlignasNonConstant) even in a layout-free analysis, NEVER silent.
        TypeId const queried =
            resolveTypeNode(s, cfg, tree, typeChild, fromScope, /*emitOnMiss=*/false);
        if (queried.valid()) s.nodeToType.set(typeChild, queried);
        if (!queried.valid()) { emit(DiagnosticCode::S_AlignasNonConstant); return std::nullopt; }
        // A resolvable type whose alignment we cannot compute (no layout params)
        // is a genuine "cannot determine the alignment" → fail loud, never silent.
        if (!s.aggregateLayout.has_value()) {
            emit(DiagnosticCode::S_AlignasNonConstant); return std::nullopt;
        }
        auto const layout = computeLayout(queried, s.lattice.interner(),
                                          *s.aggregateLayout, s.dataModel);
        if (!layout) { emit(DiagnosticCode::S_AlignasNonConstant); return std::nullopt; }
        value = static_cast<std::int64_t>(layout->align.bytes());
    } else {
        auto folded = constIntExpr(s, tree, inner, fromScope, &cfg);
        if (!folded.has_value()) { emit(DiagnosticCode::S_AlignasNonConstant); return std::nullopt; }
        value = *folded;
    }
    // 6.7.5p3: `alignas(0)` has NO effect → no override, NO error. This is the
    // ONLY value the standard makes a defined no-op; it is NOT extended to a
    // NEGATIVE fold — `alignas(-4)` is a constraint violation (a valid alignment
    // is a positive power of two), which gcc/clang both reject. Failing it loud
    // (rather than silently swallowing it as "no alignment") is the fail-loud bar:
    // a negative is not a power of two, so S_AlignasNotPowerOfTwo names the reason.
    if (value == 0) return std::nullopt;
    if (value < 0) { emit(DiagnosticCode::S_AlignasNotPowerOfTwo); return std::nullopt; }
    // A member alignas may only RAISE alignment (a valid fundamental/extended
    // alignment is a positive power of two, C 6.7.5p3).
    std::uint64_t const uv = static_cast<std::uint64_t>(value);
    if ((uv & (uv - 1u)) != 0u) { emit(DiagnosticCode::S_AlignasNotPowerOfTwo); return std::nullopt; }
    if (uv > 256u)             { emit(DiagnosticCode::S_AlignasExceedsMax);      return std::nullopt; }
    return static_cast<std::uint32_t>(uv);
}

// FC16 (D-CSUBSET-PACKED): scan a struct/union specifier node's TRAILING
// composite-attribute list (`compositeAttrListRule`) for a honored `packed`
// attribute. Returns true iff a recognized `packed` spelling is present.
//
// When `emitDiagnostics` is true (the ONE composition site), an UNRECOGNIZED
// attribute in the STRICT (GNU `__attribute__`) form fails loud
// S_UnknownTypeAttribute (typo protection, mirroring H_UnknownLinkageSpecifier —
// GNU attribute identifiers are all meaningful, so a `__attribute__((pakced))`
// typo must not silently leave the struct unpacked); an unrecognized C23 `[[...]]`
// is standard-ignorable (the `[[deprecated]]` precedent). When false (the member-
// alignas baseline probe, which may run per member), it detects packed WITHOUT
// emitting, so the diagnostic fires exactly once.
//
// Config-driven + source-AGNOSTIC: `compositeAttrListRule` / `packedAttributeNames`
// / `compositeStrictAttrRule` name the vocabulary; nothing here hardcodes the
// spelling "packed" or a rule name. Packed detection matches an attribute
// IDENTIFIER leaf (dunder-normalized via the shared `stripDunder`, so `__packed__`
// ≡ `packed` and `[[gnu::packed]]`'s final segment `packed` matches) against
// `packedAttributeNames` — a string ARGUMENT (`section("packed")`) is a
// string-literal leaf, not an identifier, so it never false-matches.
[[nodiscard]] bool
scanCompositePacked(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                    NodeId specNode, bool emitDiagnostics) {
    if (!cfg.compositeAttrListRule.valid() || !specNode.valid()) return false;
    // The `compositeAttrList` is the structSpec/unionSpec's trailing direct child.
    NodeId listNode{};
    for (NodeId c : visibleChildren(tree, specNode)) {
        if (tree.kind(c) == NodeKind::Internal
            && tree.rule(c).v == cfg.compositeAttrListRule.v) {
            listNode = c;
            break;
        }
    }
    if (!listNode.valid()) return false;
    bool packed = false;
    // Each visible child of the list is ONE composite attribute (`compositeAttr` ->
    // attrSpec | stdAttr). For each: does it NAME a packed attribute? and is it the
    // STRICT (GNU) form (unrecognized -> diagnose) or the ignorable (C23) form?
    for (NodeId attr : visibleChildren(tree, listNode)) {
        if (tree.kind(attr) != NodeKind::Internal) continue;
        bool named  = false;   // this attribute names `packed`
        bool strict = false;   // this attribute is the GNU `__attribute__` form
        std::vector<NodeId> stack{attr};
        for (int guard = 0; guard < 4096 && !stack.empty(); ++guard) {
            NodeId const cur = stack.back();
            stack.pop_back();
            if (tree.kind(cur) == NodeKind::Internal) {
                if (cfg.compositeStrictAttrRule.valid()
                    && tree.rule(cur).v == cfg.compositeStrictAttrRule.v) {
                    strict = true;
                }
                for (NodeId g : visibleChildren(tree, cur)) stack.push_back(g);
                continue;
            }
            if (cfg.identifierToken.valid()
                && tree.tokenKind(cur) == cfg.identifierToken) {
                std::string_view const id = stripDunder(tree.text(cur));
                for (std::string const& nm : cfg.packedAttributeNames) {
                    if (id == nm) { named = true; break; }
                }
            }
        }
        if (named) { packed = true; continue; }
        // Not a recognized packed attribute. A GNU `__attribute__` typo / unsupported
        // spelling fails loud (typo protection); a C23 `[[...]]` is standard-ignorable.
        if (strict && emitDiagnostics) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::S_UnknownTypeAttribute;
            d.severity = DiagnosticSeverity::Error;
            d.buffer   = tree.source().id();
            d.span     = tree.span(attr);
            d.actual   = std::string{tree.text(attr)};
            s.reporter.report(std::move(d));
        }
    }
    return packed;
}

// C11/C23 6.7.5 (D-CSUBSET-ALIGNAS): the EFFECTIVE alignment override a
// declaration's specifier prefix requests — the MAX over every `alignasSpec` in
// the prefix (C 6.7.5p6: with several alignment specifiers, the strictest — the
// largest — wins). Each spec is computed + validated by `evalOneAlignasSpec`
// (which emits pow2/max/non-constant diagnostics). When `declType` is a valid
// type, the winning alignment is ALSO checked against its natural alignment
// (6.7.5p4: alignas may not WEAKEN) → S_AlignasWeakerThanNatural. Returns the
// override in bytes (nullopt = no override / all zero / all errored). A NO-OP
// (returns nullopt) when the declaration carries no alignas.
[[nodiscard]] std::optional<std::uint32_t>
resolveAlignasOverride(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                       NodeId declNode, DeclarationRule const& decl,
                       TypeId declType, ScopeId fromScope,
                       std::optional<std::uint32_t> naturalBaseline = std::nullopt) {
    if (!cfg.alignasSpecRule.valid()) return std::nullopt;
    NodeId const prefix = specifierPrefixChild(tree, declNode, decl);
    if (!prefix.valid()) return std::nullopt;
    // Collect every alignasSpec node in the prefix (there may be more than one).
    std::vector<NodeId> specs;
    {
        std::vector<NodeId> stack{prefix};
        for (int guard = 0; guard < 8192 && !stack.empty(); ++guard) {
            NodeId c = stack.back(); stack.pop_back();
            if (tree.kind(c) != NodeKind::Internal) continue;
            if (tree.rule(c).v == cfg.alignasSpecRule.v) { specs.push_back(c); continue; }
            for (NodeId g : visibleChildren(tree, c)) stack.push_back(g);
        }
    }
    std::optional<std::uint32_t> best;
    for (NodeId sp : specs) {
        if (auto a = evalOneAlignasSpec(s, cfg, tree, sp, fromScope)) {
            if (!best.has_value() || *a > *best) best = a;
        }
    }
    if (!best.has_value()) return std::nullopt;
    // 6.7.5p4: an alignas that is WEAKER than the type's natural alignment is a
    // constraint violation (alignas may only strengthen). Compare against the
    // declared type's computeLayout align. Skip when the type is unresolved (an
    // upstream error already fired) or there are no layout params.
    if (declType.valid() && s.aggregateLayout.has_value()) {
        auto const layout = computeLayout(declType, s.lattice.interner(),
                                          *s.aggregateLayout, s.dataModel);
        if (layout) {
            // D-CSUBSET-PACKED: `naturalBaseline` overrides the type's natural
            // alignment when the enclosing composite is PACKED (baseline 1 — packed
            // removes the padding requirement), so `alignas(1)` INSIDE a packed struct
            // is legal (never weaker-than-natural) while `alignas(1)` OUTSIDE (baseline
            // absent → the type's own align) still fails 6.7.5p4.
            std::uint64_t const naturalBytes =
                naturalBaseline.has_value()
                    ? static_cast<std::uint64_t>(*naturalBaseline)
                    : static_cast<std::uint64_t>(layout->align.bytes());
            if (*best < naturalBytes) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::S_AlignasWeakerThanNatural;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = tree.source().id();
                d.span     = tree.span(specs.empty() ? declNode : specs.front());
                d.actual   = std::string{tree.text(specs.empty() ? declNode : specs.front())};
                s.reporter.report(std::move(d));
                // Still return the requested override — the layout carrier's
                // MAX(natural, override) makes a too-weak override a no-op anyway; the
                // diagnostic is the fail-loud (the build fails via hasErrors regardless).
            }
        }
    }
    return best;
}

// FC16 D-CSUBSET-ANON-MEMBER-PROMOTION (C11/C23 §6.7.2.1 ¶13): recursively
// PROMOTE the members of an anonymous struct/union member into the enclosing
// composite's member namespace. `anonSym` is the synthetic `<anon:…>` field
// symbol whose resolved type `anonTy` is a Struct/Union; `path` is the ordered
// chain of anonymous-member SymbolIds (outermost→innermost) already traversed
// to reach it; `enclosingScope` is the FIELD SCOPE of the OUTERMOST enclosing
// composite (where the direct members were bound in Pass 1 and where a
// promoted name must not collide). For each NAMED member of the anon composite
// we record its `anonAncestorPath` (so member-access lookup + HIR lowering can
// synthesize the intermediate hops); a named member that collides with a
// DIRECT member of the enclosing composite fails loud (S_RedeclaredSymbol). A
// nested anonymous member recurses (its own synthetic name is NOT promoted).
//
// Iteration order is DETERMINISTIC (by `fieldIndex`) — `bindingsOf` order is
// unspecified, and a stable order keeps a sibling-collision diagnostic and the
// recorded paths reproducible. Only the Ordinary namespace is promoted (a
// nested tag stays in its own namespace).
void promoteAnonMembers(EngineState& s, Tree const& tree, SymbolId anonSym,
                        TypeId anonTy, std::vector<SymbolId> path,
                        ScopeId enclosingScope) {
    path.push_back(anonSym);
    auto const scopeIt = s.compositeScopeByType.find(anonTy.v);
    if (scopeIt == s.compositeScopeByType.end()) return;   // unresolved anon body
    ScopeId const anonScope = scopeIt->second;
    // Snapshot + sort the Ordinary bindings by declaration-order fieldIndex.
    std::vector<SymbolId> ordered;
    for (auto const& [name, ns, fsym] : s.scopes.bindingsOf(anonScope)) {
        (void)name;
        if (ns != SymbolNamespace::Ordinary) continue;
        if (fsym.valid()) ordered.push_back(fsym);
    }
    std::sort(ordered.begin(), ordered.end(),
              [&](SymbolId a, SymbolId b) {
                  return s.symbols.at(a).fieldIndex < s.symbols.at(b).fieldIndex;
              });
    for (SymbolId fsym : ordered) {
        SymbolRecord& frec = s.symbols.at(fsym);
        if (frec.isAnonymousMember) {
            // A NESTED anonymous member — recurse to promote ITS members. The
            // synthetic anon name itself is never promoted into the enclosing
            // scope (it carries no user-visible name).
            promoteAnonMembers(s, tree, fsym, frec.type, path, enclosingScope);
            continue;
        }
        // A named member: it becomes visible as a member of the enclosing
        // composite. Collision-check ONLY against a DIRECT member of the
        // enclosing composite's OWN field scope — NOT `scopes.lookup`, which
        // walks the parent chain and would falsely flag a member that legally
        // shares a name with an ordinary identifier in an enclosing lexical
        // scope (a global/typedef/function — C 6.2.1 gives each struct/union a
        // SEPARATE member name space disjoint from ordinary identifiers). We
        // read the field scope's `bindings` (the Ordinary namespace) directly,
        // exactly as `resolveMemberAccess`'s direct-member lookup does. Fail
        // loud only on a genuine same-composite duplicate.
        std::string const memberName{s.symbols.at(fsym).name};
        SymbolId direct{};
        if (enclosingScope.valid()
            && enclosingScope.v < s.scopes.scopes().size()) {
            auto const& encBindings =
                s.scopes.scopes()[enclosingScope.v].bindings;
            auto const encIt = encBindings.find(memberName);
            if (encIt != encBindings.end()) direct = encIt->second;
        }
        if (direct.valid()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::S_RedeclaredSymbol;
            d.severity = DiagnosticSeverity::Error;
            d.buffer   = tree.source().id();
            d.span     = tree.span(s.symbols.at(fsym).declNode);
            d.actual   = memberName;
            auto const& dirRec = s.symbols.at(direct);
            if (dirRec.declNode.valid() && dirRec.tree.v == tree.id().v) {
                d.related.push_back(RelatedLocation{
                    tree.source().id(),
                    tree.span(dirRec.declNode),
                    "conflicts with this direct member",
                });
            }
            s.reporter.report(std::move(d));
            continue;   // do not record a path for the colliding name
        }
        frec.anonAncestorPath = path;
    }
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
                       bool allowFlexibleArray = false,
                       bool allowInitInferredArray = false,
                       bool paramDecay = false);

// c82 D-CSUBSET-PARAM-ARRAY-ADJUSTMENT (C 6.7.6.3p7): a declaration form with
// `arrayToPointer` whose resolved declarator type is an ARRAY — sized
// (`int a[64]`), incomplete (`int a[]`), or multi-dimensional (`int a[][5]`
// — only the OUTERMOST dimension is the parameter's own type constructor) —
// ADJUSTS to a POINTER to its element type. Applied at BOTH resolution
// sites (the definitive Pass-1.5 visit that binds the symbol, and the
// FnSig param harvest `declRowDeclaredType`), so the bound symbol, the
// FnSig, and every call site agree by construction. The transparent
// kind()/operands() accessors make a qualified element ride into the
// pointee unchanged. Non-array types (and rows without the flag) pass
// through untouched.
[[nodiscard]] TypeId
adjustArrayToPointer(EngineState& s, DeclarationRule const& decl, TypeId t) {
    if (!decl.arrayToPointer || !t.valid()) return t;
    // c82: the CC's `va_list` is EXCLUDED — the per-CC va_* machinery (c63)
    // owns va_list parameter passing end-to-end (param slot, decay-at-call,
    // va_arg addressing), and C 7.16 makes a va_list observable only through
    // va_*/forwarding, so the adjustment has no program-visible effect on it
    // while silently breaking the va_arg indirection depth (a SysV SIGSEGV,
    // witnessed). Both the NAMED and the ABSTRACT param path share this
    // helper, so prototype/definition FnSigs stay structurally equal.
    if (s.vaListType.has_value() && t == *s.vaListType) return t;
    auto& in = s.lattice.interner();
    if (in.kind(t) != TypeKind::Array) return t;
    auto const elems = in.operands(t);
    if (elems.empty() || !elems[0].valid()) return t;  // interner invariant —
                                                       // downstream fails loud
    return in.pointer(elems[0]);
}

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
                // c82 (C 6.7.6.3p7) + VLA C4a-param (D-CSUBSET-VLA, FIX-2): an
                // `arrayToPointer` row is a C PARAMETER — route its array-decay
                // through the DISTINCT `paramDecay` signal (NOT the struct-field FAM
                // `allowFlexibleArray`, which stays false here). paramDecay admits the
                // absent-length `T x[]` AND builds a `vlaArray` for a non-outermost
                // present-length suffix (`int (*p)[n]` → the pointee keeps `[n]`); the
                // adjustment below then rewrites any top-level array to Ptr<element>.
                // Mirrors the definitive visit's identical sequence so the FnSig agrees.
                TypeId const t = declaratorDeclaredType(
                    s, cfg, tree, kids[*decl.declaratorChild], head, scope,
                    emitOnMiss,
                    /*allowFlexibleArray=*/false,
                    /*allowInitInferredArray=*/false,
                    /*paramDecay=*/decl.arrayToPointer);
                return adjustArrayToPointer(s, decl, t);
            }
            // Declarator structurally absent — a TYPE-ONLY (abstract) param.
            // C 6.7.6.3p7 adjusts the declared type REGARDLESS of a name:
            // `int f(const int[4]);` and `int f(const int a[4]);` are the
            // SAME signature. Both the named path (above) and this abstract
            // path run the ONE shared helper, so a prototype and its
            // definition can never drift into an S0022/S0003 asymmetry —
            // the exact mid-c82 shell.c failure (`sqlite3_vmprintf(const
            // char*, va_list)` abstract vs the named caller param; va_list
            // itself is EXCLUDED inside the helper, see its comment).
            return adjustArrayToPointer(s, decl, head);
        }
        return InvalidType;   // a LIST row has no single param type
    }
    if (decl.typeChild.has_value() && *decl.typeChild < kids.size()) {
        TypeId pty = resolveTypeNode(s, cfg, tree, kids[*decl.typeChild], scope,
                                     emitOnMiss);
        pty = applyArraySuffix(s, tree, decl, declNode, pty, scope, &cfg);
        // c82: the legacy-row twin of the declarator-mode adjustment above —
        // the two param-resolution shapes must not drift.
        return adjustArrayToPointer(s, decl, pty);
    }
    return InvalidType;
}

// Apply ONE declarator suffix (fn / array per the config roles) to the
// type accumulated so far. See the inversion comment above for ordering.
[[nodiscard]] TypeId
applyDeclaratorSuffix(EngineState& s, SemanticConfig const& cfg,
                      Tree const& tree, NodeId suffix, TypeId inner,
                      ScopeId scope, bool emitOnMiss,
                      bool allowFlexibleArray = false,
                      bool paramDecay = false) {
    DeclaratorConfig const& dc = *cfg.declarators;
    RuleId const r = tree.rule(suffix);
    if (isFnSuffixRule(r, dc)) {
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
    if (dc.arrayStarSuffixRule.has_value() && r == *dc.arrayStarSuffixRule) {
        // VLA C4c (D-CSUBSET-VLA-PARAM-STAR, C99 §6.7.6.2p4): the bare
        // unspecified-size `[*]` — a prototype-form VLA-parameter marker. Legal
        // ONLY inside a function-parameter declarator (C 6.7.6.3p7 adjusts it to
        // a pointer). For a PARAMETER (`paramDecay`) it is an UNSPECIFIED-size
        // (absent-length) array — IDENTICAL to a bare `[]` — that
        // `adjustArrayToPointer` then strips to `Ptr<element>`; the `*` carries
        // no runtime bound (unlike `[static n]`, so NEVER a vlaArray). A
        // NON-parameter `[*]` is a constraint violation — the SAME paramDecay
        // gate + diagnostic (S_ArrayParamQualifierNonParameter, 0xE054) a
        // static/qualifier decoration trips in the arraySuffixRule arm below.
        if (!paramDecay) {
            if (emitOnMiss) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::S_ArrayParamQualifierNonParameter;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = tree.source().id();
                d.span     = tree.span(suffix);
                d.actual   = std::string{tree.text(suffix)};
                s.reporter.report(std::move(d));
            }
            return InvalidType;
        }
        return s.lattice.interner().incompleteArray(inner);
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
        // VLA C4c (D-CSUBSET-VLA, C99 §6.7.6.2/6.7.6.3): the array-PARAMETER
        // decorations — a `static`, a cv-qualifier, or the unspecified-size `*` —
        // are legal ONLY in a function-parameter declarator (C 6.7.6.3p7 adjusts
        // such a `[static n]`/`[*]` to a pointer). This ONE arm folds EVERY
        // declarator-mode row; `paramDecay` is true ONLY on the `param` row, so a
        // decoration on ANY other row (a local / struct field / typedef / for-init)
        // is a constraint violation — fail loud (the typeSpecifierSeq →
        // S_InvalidTypeSpecifierCombination reject-the-construct discipline). A
        // legal deref-sized VLA `int a[*p]` is an EXPRESSION node, NOT a bare `*`
        // token, so `arraySuffixHasModifier` (direct-children scan) never trips on
        // it. externDecl takes the legacy `applyArraySuffix` twin (never a
        // parameter), which carries its own copy of this gate.
        if (!paramDecay
            && arraySuffixHasModifier(tree, suffix, dc.arraySuffixModifierTokens)) {
            emit(DiagnosticCode::S_ArrayParamQualifierNonParameter);
            return InvalidType;
        }
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
            // CRITICAL discriminator — an ABSENT length (`T x[]`, an incomplete/
            // decaying array) vs a PRESENT non-constant length (`T x[n]`, a VLA): BOTH
            // fold to nullopt, so the discriminator is whether a real BOUND child sits
            // between the brackets. VLA C4c (D-CSUBSET-VLA): NOT a raw child COUNT — a
            // `[static n]` bound sits BEHIND the `static` token (count 4, bound present)
            // while a `[*]` / `[restrict]` decoration has NO bound child (count 3, bound
            // ABSENT — it must decay like `[]`, NEVER route to `vlaArray`). The shared
            // `arraySuffixBoundNode` skips the decorations and returns the real bound (or
            // nullopt) — the ONE locator every array-suffix site now agrees on.
            bool const hasPresentLength =
                arraySuffixBoundNode(tree, suffix, dc.arraySuffixModifierTokens)
                    .has_value();
            // VLA C4a-param (D-CSUBSET-VLA, Option B): a C PARAMETER declarator
            // (`arrayToPointer` row — a DISTINCT signal threaded as `paramDecay`, NEVER
            // the struct-field FAM `allowFlexibleArray`). C 6.7.6.3p7 adjusts a param's
            // OUTERMOST array to a pointer, but a NON-outermost VLA suffix survives in
            // the pointee (`int (*p)[n]` → `int (*)[n]`; `int a[][n]` → same after the
            // decayed outer `[]`). Build the vlaArray for a PRESENT length so the
            // pointee carries the runtime row shape; an ABSENT `[]` is the (possibly
            // outermost) decaying dim → incompleteArray, which `adjustArrayToPointer`
            // then strips to `Ptr<element>`. Checked FIRST + kept off the FAM bool so
            // the struct-field FAM path (below) stays BYTE-IDENTICAL — a param never
            // reaches the FAM branch, a field never reaches here.
            if (paramDecay) {
                TypeInterner& in = s.lattice.interner();
                return hasPresentLength ? in.vlaArray(inner)
                                        : in.incompleteArray(inner);
            }
            // c10 D-CSUBSET-STRUCT-MEMBER-DECLARATOR (FAM): an ABSENT length on a
            // declaration form that may bear a flexible array member (a struct field,
            // OR an init-inferred `[]`) is an INCOMPLETE array (C99 §6.7.2.1), NOT an
            // error — the declarator-mode twin of the legacy `applyArraySuffix` FAM
            // branch. Only the field's OWN array suffix inherits this; a nested fn-ptr
            // param's array never does (the group recursion passes false).
            if (allowFlexibleArray)
                return s.lattice.interner().incompleteArray(inner);
            // VLA C1a (D-CSUBSET-VLA): a PRESENT-but-non-constant length on a NON-FAM,
            // NON-param declarator is a VARIABLE-LENGTH array (C99/C11 §6.7.6.2
            // `int a[n]` — a VLA OBJECT). Build the vlaArray HERE regardless of
            // scope/storage: the block-vs-file + automatic-storage constraints are
            // enforced by the Pass-2 `validateVlaDeclarator`, which reads the SYMBOL's
            // binding scope (`rec.scope`) — the type-construction `scope` here is a
            // descendant of, NOT equal to, the file scope even for a file-scope decl,
            // so `fileScopeOf(scope)` cannot discriminate file from block at this tier.
            // VLA C3: a VLA whose ELEMENT is itself an array or VLA (`int a[n][m]`,
            // `int a[n][5]`) is a MULTI-DIMENSIONAL VLA — the right-to-left suffix fold
            // already produced `inner` as the element; HIR→MIR sizes the runtime stride.
            if (hasPresentLength)
                return s.lattice.interner().vlaArray(inner);
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
        // VLA C3 (D-CSUBSET-VLA): a CONSTANT outer bound over a VLA element
        // (`int a[5][n]` — the `[n]` folded first into a vlaArray, now `[5]` folds
        // over it) is a fixed-outer multi-dimensional VLA. Build `array(vlaArray, 5)`;
        // the transitive `typeContainsVla` routes it to the runtime alloca/stride/
        // sizeof paths downstream. The FAM guard above still rejects an incomplete
        // (-1) element; only the -2 VLA element flows through here.
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
                   bool allowFlexibleArray, bool allowInitInferredArray,
                   bool paramDecay) {
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
        // VLA C4c (D-CSUBSET-VLA-PARAM-STAR): the bare-`[*]` suffix is a THIRD
        // array-suffix role alongside fn/array — collect it so `applyDeclarator-
        // Suffix` folds it (a param `[*]` decays like `[]`; a non-param is a
        // constraint violation). Omitting it here would SILENTLY drop the `[*]`
        // → `int a[*]` would mis-type as a plain `int` (a silent miscompile).
        if (isFnSuffixRule(cr, dc) || cr == dc.arraySuffixRule
            || (dc.arrayStarSuffixRule.has_value()
                && cr == *dc.arrayStarSuffixRule)) {
            suffixes.push_back(c);
            continue;
        }
        if (cr == dc.groupRule && !group.valid()) group = c;
    }

    // Suffixes fold RIGHT-to-LEFT (source-first suffix = outermost type).
    for (std::size_t i = suffixes.size(); i-- > 0;) {
        t = applyDeclaratorSuffix(s, cfg, tree, suffixes[i], t, scope,
                                  emitOnMiss, allowFlexibleArray, paramDecay);
        if (!t.valid()) return InvalidType;
    }

    if (nameTok.valid()) return t;   // bound at the name
    if (group.valid()) {
        NodeId const inner = declarator_walk_detail::firstChildOfRule(
            TreeDeclaratorView{tree}, group, dc.declaratorRule);
        if (!inner.valid()) return InvalidType;   // malformed group
        // GOTCHA (c10 plan-lock): FAM-ness does NOT cross into a grouped/
        // parenthesized inner declarator — a nested fn-ptr param's array
        // (`int (*f)(int x[]))`) is its OWN declarator and keeps the ordinary
        // `S_NonConstantArrayLength` behavior; only the field's TOP-LEVEL array
        // suffix is a flexible array member (so allowFlexibleArray stays false).
        // c47 (D-CSUBSET-FNPTR-ARRAY-SIZE-INFERENCE): an INIT-INFERRED `[]` DOES
        // cross in — `int (*const arr[])(T) = {…}` (an inferred-size array of
        // fn-ptrs) carries its `[]` on THIS inner declarator and is sized from the
        // top-level initializer. Propagate ONLY that init-inference signal as the
        // inner's flexible flag; a NO-init grouped `[]` keeps it false ⇒ S000B.
        // VLA C4a-param FIX-1 (D-CSUBSET-VLA): param decay applies ONLY to the
        // OUTERMOST dim (C 6.7.6.3p7); a grouped/parenthesized inner declarator is
        // NOT the decaying dim, so RESET `paramDecay=false` here (the two witnesses
        // are unaffected — `(*p)[n]`'s `[n]` folds at the OUTER suffix above, before
        // this recursion; `a[][n]` has no group — but the reset prevents a latent
        // over-lenient accept on an exotic `int (*p[])[n]`). `allowInitInferredArray`
        // stays a DISTINCT signal (c47 init-inferred fn-ptr arrays), not collapsed.
        return declaratorDeclaredType(s, cfg, tree, inner, t, scope, emitOnMiss,
                                      /*allowFlexibleArray=*/allowInitInferredArray,
                                      /*allowInitInferredArray=*/allowInitInferredArray,
                                      /*paramDecay=*/false);
    }
    return t;   // abstract direct — the type itself
}

TypeId declaratorDeclaredType(EngineState& s, SemanticConfig const& cfg,
                              Tree const& tree, NodeId node, TypeId base,
                              ScopeId scope, bool emitOnMiss,
                              bool allowFlexibleArray, bool allowInitInferredArray,
                              bool paramDecay) {
    if (!cfg.declarators.has_value()) return InvalidType;
    DeclaratorConfig const& dc = *cfg.declarators;
    if (!node.valid() || !base.valid()) return InvalidType;
    if (tree.kind(node) != NodeKind::Internal) return InvalidType;
    RuleId const r = tree.rule(node);
    if (r == dc.initDeclaratorRule) {
        NodeId const inner = declarator_walk_detail::firstChildOfRule(
            TreeDeclaratorView{tree}, node, dc.declaratorRule);
        if (!inner.valid()) return InvalidType;
        // Transparent wrapper — paramDecay rides through UNCHANGED (the decaying-dim
        // signal belongs to the wrapped declarator, not this init-declarator shell).
        return declaratorDeclaredType(s, cfg, tree, inner, base, scope,
                                      emitOnMiss, allowFlexibleArray,
                                      allowInitInferredArray, paramDecay);
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
        // Transparent wrapper — paramDecay rides through UNCHANGED (a struct member
        // slot; paramDecay is false here in practice, but stay signal-preserving).
        return declaratorDeclaredType(s, cfg, tree, inner, base, scope,
                                      emitOnMiss, allowFlexibleArray,
                                      allowInitInferredArray, paramDecay);
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
            // c27 (D-CSUBSET-VOLATILE-POINTEE): EAST volatile — a `volatile`
            // qualifier INSIDE this pointer layer (the `*volatile` / `* volatile`
            // ptrQualifier, AFTER the star) makes THIS pointer OBJECT volatile
            // (C 6.7.3): wrap the just-formed pointer in VolatileQual so the
            // symbol's type is `VolatileQual(Ptr<...>)`. This is what makes the
            // symbol object-volatile uniformly through the TYPE (the c21
            // isVolatile is now derived from the type's top-level VolatileQual-
            // ness — see the binding sites). `const`/`restrict` in the layer stay
            // ignored. Config-driven on `cfg.volatileMarker`; a layer with no
            // volatile is byte-identical to before.
            if (cfg.volatileMarker.has_value()
                && subtreeContainsToken(tree, c, *cfg.volatileMarker,
                                        &s.idx().declByRule)) {
                t = s.lattice.interner().volatileQualified(t);
            }
            // D-CSUBSET-ATOMIC (FC17.9(d) 1b): the EAST `_Atomic` — an `atomicMarker`
            // ptrQualifier INSIDE this pointer layer (after the star, `int * _Atomic p`)
            // makes THIS pointer OBJECT atomic ⇒ atomicQualified(Ptr<...>), the exact
            // mirror of the east-volatile wrap above. Composes with a co-present east
            // volatile in the one shared skin (cycle 1a bit-merge).
            if (cfg.atomicMarker.has_value()
                && subtreeContainsToken(tree, c, *cfg.atomicMarker,
                                        &s.idx().declByRule)) {
                t = s.lattice.interner().atomicQualified(t);
            }
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
                              allowFlexibleArray, allowInitInferredArray,
                              paramDecay);
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
// membership — no rule-name identity branch. STOPS at a block or the file/CU
// root; floats PAST declarator-dominator scopes AND (c38
// D-CSUBSET-NESTED-TAG-SCOPE) composite-body (member) scopes — a TAG nested in
// a struct body belongs to the enclosing block/file scope, not the member one.
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
        // c38 (D-CSUBSET-NESTED-TAG-SCOPE): a composite-body scope is a MEMBER
        // namespace, NOT a tag namespace. C 6.2.1p4: a struct/union/enum TAG
        // declared inside a struct body belongs to the nearest ENCLOSING block
        // or file scope, not the member scope — so a nested tag floats PAST a
        // composite body too (this was a `break`, which TRAPPED a nested tag in
        // the inner struct's member scope → invisible BY NAME from the enclosing
        // scope → `struct Inner *p` outside Outer failed S_NotAComposite: the
        // sqlite `WalSegment`/`sColMap`/`IdList_item`/`ExprList_item` S000D
        // cascade). A block-LOCAL nested tag still floats only to its BLOCK
        // scope (the not-in-declByRule break above stops it), staying
        // block-scoped — never leaking to file scope. Nominal identity (c24
        // decl-site key) and member composition (c25 structScope=`here`) are
        // independent of this BIND scope, so only by-name lookup changes.
        scope = scs[scope.v].parent;  // skip declarator-dominator AND composite-body scopes
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

// c33 (D-CSUBSET-TENTATIVE-DEFINITION): does this declarator-carrier node carry an
// INITIALIZER (`= expr` / `= {…}`)? A file-scope object declaration WITHOUT one is
// a TENTATIVE DEFINITION (C 6.9.2) — mergeable with a later real definition and
// with other tentatives; WITH one it is a REAL definition (collides with a second
// real definition). `dNode` is a carrier from `collectDeclarators`: either an
// `initDeclaratorRule` node (`declarator (= initValue)?`) or a bare `declaratorRule`
// node (the single-slot collapse — never an initializer). For the initDeclarator
// form the initializer is the lone VISIBLE INTERNAL child that is NOT the inner
// `declaratorRule` — structurally identical to how the HIR global-init lowering
// finds the init (cst_to_hir.cpp `lowerVarLikeInto`), so the two cannot drift.
// Config-driven on the resolved rule ROLES (no keyword / `=`-token identity).
[[nodiscard]] bool declaratorHasInitializer(Tree const& tree,
                                            DeclaratorConfig const& dc,
                                            NodeId dNode) {
    if (!dNode.valid() || tree.kind(dNode) != NodeKind::Internal) return false;
    if (tree.rule(dNode) != dc.initDeclaratorRule) return false;  // bare declarator
    for (NodeId c : visibleChildren(tree, dNode)) {
        if (tree.kind(c) != NodeKind::Internal) continue;   // skip the `=` token
        if (tree.rule(c) == dc.declaratorRule) continue;    // the declarator itself
        return true;   // any other internal child IS the initValue
    }
    return false;
}

// TLS C1 (D-CSUBSET-THREAD-LOCAL): enforce the C11/C23 6.7.1 thread-storage
// constraints for ONE declared name whose symbol was Pass-1-marked
// `isThreadLocal` (the validateConstexprDeclarator model — same hook
// discipline: Pass 2, after Pass 1.5 resolved the declared type, so the FnSig
// test sees the real signature). Runs from BOTH pass2Post declaration paths —
// the declarator-mode per-declarator loop (topLevelDecl / varDecl /
// autoInferred*) and the positional-name arm (c-subset's externDecl). Every
// arm fails loud; a thread_local declaration NEVER silently degrades to a
// plain (process-shared or automatic) object:
//   * unresolved declared type    → return (cascade — already diagnosed);
//   * FnSig / Function symbol     → S_ThreadLocalOnFunction (6.7.1p4 —
//     objects only; covers protos, definitions, and `extern thread_local`
//     function declarations);
//   * co-present `constexpr`      → S_ThreadLocalInvalidCombination (C23
//     6.7.1: constexpr pairs only with auto/register/static — read off the
//     Pass-1 isConstexpr mark, so both specifier orders are caught);
//   * a `threadLocal.incompatibleSpecifierTokens` token in the prefix
//     (c-subset: `register`)      → S_ThreadLocalInvalidCombination
//     (6.7.1p2 admits only static/extern beside thread_local), anchored at
//     the offending specifier token;
//   * BLOCK scope without `static`(-storage specifier) or `extern`
//     (6.7.1p3)                   → S_ThreadLocalRequiresStaticOrExtern.
//     The static test reads the SAME linkageSpecifiers staticStorage facet
//     the lowering routes on (scanSpecifierPrefixStorage), so the check and
//     the routing cannot disagree; the extern test is the record's
//     isExternDeclaration (block-scope extern objects do not parse in the
//     c-subset grammar today — the file-scope externDecl form is the only
//     extern surface — but the record test keeps the validator correct for
//     any grammar that admits them). File scope returns clean (static
//     storage is implicit — 6.2.4p3).
// (`typedef thread_local` cannot co-occur grammatically — typedefDecl has no
// storage-specifier prefix — so no Type-kind arm exists here; the parse
// error is the loud surface.)
void validateThreadLocalDeclarator(EngineState& s, SemanticConfig const& cfg,
                                   Tree const& tree, NodeId declNode,
                                   DeclarationRule const& decl,
                                   NodeId nameNode, SymbolId sym) {
    SymbolRecord const& rec = s.symbols.at(sym);
    TypeId const declTy = rec.type;
    auto const emit = [&](DiagnosticCode code, NodeId at, std::string what) {
        ParseDiagnostic d;
        d.code     = code;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(at);
        d.actual   = what.empty() ? std::string{tree.text(at)} : std::move(what);
        s.reporter.report(std::move(d));
    };
    if (!declTy.valid()) return;   // cascade — the type failure already reported
    TypeInterner const& in = s.lattice.interner();
    if (in.kind(declTy) == TypeKind::FnSig
        || rec.kind == DeclarationKind::Function) {
        emit(DiagnosticCode::S_ThreadLocalOnFunction, nameNode,
             std::format("'{}' — thread storage duration applies to objects "
                         "only, never functions (C 6.7.1p4)", rec.name));
        return;
    }
    if (rec.isConstexpr) {
        emit(DiagnosticCode::S_ThreadLocalInvalidCombination, nameNode,
             std::format("'{}' — a thread-storage specifier may not be "
                         "combined with 'constexpr' (C23 6.7.1)", rec.name));
        return;
    }
    if (NodeId const bad = firstPrefixTokenOfKinds(
            tree, declNode, decl, cfg.threadLocalIncompatibleTokens);
        bad.valid()) {
        emit(DiagnosticCode::S_ThreadLocalInvalidCombination, bad,
             std::format("'{}' may not be combined with a thread-storage "
                         "specifier (C 6.7.1p2 admits only 'static' or "
                         "'extern')", tree.text(bad)));
        return;
    }
    if (rec.isExternDeclaration) return;   // extern satisfies 6.7.1p3
    if (rec.scope.v == fileScopeOf(s, tree, rec.scope).v) return;  // file scope
    if (!scanSpecifierPrefixStorage(tree, declNode, decl).staticStorage) {
        emit(DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern, nameNode,
             std::format("'{}' — a block-scope thread_local object requires "
                         "'static' or 'extern' (C 6.7.1p3)", rec.name));
    }
}

// VLA C1a (D-CSUBSET-VLA, C11 §6.7.6.2p1): kinds a VLA SIZE expression may have —
// integer type (the standard integers, Bool, Char, Byte, Enum, and `_BitInt`; a
// `_BitInt(N)` bound is a legal VLA size). A float / nullptr_t / pointer / any other
// kind is rejected (S_VlaSizeNotInteger). Mirrors mapCast's integer set + Enum +
// BitInt (both integer-compatible at array-index / arithmetic sites).
[[nodiscard]] constexpr bool isVlaSizeIntegerType(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::Bool:
        case TypeKind::I8:  case TypeKind::I16: case TypeKind::I32:
        case TypeKind::I64: case TypeKind::I128:
        case TypeKind::U8:  case TypeKind::U16: case TypeKind::U32:
        case TypeKind::U64: case TypeKind::U128:
        case TypeKind::Char: case TypeKind::Byte:
        case TypeKind::Enum: case TypeKind::BitInt:
            return true;
        default:
            return false;
    }
}

// VLA C1a/C3 (D-CSUBSET-VLA): the SIZE-expression CST node of EVERY array suffix of a
// (possibly multi-dimensional) variable-length array declarator — each is the child
// BETWEEN that suffix's bracket delimiters (`arrayDeclSuffix = [ BracketOpen, expr,
// BracketClose ]`). Ordered pre-order scan (children left-to-right = SOURCE order =
// outer→inner) collecting one middle child per array-suffix node; a suffix node is
// NOT descended into (its only children are `[`, the expr, `]`). Empty if none. C3
// returns MULTIPLE nodes (`int a[n][m]` → [n, m]) so the validator can check each
// dim's bound type independently; a 1-D VLA returns exactly one.
[[nodiscard]] std::vector<NodeId>
vlaLengthNodes(Tree const& tree, NodeId declaratorNode, DeclaratorConfig const& dc) {
    std::vector<NodeId> out;
    // VLA C4c (D-CSUBSET-VLA): route through the shared bound locator so a
    // multi-dim VLA param whose inner dim carries an array-parameter decoration
    // (`int a[n][static m]`) reads the REAL bound `m`, NOT the leading `static`
    // token — a mis-located bound would query the wrong node's type in the
    // Pass-2 non-integer-size check.
    auto middleOf = [&](NodeId suffix) -> NodeId {
        return arraySuffixBoundNode(tree, suffix, dc.arraySuffixModifierTokens)
            .value_or(NodeId{});
    };
    // Explicit work-stack pre-order that preserves sibling SOURCE order: push
    // children in REVERSE so they pop left-to-right. An array-suffix node is
    // recorded (its middle child) and NOT descended into.
    std::vector<NodeId> stack{declaratorNode};
    for (int guard = 0; guard < 16384 && !stack.empty(); ++guard) {
        NodeId const c = stack.back(); stack.pop_back();
        if (tree.kind(c) != NodeKind::Internal) continue;
        if (tree.rule(c).v == dc.arraySuffixRule.v) { out.push_back(middleOf(c)); continue; }
        auto const kids = visibleChildren(tree, c);
        for (std::size_t i = kids.size(); i-- > 0;) stack.push_back(kids[i]);
    }
    return out;
}

// VLA C1a (D-CSUBSET-VLA, C99/C11 §6.7.6.2): the Pass-2 VLA constraint validator
// (the validateThreadLocalDeclarator model — `declNode` + `declaratorNode` + `decl`
// + `rec` in hand, runs AFTER expression typing). The type arm (applyDeclarator-
// Suffix / applyArraySuffix) builds the `vlaArray` REGARDLESS of scope/storage/size-
// type — the specifier prefix + the length's resolved type are not in hand at pure
// type-construction time — so THIS validator owns all three constraints, in order:
//   * FILE scope (`rec.scope` == file scope) → S_NonConstantArrayLength (a VLA needs
//     AUTOMATIC storage; the type-construction scope is a descendant of the file
//     scope even for a file-scope decl, so only `rec.scope` discriminates reliably —
//     the SAME test the thread_local validator uses);
//   * non-integer SIZE type (C 6.7.6.2p1) → S_VlaSizeNotInteger — a float / nullptr /
//     pointer bound. THIS is the ONLY tier that can catch `nullptr` (it lowers to an
//     I32 0 by MIR, so a MIR-tier integer check sees an integer);
//   * BLOCK scope with non-automatic storage (`static`/`extern`) → S_VlaWithStatic-
//     Storage (IMPORTANT-1; the static test reads the SAME staticStorage facet the
//     D-CSUBSET-LOCAL-STATIC lowering routes on; the extern test is the record flag).
// The caller gates on `isVlaArray(rec.type)` — only a RESOLVED VLA reaches here.
void validateVlaDeclarator(EngineState& s, SemanticConfig const& cfg,
                           Tree const& tree, NodeId declNode, NodeId declaratorNode,
                           DeclarationRule const& decl, NodeId nameNode,
                           SymbolId sym) {
    SymbolRecord const& rec = s.symbols.at(sym);
    auto const emit = [&](DiagnosticCode code, std::string what) {
        ParseDiagnostic d;
        d.code     = code;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(nameNode);
        d.actual   = std::move(what);
        s.reporter.report(std::move(d));
    };
    // FILE scope — a file-scope array requires a CONSTANT bound (S_NonConstantArray-
    // Length — the file-scope twin of the pre-VLA behavior). See the header comment.
    if (rec.scope.v == fileScopeOf(s, tree, rec.scope).v) {
        emit(DiagnosticCode::S_NonConstantArrayLength,
             std::format("'{}' — a file-scope array requires a constant length; a "
                         "variable-length array has automatic storage duration only "
                         "(C 6.7.6.2p2)", rec.name));
        return;
    }
    // NON-INTEGER size (C 6.7.6.2p1) — locate the length expr(s) under THIS declarator +
    // query each RESOLVED type (available now, post-typing). A float / nullptr_t /
    // pointer bound is illegal C. `nullptr` is caught HERE (semantic-tier NullptrT)
    // and nowhere downstream (it is I32 0 by MIR). An unresolved length type (the
    // length itself already failed loud) skips this — no cascade. VLA C3
    // (IMPORTANT-7): a MULTI-DIM VLA has one bound per suffix; EVERY dim is checked,
    // so `int a[n][3.5f]` is rejected on the second dim, not silently accepted.
    if (cfg.declarators.has_value()) {
        for (NodeId const lenNode :
                 vlaLengthNodes(tree, declaratorNode, *cfg.declarators)) {
            if (!lenNode.valid()) continue;
            TypeId const lenTy = subtreeType(s, tree, lenNode, rec.scope);
            if (lenTy.valid()
                && !isVlaSizeIntegerType(s.lattice.interner().kind(lenTy))) {
                emit(DiagnosticCode::S_VlaSizeNotInteger,
                     std::format("'{}' — a variable-length array size must have "
                                 "integer type (C 6.7.6.2p1)", rec.name));
                return;
            }
        }
    }
    // BLOCK scope but non-automatic storage (`static`/`extern`) — IMPORTANT-1.
    bool const isExtern = rec.isExternDeclaration;
    bool const isStatic =
        scanSpecifierPrefixStorage(tree, declNode, decl).staticStorage;
    if (!isExtern && !isStatic) return;   // automatic storage — a legal VLA
    emit(DiagnosticCode::S_VlaWithStaticStorage,
         std::format("'{}' — a variable-length array requires automatic storage "
                     "duration; it may not be declared '{}' (C 6.7.6.2p2)",
                     rec.name, isExtern ? "extern" : "static"));
}

// D-CSUBSET-FN-PROTOTYPE + D-CSUBSET-EXTERN-DEFINITION-MERGE + c33
// D-CSUBSET-TENTATIVE-DEFINITION: resolve a same-scope REDECLARATION (`prior`
// already bound `name` in `bindScope`; `newId` is the new symbol). A redeclaration
// MERGES instead of colliding when both sides name the SAME entity (both functions
// OR both objects) and are NOT both definitions. A NON-DEFINING declaration — a bare
// prototype, an `extern` declaration, OR a file-scope TENTATIVE object definition
// (`int g;` with no initializer, C 6.9.2) — names a body/storage that lives
// elsewhere, later, or is supplied by some/none of the merged declarations. The
// cases, on whether each side is non-defining:
//   nonDefining → definition : the DEFINITION wins the binding; the non-defining
//                 decl is absorbed (proto→def, extern→def, tentative→def).
//   definition → nonDefining / nonDefining → nonDefining : a redundant/tentative
//                 declaration; KEEP the prior binding, absorb the new one (def→proto,
//                 def→extern, proto→proto, extern→extern, proto↔extern,
//                 def→tentative, tentative→tentative). For two tentatives the kept
//                 prior is the single surviving object — it lowers to ONE zero-init
//                 global (C 6.9.2); the absorbed tentative emits no HIR node.
//   definition → definition : two bodies / two REAL (initialized) object definitions
//                 → S_RedeclaredSymbol. (`int g=1; int g=2;` collides; `int g; int
//                 g=5;` does NOT — the tentative is non-defining.)
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
    // c33 (D-CSUBSET-TENTATIVE-DEFINITION): a file-scope tentative object definition
    // is non-defining for merge purposes too — so tentative+def merges (the def
    // wins) and tentative+tentative merges (one survives). Two REAL definitions stay
    // a collision: both lack `isTentativeDefinition` ⇒ `bothDefinitions` ⇒ S0002.
    bool const priorNonDef = priorRec.isProtoDeclaration
                             || priorRec.isExternDeclaration
                             || priorRec.isTentativeDefinition;
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
        // TLS C1 (D-CSUBSET-THREAD-LOCAL, C11 6.7.1p3): a thread-storage
        // specifier "shall be present in the declaration of every declared
        // name with thread storage duration" — an OBJECT merge pair that
        // DISAGREES on isThreadLocal (`extern int g;` then `thread_local int
        // g = 5;`, or the reverse) fails loud in BOTH directions. Objects
        // only: protos map to Function via category() and functions are
        // rejected upstream (S_ThreadLocalOnFunction); Type/Table records
        // can never carry the flag (their rules have no storage-specifier
        // prefix). The merge still proceeds below — the error already gates
        // compilation, and keeping the binding intact avoids diagnostic
        // cascades on later uses.
        if (category(priorRec) == DeclarationKind::Variable
            && priorRec.isThreadLocal != s.symbols.at(newId).isThreadLocal) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::S_ThreadLocalRedeclarationMismatch;
            d.severity = DiagnosticSeverity::Error;
            d.buffer   = tree.source().id();
            d.span     = tree.span(nameNode);
            d.actual   = std::format(
                "'{}' — this declaration {} thread_local but a prior "
                "declaration of the same name {} (C 6.7.1p3 requires the "
                "specifier on every declaration)", name,
                s.symbols.at(newId).isThreadLocal ? "is" : "is NOT",
                priorRec.isThreadLocal ? "is" : "is not");
            if (priorRec.tree.v == tree.id().v) {
                d.related.push_back(RelatedLocation{
                    tree.source().id(),
                    tree.span(priorRec.declNode),
                    "previously declared here",
                });
            }
            s.reporter.report(std::move(d));
        }
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
        // Two real (initialized) definitions, or a cross-category (function vs
        // object) redeclaration — a genuine collision.
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

        // c32 D-CSUBSET-FNPTR-PARAM-SCOPE: the scope-open decision is the SHARED
        // `nodeOpensChildScope` predicate (a `scopes`-rule definition OR a
        // per-declarator prototype param list) so Pass 1.5 / Pass 2 re-derive the
        // SAME scope by anchor lookup. (Folds the former
        // `declIsDefiningHere && scopeByRule.contains` test — `declIsDefiningHere`
        // is still read by the mint guard below.)
        if (nodeOpensChildScope(s, cfg, tree, node)) {
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
                        // c33 (D-CSUBSET-TENTATIVE-DEFINITION): a FILE-SCOPE object
                        // declaration with NO initializer is a TENTATIVE DEFINITION
                        // (C 6.9.2) — it announces an object whose single definition
                        // merges across all its tentative declarations + at most one
                        // real (initialized) definition. Treat it like a non-defining
                        // declaration (extern / proto) so the merge direction table
                        // below folds it: tentative+def → the def wins (keeps its
                        // init); tentative+tentative → one zero-init object; the
                        // real-def+real-def case stays a COLLISION (both carry an
                        // initializer ⇒ both defining). Conditions, ALL required:
                        //   (1) a Variable (NOT a function/type; NOT a proto/extern —
                        //       isProto/isExtern carry those, and a proto is
                        //       Variable-kind pre-upgrade so it is excluded here);
                        //   (2) an OBJECT-declaration row — one that declares a
                        //       declarator LIST (`declaratorListChild`), as
                        //       file-scope object declarations do. A PARAMETER row
                        //       uses a SINGLE `declaratorChild` and is EXCLUDED: a
                        //       prototype param (`int f(int s); int g(int s);`, or
                        //       two redundant `extern` protos sharing a param name)
                        //       has no linkage and is never a tentative definition —
                        //       it must NOT merge across declarators;
                        //   (3) FILE scope — the bind scope IS this tree's file/CU
                        //       scope (C 6.9.2 is file-scope only): a block-scope
                        //       `int x; int x;` and a duplicate struct/union MEMBER
                        //       (bound in a body scope) MUST stay errors;
                        //   (4) NO initializer on this declarator (a var WITH one is
                        //       a real definition, never tentative).
                        bool const isTentativeDefinition =
                            (effectiveKind == DeclarationKind::Variable)
                            && !isProto
                            && !decl.nonDefiningDeclaration
                            && decl.declaratorListChild.has_value()
                            && bindScope == fileScopeOf(s, tree, current)
                            && cfg.declarators.has_value()
                            && !declaratorHasInitializer(tree, *cfg.declarators,
                                                         dNode);
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
                            // c36 (D-CSUBSET-MUTABLE-POINTER-TO-CONST): the
                            // OBJECT's const-ness — a `const char *p` pointer is
                            // MUTABLE (only `char * const p` is a const object),
                            // NOT a coarse whole-decl const scan. See
                            // declaratorObjectIsConst. Falls back to the legacy
                            // scan only if this language has no declarator config.
                            // D-CSUBSET-TYPEOF: the head's `typeof(...)` operand is
                            // OPAQUE to the const scan — a literal `const` inside a
                            // `typeof(const int)` head must NOT mark the object const
                            // (typeof preserves it in the resolved TYPE; typeof_unqual
                            // strips it — either way the coarse token must not leak).
                            std::array<RuleId, 2> const typeofOpaqueRules{
                                cfg.typeofTypeRule, cfg.typeofValueRule};
                            rec.isConst = cfg.declarators.has_value()
                                ? declaratorObjectIsConst(
                                      tree, node, dNode, *cfg.declarators,
                                      *decl.constMarker, &s.idx().declByRule,
                                      (decl.headChild.has_value()
                                       && *decl.headChild < kids.size())
                                          ? kids[*decl.headChild] : NodeId{},
                                      typeofOpaqueRules)
                                : subtreeContainsToken(
                                      tree, node, *decl.constMarker,
                                      &s.idx().declByRule);
                        }
                        // c27 (D-CSUBSET-VOLATILE-POINTEE): object-volatility is now
                        // derived from the symbol's resolved TYPE (top-level
                        // VolatileQual) at access lowering — the c21 coarse
                        // `isVolatile` token-scan is RETIRED here (it mis-fired for a
                        // pointer-to-volatile-POINTEE, marking the pointer OBJECT
                        // volatile when only the pointee is). `isConst` keeps its scan.
                        // FC17 (D-CSUBSET-CONSTEXPR): a `constexpr` specifier in the
                        // declaration's prefix marks EVERY declarator's symbol
                        // (C23 6.7.1 applies the storage-class to each declared
                        // object) and IMPLIES const (6.7.1p10 — a constexpr object
                        // is not assignable; setting isConst here routes it through
                        // the existing const-violation check + const-symbol init
                        // folding uniformly). No typeof/alignas opacity hazard: the
                        // keyword cannot parse inside those operands, so the prefix
                        // scan (`specifierPrefixHasConstexpr`) never false-hits.
                        // Pass 2's validateConstexprDeclarator enforces 6.7.1.
                        if (specifierPrefixHasConstexpr(cfg, tree, node, decl)) {
                            rec.isConstexpr = true;
                            rec.isConst     = true;
                        }
                        // TLS C1 (D-CSUBSET-THREAD-LOCAL): a thread-storage
                        // specifier in the prefix (a linkageSpecifiers entry
                        // with {threadStorage:true} — the SAME facet the HIR
                        // linkage fold walks) marks EVERY declarator's symbol
                        // (C 6.7.1 applies the storage-class to each declared
                        // object). Pass 2's validateThreadLocalDeclarator
                        // enforces the 6.7.1 constraints; the redeclaration
                        // merge rejects a same-TU mismatch (6.7.1p3).
                        if (scanSpecifierPrefixStorage(tree, node, decl)
                                .threadStorage) {
                            rec.isThreadLocal = true;
                        }
                        rec.isProtoDeclaration = isProto;
                        // D-CSUBSET-EXTERN-DEFINITION-MERGE: a non-defining
                        // declaration (c-subset's `extern`) — config-driven, no
                        // rule-name identity. Like a prototype it is a non-defining
                        // declaration that MERGES with an in-TU definition.
                        bool const isExtern = decl.nonDefiningDeclaration;
                        rec.isExternDeclaration = isExtern;
                        // c33 (D-CSUBSET-TENTATIVE-DEFINITION): record the tentative
                        // state so a LATER redeclaration sees THIS symbol (as `prior`)
                        // as non-defining (`priorNonDef`) and merges — a tentative is
                        // mergeable with both a real definition and another tentative.
                        rec.isTentativeDefinition = isTentativeDefinition;
                        SymbolId const newId = s.symbols.mint(rec);
                        boundNamed = true;
                        SymbolId const prior =
                            s.scopes.bind(bindScope, name, newId);
                        if (prior.valid()) {
                            // The new decl's category (Function via Function-kind or a
                            // bare proto, else Variable/Type/Table) is read from its
                            // record inside the helper. A real function definition is
                            // Function-kind and non-`isExtern`. c33: a file-scope
                            // tentative object definition is ALSO non-defining
                            // (mergeable) — see `isTentativeDefinition` above.
                            mergeOrCollideRedeclaration(
                                s, tree, bindScope, name, nameNode, prior, newId,
                                /*newNonDef=*/isProto || isExtern
                                              || isTentativeDefinition);
                        } else {
                            s.nodeToSymbol.set(nameNode, newId);
                        }
                        // FC17.5 (D-CSUBSET-FUNC-PREDEFINED-IDENTIFIER, C99
                        // 6.4.2.2): a function DEFINITION (Function-kind, its
                        // decl opened the param/body scope `here`) binds one
                        // synthetic predefined function-name symbol per
                        // configured spelling (`__func__`, `__FUNCTION__`) into
                        // `here` — BEFORE the driver walks the children, so the
                        // params bind AFTER it and a param named `__func__`
                        // collides at its OWN bind (S_RedeclaredSymbol at the
                        // param's span, "previously declared here" = the
                        // function's name). Kind=Variable + isConst (SE4's
                        // const check catches `__func__ = x` / `+=` →
                        // S_ConstViolation); type = Array<narrow-string-core,
                        // len+1> minted HERE (no CST declarator exists for
                        // Pass 1.5 to resolve) with the element core from the
                        // language's config-declared string-literal core (the
                        // SAME source string literals type from — a language
                        // with NO string-literal core configured skips the
                        // bind, and a `__func__` use then fails loud as an
                        // ordinary unresolved identifier, never a guessed
                        // type). NO nodeToSymbol entry — the function's name
                        // node keeps ITS binding; uses resolve via the normal
                        // Pass-2 scope lookup. A prototype (Variable-kind,
                        // no body) never reaches here: effectiveKind gates it.
                        if (effectiveKind == DeclarationKind::Function
                            && here.v != current.v
                            && !cfg.predefinedFunctionNameIdentifiers.empty()) {
                            TypeKind const fnNameCore =
                                s.idx().stringLiteralElementCore;
                            if (fnNameCore != TypeKind::Void) {
                                auto& in = s.lattice.interner();
                                TypeId const fnNameArrTy = in.array(
                                    in.primitive(fnNameCore),
                                    static_cast<std::int64_t>(name.size() + 1));
                                for (std::string const& spelling :
                                     cfg.predefinedFunctionNameIdentifiers) {
                                    SymbolRecord frec;
                                    frec.name         = spelling;
                                    frec.scope        = here;
                                    frec.declNode     = nameNode;
                                    frec.declRuleNode = node;
                                    frec.tree         = tree.id();
                                    frec.kind         = DeclarationKind::Variable;
                                    frec.type         = fnNameArrTy;
                                    frec.isConst      = true;   // F1: SE4 catches writes
                                    frec.isPredefinedFunctionName    = true;
                                    frec.predefinedFunctionNameText  = name;
                                    SymbolId const fid = s.symbols.mint(frec);
                                    // The scope was pushed by THIS node — the
                                    // only earlier binds are other spellings of
                                    // this same loop; config duplicates are
                                    // harmless (first bind wins).
                                    (void)s.scopes.bind(here, spelling, fid);
                                }
                            }
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
                            // c36 (D-CSUBSET-MUTABLE-POINTER-TO-CONST): an
                            // ANONYMOUS field has NO declarator (no pointer layer
                            // to redirect const onto a pointee) and is NEVER an
                            // assignment LHS, so its isConst is never read at the
                            // const-violation check — the coarse whole-node scan
                            // is harmless here (`int *;` is already rejected as
                            // declares-nothing). Left as-is by design.
                            rec.isConst = subtreeContainsToken(
                                tree, node, *decl.constMarker,
                                &s.idx().declByRule);
                        }
                        // c27: object-volatility derived from the field's resolved
                        // TYPE (top-level VolatileQual) at access lowering — the c21
                        // coarse token-scan is retired (see the named-declarator site).
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
                    // c36 (D-CSUBSET-MUTABLE-POINTER-TO-CONST): in c-subset NO
                    // positional-name row sets constMarker (the const-bearing
                    // rows — varDecl/identVarDecl/forDecl/forIdentDecl/param/
                    // topLevelDecl — all take the declarator-mode path above,
                    // which uses declaratorObjectIsConst), so this coarse scan is
                    // currently unreached. A future positional-name language that
                    // sets constMarker WITH pointer types must adopt
                    // declaratorObjectIsConst here too (else `const T *p` here
                    // would wrongly mark the pointer object const).
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
                    // c27 (D-CSUBSET-VOLATILE-POINTEE): object-volatility is derived
                    // from the symbol's resolved TYPE (top-level VolatileQual) at
                    // access lowering — the c21 coarse volatile token-scan is retired
                    // here (it could not distinguish a volatile OBJECT from a
                    // volatile POINTEE). `isConst` keeps its scan above.

                    // D-CSUBSET-EXTERN-DEFINITION-MERGE: c-subset's `extern`
                    // declaration mints through THIS legacy positional path (it
                    // declares a positional `name`, not declarator-mode). Mark it
                    // non-defining so it merges with an in-TU definition of the same
                    // name (handled below via the shared mergeOrCollideRedeclaration).
                    rec.isExternDeclaration = decl.nonDefiningDeclaration;
                    // TLS C1 (D-CSUBSET-THREAD-LOCAL): the positional-path mirror
                    // of the declarator-mode thread-storage mint — c-subset's
                    // `extern thread_local int e;` mints HERE (externDecl's
                    // externSpecifiers prefix carries the specifier). Same facet,
                    // same scan; Pass 2 + the merge own the consequences.
                    if (scanSpecifierPrefixStorage(tree, node, decl).threadStorage) {
                        rec.isThreadLocal = true;
                    }

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

// c34 (D-CSUBSET-ARRAY-SIZE-INFERENCE): the INITIALIZER node of an init-declarator
// `dNode` (`declarator ('=' init)?`), or InvalidNode if there is none. The init is
// the init-declarator's visible Internal child that is NOT the declarator itself —
// the SAME shape the HIR lowering reads (cst_to_hir's `lowerVarLikeInto` init scan).
// A plain `declarator` (no initDeclarator wrapper) or a structurally-bare
// declarator yields InvalidNode (no initializer to infer from). Config-driven:
// keyed on the resolved `initDeclaratorRule` / `declaratorRule` roles.
[[nodiscard]] NodeId
initializerNodeOf(Tree const& tree, NodeId dNode, DeclaratorConfig const& dc) {
    if (!dNode.valid() || tree.kind(dNode) != NodeKind::Internal) return {};
    if (tree.rule(dNode).v != dc.initDeclaratorRule.v) return {};
    for (NodeId c : visibleChildren(tree, dNode)) {
        if (tree.kind(c) != NodeKind::Internal) continue;
        if (tree.rule(c).v == dc.declaratorRule.v) continue;
        return c;   // the `= init` value subtree
    }
    return {};
}

// c34 (D-CSUBSET-ARRAY-SIZE-INFERENCE, C 6.7.9p22): complete an INCOMPLETE-ARRAY
// declared type from its initializer's shape. Returns the SIZED `Array<elem, N>`
// when `declTy` is `T[]` (incomplete) AND `initNode` is a recognizable initializer
// whose length we can derive EXACTLY; otherwise returns `declTy` unchanged (a
// non-array, an already-sized array, or an absent/unrecognized initializer — the
// caller keeps whatever the resolver produced, so a bare `[]` with no init stays
// incomplete and fails loud downstream, never silently sized).
//
//   * STRING-literal init (`char x[] = "abc"`): N = decoded body length + 1 (the
//     trailing NUL) — the EXACT count the per-occurrence string typer uses,
//     computed via the SHARED `decodeAdjacentStringBodies` chokepoint so escapes
//     count once (`"\x41\x42"` → 2 + NUL = 3). A malformed escape leaves `declTy`
//     incomplete (no guessed size; the HIR tier fails loud on the bad literal).
//   * BRACE init (`int a[] = {e0,e1,…}`): N = the number of TOP-LEVEL `initElement`
//     children of the brace list (C 6.7.9 positional element count).
//
// The single-occurrence completion site is the Pass-1.5 var/global declared-type
// stamp, so every downstream consumer (sizeof, layout, element access, BOTH HIR
// paths) observes the SAME sized array — there is no second place to keep in sync.
[[nodiscard]] TypeId
completeIncompleteArrayFromInit(EngineState& s, SchemaIndexes const& idx,
                                Tree const& tree, NodeId initNode, TypeId declTy) {
    TypeInterner& interner = s.lattice.interner();
    if (!declTy.valid() || !interner.isIncompleteArray(declTy)) return declTy;
    if (!initNode.valid()) return declTy;
    auto const ops = interner.operands(declTy);
    if (ops.empty() || !ops[0].valid()) return declTy;   // malformed — leave as-is
    TypeId const elem = ops[0];

    // An incomplete-array VARIABLE reaches this helper ONLY with an initializer
    // that must SIZE it (C 6.7.9). If the initializer cannot determine a positive
    // length — an EMPTY brace `T x[] = {}` (top-level count 0), or no string/brace
    // initializer at all — the array stays INCOMPLETE. Returned silently it flows
    // into the HIR/MIR tier, which has no incomplete-array guard, and LOOPS on the
    // -1 sentinel length. Fail LOUD here instead (an inferred 0-or-undeterminable
    // length is exactly the non-positive `int a[0]` case → S_ArrayLengthOutOfRange),
    // matching the no-initializer path's clean error rather than a compiler hang.
    auto failUnsized = [&]() -> TypeId {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::S_ArrayLengthOutOfRange;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(initNode);
        d.actual   = std::string{tree.text(initNode)};
        s.reporter.report(std::move(d));
        return InvalidType;
    };

    // Descend the initializer subtree to the FIRST string-literal expr OR brace
    // list (the init may sit under transparent wrappers — `initValue`, paren
    // groups). Both rule kinds are mutually exclusive at a given init position.
    std::vector<NodeId> stack{initNode};
    for (int guard = 0; guard < 8192 && !stack.empty(); ++guard) {
        NodeId c = stack.back(); stack.pop_back();
        if (tree.kind(c) != NodeKind::Internal) continue;
        RuleId const r = tree.rule(c);
        if (idx.stringLiteralExprRule.valid()
            && idx.stringLiteralBodyToken.valid()
            && r.v == idx.stringLiteralExprRule.v) {
            EscapeDecodeOutcome outcome;
            if (auto decoded = decodeAdjacentStringBodies(
                    tree, c, idx.stringLiteralBodyToken, &outcome)) {
                // C 6.7.9: the length is the code-unit count in the DECLARED
                // element's width. Narrow (`char buf[]="…"`) → byte count
                // (unchanged). A wide element (`wchar_t buf[]=L"…"`) re-encodes the
                // raw bytes so the count is code units — the SAME shared encoder
                // the literal's own type uses. A wide encode error stays incomplete
                // (fail loud later), exactly like a malformed escape.
                TypeKind const ek = interner.kind(elem);
                if (ek == TypeKind::Char || ek == TypeKind::Byte) {
                    return interner.array(
                        elem, static_cast<std::int64_t>(decoded->size() + 1));
                }
                // FF3 (D-CSUBSET-WIDE-HEX-OCTAL-ESCAPE-VALUE): a wide element sized
                // from a `\x`/octal byte escape stays INCOMPLETE (fail loud later),
                // like a malformed escape — the HIR tier emits the diagnostic.
                if (!outcome.usedByteEscape) {
                    WideEncodeResult enc;
                    if (!encodeWideString(*decoded, ek, enc)) {
                        return interner.array(
                            elem, static_cast<std::int64_t>(enc.codeUnits + 1));
                    }
                }
            }
            return declTy;   // malformed escape / wide encode error / wide byte escape — stay incomplete
        }
        if (idx.braceInitListRule.valid() && idx.initElementRule.valid()
            && r.v == idx.braceInitListRule.v) {
            std::int64_t count = 0;
            for (NodeId e : visibleChildren(tree, c)) {
                if (tree.kind(e) == NodeKind::Internal
                    && tree.rule(e).v == idx.initElementRule.v) {
                    ++count;
                }
            }
            if (count <= 0) return failUnsized();   // `T x[] = {}` — fail loud, no hang
            return interner.array(elem, count);
        }
        for (NodeId g : visibleChildren(tree, c)) stack.push_back(g);
    }
    return failUnsized();   // no string/brace initializer found — fail loud, no hang
}

// ── FC17.5 (D-CSUBSET-AUTO-TYPE-INFERENCE, C23 6.7.9): the Pass-1.5
//    initializer-inference arm ────────────────────────────────────────────────
// Computes the INFERRED declared type for an `inferTypeFromInitializer`
// declaration row (`auto x = expr;`) at the row's own DEFINITIVE Pass-1.5
// visit — so the inferred type is visible to later same-pass consumers
// (`auto x = 42; int arr[sizeof(x)];` dimensions at Pass 1.5; the Pass-2
// initializer BACKFILL alone would false-fail S_NonConstantArrayLength — the
// red-on-disable visibility pin). Returns the inferred type on success — the
// caller then runs the ORDINARY declarator fold with it as the head type, so
// every standard per-declarator behavior (symbol/type write + nodeToType
// stamps, alignas, attribute facts, incomplete-object check) rides the proven
// path unchanged. Returns InvalidType after emitting the specific
// UNSUPPRESSABLE diagnostic on every reject — the caller must then SKIP the
// fold (no type is written; the unsuppressable posture is load-bearing
// because Pass 2's decl arm backfills `rec.type = initializer-type` for any
// still-unresolved declarator-mode symbol, so a suppressed reject would
// silently adopt the initializer's type).
//
// The gate/normalization ladder (the design-audit's ★C1 + ★C3, in order):
//   1. ★C1 the specifier PRESENCE gate (`decl.requiredSpecifierToken`, C23
//      6.7.9p1's `auto`) — checked FIRST: `static x = 5;` / `register y = 2;`
//      / `alignas(4) z = 9;` / `[[maybe_unused]] w = 3;` all PARSE into the
//      headless rule and must stay errors (C89 implicit-int is not C23);
//   2. exactly ONE declarator (6.7.9p2) — S_AutoRequiresSingleDeclarator;
//   3. a PLAIN IDENTIFIER declarator (no pointer/array/function/group
//      structure; the derived forms are the WG14 v2-paper extension —
//      D-CSUBSET-AUTO-DERIVED-DECLARATOR) — S_AutoRequiresPlainIdentifier;
//   4. an initializer present — S_AutoRequiresInitializer;
//   5. the C23 braced SINGLE non-designated scalar form `auto x = {5};`
//      unwraps; `{}` / `{1,2}` / `{.x=1}` / `{{5}}` reject via the shared
//      S_InvalidScalarInitializer (0xE03F; even suppressed, the symbol stays
//      untyped → H_TypeUnresolved, never a silent accept);
//   6. PRE-STAMP the initializer's literal leaves through the shared
//      `typeLiteralIfAny` chokepoint (see its comment — at Pass 1.5 the
//      Pass-2 blanket stamps don't exist yet), then `subtreeType`;
//   7. ★C3 normalize: Array→Ptr<element> + FnSig→Ptr<FnSig> DECAY (C
//      6.3.2.1p3/p4 — `auto s = "str"` is char*, `auto f = fn` is a function
//      pointer); REJECT LOUD Void (no object type) / NullptrT (semantic-tier
//      -only — D-CSUBSET-NULLPTR-T-DECLARABLE) / invalid (incl. the
//      self-reference `auto x = x;`, whose name resolves to the symbol being
//      declared) — S_AutoInferenceInvalid; then stripVolatile (C23 6.7.9p2
//      drops top-level qualifiers; const never rides the type here — the row
//      declares no constMarker, see its $comment).
[[nodiscard]] TypeId
resolveAutoInferredDeclaration(EngineState& s, SemanticConfig const& cfg,
                               Tree const& tree, NodeId node,
                               DeclarationRule const& decl,
                               std::vector<NodeId> const& kids, ScopeId here) {
    auto const emit = [&](DiagnosticCode code, NodeId at, std::string what) {
        ParseDiagnostic d;
        d.code     = code;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree.source().id();
        d.span     = tree.span(at.valid() ? at : node);
        d.actual   = std::move(what);
        s.reporter.report(std::move(d));
    };
    auto const snippet = [&](NodeId at) {
        return std::string{tree.text(at.valid() ? at : node)};
    };

    // (1) ★C1 — the required-specifier presence gate. A bounded descendant
    // scan over the STRIPPED specifier prefix (the specifierPrefixHasConstexpr
    // idiom; the token kind comes from the ROW, never a hardcoded keyword).
    // The prefix is the only place the specifier can parse, so prefix-scoped
    // matching is exact — an initializer cannot contain the keyword.
    if (decl.requiredSpecifierToken.has_value()) {
        bool found = false;
        NodeId const prefix = specifierPrefixChild(tree, node, decl);
        if (prefix.valid()) {
            std::vector<NodeId> stack{prefix};
            for (int guard = 0; guard < 8192 && !stack.empty(); ++guard) {
                NodeId c = stack.back(); stack.pop_back();
                if (tree.kind(c) == NodeKind::Internal) {
                    for (NodeId g : visibleChildren(tree, c)) stack.push_back(g);
                    continue;
                }
                if (tree.tokenKind(c).v == decl.requiredSpecifierToken->v) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            emit(DiagnosticCode::S_AutoInferenceInvalid, node,
                 "initializer-inferred declaration requires the language's "
                 "type-inference specifier in its declaration specifiers "
                 "(a declaration with no type specifier is not implicit-int): "
                     + snippet(node));
            return InvalidType;
        }
    }
    if (!cfg.declarators.has_value()) return InvalidType;   // loader invariant
    DeclaratorConfig const& dc = *cfg.declarators;

    // (2) exactly ONE declarator (C23 6.7.9p2).
    auto const carrierIdx = decl.declaratorListChild.has_value()
                                ? decl.declaratorListChild
                                : decl.declaratorChild;
    std::vector<NodeId> declarators;
    if (carrierIdx.has_value() && *carrierIdx < kids.size()) {
        collectDeclarators(tree, kids[*carrierIdx], dc, declarators);
    }
    if (declarators.size() != 1) {
        emit(DiagnosticCode::S_AutoRequiresSingleDeclarator,
             declarators.size() > 1 ? declarators[1] : node,
             "initializer-inferred declaration declares "
                 + std::to_string(declarators.size())
                 + " declarators — exactly one is required (C23 6.7.9p2)");
        return InvalidType;
    }
    NodeId const dNode = declarators[0];

    // (3) a PLAIN IDENTIFIER declarator: the (init)declarator's inner
    // `declarator` must carry NO pointerLayer, and its `directDeclarator`
    // must be a bare name token — no fnSuffix / arrayDeclSuffix /
    // parenDeclarator (all Internal children). Structure per the config
    // roles only, never rule-name identities.
    NodeId const declNode =
        (tree.kind(dNode) == NodeKind::Internal
         && tree.rule(dNode) == dc.declaratorRule)
            ? dNode
            : declarator_walk_detail::firstChildOfRule(
                  TreeDeclaratorView{tree}, dNode, dc.declaratorRule);
    bool plain = declNode.valid();
    NodeId direct{};
    if (plain) {
        for (NodeId c : visibleChildren(tree, declNode)) {
            if (tree.kind(c) != NodeKind::Internal) continue;
            if (tree.rule(c) == dc.directRule && !direct.valid()) {
                direct = c;
                continue;
            }
            plain = false;   // a pointerLayer (or any non-direct structure)
            break;
        }
        if (!direct.valid()) plain = false;
    }
    if (plain) {
        bool haveName = false;
        for (NodeId c : visibleChildren(tree, direct)) {
            if (tree.kind(c) == NodeKind::Internal) {
                plain = false;   // fnSuffix / arrayDeclSuffix / parenDeclarator
                break;
            }
            if (tree.tokenKind(c) == dc.nameToken) haveName = true;
        }
        plain = plain && haveName;
    }
    if (!plain) {
        emit(DiagnosticCode::S_AutoRequiresPlainIdentifier, dNode,
             "initializer-inferred declaration requires a plain identifier "
             "declarator (pointer/array/function declarators are the "
             "derived-declarator extension): " + snippet(dNode));
        return InvalidType;
    }

    // (4) an initializer present — the init subtree is the Internal child of
    // the init-declarator that is NOT the declarator (the Pass-2 loop's own
    // discovery idiom, `[declarator, '=', initValue]`).
    NodeId initNode{};
    if (tree.rule(dNode) == dc.initDeclaratorRule) {
        for (NodeId c : visibleChildren(tree, dNode)) {
            if (tree.kind(c) != NodeKind::Internal) continue;
            if (tree.rule(c) == dc.declaratorRule) continue;
            initNode = c;
            break;
        }
    }
    if (!initNode.valid()) {
        emit(DiagnosticCode::S_AutoRequiresInitializer, dNode,
             "initializer-inferred declaration requires an initializer "
             "(there is no type to infer from): " + snippet(dNode));
        return InvalidType;
    }

    // (5) the C23 braced SINGLE non-designated scalar form (`auto x = {5};`,
    // 6.7.9p2 via 6.7.10p12). Bounded single-Internal-child descent to a
    // braceInitList (the validateConstexprDeclarator F3 pattern); inside it,
    // exactly ONE initElement whose sole Internal child is the value (≥2 ⇒ a
    // designated form) and whose value is not itself a nested brace list.
    NodeId valueNode = initNode;
    if (s.idx().braceInitListRule.valid()) {
        auto const descendToBrace = [&](NodeId from) -> NodeId {
            NodeId walk = from;
            for (int guard = 0; guard < 64 && walk.valid(); ++guard) {
                if (tree.kind(walk) != NodeKind::Internal) return NodeId{};
                if (tree.rule(walk).v == s.idx().braceInitListRule.v)
                    return walk;
                NodeId sole{};
                bool   multiple = false;
                for (NodeId c : visibleChildren(tree, walk)) {
                    if (tree.kind(c) != NodeKind::Internal) continue;
                    if (sole.valid()) { multiple = true; break; }
                    sole = c;
                }
                if (multiple || !sole.valid()) return NodeId{};
                walk = sole;
            }
            return NodeId{};
        };
        if (NodeId const brace = descendToBrace(initNode); brace.valid()) {
            std::vector<NodeId> elems;
            for (NodeId c : visibleChildren(tree, brace)) {
                if (tree.kind(c) != NodeKind::Internal) continue;
                if (!s.idx().initElementRule.valid()
                    || tree.rule(c).v == s.idx().initElementRule.v) {
                    elems.push_back(c);
                }
            }
            NodeId inner{};
            bool   braceOk = elems.size() == 1;
            if (braceOk) {
                int internals = 0;
                for (NodeId c : visibleChildren(tree, elems[0])) {
                    if (tree.kind(c) != NodeKind::Internal) continue;
                    ++internals;
                    inner = c;
                }
                braceOk = internals == 1;   // ≥2 ⇒ designator(s) + value
            }
            if (braceOk && descendToBrace(inner).valid()) {
                braceOk = false;   // nested `{{5}}` — not a single expression
            }
            if (!braceOk) {
                // The shared scalar-brace constraint code (0xE03F). Even
                // suppressed there is no silent accept: the arm returns
                // InvalidType, the symbol stays untyped (the Pass-2 backfill
                // cannot type a multi-element brace list), and HIR fails loud
                // H_TypeUnresolved.
                emit(DiagnosticCode::S_InvalidScalarInitializer, brace,
                     "initializer-inferred declaration takes exactly ONE "
                     "non-designated, non-nested braced expression "
                     "(C23 6.7.10p12): " + snippet(brace));
                return InvalidType;
            }
            valueNode = inner;
        }
    }

    // (6) PRE-STAMP the initializer subtree's literal leaves (bounded walk
    // through the ONE shared literal-typing chokepoint — see typeLiteralIfAny;
    // idempotent, and identical to what the Pass-2 blanket walk will stamp).
    // A subtree beyond the guard leaves some literals unstamped → the
    // inference below fails LOUD (never a silently-wrong type).
    {
        std::vector<NodeId> stack{valueNode};
        for (int guard = 0; guard < 65536 && !stack.empty(); ++guard) {
            NodeId c = stack.back(); stack.pop_back();
            if (s.typeAt(c).valid()) continue;   // already stamped
            typeLiteralIfAny(s, cfg, tree, c);
            if (tree.kind(c) != NodeKind::Internal) continue;
            // A rule-node stamp (the adjacent-string form) closes the subtree
            // — mirror subtreeType's typeAt short-circuit, don't descend.
            if (s.typeAt(c).valid()) continue;
            for (NodeId g : visibleChildren(tree, c)) stack.push_back(g);
        }
    }
    TypeId inferred = subtreeType(s, tree, valueNode, here);

    // (7) ★C3 — normalize.
    TypeInterner& in = s.lattice.interner();
    if (!inferred.valid()) {
        emit(DiagnosticCode::S_AutoInferenceInvalid, initNode,
             "initializer-inferred declaration: the initializer's type "
             "cannot be resolved at the declaration itself (a "
             "self-referential or unresolvable initializer): "
                 + snippet(initNode));
        return InvalidType;
    }
    switch (in.kind(inferred)) {
        case TypeKind::Void:
            emit(DiagnosticCode::S_AutoInferenceInvalid, initNode,
                 "initializer-inferred declaration: the initializer has "
                 "void type — there is no object type to declare: "
                     + snippet(initNode));
            return InvalidType;
        case TypeKind::NullptrT:
            // The predefined null-pointer constant's type is semantic-tier
            // -only (it must never reach MIR — the 0xA014 tripwire); an
            // object OF that type is the named deferral
            // D-CSUBSET-NULLPTR-T-DECLARABLE.
            emit(DiagnosticCode::S_AutoInferenceInvalid, initNode,
                 "initializer-inferred declaration: the null-pointer "
                 "constant's type is not a declarable object type here "
                 "(initialize a concrete pointer type instead): "
                     + snippet(initNode));
            return InvalidType;
        case TypeKind::Array: {
            // C 6.3.2.1p3 array-to-pointer decay: `auto s = "str";` is
            // char* (the ternary decayArray pattern).
            auto const elems = in.operands(inferred);
            if (elems.empty() || !elems[0].valid()) {
                emit(DiagnosticCode::S_AutoInferenceInvalid, initNode,
                     "initializer-inferred declaration: the initializer's "
                     "array type has no element type (interner invariant): "
                         + snippet(initNode));
                return InvalidType;
            }
            inferred = in.pointer(elems[0]);
            break;
        }
        case TypeKind::FnSig:
            // C 6.3.2.1p4 function-to-pointer decay: `auto f = fn;` is a
            // pointer-to-function (the c56 precedent).
            inferred = in.pointer(inferred);
            break;
        default:
            break;
    }
    // C23 6.7.9p2: the declared type drops top-level qualifiers (the
    // typeof_unqual strip; only VolatileQual is interned — const is
    // symbol-level and never set here, the row declares no constMarker).
    return in.stripVolatile(inferred);
}

// ── Pass 1.5: post-order — resolve declaration types + FnSigs ──────────────
// D-PARSE-DEEP-NEST-RECURSION-MEMORY (plan 24 Stage 2, missed site): the POST-
// CHILD work for ONE node — extracted for the iterative `resolveDeclTypes` driver
// below (mirrors the pass2 / pass2Post split). Runs AFTER all descendants are
// walked; `here` is this node's OWN resolved child scope (threaded by the driver,
// identical to what the recursion passed each child). A `return;` here ends THIS
// node's post-work and the driver loop continues — exactly as the recursive
// `return;` did. The body is byte-identical to the prior post-child portion of
// resolveDeclTypes; it reads only `here`/`node`/`s`/`cfg`/`tree` (never the
// pre-part's `current`, which the recursion consumed ONLY to derive `here`).
void resolveDeclTypesPost(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                          NodeId node, ScopeId here) {
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
                // FC17.5 (D-CSUBSET-AUTO-TYPE-INFERENCE): an
                // `inferTypeFromInitializer` row has NO head — the declared
                // type is INFERRED from the sole declarator's initializer
                // (see resolveAutoInferredDeclaration above). On success the
                // inferred type plays the head's role and the ORDINARY fold
                // below applies it (type write + stamps + alignas +
                // attribute facts + incomplete-object check — one proven
                // path, no parallel write site). On a reject the fold is
                // SKIPPED: no type is written (the emitted diagnostics are
                // unsuppressable, so the Pass-2 initializer backfill can
                // never silently resurrect the declaration).
                bool inferenceRejected = false;
                if (decl.inferTypeFromInitializer) {
                    headTy = resolveAutoInferredDeclaration(
                        s, cfg, tree, node, decl, kids, here);
                    inferenceRejected = !headTy.valid();
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
                if (!inferenceRejected
                    && carrierIdx.has_value() && *carrierIdx < kids.size()) {
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
                    // c27 (D-CSUBSET-VOLATILE-POINTEE): the c21 pointer-to-volatile-
                    // POINTEE REJECTS (the typedef arm + the per-declarator arm that
                    // fired on a head volatile + a declarator star) are GONE. A head
                    // `volatile <base>` now resolves to VolatileQual(base) in
                    // `headTy` (resolveTypeNode → the co-located arm), and the
                    // declarator's pointer layers wrap on top via
                    // `declaratorDeclaredType` → `volatile <base> *p` =
                    // Ptr<VolatileQual(base)>. This holds uniformly for local /
                    // global / param / struct-member / typedef: a `typedef volatile
                    // int vint;` aliases VolatileQual(Int), so every `vint x;`
                    // inherits the volatile through the TYPE (no longer dropped).
                    // East `int * volatile p` (the pointer OBJECT is volatile) stays
                    // on the c21 symbol-isVolatile path — that volatile is a
                    // `ptrQualifier` INSIDE the declarator (after the star), not a
                    // base qualifier, so the co-located arm excludes it.
                    //
                    // C11/C23 6.7.5 (D-CSUBSET-ALIGNAS): the alignas specifier lives
                    // on the SHARED declaration prefix (`node`), so its VALIDATION +
                    // DIAGNOSTICS (pow2 / max / non-constant / context) must fire
                    // EXACTLY ONCE per declaration, not once per declarator — a
                    // multi-declarator `alignas(3) int a, b;` is one erroneous
                    // specifier, one diagnostic. These declaration-scoped flags gate
                    // the emit to the first declarator; the computed override is then
                    // STORED on every declarator's symbol (each field/object carries
                    // it). The natural-align (raise-only) check keys on the first
                    // declarator's type — the dominant `alignas(N) T a, b;` case
                    // shares the head type, so the check is identical for every slot.
                    NodeId const declAlignasSpec =
                        firstAlignasSpecInPrefix(s, cfg, tree, node, decl);
                    // FC16 (D-CSUBSET-NORETURN): does this declaration's specifier
                    // prefix name the `noreturn` attribute? Computed ONCE per
                    // declaration (like `declAlignasSpec`); STORED per-declarator
                    // below, gated on the declared type being a FnSig (a `_Noreturn`
                    // on a non-function object is inert — a named safe-miss deferral).
                    bool const declHasNoreturn =
                        specifierPrefixNamesNoreturn(cfg, tree, node, decl);
                    // FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS): fold the standard-
                    // attribute effects from this declaration's specifier
                    // prefix ONCE (like `declAlignasSpec`/`declHasNoreturn`);
                    // applied to EVERY named declarator below, so
                    // `[[maybe_unused]] int a, b;` flags both. emitUnknown=true
                    // — this is the once-per-declaration site, so an unknown
                    // `[[frobnicate]]` warns exactly once even multi-declarator.
                    AttributeSemanticsFacts declAttrFacts;
                    scanAttributeSemantics(
                        s, cfg, tree, specifierPrefixChild(tree, node, decl),
                        /*emitUnknown=*/true, declAttrFacts);
                    bool alignasHandledForDecl = false;
                    bool alignasBitfieldReported = false;
                    bool alignasContextReported = false;
                    std::optional<std::uint32_t> declAlignOverride;
                    for (NodeId dNode : declarators) {
                        // c34 (D-CSUBSET-ARRAY-SIZE-INFERENCE): a `[]` (empty-bound)
                        // array on a VARIABLE WITH AN INITIALIZER infers its size
                        // from that initializer (C 6.7.9p22). The resolver's
                        // absent-length branch already yields an INCOMPLETE array
                        // when missing-length-is-OK (`allowFlexibleArray`) — reuse
                        // that exact path by relaxing the flag ONLY when this
                        // declarator HAS an initializer, then COMPLETE the incomplete
                        // array below. A `[]` with NO initializer keeps the original
                        // flag (false for a var/global rule) ⇒ the resolver's
                        // S_NonConstantArrayLength fires, never a silently-sized
                        // array. A struct-field rule's allowFlexibleArray is
                        // untouched (its `||` short-circuits true regardless). The
                        // top-level fold sees the relaxed flag; the group recursion
                        // propagates ONLY the init-inference signal (c47 —
                        // D-CSUBSET-FNPTR-ARRAY-SIZE-INFERENCE), so a nested fn-ptr
                        // PARAM / FAM `[]` still errors, but an inferred-size fn-ptr
                        // ARRAY `(*const arr[])(T) = {…}` is sized from its init.
                        NodeId const initNode =
                            initializerNodeOf(tree, dNode, *cfg.declarators);
                        // c82 (C 6.7.6.3p7) + VLA C4a-param (D-CSUBSET-VLA,
                        // FIX-2): an `arrayToPointer` row (a C parameter) routes
                        // its array-decay through the DISTINCT `paramDecay`
                        // signal, NOT `allowIncomplete` (the struct-field FAM /
                        // init-inference bool) — so a param's present-length
                        // non-outermost suffix builds a `vlaArray` in the pointee
                        // (`int (*p)[n]`, `int a[][n]`) while an absent `[]` still
                        // decays; the FAM path stays byte-identical. The
                        // adjustment below rewrites any top-level array to
                        // Ptr<element>, so the incomplete/VLA form never escapes
                        // as the bound type.
                        bool const allowIncomplete =
                            decl.allowFlexibleArray || initNode.valid();
                        TypeId declTy = declaratorDeclaredType(
                            s, cfg, tree, dNode, headTy, here,
                            /*emitOnMiss=*/true, allowIncomplete,
                            /*allowInitInferredArray=*/initNode.valid(),
                            /*paramDecay=*/decl.arrayToPointer);
                        // Complete an inferred `[]` from its initializer (string
                        // length + NUL, or brace top-level element count). A
                        // non-array / already-sized / no-init type passes through
                        // unchanged. ONE completion site → every downstream tier
                        // (sizeof, layout, indexing, both HIR paths) agrees.
                        if (!decl.allowFlexibleArray)
                            declTy = completeIncompleteArrayFromInit(
                                s, s.idx(), tree, initNode, declTy);
                        // c82 D-CSUBSET-PARAM-ARRAY-ADJUSTMENT: the definitive
                        // adjustment — the bound param symbol carries the
                        // POINTER (sizeof(param)==pointer-size, body indexing
                        // reads through it), mirroring the harvest site in
                        // `declRowDeclaredType` so the FnSig agrees.
                        declTy = adjustArrayToPointer(s, decl, declTy);
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
                            // VLA C4b (D-CSUBSET-VLA, design-audit I1): a VLA-typedef
                            // OBJECT (`typedef int R[n]; R a;`) derives its VLA-ness
                            // entirely from the head alias — nothing else records WHICH
                            // typedef froze the runtime size (C99 §6.7.7p2: `n` is
                            // evaluated once, at the typedef). Correlate a→R HERE: when
                            // the declared type is EXACTLY the head type (`declTy ==
                            // headTy` — a pure `R a;`, no stars/own suffix, so
                            // `directDeclaredType` returned the base byte-identically)
                            // AND that head type is (or contains) a VLA, descend the head
                            // for the alias type-name identifier and, if it resolves to a
                            // typedef of that VLA type, stamp the object's
                            // `vlaTypedefOrigin`. The `declTy == headTy` gate is
                            // load-bearing: `R a[m]` (extra dim → declTy has one more
                            // array level) and `R *p` (declTy is Ptr) DIFFER from headTy
                            // → EXCLUDED (they keep their own capture + deferred
                            // fail-loud). A miss here is a safe fail-loud downstream (no
                            // captured size), never a silent miscompile.
                            // Gate to OBJECT declarations only — `vlaTypedefOrigin`
                            // means "the VLA typedef this OBJECT froze its size from",
                            // read solely by the object's HIR alloca. A chained VLA
                            // typedef (`typedef R S;`) is itself DeclarationKind::Type
                            // and is discriminated at HIR by its own suffix presence
                            // (declaratorHasArraySuffix), so it needs no origin flag —
                            // keeping this write off typedef symbols.
                            if (s.symbols.at(sym).kind == DeclarationKind::Variable
                                && declTy == headTy
                                && (s.lattice.interner().isVlaArray(headTy)
                                    || s.lattice.interner().typeContainsVla(headTy))
                                && decl.headChild.has_value()
                                && *decl.headChild < kids.size()) {
                                // First type-name identifier token in the head (the
                                // alias); cv-qualifier keyword tokens are not
                                // identifierToken → skipped. A pure VLA-typed head is a
                                // single alias identifier. Push children REVERSED so the
                                // work-stack pops left-to-right (leftmost-first).
                                NodeId aliasTok{};
                                if (cfg.identifierToken.valid()) {
                                    std::vector<NodeId> stk{kids[*decl.headChild]};
                                    for (int guard = 0;
                                         guard < 4096 && !stk.empty()
                                             && !aliasTok.valid();
                                         ++guard) {
                                        NodeId const cur = stk.back();
                                        stk.pop_back();
                                        if (tree.kind(cur) == NodeKind::Token) {
                                            if (tree.tokenKind(cur)
                                                == cfg.identifierToken)
                                                aliasTok = cur;
                                            continue;
                                        }
                                        auto const hk = visibleChildren(tree, cur);
                                        for (std::size_t i = hk.size(); i-- > 0;)
                                            stk.push_back(hk[i]);
                                    }
                                }
                                if (aliasTok.valid() && here.valid()) {
                                    // SAME scope-resolution the type resolver used
                                    // (resolveTypeNodeImpl's alias arm: ordinary-namespace
                                    // `s.scopes.lookup(scope, text)` with scope == here),
                                    // so this recovers EXACTLY the typedef symbol that
                                    // produced `headTy`.
                                    std::string const nm{tree.text(aliasTok)};
                                    SymbolId const aliasSym = s.scopes.lookup(here, nm);
                                    if (aliasSym.valid()) {
                                        auto const& arec = s.symbols.at(aliasSym);
                                        if (arec.kind == DeclarationKind::Type
                                            && arec.type.valid()
                                            && (s.lattice.interner()
                                                    .isVlaArray(arec.type)
                                                || s.lattice.interner()
                                                       .typeContainsVla(arec.type)))
                                            s.symbols.at(sym).vlaTypedefOrigin = aliasSym;
                                    }
                                }
                            }
                            // c35 D-CSUBSET-FORWARD-STRUCT-DECLARATION: an OBJECT
                            // (a named local/global, `requireNamedDeclarators` —
                            // NOT a param, NOT a struct field [those route through
                            // the composite-composition incomplete-MEMBER guard],
                            // NOT a typedef [kind != Variable]) declared BY VALUE
                            // with an INCOMPLETE composite type fails loud HERE (C
                            // 6.7p7 / 6.2.5 — an object shall have a complete type).
                            // c35's opaque-tag forward-mint makes `struct S v;`
                            // RESOLVE the tag (so `struct S *p` opaque pointers
                            // compile); this is the by-value counterpart that keeps
                            // a never-defined `struct S v;` from silently folding to
                            // size 0 — the earliest tier with the full type (the MIR
                            // allocaForLocal computeLayout guard is the deeper
                            // backstop). A POINTER to an incomplete type is a Ptr
                            // (never an incomplete composite) and is correctly NOT
                            // flagged; an `extern` declaration (completed elsewhere)
                            // is excluded. Mirrors S_IncompleteTypeMember.
                            if (s.symbols.at(sym).kind
                                        == DeclarationKind::Variable
                                && decl.requireNamedDeclarators
                                && !s.symbols.at(sym).isExternDeclaration
                                && s.lattice.interner()
                                       .isIncompleteComposite(declTy)) {
                                ParseDiagnostic d;
                                d.code     = DiagnosticCode::S_IncompleteTypeObject;
                                d.severity = DiagnosticSeverity::Error;
                                d.buffer   = tree.source().id();
                                d.span     = tree.span(nameNode);
                                d.actual   = std::string{tree.text(nameNode)};
                                s.reporter.report(std::move(d));
                            }
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
                        // C11/C23 6.7.5 (D-CSUBSET-ALIGNAS): a struct/union member's
                        // `alignas(N)` / `alignas(T)` — read from the SHARED member
                        // specifier prefix on `node` (the whole structField/unionField;
                        // a prefix alignas applies to EVERY slot: `alignas(16) int a,b;`).
                        // Gated on this being a field rule (bitfieldSuffix present ⇒ a
                        // structField/unionField DeclarationRule). 6.7.5p2: a BIT-FIELD
                        // may NOT carry an alignment specifier → S_AlignasInvalidContext.
                        // The validate-and-emit runs ONCE per declaration (the
                        // `alignasHandledForDecl` gate); the resulting override is then
                        // STORED on THIS declarator's symbol AND every following one —
                        // the composite's Pass-1 completion gathers each into
                        // `fieldAligns`. (Per-slot storage; one diagnostic.)
                        if (decl.bitfieldSuffix.has_value() && declAlignasSpec.valid()) {
                            if (!alignasHandledForDecl) {
                                alignasHandledForDecl = true;
                                // A bit-field member carrying alignas: the CONTEXT is
                                // illegal regardless of the value. Emit once; the
                                // per-slot check below still fires for each bit-field
                                // declarator (a bit-field never stores an override).
                                if (!s.symbols.at(sym).bitFieldWidth.has_value()) {
                                    // D-CSUBSET-PACKED: a member of a PACKED composite
                                    // has a natural baseline of 1 (packed removes the
                                    // alignment requirement), so `alignas(1)` in a
                                    // packed struct is legal (not weaker-than-natural).
                                    // Resolve the enclosing composite via the field
                                    // scope's anchor (the structSpec/unionSpec node).
                                    std::optional<std::uint32_t> naturalBaseline;
                                    if (here.valid()
                                        && scanCompositePacked(
                                               s, cfg, tree,
                                               s.scopes.scopes()[here.v].anchor,
                                               /*emitDiagnostics=*/false)) {
                                        naturalBaseline = 1u;
                                    }
                                    declAlignOverride = resolveAlignasOverride(
                                        s, cfg, tree, node, decl, declTy, here,
                                        naturalBaseline);
                                }
                            }
                            if (s.symbols.at(sym).bitFieldWidth.has_value()) {
                                // A BIT-FIELD declarator may not carry alignas. This is
                                // per-slot (a mixed `alignas(8) int a:3, b;` — only the
                                // bit-field slots are illegal), but for the shared
                                // prefix the standard idiom is all-or-none; report the
                                // first offending bit-field slot only (the gate below).
                                if (!alignasBitfieldReported) {
                                    alignasBitfieldReported = true;
                                    ParseDiagnostic d;
                                    d.code     = DiagnosticCode::S_AlignasInvalidContext;
                                    d.severity = DiagnosticSeverity::Error;
                                    d.buffer   = tree.source().id();
                                    d.span     = tree.span(declAlignasSpec);
                                    d.actual   = "alignas on a bit-field member";
                                    s.reporter.report(std::move(d));
                                }
                            } else if (declAlignOverride.has_value()) {
                                s.symbols.at(sym).explicitAlignment = declAlignOverride;
                            }
                        }
                        bool const isFnSig = declTy.valid()
                            && s.lattice.interner().kind(declTy)
                                   == TypeKind::FnSig;
                        // FC16 (D-CSUBSET-NORETURN): mark a FUNCTION symbol whose
                        // declaration named the attribute. Gated on `isFnSig` so a
                        // `_Noreturn int x;` (non-function) is INERT (a safe miss —
                        // the named `_Noreturn`-on-non-function deferral), never a
                        // wrongly-flagged data object. OR-merged into a proto/def
                        // survivor by the post-1.5 sweep so a call sees the flag.
                        if (isFnSig && declHasNoreturn)
                            s.symbols.at(sym).isNoreturn = true;
                        // FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS): apply the
                        // declaration's folded attribute facts to THIS
                        // declarator's symbol (C23 6.7.13: an attribute in the
                        // declaration specifiers appertains to each declared
                        // entity). No type gate — maybe_unused is read only by
                        // the D8 object check, nodiscard only by checkCall's
                        // function arm, deprecated by any use — an
                        // inapplicable-kind flag is inert, never wrong bytes.
                        // Messages first-non-empty-wins (F7).
                        if (declAttrFacts.maybeUnused)
                            s.symbols.at(sym).isMaybeUnused = true;
                        if (declAttrFacts.deprecated) {
                            auto& atRec = s.symbols.at(sym);
                            atRec.isDeprecated = true;
                            if (atRec.deprecatedMessage.empty())
                                atRec.deprecatedMessage =
                                    declAttrFacts.deprecatedMessage;
                        }
                        if (declAttrFacts.nodiscard) {
                            auto& atRec = s.symbols.at(sym);
                            atRec.isNodiscard = true;
                            if (atRec.nodiscardMessage.empty())
                                atRec.nodiscardMessage =
                                    declAttrFacts.nodiscardMessage;
                        }
                        // C11/C23 6.7.5 (D-CSUBSET-ALIGNAS): a VARIABLE's alignas.
                        // Only for a NON-field declaration (`!bitfieldSuffix` — a
                        // field is handled above); a PARAMETER carries its own
                        // `param` DeclarationRule (no specifierPrefix + no alignasSpec
                        // in the param grammar) so it never reaches here with an
                        // alignas. 6.7.5p2: an alignas on a FUNCTION (a definition or a
                        // prototype — FnSig-typed / function-form declarator) or a
                        // TYPEDEF (kind Type) is a constraint violation →
                        // S_AlignasInvalidContext. Otherwise store the validated
                        // override on the object's symbol. The validate-and-emit runs
                        // ONCE per declaration (the `alignasHandledForDecl` gate); the
                        // override is stored on THIS declarator's symbol AND every
                        // following one. Threading the stored value to globals/locals
                        // codegen is a SEPARATE deferred task (unconsumed for variables).
                        if (!decl.bitfieldSuffix.has_value() && declAlignasSpec.valid()) {
                            DeclarationKind const dk = s.symbols.at(sym).kind;
                            char const* badCtx = nullptr;
                            if (isFnSig || isFunctionForm
                                || dk == DeclarationKind::Function)
                                badCtx = "alignas on a function";
                            else if (dk == DeclarationKind::Type)
                                badCtx = "alignas on a typedef";
                            if (!alignasHandledForDecl) {
                                alignasHandledForDecl = true;
                                if (badCtx == nullptr) {
                                    declAlignOverride = resolveAlignasOverride(
                                        s, cfg, tree, node, decl, declTy, here);
                                }
                            }
                            if (badCtx != nullptr) {
                                if (!alignasContextReported) {
                                    alignasContextReported = true;
                                    ParseDiagnostic d;
                                    d.code     = DiagnosticCode::S_AlignasInvalidContext;
                                    d.severity = DiagnosticSeverity::Error;
                                    d.buffer   = tree.source().id();
                                    d.span     = tree.span(declAlignasSpec);
                                    d.actual   = badCtx;
                                    s.reporter.report(std::move(d));
                                }
                            } else if (declAlignOverride.has_value()) {
                                s.symbols.at(sym).explicitAlignment = declAlignOverride;
                            }
                        }
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
                    // anonymous symbol (Pass 1 minted it on `node`). Stamp the
                    // head as its type, then a THREE-way decision on the head:
                    //   (1) a `: W` bit-field (`int : 5;`) — the c10 packing-slot
                    //       behavior, byte-identical.
                    //   (2) FC16 D-CSUBSET-ANON-MEMBER-PROMOTION (C11/C23
                    //       §6.7.2.1 ¶13): a non-bit-field whose head is an
                    //       anonymous Struct/Union — its members are PROMOTED
                    //       into the enclosing composite's member namespace
                    //       (`promoteAnonMembers`).
                    //   (3) else — an anonymous NON-bit-field of a scalar type
                    //       (`int ;` / `int *;`) declares nothing; fail loud with
                    //       S_DeclarationDeclaresNothing on `node` (C 6.7.2.1).
                    // The outer gate is `anonymousNameAllowed` (a field form);
                    // `resolveBitfieldSuffix` is safe for a non-bit-field field
                    // (returns `present=false`), and an anon composite member
                    // carries no bit-field suffix so it falls to arm (2)/(3).
                    if (!resolvedNamed && decl.anonymousNameAllowed) {
                        SymbolId const sym = s.symbolAtOr(node);
                        if (sym.valid() && headTy.valid()) {
                            s.symbols.at(sym).type = headTy;
                            s.nodeToType.set(node, headTy);
                        }
                        bool const anonComposite =
                            headTy.valid()
                            && (s.lattice.interner().kind(headTy)
                                    == TypeKind::Struct
                                || s.lattice.interner().kind(headTy)
                                       == TypeKind::Union);
                        if (sym.valid() && anonComposite) {
                            // (2) anonymous struct/union — promote its members.
                            // An anon COMPOSITE field is never itself a bit-field,
                            // so we DELIBERATELY do NOT call resolveBitfieldSuffix
                            // here: its bounded descendant search from `node`
                            // would find an INNER member's `: W` suffix (`union {
                            // int a : 4; };`) and mis-validate that width against
                            // the composite head type (a false
                            // S_BitFieldNonIntegerType). Each inner bit-field is
                            // resolved by the anon composite's OWN Pass-1.5 visit.
                            s.symbols.at(sym).isAnonymousMember = true;
                            promoteAnonMembers(s, tree, sym, headTy, {},
                                               s.symbols.at(sym).scope);
                        } else {
                            BitfieldResolution const bf = resolveBitfieldSuffix(
                                s, tree, decl, node, headTy, /*hasName=*/false,
                                here, &cfg);
                            if (bf.present) {
                                // (1) anonymous bit-field packing slot.
                                if (sym.valid() && bf.width.has_value())
                                    s.symbols.at(sym).bitFieldWidth = bf.width;
                            } else {
                                // (3) declares nothing — loud.
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
                                // C11/C23 6.7.5 (D-CSUBSET-ALIGNAS): gather each
                                // field's EXPLICIT alignment override (set at the
                                // field's Pass-1 resolution from the member specifier
                                // prefix) into the parallel span passed to
                                // `completeComposite` — 0 for a field with no override.
                                // ALL-fields-or-EMPTY: when NO member carries alignas
                                // the span stays EMPTY (so the composite interns
                                // byte-identically to a pre-alignas struct — zero TypeId
                                // churn, mirroring the all-ordinary bitfields rule); the
                                // interned TYPE is then the authoritative align source
                                // (computeLayout raises each field's align via max()).
                                std::vector<std::uint32_t> fieldAligns;
                                bool anyAlign = false;
                                for (auto const& fe : fields)
                                    if (s.symbols.at(fe.sym).explicitAlignment
                                            .has_value()) { anyAlign = true; break; }
                                if (anyAlign) {
                                    fieldAligns.reserve(fields.size());
                                    for (auto const& fe : fields) {
                                        auto const& a =
                                            s.symbols.at(fe.sym).explicitAlignment;
                                        fieldAligns.push_back(a.value_or(0u));
                                    }
                                }
                                // D5.4 / D5.5: struct vs union vs enum
                                // dispatch is config-driven via
                                // FieldChildrenDescriptor::compositeKind.
                                CompositeKind const ck =
                                    decl.fieldChildren->compositeKind;
                                // D-CSUBSET-PACKED: scan the composite's TRAILING
                                // attribute list for a honored `packed` (emitting the
                                // S_UnknownTypeAttribute typo diagnostic exactly ONCE
                                // here). packed applies only to struct/union. A packed
                                // composite that ALSO has a bit-field member is
                                // UNSUPPORTED (D-CSUBSET-PACKED-BITFIELD-INTERACTION):
                                // fail loud S_PackedBitfieldUnsupported and complete it
                                // UNPACKED (so the type still lays out — the build
                                // fails via the unsuppressable diagnostic; the layout
                                // nullopt belt is the backstop for interner-direct
                                // construction that bypasses this scan).
                                bool composedPacked = false;
                                NodeId const specNode =
                                    srec.structScope.valid()
                                        ? s.scopes.scopes()[srec.structScope.v].anchor
                                        : NodeId{};
                                if (ck == CompositeKind::Struct
                                    || ck == CompositeKind::Union) {
                                    composedPacked = scanCompositePacked(
                                        s, cfg, tree, specNode,
                                        /*emitDiagnostics=*/true);
                                    if (composedPacked) {
                                        bool anyBitfieldMember = false;
                                        for (std::int64_t const w : fieldBitWidths)
                                            if (w != kNotBitfield) {
                                                anyBitfieldMember = true;
                                                break;
                                            }
                                        if (anyBitfieldMember) {
                                            ParseDiagnostic d;
                                            d.code = DiagnosticCode::S_PackedBitfieldUnsupported;
                                            d.severity = DiagnosticSeverity::Error;
                                            d.buffer = tree.source().id();
                                            d.span = tree.span(
                                                specNode.valid() ? specNode : resolved.node);
                                            d.actual = std::string{srec.name};
                                            s.reporter.report(std::move(d));
                                            composedPacked = false;   // complete UNPACKED
                                        }
                                    }
                                }
                                // FC6: flexible-array-member constraints (C99
                                // §6.7.2.1), positioned at the offending field.
                                // The not-last / sole-member checks are
                                // struct-only (a bare FAM in a union is rejected
                                // earlier — the `allowFlexibleArray=false` gate on
                                // `unionField` → S_NonConstantArrayLength). The
                                // EMBEDDED-FAM check (p18) also runs for unions,
                                // but with the c99 D-CSUBSET-FAM-IN-UNION-MEMBER
                                // carve-out below: p18 forbids a FAM-bearing struct
                                // as a member of a STRUCTURE or an ELEMENT OF AN
                                // ARRAY — it says nothing about a UNION. gcc/clang
                                // both ACCEPT a FAM-struct as a DIRECT union member
                                // (sqlite's `union { SrcList sSrc; u8 space[N]; }`
                                // stack-slab idiom relies on it; the FAM tail is
                                // 0-length for sizeof, so the union sizes to
                                // max(prefix, N) — computeLayout's Union arm already
                                // does this). So a DIRECT FAM-struct union member is
                                // permitted; an ARRAY-of-FAM member (`F arr[3]` — p18
                                // "element of an array") stays forbidden EVEN in a
                                // union. Enums carry no field types. This is the
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
                                            // A field whose TYPE embeds a flexible array
                                            // member. `ft` reaching HERE is necessarily a
                                            // FAM-bearing STRUCT: typeContainsFlexibleArray
                                            // recurses into struct members but NOT into
                                            // unions, and an array whose element embeds a
                                            // FAM is already rejected upstream at array
                                            // construction (applyArraySuffix → InvalidType,
                                            // which typeContainsFlexibleArray reports absent)
                                            // before it can reach this loop. C99 §6.7.2.1p18
                                            // forbids such a struct as a member of a
                                            // STRUCTURE (and as an array element) — but says
                                            // nothing about a UNION, and gcc/clang both
                                            // ACCEPT a FAM-struct as a direct union member
                                            // (sqlite's `union { SrcList sSrc; u8 space[N]; }`
                                            // slab; D-CSUBSET-FAM-IN-UNION-MEMBER). So the
                                            // gate is purely union-vs-struct: permit in a
                                            // union, fail loud in a struct. (Deliberately
                                            // NOT `&& kind(ft)==Struct`: that is a tautology
                                            // here — every `ft` reaching this branch is a
                                            // struct — and it would WRONGLY reject the
                                            // p18-legal `union { union V v; }` were a future
                                            // change to let a union-typed `ft` through.)
                                            bool const permittedAsUnionMember =
                                                ck == CompositeKind::Union;
                                            if (!permittedAsUnionMember)
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
                                            compositeTy, fieldTypes, composedPacked,
                                            fieldBitWidths, /*fieldOffsets=*/{},
                                            fieldAligns);
                                } else if (ck == CompositeKind::Enum) {
                                    // D5.5: enum type carries no field-
                                    // operands — only its nominal name +
                                    // underlying TypeKind (I32 by default;
                                    // C23 6.7.2.2 explicit underlying below).
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
                                    //
                                    // C23 6.7.2.2 (D-CSUBSET-ENUM-UNDERLYING-TYPE,
                                    // FC17): resolve the OPTIONAL explicit
                                    // underlying-type clause (`enum E : T`). The
                                    // default stays int (I32) — unchanged for
                                    // EVERY enum without the clause. A non-integer
                                    // underlying fails loud
                                    // (S_InvalidEnumUnderlyingType) and falls back
                                    // to int so the enum still lays out; its
                                    // enumerators are then NOT range-checked (only
                                    // an explicit, valid underlying enables the
                                    // per-enumerator range check below).
                                    TypeKind underlyingKind = TypeKind::I32;
                                    bool hasExplicitUnderlying = false;
                                    if (decl.enumUnderlyingType.has_value()
                                        && specNode.valid()) {
                                        NodeId const clauseNode =
                                            findVisibleChildOfRule(
                                                tree, specNode,
                                                decl.enumUnderlyingType->rule);
                                        if (clauseNode.valid()
                                            && decl.enumUnderlyingType->typeChild
                                                   .has_value()) {
                                            auto const clauseKids =
                                                visibleChildren(tree, clauseNode);
                                            std::uint32_t const tci =
                                                *decl.enumUnderlyingType->typeChild;
                                            if (tci < clauseKids.size()) {
                                                ScopeId const uScope =
                                                    s.scopes.scopes()
                                                        [srec.structScope.v].parent;
                                                TypeId const uTy = resolveTypeNode(
                                                    s, cfg, tree, clauseKids[tci],
                                                    uScope, /*emitOnMiss=*/true);
                                                if (uTy.valid()) {
                                                    TypeKind const k =
                                                        s.lattice.interner().kind(uTy);
                                                    if (isIntegerKind(k)) {
                                                        underlyingKind = k;
                                                        hasExplicitUnderlying = true;
                                                    } else {
                                                        ParseDiagnostic d;
                                                        d.code = DiagnosticCode::
                                                            S_InvalidEnumUnderlyingType;
                                                        d.severity =
                                                            DiagnosticSeverity::Error;
                                                        d.buffer =
                                                            tree.source().id();
                                                        d.span =
                                                            tree.span(clauseNode);
                                                        d.actual =
                                                            std::string{srec.name};
                                                        s.reporter.report(
                                                            std::move(d));
                                                    }
                                                }
                                                // uTy invalid ⇒ resolveTypeNode
                                                // already emitted S_UnknownType
                                                // (fail loud on a typo'd type).
                                            }
                                        }
                                    }
                                    compositeTy = s.lattice.interner().enumType(
                                        srec.name, underlyingKind);
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
                                            // C23 6.7.2.2 (D-CSUBSET-ENUM-
                                            // UNDERLYING-TYPE): with an EXPLICIT
                                            // underlying type every enumerator
                                            // must be representable in it — fail
                                            // loud (S_EnumeratorValueOutOfRange)
                                            // on overflow (`enum E : unsigned char
                                            // { A = 256 }` / `{ A = -1 }`).
                                            // Default-int enums have
                                            // hasExplicitUnderlying == false, so
                                            // this check NEVER fires for them (the
                                            // C classic wrap-around behavior is
                                            // unchanged).
                                            if (hasExplicitUnderlying
                                                && !enumeratorValueFitsUnderlying(
                                                       value, underlyingKind)) {
                                                ParseDiagnostic d3;
                                                d3.code = DiagnosticCode::
                                                    S_EnumeratorValueOutOfRange;
                                                d3.severity =
                                                    DiagnosticSeverity::Error;
                                                d3.buffer = tree.source().id();
                                                d3.span =
                                                    tree.span(erec.declRuleNode);
                                                d3.actual = erec.name;
                                                s.reporter.report(std::move(d3));
                                            }
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
                                            compositeTy, fieldTypes, composedPacked,
                                            fieldBitWidths, /*fieldOffsets=*/{},
                                            fieldAligns);
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
                            // c82 D-CSUBSET-EXTERN-INCOMPLETE-ARRAY: never
                            // DOWNGRADE a merged symbol's already-resolved
                            // COMPLETE array to this declaration's INCOMPLETE
                            // one — `int g[3]; extern int g[];` keeps
                            // Array<int,3> (C 6.2.7: the composite of a
                            // completed and an incomplete array is the
                            // completed one, regardless of declaration
                            // order). The extern-first order needs no guard:
                            // the definition's LATER write is the completion.
                            auto& in = s.lattice.interner();
                            TypeId const existing = s.symbols.at(sym).type;
                            bool const downgrade =
                                existing.valid()
                                && in.isIncompleteArray(declTy)
                                && in.kind(existing) == TypeKind::Array
                                && !in.isIncompleteArray(existing);
                            if (downgrade) {
                                // The node still carries the (complete)
                                // composite type for downstream walks.
                                s.nodeToType.set(resolved.node, existing);
                            } else {
                                s.symbols.at(sym).type = declTy;
                                s.nodeToType.set(resolved.node, declTy);
                            }
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

// ── Pass 1.5 driver: iterative whole-tree POST-ORDER walk ──────────────────
// D-PARSE-DEEP-NEST-RECURSION-MEMORY (plan 24 Stage 2, missed site): an explicit
// heap work-stack replaces host recursion so a deeply-nested tree (e.g. a
// 1200-deep nested switch) carries flat O(N) host-stack cost instead of one host
// frame per level — the recursive form overflowed the analysis worker's stack
// under ASan's inflated frames (a `stack-use-after-scope` / `stack-overflow`).
// Two phases per node — phase 0 resolves its child scope `here` + pushes its
// children in SOURCE order; phase 1 runs `resolveDeclTypesPost` (the prior post-
// child handling). OUTPUT-IDENTICAL to the recursion: same pre-order scope
// resolution, same left-to-right child order, same post-order node handling. A
// faithful mirror of `pass2` (which proved this exact transform) modulo
// loopDepth, of which Pass 1.5 has none.
void resolveDeclTypes(EngineState& s, SemanticConfig const& cfg, Tree const& tree,
                      NodeId rootNode, ScopeId rootCurrent) {
    struct Frame {
        NodeId       node;
        ScopeId      current;
        ScopeId      here;       // resolved at phase 0, consumed at phase 1
        std::uint8_t phase;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{rootNode, rootCurrent, {}, 0});
    while (!stack.empty()) {
        // Copy out the fields we need BEFORE any push_back (which can realloc
        // and dangle a reference into `stack`).
        Frame const f = stack.back();
        if (f.phase == 0) {
            if (!f.node.valid() || isEmptySpace(tree.flags(f.node))) {
                stack.pop_back();
                continue;
            }
            // c32 D-CSUBSET-FNPTR-PARAM-SCOPE: re-derive the child scope via the
            // SAME `nodeOpensChildScope` predicate Pass 1 used (anchor lookup
            // finds whatever Pass 1 pushed — `current` when none), so the three
            // passes never diverge.
            ScopeId here = f.current;
            if (nodeOpensChildScope(s, cfg, tree, f.node)) {
                here = childScopeFor(s, tree, f.node, f.current);
            }
            // Record `here` + advance to the post phase BEFORE pushing children.
            stack.back().here  = here;
            stack.back().phase = 1;
            // Push children so they POP in source (left-to-right) order: a LIFO
            // stack needs reverse insertion. Each child inherits `here` as its
            // `current`, exactly as the recursive `resolveDeclTypes(child, here)`
            // passed.
            std::span<NodeId const> const kids = tree.children(f.node);
            for (std::size_t i = kids.size(); i-- > 0;) {
                stack.push_back(Frame{kids[i], here, {}, 0});
            }
        } else {
            stack.pop_back();
            resolveDeclTypesPost(s, cfg, tree, f.node, f.here);
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

// C11/C23 6.4.5 / 6.4.5p5: the element core of a (possibly adjacent-concatenated)
// string-literal node, keyed by the run's EFFECTIVE encoding prefix — the single
// distinct NON-narrow opener among ALL segments (narrow segments widen to it,
// position-independent), NOT merely the first opener. `L"`/`u"`/`U"`/`u8"` map to
// their declared/format-resolved core; the narrow `"` (and any grammar without a
// prefix table) falls back to `stringLiteralElementCore`. `conflict` is set when the
// run mixes ≥2 DIFFERENT non-narrow prefixes (6.4.5p5's impl-defined case, which
// this implementation rejects); the caller then fails loud + leaves the node untyped.
// The scan (SF4, the drift-prone part) is the SHARED `effectiveStringConcatPrefix`
// chokepoint the HIR tier also uses; MF2: its non-narrow classifier is the
// token-KIND set `nonNarrowStringOpeners` (format-agnostic), NOT the format-resolved
// `byStart` core. `owningNode` is the stringLiteralExpr rule node (openers are its
// children) OR a body-token's parent (the single-opener per-token fallback path).
struct StringLiteralConcatCore {
    TypeKind core;
    bool     conflict;
};
[[nodiscard]] StringLiteralConcatCore stringLiteralElementCoreOf(
        EngineState const& s, Tree const& tree, NodeId owningNode) {
    auto const& byStart = s.idx().stringLiteralElementCoreByStart;
    if (byStart.empty() || !owningNode.valid()
        || tree.kind(owningNode) != NodeKind::Internal) {
        return { s.idx().stringLiteralElementCore, false };
    }
    EffectiveStringPrefix const eff = effectiveStringConcatPrefix(
        tree, owningNode,
        [&](SchemaTokenId tk) {
            return s.idx().nonNarrowStringOpeners.count(tk.v) != 0;
        },
        /*narrowFallback=*/SchemaTokenId{});
    if (eff.conflict) return { s.idx().stringLiteralElementCore, true };
    if (eff.effectiveOpener.valid()) {
        if (auto it = byStart.find(eff.effectiveOpener.v); it != byStart.end()) {
            return { it->second, false };
        }
    }
    // All-narrow run (or an unmapped opener): the narrow default — BYTE-IDENTICAL to
    // the pre-Cycle-D path (byStart[narrowOpener] is built == stringLiteralElementCore).
    return { s.idx().stringLiteralElementCore, false };
}

// C11/C23 6.4.4.4: the WIDE/UTF element core of a character constant, keyed by its
// ACTUAL opener token — but ONLY for the wide openers (`L'`/`u'`/`U'`/`u8'`). The
// narrow `'x'` opener (`CharStart`) is DELIBERATELY absent from the map, so this
// returns std::nullopt for it and the caller keeps the flat `int` type (byte-
// identical). The opener is a DIRECT child token of `owningNode` (the charLiteralExpr
// rule node; the inline-alt opener pushes it flat), so a single child scan finds it.
// The core was format-resolved at index-build time (this tier owns `activeFormat`).
[[nodiscard]] std::optional<TypeKind>
charLiteralWideCoreOf(EngineState const& s, Tree const& tree, NodeId owningNode) {
    auto const& byStart = s.idx().charLiteralElementCoreByStart;
    if (!byStart.empty() && owningNode.valid()
        && tree.kind(owningNode) == NodeKind::Internal) {
        for (NodeId c : visibleChildren(tree, owningNode)) {
            if (tree.kind(c) != NodeKind::Token) continue;
            auto it = byStart.find(tree.tokenKind(c).v);
            if (it != byStart.end()) return it->second;
        }
    }
    return std::nullopt;
}

// C 6.4.5: the `Array<elementCore, N+1>` type of a string literal whose
// escape-decoded bytes are `decodedBytes`. For the NARROW core (Char/Byte) N is
// the byte length (unchanged path). For a WIDE core (U8/U16/U32/I32) the raw bytes
// are UTF-8-decoded and re-encoded so N is the ELEMENT-width CODE-UNIT count — the
// SAME `encodeWideString` the HIR tier runs, so both tiers agree on N. A wide
// encode error (ill-formed UTF-8 / cp>0x10FFFF) returns
// InvalidType: the node stays untyped, the semantic phase already surfaces the
// fault at the HIR tier's fail-loud, and a `sizeof` of it fails loud (never a
// guessed size). Narrow strings never reach the wide path (byte-identical).
[[nodiscard]] TypeId stringLiteralArrayType(EngineState& s, std::string const& decodedBytes,
                                            TypeKind elementCore) {
    TypeInterner& interner = s.lattice.interner();
    if (elementCore == TypeKind::Char || elementCore == TypeKind::Byte) {
        return interner.array(interner.primitive(elementCore),
                              static_cast<std::int64_t>(decodedBytes.size() + 1));
    }
    WideEncodeResult enc;
    if (encodeWideString(decodedBytes, elementCore, enc)) return InvalidType;   // fail loud later
    return interner.array(interner.primitive(elementCore),
                          static_cast<std::int64_t>(enc.codeUnits + 1));
}

// C 6.7.9p14 (D-CSUBSET-STRING-LITERAL-ARRAY-ZERO-FILL): is the initializer subtree
// a STRING LITERAL (`= "..."`)? Descends the SINGLE-child wrapper chain from the
// init node (initValue → expression → operand → … → stringLiteralExpr) — the same
// chain the scalar-init `initTy` probe walks — and returns true when it reaches the
// `stringLiteralExpr` rule node. A multi-child node (a brace-init list, a binary
// expression) stops the descent and yields false. The scalar VarDecl assignability
// check passes the result to `isAssignable` so a `char[N] <- char[M]` admission
// fires ONLY for a genuine string-literal initializer (never an array-to-array
// init, which C forbids anyway). False when the grammar has no stringLiteralExpr
// rule (toy/tsql).
[[nodiscard]] bool initIsStringLiteral(EngineState const& s, Tree const& tree,
                                       NodeId initNode) {
    if (!s.idx().stringLiteralExprRule.valid()) return false;
    for (NodeId walk = initNode; walk.valid();) {
        if (tree.kind(walk) == NodeKind::Internal
            && tree.rule(walk).v == s.idx().stringLiteralExprRule.v) {
            return true;
        }
        auto wk = visibleChildren(tree, walk);
        if (wk.size() != 1) break;
        walk = wk[0];
    }
    return false;
}

// ── literal typing (the ONE chokepoint) ────────────────────────────────────
// Types ONE node if it is a literal: a literal TOKEN (the integer ladder /
// float suffix / wide-char override / bool-keyword / nullptr rows of
// `literalTypeIds`, plus the per-token string fallback) or a
// `stringLiteralExpr` RULE node (the adjacent-concat form). Non-literal nodes
// are untouched. EXTRACTED from pass2Post (byte-identical body — the
// buildConstEvalEnv extraction precedent) so TWO tiers share it:
//   • Pass 2 (pass2Post) — the blanket post-order walk, exactly as before;
//   • FC17.5 (D-CSUBSET-AUTO-TYPE-INFERENCE): the Pass-1.5 inference arm's
//     initializer PRE-STAMP. `subtreeType` types identifier leaves by scope
//     lookup but relies on `typeAt` stamps for literal leaves (Pass-2-stamped
//     on the blanket walk) — so at Pass 1.5 a literal initializer
//     (`auto x = 42;`) would resolve to InvalidType (probe-confirmed: the
//     typeof-of-literal analog `typeof(42) a = 7; int arr[sizeof(a)];` fails
//     S_NonConstantArrayLength at HEAD — p1 passes only via the Pass-2
//     initializer backfill). The arm walks the initializer subtree through
//     THIS chokepoint first, so both tiers type literals identically by
//     construction. Idempotent (re-stamping computes the same TypeId); the
//     one diagnostic inside (S_IntegerLiteralTooLarge) leaves the node
//     untyped on BOTH tiers, and its Pass-1.5 + Pass-2 double-visit
//     collapses in the reporter's recent-duplicate window (suppressible
//     code — the dedup applies; the typeof bit-field gate precedent).
void typeLiteralIfAny(EngineState& s, SemanticConfig const& cfg,
                      Tree const& tree, NodeId node) {
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
                // C23 6.4.4.1 (D-CSUBSET-BITINT-WIDE-LITERAL / Fork-1b): a `wb`/`uwb`
                // bit-precise literal is typed `[unsigned] _BitInt(N)` with N derived
                // from its ARBITRARY-MAGNITUDE value (`decodeBigInteger`, which the u64
                // ladder cannot hold). Checked BEFORE the magnitude ladder so a `>u64`
                // uwb literal is not mis-diagnosed S_IntegerLiteralTooLarge. I4: an N
                // above __BITINT_MAXWIDTH__ reuses the specifier's over-width path.
                if (auto const bpSigned = bitPreciseLiteralSignedness(
                        text, s.idx().numberStyle, cfg.integerLiteralTyping)) {
                    if (auto mag = decodeBigInteger(text, s.idx().numberStyle)) {
                        BitIntValue const bv =
                            BitIntValue::fromLiteralMagnitude(*mag, *bpSigned);
                        if (bv.width() > kBitIntMaxWidth) {
                            ParseDiagnostic d;
                            d.code     = DiagnosticCode::S_BitIntWidthExceedsMax;
                            d.severity = DiagnosticSeverity::Error;
                            d.buffer   = tree.source().id();
                            d.span     = tree.span(node);
                            d.actual   = std::format("`_BitInt` literal width {} exceeds "
                                                     "__BITINT_MAXWIDTH__ ({})",
                                                     bv.width(), kBitIntMaxWidth);
                            s.reporter.report(std::move(d));
                            return;   // leave untyped (cascade-suppress)
                        }
                        s.nodeToType.set(node, s.lattice.interner().bitInt(
                            static_cast<std::int64_t>(bv.width()), *bpSigned));
                        return;   // fully typed as _BitInt(N); skip the ladder
                    }
                    // A bit-precise suffix with no base-valid digits is malformed —
                    // fall through to the standard ladder's fail-loud below.
                }
                auto const magnitude = decodeInteger(text, s.idx().numberStyle);
                bool fits = magnitude.has_value();
                if (fits) {
                    auto const r = typeIntegerLiteral(
                        text, s.idx().numberStyle, cfg.integerLiteralTyping,
                        s.dataModel, *magnitude);
                    switch (r.status) {
                        case IntegerLadderStatus::Typed:
                            litTy = s.lattice.interner().primitive(r.kind,
                                                                   r.vocabularyName);
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
                    cfg.floatLiteralTyping, s.dataModel, s.longDoubleFormat);
                if (fk.status == FloatLadderStatus::NoRule) {
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
                if (fk.status == FloatLadderStatus::AxisUndeclared) {
                    // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): a long-double
                    // literal (`20.0L`) on a format with no declared axis —
                    // its representation is unknowable; leave the token
                    // UNTYPED (never the base core) and emit the precise
                    // diagnostic, mirroring the typeSpecifiers bind.
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::S_LongDoubleFormatUndeclared;
                    d.severity = DiagnosticSeverity::Error;
                    d.buffer   = tree.source().id();
                    d.span     = tree.span(node);
                    d.actual   = std::format(
                        "'{}' is a long double literal, not realized on this "
                        "object format: the format declares no "
                        "longDoubleFormat axis", tree.text(node));
                    s.reporter.report(std::move(d));
                    return;
                }
                litTy = s.lattice.interner().primitive(fk.kind, fk.vocabularyName);
            }
            // C11/C23 6.4.4.4: a PREFIXED character constant (`L'x'`/`u'x'`/`U'x'`/
            // `u8'x'`) has the SCALAR type of its prefix (wchar_t/char16_t/char32_t/
            // char8_t), NOT `int`. The narrow `'x'` keeps its flat `literalTypeIds`
            // type (`int` / I32) UNTOUCHED — other consumers key on that entry
            // (`integerLiteralTokenSet`, `isLiteralIntegerZero`). ONLY a wide opener
            // OVERRIDES: look up the parent charLiteralExpr's opener in the WIDE-ONLY
            // `charLiteralElementCoreByStart` map (format-resolved at index build). A
            // wide char whose single code point does NOT fit its element (astral under
            // char16_t, `u8'`>U+007F, empty/multi-char/ill-formed) leaves the body
            // token UNTYPED so a `sizeof`/`_Alignof` of it fails loud (never a guessed
            // size) — the HIR tier emits the specific diagnostic. The decode+validate
            // is the SHARED `decodeWideCharCodepoint` the HIR tier runs, so both tiers
            // agree on representability.
            if (s.idx().charLiteralBodyToken.valid()
                && tk == s.idx().charLiteralBodyToken) {
                if (auto wideCore = charLiteralWideCoreOf(s, tree, tree.parent(node))) {
                    if (decodeWideCharCodepoint(tree.text(node), *wideCore)) {
                        litTy = s.lattice.interner().primitive(*wideCore);
                    } else {
                        return;   // unrepresentable — leave untyped (sizeof fails loud)
                    }
                }
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
                // Per-token fallback (grammars with no stringLiteralExpr rule —
                // toy/tsql, or a body not under it): the opener is the token's
                // parent. Those grammars declare no prefix map, so the core is the
                // scalar narrow default and the length is the byte count (byte-
                // identical). Route through the shared helper so a future prefixed
                // grammar without the concat rule still types correctly.
                // SINGLE-opener path (one body token's parent) → `conflict` is
                // structurally impossible; take the core.
                TypeKind const core = stringLiteralElementCoreOf(s, tree, tree.parent(node)).core;
                if (TypeId const arr = stringLiteralArrayType(s, *decoded, core); arr.valid()) {
                    s.nodeToType.set(node, arr);
                }
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
        EscapeDecodeOutcome outcome;
        if (auto decoded = decodeAdjacentStringBodies(
                tree, node, s.idx().stringLiteralBodyToken, &outcome)) {
            // C11/C23 6.4.5 / 6.4.5p5: the element core is keyed by the run's
            // EFFECTIVE prefix (the single distinct non-narrow opener among ALL
            // segments; narrow segments widen to it), and the array length is the
            // code-unit count for a wide core (via the shared encoder).
            StringLiteralConcatCore const coreInfo = stringLiteralElementCoreOf(s, tree, node);
            if (coreInfo.conflict) {
                // MF1 / N6 (6.4.5p5): the run mixes two DIFFERENT non-narrow prefixes
                // (`u"a" U"b"`) — the impl-defined case this implementation rejects.
                // Emit the reason HERE (so a `sizeof`/`_Alignof` of it reports the real
                // conflict, not a bare sizeof-of-untyped cascade) and leave the node
                // UNTYPED so a `sizeof` of it fails loud. The HIR tier emits the same
                // code + an Error node when the run is lowered as a value.
                ParseDiagnostic d;
                d.code     = DiagnosticCode::H_ConflictingStringLiteralPrefixes;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = tree.source().id();
                d.span     = tree.span(node);
                d.actual   = std::string{tree.text(node)};
                s.reporter.report(std::move(d));
            } else {
                TypeKind const core = coreInfo.core;
                // FF3 (D-CSUBSET-WIDE-HEX-OCTAL-ESCAPE-VALUE): a `\x`/octal byte escape
                // in a WIDE/UTF string names a raw code-unit value, not a code point —
                // leave the node UNTYPED so a `sizeof` of it fails loud, matching the
                // HIR tier. Narrow `\x`/octal is byte-producing and stays typed.
                bool const wideByteEscape = outcome.usedByteEscape
                    && core != TypeKind::Char && core != TypeKind::Byte;
                if (!wideByteEscape) {
                    if (TypeId const arr = stringLiteralArrayType(s, *decoded, core); arr.valid()) {
                        s.nodeToType.set(node, arr);
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

    // C23 §6.5 (D-CSUBSET-NULLPTR): the fail-loud operator gate. `nullptr`
    // (TypeKind::NullptrT) is a null pointer constant, NOT an arithmetic value;
    // under the HIR lowering it becomes the integer-0 null constant, so WITHOUT this
    // explicit gate an invalid operand use (`nullptr + 1`, `nullptr < p`, `-nullptr`,
    // `nullptr == 5`) would SILENTLY compile as `0 + 1` / `0 < p` / `-0` / `0 == 5`.
    // nullptr is admissible ONLY as an `==`/`!=` operand against a pointer or another
    // nullptr; the CONVERSION contexts (assign/init/arg/return via isAssignable, and
    // `if(nullptr)`/`!nullptr` via the HIR condition lowering to the integer-0 arm)
    // are handled elsewhere. Detecting a NullptrT operand uses a BOUNDED transparent-
    // descent — O(1): it follows thin single-Internal-child wrappers/parens to a
    // nullptr token or an already-stamped NullptrT node — so the common (no-nullptr)
    // path pays ~nothing, and a peer's full type is computed only once a nullptr
    // operand is actually present. A NullptrT value arising from a NON-literal
    // subexpression (`(c?nullptr:nullptr)+1`, astronomically rare) is not caught here
    // — it degrades to defined 0-arithmetic, never a crash: D-CSUBSET-NULLPTR-
    // NONLITERAL-OPERAND (trigger: a real program hits it).
    if (k == NodeKind::Internal
        && cfg.pointerConversions.nullPointerConstantFromNullptrT) {
        auto const& hirCfg = tree.schema().hirLowering();
        std::uint32_t const rule = tree.rule(node).v;
        bool const isBin = hirCfg.binaryExprRule.valid()
            && rule == hirCfg.binaryExprRule.v;
        bool const isUn  = hirCfg.unaryExprRule.valid()
            && rule == hirCfg.unaryExprRule.v;
        if (isBin || isUn) {
            auto& interner = s.lattice.interner();
            auto const isNullptrOperand = [&](NodeId n) -> bool {
                for (int guard = 0; n.valid() && guard < 64; ++guard) {
                    if (TypeId t = s.typeAt(n); t.valid())
                        return interner.kind(t) == TypeKind::NullptrT;
                    if (tree.kind(n) == NodeKind::Token) {
                        auto it = s.idx().literalTypeIds.find(tree.tokenKind(n).v);
                        return it != s.idx().literalTypeIds.end()
                            && interner.kind(it->second) == TypeKind::NullptrT;
                    }
                    // Descend one transparent layer: the single meaningful child is
                    // an Internal child (a paren / thin wrapper) if present, else a
                    // literal token child (an operand node wrapping the `nullptr`
                    // literal — internals == 0). More than one Internal child (a real
                    // operator / ternary / comma) is NOT a transparent nullptr operand.
                    NodeId nextInternal{}, litToken{};
                    int internals = 0;
                    for (NodeId c : visibleChildren(tree, n)) {
                        if (tree.kind(c) == NodeKind::Token) {
                            if (s.idx().literalTypeIds.find(tree.tokenKind(c).v)
                                != s.idx().literalTypeIds.end()) {
                                litToken = c;
                            }
                        } else {
                            ++internals; nextInternal = c;
                        }
                    }
                    if (internals == 1) { n = nextInternal; continue; }
                    if (internals == 0 && litToken.valid()) { n = litToken; continue; }
                    return false;
                }
                return false;
            };
            auto const opEntry =
                [&](std::vector<HirOperatorEntry> const& ops)
                -> HirOperatorEntry const* {
                for (NodeId c : visibleChildren(tree, node)) {
                    if (tree.kind(c) != NodeKind::Token) continue;
                    for (auto const& e : ops)
                        if (e.token.v == tree.tokenKind(c).v) return &e;
                }
                return nullptr;
            };
            auto const emitInvalid = [&](NodeId at) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::S_NullptrInvalidOperand;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = tree.source().id();
                d.span     = tree.span(at);
                d.actual   = std::string{tree.text(at)};
                s.reporter.report(std::move(d));
            };
            if (isBin) {
                NodeId lhsN{}, rhsN{};
                for (NodeId c : visibleChildren(tree, node)) {
                    if (tree.kind(c) == NodeKind::Token) continue;
                    if (!lhsN.valid()) lhsN = c; else if (!rhsN.valid()) rhsN = c;
                }
                bool const lNull = lhsN.valid() && isNullptrOperand(lhsN);
                bool const rNull = rhsN.valid() && isNullptrOperand(rhsN);
                if (lNull || rNull) {
                    HirOperatorEntry const* e = opEntry(hirCfg.binaryOps);
                    std::optional<HirOpKind> const op =
                        e ? coreOpFromNameSem(e->target) : std::nullopt;
                    bool ok;
                    if (!op.has_value()) {
                        // A special-tag operator (not a core op): plain Assign
                        // (`p = nullptr`, validated by isAssignable), Comma
                        // (`(nullptr, x)` discard), and LogicalAnd/LogicalOr
                        // (nullptr in a boolean context) all ACCEPT nullptr. A
                        // COMPOUND assignment (`p += nullptr`, compoundBase
                        // non-empty) is pointer arithmetic → REJECT. A null entry
                        // (unclassifiable) is left alone (no false positive).
                        ok = (e == nullptr) || e->compoundBase.empty();
                    } else if (*op == HirOpKind::Eq || *op == HirOpKind::Ne) {
                        if (lNull && rNull) {
                            ok = true;   // nullptr == nullptr (C23-valid)
                        } else {
                            // The non-nullptr peer must DECAY to a pointer: an
                            // object/function pointer (Ptr), a bare function
                            // designator (FnSig → function pointer, C 6.3.2.1p4),
                            // or an array name (Array → element pointer, 6.3.2.1p3)
                            // — `func == nullptr` / `arr == nullptr` are valid C23.
                            // Mirrors the combineTernary nullptr arm's Ptr/FnSig decay.
                            NodeId const peer = lNull ? rhsN : lhsN;
                            TypeId const pt = subtreeType(s, tree, peer, here);
                            TypeKind const pk =
                                pt.valid() ? interner.kind(pt) : TypeKind::Void;
                            ok = (pk == TypeKind::Ptr || pk == TypeKind::FnSig
                                  || pk == TypeKind::Array);
                        }
                    } else {
                        // A core arithmetic / relational / bitwise / shift op —
                        // nullptr is not a valid operand.
                        ok = false;
                    }
                    if (!ok) emitInvalid(lNull ? lhsN : rhsN);
                }
            } else {  // unary
                NodeId operandN{};
                for (NodeId c : visibleChildren(tree, node)) {
                    if (tree.kind(c) == NodeKind::Token) continue;
                    operandN = c; break;
                }
                if (operandN.valid() && isNullptrOperand(operandN)) {
                    HirOperatorEntry const* e = opEntry(hirCfg.unaryOps);
                    std::optional<HirOpKind> const op =
                        e ? coreOpFromNameSem(e->target) : std::nullopt;
                    // Reject `-nullptr` (Neg) and `~nullptr` (BitNot). `!nullptr`
                    // (Not) is VALID (nullptr→false→true); `&nullptr` is caught by
                    // the existing lvalue check (nullptr is a non-lvalue like 0/true).
                    if (op.has_value()
                        && (*op == HirOpKind::Neg || *op == HirOpKind::BitNot)) {
                        emitInvalid(operandN);
                    }
                }
            }
        }
    }

    // Literal typing — the extracted shared chokepoint (see typeLiteralIfAny
    // above; FC17.5 D-CSUBSET-AUTO-TYPE-INFERENCE made it two-tier). This
    // call IS the pre-extraction block, byte-identical behavior.
    typeLiteralIfAny(s, cfg, tree, node);

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
                        // FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS, C23 6.7.13.3):
                        // a USE of a `[[deprecated]]` symbol warns HERE — the
                        // single reference-resolution chokepoint, so every use
                        // site fires once (incl. a call's callee, which is an
                        // operand-rule reference). Decl sites never reach this
                        // arm (the isDeclSite gate above). Suppressible
                        // Warning; `.actual` = name or "name: msg". Deprecated
                        // TYPES (tags/typedefs resolve via type resolution, not
                        // here) are the named deferral
                        // D-CSUBSET-ATTRIBUTE-DEPRECATED-TYPES.
                        if (rec.isDeprecated) {
                            ParseDiagnostic d;
                            d.code     = DiagnosticCode::S_DeprecatedSymbolUsed;
                            d.severity = DiagnosticSeverity::Warning;
                            d.buffer   = tree.source().id();
                            d.span     = tree.span(resolved.node);
                            d.actual   = rec.deprecatedMessage.empty()
                                ? resolved.name
                                : resolved.name + ": " + rec.deprecatedMessage;
                            s.reporter.report(std::move(d));
                        }
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
        // node `size_t` — the result type for enclosing checks. The operand is
        // UNEVALUATED (C 6.5.3.4); only its type matters. D-LANG-TYPE-IDENTITY-
        // VOCABULARY: `size_t` is C's NAMED alias (`unsigned long` on LP64,
        // `unsigned long long` on LLP64), declared per data model in
        // `semantics.synthesizedTypes` and resolved through `synthesizedType`. A
        // bare anonymous U64 here matched NEITHER named entry, so
        // `_Generic(sizeof(int), unsigned long: 1, unsigned long long: 2,
        // default: 0)` silently took `default`. The core still comes from the
        // TARGET (both current models make it 64-bit), so this also carries the
        // ILP32 width for free once such a target exists.
        if (cfg.sizeofTypeRule.valid() && rule.v == cfg.sizeofTypeRule.v) {
            auto kids = visibleChildren(tree, node);
            if (cfg.sizeofTypeChild < kids.size()) {
                NodeId const typeNode = kids[cfg.sizeofTypeChild];
                TypeId const sized = resolveTypeNode(s, cfg, tree, typeNode, here);
                if (sized.valid()) s.nodeToType.set(typeNode, sized);
                // sized invalid ⇒ resolveTypeNode emitted S_UnknownType (fail loud).
            }
            s.nodeToType.set(node,
                             synthesizedType(s.lattice.interner(),
                                             cfg.sizeofResultType, s.dataModel,
                                             TypeKind::U64));
        }
        // C11/C23 6.5.3.4: `_Alignof(T)` typing — an ADDITIVE mirror of the sizeof
        // TYPE arm. Resolves + stamps its castTypeRef child through the SAME type
        // resolver (so the HIR lowering's `resolveStampedTypeBelow` recovers the
        // queried type) and stamps the node `size_t` (the SAME declared vocabulary
        // entry the sizeof arm mints — C 6.5.3.4p5). Type-name form ONLY (no value
        // arm — `_Alignof(expr)` is a constraint violation the binder rejects at
        // type-resolve).
        if (cfg.alignofTypeRule.valid() && rule.v == cfg.alignofTypeRule.v) {
            auto kids = visibleChildren(tree, node);
            if (cfg.alignofTypeChild < kids.size()) {
                NodeId const typeNode = kids[cfg.alignofTypeChild];
                TypeId const queried = resolveTypeNode(s, cfg, tree, typeNode, here);
                if (queried.valid()) s.nodeToType.set(typeNode, queried);
                // queried invalid ⇒ resolveTypeNode emitted S_UnknownType (fail loud).
            }
            s.nodeToType.set(node,
                             synthesizedType(s.lattice.interner(),
                                             cfg.alignofResultType, s.dataModel,
                                             TypeKind::U64));
        }
        if (cfg.sizeofValueRule.valid() && rule.v == cfg.sizeofValueRule.v) {
            // c89 (D-CSUBSET-SIZEOF-VALUE-OPERAND-TYPE): stamp the VALUE-form
            // operand with its full EXPRESSION type. Post-order means the
            // operand subtree is already typed, and `subtreeType` is the exact
            // deriver the Pass-1.5 array-dimension sizeof closure already uses
            // for this same recovery. Without this stamp the HIR lowering's
            // `resolveStampedTypeBelow` probe descends past the (unstamped)
            // operator nodes to the FIRST STAMPED LEAF, so `sizeof(*p)` /
            // `sizeof(p[0])` / `sizeof(arr[0])` sized the base POINTER/ARRAY,
            // not the element: sqlite's pthreadMutexAlloc `sizeof(*p)` (40)
            // under-allocated as 8 -> glibc pthread_mutex_init wrote the real
            // 40 bytes -> malloc top-chunk clobber -> the deterministic
            // sysmalloc SIGABRT on every sqlite3 invocation; and ArraySize
            // (sizeof(X)/sizeof(X[0])) folded to 1 silently.
            for (NodeId opnd : visibleChildren(tree, node)) {
                if (tree.kind(opnd) != NodeKind::Internal) continue;
                if (TypeId t = subtreeType(s, tree, opnd, here); t.valid()) {
                    s.nodeToType.set(opnd, t);
                }
                break;  // the single operand child
            }
            s.nodeToType.set(node,
                             synthesizedType(s.lattice.interner(),
                                             cfg.sizeofResultType, s.dataModel,
                                             TypeKind::U64));
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
                case St::AmbiguousField: {
                    // FC16 D-CSUBSET-ANON-MEMBER-PROMOTION (C 6.7.2.1 ¶13): the
                    // member name is promoted from ≥2 sibling anonymous members
                    // — ambiguous, fail loud rather than silently picking one.
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::S_RedeclaredSymbol;
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
            // TLS C1 (D-CSUBSET-THREAD-LOCAL): the POSITIONAL-name mirror of
            // the declarator-loop thread-storage hook below — c-subset's
            // externDecl declares a positional `name`, never a declarator
            // list, so its `extern thread_local int e;` / the 6.7.1p4 reject
            // `extern thread_local int f(void);` (S_ThreadLocalOnFunction)
            // validate HERE. Gated on the Pass-1 isThreadLocal mark alone.
            if (!decl.isDeclaratorMode() && decl.nameChild.has_value()
                && *decl.nameChild < kids.size()) {
                NodeId const nm = kids[*decl.nameChild];
                SymbolId const nmSym = s.symbolAtOr(nm);
                if (nmSym.valid() && s.symbols.at(nmSym).isThreadLocal) {
                    validateThreadLocalDeclarator(s, cfg, tree, node, decl,
                                                  nm, nmSym);
                }
            }
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
                        // FC17 (D-CSUBSET-CONSTEXPR) — F1: the constexpr
                        // enforcement hook sits ABOVE the initDeclaratorRule
                        // gate below, because collectDeclarators admits BARE
                        // declarator carriers (rule == declaratorRule — a
                        // missing-initializer `constexpr int x;`, a function
                        // declarator `constexpr int f(void) {…}`) that the
                        // gate `continue`s past — a post-gate hook would
                        // silently skip EXACTLY the declarators the
                        // missing-init / function constraints exist for
                        // (incl. wrongly giving `constexpr int f` the
                        // file-scope internal linkage). Symbol discovery is
                        // the loop's own idiom (declaratorNameNode +
                        // symbolAtOr); gated on the language declaring a
                        // constexpr surface, then on the Pass-1 mark.
                        // TLS C1 (D-CSUBSET-THREAD-LOCAL): the thread-storage
                        // hook shares the SAME pre-gate placement (a bare
                        // `static thread_local int x;` declarator and a
                        // `thread_local int f(void)` function declarator are
                        // exactly the carriers the gate skips) and the same
                        // discovered symbol; gated on the Pass-1 isThreadLocal
                        // mark alone (the mark can only exist when the
                        // language declares a threadStorage row). BOTH
                        // validators may fire on one declarator (`constexpr
                        // thread_local int x = argc;`) — each owns its own
                        // diagnostics, errors accumulate.
                        {
                            NodeId const vName = declaratorNameNode(
                                tree, dNode, *cfg.declarators);
                            SymbolId const vSym = vName.valid()
                                ? s.symbolAtOr(vName) : SymbolId{};
                            if (vSym.valid()) {
                                if (cfg.constexprKeywordToken.has_value()
                                    && s.symbols.at(vSym).isConstexpr) {
                                    validateConstexprDeclarator(
                                        s, cfg, tree, dNode, vName, vSym,
                                        here);
                                }
                                if (s.symbols.at(vSym).isThreadLocal) {
                                    validateThreadLocalDeclarator(
                                        s, cfg, tree, node, decl, vName,
                                        vSym);
                                }
                                // VLA C1a (D-CSUBSET-VLA): the type arm builds a
                                // vlaArray REGARDLESS of scope/storage/size-type, so
                                // this Pass-2 validator owns ALL the VLA constraints —
                                // a FILE-scope VLA (`int g[n]`, whose vlaArray the arm
                                // DID build) is rejected here by its rec.scope
                                // file-scope branch (S_NonConstantArrayLength), a
                                // non-integer size by S_VlaSizeNotInteger, and a
                                // block-scope static/extern by S_VlaWithStaticStorage.
                                // `dNode` is THIS declarator (the length-node scan
                                // needs it). The positional-name externDecl mirror
                                // below needs no VLA hook (it is declarator-mode-free
                                // and file-scope — but even a positional VLA would be
                                // caught by the file-scope branch if one existed).
                                // VLA C3 (D-CSUBSET-VLA): `||typeContainsVla` so a
                                // FIXED-outer multi-dim VLA (`int a[5][n]` — whose top
                                // type is a fixed Array, NOT isVlaArray) is ALSO routed
                                // to the constraint validator (file-scope / static /
                                // per-dim non-integer rejects apply to it too).
                                if (s.lattice.interner().isVlaArray(
                                        s.symbols.at(vSym).type)
                                    || s.lattice.interner().typeContainsVla(
                                        s.symbols.at(vSym).type)) {
                                    validateVlaDeclarator(s, cfg, tree, node, dNode,
                                                          decl, vName, vSym);
                                }
                            }
                        }
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
                        // VLA C4a-local (D-CSUBSET-VLA-PTR-INIT-FORM-TYPING, DEFERRED):
                        // the natural init form `int (*p)[n] = b` does NOT compile — the
                        // typeAt walk (and `subtreeType`, which short-circuits on the same
                        // stamped node) yields `b`'s DECAYED pointer type, not the raw
                        // `array(vlaArray(int),2)` the `Ptr<vlaArray> ← array(array)` decay
                        // compare needs, so S_TypeMismatch fires (a CLEAN fail-loud, never
                        // a silent miscompile). A guarded `subtreeType` override was tried
                        // (CRITICAL-1) but is inert here because `b`'s initializer node is
                        // pre-stamped decayed by an earlier pass. Deferred; the C4a-local
                        // witness uses the ASSIGNMENT form (`int (*p)[n]; p = b;`), which
                        // passes assignability and reaches the runtime-stride path.
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
                                                    /*charConvertsToArith=*/cfg.charConvertsToArith, /*enumConvertsToArith=*/cfg.enumConvertsToArith, /*intCrossSignednessConverts=*/cfg.intCrossSignednessConverts, /*intSameSignednessNarrows=*/cfg.intSameSignednessNarrows, /*intConvertsToFloat=*/cfg.intConvertsToFloat, /*floatConvertsToInt=*/cfg.floatConvertsToInt, /*charArrayFromStringLiteralInit=*/initIsStringLiteral(s, tree, initNode), /*bitIntConversions=*/cfg.bitIntConversions)
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
                                                    /*charConvertsToArith=*/cfg.charConvertsToArith, /*enumConvertsToArith=*/cfg.enumConvertsToArith, /*intCrossSignednessConverts=*/cfg.intCrossSignednessConverts, /*intSameSignednessNarrows=*/cfg.intSameSignednessNarrows, /*intConvertsToFloat=*/cfg.intConvertsToFloat, /*floatConvertsToInt=*/cfg.floatConvertsToInt, /*charArrayFromStringLiteralInit=*/initIsStringLiteral(s, tree, initNode), /*bitIntConversions=*/cfg.bitIntConversions)
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
                                             /*intSameSignednessNarrows=*/cfg.intSameSignednessNarrows,
                                             /*intConvertsToFloat=*/cfg.intConvertsToFloat,
                                             /*floatConvertsToInt=*/cfg.floatConvertsToInt,
                                             /*charArrayFromStringLiteralInit=*/false,
                                             /*bitIntConversions=*/cfg.bitIntConversions)
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

    // C11/C23 6.7.10 (D-CSUBSET-STATIC-ASSERT): a `_Static_assert`/`static_assert`
    // static-assertion declaration. The construct is valid at BOTH file and block
    // scope; `pass2Post` visits it in both because it walks EVERY node (the check
    // is node-based, not symbol-based — the declaration mints no symbol). The
    // condition is const-evaluated through the SAME `constIntExpr` evaluator that
    // folds `sizeof(T)` / enum constants / arithmetic in an array dimension (it
    // wires `resolveSizeof` off `s.aggregateLayout`), so `sizeof(int)==4` folds
    // here exactly as `int a[sizeof(int)]` does. A fold to ZERO — OR a condition
    // that does not fold to an integer constant expression (non-const / float /
    // unresolved; C 6.7.10 requires an ICE) — fails loud with S_StaticAssertFailed.
    // A NONZERO fold produces nothing (the construct also lowers to nothing: its
    // hirLowering row maps to Skip). The child roles are POSITIONAL, matching the
    // grammar `keyword '(' condition [ ',' message ] ')' ';'`: the FIRST meaningful
    // (Internal) child is the condition; a SECOND, when present, is the message
    // string-literal expression.
    if (k == NodeKind::Internal
        && s.idx().staticAssertByRule.contains(tree.rule(node).v)) {
        NodeId condNode{};
        NodeId msgNode{};
        for (NodeId c : visibleChildren(tree, node)) {
            if (tree.kind(c) != NodeKind::Internal) continue;   // skip kw / ( ) ; ,
            if (!condNode.valid())      condNode = c;
            else if (!msgNode.valid())  msgNode  = c;
        }
        // Decode the message (an adjacent-string-concat literal) via the SHARED
        // chokepoint — the exact bytes the string appears as in source. Absent (the
        // C23 1-arg form) or a malformed escape ⇒ no message text; the diagnostic
        // still fires (an assertion with an unreadable message is still an
        // assertion). The message is decoration only — never the pass/fail input.
        std::string message;
        if (msgNode.valid() && s.idx().stringLiteralBodyToken.valid()) {
            if (auto decoded = decodeAdjacentStringBodies(
                    tree, msgNode, s.idx().stringLiteralBodyToken)) {
                message = std::move(*decoded);
            }
        }
        std::optional<std::int64_t> folded;
        if (condNode.valid()) folded = constIntExpr(s, tree, condNode, here, &cfg);
        bool const failed = !folded.has_value() || *folded == 0;
        if (failed) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::S_StaticAssertFailed;
            d.severity = DiagnosticSeverity::Error;
            d.buffer   = tree.source().id();
            d.span     = tree.span(node);
            // The message discriminates the two failure modes on ONE code: a
            // FALSE assertion carries the author's string; a NON-CONSTANT
            // condition says so (C requires an integer constant expression).
            if (!folded.has_value()) {
                d.actual = "static assertion condition is not an integer constant "
                           "expression";
                if (!message.empty())
                    d.actual += ": \"" + message + "\"";
            } else {
                d.actual = message.empty()
                               ? std::string{"static assertion failed"}
                               : "static assertion failed: \"" + message + "\"";
            }
            s.reporter.report(std::move(d));
        }
    }

    // FC17.9(i) (D-CSUBSET-INLINE-ASM, C23 6.8 / GNU 6.47): the GNU inline-asm
    // STATEMENT `__asm__ [volatile] ( <string-literal-template> ) ;`. Cycle-1
    // implements ONLY the empty-template optimizer barrier: the template must
    // decode to STRICTLY ZERO bytes (`__asm__ volatile("")`), which HIR→MIR lowers
    // to a pure compiler reordering + full-memory fence (MirOpcode::CompilerBarrier,
    // the _ReadWriteBarrier op). A NON-EMPTY template carries real per-target
    // instructions we cannot yet emit — decode the template (the SAME chokepoint
    // staticAssert's message uses) and FAIL LOUD S_InlineAsmNonEmptyTemplate on
    // anything but a strictly-empty decoded string: non-empty text, whitespace-only
    // (`"  "` is NOT provably inert — we do not parse asm), or a malformed escape
    // (decode → nullopt). This reject is an Error → the driver aborts before codegen,
    // so a rejected asm's MIR is never emitted; silently lowering a non-empty asm to
    // a no-op barrier would DROP its instructions — a miscompile — so the code is
    // UNSUPPRESSABLE (unsuppressable_codes.cpp). Operand/clobber lists (`: … : …`) and
    // `asm goto` never reach here: the asmStmt grammar ends at `)`, so those fail loud
    // at parse (P_UnexpectedToken) — the D-CSUBSET-INLINE-ASM-OPERANDS / -GOTO
    // deferrals. `volatile` is accepted but semantically INERT for the empty form.
    // The template is the SOLE Internal child (the keyword / volatile / parens /
    // semicolon are Tokens), located exactly as staticAssert locates its message.
    if (k == NodeKind::Internal
        && s.idx().inlineAsmByRule.contains(tree.rule(node).v)) {
        NodeId tmplNode{};
        for (NodeId c : visibleChildren(tree, node)) {
            if (tree.kind(c) != NodeKind::Internal) continue;   // skip kw / volatile / ( ) ;
            tmplNode = c;
            break;
        }
        std::optional<std::string> decoded;
        if (tmplNode.valid() && s.idx().stringLiteralBodyToken.valid()) {
            decoded = decodeAdjacentStringBodies(
                tree, tmplNode, s.idx().stringLiteralBodyToken);
        }
        // Acceptance = a template that decodes to STRICTLY zero bytes. Anything else
        // (non-empty / whitespace-only / malformed-escape nullopt / a malformed node
        // with no template) fails loud — never a silently dropped instruction.
        if (!(decoded && decoded->empty())) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::S_InlineAsmNonEmptyTemplate;
            d.severity = DiagnosticSeverity::Error;
            d.buffer   = tree.source().id();
            d.span     = tree.span(tmplNode.valid() ? tmplNode : node);
            d.actual   = "inline asm with a non-empty template is not yet supported "
                         "(only the empty-template optimizer barrier "
                         "`__asm__ volatile(\"\")` is implemented); real asm text is a "
                         "per-target deferral (D-CSUBSET-INLINE-ASM-TEXT)";
            s.reporter.report(std::move(d));
        }
    }

    // FC17 (D-CSUBSET-ATTRIBUTE-STATEMENT, C23 6.8.1): a BARE attribute-
    // declaration statement (`[[fallthrough]];` / `__attribute__((...));`).
    // No symbol to mark — the scan runs for its UNKNOWN-standard-attribute
    // warning only (`[[frobnicate]];` warns suppressibly; the folded facts are
    // discarded). The construct lowers to nothing (hirLowering row → Skip);
    // DSS emits no fallthrough-placement warning to suppress, so
    // `[[fallthrough]]` is semantics-neutral by construction (design note F7).
    if (k == NodeKind::Internal
        && cfg.attrBareStatementRule.valid()
        && tree.rule(node).v == cfg.attrBareStatementRule.v) {
        AttributeSemanticsFacts discardedFacts;
        scanAttributeSemantics(s, cfg, tree, node, /*emitUnknown=*/true,
                               discardedFacts);
    }

    // FC16 C11/C23 6.5.1.1 (D-CSUBSET-GENERIC-SELECTION): a `_Generic ( ctrl ,
    // assoc-list )` generic selection. The SELECTION is a compile-time decision
    // made here (the point with the resolved controlling type + the machinery to
    // resolve each association's type-name), mirroring how `va_arg` resolves its
    // type child and `static_assert` const-evaluates its condition. Post-order
    // means the controlling expression is already typed; each typed association's
    // castTypeRef is resolved through the SAME resolver casts/sizeof/va_arg use
    // (a VALUE in type position fails loud S_UnknownType). The controlling type
    // is lvalue-converted (top-level volatile stripped — the isAssignable
    // precedent; C 6.3.2.1 also drops the qualifiers, and c-subset does not
    // materialize const). A typed association MATCHES when its resolved type is
    // COMPATIBLE with the controlling type — interned TypeId equality (`sameType`)
    // after stripping each association type's own top-level volatile. C 6.5.1.1p2
    // requires EXACTLY ONE match or the `default`: zero-and-no-default is
    // S_GenericSelectionNoMatch, two-or-more is S_GenericSelectionAmbiguous (with
    // interned equality, two matches means two associations named the same type).
    // On success the genericExpr node is stamped the WINNER's result type (so the
    // enclosing expression types + the HIR lowering's `typeAt` probe find it) and
    // the winning association's result-expression NodeId is recorded in
    // `nodeToSelectedExpr` (so `lowerGeneric` lowers ONLY that sub-expression).
    // The NON-selected associations are UNEVALUATED (6.5.1.1p3) — they are parsed
    // and their type-names resolved (a value in type position of ANY association
    // is still a constraint violation), but their result EXPRESSIONS impose no
    // constraints and are never lowered; see the pinned deferral in the plan.
    if (k == NodeKind::Internal
        && cfg.genericRule.valid()
        && tree.rule(node).v == cfg.genericRule.v) {
        // Type the controlling expression ONCE, at top level (subtreeType is a
        // flat work-stack internally — a controlling-nested `_Generic` never host-
        // recurses), then run the shared selection chokepoint on that type. The
        // helper is FULLY SILENT; pass2Post owns every side effect it is free of —
        // the loud assoc-type resolves + stamps, the ambiguous/no-match
        // diagnostics, and the node/selectedExpr stamps.
        auto genericKids = visibleChildren(tree, node);
        TypeId const ctrlTy = cfg.genericControlChild < genericKids.size()
            ? subtreeType(s, tree, genericKids[cfg.genericControlChild], here)
            : InvalidType;
        GenericSelection const sel =
            selectGenericAssociation(s, cfg, tree, node, here, ctrlTy);

        // Re-resolve each typed association's type-name LOUD (the helper's resolves
        // were rolled back) — this is the SOLE emitter of the assoc-type
        // constraint diagnostics (S_UnknownType for a value in type position, the
        // `_BitInt`/typeof-bitfield family) and stamps the valid ones, exactly as
        // the single pre-chokepoint walk did.
        for (NodeId const typeNode : sel.typedAssocTypeNodes) {
            TypeId const assocTy = resolveTypeNode(s, cfg, tree, typeNode, here);
            if (assocTy.valid()) s.nodeToType.set(typeNode, assocTy);
        }

        // Resolve the selection + fail loud. C 6.5.1.1p2 requires EXACTLY ONE
        // typed match or the `default`. A bad controlling type or a rejected
        // association type-name already emitted its own diagnostic; only CASCADE-
        // SUPPRESS the no-match report in that case (never a double error),
        // exactly like the va_arg invalid-type path.
        NodeId const selected = sel.selected;

        if (sel.matchCount > 1) {
            // FC17.9(e) (D-CSUBSET-LONG-DOUBLE-GENERIC-DISTINCT): on an
            // f64-axis format `double:` and `long double:` associations BOTH
            // intern F64 → matchCount 2 lands HERE — LOUD (the LLP64
            // long≡int collapse precedent), but it rejects valid C11 6.5.1.1
            // (the two are distinct TYPES even when same-representation).
            // <tgmath.h> (FC17.9(g)) dispatches on exactly that pair;
            // distinct-type identity for _Generic rides that arc.
            ParseDiagnostic d;
            d.code     = DiagnosticCode::S_GenericSelectionAmbiguous;
            d.severity = DiagnosticSeverity::Error;
            d.buffer   = tree.source().id();
            d.span     = tree.span(node);
            d.actual   = "_Generic controlling type matches more than one "
                         "association";
            s.reporter.report(std::move(d));
        } else if (!selected.valid() && sel.controllingResolved
                   && !sel.anyBadType) {
            // No typed match AND no default — and the failure is NOT a cascade
            // from an unresolved controlling type / bad association type-name.
            ParseDiagnostic d;
            d.code     = DiagnosticCode::S_GenericSelectionNoMatch;
            d.severity = DiagnosticSeverity::Error;
            d.buffer   = tree.source().id();
            d.span     = tree.span(node);
            d.actual   = "_Generic controlling type matches no association and "
                         "there is no default";
            s.reporter.report(std::move(d));
        }

        if (selected.valid()) {
            // The genericExpr's RESULT type is the selected association's result-
            // expression type; record the selected node for the HIR lowering.
            TypeId const resultTy = subtreeType(s, tree, selected, here);
            if (resultTy.valid()) s.nodeToType.set(node, resultTy);
            s.nodeToSelectedExpr.set(node, selected);
        }
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
            // c32 D-CSUBSET-FNPTR-PARAM-SCOPE: re-derive via the SHARED predicate
            // (identical to Pass 1 / Pass 1.5) so a prototype param binds and is
            // looked up in the SAME scope — anchor lookup finds Pass 1's scope.
            ScopeId here = f.current;
            if (nodeOpensChildScope(s, cfg, tree, f.node)) {
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
                                                    /*charConvertsToArith=*/cfg.charConvertsToArith, /*enumConvertsToArith=*/cfg.enumConvertsToArith, /*intCrossSignednessConverts=*/cfg.intCrossSignednessConverts, /*intSameSignednessNarrows=*/cfg.intSameSignednessNarrows, /*intConvertsToFloat=*/cfg.intConvertsToFloat, /*floatConvertsToInt=*/cfg.floatConvertsToInt, /*charArrayFromStringLiteralInit=*/false, /*bitIntConversions=*/cfg.bitIntConversions)) {
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
    // C23 §6.5.2.2 (D-CSUBSET-NULLPTR): `nullptr` passed as a VARIADIC / unprototyped
    // argument (a position with no declared parameter to convert to) is REJECTED
    // fail-loud. nullptr_t undergoes no default argument promotion, so what a
    // matching `va_arg` reads is non-portable; and under the HIR lowering the value
    // would otherwise silently become a plain integer 0. Fixed-parameter positions
    // (i < checkCount) were already checked above — they convert via isAssignable's
    // NullptrT arm. Gated on the same flag; scans only the variadic tail.
    if (variadic && ptrRules.nullPointerConstantFromNullptrT) {
        for (std::size_t i = checkCount; i < argNodes.size(); ++i) {
            TypeId const argTy = subtreeType(s, tree, argNodes[i]);
            if (argTy.valid()
                && s.lattice.interner().kind(argTy) == TypeKind::NullptrT) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::S_NullptrInvalidOperand;
                d.severity = DiagnosticSeverity::Error;
                d.buffer   = tree.source().id();
                d.span     = tree.span(argNodes[i]);
                d.actual   = std::string{tree.text(argNodes[i])};
                s.reporter.report(std::move(d));
            }
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
        // D-CSUBSET-FUNCTION-POINTER-DEREF (c54): the structural deref-peel
        // above descends PAST each `*` token, which is correct only for the
        // function-designator collapse (`*fp == fp` when fp is FnSig /
        // Ptr<FnSig>, C 6.5.3.2p4) but DROPS a real pointer level for a data
        // pointer — so a callee like `(**(finder_type*)pVfs->pAppData)(…)`
        // (SQLite os_unix: `finder_type` is a fn-ptr typedef, so
        // `finder_type*` is Ptr<Ptr<FnSig>>) landed as Ptr<Ptr<FnSig>> and
        // was wrongly rejected S_NotCallable. subtreeType types the WHOLE
        // callee applying derefResultType per `*` (the C-correct per-level
        // reduction), so when it yields a callable type that result GOVERNS.
        // This only UPGRADES the deref case: a non-callable full-callee type
        // leaves landedTy untouched (so `int* p; (*p)(3)` stays
        // S_NotCallable), and the landed-identifier path above is preserved
        // intact for the variadicBuiltin + undeclared-dedup contracts.
        {
            TypeId const calleeExprTy = subtreeType(s, tree, calleeNode);
            if (calleeExprTy.valid()
                && (in.kind(calleeExprTy) == TypeKind::FnSig
                    || isFnPointerType(in, calleeExprTy))) {
                landedTy = calleeExprTy;
            }
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

    // FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS, C23 6.7.13.2): the nodiscard
    // discarded-result warning, in the DIRECT-symbol tail (calleeSym in scope;
    // fn-pointer / expression callees are the named deferral
    // D-CSUBSET-NODISCARD-INDIRECT-CALLEE — the isDirectNoreturnCall scope
    // precedent). The discard context is the TWO-hop shape (design-audit F1):
    // the expression engine materializes an `expression` node between the call
    // and its statement, so the call's result is discarded iff
    // parent(call)==expressionRule AND grandparent(call)==discardStatementRule.
    // By construction this makes `(void)f();` silent (a castExpr interposes)
    // and `x=f()` / `g(f())` / `return f()` no-fire (wrong parent/grandparent).
    // Comma/paren-wrapped discards (`(f());`, `f(), g();`) are the named
    // deferral D-CSUBSET-NODISCARD-INDIRECT-DISCARD-CONTEXT (warning-only
    // miss, never wrong bytes). Suppressible Warning; `.actual` = name or
    // "name: msg".
    if (s.symbols.at(calleeSym).isNodiscard
        && cfg.nodiscardExpressionRule.valid()
        && cfg.nodiscardDiscardStatementRule.valid()) {
        NodeId const p1 = tree.parent(node);
        if (p1.valid() && tree.kind(p1) == NodeKind::Internal
            && tree.rule(p1).v == cfg.nodiscardExpressionRule.v) {
            NodeId const p2 = tree.parent(p1);
            if (p2.valid() && tree.kind(p2) == NodeKind::Internal
                && tree.rule(p2).v == cfg.nodiscardDiscardStatementRule.v) {
                auto const& ndRec = s.symbols.at(calleeSym);
                ParseDiagnostic d;
                d.code     = DiagnosticCode::S_NodiscardResultDiscarded;
                d.severity = DiagnosticSeverity::Warning;
                d.buffer   = tree.source().id();
                d.span     = tree.span(node);
                d.actual   = ndRec.nodiscardMessage.empty()
                    ? resolved.name
                    : resolved.name + ": " + ndRec.nodiscardMessage;
                s.reporter.report(std::move(d));
            }
        }
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
        // D-CSUBSET-BITINT: inject the `_BitInt`-conversions flag (a separate
        // top-level flag, mirroring the cst_to_hir resolve site) so the semantic-
        // tier expression typer agrees with the HIR lowering on a BitInt common type.
        arith->bitIntConversions = sem.bitIntConversions;
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
            // c40 (D-CSUBSET-POINTER-SUBTRACTION): mirror cst_to_hir's
            // combineBinary — `p - q` (both Ptr<T>, same pointee) is ptrdiff_t
            // so it passes as a numeric function ARGUMENT. This is the
            // asymmetry the cycle fixes: a `long n = a - b;` init-check skips the
            // binary node (no error), but a call-arg's `isAssignable(I64param, …)`
            // saw Ptr<T> → S_TypeMismatch; now it sees the integer. Same-pointee
            // only. D-LANG-TYPE-IDENTITY-VOCABULARY: `ptrdiff_t` is C's NAMED
            // alias (`long` on LP64, `long long` on LLP64), declared per data
            // model — a bare anonymous I64 matches NEITHER in a `_Generic`.
            if (*op == HirOpKind::Sub
                && lt.valid() && rt.valid()
                && interner.kind(lt) == TypeKind::Ptr
                && interner.kind(rt) == TypeKind::Ptr
                && interner.operands(lt)[0] == interner.operands(rt)[0]) {
                return synthesizedType(interner, sem.pointerDifferenceType,
                                       s.dataModel, TypeKind::I64);
            }
            // c41 (D-CSUBSET-POINTER-INT-ARITHMETIC): `n + p` (Int LHS, Ptr RHS,
            // the commutative add form) is a POINTER, not the integer. `p + n`
            // and `p - n` already fall through to `lt` (the Ptr) via the default
            // below. This arm only fixes the int-on-LEFT add (else `n + p` would
            // wrongly type as Int → a pointer-arg use would fail isAssignable).
            if (*op == HirOpKind::Add && lt.valid() && rt.valid()
                && interner.kind(lt) != TypeKind::Ptr
                && interner.kind(rt) == TypeKind::Ptr) {
                return rt;   // Ptr<T>
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
    auto const combineTernary = [&](TypeId thenT, TypeId elseT,
                                    NodeId thenN, NodeId elseN) -> TypeId {
        TypeId const common = commonArithType(thenT, elseT);
        if (common.valid()) return common;
        // C 6.5.15p6: a conditional with ONE arm a pointer and the OTHER a
        // null-pointer-constant (a literal integer 0) has the POINTER type. Reuse
        // the existing null-pointer-constant machinery (`isLiteralIntegerZero` —
        // structural, no flag check) gated on the same per-language flag
        // `admitsNullPointerConstant` reads. This MUST win over the first-arm
        // fallback below — else the leading `0` in `cond ? 0 : ptr` would shadow
        // the pointer arm (its int type has no arith rank vs the pointer → Invalid
        // common → first-arm I32). The HIR tier then materializes the
        // `Cast(0 → Ptr)` on the literal-0 arm. Handles BOTH orders. A NON-literal
        // int arm (`n ? n : ptr`) is NOT a null-pointer-constant → falls through.
        if (sem.pointerConversions.nullPointerConstantFromIntegerZero) {
            if (thenT.valid()
                && interner.kind(thenT) == TypeKind::Ptr
                && isLiteralIntegerZero(s, tree, elseN)) {
                return thenT;
            }
            if (elseT.valid()
                && interner.kind(elseT) == TypeKind::Ptr
                && isLiteralIntegerZero(s, tree, thenN)) {
                return elseT;
            }
            // c56 (D-CSUBSET-TERNARY-NULL-FUNCTION-POINTER): the FUNCTION-POINTER
            // sibling of the Ptr arms above. A bare function NAME is a function
            // designator (TypeKind::FnSig, un-decayed at subtreeType); paired with a
            // literal-0 (`cond ? 0 : fn` assigned to a fn-ptr lvalue — e.g. sqlite's
            // `xSelectCallback = (...) ? 0 : resolveSelectStep`), the conditional
            // type is the DECAYED pointer-to-function (C 6.3.2.1p4 function-to-pointer
            // decay + 6.5.15p6). Returning `pointer(FnSig)` (NOT bare FnSig) makes
            // BOTH arms coerce to Ptr<FnSig> at the HIR tier (the literal-0 → null-ptr
            // Cast, the designator → FnSig→Ptr Bitcast) and also fixes the
            // designator-FIRST order, which otherwise mistyped as bare FnSig (its 0
            // arm never got the null-ptr Cast = a silent gap).
            if (thenT.valid()
                && interner.kind(thenT) == TypeKind::FnSig
                && isLiteralIntegerZero(s, tree, elseN)) {
                return interner.pointer(thenT);
            }
            if (elseT.valid()
                && interner.kind(elseT) == TypeKind::FnSig
                && isLiteralIntegerZero(s, tree, thenN)) {
                return interner.pointer(elseT);
            }
            // c66 (D-CSUBSET-TERNARY-NULL-STRING-LITERAL): the ARRAY/string-literal
            // sibling of the Ptr/FnSig arms above. A string-literal arm
            // (`cond ? "%s" : 0` — sqlite's `sParse.zErrMsg ? "%s" : 0`) is an
            // un-decayed Array; paired with a literal-0 (a null pointer constant),
            // the conditional decays the array to Ptr<elem> (C 6.3.2.1p3 +
            // 6.5.15p6) → the POINTER type. The c64 array arm below needs BOTH arms
            // to be arrays, so it misses the `array : 0` pair → the fallback would
            // type it Array<char,N> and the aggregate lowering then materializes
            // the literal-0 arm as a string → H0009 (D-LK4-RODATA-PRODUCER-
            // NONSTRING-ARRAY-LITERAL-DECAY). Handles BOTH arm orders.
            if (thenT.valid()
                && interner.kind(thenT) == TypeKind::Array
                && isLiteralIntegerZero(s, tree, elseN)) {
                auto const e = interner.operands(thenT);
                if (!e.empty()) return interner.pointer(e[0]);
            }
            if (elseT.valid()
                && interner.kind(elseT) == TypeKind::Array
                && isLiteralIntegerZero(s, tree, thenN)) {
                auto const e = interner.operands(elseT);
                if (!e.empty()) return interner.pointer(e[0]);
            }
        }
        // c64 (D-CSUBSET-TERNARY-ARRAY-DECAY): C 6.3.2.1p3 + 6.5.15 — an ARRAY
        // arm of a conditional decays to a pointer-to-element, so `cond ? "a" :
        // "bb"` (two string literals — sqlite's `isFreeList ? "size" :
        // "overflow list length"`) and `cond ? arr : ptr` yield the common
        // POINTER type, never an aggregate. Without this the fallback below
        // returns the first arm's `Array<char,N>`, so the conditional types as an
        // aggregate and the HIR lowering trips the aggregate-valued-control-expr
        // guard (D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR — a phi has no
        // aggregate-width SSA value). Genuine struct/union arms do NOT decay (no
        // array-to-pointer rule) → they stay aggregates and reach the by-address
        // ternary lowering. Mirrored at the HIR combineTernary, which emits the
        // per-arm decay Cast. Conservative: requires BOTH arms to decay to the
        // SAME pointer type (the matching-element case); incompatible-element
        // conditionals stay on the existing fallback.
        {
            auto const decayArray = [&](TypeId t) -> TypeId {
                if (!t.valid() || interner.kind(t) != TypeKind::Array) return t;
                auto const elems = interner.operands(t);
                return elems.empty() ? t : interner.pointer(elems[0]);
            };
            TypeId const thenD = decayArray(thenT);
            TypeId const elseD = decayArray(elseT);
            if (thenD.valid() && thenD == elseD
                && interner.kind(thenD) == TypeKind::Ptr
                && (interner.kind(thenT) == TypeKind::Array
                    || interner.kind(elseT) == TypeKind::Array)) {
                return thenD;
            }
        }
        // C23 §6.5.15 (D-CSUBSET-NULLPTR): a conditional with ONE arm a pointer (or a
        // function designator, which decays to a pointer) and the OTHER the predefined
        // `nullptr` (TypeKind::NullptrT) has the POINTER type — nullptr IS the null
        // pointer constant, so it takes the other arm's pointer type. The
        // isLiteralIntegerZero-keyed arms above do NOT fire for nullptr (it is a
        // keyword, not the literal `0`), so this dedicated arm is required — WITHOUT
        // it `cond ? nullptr : p` (nullptr first) would fall through to the first-arm
        // fallback and mistype as NullptrT. The HIR tier lowers nullptr to the
        // synthetic integer-0 literal, whose combineTernary null arm realizes the
        // Cast(0→Ptr) on that arm, so the two tiers agree. Gated; handles both orders.
        if (sem.pointerConversions.nullPointerConstantFromNullptrT) {
            auto const ptrFromNullptrPair = [&](TypeId a, TypeId b) -> TypeId {
                // `a` is the candidate pointer/designator arm; `b` must be NullptrT.
                if (!a.valid() || !b.valid()
                    || interner.kind(b) != TypeKind::NullptrT) return {};
                if (interner.kind(a) == TypeKind::Ptr)   return a;
                if (interner.kind(a) == TypeKind::FnSig) return interner.pointer(a);
                return {};
            };
            if (TypeId r = ptrFromNullptrPair(thenT, elseT); r.valid()) return r;
            if (TypeId r = ptrFromNullptrPair(elseT, thenT); r.valid()) return r;
        }
        return thenT.valid() ? thenT : elseT;   // non-arith arms (pointers etc.)
    };

    // ── the explicit work-stack ──
    struct Frame {
        NodeId node;
        enum class Kind : std::uint8_t {
            Binary, Unary, Postfix, Ternary, Wrapper, Generic
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
            // `size_t` — the SAME declared vocabulary entry Pass 2's sizeof arms
            // stamp (D-LANG-TYPE-IDENTITY-VOCABULARY); a divergence here would
            // give a Pass-1.5 `sizeof` a different TypeId than the same
            // expression re-typed in Pass 2.
            result = synthesizedType(interner, sem.sizeofResultType, s.dataModel,
                                     TypeKind::U64);
            return;
        }
        // C11/C23 6.5.3.4: `_Alignof(T)` is size_t — an ADDITIVE mirror of the
        // sizeof arm (a re-typing wrapper whose castTypeRef child must NOT be
        // read as its pre-alignof type).
        if (hirCfg.alignofRule.valid() && r.v == hirCfg.alignofRule.v) {
            result = synthesizedType(interner, sem.alignofResultType, s.dataModel,
                                     TypeKind::U64);
            return;
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
            if (cr.typeChild >= kids.size()) { result = InvalidType; return; }
            // This Pass-1.5 resolve must be NET-SILENT: emitOnMiss=false silences
            // S_UnknownType, but the `_BitInt`/typeof-bitfield constraint
            // diagnostics fire UNCONDITIONALLY and BYPASS the reporter dedup window
            // — so a bare resolve here double-emits against the Pass-2 cast arm's
            // loud re-resolve. Roll the reporter back (the same chokepoint the
            // _Generic association resolve uses): the resolved TypeId + benign
            // interner/nodeToType side effects persist; the Pass-2 cast arm is the
            // SOLE emitter (it re-resolves loud, and a FAILED resolve returns
            // InvalidType WITHOUT stamping the cast node, so the re-resolve runs).
            auto& mut = const_cast<EngineState&>(s);
            auto const snap = mut.reporter.snapshotForRollback();
            result = resolveTypeNode(mut, sem, tree, kids[cr.typeChild], scope,
                                     /*emitOnMiss=*/false);
            mut.reporter.truncateTo(snap);
            return;
        }
        if (auto const it = s.idx().compoundLiteralByRule.find(r.v);
            it != s.idx().compoundLiteralByRule.end()) {
            auto const& cl = sem.compoundLiteralRules[it->second];
            auto const kids = visibleChildren(tree, node);
            if (cl.typeChild >= kids.size()) { result = InvalidType; return; }
            // NET-SILENT, same rationale + rollback as the cast arm above (the
            // Pass-2 compound-literal arm re-resolves loud as the sole emitter).
            auto& mut = const_cast<EngineState&>(s);
            auto const snap = mut.reporter.snapshotForRollback();
            result = resolveTypeNode(mut, sem, tree, kids[cl.typeChild], scope,
                                     /*emitOnMiss=*/false);
            mut.reporter.truncateTo(snap);
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
        // C11/C23 6.5.1.1p3 (D-CSUBSET-GENERIC-RESULT-TYPE-DEDUCTION): a generic
        // selection's type IS the SELECTED association's result-expression type,
        // NOT the controlling expression's. Pass 2 stamps this node with the
        // winner's type, but Pass 1.5 types it BEFORE that stamp exists — so
        // without this arm the node would fall through to the transparent-wrapper
        // fallback below and take its FIRST visible child = the CONTROLLING
        // expression (the silent wrong-width bug). Type it on THIS work-stack via
        // a multi-phase Generic frame (mirroring Ternary): phase 0 types the
        // controlling child, phase 1 picks the winner from that type + types the
        // winner, phase 2 yields the winner's type. Both a controlling-nested AND
        // a winner-nested `_Generic` stay on the stack — ZERO host recursion
        // (D-PARSE-DEEP-NEST-RECURSION-MEMORY). `n0` carries the controlling child.
        if (sem.genericRule.valid() && r.v == sem.genericRule.v) {
            std::vector<NodeId> gkids;
            for (NodeId c : visibleChildren(tree, node)) gkids.push_back(c);
            NodeId const ctrlChild = sem.genericControlChild < gkids.size()
                ? gkids[sem.genericControlChild] : InvalidNode;
            work.push_back(Frame{node, Frame::Kind::Generic, 0, ctrlChild, {},
                                 nullptr, {}, 0, {}});
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
                // CAPTURE the arm node-ids BEFORE pop_back: `f` is `Frame& =
                // work.back()`, so the pop dangles it (the Binary arm above copies
                // `e`/`lt` first for the same reason). Reading `f.list[*]` after the
                // pop is a use-after-pop — UB that MSVC tolerated (Windows green) but
                // gcc/clang clobbered → a `NodeId out of range` crash on WSL/qemu.
                NodeId thenN = f.list[1];
                NodeId elseN = f.list[2];
                work.pop_back();
                result = combineTernary(thenT, result, thenN, elseN);
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
        case Frame::Kind::Generic:
            if (f.phase == 0) {
                // Type the controlling expression on THIS work-stack (flat — a
                // controlling-nested `_Generic` re-enters here, never host-recurses).
                f.phase = 1; NodeId n = f.n0; enter(n);
            } else if (f.phase == 1) {
                // `result` now holds the controlling type. Pick the winner WITHOUT
                // recursion (the helper takes the controlling type as a param), then
                // type the winner on this work-stack too.
                NodeId const genericNode = f.node;
                GenericSelection const gs = selectGenericAssociation(
                    s, sem, tree, genericNode, scope, result);
                if (!gs.selected.valid()) { work.pop_back(); result = InvalidType; }
                else { f.phase = 2; NodeId n = gs.selected; enter(n); }
            } else {
                // `result` holds the winner's type — the selection's type.
                work.pop_back();
            }
            break;
        }
    }
    return result;
}

// The ONE generic-selection chokepoint (forward-declared above). Extracted from
// pass2Post's association walk so `subtreeType` (Pass 1.5) and `pass2Post`
// (Pass 2) agree on the winner. It does NOT call subtreeType (the caller supplies
// the controlling type — no host recursion for a controlling-nested `_Generic`)
// and is FULLY SILENT (its association resolves are rolled back — pass2Post's
// stamp-loop is the sole emitter).
GenericSelection
selectGenericAssociation(EngineState const& s, SemanticConfig const& cfg,
                         Tree const& tree, NodeId node, ScopeId scope,
                         TypeId controllingType) {
    GenericSelection sel;
    // The const_cast is subtreeType's exact discipline — the interner memoizes,
    // resolveTypeNode(emitOnMiss=false) may mint a forward tag, and the reporter
    // snapshot/rollback below runs on this handle; none mutates a const object
    // (every caller owns a non-const EngineState).
    EngineState& mutS = const_cast<EngineState&>(s);   // NOLINT
    TypeInterner& in = mutS.lattice.interner();
    auto kids = visibleChildren(tree, node);

    // (1) The controlling type is supplied by the caller (typed on its OWN
    // work-stack / at top level), lvalue-converted here (top-level volatile
    // stripped — the isAssignable precedent). The helper never re-types it, so a
    // controlling-nested `_Generic` never host-recurses.
    TypeId const ctrlConv = controllingType.valid()
        ? in.stripVolatile(controllingType) : InvalidType;
    sel.controllingResolved = ctrlConv.valid();

    // (2) Walk the associations. Each association child is a `genericAssoc`
    // UMBRELLA node (an `alt` rule) wrapping either a `genericTypedAssoc` or a
    // `genericDefaultAssoc`; descend through the wrapper to the inner
    // alternative (robust to a grammar that references the alternatives directly).
    NodeId matchedExpr = InvalidNode;
    auto innerAssoc = [&](NodeId assoc) -> NodeId {
        RuleId const r = tree.rule(assoc);
        bool const isInner =
            (cfg.genericTypedAssocRule.valid()
             && r.v == cfg.genericTypedAssocRule.v)
            || (cfg.genericDefaultAssocRule.valid()
                && r.v == cfg.genericDefaultAssocRule.v);
        if (isInner) return assoc;
        if (cfg.genericAssocRule.valid() && r.v == cfg.genericAssocRule.v) {
            for (NodeId c : visibleChildren(tree, assoc)) {
                if (tree.kind(c) == NodeKind::Internal) return c;
            }
        }
        return InvalidNode;
    };

    // The association type-name resolves are FULLY SILENT: resolveTypeNode's own
    // S_UnknownType respects emitOnMiss=false, but the `_BitInt`/typeof-bitfield
    // constraint diagnostics fire UNCONDITIONALLY and BYPASS the reporter dedup
    // window (diagnostic_reporter.cpp) — so a bare resolve here would multi-emit
    // against pass2Post's stamp-loop re-resolve (and Pass 1.5's auto call). Roll
    // the reporter back afterwards: the resolved TypeIds (for matching) and the
    // benign interner/nodeToType side effects persist; every diagnostic is
    // dropped. pass2Post re-resolves LOUD (its stamp-loop) as the SOLE emitter.
    auto const diagSnapshot = mutS.reporter.snapshotForRollback();

    for (NodeId assocChild : kids) {
        if (tree.kind(assocChild) != NodeKind::Internal) continue;
        NodeId const assoc = innerAssoc(assocChild);
        if (!assoc.valid()) continue;   // not an association child
        RuleId const assocRule = tree.rule(assoc);
        bool const isTyped =
            cfg.genericTypedAssocRule.valid()
            && assocRule.v == cfg.genericTypedAssocRule.v;
        bool const isDefault =
            cfg.genericDefaultAssocRule.valid()
            && assocRule.v == cfg.genericDefaultAssocRule.v;
        if (!isTyped && !isDefault) continue;

        // The result expression is the LAST internal child (the assignmentExpr
        // after the ':'); the typed form's FIRST internal child is the type-name.
        NodeId typeNode{}, exprNode{};
        for (NodeId c : visibleChildren(tree, assoc)) {
            if (tree.kind(c) != NodeKind::Internal) continue;
            if (isTyped && !typeNode.valid()) { typeNode = c; continue; }
            exprNode = c;   // keep last internal child as the result expr
        }

        if (isDefault) {
            // Two `default`s are a constraint violation; keep the FIRST here
            // (the exactly-one-default enforcement is pinned separately).
            if (!sel.defaultExpr.valid()) sel.defaultExpr = exprNode;
            continue;
        }

        // Typed association: resolve its type-name (rolled back below) to match
        // + record its type-node for pass2Post's loud re-resolve; a value in type
        // position leaves the type Invalid → anyBadType (no-match cascade guard).
        TypeId assocTy = InvalidType;
        if (typeNode.valid()) {
            assocTy = resolveTypeNode(mutS, cfg, tree, typeNode, scope,
                                      /*emitOnMiss=*/false);
            sel.typedAssocTypeNodes.push_back(typeNode);
            if (!assocTy.valid()) sel.anyBadType = true;
        }
        if (assocTy.valid() && ctrlConv.valid()
            && sameType(in.stripVolatile(assocTy), ctrlConv)) {
            ++sel.matchCount;
            if (!matchedExpr.valid()) matchedExpr = exprNode;
        }
    }

    mutS.reporter.truncateTo(diagSnapshot);   // drop every resolve diagnostic

    // (3) Resolve the selection: exactly-one typed match wins, else the
    // `default:`; >1 typed match is ambiguous (InvalidNode — pass2Post reports).
    sel.selected = sel.matchCount == 1 ? matchedExpr
                 : sel.matchCount == 0 ? sel.defaultExpr
                 : InvalidNode;
    return sel;
}

// FC16 D-CSUBSET-ANON-MEMBER-PROMOTION (C11/C23 §6.7.2.1 ¶13): search the
// ANONYMOUS members of the field scope `fieldScope` for a member spelled
// `name`, recursing through nested anonymous members. A direct member of
// `fieldScope` is NOT searched here (the caller already tried the direct
// lookup); only names reachable THROUGH an anonymous struct/union member are.
// Returns the SINGLE matching promoted field symbol (whose `anonAncestorPath`
// Pass 1.5 recorded). If the name matches ≥2 distinct fields across sibling
// anonymous members (`struct { union{int x;}; union{int x;}; }`), `ambiguous`
// is set and no symbol is returned (the caller fails loud, C forbids it). NO
// side effects — read-only over the frozen Pass-1.5 symbol/scope state.
[[nodiscard]] std::optional<SymbolId>
findPromotedField(EngineState const& s, ScopeId fieldScope,
                  std::string_view name, bool& ambiguous) {
    ambiguous = false;
    std::optional<SymbolId> found;
    // Recursive walk implemented iteratively over a worklist of scopes to
    // search; each anonymous member contributes its own composite scope. We
    // collect ALL matches (across sibling anon members) so an ambiguous name is
    // detected rather than silently resolving to the first sibling.
    std::vector<ScopeId> worklist{fieldScope};
    for (std::size_t wi = 0; wi < worklist.size(); ++wi) {
        ScopeId const cur = worklist[wi];
        for (auto const& [bindName, ns, fsym] : s.scopes.bindingsOf(cur)) {
            (void)bindName;
            if (ns != SymbolNamespace::Ordinary) continue;
            if (!fsym.valid()) continue;
            if (!s.symbols.at(fsym).isAnonymousMember) continue;
            TypeId const anonTy = s.symbols.at(fsym).type;
            auto const anonScopeIt = s.compositeScopeByType.find(anonTy.v);
            if (anonScopeIt == s.compositeScopeByType.end()) continue;
            ScopeId const anonScope = anonScopeIt->second;
            // Direct hit inside THIS anonymous member's scope?
            for (auto const& [mName, mNs, mSym] : s.scopes.bindingsOf(anonScope)) {
                if (mNs != SymbolNamespace::Ordinary) continue;
                if (!mSym.valid()) continue;
                if (s.symbols.at(mSym).isAnonymousMember) continue;   // a nested anon name — not a member spelling
                if (mName != name) continue;
                if (found.has_value() && found->v != mSym.v) {
                    ambiguous = true;
                    return std::nullopt;
                }
                found = mSym;
            }
            // Recurse into THIS anonymous member (nested anon).
            worklist.push_back(anonScope);
        }
    }
    return found;
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

    // c27 (D-CSUBSET-VOLATILE-POINTEE): strip a top-level VolatileQual so a
    // `volatile struct S a; a.x` (effectiveType = VolatileQual(Struct)) resolves
    // its field against the MATERIAL struct's scope. `compositeScopeByType` is
    // keyed by the material composite TypeId, never the qualified skin.
    TypeId effectiveType = s.lattice.interner().stripVolatile(lhsType);
    if (ma->dereferences) {
        // Arrow form `p->x` = `(*p).x`: the LHS must be `Ptr<Struct>` (one level).
        // c82 (D-CSUBSET-ARRAY-ARROW-DECAY, C 6.3.2.1p3 + 6.5.2.3): an ARRAY
        // LHS decays to a pointer to its first element before `->` applies —
        // sqlite shell.c's `data.aAuxDb->zDbFilename` (aAuxDb is an in-struct
        // ARRAY member; the arrow reads element [0]'s field). Both kinds
        // unwrap to the ELEMENT/POINTEE type identically; the HIR lowering's
        // arrow path decays the array LHS the same way (one rule, two tiers).
        TypeKind const lhsKind = s.lattice.interner().kind(effectiveType);
        if (lhsKind == TypeKind::Ptr || lhsKind == TypeKind::Array) {
            auto const ops = s.lattice.interner().operands(effectiveType);
            if (ops.empty()) { out.status = MemberResolution::Status::NotAPointer; return out; }
            // Strip a VolatileQual POINTEE too (`volatile struct S *p; p->x` →
            // pointee VolatileQual(Struct)) so the field scope resolves.
            effectiveType = s.lattice.interner().stripVolatile(ops[0]);
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
        // FC16 D-CSUBSET-ANON-MEMBER-PROMOTION (C11/C23 §6.7.2.1 ¶13): a direct
        // member miss — recursively search the composite's ANONYMOUS members.
        // `struct S { union { int a; }; } s; s.a` resolves `a` through the anon
        // union member (its `anonAncestorPath` was recorded at Pass 1.5). An
        // AMBIGUOUS name (matching two sibling anon members) fails loud.
        bool ambiguous = false;
        auto const promoted =
            findPromotedField(s, fieldScope, fnRes.name, ambiguous);
        if (ambiguous) {
            out.status = MemberResolution::Status::AmbiguousField; return out;
        }
        if (!promoted.has_value()) {
            out.status = MemberResolution::Status::UndeclaredField; return out;
        }
        out.fieldSym  = *promoted;
        out.fieldType = s.symbols.at(out.fieldSym).type;
        out.status    = MemberResolution::Status::Ok;
        return out;
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
                                                    /*charConvertsToArith=*/cfg.charConvertsToArith, /*enumConvertsToArith=*/cfg.enumConvertsToArith, /*intCrossSignednessConverts=*/cfg.intCrossSignednessConverts, /*intSameSignednessNarrows=*/cfg.intSameSignednessNarrows, /*intConvertsToFloat=*/cfg.intConvertsToFloat, /*floatConvertsToInt=*/cfg.floatConvertsToInt, /*charArrayFromStringLiteralInit=*/false, /*bitIntConversions=*/cfg.bitIntConversions)) {
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
                                 std::optional<std::string_view> activeTarget,
                                 LongDoubleFormat longDoubleFormat);

SemanticModel analyze(std::shared_ptr<CompilationUnit const> cu,
                      DataModel dataModel,
                      std::optional<AggregateLayoutParams> aggregateLayout,
                      std::optional<VaListStrategy> vaListStrategy,
                      std::optional<ObjectFormatKind> activeFormat,
                      std::optional<std::string_view> activeTarget,
                      LongDoubleFormat longDoubleFormat,
                      std::size_t deepRecursionReserveBytes) {
    // Run the recursive analysis on a dedicated large-stack worker thread
    // (JOIN-synchronous — no concurrency) so a deeply-nested-but-legal
    // expression tree does not overflow the host's ~1 MB main thread stack.
    // This is what lets `ParserConfig::maxExpressionDepth` be a real
    // semantic cap (config-driven: c-subset = 1024) rather than a host-stack
    // artifact (`D-PARSE-DEEP-FRONTEND-STACK`). The null-CU + every other contract
    // check lives in `analyzeImpl`, which runs on the worker; any exception
    // it throws is re-thrown here by `callOnLargeStack`.
    //
    // The reserve is the standard 64 MiB (`kDeepRecursionStackBytes`) unless the
    // caller passes a smaller, BOUNDED reserve (the `0` sentinel ⇒ default) —
    // the deep-nest regression pins do so to witness that the Pass-1.5/Pass-2
    // walks are FLAT (explicit heap work-stacks, O(1) host stack per level): the
    // analysis completes on the bounded reserve where a would-be per-level
    // recursion would overflow it.
    std::size_t const reserveBytes =
        deepRecursionReserveBytes != 0 ? deepRecursionReserveBytes
                                       : dss::substrate::kDeepRecursionStackBytes;
    return dss::substrate::callOnLargeStack(reserveBytes, [&] {
            return analyzeImpl(std::move(cu), dataModel, std::move(aggregateLayout),
                               std::move(vaListStrategy), std::move(activeFormat),
                               std::move(activeTarget), longDoubleFormat);
        });
}

static SemanticModel analyzeImpl(std::shared_ptr<CompilationUnit const> cu,
                                 DataModel dataModel,
                                 std::optional<AggregateLayoutParams> aggregateLayout,
                                 std::optional<VaListStrategy> vaListStrategy,
                                 std::optional<ObjectFormatKind> activeFormat,
                                 std::optional<std::string_view> activeTarget,
                                 LongDoubleFormat longDoubleFormat) {
    if (!cu) {
        std::fputs("dss::analyze fatal: null CompilationUnit\n", stderr);
        std::abort();
    }
    EngineState s{*cu};
    s.dataModel = dataModel;
    s.longDoubleFormat = longDoubleFormat;
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
        // c34 (D-CSUBSET-ARRAY-SIZE-INFERENCE): the brace-init-list + init-element
        // rules, by name from THIS schema — the same source as stringLiteralExprRule
        // (the HIR-lowering config names them too, but the semantic tier resolves its
        // own copy rather than reach across into the HIR config).
        idx.braceInitListRule = sch.rules().find("braceInitList");
        idx.initElementRule   = sch.rules().find("initElement");
        buildIndexes(s, idx, sch.semantics());
        // C11/C23 6.4.5: the per-opener element-core map (`L"`/`u"`/`U"`/`u8"`).
        // The core is resolved by a PURE CONFIG-MAP LOOKUP — `resolveElementCore`
        // returns the per-format override (`elementCoreByFormat`, how `L"…"`/wchar_t
        // declares its pe→U16 / elf/macho→I32 width AS CONFIG DATA, mirroring
        // builtinTypes' `coreByDataModel`) or the base `elementCore`. NO engine-tier
        // `format == …` branch — this tier owns `activeFormat` only to KEY the map.
        // The HIR tier reads the resulting element core back off the stamped node
        // type. (D-FFI-STDDEF-WCHAR-PE-WIDTH — width is config-declared, not coded.)
        SchemaTokenId const narrowOpener = sch.hirLowering().stringStartToken;
        for (auto const& px : sch.hirLowering().stringLiteralPrefixes) {
            if (!px.startToken.valid()) continue;
            TypeKind core;
            if (narrowOpener.valid() && px.startToken.v == narrowOpener.v
                && px.elementCoreByFormat.empty()
                && idx.stringLiteralElementCore != TypeKind::Void) {
                // The NARROW opener's core is the language's declared string-literal
                // element core (`literalTypes` `core`), NOT the loader's auto-seed
                // placeholder (Char) — so a grammar whose narrow string core is not
                // Char stays consistent with the scalar `stringLiteralElementCore`
                // fallback. (Every shipped grammar's narrow core IS Char, so this is
                // a no-op today; it forecloses a future non-Char-narrow divergence.
                // Skipped when the narrow opener declares a per-format map, so an
                // explicit config override always wins.)
                core = idx.stringLiteralElementCore;
            } else {
                core = px.resolveElementCore(s.activeFormat);   // config-map lookup
            }
            idx.stringLiteralElementCoreByStart[px.startToken.v] = core;
            // C11/C23 6.4.5p5 (Cycle D): classify this opener as NON-narrow keyed on
            // its TOKEN KIND (format-agnostic) — non-narrow when the base `elementCore`
            // OR any `elementCoreByFormat` value is not Char/Byte. Mirrors the HIR
            // tier's `isWideStringOpenerKind` so the adjacent-concat prefix fold agrees
            // across tiers WITHOUT reading the format-resolved core above (which would
            // make `u"a" L"b"` accept on pe but reject on elf).
            auto const isNonNarrowCore = [](TypeKind c) {
                return c != TypeKind::Char && c != TypeKind::Byte;
            };
            bool nonNarrow = isNonNarrowCore(px.elementCore);
            for (auto const& [fmt, fmtCore] : px.elementCoreByFormat) {
                (void)fmt;
                if (isNonNarrowCore(fmtCore)) nonNarrow = true;
            }
            if (nonNarrow) idx.nonNarrowStringOpeners.insert(px.startToken.v);
        }
        // C11/C23 6.4.4.4: the WIDE char-opener → format-resolved element-core map
        // (`L'`/`u'`/`U'`/`u8'`). The narrow `CharStart` is EXCLUDED — the unprefixed
        // `'x'` stays `int` via the flat `literalTypeIds` path (other integer-literal
        // consumers key on that entry), so only a wide opener overrides the char
        // type. Same PURE CONFIG-MAP LOOKUP (`resolveElementCore`) as the string map:
        // WideCharStart (wchar_t) resolves pe→U16 / elf,macho→I32 AS CONFIG DATA — NO
        // engine-tier `format == …` branch. The HIR tier reads the core back off the
        // stamped body token (it lacks format), so this is the format-aware point.
        SchemaTokenId const narrowChar = sch.hirLowering().charStartToken;
        idx.charLiteralBodyToken = sch.hirLowering().charBodyToken;
        for (auto const& px : sch.hirLowering().charLiteralPrefixes) {
            if (!px.startToken.valid()) continue;
            if (narrowChar.valid() && px.startToken.v == narrowChar.v)
                continue;   // narrow `'x'` → flat literalTypeIds int path (untouched)
            idx.charLiteralElementCoreByStart[px.startToken.v] =
                px.resolveElementCore(s.activeFormat);
        }
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
    // c82 (D-FFI-DESCRIPTOR-VA-LIST-TYPE): capture the per-CC `va_list` TypeId
    // the builtin injection below mints, so the FF11 shipped-descriptor read
    // can resolve a descriptor-spelled `va_list` (stdio.json's vfprintf) to
    // the SAME TypeId a user-written prototype gets. Multi-schema CUs mint
    // through ONE interner with ONE vaListStrategy, so re-mints dedup to the
    // same id — last-write is deterministic. nullopt when no schema declares
    // a va_arg surface: the descriptor alias then stays unresolved and the
    // read fails LOUD (F_ShippedLibUnsupportedType), never a guessed type.
    std::optional<TypeId> vaListBuiltinType;
    for (GrammarSchema const* sch : distinctSchemas) {
        ScopeId const builtinScope = s.scopes.pushScope(cuRoot, InvalidNode, InvalidTree);
        builtinScopeBySchema[sch->schemaId().v] = builtinScope;
        for (auto const& bf : sch->semantics().builtinFunctions) {
            // c104 (D-CSUBSET-INTRINSIC-ATOMIC-CAS): a builtin declaring a FULL
            // type-text `signature` (pointer-bearing params the scalar core axis
            // cannot spell) decodes HERE — the injection site owns the CU's
            // interner, which the interner-free schema decode does not. Same ONE
            // codec as shipped-lib symbol signatures. Fail loud on a non-FnSig
            // or malformed text (parseTypeFromText already reported the detail).
            TypeId fnTy = InvalidType;
            if (!bf.signatureText.empty()) {
                // D-LANG-TYPE-IDENTITY-VOCABULARY: decode EVERY declared
                // per-data-model override, not just the active one — a malformed
                // override under an inactive model would otherwise lurk until
                // that model is first compiled (the shipped-lib
                // `signatureByDataModel` anti-lurking rule). The ACTIVE model's
                // text, when declared, is the one that binds.
                bool decodeFailed = false;
                for (auto const& [dm, text] : bf.signatureTextByDataModel) {
                    TypeId const t = parseTypeFromText(text, s.lattice.interner(),
                                                       s.lattice.registry(),
                                                       s.reporter);
                    bool const bad =
                        !t.valid()
                        || s.lattice.interner().kind(t) != TypeKind::FnSig;
                    if (bad) {
                        ParseDiagnostic d;
                        d.code     = DiagnosticCode::C_InvalidSemantics;
                        d.severity = DiagnosticSeverity::Error;
                        d.actual   = std::format(
                            "builtin function '{}': 'signatureByDataModel.{}' must "
                            "decode to a function type, got '{}'",
                            bf.name, dataModelName(dm), text);
                        s.reporter.report(std::move(d));
                        decodeFailed = true;
                        continue;
                    }
                    if (dm == s.dataModel) fnTy = t;
                }
                if (decodeFailed) continue;
                if (!fnTy.valid()) {
                    fnTy = parseTypeFromText(bf.signatureText, s.lattice.interner(),
                                             s.lattice.registry(), s.reporter);
                }
                if (!fnTy.valid()
                 || s.lattice.interner().kind(fnTy) != TypeKind::FnSig) {
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::C_InvalidSemantics;
                    d.severity = DiagnosticSeverity::Error;
                    d.actual   = std::format(
                        "builtin function '{}': 'signature' must decode to a "
                        "function type, got '{}'", bf.name, bf.signatureText);
                    s.reporter.report(std::move(d));
                    continue;
                }
            } else {
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
                fnTy = s.lattice.interner().fnSig(
                    paramTypes, s.lattice.interner().primitive(bf.resultCore),
                    CallConv::CcSysV);
            }
            SymbolRecord rec;
            rec.name            = bf.name;
            rec.scope           = builtinScope;
            rec.tree            = InvalidTree;
            rec.kind            = DeclarationKind::Function;
            rec.type            = fnTy;
            rec.variadicBuiltin = bf.variadic;
            rec.builtinLowering = bf.lowering;  // c103: intrinsic-lowering builtins
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
                vaListBuiltinType = vaStructTy;   // c82: descriptor alias
                s.vaListType      = vaStructTy;   // c82: adjustment exclusion
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
                vaListBuiltinType = charPtr;      // c82: descriptor alias
                s.vaListType      = charPtr;      // c82: adjustment exclusion
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
                vaListBuiltinType = vaListTy;     // c82: descriptor alias
                s.vaListType      = vaListTy;     // c82: adjustment exclusion
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
    // c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): name → per-format library
    // map for descriptor symbols the goal-2 skip SUPPRESSED (a user decl
    // claimed the name). The CST→HIR bare-proto extern synthesis reads it so a
    // user's bare re-declaration of a shipped symbol (`popen` over `#include
    // <stdio.h>`) re-binds the descriptor's import library rather than
    // surviving to the linker as an undefined symbol.
    std::unordered_map<std::string, SuppressedShippedSymbol>
        suppressedShippedLibraries;
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

        // ── D-LANG-TYPE-IDENTITY-VOCABULARY: the CROSS-DESCRIPTOR invariant ──
        //
        // Both dedup sets above are FIRST-WINS BY NAME, and only the winner gets
        // a `compositeScopeByType` field scope. So two descriptors declaring one
        // tag DIFFERENTLY (the `struct timeval` that `sys/time.json` spells
        // `{i64 "long", i64 "long"}` and `sys/resource.json` once spelled
        // `{i64, i64}`) intern TWO TypeIds, and whichever loses the race has
        // UNREACHABLE members — an include-order-dependent `S000D`. The per-file
        // reader structurally cannot catch it; this accumulator can, and does,
        // BEFORE the first-wins skip silently swallows the divergence.
        //
        // The checker also verifies that every vocabulary tag a descriptor
        // spells has the width THIS LANGUAGE gives that name under the active
        // data model — a `i64 "long"` on LLP64 is a phantom pair no source
        // spelling can produce. The vocabulary is handed over as opaque
        // (name → core) rows resolved HERE from the active schemas' own
        // `typeSpecifiers`; the checker never sees a spelling it can branch on.
        //
        // A name two schemas of a MIXED-language CU resolve DIFFERENTLY is
        // dropped rather than guessed — the descriptor is neutral, so there is
        // no single language to hold it to.
        std::vector<std::pair<std::string, TypeKind>> vocabRows;
        {
            std::unordered_map<std::string, TypeKind> byName;
            std::unordered_set<std::string>           ambiguous;
            for (GrammarSchema const* sch : distinctSchemas) {
                for (auto const& ts : sch->semantics().typeSpecifiers) {
                    if (ts.name.empty()) continue;
                    auto const core = ts.resolveCore(s.dataModel, s.longDoubleFormat);
                    if (!core.has_value()) continue;   // unrealized on this axis
                    auto const [it, fresh] = byName.try_emplace(ts.name, *core);
                    if (!fresh && it->second != *core) ambiguous.insert(ts.name);
                }
            }
            vocabRows.reserve(byName.size());
            for (auto& [name, core] : byName) {
                if (ambiguous.contains(name)) continue;
                vocabRows.emplace_back(name, core);
            }
        }
        std::vector<ffi::VocabularyCore> vocabulary;
        vocabulary.reserve(vocabRows.size());
        for (auto const& [name, core] : vocabRows) {
            vocabulary.push_back(ffi::VocabularyCore{name, core});
        }
        ffi::ShippedTypeConsistency typeConsistency{s.lattice.interner(),
                                                    vocabulary, s.activeFormat};
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
            // c82 (D-FFI-DESCRIPTOR-VA-LIST-TYPE): thread the builtin `va_list`
            // TypeId (minted per the active CC's vaListStrategy above) as a
            // named-type binding, so a descriptor signature can spell C's
            // `va_list` and land the SAME TypeId a user prototype gets. No
            // schema minted one ⇒ empty span ⇒ a descriptor spelling the
            // alias fails LOUD at decode (never a guessed layout).
            std::array<NamedTypeBinding, 1> namedTypeStorage{};
            std::span<NamedTypeBinding const> namedTypes{};
            if (vaListBuiltinType.has_value()) {
                namedTypeStorage[0] =
                    NamedTypeBinding{"va_list", *vaListBuiltinType};
                namedTypes = std::span<NamedTypeBinding const>{
                    namedTypeStorage.data(), 1};
            }
            auto desc = ffi::readShippedLibDescriptor(
                descPath, s.lattice.interner(), s.lattice.registry(), s.reporter,
                s.dataModel, activeTargetView, s.activeFormat, namedTypes);
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

            // D-LANG-TYPE-IDENTITY-VOCABULARY: cross-descriptor consistency, run
            // AFTER the availability gate (a header that does not exist on this
            // target contributes no declarations) and BEFORE injection (so the
            // ROOT CAUSE is reported instead of the downstream member-access
            // failure). Reports and CONTINUES: the error delta already aborts
            // the pipeline, and injecting anyway keeps the historic first-wins
            // behavior for every other name in the descriptor rather than
            // cascading a wall of "undefined symbol".
            (void)typeConsistency.add(desc->header, *desc, s.reporter);

            for (auto const& sym : desc->symbols) {
                // GOAL-2: a user decl of this name wins — skip the descriptor's.
                if (userDeclaredNames.contains(sym.name)) {
                    // c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): record the
                    // SUPPRESSED symbol's library identity so a user BARE
                    // PROTOTYPE that re-declares this shipped name can bind its
                    // synthesized extern to the descriptor's library (the proto
                    // carries the import the descriptor would have injected).
                    // Availability-gated exactly like injection (a symbol that
                    // does not EXIST on this target must not plant an import)
                    // and first-wins across descriptors (emplace — the same
                    // order injection would have used). Recording objects too
                    // is harmless: the proto-synthesis consumer is fn-only.
                    // c156 (D-LK-ELF-SYMBOL-VERSIONING): the REQUIRED symbol
                    // version rides ALONGSIDE the library — a suppressed
                    // versioned symbol (realpath over `#include <stdlib.h>`)
                    // whose user prototype loses the version would silently
                    // misbind the oldest compat instance.
                    if (!s.activeFormat.has_value()
                        || ffi::objectFormatInAvailabilitySet(
                               sym.availableObjectFormats, *s.activeFormat)) {
                        suppressedShippedLibraries.emplace(sym.name,
                            SuppressedShippedSymbol{desc->library, sym.version});
                    }
                    continue;
                }
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
                // FC16 (D-CSUBSET-NORETURN): a descriptor-declared noreturn extern
                // (stdlib.json's `abort`/`exit`) — externs have no user prototype
                // to carry `_Noreturn`, so the flag rides the descriptor. A direct
                // call to one is wrapped at HIR lowering exactly like a user one.
                rec.isNoreturn = sym.noreturn;
                // FC17.9(c) (D-CSUBSET-SETJMP): a descriptor-declared returns-twice
                // extern (setjmp.json's `setjmp`/`_setjmp`) — externs have no user
                // prototype to carry the attribute, so it rides the descriptor. Read
                // at HIR->MIR to stamp the Call's MirInstFlags::ReturnsTwice (the
                // isNoreturn-from-descriptor mirror, one line above).
                rec.returnsTwice = sym.returnsTwice;
                SymbolId const id = s.symbols.mint(rec);
                s.scopes.injectBinding(cuRoot, sym.name, id);

                shippedExterns.push_back(ShippedExternSymbol{
                    id, sym.name, sym.signature, desc->library,
                    sym.kind == ffi::ShippedSymbolKind::Function,
                    sym.synthesize,   // D-CSUBSET-C11-THREADS-HEADER (pe shim tag)
                    sym.version});    // D-LK-ELF-SYMBOL-VERSIONING (c156)
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

            // c52 (D-FFI-MATH-INFINITY): inject the descriptor's FLOAT CONSTANTS
            // (`INFINITY` etc.) — the float sibling of the integer-`constants` loop
            // above, sharing the SAME `isInjectedConstant` fold path. The only
            // difference is the carried value: `enumValue` is an int64, so the
            // double is stored as its IEEE-754 BIT-PATTERN (std::bit_cast) and the
            // shared `constantLiteralForSymbol` builder bit_casts it back when the
            // symbol's type is a float kind (the integer path reads `enumValue`
            // directly). `type` is the constant's own float scalar (F32/F64).
            for (auto const& fc : desc->floatConstants) {
                if (userDeclaredNames.contains(fc.name)) continue;
                if (!injectedNames.insert(fc.name).second) continue;  // first wins
                SymbolRecord rec;
                rec.name               = fc.name;
                rec.scope              = cuRoot;
                rec.tree               = InvalidTree;   // not a user decl
                rec.kind               = DeclarationKind::Variable;
                rec.type               = fc.type;
                rec.enumValue          = std::bit_cast<std::int64_t>(fc.value);  // f64 bit-pattern
                rec.isInjectedConstant = true;
                rec.isConst            = true;          // a macro constant is not assignable
                SymbolId const id = s.symbols.mint(rec);
                s.scopes.injectBinding(cuRoot, fc.name, id);
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
            // FC16 (D-CSUBSET-NORETURN): OR-merge the noreturn flag across the
            // proto/def pair BEFORE the type-compat gate below (the flag is
            // independent of signature compatibility, so it must not be skipped by
            // the incompatible-redeclaration `continue`). Detection marks whichever
            // side spelled the attribute (typically the prototype); a call resolves
            // to the SURVIVOR (definition), so the load-bearing direction is INTO
            // the survivor. OR-ing BOTH is harmless and covers either merge order.
            {
                bool const nr = s.symbols.at(survivor).isNoreturn
                             || s.symbols.at(absorbed).isNoreturn;
                s.symbols.at(survivor).isNoreturn = nr;
                s.symbols.at(absorbed).isNoreturn = nr;
            }
            // FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS): OR-merge deprecated /
            // nodiscard across the proto/def pair, the isNoreturn precedent —
            // a use/call resolves to the SURVIVOR (definition), so a flag
            // spelled only on the prototype must merge INTO it (OR-ing both
            // covers either merge order). Messages first-non-empty-wins,
            // survivor-preferred (F7). `isMaybeUnused` is deliberately NOT
            // merged — it is consulted only at the declarator's own D8 check.
            {
                bool const dep = s.symbols.at(survivor).isDeprecated
                              || s.symbols.at(absorbed).isDeprecated;
                bool const nd  = s.symbols.at(survivor).isNodiscard
                              || s.symbols.at(absorbed).isNodiscard;
                std::string const depMsg =
                    !s.symbols.at(survivor).deprecatedMessage.empty()
                        ? s.symbols.at(survivor).deprecatedMessage
                        : s.symbols.at(absorbed).deprecatedMessage;
                std::string const ndMsg =
                    !s.symbols.at(survivor).nodiscardMessage.empty()
                        ? s.symbols.at(survivor).nodiscardMessage
                        : s.symbols.at(absorbed).nodiscardMessage;
                for (SymbolId const side : {survivor, absorbed}) {
                    auto& rec = s.symbols.at(side);
                    rec.isDeprecated      = dep;
                    rec.isNodiscard       = nd;
                    rec.deprecatedMessage = depMsg;
                    rec.nodiscardMessage  = ndMsg;
                }
            }
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
            // FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS, C23 6.7.13.4): a declaration
            // flagged `[[maybe_unused]]` / GNU `__attribute__((unused))` opts
            // its symbols OUT of the unused warning — the whole point of the
            // attribute. Same engine, same empty use-set, no warning.
            if (rec.isMaybeUnused) continue;
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
        std::move(s.nodeToSelectedExpr),
        std::move(s.reporter),
        std::move(s.usesBySymbol),
        std::move(s.compositeScopeByType),
        std::move(s.nullPointerConstantNodes),
        std::move(shippedExterns),
        std::move(suppressedShippedLibraries),
        dataModel,
        longDoubleFormat,
    };
}

} // namespace dss
