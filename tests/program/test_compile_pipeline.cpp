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

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/substrate/phase_timers.hpp"   // c97: per-phase --time pin
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "diagnostic_count.hpp"
#include "ffi/abi/abi_catalog.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "mir/merge/mir_merge.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/optimizer.hpp"
#include "program/compile_pipeline.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
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

// Module-wide count of a MIR opcode (the established opt-test pattern). Used by the
// cross-CU inlining effectiveness pin to assert a Call disappeared from the optimized
// merged module.
std::size_t countOpInModule(Mir const& mir, MirOpcode op) {
    std::size_t n = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                if (mir.instOpcode(mir.blockInstAt(b, ii)) == op) ++n;
            }
        }
    }
    return n;
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

// ── c97: per-phase --time accumulators ─────────────────────────
//
// Pin for the PhaseTimers substrate the `--time` report reads: after ONE
// real end-to-end compile, (a) every CompilePhase has a distinct non-empty
// pipeline-verb name (the report loop prints every ordinal, so name
// coverage here pins the report's row set), (b) every phase the tiny-source
// pipeline necessarily runs recorded at least one run, and (c) the
// attributed total is a plausible nonzero. RED-on-disable: deleting any
// instrumented Scope zeroes that phase's run count and the matching
// EXPECT fails. (Phases a trivial source legitimately skips — tokenize
// [c-subset preprocesses], reparse [no ambiguous cast], synthesize-ffi
// [no externs] — are deliberately un-asserted.)
TEST(Program_CompileFiles, PhaseTimersRecordEveryPipelinePhase) {
    using dss::substrate::CompilePhase;
    using dss::substrate::PhaseTimers;
    using dss::substrate::compilePhaseName;
    using dss::substrate::kCompilePhaseCount;

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "timed.c",
        "int timed() { return 9; }\n");
    scratch.useAsCwd();

    PhaseTimers::reset();
    Program prog;
    int const rc = prog.compileFiles(
        {src.generic_string()}, "c-subset", {"x86_64:elf64-x86_64-linux"});
    ASSERT_EQ(rc, 0);

    // (a) every phase name is a distinct, non-empty pipeline verb.
    std::set<std::string_view> names;
    for (std::size_t i = 0; i < kCompilePhaseCount; ++i) {
        auto const name = compilePhaseName(static_cast<CompilePhase>(i));
        EXPECT_FALSE(name.empty());
        EXPECT_NE(name, "<invalid-phase>");
        EXPECT_TRUE(names.insert(name).second)
            << "duplicate phase name: " << name;
    }

    // (b) the phases this compile necessarily exercises each recorded a run.
    for (CompilePhase p : {CompilePhase::Preprocess, CompilePhase::Parse,
                           CompilePhase::ResolveImports, CompilePhase::Semantic,
                           CompilePhase::LowerHir, CompilePhase::LowerMir,
                           CompilePhase::Optimize, CompilePhase::LowerLir,
                           CompilePhase::Regalloc, CompilePhase::Encode,
                           CompilePhase::Link}) {
        EXPECT_GE(PhaseTimers::read(p).runs, 1u)
            << "phase '" << compilePhaseName(p) << "' recorded no run";
    }

    // (c) plausible nonzero attributed total.
    EXPECT_GT(PhaseTimers::attributedNanoseconds(), 0u);
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

// ── D-LK10-OUTPUT-PIPELINE-E2E ─────────────────────────────────────
//
// `--output <dir>` routes emitted artifacts away from the default
// `<cwd>/target/<formatName>/<binary>` layout. The path-construction
// branch at `src/program/program.cpp::compileOneTarget` is a
// ternary on `multiTargetBuild`:
//   single-target  → `<outputDir>/<binary>`
//   multi-target   → `<outputDir>/<formatName>/<binary>`
// A regression flipping the ternary's arms would either silently
// route multi-target outputs to a flat dir (collision risk) or
// single-target outputs through a phantom `<formatName>/` subdir
// (path drift). These tests pin the disk-side layout end-to-end.

TEST(Program_CompileFiles, OutputFlagSingleTargetPlacesArtifactFlat) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "forty_two.c",
        "int forty_two() { return 42; }\n");
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    Program prog;
    prog.setOutputDir(outDir);
    int const rc = prog.compileFiles(
        {src.generic_string()},
        "c-subset",
        {"x86_64:elf64-x86_64-linux"});
    ASSERT_EQ(rc, 0);
    // Single-target with --output: artifact lands DIRECTLY under
    // the user-specified directory, with NO format subdir
    // interposed. A regression to multi-target-style routing would
    // place the file at `<outDir>/elf64-x86_64-linux/forty_two.o`
    // instead — the absent-path assertion catches that drift.
    EXPECT_TRUE(fs::exists(outDir / "forty_two.o"))
        << "single-target --output must place artifact flat: "
           "<outDir>/<binary>";
    EXPECT_FALSE(fs::exists(
        outDir / "elf64-x86_64-linux" / "forty_two.o"))
        << "single-target must NOT interpose a <formatName>/ subdir "
           "— that's the multi-target arm; regression would silently "
           "drift artifact paths for downstream build scripts";
}

// ── D-LK10-ENTRY-MAIN-IMPLICIT-RETURN ──────────────────────────────
//
// C99 §5.1.2.2.3: a `main` function that reaches the closing `}`
// without an explicit `return` has the semantics of an implicit
// `return 0`. Source-agnostically expressed via the c-subset's
// semantic config (`declarations[topLevelDecl].
// implicitReturnZeroForFunctionNames: ["main"]`); the HIR lowering
// at `cst_to_hir.cpp::lowerFunctionDecl` reads the list and
// appends a synthetic `return 0` to a body that doesn't
// structurally terminate. Other languages declare their own
// entry-fn names in their own `.lang.json` — shared HIR substrate
// has zero language hardcodes.
//
// Before this fix: the verifier's `checkReturnCompleteness`
// loud-failed `int main() { }` with H_VerifierFailure ("non-void
// function may fall through"). After: the synthetic return makes
// the verifier pass, and downstream MIR/LIR see a defined exit
// value (0) so the trampoline's `mov status, rax` reads
// deterministic 0 rather than register-uninitialized garbage.

TEST(Program_CompileFiles, MainWithoutExplicitReturnGetsImplicitReturnZero) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    // `int main() { }` — no explicit return. Pre-fix this would
    // have failed verification; post-fix it lowers cleanly.
    auto const src = writeCSubsetSource(
        scratch.path(), "implicit_main.c",
        "int main() { }\n");
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileFiles(
        {src.generic_string()},
        "c-subset",
        {"x86_64:elf64-x86_64-linux"});
    EXPECT_EQ(rc, 0)
        << "main without explicit return must compile cleanly — "
           "the HIR lowering inserts synthetic `return 0` per C99 "
           "§5.1.2.2.3 via c-subset's "
           "implicitReturnZeroForFunctionNames config";
    auto const outDir =
        scratch.path() / "target" / "elf64-x86_64-linux";
    EXPECT_TRUE(fs::exists(outDir / "implicit_main.o"))
        << "successful compile must produce the .o artifact";
}

