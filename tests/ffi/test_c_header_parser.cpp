// Plan 11 FF2 (C header parser) tests — `dss::ffi::readCHeader[FromText,Shipped]`.
//
// Each test pins a single contract; together they cover the closed
// set of HeaderReadErrorKind variants plus the happy paths. The
// shipped-header tests use `readCHeaderShipped` so the test no
// longer duplicates the `findShippedConfig` walk-up.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "ffi/c_header_parser.hpp"
#include "diagnostic_count.hpp"

#include <gtest/gtest.h>

#include <filesystem>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::countCode;
namespace fs = std::filesystem;

// ── Happy-path row emission ───────────────────────────────────

TEST(FfiCHeaderParser, ExternFunctionLandsAsImportSurfaceRow) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "extern int puts(const char* s);\n",
        "<test>", "libc.so.6", rep);
    ASSERT_TRUE(rowsOrErr.has_value()) << headerReadErrorKindName(rowsOrErr.error().kind);
    auto const& rows = *rowsOrErr;
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].mangledName, "puts");
    EXPECT_EQ(rows[0].libraryPath, "libc.so.6");
    EXPECT_EQ(rows[0].kind, SymbolKind::Function);
    EXPECT_EQ(rows[0].visibility, SymbolVisibility::Default);
    EXPECT_EQ(rows[0].linkage, SymbolLinkage::External);
}

TEST(FfiCHeaderParser, ExternGlobalLandsAsImportSurfaceRow) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "extern int errno;\n",
        "<test>", "libc.so.6", rep);
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
        "<test>", "libc.so.6", rep);
    ASSERT_TRUE(rowsOrErr.has_value()) << headerReadErrorKindName(rowsOrErr.error().kind);
    auto const& rows = *rowsOrErr;
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].mangledName, "puts");
    EXPECT_EQ(rows[0].kind, SymbolKind::Function);
    EXPECT_EQ(rows[1].mangledName, "errno");
    EXPECT_EQ(rows[1].kind, SymbolKind::Object);
    EXPECT_EQ(rows[2].mangledName, "putchar");
    EXPECT_EQ(rows[2].kind, SymbolKind::Function);
    // Per-row invariants tightening (pr-test-analyzer P6 fold):
    // a refactor introducing per-row library inference must trip
    // the test, not slip past on the size+name check alone.
    for (auto const& row : rows) {
        EXPECT_EQ(row.libraryPath, "libc.so.6");
        EXPECT_EQ(row.visibility, SymbolVisibility::Default);
        EXPECT_EQ(row.linkage, SymbolLinkage::External);
    }
}

TEST(FfiCHeaderParser, EmptyHeaderProducesEmptySurfaceAndNoDiagnostic) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "", "<test>", "libc.so.6", rep);
    ASSERT_TRUE(rowsOrErr.has_value()) << headerReadErrorKindName(rowsOrErr.error().kind);
    EXPECT_EQ(rowsOrErr->size(), 0u);
    // No-input must produce no diagnostic — pins the "informational
    // chatter on empty input" silent-failure surface (pr-test-analyzer
    // P5 fold).
    EXPECT_EQ(rep.all().size(), 0u);
}

TEST(FfiCHeaderParser, TypedefAcceptedProducesNoRow) {
    // pr-test-analyzer P8 fold: typedef is documented as "absorbed
    // into the type system, no surface row". This pins that the
    // TypeDecl arm of the kind switch produces 0 rows and 0
    // diagnostics — a refactor accidentally rejecting typedefs
    // would surface here.
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "typedef int byte_t;\n"
        "extern int puts(const char* s);\n",
        "<test>", "libc.so.6", rep);
    ASSERT_TRUE(rowsOrErr.has_value()) << headerReadErrorKindName(rowsOrErr.error().kind);
    ASSERT_EQ(rowsOrErr->size(), 1u);
    EXPECT_EQ((*rowsOrErr)[0].mangledName, "puts");
}

// ── Each error kind has at least one test ─────────────────────

TEST(FfiCHeaderParser, FunctionBodyRejectedLoud) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "int f(int x) { return x; }\n",
        "<test>", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::HeaderHasFunctionBody);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_HeaderHasFunctionBody), 1u);
}

