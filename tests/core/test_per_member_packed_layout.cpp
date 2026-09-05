// D-CSUBSET-PER-MEMBER-PACKED — the hermetic half of the per-FIELD packed witness.
//
// GNU lets `packed` sit on ONE member-declarator:
//     struct S { char a; int z __attribute__((packed)); double d; };
// which packs `z` and NOTHING else. Every golden below is TRANSCRIBED FROM A
// MEASUREMENT, never hand-reasoned — the instrument is
// `scratchpad/p58/pk/probe_layout.c` + `probe2.c` + `probe3.c` + `probe4.c`,
// compiled `-Wall -Wextra -Wattributes` and RUN, printing `sizeof`, `_Alignof` and
// `offsetof` for every member.
//
// ★★ REFERENCES PROBED SEPARATELY, AND EVERY WORKING ARM AGREED BYTE FOR BYTE —
// across BOTH BYTE ORDERS, so these goldens are not a little-endian claim:
//   • gcc 13.3.0        x86_64-linux (native)
//   • gcc 13.3.0        aarch64-linux-gnu cross, run under qemu-aarch64
//   • gcc 13            s390x-linux-gnu cross, run under qemu-s390x — BIG-ENDIAN,
//                       and identical on the whole matrix incl. every discriminating
//                       shape below
//   • clang 18.1.3      x86_64-linux (native)
//   • clang 18.1.3      aarch64-linux-gnu, run under qemu-aarch64
//   • clang 18.1.3      s390x-linux-gnu, run under qemu-s390x — the SECOND
//                       big-endian vote, probed separately from gcc's because the
//                       two are different references; identical on the full matrix
//                       INCLUDING the bit-field straddle discriminators below
//   • mingw-w64 gcc 13.2.0 (the PE reference) agreed on every non-bit-field shape
//   • MSVC 19.51.36252 **ABSTAINS** — it implements no `__attribute__` in ANY
//     position (error C2146/C2061/C2059 at /std:c11, /std:c17 AND /std:clatest,
//     rc 2). An abstention is NOT agreement; two working references make the GNU
//     spelling REQUIRED.
// There is NO meaning fork here: nothing accepted this construct and disagreed
// about what it means.
//
// ★★★ WHY THE PINS BELOW CHECK OFFSETS AND NOT ONLY SIZES. The headline shape
// `{char a; int z <packed>; double d;}` is sizeof 16 / _Alignof 8 — IDENTICAL to the
// undecorated control. ONLY `z`'s offset moves, 4 → 1. A size-based or
// alignment-based assertion is blind to this entire defect, and so is every
// downstream consumer that compares sizes. Each such pin is marked SIZE-BLIND below.
//
// ★★ AND WHY THIS IS NOT THE WHOLE-COMPOSITE FLAG APPLIED NARROWLY. A per-member
// packed never touches the AGGREGATE's alignment; the aggregate keeps the ordinary
// MAX-fold over the EFFECTIVE member alignments. MEASURED, same shape:
//     per-member : 16 / align 8 / a@0 z@1 d@8
//     whole-composite: 13 / align 1 / a@0 z@1 d@5
// `{char a; int z <packed>;}` DOES come out 5 / align 1 — but only because 1 is then
// the max, which is why the row's own illustration of the defect could not
// distinguish the two spellings and this file does not use it.

#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "core/types/type_lattice/type_reintern.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>

using namespace dss;

namespace {

[[nodiscard]] TypeInterner makeInterner(std::uint32_t owner) {
    return TypeInterner{CompilationUnitId{owner}};
}

constexpr AggregateLayoutParams kNatural16{ScalarAlignmentRule::Natural, 16};
constexpr AggregateLayoutParams kGnu16{
    ScalarAlignmentRule::Natural, 16, BitFieldStrategy::GnuPacked};

[[nodiscard]] StructLayout layoutOf(TypeId id, TypeInterner const& ti,
                                    AggregateLayoutParams p = kNatural16,
                                    DataModel dm = DataModel::Lp64) {
    auto const l = computeLayout(id, ti, p, dm);
    EXPECT_TRUE(l.has_value()) << "expected a layout for this type";
    return l.value_or(StructLayout{});
}

// Build a struct whose fields carry PER-FIELD packed flags. `packedFlags` is
// all-fields-or-empty, exactly like `fieldAligns`.
[[nodiscard]] TypeId perMemberPackedStruct(TypeInterner& ti, std::string_view name,
                                           std::uint64_t key,
                                           std::span<TypeId const> fields,
                                           std::span<std::uint8_t const> packedFlags) {
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 0> const noAligns{};
    TypeId const s = ti.forwardComposite(TypeKind::Struct, name, key);
    ti.completeComposite(s, fields, /*packed=*/false, noWidths, noOffs, noAligns,
                         /*explicitAlign=*/0, /*maxFieldAlign=*/0, packedFlags);
    return s;
}

} // namespace

