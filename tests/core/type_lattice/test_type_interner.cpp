// SP2: TypeInterner canonicalization + cross-CU TypeId isolation.

#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <set>
#include <type_traits>
#include <vector>

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

// D-CSUBSET-MEMBER-ALIGNAS: a member-alignas override enters the struct's CONTENT
// identity. Zero-churn: the 2-arg overload and the new 5-arg overload with an EMPTY
// aligns span produce the SAME TypeId (an align-free struct interns byte-identically,
// exactly like the offsets channel). An aligns={16} version is a DISTINCT TypeId.
// RED-ON-DISABLE: were the aligns dropped from `contentDeclSiteKey`, the align-bearing
// struct would alias its natural twin and `hasExplicitAligns` would still be false.
TEST(TypeInterner, MemberAlignsEnterContentIdentityZeroChurn) {
    auto ti = makeInterner(1);
    const TypeId i32 = ti.primitive(TypeKind::I32);
    std::array<TypeId, 1>        const field{i32};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};

    // 2-arg (no aligns) vs the 5-arg overload with an EMPTY aligns span → SAME TypeId.
    const TypeId plain = ti.structType("S", field);
    std::array<std::uint32_t, 0> const noAligns{};
    const TypeId plainViaNew = ti.structType("S", field, noWidths, noOffs, noAligns);
    EXPECT_EQ(plain.v, plainViaNew.v)
        << "an empty aligns span must intern byte-identically to the 2-arg overload";
    EXPECT_FALSE(ti.hasExplicitAligns(plain));

    // An aligns={16} version is a DISTINCT interned type (identity mix works).
    std::array<std::uint32_t, 1> const aligns16{16};
    const TypeId aligned = ti.structType("S", field, noWidths, noOffs, aligns16);
    EXPECT_NE(aligned.v, plain.v)
        << "a member-aligned struct must not alias its natural-alignment twin";
    EXPECT_TRUE(ti.hasExplicitAligns(aligned));
    EXPECT_EQ(ti.explicitFieldAlign(aligned, 0), 16u);

    // Re-interning the SAME aligns canonicalizes to the SAME TypeId (content dedup).
    const TypeId aligned2 = ti.structType("S", field, noWidths, noOffs, aligns16);
    EXPECT_EQ(aligned.v, aligned2.v);

    // A plain struct returns 0 (no override) for every field via explicitFieldAlign.
    EXPECT_EQ(ti.explicitFieldAlign(plain, 0), 0u);
}

// ── D-CSUBSET-SELF-REFERENTIAL-STRUCT: nominal composites ───────────────────

TEST(TypeInterner, SelfReferentialStructForwardMintCompletes) {
    // The crux: a struct whose field points to ITSELF. Forward-mint the nominal
    // TypeId FIRST (no fields yet), build `Ptr<N>` against it, then complete with
    // [Ptr<N>, i32]. field[0] resolves back to N — no un-internable cycle, no wart.
    auto ti = makeInterner(1);
    const TypeId i32 = ti.primitive(TypeKind::I32);
    const TypeId n   = ti.forwardComposite(TypeKind::Struct, "N", /*declSiteKey=*/1001);
    EXPECT_TRUE(ti.isIncompleteComposite(n));     // forward-only ⇒ incomplete
    const TypeId ptrN = ti.pointer(n);            // the self-referential field type
    std::array<TypeId, 2> const fields{ptrN, i32};
    ti.completeComposite(n, fields);
    EXPECT_FALSE(ti.isIncompleteComposite(n));     // now complete
    ASSERT_EQ(ti.operands(n).size(), 2u);
    EXPECT_EQ(ti.operands(n)[0].v, ptrN.v);        // field[0] = Ptr<N>
    EXPECT_EQ(ti.operands(n)[1].v, i32.v);
    EXPECT_EQ(ti.operands(ti.operands(n)[0]).size(), 1u);
    EXPECT_EQ(ti.operands(ti.operands(n)[0])[0].v, n.v);   // Ptr<N> pointee IS n
    EXPECT_EQ(ti.kind(n), TypeKind::Struct);
    EXPECT_EQ(ti.name(n), "N");
}

