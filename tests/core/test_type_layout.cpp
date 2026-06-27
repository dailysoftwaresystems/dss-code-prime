// FC6: the struct/union/array LAYOUT engine — field offsets, alignment, padding,
// total size, flexible-array-member handling. Multi-form contract: every aggregate
// form is laid out by building its TypeId directly via the interner (incl. forms no
// shipped C program reaches yet — FAM, i128, deeply nested). The engine is target-
// AGNOSTIC: it runs ONE bounded natural-alignment algorithm parameterized by the
// declared params, proven by the agnosticism pin (different params → different layout).

#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include <gtest/gtest.h>

#include <array>

using namespace dss;

namespace {

[[nodiscard]] TypeInterner makeInterner(std::uint32_t owner) {
    return TypeInterner{CompilationUnitId{owner}};
}

// The shipped-target params (natural alignment, 16-byte ISA cap), LP64.
constexpr AggregateLayoutParams kNatural16{ScalarAlignmentRule::Natural, 16};

[[nodiscard]] StructLayout layoutOf(TypeId id, TypeInterner const& ti,
                                    AggregateLayoutParams p = kNatural16,
                                    DataModel dm = DataModel::Lp64) {
    auto const l = computeLayout(id, ti, p, dm);
    EXPECT_TRUE(l.has_value()) << "expected a layout for this type";
    return l.value_or(StructLayout{});
}

} // namespace

// ── scalars + pointers ──────────────────────────────────────────────────────

TEST(TypeLayout, ScalarSizesAndAligns) {
    auto ti = makeInterner(1);
    struct Case { TypeKind k; std::uint64_t size; std::uint32_t align; };
    for (Case const c : {
             Case{TypeKind::Bool, 1, 1}, Case{TypeKind::Char, 1, 1},
             Case{TypeKind::I8, 1, 1},   Case{TypeKind::U8, 1, 1},
             Case{TypeKind::I16, 2, 2},  Case{TypeKind::F16, 2, 2},
             Case{TypeKind::I32, 4, 4},  Case{TypeKind::F32, 4, 4},
             Case{TypeKind::I64, 8, 8},  Case{TypeKind::F64, 8, 8},
             Case{TypeKind::I128, 16, 16}, Case{TypeKind::F128, 16, 16},
         }) {
        auto const l = layoutOf(ti.primitive(c.k), ti);
        EXPECT_EQ(l.size, c.size) << "size of kind " << static_cast<int>(c.k);
        EXPECT_EQ(l.align.bytes(), c.align) << "align of kind " << static_cast<int>(c.k);
        EXPECT_TRUE(l.fieldOffsets.empty());
    }
}

TEST(TypeLayout, PointerWidthFollowsDataModel) {
    auto ti = makeInterner(1);
    TypeId const p = ti.pointer(ti.primitive(TypeKind::I32));
    EXPECT_EQ(layoutOf(p, ti, kNatural16, DataModel::Lp64).size, 8u);
    EXPECT_EQ(layoutOf(p, ti, kNatural16, DataModel::Llp64).size, 8u);
    EXPECT_EQ(layoutOf(p, ti, kNatural16, DataModel::Ilp32).size, 4u);
    EXPECT_EQ(layoutOf(p, ti, kNatural16, DataModel::Ilp32).align.bytes(), 4u);
}

// ── structs: the canonical padding cases ────────────────────────────────────

TEST(TypeLayout, StructCharIntCharIsThePaddingClassic) {
    auto ti = makeInterner(1);
    TypeId const c = ti.primitive(TypeKind::Char);
    TypeId const i = ti.primitive(TypeKind::I32);
    std::array<TypeId, 3> const fields{c, i, c};
    auto const l = layoutOf(ti.structType("S", fields), ti);
    // char@0, pad[1..3], int@4, char@8, pad[9..11] → size 12, align 4.
    ASSERT_EQ(l.fieldOffsets.size(), 3u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[1], 4u);
    EXPECT_EQ(l.fieldOffsets[2], 8u);
    EXPECT_EQ(l.size, 12u);
    EXPECT_EQ(l.align.bytes(), 4u);
    EXPECT_FALSE(l.hasFlexibleArrayMember);
}

