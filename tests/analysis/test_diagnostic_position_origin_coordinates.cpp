// ★★★ [[D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED]] — THE PINS THAT A
// DIAGNOSTIC'S POSITION IS THE POSITION IN A FILE THE USER CAN OPEN.
//
// THE DEFECT, in the shape it actually shipped. When a C file goes through the
// preprocessor, the tree that comes out is parsed from a SYNTHESIZED buffer:
// the built-in predefine prologue, then one line per `--define`, then every
// spliced header's text, then the main file. Its offsets belong to no file on
// disk. `UnitBuilder` converts the PARSE tier's diagnostics off it at parse
// time — and then the builder is destroyed, taking the line-map remap with it.
// Every LATER tier (semantic, HIR, MIR, LIR, the assembly engine) positions its
// own diagnostics from the SAME trees and so re-mints the same synthesized
// coordinates, with nothing left to convert them.
//
// ★ AND IT WAS INVISIBLE, WHICH IS WHY IT LASTED. The synth buffer is
// constructed with the MAIN SOURCE'S NAME, so the render shows a plausible file
// and a plausible line of text — just not the line the construct is on.
// ✔MEASURED through the shipped CLI at the commit these pins landed on, one
// variable per arm, an undeclared identifier at source line 2 column 13:
//   x86_64:elf64-x86_64-linux-exec                        -> 2:13  (correct)
//   arm64:elf64-aarch64-linux-exec                        -> 2:13  (correct)
//   x86_64:pe64-x86_64-windows-exec                       -> 4:13  (+2)
//   x86_64:elf64-x86_64-linux-exec  --define A --define B -> 4:13  (+2)
//   x86_64:pe64-x86_64-windows-exec --define A --define B -> 6:13  (+4)
// The COLUMN is right in every arm; the shift is exactly (that target's
// predefine prologue) + (one line per `--define`). ⚠ ON AN ELF LEG WITH NO
// `--define`s THE SHIFT IS ZERO — a probe run only there reports this defect as
// ABSENT, which is how it survived. That is why the fixtures below FORCE a
// prologue instead of trusting a default.
//
// ★ WHY THE ASSERTIONS DO NOT LIVE IN `tests/analysis/test_diagnostic_corpus`.
// That harness builds its CU through `UnitBuilder::addInMemory` with NO target
// and NO `--define`s, so no prologue is ever synthesized and it renders RAW
// SOURCE coordinates BY CONSTRUCTION. It was green throughout the whole life of
// this defect and would have stayed green. A position pin has to assert through
// a surface that HAS a prologue, or it asserts nothing.
//
// ── WHAT EACH GROUP PINS, AND WHICH MECHANISM IT IS RED FOR ────────────────
// The fix has two application sites, deliberately: `analyze`'s single exit
// (so every caller of the semantic tier is covered — the compile driver, the
// LSP, the FFI header parser) and a destructor in the driver's per-target
// compile (so the tiers BELOW semantic, which have no single exit, are covered
// too). Two overlapping sites means neither is individually red through the
// CLI, so each group below is deliberately routed at the surface where only ONE
// of them is in play:
//   * `SemanticTier*` — `UnitBuilder` + `analyze` DIRECTLY, no driver. Red iff
//     `analyze`'s `remapPreprocessedPositions` call is removed.
//   * `DriverPerTargetCompile*` — the real driver, asserting an ASM-tier
//     diagnostic, which `analyze` never touches. Red iff the destructor in
//     `compileOneTarget` is removed.
//   * `SemanticTierRelatedLocation*` — a `note:` location, which
//     `DiagnosticReporter::remapBuffers` used to skip entirely. Red iff that
//     loop stops visiting `related`.
//
// ── WHY THE CODE ASSERTIONS BELOW DERIVE THEIR SPELLING ────────────────────
//   D-DIAG-TWO-CODE-RENDERINGS (cycle P42)
//
// Two arms here asserted the SHORT HEX literal `"A0008"` against captured CLI
// output. When every render surface was unified on `diagnosticCodeName`, the
// CLI stopped emitting that spelling and both arms went RED. They were right in
// INTENT and stale in SPELLING, so they now derive the token from the
// enumerator (`diagnosticCodeName(DiagnosticCode::A_AsmTextUnsupported)`) and
// cannot go stale again.
//
// ★★ THE LESSON IS THE OPPOSITE OF THE USUAL INSTINCT, AND IT IS WORTH THE
// PARAGRAPH. A hardcoded code inside `contains(...)` goes RED and ANNOUNCES
// itself the moment the rendering changes. A test that instead EXTRACTED the
// code with an `error\[[A-Z0-9]+\]`-shaped regex would have gone silently blank
// and PASSED — matching nothing, asserting nothing, reporting success. Both
// shapes exist in this tree; only one tells you it is wrong. ⇒ when you must
// pin compiler output, prefer a literal you will be forced to update over an
// extraction that can quietly stop matching — or, better still, derive it from
// the enum as these two arms now do, which is loud AND cannot rot.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "program/program.hpp"

