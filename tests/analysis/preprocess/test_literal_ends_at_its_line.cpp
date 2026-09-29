// ★★★ A LITERAL ENDS AT ITS LINE, AND A TOKEN NOBODY CONVERTS IS NOT JUDGED.
//
// Two defects, found together and closed together, because the first one hid
// behind the second.
//
// ── [[D-TOK-STRING-STYLE-MULTILINE-IS-NEVER-READ]] — the swallow ────────────
//
// `stringStyle.multiline` is documented (docs/language-config-spec.md) as
// "whether newlines are allowed in the body", default FALSE, and the tokenizer
// never read it: every body ran to its closing delimiter across any number of
// lines. C23 6.4.4.4 (c-char), 6.4.5 (s-char) and 6.4.7 (h-char) all exclude
// new-line. ✔MEASURED 2026-09-22 on this program —
//
//     #define V 4
//     #ifndef V
//     #error V's missing
//     #endif
//     #if V != 5
//     #error V's wrong
//     #endif
//
// — gcc 13.3.0, clang 18.1.3 and MinGW gcc refuse it (rc 1) and MSVC VS 18
// refuses it (rc 2), every one on the second `#error`. DSS COMPILED AND RAN IT:
// the `'` of `V's missing` opened a character constant that ran to the `'` of
// `V's wrong`, and every directive between them was body bytes. With a lone `"`
// in place of each `'` the result was the same. It was exposed by a vacuous
// green: a module-settings pin in `tests/program/test_dependency_resolver.cpp`
// whose `#error` messages held possessives passed without its second check ever
// being evaluated.
//
// ── [[D-PP-CONVERSION-DIAGNOSTIC-FIRES-ON-A-TOKEN-NEVER-CONVERTED]] ─────────
//
// Once the literal ends at its line, the next question is WHO judges it. It is
// ONE preprocessing token from the quote to the end of its line (gcc spells it
// `token "'a" is not valid`), and only its CONVERSION fails — so, like every
// conversion judgement, it applies only to a token phase 7 converts. The gate
// used to decide by the byte's LIVENESS. ✔MEASURED, same four references: a
// lone quote in `#warning don't panic` text or an unused `#define X 'a` is
// accepted by gcc, clang and MinGW (one working reference makes it required);
// `#undef Y don't` and `#if 1`/`#elif 'a` by all four; and the same holds for a
// malformed number (`#warning 0x1g`, `#define Z 0x1g`, `#if 1`/`#elif 2d`) —
// while all four refuse the same tokens in live code or an evaluated `#if`.
//
// ⚠ EVERY ACCEPT ARM IS PAIRED WITH ITS REFUSAL, and every accept arm checks the
// live code after it survived, so neither "nothing was reported" nor "nothing
// was refused" can be satisfied by a preprocessor that dropped the program.

#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "scratch_dir.hpp"
#include "shipped_schema_or_throw.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dss;

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema =
        dss::test_support::shippedSchemaOrThrow("c");
    return schema;
}

// ── the tokenizer, alone ────────────────────────────────────────────────────

struct Lexed {
    std::shared_ptr<SourceBuffer> src;
    std::vector<Token>            tokens;
    std::vector<ParseDiagnostic>  diags;

    [[nodiscard]] std::string_view text(Token const& t) const { return src->slice(t.span); }

    [[nodiscard]] std::vector<ParseDiagnostic> withCode(DiagnosticCode c) const {
        std::vector<ParseDiagnostic> out;
        for (auto const& d : diags) {
            if (d.code == c) out.push_back(d);
        }
        return out;
    }

    // The non-trivia lexemes, in order — what an accept arm compares.
    [[nodiscard]] std::vector<std::string> lexemes() const {
        std::vector<std::string> out;
        for (Token const& t : tokens) {
            if (t.coreKind == CoreTokenKind::Whitespace
                || t.coreKind == CoreTokenKind::Newline
                || t.coreKind == CoreTokenKind::Eof) {
                continue;
            }
            if ((t.flags & NodeFlags::EmptySpace) != NodeFlags::None) continue;
            out.emplace_back(text(t));
        }
        return out;
    }
};

