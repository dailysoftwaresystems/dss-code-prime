// Cycle 25, Stage A: TypeInterner re-intern walker (the whole-program MIR-merge
// foundation). Re-interning a TypeId from one CU's interner into a destination
// host TypeLattice must reproduce the type STRUCTURALLY — every kind, operand,
// scalar, name, and extensionKind — recursing bottom-up, memoizing per srcId,
// and letting the host's hash-consing dedup structurally-identical types.

#include "core/types/data_model.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "core/types/type_lattice/type_layout.hpp"   // D-CSUBSET-PACKED: packed layout survives
#include "core/types/type_lattice/type_reintern.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
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

// D-CSUBSET-SELF-REFERENTIAL-STRUCT: re-interning a SELF-REFERENTIAL composite
// must TERMINATE (the field is a Ptr back to the struct's OWN TypeId — a cycle in
// the operand graph). The reintern walker forward-mints the host composite and
// memoizes it BEFORE recursing the fields, so the self-ref field resolves to the
// placeholder instead of looping. Without that (the old memo-after-recursion) this
// would infinite-loop / stack-overflow. The re-interned type must be structurally
// the same self-referential shape.
TEST(TypeReintern, SelfReferentialStructTerminatesAndReproduces) {
    TypeInterner src{CompilationUnitId{1}};
    const TypeId i32 = src.primitive(TypeKind::I32);
    const TypeId n   = src.forwardComposite(TypeKind::Struct, "N", /*declSiteKey=*/9);
    const TypeId ptrN = src.pointer(n);
    std::array<TypeId, 2> const fields{ptrN, i32};   // { N* next; int v; }
    src.completeComposite(n, fields, /*packed=*/false);

    TypeLattice host{CompilationUnitId{2}};
    std::unordered_map<std::uint32_t, TypeId> remap;
    const TypeId hostN = reinternType(src, n, host, remap);   // must not hang

    auto const& hi = host.interner();
    ASSERT_TRUE(hostN.valid());
    EXPECT_EQ(hi.kind(hostN), TypeKind::Struct);
    EXPECT_EQ(hi.name(hostN), "N");
    ASSERT_EQ(hi.operands(hostN).size(), 2u);
    // field[0] = Ptr<N> whose pointee is the SAME host TypeId (cycle reproduced).
    ASSERT_EQ(hi.kind(hi.operands(hostN)[0]), TypeKind::Ptr);
    EXPECT_EQ(hi.operands(hi.operands(hostN)[0])[0].v, hostN.v);
    EXPECT_EQ(hi.kind(hi.operands(hostN)[1]), TypeKind::I32);
}

// D-CSUBSET-SELF-REFERENTIAL-STRUCT: an INCOMPLETE source composite (forward-
// declared, never completed) re-interns to an incomplete host composite (no
// fields), not a hang or a fabricated body.
TEST(TypeReintern, IncompleteCompositeReinternsIncomplete) {
    TypeInterner src{CompilationUnitId{1}};
    const TypeId fwd = src.forwardComposite(TypeKind::Struct, "Opaque", 3);
    ASSERT_TRUE(src.isIncompleteComposite(fwd));

    TypeLattice host{CompilationUnitId{2}};
    std::unordered_map<std::uint32_t, TypeId> remap;
    const TypeId hostFwd = reinternType(src, fwd, host, remap);
    EXPECT_EQ(host.interner().kind(hostFwd), TypeKind::Struct);
    EXPECT_TRUE(host.interner().isIncompleteComposite(hostFwd));
}

// c27 (D-CSUBSET-VOLATILE-POINTEE): a VolatileQual wrapper must SURVIVE re-intern.
// RED-ON-DISABLE: reinternType reads the RAW record kind (`get().kind`), NOT the
// transparent `kind()` — using `kind()` would reintern `volatile int` AS plain
// `int`, silently DROPPING the qualifier (a cross-CU-merge miscompile). The
// `assertStructurallyEqual` helper above uses the transparent `kind()` and so
// canNOT catch this; this test checks the RAW kind + `isVolatileQualified`.
TEST(TypeReintern, VolatileQualifierSurvivesReintern) {
    TypeInterner src{CompilationUnitId{1}};
    const TypeId i32   = src.primitive(TypeKind::I32);
    const TypeId vi32  = src.volatileQualified(i32);      // volatile int
    const TypeId pvi32 = src.pointer(vi32);                // volatile int *

    TypeLattice host{CompilationUnitId{2}};
    auto& hi = host.interner();
    std::unordered_map<std::uint32_t, TypeId> remap;

    // bare `volatile int` round-trips as a VolatileQual over I32.
    const TypeId hvi32 = reinternType(src, vi32, host, remap);
    EXPECT_EQ(hi.get(hvi32).kind, TypeKind::VolatileQual)
        << "the VolatileQual wrapper must NOT be dropped (raw kind preserved)";
    EXPECT_TRUE(hi.isVolatileQualified(hvi32));
    EXPECT_EQ(hi.kind(hvi32), TypeKind::I32);              // transparent material kind
    EXPECT_EQ(hi.stripVolatile(hvi32).v, hi.primitive(TypeKind::I32).v);

    // `volatile int *` round-trips as Ptr<VolatileQual(I32)> (the sqlite shape).
    const TypeId hpvi32 = reinternType(src, pvi32, host, remap);
    EXPECT_EQ(hi.kind(hpvi32), TypeKind::Ptr);
    EXPECT_FALSE(hi.isVolatileQualified(hpvi32));          // the pointer is plain
    ASSERT_EQ(hi.operands(hpvi32).size(), 1u);
    EXPECT_TRUE(hi.isVolatileQualified(hi.operands(hpvi32)[0]))
        << "the POINTEE must stay volatile-qualified through re-intern";
}

