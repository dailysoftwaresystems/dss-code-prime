#pragma once

#include "core/export.hpp"
#include "core/types/data_model.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Per-language semantic config (schema v4 `semantics` block).
//
// This file ships the POD vocabulary a language declares to drive the
// language-agnostic semantic analyzer (see `src/analysis/semantic/`).
// The engine reads only this struct — it NEVER branches on
// `schema.name()`. Adding a new language = adding a `semantics` block
// to its `.lang.json`.
//
// Empty / unpopulated fields mean "no analysis for that facet" — a
// language with no scopes, no built-in types, no typed literals, etc.
// is valid and simply analyzes less. The loader rejects malformed
// entries (wrong field types, dangling rule/token names, unknown
// `kind`/`core`/`constructor` strings) via `C_InvalidSemantics`,
// `C_MissingField`, `C_UnknownShape`, `C_UnknownToken`.

namespace dss {

// A declaration introduced by a rule. `name`/`type`/`init` are
// VISIBLE-child indices (EmptySpace skipped), matching how the parser-
// era typed views indexed children.
//
// `kind` enumerates the categories of declaration the analyzer knows
// about. `Variable` is the default; languages may declare functions,
// tables (SQL), or named types.
//
// `nameMatch` controls how the name node is resolved out of the visible
// child at `name`:
//   Self          — the child node IS the name (e.g. an Identifier
//                    leaf or a single-child name wrapper). Default.
//   LastIdentifier — descend the child subtree and use the LAST
//                    identifierToken (per the config's resolved
//                    `identifierToken`; tsql's `qualifiedName` — the
//                    last name in `db.schema.table` is the table).
enum class DeclarationKind : std::uint8_t {
    Variable,
    Function,
    Table,
    Type,
};

enum class NameMatchMode : std::uint8_t {
    Self,
    LastIdentifier,
};

// A kind-discriminator facet: lets a single declaration shape decide its
// effective `kind` at analysis time by inspecting a child sub-rule. Used
// by grammars (like c-subset's `topLevelDecl`) that factor the common
// prefix of variables and functions into one declaration rule and only
// disambiguate via a trailing alt (`topLevelDeclTail = alt[funcDefTail,
// varDeclTail]`). The structural info "funcDefTail present ⇒ Function"
// lives in the tree; this struct lets the schema express it.
//
// Evaluation: starting at the matching declaration node, descend
// visible children following `childPath`. If that node is Internal and
// its rule == `whenRule`, the effective kind is `whenKind` and (when
// matched) `paramsPath` / `bodyPath` are walked as additional
// visible-child sequences FROM the matched discriminator node to
// resolve the params and body subtrees. Otherwise the DeclarationRule's
// static `kind` field is used.
//
// E.g. c-subset's `topLevelDecl → [typeRef, Identifier, topLevelDeclTail]`
// with `topLevelDeclTail → alt[funcDefTail, varDeclTail]`. The schema
// declares:
//   childPath: [2, 0]            → descend to topLevelDeclTail's child
//   whenRule:  "funcDefTail"     → match when that child is funcDefTail
//   paramsPath: [0]              → funcDefTail's first child (funcParams)
//   bodyPath:   [1]              → funcDefTail's second child (block)
struct DSS_EXPORT KindDiscriminator {
    // Path of visible-child indices from the declaration node to the
    // discriminator-deciding node. A single-int path (e.g. `[2]`)
    // covers the simplest case; deeper paths skip thin wrapper alts.
    std::vector<std::uint32_t> childPath;
    // If the discriminator node's rule == `whenRule`, kind = `whenKind`.
    RuleId          whenRule{};
    std::string     whenRuleName;     // retained for diagnostics
    DeclarationKind whenKind = DeclarationKind::Function;
    // Path of visible-child indices from the matched discriminator node
    // to the params node (when matched as `whenKind == Function`).
    // Empty ⇒ no params resolved.
    std::vector<std::uint32_t> paramsPath;
    // Path of visible-child indices from the matched discriminator node
    // to the body node.
    std::vector<std::uint32_t> bodyPath;
};

// SE-arrays (HR9): a C-style declarator suffix (e.g. `int a[10]`). When a
// declaration configures one and a node of `rule` appears in its subtree, the
// declared type is wrapped as Array<base, length>, where `length` is the
// constant integer in the suffix's visible child at `lengthChild`. A missing or
// non-constant length fails loud (S_NonConstantArrayLength); an out-of-range one
// is S_ArrayLengthOutOfRange — never a silent pointer decay. Nested in an
// `optional` (like `kindByChild`) so the off state can't carry stray fields.
struct DSS_EXPORT ArraySuffix {
    RuleId                       rule{};        // the suffix shape rule
    std::string                  ruleName;      // source spelling, for diagnostics
    std::optional<std::uint32_t> lengthChild;   // visible-child index of the length expr
};

// FC8 D-CSUBSET-BITFIELD: a C bit-field declarator suffix (`int x : 3`). When a
// declaration configures one and a node of `rule` appears in a field's subtree,
// the field is a BIT-FIELD whose declared width is the constant integer in the
// suffix's visible child at `widthChild`. The width is evaluated + range-checked
// against the field's integer type at semantic (a non-integer field, width > the
// type's bit size, or a negative width fails loud). Config-driven (the language
// names the suffix rule); the engine never hard-codes "bitfieldDeclSuffix".
struct DSS_EXPORT BitfieldSuffix {
    RuleId                       rule{};        // the suffix shape rule
    std::string                  ruleName;      // source spelling, for diagnostics
    std::optional<std::uint32_t> widthChild;    // visible-child index of the width expr
};

// C23 6.7.2.2 (D-CSUBSET-ENUM-UNDERLYING-TYPE, FC17): an enum's OPTIONAL explicit
// underlying-type clause (`enum E : unsigned char { … }`). When a declaration
// configures one and a node of `rule` appears in the composite's subtree, the
// enum's underlying scalar TypeKind is the integer type at that node's visible
// child `typeChild` (resolved via the shared type-position resolver) instead of
// the default int; each enumerator's value is then range-checked against it. A
// non-integer underlying fails loud (S_InvalidEnumUnderlyingType); an out-of-range
// enumerator fails loud (S_EnumeratorValueOutOfRange). Config-driven (the language
// names the clause rule + type-child index); the engine never hard-codes
// "enumTypeSpecifier". Sibling shape to BitfieldSuffix.
struct DSS_EXPORT EnumUnderlyingTypeSpec {
    RuleId                       rule{};        // the underlying-type clause rule
    std::string                  ruleName;      // source spelling, for diagnostics
    std::optional<std::uint32_t> typeChild;     // visible-child index of the type-name node
};

// D5.1 / D5.4: a composite-type-introducing declaration. When a declaration
// carries `fieldChildren`, Pass 1.5 walks the scope it opened, collects every
// minted symbol whose declaring rule == `rule` (in declaration order, via each
// field's `SymbolRecord::fieldIndex`), and composes either a `TypeKind::Struct`
// or `TypeKind::Union` lattice type (per `compositeKind`) over their resolved
// types. The composite symbol's `type` is set to the result. Generic facet —
// works for any language with field-bearing record / variant types.
//
// `compositeKind` controls struct-vs-union interning (default = `Struct`).
//
// The facet ONLY makes sense paired with a `scopes` entry for the same rule
// (so fields bind into the composite's inner scope, not the enclosing one).
// The loader rejects a `fieldChildren` whose declaration rule is not also in
// `scopes` via `C_InvalidSemantics`.
enum class CompositeKind : std::uint8_t { Struct, Union, Enum };
struct DSS_EXPORT FieldChildrenDescriptor {
    RuleId        rule{};                          // the field-declaration rule
    std::string   ruleName;                        // source spelling, for diagnostics
    CompositeKind compositeKind = CompositeKind::Struct;
    // D5.5-FU2: ONLY meaningful when `compositeKind == Enum` — for
    // Struct/Union this field is loader-validated but ignored by Pass
    // 1.5 (no enclosing-scope republication makes sense for fields /
    // variants). When true, the enumerator names are ALSO bound in the
    // enclosing scope (C-classic visibility — `enum E { A } ... A`).
    // C++ default is `false` (the safer Rust-style `E.A`-only) so a
    // new enum-bearing schema must explicitly opt in via the loader's
    // `liftToEnclosingScope: true` key; a schema that forgets the
    // flag fails LOUD at the use site with an undefined-name
    // diagnostic, rather than silently leaking names.
    bool          liftToEnclosingScope = false;
};

// Forward-declared as opaque enums (a fixed underlying type makes them COMPLETE
// types, hence valid by value in the `std::optional`s below) so this header need
// not pull the heavy `symbol_attrs.hpp` → `target_schema.hpp` include — which
// `grammar_schema.hpp` includes early, before `LoadResult` is defined, closing a
// cycle. The full enum definitions + name tables live in
// `core/types/symbol_attrs.hpp`, included by the .cpp consumers that read VALUES.
enum class SymbolBinding : std::uint8_t;
enum class SymbolVisibility : std::uint8_t;

// D-CSUBSET-LINKAGE-SPECIFIERS (pre-OPT7 P1, 2026-06-04): the effect a single
// declaration-specifier token has on the declared symbol's linkage. A language
// maps each specifier's SOURCE TEXT (e.g. "static", "weak", "hidden") to one of
// these via `DeclarationRule::linkageSpecifiers`; CST→HIR lowering walks the
// declaration's specifier-prefix subtree (see `specifierPrefixRule`), looks each
// specifier token's text up in that map, and folds the effects onto the HIR
// node's `LinkageAttr`. A field left `nullopt` leaves that axis at its prior
// value (so `static` sets only `binding`; `visibility("hidden")` sets only
// `visibility`). Agnostic: the engine performs the lookup; WHICH texts exist and
// what binding/visibility they mean are entirely per-language config.
//
// D-CSUBSET-LOCAL-STATIC (2026-06-22): the same per-(rule,token) effect map also
// carries a STORAGE-DURATION axis (`staticStorage`). A C `static` storage-class
// specifier confers different effects by scope: at FILE scope it sets `binding`
// (internal linkage); at BLOCK scope it confers STATIC storage duration (the
// object lives in `.data`/`.bss`, not the stack frame) AND, for the emitted
// hidden global, internal (`local`) binding. Both are folded from the ONE
// specifier-prefix scan (`linkageFrom`); the block-scope `static` row therefore
// declares `{ "binding": "local", "staticStorage": true }`. Naming note: the map
// is the declaration-SPECIFIER effect map (linkage is one axis of several), not
// linkage-only — kept under the existing `linkageSpecifiers` facet so a single
// scan folds every axis rather than duplicating the prefix walk.
struct DSS_EXPORT LinkageSpecifierEffect {
    std::optional<SymbolBinding>    binding;
    std::optional<SymbolVisibility> visibility;
    // Block-scope static storage duration (C 6.2.4/6.7.1): the object gets
    // static (module-global) storage, not an automatic stack slot. Folded by
    // CST→HIR to route the local declaration down the global-emission path.
    bool                            staticStorage = false;
    // TLS C1 (D-CSUBSET-THREAD-LOCAL): THREAD storage duration (C11/C23
    // 6.2.4 / 6.7.1 `_Thread_local` / `thread_local`) — the 4th ORTHOGONAL
    // axis. Thread storage does NOT change binding/visibility (a file-scope
    // `thread_local int g;` keeps EXTERNAL linkage — 6.2.2 is untouched by
    // 6.7.1p3) and does NOT itself confer block-scope static routing (the
    // standard REQUIRES a co-present `static`/`extern` at block scope —
    // Pass 2's validateThreadLocalDeclarator enforces it). Consumed by the
    // semantic Pass-1 specifier scan (→ SymbolRecord.isThreadLocal, the
    // per-symbol source of truth every later tier reads). OR-only across a
    // prefix — a `{threadStorage:true}` entry can never clobber a
    // co-present static's binding/staticStorage (the noreturn
    // linkage-clobber lesson: each axis folds independently).
    bool                            threadStorage = false;
};

// FC4 c1 (M5): a config-driven fail-loud gate on a declaration form. When the
// declaration's subtree contains a token of `token` kind, semantic analysis
// emits the named diagnostic (an ERROR, positioned at the first such token).
// This is the "volatile wall" vocabulary: a language whose grammar ADMITS a
// marker it does not yet implement (C `volatile`) declares the marker here so
// every use fails loud instead of silently compiling without the semantics.
// Both the token AND the diagnostic code are per-language config — the loader
// resolves `code` through the shared `diagnosticCodeName` table and rejects
// unknown names, so a typo can never silently disarm the wall.
struct DSS_EXPORT GatedMarker {
    SchemaTokenId  token{};
    std::string    tokenName;   // source spelling, for diagnostics
    DiagnosticCode code = DiagnosticCode::None;
    std::string    codeName;    // source spelling, for diagnostics
};

// ── FC4 c1: the `declarators` block (C 6.7.6 declarator grammar roles) ──
//
// Full C declarators invert: the DECLARATION's type-specifier head carries
// only the base type; pointer stars, function suffixes `(params)`, array
// suffixes `[n]`, and grouping parens live in a recursive DECLARATOR that
// wraps the declared NAME. A language whose grammar produces that shape
// declares here WHICH of its rules/tokens play each role; the engine's
// shared declarator walk (`core/types/declarator_walk.hpp` — name
// extraction, used by BOTH the parser's binder sketch and the semantic
// analyzer) and the semantic declarator-inversion fold (Pass 1.5) consume
// ONLY these resolved ids — never a hardcoded rule name.
//
// Grammar contract the roles describe (shapes, not names):
//   declaratorRule     :=  pointerLayerRule* directRule
//   pointerLayerRule   :=  pointerToken qualifier-tokens*
//   directRule         :=  (nameToken | groupRule) suffix*
//                          where suffix ∈ { fnSuffixRule, arraySuffixRule }
//   groupRule          :=  '(' declaratorRule ')'
//   fnSuffixRule       :=  '(' fnSuffixParamsRule? ')'
//   arraySuffixRule    :=  '[' length? ']'
//   initDeclaratorRule :=  declaratorRule ('=' init)?
//   listRule           :=  initDeclaratorRule (',' initDeclaratorRule)*
//
// `fnSuffixParamsRule` is the OPTIONAL param-list rule inside a fn suffix
// (for param-type harvesting); absent ⇒ fn suffixes always build zero-param
// signatures. Every other role is required — the loader rejects a partial
// block (a missing role would silently truncate the walk mid-declarator).
struct DSS_EXPORT DeclaratorConfig {
    RuleId        declaratorRule{};
    RuleId        pointerLayerRule{};
    SchemaTokenId pointerToken{};
    RuleId        directRule{};
    RuleId        groupRule{};
    SchemaTokenId nameToken{};
    RuleId        fnSuffixRule{};
    std::optional<RuleId> fnSuffixParamsRule;
    // VLA C4c (D-CSUBSET-VLA-PARAM-STAR): the OPTIONAL guard-less post-base twin of
    // `fnSuffixRule` — a `( paramList? )` suffix grammar-IDENTICAL to `fnSuffixRule`
    // but carrying NO `commitRequiresTypeName` guard. It exists because the
    // direct-declarator SUFFIX repeat is now SPECULATIVE (to disambiguate the
    // bare-`[*]` `arrayStarSuffix` from `arrayDeclSuffix`), and a guarded
    // `fnSuffixRule` at that suffix position would spuriously roll back a valid
    // function's `()` (empty / cross-file-typedef params) — breaking `int f()`,
    // casts, EVERY named function declarator (whose `(...)` rides the suffix
    // repeat). Every function-suffix recognition site treats this rule IDENTICALLY
    // to `fnSuffixRule` (via the shared `isFnSuffixRule` predicate below); its
    // param list is the SAME `fnSuffixParamsRule`, so param harvest is unchanged.
    // `nullopt` ⇒ the grammar has only the single fn-suffix rule (every grammar
    // before this landed) — behavior is exactly as before.
    std::optional<RuleId> fnSuffixTailRule;
    // c32 (D-CSUBSET-FNPTR-PARAM-SCOPE): the OPTIONAL param-list rule that opens a
    // per-declarator FUNCTION-PROTOTYPE scope (C 6.2.1p4). A param in a
    // NON-definition declarator — a function-POINTER member/typedef/param, or a
    // bare prototype — has a scope that terminates at the END of that declarator,
    // so its name must bind into a THROWAWAY scope rather than the enclosing
    // struct/file/block scope (else sibling fn-ptr declarators collide on a shared
    // param name and the names LEAK into the enclosing scope). The semantic
    // analyzer opens this scope for any node of this rule UNLESS it is a function
    // DEFINITION's OWN param list (the fnSuffix sits on a NAMED direct declarator
    // AND the enclosing definition has a body) — those keep binding into the
    // definition's scope so they reach the body. This is typically the SAME RuleId
    // as `fnSuffixParamsRule` (the two are distinct ROLES — param-type harvest vs
    // prototype-scope — not distinct rules; the engine discriminates by the
    // definition test, not rule identity). `nullopt` ⇒ no per-declarator prototype
    // scope (the prior behavior: every param binds into the enclosing scope).
    // Toy/tsql declare no `declarators` block at all, so they are unaffected.
    std::optional<RuleId> prototypeParamScopeRule;
    RuleId        arraySuffixRule{};
    // VLA C4c (D-CSUBSET-VLA, C99 §6.7.6.2p4): the OPTIONAL bare-`[*]`
    // unspecified-size array-suffix rule (`arrayStarSuffix`) — a prototype-form
    // VLA-parameter marker. A DISTINCT CST rule from `arraySuffixRule` so the
    // flat bound-locator / lengthChild / captureVlaSize sites are untouched.
    // `nullopt` ⇒ the language has no `[*]` suffix (toy/tsql, and c-subset
    // before this landed) — the declarator engine simply never matches it.
    std::optional<RuleId> arrayStarSuffixRule;
    RuleId        initDeclaratorRule{};
    RuleId        listRule{};
    // c23 (D-CSUBSET-STRUCT-MULTI-DECLARATOR): the OPTIONAL struct/union
    // member-declarator roles — the member-list analogue of
    // `initDeclaratorRule`/`listRule`. `memberDeclaratorRule` is the per-slot
    // wrapper `{declarator? bitfieldSuffix?}` (its inner declarator carries the
    // name + per-slot pointer/array suffixes; the bitfield suffix now lives
    // INSIDE the slot, so `int a:3, b:5;` resolves each width independently);
    // `memberListRule` is `memberDeclaratorRule (',' memberDeclaratorRule)*`.
    // The shared declarator walk (`declaratorNameNode` / `collectDeclarators`)
    // and the semantic declarator-inversion fold (`declaratorDeclaredType`)
    // descend a `memberDeclaratorRule` to its inner `declaratorRule`. BOTH
    // `nullopt` for languages without the feature (toy/tsql declare no
    // `declarators` block at all) ⇒ zero behavior change.
    std::optional<RuleId> memberDeclaratorRule;
    std::optional<RuleId> memberListRule;
    // c26 (D-CSUBSET-ABSTRACT-DECLARATOR-TYPE-NAME): the OPTIONAL abstract twin of
    // `directRule` — a `direct-abstract-declarator` (C 6.7.7) whose base EXCLUDES
    // the name token, used in TYPE-NAME position (cast/sizeof/compound/va_arg)
    // where a name is illegal AND a bare-identifier base would make a parenthesized
    // multiplication (`(c * c)`) mis-commit as a cast. Its children are the SAME
    // shared group/fnSuffix/arraySuffix rules, so the semantic `directDeclaredType`
    // folds it identically to `directRule`, and `declaratorNameNode` treats it like
    // `directRule` so a NAME nested in its parenDeclarator (`(int (x))`) is still
    // found and rejected loud. `nullopt` ⇒ no abstract type-name declarator (every
    // grammar before c26).
    std::optional<RuleId> directAbstractRule;
    // FC12a-core (D-FC12A-VARIADIC-CALLEE): the `...` marker token whose presence
    // in a fnSuffix's param list makes the FnSig C-style variadic. Declarator-level
    // (vs the per-`DeclarationRule` `variadicMarker`) so the SHARED declarator-suffix
    // resolver `applyDeclaratorSuffix` — the path a function DEFINITION + fn-pointer
    // typing both take — builds a variadic FnSig (the legacy decl path scanned its
    // own per-rule marker but the suffix path did not, so a `T f(...) {...}`
    // DEFINITION built a non-variadic FnSig — the gap FC12a-core closes). `nullopt`
    // ⇒ the language has no varargs (FnSigs through this path are non-variadic).
    std::optional<SchemaTokenId> variadicMarker;
    // VLA C4c (D-CSUBSET-VLA, C99 §6.7.6.2/6.7.6.3): the token kinds that DECORATE
    // an array-PARAMETER suffix — a `static`, the cv-qualifiers, and the
    // unspecified-size `*` — none part of the length BOUND. The shared
    // `arraySuffixBoundNode`/`arraySuffixHasModifier` helpers (decl_prefix_strip.hpp)
    // skip these to LOCATE the bound behind a decoration and to detect a
    // parameter-only decoration on a non-parameter declarator (the constraint
    // violation S_ArrayParamQualifierNonParameter). EMPTY for a language without
    // array-parameter decorations (the helpers degrade to the plain
    // first-non-bracket-child view — the prior behavior, unchanged).
    std::vector<SchemaTokenId> arraySuffixModifierTokens;
    // Source spellings, retained for diagnostics (mirrors the
    // rule+ruleName pairing convention of the other facets).
    std::string   declaratorRuleName;
    std::string   pointerLayerRuleName;
    std::string   pointerTokenName;
    std::string   directRuleName;
    std::string   groupRuleName;
    std::string   nameTokenName;
    std::string   fnSuffixRuleName;
    std::string   fnSuffixParamsRuleName;
    std::string   fnSuffixTailRuleName;          // VLA C4c D-CSUBSET-VLA-PARAM-STAR
    std::string   prototypeParamScopeRuleName;   // c32 D-CSUBSET-FNPTR-PARAM-SCOPE
    std::string   arraySuffixRuleName;
    std::string   initDeclaratorRuleName;
    std::string   listRuleName;
    std::string   memberDeclaratorRuleName;   // c23 D-CSUBSET-STRUCT-MULTI-DECLARATOR
    std::string   memberListRuleName;         // c23 D-CSUBSET-STRUCT-MULTI-DECLARATOR
    std::string   directAbstractRuleName;     // c26 D-CSUBSET-ABSTRACT-DECLARATOR-TYPE-NAME
    std::string   variadicMarkerName;
    std::vector<std::string> arraySuffixModifierTokenNames;   // VLA C4c D-CSUBSET-VLA
    std::string   arrayStarSuffixRuleName;                    // VLA C4c D-CSUBSET-VLA-PARAM-STAR
};

// VLA C4c (D-CSUBSET-VLA-PARAM-STAR): is rule `r` a FUNCTION suffix — either the
// guarded base-position `fnSuffixRule` or its guard-less post-base twin
// `fnSuffixTailRule`? The two rules are grammar-IDENTICAL (`( paramList? )`) and
// MUST be recognized identically at every function-suffix site (the fn-declarator
// detection, the definition-param scope test, the FnSig type fold in
// `directDeclaredType`/`applyDeclaratorSuffix`, and the HIR param harvest) — ONE
// predicate so those sites can never drift. `fnSuffixTailRule` nullopt ⇒ the
// grammar has only the single fn-suffix rule, the second disjunct is dead, and
// behavior is exactly as before this landed. Shared across the semantic analyzer
// and the CST→HIR lowerer (both consume `DeclaratorConfig`).
[[nodiscard]] inline bool
isFnSuffixRule(RuleId r, DeclaratorConfig const& dc) {
    return r == dc.fnSuffixRule
        || (dc.fnSuffixTailRule.has_value() && r == *dc.fnSuffixTailRule);
}

struct DSS_EXPORT DeclarationRule {
    // The rule (resolved to RuleId) whose subtree introduces the decl.
    RuleId          rule{};
    // Visible-child indices. `nullopt` means "absent" (the field is not
    // used by this declaration form — e.g. tsql `createTableStmt` has no
    // init). Matches the NumberStyle optional-index precedent.
    std::optional<std::uint32_t> nameChild;
    std::optional<std::uint32_t> typeChild;
    std::optional<std::uint32_t> initChild;
    // FC4 c1: DECLARATOR-mode child roles (C 6.7.6). A row sets EITHER the
    // legacy positional `nameChild`/`typeChild` pair above OR this trio —
    // the loader rejects mixing them (C_ConflictingField). In declarator
    // mode the row's type information splits: `headChild` is the
    // type-specifier HEAD (base type only — NO pointer stars; those live in
    // the declarator), and exactly one of `declaratorListChild` (an
    // initDeclarator LIST — `int *p, q;` mints one symbol PER declarator)
    // or `declaratorChild` (a SINGLE declarator — param-like rows) names
    // where the declarator(s) sit. All three require the language to
    // declare the `declarators` block (the role vocabulary the walk/fold
    // consume); the loader rejects declarator-mode rows without it.
    std::optional<std::uint32_t> headChild;
    std::optional<std::uint32_t> declaratorListChild;
    std::optional<std::uint32_t> declaratorChild;
    // Function-decl child roles (SE6). `paramsChild` points at the visible
    // child whose subtree holds the parameter declarations; `bodyChild` at
    // the body subtree (which is also a `scopes` rule so params bind into
    // it). `nullopt` for non-function declarations.
    std::optional<std::uint32_t> paramsChild;
    std::optional<std::uint32_t> bodyChild;
    // FC17.5 (D-CSUBSET-AUTO-TYPE-INFERENCE, C23 6.7.9): when true, this
    // declarator-mode row has NO type-specifier head — the declared type is
    // INFERRED from the sole declarator's initializer at Pass 1.5 (the
    // definitive resolveDeclTypes visit, so the inferred type is visible to
    // later same-pass consumers like sizeof-in-array-dims). A generic opt-in
    // engine capability ("the type derives from the initializer"): the loader
    // relaxes the declarator-mode `head` requirement for rows carrying it and
    // rejects a row that sets BOTH (C_ConflictingField — the two type sources
    // would compete). The inference itself is the config-driven Pass-1.5 arm
    // (single plain named declarator, initializer required, array/function
    // decay, loud rejects for uninferable initializers). Default false — every
    // pre-existing row keeps the mandatory head, byte-identical.
    bool inferTypeFromInitializer = false;
    // FC17.5 (D-CSUBSET-AUTO-TYPE-INFERENCE): a token kind that MUST appear in
    // this declaration's specifier prefix for the row to be semantically valid
    // — the inference-marker presence gate (C23 6.7.9p1: type inference
    // happens only under the `auto` storage-class specifier). WITHOUT this
    // gate a headless row would silently accept C89 implicit-int shapes
    // (`static x = 5;` / `register y = 2;` parse structurally identical to an
    // inference declaration). Checked FIRST in the Pass-1.5 inference arm;
    // absent token ⇒ loud error, never a silently-adopted initializer type.
    // Loader-resolved from a token-kind NAME (the constMarker idiom — the
    // engine never names a keyword). `nullopt` ⇒ no presence gate (a language
    // whose inference form is structurally unambiguous).
    std::optional<SchemaTokenId> requiredSpecifierToken;
    // SE4 const-correctness: a token kind that, when found anywhere in the
    // `typeChild` subtree (or the whole declaration subtree when no
    // `typeChild` is set), marks the minted symbol const. `nullopt` ⇒ the
    // language has no const marker for this declaration form.
    std::optional<SchemaTokenId> constMarker;
    // c21 (D-CSUBSET-VOLATILE-QUALIFIER): a token kind that, when found in the
    // `typeChild` subtree (or the whole declaration subtree when no `typeChild`),
    // marks the minted symbol VOLATILE — mirrors `constMarker` exactly (an
    // independent scan, so `const volatile` sets BOTH bits). Read at HIR-access
    // lowering to thread `MirInstFlags::Volatile` onto the symbol's Load/Store so
    // the optimizer (DCE/CSE/Mem2Reg/LICM, all already Volatile-aware) cannot
    // elide or reorder a volatile access. `nullopt` ⇒ the language has no volatile
    // marker for this declaration form. c27 (D-CSUBSET-VOLATILE-POINTEE): this
    // token ALSO drives the resolver's VolatileQual construction — a head volatile
    // wraps the base (`volatile int *` => Ptr<VolatileQual(int)>) and an east
    // ptrQualifier volatile wraps the pointer; the former pointee-volatile reject is
    // retired (volatile is now a type qualifier). Config-driven, no hardcoded keyword.
    std::optional<SchemaTokenId> volatileMarker;
    // D-LANG-VARIADIC (step 13.4, 2026-06-02): a token kind that, when
    // found anywhere in this declaration's params subtree (the subtree
    // rooted at the `paramsChild` visible child), marks the declared
    // FnSig as C-style variadic. The semantic analyzer scans for this
    // token at FnSig-build time and passes `isVariadic=true` to the
    // 4-arg `TypeInterner::fnSig()` overload when present. Source-
    // language agnostic: each language declares its own marker token
    // (c-subset: `EllipsisOp`; future Rust would declare none; etc.).
    // `nullopt` ⇒ the language has no variadic-marker for this
    // declaration form (the FnSig is always non-variadic).
    std::optional<SchemaTokenId> variadicMarker;
    // D-DECL-SPECIFIER-PREFIX-SUBSTRATE (2026-06-04): an optional leading
    // declaration-specifier prefix — a child whose rule is this RuleId, sitting
    // BEFORE the type/name (e.g. C `static int f()` / `__attribute__((weak)) int
    // g()`). When set AND the declaration's first visible child matches this
    // rule, the resolver STRIPS it before resolving the positional
    // `typeChild`/`nameChild`/`paramsChild`/`bodyChild`/`kindByChild` indices, so
    // those indices stay stable whether or not specifiers are present (a leading
    // optional child would otherwise shift them). The prefix subtree remains
    // reachable for a per-language specifier→attribute scan (e.g. linkage). The
    // engine learns nothing language-specific: WHICH rule is the prefix, and what
    // its specifiers mean, are both per-language config. `nullopt` ⇒ this
    // declaration form has no specifier prefix (every shipped decl today).
    std::optional<RuleId>        specifierPrefixRule;
    // D-CSUBSET-LINKAGE-SPECIFIERS (pre-OPT7 P1, 2026-06-04): maps a
    // declaration-specifier token's SOURCE TEXT to its linkage effect (see
    // `LinkageSpecifierEffect`). Consulted by CST→HIR lowering ONLY for the
    // specifier tokens inside this declaration's `specifierPrefixRule` subtree;
    // the resolved `LinkageAttr` is attached to the HIR Function/Global node and
    // threaded to the MirFunc/MirGlobal binding+visibility (the DCE-protect
    // input). Empty ⇒ this declaration form derives no linkage from specifiers
    // (every shipped decl before c-subset `static` landed). Source/target/linker
    // agnostic: the VALUES reuse the shared `SymbolBinding`/`SymbolVisibility`
    // vocabulary; the token→effect MAP is per-language config.
    std::unordered_map<std::string, LinkageSpecifierEffect> linkageSpecifiers;
    // D-CSUBSET-LINKAGE-UNKNOWN-SPECIFIER-DIAGNOSTIC (cycle 14): the specifier
    // prefix's STRUCTURAL token kinds — syntax, NOT specifier-identities (e.g.
    // `__attribute__`, `(`, `)`) — to SKIP when scanning the prefix for linkage.
    // Any prefix token whose kind is NOT in this set MUST resolve in
    // `linkageSpecifiers`, else it is an unrecognized specifier and fails loud
    // (`H_UnknownLinkageSpecifier`). The skip-list's DEFAULT is fail-loud: an
    // unanticipated/typo'd specifier (a kind not listed here) is validated, so it
    // ERRORS rather than being silently ignored — the safe direction. Resolved
    // loader-side from token-kind names (unknown name → fail-loud). Empty for a
    // declaration form that derives no linkage from specifiers.
    std::vector<SchemaTokenId> linkageSpecifierIgnoredKinds;
    // FC4 c1 (D14): RULES whose entire SUBTREES the linkage prefix scan skips
    // wholesale — attribute forms a language parses but semantically IGNORES
    // (C23 `[[deprecated]]`: its identifiers must neither resolve as linkage
    // specifiers nor fire H_UnknownLinkageSpecifier). Loader-resolved rule
    // names (unknown → fail-loud); empty ⇒ nothing skipped (the strict
    // default — an unanticipated subtree's tokens are still validated).
    std::vector<RuleId>        linkageSpecifierIgnoredRules;
    // FC16 (D-CSUBSET-NORETURN): specifier IDENTIFIER spellings the linkage scan
    // skips as semantic NO-OPs — completing the ignore trio at identifier
    // granularity (ignoredKinds = token kinds; ignoredRules = whole subtrees;
    // THIS = identifier texts). Needed for a non-linkage ATTRIBUTE that shares the
    // GNU `__attribute__((...))` rule with HONORED linkage attributes (`weak`,
    // `visibility`), so its subtree cannot be ignored wholesale: `noreturn` must
    // be skipped WITHOUT firing H_UnknownLinkageSpecifier AND without giving it a
    // spurious linkage EFFECT (a `{binding:global}` no-op entry would clobber a
    // co-present `static`/`weak` last-wins — an order-dependent silent linkage
    // miscompile). Matched dunder-normalized (`stripDunder`), so a single
    // `"noreturn"` entry covers `noreturn` AND `__noreturn__`. An identifier NOT
    // listed here (and not a recognized linkage specifier) STILL fails loud — the
    // strict default is preserved. Empty ⇒ nothing skipped by name.
    std::vector<std::string>   linkageSpecifierIgnoredNames;
    DeclarationKind kind        = DeclarationKind::Variable;
    NameMatchMode   nameMatch   = NameMatchMode::Self;
    // FC4 c1 stage 2a: when true, every declarator under this (declarator-
    // mode) row must carry a NAME — an abstract declarator (`int *;`,
    // `int (int);`) emits S_DeclarationDeclaresNothing positioned at the
    // declarator. C's named declaration positions (locals, globals,
    // typedefs) declare true; parameter-like positions (abstract
    // declarators legal) leave it false. Default false = the permissive
    // direction ONLY because abstract declarators mint nothing — there is
    // no silent-wrong-binding risk, just a silently-useless declaration,
    // which named-position rows opt into rejecting.
    bool            requireNamedDeclarators = false;
    // FC4 c1 stage 2a: when true, a row whose name child is structurally
    // absent or not an identifier leaf (C's ANONYMOUS composite forms —
    // `typedef struct { ... } T;`) still mints a TYPE symbol under a
    // synthesized unique name ("<anon:rule:node>") with the declaration
    // node itself as the symbol's node anchor. The composite/fieldChildren
    // machinery then composes its lattice type exactly like a named one,
    // and type-position resolution returns the minted type via the node
    // anchor. Default false: a garbled name child mints nothing (the
    // legacy degrade).
    bool            anonymousNameAllowed = false;
    // D8 unused-variable warning: when true, a symbol minted by this
    // declaration that is NEVER referenced (empty use-set after analysis)
    // emits S_UnusedVariable (a WARNING). Per-declaration opt-in so a
    // language can warn on local variables but not on parameters (unused
    // params are intentional) or globals/columns. Default false ⇒ no
    // unused check for this declaration form.
    bool            warnIfUnused = false;
    // D-CSUBSET-EXTERN-DEFINITION-MERGE: when true, a symbol minted by this
    // declaration is a NON-DEFINING declaration — it announces a name whose
    // storage/body lives in another translation unit (an `extern` declaration in
    // C). Such a declaration MERGES with an in-TU DEFINITION of the same name: the
    // definition WINS the binding and this non-defining declaration is absorbed
    // (its HIR ExternFunction/ExternGlobal node is suppressed). Two non-defining
    // declarations of the same name are idempotent; two definitions still collide
    // (S_RedeclaredSymbol). Per-declaration opt-in (c-subset's `externDecl`),
    // source-agnostic — the engine never hardcodes a rule name. Default false ⇒
    // an ordinary defining declaration (a redeclaration collides as before).
    bool            nonDefiningDeclaration = false;
    // c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): when true, a SURVIVING bare
    // function PROTOTYPE minted by this declaration (`int f(int);` — a proto with
    // NO in-TU definition: `isProtoDeclaration && !isAbsorbedProto`, external
    // linkage) synthesizes an ExternFunction HIR node with NO library binding
    // (C 6.2.2p5 — an undecorated function declaration has external linkage and
    // refers to a definition SOMEWHERE in the program). Resolution order:
    //   (1) the whole-program LK11 merge binds it to a sibling-TU DEFINITION
    //       (sqlite3.c defines what shell.c bare-declares) — import row stripped,
    //       calls rewired direct;
    //   (2) a bare re-declaration of a SHIPPED descriptor symbol re-binds to
    //       that descriptor's library (goal-2 suppressed the descriptor's own
    //       injection because the user decl claimed the name — the proto's
    //       synthesized extern carries the descriptor's per-format library map
    //       instead, so `puts` re-declared over `#include <stdio.h>` still
    //       imports from libc);
    //   (3) NEITHER ⇒ the import survives with an empty library and the LINKER
    //       rejects it LOUD as an undefined symbol (K_SymbolUndefined naming the
    //       symbol — ld's behavior).
    // false ⇒ the pre-c86 shape: an unabsorbed proto emits nothing and a call to
    // it fails loud at HIR→MIR (H0009 Ref to unbound symbol). Per-declaration
    // opt-in (c-subset's `topLevelDecl` + `varDecl`), source-agnostic — the
    // engine reads only this flag, never a rule name. Internal-linkage (`static`)
    // and weak protos never synthesize (their reference must NOT bind another
    // TU's public symbol); they keep the loud H0009. Default false.
    bool            prototypeSynthesizesExtern = false;
    // D-LK10-ENTRY-MAIN-IMPLICIT-RETURN: HIR-tier implicit-return
    // insertion rule (source-agnostic). When this declaration is a
    // FUNCTION declaration AND the declared symbol's name appears
    // in this list AND the function's return type is non-void AND
    // the function's body does not structurally terminate on every
    // path, the HIR lowering appends a synthetic `return <zero>`
    // (a synthetic literal of the function's return type) as the
    // last statement of the body's outermost Block. Per C99
    // §5.1.2.2.3 for `main`; configurable per language so other
    // source languages can declare their own entry-fn conventions
    // (Pascal's `program`, Rust's `fn main`, etc.) WITHOUT touching
    // shared HIR substrate. Empty ⇒ no implicit insertion for any
    // function of this declaration form (every non-terminating non-
    // void function then falls through to the verifier's
    // checkReturnCompleteness loud-fail, which is the language-
    // strict default).
    //
    // Both the synthetic ReturnStmt AND a fresh wrapping Block are
    // appended (both flagged `HirFlags::Synthetic`); the original
    // body's children are copied into the new block verbatim with
    // the synthetic return appended at the tail. The original Block
    // node is left detached (no in-place node mutation — HIR is
    // built bottom-up immutable). Restricted to integer return
    // types (Bool / I8..I128 / U8..U128 / Char / Byte) so a non-
    // conformant `float main()` or `struct S main()` doesn't get a
    // silently wrong-typed synthetic return — those fall through
    // to the verifier's loud-fail. The verifier then sees a
    // terminating body and downstream MIR/LIR see a defined return
    // value at the function's exit register — preventing the
    // "garbage-rax-at-exit" downstream of the runnable-binary
    // trampoline (D-LK10-ENTRY).
    std::vector<std::string> implicitReturnZeroForFunctionNames;
    // FC5 (D-LK10-ENTRY-MAIN-IMPLICIT-RETURN): the program ENTRY-point function
    // name(s) — the symbol(s) the driver resolves as the executable's entry. This
    // is SEPARATE from `implicitReturnZeroForFunctionNames` (the C `main`-style
    // reach-`}`-⇒-`return 0` set): a language could declare an entry whose
    // fall-through is NOT a return-0 (e.g. a `void`-returning runtime entry), or a
    // return-0 function that is not the entry. For c-subset both are `["main"]`,
    // but the driver's entry-symbol resolution reads THIS field so the two
    // concepts can diverge without one silently dragging the other. Absent/empty →
    // the driver falls back to its format-declared entry default.
    std::vector<std::string> entryFunctionNames;
    // Optional kind-discriminator. When set, the engine evaluates it at
    // pass 1 and uses the resulting effective kind / params / body
    // instead of the static fields above.
    std::optional<KindDiscriminator> kindByChild;
    // SE-arrays (HR9): optional C-style declarator suffix (e.g. `int a[10]`).
    // The suffix is a sibling of the type (not a type-position constructor), so
    // the engine matches it by rule within the declaration subtree rather than
    // via `typeShapes`. `nullopt` ⇒ this declaration form has no array syntax.
    std::optional<ArraySuffix> arraySuffix;
    // FC8 D-CSUBSET-BITFIELD: optional C bit-field declarator suffix (`int x:3`).
    // Matched by rule within the field subtree (a sibling of the name, like
    // arraySuffix). `nullopt` ⇒ this declaration form has no bit-field syntax.
    std::optional<BitfieldSuffix> bitfieldSuffix;
    // C23 6.7.2.2 (D-CSUBSET-ENUM-UNDERLYING-TYPE, FC17): optional explicit enum
    // underlying-type clause (`enum E : unsigned char { … }`). Matched by rule
    // within the composite subtree. `nullopt` ⇒ this declaration form has no
    // explicit-underlying-type syntax (the enum defaults to int).
    std::optional<EnumUnderlyingTypeSpec> enumUnderlyingType;
    // FC6 (FAM): when true, an ABSENT array length on this declaration form
    // (`T x[]`) resolves to an INCOMPLETE array type (C99 §6.7.2.1 flexible
    // array member) instead of the `S_NonConstantArrayLength` error. Only
    // declaration forms that legally bear a flexible array set this — a struct
    // field. A standalone `T x[]` (a local/global) keeps `allowFlexibleArray =
    // false`, so its absent length still fails loud. Config-driven: the language
    // declares which declaration forms may carry a flexible array; the engine
    // never hard-codes "struct field".
    bool allowFlexibleArray = false;
    // c82 D-CSUBSET-PARAM-ARRAY-ADJUSTMENT (C 6.7.6.3p7): when true, a declarator
    // on this declaration form whose resolved type is 'array of T' — sized OR
    // unsized — ADJUSTS to 'pointer to T'. The flag both (a) permits the absent
    // length (`T x[]`), resolving through the SAME incomplete-array path
    // `allowFlexibleArray` uses, and (b) rewrites the resolved TOP-LEVEL array to
    // Ptr<element> at both resolution sites (the definitive Pass-1.5 visit that
    // binds the symbol, and the FnSig param harvest), so the bound symbol, the
    // FnSig, and every call site agree on the adjusted pointer. Only declaration
    // forms with C parameter semantics set this (c-subset's `param` row); the
    // engine never hard-codes "parameter". Inner array dimensions are untouched
    // (`int a[][5]` → Ptr<Array<int,5>>), and an inner ABSENT dimension still
    // fails loud via the incomplete-element-in-aggregate guard.
    bool arrayToPointer = false;
    // D5.1: optional composite-type collection. When set, Pass 1.5 composes the
    // declaration's `kind: type` symbol's TypeId via `interner.structType(name,
    // fieldTypes)` from the field-symbols minted in this declaration's scope.
    // `kind` must be `Type` and the rule must also appear in `scopes`. Generic
    // across record-bearing languages.
    std::optional<FieldChildrenDescriptor> fieldChildren;
    // c25 D-CSUBSET-UNIFIED-COMPOSITE-SPECIFIER: DUAL-MODE gate. When set, this
    // declaration row is a DEFINITION site ONLY when the matching node has a
    // VISIBLE CHILD of this rule; when that child is ABSENT the node is NOT a
    // definition (it mints nothing, opens no scope, binds no tag) — it is instead
    // a pure REFERENCE, resolved through a SEPARATE `references[]` row declared on
    // the SAME grammar rule. This lets ONE grammar rule (C's unified
    // `struct-or-union-specifier` — c-subset's `structSpec`/`unionSpec`/`enumSpec`,
    // shaped `Kw {opt tag} {opt body}`) serve BOTH the type-definition form
    // (`struct P { … }` — a `structBody` child present) and the bare tag-reference
    // form (`struct P` — absent), so the parser can treat it as the SOLE candidate
    // for its lead keyword (unique-production direct descent — no body-vs-ref
    // speculation budget). Generic + agnostic: the engine keys on a CHILD RULE
    // name (resolved loader-side to this RuleId); no keyword/language is hardcoded.
    // `nullopt` ⇒ this declaration is ALWAYS a definition (every shipped decl
    // before c25). The body-present semantics are EXACTLY the row's existing
    // define path (fieldChildren / scope / tag-bind); the body-absent resolution
    // is the existing `isTagReference` path on the paired reference row.
    //
    // The loader REQUIRES the paired `references[]` row to exist for the SAME rule
    // when this is set (else a body-absent occurrence would silently resolve to
    // nothing), and that the named child rule exists.
    std::optional<RuleId> definesWhenChildRule;
    std::string           definesWhenChildRuleName;   // source spelling, for diagnostics
    // FC4 c1 (M5): config-driven fail-loud marker gates. At semantic
    // analysis of this declaration (declarator-mode AND legacy rows alike),
    // each entry whose token appears in the decl subtree emits its declared
    // diagnostic as an ERROR, positioned at the first such token. See
    // `GatedMarker` above. Empty ⇒ no gates for this declaration form.
    std::vector<GatedMarker> gatedMarkers;
    // Source-text name of the declared rule, retained for diagnostics.
    std::string     ruleName;