#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dss;
namespace fs = std::filesystem;

// One scratch root per PROCESS, claimed atomically — the same scheme
// `tests/analysis/preprocess/test_preprocessor.cpp` uses, and for the same
// reason: ctest runs test binaries concurrently, so a CONSTANT temp path is two
// processes writing each other's fixtures.
[[nodiscard]] fs::path const& scratchRoot() {
    static test_support::ScratchDir const root{test_support::Location::Temp,
                                               "diag_position_origin"};
    return root.path();
}

// Shared schema fixture: loaded once, handed back BY REFERENCE to a cached
// owner. Returning the `shared_ptr` by value would let
// `helper()->accessor()` bind a reference into a schema owned only by the
// temporary (D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE).
[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cSubset() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded.has_value()) {
            // THROW, never `std::abort()`: abort kills the whole test
            // BINARY, so every sibling test in this executable loses its
            // verdict and the harness cannot say which unit failed.
            // GoogleTest reports a throw as a failure of this ONE test.
            // The rule and its measurement live in
            // tests/test_support/repo_root.hpp; it is machine-checked by
            // check-no-abort-in-tests, whose ratchet this had breached.
            throw std::runtime_error{"loadShipped(c) failed"};
        }
        return *loaded;
    }();
    return schema;
}

// A fixture directory unique to one test, so a failure leaves its inputs behind
// under a name that says which test wrote them.
[[nodiscard]] fs::path makeCaseDir(std::string_view caseName) {
    auto dir = scratchRoot() / caseName;
    fs::create_directories(dir);
    return dir;
}

