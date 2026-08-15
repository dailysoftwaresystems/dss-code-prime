#include "core/types/parse_diagnostic.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

using dss::diagnosticCodeName;
using dss::diagnosticCodePrefix;
using dss::DiagnosticCode;
using dss::DiagnosticSeverity;
using dss::ParseDiagnostic;
using dss::ScopeKind;
using dss::scopeName;
using dss::severityName;

TEST(DiagnosticCode, SymbolicNameRoundtrip) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::P_UnexpectedToken),     "P_UnexpectedToken");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::P_PrematureEndOfInput), "P_PrematureEndOfInput");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::C_MissingField),        "C_MissingField");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::P_TooManyDiagnostics),  "P_TooManyDiagnostics");
    // D_* driver / compilation-unit codes (CU2).
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_FileNotFound),        "D_FileNotFound");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_EmptyInput),          "D_EmptyInput");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DuplicateFile),       "D_DuplicateFile");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::C_UnknownTypeExtension), "C_UnknownTypeExtension");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::C_TypeExtensionParamMismatch),
              "C_TypeExtensionParamMismatch");
    // H_* HIR verifier / lowering codes (plan 09, HR2 + HR3).
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::H_TypeUnresolved),      "H_TypeUnresolved");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::H_InvalidBreak),        "H_InvalidBreak");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::H_VerifierFailure),     "H_VerifierFailure");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::H_ExternHasInitializer), "H_ExternHasInitializer");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::H_ExternDeclMalformed),  "H_ExternDeclMalformed");
    // F_* FFI band — binary readers (D-FF1-PARTIAL-CORRUPTION-LOUD).
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_BinaryReaderPartialCorruption),
              "F_BinaryReaderPartialCorruption");
    // D_* driver band — `.dss-project.json` build hooks (`preBuildScripts` /
    // `postBuildScripts`, 0xD017..0xD018) and project dependencies
    // (`dependsOn`, 0xD019..0xD020).
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_ScriptSpawnFailed),
              "D_ScriptSpawnFailed");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_ScriptExitedNonZero),
              "D_ScriptExitedNonZero");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyManifestNotFound),
              "D_DependencyManifestNotFound");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyCycle),
              "D_DependencyCycle");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyArtifactProfileUnsupported),
              "D_DependencyArtifactProfileUnsupported");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyLanguageMismatch),
              "D_DependencyLanguageMismatch");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyGitNotFound),
              "D_DependencyGitNotFound");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyGitAcquireFailed),
              "D_DependencyGitAcquireFailed");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyGitFetchFallback),
              "D_DependencyGitFetchFallback");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyGitNameCollision),
              "D_DependencyGitNameCollision");
    // The same feature CONTINUED at 0xD022 after 0xD021 landed in between; see
    // the band-continuation pin below for why the gap is kept rather than
    // closed.
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyTargetFormatUnresolvable),
              "D_DependencyTargetFormatUnresolvable");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyTargetFormatAmbiguous),
              "D_DependencyTargetFormatAmbiguous");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyDerivedNameInvalid),
              "D_DependencyDerivedNameInvalid");
}