TEST(TypeLayout, StructTailPaddingAndNesting) {
    auto ti = makeInterner(1);
    TypeId const i = ti.primitive(TypeKind::I32);
    TypeId const c = ti.primitive(TypeKind::Char);
    // {int, char} → int@0, char@4, size rounded to align 4 → 8.
    std::array<TypeId, 2> const ic{i, c};
    auto const flat = layoutOf(ti.structType("IC", ic), ti);
    EXPECT_EQ(flat.fieldOffsets[0], 0u);
    EXPECT_EQ(flat.fieldOffsets[1], 4u);
    EXPECT_EQ(flat.size, 8u);
    EXPECT_EQ(flat.align.bytes(), 4u);

    // {char, {int,char}} → inner@4 (align 4), outer size 12.
    TypeId const inner = ti.structType("IC", ic);
    std::array<TypeId, 2> const nest{c, inner};
    auto const outer = layoutOf(ti.structType("N", nest), ti);
    EXPECT_EQ(outer.fieldOffsets[0], 0u);
    EXPECT_EQ(outer.fieldOffsets[1], 4u);
    EXPECT_EQ(outer.size, 12u);
    EXPECT_EQ(outer.align.bytes(), 4u);
}

// ── unions, arrays, enums ───────────────────────────────────────────────────

TEST(TypeLayout, UnionIsMaxSizeAllAtOffsetZero) {
    auto ti = makeInterner(1);
    TypeId const c = ti.primitive(TypeKind::Char);
    TypeId const d = ti.primitive(TypeKind::F64);
    std::array<TypeId, 2> const fields{c, d};
    auto const l = layoutOf(ti.unionType("U", fields), ti);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[1], 0u);
    EXPECT_EQ(l.size, 8u);          // max(1, 8) rounded to align 8
    EXPECT_EQ(l.align.bytes(), 8u);
}

TEST(TypeLayout, ArrayStride) {
    auto ti = makeInterner(1);
    TypeId const i = ti.primitive(TypeKind::I32);
    auto const l = layoutOf(ti.array(i, 5), ti);
    EXPECT_EQ(l.size, 20u);         // 5 × 4
    EXPECT_EQ(l.align.bytes(), 4u);
    // An array of a padded struct uses the padded stride.
    TypeId const c  = ti.primitive(TypeKind::Char);
    std::array<TypeId, 2> const ic{i, c};
    TypeId const s  = ti.structType("IC", ic);   // size 8, align 4
    EXPECT_EQ(layoutOf(ti.array(s, 3), ti).size, 24u);  // 3 × 8
}

TEST(TypeLayout, EnumFollowsUnderlying) {
    auto ti = makeInterner(1);
    auto const l = layoutOf(ti.enumType("E", TypeKind::I32), ti);
    EXPECT_EQ(l.size, 4u);
    EXPECT_EQ(l.align.bytes(), 4u);
}

// ── flexible array member (FAM) ─────────────────────────────────────────────

TEST(TypeLayout, FlexibleArrayMemberContributesOffsetNotSize) {
    auto ti = makeInterner(1);
    TypeId const n   = ti.primitive(TypeKind::I32);
    TypeId const fam = ti.incompleteArray(ti.primitive(TypeKind::I32));
    std::array<TypeId, 2> const fields{n, fam};
    auto const l = layoutOf(ti.structType("Fam", fields), ti);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[1], 4u);   // FAM offset (element align 4)
    EXPECT_EQ(l.size, 4u);              // tail excluded — only `n`
    EXPECT_EQ(l.align.bytes(), 4u);
    EXPECT_TRUE(l.hasFlexibleArrayMember);

    // A BARE incomplete array has no standalone size — fail loud.
    EXPECT_FALSE(computeLayout(fam, ti, kNatural16, DataModel::Lp64).has_value());
}

