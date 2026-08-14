#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/unsuppressable_codes.hpp"
// D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN: the caret-width pin below drives a REAL
// literal through the tokenizer + parser rather than a hand-built span. See the
// comment on `CaretWidthOverRealStringLiteralCoversTheClosingQuote`.
#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/tree.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
    // 5 of the 10 never landed: the 6th (which the marker stands in for)
    // plus reports 7-10. Exact, not a floor — an off-by-one in which
    // diagnostic the marker replaces is precisely what this catches.
    EXPECT_EQ(r.droppedByCap(), 5u);
}

// ─────────────────────────────────────────────────────────────────────────────
// THE CAP MUST NOT BE ABLE TO TRUNCATE IN SILENCE.
//
// ★ WHAT "SILENT" MEANT HERE, because the marker already existed and the
// defect survived it anyway. The old notice read, in full:
//
//     reporter cap of 1000 diagnostics reached; further reports dropped
//
// It announces the elision and names the limit, and it is still useless for
// the decision the operator has to make, because it omits the only two facts
// that decide it: HOW MANY diagnostics went missing (one? ten thousand?) and
// WHAT TO CHANGE to see them. Worse, the count is the one number nobody can
// recover afterwards — a dropped diagnostic leaves nothing behind to count,
// so if the reporter does not tally at the moment it discards, the
// information is gone for good. `hitCap_` short-circuited every later report
// without tallying anything.
//
// RED-ON-DISABLE: restore the old one-line `marker.actual` format string and
// drop the `noteCapDrop_()` calls — the count assertions below fail, and the
// remedy-substring assertions fail, on the pin's own matchers.
TEST(Reporter, CapNoticeStatesHowManyWereDroppedAndHowToRaiseTheLimit) {
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 2;
    cfg.maxPerCode     = 100;
    cfg.dedupWindow    = 0;
    DiagnosticReporter r{cfg};
    BufferId b{1};

    // 2 land, the 3rd is replaced by the marker, 4th-10th are eaten.
    for (uint32_t i = 0; i < 10; ++i) {
        r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error,
                          b, i * 10, i * 10 + 1));
    }

    ASSERT_EQ(r.all().size(), 3u);
    ASSERT_EQ(r.all().back().code, DiagnosticCode::P_TooManyDiagnostics);
    EXPECT_EQ(r.droppedByCap(), 8u);

    // ★ THE PIN: the notice's own text, which is what an operator reads.
    // Full equality, not a substring probe — the count, the limit and the
    // remedy are all load-bearing and a partial match would let any one of
    // them regress unnoticed.
    EXPECT_EQ(r.all().back().actual,
              "reporter cap of 2 diagnostics reached; 8 further diagnostics "
              "were dropped and NOT shown. Raise the cap above 2 to see them: "
              "pass --max-diagnostics=N on the command line, or set "
              "DiagnosticReporter::Config::maxDiagnostics when embedding.")
        << "the elision must state its size and its remedy; \"further "
           "reports dropped\" states neither";
}

// ─────────────────────────────────────────────────────────────────────────────
// THE REMEDY MUST BE ONE THE READER CAN ACT ON.
//
// The pin above is byte-exact and therefore already covers this — but it
// covers it INCIDENTALLY, as one of forty-odd characters in a sentence, so a
// future rewording that drops the flag while adjusting the same literal in
// this file would move both together and stay green. This test asserts the
// PROPERTY separately: whoever the reader is, the notice hands them something
// they can type or set.
//
// Both clauses, because the marker has two audiences and only one of them has
// a command line: an OPERATOR (the CLI's `--max-diagnostics`, by far the most
// likely reader) and an EMBEDDER or the LSP (the C++ config field, true for
// every consumer). Naming only the field was the pre-existing defect — it is a
// remedy no CLI user can reach; naming only the flag would be the mirror
// defect for the LSP.
//
// RED-ON-DISABLE: drop either clause from `noteCapDrop_`'s format string.
TEST(Reporter, CapNoticeRemedyNamesBothTheCliFlagAndTheConfigField) {
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 1;
    cfg.maxPerCode     = 100;
    cfg.dedupWindow    = 0;
    DiagnosticReporter r{cfg};
    BufferId b{1};

    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, b, 0, 1));
    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, b, 10, 11));
    ASSERT_TRUE(r.hitCap());

    auto const& text = r.all().back().actual;
    EXPECT_NE(text.find("--max-diagnostics"), std::string::npos)
        << "the remedy must name the CLI flag: an operator at a terminal "
           "cannot set a C++ config field, and they are the likeliest reader";
    EXPECT_NE(text.find("DiagnosticReporter::Config::maxDiagnostics"),
              std::string::npos)
        << "and it must still name the field: the LSP and embedders have no "
           "command line, and this string is core-tier";

    // PURE ASCII is a hard constraint, not a preference: no `/utf-8` reaches
    // MSVC anywhere in this build, so a non-ASCII byte in this literal is a
    // cross-toolchain encoding flake. Asserted on the produced string rather
    // than trusted of the source file.
    for (unsigned char const ch : text) {
        ASSERT_LT(static_cast<unsigned>(ch), 0x80u)
            << "cap-notice text must be pure ASCII; got byte value "
            << static_cast<unsigned>(ch);
    }
}

