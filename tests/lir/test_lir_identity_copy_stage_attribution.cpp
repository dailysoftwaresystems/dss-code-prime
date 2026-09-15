// D-LIR-PEEPHOLE-CALLCONV-IDENTITY-COPY-CLAIM-HAS-NO-INSTRUMENT.
//
// `lir_peephole.hpp` justifies running the peephole BEFORE
// `materializeCallingConvention` — rather than last, where it would also see
// the ABI copies — with one load-bearing sentence: *callconv mints exactly ZERO
// identity copies*. That sentence decides a pass ORDER, and the order decides
// whether `LirCallconvResult::perFuncCfi` (keyed by `LirInstId`) still
// describes the instructions it was computed for. It is not a decorative claim.
//
// ⚠ THE EVIDENCE IT CARRIED COULD NOT TEST IT. The stated measurement was
// "5575 at post-rewrite and 5575 at post-callconv", and those were the only two
// LIR dump stages that existed. THREE passes sit between them —
// `legalizeTwoAddress` (which SYNTHESIZES class moves), `runLirPeephole` (which
// DELETES members of exactly this population) and `materializeCallingConvention`
// (the subject) — so an equal count across that span is a NET, and a net of
// zero is not a per-pass zero. A later lane measured a NON-ZERO residue at
// post-callconv and had no way to tell which pass minted it; that is the cost
// of a claim whose instrument answers an adjacent question.
//
// This file is the instrument, at the tier that can run the passes separately.
// What it pins:
//
//   * ONE OWNER. `lir_pass_util::classifyIdentityClassMove` is R1's predicate,
//     and the census counts its verdict. The pin is exact:
//     `redundantCopiesRemoved` equals the census's `deletable` count of the
//     pass's own INPUT. A census that drifted from the rule would measure a
//     population the rule does not act on — the original failure, repeated.
//
//   * THE FIXPOINT. Nothing R1 could delete survives R1. This is what makes a
//     post-callconv residue a statement about REFUSALS rather than about
//     MISSES, and it is the half a stage-boundary count alone cannot supply.
//
//   * ★★★ THE CLAIM ITSELF, MEASURED ON THE PASS IT NAMES. Real `c` source
//     lowered through the real chain, then the census read on BOTH SIDES OF
//     CALLCONV ALONE. Every bucket must be unchanged.
//
//   * THE VACUITY GUARD ON THAT CLAIM. `materializeCallingConvention` must be
//     observed MINTING class moves in the same module — otherwise "it minted no
//     identity one" is a fact about a pass that emitted nothing, and the day the
//     ABI materialization stops going through the class move this pin would
//     pass while measuring nothing.
//
// ⓘ NOT PINNED HERE: the corpus SPLIT of the surviving population across R1's
// refusal buckets. That is a property of `examples/c/**`, not of the rule, and
// the test tier cannot hold it. It is re-measurable at any time with the same
// predicate — set `DSS_DUMP_LIR_MIN_INSTS` / `DSS_DUMP_LIR_FILE` and read the
// `icm=` census on each `########## STAGE` line, which is emitted at all four
// post-regalloc boundaries.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"
#include "lir/lir_peephole.hpp"
#include "lir/lir_reg.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace dss;
using dss::lir_pass_util::censusIdentityClassMoves;
using dss::lir_pass_util::ClassMoveOpcodeCache;
using dss::lir_pass_util::IdentityClassMoveVerdict;
using dss::test_support::lowerCToLir;

namespace {

// Every claim in this file is a claim about a TARGET-BLIND rule and a
// TARGET-BLIND pass, so every pin that can run on both shipped targets does.
constexpr std::array<char const*, 2> kShippedTargets{"x86_64", "arm64"};

[[nodiscard]] std::shared_ptr<TargetSchema> shipped(char const* name) {
    auto t = TargetSchema::loadShipped(name);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(" << name << ") failed";
        return nullptr;
    }
    return *t;
}

