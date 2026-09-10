// ── THE UNDO JOURNAL, PINNED ON ITS OWN ──────────────────────────────────
//
// `core/types/speculation_trail.hpp` is what makes a speculative checkpoint
// O(1) instead of O(depth): the containers stay ordinary and contiguous, and
// only the values a mutation SUPERSEDES are journaled. The whole design rests
// on one claim that is easy to state and easy to get subtly wrong —
//
//   restoring a LIFO stack to a mark needs NO record for a push, one record
//   per pop, and one per in-place write; replaying those records in REVERSE
//   chronological order after resizing to the mark's size is EXACT.
//
// — so this file attacks that claim directly, with the interleavings that
// break a naive implementation: a stack drained BELOW the mark and refilled
// with different values, the same slot superseded several times, an in-place
// write followed by a truncation past it, and a mark taken while an outer
// mark is still live.
//
// ⚠ THE ORACLE IS A PLAIN `std::vector` DRIVEN IN LOCKSTEP. Asserting a
// hand-written expected sequence would only re-encode whatever the author
// believed; a shadow vector that receives the same operations answers the
// question the design actually asks.

#include "core/types/speculation_trail.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::vector<int> contentsOf(TrailedStack<int> const& s) {
    return std::vector<int>{s.begin(), s.end()};
}

} // namespace

// ── the exactness claim ─────────────────────────────────────────────────

TEST(SpeculationTrail, PushOnlyExcursionRewindsToTheMark) {
    TrailedStack<int> s;
    for (int i = 0; i < 5; ++i) s.push(i);
    const auto oracle = contentsOf(s);

    const auto m = s.mark();
    for (int i = 100; i < 110; ++i) s.push(i);
    s.rewindTo(m);

    EXPECT_EQ(contentsOf(s), oracle);
    EXPECT_EQ(s.journalSize(), 0u)
        << "a push-only excursion must write no undo records at all — that is "
           "the property that keeps a descent free";
}

TEST(SpeculationTrail, DrainingBelowTheMarkAndRefillingRewindsExactly) {
    TrailedStack<int> s;
    for (int i = 0; i < 6; ++i) s.push(i);
    const auto oracle = contentsOf(s);

    const auto m = s.mark();
    // Drain FOUR levels below the mark, then refill with different values —
    // the interleaving a size-only restore gets wrong: right length, wrong
    // contents.
    for (int i = 0; i < 4; ++i) s.pop();
    for (int i = 900; i < 908; ++i) s.push(i);
    s.rewindTo(m);

    EXPECT_EQ(contentsOf(s), oracle);
}

TEST(SpeculationTrail, RepeatedSupersessionOfOneSlotKeepsTheEARLIESTValue) {
    TrailedStack<int> s;
    s.push(7);
    s.push(8);
    const auto oracle = contentsOf(s);

    const auto m = s.mark();
    // Slot 1 is superseded three times. Only the FIRST record — the value it
    // held AT the mark — may win the replay.
    s.pop(); s.push(80);
    s.pop(); s.push(81);
    s.pop(); s.push(82);
    s.rewindTo(m);

    EXPECT_EQ(contentsOf(s), oracle);
}

TEST(SpeculationTrail, InPlaceWriteThenTruncationPastItRewindsExactly) {
    TrailedStack<int> s;
    for (int i = 0; i < 5; ++i) s.push(i);
    const auto oracle = contentsOf(s);

    const auto m = s.mark();
    s.assign(1, -1);          // in-place write below the mark
    s.assignBack(-2);         // in-place write at the top
    s.truncate(0);            // then drop everything, including the written slots
    for (int i = 500; i < 503; ++i) s.push(i);
    s.rewindTo(m);

    EXPECT_EQ(contentsOf(s), oracle);
}

