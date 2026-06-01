#include "core/types/diagnostic_reporter.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/unsuppressable_codes.hpp"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>

// D-FF2-UNSUPP closed-table pins.
//
// PINS the contract that severity-gating codes (architectural exclusions,
// pending-plan announcements, lowering / verifier / linker invariants)
// pass through DiagnosticReporter::applyPolicy unchanged regardless of
// --suppress / overrides / warningsAsErrors. Without these pins, a
// regression that drops the `isUnsuppressable` gate would silently
// re-open the silent-drop surface every such code was introduced to
// close — exactly the bug class D-FF2-UNSUPP closes.

using namespace dss;

namespace {

ParseDiagnostic makeDiag(DiagnosticCode code,
                         DiagnosticSeverity sev = DiagnosticSeverity::Error) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = sev;
    d.buffer   = BufferId{1};
    d.span     = SourceSpan::of(0, 1);
    d.actual   = "x";
    return d;
}

} // namespace

TEST(UnsuppressableCodes, MembershipIncludesCoreArchitecturalCodes) {
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::H_ExternHasInitializer));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::D_TargetAbiModelUnsupportedByDriver));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::F_FfiIngestAbiModelUnsupported));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::F_FfiIngestEmptyCanonical));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::D_PlanNotLanded));
}

TEST(UnsuppressableCodes, BothH2SplitArmsAreUnsuppressable) {
    // Post-fold #11 type-design CRITICAL pin: H2 split (post-fold #9)
    // introduced `H_ExternDeclMalformed` alongside the pre-existing
    // `H_UnsupportedLoweringForKind` arm in `lowerExternDecl`. Both
    // terminate lowering with `return errorNode(node)` + gate ok via
    // errorCount. Pre-fix the new arm was missing from the closed-
    // table, silently re-opening half the gate's coverage for any
    // future grammar admitting parse-recovery shapes that reach
    // lowering. This pin ensures the H2 split's two arms travel
    // together — any future re-split must add the new code here too.
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::H_UnsupportedLoweringForKind))
        << "engine-config arm: --suppress would let an extern decl "
           "with no kindByChild config silently fall through to "
           "makeExternGlobal, re-opening the D-FF2-3 silent-drop";
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::H_ExternDeclMalformed))
        << "parse-recovery arm: --suppress would let a malformed "
           "extern decl (incomplete CST) silently fall through, "
           "same silent-drop surface as the engine-config arm";
}

TEST(UnsuppressableCodes, RegularDiagnosticsRemainSuppressable) {
    // Stylistic / non-gating codes MUST stay suppressable so --suppress
    // remains a useful policy tool for the codes it was designed for.
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::P_UnexpectedToken));
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::P_DeprecatedSyntax));
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::P_AmbiguousToken));
}

TEST(UnsuppressableCodes, ListSelfConsistent) {
    // Every member of the public closed-table view must report
    // unsuppressable; no member duplicated; every entry must be a
    // real enumerated DiagnosticCode (not a static_cast from a stray
    // integer that slipped past type-checking); no member listed at
    // the "Unknown" sentinel.
    auto const codes = unsuppressableCodes();
    EXPECT_GT(codes.size(), 0u);
    std::unordered_set<DiagnosticCode> seen;
    for (auto c : codes) {
        EXPECT_TRUE(isUnsuppressable(c))
            << "code " << diagnosticCodeName(c)
            << " is in the closed-table list but isUnsuppressable returns false";
        EXPECT_TRUE(seen.insert(c).second)
            << "duplicate entry for " << diagnosticCodeName(c);
        EXPECT_STRNE(diagnosticCodeName(c).data(), "Unknown")
            << "code value 0x" << std::hex << static_cast<unsigned>(c)
            << " does not name a real DiagnosticCode (sentinel reached)";
    }
}

TEST(Reporter, SuppressIsIgnoredForUnsuppressableCode) {
    // The signature pin: a user setting --suppress=H_ExternHasInitializer
    // does NOT silence the diagnostic. Closes the silent-drop surface
    // where the suppress flag would otherwise re-open the failure
    // mode the H_ExternHasInitializer fold permanently closes.
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::H_ExternHasInitializer);
    DiagnosticReporter r{cfg};
    r.report(makeDiag(DiagnosticCode::H_ExternHasInitializer));
    EXPECT_EQ(r.all().size(), 1u)
        << "unsuppressable code must reach `all_` despite --suppress";
    EXPECT_EQ(r.errorCount(), 1u)
        << "errorCount must reflect the unsuppressable error so "
           "downstream exit-code gates fire correctly";
}

