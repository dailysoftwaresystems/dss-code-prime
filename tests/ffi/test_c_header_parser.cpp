// Plan 11 FF2 (C header parser) tests — `dss::ffi::readCHeader[FromText]`.
//
// Pins:
//   * extern function declarations land as ImportSurface rows with
//     SymbolKind::Function and the user-supplied importLibrary.
//   * extern global declarations land as ImportSurface rows with
//     SymbolKind::Object.
//   * typedef declarations are accepted but produce no row.
//   * NON-extern function definitions (function bodies) fail loud with
//     F_HeaderHasFunctionBody — declarations-only mode.
//   * NON-extern globals fail loud with F_HeaderHasNonExternDecl.
//   * Empty importLibrary at the entry fails loud (would be silent-
//     failure surface — caller forgot to specify the library and a
//     downstream linker would either silently drop the row or fail
//     with a less specific code).
//   * Tokenize / parse / semantic errors propagate as
//     F_HeaderParseFailed with the underlying P_*/S_* codes also
//     reaching the reporter.
//   * Diagnostic-code name round-trip for the 3 new F_* codes.
//   * Shipped pre-reduced libc/stdio.h round-trips through the parser.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "ffi/c_header_parser.hpp"

#include <gtest/gtest.h>

#include <filesystem>

using namespace dss;
using namespace dss::ffi;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r,
                                    DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == c) ++n;
    return n;
}

} // namespace

TEST(FfiCHeaderParser, ExternFunctionLandsAsImportSurfaceRow) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "extern int puts(const char* s);\n",
        "<test>",
        "libc.so.6",
        rep);
    ASSERT_TRUE(rowsOrErr.has_value()) << headerReadErrorKindName(rowsOrErr.error().kind);
    auto const& rows = *rowsOrErr;
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].mangledName, "puts");
    EXPECT_EQ(rows[0].libraryPath, "libc.so.6");
    EXPECT_EQ(rows[0].kind, SymbolKind::Function);
    EXPECT_EQ(rows[0].visibility, SymbolVisibility::Default);
    EXPECT_EQ(rows[0].linkage, SymbolLinkage::External);
    EXPECT_FALSE(rows[0].cSignature.has_value());  // FF3 owns signature shape
}

TEST(FfiCHeaderParser, ExternGlobalLandsAsImportSurfaceRow) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "extern int errno;\n",
        "<test>",
        "libc.so.6",
        rep);
    ASSERT_TRUE(rowsOrErr.has_value()) << headerReadErrorKindName(rowsOrErr.error().kind);
    auto const& rows = *rowsOrErr;
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].mangledName, "errno");
    EXPECT_EQ(rows[0].kind, SymbolKind::Object);
    EXPECT_EQ(rows[0].linkage, SymbolLinkage::External);
}

TEST(FfiCHeaderParser, MixedExternsLandInDeclarationOrder) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "extern int puts(const char* s);\n"
        "extern int errno;\n"
        "extern int putchar(int c);\n",
        "<test>",
        "libc.so.6",
        rep);
    ASSERT_TRUE(rowsOrErr.has_value()) << headerReadErrorKindName(rowsOrErr.error().kind);
    auto const& rows = *rowsOrErr;
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].mangledName, "puts");
    EXPECT_EQ(rows[0].kind, SymbolKind::Function);
    EXPECT_EQ(rows[1].mangledName, "errno");
    EXPECT_EQ(rows[1].kind, SymbolKind::Object);
    EXPECT_EQ(rows[2].mangledName, "putchar");
    EXPECT_EQ(rows[2].kind, SymbolKind::Function);
}

TEST(FfiCHeaderParser, FunctionBodyRejectedLoud) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "int f(int x) { return x; }\n",
        "<test>",
        "libc.so.6",
        rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::HeaderHasFunctionBody);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_HeaderHasFunctionBody), 1u);
}

TEST(FfiCHeaderParser, NonExternGlobalRejectedLoud) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "int counter;\n",
        "<test>",
        "libc.so.6",
        rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::HeaderHasNonExternDecl);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_HeaderHasNonExternDecl), 1u);
}

TEST(FfiCHeaderParser, EmptyImportLibraryRejectedAtEntry) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "extern int puts(const char* s);\n",
        "<test>",
        "",  // empty — would be silent-failure surface downstream
        rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::HeaderParseFailed);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_HeaderParseFailed), 1u);
}

