#include "core/types/diagnostic_reporter.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/unsuppressable_codes.hpp"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>

// D-FF2-4 closed-table pins.
//
// PINS the contract that severity-gating codes (architectural exclusions,
// pending-plan announcements, lowering / verifier / linker invariants)
// pass through DiagnosticReporter::applyPolicy unchanged regardless of
// --suppress / overrides / warningsAsErrors. Without these pins, a
// regression that drops the `isUnsuppressable` gate would silently
// re-open the silent-drop surface every such code was introduced to
// close — exactly the bug class D-FF2-4 closes.

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

TEST(UnsuppressableCodes, RegularDiagnosticsRemainSuppressable) {
    // Stylistic / non-gating codes MUST stay suppressable so --suppress
    // remains a useful policy tool for the codes it was designed for.
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::P_UnexpectedToken));
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::P_DeprecatedSyntax));
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::P_AmbiguousToken));
}

TEST(UnsuppressableCodes, ListSelfConsistent) {
    // Every member of the public closed-table view must report
    // unsuppressable; no member duplicated.
    auto const codes = unsuppressableCodes();
    EXPECT_GT(codes.size(), 0u);
    std::unordered_set<DiagnosticCode> seen;
    for (auto c : codes) {
        EXPECT_TRUE(isUnsuppressable(c))
            << "code " << diagnosticCodeName(c)
            << " is in the closed-table list but isUnsuppressable returns false";
        EXPECT_TRUE(seen.insert(c).second)
            << "duplicate entry for " << diagnosticCodeName(c);
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
