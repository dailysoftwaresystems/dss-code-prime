// `MirIdRemap` — the per-function OLD→NEW id translation that replaced the
// rebuild substrate's `std::unordered_map<std::uint32_t, IdT>`
// (D-PERF-OPT-REBUILD-REMAP-IS-A-HASH-MAP).
//
// The pass tests exercise this transitively through every rebuild, but a
// transitive failure blames whichever pass happened to run first. These records
// pin the four properties the substitution actually turns on, and each one is a
// property a hash map had for free — which is exactly why they need pinning now
// that a hand-written container provides them:
//
//   1. dense round trip            — a value stored is the value read back
//   2. ABSENT is not zero-valued   — an unwritten in-range slot reads absent,
//                                    NOT as a default-constructed id
//   3. reset CLEARS                — the classic "reused the buffer" bug; a
//                                    stale entry from the previous function
//                                    would silently rewrite an operand to
//                                    another function's instruction
//   4. query vs WRITE asymmetry    — an out-of-range QUERY answers absent (the
//                                    hash map's answer, which Dce and Licm both
//                                    depend on), an out-of-range WRITE aborts

#include "core/types/strong_ids.hpp"
#include "opt/passes/mir_id_remap.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace dss;
using namespace dss::opt::passes;

namespace {

// Arena ids carry a tag; the remap stores ids verbatim and never inspects it, so
// one non-zero tag throughout is enough to prove the value survives the trip.
constexpr std::uint32_t kTag = 7;

MirInstId inst(std::uint32_t v) { return MirInstId{v, kTag}; }

} // namespace

TEST(MirIdRemap, DenseRangeRoundTrips) {
    MirInstRemap m;
    m.reset(100, 8, "rewrite");
    for (std::uint32_t i = 0; i < 8; ++i) m.put(100 + i, inst(500 + i));
    for (std::uint32_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(m.contains(100 + i)) << "slot " << (100 + i);
        EXPECT_EQ(m.at(100 + i).v, 500 + i);
        ASSERT_NE(m.find(100 + i), nullptr);
        EXPECT_EQ(m.find(100 + i)->v, 500 + i);
    }
    EXPECT_EQ(m.size(), 8u);
}

// The base offset is the whole point — slot `base` must land at index 0 and a
// slot below `base` must not alias the top of the range through unsigned wrap.
TEST(MirIdRemap, BaseOffsetIsApplied) {
    MirInstRemap m;
    m.reset(1000, 4, "rewrite");
    m.put(1000, inst(1));
    m.put(1003, inst(4));
    EXPECT_EQ(m.at(1000).v, 1u);
    EXPECT_EQ(m.at(1003).v, 4u);
    EXPECT_FALSE(m.contains(999));
    EXPECT_FALSE(m.contains(1004));
    EXPECT_FALSE(m.contains(0));
    EXPECT_EQ(m.size(), 2u);
}

// An in-range slot nobody wrote is ABSENT, not a default-constructed id. A
// rebuild that read a defaulted `MirInstId{}` as a real translation would emit
// an operand pointing at the arena's slot-0 sentinel.
TEST(MirIdRemap, UnwrittenInRangeSlotIsAbsent) {
    MirInstRemap m;
    m.reset(10, 5, "rewrite");
    m.put(12, inst(99));
    EXPECT_TRUE(m.contains(12));
    for (std::uint32_t v : {10u, 11u, 13u, 14u}) {
        EXPECT_FALSE(m.contains(v)) << "slot " << v;
        EXPECT_EQ(m.find(v), nullptr) << "slot " << v;
    }
    EXPECT_EQ(m.size(), 1u);
}

// ★ THE ONE THAT MATTERS MOST. The rebuild calls `reset` once per function and
// the storage is reused across all of them. A `reset` that resized without
// clearing would leave the previous function's translations visible at the same
// indices — a silent cross-function operand rewrite, which is a miscompile with
// no diagnostic anywhere.
TEST(MirIdRemap, ResetClearsEveryPreviousEntry) {
    MirInstRemap m;
    m.reset(0, 4, "rewrite");
    for (std::uint32_t i = 0; i < 4; ++i) m.put(i, inst(700 + i));
    EXPECT_EQ(m.size(), 4u);

    // A second function at a DIFFERENT base, same extent — the indices overlap
    // exactly, so a non-clearing reset would hand back the first function's ids.
    m.reset(80, 4, "rewrite");
    EXPECT_EQ(m.size(), 0u);
    for (std::uint32_t i = 0; i < 4; ++i) {
        EXPECT_FALSE(m.contains(80 + i)) << "slot " << (80 + i);
    }
    m.put(81, inst(4242));
    EXPECT_EQ(m.at(81).v, 4242u);
    EXPECT_EQ(m.size(), 1u);
}

// A zero-extent reset is the blockless-function case; every query must answer
// absent rather than index an empty buffer.
TEST(MirIdRemap, EmptyRangeAnswersAbsent) {
    MirInstRemap m;
    m.reset(0, 0, "rewrite");
    EXPECT_FALSE(m.contains(0));
    EXPECT_FALSE(m.contains(1));
    EXPECT_EQ(m.find(0), nullptr);
    EXPECT_EQ(m.size(), 0u);
}

// Re-`put` on a live slot overwrites without double-counting — `size()` feeds
// pass counters that would drift if it did.
TEST(MirIdRemap, OverwriteDoesNotDoubleCount) {
    MirInstRemap m;
    m.reset(5, 3, "rewrite");
    m.put(6, inst(1));
    m.put(6, inst(2));
    EXPECT_EQ(m.at(6).v, 2u);
    EXPECT_EQ(m.size(), 1u);
}

// Blocks get their own instantiation; prove the template is not instruction-only.
TEST(MirIdRemap, BlockRemapRoundTrips) {
    MirBlockRemap m;
    m.reset(3, 3, "blockMap");
    m.put(4, MirBlockId{44, kTag});
    EXPECT_TRUE(m.contains(4));
    EXPECT_EQ(m.at(4).v, 44u);
    EXPECT_FALSE(m.contains(2));
    EXPECT_FALSE(m.contains(6));
}

// ── The asymmetry, exercised rather than read ────────────────────────
// A QUERY outside the range is legitimate (the hash map answered "no", and Dce
// asks it about phi predecessors while Licm asks it about foreign self-looping
// blocks under D-OPT-LICM-NATURAL-LOOPS-MODULE-WIDE-SCAN). The four EXPECT_FALSE
// records above already pin that. A WRITE outside the range is a
// substrate-contract violation and must abort — the hash map could not tell the
// two apart, and silently growing would mean the rebuild believes a foreign id
// belongs to the function it is emitting.
TEST(MirIdRemapDeathTest, WriteAboveRangeAborts) {
    MirInstRemap m;
    m.reset(10, 4, "rewrite");
    EXPECT_DEATH({ m.put(14, inst(1)); }, "outside this function's range");
}

TEST(MirIdRemapDeathTest, WriteBelowRangeAborts) {
    MirInstRemap m;
    m.reset(10, 4, "rewrite");
    EXPECT_DEATH({ m.put(9, inst(1)); }, "outside this function's range");
}

// The checked read of an ABSENT slot is the hash map's throwing `.at()`, and it
// must name the map rather than unwinding anonymously
// (D-OPT2-REWRITE-MAP-COMPLETENESS).
TEST(MirIdRemapDeathTest, CheckedReadOfAbsentSlotAborts) {
    MirInstRemap m;
    m.reset(10, 4, "rewrite");
    EXPECT_DEATH({ (void)m.at(11); }, "no translation");
}