TEST(TypeLayout, NonLastFlexibleArrayMemberFailsLoud) {
    // A FAM is legal ONLY as the last member. A FAM followed by another field
    // would silently overlay the unsized tail — the engine must fail loud
    // (nullopt) rather than mislay the trailing field. Red-on-disable: drop the
    // `i + 1 != fields.size()` guard and this struct lays out with `n` aliasing
    // `data`'s offset.
    auto ti = makeInterner(1);
    TypeId const fam = ti.incompleteArray(ti.primitive(TypeKind::I32));
    TypeId const n   = ti.primitive(TypeKind::I32);
    std::array<TypeId, 2> const fields{fam, n};   // FAM is NOT last
    EXPECT_FALSE(
        computeLayout(ti.structType("BadFam", fields), ti, kNatural16, DataModel::Lp64)
            .has_value());
}

// ── AGNOSTICISM PIN: different params → different layout (no hardcoded rule) ──

TEST(TypeLayout, MaxAlignmentCapChangesOffsetsRedOnDisable) {
    auto ti = makeInterner(1);
    TypeId const c = ti.primitive(TypeKind::Char);
    TypeId const d = ti.primitive(TypeKind::F64);   // size 8, natural align 8
    std::array<TypeId, 2> const fields{c, d};
    TypeId const s = ti.structType("CD", fields);

    // Natural (cap 16): char@0, double aligns to 8 → double@8, size 16, align 8.
    auto const wide = layoutOf(s, ti, AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    EXPECT_EQ(wide.fieldOffsets[1], 8u);
    EXPECT_EQ(wide.size, 16u);
    EXPECT_EQ(wide.align.bytes(), 8u);

    // i386-style cap 4: double's align CAPPED to 4 → double@4, size 12, align 4.
    // This diverges on a value all 4 current ABIs SHARE (16); a future hardcoded
    // `align==size` regression makes this case go RED.
    auto const capped = layoutOf(s, ti, AggregateLayoutParams{ScalarAlignmentRule::Natural, 4});
    EXPECT_EQ(capped.fieldOffsets[1], 4u);
    EXPECT_EQ(capped.size, 12u);
    EXPECT_EQ(capped.align.bytes(), 4u);
}

// ── FAIL-LOUD: out-of-scope field types / Void → nullopt, never a guessed size ─

TEST(TypeLayout, OutOfScopeTypesFailLoud) {
    auto ti = makeInterner(1);
    DataModel const dm = DataModel::Lp64;
    // Void has no size.
    EXPECT_FALSE(computeLayout(ti.primitive(TypeKind::Void), ti, kNatural16, dm).has_value());
    // A struct with a bare-function-typed field (FnSig) → rejected, not size 0.
    TypeId const i  = ti.primitive(TypeKind::I32);
    std::array<TypeId, 1> const params{i};
    TypeId const fn = ti.fnSig(params, i, CallConv::CcSysV);
    std::array<TypeId, 2> const bad{i, fn};
    EXPECT_FALSE(computeLayout(ti.structType("Bad", bad), ti, kNatural16, dm).has_value());
    // A struct with a Void field → rejected.
    TypeId const v = ti.primitive(TypeKind::Void);
    std::array<TypeId, 1> const badv{v};
    EXPECT_FALSE(computeLayout(ti.structType("BadV", badv), ti, kNatural16, dm).has_value());
}

// ── FC8 D-CSUBSET-BITFIELD: bit-field packing (gnu_packed, little-endian) ──────
// The params the shipped targets declare for bit-fields. Identical to kNatural16
// but with the packing strategy ON. RED-ON-DISABLE for the WHOLE feature: with
// `BitFieldStrategy::None` a struct that HAS a bit-field computes no layout.
constexpr AggregateLayoutParams kGnu16{
    ScalarAlignmentRule::Natural, 16, BitFieldStrategy::GnuPacked};

// A struct with NO bit-field interns with EMPTY scalars → `bitFields` empty AND
// the byte path is byte-identical (the anyBitfield gate). This pins that the
// bitfield machinery NEVER perturbs an ordinary struct.
TEST(TypeLayout, BitFieldFreeStructUnchangedAndNoBitFieldsVector) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::I32), ti.primitive(TypeKind::I32)};
    auto const l = layoutOf(ti.structType("S", fields), ti, kGnu16);
    EXPECT_EQ(l.size, 8u);
    EXPECT_TRUE(l.bitFields.empty());
    EXPECT_EQ(l.fieldOffsets[1], 4u);
}