// The count is a RUNNING total, not the value it happened to have when the
// cap tripped. Pinned separately because the failure mode is specific and
// invisible to the test above: a rewrite that formats the marker ONCE (at
// trip time) reports "1 further diagnostic was dropped" no matter how many
// thousands follow, and every other assertion still passes.
TEST(Reporter, CapNoticeCountTracksLaterDropsNotJustTheTriggeringOne) {
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 1;
    cfg.maxPerCode     = 100;
    cfg.dedupWindow    = 0;
    DiagnosticReporter r{cfg};
    BufferId b{1};

    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, b, 0, 1));
    ASSERT_FALSE(r.hitCap());

    // The report that trips the cap. Singular prose is part of the contract:
    // "1 further diagnostics were dropped" is the tell of a count spliced
    // into a fixed sentence.
    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, b, 10, 11));
    ASSERT_TRUE(r.hitCap());
    EXPECT_EQ(r.droppedByCap(), 1u);
    EXPECT_EQ(r.all().back().actual,
              "reporter cap of 1 diagnostics reached; 1 further diagnostic "
              "was dropped and NOT shown. Raise the cap above 1 to see it: "
              "pass --max-diagnostics=N on the command line, or set "
              "DiagnosticReporter::Config::maxDiagnostics when embedding.");

    for (uint32_t i = 2; i < 42; ++i) {
        r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error,
                          b, i * 10, i * 10 + 1));
    }
    EXPECT_EQ(r.droppedByCap(), 41u);
    EXPECT_NE(r.all().back().actual.find("41 further diagnostics were dropped"),
              std::string::npos)
        << "the marker's count must still be accurate after the drops that "
           "followed it; a format-once implementation is frozen at 1";
    // And exactly one marker, however many reports the cap ate.
    std::size_t markers = 0;
    for (auto const& d : r.all()) {
        if (d.code == DiagnosticCode::P_TooManyDiagnostics) ++markers;
    }
    EXPECT_EQ(markers, 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// A CAP OF ZERO IS A DEFINED VALUE, AND THIS IS THE DEFINITION.
//
// `--max-diagnostics=0` is accepted by the CLI, and that acceptance is an
// argument about what `report()` DOES rather than a matter of taste — so the
// behaviour it rests on is pinned here, at the tier that implements it, and not
// left as prose in the parser's docblock. The gate is
// `all_.size() >= cfg_.maxDiagnostics` on unsigned operands: at 0 it is
// satisfied before anything is stored, so the FIRST cap-eligible report trips
// the cap, is counted, and is replaced by the marker. No crash, and in
// particular no `capMarkerFatal` — `noteCapDrop_` pushes the marker BEFORE it
// counts, so its `capMarkerIndex_ >= all_.size()` guard holds at index 0.
//
// The value is therefore coherent and otherwise unobtainable: "show me none,
// just tell me how many there were". And it is not a hole in the guarantees —
// `DiagnosticDelivery::Guaranteed` bypasses the cap at 0 exactly as at every
// other value, which is the half that makes 0 safe to expose at all.
//
// RED-ON-DISABLE: change the gate to `>` (or add a `maxDiagnostics == 0` early
// return) and the size / count / bypass assertions below fail.
TEST(Reporter, CapOfZeroStoresOnlyTheMarkerAndStillDeliversGuaranteed) {
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 0;
    cfg.maxPerCode     = 100;
    cfg.dedupWindow    = 0;
    DiagnosticReporter r{cfg};
    BufferId b{1};

    for (uint32_t i = 0; i < 5; ++i) {
        r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error,
                          b, i * 10, i * 10 + 1));
    }

    // EXACTLY one entry, and it is the marker. Not zero — the elision must
    // still announce itself, or a cap of 0 would be the silent truncation the
    // whole mechanism exists to forbid.
    ASSERT_EQ(r.all().size(), 1u);
    EXPECT_EQ(r.all()[0].code, DiagnosticCode::P_TooManyDiagnostics);
    EXPECT_TRUE(r.hitCap());
    EXPECT_EQ(r.droppedByCap(), 5u);
    EXPECT_EQ(r.all()[0].actual,
              "reporter cap of 0 diagnostics reached; 5 further diagnostics "
              "were dropped and NOT shown. Raise the cap above 0 to see them: "
              "pass --max-diagnostics=N on the command line, or set "
              "DiagnosticReporter::Config::maxDiagnostics when embedding.");

    // ★ The half that makes 0 safe: a cap of 0 still cannot swallow a
    // diagnostic whose delivery is guaranteed.
    auto guaranteed = makeDiag(DiagnosticCode::P_DeprecatedSyntax,
                               DiagnosticSeverity::Info, b, 999, 1000,
                               "guaranteed past a zero cap");
    guaranteed.delivery = DiagnosticDelivery::Guaranteed;
    r.report(std::move(guaranteed));

    ASSERT_EQ(r.all().size(), 2u);
    EXPECT_EQ(r.all()[1].actual, "guaranteed past a zero cap");
    EXPECT_EQ(r.droppedByCap(), 5u)
        << "a guaranteed diagnostic bypasses the cap; it must not be counted "
           "as dropped";
}

