#pragma once

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/compilation_unit/unit_attribute.hpp"
#include "core/export.hpp"
#include "core/substrate/transparent_string_hash.hpp"  // c97: heterogeneous scope-binding lookup
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_lattice.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The output of phase #8's semantic analysis. Move-only. Holds:
//   - a shared_ptr to the analyzed CompilationUnit (stable address —
//     `UnitAttribute<T>` holds raw Tree* and they must not dangle);
//   - the per-CU TypeLattice (TypeInterner + TypeRegistry);
//   - the symbol table (SymbolId → SymbolRecord) and scope tree;
//   - per-node side tables (NodeId → SymbolId for both decls and uses;
//     NodeId → TypeId for typed expression positions);
//   - the analyzer's own DiagnosticReporter (S_* diagnostics).
//
// Three-pass discipline (semantic_analyzer.cpp): Pass 1 mints every
// declaration into its tree's root scope; Pass 1.5 resolves declared
// types; Pass 2 resolves uses and propagates/checks types. Forward
// references (G-209) fall out for free (all decls minted before any
// use is resolved).

namespace dss {

// C 6.2.3 name spaces. C puts struct/union/enum TAGS (`struct Foo`) in a
// namespace SEPARATE from ordinary identifiers (objects, functions, typedef
// names, enumerators) — so `typedef struct Pair { … } Pair;` is legal (the
// tag `Pair` and the typedef alias `Pair` are distinct names). Each binding
// (and lookup) selects a namespace; the two are independent maps in a scope.
// This is the only axis C 6.2.3 requires for this frontend's subset (label
// and member namespaces are handled elsewhere — labels by the goto pre-scan,
// members by the per-struct field scope).
enum class SymbolNamespace : std::uint8_t {
    Ordinary = 0,   // objects, functions, typedef names, enumerators
    Tag      = 1,   // struct / union / enum TAGS
};

// A scope-tree node. ScopeId is the index into SemanticModel's scope
// vector (slot 0 is the InvalidScope sentinel; slot 1 is the CU root).
// Lookup walks `parent` links; `children` is retained for tooling/tests.
struct DSS_EXPORT ScopeRecord {
    ScopeId  parent{};
    NodeId   anchor{};   // tree node whose subtree opens this scope (or invalid for root)
    TreeId   tree{};
    // name -> SymbolId, for the ORDINARY namespace. Same-scope redeclaration
    // is caught here. (c97: transparent hasher/equality — `ScopeTree::lookup`
    // walks the parent chain with a `string_view` key, so the per-hop
    // `std::string` materialization is gone; existing `std::string` callers
    // are unaffected.)
    substrate::TransparentStringMap<SymbolId> bindings;
    // C 6.2.3 tag namespace: name -> SymbolId for struct/union/enum TAGS,
    // SEPARATE from `bindings`. A tag and an ordinary symbol of the same name
    // (`typedef struct Pair {…} Pair;`) coexist — one lives here, one in
    // `bindings`. Empty for any scope that declares no tags.
    substrate::TransparentStringMap<SymbolId> tagBindings;
    std::vector<ScopeId> children;
};

// One declared symbol. `type` is invalid when the analyzer could not
// determine the symbol's type (e.g. `var x;` with no initializer in a
// language without inferred typing). Pass 2 may upgrade `type` once
// initializer-inference runs.
struct DSS_EXPORT SymbolRecord {
    std::string name;
    ScopeId     scope{};
    NodeId      declNode{};         // the declaration's name node (or the rule node if no name child)
    NodeId      declRuleNode{};     // the declaration rule node itself (for diagnostic spans)
    TreeId      tree{};
    TypeId      type{};
    // The DeclarationRule's `kind` — Variable/Function/Table/Type. Read by
    // type-resolution (a Function symbol carries a FnSig type, etc.).
    DeclarationKind kind = DeclarationKind::Variable;
    // SE4 const-correctness: set when the decl's `constMarker` token was
    // found in the type subtree. A reassignment of a const symbol emits
    // S_ConstViolation.
    bool            isConst = false;
    // c27 (D-CSUBSET-VOLATILE-POINTEE) RETIRED the c21 `isVolatile` bool: volatile
    // is now a TYPE qualifier (TypeKind::VolatileQual), so OBJECT-volatility is
    // read directly off a symbol's resolved `type` (top-level VolatileQual) at
    // CST→HIR access lowering (`recordVolatility`), and POINTEE-volatility rides
    // the accessed type at the deref/index/member (`volatileFlagForType` in
    // HIR→MIR). The coarse token-scan the bool fronted could not tell a volatile
    // OBJECT (`int * volatile p`) from a volatile POINTEE (`volatile int *p`),
    // which c27's type model now distinguishes. No separate symbol bool remains.
    // SE6: set on a builtin-function symbol declared `variadic` — the
    // call-check skips arg-count enforcement for it.
    bool            variadicBuiltin = false;
    // c103 (D-CSUBSET-INTRINSIC-UMULH): copied from the builtin's
    // BuiltinFunctionMapping.lowering at injection. When != None, a CALL to this
    // builtin symbol lowers (in CST→HIR) to a `HirKind::BuiltinCall` carrying this
    // lowering, which HIR→MIR maps to the dedicated MirOpcode — NOT an ordinary
    // Call. None (the default) for every non-lowering symbol.
    BuiltinLowering builtinLowering = BuiltinLowering::None;
    // SE7/D8: copied from the minting DeclarationRule's `warnIfUnused`. After
    // analysis, a symbol with this flag set AND an empty use-set emits
    // S_UnusedVariable (a WARNING) at `declRuleNode`'s span.
    bool            warnIfUnused = false;
    // D5.1: field ordinal within the enclosing composite-type declaration
    // (struct/union). Set on EVERY minted symbol by Pass 1 to its declaration-
    // order index in its declaring scope; meaningful only for field symbols
    // (the inner declarations of a composite-type rule with `fieldChildren`)
    // and read at HIR-lowering time as the `MemberAccess.payload` field index.
    // For all other symbols (Variable/Function/Type/Table outside a composite
    // scope) it is harmless 0-or-positional noise — follows the established
    // SymbolRecord precedent for kind-specific fields (`isConst`,
    // `warnIfUnused`, `variadicBuiltin`).
    std::uint32_t   fieldIndex = 0;
    // FC16 D-CSUBSET-ANON-MEMBER-PROMOTION (C11/C23 §6.7.2.1 ¶13): true iff this
    // is a synthetic anonymous-member field symbol (`<anon:…>`) WHOSE resolved
    // type is a Struct/Union — i.e. a genuine anonymous struct/union whose
    // members are PROMOTED into the enclosing composite's member namespace (NOT
    // a bare `int : 3;` bit-field or a rejected `int ;`). Set at Pass 1.5 once
    // the head type resolves. Member-access resolution recurses through such
    // members' own field scopes; HIR lowering synthesizes one intermediate
    // MemberAccess hop per promotion level. Harmless false for every other symbol.
    bool            isAnonymousMember = false;
    // FC16 D-CSUBSET-ANON-MEMBER-PROMOTION: for a field reachable ONLY through
    // one or more anonymous-member promotions, the ordered chain of anonymous-
    // member SymbolIds (outermost→innermost) that must be traversed to reach it.
    // EMPTY for every direct field (zero overhead). HIR lowering emits one
    // synthetic MemberAccess per entry before the final field access.
    std::vector<SymbolId> anonAncestorPath;
    // D5.1: the inner scope holding this symbol's fields, set by Pass 1 on a
    // Type-kind symbol minted from a declaration with `fieldChildren`. Pass 2's
    // member-access resolution reads this to look up `field` in `obj.field`:
    // TypeId → struct symbol → `structScope` → name lookup. `InvalidScope`
    // (default) for every non-composite symbol.
    ScopeId         structScope{};
    // D5.5: the integer value of a named INTEGER CONSTANT symbol. Set by Pass
    // 1.5 for an enumerator (explicit `= N` overrides the running counter;
    // missing = previous + 1, C99 §6.7.2.2), OR at descriptor injection for a
    // shipped CONSTANT (`isInjectedConstant`). Carries the int64 BIT-PATTERN —
    // for an unsigned-typed constant the uint64 value reinterpreted; the HIR
    // fold re-reads it per the type's signedness. c52: a FLOAT-typed injected
    // constant (`INFINITY`) reuses this int64 carrier to hold the IEEE-754 f64
    // BIT-PATTERN (std::bit_cast), which the fold bit_casts back to a `double`
    // when the type is a float kind. Meaningful only when exactly one of
    // `isEnumerator` / `isInjectedConstant` is set; harmless 0 elsewhere.
    std::int64_t    enumValue = 0;
    // D-CSUBSET-FN-PROTOTYPE: a bare function PROTOTYPE — a function-TYPED object
    // declaration with a function suffix on its NAME and NO body (`int f(int);`).
    // Set by Pass 1 (effectiveKind == Variable + the name carries a function
    // suffix); Pass 1.5 UPGRADES such a symbol's `kind` to Function (it is a
    // function declaration, callable, mergeable with a later definition). A
    // function POINTER (`int (*fp)(int)`) does NOT set this — its suffix sits on
    // the outer declarator, not the name's direct declarator. Default false.
    bool            isProtoDeclaration = false;
    // D-CSUBSET-FN-PROTOTYPE: a proto / redundant function redeclaration that a
    // SURVIVING declaration superseded (proto→def: the proto is absorbed and the
    // definition wins the binding; def→proto / proto→proto: the new redundant
    // decl is absorbed and the prior binding is kept). An absorbed declarator
    // emits NO HIR node — the survivor carries the symbol (the definition emits
    // the body; an unabsorbed proto emits nothing either). Default false.
    bool            isAbsorbedProto = false;
    // D-CSUBSET-EXTERN-DEFINITION-MERGE: TRUE iff this symbol was minted from a
    // NON-DEFINING declaration — a declaration that announces a symbol whose
    // storage/body lives elsewhere (an `extern` declaration in C). Set by Pass 1
    // from the minting DeclarationRule's `nonDefiningDeclaration` flag (config-
    // driven, no rule-name identity). A non-defining declaration of the same name
    // as an in-TU DEFINITION MERGES: the definition WINS the binding and the
    // extern is absorbed (`isAbsorbedProto` set, its HIR ExternFunction/
    // ExternGlobal node suppressed). Two non-defining declarations are idempotent;
    // two definitions still collide (S_RedeclaredSymbol). Default false.
    bool            isExternDeclaration = false;
    // c33 (D-CSUBSET-TENTATIVE-DEFINITION): TRUE iff this symbol was minted from a
    // file-scope OBJECT declaration with NO initializer — a TENTATIVE DEFINITION
    // (C 6.9.2). Like `extern`/proto it is NON-DEFINING for redeclaration-merge
    // purposes: it merges with a later real (initialized) definition (the def wins
    // the binding, the tentative is absorbed) and with other tentatives of the same
    // name (one of them survives and lowers to a single zero-initialized global).
    // Two REAL definitions (both initialized) still collide (S_RedeclaredSymbol); a
    // tentative + a real definition of an INCOMPATIBLE type fails loud after Pass 1.5
    // (S_IncompatibleRedeclaration) via the shared merged-decl type sweep. Read ONLY
    // by `mergeOrCollideRedeclaration` (folded into its non-defining test); the HIR
    // lowering keys off `isAbsorbedProto` (set on whichever side is absorbed), so a
    // SURVIVING tentative emits its zero-init global unchanged. Default false.
    bool            isTentativeDefinition = false;
    // D-CSUBSET-ENUM-INT-CONVERSION (FC8): TRUE iff this symbol IS an enumerator
    // constant (bound under a `compositeKind:"enum"` decl, where `enumValue` was
    // set). DISTINGUISHES an enumerator from a storage-backed `enum E e;` local —
    // BOTH carry `type.kind == Enum`, but only the enumerator may fold to its
    // constant value at HIR Ref-lowering; folding a storage-backed local would be
    // a silent miscompile. Default false (every non-enumerator symbol).
    bool            isEnumerator = false;
    // Item 1 (shipped-header constants): TRUE iff this symbol is a NAMED CONSTANT
    // injected from a neutral shipped-lib descriptor's `constants` (integer, e.g.
    // `CHAR_BIT` from `limits.json`) OR `floatConstants` (float, e.g. `INFINITY`
    // from `math.json` — c52). Like an enumerator it folds its Ref to `enumValue`
    // at HIR lowering AND resolves to that value in a constant-expression context
    // (array dim / case / global init) via the const-eval engines' direct-value
    // arm — but its `type` is the constant's OWN scalar (NOT an Enum), so the fold
    // derives the literal core from the type directly (an integer core reads the
    // int64 carrier; a float core bit_casts it back to a double). INVARIANT: at
    // most one of `isEnumerator` / `isInjectedConstant` is true on any symbol
    // (they share `enumValue` but fold via different cores).
    bool            isInjectedConstant = false;
    // D-CSUBSET-BITFIELD (FC8): the declared bit-field width of a struct/union
    // field, or nullopt for an ordinary field. A TRANSIENT carrier — set at the
    // field's Pass 1.5 resolution (the `: width` const-expr evaluated + validated
    // against the field's integer type there), then READ at the composite's
    // Pass 1.5 type composition to build the `fieldBitWidths` passed to
    // `structType`. After composition the interned TYPE is the authoritative
    // source (layout + codegen read `TypeInterner::fieldBitWidth`); this record
    // field is only the resolution→composition plumbing (cf. `enumValue`). A
    // zero-width (anonymous `int : 0;`) bit-field stores 0 (distinct from nullopt).
    std::optional<std::uint32_t> bitFieldWidth;
    // C11/C23 6.7.5 (D-CSUBSET-ALIGNAS): the EXPLICIT `alignas(N)` / `alignas(T)`
    // alignment override on this declaration (bytes, a power of two), or nullopt
    // for no override. Set at the declaration's Pass-1 resolution — the value form
    // const-folds via `constIntExpr`, the type form is `_Alignof(T)` via
    // `computeLayout`, both validated (power-of-two / ≤256 / ≥ natural align /
    // context) there. `alignas(0)` is a NO-OP (6.7.5p3) → left nullopt. For a
    // struct/union MEMBER it is read at the composite's Pass-1 completion to build
    // the `fieldAligns` span passed to `completeComposite` (the interned TYPE then
    // owns the raised layout — mirrors `bitFieldWidth`). For a VARIABLE it is
    // stored here as the authoritative value; threading it to globals/locals
    // codegen is a SEPARATE deferred task (D-CSUBSET-ALIGNAS: this cycle stores it
    // unconsumed for variables — member alignas works end-to-end via the interner).
    std::optional<std::uint32_t> explicitAlignment;
    // VLA C4b (D-CSUBSET-VLA): for a VLA-TYPEDEF OBJECT (`typedef int R[n]; R a;`)
    // — the SymbolId of the typedef `R` whose (variable-length) array type this
    // object aliases; `InvalidSymbol` (default) for every other symbol. C99
    // §6.7.7p2: the size expression `n` is evaluated ONCE, when the typedef `R`
    // is reached, and FROZEN — every later `R a;` allocates with that frozen
    // size. `R a;`'s VLA-ness comes entirely from the head alias, so the object's
    // own declarator carries no size to capture; this field records WHICH typedef
    // froze it. Set in `resolveDeclTypesPost` ONLY when the object's declared type
    // is EXACTLY the head type (`declTy == headTy` — a pure `R a;`, no own suffix
    // / stars) AND that head type is (or contains) a VLA; the `declTy == headTy`
    // gate excludes the deferred stacked-suffix (`R a[m]`) and ptr (`R *p`)
    // shapes. Read at HIR lowering (record a.v→R.v into `typedefVlaOriginBySymbol`
    // + skip the object's own size capture) and threaded to HIR→MIR, where `R a;`'s
    // alloca copies R's decl-frozen size slots down into its own. A dropped/unset
    // origin is a safe fail-loud downstream (no captured size), never a miscompile.
    SymbolId        vlaTypedefOrigin{};
    // FC16 (D-CSUBSET-NORETURN): TRUE iff this FUNCTION symbol is declared
    // `noreturn` (C11 6.7.4 `_Noreturn` / C23 6.7.12.7 `[[noreturn]]` / GNU
    // `__attribute__((noreturn))`). Set at Pass-1.5 declarator resolution when the
    // function declaration's specifier prefix names the attribute (gated on the
    // declared type being a FnSig), OR from a shipped-lib descriptor's `noreturn`
    // (abort/exit). OR-merged across a proto/definition pair (the post-1.5
    // mergedFnDecls sweep) so a call — which resolves to the definition — sees the
    // flag even when only the prototype spelled it. Read at HIR lowering: a DIRECT
    // call to such a function is wrapped `Block{ ExprStmt(call), Unreachable }` so
    // the path structurally terminates (the `wrapIfProvablyInfinite` precedent). A
    // DROPPED flag is a safe miss (a spurious H_VerifierFailure — fail-loud), never
    // a silent miscompile. Default false.
    bool            isNoreturn = false;
    // FC17 (D-CSUBSET-CONSTEXPR): TRUE iff this symbol was declared with the C23
    // 6.7.1 `constexpr` OBJECT storage-class. Set at Pass-1 minting when the
    // declaration's specifier prefix carries the language's
    // `constexprKeywordToken` (`specifierPrefixHasConstexpr`, the
    // `specifierPrefixNamesNoreturn` mirror); IMPLIES `isConst` (a constexpr
    // object is const — the minting site sets both, so every const consumer
    // [const-violation check, const-symbol init folding] sees it uniformly).
    // Read by Pass 2's `validateConstexprDeclarator`, which enforces the 6.7.1
    // constraints AT THE DECLARATION (compile-time-constant initializer /
    // missing initializer / function / volatile-qualified / aggregate — each a
    // fail-loud diagnostic, never a silent degrade to plain const). ZERO
    // codegen reads it: a VALIDATED constexpr object lowers byte-identically to
    // a const object with a foldable initializer (the file-scope INTERNAL
    // linkage — C23 6.2.2p3 — rides the declaration row's `linkageSpecifiers`
    // config, not this flag). Default false.
    bool            isConstexpr = false;
    // TLS C1 (D-CSUBSET-THREAD-LOCAL): TRUE iff this OBJECT symbol was declared
    // with C11/C23 6.7.1 thread storage duration (`_Thread_local` /
    // `thread_local`). Set at Pass-1 minting when the declaration's specifier
    // prefix carries a token whose row `linkageSpecifiers` entry declares
    // `{threadStorage: true}` (`specifierPrefixHasThreadStorage` — the
    // specifierPrefixHasConstexpr mirror, keyed on the SAME config facet the
    // linkage scan folds, so the two tiers can never disagree on the
    // vocabulary). Read by Pass 2's `validateThreadLocalDeclarator` (6.7.1
    // constraints: objects only / block scope needs static-or-extern /
    // forbidden combinations) and by the redeclaration merge (a same-TU
    // mismatch on this flag is S_ThreadLocalRedeclarationMismatch — 6.7.1p3
    // requires the specifier on EVERY declaration of the name). CST→HIR's
    // `recordThreadLocal` projects it onto the HirThreadLocalMap side-table
    // (the recordMutability/isConst precedent) → PendingGlobal.isThreadLocal
    // → MirGlobal.isThreadLocal → the asm/walker TLS section tiers (slices
    // B/C). Orthogonal to binding/visibility (a file-scope thread_local
    // keeps external linkage). Default false.
    bool            isThreadLocal = false;
    // FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS, C23 6.7.13): the standard-attribute
    // facts folded from the declaration's specifier prefix by
    // `scanAttributeSemantics` (Pass-1.5 declarator resolution — the
    // alignas/noreturn shared-prefix precedent; computed once per declaration,
    // applied to EVERY declarator, so `[[maybe_unused]] int a, b;` flags both).
    //
    // `isMaybeUnused` (C23 6.7.13.4 / GNU `unused`): the D8 unused-variable
    // check skips this symbol. Deliberately NOT proto/def-merged — it is
    // consulted only at the declarator's OWN D8 check (each declaration
    // suppresses its own warning). A dropped flag is a spurious WARNING, never
    // a miscompile.
    bool            isMaybeUnused = false;
    // `isDeprecated` (C23 6.7.13.3): every USE of this symbol warns
    // S_DeprecatedSymbolUsed at the Pass-2 reference-resolution chokepoint
    // (per use site, incl. a call's callee). OR-merged across a proto/def pair
    // (the isNoreturn mergedFnDecls precedent — a call resolves to the
    // survivor); `deprecatedMessage` merges first-non-empty-wins. Warning-only:
    // a dropped flag misses advice, never bytes. Types (struct/union/enum tags,
    // typedefs) are the named deferral D-CSUBSET-ATTRIBUTE-DEPRECATED-TYPES
    // (they resolve via type resolution, not this chokepoint — silently inert).
    bool            isDeprecated = false;
    std::string     deprecatedMessage;
    // `isNodiscard` (C23 6.7.13.2 / GNU `warn_unused_result`): a DIRECT call to
    // this function whose result is discarded as a bare expression statement
    // warns S_NodiscardResultDiscarded (checkCall's two-hop discard-context
    // check; the `(void)f()` cast idiom suppresses by construction). OR-merged
    // across a proto/def pair like isDeprecated; message first-non-empty-wins.
    bool            isNodiscard = false;
    std::string     nodiscardMessage;
    // FC17.5 (D-CSUBSET-FUNC-PREDEFINED-IDENTIFIER, C99 6.4.2.2): TRUE iff this
    // is a SYNTHETIC predefined function-name symbol (`__func__` / a configured
    // alias) that Pass 1 bound into a function DEFINITION's own scope, BEFORE
    // the params (so a param of the same name collides → S_RedeclaredSymbol at
    // its own span). Such a symbol is `isConst` (SE4 catches assignment /
    // compound-assign → S_ConstViolation) and carries `type` =
    // Array<narrow-string-core, len+1> minted AT THE BIND (there is no CST
    // declarator to resolve at Pass 1.5). HIR lowering FOLDS a read to a
    // string-literal-shaped constant (`predefinedFunctionNameText` supplies the
    // bytes) — byte-identical to a real string literal, so rodata/decay/
    // indexing ride unchanged; the ++/--/compound-assign lvalue classifiers
    // reject it (S_PredefinedIdentifierNotAddressable — there is no storage
    // slot to write back to). Default false.
    bool            isPredefinedFunctionName = false;
    // The enclosing FUNCTION's name — the bytes a `__func__` read folds to
    // (WITHOUT the trailing NUL; the Array type's +1 carries it, exactly like a
    // string literal's pool entry). Meaningful only when
    // `isPredefinedFunctionName` is set; empty otherwise.
    std::string     predefinedFunctionNameText;
};

// FF11 neutral-JSON shipped-library descriptor extern
// (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC). One row per symbol the semantic
// phase MINTED from a resolved shipped-lib descriptor (e.g. `puts` from
// `stdio.json`, pulled in by `#include <stdio.h>`). The semantic phase injects
// each as an extern `SymbolRecord` into scope (so a call resolves like any
// declared function) AND records this row so the CST→HIR lowerer can synthesize
// the matching `ExternFunction`/`ExternGlobal` HIR node + an `HirExternRecord`
// (which FF5 `synthesizeFfiFromSourceDecls` then binds to the library). A
// descriptor symbol that a user declaration already claimed (goal-2) is SKIPPED
// at injection — no row here — so the user's decl is the sole authority and
// nothing is double-declared. The `signature` TypeId is interned in THIS
// model's lattice (the CU interner the lowerer also lowers through).
//
// Kept decoupled from the ffi descriptor enums: `isFunction` selects
// ExternFunction (true) vs ExternGlobal (false) at lowering, the only
// distinction the lowerer needs.
struct DSS_EXPORT ShippedExternSymbol {
    SymbolId    symbol;       // the minted extern symbol
    std::string name;         // the undecorated identifier → HirExternRecord.canonicalName
    TypeId      signature;    // its FnSig (function) or value type (object)
    // Model 3 (2026-06-09): the descriptor's per-object-format `library` map
    // ("pe"/"elf"/"macho" → image name) → HirExternRecord.libraryOverride. The
    // map is carried target-agnostically through HIR; compile_pipeline folds it
    // to a single string for the ACTIVE target's format (where the format is in
    // scope). A missing format key inherits externLibraryByFormat[format].
    std::unordered_map<std::string, std::string> library;
    bool        isFunction = true;  // ExternFunction vs ExternGlobal
    // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): the pe64 <threads.h> synth-recipe id
    // (== the symbol name, a validated descriptor invariant), or EMPTY for an ordinary
    // shipped extern. When non-empty the CST->HIR lowerer SKIPS this symbol's
    // extern-import synthesis (kernel32 exports no mtx_lock — the eager-import law) and
    // records {symbol, recipeId} into `CstToHirResult.synthRecipeBySymbol` so HIR->MIR
    // seeds `functionSymbols` (the user call lowers to GlobalAddr against a not-yet-
    // defined callee) and `synthesizeThreadsShim` supplies the definition before link.
    std::string recipeId;
};

class DSS_EXPORT SemanticModel {
public:
    // The analyzer is the only producer; construction is by move out of
    // analyze() (declared in semantic_analyzer.hpp).
    SemanticModel(std::shared_ptr<CompilationUnit const> cu,
                  TypeLattice                            lattice,
                  std::vector<ScopeRecord>               scopes,
                  std::vector<SymbolRecord>              symbols,
                  UnitAttribute<SymbolId>                nodeToSymbol,
                  UnitAttribute<TypeId>                  nodeToType,
                  UnitAttribute<NodeId>                  nodeToSelectedExpr,
                  DiagnosticReporter                     diagnostics,
                  std::unordered_map<std::uint32_t, std::vector<NodeId>> usesBySymbol,
                  std::unordered_map<std::uint32_t, ScopeId> compositeScopeByType,
                  UnitAttribute<bool>                    nullPointerConstantNodes,
                  std::vector<ShippedExternSymbol>       shippedExterns,
                  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
                                                         suppressedShippedLibraries,
                  DataModel                              dataModel) noexcept
        : cu_(std::move(cu)),
          lattice_(std::move(lattice)),
          scopes_(std::move(scopes)),
          symbols_(std::move(symbols)),
          nodeToSymbol_(std::move(nodeToSymbol)),
          nodeToType_(std::move(nodeToType)),
          nodeToSelectedExpr_(std::move(nodeToSelectedExpr)),
          diagnostics_(std::move(diagnostics)),
          usesBySymbol_(std::move(usesBySymbol)),
          compositeScopeByType_(std::move(compositeScopeByType)),
          nullPointerConstantNodes_(std::move(nullPointerConstantNodes)),
          shippedExterns_(std::move(shippedExterns)),
          suppressedShippedLibraries_(std::move(suppressedShippedLibraries)),
          dataModel_(dataModel) {}