// Adjacent bit-fields pack LSB-first into ONE allocation unit: a:3 at bit 0,
// b:5 at bit 3 — both in the 4-byte unit at offset 0; struct size 4.
// RED-ON-DISABLE: a regressed packer that treats each as a full int would place
// b at offset 4 and size 8.
TEST(TypeLayout, BitFieldPacksAdjacentIntoOneUnit) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::U32), ti.primitive(TypeKind::U32)};
    std::array<std::int64_t, 2> const widths{3, 5};
    auto const l = layoutOf(ti.structType("S", fields, widths), ti, kGnu16);
    ASSERT_EQ(l.bitFields.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[1], 0u);
    EXPECT_EQ(l.bitFields[0].unitBytes, 4u);
    EXPECT_EQ(l.bitFields[0].bitOffset, 0u);
    EXPECT_EQ(l.bitFields[0].bitWidth, 3u);
    EXPECT_EQ(l.bitFields[1].bitOffset, 3u);
    EXPECT_EQ(l.bitFields[1].bitWidth, 5u);
    EXPECT_EQ(l.size, 4u);
}

// A bit-field that would straddle its type's unit boundary starts the NEXT unit:
// a:30 fills bits 0..29 of unit 0; b:5 (30+5 > 32) cannot fit → unit 1 (offset
// 4), bit 0; struct size 8.
TEST(TypeLayout, BitFieldStraddleStartsNewUnit) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::U32), ti.primitive(TypeKind::U32)};
    std::array<std::int64_t, 2> const widths{30, 5};
    auto const l = layoutOf(ti.structType("S", fields, widths), ti, kGnu16);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.bitFields[0].bitOffset, 0u);
    EXPECT_EQ(l.fieldOffsets[1], 4u);
    EXPECT_EQ(l.bitFields[1].bitOffset, 0u);
    EXPECT_EQ(l.size, 8u);
}

// A zero-width unnamed bit-field forces the next field to a fresh unit boundary:
// a:3 in unit 0; the `:0` breaks; b:3 starts unit 1 (offset 4).
TEST(TypeLayout, BitFieldZeroWidthForcesNewUnit) {
    auto ti = makeInterner(1);
    std::array<TypeId, 3> const fields{
        ti.primitive(TypeKind::U32), ti.primitive(TypeKind::U32),
        ti.primitive(TypeKind::U32)};
    std::array<std::int64_t, 3> const widths{3, 0, 3};  // middle = zero-width break
    auto const l = layoutOf(ti.structType("S", fields, widths), ti, kGnu16);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.bitFields[0].bitOffset, 0u);
    EXPECT_EQ(l.bitFields[1].unitBytes, 0u);   // the break is not an addressable field
    EXPECT_EQ(l.fieldOffsets[2], 4u);          // c forced to the next unit
    EXPECT_EQ(l.bitFields[2].bitOffset, 0u);
}

// A bit-field followed by an ORDINARY field: the ordinary field closes the open
// bit-unit and lands at the next aligned BYTE offset (not packed into the unit).
TEST(TypeLayout, BitFieldThenOrdinaryFieldClosesUnit) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::U32), ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 2> const widths{3, kNotBitfield};
    auto const l = layoutOf(ti.structType("S", fields, widths), ti, kGnu16);
    EXPECT_EQ(l.bitFields[0].bitWidth, 3u);
    EXPECT_EQ(l.bitFields[1].unitBytes, 0u);   // ordinary field
    EXPECT_EQ(l.fieldOffsets[1], 4u);          // n at the next int slot
    EXPECT_EQ(l.size, 8u);
}

// A struct WITH a bit-field but NO declared strategy fails loud (nullopt) — a
// missing rule can never silently bake a wrong placement. RED-ON-DISABLE for the
// config gate.
TEST(TypeLayout, BitFieldWithoutStrategyFailsLoud) {
    auto ti = makeInterner(1);
    std::array<TypeId, 1> const fields{ti.primitive(TypeKind::U32)};
    std::array<std::int64_t, 1> const widths{3};
    // kNatural16 has bitFieldStrategy == None.
    EXPECT_FALSE(
        computeLayout(ti.structType("S", fields, widths), ti, kNatural16,
                      DataModel::Lp64).has_value());
}

