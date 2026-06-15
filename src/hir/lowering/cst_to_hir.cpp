#include "hir/lowering/cst_to_hir.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/type_rules.hpp"      // FC3 c1: usualArithmeticCommonType / resolveArithmeticRules
#include "core/types/data_model.hpp"
#include "core/types/decl_prefix_strip.hpp"     // declRoleChildren / descendVisibleDecl / specifierPrefixChild
#include "core/types/declarator_walk.hpp"       // FC4: collectDeclarators / declaratorNameNode
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/char_decode.hpp"
#include "core/types/hir_lowering_config.hpp"
#include "core/types/integer_literal_ladder.hpp" // FC3 c1: the C 6.4.4.1 ladder (shared with the semantic tier)
#include "core/types/object_format_kind.hpp"     // kObjectFormatKindTable (per-format library-map keys)
#include "core/types/number_decode.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"             // isEmptySpace
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/const_eval_arith.hpp"
#include "hir/cst_const_eval.hpp"
#include "hir/hir_op.hpp"
#include "hir/hir_verifier.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <memory>
#include <unordered_map>
#include <vector>

// The single language-agnostic CST→HIR engine (plan 09 HR8). Reads the schema's
// `hirLowering` + `semantics` config and lowers each tree's CST to HIR via the
// HirBuilder, inferring each expression node's result type as it goes (the
// semantic phase types literals/refs/calls but not operator result nodes — per
// plan §2.4 lowering populates typeId per node). Never branches on schema.name().

namespace dss {

namespace {

// Core operator name → HirOpKind (reverse of opName()); std::nullopt if not a
// core op. Used to resolve the config's `target` strings.
[[nodiscard]] std::optional<HirOpKind> coreOpFromName(std::string_view s) {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(HirOpKind::Count_); ++i) {
        auto const op = static_cast<HirOpKind>(i);
        if (opName(op) == s) return op;
    }
    return std::nullopt;
}

// `isComparison` lives in `hir/hir_op.hpp` — one source of truth across HR
// lowering's `combineBinary`, MIR's `mapBinaryOp`, and the constants-eval
// engine's BinaryOp branch. A new comparison-shaped op (e.g. `Spaceship`)
// would otherwise need updates in all three sites.

// decodeInteger + decodeFloat live in core/types/number_decode.hpp —
// shared so a literal's text is interpreted identically everywhere.
// (FC1 cycle 2, 2026-06-10: decodeFloat was hoisted from here; its old
// local body stripped EVERY 'f'/'F' char — a hardcoded C-ism that
// value-corrupted hex-float mantissas like `0x1.fp3`. The shared one
// strips only a trailing DECLARED suffix.)

[[nodiscard]] bool isSignedCore(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::U8: case TypeKind::U16: case TypeKind::U32:
        case TypeKind::U64: case TypeKind::U128: return false;
        default: return true;
    }
}
[[nodiscard]] bool isFloatCore(TypeKind k) noexcept {
    return k == TypeKind::F16 || k == TypeKind::F32 || k == TypeKind::F64 || k == TypeKind::F128;
}

// Arithmetic-kind predicate — the implicit-conversion surface (ints +
// floats + the int-adjacent Char/Byte/Bool scalars). Shared by `coerce`
// (which conversion pairs may materialize a Cast) and `coerceCondition`
// (which condition types take the `!= 0` truthiness test; Bool is
// early-outed there before this is consulted). A SHAPE predicate over
// TypeKind — never language identity.
[[nodiscard]] bool isArithmeticCore(TypeKind k) noexcept {
    return k == TypeKind::Bool || k == TypeKind::Char || k == TypeKind::Byte
        || k == TypeKind::I8   || k == TypeKind::I16  || k == TypeKind::I32
        || k == TypeKind::I64  || k == TypeKind::I128
        || k == TypeKind::U8   || k == TypeKind::U16  || k == TypeKind::U32
        || k == TypeKind::U64  || k == TypeKind::U128
        || k == TypeKind::F16  || k == TypeKind::F32
        || k == TypeKind::F64  || k == TypeKind::F128;
}

// Full-width-integer return predicate for D-LK10-ENTRY-MAIN-IMPLICIT-
// RETURN. Restricted to I32+ / U32+ because the SysV AMD64 + MS x64
// ABIs do NOT zero-extend sub-32-bit integer returns to the full GPR
// — the trampoline's 64-bit `mov status, returnGprs[0]` would copy
// indeterminate upper bits to the exit syscall arg → non-deterministic
// exit code with no diagnostic. I32-and-up returns are safe because
// `eax`-write zero-extends to `rax` on x86_64 (ISA invariant); 64-bit
// types fill the GPR directly. Sub-i32 entry-fn returns are non-
// conformant C99 (§5.1.2.2.3 requires `int` specifically), so falling
// through to the verifier's loud-fail is the correct outcome.
//
// Placed at file scope alongside `isSignedCore` / `isFloatCore` for
// consistency — this predicate is stateless and reads no member, so
// the per-Lowerer `static` placement was unjustified coupling (type-
// design Q3 fold, 3rd-order audit on 39897eb).
[[nodiscard]] bool isIntegerReturnCore(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::I32: case TypeKind::I64: case TypeKind::I128:
        case TypeKind::U32: case TypeKind::U64: case TypeKind::U128:
            return true;
        default:
            return false;
    }
}

// Model 3 (2026-06-09): a SOURCE-declared `extern "libname" …` override is the
// user's format-INDEPENDENT choice, but `HirExternRecord.libraryOverride` is a
// per-OBJECT-FORMAT map (so a SHIPPED descriptor can route a different image
// per format). Project the single override under EVERY real object-format key
// so the compile-pipeline fold yields it whatever the active target's format —
// preserving the pre-Model-3 behavior exactly. AGNOSTIC: the key set is the
// `kObjectFormatKindTable` vocabulary (skipping the `Unknown` sentinel), never
// a hand-written `{"pe","elf","macho"}` identity list. Empty input → empty map
// (no override → the FFI synthesize stage uses the language default).
[[nodiscard]] std::unordered_map<std::string, std::string>
uniformLibraryMap(std::string lib) {
    std::unordered_map<std::string, std::string> out;
    if (lib.empty()) return out;
    // Deliberately projects the override under EVERY real format key (incl.
    // wasm/spirv, which have no runnable exec path yet) — the per-target fold
    // (compile_pipeline) reads only the ACTIVE format's key, so the extra keys
    // are harmless + future-proof (a new format inherits the override).
    for (auto const& [kind, formatName] : kObjectFormatKindTable.rows) {
        if (kind == ObjectFormatKind::Unknown) continue;
        out.emplace(std::string{formatName}, lib);
    }
    return out;
}

// Post-fold #8 simplifier R2 + code-reviewer I1: the
// `decl.arraySuffix ? decl.arraySuffix->rule : RuleId{}` pattern
// appears at lowerTopLevel (global init walk) AND lowerExternDecl
// (extern-init reject). Stateless — lives at file scope alongside
// the other anon-namespace helpers. Returns `RuleId{}` when the
// language has no array decl form; downstream consumers gate on
// `.valid()` to skip the rule-match.
[[nodiscard]] RuleId
arraySuffixSkipRule(DeclarationRule const& decl) noexcept {
    return decl.arraySuffix ? decl.arraySuffix->rule : RuleId{};
}

// HR11: one Lowerer is a single-LANGUAGE lowering context bound to one schema's
// `cfg`/`sem`/`numberStyle` + its schema-specific rule/token index maps. A
// multi-language CU runs one Lowerer per distinct schema, all sharing the one
// `builder` / `literals` / `spans` (and thus one HIR module + arena + kind
// registry + literal pool + source map), so the whole CU lowers to ONE module.
// A homogeneous CU is the one-Lowerer case. The single-language body below never
// changed for HR11 — only the shared output moved out of the struct.
struct Lowerer {
    SemanticModel&           model;
    HirLoweringConfig const& cfg;
    SemanticConfig const&    sem;
    NumberStyle const*       numberStyle;
    TypeInterner&            interner;
    DiagnosticReporter&      reporter;
    HirBuilder&              builder;    // shared across all per-schema Lowerers
    HirLiteralPool&          literals;   // shared
    Tree const*              t_ = nullptr;

    // pendingSpans (shared): applied to the result's HirSourceMap after finish().
    std::vector<std::pair<HirNodeId, HirSourceLoc>>& spans;
    // FF6 Slice 2 (2026-06-02): shared accumulator for source-
    // declared externs, one record per `lowerExternDecl` call
    // that successfully produced an ExternFunction / ExternGlobal
    // HIR node. Consumed by `compileSingleUnit` via
    // `synthesizeFfiFromSourceDecls` to populate the
    // `HirFfiMap` between HIR and MIR lowering.
    std::vector<HirExternRecord>& externDecls;
    // D-CSUBSET-LINKAGE-SPECIFIERS (pre-OPT7 P1, 2026-06-04): shared accumulator
    // of (decl HIR node → LinkageAttr) pairs derived from each declaration's
    // specifier-prefix subtree. Applied to the result's HirLinkageMap AFTER
    // finish() — the side-table binds to the FROZEN hir, so (exactly like
    // `spans`) it cannot be written during lowering. Only NON-default linkage is
    // recorded (absence ⇒ Global/Default ⇒ externally visible), keeping it sparse.
    std::vector<std::pair<HirNodeId, LinkageAttr>>& linkage;

    // O(1) lookups.
    std::unordered_map<std::uint32_t, std::size_t> ruleMap_;     // RuleId.v → ruleMappings idx
    std::unordered_map<std::uint32_t, std::size_t> declMap_;     // RuleId.v → sem.declarations idx
    std::unordered_map<std::uint32_t, std::size_t> binOp_, unOp_, postOp_;  // SchemaTokenId.v → idx
    std::unordered_map<std::uint32_t, TypeId>      litType_;     // SchemaTokenId.v → core TypeId
    std::unordered_map<std::uint32_t, TypeKind>    litCore_;     // SchemaTokenId.v → TypeKind
    // FC3 c1: keyword literals' config-declared fixed VALUES (`true` →
    // 1). `lowerLiteral` uses the value directly instead of decoding the
    // token text as a number (decodeInteger("true") would silently
    // yield 0). Sparse — only rows declaring `value`.
    std::unordered_map<std::uint32_t, std::int64_t> litFixed_;   // SchemaTokenId.v → value
    std::unordered_map<std::uint32_t, bool>        deferred_;    // RuleId.v of explicitly-deferred rules

    // FC3 c1: the analysis-time data model, read OFF THE MODEL (the
    // `analyze()` parameter travels on the SemanticModel) so this tier
    // can never run under a different model than the semantic tier.
    DataModel dataModel_ = DataModel::Lp64;
    // FC3 c1: the language's usual-arithmetic-conversion rules resolved
    // for `dataModel_`. nullopt (no `arithmeticConversions` block) keeps
    // every combine site on the legacy `TypeInterner::commonType` path
    // EXACTLY (toy / tsql — pinned by their typing-unchanged tests).
    std::optional<ResolvedArithmeticRules> arith_;

    // The common arithmetic type at a binary / ternary / compound-assign
    // combine site: the config-driven C 6.3.1.8 engine when the language
    // declares the block, else the legacy interner rule.
    [[nodiscard]] TypeId commonArithType(TypeId a, TypeId b) {
        if (arith_.has_value()) {
            return usualArithmeticCommonType(interner, a, b, *arith_);
        }
        return interner.commonType(a, b);
    }

    // The enclosing function's declared return type, threaded into `lowerReturn`
    // so a `return expr;` whose `expr.type` differs from the declared type
    // emits an implicit `Cast(expr → declaredType)`. Invalid outside any
    // function body (top-level Module / global initializers).
    TypeId currentReturnType_{};

    // FC5: per-function label namespace — label NAME → a per-function ordinal.
    // Pre-scanned from the function body CST BEFORE lowering (so a forward
    // `goto end;` resolves to a later `end:`), saved/restored around each function
    // body like `currentReturnType_`. A GotoStmt and its target LabelStmt carry
    // the same ordinal in their payload; the MIR lowering maps ordinal → block.
    // Label-namespace validation (duplicate / undefined) is emitted HERE at the
    // label-resolution chokepoint — `prescanLabels` (S_DuplicateLabel) and
    // `lowerGoto` (S_UndefinedLabel) — NOT in a separate semantic pass, since this
    // pre-scan is the single site that collects label names; both errors halt the
    // pipeline before MIR (the HIR-tier error gate), so a downstream consumer
    // never sees an unresolved label.
    std::unordered_map<std::string, std::uint32_t> labelOrdinals_;

    // The result of lowering an expression: the HIR node + its resolved type.
    struct E { HirNodeId id; TypeId type; };