// ── the headline shape: the offset moves, the size does not ─────────────────

TEST(PerMemberPackedLayout, PacksOneFieldAndLeavesTheAggregateAlone) {
    auto ti = makeInterner(1);
    // MEASURED (all four arms): {char a; int z <pk>; double d;} 16 / align 8 /
    //                            a@0 z@1 d@8
    //           control          {char a; int z;     double d;} 16 / align 8 /
    //                            a@0 z@4 d@8
    std::array<TypeId, 3> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I32),
                                       ti.primitive(TypeKind::F64)};
    std::array<std::uint8_t, 3> const flags{0, 1, 0};   // packed on `z` only
    TypeId const s = perMemberPackedStruct(ti, "Disc", 1, fields, flags);

    EXPECT_TRUE(ti.hasFieldPacked(s));
    EXPECT_FALSE(ti.isFieldPacked(s, 0));
    EXPECT_TRUE(ti.isFieldPacked(s, 1));
    EXPECT_FALSE(ti.isFieldPacked(s, 2));
    EXPECT_FALSE(ti.isPacked(s)) << "a per-member packed is NOT the composite flag";

    auto const l = layoutOf(s, ti);
    ASSERT_EQ(l.fieldOffsets.size(), 3u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[1], 1u);   // z@1, NOT @4            RED-ON-DISABLE
    EXPECT_EQ(l.fieldOffsets[2], 8u);   // d@8 — d is NOT packed  RED-ON-DISABLE
    EXPECT_EQ(l.align.bytes(), 8u);     // the aggregate alignment is UNTOUCHED
    EXPECT_EQ(l.size, 16u);             // SIZE-BLIND: the control is 16 too

    // The same fields with NO per-field flag: a DISTINCT interned type with the
    // padded layout. Same size, same alignment, different offset — which is exactly
    // why the flags had to enter the content identity.
    std::array<TypeId, 3> const fields2{ti.primitive(TypeKind::Char),
                                        ti.primitive(TypeKind::I32),
                                        ti.primitive(TypeKind::F64)};
    TypeId const nat = ti.structType("Disc", fields2);
    EXPECT_NE(s.v, nat.v) << "per-field packed must enter the content identity";
    auto const ln = layoutOf(nat, ti);
    EXPECT_EQ(ln.fieldOffsets[1], 4u);  // the control                RED-ON-DISABLE
    EXPECT_EQ(ln.align.bytes(), 8u);    // identical to the packed one
    EXPECT_EQ(ln.size, 16u);            // identical to the packed one
}

TEST(PerMemberPackedLayout, IsDistinctFromTheWholeCompositeSpelling) {
    auto ti = makeInterner(1);
    // MEASURED: the whole-composite spelling of the SAME fields is 13 / align 1 /
    // a@0 z@1 d@5 — a different size AND a different alignment AND a different
    // offset for `d`. The two spellings are not the same feature at two grains.
    std::array<TypeId, 3> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I32),
                                       ti.primitive(TypeKind::F64)};
    TypeId const whole = ti.forwardComposite(TypeKind::Struct, "W", 7);
    ti.completeComposite(whole, fields, /*packed=*/true);
    auto const lw = layoutOf(whole, ti);
    EXPECT_EQ(lw.fieldOffsets[1], 1u);
    EXPECT_EQ(lw.fieldOffsets[2], 5u);   // d@5, not @8              RED-ON-DISABLE
    EXPECT_EQ(lw.align.bytes(), 1u);
    EXPECT_EQ(lw.size, 13u);
}

