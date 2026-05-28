// HR4: typed declaration builders + read accessors (Module / Function / Global /
// TypeDecl / ExternFunction / ExternGlobal / ImportGroup) and the
// HirAttribute<FfiMetadata> side-table on extern nodes.

#include "core/types/type_lattice/type_interner.hpp"
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/hir.hpp"
#include "hir/hir_node.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <vector>

using dss::CallConv;
using dss::FfiLinkage;
using dss::FfiMetadata;
using dss::FfiVisibility;
using dss::Hir;
using dss::HirAttribute;
using dss::HirBuilder;
using dss::HirKind;
using dss::HirNodeId;
using dss::SymbolId;
using dss::TypeId;
using dss::TypeInterner;
using dss::TypeKind;

namespace {

TypeInterner makeInterner() { return TypeInterner{dss::CompilationUnitId{1}}; }

} // namespace

TEST(HirDecl, FunctionCarriesSigParamsAndBody) {
    HirBuilder b{"c-subset"};
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);  // int f(int)

    HirNodeId const p0   = b.makeVarDecl(i32, /*symbol=*/10);   // parameter
    HirNodeId const body = b.makeBlock({});
    HirNodeId const fn   = b.makeFunction(fnTy, /*symbol=*/5, std::array{p0}, body);
    HirNodeId const mod  = b.makeModule(std::array{fn});
    Hir h = std::move(b).finish(mod);

    EXPECT_EQ(h.kind(fn), HirKind::Function);
    EXPECT_EQ(h.functionSignature(fn), fnTy);
    EXPECT_EQ(h.functionSymbol(fn), SymbolId{5});
    ASSERT_EQ(h.functionParams(fn).size(), 1u);
    EXPECT_EQ(h.functionParams(fn)[0], p0);
    EXPECT_EQ(h.functionBody(fn), body);
    // Module exposes its declarations.
    ASSERT_EQ(h.moduleDecls(mod).size(), 1u);
    EXPECT_EQ(h.moduleDecls(mod)[0], fn);
}

TEST(HirDecl, MultiParamFunctionParamsAreOrderedAndExcludeBody) {
    HirBuilder b{"c-subset"};
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32, i32, i32}, i32, CallConv::CcSysV);
    HirNodeId const p0 = b.makeVarDecl(i32, 10);
    HirNodeId const p1 = b.makeVarDecl(i32, 11);
    HirNodeId const p2 = b.makeVarDecl(i32, 12);
    HirNodeId const body = b.makeBlock({});
    HirNodeId const fn = b.makeFunction(fnTy, 5, std::array{p0, p1, p2}, body);
    Hir h = std::move(b).finish(fn);

    auto params = h.functionParams(fn);
    ASSERT_EQ(params.size(), 3u);            // the body is excluded from params
    EXPECT_EQ(params[0], p0);
    EXPECT_EQ(params[1], p1);                // order preserved
    EXPECT_EQ(params[2], p2);
    EXPECT_EQ(h.functionBody(fn), body);
    EXPECT_EQ(h.children(fn).size(), 4u);    // 3 params + body
}

TEST(HirDecl, EmptyModuleHasNoDecls) {
    HirBuilder b{"toy"};
    HirNodeId const mod = b.makeModule({});
    Hir h = std::move(b).finish(mod);
    EXPECT_EQ(h.kind(mod), HirKind::Module);
    EXPECT_TRUE(h.moduleDecls(mod).empty());
}

TEST(HirDecl, ZeroParamFunctionIsBodyOnly) {
    HirBuilder b{"c-subset"};
    TypeInterner ti = makeInterner();
    TypeId const voidT = ti.primitive(TypeKind::Void);
    TypeId const fnTy  = ti.fnSig({}, voidT, CallConv::CcSysV);   // void f(void)
    HirNodeId const body = b.makeBlock({});
    HirNodeId const fn   = b.makeFunction(fnTy, /*symbol=*/1, {}, body);
    Hir h = std::move(b).finish(fn);

    EXPECT_TRUE(h.functionParams(fn).empty());
    EXPECT_EQ(h.functionBody(fn), body);
    EXPECT_EQ(h.children(fn).size(), 1u);   // body only
}

