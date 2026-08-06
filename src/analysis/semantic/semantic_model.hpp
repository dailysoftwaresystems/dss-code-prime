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
    // TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME — GNU/Clang ASM LABEL,
    // GCC 6.47.5 "Controlling Names Used in
    // Assembler Code"): the EXPLICIT assembler name written on this symbol's
    // declarator — `int f(void) __asm("_myname");`. EMPTY (the overwhelmingly
    // common case) means "no label; derive the on-binary name from `name` the
    // usual way".
    //
    // THE CONTRACT, MEASURED against /usr/bin/clang on arm64-darwin: when set,
    // this string IS the on-binary symbol name, VERBATIM — the format's C
    // mangling (`applyCMangling`) is BYPASSED, not applied on top. `int gv
    // __asm("myglobal");` emits `myglobal`, not `_myglobal`; that is exactly why
    // the macOS SDK's `__DARWIN_ALIAS` family writes its own leading underscore
    // (`__asm("_" __STRING(sym) …)`). The in-tree precedent for a pre-decorated,
    // never-re-mangled name is a format descriptor's `importMangledName`
    // (macho64-arm64-darwin-exec.format.json ships `"_exit"` and
    // entry_trampoline.cpp uses it verbatim).
    //
    // ★ IT LIVES ON THE SYMBOL, NOT ON HIR, AND THAT IS FORCED. The two rails
    // that turn a symbol into an emitted name — `compile_pipeline`'s `nameOf`
    // and `program.cpp`'s cross-CU merge key — read `SemanticModel`, never HIR.
    // A HIR side-table (the `HirLinkageMap` / `HirAlignmentMap` shape) would be
    // structurally INVISIBLE to both, and a label that never reaches them is a
    // parse-and-ignore rename: clean compile, wrong symbol, no diagnostic.
    //
    // ★ IT SURVIVES THE REDECLARATION MERGE. MEASURED: a label on a PROTOTYPE
    // renames the later DEFINITION (`int deffn(int) __asm("mydeffn"); int
    // deffn(int x){…}` emits `mydeffn`), so `mergeOrCollideRedeclaration` carries
    // a non-empty label from the absorbed declaration onto the survivor.
    std::string asmName;
    // TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): the per-target LINK
    // BASE NAME a SHIPPED-LIBRARY DESCRIPTOR declared for this symbol, already
    // resolved for the active (arch, format) and UNDECORATED. EMPTY for every
    // user-written declaration and for every descriptor row that does not opt in.
    //
    // ★ IT IS NOT `asmName` ABOVE, AND THE DIFFERENCE IS THE MANGLING. `asmName`
    // is the user's C `__asm("x")`: C says that string IS the symbol, so it
    // BYPASSES `applyCMangling`. This is DSS's answer to "which base name does
    // the shipped library export on this target", so it is the INPUT to
    // `applyCMangling` — `linkName:"fstat$INODE64"` must reach Mach-O as
    // `_fstat$INODE64`, with the `_` supplied by the FORMAT rule and not by
    // config. Both are folded into the ONE naming function `ffi::linkNameFor`,
    // which is where their precedence (asmName > linkName > name) is stated.
    //
    // ★ IT LIVES HERE FOR THE SAME FORCED REASON `asmName` DOES: `compile_
    // pipeline`'s `nameOf` and `program.cpp`'s cross-CU merge key read
    // `SemanticModel`, never HIR. The import rail carries the identical string
    // (ShippedExternSymbol → HirExternRecord → ExternDeclRef), and BOTH rails
    // must hand `linkNameFor` the same inputs or `mir_merge`'s
    // `definedNames.count(e.mangledName)` misses and an intra-image call is
    // silently emitted as a dynamic import.
    //
    // ★ IT NEEDS NO REDECLARATION-MERGE CARRY, unlike `asmName` above, and the
    // reason is structural rather than an oversight: descriptor injection is
    // gated on `userDeclaredNames`, which is a WHOLE-TU, POSITION-BLIND name set
    // — so a symbol carrying a `linkName` was, by construction, never declared by
    // the user anywhere in the TU and can never reach
    // `mergeOrCollideRedeclaration`. The case that DOES arise — a user
    // prototype SUPPRESSING a descriptor row — is served by
    // `SuppressedShippedSymbol::linkName`, not by a merge.
    std::string linkName;
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
    // C34c (D-CSUBSET-FN-TYPEDEF-PROTOTYPE): TRUE iff this symbol was minted from a
    // bare, UNDECORATED-name object declaration whose head is a TYPE-NAME reference
    // (a potential typedef) — e.g. `Fn foo;` / `Tcl_ObjCmdProc foo;`. Such a
    // declaration IS a function PROTOTYPE (C 6.7 / 6.9.1p2: `T x;` where T is a
    // function type declares a function) WHEN that type-name resolves to a function
    // type — but the function-ness is unknowable in Pass 1 (a shipped-descriptor
    // typedef is not injected until AFTER Pass 1, and user typedefs resolve their
    // type only in Pass 1.5). So Pass 1 marks the declaration a CANDIDATE: it is
    // treated as Function-category for a same-name redeclaration merge (so a proto +
    // its definition MERGE instead of colliding), and Pass 1.5 upgrades it to a real
    // Function proto once the type resolves to a FnSig. The post-1.5 sweep VERIFIES:
    // if the resolved type is NOT a function (a genuine object-vs-function clash such
    // as `MyInt foo; int foo(){}`), the deferred S_RedeclaredSymbol fires there.
    // Distinct from `isProtoDeclaration` (a SYNTACTIC `name()` proto, known in
    // Pass 1); only ever set on an object-declaration row (never a struct field or a
    // parameter). Default false.
    bool            maybeFnTypedefProto = false;
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
    // TF-C78 (D-CSUBSET-NOINLINE): TRUE iff this FUNCTION symbol is declared
    // `__attribute__((noinline))` (GNU; no C11/C23 standard spelling). Set at
    // Pass-1.5 declarator resolution from the `attributeSemantics` table's
    // `noInline` effect verb — NOT from a hardcoded name test — gated on the
    // declared type being a FnSig (the `isNoreturn` discipline: the attribute on
    // a non-function is inert rather than wrongly recorded). OR-merged across a
    // proto/definition pair by the post-1.5 `mergedFnDecls` sweep, so a call —
    // which resolves to the DEFINITION — still sees a flag only the PROTOTYPE
    // spelled (sqlite declares `SQLITE_NOINLINE` on both, but glibc-style headers
    // often annotate the prototype alone).
    //
    // Projected onto the `HirNoInlineMap` side-table at CST→HIR lowering and
    // stamped onto `MirFunc.noInline` at HIR→MIR, where the inliner's §2.9
    // legality gate refuses to splice the callee.
    //
    // ★ A DROPPED FLAG IS NOT A SAFE MISS HERE — unlike `isNoreturn` (whose loss
    // yields a loud H_VerifierFailure), losing this one means the function gets
    // INLINED, silently, which is exactly what the source forbade. That is why it
    // is propagated through every MirFunc copy/rebuild path rather than only the
    // lowering that mints it. Default false.
    bool            isNoInline = false;
    // TF-C81 (D-CSUBSET-ALWAYSINLINE): TRUE iff this FUNCTION symbol is declared
    // `__attribute__((always_inline))` (GNU; no C11/C23 standard spelling). The
    // exact structural mirror of `isNoInline` above — set at Pass-1.5 declarator
    // resolution from the `attributeSemantics` table's `alwaysInline` effect verb
    // (never a hardcoded name test), FnSig-gated, OR-merged across a proto/
    // definition pair by the post-1.5 `mergedFnDecls` sweep, projected onto
    // `HirAlwaysInlineMap` and stamped onto `MirFunc.alwaysInline`.
    //
    // ★ WHAT IT BUYS, STATED NARROWLY: the inliner's §2.9 gate skips its
    // SIZE-BASED cost model (rule 6) for this callee. It does NOT override any
    // correctness refusal, and it does nothing at all under a pipeline with no
    // `Inlining` pass. A DROPPED flag is therefore a PERFORMANCE miss, never a
    // miscompile — the opposite direction from `isNoInline`, whose loss is a
    // silent violation of the source's directive. It is nevertheless propagated
    // through every MirFunc copy/rebuild path, because a half-landed flag and no
    // flag are indistinguishable in the emitted binary (TF-C78's finding).
    //
    // ★ MUTUALLY EXCLUSIVE WITH `isNoInline` AT THE SOURCE TIER: a declaration —
    // or a proto/definition pair — carrying both is a LOUD
    // S_ConflictingInlineAttributes, so both bits are never set together on a
    // record that survives semantic analysis. Default false.
    bool            isAlwaysInline = false;
    // TF-C92 (D-CSUBSET-NO-SANITIZE-THREAD): TRUE iff this FUNCTION symbol is
    // declared `__attribute__((no_sanitize_thread))` (GNU; no C11/C23 standard
    // spelling). The structural twin of `isNoInline` above — set at Pass-1.5
    // declarator resolution from the `attributeSemantics` table's
    // `noSanitizeThread` effect verb (never a hardcoded name test), FnSig-gated,
    // OR-merged across a proto/definition pair by the post-1.5 `mergedFnDecls`
    // sweep, projected onto `HirNoSanitizeThreadMap` and stamped onto
    // `MirFunc.noSanitizeThread`.
    //
    // ★ WHAT IT BUYS, STATED NARROWLY AND WITHOUT INVENTING A CONSUMER: the fact
    // is RECORDED and stays queryable through the whole tier stack, surfacing in
    // `.dssir` MIR text as the `nosanitizethread` function attribute. DSS has NO
    // sanitizer — MEASURED, `grep -rni sanitiz src/` returns nothing — so there is
    // no instrumentation pass for the flag to switch off today, and this record is
    // deliberately NOT described as suppressing one. It exists so the day such a
    // pass lands it has a per-function input instead of a discarded attribute.
    //
    // ★ WHY OR-MERGED LIKE `isNoInline` AND NOT PER-DECLARATION LIKE
    // `isNoOptimize`: this is an ATTRIBUTE ON THE DECLARATION, not a lexical
    // preprocessor region, so the ordinary C shape — annotate the prototype in a
    // header, define plainly in the .c — must reach the DEFINITION's symbol,
    // which is the one HIR→MIR stamps from. (sqlite's own two sites annotate the
    // definition directly, so the merge is not what makes sqlite work; it is what
    // makes the glibc-style split work.)
    //
    // ★ NOT MUTUALLY EXCLUSIVE WITH ANYTHING. Unlike the inline pair above there
    // is no contradicting partner attribute to gate — `no_sanitize_thread`
    // composes freely with `noinline`, `always_inline` and the pragma-borne
    // `isNoOptimize`. Default false.
    bool            isNoSanitizeThread = false;
    // ★★ TF-C85: TRUE iff this FUNCTION symbol's declaration sits inside an MSVC
    // `#pragma optimize("", off)` region. Its two neighbours above come from an
    // ATTRIBUTE on the declaration; this one comes from a LEXICALLY SCOPED
    // PREPROCESSOR REGION, so it is set from the token-offset stamps
    // (`State::noOptimizeAtOffset`, keyed on the declaration's leftmost EMITTED
    // token) rather than from the `attributeSemantics` fold. Everything DOWNSTREAM
    // of this record is identical to `isNoInline`'s route: projected onto
    // `HirNoOptimizeMap` at CST→HIR and stamped onto `MirFunc.noOptimize` at
    // HIR→MIR, where the optimizer's rebuild seams read it.
    //
    // ★ WHAT IT BUYS, STATED NARROWLY AND HONESTLY. Every pass in the shipped
    // release pipeline is semantics-PRESERVING, so this flag changes performance,
    // never behavior — MEASURED, nothing in this tree can perturb float
    // arithmetic (integer-only const-fold maps, no reassociation, no FMA/fast-math,
    // `double` is SSE2 at exactly 64 bits). It exists because the source states a
    // directive and DSS should honor what it records, and because it is what lets
    // the pe64 corpus leg stop failing on an unclaimed pragma. It is NOT a fix for
    // a live floating-point miscompile and must never be described as one.
    //
    // ★ NOT merged across a proto/definition pair, unlike its two neighbours, and
    // deliberately: MSVC's pragma applies to functions DEFINED in the region, so a
    // PROTOTYPE that happens to fall inside one says nothing about where the
    // definition lives. Leaving each record to answer for its own declaration
    // means a stray prototype cannot silently de-optimize a definition compiled
    // elsewhere in the file — and in the shape where both land in the region (the
    // sqlite case) both records carry it anyway. Default false.
    bool            isNoOptimize = false;
    // TF-C79 (D-CSUBSET-INLINE-FUNCTION-SPECIFIER, C99 6.7.4): TRUE iff THIS
    // declaration of a FUNCTION symbol spells the `inline` specifier WITHOUT
    // `extern` — 6.7.4p7's exact clause, not merely "the word inline appears".
    // Set at Pass-1.5 declarator resolution from `cfg.inlineKeywordToken` /
    // `cfg.inlineExternSpecifierTokens` — never a hardcoded spelling — gated on
    // the declared type being a FnSig (the `isNoInline` discipline); the
    // non-function case is a 6.7.4p1 constraint violation reported loud as
    // S_InlineNonFunction rather than recorded here.
    //
    // ★ **AND**-MERGED across a proto/definition pair by the post-1.5
    // `mergedFnDecls` sweep — the one flag on this record that is not OR-merged,
    // and the asymmetry is the C standard's, not a preference. 6.7.4p7 makes a
    // definition an INLINE definition (providing NO external definition) only
    // when ALL of the file-scope declarations spell inline-without-extern, so a
    // single plain `int f(int);` beside `inline int f(int){…}` restores the
    // external definition. OR-merging would decide the opposite on exactly the
    // two commonest shapes (an inline prototype with a plain definition, and a
    // plain prototype with an inline definition) — MEASURED, clang emits `T _f`
    // for both.
    //
    // Read at CST→HIR: a file-scope function DEFINITION whose symbol carries
    // this flag with a Global binding is lowered as an `ExternFunction`
    // DECLARATION instead of a `Function`, so nothing is emitted for it and the
    // reference resolves against a sibling CU (or fails loud K_SymbolUndefined).
    // A `static inline` keeps its Local binding and IS emitted — 6.7.4p7
    // constrains external linkage only. Default false.
    bool            isInline = false;
    // FC17.9(c) (D-CSUBSET-SETJMP): TRUE iff this FUNCTION symbol "returns more than
    // once" (C11 7.13.1.1 — `setjmp`/`_setjmp`: a matching `longjmp` makes the setjmp
    // call appear to return a SECOND time). There is NO source syntax for it (unlike
    // `_Noreturn`); it rides ONLY a shipped-lib descriptor's `returnsTwice`
    // (setjmp.json), threaded here at descriptor injection — the `isNoreturn`-from-
    // descriptor mirror. Read at HIR->MIR lowering, where a DIRECT call to such a
    // callee stamps `MirInstFlags::ReturnsTwice` on the emitted `Call` (via a CST->HIR
    // side-table, the `isVolatile` funnel discipline). That MIR flag — NOT this
    // semantic bit — is what the optimizer's returns-twice-aware passes consult
    // (noreturn is HIR-discharged and never reaches MIR; returnsTwice MUST, so it needs
    // the carrier). A DROPPED flag is a conservative miss for the WALKING-SKELETON
    // (determinate cases already work — a Call is a memory barrier and longjmp restores
    // callee-saved+SP), never a silent miscompile. Default false.
    bool            returnsTwice = false;
    // D-LANG-FFI-DESCRIPTOR-INT-POINTEE-COMPAT: TRUE iff this FUNCTION symbol was
    // MINTED from a resolved shipped-library FFI descriptor (tcl.json, stdio.json,
    // …) — the descriptor-injection sibling of `isNoreturn`/`returnsTwice`. Set
    // ONLY in the shipped-lib injection loop for `kind==Function`; the SEPARATE
    // builtin/intrinsic injection loop deliberately leaves it FALSE (both loops use
    // `tree==InvalidTree`, so that field cannot distinguish them — this flag is the
    // discriminator that keeps `_InterlockedCompareExchange`'s `int*`-pointee reject
    // pinned). Read ONLY by `checkCallAgainstSig` at the DIRECT-symbol call site to
    // decide whether the config-gated integer-pointee pointer-arg relaxation
    // (`ptr<i64>` accepting a same-representation `long long*`/`long*`-on-LP64) may
    // fire AT THE CALL-ARG BOUNDARY (never native C-to-C, never init/assign/return).
    // Default false → every non-shipped callee stays strict.
    bool            isShippedDescriptorFn = false;
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
    // c156 (D-LK-ELF-SYMBOL-VERSIONING): the REQUIRED ELF symbol version
    // (e.g. "GLIBC_2.3"), already resolved for the active (arch, format) by the
    // descriptor reader's per-target `version` variant. Empty ⇒ unversioned.
    // Carried verbatim to HirExternRecord.version → the ELF writer's
    // .gnu.version_r; ELF-only (unused on PE/Mach-O).
    std::string version;
    // TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): the descriptor's
    // per-target LINK BASE NAME, already resolved for the active (arch, format)
    // — UNDECORATED (`fstat$INODE64`, never `_fstat$INODE64`). EMPTY ⇒ the
    // canonical `name`. Carried verbatim to HirExternRecord.linkName →
    // ExternDeclRef.linkName → `ffi::linkNameFor`, which applies the FORMAT's
    // decoration to it exactly as it would to `name`. The SAME string is also
    // stamped on this symbol's `SymbolRecord.linkName` at injection so the
    // definition rail and the import rail feed `linkNameFor` identical inputs
    // (the byte-for-byte agreement its header documents).
    std::string linkName;
};

