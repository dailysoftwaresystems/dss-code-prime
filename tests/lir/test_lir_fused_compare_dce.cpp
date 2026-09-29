// ────────────────────────────────────────────────────────────────────────────
// ── D-LIR-SETCC-DEAD-AFTER-FUSION — THE USE-COUNT GATE, PINNED BOTH WAYS ────
// ────────────────────────────────────────────────────────────────────────────
//
// THE DEFECT THIS FILE PINS. `lowerICmp` materializes a compare's Bool as a
// `cmp → setcc b8 → zext` trio so that a bool which is stored, returned or fed
// to a ternary has correct width semantics (D-LIR-SETCC-WIDTH-CONTRACT). When
// `lowerCondBr` then FUSES the adjacent compare into `cmp lhs, rhs; jcc-cond`
// it RE-EMITS the compare and branches on its flags directly — it never reads
// the Bool. Before the gate, the trio was emitted anyway: written, allocated a
// physical register, encoded and executed for a result with no LIR consumer at
// all, once for every `if` and every `while` in the program.
//
// ✔MEASURED 2026-09-16 at HEAD `4c1f0ed8` on `x86_64:pe64-x86_64-windows-exec`,
// the LIR dump of `if (a < b) … while (a < 100) …` at the post-callconv stage
// (`DSS_DUMP_LIR_MIN_INSTS=1`) — the fused block read:
//
//     cmp p1 p2          ← lowerICmp's compare      DEAD
//     p9 = setcc         ←                          DEAD
//     p9 = zext p9       ←                          DEAD
//     cmp p1 p2          ← the FUSED compare        live
//     jcc _
//
// ★★★ THE WASTE IS THREE INSTRUCTIONS, NOT TWO, AND THAT IS WHY THE FIX IS AT
// THE LOWERING RATHER THAN IN A LATER DCE. The registry row prescribed "a
// side-effect-free LIR instruction whose result vreg has no remaining use is
// elided", which reaches the `setcc` and the `zext` — and cannot reach the
// leading `cmp`, because `cmp` DEFINES NO REGISTER (it writes flags) and so has
// no use count to be zero. A rule shaped around a dead RESULT recovers two of
// the three. Declining to MINT the trio recovers all three, and the register
// pressure with them.
//
// ★★★ AND THE PRESCRIBED SITE COULD NOT HAVE HELD IT. ✔MEASURED: `runLirPeephole`
// runs POST-REGALLOC (`compile_pipeline.cpp` step 8b, between
// `legalizeTwoAddress` and `materializeCallingConvention`), by which point
// `rewriteWithAllocation` has replaced every virtual register with a physical
// one and `verifyLirPostRegalloc` REFUSES any survivor. A "result vreg has no
// remaining use" rule is therefore VACUOUS there. Its physical-register form is
// not vacuous but UNSOUND: `lowerReturn`'s F128 arm writes the ABI return
// register with no LIR consumer whatever (its consumer is the CALLER), and a
// use count would delete it — the hazard `mir_to_lir.cpp` already names where
// it says that arm's old safety argument was "no LIR DCE". `analyzeLiveness`
// agrees by construction: `LirLiveRange::make` FATALS on a physical register,
// because the liveness substrate is pre-regalloc only.
//
// ★★★ IT IS A USE COUNT AND NOT A `setcc` PATTERN, WHICH IS THE WHOLE
// CORRECTNESS ARGUMENT AND THE REASON THIS FILE HAS TWO ARMS. The fusion does
// not CONSUME the Bool, it IGNORES it, so a SECOND consumer leaves the trio
// genuinely live while the branch still fuses. A rule keyed on the mnemonic
// deletes it in exactly that case (D-LIR-FUSION-USE-COUNT-GATE). Arm (B) below
// is that case, and it is the control: it must not move by a byte.
//
// ── WHAT EACH ARM ASSERTS ──────────────────────────────────────────────
//
//   (A) FUSED-ONLY — the compare's Bool is read by NOTHING but the CondBr that
//       fuses it. The lowered block must contain ZERO `setcc`, ZERO `zext` and
//       EXACTLY ONE `cmp`, and the function's encoded byte count must be
//       `kFusedOnlyBytes`. This is the arm that goes RED when the gate is
//       disabled (it then carries 1 `setcc`, 1 `zext` and 2 `cmp`).
//
//   (B) SECOND CONSUMER — the CONTROL, and the half that proves no live
//       instruction was deleted. The identical CFG, one extra read of the same
//       Bool, so `count != 1` and the materialization must happen exactly as it
//       always did: EXACTLY ONE `setcc`, ONE `zext`, TWO `cmp`, and
//       `kSecondConsumerBytes` encoded bytes. This arm is GREEN with the gate
//       and GREEN without it — a gate that fires here is the silent miscompile
//       the row warns about, and it fails LOUD here instead.
//
//   (C) BOTH ARMS STILL BRANCH ON THE COMPARE'S OWN CONDITION — the `jcc`
//       carries the compare's condition code (`Slt`), not the `Ne` of a
//       cmp-against-0. An "assert the setcc is absent" pin reads green over a
//       function whose branch silently stopped fusing, so the fusion itself is
//       asserted beside its residue.
//
// ⚠ EVERY ABSENCE ARM ASSERTS A POSITIVE COUNT BESIDE IT. "No `setcc` appears"
// is vacuously true of an empty instruction list, so each arm pins the `cmp`
// and `jcc` it MUST have in the same breath as the `setcc` it must not.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_peephole.hpp"
#include "lir/lir_reg.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace dss;

