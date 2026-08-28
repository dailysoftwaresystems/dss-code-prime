// ★★★ THE DRIVER'S SUPPLYING IS THE SUBJECT — NOT THE CALLEE'S BEHAVIOUR
// (D-TEST-STATIC-LINK-UNIT-SUITE-CANNOT-WITNESS-A-DRIVER-THREADING-GAP).
//
// ── WHAT THIS FILE IS FOR ───────────────────────────────────────────────────
// `src/program/compile_pipeline.hpp` exports the pipeline kernel. Several of
// its entry points take a parameter whose correct value is DERIVED by
// `src/program/program.cpp` — from a CLI argument, from the loaded
// `.lang`/`.target`/`.format` document, or from earlier pipeline state. A unit
// case that calls such an entry point directly CONSTRUCTS that argument itself,
// so it is testing the callee while ASSUMING the caller. Nothing in the
// in-process unit suite can then witness the caller failing to supply it.
//
// That assumption was FALSIFIED, by a mutant, in cycle P22: dropping a
// newly-threaded parameter at ONE driver call site reproduced the original CLI
// failure exactly while the entire in-process suite stayed green at 32/32. The
// header's classification block names the criterion and every entry point it
// applies to; this file holds the driver-level pins that block was written to
// cite, for the routes that had none.
//
// ── THE CRITERION EACH PIN HERE ANSWERS ─────────────────────────────────────
// A parameter is DRIVER-SUPPLIED when all three hold:
//   1. its correct value is DERIVED by the driver, not handed to it;
//   2. a WRONG value COMPILES — a default argument, an empty span, `nullopt`,
//      a zero ordinal, or a same-typed sibling is available at the call site;
//   3. the wrong value still produces a BUILD. The loss is a dropped
//      capability, a wrong ABI or a silently different schedule, never a
//      diagnostic.
// Clause 3 is what makes these pins necessary rather than redundant: a
// mis-supply that failed loud would already be caught by any test that
// compiles anything.
//
// ── AND THE CLASSIFICATION IS PER **ROUTE**, NOT PER ENTRY POINT ────────────
// This is the finding that produced this file. `program.cpp` reaches
// `linkAndWriteWithStaticArchives` by THREE routes (assembly/`encode` tier,
// N==1 sole CU, N>1 merged) and `optimizeModule` by THREE (archive member,
// N==1, merged). A pin on ONE route says NOTHING about the others — each is a
// separate argument list that a later edit can change independently, which is
// exactly how a parameter gets added to two of three call sites. Every pin
// below therefore names its route.
//
// ── EACH PIN CARRIES ITS OWN CONTROL ARM ────────────────────────────────────
// A refusal is only evidence that the driver supplied the parameter if the
// SAME fixture builds green without the request. Otherwise a broken fixture
// reds and reads as a passing pin. So every request pin below compiles its
// source twice: once with no request (must succeed) and once with the request
// (must be refused, by CODE — never merely a non-zero exit).
//
// ⚠ THIS FILE MUST NOT BECOME A DETECTOR. The registry row forbids turning
// this classification into a gate, and states why: what a unit test may
// legitimately construct is a JUDGEMENT, and a detector would need an
// allowlist that re-states the convention in the place least likely to be read.
// These are ordinary pins over named routes, and the judgement lives in the
// header beside the exports it classifies.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/substrate/phase_timers.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using dss::CompileConfig;
using dss::DiagnosticCode;
using dss::DiagnosticReporter;
using dss::Program;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// The request value used by every `imageRequest` pin: 1 byte.
//
// ★ CHOSEN SO THE REFUSAL IS DECLARED RATHER THAN INCIDENTAL. ✔MEASURED over
// the shipped `object-formats/` corpus: `pe64-x86_64-windows-exec` is the ONLY
// format declaring `stackReserveControl` (minimumBytes 65536, granularityBytes
// 4096); every other shipped format declares `stackReserveUnsupportedReason`
// instead. So 1 byte is below the pe64 exec minimum — `K_InvalidStackReserveRequest`
// — and is refused on every other format with `K_FormatLacksStackReserveControl`.
// One value, two declared refusals, no format-identity branch in the test.
constexpr std::uint64_t kBelowEveryMinimum = 1u;

constexpr std::string_view kPeExecSpec = "x86_64:pe64-x86_64-windows-exec";
constexpr std::string_view kElfStaticLibSpec =
    "x86_64:elf64-x86_64-linux-staticlib";
constexpr std::string_view kAttAsmLanguage = "asm-x86_64-att";

fs::path writeSrc(fs::path const& dir, std::string_view name,
                  std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream f(p, std::ios::binary);
    f << text;
    return p;
}

[[nodiscard]] std::string allDiagnosticText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        out += d.contextPrefix;
        out += ' ';
        out += d.actual;
        out += '\n';
    }
    return out;
}

[[nodiscard]] bool sawCode(DiagnosticReporter const& rep, DiagnosticCode c) {
    for (auto const& d : rep.all()) {
        if (d.code == c) return true;
    }
    return false;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// `ImageRequest` — the per-PROGRAM image knobs (D-SQLITE-PE64-FULL-TIER-STACK-DEPTH),
// DERIVED by the driver from `--stack-reserve` / a project
// manifest's `stackReserve` key.
// ════════════════════════════════════════════════════════════════════════════
//
// ★★ IT IS A **DEFAULTED** PARAMETER on all three link entry points
// (`linkAndWrite`, `linkAndWriteWithStaticArchives`, `linkAndWriteStaticArchive`),
// which is precisely clause 2 of the criterion: a driver route that simply
// omits the argument COMPILES, links, writes the artifact, and reports success
// — while the operator's request has vanished. At runtime that is
// indistinguishable from never having asked, which is the whole class of defect
// the capability gate exists to close.
//
// The driver threads it to FOUR call sites. Before this file exactly ONE of
// them had a driver-level pin — `program/test_artifact_report`
// `ArtifactReport.ALinkFailureAfterThePathIsKnownReportsNoArtifact`, which
// takes the N==1 sole-CU route. The three below were unwitnessed.

// ROUTE: N>1 whole-program merge → `linkAndWriteWithStaticArchives`.
TEST(DriverArgumentSupply, MergedMultiCuRouteSuppliesTheImageRequest) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-supply"};
    scratch.useAsCwd();
    auto const a = writeSrc(scratch.path(), "merged_a.c",
                            "extern int helper(int x);\n"
                            "int main(void) { return helper(21) - 42; }\n");
    auto const b = writeSrc(scratch.path(), "merged_b.c",
                            "int helper(int x) { return x + x; }\n");
    std::vector<std::string> const files{a.generic_string(), b.generic_string()};

    // Control: the same two CUs, no request — the merged route must build.
    {
        Program prog;
        prog.setOutputDir(scratch.path() / "merged_ok");
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileUnits(files, "c",
                                    {std::string{kPeExecSpec}}, rep), 0)
            << "control arm: the N>1 merged route must build with no request; "
               "a red here means the FIXTURE is broken, not the pin\n"
            << allDiagnosticText(rep);
    }

    // The pin: the request must reach the merged route's link.
    Program prog;
    prog.setOutputDir(scratch.path() / "merged_req");
    prog.setStackReserveBytes(kBelowEveryMinimum);
    DiagnosticReporter rep;
    int const rc = prog.compileUnits(files, "c",
                                     {std::string{kPeExecSpec}}, rep);
    EXPECT_NE(rc, 0)
        << "the N>1 merged route must REFUSE an out-of-range stack reserve; a "
           "zero exit means `imageRequest` never reached its "
           "`linkAndWriteWithStaticArchives` call\n"
        << allDiagnosticText(rep);
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::K_InvalidStackReserveRequest))
        << "the refusal must be the DECLARED capability gate, by code — any "
           "other failure would satisfy a bare non-zero-exit assertion while "
           "the request was still being dropped\n"
        << allDiagnosticText(rep);
}