    SemanticModel(SemanticModel const&)            = delete;
    SemanticModel& operator=(SemanticModel const&) = delete;
    SemanticModel(SemanticModel&&)                 = default;
    SemanticModel& operator=(SemanticModel&&)      = default;

    [[nodiscard]] CompilationUnit const&        unit()     const noexcept { return *cu_; }
    [[nodiscard]] TypeLattice const&            lattice()  const noexcept { return lattice_; }
    // Non-const: downstream HIR/MIR lowering interns NEW types (lowered
    // expression types, synthesized signatures) into the same per-CU
    // lattice after analysis, so the interner must stay open past the
    // model boundary. SE1-SE3 themselves do not mutate it post-analyze().
    [[nodiscard]] TypeLattice&                  lattice()        noexcept { return lattice_; }
    [[nodiscard]] DiagnosticReporter const&     diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] bool                          hasErrors() const noexcept { return diagnostics_.hasErrors(); }

    // ── scope tree ──
    // Slot 0 is the InvalidScope sentinel; slot 1 is the CU root scope. Real
    // scopes are dense thereafter. `scopes()` returns the vector for tooling;
    // `recordFor(scope)` is the named lookup.
    [[nodiscard]] std::vector<ScopeRecord> const& scopes() const noexcept { return scopes_; }
    [[nodiscard]] ScopeRecord const&              scopeRecord(ScopeId id) const;

