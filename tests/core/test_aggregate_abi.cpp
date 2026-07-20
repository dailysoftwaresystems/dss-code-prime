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

// D-CSUBSET-LONG-DOUBLE-AGGREGATE-ABI (LD-4): a long-double (F80|F128) LEAF forces
// the SysV x87 MEMORY class → the WHOLE aggregate goes BY REFERENCE (X87/X87UP →
// MEMORY, regardless of size). INVERTED from the FC17.9(e) nullopt-fail-loud pin:
// the real rule is now modeled. NOT the SSE/INTEGER eightbyte merge (a float-kind
// join would say SSE → XMM pieces; a non-join says INTEGER = a silent 2-GPR pass,
// ABI-divergent at FFI). Inherits the pre-existing D-FC7 hidden-pointer path.
TEST(AggregateAbiSysV, LongDoubleLeafStructIsByReference) {
    auto ti = makeInterner(1);
    auto r80 = classifySysV(structOf(ti, "LD", {ti.primitive(TypeKind::F80)}), ti);
    ASSERT_TRUE(r80.has_value());
    EXPECT_EQ(r80->kind, AbiPassing::Kind::ByReference)
        << "an F80 (x87 long double) leaf → the SysV MEMORY class (ByReference)";
    EXPECT_TRUE(r80->pieces.empty())
        << "a ByReference pass carries NO register pieces (a hidden pointer)";
    auto r128 = classifySysV(structOf(ti, "LQ", {ti.primitive(TypeKind::F128)}), ti);
    ASSERT_TRUE(r128.has_value());
    EXPECT_EQ(r128->kind, AbiPassing::Kind::ByReference)
        << "an F128 (binary128) leaf → MEMORY too (X87/X87UP → the whole agg)";
    // Nested: the leaf walk must see THROUGH an inner struct.
    TypeId const inner = structOf(ti, "In", {ti.primitive(TypeKind::F80)});
    auto nested = classifySysV(structOf(ti, "Out", {inner}), ti);
    ASSERT_TRUE(nested.has_value());
    EXPECT_EQ(nested->kind, AbiPassing::Kind::ByReference)
        << "a NESTED F80 leaf → MEMORY — the check is leaf-deep";
}

// D-CSUBSET-LONG-DOUBLE-AGGREGATE-ABI (LD-4): AAPCS64 — a binary128 (F128) leaf is
// a legitimate HFA member (a 16-byte Q-register piece), so it now CLASSIFIES to an
// HFA (INVERTED from the FC17.9(e) nullopt pin). An F80 (x87) leaf still refuses
// (no AAPCS64 x87 register class) — see F80LeafStructStillRefuses below.
TEST(AggregateAbiAapcs64, Binary128LeafStructIsHfaFpr) {
    auto ti = makeInterner(1);
    // Single binary128 member → a 1-element HFA in ONE Q-register (v0): a 16-byte
    // Fpr piece at offset 0 (NEVER an 8-byte piece — that would move 8 of 16 bytes).
    TypeId const s1 = structOf(ti, "LQ", {ti.primitive(TypeKind::F128)});
    auto r1 = classifyAggregate(AggregateClassKind::Aapcs64Hfa, 16, s1, ti,
                                kNatural16, DataModel::Lp64);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->kind, AbiPassing::Kind::InRegisters);
    ASSERT_EQ(r1->pieces.size(), 1u);
    EXPECT_EQ(r1->pieces[0].cls, AbiPieceClass::Fpr);
    EXPECT_EQ(r1->pieces[0].byteOffset, 0u);
    EXPECT_EQ(r1->pieces[0].widthBytes, 16u)
        << "a binary128 HFA piece is 16 bytes (a full Q-register), never 8";
    // Two binary128 members → a 2-element HFA in v0/v1: two 16-byte Fpr pieces.
    TypeId const s2 = structOf(ti, "LQ2",
        {ti.primitive(TypeKind::F128), ti.primitive(TypeKind::F128)});
    auto r2 = classifyAggregate(AggregateClassKind::Aapcs64Hfa, 16, s2, ti,
                                kNatural16, DataModel::Lp64);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->kind, AbiPassing::Kind::InRegisters);
    ASSERT_EQ(r2->pieces.size(), 2u) << "a 2-binary128 aggregate = a 2-piece HFA";
    EXPECT_EQ(r2->pieces[0].cls, AbiPieceClass::Fpr);
    EXPECT_EQ(r2->pieces[0].byteOffset, 0u);
    EXPECT_EQ(r2->pieces[0].widthBytes, 16u);
    EXPECT_EQ(r2->pieces[1].cls, AbiPieceClass::Fpr);
    EXPECT_EQ(r2->pieces[1].byteOffset, 16u);
    EXPECT_EQ(r2->pieces[1].widthBytes, 16u);
}