TEST(Program_CompileFiles, MainWithExplicitReturnCompilesCleanly) {
    // Anti-double-insert pin: when `main` already path-terminates
    // via its own explicit `return`, the `pathTerminates` guard in
    // `maybeAppendImplicitReturnZero` SHORT-CIRCUITS — the
    // synthetic return is NOT appended (avoiding unreachable-code
    // diagnostics from `checkBlockTermination`). A regression
    // dropping that guard would cause every main to land with two
    // returns, the second one unreachable → loud verifier error
    // → rc != 0. This test pins rc==0 for the explicit-return path.
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "explicit_return.c",
        "int main() { return 42; }\n");
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileFiles(
        {src.generic_string()},
        "c-subset",
        {"x86_64:elf64-x86_64-linux"});
    EXPECT_EQ(rc, 0)
        << "main with explicit return must compile cleanly — the "
           "pathTerminates guard must short-circuit BEFORE appending "
           "a synthetic return (otherwise we'd get an unreachable-"
           "code diagnostic on the second return)";
}

TEST(Program_CompileFiles, NonMainWithoutExplicitReturnStillFailsLoud) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    // A non-main function lacking a return must STILL be rejected.
    // The implicit-return-0 rule is scoped to names in the
    // language config's `implicitReturnZeroForFunctionNames` list
    // (c-subset declares only `main`); every other non-void
    // unreturning fn falls through to verifier's loud-fail.
    auto const src = writeCSubsetSource(
        scratch.path(), "bad_helper.c",
        "int helper() { }\n"
        "int main() { return helper(); }\n");
    scratch.useAsCwd();

    Program prog;
    DiagnosticReporter rep;
    int const rc = prog.compileFiles(
        {src.generic_string()},
        "c-subset",
        {"x86_64:elf64-x86_64-linux"},
        rep);
    EXPECT_NE(rc, 0)
        << "non-main non-void function without explicit return "
           "must STILL fail the verifier — the implicit-return "
           "rule is scoped to the names the language declares "
           "(only `main` for c-subset)";
    // test-analyzer C-3 fold (3rd-order audit on 39897eb): pin the
    // EXACT diagnostic code so a regression that fails this corpus
    // via a different code path (e.g., bailing in MIR-lowering
    // before HIR verification) doesn't silently satisfy the loose
    // rc != 0. The test's PURPOSE is to pin "implicit-return-0
    // rule is scoped to names in the language config" — that's a
    // property of `checkReturnCompleteness` firing in HirVerifier,
    // not any later tier.
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::H_VerifierFailure),
              1u)
        << "exactly one H_VerifierFailure (from "
           "checkReturnCompleteness on `helper`); a different "
           "diagnostic code firing would mean the scope-restriction "
           "is in the wrong tier";
}

TEST(Program_CompileFiles, OutputFlagMultiTargetPlacesArtifactsInFormatSubdirs) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "small.c",
        "int small() { return 7; }\n");
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    Program prog;
    prog.setOutputDir(outDir);
    prog.compileFiles({src.generic_string()},
                       "c-subset",
                       {"x86_64:elf64-x86_64-linux",
                        "x86_64:pe64-x86_64-windows"});
    // Multi-target with --output: each target gets its own
    // `<formatName>/` subdir under outDir to prevent same-named
    // artifacts (small.o on ELF, small.obj on PE happen to differ
    // by extension here, but the discipline applies universally
    // when extensions collide).
    EXPECT_TRUE(fs::is_directory(outDir / "elf64-x86_64-linux"))
        << "multi-target --output must create <outDir>/<formatName>/ "
           "subdirs to prevent artifact collisions";
    EXPECT_TRUE(fs::is_directory(outDir / "pe64-x86_64-windows"));
    // Regression assertion (anti-flatten): a future change that
    // routes multi-target to a flat dir would leave the per-format
    // subdirs absent.
    EXPECT_FALSE(fs::exists(outDir / "small.o"))
        << "multi-target must NOT flatten — artifacts must live "
           "under their <formatName>/ subdir";
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

// 0f7d714 audit-fold (2026-06-01): pin that the CLI stderr drain
// actually renders `d.contextPrefix` for per-target diagnostics.
// The `eb2c6c7` Track 1 split moved the `[target=...]` stamp from
// d.actual into d.contextPrefix; the 0f7d714 audit-fold added the
// prepend at drainDiagnosticsToStderr; this test pins the e2e CLI
// behavior. Without this pin a regression dropping `<< d.contextPrefix`
// from drainDiagnosticsToStderr would silently re-open the multi-
// target stderr skew (operators could no longer tell which target
// produced each line). Uses `extern int x = 5;` because it parses
// cleanly (parse errors come from a CU-level shared reporter that
// is NOT routed through mergeWithTargetContext); the H_Extern* error
// fires in the per-target loop and IS prefixed.
TEST(Program_CompileFiles, StderrIncludesTargetContextPrefixOnPerTargetError) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "ext_init.c", "extern int x = 5;\n");
    scratch.useAsCwd();
    Program prog;
    testing::internal::CaptureStderr();
    int const rc = prog.compileFiles(
        {src.generic_string()},
        "c-subset",
        {"x86_64:elf64-x86_64-linux"});
    auto const stderrOut = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 1);
    EXPECT_NE(stderrOut.find("[target=x86_64:elf64-x86_64-linux]"),
              std::string::npos)
        << "drainDiagnosticsToStderr MUST render contextPrefix so "
           "multi-target operators can route per-target diagnostics; "
           "got stderr:\n" << stderrOut;
}

// ── Plan 06 V2-4 Part A: positioned source-context diagnostics ──────
// The driver drain now routes a buffer-bearing diagnostic (parser /
// semantic, with a span into real source) through DSS's OWN renderer
// (`DiagnosticReporter::format` — hand-written over our SourceBuffer /
// SourceSpan; NO clang / LLVM dependency): `--> file:line:col` + the
// source line + a `^` caret. The BufferRegistry that resolves the
// diagnostic's BufferId is built in `runCusToTargets` from the CUs'
// trees. Buffer-LESS driver `D_*` diagnostics keep the code-only line.

// A malformed-source compile prints the positioned context + caret at
// the exact offending column. RED-on-disable: revert
// `drainDiagnosticsToStderr` to the old code-only loop (drop the
// `format()` branch) and the `-->` / `bad.c:2:12` / `^` assertions all
// go red — this is the cycle's effectiveness lever.
TEST(Program_CompileFiles, MalformedSourceRendersPositionedCaret) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    // Illegal character '@' at line 2, column 12 (after `return `).
    auto const src = writeCSubsetSource(
        scratch.path(), "bad.c", "int main() {\n    return @;\n}\n");
    scratch.useAsCwd();
    Program prog;
    testing::internal::CaptureStderr();
    int const rc = prog.compileFiles(
        {src.generic_string()}, "c-subset", {"x86_64:elf64-x86_64-linux-exec"});
    auto const err = testing::internal::GetCapturedStderr();

    EXPECT_NE(rc, 0) << "malformed source must fail the build";
    // `--> <file>:<line>:<col>` header at the exact position of '@'. The
    // parser stamps the span; format() resolves it via the registry.
    EXPECT_NE(err.find("bad.c:2:12"), std::string::npos)
        << "expected positioned header '<file>:2:12'; got:\n" << err;
    EXPECT_NE(err.find("-->"), std::string::npos)
        << "expected a '-->' source-location header; got:\n" << err;
    EXPECT_NE(err.find('^'), std::string::npos)
        << "expected a '^' caret underline; got:\n" << err;
    // The source line is echoed (the positioned context the renderer adds).
    EXPECT_NE(err.find("return @"), std::string::npos)
        << "expected the offending source line echoed; got:\n" << err;
}