// ROUTE: static-library artifact → `linkAndWriteStaticArchive`.
//
// ★ THIS ROUTE'S CONTRACT IS THE REFUSAL ITSELF. `linkAndWriteStaticArchive`'s
// docblock states it: no relocatable/archive format declares a stack-reserve
// capability (an archive carries no image headers at all), so a request routed
// here is REFUSED — "accepting the parameter and ignoring it is what would let
// the request vanish on a `staticlib` target". A driver that omitted the
// argument here would produce exactly the vanishing the sentence forbids, and
// would produce it SILENTLY: the archive still builds.
TEST(DriverArgumentSupply, StaticArchiveRouteSuppliesTheImageRequest) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-supply"};
    scratch.useAsCwd();
    auto const src = writeSrc(scratch.path(), "libmember.c",
                              "int lib_answer(void) { return 42; }\n");
    std::vector<std::string> const files{src.generic_string()};

    {
        Program prog;
        prog.setOutputDir(scratch.path() / "lib_ok");
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileUnits(files, "c",
                                    {std::string{kElfStaticLibSpec}}, rep), 0)
            << "control arm: the staticlib route must build with no request\n"
            << allDiagnosticText(rep);
    }

    Program prog;
    prog.setOutputDir(scratch.path() / "lib_req");
    prog.setStackReserveBytes(kBelowEveryMinimum);
    DiagnosticReporter rep;
    int const rc = prog.compileUnits(files, "c",
                                     {std::string{kElfStaticLibSpec}}, rep);
    EXPECT_NE(rc, 0)
        << "the staticlib route must REFUSE a stack reserve it cannot carry; a "
           "zero exit means `imageRequest` never reached "
           "`linkAndWriteStaticArchive` and the request vanished\n"
        << allDiagnosticText(rep);
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::K_FormatLacksStackReserveControl))
        << "the archive format declares no `stackReserveControl`, so the "
           "refusal must name that missing capability by code\n"
        << allDiagnosticText(rep);
}

// ROUTE: the `encode` tier (a hand-written `.s`) → `assembleAsmUnit` then
// `linkAndWriteWithStaticArchives`.
//
// ★ THE ROUTE WITH THE MOST HISTORY OF DROPPING THINGS. It is the one that
// already lost `--resolve-library` silently on BOTH halves
// (D-ASM-EXTERN-CALL-CANNOT-BIND-A-LIBRARY +
// D-ASM-RESOLVE-LIBRARY-SILENTLY-IGNORED-ON-ENCODE-TIER): the flag was
// accepted by the parser and handed to nobody. It reaches the same link entry
// point as the C routes and carries the same defaulted parameter.
TEST(DriverArgumentSupply, AsmEncodeRouteSuppliesTheImageRequest) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-supply"};
    scratch.useAsCwd();
    constexpr std::string_view kAsmMain =
        "\t.text\n"
        "\t.globl\tmain\n"
        "\t.type\tmain, @function\n"
        "main:\n"
        "\tsubq\t$40, %rsp\n"
        "\tmovl\t$0, %eax\n"
        "\taddq\t$40, %rsp\n"
        "\tret\n";
    auto const src = writeSrc(scratch.path(), "encode_main.s", kAsmMain);
    std::vector<std::string> const files{src.generic_string()};

    {
        Program prog;
        prog.setOutputDir(scratch.path() / "asm_ok");
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileFiles(files, std::string{kAttAsmLanguage},
                                    {std::string{kPeExecSpec}}, rep), 0)
            << "control arm: the encode tier must build this `.s` with no "
               "request\n"
            << allDiagnosticText(rep);
    }

    Program prog;
    prog.setOutputDir(scratch.path() / "asm_req");
    prog.setStackReserveBytes(kBelowEveryMinimum);
    DiagnosticReporter rep;
    int const rc = prog.compileFiles(files, std::string{kAttAsmLanguage},
                                     {std::string{kPeExecSpec}}, rep);
    EXPECT_NE(rc, 0)
        << "the encode tier's link must REFUSE an out-of-range stack reserve; "
           "a zero exit means the assembly route dropped `imageRequest`\n"
        << allDiagnosticText(rep);
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::K_InvalidStackReserveRequest))
        << "the refusal must be the declared capability gate, by code\n"
        << allDiagnosticText(rep);
}

// ════════════════════════════════════════════════════════════════════════════
// `PipelineStage` — WHICH optimizer schedule runs at this call site
// (D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE).
// ════════════════════════════════════════════════════════════════════════════
//
// The DRIVER owns the slots; the config document owns what runs in each. So the
// stage ordinal is a driver-derived argument in the strictest sense: it is
// STRUCTURAL knowledge about which call site this is, and nothing downstream
// can check it. Passing `Unit` at a program site, or deleting the program-stage
// call entirely, compiles and still produces a working artifact — one optimized
// at the unit schedule only.
//
// ★★ THE INSTRUMENT IS `PhaseTimers::read(Optimize).runs`, AND IT IS EXACT.
// The `CompilePhase::Optimize` scope is opened INSIDE `optimizeModule`, so the
// count is the number of `optimizeModule` INVOCATIONS — not a proxy for it.
// Delete a driver's program-stage call and the count drops by exactly one,
// independent of which passes either schedule happens to contain.
//
// The archive-member route already had this pin — `program/test_static_link`
// `StaticArchive.ReleaseMemberIsProgramStageOptimized`. The other two driver
// sites did not: the two tests that look like they cover them,
// `Program_WholeProgramMerge.ShippedStageRoutingInlinesCrossCuAtProgramStage`
// and `Program_WholeProgramMerge.UnitStageRunsTheUnitDocumentNotTheConfigDoc`,
// call `buildCuMir` / `optimizeModule` DIRECTLY and re-derive `ccIndex` and the
// stage themselves. Their `Program_` suite prefix names the subject, not the
// tier — they are unit cases, and by construction cannot see a driver that
// stopped making the call.

// ROUTE: N==1 sole CU (`compileFiles`) → the pre-lower program-stage optimize.
TEST(DriverArgumentSupply, SingleCuRouteRunsTheProgramStageOptimize) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-supply"};
    scratch.useAsCwd();
    auto const src = writeSrc(
        scratch.path(), "solecu.c",
        "int fold(int a) { return (2 + 3) * a; }\n"
        "int main(void) { return fold(1) + fold(2) - 15; }\n");

    dss::substrate::PhaseTimers::reset();
    Program prog;
    prog.setCompileConfig(CompileConfig::Release);
    prog.setOutputDir(scratch.path() / "solecu_out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileFiles({src.generic_string()}, "c",
                                {std::string{kPeExecSpec}}, rep), 0)
        << allDiagnosticText(rep);

    EXPECT_EQ(dss::substrate::PhaseTimers::read(
                  dss::substrate::CompilePhase::Optimize).runs,
              2u)
        << "a 1-CU exec build must run Optimize TWICE — the UNIT stage inside "
           "`buildCuMir` and the PROGRAM stage the driver makes before "
           "`lowerCuMirToAssembly`. Reading 1 means the sole-CU route's "
           "program-stage call is gone: the artifact still builds, optimized "
           "at the unit schedule only (D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE)";
}

// ROUTE: N>1 whole-program merge (`compileUnits`) → the merged program-stage
// optimize. TWO unit stages (one per CU, on the pool) plus ONE program stage.
TEST(DriverArgumentSupply, MergedMultiCuRouteRunsTheProgramStageOptimize) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-supply"};
    scratch.useAsCwd();
    auto const a = writeSrc(scratch.path(), "stage_a.c",
                            "extern int helper(int x);\n"
                            "int main(void) { return helper(21) - 42; }\n");
    auto const b = writeSrc(scratch.path(), "stage_b.c",
                            "int helper(int x) { return x + x; }\n");

    dss::substrate::PhaseTimers::reset();
    Program prog;
    prog.setCompileConfig(CompileConfig::Release);
    prog.setOutputDir(scratch.path() / "merged_stage_out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileUnits({a.generic_string(), b.generic_string()},
                                "c", {std::string{kPeExecSpec}}, rep), 0)
        << allDiagnosticText(rep);

    EXPECT_EQ(dss::substrate::PhaseTimers::read(
                  dss::substrate::CompilePhase::Optimize).runs,
              3u)
        << "a 2-CU merged exec build must run Optimize THREE times — one UNIT "
           "stage per CU plus the PROGRAM stage over the merged module. "
           "Reading 2 means the merged route's program-stage call is gone, and "
           "cross-CU inlining (the whole point of the merge) silently stops "
           "happening while the binary still builds and still runs";
}

