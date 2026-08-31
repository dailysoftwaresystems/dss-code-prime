// ★★★ [[D-PP-SKIPPED-CONDITIONAL-GROUP-VALIDATED-AS-A-PHASE-7-NUMBER]] — A
// SKIPPED GROUP IS DIVIDED INTO PREPROCESSING TOKENS AND NOT OTHERWISE
// PROCESSED.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// C 6.10.1p6: the text of a skipped conditional group is divided into
// preprocessing tokens, and the remaining preprocessing tokens "are otherwise
// ignored". C 6.4.8: `2d`, `1e` and `0x1e+2` are single, perfectly valid
// PREPROCESSING NUMBERS. They convert to no numeric literal, so they are errors
// in phase 7 — and phase 7 never runs on text that was skipped.
//
// DSS emitted `P_MalformedNumber` for them anyway, refusing the whole
// translation unit. That is where the entire sqlite corpus stopped, on every
// host and both artefacts: upstream `src/printf.c` keeps a Tcl script inside
// `#if 0 … #endif`, and one of its lines contains `%2d`.
//
// ── WHY THE FIX IS ONE LINE IN A GATE THAT ALREADY EXISTED ─────────────────
//
// The gate was already there and already correct — it just named ONE diagnostic
// code instead of the CLASS the code belongs to. `P_IllegalChar` inside `#if 0`
// had been suppressed since c17 by exactly this byte-liveness oracle, with a
// comment reading "ALL other tokenizer diagnostics forward unconditionally".
// The rule is not about illegal characters: it is about which TRANSLATION PHASE
// a scanner diagnostic belongs to. A judgement phase 7 makes while CONVERTING a
// preprocessing token cannot apply to a token that is never converted; a
// judgement phase 3 makes while DECOMPOSING text into preprocessing tokens
// applies to every byte of the file, skipped or not.
//
// So the tier is now a property OF THE DIAGNOSTIC CODE
// (`isTokenConversionDiagnostic`, core/types/parse_diagnostic), and the gate
// asks the class. A third conversion diagnostic inherits the rule by classifying
// itself — which is the whole reason this is not a second copy of the c17 rule
// spelled with a second code name.
//
// ── THE REFERENCE MATRIX, PROBED SEPARATELY ────────────────────────────────
//
// ✔MEASURED 2026-08-31, gcc 13.3.0 and clang 18.1.3, one TU per shape, every one
// carrying a positive control that compiled in the same run. BOTH ACCEPT, inside
// a skipped group: `2d`, `1e`, `0x1e+2`, a stray `@`, an unterminated `'`, and
// `"\q"`. BOTH REFUSE, live: `int x = 2d;`, `int x = 0x1e+2;`, `#if 2d`,
// `#if 0 && 2d` and `#if 1 ? 1 : 2d` — a `#if` controlling expression IS a
// conversion context (C 6.10.1p4) even where the operand is not evaluated. And
// BOTH REFUSE an unterminated COMMENT inside `#if 0` — comments are replaced in
// phase 3 regardless of conditional inclusion, so that one is a DECOMPOSITION
// failure and must stay loud.
//
// ⓘ `"\q"` IS NOT A USABLE WITNESS HERE and the first draft of this file used it
// vacuously. DSS's scanner is deliberately permissive about escape CONTENT — the
// escape is decoded in lowering — so `P_InvalidEscape` never fired for it in
// either position, and an EXPECT_FALSE passed for a reason that had nothing to
// do with the fix. The code fires in exactly one situation, an escape lead that
// is the LAST byte of the source, and that is what the class arm now drives.
//
// ⚠ EVERY POSITIVE ARM BELOW IS PAIRED WITH ITS REFUSAL. A file asserting only
// "no P_MalformedNumber in a skipped group" passes just as green over a
// validator that has been deleted outright, which trades a false positive for a
// silent miscompile. Nothing here is provable from the other half.
//
// ── AND ONE MORE DEFECT THE SWEEP FOUND, IN THE SAME TRANSFORM ─────────────
//
// The pp-number tail's exponent-letter test read only `integerPrefixes[].float`,
// where C declares `p`/`P`. C's DECIMAL exponent letters `e`/`E` live in the
// separate top-level `numberStyle.exponent`, so `+`/`-` never continued a run
// ending in `e` — and `0x1e+2` scanned as THREE tokens and BUILT AN ARTIFACT
// computing 32. ✔MEASURED: gcc 13.3.0 and clang 18.1.3 both refuse `0x1e+2`,
// `0xFE+1` and `0x1e-2` (`invalid suffix "+2" on integer constant`), and both
// accept `0x1a+2`, `12+3` and `0x1p+3`. That is the ONLY member of this family
// that failed toward a WRONG ANSWER rather than toward a visible refusal, so it
// is fixed here too, and pinned in both directions below.