// ─────────────────────────────────────────────────────────────────────────────
// AN ERROR EATEN BY THE CAP MUST NOT PRODUCE AN EMPTY EXPLANATION.
//
// This is the shape the whole cycle exists to forbid: a build that exits
// non-zero while stderr says nothing about why. The driver
// (`program.cpp::drainDiagnosticsToStderr`) renders a buffer-LESS diagnostic
// as `<severity>[<code>] <actual>` and a buffer-bearing one through
// `format()`; this pins BOTH renderings directly, so a regression is caught
// at the boundary the operator actually reads rather than at an internal
// count.
TEST(Reporter, ErrorPastTheCapStillProducesNonEmptyExplanatoryOutput) {
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 2;
    cfg.maxPerCode     = 100;
    cfg.dedupWindow    = 0;
    DiagnosticReporter r{cfg};

    BufferRegistry bufs;
    auto buf = SourceBuffer::fromString("int x = 1;\n", "<inline>");
    bufs.add(buf);

    r.report(makeDiag(DiagnosticCode::P_DeprecatedSyntax, DiagnosticSeverity::Warning,
                      buf->id(), 0, 1));
    r.report(makeDiag(DiagnosticCode::P_DeprecatedSyntax, DiagnosticSeverity::Warning,
                      buf->id(), 2, 3));
    ASSERT_FALSE(r.hitCap());

    // The Error arrives after the budget is spent and is itself dropped.
    r.report(makeDiag(DiagnosticCode::D_FileNotFound, DiagnosticSeverity::Error,
                      buf->id(), 4, 5, "'missing.h'"));
    ASSERT_TRUE(r.hitCap());

    // Honest about what happened: the error's own text is gone.
    for (auto const& d : r.all()) {
        EXPECT_NE(d.code, DiagnosticCode::D_FileNotFound);
    }

    // ★ But the build still FAILS — the marker is Error-severity, so the
    // exit-code gate cannot read green on a run that threw an error away.
    EXPECT_TRUE(r.hasErrors());
    EXPECT_EQ(r.errorCount(), 1u);

    // ★ And stderr is not empty, and not uninformative. Route 1: the
    // positioned renderer.
    auto const positioned = r.formatAll(bufs);
    EXPECT_FALSE(positioned.empty());
    EXPECT_NE(positioned.find("1 further diagnostic was dropped"),
              std::string::npos)
        << "the rendered stream must say that something was dropped AND how "
           "much; this is the empty-stderr shape the cycle closes";
    EXPECT_NE(positioned.find("DiagnosticReporter::Config::maxDiagnostics"),
              std::string::npos)
        << "...and what to change to see it";

    // Route 2: the driver's buffer-less one-liner shape, reproduced exactly
    // as `drainDiagnosticsToStderr` composes it.
    auto const& marker = r.all().back();
    ASSERT_EQ(marker.code, DiagnosticCode::P_TooManyDiagnostics);
    std::string const oneLiner = std::string{severityName(marker.severity)} + "["
                               + std::string{diagnosticCodeName(marker.code)} + "] "
                               + marker.contextPrefix + marker.actual;
    EXPECT_FALSE(oneLiner.empty());
    EXPECT_NE(oneLiner.find("1 further diagnostic was dropped"), std::string::npos);
    EXPECT_NE(oneLiner.find("DiagnosticReporter::Config::maxDiagnostics"),
              std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// DELIVERY IS ITS OWN PROPERTY, AND IT IS NOT SUPPRESSION.
//
// `DiagnosticDelivery::Guaranteed` exists so a diagnostic that must survive
// the reporter's volume controls does not have to join `kUnsuppressableCodes`
// to get there — membership answers whether the USER may silence a code, and
// borrowing it for cap-immunity loosens that criterion every time. These pins
// hold the two axes apart in both directions.
TEST(Reporter, GuaranteedDeliverySurvivesACapExceedingRun) {
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 2;
    cfg.maxPerCode     = 100;
    cfg.dedupWindow    = 0;
    DiagnosticReporter r{cfg};
    BufferId b{1};

    for (uint32_t i = 0; i < 20; ++i) {
        r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error,
                          b, i * 10, i * 10 + 1));
    }
    ASSERT_TRUE(r.hitCap());

    // A SUPPRESSABLE code — deliberately not a closed-table member — asking
    // for delivery on its own merits. Info severity, so it is also not
    // riding any "errors are special" shortcut.
    auto d = makeDiag(DiagnosticCode::P_DeprecatedSyntax, DiagnosticSeverity::Info,
                      b, 999, 1000, "the build reused a stale checkout");
    d.delivery = DiagnosticDelivery::Guaranteed;
    r.report(std::move(d));

    ASSERT_FALSE(isUnsuppressable(DiagnosticCode::P_DeprecatedSyntax))
        << "test setup: this must NOT be a closed-table member, or the pin "
           "proves membership rather than the delivery property";

    // ★ THE MESSAGE, not merely the code: a diagnostic reduced to a code
    // with no prose is not delivery, it is a tally.
    bool found = false;
    for (auto const& kept : r.all()) {
        if (kept.code == DiagnosticCode::P_DeprecatedSyntax) {
            found = true;
            EXPECT_EQ(kept.actual, "the build reused a stale checkout");
            EXPECT_EQ(kept.severity, DiagnosticSeverity::Info);
        }
    }
    EXPECT_TRUE(found)
        << "a Guaranteed-delivery diagnostic must reach `all_` past the "
           "global cap without being a member of kUnsuppressableCodes";
    // ...and it is renderable, i.e. it reaches the operator's terminal.
    BufferRegistry bufs;
    EXPECT_NE(r.formatAll(bufs).find("the build reused a stale checkout"),
              std::string::npos);
}