void writeFile(fs::path const& p, std::string_view text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

// The diagnostic BufferRegistry EXACTLY as the driver assembles it: every
// tree's own source PLUS the CU's auxiliary (preprocessor origin) buffers. A
// remapped diagnostic points at one of the latter, so a registry built from the
// trees alone renders it as `<unknown-buffer:N>`.
[[nodiscard]] BufferRegistry driverRegistryFor(CompilationUnit const& cu) {
    BufferRegistry bufs;
    for (auto const& tree : cu.trees()) {
        if (auto src = tree.sourceShared()) bufs.add(std::move(src));
    }
    for (auto const& b : cu.auxiliaryBuffers()) {
        if (b) bufs.add(b);
    }
    return bufs;
}

[[nodiscard]] ParseDiagnostic const* firstWithCode(
        std::span<ParseDiagnostic const> all, DiagnosticCode code) {
    for (auto const& d : all) {
        if (d.code == code) return &d;
    }
    return nullptr;
}

// The name of the buffer a position points into — the half of the defect that
// the rendering could never show, because the synthesized buffer is named after
// the main source. `<unregistered>` rather than a throw so a failing assertion
// prints something diagnosable.
[[nodiscard]] std::string bufferNameOf(BufferRegistry const& bufs, BufferId id) {
    if (auto buf = bufs.tryGet(id)) return std::string{buf->name()};
    return "<unregistered>";
}

[[nodiscard]] bool endsWith(std::string_view hay, std::string_view needle) {
    return hay.size() >= needle.size()
           && hay.compare(hay.size() - needle.size(), needle.size(), needle) == 0;
}

[[nodiscard]] bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

// The rendered form of ONE position — `<file>:<line>:<column>` — composed from
// named parts.
//
// ★★ WHY COMPOSED AND NOT SPELLED OUT AS A LITERAL. Written out, every
// expectation in this file reads to `scripts/check-plan-citations` as a
// POSITIONAL CITATION, and that guard is right to be blunt about the shape: a
// `path:line` written into a source file is a claim about a file that nothing
// rechecks, and it stays plausible after it becomes wrong. These are the exact
// opposite — EXPECTED COMPILER OUTPUT about a fixture this test WRITES a few
// statements earlier and rechecks on every run — but the guard cannot tell the
// two apart from the text, and a guard that learns to make exceptions for
// "obviously fine" cases is a guard nobody reads. So the file carries no such
// literal, and each number arrives through a constant that says what it IS
// (which line the construct is on; how many lines the prologue adds). The
// assertions are unchanged in strength — and the WRONG-arm expectations now
// DERIVE from the prologue line count instead of restating a magic number, so
// they state the mechanism rather than a symptom of it.
[[nodiscard]] std::string positionOf(std::string_view file, int line, int column) {
    return std::format("{}:{}:{}", file, line, column);
}

// Capture everything the driver writes to stderr for the lifetime of the
// object. The CLI renders positioned diagnostics through `std::cerr`, so this
// is the operator-visible text and not a re-derivation of it.
class CapturedStderr {
public:
    CapturedStderr() : saved_{std::cerr.rdbuf(sink_.rdbuf())} {}
    CapturedStderr(CapturedStderr const&)            = delete;
    CapturedStderr& operator=(CapturedStderr const&) = delete;
    ~CapturedStderr() { std::cerr.rdbuf(saved_); }
    [[nodiscard]] std::string text() const { return sink_.str(); }

private:
    std::ostringstream sink_;
    std::streambuf*    saved_;
};

// ── GROUP 1: the SEMANTIC tier, driven directly (no driver) ────────────────

// A synthetic prologue of TWO lines (one per `--define`) in front of a file
// whose semantic error is on line 2. Before the fix this reported line 4 — the
// arm the row measured, reproduced here at the tier that produces it.
TEST(SemanticTierPositionRemap, UnderUserDefinesReportsTheOriginalSourceLine) {
    auto const dir  = makeCaseDir("semantic_user_defines");
    auto const main = dir / "main.c";
    // `zzz` is at the coordinates named below. NO `#include`, NO macro of its
    // own — the shift cannot be blamed on this file's own preprocessing.
    writeFile(main, "int main(void) {\n    int y = zzz;\n    return y;\n}\n");
    constexpr int kUndeclaredLine   = 2;
    constexpr int kUndeclaredColumn = 13;
    // The preprocessor emits ONE prologue line per `--define`, and the shift the
    // defect produced was exactly that count. Deriving the wrong answer from it
    // is what makes the negative assertion a statement about the MECHANISM.
    constexpr int kPrologueLines = 2;

    UnitBuilder builder{cSubset(), DiagnosticBudget::libraryDefault()};
    builder.setUserDefines({"AAA=1", "BBB=2"});
    builder.addFile(main);
    auto cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());

    auto const model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const bufs  = driverRegistryFor(*cu);

    auto const* d = firstWithCode(model.diagnostics().all(),
                                  DiagnosticCode::S_UndeclaredIdentifier);
    ASSERT_NE(d, nullptr) << "the fixture must produce S_UndeclaredIdentifier";

    std::string const rendered = model.diagnostics().format(*d, bufs);
    EXPECT_TRUE(contains(rendered, positionOf("main.c", kUndeclaredLine,
                                              kUndeclaredColumn)))
        << "a semantic diagnostic must report the line in the ORIGINAL source, "
           "not the line in the synthesized preprocessor buffer (two "
           "`--define`s put a two-line prologue in front of it)\n"
        << rendered;
    EXPECT_FALSE(contains(rendered,
                          positionOf("main.c", kUndeclaredLine + kPrologueLines,
                                     kUndeclaredColumn)))
        << "the source line PLUS the prologue is the SYNTHESIZED coordinate — "
           "the exact wrong answer this pin exists to catch, and the one that "
           "renders plausibly\n"
        << rendered;
}