[[nodiscard]] Lexed lex(std::string text) {
    Lexed out;
    out.src = SourceBuffer::fromString(std::move(text), "<test>");
    Tokenizer t{out.src, cLanguage(), DiagnosticBudget::libraryDefault()};
    auto [stream, reporter] = std::move(t).tokenize();
    while (!stream.isAtEnd()) out.tokens.push_back(stream.advance());
    auto const all = reporter->all();
    out.diags.assign(all.begin(), all.end());
    return out;
}

[[nodiscard]] SchemaTokenId kindNamed(std::string_view name) {
    return cLanguage()->schemaTokens().find(std::string{name});
}

// ── the preprocessor ────────────────────────────────────────────────────────

struct PPRun {
    PreprocessResult         result;
    std::vector<std::string> lexemes;
};

[[nodiscard]] PPRun ppRun(std::string_view source) {
    PPRun out;
    auto  buf = SourceBuffer::fromString(std::string{source}, "main.c");
    std::vector<std::filesystem::path> noDirs;
    out.result = preprocess(buf, cLanguage(), noDirs, kDefaultHeaderNameMatching,
                            DiagnosticBudget::libraryDefault());
    for (Token const& t : out.result.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        if ((t.flags & NodeFlags::EmptySpace) != NodeFlags::None) continue;
        out.lexemes.push_back(std::string{out.result.synthBuffer->slice(t.span)});
    }
    return out;
}

[[nodiscard]] std::vector<ParseDiagnostic> diagsWith(PPRun const& r, DiagnosticCode c) {
    std::vector<ParseDiagnostic> out;
    if (!r.result.diagnostics) return out;
    for (ParseDiagnostic const& d : r.result.diagnostics->all()) {
        if (d.code == c) out.push_back(d);
    }
    return out;
}

[[nodiscard]] bool has(PPRun const& r, DiagnosticCode c) { return !diagsWith(r, c).empty(); }

[[nodiscard]] std::string everything(PPRun const& r) {
    std::string out;
    if (!r.result.diagnostics) return out;
    for (ParseDiagnostic const& d : r.result.diagnostics->all()) {
        out += "\n  ";
        out += diagnosticCodeName(d.code);
        out += ": ";
        out += d.actual;
    }
    return out.empty() ? std::string{" <none>"} : out;
}

// The live tail every accept arm carries.
constexpr std::string_view kLiveTail = "int survivor;\n";

