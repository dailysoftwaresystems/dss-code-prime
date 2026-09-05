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
#include <unordered_map>

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
    HirBuilder b{"c"};
    constexpr std::uint32_t kExternSymV = 17;
    HirNodeId const ef =
        b.makeExternFunction(fnTy, /*symbol=*/kExternSymV, {});
    HirNodeId const root = b.makeModule(std::array{ef});
    Built out{std::move(b).finish(root), ef, SymbolId{kExternSymV}};
    return out;
}

} // namespace

// D-OPT-LOAD-ALIAS-ANALYSIS-STRICT-TBAA-WIRING (cycle 10d): proves the
// MirLoweringConfig.strictAliasingOnDistinctTypes flag is honored at
// HIR→MIR boundary. The compile_pipeline reads from
// `grammar.semantics().pointerAliasing.strictAliasingOnDistinctTypes`
// and threads into MirLoweringConfig; lowerToMir calls
// MirBuilder::setAliasingMode based on the field. Paired
// (default false / explicit true) so a regression that ignores the
// field fails one arm.
TEST(MirLoweringExtern, MirAliasingModeDefaultsToPermissive) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};
    FfiMetadata meta;
    meta.mangledName   = "printf";
    meta.importLibrary = "libc.so.6";
    ffi.set(built.externNode, meta);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    MirLoweringConfig cfg;  // strictAliasingOnDistinctTypes defaults false
    auto result = lowerToMir(built.hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, cfg, &ffi);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.mir.aliasingMode(), MirAliasingMode::Permissive);
}

// Bridge pin (covers BOTH alias flags' MirLoweringConfig → lowerToMir
// → Mir threading in one fixture): set both flags to NON-DEFAULT
// values and assert both on the resulting Mir. Catches the "knob
// that lies" silent-failure class — e.g. a typo at hir_to_mir.cpp's
// setter calls that forwards one flag's value into the other field,
// OR a future maintainer wiring up a third flag who copies the
// pattern wrong. Without this pin, individual round-trip tests + a
// per-flag predicate test still pass while the bridge is broken.
TEST(MirLoweringExtern, MirAliasingFlagsBothBridgeThroughLowerToMir) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};
    FfiMetadata meta;
    meta.mangledName   = "printf";
    meta.importLibrary = "libc.so.6";
    ffi.set(built.externNode, meta);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    MirLoweringConfig cfg;
    // Both NON-DEFAULT: opt into strict-TBAA AND opt OUT of the C99
    // §6.5 ¶7 character-type exception (a hypothetical Rust-like
    // strict-typed language).
    cfg.strictAliasingOnDistinctTypes = true;
    cfg.charTypesAliasAll             = false;
    auto result = lowerToMir(built.hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, cfg, &ffi);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.mir.aliasingMode(), MirAliasingMode::StrictTBAA)
        << "MirLoweringConfig.strictAliasingOnDistinctTypes=true must "
           "translate to StrictTBAA on the lowered Mir";
    EXPECT_FALSE(result.mir.charTypesAliasAll())
        << "MirLoweringConfig.charTypesAliasAll=false must translate "
           "to charTypesAliasAll()==false on the lowered Mir";
}

TEST(MirLoweringExtern, MirAliasingModeFlipsToStrictTBAAWhenConfigOptsIn) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};
    FfiMetadata meta;
    meta.mangledName   = "printf";
    meta.importLibrary = "libc.so.6";
    ffi.set(built.externNode, meta);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    MirLoweringConfig cfg;
    cfg.strictAliasingOnDistinctTypes = true;
    auto result = lowerToMir(built.hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, cfg, &ffi);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.mir.aliasingMode(), MirAliasingMode::StrictTBAA)
        << "MirLoweringConfig.strictAliasingOnDistinctTypes=true must "
           "translate to StrictTBAA on the lowered Mir";
}

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
    HirBuilder b{"c"};
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
    HirBuilder b{"c"};
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
    HirBuilder b{"c"};
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
    HirBuilder b{"c"};
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
    HirBuilder b{"c"};
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
    HirBuilder b{"c"};
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

