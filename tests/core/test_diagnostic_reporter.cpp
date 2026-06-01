#include "core/types/diagnostic_reporter.hpp"
#include "core/types/source_buffer.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace dss;

namespace {

// Build a minimal diagnostic for a given (code, buffer, span). Keeps the
// tests focused on reporter semantics, not field assembly.
ParseDiagnostic makeDiag(DiagnosticCode code,
                        DiagnosticSeverity sev,
                        BufferId buf,
                        ByteOffset start = 0,
                        ByteOffset end   = 0,
                        std::string actual = "x") {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = sev;
    d.buffer   = buf;
    d.span     = SourceSpan::of(start, end);
    d.actual   = std::move(actual);
    return d;
}

} // namespace

TEST(BufferRegistry, AddAndGet) {
    BufferRegistry r;
    auto buf = SourceBuffer::fromString("hello", "<unit>");
    auto id = r.add(buf);
    EXPECT_EQ(id, buf->id());
    EXPECT_EQ(&r.get(id), buf.get());
    EXPECT_NE(r.tryGet(id), nullptr);
}

TEST(BufferRegistry, TryGetReturnsNullForMissing) {
    BufferRegistry r;
    EXPECT_EQ(r.tryGet(BufferId{9999}), nullptr);
}

TEST(BufferRegistry, AddNullThrows) {
    BufferRegistry r;
    EXPECT_THROW(r.add(nullptr), std::invalid_argument);
}

TEST(Reporter, EmptyByDefault) {
    DiagnosticReporter r;
    EXPECT_FALSE(r.hasErrors());
    EXPECT_FALSE(r.hitCap());
    EXPECT_EQ(r.errorCount(), 0u);
    EXPECT_EQ(r.warningCount(), 0u);
    EXPECT_TRUE(r.all().empty());
}

TEST(Reporter, AppendsAndCountsBySeverity) {
    DiagnosticReporter r;
    BufferId b{1};
    r.report(makeDiag(DiagnosticCode::P_UnexpectedToken,    DiagnosticSeverity::Error,   b, 0,  1));
    r.report(makeDiag(DiagnosticCode::P_DeprecatedSyntax,   DiagnosticSeverity::Warning, b, 2,  3));
    r.report(makeDiag(DiagnosticCode::P_InvalidEscapeSequence, DiagnosticSeverity::Info, b, 4,  5));
    EXPECT_EQ(r.errorCount(),   1u);
    EXPECT_EQ(r.warningCount(), 1u);
    EXPECT_TRUE(r.hasErrors());
    EXPECT_EQ(r.all().size(), 3u);
}

TEST(Reporter, DedupesIdenticalInWindow) {
    DiagnosticReporter r;
    BufferId b{1};
    // Three identical diagnostics — should collapse to one.
    for (int i = 0; i < 3; ++i) {
        r.report(makeDiag(DiagnosticCode::P_UnexpectedToken, DiagnosticSeverity::Error, b, 5, 6));
    }
    EXPECT_EQ(r.all().size(), 1u);
}

TEST(Reporter, DistinctSpansAreNotDeduped) {
    DiagnosticReporter r;
    BufferId b{1};
    r.report(makeDiag(DiagnosticCode::P_UnexpectedToken, DiagnosticSeverity::Error, b, 5, 6));
    r.report(makeDiag(DiagnosticCode::P_UnexpectedToken, DiagnosticSeverity::Error, b, 7, 8));
    EXPECT_EQ(r.all().size(), 2u);
}