    // ── symbol table ──
    [[nodiscard]] std::vector<SymbolRecord> const& symbols() const noexcept { return symbols_; }
    [[nodiscard]] SymbolRecord const*              recordFor(SymbolId id) const noexcept;

    // ── side-table queries ──
    // `symbolAt(nodeId)` returns the SymbolId bound to a name-node (a
    // declaration's name OR a resolved use). InvalidSymbol when nothing was
    // bound. Aborts via UnitAttribute's CU guard if `nodeId` is not from a
    // tree in this CU.
    [[nodiscard]] SymbolId symbolAt(NodeId id) const;
    [[nodiscard]] TypeId   typeAt(NodeId id)   const;

    // FC16 (D-CSUBSET-GENERIC-SELECTION): for a `_Generic` node, the NodeId of
    // the selected association's result-expression (the compile-time type-match
    // winner Pass 2 recorded). Returns InvalidNode for any node that is not a
    // successfully-selected `_Generic` (incl. a no-match/ambiguous one, which the
    // analyzer left untyped + errored). The CST→HIR `lowerGeneric` reads this to
    // lower ONLY the selected sub-expression.
    [[nodiscard]] NodeId   selectedGenericExpr(NodeId id) const;

    // Reverse use-index (SE7): every NodeId that resolved to `symbol`
    // during Pass 2 (the symbol's USE sites — NOT its declaration name
    // node). Returns an empty span for an unknown / never-used symbol.
    // Powers LSP references / rename.
    [[nodiscard]] std::span<NodeId const> usesOf(SymbolId symbol) const noexcept;