// TLS C1 (D-CSUBSET-THREAD-LOCAL): an ExternGlobal whose declaration carries
// the HirThreadLocalMap attribute mints its ExternImport row with
// isThreadLocal=true — the `extern thread_local int e;` cross-TU carrier
// (the LK11 merge's survivingExterns copies the WHOLE row, so the flag rides
// the merge by construction; the linker-side surviving-import handling is
// slice C). The unmarked sibling in the SAME module stays false — exact
// per-row assertions. RED-ON-DISABLE: drop the threadLocalMap read at the
// collectExterns ExternGlobal arm and the marked row's EXPECT reds.
TEST(MirLoweringExtern, ExternGlobalThreadLocalFlagReachesImportRow) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirBuilder b{"c"};
    constexpr std::uint32_t kTlsSym   = 41;
    constexpr std::uint32_t kPlainSym = 42;
    HirNodeId const tlsEg   = b.makeExternGlobal(i32, kTlsSym);
    HirNodeId const plainEg = b.makeExternGlobal(i32, kPlainSym);
    HirNodeId const root = b.makeModule(std::array{tlsEg, plainEg});
    Hir hir = std::move(b).finish(root);

    HirFfiMap ffi{hir};
    FfiMetadata mTls;
    mTls.mangledName = "e";  mTls.importLibrary = "libc.so.6";
    ffi.set(tlsEg, mTls);
    FfiMetadata mPlain;
    mPlain.mangledName = "g"; mPlain.importLibrary = "libc.so.6";
    ffi.set(plainEg, mPlain);
    HirThreadLocalMap tls{hir};
    tls.set(tlsEg, ThreadLocalAttr{/*isThreadLocal=*/true});

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, MirLoweringConfig{},
                             &ffi, /*linkageMap=*/nullptr,
                             /*mutabilityMap=*/nullptr,
                             /*volatileMap=*/nullptr,
                             /*alignmentMap=*/nullptr, &tls);
    ASSERT_TRUE(result.ok)
        << (rep.all().empty() ? "" : rep.all()[0].actual);
    ASSERT_EQ(result.externImports.size(), 2u);
    EXPECT_TRUE(result.externImports[0].isData);
    EXPECT_TRUE(result.externImports[0].isThreadLocal)
        << "the marked extern-data row must carry thread storage duration";
    EXPECT_TRUE(result.externImports[1].isData);
    EXPECT_FALSE(result.externImports[1].isThreadLocal)
        << "the unmarked sibling must stay process-shared";
}

