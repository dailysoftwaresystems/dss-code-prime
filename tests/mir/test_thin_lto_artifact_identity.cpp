// OPT11 — `--lto=thin` MUST BE DETERMINISTIC (D-OPT11-LAZY-IMPORT-EDGE).
//
// `test_lazy_import_optimize.cpp` pins the import MECHANISM on hand-built
// modules. This file pins what that mechanism owes the whole compiler once it is
// wired into the driver, and plan 22 §0.2 names it as THE load-bearing risk of
// the arc, verbatim: *"Per-TU parallel optimization with on-demand imports must
// emit a byte-identical artifact run to run: no iteration-order dependence, no
// address-ordered containers, no first-finisher-wins."*
//
// TWO axes, because they fail differently:
//   * RUN TO RUN — a hash-map iteration or an address-ordered container leaks
//     into a decision, and the same command twice gives two programs.
//   * POOL WIDTH — N importers race, and whichever finishes first wins
//     something it should not have. `SynchronousExecutor` vs a `ThreadPool` is
//     the established shape for that comparison in this repo.
//
// ⚠ WHAT THIS FILE DELIBERATELY DOES **NOT** CLAIM: that `--lto=thin` emits the
// same bytes as `--lto=full`. ✔MEASURED on the 103-TU SQLite corpus, it does
// not — thin performs MORE inlining (its per-TU stage and the whole-program
// stage that follows each carry their own per-caller growth budget), so the two
// topologies emit different, both-correct programs. That is what `-flto` does
// beside `-O2` in every reference compiler, and it is a different question from
// the operator's byte-identity ruling, which is about PREFETCH DEPTH not
// changing the output — `LazyImportEdge.PrefetchDepthChangesTheBatchCountAndNothingElse`
// is where that one is pinned, and it holds.
//
// ★★ NON-VACUITY. A `--lto=thin` that had quietly become a no-op would pass
// every determinism comparison trivially, so each arm also reads
// `PhaseTimers::read(Optimize).runs` — the exact invocation counter three
// driver-supply tests already depend on. Full over N TUs runs N unit optimizes
// plus one whole-program one; thin inserts one MORE per TU that imported
// anything, so a strictly larger count is the witness that the stage ran at all.
//
// ⓘ WHY THIS LIVES UNDER `tests/mir/`. Its SUBJECT is the MIR-tier import stage
// (`mir/summary/lazy_import_optimize.hpp`) and it sits beside the two files that
// pin the same anchor's other halves; it drives the whole driver only because
// determinism of an emitted artifact is not observable any lower down.

#include "core/substrate/phase_timers.hpp"
#include "core/substrate/thread_pool.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "program/cli_args.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// `Location::InsideRepo` for the same reason the compile_pipeline tests use it:
// the schema loader walks UP from cwd to find `src/dss-config/`, and a
// temp-rooted scratch breaks the walk.
fs::path writeC(fs::path const& dir, std::string_view name,
                std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream f(p);
    f << text;
    return p;
}

[[nodiscard]] std::vector<std::uint8_t> readAllBytes(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

// FOUR TUs, one function each, in a chain — the shape that separates a lazy
// edge from an eager plan (see `examples/c/cross_cu_lazy_import_chain/`). Two
// TUs would be green either way and would prove nothing about either claim.
void writeChain(fs::path const& dir) {
    writeC(dir, "main.c",
           "int level1(int x);\nint main(void) { return level1(3); }\n");
    writeC(dir, "a.c",
           "int level2(int x);\nint level1(int x) { return level2(x + 4); }\n");
    writeC(dir, "b.c",
           "int level3(int x);\nint level2(int x) { return level3(x * 3); }\n");
    writeC(dir, "c.c", "int level3(int x) { return x * 2; }\n");
}

std::vector<std::string> chainFiles(fs::path const& dir) {
    return {(dir / "main.c").generic_string(), (dir / "a.c").generic_string(),
            (dir / "b.c").generic_string(), (dir / "c.c").generic_string()};
}

struct Built {
    std::vector<std::uint8_t> bytes;
    std::uint64_t             optimizeRuns = 0;
};

Built compileChain(std::vector<std::string> const& files, dss::LtoModeArg mode,
                   dss::substrate::IExecutor* exec, fs::path const& outDir) {
    dss::substrate::PhaseTimers::reset();
    dss::Program prog;
    prog.setExecutor(exec);
    prog.setOutputDir(outDir);
    // RELEASE, deliberately: `debug` runs no Inlining at all, so a debug build
    // would compare two artifacts neither of which had any cross-CU inlining to
    // get wrong.
    prog.setCompileConfig(dss::CompileConfig::Release);
    prog.setLtoMode(mode);
    int const rc = prog.compileUnits(files, "c", {"x86_64:elf64-x86_64-linux"});
    EXPECT_EQ(rc, 0) << "the 4-TU chain must link and emit an artifact";
    return Built{readAllBytes(outDir / "main.o"),
                 dss::substrate::PhaseTimers::read(
                     dss::substrate::CompilePhase::Optimize).runs};
}

} // namespace