TEST(Reporter, GuaranteedDeliveryDoesNotDefeatSuppress) {
    // The other direction, and the one that keeps the two axes honest: a
    // delivery guarantee must NOT smuggle a code past the user's explicit
    // `--suppress`. If it did, the new property would be a second, weaker
    // unsuppressable table — exactly what it exists to avoid.
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::P_DeprecatedSyntax);
    DiagnosticReporter r{cfg};

    auto d = makeDiag(DiagnosticCode::P_DeprecatedSyntax, DiagnosticSeverity::Warning,
                      BufferId{1});
    d.delivery = DiagnosticDelivery::Guaranteed;
    r.report(std::move(d));

    EXPECT_EQ(r.all().size(), 0u)
        << "delivery answers the CAP, not the user's suppress policy; a "
           "Guaranteed diagnostic the user named must still be silenced";
}

TEST(Reporter, DefaultDeliveryIsCappedSoGuaranteesAreAskedForNotInherited) {
    // A delivery guarantee is a commitment; acquiring one by omission would
    // make the property meaningless within a release or two. Pinned as a
    // default-value invariant so a "helpful" flip of the default reds here.
    ParseDiagnostic const fresh{};
    EXPECT_EQ(fresh.delivery, DiagnosticDelivery::Capped);

    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 1;
    cfg.dedupWindow    = 0;
    DiagnosticReporter r{cfg};
    BufferId b{1};
    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, b, 0, 1));
    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, b, 5, 6));
    ASSERT_TRUE(r.hitCap());
    r.report(makeDiag(DiagnosticCode::P_AmbiguousToken, DiagnosticSeverity::Error, b, 9, 10));
    for (auto const& d : r.all()) {
        EXPECT_NE(d.code, DiagnosticCode::P_AmbiguousToken)
            << "an ordinary diagnostic must still be capped by default";
    }
}