// ════════════════════════════════════════════════════════════════════════════
// `formatVerbs` — the active object format's DECLARED entry-materialization
// verbs, supplied to `resolveProgramEntry` by the merged driver path
// (D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE).
// ════════════════════════════════════════════════════════════════════════════
//
// ★★★ THE WORST FAILURE SHAPE IN THIS FILE, AND IT IS SILENT.
// `resolveProgramEntry` treats an EMPTY `formatVerbs` span as a DECLARED
// answer — "this format starts no program" — and then resolves the entry BY
// NAME with no verb requirement and NO ambiguity check, returning candidate
// index 0. That arm is correct and load-bearing for relocatable / staticlib
// builds. But it means a driver that supplied `{}` instead of
// `format.entryVerbs()` would take it on an EXEC format too, and a program
// defining both `main` and `wmain` would then get FIRST-CANDIDATE-WINS with no
// diagnostic — the exact pre-`resolveProgramEntry` defect its docblock records
// ("`main` in a.c and `wmain` in b.c silently picked one").
//
// A wrong program entry is the worst outcome available at this seam, and every
// existing witness of the intersection rule is a UNIT fixture in
// `tests/mir/test_mir_merge.cpp` that CONSTRUCTS the verb span itself.
//
// ★ WHY `main` + `wmain` AND NOT ANY OTHER PAIR: `pe64-x86_64-windows-exec` is
// the only shipped format whose declared `entryVerbs` contain `argc-wargv`, so
// this is the one pair that is BOTH realizable on one format — making the
// correct answer `K_ProgramEntryAmbiguous` — and reduced to first-match-wins by
// an empty span. On ELF the `wmain` row does not survive the intersection at
// all, so the same source there resolves to `main` and witnesses nothing.
TEST(DriverArgumentSupply, MergedMultiCuRouteSuppliesTheFormatsEntryVerbs) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-supply"};
    scratch.useAsCwd();
    auto const a = writeSrc(scratch.path(), "entry_main.c",
                            "int main(int argc, char** argv) {\n"
                            "    return argc + (argv != 0);\n"
                            "}\n");
    auto const b = writeSrc(scratch.path(), "entry_wmain.c",
                            "int wmain(int argc, unsigned short** argv) {\n"
                            "    return argc + (argv != 0);\n"
                            "}\n");

    Program prog;
    prog.setOutputDir(scratch.path() / "entry_out");
    DiagnosticReporter rep;
    int const rc = prog.compileUnits({a.generic_string(), b.generic_string()},
                                     "c", {std::string{kPeExecSpec}},
                                     rep);
    EXPECT_NE(rc, 0)
        << "two realizable program entries in one pe64 program must be "
           "REFUSED; a zero exit means one of them was silently chosen\n"
        << allDiagnosticText(rep);
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::K_ProgramEntryAmbiguous))
        << "the merged driver path must hand `resolveProgramEntry` the "
           "FORMAT's declared entryVerbs. An empty span takes the "
           "'this format starts no program' arm, which resolves by name with "
           "no ambiguity check and returns candidate 0 — first-match-wins on "
           "an exec format, with no diagnostic\n"
        << allDiagnosticText(rep);
}

// ════════════════════════════════════════════════════════════════════════════
// `lowerMergedToAssembly` — the LARGEST concentration of driver-derived
// arguments in the kernel, and the least-travelled route to them.
// ════════════════════════════════════════════════════════════════════════════
//
// ★★★ WHY THIS SECTION IS BIGGER THAN THE REST OF THE FILE. The entry point
// takes FOURTEEN arguments, TEN of them driver-derived, and SIX of those are
// optionals or containers whose EMPTY value both compiles and is the correct
// answer for some shipped format — so no callee-side check can tell "this
// format declares none" from "the driver dropped it". It is reachable only
// through `Program::compileUnits` with ≥2 sources; ✔MEASURED by walking
// `examples/` for directories carrying an `expected.json` and counting their
// `.c`/`.s` files, 22 of 613 shipped corpus example manifests have ≥2 such
// sources, so the corpus exercises this route about 3.6% as often as the
// single-CU one.
//
// ★★ EACH PIN BELOW ASSERTS THE OBSERVABLE CONSEQUENCE IN THE PRODUCED IMAGE,
// NEVER THE ARGUMENT'S SHAPE. A pin phrased as "the container is non-empty"
// would be FALSE on every format that legitimately declares none — and, worse,
// it would be asserting the driver's own arithmetic back at itself. What the
// merged route owes is a correct ARTIFACT, so that is what is measured: bytes
// in the emitted image, or a relocation type, or a declared library name.
//
// ★★ AND EVERY ONE OF THEM WAS MEASURED BY MUTATING THE DRIVER, NOT BY READING
// IT. ✔2026-08-20, one mutation at a time at the `lowerMergedToAssembly` CALL
// SITE in `src/program/program.cpp`, each rebuilt in an isolated tree with the
// subject DLL's mtime asserted to have advanced on BOTH the mutate and the
// restore, and `src/program/program.cpp` restored byte-identically (sha256
// re-checked) each time. SIX of the eight mutants produce a SILENT WRONG
// ARTIFACT — exit 0, no diagnostic, a binary that builds and, where it can be
// run, answers wrong; the other TWO (`externCallDispatch`,
// `wideFloatSoftcallLibrary`) refuse at MIR→LIR rather than guess, so their
// pins are red-on-mutation by failing to BUILD. The measured consequence is
// quoted on each pin. That is the class this file exists for: nothing in the
// callee's unit suite can see any of it, because every unit case constructs the
// argument the driver is supposed to derive — ✔MEASURED, `lir/test_mir_to_lir`
// and `mir/test_mir_merge` stayed GREEN under all eight.

namespace {

// ── Byte readers, shared by the image pins ─────────────────────────────────
// Bounds-safe: an out-of-range read yields 0 rather than UB, so a truncated or
// unexpected artifact makes a pin fail with its own message instead of
// crashing the suite.
[[nodiscard]] std::uint16_t rdU16(std::vector<std::uint8_t> const& b,
                                  std::size_t o) {
    if (o + 2 > b.size()) return 0;
    return static_cast<std::uint16_t>(static_cast<unsigned>(b[o])
                                      | (static_cast<unsigned>(b[o + 1]) << 8));
}
[[nodiscard]] std::uint32_t rdU32(std::vector<std::uint8_t> const& b,
                                  std::size_t o) {
    if (o + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[o])
           | (static_cast<std::uint32_t>(b[o + 1]) << 8)
           | (static_cast<std::uint32_t>(b[o + 2]) << 16)
           | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}
[[nodiscard]] std::uint64_t rdU64(std::vector<std::uint8_t> const& b,
                                  std::size_t o) {
    if (o + 8 > b.size()) return 0;
    return static_cast<std::uint64_t>(rdU32(b, o))
           | (static_cast<std::uint64_t>(rdU32(b, o + 4)) << 32);
}

[[nodiscard]] std::vector<std::uint8_t> readAllBytes(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>()};
}

// Every offset at which `needle` occurs in `hay`. Returned as a LIST, not a
// count and not a first-hit, so a pin can assert BOTH "it is there" and "it is
// there exactly once" — an anchor that turns out to be ambiguous makes the pin
// say so instead of silently measuring the wrong occurrence.
[[nodiscard]] std::vector<std::size_t>
findAll(std::vector<std::uint8_t> const& hay,
        std::span<std::uint8_t const>    needle) {
    std::vector<std::size_t> hits;
    if (needle.empty() || hay.size() < needle.size()) return hits;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        if (std::equal(needle.begin(), needle.end(),
                       hay.begin() + static_cast<std::ptrdiff_t>(i))) {
            hits.push_back(i);
        }
    }
    return hits;
}