TEST(ThinLto, ArtifactBytesAreIdenticalRunToRun) {
    ScratchDir scratch{Location::InsideRepo, "mir_thin_lto_runs"};
    writeChain(scratch.path());
    scratch.useAsCwd();
    auto const files = chainFiles(scratch.path());

    dss::substrate::SynchronousExecutor sync;
    auto const first =
        compileChain(files, dss::LtoModeArg::Thin, &sync, scratch.path() / "r1");
    auto const second =
        compileChain(files, dss::LtoModeArg::Thin, &sync, scratch.path() / "r2");

    ASSERT_FALSE(first.bytes.empty()) << "the artifact must be non-empty";
    EXPECT_EQ(first.bytes, second.bytes)
        << "two `--lto=thin` builds of the same program must emit the same "
           "bytes — a difference means an iteration order, an address-ordered "
           "container or a first-finisher-wins decision reached the output";
}

TEST(ThinLto, ArtifactBytesAreIndependentOfPoolWidth) {
    ScratchDir scratch{Location::InsideRepo, "mir_thin_lto_pool"};
    writeChain(scratch.path());
    scratch.useAsCwd();
    auto const files = chainFiles(scratch.path());

    dss::substrate::SynchronousExecutor sync;
    dss::substrate::ThreadPool          pool{4};
    auto const seq = compileChain(files, dss::LtoModeArg::Thin, &sync,
                                  scratch.path() / "out_sync");
    auto const par = compileChain(files, dss::LtoModeArg::Thin, &pool,
                                  scratch.path() / "out_pool");

    ASSERT_FALSE(seq.bytes.empty()) << "the artifact must be non-empty";
    EXPECT_EQ(seq.bytes, par.bytes)
        << "N importers run at once and every one of them reads every other "
           "TU's module; the only observable difference that may introduce is "
           "speed, never bytes";
}

// NON-VACUITY for both pins above: without this they would also hold for a thin
// mode that did nothing at all, which is the one way they could stay green while
// the feature was gone.
// ── the two shapes whose symbols are NEITHER defined NOR importable ────────
//
// `declareUndefinedReferences` turns every reference the post-import module
// cannot satisfy into an ordinary cross-TU `extern`, and ABORTS on one it cannot
// name — which is right, because a module that reaches codegen with a dangling
// reference is worse than a stopped build. Two kinds of symbol are neither
// defined nor externable and would trip that abort if they were not recognized:
//
//   * A BLOCK-EXPORT symbol — anonymous by construction, defined by its own
//     `BlockAddressExport` instruction, and REACHED through a static
//     initializer's relocation, which is exactly where the scan finds it. This
//     is the P37 jump-table shape ([[D-LINK-MERGE-DOES-NOT-REMAP-BLOCK-SYMBOLS]])
//     and it cannot fire below two CUs, which is why both fixtures are 2-TU.
//   * A REFERENCED-ONLY SHIPPED-LIBRARY SHIM — on pe64 `printf` has no
//     definition and no import row until the post-merge synthesis creates one.
//
// These pins assert only that the build SUCCEEDS, and that is the whole point:
// the failure they guard is a hard abort, so a regression takes the test binary
// down rather than producing a wrong answer to compare.
TEST(ThinLto, CompilesACrossCuComputedGotoTable) {
    ScratchDir scratch{Location::InsideRepo, "mir_thin_lto_goto"};
    writeC(scratch.path(), "main.c",
           "int dispatch(int k);\nint main(void) { return dispatch(3); }\n");
    // A DENSE, WIDE switch in a SEPARATE CU: dense lowers to a jump TABLE (a
    // data item whose relocations name synthetic block symbols); a sparse one
    // lowers to a compare chain, carries no data item, and would make this
    // fixture green for the wrong reason.
    writeC(scratch.path(), "dispatch.c",
           "int dispatch(int k) {\n"
           "  switch (k) {\n"
           "    case 0: return 10; case 1: return 20; case 2: return 30;\n"
           "    case 3: return 40; case 4: return 50; case 5: return 60;\n"
           "    case 6: return 70; case 7: return 80; case 8: return 90;\n"
           "    case 9: return 100; case 10: return 110; case 11: return 120;\n"
           "    default: return -1;\n"
           "  }\n"
           "}\n");
    scratch.useAsCwd();
    std::vector<std::string> const files{
        (scratch.path() / "main.c").generic_string(),
        (scratch.path() / "dispatch.c").generic_string()};

    dss::substrate::SynchronousExecutor sync;
    dss::substrate::PhaseTimers::reset();
    dss::Program prog;
    prog.setExecutor(&sync);
    prog.setOutputDir(scratch.path() / "out");
    prog.setCompileConfig(dss::CompileConfig::Release);
    prog.setLtoMode(dss::LtoModeArg::Thin);
    EXPECT_EQ(prog.compileUnits(files, "c", {"x86_64:pe64-x86_64-windows"}), 0)
        << "a jump table's block symbols are DEFINED by their own instruction; "
           "treating one as an undefined reference stops a well-formed build";
}

