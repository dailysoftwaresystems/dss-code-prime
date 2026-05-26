// HR2: the open-core operator vocabulary (HirOpKind + payload codec) and the
// extension-operator registry (HirOpRegistry) — the operator analog of
// HirKind / HirKindRegistry.

#include "hir/hir_op.hpp"
#include "hir/hir_op_registry.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using dss::arityOf;
using dss::decodeCoreOp;
using dss::decodeExtOp;
using dss::encodeOp;
using dss::HirOpArity;
using dss::HirOpId;
using dss::HirOpKind;
using dss::HirOpRegistry;
using dss::isCoreOp;
using dss::kFirstHirExtensionOp;
using dss::opName;

// ── compile-time invariants ──
static_assert(static_cast<std::uint32_t>(HirOpKind::Count_) < 256,
              "core operators must fit [0, 256)");
static_assert(kFirstHirExtensionOp == 256);

TEST(HirOpKind, CoreArityClassification) {
    // A representative of each binary group + each unary op.
    EXPECT_EQ(arityOf(HirOpKind::Add),    HirOpArity::Binary);
    EXPECT_EQ(arityOf(HirOpKind::Shr),    HirOpArity::Binary);
    EXPECT_EQ(arityOf(HirOpKind::Eq),     HirOpArity::Binary);
    EXPECT_EQ(arityOf(HirOpKind::Ge),     HirOpArity::Binary);
    EXPECT_EQ(arityOf(HirOpKind::Neg),    HirOpArity::Unary);
    EXPECT_EQ(arityOf(HirOpKind::Not),    HirOpArity::Unary);
    EXPECT_EQ(arityOf(HirOpKind::BitNot), HirOpArity::Unary);
}

TEST(HirOpKind, NamesAreStable) {
    EXPECT_EQ(opName(HirOpKind::Add),    "Add");
    EXPECT_EQ(opName(HirOpKind::BitXor), "BitXor");
    EXPECT_EQ(opName(HirOpKind::Ne),     "Ne");
    EXPECT_EQ(opName(HirOpKind::BitNot), "BitNot");
}

TEST(HirOpKind, EveryCoreOpHasNameAndArity) {
    // Exhaustively pin that no core operator falls through opName's "?" default
    // or is left unclassified by arityOf — a new core op added without updating
    // either switch would surface here (the build does not enforce -Wswitch).
    auto const count = static_cast<std::uint32_t>(HirOpKind::Count_);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto const op = static_cast<HirOpKind>(i);
        EXPECT_NE(opName(op), "?") << "core op ordinal " << i << " has no name";
        HirOpArity const a = arityOf(op);
        EXPECT_TRUE(a == HirOpArity::Binary || a == HirOpArity::Unary);
    }
}

TEST(HirOpCodec, CoreOpRoundTripsBelow256) {
    std::uint32_t const p = encodeOp(HirOpKind::Add);
    EXPECT_EQ(p, 0u);                       // Add is the first core ordinal
    EXPECT_TRUE(isCoreOp(p));
    EXPECT_EQ(decodeCoreOp(p), HirOpKind::Add);

    std::uint32_t const q = encodeOp(HirOpKind::BitNot);
    EXPECT_TRUE(isCoreOp(q));
    EXPECT_EQ(decodeCoreOp(q), HirOpKind::BitNot);
}

TEST(HirOpCodec, ExtensionOpRoundTripsAtOrAbove256) {
    HirOpId const op{kFirstHirExtensionOp};
    std::uint32_t const p = encodeOp(op);
    EXPECT_EQ(p, 256u);
    EXPECT_FALSE(isCoreOp(p));
    EXPECT_EQ(decodeExtOp(p), op);
}

TEST(HirOpCodec, CoreExtensionSplitIsExactlyAt256) {
    // The off-by-one most likely to regress if kFirstHirExtensionOp or the
    // comparison is ever edited: 255 is the last core payload, 256 the first
    // extension payload.
    EXPECT_TRUE(isCoreOp(255u));
    EXPECT_FALSE(isCoreOp(256u));
}

