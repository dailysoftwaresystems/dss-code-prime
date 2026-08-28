// ★★★ [[D-PP-PASTE-REJECTS-A-VALID-PREPROCESSING-NUMBER]] — `##` VALIDITY IS A
// QUESTION ABOUT PREPROCESSING TOKENS, AND IT IS THE SCANNER'S TO ANSWER.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// C 6.10.3.3p3 requires only that a paste's result be a valid PREPROCESSING
// token. A preprocessing number (C 6.4.8) is a far wider grammar than any
// language's numeric literal: once a number starts it absorbs every following
// digit, identifier byte, fraction point and exponent-sign, and only phase 7
// asks whether the run converts to anything. `0y1`, `1e` and `1x` are all single
// valid pp-numbers.
//
// DSS decided paste validity by re-tokenizing with the PHASE 7 literal grammar
// and counting tokens. That grammar stops short, so `0 ## y1` came back as two
// tokens and the paste was REFUSED — DSS was stricter than gcc, than clang, AND
// than ISO C, which the bar forbids in the same breath as an unimplemented
// feature.
//
// ── THE FIX IS A RETIMING, NOT A RELAXATION, AND THAT IS WHAT THESE PIN ────
//
// The references do not accept `0y1` silently: they accept the PASTE and then
// diagnose the number. So the refusal MOVED from the preprocessor to the scanner
// that forms the pp-number. A test asserting only "no P_PreprocessorPaste" would
// be the relaxation trap wearing a pin's clothes, so every positive arm below
// ALSO asserts that the program is still refused, and by which code.
//
// ── THE REFERENCE MATRIX, PROBED SEPARATELY ────────────────────────────────
//
// ✔MEASURED, gcc 13.3.0 and clang 18.1.3, one TU per shape so no shape's verdict
// masks another's. Both REFUSE the paste for `. ## .`, `+ ## -`, `/ ## /`,
// `/ ## *` and `1 ## "x"` (gcc: `pasting "." and "." does not give a valid
// preprocessing token`; clang: `pasting formed '..', an invalid preprocessing
// token`). Both ACCEPT the paste for `0 ## y1`, `1 ## e` and `1 ## x` and then
// report an invalid integer suffix or an exponent with no digits. Both ACCEPT
// and compile clean `0 ## x1`, `1 ## 2u`, `1 ## e5`, `. ## 5`, `x ## y`,
// `x ## 1`, `+ ## +`, `- ## >`, `< ## <`, `& ## &`, `# ## #`, `L ## "s"`.
//
// ⚠ THE PROBE THAT MEASURED THIS FELL INTO A VACUOUS-GREEN TRAP FIRST, and the
// shape is worth recording: its negative arms substituted the invocation into a
// per-shape "use" string that, for exactly those arms, never mentioned the macro
// — so six shapes were reported ACCEPTED-CLEAN having never been expanded at all.
// A probe whose subject can be omitted is a probe that fails toward clean.

#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dss;

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded.has_value()) {
            // THROW, never abort: an abort kills the whole binary and every
            // sibling test loses its verdict (scripts/check-no-abort-in-tests).
            throw std::runtime_error{"loadShipped(c) failed"};
        }
        return *loaded;
    }();
    return schema;
}

// Preprocess one source and hand back the result, so the caller can ask which
// diagnostic CODES it produced. Driving `preprocess()` directly (rather than the
// CLI) keeps each arm's failure message about the paste and nothing else.
[[nodiscard]] PreprocessResult ppRun(std::string_view source) {
    auto buf = SourceBuffer::fromString(std::string{source}, "main.c");
    std::vector<std::filesystem::path> noDirs;
    return preprocess(buf, cLanguage(), noDirs, kDefaultHeaderNameMatching,
                      DiagnosticBudget::libraryDefault());
}

[[nodiscard]] bool hasCode(PreprocessResult const& r, DiagnosticCode code) {
    if (!r.diagnostics) return false;
    for (ParseDiagnostic const& d : r.diagnostics->all()) {
        if (d.code == code) return true;
    }
    return false;
}

// One TU that pastes `lhs` and `rhs` through a function-like macro and USES the
// product, so the invocation can never be omitted — the vacuous-probe guard the
// header note describes, enforced in code rather than remembered.
[[nodiscard]] std::string pasteTU(std::string_view lhs, std::string_view rhs) {
    std::string src = "#define CAT(a, b) a##b\nint f(void) { CAT(";
    src += lhs;
    src += ", ";
    src += rhs;
    src += ") ; return 0; }\n";
    return src;
}

