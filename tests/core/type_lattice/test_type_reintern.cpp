// Cycle 25, Stage A: TypeInterner re-intern walker (the whole-program MIR-merge
// foundation). Re-interning a TypeId from one CU's interner into a destination
// host TypeLattice must reproduce the type STRUCTURALLY — every kind, operand,
// scalar, name, and extensionKind — recursing bottom-up, memoizing per srcId,
// and letting the host's hash-consing dedup structurally-identical types.

#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "core/types/type_lattice/type_reintern.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

using namespace dss;

namespace {

// Walk BOTH trees in lockstep and assert full structural equality: same kind,
// same operand count, same scalars, same name, same extensionKind, recursing on
// every operand. This is the strong assertion — "valid()" alone would not catch
// a dropped scalar, a mis-paired operand, or a lost name.
void assertStructurallyEqual(TypeInterner const& a, TypeId aId,
                             TypeInterner const& b, TypeId bId) {
    ASSERT_EQ(a.kind(aId), b.kind(bId))
        << "kind mismatch at node";
    EXPECT_EQ(a.name(aId), b.name(bId))
        << "name mismatch for kind " << static_cast<int>(a.kind(aId));
    EXPECT_EQ(a.get(aId).extensionKind.v, b.get(bId).extensionKind.v)
        << "extensionKind mismatch for kind " << static_cast<int>(a.kind(aId));

    auto const aScalars = a.scalars(aId);
    auto const bScalars = b.scalars(bId);
    ASSERT_EQ(aScalars.size(), bScalars.size())
        << "scalar count mismatch for kind " << static_cast<int>(a.kind(aId));
    for (std::size_t i = 0; i < aScalars.size(); ++i) {
        EXPECT_EQ(aScalars[i], bScalars[i])
            << "scalar[" << i << "] mismatch for kind "
            << static_cast<int>(a.kind(aId));
    }

    auto const aOps = a.operands(aId);
    auto const bOps = b.operands(bId);
    ASSERT_EQ(aOps.size(), bOps.size())
        << "operand count mismatch for kind " << static_cast<int>(a.kind(aId));
    for (std::size_t i = 0; i < aOps.size(); ++i) {
        assertStructurallyEqual(a, aOps[i], b, bOps[i]);
    }
}

// Build a single fixture type in `src` that TRANSITIVELY exercises every
// re-internable TypeKind, and return its root TypeId. The root is an Outer
// struct whose fields reach: primitives, ptr-to-struct, a fixed array of a
// pointer to a VARIADIC fnSig, a vector, a matrix, an enum (non-default
// underlying), a NON-variadic fnSig, ref, nullable, optional, slice, tuple,
// union, and an extension. The enum + both fnSig forms (the trickiest
// encodings) are all present.
[[nodiscard]] TypeId buildOuterFixture(TypeInterner& s) {
    const TypeId i32  = s.primitive(TypeKind::I32);
    const TypeId i64  = s.primitive(TypeKind::I64);
    const TypeId u8   = s.primitive(TypeKind::U8);
    const TypeId f32  = s.primitive(TypeKind::F32);
    const TypeId f64  = s.primitive(TypeKind::F64);
    const TypeId boolT = s.primitive(TypeKind::Bool);
    const TypeId charT = s.primitive(TypeKind::Char);
    const TypeId voidT = s.primitive(TypeKind::Void);

    // ptr-to-struct: Inner{i32, f64}; Ptr<Inner>.
    std::array<TypeId, 2> const innerFields{i32, f64};
    const TypeId inner    = s.structType("Inner", innerFields);
    const TypeId ptrInner = s.pointer(inner);

    // VARIADIC fnSig: (Ptr<Char>, ...) -> I32, CcSysV. Then Ptr<that>, then
    // Array<Ptr<varFnSig>, 3>.
    const TypeId ptrChar = s.pointer(charT);
    std::array<TypeId, 1> const varParams{ptrChar};
    const TypeId varFnSig = s.fnSig(varParams, i32, CallConv::CcSysV,
                                    /*isVariadic=*/true);
    const TypeId ptrVarFn = s.pointer(varFnSig);
    const TypeId arrVarFn = s.array(ptrVarFn, 3);

    // NON-variadic fnSig: (I32, F64) -> Void, CcMS64.
    std::array<TypeId, 2> const fixedParams{i32, f64};
    const TypeId nonVarFnSig = s.fnSig(fixedParams, voidT, CallConv::CcMS64);

    // SIMD.
    const TypeId vec = s.vector(f64, 4);
    const TypeId mat = s.matrix(f32, 2, 3);

    // Enum with a NON-default underlying type (default is I32; use I64).
    const TypeId enumColor = s.enumType("Color", TypeKind::I64);

    // Indirections + slice.
    const TypeId refI32   = s.reference(i32);
    const TypeId nullU8   = s.nullable(u8);
    const TypeId optBool  = s.optional(boolT);
    const TypeId sliceCh  = s.slice(charT);

    // Tuple + union.
    std::array<TypeId, 2> const tupElems{i32, f64};
    const TypeId tup = s.tuple(tupElems);
    std::array<TypeId, 2> const unionVariants{i32, boolT};
    const TypeId uni = s.unionType("Variant", unionVariants);

    // Extension: a Varchar-like nominal kind with a type arg + scalar arg.
    const TypeKindId varchar{kFirstExtensionKind};
    std::array<TypeId, 1> const extTypeArgs{charT};
    std::array<std::int64_t, 1> const extScalars{255};
    const TypeId ext = s.extension(varchar, "TSQL::Varchar", extTypeArgs, extScalars);

    // Outer struct gathering one field per branch (silences "unused" and pins
    // every kind into the transitive closure of the returned root).
    std::array<TypeId, 14> const outerFields{
        ptrInner,     // -> Ptr -> Struct -> {I32, F64}
        arrVarFn,     // -> Array -> Ptr -> variadic FnSig -> Ptr<Char>, I32
        vec,          // -> Vector -> F64
        mat,          // -> Matrix -> F32
        enumColor,    // -> Enum (I64 underlying)
        nonVarFnSig,  // -> non-variadic FnSig -> I32, F64, Void
        refI32,       // -> Ref
        nullU8,       // -> Nullable
        optBool,      // -> Optional
        sliceCh,      // -> Slice
        tup,          // -> Tuple
        uni,          // -> Union
        ext,          // -> Extension
        i64,          // -> a bare extra primitive (U-rank coverage via U8 above)
    };
    return s.structType("Outer", outerFields);
}

} // namespace

