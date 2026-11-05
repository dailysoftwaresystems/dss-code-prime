#pragma once

#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

// ── P69 (lane `cs`, fold f26b): C's COMPATIBILITY, AND ITS COMPOSITE, AT EVERY
//    DERIVATION LEVEL — pointers, arrays and FUNCTIONS, the one relation and the one
//    builder every site asks ──
//
// C 6.2.7p1 with 6.7.6.1p2, 6.7.6.2p6 and 6.7.6.3p15: two pointers are compatible
// when they point to compatible types identically qualified; two arrays when their
// elements are compatible and their sizes agree where both are integer constants;
// two functions when their results are compatible and their parameter lists agree in
// number, in the ellipsis and in each parameter, each taken UNQUALIFIED — and the
// relation goes on through every level, a function's parameters and its result
// included. Their COMPOSITE (C 6.2.7p3) is the type built the same way level by level,
// each array taking the size either one knows, so it may be a type neither one spells
// (`void (*)(int (*)[], int (*)[3])` and `void (*)(int (*)[2], int (*)[])` give
// `void (*)(int (*)[2], int (*)[3])`). ✔MEASURED 2026-10-16 (lane `cs`'s probe rc5):
// gcc 13.3.0, clang 18.1.3 and mingw-w64 gcc 13.2.0 accept and run two file-scope
// declarations of one object whose composite lies below an array of pointers
// (`int (*ap[2])[];` / `int (*ap[2])[3];`, both orders), inside a pointed-to
// function's parameter or result, and the composite neither spells, reading the
// composite's sizes after them; all three refuse two known sizes that differ at any
// level (cl 19.51 refuses the array-level cells, C2369, and accepts the function-level
// ones with C4028 / C4113, the two controls included). Until this fold the relation
// stopped at a function type, so DSS refused all nine cells and warned an
// incompatible pointer conversion where the two function types are compatible.
//
// WHERE AN ARRAY IS COMPARED. Below a pointer always. At the TOP only where the caller
// asks (`ArraysCompared::Anywhere`): the declarations of one OBJECT take the composite
// of two arrays (`extern int a[]; int a[3];`), while a conversion, a `_Generic`
// association or a parameter never meets an array at its top (it has decayed or been
// adjusted to a pointer by then).
//
// AGNOSTIC and ITERATIVE: an explicit heap work stack, so the depth of a derivation
// costs no host stack, and a pair already met is not walked again.
enum class ArraysCompared : std::uint8_t { BelowAPointer, Anywhere };

namespace detail::compat {

[[nodiscard]] inline std::uint64_t pairKey(TypeId a, TypeId b) {
    return (static_cast<std::uint64_t>(a.v) << 32) | b.v;
}

// Pushes onto `work` the pairs of the two types' next level that must be compatible
// for `x` and `y` (both of kind `k`, both UNQUALIFIED at their own top) to be, and
// answers false where the level itself already differs. A pushed pair is unqualified
// at its top, its qualifiers having been compared here.
template <class Push>
[[nodiscard]] inline bool nextLevel(TypeInterner const& in, TypeKind k, TypeId x, TypeId y,
                                    bool arraysHere, Push&& push) {
    auto const ox = in.operands(x);
    auto const oy = in.operands(y);
    switch (k) {
        case TypeKind::Ptr: {
            if (ox.empty() || oy.empty()) return false;
            if (in.qualifierBits(ox[0]) != in.qualifierBits(oy[0])) return false;
            push(in.stripVolatile(ox[0]), in.stripVolatile(oy[0]));
            return true;
        }
        case TypeKind::Array: {
            if (!arraysHere || ox.empty() || oy.empty()) return false;
            auto const lx = in.scalars(x);
            auto const ly = in.scalars(y);
            if (lx.empty() || ly.empty() || !arrayLengthsCompatible(lx[0], ly[0])) return false;
            if (in.qualifierBits(ox[0]) != in.qualifierBits(oy[0])) return false;
            push(in.stripVolatile(ox[0]), in.stripVolatile(oy[0]));
            return true;
        }
        case TypeKind::FnSig: {
            // operands = [result, params...], scalars = [calling convention, variadic].
            auto const sx = in.scalars(x);
            auto const sy = in.scalars(y);
            if (sx.size() != sy.size() || !std::equal(sx.begin(), sx.end(), sy.begin())) return false;
            if (ox.size() != oy.size() || ox.empty()) return false;
            // C23 6.7.6.3p15: each parameter (and the result) is taken unqualified — but
            // `_Atomic T` is a type of its own, not a qualified spelling of `T`.
            for (std::size_t i = 0; i < ox.size(); ++i) {
                if (in.isAtomicQualified(ox[i]) != in.isAtomicQualified(oy[i])) return false;
            }
            for (std::size_t i = ox.size(); i-- > 0;) push(in.stripVolatile(ox[i]), in.stripVolatile(oy[i]));
            return true;
        }
        default:
            return false;   // any other kind is compatible with itself alone (identity, asked first)
    }
}

}  // namespace detail::compat

