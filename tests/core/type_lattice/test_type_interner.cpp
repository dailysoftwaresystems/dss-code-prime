// SP2: TypeInterner canonicalization + cross-CU TypeId isolation.

#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <set>
#include <type_traits>

using namespace dss;

namespace {
[[nodiscard]] TypeInterner makeInterner(std::uint32_t owner) {
    return TypeInterner{CompilationUnitId{owner}};
}
} // namespace

TEST(TypeInterner, PrimitivesCanonicalize) {
    auto ti = makeInterner(1);
    EXPECT_EQ(ti.size(), 0u);
    const TypeId f32a = ti.primitive(TypeKind::F32);
    const TypeId f32b = ti.primitive(TypeKind::F32);
    const TypeId i32  = ti.primitive(TypeKind::I32);
    EXPECT_EQ(f32a.v, f32b.v);            // same primitive → one TypeId
    EXPECT_NE(f32a.v, i32.v);
    EXPECT_EQ(ti.kind(f32a), TypeKind::F32);
    EXPECT_EQ(ti.size(), 2u);             // f32 + i32 (not 3)
    EXPECT_EQ(f32a.arenaTag, 1u);          // carries owner-CU provenance
}

TEST(TypeInterner, VectorOfF32x4InternsToOneTypeId) {
    auto ti = makeInterner(1);
    const TypeId f32 = ti.primitive(TypeKind::F32);
    const TypeId v1  = ti.vector(f32, 4);
    const TypeId v2  = ti.vector(f32, 4);
    EXPECT_EQ(v1.v, v2.v);                                       // canonical
    EXPECT_NE(v1.v, ti.vector(f32, 2).v);                        // lanes matter
    EXPECT_NE(v1.v, ti.vector(ti.primitive(TypeKind::I32), 4).v); // element matters
    EXPECT_EQ(ti.kind(v1), TypeKind::Vector);
    ASSERT_EQ(ti.operands(v1).size(), 1u);
    EXPECT_EQ(ti.operands(v1)[0].v, f32.v);
    ASSERT_EQ(ti.scalars(v1).size(), 1u);
    EXPECT_EQ(ti.scalars(v1)[0], 4);
}

TEST(TypeInterner, StructIsNominalAndStructural) {
    auto ti = makeInterner(1);
    const TypeId i32 = ti.primitive(TypeKind::I32);
    const TypeId f32 = ti.primitive(TypeKind::F32);
    std::array<TypeId, 1> const intField{i32};
    std::array<TypeId, 1> const floatField{f32};
    const TypeId fooIntA = ti.structType("Foo", intField);
    const TypeId fooIntB = ti.structType("Foo", intField);
    const TypeId fooFloat = ti.structType("Foo", floatField);   // same name, other fields
    const TypeId bar      = ti.structType("Bar", intField);     // other name, same fields
    EXPECT_EQ(fooIntA.v, fooIntB.v);
    EXPECT_NE(fooIntA.v, fooFloat.v);
    EXPECT_NE(fooIntA.v, bar.v);
    EXPECT_EQ(ti.name(fooIntA), "Foo");
    EXPECT_EQ(ti.name(i32), "");          // structural primitives have no name
}