// The other shape the row names: an error INSIDE an `#include`d header. The
// file NAME is wrong here, and the rendering cannot show it — the synth buffer
// carries the main source's name, so the header's text renders under main.c.
TEST(SemanticTierPositionRemap, InsideAnIncludedHeaderNamesTheHeader) {
    auto const dir  = makeCaseDir("semantic_included_header");
    auto const hdr  = dir / "bad.h";
    auto const main = dir / "main.c";
    writeFile(hdr,  "static int hdrbad(void) { return qqq; }\n");
    writeFile(main, "#include \"bad.h\"\nint main(void) { return hdrbad(); }\n");
    // Where `qqq` sits IN THE HEADER. gcc 13.3 reports the same pair for this
    // fixture, which is the reference this expectation is taken from.
    constexpr int kUndeclaredLine   = 1;
    constexpr int kUndeclaredColumn = 34;

    UnitBuilder builder{cSubset(), DiagnosticBudget::libraryDefault()};
    builder.addFile(main);
    auto cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());

    auto const model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const bufs  = driverRegistryFor(*cu);

    auto const* d = firstWithCode(model.diagnostics().all(),
                                  DiagnosticCode::S_UndeclaredIdentifier);
    ASSERT_NE(d, nullptr) << "the fixture must produce S_UndeclaredIdentifier";

    // The BUFFER, not the rendering — this is the half rendering cannot show.
    std::string const name = bufferNameOf(bufs, d->buffer);
    EXPECT_TRUE(endsWith(name, "bad.h"))
        << "a semantic error inside an included header must be attributed to "
           "the HEADER's buffer; got '" << name << "'";
    EXPECT_FALSE(endsWith(name, "main.c"))
        << "attributing it to the main file is the silent mis-direction this "
           "pin exists to catch — the header's TEXT then renders under the "
           "main file's NAME";

    std::string const rendered = model.diagnostics().format(*d, bufs);
    EXPECT_TRUE(contains(rendered, positionOf("bad.h", kUndeclaredLine,
                                              kUndeclaredColumn)))
        << rendered;
    EXPECT_FALSE(contains(rendered, "<unknown-buffer"))
        << "the header origin buffer must be reachable through the registry\n"
        << rendered;
}