// The interner round-trips the per-field width via `fieldBitWidth`, and a
// bitfield-free struct interns BIT-IDENTICALLY to the 2-arg overload.
TEST(TypeLayout, InternerFieldBitWidthRoundTripAndIdentity) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::U32), ti.primitive(TypeKind::U32)};
    std::array<std::int64_t, 2> const widths{3, kNotBitfield};
    TypeId const bf = ti.structType("S", fields, widths);
    ASSERT_TRUE(ti.fieldBitWidth(bf, 0).has_value());
    EXPECT_EQ(*ti.fieldBitWidth(bf, 0), 3u);
    EXPECT_FALSE(ti.fieldBitWidth(bf, 1).has_value());   // ordinary field
    // All-ordinary widths ⇒ same TypeId as the 2-arg overload (empty scalars).
    std::array<std::int64_t, 2> const none{kNotBitfield, kNotBitfield};
    EXPECT_EQ(ti.structType("T", fields, none).v, ti.structType("T", fields).v);
}

// ── D-CSUBSET-BITFIELD-ABI-EXACT: per-ABI byte-exact bit-field layout ──────────
//
// The conformance witness (hermetic half). `computeLayout` under EACH strategy is
// pinned to the values DERIVED FROM the native compiler — `cl.exe` 14.51 for
// MsvcStraddle, `gcc` 11.4 for GnuPacked (the exact sizeof + set-one-field byte
// probe measured during this cycle; see aggregate_layout.hpp). The two strategies
// DIVERGE on the same struct, so flipping the strategy makes these goldens go RED
// (red-on-disable for the WHOLE per-ABI feature). The CI cross-compile-compare
// step (examples runner / a gated tool) re-derives the goldens from the native
// compiler where present, so the constants below can never silently drift from the
// real ABI.

// The MSVC x64 (PE) params: identical to kGnu16 but with the MS straddling rule.
constexpr AggregateLayoutParams kMsvc16{
    ScalarAlignmentRule::Natural, 16, BitFieldStrategy::MsvcStraddle};

// Struct A = `{int a:1; char b:1;}`. The headline divergence:
//   gcc  : sizeof 4 — b packs into a's int unit at bit 1.
//   cl.exe: sizeof 8 — b is a `char` (size 1 ≠ int 4) → a NEW unit at byte 4.
TEST(TypeLayout, BitFieldAbiExact_A_IntThenChar) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::I32), ti.primitive(TypeKind::Char)};
    std::array<std::int64_t, 2> const widths{1, 1};
    TypeId const s = ti.structType("A", fields, widths);

    // gnu_packed (gcc golden): a@byte0 bit0, b@byte0 bit1, size 4.
    auto const g = layoutOf(s, ti, kGnu16);
    EXPECT_EQ(g.size, 4u);
    EXPECT_EQ(g.fieldOffsets[0], 0u);
    EXPECT_EQ(g.bitFields[0].bitOffset, 0u);
    EXPECT_EQ(g.fieldOffsets[1], 0u);
    EXPECT_EQ(g.bitFields[1].bitOffset, 1u);

    // msvc_straddle (cl.exe golden): a@byte0 bit0, b@byte4 bit0, size 8, align 4.
    auto const m = layoutOf(s, ti, kMsvc16);
    EXPECT_EQ(m.size, 8u);
    EXPECT_EQ(m.align.bytes(), 4u);
    EXPECT_EQ(m.fieldOffsets[0], 0u);
    EXPECT_EQ(m.bitFields[0].bitOffset, 0u);
    EXPECT_EQ(m.bitFields[0].unitBytes, 4u);   // a's unit is an int
    EXPECT_EQ(m.fieldOffsets[1], 4u);          // b starts a NEW unit at byte 4
    EXPECT_EQ(m.bitFields[1].bitOffset, 0u);
    EXPECT_EQ(m.bitFields[1].unitBytes, 1u);   // b's unit is a char
}