void expectTailSurvived(PPRun const& r) {
    ASSERT_GE(r.lexemes.size(), 3u) << "the live tail must survive";
    auto const n = r.lexemes.size();
    EXPECT_EQ(r.lexemes[n - 3], "int");
    EXPECT_EQ(r.lexemes[n - 2], "survivor");
    EXPECT_EQ(r.lexemes[n - 1], ";");
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// GROUP 1 — the tokenizer honours `multiline` (the owner of the swallow)
// ════════════════════════════════════════════════════════════════════════════

// A character constant with no closer ends at its line: the body token holds the
// rest of THAT line, no closer is invented, the new-line survives, and the next
// line lexes as code. The report sits at the OPENER.
TEST(LiteralEndsAtItsLine, CharacterConstantEndsAtTheNewline) {
    std::string const src = "int c = 'a;\nint d;\n";
    Lexed const r = lex(src);
    auto const unterminated = r.withCode(DiagnosticCode::P_UnterminatedString);
    ASSERT_EQ(unterminated.size(), 1u);
    EXPECT_EQ(unterminated[0].span.start(), src.find('\''))
        << "reported at the literal the author wrote, not where the scan gave up";
    EXPECT_EQ(unterminated[0].span.end(), src.find('\n'))
        << "and it ends at its own line";

    auto const body = std::ranges::find_if(r.tokens, [](Token const& t) {
        return t.schemaKind == kindNamed("CharLiteral");
    });
    ASSERT_NE(body, r.tokens.end());
    EXPECT_EQ(r.text(*body), "a;") << "the body is the rest of THAT line, nothing more";
    EXPECT_TRUE(std::ranges::none_of(r.tokens, [](Token const& t) {
        return t.schemaKind == kindNamed("CharEnd");
    })) << "there is no closer in the source, so none may be emitted";

    auto const lexs = r.lexemes();
    ASSERT_GE(lexs.size(), 3u);
    EXPECT_EQ(lexs[lexs.size() - 3], "int");
    EXPECT_EQ(lexs[lexs.size() - 2], "d");
    EXPECT_EQ(lexs[lexs.size() - 1], ";") << "the next line is code, not literal";
}

TEST(LiteralEndsAtItsLine, StringLiteralEndsAtTheNewline) {
    std::string const src = "char const *s = \"abc;\nint d;\n";
    Lexed const r = lex(src);
    auto const unterminated = r.withCode(DiagnosticCode::P_UnterminatedString);
    ASSERT_EQ(unterminated.size(), 1u);
    EXPECT_EQ(unterminated[0].span.start(), src.find('"'));
    auto const lexs = r.lexemes();
    ASSERT_GE(lexs.size(), 2u);
    EXPECT_EQ(lexs[lexs.size() - 2], "d");
}

// C23 6.4.7: an h-char is never a new-line either.
TEST(LiteralEndsAtItsLine, HeaderNameEndsAtTheNewline) {
    std::string const src = "#include <foo.h\nint d;\n";
    Lexed const r = lex(src);
    auto const unterminated = r.withCode(DiagnosticCode::P_UnterminatedString);
    ASSERT_EQ(unterminated.size(), 1u);
    EXPECT_EQ(unterminated[0].span.start(), src.find('<'));
    auto const lexs = r.lexemes();
    ASSERT_GE(lexs.size(), 2u);
    EXPECT_EQ(lexs[lexs.size() - 2], "d");
}

// THE CONTROL: the one C body that declares `multiline: true` still spans lines,
// and a closed literal is untouched.
TEST(LiteralEndsAtItsLine, BlockCommentStillSpansLinesAndClosedLiteralsAreUntouched) {
    Lexed const r = lex("/* it's\n a \" note */ char c = '\\'';\nchar const *s = \"it's\";\n");
    EXPECT_TRUE(r.diags.empty());
    std::vector<std::string> const want{"char", "c", "=", "'", "\\'", "'", ";",
                                        "char", "const", "*", "s", "=",
                                        "\"", "it's", "\"", ";"};
    EXPECT_EQ(r.lexemes(), want);
}

// An ESCAPED new-line is the escape rule's — a continuation — and runs first.
// (In a C translation unit phase 2 has already spliced it away; the tokenizer's
// own rule is pinned here on raw text.)
TEST(LiteralEndsAtItsLine, AnEscapedNewlineContinuesTheBody) {
    Lexed const r = lex("char const *s = \"ab\\\ncd\";\n");
    EXPECT_TRUE(r.diags.empty());
    EXPECT_TRUE(std::ranges::any_of(r.tokens, [](Token const& t) {
        return t.schemaKind == kindNamed("StringEnd");
    }));
}

// The end-of-source report moved to the opener too: the same site, one rule.
TEST(LiteralEndsAtItsLine, EndOfSourceInsideALiteralReportsAtTheOpener) {
    std::string const src = "char c = 'a";
    Lexed const r = lex(src);
    auto const unterminated = r.withCode(DiagnosticCode::P_UnterminatedString);
    ASSERT_EQ(unterminated.size(), 1u);
    EXPECT_EQ(unterminated[0].span.start(), src.find('\''));
    EXPECT_EQ(unterminated[0].span.end(), src.size());
}

// ════════════════════════════════════════════════════════════════════════════
// GROUP 2 — the swallow, through the preprocessor
// ════════════════════════════════════════════════════════════════════════════

// ★★★ THE PIN THE DEFECT WAS: the second `#error` must fire, with its own text.
// RED-ON-DISABLE: stop ending the body at the new-line and the `'` of
// `V's missing` swallows through `V's wrong` — no `#error` fires at all.
TEST(LiteralEndsAtItsLine, LoneApostrophesDoNotSwallowTheDirectivesBetweenThem) {
    PPRun const r = ppRun("#define V 4\n#ifndef V\n#error V's missing\n#endif\n"
                          "#if V != 5\n#error V's wrong\n#endif\n");
    auto const errors = diagsWith(r, DiagnosticCode::P_PreprocessorErrorDirective);
    ASSERT_EQ(errors.size(), 1u) << everything(r);
    EXPECT_NE(errors[0].actual.find("V's wrong"), std::string::npos)
        << "the refusal is the second `#error`, carrying its own text: "
        << errors[0].actual;
    EXPECT_FALSE(has(r, DiagnosticCode::P_UnterminatedString))
        << "an `#error`'s words are never converted, so its quote is not judged"
        << everything(r);
}

TEST(LiteralEndsAtItsLine, LoneDoubleQuotesDoNotSwallowTheDirectivesBetweenThem) {
    PPRun const r = ppRun("#define V 4\n#ifndef V\n#error say \"missing\n#endif\n"
                          "#if V != 5\n#error say \"wrong\n#endif\n");
    auto const errors = diagsWith(r, DiagnosticCode::P_PreprocessorErrorDirective);
    ASSERT_EQ(errors.size(), 1u) << everything(r);
    EXPECT_NE(errors[0].actual.find("\"wrong"), std::string::npos) << errors[0].actual;
}

// Prose in a skipped group — the everyday case: one quote on a line, never
// closed. ✔ all four references accept it.
TEST(LiteralEndsAtItsLine, ProseWithLoneQuotesInASkippedGroupCompiles) {
    PPRun const r = ppRun(std::string{"#if 0\nthis isn't code\nsay \"hi\n"
                                      "#error it's skipped\n#warning don't\n"
                                      "#include <unclosed.h\n#endif\n"}
                          + std::string{kLiveTail});
    EXPECT_FALSE(r.result.diagnostics->hasErrors()) << everything(r);
    expectTailSurvived(r);
}

// `#warning` text is never converted: accepted, WITH its warning. ✔ gcc, clang
// and MinGW accept (MSVC errors), and one working reference makes it required.
TEST(LiteralEndsAtItsLine, WarningTextWithALoneQuoteIsAcceptedWithItsWarning) {
    PPRun const r = ppRun(std::string{"#warning don't panic\n"} + std::string{kLiveTail});
    EXPECT_FALSE(r.result.diagnostics->hasErrors()) << everything(r);
    auto const warnings = diagsWith(r, DiagnosticCode::P_PreprocessorWarningDirective);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].actual.find("don't panic"), std::string::npos) << warnings[0].actual;
    expectTailSurvived(r);
}

