// D-LIR-TEXT-PARSE-UNSEALED-BLOCK-ABORT — a malformed instruction in a
// NON-FINAL block must be REFUSED, never answered with `std::abort()`.
//
// ★★★ WHY THIS FILE EXISTS RATHER THAN A FEW MORE CASES IN
// `test_lir_text.cpp`: the property under test is that the process SURVIVES.
// Every case here is an arrangement that used to kill the runner, so the
// failure mode of a regression is a dead binary with no gtest summary — not a
// polite `EXPECT` diff. Keeping them in one small target means a regression
// takes down one focused binary and names itself, instead of vaporising the
// 100-odd unrelated assertions in the main text-codec file.
//
// ★★ THE BLIND SPOT THIS CLOSES, STATED PRECISELY. `parseLir` reported the
// malformed instruction correctly and then kept driving `LirBuilder`. The
// block whose terminator the panic-mode skip had just consumed was left
// UNSEALED, and the next `parseBlock` called `beginBlock`, whose *"current
// block has no terminator"* fatal killed the process. It survived every
// malformed-input test in the suite because those tests all placed the bad
// instruction in the LAST block, where `finalize` short-circuits on `errors_`
// before `builder_.finish()` ever runs — the one arrangement that cannot
// reach the abort. `TheSameMalformationInTheFinalBlockWasAlwaysRefused` below
// pins that asymmetry so the blind spot cannot silently reopen.
//
// ★ FAIL LOUD IS NOT ABORT. A process-killing fatal on malformed INPUT is a
// crash on data the parser exists to reject. The required outcome is a
// diagnostic plus `ok == false`, which is what every case here asserts.
//
// RED-ON-DISABLE: delete the `if (!unterminatedBlock_ &&
// !builder_.openBlockIsTerminated())` refusal at the end of `parseBlock` in
// `src/lir/lir_text.cpp` → every `...InANonFinalBlock...` case below dies at
// `LirBuilder::beginBlock`'s fatal, taking the runner with it.
// `AWellFormedTwoBlockModuleStillParsesGreen` is the CONTROL: it stays green
// under that mutant, so a dead binary is attributable to the deleted refusal
// rather than to the mutant breaking the parser wholesale.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_text.hpp"

#include <gtest/gtest.h>

#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace dss;

