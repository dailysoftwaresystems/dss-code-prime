// D-LIR-VREG-ID-BITFIELD-TRUNCATES-SILENTLY-PAST-ITS-WIDTH — the REPRESENTATION pin.
//
// ★★★ WHAT WENT WRONG, AND IT WAS THREE LITERALS THAT DID NOT ADD UP TO 32.
// `LirReg` is a 4-byte POD whose three bit-fields were written as literals:
// `id : 24`, `classKind : 6`, `isPhysical : 1`. That is 31 of the 32 bits in the
// allocation unit — one bit unspent — and a SIX-bit field naming FIVE register
// classes. Handing `makeVirtualReg` an id of 2²⁴ or more therefore truncated it
// MODULO 2²⁴, silently: the argument and the field are both `std::uint32_t`, so
// no conversion warning exists to catch it and no runtime check looked.
//
// ★★★ WHY THAT IS A CORRECTNESS DEFECT AND NOT A CAPACITY ONE. Liveness keys
// live ranges BY VREG ID. Two distinct virtual registers that truncate to the
// same id become ONE range spanning both, so the allocator may hand two
// simultaneously-live values one register — and `findAllocationConflict`, the
// auditor this pipeline installs against exactly that outcome, CANNOT SEE IT:
// it re-derives interference from the same liveness table and inherits the same
// collision. That is a silent miscompile with no instrument pointed at it.
//
// ✔MEASURED 2026-09-17, one AArch64 function of N repeated `volatile struct
// wide` aggregate copies, Release, Windows. The allocator sees `130·N + 4`
// distinct vreg ids — exact at N = 64 000 / 100 000 / 128 000 — so the id space
// runs out between two ADJACENT sizes:
//
//     N = 129 055 → 16 777 154 ids (2²⁴−1 minus 61) → compiles in 30 s
//     N = 129 056 → 16 777 284 ids (2²⁴−1 plus 69) → REFUSED, and the compiler's
//         own words were `verifyLirPostRegalloc: inst 20907039 has a virtual
//         result reg (vreg id 0)` — because 16 777 216 mod 2²⁴ IS 0, the
//         invalid sentinel.
//
// ⚠⚠ AND THAT DIAGNOSTIC IS AN ACCIDENT, WHICH IS THE WORST PART. It fires only
// while the wrap LANDS ON 0. The very next overflowed ids are 1, 2, 3 … which
// collide with real live registers, resolve cleanly, receive allocations and are
// flagged by nothing. At N = 130 000 the same run reported 92 540 spilled vregs
// beside two id-0 errors: 122 789 ids had overflowed and only the ones sitting
// on the sentinel were named.
//
// Further out the aliased hulls (✔MEASURED: one spanning 41 800 000 of the
// function's 51 840 018 positions) made every value unallocatable; at N = 160 000
// the compile had minted 898 406 spill slots and was still running after 900 s.
// The STALL was the lucky outcome. The wrong artifact is the one to fear.
//
// ★★★ THE FIX, AND WHY THIS PIN IS AN IDENTITY AND NOT A RATIO. The sibling
// complexity pin in this directory asserts a GROWTH RATIO, because a complexity
// claim is about how work scales. This defect is not about scaling: it is about
// what a type can NAME. The honest instrument for that is an exact integer
// identity — equally deterministic, equally host- and load-independent, equally
// identical in Debug and Release, and sharper, because it holds or it does not.
// No wall clock appears here, for the reason
// `scripts/check-wall-clock-in-tests/` exists.
//
// The widths are now DERIVED rather than chosen: the class field is the
// narrowest that can name every `LirRegClass` (3 bits), `isPhysical` is one, and
// **the id takes the entire remainder** (28 bits, 268 435 455 ids). There is no
// literal to drift and nothing to configure.
//
// ★★ THE ARMS, AND WHY NO PASS CAN BE VACUOUS.
//   * `TheIdFieldNamesEveryIdTheMintDoorAccepts` — THE PIN. Every id up to
//     `kLirRegMaxId` round-trips. Against the predecessor's literal 24 this
//     fails at the top of the ladder.
//   * `IdsDifferingOnlyAboveTheOldWidthAreDistinctRegisters` — THE PIN, aimed
//     straight at the hazard: two ids congruent modulo 2²⁴ must be two
//     REGISTERS, not one. Against the predecessor they compare EQUAL, which is
//     the aliasing itself, reproduced in one line.
//   * `TheDeclaredMaximumIsTheFieldsTrueCapacity` — THE PREMISE, and it is what
//     stops this file testing a number instead of a type. `kLirRegMaxId` is the
//     value `LirBuilder::newVReg` refuses past; if it were merely *a* bound
//     rather than *the* bound, the refusal would wave truncating ids through and
//     every other arm here would still pass. So: one past it must wrap to the
//     sentinel, and it must sit above the width that truncated — which is also
//     what keeps the arm above from being a statement about nothing.
//   * `TheThreeWidthsSpendTheWholeAllocationUnit` — the derivation, at runtime
//     as well as at compile time.
//   * `OrdinaryRegistersAndTheSealAreUnchangedControl` — THE CONTROL. Size,
//     class round-trip, virtual-versus-physical, the invalid sentinel and the
//     positional-init seal. It must stay GREEN under the mutant, so a red in
//     this file means "the id got narrower", never "something broke".
//   * `TheMintDoorHandsOutDistinctIdsControl` — the second CONTROL, one tier up:
//     the builder really is the single door, and it still mints distinctly.
//
// ⚠ ONE LIMITATION, STATED RATHER THAN PAPERED OVER. Nothing here exercises
// `newVReg`'s REFUSAL, and nothing can: reaching it needs 268 435 456 mints in
// one function, which is minutes and gigabytes. What is pinned instead is the
// property the refusal's threshold depends on — that `kLirRegMaxId` is exactly
// the field's capacity — so a refusal that is too generous is caught here even
// though the refusal itself is not executed. The end-to-end evidence is the
// 129 055 / 129 056 boundary above, reproducible with
// `.temp/sx-island.py` + `.temp/sx2-boundary.sh`.