TEST(AggregateAbiAapcs64, F80LeafStructStillRefuses) {
    auto ti = makeInterner(1);
    TypeId const s = structOf(ti, "LD", {ti.primitive(TypeKind::F80)});
    EXPECT_FALSE(classifyAggregate(AggregateClassKind::Aapcs64Hfa, 16, s, ti,
                                   kNatural16, DataModel::Lp64).has_value())
        << "AAPCS64: an x87 F80 leaf has no fundamental-FP register class — must "
           "refuse (F80 only arises from a hand-built type here; arm64 long double "
           "is F128), never emit an 8-byte FPR piece over a 16-byte x87 value";
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

// C99 _Complex (D-CSUBSET-COMPLEX / D10): `double _Complex` = collectLeaves emits 2
// F64 leaves @0/@8 → SysV classes it EXACTLY like struct{double,double} → 2 FPR
// eightbytes. This is the RED-ON-DISABLE for the collectLeaves Complex arm: WITHOUT
// it, the bare-Complex default emits ONE zero-size non-float leaf → the eightbyte
// mis-classes INTEGER (a silent 2-GPR by-value pass, ABI-divergent at FFI).
TEST(AggregateAbiSysV, DoubleComplex_TwoFprEightbytes) {
    auto ti = makeInterner(1);
    TypeId const cd = ti.complex(ti.primitive(TypeKind::F64));
    auto r = classifySysV(cd, ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 2u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Fpr);
    EXPECT_EQ(r->pieces[0].byteOffset, 0u);
    EXPECT_EQ(r->pieces[1].cls, AbiPieceClass::Fpr);
    EXPECT_EQ(r->pieces[1].byteOffset, 8u) << "imag leaf must sit at elemSize=8";
    // float _Complex = 8 bytes, both F32 in eightbyte 0 → ONE SSE eightbyte.
    TypeId const cf = ti.complex(ti.primitive(TypeKind::F32));
    auto rf = classifySysV(cf, ti);
    ASSERT_TRUE(rf.has_value());
    ASSERT_EQ(rf->pieces.size(), 1u);
    EXPECT_EQ(rf->pieces[0].cls, AbiPieceClass::Fpr);
}

// D-CSUBSET-LONG-DOUBLE-AGGREGATE-ABI (LD-4): long double _Complex (F80/F128
// element). A `_Complex` is ABI-passed as struct{re, im} (2 leaves). On AAPCS64 a
// binary128 _Complex → 2 binary128 leaves @0/@16 → a 2-element HFA (v0/v1, two
// 16-byte Fpr pieces) — INVERTED from the FC17.9(e) refusal. An F80-element complex
// → 2 F80 leaves → still refuses (no AAPCS64 x87 register class). On SysV a 32-byte
// long-double-complex is MEMORY (ByReference) by the >16B rule (which runs first) —
// a VALID by-reference pass (the value moves; VALUE arithmetic walls at the ops).
TEST(AggregateAbiAapcs64, Binary128ComplexIsTwoPieceHfa) {
    auto ti = makeInterner(1);
    TypeId const cq = ti.complex(ti.primitive(TypeKind::F128));
    auto r = classifyAggregate(AggregateClassKind::Aapcs64Hfa, 16, cq, ti,
                               kNatural16, DataModel::Lp64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::InRegisters);
    ASSERT_EQ(r->pieces.size(), 2u) << "a binary128 _Complex = a 2-element HFA";
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Fpr);
    EXPECT_EQ(r->pieces[0].byteOffset, 0u);
    EXPECT_EQ(r->pieces[0].widthBytes, 16u);
    EXPECT_EQ(r->pieces[1].cls, AbiPieceClass::Fpr);
    EXPECT_EQ(r->pieces[1].byteOffset, 16u) << "imag leaf at elemSize=16";
    EXPECT_EQ(r->pieces[1].widthBytes, 16u);
}

