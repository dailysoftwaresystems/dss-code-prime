// [[D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH]]
// (parent mechanism: [[D-LK-AARCH64-CALL26-BEYOND-RANGE-HAS-NO-VENEER]])
// The link-tier branch-veneer pass. Its sibling one tier down is
// [[D-CSUBSET-LONG-BRANCH]] (assembler islands, intra-function).
//
// ★★★ EVERY EXPECTATION HERE WAS READ OFF A REFERENCE LINKER FIRST. Lane `vn`
// (2026-09-19) measured GNU ld 2.42, ld.lld 18.1.3 and ld64.lld 18.1.3
// separately, on the same shapes these arms build; each arm names the reference
// behaviour it pins. The transcripts are cited in the row.
//
// ★★ THE PLANNER IS PINNED AT THE REAL ±128 MiB REACH WITHOUT ALLOCATING A BYTE
// OF FILLER. It reads function SIZES and branch sites, so a 300 MiB `.text` is a
// few numbers in a `VeneerLayout`. `injectBranchVeneers` builds exactly that
// model from a module and runs exactly that planner, so a planner arm is an arm
// on the production decision, not on a copy of it. Only the arms that must see
// real BYTES — the assembled body, the re-aimed relocations — build a module,
// and they share ONE 144 MiB zero-filled filler.
//
// ⓘ WHY NOT A TEST TARGET WITH A TINY REACH. The reach is not declared in a
// target document: it is the relocation FORMULA's geometry
// (`branch_reloc_geometry.hpp`), and the loader admits only 4- and 8-byte fields.
// A tiny-reach test target would need a formula no real target has. The model
// makes the real reach free instead.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/branch_reloc_geometry.hpp"
#include "link/branch_veneers.hpp"
#include "link/import_call_stub_layout.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace dss;
using linker::kVeneerTargetIsStub;
using linker::VeneerLayout;
using linker::VeneerLayoutSite;
using linker::VeneerLayoutTarget;
using linker::VeneerPlan;
using linker::VeneerPlanStatus;

