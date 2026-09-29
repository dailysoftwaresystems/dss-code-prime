// D-LIR-COALESCE-ANTI-AFFINITY-QUERY-IS-QUADRATIC — the COMPLEXITY pin.
//
// ★★★ WHAT WENT WRONG, AND IT WAS A SIZE CLAIM IN A COMMENT.
// `buildCoalescePartition`'s anti-affinity veto answered "does any forbidden
// pair straddle these two roots" by RE-SCANNING THE ENTIRE `forbidden` list on
// every query, on the written premise that "the lists are short (one entry per
// untied register operand of a 2-address instruction)". That premise is a claim
// about SIZE and it is false: one entry per 2-address instruction is
// Θ(instructions). A single function of N straight-line `s += k;` statements
// lowers on x86-64 to N two-address `add`s, minting N forbidden pairs AND N copy
// edges, and the query runs once per edge — `|edges| × |forbidden|` = N².
//
// ✔MEASURED before the fix (Release, Windows, one host, one run), inner
// iterations of that scan: 400 000 000 at N=20 000; 1 600 000 000 at N=40 000;
// 6 400 000 000 at N=80 000 — the identity `iterations == N²` holding to the
// digit. The whole compile was 8.3 s at N=80 000 of which 5.2 s (63%) was this
// veto, against gcc 13.2.0 at 0.63 s on the identical file scaling at n^0.9.
// Under the union rule (`DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C`, taken over what
// WORKS) gcc is a WORKING reference here, so near-linear scaling is REQUIRED,
// not an aspiration.
//
// ★★★ WHY THIS PIN COUNTS OPERATIONS AND DOES NOT TIME THE CLOCK.
// A wall-clock performance assertion is sized on the machine that wrote it and
// reds on the slowest leg that runs it, NAMING THE WRONG EVENT —
// `.harness-config/runner/actions/check-wall-clock-in-tests/` refuses new ones for exactly that
// reason, and this repository has already paid for one: a wall-clock literal
// that entered a test before anything guarded against it. What is asserted
// here is `LirFuncAllocation::coalesceAntiAffinity{Queries,Probes}`: THE
// ALGORITHM'S OWN WORK. Those numbers are deterministic, host-independent,
// load-independent and identical in Debug and Release, so a statement about
// them is a fact about the algorithm rather than about the box. A busy CI
// machine cannot red it and a genuine complexity regression cannot hide from it.
//
// ★★ THE FOUR ARMS, AND WHY NONE OF THEM CAN GO VACUOUS.
//   * PREMISE (`TheSubjectReallyPresentsLinearlyManyPairs`) — the subject
//     really does present Θ(N) forbidden pairs, and the veto really is
//     consulted. The pair count is RE-DERIVED from the LIR rather than read
//     back from the allocator, because asking an instrument to confirm its own
//     premise is how a pin ends up testing its helper instead of the property.
//     Without this arm, a lowering change that stopped emitting two-address
//     ties would leave every other arm passing over an empty list.
//   * WORK, x86-64 — total inspections must be O(|forbidden|), not
//     O(queries × |forbidden|), plus a growth arm across a doubled input.
//   * WORK, arm64 — the same statement where the list is EMPTY, because the
//     index is a data structure and not a target special case.
//   * CONTROL (`CoalescingDecisionsAreUnchangedControl`) — the coalescer's
//     DECISIONS and its query COUNT are unchanged. Reverting the index leaves
//     both identical, so THIS ARM STAYS GREEN UNDER THE MUTANT, which is what
//     makes the work arms' red evidence about COMPLEXITY specifically rather
//     than about "the mutant broke something".
//
// ⚠ `probes == 0` IS A LEGITIMATE READING AND THE PIN IS BUILT AROUND THAT.
// A straddling pair is incident to BOTH classes, so an empty incidence list on
// either side settles the query without inspecting anything. That is why
// `queries` exists as a separate counter: with only `probes`, "the index made
// this free" and "the veto is no longer consulted at all" are the same number,
// and the second is a correctness regression wearing a performance win's
// clothes.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"
#include "lir/lir_regalloc.hpp"
#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