    // SP3.a: TypeId→declaring-struct-scope substrate. Composite types
    // (struct/union) carry an associated inner scope holding their
    // field symbols (populated by Pass 1.5 when the struct's TypeId is
    // interned). Returns `InvalidScope` for non-composite types or for
    // composites whose scope didn't get populated (semantic-phase
    // failure). Used by D5.3 designator-position name resolution
    // (look up `.x` in the struct's scope derived from the context
    // type, not the lexical scope) and by future MemberAccess refactors
    // that want a uniform substrate.
    [[nodiscard]] ScopeId compositeScopeFor(TypeId type) const noexcept;

    // R2 (D-SEMANTIC-NULL-CONSTANT-FOLDING): true iff `id` is a source node the
    // analyzer admitted as a FOLDED null-pointer constant (a non-literal integer
    // constant expression with value 0 — `1-1`, `-0`). The CST→HIR lowerer
    // materializes a synthetic Literal 0 in its place so the existing coerce
    // literal-0 arm emits the Cast→Ptr. False for every other node (incl. a
    // structural literal `0`, which the coerce arm admits directly).
    [[nodiscard]] bool isNullPointerConstant(NodeId id) const {
        return nullPointerConstantNodes_.has(id);
    }

    // The full attributes — convenient for tooling / forEach iteration.
    [[nodiscard]] UnitAttribute<SymbolId> const& nodeToSymbol() const noexcept { return nodeToSymbol_; }
    [[nodiscard]] UnitAttribute<TypeId>   const& nodeToType()   const noexcept { return nodeToType_; }

