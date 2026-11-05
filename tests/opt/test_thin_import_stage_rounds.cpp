// THE THIN-LTO IMPORT STAGE OF A CASCADE, THROUGH THE DRIVER (P69 H6; P2 of the plan for
// [[D-OPT-RELEASE-UNIT-FIXPOINT-CAP-WARNS-THOUGH-THE-PROGRAM-STAGE-RESUMES]]; the orchestrator's ledgers 500 and 506).
//
// The import fixpoint's bound (`kMaxImportRounds`) is an INVARIANT, not a truncation: the closure takes every direct
// callee a module names, transitively, before the first optimize, and a callee whose address escapes is never
// importable (`isInlineCandidate`'s gate rule 4), so an outer round can find no demand the first did not take, and
// each importing unit's import stage converges in ONE round. Reaching the bound means the closure rule is broken -- a
// compiler defect, X_OptImportBoundReached (Error), the stage stopping (`importBoundReached`, compile_pipeline.hpp).
// What is pinned here, `--config=release --lto=thin` with every warning an error, on three object targets:
//   * ★ THE MEASUREMENT THE INVARIANT RESTS ON: over cascades of units -- a ten-unit chain of direct calls, a chain of
//     returned function pointers an optimize devirtualizes, a constant table of them -- the build succeeds, nothing
//     reports a truncation or the bound, and the import stage runs ONE round per importing unit. A cascade that runs a
//     unit past one round refutes the invariant (an optimize exposed demand the closure never took), and this case is
//     red with the counts -- a closure that took only the names a module itself calls reaches the bound on the
//     direct chain, and that red is the bound's tripwire proven.
//   * the bound's report itself: reached, it stops the stage, and a suppressed report still says so.
//
// The rounds are counted through the stage seam (`pipeline_testing::setStageObserver`): one ThinImport-stage optimize
// per round of a unit that imported something, so the count over a cascade is its importing units, each once.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "diagnostic_count.hpp"
#include "program/compile_pipeline.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace dss;
using dss::test_support::countCode;
namespace fs = std::filesystem;

