// FC6: the struct/union/array LAYOUT engine — field offsets, alignment, padding,
// total size, flexible-array-member handling. Multi-form contract: every aggregate
// form is laid out by building its TypeId directly via the interner (incl. forms no
// shipped C program reaches yet — FAM, i128, deeply nested). The engine is target-
// AGNOSTIC: it runs ONE bounded natural-alignment algorithm parameterized by the
// declared params, proven by the agnosticism pin (different params → different layout).

#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"   // D-CSUBSET-COMPLEX: regClassForCoreType pin
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

// Bit-field params: identical to kNatural16 but with a REALIZED packing strategy
// (so only a genuine fail-loud condition — e.g. the c107 offsets+bitfields guard —
// can reject). Hoisted here so both the member-alignas-on-bitfield tests (#1) and
// the FC8 bit-field ABI tests below can reference them.
constexpr AggregateLayoutParams kGnu16{
    ScalarAlignmentRule::Natural, 16, BitFieldStrategy::GnuPacked};
constexpr AggregateLayoutParams kMsvc16{
    ScalarAlignmentRule::Natural, 16, BitFieldStrategy::MsvcStraddle};

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
             // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): x87 80-bit STORES 16/16 —
             // x86_64-SysV pads the 10 significant bytes to a 16-byte,
             // 16-aligned slot (the same envelope binary128 uses).
             Case{TypeKind::F80, 16, 16},
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

// c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): a struct with EXPLICIT per-field byte
// offsets lays its members at those offsets — which may OVERLAP — instead of
// deriving them by natural alignment. ULARGE_INTEGER {QuadPart u64@0, LowPart
// u32@0, HighPart u32@4}: size is the MAX field EXTENT (8), align the max field
// align (8), and the members share bytes. RED-ON-DISABLE: were offsets ignored,
// the derive path would place them at 0/8/12 → size 16.
TEST(TypeLayout, ExplicitOffsetsOverlapAndSizeIsMaxExtent) {
    auto ti = makeInterner(1);
    TypeId const u64 = ti.primitive(TypeKind::U64);
    TypeId const u32 = ti.primitive(TypeKind::U32);
    std::array<TypeId, 3>        const fields{u64, u32, u32};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 3> const offsets{0, 0, 4};
    TypeId const ov = ti.structType("ULARGE", fields, noWidths, offsets);
    EXPECT_TRUE(ti.hasExplicitOffsets(ov));
    auto const l = layoutOf(ov, ti);
    ASSERT_EQ(l.fieldOffsets.size(), 3u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);   // QuadPart
    EXPECT_EQ(l.fieldOffsets[1], 0u);   // LowPart  overlays QuadPart low
    EXPECT_EQ(l.fieldOffsets[2], 4u);   // HighPart overlays QuadPart high
    EXPECT_EQ(l.size, 8u);              // max extent (4 + 4), NOT 16
    EXPECT_EQ(l.align.bytes(), 8u);     // max field align (u64)

    // The same field-types laid out NATURALLY are a distinct type + a distinct
    // (non-overlapping) layout — the identity fork that keeps an FFI overlap from
    // ever aliasing an ordinary struct.
    TypeId const nat = ti.structType("ULARGE", fields);
    EXPECT_NE(ov, nat);
    EXPECT_FALSE(ti.hasExplicitOffsets(nat));
    EXPECT_EQ(layoutOf(nat, ti).size, 16u);
}

// c107: bit-fields and explicit offsets are mutually exclusive (the offsets ride a
// SEPARATE channel from the bit-width scalars). A struct that somehow carried BOTH
// must FAIL LOUD at layout (nullopt), never silently mis-place — the layout arm's
// `!scalars(id).empty()` guard on the explicit-offset path.
TEST(TypeLayout, ExplicitOffsetsWithBitfieldsFailsLoud) {
    auto ti = makeInterner(1);
    TypeId const u32 = ti.primitive(TypeKind::U32);
    std::array<TypeId, 2>        const fields{u32, u32};
    std::array<std::int64_t, 2>  const widths{4, kNotBitfield};   // a real bit-field
    std::array<std::uint64_t, 2> const offsets{0, 0};
    TypeId const bad = ti.structType("BAD", fields, widths, offsets);
    AggregateLayoutParams p{ScalarAlignmentRule::Natural, 16};
    p.bitFieldStrategy = BitFieldStrategy::GnuPacked;   // realized, so only the c107 guard can reject
    EXPECT_FALSE(computeLayout(bad, ti, p, DataModel::Lp64).has_value());
}

// D-CSUBSET-MEMBER-ALIGNAS: a member `alignas(16)` RAISES the field's (and thus the
// struct's) alignment, padding the struct up to 16. `struct{alignas(16) int x;}`:
// x@0 (int align raised to 16), struct align 16, size rounded up to 16.
// RED-ON-DISABLE: were the override ignored, align stays 4 and size is 4.
TEST(TypeLayout, MemberAlignasRaisesStructAlignAndSize) {
    auto ti = makeInterner(1);
    TypeId const i32 = ti.primitive(TypeKind::I32);
    std::array<TypeId, 1>        const fields{i32};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 1> const aligns{16};
    TypeId const s = ti.structType("S", fields, noWidths, noOffs, aligns);
    EXPECT_TRUE(ti.hasExplicitAligns(s));
    auto const l = layoutOf(s, ti);
    ASSERT_EQ(l.fieldOffsets.size(), 1u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);   // the int still starts at 0
    EXPECT_EQ(l.align.bytes(), 16u);    // raised from natural 4 → 16
    EXPECT_EQ(l.size, 16u);             // padded up to the 16-byte alignment

    // The same field with NO override is the ordinary 4-byte int struct.
    TypeId const nat = ti.structType("S", fields);
    EXPECT_FALSE(ti.hasExplicitAligns(nat));
    auto const ln = layoutOf(nat, ti);
    EXPECT_EQ(ln.align.bytes(), 4u);
    EXPECT_EQ(ln.size, 4u);
}

// D-CSUBSET-MEMBER-ALIGNAS: the override uses MAX semantics — it can only RAISE, never
// LOWER. `alignas(2)` on an `int` (natural align 4) is a no-op: the effective align
// stays 4. RED-ON-DISABLE: if the override replaced (rather than max'd) the natural
// align, the struct would mis-align to 2 and mis-size.
TEST(TypeLayout, MemberAlignasNeverLowersBelowNatural) {
    auto ti = makeInterner(1);
    TypeId const i32 = ti.primitive(TypeKind::I32);
    std::array<TypeId, 1>        const fields{i32};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 1> const aligns{2};   // BELOW the int's natural 4
    TypeId const s = ti.structType("S", fields, noWidths, noOffs, aligns);
    auto const l = layoutOf(s, ti);
    EXPECT_EQ(l.align.bytes(), 4u);     // natural 4 wins over the 2 override
    EXPECT_EQ(l.size, 4u);
}

// D-CSUBSET-MEMBER-ALIGNAS: a member alignas on a LATER field raises BOTH the struct's
// alignment AND that field's start boundary. `struct{int i; alignas(16) int j;}`:
// i@0, j forced to the next 16-aligned offset → j@16 (not the natural 4), struct align
// 16, size 20→32. RED-ON-DISABLE: were the override ignored, j@4, align 4, size 8.
TEST(TypeLayout, MemberAlignasRaisesFollowingFieldOffset) {
    auto ti = makeInterner(1);
    TypeId const i32 = ti.primitive(TypeKind::I32);
    std::array<TypeId, 2>        const fields{i32, i32};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 2> const aligns{0, 16};   // alignas(16) on the 2nd int
    TypeId const s = ti.structType("S", fields, noWidths, noOffs, aligns);
    auto const l = layoutOf(s, ti);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);    // i@0
    EXPECT_EQ(l.fieldOffsets[1], 16u);   // j forced to the 16-byte boundary (not 4)
    EXPECT_EQ(l.align.bytes(), 16u);     // struct align raised to 16
    EXPECT_EQ(l.size, 32u);              // 20 rounded up to align 16

    // The same two ints with NO override pack naturally: j@4, align 4, size 8.
    TypeId const nat = ti.structType("S", fields);
    auto const ln = layoutOf(nat, ti);
    EXPECT_EQ(ln.fieldOffsets[1], 4u);
    EXPECT_EQ(ln.align.bytes(), 4u);
    EXPECT_EQ(ln.size, 8u);
}

