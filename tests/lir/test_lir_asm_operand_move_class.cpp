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
//   (1) SAME CLASS, WRONG INSTRUCTION (the x86_64 `"x"` arm) — the copy goes
//       through the per-class move table, the lookup the rest of this file
//       already used and whose own comment says it exists to kill "the silent
//       class-blind miscompile". A verb that already existed; no new
//       vocabulary.
//   (2) DIFFERENT CLASSES (the arm64 `"r"`-with-a-double arm) — a cross-FILE
//       move is a distinct machine operation (`fmov x0, d0`, `movq %xmm0,
//       %rax`).
//
// ★★★ (2) USED TO REFUSE, AND D-TARGET-NO-CROSS-CLASS-MOVE-VERB CLOSED IT.
// The refusal was correct while the vocabulary had no slot — `registerClassOps`
// was indexed by ONE class, so the cross-product had nowhere to live — and this
// file's earlier arm (A) pinned that refusal with the note *"do not read the
// refusal as the intended end state"*. It is no longer the state at all:
// `registerClassOps` is now keyed by the class PAIR, both shipped targets
// declare the two off-diagonal rows out of opcodes that already shipped
// byte-pinned, and the template gcc compiles to `fmov x0, d0 / nop /
// fmov d0, x0` now compiles here and RUNS — ✔MEASURED by execution at rc=42 on
// x86_64 natively and on arm64 under qemu, debug and release
// (`examples/c/c_asm_cross_class_operand`).
//
// ★ WHAT EACH ARM ASSERTS:
//   (A) THE CROSS-CLASS COPY LOWERS TO THE DECLARED CROSS-CLASS MOVE, on BOTH
//       targets and in BOTH directions — and specifically NOT to either
//       class's own diagonal move, which is the wrong-file miscompile above.
//       "It lowered" is not the claim; WHICH INSTRUCTION is.
//   (A2) A CONSTRAINT WHOSE CLASS IS THE VALUE'S CLASS NEEDS NO MOVE AT ALL,
//       and an undeclared pair still RESOLVES TO NOTHING. ⚠ This arm used to
//       assert that arm64's `"w"` on a `double` was REFUSED, on a config where
//       `"w"` bound a second class over the SIMD&FP file; R1 of design A′
//       removed that second class, gcc compiles the template, and so must DSS.
//       The half that guards against "every unresolved pair silently picks a
//       neighbour" survives as the lookup assertion inside it.
//   (B) THE SAME-CLASS COPY STILL COMPILES — the diagonal must not be
//       collateral damage of generalizing the table.
//   (C) THE INTEGER CASE IS UNTOUCHED — `"r"` with an `int` is the overwhelming
//       majority of real inline asm and must keep working byte-for-byte.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_node.hpp"
#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
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

