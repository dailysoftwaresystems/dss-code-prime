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

// Are `a` and `b` — both UNQUALIFIED at the top — the same type, an enumerated type
// and its compatible integer type, or pointers to such pairs, identically qualified
// at every level below? Iterative: the depth of a pointer chain costs no host stack.
[[nodiscard]] inline bool sameOrEnumCompatible(TypeInterner const& interner,
                                               TypeId a, TypeId b) {
    while (true) {
        if (!a.valid() || !b.valid()) return false;
        if (a == b) return true;
        TypeId const ea = interner.enumUnderlyingType(a);
        if (ea.valid() && ea == b) return true;
        TypeId const eb = interner.enumUnderlyingType(b);
        if (eb.valid() && eb == a) return true;
        if (interner.kind(a) != TypeKind::Ptr || interner.kind(b) != TypeKind::Ptr)
            return false;
        auto const pa = interner.operands(a);
        auto const pb = interner.operands(b);
        if (pa.empty() || pb.empty()) return false;
        TypeId const na = pa[0];
        TypeId const nb = pb[0];
        if (interner.qualifierBits(na) != interner.qualifierBits(nb)) return false;
        a = interner.stripVolatile(na);
        b = interner.stripVolatile(nb);
    }
}

}  // namespace dss