// D-CSUBSET-ALIGNAS: a UNION member alignas raises the union's alignment (and thus
// its rounded size) — the computeLayout UNION arm folds `explicitFieldAlign` exactly
// like the struct arm. `union{ alignas(16) char c; int i; }`: every member at
// offset 0, natural align max(1,4)=4, but c's alignas(16) raises the union to align
// 16 → size max(1,4)=4 rounded up to 16. RED-ON-DISABLE (the union-arm fold): were
// the override ignored, the union would align to 4 and size to 4. The union is built
// via forwardComposite + completeComposite (the semantic analyzer's path — there is
// no complete-at-once `unionType` overload carrying aligns).
TEST(TypeLayout, UnionMemberAlignasRaisesAlignAndSize) {
    auto ti = makeInterner(1);
    TypeId const c8  = ti.primitive(TypeKind::Char);
    TypeId const i32 = ti.primitive(TypeKind::I32);
    std::array<TypeId, 2>        const members{c8, i32};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 2> const aligns{16, 0};   // alignas(16) on the char
    TypeId const u = ti.forwardComposite(TypeKind::Union, "U", /*declSiteKey=*/1);
    ti.completeComposite(u, members, /*packed=*/false, noWidths, noOffs, aligns);
    EXPECT_TRUE(ti.hasExplicitAligns(u));
    auto const l = layoutOf(u, ti);
    EXPECT_EQ(l.align.bytes(), 16u);   // raised from natural 4 → 16 (the char's alignas)
    EXPECT_EQ(l.size, 16u);            // max member extent 4, rounded up to align 16

    // The same union with NO override: align 4 (the int), size 4.
    TypeId const nat = ti.forwardComposite(TypeKind::Union, "U", /*declSiteKey=*/2);
    ti.completeComposite(nat, members, /*packed=*/false);
    EXPECT_FALSE(ti.hasExplicitAligns(nat));
    auto const ln = layoutOf(nat, ti);
    EXPECT_EQ(ln.align.bytes(), 4u);
    EXPECT_EQ(ln.size, 4u);
}

// ── #1: member `alignas` on an ORDINARY field of a BIT-FIELD-bearing struct ──
// C11/C23 6.7.5: `alignas` on a non-bit-field member is LEGAL even when the
// struct also has a bit-field, and must be HONORED. The bit-field packers' own
// ordinary-field arms previously used the bare natural alignment (dropping the
// override) — this is the cross-seam bug: `anyBitfield` routes AWAY from the
// non-bitfield `effectiveAlign` path into the packer, whose ordinary arm ignored
// the override. RED-ON-DISABLE: revert `bitfieldPackerEffectiveAlign` and align
// falls back to 4 (and size to 8), so the align==16 assertion fails.
TEST(TypeLayout, BitFieldStructMemberAlignasHonoredGnu) {
    auto ti = makeInterner(1);
    // struct S { alignas(16) int a; unsigned b : 3; };
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::I32), ti.primitive(TypeKind::U32)};
    std::array<std::int64_t, 2>  const widths{-1 /*kNotBitfield*/, 3};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 2> const aligns{16, 0};   // alignas(16) on `a` only
    TypeId const s = ti.structType("S", fields, widths, noOffs, aligns);
    EXPECT_TRUE(ti.hasExplicitAligns(s));
    auto const l = layoutOf(s, ti, kGnu16);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);    // a@0
    EXPECT_EQ(l.fieldOffsets[1], 4u);    // b's unit at byte 4 (past a's 4 bytes)
    EXPECT_EQ(l.bitFields[1].bitOffset, 0u);
    EXPECT_EQ(l.bitFields[1].bitWidth, 3u);
    EXPECT_EQ(l.align.bytes(), 16u);     // a's alignas(16) RAISES the struct align
    EXPECT_EQ(l.size, 16u);              // 5 bytes rounded up to align 16

    // Same struct WITHOUT the override: align 4, size 8 (b's unit at byte 4).
    TypeId const nat = ti.structType("S", fields, widths);
    EXPECT_FALSE(ti.hasExplicitAligns(nat));
    auto const ln = layoutOf(nat, ti, kGnu16);
    EXPECT_EQ(ln.align.bytes(), 4u);
    EXPECT_EQ(ln.size, 8u);
}

// #1, MsvcStraddle strategy: the SAME struct through the other packer's ordinary
// arm. RED-ON-DISABLE identically (the MsvcStraddle ordinary arm also folds the
// override now). cl.exe lays `struct S { alignas(16) int a; unsigned b:3; }` at
// align 16 / size 16 / a@0 / b's unit @4.
TEST(TypeLayout, BitFieldStructMemberAlignasHonoredMsvc) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::I32), ti.primitive(TypeKind::U32)};
    std::array<std::int64_t, 2>  const widths{-1, 3};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 2> const aligns{16, 0};
    TypeId const s = ti.structType("S", fields, widths, noOffs, aligns);
    auto const l = layoutOf(s, ti, kMsvc16);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[1], 4u);
    EXPECT_EQ(l.align.bytes(), 16u);
    EXPECT_EQ(l.size, 16u);
}

// #1, LATER over-aligned ordinary field after a bit-field: the alignas forces the
// field's own start boundary past the bit-unit. `struct{ int b:3; alignas(16) int
// a; }` → b's unit @0, a forced to @16, align 16, size 32. RED-ON-DISABLE: a lands
// at @4 (byte after the unit), align 4, size 8.
TEST(TypeLayout, BitFieldThenAlignasOrdinaryFieldGnu) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{
        ti.primitive(TypeKind::U32), ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 2>  const widths{3, -1};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 2> const aligns{0, 16};   // alignas(16) on `a` (2nd)
    TypeId const s = ti.structType("S", fields, widths, noOffs, aligns);
    auto const l = layoutOf(s, ti, kGnu16);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);    // b's unit @0
    EXPECT_EQ(l.fieldOffsets[1], 16u);   // a forced to the 16-byte boundary
    EXPECT_EQ(l.align.bytes(), 16u);
    EXPECT_EQ(l.size, 32u);              // 20 rounded up to align 16
}

// #1, UNION variant — an ordinary member alignas coexisting with a bit-field
// member. `union { alignas(16) char c; int i : 3; }`: every member at offset 0,
// c's alignas(16) raises the union to align 16, i is a 3-bit field of its own
// 4-byte unit. align 16 / size 16. RED-ON-DISABLE (the union arm already folded
// the override, so this pins the bit-field+alignas COMBINATION does not regress
// the union arm): without the fold, align 4 / size 4. Built via forward/complete
// (there is no complete-at-once unionType overload carrying aligns).
TEST(TypeLayout, UnionBitFieldMemberAlignasHonored) {
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const members{
        ti.primitive(TypeKind::Char), ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 2>  const widths{-1, 3};   // i is a 3-bit field
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 2> const aligns{16, 0};   // alignas(16) on the char
    TypeId const u = ti.forwardComposite(TypeKind::Union, "U", /*declSiteKey=*/1);
    ti.completeComposite(u, members, /*packed=*/false, widths, noOffs, aligns);
    auto const l = layoutOf(u, ti, kGnu16);
    EXPECT_EQ(l.align.bytes(), 16u);   // the char's alignas raises the union
    EXPECT_EQ(l.size, 16u);            // max member extent 4, rounded up to 16
    ASSERT_EQ(l.bitFields.size(), 2u);
    EXPECT_EQ(l.bitFields[1].bitWidth, 3u);
}

// #1, FLEXIBLE-ARRAY-MEMBER variant — an `alignas` on the FAM of a bit-field-
// bearing struct. The packer FAM arm also folds the override now. `struct{ int
// b:3; alignas(16) int fam[]; }`: b's unit @0, fam forced to @16 (its element
// align raised from 4 to 16), align 16, FAM contributes 0 to size → size 16.
// RED-ON-DISABLE: fam lands at @4, align 4, size 4.
TEST(TypeLayout, BitFieldStructFlexibleArrayMemberAlignasHonoredGnu) {
    auto ti = makeInterner(1);
    TypeId const i32 = ti.primitive(TypeKind::I32);
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::U32),
                                       ti.incompleteArray(i32)};
    std::array<std::int64_t, 2>  const widths{3, -1};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 2> const aligns{0, 16};   // alignas(16) on the FAM
    TypeId const s = ti.structType("S", fields, widths, noOffs, aligns);
    auto const l = layoutOf(s, ti, kGnu16);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);    // b's unit @0
    EXPECT_EQ(l.fieldOffsets[1], 16u);   // fam forced to the 16-byte boundary
    EXPECT_TRUE(l.hasFlexibleArrayMember);
    EXPECT_EQ(l.align.bytes(), 16u);
    EXPECT_EQ(l.size, 16u);              // FAM adds 0; 2 (b's byte) padded to 16
}

