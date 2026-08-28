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

// ── THE DUPLICATE-MATCH PROMISE ────────────────────────────────────────────
//    D-LK-COFF-COMDAT-SAME-SIZE-EXACT-MATCH-UNCHECKED
//
// A weak definition may declare that its duplicates are required to have the
// same LENGTH (COFF IMAGE_COMDAT_SELECT_SAME_SIZE) or identical BYTES
// (IMAGE_COMDAT_SELECT_EXACT_MATCH). The format specifies a violation as "a
// multiply defined symbol error"; before this the kernel folded all three
// selections lowest-key with no comparison at all.
//
// ★ WHY THESE PINS LIVE HERE AND NOT ONLY IN THE COFF READER'S TESTS: the
// reader's job ends at DECODING the selection byte into a duty. Whether the
// duty is DISCHARGED is a property of the fold, and the fold is shared with the
// whole-program MIR merge -- so it is pinned against the shared kernel, in the
// same file as the strong-shadows-weak and lowest-key policies it sits beside.
namespace {

// A weak definition carrying a duty and a body. The bytes are OWNED by the
// caller; every test below keeps its bodies alive for the whole call, which is
// the contract `CrossCuDef::body` documents.
CrossCuDef weakDef(std::uint32_t cuId, std::uint32_t sym, std::string name,
                   dss::DuplicateMatch duty,
                   std::span<std::uint8_t const> bodyBytes) {
    CrossCuDef d = def(cuId, sym, std::move(name), SymbolBinding::Weak);
    d.duplicateMatch = duty;
    d.bodySize       = bodyBytes.size();
    d.body           = bodyBytes;
    return d;
}

// A ZERO-FILL definition: it OCCUPIES `size` bytes and STORES none, which is
// exactly what a `.bss` COMDAT is. The whole point of `bodySize` being separate
// from `body.size()` -- a comparison keyed on the span would see 0 == 0 here
// however different the two extents are.
CrossCuDef weakZeroFillDef(std::uint32_t cuId, std::uint32_t sym,
                           std::string name, dss::DuplicateMatch duty,
                           std::size_t size) {
    CrossCuDef d = def(cuId, sym, std::move(name), SymbolBinding::Weak);
    d.duplicateMatch = duty;
    d.bodySize       = size;
    d.body           = {};
    return d;
}

[[nodiscard]] std::vector<std::uint8_t> bodyOf(std::string_view s) {
    return {s.begin(), s.end()};
}

} // namespace

// SAME_SIZE, satisfied: two weak definitions of equal LENGTH but DIFFERENT
// content fold silently. This is the arm that must NOT become stricter -- the
// selection promises the size, and nothing about the bytes.
TEST(CrossCuResolve, SameSizeDuplicatesOfEqualLengthFoldEvenWhenBytesDiffer) {
    auto const a = bodyOf("AAAA");
    auto const b = bodyOf("BBBB");
    std::vector<CrossCuDef> defs{
        weakDef(1, 1, "g", dss::DuplicateMatch::SameSize, a),
        weakDef(2, 2, "g", dss::DuplicateMatch::SameSize, b),
    };
    auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
    EXPECT_TRUE(r.duplicateMismatches.empty())
        << "SAME_SIZE promises the LENGTH only; equal-length copies with "
           "different bytes satisfy it and must fold";
    ASSERT_EQ(r.winners.count("g"), 1u);
    EXPECT_EQ(r.winners.at("g").cuId.v, 1u);
}

// SAME_SIZE, broken. The mismatch is RECORDED as data with both keys, the
// governing duty and both sizes -- the caller turns it into the diagnostic.
TEST(CrossCuResolve, SameSizeDuplicatesOfDifferentLengthAreRecorded) {
    auto const a = bodyOf("AAAA");
    auto const b = bodyOf("BB");
    std::vector<CrossCuDef> defs{
        weakDef(1, 1, "g", dss::DuplicateMatch::SameSize, a),
        weakDef(2, 2, "g", dss::DuplicateMatch::SameSize, b),
    };
    auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
    ASSERT_EQ(r.duplicateMismatches.size(), 1u)
        << "a SAME_SIZE promise broken by a 4-byte vs 2-byte pair must be "
           "recorded exactly once";
    auto const& m = r.duplicateMismatches.front();
    EXPECT_EQ(m.name, "g");
    EXPECT_EQ(m.required, dss::DuplicateMatch::SameSize);
    EXPECT_EQ(m.existingSize, 4u);
    EXPECT_EQ(m.incomingSize, 2u);
    EXPECT_EQ(m.existing.cuId.v, 1u);
    EXPECT_EQ(m.incoming.cuId.v, 2u);
    // The resolution still names a winner: recording is not refusing. The
    // CALLER decides that an error stops the link, exactly as it does for a
    // two-strong conflict.
    EXPECT_EQ(r.winners.count("g"), 1u);
}