// ★ In LIVE code the unterminated literal IS converted, so it is refused — at
// ITS line — and nothing after that line is swallowed: the malformed number two
// lines later is reported too, which it could only be if it was lexed as code.
TEST(LiteralEndsAtItsLine, InLiveCodeItIsRefusedAtItsLineAndSwallowsNothing) {
    std::string const src = "int f(void){ return 'a; }\nint g = 1;\nint h = 2d;\n";
    PPRun const r = ppRun(src);
    auto const unterminated = diagsWith(r, DiagnosticCode::P_UnterminatedString);
    ASSERT_EQ(unterminated.size(), 1u) << everything(r);
    // Synth-buffer coordinates (the predefines precede the source), so the
    // position is read as the TEXT it covers: from the opener to the end of its
    // own line, and not one byte further.
    EXPECT_EQ(r.result.synthBuffer->slice(unterminated[0].span), "'a; }");
    EXPECT_TRUE(has(r, DiagnosticCode::P_MalformedNumber))
        << "line 3 was lexed as code, so its own defect is reported" << everything(r);
}

// A replacement list is converted only where the macro is expanded.
TEST(LiteralEndsAtItsLine, AnUnexpandedDefineWithALoneQuoteIsNotJudged) {
    PPRun const unused = ppRun(std::string{"#define Q don't\n"} + std::string{kLiveTail});
    EXPECT_FALSE(unused.result.diagnostics->hasErrors()) << everything(unused);
    expectTailSurvived(unused);

    PPRun const used = ppRun("#define Q 'a\nint x = Q;\n");
    EXPECT_TRUE(has(used, DiagnosticCode::P_UnterminatedString))
        << "expanded, the quote reaches phase 7" << everything(used);
}