[[nodiscard]] std::size_t countBytes(std::vector<std::uint8_t> const& hay,
                                     std::string_view                 needle) {
    std::vector<std::uint8_t> n(needle.begin(), needle.end());
    return findAll(hay, std::span<std::uint8_t const>{n}).size();
}

[[nodiscard]] std::string hexWindow(std::vector<std::uint8_t> const& b,
                                    std::size_t at, std::size_t n) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = at; i < at + n && i < b.size(); ++i) {
        out += kDigits[b[i] >> 4];
        out += kDigits[b[i] & 0x0F];
        out += ' ';
    }
    return out;
}

// ── The shared 2-CU merged build ───────────────────────────────────────────
// ★ THE SECOND SOURCE IS NOT DECORATION. `Program::compileUnits` routes on the
// CU COUNT: one source takes the sole-CU path and NEVER reaches
// `lowerMergedToAssembly` at all, so a fixture that lost its second file would
// keep passing while testing a different route entirely. Each pin therefore
// hands this helper two files and asserts on the merged artifact.
[[nodiscard]] int buildMergedPair(ScratchDir& scratch, std::string_view nameA,
                                  std::string_view srcA, std::string_view nameB,
                                  std::string_view    srcB,
                                  std::string_view    targetSpec,
                                  fs::path const&     outDir,
                                  DiagnosticReporter& rep) {
    auto const a = writeSrc(scratch.path(), nameA, srcA);
    auto const b = writeSrc(scratch.path(), nameB, srcB);
    Program prog;
    prog.setOutputDir(outDir);
    return prog.compileUnits({a.generic_string(), b.generic_string()},
                             "c", {std::string{targetSpec}}, rep);
}

// ── ELF64: count relocations of one wire type ──────────────────────────────
// Walks every SHT_RELA section rather than looking `.rela.text` up by name: the
// section NAME is a convention, the section TYPE is the format. Returns 0 for
// anything that is not an ELF64 little-endian object, so a pin that pointed at
// the wrong artifact fails on its count rather than reading garbage.
[[nodiscard]] std::size_t elfRelaTypeCount(std::vector<std::uint8_t> const& img,
                                           std::uint32_t wireType) {
    constexpr std::uint32_t kShtRela      = 4;
    constexpr std::size_t   kRelaEntry    = 24;   // r_offset, r_info, r_addend
    constexpr std::size_t   kShdrTypeOff  = 4;
    constexpr std::size_t   kShdrOffOff   = 0x18;
    constexpr std::size_t   kShdrSizeOff  = 0x20;
    if (img.size() < 0x40) return 0;
    if (!(img[0] == 0x7F && img[1] == 'E' && img[2] == 'L' && img[3] == 'F'
          && img[4] == 2 && img[5] == 1)) {
        return 0;
    }
    std::uint64_t const shoff     = rdU64(img, 0x28);
    std::uint16_t const shentsize = rdU16(img, 0x3A);
    std::uint16_t const shnum     = rdU16(img, 0x3C);
    std::size_t         hits      = 0;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::size_t const o = static_cast<std::size_t>(shoff) + i * shentsize;
        if (rdU32(img, o + kShdrTypeOff) != kShtRela) continue;
        std::size_t const off = static_cast<std::size_t>(rdU64(img, o + kShdrOffOff));
        std::size_t const sz  = static_cast<std::size_t>(rdU64(img, o + kShdrSizeOff));
        for (std::size_t e = 0; e + kRelaEntry <= sz; e += kRelaEntry) {
            // r_info's LOW 32 bits are the type; the high 32 are the symbol
            // index (ELF64 splits them the opposite way round from ELF32).
            if (static_cast<std::uint32_t>(rdU64(img, off + e + 8) & 0xFFFFFFFFu)
                == wireType) {
                ++hits;
            }
        }
    }
    return hits;
}

// ── PE: how many functions carry an EXCEPTION HANDLER in their unwind info ──
// Walks `.pdata`'s RUNTIME_FUNCTION array, follows each entry's UnwindInfo RVA
// into `.xdata`, and reads the flags nibble. `outTotal` receives the number of
// entries actually resolved, so a pin can guard its own PREMISE (a fixture that
// emitted no unwind info at all would otherwise report "0 handlers" and read
// exactly like the defect).
struct PeSection {
    std::string   name;
    std::uint32_t vaddr = 0, vsize = 0, rawPtr = 0, rawSize = 0;
};

[[nodiscard]] std::vector<PeSection>
peSections(std::vector<std::uint8_t> const& img) {
    std::vector<PeSection> out;
    if (img.size() < 0x40) return out;
    std::size_t const peOff = rdU32(img, 0x3C);
    if (peOff + 24 > img.size()) return out;
    if (!(img[peOff] == 'P' && img[peOff + 1] == 'E' && img[peOff + 2] == 0
          && img[peOff + 3] == 0)) {
        return out;
    }
    std::uint16_t const nsec    = rdU16(img, peOff + 6);
    std::uint16_t const optSize = rdU16(img, peOff + 20);
    std::size_t const   secOff  = peOff + 24 + optSize;
    for (std::uint16_t i = 0; i < nsec; ++i) {
        std::size_t const o = secOff + static_cast<std::size_t>(i) * 40;
        if (o + 40 > img.size()) break;
        PeSection s;
        for (std::size_t k = 0; k < 8 && img[o + k] != 0; ++k) {
            s.name.push_back(static_cast<char>(img[o + k]));
        }
        s.vsize   = rdU32(img, o + 8);
        s.vaddr   = rdU32(img, o + 12);
        s.rawSize = rdU32(img, o + 16);
        s.rawPtr  = rdU32(img, o + 20);
        out.push_back(std::move(s));
    }
    return out;
}

[[nodiscard]] std::size_t peRvaToOffset(std::vector<PeSection> const& secs,
                                        std::uint32_t                 rva) {
    for (auto const& s : secs) {
        std::uint32_t const span = std::max(s.vsize, s.rawSize);
        if (rva >= s.vaddr && rva < s.vaddr + span) {
            return static_cast<std::size_t>(rva - s.vaddr) + s.rawPtr;
        }
    }
    return 0;
}

[[nodiscard]] std::size_t peFunctionsWithExceptionHandler(
    std::vector<std::uint8_t> const& img, std::size_t& outTotal) {
    // UNWIND_INFO's first byte packs Version in bits 0..2 and Flags in bits
    // 3..7; UNW_FLAG_EHANDLER is flag bit 0, i.e. 0x08 of that byte.
    constexpr std::uint8_t kUnwFlagEHandler = 0x08;
    constexpr std::size_t  kRuntimeFunction = 12;  // begin, end, unwind RVAs
    outTotal = 0;
    auto const secs = peSections(img);
    for (auto const& s : secs) {
        if (s.name != ".pdata") continue;
        std::size_t const sz = std::min<std::size_t>(s.vsize, s.rawSize);
        std::size_t       withHandler = 0;
        for (std::size_t e = 0; e + kRuntimeFunction <= sz; e += kRuntimeFunction) {
            std::uint32_t const unwindRva = rdU32(img, s.rawPtr + e + 8);
            std::size_t const   at        = peRvaToOffset(secs, unwindRva);
            if (at == 0 || at >= img.size()) continue;
            ++outTotal;
            if ((img[at] & kUnwFlagEHandler) != 0) ++withHandler;
        }
        return withHandler;
    }
    return 0;
}

