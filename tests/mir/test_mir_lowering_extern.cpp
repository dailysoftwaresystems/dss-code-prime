// LK6 cycle 2d (D-LK6-6 closure): HIR ExternFunction nodes thread
// through to `HirToMirResult.externImports` via the FFI side-table.
// Replaces the hand-constructed `AssembledModule.externImports` of
// cycles 2a-c with a real lowering-driven path.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/hir_node.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>

using namespace dss;

namespace {

TypeInterner makeInterner() {
    return TypeInterner{CompilationUnitId{1}};
}

// Build a tiny module containing one ExternFunction.
struct Built {
    Hir hir;
    HirNodeId externNode;
    SymbolId  externSym;
};

[[nodiscard]] Built buildModuleWithExtern(TypeInterner& ti) {
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    HirBuilder b{"c-subset"};
    constexpr std::uint32_t kExternSymV = 17;
    HirNodeId const ef =
        b.makeExternFunction(fnTy, /*symbol=*/kExternSymV, {});
    HirNodeId const root = b.makeModule(std::array{ef});
    Built out{std::move(b).finish(root), ef, SymbolId{kExternSymV}};
    return out;
}

} // namespace

TEST(MirLoweringExtern, ExternFunctionWithFfiMetadataPopulatesExternImports) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};
    FfiMetadata meta;
    meta.mangledName   = "printf";
    meta.importLibrary = "libc.so.6";
    ffi.set(built.externNode, meta);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(built.hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, MirLoweringConfig{},
                             &ffi);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.externImports.size(), 1u);
    EXPECT_EQ(result.externImports[0].symbol, built.externSym);
    EXPECT_EQ(result.externImports[0].mangledName, "printf");
    EXPECT_EQ(result.externImports[0].libraryPath, "libc.so.6");
}

TEST(MirLoweringExtern, MissingFfiMetadataFailsLoud) {
    // Without an FFI side-table entry the lowerer must fail loud:
    // every extern needs a non-empty mangledName the linker can
    // resolve. Anchors at the HIR node so the diagnostic carries
    // source-span context (rather than failing at the linker
    // where the span has been lost).
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(built.hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, MirLoweringConfig{},
                             /*ffiMap=*/nullptr);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.externImports.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::H_UnsupportedLoweringForKind)
            sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(MirLoweringExtern, EmptyLibraryPathFailsLoud) {
    // Symmetric reject for an FfiMetadata entry whose
    // `importLibrary` is empty: the linker cannot emit a
    // DT_NEEDED / LC_LOAD_DYLIB / IMAGE_IMPORT_DESCRIPTOR row
    // without it. Surfacing the failure at the HIR node keeps
    // the diagnostic close to the source.
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};
    FfiMetadata meta;
    meta.mangledName = "printf";
    // importLibrary left empty.
    ffi.set(built.externNode, meta);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(built.hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, MirLoweringConfig{},
                             &ffi);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.externImports.empty());
}

TEST(MirLoweringExtern, EmptyMangledNameFailsLoud) {
    // Same symmetry on `mangledName`: an FfiMetadata entry whose
    // mangledName is the empty string is structurally identical
    // to "the map didn't carry this node" — both must fail loud.
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};
    FfiMetadata meta;
    // mangledName left empty.
    meta.importLibrary = "libc.so.6";
    ffi.set(built.externNode, meta);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(built.hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, MirLoweringConfig{},
                             &ffi);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.externImports.empty());
}

TEST(MirLoweringExtern, ModuleWithoutExternsProducesEmptyExternImports) {
    // Backward-compatibility: a module with no ExternFunction
    // nodes produces an empty externImports vector — every
    // existing cycle-2a/2b/2c test path is unchanged.
    TypeInterner ti = makeInterner();
    HirBuilder b{"c-subset"};
    HirNodeId const root = b.makeModule({});
    Hir hir = std::move(b).finish(root);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, MirLoweringConfig{},
                             /*ffiMap=*/nullptr);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.externImports.empty());
}

TEST(MirLoweringExtern, MissingSymbolIdFailsLoud) {
    // Fourth fail-loud arm: an ExternFunction whose semantic
    // model failed to bind a SymbolId (sym.v == 0) is rejected
    // at the MIR pre-pass with H_UnsupportedLoweringForKind.
    // (pr-test-analyzer Gap 4 fold, LK6 cycle 2d post-fold review.)
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    HirBuilder b{"c-subset"};
    HirNodeId const ef = b.makeExternFunction(fnTy, /*symbol=*/0, {});
    HirNodeId const root = b.makeModule(std::array{ef});
    Hir hir = std::move(b).finish(root);

    HirFfiMap ffi{hir};
    FfiMetadata meta;
    meta.mangledName   = "printf";
    meta.importLibrary = "libc.so.6";
    ffi.set(ef, meta);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, MirLoweringConfig{},
                             &ffi);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.externImports.empty());
}

