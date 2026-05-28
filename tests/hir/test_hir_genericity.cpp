// HR1 genericity proof: a registered extension HIR-kind — vocabulary the core
// enum has NEVER heard of — composes into the arena, the walker, and an
// attribute side-table identically to a core kind, with zero special-casing.
// This is the open-core+extensions discipline (mirrors the Synth* schema proofs
// for the grammar/semantic layers): a new language/domain onboards by
// REGISTERING kinds, never by editing the core.

#include "hir/hir.hpp"
#include "hir/hir_cursor.hpp"
#include "hir/hir_kind_registry.hpp"
#include "hir/hir_node.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>

using dss::Hir;
using dss::HirBuilder;
using dss::HirCursor;
using dss::HirKind;
using dss::HirKindId;
using dss::HirNodeId;
using dss::kFirstHirExtensionKind;

TEST(HirGenericity, ExtensionKindComposesLikeACoreKind) {
    HirBuilder b{"Synth"};
    // A synthetic, NON-shipped extension kind — proves no coupling to any real
    // language's vocabulary.
    HirKindId const widget = b.registry().registerExtension("Synth::Widget", "Synth");
    // A fresh registry mints the very first extension at exactly 256.
    ASSERT_EQ(widget.v, kFirstHirExtensionKind);

    // An Extension-marked node carries the concrete kindId in its payload. It
    // nests core children freely — no special-casing.
    HirNodeId const inner = b.addLeaf(HirKind::Literal);
    HirNodeId const ext   = b.addParent(HirKind::Extension, std::array{inner},
                                        dss::InvalidType, /*payload=*/widget.v);
    HirNodeId const mod   = b.addParent(HirKind::Module, std::array{ext});
    Hir hir = std::move(b).finish(mod);

    // The frozen module carries the registry; the payload round-trips back to a
    // descriptor with the exact qualified name and owning domain.
    EXPECT_EQ(hir.kind(ext), HirKind::Extension);
    EXPECT_EQ(hir.payload(ext), widget.v);
    auto const& d = hir.registry().descriptor(HirKindId{hir.payload(ext)});
    EXPECT_EQ(d.name(), "Synth::Widget");
    EXPECT_EQ(d.sourceLanguage(), "Synth");

    // The walker reaches the extension node exactly like any core node.
    HirCursor c{hir, hir.root()};
    ASSERT_TRUE(c.gotoFirstChild());
    EXPECT_EQ(c.kind(), HirKind::Extension);
    EXPECT_EQ(c.current(), ext);
    ASSERT_TRUE(c.gotoFirstChild());
    EXPECT_EQ(c.kind(), HirKind::Literal);

    // The open-core composes with the side-table substrate too: HirAttribute
    // binds to a module containing an Extension node and the attribute key
    // round-trips through the same generic machinery as any core-kind node.
    dss::HirAttribute<HirKindId> kindBySite{hir};
    kindBySite.set(ext, widget);
    ASSERT_TRUE(kindBySite.has(ext));
    EXPECT_EQ(kindBySite.get(ext).v, widget.v);
}