namespace {

struct Cascade {
    char const*                                       what;
    std::vector<std::pair<std::string, std::string>> units;   // file name, source
    std::size_t                                       importingUnits;   // units whose closure takes a body
};

[[nodiscard]] std::vector<Cascade> cascades() {
    std::vector<Cascade> out;
    {
        // main -> f1 -> f2 -> ... -> f10, one function per unit: every unit but f10's imports its callee.
        Cascade c{"a ten-unit chain of direct calls", {}, 10};
        for (int k = 1; k <= 10; ++k) {
            c.units.emplace_back(std::format("f{}.c", k),
                                 k == 10 ? std::string{"int f10(int x) { return x; }\n"}
                                         : std::format("int f{0}(int x);\nint f{1}(int x) {{ return f{0}(x) + 1; }}\n",
                                                       k + 1, k));
        }
        c.units.emplace_back("main.c", "int f1(int x);\nint main(void) { return f1(1) == 10 ? 0 : 1; }\n");
        out.push_back(std::move(c));
    }
    {
        // Each unit hands out the address of an EXTERNAL function of its own; main calls through each. The getters
        // are imported (their own addresses never escape, and what they reference is linkable), and inlined, so the
        // optimize turns each call through a returned pointer into a direct call of `hk` -- whose address escapes, so
        // it is no demand. ⚠ A `static` `hk` makes the getter's body reference its unit's private symbol, which is
        // never importable: main then imports nothing and the cascade measures nothing (✔MEASURED, P69 H6, the first
        // cut of this case, macos-arm64-debug: 0 rounds on all three targets).
        Cascade c{"a chain of returned function pointers", {}, 1};
        std::string main = "typedef int (*fn)(int);\n";
        std::string sum  = "0";
        for (int k = 1; k <= 10; ++k) {
            c.units.emplace_back(std::format("g{}.c", k),
                                 std::format("typedef int (*fn)(int);\nint h{0}(int x) {{ return x + {0}; }}\n"
                                             "fn get{0}(void) {{ return h{0}; }}\n",
                                             k));
            main += std::format("fn get{}(void);\n", k);
            sum += std::format(" + get{}()(0)", k);
        }
        main += "int main(void) { return (" + sum + ") == 55 ? 0 : 1; }\n";
        c.units.emplace_back("main.c", main);
        out.push_back(std::move(c));
    }
    {
        // A constant table of function pointers in one unit, read through by another: no direct callee at all.
        Cascade c{"a constant table of function pointers", {}, 0};
        c.units.emplace_back("table.c",
                             "static int t1(int x) { return x + 1; }\nstatic int t2(int x) { return x + 2; }\n"
                             "int (*const table[2])(int) = {t1, t2};\n");
        c.units.emplace_back("main.c",
                             "extern int (*const table[2])(int);\n"
                             "int main(void) { return table[0](1) + table[1](2) == 6 ? 0 : 1; }\n");
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace

TEST(ThinImportStageRounds, EveryCascadesImportStageConvergesInOneRoundAndReportsNothing) {
    for (char const* target : {"x86_64:elf64-x86_64-linux", "x86_64:pe64-x86_64-windows", "arm64:elf64-aarch64-linux"}) {
        for (Cascade const& cascade : cascades()) {
            SCOPED_TRACE(std::string{cascade.what} + " on " + target);
            dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "thin-import-rounds"};
            std::vector<std::string>      sources;
            for (auto const& [name, text] : cascade.units) {
                fs::path const p = scratch.path() / name;
                std::ofstream{p, std::ios::binary} << text;
                sources.push_back(p.generic_string());
            }
            Program prog;
            prog.setOutputDir(scratch.path() / "out");
            prog.setCompileConfig(CompileConfig::Release);
            prog.setLtoMode(LtoModeArg::Thin);
            std::size_t importRounds = 0;
            std::size_t programRuns  = 0;
            pipeline_testing::setStageObserver([&](StageEvent const& e) {
                if (e.kind != StageEvent::Kind::Optimized) return;
                if (e.stage == PipelineStage::ThinImport) ++importRounds;
                if (e.stage == PipelineStage::Program) ++programRuns;
            });
            // EVERY REPORT KEPT, and every warning an error: a truncation the import stage reported would fail the
            // build here, and none is dropped as a recent duplicate.
            DiagnosticReporter::Config every;
            every.maxDiagnostics          = std::numeric_limits<std::size_t>::max();
            every.maxPerCode              = std::numeric_limits<std::size_t>::max();
            every.dedupWindow             = 0;
            every.policy.warningsAsErrors = true;
            DiagnosticReporter rep{every};
            int const          rc = prog.compileUnits(sources, "c", {target}, rep);
            pipeline_testing::setStageObserver({});
            std::string said;
            for (auto const& d : rep.all()) said += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
            std::string const counts = std::format("the import stage ran {} round(s) over {} importing unit(s)",
                                                   importRounds, cascade.importingUnits);
            ASSERT_EQ(rc, 0) << "the cascade did not build under --lto=thin with warnings as errors; " << counts << ":"
                             << said;
            EXPECT_EQ(countCode(rep, DiagnosticCode::X_OptFixpointTruncated), 0u) << said;
            EXPECT_EQ(countCode(rep, DiagnosticCode::X_OptImportBoundReached), 0u) << said;
            EXPECT_GE(programRuns, 1u) << "no Program stage ran: the route measured nothing";
            EXPECT_EQ(importRounds, cascade.importingUnits)
                << counts
                << (importRounds > cascade.importingUnits
                        ? ": a unit ran more than one round, so an optimize exposed demand the closure never took -- "
                          "the import fixpoint's invariant does not hold"
                        : ": fewer rounds than importing units, so a unit the case says imports imported nothing -- the "
                          "cascade no longer measures what it states");
        }
    }
}

// The bound's one report: a unit that reached it stops the stage and is reported as the compiler defect it is, and
// neither a `--suppress` of the code nor a demotion of it changes the answer the stage stops on -- a suppressed Error
// is still reported (DiagnosticPolicy::suppress drops warnings and notes only). A unit that did not reach it reports
// nothing and stops nothing (the control).
TEST(ThinImportStageRounds, AUnitAtTheImportBoundStopsTheStageAndASuppressedReportStillSaysSo) {
    {
        DiagnosticReporter rep;
        EXPECT_FALSE(importBoundReached(0, 0, rep)) << "a unit with no demand left stopped the stage";
        EXPECT_TRUE(rep.all().empty()) << "a unit with no demand left reported something";
    }
    {
        DiagnosticReporter rep;
        EXPECT_TRUE(importBoundReached(3, 2, rep)) << "a unit at the import bound did not stop the stage";
        ASSERT_EQ(countCode(rep, DiagnosticCode::X_OptImportBoundReached), 1u);
        EXPECT_EQ(rep.errorCount(), 1u) << "the bound was not reported as an Error";
        auto const& d = rep.all().front();
        EXPECT_NE(d.actual.find("module #3 still had 2 importable callee(s)"), std::string::npos) << d.actual;
        EXPECT_NE(d.actual.find("defect of the compiler's import closure"), std::string::npos) << d.actual;
    }
    {
        DiagnosticReporter::Config suppressing;
        suppressing.policy.suppress.insert(DiagnosticCode::X_OptImportBoundReached);
        DiagnosticReporter rep{suppressing};
        EXPECT_TRUE(importBoundReached(3, 2, rep)) << "a suppressed report changed what the stage stops on";
        EXPECT_EQ(countCode(rep, DiagnosticCode::X_OptImportBoundReached), 1u)
            << "a --suppress silenced the Error: the stage would stop with nothing said";
        EXPECT_GE(rep.errorCount(), 1u);
    }
    {
        DiagnosticReporter::Config demoting;
        demoting.policy.overrides.emplace(DiagnosticCode::X_OptImportBoundReached, DiagnosticSeverity::Info);
        DiagnosticReporter rep{demoting};
        EXPECT_TRUE(importBoundReached(3, 2, rep)) << "a demoted report changed what the stage stops on";
    }
}
