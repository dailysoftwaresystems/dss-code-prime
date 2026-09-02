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

// c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY) / D-MIR-OVERLAP-STRUCT-ZERO-INIT /
// D-CORE-COMPOSITE-OVERLAP-CLAIM-BLIND-TO-BITFIELDS: do two DISTINCT members of
// composite `id` occupy INTERSECTING BYTES under this ABI — is
// `[begin_i, end_i)` ∩ `[begin_j, end_j)` non-empty for some i≠j? The authority
// for that question, consumed by the two tiers that must refuse (or specially
// handle) a FULL-WIDTH positional member-wise write: the MIR brace-init lowering
// and the static-data encoder.
//
// TWO CHANNELS put members in the same bytes, and BOTH are swept:
//   * EXPLICIT per-field byte offsets — an FFI overlay a descriptor pins
//     (`ULARGE_INTEGER {QuadPart u64@0, LowPart u32@0, HighPart u32@4}`). A
//     member's extent is its own type's size.
//   * BIT-FIELDS — `struct { unsigned a:3; unsigned b:5; }` puts both members in
//     ONE byte (under `__attribute__((packed))` the whole struct is that byte:
//     `fieldOffsets` 0 and 0, `bitOffset` 0 and 3, pinned by
//     `TypeInterner.PackedBitfieldCompositeLaysOutGnuTight`). A bit-field member's
//     extent is the bytes ITS BITS land in — `off + bitOffset/8` up to
//     `off + ceil((bitOffset+bitWidth)/8)`.
// The O(1) short-circuit survives for a composite with NEITHER channel, where the
// engine's monotonic invariant (each field at or after the previous field's end)
// makes an intersection impossible.
//
// ⚠ A BIT-FIELD'S EXTENT IS ITS BITS' BYTES, **NOT** ITS ALLOCATION UNIT, and that
// distinction is load-bearing rather than pedantic — it is the difference between a
// correct answer and a false positive on a shape the shipped corpus contains.
// ✔MEASURED (gcc 13.3.0 + clang 18.1.3, x86_64-linux, `-std=c17 -c`, one
// `_Static_assert` battery, both rc=0): `struct { unsigned a:3; char x; }` is
// sizeof 4 with `offsetof(x) == 1` — the ordinary member is packed INSIDE the
// bit-field's 4-byte unit, which is the `gnu_packed` rule this engine implements —
// so sweeping UNIT ranges reports a and x as sharing bytes when they provably do
// not; `examples/c/bitfield_init`'s `struct T { char x; unsigned a:3; unsigned
// b:4; }` is the same shape from the other side (sizeof 4, `offsetof(x) == 0`, a
// AND b in byte 1, a's unit still anchored at 0). Likewise `struct { unsigned a:16;
// unsigned b:16; }` (sizeof 4) shares ONE unit while a owns bytes [0,2) and b owns
// [2,4). Sweeping BITS answers `false`, `false` and (for T) `true` from a ∩ b —
// each correctly.
//
// ★ THAT MAKES THIS THE "DO THEY SHARE BYTES" AUTHORITY AND **NOT** A "IS A
// POSITIONAL WRITE SAFE" ORACLE, WHICH IS A DIFFERENT QUESTION WITH A DIFFERENT
// ANSWER. A full-width member-wise write of ANY bit-field composite is unsafe
// whatever this returns — writing `a:3` as a whole `unsigned` clobbers every
// co-resident neighbour in the unit. The predicate for THAT question is
// `computeLayout(...)->bitFields` being NON-EMPTY (the layout authority's own
// invariant: non-empty ⇔ the composite has a bit-field), and it is what BOTH
// callers route on, ahead of asking this:
//   * `hir_to_mir.cpp`'s `lowerAggregateInitIntoSlot` tests `hasBitfieldMember`
//     and diverts to `lowerBitfieldAggregateInitIntoSlot` BEFORE this gate;
//   * `asm.cpp`'s `encodeAggregateValue` hoists its `computeLayout` above the gate
//     and consults this ONLY when `bitFields` is empty, then packs bit-fields by
//     pre-zero + OR into the unit rather than by positional full-width writes.
// So when this IS consulted by either caller the composite has no bit-fields, and
// a `true` can only have come from the explicit-offset channel — which is what
// keeps both refusal messages ("overlapping explicit-offset struct") accurate.
//
// ✔NO EXCLUSION REMAINS — D-CORE-COMPOSITE-OVERLAP-CLAIM-BLIND-TO-UNIONS closed the
// last one, and it closed it by CHANGING THE ANSWER rather than by narrowing the
// sentence. A UNION whose members carry neither channel used to reach the O(1)
// short-circuit and answer `false`, though every member sits at offset 0 and they
// therefore share bytes BY DEFINITION. It now falls through to the layout + range
// sweep like any other composite and answers `true` whenever two or more of its
// members are sizeable (a single-member union, or one whose members are all
// zero-size, correctly stays `false` — there is nothing to intersect).
//
// ⓘ THE SHORT-CIRCUIT IS NOW KEYED ON `Struct`, AND THAT IS A SCOPE, NOT AN
// EXCEPTION. Its justification is a theorem about the natural/packed BYTE PATH —
// `off = alignUp(off); push(off); off += size` cannot emit an intersection for any
// field list — and that path is what lays out a struct. The union arm of
// `computeLayout` does not advance an offset at all; it places every member at 0
// and folds a max size, so the theorem was never about unions and the old code was
// applying it outside its domain. Keeping the struct fast path is deliberate: it is
// what makes the common shape O(1) with no layout computed at all, and routing
// structs through the sweep would buy uniformity the answer does not need at the
// cost of a full `computeLayout` per naturally-laid-out struct in the corpus.
//
// ★ AND THE TRUTHFUL ANSWER IS WHY BOTH CALLERS NOW ROUTE UNIONS AWAY FROM THIS
// GATE, which is the same shape the bit-field closure applied and NOT a way around
// the predicate. The gate's question is "would a positional member-wise write
// clobber a sibling", and for a union brace-init the answer is NO whatever the
// members share: C 6.7.9p17 initializes exactly ONE member, so there is exactly one
// write and no sibling to lose. ✔MEASURED — THREE independent things already give
// that: `lowerUnionBraceInit` returns a one-child `ConstructAggregate`,
// `synthZeroOrError`'s composite arm computes `n = (core == Union) ? 1 :
// ops.size()`, and `HirVerifier::checkConstructAggregate` refuses any other child
// count. BOTH callers nevertheless ASSERT it rather than resting on it, because all
// three guarantees live UPSTREAM and the verifier is a separate pass neither caller
// runs: a multi-child union aggregate reaching either one would be silently written
// member-wise, second write clobbering the first at offset 0, which is precisely
// the permission this routing grants and the gate exists to refuse.
//
// A purely STRUCTURAL question about the type under one ABI — no target, format,
// or language identity enters (the ABI arrives only through `params`/`dm`, exactly
// as `computeLayout`'s does; the bit→byte mapping is `BitFieldPlacement`'s own
// LSB-first model, the only one the engine has). Member SIZES are ABI-dependent
// (`long` is 4 or 8) and so is bit placement, so overlap must be asked of a
// LAID-OUT type, never of the bare field list.
//
// Zero-SIZE members occupy no bytes and can never overlap; they are skipped, as
// are the two members that occupy none BY CONSTRUCTION — a zero-width bit-field
// (`unsigned : 0;`, a packing break whose `fieldOffsets` entry aliases the NEXT
// unit) and a flexible array member (its unsized tail contributes nothing).
// An UN-COMPUTABLE layout (incomplete/out-of-scope field, an unrealized bit-field
// strategy, a straddler the placement model cannot express) answers `true` — the
// CONSERVATIVE direction, so a caller keeps its LOUD refusal rather than silently
// admitting a layout it could not verify.
//
// ✔THAT PROMISE HOLDS FOR EVERY COMPOSITE THIS SWEEPS, WHICH IS THE SECOND DEFECT
// D-CORE-COMPOSITE-OVERLAP-CLAIM-BLIND-TO-BITFIELDS RECORDED AND IT IS FIXED BY
// BEHAVIOUR, not by narrowing the sentence: the WHOLE-composite layout is attempted
// before any range is derived, where the old code reached a layout only per-FIELD
// and only past the explicit-offset gate. A bit-field composite whose layout the
// engine refuses (`pack(2) struct { unsigned a; unsigned long long b:40; }`, or any
// composite under an undeclared `bitFieldStrategy`) used to answer a PERMISSIVE
// `false`; it now answers `true`.
//
// ⓘ The one case that still answers `false` WITHOUT attempting a layout is a
// STRUCT with NEITHER channel, and there `false` is CORRECT rather than
// permissive — it is a property of the placement ALGORITHM, not of a particular
// outcome. The natural/packed byte path places each field at `alignUp(off)` and
// then advances `off` by that field's size, so it cannot emit an intersection for
// ANY field list, sizeable or not; a flexible array member takes an offset and no
// advance, so it contributes no range either way. A UNION is not laid out by that
// path at all and is no longer routed through this short-circuit — see the
// D-CORE-COMPOSITE-OVERLAP-CLAIM-BLIND-TO-UNIONS note above.
[[nodiscard]] DSS_EXPORT bool
compositeFieldsOverlap(TypeId id, TypeInterner const& interner,
                       AggregateLayoutParams params, DataModel dm);

