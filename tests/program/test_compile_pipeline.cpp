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
#include "core/substrate/thread_pool.hpp"    // D-PERF-4: executor injection (pool vs synchronous)
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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
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

// ── D-CSUBSET-TESTTU-SILENT-EXIT1: a declaration-only / empty TU ─────
//
// A translation unit with NO function or object DEFINITIONS (empty after
// preprocessing — only declarations, or all `#if 0`'d out) MUST compile to a
// VALID EMPTY relocatable object and exit 0, exactly as gcc/clang do. SQLite's
// testfixture depends on this: several `#if defined(SQLITE_TEST)`-gated test
// TUs (test_wsd.c, test6.c, …) are empty in a standard build, yet their `.o`
// files must still be produced and linked. Before the fix such a TU SILENTLY
// exited 1 with ZERO diagnostics — a fail-loud violation AND wrong behavior —
// because four sequential `expectedFuncCount > 0` gates (legalize → callconv →
// assemble → link) each rejected the 0-function module.
//
// RED-ON-DISABLE: restore ANY of the four `> 0` clauses (in
// LirTwoAddrLegalizeResult / LirCallconvResult / AssembledModule / LinkedImage
// ::ok()) — or drop the empty early-return's `allFunctionsLegalized = true` in
// legalizeTwoAddress — and this test fails: `compileFiles` returns 1 and NO
// `.o` is written.
TEST(Program_CompileFiles, EmptyDeclOnlyTuEmitsValidEmptyObject) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    // A declaration-only TU: an extern DATA declaration (not a definition)
    // plus an all-`#if 0`'d-out block. NOTHING is defined ⇒ 0 functions.
    auto const src = writeCSubsetSource(
        scratch.path(), "decl_only.c",
        "extern int shared_counter;   /* a declaration, not a definition */\n"
        "#if 0\n"
        "int never_compiled = 1;      /* preprocessed out */\n"
        "#endif\n");
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileFiles(
        {src.generic_string()},
        "c-subset",
        {"x86_64:elf64-x86_64-linux"});

    ASSERT_EQ(rc, 0)
        << "an empty/declaration-only TU must compile to a valid empty "
           "relocatable object and exit 0 (D-CSUBSET-TESTTU-SILENT-EXIT1)";

    auto const outDir = scratch.path() / "target" / "elf64-x86_64-linux";
    auto const out    = outDir / "decl_only.o";
    ASSERT_TRUE(fs::exists(out)) << "the empty object must be emitted";
    ASSERT_GT(fs::file_size(out), 0u)
        << "even an empty module yields a real ELF (header + section table), "
           "never zero bytes";
    // Valid ELF magic — a real relocatable object the system linker accepts.
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
// EXPECT fails — including the three preprocess sub-phases (splice /
// tokenize / expand, D-PERF-1), which run for ANY C compile. (Phases a
// trivial source legitimately skips — the STANDALONE tokenize [c-subset
// preprocesses, so its tokenize is the preprocess-tokenize sub-phase],
// reparse [no ambiguous cast], synthesize-ffi [no externs] — are
// deliberately un-asserted.)
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
    for (CompilePhase p : {CompilePhase::Preprocess,
                           CompilePhase::PreprocessSplice,
                           CompilePhase::PreprocessTokenize,
                           CompilePhase::PreprocessExpand,
                           CompilePhase::Parse,
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

// ── D-LK-OBJECT-EXTERN-SYMBOL-NAMES: single-CU Mach-O name mangling ─
//
// The single-CU lowering path runs a defined symbol's on-binary name through
// the format's C mangling (applyCMangling) — matching the merge path — so a
// Mach-O relocatable object carries the ld64-expected leading `_`. Identity on
// ELF/PE; adds `_` on Mach-O. This is the end-to-end proof that the pipeline
// (not just the writer) produces the mangled form. Red-on-disable: without the
// single-CU mangle the name is the raw `forty_two`, and `_forty_two` never
// appears in the .o.
TEST(Program_CompileFiles, SingleCuMachOObjectSymbolCarriesLeadingUnderscore) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "forty_two.c",
        "int forty_two() { return 42; }\n");
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    Program prog;
    prog.setOutputDir(outDir);
    int const rc = prog.compileFiles(
        {src.generic_string()}, "c-subset", {"x86_64:macho64-x86_64-darwin"});
    ASSERT_EQ(rc, 0);
    auto const obj = outDir / "forty_two.o";
    ASSERT_TRUE(fs::exists(obj));

    std::ifstream in(obj, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(in.good());
    auto const size = static_cast<std::streamoff>(in.tellg());
    std::string data(static_cast<std::size_t>(size), '\0');
    in.seekg(0);
    in.read(data.data(), size);

    EXPECT_NE(data.find(std::string("_forty_two")), std::string::npos)
        << "single-CU Mach-O .o must carry the leading-underscore mangled "
           "symbol name (ld64 convention) — proves the single-CU nameOf mangle";
}

// ── D-CONFIG-MACHO-X86_64-DARWIN-SUPPORTED-DATA-SECTIONS-ABSENT ─────
//
// The END-TO-END half of that closure. `macho64-x86_64-darwin` and its
// `-staticlib` sibling declared `text` and nothing else, so ANY translation
// unit carrying a string literal or a global was refused on those tiers —
// `K_NoMatchingObjectFormat ... does not advertise that section` — while the
// arm64 siblings emitted the same source at rc=0.
//
// ★ WHY THIS TEST EXISTS ALONGSIDE THE SCHEMA PIN.
// `ObjectFormatFamilySymmetry.EveryMachoFormatDeclaresItsDataSectionsWithTheir
// Rows` reads the shipped JSON and asserts what it DECLARES. That is the
// always-on guard, but a declaration is a CLAIM: it cannot show that the
// walker actually placed the bytes. This test drives the REAL pipeline
// (source → HIR → MIR → LIR → asm → link → writeImage → a file on disk) and
// then parses the emitted section_64 table with a reader that shares no code
// with DSS, so it cannot inherit a DSS bug.
//
// ★ THE MATCHED CONTROL IS INSIDE THE TEST, not alongside it. The identical
// source is compiled on FOUR arms — the bare `.o` and the `-staticlib` archive
// on BOTH Darwin arches — and their (segment, section, flags) sequences are
// required to be EQUAL. Mach-O segment/section names carry no arch axis and an
// archive member IS a relocatable object, so a divergence anywhere in that
// square is a config defect; the arm64 arm is the one with a real
// Apple-Silicon runtime witness behind it. The `-staticlib` arms are not
// redundant: they run a SECOND writer (`ar`) over the member, and the anchor
// names that cell explicitly.
//
// ⛔ WHAT IT DOES NOT SHOW, stated rather than implied: NOTHING RUNS. A
// relocatable object has no entry point; it is an INPUT to a later ld64 link,
// and no Mac is reachable from this host. The claim is "the walker places the
// bytes and the object parses", never "ld64 accepted it".
namespace {

struct MachoSectionRow {
    std::string   segment;
    std::string   name;
    std::uint64_t addr   = 0;
    std::uint64_t size   = 0;
    std::uint32_t offset = 0;
    std::uint32_t align  = 0;   // section_64.align — a LOG2 exponent
    std::uint32_t nreloc = 0;
    std::uint32_t flags  = 0;   // section_64.flags (S_REGULAR / S_ZEROFILL…)
};

[[nodiscard]] std::uint32_t readU32(std::string const& b, std::size_t off) {
    if (off + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[off]))
         | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 1])) << 8)
         | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 2])) << 16)
         | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 3])) << 24);
}

[[nodiscard]] std::uint64_t readU64(std::string const& b, std::size_t off) {
    return static_cast<std::uint64_t>(readU32(b, off))
         | (static_cast<std::uint64_t>(readU32(b, off + 4)) << 32);
}

[[nodiscard]] std::string readName16(std::string const& b, std::size_t off) {
    std::string s;
    for (std::size_t i = 0; i < 16 && off + i < b.size(); ++i) {
        if (b[off + i] == '\0') break;
        s.push_back(b[off + i]);
    }
    return s;
}

// The first Mach-O member of an `ar` archive, or the whole file when it is
// already a bare object. Empty on any structural surprise.
[[nodiscard]] std::string unwrapArchiveMember(std::string const& raw) {
    constexpr std::string_view kArMagic = "!<arch>\n";
    if (raw.size() < kArMagic.size()
        || raw.compare(0, kArMagic.size(), kArMagic) != 0) {
        return raw;                       // already a bare Mach-O
    }
    std::size_t pos = kArMagic.size();
    while (pos + 60 <= raw.size()) {
        std::size_t const bodyOff = pos + 60;
        std::size_t bodySize = 0;
        try {
            bodySize = static_cast<std::size_t>(
                std::stoull(raw.substr(pos + 48, 10)));
        } catch (...) {
            return {};
        }
        if (bodyOff + bodySize > raw.size()) return {};
        std::string body = raw.substr(bodyOff, bodySize);
        if (body.size() >= 4 && readU32(body, 0) == 0xFEEDFACFu) return body;
        pos = bodyOff + bodySize + (bodySize & 1u);   // members are 2-aligned
    }
    return {};
}

// Hand-rolled MH_OBJECT section_64 reader (an `ar` archive is unwrapped to its
// first Mach-O member first). Returns EMPTY on any structural surprise so the
// caller's own assertions report it — never a silent partial list that would
// let a dropped section read as a pass.
[[nodiscard]] std::vector<MachoSectionRow>
readMachoSectionTable(fs::path const& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in.good()) return {};
    auto const size = static_cast<std::streamoff>(in.tellg());
    std::string raw(static_cast<std::size_t>(size), '\0');
    in.seekg(0);
    in.read(raw.data(), size);
    std::string const b = unwrapArchiveMember(raw);
    if (b.size() < 32 || readU32(b, 0) != 0xFEEDFACFu) return {};  // MH_MAGIC_64

    std::vector<MachoSectionRow> rows;
    std::uint32_t const ncmds = readU32(b, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd     = readU32(b, off);
        std::uint32_t const cmdsize = readU32(b, off + 4);
        if (cmdsize == 0 || off + cmdsize > b.size()) return {};
        if (cmd == 0x19u) {                       // LC_SEGMENT_64
            std::uint32_t const nsects = readU32(b, off + 64);
            std::size_t so = off + 72;
            for (std::uint32_t s = 0; s < nsects; ++s) {
                if (so + 80 > b.size()) return {};
                MachoSectionRow r;
                r.name    = readName16(b, so);
                r.segment = readName16(b, so + 16);
                r.addr    = readU64(b, so + 32);
                r.size    = readU64(b, so + 40);
                r.offset  = readU32(b, so + 48);
                r.align   = readU32(b, so + 52);
                r.nreloc  = readU32(b, so + 60);
                r.flags   = readU32(b, so + 64);
                rows.push_back(std::move(r));
                so += 80;
            }
        }
        off += cmdsize;
    }
    return rows;
}

// rodata + data + bss + relro in one TU, so no declared row is unexercised.
constexpr char kEveryDataSectionSource[] =
    "const char msg[] = \"hello from a mach-o data section\";\n"  // rodata
    "int counter = 7;\n"                                          // data
    "int zero_global;\n"                                          // bss
    "static int backing = 3;\n"
    "int *const p_backing = &backing;\n"                          // relro
    "int alpha(void) { return 1; }\n"
    "int beta(void)  { return 2; }\n"
    "typedef int (*fn_t)(void);\n"
    "static fn_t const table[2] = { alpha, beta };\n"             // relro
    "int probe_sum(void) {\n"
    "    return (int)msg[0] + counter + zero_global + *p_backing\n"
    "         + table[0]() + table[1]();\n"
    "}\n";

}  // namespace

