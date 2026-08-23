#pragma once

#include "core/export.hpp"
#include "core/substrate/arena_attribute.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/alignment.hpp"
#include "core/types/data_model.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// ── FC6: the struct/union/array LAYOUT engine (realizes D-FF3-1, layout half) ──
//
// A GENERIC, target-AGNOSTIC engine that computes the byte layout of any complete
// type — field offsets, alignment, padding, total size, flexible-array-member
// handling — from (a) the field TypeKinds (FC3 already baked each field's width
// into its TypeId, so widths need no second lookup except pointers), (b) the
// format's `DataModel` (the OS-dependent pointer width), and (c) the target's
// `AggregateLayoutParams` (the per-ABI alignment rule + max alignment, declared in
// `.target.json`). The engine NEVER branches on a target/format/language name — it
// runs ONE bounded natural-alignment algorithm parameterized by the declared
// params, so a future non-natural ABI is a config change, not an engine change.
//
// Layout is target/ABI-dependent, so it is NOT baked into the interned
// `TypeRecord` (that would make a language-level interned type target-specific);
// it lives in an `ArenaAttribute<TypeInterner, StructLayout>` side-table — the
// codebase's established side-table pattern. FC7 (member-access codegen) and
// `sizeof`-folding consume it via the table.

namespace dss {

// FC8 bitfields (D-CSUBSET-BITFIELD): the bit placement of one struct field. A
// field is a bit-field iff `unitBytes != 0`. For a bit-field, the access is a
// `unitBytes`-byte integer load/store at the field's `fieldOffsets[i]`, and the
// field occupies `bitWidth` bits starting at bit `bitOffset` within that unit
// (LSB-first; both shipped targets are little-endian). An ordinary field has
// `unitBytes == 0` (use `fieldOffsets[i]` + the field's own type for the access).
struct BitFieldPlacement {
    std::uint32_t unitBytes = 0;   // 0 = ordinary field; else the load/store width
    std::uint32_t bitOffset = 0;   // bit offset within the unit
    std::uint32_t bitWidth  = 0;   // declared width (a zero-width bitfield is a
                                   // layout-only break — it never appears here as
                                   // an addressable field)
};

// The computed layout of a complete type. For a scalar/pointer/array the
// `fieldOffsets` are empty; for a struct/union there is one offset per field
// (declaration order). `size` excludes a flexible-array-member's unsized tail.
struct StructLayout {
    std::uint64_t              size = 0;       // total bytes (FAM tail excluded)
    Alignment                  align{};        // alignment requirement (pow2)
    std::vector<std::uint64_t> fieldOffsets;   // byte offset per field (struct/union)
    bool                       hasFlexibleArrayMember = false;
    // FC8 bitfields: per-field bit placement, parallel to `fieldOffsets`. EMPTY
    // when the struct has NO bit-field (every existing layout is byte-identical).
    // When non-empty there is one entry per field; an ordinary field's entry has
    // `unitBytes == 0`. A zero-width bit-field is a packing break only — it gets a
    // `fieldOffsets` slot (so indices stay parallel) with `unitBytes == 0`.
    std::vector<BitFieldPlacement> bitFields;
};

// Side-table keyed by TypeId — the per-CU memoized layout table FC7 reads.
using TypeLayoutTable = substrate::ArenaAttribute<TypeInterner, StructLayout>;

// The byte size of a SCALAR/pointer TypeKind under a data model. nullopt for any
// kind that is not a sized scalar (Void, aggregates, FnSig/Slice/… — the caller's
// fail-loud signal; aggregates are sized by `computeLayout`, not this). Pointer-
// class kinds (Ptr/Ref/FnPtr) take the `DataModel` pointer width.
[[nodiscard]] DSS_EXPORT std::optional<std::uint64_t>
scalarByteSize(TypeKind kind, DataModel dm) noexcept;

// C23 _BitInt(N) (D-CSUBSET-BITINT): the TypeId-aware companion to
// `scalarByteSize`. A `_BitInt(N)`'s size cannot be derived from its KIND alone
// (the width N lives in the interned record's scalars), so a caller that has a
// TypeId — a data-global leaf, an aggregate-ABI leaf — routes through here: it
// returns the `_BitInt(N)` CONTAINER byte size (N≤64 → {1,2,4,8}; N>64 →
// ceil(N/64) eightbytes, the C2 multi-limb layout) and, for every OTHER kind,
// exactly `scalarByteSize(kind, dm)`. nullopt for a non-sized kind (aggregate /
// Void / FnSig / …) — the caller's fail-loud signal, same as `scalarByteSize`.
[[nodiscard]] DSS_EXPORT std::optional<std::uint64_t>
sizeOfScalarOrBitInt(TypeInterner const& interner, TypeId id, DataModel dm) noexcept;

// ── The WIDE-INTEGER facade (D-CSUBSET-BITINT-C2-WIDE + D-CSUBSET-UINT128-TYPE) ──
//
// A WIDE integer is one with NO single native container — a MULTI-LIMB value
// (ceil(N/64) little-endian i64 limbs) that is memory-resident and reached by
// ADDRESS. Three kinds qualify: a C23 `_BitInt(N>64)` (the C2 arm), and the
// 128-bit standard-rank integers `I128`/`U128` (`__int128` / `unsigned __int128`).
// The 128-bit kinds join by SHAPE, not by identity: they are exactly 2 limbs, so
// every shipped multi-limb emitter (`emitWideAddSub`/`emitWideMul`/`emitWideDivMod`/
// … in hir_to_mir) drives them unchanged once width + signedness come from here
// instead of from the BitInt-only interner accessors. This facade is the ONE place
// that knows the membership — a caller asking "is this multi-limb?" must ask here,
// never re-derive it from a TypeKind test, so a fourth wide kind is one edit.
//
// TF-C94 MEASURED: generalizing this predicate is what makes `isMemoryResidentType`
// / `isByValueClass` return true for I128/U128 and therefore what routes 128-bit
// values through the limb emitters instead of a bare SSA scalar (which would carry
// only the low 8 bytes). NOT a target/format/language branch — pure type shape.
//
// ⚠ `_BitInt(128)` and `U128` are INDEPENDENT types here and MUST stay so: both are
// 16 bytes, but `_BitInt(128)` has align 8 (x86-64 psABI, `computeLayout`'s BitInt
// arm) while `U128` has align 16 (`scalarByteSize` + the natural-alignment rule).
// Sharing a limb COUNT is not sharing a LAYOUT — do not unify them.
//
// `true` iff `id` is a wide integer. A `_BitInt(N≤64)` stays a single native
// container (C1) — false; so is every non-integer kind and the invalid TypeId.
[[nodiscard]] DSS_EXPORT bool
isWideInt(TypeInterner const& interner, TypeId id) noexcept;

// The width in bits of a WIDE integer: the interned width for a `_BitInt(N>64)`,
// 128 for I128/U128. FAILS LOUD (abort) for anything else — every call site is
// downstream of an `isWideInt` gate, so a non-wide argument is a broken invariant,
// never user input. This mirrors `TypeInterner::bitIntWidth` (type_lattice.cpp)
// exactly, which is the point: a facade site the TF-C94 sweep MISSED still aborts
// on a 128-bit value rather than silently sizing a limb loop from a guess.
[[nodiscard]] DSS_EXPORT std::int64_t
wideIntWidthBits(TypeInterner const& interner, TypeId id);

// The signedness of a WIDE integer: the interned flag for a `_BitInt(N>64)`, `true`
// for I128, `false` for U128. FAILS LOUD (abort) for anything else — same contract
// and same reasoning as `wideIntWidthBits`, mirroring `TypeInterner::bitIntIsSigned`
// (type_lattice.cpp). Signedness drives every limb sign-fill, the top-limb mask,
// and the ordered-compare's top-limb arm, so a guessed answer is wrong BYTES.
[[nodiscard]] DSS_EXPORT bool
wideIntIsSigned(TypeInterner const& interner, TypeId id);

// C99 _Complex (D-CSUBSET-COMPLEX): a pure type-shape query — `true` iff `id` is a
// Complex kind. The dedicated helper the by-address contract funnels through: the
// hir_to_mir request value->address FLIP, the lowerLvalueAddressNode materialize
// dispatch, and the combineBinaryOp/combineCast misroute guards all key on it
// (mirroring `isWideInt`). No target/format/language identity — Complex only
// ever appears in a `_Complex`-declaring schema, so this is inert elsewhere.
[[nodiscard]] DSS_EXPORT bool
isComplex(TypeInterner const& interner, TypeId id) noexcept;

// C23 _BitInt(N>64) + __int128 (D-CSUBSET-BITINT-C2-WIDE / D-CSUBSET-UINT128-TYPE):
// the by-construction STORAGE/GUARD predicate. A wide integer is MEMORY-RESIDENT —
// like an aggregate it has NO SSA register value and is always reached by ADDRESS.
// `true` for Struct/Union/Array/Complex AND any `isWideInt` kind (a wide
// `_BitInt(N>64)`, I128, U128). The alloca-sizing site + the anti-resurrection
// guards (an aggregate/wide value reaching a bare-SSA position) funnel through here
// so coverage is BY CONSTRUCTION (§A.5), not by enumerating edits. A pure type-shape
// query — no target/format/language identity (the agnostic bar).
// ⚠ TF-C94: this function has a `default:` arm, so `-Werror=switch` / C4062
// cannot see a missed kind here — those are the NO-`default` flavor. ✔RE-MEASURED
// 2026-08-23: the flag is now PROJECT-WIDE (the root `CMakeLists.txt`), not the
// src/core-only opt-in this note used to name, and the conclusion is unchanged
// because the `default:` arm is what defeats it. A missed kind is a SILENT
// `return false`, not a build error. Any new memory-resident kind must be added
// here BY HAND.
[[nodiscard]] DSS_EXPORT bool
isMemoryResidentType(TypeInterner const& interner, TypeId id) noexcept;

// C23 _BitInt(N>64) + __int128 (D-CSUBSET-BITINT-C2-WIDE / D-CSUBSET-UINT128-TYPE):
// the by-VALUE-CLASS twin of `isMemoryResidentType` with ARRAY EXCLUDED — `true` for
// Struct/Union/Complex AND any `isWideInt` kind, but NOT Array (an array is never
// passed / returned / copy-assigned BY VALUE in C — it decays to a pointer). The
// calling-convention gates (call arg/return, param reception, ReturnStmt, the
// call-consumer arm) + the aggregate copy-init/assign sites funnel here. A pure
// type-shape query.
// ⚠ TF-C94: same `default:`-arm / no-`-Werror=switch` caveat as the sibling above.
[[nodiscard]] DSS_EXPORT bool
isByValueClass(TypeInterner const& interner, TypeId id) noexcept;

// Compute the full layout of a COMPLETE type. Recursive (nested aggregates) and
// PURE (no caching — the caller memoizes via `TypeLayoutTable`). Returns nullopt
// — the fail-loud signal, never a guessed size — when the type is INCOMPLETE
// (a bare flexible-array `T[]`, which has no standalone size) or OUT OF SCOPE
// (a FnSig/Slice/Vector/… field, or a Void field). The caller turns nullopt into
// a positioned diagnostic.
[[nodiscard]] DSS_EXPORT std::optional<StructLayout>
computeLayout(TypeId id, TypeInterner const& interner,
              AggregateLayoutParams params, DataModel dm);

// c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY) / D-MIR-OVERLAP-STRUCT-ZERO-INIT: do two
// DISTINCT fields of composite `id` occupy INTERSECTING byte ranges — i.e. is
// `[off_i, off_i+size_i)` ∩ `[off_j, off_j+size_j)` non-empty for some i≠j? THE
// single authority for "this struct's members share bytes", consumed by every
// tier that must refuse (or specially handle) a positional member-wise write:
// the MIR brace-init lowering and the static-data encoder.
//
// A purely STRUCTURAL question about the type under one ABI — no target, format,
// or language identity enters (the ABI arrives only through `params`/`dm`, exactly
// as `computeLayout`'s does). Field SIZES are ABI-dependent (`long` is 4 or 8), so
// overlap must be asked of a LAID-OUT type, never of the bare field list.
//
// Only the explicit-offset channel can answer true: natural layout places each
// field at or after the previous field's end (the engine's monotonic invariant),
// so a composite with no explicit offsets short-circuits to `false` in O(1).
// A UNION's members overlap BY DEFINITION — but a union is not laid out
// field-wise by its consumers, so it is reported through the same explicit-offset
// gate as a struct and answers `false` unless it actually carries offsets.
//
// Zero-SIZE fields occupy no bytes and can never overlap; they are skipped.
// An UN-COMPUTABLE layout (incomplete/out-of-scope field) answers `true` — the
// CONSERVATIVE direction, so a caller keeps its LOUD refusal rather than silently
// admitting a layout it could not verify.
[[nodiscard]] DSS_EXPORT bool
compositeFieldsOverlap(TypeId id, TypeInterner const& interner,
                       AggregateLayoutParams params, DataModel dm);

} // namespace dss