// A `note:` is a (buffer, span) of the SAME diagnostic and lives in the SAME
// coordinate system as the primary. `DiagnosticReporter::remapBuffers` used to
// rewrite only the primary, so a remapped diagnostic dragged its note along
// unconverted — and the note is the location that names the OTHER file.
TEST(SemanticTierRelatedLocationRemap, InsideAnIncludedHeaderNamesTheHeader) {
    auto const dir  = makeCaseDir("semantic_related_header");
    auto const hdr  = dir / "prior.h";
    auto const main = dir / "main.c";
    writeFile(hdr,  "static int dupv = 1;\n");
    writeFile(main, "#include \"prior.h\"\nstatic int dupv = 2;\n"
                    "int main(void){ return dupv; }\n");
    // The SECOND declaration (the primary) in the main file, and the FIRST
    // (the note) in the header. Same column in both by construction, so the
    // discriminating fact is the FILE — which is the half that was wrong.
    constexpr int kRedeclaredColumn = 12;
    constexpr int kPrimaryLine      = 2;   // in main.c, AFTER the include splice
    constexpr int kNoteLine         = 1;   // in prior.h

    UnitBuilder builder{cSubset(), DiagnosticBudget::libraryDefault()};
    builder.addFile(main);
    auto cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());

    auto const model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const bufs  = driverRegistryFor(*cu);

    auto const* d = firstWithCode(model.diagnostics().all(),
                                  DiagnosticCode::S_RedeclaredSymbol);
    ASSERT_NE(d, nullptr) << "the fixture must produce S_RedeclaredSymbol";
    ASSERT_FALSE(d->related.empty())
        << "the fixture must carry a 'previously declared here' note — without "
           "one this test asserts nothing about related locations";

    // The primary is the SECOND declaration, in the main file on line 2 (the
    // leading `#include` splice must not drift it).
    EXPECT_TRUE(endsWith(bufferNameOf(bufs, d->buffer), "main.c"))
        << "primary buffer: " << bufferNameOf(bufs, d->buffer);
    // The note is the FIRST declaration, in the header.
    std::string const noteName = bufferNameOf(bufs, d->related.front().buffer);
    EXPECT_TRUE(endsWith(noteName, "prior.h"))
        << "a related location whose declaration lives in an included header "
           "must be attributed to the HEADER; got '" << noteName << "'";

    std::string const rendered = model.diagnostics().format(*d, bufs);
    EXPECT_TRUE(contains(rendered, positionOf("main.c", kPrimaryLine,
                                              kRedeclaredColumn)))
        << rendered;
    EXPECT_TRUE(contains(rendered, positionOf("prior.h", kNoteLine,
                                              kRedeclaredColumn)))
        << rendered;
}

// The DISCRIMINATOR behind the refusal, asserted directly rather than inferred
// from a rendering: before the conversion a tree-derived position names a
// synthesized buffer; after it, it does not. `remapPreprocessedPosition` refuses
// (release-fatal) rather than return with the first state still true, so this is
// also the statement of what that refusal tests for.
TEST(CompilationUnitPositionRemap, SynthesizedBufferDiscriminatorFlipsAcrossTheConversion) {
    auto const dir  = makeCaseDir("cu_synth_discriminator");
    auto const hdr  = dir / "inc.h";
    auto const main = dir / "main.c";
    writeFile(hdr,  "int fromheader(void);\n");
    writeFile(main, "#include \"inc.h\"\nint main(void){ return fromheader(); }\n");

    UnitBuilder builder{cSubset(), DiagnosticBudget::libraryDefault()};
    builder.addFile(main);
    auto const cu = std::move(builder).finish();
    ASSERT_EQ(cu.trees().size(), 1u);

    Tree const& tree = cu.trees()[0];
    BufferId    buffer = tree.source().id();
    SourceSpan  span   = tree.span(tree.root());

    EXPECT_TRUE(cu.isSynthesizedPreprocessorBuffer(buffer))
        << "a preprocessed tree's own source IS a synthesized buffer — if this "
           "is false the rest of the test asserts nothing";

    cu.remapPreprocessedPosition(buffer, span);

    EXPECT_FALSE(cu.isSynthesizedPreprocessorBuffer(buffer))
        << "after the conversion the position must name an ORIGIN file, never "
           "the synthesized buffer";
}