constexpr std::string_view kElfArm64ExecSpec  = "arm64:elf64-aarch64-linux-exec";
constexpr std::string_view kElfArm64RelocSpec = "arm64:elf64-aarch64-linux";

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// `bitFieldStrategy` — the FORMAT-resolved C bit-field packing rule
// (D-CSUBSET-BITFIELD-ABI-EXACT), overlaid onto the target's
// `AggregateLayoutParams` before the merged module's globals are laid out.
// ════════════════════════════════════════════════════════════════════════════
//
// ★★★ THE MUTANT THAT MOTIVATES THIS PIN IS NOT A DROPPED ARGUMENT — IT IS THE
// PLAUSIBLE-LOOKING ONE. `effectiveBitFieldStrategy(target, format)` resolves
// FORMAT-first with the TARGET's field as the back-compat fallback. Writing
// `target.aggregateLayout().bitFieldStrategy` at the call site compiles, reads
// like the obvious thing, and is RIGHT on every ELF and Mach-O leg — ✔MEASURED,
// `x86_64.target.json` declares `gnu_packed` and `elf64-x86_64-linux-exec`
// declares `gnu_packed`, so the two agree. It is wrong on exactly one axis: PE,
// where `pe64-x86_64-windows-exec.format.json` declares `msvc_straddle`.
//
// ★★ AND THE SEMANTIC TIER DOES NOT NOTICE, WHICH IS THE WHOLE DEFECT. The
// driver overlays this strategy at THREE consumer sites (analyze / HIR→MIR /
// asm globals). `lowerMergedToAssembly` is the third. Mis-supply it there alone
// and the CODE still reads the field at the format-correct offset while the
// GLOBAL'S BYTES were laid out at a different one — a silent disagreement
// inside one image, with no diagnostic anywhere.
//
// ★ THE STRUCT IS CHOSEN, NOT ARBITRARY. `{int a:1; char b:1; long long c;}` is
// SIXTEEN BYTES UNDER BOTH STRATEGIES, so the read stays in bounds and the
// difference is purely WHERE `b` lives: msvc_straddle opens a fresh unit at the
// type-size change (b at byte 4, bit 0) while gnu_packed shares the int unit (b
// at byte 0, bit 1). A struct whose SIZE differed would make the mutant's
// artifact fail for a second reason and the pin would stop discriminating.
//
// ✔MEASURED 2026-08-20 by planting the mutant at the driver and running the
// emitted pe64 binary: exit 42 → exit 41, rc 0, ZERO diagnostics. The 16 global
// bytes went `01 00 00 00 01 00 00 00 88 77 66 55 44 33 22 11` (correct) →
// `03 00 00 00 00 00 00 00 88 77 66 55 44 33 22 11` (gnu_packed).
TEST(DriverArgumentSupply, MergedMultiCuRouteSuppliesTheFormatsBitFieldStrategy) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-bitfield"};
    scratch.useAsCwd();
    auto const out = scratch.path() / "bitfield_out";
    DiagnosticReporter rep;
    ASSERT_EQ(buildMergedPair(
                  scratch, "bitfield_a.c",
                  "struct S { int a : 1; char b : 1; long long c; };\n"
                  "struct S bf_g = { 1, 1, 0x1122334455667788LL };\n"
                  "int bf_read_b(void) { return (int)bf_g.b; }\n",
                  "bitfield_b.c",
                  "extern int bf_read_b(void);\n"
                  "int main(void) { return bf_read_b() + 41; }\n",
                  kPeExecSpec, out, rep),
              0)
        << "the 2-CU pe64 bit-field build must succeed; a red here means the "
           "FIXTURE is broken, not the pin\n"
        << allDiagnosticText(rep);

    auto const img = readAllBytes(out / "bitfield_a.exe");
    ASSERT_FALSE(img.empty()) << "the merged pe64 artifact must exist";

    // The trailing `long long` is the ANCHOR: it is unaffected by either
    // strategy, so it locates the global without depending on section layout.
    constexpr std::uint8_t kAnchor[] = {0x88, 0x77, 0x66, 0x55,
                                        0x44, 0x33, 0x22, 0x11};
    auto const hits = findAll(img, std::span<std::uint8_t const>{kAnchor});
    ASSERT_EQ(hits.size(), 1u)
        << "the anchor qword must appear EXACTLY once — more than one and this "
           "pin would be measuring some other bytes";
    ASSERT_GE(hits[0], 8u);

    // msvc_straddle: `a` in the int unit at byte 0 bit 0, `b` in a FRESH
    // char-aligned unit at byte 4 bit 0.
    constexpr std::uint8_t kMsvcStraddle[] = {0x01, 0x00, 0x00, 0x00,
                                              0x01, 0x00, 0x00, 0x00};
    std::vector<std::uint8_t> const got(img.begin()
                                            + static_cast<std::ptrdiff_t>(hits[0] - 8),
                                        img.begin()
                                            + static_cast<std::ptrdiff_t>(hits[0]));
    EXPECT_TRUE(std::equal(std::begin(kMsvcStraddle), std::end(kMsvcStraddle),
                           got.begin()))
        << "the merged route must hand the lower half the FORMAT-resolved "
           "bit-field strategy. Reading `03 00 00 00 00 00 00 00` here is "
           "gnu_packed — the TARGET's back-compat fallback, which the pe64 "
           "format overrides — and it is a SILENT MISCOMPILE: the code reads "
           "`b` at byte 4 while the global stored it at byte 0 bit 1. Got: "
        << hexWindow(img, hits[0] - 8, 16);
}

// ════════════════════════════════════════════════════════════════════════════
// `dataModel` — the FORMAT's data model, threaded into the merged module's
// global-data layout (D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL).
// ════════════════════════════════════════════════════════════════════════════
//
// ★★★ THE MEASUREMENT THAT SHAPED THIS PIN, AND IT NARROWS THE ARGUMENT'S
// REACH RATHER THAN WIDENING IT. At this seam `dataModel` carries EXACTLY ONE
// dimension: the POINTER WIDTH. `scalarByteSize` consults it for `Ptr`/`Ref`/
// `FnPtr`/`NullptrT` and for nothing else — every other scalar's width is
// already baked into its TypeKind by the front end, so `long` has become I32 or
// I64 long before the merged lower sees it. ⇒ **LP64 and LLP64 are BYTE-
// IDENTICAL here.** ✔MEASURED 2026-08-20 by mutating the driver to hand the
// merged route `DataModel::Lp64` on a pe64 (LLP64) build: all three probe
// artifacts came back byte-for-byte identical to the unmutated baseline. A pin
// that used the other 64-bit model as its mutant would be GREEN OVER A LIVE
// MUTATION — the exact vacuity this file exists to prevent.
//
// So the discriminating sibling is `Ilp32`, which two shipped formats really do
// declare (`wasm32-v1`, `spirv-1.6`) — a same-typed value sitting in scope, and
// clause 2 of the criterion in its literal form.
//
// ✔MEASURED with that mutant: exit 42 → exit 41, rc 0, ZERO diagnostics. The
// item went `EF BE AD DE | 00 00 00 00 | <8-byte pointer> | A5 A5 A5 A5`
// (correct) → `EF BE AD DE | <8-byte pointer overwriting the 4-byte slot AND
// the tag>`, i.e. the 8-byte absolute relocation was written into a 4-byte
// pointer slot and ate the following field.
TEST(DriverArgumentSupply, MergedMultiCuRouteSuppliesTheFormatsDataModel) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-datamodel"};
    scratch.useAsCwd();
    auto const out = scratch.path() / "datamodel_out";
    DiagnosticReporter rep;
    ASSERT_EQ(buildMergedPair(
                  scratch, "datamodel_a.c",
                  "int dm_anchor = 7;\n"
                  "struct P { unsigned int head; int* p; unsigned int tag; };\n"
                  "struct P dm_g = { 0xDEADBEEFu, &dm_anchor, 0xA5A5A5A5u };\n"
                  "int dm_read_tag(void) "
                  "{ return dm_g.tag == 0xA5A5A5A5u ? 1 : 0; }\n",
                  "datamodel_b.c",
                  "extern int dm_read_tag(void);\n"
                  "int main(void) { return dm_read_tag() + 41; }\n",
                  kPeExecSpec, out, rep),
              0)
        << "the 2-CU pe64 pointer-global build must succeed; a red here means "
           "the FIXTURE is broken, not the pin\n"
        << allDiagnosticText(rep);

    auto const img = readAllBytes(out / "datamodel_a.exe");
    ASSERT_FALSE(img.empty()) << "the merged pe64 artifact must exist";

    // `head` is the anchor; the POINTER between it and `tag` is relocated, so
    // its bytes cannot be pinned — its WIDTH can, by where `tag` lands.
    constexpr std::uint8_t kHead[] = {0xEF, 0xBE, 0xAD, 0xDE};
    constexpr std::uint8_t kTag[]  = {0xA5, 0xA5, 0xA5, 0xA5};
    auto const hits = findAll(img, std::span<std::uint8_t const>{kHead});
    ASSERT_EQ(hits.size(), 1u)
        << "the head marker must appear EXACTLY once in the image";

    std::size_t const tagAt = hits[0] + 16;
    ASSERT_LE(tagAt + 4, img.size());
    EXPECT_TRUE(std::equal(std::begin(kTag), std::end(kTag),
                           img.begin() + static_cast<std::ptrdiff_t>(tagAt)))
        << "`tag` must sit 16 bytes after `head`: 4 bytes of `head`, 4 of "
           "alignment padding, and an EIGHT-byte pointer. Anything else means "
           "the merged route handed the globals layout a data model whose "
           "pointer width is not the format's — and the artifact still builds, "
           "still runs, and answers wrong. Window from `head`: "
        << hexWindow(img, hits[0], 24);
}

