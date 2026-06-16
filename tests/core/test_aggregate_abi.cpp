// FC7 (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): the by-value aggregate ABI classifier.
// Exhaustive SysV AMD64 eightbyte truth-table — every form built directly via the
// interner (incl. forms no shipped c-subset program reaches yet). A wrong
// classification = a SILENT MISCOMPILE (struct passed in the wrong registers), so
// these pin the EXACT (class, offset, width) of every eightbyte. AGNOSTIC: the
// engine switches on the AggregateClassKind strategy, never on a target name.

#include "core/types/aggregate_abi.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <gtest/gtest.h>

#include <array>

using namespace dss;

namespace {

// Shipped-target params: natural alignment, 16-byte ISA cap, LP64.
constexpr AggregateLayoutParams kNatural16{ScalarAlignmentRule::Natural, 16};

[[nodiscard]] TypeInterner makeInterner(std::uint32_t owner) {
    return TypeInterner{CompilationUnitId{owner}};
}

[[nodiscard]] std::optional<AbiPassing>
classifySysV(TypeId t, TypeInterner const& ti) {
    return classifyAggregate(AggregateClassKind::SysVEightbyte, 16, t, ti,
                             kNatural16, DataModel::Lp64);
}

[[nodiscard]] TypeId structOf(TypeInterner& ti, std::string_view name,
                              std::initializer_list<TypeId> fields) {
    std::vector<TypeId> v(fields);
    return ti.structType(name, v);
}

} // namespace

// {int,int} = 8 bytes = ONE eightbyte → one GPR piece (NOT two — size drives the
// eightbyte count; two ints pack into one 8-byte register).
TEST(AggregateAbiSysV, TwoInts_OneGprEightbyte) {
    auto ti = makeInterner(1);
    TypeId const i = ti.primitive(TypeKind::I32);
    auto r = classifySysV(structOf(ti, "II", {i, i}), ti);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::InRegisters);
    ASSERT_EQ(r->pieces.size(), 1u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Gpr);
    EXPECT_EQ(r->pieces[0].byteOffset, 0u);
    EXPECT_EQ(r->pieces[0].widthBytes, 8u);
}

// {long,long} = 16 bytes = TWO eightbytes → two GPR pieces.
TEST(AggregateAbiSysV, TwoLongs_TwoGprEightbytes) {
    auto ti = makeInterner(1);
    TypeId const l = ti.primitive(TypeKind::I64);
    auto r = classifySysV(structOf(ti, "LL", {l, l}), ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 2u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Gpr);
    EXPECT_EQ(r->pieces[0].byteOffset, 0u);
    EXPECT_EQ(r->pieces[0].widthBytes, 8u);
    EXPECT_EQ(r->pieces[1].cls, AbiPieceClass::Gpr);
    EXPECT_EQ(r->pieces[1].byteOffset, 8u);
    EXPECT_EQ(r->pieces[1].widthBytes, 8u);
}

// {int,float} = 8 bytes, one eightbyte holding an int(0..3) AND a float(4..7) →
// INTEGER wins the mixed eightbyte → one GPR piece.
TEST(AggregateAbiSysV, IntFloat_MixedEightbyteIsInteger) {
    auto ti = makeInterner(1);
    auto r = classifySysV(structOf(ti, "IF",
                 {ti.primitive(TypeKind::I32), ti.primitive(TypeKind::F32)}), ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 1u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Gpr) << "INTEGER wins a mixed eightbyte";
}

// {double,double} = 16 bytes → two SSE eightbytes → two FPR pieces.
TEST(AggregateAbiSysV, TwoDoubles_TwoFprEightbytes) {
    auto ti = makeInterner(1);
    TypeId const d = ti.primitive(TypeKind::F64);
    auto r = classifySysV(structOf(ti, "DD", {d, d}), ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 2u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Fpr);
    EXPECT_EQ(r->pieces[1].cls, AbiPieceClass::Fpr);
}

// {float,float} = 8 bytes, BOTH floats in eightbyte 0 → SSE → one FPR piece.
TEST(AggregateAbiSysV, TwoFloats_OneSseEightbyte) {
    auto ti = makeInterner(1);
    TypeId const f = ti.primitive(TypeKind::F32);
    auto r = classifySysV(structOf(ti, "FF", {f, f}), ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 1u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Fpr) << "all-float eightbyte is SSE";
}