TEST(TypeInterner, ForwardCompositeDedupsSameDeclSiteNoDuplicate) {
    // The byHash-dedup charge: a re-mint of the SAME (kind, name, declSiteKey)
    // returns the EXISTING forward record — "one composite → one TypeId" — so the
    // Pass-1.5 completion and the self-ref field never split into two TypeIds.
    auto ti = makeInterner(1);
    const std::size_t before = ti.size();
    const TypeId a = ti.forwardComposite(TypeKind::Struct, "N", 7);
    const TypeId b = ti.forwardComposite(TypeKind::Struct, "N", 7);   // same decl-site
    EXPECT_EQ(a.v, b.v);
    EXPECT_EQ(ti.size(), before + 1);            // exactly ONE new type, not two
}

TEST(TypeInterner, DeclSiteIdentityKeepsSameNameDistinct) {
    // IDENTITY-SAFETY: two same-name composites with DIFFERENT decl-sites (e.g. a
    // file-scope `struct S` and a block-scoped `struct S` of different layout in
    // ONE CU) must stay DISTINCT TypeIds. A name-only identity would merge them →
    // wrong field offsets (silent layout miscompile). RED-ON-DISABLE: drop the
    // declSiteKey from the identity and these become equal.
    auto ti = makeInterner(1);
    const TypeId s1 = ti.forwardComposite(TypeKind::Struct, "S", 100);
    const TypeId s2 = ti.forwardComposite(TypeKind::Struct, "S", 200);
    EXPECT_NE(s1.v, s2.v);
    // ...and they carry INDEPENDENT field lists.
    const TypeId i32 = ti.primitive(TypeKind::I32);
    const TypeId f64 = ti.primitive(TypeKind::F64);
    std::array<TypeId, 1> const fa{i32};
    std::array<TypeId, 2> const fb{i32, f64};
    ti.completeComposite(s1, fa);
    ti.completeComposite(s2, fb);
    EXPECT_EQ(ti.operands(s1).size(), 1u);
    EXPECT_EQ(ti.operands(s2).size(), 2u);
}

TEST(TypeInterner, CompleteEmptyStructIsNotIncomplete) {
    // `struct E {}` is a LEGAL COMPLETE zero-field struct (size 0) — NOT incomplete.
    // The flag is EXPLICIT, never "operands empty".
    auto ti = makeInterner(1);
    const TypeId e = ti.forwardComposite(TypeKind::Struct, "E", 5);
    EXPECT_TRUE(ti.isIncompleteComposite(e));     // before completion
    ti.completeComposite(e, {});                  // complete with ZERO fields
    EXPECT_FALSE(ti.isIncompleteComposite(e));     // a complete empty struct
    EXPECT_TRUE(ti.operands(e).empty());
    // A non-composite is never "incomplete" here.
    EXPECT_FALSE(ti.isIncompleteComposite(ti.primitive(TypeKind::I32)));
}

TEST(TypeInterner, StructTypeOverloadRoutesToForwardCompleteNoDuplicate) {
    // The 2-arg/3-arg convenience overloads MUST forward-mint + complete and
    // CANONICALIZE: two structType("S", sameFields) calls collapse to one TypeId
    // (so shipped descriptors + reintern stay byte-identical) and the result is
    // COMPLETE. A duplicate-minting bug would make these distinct or leave the
    // type incomplete.
    auto ti = makeInterner(1);
    const TypeId i32 = ti.primitive(TypeKind::I32);
    std::array<TypeId, 1> const fields{i32};
    const std::size_t before = ti.size();
    const TypeId a = ti.structType("S", fields);
    const TypeId b = ti.structType("S", fields);   // identical content
    EXPECT_EQ(a.v, b.v);                            // canonical (no duplicate)
    EXPECT_EQ(ti.size(), before + 1);              // exactly one new composite
    EXPECT_FALSE(ti.isIncompleteComposite(a));      // complete-at-once
    ASSERT_EQ(ti.operands(a).size(), 1u);
    EXPECT_EQ(ti.operands(a)[0].v, i32.v);
}

