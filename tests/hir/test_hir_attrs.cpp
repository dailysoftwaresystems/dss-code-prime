// HR5: the HIR attribute catalog — the standard per-node side-tables bind to a
// frozen Hir and round-trip their values. Each map is `HirAttribute<T>` over a
// value struct in src/hir/attributes/; this exercises the binding + the value
// shapes (using non-default field values so a dropped field would be caught).

#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/hir_node.hpp"

#include <gtest/gtest.h>

#include <array>

using dss::BufferId;
using dss::CallConv;
using dss::CompilationUnitId;
using dss::DiagnosticCode;
using dss::DiagnosticInfo;
using dss::FfiLinkage;
using dss::FfiMetadata;
using dss::FfiVisibility;
using dss::Hir;
using dss::HirBuilder;
using dss::HirDiagnosticMap;
using dss::HirFfiMap;
using dss::HirNodeId;
using dss::HirRecovery;
using dss::HirShaderMap;
using dss::HirSourceLoc;
using dss::HirSourceMap;
using dss::HirTranspileMap;
using dss::ShaderBuiltin;
using dss::ShaderIntrinsic;
using dss::ShaderStage;
using dss::SourceSpan;
using dss::TranspileHint;
using dss::TranspileIdiom;
using dss::TypeId;
using dss::TypeInterner;
using dss::TypeKind;

namespace {

TypeInterner makeInterner() { return TypeInterner{CompilationUnitId{1}}; }

// A one-literal module; returns its finished Hir and reports the literal node.
Hir oneLiteral(HirNodeId& node) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirBuilder b{"toy"};
    node = b.makeLiteral(i32, 42);
    return std::move(b).finish(node);
}

} // namespace

TEST(HirAttrs, SourceMapRoundTrips) {
    HirNodeId n;
    Hir h = oneLiteral(n);

    HirSourceMap spans{h};
    EXPECT_FALSE(spans.has(n));

    BufferId const buf{5};
    SourceSpan const sp = SourceSpan::of(100, 112);
    spans.set(n, HirSourceLoc{buf, sp});

    ASSERT_TRUE(spans.has(n));
    EXPECT_EQ(spans.get(n).buffer, buf);
    EXPECT_EQ(spans.get(n).span, sp);
}

TEST(HirAttrs, SourceLocDefaultsAreNoLocation) {
    // The zero-state of a HirSourceLoc must be the honest "no location" pair.
    HirSourceLoc const loc;
    EXPECT_EQ(loc.buffer, dss::InvalidBuffer);
    EXPECT_TRUE(loc.span.isEmpty());
}

TEST(HirAttrs, ShaderMapRoundTrips) {
    HirNodeId n;
    Hir h = oneLiteral(n);

    HirShaderMap shader{h};
    ShaderIntrinsic si;
    si.stage          = ShaderStage::Compute;
    si.builtin        = ShaderBuiltin::GlobalInvocationId;
    si.workgroup      = {8, 4, 1};
    si.binding        = {2, 3};
    si.location       = 7;
    shader.set(n, si);

    ASSERT_TRUE(shader.has(n));
    EXPECT_EQ(shader.get(n).stage, ShaderStage::Compute);
    EXPECT_EQ(shader.get(n).builtin, ShaderBuiltin::GlobalInvocationId);
    EXPECT_EQ(shader.get(n).workgroup.x, 8u);
    EXPECT_EQ(shader.get(n).workgroup.y, 4u);
    EXPECT_EQ(shader.get(n).binding.set, 2u);
    EXPECT_EQ(shader.get(n).binding.binding, 3u);
    EXPECT_EQ(shader.get(n).location, 7u);
}

TEST(HirAttrs, TranspileMapRoundTrips) {
    HirNodeId n;
    Hir h = oneLiteral(n);

    HirTranspileMap hints{h};
    TranspileHint th;
    th.targetLanguage = "javascript";
    th.overrideKind   = "JsDot";
    th.idiom          = TranspileIdiom::EarlyReturn;
    hints.set(n, th);

    ASSERT_TRUE(hints.has(n));
    EXPECT_EQ(hints.get(n).targetLanguage, "javascript");
    EXPECT_EQ(hints.get(n).overrideKind, "JsDot");
    EXPECT_EQ(hints.get(n).idiom, TranspileIdiom::EarlyReturn);
}

TEST(HirAttrs, DiagnosticMapRoundTrips) {
    HirNodeId n;
    Hir h = oneLiteral(n);

    HirDiagnosticMap diags{h};
    DiagnosticInfo di;
    di.code     = DiagnosticCode::H_TypeUnresolved;
    di.recovery = HirRecovery::Substituted;
    di.origin   = n;
    di.detail   = "operand type unresolvable; substituted Error";
    diags.set(n, di);

    ASSERT_TRUE(diags.has(n));
    EXPECT_EQ(diags.get(n).code, DiagnosticCode::H_TypeUnresolved);
    EXPECT_EQ(diags.get(n).recovery, HirRecovery::Substituted);
    EXPECT_EQ(diags.get(n).origin, n);
    EXPECT_EQ(diags.get(n).detail, "operand type unresolvable; substituted Error");
}

TEST(HirAttrs, FfiMapAliasBindsToExternNodes) {
    // The HirFfiMap catalog alias is the same side-table HR4 bound by hand; this
    // confirms the alias resolves and round-trips.
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    HirBuilder b{"c-subset"};
    HirNodeId const ef = b.makeExternFunction(fnTy, /*symbol=*/6, {});
    Hir h = std::move(b).finish(ef);

    HirFfiMap ffi{h};
    FfiMetadata m;
    m.mangledName = "printf";
    m.linkage     = FfiLinkage::Weak;
    m.visibility  = FfiVisibility::Hidden;
    ffi.set(ef, m);

    ASSERT_TRUE(ffi.has(ef));
    EXPECT_EQ(ffi.get(ef).mangledName, "printf");
    EXPECT_EQ(ffi.get(ef).linkage, FfiLinkage::Weak);
    EXPECT_EQ(ffi.get(ef).visibility, FfiVisibility::Hidden);
}

TEST(HirAttrs, IndependentMapsDoNotAlias) {
    // Two different side-tables over the same module are independent: a node set
    // in one is not "present" in another.
    HirNodeId n;
    Hir h = oneLiteral(n);

    HirSourceMap   spans{h};
    HirTranspileMap hints{h};
    spans.set(n, HirSourceLoc{BufferId{1}, SourceSpan::of(0, 1)});

    EXPECT_TRUE(spans.has(n));
    EXPECT_FALSE(hints.has(n));
}