#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
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

// Preprocess one source and hand back BOTH the diagnostics and the surviving
// lexemes. The lexemes are what make an accept arm strict: "no diagnostic" is
// satisfied by a preprocessor that dropped the live code too.
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
        out.lexemes.push_back(
            std::string{out.result.synthBuffer->slice(t.span)});
    }
    return out;
}

[[nodiscard]] bool hasCode(PPRun const& r, DiagnosticCode code) {
    if (!r.result.diagnostics) return false;
    for (ParseDiagnostic const& d : r.result.diagnostics->all()) {
        if (d.code == code) return true;
    }
    return false;
}

// The live tail every accept arm carries, so "nothing was reported" can never be
// satisfied by "nothing was preprocessed".
constexpr std::string_view kLiveTail = "int survivor;\n";

void expectLiveTailSurvived(PPRun const& r) {
    ASSERT_EQ(r.lexemes.size(), 3u)
        << "the live tail must survive intact — exactly `int survivor ;`";
    EXPECT_EQ(r.lexemes[0], "int");
    EXPECT_EQ(r.lexemes[1], "survivor");
    EXPECT_EQ(r.lexemes[2], ";");
}

// ── GROUP 1: the skipped group ACCEPTS every preprocessing number ──────────

TEST(SkippedGroupNotOtherwiseProcessed, PpNumbersInsideIfZeroAreNotConverted) {
    // Each spelling is a valid pp-number (C 6.4.8) that converts to no numeric
    // literal. `%2d` is the sqlite spelling verbatim — the `2d` is what DSS
    // stopped on inside `src/printf.c`'s `#if 0` Tcl script.
    for (auto const* dead : {"int x = 2d;", "1e", "0x1e+2",
                             "puts -nonewline [format %2d: $r]", "1zz9", "9y"}) {
        std::string src = "#if 0\n";
        src += dead;
        src += "\n#endif\n";
        src += kLiveTail;
        const PPRun r = ppRun(src);
        EXPECT_FALSE(hasCode(r, DiagnosticCode::P_MalformedNumber))
            << "`" << dead
            << "` inside #if 0 is a preprocessing number that is never "
               "converted (C 6.10.1p6); gcc 13.3.0 and clang 18.1.3 both accept";
        EXPECT_FALSE(r.result.diagnostics->hasErrors())
            << "`" << dead << "` inside #if 0 must produce no error at all";
        expectLiveTailSurvived(r);
    }
}

TEST(SkippedGroupNotOtherwiseProcessed, EverySkipFormSuppressesEqually) {
    // The rule is about the GROUP being skipped, not about which directive did
    // the skipping. A fix keyed on `#if 0` alone would leave the sqlite
    // cross-compile pattern (`#ifdef SQLITE_OS_WIN`) still broken.
    struct Form {
        char const* label;
        char const* source;
    };
    const Form forms[] = {
        {"#if 0", "#if 0\n2d\n#endif\n"},
        {"#ifdef of an undefined name",
         "#ifdef PP_NOT_DEFINED_ANYWHERE\n2d\n#endif\n"},
        {"#ifndef of a defined name",
         "#define PP_DEFINED 1\n#ifndef PP_DEFINED\n2d\n#endif\n"},
        {"the dead #else arm", "#if 1\n#else\n2d\n#endif\n"},
        {"the dead #elif arm", "#if 1\n#elif 1\n2d\n#endif\n"},
        {"a live-looking #if 1 nested in a dead #if 0",
         "#if 0\n#if 1\n2d\n#endif\n#endif\n"},
        {"a dead #define inside a dead group", "#if 0\n#define M 2d\n#endif\n"},
    };
    for (Form const& f : forms) {
        std::string src = f.source;
        src += kLiveTail;
        const PPRun r = ppRun(src);
        EXPECT_FALSE(hasCode(r, DiagnosticCode::P_MalformedNumber))
            << "a pp-number skipped via " << f.label
            << " must not be converted";
        EXPECT_FALSE(r.result.diagnostics->hasErrors())
            << "no error at all for " << f.label;
        expectLiveTailSurvived(r);
    }
}