TEST(TypeReintern, ReinternEveryKindStructurallyEqual) {
    TypeInterner src{CompilationUnitId{1}};
    TypeLattice  host{CompilationUnitId{2}};

    const TypeId outer = buildOuterFixture(src);

    std::unordered_map<std::uint32_t, TypeId> remap;
    const TypeId hostOuter = reinternType(src, outer, host, remap);

    ASSERT_TRUE(hostOuter.valid());
    // The host TypeId is host-stamped (CU 2), not src-stamped (CU 1).
    EXPECT_EQ(hostOuter.arenaTag, 2u);

    // Full recursive structural equality across the entire transitive tree.
    assertStructurallyEqual(src, outer, host.interner(), hostOuter);

    // Spot-check the two trickiest encodings survived as their own kinds with
    // the right discriminators — the variadic fnSig (2-scalar) and the enum
    // (non-default underlying, zero operands). Locate them via the Outer fields.
    auto const hostFields = host.interner().operands(hostOuter);
    ASSERT_EQ(hostFields.size(), 14u);

    // Field 1 = Array<Ptr<variadic FnSig>, 3>: drill to the FnSig and assert
    // variadic + scalar shape.
    const TypeId hArr = hostFields[1];
    ASSERT_EQ(host.interner().kind(hArr), TypeKind::Array);
    const TypeId hPtrFn = host.interner().operands(hArr)[0];
    ASSERT_EQ(host.interner().kind(hPtrFn), TypeKind::Ptr);
    const TypeId hVarFn = host.interner().operands(hPtrFn)[0];
    ASSERT_EQ(host.interner().kind(hVarFn), TypeKind::FnSig);
    EXPECT_TRUE(host.interner().fnIsVariadic(hVarFn));
    EXPECT_EQ(host.interner().scalars(hVarFn).size(), 2u);   // [cc, isVariadic]
    EXPECT_EQ(host.interner().fnParams(hVarFn).size(), 1u);  // (Ptr<Char>)

    // Field 5 = non-variadic FnSig: 1 scalar, not variadic.
    const TypeId hNonVar = hostFields[5];
    ASSERT_EQ(host.interner().kind(hNonVar), TypeKind::FnSig);
    EXPECT_FALSE(host.interner().fnIsVariadic(hNonVar));
    EXPECT_EQ(host.interner().scalars(hNonVar).size(), 1u);  // [cc] only

    // Field 4 = Enum with NON-default underlying (I64) and zero operands.
    const TypeId hEnum = hostFields[4];
    ASSERT_EQ(host.interner().kind(hEnum), TypeKind::Enum);
    EXPECT_EQ(host.interner().name(hEnum), "Color");
    EXPECT_TRUE(host.interner().operands(hEnum).empty());
    ASSERT_EQ(host.interner().scalars(hEnum).size(), 1u);
    EXPECT_EQ(host.interner().scalars(hEnum)[0],
              static_cast<std::int64_t>(TypeKind::I64));

    // Field 12 = Extension: extensionKind + name + type arg + scalar arg.
    const TypeId hExt = hostFields[12];
    ASSERT_EQ(host.interner().kind(hExt), TypeKind::Extension);
    EXPECT_EQ(host.interner().get(hExt).extensionKind.v, kFirstExtensionKind);
    EXPECT_EQ(host.interner().name(hExt), "TSQL::Varchar");
    ASSERT_EQ(host.interner().operands(hExt).size(), 1u);
    ASSERT_EQ(host.interner().scalars(hExt).size(), 1u);
    EXPECT_EQ(host.interner().scalars(hExt)[0], std::int64_t{255});
}