TEST(MirLoweringExtern, ExternSymbolCollidesWithFunctionSymbolFailsLoud) {
    // silent-failure HIGH fold: an ExternFunction sharing a
    // SymbolId with an intra-module Function would route every
    // call to either through the same `functionSymbols` set,
    // making the cross-table relationship ambiguous. The linker
    // also rejects this, but anchoring the diagnostic at the HIR
    // node preserves source-span context.
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    HirBuilder b{"c-subset"};
    constexpr std::uint32_t kSharedSym = 42;
    // Build a tiny function body so the Function node is well-
    // formed.
    HirNodeId const param = b.makeVarDecl(i32, /*sym=*/1);
    HirNodeId const body  = b.makeBlock({});
    HirNodeId const fn = b.makeFunction(fnTy, kSharedSym,
                                        std::array{param}, body);
    HirNodeId const ef = b.makeExternFunction(fnTy, kSharedSym, {});
    HirNodeId const root = b.makeModule(std::array{fn, ef});
    Hir hir = std::move(b).finish(root);

    HirFfiMap ffi{hir};
    FfiMetadata meta;
    meta.mangledName   = "collision";
    meta.importLibrary = "libc.so.6";
    ffi.set(ef, meta);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, MirLoweringConfig{},
                             &ffi);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.externImports.empty());
}

TEST(MirLoweringExtern, DuplicateExternSymbolFailsLoud) {
    // silent-failure HIGH fold: two ExternFunction decls sharing
    // a SymbolId would push two rows into externImports — the
    // linker would later catch the cross-extern duplicate, but
    // again, the HIR-tier diagnostic preserves source-span context.
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    HirBuilder b{"c-subset"};
    constexpr std::uint32_t kSym = 17;
    HirNodeId const ef1 = b.makeExternFunction(fnTy, kSym, {});
    HirNodeId const ef2 = b.makeExternFunction(fnTy, kSym, {});
    HirNodeId const root = b.makeModule(std::array{ef1, ef2});
    Hir hir = std::move(b).finish(root);

    HirFfiMap ffi{hir};
    FfiMetadata meta;
    meta.mangledName   = "dup";
    meta.importLibrary = "libc.so.6";
    ffi.set(ef1, meta);
    ffi.set(ef2, meta);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, MirLoweringConfig{},
                             &ffi);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.externImports.size(), 1u);   // first row landed; second rejected
}

TEST(MirLoweringExtern, InvalidExternSignatureFailsLoud) {
    // silent-failure MEDIUM fold: an extern whose FnSig is
    // `InvalidType` has no ABI shape the assembler / linker can
    // resolve. Symmetric with `collectFunctions`'s sig.valid()
    // guard.
    TypeInterner ti = makeInterner();
    HirBuilder b{"c-subset"};
    HirNodeId const ef = b.makeExternFunction(InvalidType,
                                              /*symbol=*/17, {});
    HirNodeId const root = b.makeModule(std::array{ef});
    Hir hir = std::move(b).finish(root);

    HirFfiMap ffi{hir};
    FfiMetadata meta;
    meta.mangledName   = "printf";
    meta.importLibrary = "libc.so.6";
    ffi.set(ef, meta);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, MirLoweringConfig{},
                             &ffi);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.externImports.empty());
}