// ── GROUP 1: the SCANNER, at the unit — the tier the refusal moved TO ──────

// Count the significant tokens one spelling scans to, in a named phase. This is
// the whole behavioural difference in one function.
struct ScanCounts {
    std::size_t significant = 0;
    bool        malformed   = false;
};

[[nodiscard]] ScanCounts scanAs(std::string_view spelling, Tokenizer::Phase phase) {
    auto buf = SourceBuffer::fromString(std::string{spelling}, "frag");
    Tokenizer tk{buf, cLanguage(), DiagnosticBudget::libraryDefault(),
                 LexerModeId{}, phase};
    auto res = std::move(tk).tokenize();
    ScanCounts out;
    while (!res.stream.isAtEnd()) {
        Token const t = res.stream.advance();
        if (t.coreKind == CoreTokenKind::Eof) break;
        // Trivia is not a preprocessing token; the paste's own count skips it
        // the same way, so the two questions stay the same question.
        if (t.coreKind == CoreTokenKind::Whitespace
            || t.coreKind == CoreTokenKind::LineComment
            || t.coreKind == CoreTokenKind::BlockComment
            || t.coreKind == CoreTokenKind::Newline) {
            continue;
        }
        ++out.significant;
    }
    if (res.diagnostics) {
        for (ParseDiagnostic const& d : res.diagnostics->all()) {
            if (d.code == DiagnosticCode::P_MalformedNumber) out.malformed = true;
        }
    }
    return out;
}

TEST(PreprocessingNumberScan, PhaseSevenStopsAtTheLiteralGrammar) {
    // The phase-7 scan is what every ordinary parse uses, and it is deliberately
    // UNCHANGED: `0y1` is a number then an identifier.
    const auto s = scanAs("0y1", Tokenizer::Phase::Tokens);
    EXPECT_EQ(s.significant, 2u)
        << "phase 7 scans the language's literal grammar and stops where it "
           "stops; changing that would re-lex every source file in the corpus";
    EXPECT_FALSE(s.malformed);
}

TEST(PreprocessingNumberScan, PhaseThreeAbsorbsTheMaximalPreprocessingNumber) {
    for (auto const* spelling : {"0y1", "1e", "1x", "1zz9"}) {
        const auto s = scanAs(spelling, Tokenizer::Phase::PreprocessingTokens);
        EXPECT_EQ(s.significant, 1u)
            << spelling << " is a single preprocessing number (C 6.4.8)";
        EXPECT_TRUE(s.malformed)
            << spelling << " is not a literal this language accepts, so the ONE "
                           "token must be flagged malformed — never split, and "
                           "never silently accepted";
    }
}

TEST(PreprocessingNumberScan, PhaseThreeLeavesAValidLiteralAlone) {
    // A run the literal grammar DOES accept must not acquire a malformed flag
    // just because phase 3 looked at it.
    for (auto const* spelling : {"0x1", "1e5", "12u", ".5", "0"}) {
        const auto s = scanAs(spelling, Tokenizer::Phase::PreprocessingTokens);
        EXPECT_EQ(s.significant, 1u) << spelling;
        EXPECT_FALSE(s.malformed)
            << spelling << " is a valid literal; phase 3 must not invent a "
                           "defect in it";
    }
}

TEST(PreprocessingNumberScan, PhaseThreeDoesNotSwallowANonNumberNeighbour) {
    // The tail rule starts only once a NUMBER has started. `+-` and `..` are two
    // tokens in both phases, which is what keeps the paste refusing them.
    for (auto const* spelling : {"+-", "..", "x y"}) {
        const auto s = scanAs(spelling, Tokenizer::Phase::PreprocessingTokens);
        EXPECT_GT(s.significant, 1u)
            << spelling << " is not one preprocessing token in any phase";
    }
}

// ── GROUP 2: the PASTE — accepted, and still refused, in the right places ──