TEST(Reporter, ForceReportIsTheDeliveryPropertyUnderAnotherName) {
    // `forceReport` used to be a SECOND body that applied policy and pushed
    // on its own — the same rule ("policy applies, volume controls do not")
    // written down twice, free to drift, with only one of the two on any
    // given caller's path. It is now `delivery = Guaranteed; report(...)`.
    // This pin holds the two spellings observably equivalent so a future
    // edit to one is an edit to both.
    auto run = [](bool viaForce) {
        DiagnosticReporter::Config cfg;
        cfg.maxDiagnostics = 1;
        cfg.dedupWindow    = 0;
        DiagnosticReporter r{cfg};
        BufferId b{1};
        r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, b, 0, 1));
        r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, b, 5, 6));
        auto d = makeDiag(DiagnosticCode::P_BuilderInvariant, DiagnosticSeverity::Warning,
                          b, 20, 21, "uncommitted checkpoint");
        if (viaForce) {
            r.forceReport(std::move(d));
        } else {
            d.delivery = DiagnosticDelivery::Guaranteed;
            r.report(std::move(d));
        }
        std::vector<std::pair<DiagnosticCode, std::string>> out;
        for (auto const& kept : r.all()) out.emplace_back(kept.code, kept.actual);
        return out;
    };
    auto const viaForce    = run(true);
    auto const viaProperty = run(false);

    // ★ THE ABSOLUTE ASSERTION COMES FIRST, AND IT IS THE POINT.
    // MEASURED during red-on-disable: with only the equality check below,
    // deleting the delivery arm from `mustDeliver` left this test GREEN —
    // both spellings then dropped the diagnostic, and comparing two broken
    // runs to each other proves nothing. An equivalence pin needs a
    // behavioural anchor or it degrades into a tautology the moment the
    // shared implementation regresses.
    for (auto const& stream : {viaForce, viaProperty}) {
        bool landed = false;
        for (auto const& [code, actual] : stream) {
            if (code == DiagnosticCode::P_BuilderInvariant) {
                landed = true;
                EXPECT_EQ(actual, "uncommitted checkpoint");
            }
        }
        EXPECT_TRUE(landed)
            << "both spellings must actually DELIVER past the global cap";
    }
    EXPECT_EQ(viaForce, viaProperty)
        << "forceReport must be exactly `delivery = Guaranteed` — same "
           "landed stream, same prose, same order";
}

TEST(Reporter, RollbackRestoresTheCapAccountingWithTheStream) {
    // `capMarkerIndex_` is an index INTO `all_`, and `truncateTo` shrinks
    // `all_`. Restoring `hitCap_` without it would leave the index pointing
    // past the end, and the next cap drop would write out of bounds — a
    // memory fault produced by the machinery that exists to explain faults.
    // Pinned because the failure is silent until it is catastrophic.
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 2;
    cfg.dedupWindow    = 0;
    DiagnosticReporter r{cfg};
    BufferId b{1};

    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, b, 0, 1));
    auto const snap = r.snapshotForRollback();
    ASSERT_FALSE(r.hitCap());
    ASSERT_EQ(r.droppedByCap(), 0u);

    // Speculate past the cap, then roll the whole thing back.
    for (uint32_t i = 1; i < 8; ++i) {
        r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error,
                          b, i * 10, i * 10 + 1));
    }
    ASSERT_TRUE(r.hitCap());
    ASSERT_GT(r.droppedByCap(), 0u);

    r.truncateTo(snap);
    EXPECT_EQ(r.all().size(), 1u);
    EXPECT_FALSE(r.hitCap());
    EXPECT_EQ(r.droppedByCap(), 0u)
        << "the drop tally is part of the cap's state and must roll back "
           "with it, or a committed parse inherits a speculative count";

    // Re-drive past the cap on the restored reporter: the marker must land
    // at the CURRENT end of the stream and carry the CURRENT count. Of the
    // four reports below, one fills the restored budget (1 of 2), the second
    // trips the cap, and the last two are eaten -> 3.
    for (uint32_t i = 1; i < 5; ++i) {
        r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error,
                          b, i * 100, i * 100 + 1));
    }
    ASSERT_TRUE(r.hitCap());
    ASSERT_EQ(r.all().size(), 3u);
    ASSERT_EQ(r.all().back().code, DiagnosticCode::P_TooManyDiagnostics);
    EXPECT_EQ(r.droppedByCap(), 3u)
        << "the count must restart from the restored state, not resume the "
           "speculative tally";
    EXPECT_NE(r.all().back().actual.find("3 further diagnostics were dropped"),
              std::string::npos);
}