// ════════════════════════════════════════════════════════════════════════════
// `callingConventionIndex` on the MERGED route — the ordinal `dss::ffi::
// resolveAbi` produced, turned into an index by pointer distance (D-FF3-3).
// ════════════════════════════════════════════════════════════════════════════
//
// ★★ THE SINGLE-CU ROUTE IS PINNED BY `program/test_entry_argv_run`
// `EntryArgvRun.RealCommandLineReachesMainByteExact`; THIS IS ITS MERGED TWIN,
// and it is Windows-gated for the same reason that one is: the ordinal's
// consequence is an ABI, and an ABI is only observable where the image RUNS.
// ✔MEASURED: `x86_64.target.json` declares `sysv_amd64` at ordinal 0 (six
// argument GPRs, six callee-saved registers, no shadow space) and `ms_x64` at
// ordinal 1 (four argument GPRs, sixteen callee-saved registers, 32 bytes of
// shadow space). So the literal `0` — clause 2's "a zero ordinal", and the
// value a hand-written call site is most likely to carry — compiles and emits
// SysV frames into a PE image.
//
// ★ WHY NO BYTE PIN HERE, STATED SO THE OMISSION DOES NOT READ AS LAZINESS.
// Every static signature of the wrong convention that was examined —
// which register `main` reads `argc` from, which callee-saved registers get
// spilled, the size of the `sub rsp` in a calling frame, the UNWIND_INFO save
// offsets — keys on a REGISTER-ALLOCATION OUTCOME. A pin on any of them would
// go red on honest allocator work, which is the shape this project rejects.
// The run is the honest instrument; on every other leg this test asserts only
// that the merged pe64 build SUCCEEDS, and says so rather than implying more.
//
// ✔MEASURED 2026-08-20 with the mutant: rc 0, ZERO diagnostics, and the emitted
// binary died with STATUS_STACK_BUFFER_OVERRUN (0xC0000409) instead of exiting
// 42. The sibling probe that calls a UCRT import died with 0xC0000005.
TEST(DriverArgumentSupply, MergedMultiCuRouteSuppliesTheCallingConventionIndex) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-callconv"};
    scratch.useAsCwd();
    auto const out = scratch.path() / "callconv_out";
    DiagnosticReporter rep;
    ASSERT_EQ(buildMergedPair(scratch, "callconv_a.c",
                              "int cc_double(int a) { return a + a; }\n",
                              "callconv_b.c",
                              "extern int cc_double(int);\n"
                              "int main(int argc, char** argv) {\n"
                              "    return cc_double(argc) + (argv != 0) + 39;\n"
                              "}\n",
                              kPeExecSpec, out, rep),
              0)
        << "the 2-CU pe64 build must succeed\n"
        << allDiagnosticText(rep);
    auto const exe = out / "callconv_a.exe";
    ASSERT_TRUE(fs::exists(exe)) << "the merged pe64 artifact must exist";

#if defined(_WIN32)
    auto const r = dss::test_support::runBinary(exe, dss::test_support::kRunBudget);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_FALSE(r.timedOut) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 42u)
        << "argc(1) doubled, plus a non-null argv, plus 39. A merged pe64 "
           "program must be lowered under the ordinal `resolveAbi` returned "
           "for THIS format. Supplied ordinal 0 (`sysv_amd64`) the frames lose "
           "the 32-byte shadow space and `main` reads its arguments from the "
           "wrong registers — the build still succeeds with no diagnostic and "
           "the image dies at run time (D-FF3-3)";
#else
    GTEST_SKIP() << "a pe64 image only RUNS on Windows; the merged route's "
                    "calling-convention ordinal has no host-independent "
                    "signature that is not a register-allocation outcome. The "
                    "build assertion above still runs on every leg.";
#endif
}

// ════════════════════════════════════════════════════════════════════════════
// `externCallDispatch` — the FORMAT's extern-call shape
// (D-FFI-EXTERN-CALL-DISPATCH), supplied on the merged route.
// ════════════════════════════════════════════════════════════════════════════
//
// ★ THIS ONE FAILS LOUD, AND THAT IS EXACTLY WHY THE PIN IS "THE BUILD MUST
// SUCCEED". MIR→LIR refuses at construction when a module declares extern
// imports and no dispatch shape came with them — it will not guess between the
// indirect-slot and direct-PLT forms, because guessing wrong dereferences code
// as a pointer. So the driver dropping this argument turns a legitimate
// program into a compile ERROR, and the pin is red-on-mutation by refusing to
// build rather than by mis-building.
//
// ★★ THE PREMISE GUARD IS THE LOAD-BEARING HALF. "It builds" is a worthless
// assertion if the fixture has no extern import — the gate would not even be
// armed, and the pin would stay green under any mutation. The import table is
// therefore asserted directly: the emitted image must NAME the imported
// function and the library it comes from.
//
// ✔MEASURED 2026-08-20 with the mutant: every probe carrying an extern import
// stopped building with `L_RequiredLirOpcodeMissing` ("module declares extern
// imports but the active object format declares no `externCallDispatch`
// shape"), while the two probes with no externs were byte-identical.
TEST(DriverArgumentSupply,
     MergedMultiCuRouteSuppliesTheFormatsExternCallDispatch) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-externcall"};
    scratch.useAsCwd();
    auto const out = scratch.path() / "externcall_out";
    DiagnosticReporter rep;
    ASSERT_EQ(buildMergedPair(
                  scratch, "externcall_a.c",
                  "#include <stdio.h>\n"
                  "int ecd_emit(void) { return puts(\"driver-arg-supply\"); }\n",
                  "externcall_b.c",
                  "extern int ecd_emit(void);\n"
                  "int main(void) { return ecd_emit() >= 0 ? 42 : 1; }\n",
                  kPeExecSpec, out, rep),
              0)
        << "a merged 2-CU program with a library CALL must build. A refusal "
           "naming `externCallDispatch` means the merged route stopped "
           "supplying the format's dispatch shape — MIR→LIR will not guess "
           "between indirect-slot and direct-PLT (D-FFI-EXTERN-CALL-DISPATCH)\n"
        << allDiagnosticText(rep);

    auto const img = readAllBytes(out / "externcall_a.exe");
    ASSERT_FALSE(img.empty()) << "the merged pe64 artifact must exist";
    // The premise: this fixture really does carry a live extern import, so the
    // dispatch gate really was armed by the build above.
    EXPECT_GE(countBytes(img, "puts"), 1u)
        << "the emitted image must NAME the imported function — without a live "
           "extern import the dispatch gate never fires and the assertion "
           "above would be vacuous";
    EXPECT_GE(countBytes(img, "ucrtbase.dll"), 1u)
        << "the emitted image must name the library the import binds to";
}