TEST(PerMemberPackedLayout, LowersTheAggregateOnlyWhenItIsTheMaxContributor) {
    auto ti = makeInterner(1);
    // MEASURED: {int z <pk>;} is 4 / align 1 (control 4 / align 4) — the aggregate
    // alignment falls out of the ordinary MAX-fold over EFFECTIVE member alignments,
    // not out of a special rule. SIZE-BLIND on both counts: sizeof is 4 either way.
    std::array<TypeId, 1> const fields{ti.primitive(TypeKind::I32)};
    std::array<std::uint8_t, 1> const flags{1};
    TypeId const s = perMemberPackedStruct(ti, "Only", 1, fields, flags);
    auto const l = layoutOf(s, ti);
    EXPECT_EQ(l.align.bytes(), 1u);   // RED-ON-DISABLE
    EXPECT_EQ(l.size, 4u);            // SIZE-BLIND

    std::array<TypeId, 1> const fields2{ti.primitive(TypeKind::I32)};
    TypeId const nat = ti.structType("OnlyC", fields2);
    auto const ln = layoutOf(nat, ti);
    EXPECT_EQ(ln.align.bytes(), 4u);
    EXPECT_EQ(ln.size, 4u);
}

TEST(PerMemberPackedLayout, MultipleFieldsAndAWideUnpackedSibling) {
    auto ti = makeInterner(1);
    // MEASURED: {char a; int z <pk>; double d; int w <pk>;} 24 / align 8 /
    //           a@0 z@1 d@8 w@16
    std::array<TypeId, 4> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I32),
                                       ti.primitive(TypeKind::F64),
                                       ti.primitive(TypeKind::I32)};
    std::array<std::uint8_t, 4> const flags{0, 1, 0, 1};
    TypeId const s = perMemberPackedStruct(ti, "F1", 1, fields, flags);
    auto const l = layoutOf(s, ti);
    ASSERT_EQ(l.fieldOffsets.size(), 4u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[1], 1u);    // RED-ON-DISABLE
    EXPECT_EQ(l.fieldOffsets[2], 8u);    // d is unpacked and still 8-aligned
    EXPECT_EQ(l.fieldOffsets[3], 16u);   // w packed, lands right after d
    EXPECT_EQ(l.align.bytes(), 8u);
    EXPECT_EQ(l.size, 24u);
}

TEST(PerMemberPackedLayout, MemberAlignasRaisesFromThePackedBaselineNotTheNatural) {
    auto ti = makeInterner(1);
    // MEASURED: {char a; int z __attribute__((packed, aligned(2)));} is 6 / align 2
    // with z@2. If the fold were max(NATURAL 4, 2) the answer would be 4/8/z@4;
    // it is max(PACKED BASELINE 1, 2) = 2. This pin is what fixes the ORDER of the
    // two operations, which no size check could.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 2> const aligns{0, 2};      // aligned(2) on z
    std::array<std::uint8_t, 2>  const flags{0, 1};       // packed on z
    TypeId const s = ti.forwardComposite(TypeKind::Struct, "G", 1);
    ti.completeComposite(s, fields, /*packed=*/false, noWidths, noOffs, aligns,
                         /*explicitAlign=*/0, /*maxFieldAlign=*/0, flags);
    auto const l = layoutOf(s, ti);
    EXPECT_EQ(l.fieldOffsets[1], 2u);   // RED-ON-DISABLE
    EXPECT_EQ(l.align.bytes(), 2u);
    EXPECT_EQ(l.size, 6u);
}