    // FF11 shipped-lib descriptor externs the semantic phase minted (one per
    // injected descriptor symbol; goal-2-skipped names are absent). The CST→HIR
    // lowerer reads this to synthesize the matching extern HIR nodes +
    // HirExternRecords. Empty unless the CU resolved a shipped-lib descriptor.
    [[nodiscard]] std::span<ShippedExternSymbol const> shippedExterns() const noexcept {
        return shippedExterns_;
    }

    // c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): the per-format library map of
    // a shipped descriptor symbol that GOAL-2 SUPPRESSED because a user
    // declaration claimed the name (shell.c bare-declares `popen` while also
    // `#include <stdio.h>`). Read by the CST→HIR bare-proto extern synthesis so
    // the user's prototype re-binds the descriptor's import library instead of
    // surviving to the linker as an undefined symbol. Availability-gated +
    // first-wins at record time (exactly mirroring injection). Returns nullptr
    // when no suppressed descriptor symbol carries this name.
    [[nodiscard]] std::unordered_map<std::string, std::string> const*
    suppressedShippedLibraryFor(std::string const& name) const noexcept {
        auto const it = suppressedShippedLibraries_.find(name);
        return it == suppressedShippedLibraries_.end() ? nullptr : &it->second;
    }

    // FC3 c1: the data model this analysis ran under (`analyze()`'s
    // parameter — the active format's declared width triple). The HIR
    // lowering reads THIS (never a second parameter), so the two tiers'
    // ladder / UAC resolutions agree by construction.
    [[nodiscard]] DataModel dataModel() const noexcept { return dataModel_; }

private:
    std::shared_ptr<CompilationUnit const> cu_;
    TypeLattice                            lattice_;
    std::vector<ScopeRecord>               scopes_;
    std::vector<SymbolRecord>              symbols_;
    UnitAttribute<SymbolId>                nodeToSymbol_;
    UnitAttribute<TypeId>                  nodeToType_;
    // FC16 (D-CSUBSET-GENERIC-SELECTION): `_Generic` node → selected assoc's
    // result-expression NodeId. See `selectedGenericExpr`.
    UnitAttribute<NodeId>                  nodeToSelectedExpr_;
    DiagnosticReporter                     diagnostics_;
    // SymbolId.v → its USE-site NodeIds. Built once during analyze().
    std::unordered_map<std::uint32_t, std::vector<NodeId>> usesBySymbol_;
    // SP3.a: composite-TypeId.v → declaring-struct-scope. Populated by
    // Pass 1.5 when a struct's TypeId is interned (see
    // `compositeScopeByType` in semantic_analyzer.cpp's EngineState).
    std::unordered_map<std::uint32_t, ScopeId>             compositeScopeByType_;
    // R2 (D-SEMANTIC-NULL-CONSTANT-FOLDING): source nodes the analyzer admitted as
    // a FOLDED null-pointer constant (`1-1`, `-0`). The CST→HIR lowerer reads
    // `isNullPointerConstant` to materialize a synthetic Literal 0 in place.
    // TREE-KEYED UnitAttribute (NodeId is tree-local — a flat set would alias node
    // indices across a multi-source CU's trees → cross-tree silent miscompile).
    UnitAttribute<bool>                                   nullPointerConstantNodes_;
    // FF11: descriptor externs minted from resolved shipped-lib JSON
    // descriptors (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC). Consumed by the
    // CST→HIR lowerer.
    std::vector<ShippedExternSymbol>                       shippedExterns_;
    // c86: name → per-format library map for goal-2-SUPPRESSED shipped
    // descriptor symbols (see `suppressedShippedLibraryFor`).
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
                                                           suppressedShippedLibraries_;
    // FC3 c1: the analysis-time data model (see `dataModel()`).
    DataModel                                              dataModel_ = DataModel::Lp64;
};

// Pin move-only / non-copyable at compile time so a future refactor
// can't silently make the model copyable (the side-tables would then
// duplicate their per-tree NodeAttribute storage, breaking the
// shared_ptr<CU>-anchors-the-raw-Tree*-pointers invariant).
static_assert(!std::is_copy_constructible_v<SemanticModel>,
              "SemanticModel must be move-only — the side-tables hold raw "
              "Tree* into the bound CU; copying would silently alias them.");
static_assert(!std::is_copy_assignable_v<SemanticModel>,
              "SemanticModel must be move-only.");
static_assert(std::is_move_constructible_v<SemanticModel>);

} // namespace dss
