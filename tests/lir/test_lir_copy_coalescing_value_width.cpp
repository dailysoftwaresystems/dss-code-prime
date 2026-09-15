// D-LIR-COPY-COALESCING-ASKS-THE-REGISTERS-WIDTH-NOT-THE-VALUES.
//
// The copy-coalescing admission test used to be `copyWidth == the widest
// register this CLASS declares`. Every REGISTER-TO-REGISTER class move this
// pipeline emits carries the width-default flags (`lirInstWidthBits(0)` = 64)
// and both shipped targets declare their FP file 16 bytes wide, so the equality
// could never
// hold for an FP copy — on ANY target — and the same mismatch stopped
// `lir_peephole`'s R1 from deleting the identity FP copies the allocator left
// behind. The predicate now asks about the VALUE: a copy may be merged when it
// is at least as wide as every stated access to either of its ends, and the
// proof travels to `rewriteWithAllocation`, which does not emit the copy at all.
//
// What is pinned here, and why each pin cannot go vacuous:
//   * the PREMISE — the FP file really is wider than the copies the pipeline
//     emits (a config change that made them equal would silently turn every
//     other pin in this file into a test of nothing);
//   * the POSITIVE, through the REAL lowering on BOTH shipped targets — FP
//     copies are proved, and no identity class move survives the rewrite;
//   * the NEGATIVE, two-sided and hand-built — a WIDER stated access to either
//     end removes the proof, and the copy is then still emitted. The control is
//     the byte-identical module with the wide access narrowed, which IS proved.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_verifier.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace dss;
using dss::test_support::lowerCToLir;

namespace {

// The two shipped targets. Every claim in this file is a claim about the RULE,
// which is target-agnostic, so every pin that can run on both does.
constexpr std::array<char const*, 2> kShippedTargets{"x86_64", "arm64"};

[[nodiscard]] std::shared_ptr<TargetSchema> shipped(char const* name) {
    auto t = TargetSchema::loadShipped(name);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(" << name << ") failed";
        return nullptr;
    }
    return *t;
}

[[nodiscard]] std::uint16_t fprMove(TargetSchema const& sch) {
    auto const op = sch.regClassOpOpcode(TargetRegClass::FPR, RegClassOp::Move);
    EXPECT_TRUE(op.has_value())
        << "the target declares no FPR class move — every pin in this file "
           "would then be asserting about an opcode that cannot be emitted";
    return op.has_value() ? *op : 0;
}

// The widest register, in bits, the target declares for one class. Re-derived
// HERE rather than imported, because the point of the premise pin is to check
// the number the production predicate USED TO compare against.
[[nodiscard]] std::uint32_t widestClassRegisterBits(TargetSchema const& sch,
                                                    TargetRegClass cls) {
    std::uint32_t widest = 0;
    for (auto const& info : sch.registers()) {
        if (info.regClass != cls) continue;
        auto const bits = static_cast<std::uint32_t>(info.widthBytes) * 8u;
        if (bits > widest) widest = bits;
    }
    return widest;
}

struct CopyCensus {
    std::uint32_t total    = 0;  // instructions whose opcode IS the class move
    std::uint32_t identity = 0;  // …whose sole operand equals the result
};

[[nodiscard]] CopyCensus censusClassMoves(Lir const& lir, std::uint16_t movOp) {
    CopyCensus out;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        LirFuncId const fn = lir.funcAt(f);
        for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
            LirBlockId const blk = lir.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                LirInstId const inst = lir.blockInstAt(blk, i);
                if (lir.instOpcode(inst) != movOp) continue;
                auto const ops = lir.instOperands(inst);
                if (ops.size() != 1) continue;
                if (ops[0].kind != LirOperandKind::Reg) continue;
                ++out.total;
                LirReg const res = lir.instResult(inst);
                if (res.valid() && ops[0].reg.valid() && ops[0].reg == res)
                    ++out.identity;
            }
        }
    }
    return out;
}

// ★ THE PROBE IS PHI-SHAPED, AND THAT IS THE MEASURED REASON, NOT A STYLE
// CHOICE. This tier is MIR→LIR with NO optimizer, so a chain of plain
// assignments between doubles lowers to frame-slot traffic and mints ZERO
// register-to-register class moves — ✔MEASURED: a six-double rotate loop
// produced not one `fmov`/`movaps`, and the vacuity guard below is what said so.
// A phi DOES mint one (`mir_to_lir`'s phi-copy step resolves the mnemonic
// through `classOp(regClass, RegClassOp::Move)`, so an FP phi copies via
// `movaps`/`fmov` and never the GPR `mov`), which is why the probe is built out
// of conditional expressions. Deliberately self-contained: the fixture REFUSES a
// source carrying a bare prototype, so the probe may not call out.
constexpr char const* kFpShuffleSource =
    "double pick(double a, double b, double c, double d, int k) {\n"
    "    double x = k > 0 ? a : b;\n"
    "    double y = k > 1 ? c : d;\n"
    "    double z = k > 2 ? x : y;\n"
    "    double w = k > 3 ? z : x;\n"
    "    return x + y + z + w;\n"
    "}\n";