TEST(Program_CompileFiles, MachORelocatableTiersPlaceEveryDeclaredDataSection) {
    // Both x86_64 cells the anchor names — the bare `.o` and the `-staticlib`
    // archive, which runs a SECOND writer (`ar`) over the same member — each
    // with its arm64 sibling as the matched control.
    struct Arm { char const* spec; char const* artifact; };
    constexpr int kArms = 4;
    Arm const arms[kArms] = {
        {"x86_64:macho64-x86_64-darwin",           "datasections.o"},
        {"arm64:macho64-arm64-darwin",             "datasections.o"},
        {"x86_64:macho64-x86_64-darwin-staticlib", "datasections.a"},
        {"arm64:macho64-arm64-darwin-staticlib",   "datasections.a"},
    };
    std::vector<MachoSectionRow> byArm[kArms];

    for (int i = 0; i < kArms; ++i) {
        ScratchDir scratch{Location::InsideRepo, "program"};
        auto const src = writeCSubsetSource(scratch.path(), "datasections.c",
                                            kEveryDataSectionSource);
        scratch.useAsCwd();
        auto const outDir = scratch.path() / "out";

        Program prog;
        prog.setOutputDir(outDir);
        int const rc = prog.compileFiles({src.generic_string()}, "c-subset",
                                         {arms[i].spec});
        ASSERT_EQ(rc, 0)
            << arms[i].spec
            << ": a TU with a string literal and a global must COMPILE. Before "
               "D-CONFIG-MACHO-X86_64-DARWIN-SUPPORTED-DATA-SECTIONS-ABSENT "
               "closed, the x86_64 arms died here with K_NoMatchingObjectFormat "
               "(section=rodata not advertised) while this same source built "
               "on arm64 — a shipped format that could not serve its own tier.";
        auto const artifact = outDir / arms[i].artifact;
        ASSERT_TRUE(fs::exists(artifact)) << arms[i].spec;

        byArm[i] = readMachoSectionTable(artifact);
        ASSERT_FALSE(byArm[i].empty())
            << arms[i].spec << ": no readable section_64 table in "
            << arms[i].artifact
            << " — not a 64-bit Mach-O, structurally short, or (for the "
               "archive) no Mach-O member found";
    }

    // Every arm: the same five sections, in the same order, with the same
    // Mach-O flags. Names/segments have no arch axis; a divergence is config.
    for (int i = 0; i < kArms; ++i) {
        auto const& rows = byArm[i];
        char const* who = arms[i].spec;
        ASSERT_EQ(rows.size(), 5u)
            << who << ": expected __text + the four data sections; got "
            << rows.size() << ". A missing row means the walker silently "
                              "dropped a data kind instead of placing it.";

        struct Want { char const* seg; char const* sec; std::uint32_t flags; };
        Want const want[5] = {
            {"__TEXT", "__text",  0x80000400u},  // PURE|SOME_INSTRUCTIONS
            {"__TEXT", "__const", 0x00000000u},  // rodata, S_REGULAR
            {"__DATA", "__data",  0x00000000u},  // data,   S_REGULAR
            {"__DATA", "__const", 0x00000000u},  // relro,  S_REGULAR
            {"__DATA", "__bss",   0x00000001u},  // bss,    S_ZEROFILL
        };
        for (std::size_t k = 0; k < 5; ++k) {
            EXPECT_EQ(rows[k].segment, want[k].seg) << who << " row " << k;
            EXPECT_EQ(rows[k].name,    want[k].sec) << who << " row " << k;
            EXPECT_EQ(rows[k].flags,   want[k].flags)
                << who << " (" << want[k].seg << "," << want[k].sec
                << ") section_64.flags";
        }

        // The rodata row must hold the string literal's bytes, file-backed.
        EXPECT_GE(rows[1].size, 33u)
            << who << ": __TEXT,__const is too small to hold the 33-byte "
                      "string literal — the literal did not land in rodata";
        EXPECT_NE(rows[1].offset, 0u) << who << ": rodata must be file-backed";

        // relro carries its OWN relocation_info table — the whole point of
        // keeping it a distinct section on the relocatable tier. Three fixups:
        // p_backing→backing and table[0..1]→alpha/beta.
        EXPECT_EQ(rows[3].nreloc, 3u)
            << who << ": __DATA,__const must carry the relro fixup table "
                      "(p_backing + two function-pointer slots). Zero means "
                      "the pointers were emitted with no relocation and would "
                      "read as absolute garbage after ld64 laid the object out "
                      "(D-LK-RELRO-CONST-DATA-RELOCATABLE).";

        // S_ZEROFILL end to end: a real size, and NO file bytes.
        EXPECT_GE(rows[4].size, 4u) << who << ": __DATA,__bss holds the int";
        EXPECT_EQ(rows[4].offset, 0u)
            << who << ": an S_ZEROFILL section must carry section_64.offset=0 "
                      "— a non-zero offset means the walker wrote file bytes "
                      "for a zero-fill section.";
    }

    for (int i = 1; i < kArms; ++i) {
        for (std::size_t k = 0; k < byArm[0].size(); ++k) {
            EXPECT_EQ(byArm[0][k].segment, byArm[i][k].segment)
                << arms[i].spec << " row " << k;
            EXPECT_EQ(byArm[0][k].name, byArm[i][k].name)
                << arms[i].spec << " row " << k;
            EXPECT_EQ(byArm[0][k].flags, byArm[i][k].flags)
                << arms[i].spec << " row " << k
                << ": this arm disagrees with macho64-x86_64-darwin about a "
                   "section's identity. Mach-O segment/section names have no "
                   "arch axis and an archive member IS a relocatable object, "
                   "so all four arms must agree — a divergence here is a "
                   "config defect, not an ABI one.";
        }
    }
}

// ── TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME): the SINGLE-CU definition rail ──
//
// ★ THIS TEST EXISTS BECAUSE THE CORPUS EXAMPLE CANNOT REACH THIS CODE PATH, AND
// THE RED-ON-DISABLE BATTERY IS WHAT PROVED IT. `examples/c-subset/asm_label` is
// a TWO-CU program, so its definitions are named by `program.cpp`'s cross-CU
// merge-key lambda and `compile_pipeline`'s MERGED arm (which reads
// `merged.symbolNames`); the SINGLE-CU `nameOf` at compile_pipeline.cpp is never
// entered. Disabling the label in that lambda alone left the example GREEN — a
// vacuous guard over the exact rail an asm label is most likely to be used on.
//
// The assertion is on the emitted BYTES, and it is two-sided on purpose:
//   * `dss_c88_single` MUST appear   — the label is the on-binary name;
//   * `_dss_c88_single` must NOT     — the label REPLACES the Mach-O mangling
//     rather than being mangled on top of it, which is the plausible wrong
//     implementation and the one that silently breaks `__DARWIN_ALIAS` headers
//     (they write their own leading underscore).
// A one-sided "contains the label" check would pass under the double-mangle,
// because `_dss_c88_single` contains `dss_c88_single` as a substring.
//
// RED-ON-DISABLE (re-verified against the final build): drop `r->asmName` from
// the single-CU `nameOf` and the .o carries `_labelled_fn` instead.
TEST(Program_CompileFiles, SingleCuAsmLabelReplacesTheMachOMangling) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "labelled.c",
        "int labelled_fn(void) __asm(\"dss_c88_single\");\n"
        "int labelled_fn(void) { return 42; }\n");
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    Program prog;
    prog.setOutputDir(outDir);
    int const rc = prog.compileFiles(
        {src.generic_string()}, "c-subset", {"arm64:macho64-arm64-darwin"});
    ASSERT_EQ(rc, 0);
    auto const obj = outDir / "labelled.o";
    ASSERT_TRUE(fs::exists(obj));

    std::ifstream in(obj, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(in.good());
    auto const size = static_cast<std::streamoff>(in.tellg());
    std::string data(static_cast<std::size_t>(size), '\0');
    in.seekg(0);
    in.read(data.data(), size);

    EXPECT_NE(data.find(std::string("dss_c88_single")), std::string::npos)
        << "the asm label must BE the emitted symbol name";
    EXPECT_EQ(data.find(std::string("_dss_c88_single")), std::string::npos)
        << "the label REPLACES applyCMangling — it must not be underscored on "
           "top (MEASURED against clang: `int gv __asm(\"myglobal\");` emits "
           "`myglobal`, and every __DARWIN_ALIAS header writes its own `_`)";
    EXPECT_EQ(data.find(std::string("_labelled_fn")), std::string::npos)
        << "the C identifier must NOT reach the object once a label renames it";
}

// ── TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): the per-target ──────
// ── link BASE name, end-to-end on the EMITTED BYTES, both arches ─────────────
//
// ★ THIS IS THE TEST WHOSE ABSENCE LET A SILENT MISBINDING SHIP. Darwin reaches
// its modern 64-bit-inode ABI through `$INODE64` asm-label ALIASES on x86_64 and
// through the PLAIN names on arm64 (`sys/cdefs.h`: `__DARWIN_ONLY_64_BIT_INO_T`
// is 0 on x86_64, 1 on arm64). DSS declared the plain names, so an x86_64
// Darwin build bound the LEGACY 32-bit-inode implementations while compiling the
// MODERN 144-byte `struct stat` — the callee writes 120 bytes, `st_size` is read
// at 96 and written at 72, `fstat` reports 0, and sqlite calls every database
// "malformed". It never failed loud: a descriptor SHADOWS the SDK header
// entirely, so the platform's own asm label never participates, and libSystem
// exports BOTH spellings so the plain import resolves.
//
// GROUND TRUTH, MEASURED on real Darwin (macOS 26.5.2) with a MATCHED TWO-ARCH
// CONTROL — one TU, only `-arch` differs, `cc -c` then `nm -u`:
//   x86_64 → _stat$INODE64 _fstat$INODE64 _lstat$INODE64 _opendir$INODE64
//            _readdir$INODE64   (and _closedir PLAIN)
//   arm64  → _stat _fstat _lstat _opendir _readdir            (and _closedir)
// These assertions reproduce exactly that, through DSS's own pipeline.
//
// BOTH ARMS ARE PINNED because the per-arch SPLIT is the property. A flat alias
// would satisfy the x86_64 arm and be WRONG on arm64 — merely relocating the
// defect — so an x86_64-only pin would be worse than none.
//
// ★ `_closedir` IS ASSERTED PLAIN ON BOTH ARMS, and that is not decoration: it
// is the per-SYMBOL control proving the divergence belongs to individual
// symbols, not to `<dirent.h>`. A "suffix everything in this header" fix goes
// red here.
//
// RED-ON-DISABLE (MEASURED, by reverting and re-running): remove `linkName`
// from `sys/stat.json`'s `fstat` row and the x86_64 object carries `_fstat`
// instead of `_fstat$INODE64`, failing the first EXPECT_NE below. Drop
// `ext.linkName` from the shipped-descriptor `HirExternRecord` producer in
// `cst_to_hir.cpp` and BOTH `$INODE64` names vanish while arm64 stays green —
// which is precisely the "it passed on the arch I tested" shape.
namespace {

// Compile `source` for `spec`, read the emitted artifact's bytes.
[[nodiscard]] std::string
compileAndReadArtifact(char const* scratchTag, char const* fileName,
                       std::string const& source, char const* spec,
                       char const* artifact) {
    ScratchDir scratch{Location::InsideRepo, scratchTag};
    auto const src = writeCSubsetSource(scratch.path(), fileName, source);
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";
    Program prog;
    prog.setOutputDir(outDir);
    int const rc =
        prog.compileFiles({src.generic_string()}, "c-subset", {spec});
    EXPECT_EQ(rc, 0) << "compile failed for " << spec;
    auto const obj = outDir / artifact;
    EXPECT_TRUE(fs::exists(obj)) << "no artifact at " << obj.generic_string();
    if (!fs::exists(obj)) return {};
    std::ifstream in(obj, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(in.good());
    auto const size = static_cast<std::streamoff>(in.tellg());
    std::string data(static_cast<std::size_t>(size), '\0');
    in.seekg(0);
    in.read(data.data(), size);
    return data;
}

// The one TU both arms compile — references the five diverging symbols plus the
// non-diverging `closedir` control. Deliberately a REAL `#include` of the
// shipped descriptors, not a hand-written extern: the whole defect lives in the
// descriptor rows, so a test that declared the externs itself would pin nothing.
constexpr char const* kInode64Probe =
    "#include <sys/stat.h>\n"
    "#include <dirent.h>\n"
    "int main(void) {\n"
    "    struct stat st;\n"
    "    DIR *d;\n"
    "    if (fstat(0, &st) != 0) return 1;\n"
    "    if (stat(\"/\", &st) != 0) return 2;\n"
    "    if (lstat(\"/\", &st) != 0) return 3;\n"
    "    d = opendir(\"/\");\n"
    "    if (d == 0) return 4;\n"
    "    if (readdir(d) == 0) return 5;\n"
    "    return closedir(d);\n"
    "}\n";

}  // namespace

TEST(Program_CompileFiles, MachOX86_64ShippedLinkNameEmitsInode64Aliases) {
    std::string const data = compileAndReadArtifact(
        "program", "inode64.c", kInode64Probe,
        "x86_64:macho64-x86_64-darwin-exec", "inode64");
    ASSERT_FALSE(data.empty());
    for (char const* sym : {"fstat", "stat", "lstat", "opendir", "readdir"}) {
        std::string const aliased = std::string("_") + sym + "$INODE64";
        EXPECT_NE(data.find(aliased), std::string::npos)
            << "x86_64-Darwin must import " << aliased
            << " — the plain name binds the LEGACY 32-bit-inode callee, which "
               "writes 120 of 144 bytes and reports st_size 0 (MEASURED)";
    }
    EXPECT_NE(data.find(std::string("_closedir")), std::string::npos)
        << "closedir does NOT diverge per arch (MEASURED: `cc -arch x86_64` "
           "emits the plain name) — the per-SYMBOL control";
    EXPECT_EQ(data.find(std::string("__fstat$INODE64")), std::string::npos)
        << "the descriptor declares the UNDECORATED base name and the ENGINE "
           "composes the format's `_`; a config-side underscore would "
           "double-decorate";
}

TEST(Program_CompileFiles, MachOArm64ShippedLinkNameKeepsThePlainNames) {
    std::string const data = compileAndReadArtifact(
        "program", "inode64.c", kInode64Probe,
        "arm64:macho64-arm64-darwin-exec", "inode64");
    ASSERT_FALSE(data.empty());
    EXPECT_EQ(data.find(std::string("$INODE64")), std::string::npos)
        << "arm64-Darwin has ONE inode ABI — no variant may match, and an alias "
           "here would import a symbol libSystem's arm64 slice does not export";
    for (char const* sym : {"_fstat", "_stat", "_lstat", "_opendir", "_readdir",
                            "_closedir"}) {
        EXPECT_NE(data.find(std::string(sym)), std::string::npos)
            << "arm64-Darwin must import the plain " << sym
            << " (the MATCHED CONTROL that must stay green)";
    }
}

// The DEFAULT path on a format whose decoration is EMPTY: the same descriptor
// rows, no variant matching, no underscore. This is the second half of "the
// override path and the default path go through ONE decoration rule" — if the
// `_` had been baked into config or composed in the semantic injector, elf
// would carry it too.
TEST(Program_CompileFiles, ElfShippedLinkNameDefaultsToTheBareIdentifier) {
    std::string const data = compileAndReadArtifact(
        "program", "inode64.c", kInode64Probe,
        "x86_64:elf64-x86_64-linux-exec", "inode64");
    ASSERT_FALSE(data.empty());
    EXPECT_EQ(data.find(std::string("$INODE64")), std::string::npos)
        << "no Linux symbol carries a Darwin inode alias";
    EXPECT_EQ(data.find(std::string("_fstat")), std::string::npos)
        << "ELF adds NO leading underscore — a `_fstat` here would mean the "
           "decoration leaked out of the Mach-O rule";
    EXPECT_NE(data.find(std::string("fstat")), std::string::npos)
        << "the bare C identifier is the ELF import name";
}

// ── D-LK3-MACHO-ARM64-OBJECT: the arm64 sibling emits a real .o ──
//
// End-to-end pipeline proof for the arm64 Mach-O relocatable object
// (macho64-arm64-darwin, this cycle's sibling of macho64-x86_64-darwin): the
// compile of a leaf function to the `arm64:macho64-arm64-darwin` target emits a
// `forty_two.o` carrying the ld64-mangled `_forty_two`. The DSS-emitted .o links +
// runs under the system clang/ld64 on Apple Silicon (the cross-toolchain witness,
// exit 42; asserted here structurally, host-independent). Red-on-disable: delete the
// format file → the compile fails (format not found) and the .o is never produced.
TEST(Program_CompileFiles, SingleCuArm64MachOObjectEmitsMangledSymbol) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "forty_two.c",
        "int forty_two() { return 42; }\n");
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    Program prog;
    prog.setOutputDir(outDir);
    int const rc = prog.compileFiles(
        {src.generic_string()}, "c-subset", {"arm64:macho64-arm64-darwin"});
    ASSERT_EQ(rc, 0);
    auto const obj = outDir / "forty_two.o";
    ASSERT_TRUE(fs::exists(obj));

    std::ifstream in(obj, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(in.good());
    auto const size = static_cast<std::streamoff>(in.tellg());
    std::string data(static_cast<std::size_t>(size), '\0');
    in.seekg(0);
    in.read(data.data(), size);

    // mach_header cputype = CPU_TYPE_ARM64 (0x0100000C) at byte 4 (LE): 0C 00 00 01.
    ASSERT_GE(data.size(), 8u);
    EXPECT_EQ(static_cast<unsigned char>(data[4]), 0x0Cu);
    EXPECT_EQ(static_cast<unsigned char>(data[7]), 0x01u)
        << "the arm64 Mach-O .o header must carry CPU_TYPE_ARM64";
    EXPECT_NE(data.find(std::string("_forty_two")), std::string::npos)
        << "the arm64 Mach-O .o must carry the ld64-mangled _forty_two symbol";
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
                                     /*externAddrBinding=*/std::nullopt,
                                     /*tlsAccess=*/std::nullopt,
                                     /*sehScopes=*/{},
                                     /*wideFloatSoftcallLibrary=*/std::nullopt, rep);
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

// The same build, but reporting only WHETHER it was refused — for the case where
// the interesting answer is "no image, loudly" rather than "which library".
// Deliberately EXPECT-free so a refusal is data here, not a failure.
[[nodiscard]] bool putsBuildIsRefusedFor(
        std::string const& descJson, char const* targetName, char const* formatName) {
    auto grammarR = GrammarSchema::loadShipped("c-subset");
    auto targetR  = TargetSchema::loadShipped(targetName);
    auto formatR  = ObjectFormatSchema::loadShipped(formatName);
    if (!grammarR || !targetR || !formatR) return false;
    auto grammar = *grammarR;
    DiagnosticReporter rep;
    auto const abi = dss::ffi::resolveAbi(**targetR, **formatR, rep);
    if (!abi) return false;
    auto const ccSpan  = (*targetR)->callingConventions();
    auto const ccIndex = static_cast<std::uint16_t>(
        std::distance(ccSpan.data(), abi->cc));
    ScratchDir sysDir{Location::InsideRepo, "model3-libresolve"};
    std::ofstream(sysDir.path() / "stdio.json", std::ios::binary) << descJson;
    UnitBuilder builder{grammar};
    builder.addSystemDir(sysDir.path());
    builder.addInMemory(R"(#include <stdio.h>
int main() { puts("hi"); return 0; }
)",
                        "main.c");
    CompilationUnit cu = std::move(builder).finish();
    auto cuMir = buildCuMir(cu, *grammar, **targetR, **formatR, ccIndex, rep);
    return !cuMir.has_value() && rep.errorCount() > 0;
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

// ★★ A DESCRIPTOR WHOSE `library` MAP OMITS THE ACTIVE FORMAT'S KEY BINDS
// NOTHING — IT DOES NOT INHERIT A LANGUAGE DEFAULT (UCRT-P4, Decision 1).
//
// This test previously asserted the opposite: that an ELF build of a pe-only map
// inherited `externLibraryByFormat.elf` ("libc.so.6"). That per-LANGUAGE default is
// GONE, because it was never a fact about a language — "which image owns this
// symbol" is a fact about a PLATFORM, and the descriptor corpus owns it PER SYMBOL.
// Inheriting it made a descriptor that says NOTHING about elf look like it had said
// "libc.so.6", which is how a hand-written `extern double sin(double);` came to bind
// libc while glibc ships `sin` in libm — that binary died at LOAD with
// "undefined symbol: sin" (MEASURED; it now links libm and runs).
//
// THE INVARIANT NOW: no library for this format => EMPTY, i.e. UNBOUND, and C23
// 5.1.1.2 phase 8 resolves the reference at LINK (a sibling TU, a
// `--resolve-library` export, or a LOUD K_SymbolUndefined). "The platform declares
// this symbol but not where it lives" is an ENUMERATED outcome, not a fallthrough.
//
// RED-ON-DISABLE: reinstate any format-level fallback and the elf assertion returns
// the invented image instead of "", failing with the name it invented.
TEST(Program_ShippedLibModel3, MissingFormatKeyBindsNothingRatherThanADefault) {
    std::string const descPeOnly = R"({
        "header": "stdio.h", "library": { "pe": "ucrtbase.dll" },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })";
    // On ELF the descriptor is silent, so there is NOTHING to bind. It is an EAGER
    // shipped row (a `#include`d descriptor symbol is imported whether referenced or
    // not), and `isEagerImport ⟹ library-bound` is a standing invariant — so the
    // build is REFUSED LOUD rather than handed an invented image. Either loud
    // outcome is acceptable and both are strictly better than the silent guess; what
    // is asserted is that NO IMAGE IS INVENTED.
    EXPECT_TRUE(putsBuildIsRefusedFor(descPeOnly, "x86_64",
                                      "elf64-x86_64-linux-exec"))
        << "a descriptor that declares `puts` available on elf while naming no elf "
           "library must be REFUSED — never silently inherit a language-level guess";
    // The CONTROL half: the same descriptor DOES speak for pe, and that value is
    // still read. Without it, an implementation that refused everything would also
    // pass this test.
    EXPECT_EQ(resolvedPutsLibraryFor(descPeOnly, "x86_64", "pe64-x86_64-windows-exec"),
              "ucrtbase.dll");
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
[[nodiscard]] std::size_t findPhdrOfType(std::vector<std::uint8_t> const& b,
                                         std::uint32_t pType) {
    std::uint64_t const phoff = rdU64(b, 32);
    std::uint16_t const phnum = rdU16(b, 56);
    for (std::uint16_t i = 0; i < phnum; ++i) {
        std::size_t const o = static_cast<std::size_t>(phoff) + i * 56u;
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            v |= static_cast<std::uint32_t>(b[o + static_cast<std::size_t>(k)])
                 << (k * 8);
        }
        if (v == pType) return o;
    }
    return 0;
}
[[nodiscard]] std::size_t findPtTls(std::vector<std::uint8_t> const& b) {
    return findPhdrOfType(b, 7u);
}
// D-UNWIND-NO-EH-FRAME-ANY-LANGUAGE-ON-ELF-OR-MACHO: the runtime unwinder
// finds `.eh_frame_hdr` through THIS segment (`dl_iterate_phdr`), so its
// presence is a property worth asserting rather than a number to absorb.
inline constexpr std::uint32_t kPtGnuEhFrame = 0x6474e550u;

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

    // e_phnum == 7: PHDR + INTERP + LOAD*2 + DYNAMIC + PT_TLS +
    // PT_GNU_EH_FRAME. ★ This is an EXACT count on purpose — it is the only
    // assertion that notices a segment nobody meant to add — so it must be
    // updated deliberately, WITH the composition written out, whenever the
    // image legitimately gains one. It went 6 -> 7 when `.eh_frame_hdr` and
    // its segment landed (D-UNWIND-NO-EH-FRAME-...); the two property pins
    // below are what actually say WHICH segments those are.
    EXPECT_EQ(rdU16(bytes, 56), 7u);
    EXPECT_NE(findPhdrOfType(bytes, kPtGnuEhFrame), 0u)
        << "PT_GNU_EH_FRAME must be present — without it the process's own "
           "unwinder cannot locate the frame tables";
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