// ════════════════════════════════════════════════════════════════════════════
// `dataImportBinding` — how an extern DATA object's address materializes
// (D-LK-EXTERN-DATA-IMPORT), supplied on the merged route.
// ════════════════════════════════════════════════════════════════════════════
//
// ★★★ THE FAILURE IS ONE MISSING LOAD, AND NOTHING DIAGNOSES IT. Under a
// declared `got-indirect` binding the object's address is LOADED from its
// import slot; with the binding dropped the lowering stops one indirection
// short and the program reads the SLOT'S OWN ADDRESS as if it were the
// object's. ✔MEASURED 2026-08-20: with the mutant the pe64 `.text` shrank by
// exactly the deref instruction, rc was 0, there were ZERO diagnostics, and the
// binary exited 4 instead of 42 — it is the shipped example
// `examples/c/extern_data_import_pe`'s exit-4 rung, reached on the
// MERGED route, which that single-CU example cannot reach.
//
// ★ THE ORACLE IS UCRT'S OWN ACCESSOR, NOT A MAGIC ADDRESS — the same design as
// that example: `_mbcasemap` (a genuine `.data` export) must agree with
// `__p__mbcasemap()`, so nothing host-, locale- or CRT-version-specific is
// asserted. Bound one indirection short, the two disagree.
//
// ★ WINDOWS-GATED FOR THE DISCRIMINATING ARM, and honestly so: the consequence
// is a wrong VALUE at run time, and the ELF twin was measured NOT to
// discriminate (a merged program comparing `stdout` against `stderr` still sees
// two distinct non-null words when both are slot addresses, so it exits 42
// either way). Every leg still asserts the build and the import premise.
TEST(DriverArgumentSupply,
     MergedMultiCuRouteSuppliesTheFormatsDataImportBinding) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-dataimport"};
    scratch.useAsCwd();
    auto const out = scratch.path() / "dataimport_out";
    DiagnosticReporter rep;
    ASSERT_EQ(buildMergedPair(
                  scratch, "dataimport_a.c",
                  "extern unsigned char *_mbcasemap;\n"
                  "extern unsigned char *__p__mbcasemap(void);\n"
                  "int dib_check(void) {\n"
                  "    unsigned char *viaImport = _mbcasemap;\n"
                  "    unsigned char *viaAccessor;\n"
                  "    if (viaImport == 0) return 2;\n"
                  "    viaAccessor = __p__mbcasemap();\n"
                  "    if (viaAccessor == 0) return 3;\n"
                  "    if (viaImport != viaAccessor) return 4;\n"
                  "    return 42;\n"
                  "}\n",
                  "dataimport_b.c",
                  "extern int dib_check(void);\n"
                  "int main(void) { return dib_check(); }\n",
                  kPeExecSpec, out, rep),
              0)
        << "the 2-CU pe64 extern-DATA build must succeed\n"
        << allDiagnosticText(rep);

    auto const exe = out / "dataimport_a.exe";
    auto const img = readAllBytes(exe);
    ASSERT_FALSE(img.empty()) << "the merged pe64 artifact must exist";
    // The premise: BOTH the data object and its accessor are really imported,
    // so the binding this pin is about really was consulted.
    EXPECT_GE(countBytes(img, "_mbcasemap"), 1u)
        << "the emitted image must import the extern DATA object";
    EXPECT_GE(countBytes(img, "__p__mbcasemap"), 1u)
        << "the emitted image must import the accessor that serves as oracle";

#if defined(_WIN32)
    auto const r = dss::test_support::runBinary(exe, dss::test_support::kRunBudget);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_FALSE(r.timedOut) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 42u)
        << "exit 4 is the AGREEMENT rung: the data import and UCRT's own "
           "accessor named different addresses, which is what a dropped "
           "`dataImportBinding` produces — the address materialization stops "
           "one indirection short and yields the import SLOT's address instead "
           "of the object's. Exit 2 would mean the slot was never filled";
#else
    GTEST_SKIP() << "a pe64 image only RUNS on Windows, and the ELF twin was "
                    "MEASURED not to discriminate this binding. The build and "
                    "import-premise assertions above still run on every leg.";
#endif
}

// ════════════════════════════════════════════════════════════════════════════
// `externAddrBinding` — how an undefined extern's ADDRESS-AS-A-VALUE
// materializes (D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT), supplied on the merged
// route.
// ════════════════════════════════════════════════════════════════════════════
//
// ★★ THE ONE MERGED-ROUTE FORMAT THAT CAN WITNESS IT, AND FINDING THAT OUT WAS
// HALF THE WORK. ✔MEASURED over the shipped `object-formats/` corpus, exactly
// two formats declare `externAddrBinding` — `elf64-aarch64-linux-staticlib` and
// `elf64-aarch64-linux`. The staticlib one CANNOT reach this entry point:
// `Program::compileOneTarget` short-circuits every `isStaticArchive()` format
// to the per-member `lowerCuMirToAssembly` route BEFORE the merge, because an
// archive packages separate objects. ✔MEASURED by mutation — a 2-CU staticlib
// build came back byte-identical under the mutant. So the relocatable format is
// the only merged-route witness there is, and a pin aimed at the archive would
// have been silently vacuous.
//
// ★ THE ASSERTION IS ON RELOCATION TYPES, WHICH IS THE FACT ITSELF. Under a
// declared `got` binding the address goes through a foreign-linker GOT slot;
// without it the lowering emits an absolute page-pair `lea` that a default-PIE
// link REJECTS. Both spellings assemble, both link locally, and the difference
// is invisible until someone else's linker sees the object.
//
// ✔MEASURED 2026-08-20, same size, rc 0, ZERO diagnostics, `.rela.text` went
// from `R_AARCH64_ADR_GOT_PAGE` + `R_AARCH64_LD64_GOT_LO12_NC` to
// `R_AARCH64_ADR_PREL_PG_HI21` + `R_AARCH64_ADD_ABS_LO12_NC`. The four wire
// numbers below are the `nativeId` values DECLARED on those rows in
// `elf64-aarch64-linux.format.json`, not constants invented here.
TEST(DriverArgumentSupply,
     MergedMultiCuRouteSuppliesTheFormatsExternAddrBinding) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-externaddr"};
    scratch.useAsCwd();
    auto const out = scratch.path() / "externaddr_out";
    DiagnosticReporter rep;
    ASSERT_EQ(buildMergedPair(
                  scratch, "externaddr_a.c",
                  "extern int eab_extern_obj;\n"
                  "int* eab_addr_of(void) { return &eab_extern_obj; }\n",
                  "externaddr_b.c",
                  "extern int* eab_addr_of(void);\n"
                  "int eab_entry(void) { return eab_addr_of() != 0 ? 42 : 1; }\n",
                  kElfArm64RelocSpec, out, rep),
              0)
        << "the 2-CU arm64 relocatable build must succeed\n"
        << allDiagnosticText(rep);

    auto const img = readAllBytes(out / "externaddr_a.o");
    ASSERT_FALSE(img.empty()) << "the merged arm64 relocatable must exist";

    constexpr std::uint32_t kAdrGotPage   = 311;  // R_AARCH64_ADR_GOT_PAGE
    constexpr std::uint32_t kLd64GotLo12  = 312;  // R_AARCH64_LD64_GOT_LO12_NC
    constexpr std::uint32_t kAdrPrelPgHi21 = 275; // R_AARCH64_ADR_PREL_PG_HI21
    constexpr std::uint32_t kAddAbsLo12   = 277;  // R_AARCH64_ADD_ABS_LO12_NC

    EXPECT_GE(elfRelaTypeCount(img, kAdrGotPage), 1u)
        << "`&extern` used as a VALUE must materialize through a GOT slot when "
           "the format declares `externAddrBinding: got`. Zero of these means "
           "the merged route dropped the binding and emitted an absolute "
           "page-pair lea instead (D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT)";
    EXPECT_GE(elfRelaTypeCount(img, kLd64GotLo12), 1u)
        << "the GOT page relocation's low-12 partner must be present too — one "
           "without the other is a half-materialized address";
    EXPECT_EQ(elfRelaTypeCount(img, kAdrPrelPgHi21), 0u)
        << "an ABSOLUTE page-pair relocation is what a dropped "
           "`externAddrBinding` emits, and a foreign default-PIE link rejects "
           "it. If a later change makes one legitimate here, this pin should "
           "be re-derived rather than relaxed";
    EXPECT_EQ(elfRelaTypeCount(img, kAddAbsLo12), 0u)
        << "the absolute page-pair's low-12 partner, same reasoning";
}

