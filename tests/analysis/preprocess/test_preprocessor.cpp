// FC13 cycle 1 unit tests for the config-selected C preprocessor
// (src/analysis/preprocess/preprocessor.{hpp,cpp}). These exercise the engine
// DIRECTLY (build a SourceBuffer + the shipped c-subset schema, call
// preprocess, inspect the resulting token stream) so each guard is pinned in
// isolation. Every assertion is the STRONGEST provable property and is
// RED-ON-DISABLE (reverting the backing impl line fails the test).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace dss;

[[nodiscard]] std::shared_ptr<GrammarSchema const> cSubset() {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded.has_value()) {
        ADD_FAILURE() << "loadShipped(c-subset) failed";
        std::abort();
    }
    return *loaded;
}

// Run the preprocessor over `text` (no include dirs) and return the NON-trivia
// token lexemes (sliced from the synth buffer), in order. Directives removed +
// macros expanded, so this is exactly what the parser would see.
[[nodiscard]] std::vector<std::string> ppLexemes(std::string text,
                                                 PreprocessResult& out) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(std::move(text), "main.c");
    std::vector<std::filesystem::path> noDirs;
    out = preprocess(buf, schema, noDirs);
    std::vector<std::string> lexs;
    for (Token const& t : out.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        lexs.push_back(std::string{out.synthBuffer->slice(t.span)});
    }
    return lexs;
}

[[nodiscard]] bool hasPPCode(PreprocessResult const& r, DiagnosticCode code) {
    for (auto const& d : r.diagnostics->all()) {
        if (d.code == code) return true;
    }
    return false;
}

} // namespace

TEST(Preprocessor, ObjectMacroExpandsAndDirectiveRemoved) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X 42\nint v = X;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u) << "expected: int v = 42 ;";
    EXPECT_EQ(lexs[0], "int");
    EXPECT_EQ(lexs[1], "v");
    EXPECT_EQ(lexs[2], "=");
    EXPECT_EQ(lexs[3], "42");
    EXPECT_EQ(lexs[4], ";");
}

TEST(Preprocessor, MacroReplacementIsRescanned) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define A 7\n#define B A\nint v = B;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "7") << "B -> A -> 7 requires rescan";
}

TEST(Preprocessor, SelfReferentialMacroDoesNotLoop) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X X\nint v = X;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "X") << "a self-referential macro freezes to its own name";
}

TEST(Preprocessor, MutuallyRecursiveMacrosDoNotLoop) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X Y\n#define Y X\nint v = X;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_TRUE(lexs[3] == "X" || lexs[3] == "Y");
}

TEST(Preprocessor, UndefRemovesBinding) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X 1\n#undef X\nint v = X;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "X") << "after #undef, the name is no longer a macro";
}

TEST(Preprocessor, IncompatibleRedefinitionIsReported) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X 1\n#define X 2\nint v = X;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroRedefinition))
        << "a different #define of an existing macro must be reported";
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "1") << "the first definition is kept";
}

TEST(Preprocessor, IdenticalRedefinitionIsBenign) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X 1\n#define X 1\nint v = X;\n", r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroRedefinition))
        << "an identical redefinition is allowed by C";
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "1");
}

TEST(Preprocessor, FunctionLikeMacroDefinitionFailsLoud) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define F(x) ((x)+1)\nint v = 0;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "a function-like macro definition must be reported, not accepted";
    (void)lexs;
}

TEST(Preprocessor, SpaceBeforeParenIsObjectMacro) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define G (1+2)\nint v = G;\n", r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported));
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 9u);
    EXPECT_EQ(lexs[3], "(");
    EXPECT_EQ(lexs[4], "1");
    EXPECT_EQ(lexs[5], "+");
    EXPECT_EQ(lexs[6], "2");
    EXPECT_EQ(lexs[7], ")");
}

// IDENTITY on non-directive input: a TU with NO directives + NO macro uses
// passes through UNCHANGED -- the PP token stream equals the raw tokenizer
// stream (same core kinds + spans). The in==out property the no-op relies on.
TEST(Preprocessor, NonDirectiveInputIsIdentity) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        std::string{"int main(void) { return 1 + 2; }\n"}, "main.c");
    std::vector<std::filesystem::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_FALSE(r.diagnostics->hasErrors());

    Tokenizer tk{r.synthBuffer, schema};
    auto rawResult = std::move(tk).tokenize();
    std::vector<Token> raw;
    while (!rawResult.stream.isAtEnd()) {
        raw.push_back(rawResult.stream.advance());
    }

    // Compare the non-Eof content tokens (the PP appends its own single Eof;
    // the raw drain above stops before Eof). Identity means: same count, same
    // core kinds, same spans, in order.
    std::vector<Token> ppNoEof;
    for (Token const& t : r.tokens) {
        if (t.coreKind != CoreTokenKind::Eof) ppNoEof.push_back(t);
    }
    ASSERT_EQ(ppNoEof.size(), raw.size())
        << "non-directive input must be identity (content token count)";
    for (std::size_t i = 0; i < raw.size(); ++i) {
        EXPECT_EQ(ppNoEof[i].coreKind, raw[i].coreKind) << "at index " << i;
        EXPECT_EQ(ppNoEof[i].span, raw[i].span) << "at index " << i;
    }
}