// The ten codes allocated for `.dss-project.json`'s `preBuildScripts` /
// `postBuildScripts` and `dependsOn` features occupy a CONTIGUOUS run at the
// top of the driver band, 0xD017..0xD020. Two things are pinned here that the
// name round-trip above cannot see:
//
//   (1) the VALUES. `diagnosticCodeName` would keep answering correctly if a
//       future edit renumbered one of these enumerators, but the number is
//       the operator-visible identity (`error[D0019]`) and appears in docs /
//       expected.json fixtures — so it is pinned literally, not derived.
//
//   (2) the nibble-boundary RENDERING. This run is the first to cross a hex
//       DECADE inside the band (0xD019 → 0xD01A) and to reach 0xD020, whose
//       low three nibbles (0x020) are easy to confuse with the 0xD002 slot
//       when read quickly. `diagnosticCodePrefix` strips only the 0xF000
//       family nibble (see the 0xD000u arm in parse_diagnostic.cpp), so the
//       expected renderings are "D0017".."D0020" — NOT "D0017".."D0032"
//       (decimal drift) and NOT "DD017" (unstripped nibble). Same property the
//       X_* band pin asserts, checked at the one place the D_* band has
//       actually grown.
TEST(DiagnosticCode, ProjectScriptsAndDependsOnBandIsContiguousAndRenders) {
    struct Row {
        DiagnosticCode   code;
        std::uint16_t    value;
        std::string_view rendered;
    };
    // Order is the allocation order; `value` ascends by exactly 1 per row,
    // which the contiguity check below relies on.
    constexpr Row kRows[] = {
        {DiagnosticCode::D_ScriptSpawnFailed,                    0xD017, "D0017"},
        {DiagnosticCode::D_ScriptExitedNonZero,                  0xD018, "D0018"},
        {DiagnosticCode::D_DependencyManifestNotFound,           0xD019, "D0019"},
        {DiagnosticCode::D_DependencyCycle,                      0xD01A, "D001A"},
        {DiagnosticCode::D_DependencyArtifactProfileUnsupported, 0xD01B, "D001B"},
        {DiagnosticCode::D_DependencyLanguageMismatch,           0xD01C, "D001C"},
        {DiagnosticCode::D_DependencyGitNotFound,                0xD01D, "D001D"},
        {DiagnosticCode::D_DependencyGitAcquireFailed,           0xD01E, "D001E"},
        {DiagnosticCode::D_DependencyGitFetchFallback,           0xD01F, "D001F"},
        {DiagnosticCode::D_DependencyGitNameCollision,           0xD020, "D0020"},
    };

    int checked = 0;
    std::uint16_t previous = 0xD016;  // D_SynthRecipeFamilyUnknown, the predecessor.
    for (Row const& row : kRows) {
        EXPECT_EQ(static_cast<std::uint16_t>(row.code), row.value)
            << diagnosticCodeName(row.code) << " was renumbered";
        // Contiguous, ascending, no gaps and no reuse.
        EXPECT_EQ(row.value, static_cast<std::uint16_t>(previous + 1u))
            << diagnosticCodeName(row.code) << " is not contiguous with its predecessor";
        previous = row.value;
        // Still inside the driver band (the 0xD000u arm of diagnosticCodePrefix).
        ASSERT_EQ(row.value & 0xF000u, 0xD000u)
            << diagnosticCodeName(row.code) << " escaped the D_* band";
        EXPECT_EQ(diagnosticCodePrefix(row.code), row.rendered);
        ++checked;
    }
    // Non-vacuity: a counter incremented in the loop, not sizeof the array.
    EXPECT_EQ(checked, 10);
}

// ★ THE SAME FEATURE, CONTINUED AT 0xD022 — AND THE GAP AT 0xD021 IS PINNED
// DELIBERATELY, NOT TOLERATED. AP6's consumer-driven format derivation added
// three more `dependsOn` codes, but `D_SuppressRequestIgnored` (0xD021) had
// already taken the next slot, so this topic is NO LONGER one contiguous run.
// That is the correct outcome and this pin exists to stop someone "tidying" it:
// the number is the OPERATOR-VISIBLE identity (`error[D0022]`), it appears in
// docs and in `expected.json` fixtures, and renumbering an allocated code to
// close a cosmetic gap would rewrite a published name. A gap inside a topic's
// range is cheap; a moved number is not.
//
// So this asserts two different things from the run above: the three values
// themselves, AND that their predecessor is 0xD021 rather than 0xD020 — i.e.
// the gap is where it is supposed to be. A future code appended at 0xD025 must
// extend this pin, not the one above.
TEST(DiagnosticCode, DependsOnBandContinuationAfterTheSuppressSlot) {
    struct Row {
        DiagnosticCode   code;
        std::uint16_t    value;
        std::string_view rendered;
    };
    constexpr Row kRows[] = {
        {DiagnosticCode::D_DependencyTargetFormatUnresolvable, 0xD022, "D0022"},
        {DiagnosticCode::D_DependencyTargetFormatAmbiguous,    0xD023, "D0023"},
        {DiagnosticCode::D_DependencyDerivedNameInvalid,       0xD024, "D0024"},
    };

    // The predecessor is the SUPPRESS slot, not the last `dependsOn` code —
    // asserted explicitly so the discontinuity is a stated fact rather than an
    // accident someone later "corrects".
    ASSERT_EQ(static_cast<std::uint16_t>(DiagnosticCode::D_SuppressRequestIgnored),
              0xD021)
        << "the slot this run continues after moved";
    ASSERT_EQ(static_cast<std::uint16_t>(DiagnosticCode::D_DependencyGitNameCollision),
              0xD020)
        << "the earlier dependsOn run moved; both pins must agree";

    int           checked  = 0;
    std::uint16_t previous = 0xD021;
    for (Row const& row : kRows) {
        EXPECT_EQ(static_cast<std::uint16_t>(row.code), row.value)
            << diagnosticCodeName(row.code) << " was renumbered";
        EXPECT_EQ(row.value, static_cast<std::uint16_t>(previous + 1u))
            << diagnosticCodeName(row.code) << " is not contiguous with its predecessor";
        previous = row.value;
        ASSERT_EQ(row.value & 0xF000u, 0xD000u)
            << diagnosticCodeName(row.code) << " escaped the D_* band";
        EXPECT_EQ(diagnosticCodePrefix(row.code), row.rendered);
        ++checked;
    }
    // Non-vacuity: a counter incremented in the loop, not sizeof the array.
    EXPECT_EQ(checked, 3);
}