// A buffer-LESS driver-tier diagnostic (no source span) stays on the
// code-only path: NO bogus `--> <unknown-buffer>` line, NO caret, and
// the human-readable SYMBOLIC code name (not the numeric band). Pins
// the per-diagnostic routing split. Empty targets → D_InvalidTargetSpec
// is emitted buffer-less, before any CU/source is touched.
TEST(Program_CompileFiles, BufferlessDriverDiagnosticStaysCodeOnly) {
    Program prog;
    testing::internal::CaptureStderr();
    int const rc = prog.compileFiles({"unused.c"}, "c-subset", /*targets*/ {});
    auto const err = testing::internal::GetCapturedStderr();

    EXPECT_EQ(rc, 1);
    EXPECT_NE(err.find("[D_InvalidTargetSpec]"), std::string::npos)
        << "a buffer-less driver diagnostic must render the code-only line "
           "with the SYMBOLIC code name; got:\n" << err;
    EXPECT_EQ(err.find("-->"), std::string::npos)
        << "a buffer-less diagnostic must NOT print a source-location line; "
           "got:\n" << err;
    EXPECT_EQ(err.find("<unknown-buffer"), std::string::npos)
        << "a buffer-less diagnostic must NOT print '<unknown-buffer>'; "
           "got:\n" << err;
}

// D-CAP-MARKER-MULTI-TARGET-E2E-PIN close (e4508b9 → next 2026-06-01):
// the prior anchor was reserved because `D_TargetMachineCodeMismatch`
// joined `kUnsuppressableCodes` and bypasses all cap/dedup gates —
// no `P_TooManyDiagnostics` marker can fire on the cross-validate
// path regardless of `maxDiagnostics`. The new
// `compileFiles(..., DiagnosticReporter&)` rep-injection overload
// (program.hpp / program.cpp) lets us inspect rep post-run and pin
// the actual single-chokepoint contract: with 2 targets each firing
// a SUPPRESSABLE per-target error (`D_SchemaLoadFailed`, NOT in
// kUnsuppressableCodes) and `maxDiagnostics=1`, the cap fires
// EXACTLY ONCE at the run-wide `rep` during merge — emitting one
// `P_TooManyDiagnostics` marker. Target 2's error arrives during
// merge but rep is capped and `report()` no-ops.
TEST(Program_CompileFiles, CapMarkerAppearsExactlyOnceAfterMultiTargetSaturation) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "ok.c", "int f() { return 0; }\n");
    scratch.useAsCwd();
    Program prog;
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 1;
    DiagnosticReporter rep{cfg};
    int const rc = prog.compileFiles(
        {src.generic_string()},
        "c-subset",
        {"x86_64:no-such-format-A",
         "x86_64:no-such-format-B"},
        rep);
    EXPECT_EQ(rc, 1);
    // Three-sided assertion (d312c1c audit fold, test-analyzer #2):
    // pin marker count + total size + cap shape. With maxDiagnostics=1,
    // target A's FIRST diagnostic (the format-schema JSON load fires
    // forwardConfigDiagnostics → C_InvalidFormatName, per cycle 10m's
    // per-kind diag-code split) lands in scratch, gets merged into
    // rep, fills the cap → marker fires. Every subsequent diagnostic
    // from both targets is silently dropped at rep's hitCap_ gate.
    // Final state: exactly 2 entries in rep.all() — one
    // C_InvalidFormatName + one P_TooManyDiagnostics marker.
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::P_TooManyDiagnostics),
              1u)
        << "single-chokepoint contract: cap fires EXACTLY ONCE at "
           "rep during merge, regardless of how many targets exceed "
           "their per-target diagnostic count.";
    EXPECT_EQ(rep.all().size(), 2u)
        << "rep contents must be exactly {first cap-filling diagnostic, "
           "P_TooManyDiagnostics marker}; any other diagnostics signal "
           "a regression in the per-target loop or merge path.";
    // 9945457 audit fold (3-agent convergence: silent-failure H2 +
    // code-architect Q5 + test-analyzer-dim-2 #6): pin the IDENTITY
    // of the cap-filling diagnostic. A regression in
    // forwardConfigDiagnostics that swaps the first emitted code
    // (e.g. renamed C_InvalidFormatName or rerouted to a different
    // C_*/D_*) would leave size==2 + marker==1 green while silently
    // shifting cap-fill semantics. The comment above already claimed
    // this identity; the test now enforces it.
    ASSERT_GE(rep.all().size(), 1u);
    // Cycle 10m closure of D-CONFIG-DIAGNOSTIC-CODE-PER-KIND: the
    // first cap-filling diagnostic was historically `C_Invalid
    // LanguageName` because `findShippedConfig` callers all routed
    // their "name invalid / not found" errors through that one code
    // regardless of config kind. Post-cycle the format-schema JSON
    // load emits `C_InvalidFormatName` (per-kind specificity). The
    // identity-pin discipline (catch a refactor that swaps the
    // emitted code) is preserved — just on the new kind-specific
    // code.
    EXPECT_EQ(rep.all()[0].code, DiagnosticCode::C_InvalidFormatName)
        << "first cap-filling diagnostic must be C_InvalidFormatName "
           "(forwardConfigDiagnostics fires at format-schema JSON load). "
           "A different first code signals a refactor in the config-"
           "diagnostic plumbing.";
}

// Negative pin: with maxDiagnostics at the default (large) cap, no
// marker should fire — both targets' D_SchemaLoadFailed entries
// surface intact in rep. Catches a regression that fires the marker
// unconditionally on multi-target runs.
TEST(Program_CompileFiles, NoCapMarkerWhenDiagnosticsBudgetExceedsErrorCount) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "ok.c", "int f() { return 0; }\n");
    scratch.useAsCwd();
    Program prog;
    DiagnosticReporter rep;  // default config — large cap
    int const rc = prog.compileFiles(
        {src.generic_string()},
        "c-subset",
        {"x86_64:no-such-format-A",
         "x86_64:no-such-format-B"},
        rep);
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::P_TooManyDiagnostics),
              0u)
        << "default cap is large enough to hold both D_SchemaLoadFailed "
           "entries; no marker should fire";
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::D_SchemaLoadFailed),
              2u)
        << "both per-target schema-load failures must surface in rep";
}

// ── compileProject: plan 06 AP2 (project-config loader + profile gate) ──
// (Was the `D_PlanNotLanded` stub pre-AP2; the stub is gone — these now
// pin the real loader's fail-loud surface. Deeper AP2 wiring — the
// profile-enforcement gate against a real `.dss-project.json` — is pinned
// in `tests/program/test_project_config.cpp`.)

TEST(Program_CompileProject, FailsLoudMissingProjectFile) {
    Program prog;
    DiagnosticReporter rep;
    // A nonexistent project-config path fails loud with D_FileNotFound
    // (loadProjectConfig's open-failure arm), not the removed
    // D_PlanNotLanded stub.
    EXPECT_EQ(prog.compileProject("does-not-exist.dss-project.json", rep), 1);
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::D_FileNotFound), 1u);
}

// H2 behavioral pin (silent-failure audit post-fold #2), re-aimed at a
// code compileProject ACTUALLY emits: even with the fail-loud code
// suppressed, compileProject must return non-zero. The suppress hides
// the stderr message but MUST NOT absorb the "the operation didn't
// happen" signal into a silent success exit — compileProject returns 1
// on the failure path explicitly, not via `errorCount() == 0 ? 0 : 1`.
// Without this, build systems downstream treat exit 0 as "build
// happened" and consume nonexistent outputs.
TEST(Program_CompileProject, SuppressedFailLoudStillReturnsNonZero) {
    Program prog;
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::D_FileNotFound);
    EXPECT_EQ(prog.compileProject("does-not-exist.dss-project.json", cfg), 1);
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

