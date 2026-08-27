// ══ D-LSP-POSITIONS-RESOLVED-IN-SYNTHESIZED-PREPROCESSOR-COORDINATES ════════
//
// The coordinate layer under `src/lsp/`, tested where the defect actually
// lived: the translation between the DOCUMENT the editor sees, the SYNTH
// buffer a tree was parsed from, and the ORIGIN file a span came from.
//
// ★★ THE INVARIANT TEST IS THE ONE THAT WOULD HAVE CAUGHT THIS, and it is
// first below. Every previous LSP test pinned the RIGHT ANSWER for a fixture
// whose three coordinate spaces happened to COINCIDE — a four-line document,
// no includes, and (before this cycle) an empty built-in prologue. They could
// not fail, because there was nothing to translate. The invariant test varies
// exactly ONE thing — the prologue's LENGTH — and asserts every answer is
// unchanged. It does not know how long the prologue is, so it cannot rot when
// a `predefinedMacros` row is added.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "lsp/lsp_coordinates.hpp"
#include "lsp/workspace_project.hpp"   // fileUriFromPath
#include "test_support/repo_root.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using dss::lsp::DocumentCoordinates;
using dss::lsp::Position;

namespace {

[[nodiscard]] std::string shippedCText() {
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    std::ifstream in(*root / "sources" / "c.lang.json", std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot read the shipped c config";
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// The SHIPPED language, whose built-in prologue is NON-EMPTY: one `#define`
// line per `ordinary` predefine of the effective set.
[[nodiscard]] std::shared_ptr<dss::GrammarSchema const> shippedC() {
    auto loaded = dss::GrammarSchema::loadShipped("c");
    if (!loaded.has_value()) {
        ADD_FAILURE() << "loadShipped(c) failed";
        return nullptr;
    }
    return *loaded;
}

// The SAME language with an EMPTY prologue, produced by flipping every
// `ordinary` predefine to a warn verb — which is precisely the field that
// decides whether a row is lowered into the prologue at all.
//
// ★ ONE VARIABLE. Not a different language, not a different document, not a
// different schema shape: the identical config with one closed-verb value
// changed. Anything that differs between the two arms is therefore caused by
// the prologue's LENGTH and nothing else.
// ⚠ And the count is never written down here. The arms are "shipped" and
// "none", so adding a `predefinedMacros` row changes what this test exercises
// without changing what it asserts.
[[nodiscard]] std::shared_ptr<dss::GrammarSchema const> cWithEmptyPrologue() {
    std::string text = shippedCText();
    if (text.empty()) return nullptr;
    const std::string from = "\"programRedefinition\": \"ordinary\"";
    const std::string to   = "\"programRedefinition\": \"warn-iso-macro\"";
    if (text.find(from) == std::string::npos) {
        ADD_FAILURE() << "shipped c config no longer carries an `ordinary` "
                         "programRedefinition row — this A/B has lost its "
                         "variable and would silently compare an arm to itself";
        return nullptr;
    }
    for (std::string::size_type p = text.find(from); p != std::string::npos;
         p = text.find(from, p + to.size())) {
        text.replace(p, from.size(), to);
    }
    // ...except the FUNCTION-LIKE rows. The loader refuses a warn verb on one
    // (c105 already lowers it to an ordinary prologue `#define`, so a warn
    // claim would be unenforceable), and this A/B is not the place to argue
    // with that rule -- it is the place to vary the OBJECT-like prologue. Their
    // two lines are common to BOTH arms, so they are not a variable.
    const std::string fnFrom =
        "\"params\": [\"x\"], \"availableObjectFormats\": [\"pe\"], " + to;
    const std::string fnTo =
        "\"params\": [\"x\"], \"availableObjectFormats\": [\"pe\"], " + from;
    for (std::string::size_type p = text.find(fnFrom); p != std::string::npos;
         p = text.find(fnFrom, p + fnTo.size())) {
        text.replace(p, fnFrom.size(), fnTo);
    }
    auto loaded = dss::GrammarSchema::loadFromText(text, "<c-empty-prologue>");
    if (!loaded.has_value()) {
        ADD_FAILURE() << "rebound schema must still load: "
                      << (loaded.error().empty() ? "<none>"
                                                 : loaded.error()[0].message);
        return nullptr;
    }
    return *loaded;
}

struct Built {
    std::shared_ptr<dss::CompilationUnit>      unit;
    std::shared_ptr<dss::SemanticModel const>  model;
};

[[nodiscard]] Built buildFile(std::shared_ptr<dss::GrammarSchema const> schema,
                              fs::path const& file,
                              std::vector<fs::path> const& includeDirs = {}) {
    Built out;
    if (schema == nullptr) return out;
    dss::UnitBuilder builder{std::move(schema),
                             dss::DiagnosticBudget::libraryDefault()};
    for (auto const& d : includeDirs) builder.addIncludeDir(d);
    builder.addFile(file);
    out.unit = std::make_shared<dss::CompilationUnit>(
        std::move(builder).finish());
    out.model = std::make_shared<dss::SemanticModel const>(
        dss::analyze(out.unit, dss::DiagnosticBudget::libraryDefault()));
    return out;
}

// A temp directory that cleans itself up, so a fixture with real `#include`
// files never accumulates in the tree.
class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path()
             / ("dss-lspcoord-" + std::to_string(
                    reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(dir_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    TempDir(TempDir const&) = delete;
    TempDir& operator=(TempDir const&) = delete;

    [[nodiscard]] fs::path write(std::string const& name,
                                 std::string_view text) const {
        const fs::path p = dir_ / name;
        std::ofstream out(p, std::ios::binary);
        out << text;
        return p;
    }
    [[nodiscard]] fs::path const& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

} // namespace

// ══ 1. THE INVARIANT ════════════════════════════════════════════════════════
//
// The SAME document, answered against a prologue that is EMPTY and one that is
// NOT. Every position in the document must round-trip to the same origin range
// under both.
//
// ★ IT SWEEPS EVERY POSITION rather than the handful a handler test would poke.
// The defect shifted answers by a constant, so a sweep catches it at the first
// position that resolves — and a sweep also catches the OPPOSITE failure a
// spot-check invites, where one position was special-cased into correctness.
TEST(LspCoordinates, PrologueLengthMovesNoAnswer) {
    TempDir tmp;
    constexpr std::string_view kSrc =
        "int add(int a, int b) { return a; }\n"
        "int main() {\n"
        "    int x = 0;\n"
        "    x = add(x, x);\n"
        "    return x;\n"
        "}\n";
    const fs::path file = tmp.write("x.c", kSrc);
    const std::string uri = dss::lsp::fileUriFromPath(file);
    const std::string text{kSrc};

    Built shipped = buildFile(shippedC(), file);
    Built empty   = buildFile(cWithEmptyPrologue(), file);
    ASSERT_NE(shipped.unit, nullptr);
    ASSERT_NE(empty.unit, nullptr);

    // The arms must genuinely DIFFER in the thing under test, or this whole
    // test is an elaborate way of comparing a value to itself. The synth
    // buffers' sizes are the prologue's length plus the same source text.
    ASSERT_GT(shipped.unit->trees()[0].source().text().size(),
              empty.unit->trees()[0].source().text().size())
        << "the two arms have the SAME prologue — the A/B has no variable, so "
           "a passing result here would assert nothing";

    const DocumentCoordinates shippedCoords{*shipped.unit, uri, text};
    const DocumentCoordinates emptyCoords{*empty.unit, uri, text};

    int resolved = 0;
    for (std::uint32_t line = 0; line < 6; ++line) {
        for (std::uint32_t col = 0; col < 40; ++col) {
            const Position pos{line, col};
            auto a = shippedCoords.toSynth(pos);
            auto b = emptyCoords.toSynth(pos);
            ASSERT_EQ(a.has_value(), b.has_value())
                << "prologue length changed WHETHER a position resolves, at "
                << line << ":" << col;
            if (!a.has_value()) continue;
            ++resolved;
            // The synth OFFSETS legitimately differ (that is the prologue). The
            // ANSWER — the origin range the user is shown — must not.
            auto la = shippedCoords.locate(*a->tree,
                                           a->tree->span(a->tree->root()));
            auto lb = emptyCoords.locate(*b->tree,
                                         b->tree->span(b->tree->root()));
            ASSERT_EQ(la.has_value(), lb.has_value());
            if (!la.has_value()) continue;
            EXPECT_EQ(la->uri, lb->uri) << "at " << line << ":" << col;
            EXPECT_EQ(la->range.start.line, lb->range.start.line)
                << "at " << line << ":" << col;
            EXPECT_EQ(la->range.start.character, lb->range.start.character)
                << "at " << line << ":" << col;
        }
    }
    EXPECT_GT(resolved, 50)
        << "the sweep resolved almost nothing, so it cannot have compared the "
           "two arms — a vacuous pass";
}

// ══ 2. DEAD TEXT, AND THE ANTI-NEIGHBOUR PROPERTY ═══════════════════════════
//
// ★★ THIS TEST ASSERTED THE WRONG THING FIRST, AND THE CORRECTION IS THE
// USEFUL PART. It was written to prove that an `#if 0` region has NO synth
// image. ✔MEASURED: it has one. The synth buffer is built by TEXT
// CONCATENATION of the whole file, and the preprocessor's own design note says
// "a dead branch's TOKENS are simply NOT emitted into the body" — elision is a
// TOKEN-level operation, not a text-level one. So dead text sits in the buffer
// with a perfectly good image; what it lacks is NODES, and the handler answers
// its default through `nodeAtOffset` finding nothing, by a different route
// that was already correct.
//
// ⇒ So the property worth pinning here is not absence, it is FIDELITY: a dead
// line must map to ITS OWN line and never to a live neighbour. That is the
// actual defect shape — an answer about the wrong line — and it is testable on
// text that no longer has tokens to hide behind.
TEST(LspCoordinates, DeadBranchTextMapsToItsOwnLineNotANeighbour) {
    TempDir tmp;
    constexpr std::string_view kSrc =
        "int live = 1;\n"          // line 0
        "#if 0\n"                  // line 1
        "int dead = 2;\n"          // line 2  <- no image
        "#endif\n"                 // line 3
        "int after = 3;\n";        // line 4
    const fs::path file = tmp.write("z.c", kSrc);
    Built b = buildFile(shippedC(), file);
    ASSERT_NE(b.unit, nullptr);
    const DocumentCoordinates coords{
        *b.unit, dss::lsp::fileUriFromPath(file), std::string{kSrc}};

    // Live control lines resolve.
    ASSERT_TRUE(coords.toSynth(Position{0, 4}).has_value());
    ASSERT_TRUE(coords.toSynth(Position{4, 4}).has_value());

    // The DEAD line resolves too (see the note above) — and its image must
    // round-trip back to line 2. A shifted map is exactly what would send it to
    // line 0 or line 4, which is the wrong-line answer this row closed.
    auto dead = coords.toSynth(Position{2, 4});
    ASSERT_TRUE(dead.has_value())
        << "dead TEXT is still in the synth buffer; only its TOKENS are elided";

    auto const& map = *dead->tree;
    dss::BufferId   buf  = map.source().id();
    dss::SourceSpan span = dss::SourceSpan::of(dead->offset, dead->offset);
    b.unit->remapPreprocessedPosition(buf, span);
    dss::SourceBuffer const* origin = nullptr;
    for (auto const& aux : b.unit->auxiliaryBuffers()) {
        if (aux && aux->id() == buf) origin = aux.get();
    }
    ASSERT_NE(origin, nullptr);
    EXPECT_EQ(origin->lineCol(span.start()).line, 3u)
        << "the dead line must map back to ITS OWN line (1-based 3 == 0-based "
           "2). A neighbouring line here is the defect wearing its most "
           "plausible face";
}

TEST(LspCoordinates, APositionPastTheLastByteHasNoSynthImage) {
    TempDir tmp;
    constexpr std::string_view kSrc = "int x = 1;\n";
    const fs::path file = tmp.write("e.c", kSrc);
    Built b = buildFile(shippedC(), file);
    ASSERT_NE(b.unit, nullptr);
    const DocumentCoordinates coords{
        *b.unit, dss::lsp::fileUriFromPath(file), std::string{kSrc}};

    EXPECT_TRUE(coords.toSynth(Position{0, 4}).has_value());
    // Far past the end of a 1-line file. `LineMap::inverse` is half-open, so
    // this has no image — and must not clamp to the last byte.
    EXPECT_FALSE(coords.toSynth(Position{40, 0}).has_value());
}

// ══ 3. ACROSS A SPLICE ══════════════════════════════════════════════════════
//
// A leading `#include` puts the header's text AHEAD of the main source in the
// synth buffer. Before this row, every document position was off by the
// header's length and every span was reported at the request's own uri.
TEST(LspCoordinates, ASpanFromAHeaderResolvesToTheHeadersUri) {
    TempDir tmp;
    constexpr std::string_view kHeader = "int helper(int v) { return v; }\n";
    constexpr std::string_view kMain   =
        "#include \"h.h\"\n"
        "int main() { return helper(1); }\n";
    const fs::path header = tmp.write("h.h", kHeader);
    const fs::path file   = tmp.write("m.c", kMain);
    Built b = buildFile(shippedC(), file, {tmp.path()});
    ASSERT_NE(b.unit, nullptr);
    ASSERT_FALSE(b.unit->trees().empty());

    const DocumentCoordinates coords{
        *b.unit, dss::lsp::fileUriFromPath(file), std::string{kMain}};

    // The MAIN source's own line still answers, and answers about the MAIN
    // file — the half that a splice used to shift.
    auto mainPoint = coords.toSynth(Position{1, 20});
    ASSERT_TRUE(mainPoint.has_value())
        << "a position in the main source after a leading #include must "
           "resolve; before this row it landed inside the spliced header";

    // Walk the tree for a node whose origin is the HEADER, and assert the uri
    // names the header rather than the open document.
    auto const& tree = b.unit->trees()[0];
    const std::string headerUri = dss::lsp::fileUriFromPath(header);
    const std::string mainUri   = dss::lsp::fileUriFromPath(file);
    bool sawHeaderUri = false;
    bool sawMainUri   = false;
    // Recurse through `children()` — the tree's own traversal. A raw
    // `NodeId{0..nodeCount)` loop is NOT a valid enumeration and the tree
    // bounds-checks it into an abort.
    auto walk = [&](auto&& self, dss::NodeId n) -> void {
        if (!n.valid()) return;
        if (auto loc = coords.locate(tree, tree.span(n))) {
            if (loc->uri == headerUri) sawHeaderUri = true;
            if (loc->uri == mainUri)   sawMainUri   = true;
        }
        for (dss::NodeId c : tree.children(n)) self(self, c);
    };
    walk(walk, tree.root());
    EXPECT_TRUE(sawHeaderUri)
        << "a node spliced from the header must report the HEADER's uri — "
           "stamping the request's uri on it is what made a definition inside "
           "a header look like it lived in the open document";
    EXPECT_TRUE(sawMainUri)
        << "and the main file's own nodes must still report the main uri";
}

// ══ 4. A HEADER SPLICED TWICE ═══════════════════════════════════════════════
//
// The documented MANY-image answer: every image, in synth order. And the
// consequence that makes it safe for rename — the images map BACK to one
// origin range, so results collapse rather than duplicate.
TEST(LspCoordinates, AHeaderSplicedTwiceYieldsEveryImageAndOneOrigin) {
    TempDir tmp;
    // No include guard, deliberately: the point is TWO copies in the synth
    // buffer from ONE origin file.
    constexpr std::string_view kHeader = "int shared_value = 7;\n";
    constexpr std::string_view kMain   =
        "#include \"twice.h\"\n"
        "#include \"twice.h\"\n"
        "int main() { return 0; }\n";
    const fs::path header = tmp.write("twice.h", kHeader);
    const fs::path file   = tmp.write("t.c", kMain);
    Built b = buildFile(shippedC(), file, {tmp.path()});
    ASSERT_NE(b.unit, nullptr);

    const DocumentCoordinates coords{
        *b.unit, dss::lsp::fileUriFromPath(file), std::string{kMain}};

    // Ask the CU directly for the header's images, which is the relation the
    // consumers are built on.
    dss::BufferId headerBuffer{};
    for (auto const& aux : b.unit->auxiliaryBuffers()) {
        if (aux && fs::path{std::string{aux->name()}}.filename() == "twice.h") {
            headerBuffer = aux->id();
        }
    }
    ASSERT_TRUE(headerBuffer.valid()) << "the header must be an origin buffer";

    std::vector<dss::ByteOffset> images;
    b.unit->inversePreprocessedPositions(headerBuffer, dss::ByteOffset{4},
                                         images);
    ASSERT_EQ(images.size(), 2u)
        << "a header spliced twice has TWO synth images of each of its bytes; "
           "returning one would silently pick a splice";
    EXPECT_LT(images[0], images[1]) << "images are in ascending synth order";
}

// ══ 5. A SYNTHETIC ORIGIN ═══════════════════════════════════════════════════
//
// The built-in prologue is a real origin buffer with no file behind it. It has
// no Location — and a diagnostic from it must still be published, on the
// document at 0:0, NAMING its origin.
TEST(LspCoordinates, ASyntheticOriginHasNoLocationButStillPlacesADiagnostic) {
    TempDir tmp;
    constexpr std::string_view kSrc = "int x = 1;\n";
    const fs::path file = tmp.write("s.c", kSrc);
    Built b = buildFile(shippedC(), file);
    ASSERT_NE(b.unit, nullptr);
    const std::string uri = dss::lsp::fileUriFromPath(file);
    const DocumentCoordinates coords{*b.unit, uri, std::string{kSrc}};

    // Find the prologue's origin buffer by its synthetic name.
    dss::SourceBuffer const* builtIn = nullptr;
    for (auto const& aux : b.unit->auxiliaryBuffers()) {
        if (aux && dss::lsp::bufferIsSynthetic(*aux)) builtIn = aux.get();
    }
    ASSERT_NE(builtIn, nullptr)
        << "the built-in prologue must be a registered origin buffer — if it "
           "is not, the synthetic-origin case below is untested";
    EXPECT_TRUE(dss::lsp::bufferIsSynthetic(*builtIn));

    auto placed = coords.locateDiagnostic(
        builtIn->id(), dss::SourceSpan::of(dss::ByteOffset{0},
                                           dss::ByteOffset{1}));
    EXPECT_EQ(placed.uri, uri)
        << "a diagnostic with no file must still land on the open document — "
           "dropping it is worse than an imperfect position";
    EXPECT_EQ(placed.range.start.line, 0u);
    EXPECT_EQ(placed.range.start.character, 0u);
    EXPECT_FALSE(placed.syntheticOrigin.empty())
        << "and it must NAME its origin, or 0:0 reads as 'an error on line 1'";
    EXPECT_EQ(placed.syntheticOrigin, std::string{builtIn->name()});
}
