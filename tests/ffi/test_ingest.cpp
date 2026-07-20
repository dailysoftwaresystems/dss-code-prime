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
#include "diagnostic_count.hpp"
#include "ffi/ingest.hpp"
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/hir_node.hpp"
#include "link/object_format_schema.hpp"
#include "byte_emit.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::Location;
using dss::test_support::ScratchDir;
using dss::test_support::appU16;
using dss::test_support::appU32;
using dss::test_support::appU64;
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

// ── D-FF1-READER-SONAME (c171) consumer fixture ────────────────────
//
// `ingest()` consumes `IngestionSource` (a binary path / header), NOT raw
// `ImportSurface` rows — there is no row-injection seam. So to witness the
// consumer's soname-PREFER logic end-to-end we synthesize a real ELF `.so`
// (the canonical DT_SONAME carrier), write it to a scratch file, and feed it
// via `BinaryLibrarySource`. This is a minimal single-export sibling of
// `tests/ffi/test_binary_reader.cpp`'s `buildMinimalElf64` (which lives in a
// sibling TU's anonymous namespace and cannot be shared); it reuses the shared
// `byte_emit` primitives and emits an optional `.dynamic`/DT_SONAME the same
// way. Layout: Ehdr(64) | .dynstr | .dynsym | .shstrtab | [.dynamic] | SHT.
[[nodiscard]] std::vector<std::uint8_t>
buildElfSo(std::string const& exportName, std::string const& soname) {
    bool const hasDynamic = !soname.empty();
    std::vector<std::uint8_t> b;
    b.resize(64, 0);  // Ehdr placeholder

    // .dynstr: NUL sentinel, exportName, [soname].
    std::vector<std::uint8_t> dynstr;
    dynstr.push_back(0);
    std::uint32_t const nameStrOff = static_cast<std::uint32_t>(dynstr.size());
    for (char c : exportName) dynstr.push_back(static_cast<std::uint8_t>(c));
    dynstr.push_back(0);
    std::uint32_t sonameStrOff = 0;
    if (hasDynamic) {
        sonameStrOff = static_cast<std::uint32_t>(dynstr.size());
        for (char c : soname) dynstr.push_back(static_cast<std::uint8_t>(c));
        dynstr.push_back(0);
    }
    std::uint64_t const dynstrOff = b.size();
    b.insert(b.end(), dynstr.begin(), dynstr.end());

    // .dynsym: STN_UNDEF slot (24 zero bytes) + one global STT_FUNC export.
    while (b.size() % 8 != 0) b.push_back(0);
    std::uint64_t const dynsymOff = b.size();
    for (int i = 0; i < 24; ++i) b.push_back(0);           // slot 0 = STN_UNDEF
    appU32(b, nameStrOff);                                 // st_name
    b.push_back(static_cast<std::uint8_t>((1u << 4) | 2u));// STB_GLOBAL|STT_FUNC
    b.push_back(0);                                        // st_other = STV_DEFAULT
    appU16(b, 1);                                          // st_shndx
    appU64(b, 0x1000);                                     // st_value
    appU64(b, 0);                                          // st_size
    std::uint64_t const dynsymSize = b.size() - dynsymOff;

    // .shstrtab
    std::uint64_t const shstrtabOff = b.size();
    b.push_back(0);
    auto pushName = [&](char const* s) -> std::uint32_t {
        std::uint32_t off = static_cast<std::uint32_t>(b.size() - shstrtabOff);
        for (char const* p = s; *p; ++p) b.push_back(static_cast<std::uint8_t>(*p));
        b.push_back(0);
        return off;
    };
    std::uint32_t const nDynstr   = pushName(".dynstr");
    std::uint32_t const nDynsym   = pushName(".dynsym");
    std::uint32_t const nShstrtab = pushName(".shstrtab");
    std::uint32_t const nDynamic  = hasDynamic ? pushName(".dynamic") : 0u;
    std::uint64_t const shstrtabSize = b.size() - shstrtabOff;

    // .dynamic (DT_SONAME + DT_NULL), only when a soname is requested.
    std::uint64_t dynamicOff = 0, dynamicSize = 0;
    if (hasDynamic) {
        while (b.size() % 8 != 0) b.push_back(0);
        dynamicOff = b.size();
        appU64(b, 14); appU64(b, sonameStrOff);  // DT_SONAME (tag 14)
        appU64(b, 0);  appU64(b, 0);             // DT_NULL
        dynamicSize = b.size() - dynamicOff;
    }

    // Section header table.
    while (b.size() % 8 != 0) b.push_back(0);
    std::uint64_t const shtOff = b.size();
    auto writeShdr = [&](std::uint32_t name, std::uint32_t type,
                         std::uint64_t off, std::uint64_t size,
                         std::uint32_t link, std::uint64_t entsize) {
        appU32(b, name); appU32(b, type);
        appU64(b, 0); appU64(b, 0);        // flags, addr
        appU64(b, off); appU64(b, size);
        appU32(b, link); appU32(b, 0);     // link, info
        appU64(b, 1); appU64(b, entsize);  // addralign, entsize
    };
    writeShdr(0, 0, 0, 0, 0, 0);                               // NULL
    writeShdr(nDynstr, 3, dynstrOff, dynstr.size(), 0, 0);     // .dynstr (STRTAB)
    writeShdr(nDynsym, 11, dynsymOff, dynsymSize, 1, 24);      // .dynsym (DYNSYM)
    writeShdr(nShstrtab, 3, shstrtabOff, shstrtabSize, 0, 0);  // .shstrtab
    if (hasDynamic)
        writeShdr(nDynamic, 6, dynamicOff, dynamicSize, 1, 16);// .dynamic (DYNAMIC)

    // Ehdr.
    b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 2;   // ELFCLASS64
    b[5] = 1;   // ELFDATA2LSB
    b[6] = 1;   // EV_CURRENT
    b[16] = 3;  // ET_DYN
    b[18] = 62; // EM_X86_64
    b[20] = 1;
    std::memcpy(&b[40], &shtOff, 8);                        // e_shoff
    b[52] = 64;                                             // e_ehsize
    b[58] = 64;                                             // e_shentsize
    b[60] = static_cast<std::uint8_t>(hasDynamic ? 5 : 4); // e_shnum
    b[62] = 3;                                              // e_shstrndx
    return b;
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

// c162 (D-FF1-READER-CONSUMER): `ingest()` is a BIND MECHANISM, not the
// validation-policy owner. An extern that matches NO row in the aggregated
// surface is SILENTLY SKIPPED here (no FfiMetadata written, ok() stays true)
// -- the descriptor-aware VALIDATION POLICY (a bare `extern puts;` falls
// through to its format-default library; a genuine typo fails loud) lives in
// the sole production caller (compile_pipeline step 2.5), which has the
// shipped-descriptor knowledge `ingest()` lacks. A blanket fail-loud HERE
// would wrongly reject a legitimate `bare extern puts + --resolve-library
// ownlib` mixed program (silent-failure review HIGH, folded).
TEST(FfiIngest, UnmatchedExternIsSkippedBindMechanism) {
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

    // Unmatched -> skipped (no error, no binding); the CALLER owns the policy.
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

TEST(FfiIngest, OperandStackTargetShortCircuitsWithDedicatedCode) {
    // Post-fold #6: dedicated F_FfiIngestAbiModelUnsupported code
    // replaces the previous D_PlanNotLanded reuse. The pairing is a
    // permanent architectural exclusion, NOT pending-arrival.
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    auto target = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"wasm32","version":"0.0","abiModel":"operand-stack"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ((*target)->abiModel(), TargetAbiModel::OperandStack);
    auto format = ObjectFormatSchema::loadShipped("wasm32-v1");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array<IngestionSource, 0> sources{};
    std::array externs{ExternDeclRef{built.externNode, "puts"}};
    auto result = ingest(sources, externs, **target, **format, ffi, rep);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 0u);
    EXPECT_EQ(ffi.tryGet(built.externNode), nullptr);
    bool sawAbiUnsupported = false, sawPlanNotLanded = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_FfiIngestAbiModelUnsupported) {
            sawAbiUnsupported = true;
        }
        if (d.code == DiagnosticCode::D_PlanNotLanded) sawPlanNotLanded = true;
    }
    EXPECT_TRUE(sawAbiUnsupported);
    EXPECT_FALSE(sawPlanNotLanded)
        << "must NOT use D_PlanNotLanded — that's pending-arrival surface";
}