// ── D-CSUBSET-PACKED: `__attribute__((packed))` layout ──────────────────────
// A packed struct/union removes ALL inter-field padding and sets the aggregate's
// natural alignment to 1. Built directly via forwardComposite + completeComposite
// with packed=true (the semantic analyzer's path; there is no complete-at-once
// structType overload carrying packed). Each is RED-ON-DISABLE: revert the layout
// packed baseline and the size/offset assertions revert to the padded values.

[[nodiscard]] TypeId packedStruct(TypeInterner& ti, std::string_view name,
                                  std::uint64_t key, std::span<TypeId const> fields) {
    TypeId const s = ti.forwardComposite(TypeKind::Struct, name, key);
    ti.completeComposite(s, fields, /*packed=*/true);
    return s;
}

TEST(TypeLayout, PackedStructRemovesAllPadding) {
    auto ti = makeInterner(1);
    // struct S { char c; uint32_t v; } __attribute__((packed));
    // Unpacked: c@0, pad[1..3], v@4 → size 8, align 4.
    // Packed:   c@0, v@1 (no padding) → size 5, align 1.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::U32)};
    TypeId const s = packedStruct(ti, "S", 1, fields);
    EXPECT_TRUE(ti.isPacked(s));
    auto const l = layoutOf(s, ti);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);   // c@0
    EXPECT_EQ(l.fieldOffsets[1], 1u);   // v@1 (NOT @4 — no padding)  RED-ON-DISABLE
    EXPECT_EQ(l.align.bytes(), 1u);     // natural alignment 1        RED-ON-DISABLE
    EXPECT_EQ(l.size, 5u);              // 1 + 4, no tail padding     RED-ON-DISABLE

    // The SAME fields UNPACKED are a distinct type with the padded layout.
    std::array<TypeId, 2> const fields2{ti.primitive(TypeKind::Char),
                                        ti.primitive(TypeKind::U32)};
    TypeId const nat = ti.structType("S", fields2);
    EXPECT_FALSE(ti.isPacked(nat));
    EXPECT_NE(s.v, nat.v);              // packed enters the content identity
    auto const ln = layoutOf(nat, ti);
    EXPECT_EQ(ln.fieldOffsets[1], 4u);
    EXPECT_EQ(ln.align.bytes(), 4u);
    EXPECT_EQ(ln.size, 8u);
}

TEST(TypeLayout, PackedStructAlignasMemberStillRaisesPerField) {
    auto ti = makeInterner(1);
    // struct S { char c; alignas(4) int v; } __attribute__((packed));
    // packed baseline is 1, but the member alignas(4) RAISES v to 4-aligned:
    // c@0, v@4 (alignas wins per-field even under packed), struct align 4, size 8.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 2> const aligns{0, 4};   // alignas(4) on v
    TypeId const s = ti.forwardComposite(TypeKind::Struct, "S", 1);
    ti.completeComposite(s, fields, /*packed=*/true, noWidths, noOffs, aligns);
    EXPECT_TRUE(ti.isPacked(s));
    EXPECT_TRUE(ti.hasExplicitAligns(s));
    auto const l = layoutOf(s, ti);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[1], 4u);   // alignas(4) RAISES despite packed  RED-ON-DISABLE
    EXPECT_EQ(l.align.bytes(), 4u);     // alignas(4) raises the struct align too
    EXPECT_EQ(l.size, 8u);              // 4 + 4

    // Without the alignas, the SAME packed struct packs v@1 → size 5.
    std::array<TypeId, 2> const fields2{ti.primitive(TypeKind::Char),
                                        ti.primitive(TypeKind::I32)};
    TypeId const noAl = packedStruct(ti, "S", 2, fields2);
    auto const ln = layoutOf(noAl, ti);
    EXPECT_EQ(ln.fieldOffsets[1], 1u);
    EXPECT_EQ(ln.size, 5u);
}

TEST(TypeLayout, PackedUnionHasAlignmentOne) {
    auto ti = makeInterner(1);
    // union U { char c; int i; } __attribute__((packed));
    // Members at offset 0; packed → union alignment 1 (unpacked would be 4).
    // size = max member size (4), rounded up to align 1 = 4.
    std::array<TypeId, 2> const members{ti.primitive(TypeKind::Char),
                                        ti.primitive(TypeKind::I32)};
    TypeId const u = ti.forwardComposite(TypeKind::Union, "U", 1);
    ti.completeComposite(u, members, /*packed=*/true);
    EXPECT_TRUE(ti.isPacked(u));
    auto const l = layoutOf(u, ti);
    EXPECT_EQ(l.align.bytes(), 1u);   // packed → alignment 1  RED-ON-DISABLE
    EXPECT_EQ(l.size, 4u);            // max member extent (int)

    // Unpacked, the union aligns to 4 (the int).
    std::array<TypeId, 2> const members2{ti.primitive(TypeKind::Char),
                                         ti.primitive(TypeKind::I32)};
    TypeId const nat = ti.unionType("U", members2);
    EXPECT_EQ(layoutOf(nat, ti).align.bytes(), 4u);
}

TEST(TypeLayout, PackedStructAsArrayElementStride) {
    auto ti = makeInterner(1);
    // A packed {char; uint32_t} has size 5, align 1. An array of 2 → stride 5,
    // size 10 (a padded element would be size 8 → array size 16).
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::U32)};
    TypeId const s   = packedStruct(ti, "S", 1, fields);
    TypeId const arr = ti.array(s, 2);
    auto const l = layoutOf(arr, ti);
    EXPECT_EQ(l.align.bytes(), 1u);
    EXPECT_EQ(l.size, 10u);           // 2 * stride(5)  RED-ON-DISABLE (padded → 16)
}

TEST(TypeLayout, NestedPackedStructPacksInnerToOffsetOne) {
    auto ti = makeInterner(1);
    // struct Inner { char c; uint32_t v; } __attribute__((packed));  // size 5, align 1
    // struct Outer { char a; struct Inner inner; } __attribute__((packed));
    // Outer: a@0, inner@1 (packed baseline 1, inner align 1) → size 6, align 1.
    std::array<TypeId, 2> const innerFields{ti.primitive(TypeKind::Char),
                                            ti.primitive(TypeKind::U32)};
    TypeId const inner = packedStruct(ti, "Inner", 1, innerFields);
    std::array<TypeId, 2> const outerFields{ti.primitive(TypeKind::Char), inner};
    TypeId const outer = packedStruct(ti, "Outer", 2, outerFields);
    auto const l = layoutOf(outer, ti);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[1], 1u);   // inner packed right after `a`  RED-ON-DISABLE
    EXPECT_EQ(l.align.bytes(), 1u);
    EXPECT_EQ(l.size, 6u);              // 1 + 5
}

TEST(TypeLayout, PackedPlusBitfieldFailsLoud) {
    auto ti = makeInterner(1);
    // A packed struct carrying a bit-field is UNSUPPORTED (bit-granular packed
    // packing is a distinct algorithm) — computeLayout returns nullopt (the F5
    // belt), the reliable backstop behind the semantic S_PackedBitfieldUnsupported.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::I32),
                                       ti.primitive(TypeKind::U32)};
    std::array<std::int64_t, 2> const widths{-1 /*kNotBitfield*/, 3};
    TypeId const s = ti.forwardComposite(TypeKind::Struct, "S", 1);
    ti.completeComposite(s, fields, /*packed=*/true, widths);
    EXPECT_TRUE(ti.isPacked(s));
    EXPECT_FALSE(computeLayout(s, ti, kGnu16, DataModel::Lp64).has_value());  // belt
}