// ── D-CSUBSET-VOID-POINTER-ARITHMETIC-REFUSED: the OPERAND size, which is a
//    DIFFERENT QUESTION from the object layout above ─────────────────────────
//
// `computeLayout` answers "how is an OBJECT of this type STORED". `sizeof`,
// `_Alignof` and the element stride of pointer arithmetic ask something else:
// "what size does this type have as an OPERAND". For every type that HAS an
// object representation the two answers coincide, which is why one function
// served both for so long — but they diverge on exactly the types that have no
// object representation at all, and there the difference is the whole point.
//
// ISO C keeps them together: 6.5.3.4p1 forbids `sizeof` on a function type or an
// incomplete type (`void` is one, permanently — 6.2.5p19), and 6.5.6p2 admits
// pointer arithmetic only on a complete OBJECT type. GNU C splits them, and both
// reference compilers implement the split identically: gcc's manual states the
// rule once, as "the size of a void or of a function" being 1, which is what
// makes `void *p; p + 1;` byte arithmetic and `sizeof(void) == 1`.
// ✔MEASURED at P42, gcc 13.3.0 `-std=c2x` and clang 18.1.3 `-std=c23` probed
// SEPARATELY, every shape BUILT AND RUN: both accept `p = p + 1` / `p += 2` /
// `p++` / `++p` / `--p` / `p -= 1` / `q - p` / a `const void *` / `sizeof(void)`
// / `sizeof(*p)` / `sizeof(f)` / `sizeof(*f)` / `_Alignof(void)` / `fp + 0`.
// Under `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` a unanimous ACCEPTANCE settles it.
//
// ★ THE OBJECT QUESTION IS DELIBERATELY LEFT REFUSING, AND THAT IS THE SAFETY
//   PROPERTY. `computeLayout` still nullopts for `void`/`FnSig`, so `void x;`,
//   `struct T { void x; };` and `void a[3];` cannot start silently ALLOCATING —
//   all three are constraint violations both references reject, and the first
//   two were silently ACCEPTED by DSS before P42 (now `S_IncompleteTypeObject` /
//   `S_IncompleteTypeMember`). Sizing `void` at the object question would have
//   traded 19 conformance closures for a silent-allocation class.