TEST(FfiCHeaderParser, NonExternGlobalRejectedLoud) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "int counter;\n",
        "<test>", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::HeaderHasNonExternDecl);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_HeaderHasNonExternDecl), 1u);
}

TEST(FfiCHeaderParser, EmptyImportLibraryRejectedAtEntry) {
    // Post-fold type-design split: this is the EmptyImportLibrary
    // kind / F_HeaderEmptyImportLibrary code (caller-API misuse),
    // distinct from F_HeaderParseFailed (header source error).
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "extern int puts(const char* s);\n",
        "<test>", "", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::EmptyImportLibrary);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_HeaderEmptyImportLibrary), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_HeaderParseFailed), 0u);
}

TEST(FfiCHeaderParser, ParseFailurePropagatesUnderlyingDiagnostics) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "@@@ this is not c @@@\n",
        "<test>", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::HeaderParseFailed);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_HeaderParseFailed), 1u);
    // Underlying frontend diagnostics MUST reach the caller's
    // reporter — FF2 wraps the verdict, doesn't swallow the cause.
    EXPECT_GT(rep.all().size(), 1u)
        << "expected at least one underlying frontend diagnostic in "
           "reporter alongside the FF2-layer F_HeaderParseFailed";
}

TEST(FfiCHeaderParser, DuplicateExternRedeclarationRejectedByFrontend) {
    // pr-test-analyzer P8 fold: same-symbol redeclaration must
    // propagate the c-subset frontend's S_* / P_* code, not be
    // silently merged. If the frontend ever relaxes redecl, the
    // test trips and we re-decide FF2's stance.
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "extern int puts(const char* s);\n"
        "extern int puts(const char* s);\n",
        "<test>", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::HeaderParseFailed);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_HeaderParseFailed), 1u);
    EXPECT_GT(rep.all().size(), 1u);  // underlying S_* / P_* reaches reporter
}

TEST(FfiCHeaderParser, FileNotFoundReportsFileOpenFailed) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeader(
        fs::path{"this/path/definitely/does/not/exist/nope.h"},
        "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::FileOpenFailed);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_FileOpenFailed), 1u);
}

// ── Source-span attribution on FF2-layer diagnostics ──────────

TEST(FfiCHeaderParser, RejectDiagnosticCarriesSourceSpan) {
    // Silent-failure HIGH-4 fold: header-mode rejection diagnostics
    // (HeaderHasFunctionBody / HeaderHasNonExternDecl / etc.) carry
    // a (buffer, span) tuple pointing at the offending top-level
    // decl. Without this, LSP / editor integrations can't underline
    // the source.
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "int f(int x) { return x; }\n",
        "<test>", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    bool foundWithSpan = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_HeaderHasFunctionBody) {
            EXPECT_TRUE(d.buffer.valid())
                << "F_HeaderHasFunctionBody must carry a BufferId";
            EXPECT_GT(d.span.length(), 0u)
                << "F_HeaderHasFunctionBody must carry a non-empty span";
            foundWithSpan = true;
            break;
        }
    }
    EXPECT_TRUE(foundWithSpan);
}

TEST(FfiCHeaderParser, ErrorStructCarriesLocationOnRejection) {
    // D-FF2-2: HeaderReadError::at carries (buffer, span) of the
    // offending decl so programmatic consumers (LSP, test pins,
    // future introspection) can locate the error without re-parsing
    // reporter prose. Mirrors the (buffer, span) that emitAndReturn
    // already threads into the emitted ParseDiagnostic. Post-fold #7
    // T1: no optional<> wrapper — `HirSourceLoc{}` is the absent
    // sentinel (buffer.valid() == false).
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "int f(int x) { return x; }\n",
        "<test>", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    auto const& err = rowsOrErr.error();
    EXPECT_EQ(err.kind, HeaderReadErrorKind::HeaderHasFunctionBody);
    EXPECT_TRUE(err.at.isPresent())
        << "HeaderReadError::at.buffer must be valid for per-decl "
           "rejections";
    EXPECT_GT(err.at.span.length(), 0u)
        << "HeaderReadError::at.span must be non-empty";
}