// FC17.9(d) 1a (D-CSUBSET-QUAL-BITSET): the qualifier MASK must survive re-intern —
// an `_Atomic` (or `_Atomic volatile`) qualifier must NOT degrade to plain volatile on
// a cross-CU merge / text round-trip. RED-ON-DISABLE: the reintern arm rebuilds via
// `qualified(inner, src.qualifierBits(srcId))`; reverting it to `volatileQualified`
// sets only the Volatile bit, so `isAtomicQualified` below goes RED (Trap 2 — the
// silent loss-of-atomicity twin of the volatile-drop caught above).
TEST(TypeReintern, AtomicQualifierMaskSurvivesReintern) {
    TypeInterner src{CompilationUnitId{1}};
    const TypeId i32   = src.primitive(TypeKind::I32);
    const TypeId ai32  = src.atomicQualified(i32);                          // _Atomic int
    const TypeId avi32 = src.atomicQualified(src.volatileQualified(i32));   // _Atomic volatile int

    TypeLattice host{CompilationUnitId{2}};
    auto& hi = host.interner();
    std::unordered_map<std::uint32_t, TypeId> remap;

    // bare `_Atomic int` round-trips with the Atomic bit intact (NOT dropped/degraded).
    const TypeId hai32 = reinternType(src, ai32, host, remap);
    EXPECT_EQ(hi.get(hai32).kind, TypeKind::VolatileQual);   // the shared qualifier skin kind
    EXPECT_TRUE(hi.isAtomicQualified(hai32));                // ← RED if the mask is dropped
    EXPECT_FALSE(hi.isVolatileQualified(hai32));             // and NOT spuriously volatile
    EXPECT_EQ(hi.kind(hai32), TypeKind::I32);                // transparent material kind

    // `_Atomic volatile int` round-trips with BOTH bits (order-independent, one skin).
    const TypeId havi32 = reinternType(src, avi32, host, remap);
    EXPECT_TRUE(hi.isAtomicQualified(havi32));
    EXPECT_TRUE(hi.isVolatileQualified(havi32));
    EXPECT_EQ(hi.stripVolatile(havi32).v, hi.primitive(TypeKind::I32).v);
}

// D-CSUBSET-MEMBER-ALIGNAS: member-alignas overrides must SURVIVE re-intern — without
// threading them, the reinterned composite loses its declared field alignment (falls
// back to natural) and forks the TypeId that `.member` scope keys on. RED-ON-DISABLE:
// drop the aligns from the reintern completeComposite call and `hasExplicitAligns` on
// the host is false / the override reads back 0.
TEST(TypeReintern, MemberAlignsSurviveReintern) {
    TypeInterner src{CompilationUnitId{1}};
    const TypeId i32 = src.primitive(TypeKind::I32);
    std::array<TypeId, 1>        const fields{i32};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 1> const aligns{16};
    const TypeId s = src.structType("S", fields, noWidths, noOffs, aligns);
    ASSERT_TRUE(src.hasExplicitAligns(s));

    TypeLattice host{CompilationUnitId{2}};
    auto& hi = host.interner();
    std::unordered_map<std::uint32_t, TypeId> remap;
    const TypeId hs = reinternType(src, s, host, remap);

    ASSERT_TRUE(hs.valid());
    EXPECT_EQ(hs.arenaTag, 2u);                       // host-stamped
    ASSERT_EQ(hi.kind(hs), TypeKind::Struct);
    EXPECT_TRUE(hi.hasExplicitAligns(hs))
        << "the member-alignas override must NOT be dropped through re-intern";
    EXPECT_EQ(hi.explicitFieldAlign(hs, 0), 16u);
    ASSERT_EQ(hi.operands(hs).size(), 1u);
    EXPECT_EQ(hi.kind(hi.operands(hs)[0]), TypeKind::I32);
}

// D-CSUBSET-PACKED (F2): the whole-composite packed flag must SURVIVE re-intern —
// without threading it through the destination completeComposite, a packed struct
// crossing a CU/round-trip boundary reinterns as UNPACKED (padded), a silent ABI
// miscompile. RED-ON-DISABLE: drop `src.isPacked(srcId)` from the reintern
// completeComposite call and `isPacked` on the host is false + the layout re-pads.
TEST(TypeReintern, PackedFlagSurvivesReintern) {
    TypeInterner src{CompilationUnitId{1}};
    // struct S { char c; uint32_t v; } __attribute__((packed));  // size 5, align 1
    std::array<TypeId, 2> const fields{src.primitive(TypeKind::Char),
                                       src.primitive(TypeKind::U32)};
    const TypeId s = src.forwardComposite(TypeKind::Struct, "S", /*declSiteKey=*/7);
    src.completeComposite(s, fields, /*packed=*/true);
    ASSERT_TRUE(src.isPacked(s));

    TypeLattice host{CompilationUnitId{2}};
    auto& hi = host.interner();
    std::unordered_map<std::uint32_t, TypeId> remap;
    const TypeId hs = reinternType(src, s, host, remap);

    ASSERT_TRUE(hs.valid());
    ASSERT_EQ(hi.kind(hs), TypeKind::Struct);
    EXPECT_TRUE(hi.isPacked(hs))
        << "the packed flag must NOT be dropped through re-intern (silent ABI drop)";
    // The packed LAYOUT survives too: char@0, v@1 (no padding) → size 5, align 1.
    constexpr AggregateLayoutParams params{ScalarAlignmentRule::Natural, 16};
    auto const l = computeLayout(hs, hi, params, DataModel::Lp64);
    ASSERT_TRUE(l.has_value());
    EXPECT_EQ(l->size, 5u);
    EXPECT_EQ(l->align.bytes(), 1u);
    ASSERT_EQ(l->fieldOffsets.size(), 2u);
    EXPECT_EQ(l->fieldOffsets[1], 1u);
}