TEST(FfiCHeaderParser, ParseFailurePropagatesUnderlyingDiagnostics) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "@@@ this is not c @@@\n",
        "<test>",
        "libc.so.6",
        rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::HeaderParseFailed);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_HeaderParseFailed), 1u);
    // The c-subset frontend's underlying tokenize/parse/semantic
    // diagnostics must reach the caller's reporter — FF2 wraps the
    // verdict, doesn't swallow the cause. Pinned by: more than just
    // the FF2-layer F_HeaderParseFailed reaches the reporter.
    EXPECT_GT(rep.all().size(), 1u)
        << "expected at least one underlying frontend diagnostic in "
           "reporter alongside the FF2-layer F_HeaderParseFailed";
}

TEST(FfiCHeaderParser, EmptyHeaderProducesEmptySurface) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "",
        "<test>",
        "libc.so.6",
        rep);
    ASSERT_TRUE(rowsOrErr.has_value()) << headerReadErrorKindName(rowsOrErr.error().kind);
    EXPECT_EQ(rowsOrErr->size(), 0u);
}

TEST(FfiCHeaderParser, FileNotFoundReportsFileOpenFailed) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeader(
        fs::path{"this/path/definitely/does/not/exist/nope.h"},
        "libc.so.6",
        rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::FileOpenFailed);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_FileOpenFailed), 1u);
}

TEST(FfiCHeaderParser, DiagnosticCodeNameRoundTripFHeaderParseFailed) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_HeaderParseFailed),
              "F_HeaderParseFailed");
}

TEST(FfiCHeaderParser, DiagnosticCodeNameRoundTripFHeaderHasFunctionBody) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_HeaderHasFunctionBody),
              "F_HeaderHasFunctionBody");
}

TEST(FfiCHeaderParser, DiagnosticCodeNameRoundTripFHeaderHasNonExternDecl) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_HeaderHasNonExternDecl),
              "F_HeaderHasNonExternDecl");
}

TEST(FfiCHeaderParser, HeaderReadErrorKindNameRoundTrip) {
    EXPECT_EQ(headerReadErrorKindName(HeaderReadErrorKind::FileOpenFailed),
              "FileOpenFailed");
    EXPECT_EQ(headerReadErrorKindName(HeaderReadErrorKind::HeaderParseFailed),
              "HeaderParseFailed");
    EXPECT_EQ(headerReadErrorKindName(HeaderReadErrorKind::HeaderHasFunctionBody),
              "HeaderHasFunctionBody");
    EXPECT_EQ(headerReadErrorKindName(HeaderReadErrorKind::HeaderHasNonExternDecl),
              "HeaderHasNonExternDecl");
}

TEST(FfiCHeaderParser, ShippedLibcStdioParsesCleanly) {
    DiagnosticReporter rep;
    // src/dss-config/ffi-headers/libc/stdio.h is the smallest shipped
    // pre-reduced header (plan 11 §4 Q1). Pins that the shipped
    // header round-trips through the FF2 dispatch — the per-row
    // contents are pinned via the explicit-text tests above; this
    // pins that the file-on-disk pathway also works.
    fs::path here = fs::current_path();
    fs::path candidate{};
    for (fs::path p = here; !p.empty() && p != p.root_path(); p = p.parent_path()) {
        fs::path try1 = p / "src" / "dss-config" / "ffi-headers" / "libc" / "stdio.h";
        if (fs::exists(try1)) { candidate = try1; break; }
    }
    if (candidate.empty()) {
        GTEST_SKIP() << "shipped libc/stdio.h not located from cwd; "
                     << "not a regression — only pinnable when run with "
                     << "the repo root reachable upward from cwd.";
    }
    auto rowsOrErr = readCHeader(candidate, "libc.so.6", rep);
    ASSERT_TRUE(rowsOrErr.has_value()) << headerReadErrorKindName(rowsOrErr.error().kind);
    EXPECT_EQ(rowsOrErr->size(), 2u);
    EXPECT_EQ((*rowsOrErr)[0].mangledName, "puts");
    EXPECT_EQ((*rowsOrErr)[1].mangledName, "putchar");
    for (auto const& row : *rowsOrErr) {
        EXPECT_EQ(row.libraryPath, "libc.so.6");
        EXPECT_EQ(row.kind, SymbolKind::Function);
    }
}