using namespace dss;
using dss::test_support::lowerCToLir;

namespace {

// One function, N straight-line `s += k;` statements, no control flow.
//
// ⓘ THE SHAPE IS THE SUBJECT, NOT AN ARBITRARY STRESSOR. It is the shape the
// orchestrator's gcc/clang scaling table was taken on, so a number measured here
// is comparable with a number measured there. It also has NO branches, which
// keeps the assembler's relaxation fixed point at exactly one pass and leaves
// the allocator as the only thing that can be superlinear.
[[nodiscard]] std::string straightLineSource(unsigned n) {
    std::string src = "long sx_straight(long const* p) {\n    long s = 0;\n";
    src.reserve(src.size() + static_cast<std::size_t>(n) * 20u + 64u);
    for (unsigned i = 0; i < n; ++i) {
        src += "    s += p[";
        src += std::to_string(i);
        src += "];\n";
    }
    src += "    return s;\n}\n";
    return src;
}

struct CoalesceWork {
    std::uint64_t queries = 0;   // times the veto was ASKED
    std::uint64_t probes  = 0;   // forbidden pairs it INSPECTED to answer
    std::uint32_t unions  = 0;
    std::uint32_t pairs   = 0;   // forbidden pairs, re-derived from the LIR
    bool          ok      = false;
};

// Count the anti-affinity pairs the subject presents to the coalescer, read
// STRAIGHT OFF THE LIR rather than off the allocator's own counter.
//
// ★ IT IS RE-DERIVED ON PURPOSE. The premise this pin rests on is "the
// forbidden list is Θ(instructions), not short", and asking the allocator to
// confirm its own premise would be asking the instrument about itself — the
// shape `check-anchor-registry`'s round-trip pin fell into when it read its
// cell through the very helper it was meant to guard. This walks the module and
// applies the same DECLARED rule `collectCoalesceInput` applies: a
// `requires2Address` opcode's virtual result against each UNTIED virtual
// register operand that is not the tied one.
[[nodiscard]] std::uint32_t countAntiAffinityPairs(Lir const& lir,
                                                   TargetSchema const& schema) {
    std::uint32_t pairs = 0;
    for (std::size_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(static_cast<std::uint32_t>(fi));
        std::uint32_t const nb = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            std::uint32_t const ni = lir.blockInstCount(blk);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                LirInstId const inst = lir.blockInstAt(blk, ii);
                auto const* info = schema.opcodeInfo(lir.instOpcode(inst));
                if (info == nullptr || !info->requires2Address.has_value())
                    continue;
                LirReg const result = lir.instResult(inst);
                if (!result.valid() || result.isPhysical != 0) continue;
                std::size_t const tied = *info->requires2Address;
                auto const ops = lir.instOperands(inst);
                std::uint32_t tiedId = 0;
                if (ops.size() > tied && ops[tied].kind == LirOperandKind::Reg
                    && ops[tied].reg.valid() && ops[tied].reg.isPhysical == 0) {
                    tiedId = ops[tied].reg.id;
                }
                for (std::size_t k = 0; k < ops.size(); ++k) {
                    if (k == tied) continue;
                    if (ops[k].kind != LirOperandKind::Reg) continue;
                    LirReg const r = ops[k].reg;
                    if (!r.valid() || r.isPhysical != 0) continue;
                    if (tiedId != 0 && r.id == tiedId) continue;
                    ++pairs;
                }
            }
        }
    }
    return pairs;
}