// ── GROUP 2: the LOUDNESS that must survive the retiming ───────────────────

TEST(SkippedGroupNotOtherwiseProcessed, LivePpNumberIsStillRefused) {
    // The refusal is CORRECT and both references agree: this is the arm that
    // separates a retiming from a deletion.
    for (auto const* live : {"int x = 2d;", "int x = 1e;", "int x = 9y;",
                             "int x = 1zz9;"}) {
        std::string src = live;
        src += "\n";
        const PPRun r = ppRun(src);
        EXPECT_TRUE(hasCode(r, DiagnosticCode::P_MalformedNumber))
            << "`" << live
            << "` is LIVE: it reaches phase 7 and must still fail loud (gcc "
               "13.3.0 and clang 18.1.3 both refuse it)";
    }
}

TEST(PreprocessingNumberExponentTail, AHexRunEndingInEContinuesOnASign) {
    // ★ THE WRONG-ANSWER ARM. Before the exponent-letter union, DSS scanned
    // `0x1e+2` as `0x1e` `+` `2`, produced an ARTIFACT and computed 32 — a
    // program gcc 13.3.0 and clang 18.1.3 both refuse. A visible refusal can be
    // reported by whoever hits it; a wrong answer with exit code 0 cannot.
    for (auto const* live : {"int x = 0x1e+2;", "int x = 0xFE+1;",
                             "int x = 0x1e-2;", "int x = 0XE+1;"}) {
        std::string src = live;
        src += "\n";
        const PPRun r = ppRun(src);
        EXPECT_TRUE(hasCode(r, DiagnosticCode::P_MalformedNumber))
            << "`" << live
            << "` is ONE preprocessing number (C 6.4.8 continues a pp-number on "
               "an exponent letter followed by a sign) and converts to no "
               "literal";
    }
}

TEST(PreprocessingNumberExponentTail, APrefixedRunContinuesOnAnIdentifierByte) {
    // ★★ THE REACHABILITY ARM, and it is a different defect from the one above.
    // The phase-3 tail used to sit after the literal grammar's FINAL return,
    // and the PREFIX arm returns twice before ever getting there — so no run
    // entering through `0x`/`0b`/`0` reached the tail at all. Nothing about
    // exponent letters is involved here: `g`, `z` and `q` are plain identifier
    // bytes. ✔MEASURED: gcc 13.3.0 and clang 18.1.3 both refuse all three
    // ("invalid suffix"), and DSS scanned each as a number plus a separate
    // identifier — which also made `0x1 ## g` a paste DSS refused and both
    // references accept.
    for (auto const* live : {"int x = 0x1g;", "int x = 0b1z;", "int x = 0777q;"}) {
        std::string src = live;
        src += "\n";
        const PPRun r = ppRun(src);
        EXPECT_TRUE(hasCode(r, DiagnosticCode::P_MalformedNumber))
            << "`" << live
            << "` is ONE preprocessing number: the tail must be reachable from "
               "the PREFIX arm of the literal grammar, not only from the "
               "decimal one";
    }
}

