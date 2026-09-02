// D-MIRTEXT-UNSEALED-BLOCK-ABORTS-WHEN-NOTHING-SET-ERRORS — the `.dssir` twin
// of D-LIR-TEXT-PARSE-UNSEALED-BLOCK-ABORT, for the two shapes that emit NO
// diagnostic at all.
//
// `mir_text.cpp` already guards this class with `if (errors_) return;` before
// `beginBlock`, and it is correct for every DIAGNOSED malformation: a refused
// instruction sets `errors_`, so the next block never opens and the kill is
// avoided. ⚠ IT KEYS ON A DIAGNOSTIC HAVING BEEN EMITTED. Two malformed
// inputs emit none —
//
//   (a) a non-final block holding only well-formed non-terminators, and
//   (b) an empty non-final block —
//
// so `errors_` stayed false, `beginBlock` ran, and `MirBuilder::closeBlock_`
// killed the process with *"block MirBlockId=N has no terminator"*.
// ✔MEASURED 2026-08-28, both shapes, exit `0xc0000409`.
//
// ★★ ON NON-VACUITY, WHICH COST THREE WRONG PROBES HERE. The first three
// versions of case (a) PASSED — and every one of them passed for the wrong
// reason: `lit int 42` written without parentheses, then without its
// `: <core>` tag, each made the INSTRUCTION malformed, which set `errors_`
// and tripped the very guard the case exists to bypass. A fixture that is
// malformed in a way it did not intend measures something else entirely. So
// the fixture below is well-formed in every respect EXCEPT the missing
// terminator — `[entry]` marker included, without which verify-on-load
// objects instead — and each negative asserts the diagnostic it expects BY
// TEXT rather than merely asserting that some diagnostic exists.
//
// RED-ON-DISABLE: delete the `currentlyOpenBlock().valid()` refusal at the end
// of `parseBlock` in `src/mir/mir_text.cpp` → both negatives below die at
// `MirBuilder::closeBlock_`, taking the runner with them.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "mir/mir_text.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace dss;

namespace {

// `body1` is spliced in as the whole body of `%b1`, which is NON-FINAL: `%b2`
// follows it and is always well-formed.
//
// ⚠ THE ORDER IS THE FIXTURE. An unsealed FINAL block never reaches the
// abort, because `finalize()` short-circuits before `finish()`; only the
// transition to a NEXT block calls `beginBlock` and therefore `closeBlock_`.
[[nodiscard]] std::string doc(std::string_view body1) {
    return std::string{
        "dssir 1\n"
        "symbols {\n"
        "  %1 \"main\"\n"
        "}\n"
        "module {\n"
        "  function %1 : fn() -> void {\n"
        "    block %b1 [entry] {\n"} +
        std::string{body1} +
        "    }\n"
        "    block %b2 {\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "}\n";
}

[[nodiscard]] std::string diagText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) out += "[" + d.actual + "] ";
    if (out.empty()) out = "(no diagnostics)";
    return out;
}

// Did the unsealed-block refusal itself fire? This is the NON-VACUITY check:
// it separates "the parse failed because the block lost its terminator" from
// "the parse failed because this test's fixture was malformed some other
// way", which is how three earlier drafts of case (a) passed.
[[nodiscard]] bool sawUnsealedRefusal(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::I_TextMalformed
            && d.actual.find("closes without a terminator") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(MirTextUnsealedBlock, ANonFinalBlockHoldingOnlyNonTerminatorsIsRefusedNotAborted) {
    // Every instruction here is well-formed. The BLOCK is not: it never
    // branches. Before the fix this reported NOTHING and killed the process.
    DiagnosticReporter rep;
    auto res = parseMir(doc("      %v1 = const : i32 (lit int 42 : i32)\n"),
                        CompilationUnitId{1}, rep);
    // Reaching this line at all is most of the assertion.
    EXPECT_FALSE(res->ok);
    EXPECT_TRUE(sawUnsealedRefusal(rep))
        << "nothing else in this input is malformed, so the unsealed-block "
           "refusal is the ONLY diagnostic that can legitimately appear — any "
           "other one means the fixture broke, not the parser: "
        << diagText(rep);
}

TEST(MirTextUnsealedBlock, AnEmptyNonFinalBlockIsRefusedNotAborted) {
    DiagnosticReporter rep;
    auto res = parseMir(doc(""), CompilationUnitId{1}, rep);
    EXPECT_FALSE(res->ok);
    EXPECT_TRUE(sawUnsealedRefusal(rep)) << diagText(rep);
}

TEST(MirTextUnsealedBlock, AWellFormedTwoBlockFunctionStillParsesGreen) {
    // CONTROL: byte-for-byte the same fixture with `%b1` sealed. Without it a
    // parser that refused every block would satisfy both negatives and be
    // useless — and it is what caught the missing `[entry]` marker that made
    // the first version of this fixture invalid for an unrelated reason.
    DiagnosticReporter rep;
    auto res = parseMir(doc("      br %b2\n"), CompilationUnitId{1}, rep);
    EXPECT_TRUE(res->ok) << diagText(rep);
    EXPECT_EQ(rep.errorCount(), 0u) << diagText(rep);
    EXPECT_FALSE(sawUnsealedRefusal(rep))
        << "a sealed block must never be reported unsealed";
}