TEST(SpeculationTrail, NestedMarksRewindIndependently) {
    TrailedStack<int> s;
    for (int i = 0; i < 3; ++i) s.push(i);
    const auto atOuter = contentsOf(s);

    const auto outer = s.mark();
    s.pop();
    s.push(30);
    const auto atInner = contentsOf(s);

    const auto inner = s.mark();
    s.pop(); s.pop(); s.pop();
    s.push(99);
    s.rewindTo(inner);
    EXPECT_EQ(contentsOf(s), atInner) << "inner rewind";

    s.rewindTo(outer);
    EXPECT_EQ(contentsOf(s), atOuter) << "outer rewind after an inner one";
}

// ── a randomized lockstep run against a plain vector ────────────────────

TEST(SpeculationTrail, RandomizedOperationsMatchAShadowVector) {
    // Deterministic pseudo-random (a fixed LCG, no <random> engine
    // portability question): the point is a fixed, reproducible interleaving
    // wide enough to hit combinations nobody enumerated.
    std::uint32_t rng = 0x1234'5678u;
    auto next = [&rng](std::uint32_t n) {
        rng = rng * 1664525u + 1013904223u;
        return (rng >> 16) % n;
    };

    TrailedStack<int> s;
    std::vector<int>  shadow;
    for (int i = 0; i < 40; ++i) { s.push(i); shadow.push_back(i); }

    const auto m       = s.mark();
    const auto oracle  = shadow;

    for (int step = 0; step < 4000; ++step) {
        switch (next(4)) {
            case 0: {
                const int v = 1000 + step;
                s.push(v); shadow.push_back(v);
                break;
            }
            case 1:
                if (!shadow.empty()) { s.pop(); shadow.pop_back(); }
                break;
            case 2:
                if (!shadow.empty()) {
                    const auto i = next(static_cast<std::uint32_t>(shadow.size()));
                    const int  v = 2000 + step;
                    s.assign(i, v);
                    shadow[i] = v;
                }
                break;
            default:
                if (!shadow.empty()) {
                    const auto n = next(static_cast<std::uint32_t>(shadow.size()));
                    s.truncate(n);
                    shadow.resize(n);
                }
                break;
        }
        ASSERT_EQ(contentsOf(s), shadow) << "diverged at step " << step;
    }

    s.rewindTo(m);
    EXPECT_EQ(contentsOf(s), oracle)
        << "4000 mixed mutations did not rewind to the marked state";
}

// ── journal retirement — the thing that keeps memory linear ─────────────

TEST(SpeculationTrail, DiscardJournalStopsRecordingUntilTheNextMark) {
    TrailedStack<int> s;
    for (int i = 0; i < 4; ++i) s.push(i);

    EXPECT_FALSE(s.journalArmed());
    s.pop();
    EXPECT_EQ(s.journalSize(), 0u)
        << "a pop with no live mark must cost nothing";

    (void)s.mark();
    s.pop();
    EXPECT_EQ(s.journalSize(), 1u);

    s.discardJournal();
    EXPECT_EQ(s.journalSize(), 0u);
    EXPECT_FALSE(s.journalArmed());
    s.pop();
    EXPECT_EQ(s.journalSize(), 0u)
        << "after retirement the journal must not resume growing on its own — "
           "this is what keeps a whole-file parse linear rather than "
           "accumulating one record per frame close forever";
}

TEST(SpeculationTrail, RepeatedMarkAndRewindCyclesDoNotAccumulateRecords) {
    TrailedStack<int> s;
    for (int i = 0; i < 16; ++i) s.push(i);

    for (int cycle = 0; cycle < 500; ++cycle) {
        const auto m = s.mark();
        for (int i = 0; i < 8; ++i) s.pop();
        for (int i = 0; i < 8; ++i) s.push(cycle * 100 + i);
        s.rewindTo(m);
        s.discardJournal();
        ASSERT_EQ(s.journalSize(), 0u) << "cycle " << cycle;
    }
    EXPECT_EQ(s.size(), 16u);
}