TEST(DiagnosticCode, PrefixIsPhaseLetterPlusHexNumber) {
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::P_UnexpectedToken),  "P0001");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::P_AmbiguousToken),   "P0008");
    // 9xxx range stays as 9xxx (not collapsed via the high-nibble strip).
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::P_BuilderInvariant), "P9000");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::P_RecoveryStalled),  "P9003");
    // C_* prefix and the high nibble is stripped for the numeric portion.
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::C_MissingField),     "C0001");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::C_AmbiguousAlternatives), "C0010");
    // D_* prefix; high nibble stripped like C_*.
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::D_FileNotFound),     "D0001");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::D_DuplicateFile),    "D0003");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::C_UnknownTypeExtension),       "C002A");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::C_TypeExtensionParamMismatch), "C002B");
    // H_* prefix; the 0xF high nibble renders as 'H' and is stripped from the
    // numeric portion (like C/D/S).
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::H_TypeUnresolved),   "H0001");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::H_InvalidBreak),     "H0002");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::H_VerifierFailure),  "H0003");
    // X_* prefix; the 0x2 high nibble renders as 'X' and is stripped from the
    // numeric portion (like A/C/D/S/H). Spot checks at both ends of the band —
    // the band-wide property is pinned by
    // PrefixOptimizerBandRendersAsXWithNibbleStripped below.
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::X_UnknownPassId),          "X0001");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::X_PipelineVersionMismatch), "X0002");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::X_InlineMalformedCallSite), "X0008");
}