TEST(AggregateAbiAapcs64, F80ComplexStillRefusesAndSysVIsByReference) {
    auto ti = makeInterner(1);
    TypeId const cld = ti.complex(ti.primitive(TypeKind::F80));
    EXPECT_FALSE(classifyAggregate(AggregateClassKind::Aapcs64Hfa, 16, cld, ti,
                                   kNatural16, DataModel::Lp64).has_value())
        << "AAPCS64: an F80-element complex has F80 leaves → must refuse (no x87 "
           "register class), never a guessed piece";
    // SysV: a 32-byte long-double-complex is MEMORY (ByReference) — the >16B rule
    // runs first (the value moves; arithmetic walls elsewhere).
    auto sysv = classifySysV(cld, ti);
    ASSERT_TRUE(sysv.has_value());
    EXPECT_EQ(sysv->kind, AbiPassing::Kind::ByReference);
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
    // Only the `None` sentinel is unimplemented → nullopt (the caller fails
    // loud). SysV (C1), Win64 (C2), AAPCS64 (C3) are all implemented now.
    EXPECT_FALSE(classifyAggregate(AggregateClassKind::None, 16, s, ti,
                                   kNatural16, DataModel::Lp64).has_value());
}

TEST(AggregateAbiSysV, ImplementedFlagMatchesC1C2C3Scope) {
    EXPECT_TRUE(aggregateAbiImplemented(AggregateClassKind::SysVEightbyte));
    EXPECT_TRUE(aggregateAbiImplemented(AggregateClassKind::Win64BySize));   // C2
    EXPECT_TRUE(aggregateAbiImplemented(AggregateClassKind::Aapcs64Hfa));    // C3
    EXPECT_FALSE(aggregateAbiImplemented(AggregateClassKind::None));
}

// ── FC7 C2: MS x64 (Win64) by-size classification ──────────────────────────
// A struct is passed/returned in ONE GPR iff its size is a power of two ≤ 8
// (1/2/4/8); EVERY other size (3/5/6/7 or > 8) goes BY REFERENCE. Win64 has no
// SSE/HFA rule — a small aggregate is an integer of its size, float members or
// not. maxRegBytes = 8 (a single register).
namespace {
[[nodiscard]] std::optional<AbiPassing>
classifyWin64(TypeId t, TypeInterner const& ti) {
    return classifyAggregate(AggregateClassKind::Win64BySize, 8, t, ti,
                             kNatural16, DataModel::Lp64);
}
}  // namespace

TEST(AggregateAbiWin64, EightByteStruct_OneGprPiece) {
    auto ti = makeInterner(1);
    TypeId const i = ti.primitive(TypeKind::I32);
    auto r = classifyWin64(structOf(ti, "II", {i, i}), ti);  // 8 bytes
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::InRegisters);
    ASSERT_EQ(r->pieces.size(), 1u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Gpr);
    EXPECT_EQ(r->pieces[0].byteOffset, 0u);
    EXPECT_EQ(r->pieces[0].widthBytes, 8u);
}

TEST(AggregateAbiWin64, FourByteFloatStruct_OneGprNotSse) {
    auto ti = makeInterner(1);
    // {float} = 4 bytes → ONE GPR (Win64 treats it as a 4-byte integer; NO SSE,
    // unlike SysV which would put a float-only eightbyte in an FPR).
    auto r = classifyWin64(structOf(ti, "F", {ti.primitive(TypeKind::F32)}), ti);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::InRegisters);
    ASSERT_EQ(r->pieces.size(), 1u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Gpr)
        << "Win64 passes a small float-only struct in a GPR, not an FPR";
    EXPECT_EQ(r->pieces[0].widthBytes, 4u);
}

TEST(AggregateAbiWin64, ThreeByteStruct_ByReference) {
    auto ti = makeInterner(1);
    TypeId const c = ti.primitive(TypeKind::Char);
    // {char,char,char} = 3 bytes — NOT a power of two → BY REFERENCE.
    auto r = classifyWin64(structOf(ti, "CCC", {c, c, c}), ti);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::ByReference)
        << "a 3-byte struct is not power-of-two → by reference on Win64";
}

TEST(AggregateAbiWin64, TwelveByteStruct_ByReference) {
    auto ti = makeInterner(1);
    TypeId const i = ti.primitive(TypeKind::I32);
    // {int,int,int} = 12 bytes — > 8 → BY REFERENCE (SysV would split into two
    // eightbytes; Win64 always passes > 8 by reference).
    auto r = classifyWin64(structOf(ti, "III", {i, i, i}), ti);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::ByReference);
}