TEST(Reporter, RuleContextIsPartOfDedupKey) {
    // Regression for the dedup fix that enabled per-frame EOF
    // diagnostics in the builder: identical (code, buffer, span) with
    // DIFFERENT ruleContexts must NOT collapse.
    DiagnosticReporter r;
    BufferId b{1};
    auto d1 = makeDiag(DiagnosticCode::P_PrematureEndOfInput, DiagnosticSeverity::Error, b, 10, 10);
    d1.ruleContext = RuleId{1};
    auto d2 = makeDiag(DiagnosticCode::P_PrematureEndOfInput, DiagnosticSeverity::Error, b, 10, 10);
    d2.ruleContext = RuleId{2};
    auto d3 = makeDiag(DiagnosticCode::P_PrematureEndOfInput, DiagnosticSeverity::Error, b, 10, 10);
    d3.ruleContext = RuleId{1};   // same code+span+rule as d1 → must dedup
    r.report(d1);
    r.report(d2);
    r.report(d3);
    EXPECT_EQ(r.all().size(), 2u);
}

TEST(Reporter, ActualIsPartOfDedupKey) {
    // Diagnostics sharing (code, buffer, span, rule) but carrying DIFFERENT
    // `actual` detail convey distinct information (e.g. two different missing
    // files, or two different HIR nodes with no source span) and must NOT be
    // collapsed. True duplicates (identical `actual`) still dedup — see
    // DedupesIdenticalInWindow above.
    DiagnosticReporter r;
    BufferId b{1};
    r.report(makeDiag(DiagnosticCode::D_FileNotFound, DiagnosticSeverity::Error, b, 0, 0, "a.h"));
    r.report(makeDiag(DiagnosticCode::D_FileNotFound, DiagnosticSeverity::Error, b, 0, 0, "b.h"));
    EXPECT_EQ(r.all().size(), 2u);
}

TEST(Reporter, PerCodeCapCoalescesSilently) {
    DiagnosticReporter::Config cfg;
    cfg.maxPerCode = 3;
    DiagnosticReporter r{cfg};
    BufferId b{1};
    // 10 distinct spans, same code — only first 3 should land.
    for (uint32_t i = 0; i < 10; ++i) {
        r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error,
                          b, i * 10, i * 10 + 1));
    }
    EXPECT_EQ(r.all().size(), 3u);
    EXPECT_EQ(r.errorCount(), 3u);
}

TEST(Reporter, ContextPrefixDoesNotPolluteDedupKey) {
    // D-MERGE-DEDUP-PREFIX-COLLISION fold (2026-06-01): the
    // `contextPrefix` field on ParseDiagnostic is rendering-only and
    // MUST NOT enter the dedup hash. Two diagnostics with identical
    // (code, buffer, span, ruleContext, actual) but DIFFERENT
    // contextPrefix values must collapse via dedup.
    //
    // Pre-fix `mergeWithTargetContext` mutated `actual` directly, so
    // multi-target runs leaked structurally-identical duplicates
    // through to the merged reporter (different prefix → different
    // hash → no dedup). The contextPrefix field moves the rendering
    // string out of the hash input.
    DiagnosticReporter::Config cfg;
    cfg.dedupWindow = 4;
    DiagnosticReporter r{cfg};
    BufferId b{1};

    ParseDiagnostic d1;
    d1.code     = DiagnosticCode::P_UnexpectedToken;
    d1.severity = DiagnosticSeverity::Error;
    d1.buffer   = b;
    d1.span     = SourceSpan::of(0, 5);
    d1.actual   = "same";
    d1.contextPrefix = "[target=spec-A] ";

    ParseDiagnostic d2 = d1;
    d2.contextPrefix = "[target=spec-B] ";  // different prefix only

    r.report(std::move(d1));
    r.report(std::move(d2));

    EXPECT_EQ(r.all().size(), 1u)
        << "structurally-identical diagnostics with different "
           "contextPrefix must collapse via dedup; pre-fix this leaked "
           "to size==2 because the prefix was in `actual`";
}

TEST(Reporter, GlobalCapEmitsMarkerAndStops) {
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 5;
    cfg.maxPerCode     = 100;     // don't let per-code cap interfere
    cfg.dedupWindow    = 0;       // and don't let dedup interfere
    DiagnosticReporter r{cfg};
    BufferId b{1};

    // Push 10; first 5 are regular, 6th becomes the cap marker, 7-10 dropped.
    for (uint32_t i = 0; i < 10; ++i) {
        r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error,
                          b, i * 10, i * 10 + 1));
    }
    EXPECT_EQ(r.all().size(), 6u);                    // 5 + 1 marker
    EXPECT_TRUE(r.hitCap());
    EXPECT_EQ(r.all().back().code, DiagnosticCode::P_TooManyDiagnostics);
}