// ── THE HAND-BUILT NEGATIVE ─────────────────────────────────────────────────
//
// Three chained class moves in one block:
//     v1 = MOVE(pFpr0)      width 64
//     v2 = MOVE(v1)         width 64      ← the candidate
//     v3 = MOVE(v2)         width `tailWidthFlags`
//     ret v3
// The tail is the ONLY thing that moves between the two arms. At width 128 it
// states an access to `v2` wider than the copy that defines it, which is exactly
// the shape the value-width bound exists to refuse; at width 64 it does not.
//
// ⓘ WHY A CHAINED MOVE AND NOT A MEMORY OP. A wide FP store would carry a
// base-register operand and drag the memory-operand shape into a pin about
// widths; the class move names ONE register and states ONE width, so the arm
// that differs differs in nothing else.
[[nodiscard]] Lir buildChainedMoveFn(TargetSchema const& sch,
                                     std::uint8_t tailWidthFlags) {
    LirBuilder b{sch};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);

    std::uint16_t const mov = fprMove(sch);
    LirReg const p0 = makePhysicalReg(0, LirRegClass::FPR);
    LirReg const v1 = makeVirtualReg(1, LirRegClass::FPR);
    LirReg const v2 = makeVirtualReg(2, LirRegClass::FPR);
    LirReg const v3 = makeVirtualReg(3, LirRegClass::FPR);

    std::array<LirOperand, 1> const fromP0{LirOperand::makeReg(p0)};
    (void)b.addInst(mov, v1, fromP0, /*payload=*/0, /*flags=*/0);
    std::array<LirOperand, 1> const fromV1{LirOperand::makeReg(v1)};
    (void)b.addInst(mov, v2, fromV1, /*payload=*/0, /*flags=*/0);
    std::array<LirOperand, 1> const fromV2{LirOperand::makeReg(v2)};
    (void)b.addInst(mov, v3, fromV2, /*payload=*/0, tailWidthFlags);

    auto const retOp = sch.opcodeByMnemonic("ret");
    EXPECT_TRUE(retOp.has_value());
    std::array<LirOperand, 1> const retOps{LirOperand::makeReg(v3)};
    (void)b.addReturn(retOp.value_or(0), retOps);
    return std::move(b).finish();
}

} // namespace

// ── THE PREMISE ─────────────────────────────────────────────────────────────
//
// Every other pin here is about a copy that is NARROWER than the register
// holding it. If a target ever declared an FP file exactly as wide as the copies
// the pipeline emits, the defect would be unreachable on it and the pins below
// would pass while measuring nothing. Assert the premise so that day is loud.
TEST(LirCopyCoalescingValueWidth, TheFpFileIsWiderThanTheCopiesThePipelineEmits) {
    for (auto const* name : kShippedTargets) {
        auto sch = shipped(name);
        ASSERT_NE(sch, nullptr);
        EXPECT_EQ(widestClassRegisterBits(*sch, TargetRegClass::FPR), 128u)
            << name << ": the FP register file is no longer 16 bytes wide. "
               "The value-width pins in this file assume a copy can be "
               "NARROWER than its register; re-read them before adjusting "
               "this number.";
        // And the GPR file is 64, which is why the predecessor predicate looked
        // correct for years: integer copies satisfied `width == class width` by
        // coincidence, so the veto only ever bit the FP file.
        EXPECT_EQ(widestClassRegisterBits(*sch, TargetRegClass::GPR), 64u)
            << name << ": the GPR file width moved";

        // The copies themselves: emitted with the width-default flags, which
        // `lirInstWidthBits` decodes as 64 — never 128.
        auto lowered = lowerCToLir(kFpShuffleSource, sch);
        ASSERT_TRUE(lowered.lir.ok) << name;
        std::uint16_t const mov = fprMove(*sch);
        bool sawNarrowFpCopy = false;
        Lir const& lir = lowered.lir.lir;
        for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
            LirFuncId const fn = lir.funcAt(f);
            for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
                LirBlockId const blk = lir.funcBlockAt(fn, bi);
                for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                    LirInstId const inst = lir.blockInstAt(blk, i);
                    if (lir.instOpcode(inst) != mov) continue;
                    EXPECT_LT(lirInstWidthBits(lir.instFlags(inst)), 128)
                        << name << ": an FP class move now STATES the full "
                           "register width — the predecessor predicate would "
                           "have admitted it and this file's subject is gone";
                    sawNarrowFpCopy = true;
                }
            }
        }
        EXPECT_TRUE(sawNarrowFpCopy)
            << name << ": the probe lowered no FP class move at all, so every "
               "FP claim below is vacuous";
    }
}