TEST(PerMemberPackedLayout, WinsOverASurroundingPackPragmaCap) {
    auto ti = makeInterner(1);
    // MEASURED under `#pragma pack(4)`:
    //   {char a; long long z <pk>;}  9 / align 1 / z@1
    //   {char a; long long z;}      12 / align 4 / z@4   (the cap alone)
    // The per-field flag WINS over the cap, exactly as the whole-composite flag
    // does — which falls out of `clampedBaselineAlign` returning the packed
    // baseline BEFORE it consults the cap.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I64)};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 0> const noAligns{};
    std::array<std::uint8_t, 2>  const flags{0, 1};
    TypeId const s = ti.forwardComposite(TypeKind::Struct, "P1", 1);
    ti.completeComposite(s, fields, /*packed=*/false, noWidths, noOffs, noAligns,
                         /*explicitAlign=*/0, /*maxFieldAlign=*/4, flags);
    auto const l = layoutOf(s, ti);
    EXPECT_EQ(l.fieldOffsets[1], 1u);   // RED-ON-DISABLE
    EXPECT_EQ(l.align.bytes(), 1u);
    EXPECT_EQ(l.size, 9u);

    // The cap alone, same fields, no per-field flag.
    std::array<TypeId, 2> const fields2{ti.primitive(TypeKind::Char),
                                        ti.primitive(TypeKind::I64)};
    std::array<std::uint32_t, 0> const noAligns2{};
    std::array<std::uint8_t, 0>  const noFlags{};
    TypeId const c = ti.structType("P1c", fields2, noWidths, noOffs, noAligns2,
                                   /*explicitAlign=*/0, /*maxFieldAlign=*/4, noFlags);
    auto const lc = layoutOf(c, ti);
    EXPECT_EQ(lc.fieldOffsets[1], 4u);
    EXPECT_EQ(lc.align.bytes(), 4u);
    EXPECT_EQ(lc.size, 12u);
}

TEST(PerMemberPackedLayout, WholeCompositeAlignedStillRaisesTheAggregate) {
    auto ti = makeInterner(1);
    // MEASURED: {char a; int z <pk>;} __attribute__((aligned(16))) is 16 / align 16
    // with z@1 — the per-field lowering and the whole-composite raise compose, the
    // same way they already do for the composite `packed`.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 0> const noAligns{};
    std::array<std::uint8_t, 2>  const flags{0, 1};
    TypeId const s = ti.forwardComposite(TypeKind::Struct, "E1", 1);
    ti.completeComposite(s, fields, /*packed=*/false, noWidths, noOffs, noAligns,
                         /*explicitAlign=*/16, /*maxFieldAlign=*/0, flags);
    auto const l = layoutOf(s, ti);
    EXPECT_EQ(l.fieldOffsets[1], 1u);
    EXPECT_EQ(l.align.bytes(), 16u);
    EXPECT_EQ(l.size, 16u);
}

TEST(PerMemberPackedLayout, PackingAnAggregateMemberLowersItsAlignmentNotItsLayout) {
    auto ti = makeInterner(1);
    // MEASURED: {char a; struct {char; int; double} inner <pk>;} is 17 / align 1
    // with inner@1 — the INNER struct keeps its own 16-byte layout intact and only
    // its placement alignment falls to 1.
    std::array<TypeId, 3> const innerF{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I32),
                                       ti.primitive(TypeKind::F64)};
    TypeId const inner = ti.structType("Inner", innerF);
    EXPECT_EQ(layoutOf(inner, ti).size, 16u);

    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char), inner};
    std::array<std::uint8_t, 2> const flags{0, 1};
    TypeId const s = perMemberPackedStruct(ti, "H", 2, fields, flags);
    auto const l = layoutOf(s, ti);
    EXPECT_EQ(l.fieldOffsets[1], 1u);   // RED-ON-DISABLE
    EXPECT_EQ(l.align.bytes(), 1u);
    EXPECT_EQ(l.size, 17u);             // 1 + the inner's UNCHANGED 16
}

TEST(PerMemberPackedLayout, UnionMemberLowersTheUnionAlignmentWithoutChangingItsSize) {
    auto ti = makeInterner(1);
    // MEASURED: union {char a; int z <pk>;} is 4 / align 1; the control is 4 /
    // align 4. SIZE-BLIND — only the alignment moves.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 0> const noAligns{};
    std::array<std::uint8_t, 2>  const flags{0, 1};
    TypeId const u = ti.forwardComposite(TypeKind::Union, "U1", 1);
    ti.completeComposite(u, fields, /*packed=*/false, noWidths, noOffs, noAligns,
                         /*explicitAlign=*/0, /*maxFieldAlign=*/0, flags);
    auto const l = layoutOf(u, ti);
    EXPECT_EQ(l.align.bytes(), 1u);   // RED-ON-DISABLE
    EXPECT_EQ(l.size, 4u);            // SIZE-BLIND

    std::array<TypeId, 2> const fields2{ti.primitive(TypeKind::Char),
                                        ti.primitive(TypeKind::I32)};
    TypeId const c = ti.unionType("U1c", fields2);
    auto const lc = layoutOf(c, ti);
    EXPECT_EQ(lc.align.bytes(), 4u);
    EXPECT_EQ(lc.size, 4u);
}

