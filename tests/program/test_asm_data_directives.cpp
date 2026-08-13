// ★★★ A HAND-WRITTEN `.s` PUTS ITS DATA IN THE IMAGE
// (D-ASM-DATA-ITEMS-NOT-WIRED-INTO-THE-DRIVER, closed 2026-08-13).
//
// The text→LIR walker has understood `.data` / `.byte` / `.word` / `.quad` /
// `.zero` since the directive verbs landed: it produced a fully-formed
// `AsmTextModule::dataItems` vector, correctly sized and correctly laid out.
// The driver then DROPPED IT ON THE FLOOR. `assemble()` builds its module from
// the LIR, and LIR carries no data, so the walker's vector was the only copy
// and `assembleAsmUnit` never read it — every byte a `.s` declared was
// computed and discarded, with no diagnostic anywhere.
//
// ★★ WHY THE OBSERVABLE IS THE EMITTED FILE'S BYTES AND NOT `dataItems.size()`.
// "The walker produced items" was ALREADY TRUE while the defect was live —
// asserting it would have passed against the broken driver. The two claims
// that differ are "the bytes are computed" and "the bytes are in the image",
// and only the second one is what a program can read at runtime. So the pin
// searches the linked artifact for the exact little-endian byte string the
// source declared.
//
// ── WHY THIS TIER AND NOT `examples/**` ─────────────────────────────────────
// A corpus example asserts an EXIT CODE, so it can only witness data the
// program READS BACK — and assembly cannot yet name a data symbol in an
// ordinary instruction (`adr x0, sym` fails loud, "symbolic operand is not yet
// lowered by this build"; anchored D-ASM-SYMBOL-OPERAND-NOT-LOWERED). Until
// that lands, the strongest available witness is the image itself, and that is
// a driver-tier observable. ⚠ THIS COMMENT IS THE TRIGGER: when symbol
// operands lower, this file's claim becomes reachable from a running program
// and an `examples/asm/**` fixture should take over the end-to-end half.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// ★ THE arm64 DIALECT IS THE SUBJECT because it is the one that declares the
// data directives. The claim under test is the DRIVER's, which is dialect- and
// target-blind — the same `assembleAsmUnit` line serves every dialect — so a
// second dialect would re-measure the driver through a different config rather
// than test a second driver.
//
// ⚠ EVERY VALUE IS CHOSEN TO BE A DISTINCTIVE BYTE STRING, and the `.quad` is
// the primary witness: `0x1122334455667788` little-endian is 8 bytes that will
// not occur by chance in an ELF header, a symbol table or a code section. A
// witness like `.quad 1` would be found in a dozen places and the pin would
// pass against a driver that emitted nothing.
constexpr std::string_view kDataSource =
    "\t.data\n"
    "\t.globl\tdsstable\n"
    "dsstable:\n"
    "\t.quad\t0x1122334455667788\n"
    "\t.byte\t17, 34, 51\n"
    "\t.word\t1000\n"
    "\t.zero\t5\n"
    "\n"
    "\t.text\n"
    "\t.globl\tmain\n"
    "\t.type\tmain, %function\n"
    "main:\n"
    "\tmov\tx0, #11\n"
    "\tret\n";

constexpr std::string_view kArm64Dialect = "asm-arm64-gas";
constexpr std::string_view kArm64Spec    = "arm64:elf64-aarch64-linux-exec";

// The `.quad`, little-endian — the load-bearing witness.
constexpr std::uint8_t kQuadLE[] = {0x88, 0x77, 0x66, 0x55,
                                    0x44, 0x33, 0x22, 0x11};
// `.byte 17, 34, 51` immediately followed by `.word 1000` (FOUR bytes on this
// port, not two) — pins the element widths as they land in the image, not just
// as the loader parsed them.
constexpr std::uint8_t kBytesThenWordLE[] = {0x11, 0x22, 0x33,
                                             0xE8, 0x03, 0x00, 0x00};

fs::path writeFile(fs::path const& dir, std::string_view name,
                   std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream f(p, std::ios::binary);
    f << text;
    return p;
}

[[nodiscard]] std::vector<std::uint8_t> readAll(fs::path const& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),
                                     std::istreambuf_iterator<char>());
}