    // FC4 c1: true when this row declares the declarator-mode child roles
    // (any of head/declaratorList/declarator set — the loader guarantees a
    // consistent trio). The mode discriminator every consumer branches on.
    [[nodiscard]] bool isDeclaratorMode() const noexcept {
        return headChild.has_value() || declaratorListChild.has_value()
            || declaratorChild.has_value();
    }
};

// SE4: an assignment expression. When Pass 2 sees a node with this rule,
// it resolves the LHS child to a symbol; if that symbol is const, it
// emits S_ConstViolation. `operatorToken` (when set) gates the match —
// the rule only counts as an assignment when one of its visible children
// is a token of that kind (so an operator-table `binaryExpr` reused for
// every binary op only fires on the assignment operator).
//
// When several entries share the same `rule` (e.g. one per compound-assign
// operator), the engine applies the FIRST entry whose `operatorToken` gate
// matches, then stops. Invariant: an UNGATED entry (no `operatorToken`)
// matches every node of its rule, so it must be the SOLE entry for that rule
// — mixing an ungated entry with gated ones would let the ungated catch-all
// fire first and shadow the gated entries. Shipped configs gate every entry.
struct DSS_EXPORT AssignmentRule {
    RuleId                       rule{};
    std::optional<SchemaTokenId> operatorToken;
    std::uint32_t                lhsChild = 0;
    std::uint32_t                rhsChild = 0;
    std::string                  ruleName;
};

// SE6: a call expression. When Pass 2 sees a node with this rule, it
// resolves the `calleeChild` subtree to a symbol; if that symbol's type
// is not a FnSig → S_NotCallable; otherwise it counts the comma-separated
// args in the `argsChild` subtree against the signature's arity
// (S_ArgCountMismatch) and checks each arg's assignability (S_TypeMismatch).
struct DSS_EXPORT CallRule {
    RuleId        rule{};
    std::uint32_t calleeChild = 0;
    std::uint32_t argsChild   = 0;
    // Optional gating token kind. When set, the rule only counts as a
    // call when one of its visible children IS a token of this kind —
    // analogous to AssignmentRule's `operatorToken`. Used by languages
    // (e.g. c-subset) whose `postfixExpr` is shared across `++`/`--`/`[]`
    // AND call shapes; the call site is the one with `(`.
    std::optional<SchemaTokenId> operatorToken;
    std::string   ruleName;
};

// c103 (D-CSUBSET-INTRINSIC-UMULH): a builtin whose call the engine lowers to a
// DEDICATED compiler intrinsic (a target instruction) rather than an ordinary
// call/import. A LEAF enum with NO HIR/MIR dependency (it lives in core so the
// SemanticConfig + SymbolRecord can carry it): resolved from the config
// `lowering` string at decode, then each downstream layer maps it into its OWN
// vocabulary -- the HIR lowering (cst_to_hir) maps it onto a `HirKind::BuiltinCall`
// payload, and the MIR lowering (hir_to_mir) maps THAT onto the concrete
// `MirOpcode`. No layer depends upward and no arch/name identity branch appears
// in shared substrate; the string->enum and enum->enum maps are uniform tables.
enum class BuiltinLowering : std::uint16_t {
    None = 0,     // ordinary semantic-only builtin (e.g. tsql COALESCE)
    UMulHigh,     // __umulh: high 64 bits of the u64*u64 128-bit product
    // c104 (D-CSUBSET-INTRINSIC-ATOMIC-CAS): InterlockedCompareExchange — the
    // atomic compare-and-swap. Operands (ptr, comparand, newval) → the ORIGINAL
    // value at *ptr; iff original==comparand the newval is stored, atomically
    // (x86 `lock cmpxchg`; arm64 LDAXR/STLXR acquire-release loop).
    AtomicCas,
    // c113 (D-CSUBSET-INTRINSIC-BARRIER): _ReadWriteBarrier — an MSVC COMPILER
    // reordering barrier (NOT a CPU fence). Takes no operands, produces no value,
    // emits NO runtime instruction; its whole job is to forbid the optimizer from
    // moving memory accesses (loads OR stores) across it. Realized by a
    // side-effecting zero-operand MIR op (MirOpcode::CompilerBarrier) that the
    // CSE/LICM clobber walk treats as a full memory clobber.
    Barrier,
    // c115 (SEH arc 2/3, D-WIN64-SEH-FUNCLETS): the two MSVC SEH intrinsics —
    // excpt.h's `_exception_code()` (→ u32, the exception code) and
    // `_exception_info()` (→ void*, the EXCEPTION_POINTERS). Legal ONLY inside
    // an `__except` filter expression (_exception_code also in the handler
    // body) — HirVerifier::checkSehContext enforces it. Each lowers to a
    // dedicated zero-operand value MIR op (SehExceptionCode / SehExceptionInfo)
    // that the c116 funclet lowering wires to the __C_specific_handler dispatch
    // context; until then mir_to_lir fails loud on them.
    SehExceptionCode,
    SehExceptionInfo,
    // FC17.9(b) walking-skeleton (D-CSUBSET-BITCOUNT-INTRINSICS): the 3 hardware
    // bit-count primitives, exposed as 6 GCC-compatible builtins that SHARE these
    // 3 lowerings (the {,ll} width pair per op). Each maps to a dedicated pure
    // unary MirOpcode (Popcount/Clz/Ctz) at hir_to_mir — the C23 <stdbit.h>
    // substrate. Clz/Ctz are defined at 0 = width (LZCNT/CLZ/TZCNT semantics), a
    // safe superset of GCC's UB-at-0.
    Popcount,
    Clz,
    Ctz,
    // FC17.9(b) C23 <stdbit.h> (D-FULLC-STDBIT): the 14 type-generic `stdc_*`
    // bit operations, each a distinct leaf lowering that COMPOSES the 3 hardware
    // primitives (Popcount/Clz/Ctz) + universal ALU verbs into the N3096 §7.18
    // formula at hir_to_mir (the ONE place the width-correct, single-eval,
    // branchless composition lives). The operand's EXACT width W∈{8,16,32,64} is
    // read from its param core (56 `__builtin_stdc_<op>_<T>` builtins, 4 widths ×
    // 14 ops); all count/index/width ops return U32, has_single_bit returns Bool,
    // bit_floor/bit_ceil return the operand core (C23 return-type rules). NO new
    // MIR op — these are pure HIR→MIR composition over the proven substrate.
    StdcLeadingZeros,
    StdcLeadingOnes,
    StdcTrailingZeros,
    StdcTrailingOnes,
    StdcFirstLeadingZero,
    StdcFirstLeadingOne,
    StdcFirstTrailingZero,
    StdcFirstTrailingOne,
    StdcCountZeros,
    StdcCountOnes,
    StdcHasSingleBit,
    StdcBitWidth,
    StdcBitFloor,
    StdcBitCeil,
    // FC17.9(d) atomic cycle-1 (D-CSUBSET-ATOMIC): the C11 <stdatomic.h> explicit-
    // order scalar accessors `atomic_load_explicit`/`atomic_store_explicit`. Each maps
    // to the dedicated MirOpcode AtomicLoad/AtomicStore (the SAME ops a bare `_Atomic`
    // access emits) with the memory_order arg const-folded into MirInst.payload (0..5)
    // at hir_to_mir. APPENDED here (not grouped by AtomicCas) so every pre-existing
    // enumerator keeps its integer value — the BuiltinCall payload prints numerically
    // in `.dsshir` text (the TypeKind-placed-LAST numeric-stability precedent).
    AtomicLoad,
    AtomicStore,
    // C99 _Complex (D-CSUBSET-COMPLEX §7.3): the complex builtins the <complex.h>
    // macros route to. ComplexMake(re, im) constructs a complex BY ADDRESS (the
    // first aggregate-returning builtin — its "value" is the materialized slot
    // address; CRITICAL-2). ComplexReal/ComplexImag take a complex BY ADDRESS (the
    // request value->address flip delivers it) and Gep+Load the F64 component.
    // ComplexConj copies re, negates im into a fresh slot (by address). APPENDED
    // (not grouped) so every pre-existing enumerator keeps its integer value — the
    // BuiltinCall payload prints numerically in `.dsshir` text (the AtomicLoad/Store
    // + TypeKind-placed-LAST numeric-stability precedent).
    ComplexMake,
    ComplexReal,
    ComplexImag,
    ComplexConj,
};

// Resolve the config `lowering` name to its BuiltinLowering. nullopt = an unknown
// name (rejected with a diagnostic at the decode site) -- distinct from "absent"
// (an ordinary builtin, which never carries a `lowering` key).
[[nodiscard]] inline std::optional<BuiltinLowering>
builtinLoweringFromName(std::string_view name) noexcept {
    if (name == "umulh")      { return BuiltinLowering::UMulHigh;  }
    if (name == "atomic_cas") { return BuiltinLowering::AtomicCas; }
    // FC17.9(d) atomic cycle-1 (D-CSUBSET-ATOMIC): the explicit-order scalar accessors.
    if (name == "atomic_load")  { return BuiltinLowering::AtomicLoad;  }
    if (name == "atomic_store") { return BuiltinLowering::AtomicStore; }
    // C99 _Complex (D-CSUBSET-COMPLEX §7.3): the complex-builtin lowerings.
    if (name == "complex_make") { return BuiltinLowering::ComplexMake; }
    if (name == "complex_real") { return BuiltinLowering::ComplexReal; }
    if (name == "complex_imag") { return BuiltinLowering::ComplexImag; }
    if (name == "complex_conj") { return BuiltinLowering::ComplexConj; }
    if (name == "barrier")    { return BuiltinLowering::Barrier;   }
    if (name == "seh_exception_code") { return BuiltinLowering::SehExceptionCode; }
    if (name == "seh_exception_info") { return BuiltinLowering::SehExceptionInfo; }
    // FC17.9(b) (D-CSUBSET-BITCOUNT-INTRINSICS): the 3 bit-count verbs shared by
    // the 6 __builtin_{popcount,clz,ctz}{,ll} rows (the width lives in the param
    // core U32/U64, read by the hir_to_mir arm — the lowering tag is width-blind).
    if (name == "popcount") { return BuiltinLowering::Popcount; }
    if (name == "clz")      { return BuiltinLowering::Clz;      }
    if (name == "ctz")      { return BuiltinLowering::Ctz;      }
    // FC17.9(b) C23 <stdbit.h> (D-FULLC-STDBIT): the 14 `stdc_*` op lowerings
    // (shared by each op's 4 width rows — the width lives in the param core, read
    // by the hir_to_mir arm; the lowering tag is width-blind, like popcount/clz/ctz).
    if (name == "stdc_leading_zeros")       { return BuiltinLowering::StdcLeadingZeros; }
    if (name == "stdc_leading_ones")        { return BuiltinLowering::StdcLeadingOnes; }
    if (name == "stdc_trailing_zeros")      { return BuiltinLowering::StdcTrailingZeros; }
    if (name == "stdc_trailing_ones")       { return BuiltinLowering::StdcTrailingOnes; }
    if (name == "stdc_first_leading_zero")  { return BuiltinLowering::StdcFirstLeadingZero; }
    if (name == "stdc_first_leading_one")   { return BuiltinLowering::StdcFirstLeadingOne; }
    if (name == "stdc_first_trailing_zero") { return BuiltinLowering::StdcFirstTrailingZero; }
    if (name == "stdc_first_trailing_one")  { return BuiltinLowering::StdcFirstTrailingOne; }
    if (name == "stdc_count_zeros")         { return BuiltinLowering::StdcCountZeros; }
    if (name == "stdc_count_ones")          { return BuiltinLowering::StdcCountOnes; }
    if (name == "stdc_has_single_bit")      { return BuiltinLowering::StdcHasSingleBit; }
    if (name == "stdc_bit_width")           { return BuiltinLowering::StdcBitWidth; }
    if (name == "stdc_bit_floor")           { return BuiltinLowering::StdcBitFloor; }
    if (name == "stdc_bit_ceil")            { return BuiltinLowering::StdcBitCeil; }
    return std::nullopt;
}

// SE6: a built-in function the engine binds into a CU-wide "builtins"
// scope (visible everywhere, shadow-able by user decls). Interned as a
// FnSig over `paramCores` → `resultCore`. A `variadic` builtin skips the
// arg-count check (e.g. tsql's COALESCE accepts any arity).
struct DSS_EXPORT BuiltinFunctionMapping {
    std::string           name;
    std::vector<TypeKind> paramCores;
    TypeKind              resultCore = TypeKind::Void;
    bool                  variadic   = false;
    // c103: when != None, a call to this builtin lowers to the named compiler
    // intrinsic (a target instruction) instead of an ordinary call. None (the
    // default) preserves the pure-semantic builtin behaviour (COALESCE).
    BuiltinLowering       lowering   = BuiltinLowering::None;
    // c104 (D-CSUBSET-INTRINSIC-ATOMIC-CAS): OPTIONAL full type-text signature
    // (the ONE shipped-lib codec, e.g. "fn(ptr<i32>, i32, i32) -> i32") for a
    // builtin whose parameters need REAL types the scalar `paramCores` axis
    // cannot express (pointers). Schema decode is interner-free, so the TEXT is
    // stored here and parsed at the semantic INJECTION site (where the CU's
    // interner exists) via `parseTypeFromText` — exactly how shipped-lib symbol
    // signatures decode. Mutually exclusive with params/result (fail-loud at
    // decode if both are present); must decode to an FnSig (fail-loud else).
    std::string           signatureText;
    // D-LANG-TYPE-IDENTITY-VOCABULARY: OPTIONAL per-data-model REPLACEMENT for
    // `signatureText` — the exact shape (and JSON key name) the shipped-lib
    // reader's `signatureByDataModel` already uses. A platform intrinsic can
    // carry a parameter C spells with a NAMED type whose vocabulary entry is
    // data-model-dependent: `_InterlockedCompareExchange` takes a `LONG*`, i.e.
    // `long*`, which is a 32-bit `long` on the LLP64 platform the intrinsic
    // belongs to. A single FIXED signature cannot say that without lying on the
    // other model, so the base text stays the model-agnostic one and each
    // declared model overrides it. EAGER: every declared override is decoded at
    // the injection site regardless of which model is active, so a malformed
    // INACTIVE override fails on EVERY target (anti-lurking).
    std::unordered_map<DataModel, std::string> signatureTextByDataModel;
};

// D5.1: a member-access expression rule. When Pass 2 sees a node with this
// rule, it (a) resolves the LHS subtree (the object) to its expression type via
// `typeAt`, (b) follows the LHS type — through one `Ptr` indirection if
// `dereferences == true` (the arrow form `p->x`) — to its `TypeKind::Struct`
// pointee, (c) looks up the RHS field-name identifier in the struct symbol's
// inner scope, (d) records the resolved field symbol on the field-name node
// via `nodeToSymbol_` and the field's type on the member-access node via
// `nodeToType_`. The HIR lowering reads the field's `SymbolRecord::fieldIndex`
// to produce a `MemberAccess` node with the right payload.
//
// `lhsChild` / `nameChild` are visible-child indices. The arrow form is
// modelled as a *distinct* `MemberAccessRule` entry (`dereferences: true`) so
// the schema declares both `p->x` and `p.x` shapes if the language has both;
// the engine never special-cases either at parse time.
struct DSS_EXPORT MemberAccessRule {
    RuleId        rule{};
    std::uint32_t lhsChild  = 0;       // visible-child index of the object subtree
    std::uint32_t nameChild = 0;       // visible-child index of the field-name token
    bool          dereferences = false; // true ⇒ `p->x` (deref the LHS pointer first)
    // Optional gating token kind — when multiple entries share the same `rule`
    // (e.g. c-subset's `postfixExpr` covers both `.` and `->` shapes), each
    // entry is distinguished by the operator token present in the node's
    // visible children. Pass 2 picks the FIRST matching entry. Parallels
    // `AssignmentRule.operatorToken` and `CallRule.operatorToken`. An ungated
    // entry (no `operatorToken`) matches every node of its rule, so when an
    // ungated entry is present it must be the SOLE entry for that rule.
    std::optional<SchemaTokenId> operatorToken;
    std::string   ruleName;
};

// FC2: an explicit cast expression (`(T)expr` in C-family syntax). When
// Pass 2 sees a node with this rule, it (a) resolves the TYPE-position
// subtree at visible child `typeChild` via the standard type-position
// resolver (builtins + pointer stars + struct refs + typedef aliases —
// S_UnknownType on a name that resolves to nothing), (b) stamps the
// resolved target type on BOTH the type child (so the HIR lowering's
// stamped-type probe finds it, the compound-literal precedent) and the
// cast node itself (the expression's RESULT type for enclosing checks),
// and (c) validates the (target, operand) pair against the explicit-cast
// matrix (`isExplicitCastable`) — emitting S_InvalidCast on illegal
// pairs (struct-value casts, void, arrays). The operand's type is read
// from the visible child at `operandChild` (post-order traversal has
// already typed it). Engine-generic: WHICH rule is a cast and WHERE its
// children sit is per-language config.
struct DSS_EXPORT CastRule {
    RuleId        rule{};
    std::uint32_t typeChild    = 0;   // visible-child index of the type subtree
    std::uint32_t operandChild = 0;   // visible-child index of the operand expr
    std::string   ruleName;           // source spelling, for diagnostics
};

// FC3.5 sweep-c3 (D-CSUBSET-COMPOUND-LITERAL-TYPEDEF): a compound
// literal expression (`(T){...}` in C-family syntax). Pass 2 resolves
// the TYPE-position subtree at visible child `typeChild` via the SAME
// standard type-position resolver casts use (builtins + pointer stars
// + struct refs + typedef aliases) and stamps the resolved type on
// BOTH the type child (the HIR lowering's `resolveStampedTypeBelow`
// probe) and the node itself (the literal's RESULT type for enclosing
// checks). Deliberately a SEPARATE vocabulary from `CastRule`: a
// compound literal is C 6.5.2.5 postfix syntax, NOT a conversion — no
// operand child exists and the explicit-cast matrix must never run
// against the brace-init (the per-element checks live in the HIR
// brace-init lowering, contextually typed by the stamped type).
// Pre-sweep only struct-ref type children worked (the struct-name
// resolution stamped them as a side effect); builtin keywords and
// typedef names in compound-literal position resolved to NOTHING and
// the HIR lowering fail-louded.
struct DSS_EXPORT CompoundLiteralRule {
    RuleId        rule{};
    std::uint32_t typeChild = 0;      // visible-child index of the type subtree
    std::string   ruleName;           // source spelling, for diagnostics
};

// Identifier-use recognition. The named rule (whose RuleId the loader
// resolves) is a "reference site": when Pass 2 sees a node with this
// rule, it extracts the identifier text per `nameMatch` and does a
// scope-chain lookup.
struct DSS_EXPORT ReferenceRule {
    RuleId          rule{};
    NameMatchMode   nameMatch = NameMatchMode::Self;
    std::string     ruleName;
    // Positional control over the "unresolved is an error" decision, for
    // languages where the SAME reference rule appears both in must-resolve and
    // bind-late positions. T-SQL's `qualifiedName` is a TABLE reference under
    // `tableRef` / the DML-statement target (must resolve — a missing table is an
    // error) but a relational COLUMN reference inside an expression (binds
    // against the FROM relation, which this frontend does not model — unresolved
    // is NOT an error). When `hardParents` is non-empty, an unresolved reference
    // emits S_UndeclaredIdentifier ONLY when its parent node's rule is in the
    // list; elsewhere it stays soft (sym 0, name recoverable from provenance).
    // Empty (the default) ⇒ hard everywhere (c-subset / toy lexical resolution).
    // A resolvable name always binds regardless of position.
    std::vector<RuleId>      hardParents;
    std::vector<std::string> hardParentNames;   // source names, for diagnostics
    // C 6.2.3 tag namespace: when set, a USE of this reference rule resolves
    // against the TAG namespace (`struct Foo` / `union Foo` / `enum Foo` —
    // the tag identifier), NOT the ordinary-identifier namespace. The matching
    // composite TAG BIND is namespace-routed by the existing `fieldChildren`
    // gate (a declaration WITH a field-body binds Tag); this flag is the
    // LOOKUP counterpart, so a tag reference and a same-named ordinary symbol
    // (`typedef struct Pair {…} Pair;`) resolve independently. Default false —
    // every reference rule resolves Ordinary unless a language opts a
    // tag-reference rule in. Engine-generic: WHICH rule is a tag reference is
    // per-language config (c-subset's structTypeRef/unionTypeRef/enumTypeRef),
    // never a hardcoded keyword.
    bool isTagReference = false;
    // D-CSUBSET-FORWARD-STRUCT-DECLARATION (c35): the composite kind this tag
    // reference names — read ONLY when `isTagReference` is true. When a tag
    // reference MISSES (the tag was never bound) and this kind is Struct/Union,
    // the resolver FORWARD-MINTS an INCOMPLETE composite (`forwardComposite`) and
    // binds it into the Tag namespace, so an opaque handle (`struct S *` whose S
    // is never defined — the sqlite3_stmt/sqlite3_blob pattern) resolves to a
    // sizeable `Ptr<incomplete>` instead of failing S_UnknownType. A VALUE /
    // by-value member / sizeof of the incomplete type still fails loud through
    // the unchanged computeLayout incomplete guard. Enum is value-typed (an
    // opaque enum has no representation), so an Enum tag-miss keeps the
    // fail-loud path. Default Struct; the loader only honours it on tag rows.
    CompositeKind compositeKind = CompositeKind::Struct;
};

// Source built-in type name → lattice type mapping. Used during
// type-position resolution. e.g. `int` in c-subset's typeRef → I32.
//
// A mapping resolves to EITHER a core primitive (`core`, the common case)
// OR a registered type-extension (`extension`, naming a `typeExtensions[]`
// entry by its language-qualified name, e.g. "TSQL::Varchar"). The
// extension form lets a language whose source type name has no core-lattice
// equivalent (e.g. T-SQL's VARCHAR, which is the parameterized
// `TSQL::Varchar` extension, not any core string kind) still resolve in
// type position so the column does not spuriously emit S_UnknownType.
// `extension` is mutually exclusive with `core`; the loader rejects both
// or neither for an extension-named mapping.
struct DSS_EXPORT BuiltinTypeMapping {
    std::string                name;       // user-visible name in source (e.g. "int")
    TypeKind                   core = TypeKind::Void;
    std::optional<std::string> extension;  // language-qualified extension name, when set
    // FC3 c1 (D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH): per-data-model
    // core override. `core` is the BASE mapping; a key for the ACTIVE
    // format's `DataModel` replaces it (c-subset: long → I64, LLP64 → I32).
    // Loader rejects unknown data-model keys + non-core values; mutually
    // exclusive with `extension` (an extension type has no width to vary).
    std::unordered_map<DataModel, TypeKind> coreByDataModel;
    // D-LANG-TYPE-IDENTITY-VOCABULARY: the vocabulary identity tag this name
    // interns under (empty = the anonymous representative of its core). NOT
    // authored in the JSON — the loader DERIVES it from the single-token
    // `typeSpecifiers` row that resolves the same keyword, so the text-keyed and
    // keyword-multiset paths can never disagree about whether `long` is a
    // distinct named type.
    std::string vocabularyName;
    // The mapping's effective core under the active data model.
    [[nodiscard]] TypeKind resolveCore(DataModel dm) const {
        if (auto it = coreByDataModel.find(dm); it != coreByDataModel.end()) {
            return it->second;
        }
        return core;
    }
};

// ── FC3 c1: type-specifier multiset table (`semantics.typeSpecifiers`) ──
//
// C 6.7.2 declares type specifiers as an order-free MULTISET: `unsigned
// long long int` ≡ `long long unsigned int` ≡ `long unsigned long`. A
// language whose grammar produces a specifier-keyword run in type
// position (c-subset's `typeSpecifierSeq`) declares here which multisets
// are valid and what core type each resolves to. The ENGINE collects the
// run's keyword token KINDS, canonicalizes (sorts) them, and looks the
// multiset up — an undeclared combination (`unsigned float`, `short
// long`) fails loud with S_InvalidTypeSpecifierCombination, by ABSENCE
// from the table, never by a hardcoded legality matrix.
//
// `tokens` is the loader-SORTED token-kind multiset key (sorted by
// SchemaTokenId.v; duplicates legal — C's `long long` is the LongKeyword
// kind twice). `coreByDataModel` mirrors BuiltinTypeMapping's override
// (LLP64 long → I32). The loader rejects: duplicate multisets across
// rows, unknown token-kind names, unknown core names, unknown data-model
// keys, and empty token lists.
// D-LANG-TYPE-IDENTITY-VOCABULARY: two ORTHOGONAL axes. This table is the
// LANGUAGE axis — it declares the type VOCABULARY as NAMED entries. The
// TARGET axis (`core` / `coreByDataModel` / `coreByLongDoubleFormat`) declares
// each entry's REPRESENTATION. `name` is what the engine interns identity on,
// so `long` stays a different type from `int` even where a data model gives
// them the same core, and `long double` stays different from `double` on an
// f64 axis. IDENTITY IS NEVER DERIVED FROM REPRESENTATION.
//
// Declare `name` ONLY on rows whose type can COLLIDE with another named entry
// under some target axis. `int`/`short`/`unsigned int`/`unsigned short`/
// `float`/`double`/`bool`/`void`/plain `char` deliberately stay UNNAMED: they
// must remain the ANONYMOUS representative of their core, because integer
// promotion and enum-underlying synthesis independently re-mint anonymous
// primitives of those kinds — naming `int` would make a promoted `char + char`
// stop matching a declared `int`.
struct DSS_EXPORT TypeSpecifierRule {
    std::vector<SchemaTokenId> tokens;       // SORTED multiset key
    std::vector<std::string>   tokenNames;   // source spellings, for diagnostics
    // The vocabulary identity tag; EMPTY (the default) = today's anonymous
    // behavior, identity == the core alone. Every row sharing a name must
    // resolve to the SAME representation on every axis (loader-enforced), so a
    // name can never mean two widths.
    std::string                name;
    // C 6.3.1.1 conversion RANK of this vocabulary entry (`long long` > `long`
    // > `int`; `long double` > `double` > `float`). Rank is defined by the type
    // NAME, not its width — with a width-derived rank `someInt + someLong` on
    // LLP64 (both I32) yields the wrong NAME, observable through `_Generic`.
    // 0 = undeclared, which is also the rank of every anonymous primitive; a
    // named entry therefore always out-ranks the anonymous representative of
    // its own kind. Only meaningful with a `name` (loader rejects rank alone).
    // Used ONLY as the tie-break between two operands of the SAME kind.
    int                        rank = 0;
    TypeKind                   core = TypeKind::Void;
    std::unordered_map<DataModel, TypeKind> coreByDataModel;
    // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): the per-longDoubleFormat override —
    // `{"x87-80": "F80", "ieee128": "F128"}` on the `long double` row (the
    // f64 axis takes the BASE core, the LLP64 `long`≡`int` collapse
    // precedent). Closed keys (longDoubleFormatFromName) + closed values
    // (coreTypeFromName) at load, mirroring coreByDataModel.
    std::unordered_map<LongDoubleFormat, TypeKind> coreByLongDoubleFormat;
    // C99 _Complex (D-CSUBSET-COMPLEX §6.2.5): when true, the resolved (data-model +
    // long-double-axis-aware) `core` is the ELEMENT float type, and the specifier-
    // resolution site wraps it in `interner.complex(interner.primitive(element))`
    // instead of `interner.primitive(element)`. So `double _Complex`→complex(F64),
    // `long double _Complex`→complex(F80/F128/F64) — the element rides the SAME
    // resolveCore axis machinery for free. `false` = an ordinary scalar specifier
    // (every pre-existing row, byte-identical). A non-float `core` under `complex`
    // is a config bug (the loader could validate; the interner would just wrap it).
    bool complex = false;
    [[nodiscard]] TypeKind resolveCore(DataModel dm) const {
        if (auto it = coreByDataModel.find(dm); it != coreByDataModel.end()) {
            return it->second;
        }
        return core;
    }
    // FC17.9(e): the axis-aware resolver. nullopt ⇔ the row DEPENDS on the
    // long-double axis (`coreByLongDoubleFormat` non-empty) but the active
    // format declared none (`None`) — the row is UNREALIZED; the caller
    // fails loud (S_LongDoubleFormatUndeclared). NEVER falls back to the
    // base core in that case — the base is the F64-axis meaning, and
    // silently binding it under an undeclared axis is the exact
    // representation mis-bind this axis exists to prevent (the `long`
    // lesson). A declared axis MISSING from the map (f64) takes the base
    // core; a row with no map resolves exactly as resolveCore(dm).
    [[nodiscard]] std::optional<TypeKind>
    resolveCore(DataModel dm, LongDoubleFormat ldf) const {
        if (!coreByLongDoubleFormat.empty()) {
            if (auto it = coreByLongDoubleFormat.find(ldf);
                it != coreByLongDoubleFormat.end()) {
                return it->second;
            }
            if (ldf == LongDoubleFormat::None) return std::nullopt;
        }
        return resolveCore(dm);
    }
};

// ── FC3 c1: a LOAD-RESOLVED dataModel-aware type-name reference ──
//
// Several config blocks (`integerLiteralTyping` candidates,
// `arithmeticConversions.integerPromotion`) name types by their SOURCE
// spelling ("int", "unsigned long"). The loader resolves each name ONCE
// at load time — through the `typeSpecifiers` table (splitting the name
// on spaces and mapping each word through the schema's keyword table to
// its token kind) with `builtinTypes` as the single-word text fallback —
// and stores the resolved (core, coreByDataModel) pair here. An
// unresolvable name is a LOAD reject (C_InvalidSemantics), so runtime
// consumers only ever apply the trivial data-model select.
struct DSS_EXPORT DataModelTypeRef {
    std::string name;                        // source spelling, for diagnostics
    TypeKind    core = TypeKind::Void;
    // D-LANG-TYPE-IDENTITY-VOCABULARY: the resolved row's vocabulary tag,
    // copied at load. WITHOUT it a literal ladder candidate would mint the
    // ANONYMOUS primitive of the core and silently type `20L` as `int`-identity
    // on LP64 (same I64 core as `long`) — the knob-that-lies again, one tier
    // down. Empty = the candidate resolves to an anonymous primitive.
    std::string vocabularyName;
    std::unordered_map<DataModel, TypeKind> coreByDataModel;
    // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): copied from the resolved
    // typeSpecifiers row at load (the "long double" float-literal rule) —
    // WITHOUT this copy the literal row would silently drop the axis map
    // and type every `20.0L` at the base core (the knob-that-lies).
    std::unordered_map<LongDoubleFormat, TypeKind> coreByLongDoubleFormat;
    [[nodiscard]] TypeKind resolveCore(DataModel dm) const {
        if (auto it = coreByDataModel.find(dm); it != coreByDataModel.end()) {
            return it->second;
        }
        return core;
    }
    // FC17.9(e): the axis-aware resolver — nullopt ⇔ axis-dependent ref
    // under an undeclared (None) axis; see TypeSpecifierRule::resolveCore.
    [[nodiscard]] std::optional<TypeKind>
    resolveCore(DataModel dm, LongDoubleFormat ldf) const {
        if (!coreByLongDoubleFormat.empty()) {
            if (auto it = coreByLongDoubleFormat.find(ldf);
                it != coreByLongDoubleFormat.end()) {
                return it->second;
            }
            if (ldf == LongDoubleFormat::None) return std::nullopt;
        }
        return resolveCore(dm);
    }
};

// ── D-LANG-TYPE-IDENTITY-VOCABULARY: an ENGINE-SYNTHESIZED standard type ──
//
// A handful of types are minted by the ENGINE, not spelled by the source:
// `sizeof`/`_Alignof` yield C's `size_t`, and `p - q` yields `ptrdiff_t`. C
// defines each as an ALIAS of a standard NAMED type — and WHICH name is
// DATA-MODEL-dependent: `size_t` IS `unsigned long` on LP64 and `unsigned long
// long` on LLP64. Before identity was split off representation that did not
// matter (everything 64-bit unsigned was one TypeId); now an ANONYMOUS U64 is a
// THIRD thing matching NEITHER named entry, so `_Generic(sizeof(int), unsigned
// long: 1, unsigned long long: 2, default: 0)` silently takes `default`.
//
// So the ENGINE must not hardcode a core here. Each row maps a DATA MODEL to a
// `typeSpecifiers` VOCABULARY entry, resolved at LOAD through the same
// `DataModelTypeRef` machinery `integerLiteralTyping` uses — the engine never
// sees, compares, or branches on the name's SPELLING, it just carries the
// resolved (core, tag) pair to `TypeInterner::primitive`. Representation still
// comes from the TARGET (the named entry's own `coreByDataModel`), so the two
// axes stay independent.
//
// UNDECLARED (`byDataModel` empty) ⇒ the consumer keeps its historic anonymous
// core, so every language that ships no rows (toy / tsql) is byte-identical. A
// DECLARED role must cover EVERY data model in the closed enum (loader-enforced)
// — a role that silently had no entry for the active target would fall back to
// the anonymous core, i.e. re-introduce the exact defect on that one model.
struct DSS_EXPORT SynthesizedTypeRule {
    std::unordered_map<DataModel, DataModelTypeRef> byDataModel;
    [[nodiscard]] bool declared() const noexcept { return !byDataModel.empty(); }
    // The (core, vocabularyName) this role resolves to under `dm`; nullopt when
    // undeclared. The name is a view into this rule and outlives every call.
    [[nodiscard]] std::optional<std::pair<TypeKind, std::string_view>>
    resolve(DataModel dm) const {
        auto const it = byDataModel.find(dm);
        if (it == byDataModel.end()) return std::nullopt;
        return std::pair<TypeKind, std::string_view>{
            it->second.resolveCore(dm), it->second.vocabularyName};
    }
};

// ── FC3 c1: integer-literal typing ladder (`semantics.integerLiteralTyping`) ──
//
// C 6.4.4.1: an integer constant's type is the FIRST of an ordered
// candidate list (keyed by its suffix and whether it is decimal) whose
// range can represent the value. One rule per suffix GROUP:
//
//   * `suffixes` — the EXACT suffix spellings (as declared in
//     `numberStyle.integerSuffixes`) this rule covers; the EMPTY list is
//     the unsuffixed rule. The engine longest-matches the raw token
//     tail against the numberStyle suffix list (the same match
//     `decodeInteger`'s strip performs), then selects the rule whose
//     `suffixes` contains the matched spelling. The loader cross-checks
//     that EVERY numberStyle suffix appears in exactly one rule (a
//     suffix the lexer admits but the ladder cannot type would be a
//     silent config hole) and that an unsuffixed rule exists.
//   * `decimal` / `nondecimal` — ordered candidate lists for the two
//     radix classes (nondecimal = any literal whose text matched a
//     declared `numberStyle.integerPrefixes` prefix; C gives hex/octal
//     constants the extra unsigned candidates). Names are load-resolved
//     (`DataModelTypeRef`) so LLP64's 32-bit `long` falls out of the
//     data-model select. Loader rejects empty lists + non-integer cores.
//
// A magnitude exceeding the LAST candidate's range fails loud
// (S_IntegerLiteralTooLarge). Languages WITHOUT this block keep the
// `literalTypes` token-kind map exactly (toy / tsql — pinned).
struct DSS_EXPORT IntegerLiteralTypingRule {
    std::vector<std::string>      suffixes;   // exact spellings; empty = unsuffixed
    std::vector<DataModelTypeRef> decimal;
    std::vector<DataModelTypeRef> nondecimal;
    // C23 6.4.4.1 (D-CSUBSET-BITINT-WIDE-LITERAL / Fork-1b): a `wb`/`uwb`
    // bit-precise suffix rule. When true, `decimal`/`nondecimal` are EMPTY (the
    // type is not a fixed core — it is `[unsigned] _BitInt(N)` with N derived from
    // the literal's decoded MAGNITUDE, magnitude-derived at the two typing call
    // sites via `BitIntValue::fromLiteralMagnitude`); `bitPreciseSigned` selects
    // `wb` (signed) vs `uwb` (unsigned). This keeps wb/uwb typing INSIDE the one
    // `integerLiteralTyping` mechanism, so the loader's suffix-coverage cross-check
    // is satisfied natively (a bit-precise rule IS coverage). A schema without any
    // bit-precise rule never mints a `_BitInt` from a literal.
    bool                          bitPrecise       = false;
    bool                          bitPreciseSigned = false;
};

// ── FC3.5 sweep-c2: float-literal typing (`semantics.floatLiteralTyping`) ──
//
// C 6.4.4.2: a floating constant's type is keyed by its SUFFIX alone
// (no magnitude ladder — an unsuffixed constant is `double`, `f`/`F`
// is `float`). One rule per suffix GROUP, mirroring
// `IntegerLiteralTypingRule`'s shape minus the radix/range machinery:
//
//   * `suffixes` — the EXACT spellings (as declared in
//     `numberStyle.floatSuffixes`) this rule covers; the EMPTY list is
//     the unsuffixed rule. The engine longest-matches the raw token
//     tail against the numberStyle float-suffix list, then selects the
//     rule whose `suffixes` contains the matched spelling.
//   * `type` — ONE type name resolved at load through the same
//     typeSpecifiers/builtinTypes path the integer ladder candidates
//     use (a dataModel-aware ref, though C's float widths are
//     model-invariant).
//
// The loader cross-checks mirror the integer ladder's: every
// numberStyle float suffix covered exactly once, exactly one
// unsuffixed rule, no suffix the lexer doesn't admit (dead config),
// and every type resolving to a FLOAT kind under every data model.
// Languages WITHOUT the block keep the `literalTypes` token-kind core
// exactly (toy / tsql — pinned). (D-CSUBSET-F32-CODEGEN closure: this
// block is what flips c-subset's `1.5f` from the interim F64 pin to
// its C-correct F32.)
struct DSS_EXPORT FloatLiteralTypingRule {
    std::vector<std::string> suffixes;  // exact spellings; empty = unsuffixed
    DataModelTypeRef         type;
};

// ── FC3 c1: usual arithmetic conversions (`semantics.arithmeticConversions`) ──
//
// Parameterizes the C 6.3.1.8 binary-operand conversion algorithm the
// HIR lowering applies at every binary / ternary / compound-assign
// combine site (see `usualArithmeticCommonType` in
// `analysis/semantic/type_rules.hpp`). Languages WITHOUT the block keep
// the legacy `TypeInterner::commonType` path EXACTLY (toy/tsql — pinned).
//
//   * `integerPromotion.minRankType` — operands of integer rank BELOW
//     this type's rank promote to it before the conversion (C: `int`).
//   * `integerPromotion.alsoPromote` — type names OUTSIDE the integer
//     rank lattice that join promotion (c-subset: "char", "bool" — C
//     promotes both to int; the engine never hardcodes C's view of
//     char). Resolved per data model like every other name here.
//   * `mixedSignedness` — closed verb for the cross-signedness rule.
//     `rank-prefer-unsigned` (C): unsigned rank ≥ signed rank → the
//     unsigned type; else the signed type (which, at strictly higher
//     width-rank, represents the whole unsigned range). The loader
//     rejects unknown verbs — a typo can never silently no-op.
//   * `promoteComparisons` — when true (C), comparison operands run the
//     same conversion (so `-1 > 0ul` compares as U64); the result stays
//     Bool. When false, comparisons keep their raw operand types.
//   * `shiftResult` — closed verb for the C 6.5.7 shift-result discipline
//     (D-UAC-SHIFT-RESULT-RULE-CONFIG). `promotedLeft` (C): a shift's result
//     is the integer-PROMOTED LEFT operand's type; the right operand never
//     contributes (`i32 << i64` is I32). `commonType`: the shift is typed
//     like an ordinary binary op — both operands run the usual conversions
//     and the result is their common type (`i32 << i64` is I64). The engine
//     reads this verb instead of hardcoding C's special shift rule; the
//     loader rejects an unknown verb. Default `promotedLeft` (a block WITHOUT
//     it keeps C's rule, so existing adopters are byte-identical).
enum class MixedSignednessRule : std::uint8_t {
    RankPreferUnsigned = 1,   // C 6.3.1.8
};

enum class ShiftResultRule : std::uint8_t {
    PromotedLeft = 1,   // C 6.5.7 — result = the promoted LEFT operand
    CommonType   = 2,   // symmetric — result = the usual-arithmetic common type
};

struct DSS_EXPORT ArithmeticConversions {
    DataModelTypeRef              minRankType;          // e.g. "int"
    std::vector<DataModelTypeRef> alsoPromote;          // e.g. ["char", "bool"]
    MixedSignednessRule mixedSignedness = MixedSignednessRule::RankPreferUnsigned;
    bool                promoteComparisons = true;
    ShiftResultRule     shiftResult = ShiftResultRule::PromotedLeft;  // C 6.5.7
};

// Type-expression constructors. When a type-position subtree matches
// the named rule, build the lattice type via the named constructor
// over `operandChild` (a visible-child index pointing to the inner
// type expression).
//
// Supported constructors mirror TypeInterner's builders:
//   Pointer, Reference, Nullable, Optional, Slice
// (Array adds a length scalar — defer until SE4 if a language needs it.)
enum class TypeConstructor : std::uint8_t {
    Pointer,
    Reference,
    Nullable,
    Optional,
    Slice,
};

struct DSS_EXPORT TypeShapeRule {
    RuleId          rule{};
    TypeConstructor constructor = TypeConstructor::Pointer;
    std::int32_t    operandChild = 0;
    std::string     ruleName;
};

// A rule that opens a fresh lexical scope. Bundles the resolved RuleId
// with its source-text name (for diagnostics) — mirroring how
// DeclarationRule/ReferenceRule/TypeShapeRule pair rule+ruleName rather
// than carrying parallel vectors.
struct DSS_EXPORT ScopeRule {
    RuleId      rule{};
    std::string ruleName;
};

// A return-statement shape (GAP A). When Pass 2 sees a node with this
// rule, it resolves the nearest enclosing function's result type (via the
// scope→fnResult map the analyzer builds in pass 1.5) and the returned
// expression's type (the visible child at `valueChild`), then checks
// assignability. `valueChild` absent ⇒ this rule is a bare `return;`
// shape (no returned expression). A `return expr;` shape whose value
// child is structurally optional in the grammar is handled by the engine:
// if the child at `valueChild` is the statement terminator (not an
// expression), it is treated as a bare return.
struct DSS_EXPORT ReturnRule {
    RuleId                       rule{};       // the return-statement shape
    std::optional<std::uint32_t> valueChild;   // visible-child index of the returned expr
    std::string                  ruleName;
};

// A break/continue-style control statement (GAP C). When Pass 2 visits a
// node with this rule at loop-context depth 0 (outside any `loopRules`
// subtree), the engine emits S_ControlOutsideLoop. Bundles rule+ruleName
// like ScopeRule.
struct DSS_EXPORT LoopControlRule {
    RuleId      rule{};        // a break/continue statement shape
    std::string ruleName;
};

// FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS, C23 6.7.13): ONE row of the standard-
// attribute semantics TABLE — a set of attribute NAMES (matched dunder-
// normalized against the clause's last-::-segment, so `__deprecated__` ≡
// `deprecated` and `gnu::packed` matches by `packed`) mapped to ONE effect of
// the CLOSED verb set:
//   • SuppressUnused — the declared symbol never warns S_UnusedVariable
//     (`[[maybe_unused]]` / GNU `unused`);
//   • WarnOnUse      — every use-site of the declared symbol warns
//     S_DeprecatedSymbolUsed (`[[deprecated["msg"]]]`);
//   • WarnOnDiscard  — a call to the declared function whose result is
//     discarded as a bare expression statement warns
//     S_NodiscardResultDiscarded (`[[nodiscard]]` / GNU `warn_unused_result`);
//   • None           — KNOWN vocabulary, no effect HERE (either consumed by a
//     dedicated scan — noreturn/packed — or deliberately inert — fallthrough/
//     likely/unlikely/reproducible/unsequenced). Listed so the UNKNOWN-
//     attribute warning never false-fires on a name the language knows.
// A name matching NO row in the C23 `[[...]]` form warns S_UnknownAttribute
// (suppressible — C23 forbids fatal unknown standard attributes). The loader
// validates the effect verb against the closed set (C_InvalidSemantics on an
// unknown verb — a typo can never silently disarm a row).
enum class AttributeEffect : std::uint8_t {
    SuppressUnused,
    WarnOnUse,
    WarnOnDiscard,
    None,
};
struct DSS_EXPORT AttributeSemanticsRow {
    std::vector<std::string> names;                        // dunder-normalized match set
    AttributeEffect          effect = AttributeEffect::None;
};

// Literal token-kind → core TypeKind. Pass 2 reads the token-kind of a
// matched literal leaf and assigns the corresponding lattice type via
// `TypeInterner::primitive(core)`.
struct DSS_EXPORT LiteralTypeMapping {
    SchemaTokenId   literal{};
    TypeKind        core = TypeKind::Void;
    std::string     literalName;   // source-text name retained for diags
    // FC3 c1: KEYWORD literals (C23 `true` / `false`). When set, the HIR
    // lowering uses this value directly instead of decoding the token's
    // TEXT as a number — `decodeInteger("true")` would otherwise silently
    // produce 0 (no leading digits). Declared per-row so any language can
    // map a keyword token to a fixed-value literal of any core (`true` →
    // Bool 1, a future `nil` → Ptr 0, …) with zero engine vocabulary.
    std::optional<std::int64_t> fixedValue;
    // String-literal rows (C 6.4.5): a string literal's type is `Array<core, N+1>`
    // where N is the decoded body length (per-occurrence, so NOT a fixed interned
    // TypeId like the other rows). When `stringArray` is set, `core` is the ELEMENT
    // type (e.g. Char) and the consumer builds the array per occurrence by decoding
    // the token text — it is NOT placed in the fixed `literalTypeIds` map. Zero
    // engine vocabulary: any language declaring a string body token gets it.
    bool stringArray = false;
};

// ── FC4 c1: parameter-list conventions (`semantics.parameters`) ──
//
// C 6.7.6.3p10: a parameter list of exactly `(void)` declares a function
// taking NO parameters. When `soleVoidMeansEmpty` is true, the engine's
// param-harvest chokepoint drops a SOLE, UNNAMED parameter whose resolved
// type is lattice `Void`; a NAMED void parameter, or void mixed with other
// parameters, is ill-formed and emits S_InvalidVoidParam (an ERROR,
// positioned at the param). Default false ⇒ raw param lists (toy / tsql —
// pinned; a void-typed param would then surface through the normal
// invalid-type checks downstream).
struct DSS_EXPORT ParametersConfig {
    bool soleVoidMeansEmpty = false;
};

// The full `semantics` block. Every facet is optional; absent ⇒ that
// facet is just not analyzed.
struct DSS_EXPORT SemanticConfig {
    std::vector<DeclarationRule>    declarations;
    // FC4 c1: the declarator role vocabulary (C 6.7.6) — see
    // DeclaratorConfig above. nullopt ⇒ the language has no recursive
    // declarators; every declarator-mode DeclarationRule row requires it
    // (loader-enforced), so consumers may dereference when a row's
    // `isDeclaratorMode()` is true.
    std::optional<DeclaratorConfig> declarators;
    std::vector<ReferenceRule>      references;
    // D5.1: member-access expression rules (`obj.field` and `ptr->field`). Pass 2
    // resolves each to its field's SymbolId + type. Empty ⇒ the language has no
    // member-access surface (toy / tsql currently).
    std::vector<MemberAccessRule>   memberAccesses;
    std::vector<ScopeRule>          scopes;        // rules that open a fresh lexical scope
    std::vector<BuiltinTypeMapping> builtinTypes;
    // FC3 c1: type-specifier keyword-multiset table (C 6.7.2 `unsigned
    // long long int` ≡ `long long unsigned int`). Empty ⇒ the language
    // has no specifier-run type syntax (toy / tsql) and type-position
    // resolution is untouched. See TypeSpecifierRule above.
    std::vector<TypeSpecifierRule>  typeSpecifiers;
    std::vector<TypeShapeRule>      typeShapes;
    std::vector<LiteralTypeMapping> literalTypes;
    // FC3 c1: the integer-literal typing ladder (C 6.4.4.1). Empty ⇒ the
    // `literalTypes` token-kind map types integer literals exactly as
    // before (toy / tsql — pinned). See IntegerLiteralTypingRule above.
    std::vector<IntegerLiteralTypingRule> integerLiteralTyping;
    // FC3.5 sweep-c2: float-literal suffix typing (C 6.4.4.2). Empty ⇒
    // the `literalTypes` token-kind core types float literals exactly
    // as before (toy / tsql — pinned). See FloatLiteralTypingRule.
    std::vector<FloatLiteralTypingRule>   floatLiteralTyping;
    // FC3 c1: usual-arithmetic-conversions parameter block (C 6.3.1.8).
    // nullopt ⇒ the legacy `TypeInterner::commonType` behavior at every
    // HIR combine site (toy / tsql — pinned). See ArithmeticConversions.
    std::optional<ArithmeticConversions>  arithmeticConversions;
    // FC4 c1: parameter-list conventions (C's `(void)` = zero params).
    // Default-constructed (all false) for languages without the block.
    ParametersConfig                parameters;
    std::vector<AssignmentRule>     assignments;       // SE4 const-correctness
    std::vector<CallRule>           callRules;         // SE6 call checking
    std::vector<CastRule>           castRules;         // FC2 explicit casts
    // FC6: `sizeof` typing. `sizeofTypeRule` = the `sizeof ( type-name )` form;
    // pass 2 resolves + stamps its `sizeofTypeChild` castTypeRef child (so the HIR
    // lowering recovers the SIZED type) and stamps the node size_t. `sizeofValueRule`
    // = the `sizeof unary-expression` form; the node is stamped size_t (the operand
    // is typed normally — its type is the sized type). Both invalid ⇒ no `sizeof`.
    RuleId        sizeofTypeRule{};   std::string sizeofTypeRuleName;
    std::uint32_t sizeofTypeChild = 0;
    RuleId        sizeofValueRule{};  std::string sizeofValueRuleName;
    // C11/C23 6.5.3.4: `_Alignof`/`alignof` typing. `alignofTypeRule` = the
    // `_Alignof ( type-name )` form (TYPE-NAME ONLY — no value form, unlike
    // sizeof); pass 2 resolves + stamps its `alignofTypeChild` castTypeRef child
    // (so the HIR lowering recovers the type whose ALIGNMENT is read) and stamps
    // the node size_t (U64). Invalid ⇒ the language has no `_Alignof` surface.
    RuleId        alignofTypeRule{};  std::string alignofTypeRuleName;
    std::uint32_t alignofTypeChild = 0;
    // D-LANG-TYPE-IDENTITY-VOCABULARY (`semantics.synthesizedTypes`): the
    // VOCABULARY ENTRY each engine-synthesized standard type resolves to, per
    // data model. `sizeof`/`_Alignof` yield C's `size_t`; a same-pointee
    // `p - q` yields `ptrdiff_t`. Undeclared ⇒ the historic anonymous core
    // (U64 / I64) — see `SynthesizedTypeRule`.
    SynthesizedTypeRule sizeofResultType;
    SynthesizedTypeRule alignofResultType;
    SynthesizedTypeRule pointerDifferenceType;
    // C23 6.7.2.5 (D-CSUBSET-TYPEOF): `typeof`/`typeof_unqual` typing. Both are
    // TYPE-SPECIFIERS resolving to the operand's type. `typeofTypeRule` = the
    // TYPE-NAME operand form (`typeof ( type-name )`, whose `typeofOperandChild`
    // is a castTypeRef — resolved through the SAME type resolver casts/sizeof use);
    // `typeofValueRule` = the EXPRESSION operand form (`typeof ( expression )`,
    // whose operand type is read via subtreeType — UNEVALUATED). BOTH forms share
    // the operand visible-child index `typeofOperandChild` (2 — the inline
    // keyword-alt matches ONE token at child 0, same layout as alignof/sizeof).
    // `typeofStripQualifiersToken` is the OPTIONAL leading-keyword token that means
    // "strip top-level qualifiers" (`typeof_unqual`): when the typeof node's child-0
    // keyword IS this token, the resolved type has its top-level VolatileQual
    // stripped (only volatile is interned; const/restrict are not). Absent (nullopt)
    // ⇒ the language's typeof never strips. Both rules invalid ⇒ no typeof surface.
    // The resolveTypeNodeImpl arm ALSO makes the typeof operand subtree opaque to the
    // coarse volatile/const qualifier scans so a stripped qualifier is never silently
    // re-applied. Source-AGNOSTIC: nothing hardcodes "typeof"/"typeof_unqual".
    RuleId        typeofTypeRule{};   std::string typeofTypeRuleName;
    RuleId        typeofValueRule{};  std::string typeofValueRuleName;
    std::uint32_t typeofOperandChild = 0;
    std::optional<SchemaTokenId> typeofStripQualifiersToken;
    // C23 6.2.5/6.7.2 (D-CSUBSET-BITINT): the `_BitInt(N)` bit-precise integer
    // type-specifier. `bitIntSpecRule` = the `bitIntSpecifier` shape = [ keyword,
    // '(', const-expr, ')' ]; `bitIntWidthChild` = the visible-child index of the
    // width constant-expression (2). `bitIntUnsignedToken`/`bitIntSignedToken` name
    // the C 6.7.2 signedness keywords the resolveTypeNodeImpl bitInt arm scans for
    // among a specifier RUN's sibling tokens (a `_BitInt` inside a `typeSpecifierSeq`
    // composes with `unsigned`/`signed`, order-independently; DEFAULT signed when
    // neither is present). The arm const-folds + validates N (the S_BitIntWidthNot*
    // width gates 0xE04A–0xE04D) and interns `bitInt(N, signed)` for ANY valid width —
    // N>64 is a runnable multi-limb type (the C1 `S_BitIntWidthAboveC1Limit` N>64 gate
    // is RETIRED in C2). Invalid `bitIntSpecRule` ⇒ the language has no _BitInt
    // surface (the arm never fires). Source-AGNOSTIC: nothing hardcodes "_BitInt".
    RuleId        bitIntSpecRule{};   std::string bitIntSpecRuleName;
    std::uint32_t bitIntWidthChild = 0;
    std::optional<SchemaTokenId> bitIntUnsignedToken;
    std::optional<SchemaTokenId> bitIntSignedToken;
    // C11/C23 6.7.5 (D-CSUBSET-ALIGNAS): the `_Alignas`/`alignas` alignment
    // SPECIFIER. `alignasSpecRule` = the `alignasSpec` shape = [ keyword, '(',
    // alignasArg, ')' ]; `alignasArgChild` = the visible-child index of the
    // `alignasArg` operand (2). `alignasArgTypeRule` = the `alignasTypeName` rule id
    // (the guarded TYPE-form WRAPPER branch of the `alignasArg` alt, whose sole
    // child is the `castTypeRef` — the wrapper exists so `commitRequiresTypeName`
    // sits on the probed branch): the semantic tier reads alignasArg's committed
    // reading and discriminates the TYPE form (committed child's rule == this ⇒
    // alignment = _Alignof(T) via computeLayout(...)->align of the castTypeRef
    // inside the wrapper) from the VALUE form (else ⇒ const-evaluate the constant-
    // expression via the SAME `constIntExpr` sizeof/static_assert/array-dims use).
    // The result is validated (power-of-two / ≤256 / ≥ natural align / context /
    // constant) and STORED on `SymbolRecord.explicitAlignment` (variable) or fed
    // into the composite's `fieldAligns` (member). Invalid `alignasSpecRule` ⇒ the
    // language has no `alignas` surface (the scan never runs).
    RuleId        alignasSpecRule{};    std::string alignasSpecRuleName;
    std::uint32_t alignasArgChild = 0;
    RuleId        alignasArgTypeRule{}; std::string alignasArgTypeRuleName;
    // FC16 (D-CSUBSET-PACKED): the composite type-attribute list rule
    // (`compositeAttrList` = repeated `compositeAttr`, a trailing
    // `__attribute__((...))` / `[[...]]` after a struct/union body) + the recognized
    // `packed` attribute-name set. The semantic tier scans a structSpec/unionSpec node
    // for `compositeAttrListRule` children, extracts each attribute identifier,
    // dunder-normalizes it (`__packed__` ≡ `packed`, via the shared `stripDunder`),
    // and marks the composite `packed` when the name is in `packedAttributeNames`; an
    // UNRECOGNIZED `__attribute__` identifier fails loud (S_UnknownTypeAttribute).
    // Invalid `compositeAttrListRule` ⇒ the language has no composite-attribute surface
    // (the scan never runs — toy/tsql). Source-AGNOSTIC: WHICH rule + WHICH names are
    // both per-language config; the engine never hardcodes the spelling "packed".
    RuleId                   compositeAttrListRule{};
    std::string              compositeAttrListRuleName;
    std::vector<std::string> packedAttributeNames;
    // FC16 (D-CSUBSET-PACKED): the STRICT composite-attribute rule (the GNU
    // `__attribute__((...))` form, `attrSpec`). An UNRECOGNIZED attribute in a strict-
    // form node fails loud (S_UnknownTypeAttribute — typo protection, like
    // `H_UnknownLinkageSpecifier`); an unrecognized attribute in the NON-strict form
    // (C23 `[[...]]`, `stdAttr`) is standard-ignorable (the `[[deprecated]]`
    // precedent). Invalid ⇒ no strict form (every unrecognized attribute ignorable).
    RuleId                   compositeStrictAttrRule{};
    std::string              compositeStrictAttrRuleName;
    // FC16 (D-CSUBSET-NORETURN): the C11/C23 `noreturn` FUNCTION attribute
    // vocabulary. `noreturnKeywordToken` is the `_Noreturn` KEYWORD token (C11
    // 6.7.4); `noreturnAttributeNames` is the recognized ATTRIBUTE-identifier set
    // (`noreturn` — C23 6.7.12.7 `[[noreturn]]` / GNU `__attribute__((noreturn))`
    // / `[[gnu::noreturn]]`, dunder-normalized at the scan so `__noreturn__`
    // matches). The semantic tier scans a function declaration's SPECIFIER PREFIX
    // for EITHER form (`specifierPrefixNamesNoreturn`) and marks the function
    // symbol `isNoreturn`; the HIR lowering then wraps a direct call to such a
    // function as `Block{ ExprStmt(call), Unreachable }` so a noreturn-terminated
    // path structurally terminates (the `wrapIfProvablyInfinite` precedent).
    // Both invalid/empty ⇒ the language has no `noreturn` surface (the scan never
    // runs — toy/tsql). Source-AGNOSTIC: WHICH token + WHICH names are per-language
    // config; the engine never hardcodes the spelling "noreturn".
    std::optional<SchemaTokenId> noreturnKeywordToken;
    std::vector<std::string>     noreturnAttributeNames;
    // FC17 (D-CSUBSET-CONSTEXPR): the C23 6.7.1 `constexpr` OBJECT storage-class
    // KEYWORD token. Pass 1 scans a declaration's specifier prefix for it
    // (`specifierPrefixHasConstexpr`, the `specifierPrefixNamesNoreturn` mirror)
    // and marks each declared symbol `isConstexpr` (implies `isConst`); Pass 2's
    // `validateConstexprDeclarator` then enforces the 6.7.1 constraints AT THE
    // DECLARATION (compile-time-constant initializer / no missing initializer /
    // no function declarator / no volatile-qualified object / aggregate types a
    // named loud deferral). Unset ⇒ the language has no `constexpr` surface (the
    // scan never runs — toy/tsql). Source-AGNOSTIC: WHICH token is per-language
    // config; the engine never hardcodes the spelling "constexpr". Linkage is a
    // SEPARATE, ALSO config-driven axis: the C23 6.2.2p3 file-scope INTERNAL
    // linkage rides the declaration row's `linkageSpecifiers` map (keyword text →
    // {binding:local}), not this token.
    std::optional<SchemaTokenId> constexprKeywordToken;
    // TLS C1 (D-CSUBSET-THREAD-LOCAL): storage-class specifier TOKENS that may
    // NOT be combined with a thread-storage specifier in one declaration
    // (C11/C23 6.7.1p2 admits only `static`/`extern` beside `thread_local`;
    // c-subset lists `RegisterKeyword` — `register` parses as an inert
    // storage-class specifier, so `register thread_local int x;` must reject
    // rather than silently drop one specifier). Pass 2's
    // `validateThreadLocalDeclarator` scans the declaration's specifier
    // prefix for these kinds on a thread-local-marked symbol →
    // S_ThreadLocalInvalidCombination. EMPTY ⇒ no forbidden-combination scan
    // (a language whose grammar already excludes the pairings). NOTE the
    // thread-storage vocabulary itself is NOT declared here: which tokens
    // confer thread storage rides the declaration rows' `linkageSpecifiers`
    // map ({"threadStorage": true}) — one facet, one scan, per-language.
    // Source-AGNOSTIC: WHICH kinds are per-language config; the engine never
    // hardcodes the spelling "register".
    std::vector<SchemaTokenId>   threadLocalIncompatibleTokens;
    // FC17.5 (D-CSUBSET-FUNC-PREDEFINED-IDENTIFIER): the C99 6.4.2.2 predefined
    // function-name identifier spellings (`__func__` + the GNU `__FUNCTION__`
    // alias for c-subset). Pass 1 binds, for EACH spelling, one synthetic
    // SymbolRecord into a function DEFINITION's own (param/body) scope BEFORE
    // the params are visited: kind=Variable, isConst=true (SE4's const check
    // then catches `__func__ = x` / `+=` → S_ConstViolation),
    // type=Array<narrow-string-core, len+1> (the element core is the language's
    // config-declared string-literal core — the SAME source string literals
    // type from — never a hardcoded Char), isPredefinedFunctionName=true +
    // predefinedFunctionNameText=the function's name. HIR lowering FOLDS a read
    // of such a symbol to a string-literal-shaped constant (byte-identical to a
    // real string literal, so rodata/decay/indexing ride unchanged). EMPTY ⇒
    // the language has no predefined function-name surface (the bind never
    // runs — toy/tsql). Source-AGNOSTIC: WHICH spellings are per-language
    // config; the engine never hardcodes "__func__".
    std::vector<std::string>     predefinedFunctionNameIdentifiers;
    // FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS, C23 6.7.13): the standard-attribute
    // semantics table (see AttributeSemanticsRow). `attrSpecRule`/`stdAttrRule`
    // name the GNU `__attribute__((...))` / C23 `[[...]]` attribute-specifier
    // shapes — the scan's bounded descent STOPS AT each such node and extracts
    // its clause(s) (an attrSpec is ONE clause; a stdAttr's Internal children
    // are one clause each). `attrBareStatementRule` is the bare attribute-
    // declaration STATEMENT (`[[fallthrough]];`) — pass2Post runs the scan on
    // it for the unknown-name warning only (no symbol). `attributeEffects` is
    // the name→effect table. All invalid/empty ⇒ the language has no standard-
    // attribute semantics (the scan never runs — toy/tsql). Source-AGNOSTIC:
    // WHICH rules + WHICH names are per-language config; the engine walks the
    // closed AttributeEffect verb set only.
    RuleId attrSpecRule{};          std::string attrSpecRuleName;
    RuleId stdAttrRule{};           std::string stdAttrRuleName;
    RuleId attrBareStatementRule{}; std::string attrBareStatementRuleName;
    std::vector<AttributeSemanticsRow> attributeEffects;
    // FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS): the nodiscard DISCARD-CONTEXT rule
    // ids (the `semantics.nodiscard` block). A WarnOnDiscard-flagged call's
    // result counts as discarded iff the call node's PARENT is
    // `nodiscardExpressionRule` AND its GRANDPARENT is
    // `nodiscardDiscardStatementRule` — the TWO-hop shape (design-audit F1: the
    // expression-engine materializes an `expression` node between the call and
    // the expression statement, so a one-hop parent==exprStmt check would
    // NEVER fire). The two-hop-exact match makes `(void)f();` (castExpr
    // interposes) and `x=f()`/`g(f())`/`return f()` no-fire by construction.
    // Either invalid ⇒ WarnOnDiscard rows never fire.
    RuleId nodiscardDiscardStatementRule{}; std::string nodiscardDiscardStatementRuleName;
    RuleId nodiscardExpressionRule{};       std::string nodiscardExpressionRuleName;
    // FC12a-core (D-FC12A-VARIADIC-CALLEE): variadic-intrinsic typing. `vaArgRule`
    // = the `va_arg(ap,T)` form; pass 2 resolves+stamps its `vaArgTypeChild`
    // castTypeRef (so the HIR lowering recovers the read type T) + stamps the node
    // T, and type-checks the `vaArgApChild` operand is a va_list. `vaStartRule`/
    // `vaEndRule` stamp the node `void`, type-check their `va*ApChild` operand is a
    // va_list, and `vaStartRule` additionally flips the enclosing function's
    // uses-va-start attribute (the LIR prologue spills the arg regs only for such
    // functions). All invalid ⇒ the language has no variadic-intrinsic surface.
    RuleId        vaArgRule{};        std::string vaArgRuleName;
    std::uint32_t vaArgApChild   = 0;
    std::uint32_t vaArgTypeChild = 0;
    RuleId        vaStartRule{};      std::string vaStartRuleName;
    std::uint32_t vaStartApChild = 0;
    RuleId        vaEndRule{};        std::string vaEndRuleName;
    std::uint32_t vaEndApChild   = 0;
    // C11/C23 6.7.10 (D-CSUBSET-STATIC-ASSERT): the `_Static_assert`/`static_assert`
    // static-assertion DECLARATION rule. When Pass 2 visits a node of this rule it
    // const-evaluates the FIRST meaningful child (the condition — the `assignmentExpr`
    // after the keyword + `(`) via the SAME `constIntExpr` evaluator that folds
    // sizeof(T)/enum/arithmetic in an array dimension: a fold to ZERO emits
    // S_StaticAssertFailed (message = the OPTIONAL trailing string-literal child); a
    // condition that does not fold to an integer constant expression (non-const /
    // float / unresolved) ALSO emits S_StaticAssertFailed (C requires an ICE); a
    // NONZERO fold produces nothing. The construct itself lowers to nothing (its
    // hirLowering row maps to Skip). Invalid ⇒ the language has no static-assertion
    // surface (toy/tsql — the check never runs).
    RuleId        staticAssertRule{}; std::string staticAssertRuleName;
    // FC17.9(i) (D-CSUBSET-INLINE-ASM, C23 6.8 / GNU 6.47): the `__asm__` inline-asm
    // STATEMENT rule (asmStmt). Pass 2 decodes the template child (the SAME
    // decodeAdjacentStringBodies chokepoint staticAssert's message uses) and REQUIRES
    // a strictly-empty decoded string; a non-empty / whitespace-only / malformed-escape
    // template fails loud S_InlineAsmNonEmptyTemplate (real per-target asm text is the
    // D-CSUBSET-INLINE-ASM-TEXT deferral). The empty form lowers to a MirOpcode::
    // CompilerBarrier fence (hirLowering asmStmt → InlineAsm). Invalid ⇒ the language
    // has no inline-asm surface (toy/tsql — the check never runs).
    RuleId        inlineAsmRule{}; std::string inlineAsmRuleName;
    // FC16 C11/C23 6.5.1.1 (D-CSUBSET-GENERIC-SELECTION): `_Generic` generic
    // selection. `genericRule` = the `genericExpr` shape; `genericControlChild` =
    // the visible-child index of the controlling `assignmentExpr`. When Pass 2
    // visits a node of `genericRule` it (1) reads the controlling expression's
    // resolved type and lvalue-converts it (strips a top-level VolatileQual, the
    // `isAssignable` precedent); (2) walks the node's children, and for each
    // `genericTypedAssocRule` child resolves its FIRST child (a `castTypeRef`)
    // through the SAME type resolver casts/sizeof/va_arg use — a VALUE in type
    // position fails loud (S_UnknownType) — and its LAST child is the result
    // expression; a `genericDefaultAssocRule` child is the `default` fallback;
    // (3) matches the controlling type against each typed assoc's resolved type
    // for COMPATIBILITY (interned TypeId equality via `sameType`), requiring
    // EXACTLY ONE typed match (>1 ⇒ S_GenericSelectionAmbiguous) OR the default
    // (none-and-no-default ⇒ S_GenericSelectionNoMatch); (4) stamps the
    // genericExpr node with the winner's RESULT type and records the winning
    // association's result-expression NodeId (the `nodeToSelectedExpr` side-table)
    // so the HIR lowering lowers ONLY that sub-expression. `genericAssocRule` is
    // the umbrella alt (genericTypedAssoc | genericDefaultAssoc) — retained for
    // loader validation. All invalid ⇒ the language has no generic-selection
    // surface (toy/tsql — the check never runs).
    RuleId        genericRule{};            std::string genericRuleName;
    std::uint32_t genericControlChild = 0;
    RuleId        genericAssocRule{};       std::string genericAssocRuleName;
    RuleId        genericTypedAssocRule{};  std::string genericTypedAssocRuleName;
    RuleId        genericDefaultAssocRule{}; std::string genericDefaultAssocRuleName;
    // FC3.5 sweep-c3: compound-literal type-position stamping rules
    // (D-CSUBSET-COMPOUND-LITERAL-TYPEDEF). See CompoundLiteralRule.
    std::vector<CompoundLiteralRule> compoundLiteralRules;
    std::vector<BuiltinFunctionMapping> builtinFunctions;  // SE6 builtins
    std::vector<ReturnRule>         returnRules;       // GAP A return-type checking
    // Rules that establish a break/continue-valid context (while/for/do/
    // switch). Bundled rule+ruleName via ScopeRule — same house pattern.
    std::vector<ScopeRule>          loopRules;         // GAP C loop contexts
    std::vector<LoopControlRule>    loopControls;      // GAP C break/continue stmts
    // The token kind whose text is a language identifier (e.g.
    // "Identifier"). Resolved by the loader to a SchemaTokenId; absent
    // (InvalidSchemaToken) when the language declares no identifierToken.
    // The engine reads THIS instead of hardcoding a token name, so a
    // language whose identifier token is named "Word" works unchanged.
    // The loader emits C_MissingField when a `nameMatch: "lastIdentifier"`
    // rule is declared without an identifierToken.
    SchemaTokenId                   identifierToken{};
    // GAP D: an OPTIONAL second token kind whose leaf also counts as a name
    // in LastIdentifier mode — a bracket-quoted identifier opener (tsql's
    // `[Orders]` → `BracketIdStart`). When set, `extractNameNode` /
    // lastIdentifierText accept it in addition to `identifierToken`, reading
    // the bracketed text from the source slice (brackets stripped). Absent
    // (InvalidSchemaToken) for languages with no bracket-id syntax.
    std::optional<SchemaTokenId>    bracketIdentifierToken;
    // SE-pointers (G5): a token whose occurrence in a type-position subtree
    // wraps the resolved type one level in `Ptr<…>` (C's `int *p` / `int **p`
    // declarator stars). The engine counts these tokens within a type node and
    // applies that many `Ptr` constructors — a declarator-DEPTH model. Absent
    // (InvalidSchemaToken) for languages with no pointer declarator. Full C
    // declarators (function pointers, arrays-of-pointers) stay future surface.
    std::optional<SchemaTokenId>    pointerToken;
    // c27 (D-CSUBSET-VOLATILE-POINTEE): the language's `volatile`-class qualifier
    // token (c-subset: `VolatileKeyword`). Used by the type-position resolver's
    // CO-LOCATED arm (`typeRefAllowingStruct` / `castTypeRef`, where the qualifier +
    // stars are siblings of ONE node, AND the split-form head `volatile <base>`) to
    // BUILD a VolatileQual: a `volatileMarker` token BEFORE the first `pointerToken`
    // qualifies the base (the innermost pointee) ⇒ wrap base in VolatileQual so
    // `volatile int *` = Ptr<VolatileQual(int)>; AFTER the last star (east) is the
    // POINTER OBJECT's volatile, threaded by the declarator's pointer-layer loop as
    // VolatileQual(Ptr<...>). The former pointee-volatile REJECT is retired (volatile
    // is now a type qualifier). Absent (InvalidSchemaToken) ⇒ the language has no
    // volatile qualifier. Source-agnostic: the engine reads THIS, never a hardcoded
    // token name.
    std::optional<SchemaTokenId>    volatileMarker;
    // FC17.9(d) cycle 1b (D-CSUBSET-ATOMIC): the language's `_Atomic`-class qualifier
    // token (c-subset: `AtomicKeyword`). The LIVE driver of the atomic type-qualifier
    // wrap, EXACTLY parallel to `volatileMarker` above and read at the SAME two resolver
    // arms: a `atomicMarker` token BEFORE the first `pointerToken` qualifies the base
    // (innermost pointee) ⇒ wrap base via `atomicQualified` so `_Atomic int *` =
    // Ptr<atomicQualified(int)>; AFTER the last star (east) is the POINTER OBJECT's
    // `_Atomic`, threaded by the declarator's pointer-layer loop as
    // atomicQualified(Ptr<...>). Composes with `volatileMarker` in the ONE shared
    // {volatile,atomic} bitset skin (cycle 1a `qualified` merges bits, order-independent).
    // Absent (nullopt) ⇒ the language has no `_Atomic` qualifier. Source-agnostic: the
    // engine reads THIS, never a hardcoded token name.
    std::optional<SchemaTokenId>    atomicMarker;
    // FF6 Slice 2 + audit fold (2026-06-02): per-object-format
    // runtime library identity for SOURCE-DECLARED externs. The
    // source language's grammar emits a complete `extern int
    // puts(const char*);`-shape signature; the synthesizeFfi
    // path trusts that signature as authoritative and binds the
    // resulting FfiMetadata to this map's per-format entry.
    //
    // Key: `ObjectFormatKind` name as a string ("pe", "elf",
    // "macho", "wasm", "spirv" — matches `objectFormatKindName`).
    // Value: runtime library identity the linker writes into the
    // .idata / .dynamic / etc. import descriptor (e.g.
    // "msvcrt.dll", "libc.so.6", "/usr/lib/libSystem.B.dylib").
    //
    // Empty map ⇒ the language declares no source-side externs
    // OR the active CU has none. When the source DOES declare an
    // extern AND the active object format has no entry in this
    // map, the FFI synthesis call fails loud with
    // `F_FfiNoImportLibraryForFormat` (unsuppressable).
    //
    // Per-LANGUAGE field (NOT per-declaration-rule). The pre-fold
    // 2026-06-02 placement on `DeclarationRule` was fragile: the
    // pipeline iterated declarations and took the first-match
    // result, leaving multi-extern-decl-rule grammars with non-
    // obvious lookup order. Per-language placement matches the
    // semantic invariant — "when compiling THIS LANGUAGE for THIS
    // FORMAT, source-declared externs live in THIS LIBRARY" —
    // and lets the c-subset config retain a single declaration
    // entry without spurious rule-level coupling.
    //
    // The "extern <lib> int foo(...);" per-declaration override
    // is anchored `D-CSUBSET-EXTERN-LIBRARY-SYNTAX` for a future
    // grammar extension that would layer per-symbol overrides on
    // top of this language-level default.
    std::unordered_map<std::string, std::string> externLibraryByFormat;