TEST(FfiCHeaderParser, ErrorStructAtSetForHeaderHasNonExternDecl) {
    // Post-fold #7 PT2a: every per-decl rejection site must populate
    // `at`, not just HeaderHasFunctionBody. Pins the NonExternDecl arm.
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "int counter;\n",
        "<test>", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::HeaderHasNonExternDecl);
    EXPECT_TRUE(rowsOrErr.error().at.isPresent());
    EXPECT_GT(rowsOrErr.error().at.span.length(), 0u);
}

TEST(FfiCHeaderParser, ErrorStructAtSetOnLoweringFailure) {
    // Post-fold #7 silent-failure F2: D-FF2-3's H_ExternHasInitializer
    // is emitted by lowering, then wrapped as HeaderParseFailed at the
    // FF2 boundary. Without the F2 fold, the wrap dropped the locus —
    // the LSP consumer would see HeaderReadError::at absent despite a
    // span-bearing diagnostic in the reporter. Pin that the wrap now
    // forwards the first reported Error's span into the struct.
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "extern int x = 5;\n",
        "<test>", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::HeaderParseFailed);
    EXPECT_TRUE(rowsOrErr.error().at.isPresent())
        << "HeaderReadError::at must mirror the underlying lowering "
           "diagnostic's span — F2 fold contract";
    EXPECT_GT(rowsOrErr.error().at.span.length(), 0u);
}

TEST(FfiCHeaderParser, FirstReportedErrorSpanBoundedToCurrentCall) {
    // Post-fold #8 self-audit CRITICAL (rewritten post-#8 audit):
    // concrete consumer is `readCHeaderDirectory` (ingest.cpp loop)
    // — it reuses ONE reporter across N header reads. Without the
    // subspan(errStart) bound, file #2's HeaderReadError.at would
    // inherit file #1's leftover span on the shared reporter.
    //
    // Both calls below take the SAME lowering-failure path
    // (`extern int = ...;`) on the shared reporter; both calls'
    // diagnostics have spans. Pre-fix, call 2's scan starting at
    // index 0 would return call 1's H_ExternHasInitializer span
    // (in <call1>'s buffer). With the subspan bound, call 2's
    // `at` correctly points into <call2>'s buffer.
    //
    // The earlier post-#7 version of this test used EmptyImportLibrary
    // for call 2, which short-circuits BEFORE the F2 scan — it pinned
    // a contract that was already true pre-fix. The current shape
    // genuinely regression-blocks the subspan(errStart) bound.
    DiagnosticReporter rep;

    auto first = readCHeaderFromText(
        "extern int x = 5;\n",
        "<call1>", "libc.so.6", rep);
    ASSERT_FALSE(first.has_value());
    ASSERT_TRUE(first.error().at.isPresent())
        << "test setup: call 1 must produce a span-bearing error";
    auto const call1Buffer = first.error().at.buffer;

    // Call 2 on the SAME reporter, also a lowering-fail with span,
    // but in a DIFFERENT buffer.
    auto second = readCHeaderFromText(
        "extern int y = 7;\n",
        "<call2>", "libc.so.6", rep);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().kind, HeaderReadErrorKind::HeaderParseFailed);
    ASSERT_TRUE(second.error().at.isPresent())
        << "test setup: call 2 must also produce a span-bearing error";
    EXPECT_NE(second.error().at.buffer, call1Buffer)
        << "Call 2's at.buffer must point into <call2>, not <call1>. "
           "Pre-fix the unbounded scan returned call 1's earlier "
           "span; this is the genuine regression-blocker for the "
           "subspan(errStart) bound.";
}

TEST(FfiCHeaderParser, ErrorStructLocationAbsentEvenWithPriorCallError) {
    // Companion to the cross-call test: when call 2 takes a path that
    // legitimately has no decl locus (EmptyImportLibrary entry-point
    // error), `at` must STILL stay default — call 1's leftover error
    // on the shared reporter must not leak via any other path.
    DiagnosticReporter rep;

    auto first = readCHeaderFromText(
        "extern int x = 5;\n",
        "<call1>", "libc.so.6", rep);
    ASSERT_FALSE(first.has_value());
    ASSERT_TRUE(first.error().at.isPresent());

    auto second = readCHeaderFromText(
        "extern int puts(const char* s);\n",
        "<call2>", "", rep);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().kind, HeaderReadErrorKind::EmptyImportLibrary);
    EXPECT_FALSE(second.error().at.isPresent())
        << "EmptyImportLibrary is an entry-point error; `at` must "
           "stay absent regardless of prior reporter contents";
}