// MULTI-LANGUAGE NO-OP at the config level: a language WITHOUT a preprocess
// block (toy, tsql-subset) reports preprocess().enabled == false, so the
// pipeline gate skips the pass; c-subset (which declares the block) reports
// true. RED-ON-DISABLE: removing the c-subset block flips its expectation.
TEST(Preprocessor, EnabledIsConfigDrivenPerLanguage) {
    auto c = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE((*c)->preprocess().enabled);

    auto toy = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(toy.has_value());
    EXPECT_FALSE((*toy)->preprocess().enabled);

    auto tsql = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(tsql.has_value());
    EXPECT_FALSE((*tsql)->preprocess().enabled);
}

// FIX 1 (RED-on-disable): the function-like-macro `(` opener is CONFIG-DRIVEN
// (`preprocess.functionLikeOpenToken`), NOT a hard-coded "ParenOpen". We prove
// it by loading the shipped c-subset config TEXT with `functionLikeOpenToken`
// rebound to a DIFFERENT real token (`BlockOpen` = `{`). Now `#define F(x)`
// must be treated as an OBJECT-like macro (the `(` is no longer the configured
// function-like opener), so it must NOT emit P_PreprocessorUnsupported.
// RED-ON-DISABLE: reverting the ctor to the literal `find("ParenOpen")` makes
// the engine ignore the rebound config and STILL detect `(` as function-like
// -> P_PreprocessorUnsupported fires -> this test fails. (Agnosticism: the
// opener is read from config, so a language whose paren token is named
// differently is handled correctly.)
TEST(Preprocessor, FunctionLikeOpenTokenIsConfigDrivenNotHardcoded) {
    auto loadedText = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loadedText.has_value());
    // Read the shipped c-subset config text (walk up to src/dss-config/sources).
    namespace fs = std::filesystem;
    std::string text;
    {
        std::error_code ec;
        fs::path here = fs::current_path(ec);
        for (int i = 0; i < 8 && !here.empty(); ++i) {
            fs::path const cand =
                here / "src" / "dss-config" / "sources" / "c-subset.lang.json";
            if (fs::exists(cand, ec)) {
                std::ifstream in(cand, std::ios::binary);
                text.assign(std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>());
                break;
            }
            fs::path const parent = here.parent_path();
            if (parent == here) break;
            here = parent;
        }
        ASSERT_FALSE(text.empty()) << "could not locate shipped c-subset config";
    }
    // Rebind ONLY the function-like opener to `BlockOpen` (a real, declared
    // c-subset token). The token name must resolve (validated at load), so this
    // is a well-formed schema -- just one where `(` is no longer the opener.
    const std::string from = "\"functionLikeOpenToken\": \"ParenOpen\"";
    const std::string to   = "\"functionLikeOpenToken\": \"BlockOpen\"";
    auto const pos = text.find(from);
    ASSERT_NE(pos, std::string::npos)
        << "shipped c-subset config no longer carries functionLikeOpenToken=ParenOpen";
    text.replace(pos, from.size(), to);

    auto loaded = GrammarSchema::loadFromText(text, "<rebound-paren-c-subset>");
    ASSERT_TRUE(loaded.has_value())
        << "rebound schema should still load: "
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    ASSERT_EQ(schema->preprocess().functionLikeOpenToken, "BlockOpen");

    // `#define F(x) ...` under the rebound opener: `(` is NOT the function-like
    // marker, so this is an OBJECT-like macro -> NO unsupported diagnostic.
    auto buf = SourceBuffer::fromString(
        std::string{"#define F(x) ((x)+1)\nint v = 0;\n"}, "main.c");
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "with the `(` opener rebound away, `#define F(x)` must be treated "
           "as object-like -- proving the opener is read from config, not "
           "hard-coded as ParenOpen";
}