TEST(TypeInternerDeathTest, CompleteCompositeConflictingReCompletionAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto trigger = [] {
        auto ti = makeInterner(1);
        const TypeId i32 = ti.primitive(TypeKind::I32);
        const TypeId f64 = ti.primitive(TypeKind::F64);
        const TypeId n   = ti.forwardComposite(TypeKind::Struct, "N", 1);
        std::array<TypeId, 1> const fa{i32};
        std::array<TypeId, 1> const fb{f64};
        ti.completeComposite(n, fa);
        ti.completeComposite(n, fb);   // DIFFERENT fields → caller bug → abort
    };
    EXPECT_DEATH({ trigger(); }, "re-completed with different fields");
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

    // D-LANG-VARIADIC (step 13.4): the 3-arg overload encodes non-
    // variadic (scalars=[cc], length 1) — backward-compat default
    // for every pre-13.4 call site.
    EXPECT_FALSE(ti.fnIsVariadic(sig));
}

TEST(TypeInterner, FnSigVariadicEncodingDistinctFromNonVariadic) {
    // D-LANG-VARIADIC (step 13.4): the 4-arg `fnSig(...isVariadic)`
    // overload encodes scalars=[cc, isVariadic]. A variadic and a
    // non-variadic signature over the same param / result / cc
    // INTERN AS DISTINCT TypeIds (scalar count + value differ), and
    // `fnIsVariadic` decodes correctly for both.
    auto ti = makeInterner(1);
    const TypeId i32     = ti.primitive(TypeKind::I32);
    const TypeId charTy  = ti.primitive(TypeKind::Char);
    const TypeId ptrChar = ti.pointer(charTy);
    std::array<TypeId, 1> const fixedParams{ptrChar};

    const TypeId nonVar = ti.fnSig(fixedParams, i32, CallConv::CcSysV);
    const TypeId varSig = ti.fnSig(fixedParams, i32, CallConv::CcSysV,
                                   /*isVariadic=*/true);
    EXPECT_NE(nonVar.v, varSig.v);
    EXPECT_FALSE(ti.fnIsVariadic(nonVar));
    EXPECT_TRUE (ti.fnIsVariadic(varSig));
    // Fixed-param accessor sees only the declared params (no
    // synthetic "vararg" entry).
    EXPECT_EQ(ti.fnParams(varSig).size(), 1u);
    // Re-interning with the same (params, result, cc, isVariadic)
    // tuple is canonical.
    EXPECT_EQ(varSig.v,
              ti.fnSig(fixedParams, i32, CallConv::CcSysV, true).v);
    // Scalar encoding: 1 slot for non-variadic (cc only), 2 for
    // variadic (cc + isVariadic=1).
    EXPECT_EQ(ti.scalars(nonVar).size(), 1u);
    EXPECT_EQ(ti.scalars(varSig).size(), 2u);
    EXPECT_EQ(ti.scalars(varSig)[0],
              static_cast<std::int64_t>(CallConv::CcSysV));
    EXPECT_EQ(ti.scalars(varSig)[1], std::int64_t{1});
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

// ── D-TYPEINTERNER-OPERAND-SPAN-LIFETIME-GUARD ──────────────────────────────
// operands()/scalars()/fnParams() return a VIEW into the interner pool. Holding
// it across a later intern (which may realloc the pool) is a heap-use-after-free
// that bit TWICE (hir_to_mir 4660 + 2490), host-dependent and INVISIBLE to the
// Windows-only non-ASan local gate. The debug GuardedSpan makes that read abort
// DETERMINISTICALLY on EVERY host — no ASan, no realloc-luck needed.

TEST(TypeInternerDeathTest, StaleOperandSpanAfterInternAborts) {
#ifdef NDEBUG
    GTEST_SKIP() << "GuardedSpan is a zero-overhead std::span alias in release; "
                    "the deterministic span-lifetime guard is debug-only.";
#else
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // RED-ON-DISABLE: remove the `check_()` call inside GuardedSpan and this stops
    // aborting → EXPECT_DEATH fails. Hoisted into a lambda (not inlined in the
    // macro) because the `std::array<TypeId, 1>` comma would split the EXPECT_DEATH
    // argument list; self-contained so the threadsafe re-exec reproduces it
    // deterministically (no allocator/realloc dependence).
    auto trigger = [] {
        auto ti = makeInterner(1);
        const TypeId i32 = ti.primitive(TypeKind::I32);
        std::array<TypeId, 1> const fields{i32};
        const TypeId s = ti.structType("S", fields);
        auto ops = ti.operands(s);            // GuardedSpan captures the pool gen
        (void)ti.primitive(TypeKind::F64);    // a NEW intern bumps the gen
        (void)ops.size();                     // stale read → loud abort
    };
    EXPECT_DEATH({ trigger(); }, "stale operand/scalar span");
#endif
}

// The guard does NOT false-fire on correct use: an immediate read is fine, and
// the copy-to-owning-vector FIX pattern survives any number of later interns.
TEST(TypeInterner, FreshSpanAndOwningCopySurviveIntern) {
    auto ti = makeInterner(1);
    const TypeId i32 = ti.primitive(TypeKind::I32);
    const TypeId f32 = ti.primitive(TypeKind::F32);
    std::array<TypeId, 2> const fields{i32, f32};
    const TypeId s = ti.structType("S", fields);

    auto ops = ti.operands(s);
    ASSERT_EQ(ops.size(), 2u);          // immediate read — no intervening intern
    EXPECT_EQ(ops[0].v, i32.v);
    EXPECT_EQ(ops[1].v, f32.v);

    // Own-before-mutate (the fix pattern): copy out, THEN intern freely.
    std::vector<TypeId> const owned(ops.begin(), ops.end());
    (void)ti.array(i32, 8);             // mutate the pool (new types)
    (void)ti.primitive(TypeKind::F64);
    ASSERT_EQ(owned.size(), 2u);        // the owning copy is immune to the realloc
    EXPECT_EQ(owned[0].v, i32.v);
    EXPECT_EQ(owned[1].v, f32.v);
}

// ── c27 (D-CSUBSET-VOLATILE-POINTEE): TypeKind::VolatileQual ──
//
// `volatile T` is a DISTINCT interned identity that is a TRANSPARENT skin: it has
// its own TypeId (so `volatile int` != `int` for interning/equality — what carries
// the volatile through a declaration's type to its access), but the `kind()` /
// `operands()` accessors SEE THROUGH it to the material type, so every structural
// consumer dispatches on the underlying kind WITHOUT a per-site strip. The wrapper
// is observable only via `isVolatileQualified` / `get()`.

TEST(TypeInternerVolatile, DistinctIdentityButTransparentKind) {
    auto ti = makeInterner(1);
    const TypeId i32  = ti.primitive(TypeKind::I32);
    const TypeId vi32 = ti.volatileQualified(i32);
    // DISTINCT TypeId — the wrapper is real (not a no-op that returns `i32`).
    EXPECT_NE(vi32.v, i32.v);
    EXPECT_TRUE(ti.isVolatileQualified(vi32));
    EXPECT_FALSE(ti.isVolatileQualified(i32));
    // TRANSPARENT: kind() reports the MATERIAL kind (I32, not VolatileQual) so the
    // ~128 kind-dispatch sites are correct by construction.
    EXPECT_EQ(ti.kind(vi32), TypeKind::I32);
    EXPECT_EQ(ti.kind(i32), TypeKind::I32);
    // The RAW record (get) preserves the wrapper for reintern / text round-trip.
    EXPECT_EQ(ti.get(vi32).kind, TypeKind::VolatileQual);
    // stripVolatile recovers the material id; idempotent on a non-qualified id.
    EXPECT_EQ(ti.stripVolatile(vi32).v, i32.v);
    EXPECT_EQ(ti.stripVolatile(i32).v, i32.v);
}

TEST(TypeInternerVolatile, Canonicalizes) {
    auto ti = makeInterner(1);
    const TypeId i32  = ti.primitive(TypeKind::I32);
    const TypeId a = ti.volatileQualified(i32);
    const TypeId b = ti.volatileQualified(i32);
    EXPECT_EQ(a.v, b.v);   // `volatile int` interns to ONE TypeId
}

TEST(TypeInternerVolatile, Idempotent) {
    auto ti = makeInterner(1);
    const TypeId i32  = ti.primitive(TypeKind::I32);
    const TypeId v1 = ti.volatileQualified(i32);
    const TypeId v2 = ti.volatileQualified(v1);   // volatile (volatile int)
    EXPECT_EQ(v1.v, v2.v);   // ≡ volatile int (C 6.7.3p5) — no double-wrap
    EXPECT_EQ(ti.get(v2).kind, TypeKind::VolatileQual);
    EXPECT_EQ(ti.stripVolatile(v2).v, i32.v);   // one level, fully stripped
}

TEST(TypeInternerVolatile, InvalidInnerYieldsInvalid) {
    auto ti = makeInterner(1);
    EXPECT_FALSE(ti.volatileQualified(InvalidType).valid());
}

TEST(TypeInternerVolatile, PointerBindsInnermostPointee) {
    // `volatile u32 *` = Ptr<VolatileQual(u32)> (volatile binds the pointee,
    // C 6.7.3). The outer Ptr is a REAL pointer (kind Ptr), its operand is the
    // volatile-qualified pointee — what a deref reads to drive the access flag.
    auto ti = makeInterner(1);
    const TypeId u32  = ti.primitive(TypeKind::U32);
    const TypeId vu32 = ti.volatileQualified(u32);
    const TypeId p    = ti.pointer(vu32);
    EXPECT_EQ(ti.kind(p), TypeKind::Ptr);          // the pointer itself is plain
    EXPECT_FALSE(ti.isVolatileQualified(p));
    auto const ops = ti.operands(p);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_TRUE(ti.isVolatileQualified(ops[0]));    // the pointee IS volatile
    EXPECT_EQ(ti.kind(ops[0]), TypeKind::U32);      // material kind U32

    // EAST `u32 * volatile` = VolatileQual(Ptr<u32>) (a volatile POINTER): the
    // wrapper is on the OUTSIDE; kind is transparently Ptr, operand is plain u32.
    const TypeId pu32  = ti.pointer(u32);
    const TypeId vpu32 = ti.volatileQualified(pu32);
    EXPECT_TRUE(ti.isVolatileQualified(vpu32));
    EXPECT_EQ(ti.kind(vpu32), TypeKind::Ptr);       // transparent: a pointer
    EXPECT_NE(vpu32.v, p.v);   // `volatile u32 *` != `u32 * volatile` (distinct)
}

TEST(TypeInternerVolatile, OperandsAndScalarsSeeThroughToComposite) {
    // A `volatile struct S` redirects operands()/scalars() to S's fields, so a
    // layout/ABI consumer reading a volatile-qualified struct sees its real shape.
    auto ti = makeInterner(1);
    const TypeId i32 = ti.primitive(TypeKind::I32);
    const TypeId f32 = ti.primitive(TypeKind::F32);
    std::array<TypeId, 2> const fields{i32, f32};
    const TypeId s  = ti.structType("S", fields);
    const TypeId vs = ti.volatileQualified(s);
    EXPECT_EQ(ti.kind(vs), TypeKind::Struct);       // transparent struct
    auto const ops = ti.operands(vs);               // redirected to S's fields
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0].v, i32.v);
    EXPECT_EQ(ops[1].v, f32.v);
}