// ── the bit-field interaction: the straddle gate is PER FIELD ───────────────

TEST(PerMemberPackedLayout, OrdinaryMemberPackedInABitfieldBearingStruct) {
    auto ti = makeInterner(1);
    // MEASURED (gcc + clang, x86_64 + aarch64):
    //   {unsigned char a; int c <pk>; unsigned b:31; char t;} 16 / align 4 / a@0 c@1
    //   {unsigned char a; int c;      unsigned b:31; char t;} 16 / align 4 / a@0 c@4
    // SIZE-BLIND and ALIGN-BLIND: only `c`'s offset moves.
    std::array<TypeId, 4> const fields{ti.primitive(TypeKind::U8),
                                       ti.primitive(TypeKind::I32),
                                       ti.primitive(TypeKind::U32),
                                       ti.primitive(TypeKind::Char)};
    std::array<std::int64_t, 4> const widths{
        kNotBitfield, kNotBitfield, 31,
        kNotBitfield};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 0> const noAligns{};
    std::array<std::uint8_t, 4>  const flags{0, 1, 0, 0};
    TypeId const s = ti.forwardComposite(TypeKind::Struct, "T4", 1);
    ti.completeComposite(s, fields, /*packed=*/false, widths, noOffs, noAligns,
                         /*explicitAlign=*/0, /*maxFieldAlign=*/0, flags);
    auto const l = layoutOf(s, ti, kGnu16);
    ASSERT_EQ(l.fieldOffsets.size(), 4u);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[1], 1u);   // c@1, NOT @4            RED-ON-DISABLE
    EXPECT_EQ(l.align.bytes(), 4u);     // ALIGN-BLIND: control is 4 too
    EXPECT_EQ(l.size, 16u);             // SIZE-BLIND:  control is 16 too
}