// ── Post-fold #6 silent-failure C1: empty canonical reject ────

TEST(FfiIngest, EmptyExternDeclRefCanonicalNameRejectedLoud) {
    // Caller-side empty canonicalName must NOT silently bind to an
    // empty-string row in bySymbol.
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array<IngestionSource, 0> sources{};
    std::array externs{ExternDeclRef{built.externNode, ""}};
    auto result = ingest(sources, externs, **target, **format, ffi, rep);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 0u);
    EXPECT_EQ(ffi.tryGet(built.externNode), nullptr);
    bool sawEmpty = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_FfiIngestEmptyCanonical) sawEmpty = true;
    }
    EXPECT_TRUE(sawEmpty);
}

// ── Post-fold #6 H2 + Gap 2: partial-failure surface ──────────

TEST(FfiIngest, ReadCHeaderDirectoryPartialFailureReturnsSurface) {
    // Pin H1: one bad header in a directory must NOT amputate the
    // good headers' symbols from the aggregated surface.
    ScratchDir scratch{Location::Temp, "ff5-partial"};
    auto const tmpDir = scratch.path();
    {
        std::ofstream{tmpDir / "good.h"}
            << "extern int puts(const char* s);\n";
        std::ofstream{tmpDir / "bad.h"}
            << "this is not valid C at all @@@\n";
    }
    DiagnosticReporter rep;
    auto rows = readCHeaderDirectory(tmpDir, "libc.so.6", rep);
    // Partial success: good.h's row reaches the surface; bad.h's
    // failure is in the reporter; readCHeaderDirectory returns
    // expected (not unexpected) because at least one file parsed.
    ASSERT_TRUE(rows.has_value())
        << "partial-success contract: at least one file parsed";
    bool sawPuts = false;
    for (auto const& r : *rows) {
        if (r.mangledName == "puts") sawPuts = true;
    }
    EXPECT_TRUE(sawPuts);
    EXPECT_GT(rep.errorCount(), 0u)
        << "bad.h's parse failure is recorded in the reporter";
}