// ── Cycle 25 Stage C — whole-program MIR merge makes a cross-CU call DIRECT ──────
//
// The driver's N>1 path now folds the per-CU MIRs into ONE module via `mergeCuMirs`
// BEFORE lowering, so a cross-CU call (`main` → `add5` defined in a sibling CU) is an
// intra-module DIRECT call — NOT the cycle-19 assembled-tier GOT-like rodata thunk
// slot. This pins that the thunk is GONE end-to-end, by driving the SAME source the
// `cross_cu_call` runtime example uses through `buildCuMir` ×2 → `mergeCuMirs` →
// `lowerMergedToAssembly` and asserting:
//   1. the merge STRIPS the resolved cross-CU extern (`add5` absent from the merged
//      externImports) — the import that WOULD have forced a thunk;
//   2. the single lowered `AssembledModule` likewise carries no `add5` import;
//   3. `linker::link` over that ONE module takes its single-module path and produces
//      ZERO `resolvedCrossCuRefs` — and `mergeModules` mints a thunk slot ONLY from a
//      non-empty `resolvedCrossCuRefs`, so an empty list is the definitive "no thunk".
// A regression that routed the N>1 build back through the per-CU-then-link-merge path
// would leave `add5` as a surviving cross-CU reference → a thunk slot → this fails.
TEST(Program_WholeProgramMerge, CrossCuCallIsDirectNoThunkSlot) {
    auto grammarR = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(grammarR.has_value());
    auto grammar = *grammarR;
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    auto formatR = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(formatR.has_value());

    DiagnosticReporter rep;
    auto const abi = dss::ffi::resolveAbi(**targetR, **formatR, rep);
    ASSERT_TRUE(abi.has_value());
    ASSERT_NE(abi->cc, nullptr);
    auto const ccSpan = (*targetR)->callingConventions();
    auto const ccIndex = static_cast<std::uint16_t>(
        std::distance(ccSpan.data(), abi->cc));

    // The exact cross_cu_call corpus: main calls extern add5; helper defines it.
    auto buildCu = [&](std::string src, std::string label) {
        UnitBuilder builder{grammar};
        builder.addInMemory(std::move(src), std::move(label));
        return std::move(builder).finish();
    };
    CompilationUnit cuMain =
        buildCu("extern int add5(int x);\nint main() { return add5(37); }\n", "main.c");
    CompilationUnit cuHelper =
        buildCu("int add5(int x) { return x + 5; }\n", "helper.c");

    // LOOP 1 (driver-parity): build each CU's MIR.
    auto mirMain = buildCuMir(cuMain, *grammar, **targetR, **formatR, ccIndex, rep);
    ASSERT_TRUE(mirMain.has_value()) << "errorCount=" << rep.errorCount();
    auto mirHelper = buildCuMir(cuHelper, *grammar, **targetR, **formatR, ccIndex, rep);
    ASSERT_TRUE(mirHelper.has_value()) << "errorCount=" << rep.errorCount();

    // MergeCuInputs (constructed exactly as `compileOneTarget`'s N>1 arm does).
    std::vector<CuMirModule> cuMirs;
    cuMirs.push_back(std::move(*mirMain));
    cuMirs.push_back(std::move(*mirHelper));

    std::vector<MergeCuInput> inputs;
    for (auto& cm : cuMirs) {
        MergeCuInput in;
        in.mir      = &cm.mir;
        in.interner = &cm.model.lattice().interner();
        in.nameOf   = [cmP = &cm](SymbolId s) -> std::string {
            if (SymbolRecord const* r = cmP->model.recordFor(s)) return r->name;
            for (auto const& e : cmP->externImports) {
                if (e.symbol.v == s.v) return e.mangledName;
            }
            return std::string{};
        };
        in.externImports = cm.externImports;
        inputs.push_back(std::move(in));
    }

    TypeLattice host{cuMirs[0].cuId,
                     std::string{cuMirs[0].model.lattice().registry().sourceLanguage()}};
    std::vector<std::string> entryNames;
    for (auto const& decl : grammar->semantics().declarations) {
        for (auto const& n : decl.implicitReturnZeroForFunctionNames) {
            entryNames.push_back(n);
        }
    }

    auto merged = mergeCuMirs(
        std::span<MergeCuInput const>{inputs.data(), inputs.size()},
        std::move(host),
        std::span<std::string const>{entryNames.data(), entryNames.size()}, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

    // (1a) Both `main` AND `add5` are DEFINED in the single merged module — the
    //      positive shape of a direct intra-module call (the call target is in-module,
    //      not an external symbol the linker must bridge).
    bool haveMain = false, haveAdd5 = false;
    for (std::uint32_t i = 0; i < merged->mir.moduleFuncCount(); ++i) {
        auto const it = merged->symbolNames.find(
            merged->mir.funcSymbol(merged->mir.funcAt(i)).v);
        if (it == merged->symbolNames.end()) continue;
        if (it->second == "main") haveMain = true;
        if (it->second == "add5") haveAdd5 = true;
    }
    EXPECT_TRUE(haveMain) << "main must be defined in the merged module";
    EXPECT_TRUE(haveAdd5)
        << "add5 must be DEFINED in the merged module — the cross-CU call's target is "
           "now in-module (direct call), the precondition for there being no thunk";

    // (1b) The merge rewired main→add5 to a DIRECT call and STRIPPED the extern.
    for (auto const& e : merged->externImports) {
        EXPECT_NE(e.mangledName, "add5")
            << "the cross-CU-resolved extern `add5` must NOT survive the merge — its "
               "call is now a direct intra-module call, so no thunk is needed";
    }

    // Lower the single merged module → ONE AssembledModule. The cross-CU
    // extern (add5) was stripped to a DIRECT call by the merge, so the
    // merged module has no surviving externs — the extern-call dispatch is
    // never consumed (nullopt is the faithful value; a guard fires only if
    // an extern import survives without one). D-FFI-EXTERN-CALL-DISPATCH.
    auto mod = lowerMergedToAssembly(*merged, *grammar, **targetR,
                                     (*formatR)->dataModel(),
                                     effectiveBitFieldStrategy(**targetR, **formatR),
                                     ccIndex,
                                     cuMirs[0].cuId,
                                     /*externCallDispatch=*/std::nullopt,
                                     /*dataImportBinding=*/std::nullopt,
                                     /*tlsAccess=*/std::nullopt,
                                     /*sehScopes=*/{}, rep);
    ASSERT_TRUE(mod.has_value()) << "errorCount=" << rep.errorCount();

    // (2) The lowered module carries no `add5` import either (direct call).
    for (auto const& e : mod->externImports) {
        EXPECT_NE(e.mangledName, "add5");
    }

    // (3) The linker receives a SINGLE module → its single-module path → ZERO
    //     resolvedCrossCuRefs → NO thunk slot minted. (A two-strong / undefined ref
    //     would also be empty here; the merged module being one self-contained unit is
    //     why there is no cross-CU edge at all.)
    auto const before = rep.errorCount();
    auto image = dss::linker::link(
        std::span<AssembledModule const>{&*mod, 1}, **targetR, **formatR, rep);
    EXPECT_EQ(rep.errorCount(), before) << "linking the single merged module must not error";
    EXPECT_TRUE(image.ok()) << "the merged single-module image must link cleanly";
    EXPECT_TRUE(image.resolvedCrossCuRefs.empty())
        << "the merged image has NO cross-CU reference — `mergeModules` mints a thunk "
           "slot only from a non-empty resolvedCrossCuRefs, so this is the definitive "
           "proof the cycle-19 thunk is gone for the now-internal main→add5 call";
}

// ── G2 (Cycle 26, D-OPT7-1): a cross-CU call is INLINED on the merged module ──────────
//
// The cycle-25 merge made main→add5 an intra-module DIRECT call; cycle 26 optimizes the
// MERGED module so the inliner (whose `symToFunc` now resolves the in-module add5)
// SPLICES add5's body into main. This pin drives the exact `cross_cu_call` corpus source
// through buildCuMir×2 → mergeCuMirs → `optimizeModule` and asserts main's Call to add5
// is GONE in the optimized merged module.
//
// RED-on-disable is demonstrated IN-TEST by a second arm that runs an `[Identity]`
// pipeline over the SAME freshly-merged module: with no Inlining pass the Call SURVIVES.
// So a green `[Inlining]` arm (Call gone) + a green `[Identity]` arm (Call present) prove
// the disappearance is caused by the Inlining pass running on the merged module — not by
// the merge, lowering, or any unrelated rewrite. (Equivalently: deleting the Part-2
// `optimizeModule` wiring leaves the Call present, which this `[Inlining]` arm catches.)
//
// NON-VACUITY: `add5(37)` is a real cross-FUNCTION call. The `[Inlining]`-only pipeline
// runs no ConstFold, so `37 + 5` is NOT folded — the only way main's Call vanishes is the
// callee body being spliced in. A surviving Call ⇒ no inline happened.
TEST(Program_WholeProgramMerge, CrossCuCallIsInlinedOnMergedModule) {
    auto grammarR = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(grammarR.has_value());
    auto grammar = *grammarR;
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    auto formatR = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(formatR.has_value());

    DiagnosticReporter rep;
    auto const abi = dss::ffi::resolveAbi(**targetR, **formatR, rep);
    ASSERT_TRUE(abi.has_value());
    ASSERT_NE(abi->cc, nullptr);
    auto const ccSpan = (*targetR)->callingConventions();
    auto const ccIndex = static_cast<std::uint16_t>(
        std::distance(ccSpan.data(), abi->cc));

    auto buildCu = [&](std::string src, std::string label) {
        UnitBuilder builder{grammar};
        builder.addInMemory(std::move(src), std::move(label));
        return std::move(builder).finish();
    };

    // Build a fresh whole-program MERGED module from the cross_cu_call corpus sources.
    // Re-built per arm because `optimizeModule` mutates the merged `Mir` in place — each
    // arm needs its own pristine merged module. The CUs/CuMirModules are rebuilt too so
    // there is zero shared mutable state between arms.
    auto buildMergedModule = [&]() -> std::optional<MergedMirModule> {
        CompilationUnit cuMain = buildCu(
            "extern int add5(int x);\nint main() { return add5(37); }\n", "main.c");
        CompilationUnit cuHelper =
            buildCu("int add5(int x) { return x + 5; }\n", "helper.c");

        auto mirMain = buildCuMir(cuMain, *grammar, **targetR, **formatR, ccIndex, rep);
        if (!mirMain.has_value()) return std::nullopt;
        auto mirHelper = buildCuMir(cuHelper, *grammar, **targetR, **formatR, ccIndex, rep);
        if (!mirHelper.has_value()) return std::nullopt;

        // `cuMirs` + `inputs` must stay alive through `mergeCuMirs` (the merge reads each
        // CU's nameOf + interner while cloning). The returned `MergedMirModule` is
        // self-contained (owns its host lattice + cloned MIR), so dropping these locals
        // at lambda exit is safe.
        std::vector<CuMirModule> cuMirs;
        cuMirs.push_back(std::move(*mirMain));
        cuMirs.push_back(std::move(*mirHelper));

        std::vector<MergeCuInput> inputs;
        for (auto& cm : cuMirs) {
            MergeCuInput in;
            in.mir      = &cm.mir;
            in.interner = &cm.model.lattice().interner();
            in.nameOf   = [cmP = &cm](SymbolId s) -> std::string {
                if (SymbolRecord const* r = cmP->model.recordFor(s)) return r->name;
                for (auto const& e : cmP->externImports) {
                    if (e.symbol.v == s.v) return e.mangledName;
                }
                return std::string{};
            };
            in.externImports = cm.externImports;
            inputs.push_back(std::move(in));
        }

        TypeLattice host{cuMirs[0].cuId,
                         std::string{cuMirs[0].model.lattice().registry().sourceLanguage()}};
        std::vector<std::string> entryNames;
        for (auto const& decl : grammar->semantics().declarations) {
            for (auto const& n : decl.implicitReturnZeroForFunctionNames) {
                entryNames.push_back(n);
            }
        }

        return mergeCuMirs(
            std::span<MergeCuInput const>{inputs.data(), inputs.size()},
            std::move(host),
            std::span<std::string const>{entryNames.data(), entryNames.size()}, rep);
    };

    // The cross_cu_call merged module always carries exactly one main→add5 Call before
    // optimization — the precondition the two arms diverge from.
    {
        auto merged = buildMergedModule();
        ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();
        ASSERT_EQ(countOpInModule(merged->mir, MirOpcode::Call), 1u)
            << "before optimization the merged module holds main's single direct call "
               "to the in-module add5";
    }

    // ── ARM 1: [Inlining] over the merged module → the Call is GONE. ──
    {
        auto merged = buildMergedModule();
        ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

        opt::OptPipeline inlining{"inlining", {opt::PassId::Inlining}};
        CompileOptions opts;
        opts.pipelineOverride = &inlining;
        auto const before = rep.errorCount();
        ASSERT_TRUE(optimizeModule(merged->mir, **targetR,
                                   merged->host.interner(), opts, rep))
            << "optimizing the merged module with [Inlining] must succeed";
        EXPECT_EQ(rep.errorCount(), before)
            << "the merged-module optimize must not emit any error";
        EXPECT_EQ(countOpInModule(merged->mir, MirOpcode::Call), 0u)
            << "the cross-CU call main→add5 must be INLINED on the merged module — its "
               "Call is replaced by the spliced add5 body (D-OPT7-1)";
    }

    // ── ARM 2 (RED-on-disable demonstration): [Identity] → the Call SURVIVES. ──
    // Identical merged module, but the pipeline runs NO Inlining pass — so the Call is
    // still present. This proves arm 1's disappearance is caused by Inlining specifically.
    {
        auto merged = buildMergedModule();
        ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

        opt::OptPipeline identity{"identity", {opt::PassId::Identity}};
        CompileOptions opts;
        opts.pipelineOverride = &identity;
        ASSERT_TRUE(optimizeModule(merged->mir, **targetR,
                                   merged->host.interner(), opts, rep));
        EXPECT_EQ(countOpInModule(merged->mir, MirOpcode::Call), 1u)
            << "with an [Identity] pipeline (no Inlining) the cross-CU Call MUST survive "
               "— the inlining in arm 1 is what removes it (RED-on-disable witness)";
    }
}

// ── Model 3 per-OBJECT-FORMAT shipped-library resolution (2026-06-09) ─────────
//
// The END-TO-END pin of the Model-3 fold: a PLATFORM-NEUTRAL `stdio.json` whose
// `library` is a per-format MAP, pulled in by `#include <stdio.h>`, must resolve
// the synthesized `puts` import to the runtime image of the ACTIVE target's
// object FORMAT — `libc.so.6` for an ELF target, `msvcrt.dll` for a PE target,
// `/usr/lib/libSystem.B.dylib` for a Mach-O target — all from the SAME descriptor.
// `buildCuMir` runs the front half (resolve → semantic inject → HIR synthesize →
// the compile_pipeline fold), and the resolved image lands on the `puts`
// `ExternImport.libraryPath`. This is what makes `puts("hello")` link on
// linux/macos that previously fixed every target to msvcrt.dll.
//
// AGNOSTIC: the per-format selection is keyed by objectFormatKindName, exercised
// here by driving the SAME source+descriptor across three formats and asserting
// three different images — no `if(format)` anywhere on the path.
//
// RED-on-disable: hardcode the descriptor's `library.elf` wrong (or revert the
// fold to ignore the map) and the ELF arm's libraryPath assertion fails.
namespace {
// The resolved `libraryPath` of the `puts` import after the front-half fold, for
// a `#include <stdio.h>; puts("hi")` CU built against `descJson` on a system dir,
// for the given target/format. Empty string ⇒ no `puts` import was produced.
[[nodiscard]] std::string resolvedPutsLibraryFor(
        std::string const& descJson, char const* targetName, char const* formatName) {
    auto grammarR = GrammarSchema::loadShipped("c-subset");
    EXPECT_TRUE(grammarR.has_value());
    if (!grammarR) return {};
    auto grammar = *grammarR;
    auto targetR = TargetSchema::loadShipped(targetName);
    EXPECT_TRUE(targetR.has_value()) << targetName;
    auto formatR = ObjectFormatSchema::loadShipped(formatName);
    EXPECT_TRUE(formatR.has_value()) << formatName;
    if (!targetR || !formatR) return {};

    DiagnosticReporter rep;
    auto const abi = dss::ffi::resolveAbi(**targetR, **formatR, rep);
    EXPECT_TRUE(abi.has_value());
    if (!abi) return {};
    auto const ccSpan = (*targetR)->callingConventions();
    auto const ccIndex = static_cast<std::uint16_t>(
        std::distance(ccSpan.data(), abi->cc));

    ScratchDir sysDir{Location::InsideRepo, "model3-libresolve"};
    std::ofstream(sysDir.path() / "stdio.json", std::ios::binary) << descJson;
    UnitBuilder builder{grammar};
    builder.addSystemDir(sysDir.path());
    builder.addInMemory("#include <stdio.h>\nint main() { puts(\"hi\"); return 0; }\n",
                        "main.c");
    CompilationUnit cu = std::move(builder).finish();

    auto cuMir = buildCuMir(cu, *grammar, **targetR, **formatR, ccIndex, rep);
    EXPECT_TRUE(cuMir.has_value()) << formatName << " errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u) << formatName;
    if (!cuMir) return {};
    for (auto const& e : cuMir->externImports) {
        if (e.mangledName == "puts" || e.mangledName == "_puts") return e.libraryPath;
    }
    return {};
}
} // namespace

TEST(Program_ShippedLibModel3, PerFormatLibraryResolvesFromNeutralDescriptor) {
    // ONE neutral descriptor — different runtime image per object format.
    std::string const desc = R"({
        "header": "stdio.h",
        "library": { "pe": "msvcrt.dll", "elf": "libc.so.6", "macho": "/usr/lib/libSystem.B.dylib" },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })";

    // ELF target → libc.so.6 (NOT msvcrt.dll — the whole point of Model 3; the
    // pre-Model-3 hardcoded windows dir always produced msvcrt.dll here).
    EXPECT_EQ(resolvedPutsLibraryFor(desc, "x86_64", "elf64-x86_64-linux-exec"),
              "libc.so.6");
    // PE target → msvcrt.dll (Windows byte-identity preserved).
    EXPECT_EQ(resolvedPutsLibraryFor(desc, "x86_64", "pe64-x86_64-windows-exec"),
              "msvcrt.dll");
    // Mach-O target → libSystem (the macos image, from the same descriptor).
    EXPECT_EQ(resolvedPutsLibraryFor(desc, "arm64", "macho64-arm64-darwin-exec"),
              "/usr/lib/libSystem.B.dylib");
}

