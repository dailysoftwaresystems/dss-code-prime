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
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyOutputNameCollision),
              "D_DependencyOutputNameCollision");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyGraphTooDeep),
              "D_DependencyGraphTooDeep");
    // (0xD027 is UNUSED — see the band-continuation pin below.)
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_ArtifactProfileNoServingFormat),
              "D_ArtifactProfileNoServingFormat");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DependencyBuildFailed),
              "D_DependencyBuildFailed");
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
//
// ★ EXTENDED THROUGH 0xD029, AND THE EXTENSION IS THE POINT OF THE PIN, NOT
// BOOKKEEPING. `D-AP6-NEW-DIAGNOSTIC-CODES-HAD-NO-VALUE-PIN` closed on exactly
// this failure: a literal `EXPECT_EQ(checked, N)` cannot notice codes that were
// never added to the table, so four later allocations (0xD025 / 0xD026 from the
// AP6 graph work, 0xD028 from the AP3 artifact-profile reject split, and
// 0xD029 below) sat outside any value pin. The contiguity check is what forces
// the issue — appending 0xD029 alone would fail `previous + 1` against 0xD024 —
// so the run is carried forward whole rather than sampled.
//
// 0xD029 is `D_DependencyBuildFailed`, and it is the reason this edit exists:
// it splits "the dependency's own build failed" out of 0xD022, which had been
// emitted for BOTH that and the zero-candidate format derivation. The two rows
// sitting at opposite ends of one contiguous run is the visible form of a
// distinction that has to survive — see the paired resolver pins, which assert
// each fact fires its OWN code and NOT the other's.
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
        {DiagnosticCode::D_DependencyOutputNameCollision,      0xD025, "D0025"},
        {DiagnosticCode::D_DependencyGraphTooDeep,             0xD026, "D0026"},
        // 0xD027 is deliberately ABSENT: it was allocated as
        // `D_ArtifactProfileEmitsNoArtifact` and freed again in the same
        // in-flight change (plan 06 §5.1 B.12 and its correction, 2026-08-15 —
        // a `module` IS a library, so a standalone module build is legitimate
        // and the code it would have reported was withdrawn with the rule).
        // The slot is left unused rather than back-filled, because 0xD028
        // shipped alongside it and renumbering a code is what the append-only
        // rule forbids.
        {DiagnosticCode::D_ArtifactProfileNoServingFormat,     0xD028, "D0028"},
        {DiagnosticCode::D_DependencyBuildFailed,              0xD029, "D0029"},
        // ★ 0xD02A IS A DIFFERENT TOPIC AND IT BELONGS IN THIS RUN ANYWAY —
        // `D_LanguageTargetIsaMismatch`, the language↔target architecture gate
        // (plan 06 §5.1 B.13.5), not a `dependsOn` code. It is carried here
        // because THIS PIN GUARDS AN ORDINAL RUN, NOT A TOPIC: the moment a
        // value is left out, `previous + 1` stops constraining everything after
        // it, and a code with no pin is exactly what re-opened
        // `D-AP6-NEW-DIAGNOSTIC-CODES-HAD-NO-VALUE-PIN` one cycle after it
        // closed. ⓘ Splitting this into a topic-named pin later is fine; doing
        // so must KEEP the value in some contiguity run, not merely move it to
        // a lone `EXPECT_EQ` that the next allocation can silently step past.
        // ⚠ One of its two emit sites IS in the dependency resolver
        // (`dependency_resolver.cpp:947`), so the topic boundary was never as
        // clean as the test name suggests.
        {DiagnosticCode::D_LanguageTargetIsaMismatch,          0xD02A, "D002A"},
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

    // ⓘ THE RUN NOW CARRIES A SECOND, STATED HOLE AT 0xD027 — an allocation
    // that was withdrawn in the same in-flight change that made it (see the
    // table above). It is named here rather than tolerated as slack, so the
    // contiguity check keeps its whole strength everywhere else: exactly ONE
    // value may be skipped, and it must be THIS one.
    constexpr std::uint16_t kWithdrawnSlot = 0xD027;

    int           checked  = 0;
    std::uint16_t previous = 0xD021;
    for (Row const& row : kRows) {
        EXPECT_EQ(static_cast<std::uint16_t>(row.code), row.value)
            << diagnosticCodeName(row.code) << " was renumbered";
        std::uint16_t const expected =
            static_cast<std::uint16_t>(previous + 1u) == kWithdrawnSlot
                ? static_cast<std::uint16_t>(previous + 2u)
                : static_cast<std::uint16_t>(previous + 1u);
        EXPECT_EQ(row.value, expected)
            << diagnosticCodeName(row.code) << " is not contiguous with its predecessor";
        previous = row.value;
        ASSERT_EQ(row.value & 0xF000u, 0xD000u)
            << diagnosticCodeName(row.code) << " escaped the D_* band";
        EXPECT_EQ(diagnosticCodePrefix(row.code), row.rendered);
        ++checked;
    }
    // Non-vacuity: a counter incremented in the loop, not sizeof the array.
    // ⚠ THIS LITERAL IS THE THING THAT WENT WRONG LAST TIME. It must be raised
    // by hand whenever a row is appended, and a code allocated WITHOUT touching
    // it is a code with no value pin — which is precisely how 0xD022..0xD024
    // once landed unpinned behind an `EXPECT_EQ(checked, 10)`.
    //
    // ⓘ THIS LITERAL IS NO LONGER THE ONLY THING STANDING BETWEEN AN ALLOCATION
    // AND AN UNPINNED CODE, and that is the point of the change that raised it
    // to 8. `scripts/check-diagnostic-codes/check-diagnostic-codes.py` reads the ENUM itself and fails on
    // any code no compiled test names — a hand-maintained table can only check
    // rows somebody remembered to add, which is precisely how 0xD02A shipped
    // engine code while appearing in zero test files. Keep raising this literal;
    // just do not mistake it for the mechanism.
    EXPECT_EQ(checked, 8);
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
// The scan carries a FLOOR (same reasoning as scripts/check-anchor-registry/check-anchor-registry.sh's
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

