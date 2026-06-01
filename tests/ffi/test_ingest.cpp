// Plan 11 FF5 (`ingest()`) tests — closes D-FF4-Apply + D-FF5-INGESTION-SOURCE
// + D-FF6-HEADER-DIR-READER at the ingest boundary.
//
// Pins:
//   * IngestionSource variant accepts each of the 3 source kinds.
//   * Matches a HIR ExternFunction to an aggregated ImportSurface row
//     by canonical name (post-FF4-unapply for binary-reader rows,
//     direct for header-parser rows).
//   * Populates HirFfiMap with the per-format-decorated mangledName
//     (FF4 apply), correct importLibrary, and converted linkage /
//     visibility.
//   * Unmatched externs do NOT fail; FF5 reports
//     externsAnnotated < externs.size() so the driver can warn.
//   * Multi-file directory ingest (D-FF6) enumerates *.h in
//     alphabetical order.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "ffi/ingest.hpp"
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/hir_node.hpp"
#include "link/object_format_schema.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::Location;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

TypeInterner makeInterner() { return TypeInterner{CompilationUnitId{1}}; }

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

// ── Happy path: header source → matched extern → ffiMap populated ─

TEST(FfiIngest, CHeaderSourceMatchesAndAnnotatesExtern) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    // Write a one-decl header to a scratch file.
    ScratchDir scratch{Location::Temp, "ff5-header"};
    auto const tmpDir = scratch.path();
    auto const headerPath = tmpDir / "tiny.h";
    {
        std::ofstream out{headerPath};
        out << "extern int puts(const char* s);\n";
    }

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array sources{
        IngestionSource{CHeaderSource{headerPath, "libc.so.6"}}};
    std::array externs{
        ExternDeclRef{built.externNode, "puts"}};
    auto result = ingest(sources, externs, **target, **format, ffi, rep);

    EXPECT_TRUE(result.ok()) << "rep.errorCount=" << rep.errorCount();
    EXPECT_EQ(result.externsAnnotated, 1u);
    EXPECT_EQ(result.sourcesProcessed, 1u);
    EXPECT_EQ(result.rowsAggregated, 1u);

    auto const* meta = ffi.tryGet(built.externNode);
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->mangledName, "puts");  // ELF → no decoration
    EXPECT_EQ(meta->importLibrary, "libc.so.6");
    EXPECT_EQ(meta->linkage, FfiLinkage::Strong);
    EXPECT_EQ(meta->visibility, FfiVisibility::Default);

}

// ── D-FF4-Apply on Mach-O: applyCMangling adds leading underscore ─

TEST(FfiIngest, MachOFormatAppliesLeadingUnderscoreOnMangling) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    ScratchDir scratch{Location::Temp, "ff5-macho"};
    auto const tmpDir = scratch.path();
    auto const headerPath = tmpDir / "tiny.h";
    {
        std::ofstream out{headerPath};
        out << "extern int puts(const char* s);\n";
    }

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array sources{
        IngestionSource{CHeaderSource{headerPath, "libSystem.B.dylib"}}};
    std::array externs{
        ExternDeclRef{built.externNode, "puts"}};
    auto result = ingest(sources, externs, **target, **format, ffi, rep);

    ASSERT_TRUE(result.ok()) << "rep.errorCount=" << rep.errorCount();
    EXPECT_EQ(result.externsAnnotated, 1u);

    auto const* meta = ffi.tryGet(built.externNode);
    ASSERT_NE(meta, nullptr);
    // Mach-O decorates with leading underscore (D-FF4-Apply contract).
    EXPECT_EQ(meta->mangledName, "_puts");
    EXPECT_EQ(meta->importLibrary, "libSystem.B.dylib");

}

// ── Unmatched extern: silent skip, externsAnnotated < externs.size ─

TEST(FfiIngest, UnmatchedExternIsSilentlySkipped) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    ScratchDir scratch{Location::Temp, "ff5-unmatched"};
    auto const tmpDir = scratch.path();
    auto const headerPath = tmpDir / "tiny.h";
    {
        std::ofstream out{headerPath};
        out << "extern int other_func(int x);\n";
    }

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array sources{
        IngestionSource{CHeaderSource{headerPath, "libc.so.6"}}};
    std::array externs{
        ExternDeclRef{built.externNode, "puts"}};  // not in header
    auto result = ingest(sources, externs, **target, **format, ffi, rep);

    // No error — the linker will fail loud later if puts stays unresolved.
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 0u);
    EXPECT_EQ(result.rowsAggregated, 1u);
    EXPECT_EQ(ffi.tryGet(built.externNode), nullptr);

}