#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_reg.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace dss;

namespace {

// The width the predecessor's literal gave the id field. It appears here as the
// SUBJECT of the pin — "ids that differ only above this must stay distinct" —
// and nowhere in `src/`, where the widths are derived.
constexpr std::uint32_t kWidthThatTruncated = 24u;
constexpr std::uint32_t kOldModulus         = std::uint32_t{1} << kWidthThatTruncated;

// Ids are carried through a vector so the narrowing happens on a value the
// optimizer cannot fold into a constant initializer — a constant one would be a
// `-Woverflow` diagnostic at compile time rather than the runtime truncation
// this pin is about.
[[nodiscard]] std::uint32_t opaque(std::uint32_t v) {
    std::vector<std::uint32_t> const box{v};
    return box.front();
}

[[nodiscard]] std::uint32_t idOf(LirReg r) {
    return static_cast<std::uint32_t>(r.id);
}

// ⚠ THROWS, never `abort()`. `abort()` kills the whole test PROCESS, so every
// sibling arm in this executable loses its verdict and the harness cannot say
// which one failed; a throw is reported by GoogleTest as a failure of the ONE
// test that touched it. `no_abort_in_tests_guard` enforces exactly this, and its
// inventory ceiling only ever comes down.
std::shared_ptr<TargetSchema> const& x86Schema() {
    static std::shared_ptr<TargetSchema> const schema = [] {
        auto r = TargetSchema::loadShipped("x86_64");
        if (!r.has_value()) {
            throw std::runtime_error(
                "the shipped x86_64 target schema did not load — this arm needs "
                "a real schema to build a LirBuilder, so the environment is "
                "misconfigured rather than the property being false");
        }
        return *r;
    }();
    return schema;
}

}  // namespace

// ── THE PIN ─────────────────────────────────────────────────────────────────
TEST(LirVRegIdWidth, TheIdFieldNamesEveryIdTheMintDoorAccepts) {
    // A ladder to the top rather than the top alone: a field one bit short
    // fails only at the last rung, and a field many bits short fails early, so
    // the failure message localizes the width instead of merely denying it.
    for (std::uint32_t bit = 0; bit < kLirRegIdBits; ++bit) {
        std::uint32_t const id = opaque((std::uint32_t{1} << bit));
        EXPECT_EQ(idOf(makeVirtualReg(id, LirRegClass::GPR)), id)
            << "a virtual register built with id " << id << " (bit " << bit
            << ") came back naming a DIFFERENT register. The id field is "
               "narrower than the " << kLirRegIdBits
            << " bits kLirRegIdBits claims, so ids past its true width "
               "truncate — and two distinct values then share one virtual "
               "register, which liveness cannot tell apart and "
               "findAllocationConflict cannot catch.";
    }
    EXPECT_EQ(idOf(makeVirtualReg(opaque(kLirRegMaxId), LirRegClass::GPR)),
              kLirRegMaxId)
        << "the largest id the builder will hand out (" << kLirRegMaxId
        << ") does not survive being stored in a LirReg. The refusal in "
           "LirBuilder::newVReg is keyed on this value, so an id field narrower "
           "than it means the door admits ids it then silently corrupts.";
}