// Lower, allocate, and read the coalescer's own counters. Uses the REAL
// c → HIR → MIR → LIR lowering and the REAL allocator entry point: a
// hand-built module could not establish that the production lowering is what
// mints Θ(N) forbidden pairs, which is the whole premise.
[[nodiscard]] CoalesceWork coalesceWorkFor(unsigned n, char const* targetName) {
    CoalesceWork out;
    auto target = TargetSchema::loadShipped(targetName);
    if (!target.has_value()) {
        ADD_FAILURE() << "loadShipped(" << targetName << ") failed";
        return out;
    }
    auto lowered = lowerCToLir(straightLineSource(n), *target);
    if (!lowered.lir.ok) {
        ADD_FAILURE() << targetName << ": lowering failed at n=" << n;
        return out;
    }
    LirLiveness const liveness = analyzeLiveness(lowered.lir.lir);
    DiagnosticReporter rep;
    LirAllocation const alloc =
        allocateRegisters(lowered.lir.lir, **target, liveness, /*ccIndex=*/0, rep);
    if (!alloc.ok()) {
        ADD_FAILURE() << targetName << ": allocation failed at n=" << n;
        return out;
    }
    for (auto const& f : alloc.perFunc) {
        out.queries += f.coalesceAntiAffinityQueries;
        out.probes  += f.coalesceAntiAffinityProbes;
        out.unions  += f.coalescedCopies;
    }
    out.pairs = countAntiAffinityPairs(lowered.lir.lir, **target);
    out.ok = true;
    return out;
}

// The two sizes. Small enough that the pin costs a fraction of a second even
// under the QUADRATIC predecessor (2 000 queries x 2 000 pairs = 4 x 10⁶
// inspections, which is milliseconds), because a pin only affordable at the
// size where the defect HURTS is a pin nobody runs. What carries the claim is
// the RELATION between work and input, and that relation does not need a large
// N to separate "linear" from "quadratic".
constexpr unsigned kSmall = 1000;
constexpr unsigned kLarge = 2000;

// ── THE BOUND, AND WHY IT IS THIS SHAPE ─────────────────────────────────────
//
// The veto's total work must be O(|forbidden|) — linear in the input it reads —
// NOT O(queries × |forbidden|). Expressed as `probes <= K * pairs`, this is a
// COMPLEXITY statement that needs only one size to evaluate, and the separation
// is enormous rather than marginal: at n=1000 the predecessor inspected
// 2000 × 2000 = 4 000 000 pairs against a bound of 8 × 2000 = 16 000, a factor
// of 250. K is 8 rather than 1 because the index legitimately re-inspects a
// pair once per class it survives into; it is NOT a tuning knob, and a failure
// here is never answered by raising it.
constexpr std::uint64_t kMaxProbesPerPair = 8;

// The growth arm, kept alongside the absolute bound because the two fail
// differently: a regression that is linear-but-enormous trips the bound, and
// one that only shows up as the input grows trips the ratio. Quadratic scores
// exactly 4.0 here; linear scores 2.0.
constexpr double kMaxGrowthForDoubledInput = 2.75;


}  // namespace

// ── THE PREMISE, ASSERTED ON ITS OWN AND FIRST ──────────────────────────────
//
// Everything below claims something about work done over a forbidden list that
// is Θ(N). If that list stops being Θ(N) — a lowering change, a schema change,
// a different tie declaration — every other arm in this file passes while
// witnessing NOTHING, and it passes QUIETLY, which is the failure mode this
// repository cares most about. So the population is re-derived from the LIR
// (`countAntiAffinityPairs`, which applies the declared rule itself rather than
// asking the allocator to confirm its own premise) and pinned as a RELATION:
// doubling the statements doubles the pairs.
TEST(LirCoalesceAntiAffinityComplexity, TheSubjectReallyPresentsLinearlyManyPairs) {
    CoalesceWork const small = coalesceWorkFor(kSmall, "x86_64");
    CoalesceWork const large = coalesceWorkFor(kLarge, "x86_64");
    ASSERT_TRUE(small.ok);
    ASSERT_TRUE(large.ok);

    ASSERT_GT(small.pairs, 0u)
        << "x86_64: the subject presents ZERO anti-affinity pairs at n="
        << kSmall << ". Every other arm in this file is then VACUOUS — they "
           "would all pass over a coalescer that never reads a forbidden list "
           "at all. Fix the SUBJECT (or retire the claim); do not relax a bound.";
    EXPECT_EQ(large.pairs, small.pairs * 2u)
        << "x86_64: doubling the statement count changed the anti-affinity "
           "pair count from " << small.pairs << " to " << large.pairs
        << " rather than to " << (small.pairs * 2u)
        << ". The premise of this file is that the forbidden list is Θ(N); if "
           "it is no longer, the complexity claim below is about a different "
           "input than the one that was measured.";

    // The veto must be ON THE PATH. A coalescer that never asks cannot be
    // measured for how cheaply it answers.
    ASSERT_GT(small.queries, 0u)
        << "x86_64: the anti-affinity veto was never CONSULTED at n=" << kSmall
        << " — an earlier veto is refusing every edge, so this file measures a "
           "query that does not happen.";
    ASSERT_GT(small.unions, 0u)
        << "x86_64: the coalescer merged nothing at n=" << kSmall
        << " — no edge reached the veto, so the pin is VACUOUS.";
}

