// FC7 (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): the by-value aggregate ABI classifier.
// Exhaustive SysV AMD64 eightbyte truth-table — every form built directly via the
// interner (incl. forms no shipped c program reaches yet). A wrong
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
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// Shipped-target params: natural alignment, 16-byte ISA cap, LP64.
constexpr AggregateLayoutParams kNatural16{ScalarAlignmentRule::Natural, 16};

// ★ P69 (lane `cs`, fold f26d — D-TARGET-A-WIDE-BITINT-TAKES-THE-X86-64-ALIGNMENT-ON-ARM64):
// a wide `_BitInt` leaf is sized by the TARGET's chunk, so each convention classifies
// under its own target's: x86_64 (SysV, Win64) 8, arm64 (AAPCS64) 16.
[[nodiscard]] constexpr AggregateLayoutParams withBitPreciseChunk(AggregateLayoutParams p,
                                                                  std::uint32_t chunk) noexcept {
    p.bitPreciseChunk = chunk;
    return p;
}
constexpr AggregateLayoutParams kX86_64Layout = withBitPreciseChunk(kNatural16, 8);
constexpr AggregateLayoutParams kArm64Layout  = withBitPreciseChunk(kNatural16, 16);

[[nodiscard]] TypeInterner makeInterner(std::uint32_t owner) {
    return TypeInterner{CompilationUnitId{owner}};
}