// A CU whose language declares NO preprocess block has no synthesized buffer at
// all, so the conversion must be a strict identity — not "close enough". This
// is the no-op arm that keeps the mechanism from quietly rewriting positions it
// has no business touching.
TEST(CompilationUnitPositionRemap, NonPreprocessedCompilationUnitIsAStrictIdentity) {
    auto const dir  = makeCaseDir("cu_no_preprocess");
    auto const main = dir / "main.c";
    writeFile(main, "int main(void) {\n    int y = zzz;\n    return y;\n}\n");

    UnitBuilder builder{cSubset(), DiagnosticBudget::libraryDefault()};
    builder.addFile(main);
    auto const cu = std::move(builder).finish();
    ASSERT_EQ(cu.trees().size(), 1u);

    // A buffer this CU has never seen — the shape any mid-compile fragment
    // (an embedded assembly template, say) presents.
    auto foreign = SourceBuffer::fromString("int elsewhere;\n", "elsewhere.c");
    BufferId   buffer = foreign->id();
    SourceSpan span   = SourceSpan::of(4, 13);

    EXPECT_FALSE(cu.isSynthesizedPreprocessorBuffer(buffer));
    cu.remapPreprocessedPosition(buffer, span);
    EXPECT_EQ(buffer, foreign->id())
        << "a position in a buffer this CU does not own must be left alone";
    EXPECT_EQ(span.start(), 4u);
    EXPECT_EQ(span.end(), 13u);
}

// ── GROUP 2: the tiers BELOW semantic, driven through the real driver ──────

// An ASM-tier refusal (`A_*`) is produced long after `analyze` has returned, by
// code that positions itself from the same tree — so it re-mints synthesized
// coordinates on its own account. ✔MEASURED before the fix: `--define`s shifted
// it exactly as they shifted `S0001`. A semantic-only fix would have left this
// wrong, which is why it is pinned separately.
TEST(DriverPerTargetCompilePositionRemap, AsmTierDiagnosticUnderUserDefinesReportsTheOriginalLine) {
    auto const dir  = makeCaseDir("driver_asm_user_defines");
    auto const main = dir / "main.c";
    writeFile(main, "int main(void) {\n    __asm__(\"nosuchmnemonic_zz\");\n"
                    "    return 0;\n}\n");
    // Where the unknown mnemonic's statement sits, and the prologue the two
    // `--define`s put in front of it.
    constexpr int kAsmLine       = 2;
    constexpr int kAsmColumn     = 5;
    constexpr int kPrologueLines = 2;

    Program prog;
    prog.setOutputDir(dir / "out");
    prog.setUserDefines({"AAA=1", "BBB=2"});

    std::string captured;
    {
        CapturedStderr const cap;
        DiagnosticReporter rep{DiagnosticBudget::libraryDefault().asConfig()};
        (void)prog.compileFiles({main.string()}, "c",
                                {"x86_64:elf64-x86_64-linux-exec"}, rep);
        captured = cap.text();
    }

    ASSERT_TRUE(contains(captured,
                         diagnosticCodeName(
                             DiagnosticCode::A_AsmTextUnsupported)))
        << "the fixture must reach the assembly engine's unknown-mnemonic "
           "refusal — otherwise this asserts nothing about the tiers below "
           "semantic\n"
        << captured;
    EXPECT_TRUE(contains(captured, positionOf("main.c", kAsmLine, kAsmColumn)))
        << "an ASM-tier diagnostic must report the ORIGINAL source line\n"
        << captured;
    EXPECT_FALSE(contains(captured, positionOf("main.c",
                                               kAsmLine + kPrologueLines,
                                               kAsmColumn)))
        << "the source line PLUS the prologue is the SYNTHESIZED coordinate\n"
        << captured;
}