namespace {

// ── THE FIXTURE, AND WHY IT IS HAND-BUILT MIR ──────────────────────────
//
// The subject is a USE COUNT on ONE MIR value, so the two arms must differ in
// that count and in NOTHING else. Driven from c source the difference would be
// the front end's to decide: an un-optimized local becomes an alloca + store +
// load, so `int c = (a<b); if (c)` hands the CondBr a LOAD rather than the
// compare and never fuses at all — the arm would pass while measuring a shape
// the row is not about. Built directly, arm (B) is arm (A) plus one operand
// reference, which is exactly the hypothesis.

// `bool f(i32 a, i32 b)`: `if (a < b) return <then>; else return false;`
//
// `thenReturnsTheCompare` is the ONLY difference between the two arms. False
// builds the fused-only shape (the CondBr is the compare's sole reader); true
// adds a SECOND read of the same compare and nothing else.
[[nodiscard]] Mir buildCompareBranch(TypeInterner& interner,
                                     bool thenReturnsTheCompare) {
    auto const i32   = interner.primitive(TypeKind::I32);
    auto const boolT = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {i32, i32};
    auto const sig = interner.fnSig(params, boolT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const thenB = mb.createBlock(StructCfMarker::IfThen);
    MirBlockId const elseB = mb.createBlock(StructCfMarker::IfElse);

    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    MirInstId const b = mb.addArg(1, i32);
    MirInstId const cmpOps[] = {a, b};
    MirInstId const cmp = mb.addInst(MirOpcode::ICmpSlt, cmpOps, boolT);
    mb.addCondBr(cmp, thenB, elseB);

    mb.beginBlock(thenB);
    if (thenReturnsTheCompare) {
        // THE SECOND CONSUMER. One more read of `cmp`, so the census counts 2
        // and the materialization must survive.
        mb.addReturn(cmp);
    } else {
        MirLiteralValue one;
        one.value = std::uint64_t{1};
        one.core  = TypeKind::Bool;
        mb.addReturn(mb.addConst(one, boolT));
    }

    mb.beginBlock(elseB);
    MirLiteralValue zero;
    zero.value = std::uint64_t{0};
    zero.core  = TypeKind::Bool;
    mb.addReturn(mb.addConst(zero, boolT));

    return std::move(mb).finish();
}

struct LoweredShape {
    DiagnosticReporter        rep;
    Lir                       lir{};
    std::vector<std::uint8_t> bytes;
    std::uint32_t             cmpCount   = 0;
    std::uint32_t             setccCount = 0;
    std::uint32_t             zextCount  = 0;
    std::uint32_t             jccCount   = 0;
    std::uint32_t             jccPayload = 0;
    bool                      ok         = false;
};

// Count one mnemonic across every block of every function of `lir`.
[[nodiscard]] std::uint32_t countOpcode(Lir const& lir, std::uint16_t op) {
    std::uint32_t n = 0;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        LirFuncId const fn = lir.funcAt(f);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(blk); ++ii) {
                if (lir.instOpcode(lir.blockInstAt(blk, ii)) == op) ++n;
            }
        }
    }
    return n;
}