TEST(PreprocessingNumberExponentTail, ASignAfterANonExponentByteStillSeparates) {
    // ★★ THE NEGATIVE CONTROL, and it is what makes the arm above a fix rather
    // than a blunt "a sign always continues a number". `a` is a hex DIGIT, not
    // an exponent letter, so `0x1a+2` is a number, a plus and a number — on all
    // three compilers. Without this pin the union could have been written as
    // "any `+` after any hex digit" and stayed green above while breaking every
    // ordinary arithmetic expression in the corpus.
    for (auto const* live : {"int x = 0x1a+2;", "int x = 12+3;",
                             "int x = 1e5+2;", "int x = 0x1p+3 == 8.0;",
                             "unsigned x = 0xFFul;", "int x = 0b1010;",
                             "int x = 0777;", "double d = 0x1.8p3;"}) {
        std::string src = live;
        src += "\n";
        const PPRun r = ppRun(src);
        EXPECT_FALSE(hasCode(r, DiagnosticCode::P_MalformedNumber))
            << "`" << live
            << "` does not end in an exponent letter, so the sign starts a new "
               "token; gcc and clang both accept it";
        EXPECT_FALSE(r.result.diagnostics->hasErrors()) << live;
    }
}

TEST(SkippedGroupNotOtherwiseProcessed, LivePpNumberInsideALiveGroupIsStillRefused) {
    // A live `#if 1` body is not a skipped group. A too-broad suppression — one
    // keyed on "is there a conditional anywhere" rather than on the byte's
    // liveness — would silence this.
    const PPRun r = ppRun("#if 1\nint x = 2d;\n#endif\n");
    EXPECT_TRUE(hasCode(r, DiagnosticCode::P_MalformedNumber))
        << "a pp-number in the LIVE arm still reaches phase 7";
}

TEST(SkippedGroupNotOtherwiseProcessed, PpNumberInAControllingExpressionIsStillRefused) {
    // ★ A `#if` controlling expression IS a conversion context (C 6.10.1p4:
    // its preprocessing tokens are converted), and the controlling directive's
    // own line is deliberately OUTSIDE the dead byte range — the range opens at
    // the END of that line. ✔MEASURED: gcc and clang refuse all three, including
    // the two where the operand is not evaluated.
    for (auto const* line : {"#if 2d\n#endif\n", "#if 0 && 2d\n#endif\n",
                             "#if 1 ? 1 : 2d\n#endif\n"}) {
        const PPRun r = ppRun(std::string{line} + std::string{kLiveTail});
        EXPECT_TRUE(hasCode(r, DiagnosticCode::P_MalformedNumber))
            << "`" << line
            << "` converts its operands even when it does not evaluate them";
    }
}

// ── GROUP 3: the CLASS, not the code — and where the class ENDS ────────────

TEST(SkippedGroupNotOtherwiseProcessed, EveryConversionDiagnosticIsSuppressedAlike) {
    // ★ A SECOND CODE THAT ACTUALLY MOVES, so the gate is proven to read the
    // CLASS rather than a second code name. `P_InvalidEscape` fires in exactly
    // one situation — the escape lead is the LAST byte of the source (DSS is
    // otherwise permissive: `"\q"` is decided in lowering, not by the scanner,
    // and gcc and clang only warn) — so the fixture has to end mid-literal.
    //
    // ⚠ The dead arm is still REFUSED, by the unterminated conditional, and that
    // is the point: the file's loudness is unchanged, only the phase-7 judgement
    // about a byte inside the skipped group is gone.
    const std::string escapeAtEof = "char const *s = \"abc\\";
    const PPRun liveEscape = ppRun(escapeAtEof);
    EXPECT_TRUE(hasCode(liveEscape, DiagnosticCode::P_InvalidEscape))
        << "a LIVE escape lead at end-of-source still reaches phase 7";
    const PPRun deadEscape = ppRun("#if 0\n" + escapeAtEof);
    EXPECT_FALSE(hasCode(deadEscape, DiagnosticCode::P_InvalidEscape))
        << "the same bytes inside a skipped group are never converted — and "
           "this arm is what separates a class-keyed gate from one naming "
           "P_MalformedNumber a second time";
    EXPECT_TRUE(deadEscape.result.diagnostics->hasErrors())
        << "the file is still REFUSED (unterminated conditional): suppression "
           "moved one judgement, it did not make a broken file compile";

    const PPRun stray = ppRun("#if 0\n@ ` \n#endif\n" + std::string{kLiveTail});
    EXPECT_FALSE(hasCode(stray, DiagnosticCode::P_IllegalChar))
        << "the c17 behaviour must be preserved by the generalisation, not "
           "replaced by it";
    expectLiveTailSurvived(stray);
}

