// D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE — the END-TO-END pin.
//
// The row's own defect is an operator raising the cap and still being shown
// 1000, so the closing evidence has to be exactly that, driven through the real
// driver: a translation unit that genuinely trips a SUB-TIER cap, compiled at
// the default and again with `--max-diagnostics` raised, with the cap notice
// read back out of the run-wide reporter.
//
// ★ WHY THIS IS NOT A UNIT TEST ON `Config`. A `Config` assertion proves a
// struct holds a number. It cannot see the defect, which was that the number
// stopped at `Program::compileFiles` and every tier below it built its own
// reporter from the library's in-class initializers. Only a real compile can
// show the notice naming a value the operator never chose.
//
// ★ WHY THE INPUT LOOKS LIKE THIS. A sub-tier's GLOBAL cap is unreachable
// through ordinary diagnostics: `report()` checks `maxPerCode` (50) before
// `maxDiagnostics` (1000), so an ordinary tier is bounded at
// (distinct codes x 50). ✔MEASURED through the shipped CLI: 1400 illegal
// characters reach stderr as 100 diagnostics, 1400 malformed declarations as
// 53, 1400 `#warning` directives as 50 — not one of them trips a global cap at
// any flag value. The ONLY traffic that grows a tier's `all_` past 1000 is what
// `mustDeliver` admits ahead of all four volume gates: `Guaranteed` delivery
// and `isUnsuppressable(code)` members. `S_StaticAssertFailed` is such a
// member, so 1400 failing `_Static_assert`s fill the semantic tier's budget,
// and the ordinary diagnostics that arrive next are dropped by a cap the
// operator could not raise. That is the shape of the input below, and it is the
// exact shape MEASURED producing the defect before the fix.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// 1400 unsuppressable diagnostics (they bypass `maxPerCode` and fill the tier's
// GLOBAL budget) followed by 60 ordinary ones (which the tier then drops).
constexpr std::size_t kUnsuppressableLines = 1400;
constexpr std::size_t kOrdinaryLines       = 60;

[[nodiscard]] std::string capTrippingSource() {
    std::ostringstream os;
    os << "int main(void) { return 0; }\n";
    for (std::size_t i = 0; i < kUnsuppressableLines; ++i) {
        os << "_Static_assert(0, \"sa" << i << "\");\n";
    }
    for (std::size_t i = 0; i < kOrdinaryLines; ++i) {
        os << "int trip" << i << " = undefined_symbol_" << i << ";\n";
    }
    return os.str();
}

// The cap marker's own statement of the limit it enforced. `noteCapDrop_`
// formats `cfg_.maxDiagnostics` into this text, so the number here is whichever
// reporter capped FIRST — which is precisely what the defect made wrong.
[[nodiscard]] std::optional<std::string> capNotice(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::P_TooManyDiagnostics) return d.actual;
    }
    return std::nullopt;
}

[[nodiscard]] bool mentions(std::string const& hay, std::string const& needle) {
    return hay.find(needle) != std::string::npos;
}

[[nodiscard]] std::string compileAndCapture(fs::path const& srcFile,
                                            DiagnosticReporter& rep) {
    Program prog;
    (void)prog.compileFiles({srcFile.generic_string()}, "c-subset",
                            {"x86_64:elf64-x86_64-linux"}, rep);
    auto const notice = capNotice(rep);
    return notice ? *notice : std::string{"<NO CAP NOTICE>"};
}

[[nodiscard]] DiagnosticReporter::Config configWithCap(std::size_t maxDiagnostics) {
    DiagnosticReporter::Config cfg{};   // the shipped defaults are the base…
    cfg.maxDiagnostics = maxDiagnostics;  // …and only the operator's axis moves
    return cfg;
}

}  // namespace