// The ordinal of SOME register the target declares to be a full 64-bit GPR.
// Derived from the register table rather than named, because the hand-built
// module below must be buildable on every shipped target and the two do not
// share a single register name.
[[nodiscard]] std::uint32_t someFullWidthGpr(TargetSchema const& sch,
                                             std::uint32_t       skip) {
    std::uint32_t seen = 0;
    auto const&   regs = sch.registers();
    for (std::uint32_t i = 0; i < regs.size(); ++i) {
        if (regs[i].regClass != TargetRegClass::GPR) continue;
        if (regs[i].widthBytes != 8) continue;
        if (seen++ < skip) continue;
        return i;
    }
    ADD_FAILURE() << "the target declares fewer than " << (skip + 1)
                  << " 64-bit GPRs — this fixture needs two distinct ones";
    return 0;
}

// A post-regalloc function carrying, in one block, exactly one member of each
// population class R1 distinguishes:
//
//   pA = mov  pA         full width      → Deletable
//   pA = mov  pA         width 32        → RefusedNarrowerThanRegister
//   pA = zext pA         width 32        → NOT the class move: outside the
//                                          population, inside the superset
//   pB = mov  pA         full width      → not self-referential at all
//   ret pA
//
// ⚠ THE NARROW ARM IS THE ANTI-VACUITY HALF AND IT IS NOT DECORATION. Without
// it the fixpoint pin below would be satisfied by a peephole that deleted the
// WHOLE population, which is a wrong answer (a 32-bit `mov` into a 64-bit
// register zeroes the upper half — it writes bits it did not read). The pin has
// to separate "R1 left nothing it could take" from "R1 took everything".
//
// ⚠ AND THE `zext` ARM IS WHAT MAKES THE SUPERSET A STRICT ONE. It is the
// instruction a mnemonic- or byte-pattern rule would delete along with the
// copies (on x86_64 `mov`, `trunc` and `zext` all disassemble as `mov`), so it
// is the margin the schema-driven opcode test buys, made countable.
[[nodiscard]] Lir mixedIdentityModule(TargetSchema const& sch) {
    LirBuilder b{sch};
    auto const mov = sch.regClassOpOpcode(TargetRegClass::GPR,
                                          RegClassOp::Move);
    EXPECT_TRUE(mov.has_value())
        << "the target declares no GPR class move — every pin in this file "
           "would be asserting about an opcode that cannot be emitted";
    auto const ret  = sch.opcodeByMnemonic("ret");
    auto const zext = sch.opcodeByMnemonic("zext");
    EXPECT_TRUE(ret.has_value());
    EXPECT_TRUE(zext.has_value())
        << "the target declares no `zext` — the fixture needs ONE "
           "self-referential instruction that is NOT the class move, or the "
           "superset it pins stops being strict and measures nothing";

    LirReg const a = makePhysicalReg(someFullWidthGpr(sch, 0), LirRegClass::GPR);
    LirReg const c = makePhysicalReg(someFullWidthGpr(sch, 1), LirRegClass::GPR);

    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    std::array<LirOperand, 1> const fromA{LirOperand::makeReg(a)};
    (void)b.addInst(mov.value_or(0), a, fromA, /*payload=*/0, /*flags=*/0);
    (void)b.addInst(mov.value_or(0), a, fromA, /*payload=*/0,
                    kLirInstFlagWidth32);
    (void)b.addInst(zext.value_or(0), a, fromA, /*payload=*/0,
                    kLirInstFlagWidth32);
    (void)b.addInst(mov.value_or(0), c, fromA, /*payload=*/0, /*flags=*/0);
    std::array<LirOperand, 1> const retOps{LirOperand::makeReg(a)};
    b.addReturn(ret.value_or(0), retOps);
    return std::move(b).finish();
}

// A probe that puts the ABI materialization to work: several calls with
// register-passed arguments, and returns. `materializeCallingConvention` mints
// its class moves at exactly these two shapes (an outgoing argument into its
// arg register, a value into the return register), so a module without them
// would make the vacuity guard below unsatisfiable.
constexpr char const* kCallHeavySource =
    "static int add3(int a, int b, int c) { return a + b + c; }\n"
    "static long mix(long a, long b) { return a * 3 + b; }\n"
    "int probe(int k) {\n"
    "    int s = add3(k, k + 1, k + 2);\n"
    "    s += add3(s, k, 7);\n"
    "    long t = mix((long)s, (long)k);\n"
    "    t += mix(t, (long)s);\n"
    "    return (int)t + s;\n"
    "}\n";