// Struct B = `{char a:7; int b:25;}`.
//   gcc  : sizeof 4 — a bits 0..6, b bits 7..31 (one int unit).
//   cl.exe: sizeof 8 — a in a char unit at byte0, b (int ≠ char) → byte 4.
TEST(TypeLayout, BitFieldAbiExact_B_CharThenInt) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::Char), ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 2> const widths{7, 25};
    TypeId const s = ti.structType("B", fields, widths);

    auto const g = layoutOf(s, ti, kGnu16);
    EXPECT_EQ(g.size, 4u);
    EXPECT_EQ(g.fieldOffsets[0], 0u);
    EXPECT_EQ(g.bitFields[0].bitOffset, 0u);
    EXPECT_EQ(g.fieldOffsets[1], 0u);          // same unit
    EXPECT_EQ(g.bitFields[1].bitOffset, 7u);   // b right after a

    auto const m = layoutOf(s, ti, kMsvc16);
    EXPECT_EQ(m.size, 8u);
    EXPECT_EQ(m.fieldOffsets[0], 0u);
    EXPECT_EQ(m.bitFields[0].unitBytes, 1u);   // char unit
    EXPECT_EQ(m.fieldOffsets[1], 4u);          // int unit at byte 4
    EXPECT_EQ(m.bitFields[1].bitOffset, 0u);
    EXPECT_EQ(m.bitFields[1].unitBytes, 4u);
}

// Struct F = `{char a:1; int b:1;}`.
//   gcc  : sizeof 4 — a bit0, b bit1 (a's natural unit window holds both).
//   cl.exe: sizeof 8 — b (int ≠ char) starts a fresh unit at byte 4.
TEST(TypeLayout, BitFieldAbiExact_F_CharThenIntBoth1) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::Char), ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 2> const widths{1, 1};
    TypeId const s = ti.structType("F", fields, widths);

    auto const g = layoutOf(s, ti, kGnu16);
    EXPECT_EQ(g.size, 4u);
    EXPECT_EQ(g.bitFields[1].bitOffset, 1u);

    auto const m = layoutOf(s, ti, kMsvc16);
    EXPECT_EQ(m.size, 8u);
    EXPECT_EQ(m.fieldOffsets[1], 4u);
    EXPECT_EQ(m.bitFields[1].bitOffset, 0u);
}

// Same-type CONTROL `{int a:3; int b:5;}` — both ABIs AGREE (sizeof 4, packed).
// This proves msvc_straddle does NOT gratuitously split SAME-typed bit-fields:
// the divergence is specifically about a declared-type SIZE change.
TEST(TypeLayout, BitFieldAbiExact_SameTypeControlAgrees) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::I32), ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 2> const widths{3, 5};
    TypeId const s = ti.structType("C", fields, widths);
    for (auto const p : {kGnu16, kMsvc16}) {
        auto const l = layoutOf(s, ti, p);
        EXPECT_EQ(l.size, 4u);
        EXPECT_EQ(l.fieldOffsets[0], 0u);
        EXPECT_EQ(l.fieldOffsets[1], 0u);
        EXPECT_EQ(l.bitFields[0].bitOffset, 0u);
        EXPECT_EQ(l.bitFields[1].bitOffset, 3u);
    }
}

// MSVC: two SAME-typed bit-fields after a type transition SHARE the new unit
// (`{int a:1; char b:1; char c:1;}` → b+c in one char unit at byte4, size 8).
// gcc packs all three into the int unit (size 4). Proves the MS rule reuses a
// unit when the type size matches, not blindly one-unit-per-field.
TEST(TypeLayout, BitFieldAbiExact_MsvcReusesUnitForSameType) {
    auto ti = makeInterner(1);
    std::array<TypeId, 3> const fields{
        ti.primitive(TypeKind::I32), ti.primitive(TypeKind::Char),
        ti.primitive(TypeKind::Char)};
    std::array<std::int64_t, 3> const widths{1, 1, 1};
    TypeId const s = ti.structType("O", fields, widths);

    auto const g = layoutOf(s, ti, kGnu16);
    EXPECT_EQ(g.size, 4u);

    auto const m = layoutOf(s, ti, kMsvc16);
    EXPECT_EQ(m.size, 8u);
    EXPECT_EQ(m.fieldOffsets[1], 4u);          // b: new char unit at byte4
    EXPECT_EQ(m.bitFields[1].bitOffset, 0u);
    EXPECT_EQ(m.fieldOffsets[2], 4u);          // c: SAME char unit
    EXPECT_EQ(m.bitFields[2].bitOffset, 1u);
}