namespace {

[[nodiscard]] std::shared_ptr<TargetSchema> shippedX86() {
    auto target = TargetSchema::loadShipped("x86_64");
    // ★ THROW, NEVER `std::abort()`. The sibling fixtures in
    // `test_lir_text.cpp` abort here and are grandfathered by
    // `check-no-abort-in-tests`' inventory; copying that idiom into a NEW file
    // is what the guard exists to stop, and it caught this one. It would also
    // have been self-refuting: every case in this file asserts that a refusal
    // may not kill the process, so a fixture that kills the process to report
    // a missing schema would take the whole binary down for the one reason the
    // file is arguing against. A throw is reported by GoogleTest as a failure
    // of the ONE test that hit it.
    if (!target) {
        throw std::runtime_error{"loadShipped(x86_64) failed"};
    }
    return *target;
}

// Two blocks, `^b0` FIRST and therefore NON-FINAL, `^b1` last and always
// well-formed. `body0` is spliced in as the whole body of `^b0`.
//
// ⚠ THE ORDER IS THE ENTIRE FIXTURE. Put the same `body0` in `^b1` instead
// and every case here passes without the fix, because the abort lives on the
// transition to the NEXT block. `TheSameMalformationInTheFinalBlockWasAlwaysRefused`
// builds exactly that arrangement, to pin the asymmetry rather than fall into it.
[[nodiscard]] std::string doc(TargetSchema const& sch, std::string_view body0,
                             std::string_view body1 =
                                 "      ret rax ; payload=0 flags=0\n") {
    return std::format(
        "dsslir 1\n"
        "target {} version \"{}\"\n"
        "symbols {{\n  %1 \"main\"\n}}\n"
        "literal_pool {{}}\n"
        "module {{\n"
        "  function %1 \"main\" {{\n"
        "    block ^b0 [entry] -> [^b1] {{\n"
        "{}"
        "    }}\n"
        "    block ^b1 -> [] {{\n"
        "{}"
        "    }}\n"
        "  }}\n"
        "}}\n",
        sch.name(), std::string{sch.version()}, body0, body1);
}

// Shared verdict for every refusal case: the parse REPORTED and RETURNED.
// Reaching this function at all is most of the assertion — the pre-fix
// binary never got here.
void expectRefusedNotAborted(LirParseResult const& result,
                             DiagnosticReporter const& rep,
                             std::string_view what) {
    EXPECT_FALSE(result.ok)
        << what << ": a malformed non-final block must fail the parse";
    EXPECT_GT(rep.errorCount(), 0u)
        << what << ": a refusal with no diagnostic is the silent drop, not a "
                   "refusal";
    EXPECT_TRUE(result.symbolNames.empty())
        << what << ": `ok == false` owes an EMPTY result, never a "
                   "partially-built one";
}

// Did the end-of-block sealing refusal itself fire? Distinguishes "the parse
// failed for some other reason" from "the block was caught unsealed", which
// matters for the cases whose only defect IS the missing terminator.
[[nodiscard]] bool sawUnsealedBlockRefusal(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::I_TextMalformed
            && d.actual.find("without a terminator") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// ── the panic-mode family, each shape in a NON-FINAL block ─────────────────
//
// The five shapes below share one recovery path: report, then
// `skipToNextInstOrBlockEnd`. That is correct recovery for a non-terminator
// and drops the block's seal when the skipped instruction WAS the terminator.

TEST(LirTextUnsealedBlock, UnknownMnemonicInANonFinalBlockIsRefusedNotAborted) {
    auto sch = shippedX86();
    // The row's own probe. `bogusopcode` is not in the target's opcode table,
    // so the instruction is skipped and `^b0` never receives a terminator.
    std::string const text =
        doc(*sch, "      rax = bogusopcode #0 ; payload=0 flags=0\n");
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    expectRefusedNotAborted(*result, rep, "unknown mnemonic");
    bool sawUnknown = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::I_TextUnknownName) sawUnknown = true;
    }
    EXPECT_TRUE(sawUnknown)
        << "the ORIGINAL cause must still be named — the sealing refusal "
           "explains why the parse stopped, not what was wrong";
    EXPECT_TRUE(sawUnsealedBlockRefusal(rep))
        << "and the unsealed block must be reported in its own right";
}

TEST(LirTextUnsealedBlock, MissingPayloadTailInANonFinalBlockIsRefusedNotAborted) {
    auto sch = shippedX86();
    // The terminator's MANDATORY `payload=N` tail is absent, so `jmp` is
    // rejected mid-parse and skipped — terminator and all.
    std::string const text = doc(*sch, "      jmp ^b1 ; flags=0\n");
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    expectRefusedNotAborted(*result, rep, "missing payload= tail");
}

// ★★ ALREADY SAFE BEFORE THE FIX, AND THE REASON IS AN ACCIDENT — so this
// case is a CONTROL, not a pin, and is labelled so no reader counts it as
// coverage. ✔MEASURED under the sealing mutant: it PASSES, alone among the
// non-final shapes. `expectIdent` takes its token UNCONDITIONALLY before
// comparing, so the failed `flags` match swallows the block's closing brace;
// recovery then runs on past `^b1`'s header and body until it finds the next
// `;`, and the block that would have called `beginBlock` a second time has
// been consumed. The shape is not safe because the parser handles it — it is
// safe because its recovery ate the evidence. Its sibling above (`payload=`)
// differs only in WHICH token the failed `expectIdent` swallowed, and that
// one aborts.
//
// ⚠ It is kept because the shape is named in the row's family and a reader
// must be able to see that it was EXERCISED and found safe, rather than
// assume it was covered by the cases around it.
TEST(LirTextUnsealedBlock, MissingFlagsTailInANonFinalBlockIsRefusedNotAborted) {
    auto sch = shippedX86();
    std::string const text = doc(*sch, "      jmp ^b1 ; payload=0\n");
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    expectRefusedNotAborted(*result, rep, "missing flags= tail");
}

TEST(LirTextUnsealedBlock, MissingStatementSemicolonInANonFinalBlockIsRefusedNotAborted) {
    auto sch = shippedX86();
    // The `;` that opens the tail is gone. ⚠ This one does NOT take the
    // clean "expected `;`" path: `parseInst`'s 8-token lookahead finds the
    // `=` of `payload=0` with no `;` before it and reads the line as a
    // RESULT-form instruction, so recovery runs through `parseRegOperand`
    // first. The property is the same and that is the point — the unwind
    // must hold for the messy shapes too, not only the tidy one.
    std::string const text = doc(*sch, "      jmp ^b1 payload=0 flags=0\n");
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    expectRefusedNotAborted(*result, rep, "missing statement semicolon");
}