// The seven inline-asm P5 operand-binding codes occupy a CONTIGUOUS run at the
// top of the semantic band, 0xE065..0xE06B, immediately after the P1 arc's last
// code (`S_InlineAsmDuplicateQualifier`, 0xE064). Same three properties the
// D_* band pin above asserts, at the one place the S_* band has just grown:
//
//   (1) the NAMES. `diagnosticCodeName` has a no-`default:` exhaustive switch
//       under `-Werror=switch` (D-DIAG-CODENAME-EXHAUSTIVE-WARN), so a code
//       added with NO name arm fails the BUILD on GCC/Clang — but on a local
//       MSVC build that gate is `/we4062`, and either way "the switch has an
//       arm" is not "the arm returns the right string". A copy-pasted arm
//       returning its neighbour's name compiles clean and is caught only here.
//
//   (2) the VALUES. The number is the operator-visible identity (`error[S0065]`
//       on stderr, in `.diag` goldens, in `expectDiagnostics` manifests), so it
//       is pinned literally rather than derived from the enumerator.
//
//   (3) CONTIGUITY with 0xE064. The S_* band is dense — ✔MEASURED over the
//       enum's own declaration lines at this commit: 107 codes spanning
//       0xE001..0xE06B with NO gaps, FOUR of them RETIRED-but-reserved
//       (0xE015, 0xE04E, 0xE04F, 0xE052 — reserved, never renumbered, never
//       reused) — so "the next free value" is a fact about the band and not a
//       guess. A future code that lands on a used slot, or that skips one,
//       reds here instead of colliding silently with a golden.
TEST(DiagnosticCode, InlineAsmOperandBindingBandIsContiguousAndRenders) {
    struct Row {
        DiagnosticCode   code;
        std::uint16_t    value;
        std::string_view rendered;
        std::string_view name;
    };
    // Allocation order; `value` ascends by exactly 1 per row.
    constexpr Row kRows[] = {
        {DiagnosticCode::S_InlineAsmConstraintLetterUndeclared, 0xE065, "S0065",
         "S_InlineAsmConstraintLetterUndeclared"},
        {DiagnosticCode::S_InlineAsmConstraintUnsupportedForm, 0xE066, "S0066",
         "S_InlineAsmConstraintUnsupportedForm"},
        {DiagnosticCode::S_InlineAsmOperandModifierUnsupported, 0xE067, "S0067",
         "S_InlineAsmOperandModifierUnsupported"},
        {DiagnosticCode::S_InlineAsmClobberUnknown, 0xE068, "S0068",
         "S_InlineAsmClobberUnknown"},
        {DiagnosticCode::S_InlineAsmTemplateUnparsable, 0xE069, "S0069",
         "S_InlineAsmTemplateUnparsable"},
        {DiagnosticCode::S_InlineAsmPlaceholderOutOfRange, 0xE06A, "S006A",
         "S_InlineAsmPlaceholderOutOfRange"},
        {DiagnosticCode::S_InlineAsmPlaceholderInBasicTemplate, 0xE06B, "S006B",
         "S_InlineAsmPlaceholderInBasicTemplate"},
    };

    int checked = 0;
    std::uint16_t previous = 0xE064;  // S_InlineAsmDuplicateQualifier, the predecessor.
    ASSERT_EQ(static_cast<std::uint16_t>(DiagnosticCode::S_InlineAsmDuplicateQualifier),
              previous)
        << "the predecessor this run is anchored to was renumbered, so the "
           "contiguity check below would compare against a stale value";
    for (Row const& row : kRows) {
        EXPECT_EQ(static_cast<std::uint16_t>(row.code), row.value)
            << row.name << " was renumbered";
        EXPECT_EQ(row.value, static_cast<std::uint16_t>(previous + 1u))
            << row.name << " is not contiguous with its predecessor";
        previous = row.value;
        ASSERT_EQ(row.value & 0xF000u, 0xE000u)
            << row.name << " escaped the S_* semantic band";
        // The name arm exists AND returns this code's own name.
        EXPECT_EQ(diagnosticCodeName(row.code), row.name);
        EXPECT_EQ(diagnosticCodePrefix(row.code), row.rendered);
        ++checked;
    }
    // Non-vacuity: a counter incremented in the loop, not sizeof the array.
    EXPECT_EQ(checked, 7);
}