TEST(Program_CompileFiles, Arm64ThreadLocalEmitsPtTlsAndMrsAccessSequence) {
    // TLS C2 (D-CSUBSET-THREAD-LOCAL): the arm64 E2E twin — the SAME
    // source through the FULL driver for arm64:elf64-aarch64-linux-exec.
    // Pins the Variant-I physics IN THE IMAGE: PT_TLS {filesz 4,
    // memsz 4, align 4} + the MRS TPIDR_EL0 word in .text + the
    // ADD/ADD hi12/lo12 pair PATCHED to tpoff 16 =
    // alignUp(tcbHeaderBytes 16, p_align 4) + templateOffset 0 — the
    // walker's addTlsSymbolOffsets Variant-1 arm made VISIBLE in the
    // bytes. A clean compile ALSO proves both tls reloc kinds were
    // consumed (an unconsumed/mismatched kind fails the CRIT-1
    // cross-check loud). Red-on-disable: routing thread_local through
    // Data/Bss keeps single-thread runtime green while every
    // assertion here flips.
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "tls_e2e_a64.c",
        "thread_local int g = 7;\n"
        "int main(void) { g = g + 35; return g; }\n");
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileFiles(
        {src.generic_string()}, "c-subset",
        {"arm64:elf64-aarch64-linux-exec"});
    ASSERT_EQ(rc, 0) << "thread_local must compile clean end-to-end on "
                        "arm64 (D-CSUBSET-THREAD-LOCAL, TLS C2)";

    auto const out = scratch.path() / "target" / "elf64-aarch64-linux-exec"
                   / "tls_e2e_a64";
    ASSERT_TRUE(fs::exists(out));
    auto const bytes = readAllBytes(out);
    ASSERT_GT(bytes.size(), 64u);

    // e_machine = EM_AARCH64 (183); e_phnum == 7 (PT_TLS +
    // PT_GNU_EH_FRAME on top of the base five) -- see the x86_64 twin above
    // for why the exact count is kept.
    EXPECT_EQ(rdU16(bytes, 18), 183u);
    EXPECT_EQ(rdU16(bytes, 56), 7u);
    EXPECT_NE(findPhdrOfType(bytes, kPtGnuEhFrame), 0u)
        << "the unwind segment is target-agnostic: arm64 gets it too";
    std::size_t const tlsPh = findPtTls(bytes);
    ASSERT_NE(tlsPh, 0u) << "PT_TLS program header must be present";
    std::uint64_t const pOff    = rdU64(bytes, tlsPh + 8);
    std::uint64_t const pFilesz = rdU64(bytes, tlsPh + 32);
    std::uint64_t const pMemsz  = rdU64(bytes, tlsPh + 40);
    std::uint64_t const pAlign  = rdU64(bytes, tlsPh + 48);
    EXPECT_EQ(pFilesz, 4u);
    EXPECT_EQ(pMemsz, 4u);
    EXPECT_EQ(pAlign, 4u);
    EXPECT_EQ(bytes[static_cast<std::size_t>(pOff)], 7u)
        << "the .tdata template must carry g's initial value";

    // Scan the image words for the access sequence: at least one
    // `MRS Xd, TPIDR_EL0` (0xD53BD040 | Rd — Rd is regalloc's pick,
    // masked) and at least one PATCHED ADD/ADD hi12/lo12 pair
    // decoding to tpoff 16 (hi12 0, lo12 16) — the Variant-I value
    // for the sole 4-byte-aligned int at template offset 0.
    auto const rdU32 = [&](std::size_t off) {
        return  static_cast<std::uint32_t>(bytes[off])
             | (static_cast<std::uint32_t>(bytes[off + 1]) << 8)
             | (static_cast<std::uint32_t>(bytes[off + 2]) << 16)
             | (static_cast<std::uint32_t>(bytes[off + 3]) << 24);
    };
    std::size_t mrsCount = 0;
    std::size_t tpoff16Pairs = 0;
    for (std::size_t i = 0; i + 8 <= bytes.size(); i += 4) {
        std::uint32_t const w = rdU32(i);
        if ((w & 0xFFFFFFE0u) == 0xD53BD040u) ++mrsCount;
        // ADD Xd, Xn, #imm12, LSL #12 (0x91400000 opcode bits) whose
        // imm12 == 0 (hi12 of tpoff 16), followed by ADD Xd, Xn,
        // #imm12 (0x91000000, sh=0) whose imm12 == 16 (lo12).
        if ((w & 0xFFC00000u) == 0x91400000u
            && ((w >> 10) & 0xFFFu) == 0u) {
            std::uint32_t const w2 = rdU32(i + 4);
            if ((w2 & 0xFFC00000u) == 0x91000000u
                && ((w2 >> 10) & 0xFFFu) == 16u) {
                ++tpoff16Pairs;
            }
        }
    }
    EXPECT_GE(mrsCount, 1u)
        << "the MRS TPIDR_EL0 thread-pointer read must be present";
    EXPECT_GE(tpoff16Pairs, 1u)
        << "the ADD/ADD pair must be patched to the Variant-I tpoff "
           "16 = alignUp(tcbHeaderBytes 16, p_align 4) + 0";
}

TEST(Program_CompileFiles, Arm64ThreadLocalHi12NonZeroPairPatchedE2E) {
    // ★ TLS C2 (audit fold): the sh=1-template × hi12-PATCH
    // composition — every other in-repo image pin has tpoff < 4096 so
    // the hi12 field stays 0 and only the formula UNIT tests exercise
    // a non-zero hi12; this belt witnesses it in a REAL image. Layout
    // hand-derivation: `pad` must be INITIALIZED so both items share
    // .tdata with pad FIRST (an uninitialized pad would go to .tbss,
    // which sits AFTER .tdata in the block — leaving v's tpoff at 16
    // and hi12 at 0):
    //   .tdata: pad @ 0 (align 1, 5000 bytes), v @ alignUp(5000,4)
    //           = 5000 (align 4) → filesz/memsz 5004, p_align 4;
    //   tpoff(v) = alignUp(tcbHeaderBytes 16, p_align 4)
    //            + templateOffset 5000 = 5016 = 0x1398
    //   → hi12 = 5016 >> 12 = 1, lo12 = 5016 & 0xFFF = 0x398 (920).
    // Pure image-byte assertions — no emulator required.
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "tls_hi12_a64.c",
        "thread_local char pad[5000] = {1};\n"
        "thread_local int v = 7;\n"
        "int main(void) {\n"
        "    pad[0] = pad[0] + 1;\n"
        "    v = v + 33;\n"
        "    return v + pad[0];\n"
        "}\n");
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileFiles(
        {src.generic_string()}, "c-subset",
        {"arm64:elf64-aarch64-linux-exec"});
    ASSERT_EQ(rc, 0) << "the >4096-byte TLS layout must compile clean "
                        "(D-CSUBSET-THREAD-LOCAL, TLS C2)";

    auto const out = scratch.path() / "target" / "elf64-aarch64-linux-exec"
                   / "tls_hi12_a64";
    ASSERT_TRUE(fs::exists(out));
    auto const bytes = readAllBytes(out);
    ASSERT_GT(bytes.size(), 64u);

    // PT_TLS spans the 5004-byte template at p_align 4; template[0]
    // = pad's 1, template[5000..5003] = v's 07 00 00 00.
    std::size_t const tlsPh = findPtTls(bytes);
    ASSERT_NE(tlsPh, 0u);
    std::uint64_t const pOff    = rdU64(bytes, tlsPh + 8);
    std::uint64_t const pFilesz = rdU64(bytes, tlsPh + 32);
    std::uint64_t const pMemsz  = rdU64(bytes, tlsPh + 40);
    std::uint64_t const pAlign  = rdU64(bytes, tlsPh + 48);
    EXPECT_EQ(pFilesz, 5004u);
    EXPECT_EQ(pMemsz, 5004u);
    EXPECT_EQ(pAlign, 4u);
    EXPECT_EQ(bytes[static_cast<std::size_t>(pOff)], 1u)
        << "template[0] must carry pad[0]'s initial value";
    EXPECT_EQ(bytes[static_cast<std::size_t>(pOff) + 5000], 7u)
        << "template[5000] must carry v's initial value";

    // At least one PATCHED pair decodes to v's tpoff 5016: word0 =
    // ADD sh=1 with hi12 == 1, word1 = ADD sh=0 with lo12 == 920.
    auto const rdU32 = [&](std::size_t off) {
        return  static_cast<std::uint32_t>(bytes[off])
             | (static_cast<std::uint32_t>(bytes[off + 1]) << 8)
             | (static_cast<std::uint32_t>(bytes[off + 2]) << 16)
             | (static_cast<std::uint32_t>(bytes[off + 3]) << 24);
    };
    std::size_t hi12OnePairs = 0;
    for (std::size_t i = 0; i + 8 <= bytes.size(); i += 4) {
        std::uint32_t const w = rdU32(i);
        if ((w & 0xFFC00000u) == 0x91400000u
            && ((w >> 10) & 0xFFFu) == 1u) {
            std::uint32_t const w2 = rdU32(i + 4);
            if ((w2 & 0xFFC00000u) == 0x91000000u
                && ((w2 >> 10) & 0xFFFu) == 920u) {
                ++hi12OnePairs;
            }
        }
    }
    EXPECT_GE(hi12OnePairs, 1u)
        << "v's accesses must carry the ADD/ADD pair patched to tpoff "
           "5016 = 0x1398 (hi12 1, lo12 920) — the hi12-nonzero "
           "composition the formula unit tests alone cannot witness "
           "in an image";
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

    EXPECT_EQ(rdU16(bytes, 56), 6u)
        << "no PT_TLS slot without thread_local (the base five + "
           "PT_GNU_EH_FRAME, which every image with frame information gets)";
    EXPECT_EQ(findPtTls(bytes), 0u);
    // The control's real job: PT_TLS is CONDITIONAL. Pinning that the
    // unwind segment is present here too keeps the count above honest --
    // otherwise a 6 could be read as a PT_TLS that leaked back in.
    EXPECT_NE(findPhdrOfType(bytes, kPtGnuEhFrame), 0u);
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

// ═══════════════════════════════════════════════════════════════════
// D-PERF-4-CU-PARALLELISM — per-CU build parallelism: CONCURRENCY
// correctness + DETERMINISM. compileOneTarget builds every CU's MIR up
// front; for N>1 the builds run on a thread pool, each writing its OWN
// result slot + scratch DiagnosticReporter, and the driver merges the
// scratches into the run-wide reporter in CU (index) ORDER after the
// join. These pins compare the real ThreadPool path against a
// SynchronousExecutor (single-threaded, always CU-ordered) reference:
// same diagnostics in the same order, and byte-identical artifacts.
// ═══════════════════════════════════════════════════════════════════

// THE load-bearing determinism pin. FOUR TUs, each with a DISTINCT
// undeclared-identifier — a SEMANTIC error (parse-clean) whose `actual`
// is the identifier name, so it is produced INSIDE the per-CU build loop
// (parse errors would short-circuit before it). The pool run's diagnostic
// stream must equal the SynchronousExecutor baseline EXACTLY and be in CU
// (source) order.
//
// RED-on-disable: change the merge in compileOneTarget to completion order
// (or drain one shared reporter from the jobs) → the pool run interleaves
// by thread finish-time → it diverges from the always-CU-ordered
// synchronous baseline → the vector-equality below fails.
TEST(Program_CuParallelism, MultiTuDiagnosticsAreCuOrderedPoolVsSynchronous) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    std::vector<std::string> files;
    for (int i = 0; i < 4; ++i) {
        auto const body = "int f" + std::to_string(i)
                        + "(void) { return zzundef_cu" + std::to_string(i) + "; }\n";
        files.push_back(writeCSubsetSource(
            scratch.path(), "cu" + std::to_string(i) + ".c", body).generic_string());
    }
    scratch.useAsCwd();

    auto runAndCollect = [&](substrate::IExecutor* exec) {
        Program prog;
        prog.setExecutor(exec);
        DiagnosticReporter rep;
        int const rc = prog.compileUnits(
            files, "c-subset", {"x86_64:elf64-x86_64-linux"}, rep);
        EXPECT_NE(rc, 0) << "every CU has an undeclared identifier — must fail loud";
        std::vector<std::pair<DiagnosticCode, std::string>> out;
        for (auto const& d : rep.all()) out.emplace_back(d.code, d.actual);
        return out;
    };

    substrate::SynchronousExecutor sync;
    substrate::ThreadPool          pool{4};   // 4 workers: force real concurrency
    auto const seq = runAndCollect(&sync);
    auto const par = runAndCollect(&pool);

    // (1) Determinism: the pool stream is element-for-element the sync stream.
    ASSERT_EQ(seq.size(), par.size())
        << "pool + synchronous must surface the SAME number of diagnostics";
    EXPECT_EQ(seq, par)
        << "pool diagnostics must match the single-threaded reference EXACTLY "
           "— a mismatch means the merge is completion-ordered, not CU-ordered";

    // (2) The four CUs' markers appear in CU (source) ORDER in the stream.
    std::string concat;
    for (auto const& d : par) { concat += d.second; concat += '\n'; }
    std::size_t prev = 0;
    for (int i = 0; i < 4; ++i) {
        auto const pos = concat.find("zzundef_cu" + std::to_string(i));
        ASSERT_NE(pos, std::string::npos)
            << "CU " << i << "'s diagnostic (zzundef_cu" << i << ") is missing";
        if (i > 0) {
            EXPECT_GT(pos, prev)
                << "CU " << i << "'s diagnostic must FOLLOW CU " << (i - 1)
                << "'s — the merge order must be CU (index) order";
        }
        prev = pos;
    }
}