TEST(LirVRegIdWidth, IdsDifferingOnlyAboveTheOldWidthAreDistinctRegisters) {
    // The hazard itself, in one line each. Under the predecessor's 24-bit field
    // every one of these pairs compares EQUAL — which is two live values
    // becoming one virtual register.
    for (std::uint32_t const low : {1u, 2u, 7u, 4095u, kOldModulus - 1u}) {
        std::uint32_t const a = opaque(low);
        std::uint32_t const b = opaque(low + kOldModulus);
        ASSERT_LE(b, kLirRegMaxId)
            << "the pin's own subject must be a representable id, or this arm "
               "asserts nothing";
        LirReg const ra = makeVirtualReg(a, LirRegClass::GPR);
        LirReg const rb = makeVirtualReg(b, LirRegClass::GPR);
        EXPECT_NE(idOf(ra), idOf(rb))
            << "ids " << a << " and " << b << " differ by exactly 2^"
            << kWidthThatTruncated << ", and LirReg gave them the SAME id "
            << idOf(ra)
            << ". Liveness keys ranges by id, so these two virtual registers "
               "would share one live range and may be given one physical "
               "register while both are live.";
        EXPECT_FALSE(ra == rb)
            << "two distinct virtual registers compared EQUAL — every "
               "id-keyed table downstream (rangeOf, hintOf, assignments) then "
               "holds one entry where two are owed.";
    }
}

// ── THE PREMISE ─────────────────────────────────────────────────────────────
TEST(LirVRegIdWidth, TheDeclaredMaximumIsTheFieldsTrueCapacity) {
    // Without this, every other arm could pass against a `kLirRegMaxId` that
    // merely happens to be small enough — and `newVReg`'s refusal, which is
    // keyed on exactly this constant, would admit ids the field truncates.
    EXPECT_EQ(idOf(makeVirtualReg(opaque(kLirRegMaxId + 1u), LirRegClass::GPR)),
              0u)
        << "one id past kLirRegMaxId did not wrap to the invalid sentinel, so "
           "kLirRegMaxId is NOT the field's capacity — it is a number smaller "
           "than the field, and the id space is being under-used, or larger, "
           "and LirBuilder::newVReg is admitting ids it corrupts.";
    EXPECT_GT(kLirRegMaxId, kOldModulus - 1u)
        << "kLirRegMaxId is no wider than the " << kWidthThatTruncated
        << "-bit width that truncated, so the distinctness arm in this file "
           "has no representable subject and would pass vacuously.";
}

TEST(LirVRegIdWidth, TheThreeWidthsSpendTheWholeAllocationUnit) {
    EXPECT_EQ(kLirRegIdBits + kLirRegClassBits + 1u, 32u)
        << "the id, class and physicality fields do not spend the 4-byte "
           "allocation unit exactly; an unspent bit is a smaller "
           "largest-compilable-function for no reason.";
    EXPECT_GE(std::size_t{1} << kLirRegClassBits, kLirRegClassCount)
        << "the class field cannot name every LirRegClass.";
    EXPECT_EQ(kLirRegMaxId, (std::uint32_t{1} << kLirRegIdBits) - 1u);
}

// ── THE CONTROLS — these must stay GREEN when the id field is narrowed back ──
TEST(LirVRegIdWidth, OrdinaryRegistersAndTheSealAreUnchangedControl) {
    EXPECT_EQ(sizeof(LirReg), 4u);
    EXPECT_TRUE(std::is_standard_layout_v<LirReg>);
    EXPECT_FALSE(std::is_aggregate_v<LirReg>);  // the positional-init seal

    for (auto const cls : {LirRegClass::GPR, LirRegClass::FPR,
                           LirRegClass::VR, LirRegClass::Flags}) {
        LirReg const v = makeVirtualReg(opaque(12345u), cls);
        EXPECT_EQ(v.regClass(), cls);
        EXPECT_EQ(idOf(v), 12345u);
        EXPECT_EQ(static_cast<unsigned>(v.isPhysical), 0u);
        EXPECT_TRUE(v.valid());

        LirReg const p = makePhysicalReg(opaque(7u), cls);
        EXPECT_EQ(p.regClass(), cls);
        EXPECT_EQ(idOf(p), 7u);
        EXPECT_EQ(static_cast<unsigned>(p.isPhysical), 1u);
        EXPECT_FALSE(v == p);
    }

    EXPECT_FALSE(InvalidLirReg.valid());
    EXPECT_EQ(idOf(InvalidLirReg), 0u);
}

TEST(LirVRegIdWidth, TheMintDoorHandsOutDistinctIdsControl) {
    // One tier up from the type: the builder is the single mint door (it is
    // where the refusal lives), and it still mints monotonically and distinctly.
    LirBuilder b{*x86Schema()};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    std::uint32_t previous = 0;
    for (int i = 0; i < 4096; ++i) {
        LirReg const r = b.newVReg(LirRegClass::GPR);
        EXPECT_GT(idOf(r), previous)
            << "the builder handed out a non-increasing vreg id at mint " << i
            << " — ids 1..N are the contract every id-keyed table relies on.";
        previous = idOf(r);
    }
    b.poison();  // this control never terminates its block; poison, do not abort
    (void)std::move(b).finish();
}