// ★★ D-CSUBSET-COMPOSITE-ALIGNED (TF-C73): the WHOLE-COMPOSITE explicit alignment
// must SURVIVE re-intern. This is the pin the whole channel hangs on: dropping the
// value here is the exact silent miscompile the cycle exists to kill — a
// `__attribute__((aligned(16)))` struct crosses a CU / static-link merge / text
// round-trip and comes back UNDER-ALIGNED with its size quietly SHRUNK (16 → 5 for
// the packed witness below), with no diagnostic anywhere.
//
// It also stands in for a guarantee `packed` gets from the compiler and this channel
// cannot: `completeComposite`'s `packed` parameter is non-defaulted, so forgetting it
// fails to COMPILE. `explicitAlign` must be defaulted (an undefaulted parameter
// cannot follow the already-defaulted spans, and moving it earlier would break
// `completeComposite` call sites in files this change does not own), so this test IS
// the guard. RED-ON-DISABLE: drop `src.explicitCompositeAlign(srcId)` from the
// reintern `completeComposite` call — the host reads 0 and the layout falls to 5 / 1.
TEST(TypeReintern, CompositeExplicitAlignSurvivesReintern) {
    TypeInterner src{CompilationUnitId{1}};
    // struct S { char a; int b; } __attribute__((packed, aligned(16)));
    // clang (MEASURED, compiled and run on arm64 macOS): sizeof 16, _Alignof 16.
    std::array<TypeId, 2> const fields{src.primitive(TypeKind::Char),
                                       src.primitive(TypeKind::I32)};
    const TypeId s = src.forwardComposite(TypeKind::Struct, "S", /*declSiteKey=*/9);
    src.completeComposite(s, fields, /*packed=*/true, /*fieldBitWidths=*/{},
                          /*fieldOffsets=*/{}, /*fieldAligns=*/{},
                          /*explicitAlign=*/16u);
    ASSERT_EQ(src.explicitCompositeAlign(s), 16u);

    TypeLattice host{CompilationUnitId{2}};
    auto& hi = host.interner();
    std::unordered_map<std::uint32_t, TypeId> remap;
    const TypeId hs = reinternType(src, s, host, remap);

    ASSERT_TRUE(hs.valid());
    EXPECT_EQ(hs.arenaTag, 2u);                       // host-stamped
    ASSERT_EQ(hi.kind(hs), TypeKind::Struct);
    EXPECT_EQ(hi.explicitCompositeAlign(hs), 16u)
        << "the whole-composite alignment must NOT be dropped through re-intern";
    EXPECT_TRUE(hi.isPacked(hs));                     // the packed channel too
    // ...and so must the resulting LAYOUT — the value being carried is only worth
    // anything if the bytes come out the same on the far side.
    constexpr AggregateLayoutParams params{ScalarAlignmentRule::Natural, 16};
    auto const l = computeLayout(hs, hi, params, DataModel::Lp64);
    ASSERT_TRUE(l.has_value());
    EXPECT_EQ(l->align.bytes(), 16u);   // RED-ON-DISABLE (packed alone → 1)
    EXPECT_EQ(l->size, 16u);            // RED-ON-DISABLE (packed alone → 5)
    ASSERT_EQ(l->fieldOffsets.size(), 2u);
    EXPECT_EQ(l->fieldOffsets[1], 1u);  // packed placement preserved as well
}

// A composite with NO alignment request must reintern with NO request — the channel
// is sparse, and a reintern that invented a value (or copied a neighbour's) would
// change the layout of every ordinary struct that crosses a CU boundary.
TEST(TypeReintern, CompositeWithoutExplicitAlignReinternsWithout) {
    TypeInterner src{CompilationUnitId{1}};
    std::array<TypeId, 2> const fields{src.primitive(TypeKind::Char),
                                       src.primitive(TypeKind::I32)};
    const TypeId s = src.structType("Plain", fields);
    ASSERT_EQ(src.explicitCompositeAlign(s), 0u);

    TypeLattice host{CompilationUnitId{2}};
    std::unordered_map<std::uint32_t, TypeId> remap;
    const TypeId hs = reinternType(src, s, host, remap);
    EXPECT_EQ(host.interner().explicitCompositeAlign(hs), 0u);
    constexpr AggregateLayoutParams params{ScalarAlignmentRule::Natural, 16};
    auto const l = computeLayout(hs, host.interner(), params, DataModel::Lp64);
    ASSERT_TRUE(l.has_value());
    EXPECT_EQ(l->align.bytes(), 4u);
    EXPECT_EQ(l->size, 8u);
}

// D-LANG-TYPE-IDENTITY-VOCABULARY: a primitive's VOCABULARY TAG must survive
// re-intern. RED-ON-DISABLE: the reintern primitive arm is
// `dst.primitive(kind, src.name(srcId))`; reverting it to `dst.primitive(kind)`
// drops the tag on EVERY primitive that crosses a CU / static-link-merge / text
// round-trip — silently re-collapsing `long` onto `int` (LLP64) and `long`
// onto `long long` (LP64) at exactly the boundary the front-end just fixed.
TEST(TypeReintern, VocabularyNameSurvivesReintern) {
    TypeInterner src{CompilationUnitId{1}};
    // Three types with the SAME representation (I64) and three identities.
    const TypeId anon     = src.primitive(TypeKind::I64);
    const TypeId longT    = src.primitive(TypeKind::I64, "long");
    const TypeId longLong = src.primitive(TypeKind::I64, "long long");
    ASSERT_NE(anon.v, longT.v);
    ASSERT_NE(longT.v, longLong.v);

    TypeLattice host{CompilationUnitId{2}};
    auto& hi = host.interner();
    std::unordered_map<std::uint32_t, TypeId> remap;

    const TypeId hAnon     = reinternType(src, anon, host, remap);
    const TypeId hLong     = reinternType(src, longT, host, remap);
    const TypeId hLongLong = reinternType(src, longLong, host, remap);

    // All three stay THREE distinct host types with their tags intact.
    EXPECT_EQ(hi.name(hAnon), "");
    EXPECT_EQ(hi.name(hLong), "long");
    EXPECT_EQ(hi.name(hLongLong), "long long");
    EXPECT_NE(hAnon.v, hLong.v);
    EXPECT_NE(hLong.v, hLongLong.v);
    EXPECT_NE(hAnon.v, hLongLong.v);
    // ... and the representation is untouched on every one.
    EXPECT_EQ(hi.kind(hAnon), TypeKind::I64);
    EXPECT_EQ(hi.kind(hLong), TypeKind::I64);
    EXPECT_EQ(hi.kind(hLongLong), TypeKind::I64);

    // A DERIVED type carries the tag through its operands (the FFI-pointer and
    // struct-field shapes): `long *` and `long long *` stay distinct in the host.
    const TypeId hpLong     = reinternType(src, src.pointer(longT), host, remap);
    const TypeId hpLongLong = reinternType(src, src.pointer(longLong), host, remap);
    EXPECT_NE(hpLong.v, hpLongLong.v)
        << "two pointers whose pointees differ only by vocabulary entry must NOT "
           "collapse in the host lattice";
    ASSERT_EQ(hi.operands(hpLong).size(), 1u);
    EXPECT_EQ(hi.name(hi.operands(hpLong)[0]), "long");
}