// ── (A) THE CROSS-CLASS COPY LOWERS TO THE DECLARED CROSS-CLASS MOVE ────────
TEST(LirAsmOperandMoveClass, ABindingWhoseClassDiffersFromItsValueUsesTheCrossClassMove) {
    for (char const* const t : {"arm64", "x86_64"}) {
        SCOPED_TRACE(t);
        // `"+r"` is BOTH directions in one operand: the value is materialised
        // fpr→gpr on the way in and carried gpr→fpr on the way out, so this one
        // snippet must exercise both off-diagonal cells.
        auto r = lowerCToLir(
            R"(double f(double x){ double y = x; __asm__("nop" : "+r"(y)); return y; })",
            t);
        requireFrontEndClean(r);
        ASSERT_FALSE(r.lirReporter.hasErrors())
            << "gcc 13.3.0 -O2 compiles this template (`fmov x0, d0 / nop / "
               "fmov d0, x0` on aarch64), so DSS must — one working reference "
               "makes the behaviour required. Got:"
            << summarize(r);

        auto const toGpr = r.target->regClassOpOpcode(
            TargetRegClass::FPR, TargetRegClass::GPR, RegClassOp::Move);
        auto const toFpr = r.target->regClassOpOpcode(
            TargetRegClass::GPR, TargetRegClass::FPR, RegClassOp::Move);
        ASSERT_TRUE(toGpr.has_value())
            << "the target declares no fpr->gpr move — this arm would then be "
               "asserting nothing about which instruction was chosen";
        ASSERT_TRUE(toFpr.has_value())
            << "the target declares no gpr->fpr move";
        auto const gprMove =
            r.target->regClassOpOpcode(TargetRegClass::GPR, RegClassOp::Move);
        auto const fprMove =
            r.target->regClassOpOpcode(TargetRegClass::FPR, RegClassOp::Move);
        ASSERT_TRUE(gprMove.has_value());
        ASSERT_TRUE(fprMove.has_value());
        // ⚠ THE DISCRIMINATING PRECONDITION. If a cross-class cell resolved to
        // the SAME opcode as a diagonal cell, every assertion below would pass
        // over the exact miscompile this file records — the register number
        // written into the wrong file.
        ASSERT_NE(*toGpr, *gprMove);
        ASSERT_NE(*toGpr, *fprMove);
        ASSERT_NE(*toFpr, *gprMove);
        ASSERT_NE(*toFpr, *fprMove);

        Lir const& lir = r.lir.lir;
        bool sawToGpr = false;
        bool sawToFpr = false;
        for (LirInstId const id : allInsts(lir)) {
            auto const op = lir.instOpcode(id);
            if (op == *toGpr) {
                sawToGpr = true;
                EXPECT_EQ(lir.instResult(id).regClass(), LirRegClass::GPR)
                    << "the fpr->gpr move must LAND in a gpr register";
            }
            if (op == *toFpr) {
                sawToFpr = true;
                EXPECT_EQ(lir.instResult(id).regClass(), LirRegClass::FPR)
                    << "the gpr->fpr move must LAND in an fpr register";
            }
            // The integer move must never define a floating register, and the
            // floating move must never define an integer one — the two shapes
            // of the wrong-file write.
            if (op == *gprMove) {
                EXPECT_NE(lir.instResult(id).regClass(), LirRegClass::FPR)
                    << "the target's INTEGER move defines a floating-point "
                       "register (the measured `mov x29, x15` against a value "
                       "in d15)";
            }
            if (op == *fprMove) {
                EXPECT_NE(lir.instResult(id).regClass(), LirRegClass::GPR)
                    << "the target's FLOATING move defines an integer register";
            }
        }
        EXPECT_TRUE(sawToGpr)
            << "the value never left the floating file, so the template's "
               "\"r\"-bound `%0` read a register nothing wrote";
        EXPECT_TRUE(sawToFpr)
            << "the value never came back out of the integer file, so the "
               "function returns whatever the floating register held";
    }
}