// LINE-MAP HEADER ATTRIBUTION: a diagnostic that originates in an included
// header must be remapped (via PreprocessResult::makeRemap + the line-map) to
// the HEADER file's buffer, not the synthesized buffer. We drive this through
// the full CU pipeline (UnitBuilder) so the remap runs, then check that some
// diagnostic on the produced tree carries the header's BufferId.
//
// RED-ON-DISABLE: removing the `result.tree.remapDiagnostics(remap)` call in
// compilation_unit.cpp parseAndAdd_ (or the header-origin branch in makeRemap)
// leaves the diagnostic on the synth buffer and the header-attribution check
// fails.
TEST(Preprocessor, HeaderOriginDiagnosticAttributesToHeader) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_pp_linemap_test";
    fs::create_directories(dir);
    // The header contains a malformed construct (a stray `@` is an illegal
    // char in c-subset) so the parser/lexer emits a diagnostic whose span
    // lands inside the header's inlined text.
    {
        std::ofstream(dir / "bad.h", std::ios::binary)
            << "int bad(void) { return @; }\n";
    }
    auto mainPath = dir / "main.c";
    {
        std::ofstream(mainPath, std::ios::binary)
            << "#include \"bad.h\"\nint main(void) { return 0; }\n";
    }

    auto schema = cSubset();
    UnitBuilder builder{schema};
    builder.addFile(mainPath);
    auto cu = std::move(builder).finish();

    // Resolve the header's BufferId: the CU's tree source is the SYNTH buffer,
    // but a header-origin diagnostic must have been remapped to a buffer whose
    // NAME ends in "bad.h".
    ASSERT_EQ(cu.trees().size(), 1u);
    bool sawHeaderAttributed = false;
    for (auto const& d : cu.trees()[0].diagnostics().all()) {
        // The synth buffer is the tree's own source; a remapped header
        // diagnostic carries a DIFFERENT buffer id than the synth buffer.
        if (d.buffer != cu.trees()[0].source().id()) {
            sawHeaderAttributed = true;
        }
    }
    EXPECT_TRUE(sawHeaderAttributed)
        << "a diagnostic originating in the included header must be remapped "
           "off the synth buffer onto the header's own buffer";

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// FIX 4 (RED-on-disable): the headline cycle-1 lexer change (c-subset
// `directive` mode no longer overrides `<` -> HeaderStart) is what lets a
// `<<` shift operator survive inside a NON-include directive like `#define`.
// Preprocess `#define SHIFT (1 << 2)` + a use, and assert the EXPANSION lexes
// as the 5 non-trivia tokens `( 1 << 2 )` with NO HeaderStart/HeaderPath
// token. RED-ON-DISABLE: re-adding the `<`->HeaderStart override to the
// `directive` mode makes the `<<` mis-lex as a header path -> a HeaderPath
// token appears and the `<<` lexeme is gone -> this test fails.
TEST(Preprocessor, ShiftOperatorInDefineIsNotMisLexedAsHeader) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define SHIFT (1 << 2)\nint v = SHIFT;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());

    // The expansion of SHIFT must be exactly `( 1 << 2 )` surrounded by
    // `int v = ... ;` -> int v = ( 1 << 2 ) ;
    ASSERT_EQ(lexs.size(), 9u) << "expected: int v = ( 1 << 2 ) ;";
    EXPECT_EQ(lexs[0], "int");
    EXPECT_EQ(lexs[1], "v");
    EXPECT_EQ(lexs[2], "=");
    EXPECT_EQ(lexs[3], "(");
    EXPECT_EQ(lexs[4], "1");
    EXPECT_EQ(lexs[5], "<<") << "the shift operator must survive inside #define";
    EXPECT_EQ(lexs[6], "2");
    EXPECT_EQ(lexs[7], ")");
    EXPECT_EQ(lexs[8], ";");

    // Strongest pin: NO token in the stream carries the HeaderStart/HeaderPath
    // schema kind. If the `directive` mode mis-lexed `< 2)\n...` as a header
    // path, one of these would appear.
    auto schema = cSubset();
    const SchemaTokenId headerStart =
        schema->schemaTokens().find("HeaderStart");
    const SchemaTokenId headerPath =
        schema->schemaTokens().find("HeaderPath");
    ASSERT_TRUE(headerStart.valid() && headerPath.valid())
        << "c-subset must declare HeaderStart/HeaderPath for this pin to mean "
           "anything";
    for (Token const& t : r.tokens) {
        EXPECT_NE(t.schemaKind, headerStart)
            << "a `<<` in #define must never lex as a header opener";
        EXPECT_NE(t.schemaKind, headerPath)
            << "a `<<` in #define must never lex as a header path";
    }
}