TEST(Reporter, CapNoticeItselfCannotBeSuppressed) {
    // The notice that diagnostics were hidden must not itself be hideable,
    // or the cap goes silent again through the front door. Today this holds
    // STRUCTURALLY — the marker is pushed straight to `all_` rather than
    // re-entered through `report`, so it never meets `applyPolicy` — and
    // nothing asserted it. An accidental "tidy the marker through report()"
    // refactor would hand `--suppress=P_TooManyDiagnostics` the power to
    // erase the record of an elision.
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 1;
    cfg.dedupWindow    = 0;
    cfg.policy.suppress.insert(DiagnosticCode::P_TooManyDiagnostics);
    DiagnosticReporter r{cfg};
    BufferId b{1};

    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, b, 0, 1));
    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error, b, 5, 6));

    ASSERT_EQ(r.all().size(), 2u);
    EXPECT_EQ(r.all().back().code, DiagnosticCode::P_TooManyDiagnostics)
        << "--suppress must not be able to remove the truncation notice";
    EXPECT_NE(r.all().back().actual.find("1 further diagnostic was dropped"),
              std::string::npos);
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

// eb2c6c7 audit fold (test-analyzer Finding C): pin that `format()`
// actually renders `d.contextPrefix` between the code-band and the
// expected/actual prose. The pre-existing `ContextPrefixDoesNotPollute
// DedupKey` test pins HASH-key exclusion only — a regression that
// dropped the `out += d.contextPrefix;` line in `format()` would
// still pass that test (dedup still collapses) and every existing
// format test (which leave `contextPrefix` empty). This is the
// dedicated rendering pin.
TEST(ReporterFormat, ContextPrefixAppearsBeforeExpectedActual) {
    DiagnosticReporter r;
    BufferRegistry bufs;
    auto buf = SourceBuffer::fromString("var x = 1 + 2 }\n", "<inline>");
    bufs.add(buf);

    auto d = makeDiag(DiagnosticCode::P_UnexpectedToken,
                      DiagnosticSeverity::Error,
                      buf->id(),
                      14, 15, "'}'");
    d.contextPrefix = "[target=x86_64:elf64-x86_64-linux] ";
    r.report(d);

    auto const out = r.format(r.all()[0], bufs);
    auto const prefixPos = out.find("[target=x86_64:elf64-x86_64-linux]");
    auto const gotPos    = out.find("got '}'");
    ASSERT_NE(prefixPos, std::string::npos)
        << "contextPrefix must appear in the rendered header line; a "
           "regression that drops `out += d.contextPrefix;` from "
           "format() must not silently pass";
    ASSERT_NE(gotPos, std::string::npos);
    EXPECT_LT(prefixPos, gotPos)
        << "contextPrefix must render BEFORE the expected/actual prose "
           "(headline shape: severity[code]: <prefix><prose>)";
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

// ─────────────────────────────────────────────────────────────────────────────
// D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN — CARET WIDTH FROM A **REAL** LITERAL.
//
// ★ WHY THIS TEST EXISTS, AND WHY IT IS NOT LIKE ITS NEIGHBOURS.
// Every other caret test in this file builds its span BY HAND
// (`makeDiag(..., 4, 7, ...)`) and therefore tests only the renderer's
// span → `^^^` loop. That is precisely how the delimiter defect survived: the
// renderer was always correct, and the span handed to it was one byte short.
// A suite that only ever feeds synthetic spans cannot observe a bug in how real
// spans are PRODUCED, no matter how many caret cases it enumerates.
//
// So this one takes the span from the REAL pipeline — tokenizer → parser → CST
// node span — and pins the user-visible symptom the whole cycle exists to fix:
//
//     int x = "abc";
//             ^^^^^     5 columns, the whole literal
//             ^^^^      4 columns was the BUG (the closing `"` had no token,
//                       so the node span that joins the children stopped short)
//
// The chain each assertion covers: the tokenizer emits a `StringEnd` token for
// the closing quote → `tree_builder` joins its children's spans into the
// `stringLiteralExpr` node span → that node span reaches a diagnostic → the
// reporter underlines it. A regression ANYWHERE on that chain shortens the
// caret, and only an end-to-end pin can see it.
//
// RED-ON-DISABLE: revert the closer emit and the underline becomes `^^^^`.
TEST(ReporterFormat, CaretWidthOverRealStringLiteralCoversTheClosingQuote) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;

    // Offsets: `int x = "abc";`
    //           0123456789...   → the literal is [8,13): `"`, `abc`, `"`.
    const std::string source = "int x = \"abc\";\n";
    auto src = SourceBuffer::fromString(source, "<inline>");

    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, _] = std::move(tk).tokenize();
    Parser p{src, schema, std::move(stream), DiagnosticBudget::libraryDefault()};
    Tree t = std::move(p).parse().tree;
    ASSERT_NE(t.root(), InvalidNode);

    // Locate the `stringLiteralExpr` NODE — the span a real diagnostic over the
    // literal would carry.
    const auto ruleId = t.schema().rules().find("stringLiteralExpr");
    ASSERT_TRUE(ruleId.valid());
    NodeId sle{};
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == ruleId.v) {
            sle = id;
            break;
        }
    }
    ASSERT_TRUE(sle.valid()) << "`\"abc\"` must parse to a stringLiteralExpr";

    // ★ THE PIN. The node's span must cover all FIVE bytes of `"abc"`, not the
    // four it covered while the closing quote had no token to contribute one.
    const SourceSpan span = t.span(sle);
    EXPECT_EQ(span.start(),  8u);
    EXPECT_EQ(span.end(),   13u) << "the node span must include the closing `\"`";
    EXPECT_EQ(span.length(), 5u)
        << "a 5-byte literal must have a 5-byte node span — 4 is the "
           "D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN defect";
    EXPECT_EQ(source.substr(span.start(), span.length()), "\"abc\"")
        << "the span must slice back to the COMPLETE literal, delimiters "
           "included — this is the source-extent round-trip the anchor is about";

    // ...and that span must render a 5-wide caret.
    DiagnosticReporter r;
    BufferRegistry bufs;
    bufs.add(src);
    auto d = makeDiag(DiagnosticCode::P_UnexpectedToken,
                      DiagnosticSeverity::Error,
                      src->id(),
                      span.start(), span.end(), "\"abc\"");
    r.report(d);
    auto const out = r.format(r.all()[0], bufs);
    EXPECT_NE(out.find("^^^^^"), std::string::npos)
        << "a diagnostic over `\"abc\"` must underline 5 columns; the witnessed "
           "symptom of this defect was a 4-wide caret under a 5-byte literal";
    EXPECT_EQ(out.find("^^^^^^"), std::string::npos)
        << "...and exactly 5 — a 6-wide caret would mean the span overshot the "
           "literal";
}