TEST(TypeReintern, ReinternIsIdempotent) {
    TypeInterner src{CompilationUnitId{1}};
    TypeLattice  host{CompilationUnitId{2}};

    const TypeId i32     = src.primitive(TypeKind::I32);
    const TypeId ptrI32  = src.pointer(i32);

    std::unordered_map<std::uint32_t, TypeId> remap;
    const TypeId first  = reinternType(src, ptrI32, host, remap);
    const TypeId second = reinternType(src, ptrI32, host, remap);  // same memo
    EXPECT_EQ(first.v, second.v);
    EXPECT_TRUE(first.valid());

    // The memo also stabilizes the children: re-interning the inner i32 returns
    // the SAME host TypeId the pointer's pointee already resolved to.
    const TypeId innerAgain = reinternType(src, i32, host, remap);
    EXPECT_EQ(innerAgain.v, host.interner().operands(first)[0].v);
}

TEST(TypeReintern, ReinternDedupsStructurallyIdentical) {
    // Two DIFFERENT src TypeIds that are structurally identical must re-intern
    // to ONE host TypeId — the host's hash-consing collapses them. Build `int*`
    // twice in src (canonicalizing makes them equal in src, so additionally
    // build a structurally-identical type whose src ids genuinely differ by
    // routing one through a SECOND source interner).
    TypeLattice host{CompilationUnitId{9}};

    // Source A (CU 1): int*.
    TypeInterner srcA{CompilationUnitId{1}};
    const TypeId aPtr = srcA.pointer(srcA.primitive(TypeKind::I32));

    // Source B (CU 2): an independently-built int* — a DIFFERENT interner, so
    // its TypeId is genuinely distinct from A's (different arenaTag + possibly
    // different .v), yet structurally identical.
    TypeInterner srcB{CompilationUnitId{2}};
    const TypeId bPtr = srcB.pointer(srcB.primitive(TypeKind::I32));

    // Independent memos (different source interners ⇒ different srcId spaces).
    std::unordered_map<std::uint32_t, TypeId> remapA;
    std::unordered_map<std::uint32_t, TypeId> remapB;
    const TypeId hostFromA = reinternType(srcA, aPtr, host, remapA);
    const TypeId hostFromB = reinternType(srcB, bPtr, host, remapB);

    // The host hash-consed them to the SAME TypeId.
    EXPECT_EQ(hostFromA.v, hostFromB.v);
    EXPECT_EQ(host.interner().kind(hostFromA), TypeKind::Ptr);

    // And a third int* built directly in the host collapses too.
    const TypeId hostDirect = host.interner().pointer(
        host.interner().primitive(TypeKind::I32));
    EXPECT_EQ(hostDirect.v, hostFromA.v);
}
