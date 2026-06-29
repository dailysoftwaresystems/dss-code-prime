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
//   NOTE (D-CSUBSET-INT-CROSS-SIGNEDNESS-CONVERT ✅ + D-CSUBSET-INT-SAME-SIGN-NARROW ✅):
//   c-subset's C-conformant integer implicit conversions are config opt-ins
//   following the charConvertsToArith/enumConvertsToArith pattern, NOT silent
//   relaxations baked into this strict default. Two gates together complete the
//   C 6.3.1.3 integer-conversion matrix (both needed for SQLite's int/unsigned/
//   size_t mixing): `intCrossSignednessConverts` (signed↔unsigned, any width,
//   below) and `intSameSignednessNarrows` (same-signedness NARROWING — `short s =
//   anInt`, `signed char c = anInt`, `int i = aLong` — gated in the rank arms
//   below). A non-C schema (both default false) keeps strict widening-only.
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
// `boolWidensToArith` (default false): admit a Bool rhs into an arithmetic
// lhs (`_Bool` → int). It is a SEMANTIC-tier conversion — the HIR `coerce()`
// materializes it as an explicit Cast, so post-coerce the slot holds an int.
// The default is false so the HIR VERIFIER (post-coerce, strict) still catches
// a RAW Bool reaching an int slot uncast (a coerce-bug); the four semantic-tier
// checks (call-arg / return / two init sites) pass `true`.
// `intSameSignednessNarrows` (default false): admit a same-signedness integer
// NARROWING (`short s = anInt;`, `signed char c = anInt;`, `int i = aLong;`) —
// C 6.3.1.3 / 6.5.16.1, value-preserving in range and modular out of range.
// WIDENING (rank(rhs) <= rank(lhs)) is admitted UNCONDITIONALLY by the rank arms
// below; only narrowing (rank(rhs) > rank(lhs)) is newly gated here. The HIR
// `coerce()` arithmetic-core arm materializes the width-exact Cast (MIR `Trunc`),
// so the post-coerce verifier (gate default false) stays strict. Mirrors the
// charConvertsToArith / enumConvertsToArith / intCrossSignednessConverts gates;
// completes the C integer-conversion matrix alongside intCrossSignednessConverts.
// Closes D-CSUBSET-INT-SAME-SIGN-NARROW.
[[nodiscard]] inline bool isAssignable(
    TypeInterner const&                                interner,
    TypeId                                             lhs,
    TypeId                                             rhs,
    SemanticConfig::PointerConversionRules const&      ptrRules = {},
    bool                                               boolWidensToArith = false,
    bool                                               charConvertsToArith = false,
    bool                                               enumConvertsToArith = false,
    bool                                               intCrossSignednessConverts = false,
    bool                                               intSameSignednessNarrows = false) noexcept {
    if (!lhs.valid() || !rhs.valid()) return true;
    // c27 (D-CSUBSET-VOLATILE-POINTEE): volatile is IGNORED for assignment
    // compatibility — C 6.5.16.1 compares the UNQUALIFIED versions of compatible
    // types (`volatile int` ↔ `int`, `int * volatile` ↔ `int *`, `volatile struct
    // S` ↔ `struct S` all assign freely). Strip the TOP-LEVEL VolatileQual from
    // BOTH sides so the identity (`sameType`) and every kind/pointer-element rule
    // below sees the material type. (A volatile-POINTEE difference — `volatile int
    // *` vs `int *` — is a pointer-element mismatch C also permits with a
    // diagnostic-less qualifier conversion; here the element compare reads the
    // transparent `operands`, where `Ptr<VolatileQual(int)>` and `Ptr<int>` differ
    // only by the inner skin, so a future strict-qualifier-mismatch rule would add
    // its own arm — today both decay to the same pointer pipeline.)
    lhs = interner.stripVolatile(lhs);
    rhs = interner.stripVolatile(rhs);
    if (sameType(lhs, rhs)) return true;
    auto const lk = interner.kind(lhs);
    auto const rk = interner.kind(rhs);
    using namespace detail::type_rules;
    if (signedIntRank(lk) != 0 && signedIntRank(rk) != 0) {
        if (signedIntRank(rk) <= signedIntRank(lk)) return true;  // widening: always
        return intSameSignednessNarrows;          // narrowing: C 6.3.1.3, gated
    }
    if (unsignedIntRank(lk) != 0 && unsignedIntRank(rk) != 0) {
        if (unsignedIntRank(rk) <= unsignedIntRank(lk)) return true;  // widening: always
        return intSameSignednessNarrows;          // narrowing: C 6.3.1.3, gated
    }
    if (floatRank(lk) != 0 && floatRank(rk) != 0) {
        return floatRank(rk) <= floatRank(lk);
    }
    // A Bool value WIDENS into any arithmetic slot (C99 6.3.1.2 — `_Bool`
    // promotes to `int`; a comparison/logical result `Bool` flowing into an
    // int/float lhs, e.g. `int f(){ return a < b; }`, `int x = a && b;`).
    // The complete semantic-tier expression typer (`subtreeType`) now types
    // `a < b` as Bool, so this check actually runs; admitting it makes the
    // semantic tier AGREE with the HIR `coerce()` (which materializes the
    // Bool→int Cast). Gated on `boolWidensToArith` so ONLY the pre-coerce
    // semantic checks admit it — the post-coerce verifier stays strict. This
    // is the ASSIGNMENT direction only — `Bool` stays out of `isArithmetic`
    // (above), so binary PROMOTION (`bool + bool`) is unaffected. c48
    // (D-CSUBSET-BOOL-CHAR-WIDENING): when `charConvertsToArith` also treats
    // `char` (interned as `TypeKind::Char`, outside the int RANKS) as an
    // arithmetic slot, a Bool widens into a `char` lhs too — `char c = (a==b);`,
    // the sqlite `p->nFloor = (p->D==31)` shape. Arithmetic `char = a-b` already
    // worked via the `charConvertsToArith` arm below (int→char); only the
    // Bool-RESULT (comparison/logical) flowing into a plain `char` was missed.
    if (boolWidensToArith && rk == TypeKind::Bool
        && (signedIntRank(lk) != 0 || unsignedIntRank(lk) != 0
            || floatRank(lk) != 0
            || (charConvertsToArith && lk == TypeKind::Char))) {
        return true;
    }
    // C 6.3.1.1 / 6.5.16.1: `char` is an integer type — implicitly convertible to AND
    // from the integer ranks in assignment (the usual arithmetic conversions). DSS
    // interns `char` as `TypeKind::Char` (outside the signed/unsigned int RANKS), so
    // this arm bridges Char ↔ the integer ranks in BOTH directions: int→char
    // (`char x = 'c';` / `char x = 5;`, narrowing) and char→int (`int y = c;`,
    // `int f(char c){ return c; }`, widening — the codegen materializes a `Char→int`
    // SExt). Gated on `charConvertsToArith` (default false → a non-C schema keeps
    // `Char` strictly distinct from the integer ranks); mirrors the
    // `boolWidensToArith` gate. Closes D-CSUBSET-CHAR-INT-WIDENING.
    if (charConvertsToArith
        && ((lk == TypeKind::Char
             && (signedIntRank(rk) != 0 || unsignedIntRank(rk) != 0))
            || (rk == TypeKind::Char
                && (signedIntRank(lk) != 0 || unsignedIntRank(lk) != 0)))) {
        return true;
    }
    // C 6.7.2.2: an enumeration constant / enum-typed value HAS an integer
    // type (the implementation-defined underlying type — here the enum's
    // scalars[0], default int). So an enum is implicitly assignable to AND
    // from the integer ranks in BOTH directions: enum→int (`int x = BLUE;`,
    // `return color;` from an int-returning fn — widening/identity) and
    // int→enum (`enum Color c = 1;`, the `c += 1` read-modify-write-back).
    // Gated on `enumConvertsToArith` (default false → a non-C schema keeps
    // `Enum` strictly distinct from the integer ranks); mirrors the
    // `charConvertsToArith` gate. SCOPE: admits enum↔INT only — an
    // enum↔DIFFERENT-enum pair satisfies neither disjunct (signedIntRank/
    // unsignedIntRank of an Enum kind is 0), so it stays a loud mismatch (a
    // C constraint violation); same-enum is already caught by the sameType
    // identity above. Closes D-CSUBSET-ENUM-INT-CONVERSION.
    if (enumConvertsToArith
        && ((lk == TypeKind::Enum
             && (signedIntRank(rk) != 0 || unsignedIntRank(rk) != 0))
            || (rk == TypeKind::Enum
                && (signedIntRank(lk) != 0 || unsignedIntRank(lk) != 0)))) {
        return true;
    }
    // C 6.3.1.3 / 6.5.16.1 (D-CSUBSET-INT-CROSS-SIGNEDNESS-CONVERT): a signed integer
    // and an unsigned integer are mutually assignable — value-preserving in range,
    // modular out of range (`int x = u;`, `unsigned u = i;`, `return i;`/`f(u)` across
    // the signedness boundary), in BOTH directions and at ANY width. The same-signedness
    // rank arms above already RETURNED for matching signedness, so this arm is reached
    // only for a signed↔unsigned MIX (one rank is signed, the other unsigned); a
    // non-int rhs/lhs (float/char/enum — already handled, or int↔float) fails both
    // disjuncts and stays a loud mismatch. The HIR `coerce()` arithmetic-core arm
    // materializes the width-exact Cast, so the post-coerce verifier (gate default
    // false) stays strict. Gated on `intCrossSignednessConverts` (default false → a
    // non-C schema keeps signed/unsigned strictly distinct); mirrors the
    // charConvertsToArith / enumConvertsToArith gates. SAME-signedness narrowing
    // (`int x = aLong`) is UNAFFECTED — it returned at the rank arms above.
    if (intCrossSignednessConverts
        && ((signedIntRank(lk) != 0 && unsignedIntRank(rk) != 0)
            || (unsignedIntRank(lk) != 0 && signedIntRank(rk) != 0))) {
        return true;
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
    // C-standard function-to-pointer decay (C 6.3.2.1p4): a function
    // designator of type `FnSig` is implicitly assignable to a
    // `Ptr<FnSig>` of the SAME signature — `int (*fp)(int,int); fp = add;`,
    // the most common function-pointer idiom (vtables / callbacks /
    // dispatch tables). This is the SIBLING of the array-to-pointer decay
    // above and part of the SAME shared-lattice structural-decay family
    // (D-LANG-STRUCTURAL-DECAY-OPT-OUT) — ungated, because a function name
    // has no other use as an rvalue (a function value cannot be copied), so
    // admitting the decay never masks a real mismatch. UNLIKE array decay,
    // NO synthetic HIR decay node is needed: a bare function Ref inherently
    // lowers to the function's ADDRESS at MIR time (which is why `fp = add`
    // compiled AND ran correctly before the assign-stmt assignability check
    // existed — only the new SEMANTIC check rejected it). Pinned to the SAME
    // signature (the lhs pointee FnSig == the rhs FnSig, by interner
    // identity): an incompatible-signature assignment (`int (*g)(int) = add`
    // where add is `int(int,int)`) interns a DISTINCT FnSig and stays a loud
    // mismatch. The arm is the shared `isAssignable` chokepoint, so it fixes
    // assignment, initialization, call-argument, and return positions at
    // once. Closes the `fp = add` regression of
    // D-SEMANTIC-ASSIGN-STMT-ASSIGNABILITY-BYPASS.
    if (lk == TypeKind::Ptr && rk == TypeKind::FnSig) {
        auto const lhsElem = interner.operands(lhs);
        if (!lhsElem.empty() && lhsElem[0] == rhs) {
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
            // c27 (D-CSUBSET-VOLATILE-POINTEE): a POINTEE volatile-qualifier
            // difference is assignment-compatible — C 6.5.16.1 lets the lhs
            // pointee ADD qualifiers (`int *` → `volatile int *`, the sqlite WAL
            // shape `s.p = &x`). Compare the pointees MODULO their top-level
            // VolatileQual skin; identical material pointees ⇒ compatible. (C also
            // diagnoses DROPPING volatile `volatile int *` → `int *`; we admit
            // both directions for the shipped C surface — the qualifier never
            // changes layout/codegen, only the access flag, which is keyed off the
            // ACCESSED type, so a dropped-qualifier pointer simply yields a plain
            // access through the lhs's stripped pointee — never a miscompile.)
            if (interner.stripVolatile(lhsElem[0])
                == interner.stripVolatile(rhsElem[0])) {
                return true;
            }
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
//   * Ptr ← FnSig — a function DESIGNATOR decays to its address (c37,
//     D-CSUBSET-FUNCTION-DESIGNATOR-CAST; mapCast: FnSig→Ptr Bitcast). A
//     function→integer cast stays REJECTED (target not Ptr).
//   * InvalidType on either side → allowed (cascade suppression, same
//     posture as isAssignable).
//
// Everything else is false → the analyzer emits S_InvalidCast. Notably
// REJECTED (fail loud, never miscompile): struct/union VALUES (C forbids
// casts to composite types), `void` as the OPERAND (a void value cannot
// convert to anything; the `(void)expr` DISCARD direction — void as the
// TARGET — is admitted separately by `isVoidDiscardCast` below, FC3.5
// sweep-c3), and Array-typed TARGETS (C 6.5.4 forbids casts to array
// types).
//
// ARRAY-TYPED OPERANDS decay first (D-CSUBSET-CAST-ARRAY-DECAY ✅
// FC3.5 sweep-c3): per C 6.3.2.1p3 the cast operand undergoes the
// array-to-pointer conversion BEFORE the cast applies, so
// `(char*)"str"` is Ptr↔Ptr legality-wise (and `(long)arr` is the
// Ptr→integer round-trip). The legality view simply re-kinds the
// operand as Ptr; the HIR cast epilogue (`combineCast`, cst_to_hir.cpp)
// emits the SAME synthetic decay Cast the implicit path uses, so the
// value side always sees pointer-typed input.
[[nodiscard]] inline bool isExplicitCastable(TypeInterner const& interner,
                                             TypeId target,
                                             TypeId operand) noexcept {
    if (!target.valid() || !operand.valid()) return true;   // cascade suppression
    auto const tk = interner.kind(target);
    auto const ok0 = interner.kind(operand);
    // C 6.3.2.1p3 array-to-pointer decay on the OPERAND side only —
    // an Array TARGET stays rejected below (no arm admits it).
    auto const ok = (ok0 == TypeKind::Array) ? TypeKind::Ptr : ok0;
    // Mirrors mapCast's isInt: every integral scalar the MIR lattice
    // casts between (incl. Bool/Char/Byte — C casts these freely).
    auto const isCastableInt = [](TypeKind k) noexcept {
        using namespace detail::type_rules;
        return signedIntRank(k) != 0 || unsignedIntRank(k) != 0
            || k == TypeKind::Char || k == TypeKind::Byte
            || k == TypeKind::Bool || k == TypeKind::Enum;
    };
    auto const isCastableScalar = [&](TypeKind k) noexcept {
        return isCastableInt(k) || detail::type_rules::floatRank(k) != 0;
    };
    if (isCastableScalar(tk) && isCastableScalar(ok)) return true;
    if (tk == TypeKind::Ptr && ok == TypeKind::Ptr)   return true;
    // c37 (D-CSUBSET-FUNCTION-DESIGNATOR-CAST) C 6.3.2.1p4 + 6.3.2.3p8: a
    // function DESIGNATOR (FnSig) decays to the function's ADDRESS, so a cast
    // to ANY pointer target is value-preserving (mapCast lowers FnSig→Ptr as a
    // Bitcast over the GlobalAddr — already present from c12; NO HIR/MIR change).
    // Cross-signature fn-ptr casts ARE legal (C 6.3.2.3p8 — calling THROUGH an
    // incompatible type is UB, the CAST is not), so this is NOT gated on a
    // signature match — the sqlite `(sqlite3_destructor_type)fn` /
    // `(sqlite3_syscall_ptr)fn` shapes are exactly that conversion. SIBLING:
    // `isAssignable`'s Ptr/FnSig arm admits the same decay for init/assign/arg;
    // this closes the explicit-CAST path. A non-Ptr TARGET (`(long)g`) and a
    // non-FnSig OPERAND (`(fp)struct`) stay rejected (the tk==Ptr / ok==FnSig
    // guards do not fire for them) — verified by red-on-disable pins.
    if (tk == TypeKind::Ptr && ok == TypeKind::FnSig) return true;
    if (tk == TypeKind::Ptr && isCastableInt(ok))     return true;
    if (isCastableInt(tk) && ok == TypeKind::Ptr)     return true;
    return false;
}

// FC3.5 sweep-c3 (D-CSUBSET-CAST-VOID-DISCARD): the C discard idiom
// `(void)expr` — C 6.5.4p2 exempts a void TARGET from the scalar-
// operand constraint and C 6.3.2.2 defines the semantics as evaluate-
// and-discard. ANY operand type (scalar, pointer, array, struct, even
// void itself) is admissible. Kept SEPARATE from `isExplicitCastable`
// deliberately: everything that matrix admits must be lowerable by
// MIR's `mapCast`, while a void discard produces NO Cast node at all —
// the cast epilogue `combineCast` (cst_to_hir.cpp) keeps the operand's
// already-lowered value (lowered for its side effects) and re-types it
// void, wrapping it in no Cast node (an expression-statement effect).
// The analyzer's cast-legality site checks this FIRST; a void target
// never reaches the matrix.
[[nodiscard]] inline bool isVoidDiscardCast(TypeInterner const& interner,
                                            TypeId target) noexcept {
    return target.valid() && interner.kind(target) == TypeKind::Void;
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

// ── Per-verb expression-type DERIVATIONS — the SINGLE source ────────────
//
// The closed-universal result-type laws for the `Deref`/`Index` operator
// verbs (the verb VOCABULARY is config — `hirLowering.{unaryOps,postfixOps}`
// `target` strings; the per-verb DERIVATION is the verb's universal
// definition, exactly as `usualArithmeticCommonType` is for `Add`). BOTH the
// CST→HIR lowering (`cst_to_hir.cpp`) and the semantic-tier expression typer
// (`subtreeType`/`exprType`) call these, so the two tiers cannot drift
// (`D-SEMANTIC-SUBTREETYPE-TRANSPARENT-WRAPPERS`). `AddressOf` is a one-liner
// (`interner.pointer(t)`) inlined at both sites; the binary/ternary verbs use
// `usualArithmeticCommonType` below.

// `Deref` (`*p`): pointee of a pointer. C 6.5.3.2p4 designator decay as a
// lattice law — `*` on a function designator (`FnSig`) or a function POINTER
// (`Ptr<FnSig>`) is the IDENTITY (the designator decays straight back), so the
// result is the operand type unchanged; a deref node there would lower to a
// memory LOAD through the code pointer (silent garbage). Otherwise `Ptr<T>`→T;
// a non-pointer operand → InvalidType (cascade-suppress; deeper tiers wall it).
[[nodiscard]] inline TypeId
derefResultType(TypeInterner const& interner, TypeId operand) noexcept {
    if (!operand.valid()) return InvalidType;
    TypeKind const opk = interner.kind(operand);
    if (opk == TypeKind::FnSig) return operand;                       // identity
    if (opk == TypeKind::Ptr && !interner.operands(operand).empty()
        && interner.kind(interner.operands(operand)[0]) == TypeKind::FnSig)
        return operand;                                              // identity
    if (opk == TypeKind::Ptr) return interner.operands(operand)[0];   // pointee
    return InvalidType;
}

// `Index` (`a[i]`): the element type of the base — an Array/Ptr/Slice (each
// stores its element as operand[0]); any other base → InvalidType.
[[nodiscard]] inline TypeId
indexResultType(TypeInterner const& interner, TypeId base) noexcept {
    if (!base.valid()) return InvalidType;
    TypeKind const bk = interner.kind(base);
    if ((bk == TypeKind::Array || bk == TypeKind::Ptr || bk == TypeKind::Slice)
        && !interner.operands(base).empty())
        return interner.operands(base)[0];
    return InvalidType;
}

// ── FC3 c1: config-driven usual arithmetic conversions (C 6.3.1.8) ──────
//
// The `arithmeticConversions` SemanticConfig block, RESOLVED for the
// active DataModel (the load-resolved `DataModelTypeRef`s collapse to
// concrete TypeKinds here, once per (schema × dataModel)). Consumed by
// the CST→HIR combine sites (binary / ternary / compound-assign): a
// language WITH the block runs `usualArithmeticCommonType` below; a
// language WITHOUT it keeps the legacy `TypeInterner::commonType`
// EXACTLY (toy/tsql — pinned by their typing-unchanged tests). The
// engine never hardcodes C's view of int/char/bool — the promotion
// floor, the extra promoted kinds, and the mixed-signedness verb are all
// declared by the language and validated fail-loud at load.
struct ResolvedArithmeticRules {
    TypeKind              minRank = TypeKind::I32;  // promotion floor kind
    std::vector<TypeKind> alsoPromote;              // out-of-lattice promoted kinds
    MixedSignednessRule   mixedSignedness = MixedSignednessRule::RankPreferUnsigned;
    bool                  promoteComparisons = true;
    ShiftResultRule       shiftResult = ShiftResultRule::PromotedLeft;  // C 6.5.7
};

[[nodiscard]] inline ResolvedArithmeticRules
resolveArithmeticRules(ArithmeticConversions const& cfg, DataModel dm) {
    ResolvedArithmeticRules out;
    out.minRank = cfg.minRankType.resolveCore(dm);
    out.alsoPromote.reserve(cfg.alsoPromote.size());
    for (auto const& p : cfg.alsoPromote) out.alsoPromote.push_back(p.resolveCore(dm));
    out.mixedSignedness    = cfg.mixedSignedness;
    out.promoteComparisons = cfg.promoteComparisons;
    out.shiftResult        = cfg.shiftResult;
    return out;
}

namespace detail::type_rules {

// Width rank of any core integer kind (signed or unsigned): 1..5 for
// 8..128 bits; 0 = outside the integer rank lattice.
[[nodiscard]] inline constexpr int intWidthRank(TypeKind k) noexcept {
    int const s = signedIntRank(k);
    return s != 0 ? s : unsignedIntRank(k);
}

// Inverse mapper: the canonical kind at (width rank, signedness).
[[nodiscard]] inline constexpr TypeKind kindAtRank(int rank, bool isSigned) noexcept {
    switch (rank) {
        case 1: return isSigned ? TypeKind::I8   : TypeKind::U8;
        case 2: return isSigned ? TypeKind::I16  : TypeKind::U16;
        case 3: return isSigned ? TypeKind::I32  : TypeKind::U32;
        case 4: return isSigned ? TypeKind::I64  : TypeKind::U64;
        case 5: return isSigned ? TypeKind::I128 : TypeKind::U128;
        default: return TypeKind::Void;
    }
}

// Integer promotion (C 6.3.1.8 step 1, parameterized): a kind in the
// rules' `alsoPromote` set, or one whose width rank is BELOW the floor's,
// promotes to the floor kind (value-preserving: every promoted kind's
// range fits the floor — C's Bool/Char/I8..U16 all fit int). Kinds at or
// above the floor — and kinds outside the lattice that the language did
// NOT list (pointers, structs, floats) — pass through unchanged.
[[nodiscard]] inline TypeKind
promoteIntegerKind(TypeKind k, ResolvedArithmeticRules const& rules) noexcept {
    for (TypeKind p : rules.alsoPromote) {
        if (p == k) return rules.minRank;
    }
    int const r     = intWidthRank(k);
    int const floor = intWidthRank(rules.minRank);
    if (r != 0 && r < floor) return rules.minRank;
    return k;
}

} // namespace detail::type_rules

// C 6.7.2.2: an enum's "real" arithmetic type is its underlying integer
// type. When an enum participates in the usual arithmetic conversions
// (`e + 1`, `e == other`, a shift count), it does so AS that underlying int
// — the result is int, never enum. This resolves an Enum TypeId to its
// underlying primitive (scalars[0]); any non-enum passes through unchanged.
// Used by `usualArithmeticCommonType` / `integerPromotedType` below so the
// closed arithmetic verb never has to special-case Enum. Part of
// D-CSUBSET-ENUM-INT-CONVERSION.
[[nodiscard]] inline TypeId
enumUnderlyingOrSelf(TypeInterner& interner, TypeId t) {
    if (!t.valid() || interner.kind(t) != TypeKind::Enum) return t;
    auto const sc = interner.scalars(t);
    return sc.empty() ? t : interner.primitive(static_cast<TypeKind>(sc[0]));
}

// The C 6.3.1.8 common type of two operands under the language's
// declared rules — the config-driven sibling of
// `TypeInterner::commonType` (which keeps serving block-less languages
// byte-identically). Returns InvalidType for non-arithmetic pairs (the
// caller falls back to its structural rule, exactly the commonType
// contract). Algorithm:
//   1. float hierarchy: if either operand is float, BOTH must be
//      arithmetic; the result is the wider float (int converts to the
//      float side).
//   2. integer promotion per `promoteIntegerKind` (floor + alsoPromote).
//   3. same kind → it; same signedness → wider rank; mixed signedness
//      per the closed verb. `rank-prefer-unsigned` (C): unsigned rank ≥
//      signed rank → the unsigned kind at its rank; else the signed kind
//      (a strictly-wider power-of-two signed type represents the whole
//      unsigned range, so C's third branch — "the unsigned counterpart
//      of the signed type" — is unreachable over width-distinct core
//      kinds; it exists only for same-width different-rank ABO types).
[[nodiscard]] inline TypeId
usualArithmeticCommonType(TypeInterner& interner, TypeId a, TypeId b,
                          ResolvedArithmeticRules const& rules) {
    if (!a.valid() || !b.valid()) return InvalidType;
    using namespace detail::type_rules;
    a = enumUnderlyingOrSelf(interner, a);
    b = enumUnderlyingOrSelf(interner, b);
    TypeKind const ka = interner.kind(a);
    TypeKind const kb = interner.kind(b);
    auto const isArith = [&](TypeKind k) noexcept {
        if (floatRank(k) != 0 || intWidthRank(k) != 0) return true;
        for (TypeKind p : rules.alsoPromote) {
            if (p == k) return true;
        }
        return false;
    };
    int const fa = floatRank(ka);
    int const fb = floatRank(kb);
    if (fa != 0 || fb != 0) {
        if (!isArith(ka) || !isArith(kb)) return InvalidType;
        if (fa != 0 && fb != 0) return fa >= fb ? a : b;
        return fa != 0 ? a : b;
    }
    TypeKind const pa = promoteIntegerKind(ka, rules);
    TypeKind const pb = promoteIntegerKind(kb, rules);
    int const ra = intWidthRank(pa);
    int const rb = intWidthRank(pb);
    if (ra == 0 || rb == 0) return InvalidType;   // not arithmetic
    if (pa == pb) return interner.primitive(pa);
    bool const sa = signedIntRank(pa) != 0;
    bool const sb = signedIntRank(pb) != 0;
    if (sa == sb) {
        return interner.primitive(kindAtRank(ra >= rb ? ra : rb, sa));
    }
    // Mixed signedness — closed verb (loader rejects unknown spellings,
    // so this switch is exhaustive over the declared vocabulary).
    switch (rules.mixedSignedness) {
        case MixedSignednessRule::RankPreferUnsigned: {
            int const  uRank = sa ? rb : ra;
            int const  sRank = sa ? ra : rb;
            if (uRank >= sRank) {
                return interner.primitive(kindAtRank(uRank, /*isSigned=*/false));
            }
            return interner.primitive(kindAtRank(sRank, /*isSigned=*/true));
        }
    }
    return InvalidType;
}

// One operand's integer promotion as a TypeId (C 6.5.7: a SHIFT's result
// type is the PROMOTED LEFT operand — the count's type never
// contributes). Non-arithmetic and float operands pass through.
[[nodiscard]] inline TypeId
integerPromotedType(TypeInterner& interner, TypeId t,
                    ResolvedArithmeticRules const& rules) {
    if (!t.valid()) return t;
    using namespace detail::type_rules;
    t = enumUnderlyingOrSelf(interner, t);
    TypeKind const k = interner.kind(t);
    if (floatRank(k) != 0) return t;
    TypeKind const p = promoteIntegerKind(k, rules);
    if (p == k) return t;
    return interner.primitive(p);
}

// C 6.5.7 shift RESULT TYPE under the config verb `shiftResult`
// (D-UAC-SHIFT-RESULT-RULE-CONFIG) — the SINGLE chokepoint both the CST→HIR
// shift lowering and the semantic-tier expression typer call, so the two tiers
// can never diverge on the verb (a regression to one is a regression to both,
// caught by the one test). `PromotedLeft` (C): the PROMOTED LEFT operand — the
// count's type never contributes (`i32 << i64` is I32). `CommonType`: the
// usual-arithmetic common type — a shift typed like an ordinary binary op
// (`i32 << i64` is I64). Either falls back to the first valid operand for a
// non-arithmetic pair, mirroring the generic binary result rule.
[[nodiscard]] inline TypeId
shiftResultType(TypeInterner& interner, TypeId lhs, TypeId rhs,
                ResolvedArithmeticRules const& rules) {
    if (rules.shiftResult == ShiftResultRule::PromotedLeft) {
        TypeId const lp = integerPromotedType(interner, lhs, rules);
        return lp.valid() ? lp : (lhs.valid() ? lhs : rhs);
    }
    TypeId const common = usualArithmeticCommonType(interner, lhs, rhs, rules);
    return common.valid() ? common : (lhs.valid() ? lhs : rhs);
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