TEST(PerMemberPackedLayout, AnUndecoratedBitfieldStillTakesTheStraddleBump) {
    auto ti = makeInterner(1);
    // ★★ THE GATE IS PER-FIELD, AND THIS IS THE PIN THAT SAYS SO. MEASURED:
    //   {unsigned char a; unsigned b:31; int c <pk>;} is 12 / align 4 / a@0 c@8 —
    //   BYTE-IDENTICAL to the undecorated control, so `b` still bumps to its own
    //   allocation unit even though a SIBLING carries packed. Folding the per-member
    //   flag into the struct-wide `packed` would suppress that bump and silently
    //   re-place a bit-field the programmer never annotated.
    std::array<TypeId, 3> const fields{ti.primitive(TypeKind::U8),
                                       ti.primitive(TypeKind::U32),
                                       ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 3> const widths{
        kNotBitfield, 31, kNotBitfield};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 0> const noAligns{};
    std::array<std::uint8_t, 3>  const flags{0, 0, 1};   // packed on `c` ONLY
    TypeId const s = ti.forwardComposite(TypeKind::Struct, "T2", 1);
    ti.completeComposite(s, fields, /*packed=*/false, widths, noOffs, noAligns,
                         /*explicitAlign=*/0, /*maxFieldAlign=*/0, flags);
    auto const l = layoutOf(s, ti, kGnu16);
    EXPECT_EQ(l.fieldOffsets[0], 0u);
    EXPECT_EQ(l.fieldOffsets[2], 8u);   // c@8 — the bump SURVIVED  RED-ON-DISABLE
    EXPECT_EQ(l.align.bytes(), 4u);
    EXPECT_EQ(l.size, 12u);
}

TEST(PerMemberPackedLayout, APackedBitfieldLowersItsOwnUnitAlignment) {
    auto ti = makeInterner(1);
    // MEASURED (gcc 13.3.0 x86_64 + aarch64, clang 18.1.3 x86_64 — all identical):
    //   {char a; unsigned b:20 <pk>;} 4 / align 1
    //   {char a; unsigned b:20;}      4 / align 4   (control)
    // SIZE-BLIND — sizeof is 4 either way and ONLY the alignment moves. This is the
    // per-field gate reaching a BIT-FIELD's storage unit, in the direction that does
    // not run into the straddle refusal below.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::U32)};
    std::array<std::int64_t, 2> const widths{kNotBitfield, 20};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 0> const noAligns{};
    std::array<std::uint8_t, 2>  const flags{0, 1};
    TypeId const s = ti.forwardComposite(TypeKind::Struct, "N1", 1);
    ti.completeComposite(s, fields, /*packed=*/false, widths, noOffs, noAligns,
                         /*explicitAlign=*/0, /*maxFieldAlign=*/0, flags);
    auto const l = layoutOf(s, ti, kGnu16);
    EXPECT_EQ(l.align.bytes(), 1u);   // RED-ON-DISABLE
    EXPECT_EQ(l.size, 4u);            // SIZE-BLIND

    std::array<TypeId, 2> const fields2{ti.primitive(TypeKind::Char),
                                        ti.primitive(TypeKind::U32)};
    std::array<std::int64_t, 2> const widths2{kNotBitfield, 20};
    TypeId const c = ti.structType("N1c", fields2, widths2);
    auto const lc = layoutOf(c, ti, kGnu16);
    EXPECT_EQ(lc.align.bytes(), 4u);
    EXPECT_EQ(lc.size, 4u);
}

TEST(PerMemberPackedLayout, PackedBitfieldStraddlerIsRefusedNotMislaid) {
    // ★★ A NAMED RESIDUE, NOT A SILENT GAP, AND NOT INTRODUCED HERE. Suppressing the
    // straddle bump lets a bit-field's bits FLOW THROUGH its allocation-unit
    // boundary, and this engine's placement model cannot EXPRESS a straddler — the
    // `bitInUnit + w > unitBits` guard REFUSES it (nullopt → a positioned
    // diagnostic at the caller). `#pragma pack(N)` already lands in exactly this
    // refusal for the same shape, by the same line, and that refusal is documented
    // as deliberate: with the bump left on, DSS would compute a wrong ABI and say
    // nothing. The per-field spelling reaches the SAME behaviour rather than a new
    // one.
    // MEASURED: gcc and clang lay `{unsigned a; unsigned long long b:40 <pk>;}` out
    // as 12 / align 4. DSS REFUSES it. That is a conformance gap DSS reports, not a
    // miscompile it hides — and it is the pre-existing straddle limitation, whose
    // closure belongs to the row that owns the placement model.
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::U32),
                                       ti.primitive(TypeKind::U64)};
    std::array<std::int64_t, 2> const widths{kNotBitfield, 40};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 0> const noAligns{};
    std::array<std::uint8_t, 2>  const flags{0, 1};
    TypeId const s = ti.forwardComposite(TypeKind::Struct, "T5", 1);
    ti.completeComposite(s, fields, /*packed=*/false, widths, noOffs, noAligns,
                         /*explicitAlign=*/0, /*maxFieldAlign=*/0, flags);
    EXPECT_FALSE(computeLayout(s, ti, kGnu16, DataModel::Lp64).has_value())
        << "a packed bit-field straddler must be REFUSED, never mislaid";

    // The CONTROL: without the flag the bump applies, the field fits its own unit,
    // and the layout succeeds at the reference's 16 / align 8.
    std::array<TypeId, 2> const fields2{ti.primitive(TypeKind::U32),
                                        ti.primitive(TypeKind::U64)};
    std::array<std::int64_t, 2> const widths2{kNotBitfield, 40};
    TypeId const c = ti.structType("T5c", fields2, widths2);
    auto const lc = layoutOf(c, ti, kGnu16);
    EXPECT_EQ(lc.align.bytes(), 8u);
    EXPECT_EQ(lc.size, 16u);
}

// ── the channel must survive every boundary a layout channel crosses ────────