TEST(HirDecl, GlobalWithAndWithoutInit) {
    HirBuilder b{"c-subset"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirNodeId const init = b.makeLiteral(i32);
    HirNodeId const g0 = b.makeGlobal(i32, /*symbol=*/2, init);
    HirNodeId const g1 = b.makeGlobal(i32, /*symbol=*/3);          // no initializer
    HirNodeId const mod = b.makeModule(std::array{g0, g1});
    Hir h = std::move(b).finish(mod);

    EXPECT_EQ(h.globalType(g0), i32);
    EXPECT_EQ(h.globalSymbol(g0), SymbolId{2});
    ASSERT_TRUE(h.globalInit(g0).has_value());
    EXPECT_EQ(*h.globalInit(g0), init);
    EXPECT_FALSE(h.globalInit(g1).has_value());
}

TEST(HirDecl, TypeDeclIsLeafCarryingType) {
    HirBuilder b{"c-subset"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirNodeId const td = b.makeTypeDecl(i32, /*symbol=*/4);   // typedef int MyInt;
    Hir h = std::move(b).finish(td);

    EXPECT_EQ(h.kind(td), HirKind::TypeDecl);
    EXPECT_EQ(h.typeDeclType(td), i32);
    EXPECT_EQ(h.typeDeclSymbol(td), SymbolId{4});
    EXPECT_TRUE(h.children(td).empty());
}

TEST(HirDecl, ExternFunctionHasParamsNoBody) {
    HirBuilder b{"c-subset"};
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    HirNodeId const p0 = b.makeVarDecl(i32, /*symbol=*/11);
    HirNodeId const ef = b.makeExternFunction(fnTy, /*symbol=*/6, std::array{p0});
    Hir h = std::move(b).finish(ef);

    EXPECT_EQ(h.kind(ef), HirKind::ExternFunction);
    EXPECT_EQ(h.externFunctionSignature(ef), fnTy);
    EXPECT_EQ(h.externFunctionSymbol(ef), SymbolId{6});
    ASSERT_EQ(h.externFunctionParams(ef).size(), 1u);
    EXPECT_EQ(h.externFunctionParams(ef)[0], p0);
}

TEST(HirDecl, ExternMayBeUntyped) {
    // 11-ffi-plan binary-only ingestion can yield an extern with no resolved type.
    HirBuilder b{"c-subset"};
    HirNodeId const ef = b.makeExternFunction(dss::InvalidType, /*symbol=*/7, {});
    HirNodeId const eg = b.makeExternGlobal(dss::InvalidType, /*symbol=*/8);
    HirNodeId const mod = b.makeModule(std::array{ef, eg});
    Hir h = std::move(b).finish(mod);

    EXPECT_FALSE(h.externFunctionSignature(ef).valid());
    EXPECT_FALSE(h.externGlobalType(eg).valid());
    EXPECT_EQ(h.externGlobalSymbol(eg), SymbolId{8});
}

TEST(HirDecl, ImportGroupGroupsMembers) {
    HirBuilder b{"c-subset"};
    TypeInterner ti = makeInterner();
    TypeId const fnTy = ti.fnSig({}, ti.primitive(TypeKind::Void), CallConv::CcSysV);
    HirNodeId const ef = b.makeExternFunction(fnTy, /*symbol=*/9, {});
    HirNodeId const ig = b.makeImportGroup(std::array{ef});
    Hir h = std::move(b).finish(ig);

    EXPECT_EQ(h.kind(ig), HirKind::ImportGroup);
    ASSERT_EQ(h.importGroupMembers(ig).size(), 1u);
    EXPECT_EQ(h.importGroupMembers(ig)[0], ef);
    // An empty import group is also legal.
    EXPECT_NO_FATAL_FAILURE((void)h.importGroupMembers(ig));
}

TEST(HirDecl, FfiMetadataBindsToExternNodes) {
    HirBuilder b{"c-subset"};
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    HirNodeId const ef = b.makeExternFunction(fnTy, /*symbol=*/6, {});
    Hir h = std::move(b).finish(ef);

    HirAttribute<FfiMetadata> ffi{h};
    EXPECT_FALSE(ffi.has(ef));
    FfiMetadata m;
    m.mangledName   = "printf";
    // Use NON-default enum values so the round-trip actually proves the fields
    // are stored (Strong/Default are the struct defaults — asserting them would
    // pass even against a setter that dropped the value).
    m.linkage       = FfiLinkage::Weak;
    m.visibility    = FfiVisibility::Hidden;
    m.importLibrary = "libc.so.6";
    m.soname        = "libc.so.6";
    ffi.set(ef, m);

    ASSERT_TRUE(ffi.has(ef));
    EXPECT_EQ(ffi.get(ef).mangledName, "printf");
    EXPECT_EQ(ffi.get(ef).importLibrary, "libc.so.6");
    EXPECT_EQ(ffi.get(ef).soname, "libc.so.6");
    EXPECT_EQ(ffi.get(ef).linkage, FfiLinkage::Weak);
    EXPECT_EQ(ffi.get(ef).visibility, FfiVisibility::Hidden);
}

TEST(HirDeclDeathTest, FunctionBodyOnBodylessFunctionAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // A 0-child Function is malformed (arity {1,∞}); functionBody must abort loud
    // rather than read past the (empty) child span.
    HirBuilder b{"c-subset"};
    HirNodeId const fn = b.addParent(HirKind::Function, {}, dss::InvalidType, /*payload=*/1);
    Hir h = std::move(b).finish(fn);
    EXPECT_DEATH({ (void)h.functionBody(fn); }, "has no body child");
}

// Cross-check: declaration helpers honor the childArity single-source-of-truth.
TEST(HirDecl, HelpersSatisfyChildAritySpec) {
    HirBuilder b{"c-subset"};
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig({}, i32, CallConv::CcSysV);

    std::vector<HirNodeId> nodes;
    nodes.push_back(b.makeFunction(fnTy, 1, {}, b.makeBlock({})));
    nodes.push_back(b.makeGlobal(i32, 2));
    nodes.push_back(b.makeTypeDecl(i32, 3));
    nodes.push_back(b.makeExternFunction(fnTy, 4, {}));
    nodes.push_back(b.makeExternGlobal(i32, 5));
    nodes.push_back(b.makeImportGroup({}));
    HirNodeId const mod = b.makeModule(nodes);
    nodes.push_back(mod);
    Hir h = std::move(b).finish(mod);

    for (HirNodeId n : nodes) {
        auto const a = dss::childArity(h.kind(n));
        auto const c = static_cast<std::uint32_t>(h.children(n).size());
        EXPECT_GE(c, a.min) << "kind ordinal " << static_cast<unsigned>(h.kind(n));
        if (a.max != dss::kUnboundedArity) {
            EXPECT_LE(c, a.max) << "kind ordinal " << static_cast<unsigned>(h.kind(n));
        }
    }
}