// MIR → LIR → liveness → regalloc → rewrite → 2-addr → PEEPHOLE → callconv →
// assemble: the exact stage order `compile_pipeline.cpp` runs, including the
// peephole, because the byte count is the subject and the peephole is the one
// pass between the lowering and the encoder that can also remove instructions.
// The MNEMONIC COUNTS are taken off the LOWERED module (the tier whose gate is
// the subject); the BYTES off the end of the chain.
void runToBytes(Mir& mir, TypeInterner const& interner,
                TargetSchema const& target, std::uint16_t ccIndex,
                LoweredShape& out) {
    auto lowered = lowerToLir(mir, target, interner, out.rep);
    ASSERT_TRUE(lowered.ok) << "MIR->LIR failed";
    ASSERT_EQ(out.rep.errorCount(), 0u) << "the lowering must be clean";

    auto const cmpOp   = target.opcodeByMnemonic("cmp");
    auto const setccOp = target.opcodeByMnemonic("setcc");
    auto const zextOp  = target.opcodeByMnemonic("zext");
    auto const jccOp   = target.opcodeByMnemonic("jcc");
    ASSERT_TRUE(cmpOp.has_value() && setccOp.has_value()
                && zextOp.has_value() && jccOp.has_value())
        << "the target must declare all four mnemonics for this pin to mean "
           "anything — an absent one would make every count vacuously 0";
    out.cmpCount   = countOpcode(lowered.lir, *cmpOp);
    out.setccCount = countOpcode(lowered.lir, *setccOp);
    out.zextCount  = countOpcode(lowered.lir, *zextOp);
    out.jccCount   = countOpcode(lowered.lir, *jccOp);
    for (std::uint32_t f = 0; f < lowered.lir.moduleFuncCount(); ++f) {
        LirFuncId const fn = lowered.lir.funcAt(f);
        for (std::uint32_t bi = 0; bi < lowered.lir.funcBlockCount(fn); ++bi) {
            LirBlockId const blk = lowered.lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < lowered.lir.blockInstCount(blk); ++ii) {
                LirInstId const id = lowered.lir.blockInstAt(blk, ii);
                if (lowered.lir.instOpcode(id) == *jccOp) {
                    out.jccPayload = lowered.lir.instPayload(id);
                }
            }
        }
    }

    auto const liveness = analyzeLiveness(lowered.lir);
    auto const alloc =
        allocateRegisters(lowered.lir, target, liveness, ccIndex, out.rep);
    ASSERT_TRUE(alloc.ok()) << "regalloc failed";
    auto rewritten = rewriteWithAllocation(lowered.lir, target, alloc, out.rep);
    ASSERT_TRUE(rewritten.ok) << "rewrite failed";
    auto legal = legalizeTwoAddress(rewritten.lir, target, out.rep);
    ASSERT_TRUE(legal.ok()) << "2-addr legalize failed";
    auto peeped = runLirPeephole(legal.lir, target, out.rep);
    ASSERT_TRUE(peeped.ok()) << "peephole failed";
    auto cc = materializeCallingConvention(peeped.lir, target, alloc, out.rep);
    ASSERT_TRUE(cc.ok()) << "callconv failed";
    std::vector<MirInstId> lirToMir(cc.lir.instCount(), InvalidMirInst);
    auto assembled = assemble(cc.lir, target, lirToMir, out.rep);
    ASSERT_TRUE(assembled.ok()) << "assemble failed";
    ASSERT_EQ(assembled.functions.size(), 1u);
    out.bytes = assembled.functions[0].bytes;
    out.ok    = true;
}

// ★ THE BYTE PIN. Both constants are MEASURED on the shipped `x86_64` schema at
// cc index 0, 2026-09-16, and neither is derived from the other — they are two
// readings of two functions, not one number and an arithmetic claim about it.
//
// ⚠ THE TWO ARMS' BODIES DIFFER BY MORE THAN THE TRIO (arm (B)'s `then` block
// returns the compare where arm (A)'s returns a constant), so `35 - 29` is NOT
// the trio's size and this file never asserts that it is. The trio's cost is
// read off arm (A) ALONE, by moving it: ✔MEASURED 2026-09-16 with the gate
// forced off, this same function encodes to 39 bytes — TEN bytes of
// `cmp r/m64,r64` + forced-REX `setcc r/m8` + `zext r64,r/m8`
// (`REX.W 0F B6 /r`) that nothing reads, once per fused compare, i.e. once per
// `if` and once per `while` in the program.
//
// `kSecondConsumerBytes` is the one that MUST NOT MOVE, in either direction,
// and it is the whole negative half of this pin: a gate that fired on a live
// Bool would shrink THIS number too, and shrinking it is a wrong answer rather
// than an optimization.
inline constexpr std::size_t kFusedOnlyBytes      = 29;
inline constexpr std::size_t kSecondConsumerBytes = 35;

} // namespace