TEST(Reporter, PolicySuppressDropsSilently) {
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::P_DeprecatedSyntax);
    DiagnosticReporter r{cfg};
    BufferId b{1};
    r.report(makeDiag(DiagnosticCode::P_DeprecatedSyntax, DiagnosticSeverity::Warning, b));
    r.report(makeDiag(DiagnosticCode::P_UnexpectedToken,  DiagnosticSeverity::Error,   b, 1, 2));
    EXPECT_EQ(r.all().size(), 1u);
    EXPECT_EQ(r.all()[0].code, DiagnosticCode::P_UnexpectedToken);
}

TEST(Reporter, PolicyOverrideRemapsSeverity) {
    DiagnosticReporter::Config cfg;
    cfg.policy.overrides[DiagnosticCode::P_AmbiguousToken] = DiagnosticSeverity::Error;
    DiagnosticReporter r{cfg};
    BufferId b{1};
    r.report(makeDiag(DiagnosticCode::P_AmbiguousToken, DiagnosticSeverity::Warning, b));
    EXPECT_EQ(r.all().size(), 1u);
    EXPECT_EQ(r.all()[0].severity, DiagnosticSeverity::Error);
    EXPECT_EQ(r.errorCount(), 1u);
}

TEST(Reporter, PolicyWarningsAsErrorsPromotes) {
    DiagnosticReporter::Config cfg;
    cfg.policy.warningsAsErrors = true;
    DiagnosticReporter r{cfg};
    BufferId b{1};
    r.report(makeDiag(DiagnosticCode::P_DeprecatedSyntax, DiagnosticSeverity::Warning, b));
    EXPECT_EQ(r.errorCount(), 1u);
    EXPECT_EQ(r.warningCount(), 0u);
}

TEST(Reporter, ExplicitOverrideBeatsWarningsAsErrors) {
    // Override demotes Warning→Info; warningsAsErrors only promotes Warnings,
    // so the explicit demotion wins.
    DiagnosticReporter::Config cfg;
    cfg.policy.overrides[DiagnosticCode::P_DeprecatedSyntax] = DiagnosticSeverity::Info;
    cfg.policy.warningsAsErrors = true;
    DiagnosticReporter r{cfg};
    BufferId b{1};
    r.report(makeDiag(DiagnosticCode::P_DeprecatedSyntax, DiagnosticSeverity::Warning, b));
    EXPECT_EQ(r.all()[0].severity, DiagnosticSeverity::Info);
    EXPECT_EQ(r.errorCount(), 0u);
}

TEST(ReporterFormat, IncludesPrefixSeverityAndCaret) {
    DiagnosticReporter r;
    BufferRegistry bufs;
    auto buf = SourceBuffer::fromString("var x = 1 + 2 }\n", "<inline>");
    bufs.add(buf);

    auto d = makeDiag(DiagnosticCode::P_UnexpectedToken,
                      DiagnosticSeverity::Error,
                      buf->id(),
                      14, 15, "'}'");
    d.expected = {"';'", "','"};
    d.suggestion = "insert ';' before this token";
    d.scopeStack = {ScopeKind::Root, ScopeKind::Block};
    r.report(d);

    auto out = r.format(r.all()[0], bufs);
    // Spot-checks — exact pretty-print is intentionally not byte-locked
    // so we can polish the renderer without flaking tests.
    EXPECT_NE(out.find("error[P0001]"), std::string::npos);
    EXPECT_NE(out.find("expected ';' or ','"), std::string::npos);
    EXPECT_NE(out.find("got '}'"), std::string::npos);
    EXPECT_NE(out.find("var x = 1 + 2 }"), std::string::npos);
    EXPECT_NE(out.find("^"), std::string::npos);
    EXPECT_NE(out.find("scope: Root > Block"), std::string::npos);
    EXPECT_NE(out.find("hint:  insert ';' before this token"), std::string::npos);
}