TEST(FfiCHeaderParser, ErrorStructLocationAbsentForEntryPointFailures) {
    // D-FF2-2 negative: entry-point errors (EmptyImportLibrary /
    // FileOpenFailed / GrammarLoadFailed / InvalidShippedPath) have
    // no single decl locus — `at` stays default-constructed
    // (buffer.valid() == false). Programmatic consumers must check
    // before reading; this pins the contract.
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "extern int puts(const char* s);\n",
        "<test>", "", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::EmptyImportLibrary);
    EXPECT_FALSE(rowsOrErr.error().at.isPresent())
        << "EmptyImportLibrary is a caller-API entry-point error; "
           "no decl locus exists, so `at.buffer` must be invalid";
}

// ── Diagnostic code name round-trips ──────────────────────────

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
TEST(FfiCHeaderParser, DiagnosticCodeNameRoundTripFHeaderEmptyImportLibrary) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_HeaderEmptyImportLibrary),
              "F_HeaderEmptyImportLibrary");
}
TEST(FfiCHeaderParser, DiagnosticCodeNameRoundTripFHeaderGrammarLoadFailed) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_HeaderGrammarLoadFailed),
              "F_HeaderGrammarLoadFailed");
}
TEST(FfiCHeaderParser, DiagnosticCodeNameRoundTripFHeaderHasUnsupportedTopLevel) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_HeaderHasUnsupportedTopLevel),
              "F_HeaderHasUnsupportedTopLevel");
}
TEST(FfiCHeaderParser, DiagnosticCodeNameRoundTripFHeaderInternalInvariant) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_HeaderInternalInvariant),
              "F_HeaderInternalInvariant");
}
TEST(FfiCHeaderParser, DiagnosticCodeNameRoundTripFHeaderInvalidShippedPath) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_HeaderInvalidShippedPath),
              "F_HeaderInvalidShippedPath");
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
    EXPECT_EQ(headerReadErrorKindName(HeaderReadErrorKind::EmptyImportLibrary),
              "EmptyImportLibrary");
    EXPECT_EQ(headerReadErrorKindName(HeaderReadErrorKind::GrammarLoadFailed),
              "GrammarLoadFailed");
    EXPECT_EQ(headerReadErrorKindName(HeaderReadErrorKind::HeaderHasUnsupportedTopLevel),
              "HeaderHasUnsupportedTopLevel");
    EXPECT_EQ(headerReadErrorKindName(HeaderReadErrorKind::InternalInvariant),
              "InternalInvariant");
    EXPECT_EQ(headerReadErrorKindName(HeaderReadErrorKind::InvalidShippedPath),
              "InvalidShippedPath");
}

// ── Shipped headers via readCHeaderShipped ───────────────────