// MSVC: an ordinary field between/around bit-fields forces the bit-field to its
// own type-aligned unit (`{char x; int a:3;}` → a@byte4, size 8) — gcc packs a
// into the int window overlapping x (a@byte1, size 4). Proves the ordinary-field
// boundary participates in the MS rule, not only adjacent-bit-field type changes.
TEST(TypeLayout, BitFieldAbiExact_MsvcOrdinaryFieldForcesUnit) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::Char), ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 2> const widths{kNotBitfield, 3};
    TypeId const s = ti.structType("J", fields, widths);

    auto const g = layoutOf(s, ti, kGnu16);
    EXPECT_EQ(g.size, 4u);
    EXPECT_EQ(g.fieldOffsets[0], 0u);          // x@byte0
    EXPECT_EQ(g.fieldOffsets[1], 0u);          // a's int unit at byte0 (overlaps x window)
    EXPECT_EQ(g.bitFields[1].bitOffset, 8u);   // a at bit8 = byte1

    auto const m = layoutOf(s, ti, kMsvc16);
    EXPECT_EQ(m.size, 8u);
    EXPECT_EQ(m.fieldOffsets[0], 0u);          // x@byte0
    EXPECT_EQ(m.fieldOffsets[1], 4u);          // a's int unit at byte4
    EXPECT_EQ(m.bitFields[1].bitOffset, 0u);
}

// MSVC sizes the LAST unit to its FULL declared-type width even with no other
// field forcing alignment: `{int a:1;}` → 4, `{char a:1; char b:1;}` → 1. Both
// ABIs agree here (these have no type transition), but it pins the MS sizing rule
// so a regression to "bits-used rounding" for a lone wide unit is caught.
TEST(TypeLayout, BitFieldAbiExact_MsvcSizesLastUnitToTypeWidth) {
    auto ti = makeInterner(1);
    {   // single int:1 → size 4 under both
        std::array<TypeId, 1> const fields{ti.primitive(TypeKind::I32)};
        std::array<std::int64_t, 1> const widths{1};
        TypeId const s = ti.structType("M", fields, widths);
        EXPECT_EQ(layoutOf(s, ti, kMsvc16).size, 4u);
        EXPECT_EQ(layoutOf(s, ti, kGnu16).size, 4u);
    }
    {   // two char:1 → size 1 under both
        std::array<TypeId, 2> const fields{
            ti.primitive(TypeKind::Char), ti.primitive(TypeKind::Char)};
        std::array<std::int64_t, 2> const widths{1, 1};
        TypeId const s = ti.structType("N", fields, widths);
        EXPECT_EQ(layoutOf(s, ti, kMsvc16).size, 1u);
        EXPECT_EQ(layoutOf(s, ti, kGnu16).size, 1u);
    }
    {   // char,int,char → MSVC never reopens the byte0 char unit: c@byte8, size 12
        std::array<TypeId, 3> const fields{
            ti.primitive(TypeKind::Char), ti.primitive(TypeKind::I32),
            ti.primitive(TypeKind::Char)};
        std::array<std::int64_t, 3> const widths{1, 1, 1};
        TypeId const s = ti.structType("P", fields, widths);
        auto const m = layoutOf(s, ti, kMsvc16);
        EXPECT_EQ(m.size, 12u);
        EXPECT_EQ(m.fieldOffsets[0], 0u);
        EXPECT_EQ(m.fieldOffsets[1], 4u);
        EXPECT_EQ(m.fieldOffsets[2], 8u);
        EXPECT_EQ(layoutOf(s, ti, kGnu16).size, 4u);  // gcc packs all into one int unit
    }
}