namespace {

constexpr std::uint64_t kMiB = 1024u * 1024u;

// The shipped arm64 target: the vocabulary under test is its own.
TargetSchema const& arm64() {
    static auto const loaded = TargetSchema::loadShipped("arm64");
    EXPECT_TRUE(loaded.has_value()) << "arm64 target did not load";
    return **loaded;
}

// The `call26` window, derived from the formula exactly as the pass derives it
// — never from a memory of "128 MiB".
link::RelocReach call26Window() {
    auto const* row = arm64().relocationByName("call26");
    EXPECT_NE(row, nullptr);
    return *link::relocFieldReach(*row);
}

// The body the shipped vocabulary elects, as a planner input: 12 bytes, ADRP
// reach measured from the veneer's start (the ADRP is its first word).
link::RelocReach adrpWindow() {
    auto const* row = arm64().relocationByName("adr_prel_pg_hi21");
    EXPECT_NE(row, nullptr);
    return *link::relocFieldReach(*row);
}
constexpr std::uint64_t kBodyBytes = 12;

// A model builder that reads like the layout it describes.
struct Model {
    VeneerLayout L;
    Model() {
        L.reaches.push_back(call26Window());
        L.veneerSize  = kBodyBytes;
        L.veneerReach = adrpWindow();
    }
    std::uint32_t fn(std::uint64_t size) {
        L.functionSizes.push_back(size);
        return static_cast<std::uint32_t>(L.functionSizes.size() - 1);
    }
    std::uint32_t toFunction(std::uint32_t f, std::int64_t addend = 0) {
        L.targets.push_back(VeneerLayoutTarget{f, 0, addend});
        return static_cast<std::uint32_t>(L.targets.size() - 1);
    }
    std::uint32_t toStub(std::uint64_t pastTextEnd) {
        L.targets.push_back(VeneerLayoutTarget{kVeneerTargetIsStub, pastTextEnd, 0});
        return static_cast<std::uint32_t>(L.targets.size() - 1);
    }
    void branch(std::uint32_t f, std::uint32_t offset, std::uint32_t target,
                bool routable = true) {
        L.sites.push_back(VeneerLayoutSite{f, offset, target, 0, routable});
    }
};

// A plan is only correct if it survives its own insertions: every arm that
// accepts a plan re-measures it exactly on the layout it produces.
void expectExact(VeneerLayout const& L, VeneerPlan const& plan) {
    auto const misfit = linker::findVeneerPlanMisfit(L, plan);
    EXPECT_FALSE(misfit.has_value())
        << "the plan does not survive its own insertions: "
        << (misfit->isVeneer ? "veneer " : "site ") << misfit->index
        << " is " << misfit->delta << " bytes from where it aims";
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// THE CONTROL — a branch that reaches is not touched.
// ─────────────────────────────────────────────────────────────────────────
TEST(LinkBranchVeneerPlanner, ABranchInReachGetsNoVeneerAndNoWork) {
    Model m;
    auto const caller = m.fn(64);
    auto const filler = m.fn(64 * kMiB);
    auto const callee = m.fn(16);
    (void)filler;
    m.branch(caller, 8, m.toFunction(callee));
    auto const plan = linker::planBranchVeneers(m.L);
    ASSERT_EQ(plan.status, VeneerPlanStatus::Ok);
    EXPECT_TRUE(plan.veneers.empty())
        << "64 MiB is inside +-128 MiB — a veneer here would bloat every image";
    EXPECT_EQ(plan.siteVeneer[0], -1);
    EXPECT_EQ(plan.work.farSites, 1u)
        << "64 MiB is past HALF the reach, so the site counts as far — the "
           "margin's base — and is still decided direct";
    EXPECT_EQ(plan.margin, kBodyBytes)
        << "one far site can add at most one body's worth of bytes";
    expectExact(m.L, plan);
}

// ─────────────────────────────────────────────────────────────────────────
// PLACEMENT — the furthest boundary AHEAD within reach (ld64.lld's rule).
// ─────────────────────────────────────────────────────────────────────────
TEST(LinkBranchVeneerPlanner, AVeneerStandsAtTheFurthestBoundaryAheadInReach) {
    Model m;
    auto const caller = m.fn(64);
    (void)m.fn(100 * kMiB);                 // f1
    auto const f2 = m.fn(20 * kMiB);        // starts at ~100 MiB: in reach
    auto const f3 = m.fn(30 * kMiB);        // starts at ~120 MiB: in reach
    auto const callee = m.fn(16);           // starts at ~150 MiB: out of reach
    (void)f2;
    m.branch(caller, 8, m.toFunction(callee));
    auto const plan = linker::planBranchVeneers(m.L);
    ASSERT_EQ(plan.status, VeneerPlanStatus::Ok);
    ASSERT_EQ(plan.veneers.size(), 1u);
    EXPECT_EQ(plan.veneers[0].boundary, f3)
        << "the veneer must stand at the FURTHEST boundary still in reach of "
           "the call (before f3, ~120 MiB on), where it can serve every later "
           "call as far as possible — not at the first boundary it meets";
    EXPECT_EQ(plan.siteVeneer[0], 0);
    EXPECT_EQ(plan.work.behindPlacements, 0u);
    expectExact(m.L, plan);
}

// ─────────────────────────────────────────────────────────────────────────
// SHARING — one veneer per target per reachable window (all three refs).
// ─────────────────────────────────────────────────────────────────────────
//
// ✔MEASURED shape (f): a1,a2 │ 144 MiB │ m1,m2 │ 144 MiB │ far2 — GNU ld and
// ld.lld each make exactly TWO veneers for far2, one per region, shared.
TEST(LinkBranchVeneerPlanner, BranchesInOneWindowShareOneVeneerPerTarget) {
    Model m;
    auto const a = m.fn(64);                // a1, a2 live here
    (void)m.fn(144 * kMiB);
    auto const mid = m.fn(64);              // m1, m2 live here
    (void)m.fn(144 * kMiB);
    auto const far2 = m.fn(16);
    auto const t = m.toFunction(far2);
    m.branch(a, 0, t);
    m.branch(a, 4, t);
    m.branch(mid, 0, t);
    m.branch(mid, 4, t);
    auto const plan = linker::planBranchVeneers(m.L);
    ASSERT_EQ(plan.status, VeneerPlanStatus::Ok);
    EXPECT_EQ(plan.veneers.size(), 2u)
        << "two regions a full reach apart need exactly two veneers for one "
           "target — the count GNU ld 2.42 and ld.lld 18.1.3 both produce";
    EXPECT_EQ(plan.work.veneersPlaced, 2u);
    EXPECT_EQ(plan.work.veneersReused, 2u);
    EXPECT_EQ(plan.siteVeneer[0], plan.siteVeneer[1]) << "a1 and a2 share";
    EXPECT_EQ(plan.siteVeneer[2], plan.siteVeneer[3]) << "m1 and m2 share";
    EXPECT_NE(plan.siteVeneer[0], plan.siteVeneer[2]);
    expectExact(m.L, plan);
}

// Two branches to the SAME symbol at different addends are different targets:
// one veneer each (ld.lld keys its thunks on symbol + addend).
TEST(LinkBranchVeneerPlanner, ADifferentAddendIsADifferentTarget) {
    Model m;
    auto const caller = m.fn(64);
    (void)m.fn(144 * kMiB);
    auto const callee = m.fn(64);
    m.branch(caller, 0, m.toFunction(callee, 0));
    m.branch(caller, 4, m.toFunction(callee, 8));
    auto const plan = linker::planBranchVeneers(m.L);
    ASSERT_EQ(plan.status, VeneerPlanStatus::Ok);
    EXPECT_EQ(plan.veneers.size(), 2u);
    expectExact(m.L, plan);
}

// ─────────────────────────────────────────────────────────────────────────
// BACKWARD — a far call to something BEFORE it is carried the same way.
// ─────────────────────────────────────────────────────────────────────────
TEST(LinkBranchVeneerPlanner, ABackwardFarCallIsCarried) {
    Model m;
    auto const near = m.fn(16);
    (void)m.fn(144 * kMiB);
    auto const tail = m.fn(64);
    m.branch(tail, 8, m.toFunction(near));
    auto const plan = linker::planBranchVeneers(m.L);
    ASSERT_EQ(plan.status, VeneerPlanStatus::Ok);
    ASSERT_EQ(plan.veneers.size(), 1u);
    EXPECT_GE(plan.veneers[0].boundary, tail)
        << "the only boundaries in reach of a call in `tail` are its own";
    expectExact(m.L, plan);
}

// ─────────────────────────────────────────────────────────────────────────
// BEHIND — the call is near the TOP of a function longer than the reach.
// ─────────────────────────────────────────────────────────────────────────
//
// ✔MEASURED: ld.lld 18.1.3 LINKS this (a thunk 4 bytes before the big section;
// the image ran, exit 42); GNU ld 2.42 refuses (*relocation truncated to fit*)
// and ld64.lld 18.1.3 refuses (*thunk range overrun*). One working reference
// makes it required.
TEST(LinkBranchVeneerPlanner, ACallAtTheTopOfAHugeFunctionUsesTheBoundaryBehindIt) {
    Model m;
    (void)m.fn(16);                          // _start
    auto const big = m.fn(144 * kMiB);       // bl callee at offset 4
    auto const callee = m.fn(16);
    m.branch(big, 4, m.toFunction(callee));
    auto const plan = linker::planBranchVeneers(m.L);
    ASSERT_EQ(plan.status, VeneerPlanStatus::Ok);
    ASSERT_EQ(plan.veneers.size(), 1u);
    EXPECT_EQ(plan.veneers[0].boundary, big)
        << "no boundary ahead is within reach (the function's end is 144 MiB "
           "on); the boundary BEHIND the call, 4 bytes back, is";
    EXPECT_EQ(plan.work.behindPlacements, 1u);
    expectExact(m.L, plan);
}

// ─────────────────────────────────────────────────────────────────────────
// THE SHAPE NO REFERENCE LINKS — refused, by name, at their boundary.
// ─────────────────────────────────────────────────────────────────────────
//
// ✔MEASURED: a call 144 MiB from BOTH ends of its own section — GNU ld 2.42
// *relocation truncated to fit*, ld.lld 18.1.3 *InputSection too large for
// range extension thunk*.
TEST(LinkBranchVeneerPlanner, ACallBuriedAFullReachFromBothEndsHasNoSite) {
    Model m;
    auto const R = static_cast<std::uint64_t>(call26Window().maxDelta);
    (void)m.fn(16);
    auto const big = m.fn(2 * R + 64);
    auto const callee = m.fn(16);
    m.branch(big, static_cast<std::uint32_t>(R + 32), m.toFunction(callee));
    auto const plan = linker::planBranchVeneers(m.L);
    EXPECT_EQ(plan.status, VeneerPlanStatus::NoBoundaryInReach)
        << "no boundary stands within reach of the call on either side; no "
           "veneer body of any shape changes that";
    EXPECT_EQ(plan.failedSite, 0u);
}

// ─────────────────────────────────────────────────────────────────────────
// THE SUBJECT — the synthetic entry's call to its process-exit import.
// ─────────────────────────────────────────────────────────────────────────
//
// `_start` sits at `.text` offset 0; the writer puts `exit`'s stub past ALL of
// `.text`. ✔MEASURED before this change: a 212 992-statement program refused for
// a `call26` of 139 722 816 bytes. ld.lld and ld64.lld veneer this call at the
// boundary right after the caller (GNU ld avoids it by putting `.plt` first).
TEST(LinkBranchVeneerPlanner, TheEntrysCallToItsExitImportIsCarriedFromOffsetZero) {
    Model m;
    auto const start = m.fn(24);             // the entry trampoline
    auto const main  = m.fn(139 * kMiB);     // the program
    (void)main;
    auto const exitStub = m.toStub(64);      // `.rodata` + alignment + slot
    m.branch(start, 16, exitStub);
    auto const plan = linker::planBranchVeneers(m.L);
    ASSERT_EQ(plan.status, VeneerPlanStatus::Ok);
    ASSERT_EQ(plan.veneers.size(), 1u);
    EXPECT_EQ(plan.veneers[0].boundary, 1u)
        << "the only boundary in reach of the entry's call is the one right "
           "after the entry itself";
    EXPECT_EQ(plan.veneers[0].target, exitStub);
    expectExact(m.L, plan);
}

// The mirror: a call to an import from the END of a large `.text` reaches the
// stub directly — which is why ld.lld and ld64.lld need no veneer there.
TEST(LinkBranchVeneerPlanner, ACallToAnImportFromTheEndOfTextReachesDirectly) {
    Model m;
    (void)m.fn(139 * kMiB);
    auto const tail = m.fn(64);
    m.branch(tail, 8, m.toStub(64));
    auto const plan = linker::planBranchVeneers(m.L);
    ASSERT_EQ(plan.status, VeneerPlanStatus::Ok);
    EXPECT_TRUE(plan.veneers.empty());
    expectExact(m.L, plan);
}

// ─────────────────────────────────────────────────────────────────────────
// WHAT THE ABI DOES NOT ROUTE — refused like both references refuse it.
// ─────────────────────────────────────────────────────────────────────────
//
// ✔MEASURED: an out-of-reach CONDBR19/TSTBR14 — GNU ld *relocation truncated to
// fit*, ld.lld *out of range*. AAELF64 lets a linker veneer only CALL26/JUMP26.
TEST(LinkBranchVeneerPlanner, ANonRoutableBranchOutOfReachIsRefused) {
    Model m;
    auto const caller = m.fn(64);
    (void)m.fn(144 * kMiB);
    auto const callee = m.fn(16);
    m.branch(caller, 8, m.toFunction(callee), /*routable=*/false);
    auto const plan = linker::planBranchVeneers(m.L);
    EXPECT_EQ(plan.status, VeneerPlanStatus::NotRoutable);
    EXPECT_TRUE(plan.veneers.empty());
}

// A body whose own fields cannot reach the target from where it must stand is
// refused — the > ±4 GiB edge, measured here on a narrowed body window so the
// arm costs nothing.
TEST(LinkBranchVeneerPlanner, ABodyThatCannotReachTheTargetIsRefused) {
    Model m;
    m.L.veneerReach = link::RelocReach{-static_cast<std::int64_t>(16 * kMiB),
                                       static_cast<std::int64_t>(16 * kMiB)};
    auto const caller = m.fn(64);
    (void)m.fn(144 * kMiB);
    auto const callee = m.fn(16);
    m.branch(caller, 8, m.toFunction(callee));
    auto const plan = linker::planBranchVeneers(m.L);
    EXPECT_EQ(plan.status, VeneerPlanStatus::BodyOutOfReach);
}

// ─────────────────────────────────────────────────────────────────────────
// ONE PASS IS EXACT — and the exact check is not vacuous.
// ─────────────────────────────────────────────────────────────────────────
//
// The adversarial shape for a one-pass planner: 4096 functions of 64 KiB, and
// every function i < 2048 calls function i + 2048 — exactly 128 MiB on, ONE
// word past the reach. Each veneer lands at the furthest boundary in reach,
// 64 KiB short of its target, so between every call and its veneer stand up
// to 2046 OTHER veneers, each pushing the pair apart by 12 bytes. The margin
// (far sites × body size) is what absorbs exactly that; the plan must survive
// its own insertions with no second pass.
TEST(LinkBranchVeneerPlanner, EdgeBranchesSurviveEveryVeneerInsertedBetweenThemAndTheirVeneers) {
    Model m;
    constexpr std::uint64_t kFn   = 64 * 1024;
    constexpr std::uint32_t kFns  = 4096;
    constexpr std::uint32_t kSpan = 2048;  // 2048 x 64 KiB = 128 MiB
    for (std::uint32_t i = 0; i < kFns; ++i) (void)m.fn(kFn);
    for (std::uint32_t i = 0; i + kSpan < kFns; ++i)
        m.branch(i, 0, m.toFunction(i + kSpan));
    auto const plan = linker::planBranchVeneers(m.L);
    ASSERT_EQ(plan.status, VeneerPlanStatus::Ok);
    EXPECT_FALSE(plan.marginCapped);
    ASSERT_EQ(plan.veneers.size(), kFns - kSpan)
        << "every call is one word out of reach and has a target of its own";
    for (std::uint32_t i = 0; i < plan.veneers.size(); ++i) {
        EXPECT_EQ(plan.veneers[i].boundary, i + kSpan - 1)
            << "veneer " << i << " must stand at the furthest boundary in "
               "reach of its call — the one right before its target";
        if (::testing::Test::HasFailure()) break;
    }
    expectExact(m.L, plan);
}

// ★ THE MARGIN, AT THE RAZOR'S EDGE. Call A reaches its target T with 8 bytes
// to spare. A LATER call, from the next function, needs a veneer, and the
// furthest boundary in ITS reach is the one right before T — so that veneer is
// inserted between A and T and pushes T 12 bytes further, out of A's reach. A
// planner that judged A against the raw reach would leave it direct and emit a
// broken branch; judged against the reach minus the margin, A gets a veneer of
// its own and the plan survives every insertion.
TEST(LinkBranchVeneerPlanner, ACallThatReachesOnlyBeforeLaterInsertionsIsJudgedAgainstTheMargin) {
    Model m;
    auto const R = static_cast<std::uint64_t>(call26Window().maxDelta);
    auto const a  = m.fn(64);                 // site A at offset 0
    auto const f1 = m.fn(64);                 // site B at offset 0 (starts at 64)
    (void)m.fn(R - 8 - 128);                  // T then starts at exactly R - 8
    auto const t  = m.fn(256);                // the boundary AFTER T is out of B's reach
    (void)m.fn(144 * kMiB);
    auto const z  = m.fn(16);                 // B's target: far beyond everything
    m.branch(a, 0, m.toFunction(t));
    m.branch(f1, 0, m.toFunction(z));
    auto const plan = linker::planBranchVeneers(m.L);
    ASSERT_EQ(plan.status, VeneerPlanStatus::Ok);
    EXPECT_GE(plan.siteVeneer[0], 0)
        << "A reaches T by 8 bytes today, but B's veneer will stand before T; "
           "only a decision taken against the margin sees that A must not stay "
           "direct";
    ASSERT_GE(plan.siteVeneer[1], 0);
    EXPECT_EQ(plan.veneers[static_cast<std::size_t>(plan.siteVeneer[1])].boundary, t)
        << "B's veneer stands at the furthest boundary in its reach: right "
           "before T — the insertion that would break a direct A";
    expectExact(m.L, plan);
}

// ─────────────────────────────────────────────────────────────────────────
// THE CAPPED MARGIN — the one regime where one pass is not exact by
// construction, and what happens there.
// ─────────────────────────────────────────────────────────────────────────
//
// The margin is (far branches × body size), capped at half the narrowest reach.
// At the real reach the cap engages only past 5 592 405 far branches, so these
// arms shrink the REACH instead: a reach class is a model value and the planner
// reads nothing else, so ±1 KiB engages the cap at 43 far branches. Capped,
// every decision is taken against HALF the reach and the exact check decides:
// the plan stands while the veneers between any branch and its aim fit in that
// half, and past it the check names the first branch that no longer reaches.
namespace {

// K calls from one function, call k at byte 4k, each to a target of its own
// behind a 4 KiB function: every veneer lands in the ONE island right after the
// calling function, in call order, so call k's veneer stands 4K + 8k bytes on.
Model cappedMarginSubject(std::uint32_t k) {
    Model m;
    m.L.reaches     = {link::RelocReach{-1024, 1020}};
    m.L.veneerReach = link::RelocReach{std::numeric_limits<std::int64_t>::min(),
                                       std::numeric_limits<std::int64_t>::max()};
    auto const caller = m.fn(4u * k);
    (void)m.fn(4096);
    std::vector<std::uint32_t> targets;
    for (std::uint32_t i = 0; i < k; ++i) targets.push_back(m.toFunction(m.fn(4)));
    for (std::uint32_t i = 0; i < k; ++i) m.branch(caller, 4u * i, targets[i]);
    return m;
}

}  // namespace

// 85 far calls want 1020 bytes of margin — the whole forward reach, which would
// leave no boundary ahead in reach of any call and refuse the link. Capped at
// 510, every veneer lands right after the caller, and the farthest call is
// 340 + 8 × 84 = 1012 bytes from its veneer: in reach, with 8 to spare.
TEST(LinkBranchVeneerPlanner, ACappedMarginStillLinksWhileTheVeneersFitHalfTheReach) {
    auto m = cappedMarginSubject(85);
    auto const plan = linker::planBranchVeneers(m.L);
    ASSERT_EQ(plan.status, VeneerPlanStatus::Ok);
    EXPECT_TRUE(plan.marginCapped);
    EXPECT_EQ(plan.margin, 510u) << "half of the narrower side of [-1024, 1020]";
    ASSERT_EQ(plan.veneers.size(), 85u);
    for (auto const& v : plan.veneers) {
        EXPECT_EQ(v.boundary, 1u)
            << "decided against half the reach, the boundary right after the "
               "caller is in reach of every call";
        if (::testing::Test::HasFailure()) break;
    }
    expectExact(m.L, plan);
}

// 127 far calls put 1524 bytes of veneers in one island: call k's veneer stands
// 508 + 8k bytes on, past the 1020-byte reach from k = 65. The planner cannot
// see that; the exact check must, and name call 65.
TEST(LinkBranchVeneerPlanner, ACappedMarginTheVeneersOutgrowIsCaughtByTheExactCheck) {
    auto m = cappedMarginSubject(127);
    auto const plan = linker::planBranchVeneers(m.L);
    ASSERT_EQ(plan.status, VeneerPlanStatus::Ok);
    EXPECT_TRUE(plan.marginCapped);
    auto const misfit = linker::findVeneerPlanMisfit(m.L, plan);
    ASSERT_TRUE(misfit.has_value())
        << "more than half the reach of veneers stands between a call and its "
           "aim; a plan that does not reach must never be blessed";
    EXPECT_FALSE(misfit->isVeneer);
    EXPECT_EQ(misfit->index, 65u);
    EXPECT_EQ(misfit->delta, 508 + 8 * 65);
}

// CONTROL, the same shape under the cap: 42 far calls want 504 bytes, the
// margin is exactly that, and the plan is exact by construction.
TEST(LinkBranchVeneerPlanner, AnUncappedMarginIsTheFarBranchesTimesTheBody) {
    auto m = cappedMarginSubject(42);
    auto const plan = linker::planBranchVeneers(m.L);
    ASSERT_EQ(plan.status, VeneerPlanStatus::Ok);
    EXPECT_FALSE(plan.marginCapped);
    EXPECT_EQ(plan.work.farSites, 42u);
    EXPECT_EQ(plan.margin, 42u * kBodyBytes);
    expectExact(m.L, plan);
}

// The exact check must be able to say NO: a hand-made plan that aims a branch
// at a veneer a full reach away is caught, by index.
TEST(LinkBranchVeneerPlanner, TheExactCheckCatchesAVeneerOutOfItsBranchsReach) {
    Model m;
    auto const caller = m.fn(64);
    (void)m.fn(144 * kMiB);
    auto const callee = m.fn(16);
    auto const t = m.toFunction(callee);
    m.branch(caller, 8, t);
    VeneerPlan bad;
    bad.siteVeneer = {0};
    bad.veneers.push_back(linker::PlannedVeneer{2, t});  // right before callee
    auto const misfit = linker::findVeneerPlanMisfit(m.L, bad);
    ASSERT_TRUE(misfit.has_value())
        << "a veneer 144 MiB from its branch cannot be reached; the check that "
           "blesses every accepted plan must be able to refuse one";
    EXPECT_FALSE(misfit->isVeneer);
    EXPECT_EQ(misfit->index, 0u);
}

// ─────────────────────────────────────────────────────────────────────────
// THE FULL PATH — real bytes, the shipped vocabulary, one shared filler.
// ─────────────────────────────────────────────────────────────────────────
namespace {

constexpr std::uint32_t kBL = 0x94000000u;
constexpr std::uint32_t kRet = 0xD65F03C0u;

AssembledFunction word(SymbolId sym, std::uint32_t w) {
    AssembledFunction fn;
    fn.symbol = sym;
    fn.bytes  = {static_cast<std::uint8_t>(w & 0xFFu),
                 static_cast<std::uint8_t>((w >> 8) & 0xFFu),
                 static_cast<std::uint8_t>((w >> 16) & 0xFFu),
                 static_cast<std::uint8_t>((w >> 24) & 0xFFu)};
    return fn;
}

// entry: `BL main` @0 and `BL exit@stub` @4 │ main (`RET`) │ 144 MiB filler │
// callee (`RET`) — the subject's shape: the entry's own import call reaches
// past the whole image.
struct EntryShaped {
    AssembledModule            module;
    link::ImportCallStubLayout stubs;
    SymbolId entry{10}, main{11}, filler{12}, callee{13}, exitImport{50};
};

EntryShaped buildEntryShaped() {
    EntryShaped s;
    auto const* call26 = arm64().relocationByName("call26");
    EXPECT_NE(call26, nullptr);
    AssembledFunction entry;
    entry.symbol = s.entry;
    for (auto const w : {kBL, kBL, kRet}) {
        auto const one = word(s.entry, w);
        entry.bytes.insert(entry.bytes.end(), one.bytes.begin(), one.bytes.end());
    }
    entry.relocations.push_back(Relocation{0u, s.main, call26->kind, 0});
    entry.relocations.push_back(Relocation{4u, s.exitImport, call26->kind, 0});
    s.module.functions.push_back(std::move(entry));
    s.module.functions.push_back(word(s.main, kRet));
    AssembledFunction filler;
    filler.symbol = s.filler;
    filler.bytes.resize(static_cast<std::size_t>(144 * kMiB), 0u);
    s.module.functions.push_back(std::move(filler));
    s.module.functions.push_back(word(s.callee, kRet));
    s.module.expectedFuncCount = s.module.functions.size();
    s.module.imageEntryOverride = 0;
    ExternImport exitImp;
    exitImp.symbol      = s.exitImport;
    exitImp.mangledName = "exit";
    exitImp.libraryPath = "libc.so.6";
    s.module.externImports.push_back(exitImp);
    s.stubs.maxPastTextEnd.emplace(s.exitImport, 64u);
    return s;
}

std::uint32_t wordAt(std::vector<std::uint8_t> const& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at])
         | (static_cast<std::uint32_t>(b[at + 1]) << 8)
         | (static_cast<std::uint32_t>(b[at + 2]) << 16)
         | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

}  // namespace

// ★★★ THE HEADLINE, ON REAL BYTES. Before this change the pass never saw the
// import-bound call (its target is no function of the module) and the writer
// refused the image. Now the writer's stub layout makes it a target like any
// other, and the veneer is the reference body, byte for byte.
TEST(LinkBranchVeneer, TheEntrysImportCallIsCarriedByTheReferenceBody) {
    auto s = buildEntryShaped();
    auto const& target = arm64();
    ASSERT_TRUE(linker::branchVeneersNeeded(s.module, target, s.stubs))
        << "the entry's call to its exit import spans 144 MiB of `.text`; the "
           "pass must SEE it through the writer's stub layout";
    // CONTROL — the SAME module without the stub layout: the call is invisible
    // (nothing says where the stub lands), which is exactly the blind spot the
    // stub layout exists to close.
    EXPECT_FALSE(linker::branchVeneersNeeded(s.module, target,
                                             link::ImportCallStubLayout{}))
        << "CONTROL: with no stub layout the import is no placeable target, so a "
           "`true` above could only have come from the layout";

    DiagnosticReporter rep;
    linker::BranchVeneerWork work;
    ASSERT_TRUE(linker::injectBranchVeneers(s.module, target, s.stubs, rep, &work));
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(linker::branchVeneersNeeded(s.module, target, s.stubs))
        << "after the pass every branch must reach where it points";

    // Exactly one veneer, at the FURTHEST boundary still in reach of the call:
    // after `main`, before the 144 MiB filler (the next boundary, after the
    // filler, is out of reach).
    ASSERT_EQ(s.module.functions.size(), 5u);
    EXPECT_EQ(work.veneersPlaced, 1u);
    EXPECT_EQ(s.module.functions[0].symbol.v, s.entry.v);
    EXPECT_EQ(s.module.functions[1].symbol.v, s.main.v);
    auto const& veneer = s.module.functions[2];
    EXPECT_EQ(s.module.functions[3].symbol.v, s.filler.v);

    // THE REFERENCE BODY: `adrp x16, T; add x16, x16, :lo12:T; br x16` —
    // ✔MEASURED identical words in GNU ld 2.42, ld.lld 18.1.3 (PIE) and
    // ld64.lld 18.1.3. Built from the target's own `lea` and `jmp_indirect`
    // rows, never spelled in the linker.
    ASSERT_EQ(veneer.bytes.size(), 12u);
    EXPECT_EQ(wordAt(veneer.bytes, 0), 0x90000010u) << "adrp x16, <page>";
    EXPECT_EQ(wordAt(veneer.bytes, 4), 0x91000210u) << "add x16, x16, #<lo12>";
    EXPECT_EQ(wordAt(veneer.bytes, 8), 0xD61F0200u) << "br x16";
    ASSERT_EQ(veneer.relocations.size(), 2u);
    EXPECT_EQ(veneer.relocations[0].offset, 0u);
    EXPECT_EQ(veneer.relocations[0].kind,
              target.relocationByName("adr_prel_pg_hi21")->kind);
    EXPECT_EQ(veneer.relocations[1].offset, 4u);
    EXPECT_EQ(veneer.relocations[1].kind,
              target.relocationByName("add_abs_lo12_nc")->kind);
    for (auto const& r : veneer.relocations)
        EXPECT_EQ(r.target.v, s.exitImport.v)
            << "the veneer finishes the trip to the IMPORT, not to a function";

    // The entry's call is re-aimed at the veneer; its call to main is not.
    auto const& entryRelocs = s.module.functions[0].relocations;
    EXPECT_EQ(entryRelocs[0].target.v, s.main.v)
        << "a call that reached was re-pointed";
    EXPECT_EQ(entryRelocs[1].target.v, veneer.symbol.v);
    EXPECT_EQ(entryRelocs[1].addend, 0);

    // The image still starts at the entry, and the count moved with the veneer.
    ASSERT_TRUE(s.module.imageEntryOverride.has_value());
    EXPECT_EQ(*s.module.imageEntryOverride, 0u);
    EXPECT_EQ(s.module.expectedFuncCount, s.module.functions.size());
}

// A module whose branches all reach is returned untouched.
TEST(LinkBranchVeneer, AModuleThatReachesIsUntouched) {
    AssembledModule module;
    auto const* call26 = arm64().relocationByName("call26");
    ASSERT_NE(call26, nullptr);
    auto caller = word(SymbolId{1}, kBL);
    caller.relocations.push_back(Relocation{0u, SymbolId{2}, call26->kind, 0});
    module.functions.push_back(std::move(caller));
    module.functions.push_back(word(SymbolId{2}, kRet));
    module.expectedFuncCount = 2;
    EXPECT_FALSE(linker::branchVeneersNeeded(module, arm64(),
                                             link::ImportCallStubLayout{}));
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectBranchVeneers(module, arm64(),
                                            link::ImportCallStubLayout{}, rep));
    EXPECT_EQ(module.functions.size(), 2u);
    EXPECT_EQ(module.functions[0].relocations[0].target.v, 2u);
}