TEST(FfiCHeaderParser, ShippedLibcStdioRoundTrips) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderShipped("libc/stdio.h", "libc.so.6", rep);
    if (!rowsOrErr.has_value()
        && rowsOrErr.error().kind == HeaderReadErrorKind::FileOpenFailed) {
        GTEST_SKIP() << "shipped libc/stdio.h not located from cwd; "
                     << "build configs without an upward-reachable repo root "
                     << "can't run this test (not a regression).";
    }
    ASSERT_TRUE(rowsOrErr.has_value()) << headerReadErrorKindName(rowsOrErr.error().kind);
    ASSERT_EQ(rowsOrErr->size(), 2u);
    EXPECT_EQ((*rowsOrErr)[0].mangledName, "puts");
    EXPECT_EQ((*rowsOrErr)[1].mangledName, "putchar");
    for (auto const& row : *rowsOrErr) {
        EXPECT_EQ(row.libraryPath, "libc.so.6");
        EXPECT_EQ(row.kind, SymbolKind::Function);
    }
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(FfiCHeaderParser, ShippedLibcStdlibRoundTrips) {
    // pr-test-analyzer P9 fold: stdlib.h exercises `void*` return,
    // `void*` parameter, and `void` return — distinct shapes from
    // stdio.h. A c-subset grammar regression dropping `void*` for
    // return types would trip ONLY this test.
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderShipped("libc/stdlib.h", "libc.so.6", rep);
    if (!rowsOrErr.has_value()
        && rowsOrErr.error().kind == HeaderReadErrorKind::FileOpenFailed) {
        GTEST_SKIP() << "shipped libc/stdlib.h not located from cwd.";
    }
    ASSERT_TRUE(rowsOrErr.has_value()) << headerReadErrorKindName(rowsOrErr.error().kind);
    ASSERT_EQ(rowsOrErr->size(), 3u);
    EXPECT_EQ((*rowsOrErr)[0].mangledName, "malloc");
    EXPECT_EQ((*rowsOrErr)[1].mangledName, "free");
    EXPECT_EQ((*rowsOrErr)[2].mangledName, "exit");
    for (auto const& row : *rowsOrErr) {
        EXPECT_EQ(row.libraryPath, "libc.so.6");
        EXPECT_EQ(row.kind, SymbolKind::Function);
    }
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(FfiCHeaderParser, ShippedHeaderPathTraversalRejected) {
    // Post-FF2-#2 H2 fold: path-traversal is now a distinct kind
    // (InvalidShippedPath) + code (F_HeaderInvalidShippedPath),
    // separating caller-API bugs from file-not-found.
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderShipped("../etc/passwd", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::InvalidShippedPath);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_HeaderInvalidShippedPath), 1u);
}

TEST(FfiCHeaderParser, ShippedHeaderEmptyPathRejected) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderShipped("", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::InvalidShippedPath);
}

TEST(FfiCHeaderParser, ShippedHeaderLeadingSlashRejected) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderShipped("/etc/passwd", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::InvalidShippedPath);
}

#if defined(_WIN32)
TEST(FfiCHeaderParser, ShippedHeaderWindowsAbsoluteRejected) {
    // CRITICAL-1 fold: Windows absolute paths (drive letter) must
    // be rejected — the pre-fold leading-char check would have
    // accepted `C:\Windows\...` and composed it as an absolute
    // path via `fs::path::operator/`.
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderShipped("C:/Windows/System32/drivers/etc/hosts",
                                        "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::InvalidShippedPath);
}
#endif

TEST(FfiCHeaderParser, ShippedHeaderLeadingDotRejected) {
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderShipped(".config/secrets", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::InvalidShippedPath);
}

TEST(FfiCHeaderParser, ShippedHeaderEmbeddedTraversalRejectedByComponent) {
    // Per-component check: a `..` in the middle of the path is also
    // rejected (the pre-fold substring check matched but so did
    // `foo..bar`; this test pins the cleaner component check).
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderShipped("libc/../etc/passwd", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    EXPECT_EQ(rowsOrErr.error().kind, HeaderReadErrorKind::InvalidShippedPath);
}

TEST(FfiCHeaderParser, NonExternGlobalRejectionCarriesSourceSpan) {
    // Post-FF2-#2 test-analyzer fold: every FF2-layer rejection
    // carries a span — pin the Global arm explicitly, since the
    // Function arm pin alone would miss a regression that drops
    // the &loc pass-through on the Global arm specifically.
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderFromText(
        "int counter;\n",
        "<test>", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    bool foundWithSpan = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_HeaderHasNonExternDecl) {
            EXPECT_TRUE(d.buffer.valid());
            EXPECT_GT(d.span.length(), 0u);
            foundWithSpan = true;
            break;
        }
    }
    EXPECT_TRUE(foundWithSpan);
}

TEST(FfiCHeaderParser, ShippedPathRejectionInlinesCauseInWrapMessage) {
    // Post-FF2-#2 silent-failure C2 fold: the wrap diagnostic must
    // include the underlying cause text so that even with
    // `--suppress=C_*` the operator sees what went wrong.
    DiagnosticReporter rep;
    auto rowsOrErr = readCHeaderShipped("libc/../etc/passwd", "libc.so.6", rep);
    ASSERT_FALSE(rowsOrErr.has_value());
    // Cause was "shipped-ffi-header path must not contain a '..' component".
    EXPECT_NE(rowsOrErr.error().detail.find("'..'"), std::string::npos)
        << "wrap must inline the underlying ConfigDiagnostic cause; got: "
        << rowsOrErr.error().detail;
}