// The 2-CU shape the whole-program / static-link merge actually runs: TWO source
// interners folded into ONE host lattice. The same vocabulary entry from both CUs
// must CANONICALIZE to one host TypeId, while entries that merely share a
// representation must stay distinct.
TEST(TypeReintern, TwoCompilationUnitsUnifyVocabularyInOneHost) {
    TypeInterner cu1{CompilationUnitId{1}};
    TypeInterner cu2{CompilationUnitId{2}};
    const TypeId cu1Long     = cu1.primitive(TypeKind::I64, "long");
    const TypeId cu1LongLong = cu1.primitive(TypeKind::I64, "long long");
    const TypeId cu2Long     = cu2.primitive(TypeKind::I64, "long");
    const TypeId cu2Anon     = cu2.primitive(TypeKind::I64);

    TypeLattice host{CompilationUnitId{99}};
    auto& hi = host.interner();
    std::unordered_map<std::uint32_t, TypeId> remap1;
    std::unordered_map<std::uint32_t, TypeId> remap2;

    const TypeId h1Long     = reinternType(cu1, cu1Long, host, remap1);
    const TypeId h1LongLong = reinternType(cu1, cu1LongLong, host, remap1);
    const TypeId h2Long     = reinternType(cu2, cu2Long, host, remap2);
    const TypeId h2Anon     = reinternType(cu2, cu2Anon, host, remap2);

    EXPECT_EQ(h1Long.v, h2Long.v)
        << "the SAME vocabulary entry from two CUs must canonicalize to ONE host "
           "TypeId — otherwise a merged program would hold two `long` types";
    EXPECT_NE(h1Long.v, h1LongLong.v);
    EXPECT_NE(h1Long.v, h2Anon.v)
        << "a named entry must never collapse onto the anonymous representative "
           "of its core just because it crossed a CU boundary";
    EXPECT_EQ(hi.name(h1Long), "long");
    EXPECT_EQ(hi.name(h2Anon), "");
}

// ═══════════════════════════════════════════════════════════════════════════
// D-MIR-MERGE-COMPOSITE-HOST-IDENTITY-IS-THE-DECLARATION-SITE
// ═══════════════════════════════════════════════════════════════════════════
//
// A composite's HOST identity used to be its SOURCE DECLARATION SITE, so ONE C
// type forked once per CU that mentioned it. The suite below pins the two halves
// that must hold TOGETHER: a forward declaration and its definition unify, and
// two same-tag composites with DIFFERENT layouts never do.
//
// ⚠ Every test here uses the FIVE-argument reinternType with ONE shared,
// pre-observed CompositeIdentityIndex — that is the merge's own calling
// convention, and the four-argument overload cannot express the cross-CU case at
// all (its scratch index has seen a single interner).
namespace {

// Build `struct Bitvec { int a; }` in `cu`, returning the composite.
TypeId defineBitvec(TypeInterner& cu, std::uint64_t declSiteKey) {
    const TypeId c = cu.forwardComposite(TypeKind::Struct, "Bitvec", declSiteKey);
    std::array<TypeId, 1> const fields{cu.primitive(TypeKind::I32)};
    cu.completeComposite(c, fields, /*packed=*/false);
    return c;
}

} // namespace

// ★ THE SHAPE THE FIX EXISTS FOR — sqlite's `typedef struct Bitvec Bitvec;` in a
// header, defined in exactly one `.c`. Every TU that only ever handles a
// `Bitvec*` contributes its OWN incomplete `Bitvec`; before the fix the merge
// kept all of them apart from the definition, and the release build died with
// I_StoreValueTypeMismatch on stores whose two sides were one C type.
TEST(TypeReinternCompositeIdentity, OpaquePointerIdiomUnifiesAcrossCus) {
    TypeInterner cuOpaque{CompilationUnitId{1}};   // sees only the typedef
    const TypeId fwd = cuOpaque.forwardComposite(TypeKind::Struct, "Bitvec", 11);
    ASSERT_TRUE(cuOpaque.isIncompleteComposite(fwd));

    TypeInterner cuDef{CompilationUnitId{2}};      // bitvec.c
    const TypeId def = defineBitvec(cuDef, 77);

    CompositeIdentityIndex index;
    index.observe(cuOpaque);
    index.observe(cuDef);
    EXPECT_EQ(index.observedTagCount(), 1u)
        << "one COMPLETE definition of one tag was observed";

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> remapOpaque;
    std::unordered_map<std::uint32_t, TypeId> remapDef;
    const TypeId hFwd = reinternType(cuOpaque, fwd, host, remapOpaque, index);
    const TypeId hDef = reinternType(cuDef, def, host, remapDef, index);

    EXPECT_EQ(hFwd.v, hDef.v)
        << "a forward declaration and its definition are ONE C type and must "
           "reintern to ONE host TypeId";
    EXPECT_FALSE(host.interner().isIncompleteComposite(hDef))
        << "the unified host type carries the definition's body";
    EXPECT_EQ(host.interner().operands(hDef).size(), 1u);
}

// The same pair walked the OTHER way round. The merge does not choose the order
// its CUs are visited in, so a fix that only works when the definition happens to
// come first is not a fix — it is a coin flip that passes its own test.
TEST(TypeReinternCompositeIdentity, OpaquePointerIdiomUnifiesInEitherOrder) {
    TypeInterner cuOpaque{CompilationUnitId{1}};
    const TypeId fwd = cuOpaque.forwardComposite(TypeKind::Struct, "Bitvec", 11);
    TypeInterner cuDef{CompilationUnitId{2}};
    const TypeId def = defineBitvec(cuDef, 77);

    CompositeIdentityIndex index;
    index.observe(cuOpaque);
    index.observe(cuDef);

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> remapDef;
    std::unordered_map<std::uint32_t, TypeId> remapOpaque;
    const TypeId hDef = reinternType(cuDef, def, host, remapDef, index);
    const TypeId hFwd = reinternType(cuOpaque, fwd, host, remapOpaque, index);

    EXPECT_EQ(hFwd.v, hDef.v);
    EXPECT_FALSE(host.interner().isIncompleteComposite(hFwd))
        << "reaching the definition FIRST must still leave the forward "
           "declaration resolving to the complete host type";
}