// ── D-CSUBSET-COMPOSITE-ALIGNED (TF-C73): the WHOLE-COMPOSITE `aligned(N)` ──────
//
// `struct S {…} __attribute__((aligned(N)))` raises the AGGREGATE's own alignment
// (C 6.7.5: the composite's alignment is the MAX of its natural alignment and the
// request), which also rounds its size up. `computeLayout` realizes this by SEEDING
// `StructLayout::align` from `TypeInterner::explicitCompositeAlign` before the
// per-field folds, all of which are `maxAlign` — so it can only ever RAISE.
//
// Every number below was MEASURED with clang on arm64 macOS (compiled AND run,
// `-fsyntax-only -Wall -Wextra -isysroot $(xcrun --show-sdk-path)` clean).
// Each is RED-ON-DISABLE: remove the `compositeSeedAlign` seed from the Struct or
// Union arm and the assertion reverts to the un-raised natural value.

[[nodiscard]] TypeId alignedComposite(TypeInterner& ti, TypeKind kind,
                                      std::string_view name, std::uint64_t key,
                                      std::span<TypeId const> fields,
                                      std::uint32_t align, bool packed = false,
                                      std::span<std::int64_t const> widths = {}) {
    TypeId const s = ti.forwardComposite(kind, name, key);
    ti.completeComposite(s, fields, packed, widths, /*fieldOffsets=*/{},
                         /*fieldAligns=*/{}, align);
    return s;
}

TEST(TypeLayout, CompositeAlignedRaisesAggregateAlignAndSize) {
    auto ti = makeInterner(1);
    // clang: `struct A { char c; unsigned v; } __attribute__((aligned(16)));`
    //        → size 16, align 16 (natural would be 8 / 4).
    // Field OFFSETS are UNCHANGED — the request touches the aggregate, not the
    // fields: c@0, v@4 exactly as in the unaligned twin.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::U32)};
    TypeId const a = alignedComposite(ti, TypeKind::Struct, "A", 1, fields, 16u);
    EXPECT_EQ(ti.explicitCompositeAlign(a), 16u);
    auto const l = layoutOf(a, ti);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[1], 4u);   // natural placement — NOT pushed by the request
    EXPECT_EQ(l.align.bytes(), 16u);    // RED-ON-DISABLE (natural 4)
    EXPECT_EQ(l.size, 16u);             // RED-ON-DISABLE (natural 8)

    // The same fields with NO request are a distinct type with the natural layout.
    std::array<TypeId, 2> const fields2{ti.primitive(TypeKind::Char),
                                        ti.primitive(TypeKind::U32)};
    TypeId const nat = ti.structType("A", fields2);
    EXPECT_EQ(ti.explicitCompositeAlign(nat), 0u);
    auto const ln = layoutOf(nat, ti);
    EXPECT_EQ(ln.align.bytes(), 4u);
    EXPECT_EQ(ln.size, 8u);
}

// ── ★★ TF-C82 (D-PP-PRAGMA-REGISTRY): the `#pragma pack(N)` MEMBER-ALIGNMENT CAP ──
//
// The EXACT DUAL of the `aligned(N)` block above: that one RAISES the aggregate's
// alignment, this one CLAMPS each member's — so it moves field OFFSETS, which
// `aligned(N)` never does. `computeLayout` realizes it by capping each field's
// baseline alignment before the member-`alignas` MAX-fold.
//
// Every number below was MEASURED with clang on arm64 macOS (compiled AND run).
// Each is RED-ON-DISABLE: remove the cap from the Struct or Union arm and the
// assertion reverts to the uncapped natural value.
[[nodiscard]] TypeId cappedComposite(TypeInterner& ti, TypeKind kind,
                                     std::string_view name, std::uint64_t key,
                                     std::span<TypeId const> fields,
                                     std::uint32_t cap, bool packed = false,
                                     std::uint32_t explicitAlign = 0) {
    TypeId const s = ti.forwardComposite(kind, name, key);
    ti.completeComposite(s, fields, packed, /*fieldBitWidths=*/{},
                         /*fieldOffsets=*/{}, /*fieldAligns=*/{}, explicitAlign,
                         cap);
    return s;
}

TEST(TypeLayout, PragmaPackCapsMemberAlignmentAndMovesOffsets) {
    auto ti = makeInterner(1);
    // THE CORPUS WITNESS, `sys/fcntl.h`'s `struct log2phys` under `#pragma
    // pack(4)`: { unsigned int; long long; long long }.
    // clang MEASURED: offsets 0/4/12, size 20, align 4.
    // Uncapped it is  offsets 0/8/16, size 24, align 8 — the value DSS computed
    // before this cycle, for a struct sqlite hands to `fcntl(F_LOG2PHYS)`.
    std::array<TypeId, 3> const fields{ti.primitive(TypeKind::U32),
                                       ti.primitive(TypeKind::I64),
                                       ti.primitive(TypeKind::I64)};
    TypeId const capped =
        cappedComposite(ti, TypeKind::Struct, "log2phys", 1, fields, 4u);
    EXPECT_EQ(ti.maxFieldAlign(capped), 4u);
    auto const l = layoutOf(capped, ti);
    ASSERT_EQ(l.fieldOffsets.size(), 3u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[1], 4u);   // RED-ON-DISABLE (uncapped 8)
    EXPECT_EQ(l.fieldOffsets[2], 12u);  // RED-ON-DISABLE (uncapped 16)
    EXPECT_EQ(l.align.bytes(), 4u);     // RED-ON-DISABLE (uncapped 8)
    EXPECT_EQ(l.size, 20u);             // RED-ON-DISABLE (uncapped 24)

    // The CONTROL that makes every line above non-vacuous: the SAME fields with
    // no cap. It is also the interning pin — the cap is part of the content
    // identity, so these are two DISTINCT TypeIds and must not collapse.
    std::array<TypeId, 3> const fields2{ti.primitive(TypeKind::U32),
                                        ti.primitive(TypeKind::I64),
                                        ti.primitive(TypeKind::I64)};
    TypeId const nat = ti.structType("log2phys", fields2);
    EXPECT_NE(nat.v, capped.v)
        << "identical fields under different pack caps lay out to different "
           "sizes; collapsing them onto one TypeId is a layout miscompile";
    EXPECT_EQ(ti.maxFieldAlign(nat), 0u);
    auto const ln = layoutOf(nat, ti);
    EXPECT_EQ(ln.fieldOffsets[1], 8u);
    EXPECT_EQ(ln.align.bytes(), 8u);
    EXPECT_EQ(ln.size, 24u);
}

TEST(TypeLayout, PragmaPackCapWeakerThanNaturalIsANoOp) {
    auto ti = makeInterner(1);
    // `#pragma pack(16)` over { char; unsigned } asks for a cap LOOSER than
    // every member's natural alignment — a no-op, exactly like an `aligned(N)`
    // weaker than natural. A cap that ROUNDED UP would be a silent over-align.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::U32)};
    TypeId const s = cappedComposite(ti, TypeKind::Struct, "L", 1, fields, 16u);
    auto const l = layoutOf(s, ti);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[1], 4u);
    EXPECT_EQ(l.align.bytes(), 4u);
    EXPECT_EQ(l.size, 8u);
}

TEST(TypeLayout, PragmaPackCapAndPackedComposeWithPackedWinning) {
    auto ti = makeInterner(1);
    // gcc: an explicit `__attribute__((packed))` is NOT weakened by a
    // surrounding `#pragma pack(4)` — the stricter (1) wins. { char; unsigned }
    // is then 5/1, not the cap's 8/4.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::U32)};
    TypeId const s = cappedComposite(ti, TypeKind::Struct, "PK", 1, fields, 4u,
                                     /*packed=*/true);
    auto const l = layoutOf(s, ti);
    EXPECT_EQ(l.fieldOffsets[1], 1u) << "packed wins over the pack cap";
    EXPECT_EQ(l.align.bytes(), 1u);
    EXPECT_EQ(l.size, 5u);

    // And the cap composes with a whole-composite `aligned(N)` in the other
    // direction: the cap lowers members, the request raises the aggregate.
    std::array<TypeId, 2> const f2{ti.primitive(TypeKind::U32),
                                   ti.primitive(TypeKind::I64)};
    TypeId const both = cappedComposite(ti, TypeKind::Struct, "BOTH", 2, f2, 4u,
                                        /*packed=*/false, /*explicitAlign=*/16u);
    auto const lb = layoutOf(both, ti);
    EXPECT_EQ(lb.fieldOffsets[1], 4u) << "the cap still moves the member";
    EXPECT_EQ(lb.align.bytes(), 16u) << "the request still raises the aggregate";
    EXPECT_EQ(lb.size, 16u);
}