TEST(PerMemberPackedLayout, PerFieldPackedSurvivesReintern) {
    // A per-field packed struct crossing a CU / round-trip boundary must come back
    // with the SAME offsets. Dropping the channel here is the exact silent ABI-merge
    // class `D-CSUBSET-PACKED` was about, made harder to see: the dropped form has
    // the same size AND the same alignment.
    auto src = makeInterner(1);
    std::array<TypeId, 3> const fields{src.primitive(TypeKind::Char),
                                       src.primitive(TypeKind::I32),
                                       src.primitive(TypeKind::F64)};
    std::array<std::uint8_t, 3> const flags{0, 1, 0};
    TypeId const s = perMemberPackedStruct(src, "Disc", 1, fields, flags);

    TypeLattice host{CompilationUnitId{2}};
    std::unordered_map<std::uint32_t, TypeId> remap;
    TypeId const r = reinternType(src, s, host, remap);
    TypeInterner const& dst = host.interner();

    EXPECT_TRUE(dst.hasFieldPacked(r));
    EXPECT_FALSE(dst.isFieldPacked(r, 0));
    EXPECT_TRUE(dst.isFieldPacked(r, 1));    // RED-ON-DISABLE
    EXPECT_FALSE(dst.isFieldPacked(r, 2));
    auto const l = layoutOf(r, dst);
    ASSERT_EQ(l.fieldOffsets.size(), 3u);
    EXPECT_EQ(l.fieldOffsets[1], 1u);        // the offset SURVIVED  RED-ON-DISABLE
    EXPECT_EQ(l.size, 16u);
    EXPECT_EQ(l.align.bytes(), 8u);
}

TEST(PerMemberPackedLayout, PackCapSurvivesReintern) {
    // ★★ NOT this row's channel — TF-C82's `#pragma pack(N)` cap, which was NOT
    // carried across reintern at base `01642ee3`. Found while threading the per-field
    // channel through the very list `type_reintern.cpp`'s own note says must hold
    // EVERY channel `completeComposite` accepts. A capped composite reinterned
    // UNCAPPED, and two composites under different caps merged onto one host type.
    // MEASURED: {char a; long long z;} under cap 4 is 12 / align 4 / z@4; uncapped
    // it is 16 / align 8 / z@8.
    auto src = makeInterner(1);
    std::array<TypeId, 2> const fields{src.primitive(TypeKind::Char),
                                       src.primitive(TypeKind::I64)};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 0> const noAligns{};
    std::array<std::uint8_t, 0>  const noFlags{};
    TypeId const s = src.forwardComposite(TypeKind::Struct, "Capped", 1);
    src.completeComposite(s, fields, /*packed=*/false, noWidths, noOffs, noAligns,
                          /*explicitAlign=*/0, /*maxFieldAlign=*/4, noFlags);
    EXPECT_EQ(src.maxFieldAlign(s), 4u);

    TypeLattice host{CompilationUnitId{2}};
    std::unordered_map<std::uint32_t, TypeId> remap;
    TypeId const r = reinternType(src, s, host, remap);
    TypeInterner const& dst = host.interner();

    EXPECT_EQ(dst.maxFieldAlign(r), 4u);   // RED-ON-DISABLE
    auto const l = layoutOf(r, dst);
    ASSERT_EQ(l.fieldOffsets.size(), 2u);
    EXPECT_EQ(l.fieldOffsets[1], 4u);      // z@4, not @8           RED-ON-DISABLE
    EXPECT_EQ(l.align.bytes(), 4u);
    EXPECT_EQ(l.size, 12u);
}