// D-DIAG-OPT-FAMILY-NIBBLE-CLAIMED-IN-HEADER-BUT-NOT-IN-RENDERER.
//
// The X_* optimizer family was allocated in the 0x2xxx band in
// parse_diagnostic.hpp, but diagnosticCodePrefix() shipped with no 0x2000u
// arm — so the whole family rendered under the parser's default letter with
// the family nibble left in the number ("P2002" instead of "X0002"). Plan 00
// §0.3 requires claiming a new family in the header AND in the renderer in
// the same PR; only the header half had landed.
//
// This pin is deliberately BAND-WIDE rather than a couple of spot checks: it
// asserts the PROPERTY (every allocated 0x2xxx code renders 'X' + a stripped
// nibble) rather than two instances of it. Two halves:
//
//   (1) the eight currently-allocated codes, named explicitly, so a failure
//       report names the enumerator;
//   (2) a DISCOVERY scan of the whole 0x2000..0x2FFF band. DiagnosticCode has
//       a fixed underlying type (std::uint16_t), so every value in that range
//       is a valid enum value and `diagnosticCodeName` answers "Unknown" for
//       the unallocated ones. That makes the band enumerable WITHOUT an
//       iterable-enum facility — and it means a NINTH X_* code added later
//       inherits this guarantee automatically instead of silently escaping a
//       hand-maintained list, which is how the original defect survived.
//
// The scan carries a FLOOR (same reasoning as tools/check-anchor-registry.sh's
// per-root floors): a scan that discovers nothing would otherwise pass while
// checking nothing.
TEST(DiagnosticCode, PrefixOptimizerBandRendersAsXWithNibbleStripped) {
    // The property under test, applied to one code.
    const auto expectRendersAsX = [](DiagnosticCode code) {
        const auto raw = static_cast<std::uint16_t>(code);
        const std::string rendered = diagnosticCodePrefix(code);
        // The whole point of the fix: letter 'X', not the default 'P'.
        EXPECT_EQ(rendered.front(), 'X')
            << diagnosticCodeName(code) << " rendered as " << rendered;
        // ...and the family nibble is STRIPPED, so 0x2002 is "X0002" not "X2002".
        EXPECT_EQ(rendered, std::format("X{:04X}", raw & 0x0FFFu))
            << diagnosticCodeName(code) << " rendered as " << rendered;
    };

    // (1) Every X_* code allocated as of TF-C118, 0x2001..0x2008.
    constexpr DiagnosticCode kOptimizerBand[] = {
        DiagnosticCode::X_UnknownPassId,
        DiagnosticCode::X_PipelineVersionMismatch,
        DiagnosticCode::X_UnknownPassName,
        DiagnosticCode::X_PipelineMalformed,
        DiagnosticCode::X_PipelineNameResolutionFailed,
        DiagnosticCode::X_OptReturnFalseWithoutDiagnostic,
        DiagnosticCode::X_OptPassSkipped,
        DiagnosticCode::X_InlineMalformedCallSite,
    };

    int named = 0;
    for (const DiagnosticCode code : kOptimizerBand) {
        // Guards the list itself: anything listed must really be in the band.
        ASSERT_EQ(static_cast<std::uint16_t>(code) & 0xF000u, 0x2000u)
            << diagnosticCodeName(code) << " is not in the 0x2xxx optimizer band";
        expectRendersAsX(code);
        ++named;
    }
    // Non-vacuity: a COUNTER incremented in the loop, not sizeof the array —
    // the latter is a compile-time tautology that stays green even if the loop
    // never executes.
    EXPECT_EQ(named, 8);

    // (2) Discovery scan of the entire band — catches any X_* code that exists
    // but is missing from the list above.
    int discovered = 0;
    for (std::uint32_t v = 0x2000u; v <= 0x2FFFu; ++v) {
        const auto code = static_cast<DiagnosticCode>(static_cast<std::uint16_t>(v));
        if (diagnosticCodeName(code) == std::string_view{"Unknown"}) {
            continue;  // unallocated slot in the band
        }
        expectRendersAsX(code);
        ++discovered;
    }
    // Floor, not equality: a new X_* code should not fail this test, but a
    // COLLAPSED scan (discovering nothing, e.g. if diagnosticCodeName stopped
    // answering) must not pass as clean.
    EXPECT_GE(discovered, named)
        << "band scan discovered fewer codes than are explicitly listed";
}

TEST(DiagnosticSeverity, NameMapping) {
    EXPECT_EQ(severityName(DiagnosticSeverity::Hint),    "hint");
    EXPECT_EQ(severityName(DiagnosticSeverity::Info),    "info");
    EXPECT_EQ(severityName(DiagnosticSeverity::Warning), "warning");
    EXPECT_EQ(severityName(DiagnosticSeverity::Error),   "error");
}

TEST(ParseDiagnostic, DefaultsAreSensible) {
    ParseDiagnostic d;
    EXPECT_EQ(d.code, DiagnosticCode::None);
    EXPECT_EQ(d.severity, DiagnosticSeverity::Error);
    EXPECT_FALSE(d.ruleContext.has_value());
    EXPECT_TRUE(d.expected.empty());
    EXPECT_TRUE(d.scopeStack.empty());
    EXPECT_TRUE(d.related.empty());
    EXPECT_TRUE(d.actual.empty());
}

TEST(ScopeKind, NameMapping) {
    EXPECT_EQ(scopeName(ScopeKind::None),    "None");
    EXPECT_EQ(scopeName(ScopeKind::Root),    "Root");
    EXPECT_EQ(scopeName(ScopeKind::Block),   "Block");
    EXPECT_EQ(scopeName(ScopeKind::Generic), "Generic");
    EXPECT_EQ(scopeName(static_cast<ScopeKind>(2048)), "Custom");
}