TEST(FfiIngest, ReadCHeaderDirectoryAllFailuresPropagatesError) {
    // Pin the other branch: when EVERY file fails, return unexpected
    // with the first failure's HeaderReadError detail.
    ScratchDir scratch{Location::Temp, "ff5-allfail"};
    auto const tmpDir = scratch.path();
    {
        std::ofstream{tmpDir / "bad1.h"} << "garbage 1 @@@\n";
        std::ofstream{tmpDir / "bad2.h"} << "garbage 2 @@@\n";
    }
    DiagnosticReporter rep;
    auto rows = readCHeaderDirectory(tmpDir, "libc.so.6", rep);
    ASSERT_FALSE(rows.has_value());
    EXPECT_EQ(rows.error().kind, HeaderReadErrorKind::HeaderParseFailed);
}

// ── Post-fold #6 H7: ok() snapshot semantics pin ──────────────

TEST(FfiIngest, OkSnapshotImmuneToPostReturnReporterErrors) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    ScratchDir scratch{Location::Temp, "ff5-snapshot"};
    auto const tmpDir = scratch.path();
    {
        std::ofstream{tmpDir / "h.h"} << "extern int puts(const char* s);\n";
    }
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array sources{
        IngestionSource{CHeaderSource{tmpDir / "h.h", "libc.so.6"}}};
    std::array externs{ExternDeclRef{built.externNode, "puts"}};
    auto result = ingest(sources, externs, **target, **format, ffi, rep);
    ASSERT_TRUE(result.ok());

    // Emit an error AFTER ingest returns — ok() must NOT flip.
    dss::report(rep, DiagnosticCode::F_AbiUnknownTuple,
                DiagnosticSeverity::Error, "post-ingest error");
    EXPECT_TRUE(result.ok())
        << "ok() reflects snapshot at return, not live reporter state";
    EXPECT_EQ(rep.errorCount(), 1u);
}