// A descriptor whose `library` map OMITS the active format's key falls back to
// the language's `externLibraryByFormat[format]` default (the pre-Model-3
// "empty library inherits default" contract, preserved per format). Here the map
// has only "pe", so an ELF build inherits the c-subset ELF default (libc.so.6).
TEST(Program_ShippedLibModel3, MissingFormatKeyInheritsLanguageDefault) {
    std::string const descPeOnly = R"({
        "header": "stdio.h", "library": { "pe": "msvcrt.dll" },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })";
    // ELF build, map has no "elf" key → inherit c-subset externLibraryByFormat.elf.
    EXPECT_EQ(resolvedPutsLibraryFor(descPeOnly, "x86_64", "elf64-x86_64-linux-exec"),
              "libc.so.6");
}

// RED-on-disable for the knob-that-lies: the descriptor's `library.elf` value is
// AUTHORITATIVE — the fold MUST read the map, not silently fall through to the
// language default. PerFormatLibraryResolvesFromNeutralDescriptor uses real-world
// images (libc.so.6 …) that are byte-IDENTICAL to c-subset's externLibraryByFormat
// fallback, so it passes whether the map is read OR ignored — it cannot disprove
// the knob-that-lies. Here the map's `elf` image is a DISCRIMINATING value that is
// NOT the format default, so the ELF assertion goes RED iff the compile_pipeline
// map-read is deleted (every key would then inherit the default "libc.so.6").
TEST(Program_ShippedLibModel3, MapValueIsAuthoritativeOverFormatDefault) {
    std::string const descCustom = R"({
        "header": "stdio.h",
        "library": { "elf": "libcustom.so.9", "pe": "msvcrt.dll", "macho": "/usr/lib/libSystem.B.dylib" },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })";
    // The map's "elf"="libcustom.so.9" MUST win over c-subset's
    // externLibraryByFormat.elf default ("libc.so.6"). This can ONLY pass if the
    // fold genuinely reads the descriptor's per-format library map.
    EXPECT_EQ(resolvedPutsLibraryFor(descCustom, "x86_64", "elf64-x86_64-linux-exec"),
              "libcustom.so.9");
}

