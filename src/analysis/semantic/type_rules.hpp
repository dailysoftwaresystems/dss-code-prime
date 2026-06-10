#pragma once

#include "core/export.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <cstdint>

// Header-only type-relation rules over `TypeInterner const&`. SE1 ships
// the minimum the toy/c-subset/tsql tests need: assignability, the
// numeric-widening lattice, and a simple `unify` that returns the
// widening top of two arithmetic types or InvalidType when no
// unification exists. SE2+ extend per need.
//
// InvalidType is the "I don't know" sentinel — assignable to anything
// AND from anything, so a single cascade error doesn't trigger a
// downpour of follow-on diagnostics.

namespace dss {

namespace detail::type_rules {

// Signed-integer widening rank. I8 < I16 < I32 < I64 < I128. Non-signed
// or non-integer kinds return 0 (the "not in this lattice" sentinel).
[[nodiscard]] inline constexpr int signedIntRank(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::I8:   return 1;
        case TypeKind::I16:  return 2;
        case TypeKind::I32:  return 3;
        case TypeKind::I64:  return 4;
        case TypeKind::I128: return 5;
        default:             return 0;
    }
}

[[nodiscard]] inline constexpr int unsignedIntRank(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::U8:   return 1;
        case TypeKind::U16:  return 2;
        case TypeKind::U32:  return 3;
        case TypeKind::U64:  return 4;
        case TypeKind::U128: return 5;
        default:             return 0;
    }
}

[[nodiscard]] inline constexpr int floatRank(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::F16:  return 1;
        case TypeKind::F32:  return 2;
        case TypeKind::F64:  return 3;
        case TypeKind::F128: return 4;
        default:             return 0;
    }
}

} // namespace detail::type_rules

// Arithmetic = any integer (signed/unsigned) or float type. Bool/Char/
// Byte are deliberately NOT arithmetic — promoting them implicitly
// produces silently-wrong code in the languages we ship.
[[nodiscard]] inline bool isArithmetic(TypeInterner const& interner, TypeId t) noexcept {
    if (!t.valid()) return false;
    auto const k = interner.kind(t);
    return detail::type_rules::signedIntRank(k)   != 0
        || detail::type_rules::unsignedIntRank(k) != 0
        || detail::type_rules::floatRank(k)       != 0;
}

// Same-kind same? Structural equality on TypeIds inside one CU collapses
// to pointer equality after interning, so a single `==` is correct.
[[nodiscard]] inline bool sameType(TypeId a, TypeId b) noexcept {
    return a.valid() && b.valid() && a.v == b.v;
}