TEST(LirTextUnsealedBlock, NonIdentifierWhereTheMnemonicBelongsIsRefusedNotAborted) {
    auto sch = shippedX86();
    // `expected opcode mnemonic` — the sibling of the unknown-mnemonic arm,
    // reached when the token is not an identifier at all.
    std::string const text = doc(*sch, "      42 ; payload=0 flags=0\n");
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    expectRefusedNotAborted(*result, rep, "non-identifier mnemonic");
}

// ── the two shapes that were SILENT, not merely fatal ──────────────────────
//
// ★★ These are the worst of the family: the text is not malformed at the
// INSTRUCTION level at all, so the pre-fix parser emitted NO diagnostic
// whatsoever before killing the process. "Fail loud" had nothing to be loud
// with; the abort was the only output.

TEST(LirTextUnsealedBlock, ANonFinalBlockHoldingOnlyNonTerminatorsIsRefusedNotAborted) {
    auto sch = shippedX86();
    // Every instruction here is well-formed. The BLOCK is not: it declares a
    // successor and never branches to it.
    std::string const text = doc(*sch, "      rax = mov #42 ; payload=0 flags=0\n");
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    expectRefusedNotAborted(*result, rep, "block with no terminator");
    EXPECT_TRUE(sawUnsealedBlockRefusal(rep))
        << "nothing else in this input is wrong, so the sealing refusal is "
           "the ONLY diagnostic that can be emitted — without it the parse "
           "reported nothing and aborted";
}

TEST(LirTextUnsealedBlock, AnEmptyNonFinalBlockIsRefusedNotAborted) {
    auto sch = shippedX86();
    std::string const text = doc(*sch, "");
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    expectRefusedNotAborted(*result, rep, "empty non-final block");
    EXPECT_TRUE(sawUnsealedBlockRefusal(rep));
}

// ── the asymmetry that hid the defect ─────────────────────────────────────

TEST(LirTextUnsealedBlock, TheSameMalformationInTheFinalBlockWasAlwaysRefused) {
    auto sch = shippedX86();
    // ^b0 is well-formed and ^b1 — the LAST block — carries the bad
    // instruction. This arrangement was ALWAYS safe: `finalize` sees
    // `errors_` and returns the empty result without calling
    // `builder_.finish()`, so `closeFunction_`'s fatal is never reached.
    // Every pre-existing malformed-input test in the suite has this shape,
    // which is exactly why none of them could see the defect.
    std::string const text =
        doc(*sch,
            "      jmp ^b1 ; payload=0 flags=0\n",
            "      rax = bogusopcode #0 ; payload=0 flags=0\n");
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    expectRefusedNotAborted(*result, rep, "malformation in the final block");
}

// ── control ───────────────────────────────────────────────────────────────

TEST(LirTextUnsealedBlock, AWellFormedTwoBlockModuleStillParsesGreen) {
    auto sch = shippedX86();
    // Byte-for-byte the fixture every case above corrupts, uncorrupted. It
    // pins that the sealing refusal fires on UNSEALED blocks only: a parser
    // that refused every block would satisfy all the negatives above and be
    // useless.
    std::string const text = doc(*sch, "      jmp ^b1 ; payload=0 flags=0\n");
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    EXPECT_TRUE(result->ok) << "the uncorrupted fixture must parse clean";
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(sawUnsealedBlockRefusal(rep))
        << "a sealed block must never be reported unsealed";
    // ⚠ `moduleFuncCount()`, NOT `funcCount()`: the latter is the ARENA's
    // node count and includes the slot-0 sentinel, so it reads 2 for a
    // one-function module and `funcAt` refuses the index it implies.
    ASSERT_EQ(result->lir.moduleFuncCount(), 1u);
    LirFuncId const fn = result->lir.funcAt(0);
    EXPECT_EQ(result->lir.funcBlockCount(fn), 2u)
        << "both blocks must survive — the negatives are meaningless if the "
           "positive never built the second block in the first place";
}