TEST(MirLoweringExtern, MultipleExternsAcrossTwoLibrariesPropagateInOrder) {
    // Two externs from two different libraries — the cycle-2d
    // pre-pass walks declaration order and produces parallel
    // ExternImport rows. Order matters: the linker groups by
    // libraryPath to produce one DT_NEEDED / LC_LOAD_DYLIB /
    // IMAGE_IMPORT_DESCRIPTOR per unique library, and a stable
    // declaration order makes the byte output deterministic.
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    HirBuilder b{"c-subset"};
    constexpr std::uint32_t kSym1 = 17;
    constexpr std::uint32_t kSym2 = 18;
    HirNodeId const ef1 = b.makeExternFunction(fnTy, kSym1, {});
    HirNodeId const ef2 = b.makeExternFunction(fnTy, kSym2, {});
    HirNodeId const root = b.makeModule(std::array{ef1, ef2});
    Hir hir = std::move(b).finish(root);

    HirFfiMap ffi{hir};
    FfiMetadata m1;
    m1.mangledName = "printf"; m1.importLibrary = "libc.so.6";
    ffi.set(ef1, m1);
    FfiMetadata m2;
    m2.mangledName = "_objc_msgSend";
    m2.importLibrary = "/usr/lib/libobjc.A.dylib";
    ffi.set(ef2, m2);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, MirLoweringConfig{},
                             &ffi);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.externImports.size(), 2u);
    EXPECT_EQ(result.externImports[0].mangledName, "printf");
    EXPECT_EQ(result.externImports[0].libraryPath, "libc.so.6");
    EXPECT_EQ(result.externImports[1].mangledName, "_objc_msgSend");
    EXPECT_EQ(result.externImports[1].libraryPath,
              "/usr/lib/libobjc.A.dylib");
}

TEST(MirLoweringExtern, ExternGlobalCurrentlyFailsLoudPendingFeatureWork) {
    // D-FF2-5 audit pin (2026-06-01): `extern int x;` (and the
    // array form `extern int x[10];` post-fold #11) lowers to a
    // HIR `ExternGlobal` node correctly — but the MIR builder at
    // src/mir/lowering/hir_to_mir.cpp (HirKind::ExternGlobal arm of
    // the decl switch) currently rejects the kind with `unsupported()`
    // because the FFI side of ExternGlobal (data-symbol ingestion +
    // linker symbol-table emission) is not yet implemented end-to-end.
    //
    // PRE-FOLD #11: `extern int x[10];` silently lost its array
    // type (externDecl had no arraySuffix configured); lowered as
    // `int`. Post-fold #11 the array type survives semantic
    // analysis but the MIR builder still rejects ExternGlobal
    // wholesale.
    //
    // This test pins the CURRENT loud-rejection behavior. A future
    // fold landing real ExternGlobal MIR support (extending
    // `collectExterns` + `ExternImport` with TypeId, etc. — see
    // anchor D-FF2-5-FEATURE) will replace this test with the
    // positive pin. Until then, a regression that silently accepted
    // ExternGlobal at the MIR-builder layer (distinct from D-FF2-3's
    // parser-level extern-with-init surface) would slip past the
    // audit; this test catches that exact silent-accept surface.
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirBuilder b{"c-subset"};
    constexpr std::uint32_t kExternGlobalSymV = 31;
    HirNodeId const eg =
        b.makeExternGlobal(i32, /*symbol=*/kExternGlobalSymV);
    HirNodeId const root = b.makeModule(std::array{eg});
    Hir hir = std::move(b).finish(root);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep,
                             /*sourceMap=*/nullptr,
                             MirLoweringConfig{},
                             /*ffiMap=*/nullptr);
    EXPECT_FALSE(result.ok)
        << "ExternGlobal currently fails loud at MIR lowering — "
           "silent-accept would slip a feature gap past the audit";
    EXPECT_TRUE(result.externImports.empty());
    bool sawUnsupported = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::H_UnsupportedLoweringForKind) {
            sawUnsupported = true;
            break;
        }
    }
    EXPECT_TRUE(sawUnsupported)
        << "MIR builder must emit H_UnsupportedLoweringForKind for "
           "ExternGlobal until full lowering support lands";
}

TEST(MirLoweringExtern, LowerToLirPropagatesExternsToMirToLirResult) {
    // pr-test-analyzer Gap 2 fold: a non-empty externs vector
    // passed to `lowerToLir` propagates verbatim into
    // `MirToLirResult.externImports`. Pins the std::move
    // assignment so a future refactor that drops the wire is a
    // test failure (not a silent regression).
    TypeInterner ti = makeInterner();
    Mir emptyMir{};  // value-default Mir is OK for this propagation test
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    std::vector<ExternImport> externs;
    externs.push_back(
        ExternImport{SymbolId{99}, "printf", "libc.so.6"});
    DiagnosticReporter rep;
    auto result = lowerToLir(emptyMir, **target, ti, rep,
                             std::move(externs));
    ASSERT_EQ(result.externImports.size(), 1u);
    EXPECT_EQ(result.externImports[0].symbol, SymbolId{99});
    EXPECT_EQ(result.externImports[0].mangledName, "printf");
    EXPECT_EQ(result.externImports[0].libraryPath, "libc.so.6");
}