TEST(AggregateAbiWin64, OneByteStruct_OneGprWidthOne) {
    auto ti = makeInterner(1);
    auto r = classifyWin64(structOf(ti, "C", {ti.primitive(TypeKind::Char)}), ti);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::InRegisters);
    ASSERT_EQ(r->pieces.size(), 1u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Gpr);
    EXPECT_EQ(r->pieces[0].widthBytes, 1u);
}

// ── FC7 C3: AAPCS64 / Apple arm64 classification ───────────────────────────
// HFA (1-4 leaves all the SAME FP type) → that many FPR pieces, each the element
// width (NOT size-limited to 16B). Non-HFA ≤16B → 1-2 GPR pieces; >16B → by-ref.
namespace {
[[nodiscard]] std::optional<AbiPassing>
classifyAapcs64(TypeId t, TypeInterner const& ti) {
    return classifyAggregate(AggregateClassKind::Aapcs64Hfa, 16, t, ti,
                             kNatural16, DataModel::Lp64);
}
}  // namespace

TEST(AggregateAbiAapcs64, TwoDoubles_TwoFprPiecesWidth8) {
    auto ti = makeInterner(1);
    TypeId const d = ti.primitive(TypeKind::F64);
    auto r = classifyAapcs64(structOf(ti, "DD", {d, d}), ti);  // 16B HFA
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::InRegisters);
    ASSERT_EQ(r->pieces.size(), 2u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Fpr);
    EXPECT_EQ(r->pieces[0].byteOffset, 0u);
    EXPECT_EQ(r->pieces[0].widthBytes, 8u);
    EXPECT_EQ(r->pieces[1].cls, AbiPieceClass::Fpr);
    EXPECT_EQ(r->pieces[1].byteOffset, 8u);
    EXPECT_EQ(r->pieces[1].widthBytes, 8u);
}

TEST(AggregateAbiAapcs64, ThreeFloats_ThreeFprPiecesWidth4) {
    auto ti = makeInterner(1);
    TypeId const f = ti.primitive(TypeKind::F32);
    auto r = classifyAapcs64(structOf(ti, "FFF", {f, f, f}), ti);  // 12B float HFA
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(r->pieces[i].cls, AbiPieceClass::Fpr);
        EXPECT_EQ(r->pieces[i].byteOffset, i * 4u);
        EXPECT_EQ(r->pieces[i].widthBytes, 4u)
            << "a float HFA element is 4 bytes (s-register), NOT a truncated 8";
    }
}

TEST(AggregateAbiAapcs64, FourDoubles_FourFprPieces_NotByRefDespite32Bytes) {
    auto ti = makeInterner(1);
    TypeId const d = ti.primitive(TypeKind::F64);
    auto r = classifyAapcs64(structOf(ti, "DDDD", {d, d, d, d}), ti);  // 32B HFA
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::InRegisters)
        << "a 4-double HFA is 32 bytes but passes in v0..v3, NOT by reference";
    ASSERT_EQ(r->pieces.size(), 4u);
    EXPECT_EQ(r->pieces[3].cls, AbiPieceClass::Fpr);
    EXPECT_EQ(r->pieces[3].byteOffset, 24u);
}

TEST(AggregateAbiAapcs64, SingleFloat_OneFprPiece) {
    auto ti = makeInterner(1);
    auto r = classifyAapcs64(structOf(ti, "F", {ti.primitive(TypeKind::F32)}), ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 1u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Fpr);
    EXPECT_EQ(r->pieces[0].widthBytes, 4u);
}

TEST(AggregateAbiAapcs64, ThreeInts_TwoGprPieces) {
    auto ti = makeInterner(1);
    TypeId const i = ti.primitive(TypeKind::I32);
    auto r = classifyAapcs64(structOf(ti, "III", {i, i, i}), ti);  // 12B non-HFA
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::InRegisters);
    ASSERT_EQ(r->pieces.size(), 2u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Gpr);
    EXPECT_EQ(r->pieces[1].cls, AbiPieceClass::Gpr);
}

TEST(AggregateAbiAapcs64, IntFloatMixed_NotHfa_OneGpr) {
    auto ti = makeInterner(1);
    // {int, float} = 8B — leaves are I32 + F32, NOT all the same FP type → NOT an
    // HFA → general rule → 1 GPR piece (NOT 1 FPR).
    auto r = classifyAapcs64(structOf(ti, "IF",
                 {ti.primitive(TypeKind::I32), ti.primitive(TypeKind::F32)}), ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 1u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Gpr)
        << "a mixed int/float aggregate is not an HFA → GPR, not FPR";
}