// ── Post-fold #5 P6 multiple-extern match + #6 H6 cross-bind ─

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
    // post-fold #6 H6: pin per-extern decoration so a refactor that
    // matches by FnSig (instead of canonical name) or that binds the
    // SAME row to both externs trips the test.
    auto const* metaEf1 = ffi.tryGet(ef1);
    auto const* metaEf2 = ffi.tryGet(ef2);
    ASSERT_NE(metaEf1, nullptr);
    ASSERT_NE(metaEf2, nullptr);
    EXPECT_EQ(metaEf1->mangledName, "puts");
    EXPECT_EQ(metaEf2->mangledName, "exit");
    EXPECT_NE(metaEf1->mangledName, metaEf2->mangledName);
}

// ── Diagnostic code name round-trips ───────────────────────────

TEST(FfiIngest, DiagnosticCodeNameRoundTripFFfiIngestDuplicateSymbol) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_FfiIngestDuplicateSymbol),
              "F_FfiIngestDuplicateSymbol");
}
TEST(FfiIngest, DiagnosticCodeNameRoundTripFFfiIngestAbiModelUnsupported) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_FfiIngestAbiModelUnsupported),
              "F_FfiIngestAbiModelUnsupported");
}
TEST(FfiIngest, DiagnosticCodeNameRoundTripFFfiIngestEmptyCanonical) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_FfiIngestEmptyCanonical),
              "F_FfiIngestEmptyCanonical");
}
TEST(FfiIngest, DiagnosticCodeNameRoundTripDTargetAbiModelUnsupportedByDriver) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_TargetAbiModelUnsupportedByDriver),
              "D_TargetAbiModelUnsupportedByDriver");
}
TEST(FfiIngest, DiagnosticCodeNameRoundTripFFfiResolveLibrarySymbolAbsent) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_FfiResolveLibrarySymbolAbsent),
              "F_FfiResolveLibrarySymbolAbsent");
}

// c162 (D-FF1-READER-CONSUMER): a BinaryLibrarySource carrying a non-empty
// `importName` OVERRIDES the reader's path-derived libraryPath on every row
// it produced, so the resolved extern's import records the loader-resolvable
// soname/DLL-name rather than the absolute build-time path. Proven here
// through the header-source sibling is not possible (importName is
// binary-only), so this pins the STRUCT contract: the field exists, defaults
// empty, and a set value survives into the variant. The end-to-end binding
// effect is witnessed by the round-trip integration test.
TEST(FfiIngest, BinaryLibrarySourceImportNameDefaultsEmptyAndCarries) {
    BinaryLibrarySource a{"/tmp/libdsslib.so"};
    EXPECT_TRUE(a.importName.empty());
    BinaryLibrarySource b{"/build/abs/path/libdsslib.so", "libdsslib.so"};
    EXPECT_EQ(b.importName, "libdsslib.so");
    IngestionSource src{b};
    ASSERT_TRUE(std::holds_alternative<BinaryLibrarySource>(src));
    EXPECT_EQ(std::get<BinaryLibrarySource>(src).importName, "libdsslib.so");
}