    // FF11 (2026-06-05): SYSTEM include search path — the per-language
    // analogue of C's /usr/include. Each entry is a subdirectory under
    // `src/dss-config/` (e.g. "shippedLibs/windows-x86_64"); the
    // angle-form `#include <h>` resolves the header name against these
    // dirs (the wiring layer walks up from cwd to find each, mirroring
    // `findShippedConfig`). DISTINCT from the quote form's search
    // (self-dir + includeDirs). The angle name (`<stdio.h>`) resolves to a
    // language-NEUTRAL JSON descriptor `<stem>.json` (D-FFI-SHIPPED-LIB-
    // DESCRIPTOR-AGNOSTIC, v0.0.2 V2-2): its symbols (name + a hir-text
    // type-string signature decoded by `parseTypeFromText`) are injected
    // into semantic scope BEFORE Pass 2 (the `builtinFunctions` seam) so a
    // call resolves with NO inline `extern`, then synthesized as externs
    // flowing through FF5 `synthesizeFfiFromSourceDecls` like a program's
    // own. (Pre-v0.0.2 a header here was a c-subset SOURCE `.h` parsed +
    // tree-merged; that source-tree path is retired for shipped descriptors.)
    // Empty ⇒ the language ships no system headers (the angle form, if
    // declared, resolves nothing and hard-fails on use).
    //
    // Per-language data: a second language ships its own headers under
    // its own dir(s) with ZERO engine change. Platform auto-select
    // (picking windows-x86_64 vs linux-x86_64 from the active target) is
    // DEFERRED — anchored D-FFI-SHIPPED-LIB-PLATFORM-SELECT; for now the
    // single shipped dir names its platform explicitly.
    std::vector<std::string> shippedLibDirs;

