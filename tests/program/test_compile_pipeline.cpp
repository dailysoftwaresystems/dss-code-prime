// Driver pipeline tests — plan 14 LK10 cycle 2.
//
// Pins:
//   * `Program::compileFiles` runs c-subset source through
//     HIR→MIR→LIR→regalloc→callconv→ASM→link→writeImage and
//     commits valid ELF bytes to `<cwd>/target/<formatName>/<stem>.o`.
//   * Fail-loud surfaces emit the right D_* code on:
//     - empty source list                           (D_EmptyInput)
//     - empty targets list                          (D_InvalidTargetSpec)
//     - malformed `targets[i]`                      (D_InvalidTargetSpec)
//     - unknown language schema                     (D_SchemaLoadFailed)
//     - unknown target schema                       (D_SchemaLoadFailed)
//     - unknown format schema                       (D_SchemaLoadFailed)
//     - compileProject  (plan 6 unstarted)          (D_PlanNotLanded)
//     - compileDirectory on a missing dir           (D_FileNotFound)
//     - compileDirectory with no matching files     (D_EmptyInput)
//   * Format-blindness: same source compiles to ELF.o AND PE.obj
//     and BOTH artifacts land in distinct subdirs.
//
// ML7 callconv lowering of the virtual `arg` op (plan 12 cycle 2)
// is the gating constraint for non-zero-arg functions — the
// acceptance pin uses `int forty_two() { return 42; }` (zero arg).
// Anchored at plan 14 §3.1 D-LK10-2 for closure when ML7 cycle 2
// lands.

#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;

namespace {
// ScratchDir hoisted to `tests/test_support/scratch_dir.hpp` at
// D-LK10-6 closure (2026-06-01). Use `Location::InsideRepo` here
// because compile_pipeline tests drive `compileFiles`, whose
// schema loader walks UP from cwd to find `src/dss-config/`; a
// temp-rooted scratch would break the walk.
using dss::test_support::Location;
using dss::test_support::ScratchDir;

// Not nodiscard — some call sites only care about the side effect.
fs::path writeCSubsetSource(fs::path const& dir,
                             std::string_view name,
                             std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream f(p);
    f << text;
    return p;
}

} // namespace

// ── compileFiles: end-to-end wiring ─────────────────────────────
//
// **Cycle 2 acceptance scope (D-LK10-2)**: these tests pin the
// DRIVER WIRING — that compileFiles loads schemas, builds a CU,
// drives the pipeline through every tier, and invokes link +
// writeImage on success. They do NOT pin byte-correctness of the
// emitted artifact, because two upstream gaps remain open:
//
//   * Plan 12 ML7 cycle 2 — virtual `arg` pseudo-op lowering to
//     concrete arg-register moves (only matters for non-zero-arg
//     functions; zero-arg `int forty_two()` does NOT trip this).
//   * Plan 13 AS cycles — `load` / `store` / `ret` operand-kind
//     variants and stack-frame prologue/epilogue opcode coverage
//     in `x86_64.target.json` (the assembler currently emits
//     `A_NoEncodingDeclared` / `A_NoMatchingEncodingVariant`
//     diagnostics for the frame_load/frame_store ops the
//     calling-convention pass materializes for ANY function).
//
// Wiring proof: the output target directory IS created (the driver
// does `fs::create_directories` BEFORE invoking the pipeline) iff
// both schemas loaded and the target spec parsed cleanly. When the
// upstream gaps close, the same tests tighten by asserting bytes —
// the directory check is necessary, the byte check is the future
// pin (anchored D-LK10-2).