TEST(PerMemberPackedLayout, DifferentFlagVectorsDoNotMergeAcrossReintern) {
    // The identity signature must SEPARATE two composites that differ only in which
    // member is packed. Same name, same fields, same size, same alignment — only one
    // offset apart. If the signature ignored the flags these would merge onto one
    // host type and one of the two programs would get the other's ABI.
    auto srcA = makeInterner(1);
    std::array<TypeId, 3> const fa{srcA.primitive(TypeKind::Char),
                                   srcA.primitive(TypeKind::I32),
                                   srcA.primitive(TypeKind::F64)};
    std::array<std::uint8_t, 3> const flagsA{0, 1, 0};
    TypeId const a = perMemberPackedStruct(srcA, "S", 1, fa, flagsA);

    auto srcB = makeInterner(3);
    std::array<TypeId, 3> const fb{srcB.primitive(TypeKind::Char),
                                   srcB.primitive(TypeKind::I32),
                                   srcB.primitive(TypeKind::F64)};
    std::array<std::uint8_t, 3> const flagsB{0, 0, 0};   // nothing packed
    TypeId const b = perMemberPackedStruct(srcB, "S", 1, fb, flagsB);

    TypeLattice host{CompilationUnitId{2}};
    CompositeIdentityIndex index;
    index.observe(srcA);
    index.observe(srcB);
    std::unordered_map<std::uint32_t, TypeId> remapA;
    std::unordered_map<std::uint32_t, TypeId> remapB;
    TypeId const ra = reinternType(srcA, a, host, remapA, index);
    TypeId const rb = reinternType(srcB, b, host, remapB, index);
    TypeInterner const& dst = host.interner();

    EXPECT_NE(ra.v, rb.v) << "two different per-field packed layouts merged";
    EXPECT_EQ(layoutOf(ra, dst).fieldOffsets[1], 1u);   // RED-ON-DISABLE
    EXPECT_EQ(layoutOf(rb, dst).fieldOffsets[1], 4u);   // RED-ON-DISABLE
}

// ── fail-loud: the writer must never lose or half-carry the channel ─────────

TEST(PerMemberPackedLayout, NoFlagsIsByteIdenticalToTheUnchangedPath) {
    // Zero TypeId churn: a composite completed with an EMPTY flag span must intern
    // to the SAME TypeId as one built through the pre-channel overload. This is the
    // guard on the content-identity mix being GUARDED on non-empty.
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I32)};
    TypeId const plain = ti.structType("Z", fields);

    std::array<TypeId, 2> const fields2{ti.primitive(TypeKind::Char),
                                        ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 0> const noAligns{};
    std::array<std::uint8_t, 0>  const noFlags{};
    TypeId const viaNew = ti.structType("Z", fields2, noWidths, noOffs, noAligns,
                                        /*explicitAlign=*/0, /*maxFieldAlign=*/0,
                                        noFlags);
    EXPECT_EQ(plain.v, viaNew.v) << "an empty flag span must not fork the TypeId";
    EXPECT_FALSE(ti.hasFieldPacked(plain));
    EXPECT_FALSE(ti.isFieldPacked(plain, 0));
    EXPECT_FALSE(ti.isFieldPacked(plain, 99));   // out of range → unpacked, no abort
}

TEST(PerMemberPackedLayoutDeathTest, PartialFlagSpanIsRefusedNotSilentlyPadded) {
    // ALL-fields-or-NONE. A span one short would leave the LAST members unpacked
    // with nothing said — the same discipline `fieldOffsets` and `fieldAligns` are
    // held to, and the same reason.
    auto ti = makeInterner(1);
    std::array<TypeId, 3> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I32),
                                       ti.primitive(TypeKind::F64)};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 0> const noAligns{};
    std::array<std::uint8_t, 2>  const shortFlags{0, 1};   // 2 flags, 3 fields
    TypeId const s = ti.forwardComposite(TypeKind::Struct, "Short", 1);
    EXPECT_DEATH(
        ti.completeComposite(s, fields, /*packed=*/false, noWidths, noOffs, noAligns,
                             /*explicitAlign=*/0, /*maxFieldAlign=*/0, shortFlags),
        "per-field packed flags must cover every field");
}

TEST(PerMemberPackedLayoutDeathTest, FlagsWithExplicitOffsetsIsRefused) {
    // Explicit offsets place fields wholesale, so a per-field packed is as
    // contradictory with them as the whole-composite flag already is.
    auto ti = makeInterner(1);
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::Char),
                                       ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 2> const offs{0, 1};
    std::array<std::uint32_t, 0> const noAligns{};
    std::array<std::uint8_t, 2>  const flags{0, 1};
    TypeId const s = ti.forwardComposite(TypeKind::Struct, "Both", 1);
    EXPECT_DEATH(
        ti.completeComposite(s, fields, /*packed=*/false, noWidths, offs, noAligns,
                             /*explicitAlign=*/0, /*maxFieldAlign=*/0, flags),
        "per-field packed flags and explicit field offsets");
}
