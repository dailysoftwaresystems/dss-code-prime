#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"

#include <cstdint>
#include <optional>
#include <string>
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
struct DSS_EXPORT LinkageSpecifierEffect {
    std::optional<SymbolBinding>    binding;
    std::optional<SymbolVisibility> visibility;
};

struct DSS_EXPORT DeclarationRule {
    // The rule (resolved to RuleId) whose subtree introduces the decl.
    RuleId          rule{};
    // Visible-child indices. `nullopt` means "absent" (the field is not
    // used by this declaration form — e.g. tsql `createTableStmt` has no
    // init). Matches the NumberStyle optional-index precedent.
    std::optional<std::uint32_t> nameChild;
    std::optional<std::uint32_t> typeChild;
    std::optional<std::uint32_t> initChild;
    // Function-decl child roles (SE6). `paramsChild` points at the visible
    // child whose subtree holds the parameter declarations; `bodyChild` at
    // the body subtree (which is also a `scopes` rule so params bind into
    // it). `nullopt` for non-function declarations.
    std::optional<std::uint32_t> paramsChild;
    std::optional<std::uint32_t> bodyChild;
    // SE4 const-correctness: a token kind that, when found anywhere in the
    // `typeChild` subtree (or the whole declaration subtree when no
    // `typeChild` is set), marks the minted symbol const. `nullopt` ⇒ the
    // language has no const marker for this declaration form.
    std::optional<SchemaTokenId> constMarker;
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
    DeclarationKind kind        = DeclarationKind::Variable;
    NameMatchMode   nameMatch   = NameMatchMode::Self;
    // D8 unused-variable warning: when true, a symbol minted by this
    // declaration that is NEVER referenced (empty use-set after analysis)
    // emits S_UnusedVariable (a WARNING). Per-declaration opt-in so a
    // language can warn on local variables but not on parameters (unused
    // params are intentional) or globals/columns. Default false ⇒ no
    // unused check for this declaration form.
    bool            warnIfUnused = false;
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
    // Optional kind-discriminator. When set, the engine evaluates it at
    // pass 1 and uses the resulting effective kind / params / body
    // instead of the static fields above.
    std::optional<KindDiscriminator> kindByChild;
    // SE-arrays (HR9): optional C-style declarator suffix (e.g. `int a[10]`).
    // The suffix is a sibling of the type (not a type-position constructor), so
    // the engine matches it by rule within the declaration subtree rather than
    // via `typeShapes`. `nullopt` ⇒ this declaration form has no array syntax.
    std::optional<ArraySuffix> arraySuffix;
    // D5.1: optional composite-type collection. When set, Pass 1.5 composes the
    // declaration's `kind: type` symbol's TypeId via `interner.structType(name,
    // fieldTypes)` from the field-symbols minted in this declaration's scope.
    // `kind` must be `Type` and the rule must also appear in `scopes`. Generic
    // across record-bearing languages.
    std::optional<FieldChildrenDescriptor> fieldChildren;
    // Source-text name of the declared rule, retained for diagnostics.
    std::string     ruleName;
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

// SE6: a built-in function the engine binds into a CU-wide "builtins"
// scope (visible everywhere, shadow-able by user decls). Interned as a
// FnSig over `paramCores` → `resultCore`. A `variadic` builtin skips the
// arg-count check (e.g. tsql's COALESCE accepts any arity).
struct DSS_EXPORT BuiltinFunctionMapping {
    std::string           name;
    std::vector<TypeKind> paramCores;
    TypeKind              resultCore = TypeKind::Void;
    bool                  variadic   = false;
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

// Literal token-kind → core TypeKind. Pass 2 reads the token-kind of a
// matched literal leaf and assigns the corresponding lattice type via
// `TypeInterner::primitive(core)`.
struct DSS_EXPORT LiteralTypeMapping {
    SchemaTokenId   literal{};
    TypeKind        core = TypeKind::Void;
    std::string     literalName;   // source-text name retained for diags
};

// The full `semantics` block. Every facet is optional; absent ⇒ that
// facet is just not analyzed.
struct DSS_EXPORT SemanticConfig {
    std::vector<DeclarationRule>    declarations;
    std::vector<ReferenceRule>      references;
    // D5.1: member-access expression rules (`obj.field` and `ptr->field`). Pass 2
    // resolves each to its field's SymbolId + type. Empty ⇒ the language has no
    // member-access surface (toy / tsql currently).
    std::vector<MemberAccessRule>   memberAccesses;
    std::vector<ScopeRule>          scopes;        // rules that open a fresh lexical scope
    std::vector<BuiltinTypeMapping> builtinTypes;
    std::vector<TypeShapeRule>      typeShapes;
    std::vector<LiteralTypeMapping> literalTypes;
    std::vector<AssignmentRule>     assignments;       // SE4 const-correctness
    std::vector<CallRule>           callRules;         // SE6 call checking
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
    // (self-dir + includeDirs). A header found here is a c-subset SOURCE
    // file parsed by THIS language's own grammar and merged via the
    // existing include-following resolver — its `extern` decls then flow
    // through FF5 `synthesizeFfiFromSourceDecls` like a program's own.
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
    //     permits it. When function-pointer types land (D-ML7-2.4),
    //     another `allowVoidPtrFnConvert: bool` opt-in extends this
    //     struct.
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
    };
    PointerConversionRules pointerConversions;

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
