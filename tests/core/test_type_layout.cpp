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
