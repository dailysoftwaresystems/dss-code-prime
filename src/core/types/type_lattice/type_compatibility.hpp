#pragma once

#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"

// ── P68 round 12 (lane `cs`, the enumeration P1): AN ENUMERATED TYPE AND ITS
//    COMPATIBLE INTEGER TYPE ARE COMPATIBLE — the ONE relation every site asks ──
//
// C 6.2.7p1 / 6.7.2.2p4 (C23 6.7.3.3p13, p16): each enumerated type is compatible
// with the integer type its record keeps — the declared one for a fixed underlying
// type, the one the language CHOSE otherwise (`TypeInterner::enumUnderlyingType`) —
// and pointers to compatible types are compatible (6.7.6.1p2) at every level,
// identically qualified. Two DIFFERENT enumerated types stay incompatible with each
// other even when both are compatible with one integer type: compatibility is not
// transitive. A kind-only enum record (a language that declares no choice) is
// compatible with nothing but itself.
//
// Asked by `_Generic`'s association match, the pointer arms of assignment and of
// the `==` / `?:` pairings, the array-to-pointer decay's element test, and the
// redeclaration oracles (functions and objects) — one owner, so the four can never
// answer the same pair two ways. ✔MEASURED 2026-09-24 (lane `cs`'s
// `.temp/probe/ect4`, `ect8`): gcc 13.3.0, clang 18.1.3 and mingw-w64 13.2.0 select
// the compatible type's `_Generic` association for an enumeration object, convert a
// pointer to one into a pointer to the other silently, and accept a redeclaration
// across the pair; DSS did none of the three
// ([[D-C-AN-ENUMERATED-TYPE-IS-NOT-COMPATIBLE-WITH-ITS-UNDERLYING-INTEGER-TYPE]]).
//
// AGNOSTIC: pure queries over `TypeInterner` const accessors; nothing is interned,
// so no caller's GuardedSpan is invalidated by asking.

namespace dss {

// ── P69 (lane `cs`): AN ARRAY BELOW A POINTER IS COMPATIBLE WITH ONE WHOSE SIZE IS
//    UNKNOWN, OR NOT A CONSTANT ──
//
// C 6.7.6.2p6: two array types are compatible when their element types are, and their
// sizes must agree only "if both size specifiers are present and integer constant
// expressions" — so an array of unknown size (`kIncompleteArrayLength`, which the
// prototype-only `[*]` builds too) and a variable length one (`kVlaLength`) are
// compatible with an array of any size of a compatible element. Asked BELOW A POINTER
// only: there a pair has no composite to keep (a pointer is a pointer), while at the
// top an object's declarations take the composite (C 6.2.7p3) and the object
// redeclaration's own arm, which builds it, answers them. ✔MEASURED (lane `cs`, probe
// vm cell 8): `int f(int n, int a[n][*]);` beside `int f(int n, int a[n][3]) {…}` —
// parameters `int (*)[]` and `int (*)[3]` — is built and RUN by gcc 13.3.0 and clang
// 18.1.3; the redeclaration oracle refused it ("parameter 2 has a different type").
[[nodiscard]] inline bool arrayLengthsCompatible(std::int64_t a, std::int64_t b) {
    return a == b || a == kIncompleteArrayLength || b == kIncompleteArrayLength
        || a == kVlaLength || b == kVlaLength;
}

// Are `a` and `b` — both UNQUALIFIED at the top — the same type, an enumerated type
// and its compatible integer type, or pointers to such pairs (an array below a
// pointer compatible with one of a compatible size), identically qualified at every
// level below? Iterative: the depth of a pointer chain costs no host stack.
[[nodiscard]] inline bool sameOrEnumCompatible(TypeInterner const& interner,
                                               TypeId a, TypeId b) {
    bool belowPointer = false;
    while (true) {
        if (!a.valid() || !b.valid()) return false;
        if (a == b) return true;
        TypeId const ea = interner.enumUnderlyingType(a);
        if (ea.valid() && ea == b) return true;
        TypeId const eb = interner.enumUnderlyingType(b);
        if (eb.valid() && eb == a) return true;
        TypeKind const ka = interner.kind(a);
        TypeKind const kb = interner.kind(b);
        bool const arrays = belowPointer && ka == TypeKind::Array && kb == TypeKind::Array;
        if (!arrays && (ka != TypeKind::Ptr || kb != TypeKind::Ptr)) return false;
        if (arrays) {
            auto const la = interner.scalars(a);
            auto const lb = interner.scalars(b);
            if (la.empty() || lb.empty() || !arrayLengthsCompatible(la[0], lb[0]))
                return false;
        }
        auto const pa = interner.operands(a);
        auto const pb = interner.operands(b);
        if (pa.empty() || pb.empty()) return false;
        TypeId const na = pa[0];
        TypeId const nb = pb[0];
        if (interner.qualifierBits(na) != interner.qualifierBits(nb)) return false;
        a = interner.stripVolatile(na);
        b = interner.stripVolatile(nb);
        belowPointer = true;
    }
}

// P69 (lane `cs`): of two types `sameOrEnumCompatible` pairs, the one that is ALSO
// their composite (C 6.2.7p3) where one is: at every array level below a pointer it
// knows the size wherever the other does (a constant size beats an unknown or a
// variable one). Invalid when each knows a size the other does not — a composite
// neither declaration spells — and `a` when they differ in no array size.
[[nodiscard]] inline TypeId completerOfCompatible(TypeInterner const& interner,
                                                  TypeId a, TypeId b) {
    auto const fixed = [](std::int64_t n) {
        return n != kIncompleteArrayLength && n != kVlaLength;
    };
    bool aKnowsMore = false;
    bool bKnowsMore = false;
    TypeId x = a;
    TypeId y = b;
    while (x.valid() && y.valid() && x != y) {
        TypeKind const kx = interner.kind(x);
        if (kx != interner.kind(y) || (kx != TypeKind::Ptr && kx != TypeKind::Array))
            break;
        if (kx == TypeKind::Array) {
            auto const lx = interner.scalars(x);
            auto const ly = interner.scalars(y);
            if (lx.empty() || ly.empty()) break;
            aKnowsMore = aKnowsMore || (fixed(lx[0]) && !fixed(ly[0]));
            bKnowsMore = bKnowsMore || (fixed(ly[0]) && !fixed(lx[0]));
        }
        auto const ox = interner.operands(x);
        auto const oy = interner.operands(y);
        if (ox.empty() || oy.empty()) break;
        x = interner.stripVolatile(ox[0]);
        y = interner.stripVolatile(oy[0]);
    }
    if (aKnowsMore && bKnowsMore) return InvalidType;
    return bKnowsMore ? b : a;
}

}  // namespace dss
