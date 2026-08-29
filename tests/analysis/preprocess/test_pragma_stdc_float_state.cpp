// D-PP-PRAGMA-RECOGNIZED-SEMANTICS — `#pragma STDC` (C23 6.10.8
// `standard-pragma`) is RECOGNIZED, ACCEPTED, and per-form JUDGED.
//
// Until 2026-08-29 `{"prefix": ["STDC"], "effect": "unsupported"}` made every
// one of the nine standard forms REFUSE the translation unit. ✔MEASURED
// 2026-08-29, each reference invoked SEPARATELY, one self-contained TU per form
// with no system header (so MSVC needs no vcvars):
//     WSL gcc 13.3.0        `wsl.exe -e gcc -c -std=c17`     all 9 accept rc=0
//     WSL clang 18.1.3      `wsl.exe -e clang -c -std=c17`   all 9 accept rc=0
//     mingw-w64 gcc 13.2.0  native `gcc -c -std=c17`         all 9 accept rc=0
//     MSVC 19.51.36252      `cl.exe -nologo -c -TC -Fonul`   all 9 accept rc=0
// So refusing any of them is BELOW THE REFERENCE UNION and a conformance defect.
//
// ★★★ BUT ACCEPTING ALL NINE SILENTLY WOULD BE WORSE THAN THE REFUSAL, AND THAT
// IS WHAT THIS FILE PINS. Three of the nine ask for a state DSS does not
// provide, and the difference changes floating-point RESULTS rather than only
// performance. A blanket accept is a silent wrong-numerics program; the refusal
// it replaced was at least visible. The shipped shape is ACCEPTED-WITH-NOTICE:
// the TU compiles (matching every reference) and the unhonoured request is NAMED
// in a WARNING that rides an UNSUPPRESSABLE code, so it can be neither capped
// nor `--suppress`ed away.
//
// ★★ WHICH FORM GETS WHICH VERDICT IS MEASURED AGAINST C23 N3220, NOT ARGUED:
//   FP_CONTRACT      ON/OFF/DEFAULT  satisfied — 7.12.2p2 makes ON a PERMISSION
//                                    ("can be used to ALLOW"), never a
//                                    requirement, so never contracting satisfies
//                                    both polarities; and the DEFAULT state is
//                                    explicitly implementation-defined.
//   FENV_ACCESS      OFF/DEFAULT     satisfied — the permissive state; DEFAULT
//                                    is implementation-defined (7.6.1p2).
//   FENV_ACCESS      ON              DIVERGES — 7.6.1 footnote 248 says the
//                                    pragma exists to disallow CSE, code motion
//                                    and constant folding that could subvert
//                                    flag tests, and DSS's `isCseCandidateOpcode`
//                                    admits FAdd/FMul/FDiv with a call as no
//                                    barrier.
//   CX_LIMITED_RANGE ON              satisfied — 7.3.4p2 says ON licenses "the
//                                    usual mathematical formulas", which is
//                                    exactly what DSS emits.
//   CX_LIMITED_RANGE OFF/DEFAULT     DIVERGES — and DEFAULT diverges for a
//                                    reason the other two pragmas do not share:
//                                    7.3.4p2 MANDATES "The default state for the
//                                    pragma is off", so unlike FP_CONTRACT and
//                                    FENV_ACCESS it is NOT implementation-defined
//                                    and cannot be defined into conformance.
//
// ★ RED-ON-DISABLE. Flip any `standardFloatState` row in `c.lang.json` to
// `unsupported` and its `Accepts*` arm goes red on `hasErrors()`; flip a
// `standardFloatStateDiverges` row to `standardFloatState` and the matching
// `Notices*` arm goes red on the missing warning — which is the mutant that
// matters, because it is the silent-accept this file exists to forbid.

#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/header_name_matching.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dss;
namespace fs = std::filesystem;

// See test_include_bare_relative_includer_dir.cpp's note under
// D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE: a by-value return makes
// `helper()->accessor()` a heap-use-after-free.
[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cSchema() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        // THROW, never abort: abort kills the whole binary and every sibling
        // test loses its verdict.
        if (!loaded.has_value()) throw std::runtime_error{"loadShipped(c) failed"};
        return *loaded;
    }();
    return schema;
}

[[nodiscard]] PreprocessResult pp(std::string text) {
    auto schema = cSchema();
    auto buf = SourceBuffer::fromString(std::move(text), std::string{"stdc.c"});
    std::vector<fs::path> const noDirs;
    return preprocess(buf, schema, noDirs, kDefaultHeaderNameMatching,
                      DiagnosticBudget::libraryDefault());
}

[[nodiscard]] std::string pragmaSource(std::string_view sw,
                                       std::string_view state) {
    return std::string{"#pragma STDC "} + std::string{sw} + " "
         + std::string{state} + "\ndouble f(double a){return a;}\n";
}

// Count the pragma diagnostics at a given severity. Reading SEVERITY rather
// than mere presence is the whole discriminator here: the same code carries the
// loud refusal of a malformed pragma AND the accepted-with-notice warning, so a
// test that only asked "is there a P_PreprocessorPragma?" could not tell the
// shipped behaviour from the defect it replaced.
[[nodiscard]] std::size_t pragmaDiagsAt(PreprocessResult const& r,
                                        DiagnosticSeverity sev) {
    std::size_t n = 0;
    for (auto const& d : r.diagnostics->all())
        if (d.code == DiagnosticCode::P_PreprocessorPragma && d.severity == sev)
            ++n;
    return n;
}

// ── THE SIX FORMS DSS SATISFIES: accepted, and SILENT ──────────────────────
//
// Silence here is a CLAIM, not an omission — a row asserts DSS's behaviour
// already meets the request. A warning on any of these would be crying wolf on
// six of nine forms and would train a reader to ignore the three that matter.

