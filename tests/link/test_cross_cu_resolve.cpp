// Cross-CU DEFINITION resolver — DIRECT unit tests for the pure, tier-neutral
// `resolveCrossCuDefs` kernel (Cycle 24 extraction from linker.cpp).
//
// This is the TRIPWIRE. `resolveCrossCuDefs` is the single source of truth for the
// cross-CU symbol policy (strong-shadows-weak / two-strong is ambiguous / all-weak
// lowest-key wins, order-independent) that the linker AND a future whole-program MIR
// merge (Cycle 25) must agree on. Pinning the policy here — by calling the function
// with hand-built `(name, binding, key)` triples, NOT by threading a whole link — means
// a c25 change that makes one consumer diverge from the other fails THIS test first.
//
// The kernel takes NO reporter / AssembledModule / target / format / language — so
// these tests need none either: pure value-in, value-out.

#include "core/types/symbol_attrs.hpp"  // SymbolBinding
#include "link/cross_cu_resolve.hpp"
#include "link/symbol_kind.hpp"         // LinkedSymbolKey

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::linker::CrossCuConflict;
using dss::linker::CrossCuDef;
using dss::linker::resolveCrossCuDefs;

namespace {

// Compact builders mirroring the link-tier test helpers' shape.
CrossCuDef def(std::uint32_t cuId, std::uint32_t sym, std::string name, SymbolBinding b) {
    return CrossCuDef{std::move(name), b, LinkedSymbolKey{CompilationUnitId{cuId},
                                                          SymbolId{sym}}};
}

[[nodiscard]] std::size_t
countConflictName(std::vector<dss::linker::CrossCuConflict> const& conflicts,
                  std::string_view target) {
    return static_cast<std::size_t>(std::count_if(
        conflicts.begin(), conflicts.end(),
        [&](dss::linker::CrossCuConflict const& c) { return c.name == target; }));
}

// A strong (Global) definition shadows a weak one of the same name — the winning key is
// the STRONG def's, regardless of declaration order. No ambiguity.
TEST(CrossCuResolve, StrongShadowsWeak) {
    std::vector<CrossCuDef> defs{
        def(1, 1, "f", SymbolBinding::Weak),
        def(2, 2, "f", SymbolBinding::Global),
    };
    auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
    ASSERT_EQ(r.winners.count("f"), 1u);
    EXPECT_EQ(r.winners.at("f").cuId.v, 2u)
        << "the strong (Global) def must shadow the weak one";
    EXPECT_EQ(r.winners.at("f").symbol.v, 2u);
    EXPECT_TRUE(r.conflicts.empty())
        << "strong-over-weak is NOT a redefinition";
}

// Among all-weak definitions the lexicographically-lowest (cuId, SymbolId) wins —
// INDEPENDENT of the order the definitions are presented. Resolve BOTH orderings and
// assert the SAME winner across both: the order-independence pin.
TEST(CrossCuResolve, AllWeakLowestKeyWinsOrderIndependent) {
    std::vector<CrossCuDef> forward{
        def(2, 2, "f", SymbolBinding::Weak),
        def(1, 1, "f", SymbolBinding::Weak),
    };
    std::vector<CrossCuDef> reversed{
        def(1, 1, "f", SymbolBinding::Weak),
        def(2, 2, "f", SymbolBinding::Weak),
    };
    auto const rf = resolveCrossCuDefs(std::span<CrossCuDef const>{forward});
    auto const rr = resolveCrossCuDefs(std::span<CrossCuDef const>{reversed});
    ASSERT_EQ(rf.winners.count("f"), 1u);
    ASSERT_EQ(rr.winners.count("f"), 1u);
    // Lexicographically-lowest key (cu1, s1) wins in BOTH orderings.
    EXPECT_EQ(rf.winners.at("f").cuId.v, 1u);
    EXPECT_EQ(rf.winners.at("f").symbol.v, 1u);
    EXPECT_EQ(rr.winners.at("f").cuId.v, rf.winners.at("f").cuId.v)
        << "all-weak resolution must be order-independent";
    EXPECT_EQ(rr.winners.at("f").symbol.v, rf.winners.at("f").symbol.v)
        << "all-weak resolution must be order-independent";
    EXPECT_TRUE(rf.conflicts.empty());
    EXPECT_TRUE(rr.conflicts.empty());
}

// Two strong (Global) definitions of one name is an ambiguous redefinition — recorded
// as data in `conflicts` (exactly once for the single collision event), NOT emitted as a
// diagnostic (the kernel is reporter-free; the caller emits). The recorded conflict
// carries the colliding key PAIR — `existing` (the winner-so-far when the duplicate was
// seen) and `incoming` (the duplicate) — which is exactly what the linker names in its
// both-CUs diagnostic AND what the Cycle-25 MIR merge folds onto.
TEST(CrossCuResolve, TwoStrongIsAmbiguous) {
    std::vector<CrossCuDef> defs{
        def(1, 1, "f", SymbolBinding::Global),  // seen first → the existing winner-so-far
        def(2, 2, "f", SymbolBinding::Global),  // seen second → the incoming duplicate
    };
    auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
    ASSERT_EQ(r.conflicts.size(), 1u)
        << "two strong defs of one name → exactly one ambiguity event "
           "(K strongs → K-1 events, matching the former per-pair diagnostic count)";
    EXPECT_EQ(countConflictName(r.conflicts, "f"), 1u);
    // The conflict names BOTH colliding definitions — the exact key pair the linker
    // turns into "CU #existing and CU #incoming". `existing` is the first-seen def's key
    // (cu1,s1); `incoming` is the duplicate's (cu2,s2).
    CrossCuConflict const& c = r.conflicts.front();
    EXPECT_EQ(c.name, "f");
    EXPECT_EQ(c.existing.cuId.v, 1u);
    EXPECT_EQ(c.existing.symbol.v, 1u);
    EXPECT_EQ(c.incoming.cuId.v, 2u);
    EXPECT_EQ(c.incoming.symbol.v, 2u);
    // The name still resolves to a deterministic winner (lowest key) so a downstream
    // merge has SOME definition to fold onto even on the error path.
    ASSERT_EQ(r.winners.count("f"), 1u);
    EXPECT_EQ(r.winners.at("f").cuId.v, 1u);
}

// A Local-binding definition is module-private and NEVER enters the winner table — two
// Locals of the same name in different CUs do not collide, do not resolve to a shared
// winner, and produce no ambiguity. A Local also never shadows / satisfies a Global.
TEST(CrossCuResolve, LocalExcluded) {
    std::vector<CrossCuDef> defs{
        def(1, 1, "f", SymbolBinding::Local),
        def(2, 2, "f", SymbolBinding::Local),
    };
    auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
    EXPECT_EQ(r.winners.count("f"), 0u)
        << "a Local-binding symbol must never enter the cross-CU winner table";
    EXPECT_TRUE(r.conflicts.empty());

    // A Local "f" alongside a Global "f": only the Global resolves; no false ambiguity
    // (the Local is invisible to cross-CU resolution).
    std::vector<CrossCuDef> mixed{
        def(1, 1, "f", SymbolBinding::Local),
        def(2, 2, "f", SymbolBinding::Global),
    };
    auto const rm = resolveCrossCuDefs(std::span<CrossCuDef const>{mixed});
    ASSERT_EQ(rm.winners.count("f"), 1u);
    EXPECT_EQ(rm.winners.at("f").cuId.v, 2u)
        << "the Global def alone resolves; the Local is module-private";
    EXPECT_TRUE(rm.conflicts.empty());
}

// THREE strong (Global) definitions of one name yield K-1 = 2 ambiguity EVENTS — the
// load-bearing count the header makes a contract ("K strongs → K-1 entries, matching the
// former per-pair diagnostic count"). Pins the conflict-count arithmetic AND the event
// ORDERING: the winner-so-far stays the lowest key (cu1,s1) throughout (no later strong
// has a lower key), so BOTH events carry `existing == (cu1,s1)`, with `incoming` running
// (cu2,s2) then (cu3,s3) in feed order. A regression that pushed the conflict only once
// (e.g. an "already-recorded" guard) would make `conflicts.size()` 1 ≠ 2 here.
TEST(CrossCuResolve, ThreeStrongYieldsTwoConflicts) {
    std::vector<CrossCuDef> defs{
        def(1, 1, "f", SymbolBinding::Global),  // first → the winner-so-far throughout
        def(2, 2, "f", SymbolBinding::Global),  // second → incoming of event[0]
        def(3, 3, "f", SymbolBinding::Global),  // third  → incoming of event[1]
    };
    auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
    // K = 3 strongs → K-1 = 2 collision events (the header's load-bearing arithmetic).
    ASSERT_EQ(r.conflicts.size(), 2u)
        << "three strong defs of one name → exactly two ambiguity events (K-1)";
    EXPECT_EQ(countConflictName(r.conflicts, "f"), 2u);
    // event[0]: existing is the first-seen winner (cu1,s1); incoming is the 2nd (cu2,s2).
    EXPECT_EQ(r.conflicts[0].name, "f");
    EXPECT_EQ(r.conflicts[0].existing.cuId.v, 1u);
    EXPECT_EQ(r.conflicts[0].existing.symbol.v, 1u);
    EXPECT_EQ(r.conflicts[0].incoming.cuId.v, 2u);
    EXPECT_EQ(r.conflicts[0].incoming.symbol.v, 2u);
    // event[1]: the winner-so-far is STILL (cu1,s1) (it never lost — lowest key); the
    // incoming is the 3rd def (cu3,s3). This pins that `existing` tracks the running
    // winner, not merely the previous def.
    EXPECT_EQ(r.conflicts[1].name, "f");
    EXPECT_EQ(r.conflicts[1].existing.cuId.v, 1u);
    EXPECT_EQ(r.conflicts[1].existing.symbol.v, 1u);
    EXPECT_EQ(r.conflicts[1].incoming.cuId.v, 3u);
    EXPECT_EQ(r.conflicts[1].incoming.symbol.v, 3u);
    // Deterministic lowest-key winner survives all the collisions.
    ASSERT_EQ(r.winners.count("f"), 1u);
    EXPECT_EQ(r.winners.at("f").cuId.v, 1u);
    EXPECT_EQ(r.winners.at("f").symbol.v, 1u);
}

// A STRONG def seen FIRST, then a WEAK def of the same name — the opposite feed order
// from `StrongShadowsWeak` (which is weak-first). This is the ONLY ordering that reaches
// the kernel's terminal `else: existing strong shadows the new weak` arm (newStrong=false
// while curStrong=true), which the weak-first test cannot exercise. The existing strong
// stays the winner and a strong-over-weak shadow is NOT a conflict.
TEST(CrossCuResolve, StrongThenWeakShadowsNoConflict) {
    std::vector<CrossCuDef> defs{
        def(1, 1, "f", SymbolBinding::Global),  // strong FIRST → the winner
        def(2, 2, "f", SymbolBinding::Weak),    // weak second → shadowed by the strong
    };
    auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
    ASSERT_EQ(r.winners.count("f"), 1u);
    EXPECT_EQ(r.winners.at("f").cuId.v, 1u)
        << "the existing strong def shadows the later weak def";
    EXPECT_EQ(r.winners.at("f").symbol.v, 1u);
    EXPECT_TRUE(r.conflicts.empty())
        << "a strong shadowing a later weak is NOT a redefinition conflict";
}

} // namespace