// EXACT_MATCH is strictly stronger than SAME_SIZE: equal length is no longer
// enough. This is the pair the first test deliberately lets through, so the two
// together pin that the scale really has two distinct steps.
TEST(CrossCuResolve, ExactMatchRejectsEqualLengthDifferentBytes) {
    auto const a = bodyOf("AAAA");
    auto const b = bodyOf("BBBB");
    std::vector<CrossCuDef> defs{
        weakDef(1, 1, "g", dss::DuplicateMatch::ExactContent, a),
        weakDef(2, 2, "g", dss::DuplicateMatch::ExactContent, b),
    };
    auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
    ASSERT_EQ(r.duplicateMismatches.size(), 1u);
    EXPECT_EQ(r.duplicateMismatches.front().required,
              dss::DuplicateMatch::ExactContent);
    EXPECT_EQ(r.duplicateMismatches.front().existingSize, 4u);
    EXPECT_EQ(r.duplicateMismatches.front().incomingSize, 4u)
        << "equal sizes -- this is the case ONLY a content compare can catch";
}

TEST(CrossCuResolve, ExactMatchAcceptsIdenticalBytes) {
    auto const a = bodyOf("hello");
    auto const b = bodyOf("hello");
    std::vector<CrossCuDef> defs{
        weakDef(1, 1, "g", dss::DuplicateMatch::ExactContent, a),
        weakDef(2, 2, "g", dss::DuplicateMatch::ExactContent, b),
    };
    auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
    EXPECT_TRUE(r.duplicateMismatches.empty());
    ASSERT_EQ(r.winners.count("g"), 1u);
    EXPECT_EQ(r.winners.at("g").cuId.v, 1u) << "lowest key still wins";
}

// ANY promises nothing, so nothing is checked -- and this is the arm that keeps
// DSS's OWN weak definitions (emitted as IMAGE_COMDAT_SELECT_ANY) linking.
// Sizes AND bytes differ here and the fold must be silent.
TEST(CrossCuResolve, AnyDuplicatesAreNeverCompared) {
    auto const a = bodyOf("AAAAAAAA");
    auto const b = bodyOf("B");
    std::vector<CrossCuDef> defs{
        weakDef(1, 1, "g", dss::DuplicateMatch::Any, a),
        weakDef(2, 2, "g", dss::DuplicateMatch::Any, b),
    };
    auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
    EXPECT_TRUE(r.duplicateMismatches.empty())
        << "IMAGE_COMDAT_SELECT_ANY may differ freely; comparing it would "
           "refuse DSS's own weak-definition encoding";
}

// The GOVERNING duty is the stricter of the pair's two. A definition that
// promises nothing must not dilute a sibling that promised EXACT_MATCH -- and
// the recorded `required` must name the promise that was actually broken,
// whichever member declared it. Both orders are exercised because "whichever
// member" is precisely the thing an implementation gets half-right.
TEST(CrossCuResolve, TheStricterOfTheTwoDutiesGovernsInEitherOrder) {
    auto const a = bodyOf("AAAA");
    auto const b = bodyOf("BBBB");
    {
        std::vector<CrossCuDef> defs{
            weakDef(1, 1, "g", dss::DuplicateMatch::Any, a),
            weakDef(2, 2, "g", dss::DuplicateMatch::ExactContent, b),
        };
        auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
        ASSERT_EQ(r.duplicateMismatches.size(), 1u)
            << "the ANY member must not dilute the EXACT_MATCH member";
        EXPECT_EQ(r.duplicateMismatches.front().required,
                  dss::DuplicateMatch::ExactContent);
    }
    {
        std::vector<CrossCuDef> defs{
            weakDef(1, 1, "g", dss::DuplicateMatch::ExactContent, a),
            weakDef(2, 2, "g", dss::DuplicateMatch::Any, b),
        };
        auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
        ASSERT_EQ(r.duplicateMismatches.size(), 1u)
            << "the duty must be read off BOTH members, not off the incoming "
               "one alone";
        EXPECT_EQ(r.duplicateMismatches.front().required,
                  dss::DuplicateMatch::ExactContent);
    }
}

// A STRONG definition overriding a weak one is not a duplicate COPY of it, so
// the format asks nothing of the pair. Checking here would refuse the ordinary
// "a real definition overrides a selectany/inline one" shape.
TEST(CrossCuResolve, AStrongDefinitionIsNotCheckedAgainstAWeakPromise) {
    auto const a = bodyOf("AAAA");
    std::vector<CrossCuDef> defs{
        weakDef(1, 1, "g", dss::DuplicateMatch::ExactContent, a),
        def(2, 2, "g", SymbolBinding::Global),
    };
    auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
    EXPECT_TRUE(r.duplicateMismatches.empty())
        << "a strong definition OVERRIDES the weak copies; it is not one of "
           "them and the selection promise does not reach it";
    ASSERT_EQ(r.winners.count("g"), 1u);
    EXPECT_EQ(r.winners.at("g").cuId.v, 2u);
}