[[nodiscard]] std::optional<AbiPassing>
classifySysV(TypeId t, TypeInterner const& ti,
             AggregatePosition position = AggregatePosition::Argument) {
    return classifyAggregate(AggregateConventionRules{AggregateClassKind::SysVEightbyte, 16}, t, ti,
                             kX86_64Layout, DataModel::Lp64, position);
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
// the SysV x87 MEMORY class → the WHOLE aggregate is passed IN MEMORY (X87/X87UP →
// MEMORY, regardless of size). INVERTED from the FC17.9(e) nullopt-fail-loud pin:
// the real rule is now modeled. NOT the SSE/INTEGER eightbyte merge (a float-kind
// join would say SSE → XMM pieces; a non-join says INTEGER = a silent 2-GPR pass,
// ABI-divergent at FFI). As an ARGUMENT (the position below); a RESULT of class X87
// is the next test's.
TEST(AggregateAbiSysV, LongDoubleLeafStructIsPassedInMemory) {
    auto ti = makeInterner(1);
    auto r80 = classifySysV(structOf(ti, "LD", {ti.primitive(TypeKind::F80)}), ti);
    ASSERT_TRUE(r80.has_value());
    EXPECT_EQ(r80->kind, AbiPassing::Kind::Memory)
        << "an F80 (x87 long double) leaf → the SysV MEMORY class";
    EXPECT_TRUE(r80->pieces.empty())
        << "a Memory pass carries NO register pieces";
    auto r128 = classifySysV(structOf(ti, "LQ", {ti.primitive(TypeKind::F128)}), ti);
    ASSERT_TRUE(r128.has_value());
    EXPECT_EQ(r128->kind, AbiPassing::Kind::Memory)
        << "an F128 (binary128) leaf → MEMORY too (X87/X87UP → the whole agg)";
    // Nested: the leaf walk must see THROUGH an inner struct.
    TypeId const inner = structOf(ti, "In", {ti.primitive(TypeKind::F80)});
    auto nested = classifySysV(structOf(ti, "Out", {inner}), ti);
    ASSERT_TRUE(nested.has_value());
    EXPECT_EQ(nested->kind, AbiPassing::Kind::Memory)
        << "a NESTED F80 leaf → MEMORY — the check is leaf-deep";
}

// ★ P69 (lane `cs`, fold f26c — D-TARGET-SYSV-AMD64-MEMORY-CLASS-ARGUMENTS-GO-BY-POINTER-AND-X87-RESULTS-BY-SRET):
// SysV psABI 3.2.3 classes an aggregate whose two eightbytes are X87 and X87UP — its
// ONLY content one x87 `long double` at offset 0 — as passed IN MEMORY but RETURNED in
// %st0 ("If the class is X87, the value is returned on the X87 stack in %st0"). The
// classifier answers by POSITION: X87 for a result — its ONE x87 value at offset 0 the
// piece (f26c2: an X87 result states its x87 values, as a COMPLEX_X87 one does, the next
// test's) — Memory for an argument. Any other leaf merged in, or a size over two
// eightbytes, is MEMORY in both positions. ✔MEASURED 2026-10-07 (lane `lm`'s two-unit
// witness, gcc 13.3.0 and clang 18.1.3): both leave `struct { long double x; }` on the
// x87 stack; a DSS caller read a wrong value through the hidden pointer it assumed.
TEST(AggregateAbiSysV, AnX87OnlyAggregateIsReturnedInX87AndPassedInMemory) {
    auto ti = makeInterner(1);
    TypeId const ld = ti.primitive(TypeKind::F80);
    std::array<TypeId, 2> const twoLd{ld, ld};
    std::array<TypeId, 2> const ldInt{ld, ti.primitive(TypeKind::I32)};
    struct Cell { char const* what; TypeId type; AbiPassing::Kind asResult; };
    std::array<Cell, 7> const cells{{
        {"struct { long double x; }", structOf(ti, "X1", {ld}), AbiPassing::Kind::X87},
        {"struct { long double a[1]; }", structOf(ti, "XA", {ti.array(ld, 1)}),
         AbiPassing::Kind::X87},
        {"union { long double a; long double b; }", ti.unionType("XU", twoLd),
         AbiPassing::Kind::X87},
        {"struct { struct { long double x; } in; }",
         structOf(ti, "XN", {structOf(ti, "XI", {ld})}), AbiPassing::Kind::X87},
        // controls: another leaf in the eightbyte, a second eightbyte pair, a binary128
        // leaf (no x87 class) — MEMORY as a result too. (The bare `_Complex long double`
        // is COMPLEX_X87, the next test's.)
        {"union { long double a; int i; }", ti.unionType("XUI", ldInt),
         AbiPassing::Kind::Memory},
        {"struct { long double a; long b; }",
         structOf(ti, "XL", {ld, ti.primitive(TypeKind::I64)}), AbiPassing::Kind::Memory},
        {"struct { binary128 q; }", structOf(ti, "XQ", {ti.primitive(TypeKind::F128)}),
         AbiPassing::Kind::Memory},
    }};
    for (Cell const& c : cells) {
        auto const asArg = classifySysV(c.type, ti, AggregatePosition::Argument);
        auto const asRes = classifySysV(c.type, ti, AggregatePosition::Result);
        ASSERT_TRUE(asArg.has_value()) << c.what;
        ASSERT_TRUE(asRes.has_value()) << c.what;
        EXPECT_EQ(asArg->kind, AbiPassing::Kind::Memory)
            << c.what << ": passed IN MEMORY (X87/X87UP and MEMORY alike)";
        EXPECT_EQ(asRes->kind, c.asResult) << c.what << ": returned";
        EXPECT_TRUE(asArg->pieces.empty()) << c.what;
        if (c.asResult != AbiPassing::Kind::X87) {
            EXPECT_TRUE(asRes->pieces.empty()) << c.what;
            continue;
        }
        ASSERT_EQ(asRes->pieces.size(), 1u) << c.what << ": one x87 value, in %st0";
        EXPECT_EQ(asRes->pieces[0].cls, AbiPieceClass::Fpr) << c.what;
        EXPECT_EQ(asRes->pieces[0].byteOffset, 0u) << c.what;
        EXPECT_EQ(asRes->pieces[0].widthBytes, 16u) << c.what;
    }
}

// ★ P69 (lane `cs`, fold f26c2 — review-csf26c F3): psABI 3.2.3's COMPLEX_X87. A bare
// `long double _Complex` is RETURNED on the x87 stack as two values — "the real part of
// the value is returned in %st0 and the imaginary part in %st1" — and PASSED in memory
// ("If the class is X87, X87UP or COMPLEX_X87, it is passed in memory"). The class is the
// complex TYPE's: an aggregate holding one is 32 bytes, MEMORY by size in both positions.
// The X87UP rule (`memory` psABI, `sse` Apple's x86_64) does not touch it. Controls: the
// SSE complexes. ✔MEASURED 2026-11-05 (probe cx, -O1 and -O0 asm): gcc 13.3.0 and clang
// 18.1.3 on Linux, Apple clang 21.0.0 for x86_64 — the result in %st0 (real) / %st1
// (imaginary) with no hidden pointer, the argument as 32 stacked bytes, a struct holding
// one through the hidden pointer and on the stack, `float _Complex` in %xmm0, `double
// _Complex` in %xmm0:%xmm1. Before this fold the result went through sret (MEMORY by size).
TEST(AggregateAbiSysV, AComplexLongDoubleIsReturnedAsTwoX87ValuesAndPassedInMemory) {
    auto ti = makeInterner(1);
    TypeId const ld = ti.primitive(TypeKind::F80);
    TypeId const cld = ti.complex(ld);
    using K = AbiPassing::Kind;
    for (X87UpWithoutX87 const rule : {X87UpWithoutX87::Memory, X87UpWithoutX87::Sse}) {
        AggregateConventionRules const rules{AggregateClassKind::SysVEightbyte, 16,
                                             AggregateMemoryArgument::Stack, false, rule};
        auto const classify = [&](TypeId t, AggregatePosition pos) {
            return classifyAggregate(rules, t, ti, kNatural16, DataModel::Lp64, pos);
        };
        SCOPED_TRACE(x87UpWithoutX87Name(rule));
        // The bare type: two x87 values as a result, %st0 the real part at 0, %st1 the
        // imaginary at 16; memory as an argument.
        auto const res = classify(cld, AggregatePosition::Result);
        ASSERT_TRUE(res.has_value());
        EXPECT_EQ(res->kind, K::X87) << "COMPLEX_X87 is returned on the x87 stack, not through sret";
        ASSERT_EQ(res->pieces.size(), 2u) << "the real part in %st0, the imaginary in %st1";
        for (std::size_t k = 0; k < 2; ++k) {
            EXPECT_EQ(res->pieces[k].cls, AbiPieceClass::Fpr) << "piece " << k;
            EXPECT_EQ(res->pieces[k].byteOffset, 16u * k) << "piece " << k << " (x87 stack ordinal " << k << ")";
            EXPECT_EQ(res->pieces[k].widthBytes, 16u) << "piece " << k;
        }
        auto const arg = classify(cld, AggregatePosition::Argument);
        ASSERT_TRUE(arg.has_value());
        EXPECT_EQ(arg->kind, K::Memory) << "COMPLEX_X87 is passed in memory";
        EXPECT_TRUE(arg->pieces.empty());
        // An aggregate holding one: MEMORY by size, both positions.
        std::array<TypeId, 2> const cldLong{cld, ti.primitive(TypeKind::I64)};
        for (auto const& [what, t] :
             {std::pair{"struct { long double _Complex z; }", structOf(ti, "SZ", {cld})},
              std::pair{"struct { long double _Complex z[1]; }", structOf(ti, "SZA", {ti.array(cld, 1)})},
              std::pair{"union { long double _Complex z; long l; }", ti.unionType("UZL", cldLong)}}) {
            for (AggregatePosition const pos : {AggregatePosition::Argument, AggregatePosition::Result}) {
                auto const r = classify(t, pos);
                ASSERT_TRUE(r.has_value()) << what;
                EXPECT_EQ(r->kind, K::Memory) << what << ": MEMORY by size, "
                                              << (pos == AggregatePosition::Result ? "result" : "argument");
                EXPECT_TRUE(r->pieces.empty()) << what;
            }
        }
        // Controls: the SSE complexes, in registers in both positions.
        auto const cf = classify(ti.complex(ti.primitive(TypeKind::F32)), AggregatePosition::Result);
        ASSERT_TRUE(cf.has_value());
        EXPECT_EQ(cf->kind, K::InRegisters) << "float _Complex: %xmm0";
        ASSERT_EQ(cf->pieces.size(), 1u);
        EXPECT_EQ(cf->pieces[0].cls, AbiPieceClass::Fpr);
        auto const cd = classify(ti.complex(ti.primitive(TypeKind::F64)), AggregatePosition::Result);
        ASSERT_TRUE(cd.has_value());
        EXPECT_EQ(cd->kind, K::InRegisters) << "double _Complex: %xmm0:%xmm1";
        ASSERT_EQ(cd->pieces.size(), 2u);
        EXPECT_EQ(cd->pieces[0].cls, AbiPieceClass::Fpr);
        EXPECT_EQ(cd->pieces[1].cls, AbiPieceClass::Fpr);
    }
}

// ★ P69 (lane `cs`, fold f26c, main's ledger 454): AN x87-BEARING AGGREGATE IS CLASSED BY psABI's MERGE, AND AN
// X87UP EIGHTBYTE NOT PRECEDED BY X87 IS THE CONVENTION'S. The universe: a union of `long double` and a member of
// eight bytes or less -- int, long, pointer (INTEGER), double, float (SSE) -- and the two controls whose second
// eightbyte holds that member's type too (long[2], double[2]), as an argument and as a result, under the psABI rule
// (`memory`, sysv_amd64) and Apple's (`sse`, sysv_amd64_darwin). The merge: X87 with INTEGER is INTEGER, X87 with
// SSE is MEMORY; X87UP with INTEGER is INTEGER. ✔MEASURED (probe su, -O1 asm): gcc 13.3.0 and clang 18.1.3 on Linux pass and return the int /
// long / pointer unions in memory, Apple clang 21.0.0 for x86_64 in an integer register and %xmm0; the double and
// float unions and double[2] in memory on all three; long[2] in two integer registers on all three.
TEST(AggregateAbiSysV, AnX87UpEightbyteWithoutX87IsTheConventionsAndTheMergeDecidesTheRest) {
    auto ti = makeInterner(1);
    TypeId const ld = ti.primitive(TypeKind::F80);
    auto const unionOf = [&ti, ld](char const* name, TypeId other) {
        std::array<TypeId, 2> const v{ld, other};
        return ti.unionType(name, v);
    };
    using K = AbiPassing::Kind;
    using P = AbiPieceClass;
    struct Cell {
        char const*        what;
        TypeId             type;
        K                  psabi;     // sysv_amd64 (`memory`): either position
        K                  apple;     // sysv_amd64_darwin (`sse`): either position
        std::array<P, 2>   pieces;    // when InRegisters
    };
    std::array<Cell, 7> const cells{{
        {"union { long double a; int i; }", unionOf("UI", ti.primitive(TypeKind::I32)), K::Memory,
         K::InRegisters, {P::Gpr, P::Fpr}},
        {"union { long double a; long l; }", unionOf("UL", ti.primitive(TypeKind::I64)), K::Memory,
         K::InRegisters, {P::Gpr, P::Fpr}},
        {"union { long double a; void *p; }", unionOf("UP", ti.pointer(ti.primitive(TypeKind::I8))),
         K::Memory, K::InRegisters, {P::Gpr, P::Fpr}},
        {"union { long double a; double d; }", unionOf("UD", ti.primitive(TypeKind::F64)), K::Memory,
         K::Memory, {P::Gpr, P::Gpr}},
        {"union { long double a; float f; }", unionOf("UF", ti.primitive(TypeKind::F32)), K::Memory,
         K::Memory, {P::Gpr, P::Gpr}},
        {"union { long double a; long l2[2]; }", unionOf("UL2", ti.array(ti.primitive(TypeKind::I64), 2)),
         K::InRegisters, K::InRegisters, {P::Gpr, P::Gpr}},
        {"union { long double a; double d2[2]; }", unionOf("UD2", ti.array(ti.primitive(TypeKind::F64), 2)),
         K::Memory, K::Memory, {P::Gpr, P::Gpr}},
    }};
    for (X87UpWithoutX87 const rule : {X87UpWithoutX87::Memory, X87UpWithoutX87::Sse}) {
        AggregateConventionRules const rules{AggregateClassKind::SysVEightbyte, 16,
                                             AggregateMemoryArgument::Stack, false, rule};
        for (Cell const& c : cells) {
            K const want = rule == X87UpWithoutX87::Memory ? c.psabi : c.apple;
            for (AggregatePosition const pos : {AggregatePosition::Argument, AggregatePosition::Result}) {
                auto const r = classifyAggregate(rules, c.type, ti, kNatural16, DataModel::Lp64, pos);
                ASSERT_TRUE(r.has_value()) << c.what;
                EXPECT_EQ(r->kind, want) << c.what << " under '" << x87UpWithoutX87Name(rule) << "', "
                                         << (pos == AggregatePosition::Result ? "result" : "argument");
                if (want != K::InRegisters) {
                    EXPECT_TRUE(r->pieces.empty()) << c.what;
                    continue;
                }
                ASSERT_EQ(r->pieces.size(), 2u) << c.what;
                for (std::size_t e = 0; e < 2; ++e) {
                    EXPECT_EQ(r->pieces[e].cls, c.pieces[e]) << c.what << ", eightbyte " << e;
                    EXPECT_EQ(r->pieces[e].byteOffset, 8u * e) << c.what;
                    EXPECT_EQ(r->pieces[e].widthBytes, 8u) << c.what;
                }
            }
        }
        // The x87 value alone stays what it was under either rule: st0 as a result, memory as an argument.
        TypeId const x1 = structOf(ti, rule == X87UpWithoutX87::Memory ? "XM" : "XS", {ld});
        EXPECT_EQ(classifyAggregate(rules, x1, ti, kNatural16, DataModel::Lp64, AggregatePosition::Result)->kind,
                  K::X87);
        EXPECT_EQ(classifyAggregate(rules, x1, ti, kNatural16, DataModel::Lp64, AggregatePosition::Argument)->kind,
                  K::Memory);
    }
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
    auto r1 = classifyAggregate(AggregateConventionRules{AggregateClassKind::Aapcs64Hfa, 16}, s1, ti,
                                kNatural16, DataModel::Lp64, AggregatePosition::Argument);
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
    auto r2 = classifyAggregate(AggregateConventionRules{AggregateClassKind::Aapcs64Hfa, 16}, s2, ti,
                                kNatural16, DataModel::Lp64, AggregatePosition::Argument);
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
    EXPECT_FALSE(classifyAggregate(AggregateConventionRules{AggregateClassKind::Aapcs64Hfa, 16}, s, ti,
                                   kNatural16, DataModel::Lp64,
                                   AggregatePosition::Argument).has_value())
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
// long-double-complex is MEMORY by the >16B rule (which runs first) — a VALID
// memory pass (the value moves; VALUE arithmetic walls at the ops).
TEST(AggregateAbiAapcs64, Binary128ComplexIsTwoPieceHfa) {
    auto ti = makeInterner(1);
    TypeId const cq = ti.complex(ti.primitive(TypeKind::F128));
    auto r = classifyAggregate(AggregateConventionRules{AggregateClassKind::Aapcs64Hfa, 16}, cq, ti,
                               kNatural16, DataModel::Lp64, AggregatePosition::Argument);
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

TEST(AggregateAbiAapcs64, F80ComplexStillRefusesAndSysVIsMemory) {
    auto ti = makeInterner(1);
    TypeId const cld = ti.complex(ti.primitive(TypeKind::F80));
    EXPECT_FALSE(classifyAggregate(AggregateConventionRules{AggregateClassKind::Aapcs64Hfa, 16}, cld, ti,
                                   kNatural16, DataModel::Lp64,
                                   AggregatePosition::Argument).has_value())
        << "AAPCS64: an F80-element complex has F80 leaves → must refuse (no x87 "
           "register class), never a guessed piece";
    // SysV: a long-double complex ARGUMENT is MEMORY — its class is COMPLEX_X87, which
    // is passed in memory (a RESULT of it is two x87 values: AggregateAbiSysV's
    // AComplexLongDoubleIsReturnedAsTwoX87ValuesAndPassedInMemory).
    auto sysv = classifySysV(cld, ti);
    ASSERT_TRUE(sysv.has_value());
    EXPECT_EQ(sysv->kind, AbiPassing::Kind::Memory);
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

// 24 bytes (> 16) → MEMORY (an argument by the convention's carrier; sret for returns).
TEST(AggregateAbiSysV, TwentyFourBytes_Memory) {
    auto ti = makeInterner(1);
    TypeId const l = ti.primitive(TypeKind::I64);
    auto r = classifySysV(structOf(ti, "LLL", {l, l, l}), ti);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::Memory);
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
    EXPECT_FALSE(classifyAggregate(AggregateConventionRules{AggregateClassKind::None, 16}, s, ti,
                                   kNatural16, DataModel::Lp64,
                                   AggregatePosition::Argument).has_value());
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
classifyWin64(TypeId t, TypeInterner const& ti,
              AggregatePosition position = AggregatePosition::Argument) {
    return classifyAggregate(AggregateConventionRules{AggregateClassKind::Win64BySize, 8}, t, ti,
                             kX86_64Layout, DataModel::Lp64, position);
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
    EXPECT_EQ(r->kind, AbiPassing::Kind::Memory)
        << "a 3-byte struct is not power-of-two → by reference on Win64";
}

TEST(AggregateAbiWin64, TwelveByteStruct_ByReference) {
    auto ti = makeInterner(1);
    TypeId const i = ti.primitive(TypeKind::I32);
    // {int,int,int} = 12 bytes — > 8 → BY REFERENCE (SysV would split into two
    // eightbytes; Win64 always passes > 8 by reference).
    auto r = classifyWin64(structOf(ti, "III", {i, i, i}), ti);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->kind, AbiPassing::Kind::Memory);
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
classifyAapcs64(TypeId t, TypeInterner const& ti,
                AggregatePosition position = AggregatePosition::Argument) {
    return classifyAggregate(AggregateConventionRules{AggregateClassKind::Aapcs64Hfa, 16}, t, ti,
                             kArm64Layout, DataModel::Lp64, position);
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

// ★ P69 (lane `cs`, fold f26c2 — review-csf26c F14): THE CLASSIFIER STATES AN HFA'S ELEMENT SIZE, because a
// convention may place a stacked HFA by its element's alignment instead of its own (Apple arm64:
// `homogeneousAggregateAlignmentOf: element`). ✔MEASURED (lane `cs`, probe f14): Apple clang 21 puts
// `struct { _Alignas(32) double a, b, c, d; }` on an 8-byte slot, aarch64 gcc 13.3.0 and clang 18.1.3 on a
// 16-aligned one. Every HFA states its element (a float's 4, a double's 8, a binary128's 16); a non-HFA, an
// aggregate passed by reference, and every SysV answer state 0.
//
// RED-ON-DISABLE: drop the `homogeneousElementBytes` line of the AAPCS64 arm and every HFA states 0.
TEST(AggregateAbiAapcs64, AnHfaStatesItsElementSizeAndNothingElseDoes) {
    auto ti = makeInterner(1);
    TypeId const f = ti.primitive(TypeKind::F32);
    TypeId const d = ti.primitive(TypeKind::F64);
    TypeId const q = ti.primitive(TypeKind::F128);
    TypeId const i = ti.primitive(TypeKind::I64);
    struct Cell {
        char const*   what;
        TypeId        type;
        std::uint32_t element;
    };
    for (Cell const& c : {Cell{"three floats", structOf(ti, "HF3", {f, f, f}), 4},
                          Cell{"four doubles", structOf(ti, "HD4", {d, d, d, d}), 8},
                          Cell{"two binary128", structOf(ti, "HQ2", {q, q}), 16},
                          Cell{"two longs (not an HFA)", structOf(ti, "NL2", {i, i}), 0},
                          Cell{"a double and a long (not an HFA)", structOf(ti, "NDL", {d, i}), 0},
                          Cell{"five doubles (by reference)", structOf(ti, "ND5", {d, d, d, d, d}), 0}}) {
        auto const r = classifyAapcs64(c.type, ti);
        ASSERT_TRUE(r.has_value()) << c.what;
        EXPECT_EQ(r->homogeneousElementBytes, c.element) << c.what;
    }
    auto const sysv = classifySysV(structOf(ti, "SD2", {d, d}), ti);
    ASSERT_TRUE(sysv.has_value());
    EXPECT_EQ(sysv->kind, AbiPassing::Kind::InRegisters) << "two SSE eightbytes";
    EXPECT_EQ(sysv->homogeneousElementBytes, 0u) << "SysV classifies eightbytes, and states no element";
}

// ★ P69 (lane `cs`, fold f26c2 — F14's neighbour): A COMPOSITE OF ONE FLOATING TYPE IS AN HFA ONLY WHERE ITS ELEMENTS
// TILE IT. ✔MEASURED (lane `cs`, probe hp, -O1 assembly of a callee and a caller of each shape): aarch64 gcc 13.3.0,
// clang 18.1.3 and Apple clang agree, shape for shape -- a member alignment that pads the composite makes it no HFA:
// in general registers at 16 bytes, by reference past them; the two controls, which tile their bytes, are HFAs.
//
// RED-ON-DISABLE: drop the tiling test of the AAPCS64 arm and the four padded shapes are HFAs of 2 or 4 elements.
TEST(AggregateAbiAapcs64, APaddedCompositeOfOneFloatingTypeIsNoHfa) {
    auto ti = makeInterner(1);
    TypeId const f = ti.primitive(TypeKind::F32);
    TypeId const d = ti.primitive(TypeKind::F64);
    auto const aligned = [&](std::string_view name, std::vector<TypeId> fields, std::vector<std::uint32_t> aligns) {
        return ti.structType(name, fields, {}, {}, aligns);
    };
    struct Cell {
        char const*      what;
        TypeId           type;
        AbiPassing::Kind kind;
        std::size_t      pieces;
        AbiPieceClass    cls;   // of every piece
    };
    for (Cell const& c : {
             Cell{"struct { _Alignas(16) double a; } (x0, x1)", aligned("P1", {d}, {16}),
                  AbiPassing::Kind::InRegisters, 2, AbiPieceClass::Gpr},
             Cell{"struct { float a; _Alignas(8) float b; } (x0, x1)", aligned("P2", {f, f}, {0, 8}),
                  AbiPassing::Kind::InRegisters, 2, AbiPieceClass::Gpr},
             Cell{"struct { _Alignas(16) float a; float b; } (x0, x1)", aligned("P3", {f, f}, {16, 0}),
                  AbiPassing::Kind::InRegisters, 2, AbiPieceClass::Gpr},
             Cell{"struct { double a; _Alignas(16) double b; } (by reference)", aligned("P4", {d, d}, {0, 16}),
                  AbiPassing::Kind::Memory, 0, AbiPieceClass::Gpr},
             Cell{"CONTROL: struct { double a, b; } (d0, d1)", structOf(ti, "P5", {d, d}),
                  AbiPassing::Kind::InRegisters, 2, AbiPieceClass::Fpr},
             Cell{"CONTROL: struct { _Alignas(32) double a, b, c, d; } (d0..d3)",
                  aligned("P6", {d, d, d, d}, {32, 0, 0, 0}), AbiPassing::Kind::InRegisters, 4, AbiPieceClass::Fpr},
         }) {
        auto const r = classifyAapcs64(c.type, ti);
        ASSERT_TRUE(r.has_value()) << c.what;
        EXPECT_EQ(r->kind, c.kind) << c.what;
        ASSERT_EQ(r->pieces.size(), c.pieces) << c.what;
        for (AbiPiece const& p : r->pieces) EXPECT_EQ(p.cls, c.cls) << c.what;
        EXPECT_EQ(r->homogeneousElementBytes, c.cls == AbiPieceClass::Fpr ? 8u : 0u) << c.what;
    }
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
    EXPECT_EQ(r->kind, AbiPassing::Kind::Memory);
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
        EXPECT_EQ(classifySysV(b200, ti)->kind, AbiPassing::Kind::Memory);
    }
    // Win64: a 16-byte value is NOT a power-of-two ≤ 8 → by reference; 200b likewise.
    {
        EXPECT_EQ(classifyWin64(b128, ti)->kind, AbiPassing::Kind::Memory);
        EXPECT_EQ(classifyWin64(b200, ti)->kind, AbiPassing::Kind::Memory);
    }
    // AAPCS64: 128b is a non-HFA ≤16B → two GPRs; 200b >16B → by reference.
    {
        auto r = classifyAapcs64(b128, ti);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->kind, AbiPassing::Kind::InRegisters);
        ASSERT_EQ(r->pieces.size(), 2u);
        EXPECT_EQ(r->pieces[0].cls, AbiPieceClass::Gpr);
        EXPECT_EQ(r->pieces[1].cls, AbiPieceClass::Gpr);
        EXPECT_EQ(classifyAapcs64(b200, ti)->kind, AbiPassing::Kind::Memory);
    }
}

// The position is SysV's question alone: Win64 and AAPCS64 answer a result exactly
// as an argument (their F80-free axes cannot form an X87 class).
TEST(AggregateAbiWin64AndAapcs64, APositionChangesNoClassOutsideSysV) {
    auto ti = makeInterner(1);
    TypeId const l = ti.primitive(TypeKind::I64);
    TypeId const d = ti.primitive(TypeKind::F64);
    std::array<TypeId, 3> const shapes{
        structOf(ti, "PL", {l}), structOf(ti, "PLLL", {l, l, l}), structOf(ti, "PDD", {d, d})};
    for (TypeId const t : shapes) {
        auto const wa = classifyWin64(t, ti, AggregatePosition::Argument);
        auto const wr = classifyWin64(t, ti, AggregatePosition::Result);
        ASSERT_TRUE(wa.has_value() && wr.has_value());
        EXPECT_EQ(wa->kind, wr->kind);
        EXPECT_EQ(wa->pieces.size(), wr->pieces.size());
        auto const aa = classifyAapcs64(t, ti, AggregatePosition::Argument);
        auto const ar = classifyAapcs64(t, ti, AggregatePosition::Result);
        ASSERT_TRUE(aa.has_value() && ar.has_value());
        EXPECT_EQ(aa->kind, ar->kind);
        EXPECT_EQ(aa->pieces.size(), ar->pieces.size());
    }
}