// ── THE POSITIVE, THROUGH THE REAL LOWERING ─────────────────────────────────
TEST(LirCopyCoalescingValueWidth, AnFpCopyIsProvedAndTheRewriteNeverEmitsIt) {
    for (auto const* name : kShippedTargets) {
        auto sch = shipped(name);
        ASSERT_NE(sch, nullptr);
        auto lowered = lowerCToLir(kFpShuffleSource, sch);
        ASSERT_TRUE(lowered.lir.ok) << name;

        std::uint16_t const mov = fprMove(*sch);
        auto const before = censusClassMoves(lowered.lir.lir, mov);
        ASSERT_GT(before.total, 0u)
            << name << ": no FP class move reached regalloc — vacuous";

        LirLiveness const liveness = analyzeLiveness(lowered.lir.lir);
        DiagnosticReporter allocRep;
        LirAllocation const alloc = allocateRegisters(
            lowered.lir.lir, *sch, liveness, /*ccIndex=*/0, allocRep);
        ASSERT_TRUE(alloc.ok()) << name;

        // (1) THE PROOF REACHES FP COPIES. Under the predecessor predicate this
        // set could not contain a single one.
        std::uint32_t provedFpCopies = 0;
        for (auto const& f : alloc.perFunc) {
            for (auto const instV : f.coalescedCopyInsts) {
                if (lowered.lir.lir.instOpcode(LirInstId{instV}) == mov)
                    ++provedFpCopies;
            }
        }
        EXPECT_GT(provedFpCopies, 0u)
            << name << ": not one FP copy was proved width-safe — the value-"
               "width bound is refusing the whole class again";

        // (2) AND NOTHING IDENTITY SURVIVES. This is the emitted-code claim:
        // the allocator either kept the two ends apart or did not emit the copy.
        DiagnosticReporter rewriteRep;
        auto rewritten = rewriteWithAllocation(lowered.lir.lir, *sch, alloc,
                                               rewriteRep);
        ASSERT_TRUE(rewritten.ok) << name;
        auto const after = censusClassMoves(rewritten.lir, mov);
        EXPECT_EQ(after.identity, 0u)
            << name << ": " << after.identity << " identity FP class move(s) "
               "survived the rewrite. `lir_peephole`'s R1 cannot delete them "
               "either (they are narrower than the register), so every one "
               "reaches the emitted stream.";
        EXPECT_LT(after.total, before.total)
            << name << ": the rewrite emitted as many FP copies as it was "
               "given — coalescing removed none";

        // (3) The module is still well formed after an instruction vanished.
        DiagnosticReporter verifyRep;
        EXPECT_TRUE(verifyLirPostRegalloc(rewritten.lir, *sch, verifyRep))
            << name;
        DiagnosticReporter rebuildRep;
        EXPECT_TRUE(verifyLirRebuild(lowered.lir.lir, rewritten.lir,
                                     "rewrite", rebuildRep))
            << name << ": dropping the coalesced copy lost a module "
               "side-structure reference";
    }
}