    // D-LANG-POINTER-VOID-CONVERT (step 13.2, 2026-06-02): per-language
    // rules governing implicit conversion between `Ptr<Void>` (untyped
    // memory) and `Ptr<T>` (typed memory). The two directions carry
    // DIFFERENT safety characteristics and are configured INDEPENDENTLY
    // — C++ (DSS's self-host target) allows the safe direction
    // (`T* → void*`, widening to untyped) but FORBIDS the unsafe
    // direction (`void* → T*`, asserting untyped memory IS T-typed;
    // requires an explicit cast). A single bool would conflate the two
    // and force a known-future-split when C++ frontend lands.
    //
    // Shipped values:
    //   * c-subset (C semantics): BOTH true (C-standard §6.3.2.3:
    //     `void*` converts implicitly to/from any object-pointer type).
    //   * (Future) c++-subset: `{implicitToVoidPtr: true,
    //     implicitFromVoidPtr: false}` — matching ISO C++ §7.11.
    //   * (Future) rust-like / swift-like: BOTH false (strict typing;
    //     explicit cast required in both directions).
    //   * Default (struct initializer): BOTH false. This is the
    //     SAFETY-RESPECTING default — a new language schema that
    //     doesn't think about pointer conversions gets strict typing
    //     and must explicitly opt into either direction. This default
    //     direction INTENTIONALLY DIFFERS from the Array→Ptr decay
    //     opt-out (which defaults ON) because implicit pointer-
    //     conversion is a type-safety relaxation, not a pervasive
    //     idiom; relaxations should require explicit opt-in.
    //
    // Consumed by:
    //   * `isAssignable` in `type_rules.hpp` — admits the assignment
    //     when the relevant direction's flag is true.
    //   * `coerce()` in `cst_to_hir.cpp` — emits a synthetic `Cast`
    //     HIR node when admitted (mirror of the existing Array→Ptr
    //     decay arm). MIR-tier `mapCast` already lowers Ptr→Ptr as
    //     `Bitcast` (no representation change at runtime).
    //
    // Anchored for future:
    //   * `D-LANG-VOIDPTR-ARITH-REJECT`: pointer arithmetic on void*
    //     is undefined in standard C (sizeof(void) is invalid); GCC
    //     permits it as an extension treating void as 1-byte. When
    //     c-subset gains pointer arithmetic, the void* arm rejects by
    //     default; a `allowVoidPtrArithmetic: bool` opt-in field
    //     extends this struct.
    //   * `D-LANG-VOIDPTR-FN-CONVERT`: `void* ↔ fn-pointer` is
    //     technically UB in standard C even though every compiler
    //     permits it. Function-pointer types landed (FC4: Ptr<FnSig>
    //     declarators + indirect calls). LANDED: the `allowVoidPtrFnConvert`
    //     opt-in below now gates the whole fn<->void* class (Option B, the
    //     single authoritative gate) for the gcc/POSIX dlsym / Tcl ClientData
    //     idiom; c-subset opts in, default false stays ISO-strict.
    //   * `D-LANG-VOIDPTR-PREDICATE-GATE` (type-design analyst,
    //     step 13.2 audit fold): if a future language needs
    //     per-element-type predicates ("only T* → void* when T ∈
    //     {char, byte}" or "only when sizeof(T) ≥ alignof(void*)"),
    //     today's two-bool shape forecloses it. Trigger: first
    //     language whose `void*` rules depend on the element T.
    //     Closure: add a `PointerConversionPredicate` variant slot
    //     beside the bools (additive, doesn't break existing flags).
    //   * `D-TYPERULES-PTRRULES-PASS-BY-VALUE` (type-design analyst
    //     D4, step 13.2 audit fold): the `isAssignable` signature
    //     takes `PointerConversionRules const&` for a 2-byte POD.
    //     By-value would marginally simplify; const-ref form is
    //     idiomatic-enough today. Trigger: any post-merge pass
    //     touching the `isAssignable` signature (e.g. when a 3rd
    //     rules-block lands).
    struct PointerConversionRules {
        // T* → void* (typed → untyped). Information-erasing direction.
        // Universally safe (no runtime risk; just forgetting type).
        // C, C++, Objective-C: implicit. Rust, Swift: explicit.
        bool implicitToVoidPtr   = false;
        // void* → T* (untyped → typed). Information-asserting direction.
        // Unsafe (caller asserts untyped memory IS T-typed; unverifiable
        // at compile time). C, Objective-C: implicit. C++, Rust, Swift:
        // requires explicit cast.
        bool implicitFromVoidPtr = false;
        // D-LANG-NULL-POINTER-CONSTANT (step 13.3, 2026-06-02): per C
        // §6.3.2.3.3, an integer constant expression with value 0 is a
        // null pointer constant — convertible WITHOUT cast to any
        // object-pointer OR function-pointer type. C, C++, Obj-C all
        // admit. C++11+ also has `nullptr` (a typed `nullptr_t`); the
        // `0`-form remains valid alongside.
        //
        // This rule is VALUE-AWARE (looks at the literal's value, not
        // just its type), so it lives in the semantic analyzer at the
        // call-arg / return / init check sites (NOT in `isAssignable`,
        // which stays type-only). HIR `coerce()` materializes the
        // admitted conversion as `Cast(IntLiteral(0), Ptr<T>)`.
        //
        // Rust / Swift / Zig: false — they have explicit `std::ptr::null`
        // / `nil` / `null` keywords typed at the source level.
        // When this is the default-false, an extern signature with a
        // `Ptr<T>` parameter rejects the literal `0` arg and the user
        // must use the language's typed-null mechanism.
        bool nullPointerConstantFromIntegerZero = false;
        // C23 §6.3.2.3.4 / §6.2.5 (D-CSUBSET-NULLPTR): the predefined constant
        // `nullptr` (type nullptr_t, interned TypeKind::NullptrT) converts WITHOUT
        // cast to any pointer type. TYPE-aware (not value-aware, unlike the integer-0
        // form), so it lives in the `isAssignable` chokepoint as a ONE-WAY arm
        // (Ptr←NullptrT) gated on this flag; nothing converts TO nullptr_t (the
        // one-way constraint enforced by the absence of any NullptrT-as-lhs arm).
        // Default false → a non-C23 schema (toy/tsql/older-C) keeps NullptrT entirely
        // inert. Only C23+ declares it true (alongside
        // `nullPointerConstantFromIntegerZero`, since the `0`-form remains valid in
        // C23). nullptr→BOOL is DEFERRED (D-CSUBSET-NULLPTR-BOOL-CONVERSION): the
        // c-subset has no scalar→bool conversion yet; nullptr in a controlling
        // expression still works via the HIR condition lowering, so nothing real is
        // lost.
        bool nullPointerConstantFromNullptrT = false;
        // D-LANG-VOIDPTR-FN-CONVERT (C 6.3.2.3): implicit function-pointer to/from
        // `void*` conversion -- INCLUDING the bare function DESIGNATOR (`FnSig`, not
        // yet decayed) -> `void*` form, the gcc/POSIX `dlsym` / Tcl `ClientData`
        // idiom (`Tcl_CreateCommand(i, "md5", MD5DigestToBase16, ...)` passes a bare
        // function name into a `void*` ClientData parameter). Converting between a
        // function pointer and `void*` is UNDEFINED in ISO C (6.3.2.3 guarantees only
        // object-pointer to/from `void*`), but POSIX (`dlsym`) REQUIRES it and on
        // every LP64/LLP64 target a function pointer and `void*` share the SAME
        // representation and width -- so the conversion is representation-identical
        // and can NEVER be a miscompile (the HIR realizes it as the same FnSig->Ptr
        // Bitcast-over-GlobalAddr the function-pointer decay already uses; no MIR
        // change). This is the SINGLE authoritative gate for the WHOLE fn<->void*
        // class (Option B): both the bare-designator -> `void*` admit AND the
        // `Ptr<FnSig>` <-> `void*` pointer-to-pointer arms route through THIS flag
        // (not the generic object-pointer `implicitToVoidPtr`/`implicitFromVoidPtr`).
        // Default false = strict (a non-C schema, or a language wanting ISO-strict
        // function-pointer typing, keeps it rejected). Read by `isAssignable` (admit)
        // and `coerce()` in `cst_to_hir.cpp` (realize), in lockstep. The boundary is
        // Void-pointee-ONLY: a function pointer / designator -> a NON-void object
        // pointer (`char*`, `int*`, `struct S*`) STAYS a loud reject regardless of
        // this flag.
        bool allowVoidPtrFnConvert = false;
        // D-LANG-FFI-DESCRIPTOR-INT-POINTEE-COMPAT: at a SHIPPED-FFI-DESCRIPTOR
        // function's CALL-ARGUMENT boundary ONLY, admit a real C integer pointer
        // (`long long*`, `sqlite3_int64*`, `long*`-on-LP64) into a descriptor
        // parameter modeled as the abstract width-based `ptr<i64>` (…) whose pointee
        // is the SAME representation (size ∧ signedness ∧ integer-base-kind, via
        // TypeInterner::sameRepresentation) but a DISTINCT identity (the `_Generic`-
        // splitting vocabulary NAME differs). The SQLite testfixture needs it for
        // `Tcl_GetWideIntFromObj(interp, obj, &wideIntLvalue)`: gcc accepts the
        // ABI-identical 8-byte pointer, DSS's strict pointer-pointee typing rejected
        // it S0003. Read by `isAssignable` (admit — the trailing
        // `ffiDescriptorPointeeIntCompat` arg, passed true ONLY by
        // `checkCallAgainstSig` at a `isShippedDescriptorFn` DIRECT callee) and by
        // `cst_to_hir.cpp::coerce` (realize — the node-mark-gated Ptr→Ptr bitcast), in
        // lockstep. Default FALSE = strict: the boundary is SCOPED — native C-to-C
        // pointer typing, init/assign/return, and the fn-pointer/indirect call paths
        // ALL stay strict; identity is NEVER merged (a compat admission, not a
        // canonicalization — `_Generic(long:,long long:)` still distinguishes). Only
        // c-subset opts in. Per-target by construction: on LLP64/pe64 `long` is I32,
        // so `long*` still REFUSES `ptr<i64>` (sameRepresentation's kind axis) with NO
        // format branch. Sibling of `allowVoidPtrFnConvert` (the fn<->void* Option-B
        // gate) — the same admit/realize-in-lockstep discipline.
        bool ffiDescriptorIntPointeeCompat = false;
    };
    PointerConversionRules pointerConversions;