// ── the sibling shape: a block that never OPENS ───────────────────────────

// D-LIR-TEXT-PARSE-BLOCK-HEADER-WITHOUT-BODY-BRACE-ABORT
// — the sibling reached from a malformed block HEADER rather than a
// malformed instruction, killing the process at a DIFFERENT builder guard.
//
// RED-ON-DISABLE: drop the `markUnterminatedBlock()` from the
// `expect(TokKind::LBrace)` failure arm of `parseBlock` → this case dies at
// `closeFunction_`'s *"block created but never `beginBlock`'d"* fatal.
TEST(LirTextUnsealedBlock, ABlockHeaderWithNoBodyBraceIsRefusedNotAborted) {
    auto sch = shippedX86();
    // ^b0's body brace is a stray `;`. `scanBlockHeaders` had already minted a
    // `LirBlockId` for it, so the block is created and never `beginBlock`'d —
    // `closeFunction_`'s OTHER fatal, *"block created but never
    // `beginBlock`'d"*, reached from a malformed block HEADER rather than a
    // malformed instruction, and fired at a different guard than the
    // unsealed-block half.
    //
    // ★★ A STRAY `;` RATHER THAN A DELETED `{`, AND THE DIFFERENCE IS THE
    // WHOLE TEST. ✔MEASURED: with the brace simply DELETED this case passes
    // with the fix removed. Deleting it desynchronises `scanBlockHeaders`'
    // brace count, the module loop's panic skip then breaks at the first `}`,
    // `function %2` is never reached, `closeFunction_` never runs a second
    // time and nothing aborts. Substituting a token keeps every brace paired,
    // so the parse walks on to the second function and reaches the fatal. An
    // "obviously equivalent" corruption was not equivalent at all.
    //
    // ⚠ THE SECOND FUNCTION IS LOAD-BEARING, for the same reason block ORDER
    // is above: the fatal fires inside the `addFunction` that closes the
    // previous one. Delete `function %2` and this input is harmless — the
    // identical asymmetry that hid the unsealed-block half.
    std::string const text = std::format(
        "dsslir 1\n"
        "target {} version \"{}\"\n"
        "symbols {{\n  %1 \"main\"\n  %2 \"second\"\n}}\n"
        "literal_pool {{}}\n"
        "module {{\n"
        "  function %1 \"main\" {{\n"
        "    block ^b0 [entry] -> [^b1] ;\n"
        "    block ^b1 -> [] {{\n"
        "      ret rax ; payload=0 flags=0\n"
        "    }}\n"
        "  }}\n"
        "  function %2 \"second\" {{\n"
        "    block ^b0 [entry] -> [] {{\n"
        "      ret rax ; payload=0 flags=0\n"
        "    }}\n"
        "  }}\n"
        "}}\n",
        sch->name(), std::string{sch->version()});
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    expectRefusedNotAborted(*result, rep, "block header with no body brace");
}

TEST(LirTextUnsealedBlock, TheTwoFunctionControlStillParsesGreen) {
    auto sch = shippedX86();
    // The same two-function fixture with ^b0's brace restored. Pins that the
    // negative above is caused by the missing brace and not by the second
    // function, and that abandoning a parse on a malformed header did not
    // become the parser's answer to well-formed multi-function input.
    std::string const text = std::format(
        "dsslir 1\n"
        "target {} version \"{}\"\n"
        "symbols {{\n  %1 \"main\"\n  %2 \"second\"\n}}\n"
        "literal_pool {{}}\n"
        "module {{\n"
        "  function %1 \"main\" {{\n"
        "    block ^b0 [entry] -> [^b1] {{\n"
        "      jmp ^b1 ; payload=0 flags=0\n"
        "    }}\n"
        "    block ^b1 -> [] {{\n"
        "      ret rax ; payload=0 flags=0\n"
        "    }}\n"
        "  }}\n"
        "  function %2 \"second\" {{\n"
        "    block ^b0 [entry] -> [] {{\n"
        "      ret rax ; payload=0 flags=0\n"
        "    }}\n"
        "  }}\n"
        "}}\n",
        sch->name(), std::string{sch->version()});
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    EXPECT_TRUE(result->ok) << "the uncorrupted two-function fixture must "
                               "parse clean";
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(result->lir.moduleFuncCount(), 2u);  // see the sentinel note above
}
