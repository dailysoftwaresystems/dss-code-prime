// [[D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH]] — the
// COMPLEXITY pin of the branch-veneer placement.
//
// ★★★ WHAT IT REPLACED WAS SUPERLINEAR, AND IN MORE THAN ONE PLACE. The pass
// this row retired scanned EVERY function boundary for every out-of-reach
// branch (O(branches × functions)), scanned every standing veneer for every
// branch to find one to re-aim at (O(branches × veneers)), and inserted veneers
// into the function vector one `vector::insert` at a time (O(veneers ×
// functions) element moves). The references are linear or near it: ld64.lld
// states its design as "Single pass: O(n) on the number of call sites", and
// GNU ld and ld.lld both link a GiB of `.text` with veneers in ~0.3 s
// (✔MEASURED, lane `vn`, 2026-09-19, with a contention witness).
//
// ★★★ WHY THIS PIN COUNTS OPERATIONS AND DOES NOT TIME THE CLOCK. A wall-clock
// assertion is sized on the machine that wrote it and reds on the slowest leg
// that runs it, naming the wrong event (lane `cr`'s pattern:
// `test_lir_coalesce_anti_affinity_complexity.cpp`). `BranchVeneerWork` is the
// ALGORITHM'S OWN WORK — deterministic, host-independent, identical in Debug
// and Release — so a statement about it is a fact about the algorithm.
//
// ★★ THE FOUR ARMS, AND WHY NONE CAN GO VACUOUS.
//   * PREMISE — the subject really presents Θ(N) far branches, RE-DERIVED here
//     from the layout rather than read back from the planner's own counter.
//   * WORK, ABSOLUTE — the boundary pointer moves at most once per function,
//     every site is visited exactly once, the exact check visits each site and
//     veneer once.
//   * WORK, GROWTH — doubling the input doubles the work (≤ 2.75×; quadratic
//     scores 4.0).
//   * CONTROL — the DECISIONS scale exactly with the input (twice the veneers,
//     twice the reuses). Reverting to a quadratic search leaves them identical,
//     so this arm STAYS GREEN under that mutant — which is what makes the work
//     arms' red evidence about complexity, not about "the mutant broke it".

#include "core/types/target_schema.hpp"
#include "link/branch_reloc_geometry.hpp"
#include "link/branch_veneers.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace dss;

namespace {

// The subject is a TILE repeated `tiles` times, so that doubling the tile count
// doubles EVERY population in it exactly (no edge effect to explain away). A
// tile is 4200 functions of 64 KiB (262.5 MiB, ~2048 boundaries per reach):
//   * j < 2100 calls j + 2100 — 131 MiB on, out of reach FORWARD;
//   * j >= 2100 calls j - 2100 — out of reach BACKWARD;
//   * j < 1024 also calls the tile's LAST function (>= 198 MiB on), one shared
//     target — so REUSE is exercised at scale, 1023 times per tile.
// Every call is far; each gets a veneer except the shared-target calls, which
// share one per tile.
constexpr std::uint32_t kTile   = 4200;
constexpr std::uint32_t kSpan   = 2100;
constexpr std::uint32_t kShared = 1024;

struct Subject {
    linker::VeneerLayout layout;
    std::uint64_t        farSites = 0;  // RE-DERIVED, never read off the planner
};

Subject subject(std::uint32_t tiles) {
    Subject s;
    auto& L = s.layout;
    auto const tgt = TargetSchema::loadShipped("arm64");
    EXPECT_TRUE(tgt.has_value());
    auto const call26 = *link::relocFieldReach(*(*tgt)->relocationByName("call26"));
    L.reaches.push_back(call26);
    L.veneerSize  = 12;
    L.veneerReach = *link::relocFieldReach(*(*tgt)->relocationByName("adr_prel_pg_hi21"));

    constexpr std::uint64_t kFn = 64 * 1024;
    std::uint32_t const n = tiles * kTile;
    L.functionSizes.assign(n, kFn);
    for (std::uint32_t i = 0; i < n; ++i)
        L.targets.push_back(linker::VeneerLayoutTarget{i, 0, 0});

    auto const half = std::min(call26.maxDelta, -call26.minDelta) / 2;
    auto const note = [&](std::uint32_t f, std::uint32_t off, std::uint32_t t) {
        L.sites.push_back(linker::VeneerLayoutSite{f, off, t, 0, true});
        std::int64_t const d = static_cast<std::int64_t>(t * kFn)
                             - static_cast<std::int64_t>(f * kFn + off);
        if (d > half || d < -half) ++s.farSites;
    };
    for (std::uint32_t t = 0; t < tiles; ++t) {
        std::uint32_t const base = t * kTile;
        for (std::uint32_t j = 0; j < kTile; ++j) {
            note(base + j, 0, j < kSpan ? base + j + kSpan : base + j - kSpan);
            if (j < kShared) note(base + j, 4, base + kTile - 1);
        }
    }
    return s;
}

constexpr std::uint32_t kSmall = 4;   // ~1.03 GiB of `.text`
constexpr std::uint32_t kLarge = 8;   // ~2.05 GiB of `.text`
constexpr double kMaxGrowthForDoubledInput = 2.75;

struct Measured {
    linker::VeneerPlan  plan;
    linker::BranchVeneerWork verify;
    std::uint64_t sites = 0, functions = 0, farSites = 0;
};

Measured measure(std::uint32_t n) {
    Measured m;
    auto const s = subject(n);
    m.plan = linker::planBranchVeneers(s.layout);
    EXPECT_EQ(m.plan.status, linker::VeneerPlanStatus::Ok);
    auto const misfit = linker::findVeneerPlanMisfit(s.layout, m.plan, &m.verify);
    EXPECT_FALSE(misfit.has_value()) << "the plan must survive its own insertions";
    m.sites     = s.layout.sites.size();
    m.functions = s.layout.functionSizes.size();
    m.farSites  = s.farSites;
    return m;
}

}  // namespace