TEST(TypeInterner, FnSigEncodesResultParamsAndCc) {
    auto ti = makeInterner(1);
    const TypeId i32 = ti.primitive(TypeKind::I32);
    const TypeId f64 = ti.primitive(TypeKind::F64);
    std::array<TypeId, 2> const params{i32, f64};
    const TypeId sig = ti.fnSig(params, i32, CallConv::CcSysV);
    EXPECT_EQ(ti.kind(sig), TypeKind::FnSig);
    // Decode via the typed accessors (callers never hand-decode the operand
    // layout).
    EXPECT_EQ(ti.fnResult(sig).v, i32.v);
    auto const ps = ti.fnParams(sig);
    ASSERT_EQ(ps.size(), 2u);
    EXPECT_EQ(ps[0].v, i32.v);
    EXPECT_EQ(ps[1].v, f64.v);
    ASSERT_EQ(ti.scalars(sig).size(), 1u);
    EXPECT_EQ(ti.scalars(sig)[0], static_cast<std::int64_t>(CallConv::CcSysV));
    EXPECT_EQ(sig.v, ti.fnSig(params, i32, CallConv::CcSysV).v);          // canonical
    EXPECT_NE(sig.v, ti.fnSig(params, i32, CallConv::CcMS64).v);          // cc matters

    // A zero-param signature is distinct and decodes to empty params.
    const TypeId thunk = ti.fnSig({}, i32, CallConv::CcSysV);
    EXPECT_NE(thunk.v, sig.v);
    EXPECT_EQ(ti.fnResult(thunk).v, i32.v);
    EXPECT_TRUE(ti.fnParams(thunk).empty());
}

TEST(TypeInterner, ExtensionRecordsCarryKindAndArgs) {
    auto ti = makeInterner(1);
    const TypeKindId varchar{kFirstExtensionKind};   // a registry-minted kind
    std::array<std::int64_t, 1> const len{20};
    const TypeId v = ti.extension(varchar, "TSQL::Varchar", {}, len);
    EXPECT_EQ(ti.kind(v), TypeKind::Extension);
    EXPECT_EQ(ti.get(v).extensionKind.v, varchar.v);
    EXPECT_EQ(ti.name(v), "TSQL::Varchar");
    ASSERT_EQ(ti.scalars(v).size(), 1u);
    EXPECT_EQ(ti.scalars(v)[0], 20);
    EXPECT_EQ(v.v, ti.extension(varchar, "TSQL::Varchar", {}, len).v);  // canonical
}

TEST(TypeInterner, AllBuildersProduceDistinctKindsAndCanonicalize) {
    auto ti = makeInterner(1);
    const TypeId t = ti.primitive(TypeKind::I32);
    // The five single-operand indirections share an identical builder body but
    // for the kind — guard against a copy-paste kind mix-up.
    const TypeId ptr  = ti.pointer(t);
    const TypeId ref  = ti.reference(t);
    const TypeId nul  = ti.nullable(t);
    const TypeId opt  = ti.optional(t);
    const TypeId slc  = ti.slice(t);
    EXPECT_EQ(ti.kind(ptr), TypeKind::Ptr);
    EXPECT_EQ(ti.kind(ref), TypeKind::Ref);
    EXPECT_EQ(ti.kind(nul), TypeKind::Nullable);
    EXPECT_EQ(ti.kind(opt), TypeKind::Optional);
    EXPECT_EQ(ti.kind(slc), TypeKind::Slice);
    std::set<std::uint32_t> const distinct{ptr.v, ref.v, nul.v, opt.v, slc.v};
    EXPECT_EQ(distinct.size(), 5u);                 // all distinct
    EXPECT_EQ(ptr.v, ti.pointer(t).v);              // each canonicalizes

    // matrix: scalars = [rows, cols] — order matters.
    const TypeId m23 = ti.matrix(t, 2, 3);
    EXPECT_EQ(ti.kind(m23), TypeKind::Matrix);
    EXPECT_NE(m23.v, ti.matrix(t, 3, 2).v);
    ASSERT_EQ(ti.scalars(m23).size(), 2u);
    EXPECT_EQ(ti.scalars(m23)[0], 2);
    EXPECT_EQ(ti.scalars(m23)[1], 3);

    // array length discriminates; slice has no scalar.
    EXPECT_NE(ti.array(t, 4).v, ti.array(t, 8).v);
    EXPECT_EQ(ti.array(t, 4).v, ti.array(t, 4).v);
    EXPECT_TRUE(ti.scalars(slc).empty());

    // tuple: operand order matters.
    const TypeId f32 = ti.primitive(TypeKind::F32);
    std::array<TypeId, 2> const ab{t, f32};
    std::array<TypeId, 2> const ba{f32, t};
    EXPECT_NE(ti.tuple(ab).v, ti.tuple(ba).v);
    EXPECT_EQ(ti.tuple(ab).v, ti.tuple(ab).v);

    // union is nominal like struct.
    EXPECT_EQ(ti.kind(ti.unionType("U", ab)), TypeKind::Union);
    EXPECT_NE(ti.unionType("U", ab).v, ti.unionType("V", ab).v);
}