// ── D-FF5-INGESTION-SOURCE: variant accepts all 3 source kinds ────

TEST(FfiIngest, IngestionSourceVariantHoldsThreeKinds) {
    IngestionSource a{BinaryLibrarySource{"/tmp/some.so"}};
    IngestionSource b{CHeaderSource{"/tmp/some.h", "libc.so.6"}};
    IngestionSource c{CHeaderDirSource{"/tmp/headers", "libc.so.6"}};
    EXPECT_TRUE(std::holds_alternative<BinaryLibrarySource>(a));
    EXPECT_TRUE(std::holds_alternative<CHeaderSource>(b));
    EXPECT_TRUE(std::holds_alternative<CHeaderDirSource>(c));
}

// ── D-FF6-HEADER-DIR-READER: enumerate *.h in alphabetical order ──

TEST(FfiIngest, ReadCHeaderDirectoryEnumeratesAlphabetical) {
    ScratchDir scratch{Location::Temp, "ff5-dir"};
    auto const tmpDir = scratch.path();
    {
        std::ofstream{tmpDir / "stdio.h"}
            << "extern int puts(const char* s);\n";
        std::ofstream{tmpDir / "stdlib.h"}
            << "extern void exit(int status);\n";
        std::ofstream{tmpDir / "not_a_header.txt"}
            << "ignored\n";
    }
    DiagnosticReporter rep;
    auto rows = readCHeaderDirectory(tmpDir, "libc.so.6", rep);
    ASSERT_TRUE(rows.has_value()) << headerReadErrorKindName(rows.error().kind);
    ASSERT_EQ(rows->size(), 2u);
    // Alphabetical: stdio.h before stdlib.h.
    EXPECT_EQ((*rows)[0].mangledName, "puts");
    EXPECT_EQ((*rows)[1].mangledName, "exit");
}

TEST(FfiIngest, ReadCHeaderDirectoryRejectsNonDirectory) {
    DiagnosticReporter rep;
    auto rows = readCHeaderDirectory("/no/such/dir/here", "libc.so.6", rep);
    ASSERT_FALSE(rows.has_value());
    EXPECT_EQ(rows.error().kind, HeaderReadErrorKind::FileOpenFailed);
}

TEST(FfiIngest, ReadCHeaderDirectoryRejectsEmptyImportLibrary) {
    ScratchDir scratch{Location::Temp, "ff5-emptyLib"};
    auto const tmpDir = scratch.path();
    DiagnosticReporter rep;
    auto rows = readCHeaderDirectory(tmpDir, "", rep);
    ASSERT_FALSE(rows.has_value());
    EXPECT_EQ(rows.error().kind, HeaderReadErrorKind::EmptyImportLibrary);
}

// ── Composes with CHeaderDirSource variant via ingest() ───────────

TEST(FfiIngest, CHeaderDirSourceAggregatesMultipleFiles) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    ScratchDir scratch{Location::Temp, "ff5-dirsource"};
    auto const tmpDir = scratch.path();
    {
        std::ofstream{tmpDir / "a.h"}
            << "extern int puts(const char* s);\n";
        std::ofstream{tmpDir / "b.h"}
            << "extern void exit(int status);\n";
    }

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array sources{
        IngestionSource{CHeaderDirSource{tmpDir, "libc.so.6"}}};
    std::array externs{
        ExternDeclRef{built.externNode, "puts"}};
    auto result = ingest(sources, externs, **target, **format, ffi, rep);

    ASSERT_TRUE(result.ok()) << "rep.errorCount=" << rep.errorCount();
    EXPECT_EQ(result.rowsAggregated, 2u);  // both a.h + b.h contribute
    EXPECT_EQ(result.externsAnnotated, 1u);  // only puts is in externs[]

}

// ── Post-fold #5 silent-failure CRITICAL-3 + code-reviewer #88 ──