// rhs assignable into lhs?
//   InvalidType on either side → true (cascade suppression).
//   Identical → true.
//   Within the signed-integer / unsigned-integer / float widening
//   lattice → true when rank(rhs) <= rank(lhs).
//   Otherwise → false. Cross-signedness and int↔float cross-conversions
//   are deliberately NOT assignable; languages requiring them widen
//   their config (typeShapes / numeric-promotion table) rather than
//   getting silent C-style implicit conversions.
//
// D-LANG-POINTER-VOID-CONVERT (step 13.2, 2026-06-02): when the
// caller supplies a `PointerConversionRules` block from the active
// language's `SemanticConfig`, this function admits the direction-
// specific `Ptr<Void>` ↔ `Ptr<T>` conversions per the flags. The
// default-constructed rules (both false) preserve strict semantics
// for callers that don't yet thread the schema (e.g., the HIR
// verifier — post-coerce, implicit conversions are already
// materialized as explicit `Cast` HIR nodes; see anchor
// `D-HIR-VERIFIER-POINTER-CONVERT-CONTRACT` in `hir_verifier.cpp`
// at the `isAssignable` call site for the post-coerce invariant).
//
// Note: `TypeKind::Void` carries DUAL semantics across the type
// lattice: (a) "no value" as a function-return type, and (b)
// "untyped memory" as the element of a `Ptr<>`. The conflation is
// contained at the type-rules tier — every tier below MIR (LIR,
// regalloc, asm) sees `Ptr<>` uniformly as pointer-width and never
// inspects the element type. Future readers checking `kind ==
// Void`: it might be the no-value case OR the untyped-memory case
// depending on context (look at whether it's the result kind or a
// `Ptr<>` operand element).
[[nodiscard]] inline bool isAssignable(
    TypeInterner const&                                interner,
    TypeId                                             lhs,
    TypeId                                             rhs,
    SemanticConfig::PointerConversionRules const&      ptrRules = {}) noexcept {
    if (!lhs.valid() || !rhs.valid()) return true;
    if (sameType(lhs, rhs)) return true;
    auto const lk = interner.kind(lhs);
    auto const rk = interner.kind(rhs);
    using namespace detail::type_rules;
    if (signedIntRank(lk) != 0 && signedIntRank(rk) != 0) {
        return signedIntRank(rk) <= signedIntRank(lk);
    }
    if (unsignedIntRank(lk) != 0 && unsignedIntRank(rk) != 0) {
        return unsignedIntRank(rk) <= unsignedIntRank(lk);
    }
    if (floatRank(lk) != 0 && floatRank(rk) != 0) {
        return floatRank(rk) <= floatRank(lk);
    }
    // C-standard array-to-pointer decay (D-LK4-RODATA-PRODUCER-STRING
    // closure, 2026-06-02): `Array<T,N>` is implicitly assignable to
    // `Ptr<T>` via the address-of-first-element conversion. Pinned to
    // same-element-type — covariant element conversions (Array<T> →
    // Ptr<const T>) wait for const-qualified pointers, anchored as
    // D-CSUBSET-CONST-PTR-DECAY. The HIR lowering (cst_to_hir.cpp::
    // coerce) emits a synthetic decay node when this branch admits
    // the assignment; the MIR-tier sees a Ptr-typed value and
    // routes through the standard pointer pipeline.
    //
    // Agnosticism note: this rule is part of the SHARED type lattice
    // (not language-specific). A schema-driven language that does
    // NOT want array decay would have to declare the prohibition
    // explicitly; the C-family (today's only consumer) wants it.
    // Anchor: D-LANG-STRUCTURAL-DECAY-OPT-OUT.
    if (lk == TypeKind::Ptr && rk == TypeKind::Array) {
        auto const lhsElem = interner.operands(lhs);
        auto const rhsElem = interner.operands(rhs);
        if (!lhsElem.empty() && !rhsElem.empty()
            && lhsElem[0] == rhsElem[0]) {
            return true;
        }
    }
    // D-LANG-POINTER-VOID-CONVERT (step 13.2, 2026-06-02): the two
    // directions of `void*` ↔ `T*` conversion are configured
    // INDEPENDENTLY — they carry different safety characteristics:
    //
    //   * `Ptr<T> → Ptr<Void>` (T* → void*; lhs is void*, rhs is T*)
    //     — typed→untyped, information-erasing, ALWAYS safe.
    //     C / C++ / Obj-C: implicit. Rust / Swift: explicit cast.
    //   * `Ptr<Void> → Ptr<T>` (void* → T*; lhs is T*, rhs is void*)
    //     — untyped→typed, information-asserting, UNSAFE.
    //     C / Obj-C: implicit. C++ / Rust / Swift: explicit cast.
    //
    // The arms below are written with `lk`/`rk` standing for
    // lhs-kind / rhs-kind throughout, matching the rest of the
    // function. Same-element-type is the trivial sameType() case
    // already covered above; here we admit the conversion ONLY when
    // exactly one side's pointer element is Void (the directional
    // void-conversion case).
    if (lk == TypeKind::Ptr && rk == TypeKind::Ptr) {
        auto const lhsElem = interner.operands(lhs);
        auto const rhsElem = interner.operands(rhs);
        if (!lhsElem.empty() && !rhsElem.empty()) {
            bool const lhsIsVoidPtr =
                interner.kind(lhsElem[0]) == TypeKind::Void;
            bool const rhsIsVoidPtr =
                interner.kind(rhsElem[0]) == TypeKind::Void;
            // T* → void* direction: lhs is void*, rhs is T* (T != void).
            if (lhsIsVoidPtr && !rhsIsVoidPtr) {
                return ptrRules.implicitToVoidPtr;
            }
            // void* → T* direction: lhs is T* (T != void), rhs is void*.
            if (!lhsIsVoidPtr && rhsIsVoidPtr) {
                return ptrRules.implicitFromVoidPtr;
            }
            // BOTH void: caught by sameType() above (Ptr<Void> ==
            // Ptr<Void> via interning). NEITHER void: distinct typed
            // pointers; fall through to the strict-reject default
            // below (Ptr<int> → Ptr<float> is NOT implicit).
        }
    }
    return false;
}

