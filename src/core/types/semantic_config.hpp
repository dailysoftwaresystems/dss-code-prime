#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"

#include <cstdint>
#include <optional>
#include <string>
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
};

} // namespace dss