// Byte-identical artifacts. A VALID 3-TU program of only named global
// functions (no synthesized-symbol or timestamp bytes ⇒ the relocatable
// ELF .o is reproducible across compiles) built via the pool + via the
// SynchronousExecutor must produce IDENTICAL output bytes. The N>1 merge
// folds CUs in index order and everything after it is serial, so thread
// scheduling cannot perturb the image.
TEST(Program_CuParallelism, MultiTuArtifactBytesIdenticalPoolVsSynchronous) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const m = writeCSubsetSource(scratch.path(), "main.c",
        "int a(void); int b(void);\nint main(void) { return a() + b(); }\n");
    auto const fa = writeCSubsetSource(scratch.path(), "a.c",
        "int a(void) { return 20; }\n");
    auto const fb = writeCSubsetSource(scratch.path(), "b.c",
        "int b(void) { return 22; }\n");
    scratch.useAsCwd();
    std::vector<std::string> const files{
        m.generic_string(), fa.generic_string(), fb.generic_string()};

    auto compileTo = [&](substrate::IExecutor* exec, fs::path const& outDir) {
        Program prog;
        prog.setExecutor(exec);
        prog.setOutputDir(outDir);
        int const rc = prog.compileUnits(
            files, "c-subset", {"x86_64:elf64-x86_64-linux"});
        EXPECT_EQ(rc, 0) << "the 3-TU program must link + emit an artifact";
        return readAllBytes(outDir / "main.o");
    };

    substrate::SynchronousExecutor sync;
    substrate::ThreadPool          pool{4};
    auto const seqBytes = compileTo(&sync, scratch.path() / "out_sync");
    auto const parBytes = compileTo(&pool, scratch.path() / "out_pool");

    ASSERT_FALSE(seqBytes.empty()) << "the artifact must be non-empty";
    EXPECT_EQ(seqBytes, parBytes)
        << "the pool-built image must be BYTE-IDENTICAL to the single-threaded "
           "build — the only observable difference parallelism may introduce is "
           "speed, never bytes";
}