    // Coerce an expression to `target` by emitting a `Cast` when its type
    // differs. Same-type, invalid-target (treat as a pass-through — the
    // semantic phase has likely already flagged the mismatch), or invalid-
    // source all pass through unchanged. Calling this is the single point
    // where HR commits to an implicit-conversion site; the MIR-side `Cast`
    // lowering (cycle C's mapCast) picks the right opcode from the
    // (sourceKind, targetKind) pair. The emitted Cast is aliased to its
    // OPERAND's source-map entry so diagnostics anchored at the synthetic
    // Cast still locate to real source.
    [[nodiscard]] E coerce(E child, TypeId target) {
        if (!target.valid() || !child.type.valid()) return child;
        if (child.type == target) return child;
        TypeKind const ck = interner.kind(child.type);
        TypeKind const tk = interner.kind(target);
        // C-standard array-to-pointer decay (D-LK4-RODATA-PRODUCER-
        // STRING closure, 2026-06-02): `Array<T,N>` rvalue in a
        // `Ptr<T>` context decays to the address of the first
        // element. Emitted as a `Cast` HIR node (the universal
        // type-conversion marker); HIR→MIR's `mapCast` recognizes
        // the Array→Ptr pair and routes to the appropriate
        // materialization (for string literals: synthesize the
        // MirGlobal + emit GlobalAddr; for local arrays: anchored
        // D-LK4-RODATA-PRODUCER-LOCAL-ARRAY-DECAY).
        //
        // Pin the same-element-type rule isAssignable already
        // checked: only matching `Array<T,N>`/`Ptr<T>` qualifies
        // for the decay. The HIR verifier expects the Cast's
        // result type matches `target` here.
        //
        // Anchor: D-LANG-STRUCTURAL-DECAY-OPT-OUT (per-language
        // opt-out when a non-C-family schema wants strict-no-
        // implicit-decay semantics).
        if (ck == TypeKind::Array && tk == TypeKind::Ptr) {
            auto const arrElem = interner.operands(child.type);
            auto const ptrElem = interner.operands(target);
            if (!arrElem.empty() && !ptrElem.empty()
                && arrElem[0] == ptrElem[0]) {
                HirNodeId const decay = builder.makeCast(
                    child.id, target, HirFlags::Synthetic);
                for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
                    if (it->first == child.id) {
                        spans.push_back({decay, it->second});
                        break;
                    }
                }
                return {decay, target};
            }
        }
        // D-LANG-POINTER-VOID-CONVERT (step 13.2, 2026-06-02): when
        // the active language's `pointerConversions` rules admit the
        // direction-specific `Ptr<Void>` ↔ `Ptr<T>` conversion, emit
        // a synthetic `Cast` HIR node. MIR-tier `mapCast` already
        // routes Ptr→Ptr as `Bitcast` (no representation change at
        // runtime) — every tier below MIR sees Ptr as pointer-width
        // uniformly regardless of element type.
        //
        // The two arms are independent because the C++ self-host
        // target distinguishes them: `T* → void*` (lhs is void*, rhs
        // is T*, this is the `tk == Void` arm) is safe widening,
        // permitted by C and C++; `void* → T*` (lhs is T*, rhs is
        // void*, this is the `ck == Void` arm) is unsafe narrowing,
        // permitted by C but FORBIDDEN by C++ (requires explicit
        // cast). c-subset declares both true in
        // `c-subset.lang.json`; default-constructed
        // `PointerConversionRules` has both false (strict typing).
        //
        // Sister rule in `type_rules.hpp::isAssignable` — the
        // semantic phase has already vetoed unsupported directions
        // before reaching here, so this arm fires only when the
        // active language admits the conversion. The `sem` field
        // (Lowerer member, defined near the top of this TU) is
        // bound by reference to the active schema's `SemanticConfig`
        // at Lowerer construction (via `sch.semantics()`); there is
        // no default-constructed fallback path. A schema whose
        // `pointerConversions` block is absent loads as both flags
        // `false` (strict typing) via the optional-field path in
        // `grammar_schema_json.cpp`'s `pointerConversions` loader
        // (search for the `if (sem.contains("pointerConversions"))`
        // block — file-line citation deliberately omitted to remain
        // stable under future reformatting of the loader TU).
        if (ck == TypeKind::Ptr && tk == TypeKind::Ptr) {
            auto const fromElem = interner.operands(child.type);
            auto const toElem   = interner.operands(target);
            if (!fromElem.empty() && !toElem.empty()) {
                bool const fromIsVoid =
                    interner.kind(fromElem[0]) == TypeKind::Void;
                bool const toIsVoid =
                    interner.kind(toElem[0]) == TypeKind::Void;
                bool admit = false;
                // T* → void* (toIsVoid && !fromIsVoid)
                if (toIsVoid && !fromIsVoid) {
                    admit = sem.pointerConversions.implicitToVoidPtr;
                }
                // void* → T* (fromIsVoid && !toIsVoid)
                else if (fromIsVoid && !toIsVoid) {
                    admit = sem.pointerConversions.implicitFromVoidPtr;
                }
                if (admit) {
                    HirNodeId const cast = builder.makeCast(
                        child.id, target, HirFlags::Synthetic);
                    for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
                        if (it->first == child.id) {
                            spans.push_back({cast, it->second});
                            break;
                        }
                    }
                    return {cast, target};
                }
            }
        }
        // D-LANG-NULL-POINTER-CONSTANT (step 13.3, 2026-06-02): when
        // the source expression is an integer-literal in a pointer-
        // typed context and the active language admits null-pointer-
        // constant conversion, materialize the null-pointer conversion
        // as `Cast(IntLit, Ptr<T>)`. The MIR-tier Cast lowering routes
        // integer→pointer as a zero-extending move into a pointer-
        // width register (literal 0 → 8-byte zero on Win64 ms_x64),
        // which Windows ABI reads as NULL.
        //
        // **Trust the semantic admission**: the value check (literal
        // == 0) lives in `semantic_analyzer.cpp::isLiteralIntegerZero`
        // at the CST tier. By the time coerce() is called, the
        // semantic analyzer has already verified value==0 AND admitted
        // the conversion; a NON-zero literal would have produced
        // S_TypeMismatch before HIR lowering. Duplicating the value
        // check at HIR tier requires a literal-pool lookup
        // (`builder.payload` + `literals.at` + variant arm dispatch
        // via `asInt64`) which is sensitive to STL implementation
        // differences (caught on macOS CI 2026-06-02 where the literal
        // pool variant access disagreed between hosts, silently
        // skipping Cast emission). Trusting the semantic gate makes
        // the lowering single-source-of-truth at the right tier.
        //
        // Source-agnostic: gates on the per-language config flag.
        // Languages without null-pointer-constant (Rust/Swift/Zig)
        // never admit at the semantic tier and never reach this arm
        // with int→Ptr.
        if (tk == TypeKind::Ptr
            && sem.pointerConversions.nullPointerConstantFromIntegerZero
            && builder.kind(child.id) == HirKind::Literal) {
            auto const ck2 = interner.kind(child.type);
            bool const isInt =
                   ck2 == TypeKind::I8  || ck2 == TypeKind::I16
                || ck2 == TypeKind::I32 || ck2 == TypeKind::I64
                || ck2 == TypeKind::I128
                || ck2 == TypeKind::U8  || ck2 == TypeKind::U16
                || ck2 == TypeKind::U32 || ck2 == TypeKind::U64
                || ck2 == TypeKind::U128;
            if (isInt) {
                HirNodeId const cast = builder.makeCast(
                    child.id, target, HirFlags::Synthetic);
                for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
                    if (it->first == child.id) {
                        spans.push_back({cast, it->second});
                        break;
                    }
                }
                return {cast, target};
            }
        }
        // Pointers, structs, FnSig are not coerced implicitly; let the
        // caller decide whether the mismatch is a diagnostic. Arithmetic
        // (int + float kinds — file-scope `isArithmeticCore`) is the
        // implicit-conversion surface.
        if (!isArithmeticCore(ck) || !isArithmeticCore(tk)) return child;
        HirNodeId const cast = builder.makeCast(child.id, target, HirFlags::Synthetic);
        // Alias the synthetic Cast to its operand's pending span entry so
        // diagnostics anchored at the Cast locate to real source. The
        // operand may have multiple pending entries (rare — only when an
        // earlier coerce already wrapped it); use the most recent (last).
        for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
            if (it->first == child.id) {
                spans.push_back({cast, it->second});
                break;
            }
        }
        return E{cast, target};
    }

    // Truthiness at CONDITION positions (`if`/`while`/`do-while`/`for`
    // conditions, the ternary cond, LogicalAnd/Or operands): a non-Bool
    // scalar condition tests `!= 0` (C99 6.8.4.1p2/6.8.5p4 "compares
    // unequal to 0"; 6.5.13/6.5.14/6.5.15 for the operand forms), NOT a
    // value-truncating Cast-to-Bool — `if (2)` is TRUE under truthiness,
    // while `Cast → MIR Trunc(2 → Bool)` keeps only the low bit (false).
    // Builds the SAME compare-against-typed-zero shape `hir_to_mir`
    // already lowers `HirOpKind::Not` to (ICmpEq there; the binary `Ne`
    // here routes via `mapBinaryOp` → ICmpNe for integer kinds — Ne is
    // signedness-irrelevant — and FCmpUNE for float kinds: the
    // UNORDERED-or-unequal predicate, TRUE on NaN, so `if (NaN)` is
    // true exactly as C requires — NaN compares unequal to 0.0; the
    // FC3.5 D-COND-FLOAT-NAN-TRUTHINESS-FCMP adjudication, flipped
    // from the interim FCmpOne when FCmp gained its LIR lowering).
    //
    // Dispatch is a TypeKind SHAPE predicate — never language identity:
    //  - invalid type, or already Bool → UNCHANGED (a comparison already
    //    yields Bool: zero extra nodes; invalid passes through exactly
    //    like `coerce`).
    //  - arithmetic non-Bool (int kinds, float kinds, Char, Byte) →
    //    synthetic `BinaryOp Ne(cond, zero-of-cond's-own-type)` typed
    //    Bool. The zero keeps the cond's type (no promotion is needed
    //    for an unequal-to-zero test), minted by `synthZeroOrError`'s
    //    scalar arm.
    //  - Ptr, when the active language admits integer-zero null-pointer
    //    constants (`pointerConversions.nullPointerConstantFromIntegerZero`)
    //    → `Ne(cond, Cast(0 → cond's Ptr type))` — the 13.3 null-pointer-
    //    constant node shape the semantic-admitted `coerce` arm builds
    //    for a source-level zero in pointer context ("scalar" in C
    //    6.8.4.1 includes pointers). Languages without the rule keep the
    //    pass-through below.
    //  - everything else (Struct, Array, Enum, FnSig, Void, …) →
    //    UNCHANGED: exactly the prior coerce(_, Bool) pass-through for
    //    non-arithmetic kinds; the MIR verifier's CondBr-expects-Bool
    //    check remains the loud gate.
    //
    // Replaced `coerce(cond, boolType())` at every condition site
    // (2026-06-11): for an I32 cond that emitted `Cast(I32 → Bool)`
    // which MIR's `mapCast` lowers as `Trunc` — semantically wrong for
    // truthiness conditions (`if (2)` would have been false; every
    // reachable case was caught by the FC3-c2 conversion-width gate, so
    // nothing miscompiled silently).
    [[nodiscard]] E coerceCondition(E cond, NodeId anchor) {
        if (!cond.type.valid()) return cond;
        TypeKind const ck = interner.kind(cond.type);
        if (ck == TypeKind::Bool) return cond;
        if (isArithmeticCore(ck)) {
            HirNodeId const zero = synthZeroOrError(anchor, cond.type);
            return {track(builder.addParent(HirKind::BinaryOp,
                                            std::array{cond.id, zero},
                                            boolType(),
                                            encodeOp(HirOpKind::Ne)),
                          anchor),
                    boolType()};
        }
        if (ck == TypeKind::Ptr
            && sem.pointerConversions.nullPointerConstantFromIntegerZero) {
            // Null-pointer constant: Cast(Literal 0 : I32 → cond's Ptr
            // type), both Synthetic — the exact shape the 13.3 coerce
            // arm materializes for a semantic-admitted source-level
            // integer zero in pointer context; MIR's `mapCast` routes
            // it as IntToPtr.
            HirLiteralValue v;
            v.core  = TypeKind::I32;
            v.value = std::int64_t{0};
            HirNodeId const zeroLit = builder.makeLiteral(
                interner.primitive(TypeKind::I32), literals.add(v),
                HirFlags::Synthetic);
            HirNodeId const nullPtr = builder.makeCast(
                zeroLit, cond.type, HirFlags::Synthetic);
            return {track(builder.addParent(HirKind::BinaryOp,
                                            std::array{cond.id, nullPtr},
                                            boolType(),
                                            encodeOp(HirOpKind::Ne)),
                          anchor),
                    boolType()};
        }
        return cond;
    }

    Lowerer(SemanticModel& m, HirLoweringConfig const& c, SemanticConfig const& s,
            NumberStyle const* ns, DiagnosticReporter& r, HirBuilder& b,
            HirLiteralPool& lits, std::vector<std::pair<HirNodeId, HirSourceLoc>>& sp,
            std::vector<HirExternRecord>& ed,
            std::vector<std::pair<HirNodeId, LinkageAttr>>& lk)
        : model(m), cfg(c), sem(s), numberStyle(ns), interner(m.lattice().interner()),
          reporter(r), builder(b), literals(lits), spans(sp), externDecls(ed),
          linkage(lk) {
        for (std::size_t i = 0; i < cfg.ruleMappings.size(); ++i)
            ruleMap_.emplace(cfg.ruleMappings[i].rule.v, i);
        for (std::size_t i = 0; i < sem.declarations.size(); ++i)
            declMap_.emplace(sem.declarations[i].rule.v, i);
        for (std::size_t i = 0; i < cfg.binaryOps.size(); ++i)
            binOp_.emplace(cfg.binaryOps[i].token.v, i);
        for (std::size_t i = 0; i < cfg.unaryOps.size(); ++i)
            unOp_.emplace(cfg.unaryOps[i].token.v, i);
        for (std::size_t i = 0; i < cfg.postfixOps.size(); ++i)
            postOp_.emplace(cfg.postfixOps[i].token.v, i);
        for (auto const& lt : sem.literalTypes) {
            litType_.emplace(lt.literal.v, interner.primitive(lt.core));
            litCore_.emplace(lt.literal.v, lt.core);
            if (lt.fixedValue.has_value()) {
                litFixed_.emplace(lt.literal.v, *lt.fixedValue);
            }
        }
        // FC3 c1: data model + resolved UAC rules (see the member docs).
        dataModel_ = m.dataModel();
        if (sem.arithmeticConversions.has_value()) {
            arith_ = resolveArithmeticRules(*sem.arithmeticConversions, dataModel_);
        }
        for (RuleId r : cfg.deferredRules) deferred_.emplace(r.v, true);
        // HR10: register every declared extension kind up front, so a rule mapped
        // to one (or a NULL literal) lowers to a HirKind::Extension carrying its id.
        for (auto const& e : cfg.extensionKinds)
            extKindByName_.emplace(e.name, builder.registry().registerExtension(e.name, e.lang));
        for (std::size_t i = 0; i < sem.callRules.size(); ++i)
            callMap_.emplace(sem.callRules[i].rule.v, i);
        for (auto const& rr : sem.references) refRule_.emplace(rr.rule.v, true);
    }

    std::unordered_map<std::string, HirKindId> extKindByName_;  // HR10 extension kinds
    std::unordered_map<std::uint32_t, std::size_t> callMap_;    // RuleId.v → sem.callRules idx
    std::unordered_map<std::uint32_t, bool>        refRule_;     // RuleId.v of reference rules

    // The registered HirKindId for an extension-kind name. Every name reaching
    // here is declared in cfg.extensionKinds: ruleMapping/nested-ext callers are
    // gated by `extKindByName_.count`, and the loader validates `nullExtensionKind`
    // / `refExtensionKind` against `extensionKinds`. A miss is therefore an
    // internal invariant violation, not user-config drift — report it loud (never
    // silently mint a phantom kind) and still register so the node stays
    // well-formed for the rest of the pass.
    [[nodiscard]] HirKindId extKind(std::string const& name) {
        auto it = extKindByName_.find(name);
        if (it != extKindByName_.end()) return it->second;
        unsupported(NodeId{}, std::format("internal: extension kind '{}' was not "
                                          "declared in hirLowering.extensionKinds", name));
        HirKindId id = builder.registry().registerExtension(name, std::string{model.unit().schema().name()});
        extKindByName_.emplace(name, id);
        return id;
    }

    // True iff the subtree rooted at `node` contains a node whose rule is
    // explicitly deferred (e.g. an array declarator). Bounded.
    [[nodiscard]] bool subtreeHasDeferred(NodeId node) const {
        if (deferred_.empty()) return false;
        std::vector<NodeId> stack{node};
        for (int guard = 0; !stack.empty() && guard < 8192; ++guard) {
            NodeId c = stack.back();
            stack.pop_back();
            if (tree().kind(c) == NodeKind::Internal && deferred_.count(tree().rule(c).v) != 0)
                return true;
            for (NodeId g : visible(c)) stack.push_back(g);
        }
        return false;
    }

    [[nodiscard]] Tree const& tree() const { return *t_; }

    // ── small helpers ─────────────────────────────────────────────────────────
    [[nodiscard]] std::vector<NodeId> visible(NodeId parent) const {
        std::vector<NodeId> out;
        for (NodeId c : tree().children(parent))
            if (!isEmptySpace(tree().flags(c))) out.push_back(c);
        return out;
    }
    [[nodiscard]] bool isToken(NodeId n) const { return tree().kind(n) == NodeKind::Token; }

    // ── declaration-specifier prefix (D-DECL-SPECIFIER-PREFIX-SUBSTRATE consumer)
    // The c-subset's `static`/`__attribute__` prefix rides as an OPTIONAL leading
    // child of a declaration. CST→HIR resolves positional declaration children
    // via the SHARED strip-aware helpers in `core/types/decl_prefix_strip.hpp`
    // (`declRoleChildren` / `descendVisibleDecl` / `specifierPrefixChild` —
    // D-DECL-PREFIX-STRIP-SHARED-HELPER: one source of truth with the semantic
    // analyzer and cst_const_eval), so lowering sees the same prefix-free
    // positional indices the analyzer minted symbols against — without the
    // strip, a leading prefix shifts name/type/params/body/kindByChild by one
    // (silent wrong-child). The prefix subtree stays reachable for `linkageFrom`
    // via `specifierPrefixChild`.

    // Fold the linkage effects of every specifier token in `prefixNode` onto a
    // LinkageAttr, per the declaration's `linkageSpecifiers` facet (token SOURCE
    // TEXT → effect). A token whose KIND is in `linkageSpecifierIgnoredKinds` (the
    // prefix's declared STRUCTURAL syntax — `__attribute__`, parens) is skipped;
    // any OTHER token MUST resolve in `linkageSpecifiers`, else it is an
    // unrecognized specifier and fails loud (`H_UnknownLinkageSpecifier`) — a typo
    // (`__attribute__((wek))`) or an unsupported attribute, never a silent no-op
    // (D-CSUBSET-LINKAGE-UNKNOWN-SPECIFIER-DIAGNOSTIC). Agnostic: BOTH the effect
    // map and the ignored-kind set are per-language config; the engine compares
    // resolved SchemaTokenIds + source text, never a hardcoded kind/identity.
    [[nodiscard]] LinkageAttr linkageFrom(NodeId prefixNode, DeclarationRule const& decl) {
        LinkageAttr attr{};
        if (!prefixNode.valid() || decl.linkageSpecifiers.empty()) return attr;
        // Collect the prefix's tokens in SOURCE order (the composite-key
        // pairing below is order-sensitive), skipping the subtrees of any
        // `linkageSpecifierIgnoredRules` rule wholesale (FC4 c1 / D14 —
        // attribute forms the language parses but semantically ignores,
        // e.g. C23 `[[deprecated]]`: their identifiers must neither
        // resolve as linkage specifiers nor fail loud as unknown ones).
        std::vector<NodeId> toks;
        {
            std::vector<NodeId> stack{prefixNode};
            while (!stack.empty()) {
                NodeId n = stack.back();
                stack.pop_back();
                if (isToken(n)) {
                    toks.push_back(n);
                    continue;
                }
                bool skip = false;
                for (RuleId rid : decl.linkageSpecifierIgnoredRules) {
                    if (tree().rule(n).v == rid.v) { skip = true; break; }
                }
                if (skip) continue;
                auto const kids = visible(n);
                for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                    stack.push_back(*it);   // reverse-push → source order
                }
            }
        }
        // String-literal token kinds (from the language's hirLowering
        // literal config) drive the COMPOSITE form: a specifier identifier
        // immediately followed by a parenthesized string literal forms the
        // lookup key `<identifier>:<decoded-body>` — c-subset's
        // `__attribute__((visibility("hidden")))` resolves the declared
        // facet key "visibility:hidden". Generic: ANY attr-with-string-arg
        // works; an unknown composite fails loud with the composite key.
        SchemaTokenId const strStart = cfg.stringStartToken;
        SchemaTokenId const strBody  = cfg.stringBodyToken;
        auto const isIgnoredKind = [&](SchemaTokenId kind) {
            for (SchemaTokenId k : decl.linkageSpecifierIgnoredKinds)
                if (k == kind) return true;
            return false;
        };
        for (std::size_t i = 0; i < toks.size(); ++i) {
            NodeId const n = toks[i];
            SchemaTokenId const kind = tree().tokenKind(n);
            if (isIgnoredKind(kind)) continue;  // declared structural syntax
            if (strStart.valid()
                && (kind == strStart
                    || (strBody.valid() && kind == strBody))) {
                // String tokens are consumed by the composite pairing
                // below; a string with NO preceding specifier identifier
                // falls through that pairing and is skipped here (the
                // grammar only admits strings inside an attr argument).
                continue;
            }
            std::string key{tree().text(n)};
            // Composite probe: the next non-ignored token opens a string
            // literal → pair this specifier with the decoded body.
            if (strStart.valid() && strBody.valid()) {
                std::size_t j = i + 1;
                while (j < toks.size()
                       && isIgnoredKind(tree().tokenKind(toks[j]))) {
                    ++j;
                }
                if (j + 1 < toks.size()
                    && tree().tokenKind(toks[j]) == strStart
                    && tree().tokenKind(toks[j + 1]) == strBody) {
                    auto decoded =
                        decodeStringLiteralBody(tree().text(toks[j + 1]));
                    if (decoded.has_value()) {
                        key += ':';
                        key += *decoded;
                    } else {
                        emitH(DiagnosticCode::H_UnknownLinkageSpecifier,
                              toks[j + 1],
                              std::format("malformed string argument in "
                                          "specifier '{}'", key));
                        continue;
                    }
                }
            }
            auto it = decl.linkageSpecifiers.find(key);
            if (it != decl.linkageSpecifiers.end()) {
                if (it->second.binding)    attr.binding    = *it->second.binding;
                if (it->second.visibility) attr.visibility = *it->second.visibility;
            } else {
                emitH(DiagnosticCode::H_UnknownLinkageSpecifier, n,
                      std::format("'{}' is not a recognized linkage specifier",
                                  key));
            }
        }
        return attr;
    }
    // Record NON-default linkage for a lowered decl node (sparse: default linkage
    // is the implicit externally-visible state and needn't be stored).
    void recordLinkage(HirNodeId node, LinkageAttr attr) {
        if (attr.binding != SymbolBinding::Global
            || attr.visibility != SymbolVisibility::Default)
            linkage.push_back({node, attr});
    }

    [[nodiscard]] bool isExprNode(NodeId n) const {
        if (tree().kind(n) != NodeKind::Internal) return false;
        std::uint32_t const r = tree().rule(n).v;
        return r == cfg.binaryExprRule.v || r == cfg.unaryExprRule.v
            || r == cfg.postfixExprRule.v || r == cfg.operandRule.v
            || (cfg.ternaryExprRule.valid() && r == cfg.ternaryExprRule.v);
    }
    [[nodiscard]] TypeId boolType() { return interner.primitive(TypeKind::Bool); }
    [[nodiscard]] TypeId typeAtOr(NodeId n, TypeId fallback) const {
        TypeId t = model.typeAt(n);
        return t.valid() ? t : fallback;
    }

    HirNodeId track(HirNodeId id, NodeId cst) {
        if (cst.valid())
            spans.push_back({id, HirSourceLoc{tree().source().id(), tree().span(cst)}});
        return id;
    }

    // Parameterized lowering-error emission. Post-fold #7 simplifier
    // R1 + type-design T2: collapses the previous per-code helpers
    // (`unsupported`, `externHasInitializer`) into a neutrally-named
    // core so a third H_* code lands as one inline call site, not a
    // third near-identical helper.
    void emitH(DiagnosticCode code, NodeId node, std::string detail) {
        ParseDiagnostic d;
        d.code     = code;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree().source().id();
        d.span     = node.valid() ? tree().span(node) : SourceSpan::empty(0);
        d.actual   = std::move(detail);
        reporter.report(std::move(d));
    }
    void unsupported(NodeId node, std::string detail) {
        emitH(DiagnosticCode::H_UnsupportedLoweringForKind,
              node, std::move(detail));
    }
    HirNodeId errorNode(NodeId cst, TypeId type = InvalidType) {
        return track(builder.addLeaf(HirKind::Error, type, 0, HirFlags::HasError), cst);
    }
    // Report + an Error sentinel (every Error node is paired with a diagnostic).
    HirNodeId reportedError(NodeId cst, std::string detail) {
        unsupported(cst, std::move(detail));
        return errorNode(cst);
    }
    E exprError(NodeId cst, std::string detail) { return {reportedError(cst, std::move(detail)), InvalidType}; }
    // An optional child, or a reported Error sentinel when absent (only builds the
    // Error — and only reports — when the optional is empty; avoids the eager-eval
    // trap of `opt.value_or(errorNode(...))`).
    HirNodeId orError(std::optional<HirNodeId> v, NodeId cst, std::string detail) {
        return v ? *v : reportedError(cst, std::move(detail));
    }

    [[nodiscard]] HirRuleMapping const* mappingFor(NodeId n) const {
        if (tree().kind(n) != NodeKind::Internal) return nullptr;
        auto it = ruleMap_.find(tree().rule(n).v);
        return it == ruleMap_.end() ? nullptr : &cfg.ruleMappings[it->second];
    }

    // Alt rules (`statement`, `topLevel`, `expression`, `switchBodyItem`, …)
    // materialize as wrapper nodes with a single meaningful child. Peel them
    // until reaching a node the engine recognizes — an expression form, a
    // mapped statement/decl rule, or the case-label rule — so callers can
    // classify a child by ROLE without knowing the grammar's wrapper shapes.
    [[nodiscard]] NodeId peelToCore(NodeId n) const {
        NodeId cur = n;
        for (int guard = 0; guard < 128 && cur.valid(); ++guard) {
            if (tree().kind(cur) != NodeKind::Internal) return cur;  // token
            std::uint32_t const rv = tree().rule(cur).v;
            if (isExprNode(cur)) return cur;
            if (ruleMap_.count(rv) != 0) return cur;
            if (cfg.caseLabelRule.valid() && rv == cfg.caseLabelRule.v) return cur;
            NodeId only = soleMeaningfulChild(cur);
            if (!only.valid()) return cur;
            cur = only;
        }
        return cur;
    }

    enum class Role { Expr, Stmt, Other };
    [[nodiscard]] Role classify(NodeId n) const {
        NodeId core = peelToCore(n);
        if (core.valid() && tree().kind(core) == NodeKind::Internal) {
            if (isExprNode(core)) return Role::Expr;
            if (ruleMap_.count(tree().rule(core).v) != 0) return Role::Stmt;
        }
        return Role::Other;
    }

    // R2 (D-SEMANTIC-NULL-CONSTANT-FOLDING): a source node the semantic tier
    // admitted as a FOLDED null-pointer constant (`1-1`, `-0` — a non-literal
    // integer constant expression with value 0) lowers to a synthetic Literal 0,
    // NOT its operator tree. The value is provably 0 (semantic-verified), so the
    // existing coerce() literal-0 arm materializes `Cast(0 → Ptr)` downstream.
    // Short-circuiting at the lowering ENTRY means the operator subtree is never
    // emitted (no dead node) and const-folding stays OUT of lowering — the semantic
    // marker is the single authority. Fixed I32 zero: the marked node's own type is
    // an interior arithmetic node Pass 2 never stamped (model.typeAt would be
    // invalid); the coerce arm + verifier fallback need only an integer-typed
    // Literal. Mirrors the null-materialization template at the ternary-condition
    // arm. nullopt for an unmarked node (the overwhelming common case).
    [[nodiscard]] std::optional<E> nullPointerConstantLiteral(NodeId node) {
        if (!model.isNullPointerConstant(node)) return std::nullopt;
        HirLiteralValue v;
        v.core  = TypeKind::I32;
        v.value = std::int64_t{0};
        HirNodeId const zeroLit = builder.makeLiteral(
            interner.primitive(TypeKind::I32), literals.add(v),
            HirFlags::Synthetic);
        return E{track(zeroLit, node), interner.primitive(TypeKind::I32)};
    }

    // ── expressions ───────────────────────────────────────────────────────────
    E lowerExpr(NodeId node) {
        if (auto npc = nullPointerConstantLiteral(node)) return *npc;
        if (tree().kind(node) == NodeKind::Internal) {
            std::uint32_t const r = tree().rule(node).v;
            if (cfg.flatExprRule.valid() && r == cfg.flatExprRule.v)
                return lowerFlatExpr(node);   // HR10: SQL-style flat expression
            if (r == cfg.operandRule.v)     return lowerOperand(node);
            if (r == cfg.binaryExprRule.v)  return lowerBinary(node);
            if (r == cfg.unaryExprRule.v)   return lowerUnary(node);
            if (r == cfg.postfixExprRule.v) return lowerPostfix(node);
            if (cfg.ternaryExprRule.valid() && r == cfg.ternaryExprRule.v)
                return lowerTernary(node);
            // Unknown wrapper (e.g. an `expression` node): descend through a
            // single meaningful child.
            NodeId only = soleMeaningfulChild(node);
            if (only.valid()) return lowerExpr(only);
        }
        unsupported(node, "expression form has no hirLowering mapping");
        return {errorNode(node), InvalidType};
    }

    // The single non-token child, or invalid if there isn't exactly one.
    [[nodiscard]] NodeId soleMeaningfulChild(NodeId node) const {
        NodeId found{};
        for (NodeId c : visible(node)) {
            if (isToken(c)) continue;
            if (found.valid()) return {};
            found = c;
        }
        return found;
    }

    // Detect and lower a value-bearing LEAF literal directly under `node`: the
    // char / string / unicode-string forms materialize as a `[startToken,
    // COALESCED body]` subtree, and NULL as a single child token — none of which
    // is an expression node, so both operand lowerers probe for them before the
    // generic internal-child descent. std::nullopt ⇒ not such a leaf literal.
    [[nodiscard]] std::optional<E> tryLowerLeafLiteral(NodeId node) {
        if (NodeId chl = bodyLiteralNodeOf(node, cfg.charStartToken); chl.valid())
            return lowerCharLiteral(chl);
        if (NodeId sl = bodyLiteralNodeOf(node, cfg.stringStartToken); sl.valid())
            return lowerStringLiteral(sl);
        if (NodeId us = bodyLiteralNodeOf(node, cfg.unicodeStringStartToken); us.valid())
            return lowerStringLiteral(us);   // SQL `N'…'` — same body token + decoder
        if (cfg.nullToken.valid())            // SQL NULL → a typeless extension leaf
            if (NodeId nt = childTokenOfKind(node, cfg.nullToken); nt.valid())
                return lowerNullLiteral(node);
        return std::nullopt;
    }

    E lowerOperand(NodeId node) {
        // operand = Identifier | <literal token> | <char/string literal>
        //         | ( expression ) | compoundLiteralExpr | castExpr | ...
        if (auto lit = tryLowerLeafLiteral(node)) return *lit;
        // D5.3 cycle 1b.3: compound literal `(T){...}` as an expression.
        // Detected BEFORE the generic internal-child descent because
        // its shape is `(T){...}` — lowerExpr's rule dispatch doesn't
        // recognize compoundLiteralExpr; route it through the dedicated
        // lowering that resolves the type-ref + lowers the brace child.
        // FC2: the explicit cast `(T)expr` routes the same way (its type
        // child must NOT be lowered as an expression).
        for (NodeId c : visible(node)) {
            if (tree().kind(c) != NodeKind::Internal) continue;
            if (cfg.compoundLiteralRule.valid()
             && tree().rule(c).v == cfg.compoundLiteralRule.v) {
                return lowerCompoundLiteral(c);
            }
            if (cfg.castRule.valid()
             && tree().rule(c).v == cfg.castRule.v) {
                return lowerCast(c);
            }
            if (cfg.sizeofRule.valid()
             && tree().rule(c).v == cfg.sizeofRule.v) {
                return lowerSizeof(c);
            }
        }
        for (NodeId c : visible(node)) {
            if (tree().kind(c) == NodeKind::Internal) return lowerExpr(c);  // paren-wrapped
        }
        for (NodeId c : visible(node)) {
            if (!isToken(c)) continue;
            SchemaTokenId const tk = tree().tokenKind(c);
            if (sem.identifierToken.valid() && tk.v == sem.identifierToken.v) {
                TypeId const type = typeAtOr(node, InvalidType);
                SymbolId const sym = model.symbolAt(c);
                return {track(builder.makeRef(type, sym.v), node), type};
            }
            auto lit = litType_.find(tk.v);
            if (lit != litType_.end()) return lowerLiteral(node, c, tk, lit->second);
        }
        unsupported(node, "operand has no Identifier / literal / parenthesized child");
        return {errorNode(node), InvalidType};
    }

    // The child rule node of `operand` whose first visible child is `startTok`
    // (a `charLiteralExpr` / `stringLiteralExpr` subtree), or invalid.
    [[nodiscard]] NodeId bodyLiteralNodeOf(NodeId operand, SchemaTokenId startTok) {
        if (!startTok.valid()) return {};
        for (NodeId c : visible(operand)) {
            if (tree().kind(c) != NodeKind::Internal) continue;
            for (NodeId g : visible(c)) {
                if (isToken(g) && tree().tokenKind(g).v == startTok.v) return c;
                break;  // only the FIRST visible child decides
            }
        }
        return {};
    }

    // The first visible child token of `node` whose kind is `k`, or invalid.
    [[nodiscard]] NodeId childTokenOfKind(NodeId node, SchemaTokenId k) {
        for (NodeId c : visible(node))
            if (isToken(c) && tree().tokenKind(c).v == k.v) return c;
        return {};
    }

    // `'a'` / `'\n'` → a Char-typed literal carrying the decoded codepoint.
    E lowerCharLiteral(NodeId node) {
        TypeId const type = interner.primitive(TypeKind::Char);
        NodeId const bodyTok = childTokenOfKind(node, cfg.charBodyToken);
        std::string_view const body = bodyTok.valid() ? tree().text(bodyTok) : std::string_view{};
        auto cp = decodeCharLiteralBody(body);
        if (!cp) {
            unsupported(node, std::format("char literal '{}' is empty, multi-character, "
                                          "or has an unsupported escape", body));
            return {errorNode(node, type), type};
        }
        HirLiteralValue v;
        v.core  = TypeKind::Char;
        v.value = static_cast<std::uint64_t>(*cp);
        return {track(builder.makeLiteral(type, literals.add(std::move(v))), node), type};
    }

    // `"hello"` → an Array<Char, N+1> literal carrying the decoded bytes (NUL
    // implied by the +1 length).
    E lowerStringLiteral(NodeId node) {
        NodeId const bodyTok = childTokenOfKind(node, cfg.stringBodyToken);
        std::string_view const body = bodyTok.valid() ? tree().text(bodyTok) : std::string_view{};
        std::string bytes;
        if (cfg.stringDoubledDelimiter) {
            // SQL `'…''…'`: doubled-delimiter escaping (never fails — pairs only).
            bytes = decodeDoubledDelimiterBody(body, cfg.stringDelimiter);
        } else if (auto decoded = decodeStringLiteralBody(body)) {  // C-family backslash escapes
            bytes = std::move(*decoded);
        } else {
            unsupported(node, std::format("string literal \"{}\" has an unsupported escape", body));
            return {errorNode(node), InvalidType};
        }
        TypeId const type = interner.array(interner.primitive(TypeKind::Char),
                                           static_cast<std::int64_t>(bytes.size() + 1));
        HirLiteralValue v;
        v.core  = TypeKind::Char;
        v.value = std::move(bytes);
        return {track(builder.makeLiteral(type, literals.add(std::move(v))), node), type};
    }

    // SQL `NULL` (typeless) → a leaf Extension node of the configured kind.
    E lowerNullLiteral(NodeId node) {
        HirKindId const kid = extKind(cfg.nullExtensionKind);
        return {track(builder.addLeaf(HirKind::Extension, InvalidType, kid.v), node), InvalidType};
    }

    // ── HR10: flat expression + SQL operand lowering ───────────────────────────
    [[nodiscard]] bool isNameToken(NodeId n) const {
        if (!isToken(n)) return false;
        SchemaTokenId const tk = tree().tokenKind(n);
        return (sem.identifierToken.valid() && tk.v == sem.identifierToken.v)
            || (sem.bracketIdentifierToken && sem.bracketIdentifierToken->valid()
                && tk.v == sem.bracketIdentifierToken->v);
    }

    // A flat `operand (binaryOpRule operand)*` sequence (SQL's `expression`),
    // left-folded into nested core BinaryOp nodes. Distinct from the Pratt path.
    E lowerFlatExpr(NodeId node) {
        if (auto npc = nullPointerConstantLiteral(node)) return *npc;
        std::vector<NodeId> operands;
        std::vector<NodeId> opToks;
        for (NodeId c : visible(node)) {
            if (tree().kind(c) != NodeKind::Internal) continue;
            if (cfg.flatBinaryOpRule.valid() && tree().rule(c).v == cfg.flatBinaryOpRule.v) {
                for (NodeId t : visible(c)) if (isToken(t)) { opToks.push_back(t); break; }
            } else {
                operands.push_back(c);
            }
        }
        if (operands.empty()) return exprError(node, "flat expression has no operand");
        // A well-formed `operand (op operand)*` has exactly one fewer operator
        // than operands. A mismatch is a malformed parse — fail loud rather than
        // silently fold a truncated expression.
        if (opToks.size() + 1 != operands.size())
            return exprError(node, std::format("malformed flat expression: {} operands "
                                               "but {} operators", operands.size(), opToks.size()));
        E acc = lowerFlatOperand(operands[0]);
        for (std::size_t i = 0; i + 1 < operands.size() && i < opToks.size(); ++i) {
            auto it = binOp_.find(tree().tokenKind(opToks[i]).v);
            if (it == binOp_.end()) {
                unsupported(opToks[i], std::format("binary operator '{}' has no hirLowering mapping",
                                                   tree().text(opToks[i])));
                return {errorNode(node), InvalidType};
            }
            acc = combineBinary(opToks[i], cfg.binaryOps[it->second], acc, lowerFlatOperand(operands[i + 1]));
        }
        return acc;
    }

    // One operand of a flat SQL expression: literal / string / NULL / unary-minus
    // / call / name-reference / parenthesized sub-expression.
    E lowerFlatOperand(NodeId node) {
        if (auto lit = tryLowerLeafLiteral(node)) return *lit;
        // Unary prefix (SQL `-operand`): a unary-op token + an inner operand.
        {
            NodeId negTok{}, inner{};
            for (NodeId c : visible(node)) {
                if (isToken(c)) { if (unOp_.count(tree().tokenKind(c).v)) negTok = c; }
                else if (!inner.valid()) inner = c;
            }
            if (negTok.valid() && inner.valid()) {
                auto it = unOp_.find(tree().tokenKind(negTok).v);
                auto op = coreOpFromName(cfg.unaryOps[it->second].target);
                E e = lowerFlatOperand(inner);
                if (!op || arityOf(*op) != HirOpArity::Unary)
                    return exprError(node, "unary operand has no core unary op");
                return {track(builder.addParent(HirKind::UnaryOp, std::array{e.id}, e.type,
                                                encodeOp(*op)), node), e.type};
            }
        }
        for (NodeId c : visible(node)) {
            if (tree().kind(c) != NodeKind::Internal) continue;
            if (cfg.flatExprRule.valid() && tree().rule(c).v == cfg.flatExprRule.v)
                return lowerFlatExpr(c);                       // parenthesized expression
            // SQL call `f(args)`: the operand wraps a call rule (e.g. nameOrCall →
            // callExpr). `peelToCore` would descend PAST the call into its sole
            // non-token child (the argument list), so detect the call rule
            // explicitly in the subtree before the name-reference fallback.
            if (NodeId call = findCallNode(c); call.valid()) return lowerSqlCall(call);
            // A name reference: `c` peels directly to a name token, or wraps one
            // (qualifiedName → nameAtom → Identifier — `peelToCore` stops at the
            // nameAtom wrapper, so probe the subtree for the name token).
            if (firstNameToken(c).node.valid()) return nameRefExpr(c);
            return lowerExpr(c);                                // defensive fallback
        }
        for (NodeId c : visible(node)) {                        // direct literal token (IntLiteral)
            if (!isToken(c)) continue;
            SchemaTokenId const tk = tree().tokenKind(c);
            auto lit = litType_.find(tk.v);
            if (lit != litType_.end()) return lowerLiteral(node, c, tk, lit->second);
        }
        return exprError(node, "SQL operand has no recognizable form");
    }

    // Shared callee-signature resolution (FC4 c2, plan-lock MUST-FIX 3):
    // a callee expression carries either the FnSig directly (a direct /
    // paren-wrapped designator) or `Ptr<FnSig>` (a function-pointer
    // value — the canonical c-subset fn-ptr type). Unwrap ONE pointer
    // level, mirroring hir_verifier.cpp's checkCallArguments. Feeds
    // BOTH call arms' param COERCION + inferred result type — without
    // it, indirect-call arguments would silently skip coercion (the
    // width/signedness miscompile class). InvalidType when the callee
    // type is neither shape (opaque/extension callee — the verifier
    // owns arity rules there).
    [[nodiscard]] TypeId calleeSigOf(TypeId t) const {
        if (!t.valid()) return InvalidType;
        if (interner.kind(t) == TypeKind::FnSig) return t;
        if (interner.kind(t) == TypeKind::Ptr) {
            auto const ops = interner.operands(t);
            if (!ops.empty() && interner.kind(ops[0]) == TypeKind::FnSig) {
                return ops[0];
            }
        }
        return InvalidType;
    }

    // SQL `f(args)` → a core Call (callee Ref + lowered argument expressions),
    // reusing the semantics `callRules` callee/args child positions.
    E lowerSqlCall(NodeId callNode) {
        auto it = callMap_.find(tree().rule(callNode).v);
        if (it == callMap_.end()) return exprError(callNode, "call rule has no semantics entry");
        CallRule const& cr = sem.callRules[it->second];
        auto vis = visible(callNode);
        HirNodeId callee{};
        TypeId calleeTy = InvalidType;
        if (cr.calleeChild < vis.size()) {
            NodeId calleeNode = vis[cr.calleeChild];
            NodeId tok = isToken(calleeNode) ? calleeNode : peelToCore(calleeNode);
            SymbolId const sym = isNameToken(tok) ? model.symbolAt(tok) : SymbolId{};
            calleeTy = typeAtOr(calleeNode, InvalidType);
            // The callee is a name. In a relational-name language (refExtensionKind
            // set), a function name is symbolic like any other name — not a typed
            // value read — so it lowers to the name Extension; the Call node itself
            // carries the result type. Otherwise a typed core Ref (C-family).
            callee = makeNameNode(calleeNode, sym, calleeTy);
        } else {
            return exprError(callNode, "call has no callee child");
        }
        std::vector<HirNodeId> args;
        // If the callee resolves to a signature — a FnSig designator OR
        // a `Ptr<FnSig>` function-pointer value (FC4 c2: `calleeSigOf`
        // unwraps one level so indirect-call args coerce exactly like
        // direct-call args) — coerce each arg to its declared param
        // type. Variadic / unknown signatures pass through unchanged
        // (the verifier owns the arity-vs-FnSig rule).
        //
        // M2 silent-failure fix (3-agent root-cause analysis, 2026-06-02):
        // copy `interner.fnParams()`'s span into a stable owned vector
        // BEFORE the arg loop. The span points into the interner's
        // `operandPool_` (a `std::vector<TypeId>`) which could be
        // reallocated by ANY interner-growing call inside the loop
        // (e.g., `interner.pointer()` / `interner.array()` during a
        // nested arg lowering), silently dangling the span. Same
        // pattern applied at lowerPostfix's call arm.
        TypeId const calleeSig = calleeSigOf(calleeTy);
        std::vector<TypeId> paramTypes;
        if (calleeSig.valid()) {
            auto const paramSpan = interner.fnParams(calleeSig);
            paramTypes.assign(paramSpan.begin(), paramSpan.end());
        }
        std::size_t argIdx = 0;
        if (cr.argsChild < vis.size()) {
            for (NodeId a : visible(vis[cr.argsChild])) {
                if (isToken(a)) continue;                       // skip commas
                TypeId const paramType = (argIdx < paramTypes.size())
                                       ? paramTypes[argIdx] : InvalidType;
                // D5.3 cycle 1b.4: brace-init argument `f({1, 2})`
                // lowers via the shared helper with the callee's
                // FnSig param type pushed as the brace-init context.
                // For non-brace args the helper degrades to the same
                // `lowerExpr + coerce` shape, but the flat-expression
                // arm uses `lowerFlatExpr` (SQL-style flat expressions)
                // — handle that specifically.
                NodeId const core = peelToBraceInitOrCore(a);
                HirNodeId argNode;
                if (isBraceInitList(core)) {
                    argNode = lowerBraceInit(core, paramType);
                } else {
                    E const arg = lowerFlatExpr(a);
                    E const coerced = paramType.valid() ? coerce(arg, paramType)
                                                        : arg;
                    argNode = coerced.id;
                }
                args.push_back(argNode);
                ++argIdx;
            }
        }
        TypeId const result = calleeSig.valid()
                            ? interner.fnResult(calleeSig)
                            : typeAtOr(callNode, InvalidType);
        return {track(builder.makeCall(callee, args, result), callNode), result};
    }

    // ── HR10: generic extension-node lowering ──────────────────────────────────
    // Build a HirKind::Extension node of `m.hirKind`, gathering role children per
    // `m.childGathering`. Entirely config-driven — no language vocabulary here.
    HirNodeId lowerExtensionNode(NodeId cstNode, HirRuleMapping const& m) {
        HirKindId const kid = extKind(m.hirKind);
        std::vector<HirNodeId> children;
        for (ChildSlotSpec const& slot : m.childGathering) {
            if (slot.list) {
                // Lower EACH `matchRule` item of a comma-separated list. Some
                // grammars flatten `item (, item)*` (all items direct children);
                // others nest the tail in a repeat-group node. `collectListItems`
                // handles both by gathering matches in document order, stopping
                // descent at each match (list items don't nest), so it never
                // double-counts. A required (non-optional) list that matched no
                // item is a malformed tree — fail loud rather than emit an empty
                // grouping.
                std::vector<NodeId> items;
                collectListItems(cstNode, slot.matchRule, items);
                if (items.empty() && !slot.optional) {
                    children.push_back(reportedError(cstNode,
                        std::format("extension list slot '{}' matched no items", slot.role)));
                    continue;
                }
                for (NodeId item : items) children.push_back(lowerSlot(item, slot));
                continue;
            }
            NodeId child = slot.classifier == "expr"
                ? findExprChild(cstNode)
                : (slot.matchRule.valid() ? findChildByRule(cstNode, slot.matchRule) : NodeId{});
            if (!child.valid()) {
                if (!slot.optional)
                    children.push_back(reportedError(cstNode,
                        std::format("extension slot '{}' not found", slot.role)));
                continue;
            }
            children.push_back(lowerSlot(child, slot));
        }
        return track(builder.addParent(HirKind::Extension, children, typeAtOr(cstNode, InvalidType),
                                       kid.v), cstNode);
    }

    // Collect, in document order, every descendant of `parent` whose rule is
    // `matchRule`, descending through intervening wrapper nodes (e.g. a grammar's
    // repeat-group) but STOPPING at each match — so a same-rule list item nested
    // inside another (were a grammar to allow it) is not flattened into the outer
    // list. When `matchRule` is unset, gathers the direct internal children.
    void collectListItems(NodeId parent, RuleId matchRule, std::vector<NodeId>& out) {
        for (NodeId c : visible(parent)) {
            if (tree().kind(c) != NodeKind::Internal) continue;
            if (!matchRule.valid()) { out.push_back(c); continue; }
            if (tree().rule(c).v == matchRule.v) { out.push_back(c); continue; }  // matched: don't descend
            collectListItems(c, matchRule, out);   // descend through repeat-group wrappers
        }
    }

    // The outermost call-rule node within `subtree` (a node whose rule is in the
    // semantics `callRules`), or invalid if none. Descent stops at the first
    // match, so a call's own argument subtrees (which may contain nested calls)
    // are not mistaken for the operand's call.
    [[nodiscard]] NodeId findCallNode(NodeId subtree) const {
        std::vector<NodeId> stack{subtree};
        for (int guard = 0; guard < 4096 && !stack.empty(); ++guard) {
            NodeId n = stack.back(); stack.pop_back();
            if (tree().kind(n) == NodeKind::Internal && callMap_.count(tree().rule(n).v))
                return n;                                   // matched: don't descend
            for (NodeId k : visible(n)) stack.push_back(k);
        }
        return {};
    }

    // Lower one located child per the slot's `lower` verb. The loader validated
    // the verb against the closed ChildLower set, so the switch is exhaustive.
    HirNodeId lowerSlot(NodeId child, ChildSlotSpec const& slot) {
        switch (slot.lower) {
            case ChildLower::Expr:     return lowerExpr(child).id;
            case ChildLower::FlatExpr: return lowerFlatExpr(flatExprChildOf(child)).id;
            case ChildLower::Ext:      return lowerNestedExtension(child);
            case ChildLower::VarDecl:  return lowerVarLike(child, /*asGlobal=*/false);
            case ChildLower::Ref:      return nameRefExpr(child).id;
        }
        return reportedError(child, "unhandled childGathering lower verb");
    }

    // The name token inside `subtree` (e.g. a qualifiedName → nameAtom →
    // Identifier) and its resolved symbol. Prefers a symbol-bearing token (the
    // resolved last identifier — what `nameMatch: lastIdentifier` binds); when no
    // name resolves, falls back to the rightmost name token's span (the LIFO walk
    // pushes children in order and pops the deepest/rightmost first), which is the
    // qualified name's last segment — the right span for an unresolved column.
    struct NameHit { NodeId node{}; SymbolId sym{}; };
    [[nodiscard]] NameHit firstNameToken(NodeId subtree) const {
        NameHit hit;
        std::vector<NodeId> stack{subtree};
        for (int guard = 0; guard < 4096 && !stack.empty(); ++guard) {
            NodeId n = stack.back(); stack.pop_back();
            if (isNameToken(n)) {
                if (!hit.node.valid()) hit.node = n;            // rightmost name (for the span)
                if (model.symbolAt(n).valid()) { hit.sym = model.symbolAt(n); hit.node = n; break; }
            }
            for (NodeId k : visible(n)) stack.push_back(k);
        }
        return hit;
    }

    // A name reference as an expression (id + type): resolves the last identifier
    // the semantic phase bound inside `subtree` (e.g. a qualifiedName's last name)
    // and builds the configured node. When `cfg.refExtensionKind` is set the
    // language's names are relational, not typed value reads (SQL table/column
    // names), so this emits a leaf Extension node (no type requirement); otherwise
    // a core typed `Ref`. An unresolved name (sym 0) is fine — its text is
    // recoverable from source provenance (correct for SQL columns, which bind
    // relationally, not lexically). See `makeNameNode`.
    E nameRefExpr(NodeId subtree) {
        NameHit h = firstNameToken(subtree);
        if (!h.node.valid())   // a `ref`-lowered slot that holds no name token: fail loud
            return exprError(subtree, "name reference subtree has no name token");
        TypeId const t = typeAtOr(h.node, InvalidType);
        HirNodeId const node = makeNameNode(h.node, h.sym, t);
        return {node, cfg.refExtensionKind.empty() ? t : InvalidType};
    }

    // Emit a name reference: a leaf Extension of `cfg.refExtensionKind` when the
    // language declares one (SQL relational names — untyped), else a core typed
    // `Ref`. Single seam so every "name reference" site agrees.
    HirNodeId makeNameNode(NodeId at, SymbolId sym, TypeId type) {
        if (!cfg.refExtensionKind.empty())
            return track(builder.addLeaf(HirKind::Extension, InvalidType,
                                         extKind(cfg.refExtensionKind).v), at);
        return track(builder.makeRef(type, sym.v), at);
    }

    // `ext` slot: the child rule must itself be extension-mapped.
    HirNodeId lowerNestedExtension(NodeId child) {
        NodeId core = peelToCore(child);
        HirRuleMapping const* cm = mappingFor(core);
        if (cm && extKindByName_.count(cm->hirKind)) return lowerExtensionNode(core, *cm);
        return reportedError(child, "ext slot's child is not an extension-mapped rule");
    }

    // For a `flatExpr` slot whose match is a clause wrapper (e.g. whereClause =
    // [WHERE, expression]): descend to the flat-expression child. If the matched
    // node IS the flat-expression already, use it directly.
    [[nodiscard]] NodeId flatExprChildOf(NodeId node) {
        if (cfg.flatExprRule.valid() && tree().kind(node) == NodeKind::Internal
            && tree().rule(node).v == cfg.flatExprRule.v)
            return node;
        if (NodeId e = findChildByRule(node, cfg.flatExprRule); e.valid()) return e;
        return node;
    }

    [[nodiscard]] NodeId findChildByRule(NodeId parent, RuleId rule) {
        if (!rule.valid()) return {};
        for (NodeId c : visible(parent)) {
            if (tree().kind(c) == NodeKind::Internal && tree().rule(c).v == rule.v) return c;
        }
        return {};
    }
    [[nodiscard]] NodeId findExprChild(NodeId parent) {
        for (NodeId c : visible(parent))
            if (tree().kind(c) == NodeKind::Internal && isExprNode(peelToCore(c))) return c;
        return {};
    }

    E lowerLiteral(NodeId operandNode, NodeId tokenNode, SchemaTokenId tk, TypeId type) {
        TypeKind core = litCore_.at(tk.v);
        std::string_view const text = tree().text(tokenNode);
        HirLiteralValue val;          // value defaults to monostate (= undecodable)
        bool ok = true;
        // FC3 c1: keyword literals (`true` / `false`) carry their
        // config-declared fixed VALUE — never decoded from the token
        // text (decodeInteger("true") would silently yield 0).
        if (auto fixedIt = litFixed_.find(tk.v); fixedIt != litFixed_.end()) {
            if (isSignedCore(core)) val.value = fixedIt->second;
            else val.value = static_cast<std::uint64_t>(fixedIt->second);
        } else if (isFloatCore(core)) {
            // FC3.5 sweep-c2: float-literal suffix typing (C 6.4.4.2)
            // — the SAME shared rule the semantic tier ran in pass 2
            // (one implementation, two call sites — the integer
            // ladder's discipline): `1.5f` refines the literalTypes
            // F64 base to F32. Languages without the block keep the
            // base core exactly (toy / tsql — pinned).
            if (!sem.floatLiteralTyping.empty() && numberStyle != nullptr
                && numberStyle->emitKind.floating.valid()
                && tk == numberStyle->emitKind.floating) {
                auto const fk = typeFloatLiteral(
                    text, numberStyle, sem.floatLiteralTyping, dataModel_);
                if (fk.has_value()) {
                    core = *fk;
                    type = interner.primitive(core);
                } else {
                    // Loader invariant violated (uncovered suffix) —
                    // stay loud through the arm below, mirroring the
                    // integer ladder's NoRule handling.
                    ok = false;
                }
            }
            if (ok) {
                double const d = decodeFloat(text, numberStyle, ok);
                if (ok) val.value = d;
            }
        } else if (auto iv = decodeInteger(text, numberStyle)) {
            // FC3 c1: the integer-literal ladder (C 6.4.4.1) — the SAME
            // shared algorithm the semantic tier ran in pass 2 (plan-lock
            // C-2: both tiers type the literal; one implementation, two
            // call sites), refining the static `literalTypes` core by
            // magnitude / suffix-class / radix-class under the model.
            if (!sem.integerLiteralTyping.empty() && numberStyle != nullptr
                && numberStyle->emitKind.integer.valid()
                && tk == numberStyle->emitKind.integer) {
                auto const r = typeIntegerLiteral(
                    text, numberStyle, sem.integerLiteralTyping, dataModel_, *iv);
                if (r.status == IntegerLadderStatus::Typed) {
                    core = r.kind;
                    type = interner.primitive(core);
                } else {
                    // TooLarge / NoRule: the semantic tier already
                    // diagnosed (S_IntegerLiteralTooLarge / loader
                    // invariant) on the production path; direct-API
                    // lowering stays loud through the arm below.
                    ok = false;
                }
            }
            if (ok) {
                if (isSignedCore(core)) val.value = static_cast<std::int64_t>(*iv);
                else                    val.value = *iv;
            }
        } else {
            ok = false;             // integer overflow
        }
        val.core = core;
        if (!ok)
            unsupported(tokenNode, std::format("literal '{}' is out of range / undecodable", text));
        std::uint32_t const idx = literals.add(val);
        return {track(builder.makeLiteral(type, idx), operandNode), type};
    }

    // Combine two lowered operands under an ALREADY-RESOLVED binary-operator entry
    // `e`, anchoring provenance + diagnostics at `anchor`. Shared by the Pratt
    // (`lowerBinary`) and flat (`lowerFlatExpr`) paths so the logical-op special
    // cases and operator-result typing (comparison → Bool, else the left operand's
    // type) stay identical. Does NOT handle `Assign` (an expression-position
    // store) — that is Pratt-only and resolved by the caller before this point.
    E combineBinary(NodeId anchor, HirOperatorEntry const& e, E lhs, E rhs) {
        // LogicalAnd/Or: operands are CONDITION positions (short-circuit
        // semantics) — each non-Bool scalar operand takes the truthiness
        // `Ne(operand, 0)` test (C99 6.5.13p3 / 6.5.14p3 "compares
        // unequal to 0"), never a value-truncating Cast.
        if (e.target == "LogicalAnd" || e.target == "LogicalOr") {
            E const lb = coerceCondition(lhs, anchor);
            E const rb = coerceCondition(rhs, anchor);
            if (e.target == "LogicalAnd")
                return {track(builder.makeLogicalAnd(lb.id, rb.id, boolType()), anchor), boolType()};
            return {track(builder.makeLogicalOr(lb.id, rb.id, boolType()), anchor), boolType()};
        }
        auto op = coreOpFromName(e.target);
        if (!op || arityOf(*op) != HirOpArity::Binary) {
            unsupported(anchor, std::format("binary target '{}' is not a core binary operator", e.target));
            return {errorNode(anchor), InvalidType};
        }
        // FC3 c1 shifts under the `arithmeticConversions` block: C 6.5.7
        // — each operand integer-promotes INDEPENDENTLY and the result
        // type is the PROMOTED LEFT operand only (the count's type never
        // contributes; `i64 << u32` is I64, `u32 >> 1` is U32 → LShr).
        // The count is ALSO coerced to the compute type so both register
        // operands share a width (value-preserving for any in-range
        // count; an out-of-range count is UB in C). Block-less languages
        // keep the legacy both-coerce-to-common path below EXACTLY.
        if (arith_.has_value()
            && (*op == HirOpKind::Shl || *op == HirOpKind::Shr)) {
            TypeId const lp = integerPromotedType(interner, lhs.type, *arith_);
            E lc = lhs, rc = rhs;
            if (lp.valid()) {
                lc = coerce(lhs, lp);
                rc = coerce(rhs, lp);
            }
            TypeId const result =
                lp.valid() ? lp : (lhs.type.valid() ? lhs.type : rhs.type);
            return {track(builder.addParent(HirKind::BinaryOp,
                                            std::array{lc.id, rc.id},
                                            result, encodeOp(*op)), anchor),
                    result};
        }
        // C99 usual arithmetic conversions: both operands coerce to their
        // common type before the op. The result type is that common type
        // (or Bool for comparisons). The common type comes from the
        // language's `arithmeticConversions` block when declared (the
        // config-driven C 6.3.1.8 engine — FC3 c1), else the legacy
        // `TypeInterner::commonType` (toy/tsql — pinned byte-identical).
        // Either returns InvalidType for non-arithmetic operand pairs —
        // fall back to the prior "first valid type wins" rule so
        // non-arithmetic Refs (e.g. pointer + pointer comparisons) still
        // lower without losing structure.
        TypeId common = commonArithType(lhs.type, rhs.type);
        // `promoteComparisons: false` (config) keeps comparison operands
        // at their raw types — no conversion is materialized.
        if (arith_.has_value() && !arith_->promoteComparisons
            && isComparison(*op)) {
            common = InvalidType;
        }
        E lc = lhs, rc = rhs;
        if (common.valid()) {
            lc = coerce(lhs, common);
            rc = coerce(rhs, common);
        }
        TypeId const result = isComparison(*op) ? boolType()
                            : (common.valid() ? common
                                              : (lhs.type.valid() ? lhs.type : rhs.type));
        return {track(builder.addParent(HirKind::BinaryOp, std::array{lc.id, rc.id},
                                        result, encodeOp(*op)), anchor), result};
    }

    E lowerBinary(NodeId node) {
        // [lhs, OP-token, rhs]
        NodeId lhsN{}, rhsN{}, opTok{};
        for (NodeId c : visible(node)) {
            if (isToken(c)) { if (!opTok.valid()) opTok = c; continue; }
            if (!lhsN.valid()) lhsN = c; else if (!rhsN.valid()) rhsN = c;
        }
        if (!opTok.valid() || !lhsN.valid() || !rhsN.valid()) {
            unsupported(node, "malformed binary expression");
            return {errorNode(node), InvalidType};
        }
        auto it = binOp_.find(tree().tokenKind(opTok).v);
        if (it == binOp_.end()) {
            unsupported(node, std::format("binary operator '{}' has no hirLowering mapping",
                                          tree().text(opTok)));
            return {errorNode(node), InvalidType};
        }
        HirOperatorEntry const& e = cfg.binaryOps[it->second];
        // Assignment is a STATEMENT in HIR, but C lets it be used as a value
        // (`while ((c = f()) != EOF)`). Lower it as a SeqExpr that performs the
        // store then yields the stored value — the sound, position-independent
        // form (hoisting the store out would be wrong inside a loop condition).
        // Covers compound assignment too (`(x += 1)` reads, applies the op, writes).
        if (e.target == "Assign") {
            // lhsN / rhsN were already extracted by the scan above.
            auto lv = lhsN.valid() ? classifyLvalue(lhsN) : std::nullopt;
            if (!lv || !rhsN.valid())
                return exprError(node, "assignment sub-expression needs an lvalue and a value");
            HirNodeId stored;
            if (e.compoundBase.empty()) {
                stored = lowerExpr(rhsN).id;                        // plain `=`
            } else {
                auto op = coreOpFromName(e.compoundBase);           // `OP=`
                if (!op || arityOf(*op) != HirOpArity::Binary)
                    return exprError(node, std::format("compound base op '{}' is not binary",
                                                       e.compoundBase));
                stored = builder.addParent(HirKind::BinaryOp,
                                           std::array{lvRead(*lv), lowerExpr(rhsN).id},
                                           lv->type, encodeOp(*op));
            }
            std::vector<HirNodeId> stmts = lv->prep;
            stmts.push_back(lvWrite(*lv, stored));
            HirNodeId yield = lvRead(*lv);   // the new value (re-read of the lvalue)
            return {track(builder.makeSeqExpr(stmts, yield, lv->type, HirFlags::Synthetic), node),
                    lv->type};
        }
        // FC5: the comma operator `a, b` — evaluate `a` for its side effects and
        // DISCARD its value (an ExprStmt), then yield `b` (value + type). The
        // existing SeqExpr substrate models exactly this (and its MIR lowering
        // evaluates the effect-statements in order, then yields the result). Built
        // NON-synthetic (programmer source — carries the comma's own span). Chains
        // `a, b, c` nest left-assoc into Seq([ExprStmt(Seq([ExprStmt a], b))], c) =
        // evaluate a, b (discard), value c — correct C semantics.
        if (e.target == "Comma") {
            E lhsE = lowerExpr(lhsN);
            HirNodeId const effect = builder.makeExprStmt(lhsE.id);
            E rhsE = lowerExpr(rhsN);
            std::array<HirNodeId, 1> const stmts{effect};
            return {track(builder.makeSeqExpr(stmts, rhsE.id, rhsE.type, HirFlags::None), node),
                    rhsE.type};
        }
        return combineBinary(node, e, lowerExpr(lhsN), lowerExpr(rhsN));
    }

    E lowerUnary(NodeId node) {
        // [OP-token, operand]
        NodeId opTok{}, operandN{};
        for (NodeId c : visible(node)) {
            if (isToken(c)) { if (!opTok.valid()) opTok = c; continue; }
            if (!operandN.valid()) operandN = c;
        }
        if (!opTok.valid() || !operandN.valid()) {
            unsupported(node, "malformed unary expression");
            return {errorNode(node), InvalidType};
        }
        auto it = unOp_.find(tree().tokenKind(opTok).v);
        if (it == unOp_.end()) {
            unsupported(node, std::format("unary operator '{}' has no hirLowering mapping",
                                          tree().text(opTok)));
            return {errorNode(node), InvalidType};
        }
        E operand = lowerExpr(operandN);
        HirOperatorEntry const& e = cfg.unaryOps[it->second];
        if (e.target == "AddressOf") {
            TypeId const result = operand.type.valid() ? interner.pointer(operand.type) : InvalidType;
            return {track(builder.makeAddressOf(operand.id, result), node), result};
        }
        if (e.target == "Deref") {
            // FC4 c2 — C 6.5.3.2p4 designator decay as a lattice law:
            // `*` applied to a function pointer yields the function
            // DESIGNATOR, which (outside sizeof/&) immediately decays
            // back to the pointer — the deref is the IDENTITY. Return
            // the operand UNCHANGED (no Deref node): covers
            // `(*fp)(40)` and the recursive `(***fp)(x)` (each level
            // folds), and `(*helper)(40)` (operand's own type is the
            // FnSig designator). WITHOUT this fold a Deref node would
            // lower to a memory LOAD THROUGH the code pointer —
            // executing the callee's first 8 instruction bytes as an
            // address: silent garbage. Lattice-kind-driven only.
            if (operand.type.valid()) {
                TypeKind const opk = interner.kind(operand.type);
                if (opk == TypeKind::FnSig) return operand;
                if (opk == TypeKind::Ptr
                    && !interner.operands(operand.type).empty()
                    && interner.kind(interner.operands(operand.type)[0])
                           == TypeKind::FnSig) {
                    return operand;
                }
            }
            // The pointee-of derivation is the SINGLE source shared with the
            // semantic-tier expression typer (type_rules.hpp). On the non-
            // identity path here it yields Ptr<T>→T / non-Ptr→InvalidType,
            // exactly as the prior inline computation.
            TypeId const result = derefResultType(interner, operand.type);
            return {track(builder.makeDeref(operand.id, result), node), result};
        }
        auto op = coreOpFromName(e.target);
        if (!op || arityOf(*op) != HirOpArity::Unary) {
            unsupported(node, std::format("unary target '{}' is not a core unary operator", e.target));
            return {errorNode(node), InvalidType};
        }
        TypeId const result = (*op == HirOpKind::Not) ? boolType() : operand.type;
        return {track(builder.addParent(HirKind::UnaryOp, std::array{operand.id},
                                        result, encodeOp(*op)), node), result};
    }

    // `cond ? then : else` → a Ternary node. The wrapper holds
    // [cond, `?`, then, `:`, else]; the three operands are the visible non-token
    // children. Result type is the then-branch's (C requires then/else to be
    // compatible; the semantic phase already checked, and prefers a node type
    // when it set one).
    E lowerTernary(NodeId node) {
        std::vector<NodeId> operands;
        for (NodeId c : visible(node)) if (!isToken(c)) operands.push_back(c);
        if (operands.size() != 3) {
            unsupported(node, "malformed ternary expression (expected cond, then, else)");
            return {errorNode(node), InvalidType};
        }
        E cond = lowerExpr(operands[0]);
        // Truthiness at the Ternary boundary: a non-Bool scalar cond
        // becomes `Ne(cond, 0)` (C99 6.5.15p4 "compares unequal to 0"),
        // keeping the CondBr-expects-Bool discipline at MIR.
        cond = coerceCondition(cond, operands[0]);
        E thenE = lowerExpr(operands[1]);
        E elseE = lowerExpr(operands[2]);
        // Coerce both arms to their common type (C99 conditional-expression
        // type-balance rule — the config-driven UAC engine when the
        // language declares the block; FC3 c1). Falls back to the
        // type-attribute-or-then.
        TypeId const common = commonArithType(thenE.type, elseE.type);
        if (common.valid()) {
            thenE = coerce(thenE, common);
            elseE = coerce(elseE, common);
        }
        TypeId const result = common.valid() ? common
                            : typeAtOr(node, thenE.type.valid() ? thenE.type : elseE.type);
        return {track(builder.makeTernary(cond.id, thenE.id, elseE.id, result), node), result};
    }

    E lowerPostfix(NodeId node) {
        // [base, OP-token, body...]   (Call: body=argList; Index: body=expression)
        NodeId baseN{}, opTok{};
        std::vector<NodeId> rest;
        for (NodeId c : visible(node)) {
            if (!baseN.valid() && !isToken(c)) { baseN = c; continue; }
            if (isToken(c)) { if (!opTok.valid()) opTok = c; continue; }
            rest.push_back(c);
        }
        if (!baseN.valid() || !opTok.valid()) {
            unsupported(node, "malformed postfix expression");
            return {errorNode(node), InvalidType};
        }
        auto it = postOp_.find(tree().tokenKind(opTok).v);
        if (it == postOp_.end()) {
            unsupported(node, std::format("postfix operator '{}' has no hirLowering mapping "
                                          "(++/-- deferred to HR9)", tree().text(opTok)));
            return {errorNode(node), InvalidType};
        }
        HirOperatorEntry const& e = cfg.postfixOps[it->second];
        // Value-yielding `x++` / `x--` (postfix yields the OLD value): save it in
        // a temp, mutate the lvalue, yield the temp — all in a SeqExpr. Handled
        // before lowering `base` (classifyLvalue lowers the lvalue itself).
        if (e.target == "PostInc" || e.target == "PostDec") {
            auto lv = classifyLvalue(baseN);
            if (!lv) return exprError(node, "++/-- needs an lvalue operand");
            SymbolId const tmp = freshSymbol();
            std::vector<HirNodeId> stmts = lv->prep;
            stmts.push_back(builder.makeVarDecl(lv->type, tmp.v, lvRead(*lv), HirFlags::Synthetic));
            HirNodeId one = synthOne(lv->type);
            HirNodeId inc = builder.addParent(HirKind::BinaryOp, std::array{lvRead(*lv), one}, lv->type,
                                              encodeOp(e.target == "PostInc" ? HirOpKind::Add
                                                                             : HirOpKind::Sub));
            stmts.push_back(lvWrite(*lv, inc));
            HirNodeId yield = builder.makeRef(lv->type, tmp.v);
            return {track(builder.makeSeqExpr(stmts, yield, lv->type, HirFlags::Synthetic), node),
                    lv->type};
        }
        E base = lowerExpr(baseN);
        if (e.target == "Call") {
            std::vector<HirNodeId> args;
            // Coerce each arg to its declared param type when the
            // callee's signature is known — a FnSig designator OR a
            // `Ptr<FnSig>` function-pointer value (FC4 c2:
            // `calleeSigOf` unwraps one level so INDIRECT-call args
            // coerce exactly like direct-call args; skipping coercion
            // here would be the width/signedness miscompile class).
            // Same discipline as lowerSqlCall — single source of truth
            // for arg-coercion.
            //
            // M2 silent-failure fix (3-agent root-cause analysis,
            // 2026-06-02): `interner.fnParams()` returns a span into
            // `operandPool_` (a `std::vector<TypeId>`). If the arg-
            // coerce loop below triggers an interner reallocation
            // (e.g., a future arg shape that calls `interner.pointer()`
            // / `interner.array()` mid-loop via lowerExpr or coerce),
            // the span would dangle silently — wrong-type reads with
            // no diagnostic. Copy to a stable owned vector BEFORE the
            // loop so iterations are robust against ANY downstream
            // interner growth. Cost: one small heap allocation per
            // call site; rare alternative is a `std::array`-backed
            // small-buffer for ≤8-param calls (most calls).
            TypeId const calleeSig = calleeSigOf(base.type);
            std::vector<TypeId> paramTypes;
            if (calleeSig.valid()) {
                auto const paramSpan = interner.fnParams(calleeSig);
                paramTypes.assign(paramSpan.begin(), paramSpan.end());
            }
            std::size_t argIdx = 0;
            for (NodeId argN : argExpressions(rest)) {
                TypeId const paramType = (argIdx < paramTypes.size())
                                       ? paramTypes[argIdx] : InvalidType;
                // D5.3 cycle 1b.4: `f({1, 2})` lowers via the shared
                // helper with the callee's FnSig param type pushed as
                // the brace-init context.
                args.push_back(lowerExprOrBraceInit(argN, paramType));
                ++argIdx;
            }
            // Prefer the semantic phase's resolved call type (it types call nodes);
            // fall back to the callee signature's result (direct FnSig
            // or unwrapped Ptr<FnSig> — FC4 c2).
            TypeId inferred = InvalidType;
            if (calleeSig.valid()) inferred = interner.fnResult(calleeSig);
            TypeId const result = typeAtOr(node, inferred);
            return {track(builder.makeCall(base.id, args, result), node), result};
        }
        if (e.target == "Index") {
            HirNodeId idx = rest.empty() ? reportedError(node, "index has no subscript expression")
                                         : lowerExpr(rest.front()).id;
            // element-of-base derivation — the SINGLE source shared with the
            // semantic-tier typer (type_rules.hpp `indexResultType`).
            TypeId const inferred = indexResultType(interner, base.type);
            TypeId const result = typeAtOr(node, inferred);
            return {track(builder.makeIndex(base.id, idx, result), node), result};
        }
        // D5.1: `obj.field` and `ptr->field`. The semantic phase (Pass 2)
        // already resolved the field's SymbolId (via the `memberAccesses`
        // facet) and propagated its type to both the field-name leaf and
        // the postfixExpr node. We read fieldIndex off the field's
        // SymbolRecord (Pass 1 stamped it as the field's declaration-order
        // ordinal in its struct scope). The arrow form is desugared at HIR
        // level: `p->x` = `MemberAccess(Deref(p), idx)` — one HIR kind
        // handles both forms, downstream MIR sees uniform GEP-after-load
        // patterns.
        if (e.target == "MemberAccess" || e.target == "MemberAccessThruPtr") {
            // Locate the field-name token inside the follower subtree
            // (c-subset's `memberFollower = {sequence: [Identifier]}`). Robust
            // against a future schema that wraps the name (e.g. bracketed
            // identifiers): scan for a real token first, fall back to the
            // first visible child if the follower is all Internal.
            NodeId followerN = rest.empty() ? NodeId{} : rest.front();
            NodeId fieldNameN{};
            if (followerN.valid()) {
                for (NodeId c : visible(followerN)) {
                    if (isToken(c)) { fieldNameN = c; break; }
                    if (!fieldNameN.valid()) fieldNameN = c;
                }
            }
            if (!fieldNameN.valid()) {
                return exprError(node, "member access has no field-name leaf");
            }
            SymbolId const fieldSym = model.symbolAt(fieldNameN);
            if (!fieldSym.valid()) {
                return exprError(node, "member access field did not resolve "
                                       "to a symbol (semantic phase miss)");
            }
            auto const* frec = model.recordFor(fieldSym);
            if (frec == nullptr) {
                return exprError(node, "member access field SymbolId has no record");
            }
            // Defensive: the resolved symbol must be a field of a composite
            // type. Pass 2's member-access path always binds to a field
            // (struct-scope lookup), but a future Pass-2 recovery path that
            // falls back to enclosing-scope lookup could mis-bind to a
            // non-field symbol whose `fieldIndex` is just declaration-order
            // noise. Catch it here rather than emitting a structurally-valid
            // but semantically-wrong MemberAccess with a bogus index.
            if (!frec->scope.valid()
                || frec->kind != DeclarationKind::Variable) {
                return exprError(node, "member access resolved to a non-field "
                                       "symbol (semantic-phase mis-binding)");
            }
            std::uint32_t const fieldIndex = frec->fieldIndex;
            // Field type: prefer the semantic-phase-propagated type on the
            // field-name node; fall back to the symbol record's type.
            TypeId fieldType = model.typeAt(fieldNameN);
            if (!fieldType.valid()) fieldType = frec->type;
            HirNodeId object = base.id;
            if (e.target == "MemberAccessThruPtr") {
                // Arrow form: dereference the LHS pointer first. The Deref's
                // result type is the pointee type (Struct) — read from the
                // interner via the base's Ptr operand.
                TypeId pointeeType = InvalidType;
                if (base.type.valid()
                    && interner.kind(base.type) == TypeKind::Ptr
                    && !interner.operands(base.type).empty()) {
                    pointeeType = interner.operands(base.type)[0];
                }
                // Pass 2 also emitted S_NotAPointer if base.type wasn't a
                // pointer, but we still need a type here for the Deref node
                // to be HIR-verifier-valid (it requires a valid type). If
                // pointee is invalid, leave it InvalidType — the verifier's
                // requiresValidType rule will surface H_TypeUnresolved.
                object = track(builder.makeDeref(base.id, pointeeType,
                                                 HirFlags::Synthetic), node);
            }
            return {track(builder.makeMemberAccess(object, fieldIndex,
                                                   fieldType), node),
                    fieldType};
        }
        return exprError(node, std::format("postfix target '{}' has no lowering", e.target));
    }

    // The argument expressions inside an argList subtree (skip Comma tokens).
    [[nodiscard]] std::vector<NodeId> argExpressions(std::span<NodeId const> postfixRest) {
        std::vector<NodeId> out;
        for (NodeId r : postfixRest) {
            if (isToken(r)) continue;          // ')' etc.
            // r is the argList node; gather its expression children.
            for (NodeId c : visible(r)) {
                if (isToken(c)) continue;       // commas
                out.push_back(c);
            }
        }
        return out;
    }

    // ── statements ────────────────────────────────────────────────────────────
    HirNodeId lowerStmt(NodeId node) {
        HirRuleMapping const* m = mappingFor(node);
        if (m == nullptr) {
            // Transparent wrapper (e.g. `varDecl = [varDeclHead, ';']`): descend.
            NodeId only = soleMeaningfulChild(node);
            if (only.valid()) return lowerStmt(only);
            unsupported(node, "statement has no hirLowering mapping");
            return errorNode(node);
        }
        std::string const& k = m->hirKind;
        if (k == "VarDecl")     return lowerVarDecl(node);
        if (k == "TypeDecl")    return lowerTypeDecl(node);
        if (k == "Block")       return lowerBlock(node);
        if (k == "ExprStmt")    return lowerExprStmt(node);
        if (k == "ReturnStmt")  return lowerReturn(node);
        if (k == "BreakStmt")    return track(builder.makeBreak(0), node);
        if (k == "ContinueStmt") return track(builder.makeContinue(0), node);
        if (k == "IfStmt")      return lowerIf(node);
        if (k == "WhileStmt")   return lowerWhile(node, /*doWhile=*/false);
        if (k == "DoWhileStmt") return lowerWhile(node, /*doWhile=*/true);
        if (k == "ForStmt")     return lowerFor(node);
        if (k == "SwitchStmt")  return lowerSwitch(node);
        if (k == "GotoStmt")    return lowerGoto(node);
        if (k == "LabelStmt")   return lowerLabel(node);
        // FC5: an empty statement `;` lowers to a no-op (an empty Block: it lowers
        // to nothing in MIR, doesn't terminate, and emits no warning). `Skip` is
        // also the include-directive's kind, handled at the top-level decl path.
        if (k == "Skip")        return track(builder.makeBlock({}), node);
        // HR10: a rule mapped to a registered extension kind → an Extension node.
        if (extKindByName_.count(k)) return lowerExtensionNode(node, *m);
        unsupported(node, std::format("statement maps to unsupported HIR kind '{}'", k));
        return errorNode(node);
    }

    // FC5 — pre-assign a per-function ordinal to every label in `node`'s subtree
    // (labels are function-scoped + forward-referenceable, so all ordinals must
    // exist before `lowerGoto` runs). First-definition-wins; a same-name duplicate
    // keeps the first ordinal AND emits S_DuplicateLabel here (C 6.8.1).
    // Descends through nested blocks/
    // statements but does NOT cross into nested functions (the front-end has no
    // nested-function grammar; a function body subtree is self-contained).
    void prescanLabels(NodeId node) {
        if (!node.valid()) return;
        HirRuleMapping const* m = mappingFor(node);
        if (m != nullptr && m->hirKind == "LabelStmt") {
            NodeId const nameTok = firstIdentifierToken(node);
            if (nameTok.valid()) {
                auto const [it, inserted] = labelOrdinals_.try_emplace(
                    std::string{tree().text(nameTok)},
                    static_cast<std::uint32_t>(labelOrdinals_.size()));
                if (!inserted) {   // C 6.8.1: a label name has function scope
                    emitH(DiagnosticCode::S_DuplicateLabel, nameTok,
                          std::format("duplicate label '{}' in this function",
                                      tree().text(nameTok)));
                }
            }
        }
        for (NodeId c : visible(node))
            if (!isToken(c)) prescanLabels(c);
    }

    // `label: stmt` — carry the pre-scanned ordinal; lower the labeled statement
    // (the sole non-token child). The shared ordinal links it to its goto(s).
    HirNodeId lowerLabel(NodeId node) {
        NodeId const nameTok = firstIdentifierToken(node);
        NodeId bodyStmt{};
        for (NodeId c : visible(node)) { if (!isToken(c)) { bodyStmt = c; break; } }
        if (!nameTok.valid() || !bodyStmt.valid()) {
            unsupported(node, "malformed labeled statement");
            return errorNode(node);
        }
        auto it = labelOrdinals_.find(std::string{tree().text(nameTok)});
        if (it == labelOrdinals_.end()) {        // pre-scan covers every label
            unsupported(node, std::format("label '{}' was not pre-scanned",
                                          tree().text(nameTok)));
            return errorNode(node);
        }
        HirNodeId const bodyH = lowerStmt(bodyStmt);
        return track(builder.makeLabelStmt(it->second, bodyH), node);
    }

    // `goto label;` — resolve the target name to its pre-scanned ordinal. A miss
    // means no matching label in this function (C 6.8.6.1): emit the positioned
    // S_UndefinedLabel here (the pre-scan is the single label-collection site) and
    // fail loud — never a silent bad ordinal.
    HirNodeId lowerGoto(NodeId node) {
        NodeId const nameTok = firstIdentifierToken(node);
        if (!nameTok.valid()) {
            unsupported(node, "goto is missing a target label");
            return errorNode(node);
        }
        auto it = labelOrdinals_.find(std::string{tree().text(nameTok)});
        if (it == labelOrdinals_.end()) {   // C 6.8.6.1: goto needs a defined label
            emitH(DiagnosticCode::S_UndefinedLabel, nameTok,
                  std::format("goto target label '{}' is not defined in this function",
                              tree().text(nameTok)));
            return errorNode(node);
        }
        return track(builder.makeGotoStmt(it->second), node);
    }

    // A statement-context expression: an assignment becomes an AssignStmt (HIR
    // has no assignment expression); anything else wraps in ExprStmt.
    HirNodeId lowerStmtExpr(NodeId exprNode) {
        // Peel the `expression` wrapper so a top-level assignment is recognized
        // (C's assignment is an expression; HIR's AssignStmt is a statement, so
        // an assignment in statement position lowers to AssignStmt, not ExprStmt).
        return lowerStmtExprCore(peelToCore(exprNode), /*wrapBare=*/true);
    }

    HirNodeId lowerAssign(NodeId binNode) {
        NodeId lhsN{}, rhsN{};
        for (NodeId c : visible(binNode)) {
            if (isToken(c)) continue;
            if (!lhsN.valid()) lhsN = c; else if (!rhsN.valid()) rhsN = c;
        }
        if (!lhsN.valid() || !rhsN.valid())  // malformed (parse-recovery) node — never abort
            return reportedError(binNode, "malformed assignment expression");
        E const lhs = lowerExpr(lhsN);
        // D5.3 cycle 1b.4: `s = {.x = 1};` lowers via the same helper
        // as VarDecl init / return — push the LHS type as the brace-
        // init's context type. Assignment is asymmetric (rhs coerces
        // to lhs's type); the helper folds in the coerce step.
        HirNodeId const rhsId = lowerExprOrBraceInit(rhsN, lhs.type);
        return track(builder.makeAssignStmt(lhs.id, rhsId), binNode);
    }

    // A simple, side-effect-free lvalue (a plain variable reference): its CST
    // peels to an `operand` whose content is an Identifier. Such an lvalue can be
    // READ MORE THAN ONCE with no observable effect, which is what lets
    // compound-assignment and ++/-- lower by duplicating it. Returns (symbol,
    // type); nullopt for a complex lvalue (index / deref / call — those would
    // need once-only evaluation HIR can't express, and c-subset can't form them).
    [[nodiscard]] std::optional<std::pair<SymbolId, TypeId>> simpleLvalue(NodeId exprCst) {
        NodeId core = peelToCore(exprCst);
        if (tree().kind(core) != NodeKind::Internal || tree().rule(core).v != cfg.operandRule.v)
            return std::nullopt;
        for (NodeId c : visible(core)) {
            if (isToken(c) && sem.identifierToken.valid()
                && tree().tokenKind(c).v == sem.identifierToken.v)
                return std::pair{model.symbolAt(c), typeAtOr(core, InvalidType)};
        }
        return std::nullopt;
    }
    // A synthetic `1` literal of `type` (for ++/--). Synthetic ⇒ no source span.
    [[nodiscard]] HirNodeId synthOne(TypeId type) {
        TypeKind const core = type.valid() ? interner.kind(type) : TypeKind::I32;
        HirLiteralValue v;
        v.core = core;
        if (isFloatCore(core))       v.value = 1.0;
        else if (isSignedCore(core)) v.value = std::int64_t{1};
        else                         v.value = std::uint64_t{1};
        return builder.makeLiteral(type, literals.add(v), HirFlags::Synthetic);
    }

    // D5.3: synthetic zero-fill literal of `type`. For scalar types this is
    // `0` / `0.0` / `false`. For aggregate types (`Struct`/`Union`/`Array`)
    // this is a recursive `ConstructAggregate` whose every field/element is
    // zero-fill — the C99 §6.7.8p21 default for omitted aggregate initializer
    // elements. Used by `lowerBraceInit` to fill un-initialized slots before
    // emitting the final aggregate. Synthetic ⇒ no source span.
    //
    // `synthZeroOrError`: same shape, but the Array path requires a
    // well-formed (sized + element-typed) Array type. Malformed inputs
    // (empty ops / scalars) emit a diagnostic against `at` rather than
    // silently falling through to a scalar literal whose declared type
    // would be `Array` — a type-system corruption.
    [[nodiscard]] HirNodeId synthZeroOrError(NodeId at, TypeId type) {
        TypeKind const core = type.valid() ? interner.kind(type) : TypeKind::I32;
        // D5.4-FU3 + D5.5-FU3: unified composite arm — Struct, Union
        // and Enum all dispatch here. Per-kind child count: Struct =
        // every field; Union = first variant only (C99 §6.7.8p18+p21);
        // Enum = zero-as-underlying tagged with the enum's TypeId (so
        // the zero literal carries the enum's nominal identity).
        if (core == TypeKind::Struct || core == TypeKind::Union) {
            auto const ops = interner.operands(type);
            if (core == TypeKind::Union && ops.empty()) {
                return reportedError(at,
                    "synthZero reached a malformed Union type "
                    "(no variants)");
            }
            std::size_t const n =
                (core == TypeKind::Union) ? std::size_t{1} : ops.size();
            std::vector<HirNodeId> children;
            children.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                children.push_back(synthZeroOrError(at, ops[i]));
            }
            return builder.makeConstructAggregate(children, type, HirFlags::Synthetic);
        }
        if (core == TypeKind::Enum) {
            // The enum's underlying is in scalars[0]; the zero literal
            // is typed AS the enum (not as the raw underlying), so a
            // downstream consumer comparing TypeIds keeps the nominal
            // distinction. Empty scalars = malformed enum type → fail
            // loud (symmetric with the Union arm's malformed-variants
            // check; missed in the first FU3 cut, surfaced by the
            // silent-failure review).
            auto const scals = interner.scalars(type);
            if (scals.empty()) {
                return reportedError(at,
                    "synthZero reached a malformed Enum type "
                    "(no underlying scalar)");
            }
            TypeKind const underlying = static_cast<TypeKind>(scals[0]);
            HirLiteralValue v;
            v.core = underlying;
            if (isFloatCore(underlying))       v.value = 0.0;
            else if (isSignedCore(underlying)) v.value = std::int64_t{0};
            else                               v.value = std::uint64_t{0};
            return builder.makeLiteral(type, literals.add(v), HirFlags::Synthetic);
        }
        if (core == TypeKind::Array) {
            auto const ops   = interner.operands(type);
            auto const scals = interner.scalars(type);
            if (ops.empty() || scals.empty()) {
                return reportedError(at,
                    "synthZero reached a malformed Array type "
                    "(missing element type or length)");
            }
            TypeId const elemT = ops[0];
            auto   const len   = scals[0];
            std::vector<HirNodeId> children;
            children.reserve(static_cast<std::size_t>(len));
            for (std::uint32_t i = 0; i < len; ++i) {
                children.push_back(synthZeroOrError(at, elemT));
            }
            return builder.makeConstructAggregate(children, type, HirFlags::Synthetic);
        }
        HirLiteralValue v;
        v.core = core;
        if (isFloatCore(core))       v.value = 0.0;
        else if (isSignedCore(core)) v.value = std::int64_t{0};
        else                         v.value = std::uint64_t{0};
        return builder.makeLiteral(type, literals.add(v), HirFlags::Synthetic);
    }

    // Peel wrapper-rule layers off `n` UNTIL reaching `braceInitListRule`
    // (or until no more sole-meaningful descents are possible). Used by
    // D5.3 lowering: `peelToCore` over-peels through a single-element
    // braceInitList to its lone initElement, so callers that need to
    // recognize a braceInitList in init position must stop AT the rule
    // rather than past it. Returns the deepest reachable node; the
    // caller checks the rule.
    [[nodiscard]] NodeId peelToBraceInitOrCore(NodeId n) const {
        NodeId cur = n;
        while (tree().kind(cur) == NodeKind::Internal) {
            if (cfg.braceInitListRule.valid()
             && tree().rule(cur).v == cfg.braceInitListRule.v) break;
            NodeId const only = soleMeaningfulChild(cur);
            if (!only.valid()) break;
            cur = only;
        }
        return cur;
    }
    [[nodiscard]] bool isBraceInitList(NodeId n) const {
        return cfg.braceInitListRule.valid()
            && tree().kind(n) == NodeKind::Internal
            && tree().rule(n).v == cfg.braceInitListRule.v;
    }

    // D5-FU3: peel wrapper-rule layers off `n` UNTIL reaching one of the
    // recognized designator-leaf rules (designatedFieldRule /
    // designatedIndexRule), or until no more sole-meaningful descents
    // are possible. The c-subset grammar's `designator: alt[...]` parses
    // to an auto-interned alt-wrapper whose rule isn't either leaf rule;
    // callers that need to recognize a designator-leaf in initElement
    // position use this peel rather than `peelToCore` (which over-peels
    // through any single-child wrapper). Returns `{designatorCore, ruleIdValue}`
    // where `ruleIdValue` is 0 if the result isn't internal.
    [[nodiscard]] std::pair<NodeId, std::uint32_t>
    peelToDesignatorLeaf(NodeId n) const {
        NodeId cur = n;
        while (tree().kind(cur) == NodeKind::Internal) {
            std::uint32_t const rr = tree().rule(cur).v;
            if (cfg.designatedFieldRule.valid() && rr == cfg.designatedFieldRule.v) break;
            if (cfg.designatedIndexRule.valid() && rr == cfg.designatedIndexRule.v) break;
            NodeId const only = soleMeaningfulChild(cur);
            if (!only.valid()) break;
            cur = only;
        }
        std::uint32_t const r = (tree().kind(cur) == NodeKind::Internal)
                                    ? tree().rule(cur).v : std::uint32_t{0};
        return {cur, r};
    }

    // D5-FU3: find the first identifier token (the schema's
    // `sem.identifierToken`) among `parent`'s visible children. Returns
    // an invalid NodeId when no such token exists. Used by every
    // designator-name + lvalue path that needs to recover the name leaf
    // without a full peel.
    [[nodiscard]] NodeId firstIdentifierToken(NodeId parent) const {
        if (!sem.identifierToken.valid()) return {};
        for (NodeId t : visible(parent)) {
            if (isToken(t) && tree().tokenKind(t).v == sem.identifierToken.v) {
                return t;
            }
        }
        return {};
    }

    // D5.3 cycle 1b consolidated brace-init-aware lowering. Used by every
    // context-typing site (VarDecl init, return, call-arg, assign-RHS,
    // nested-brace inside lowerBraceInit) — detects a `braceInitList`
    // and routes to `lowerBraceInit(...)` with the surrounding context's
    // resolved target type; otherwise falls through to ordinary
    // expression lowering + coerce. Single source of truth for the
    // detection pattern, replacing what was 5 hand-rolled copies.
    [[nodiscard]] HirNodeId lowerExprOrBraceInit(NodeId valueNode,
                                                 TypeId contextType) {
        NodeId const core = peelToBraceInitOrCore(valueNode);
        if (isBraceInitList(core)) {
            return lowerBraceInit(core, contextType);
        }
        E const ve = lowerExpr(valueNode);
        E const coerced = coerce(ve, contextType);
        return coerced.id;
    }

    // D5.3 cycle 1b.3: compound literal `(T){...}` as an expression.
    // The grammar parses `compoundLiteralExpr = ParenOpen
    // typeRefAllowingStruct ParenClose braceInitList`; the type-ref
    // child resolves via the semantic phase's per-node type stamp.
    // The semantic phase stamps types on specific leaves (the resolved
    // name token of a struct, builtin keywords, etc.) — not on the
    // outer `typeRefAllowingStruct` wrapper — so recursively probe the
    // subtree until a stamped type is found.
    [[nodiscard]] TypeId resolveStampedTypeBelow(NodeId n) const {
        if (TypeId t = model.typeAt(n); t.valid()) return t;
        if (tree().kind(n) != NodeKind::Internal) return InvalidType;
        for (NodeId c : visible(n)) {
            if (TypeId t = resolveStampedTypeBelow(c); t.valid()) return t;
        }
        return InvalidType;
    }
    [[nodiscard]] E lowerCompoundLiteral(NodeId clNode) {
        NodeId typeRefN{}, braceN{};
        for (NodeId c : visible(clNode)) {
            if (isToken(c)) continue;
            if (tree().kind(c) != NodeKind::Internal) continue;
            if (isBraceInitList(c))                     braceN   = c;
            else if (!typeRefN.valid())                 typeRefN = c;
        }
        if (!typeRefN.valid() || !braceN.valid()) {
            return exprError(clNode,
                "compound literal is missing its type-ref or brace-init");
        }
        TypeId const type = resolveStampedTypeBelow(typeRefN);
        if (!type.valid()) {
            return exprError(clNode,
                "compound literal type-ref did not resolve to a type");
        }
        HirNodeId const agg = lowerBraceInit(braceN, type);
        return {track(agg, clNode), type};
    }

    // FC6: `sizeof ( type-name )` | `sizeof unary-expression` → core
    // `HirKind::SizeOf`, result type size_t (U64). The grammar's speculative
    // `sizeofExpr` wraps the chosen form (`sizeofType` = `sizeof ( castTypeRef )`,
    // `sizeofValue` = `sizeof castOperand`); in BOTH forms the operand carries the
    // semantic-stamped TYPE being sized, which `resolveStampedTypeBelow` recovers
    // by descending past the (unstamped) sizeof wrappers. The operand is
    // UNEVALUATED (C 6.5.3.4) — only its type reaches the node; the SizeOf folds to
    // that type's byte size via the `type_layout` engine at MIR lowering.
    [[nodiscard]] E lowerSizeof(NodeId node) {
        // The SIZED type lives on the OPERAND (the castTypeRef for `sizeof(T)`, the
        // unary-expr for `sizeof e`), which sits BELOW the form node that semantic
        // stamped size_t. Descend to the form, then scan its children for the
        // operand's stamped type — skipping the form's own size_t stamp.
        NodeId form{};
        for (NodeId c : visible(node)) {
            if (tree().kind(c) == NodeKind::Internal) { form = c; break; }
        }
        NodeId const scan = form.valid() ? form : node;
        TypeId sized = InvalidType;
        for (NodeId c : visible(scan)) {
            if (TypeId t = resolveStampedTypeBelow(c); t.valid()) { sized = t; break; }
        }
        if (!sized.valid()) {
            return exprError(node, "sizeof operand did not resolve to a type");
        }
        HirNodeId const tref = track(builder.makeTypeRef(sized), node);
        TypeId const u64 = interner.primitive(TypeKind::U64);
        return {track(builder.makeSizeOf(tref, u64), node), u64};
    }

    // FC2: explicit cast `(T)expr` (`hirLowering.castRule`). The grammar
    // parses `castExpr = ParenOpen typeRef ParenClose operandExpr`; the
    // FIRST internal child is the type-ref (its target type comes from
    // the semantic phase's per-node stamp — same probe as the compound
    // literal above), the SECOND is the operand expression. Lowers to a
    // core `HirKind::Cast` with EXPLICIT flags (HirFlags::None — NOT
    // Synthetic, which marks compiler-inserted coercions; an explicit
    // cast is programmer-written source). The semantic phase already
    // validated the (target, operand) pair against the explicit-cast
    // matrix (S_InvalidCast), so an unlowerable pair never reaches the
    // MIR mapCast lattice. Fail-loud on a missing stamp or child — a
    // castRule subtree the analyzer didn't type is a phase-ordering bug,
    // not a recoverable shape.
    [[nodiscard]] E lowerCast(NodeId castNode) {
        NodeId typeRefN{}, operandN{};
        for (NodeId c : visible(castNode)) {
            if (isToken(c)) continue;
            if (tree().kind(c) != NodeKind::Internal) continue;
            if (!typeRefN.valid())       typeRefN = c;
            else if (!operandN.valid()) { operandN = c; break; }
        }
        if (!typeRefN.valid() || !operandN.valid()) {
            return exprError(castNode,
                "cast expression is missing its type-ref or operand");
        }
        TypeId const target = resolveStampedTypeBelow(typeRefN);
        if (!target.valid()) {
            return exprError(castNode,
                "cast type-ref did not resolve to a type");
        }
        E operand = lowerExpr(operandN);
        // FC3.5 sweep-c3 (D-CSUBSET-CAST-VOID-DISCARD): `(void)expr`
        // is C's evaluate-and-discard idiom (6.3.2.2) — the operand
        // lowers for its effects and NO Cast node wraps it (mapCast
        // has no void arm by design; the discard is an expression-
        // statement effect, not a conversion). The void result type
        // makes any enclosing VALUE use a loud type mismatch.
        if (interner.kind(target) == TypeKind::Void) {
            return {operand.id, target};
        }
        // FC3.5 sweep-c3 (D-CSUBSET-CAST-ARRAY-DECAY): per C 6.3.2.1p3
        // the cast operand undergoes array-to-pointer decay BEFORE the
        // cast applies — `(char*)"str"` decays the Array<Char> literal
        // to Ptr<Char> first. Reuse `coerce`'s implicit-decay arm (the
        // SAME synthetic Cast mapCast already materializes via
        // GlobalAddr), then the explicit cast operates on the decayed
        // pointer (same-type → identity Bitcast; cross-type → the
        // standard Ptr↔Ptr / Ptr↔int arms).
        if (interner.kind(operand.type) == TypeKind::Array) {
            auto const elems = interner.operands(operand.type);
            if (!elems.empty()) {
                operand = coerce(operand, interner.pointer(elems[0]));
            }
        }
        HirNodeId const cast =
            builder.makeCast(operand.id, target, HirFlags::None);
        return {track(cast, castNode), target};
    }

    // D5.3 cycle 1b.2: resolve an `designatedIndex` CST `[i]` to an
    // integer offset by walking the wrapped expression to its leaf
    // token and decoding as an integer literal. Sufficient for the
    // realistic v1 corpus (`[0]` / `[7]` / `[0x10]` etc.). Arbitrary
    // const-expression indices are mapped as a real-blocker substrate
    // item (needs CST-side const-eval — the HIR builder is write-only
    // and `const_eval` consumes HIR). Returns nullopt + emits a real
    // diagnostic when the index isn't a recognizable integer literal.
    [[nodiscard]] std::optional<std::int64_t>
    resolveIndexDesignatorLiteral(NodeId diNode) {
        NodeId exprChild{};
        for (NodeId c : visible(diNode)) {
            if (isToken(c)) continue;
            if (tree().kind(c) == NodeKind::Internal) { exprChild = c; break; }
        }
        if (!exprChild.valid()) return std::nullopt;
        return evalCstConstInt(exprChild);
    }

    // Fold a CST expression to a compile-time `int64` through the shared
    // CST const-eval engine (plan 12.5 §0.2 D6). Folds literal
    // arithmetic / bitops / ternary / parens, plus identifier refs to
    // `isConst`-bound symbols resolved through the frozen SemanticModel.
    // Returns nullopt when the expression is not a foldable integer
    // constant. Single source of truth for "what is a compile-time
    // integer at this lowering tier" — shared by the index-designator
    // resolver and the provably-infinite-loop condition test, so the
    // two can never drift on what folds.
    [[nodiscard]] std::optional<std::int64_t> evalCstConstInt(NodeId exprNode) {
        std::unordered_set<std::uint32_t> intLits;
        for (auto const& [tok, kind] : litCore_) {
            if (!isFloatCore(kind)) intLits.insert(tok);
        }
        CstEvalContext ctx{tree(), tree().schema(), intLits, numberStyle};
        // Ref resolution: name → symbol via `symbolAt(identTok)` →
        // SymbolRecord. Only `isConst` symbols are foldable. The
        // shared `findInitExprInDecl` helper (in the engine library)
        // handles initChild + role-based discovery in one place,
        // keeping this site and the semantic-side resolver in lockstep.
        CstEvalEnvironment env;
        // HIR-lowering uses the frozen SemanticModel, whose `symbolAt`
        // is already use-site-aware (Pass 2 resolved every reference
        // at the CST position). Scope-context tracking via the engine
        // is unused here — the identifier-token NodeId carries its
        // own resolved binding. The scope arg is accepted to match
        // the resolver signature; the returned `initScopeOpaque`
        // is set to the symbol's own scope for parity with the
        // semantic-side resolver.
        env.resolveSymbolInit = [this](NodeId identTok, std::uint32_t /*curScope*/)
            -> std::optional<CstResolvedSymbol> {
            SymbolId const sym = model.symbolAt(identTok);
            if (!sym.valid()) return std::nullopt;
            SymbolRecord const* rec = model.recordFor(sym);
            if (rec == nullptr || !rec->isConst) return std::nullopt;
            if (!rec->declRuleNode.valid()) return std::nullopt;
            if (rec->tree.v != tree().id().v) return std::nullopt;
            for (auto const& dr : sem.declarations) {
                if (dr.rule.v == tree().rule(rec->declRuleNode).v) {
                    // FC4 c1: declarator-mode rows need the symbol's NAME
                    // node to pick the right init out of a multi-
                    // declarator list (`const int K = 8, L = 9;`).
                    auto initExpr = findInitExprInDecl(
                        tree(), dr, rec->declRuleNode, rec->declNode);
                    if (!initExpr.has_value()) return std::nullopt;
                    return CstResolvedSymbol{*initExpr, rec->scope.v};
                }
            }
            return std::nullopt;
        };
        ConstEvalResult const r = evaluateConstantCst(exprNode, ctx, env);
        if (!r.value.has_value()) return std::nullopt;
        return asInt64Bridge(*r.value);
    }

    // D5.3 cycle 1b InitSlot tree node. The intended invariant is
    // EITHER `value` set (direct element / leaf of a designator chain)
    // OR `nested` populated (in-progress sub-aggregate addressed by
    // deeper designators) OR neither (empty — flattens to
    // `synthZeroOrError`). The xor is maintained by the helper methods
    // (`writeInitSlotAt` clears `nested` when writing `value`;
    // `initSlotAsAggregate` resets `value` when growing `nested`;
    // `flattenInitSlot` reads `value` first). Do NOT mutate the fields
    // directly — go through the helpers, or convert to
    // `std::variant<monostate, HirNodeId, std::vector<InitSlot>>` if
    // direct-mutation paths grow (the variant rewrite is the compile-
    // checked form; cycle 1b chose the field form to keep diff size
    // small for the substrate-only landing).
    struct InitSlot {
        std::optional<HirNodeId> value;
        std::vector<InitSlot>    nested;
        TypeId                   slotType{};
    };
    // Idempotent: turn `s` into an in-progress sub-aggregate with one
    // nested slot per field/element of `s.slotType`. Discards a
    // previously-stored direct value (a later designator that addresses
    // a strict sub-position overrides the earlier wholesale write per
    // C99 §6.7.8p19's "later wins" rule).
    void initSlotAsAggregate(InitSlot& s) {
        if (!s.nested.empty()) return;
        s.value.reset();
        if (!s.slotType.valid()) return;
        TypeKind const k = interner.kind(s.slotType);
        if (k == TypeKind::Struct) {
            auto fields = interner.operands(s.slotType);
            s.nested.resize(fields.size());
            for (std::size_t i = 0; i < fields.size(); ++i)
                s.nested[i].slotType = fields[i];
        } else if (k == TypeKind::Array) {
            auto ops   = interner.operands(s.slotType);
            auto scals = interner.scalars(s.slotType);
            if (!ops.empty() && !scals.empty()) {
                s.nested.resize(scals[0]);
                for (auto& n : s.nested) n.slotType = ops[0];
            }
        }
    }
    // Write `val` at the slot reachable from `s` by following the path
    // of nested-slot indices. Out-of-range step → silent no-op (callers
    // bounds-check up front; this guard is defense-in-depth).
    void writeInitSlotAt(InitSlot& s,
                         std::span<std::uint32_t const> path,
                         HirNodeId val) {
        if (path.empty()) { s.nested.clear(); s.value = val; return; }
        initSlotAsAggregate(s);
        if (path[0] >= s.nested.size()) return;
        writeInitSlotAt(s.nested[path[0]], path.subspan(1), val);
    }
    // Flatten a slot to its HIR node: a direct value when set, a
    // recursive `ConstructAggregate` when sub-aggregating, or
    // `synthZeroOrError(at, type)` when empty.
    [[nodiscard]] HirNodeId flattenInitSlot(NodeId at, InitSlot const& s) {
        if (s.value.has_value()) return *s.value;
        if (s.nested.empty()) return synthZeroOrError(at, s.slotType);
        std::vector<HirNodeId> kids;
        kids.reserve(s.nested.size());
        for (auto const& n : s.nested) kids.push_back(flattenInitSlot(at, n));
        return builder.makeConstructAggregate(kids, s.slotType,
                                              HirFlags::Synthetic);
    }

    // D5.4: union brace-init lowering. Unions hold exactly ONE active
    // variant at a time; their brace-init must therefore initialize
    // exactly one of the declared variants. C99 §6.7.8p17–p18 (the
    // current-object framework + the "only the first named member of
    // a union" rule for no-designator initializers):
    //   • positional `{ expr }` → initializes the FIRST variant.
    //   • designator `{ .name = expr }` → initializes the named
    //     variant. With no other variants zero-filled (overlapping
    //     storage; only the chosen variant is live).
    //   • multiple elements → diagnostic. The grammar's brace-init
    //     allows N elements; the SEMANTICS for unions cap at 1.
    //   • chained designators `{.a.b = ...}` → diagnostic. Variant
    //     access has no sub-position semantics in C99; chained dot
    //     would walk INTO the chosen variant and is not yet supported.
    // Result: a 1-child `ConstructAggregate(value, contextType)`.
    // Empty `{}` produces the same shape as `synthZeroOrError(union)`
    // (first-variant zero-fill per C99 §6.7.8p21).
    [[nodiscard]] HirNodeId lowerUnionBraceInit(NodeId braceInitListNode,
                                                TypeId contextType) {
        auto const variants = interner.operands(contextType);
        if (variants.empty()) {
            return reportedError(braceInitListNode,
                "union brace-init target has no variants");
        }
        // Collect all initElement children up front so we can diagnose
        // multi-element forms before lowering anything.
        std::vector<NodeId> elements;
        for (NodeId c : visible(braceInitListNode)) {
            if (isToken(c)) continue;
            if (tree().kind(c) != NodeKind::Internal) continue;
            if (cfg.initElementRule.valid()
             && tree().rule(c).v == cfg.initElementRule.v) {
                elements.push_back(c);
            }
        }
        if (elements.empty()) {
            // Empty `{}` — default-initialize the first variant per
            // §6.7.8p10 (overlap with synthZeroOrError's union path).
            return synthZeroOrError(braceInitListNode, contextType);
        }
        if (elements.size() > 1) {
            reportedError(braceInitListNode,
                "union brace-init must initialize at most one variant");
            // Take the structurally-valid zero-fill path so the
            // pipeline downstream sees a typed aggregate without
            // having to discriminate "really succeeded" from
            // "succeeded with diagnostics". res->ok is already false.
            return synthZeroOrError(braceInitListNode, contextType);
        }
        NodeId const elem = elements[0];

        // Walk the initElement: find an optional `designatedField`
        // (designators decide WHICH variant); the value expression is
        // the trailing non-designator non-token child. Index designators
        // are nonsensical for unions (variants are name-indexed only).
        // Multiple designators in one element (chained `.a.b = ...`)
        // would walk INTO the chosen variant — diagnose, don't silently
        // last-win on the leaf.
        std::optional<std::uint32_t> targetVariant;
        bool failed = false;
        int designatorCount = 0;
        NodeId valueExprCst{};
        for (NodeId c : visible(elem)) {
            if (isToken(c)) continue;
            if (tree().kind(c) != NodeKind::Internal) continue;
            auto const [designatorCore, r] = peelToDesignatorLeaf(c);
            if (cfg.designatedFieldRule.valid()
             && r == cfg.designatedFieldRule.v) {
                ++designatorCount;
                if (designatorCount > 1) {
                    reportedError(designatorCore,
                        "chained designator on a union is not supported "
                        "(a union initializer must select exactly one "
                        "variant)");
                    failed = true;
                    continue;
                }
                NodeId const nameTok = firstIdentifierToken(designatorCore);
                if (!nameTok.valid()) {
                    reportedError(designatorCore,
                        "variant designator is missing its name");
                    failed = true;
                    continue;
                }
                ScopeId const unionScope =
                    model.compositeScopeFor(contextType);
                if (!unionScope.valid()) {
                    reportedError(designatorCore,
                        "could not resolve members of the target union "
                        "type");
                    failed = true;
                    continue;
                }
                std::string const name{tree().text(nameTok)};
                auto const& scope = model.scopeRecord(unionScope);
                auto sit = scope.bindings.find(name);
                if (sit == scope.bindings.end()) {
                    reportedError(designatorCore,
                        "designator names a variant that doesn't belong "
                        "to the target union type");
                    failed = true;
                    continue;
                }
                auto const* rec = model.recordFor(sit->second);
                if (rec == nullptr || rec->kind != DeclarationKind::Variable) {
                    reportedError(designatorCore,
                        "variant designator resolved to a non-variant "
                        "symbol");
                    failed = true;
                    continue;
                }
                if (rec->fieldIndex >= variants.size()) {
                    reportedError(designatorCore,
                        "union variant index out of range");
                    failed = true;
                    continue;
                }
                targetVariant = rec->fieldIndex;
                continue;
            }
            if (cfg.designatedIndexRule.valid()
             && r == cfg.designatedIndexRule.v) {
                ++designatorCount;
                reportedError(designatorCore,
                    "index designators are not meaningful on union types");
                failed = true;
                continue;
            }
            valueExprCst = c;
        }
        if (failed) {
            // Still emit a structurally-valid (first-variant zero-fill)
            // aggregate so downstream lowering doesn't cascade. res->ok
            // is already false via reportedError.
            return synthZeroOrError(braceInitListNode, contextType);
        }
        if (!valueExprCst.valid()) {
            // `union U u = { };` — already handled at the empty-list
            // check above; reaching here implies a malformed initElement.
            reportedError(elem, "union init element has no value expression");
            return synthZeroOrError(braceInitListNode, contextType);
        }
        std::uint32_t const variant = targetVariant.value_or(0);
        TypeId const variantType = variants[variant];
        HirNodeId const valueNode =
            lowerExprOrBraceInit(valueExprCst, variantType);

        // Union HIR shape: a 1-child ConstructAggregate whose single
        // child is the chosen variant's value. The variant index is
        // implicit-by-type (the value's HIR type identifies WHICH
        // variant); a future explicit-tag substrate can layer an
        // index attribute when codegen needs it.
        std::vector<HirNodeId> children{ valueNode };
        return builder.makeConstructAggregate(children, contextType,
                                              HirFlags::Synthetic);
    }

    // D5.3 brace-init lowering. Takes a `braceInitList` CST node and a
    // CONTEXT TYPE (the resolved type the brace-init must produce — a
    // struct or array). Produces a positional `HirKind::ConstructAggregate`
    // whose every slot is set: explicit elements at their chosen
    // position, omitted slots zero-filled via `synthZeroOrError(fieldType)`.
    // Supports:
    //   • positional elements `{a, b, c}` with C99 §6.7.8 fill-cursor
    //   • single-level field designator `{.x = a, .y = b}`
    //   • dot-chained field designator `{.a.v = 1}` (SP3 — type-aware
    //     name lookup via `compositeScopeFor(currentType)` + cursor
    //     descent into the resolved field's type)
    //   • index designator `{[2] = a}` with integer-literal indices
    //   • mixed positional / designator with cursor restart at the
    //     designated position (§6.7.8p17)
    //   • chained-brace nesting `{.outer = {.inner = a}}` via recursion
    //   • zero-fill omitted slots (§6.7.8p21)
    //
    // One real-blocker substrate item remaining:
    //   • index-designator `[expr] = ...` with non-literal indices —
    //     requires CST-side const-eval (HIR builder is write-only and
    //     `const_eval` consumes HIR). Anchored at plan 12.5 §0.2 D6.
    //     Locked-in by `D5_3_NonLiteralIndexDesignatorEmitsDiag`.
    //
    // Union brace-init is routed to `lowerUnionBraceInit` above
    // (separate semantics — one active variant). D5.4 ✅.
    [[nodiscard]] HirNodeId lowerBraceInit(NodeId braceInitListNode,
                                           TypeId contextType) {
        if (!contextType.valid()) {
            return reportedError(braceInitListNode,
                "brace-init requires a known context type");
        }
        TypeKind const containerKind = interner.kind(contextType);
        bool const isArray  = (containerKind == TypeKind::Array);
        bool const isStruct = (containerKind == TypeKind::Struct);
        bool const isUnion  = (containerKind == TypeKind::Union);
        if (!isArray && !isStruct && !isUnion) {
            return reportedError(braceInitListNode,
                "brace-init target type must be struct, union, or array");
        }
        // D5.4: union brace-init has distinct semantics from struct —
        // at most ONE element, initializing exactly one variant.
        // Positional → first variant; designator → that variant. No
        // zero-fill across overlapping variants. Route to a dedicated
        // path; the rest of this function handles struct + array.
        if (isUnion) {
            return lowerUnionBraceInit(braceInitListNode, contextType);
        }
        std::uint32_t slotCount = 0;
        TypeId elemTypeForArray{};
        std::span<TypeId const> structFields{};
        if (isStruct) {
            structFields = interner.operands(contextType);
            slotCount = static_cast<std::uint32_t>(structFields.size());
        } else {
            auto const scals = interner.scalars(contextType);
            auto const ops   = interner.operands(contextType);
            if (!scals.empty()) slotCount = scals[0];
            if (!ops.empty())   elemTypeForArray = ops[0];
        }
        if (slotCount == 0) {
            return reportedError(braceInitListNode,
                "brace-init target type has zero slots");
        }
        auto slotType = [&](std::uint32_t i) -> TypeId {
            return isStruct ? structFields[i] : elemTypeForArray;
        };

        // Root level of the InitSlot tree — one slot per top-level
        // field/element. Single-designator writes have empty residual
        // path (store directly at the slot); dot-chained writes have
        // a non-empty residual that descends into the slot's `nested`
        // sub-aggregate via `writeInitSlotAt`.
        std::vector<InitSlot> rootSlots(slotCount);
        for (std::uint32_t i = 0; i < slotCount; ++i)
            rootSlots[i].slotType = slotType(i);

        // SP3.c: type-aware field-designator resolution is now inlined
        // into the initElement-walk loop below — the resolver threads a
        // `designatorCurrentType` cursor through each chained step so
        // `.a.b = 1` resolves `.b` in field `.a`'s type's scope.

        std::uint32_t cursor = 0;
        for (NodeId elem : visible(braceInitListNode)) {
            if (isToken(elem)) continue;
            if (tree().kind(elem) != NodeKind::Internal) continue;
            if (!cfg.initElementRule.valid()
             || tree().rule(elem).v != cfg.initElementRule.v) continue;

            // SP3.c: walk the initElement's children collecting a FULL
            // designator path (single OR dot-chained). At each step we
            // descend into the type that the previous step pointed to,
            // so a chain like `.a.v = 1` resolves `.v` in field `.a`'s
            // struct scope (the InitSlot tree's `nested` substrate is
            // what makes the multi-step write semantically right).
            std::vector<std::uint32_t> designatorPath;
            TypeId designatorCurrentType = contextType;
            bool designatorFailed = false;
            NodeId valueExprCst{};
            for (NodeId c : visible(elem)) {
                if (isToken(c)) continue;
                if (tree().kind(c) != NodeKind::Internal) continue;
                // D5-FU3 helper: peel through the auto-interned
                // `designator` alt-wrapper to a designator leaf rule.
                auto const [designatorCore, r] = peelToDesignatorLeaf(c);
                if (cfg.designatedFieldRule.valid()
                 && r == cfg.designatedFieldRule.v) {
                    // Resolve `.name` against the CURRENT type's scope.
                    // For the first designator, current=contextType; for
                    // each subsequent step, current= the resolved
                    // field's type (descends per the C99 chain rule).
                    NodeId const nameTok = firstIdentifierToken(designatorCore);
                    if (!nameTok.valid()) {
                        reportedError(designatorCore,
                            "field designator missing name token");
                        designatorFailed = true;
                        continue;
                    }
                    ScopeId const structScope =
                        model.compositeScopeFor(designatorCurrentType);
                    if (!structScope.valid()) {
                        reportedError(designatorCore,
                            "field designator's container is not a struct");
                        designatorFailed = true;
                        continue;
                    }
                    std::string const name{tree().text(nameTok)};
                    auto const& scope = model.scopeRecord(structScope);
                    auto sit = scope.bindings.find(name);
                    if (sit == scope.bindings.end()) {
                        reportedError(designatorCore,
                            "field designator names a field that doesn't "
                            "belong to the target struct type");
                        designatorFailed = true;
                        continue;
                    }
                    auto const* rec = model.recordFor(sit->second);
                    if (rec == nullptr || rec->kind != DeclarationKind::Variable) {
                        reportedError(designatorCore,
                            "field designator resolved to a non-field symbol");
                        designatorFailed = true;
                        continue;
                    }
                    designatorPath.push_back(rec->fieldIndex);
                    designatorCurrentType = rec->type;
                    continue;
                }
                if (cfg.designatedIndexRule.valid()
                 && r == cfg.designatedIndexRule.v) {
                    auto idx = resolveIndexDesignatorLiteral(designatorCore);
                    if (!idx.has_value()) {
                        reportedError(designatorCore,
                            "index designator must be an integer literal");
                        designatorFailed = true;
                        continue;
                    }
                    // Descend into the array element's type (so a
                    // subsequent designator can target a sub-position).
                    // Invalid current type (prior chain step landed on
                    // an unresolved field) → fail LOUD; without this
                    // arm the index would silently append to the path
                    // and `writeInitSlotAt` would no-op past an empty
                    // `nested`, dropping the init silently.
                    if (!designatorCurrentType.valid()) {
                        reportedError(designatorCore,
                            "index designator on an unresolved or "
                            "invalid prior-step type");
                        designatorFailed = true;
                        continue;
                    }
                    if (interner.kind(designatorCurrentType)
                        != TypeKind::Array) {
                        reportedError(designatorCore,
                            "index designator on a non-array type");
                        designatorFailed = true;
                        continue;
                    }
                    auto ops = interner.operands(designatorCurrentType);
                    if (ops.empty()) {
                        reportedError(designatorCore,
                            "index designator's array type has no "
                            "element type");
                        designatorFailed = true;
                        continue;
                    }
                    designatorCurrentType = ops[0];
                    designatorPath.push_back(
                        static_cast<std::uint32_t>(*idx));
                    continue;
                }
                valueExprCst = c;
            }
            if (designatorFailed) continue;
            if (!valueExprCst.valid()) {
                reportedError(elem,
                    "init element has no value expression");
                continue;
            }

            // Determine the OUTER target slot index + the residual path
            // for nested writes.
            std::uint32_t target = cursor;
            std::span<std::uint32_t const> residualPath;
            if (!designatorPath.empty()) {
                target = designatorPath[0];
                cursor = target;
                residualPath = std::span<std::uint32_t const>{
                    designatorPath}.subspan(1);
            }
            if (target >= slotCount) {
                reportedError(elem,
                    "init element targets position out of aggregate range");
                continue;
            }
            // The value's target type is the slot's type AFTER following
            // the designator path. When no path, slotType(target).
            TypeId const valueTargetType =
                designatorPath.empty() ? rootSlots[target].slotType
                                       : designatorCurrentType;

            HirNodeId const valueNode =
                lowerExprOrBraceInit(valueExprCst, valueTargetType);

            // `writeInitSlotAt(slot, residualPath, value)` writes value
            // at the slot reachable from `slot` by the residual path;
            // single-level designators have an empty residual, while
            // dot-chained designators have a non-empty residual that
            // descends into nested sub-aggregates.
            writeInitSlotAt(rootSlots[target], residualPath, valueNode);
            cursor = target + 1;
        }

        std::vector<HirNodeId> children;
        children.reserve(slotCount);
        for (auto const& s : rootSlots)
            children.push_back(flattenInitSlot(braceInitListNode, s));
        return builder.makeConstructAggregate(children, contextType,
                                              HirFlags::Synthetic);
    }

    // A fresh SymbolId for a lowering-synthesized temporary, minted above the
    // semantic symbol table so it can never collide with a source symbol. These
    // temps are self-contained (declared + referenced within one SeqExpr/Block),
    // so they need no name table — the `.dsshir` writer falls back to a `%sN`
    // handle and MIR maps them by their VarDecl node, not by an external table.
    [[nodiscard]] SymbolId freshSymbol() {
        if (nextSyntheticSym_ == 0)
            nextSyntheticSym_ = static_cast<std::uint32_t>(model.symbols().size());
        return SymbolId{nextSyntheticSym_++};
    }
    std::uint32_t nextSyntheticSym_ = 0;

    // A resolved lvalue: how to READ its current value and WRITE a new one, plus
    // the prep statements that must run FIRST. A SIMPLE variable lvalue needs no
    // prep (reading a `Ref` is side-effect-free, so it can be read repeatedly). A
    // COMPLEX lvalue (an index / deref whose address sub-expressions may have
    // side effects) binds its ADDRESS into a temp pointer once in `prep`, then
    // reads/writes through `*ptr` — so `a[f()] += 1` evaluates `f()` exactly
    // once. This is what makes compound-assign / ++ / assignment-as-value correct
    // for every lvalue, not just simple variables.
    struct Lvalue {
        bool                   simple = true;
        TypeId                 type{};       // the lvalue's value type
        SymbolId               sym{};        // simple: the variable; via-ptr: the temp pointer
        TypeId                 ptrType{};    // via-ptr only: interner.pointer(type)
        std::vector<HirNodeId> prep;         // via-ptr only: [ var ptr = &<lvalue> ]
    };

    [[nodiscard]] HirNodeId lvRead(Lvalue const& lv) {
        if (lv.simple) return builder.makeRef(lv.type, lv.sym.v);
        return builder.makeDeref(builder.makeRef(lv.ptrType, lv.sym.v), lv.type, HirFlags::Synthetic);
    }
    [[nodiscard]] HirNodeId lvWrite(Lvalue const& lv, HirNodeId value) {
        HirNodeId target = lv.simple
            ? builder.makeRef(lv.type, lv.sym.v)
            : builder.makeDeref(builder.makeRef(lv.ptrType, lv.sym.v), lv.type, HirFlags::Synthetic);
        return builder.makeAssignStmt(target, value);
    }

    // Classify an lvalue CST. A plain variable → simple (no prep). Anything else
    // (index / deref) → via a temp pointer bound in `prep`. nullopt when the
    // lvalue can't be lowered (no resolved type / not an addressable form).
    [[nodiscard]] std::optional<Lvalue> classifyLvalue(NodeId exprCst) {
        if (auto s = simpleLvalue(exprCst)) {
            Lvalue lv; lv.simple = true; lv.sym = s->first; lv.type = s->second;
            if (!lv.type.valid()) return std::nullopt;
            return lv;
        }
        E target = lowerExpr(peelToCore(exprCst));
        if (!target.type.valid()) return std::nullopt;
        Lvalue lv;
        lv.simple  = false;
        lv.type    = target.type;
        lv.ptrType = interner.pointer(target.type);
        lv.sym     = freshSymbol();
        HirNodeId addr = builder.makeAddressOf(target.id, lv.ptrType, HirFlags::Synthetic);
        lv.prep.push_back(builder.makeVarDecl(lv.ptrType, lv.sym.v, addr, HirFlags::Synthetic));
        return lv;
    }

    // Wrap [prep..., assign] as a single statement: the bare assign when there's
    // no prep (simple lvalue), else a Block (complex lvalue's temp-pointer bind +
    // the store). Used by statement-position compound-assign / ++.
    [[nodiscard]] HirNodeId asStmt(Lvalue const& lv, HirNodeId assign, NodeId cst) {
        if (lv.prep.empty()) return track(assign, cst);
        std::vector<HirNodeId> stmts = lv.prep;
        stmts.push_back(assign);
        return track(builder.makeBlock(stmts), cst);
    }

    // `lhs OP= rhs` → `lhs = lhs OP rhs` (statement). Safe only for a simple
    // lvalue (duplicating the read has no effect); complex lvalues fail loud.
    HirNodeId lowerCompoundAssign(NodeId binNode, std::string const& baseOpName) {
        NodeId lhsN{}, rhsN{};
        for (NodeId c : visible(binNode)) {
            if (isToken(c)) continue;
            if (!lhsN.valid()) lhsN = c; else if (!rhsN.valid()) rhsN = c;
        }
        auto op = coreOpFromName(baseOpName);
        auto lv = lhsN.valid() ? classifyLvalue(lhsN) : std::nullopt;
        if (!lv || !rhsN.valid() || !op || arityOf(*op) != HirOpArity::Binary)
            return reportedError(binNode, "compound assignment needs an lvalue and a binary base op");
        E rhs = lowerExpr(rhsN);
        // C99 compound-assign spec: `a OP= b` ≡ `a = (T)((a) OP (b))` where
        // T is the type of `a`, and OP is computed at the COMMON type of a
        // and b (so a narrower-than-int operand is integer-promoted first).
        // Implement that exactly: read lhs, coerce both to common, OP, then
        // narrow result back to lhs's type for the store. (FC3 c1: the
        // common type comes from the language's UAC block when declared.)
        HirNodeId const lhsRead = lvRead(*lv);
        TypeId const common = commonArithType(lv->type, rhs.type);
        E lhsE{lhsRead, lv->type};
        E rhsE = rhs;
        TypeId const opType = common.valid() ? common : lv->type;
        if (common.valid()) {
            lhsE = coerce(lhsE, common);
            rhsE = coerce(rhsE, common);
        }
        HirNodeId const opResult = track(builder.addParent(
            HirKind::BinaryOp, std::array{lhsE.id, rhsE.id}, opType,
            encodeOp(*op)), binNode);
        // Narrow back to lhs's type before the store (if different).
        E const narrowed = coerce(E{opResult, opType}, lv->type);
        return asStmt(*lv, lvWrite(*lv, narrowed.id), binNode);
    }

    // `x++` / `x--` in STATEMENT position → `x = x +/- 1` (the produced value is
    // discarded). Value-yielding ++/-- (e.g. `y = x++`) lowers via a SeqExpr in
    // lowerPostfix.
    HirNodeId lowerIncDecStmt(NodeId postfixNode, bool isInc) {
        NodeId baseN{};
        for (NodeId c : visible(postfixNode)) { if (!isToken(c)) { baseN = c; break; } }
        auto lv = baseN.valid() ? classifyLvalue(baseN) : std::nullopt;
        if (!lv) return reportedError(postfixNode, "++/-- needs an lvalue operand");
        HirNodeId one = synthOne(lv->type);
        HirNodeId value = track(builder.addParent(HirKind::BinaryOp, std::array{lvRead(*lv), one},
                                                  lv->type,
                                                  encodeOp(isInc ? HirOpKind::Add : HirOpKind::Sub)),
                                postfixNode);
        return asStmt(*lv, lvWrite(*lv, value), postfixNode);
    }

    // The statement-position dispatch shared by exprStmt and for-init/update:
    // assignment / compound-assignment / inc-dec become statements; anything else
    // is the bare lowered expression (wrapped in ExprStmt when `wrapBare`).
    HirNodeId lowerStmtExprCore(NodeId core, bool wrapBare) {
        if (tree().kind(core) == NodeKind::Internal) {
            std::uint32_t const rv = tree().rule(core).v;
            if (rv == cfg.binaryExprRule.v) {
                for (NodeId c : visible(core)) {
                    if (!isToken(c)) continue;
                    auto it = binOp_.find(tree().tokenKind(c).v);
                    if (it != binOp_.end() && cfg.binaryOps[it->second].target == "Assign") {
                        std::string const& base = cfg.binaryOps[it->second].compoundBase;
                        return base.empty() ? lowerAssign(core) : lowerCompoundAssign(core, base);
                    }
                    break;  // first token is the operator
                }
            } else if (rv == cfg.postfixExprRule.v) {
                for (NodeId c : visible(core)) {
                    if (!isToken(c)) continue;
                    auto it = postOp_.find(tree().tokenKind(c).v);
                    if (it != postOp_.end()) {
                        std::string const& t = cfg.postfixOps[it->second].target;
                        if (t == "PostInc") return lowerIncDecStmt(core, /*isInc=*/true);
                        if (t == "PostDec") return lowerIncDecStmt(core, /*isInc=*/false);
                    }
                    break;
                }
            }
        }
        HirNodeId e = lowerExpr(core).id;
        return wrapBare ? track(builder.makeExprStmt(e), core) : e;
    }

    HirNodeId lowerExprStmt(NodeId node) {
        for (NodeId c : visible(node)) if (!isToken(c)) return lowerStmtExpr(c);
        unsupported(node, "expression statement has no expression");
        return errorNode(node);
    }

    HirNodeId lowerBlock(NodeId node) {
        std::vector<HirNodeId> stmts;
        for (NodeId c : visible(node)) {
            if (isToken(c)) continue;  // { }
            stmts.push_back(lowerStmt(c));
        }
        return track(builder.makeBlock(stmts), node);
    }

    HirNodeId lowerReturn(NodeId node) {
        for (NodeId c : visible(node)) {
            if (isToken(c)) continue;
            // D5.3 cycle 1b.4: `return {1, 2};` lowers via the same
            // helper as VarDecl init — push the enclosing function's
            // declared return type as the brace-init's context type.
            // `currentReturnType_` is set by `lowerFunctionDecl` before
            // walking the body; absent (Invalid) outside any function
            // body — in which case lowerExprOrBraceInit's coerce path
            // is a no-op.
            HirNodeId const v = lowerExprOrBraceInit(c, currentReturnType_);
            return track(builder.makeReturn(v), node);
        }
        return track(builder.makeReturn(std::nullopt), node);
    }

    HirNodeId lowerIf(NodeId node) {
        std::optional<HirNodeId> cond;
        std::vector<HirNodeId> bodies;
        for (NodeId c : visible(node)) {
            if (isToken(c)) continue;
            Role const role = classify(c);
            if (role == Role::Expr && !cond) {
                E const condE = lowerExpr(c);
                cond = coerceCondition(condE, c).id;
            }
            else if (role == Role::Stmt)     bodies.push_back(lowerStmt(c));
        }
        HirNodeId condId = orError(cond, node, "if statement has no condition");
        HirNodeId then = bodies.empty() ? reportedError(node, "if statement has no then-branch")
                                        : bodies[0];
        std::optional<HirNodeId> els;
        if (bodies.size() >= 2) els = bodies[1];
        return track(builder.makeIfStmt(condId, then, els), node);
    }

    // ── provably-infinite-loop detection (D-HIR-INFINITE-LOOP-NOT-TERMINATING) ──
    //
    // The verifier's `pathTerminates` is deliberately conservative — a loop body
    // is never counted as terminating. The documented design closes the gap at
    // the lowering tier: a provably-infinite loop is wrapped as
    // `Block{ loop, Unreachable }`, so `pathTerminates` (which recurses to a
    // Block's last child) sees the synthetic `Unreachable` and reports the
    // construct as terminating. This removes the over-rejection of a non-void
    // function whose terminating tail is such a loop, WITHOUT touching the
    // verifier / H0003 / the dead-code rule (a `Block` is not an
    // `isUnconditionalTerminator`, so a statement after the wrapper is still not
    // flagged dead — keeping this decoupled from the dead-code-after-terminator
    // anchor). The synthetic `Unreachable` is never reached at runtime; orphaned
    // dead blocks are dropped by the MIR unreachable-prune (a `while(1)` exit the
    // const-true `CondBr` keeps structurally reachable stays a never-executed
    // `Unreachable` until SimplifyCfg folds the branch), so runtime is unaffected.

    // (1) Does this loop's CONDITION guarantee the test never fails — i.e. is the
    // loop, on the condition alone, non-exiting? `nullopt` cond (`for(;;)`) is
    // infinite by construction. A present cond is infinite iff it const-folds to a
    // NONZERO integer (`while(1)`, `while(1==1)`, `while(K)` for a const `K!=0`).
    // Uses the shared CST const-eval (single source of truth with the index-
    // designator path). A cond that does not fold, or folds to zero, is NOT
    // provably-infinite — we fall back to today's behavior (no wrap), so a
    // not-provably-infinite loop can never be wrongly marked infinite.
    [[nodiscard]] bool conditionIsProvablyTruthy(std::optional<NodeId> condNode) {
        if (!condNode.has_value()) return true;          // for(;;) — no test
        auto v = evalCstConstInt(*condNode);
        return v.has_value() && *v != 0;
    }

    // (2) Can a `break` exit THIS loop's frame? Scans the loop's lowered HIR body,
    // RESPECTING nesting: a `BreakStmt` reached in this loop's own frame (through
    // if / block, but NOT through a nested loop or switch) targets THIS loop and
    // so exits it. A `break` inside a nested loop/switch targets that inner
    // construct — the scan does not descend into nested loop/switch bodies, so
    // such a break does not count. `continue` (re-loops) and `return` (exits the
    // function, not to after-the-loop) never make the loop fall through, so they
    // are ignored. The de Bruijn break depth is not yet assigned at lowering
    // (`makeBreak(0)`), so this STRUCTURAL frame-respecting scan — mirroring how
    // an innermost-enclosing break target resolves — is the precise mechanism.
    // Conservatism is one-directional: if in doubt we report a break exists
    // (loop NOT infinite), never the reverse — a breakable loop is never wrongly
    // wrapped.
    [[nodiscard]] bool bodyHasReachableBreak(HirNodeId body) const {
        switch (builder.kind(body)) {
            case HirKind::BreakStmt:
                return true;                              // targets this loop's frame
            // A nested loop / switch captures any `break` inside it — do not
            // descend; a break there does not exit THIS loop.
            case HirKind::WhileStmt:
            case HirKind::DoWhileStmt:
            case HirKind::ForStmt:
            case HirKind::SwitchStmt:
                return false;
            // FC5 — a `goto` inside a provably-infinite loop is NOT counted here
            // (it falls to `default`; a GotoStmt is a leaf, so it returns false).
            // This is deliberate and HARMLESS in both directions: an INTERNAL goto
            // (target inside the loop) keeps the loop genuinely infinite, so the
            // Block{loop,Unreachable} wrap is correct; a FRAME-ESCAPING goto
            // (`while(1){ if(c) goto out; } out: …`) still gets the wrap, but the
            // wrap's synthetic Unreachable lands on a no-predecessor block that the
            // mandatory MIR unreachable-prune drops, while the goto's own Br keeps
            // the target label live — runtime-correct (witnessed by the
            // goto_infinite_escape corpus). Counting gotos as breaks here would be
            // BOTH unsound (a goto has no frame, so the break's frame-respecting
            // non-descent rule doesn't apply) and would FALSE-REJECT a valid
            // infinite loop whose goto stays internal — so we intentionally do not.
            default:
                for (HirNodeId c : builder.children(body))
                    if (bodyHasReachableBreak(c)) return true;
                return false;
        }
    }

    // A loop is PROVABLY-INFINITE iff control can never fall through past it:
    // its condition is constant-truthy/absent AND no `break` in its own frame
    // exits it. `loopStmt` is the freshly-built (unparented) loop node;
    // `loopBody` is its body subtree (already attached to `loopStmt`).
    [[nodiscard]] bool loopIsProvablyInfinite(std::optional<NodeId> condNode,
                                              HirNodeId loopBody) {
        return conditionIsProvablyTruthy(condNode)
            && !bodyHasReachableBreak(loopBody);
    }

    // Wrap a provably-infinite `loopStmt` as `Block{ loopStmt, Unreachable }`
    // (both the Block and the leaf flagged `Synthetic`) so the construct
    // structurally terminates; otherwise return `loopStmt` unchanged. `node` is
    // the loop's CST node, used only for span tracking (the synthetic
    // `Unreachable` has no source of its own).
    [[nodiscard]] HirNodeId wrapIfProvablyInfinite(NodeId node, HirNodeId loopStmt,
                                                   std::optional<NodeId> condNode,
                                                   HirNodeId loopBody) {
        if (!loopIsProvablyInfinite(condNode, loopBody)) return loopStmt;
        HirNodeId const unreachable =
            builder.addLeaf(HirKind::Unreachable, InvalidType, /*payload=*/0,
                            HirFlags::Synthetic);
        HirNodeId const wrapped[] = {loopStmt, unreachable};
        return track(builder.makeBlock(wrapped, HirFlags::Synthetic), node);
    }

    HirNodeId lowerWhile(NodeId node, bool doWhile) {
        std::optional<HirNodeId> cond, body;
        std::optional<NodeId>    condNode;
        for (NodeId c : visible(node)) {
            if (isToken(c)) continue;
            Role const role = classify(c);
            if (role == Role::Expr && !cond) {
                condNode = c;
                E const condE = lowerExpr(c);
                cond = coerceCondition(condE, c).id;
            }
            else if (role == Role::Stmt && !body) body = lowerStmt(c);
        }
        HirNodeId condId = orError(cond, node, "loop has no condition");
        HirNodeId bodyId = orError(body, node, "loop has no body");
        HirNodeId const loop =
            doWhile ? track(builder.makeDoWhileStmt(bodyId, condId), node)
                    : track(builder.makeWhileStmt(condId, bodyId), node);
        return wrapIfProvablyInfinite(node, loop, condNode, bodyId);
    }

    HirNodeId lowerFor(NodeId node) {
        // for ( init? ; cond? ; update? ) body  — segment the header by the
        // `;` separator; the body is the last meaningful child (after `)`).
        std::vector<std::pair<int, NodeId>> clauses;
        int seg = 0;
        for (NodeId c : visible(node)) {
            if (isToken(c)) {
                if (cfg.forClauseSeparator.valid()
                    && tree().tokenKind(c).v == cfg.forClauseSeparator.v) ++seg;
                continue;
            }
            clauses.push_back({seg, c});
        }
        if (clauses.empty()) { unsupported(node, "for has no body"); return errorNode(node); }
        NodeId bodyN = clauses.back().second;
        clauses.pop_back();
        std::optional<HirNodeId> init, cond, update;
        std::optional<NodeId>    condNode;   // nullopt for a clause-less `for(;;)`
        for (auto const& [s, c] : clauses) {
            if (s == 0)      init   = lowerForClause(c);
            else if (s == 1) {
                condNode = c;
                E const condE = lowerExpr(c);
                cond = coerceCondition(condE, c).id;
            }
            else if (s == 2) update = lowerForClause(c);
        }
        HirNodeId body = lowerStmt(bodyN);
        HirNodeId const loop = track(builder.makeForStmt(init, cond, update, body), node);
        return wrapIfProvablyInfinite(node, loop, condNode, body);
    }

    // A for init/update clause: a varDeclHead → VarDecl; an assignment → AssignStmt;
    // otherwise the bare expression.
    HirNodeId lowerForClause(NodeId c) {
        NodeId core = peelToCore(c);
        HirRuleMapping const* m = mappingFor(core);
        if (m != nullptr && m->hirKind == "VarDecl") return lowerVarDecl(core);
        return lowerStmtExprCore(core, /*wrapBare=*/false);
    }

    HirNodeId lowerSwitch(NodeId node) {
        std::optional<HirNodeId> disc;
        std::vector<NodeId> items;
        for (NodeId c : visible(node)) {
            if (isToken(c)) continue;
            if (!disc && classify(c) == Role::Expr) { disc = lowerExpr(c).id; continue; }
            items.push_back(c);   // switchBodyItem wrappers (caseLabel | statement)
        }
        HirNodeId discId = orError(disc, node, "switch has no discriminant");

        std::vector<HirNodeId> arms;
        std::optional<NodeId>  curValue;       // the case match expression, if any
        bool                   curIsDefault = false;
        bool                   haveArm = false;
        std::vector<HirNodeId> curBody;
        auto flush = [&]() {
            if (!haveArm) return;
            std::optional<HirNodeId> value;
            if (!curIsDefault && curValue) value = lowerExpr(*curValue).id;
            arms.push_back(track(builder.makeCaseArm(value, curBody), node));
            curBody.clear();
        };
        for (NodeId raw : items) {
            NodeId core = peelToCore(raw);
            bool const isLabel = cfg.caseLabelRule.valid()
                && tree().kind(core) == NodeKind::Internal
                && tree().rule(core).v == cfg.caseLabelRule.v;
            if (isLabel) {
                flush();
                haveArm = true;
                curIsDefault = false;
                curValue = std::nullopt;
                for (NodeId lc : visible(core)) {
                    if (isToken(lc)) {
                        if (cfg.caseDefaultToken.valid()
                            && tree().tokenKind(lc).v == cfg.caseDefaultToken.v)
                            curIsDefault = true;
                    } else {
                        curValue = lc;   // the case match expression
                    }
                }
            } else if (haveArm) {
                curBody.push_back(lowerStmt(core));
            }
        }
        flush();
        return track(builder.makeSwitchStmt(discId, arms), node);
    }

    // ── declarations ──────────────────────────────────────────────────────────
    // A `var`-style declaration. The SAME rule lowers to a local `VarDecl`
    // inside a block and to a `Global` at module scope (`asGlobal`); a language
    // whose top-level and local variables share one rule — toy's `varDecl` — is
    // disambiguated by lowering context, not by a second rule.
    // FC4 c1: declarator-mode rows lower ONE VarDecl/Global PER NAMED
    // declarator (`int x = 1, *p = q;` → two nodes). Appends to `out` so
    // multi-node consumers (lowerTopLevelInto's module globals) stay flat;
    // single-node statement consumers wrap via `lowerVarLike` below.
    void lowerVarLikeInto(NodeId node, bool asGlobal,
                          std::vector<HirNodeId>& out) {
        auto it = declMap_.find(tree().rule(node).v);
        DeclarationRule const* decl =
            (it != declMap_.end()) ? &sem.declarations[it->second] : nullptr;
        if (decl == nullptr || !decl->isDeclaratorMode()
            || !sem.declarators.has_value()) {
            out.push_back(lowerVarLikeLegacy(node, asGlobal, decl));
            return;
        }
        DeclaratorConfig const& dc = *sem.declarators;
        auto vis = declRoleChildren(tree(), node, *decl);
        auto const carrier = decl->declaratorListChild.has_value()
                                 ? decl->declaratorListChild
                                 : decl->declaratorChild;
        // SINGLE-declarator (param-like) rows: an ABSTRACT param (no name,
        // or no declarator at all — `int f(int)` / `int f(int *)`) STILL
        // occupies an argument slot. Emit a nameless VarDecl typed by the
        // semantic stamp on the row node — skipping it would silently
        // shift every later parameter's argument register (a miscompile).
        bool const singleMode = decl->declaratorChild.has_value();
        auto const emitAbstractSlot = [&]() {
            TypeId const slotTy = typeAtOr(node, InvalidType);
            // C 6.7.6.3p10 (`parameters.soleVoidMeansEmpty`): the sole
            // `(void)` parameter declares NO parameter — the semantic
            // FnSig already dropped it; emitting a VarDecl slot here
            // would desynchronize the Function node's param count from
            // its FnSig (the verifier's param-count check). Any OTHER
            // abstract void param was already rejected loud
            // (S_InvalidVoidParam), so skipping void-typed abstract
            // slots is exact, not lossy.
            if (sem.parameters.soleVoidMeansEmpty && slotTy.valid()
                && interner.kind(slotTy) == TypeKind::Void) {
                return;
            }
            out.push_back(track(
                builder.makeVarDecl(slotTy, 0, std::nullopt), node));
        };
        if (!carrier.has_value() || *carrier >= vis.size()) {
            if (singleMode) emitAbstractSlot();
            return;
        }
        std::vector<NodeId> declarators;
        collectDeclarators(tree(), vis[*carrier], dc, declarators);
        if (singleMode && declarators.empty()) {
            emitAbstractSlot();
            return;
        }
        for (NodeId d : declarators) {
            NodeId const nameNode = declaratorNameNode(tree(), d, dc);
            if (!nameNode.valid()) {
                // Abstract: param-like rows keep the slot (above);
                // list rows mint nothing — the semantic tier already
                // rejected the form where names are required.
                if (singleMode) emitAbstractSlot();
                continue;
            }
            SymbolId const sym = model.symbolAt(nameNode);
            TypeId type = InvalidType;
            if (auto const* rec = model.recordFor(sym)) type = rec->type;
            // The init = the init-declarator's visible Internal child that
            // is NOT the declarator (`[declarator, '=', initValue]`).
            std::optional<HirNodeId> init;
            if (tree().rule(d).v == dc.initDeclaratorRule.v) {
                for (NodeId c : visible(d)) {
                    if (isToken(c)) continue;
                    if (tree().rule(c).v == dc.declaratorRule.v) continue;
                    init = lowerExprOrBraceInit(c, type);
                    break;
                }
            }
            out.push_back(asGlobal
                ? track(builder.makeGlobal(type, sym.v, init), d)
                : track(builder.makeVarDecl(type, sym.v, init), d));
        }
    }

    // Single-node statement-context wrapper: one declarator lowers to its
    // bare VarDecl (the pre-FC4 shape, unchanged for every single-
    // declarator program); a multi-declarator statement wraps its VarDecls
    // in a Block (statement positions hold exactly one node). Zero named
    // declarators yield an empty Block — never silent: the semantic tier's
    // requireNamedDeclarators already erred for the named positions.
    HirNodeId lowerVarLike(NodeId node, bool asGlobal) {
        std::vector<HirNodeId> out;
        lowerVarLikeInto(node, asGlobal, out);
        if (out.size() == 1) return out[0];
        return track(builder.makeBlock(out), node);
    }

    HirNodeId lowerVarLikeLegacy(NodeId node, bool asGlobal,
                                 DeclarationRule const* decl) {
        if (subtreeHasDeferred(node))
            return reportedError(node, "array declarator is deferred to HR9 "
                                       "(the lattice has no Array type yet)");
        // STRIP-AWARE on purpose (D-DECL-PREFIX-STRIP-SHARED-HELPER closure
        // fix): this used raw `visible(node)` — a latent wrong-child shift the
        // day a VarDecl-dispatch rule gains a `specifierPrefix` (no shipped one
        // does today, so this is behavior-preserving now and load-bearing when
        // local declarations gain specifiers). No DeclarationRule ⇒ no prefix
        // possible ⇒ raw visible children.
        auto vis = decl ? declRoleChildren(tree(), node, *decl) : visible(node);
        SymbolId sym{};
        TypeId type = InvalidType;
        if (decl && decl->nameChild && *decl->nameChild < vis.size()) {
            NodeId nameNode = vis[*decl->nameChild];
            // The symbol may sit on a name token nested under a wrapper (tsql's
            // columnDecl name is a `nameAtom`, not a bare Identifier); probe for it.
            sym = model.symbolAt(nameNode);
            if (!sym.valid()) sym = firstNameToken(nameNode).sym;
            if (auto const* rec = model.recordFor(sym)) type = rec->type;
        }
        std::optional<HirNodeId> init;
        for (NodeId c : vis) {
            // Skip an array-declarator suffix: its `[N]` length expression is
            // part of the TYPE (already folded into an Array<elem,N> by the
            // semantic phase), not the variable's initializer.
            if (decl && decl->arraySuffix && tree().kind(c) == NodeKind::Internal
                && tree().rule(c).v == decl->arraySuffix->rule.v)
                continue;
            if (classify(c) == Role::Expr
             || (cfg.braceInitListRule.valid()
                 && isBraceInitList(peelToBraceInitOrCore(c)))) {
                // D5.3: the shared `lowerExprOrBraceInit` helper covers
                // both ordinary expression and aggregate brace-init
                // (`int p[3] = {1,2,3}` / `struct Point p = {.x=1}`).
                // Coerces the initializer to the declared variable type.
                init = lowerExprOrBraceInit(c, type);
                break;
            }
        }
        return asGlobal ? track(builder.makeGlobal(type, sym.v, init), node)
                        : track(builder.makeVarDecl(type, sym.v, init), node);
    }

    HirNodeId lowerVarDecl(NodeId node) { return lowerVarLike(node, /*asGlobal=*/false); }

    HirNodeId lowerTypeDecl(NodeId node) {
        auto it = declMap_.find(tree().rule(node).v);
        DeclarationRule const* decl = (it != declMap_.end()) ? &sem.declarations[it->second] : nullptr;
        // Strip-aware for the same reason as `lowerVarLike` above: no shipped
        // type-decl rule declares a `specifierPrefix` today (behavior-
        // preserving), but a prefixed one must not shift `nameChild`.
        auto vis = decl ? declRoleChildren(tree(), node, *decl) : visible(node);
        SymbolId sym{};
        TypeId type = InvalidType;
        // FC4 c1: declarator-mode type rows (c-subset typedefDecl) carry the
        // declared name inside the declarator — the shared walk finds it.
        if (decl && decl->isDeclaratorMode() && sem.declarators.has_value()) {
            auto const carrier = decl->declaratorListChild.has_value()
                                     ? decl->declaratorListChild
                                     : decl->declaratorChild;
            if (carrier.has_value() && *carrier < vis.size()) {
                std::vector<NodeId> declarators;
                collectDeclarators(tree(), vis[*carrier], *sem.declarators,
                                   declarators);
                for (NodeId d : declarators) {
                    NodeId const nameNode =
                        declaratorNameNode(tree(), d, *sem.declarators);
                    if (!nameNode.valid()) continue;
                    sym = model.symbolAt(nameNode);
                    if (auto const* rec = model.recordFor(sym)) type = rec->type;
                    break;   // typedefs declare a single declarator
                }
            }
        } else if (decl && decl->nameChild && *decl->nameChild < vis.size()) {
            sym = model.symbolAt(vis[*decl->nameChild]);
            if (auto const* rec = model.recordFor(sym)) type = rec->type;
        }
        return track(builder.makeTypeDecl(type, sym.v), node);
    }

    // FC4 c1: declarator-mode topLevelDecl — Function (the kindByChild
    // discriminator matched the block tail) or one Global PER named
    // declarator. Appends to `out` (module decls are a flat list — a
    // Block wrapper would hide globals from the HIR→MIR module walk).
    void lowerTopLevelInto(NodeId node, std::vector<HirNodeId>& out) {
        auto it = declMap_.find(tree().rule(node).v);
        if (it == declMap_.end()) {
            unsupported(node, "top-level decl has no semantics rule");
            out.push_back(errorNode(node));
            return;
        }
        DeclarationRule const& decl = sem.declarations[it->second];
        if (!decl.isDeclaratorMode() || !sem.declarators.has_value()) {
            out.push_back(lowerTopLevel(node));   // legacy positional path
            return;
        }
        DeclaratorConfig const& dc = *sem.declarators;
        auto vis = declRoleChildren(tree(), node, decl);
        LinkageAttr const linkAttr =
            linkageFrom(specifierPrefixChild(tree(), node, decl), decl);
        // Function iff the kindByChild discriminator matches (the block
        // tail). bodyPath empty ⇒ the matched node IS the body (the
        // declarator-mode convention — params live in the declarator).
        NodeId discNode{};
        bool isFn = false;
        if (decl.kindByChild) {
            discNode = descendVisibleDecl(tree(), node,
                                          decl.kindByChild->childPath, decl);
            isFn = discNode.valid()
                && tree().kind(discNode) == NodeKind::Internal
                && tree().rule(discNode).v == decl.kindByChild->whenRule.v;
        }
        auto const carrier = decl.declaratorListChild.has_value()
                                 ? decl.declaratorListChild
                                 : decl.declaratorChild;
        if (!carrier.has_value() || *carrier >= vis.size()) return;
        std::vector<NodeId> declarators;
        collectDeclarators(tree(), vis[*carrier], dc, declarators);

        if (isFn) {
            // The function = the sole named declarator (the semantic tier
            // enforces exactly-one / named / fn-suffix / no-init via
            // S_InvalidFunctionDeclarator + S_DeclarationDeclaresNothing;
            // lowering degrades to an Error node when those fired).
            NodeId fnName{};
            for (NodeId d : declarators) {
                fnName = declaratorNameNode(tree(), d, dc);
                if (fnName.valid()) break;
            }
            if (!fnName.valid()) {
                out.push_back(errorNode(node));
                return;
            }
            SymbolId const sym = model.symbolAt(fnName);
            TypeId sig = InvalidType;
            if (auto const* rec = model.recordFor(sym)) sig = rec->type;
            // Params live in the fn suffix attached to the NAME's direct
            // declarator (`int (*f(int a))(int b)` — f's params are `a`;
            // the outer suffix shapes the return type only).
            std::vector<HirNodeId> params;
            NodeId const direct = tree().parent(fnName);
            if (direct.valid() && tree().kind(direct) == NodeKind::Internal
                && tree().rule(direct).v == dc.directRule.v) {
                for (NodeId c : visible(direct)) {
                    if (tree().kind(c) == NodeKind::Internal
                        && tree().rule(c).v == dc.fnSuffixRule.v) {
                        collectParams(c, params);
                        break;
                    }
                }
            }
            NodeId const bodyNode =
                (decl.kindByChild && !decl.kindByChild->bodyPath.empty())
                    ? descend(discNode, decl.kindByChild->bodyPath)
                    : discNode;
            TypeId const savedReturn = currentReturnType_;
            TypeId const retType =
                sig.valid() ? interner.fnResult(sig) : InvalidType;
            currentReturnType_ = retType;
            auto savedLabels = std::move(labelOrdinals_);   // FC5: per-function label scope
            labelOrdinals_.clear();
            if (bodyNode.valid()) prescanLabels(bodyNode);
            HirNodeId body = bodyNode.valid()
                ? lowerStmt(bodyNode)
                : track(builder.makeBlock({}), node);
            labelOrdinals_ = std::move(savedLabels);
            currentReturnType_ = savedReturn;
            body = maybeAppendImplicitReturnZero(node, body, sym, retType,
                                                 decl);
            HirNodeId const fn_ =
                track(builder.makeFunction(sig, sym.v, params, body), node);
            recordLinkage(fn_, linkAttr);
            out.push_back(fn_);
            return;
        }

        // Globals — one per named declarator, each with the decl's linkage.
        std::size_t const before = out.size();
        lowerVarLikeInto(node, /*asGlobal=*/true, out);
        for (std::size_t i = before; i < out.size(); ++i) {
            recordLinkage(out[i], linkAttr);
        }
    }

    // topLevelDecl → Function (when the kindByChild discriminator resolves to
    // funcDefTail) or Global. LEGACY positional path (toy-style rows).
    HirNodeId lowerTopLevel(NodeId node) {
        auto it = declMap_.find(tree().rule(node).v);
        if (it == declMap_.end()) { unsupported(node, "top-level decl has no semantics rule"); return errorNode(node); }
        DeclarationRule const& decl = sem.declarations[it->second];
        auto vis = declRoleChildren(tree(), node, decl);
        SymbolId sym{};
        TypeId type = InvalidType;
        if (decl.nameChild && *decl.nameChild < vis.size()) {
            sym = model.symbolAt(vis[*decl.nameChild]);
            if (auto const* rec = model.recordFor(sym)) type = rec->type;
        }
        // D-CSUBSET-LINKAGE-SPECIFIERS: linkage from the (optional) specifier
        // prefix, attached below to the lowered Function/Global node and threaded
        // to MIR for DCE protection.
        LinkageAttr const linkAttr =
            linkageFrom(specifierPrefixChild(tree(), node, decl), decl);
        // Function iff the kindByChild discriminator matches funcDefTail.
        NodeId discNode{};
        if (decl.kindByChild) {
            discNode = descendVisibleDecl(tree(), node,
                                          decl.kindByChild->childPath, decl);
            if (discNode.valid() && tree().kind(discNode) == NodeKind::Internal
                && tree().rule(discNode).v == decl.kindByChild->whenRule.v) {
                return lowerFunction(node, sym, type, decl, *decl.kindByChild, discNode, linkAttr);
            }
        }
        // Global.
        if (subtreeHasDeferred(node))
            return reportedError(node, "array declarator is deferred to HR9 "
                                       "(the lattice has no Array type yet)");
        std::optional<HirNodeId> init;
        RuleId const skip = arraySuffixSkipRule(decl);
        for (NodeId c : descendantsForInit(node, skip, &decl)) if (isExprNode(c)) {
            // Coerce the initializer to the declared variable type — the same
            // discipline `lowerVarLike` applies for local VarDecls. Without
            // this, a module global declared `int g = 1.7 + 2.5;` lands with
            // an F64-typed init under an I32 global (mismatch), and downstream
            // const-eval (plan 12.5) folds the float arithmetic but skips the
            // narrowing the runtime would perform. Language-blind: `coerce`
            // checks arithmetic kinds via the lattice and is a no-op when
            // already at target type.
            E const initE   = lowerExpr(c);
            E const coerced = coerce(initE, type);
            init = coerced.id;
            break;
        }
        HirNodeId const g = track(builder.makeGlobal(type, sym.v, init), node);
        recordLinkage(g, linkAttr);
        return g;
    }

    // A function declared by a DEDICATED rule (e.g. toy's `funcDef`), as opposed
    // to c-subset's dual-purpose `topLevelDecl`+kindByChild. Reads the params/body
    // subtrees from the semantic DeclarationRule's `paramsChild`/`bodyChild`
    // visible-child indices.
    // D-LK10-ENTRY-MAIN-IMPLICIT-RETURN (source-agnostic): if the
    // language's semantic config declares this function's name in
    // `implicitReturnZeroForFunctionNames` AND the return type is
    // non-void AND `body` is a Block AND the body doesn't
    // structurally terminate, return a new Block that wraps the
    // original children + a synthetic `return <zero>`. Otherwise
    // return `body` unchanged. Shared between `lowerFunctionDecl`
    // (dedicated-rule front-ends like toy) and `lowerFunction`
    // (kindByChild dispatch like c-subset's `topLevelDecl`) — both
    // call this AFTER lowering the body and BEFORE handing it to
    // `builder.makeFunction`.
    [[nodiscard]] HirNodeId
    maybeAppendImplicitReturnZero(NodeId node,
                                  HirNodeId body,
                                  SymbolId sym,
                                  TypeId retType,
                                  DeclarationRule const& decl) {
        // Fast-path: most declaration forms declare no implicit-
        // return-0 names — short-circuits before any type / model
        // lookup.
        if (decl.implicitReturnZeroForFunctionNames.empty()) return body;
        // `retType.valid()` subsumes `sig.valid()` (caller computes
        // retType as `sig.valid() ? interner.fnResult(sig) : InvalidType`).
        if (!retType.valid()) return body;
        // Integer-only — silent-failure F3 fold (audit on 3-stream
        // parallel work). Excludes void, float, struct, ptr, etc.
        if (!isIntegerReturnCore(interner.kind(retType))) return body;
        if (builder.kind(body) != HirKind::Block) return body;
        // `recordFor(sym) == nullptr` means upstream semantic
        // analysis failed to mint a record for this declaration —
        // a substrate-shape violation (every reached function decl
        // must have a SymbolRecord). The earlier `retType.valid()`
        // gate already implies `sig.valid()` which is set from
        // `rec->type` at the call site, so this branch is in
        // practice unreachable through the shipped grammars.
        // Defensive `nullptr` skip preserved as belt-and-suspenders
        // — silent-failure F2 fold.
        auto const* rec = model.recordFor(sym);
        if (rec == nullptr) return body;
        if (std::ranges::find(
                decl.implicitReturnZeroForFunctionNames, rec->name)
            == decl.implicitReturnZeroForFunctionNames.end())
            return body;
        if (pathTerminates(builder, body)) return body;

        // Build the synthetic return — a zero literal of retType
        // wrapped in a ReturnStmt, both flagged Synthetic. Then NEST
        // the (still-unparented) body Block and the new return inside
        // a fresh outer Block (also Synthetic): `{ <body> return 0; }`.
        //
        // We must NOT re-wrap the body's existing children: HIR is
        // immutable and tree-shaped — every node has at most one parent
        // and there is no detach/re-parent. The body's children are
        // already attached to `body`, so reusing them under a new Block
        // would double-attach and trip `addParent`'s fail-loud guard.
        // `body` ITSELF is unparented here (the lowered function body is
        // not attached until `makeFunction` consumes the value we
        // return), so attaching it once as the outer Block's first child
        // is safe. A Block whose first child is a Block is valid HIR
        // (a nested scope, `{ {…} return 0; }`); the verifier's
        // return-completeness check recurses to the outer Block's LAST
        // child (the return) and the dead-code check sees the leading
        // Block — not an unconditional terminator — so the trailing
        // return is never flagged unreachable. MIR's unreachable-prune
        // later drops the dead return for a body that does terminate
        // (e.g. an infinite-loop main), so runtime stays correct.
        HirNodeId const zero = synthZeroOrError(node, retType);
        HirNodeId const ret  = track(
            builder.makeReturn(zero, HirFlags::Synthetic), node);
        HirNodeId const wrapped[] = {body, ret};
        return track(
            builder.makeBlock(wrapped, HirFlags::Synthetic), node);
    }

    HirNodeId lowerFunctionDecl(NodeId node) {
        auto it = declMap_.find(tree().rule(node).v);
        if (it == declMap_.end()) return reportedError(node, "function decl has no semantics rule");
        DeclarationRule const& decl = sem.declarations[it->second];
        auto vis = declRoleChildren(tree(), node, decl);
        SymbolId sym{};
        TypeId sig = InvalidType;
        if (decl.nameChild && *decl.nameChild < vis.size()) {
            sym = model.symbolAt(vis[*decl.nameChild]);
            if (auto const* rec = model.recordFor(sym)) sig = rec->type;
        }
        LinkageAttr const linkAttr =
            linkageFrom(specifierPrefixChild(tree(), node, decl), decl);
        std::vector<HirNodeId> params;
        if (decl.paramsChild && *decl.paramsChild < vis.size())
            collectParams(vis[*decl.paramsChild], params);
        // Set currentReturnType_ around the body so `lowerReturn` coerces
        // each `return expr;` to the declared return type. Saved + restored
        // around the call to handle nested functions if a future frontend
        // emits them (today's grammars don't).
        TypeId const savedReturn = currentReturnType_;
        TypeId const retType =
            sig.valid() ? interner.fnResult(sig) : InvalidType;
        currentReturnType_ = retType;
        NodeId const bodyNode = (decl.bodyChild && *decl.bodyChild < vis.size())
                              ? vis[*decl.bodyChild] : NodeId{};
        auto savedLabels = std::move(labelOrdinals_);   // FC5: per-function label scope
        labelOrdinals_.clear();
        if (bodyNode.valid()) prescanLabels(bodyNode);
        HirNodeId body = bodyNode.valid()
                       ? lowerStmt(bodyNode)
                       : track(builder.makeBlock({}), node);
        labelOrdinals_ = std::move(savedLabels);
        currentReturnType_ = savedReturn;
        body = maybeAppendImplicitReturnZero(
            node, body, sym, retType, decl);
        HirNodeId const fn_ = track(builder.makeFunction(sig, sym.v, params, body), node);
        recordLinkage(fn_, linkAttr);
        return fn_;
    }

    // extern function / global (no body). The FnSig/var type comes from the
    // symbol the semantic phase minted (FFI linkage metadata is plan 11).
    HirNodeId lowerExternDecl(NodeId node) {
        auto it = declMap_.find(tree().rule(node).v);
        if (it == declMap_.end()) return reportedError(node, "extern decl has no semantics rule");
        DeclarationRule const& decl = sem.declarations[it->second];
        auto vis = declRoleChildren(tree(), node, decl);
        SymbolId sym{};
        TypeId type = InvalidType;
        if (decl.nameChild && *decl.nameChild < vis.size()) {
            sym = model.symbolAt(vis[*decl.nameChild]);
            if (auto const* rec = model.recordFor(sym)) type = rec->type;
        }
        // D-CSUBSET-EXTERN-LIBRARY-SYNTAX closure (step 13.3,
        // 2026-06-02): scan the tail subtree (varDeclTail or
        // externFuncTail) for an optional trailing `stringLiteralExpr`
        // node — when present, decode its body as the per-symbol
        // import-library override. Source-language agnostic by rule
        // name (any grammar that produces a child rule named
        // `stringLiteralExpr` populates the override the same way).
        // The decoder uses `decodeStringLiteralBody` (the same path
        // string-literal-arg uses), so all C-family escapes work in
        // the override string body.
        auto extractLibraryOverride = [&]() -> std::string {
            if (!decl.kindByChild) return {};
            // Strip-aware: `childPath` is authored against the declaration's
            // prefix-free numbering (D-DECL-PREFIX-STRIP-SHARED-HELPER). A
            // no-op today — externDecl declares no specifierPrefix.
            NodeId disc = descendVisibleDecl(tree(), node,
                                             decl.kindByChild->childPath, decl);
            if (!disc.valid() || tree().kind(disc) != NodeKind::Internal) {
                return {};
            }
            // Match by rule-name to stay source-agnostic. Any language
            // whose grammar wraps the override in a rule named
            // `stringLiteralExpr` (StringStart + StringLiteral body)
            // gets per-symbol library routing for free.
            RuleId const stringLitRule =
                tree().schema().rules().find("stringLiteralExpr");
            if (!stringLitRule.valid()) return {};
            SchemaTokenId const stringLitTok =
                tree().schema().schemaTokens().find("StringLiteral");
            if (!stringLitTok.valid()) return {};
            for (auto const& c : tree().children(disc)) {
                if (tree().kind(c) != NodeKind::Internal) continue;
                if (isEmptySpace(tree().flags(c))) continue;
                if (tree().rule(c).v != stringLitRule.v) continue;
                // The body is the inner StringLiteral token.
                for (auto const& gc : tree().children(c)) {
                    if (tree().kind(gc) != NodeKind::Token) continue;
                    if (tree().tokenKind(gc).v != stringLitTok.v) continue;
                    auto decoded =
                        decodeStringLiteralBody(tree().text(gc));
                    if (decoded.has_value()) return std::move(*decoded);
                    // F4 audit fix (6-agent 2nd-order, step 13.3a):
                    // fail-loud on malformed escape rather than
                    // silently falling back to the format-level
                    // default — pre-fix a user-typed `extern void f()
                    // "k\xZZ.dll";` would silently link against
                    // msvcrt.dll with no breadcrumb pointing at the
                    // malformed override. H_ExternDeclMalformed is
                    // already used for malformed extern declarations.
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::H_ExternDeclMalformed;
                    d.severity = DiagnosticSeverity::Error;
                    d.buffer   = tree().source().id();
                    d.span     = tree().span(gc);
                    d.actual   = std::string{tree().text(gc)};
                    reporter.report(std::move(d));
                    return {};
                }
            }
            return {};
        };

        // FF6 Slice 2 (2026-06-02 + post-fold #1 simplifier): record
        // the extern for FFI synthesis. The canonical name comes
        // from the SymbolRecord (unmangled identifier as declared
        // in source). Empty name when the semantic phase couldn't
        // resolve the symbol — synthesize() then fails loud with
        // F_FfiIngestEmptyCanonical PROVIDED the language has a
        // per-format library entry; if the language config has no
        // entry the upstream F_FfiNoImportLibraryForFormat fires
        // FIRST and short-circuits before the per-extern guard.
        // Both surfaces are unsuppressable + upstream-of-link.
        auto recordExtern = [&](HirNodeId h) {
            // D-CSUBSET-LINKAGE-UNKNOWN-SPECIFIER-DIAGNOSTIC (cycle 14, design-audit
            // Gate 1): route the extern arm through the SAME linkageFrom chokepoint
            // as lowerTopLevel/lowerFunctionDecl, so specifier validation is
            // by-construction for EVERY decl-lowering arm, not a hand-picked subset.
            // A no-op today (externDecl declares no specifierPrefix →
            // specifierPrefixChild returns invalid → linkageFrom early-returns);
            // structural for the day an extern gains specifiers.
            recordLinkage(h, linkageFrom(specifierPrefixChild(tree(), node, decl),
                                         decl));
            auto const* rec = model.recordFor(sym);
            // Model 3: the source `"libname"` override is format-independent →
            // project it under every object-format key so the compile-pipeline
            // fold yields it for whatever the active target's format is.
            externDecls.push_back({h,
                                   rec ? rec->name : std::string{},
                                   uniformLibraryMap(extractLibraryOverride())});
        };
        if (decl.kindByChild) {
            // Strip-aware: `childPath` is authored against the declaration's
            // prefix-free numbering (D-DECL-PREFIX-STRIP-SHARED-HELPER). A
            // no-op today — externDecl declares no specifierPrefix.
            NodeId disc = descendVisibleDecl(tree(), node,
                                             decl.kindByChild->childPath, decl);
            if (disc.valid() && tree().kind(disc) == NodeKind::Internal
                && tree().rule(disc).v == decl.kindByChild->whenRule.v) {
                std::vector<HirNodeId> params;
                NodeId paramsNode = descend(disc, decl.kindByChild->paramsPath);
                if (paramsNode.valid()) collectParams(paramsNode, params);
                HirNodeId const n =
                    track(builder.makeExternFunction(type, sym.v, params), node);
                recordExtern(n);
                return n;
            }
        }
        // D-FF2-3: reject `extern int x = 5;` (and `= y`, `= {}`, etc.).
        // An extern announces a symbol whose storage lives in another
        // translation unit; an initializer would either redefine the
        // symbol locally (contradicting `extern`) or be silently dropped
        // at lowering.
        //
        // SHAPE-based detection (post-fold #7 silent-failure F4): the
        // varDeclTail subtree's visible children are at most {
        // arrayDeclSuffix, AssignOp + initValue, EndStatement }. Any
        // internal child that isn't arrayDeclSuffix IS the initValue
        // subtree — even when its contents are empty (`= {}`) or
        // contain no expression nodes. Pre-fix the check walked for
        // `isExprNode` descendants which missed empty-brace inits.
        //
        // Post-fold #8 silent-failure H1 + post-fold #9 H2 split:
        // distinguish "engine-config error" (kindByChild absent → the
        // language hasn't told the engine HOW to navigate the extern's
        // tail) from "parse-recovery shape" (kindByChild IS configured
        // but `descend` returned invalid/non-Internal for THIS
        // particular CST). Different audiences, different remediations,
        // different codes:
        //   - H_UnsupportedLoweringForKind: grammar-author config bug
        //   - H_ExternDeclMalformed: incomplete/malformed user source
        // Pre-split both arms collapsed into UnsupportedLoweringForKind
        // and blamed the grammar config for what could be a recovery
        // shape. No shipped grammar trips either arm today (c-subset
        // configures kindByChild + the `if (model.hasErrors()) return`
        // short-circuit in FF2/lowering paths catches malformed
        // input upstream), but defensive split prevents either future
        // surface from re-opening the D-FF2-3 silent-drop.
        if (!decl.kindByChild) {
            emitH(DiagnosticCode::H_UnsupportedLoweringForKind, node,
                  "externDecl rule has no kindByChild configuration — "
                  "the engine cannot locate the varDeclTail-equivalent "
                  "subtree to check for an initializer. Configure "
                  "`kindByChild` in the language's semantics so this "
                  "extern shape can be lowered safely");
            return errorNode(node);
        }
        // Strip-aware for the same reason as the two `disc` probes above.
        NodeId const varDeclTail =
            descendVisibleDecl(tree(), node, decl.kindByChild->childPath, decl);
        if (!varDeclTail.valid()
            || tree().kind(varDeclTail) != NodeKind::Internal) {
            // Post-fold #12 D-FF2-MSG-JARGON: user-facing message
            // names the user-source problem ("incomplete declaration")
            // not the engine internals ("kindByChild->childPath",
            // "Internal node", "CST"). The diagnostic infrastructure
            // already carries the source span for the user to inspect.
            emitH(DiagnosticCode::H_ExternDeclMalformed, node,
                  "extern declaration is incomplete or malformed at "
                  "this position — the declaration's body structure "
                  "could not be located; check that the declaration "
                  "is complete (e.g. `extern int x;` without an "
                  "initializer, or `extern int f(int);` for a "
                  "function declaration)");
            return errorNode(node);
        }
        RuleId const skipRule = arraySuffixSkipRule(decl);
        for (NodeId c : visible(varDeclTail)) {
            if (tree().kind(c) != NodeKind::Internal) continue;
            if (skipRule.valid()
                && tree().rule(c).v == skipRule.v) continue;
            emitH(DiagnosticCode::H_ExternHasInitializer, c,
                  "extern declarations cannot carry an "
                  "initializer — storage lives in another "
                  "translation unit; remove the initializer");
            return errorNode(node);
        }
        HirNodeId const g = track(builder.makeExternGlobal(type, sym.v), node);
        recordExtern(g);
        return g;
    }

    HirNodeId lowerFunction(NodeId node, SymbolId sym, TypeId sig,
                            DeclarationRule const& decl,
                            KindDiscriminator const& disc, NodeId discNode,
                            LinkageAttr linkAttr) {
        std::vector<HirNodeId> params;
        NodeId paramsNode = descend(discNode, disc.paramsPath);
        if (paramsNode.valid()) collectParams(paramsNode, params);
        NodeId bodyNode = descend(discNode, disc.bodyPath);
        TypeId const savedReturn = currentReturnType_;
        TypeId const retType =
            sig.valid() ? interner.fnResult(sig) : InvalidType;
        currentReturnType_ = retType;
        auto savedLabels = std::move(labelOrdinals_);   // FC5: per-function label scope
        labelOrdinals_.clear();
        if (bodyNode.valid()) prescanLabels(bodyNode);
        HirNodeId body = bodyNode.valid() ? lowerStmt(bodyNode) : track(builder.makeBlock({}), node);
        labelOrdinals_ = std::move(savedLabels);
        currentReturnType_ = savedReturn;
        body = maybeAppendImplicitReturnZero(
            node, body, sym, retType, decl);
        HirNodeId const fn_ = track(builder.makeFunction(sig, sym.v, params, body), node);
        recordLinkage(fn_, linkAttr);
        return fn_;
    }

    // Gather param VarDecls under a funcParams/fnSuffix subtree (nodes
    // mapped to VarDecl). FC4 c1: route through `lowerVarLikeInto` — a
    // declarator-mode param appends exactly its slots (0 for the dropped
    // sole-`(void)` param, 1 otherwise); the single-node `lowerVarLike`
    // wrapper would wrap a zero-slot param in an empty Block, which the
    // HIR verifier rightly rejects in parameter position.
    void collectParams(NodeId n, std::vector<HirNodeId>& out) {
        HirRuleMapping const* m = mappingFor(n);
        if (m != nullptr && m->hirKind == "VarDecl") {
            lowerVarLikeInto(n, /*asGlobal=*/false, out);
            return;
        }
        for (NodeId c : visible(n)) if (!isToken(c)) collectParams(c, out);
    }

    // Direct children to scan for a global's initializer expression. A subtree
    // rooted at `skipRule` (the array-declarator suffix) is pruned so a global
    // array's `[N]` length is never mistaken for the initializer. When `decl`
    // is supplied the walk seeds from the declaration's ROLE children
    // (specifier prefix stripped — D-DECL-PREFIX-STRIP-SHARED-HELPER), so an
    // expr-shaped specifier argument (e.g. a future
    // `__attribute__((aligned(8)))`) can never be mistaken for the global's
    // initializer. A no-op for every shipped prefix today (c-subset's
    // specifiers contain no expr-rule nodes).
    [[nodiscard]] std::vector<NodeId>
    descendantsForInit(NodeId node, RuleId skipRule = {},
                       DeclarationRule const* decl = nullptr) {
        std::vector<NodeId> out;
        std::vector<NodeId> stack =
            decl ? declRoleChildren(tree(), node, *decl) : visible(node);
        while (!stack.empty()) {
            NodeId c = stack.back(); stack.pop_back();
            if (skipRule.valid() && tree().kind(c) == NodeKind::Internal
                && tree().rule(c).v == skipRule.v)
                continue;
            if (isExprNode(c)) { out.push_back(c); continue; }
            if (tree().kind(c) == NodeKind::Internal)
                for (NodeId g : visible(c)) stack.push_back(g);
        }
        return out;
    }

    [[nodiscard]] NodeId descend(NodeId start, std::vector<std::uint32_t> const& path) {
        NodeId cur = start;
        for (auto idx : path) {
            if (!cur.valid()) return {};
            auto vis = visible(cur);
            if (idx >= vis.size()) return {};
            cur = vis[idx];
        }
        return cur;
    }

    // FC4 c1: appends 0..N module-level HIR nodes for one top-level CST
    // construct ("Skip" appends nothing; a declarator-mode multi-global
    // appends one Global per declarator — module decls are a FLAT list).
    void lowerDeclInto(NodeId node, std::vector<HirNodeId>& out) {
        // Peel the `topLevel` alt wrapper (and any nested wrappers) to the real
        // declaration node.
        NodeId core = peelToCore(node);
        HirRuleMapping const* m = mappingFor(core);
        if (m == nullptr) {
            unsupported(core, std::format("top-level construct '{}' is not lowered "
                                          "(no hirLowering mapping)",
                                          tree().kind(core) == NodeKind::Internal
                                              ? std::string{tree().rules().name(tree().rule(core))}
                                              : std::string{"<token>"}));
            out.push_back(errorNode(core));
            return;
        }
        // "Skip": a top-level construct that contributes NO HIR node (e.g. an
        // `#include` directive — its declarations arrive via the CU import
        // resolver's cross-refs, not as HIR nodes from the directive itself).
        // Config-driven (no hardcoded rule name).
        if (m->hirKind == "Skip")       return;
        if (m->hirKind == "Decl")       { lowerTopLevelInto(core, out); return; }
        if (m->hirKind == "Function")   { out.push_back(lowerFunctionDecl(core)); return; }
        if (m->hirKind == "TypeDecl")   { out.push_back(lowerTypeDecl(core)); return; }
        if (m->hirKind == "ExternDecl") { out.push_back(lowerExternDecl(core)); return; }
        // A `var`-style declaration at module scope is a Global (the same rule
        // is a local VarDecl inside a block — see lowerVarLike). Declarator-
        // mode rows append one Global per declarator (flat).
        if (m->hirKind == "VarDecl")    { lowerVarLikeInto(core, /*asGlobal=*/true, out); return; }
        // A bare statement-level decl appearing at top level (unusual): route it.
        out.push_back(lowerStmt(core));
    }

    // ── driver ─────────────────────────────────────────────────────────────────
    // Lower one tree's top-level declarations, appending to the shared module
    // decls (in tree order). The caller selects the Lowerer whose schema matches
    // this tree (HR11), so `lowerDecl` always reads this tree's own language config.
    void lowerTree(Tree const& t, std::vector<HirNodeId>& decls) {
        t_ = &t;
        if (!t.root().valid()) return;
        for (NodeId top : visible(t.root())) {
            if (isToken(top)) continue;
            // FC4 c1: a top-level construct appends 0..N module nodes
            // ("Skip" appends none; a multi-declarator global appends one
            // Global per declarator). Invalid ids are filtered defensively.
            std::vector<HirNodeId> lowered;
            lowerDeclInto(top, lowered);
            for (HirNodeId d : lowered) {
                if (d.valid()) decls.push_back(d);
            }
        }
    }
};

} // namespace