// ⚠⚠ THE MISCOMPILE GUARD, and it is the reason the key is the layout DIGEST and
// not the tag. Two `.c` files may each define a private `struct S` differently;
// C keeps them apart and so must the merge. Merging them would fix one
// miscompile by shipping a worse one.
TEST(TypeReinternCompositeIdentity, SameTagDifferentLayoutsStayDistinct) {
    TypeInterner cuA{CompilationUnitId{1}};
    const TypeId a = cuA.forwardComposite(TypeKind::Struct, "S", 5);
    std::array<TypeId, 1> const aFields{cuA.primitive(TypeKind::I32)};
    cuA.completeComposite(a, aFields, /*packed=*/false);

    TypeInterner cuB{CompilationUnitId{2}};
    const TypeId b = cuB.forwardComposite(TypeKind::Struct, "S", 5);
    std::array<TypeId, 2> const bFields{cuB.primitive(TypeKind::F64),
                                        cuB.primitive(TypeKind::I32)};
    cuB.completeComposite(b, bFields, /*packed=*/false);

    CompositeIdentityIndex index;
    index.observe(cuA);
    index.observe(cuB);

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> remapA;
    std::unordered_map<std::uint32_t, TypeId> remapB;
    const TypeId hA = reinternType(cuA, a, host, remapA, index);
    const TypeId hB = reinternType(cuB, b, host, remapB, index);

    EXPECT_NE(hA.v, hB.v)
        << "two same-tag composites with different layouts are different types";
    EXPECT_EQ(host.interner().operands(hA).size(), 1u);
    EXPECT_EQ(host.interner().operands(hB).size(), 2u);
}

// And when a tag is AMBIGUOUS, a forward declaration of it may not borrow either
// layout — there is no fact that says which one it meant. It stays opaque, which
// is exactly the pre-fix behaviour for this case and is the conservative answer.
TEST(TypeReinternCompositeIdentity, AmbiguousTagLeavesForwardDeclarationOpaque) {
    TypeInterner cuA{CompilationUnitId{1}};
    const TypeId a = cuA.forwardComposite(TypeKind::Struct, "S", 5);
    std::array<TypeId, 1> const aFields{cuA.primitive(TypeKind::I32)};
    cuA.completeComposite(a, aFields, /*packed=*/false);

    TypeInterner cuB{CompilationUnitId{2}};
    const TypeId b = cuB.forwardComposite(TypeKind::Struct, "S", 5);
    std::array<TypeId, 1> const bFields{cuB.primitive(TypeKind::F64)};
    cuB.completeComposite(b, bFields, /*packed=*/false);

    TypeInterner cuC{CompilationUnitId{3}};
    const TypeId c = cuC.forwardComposite(TypeKind::Struct, "S", 5);

    CompositeIdentityIndex index;
    index.observe(cuA);
    index.observe(cuB);
    index.observe(cuC);

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> rA, rB, rC;
    const TypeId hA = reinternType(cuA, a, host, rA, index);
    const TypeId hB = reinternType(cuB, b, host, rB, index);
    const TypeId hC = reinternType(cuC, c, host, rC, index);

    EXPECT_NE(hC.v, hA.v);
    EXPECT_NE(hC.v, hB.v);
    EXPECT_TRUE(host.interner().isIncompleteComposite(hC));
}

// Two CUs that each see the SAME definition (the ordinary case for any struct in
// a shared header) hold one type between them, not two.
TEST(TypeReinternCompositeIdentity, IdenticalDefinitionsInTwoCusUnify) {
    TypeInterner cuA{CompilationUnitId{1}};
    TypeInterner cuB{CompilationUnitId{2}};
    const TypeId a = defineBitvec(cuA, 5);
    const TypeId b = defineBitvec(cuB, 5000);   // a different decl-site key

    CompositeIdentityIndex index;
    index.observe(cuA);
    index.observe(cuB);

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> rA, rB;
    EXPECT_EQ(reinternType(cuA, a, host, rA, index).v,
              reinternType(cuB, b, host, rB, index).v)
        << "identity is the STRUCTURE, so an unrelated decl-site key must not "
           "fork one type in two";
}

// Two block-scoped `struct S`s in ONE CU with different layouts are distinct C
// types, and the source interner already keeps them apart. Nothing about keying
// the host on structure may collapse them.
TEST(TypeReinternCompositeIdentity, TwoBlockScopeSameTagStructsInOneCuStayDistinct) {
    TypeInterner cu{CompilationUnitId{1}};
    const TypeId s1 = cu.forwardComposite(TypeKind::Struct, "S", 100);
    std::array<TypeId, 1> const f1{cu.primitive(TypeKind::I32)};
    cu.completeComposite(s1, f1, /*packed=*/false);
    const TypeId s2 = cu.forwardComposite(TypeKind::Struct, "S", 200);
    std::array<TypeId, 1> const f2{cu.primitive(TypeKind::F64)};
    cu.completeComposite(s2, f2, /*packed=*/false);
    ASSERT_NE(s1.v, s2.v);

    CompositeIdentityIndex index;
    index.observe(cu);

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> remap;
    EXPECT_NE(reinternType(cu, s1, host, remap, index).v,
              reinternType(cu, s2, host, remap, index).v);
}

// A genuinely opaque type — declared everywhere, defined nowhere — still gets ONE
// host placeholder instead of one per declaring CU.
TEST(TypeReinternCompositeIdentity, ForwardDeclarationsWithNoDefinitionUnify) {
    TypeInterner cuA{CompilationUnitId{1}};
    TypeInterner cuB{CompilationUnitId{2}};
    const TypeId a = cuA.forwardComposite(TypeKind::Struct, "Opaque", 1);
    const TypeId b = cuB.forwardComposite(TypeKind::Struct, "Opaque", 2);

    CompositeIdentityIndex index;
    index.observe(cuA);
    index.observe(cuB);
    EXPECT_EQ(index.observedTagCount(), 0u)
        << "observe() records DEFINITIONS; a tag with none is not a tag it can "
           "resolve";

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> rA, rB;
    const TypeId hA = reinternType(cuA, a, host, rA, index);
    const TypeId hB = reinternType(cuB, b, host, rB, index);
    EXPECT_EQ(hA.v, hB.v);
    EXPECT_TRUE(host.interner().isIncompleteComposite(hA));
}