// x86-64 is where the defect LIVES: its `add` is two-address, so every
// statement mints a forbidden pair. This is the arm that reddens when the
// quadratic scan comes back.
TEST(LirCoalesceAntiAffinityComplexity, AntiAffinityWorkIsLinearOnX86_64) {
    CoalesceWork const small = coalesceWorkFor(kSmall, "x86_64");
    CoalesceWork const large = coalesceWorkFor(kLarge, "x86_64");
    ASSERT_TRUE(small.ok);
    ASSERT_TRUE(large.ok);
    ASSERT_GT(small.pairs, 0u) << "vacuous — see TheSubjectReallyPresentsLinearlyManyPairs";
    ASSERT_GT(small.queries, 0u) << "vacuous — the veto was never consulted";

    // (1) THE ABSOLUTE BOUND. Total inspections must be linear in the size of
    //     the list being searched, not in queries × that size.
    EXPECT_LE(small.probes, kMaxProbesPerPair * small.pairs)
        << "x86_64 n=" << kSmall << ": the anti-affinity veto inspected "
        << small.probes << " forbidden pairs over " << small.queries
        << " queries against a list of " << small.pairs
        << " — that is " << (static_cast<double>(small.probes)
                             / static_cast<double>(small.pairs))
        << " inspections per pair, where a linear query is O(1) of them. This "
           "is D-LIR-COALESCE-ANTI-AFFINITY-QUERY-IS-QUADRATIC RETURNING: the "
           "veto is re-scanning the whole `forbidden` list per query instead of "
           "the incidence index. ⛔ Restore the index. Never raise this bound, "
           "and never cap or skip the veto — a bail-out after k probes turns a "
           "slow compile into a WRONG one, which is the workaround this "
           "project's standing order forbids.";
    EXPECT_LE(large.probes, kMaxProbesPerPair * large.pairs)
        << "x86_64 n=" << kLarge << ": " << large.probes
        << " inspections over " << large.queries << " queries against "
        << large.pairs << " pairs. See the n=" << kSmall << " arm.";

    // (2) THE GROWTH ARM. Doubling the input must not quadruple the work.
    //     `+ 1` on the denominator: the index answers most queries by
    //     inspecting NOTHING, so zero is a legitimate reading and a ratio must
    //     not become 0/0 — the claim is still "did not quadruple".
    double const growth = static_cast<double>(large.probes + 1)
                        / static_cast<double>(small.probes + 1);
    EXPECT_LE(growth, kMaxGrowthForDoubledInput)
        << "x86_64: DOUBLING the input multiplied the veto's inspections by "
        << growth << "x (n=" << kSmall << ": " << small.probes << "; n="
        << kLarge << ": " << large.probes << "). Linear scores ~2.0 and "
           "QUADRATIC scores 4.0.";
}