// Total instructions in a module whose opcode is the GPR or FPR class move and
// whose operand list is a single register — the population callconv MINTS from,
// identity or not. The vacuity guard's subject.
[[nodiscard]] std::size_t countClassMoves(Lir const&          lir,
                                          TargetSchema const& sch) {
    ClassMoveOpcodeCache cache{};
    std::size_t          n = 0;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        LirFuncId const fn = lir.funcAt(f);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                LirInstId const inst = lir.blockInstAt(blk, i);
                LirReg const    res  = lir.instResult(inst);
                if (!res.valid()) continue;
                auto const mov = cache.resolve(sch, res.regClass());
                if (!mov.has_value() || lir.instOpcode(inst) != *mov) continue;
                auto const ops = lir.instOperands(inst);
                if (ops.size() == 1 && ops[0].kind == LirOperandKind::Reg) ++n;
            }
        }
    }
    return n;
}

// The whole post-regalloc chain, stage by stage, so the census can be read
// BETWEEN two named passes instead of across three.
struct StagedModules {
    bool ok = false;
    Lir  postRewrite;
    Lir  postLegalize;
    Lir  postPeephole;
    Lir  postCallconv;
};

[[nodiscard]] StagedModules stageThrough(std::string src, char const* target) {
    StagedModules out;
    auto lowered = lowerCToLir(std::move(src), std::string{target});
    if (!lowered.lir.ok) {
        ADD_FAILURE() << target << ": the probe did not lower to LIR";
        return out;
    }
    auto const&        sch = *lowered.target;
    DiagnosticReporter rep;
    auto const         liveness = analyzeLiveness(lowered.lir.lir);
    auto const alloc = allocateRegisters(lowered.lir.lir, sch, liveness,
                                         /*ccIndex=*/0, rep);
    if (!alloc.ok()) {
        ADD_FAILURE() << target << ": allocateRegisters failed";
        return out;
    }
    auto rewritten = rewriteWithAllocation(lowered.lir.lir, sch, alloc, rep);
    if (!rewritten.ok) {
        ADD_FAILURE() << target << ": rewriteWithAllocation failed";
        return out;
    }
    auto legal = legalizeTwoAddress(rewritten.lir, sch, rep);
    if (!legal.ok()) {
        ADD_FAILURE() << target << ": legalizeTwoAddress failed";
        return out;
    }
    auto peeped = runLirPeephole(legal.lir, sch, rep);
    if (!peeped.ok()) {
        ADD_FAILURE() << target << ": runLirPeephole failed";
        return out;
    }
    auto cc = materializeCallingConvention(peeped.lir, sch, alloc, rep);
    if (!cc.ok()) {
        ADD_FAILURE() << target << ": materializeCallingConvention failed";
        return out;
    }
    out.postRewrite  = std::move(rewritten.lir);
    out.postLegalize = std::move(legal.lir);
    out.postPeephole = std::move(peeped.lir);
    out.postCallconv = std::move(cc.lir);
    out.ok           = true;
    return out;
}

} // namespace

// ── ONE OWNER ───────────────────────────────────────────────────────────────
//
// The census's `deletable` bucket, read on the pass's INPUT, must equal the
// number the pass reports having removed. Anything else means the instrument
// and the rule have drifted, which is the defect this row is about.
TEST(LirIdentityCopyStageAttribution, TheCensusCountsExactlyWhatRuleR1Deletes) {
    for (auto const* name : kShippedTargets) {
        auto sch = shipped(name);
        ASSERT_NE(sch, nullptr);
        Lir const          src = mixedIdentityModule(*sch);
        DiagnosticReporter rep;
        auto const         before = censusIdentityClassMoves(src, *sch);
        auto const         result = runLirPeephole(src, *sch, rep);
        ASSERT_TRUE(result.ok()) << name;

        EXPECT_EQ(before.population, 2u)
            << name << ": the fixture no longer builds one deletable and one "
                       "refused identity class move";
        EXPECT_EQ(before.deletable, 1u) << name;
        EXPECT_EQ(before.refusedNarrowerThanRegister, 1u) << name;
        EXPECT_EQ(result.redundantCopiesRemoved, before.deletable)
            << name
            << ": `censusIdentityClassMoves` and rule R1 disagree about which "
               "instructions are redundant copies. They are the same predicate "
               "(`classifyIdentityClassMove`) by construction, so a mismatch "
               "means one of the two grew a second opinion.";
    }
}