// {double,int} = 16 bytes (double's align 8 rounds the struct up): eightbyte 0 =
// double (SSE→FPR), eightbyte 1 = int@8 + tail padding (INTEGER→GPR, width 8 —
// the FULL eightbyte is within the 16-byte struct). THE per-eightbyte
// discriminator — a whole-struct classification would get the FPR/GPR split wrong.
TEST(AggregateAbiSysV, DoubleInt_FprThenGprSplit) {
    auto ti = makeInterner(1);
    auto r = classifySysV(structOf(ti, "DI",
                 {ti.primitive(TypeKind::F64), ti.primitive(TypeKind::I32)}), ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 2u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Fpr);
    EXPECT_EQ(r->pieces[0].byteOffset, 0u);
    EXPECT_EQ(r->pieces[0].widthBytes, 8u);
    EXPECT_EQ(r->pieces[1].cls, AbiPieceClass::Gpr);
    EXPECT_EQ(r->pieces[1].byteOffset, 8u);
    EXPECT_EQ(r->pieces[1].widthBytes, 8u)
        << "struct rounds to 16 → the trailing eightbyte is a full 8 bytes";
}

// {int,int,int} = 12 bytes → [GPR@0 w8, GPR@8 w4] — the PARTIAL trailing eightbyte.
TEST(AggregateAbiSysV, ThreeInts_TrailingWidthFour) {
    auto ti = makeInterner(1);
    TypeId const i = ti.primitive(TypeKind::I32);
    auto r = classifySysV(structOf(ti, "III", {i, i, i}), ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 2u);
    EXPECT_EQ(r->pieces[0].widthBytes, 8u);
    EXPECT_EQ(r->pieces[1].byteOffset, 8u);
    EXPECT_EQ(r->pieces[1].widthBytes, 4u);
}

// struct { char a[5]; } = 5 bytes → one INTEGER eightbyte, width 5 — exercises the
// ARRAY-field leaf recursion + a non-power-of-2 trailing width.
TEST(AggregateAbiSysV, CharArrayFive_OneGprWidthFive) {
    auto ti = makeInterner(1);
    TypeId const arr = ti.array(ti.primitive(TypeKind::Char), 5);
    auto r = classifySysV(structOf(ti, "C5", {arr}), ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 1u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Gpr);
    EXPECT_EQ(r->pieces[0].widthBytes, 5u);
}

// struct { float a[2]; } = 8 bytes → one SSE eightbyte (both floats) → FPR.
TEST(AggregateAbiSysV, FloatArrayTwo_OneSseEightbyte) {
    auto ti = makeInterner(1);
    TypeId const arr = ti.array(ti.primitive(TypeKind::F32), 2);
    auto r = classifySysV(structOf(ti, "F2", {arr}), ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 1u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Fpr);
}

// 24 bytes (> 16) → MEMORY (by reference for args; sret for returns).
TEST(AggregateAbiSysV, TwentyFourBytes_ByReference) {
    auto ti = makeInterner(1);
    TypeId const l = ti.primitive(TypeKind::I64);
    auto r = classifySysV(structOf(ti, "LLL", {l, l, l}), ti);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::ByReference);
    EXPECT_TRUE(r->pieces.empty());
}

// Nested: struct Outer { struct Inner{double d;} in; int z; } = 16 bytes →
// [FPR@0 (the nested double), GPR@8 w4 (z)] — the leaf walk descends nested structs.
TEST(AggregateAbiSysV, NestedStructDouble_RecursesToFprThenGpr) {
    auto ti = makeInterner(1);
    TypeId const inner = structOf(ti, "Inner", {ti.primitive(TypeKind::F64)});
    auto r = classifySysV(structOf(ti, "Outer",
                 {inner, ti.primitive(TypeKind::I32)}), ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 2u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Fpr) << "nested double → SSE eightbyte";
    EXPECT_EQ(r->pieces[1].cls, AbiPieceClass::Gpr);
}

// AGNOSTICISM / strategy-availability: an unimplemented strategy classifies to
// nullopt (the caller fails loud); only SysV is implemented in C1.
TEST(AggregateAbiSysV, UnimplementedStrategyIsNullopt) {
    auto ti = makeInterner(1);
    TypeId const s = structOf(ti, "II",
                 {ti.primitive(TypeKind::I32), ti.primitive(TypeKind::I32)});
    EXPECT_FALSE(classifyAggregate(AggregateClassKind::None, 16, s, ti,
                                   kNatural16, DataModel::Lp64).has_value());
    EXPECT_FALSE(classifyAggregate(AggregateClassKind::Win64BySize, 8, s, ti,
                                   kNatural16, DataModel::Lp64).has_value());
    EXPECT_FALSE(classifyAggregate(AggregateClassKind::Aapcs64Hfa, 16, s, ti,
                                   kNatural16, DataModel::Lp64).has_value());
}

TEST(AggregateAbiSysV, ImplementedFlagMatchesC1Scope) {
    EXPECT_TRUE(aggregateAbiImplemented(AggregateClassKind::SysVEightbyte));
    EXPECT_FALSE(aggregateAbiImplemented(AggregateClassKind::None));
    EXPECT_FALSE(aggregateAbiImplemented(AggregateClassKind::Win64BySize));
    EXPECT_FALSE(aggregateAbiImplemented(AggregateClassKind::Aapcs64Hfa));
}