// arm64's `add` is three-address, so the same source mints no two-address tie
// and this target reaches the veto with an EMPTY forbidden list. It is here as
// the other half of the agnosticism claim — the index is a data structure, not
// a target special case, so it must be correct where it is barely exercised
// too — and because a target that suddenly started minting pairs would mean the
// premise moved under both arms at once.
//
// ⚠ AND THE ZEROES ARE ASSERTED, NOT SKIPPED. ✔MEASURED at this tree: on arm64
// this subject presents ZERO anti-affinity pairs, performs ZERO merges and asks
// the veto ZERO times — `add` is three-address there, so there is no tie to
// mint an edge from. A test that quietly returned on seeing a zero would be the
// vacuous skip this project keeps closing; pinning the zero means that if arm64
// ever DOES start coalescing this subject, a human is made to come back and
// re-read whether the bound below is still the right statement.
TEST(LirCoalesceAntiAffinityComplexity, AntiAffinityWorkIsLinearOnArm64) {
    CoalesceWork const small = coalesceWorkFor(kSmall, "arm64");
    CoalesceWork const large = coalesceWorkFor(kLarge, "arm64");
    ASSERT_TRUE(small.ok);
    ASSERT_TRUE(large.ok);

    // The WORK statement, asserted unconditionally and in the same shape as the
    // x86-64 arm: inspections are bounded by the list being searched. With an
    // empty list it says the veto inspects nothing, which IS the claim.
    EXPECT_LE(large.probes, kMaxProbesPerPair * large.pairs)
        << "arm64 n=" << kLarge << ": " << large.probes
        << " forbidden-pair inspections over " << large.queries
        << " queries against a list of " << large.pairs
        << ". See the x86_64 arm for what this means.";

    // The measured shape of this target, pinned so a change to it is LOUD.
    EXPECT_EQ(small.pairs, 0u)
        << "arm64 now presents " << small.pairs << " anti-affinity pairs for a "
           "subject that measured ZERO when this pin was written — `add` is "
           "three-address on this target, so a nonzero here means the lowering "
           "or the schema changed. That is not a failure of the fix; it means "
           "arm64 now exercises the veto and this arm should be strengthened "
           "to the x86_64 arm's full form rather than left asserting a zero.";
    EXPECT_EQ(large.pairs, small.pairs * 2u)
        << "arm64: the two sizes disagree about how many anti-affinity pairs "
           "this target presents (" << small.pairs << " -> " << large.pairs
        << "), so neither number can be trusted as a premise.";
}

// ★★ THE CONTROL ARM. It asserts what the fix must NOT have changed: the
// coalescer's DECISIONS and how often it asks the veto. Reverting the incidence
// index leaves both identical — that is the point — so THIS TEST STAYS GREEN
// UNDER THE MUTANT, which is what makes the work arms' red evidence about
// COMPLEXITY specifically rather than about "the mutant broke something".
//
// ⓘ x86-64 ONLY, DELIBERATELY. It is the only shipped target on which this
// subject makes the coalescer decide anything at all (arm64: zero merges, zero
// queries — pinned as such in the arm above). Running the same assertions there
// would assert `0 == 0 * 2` and read as a second target's worth of confidence
// while witnessing nothing.
TEST(LirCoalesceAntiAffinityComplexity, CoalescingDecisionsAreUnchangedControl) {
    CoalesceWork const small = coalesceWorkFor(kSmall, "x86_64");
    CoalesceWork const large = coalesceWorkFor(kLarge, "x86_64");
    ASSERT_TRUE(small.ok);
    ASSERT_TRUE(large.ok);
    ASSERT_GT(small.queries, 0u)
        << "x86_64: the veto was never consulted — this control would pass "
           "over a coalescer that does nothing at all.";
    ASSERT_GT(small.unions, 0u) << "x86_64: nothing coalesced — vacuous";

    // The subject is uniform, so doubling the statements doubles the merges.
    // Asserted as a RELATION rather than as two literals, so the arm survives a
    // lowering that legitimately changes how many copies exist per statement
    // while still refusing one that changes the coalescer's ANSWER.
    EXPECT_EQ(large.unions, small.unions * 2u)
        << "x86_64: doubling a uniform straight-line body changed the "
           "coalescer's merge count from " << small.unions << " to "
        << large.unions << " rather than to " << (small.unions * 2u)
        << ". The anti-affinity index is meant to make the SAME predicate "
           "cheaper; a different merge count means it answers a DIFFERENT "
           "question — a correctness regression, not a performance one.";
    EXPECT_EQ(large.queries, small.queries * 2u)
        << "x86_64: the veto was consulted " << small.queries << " times at n="
        << kSmall << " and " << large.queries << " at n=" << kLarge
        << ", not " << (small.queries * 2u)
        << ". How OFTEN the veto is asked is a property of the coalescer's edge "
           "walk, which this fix did not touch.";
}