// Are `a` and `b` — both UNQUALIFIED at the top — compatible types (C 6.2.7p1): the
// same type, an enumerated type and its compatible integer type, or pointers, arrays
// (`arrays` says where) or functions built of such pairs?
[[nodiscard]] inline bool compatibleTypes(TypeInterner const& interner, TypeId a, TypeId b,
                                          ArraysCompared arrays) {
    struct Pair {
        TypeId x, y;
        bool   arraysHere;
    };
    std::vector<Pair>               work{{a, b, arrays == ArraysCompared::Anywhere}};
    std::unordered_set<std::uint64_t> met;
    while (!work.empty()) {
        Pair const p = work.back();
        work.pop_back();
        if (!p.x.valid() || !p.y.valid()) return false;
        if (p.x == p.y) continue;
        if (!met.insert(detail::compat::pairKey(p.x, p.y)).second) continue;
        TypeId const ex = interner.enumUnderlyingType(p.x);
        if (ex.valid() && ex == p.y) continue;
        TypeId const ey = interner.enumUnderlyingType(p.y);
        if (ey.valid() && ey == p.x) continue;
        TypeKind const k = interner.kind(p.x);
        if (k != interner.kind(p.y)) return false;
        bool const levelAgrees = detail::compat::nextLevel(
            interner, k, p.x, p.y, p.arraysHere, [&](TypeId nx, TypeId ny) { work.push_back({nx, ny, true}); });
        if (!levelAgrees) return false;
    }
    return true;
}

// The relation as every site that meets no array at its top asks it: the same type,
// an enumerated type and its compatible integer type, or pointers to such pairs (an
// array below a pointer compatible with one of a compatible size), or functions —
// identically qualified at every level below.
[[nodiscard]] inline bool sameOrEnumCompatible(TypeInterner const& interner,
                                               TypeId a, TypeId b) {
    return compatibleTypes(interner, a, b, ArraysCompared::BelowAPointer);
}