// ════════════════════════════════════════════════════════════════════════════
// `sehScopes` — the SEH scope records `synthesizeSehFunclets` produced for the
// merged module (c116, D-WIN64-SEH-FUNCLETS).
// ════════════════════════════════════════════════════════════════════════════
//
// ★★★ AN EMPTY VECTOR IS A LEGITIMATE VALUE — IT IS WHAT EVERY NON-SEH PROGRAM
// PASSES — SO NOTHING DOWNSTREAM CAN TELL IT FROM A DROPPED ONE. The driver
// synthesizes the funclets into the merged MIR and then hands the scope table
// across separately; hand `{}` instead and the funclet BODIES still ship, the
// image still builds, and the unwind info simply never claims an exception
// handler. At run time the OS finds nothing to dispatch to.
//
// ★ THE PIN READS THE UNWIND DATA, NOT THE EXIT CODE, so it discriminates on
// every leg rather than only where a pe64 image runs: `.pdata`'s
// RUNTIME_FUNCTION array is walked, each entry's UNWIND_INFO is followed into
// `.xdata`, and at least one must carry UNW_FLAG_EHANDLER. The count of
// RESOLVED entries guards the pin's own premise — a fixture that emitted no
// unwind info at all would otherwise report "no handlers" and read exactly like
// the defect it is meant to catch.
//
// ✔MEASURED 2026-08-20 with the mutant: same image SIZE, rc 0, ZERO
// diagnostics; the UNWIND_INFO's first byte went 0x09 (version 1 +
// UNW_FLAG_EHANDLER) → 0x01 (version 1, no handler), `.xdata`'s virtual size
// dropped by the whole scope table, and the binary died with
// EXCEPTION_ACCESS_VIOLATION instead of exiting 42.
TEST(DriverArgumentSupply, MergedMultiCuRouteSuppliesTheSehScopes) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-seh"};
    scratch.useAsCwd();
    auto const out = scratch.path() / "seh_out";
    DiagnosticReporter rep;
    ASSERT_EQ(buildMergedPair(
                  scratch, "seh_a.c",
                  "#include <windows.h>\n"
                  "int seh_guarded(void) {\n"
                  "    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE,\n"
                  "                           PAGE_NOACCESS);\n"
                  "    if (p == 0) return 10;\n"
                  "    int rc = 0;\n"
                  "    __try { rc = *(volatile int *)p; }\n"
                  "    __except (GetExceptionCode() == "
                  "EXCEPTION_ACCESS_VIOLATION) { rc = 42; }\n"
                  "    return rc;\n"
                  "}\n",
                  "seh_b.c",
                  "extern int seh_guarded(void);\n"
                  "int main(void) { return seh_guarded(); }\n",
                  kPeExecSpec, out, rep),
              0)
        << "the 2-CU pe64 SEH build must succeed\n"
        << allDiagnosticText(rep);

    auto const img = readAllBytes(out / "seh_a.exe");
    ASSERT_FALSE(img.empty()) << "the merged pe64 artifact must exist";

    std::size_t       resolved = 0;
    std::size_t const withHandler = peFunctionsWithExceptionHandler(img, resolved);
    ASSERT_GE(resolved, 1u)
        << "the premise: `.pdata` must resolve at least one RUNTIME_FUNCTION's "
           "UNWIND_INFO. Zero would make the handler count below vacuous";
    EXPECT_GE(withHandler, 1u)
        << "a merged program containing `__try`/`__except` must ship unwind "
           "info that CLAIMS an exception handler. Zero means the merged route "
           "handed the lower half an empty SEH scope table: the funclet bodies "
           "still ship, the image still builds with no diagnostic, and the OS "
           "has nothing to dispatch to when the fault arrives "
           "(D-WIN64-SEH-FUNCLETS)";
}

// ════════════════════════════════════════════════════════════════════════════
// `wideFloatSoftcallLibrary` — the runtime library each minted F128 softcall
// binds to (D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH), pre-resolved in the driver
// because the merge lower body has no object-format kind in scope.
// ════════════════════════════════════════════════════════════════════════════
//
// ★ THE PIN ASSERTS THE VALUE, NOT ITS PRESENCE, AND THE DIFFERENCE MATTERS.
// A dropped argument makes the build fail loud, so "it compiled" already
// catches `nullopt`. What it does NOT catch is the driver supplying a WRONG
// library name: the softcall externs would be minted and bound to something
// that does not export them, the image would link, and the failure would move
// to the loader. So the emitted image is required to NAME the library the
// target declares.
//
// ✔MEASURED over the shipped corpus, `arm64.target.json` is the only target
// declaring `wideFloatSoftcallLibraryByFormat`, and it declares exactly one
// row: `elf` → `libgcc_s.so.1`. The x87-80 axis (x86_64 ELF/Mach-O) lowers
// inline and never reaches the softcall path; the f64 axis (pe64, Mach-O arm64)
// collapses `long double` to `double`. That makes arm64-ELF the ONLY merged-
// route witness available.
//
// ✔MEASURED 2026-08-20 with the mutant: the build stopped with
// `L_RequiredLirOpcodeMissing` ("F128 softcall needs a runtime-library binding
// but the active format declares none"), while every non-F128 probe was
// byte-identical.
TEST(DriverArgumentSupply,
     MergedMultiCuRouteSuppliesTheWideFloatSoftcallLibrary) {
    ScratchDir scratch{Location::InsideRepo, "driver-arg-widefloat"};
    scratch.useAsCwd();
    auto const out = scratch.path() / "widefloat_out";
    DiagnosticReporter rep;
    ASSERT_EQ(buildMergedPair(
                  scratch, "widefloat_a.c",
                  "long double wf_lhs;\n"
                  "long double wf_rhs;\n"
                  "int wf_sum(void) {\n"
                  "    wf_lhs = 20.0L;\n"
                  "    wf_rhs = 22.0L;\n"
                  "    return (int)(wf_lhs + wf_rhs);\n"
                  "}\n",
                  "widefloat_b.c",
                  "extern int wf_sum(void);\n"
                  "int main(void) { return wf_sum(); }\n",
                  kElfArm64ExecSpec, out, rep),
              0)
        << "a merged 2-CU program doing IEEE-binary128 arithmetic must build. "
           "A refusal naming the runtime-library binding means the merged route "
           "stopped supplying it (D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH)\n"
        << allDiagnosticText(rep);

    auto const img = readAllBytes(out / "widefloat_a");
    ASSERT_FALSE(img.empty()) << "the merged arm64 exec must exist";
    EXPECT_GE(countBytes(img, "libgcc_s.so.1"), 1u)
        << "the emitted image must NAME the library the target declares for "
           "this format. Its absence with a successful build would mean the "
           "minted `__addtf3`-family externs were bound to some other name — "
           "which links, and then fails at LOAD on the target";
    EXPECT_GE(countBytes(img, "__addtf3"), 1u)
        << "the premise: the fixture must really mint an F128 softcall, or the "
           "library assertion above is about a binding nothing consulted";
}