// ★★ THE DE BRUIJN PIN. A composite's digest is memoized, so a backreference
// must be a RELATIVE depth: an absolute stack index would make `struct N` digest
// differently depending on how deep the walk was when it reached it, and the memo
// would then hand a digest computed at one depth to a lookup at another. Here the
// SAME self-referential `N` is reached at depth 0 from one CU and at depth 1 (via
// `Outer`) from the next; both must land on one host TypeId.
TEST(TypeReinternCompositeIdentity, SelfReferentialCompositeUnifiesAtAnyDepth) {
    auto defineN = [](TypeInterner& cu, std::uint64_t key) {
        const TypeId n = cu.forwardComposite(TypeKind::Struct, "N", key);
        std::array<TypeId, 2> const fields{cu.pointer(n),
                                           cu.primitive(TypeKind::I32)};
        cu.completeComposite(n, fields, /*packed=*/false);
        return n;
    };

    TypeInterner cuA{CompilationUnitId{1}};
    const TypeId nA = defineN(cuA, 7);

    TypeInterner cuB{CompilationUnitId{2}};
    const TypeId nB    = defineN(cuB, 9);
    const TypeId outer = cuB.forwardComposite(TypeKind::Struct, "Outer", 10);
    std::array<TypeId, 1> const oFields{cuB.pointer(nB)};
    cuB.completeComposite(outer, oFields, /*packed=*/false);

    CompositeIdentityIndex index;
    index.observe(cuA);
    index.observe(cuB);

    // The host key is what decides this, so assert it DIRECTLY as well as
    // through the reintern: if the two disagree here, the failure is in the
    // digest, and if they agree here but the ids differ, it is in the reintern.
    // Two assertions, two suspects, no bisection.
    EXPECT_EQ(index.keyFor(cuA, nA), index.keyFor(cuB, nB));

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> rA, rB;
    const TypeId hNA    = reinternType(cuA, nA, host, rA, index);
    const TypeId hOuter = reinternType(cuB, outer, host, rB, index);

    auto const& hi = host.interner();
    ASSERT_EQ(hi.operands(hOuter).size(), 1u);
    const TypeId nUnderOuter = hi.operands(hi.operands(hOuter)[0])[0];
    EXPECT_EQ(hNA.v, nUnderOuter.v)
        << "one `struct N` reached at two different walk depths is one type";
    EXPECT_EQ(hi.operands(hi.operands(hNA)[0])[0].v, hNA.v)
        << "and it is still self-referential in the host";
}

// Mutually recursive composites must unify PAIRWISE across CUs and must never
// collapse into each other. `A` and `B` have identical field COUNTS and identical
// field KINDS — only the tags and the cycle direction tell them apart — so this
// fails loudly if the digest drops the tag or mishandles the backreference.
TEST(TypeReinternCompositeIdentity, MutuallyRecursiveCompositesUnifyPairwise) {
    auto definePair = [](TypeInterner& cu, std::uint64_t base) {
        const TypeId a = cu.forwardComposite(TypeKind::Struct, "A", base);
        const TypeId b = cu.forwardComposite(TypeKind::Struct, "B", base + 1);
        std::array<TypeId, 1> const aFields{cu.pointer(b)};
        std::array<TypeId, 1> const bFields{cu.pointer(a)};
        cu.completeComposite(a, aFields, /*packed=*/false);
        cu.completeComposite(b, bFields, /*packed=*/false);
        return std::pair<TypeId, TypeId>{a, b};
    };

    TypeInterner cuA{CompilationUnitId{1}};
    TypeInterner cuB{CompilationUnitId{2}};
    auto const [a1, b1] = definePair(cuA, 100);
    auto const [a2, b2] = definePair(cuB, 900);

    CompositeIdentityIndex index;
    index.observe(cuA);
    index.observe(cuB);

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> rA, rB;
    const TypeId hA1 = reinternType(cuA, a1, host, rA, index);
    const TypeId hB1 = reinternType(cuA, b1, host, rA, index);
    const TypeId hA2 = reinternType(cuB, a2, host, rB, index);
    const TypeId hB2 = reinternType(cuB, b2, host, rB, index);

    EXPECT_EQ(hA1.v, hA2.v);
    EXPECT_EQ(hB1.v, hB2.v);
    EXPECT_NE(hA1.v, hB1.v)
        << "`A` and `B` differ only by tag and cycle direction; collapsing them "
           "would be a layout merge C does not license";
}

// ⚠ EVERY CHANNEL reinternType CARRIES ACROSS MUST BE IN THE DIGEST, or two
// composites the digest calls equal would reintern to DIFFERENT layouts and the
// merge would pick one. `packed` is the cheapest witness for the whole class: two
// same-tag structs identical but for it must stay two types.
TEST(TypeReinternCompositeIdentity, PackedIsPartOfCompositeIdentity) {
    TypeInterner cuA{CompilationUnitId{1}};
    const TypeId a = cuA.forwardComposite(TypeKind::Struct, "W", 1);
    std::array<TypeId, 2> const aFields{cuA.primitive(TypeKind::I8),
                                        cuA.primitive(TypeKind::I32)};
    cuA.completeComposite(a, aFields, /*packed=*/true);

    TypeInterner cuB{CompilationUnitId{2}};
    const TypeId b = cuB.forwardComposite(TypeKind::Struct, "W", 1);
    std::array<TypeId, 2> const bFields{cuB.primitive(TypeKind::I8),
                                        cuB.primitive(TypeKind::I32)};
    cuB.completeComposite(b, bFields, /*packed=*/false);

    CompositeIdentityIndex index;
    index.observe(cuA);
    index.observe(cuB);

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> rA, rB;
    const TypeId hA = reinternType(cuA, a, host, rA, index);
    const TypeId hB = reinternType(cuB, b, host, rB, index);
    EXPECT_NE(hA.v, hB.v);
    EXPECT_TRUE(host.interner().isPacked(hA));
    EXPECT_FALSE(host.interner().isPacked(hB));
}