// THE COMPOSITE of two compatible types (C 6.2.7p3), interned: each level built from
// the composite of the level below, an array taking the size either type knows (a
// constant size beats a variable one, which beats none), a function the composite of
// its result and of each parameter, every qualifier where the two types state it
// alike (a parameter's, which the relation ignores, only where the two agree). Of an
// enumerated type and its integer type, `a`'s. `a` and `b` may each carry a qualifier
// skin at their top; the two must agree, and the composite carries `a`'s.
// InvalidType where they are not compatible. ⚠ INTERNS: a caller holding a span of the
// interner's storage re-reads it after asking.
[[nodiscard]] inline TypeId compositeType(TypeInterner& in, TypeId a, TypeId b, ArraysCompared arrays) {
    if (!a.valid() || !b.valid()) return InvalidType;
    if (a == b) return a;
    if (in.qualifierBits(a) != in.qualifierBits(b) || in.typeAlignOverride(a) != in.typeAlignOverride(b))
        return InvalidType;
    TypeId const ua = in.stripVolatile(a);
    TypeId const ub = in.stripVolatile(b);
    if (!compatibleTypes(in, ua, ub, arrays)) return InvalidType;
    // `like`'s qualifier skin on `t`.
    auto const qualifiedAs = [&in](TypeId t, TypeId like) {
        std::int64_t const  bits  = in.qualifierBits(like);
        std::uint32_t const align = in.typeAlignOverride(like);
        return bits == 0 && align == 0 ? t : in.qualified(t, bits, align);
    };
    // Post-order over the pairs: a frame is built once every pair below it is.
    struct Frame {
        TypeId x, y;
        bool   expanded = false;
    };
    std::unordered_map<std::uint64_t, TypeId> built;
    std::vector<Frame>                        frames{{ua, ub}};
    auto const composite = [&](TypeId x, TypeId y) { return built.at(detail::compat::pairKey(x, y)); };
    while (!frames.empty()) {
        Frame const f   = frames.back();
        auto const  key = detail::compat::pairKey(f.x, f.y);
        if (built.contains(key)) {
            frames.pop_back();
            continue;
        }
        TypeKind const k = in.kind(f.x);
        if (f.x == f.y || in.kind(f.y) != k
            || (k != TypeKind::Ptr && k != TypeKind::Array && k != TypeKind::FnSig)) {
            built.emplace(key, f.x);   // the same type, or an enumerated type and its integer type
            frames.pop_back();
            continue;
        }
        // The level's own operands, copied: interning below may move the storage a span reads.
        std::vector<TypeId> const ox(in.operands(f.x).begin(), in.operands(f.x).end());
        std::vector<TypeId> const oy(in.operands(f.y).begin(), in.operands(f.y).end());
        if (!f.expanded) {
            frames.back().expanded = true;
            for (std::size_t i = ox.size(); i-- > 0;) {
                TypeId const nx = in.stripVolatile(ox[i]);
                TypeId const ny = in.stripVolatile(oy[i]);
                if (!built.contains(detail::compat::pairKey(nx, ny))) frames.push_back({nx, ny});
            }
            continue;
        }
        // Each operand's composite, with the qualifiers the two state alike.
        std::vector<TypeId> ops(ox.size());
        for (std::size_t i = 0; i < ox.size(); ++i) {
            TypeId const c = composite(in.stripVolatile(ox[i]), in.stripVolatile(oy[i]));
            bool const   alike = in.qualifierBits(ox[i]) == in.qualifierBits(oy[i])
                              && in.typeAlignOverride(ox[i]) == in.typeAlignOverride(oy[i]);
            ops[i] = alike ? qualifiedAs(c, ox[i]) : c;
        }
        TypeId result = InvalidType;
        if (k == TypeKind::Ptr) {
            result = in.pointer(ops[0]);
        } else if (k == TypeKind::Array) {
            std::int64_t const lx = in.scalars(f.x)[0];
            std::int64_t const ly = in.scalars(f.y)[0];
            auto const fixed = [](std::int64_t n) { return n != kIncompleteArrayLength && n != kVlaLength; };
            if (fixed(lx) || fixed(ly)) result = in.array(ops[0], fixed(lx) ? lx : ly);
            else if (lx == kVlaLength || ly == kVlaLength) result = in.vlaArray(ops[0]);
            else result = in.incompleteArray(ops[0]);
        } else {
            std::vector<std::int64_t> const sx(in.scalars(f.x).begin(), in.scalars(f.x).end());
            std::vector<TypeId> const       params(ops.begin() + 1, ops.end());
            result = in.fnSig(params, ops[0], static_cast<CallConv>(sx[0]), sx.size() > 1 && sx[1] != 0);
        }
        built.emplace(key, result);
        frames.pop_back();
    }
    return qualifiedAs(composite(ua, ub), a);
}

}  // namespace dss