// An `#undef`'s extra words and an `#elif` after a taken group are never
// converted; an EVALUATED `#if` operand is.
TEST(LiteralEndsAtItsLine, OnlyAnEvaluatedConditionConvertsItsQuote) {
    PPRun const undef = ppRun(std::string{"#define Y 1\n#undef Y don't\n"}
                              + std::string{kLiveTail});
    EXPECT_FALSE(has(undef, DiagnosticCode::P_UnterminatedString)) << everything(undef);

    PPRun const notEvaluated = ppRun(std::string{"#if 1\n#elif 'a\n#endif\n"}
                                     + std::string{kLiveTail});
    EXPECT_FALSE(notEvaluated.result.diagnostics->hasErrors()) << everything(notEvaluated);
    expectTailSurvived(notEvaluated);

    PPRun const evaluated = ppRun(std::string{"#if 'a\n#endif\n"} + std::string{kLiveTail});
    EXPECT_TRUE(evaluated.result.diagnostics->hasErrors())
        << "an evaluated condition converts its tokens" << everything(evaluated);
}

// What must stay INERT: a quote inside a comment, a whole literal in a skipped
// group — including one that spells `#endif`.
TEST(LiteralEndsAtItsLine, QuotesInCommentsAndClosedLiteralsInSkippedGroupsStayInert) {
    PPRun const r = ppRun(std::string{"// it's \" fine\n/* it's\n \" fine */\n"
                                      "#if 0\nchar const *s = \"#endif\";\nint c = '#';\n#endif\n"}
                          + std::string{kLiveTail});
    EXPECT_FALSE(r.result.diagnostics->hasErrors()) << everything(r);
    ASSERT_EQ(r.lexemes.size(), 3u) << "the skipped group stayed skipped, and only "
                                       "the live tail survived";
    expectTailSurvived(r);
}

// ════════════════════════════════════════════════════════════════════════════
// GROUP 2b — a directive that INTERPRETS a quoted operand refuses an unclosed one
// ════════════════════════════════════════════════════════════════════════════
//
// ★ THE HOLE THE LEXER FIX OPENED, CLOSED IN THE SAME CHANGE. Before it, the
// unclosed name ran on to the next quote and could never resolve; after it, the
// name ends at its line and — ✔MEASURED on the first build — DSS SPLICED
// `#include "hdr.h` with `hdr.h` present, EMBEDDED `#embed "data.bin`, and
// RENAMED the presumed file on `#line 10 "foo`, each rc 0, while gcc 13.3.0,
// clang 18.1.3, MinGW gcc and MSVC VS 18 all refuse every one. Each pin carries
// its CLOSED control, which must keep working.

namespace {

// A main.c and its neighbours on disk, preprocessed by path, so a quote
// include resolves against the file's own directory.
[[nodiscard]] PPRun ppRunOnDisk(std::string_view tag, std::string_view mainText,
                                std::string_view auxName, std::string_view auxText) {
    namespace fs = std::filesystem;
    static dss::test_support::ScratchDir const root{dss::test_support::Location::Temp,
                                                    "literal-ends-at-its-line"};
    fs::path const dir = root.path() / std::string{tag};
    fs::create_directories(dir);
    { std::ofstream(dir / std::string{auxName}, std::ios::binary) << auxText; }
    fs::path const mainPath = dir / "main.c";
    { std::ofstream(mainPath, std::ios::binary) << mainText; }
    PPRun out;
    auto buf = SourceBuffer::fromFile(mainPath);
    std::vector<fs::path> noDirs;
    out.result = preprocess(buf, cLanguage(), noDirs, kDefaultHeaderNameMatching,
                            DiagnosticBudget::libraryDefault());
    for (Token const& t : out.result.tokens) {
        if (t.coreKind == CoreTokenKind::Eof || t.coreKind == CoreTokenKind::Whitespace
            || t.coreKind == CoreTokenKind::Newline) {
            continue;
        }
        out.lexemes.push_back(std::string{out.result.synthBuffer->slice(t.span)});
    }
    return out;
}

}  // namespace