// c86 + c156: the link identity of a goal-2 SUPPRESSED shipped descriptor
// symbol. When a user BARE PROTOTYPE re-declares a shipped name (e.g.
// `extern char *realpath(const char*, char*);` over `#include <stdlib.h>` — a
// common feature-test-macro pattern), goal-2 suppresses the descriptor's own
// injection and the user's prototype synthesizes the import instead. BOTH the
// per-format library map AND the required ELF symbol version must ride here, or
// the synthesized import loses its version and ld.so silently misbinds an
// unversioned reference to the library's OLDEST compat instance (the exact
// D-LK-ELF-SYMBOL-VERSIONING realpath@GLIBC_2.2.5 bug the descriptor path
// fixes). Availability-gated + first-wins at record time (mirroring injection).
//
// TF-C112 (D-FFI-PE-CRT-UCRT-MIGRATION): and so must the row's REALIZATION —
// its `synthesize` recipe id plus the declared signature that recipe answers
// to. A suppressed row is the ONLY channel by which a descriptor symbol reaches
// the link WITHOUT passing through `SemanticModel::shippedExterns()`, so every
// property the injected path reads off a row has to ride here too or that
// property is silently dropped for exactly the declarations users write most.
struct DSS_EXPORT SuppressedShippedSymbol {
    // The descriptor's per-object-format `library` map ("pe"/"elf"/"macho" →
    // runtime image), carried verbatim; folded to one string per target
    // downstream (compile_pipeline), exactly like an injected extern.
    std::unordered_map<std::string, std::string> library;
    // The required ELF symbol version, already resolved for the active target
    // (e.g. "GLIBC_2.3"); EMPTY ⇒ unversioned (D-LK-ELF-SYMBOL-VERSIONING).
    std::string version;
    // TF-C112 (D-FFI-PE-CRT-UCRT-MIGRATION): the suppressed row's `synthesize`
    // RECIPE id (`ShippedExternSymbol::recipeId`'s sibling), or EMPTY for an
    // ordinary FFI-import row. ★ THIS FIELD IS THE FIX FOR A HARD LOAD FAILURE,
    // not a convenience: the LIBRARY and the REALIZATION are two independent
    // properties of a descriptor row, and carrying only the first is what made
    // the pe UCRT flip lethal on the redeclaration path. `ucrtbase.dll` exports
    // NONE of printf/fprintf/sprintf/vfprintf/sscanf — only the
    // `__stdio_common_v*` cores — so those five rows are COMPILER-SYNTHESIZED
    // shims, not imports. Pre-flip the same rows bound `msvcrt.dll`, which does
    // export all five, so a suppressed row re-exported as a plain import was
    // inert; post-flip it plants an `ExternImport{printf, ucrtbase.dll}` that
    // the loader rejects with 0xC0000139 (D-FFI-DESCRIPTOR-EAGER-IMPORT) — at
    // PROCESS START, with rc=0 and no diagnostic at any compile stage.
    // MEASURED at TF-C112 HEAD on the three-line reproducer
    // (`#include <stdio.h>` + `int printf(const char*, ...);` + one call).
    std::string recipeId;
    // TF-C112 (D-FFI-PE-CRT-UCRT-MIGRATION): the descriptor row's DECLARED
    // signature, interned in THIS model's lattice (the `ShippedExternSymbol::
    // signature` discipline — the same interner the CST→HIR lowerer lowers
    // through, so a TypeId comparison against a user prototype's resolved type
    // is meaningful). It is the SHIM-COMPATIBILITY ORACLE: the synth pass emits
    // one FIXED body per recipe id, matching this row, so a user prototype that
    // suppresses a recipe row may inherit the shim ONLY if it agrees with this
    // signature — otherwise the call would be made under the user's ABI and
    // answered under the descriptor's. Carried for EVERY suppressed row (a
    // recipe-less row's copy is simply unread today) so the field's meaning is
    // "what the descriptor declared", never "what one consumer needed".
    TypeId signature;
    // TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): the suppressed row's
    // per-target LINK BASE NAME (`ShippedSymbol::linkName`, already resolved for
    // the active target), or EMPTY. Rides here for the SAME reason `version`
    // does, one field family up: a user bare prototype of `fstat` over
    // `#include <sys/stat.h>` — the feature-test pattern real code writes — must
    // still import Darwin-x86_64's `_fstat$INODE64`, or the suppression silently
    // reinstates the exact 32-bit-inode misbinding this field exists to kill.
    // The descriptor is the authority on which name the LIBRARY exports; a user
    // prototype restates the C signature, never the platform's link identity.
    std::string linkName;
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
                  UnitAttribute<bool>                    ffiIntPointeeCompatNodes,
                  std::vector<ShippedExternSymbol>       shippedExterns,
                  std::unordered_map<std::string, SuppressedShippedSymbol>
                                                         suppressedShippedLibraries,
                  DataModel                              dataModel,
                  LongDoubleFormat                       longDoubleFormat) noexcept
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
          ffiIntPointeeCompatNodes_(std::move(ffiIntPointeeCompatNodes)),
          shippedExterns_(std::move(shippedExterns)),
          suppressedShippedLibraries_(std::move(suppressedShippedLibraries)),
          dataModel_(dataModel),
          longDoubleFormat_(longDoubleFormat) {}

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

    // D-LANG-FFI-DESCRIPTOR-INT-POINTEE-COMPAT: true iff `id` is a call-ARG source
    // node the analyzer admitted via the shipped-FFI-descriptor integer-pointee
    // pointer relaxation (a real C integer pointer — `long long*` etc. — passed to
    // a descriptor `ptr<i64>`-style param whose pointee is same-REPRESENTATION but
    // distinct-IDENTITY). The CST→HIR `coerce()` reads this to realize the Ptr→Ptr
    // bitcast that retypes the arg to the param type (admit⟺realize parity, the
    // `isNullPointerConstant` precedent). False for every other node — the mark is
    // set ONLY when the relaxation was WHAT admitted the arg (strict-fail-then-relax-
    // succeed), so a strictly-compatible arg is never marked. Callers MUST guard
    // `id.valid()` before calling (the UnitAttribute routes by arenaTag; an untagged
    // InvalidNode is ambiguous in a multi-tree CU).
    [[nodiscard]] bool isFfiIntPointeeCompat(NodeId id) const {
        return ffiIntPointeeCompatNodes_.has(id);
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

    // c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS) + c156 (D-LK-ELF-SYMBOL-
    // VERSIONING): the link identity (per-format library map + required ELF
    // version) of a shipped descriptor symbol that GOAL-2 SUPPRESSED because a
    // user declaration claimed the name (shell.c bare-declares `popen` while
    // also `#include <stdio.h>`; the versioned case is `realpath` over `#include
    // <stdlib.h>`). Read by the CST→HIR bare-proto extern synthesis so the
    // user's prototype re-binds the descriptor's import library AND its symbol
    // version instead of surviving unversioned (a silent realpath@GLIBC_2.2.5
    // misbind) or as an undefined symbol. Availability-gated + first-wins at
    // record time (exactly mirroring injection). Returns nullptr when no
    // suppressed descriptor symbol carries this name.
    // TF-C112 (D-FFI-PE-CRT-UCRT-MIGRATION): the same reader also asks whether
    // the suppressed row carried a `synthesize` RECIPE — a row realized as a
    // compiler-emitted shim must not be re-exported as a raw import merely
    // because the user re-declared its name (`ucrtbase.dll` exports no bare
    // `printf`, so that import fails the LOAD at 0xC0000139).
    [[nodiscard]] SuppressedShippedSymbol const*
    suppressedShippedSymbolFor(std::string const& name) const noexcept {
        auto const it = suppressedShippedLibraries_.find(name);
        return it == suppressedShippedLibraries_.end() ? nullptr : &it->second;
    }

    // FC3 c1: the data model this analysis ran under (`analyze()`'s
    // parameter — the active format's declared width triple). The HIR
    // lowering reads THIS (never a second parameter), so the two tiers'
    // ladder / UAC resolutions agree by construction.
    [[nodiscard]] DataModel dataModel() const noexcept { return dataModel_; }

    // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): the `long double` axis this analysis
    // ran under (`analyze()`'s parameter — the active format's declared
    // representation, or None). The HIR lowering reads THIS (never a second
    // parameter), so the two tiers' float-literal-ladder resolutions agree by
    // construction — the dataModel() discipline.
    [[nodiscard]] LongDoubleFormat longDoubleFormat() const noexcept {
        return longDoubleFormat_;
    }

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
    // D-LANG-FFI-DESCRIPTOR-INT-POINTEE-COMPAT: call-arg source nodes the analyzer
    // admitted via the shipped-descriptor integer-pointee pointer relaxation. The
    // CST→HIR lowerer reads `isFfiIntPointeeCompat` to materialize the Ptr→Ptr
    // bitcast retyping the arg to the param type. TREE-KEYED UnitAttribute for the
    // same cross-tree-aliasing reason as `nullPointerConstantNodes_`.
    UnitAttribute<bool>                                   ffiIntPointeeCompatNodes_;
    // FF11: descriptor externs minted from resolved shipped-lib JSON
    // descriptors (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC). Consumed by the
    // CST→HIR lowerer.
    std::vector<ShippedExternSymbol>                       shippedExterns_;
    // c86 + c156: name → {library map, required ELF version} for goal-2-
    // SUPPRESSED shipped descriptor symbols (see `suppressedShippedSymbolFor`).
    std::unordered_map<std::string, SuppressedShippedSymbol>
                                                           suppressedShippedLibraries_;
    // FC3 c1: the analysis-time data model (see `dataModel()`).
    DataModel                                              dataModel_ = DataModel::Lp64;
    // FC17.9(e): the analysis-time long-double axis (see `longDoubleFormat()`).
    LongDoubleFormat                                       longDoubleFormat_ =
        LongDoubleFormat::None;
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
