// THE LANDING RULE, WITH THE ALLOCATOR'S OWN ABORT CONTAINED
// (D-LIR-NO-EXCEPTIONAL-EDGE-INTO-A-TRY-HANDLER)
//
// A value a landing block reads is kept where a call would keep it — a register
// outside the convention's `callerSaved`, or a spill slot. The allocator asks one
// predicate for that (`rangeOutlivesAVolatileRegister`: the range crosses a call
// OR covers the entry of a landing block), and an INDEPENDENT audit re-derives the
// property on the finished assignment table and ABORTS the process when a value
// would be lost (`findLandingViolation`, then `regallocFatal`).
//
// ═══ WHY THIS CASE HAS A BINARY OF ITS OWN ═════════════════════════════════════
//
// The two are meant to disagree loudly: take the landing half out of the
// predicate and the audit kills the process. That is right for a compiler and
// useless for reading the mutant — the allocator's own cases
// (lir/test_lir_regalloc) run the probe in-process, so under that mutant their
// binary dies before it reports and no case can be named as the one that caught
// it. Here the allocation runs in a CHILD: the case requires the child to leave
// with 0, and a child the audit killed makes this case fail by its name while the
// binary goes on. The control beside it runs the same module with no region,
// where there is no landing for the audit to defend, in-process.
//
// The probe is tests/lir/landing_probe.hpp, the one the allocator's cases read.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "landing_probe.hpp"
#include "lir/lir.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_regalloc.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dss;
using dss::test_support::buildLandingProbe;
using dss::test_support::ConventionFacts;
using dss::test_support::conventionFacts;
using dss::test_support::LandingProbe;

namespace {

// Whether an assignment keeps its value across a transfer that hands over only
// the frame and the convention's call-preserved registers.
[[nodiscard]] bool keptWhereACallKeepsIt(LirRegAssignment const* a,
                                         ConventionFacts const&  facts) {
    return a != nullptr
        && (a->isSpilled()
            || !facts.volatileOrdinals.contains(
                   static_cast<std::uint16_t>(a->physReg().id)));
}

// Allocates the probe WITH its guarded region under every shipped convention of
// both targets, and LEAVES THE PROCESS: 0 when every convention kept both values
// a landing block reads where a call would keep them; 3 when one did not; 4 when
// the fixture itself could not be made. The allocator's audit runs inside
// `allocateFuncRegisters` and aborts on a lost value — which is a third way out,
// and the one this function exists to contain.
[[noreturn]] void allocateUnderEveryConventionAndLeave() {
    std::size_t conventionsRun = 0;
    for (char const* targetName : {"x86_64", "arm64"}) {
        auto target = TargetSchema::loadShipped(targetName);
        if (!target.has_value()) std::exit(4);
        TargetSchema const& sch = **target;
        auto const probe = buildLandingProbe(sch);
        if (!probe.has_value()) std::exit(4);
        LandingProbe const& p = *probe;
        for (std::uint16_t ccIdx = 0; ccIdx < sch.callingConventions().size(); ++ccIdx) {
            ConventionFacts const facts = conventionFacts(sch, ccIdx);
            if (facts.volatileOrdinals.empty()) std::exit(4);
            std::array<LirGuardedRegion, 1> const regions{p.region()};
            DiagnosticReporter rep;
            auto const lv = analyzeLiveness(p.lir, regions, rep);
            if (!lv.has_value()) std::exit(4);
            LirFuncAllocation const alloc =
                allocateFuncRegisters(p.lir, sch, lv->perFunc[0], ccIdx, rep);
            if (!alloc.ok || !keptWhereACallKeepsIt(alloc.forVReg(p.g.id), facts)
                || !keptWhereACallKeepsIt(alloc.forVReg(p.f.id), facts)) {
                std::fprintf(stderr, "%s / %s: a value the landing block reads is not "
                                     "kept where a call would keep it\n",
                             targetName, sch.callingConvention(ccIdx)->name.c_str());
                std::exit(3);
            }
            ++conventionsRun;
        }
    }
    std::exit(conventionsRun >= 4 ? 0 : 4);
}

} // namespace

// THE CLAIM. Every shipped convention of both targets keeps what a landing block
// reads, and the allocator's audit never has to refuse: the child leaves with 0.
// A child that aborted is the audit catching an allocation the predicate let
// through; a child that left with 3 is an allocation both let through.
TEST(LandingRuleContained, EveryConventionKeepsWhatALandingBlockReads) {
    EXPECT_EXIT(allocateUnderEveryConventionAndLeave(), ::testing::ExitedWithCode(0), "")
        << "the allocation of a function with a guarded run did not finish with "
           "every value its landing block reads in a call-preserved register or a "
           "spill slot. A child killed by a signal or an abort is the allocator's "
           "own audit (`findLandingViolation`) refusing what the allocator chose — "
           "the predicate `rangeOutlivesAVolatileRegister` no longer asks about "
           "landings";
}

// THE CONTROL, differing by the region alone: the same module with NO region hands
// both values a VOLATILE register — there is no call in it, so nothing else keeps
// them out of one — and with no landing the audit has nothing to refuse, so this
// runs in-process. It is the premise that the claim above is the landing rule's
// doing, and the neighbour that must stay green whatever happens to that rule.
TEST(LandingRuleContained, WithNoRegionTheSameValuesTakeVolatileRegisters) {
    std::size_t conventionsRun = 0;
    for (char const* targetName : {"x86_64", "arm64"}) {
        auto target = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(target.has_value()) << targetName;
        TargetSchema const& sch = **target;
        auto const probe = buildLandingProbe(sch);
        ASSERT_TRUE(probe.has_value()) << targetName;
        LandingProbe const& p = *probe;
        for (std::uint16_t ccIdx = 0; ccIdx < sch.callingConventions().size(); ++ccIdx) {
            std::string const where =
                std::string{targetName} + " / " + sch.callingConvention(ccIdx)->name;
            ConventionFacts const facts = conventionFacts(sch, ccIdx);
            ASSERT_FALSE(facts.volatileOrdinals.empty()) << where;
            LirFuncLiveness const plain = analyzeFuncLiveness(p.lir, p.lir.funcAt(0));
            DiagnosticReporter rep;
            LirFuncAllocation const base =
                allocateFuncRegisters(p.lir, sch, plain, ccIdx, rep);
            ASSERT_TRUE(base.ok) << where;
            auto const* bg = base.forVReg(p.g.id);
            auto const* bf = base.forVReg(p.f.id);
            ASSERT_NE(bg, nullptr) << where;
            ASSERT_NE(bf, nullptr) << where;
            EXPECT_TRUE(!bg->isSpilled() && facts.volatileOrdinals.contains(
                                                static_cast<std::uint16_t>(bg->physReg().id)))
                << where << ": with no region the GPR value takes a volatile register";
            EXPECT_TRUE(!bf->isSpilled() && facts.volatileOrdinals.contains(
                                                static_cast<std::uint16_t>(bf->physReg().id)))
                << where << ": with no region the FPR value takes a volatile register";
            ++conventionsRun;
        }
    }
    EXPECT_GE(conventionsRun, 4u) << "both targets ship at least two conventions";
}