TEST(TypeLayout, PragmaPackCapOnUnionLowersAlignAndSize) {
    auto ti = makeInterner(1);
    // A union places every member at offset 0, so a cap cannot move an offset —
    // it lowers the union's own alignment, and therefore the size it rounds up
    // to. { char; long long } under pack(4) is 8/4; uncapped it is 8/8.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I64)};
    TypeId const u = cappedComposite(ti, TypeKind::Union, "U", 1, fields, 4u);
    auto const l = layoutOf(u, ti);
    EXPECT_EQ(l.align.bytes(), 4u);   // RED-ON-DISABLE (uncapped 8)
    EXPECT_EQ(l.size, 8u);
}

TEST(TypeLayout, CompositeAlignedWeakerThanNaturalIsANoOp) {
    auto ti = makeInterner(1);
    // ★ THE MAX-vs-ASSIGNMENT CONTROL. clang:
    //   `struct L { char c; unsigned v; } __attribute__((aligned(2)));` → 8 / 4.
    // The request is WEAKER than the natural 4, so it does nothing — `aligned` may
    // only RAISE (C 6.7.5). Implement the fold as an assignment instead of a MAX and
    // this becomes 4 / 2 while every other pin in this group still passes.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::U32)};
    TypeId const l2 = alignedComposite(ti, TypeKind::Struct, "L", 1, fields, 2u);
    auto const l = layoutOf(l2, ti);
    EXPECT_EQ(l.align.bytes(), 4u);   // natural 4 SURVIVES the weaker request
    EXPECT_EQ(l.size, 8u);
}

TEST(TypeLayout, CompositeAlignedComposesWithPacked) {
    auto ti = makeInterner(1);
    // ★ THE HEADLINE WITNESS. clang:
    //   `struct S { char a; int b; } __attribute__((packed, aligned(16)));` → 16 / 16.
    // packed removes the inter-field padding (a@0, b@1, extent 5) and the request
    // raises the aggregate — rounding 5 up to 16. Neither channel overrides the
    // other: drop packed and it is 16/16 by a DIFFERENT route (b@4, extent 8); drop
    // the request and it is packed's bare 5 / 1.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I32)};
    TypeId const s = alignedComposite(ti, TypeKind::Struct, "S", 1, fields, 16u,
                                      /*packed=*/true);
    EXPECT_TRUE(ti.isPacked(s));
    EXPECT_EQ(ti.explicitCompositeAlign(s), 16u);
    auto const l = layoutOf(s, ti);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[1], 1u);   // packed still removes the padding
    EXPECT_EQ(l.align.bytes(), 16u);    // ...and the request still raises the whole
    EXPECT_EQ(l.size, 16u);             // clang MEASURED 16, NOT packed's 5
}

TEST(TypeLayout, CompositeAlignedOnUnionRaisesAlignAndSize) {
    auto ti = makeInterner(1);
    // clang: `union UA { char c; int i; } __attribute__((packed, aligned(8)));`
    //        → size 8, align 8. packed alone would give align 1 / size 4.
    std::array<TypeId, 2> const members{ti.primitive(TypeKind::Char),
                                        ti.primitive(TypeKind::I32)};
    TypeId const u = alignedComposite(ti, TypeKind::Union, "UA", 1, members, 8u,
                                      /*packed=*/true);
    auto const l = layoutOf(u, ti);
    EXPECT_EQ(l.align.bytes(), 8u);   // RED-ON-DISABLE (packed → 1)
    EXPECT_EQ(l.size, 8u);            // RED-ON-DISABLE (max member 4)
}

TEST(TypeLayout, CompositeAlignedBitfieldStructRaisesThroughThePacker) {
    auto ti = makeInterner(1);
    // ★ The BIT-FIELD path must honor the request too — it is a SEPARATE code path
    // (the per-ABI packers), and the seed is what covers it BY CONSTRUCTION rather
    // than by a second edit. clang:
    //   `struct BF { int x:3; int y:5; } __attribute__((aligned(16)));` → 16 / 16.
    // (Unaligned it is 4 / 4: 8 bits in one int unit.)
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::I32),
                                       ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 2> const widths{3, 5};
    TypeId const s = alignedComposite(ti, TypeKind::Struct, "BF", 1, fields, 16u,
                                      /*packed=*/false, widths);
    auto const l = layoutOf(s, ti, kGnu16);
    EXPECT_EQ(l.align.bytes(), 16u);   // RED-ON-DISABLE (int unit → 4)
    EXPECT_EQ(l.size, 16u);            // RED-ON-DISABLE (4)
    // The MSVC strategy raises identically — the seed is strategy-independent.
    EXPECT_EQ(layoutOf(s, ti, kMsvc16).align.bytes(), 16u);
}

TEST(TypeLayout, CompositeAlignedPropagatesToArrayStrideAndEnclosingStruct) {
    auto ti = makeInterner(1);
    // The raised alignment is a property of the TYPE, so it flows outward with no
    // extra wiring. clang:
    //   struct A { char c; unsigned v; } __attribute__((aligned(16)));  → 16 / 16
    //   struct A arr[2];                                                 → 32
    //   struct Outer { char a; struct A inner; };  → size 32, align 16, inner@16
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::U32)};
    TypeId const a = alignedComposite(ti, TypeKind::Struct, "A", 1, fields, 16u);
    EXPECT_EQ(layoutOf(ti.array(a, 2), ti).size, 32u);   // stride 16, not 8
    std::array<TypeId, 2> const outerFields{ti.primitive(TypeKind::Char), a};
    auto const o = layoutOf(ti.structType("Outer", outerFields), ti);
    ASSERT_EQ(o.fieldOffsets.size(), 2u);
    EXPECT_EQ(o.fieldOffsets[1], 16u);   // inner pushed to 16  RED-ON-DISABLE (4)
    EXPECT_EQ(o.align.bytes(), 16u);
    EXPECT_EQ(o.size, 32u);
}