// ── (A2) THE `"w"` WITNESS IS GONE BECAUSE ITS CONFIG DEFECT WAS FIXED ──────
//
// ⚠⚠ THIS ARM USED TO BE `APairWithNoDeclaredRowIsRefusedNamingBothClasses`,
// AND IT ASSERTED A REFUSAL THAT IS NOW A CONFORMANCE DEFECT RATHER THAN A
// GUARANTEE. It lowered `double f(double x){ ... __asm__("nop" : "+w"(y)); }`
// on arm64 and required `hasErrors()`, on the reasoning that `"w"` bound class
// `vr` while a `double`'s value lives in `fpr`, that this lane deliberately
// declared no `vr`/`fpr` row because those two classes were two WIDTH VIEWS of
// ONE physical register file, and that a "move" between them would be a fake
// copy papering over a config defect. Every clause of that was TRUE, and the
// last one named the repair: declare the file ONCE.
//
// ★★★ THE REPAIR LANDED (R1 of design A′). `v0..v31` are class `fpr`, `"w"`
// binds `fpr` — the class a C floating value already lives in — and no `vr`
// register exists on either shipped target. So this template now COMPILES,
// which is what gcc does (`aarch64-linux-gnu-gcc 13.3.0 -O2` emits the `nop`
// and NO move at all), and the old arm would be asserting that DSS refuses a
// program a working reference accepts.
//
// ⚠ WHAT IS LOST, STATED RATHER THAN QUIETLY DROPPED. There is no longer ANY
// undeclared-but-reachable class pair on a shipped target — arm64 and x86_64
// each declare both off-diagonal rows over their only two populated classes —
// so the END-TO-END refusal (`declares no move from register class 'X' to
// register class 'Y'`) has no live witness in this file. Pinning it would need
// a MUTANT target with a row removed, which this test target cannot build (it
// does not link the JSON dependency `mutate_target_schema.hpp` needs). What is
// asserted instead, below, is the LOOKUP half — an undeclared pair still
// resolves to nothing rather than borrowing a neighbour — which is the input
// the refusal fires on, and which reds if a default is ever introduced.
TEST(LirAsmOperandMoveClass, AWConstraintOnADoubleNeedsNoCrossClassMoveAtAll) {
    auto r = lowerCToLir(
        R"(double f(double x){ double y = x; __asm__("nop" : "+w"(y)); return y; })",
        "arm64");
    requireFrontEndClean(r);
    ASSERT_FALSE(r.lirReporter.hasErrors())
        << "gcc 13.3.0 -O2 compiles this template and allocates the value to a "
           "SIMD&FP register with no move at all, so DSS must — one working "
           "reference makes the behaviour required. Got:"
        << summarize(r);

    // ★ THE CONSTRAINT AND THE VALUE ARE ONE CLASS, WHICH IS WHY NO MOVE IS
    // NEEDED. Read off the target rather than assumed: `"w"`'s declared class
    // must be the class an FP value lives in.
    auto const* w = r.target->asmConstraint("w");
    ASSERT_NE(w, nullptr) << "arm64 must declare the `w` constraint letter";
    ASSERT_TRUE(w->registerClass.has_value());
    EXPECT_EQ(*w->registerClass, TargetRegClass::FPR)
        << "`w` binds a class no C value ever lives in — that is "
           "D-TARGET-ARM64-W-CONSTRAINT-BINDS-A-CLASS-NO-C-VALUE-EVER-LIVES-IN, "
           "and it makes this template refuse by name while gcc compiles it";

    // ⚠⚠ "IT LOWERED" IS NOT THE CLAIM. A cross-class move here would be a
    // fake copy between two spellings of one physical register — the shape the
    // second class used to force. Neither off-diagonal move may appear.
    auto const toGpr = r.target->regClassOpOpcode(
        TargetRegClass::FPR, TargetRegClass::GPR, RegClassOp::Move);
    auto const toFpr = r.target->regClassOpOpcode(
        TargetRegClass::GPR, TargetRegClass::FPR, RegClassOp::Move);
    ASSERT_TRUE(toGpr.has_value() && toFpr.has_value());
    Lir const& lir = r.lir.lir;
    for (LirInstId const id : allInsts(lir)) {
        auto const op = lir.instOpcode(id);
        EXPECT_NE(op, *toGpr)
            << "a `\"w\"`-bound double was moved out of the SIMD&FP file — the "
               "value already lives there, so this is a copy between two names "
               "for one register";
        EXPECT_NE(op, *toFpr)
            << "a `\"w\"`-bound double was moved into the SIMD&FP file it was "
               "already in";
    }

    // The LOOKUP half of the refusal, kept alive: a pair the target does not
    // declare resolves to NOTHING. `vr` has no members on either shipped
    // target, so this is the surviving undeclared pair.
    EXPECT_EQ(r.target->regClassOpOpcode(TargetRegClass::FPR,
                                         TargetRegClass::VR, RegClassOp::Move),
              std::nullopt)
        << "an undeclared class pair resolved to an opcode — a neighbour's "
           "move would encode the register NUMBER in the wrong file";
    EXPECT_EQ(r.target->regClassOpOpcode(TargetRegClass::VR,
                                         TargetRegClass::FPR, RegClassOp::Move),
              std::nullopt);
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

// ── (D) THE CROSS-CLASS MOVE'S WIDTH FOLLOWS THE OPERAND'S OWN WIDTH ────────
//
// ★★★ THE ONE PROPERTY THE RUNNABLE CORPUS EXAMPLE CANNOT SEE, WHICH IS WHY IT
// IS PINNED HERE. ✔MEASURED: a 32-bit float sitting in an S/xmm register has
// ZEROES above bit 31 (the write that put it there cleared them), so a 64-bit
// cross-class move round-trips it CORRECTLY anyway — the wrong instruction
// produces the right answer, and no exit code can distinguish them. The
// distinction is real at the ENCODING: aarch64 elects FMOV Wd,Sn vs FMOV Xd,Dn
// and x86_64 MOVD vs MOVQ, which read a different number of bytes out of the
// source file. A within-a-file copy is width-agnostic on both targets
// (`movaps` copies the whole XMM, `fmov d,d` the D view), so this axis only
// exists off the diagonal — and it is why `emitAsmOperandMove` threads the
// operand's width instead of leaving it at the default.
TEST(LirAsmOperandMoveClass, TheCrossClassMoveTakesTheOperandsWidth) {
    struct Case { char const* src; std::uint8_t widthBits; };
    for (auto const& c : {
             Case{R"(float f(float x){ float y = x; __asm__("nop" : "+r"(y)); return y; })", 32},
             Case{R"(double f(double x){ double y = x; __asm__("nop" : "+r"(y)); return y; })", 64}}) {
        SCOPED_TRACE(static_cast<unsigned>(c.widthBits));
        for (char const* const t : {"arm64", "x86_64"}) {
            SCOPED_TRACE(t);
            auto r = lowerCToLir(c.src, t);
            requireFrontEndClean(r);
            ASSERT_FALSE(r.lirReporter.hasErrors()) << summarize(r);

            auto const toGpr = r.target->regClassOpOpcode(
                TargetRegClass::FPR, TargetRegClass::GPR, RegClassOp::Move);
            auto const toFpr = r.target->regClassOpOpcode(
                TargetRegClass::GPR, TargetRegClass::FPR, RegClassOp::Move);
            ASSERT_TRUE(toGpr.has_value());
            ASSERT_TRUE(toFpr.has_value());

            Lir const& lir = r.lir.lir;
            int seen = 0;
            for (LirInstId const id : allInsts(lir)) {
                auto const op = lir.instOpcode(id);
                if (op != *toGpr && op != *toFpr) continue;
                ++seen;
                EXPECT_EQ(lirInstWidthBits(lir.instFlags(id)), c.widthBits)
                    << "the cross-FILE move must run at the OPERAND's width — "
                       "the 64-bit form on a 32-bit value reads eight bytes out "
                       "of a four-byte one";
                // ⚠ AND THE TARGET MUST DECLARE A VARIANT AT THAT WIDTH, which
                // is the condition the assembler's election actually tests. A
                // flag naming a width no variant guards is
                // `A_NoMatchingEncodingVariant` one tier down — loud, but the
                // failure would surface far from its cause.
                auto const* info = r.target->opcodeInfo(op);
                ASSERT_NE(info, nullptr);
                bool electable = false;
                for (auto const& v : info->encoding.variants) {
                    if (v.guardWidthBits == 0
                        || v.guardWidthBits == c.widthBits) {
                        electable = true;
                    }
                }
                EXPECT_TRUE(electable)
                    << "no encoding variant of the cross-class move is guarded "
                       "at this width";
            }
            EXPECT_EQ(seen, 2)
                << "a `\"+r\"` operand emits BOTH directions — one copy in and "
                   "one out — so anything else means the arm did not observe "
                   "what it claims to";
        }
    }
}

// ── (E) THE TWO WIDTHS ARE DIFFERENT BYTES, NOT A COSMETIC DISTINCTION ──────
//
// Without this the arm above could be satisfied by a target whose two variants
// encode identically, and the whole width axis would be decoration.
TEST(LirAsmOperandMoveClass, TheCrossClassMoveDeclaresTwoDistinctWidthForms) {
    for (char const* const t : {"arm64", "x86_64"}) {
        SCOPED_TRACE(t);
        auto schema = TargetSchema::loadShipped(t);
        ASSERT_TRUE(schema.has_value());
        auto const& s = **schema;
        for (auto const pair : {std::pair{TargetRegClass::FPR, TargetRegClass::GPR},
                                std::pair{TargetRegClass::GPR, TargetRegClass::FPR}}) {
            auto const op = s.regClassOpOpcode(pair.first, pair.second,
                                               RegClassOp::Move);
            ASSERT_TRUE(op.has_value());
            auto const* info = s.opcodeInfo(*op);
            ASSERT_NE(info, nullptr);
            bool has32 = false;
            bool has64 = false;
            for (auto const& v : info->encoding.variants) {
                if (v.guardWidthBits == 32) has32 = true;
                if (v.guardWidthBits == 64) has64 = true;
            }
            EXPECT_TRUE(has32)
                << "no 32-bit form: a `float` bound to an integer constraint "
                   "would have no encoding at all";
            EXPECT_TRUE(has64)
                << "no 64-bit form: a `double` bound to an integer constraint "
                   "would have no encoding at all";
            EXPECT_EQ(info->encoding.variants.size(), 2u)
                << "exactly the two width forms, so neither arm above can be "
                   "satisfied by an unguarded catch-all variant";
        }
    }
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