// ★ THE RETIMING ITSELF. Not "no paste error" — that alone is the relaxation
// trap. The paste is accepted AND the program is still refused, by the scanner.
TEST(PastePreprocessingToken, APreprocessingNumberIsPastedAndThenDiagnosed) {
    struct Arm { char const* lhs; char const* rhs; };
    for (Arm const arm : {Arm{"0", "y1"}, Arm{"1", "e"}, Arm{"1", "x"}}) {
        auto const r = ppRun(pasteTU(arm.lhs, arm.rhs));
        EXPECT_FALSE(hasCode(r, DiagnosticCode::P_PreprocessorPaste))
            << arm.lhs << " ## " << arm.rhs
            << " forms a single preprocessing number, which C 6.10.3.3p3 permits "
               "and which gcc and clang both accept";
        EXPECT_TRUE(hasCode(r, DiagnosticCode::P_MalformedNumber))
            << arm.lhs << " ## " << arm.rhs
            << " must still be REFUSED — by the tier that forms the pp-number. A "
               "paste that merely stopped complaining would let a malformed "
               "token through in silence, which is worse than the refusal it "
               "replaced";
        ASSERT_TRUE(r.diagnostics != nullptr);
        EXPECT_TRUE(r.diagnostics->hasErrors())
            << "the translation unit must not become acceptable";
    }
}

// ★ THE OTHER HALF, AND IT IS THE HALF A RELAXATION WOULD BREAK. A spelling that
// genuinely cannot be one preprocessing token is still refused at the paste.
TEST(PastePreprocessingToken, AnIllFormedPasteIsStillRefusedAtThePaste) {
    struct Arm { char const* lhs; char const* rhs; };
    for (Arm const arm : {Arm{".", "."}, Arm{"+", "-"}, Arm{"/", "/"},
                          Arm{"/", "*"}, Arm{"1", "\"x\""}}) {
        auto const r = ppRun(pasteTU(arm.lhs, arm.rhs));
        EXPECT_TRUE(hasCode(r, DiagnosticCode::P_PreprocessorPaste))
            << arm.lhs << " ## " << arm.rhs
            << " is not a single preprocessing token, and BOTH references refuse "
               "it at the paste — so DSS must too";
    }
}

TEST(PastePreprocessingToken, ALegalPasteStaysClean) {
    struct Arm { char const* lhs; char const* rhs; };
    for (Arm const arm : {Arm{"0", "x1"}, Arm{"1", "2u"}, Arm{"1", "e5"},
                          Arm{".", "5"}, Arm{"x", "y"}, Arm{"x", "1"},
                          Arm{"L", "\"s\""}}) {
        auto const r = ppRun(pasteTU(arm.lhs, arm.rhs));
        EXPECT_FALSE(hasCode(r, DiagnosticCode::P_PreprocessorPaste))
            << arm.lhs << " ## " << arm.rhs << " is a legal paste";
        EXPECT_FALSE(hasCode(r, DiagnosticCode::P_MalformedNumber))
            << arm.lhs << " ## " << arm.rhs
            << " forms a literal this language accepts; flagging it malformed "
               "would be the retiming over-firing";
    }
}

// ── GROUP 3: [[D-PP-PASTE-PRODUCT-IS-RE-READ-AS-THE-PASTE-OPERATOR]] ───────
//
// A token this pass MINTED is not an operator, however it is spelled. Found
// while measuring the matrix above: `CAT(#, #)` pastes to the `##` spelling, and
// the collapse rescanned from its own product, read it as the operator, and
// reported a replacement list that is perfectly well formed as malformed.
TEST(PasteProductIsNotAnOperator, APastedHashHashIsATokenNotTheOperator) {
    auto const r = ppRun(pasteTU("#", "#"));
    EXPECT_FALSE(hasCode(r, DiagnosticCode::P_PreprocessorPaste))
        << "the product of `# ## #` is the `##` TOKEN; C 6.10.3.3p3 subjects it "
           "to further macro replacement, not to further `##` evaluation. gcc "
           "13.3.0 and clang 18.1.3 both form it and pass it through";
}

// ⚠ THE PROPERTY THE FIX MUST NOT COST: rescanning from the product is what
// makes a CHAIN work, and only the product's OPERATOR reading was removed.
TEST(PasteProductIsNotAnOperator, AChainedPasteStillCollapses) {
    auto const r = ppRun("#define CAT3(a, b, c) a##b##c\nint CAT3(v, a, r) = 1;\n"
                         "int main(void) { return var; }\n");
    ASSERT_TRUE(r.diagnostics != nullptr);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "`a##b##c` chains because the NEXT `##` is a replacement-list token, "
           "which the product flag leaves alone";
}

}  // namespace