TEST(ThinLto, CompilesAMultiTuProgramThatCallsAShippedLibraryShim) {
    ScratchDir scratch{Location::InsideRepo, "mir_thin_lto_shim"};
    // ★ THE IMPORTER IS THE TU THAT USES THE SHIM, AND THE IMPORTED BODY IS THE
    // ONE THAT DOES NOT. That asymmetry is the whole fixture: the guard runs
    // only on a module that ACTUALLY IMPORTED something, so a program where the
    // shim-using TU imports nothing never reaches it and would be green either
    // way. `add2` carries no shim, so it is importable; `main` carries `printf`,
    // so the post-import module holds a `GlobalAddr` to a symbol that is neither
    // defined nor externable until the post-merge synthesis creates it.
    writeC(scratch.path(), "main.c",
           "#include <stdio.h>\nint add2(int x);\n"
           "int main(void) { printf(\"a\\n\"); return add2(40); }\n");
    writeC(scratch.path(), "add2.c", "int add2(int x) { return x + 2; }\n");
    scratch.useAsCwd();
    std::vector<std::string> const files{
        (scratch.path() / "main.c").generic_string(),
        (scratch.path() / "add2.c").generic_string()};

    dss::substrate::SynchronousExecutor sync;
    dss::substrate::PhaseTimers::reset();
    dss::Program prog;
    prog.setExecutor(&sync);
    prog.setOutputDir(scratch.path() / "out");
    prog.setCompileConfig(dss::CompileConfig::Release);
    prog.setLtoMode(dss::LtoModeArg::Thin);
    EXPECT_EQ(prog.compileUnits(files, "c", {"x86_64:pe64-x86_64-windows"}), 0)
        << "a referenced-only shim is defined AFTER the whole-program merge; "
           "declaring it as an import would emit a row for a symbol DSS is "
           "about to define itself";
}

TEST(ThinLto, ThinRunsMoreOptimizeInvocationsThanFull) {
    ScratchDir scratch{Location::InsideRepo, "mir_thin_lto_ran"};
    writeChain(scratch.path());
    scratch.useAsCwd();
    auto const files = chainFiles(scratch.path());

    dss::substrate::SynchronousExecutor sync;
    auto const full = compileChain(files, dss::LtoModeArg::Full, &sync,
                                   scratch.path() / "out_full");
    auto const thin = compileChain(files, dss::LtoModeArg::Thin, &sync,
                                   scratch.path() / "out_thin");

    EXPECT_GT(thin.optimizeRuns, full.optimizeRuns)
        << "`--lto=thin` must run MORE optimize invocations than `--lto=full` "
           "— one extra per TU that imported a body. An equal count means the "
           "import stage never ran, and every determinism pin beside this one "
           "would then be comparing two identical no-ops";
}