// The four-argument overload keeps working and keeps its single-source meaning:
// it observes `src` and nothing else, so a definition IN THAT INTERNER still
// resolves its own forward declaration.
TEST(TypeReinternCompositeIdentity, SingleSourceOverloadResolvesItsOwnDefinition) {
    TypeInterner cu{CompilationUnitId{1}};
    const TypeId def = defineBitvec(cu, 3);
    const TypeId fwd = cu.forwardComposite(TypeKind::Struct, "Bitvec", 4);
    ASSERT_TRUE(cu.isIncompleteComposite(fwd));

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> remap;
    const TypeId hDef = reinternType(cu, def, host, remap);
    const TypeId hFwd = reinternType(cu, fwd, host, remap);
    EXPECT_EQ(hDef.v, hFwd.v);
}

// ★★★ THE SECOND PASS, AND THE FIRST ONE SHIPPED WITHOUT IT. Keying a
// composite on its FULL recursive layout still forked `struct BtCursor` on the
// 103-TU sqlite corpus, and the reason is this shape: a TU that has seen
// `struct Inner { ... }` and a TU that has only seen `struct Inner;` give the
// ENCLOSING struct two different digests — even though the enclosing struct is
// byte-for-byte identical in both, because a pointer is one word whatever it
// points at. The completeness of something you only POINT AT was leaking into
// your own identity: the same defect this file fixes, one level down.
// RED-ON-DISABLE: make a named composite behind a pointer contribute its layout
// again and this test forks the two `Outer`s.
TEST(TypeReinternCompositeIdentity, PointeeCompletenessDoesNotForkTheEnclosingType) {
    // CU 1 sees the definition of `Inner`.
    TypeInterner cuSees{CompilationUnitId{1}};
    const TypeId innerDef = cuSees.forwardComposite(TypeKind::Struct, "Inner", 1);
    std::array<TypeId, 1> const innerFields{cuSees.primitive(TypeKind::I32)};
    cuSees.completeComposite(innerDef, innerFields, /*packed=*/false);
    const TypeId outerA = cuSees.forwardComposite(TypeKind::Struct, "Outer", 2);
    std::array<TypeId, 1> const outerAFields{cuSees.pointer(innerDef)};
    cuSees.completeComposite(outerA, outerAFields, /*packed=*/false);

    // CU 2 has only the forward declaration — the ordinary opaque-handle header.
    TypeInterner cuBlind{CompilationUnitId{2}};
    const TypeId innerFwd = cuBlind.forwardComposite(TypeKind::Struct, "Inner", 1);
    const TypeId outerB   = cuBlind.forwardComposite(TypeKind::Struct, "Outer", 2);
    std::array<TypeId, 1> const outerBFields{cuBlind.pointer(innerFwd)};
    cuBlind.completeComposite(outerB, outerBFields, /*packed=*/false);

    CompositeIdentityIndex index;
    index.observe(cuSees);
    index.observe(cuBlind);

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> rA, rB;
    const TypeId hA = reinternType(cuSees, outerA, host, rA, index);
    const TypeId hB = reinternType(cuBlind, outerB, host, rB, index);

    EXPECT_EQ(hA.v, hB.v)
        << "`Outer` is one word wide in both CUs and identical in every member; "
           "what its member points AT cannot be part of ITS identity";
    // And `Inner` itself still unifies onto the one definition that exists.
    auto const& hi = host.interner();
    ASSERT_EQ(hi.operands(hA).size(), 1u);
    const TypeId hInner = hi.operands(hi.operands(hA)[0])[0];
    EXPECT_FALSE(hi.isIncompleteComposite(hInner));
}

// ⚠ THE GUARD THE CLAUSE ABOVE MUST NOT BREAK: relaxing the POINTEE does not
// relax the MEMBER. A struct that EMBEDS a composite by value takes that
// composite's layout into its own, so two same-tag structs embedding different
// layouts are still two types. This is the line between "one word whatever it
// points at" and "my size depends on yours".
TEST(TypeReinternCompositeIdentity, AnEmbeddedMembersLayoutIsStillPartOfIdentity) {
    auto build = [](TypeInterner& cu, TypeKind memberKind) {
        const TypeId inner = cu.forwardComposite(TypeKind::Struct, "Inner", 1);
        std::array<TypeId, 1> const innerFields{cu.primitive(memberKind)};
        cu.completeComposite(inner, innerFields, /*packed=*/false);
        const TypeId outer = cu.forwardComposite(TypeKind::Struct, "Outer", 2);
        std::array<TypeId, 1> const outerFields{inner};   // BY VALUE, not a Ptr
        cu.completeComposite(outer, outerFields, /*packed=*/false);
        return outer;
    };

    TypeInterner cuA{CompilationUnitId{1}};
    TypeInterner cuB{CompilationUnitId{2}};
    const TypeId a = build(cuA, TypeKind::I32);
    const TypeId b = build(cuB, TypeKind::F64);

    CompositeIdentityIndex index;
    index.observe(cuA);
    index.observe(cuB);

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> rA, rB;
    EXPECT_NE(reinternType(cuA, a, host, rA, index).v,
              reinternType(cuB, b, host, rB, index).v)
        << "an embedded member's layout IS this struct's layout; merging these "
           "would be a size and offset miscompile";
}

// A pointer's pointee TAG still matters: `struct A { struct B *p; }` and
// `struct A { struct C *p; }` are different types and neither the relaxation
// above nor the memo may collapse them.
TEST(TypeReinternCompositeIdentity, PointeeTagIsStillPartOfIdentity) {
    auto build = [](TypeInterner& cu, char const* pointeeTag) {
        const TypeId pointee = cu.forwardComposite(TypeKind::Struct, pointeeTag, 1);
        const TypeId outer   = cu.forwardComposite(TypeKind::Struct, "A", 2);
        std::array<TypeId, 1> const fields{cu.pointer(pointee)};
        cu.completeComposite(outer, fields, /*packed=*/false);
        return outer;
    };

    TypeInterner cuA{CompilationUnitId{1}};
    TypeInterner cuB{CompilationUnitId{2}};
    const TypeId a = build(cuA, "B");
    const TypeId b = build(cuB, "C");

    CompositeIdentityIndex index;
    index.observe(cuA);
    index.observe(cuB);

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> rA, rB;
    EXPECT_NE(reinternType(cuA, a, host, rA, index).v,
              reinternType(cuB, b, host, rB, index).v);
}

