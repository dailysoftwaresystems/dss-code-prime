#include "core/substrate/mint_monotonic_id.hpp"

#include "core/types/strong_ids.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <unordered_set>
#include <vector>

// Direct unit tests for `substrate::mintMonotonicId<StrongId>()`. The
// substrate is exercised indirectly by 8 production callers (the family
// of strong-id minters), but the central claims — per-type independent
// counter, monotonic non-zero output, contract-rejection of arena ids —
// are pinned here so a regression to the template surfaces locally rather
// than via a far-away symptom.

namespace {

using dss::substrate::mintMonotonicId;

}  // namespace

TEST(MintMonotonicId, EmitsMonotonicNonZeroIdsPerType) {
    // Two consecutive mints of the same StrongId type must be distinct
    // and non-zero (`v == 0` is the Invalid sentinel for every
    // DSS_STRONG_ID-shape type).
    auto const a = mintMonotonicId<dss::HirModuleId>();
    auto const b = mintMonotonicId<dss::HirModuleId>();
    EXPECT_TRUE(a.valid());
    EXPECT_TRUE(b.valid());
    EXPECT_NE(a, b);
    EXPECT_LT(a.v, b.v);  // monotonic
}

TEST(MintMonotonicId, IndependentCountersPerStrongIdType) {
    // The static counter is per template instantiation — two different
    // StrongId types share no state. Mint one of each in interleaved
    // order; the sequences must be independent.
    //
    // Use distinct types that aren't otherwise minted in this test
    // binary to keep the assertions deterministic.
    auto const a1 = mintMonotonicId<dss::MirModuleId>();
    auto const b1 = mintMonotonicId<dss::LirModuleId>();
    auto const a2 = mintMonotonicId<dss::MirModuleId>();
    auto const b2 = mintMonotonicId<dss::LirModuleId>();
    // Each type's two mints must be consecutive in its own counter.
    EXPECT_EQ(a2.v, a1.v + 1);
    EXPECT_EQ(b2.v, b1.v + 1);
}

TEST(MintMonotonicId, ConcurrentMintsAllDistinct) {
    // The substrate claims thread-safety via relaxed-atomic increment.
    // 4 threads × 256 mints = 1024 distinct ids; if the increment ever
    // collided, the set size would be < 1024.
    constexpr int kThreads = 4;
    constexpr int kPerThread = 256;
    std::vector<std::vector<dss::TreeId>> per(kThreads);
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            per[t].reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i) {
                per[t].push_back(mintMonotonicId<dss::TreeId>());
            }
        });
    }
    for (auto& th : ts) th.join();

    std::unordered_set<std::uint32_t> seen;
    seen.reserve(kThreads * kPerThread);
    for (auto const& v : per) {
        for (auto const& id : v) {
            EXPECT_TRUE(id.valid());
            EXPECT_TRUE(seen.insert(id.v).second) << "duplicate id " << id.v;
        }
    }
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(kThreads * kPerThread));
}

// The DssStrongId concept rejects arena-element ids at compile time —
// this is enforced by a static_assert when the template is instantiated.
// We cannot easily express a "this should fail to compile" runtime test,
// but the negative path is exercised by the concept constraint and would
// surface as a build failure on the offending caller.
static_assert(dss::substrate::DssStrongId<dss::HirModuleId>);
static_assert(dss::substrate::DssStrongId<dss::MirModuleId>);
static_assert(dss::substrate::DssStrongId<dss::LirModuleId>);
static_assert(dss::substrate::DssStrongId<dss::TargetSchemaId>);
static_assert(dss::substrate::DssStrongId<dss::SchemaId>);
static_assert(dss::substrate::DssStrongId<dss::BufferId>);
static_assert(dss::substrate::DssStrongId<dss::TreeId>);
static_assert(dss::substrate::DssStrongId<dss::CompilationUnitId>);
// Arena ids have sizeof == 8 (uint32 v + uint32 arenaTag) — concept
// fails on the `sizeof(T) == sizeof(uint32_t)` requirement.
static_assert(!dss::substrate::DssStrongId<dss::HirNodeId>);
static_assert(!dss::substrate::DssStrongId<dss::MirInstId>);
static_assert(!dss::substrate::DssStrongId<dss::LirInstId>);