TEST(Reporter, OverrideCannotDemoteUnsuppressableCodeBelowError) {
    // The suppress check is the headline, but `overrides` is a parallel
    // attack surface: a user could demote H_ExternHasInitializer to
    // Warning, then errorCount() would return 0 even though the
    // diagnostic IS in `all_`. Bypassing overrides on unsuppressable
    // codes closes that variant.
    DiagnosticReporter::Config cfg;
    cfg.policy.overrides[DiagnosticCode::H_ExternHasInitializer]
        = DiagnosticSeverity::Warning;
    DiagnosticReporter r{cfg};
    r.report(makeDiag(DiagnosticCode::H_ExternHasInitializer,
                      DiagnosticSeverity::Error));
    ASSERT_EQ(r.all().size(), 1u);
    EXPECT_EQ(r.all()[0].severity, DiagnosticSeverity::Error)
        << "overrides must not demote an unsuppressable code; "
           "applyPolicy short-circuits before overrides apply";
    EXPECT_EQ(r.errorCount(), 1u);
}

TEST(Reporter, UnsuppressableBypassesGlobalCap) {
    // Post-fold #11 silent-failure F1 CRITICAL: pre-fold a noisy parse
    // hitting `maxDiagnostics` would mask every subsequent
    // unsuppressable code. With the F1 fix, unsuppressable codes
    // bypass the global cap entirely — they always reach `all_` so
    // `errorCount()` correctly reflects the severity-gating diagnostic.
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 1;
    DiagnosticReporter r{cfg};
    // Fill the cap with regular suppressable errors — 1st lands, 2nd
    // trips the cap marker. (cap check fires when `all_.size() >=
    // maxDiagnostics` on the SECOND report.)
    r.report(makeDiag(DiagnosticCode::P_UnexpectedToken));
    auto d2 = makeDiag(DiagnosticCode::P_UnknownToken);
    d2.span = SourceSpan::of(5, 6);  // distinct from d1 so dedup doesn't collapse
    r.report(std::move(d2));
    ASSERT_TRUE(r.hitCap())
        << "test setup: cap must be hit before the unsuppressable report";
    // Unsuppressable code MUST still land despite hitCap_.
    r.report(makeDiag(DiagnosticCode::H_ExternHasInitializer));
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.code == DiagnosticCode::H_ExternHasInitializer) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "unsuppressable code must bypass the global cap; pre-F1 "
           "the `if (hitCap_) return;` short-circuit would have eaten "
           "this report";
}

TEST(Reporter, UnsuppressableBypassesPerCodeCap) {
    // Post-fold #11 F1: per-code coalescing must not silently drop a
    // 2nd / 3rd / ... unsuppressable diagnostic. 12 I_NotDominated
    // violations in a module against maxPerCode=10 must all surface.
    DiagnosticReporter::Config cfg;
    cfg.maxPerCode = 1;
    DiagnosticReporter r{cfg};
    for (int i = 0; i < 5; ++i) {
        auto d = makeDiag(DiagnosticCode::I_NotDominated);
        d.span = SourceSpan::of(static_cast<ByteOffset>(i),
                                static_cast<ByteOffset>(i + 1));
        r.report(std::move(d));
    }
    EXPECT_EQ(r.all().size(), 5u)
        << "all 5 unsuppressable diagnostics must reach `all_` "
           "despite maxPerCode=1; pre-F1 would have coalesced to 1";
}

TEST(Reporter, UnsuppressableBypassesDedupWindow) {
    // Post-fold #11 F1: dedup window is policy too. Two structurally
    // identical unsuppressable diagnostics in the dedup window MUST
    // both land — every instance gates ok.
    DiagnosticReporter::Config cfg;
    cfg.dedupWindow = 4;  // default
    DiagnosticReporter r{cfg};
    auto d1 = makeDiag(DiagnosticCode::K_ImageWriteShort);
    auto d2 = makeDiag(DiagnosticCode::K_ImageWriteShort);
    // Same code, same buffer, same span, same actual — dedup would
    // normally collapse these.
    r.report(std::move(d1));
    r.report(std::move(d2));
    EXPECT_EQ(r.all().size(), 2u)
        << "dedup must not collapse unsuppressable diagnostics; "
           "the second image-write failure carries distinct signal";
}