// A union bit-field member is placed IDENTICALLY under both realized strategies
// (a lone member never straddles / has a type-transition neighbour), and BOTH
// must still reject an undeclared strategy (None) — the fail-loud gate.
TEST(TypeLayout, BitFieldAbiExact_UnionAgreesAndFailsLoudOnNone) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::Char), ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 2> const widths{7, 25};
    TypeId const u = ti.unionType("U", fields, widths);
    for (auto const p : {kGnu16, kMsvc16}) {
        auto const l = layoutOf(u, ti, p);
        EXPECT_EQ(l.fieldOffsets[0], 0u);
        EXPECT_EQ(l.fieldOffsets[1], 0u);
        EXPECT_EQ(l.bitFields[0].bitOffset, 0u);
        EXPECT_EQ(l.bitFields[1].bitOffset, 0u);
        EXPECT_EQ(l.size, 4u);   // max(char-unit 1, int-unit 4), aligned 4
    }
    // None (kNatural16) → fail loud.
    EXPECT_FALSE(
        computeLayout(u, ti, kNatural16, DataModel::Lp64).has_value());
}

// Explicit RED-ON-DISABLE marker for the per-ABI feature: the SAME struct under
// the two strategies must produce DIFFERENT sizes. If a regression makes
// MsvcStraddle fall back to the gnu_packed loop (or vice versa), this fails.
TEST(TypeLayout, BitFieldAbiExact_StrategiesDivergeRedOnDisable) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::I32), ti.primitive(TypeKind::Char)};
    std::array<std::int64_t, 2> const widths{1, 1};
    TypeId const s = ti.structType("A", fields, widths);
    auto const g = layoutOf(s, ti, kGnu16);
    auto const m = layoutOf(s, ti, kMsvc16);
    EXPECT_NE(g.size, m.size) << "gnu_packed and msvc_straddle MUST diverge on "
                                 "{int a:1; char b:1;} (4 vs 8)";
    EXPECT_EQ(g.size, 4u);
    EXPECT_EQ(m.size, 8u);
}

// ── D-CSUBSET-SELF-REFERENTIAL-STRUCT: incomplete composites + self-ref ──────

TEST(TypeLayout, IncompleteCompositeHasNoLayout) {
    // An INCOMPLETE composite (forward-minted, never completed) has NO size —
    // `computeLayout` fails loud (nullopt), the signal a sizeof of it / a by-value
    // member of it surfaces as a diagnostic. RED-ON-DISABLE: drop the
    // isIncompleteComposite guard and this returns a (wrong, zero) layout.
    auto ti = makeInterner(1);
    const TypeId fwd = ti.forwardComposite(TypeKind::Struct, "Opaque", 1);
    EXPECT_FALSE(computeLayout(fwd, ti, kNatural16, DataModel::Lp64).has_value());
}

TEST(TypeLayout, CompleteEmptyStructLaysOutSizeZero) {
    // A COMPLETE empty struct (`struct E {}`) is a LEGAL zero-field type with size
    // 0 — it must still lay out (NOT trip the incomplete guard, which keys on the
    // EXPLICIT incomplete flag, never "operands empty").
    auto ti = makeInterner(1);
    const TypeId e = ti.forwardComposite(TypeKind::Struct, "E", 2);
    ti.completeComposite(e, {});
    auto const l = computeLayout(e, ti, kNatural16, DataModel::Lp64);
    ASSERT_TRUE(l.has_value());
    EXPECT_EQ(l->size, 0u);
    EXPECT_TRUE(l->fieldOffsets.empty());
}

TEST(TypeLayout, SelfReferentialStructLaysOutWithPointerField) {
    // `struct Node { int value; struct Node *next; }` on LP64: value@0 (4 bytes),
    // next@8 (8-byte pointer, 8-aligned) → size 16, align 8. The self-ref field is
    // a pointer (pointer size is independent of the pointee's completeness), so the
    // layout is well-defined even though `next` points back at Node.
    auto ti = makeInterner(1);
    const TypeId i32  = ti.primitive(TypeKind::I32);
    const TypeId node = ti.forwardComposite(TypeKind::Struct, "Node", 3);
    const TypeId ptrNode = ti.pointer(node);
    std::array<TypeId, 2> const fields{i32, ptrNode};
    ti.completeComposite(node, fields);
    auto const l = layoutOf(node, ti);
    EXPECT_EQ(l.size, 16u);
    EXPECT_EQ(l.align.bytes(), 8u);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);   // value
    EXPECT_EQ(l.fieldOffsets[1], 8u);   // next (pointer, 8-aligned)
}