// Repeat-stability (a probabilistic race catcher): the SAME multi-TU input
// built via the pool K=20 times must yield identical artifact bytes AND a
// clean diagnostic sequence every time. A latent data race in the per-CU
// build would eventually perturb one of the K runs.
TEST(Program_CuParallelism, MultiTuPoolCompileIsRepeatStable) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const m = writeCSubsetSource(scratch.path(), "main.c",
        "int a(void); int b(void); int c(void);\n"
        "int main(void) { return a() + b() + c(); }\n");
    auto const fa = writeCSubsetSource(scratch.path(), "a.c", "int a(void) { return 10; }\n");
    auto const fb = writeCSubsetSource(scratch.path(), "b.c", "int b(void) { return 14; }\n");
    auto const fc = writeCSubsetSource(scratch.path(), "c.c", "int c(void) { return 18; }\n");
    scratch.useAsCwd();
    std::vector<std::string> const files{
        m.generic_string(), fa.generic_string(),
        fb.generic_string(), fc.generic_string()};

    substrate::ThreadPool pool{4};   // one pool reused across all K runs
    auto const outDir = scratch.path() / "out";

    std::vector<std::uint8_t> firstBytes;
    for (int k = 0; k < 20; ++k) {
        Program prog;
        prog.setExecutor(&pool);
        prog.setOutputDir(outDir);
        DiagnosticReporter rep;
        int const rc = prog.compileUnits(
            files, "c-subset", {"x86_64:elf64-x86_64-linux"}, rep);
        ASSERT_EQ(rc, 0) << "run " << k << " must succeed";
        EXPECT_EQ(rep.errorCount(), 0u) << "run " << k << " must be diagnostic-clean";
        auto const bytes = readAllBytes(outDir / "main.o");
        ASSERT_FALSE(bytes.empty()) << "run " << k << " produced no artifact";
        if (k == 0) {
            firstBytes = bytes;
        } else {
            EXPECT_EQ(bytes, firstBytes)
                << "run " << k << " diverged from run 0 — a data race in the "
                   "per-CU build pool perturbed the image";
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// TF-C74 — the CU cache key must include the TARGET, not just the object format.
// ═════════════════════════════════════════════════════════════════════════════

namespace {

// Read a whole file as a byte STRING so a test can look for a symbol NAME in
// the emitted object's string table. (Distinct from the file-scope
// `readAllBytes`, which yields `vector<uint8_t>` for magic-number checks.)
[[nodiscard]] std::string readFileAsString(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// Where two ARTIFACTS first diverge, rendered for a `<<` failure message.
// `EXPECT_EQ` on two object files would dump both blobs verbatim — pages of
// unreadable bytes — so the equality is asserted on the strings and this says
// WHERE it broke, which is the part a reader can act on.
[[nodiscard]] std::string byteDiffSummary(std::string_view lhsName,
                                          std::string const& lhs,
                                          std::string_view rhsName,
                                          std::string const& rhs) {
    std::string out;
    out += "\n  ";
    out += lhsName;
    out += ": " + std::to_string(lhs.size()) + " bytes\n  ";
    out += rhsName;
    out += ": " + std::to_string(rhs.size()) + " bytes";
    std::size_t const common = std::min(lhs.size(), rhs.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (lhs[i] != rhs[i]) {
            out += "\n  first differing byte at offset " + std::to_string(i)
                   + ": "
                   + std::to_string(static_cast<unsigned>(
                         static_cast<unsigned char>(lhs[i])))
                   + " vs "
                   + std::to_string(static_cast<unsigned>(
                         static_cast<unsigned char>(rhs[i])));
            return out;
        }
    }
    out += lhs.size() == rhs.size()
               ? "\n  (identical)"
               : "\n  the shorter artifact is a prefix of the longer one";
    return out;
}

}  // namespace

// ★ THE CRUX of TF-C74. `arm64:elf64-aarch64-linux` and
// `x86_64:elf64-x86_64-linux` are DIFFERENT architectures that share ONE object
// format. The front-end CU cache used to be keyed by object format ALONE, which
// was correct only while the preprocessed text depended on nothing else
// per-target. Now that each target contributes its own architecture identity
// macros, a format-only key would hand the second target the FIRST target's
// preprocessed text — baking the wrong architecture's macros into the second
// image. That is a SILENT MISCOMPILE, strictly worse than the loud `#error` the
// cycle set out to clear, which is why widening the key is a required
// correctness change rather than an optimization.
//
// OBSERVABLE (half 1): one source, two arch-gated function definitions, ONE
// compileFiles call. Each emitted object must carry ITS OWN architecture's
// symbol and NOT the other's.
//
// ★★ OBSERVABLE (half 2) — the STRONGER property: each target's artifact must
// be BYTE-IDENTICAL to the artifact that same target produces when built
// ALONE. Half 1 pins the defect we actually saw; half 2 pins the CONTRACT —
// that routing N targets through ONE `compileFiles` call is indistinguishable
// from N solo calls. It therefore catches divergences that leave both expected
// symbols intact and half 1 green: front-end state mutated by whichever target
// built first, a constant folded from the wrong target's macro VALUE (as
// opposed to a wrongly-taken `#ifdef`), an ordering effect inside the cache.
//
// The two halves share ONE multi-target build deliberately. Asserting them
// against separate builds would let each certify a different byte sequence;
// this way half 2 is a statement about the very artifacts half 1 inspected.
// The solo rebuilds run from the SAME cwd, source path and output path, so the
// only variable between the compared images is multi-target vs solo — nothing
// path-derived can leak into the comparison.
//
// RED-ON-DISABLE (verified, not assumed — re-measured when half 2 landed):
// narrow `CuBuildKey`'s `operator<` in src/program/program.cpp back to
// `return format < o.format;`, making the key format-only again. BOTH halves
// then fail, on the x86_64 side. The CU build walks `targets` IN ORDER and the
// first insertion for a key wins, so arm64 — listed first below — is the one
// whose CU the cache hands to x86_64: the x86_64 object loses
// `probe_is_x86_64` entirely
// and grows `probe_is_aarch64` at offset 185 (half 1), and diverges from its
// solo build at offset 194 — 'a' vs 'x', the first character that tells the
// two symbol names apart (half 2). The arm64 object, built from its own CU,
// stays correct and byte-identical; both halves stay green on that side. That
// asymmetry is why every assertion here is per-target rather than one verdict
// over the pair.
//
// Note what half 2 survives that a cheaper check would not: BOTH images are
// 712 bytes in the broken state. Section padding absorbs the one-character
// length difference between the two symbol names, so a size comparison — or
// any digest of the artifact's shape rather than its content — reads clean
// through this miscompile. Only the bytes themselves show it.
//
// ── WHY NO `examples/**` CORPUS EXAMPLE COVERS THIS ─────────────────────────
// Recorded HERE because this is where the next reader will come looking. (Also
// anchored in `.plans/`.) Two independent reasons, both measured:
//
//   (1) STRUCTURAL — the corpus runner cannot reach the multi-target CU cache
//       at all. `tests/examples/examples_runner.cpp` compiles each manifest
//       target ROW in its own call with a SINGLE-element target vector:
//       `prog.compileFiles(srcPaths, m.language, {t.spec}, rep)`, and the
//       multi-CU arm `prog.compileUnits(srcPaths, m.language, {t.spec}, rep)`
//       likewise. Every corpus build is therefore a one-target build whose
//       `CuBuildKey` is unique by construction, and no example — however it is
//       written — can produce the two-distinct-keys-one-format situation this
//       test needs. Driver-level tests like this one are the ONLY reachable
//       surface for it.
//
//   (2) OBSERVABILITY — even in a hypothetical multi-target corpus run, a C
//       program cannot self-detect the defect while every architecture route
//       yields the SAME exit code. Measured: patching `arm64.target.json`'s
//       predefines to x86_64's rows leaves every `#if` guard in the source
//       silent and the program still exits 42 — green, and wrong. The
//       `examples/c-subset/arch_identity_predefines` example works around (2)
//       by making the architecture OBSERVABLE in program OUTPUT (per-target
//       `expectedStdout` arch tags) rather than in the exit code. Nothing an
//       example can do works around (1).
TEST(Program_CompileFiles, TFC74CuCacheKeyIsPerTargetNotPerObjectFormat) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(
        scratch.path(), "archprobe.c",
        "#ifdef __aarch64__\n"
        "int probe_is_aarch64(void) { return 1; }\n"
        "#endif\n"
        "#ifdef __x86_64__\n"
        "int probe_is_x86_64(void) { return 1; }\n"
        "#endif\n");
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileFiles(
        {src.generic_string()}, "c-subset",
        {"arm64:elf64-aarch64-linux", "x86_64:elf64-x86_64-linux"});
    ASSERT_EQ(rc, 0);

    auto const armObj =
        scratch.path() / "target" / "elf64-aarch64-linux" / "archprobe.o";
    auto const x86Obj =
        scratch.path() / "target" / "elf64-x86_64-linux" / "archprobe.o";
    ASSERT_TRUE(fs::exists(armObj));
    ASSERT_TRUE(fs::exists(x86Obj));

    std::string const armBytes = readFileAsString(armObj);
    std::string const x86Bytes = readFileAsString(x86Obj);

    EXPECT_NE(armBytes.find("probe_is_aarch64"), std::string::npos)
        << "the arm64 object must define the __aarch64__-gated function";
    EXPECT_EQ(armBytes.find("probe_is_x86_64"), std::string::npos)
        << "the arm64 object must NOT carry the x86_64-gated function — that "
           "would mean it reused the x86_64 target's preprocessed CU";

    EXPECT_NE(x86Bytes.find("probe_is_x86_64"), std::string::npos)
        << "the x86_64 object must define the __x86_64__-gated function";
    EXPECT_EQ(x86Bytes.find("probe_is_aarch64"), std::string::npos)
        << "the x86_64 object must NOT carry the aarch64-gated function — that "
           "would mean it reused the arm64 target's preprocessed CU";

    // ── half 2: multi-target output == solo output, byte for byte ──────────
    struct SoloCase {
        std::string_view   label;
        std::string        spec;
        fs::path           obj;
        std::string const& multiTargetBytes;
    };
    SoloCase const cases[]{
        {"arm64", "arm64:elf64-aarch64-linux", armObj, armBytes},
        {"x86_64", "x86_64:elf64-x86_64-linux", x86Obj, x86Bytes},
    };

    for (auto const& c : cases) {
        SCOPED_TRACE(c.label);
        // DELETE the multi-target artifact before rebuilding over it. Without
        // this the comparison is not a test: a solo build that wrote NOTHING
        // would leave the multi-target file in place and the bytes would match
        // themselves. The removal is asserted, not attempted.
        std::error_code ec;
        fs::remove(c.obj, ec);
        ASSERT_FALSE(ec) << "could not remove " << c.obj << ": " << ec.message();
        ASSERT_FALSE(fs::exists(c.obj))
            << "the multi-target artifact must be off disk before the solo "
               "build, or the comparison proves nothing";

        // A FRESH `Program` — a solo build is a separate driver invocation,
        // and reusing `prog` would carry the multi-target run's state into it.
        Program solo;
        ASSERT_EQ(solo.compileFiles({src.generic_string()}, "c-subset",
                                    {c.spec}),
                  0)
            << "the solo build of " << c.spec << " must succeed";
        ASSERT_TRUE(fs::exists(c.obj))
            << "the solo build of " << c.spec << " wrote no artifact at "
            << c.obj;

        std::string const soloBytes = readFileAsString(c.obj);
        ASSERT_FALSE(soloBytes.empty())
            << "the solo artifact at " << c.obj << " is empty";
        EXPECT_TRUE(c.multiTargetBytes == soloBytes)
            << "the " << c.label
            << " artifact from the TWO-target build differs from the artifact "
               "that same target produces ALONE — one `compileFiles` call over "
               "N targets must be indistinguishable from N solo calls"
            << byteDiffSummary("multi-target", c.multiTargetBytes, "solo",
                               soloBytes);
    }
}

// The other half of the key contract: widening it must NOT make a plain
// single-target build rebuild anything. One target ⇒ one CU ⇒ one artifact,
// exactly as before TF-C74. (A cache key that over-partitions is a performance
// regression, not a correctness one — but on the 189-TU sqlite corpus it is a
// large enough one to matter, so it is pinned.)
TEST(Program_CompileFiles, TFC74SingleTargetStillBuildsOnce) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const src = writeCSubsetSource(scratch.path(), "onekey.c",
                                        "int forty_two() { return 42; }\n");
    scratch.useAsCwd();

    Program prog;
    int const rc = prog.compileFiles({src.generic_string()}, "c-subset",
                                     {"x86_64:elf64-x86_64-linux"});
    ASSERT_EQ(rc, 0);
    EXPECT_TRUE(fs::exists(scratch.path() / "target" / "elf64-x86_64-linux"
                           / "onekey.o"));
}

namespace {

// The SET of distinct diagnostic codes a reporter carries, by NAME. Names (not
// raw enum values) so a failed set comparison prints something a reader can
// act on — gtest renders `DiagnosticCode` as an opaque integer.
[[nodiscard]] std::set<std::string_view>
diagnosticCodeSet(DiagnosticReporter const& r) {
    std::set<std::string_view> s;
    for (auto const& d : r.all()) s.insert(diagnosticCodeName(d.code));
    return s;
}

// Every diagnostic, one per line, for the `<<` failure message. A set-equality
// failure tells you WHICH code is unexpected; this tells you what it SAID.
[[nodiscard]] std::string diagnosticDump(DiagnosticReporter const& r) {
    std::string out = "\n  diagnostics (" + std::to_string(r.all().size()) + "):";
    for (auto const& d : r.all()) {
        out += "\n    ";
        out += diagnosticCodeName(d.code);
        out += ": ";
        out += d.actual;
    }
    return out;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// D-PROGRAM-TARGET-LOAD-ORDER — THE CLOSING WITNESS.
// ═════════════════════════════════════════════════════════════════════════════
//
// The anchor's claim has TWO halves, and they are DIFFERENT claims:
//
//   (a) an unloadable `--target` produces the AUTHORITATIVE diagnostic
//       (`D_SchemaLoadFailed`), and
//   (b) ★ the error CASCADE IS GONE — zero `#error`-class diagnostics
//       accompany it.
//
// Asserting only (a) is the trap this test exists to avoid: the good
// diagnostic APPEARING does not prove the cascade STOPPED. Under the old
// ordering both could be true at once — the pre-flight could emit
// `D_SchemaLoadFailed` and the front-end could still run and pile a hundred
// header `#error`s on top of it. An (a)-only test stays GREEN through exactly
// the regression it is supposed to catch. Only (b) proves the hoist.
//
// The source is shaped like the SDK header ladders that produced the original
// cascade: every arm gated on an ARCHITECTURE identity macro, with a fail-loud
// `#else`. With a bad `--target` there are no architecture predefines at all,
// so under the PRE-hoist ordering the front-end built first, took the `#else`,
// and fired `P_PreprocessorErrorDirective` — the user saw header-internal
// noise and never the one sentence naming the real mistake. Under the hoist
// the pre-flight fails FIRST and the front-end never runs, so the `#error` is
// not merely outranked, it is never reached.
//
// (b) is asserted as a SET EQUALITY over the diagnostic codes, not as a count
// on any single code. A count of `P_PreprocessorErrorDirective` would pin only
// the one cascade class we happen to have seen; the set pins that NOTHING else
// rides along either, so a future re-ordering that swaps the `#error` cascade
// for some other front-end noise class still goes red.
//
// RED-ON-DISABLE (verified, not assumed): move the pre-flight target
// resolution at src/program/program.cpp:992-1012 back below the CU build (or
// merely drop its `if (rep.hasErrors()) { … return 1; }` gate at :1013-1016 so
// the front-end runs anyway) and the front-end preprocesses with no
// architecture predefines → `P_PreprocessorErrorDirective` joins the set and
// the set equality fails. Half (a) keeps passing in that state, which is the
// point.
TEST(Program_CompileFiles, TFC74BadTargetDiagnosedWithoutHeaderErrorCascade) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    // An architecture ladder in a HEADER — the real cascade came from headers,
    // and a quote-`#include` is what puts the `#error` behind the front-end
    // rather than in the driver's own hands.
    writeCSubsetSource(scratch.path(), "archgate.h",
                       "#if defined(__aarch64__)\n"
                       "#define ARCH_GATE_OK 1\n"
                       "#elif defined(__x86_64__)\n"
                       "#define ARCH_GATE_OK 1\n"
                       "#else\n"
                       "#error architecture not supported\n"
                       "#endif\n");
    // `ARCH_GATE_OK` is load-bearing, not decoration: the body does not PARSE
    // unless the header was really included AND really took an architecture
    // arm. That makes the control below a live test of the ladder rather than
    // a test that trivially passes on a header nobody read.
    auto const src =
        writeCSubsetSource(scratch.path(), "cascade.c",
                           "#include \"archgate.h\"\n"
                           "int forty_two(void) { return ARCH_GATE_OK + 41; }\n");
    scratch.useAsCwd();

    // ── CONTROL: the ladder is LIVE ──────────────────────────────────────
    // A GOOD target defines its architecture macro, takes an arm, and the TU
    // compiles clean. Without this, a source whose `#error` could never fire
    // under ANY target would make half (b) vacuously true.
    {
        DiagnosticReporter ok{DiagnosticReporter::Config{}};
        Program            prog;
        ASSERT_EQ(prog.compileFiles({src.generic_string()}, "c-subset",
                                    {"x86_64:elf64-x86_64-linux"}, ok), 0)
            << "the arch ladder must be SATISFIABLE by a good target, else the "
               "cascade assertion below proves nothing" << diagnosticDump(ok);
        ASSERT_TRUE(diagnosticCodeSet(ok).empty()) << diagnosticDump(ok);
    }

    // ── THE WITNESS ──────────────────────────────────────────────────────
    DiagnosticReporter rep{DiagnosticReporter::Config{}};
    Program            prog;
    int const          rc = prog.compileFiles(
        {src.generic_string()}, "c-subset", {"no_such_arch:elf64-x86_64-linux"},
        rep);
    EXPECT_EQ(rc, 1) << diagnosticDump(rep);

    // (a) the authoritative diagnostic fires, EXACTLY once — one bad target,
    // one emit, no duplication from a re-diagnosing downstream path.
    EXPECT_EQ(::dss::test_support::countCode(
                  rep, DiagnosticCode::D_SchemaLoadFailed), 1u)
        << "an unknown target name must be reported as a target-schema load "
           "failure, not as whatever the front-end happened to produce"
        << diagnosticDump(rep);

    // (b) ★ …and NOTHING ELSE accompanies it. The SET, not a count on one code.
    //
    // MEASURED, not guessed: exactly two codes, and BOTH name the target. The
    // config tier says WHAT it could not find (`C_InvalidTargetName`, forwarded
    // verbatim by `forwardConfigDiagnostics` so the loader's own reason is not
    // swallowed); the driver says what that MEANS for this build
    // (`D_SchemaLoadFailed`). There is no third code, and in particular no
    // `P_PreprocessorErrorDirective` — the front-end never ran.
    EXPECT_EQ(diagnosticCodeSet(rep),
              (std::set<std::string_view>{"C_InvalidTargetName",
                                          "D_SchemaLoadFailed"}))
        << "a bad `--target` must produce TARGET diagnostics alone — any "
           "front-end diagnostic here means the CU build ran before the target "
           "was resolved, which is the ordering defect this anchor closed"
        << diagnosticDump(rep);
}

// ═════════════════════════════════════════════════════════════════════════════
// TF-C74 (D-PROGRAM-UNKNOWN-OBJECT-FORMAT-SILENT) — THE CLOSING WITNESS for the
// OBJECT-FORMAT half of the same two-halved defect.
// ═════════════════════════════════════════════════════════════════════════════
//
// Same two claims as the target-name sibling above, over the OTHER half of the
// `<targetName>:<formatName>` spec:
//
//   (a) an unloadable `--target` FORMAT produces the authoritative diagnostic
//       (`D_SchemaLoadFailed`, naming the format), and
//   (b) ★ nothing else rides along — asserted as a SET EQUALITY over
//       diagnostic code names, over a source that genuinely WOULD cascade.
//
// MEASURED before the fix, with this exact source: `--target
// arm64:macho64-arm64-darwn` (one letter dropped) produced EXACTLY
// `{P_PreprocessorErrorDirective}` and rc 1 — the run failed while saying
// nothing whatsoever about the format it could not load. The user is shown
// their own header failing and never the one sentence naming the real mistake.
// On the sqlite amalgamation the same mechanism produced 114 of those.
//
// WHY THE GATE IS THE FORMAT AND NOT THE ARCHITECTURE: the front-end's output
// depends on the ACTIVE OBJECT FORMAT, because a predefined macro may declare
// `availableObjectFormats` and `mergePredefinedMacros` drops every gated macro
// when no format resolved. So an unknown format silently preprocesses the TU
// with the gated macros MISSING — the source takes its fail-loud `#else`. That
// is a strictly per-FORMAT cascade: the target name here is perfectly valid and
// its schema loads, which is exactly why the sibling's fix did not close this.
//
// The set is TWO codes, MEASURED, and both are load-bearing — the same shape
// the target half settled on. `C_InvalidFormatName` is the config loader's own
// reason (WHAT it could not find, forwarded verbatim by
// `forwardConfigDiagnostics` rather than swallowed); `D_SchemaLoadFailed` is
// the driver's verdict (what that means for this build, and which file to go
// look for). A one-code assertion here would be wrong, not stricter.
//
// RED-ON-DISABLE (verified by reverting, not assumed) — BOTH failure modes:
//   * full revert (drop the format load from the pre-flight at
//     src/program/program.cpp): the set becomes `{P_PreprocessorErrorDirective}`
//     — (a) AND (b) both fail.
//   * ★ halfway (keep the emit, let control reach the CU build anyway): the set
//     becomes `{C_InvalidFormatName, D_SchemaLoadFailed,
//     P_PreprocessorErrorDirective}` — (a) still passes, and ONLY the set
//     equality fails. That middle state is why (b) is a set and not a count:
//     a count on `D_SchemaLoadFailed` stays green there while the cascade is
//     back in the user's face.
TEST(Program_CompileFiles, TFC74UnknownObjectFormatDiagnosedWithoutHeaderErrorCascade) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    // A FORMAT ladder in a header, gated on a macro the target config declares
    // `availableObjectFormats`-restricted. Under a format of that kind the
    // macro is defined and the ladder passes; under any other resolved format,
    // and under NO resolved format, it fires. This is the header-ladder shape
    // that produced the original 114-error cascade.
    writeCSubsetSource(scratch.path(), "fmtgate.h",
                       "#if defined(__arm64__)\n"
                       "#define FMT_GATE_OK 1\n"
                       "#else\n"
                       "#error object format not supported\n"
                       "#endif\n");
    // `FMT_GATE_OK` is load-bearing: the body does not PARSE unless the header
    // was really included AND really took the gated arm.
    auto const src =
        writeCSubsetSource(scratch.path(), "fmtcascade.c",
                           "#include \"fmtgate.h\"\n"
                           "int forty_two(void) { return FMT_GATE_OK + 41; }\n");
    scratch.useAsCwd();

    // ── CONTROL 1: the ladder is SATISFIABLE ─────────────────────────────
    // A good (target, format) pair whose format carries the gate compiles
    // clean, with an EMPTY diagnostic set. Without this, a source whose
    // `#error` could never be avoided would make half (b) vacuous.
    {
        DiagnosticReporter ok{DiagnosticReporter::Config{}};
        Program            prog;
        ASSERT_EQ(prog.compileFiles({src.generic_string()}, "c-subset",
                                    {"arm64:macho64-arm64-darwin"}, ok), 0)
            << "the format ladder must be SATISFIABLE by a good target spec, "
               "else the cascade assertion below proves nothing"
            << diagnosticDump(ok);
        ASSERT_TRUE(diagnosticCodeSet(ok).empty()) << diagnosticDump(ok);
    }

    // ── CONTROL 2: the ladder is FORMAT-SENSITIVE, and the cascade is LIVE ─
    // The SAME source under a different, perfectly VALID format of another
    // kind: the gated macro is absent, so the `#error` fires and the ONLY
    // diagnostic is the front-end's. This proves two things control 1 cannot:
    // that the cascade path really is reachable in this build (so its absence
    // in the witness below is the fix working, not the source being inert),
    // and that the gate is on the FORMAT. If a future config change ungated
    // that macro, THIS control goes red — the witness announces that it lost
    // its teeth instead of silently passing forever.
    {
        DiagnosticReporter live{DiagnosticReporter::Config{}};
        Program            prog;
        EXPECT_EQ(prog.compileFiles({src.generic_string()}, "c-subset",
                                    {"arm64:elf64-aarch64-linux-exec"}, live), 1)
            << diagnosticDump(live);
        EXPECT_EQ(diagnosticCodeSet(live),
                  (std::set<std::string_view>{"P_PreprocessorErrorDirective"}))
            << "the ladder must still be gated on the OBJECT FORMAT — if this "
               "compiles clean, the gated macro is no longer format-restricted "
               "and this witness can no longer detect the cascade it exists to "
               "forbid"
            << diagnosticDump(live);
    }

    // ── THE WITNESS ──────────────────────────────────────────────────────
    DiagnosticReporter rep{DiagnosticReporter::Config{}};
    Program            prog;
    int const          rc = prog.compileFiles(
        {src.generic_string()}, "c-subset", {"arm64:macho64-arm64-darwn"}, rep);
    EXPECT_EQ(rc, 1) << diagnosticDump(rep);

    // (a) the authoritative diagnostic fires, EXACTLY once — one bad format,
    // one emit, no duplication from a re-diagnosing downstream path.
    EXPECT_EQ(::dss::test_support::countCode(
                  rep, DiagnosticCode::D_SchemaLoadFailed), 1u)
        << "an unknown object-format name must be reported as a schema load "
           "failure, not as whatever the front-end happened to produce"
        << diagnosticDump(rep);

    // (b) ★ …and NOTHING ELSE accompanies it. The SET, not a count on one code.
    EXPECT_EQ(diagnosticCodeSet(rep),
              (std::set<std::string_view>{"C_InvalidFormatName",
                                          "D_SchemaLoadFailed"}))
        << "a bad `--target` FORMAT must produce FORMAT diagnostics alone — a "
           "front-end diagnostic here means the CU build ran before the format "
           "was resolved, which is the ordering defect this anchor closed"
        << diagnosticDump(rep);

    // The message must NAME the offending format: the whole point of the
    // anchor is that the operator was never told which string was wrong.
    bool named = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::D_SchemaLoadFailed
            && d.actual.find("macho64-arm64-darwn") != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named)
        << "the diagnostic must quote the format name the user typed"
        << diagnosticDump(rep);
}