// ── D-FF1-READER-SONAME (c171): ingest() prefers the embedded soname ──
//
// The consumer binds `meta.importLibrary = row.soname.empty() ? libraryPath
// : row.soname` (and `meta.soname = row.soname`). We witness both arms by
// feeding a real ELF `.so`: the source's `importName` ("widget-basename.so",
// the c162 driver override that stands in for the file basename) is what
// `libraryPath` becomes, so an embedded DT_SONAME must WIN over even that
// explicit override. RED-ON-DISABLE on BOTH: drop the prefer and the PREFER
// phase reads "widget-basename.so"; the FALLBACK phase proves the empty-soname
// row still resolves to the basename.
TEST(FfiIngest, IngestPrefersSonameOverBasenameForImportLibrary) {
    ScratchDir scratch{Location::Temp, "ff5-soname"};
    auto const dir = scratch.path();

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());

    // Build a `.so` exporting `puts` (optionally carrying `soname`), write it
    // to a scratch file, run ingest() with `importName` = "widget-basename.so"
    // + a matching `puts` extern, and return the resolved FfiMetadata.
    auto runIngest = [&](std::string const& fileName,
                         std::string const& soname) -> FfiMetadata {
        auto const path = dir / fileName;
        {
            auto const bytes = buildElfSo("puts", soname);
            std::ofstream out{path, std::ios::binary};
            out.write(reinterpret_cast<char const*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        TypeInterner ti = makeInterner();
        auto built = buildModuleWithExtern(ti);
        HirFfiMap ffi{built.hir};
        DiagnosticReporter rep;
        std::array sources{IngestionSource{
            BinaryLibrarySource{path, "widget-basename.so"}}};
        std::array externs{ExternDeclRef{built.externNode, "puts"}};
        auto result = ingest(sources, externs, **target, **format, ffi, rep);
        EXPECT_TRUE(result.ok()) << "rep.errorCount=" << rep.errorCount();
        EXPECT_EQ(result.externsAnnotated, 1u);
        auto const* meta = ffi.tryGet(built.externNode);
        EXPECT_NE(meta, nullptr);
        return meta ? *meta : FfiMetadata{};
    };

    // PREFER: the embedded DT_SONAME beats even the importName override.
    FfiMetadata const preferred = runIngest("libwidget.so", "libwidget.so.2");
    EXPECT_EQ(preferred.importLibrary, "libwidget.so.2")
        << "the embedded DT_SONAME must be PREFERRED over the basename override";
    EXPECT_EQ(preferred.soname, "libwidget.so.2");
    EXPECT_EQ(preferred.mangledName, "puts");  // ELF: no decoration

    // FALLBACK: no DT_SONAME ⇒ importLibrary falls back to the basename
    // (the overridden libraryPath), and soname stays empty.
    FfiMetadata const fallback = runIngest("libplain.so", "");
    EXPECT_EQ(fallback.importLibrary, "widget-basename.so")
        << "with no DT_SONAME, importLibrary must fall back to the basename";
    EXPECT_TRUE(fallback.soname.empty());
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

// ── synthesizeFfiFromSourceDecls (FF6 Slice 2, 2026-06-02) ─────
//
// Source-declared sibling of `ingest()`. Where `ingest()` validates
// against an aggregated ImportSurface, this function trusts each
// caller-supplied ExternDeclRef and produces FfiMetadata directly
// from FF4 C-mangling + the caller-supplied per-format library.
// Used by `compileSingleUnit` step 2.5 to thread the language's
// `externLibraryByFormat` map into the per-CU HirFfiMap.

TEST(FfiSynthesize, PEFormatProducesMangledNameAndImportLibrary) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array externs{ExternDeclRef{built.externNode, "puts"}};
    auto result = synthesizeFfiFromSourceDecls(
        externs, "msvcrt.dll", **target, **format, ffi, rep);

    EXPECT_TRUE(result.ok()) << "rep.errorCount=" << rep.errorCount();
    EXPECT_EQ(result.externsAnnotated, 1u);
    // Pin reporter cleanness: no spurious Warning/Error during
    // happy path. Without this a future regression that ALSO
    // emitted (say) F_FfiIngestDuplicateSymbol per row would
    // silently pass the `ok()` test (which only snapshots
    // errorCount).
    EXPECT_EQ(rep.errorCount(), 0u);

    auto const* meta = ffi.tryGet(built.externNode);
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->mangledName, "puts");  // PE x86_64: no leading underscore
    EXPECT_EQ(meta->importLibrary, "msvcrt.dll");
    EXPECT_EQ(meta->linkage, FfiLinkage::Strong);
    EXPECT_EQ(meta->visibility, FfiVisibility::Default);
}