TEST(Program_CompileFiles, ZeroArgFunctionWiresThroughPipeline) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "forty_two.c",
        "int forty_two() { return 42; }\n");
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileFiles(
        {src.generic_string()},
        "c-subset",
        {"x86_64:elf64-x86_64-linux"});

    // D-LK10-2 closed 2026-06-01 (commit `a22286f`). The full driver
    // pipeline now produces a real ELF .o file with `\x7fELF` magic
    // for a zero-arg c-subset function. Plan 12 ML7 cycle 2 closed
    // the IR-tier half (arg + call + ret virtual-op materialization);
    // plan 13 AS cycle (D-AS4-1 partial close — `[base+disp32]` form)
    // closed the encoder half (load/store with `[base+disp32]`
    // addressing + add/sub reg+imm32 for prologue/epilogue SP
    // adjustment). The remaining D-AS4-1 sub-items (`lea` encoding,
    // indexed/scaled addressing — D-AS4-5, Disp8 form) are unrelated
    // to the c-subset zero-arg corpus and stay deferred.
    auto const outDir = scratch.path() / "target" / "elf64-x86_64-linux";
    ASSERT_TRUE(fs::is_directory(outDir));
    ASSERT_EQ(rc, 0)
        << "byte-on-disk e2e must succeed post-D-LK10-2 closure";

    auto const out = outDir / "forty_two.o";
    ASSERT_TRUE(fs::exists(out));
    ASSERT_GT(fs::file_size(out), 0u);
    std::ifstream in(out, std::ios::binary);
    char hdr[4] = {0};
    in.read(hdr, 4);
    EXPECT_EQ(static_cast<unsigned char>(hdr[0]), 0x7Fu);
    EXPECT_EQ(hdr[1], 'E');
    EXPECT_EQ(hdr[2], 'L');
    EXPECT_EQ(hdr[3], 'F');
}

TEST(Program_CompileFiles, MultiTargetWiresDistinctArtifactDirs) {
    // Format-blindness wiring pin: ONE source, TWO targets, TWO
    // distinct output directories. Independently of byte
    // production, the driver's per-target loop must create one
    // output dir per target.
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "small.c",
        "int small() { return 7; }\n");
    scratch.useAsCwd();

    Program prog;
    prog.compileFiles({src.generic_string()},
                       "c-subset",
                       {"x86_64:elf64-x86_64-linux",
                        "x86_64:pe64-x86_64-windows"});
    EXPECT_TRUE(fs::is_directory(
        scratch.path() / "target" / "elf64-x86_64-linux"));
    EXPECT_TRUE(fs::is_directory(
        scratch.path() / "target" / "pe64-x86_64-windows"));
}

// ── compileFiles: fail-loud surfaces ──────────────────────────

TEST(Program_CompileFiles, EmptySourceListReturnsNonZero) {
    Program prog;
    EXPECT_EQ(prog.compileFiles({}, "c-subset",
                                {"x86_64:elf64-x86_64-linux"}),
              1);
}

TEST(Program_CompileFiles, EmptyTargetListReturnsNonZero) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "f.c", "int f() { return 0; }\n");
    Program prog;
    EXPECT_EQ(prog.compileFiles({src.generic_string()},
                                "c-subset", {}),
              1);
}

TEST(Program_CompileFiles, MalformedTargetSpecReturnsNonZero) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "f.c", "int f() { return 0; }\n");
    scratch.useAsCwd();

    Program prog;
    // No colon → unparseable.
    EXPECT_EQ(prog.compileFiles({src.generic_string()},
                                "c-subset",
                                {"badspec"}),
              1);
    // Empty half → unparseable.
    EXPECT_EQ(prog.compileFiles({src.generic_string()},
                                "c-subset",
                                {":elf64-x86_64-linux"}),
              1);
}

TEST(Program_CompileFiles, UnknownLanguageReturnsNonZero) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "f.txt", "ignored\n");
    Program prog;
    EXPECT_EQ(prog.compileFiles({src.generic_string()},
                                "no-such-language",
                                {"x86_64:elf64-x86_64-linux"}),
              1);
}

TEST(Program_CompileFiles, UnknownTargetNameReturnsNonZero) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "f.c", "int f() { return 0; }\n");
    scratch.useAsCwd();
    Program prog;
    EXPECT_EQ(prog.compileFiles({src.generic_string()},
                                "c-subset",
                                {"noarch:elf64-x86_64-linux"}),
              1);
}

TEST(Program_CompileFiles, UnknownFormatNameReturnsNonZero) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "f.c", "int f() { return 0; }\n");
    scratch.useAsCwd();
    Program prog;
    EXPECT_EQ(prog.compileFiles({src.generic_string()},
                                "c-subset",
                                {"x86_64:no-such-format"}),
              1);
}