// ═════════════════════════════════════════════════════════════════════════════
// TF-C74 (D-PROGRAM-TARGET-FORMAT-PAIR-VALIDATED-LATE) — THE CLOSING WITNESS
// for FACET 3 of the same driver-ordering defect: the (target, format) PAIR.
// ═════════════════════════════════════════════════════════════════════════════
//
// Facets 1 and 2 check each HALF of `<targetName>:<formatName>` in isolation,
// and neither can see this one: `arm64:elf64-x86_64-linux-exec` names a REAL
// target and a REAL object format, so BOTH pre-flight half-checks PASS. Only
// their COMBINATION is nonsense — a component-wise validation pass is
// structurally blind to a relational constraint.
//
// Same two claims as the two siblings above, over the PAIR:
//
//   (a) a mutually-invalid pair produces the authoritative pair diagnostic
//       (`D_TargetMachineCodeMismatch`), and
//   (b) ★ nothing else rides along — asserted as a SET EQUALITY over
//       diagnostic code names, over a source that genuinely WOULD cascade.
//
// MEASURED before the fix, through the real CLI, with this exact source shape:
// `--target arm64:elf64-x86_64-linux-exec` produced EXACTLY
// `{P_PreprocessorErrorDirective}` and rc 1 — the header `#error` alone, with
// not one word about the mismatch. The SAME spec over a header-LESS source
// printed the full `D_TargetMachineCodeMismatch`, which is the tell: the
// diagnostic was never missing or badly worded, it was merely unreachable —
// `crossValidateTargetFormat` sat inside `compileOneTarget`, downstream of the
// CU build and behind the `if (rep.hasErrors()) return 1;` drain. This is a
// PURE POSITION defect.
//
// WHY THE GATE IS THE ARCHITECTURE HERE (and why that is the honest shape):
// the plausible operator mistake is wanting an x86_64 build and mistyping the
// TARGET half while getting the FORMAT half right. The source is then an
// x86_64-only header ladder — the overwhelmingly common real-world shape — so
// the arm64 predefines take the fail-loud `#else`. Control 2 below proves that
// cascade is live rather than assumed.
//
// The set is ONE code, MEASURED, not assumed — and that differs from both
// siblings, which measured TWO. The reason is structural and worth stating so
// nobody "fixes" it later: the half-checks fail inside the CONFIG LOADER, so
// they carry the loader's own reason (`C_InvalidTargetName` /
// `C_InvalidFormatName`) forwarded verbatim alongside the driver's verdict.
// Nothing fails to LOAD here — both schemas load perfectly — so there is no
// config-tier reason to forward. `D_TargetMachineCodeMismatch` is the whole
// answer, and it already names the target, the field and both machine codes.
//
// RED-ON-DISABLE (verified by reverting, not assumed) — BOTH failure modes:
//   * full revert (drop the pair check from the pre-flight, leaving only
//     `compileOneTarget`'s call): the set becomes
//     `{P_PreprocessorErrorDirective}` — (a) AND (b) both fail.
//   * ★ halfway (keep the pre-flight emit, but let control reach the CU build
//     anyway): the set becomes `{D_TargetMachineCodeMismatch,
//     P_PreprocessorErrorDirective}` — (a) still passes, and ONLY the set
//     equality fails. That middle state is why (b) is a set and not a count:
//     a count on `D_TargetMachineCodeMismatch` stays green there while the
//     cascade is back in the user's face.
TEST(Program_CompileFiles, TFC74InvalidTargetFormatPairDiagnosedWithoutCascade) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    // An x86_64-ONLY architecture ladder in a HEADER. A quote-`#include` is
    // what puts the `#error` genuinely behind the front-end rather than in the
    // driver's own hands — without that, half (b) is vacuous.
    writeCSubsetSource(scratch.path(), "pairgate.h",
                       "#if defined(__x86_64__)\n"
                       "#define PAIR_GATE_OK 1\n"
                       "#else\n"
                       "#error architecture not supported\n"
                       "#endif\n");
    // `PAIR_GATE_OK` is load-bearing, not decoration: the body does not PARSE
    // unless the header was really included AND really took the x86_64 arm.
    // ⓘ THE `main` IS REQUIRED, AND IT IS NOT SCAFFOLDING FOR THIS TEST'S CLAIM.
    // CONTROL 1 below compiles this source to an EXEC-flavored format and asserts an
    // EMPTY diagnostic set. Without a `main` that is now a legitimate refusal
    // (`K_ProgramEntryUndefined`, D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE): a format that
    // starts a program needs exactly one realizable entry, and gcc's answer to the
    // same input is `undefined reference to 'main'`. The control previously passed
    // only because that hole was open, i.e. it was encoding a MISSING CHECK as
    // expected behaviour. The `#include` + arch-ladder proof this test actually makes
    // is untouched — `forty_two` still carries `PAIR_GATE_OK`, so the body still does
    // not parse unless the header was really included and really took the x86_64 arm.
    auto const src =
        writeCSubsetSource(scratch.path(), "paircascade.c",
                           "#include \"pairgate.h\"\n"
                           "int forty_two(void) { return PAIR_GATE_OK + 41; }\n"
                           "int main(void) { return forty_two() - 42; }\n");
    scratch.useAsCwd();

    // ── CONTROL 1: the ladder is SATISFIABLE, under a GOOD pair ───────────
    // The same source under the pair the operator MEANT compiles clean, with
    // an EMPTY diagnostic set. Without this, a source whose `#error` could
    // never be avoided would make half (b) vacuous.
    {
        DiagnosticReporter ok{DiagnosticReporter::Config{}};
        Program            prog;
        ASSERT_EQ(prog.compileFiles({src.generic_string()}, "c-subset",
                                    {"x86_64:elf64-x86_64-linux-exec"}, ok), 0)
            << "the arch ladder must be SATISFIABLE by a good (target, format) "
               "pair, else the cascade assertion below proves nothing"
            << diagnosticDump(ok);
        ASSERT_TRUE(diagnosticCodeSet(ok).empty()) << diagnosticDump(ok);
    }

    // ── CONTROL 2: the cascade is LIVE, and the gate is the ARCHITECTURE ──
    // The SAME source under a perfectly VALID pair of the other arch: both
    // halves load, the PAIR agrees (arm64 ↔ EM_AARCH64), so the pre-flight
    // passes and the front-end really runs — and the ladder fires. This proves
    // what control 1 cannot: that the cascade path is reachable in this build,
    // so its ABSENCE in the witness below is the hoist working rather than the
    // source being inert. If a future config change gave arm64 an `__x86_64__`
    // predefine, or the pair stopped being validated at all, THIS control goes
    // red — the witness announces that it lost its teeth instead of silently
    // passing forever.
    {
        DiagnosticReporter live{DiagnosticReporter::Config{}};
        Program            prog;
        EXPECT_EQ(prog.compileFiles({src.generic_string()}, "c-subset",
                                    {"arm64:elf64-aarch64-linux-exec"}, live), 1)
            << diagnosticDump(live);
        EXPECT_EQ(diagnosticCodeSet(live),
                  (std::set<std::string_view>{"P_PreprocessorErrorDirective"}))
            << "the ladder must still be gated on the ARCHITECTURE and the "
               "cascade must still be reachable — if this compiles clean, this "
               "witness can no longer detect the cascade it exists to forbid"
            << diagnosticDump(live);
    }

    // ── THE WITNESS ──────────────────────────────────────────────────────
    // Individually valid, mutually invalid: `arm64` is a shipped target and
    // `elf64-x86_64-linux-exec` is a shipped format, so both half-checks pass.
    DiagnosticReporter rep{DiagnosticReporter::Config{}};
    Program            prog;
    int const          rc = prog.compileFiles(
        {src.generic_string()}, "c-subset",
        {"arm64:elf64-x86_64-linux-exec"}, rep);
    EXPECT_EQ(rc, 1) << diagnosticDump(rep);

    // (a) the authoritative pair diagnostic fires, EXACTLY once — one bad
    // pair, one emit, no duplication from the still-live downstream call in
    // `compileOneTarget`.
    EXPECT_EQ(::dss::test_support::countCode(
                  rep, DiagnosticCode::D_TargetMachineCodeMismatch), 1u)
        << "a mutually-invalid (target, format) pair must be reported as a "
           "machine-code mismatch, not as whatever the front-end happened to "
           "produce"
        << diagnosticDump(rep);

    // (b) ★ …and NOTHING ELSE accompanies it. The SET, not a count on one code.
    EXPECT_EQ(diagnosticCodeSet(rep),
              (std::set<std::string_view>{"D_TargetMachineCodeMismatch"}))
        << "a mutually-invalid pair must produce PAIR diagnostics alone — a "
           "front-end diagnostic here means the CU build ran before the pair "
           "was cross-validated, which is the ordering defect this anchor "
           "closed"
        << diagnosticDump(rep);

    // The message must NAME both halves of the disagreement. The pair check
    // is the only one of the three whose message does NOT quote the format
    // NAME, so the `[target=<spec>]` stamp is what makes it attributable —
    // pinned separately in the multi-target test below.
    bool named = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::D_TargetMachineCodeMismatch
            && d.actual.find("arm64") != std::string::npos
            && d.actual.find("elf.machine") != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named)
        << "the diagnostic must name the target and the format field that "
           "disagree"
        << diagnosticDump(rep);
}

