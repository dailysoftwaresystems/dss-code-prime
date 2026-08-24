// ── D-LIR-ASM-OPERAND-MOVE-IS-CLASS-BLIND ──────────────────────────────────
// ── D-TARGET-NO-CROSS-CLASS-MOVE-VERB ──────────────────────────────────────
// ────────────────────────────────────────────────────────────────────────────
//
// ★★★ THE DEFECT THIS FILE PINS WAS A LIVE SILENT MISCOMPILE ON BOTH SHIPPED
// TARGETS, reachable from ordinary C with no mutant and no flag. The inline-asm
// lowering resolved ONE module-wide `MnemonicSlot::Mov` per asm BLOCK and used
// it to materialise EVERY operand, whatever register class that operand bound.
// `MnemonicSlot::Mov` is the INTEGER move; an operand bound to a floating-point
// or vector class got it anyway, and the encoder then wrote the register NUMBER
// into the wrong file.
//
// ✔MEASURED 2026-08-23 (cycle P28) at the CLI, `--config=release`, rc=0 and no
// diagnostic in both arms:
//
//   arm64 — `double y = x; __asm__("nop" : "+r"(y)); return y;`
//     DSS:  fmov d15, d0  /  mov x29, x15  /  nop  /  mov x0, x29
//     gcc:  fmov x0,  d0  /  nop           /  fmov d0, x0
//     `mov x29, x15` reads INTEGER register 15 while the value sits in d15 — a
//     different physical register. The function returns whatever x15 held.
//
//   x86_64 — `double y = v; __asm__("nop" : "+x"(y)); return y;`
//     DSS:  movaps %xmm0,%xmm13 / mov %r13,%r13 / nop / movaps %xmm13,%xmm0
//     The operand's own register is xmm13; the copy into it is the integer mov
//     on register number 13, a no-op on r13. The template reads a register
//     nothing ever wrote.
//
// ★ THE TWO ARMS ARE TWO DIFFERENT FAULTS THAT SHARED ONE ROOT, and the fix has
// a different shape for each:
//   (1) SAME CLASS, WRONG INSTRUCTION (the x86_64 `"x"` arm) — the copy now
//       goes through `classOp(cls, RegClassOp::Move)`, the lookup the rest of
//       this file already used and whose own comment says it exists to kill
//       "the silent class-blind miscompile". A verb that already existed; no
//       new vocabulary.
//   (2) DIFFERENT CLASSES (the arm64 `"r"`-with-a-double arm) — a cross-FILE
//       move is a distinct machine operation (`fmov x0, d0`, `movq %xmm0,
//       %rax`) and `registerClassOps` binds one move PER class, so there is no
//       slot to ask. It REFUSES, naming both classes.
//
// ⚠⚠ (2) IS A CONFORMANCE GAP, NOT A DESIGN. gcc compiles that template. The
// refusal is what the bar requires while the vocabulary is missing — a loud
// diagnostic beats the measured alternative, which is a wrong register quietly
// — and the gap is anchored at D-TARGET-NO-CROSS-CLASS-MOVE-VERB with this very
// template as its trigger. Do not read the refusal as the intended end state.
//
// ★ WHAT EACH ARM ASSERTS:
//   (A) THE CROSS-CLASS REFUSAL FIRES, and names both classes — a refusal that
//       does not say which two files it could not bridge tells the programmer
//       nothing actionable.
//   (B) THE SAME-CLASS COPY STILL COMPILES — the refusal must not swallow the
//       legal case, or (A) would be passing because everything refuses.
//   (C) THE INTEGER CASE IS UNTOUCHED — `"r"` with an `int` is the overwhelming
//       majority of real inline asm and must keep working byte-for-byte.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::test_support;

namespace {

// Every diagnostic the MIR→LIR step emitted, joined — the "what actually
// happened" half of a failure message.
[[nodiscard]] std::string summarize(LoweredLir const& r) {
    std::string s;
    for (auto const& d : r.lirReporter.all()) s += "\n  " + d.actual;
    return s.empty() ? std::string{"<no diagnostics>"} : s;
}

// ⚠ THE FRONT END MUST HAVE ACCEPTED THE SNIPPET. Without this, an arm that
// asserts "the lowering refused" would go green on a PARSE error and record
// nothing about the decision under test — the vacuous-red twin of a vacuous
// pass.
void requireFrontEndClean(LoweredLir const& r) {
    ASSERT_FALSE(r.model.hasErrors())
        << "the snippet did not get past semantic analysis, so nothing below "
           "is a statement about the LOWERING";
    ASSERT_FALSE(r.hirReporter.hasErrors());
    ASSERT_FALSE(r.mirReporter.hasErrors());
}

// Every instruction of the module, in block order.
[[nodiscard]] std::vector<LirInstId> allInsts(Lir const& lir) {
    std::vector<LirInstId> out;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const b = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
                out.push_back(lir.blockInstAt(b, ii));
            }
        }
    }
    return out;
}

} // namespace