// ── THE FIXPOINT ────────────────────────────────────────────────────────────
//
// After the peephole nothing R1 would delete remains — but the population is
// NOT emptied. Both halves are the pin: the first says a later residue is a
// refusal, the second says the refusals are still being made.
TEST(LirIdentityCopyStageAttribution, NothingRuleR1CouldDeleteSurvivesIt) {
    for (auto const* name : kShippedTargets) {
        auto sch = shipped(name);
        ASSERT_NE(sch, nullptr);
        DiagnosticReporter rep;
        auto const result = runLirPeephole(mixedIdentityModule(*sch), *sch, rep);
        ASSERT_TRUE(result.ok()) << name;
        auto const after = censusIdentityClassMoves(result.lir, *sch);

        EXPECT_EQ(after.deletable, 0u)
            << name << ": an identity class move R1 accepts survived R1";
        EXPECT_EQ(after.population, 1u)
            << name << ": the NARROW identity copy must survive — deleting it "
                       "zeroes the upper half of the register it names";
        EXPECT_EQ(after.refusedNarrowerThanRegister, 1u) << name;
    }
}

// ── THE CLAIM, MEASURED ON THE PASS IT NAMES ────────────────────────────────
//
// ★★★ THIS IS THE PIN THE HEADER'S SENTENCE NEVER HAD. The census is read
// immediately before and immediately after `materializeCallingConvention` and
// NOTHING ELSE RUNS IN BETWEEN, so a difference is that pass's own doing.
TEST(LirIdentityCopyStageAttribution, CallconvMintsNoIdentityClassMove) {
    for (auto const* name : kShippedTargets) {
        auto sch = shipped(name);
        ASSERT_NE(sch, nullptr);
        auto staged = stageThrough(kCallHeavySource, name);
        ASSERT_TRUE(staged.ok) << name;

        auto const before = censusIdentityClassMoves(staged.postPeephole, *sch);
        auto const after  = censusIdentityClassMoves(staged.postCallconv, *sch);

        // ⚠ THE VACUITY GUARD FIRST. "Minted no identity class move" is only a
        // claim about callconv while callconv is minting class moves at all.
        std::size_t const movesBefore = countClassMoves(staged.postPeephole, *sch);
        std::size_t const movesAfter  = countClassMoves(staged.postCallconv, *sch);
        ASSERT_GT(movesAfter, movesBefore)
            << name
            << ": materializeCallingConvention minted no register-to-register "
               "class move on a call-heavy probe, so the pin below would be "
               "asserting about a pass that emitted nothing. The ABI "
               "materialization has changed shape — re-read this pin before "
               "adjusting the probe.";

        EXPECT_EQ(after.population, before.population)
            << name
            << ": materializeCallingConvention changed the identity "
               "class-move population. The peephole runs BEFORE it precisely "
               "because it did not — see `lir_peephole.hpp`'s placement "
               "argument, which trades this population against renumbering "
               "every `perFuncCfi` row.";
        EXPECT_EQ(after.deletable, before.deletable) << name;
        EXPECT_EQ(after.refusedNarrowerThanRegister,
                  before.refusedNarrowerThanRegister)
            << name;
        EXPECT_EQ(after.refusedSideEffects, before.refusedSideEffects) << name;
        EXPECT_EQ(after.refusedUndeclaredRegisterWidth,
                  before.refusedUndeclaredRegisterWidth)
            << name;
        EXPECT_EQ(after.refusedNamesConstraintPoolEntry,
                  before.refusedNamesConstraintPoolEntry)
            << name;

        // And the consequence that makes the placement safe: the module handed
        // to the assembler carries nothing R1 would have taken.
        EXPECT_EQ(after.deletable, 0u)
            << name << ": a deletable identity class move reached the encoder";
    }
}