TEST(FfiSynthesize, MachOFormatAppliesLeadingUnderscore) {
    // FF4 apply-mangling on Mach-O prepends `_`. The synthesize
    // path uses the SAME applyCMangling kernel as ingest(), so
    // this pins both paths share the per-format decoration rule.
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array externs{ExternDeclRef{built.externNode, "puts"}};
    auto result = synthesizeFfiFromSourceDecls(
        externs, "/usr/lib/libSystem.B.dylib",
        **target, **format, ffi, rep);

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 1u);
    EXPECT_EQ(rep.errorCount(), 0u);
    auto const* meta = ffi.tryGet(built.externNode);
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->mangledName, "_puts");
    EXPECT_EQ(meta->importLibrary, "/usr/lib/libSystem.B.dylib");
}

TEST(FfiSynthesize, NoLibraryBindingLeavesImportLibraryEmpty) {
    // c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): a `noLibraryBinding`
    // ExternDeclRef (a bare-prototype cross-TU reference) OPTS OUT of the
    // format-default fallback — `importLibrary` stays EMPTY on purpose and
    // the flag is stamped through to FfiMetadata so the HIR→MIR extern
    // pre-pass admits the row. The C-mangling still applies (the LK11 merge
    // and the linker key on the mangled name) — pinned on Mach-O where the
    // decoration is observable. RED-ON-DISABLE: dropping the flag branch in
    // synthesizeFfiFromSourceDecls would bind the format default
    // ("/usr/lib/libSystem.B.dylib") — a genuinely-undefined typo'd function
    // would then "link" against libc and fail only at LOAD (the fail-loud
    // DOWNGRADE the c86 Section-B design rejected).
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array externs{ExternDeclRef{built.externNode, "puts",
                                     /*libraryOverride=*/{},
                                     /*noLibraryBinding=*/true}};
    auto result = synthesizeFfiFromSourceDecls(
        externs, "/usr/lib/libSystem.B.dylib",
        **target, **format, ffi, rep);

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 1u);
    EXPECT_EQ(rep.errorCount(), 0u);
    auto const* meta = ffi.tryGet(built.externNode);
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->mangledName, "_puts")
        << "C-mangling must still apply — the merge/linker key on it";
    EXPECT_TRUE(meta->importLibrary.empty())
        << "a no-library-binding extern must NOT inherit the format default";
    EXPECT_TRUE(meta->noLibraryBinding);
}

TEST(FfiSynthesize, EmptyImportLibraryFailsLoudWithDedicatedCode) {
    // The language config's `externLibraryByFormat` map has no
    // entry for the active object format ⇒ the synthesis call
    // sees an empty importLibrary and rejects loud. Distinct
    // F_FfiNoImportLibraryForFormat code so the diagnostic
    // anchors at the upstream config gap rather than at the
    // downstream linker (K_FormatLacksImportSupport /
    // K_SymbolUndefined).
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array externs{ExternDeclRef{built.externNode, "puts"}};
    auto result = synthesizeFfiFromSourceDecls(
        externs, /*importLibrary=*/"", **target, **format, ffi, rep);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 0u);
    EXPECT_EQ(ffi.tryGet(built.externNode), nullptr);
    // Pin EXACTLY one F_FfiNoImportLibraryForFormat (architectural
    // gate at the head of synthesize — fires once per call, not
    // per extern). A future refactor that moved the check into
    // the per-extern loop would fire N times for N externs;
    // EXPECT_EQ count=1 catches that regression.
    EXPECT_EQ(::dss::test_support::countCode(
                  rep, DiagnosticCode::F_FfiNoImportLibraryForFormat),
              1u);
    EXPECT_EQ(rep.errorCount(), 1u);
}