class StdcSatisfied : public testing::TestWithParam<std::pair<const char*, const char*>> {};

TEST_P(StdcSatisfied, IsAcceptedAndSilent) {
    auto const [sw, state] = GetParam();
    PreprocessResult const r = pp(pragmaSource(sw, state));

    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "`#pragma STDC " << sw << " " << state
        << "` is accepted rc=0 by all four references; refusing it is below the "
           "union";
    EXPECT_EQ(pragmaDiagsAt(r, DiagnosticSeverity::Warning), 0u)
        << "this state IS satisfied by DSS's behaviour, so warning about it "
           "would be false — and six false warnings would bury the three real "
           "ones";
}

INSTANTIATE_TEST_SUITE_P(
    AllSatisfiedForms, StdcSatisfied,
    testing::Values(std::pair{"FP_CONTRACT", "ON"},
                    std::pair{"FP_CONTRACT", "OFF"},
                    std::pair{"FP_CONTRACT", "DEFAULT"},
                    std::pair{"FENV_ACCESS", "OFF"},
                    std::pair{"FENV_ACCESS", "DEFAULT"},
                    std::pair{"CX_LIMITED_RANGE", "ON"}));

// ── THE THREE FORMS DSS DOES NOT SATISFY: accepted, and NAMED ──────────────

class StdcDiverges : public testing::TestWithParam<std::pair<const char*, const char*>> {};

TEST_P(StdcDiverges, IsAcceptedButTheDivergenceIsNamed) {
    auto const [sw, state] = GetParam();
    PreprocessResult const r = pp(pragmaSource(sw, state));

    // Half one: the TU still COMPILES. Every reference accepts this form.
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "`#pragma STDC " << sw << " " << state
        << "` must not fail the translation unit — refusing it is below the "
           "reference union, which is exactly the defect this row replaced";

    // Half two: and it is NOT SILENT. This is the assertion that separates
    // accepted-with-notice from the silent accept the bar forbids.
    EXPECT_EQ(pragmaDiagsAt(r, DiagnosticSeverity::Warning), 1u)
        << "DSS does not provide this state, and the difference changes "
           "floating-point RESULTS. Accepting it without a word would be a "
           "silent wrong-numerics program — worse than the refusal it replaced, "
           "because a refusal is at least visible";
}

INSTANTIATE_TEST_SUITE_P(
    AllDivergingForms, StdcDiverges,
    testing::Values(std::pair{"FENV_ACCESS", "ON"},
                    std::pair{"CX_LIMITED_RANGE", "OFF"},
                    // ★ DEFAULT is here and not with the satisfied forms
                    // because C23 7.3.4p2 MANDATES the default state as "off"
                    // for this pragma alone. FP_CONTRACT and FENV_ACCESS have
                    // implementation-defined defaults and sit above.
                    std::pair{"CX_LIMITED_RANGE", "DEFAULT"}));

// ── THE MALFORMED FORMS: still LOUD, and that is deliberate ────────────────

TEST(StdcMalformed, AnUnknownSwitchIsRefused) {
    PreprocessResult const r = pp(pragmaSource("NO_SUCH_THING", "ON"));
    EXPECT_GE(pragmaDiagsAt(r, DiagnosticSeverity::Error), 1u)
        << "no row claims this switch, so it falls to the one-word `STDC` row "
           "and stays unbuilt-and-loud. C23 6.10.6p2 makes a non-standard STDC "
           "pragma undefined behaviour, and guessing at a numerics request is "
           "precisely what must not happen";
}

TEST(StdcMalformed, AMalformedStateIsRefused) {
    PreprocessResult const r = pp(pragmaSource("FP_CONTRACT", "MAYBE"));
    EXPECT_GE(pragmaDiagsAt(r, DiagnosticSeverity::Error), 1u)
        << "`MAYBE` is not an on-off-switch (C23 6.10.8: `one of ON OFF "
           "DEFAULT`)";
}

// ⚠ A MEASURED, DELIBERATE DIVERGENCE, PINNED SO IT IS A DECISION AND NOT AN
// ACCIDENT. ✔MEASURED 2026-08-29: gcc 13.3.0 accepts lowercase `on` SILENTLY and
// clang 18.1.3 accepts it with `expected 'ON' or 'OFF' or 'DEFAULT'` — both
// rc=0. DSS REFUSES. That is not below the union, because the union governs
// CORRECT constructs and C23 6.10.8 spells the grammar `on-off-switch: one of ON
// OFF DEFAULT` — lowercase is malformed by the standard's own text, clang says
// so in its diagnostic, and 6.10.6p2 makes the result undefined behaviour.
TEST(StdcMalformed, ALowercaseStateIsRefusedEvenThoughGccAcceptsIt) {
    PreprocessResult const r = pp(pragmaSource("FP_CONTRACT", "on"));
    EXPECT_GE(pragmaDiagsAt(r, DiagnosticSeverity::Error), 1u)
        << "the C23 grammar is `one of ON OFF DEFAULT`; a lowercase state is a "
           "malformed numerics request, which is the worst kind to guess at";
}

// ── THE CONTROL ────────────────────────────────────────────────────────────
// Without this, every arm above could pass because the fixture never produces a
// pragma diagnostic in the first place.
TEST(StdcControl, ATranslationUnitWithNoPragmaIsSilent) {
    PreprocessResult const r = pp("double f(double a){return a;}\n");
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(pragmaDiagsAt(r, DiagnosticSeverity::Warning), 0u);
    EXPECT_EQ(pragmaDiagsAt(r, DiagnosticSeverity::Error), 0u);
}

}  // namespace