TEST(Reporter, WarningsAsErrorsDoesNotMutateUnsuppressableCode) {
    // Post-fold #12 test-analyzer Gap 5: warningsAsErrors completes
    // the policy-trifecta. Today no unsuppressable code is emitted
    // at Warning severity (the closed-table is all Error producers),
    // but a future regression could (e.g. demote H_TypeUnresolved to
    // Warning at producer). applyPolicy must preserve the producer
    // severity end-to-end — the unsuppressable short-circuit fires
    // BEFORE the warningsAsErrors flip.
    DiagnosticReporter::Config cfg;
    cfg.policy.warningsAsErrors = true;
    DiagnosticReporter r{cfg};
    // Synthesize a Warning-severity unsuppressable diagnostic.
    auto d = makeDiag(DiagnosticCode::H_TypeUnresolved,
                      DiagnosticSeverity::Warning);
    r.report(std::move(d));
    ASSERT_EQ(r.all().size(), 1u);
    EXPECT_EQ(r.all()[0].severity, DiagnosticSeverity::Warning)
        << "warningsAsErrors must not promote an unsuppressable code "
           "— producer severity is preserved end-to-end";
}

TEST(Reporter, ForceReportRespectsUnsuppressableBypassNotJustCap) {
    // Post-fold #12 test-analyzer Gap 1: forceReport bypasses the
    // cap but goes through applyPolicy. With the F1 fold,
    // unsuppressable codes ALSO bypass cap/dedup/maxPerCode/
    // maxDiagnostics inside report() itself. forceReport's contract
    // is "bypass cap"; combining with the unsuppressable bypass
    // means both paths converge — unsuppressable codes always reach
    // `all_` regardless of which entry point was used. Pin both.
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 1;
    DiagnosticReporter r{cfg};
    // Fill the cap to set hitCap_.
    r.report(makeDiag(DiagnosticCode::P_UnexpectedToken));
    auto d2 = makeDiag(DiagnosticCode::P_UnknownToken);
    d2.span = SourceSpan::of(5, 6);
    r.report(std::move(d2));
    ASSERT_TRUE(r.hitCap());
    // forceReport an unsuppressable code — must land.
    r.forceReport(makeDiag(DiagnosticCode::K_SymbolUndefined));
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "forceReport of an unsuppressable code must land in `all_` "
           "despite cap; both entry points converge through applyPolicy "
           "which short-circuits for unsuppressable codes";
}

TEST(Reporter, ForceReportStillDropsRegularSuppressedCode) {
    // Companion to the above: forceReport bypasses CAPS but does
    // still consult applyPolicy. A normal suppressable code that's
    // in the suppress set must still be dropped under forceReport.
    // Pre-fix any forceReport-shortcut around applyPolicy would
    // silently emit suppressed codes — a regression class we pin.
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::P_DeprecatedSyntax);
    DiagnosticReporter r{cfg};
    r.forceReport(makeDiag(DiagnosticCode::P_DeprecatedSyntax,
                           DiagnosticSeverity::Warning));
    EXPECT_EQ(r.all().size(), 0u)
        << "forceReport routes through applyPolicy; suppressed normal "
           "codes still drop";
}

TEST(Reporter, NormalCodeSuppressedAlongsideUnsuppressableInSameReporter) {
    // Co-existence: suppressing a regular code on the same reporter must
    // still drop the regular code while leaving the unsuppressable one
    // through. Pins that the two policies don't interfere.
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::P_DeprecatedSyntax);
    cfg.policy.suppress.insert(DiagnosticCode::H_ExternHasInitializer);
    DiagnosticReporter r{cfg};
    r.report(makeDiag(DiagnosticCode::P_DeprecatedSyntax,
                      DiagnosticSeverity::Warning));
    r.report(makeDiag(DiagnosticCode::H_ExternHasInitializer));
    ASSERT_EQ(r.all().size(), 1u);
    EXPECT_EQ(r.all()[0].code, DiagnosticCode::H_ExternHasInitializer);
}