// ═════════════════════════════════════════════════════════════════
// D-CSUBSET-THREAD-LOCAL (TLS C1): end-to-end pipeline pins — real
// c-subset source through the FULL driver (grammar -> semantics ->
// HIR/MIR flags -> asm section-select -> the ELF dynamic walker).
//
// ★ RED-ON-DISABLE POSTURE: single-thread RUNTIME cannot distinguish
// real TLS from a process-shared static alias — these STRUCTURAL pins
// (PT_TLS present, the fs-segment access sequence in .text, the
// phdr-count delta vs the control TU) are the discriminator: routing
// thread_local through Data/Bss keeps the runtime witnesses green
// while every assertion below flips red. The runnable per-thread
// discriminator is examples/c-subset/thread_local_pthread.
// ═════════════════════════════════════════════════════════════════

namespace {

[[nodiscard]] std::vector<std::uint8_t> readAllBytes(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

[[nodiscard]] std::uint16_t rdU16(std::vector<std::uint8_t> const& b,
                                  std::size_t off) {
    return static_cast<std::uint16_t>(b[off])
         | static_cast<std::uint16_t>(b[off + 1]) << 8;
}
[[nodiscard]] std::uint64_t rdU64(std::vector<std::uint8_t> const& b,
                                  std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(b[off + i]) << (i * 8);
    return v;
}

// Count the local-exec thread-pointer reads in the image: the fs
// segment override + REX.W mov + ModRM(mod=00, rm=100) + SIB 0x25 +
// disp32 0 — `mov r64, fs:[0]`. Register-allocation picks the
// destination register, so the REX.R bit and the ModRM reg field are
// masked, not pinned.
[[nodiscard]] std::size_t countTlsBaseSeq(std::vector<std::uint8_t> const& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i + 9 <= b.size(); ++i) {
        if (b[i] == 0x64 && (b[i + 1] & 0xF8u) == 0x48u
            && b[i + 2] == 0x8B && (b[i + 3] & 0xC7u) == 0x04u
            && b[i + 4] == 0x25 && b[i + 5] == 0 && b[i + 6] == 0
            && b[i + 7] == 0 && b[i + 8] == 0) {
            ++n;
        }
    }
    return n;
}

// Find PT_TLS (p_type 7); returns the phdr's file offset or 0.
[[nodiscard]] std::size_t findPtTls(std::vector<std::uint8_t> const& b) {
    std::uint64_t const phoff = rdU64(b, 32);
    std::uint16_t const phnum = rdU16(b, 56);
    for (std::uint16_t i = 0; i < phnum; ++i) {
        std::size_t const o = static_cast<std::size_t>(phoff) + i * 56u;
        if (b[o] == 7 && b[o + 1] == 0 && b[o + 2] == 0 && b[o + 3] == 0)
            return o;
    }
    return 0;
}

} // namespace