// ★★★ THE DEFECT THAT SURVIVED TWO CORRECT-LOOKING FIXES, AND THE ONLY REASON
// IT WAS FOUND IS THAT THE INDEX COUNTS ITS OWN FAILURES. An anonymous member is
// named `<anon:RULE:NODEID>` where NODEID is a per-CU AST node index, so two CUs
// that include the same header give one anonymous `union` two names — and every
// NAMED struct that reaches it inherits the split. ✔MEASURED on 103-TU sqlite:
// 98 tags forked, `Parse` / `Table` / `Select` / `Index` among them, EVERY ONE
// with a single local layout signature — which is what proved the split could
// not be a real layout difference.
// RED-ON-DISABLE: mix the raw name instead of `anonNameWithoutDeclSite` and the
// two `Outer`s fork while `forkedTagCount()` goes to 1.
TEST(TypeReinternCompositeIdentity, AnonymousMemberDeclSiteDoesNotForkItsParent) {
    // Two CUs, same header, DIFFERENT synthetic node ids for the anonymous union.
    auto build = [](TypeInterner& cu, char const* anonName) {
        const TypeId an = cu.forwardComposite(TypeKind::Union, anonName, 1);
        std::array<TypeId, 2> const anFields{cu.primitive(TypeKind::I32),
                                             cu.primitive(TypeKind::F32)};
        cu.completeComposite(an, anFields, /*packed=*/false);
        const TypeId outer = cu.forwardComposite(TypeKind::Struct, "Outer", 2);
        std::array<TypeId, 1> const oFields{an};   // embedded anonymous union
        cu.completeComposite(outer, oFields, /*packed=*/false);
        return outer;
    };

    TypeInterner cuA{CompilationUnitId{1}};
    TypeInterner cuB{CompilationUnitId{2}};
    const TypeId a = build(cuA, "<anon:unionSpec:65291>");
    const TypeId b = build(cuB, "<anon:unionSpec:71255>");

    CompositeIdentityIndex index;
    index.observe(cuA);
    index.observe(cuB);

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> rA, rB;
    const TypeId hA = reinternType(cuA, a, host, rA, index);
    const TypeId hB = reinternType(cuB, b, host, rB, index);

    EXPECT_EQ(hA.v, hB.v)
        << "the two `Outer`s differ only in WHERE their anonymous member was "
           "written, which is not part of any type's identity";
    EXPECT_EQ(index.forkedTagCount(), 0u);
    // The anonymous member itself unified too, and it must, or `Outer`'s two
    // completions would disagree on its field type and abort.
    ASSERT_EQ(host.interner().operands(hA).size(), 1u);
    EXPECT_EQ(host.interner().kind(host.interner().operands(hA)[0]),
              TypeKind::Union);
}

// The rule keeps the RULE and drops only the node id, so a `structSpec` and a
// `unionSpec` anonymous member of otherwise identical shape stay two types —
// they overlay their fields differently, and merging them would be a layout
// miscompile rather than a de-duplication.
TEST(TypeReinternCompositeIdentity, AnonymousStructAndUnionStayDistinct) {
    auto build = [](TypeInterner& cu, TypeKind k, char const* anonName) {
        const TypeId an = cu.forwardComposite(k, anonName, 1);
        std::array<TypeId, 2> const anFields{cu.primitive(TypeKind::I32),
                                             cu.primitive(TypeKind::F32)};
        cu.completeComposite(an, anFields, /*packed=*/false);
        const TypeId outer = cu.forwardComposite(TypeKind::Struct, "Outer", 2);
        std::array<TypeId, 1> const oFields{an};
        cu.completeComposite(outer, oFields, /*packed=*/false);
        return outer;
    };

    TypeInterner cuA{CompilationUnitId{1}};
    TypeInterner cuB{CompilationUnitId{2}};
    const TypeId a = build(cuA, TypeKind::Union,  "<anon:unionSpec:100>");
    const TypeId b = build(cuB, TypeKind::Struct, "<anon:structSpec:200>");

    CompositeIdentityIndex index;
    index.observe(cuA);
    index.observe(cuB);

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> rA, rB;
    EXPECT_NE(reinternType(cuA, a, host, rA, index).v,
              reinternType(cuB, b, host, rB, index).v);
}

// ⚠ THE FIXED POINT MUST CONVERGE, NOT HIT ITS BOUND. A bound that BOUND means
// two distinct types share a key, which `completeComposite` then reports by
// aborting — loudly, but only at the end of a long build. `refinementRounds()`
// is exposed so the property can be asserted where it is cheap.
// ⓘ The naive alternative — a recursive digest cutting cycles with a de Bruijn
// backreference — cannot memoize any subtree that back-references an outer
// composite, so on a mutually recursive cluster it re-expands per path.
// ✔MEASURED on 103-TU sqlite: 952 s of CPU and no output. This converges in 2.
TEST(TypeReinternCompositeIdentity, MutualRecursionConvergesWithoutHittingTheBound) {
    TypeInterner cu{CompilationUnitId{1}};
    // A ring of four composites, each pointing at the next: the shape a de
    // Bruijn walk cannot memoize and refinement handles as ordinary edges.
    std::array<TypeId, 4> ring{};
    for (std::uint32_t i = 0; i < 4; ++i)
        ring[i] = cu.forwardComposite(TypeKind::Struct,
                                      std::string("R") + char('0' + i), 10 + i);
    for (std::uint32_t i = 0; i < 4; ++i) {
        std::array<TypeId, 2> const f{cu.pointer(ring[(i + 1) % 4]),
                                      cu.primitive(TypeKind::I32)};
        cu.completeComposite(ring[i], f, /*packed=*/false);
    }

    CompositeIdentityIndex index;
    index.observe(cu);

    TypeLattice host{CompilationUnitId{9}};
    std::unordered_map<std::uint32_t, TypeId> remap;
    for (TypeId t : ring) EXPECT_TRUE(reinternType(cu, t, host, remap, index).valid());

    EXPECT_EQ(index.forkedTagCount(), 0u);
    EXPECT_GT(index.refinementRounds(), 0u);
    EXPECT_LT(index.refinementRounds(), index.nodeCount() + 2)
        << "the refinement hit its backstop instead of converging, which means "
           "two distinct types may share a key";
}