    // C 6.3.1.1: `char` is an integer type. Read by `isAssignable`'s char arm, which
    // admits BOTH directions of the char↔integer conversion — int→char (`char x='c';`,
    // narrowing) and char→int (`int y=c;`, widening; codegen materializes the Char→int
    // SExt). Default false → a non-C schema (toy/tsql) keeps `Char` strictly distinct
    // from the integer ranks. Required by the char-literal typing AND char value use.
    bool charConvertsToArith = false;

    // C23 6.3.1.3/6.3.1.8 (D-CSUBSET-BITINT): admit `_BitInt(N)` into the implicit
    // integer conversions. Read by `isAssignable`'s BitInt arm (BitInt↔BitInt and
    // BitInt↔standard-integer, either direction) AND injected into the resolved
    // `usualArithmeticCommonType` rules at the two `resolveArithmeticRules` call
    // sites (so a `_BitInt` participates in the usual arithmetic conversions without
    // promoting). Default false → a non-C schema keeps `_BitInt` inert (and it has
    // no `_BitInt` types anyway). Mirrors the charConvertsToArith / enumConvertsToArith
    // gate. Set true alongside the `semantics.bitInt` surface.
    bool bitIntConversions = false;

    // C 6.7.2.2 / 6.3.1.1: an enum is an integer type with an underlying integer
    // (DSS interns it as `TypeKind::Enum`, the underlying kind in `scalars[0]`).
    // Read by `isAssignable`'s enum arm, which admits BOTH directions of the
    // enum↔integer conversion — enum→int (`return BLUE;` / `int x = BLUE;`,
    // widening) and int→enum (`enum Color e = 1;` / the `e += 1` write-back,
    // narrowing). Default false → a non-C schema (toy/tsql) keeps `Enum` strictly
    // distinct from the integer ranks. Closes D-CSUBSET-ENUM-INT-CONVERSION.
    bool enumConvertsToArith = false;