// ── THE PREMISE, FIRST ───────────────────────────────────────────────────
TEST(BranchVeneerComplexity, TheSubjectReallyPresentsLinearlyManyFarBranches) {
    auto const small = measure(kSmall);
    auto const large = measure(kLarge);
    ASSERT_GT(small.farSites, 0u)
        << "the subject presents no far branch — every other arm would be "
           "vacuous. Fix the SUBJECT; never relax a bound";
    EXPECT_EQ(small.farSites, static_cast<std::uint64_t>(kSmall) * (kTile + kShared))
        << "every call in the subject is meant to be far";
    EXPECT_EQ(large.farSites, 2u * small.farSites)
        << "doubling the tiles must double the far branches, or the claim below "
           "is about a different input than the one measured";
    ASSERT_GT(small.plan.veneers.size(), 0u)
        << "the subject needs no veneer — the placement work is never done";
}

// ── THE ABSOLUTE BOUND ───────────────────────────────────────────────────
TEST(BranchVeneerComplexity, PlacementWorkIsLinearInSitesAndFunctions) {
    for (auto const n : {kSmall, kLarge}) {
        auto const m = measure(n);
        EXPECT_EQ(m.plan.work.sitesExamined, m.sites)
            << "n=" << n << ": every branch site is visited exactly ONCE";
        EXPECT_LE(m.plan.work.pointerAdvances, m.functions)
            << "n=" << n << ": the boundary pointer moved "
            << m.plan.work.pointerAdvances << " times over " << m.functions
            << " functions. It only ever moves FORWARD, so it cannot move more "
               "than once per boundary; more means the search restarted per "
               "site — the O(branches x functions) scan this pass replaced. "
               "Never raise this bound";
        EXPECT_EQ(m.verify.sitesVerified, m.sites + m.plan.veneers.size())
            << "n=" << n << ": the exact check visits each site and each veneer "
               "exactly once";
        EXPECT_EQ(m.plan.work.veneersPlaced + m.plan.work.veneersReused
                      + (m.sites - m.plan.work.veneersPlaced
                         - m.plan.work.veneersReused),
                  m.sites);
    }
}

// ── THE GROWTH ARM ───────────────────────────────────────────────────────
TEST(BranchVeneerComplexity, DoublingTheInputDoublesTheWork) {
    auto const small = measure(kSmall);
    auto const large = measure(kLarge);
    auto const total = [](Measured const& m) {
        return static_cast<double>(m.plan.work.sitesExamined
                                   + m.plan.work.pointerAdvances
                                   + m.verify.sitesVerified + 1);
    };
    double const growth = total(large) / total(small);
    EXPECT_LE(growth, kMaxGrowthForDoubledInput)
        << "doubling the input multiplied the placement work by " << growth
        << "x (n=" << kSmall << ": sites " << small.plan.work.sitesExamined
        << ", pointer " << small.plan.work.pointerAdvances << "; n=" << kLarge
        << ": sites " << large.plan.work.sitesExamined << ", pointer "
        << large.plan.work.pointerAdvances << "). Linear scores ~2.0 and "
           "QUADRATIC scores 4.0";
    double const pointerGrowth =
        static_cast<double>(large.plan.work.pointerAdvances + 1)
        / static_cast<double>(small.plan.work.pointerAdvances + 1);
    EXPECT_LE(pointerGrowth, kMaxGrowthForDoubledInput)
        << "the boundary search alone grew " << pointerGrowth << "x";
}

// ── THE CONTROL: the decisions scale exactly ─────────────────────────────
TEST(BranchVeneerComplexity, TheDecisionsScaleWithTheInputControl) {
    auto const small = measure(kSmall);
    auto const large = measure(kLarge);
    ASSERT_GT(small.plan.veneers.size(), 0u);
    // The tile is uniform, so the decisions are an exact multiple of the tile
    // count. Per tile there are exactly 4200 distinct targets, and the shared
    // one (the tile's last function) is ALSO the forward target of j = 2099 —
    // so the 1024 shared-target calls place ONE veneer and reuse it 1023
    // times, and j = 2099's forward call reuses it too instead of placing its
    // own: 4200 veneers and 1024 reuses per tile.
    EXPECT_EQ(small.plan.veneers.size(), static_cast<std::size_t>(kSmall) * kTile)
        << "per tile: one veneer per distinct far target";
    EXPECT_EQ(small.plan.work.veneersReused,
              static_cast<std::uint64_t>(kSmall) * kShared)
        << "per tile: 1023 shared-target calls + j = 2099's forward call, all "
           "served by the one shared veneer";
    EXPECT_EQ(large.plan.veneers.size(), 2u * small.plan.veneers.size())
        << "doubling the input changed the placement DECISIONS' count — a "
           "correctness question, not a performance one";
    EXPECT_EQ(large.plan.work.veneersReused, 2u * small.plan.work.veneersReused);
}