TEST(InterpretedOperand, AnUnclosedQuoteIncludeIsRefusedEvenWhenTheFileExists) {
    PPRun const unclosed = ppRunOnDisk("inc-open", "#include \"hdr.h\nint x = H;\n",
                                       "hdr.h", "#define H 42\n");
    EXPECT_TRUE(has(unclosed, DiagnosticCode::P_PreprocessorDirective)) << everything(unclosed);
    EXPECT_TRUE(std::ranges::none_of(unclosed.lexemes, [](std::string const& s) {
        return s == "42";
    })) << "the header must NOT have been spliced";

    PPRun const closed = ppRunOnDisk("inc-closed", "#include \"hdr.h\"\nint x = H;\n",
                                     "hdr.h", "#define H 42\n");
    EXPECT_FALSE(closed.result.diagnostics->hasErrors()) << everything(closed);
    EXPECT_TRUE(std::ranges::any_of(closed.lexemes, [](std::string const& s) {
        return s == "42";
    })) << "the closed control splices its header";
}

TEST(InterpretedOperand, AnUnclosedEmbedNameIsRefusedEvenWhenTheFileExists) {
    PPRun const unclosed = ppRunOnDisk(
        "embed-open", "unsigned char const d[] = {\n#embed \"data.bin\n};\n",
        "data.bin", "ABC");
    EXPECT_TRUE(has(unclosed, DiagnosticCode::P_PreprocessorEmbed)) << everything(unclosed);

    PPRun const closed = ppRunOnDisk(
        "embed-closed", "unsigned char const d[] = {\n#embed \"data.bin\"\n};\n",
        "data.bin", "ABC");
    EXPECT_FALSE(closed.result.diagnostics->hasErrors()) << everything(closed);
}

TEST(InterpretedOperand, AnUnclosedLineFileOperandIsRefused) {
    PPRun const unclosed = ppRun(std::string{"#line 10 \"foo\n"} + std::string{kLiveTail});
    EXPECT_TRUE(has(unclosed, DiagnosticCode::P_PreprocessorDirective)) << everything(unclosed);

    PPRun const closed = ppRun(std::string{"#line 10 \"foo.c\"\n"} + std::string{kLiveTail});
    EXPECT_FALSE(closed.result.diagnostics->hasErrors()) << everything(closed);
    expectTailSurvived(closed);
}

// ════════════════════════════════════════════════════════════════════════════
// GROUP 3 — the conversion gate, for the other conversion diagnostics
// ════════════════════════════════════════════════════════════════════════════

TEST(ConversionGate, AMalformedNumberThatIsNeverConvertedIsNotJudged) {
    for (std::string_view const shape :
         {std::string_view{"#warning 0x1g is not a number\n"},
          std::string_view{"#define Z 0x1g\n"},
          std::string_view{"#if 1\n#elif 2d\n#endif\n"}}) {
        PPRun const r = ppRun(std::string{shape} + std::string{kLiveTail});
        EXPECT_FALSE(has(r, DiagnosticCode::P_MalformedNumber))
            << "`" << shape << "` never converts its number" << everything(r);
        expectTailSurvived(r);
    }
}

TEST(ConversionGate, AMalformedNumberThatIsConvertedIsStillRefused) {
    for (std::string_view const shape :
         {std::string_view{"#define Z 0x1g\nint z = Z;\n"},
          std::string_view{"#if 0\n#elif 2d\n#endif\n"},
          std::string_view{"#if 0 && 2d\n#endif\n"},
          std::string_view{"#define N 2d\n#if N\n#endif\n"}}) {
        PPRun const r = ppRun(std::string{shape} + std::string{kLiveTail});
        EXPECT_TRUE(has(r, DiagnosticCode::P_MalformedNumber))
            << "`" << shape << "` converts its number — expanded into code, in an "
               "evaluated `#elif`, in an operand the arithmetic skips, or through "
               "a macro an evaluated `#if` expands" << everything(r);
    }
}
