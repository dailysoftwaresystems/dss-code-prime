// ★★★ [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]] — A DIAGNOSTIC WHOSE SUBJECT IS
// A MACRO-EXPANSION PRODUCT TOKEN MUST POINT AT THE EXPANSION SITE.
//
// ── THE DEFECT, IN THE SHAPE IT SHIPPED ────────────────────────────────────
//
// The preprocessor MINTS the text of a `#` / `##` / predefined-macro / `#embed`
// product: the spelling is appended AFTER the spliced prefix and the product
// token's span points into that tail, which by construction lies past the end of
// every line-map segment. `PreprocessResult::makeRemap` then took the LAST
// segment and computed `originStart + (offset - synthStart)` — an offset past the
// ORIGIN buffer's end (✔MEASURED at the row's creation: 71 through 117 against a
// 50-byte origin).
//
// ★★ A CLAMP MADE IT SAFE BUT NOT CORRECT, AND THAT DISTINCTION IS WHY THIS FILE
// EXISTS. [[D-DIAG-RENDERER-PAST-END-SPAN-HEAP-OVERREAD]] clamped the CONSUMER
// (`extractLine`), which removed a heap over-read and a non-deterministic abort.
// It did NOT repair the POSITION: the diagnostic still rendered at END OF FILE,
// which looks like a real location and is not one. A clamp at the consumer cannot
// repair an offset the producer computed wrong — so every assertion below is
// about WHERE the diagnostic points, never about whether it renders.
//
// ── WHAT THE RIGHT ANSWER IS, AND WHERE IT COMES FROM ──────────────────────
//
// ✔MEASURED, gcc 13.3.0 and clang 18.1.3 probed SEPARATELY on
//   #define STR(x) #x
//   #define CAT(a, b) a##b
//   int STR(name) = 1;
//   int CAT(0, x1) = 2;
// clang puts the ERROR on the invocation line at the `STR` token's own column
// (one `^` wide) and adds `note: expanded from macro 'STR'` on the `#define`
// line, at the `#` of the `#x` operator. gcc puts the error on the invocation
// line too — at the argument rather than the macro name — and adds the sibling
// `note: in definition of macro 'STR'` on the `#define` line.
// Neither invents a text offset for the minted bytes and neither points at
// end-of-file; both give the reader the macro's NAME and a second location. DSS
// takes clang's shape — primary at the EXPANSION SITE, `note: expanded from macro
// 'X'` at the `#define` — which is also what the row prescribes in words.
// (Both renderings are described in prose, never transcribed: a `file:line:col`
//  in a source file reads to `scripts/check-plan-citations` as a positional
//  citation, and it is right to be blunt about that shape.)
//
// ── WHY THE ASSERTIONS ARE COMPOSED AND NOT SPELLED OUT ────────────────────
// Same reason as `tests/analysis/test_diagnostic_position_origin_coordinates`: a
// `path:line` literal in a source file reads to `scripts/check-plan-citations` as
// a positional citation, and a guard that learns exceptions is a guard nobody
// reads. Every number below arrives through a named constant that says what it IS
// (which line the invocation is on; which line the `#define` is on) about a
// fixture this file writes a few statements earlier.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/line_map.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "core/types/tree.hpp"

#include "test_support/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dss;
namespace fs = std::filesystem;

// One scratch root per PROCESS, claimed atomically — the scheme every fixture
// file in this directory uses, and for the same reason: ctest runs test binaries
// concurrently, so a CONSTANT temp path is two processes writing each other's
// fixtures.
[[nodiscard]] fs::path const& scratchRoot() {
    static test_support::ScratchDir const root{test_support::Location::Temp,
                                               "pp_macro_expansion_position"};
    return root.path();
}

// Shared schema fixture: loaded once, handed back BY REFERENCE to a cached owner.
// Returning the `shared_ptr` by value would let `helper()->accessor()` bind a
// reference into a schema owned only by the temporary
// (D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE).
[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded.has_value()) {
            // THROW, never `std::abort()`: abort kills the whole test BINARY, so
            // every sibling test loses its verdict (machine-checked by
            // scripts/check-no-abort-in-tests).
            throw std::runtime_error{"loadShipped(c) failed"};
        }
        return *loaded;
    }();
    return schema;
}