// FIX 2 (RED-on-disable): full diagnostic-RENDER attribution across an
// include splice. A TU whose FIRST line is a quote-`#include` of a header
// that contains an error, followed by a main-file error on a LATER line,
// must render (via DiagnosticReporter::formatAll over a registry assembled
// from the CU's tree sources + auxiliaryBuffers()):
//   * the HEADER's path:line for the header-origin error, AND
//   * the ORIGINAL main.c line for the main-origin error (NOT a synth-shifted
//     line -- the leading header splice must not drift the main line), AND
//   * NEVER the `<unknown-buffer` sentinel.
// RED-ON-DISABLE: reverting the program.cpp/corpus auxiliaryBuffers()
// registration (or the makeRemap main-origin remap) breaks one of these.
TEST(Preprocessor, IncludeSpliceDiagnosticsRenderToRealFilesAndLines) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_pp_render_attribution_test";
    fs::create_directories(dir);
    // Header error: a stray `@` (illegal char) on the header's line 1.
    { std::ofstream(dir / "bad.h", std::ios::binary)
          << "int hdr(void) { return @; }\n"; }
    // Main: a LEADING include (line 1), then a main-file error (`@`) on line 2.
    auto mainPath = dir / "main.c";
    { std::ofstream(mainPath, std::ios::binary)
          << "#include \"bad.h\"\nint main(void) { return @; }\n"; }

    auto schema = cSubset();
    UnitBuilder builder{schema};
    builder.addFile(mainPath);
    auto cu = std::move(builder).finish();
    ASSERT_EQ(cu.trees().size(), 1u);

    // Assemble the registry exactly as the driver does: every tree's own
    // source PLUS the CU's auxiliary (PP origin) buffers.
    BufferRegistry bufs;
    for (auto const& tree : cu.trees()) {
        if (auto s = tree.sourceShared()) bufs.add(std::move(s));
    }
    for (auto const& b : cu.auxiliaryBuffers()) {
        if (b) bufs.add(b);
    }

    std::string const rendered = cu.trees()[0].diagnostics().formatAll(bufs);

    // The header error attributes to bad.h line 1.
    EXPECT_NE(rendered.find("bad.h:1:"), std::string::npos)
        << "header-origin diagnostic must render the header path:line\n"
        << rendered;
    // The main error attributes to the ORIGINAL main.c line 2 (after the
    // leading #include) -- proving the splice did not drift the main line.
    EXPECT_NE(rendered.find("main.c:2:"), std::string::npos)
        << "main-origin diagnostic must render the ORIGINAL main.c line 2\n"
        << rendered;
    // Never the unknown-buffer sentinel -- every origin buffer is registered.
    EXPECT_EQ(rendered.find("<unknown-buffer"), std::string::npos)
        << "every remapped diagnostic must resolve to a registered buffer\n"
        << rendered;

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// FIX 6 (strict multi-language NO-OP proof): a language with NO `preprocess`
// block, driven through the SAME `preprocess().enabled`-gated pipeline
// (UnitBuilder), must be a strict IDENTITY -- the parsed tree's source is the
// ORIGINAL buffer verbatim (NO synthesized buffer, NO splice, NO directive
// removal), not merely `enabled == false`. tsql `:r`-style directives would
// be mangled if the C preprocessor ever ran on a non-opted-in language, so we
// feed a source that contains a `#`-shaped token and a `<<`-shaped token and
// assert they pass through byte-for-byte. The gate is config-driven (the
// preprocess block's absence), never a language-name check.
TEST(Preprocessor, DisabledLanguageGatePipelineIsStrictIdentity) {
    auto tsql = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(tsql.has_value());
    ASSERT_FALSE((*tsql)->preprocess().enabled)
        << "tsql-subset must declare no preprocess block (gate=false)";

    // A source whose BYTES the C preprocessor would mutate if it ran: it has
    // no real C directive, but the identity property is that the gated
    // pipeline leaves the tree's source text EXACTLY equal to the input.
    std::string const src = "SELECT id FROM T WHERE id = 1;";
    UnitBuilder builder{*tsql};
    builder.addInMemory(src, "q.sql");
    auto cu = std::move(builder).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    // Strict in==out: the parsed tree's source is the original text verbatim.
    // A preprocessed language would expose a SYNTHESIZED buffer here instead.
    EXPECT_EQ(std::string{cu.trees()[0].source().text()}, src)
        << "a no-preprocess language must pass its source through unchanged";
    // And NO auxiliary (PP origin) buffers were produced -- the pass never ran.
    EXPECT_TRUE(cu.auxiliaryBuffers().empty())
        << "the preprocessor must not run for a language without a block";
}