TEST(FfiIngest, DuplicateSymbolEmitsDedicatedWarningCode) {
    // Cross-source duplicate uses F_FfiIngestDuplicateSymbol (NOT
    // F_HeaderParseFailed — that's the per-file parse-failure code).
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    ScratchDir scratch{Location::Temp, "ff5-dup"};
    auto const tmpDir = scratch.path();
    {
        std::ofstream{tmpDir / "first.h"}
            << "extern int puts(const char* s);\n";
        std::ofstream{tmpDir / "second.h"}
            << "extern int puts(const char* s);\n";
    }

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array sources{IngestionSource{CHeaderDirSource{tmpDir, "libc.so.6"}}};
    std::array externs{ExternDeclRef{built.externNode, "puts"}};
    auto result = ingest(sources, externs, **target, **format, ffi, rep);

    EXPECT_TRUE(result.ok())
        << "duplicate is a Warning — ok() stays true";
    EXPECT_EQ(result.rowsAggregated, 2u);
    EXPECT_EQ(result.externsAnnotated, 1u);

    bool sawDup = false, sawParseFailed = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_FfiIngestDuplicateSymbol) sawDup = true;
        if (d.code == DiagnosticCode::F_HeaderParseFailed) sawParseFailed = true;
    }
    EXPECT_TRUE(sawDup);
    EXPECT_FALSE(sawParseFailed);
}

// ── Post-fold #5 silent-failure C2: WASM short-circuit ─────────

TEST(FfiIngest, OperandStackTargetShortCircuitsWithPlanNotLanded) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    auto target = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"wasm32","version":"0.0","abiModel":"operand-stack"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("wasm32-v1");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array<IngestionSource, 0> sources{};
    std::array externs{ExternDeclRef{built.externNode, "puts"}};
    auto result = ingest(sources, externs, **target, **format, ffi, rep);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 0u);
    EXPECT_EQ(ffi.tryGet(built.externNode), nullptr);
    bool sawPlanNotLanded = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::D_PlanNotLanded) sawPlanNotLanded = true;
    }
    EXPECT_TRUE(sawPlanNotLanded);
}

// ── Post-fold #5 P6 multiple-extern match ─────────────────────

TEST(FfiIngest, MultipleExternsAllMatchedGetAnnotated) {
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    HirBuilder b{"c-subset"};
    HirNodeId const ef1 = b.makeExternFunction(fnTy, /*symbol=*/100, {});
    HirNodeId const ef2 = b.makeExternFunction(fnTy, /*symbol=*/101, {});
    HirNodeId const root = b.makeModule(std::array{ef1, ef2});
    Hir hir = std::move(b).finish(root);
    HirFfiMap ffi{hir};

    ScratchDir scratch{Location::Temp, "ff5-multi"};
    auto const tmpDir = scratch.path();
    {
        std::ofstream{tmpDir / "h.h"}
            << "extern int puts(const char* s);\n"
            << "extern void exit(int status);\n";
    }

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array sources{IngestionSource{CHeaderSource{tmpDir / "h.h", "libc.so.6"}}};
    std::array externs{
        ExternDeclRef{ef1, "puts"},
        ExternDeclRef{ef2, "exit"},
    };
    auto result = ingest(sources, externs, **target, **format, ffi, rep);

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 2u);
    EXPECT_NE(ffi.tryGet(ef1), nullptr);
    EXPECT_NE(ffi.tryGet(ef2), nullptr);
}

// ── Diagnostic code name round-trip ─────────────────────────────

TEST(FfiIngest, DiagnosticCodeNameRoundTripFFfiIngestDuplicateSymbol) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_FfiIngestDuplicateSymbol),
              "F_FfiIngestDuplicateSymbol");
}

// ── FF3 (resolveAbi) gate: bad (target, format) pair fails ingest ─

TEST(FfiIngest, AbiResolveFailureSkipsIngestion) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    auto target = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"riscv64","version":"0.0","abiModel":"register-machine"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array<IngestionSource, 0> sources{};
    std::array externs{ExternDeclRef{built.externNode, "puts"}};
    auto result = ingest(sources, externs, **target, **format, ffi, rep);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 0u);
    EXPECT_EQ(ffi.tryGet(built.externNode), nullptr);
    // The underlying F_AbiUnknownTuple reached the reporter.
    bool sawAbiError = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_AbiUnknownTuple) sawAbiError = true;
    }
    EXPECT_TRUE(sawAbiError);
}