[[nodiscard]] bool contains(std::vector<std::uint8_t> const& hay,
                            std::span<std::uint8_t const>   needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end())
           != hay.end();
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

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// THE HEADLINE: the bytes a `.s` declares reach the linked image.
// ════════════════════════════════════════════════════════════════════════════
//
// ── RED-ON-DISABLE (measured 2026-08-13) ────────────────────────────────────
// Delete `assembled.dataItems = std::move(lowered->dataItems);` from
// `assembleAsmUnit` (src/program/compile_pipeline.cpp). The compile still
// SUCCEEDS with rc=0 and emits an artifact — which is precisely why the defect
// survived — and both byte-string searches below come back empty. The mutant
// differs from the subject by content, not by line count, and the same
// `contains()` matcher the pin uses is what reports the witness absent.
TEST(AsmDataDirectives, DeclaredBytesReachTheLinkedImage) {
    ScratchDir scratch{Location::InsideRepo, "asm-data"};
    scratch.useAsCwd();
    auto const src = writeFile(scratch.path(), "withdata.s", kDataSource);

    DiagnosticReporter rep;
    Program            prog;
    int const rc = prog.compileFiles({src.generic_string()},
                                     std::string{kArm64Dialect},
                                     {std::string{kArm64Spec}}, rep);
    ASSERT_EQ(rc, 0) << "a `.s` declaring data must build\n"
                     << allDiagnosticText(rep);

    auto const artifact =
        scratch.path() / "target" / "elf64-aarch64-linux-exec" / "withdata";
    ASSERT_TRUE(fs::exists(artifact)) << artifact.generic_string();

    auto const image = readAll(artifact);
    ASSERT_FALSE(image.empty());

    EXPECT_TRUE(contains(image, kQuadLE))
        << "the `.quad` the source declared is ABSENT from the emitted image — "
           "the walker computed it and the driver dropped it";
    EXPECT_TRUE(contains(image, kBytesThenWordLE))
        << "the `.byte`/`.word` run is ABSENT from the emitted image (this "
           "witness also pins `.word` at FOUR bytes on this port)";
}

// ════════════════════════════════════════════════════════════════════════════
// A `.s` WITH NO DATA STAYS EXACTLY AS IT WAS.
// ════════════════════════════════════════════════════════════════════════════
//
// ⚠ THE COMPANION THAT KEEPS THE PIN HONEST. The test above would also pass if
// the driver had started inventing a data section for every unit; this one says
// the new assignment is driven by what the SOURCE declared. It is the cheap
// control that separates "the wiring works" from "something now always emits
// data".
TEST(AsmDataDirectives, SourceWithoutDataDeclaresNoDataBytes) {
    constexpr std::string_view kNoData =
        "\t.text\n"
        "\t.globl\tmain\n"
        "\t.type\tmain, %function\n"
        "main:\n"
        "\tmov\tx0, #11\n"
        "\tret\n";

    ScratchDir scratch{Location::InsideRepo, "asm-data-none"};
    scratch.useAsCwd();
    auto const src = writeFile(scratch.path(), "nodata.s", kNoData);

    DiagnosticReporter rep;
    Program            prog;
    int const rc = prog.compileFiles({src.generic_string()},
                                     std::string{kArm64Dialect},
                                     {std::string{kArm64Spec}}, rep);
    ASSERT_EQ(rc, 0) << allDiagnosticText(rep);

    auto const artifact =
        scratch.path() / "target" / "elf64-aarch64-linux-exec" / "nodata";
    ASSERT_TRUE(fs::exists(artifact));
    EXPECT_FALSE(contains(readAll(artifact), kQuadLE))
        << "no source declared this byte string, so nothing may emit it";
}

// ════════════════════════════════════════════════════════════════════════════
// A DATA VALUE THAT DOES NOT FIT ITS ELEMENT IS REFUSED, NOT TRUNCATED.
// ════════════════════════════════════════════════════════════════════════════
//
// The wiring above is only worth having if what flows through it is right.
// `.byte 300` has no correct one-byte encoding, and silently storing 0x2C is
// the failure this refuses — the same class as the dropped bytes, one level
// down.
TEST(AsmDataDirectives, ValueTooWideForItsElementFailsLoud) {
    constexpr std::string_view kTooWide =
        "\t.data\n"
        "t:\n"
        "\t.byte\t300\n"
        "\t.text\n"
        "\t.globl\tmain\n"
        "\t.type\tmain, %function\n"
        "main:\n"
        "\tmov\tx0, #0\n"
        "\tret\n";

    ScratchDir scratch{Location::InsideRepo, "asm-data-wide"};
    scratch.useAsCwd();
    auto const src = writeFile(scratch.path(), "toowide.s", kTooWide);

    DiagnosticReporter rep;
    Program            prog;
    int const rc = prog.compileFiles({src.generic_string()},
                                     std::string{kArm64Dialect},
                                     {std::string{kArm64Spec}}, rep);
    EXPECT_NE(rc, 0) << "300 does not fit the one byte `.byte` emits";
    EXPECT_TRUE(allDiagnosticText(rep).find("does not fit")
                != std::string::npos)
        << allDiagnosticText(rep);
}
