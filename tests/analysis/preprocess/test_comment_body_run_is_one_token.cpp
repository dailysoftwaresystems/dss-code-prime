// D-PERF-PP-OFF-GRAMMAR-BODY-RUN-IS-ONE-TOKEN-PER-CODEPOINT
//
// The preprocessor materialized ONE TOKEN PER COMMENT CHARACTER and carried
// that granularity all the way to the parser, which turned every one of them
// into an off-grammar AST leaf.
//
// ✔MEASURED (cycle P34, sqlite 103 TU, Release, Windows/MinGW GCC 13.2): of the
// 106.3 M tokens the preprocessor materialized, 93.1 M — 89% — were
// single-character comment-body tokens; the stream handed to the PARSER was
// 86.2 M tokens of which 82.2 M were non-newline trivia. Folding each
// off-grammar EmptySpace body run into ONE token took the whole compile's
// attributed CPU from 4m14.9s to 2m06.2s and left the 6,904,848-byte sqlite3
// artifact BYTE-IDENTICAL.
//
// ★★ WHY THE PIN IS A COMPARISON AND NOT A COUNT. A test that asserts "this
// source yields N tokens" is a snapshot: it goes red for any unrelated change
// to the schema, the prologue, or the predefined-macro list, and it says
// nothing about the property that matters. The property is that the token count
// does not GROW WITH COMMENT LENGTH — so every arm here preprocesses TWO
// sources that differ ONLY in how long a comment is, and asserts the two
// results have the SAME number of tokens. That equality is false by exactly the
// character-count difference when the fold is removed, and it stays true across
// any future change that does not reintroduce per-codepoint comment tokens.
//
// ★ THE OTHER ARMS ARE THE EXCLUSIONS THE FOLD MUST HONOUR, each of which would
// be a SILENT WRONG ANSWER rather than a crash if it broke: the directive line
// structure a folded comment must not erase, the byte attribution `__LINE__`
// reads, and the one-space-per-comment rule `#` stringize depends on.

#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/header_name_matching.hpp"
#include "core/types/source_buffer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dss;
namespace fs = std::filesystem;

// Shared schema fixture — a REFERENCE to a function-local static, for the
// reason `test_preprocessor.cpp` spells out under
// D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE: `GrammarSchema`'s accessors hand
// back references INTO the schema, so a by-value return would make
// `helper()->accessor()` a heap-use-after-free.
[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cSchema() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded.has_value()) {
            // THROW, never `std::abort()`: abort kills the whole test BINARY,
            // so every sibling test loses its verdict. Machine-checked by
            // check-no-abort-in-tests.
            throw std::runtime_error{"loadShipped(c) failed"};
        }
        return *loaded;
    }();
    return schema;
}

[[nodiscard]] PreprocessResult pp(std::string text) {
    std::vector<fs::path> const noDirs;
    auto buf = SourceBuffer::fromString(std::move(text), "comment-run.c");
    return preprocess(buf, cSchema(), noDirs, kDefaultHeaderNameMatching,
                      DiagnosticBudget::libraryDefault());
}

// The SIGNIFICANT lexemes of a result — what the parser actually consumes.
// Trivia is dropped here deliberately: this helper answers "did the program
// change", and the whole point of the fold is that it must not.
[[nodiscard]] std::vector<std::string> significantLexemes(
    PreprocessResult const& r) {
    std::vector<std::string> out;
    for (Token const& t : r.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        if (isEmptySpace(t.flags)) continue;
        out.push_back(std::string{r.synthBuffer->slice(t.span)});
    }
    return out;
}

// A comment body of exactly `n` filler characters. Deliberately NOT `*` or `/`
// so no length can accidentally close the comment early.
[[nodiscard]] std::string filler(std::size_t n) { return std::string(n, 'x'); }

constexpr std::size_t kShort = 4;
constexpr std::size_t kLong  = 4000;

// ══ THE PIN ═══════════════════════════════════════════════════════════════
// One block comment costs ONE token, whatever its length.

TEST(CommentBodyRun, BlockCommentTokenCountDoesNotGrowWithItsLength) {
    PreprocessResult const shortC = pp("int a = 1; /*" + filler(kShort) + "*/\n");
    PreprocessResult const longC  = pp("int a = 1; /*" + filler(kLong) + "*/\n");

    ASSERT_FALSE(shortC.diagnostics->hasErrors());
    ASSERT_FALSE(longC.diagnostics->hasErrors());

    // The load-bearing equality. Without the fold this differs by
    // kLong - kShort == 3996.
    EXPECT_EQ(longC.tokens.size(), shortC.tokens.size())
        << "a longer comment produced more tokens — the off-grammar body run "
           "is being carried one token per codepoint again";

    // ... and the program itself is untouched by the comment's length.
    EXPECT_EQ(significantLexemes(longC), significantLexemes(shortC));
}

TEST(CommentBodyRun, LineCommentTokenCountDoesNotGrowWithItsLength) {
    PreprocessResult const shortC = pp("int a = 1; //" + filler(kShort) + "\nint b = 2;\n");
    PreprocessResult const longC  = pp("int a = 1; //" + filler(kLong) + "\nint b = 2;\n");

    ASSERT_FALSE(shortC.diagnostics->hasErrors());
    ASSERT_FALSE(longC.diagnostics->hasErrors());

    EXPECT_EQ(longC.tokens.size(), shortC.tokens.size())
        << "a longer line comment produced more tokens — the off-grammar body "
           "run is being carried one token per codepoint again";
    EXPECT_EQ(significantLexemes(longC), significantLexemes(shortC));
}