// Order-independence, the same property the conflict pins hold: a broken
// promise is found whichever order the three definitions arrive in. Two agree
// and one differs, so no single "compare against the first" rule suffices.
TEST(CrossCuResolve, ABrokenPromiseIsFoundInEveryPermutation) {
    auto const four = bodyOf("AAAA");
    auto const two  = bodyOf("BB");
    std::vector<std::vector<CrossCuDef>> orders{
        {weakDef(1, 1, "g", dss::DuplicateMatch::SameSize, four),
         weakDef(2, 2, "g", dss::DuplicateMatch::SameSize, four),
         weakDef(3, 3, "g", dss::DuplicateMatch::SameSize, two)},
        {weakDef(3, 3, "g", dss::DuplicateMatch::SameSize, two),
         weakDef(1, 1, "g", dss::DuplicateMatch::SameSize, four),
         weakDef(2, 2, "g", dss::DuplicateMatch::SameSize, four)},
        {weakDef(2, 2, "g", dss::DuplicateMatch::SameSize, four),
         weakDef(3, 3, "g", dss::DuplicateMatch::SameSize, two),
         weakDef(1, 1, "g", dss::DuplicateMatch::SameSize, four)},
    };
    for (std::size_t i = 0; i < orders.size(); ++i) {
        auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{orders[i]});
        EXPECT_FALSE(r.duplicateMismatches.empty())
            << "permutation #" << i
            << " let a broken SAME_SIZE promise through -- the check must not "
               "depend on which definition happened to arrive first";
    }
}

// ── ZERO-FILL DEFINITIONS: THE SHAPE THAT STORES NO BYTES ──────────────────
//
// A `.bss` COMDAT occupies a declared extent and stores nothing. Every one of
// the pins above would still pass if the comparison were keyed on the STORAGE
// rather than the SIZE, because both storages are empty -- so this is the case
// that decides whether `bodySize` is real or decorative.
TEST(CrossCuResolve, ZeroFillDuplicatesOfDifferentExtentBreakSameSize) {
    std::vector<CrossCuDef> defs{
        weakZeroFillDef(1, 1, "z", dss::DuplicateMatch::SameSize, 8),
        weakZeroFillDef(2, 2, "z", dss::DuplicateMatch::SameSize, 4),
    };
    auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
    ASSERT_EQ(r.duplicateMismatches.size(), 1u)
        << "two zero-fill definitions of 8 and 4 bytes store NOTHING each; a "
           "check keyed on the byte span would compare 0 with 0 and call them "
           "a match";
    EXPECT_EQ(r.duplicateMismatches.front().existingSize, 8u);
    EXPECT_EQ(r.duplicateMismatches.front().incomingSize, 4u);
}

TEST(CrossCuResolve, ZeroFillDuplicatesOfEqualExtentSatisfyBothDuties) {
    {
        std::vector<CrossCuDef> defs{
            weakZeroFillDef(1, 1, "z", dss::DuplicateMatch::SameSize, 8),
            weakZeroFillDef(2, 2, "z", dss::DuplicateMatch::SameSize, 8),
        };
        auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
        EXPECT_TRUE(r.duplicateMismatches.empty());
    }
    {
        // EXACT_MATCH too: a zero-fill definition's content IS all-zero, so two
        // of equal extent are byte-identical and must fold.
        std::vector<CrossCuDef> defs{
            weakZeroFillDef(1, 1, "z", dss::DuplicateMatch::ExactContent, 8),
            weakZeroFillDef(2, 2, "z", dss::DuplicateMatch::ExactContent, 8),
        };
        auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
        EXPECT_TRUE(r.duplicateMismatches.empty())
            << "two zero-fill definitions of the same extent are the same "
               "bytes -- all of them zero";
    }
}

// A zero-fill definition folded against a FILE-BACKED one of the same extent.
// EXACT_MATCH must read the zero-fill side as implicit zeros and compare, not
// give up: a run of zeros MATCHES, anything else does NOT.
TEST(CrossCuResolve, ZeroFillAgainstFileBackedComparesAsImplicitZeros) {
    auto const zeros    = bodyOf(std::string_view{"\0\0\0\0", 4});
    auto const nonZeros = bodyOf("AAAA");
    {
        std::vector<CrossCuDef> defs{
            weakZeroFillDef(1, 1, "z", dss::DuplicateMatch::ExactContent, 4),
            weakDef(2, 2, "z", dss::DuplicateMatch::ExactContent, zeros),
        };
        auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
        EXPECT_TRUE(r.duplicateMismatches.empty())
            << "a 4-byte zero-fill definition and four stored zero bytes are "
               "the same content";
    }
    {
        std::vector<CrossCuDef> defs{
            weakZeroFillDef(1, 1, "z", dss::DuplicateMatch::ExactContent, 4),
            weakDef(2, 2, "z", dss::DuplicateMatch::ExactContent, nonZeros),
        };
        auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
        EXPECT_EQ(r.duplicateMismatches.size(), 1u)
            << "a zero-fill definition is NOT equal to four 'A' bytes; reading "
               "the empty span as 'nothing to compare' would have folded them";
    }
}