[[nodiscard]] fs::path makeCaseDir(std::string_view caseName) {
    auto dir = scratchRoot() / caseName;
    fs::create_directories(dir);
    return dir;
}

void writeFile(fs::path const& p, std::string_view text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

// The diagnostic BufferRegistry EXACTLY as the driver assembles it: every tree's
// own source PLUS the CU's auxiliary (preprocessor origin) buffers. A remapped
// diagnostic points at one of the latter, so a registry built from the trees
// alone renders it as `<unknown-buffer:N>`.
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

[[nodiscard]] bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

// `<file>:<line>:<column>` — composed, never a literal. See the header note.
[[nodiscard]] std::string positionOf(std::string_view file, int line, int column) {
    return std::format("{}:{}:{}", file, line, column);
}

// The compile of one fixture file, held together so the buffers outlive the
// rendering. `text` is the rendered form of the FIRST parse diagnostic, which is
// the operator-visible output and not a re-derivation of it.
struct Compiled {
    std::shared_ptr<CompilationUnit const> cu;
    BufferRegistry                         bufs;
    std::string                            rendered;
    std::size_t                            diagnosticCount = 0;
};

[[nodiscard]] Compiled compileFixture(fs::path const& main) {
    UnitBuilder builder{cLanguage(), DiagnosticBudget::libraryDefault()};
    builder.addFile(main);
    Compiled out;
    out.cu   = std::make_shared<CompilationUnit const>(std::move(builder).finish());
    out.bufs = driverRegistryFor(*out.cu);
    for (Tree const& t : out.cu->trees()) {
        auto const all = t.diagnostics().all();
        out.diagnosticCount += all.size();
        if (out.rendered.empty() && !all.empty()) {
            out.rendered = t.diagnostics().format(all.front(), out.bufs);
        }
    }
    return out;
}

// ── GROUP 1: `LineMap::originOf` — the classification, at the unit ─────────
//
// These build a map by hand rather than running the preprocessor, so each arm is
// pinned in isolation and a failure names the arm rather than a fixture.

// A 3-line origin whose text is spliced VERBATIM into a synth prefix at 0.
struct MapFixture {
    std::shared_ptr<SourceBuffer> origin;
    LineMap                       map;
};

[[nodiscard]] MapFixture makeMapFixture(std::string originText,
                                        ByteOffset  glueBytes,
                                        bool        withProducts) {
    MapFixture f;
    f.origin = SourceBuffer::fromString(std::move(originText), "origin.c");
    LineMapSegment seg;
    seg.synthStart  = 0;
    seg.synthEnd    = static_cast<ByteOffset>(f.origin->size());
    seg.origin      = f.origin;
    seg.originStart = 0;
    f.map.addSegment(std::move(seg));
    // The prefix continues past the segment for `glueBytes` — the synthesized
    // `#define` block a shipped descriptor injects, or the rewritten
    // `#include <h>` line. Neither carries a segment: `copyVerbatim` is
    // segment-driven and those appends bypass it.
    const ByteOffset prefixLen =
        static_cast<ByteOffset>(f.origin->size()) + glueBytes;
    f.map.setProductBase(prefixLen);
    if (withProducts) {
        MacroExpansionSite s;
        s.productStart  = prefixLen;
        s.productEnd    = prefixLen + 3;   // e.g. the paste product `0x1`
        s.siteOffset    = 4;               // an offset INSIDE the origin run
        s.defOffset     = 1;
        s.hasDefinition = true;
        s.name          = "CAT";
        f.map.addExpansion(std::move(s));
    }
    return f;
}

TEST(LineMapOriginOf, ARealByteResolvesDirect) {
    auto f = makeMapFixture("int a;\nint b;\n", /*glueBytes=*/0,
                            /*withProducts=*/false);
    const SynthOrigin r = f.map.originOf(7);
    EXPECT_EQ(r.kind, SynthOriginKind::Direct);
    EXPECT_EQ(r.origin, f.origin.get());
    EXPECT_EQ(r.offset, 7u);
}

// ⚠ THE REFUTATION. `LineMap`'s own comment used to claim glue attribution was
// "never out of bounds". For the ONE-byte newline between two concatenated files
// that was very nearly true; for the MULTI-BYTE glue the splice actually injects
// it was false by the full length of the glue, and this arm is the proof.
TEST(LineMapOriginOf, MultiByteSyntheticPrefixGlueStaysInsideTheOrigin) {
    const std::string text = "int a;\n";
    constexpr ByteOffset kGlue = 64;   // a descriptor's `#define` block
    auto f = makeMapFixture(text, kGlue, /*withProducts=*/false);
    // An offset deep inside the injected glue.
    const ByteOffset probe = static_cast<ByteOffset>(text.size()) + kGlue - 1;
    const SynthOrigin r = f.map.originOf(probe);
    EXPECT_EQ(r.kind, SynthOriginKind::SyntheticGlue);
    ASSERT_NE(r.origin, nullptr);
    EXPECT_LE(r.offset, r.origin->size())
        << "the OLD extrapolation answered originStart + (probe - synthStart), "
           "which is " << probe << " against a " << r.origin->size()
        << "-byte origin — an offset the renderer could only clamp";
    // The INJECTION POINT: just past the last copied byte, i.e. where the
    // synthetic text was spliced in.
    EXPECT_EQ(r.offset, static_cast<ByteOffset>(text.size()));
}

TEST(LineMapOriginOf, AProductByteResolvesToItsExpansionSite) {
    const std::string text = "int a;\nint b;\n";
    auto f = makeMapFixture(text, /*glueBytes=*/0, /*withProducts=*/true);
    const ByteOffset probe = static_cast<ByteOffset>(text.size()) + 1;
    const SynthOrigin r = f.map.originOf(probe);
    EXPECT_EQ(r.kind, SynthOriginKind::Expansion);
    ASSERT_NE(r.expansion, nullptr);
    EXPECT_EQ(r.expansion->name, "CAT");
    // The SITE, not an extrapolation of the product offset.
    EXPECT_EQ(r.offset, 4u);
    EXPECT_LE(r.offset, r.origin->size());
}

TEST(LineMapOriginOf, PastEveryProductIsTheEndOfTheUnit) {
    const std::string text = "int a;\n";
    auto f = makeMapFixture(text, /*glueBytes=*/0, /*withProducts=*/true);
    // The synthetic Eof sits one past the last product byte.
    const ByteOffset probe = static_cast<ByteOffset>(text.size()) + 3;
    const SynthOrigin r = f.map.originOf(probe);
    EXPECT_EQ(r.kind, SynthOriginKind::EndOfUnit);
    ASSERT_NE(r.origin, nullptr);
    EXPECT_EQ(r.offset, static_cast<ByteOffset>(text.size()));
    EXPECT_LE(r.offset, r.origin->size());
}

// ★ THE FAIL-LOUD CHANNEL ITSELF. A segment that does not describe its own
// origin is a compiler bug; the map must NAME it rather than hand back a
// plausible number for `remapOnePosition` to install.
TEST(LineMapOriginOf, ASegmentThatOverrunsItsOriginIsEscapedNotClamped) {
    LineMap map;
    auto origin = SourceBuffer::fromString("short\n", "origin.c");
    LineMapSegment seg;
    seg.synthStart  = 0;
    seg.synthEnd    = 100;   // claims 100 bytes of a 6-byte origin
    seg.origin      = origin;
    seg.originStart = 0;
    map.addSegment(std::move(seg));
    const SynthOrigin r = map.originOf(50);
    EXPECT_EQ(r.kind, SynthOriginKind::Escaped)
        << "an offset that cannot be a position in the buffer it names must come "
           "back as Escaped, never as a clamped-looking number";
}

TEST(LineMapOriginOf, ResolveIsDerivedFromOriginOfAndNeverExtrapolates) {
    const std::string text = "int a;\nint b;\n";
    auto f = makeMapFixture(text, /*glueBytes=*/0, /*withProducts=*/true);
    const ByteOffset probe = static_cast<ByteOffset>(text.size()) + 1;
    const LineMap::Resolved r = f.map.resolve(probe);
    ASSERT_NE(r.origin, nullptr);
    EXPECT_EQ(r.offset, 4u)
        << "resolve() must give the SAME answer originOf() does — one "
           "implementation of the forward direction, so `__LINE__`/`__FILE__` "
           "and a diagnostic can never disagree about where a product byte is";
    EXPECT_LE(r.offset, r.origin->size());
}

// ── GROUP 2: end to end — a parse error whose SUBJECT is a product token ───

// A `#`-stringize product. `int STR(name) = 1;` expands to `int "name" = 1;`,
// so the parse error's subject is the minted string literal.
TEST(MacroExpansionDiagnosticPosition, StringizeProductPointsAtTheInvocation) {
    auto const dir  = makeCaseDir("stringize_product");
    auto const main = dir / "main.c";
    writeFile(main, "#define STR(x) #x\n"
                    "\n"
                    "int STR(name) = 1;\n");
    constexpr int kDefineLine     = 1;
    constexpr int kDefineColumn   = 9;    // the macro NAME on the `#define` line
    constexpr int kInvocationLine = 3;
    constexpr int kInvocationCol  = 5;    // the `STR` token — clang reports 4:5
                                          // for the same shape one line lower
    constexpr int kLastLine       = 3;

    auto const c = compileFixture(main);
    ASSERT_GT(c.diagnosticCount, 0u) << "the fixture must produce a parse error";

    EXPECT_TRUE(contains(c.rendered,
                         positionOf("main.c", kInvocationLine, kInvocationCol)))
        << "a diagnostic whose subject is a macro-expansion PRODUCT token must "
           "report the EXPANSION SITE\n"
        << c.rendered;
    // The exact wrong answer the row names: the producer's extrapolated offset,
    // clamped by the renderer to the end of the file.
    EXPECT_FALSE(contains(c.rendered,
                          positionOf("main.c", kLastLine + 1, 1)))
        << c.rendered;
    EXPECT_FALSE(contains(c.rendered, "<unknown-buffer"))
        << "the origin buffer must be reachable through the registry\n"
        << c.rendered;
    EXPECT_TRUE(contains(c.rendered, "expanded from macro 'STR'"))
        << "the macro must be NAMED — both references do, and a bare position "
           "at the invocation says only 'something in this macro'\n"
        << c.rendered;
    EXPECT_TRUE(contains(c.rendered,
                         positionOf("main.c", kDefineLine, kDefineColumn)))
        << "the note must point at the macro's DEFINITION site\n"
        << c.rendered;
}

// A `##`-paste product. `int CAT(0, x1) = 2;` expands to `int 0x1 = 2;`.
TEST(MacroExpansionDiagnosticPosition, PasteProductPointsAtTheInvocation) {
    auto const dir  = makeCaseDir("paste_product");
    auto const main = dir / "main.c";
    writeFile(main, "#define CAT(a, b) a##b\n"
                    "int CAT(0, x1) = 2;\n");
    constexpr int kDefineLine     = 1;
    constexpr int kDefineColumn   = 9;
    constexpr int kInvocationLine = 2;
    constexpr int kInvocationCol  = 5;
    constexpr int kLastLine       = 2;

    auto const c = compileFixture(main);
    ASSERT_GT(c.diagnosticCount, 0u) << "the fixture must produce a parse error";

    EXPECT_TRUE(contains(c.rendered,
                         positionOf("main.c", kInvocationLine, kInvocationCol)))
        << c.rendered;
    EXPECT_FALSE(contains(c.rendered, positionOf("main.c", kLastLine + 1, 1)))
        << c.rendered;
    EXPECT_TRUE(contains(c.rendered, "expanded from macro 'CAT'")) << c.rendered;
    EXPECT_TRUE(contains(c.rendered,
                         positionOf("main.c", kDefineLine, kDefineColumn)))
        << c.rendered;
}

// ★ A PRODUCT SPAN HAS NO EXTENT IN ANY FILE. The caret is ONE column, matching
// clang exactly. Before this, the exclusive END of the minted run resolved as
// end-of-unit and the caret grew to swallow the rest of the source line.
TEST(MacroExpansionDiagnosticPosition, TheCaretIsAPointNotTheRestOfTheLine) {
    auto const dir  = makeCaseDir("product_caret");
    auto const main = dir / "main.c";
    writeFile(main, "#define CAT(a, b) a##b\n"
                    "int CAT(0, x1) = 2;\n");
    auto const c = compileFixture(main);
    ASSERT_GT(c.diagnosticCount, 0u);
    EXPECT_TRUE(contains(c.rendered, "^\n"))
        << "a one-column caret at the expansion site\n" << c.rendered;
    EXPECT_FALSE(contains(c.rendered, "^^"))
        << "widening a product span underlines source the product did not come "
           "from\n"
        << c.rendered;
}

// A PREDEFINED macro's value is minted through the same chokepoint and has NO
// `#define` line — so the POSITION must still be exact and the note must be
// ABSENT rather than pointing at offset zero.
TEST(MacroExpansionDiagnosticPosition, PredefinedProductHasAPositionAndNoNote) {
    auto const dir  = makeCaseDir("predefined_product");
    auto const main = dir / "main.c";
    writeFile(main, "int __LINE__x = 0;\n"
                    "int __LINE__ = 1;\n");
    constexpr int kInvocationLine = 2;
    constexpr int kInvocationCol  = 5;

    auto const c = compileFixture(main);
    ASSERT_GT(c.diagnosticCount, 0u) << "the fixture must produce a parse error";
    EXPECT_TRUE(contains(c.rendered,
                         positionOf("main.c", kInvocationLine, kInvocationCol)))
        << c.rendered;
    EXPECT_FALSE(contains(c.rendered, "expanded from macro"))
        << "a predefined macro has no definition site to point at, and inventing "
           "one is what `hasDefinition == false` exists to prevent\n"
        << c.rendered;
}

// ★ THE SHAPE THE ROW OBSERVED IN THE FIELD — a product token inside an
// `#include`d header (Apple `<_stdio.h>`). The FILE is the half a rendering
// cannot show on its own: the synth buffer carries the MAIN source's name, so a
// mis-attributed header diagnostic renders under `main.c` and looks fine.
TEST(MacroExpansionDiagnosticPosition, ProductInsideAHeaderNamesTheHeader) {
    auto const dir  = makeCaseDir("product_in_header");
    auto const hdr  = dir / "bad.h";
    auto const main = dir / "main.c";
    writeFile(hdr,  "#define HCAT(a, b) a##b\n"
                    "int HCAT(0, x1) = 3;\n");
    writeFile(main, "#include \"bad.h\"\n"
                    "int main(void) { return 0; }\n");
    constexpr int kInvocationLine = 2;
    constexpr int kInvocationCol  = 5;
    constexpr int kDefineLine     = 1;
    constexpr int kDefineColumn   = 9;

    auto const c = compileFixture(main);
    ASSERT_GT(c.diagnosticCount, 0u) << "the fixture must produce a parse error";
    EXPECT_TRUE(contains(c.rendered,
                         positionOf("bad.h", kInvocationLine, kInvocationCol)))
        << "the expansion site is in the HEADER, so the diagnostic must name the "
           "header and the header's line\n"
        << c.rendered;
    EXPECT_TRUE(contains(c.rendered,
                         positionOf("bad.h", kDefineLine, kDefineColumn)))
        << c.rendered;
    EXPECT_FALSE(contains(c.rendered, "main.c:"))
        << "attributing a header's product token to the main file is the silent "
           "mis-direction this pin exists to catch\n"
        << c.rendered;
}

}  // namespace