TEST(AggregateAbiAapcs64, TwentyByteStruct_ByReference) {
    auto ti = makeInterner(1);
    TypeId const i = ti.primitive(TypeKind::I32);
    TypeId const l = ti.primitive(TypeKind::I64);
    // {int, long, long} = 24B (> 16, non-HFA) → by reference.
    auto r = classifyAapcs64(structOf(ti, "ILL", {i, l, l}), ti);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::ByReference);
}

// ── FC12a-core (D-FC12A-VARIADIC-CALLEE): va_arg scalar-class pins ────────────
//
// `va_arg(ap, T)` for a SCALAR T routes by the SAME SysV eightbyte class the
// by-value classifier assigns: an INTEGER scalar reads the gp_offset cursor / the
// GPR half of the register-save-area; an SSE scalar reads fp_offset / the XMM half.
// These pin that contract at the classifier (the single-eightbyte forms a scalar
// `int` / `double` occupy), so a wrong-class regression — which would make va_arg
// read the WRONG save-area half (a silent garbage-vararg miscompile) — fails here.
// (The FC12a-core lowering classifies scalars via the lighter `scalarArgClass`; this
// engine is the by-value path the FC12a-struct follow-on will reuse for the same T.)

// `int` (4B) → ONE eightbyte, INTEGER → GPR (gp_offset / integer save area).
TEST(AggregateAbiSysV, VaArgInt_ClassifiesGpr) {
    auto ti = makeInterner(1);
    TypeId const i = ti.primitive(TypeKind::I32);
    auto r = classifySysV(structOf(ti, "Wi", {i}), ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 1u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Gpr)
        << "va_arg(ap,int) must read the INTEGER (gp_offset) save-area half";
}

// `double` (8B) → ONE eightbyte, SSE → FPR (fp_offset / XMM save area).
TEST(AggregateAbiSysV, VaArgDouble_ClassifiesFpr) {
    auto ti = makeInterner(1);
    TypeId const d = ti.primitive(TypeKind::F64);
    auto r = classifySysV(structOf(ti, "Wd", {d}), ti);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pieces.size(), 1u);
    EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Fpr)
        << "va_arg(ap,double) must read the SSE (fp_offset) save-area half — NOT "
           "the GPR half (the silent-miscompile this pins)";
}

// D-CSUBSET-BITINT-C2-WIDE: a RAW wide `_BitInt(N>64)` classifies BY VALUE exactly
// like a fieldless aggregate of its byte size (collectLeaves emits ONE leaf for the
// whole width). A wrong classification silently passes the multi-limb value in the
// wrong registers, so these pin the (kind, piece) truth on all three shipped ABIs.
TEST(AggregateAbiBitInt, WideBitInt128AndBitInt200AllStrategies) {
    auto ti = makeInterner(9);
    TypeId const b128 = ti.bitInt(128, true);   // 16 bytes = two eightbytes
    TypeId const b200 = ti.bitInt(200, false);  // 32 bytes  > 16

    // SysV: 128b → two INTEGER eightbytes (2 GPRs); 200b → MEMORY (by reference).
    {
        auto r = classifySysV(b128, ti);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->kind, AbiPassing::Kind::InRegisters);
        ASSERT_EQ(r->pieces.size(), 2u);
        EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Gpr);
        EXPECT_EQ(r->pieces[1].cls, AbiPieceClass::Gpr);
        EXPECT_EQ(r->pieces[1].byteOffset, 8u);
        EXPECT_EQ(classifySysV(b200, ti)->kind, AbiPassing::Kind::ByReference);
    }
    // Win64: a 16-byte value is NOT a power-of-two ≤ 8 → by reference; 200b likewise.
    {
        EXPECT_EQ(classifyWin64(b128, ti)->kind, AbiPassing::Kind::ByReference);
        EXPECT_EQ(classifyWin64(b200, ti)->kind, AbiPassing::Kind::ByReference);
    }
    // AAPCS64: 128b is a non-HFA ≤16B → two GPRs; 200b >16B → by reference.
    {
        auto r = classifyAapcs64(b128, ti);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->kind, AbiPassing::Kind::InRegisters);
        ASSERT_EQ(r->pieces.size(), 2u);
        EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Gpr);
        EXPECT_EQ(r->pieces[1].cls, AbiPieceClass::Gpr);
        EXPECT_EQ(classifyAapcs64(b200, ti)->kind, AbiPassing::Kind::ByReference);
    }
}