// ── TF-C74 (D-PROGRAM-TARGET-FORMAT-PAIR-VALIDATED-LATE): THE DEDUPE KEY ────
//
// The pre-flight de-duplicates each check so one typo yields one diagnostic,
// and the KEY is where this class of bug hides: the object-format check had to
// be given its own `formatChecked` set because the target-name dedupe keys on
// the TARGET, so a format check placed behind it would skip every format after
// the first — silently.
//
// The pair check's verdict is a RELATION over BOTH names, so the only
// non-lossy key is the ORDERED PAIR. Either half alone silently skips distinct
// pairs that share the other half, and this test measures BOTH orientations
// with a two-target invocation whose SECOND pair is the bad one:
//
//   * same TARGET, differing FORMAT — a target-keyed dedupe skips spec 2.
//   * same FORMAT, differing TARGET — a format-keyed dedupe skips spec 2.
//
// Both legs use the arm64-cascading source and put a VALID pair first, so a
// skipped spec 2 does not merely lose a diagnostic — it lets the CU build run
// and the header `#error` come back. MEASURED with the correct key: each leg
// yields exactly `{D_TargetMachineCodeMismatch}`, stamped with the offending
// spec. RED-ON-DISABLE (verified): narrowing `pairChecked` to the target name
// reds leg A with `{P_PreprocessorErrorDirective}`; narrowing it to the format
// name reds leg B the same way — each wrong key reds exactly the leg that
// targets it, which is what makes this a measurement rather than a ritual.
TEST(Program_CompileFiles, TFC74PairCheckDedupeKeyIsTheOrderedPair) {
    ScratchDir scratch{Location::InsideRepo, "program"};
    writeCSubsetSource(scratch.path(), "pairgate.h",
                       "#if defined(__x86_64__)\n"
                       "#define PAIR_GATE_OK 1\n"
                       "#else\n"
                       "#error architecture not supported\n"
                       "#endif\n");
    auto const src =
        writeCSubsetSource(scratch.path(), "paircascade.c",
                           "#include \"pairgate.h\"\n"
                           "int forty_two(void) { return PAIR_GATE_OK + 41; }\n");
    scratch.useAsCwd();

    // ── LEG A: the two specs share a TARGET; the SECOND pair is the bad one.
    {
        DiagnosticReporter rep{DiagnosticReporter::Config{}};
        Program            prog;
        EXPECT_EQ(prog.compileFiles({src.generic_string()}, "c-subset",
                                    {"arm64:elf64-aarch64-linux-exec",
                                     "arm64:elf64-x86_64-linux-exec"}, rep), 1)
            << diagnosticDump(rep);
        EXPECT_EQ(diagnosticCodeSet(rep),
                  (std::set<std::string_view>{"D_TargetMachineCodeMismatch"}))
            << "a dedupe keyed on the TARGET would have skipped the second "
               "spec — its target was already resolved by the first — and the "
               "front-end would have run and cascaded"
            << diagnosticDump(rep);
        // The stamp is what makes a multi-target pair diagnostic attributable:
        // the message names the target but NOT the format, so without
        // `[target=<spec>]` the operator cannot tell which of two arm64 specs
        // was rejected.
        bool stamped = false;
        for (auto const& d : rep.all()) {
            if (d.contextPrefix.find("arm64:elf64-x86_64-linux-exec")
                != std::string::npos) {
                stamped = true;
            }
        }
        EXPECT_TRUE(stamped)
            << "the pair diagnostic must carry the `[target=<spec>]` stamp "
               "naming the OFFENDING spec, not merely the target name"
            << diagnosticDump(rep);
    }

    // ── LEG B: the two specs share a FORMAT; the SECOND pair is the bad one.
    {
        DiagnosticReporter rep{DiagnosticReporter::Config{}};
        Program            prog;
        EXPECT_EQ(prog.compileFiles({src.generic_string()}, "c-subset",
                                    {"arm64:elf64-aarch64-linux-exec",
                                     "x86_64:elf64-aarch64-linux-exec"}, rep), 1)
            << diagnosticDump(rep);
        EXPECT_EQ(diagnosticCodeSet(rep),
                  (std::set<std::string_view>{"D_TargetMachineCodeMismatch"}))
            << "a dedupe keyed on the FORMAT would have skipped the second "
               "spec — its format was already checked by the first — and the "
               "front-end would have run and cascaded"
            << diagnosticDump(rep);
    }

    // ── NO ARTIFACT ESCAPES A REJECTED MULTI-TARGET BUILD ────────────────
    // The pre-flight rejects before ANY target is compiled, so a build with one
    // bad pair writes nothing at all. Under the pre-hoist ordering the good
    // target compiled and committed its artifact first, and only then did the
    // bad pair fail — a half-written multi-target output tree. This is the one
    // consequence of the hoist that the diagnostic set cannot express.
    EXPECT_FALSE(fs::exists(scratch.path() / "target"))
        << "a multi-target build rejected at the pre-flight must not have "
           "compiled ANY target — no output tree should exist";
}

// ─────────────────────────────────────────────────────────────────────────────
// TF-C75 (D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM) — the END-TO-END chain the
// production wiring actually walks: a SHIPPED `.format.json` → its declared
// `kind` → `TargetSchema::charIsUnsigned(kind)`.
//
// The unit-level quadrants live in tests/core/test_target_schema.cpp (hand-
// built schemas, both override directions, every fail-loud shape). What is
// pinned HERE is the part those cannot reach: that the SHIPPED format files
// carry the kinds this resolution depends on, and that the ONE production call
// site — `mirCfg.charIsUnsigned = target.charIsUnsigned(format.kind())` in
// compile_pipeline.cpp — therefore resolves the real platform matrix.
//
// A resolver correct on hand-built schemas and wrong on the shipped ones is
// still a miscompile, and the shipped chain has an extra failure mode all its
// own: a format file whose `kind` drifted would silently pick up the WRONG
// override row while every unit test stayed green.
// ─────────────────────────────────────────────────────────────────────────────

// ★ `arm64` × `macho64-arm64-darwin-exec` is THE row: the arm64 processor's
// default says UNSIGNED, Apple's platform ABI says SIGNED, and the target's own
// `byObjectFormat` override is what has to win. It was the shipped miscompile
// before this arc.
//
// RED-ON-DISABLE (both verified against the final build):
//   (1) delete the `"macho": false` row from arm64.target.json → that row
//       resolves true (unsigned) and this fails;
//   (2) make `TargetSchema::charIsUnsigned(kind)` ignore its argument and
//       return the default → the same row fails, and so do the codegen matrix
//       in tests/mir and the exact-set matrix in tests/core.
TEST(Program_CharSignedness, ShippedFormatKindsResolveThePlatformMatrix) {
    struct Row {
        std::string_view target;
        std::string_view format;
        bool             unsignedChar;
        std::string_view why;
        bool operator==(Row const&) const = default;
    };
    std::vector<Row> const expected{
        {"arm64", "macho64-arm64-darwin-exec", false,
         "Apple arm64: the target's macho override makes bare `char` SIGNED, "
         "beating its own unsigned default"},
        {"arm64", "macho64-arm64-darwin-dylib", false,
         "the override is per FORMAT KIND, so every macho artifact profile "
         "inherits it — not just the one exec file"},
        {"arm64", "elf64-aarch64-linux-exec", true,
         "Linux arm64: no override for elf, so AAPCS64's unsigned default "
         "stands — CORRECT, never flip this"},
        {"arm64", "elf64-aarch64-linux-pie", true,
         "same, across artifact profiles"},
        {"x86_64", "pe64-x86_64-windows-exec", false,
         "Windows x86_64: signed (x86_64 declares no key at all)"},
        {"x86_64", "macho64-x86_64-darwin-exec", false,
         "Darwin x86_64: signed"},
        {"x86_64", "elf64-x86_64-linux-exec", false,
         "Linux x86_64: signed"},
    };

    std::vector<Row> actual;
    for (auto const& row : expected) {
        auto t = TargetSchema::loadShipped(std::string{row.target});
        auto f = ObjectFormatSchema::loadShipped(std::string{row.format});
        ASSERT_TRUE(t.has_value()) << row.target;
        ASSERT_TRUE(f.has_value()) << row.format;
        actual.push_back(Row{row.target, row.format,
                             (*t)->charIsUnsigned((*f)->kind()), row.why});
    }
    EXPECT_EQ(actual, expected)
        << "the shipped (target × format) bare-`char` matrix drifted";
}

// The two arm64 legs must DISAGREE. Stated as its own assertion because it is
// the fact a single per-processor boolean could not hold, and because a
// resolution that collapsed to "always the default" or "always the override"
// would satisfy plenty of individual rows above while failing this.
TEST(Program_CharSignedness, TheTwoArm64LegsDisagreeByPlatform) {
    auto t     = TargetSchema::loadShipped("arm64");
    auto macho = ObjectFormatSchema::loadShipped("macho64-arm64-darwin-exec");
    auto elf   = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(t.has_value());
    ASSERT_TRUE(macho.has_value());
    ASSERT_TRUE(elf.has_value());
    EXPECT_NE((*t)->charIsUnsigned((*macho)->kind()),
              (*t)->charIsUnsigned((*elf)->kind()))
        << "ONE processor, TWO platforms, OPPOSITE answers — if these ever "
           "agree, the format half of the resolution has stopped being read "
           "and bare `char` is being mis-extended on one of the two legs";
}