TEST(TypeInterner, CrossKindStructuralCollisionStaysDistinct) {
    auto ti = makeInterner(1);
    const TypeId t = ti.primitive(TypeKind::I32);
    // Array<T,4> and Vector<T,4> have identical operands [T] + scalars [4] but
    // different kinds — they must NOT canonicalize together (guards a
    // regression that drops `kind` from the structural key).
    EXPECT_NE(ti.array(t, 4).v, ti.vector(t, 4).v);
    // Ptr<T> / Ref<T> / Slice<T>: identical operand, no scalars, kind only.
    EXPECT_NE(ti.pointer(t).v, ti.slice(t).v);
}

TEST(TypeInterner, ExtensionDedupDiscriminatesKindAndArgs) {
    auto ti = makeInterner(1);
    const TypeKindId k256{kFirstExtensionKind};
    const TypeKindId k257{kFirstExtensionKind + 1};
    std::array<std::int64_t, 1> const a20{20};
    std::array<std::int64_t, 1> const a21{21};
    const TypeId base = ti.extension(k256, "Foo", {}, a20);
    EXPECT_EQ(base.v, ti.extension(k256, "Foo", {}, a20).v);              // canonical
    EXPECT_NE(base.v, ti.extension(k257, "Foo", {}, a20).v);             // kind differs
    EXPECT_NE(base.v, ti.extension(k256, "Foo", {}, a21).v);             // scalar arg differs
}

TEST(TypeInterner, SizeUnchangedOnReIntern) {
    auto ti = makeInterner(1);
    const TypeId f32 = ti.primitive(TypeKind::F32);
    const std::size_t before = ti.size();
    (void)ti.vector(f32, 4);
    const std::size_t after = ti.size();
    (void)ti.vector(f32, 4);              // re-intern identical → no growth
    EXPECT_EQ(ti.size(), after);
    EXPECT_EQ(after, before + 1);          // one new type (the vector)
}

TEST(TypeInterner, MoveOnly) {
    static_assert(!std::is_copy_constructible_v<TypeInterner>);
    static_assert(std::is_move_constructible_v<TypeInterner>);
}

TEST(TypeInternerDeathTest, ForeignCuTypeIdAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto a = makeInterner(111);
    auto b = makeInterner(222);
    // Populate b so the foreign id is in-bounds and the tag (not bounds) check
    // is what trips.
    b.primitive(TypeKind::I32);
    b.primitive(TypeKind::F32);
    const TypeId fromA = a.primitive(TypeKind::I32);   // tagged owner 111
    EXPECT_DEATH({ (void)b.get(fromA); },
                 "TypeId from CompilationUnitId=111 used on CompilationUnitId=222");
}

TEST(TypeInternerDeathTest, InvalidOwnerAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH({ TypeInterner ti{InvalidCompilationUnit}; },
                 "owner CompilationUnitId is invalid");
}

TEST(TypeInternerDeathTest, OutOfRangeTypeIdAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto ti = makeInterner(1);
    ti.primitive(TypeKind::I32);
    EXPECT_DEATH({ (void)ti.get(TypeId{999, 1}); }, "out of range");
}

TEST(TypeInternerDeathTest, FnAccessorOnNonFnSigAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto ti = makeInterner(1);
    const TypeId i32 = ti.primitive(TypeKind::I32);
    EXPECT_DEATH({ (void)ti.fnResult(i32); }, "not a FnSig");
}