TEST(FfiSynthesize, EmptyCanonicalNameFailsLoudPerExtern) {
    // Same trap as ingest()'s F_FfiIngestEmptyCanonical surface —
    // an empty canonical name would shadow the empty-string key
    // in any downstream by-name lookup. Reuses the existing code
    // so a future audit that suppresses the surface in one place
    // hits both producers.
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array externs{ExternDeclRef{built.externNode, ""}};
    auto result = synthesizeFfiFromSourceDecls(
        externs, "msvcrt.dll", **target, **format, ffi, rep);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 0u);
    EXPECT_EQ(ffi.tryGet(built.externNode), nullptr);
    // Exactly one F_FfiIngestEmptyCanonical (per-extern guard
    // fired for the single empty entry).
    EXPECT_EQ(::dss::test_support::countCode(
                  rep, DiagnosticCode::F_FfiIngestEmptyCanonical),
              1u);
    EXPECT_EQ(rep.errorCount(), 1u);
}

TEST(FfiSynthesize, OperandStackAbiModelShortCircuitsWithDedicatedCode) {
    // Same architectural exclusion as ingest() — operand-stack
    // (WASM) / result-id (SPIR-V) targets don't take FF4
    // C-mangling. Plan 17/18 own their own ingest surfaces.
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
    std::array externs{ExternDeclRef{built.externNode, "puts"}};
    auto result = synthesizeFfiFromSourceDecls(
        externs, "libc.so.6", **target, **format, ffi, rep);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 0u);
    // Exactly one architectural-exclusion error (fires once at
    // the gate, not per extern).
    EXPECT_EQ(::dss::test_support::countCode(
                  rep,
                  DiagnosticCode::F_FfiIngestAbiModelUnsupported),
              1u);
    EXPECT_EQ(rep.errorCount(), 1u);
}

TEST(FfiSynthesize, MultipleExternsAllReceiveSameLibrary) {
    // All externs in one synthesis call share the per-format
    // library — the typical c-subset shape (every extern in a
    // single TU resolves to the same runtime DLL). Pin distinct
    // per-extern mangledNames + identical importLibrary so a
    // refactor that accidentally cross-binds the library trips
    // the test.
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    HirBuilder b{"c-subset"};
    HirNodeId const ef1 = b.makeExternFunction(fnTy, /*symbol=*/200, {});
    HirNodeId const ef2 = b.makeExternFunction(fnTy, /*symbol=*/201, {});
    HirNodeId const root = b.makeModule(std::array{ef1, ef2});
    Hir hir = std::move(b).finish(root);
    HirFfiMap ffi{hir};

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array externs{
        ExternDeclRef{ef1, "puts"},
        ExternDeclRef{ef2, "fprintf"},
    };
    auto result = synthesizeFfiFromSourceDecls(
        externs, "msvcrt.dll", **target, **format, ffi, rep);

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 2u);
    EXPECT_EQ(rep.errorCount(), 0u);
    auto const* m1 = ffi.tryGet(ef1);
    auto const* m2 = ffi.tryGet(ef2);
    ASSERT_NE(m1, nullptr);
    ASSERT_NE(m2, nullptr);
    EXPECT_EQ(m1->mangledName, "puts");
    EXPECT_EQ(m2->mangledName, "fprintf");
    EXPECT_NE(m1->mangledName, m2->mangledName);
    EXPECT_EQ(m1->importLibrary, "msvcrt.dll");
    EXPECT_EQ(m2->importLibrary, "msvcrt.dll");
}

// FF6 Slice 2 audit fold (2026-06-02 post-fold #1): test-analyzer
// G6 — ELF path of synthesize wasn't pinned directly. Most ELF
// validation lives in ingest() tests; synthesize() shares the FF4
// applyCMangling kernel but a refactor that special-cases function-
// vs-global on the ingest side without mirroring on synthesize
// would silently land without this pin.
TEST(FfiSynthesize, ELFFormatNoLeadingUnderscore) {
    TypeInterner ti = makeInterner();
    auto built = buildModuleWithExtern(ti);
    HirFfiMap ffi{built.hir};

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array externs{ExternDeclRef{built.externNode, "puts"}};
    auto result = synthesizeFfiFromSourceDecls(
        externs, "libc.so.6", **target, **format, ffi, rep);

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(rep.errorCount(), 0u);
    auto const* meta = ffi.tryGet(built.externNode);
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->mangledName, "puts");  // ELF: no decoration
    EXPECT_EQ(meta->importLibrary, "libc.so.6");
}