TEST(TypeLayout, CompositeAlignedAtTheAlignmentCap) {
    auto ti = makeInterner(1);
    // The upper bound of the `Alignment` newtype's domain. clang:
    //   `struct E0 { int a; } __attribute__((aligned(256)));` → 256 / 256.
    // NOTE `maxAlignment` in the params caps SCALAR alignment (the bounded
    // natural-alignment rule), NOT an explicit aggregate request — which is why the
    // request survives a params `maxAlignment` of 16 here, exactly as in clang.
    std::array<TypeId, 1> const fields{ti.primitive(TypeKind::I32)};
    TypeId const e = alignedComposite(ti, TypeKind::Struct, "E0", 1, fields, 256u);
    auto const l = layoutOf(e, ti, kGnu16);
    EXPECT_EQ(l.align.bytes(), 256u);
    EXPECT_EQ(l.size, 256u);
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

// FC17 (D-CSUBSET-ENUM-UNDERLYING-TYPE, C23 6.7.2.2): an enum with an EXPLICIT
// unsigned-char underlying lays out at the underlying's width/align (1/1), not
// the default int's 4/4 — the layout engine sizes purely by scalars[0], so the
// C23 explicit-underlying feature needs ZERO layout-engine change.
TEST(TypeLayout, EnumFollowsExplicitU8Underlying) {
    auto ti = makeInterner(1);
    auto const l = layoutOf(ti.enumType("E", TypeKind::U8), ti);
    EXPECT_EQ(l.size, 1u)  << "an unsigned-char-underlying enum is 1 byte";
    EXPECT_EQ(l.align.bytes(), 1u);
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

// c99 (D-CSUBSET-FAM-IN-UNION-MEMBER): a union with a FAM-bearing struct member
// sizes to max(FAM-struct PREFIX size, other members) — the FAM tail is 0-length
// for sizeof (C99 §6.7.2.1). This is the COMPANION layout-correctness pin the
// semantic carve-out relies on: the c99 diff touches only the semantic gate, not
// the layout engine, so this pins (unchanged) that once a FAM-struct is permitted
// as a UNION member (gcc/clang accept sqlite's `union { SrcList sSrc; u8 space[N]; }`)
// the union is sized correctly. It is NOT a red-on-disable guard for the carve-out
// (layout is orthogonal to the gate); it guards against a silent union-sizing
// miscompile of the newly-reachable shape. Verified against gcc: for
// `struct Slab{int n; int a[];}` and `union U{struct Slab s; char space[16];}`,
// sizeof(Slab)==4 and sizeof(U)==16.
TEST(TypeLayout, UnionWithFlexibleArrayStructMemberSizesToMaxOfPrefix) {
    auto ti = makeInterner(1);
    // struct Slab { int n; int a[]; }  → prefix size 4, align 4, FAM tail excluded.
    TypeId const n    = ti.primitive(TypeKind::I32);
    TypeId const fam  = ti.incompleteArray(ti.primitive(TypeKind::I32));
    std::array<TypeId, 2> const slabFields{n, fam};
    TypeId const slab = ti.structType("Slab", slabFields);
    auto const slabL = layoutOf(slab, ti);
    ASSERT_EQ(slabL.size, 4u);           // only `n`; the FAM adds offset, not size
    EXPECT_TRUE(slabL.hasFlexibleArrayMember);

    // union U { struct Slab s; char space[16]; } → max(4, 16) = 16, align max(4,1)=4.
    TypeId const space = ti.array(ti.primitive(TypeKind::Char), 16);
    std::array<TypeId, 2> const uFields{slab, space};
    auto const uL = layoutOf(ti.unionType("U", uFields), ti);
    ASSERT_EQ(uL.fieldOffsets.size(), 2u);
    EXPECT_EQ(uL.fieldOffsets[0], 0u);   // both members at offset 0
    EXPECT_EQ(uL.fieldOffsets[1], 0u);
    EXPECT_EQ(uL.size, 16u);             // max(prefix 4, space 16) — NOT the FAM tail
    EXPECT_EQ(uL.align.bytes(), 4u);     // max(int-align 4, char-align 1)

    // If the FAM-struct member DOMINATES the size (its prefix > the sibling), the
    // union takes the prefix — proving the FAM contributes only its non-flexible
    // prefix, never a guessed tail. `struct Big{long p; long q; int a[];}` → prefix 16.
    TypeId const l64 = ti.primitive(TypeKind::I64);
    std::array<TypeId, 3> const bigFields{l64, l64, fam};
    TypeId const big = ti.structType("Big", bigFields);
    ASSERT_EQ(layoutOf(big, ti).size, 16u);
    TypeId const oneByte = ti.array(ti.primitive(TypeKind::Char), 1);
    std::array<TypeId, 2> const u2Fields{big, oneByte};
    EXPECT_EQ(layoutOf(ti.unionType("U2", u2Fields), ti).size, 16u);   // max(16, 1)
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
// (`kGnu16` is declared at the top of this file alongside `kNatural16`.)
// RED-ON-DISABLE for the WHOLE feature: with `BitFieldStrategy::None` a struct
// that HAS a bit-field computes no layout.

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

// The MSVC x64 (PE) params — identical to kGnu16 but with the MS straddling rule —
// are declared at the top of this file alongside `kNatural16`/`kGnu16`.

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
    ti.completeComposite(e, {}, /*packed=*/false);
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
    ti.completeComposite(node, fields, /*packed=*/false);
    auto const l = layoutOf(node, ti);
    EXPECT_EQ(l.size, 16u);
    EXPECT_EQ(l.align.bytes(), 8u);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);   // value
    EXPECT_EQ(l.fieldOffsets[1], 8u);   // next (pointer, 8-aligned)
}

// ── c27 (D-CSUBSET-VOLATILE-POINTEE): a qualifier never changes layout ───────
// sizeof(volatile T) == sizeof(T) and the alignment matches (C 6.7.3). The layout
// engine strips the VolatileQual skin at entry, so a volatile scalar / pointer /
// struct / array lays out byte-identically to its material type. RED-ON-DISABLE:
// drop the `stripVolatile` at the top of computeLayout → a VolatileQual id hits
// the engine's default (no scalar size, raw-kind != Struct) → nullopt → this
// EXPECTs a layout and fails.
TEST(TypeLayout, VolatileQualifierDoesNotChangeLayout) {
    auto ti = makeInterner(1);
    // scalar: volatile int ≡ int (4/4).
    const TypeId i32  = ti.primitive(TypeKind::I32);
    const TypeId vi32 = ti.volatileQualified(i32);
    auto const li = layoutOf(i32, ti);
    auto const lvi = layoutOf(vi32, ti);
    EXPECT_EQ(lvi.size, li.size);
    EXPECT_EQ(lvi.align.bytes(), li.align.bytes());
    EXPECT_EQ(lvi.size, 4u);

    // pointer (east `T * volatile`): VolatileQual(Ptr<int>) ≡ Ptr<int> (8/8 LP64).
    const TypeId p  = ti.pointer(i32);
    const TypeId vp = ti.volatileQualified(p);
    EXPECT_EQ(layoutOf(vp, ti).size, layoutOf(p, ti).size);
    EXPECT_EQ(layoutOf(vp, ti).size, 8u);

    // struct: volatile struct S ≡ struct S (field offsets + size + align match).
    const TypeId f32 = ti.primitive(TypeKind::F32);
    std::array<TypeId, 2> const fields{i32, f32};
    const TypeId s  = ti.structType("S", fields);
    const TypeId vs = ti.volatileQualified(s);
    auto const ls  = layoutOf(s, ti);
    auto const lvs = layoutOf(vs, ti);
    EXPECT_EQ(lvs.size, ls.size);
    EXPECT_EQ(lvs.align.bytes(), ls.align.bytes());
    ASSERT_EQ(lvs.fieldOffsets.size(), ls.fieldOffsets.size());
    for (std::size_t i = 0; i < ls.fieldOffsets.size(); ++i)
        EXPECT_EQ(lvs.fieldOffsets[i], ls.fieldOffsets[i]);

    // array of volatile: Array<VolatileQual(int), 4> ≡ Array<int,4> (16 bytes).
    const TypeId va  = ti.array(vi32, 4);
    const TypeId a   = ti.array(i32, 4);
    EXPECT_EQ(layoutOf(va, ti).size, layoutOf(a, ti).size);
    EXPECT_EQ(layoutOf(va, ti).size, 16u);
}

// FC17.9(d) 1a (D-CSUBSET-QUAL-BITSET): the SAME layout-invariance holds for the
// atomic bit and for the combined `_Atomic volatile` mask — the layout engine strips
// the whole qualifier skin, so a lock-free-scalar atomic never changes size/align (a
// naturally-aligned `_Atomic int` IS lock-free; non-lock-free `_Atomic` is a cycle-1b
// fail-loud, never a silent layout change). The scalar arms are coverage-by-
// construction (kind() is transparent); the top-level `_Atomic struct` arm is the
// RED-ON-DISABLE seam — computeLayout must `stripVolatile` at entry to see the raw
// Struct kind, so deleting that strip makes the atomic-qualified struct hit the engine
// default (raw kind != Struct) → nullopt → the struct EXPECTs below fail (the exact
// `VolatileQualifierDoesNotChangeLayout` precedent, now proven for the atomic bit too).
TEST(TypeLayout, AtomicQualifierDoesNotChangeLayout) {
    auto ti = makeInterner(1);
    const TypeId i32  = ti.primitive(TypeKind::I32);
    const TypeId ai32 = ti.atomicQualified(i32);                          // _Atomic int
    const TypeId avi32 = ti.atomicQualified(ti.volatileQualified(i32));   // _Atomic volatile int
    auto const li = layoutOf(i32, ti);
    for (const TypeId q : {ai32, avi32}) {                                // scalar: 4/4
        auto const lq = layoutOf(q, ti);
        EXPECT_EQ(lq.size, li.size);
        EXPECT_EQ(lq.align.bytes(), li.align.bytes());
        EXPECT_EQ(lq.size, 4u);
    }
    // top-level `_Atomic struct S` ≡ struct S (2 fields i32+f32) — the strip-seam arm.
    const TypeId f32 = ti.primitive(TypeKind::F32);
    std::array<TypeId, 2> const fields{i32, f32};
    const TypeId s  = ti.structType("S", fields);
    const TypeId as = ti.atomicQualified(s);
    auto const ls  = layoutOf(s, ti);
    auto const las = layoutOf(as, ti);
    EXPECT_EQ(las.size, ls.size);
    EXPECT_EQ(las.align.bytes(), ls.align.bytes());
    ASSERT_EQ(las.fieldOffsets.size(), ls.fieldOffsets.size());
    for (std::size_t i = 0; i < ls.fieldOffsets.size(); ++i)
        EXPECT_EQ(las.fieldOffsets[i], ls.fieldOffsets[i]);
}

// ── C23 _BitInt(N) ABI (D-CSUBSET-BITINT) — witnessed vs clang-19 x86-64 psABI ──

TEST(TypeLayout, BitIntSizeAndAlign) {
    auto ti = makeInterner(70);
    struct Case { std::int64_t n; bool sgned; std::uint64_t size; std::uint32_t align; };
    // N<=64 → smallest {1,2,4,8}B container, align == size. N>64 → ceil(N/64) eight-
    // bytes, align 8 (the _BitInt(128) size-16 / align-8 quirk). Matches clang-19.
    for (Case const c : {
             Case{4,  true,  1, 1}, Case{4,  false, 1, 1},
             Case{8,  false, 1, 1}, Case{9,  false, 2, 2},
             Case{16, true,  2, 2}, Case{17, true,  4, 4},
             Case{32, false, 4, 4}, Case{33, true,  8, 8},
             Case{40, false, 8, 8}, Case{64, true,  8, 8},
             Case{128, true, 16, 8}, Case{200, false, 32, 8}}) {
        auto const l = layoutOf(ti.bitInt(c.n, c.sgned), ti);
        EXPECT_EQ(l.size, c.size) << "sizeof(_BitInt(" << c.n << "))";
        EXPECT_EQ(l.align.bytes(), c.align) << "_Alignof(_BitInt(" << c.n << "))";
    }
}

// `sizeOfScalarOrBitInt` — the TypeId-aware shim (aggregate-ABI / data-global leaf
// sizing): a BitInt gets its container size; every other scalar defers to
// `scalarByteSize`.
TEST(TypeLayout, SizeOfScalarOrBitIntShim) {
    auto ti = makeInterner(71);
    EXPECT_EQ(sizeOfScalarOrBitInt(ti, ti.bitInt(4, false),  DataModel::Lp64), 1u);
    EXPECT_EQ(sizeOfScalarOrBitInt(ti, ti.bitInt(40, false), DataModel::Lp64), 8u);
    EXPECT_EQ(sizeOfScalarOrBitInt(ti, ti.bitInt(128, true), DataModel::Lp64), 16u);
    EXPECT_EQ(sizeOfScalarOrBitInt(ti, ti.primitive(TypeKind::I32), DataModel::Lp64), 4u);
}

// D-CSUBSET-BITINT-C2-WIDE + D-CSUBSET-UINT128-TYPE: the memory-resident / by-value-
// class type-shape predicates over EVERY form. A WIDE integer — a `_BitInt(N>64)` or
// a 128-bit standard-rank integer (I128/U128) — is BOTH (multi-limb, reached by
// ADDRESS); a narrow `_BitInt(N<=64)` is NEITHER (a single native container — a
// scalar). Array is memory-resident but NOT by-value-class (it decays); struct/union
// are both; a plain scalar and the invalid TypeId are neither. `isWideInt` is the
// membership line: the exact N>64 boundary for BitInt, plus the two 128-bit kinds.
TEST(TypeLayout, WideBitIntTypeShapePredicates) {
    auto ti = makeInterner(72);
    TypeId const wide   = ti.bitInt(128, true);
    TypeId const wide2  = ti.bitInt(65, false);   // the smallest wide width
    TypeId const narrow = ti.bitInt(64, true);    // the widest single-container width
    TypeId const i32    = ti.primitive(TypeKind::I32);
    TypeId const i64    = ti.primitive(TypeKind::I64);
    TypeId const i128   = ti.primitive(TypeKind::I128);
    TypeId const u128   = ti.primitive(TypeKind::U128);
    std::array<TypeId, 1> const sfields{i32};
    TypeId const st     = ti.structType("S", sfields);
    TypeId const arr    = ti.array(i32, 4);

    // isWideInt — the exact N>64 boundary (64 is narrow, 65 is wide), PLUS the two
    // 128-bit standard-rank kinds. I64 pins the "64 bits is still native" line for a
    // non-BitInt kind, mirroring `narrow` on the BitInt side.
    EXPECT_TRUE(isWideInt(ti, wide));
    EXPECT_TRUE(isWideInt(ti, wide2));
    EXPECT_TRUE(isWideInt(ti, i128));
    EXPECT_TRUE(isWideInt(ti, u128));
    EXPECT_FALSE(isWideInt(ti, narrow));
    EXPECT_FALSE(isWideInt(ti, i32));
    EXPECT_FALSE(isWideInt(ti, i64));
    EXPECT_FALSE(isWideInt(ti, st));
    EXPECT_FALSE(isWideInt(ti, TypeId{}));

    // The width/signedness accessors over every wide form. BitInt reads the interned
    // pair; the 128-bit kinds answer from the KIND alone (I128 signed, U128 not).
    EXPECT_EQ(wideIntWidthBits(ti, wide), 128);
    EXPECT_EQ(wideIntWidthBits(ti, wide2), 65);
    EXPECT_EQ(wideIntWidthBits(ti, i128), 128);
    EXPECT_EQ(wideIntWidthBits(ti, u128), 128);
    EXPECT_TRUE(wideIntIsSigned(ti, wide));
    EXPECT_FALSE(wideIntIsSigned(ti, wide2));
    EXPECT_TRUE(wideIntIsSigned(ti, i128));
    EXPECT_FALSE(wideIntIsSigned(ti, u128));

    // isMemoryResidentType — struct/union/array + EVERY wide integer; NOT narrow/
    // scalar/invalid. The I128/U128 rows are what route a 128-bit value through the
    // multi-limb emitters instead of a bare-SSA scalar carrying only the low 8 bytes.
    EXPECT_TRUE(isMemoryResidentType(ti, wide));
    EXPECT_TRUE(isMemoryResidentType(ti, i128));
    EXPECT_TRUE(isMemoryResidentType(ti, u128));
    EXPECT_TRUE(isMemoryResidentType(ti, st));
    EXPECT_TRUE(isMemoryResidentType(ti, arr));
    EXPECT_FALSE(isMemoryResidentType(ti, narrow));
    EXPECT_FALSE(isMemoryResidentType(ti, i32));
    EXPECT_FALSE(isMemoryResidentType(ti, i64));
    EXPECT_FALSE(isMemoryResidentType(ti, TypeId{}));

    // isByValueClass — struct/union + EVERY wide integer; ARRAY EXCLUDED (decays), NOT
    // narrow/scalar/invalid. This is the ONLY difference from isMemoryResidentType:
    // the array. The two predicates must agree on I128/U128 (neither decays).
    EXPECT_TRUE(isByValueClass(ti, wide));
    EXPECT_TRUE(isByValueClass(ti, i128));
    EXPECT_TRUE(isByValueClass(ti, u128));
    EXPECT_TRUE(isByValueClass(ti, st));
    EXPECT_FALSE(isByValueClass(ti, arr));       // the array/by-value distinction
    EXPECT_FALSE(isByValueClass(ti, narrow));
    EXPECT_FALSE(isByValueClass(ti, i32));
    EXPECT_FALSE(isByValueClass(ti, i64));
    EXPECT_FALSE(isByValueClass(ti, TypeId{}));
}

// ★ D-CSUBSET-UINT128-TYPE: `_BitInt(128)` and `unsigned __int128` share a limb COUNT
// and a SIZE but NOT a LAYOUT, and the whole 128-bit design depends on those two facts
// staying independent. `_BitInt(128)` is 16/8 — size 16, align EIGHT — because the
// x86-64 psABI aligns a bit-precise type to its limb, not its size (computeLayout's
// BitInt arm hardcodes alignBytes=8 for N>64, and examples/c-subset/c23_bitint_wide
// pins it from real C). U128 is 16/16 — the ordinary natural-alignment rule applied to
// a 16-byte scalar via scalarByteSize. MEASURED against the shipped consumer: the pe/
// x86_64 `jmp_buf` is `arr<u128,16>` (shippedLibs/setjmp.json), sizeof 256, _Alignof 16
// — which is exactly `_setjmp`'s movaps requirement and would BREAK at align 8.
// Asserting both in ONE test is the point: a future "unify the 128-bit types" edit
// cannot make one follow the other without turning this red.
TEST(TypeLayout, Int128AndBitInt128AreIndependentLayouts) {
    auto ti = makeInterner(74);

    auto const lb = layoutOf(ti.bitInt(128, true), ti);
    EXPECT_EQ(lb.size, 16u);
    EXPECT_EQ(lb.align.bytes(), 8u) << "_Alignof(_BitInt(128)) must stay 8 (psABI)";

    auto const li = layoutOf(ti.primitive(TypeKind::I128), ti);
    EXPECT_EQ(li.size, 16u);
    EXPECT_EQ(li.align.bytes(), 16u) << "_Alignof(__int128) must stay 16";

    auto const lu = layoutOf(ti.primitive(TypeKind::U128), ti);
    EXPECT_EQ(lu.size, 16u);
    EXPECT_EQ(lu.align.bytes(), 16u) << "_Alignof(unsigned __int128) must stay 16";

    // Same size, DIFFERENT alignment — stated as a relation so the divergence itself
    // is the assertion, not just two coincidental constants.
    EXPECT_EQ(lb.size, lu.size);
    EXPECT_NE(lb.align.bytes(), lu.align.bytes());

    // The shipped consumer: `arr<u128,16>` is the pe/x86_64 jmp_buf — 256 bytes,
    // 16-aligned. Element alignment propagates to the array (setjmp_longjmp depends
    // on it: a wrong alignment crashes `_setjmp`'s movaps xmm saves).
    auto const lj = layoutOf(ti.array(ti.primitive(TypeKind::U128), 16), ti);
    EXPECT_EQ(lj.size, 256u);
    EXPECT_EQ(lj.align.bytes(), 16u);
}

// ★ D-CSUBSET-UINT128-TYPE: the align-16 fact PROPAGATED THROUGH A REAL AGGREGATE, and
// the C++ twin of the `_Static_assert(sizeof(struct N) == 528)` in
// examples/c-subset/c_int128_arith/main.c. Two reasons this lives here as well:
//
//  1. UN-MASKABLE. A failed `_Static_assert` ABORTS translation, so if the example's
//     compile-time block ever turns red it takes every runtime pin in that file with
//     it. This test cannot be masked that way — it is the belt for the example's braces.
//  2. NON-VACUOUS BY MEASUREMENT. `struct { u128 v[32]; unsigned f1, f2; }` is 528, and
//     the `_BitInt(128)` twin of the SAME struct is 520. The 8-byte difference is the
//     tail round-up: 32*16 = 512 bytes of array + 8 bytes of `unsigned` pair = 520,
//     rounded UP to the struct alignment — 16 for U128 (⇒ 528), 8 for `_BitInt(128)`
//     (⇒ 520, a no-op round). Asserting BOTH numbers in one test is the point: it is
//     the aggregate-level statement of the same independence
//     `Int128AndBitInt128AreIndependentLayouts` asserts on the bare scalars, and a
//     future "unify the 128-bit types" edit cannot satisfy both.
//
// The `unsigned` pair is load-bearing — a struct of ONLY the array would be 512 either
// way and the two layouts would agree, which is exactly the vacuous shape to avoid.
TEST(TypeLayout, Int128AggregateLayoutVsBitInt128) {
    auto ti = makeInterner(75);
    TypeId const u32Ty = ti.primitive(TypeKind::U32);

    std::array<TypeId, 3> const uFields{
        ti.array(ti.primitive(TypeKind::U128), 32), u32Ty, u32Ty};
    auto const lu = layoutOf(ti.structType("NU128", uFields), ti);
    EXPECT_EQ(lu.size, 528u)
        << "512B of u128[32] + 8B of unsigned pair, rounded UP to align 16";
    EXPECT_EQ(lu.align.bytes(), 16u) << "the member's align 16 propagates to the struct";

    std::array<TypeId, 3> const bFields{
        ti.array(ti.bitInt(128, false), 32), u32Ty, u32Ty};
    auto const lb = layoutOf(ti.structType("NBitInt128", bFields), ti);
    EXPECT_EQ(lb.size, 520u)
        << "the _BitInt(128) twin: same 520 bytes of payload, but align 8 makes the "
           "tail round-up a NO-OP — this is what makes the 528 above non-vacuous";
    EXPECT_EQ(lb.align.bytes(), 8u) << "_Alignof(_BitInt(128)) must stay 8 (psABI)";

    // Stated as a relation, so the DIVERGENCE itself is the assertion.
    EXPECT_NE(lu.size, lb.size);
    EXPECT_NE(lu.align.bytes(), lb.align.bytes());
}

// C99 _Complex (D-CSUBSET-COMPLEX §6.2.5p13): a complex lays out as {re@0, im@es},
// size 2×elemSize, align = element align — the ABI leaf {re, im} layout. It is a
// memory-resident by-VALUE class (like a struct{re,im}; NOT decaying like an array).
TEST(TypeLayout, ComplexLayoutAndShapePredicates) {
    auto ti = makeInterner(73);
    TypeId const cd  = ti.complex(ti.primitive(TypeKind::F64));   // double _Complex
    TypeId const cf  = ti.complex(ti.primitive(TypeKind::F32));   // float _Complex
    TypeId const cld = ti.complex(ti.primitive(TypeKind::F80));   // long double (x87-80)

    // Layout: size 2×elem, align == element align. double→16/8, float→8/4, F80→32/16.
    auto const ld = layoutOf(cd, ti);
    EXPECT_EQ(ld.size, 16u);   EXPECT_EQ(ld.align.bytes(), 8u);
    auto const lf = layoutOf(cf, ti);
    EXPECT_EQ(lf.size, 8u);    EXPECT_EQ(lf.align.bytes(), 4u);
    auto const lld = layoutOf(cld, ti);
    EXPECT_EQ(lld.size, 32u);  EXPECT_EQ(lld.align.bytes(), 16u);

    // isComplex + complexElement round-trip; a non-complex is not complex.
    EXPECT_TRUE(isComplex(ti, cd));
    EXPECT_FALSE(isComplex(ti, ti.primitive(TypeKind::F64)));
    EXPECT_FALSE(isComplex(ti, TypeId{}));
    EXPECT_EQ(ti.complexElement(cd).v, ti.primitive(TypeKind::F64).v);
    EXPECT_EQ(ti.complexElement(cf).v, ti.primitive(TypeKind::F32).v);

    // Memory-resident AND by-value-class (a complex is passed/returned like a struct;
    // it does NOT decay). NOT a wide _BitInt.
    EXPECT_TRUE(isMemoryResidentType(ti, cd));
    EXPECT_TRUE(isByValueClass(ti, cd));
    EXPECT_FALSE(isWideInt(ti, cd));

    // IMPORTANT-5 (red-on-disable): a Complex takes the GPR default reg class — NEVER
    // the FPR arm. requireEncodedFloatWidth no-ops on non-FPR, so the aggregate never
    // trips the F80/F128 wall; its F64 COMPONENTS query regClassForCoreType(F64)=FPR
    // correctly. A Complex→FPR arm would silently integer-plumb / wrong-gate — this
    // pin fails the moment one is added.
    EXPECT_EQ(regClassForCoreType(TypeKind::Complex), TargetRegClass::GPR);
    EXPECT_EQ(regClassForCoreType(TypeKind::F64),     TargetRegClass::FPR);
}