// ── THE NEGATIVE, TWO-SIDED ─────────────────────────────────────────────────
//
// The census is the thing the admission test reads, so assert it directly
// rather than inferring it from which register the linear scan happened to pick.
TEST(LirCopyCoalescingValueWidth, AWiderStatedAccessRemovesTheProof) {
    for (auto const* name : kShippedTargets) {
        auto sch = shipped(name);
        ASSERT_NE(sch, nullptr);
        std::uint16_t const mov = fprMove(*sch);

        struct Arm {
            char const*  label;
            std::uint8_t tailFlags;
            std::uint8_t expectedBoundOnV2;
            bool         v2CopyIsProved;
        };
        std::array<Arm, 2> const arms{
            // THE CONTROL: every access to `v2` is 64 bits, so the 64-bit copy
            // that defines it covers the whole value.
            Arm{"control (tail width 64)", 0, 64, true},
            // THE SUBJECT: the tail STATES a 128-bit read of `v2`, which the
            // 64-bit copy defining it does not cover.
            Arm{"subject (tail width 128)", kLirInstFlagWidth128, 128, false},
        };

        for (auto const& arm : arms) {
            Lir const lir = buildChainedMoveFn(*sch, arm.tailFlags);
            LirLiveness const liveness = analyzeLiveness(lir);
            ASSERT_EQ(liveness.perFunc.size(), 1u) << name << " " << arm.label;

            auto const bound = lirMaxStatedAccessWidthBits(lir,
                                                           liveness.perFunc[0]);
            ASSERT_GT(bound.size(), 2u) << name << " " << arm.label
                << ": liveness never saw v2, so the bound cannot be asserted";
            EXPECT_EQ(bound[2], arm.expectedBoundOnV2)
                << name << " " << arm.label
                << ": the census does not see the tail's stated width";
            EXPECT_EQ(bound[1], 64)
                << name << " " << arm.label
                << ": v1 is only ever touched at 64 bits";

            DiagnosticReporter allocRep;
            LirFuncAllocation const alloc = allocateFuncRegisters(
                lir, *sch, liveness.perFunc[0], /*ccIndex=*/0, allocRep);
            ASSERT_TRUE(alloc.ok) << name << " " << arm.label;

            // Is the CANDIDATE (`v2 = MOVE(v1)`, the second instruction of the
            // entry block) carried to the rewrite as proved?
            LirFuncId const fn  = lir.funcAt(0);
            LirBlockId const blk = lir.funcBlockAt(fn, 0);
            ASSERT_GE(lir.blockInstCount(blk), 3u) << name << " " << arm.label;
            LirInstId const candidate = lir.blockInstAt(blk, 1);
            ASSERT_EQ(lir.instOpcode(candidate), mov)
                << name << " " << arm.label
                << ": the module shape moved — instruction 1 is not the copy";
            bool const proved = std::binary_search(
                alloc.coalescedCopyInsts.begin(),
                alloc.coalescedCopyInsts.end(), candidate.v);
            EXPECT_EQ(proved, arm.v2CopyIsProved)
                << name << " " << arm.label
                << ": a copy narrower than a stated access to its own result "
                   "must NOT be proved — dropping it would hand the reader the "
                   "source's old upper half instead of what the copy wrote";

            // ── AND THE EMITTED-CODE HALF, WITH THE HAZARD SHAPE FORCED ─────
            //
            // ★★★ "AN UNPROVED COPY IS NOT DROPPED" IS ONLY OBSERVABLE WHEN THE
            // TWO ENDS ACTUALLY SHARE A REGISTER, AND ✔MEASURED THE SCAN NEVER
            // PRODUCES THAT FOR THE UNPROVED ARM — it did not coalesce the copy,
            // so it had no reason to put the ends together. A pin written as
            // *"if the ends happen to coincide, then…"* is therefore a pin that
            // NEVER RUNS: green, and measuring nothing. So the shape is FORCED —
            // both ends are overwritten onto ONE physical register while the
            // proof list is left exactly as the coalescer produced it. That is
            // the only combination the rewrite's two halves can be told apart
            // by, and it is a legal allocation to hand it (the two ranges abut).
            LirFuncAllocation forced = alloc;
            auto const* aSrc = alloc.forVReg(1);
            ASSERT_NE(aSrc, nullptr) << name << " " << arm.label;
            ASSERT_FALSE(aSrc->isSpilled()) << name << " " << arm.label;
            LirReg const shared = aSrc->physReg();
            ASSERT_GT(forced.assignments.size(), 2u) << name << " " << arm.label;
            forced.assignments[2] = LirRegAssignment::makePhys(
                makeVirtualReg(2, LirRegClass::FPR), shared);

            DiagnosticReporter rewriteRep;
            LirAllocation whole;
            whole.perFunc.push_back(forced);
            auto rewritten = rewriteWithAllocation(lir, *sch, whole, rewriteRep);
            ASSERT_TRUE(rewritten.ok) << name << " " << arm.label;
            auto const after = censusClassMoves(rewritten.lir, mov);
            if (arm.v2CopyIsProved) {
                EXPECT_EQ(after.identity, 0u)
                    << name << " " << arm.label
                    << ": the copy was PROVED and both ends were handed one "
                       "register, and the rewrite emitted it anyway — the "
                       "coalescer's whole benefit is that this instruction "
                       "never exists";
            } else {
                EXPECT_GE(after.identity, 1u)
                    << name << " " << arm.label
                    << ": both ends of an UNPROVED copy were on one register "
                       "and the rewrite dropped it — the OUTCOME is not "
                       "sufficient on its own. The value's upper half is "
                       "exactly what the drop changes, and the tail READS it.";
            }
        }
    }
}