// ── THE ROW'S CLOSING EVIDENCE ─────────────────────────────────────────────
//
// Both halves matter and they are asserted together, because either one alone
// is satisfiable by a broken implementation: "the raised run names 1200" is
// also true of a build that ignores the flag and happens to cap at 1200
// everywhere, and "the default run names 1000" is true of the defect itself.
TEST(MaxDiagnosticsReachesEveryTier, RaisedCapIsWhatTheOperatorIsShown) {
    ScratchDir scratch{Location::InsideRepo, "maxdiag"};
    fs::path const src = scratch.path() / "cap_tripping.c";
    {
        std::ofstream out{src, std::ios::binary};
        ASSERT_TRUE(out.good());
        out << capTrippingSource();
    }
    scratch.useAsCwd();

    // ── at the shipped default, the input must STILL cap ───────────────────
    // Without this half the raised-cap assertion proves nothing: an input that
    // stopped tripping any cap would satisfy it vacuously.
    DiagnosticReporter defaultRep{DiagnosticReporter::Config{}};
    std::string const atDefault = compileAndCapture(src, defaultRep);
    EXPECT_TRUE(mentions(atDefault, "reporter cap of 1000 diagnostics reached"))
        << "the corpus no longer trips a cap at the shipped default, so the "
           "raised-cap half below would pass vacuously. Notice was: "
        << atDefault;

    // ── raised, the operator must be shown THEIR number ────────────────────
    // 1200 is above the tiers' old hard-coded 1000 and below the corpus's 1400
    // unsuppressable diagnostics, so a tier that honours the budget caps at
    // 1200 and one that does not still caps at 1000. That gap is the only
    // direction in which threading is observable at all: below 1000 the
    // run-wide reporter enforces the same number a moment later and the output
    // is identical either way.
    DiagnosticReporter raisedRep{configWithCap(1200)};
    std::string const atRaised = compileAndCapture(src, raisedRep);
    EXPECT_TRUE(mentions(atRaised, "reporter cap of 1200 diagnostics reached"))
        << "an operator who raised the cap to 1200 was shown a different "
           "number — a tier below the run-wide reporter is still enforcing its "
           "own compiled-in limit "
           "(D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE). Notice was: "
        << atRaised;
    EXPECT_FALSE(mentions(atRaised, "reporter cap of 1000 diagnostics reached"))
        << "the run raised to 1200 still names 1000 — this is the defect "
           "verbatim: the operator does what the notice tells them to do and "
           "the notice does not change. Notice was: "
        << atRaised;

    // And the remedy the notice names must be the one that moved it.
    EXPECT_TRUE(mentions(atRaised, "--max-diagnostics=N"))
        << "the cap notice stopped naming the flag that fixes it";
}

// ── THE OTHER DRIVER HOP: program.cpp → UnitBuilder ───────────────────────
//
// A SECOND, INDEPENDENT LEVER, and it needs its own input. The test above
// travels `rep` → `CompileOptions::diagBudget` → `analyze`, so it reds when
// that chain breaks and stays green when the CU-tier hop
// (`UnitBuilder builder{grammar, DiagnosticBudget{rep.config()}}`) does. This
// one travels `rep` → `UnitBuilder` → `Tokenizer`, and it reds for the
// opposite mutation. Two hops, two pins — one pin over both is the multi-site
// contract trap this row exists to record.
//
// It raises `maxPerCode`, not `maxDiagnostics`, because that is the axis that
// actually bounds a lexer tier: 120 illegal characters all share ONE code, so
// the per-code cap of 50 is the only thing that can truncate them, and the
// global cap of 1000 is never in play. (The CLI exposes only
// `--max-diagnostics` today, which is why this pin drives the rep-injection
// overload — the same overload D-CAP-MARKER-MULTI-TARGET-E2E-PIN added.)
TEST(MaxDiagnosticsReachesEveryTier, RaisedPerCodeBudgetReachesTheCompilationUnitTier) {
    constexpr std::size_t kIllegalCharLines = 120;  // > the library's maxPerCode of 50

    ScratchDir scratch{Location::InsideRepo, "maxdiag-cu"};
    fs::path const src = scratch.path() / "illegal_chars.c";
    {
        std::ofstream out{src, std::ios::binary};
        ASSERT_TRUE(out.good());
        out << "int main(void) { return 0; }\n";
        for (std::size_t i = 0; i < kIllegalCharLines; ++i) {
            out << "int v" << i << " = 1 ` 2;\n";
        }
    }
    scratch.useAsCwd();

    DiagnosticReporter::Config cfg{};   // shipped defaults as the base…
    cfg.maxPerCode  = 400;              // …raised above the library's 50
    cfg.dedupWindow = 0;                // so identical-shaped diagnostics all land
    DiagnosticReporter rep{cfg};

    Program prog;
    (void)prog.compileFiles({src.generic_string()}, "c-subset",
                            {"x86_64:elf64-x86_64-linux"}, rep);

    std::size_t illegal = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::P_IllegalChar) ++illegal;
    }
    EXPECT_EQ(illegal, kIllegalCharLines)
        << "the front end truncated a per-code stream the operator had budgeted "
           "for — the compilation-unit tier is building its reporters from the "
           "library defaults instead of from the budget program.cpp handed the "
           "UnitBuilder (D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE)";
}