// ── HirOpRegistry ──

TEST(HirOpRegistry, MintsMonotonicIdsFrom256) {
    HirOpRegistry reg;
    HirOpId const a = reg.registerExtension("L::Spaceship", HirOpArity::Binary, "L");
    HirOpId const b = reg.registerExtension("L::Rotate",    HirOpArity::Unary,  "L");
    EXPECT_EQ(a.v, 256u);
    EXPECT_EQ(b.v, 257u);
    EXPECT_EQ(reg.extensions().size(), 2u);
}

TEST(HirOpRegistry, ReDeclarationIsIdempotentOnSameLangAndArity) {
    HirOpRegistry reg;
    HirOpId const first  = reg.registerExtension("L::Spaceship", HirOpArity::Binary, "L");
    HirOpId const second = reg.registerExtension("L::Spaceship", HirOpArity::Binary, "L");
    EXPECT_EQ(first, second);
    EXPECT_EQ(reg.extensions().size(), 1u);
}

TEST(HirOpRegistry, FindAndDescriptor) {
    HirOpRegistry reg;
    HirOpId const op = reg.registerExtension("L::Spaceship", HirOpArity::Binary, "L");
    auto found = reg.findExtension("L::Spaceship");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, op);
    EXPECT_FALSE(reg.findExtension("L::Nope").has_value());

    auto const& d = reg.descriptor(op);
    EXPECT_EQ(d.name(), "L::Spaceship");
    EXPECT_EQ(d.opId(), op);
    EXPECT_EQ(d.arity(), HirOpArity::Binary);
    EXPECT_EQ(d.sourceLanguage(), "L");
}

// Genericity proof: a wholly synthetic language registers a wholly synthetic
// operator with no special-casing — the core engine never names it. Mirrors the
// Synth::Widget HirKind genericity proof.
TEST(HirOpRegistry, SynthExtensionOperatorComposes) {
    HirOpRegistry reg;
    HirOpId const rot = reg.registerExtension("Synth::Rotate", HirOpArity::Unary, "Synth");
    EXPECT_FALSE(isCoreOp(encodeOp(rot)));
    EXPECT_EQ(reg.descriptor(rot).arity(), HirOpArity::Unary);
    EXPECT_EQ(reg.descriptor(rot).sourceLanguage(), "Synth");
}

TEST(HirOpRegistryDeathTest, CrossDomainNameCollisionAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    HirOpRegistry reg;
    reg.registerExtension("Op", HirOpArity::Binary, "LangA");
    EXPECT_DEATH({ reg.registerExtension("Op", HirOpArity::Binary, "LangB"); },
                 "re-registered under language 'LangB' but was first registered under 'LangA'");
}

TEST(HirOpRegistryDeathTest, ArityClashOnReDeclarationAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    HirOpRegistry reg;
    reg.registerExtension("Op", HirOpArity::Binary, "L");
    EXPECT_DEATH({ reg.registerExtension("Op", HirOpArity::Unary, "L"); },
                 "re-registered as unary but was first registered as binary");
}

TEST(HirOpRegistryDeathTest, DescriptorForCoreRangeIdAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    HirOpRegistry reg;
    EXPECT_DEATH({ (void)reg.descriptor(HirOpId{5}); },
                 "descriptor\\(\\) for HirOpId=5 this registry never minted");
}

TEST(HirOpRegistryDeathTest, DescriptorForUnmintedExtensionRangeIdAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    HirOpRegistry reg;
    reg.registerExtension("L::Op", HirOpArity::Binary, "L");  // mints only id 256
    // 300 is in the extension range (>= 256) but past what this registry minted
    // — the other half of descriptor()'s guard, distinct from the core-range path.
    EXPECT_DEATH({ (void)reg.descriptor(HirOpId{300}); },
                 "descriptor\\(\\) for HirOpId=300 this registry never minted");
}