// Same pin for the CHAR form (multi-FORM discipline): `'c'` rides a different
// lexer mode and a different grammar rule, so the string pin above does not
// cover it. 3 bytes → 3 carets; the pre-fix defect would render 2.
TEST(ReporterFormat, CaretWidthOverRealCharLiteralCoversTheClosingQuote) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;

    // Offsets: `int x = 'c';` → the literal is [8,11).
    const std::string source = "int x = 'c';\n";
    auto src = SourceBuffer::fromString(source, "<inline>");

    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, _] = std::move(tk).tokenize();
    Parser p{src, schema, std::move(stream), DiagnosticBudget::libraryDefault()};
    Tree t = std::move(p).parse().tree;
    ASSERT_NE(t.root(), InvalidNode);

    const auto ruleId = t.schema().rules().find("charLiteralExpr");
    ASSERT_TRUE(ruleId.valid());
    NodeId cle{};
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == ruleId.v) {
            cle = id;
            break;
        }
    }
    ASSERT_TRUE(cle.valid()) << "`'c'` must parse to a charLiteralExpr";

    const SourceSpan span = t.span(cle);
    EXPECT_EQ(span.length(), 3u)
        << "a 3-byte char constant must have a 3-byte node span";
    EXPECT_EQ(source.substr(span.start(), span.length()), "'c'");

    DiagnosticReporter r;
    BufferRegistry bufs;
    bufs.add(src);
    auto d = makeDiag(DiagnosticCode::P_UnexpectedToken,
                      DiagnosticSeverity::Error,
                      src->id(),
                      span.start(), span.end(), "'c'");
    r.report(d);
    auto const out = r.format(r.all()[0], bufs);
    EXPECT_NE(out.find("^^^"), std::string::npos)
        << "a diagnostic over `'c'` must underline all 3 columns";
    EXPECT_EQ(out.find("^^^^"), std::string::npos) << "...and exactly 3";
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