// ── D-CSUBSET-WEAK-EXTERN-IMPORT-NOT-IN-SYMBOL-TABLE — THE MINT SITE ─────
//
// `weak` on an extern IMPORT reached the HIR linkage map and STOPPED. HIR→MIR
// consumed `linkageMap` for function DEFINITIONS and GLOBALS only, so an extern
// import — which is neither — left the attribute in a map nothing read. The bit
// was parsed, understood, recorded, and dropped one layer below where it was
// recorded, which is why nothing upstream reported a problem and every emitted
// object marked the undefined symbol STRONG on all three formats.
//
// ★ THE SINGLE ARM IS PINNED BESIDE ITS CONTROL, in the same lowering, because
// a lowering that stamped Weak on EVERY import would be the worse defect: a
// REQUIRED symbol becomes optional, the image links with it missing, and the
// reference reads through null with no diagnostic.
//
// ★ BOTH RAILS, because `ExternGlobal` and `ExternFunction` build their rows
// separately and a weak DATA import and a weak FUNCTION import are the same fact
// about the same rail — two open-coded reads is how the two would drift.
TEST(MirLoweringExtern, WeakOnAnExternImportReachesTheImportRowOnBothRails) {
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    HirBuilder b{"c"};
    constexpr std::uint32_t kWeakDataSym = 51;
    constexpr std::uint32_t kPlainDataSym = 52;
    constexpr std::uint32_t kWeakFnSym   = 53;
    constexpr std::uint32_t kPlainFnSym  = 54;
    HirNodeId const weakData  = b.makeExternGlobal(i32, kWeakDataSym);
    HirNodeId const plainData = b.makeExternGlobal(i32, kPlainDataSym);
    HirNodeId const weakFn    = b.makeExternFunction(fnTy, kWeakFnSym, {});
    HirNodeId const plainFn   = b.makeExternFunction(fnTy, kPlainFnSym, {});
    HirNodeId const root =
        b.makeModule(std::array{weakData, plainData, weakFn, plainFn});
    Hir hir = std::move(b).finish(root);

    HirFfiMap ffi{hir};
    auto meta = [&](HirNodeId n, char const* name) {
        FfiMetadata m;
        m.mangledName = name;
        m.importLibrary = "libc.so.6";
        ffi.set(n, m);
    };
    meta(weakData, "wd");
    meta(plainData, "pd");
    meta(weakFn, "wf");
    meta(plainFn, "pf");

    // The declaration's OWN linkage, recorded on the extern node exactly as
    // `recordExtern` records it in CST→HIR. The two unannotated siblings are
    // deliberately left ABSENT from the map rather than set to Global: absence
    // is the correct externally-visible default, and it is the shape every
    // ordinary extern actually has.
    HirLinkageMap linkage{hir};
    LinkageAttr weakAttr;
    weakAttr.binding = SymbolBinding::Weak;
    linkage.set(weakData, weakAttr);
    linkage.set(weakFn, weakAttr);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, MirLoweringConfig{},
                             &ffi, &linkage);
    ASSERT_TRUE(result.ok)
        << (rep.all().empty() ? "" : rep.all()[0].actual);
    ASSERT_EQ(result.externImports.size(), 4u);

    auto bindingOf = [&](std::string_view name) -> SymbolBinding {
        for (auto const& e : result.externImports)
            if (e.mangledName == name) return e.binding;
        ADD_FAILURE() << "no import row named " << name;
        return SymbolBinding::Global;
    };
    EXPECT_EQ(bindingOf("wd"), SymbolBinding::Weak)
        << "a WEAK extern DATA import must carry its binding onto the import row "
           "-- otherwise the object marks the undefined symbol STRONG and a "
           "program that tests it for null cannot link at all.";
    EXPECT_EQ(bindingOf("wf"), SymbolBinding::Weak)
        << "the FUNCTION rail must read the same map at the same site -- two "
           "open-coded reads is how the two rails drift.";
    EXPECT_EQ(bindingOf("pd"), SymbolBinding::Global)
        << "the CONTROL: an unannotated DATA import stays a STRONG reference.";
    EXPECT_EQ(bindingOf("pf"), SymbolBinding::Global)
        << "the CONTROL: an unannotated FUNCTION import stays a STRONG reference.";
}

// A `Local` binding on an extern import is UNSPELLABLE — an import is a name
// this object does not define, and module-private is the one thing such a name
// cannot be. No format encodes an undefined LOCAL symbol, so folding it to
// Global (or to Weak) would silently change which linker can resolve the
// reference. It is refused at the DECLARATION, where the source span still
// exists, rather than at a walker that could only say "unresolved symbol".
TEST(MirLoweringExtern, LocalBindingOnAnExternImportFailsLoud) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirBuilder b{"c"};
    constexpr std::uint32_t kSym = 61;
    HirNodeId const eg   = b.makeExternGlobal(i32, kSym);
    HirNodeId const root = b.makeModule(std::array{eg});
    Hir hir = std::move(b).finish(root);

    HirFfiMap ffi{hir};
    FfiMetadata m;
    m.mangledName = "loc";
    m.importLibrary = "libc.so.6";
    ffi.set(eg, m);
    HirLinkageMap linkage{hir};
    LinkageAttr attr;
    attr.binding = SymbolBinding::Local;
    linkage.set(eg, attr);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep,
                             /*sourceMap=*/nullptr, MirLoweringConfig{},
                             &ffi, &linkage);
    EXPECT_FALSE(result.ok)
        << "an unspellable import binding must stop the lowering, not ride to a "
           "writer that has no encoding for it";
    ASSERT_GE(rep.errorCount(), 1u);
    bool sawIt = false;
    for (auto const& d : rep.all())
        if (d.actual.find("D-CSUBSET-WEAK-EXTERN-IMPORT-NOT-IN-SYMBOL-TABLE")
            != std::string::npos)
            sawIt = true;
    EXPECT_TRUE(sawIt)
        << "the refusal must name the row that owns the import-binding rail, so a "
           "reader lands on the rule rather than on a generic lowering failure";
    EXPECT_TRUE(result.externImports.empty())
        << "the refused declaration must mint NO import row -- a half-refused "
           "extern that still reached the linker would be the silent half of this "
           "defect wearing a diagnostic.";
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
    HirBuilder b{"c"};
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

// ── FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): the HIR→MIR seam (the audit's C1 fix). ──
// A module whose `main`-like function CALLS a symbol (99) that the CST→HIR skip left OUT
// of both the extern-import set AND the defined-function set — the referenced-only pe64
// <threads.h> shim shape. `collectThreadShimSymbols` must SEED `functionSymbols` from the
// `synthRecipeMap` so the callee Ref lowers to GlobalAddr(99); without it the Ref is
// "unbound" and lowering fails loud (the exact break the audit's C1 fix prevents).
namespace {
struct SeamBuilt { Hir hir; std::uint32_t shimSymV; };
[[nodiscard]] SeamBuilt buildModuleCallingShim(TypeInterner& ti) {
    TypeId const i32     = ti.primitive(TypeKind::I32);
    TypeId const shimSig = ti.fnSig({}, i32, CallConv::CcSysV);   // the shim: fn()->i32
    TypeId const fSig    = ti.fnSig({}, i32, CallConv::CcSysV);
    constexpr std::uint32_t kShimSym = 99;   // NOT defined here, NOT an extern
    HirBuilder b{"c"};
    HirNodeId const callee = b.makeRef(shimSig, kShimSym);
    HirNodeId const call   = b.makeCall(callee, {}, i32);
    HirNodeId const ret    = b.makeReturn(call);
    HirNodeId const body   = b.makeBlock(std::array{ret});
    HirNodeId const f      = b.makeFunction(fSig, /*symbol=*/1, {}, body);
    HirNodeId const root   = b.makeModule(std::array{f});
    return SeamBuilt{std::move(b).finish(root), kShimSym};
}
[[nodiscard]] bool hasGlobalAddrTo(Mir const& m, std::uint32_t symV) {
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi) {
        MirFuncId const f = m.funcAt(fi);
        for (std::uint32_t bi = 0; bi < m.funcBlockCount(f); ++bi) {
            MirBlockId const bb = m.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < m.blockInstCount(bb); ++ii) {
                MirInstId const inst = m.blockInstAt(bb, ii);
                if (m.instOpcode(inst) == MirOpcode::GlobalAddr
                    && m.globalAddrSymbol(inst).v == symV)
                    return true;
            }
        }
    }
    return false;
}
} // namespace

TEST(MirLoweringExtern, ThreadsShimSymbolSeedsFunctionSymbols) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleCallingShim(ti);
    std::unordered_map<std::uint32_t, std::string> const recipes{{built.shimSymV, "mtx_lock"}};
    DiagnosticReporter rep;
    HirLiteralPool pool;
    MirLoweringConfig cfg;
    auto result = lowerToMir(built.hir, pool, ti, rep, /*sourceMap=*/nullptr, cfg,
                             /*ffiMap=*/nullptr, nullptr, nullptr, nullptr, nullptr,
                             nullptr, nullptr, nullptr, nullptr, &recipes);
    ASSERT_TRUE(result.ok)
        << "a Ref to a seeded shim symbol must lower cleanly (the C1 functionSymbols seed)";
    EXPECT_TRUE(hasGlobalAddrTo(result.mir, built.shimSymV))
        << "the shim call callee must lower to GlobalAddr(shimSym), not an unbound error";
}

// RED-on-disable: the SAME module WITHOUT the seed (null map) → the shim Ref is unbound →
// lowering fails loud. If this ever passes, the seed became load-BEARING-less.
TEST(MirLoweringExtern, ThreadsShimUnseededRefFailsLoud) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleCallingShim(ti);
    DiagnosticReporter rep;
    HirLiteralPool pool;
    MirLoweringConfig cfg;
    auto result = lowerToMir(built.hir, pool, ti, rep, /*sourceMap=*/nullptr, cfg,
                             /*ffiMap=*/nullptr, nullptr, nullptr, nullptr, nullptr,
                             nullptr, nullptr, nullptr, nullptr, /*synthRecipeMap=*/nullptr);
    EXPECT_FALSE(result.ok)
        << "without the seed the shim Ref is unbound — lowering must fail loud, never silent";
}