// test-analyzer G7 — ExternGlobal arm parity. All 7 synthesize
// tests above use ExternFunction; this pin proves the per-kind
// distinction lives on the HIR node (not on the FfiMetadata side-
// table) so both ExternFunction and ExternGlobal pass through the
// SAME FF4-mangling + library-binding path with identical surface.
TEST(FfiSynthesize, ExternGlobalAlsoReceivesMangling) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirBuilder b{"c-subset"};
    constexpr std::uint32_t kSymV = 300;
    HirNodeId const eg = b.makeExternGlobal(i32, /*symbol=*/kSymV);
    HirNodeId const root = b.makeModule(std::array{eg});
    Hir hir = std::move(b).finish(root);
    HirFfiMap ffi{hir};

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped(
        "macho64-x86_64-darwin");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array externs{ExternDeclRef{eg, "errno"}};
    auto result = synthesizeFfiFromSourceDecls(
        externs, "/usr/lib/libSystem.B.dylib",
        **target, **format, ffi, rep);

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 1u);
    EXPECT_EQ(rep.errorCount(), 0u);
    auto const* meta = ffi.tryGet(eg);
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->mangledName, "_errno");  // Mach-O leading _
    EXPECT_EQ(meta->importLibrary, "/usr/lib/libSystem.B.dylib");
}

// silent-failure HIGH-4 fold (2026-06-02 post-fold #1):
// mixed-validity case — extern[0] empty canonical fails loud,
// extern[1] valid still gets annotated. Pins the per-extern
// `continue` behavior so a future refactor to a fail-fast `return`
// is caught (which would silently regress the documented partial-
// annotation contract on the HirIngestResult docblock).
TEST(FfiSynthesize, MixedValidityPartialAnnotation) {
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    HirBuilder b{"c-subset"};
    HirNodeId const efBad  = b.makeExternFunction(fnTy, /*sym=*/400, {});
    HirNodeId const efGood = b.makeExternFunction(fnTy, /*sym=*/401, {});
    HirNodeId const root = b.makeModule(std::array{efBad, efGood});
    Hir hir = std::move(b).finish(root);
    HirFfiMap ffi{hir};

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped(
        "pe64-x86_64-windows");
    ASSERT_TRUE(format.has_value());

    DiagnosticReporter rep;
    std::array externs{
        ExternDeclRef{efBad,  ""},     // empty canonical → fails loud per-extern
        ExternDeclRef{efGood, "puts"}, // proceeds despite predecessor's error
    };
    auto result = synthesizeFfiFromSourceDecls(
        externs, "msvcrt.dll", **target, **format, ffi, rep);

    // !ok() (errorCount > 0) AND externsAnnotated == 1 (the
    // good one). Partial-annotation contract pinned.
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.externsAnnotated, 1u);
    EXPECT_EQ(ffi.tryGet(efBad), nullptr);
    auto const* good = ffi.tryGet(efGood);
    ASSERT_NE(good, nullptr);
    EXPECT_EQ(good->mangledName, "puts");
    EXPECT_EQ(good->importLibrary, "msvcrt.dll");
    EXPECT_EQ(::dss::test_support::countCode(
                  rep, DiagnosticCode::F_FfiIngestEmptyCanonical),
              1u);
}

TEST(FfiSynthesize, DiagnosticCodeNameRoundTripFFfiNoImportLibraryForFormat) {
    // Symbol-table round-trip pin so the unsuppressable-codes
    // closed table's membership stays observable from outside.
    EXPECT_EQ(diagnosticCodeName(
                  DiagnosticCode::F_FfiNoImportLibraryForFormat),
              "F_FfiNoImportLibraryForFormat");
}