TEST(LirFusedCompareDce, FusedOnlyCompareMintsNoMaterialization) {
    // ARM (A). The compare's Bool is read by NOTHING but the CondBr that fuses
    // it, so the `cmp → setcc → zext` trio has no consumer and must never be
    // emitted. RED-ON-DISABLE: with `compareIsFusedOnly` forced false this
    // function carries 2 `cmp`, 1 `setcc` and 1 `zext`, and all four
    // expectations below go red at once.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildCompareBranch(interner, /*thenReturnsTheCompare=*/false);
    LoweredShape out;
    ASSERT_NO_FATAL_FAILURE(
        runToBytes(mir, interner, **schema, /*ccIndex=*/0, out));
    ASSERT_TRUE(out.ok);

    // The POSITIVE half first — this function really does compare and branch,
    // so the absences below are absences from a populated list.
    EXPECT_EQ(out.cmpCount, 1u)
        << "the fused branch re-emits the compare and that is the ONLY compare "
           "the function needs";
    EXPECT_EQ(out.jccCount, 1u) << "the branch must still be a fused jcc";
    EXPECT_EQ(out.jccPayload, static_cast<std::uint32_t>(TargetCondCode::Slt))
        << "arm (C): the jcc must carry the COMPARE's condition, not the `Ne` "
           "of a cmp-against-0 — a branch that stopped fusing would also have "
           "no setcc, and must not read as success here";

    // The NEGATIVE half — the trio the row is about.
    EXPECT_EQ(out.setccCount, 0u)
        << "nothing reads this Bool, so no setcc materializes it";
    EXPECT_EQ(out.zextCount, 0u)
        << "and no zext widens a byte nobody asked for";

    // ★ THE BYTE PIN, moving half. Disabling the gate grows this by the
    // encoded trio; nothing else in the function changes.
    EXPECT_EQ(out.bytes.size(), kFusedOnlyBytes)
        << "the fused-only function's encoded size is the measured witness — "
           "if this grew, the trio came back; if it shrank, something else "
           "was deleted and arm (B) is the place to look";
}

TEST(LirFusedCompareDce, SecondConsumerKeepsTheMaterializationByteForByte) {
    // ARM (B) — THE CONTROL, and the half that proves the gate deletes nothing
    // live. One extra read of the SAME compare, identical CFG otherwise. The
    // branch still fuses; the Bool is still needed; the trio must survive
    // exactly as it did before the gate existed.
    //
    // ⚠ This test is GREEN both with the gate and with it disabled. That is the
    // point: it is the arm that a `setcc`-keyed rule would turn red, which is
    // the silent miscompile D-LIR-FUSION-USE-COUNT-GATE names.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildCompareBranch(interner, /*thenReturnsTheCompare=*/true);
    LoweredShape out;
    ASSERT_NO_FATAL_FAILURE(
        runToBytes(mir, interner, **schema, /*ccIndex=*/0, out));
    ASSERT_TRUE(out.ok);

    EXPECT_EQ(out.setccCount, 1u)
        << "a SECOND consumer of the Bool makes the materialization live — "
           "deleting it here is a wrong answer, not an optimization";
    EXPECT_EQ(out.zextCount, 1u)
        << "and its zext with it (D-LIR-SETCC-WIDTH-CONTRACT: the consumer "
           "reads a full-width 0/1, never setcc's byte)";
    EXPECT_EQ(out.cmpCount, 2u)
        << "the materializing compare AND the fused one — the fusion does not "
           "consume the Bool, so both are live here";
    EXPECT_EQ(out.jccCount, 1u);
    EXPECT_EQ(out.jccPayload, static_cast<std::uint32_t>(TargetCondCode::Slt))
        << "the branch fuses in this shape too — the gate changes what is "
           "MATERIALIZED, never whether the branch fuses";

    // ★ THE BYTE PIN, STATIONARY half — the deliverable's second direction.
    // This number is identical with the gate and without it. A gate that
    // reached a live Bool would shrink it, and this is where that shows.
    EXPECT_EQ(out.bytes.size(), kSecondConsumerBytes)
        << "the second-consumer function must not change by a BYTE — this is "
           "the arm that fails if the elision ever stops counting uses";
}