    // C 6.3.1.2 (D-CSUBSET-NULLPTR-BOOL-CONVERSION): a scalar value converts INTO a
    // `_Bool` lhs in an assignment context — `_Bool b = 5;` / `_Bool b = ptr;` /
    // `_Bool b = nullptr;` / `_Bool b = (a<b);` — yielding 0 if it compares equal to
    // 0, else 1. Read by `isAssignable`'s scalar->Bool arm (init / assignment /
    // call-arg / return), which admits an arithmetic (int rank / float / Char /
    // Enum) OR pointer OR nullptr rhs into a Bool lhs. The HIR `coerce()` realizes
    // it as the `!= 0` truthiness test (the SAME condition-materialization `if(x)`
    // uses — NOT a low-bit-truncating Cast), so the post-coerce verifier (default
    // false) stays strict. Default false -> a non-C schema (toy/tsql) keeps `_Bool`
    // strict. The MIRROR of `boolWidensToArith` (Bool rhs -> arith lhs). Closes the
    // scalar->bool gap the D-CSUBSET-SIZEOF-COMPARISON-INT-TYPE fix unmasked (once
    // `a<b` types `int`, `_Bool b = (a<b)` needs this arm).
    bool scalarConvertsToBool = false;

    // C 6.3.1.3 / 6.5.16.1 (D-CSUBSET-INT-CROSS-SIGNEDNESS-CONVERT): a signed↔unsigned
    // implicit conversion in an ASSIGNMENT context — `int x = u;`, `x = u;`,
    // `return i;` from an int-returning fn with an unsigned `i`, `f(u)` to an int
    // param — is value-preserving in range / modular out of range. Read by
    // `isAssignable`'s cross-signedness arm, which admits signed↔unsigned WITHIN the
    // integer ranks in BOTH directions and at ANY width (incl. cross-signedness
    // narrowing like `int x = sizeUL`); the HIR `coerce()` arithmetic-core arm already
    // materializes the width-exact Cast. Default false → a non-C schema (toy/tsql) keeps
    // signed/unsigned strictly distinct. SCOPE: signed↔unsigned only — SAME-signedness
    // narrowing (`int x = aLong`) is the SIBLING gate `intSameSignednessNarrows` below.
    bool intCrossSignednessConverts = false;