TEST(SkippedGroupNotOtherwiseProcessed, LiveIllegalCharIsStillRefused) {
    // The other direction for the code that already had this behaviour: a live
    // stray byte must still fail loud, so the widened class cannot be mistaken
    // for a widened dead REGION.
    const PPRun r = ppRun("#if 1\n@\n#endif\n" + std::string{kLiveTail});
    EXPECT_TRUE(hasCode(r, DiagnosticCode::P_IllegalChar))
        << "a stray byte in a LIVE arm still reaches phase 7";
}

TEST(SkippedGroupNotOtherwiseProcessed, UnterminatedCommentInASkippedGroupIsStillRefused) {
    // Phase 3 replaces each comment with one space BEFORE any group is skipped,
    // so an unterminated comment is a DECOMPOSITION failure, not a conversion
    // one. ✔MEASURED: gcc 13.3.0 and clang 18.1.3 BOTH refuse `#if 0` / `/*` /
    // `#endif`, while both accept every other shape in this file's accept arms.
    //
    // ⚠ THIS ARM PINS THE BEHAVIOUR, NOT THE CLASSIFICATION, and the first draft
    // of this comment claimed otherwise — that a "suppress everything inside a
    // dead branch" mutant would redden it. ✔MEASURED FALSE: that mutant (the
    // whole class test replaced by `true`) left this case GREEN, and the reason
    // is the SPAN. An unterminated frame is swept at end-of-buffer, and the dead
    // range is half-open ending at exactly that offset, so the byte gate never
    // contains it either way. `P_UnterminatedComment`'s `false` classification is
    // therefore correct and FUTURE-PROOFING rather than load-bearing today —
    // exactly like `P_UnterminatedString`'s `true` one, for the same reason. The
    // classification is pinned directly by `TokenConversionDiagnosticClass`
    // below, which a constant predicate DOES redden.
    const PPRun r = ppRun("#if 0\n/*\n#endif\n" + std::string{kLiveTail});
    EXPECT_TRUE(hasCode(r, DiagnosticCode::P_UnterminatedComment))
        << "an unterminated comment is a PHASE 3 failure and stays loud inside "
           "a skipped group — the tier classification must not swallow it";
}

// ── GROUP 4: the classification itself, at the unit ────────────────────────

TEST(TokenConversionDiagnosticClass, EveryTokenizerCodeIsClassifiedByMeasurement) {
    // The five codes the tokenizer can emit, each with the reference verdict
    // that placed it. Asserting the table directly means the tier cannot drift
    // silently: a future edit that reclassifies one has to come here and change
    // a line that names the measurement it would be contradicting.
    EXPECT_TRUE(isTokenConversionDiagnostic(DiagnosticCode::P_MalformedNumber));
    EXPECT_TRUE(isTokenConversionDiagnostic(DiagnosticCode::P_IllegalChar));
    EXPECT_TRUE(isTokenConversionDiagnostic(DiagnosticCode::P_InvalidEscape));
    EXPECT_TRUE(isTokenConversionDiagnostic(DiagnosticCode::P_UnterminatedString));
    EXPECT_FALSE(isTokenConversionDiagnostic(DiagnosticCode::P_UnterminatedComment))
        << "a comment is replaced in phase 3 whether or not its group is "
           "skipped, so an unterminated one is a decomposition failure";
}

TEST(TokenConversionDiagnosticClass, AnUnclassifiedCodeFailsTowardLoud) {
    // The default direction is the load-bearing half: a code nobody classified
    // is FORWARDED. A predicate that defaulted to `true` would silence every
    // future diagnostic inside a dead branch, which is a silent miscompile
    // arriving through an omission rather than through an edit.
    EXPECT_FALSE(isTokenConversionDiagnostic(DiagnosticCode::P_UnexpectedToken));
    EXPECT_FALSE(isTokenConversionDiagnostic(DiagnosticCode::P_PreprocessorPaste));
    EXPECT_FALSE(isTokenConversionDiagnostic(DiagnosticCode::None));
}

} // namespace