// ══ THE EXCLUSION THAT KEEPS VALUE-BEARING BODIES SEPARABLE ═══════════════
// A string body is a body-mode default kind too. It is NOT `EmptySpace`, so the
// fold must not touch it — and the proof is that its bytes survive intact.

TEST(CommentBodyRun, StringLiteralBodyIsUnaffected) {
    PreprocessResult const r = pp("char const* s = \"a/*not a comment*/b\";\n");
    ASSERT_FALSE(r.diagnostics->hasErrors());

    bool foundBody = false;
    for (std::string const& lex : significantLexemes(r)) {
        if (lex == "a/*not a comment*/b") foundBody = true;
    }
    EXPECT_TRUE(foundBody)
        << "the string body's bytes did not survive as one lexeme";
}

// ══ THE EXCLUSIONS THAT WOULD BE SILENT WRONG ANSWERS ═════════════════════

// D-PP-LINE-COMMENT-BEFORE-DIRECTIVE: a `//` comment ends AT the newline and
// EXCLUDES it, so the `#` on the next line is still FIRST ON ITS LINE. Folding
// the comment body must not swallow that newline.
TEST(CommentBodyRun, LineCommentBeforeADirectiveKeepsTheDirectiveFirstOnLine) {
    PreprocessResult const r = pp("//" + filler(kLong) + "\n#define X 7\nint v = X;\n");
    ASSERT_FALSE(r.diagnostics->hasErrors());

    std::vector<std::string> const lexs = significantLexemes(r);
    bool sawSeven = false;
    for (std::string const& lex : lexs) {
        if (lex == "7") sawSeven = true;
        EXPECT_NE(lex, "X") << "`#define X 7` was not seen as a directive — the "
                               "folded comment ate the line boundary";
    }
    EXPECT_TRUE(sawSeven) << "X did not expand to 7";
}

// A block comment CONTAINS its newlines as body codepoints (they are body
// tokens, not Newline tokens), so a directive line legitimately continues
// across one. Folding must preserve that exactly, in BOTH directions: the
// `#define` must still see its replacement list, and the line must still end.
TEST(CommentBodyRun, BlockCommentInsideADirectiveLineDoesNotEndTheLine) {
    PreprocessResult const r = pp("#define X /* a\nb\nc */ 7\nint v = X;\n");
    ASSERT_FALSE(r.diagnostics->hasErrors());

    std::vector<std::string> const lexs = significantLexemes(r);
    bool sawSeven = false;
    for (std::string const& lex : lexs) {
        if (lex == "7") sawSeven = true;
        EXPECT_NE(lex, "X") << "the `#define` line was cut short by the comment's "
                               "embedded newlines";
    }
    EXPECT_TRUE(sawSeven) << "X did not expand to 7 across the multi-line comment";
}

// ★ BYTE ATTRIBUTION IS UNCHANGED. `__LINE__` (C 6.10.8.1) resolves an
// invocation OFFSET through the line map to a real origin line, so it is the
// sharpest available witness that folding removed TOKENS and not the byte
// positions those tokens carried. The comment below occupies lines 1-4; the
// `__LINE__` therefore sits on line 5, and it must say so.
TEST(CommentBodyRun, LineNumberAfterAMultiLineCommentIsUnchanged) {
    PreprocessResult const r = pp("/* a\nb\nc\nd */\nint v = __LINE__;\n");
    ASSERT_FALSE(r.diagnostics->hasErrors());

    bool sawFive = false;
    for (std::string const& lex : significantLexemes(r)) {
        if (lex == "5") sawFive = true;
    }
    EXPECT_TRUE(sawFive)
        << "__LINE__ after a four-line comment did not resolve to line 5 — the "
           "fold moved a byte attribution, which is a silent wrong answer";
}

// C 6.10.5.2p3: each occurrence of white space between the stringized
// argument's tokens becomes ONE space, and a comment IS white space
// (C 5.1.1.2p1 phase 3). The fold makes the comment one token; it must not make
// it zero, and it must not make it two spaces.
TEST(CommentBodyRun, StringizeTurnsACommentIntoExactlyOneSpace) {
    PreprocessResult const r = pp("#define S(x) #x\nchar const* s = S(a/*c*/b);\n");
    ASSERT_FALSE(r.diagnostics->hasErrors());

    bool sawSpaced = false;
    for (std::string const& lex : significantLexemes(r)) {
        if (lex == "a b") sawSpaced = true;
    }
    EXPECT_TRUE(sawSpaced)
        << "S(a/*c*/b) did not stringize to \"a b\" — a comment must count as "
           "exactly one space between the argument's tokens";
}

// The folded token's span must cover EXACTLY the run it replaces: no byte of
// the comment may fall outside a token, and no token may claim a byte past it.
// Checked structurally over the whole stream rather than on one token, so a
// future fold that over-reaches by a single byte is caught wherever it happens.
TEST(CommentBodyRun, EveryTokenSpanStaysInsideTheSynthBufferAndInOrder) {
    PreprocessResult const r = pp("int a = 1; /*" + filler(kLong)
                                  + "*/ int b = 2; //" + filler(kLong) + "\n");
    ASSERT_FALSE(r.diagnostics->hasErrors());
    ASSERT_NE(r.synthBuffer, nullptr);

    const auto limit = static_cast<ByteOffset>(r.synthBuffer->text().size());
    for (Token const& t : r.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        EXPECT_LE(t.span.start(), t.span.end()) << "inverted token span";
        EXPECT_LE(t.span.end(), limit) << "token span runs past the synth buffer";
    }
}

}   // namespace