std::unique_ptr<CstToHirResult> lowerToHir(SemanticModel& model, DiagnosticReporter& reporter) {
    std::size_t const errBefore = reporter.errorCount();

    // The shared output every per-schema Lowerer writes into: one builder (→ one
    // module, arena, kind registry, literal pool) + one literal pool + one span
    // list. The module is labelled with the CU's composite source language.
    auto const trees = model.unit().trees();
    HirBuilder builder{model.unit().compositeSourceLanguage()};
    HirLiteralPool literals;
    std::vector<std::pair<HirNodeId, HirSourceLoc>> spans;
    // FF6 Slice 2 (2026-06-02): shared accumulator for source-
    // declared externs across every per-schema Lowerer. Moved
    // into the result struct after lowering completes.
    std::vector<HirExternRecord> externDecls;
    // D-CSUBSET-LINKAGE-SPECIFIERS: shared (decl node → LinkageAttr) accumulator,
    // moved onto result->linkageMap after finish() (see Lowerer::linkage).
    std::vector<std::pair<HirNodeId, LinkageAttr>> linkage;

    // One Lowerer per distinct schema in the CU (keyed by SchemaId), each bound
    // to its language's config + the shared output. `Tree::schema()` is the
    // authoritative per-file language.
    std::unordered_map<std::uint32_t, std::unique_ptr<Lowerer>> lowerers;
    for (Tree const& t : trees) {
        GrammarSchema const& sch = t.schema();
        if (lowerers.contains(sch.schemaId().v)) continue;
        lowerers.emplace(sch.schemaId().v, std::make_unique<Lowerer>(
            model, sch.hirLowering(), sch.semantics(), sch.numberStyle(),
            reporter, builder, literals, spans, externDecls, linkage));
    }

    // Lower every tree IN ORDER, dispatching to its schema's Lowerer, into the
    // one shared decls list (so module decls follow tree-add order).
    std::vector<HirNodeId> decls;
    for (Tree const& t : trees) {
        lowerers.at(t.schema().schemaId().v)->lowerTree(t, decls);
    }

    // FF11 shipped-library descriptor externs
    // (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC): synthesize one extern HIR node
    // per descriptor symbol the semantic phase minted (e.g. `puts` from
    // `stdio.json`, pulled in by `#include <stdio.h>`). These have NO source
    // CST — the semantic phase already minted the SymbolId + interned the
    // signature into the CU lattice (`model.lattice().interner()`, the same
    // interner `builder` lowers through), and applied the goal-2 skip (a symbol
    // a user decl claimed is absent from `shippedExterns()`). Each synthesized
    // node is appended to BOTH the module `decls` (so `collectExterns` in
    // HIR→MIR finds it) and `externDecls` with the descriptor's `library` as the
    // `libraryOverride` — so the EXISTING FF5 `synthesizeFfiFromSourceDecls`
    // (compile_pipeline step 2.5) binds it to the import library exactly like a
    // source-declared extern. A function symbol synthesizes ExternFunction (the
    // FnSig carries the param types, so the node needs NO param children — the
    // HIR→MIR extern pre-pass reads the signature, never the children); an
    // object symbol synthesizes ExternGlobal.
    for (ShippedExternSymbol const& ext : model.shippedExterns()) {
        HirNodeId const node = ext.isFunction
            ? builder.makeExternFunction(ext.signature, ext.symbol.v, {})
            : builder.makeExternGlobal(ext.signature, ext.symbol.v);
        decls.push_back(node);
        // libraryOverride = the descriptor's per-object-format `library` MAP
        // (Model 3), carried verbatim. The compile-pipeline fold (step 2.5)
        // selects the ACTIVE target's format entry; an empty map OR a map
        // missing that format ⇒ FF5 falls back to the language's
        // `externLibraryByFormat[format]` default.
        externDecls.push_back(HirExternRecord{
            node, ext.name, ext.library});
    }

    HirNodeId const root = builder.makeModule(decls);
    Hir hir = std::move(builder).finish(root);
    lowerers.clear();   // drop the Lowerers (their builder ref is now moved-from)

    auto result = std::make_unique<CstToHirResult>(std::move(hir), std::move(literals));
    for (auto& [id, loc] : spans) result->sourceMap.set(id, loc);
    for (auto& [id, attr] : linkage) result->linkageMap.set(id, attr);
    result->externDecls = std::move(externDecls);

    // verify-on-load.
    HirVerifier verifier{result->hir, &result->sourceMap, &model.lattice().interner()};
    (void)verifier.verify(reporter);

    result->ok = reporter.errorCount() == errBefore;
    return result;
}

} // namespace dss