// A target that declares NO veneer vocabulary refuses by name — it is never
// handed a body it did not declare, and never a clobber it did not grant.
TEST(LinkBranchVeneer, ATargetWithNoVocabularyRefusesByName) {
    constexpr char const* kNoVocabulary = R"({
      "dssTargetVersion": 1,
      "target": {"name":"no_link_veneers"},
      "relocations":[
        { "name": "call26", "kind": 1, "formula": "aarch64_call26" }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })";
    auto bare = TargetSchema::loadFromText(kNoVocabulary);
    ASSERT_TRUE(bare.has_value());
    ASSERT_EQ((*bare)->linkVeneers(), nullptr);

    auto s = buildEntryShaped();
    // Aim the entry's second call at the far callee instead of the import, so
    // the out-of-reach branch is a plain module call.
    s.module.functions[0].relocations[1].target = s.callee;
    ASSERT_TRUE(linker::branchVeneersNeeded(s.module, **bare, s.stubs));
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::injectBranchVeneers(s.module, **bare, s.stubs, rep));
    bool namedTheKey = false;
    for (auto const& d : rep.all())
        if (d.actual.find("`linkVeneers`") != std::string::npos) namedTheKey = true;
    EXPECT_TRUE(namedTheKey)
        << "the refusal must name the vocabulary the target did not declare";
}