TEST(Program_CompileFiles, NonExistentSourceFileReturnsNonZero) {
    // pr-test-analyzer FOLD-NOW: a source file path that doesn't
    // exist is the archetype silent-failure-hunter case for the
    // post-fold CU-diagnostic drain. `UnitBuilder::addFile` emits
    // `D_FileNotFound` into the CU's driver-level reporter; that
    // diagnostic MUST reach the operator via the run-wide reporter
    // (code-reviewer F1 fold).
    ScratchDir scratch{Location::InsideRepo, "program"};
    scratch.useAsCwd();
    Program prog;
    EXPECT_EQ(prog.compileFiles({"/no/such/ghost-source-file.c"},
                                "c-subset",
                                {"x86_64:elf64-x86_64-linux"}),
              1);
}

// ── Gap-C cap-relax: e2e pin (2026-06-01) ─────────────────────
// Pins that multi-target compile returns non-zero on per-target
// errors. NOTE: the original framing of this as a "single
// P_TooManyDiagnostics marker pin" was a false pin —
// D_TargetMachineCodeMismatch is in `kUnsuppressableCodes` so it
// bypasses cap gates entirely; no marker would fire either way.
// The actual single-chokepoint contract is pinned at the unit
// layer in test_diagnostic_reporter.cpp; here we pin only the
// exit-code surface (which still validates that multi-target
// failures aggregate correctly through compileFiles → merge → exit).
// Anchored D-CAP-MARKER-MULTI-TARGET-E2E-PIN for when compileFiles
// exposes rep (or a suppressible per-target emitter exists).
TEST(Program_CompileFiles, MultiTargetMismatchAggregatesToNonZeroExit) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    scratch.useAsCwd();
    auto src = scratch.path() / "x.c";
    {
        std::ofstream out{src};
        out << "int main(void) { return 0; }\n";
    }
    Program prog;
    int const rc = prog.compileFiles(
        {src.string()},
        "c-subset",
        {"x86_64:elf64-aarch64-linux",
         "x86_64:elf64-aarch64-linux"});  // duplicate intentional
    EXPECT_EQ(rc, 1) << "multi-target compile with errors must exit 1";
}

// ── D-LK6-8.2 pr-test-analyzer Gap 5 P9: cross-validate wired ──
// Pins that crossValidateTargetFormat IS INVOKED from the compile
// pipeline (program.cpp call site between schema-load and
// compileSingleUnit). Without this, a refactor could quietly remove
// the call and every cross-validation case would silently pass
// through to compileSingleUnit — exactly the silent-failure surface
// D-LK6-8.2 was anchored to close. Pair (target=x86_64,
// format=elf64-aarch64-linux): the schemas load individually, but
// the (62 vs 183) elf.machine mismatch trips cross-validate and
// compileFiles returns non-zero.
TEST(Program_CompileFiles, CrossValidateRejectsMachineMismatch) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    scratch.useAsCwd();
    auto src = scratch.path() / "x.c";
    {
        std::ofstream out{src};
        out << "int main(void) { return 0; }\n";
    }
    Program prog;
    EXPECT_EQ(prog.compileFiles({src.string()},
                                "c-subset",
                                {"x86_64:elf64-aarch64-linux"}),
              1);
}

// ── compileProject: plan 06 fail-loud stub ────────────────────

TEST(Program_CompileProject, FailsLoudPlanNotLanded) {
    Program prog;
    EXPECT_EQ(prog.compileProject("any.dsp"), 1);
}

// H2 behavioral pin (silent-failure audit post-fold #2): even with
// --suppress=D_PlanNotLanded, compileProject + transpile must return
// non-zero. The suppress hides the stderr message but MUST NOT
// absorb the "the operation didn't happen" signal into a silent
// success exit. Without this, build systems downstream of
// dss-code-prime treat exit 0 as "transpile happened" and consume
// nonexistent outputs.
TEST(Program_CompileProject, SuppressedPlanNotLandedStillReturnsNonZero) {
    Program prog;
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::D_PlanNotLanded);
    EXPECT_EQ(prog.compileProject("any.dsp", cfg), 1);
}