// The sizes a DIALECT assigns to types that have NO object representation.
// ABSENT (the default) = the type has no operand size either, so every query
// keeps the strict-ISO refusal — a schema that declares nothing is byte-identical
// to the pre-P42 engine.
//
// ⚠ TWO NAMED FIELDS, NOT A MAP KEYED BY `TypeKind`, and the choice is
// deliberate. The set of kinds for which this question is even MEANINGFUL is
// exactly {Void, FnSig}: they are the only two that can never have an object
// representation. `computeLayout`'s `default:` arm also catches Slice/Tuple/
// Vector/Matrix/Nullable/Optional/Param/Bind/Extension, but those are types a
// future language would give a REAL layout, not objectless ones — a map would
// invite a schema to declare `"struct": 3` and silently override the engine that
// actually knows. The failure mode decides it: a mistyped named field is caught
// LOUD at config load by the block's own unknown-key check, whereas a mistyped
// map entry is either the same error (no gain) or a silently ignored line. And a
// genuinely-new objectless lattice kind is ALREADY a hand edit — `computeLayout`,
// `isMemoryResidentType` and `isByValueClass` all have `default:` arms that
// `-Werror=switch` cannot police — so the pair is consistent with how every other
// kind-keyed decision in this engine is made. `operandLayout` falls through to
// `computeLayout` for any kind not named here, i.e. it REFUSES rather than
// guesses: a third objectless kind fails loud until someone teaches it, which is
// the direction this project chooses every time.
struct NonObjectTypeSizes {
    std::optional<std::uint64_t> voidBytes;      // TypeKind::Void
    std::optional<std::uint64_t> functionBytes;  // TypeKind::FnSig
};

// The size + alignment of `id` as the OPERAND of `sizeof`/`_Alignof`, or as the
// ELEMENT of pointer arithmetic. Byte-identical to `computeLayout` for every type
// that has a layout — the two non-object kinds are the ONLY ones that consult
// `sizes`, and only when the dialect declared them. The synthesized layout is
// `{size = declared, align = 1}`: an object of these types does not exist, so the
// alignment is the one a byte-addressed offset needs, and `_Alignof(void) == 1`
// is what both references report. nullopt keeps the caller's existing fail-loud.
//
// ★ THE SINGLE STRIDE RULE. Every element-scaling site (`elementStride` →
// `scaleIndexToBytes`, the `p ± n` and `p - q` arms, the MIR `SizeOf`/`_Alignof`
// cases, the semantic const-fold `resolveSizeof`/`resolveAlignof`) routes through
// HERE, so there is no second stride rule to drift — the one rule simply stopped
// asking the object question on behalf of the operand one.
[[nodiscard]] DSS_EXPORT std::optional<StructLayout>
operandLayout(TypeId id, TypeInterner const& interner,
              AggregateLayoutParams params, DataModel dm,
              NonObjectTypeSizes const& sizes);

} // namespace dss