// ── (A) THE CROSS-CLASS REFUSAL FIRES ───────────────────────────────────────
TEST(LirAsmOperandMoveClass, ABindingWhoseClassDiffersFromItsValueIsRefused) {
    auto r = lowerCToLir(
        R"(double f(double x){ double y = x; __asm__("nop" : "+r"(y)); return y; })",
        "arm64");
    requireFrontEndClean(r);
    EXPECT_TRUE(r.lirReporter.hasErrors())
        << "a `double` bound with \"r\" needs a cross-FILE move, which no "
           "target declares — emitting the integer move encodes the register "
           "NUMBER in the wrong file (✔MEASURED: `mov x29, x15` against a value "
           "in d15). Got:"
        << summarize(r);

    bool namedBothClasses = false;
    for (auto const& d : r.lirReporter.all()) {
        if (d.actual.find("class 'gpr'") == std::string::npos) continue;
        if (d.actual.find("class 'fpr'") == std::string::npos) continue;
        namedBothClasses = true;
    }
    EXPECT_TRUE(namedBothClasses)
        << "the refusal must name BOTH the class the constraint bound and the "
           "class the value lives in — those two names are the whole content of "
           "the diagnostic. Got:"
        << summarize(r);
}

// ── (B) THE SAME-CLASS COPY STILL COMPILES ──────────────────────────────────
//
// x86_64's `"x"` binds the `fpr` class, which is where a `double`'s value
// already lives — the legal case, and the one whose INSTRUCTION was wrong.
TEST(LirAsmOperandMoveClass, ABindingWhoseClassMatchesItsValueStillLowers) {
    auto r = lowerCToLir(
        R"(double f(double v){ double y = v; __asm__("nop" : "+x"(y)); return y; })",
        "x86_64");
    requireFrontEndClean(r);
    ASSERT_FALSE(r.lirReporter.hasErrors())
        << "the constraint's class and the value's class agree here, so this "
           "must lower — a refusal that also caught the legal case would make "
           "the arm above vacuous. Got:"
        << summarize(r);

    // ⚠⚠ "IT LOWERED" IS NOT THE CLAIM — THE INSTRUCTION IS. The defect this
    // file records lowered perfectly happily; it emitted `mov %r13,%r13` for an
    // xmm operand at rc=0. An arm that only checked for the absence of a
    // diagnostic would have been GREEN on the miscompile, which is precisely
    // the vacuous shape the bar forbids. So: the FPR-class move must appear,
    // and the target's universal integer `Mov` must never define an FPR
    // register.
    Lir const& lir = r.lir.lir;
    auto const fprMove =
        r.target->regClassOpOpcode(TargetRegClass::FPR, RegClassOp::Move);
    ASSERT_TRUE(fprMove.has_value())
        << "x86_64 must declare an fpr `move` — without it this arm asserts "
           "nothing about which instruction was chosen";
    auto const genericMov = r.target->opcodeByMnemonic("mov");
    ASSERT_TRUE(genericMov.has_value());
    ASSERT_NE(*fprMove, *genericMov)
        << "the two opcodes must be DISTINCT or the check below cannot tell "
           "the right instruction from the wrong one";

    bool sawFprMove = false;
    for (LirInstId const id : allInsts(lir)) {
        auto const op = lir.instOpcode(id);
        if (op == *fprMove) sawFprMove = true;
        if (op != *genericMov) continue;
        EXPECT_NE(lir.instResult(id).regClass(), LirRegClass::FPR)
            << "the target's INTEGER move defines a floating-point register — "
               "the encoder writes the register NUMBER into the integer file, "
               "which is the measured `mov %r13,%r13` against a value in xmm13";
    }
    EXPECT_TRUE(sawFprMove)
        << "no fpr-class move was emitted at all, so this arm did not observe "
           "the operand materialisation it exists to check";
}

// ── (C) THE INTEGER CASE IS UNTOUCHED ───────────────────────────────────────
TEST(LirAsmOperandMoveClass, AnIntegerOperandBoundWithRStillLowers) {
    for (char const* const t : {"x86_64", "arm64"}) {
        SCOPED_TRACE(t);
        auto r = lowerCToLir(
            R"(int f(int a){ int y = a; __asm__("nop" : "+r"(y)); return y; })", t);
        requireFrontEndClean(r);
    EXPECT_FALSE(r.lirReporter.hasErrors())
            << "`\"r\"` with an int is the overwhelmingly common shape and must "
               "keep lowering unchanged. Got:"
            << summarize(r);
    }
}