TEST(Program_CompileFiles, ThreadLocalEmitsPtTlsAndFsAccessSequence) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "tls_e2e.c",
        "thread_local int g = 7;\n"
        "int main(void) { g = g + 35; return g; }\n");
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileFiles(
        {src.generic_string()}, "c-subset",
        {"x86_64:elf64-x86_64-linux-exec"});
    ASSERT_EQ(rc, 0) << "thread_local must compile clean end-to-end "
                        "(D-CSUBSET-THREAD-LOCAL)";

    auto const out =
        scratch.path() / "target" / "elf64-x86_64-linux-exec" / "tls_e2e";
    ASSERT_TRUE(fs::exists(out));
    auto const bytes = readAllBytes(out);
    ASSERT_GT(bytes.size(), 64u);

    // e_phnum == 6 (the PT_TLS slot) and PT_TLS covers the 4-byte
    // template with the initial value 7.
    EXPECT_EQ(rdU16(bytes, 56), 6u);
    std::size_t const tlsPh = findPtTls(bytes);
    ASSERT_NE(tlsPh, 0u) << "PT_TLS program header must be present";
    std::uint64_t const pOff    = rdU64(bytes, tlsPh + 8);
    std::uint64_t const pFilesz = rdU64(bytes, tlsPh + 32);
    std::uint64_t const pMemsz  = rdU64(bytes, tlsPh + 40);
    EXPECT_EQ(pFilesz, 4u);
    EXPECT_EQ(pMemsz, 4u);
    EXPECT_EQ(bytes[static_cast<std::size_t>(pOff)], 7u)
        << "the .tdata template must carry g's initial value";

    // The access sequence: at least one `mov r64, fs:[0]` thread-
    // pointer read (the tlsbase lowering) in the image.
    EXPECT_GE(countTlsBaseSeq(bytes), 1u)
        << "the local-exec fs-read sequence must be present";
}

TEST(Program_CompileFiles, NoThreadLocalControlHasNoTlsTrace) {
    // The byte-identity control: the SAME program with an ordinary
    // global shows ZERO TLS machinery — 5 phdrs, no PT_TLS, no
    // fs-read sequence. (The sqlite-dormant guarantee rides this:
    // every TLS emission is hasTls-gated.)
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "tls_ctl.c",
        "int g = 7;\n"
        "int main(void) { g = g + 35; return g; }\n");
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileFiles(
        {src.generic_string()}, "c-subset",
        {"x86_64:elf64-x86_64-linux-exec"});
    ASSERT_EQ(rc, 0);

    auto const out =
        scratch.path() / "target" / "elf64-x86_64-linux-exec" / "tls_ctl";
    ASSERT_TRUE(fs::exists(out));
    auto const bytes = readAllBytes(out);
    ASSERT_GT(bytes.size(), 64u);

    EXPECT_EQ(rdU16(bytes, 56), 5u) << "no PT_TLS slot without thread_local";
    EXPECT_EQ(findPtTls(bytes), 0u);
    EXPECT_EQ(countTlsBaseSeq(bytes), 0u)
        << "no fs-read sequence may appear without thread_local";
}

TEST(Program_CompileFiles, ConstThreadLocalLandsInsidePtTlsSpan) {
    // CRIT-2 / section-order pin: `thread_local const` has THREAD
    // storage duration — its bytes must live inside the PT_TLS
    // template span (per-thread address), NOT in .rodata (one shared
    // address). The isThreadLocal-FIRST section select is what routes
    // it; an isConst-first regression parks k in .rodata and empties
    // the PT_TLS span, flipping this red.
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "tls_const.c",
        "thread_local const int k = 3;\n"
        "int main(void) { return k + 39; }\n");
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileFiles(
        {src.generic_string()}, "c-subset",
        {"x86_64:elf64-x86_64-linux-exec"});
    ASSERT_EQ(rc, 0);

    auto const out =
        scratch.path() / "target" / "elf64-x86_64-linux-exec" / "tls_const";
    ASSERT_TRUE(fs::exists(out));
    auto const bytes = readAllBytes(out);

    std::size_t const tlsPh = findPtTls(bytes);
    ASSERT_NE(tlsPh, 0u) << "const thread_local must still produce PT_TLS";
    std::uint64_t const pOff    = rdU64(bytes, tlsPh + 8);
    std::uint64_t const pFilesz = rdU64(bytes, tlsPh + 32);
    ASSERT_EQ(pFilesz, 4u) << "k's 4 bytes are the whole template";
    EXPECT_EQ(bytes[static_cast<std::size_t>(pOff)], 3u)
        << "k's initial value lives INSIDE the PT_TLS span (.tdata), "
           "not .rodata";
    EXPECT_GE(countTlsBaseSeq(bytes), 1u)
        << "k must be read tp-relative";
}

// ── D-CSUBSET-THREAD-LOCAL: code-audit LOW-3 driver-tier pins ──────
// The walker-tier tests pin these mechanisms on hand-built modules;
// these three re-pin them through the FULL driver (grammar ->
// semantics -> HIR/MIR flags -> asm section-select -> merge -> the
// ELF dynamic walker) so a plumbing regression BETWEEN tiers cannot
// slip while both tiers' own tests stay green.