// ─────────────────────────────────────────────────────────────────────────
// TF-C80: past-end-of-buffer spans must never over-read or abort.
//
// The preprocessor deliberately mints macro-expansion PRODUCT tokens whose
// spans start at `buf.text().size() + productOffset` — past end-of-buffer BY
// CONSTRUCTION (preprocessor.cpp `sbTextOf` / `text(Token const&)` decode that
// encoding). When such a token is the subject of a parse error, the span
// reaches the renderer verbatim. The renderer's line-start walk had no upper
// clamp, so it read `src[lineStart - 1]` past the buffer and then threw
// `std::out_of_range: string_view::substr` — an UNCAUGHT exception, i.e.
// SIGABRT with NO diagnostic at all, bypassing fail-loud discipline entirely.
//
// The abort was NON-DETERMINISTIC in the wild (~3/20) because it fired only
// when a `'\n'` (0x0A) happened to sit in the recycled-heap window past the
// buffer. These tests make that window DETERMINISTIC: build a string that
// contains newlines, then shrink it WITHOUT releasing capacity, so the bytes
// past `size()` are known to be `'\n'`-bearing. `fromString` MOVES the string,
// so the buffer (and its trailing bytes) survive into the SourceBuffer.
//
// Red-on-disable (verified): removing the `std::min` clamp in `extractLine`
// makes PastEndSpanWithNewlineInTrailingCapacity abort with
// `libc++abi: terminating due to uncaught exception ... string_view::substr`.
namespace {

// A buffer whose bytes past `size()` deterministically contain '\n'.
std::shared_ptr<SourceBuffer> bufferWithNewlinesPastEnd(std::string name) {
    std::string text = "abc\ndef\nghi\njkl\n";   // 16 bytes, newline at [3],[7],[11],[15]
    text.resize(4);                              // size()==4 ("abc\n"); capacity keeps [5..15]
    return SourceBuffer::fromString(std::move(text), std::move(name));
}

} // namespace

TEST(ReporterFormat, PastEndSpanWithNewlineInTrailingCapacity) {
    // Span starts at 8 — four bytes past the 4-byte buffer, and the byte at
    // index 7 in the surviving capacity is '\n'. Unclamped, the backward walk
    // stops immediately at lineStart==8 > size()==4, and substr(8, 0) throws.
    DiagnosticReporter r;
    BufferRegistry bufs;
    auto buf = bufferWithNewlinesPastEnd("<tfc80>");
    ASSERT_EQ(buf->text().size(), 4u);
    bufs.add(buf);
    r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error,
                      buf->id(), 8, 9, "'product'"));

    // The contract: renders, does not abort, and never escapes the buffer.
    auto const out = r.format(r.all()[0], bufs);
    EXPECT_NE(out.find("error[P0003]"), std::string::npos);
    EXPECT_NE(out.find("<tfc80>"), std::string::npos);
    // Past-end offsets clamp to the buffer, exactly as SourceBuffer::lineCol
    // already did — so this renders the LAST line, never heap garbage.
    EXPECT_EQ(out.find("def"), std::string::npos);
    EXPECT_EQ(out.find("ghi"), std::string::npos);
    EXPECT_EQ(out.find("jkl"), std::string::npos);
}

TEST(ReporterFormat, PastEndSpanRendersSameAsClampedSpan) {
    // A span past EOF must render the same location as one clamped to size().
    // Pins the clamp semantics, so a future "fix" that invents a bogus line
    // number for product spans fails here rather than silently misreporting.
    auto buf = SourceBuffer::fromString("int x = 1;\nint y = 2;\n", "<tfc80b>");
    const auto n = static_cast<ByteOffset>(buf->text().size());

    auto renderAt = [&](ByteOffset start) {
        DiagnosticReporter r;
        BufferRegistry bufs;
        bufs.add(buf);
        r.report(makeDiag(DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error,
                          buf->id(), start, start + 1, "'x'"));
        return r.format(r.all()[0], bufs);
    };

    auto const atEnd = renderAt(n);
    for (ByteOffset over : {n + 1u, n + 7u, n + 64u, n + 4096u}) {
        EXPECT_EQ(renderAt(over), atEnd)
            << "past-end span " << over << " (buffer size " << n
            << ") must clamp to end-of-buffer, not read past it";
    }
}