// ── the set ──────────────────────────────────────────────────────────────

TEST(SpeculationTrail, SetRewindRestoresExactMembership) {
    TrailedSet<std::uint32_t> set;
    set.insert(1);
    set.insert(2);

    const auto m = set.mark();
    set.erase(1);
    set.insert(3);
    set.insert(4);
    set.erase(4);
    set.insert(1);          // re-inserting a key erased after the mark
    set.rewindTo(m);

    EXPECT_TRUE(set.contains(1));
    EXPECT_TRUE(set.contains(2));
    EXPECT_FALSE(set.contains(3));
    EXPECT_FALSE(set.contains(4));
    EXPECT_EQ(set.size(), 2u);
}

TEST(SpeculationTrail, SetRecordsOnlyTransitionsThatChangedIt) {
    TrailedSet<std::uint32_t> set;
    set.insert(1);
    const auto m = set.mark();
    EXPECT_FALSE(set.insert(1)) << "re-inserting a present key is not a change";
    EXPECT_FALSE(set.erase(9))  << "erasing an absent key is not a change";
    EXPECT_EQ(set.journalSize(), 0u)
        << "a no-op must write no undo record — its undo would remove a key "
           "that was already there before the mark";
    set.rewindTo(m);
    EXPECT_TRUE(set.contains(1));
}

// ── the write log ────────────────────────────────────────────────────────

TEST(SpeculationTrail, WriteLogReplaysTheEarliestDisplacedValue) {
    std::vector<int> cells{10, 11, 12};
    TrailedWriteLog<std::uint32_t, int> log;

    const auto m = log.mark();
    log.record(1, cells[1]); cells[1] = 111;
    log.record(1, cells[1]); cells[1] = 222;   // superseded twice
    log.record(2, cells[2]); cells[2] = 333;

    log.rewindTo(m, [&](std::uint32_t i, int v) { cells[i] = v; });
    EXPECT_EQ(cells, (std::vector<int>{10, 11, 12}));
    EXPECT_EQ(log.journalSize(), 0u);
}

TEST(SpeculationTrail, WriteLogRecordsNothingBeforeItsFirstMark) {
    TrailedWriteLog<std::uint32_t, int> log;
    EXPECT_FALSE(log.armed());
    log.record(0, 5);
    EXPECT_EQ(log.journalSize(), 0u);
    (void)log.mark();
    log.record(0, 5);
    EXPECT_EQ(log.journalSize(), 1u);
    log.discardJournal();
    EXPECT_EQ(log.journalSize(), 0u);
    EXPECT_FALSE(log.armed());
}

// ── fail-loud contracts ──────────────────────────────────────────────────

TEST(SpeculationTrailDeath, PopOnEmptyAborts) {
    TrailedStack<int> s;
    EXPECT_DEATH(s.pop(), "pop\\(\\) on an empty TrailedStack");
}

TEST(SpeculationTrailDeath, TruncateThatWouldGrowAborts) {
    TrailedStack<int> s;
    s.push(1);
    EXPECT_DEATH(s.truncate(5), "would grow a TrailedStack");
}

TEST(SpeculationTrailDeath, AssignPastTheTopAborts) {
    TrailedStack<int> s;
    s.push(1);
    EXPECT_DEATH(s.assign(3, 0), "past the top of a TrailedStack");
}

TEST(SpeculationTrailDeath, RewindToAForeignMarkAborts) {
    // A mark this stack never issued: deeper than it has ever been. Silently
    // honouring it would `resize` UP and hand back default-constructed
    // frames — a stack that LOOKS restored and is not.
    TrailedStack<int> shallow;
    shallow.push(1);
    (void)shallow.mark();

    TrailedStack<int> deep;
    for (int i = 0; i < 50; ++i) deep.push(i);
    const auto deepMark = deep.mark();

    EXPECT_DEATH(shallow.rewindTo(deepMark), "cannot have produced");
}