TEST(Program_CompileFiles, ThreadLocalPointerTemplateSlotDereferencesE2E) {
    // LOW-3(a) — the CRIT-2 shape end-to-end: `thread_local char *msg
    // = "hi";` — the .tdata TEMPLATE slot must hold, at LINK time, a
    // VA that (1) lies inside a mapped FILE-BACKED region and (2)
    // dereferences (via the phdr file<->VA congruence) to the "hi\0"
    // rodata bytes. A demoted-to-.data slot empties PT_TLS; an
    // unpatched slot holds 0; a tpoff-poisoned patch (the CRIT-1
    // class) holds a huge bit-cast negative — all three flip this red.
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "tls_ptr.c",
        "thread_local char *msg = \"hi\";\n"
        "int main(void) {\n"
        "    return (msg[0] == 'h' && msg[1] == 'i' && msg[2] == 0)\n"
        "               ? 42 : 1;\n"
        "}\n");
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileFiles(
        {src.generic_string()}, "c-subset",
        {"x86_64:elf64-x86_64-linux-exec"});
    ASSERT_EQ(rc, 0);

    auto const out =
        scratch.path() / "target" / "elf64-x86_64-linux-exec" / "tls_ptr";
    ASSERT_TRUE(fs::exists(out));
    auto const bytes = readAllBytes(out);

    std::size_t const tlsPh = findPtTls(bytes);
    ASSERT_NE(tlsPh, 0u);
    std::uint64_t const pOff    = rdU64(bytes, tlsPh + 8);
    std::uint64_t const pFilesz = rdU64(bytes, tlsPh + 32);
    ASSERT_EQ(pFilesz, 8u) << "the pointer slot is the whole template";

    // The slot's 8 template bytes = the link-time-patched target VA.
    std::uint64_t const slotVa = rdU64(bytes, static_cast<std::size_t>(pOff));
    ASSERT_NE(slotVa, 0u) << "template slot must be PATCHED, not zero";

    // Map slotVa -> file offset through the PT_LOADs (p_type 1): it
    // must fall inside a FILE-BACKED span (offset < p_filesz — the
    // .rodata region), never in a memsz-only tail.
    std::uint64_t const phoff = rdU64(bytes, 32);
    std::uint16_t const phnum = rdU16(bytes, 56);
    std::uint64_t fileOff = 0;
    bool mapped = false;
    for (std::uint16_t i = 0; i < phnum; ++i) {
        std::size_t const o = static_cast<std::size_t>(phoff) + i * 56u;
        if (bytes[o] != 1 || bytes[o + 1] != 0) continue;   // PT_LOAD
        std::uint64_t const lOff = rdU64(bytes, o + 8);
        std::uint64_t const lVa  = rdU64(bytes, o + 16);
        std::uint64_t const lFsz = rdU64(bytes, o + 32);
        if (slotVa >= lVa && slotVa < lVa + lFsz) {
            fileOff = slotVa - lVa + lOff;
            mapped = true;
            break;
        }
    }
    ASSERT_TRUE(mapped)
        << "the patched VA must lie inside a mapped file-backed "
           "PT_LOAD span (a bit-cast tpoff or garbage VA maps nowhere)";
    // Dereference: exactly 'h','i',0.
    ASSERT_GE(bytes.size(), fileOff + 3u);
    EXPECT_EQ(bytes[static_cast<std::size_t>(fileOff) + 0], 'h');
    EXPECT_EQ(bytes[static_cast<std::size_t>(fileOff) + 1], 'i');
    EXPECT_EQ(bytes[static_cast<std::size_t>(fileOff) + 2], 0u);
}

TEST(Program_CompileFiles, CrossCuExternThreadLocalMergesAndAccessesTpRelativeE2E) {
    // LOW-3(b) — the 2-CU shape: CU1 DEFINES `thread_local int g` and
    // mutates it; CU2 declares `extern thread_local int g` and reads
    // it, compiled as TWO translation units via `compileUnits` — the
    // CU6/LK11 shape the CLI routes every multi-source invocation to
    // (`routesToMultiUnit`), where the MIR merge's definedNames strip
    // unifies CU2's extern row onto CU1's definition (CRIT-3's
    // thread-storage carry included). A surviving TLS extern would
    // fail loud at the linker's initial-exec gate — pinned by
    // Linker.SurvivingThreadLocalExternImportRejectsLoud. PT_TLS must
    // be present, and BOTH CUs' accesses must go tp-relative: >= 2
    // `mov r64, fs:[0]` thread-pointer reads in the merged image (a
    // per-CU split where one CU silently fell back to an absolute
    // .data access would leave only one).
    //
    // Deliberately NOT `compileFiles`: with N>1 sources that API
    // builds ONE multi-FILE CU5 unit, whose extern-data resolution is
    // a different (API-only) surface — today it mints a surviving
    // import row for a sibling-FILE definition, which the TLS gate
    // then correctly rejects loud (observed while writing this pin;
    // flagged to the plan tier — not this test's subject).
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src1 = writeCSubsetSource(
        scratch.path(), "tls_cu1.c",
        "thread_local int g = 5;\n"
        "int bump(void) { g = g + 1; return g; }\n");
    auto const src2 = writeCSubsetSource(
        scratch.path(), "tls_cu2.c",
        "extern int bump(void);\n"
        "extern thread_local int g;\n"
        "int main(void) {\n"
        "    if (g != 5) return 1;\n"
        "    int r = bump();\n"
        "    if (r != 6) return 2;\n"
        "    if (g != 6) return 3;\n"
        "    return 42;\n"
        "}\n");
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileUnits(
        {src1.generic_string(), src2.generic_string()}, "c-subset",
        {"x86_64:elf64-x86_64-linux-exec"});
    ASSERT_EQ(rc, 0) << "extern thread_local must resolve across the "
                        "cross-CU merge (never survive to the "
                        "initial-exec linker gate)";

    auto const out =
        scratch.path() / "target" / "elf64-x86_64-linux-exec" / "tls_cu1";
    ASSERT_TRUE(fs::exists(out));
    auto const bytes = readAllBytes(out);

    std::size_t const tlsPh = findPtTls(bytes);
    ASSERT_NE(tlsPh, 0u);
    EXPECT_EQ(rdU64(bytes, tlsPh + 32), 4u);   // p_filesz — one int
    EXPECT_EQ(rdU64(bytes, tlsPh + 40), 4u);   // p_memsz
    EXPECT_GE(countTlsBaseSeq(bytes), 2u)
        << "both CUs' g accesses must read the thread pointer "
           "(fs:[0]) — a single occurrence means one CU bypassed TLS";
}

TEST(Program_CompileFiles, Alignas32ThreadLocalPAlignAndLayoutE2E) {
    // LOW-3(c) — the HIGH-1 _Alignas physics through the WHOLE driver.
    // Hand-derivation (declaration order = item order):
    //   big   {09 00 00 00} align 32 -> template offset 0
    //   small {07 00 00 00} align 4  -> alignUp(4,4) = 4, span 8
    //   tlsAlign = 32 -> PT_TLS p_align == 32
    //   p_filesz = p_memsz = 8 (no tbss part)
    //   (alignedBlockSize = alignUp(8,32) = 32 -> tpoffs big=-32,
    //   small=-28 — byte-pinned at the walker tier; here the phdr
    //   fields pin the same formula's inputs end-to-end.)
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "tls_align.c",
        "_Alignas(32) thread_local int big = 9;\n"
        "thread_local int small = 7;\n"
        "int main(void) { return big + small + 26; }\n");   // 9+7+26 = 42
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileFiles(
        {src.generic_string()}, "c-subset",
        {"x86_64:elf64-x86_64-linux-exec"});
    ASSERT_EQ(rc, 0);

    auto const out =
        scratch.path() / "target" / "elf64-x86_64-linux-exec" / "tls_align";
    ASSERT_TRUE(fs::exists(out));
    auto const bytes = readAllBytes(out);

    std::size_t const tlsPh = findPtTls(bytes);
    ASSERT_NE(tlsPh, 0u);
    std::uint64_t const pOff = rdU64(bytes, tlsPh + 8);
    EXPECT_EQ(rdU64(bytes, tlsPh + 32), 8u);   // p_filesz
    EXPECT_EQ(rdU64(bytes, tlsPh + 40), 8u);   // p_memsz
    EXPECT_EQ(rdU64(bytes, tlsPh + 48), 32u)   // p_align — THE pin
        << "the _Alignas(32) member must drive PT_TLS p_align (the "
           "tpoff formula divides by it — HIGH-1)";
    // Template bytes: big at 0, small at 4.
    std::size_t const t = static_cast<std::size_t>(pOff);
    EXPECT_EQ(bytes[t + 0], 9u);
    EXPECT_EQ(bytes[t + 4], 7u);
}