// The file-name half, at the same tier: an ASM-tier refusal raised inside an
// `#include`d header must name the HEADER.
TEST(DriverPerTargetCompilePositionRemap, AsmTierDiagnosticInsideAnIncludedHeaderNamesTheHeader) {
    auto const dir  = makeCaseDir("driver_asm_included_header");
    auto const hdr  = dir / "asmbad.h";
    auto const main = dir / "main.c";
    writeFile(hdr,  "static void hz(void) { __asm__(\"nosuchmnemonic_zz\"); }\n");
    writeFile(main, "#include \"asmbad.h\"\nint main(void) { hz(); return 0; }\n");
    // Where the `__asm__` statement sits IN THE HEADER. The header is spliced
    // at the very top, so this is also its post-splice synth line — which is
    // exactly why only the FILE NAME discriminates here.
    constexpr int kAsmLine   = 1;
    constexpr int kAsmColumn = 24;

    Program prog;
    prog.setOutputDir(dir / "out");

    std::string captured;
    {
        CapturedStderr const cap;
        DiagnosticReporter rep{DiagnosticBudget::libraryDefault().asConfig()};
        (void)prog.compileFiles({main.string()}, "c",
                                {"x86_64:elf64-x86_64-linux-exec"}, rep);
        captured = cap.text();
    }

    ASSERT_TRUE(contains(captured,
                         diagnosticCodeName(
                             DiagnosticCode::A_AsmTextUnsupported)))
        << "the fixture must reach the assembly engine's unknown-mnemonic "
           "refusal\n"
        << captured;
    EXPECT_TRUE(contains(captured, positionOf("asmbad.h", kAsmLine, kAsmColumn)))
        << "an ASM-tier refusal raised inside an included header must name the "
           "HEADER and its line\n"
        << captured;
    EXPECT_FALSE(contains(captured, positionOf("main.c", kAsmLine, kAsmColumn)))
        << "naming the MAIN file at the header's line is the silent "
           "mis-direction this pin exists to catch\n"
        << captured;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════
// D-DIAG-LINE-NUMBERS-ARE-POST-EXPANSION-WHILE-THE-FILE-NAME-IS-ORIGINAL
// (P36, Lane S) — THE CUMULATIVE-SPLICE-DRIFT PIN.
//
// ★ WHY THIS EXISTS ALONGSIDE THE PINS ABOVE RATHER THAN INSTEAD OF THEM. That
// row was closed by MEASUREMENT: its witness does not reproduce at HEAD,
// because P28 (D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED) had already fixed
// the mechanism six days after the row was filed. But every pin above uses a
// ONE- OR TWO-LINE header, and the defect that row described is CUMULATIVE:
// each `#include` spliced above a position shifts that position further, the
// row's own text records that "early lines still align", and its witness was a
// 39,578-line file reporting line 56,763. A fixture whose splice is two lines
// deep cannot distinguish a correct remap from an absent one.
//
// So this pin supplies the missing dimension — a splice ~3,600 lines deep —
// and asserts BOTH halves of the pair the row is named for: the line number
// AND the file name, together, because the row's whole point is that each was
// individually plausible while the pair was incoherent.
// ═════════════════════════════════════════════════════════════════════════
namespace {

// N harmless declarations, so the splice is large enough for drift to be
// unmistakable if it ever returns. Guarded so re-inclusion is a no-op.
std::string bigHeader(int index, int lines) {
    std::string s = "#ifndef BIG_" + std::to_string(index) + "_H\n#define BIG_"
                  + std::to_string(index) + "_H\n";
    for (int i = 0; i < lines; ++i) {
        s += "int big" + std::to_string(index) + "_decl_" + std::to_string(i)
           + "(void);\n";
    }
    s += "#endif\n";
    return s;
}

} // namespace

TEST(SemanticTierPositionRemap, SurvivesAThousandsOfLinesDeepSplice) {
    auto const dir = makeCaseDir("semantic_deep_splice");
    constexpr int kHeaders        = 3;
    constexpr int kLinesPerHeader = 1200;
    for (int h = 0; h < kHeaders; ++h) {
        writeFile(dir / ("big" + std::to_string(h) + ".h"),
                  bigHeader(h, kLinesPerHeader));
    }
    auto const main = dir / "main.c";
    // Three big includes, a blank line, then the error on a KNOWN line.
    writeFile(main,
              "#include \"big0.h\"\n"      // 1
              "#include \"big1.h\"\n"      // 2
              "#include \"big2.h\"\n"      // 3
              "\n"                          // 4
              "int main(void) {\n"          // 5
              "    return zzz_undeclared;\n" // 6  <-- THE ERROR
              "}\n");                        // 7
    constexpr int kUndeclaredLine   = 6;
    constexpr int kUndeclaredColumn = 12;

    UnitBuilder builder{cSubset(), DiagnosticBudget::libraryDefault()};
    builder.addFile(main);
    auto cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());

    auto const model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const bufs  = driverRegistryFor(*cu);

    auto const* d = firstWithCode(model.diagnostics().all(),
                                  DiagnosticCode::S_UndeclaredIdentifier);
    ASSERT_NE(d, nullptr) << "the fixture must produce S_UndeclaredIdentifier";

    std::string const rendered = model.diagnostics().format(*d, bufs);

    // (a) THE LINE. Under the defect this would be ~3,618 — the true line plus
    //     everything spliced above it. Nothing about that number looks wrong
    //     unless you count the lines in the file it names, which is exactly why
    //     the defect survived: `main.c` has 7 lines, so a report of 3,618 is
    //     past EOF and STILL renders a plausible-looking diagnostic.
    EXPECT_TRUE(contains(rendered, positionOf("main.c", kUndeclaredLine,
                                              kUndeclaredColumn)))
        << "a position " << (kHeaders * (kLinesPerHeader + 3))
        << "+ spliced lines deep must still resolve to the ORIGINAL line\n"
        << rendered;

    // (b) THE PAIR. The row is named for a line number in post-expansion
    //     coordinates paired with the ORIGINAL file's name. Asserting the line
    //     alone would pass if the name silently became the synth buffer's, so
    //     both halves are pinned together.
    EXPECT_TRUE(endsWith(bufferNameOf(bufs, d->buffer), "main.c"))
        << "the coordinates are the main file's, so the NAME must be too";
    EXPECT_FALSE(contains(rendered, "<unknown-buffer"))
        << rendered;
}