    // C 6.3.1.3 / 6.5.16.1 (D-CSUBSET-INT-SAME-SIGN-NARROW): a same-signedness integer
    // NARROWING in an ASSIGNMENT context — `short s = anInt;`, `signed char c = anInt;`,
    // `int i = aLong;`, the same across init / assignment / call-arg / return — is
    // value-preserving in range / modular (truncating) out of range. Read by
    // `isAssignable`'s signed/unsigned rank arms, which admit `rank(rhs) > rank(lhs)`
    // (narrowing) ONLY when this is true; WIDENING stays unconditionally admitted. The
    // HIR `coerce()` arithmetic-core arm materializes the width-exact Cast (MIR `Trunc`),
    // the SAME path cross-signedness narrowing already uses, so NO codegen change. Default
    // false → a non-C schema (toy/tsql) keeps the strict widening-only rank rule. Together
    // with intCrossSignednessConverts this completes the C integer-conversion matrix
    // (needed for SQLite). Pinned by test_type_rules `IsAssignableAdmitsSameSignednessNarrowingWhenGated`.
    bool intSameSignednessNarrows = false;

    // C 6.3.1.4 / 6.3.1.5 / 6.5.16.1 (D-CSUBSET-INT-FLOAT-CONVERSION): the two
    // directions of the int↔float implicit ASSIGNMENT conversion, gated
    // INDEPENDENTLY. `intConvertsToFloat` — an integer rhs flows into a floating
    // lhs (`double d = 5;`, `f(anInt)` to a `double` param; the sqlite
    // `kahanBabuskaNeumaierStep(pSum, iBig)` shape feeds an `i64` to a `volatile
    // double`). `floatConvertsToInt` — a floating rhs flows into an integer lhs
    // (`int n = aDouble;`, truncating toward zero, UB if out of range). Read by
    // `isAssignable`'s int↔float arms (init / assignment / call-arg / return). Each
    // direction's HIR `coerce()` arithmetic-core arm materializes the width-exact
    // Cast (MIR SIToFP/UIToFP for int→float, FPToSI/FPToUI for float→int), so the
    // post-coerce verifier (both default false) stays strict. Default false → a
    // non-C schema (toy/tsql) keeps int and float strictly distinct. Together with
    // intCrossSignednessConverts + intSameSignednessNarrows this completes the C
    // arithmetic-conversion matrix (needed for SQLite).
    bool intConvertsToFloat = false;
    bool floatConvertsToInt = false;

