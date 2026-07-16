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

// C23 _BitInt(N) (D-CSUBSET-BITINT-C2-WIDE): a `_BitInt(N)` whose width EXCEEDS 64
// bits — a C2 MULTI-LIMB value (ceil(N/64) little-endian i64 limbs). A pure
// type-shape query (no target/format/arch identity): `true` iff `id` is a BitInt
// with width > 64. A `_BitInt(N≤64)` stays a single native container (C1) — false.
[[nodiscard]] DSS_EXPORT bool
isWideBitInt(TypeInterner const& interner, TypeId id) noexcept;

// C99 _Complex (D-CSUBSET-COMPLEX): a pure type-shape query — `true` iff `id` is a
// Complex kind. The dedicated helper the by-address contract funnels through: the
// hir_to_mir request value->address FLIP, the lowerLvalueAddressNode materialize
// dispatch, and the combineBinaryOp/combineCast misroute guards all key on it
// (mirroring `isWideBitInt`). No target/format/language identity — Complex only
// ever appears in a `_Complex`-declaring schema, so this is inert elsewhere.
[[nodiscard]] DSS_EXPORT bool
isComplex(TypeInterner const& interner, TypeId id) noexcept;

// C23 _BitInt(N>64) (D-CSUBSET-BITINT-C2-WIDE): the by-construction STORAGE/GUARD
// predicate. A wide `_BitInt` is MEMORY-RESIDENT — like an aggregate it has NO SSA
// register value and is always reached by ADDRESS. `true` for Struct/Union/Array
// AND a wide `_BitInt(N>64)`. The alloca-sizing site + the anti-resurrection guards
// (an aggregate/wide-BitInt reaching a bare-SSA position) funnel through here so
// coverage is BY CONSTRUCTION (§A.5), not by enumerating edits. A pure type-shape
// query — no target/format/language identity (the agnostic bar).
[[nodiscard]] DSS_EXPORT bool
isMemoryResidentType(TypeInterner const& interner, TypeId id) noexcept;

// C23 _BitInt(N>64) (D-CSUBSET-BITINT-C2-WIDE): the by-VALUE-CLASS twin of
// `isMemoryResidentType` with ARRAY EXCLUDED — `true` for Struct/Union AND a wide
// `_BitInt(N>64)`, but NOT Array (an array is never passed / returned / copy-
// assigned BY VALUE in C — it decays to a pointer). The calling-convention gates
// (call arg/return, param reception, ReturnStmt, the call-consumer arm) + the
// aggregate copy-init/assign sites funnel here. A pure type-shape query.
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

} // namespace dss