TEST(SemanticTierPositionRemap, DeepSpliceStillNamesAnIncludedHeadersOwnLine) {
    // The other half, and the DECISIVE arm for that row: an error INSIDE a
    // header that itself sits thousands of spliced lines deep. Both the name
    // and the line must be the HEADER's — a self-consistent pair, which the row
    // says never happens once includes accumulate.
    auto const dir = makeCaseDir("semantic_deep_splice_header");
    constexpr int kHeaders        = 3;
    constexpr int kLinesPerHeader = 1200;
    for (int h = 0; h < kHeaders; ++h) {
        writeFile(dir / ("big" + std::to_string(h) + ".h"),
                  bigHeader(h, kLinesPerHeader));
    }
    writeFile(dir / "bad.h",
              "#ifndef BAD_H\n#define BAD_H\n"
              "static int hdrbad(void) { return qqq; }\n"   // line 3
              "#endif\n");
    auto const main = dir / "main.c";
    writeFile(main,
              "#include \"big0.h\"\n"
              "#include \"big1.h\"\n"
              "#include \"big2.h\"\n"
              "#include \"bad.h\"\n"
              "int main(void) { return hdrbad(); }\n");
    constexpr int kUndeclaredLine   = 3;
    constexpr int kUndeclaredColumn = 34;

    UnitBuilder builder{cSubset(), DiagnosticBudget::libraryDefault()};
    builder.addFile(main);
    auto cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());

    auto const model = analyze(cu, DiagnosticBudget::libraryDefault());
    auto const bufs  = driverRegistryFor(*cu);

    auto const* d = firstWithCode(model.diagnostics().all(),
                                  DiagnosticCode::S_UndeclaredIdentifier);
    ASSERT_NE(d, nullptr) << "the fixture must produce S_UndeclaredIdentifier";

    std::string const name = bufferNameOf(bufs, d->buffer);
    EXPECT_TRUE(endsWith(name, "bad.h"))
        << "got '" << name << "'";
    std::string const rendered = model.diagnostics().format(*d, bufs);
    EXPECT_TRUE(contains(rendered, positionOf("bad.h", kUndeclaredLine,
                                              kUndeclaredColumn)))
        << "the header's OWN line, not a synth offset, even 3,600 lines deep\n"
        << rendered;
}