// ── THE POPULATION IS PARTITIONED, NOT SAMPLED ──────────────────────────────
//
// A count that does not add up is an attribution that silently loses members,
// which is how a residue goes unexplained. Held on a REAL module at every one
// of the four post-regalloc stage boundaries.
TEST(LirIdentityCopyStageAttribution, EveryPopulationMemberLandsInExactlyOneBucket) {
    for (auto const* name : kShippedTargets) {
        auto sch = shipped(name);
        ASSERT_NE(sch, nullptr);
        auto staged = stageThrough(kCallHeavySource, name);
        ASSERT_TRUE(staged.ok) << name;

        struct Stage {
            char const* label;
            Lir const*  lir;
        };
        std::array<Stage, 4> const stages{
            Stage{"post-rewrite", &staged.postRewrite},
            Stage{"post-legalize", &staged.postLegalize},
            Stage{"post-peephole", &staged.postPeephole},
            Stage{"post-callconv", &staged.postCallconv}};
        for (auto const& s : stages) {
            auto const c = censusIdentityClassMoves(*s.lir, *sch);
            EXPECT_EQ(c.population,
                      c.deletable + c.refusedSideEffects
                          + c.refusedUndeclaredRegisterWidth
                          + c.refusedNarrowerThanRegister
                          + c.refusedNamesConstraintPoolEntry)
                << name << " @ " << s.label
                << ": the buckets do not sum to the population — a verdict was "
                   "added without a bucket, and the attribution is lossy";
            EXPECT_LE(c.population, c.selfReferentialSingleOperand)
                << name << " @ " << s.label
                << ": an identity class move that is not self-referential — "
                   "the superset is not a superset, so the margin the opcode "
                   "test buys cannot be read off these two numbers";
        }
    }
}

// ── THE MARGIN THE OPCODE TEST BUYS ─────────────────────────────────────────
//
// ★★★ R1'S CORRECTNESS ARGUMENT IS A REJECTION, NOT AN ACCEPTANCE. "Result ==
// its only operand" is the shape a mnemonic- or byte-pattern rule would match;
// on x86_64 `mov`, `trunc` and `zext` all disassemble as `mov`, and deleting a
// `trunc %r14d,%r14d` or a `zext %r15d,%r15d` — which CLEAR the upper 32 bits —
// is a wrong answer, not a missed optimization.
//
// The header states that margin as a corpus ratio. It is asserted here as a
// STRICT inequality on a real module, so the day the opcode test degenerates
// into the shape it is defending against, a pin says so — a ratio in a comment
// cannot.
TEST(LirIdentityCopyStageAttribution, TheOpcodeTestRejectsMoreThanItAccepts) {
    for (auto const* name : kShippedTargets) {
        auto sch = shipped(name);
        ASSERT_NE(sch, nullptr);
        auto const c = censusIdentityClassMoves(mixedIdentityModule(*sch), *sch);

        EXPECT_EQ(c.selfReferentialSingleOperand, 3u)
            << name << ": the fixture no longer builds three self-referential "
                       "single-operand instructions";
        EXPECT_EQ(c.population, 2u) << name;
        EXPECT_GT(c.selfReferentialSingleOperand, c.population)
            << name
            << ": every self-referential single-operand instruction was "
               "counted as an identity CLASS MOVE. The opcode identity test "
               "has stopped discriminating — that is the shape a mnemonic or "
               "byte-pattern rule has, and on x86_64 it deletes `trunc "
               "%r14d,%r14d` and `zext %r15d,%r15d`, which CLEAR the upper 32 "
               "bits. A wrong answer, not a missed optimization.";

        // And the margin survives the pass: R1 takes one copy and leaves the
        // `zext` alone, so the superset shrinks by exactly what R1 removed.
        DiagnosticReporter rep;
        auto const result = runLirPeephole(mixedIdentityModule(*sch), *sch, rep);
        ASSERT_TRUE(result.ok()) << name;
        auto const after = censusIdentityClassMoves(result.lir, *sch);
        EXPECT_EQ(after.selfReferentialSingleOperand,
                  c.selfReferentialSingleOperand - result.redundantCopiesRemoved)
            << name << ": the pass removed something outside the class-move "
                       "population";
    }
}