    // Two orthogonal per-language alias-analysis opt-ins, both threaded
    // through `MirLoweringConfig` → `Mir` and read by CSE/LICM Load
    // admission via `Mir.aliasingMode()` + `Mir.charTypesAliasAll()`.
    //
    //   * `strictAliasingOnDistinctTypes` — C99 §6.5 strict aliasing.
    //     Lets `Ptr<I32>` vs `Ptr<I64>` resolve to No (Rule 6).
    //   * `charTypesAliasAll` — C99 §6.5 ¶7 character-type exception.
    //     Defaults true (sound for C/C++/Objective-C/Java/Go); a Rust
    //     frontend or strict-typed DSL sets false.
    //
    // The two compose: with `strict=true` + `charAliasAll=true`, a
    // `char*` Store does not alias an `int*` Load only by character-
    // exception (i.e., it MAY alias — Maybe). With `strict=true` +
    // `charAliasAll=false`, the same pair resolves to No.
    //
    // Loader-level unknown-key fail-loud mirrors the
    // `pointerConversions` pattern (D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD
    // discipline — a typo'd key would otherwise silently fall back to
    // the default and flip the language's optimization polarity).
    struct PointerAliasingRules {
        // Per C99 §6.5 ¶7 / C++ [basic.lval]: a glvalue accessed
        // through a pointer of a type that is NOT compatible with the
        // dynamic object type is undefined behavior. Optimizers that
        // honor this can prove `Ptr<I32>` and `Ptr<I64>` don't alias
        // (Rule 6 in `mirMayAlias`). Character-type pointer behavior
        // is controlled INDEPENDENTLY by `charTypesAliasAll` below
        // (the two semantics compose orthogonally).
        //
        // C, C++, Objective-C: true. Rust (via its borrow checker) is
        // arguably stricter but does NOT use this MIR-tier flag — it
        // enforces non-aliasing at the type-checker tier. Java / Go /
        // dynamic languages: false (no spec-level guarantee).
        bool strictAliasingOnDistinctTypes = false;

        // C99 §6.5 ¶7 character-type exception: a character-typed
        // pointer (`char*` / `signed char*` / `unsigned char*` — at the
        // MIR tier `Char`/`Byte` pointees) may alias an object of ANY
        // type. Enables serializers, hash visitors, memcpy
        // implementations, and bytewise inspection to be sound under
        // strict aliasing.
        //
        // Default `true` is the CONSERVATIVE direction — every
        // language gets the safe sound-but-imprecise answer until it
        // declares otherwise. C / C++ / Objective-C declare true.
        // Rust's `u8` does NOT have this exception (Rust enforces
        // aliasing at the borrow-checker tier and treats `&[u8]` like
        // any other typed slice); a Rust frontend would declare
        // false. A hypothetical strict-typed DSL where `char` is
        // truly opaque would also declare false.
        //
        // This flag is independent of `strictAliasingOnDistinctTypes`:
        // disabling the char-exception only matters in combination
        // with strict aliasing (the exception is what stops a Rule 5
        // strict-TBAA verdict from firing on a char pointer). Closes
        // `D-OPT-MIR-ALIAS-CHAR-EXCEPTION-OVERRIDE`.
        bool charTypesAliasAll = true;
    };
    PointerAliasingRules pointerAliasing;
};

} // namespace dss