TEST(Program_Transpile, FailsLoudPlanNotLanded) {
    Program prog;
    EXPECT_EQ(
        prog.transpile({"in.c"}, "c-subset", {"x86_64-v1-link-elf"}),
        1);
}

TEST(Program_Transpile, SuppressedPlanNotLandedStillReturnsNonZero) {
    Program prog;
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::D_PlanNotLanded);
    EXPECT_EQ(
        prog.transpile({"in.c"}, "c-subset", {"x86_64-v1-link-elf"}, cfg),
        1);
}

// D-FF2-UNSUPP gate pin 2026-06-01: pins that the unsuppressable
// gate keeps `H_ExternHasInitializer` visible through the full
// post-CLI pipeline even when `--suppress=H_ExternHasInitializer`
// is set. Reporter-level unit tests cover the gate at the policy
// layer; THIS test pins it through compileFiles → per-target
// scratch → analyze → lowerToHir → errorCount → exit code.
//
// NOTE: this test does NOT pin the H1 fix (program.cpp scratch
// inheriting reporterConfig). H_ExternHasInitializer is in
// `kUnsuppressableCodes`, so `applyPolicy` short-circuits the
// suppress check regardless of which reporter receives the
// diagnostic. Reverting the H1 fix would NOT cause this test to
// fail. The H1 fix's load-bearing path is suppressible per-target
// diagnostics; pinning it requires a suppressible per-target
// emitter, which doesn't exist in the c-subset path today.
// Anchored as D-H1-SUPPRESSIBLE-PER-TARGET-PIN (trigger: first
// suppressible code that fires reliably on the per-target path).
TEST(Program_CompileFiles, SuppressedHExternHasInitializerStillReturnsNonZero) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "ext_init.c", "extern int x = 5;\n");
    scratch.useAsCwd();
    Program prog;
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::H_ExternHasInitializer);
    EXPECT_EQ(prog.compileFiles({src.generic_string()},
                                "c-subset",
                                {"x86_64:elf64-x86_64-linux"},
                                cfg),
              1);
}

// ── compileDirectory ──────────────────────────────────────────

TEST(Program_CompileDirectory, WiresThroughForMatchingFiles) {
    // Wiring pin: the recursive scan + extension filter routes
    // matching files into compileFiles which routes into the
    // pipeline. Same upstream gap as the zero-arg single-file
    // test — byte assertion is gated on plan 12 ML7 cycle 2 +
    // plan 13 AS cycle gaps (anchored D-LK10-2).
    ScratchDir scratch{Location::InsideRepo, "program"};
    writeCSubsetSource(scratch.path(), "a.c",
                        "int aaa() { return 1; }\n");
    writeCSubsetSource(scratch.path(), "b.c",
                        "int bbb() { return 2; }\n");
    // Distractor: extension not in c-subset's fileExtensions
    // (".c"/".h") — must be ignored.
    writeCSubsetSource(scratch.path(), "ignored.txt",
                        "this is not c\n");
    scratch.useAsCwd();

    Program prog;
    prog.compileDirectory(
        scratch.path().generic_string(),
        "c-subset",
        {"x86_64:elf64-x86_64-linux"});

    // Wiring proof: the target directory exists, which means the
    // scan succeeded AND compileFiles routed past schema loading
    // into the pipeline.
    auto const targetDir = scratch.path() / "target"
                                          / "elf64-x86_64-linux";
    EXPECT_TRUE(fs::is_directory(targetDir));
}

TEST(Program_CompileDirectory, RejectsMissingDirectory) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const ghost = scratch.path() / "does-not-exist";
    Program prog;
    EXPECT_EQ(prog.compileDirectory(ghost.generic_string(),
                                    "c-subset",
                                    {"x86_64:elf64-x86_64-linux"}),
              1);
}

TEST(Program_CompileDirectory, RejectsNoMatchingFiles) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    // Directory exists but contains nothing matching c-subset.
    writeCSubsetSource(scratch.path(), "only.txt", "nothing\n");
    Program prog;
    EXPECT_EQ(prog.compileDirectory(scratch.path().generic_string(),
                                    "c-subset",
                                    {"x86_64:elf64-x86_64-linux"}),
              1);
}