// FC2 explicit-cast legality (`(T)expr`). DELIBERATELY wider than
// `isAssignable` — an explicit cast is the programmer overriding the
// implicit-conversion rules — but still bounded by what the MIR cast
// lattice (`mapCast` in hir_to_mir.cpp) can lower and what C allows for
// the shipped type surface:
//
//   * scalar ↔ scalar — every (signed/unsigned int, Char, Byte, Bool,
//     float) pair, INCLUDING the float→int / int→float conversions the
//     implicit rules reject (mapCast: Trunc/SExt/ZExt/Bitcast/FPTrunc/
//     FPExt/SIToFP/UIToFP/FPToSI/FPToUI).
//   * Ptr ↔ Ptr — any object-pointer pair (mapCast: Bitcast).
//   * Ptr ↔ integer — C's implementation-defined round-trip (mapCast:
//     PtrToInt / IntToPtr). Float↔Ptr stays ILLEGAL (C constraint).
//   * InvalidType on either side → allowed (cascade suppression, same
//     posture as isAssignable).
//
// Everything else is false → the analyzer emits S_InvalidCast. Notably
// REJECTED (fail loud, never miscompile): struct/union VALUES (C forbids
// casts to composite types), `void` on either side (mapCast has no void
// arm; a value-discarding `(void)x` statement-cast is future surface),
// and Array-typed operands (decay-inside-cast, e.g. `(char*)"s"`, is a
// pinned follow-up — the implicit decay path exists but the explicit-
// cast lowering does not reuse it yet).
[[nodiscard]] inline bool isExplicitCastable(TypeInterner const& interner,
                                             TypeId target,
                                             TypeId operand) noexcept {
    if (!target.valid() || !operand.valid()) return true;   // cascade suppression
    auto const tk = interner.kind(target);
    auto const ok = interner.kind(operand);
    // Mirrors mapCast's isInt: every integral scalar the MIR lattice
    // casts between (incl. Bool/Char/Byte — C casts these freely).
    auto const isCastableInt = [](TypeKind k) noexcept {
        using namespace detail::type_rules;
        return signedIntRank(k) != 0 || unsignedIntRank(k) != 0
            || k == TypeKind::Char || k == TypeKind::Byte
            || k == TypeKind::Bool;
    };
    auto const isCastableScalar = [&](TypeKind k) noexcept {
        return isCastableInt(k) || detail::type_rules::floatRank(k) != 0;
    };
    if (isCastableScalar(tk) && isCastableScalar(ok)) return true;
    if (tk == TypeKind::Ptr && ok == TypeKind::Ptr)   return true;
    if (tk == TypeKind::Ptr && isCastableInt(ok))     return true;
    if (isCastableInt(tk) && ok == TypeKind::Ptr)     return true;
    return false;
}

// Best common type for binary arithmetic. Returns the wider operand
// when both live in the same widening lattice; InvalidType otherwise.
// InvalidType passes through (cascade suppression).
[[nodiscard]] inline TypeId unify(TypeInterner const& interner,
                                  TypeId a, TypeId b) noexcept {
    if (!a.valid()) return b;
    if (!b.valid()) return a;
    if (sameType(a, b)) return a;
    auto const ak = interner.kind(a);
    auto const bk = interner.kind(b);
    using namespace detail::type_rules;
    if (signedIntRank(ak) != 0 && signedIntRank(bk) != 0) {
        return signedIntRank(ak) >= signedIntRank(bk) ? a : b;
    }
    if (unsignedIntRank(ak) != 0 && unsignedIntRank(bk) != 0) {
        return unsignedIntRank(ak) >= unsignedIntRank(bk) ? a : b;
    }
    if (floatRank(ak) != 0 && floatRank(bk) != 0) {
        return floatRank(ak) >= floatRank(bk) ? a : b;
    }
    return InvalidType;
}

// Compile-time sanity: the rank functions are pure (constexpr) and the
// widening order matches the documented chain.
static_assert(detail::type_rules::signedIntRank(TypeKind::I8) == 1);
static_assert(detail::type_rules::signedIntRank(TypeKind::I128) == 5);
static_assert(detail::type_rules::signedIntRank(TypeKind::I32)
              < detail::type_rules::signedIntRank(TypeKind::I64));
static_assert(detail::type_rules::unsignedIntRank(TypeKind::U16)
              < detail::type_rules::unsignedIntRank(TypeKind::U32));
static_assert(detail::type_rules::floatRank(TypeKind::F32)
              < detail::type_rules::floatRank(TypeKind::F64));
static_assert(detail::type_rules::signedIntRank(TypeKind::Char) == 0);
static_assert(detail::type_rules::floatRank(TypeKind::Bool) == 0);

} // namespace dss