// Multi-byte span underlines with `^^^…` matching the span length, so
// a regression that drops the loop and prints a single `^` would not
// silently pass.
TEST(ReporterFormat, MultiCharSpanProducesMultiCaretUnderline) {
    DiagnosticReporter r;
    BufferRegistry bufs;
    auto buf = SourceBuffer::fromString("let foo = 1;\n", "<inline>");
    bufs.add(buf);

    auto d = makeDiag(DiagnosticCode::P_UnexpectedToken,
                      DiagnosticSeverity::Error,
                      buf->id(),
                      4, 7, "'foo'");
    r.report(d);
    auto out = r.format(r.all()[0], bufs);
    EXPECT_NE(out.find("^^^"), std::string::npos)
        << "3-byte span must render `^^^` not a lone `^`";
}

// Empty-span (start == end) renders a single caret rather than zero.
TEST(ReporterFormat, EmptySpanRendersSingleCaret) {
    DiagnosticReporter r;
    BufferRegistry bufs;
    auto buf = SourceBuffer::fromString("x\n", "<inline>");
    bufs.add(buf);

    auto d = makeDiag(DiagnosticCode::P_MissingRequiredChild,
                      DiagnosticSeverity::Error,
                      buf->id(),
                      1, 1, "'<eof>'");
    r.report(d);
    auto out = r.format(r.all()[0], bufs);
    EXPECT_NE(out.find("^"), std::string::npos);
    EXPECT_EQ(out.find("^^"), std::string::npos)
        << "zero-width span must render exactly one `^`";
}

TEST(ReporterFormat, FormatsRelatedLocations) {
    DiagnosticReporter r;
    BufferRegistry bufs;
    auto buf = SourceBuffer::fromString("var x = (\n  1 + 2\n}\n", "<inline>");
    bufs.add(buf);

    auto d = makeDiag(DiagnosticCode::P_UnmatchedClose, DiagnosticSeverity::Error,
                      buf->id(), 18, 19, "'}'");
    d.related.push_back({.buffer = buf->id(),
                         .span   = SourceSpan::of(8, 9),
                         .note   = "matching opener at line 1"});
    r.report(d);
    auto out = r.format(r.all()[0], bufs);
    EXPECT_NE(out.find("note: matching opener at line 1"), std::string::npos);
}

TEST(ReporterFormat, UnknownBufferDoesNotCrash) {
    // formatAll() with a reporter that holds a diag referring to a buffer
    // not in the registry must produce *some* output, not abort.
    DiagnosticReporter r;
    BufferRegistry bufs;     // intentionally empty
    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error,
                      BufferId{42}, 0, 1));
    auto out = r.formatAll(bufs);
    EXPECT_NE(out.find("error[P0003]"), std::string::npos);
    EXPECT_NE(out.find("<unknown-buffer:42>"), std::string::npos);
}

TEST(ReporterFormat, FormatAllSortsBySourceOrder) {
    DiagnosticReporter r;
    BufferRegistry bufs;
    auto buf = SourceBuffer::fromString("abcdefghij", "<inline>");
    bufs.add(buf);
    // Report out of order; the formatted output should be sorted by span.
    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, buf->id(), 7, 8, "'@7'"));
    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, buf->id(), 1, 2, "'@1'"));
    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, buf->id(), 4, 5, "'@4'"));
    auto out = r.formatAll(bufs);
    const auto p1 = out.find("'@1'");
    const auto p4 = out.find("'@4'");
    const auto p7 = out.find("'@7'");
    ASSERT_NE(p1, std::string::npos);
    ASSERT_NE(p4, std::string::npos);
    ASSERT_NE(p7, std::string::npos);
    EXPECT_LT(p1, p4);
    EXPECT_LT(p4, p7);
}
