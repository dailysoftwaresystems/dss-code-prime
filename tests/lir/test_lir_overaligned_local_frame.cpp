// ── D-CSUBSET-ALIGNAS-OVERALIGNED-STACK-LOCAL ──────────────────────────────
// ────────────────────────────────────────────────────────────────────────────
//
// THE DEFECT THIS FILE PINS. A stack local whose effective alignment exceeds the
// calling convention's `stackAlignment` — `alignas(32) int buf[4];`, or a local of a
// struct a member `alignas` raised above 16 — was REFUSED outright:
// `computeFrameLayout` reported `L_OverAlignedStackLocal` and the compile failed.
// ✔MEASURED 2026-09-01 at `dac121cc`, all four shipped target/format pairs, and the
// three references separately: gcc 13.3.0, clang 18.1.3 and mingw-w64 gcc 13.2.0
// each compile the same program AND produce a genuinely aligned local (runtime
// address check, exit 42); MSVC 19.51.36252 accepts it and emits an aligned
// frame-base pointer (`lea rbp,[rsp+112]` / `and rbp,-32`). Under
// `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` a unanimous acceptance makes it required.
//
// ★★ WHY NO OFFSET CAN DO IT, WHICH IS THE FACT THE WHOLE DESIGN TURNS ON. The
// post-prologue stack pointer is congruent to 0 modulo `stackAlignment` and NO
// FINER — that congruence is all an ABI promises at a call boundary. `sp + <constant
// offset>` is therefore only ever `stackAlignment`-aligned no matter which constant
// is chosen. So the frame reserves SPARE BYTES above such a slot and the address is
// rounded up at run time. The two derivations below are the whole of the frame's
// half of that, and this file pins them.
//
// ★★★ WHY THE PINS ARE OVER THE DERIVATIONS AND OVER A PROPERTY, NOT OVER NUMBERS.
// A table of expected offsets for `alignas(32)` on today's two targets would pass
// against a headroom rule that is right at 32 and wrong at 64, and would say nothing
// about a future ABI whose `stackAlignment` is not 16. Arm (B) instead asserts the
// SUFFICIENCY PROPERTY the reservation exists to provide — exhaustively, over every
// base the frame can produce — so a headroom that is one byte short at any alignment
// reddens. That property's violation is precisely the silent miscompile: a rounded
// address landing in the NEXT local's bytes.
//
// ⚠ THIS FILE DELIBERATELY DOES NOT DRIVE THE UNIT-TIER FRONT END — and the
// reason has CHANGED, so read this rather than inheriting the old one.
// [[D-LIR-TEST-FRONT-END-LOWERS-A-MANY-ARG-CALL-TO-NOTHING-SO-PINS-MEASURE-ZERO]]
// is CLOSED (P49): `lowerCToLir` no longer discards a refusal, and the reason it
// used to return `lowerOk` true over a mutilated module was a NULL `ffiMap` that
// refused every source carrying a prototype — now threaded. So the old sentence
// here ("any `tests/lir` pin whose subject is a frame, a spill or a saved
// register can be VACUOUS while looking green") no longer holds, and nothing in
// this file rests on it.
// What DOES still hold is the positive reason: everything here is either a pure
// function called directly or a fact read off the shipped schema, which is a
// STRONGER pin than any lowered program for a SUFFICIENCY property — there is no
// allocator decision that could make a derivation look right by accident. The
// end-to-end evidence lives in the corpus, where it is unforgeable:
// `examples/c/alignas_local_over32`, `alignas_local_cacheline64` and
// `alignas_member_over32_local` each RUN on all four targets under both the
// baseline and the shipped release pipeline and check the emitted address at run
// time.

#include "core/types/target_schema.hpp"
#include "lir/lir_callconv.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace dss;

namespace {

constexpr char const* kTargets[] = {"x86_64", "arm64"};

// `alignUp`, re-derived here from the definition rather than shared with the pass,
// so the property arm below does not check an expression against itself.
[[nodiscard]] constexpr std::uint64_t roundUpTo(std::uint64_t v,
                                                std::uint64_t a) noexcept {
    return ((v + a - 1u) / a) * a;
}

} // namespace

// ── (A) ZERO BLAST RADIUS ───────────────────────────────────────────────────
//
// Every alloca at or below the frame's own alignment must keep its exact
// pre-change placement and reserve NOTHING extra. This is the claim that lets the
// change ship without re-measuring every frame in the corpus: an alignment the frame
// already satisfied must not move one byte. Stated against each SHIPPED convention's
// declared `stackAlignment` rather than against the literal 16, so a convention that
// declares something else is covered on the day it does.
TEST(LirOverAlignedLocalFrame, AnAlignmentTheFrameAlreadySatisfiesMovesNothing) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const ccCount =
            static_cast<std::uint16_t>((*s)->callingConventionCount());
        for (std::uint16_t ci = 0; ci < ccCount; ++ci) {
            auto const* cc = (*s)->callingConvention(ci);
            ASSERT_NE(cc, nullptr);
            auto const frameAlign = static_cast<std::uint32_t>(cc->stackAlignment);
            ASSERT_GT(frameAlign, 0u)
                << "a convention with no stack alignment cannot place a local at all";
            SCOPED_TRACE(cc->name);

            // No override at all: the identity, so `alignUp` by it is a no-op.
            EXPECT_EQ(frameSlotPlacementAlign(0u, frameAlign), 1u);
            EXPECT_EQ(frameSlotAlignHeadroom(0u, frameAlign), 0u);

            for (std::uint32_t a = 1; a <= frameAlign; a *= 2u) {
                SCOPED_TRACE(a);
                EXPECT_EQ(frameSlotPlacementAlign(a, frameAlign), a)
                    << "an alloca at or below the frame's own alignment must be "
                       "placed at exactly its declared alignment — capping it here "
                       "would move a local that has always been correctly placed";
                EXPECT_EQ(frameSlotAlignHeadroom(a, frameAlign), 0u)
                    << "reserving spare bytes for an alignment a static offset "
                       "already delivers would grow every existing frame";
            }
        }
    }
}

// ── (B) THE RESERVATION IS SUFFICIENT ───────────────────────────────────────
//
// THE PROPERTY, and the one whose violation is a silent miscompile: for every raw
// slot address the frame can produce, rounding it up to the alloca's alignment must
// land inside the bytes reserved for THAT alloca. The raw address is
// `placementAlign`-aligned by construction (the local-area base is padded to it and
// the running offset is rounded to it, on top of the stack pointer's own congruence),
// so the space of raw addresses is exhaustively enumerable as multiples of the
// placement alignment — every residue the frame can ever present, not a sample.
//
// A too-small headroom passes an `alignas(32)` example and overruns at 64; a
// headroom of `align` rather than `align - frameAlign` wastes a slot's worth of
// stack in every over-aligned frame. Both directions are checked.
TEST(LirOverAlignedLocalFrame, TheHeadroomCoversEveryRoundingTheFrameCanProduce) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const* cc = (*s)->callingConvention(0);
        ASSERT_NE(cc, nullptr);
        auto const frameAlign = static_cast<std::uint32_t>(cc->stackAlignment);
        ASSERT_GT(frameAlign, 0u);

        for (std::uint32_t a = frameAlign * 2u; a <= 4096u; a *= 2u) {
            SCOPED_TRACE(a);
            auto const place = frameSlotPlacementAlign(a, frameAlign);
            auto const head  = frameSlotAlignHeadroom(a, frameAlign);
            ASSERT_EQ(place, frameAlign)
                << "an over-aligned alloca is placed at the frame's own alignment — "
                   "rounding its OFFSET finer is idle, since its BASE is no better "
                   "aligned than that";
            ASSERT_GT(head, 0u)
                << "an alignment above the frame's guarantee must reserve something, "
                   "or the runtime rounding writes into the next local";

            // Every raw address the frame can present for this alloca: a multiple of
            // the placement alignment. One full period of `a` covers every residue.
            std::uint32_t worst = 0;
            for (std::uint64_t raw = 0; raw < a; raw += place) {
                auto const rounded = roundUpTo(raw, a);
                auto const shift   = static_cast<std::uint32_t>(rounded - raw);
                ASSERT_LE(shift, head)
                    << "rounding a raw slot address up moved it " << shift
                    << " bytes, past the " << head
                    << " reserved — the rounded local overlaps its neighbour, which "
                       "is the silent miscompile this reservation exists to prevent";
                worst = std::max(worst, shift);
            }
            EXPECT_EQ(worst, head)
                << "the reservation must be TIGHT as well as sufficient: a headroom "
                   "larger than the worst rounding wastes stack in every frame that "
                   "carries an over-aligned local";
        }
    }
}

// ── (C) THE CAP IS THE FRAME'S GUARANTEE, NOT A LITERAL ─────────────────────
//
// Both derivations are functions of the CONVENTION's stack alignment. Every shipped
// ABI declares 16, so a rule written against the literal 16 would be indistinguishable
// from the correct one on this corpus — the same shape as the `max(GPR, FPR)` stride
// enumeration that was accidentally right until it was not. Synthesized against a
// convention declaring something else, which is the only way to tell them apart.
TEST(LirOverAlignedLocalFrame, TheBoundFollowsTheConventionNotTheShippedNumber) {
    // A 16-aligned frame caps a 32-byte local at 16 and reserves 16.
    EXPECT_EQ(frameSlotPlacementAlign(32u, 16u), 16u);
    EXPECT_EQ(frameSlotAlignHeadroom(32u, 16u), 16u);
    // A 32-aligned frame ALREADY carries that local: no cap, no reservation.
    EXPECT_EQ(frameSlotPlacementAlign(32u, 32u), 32u);
    EXPECT_EQ(frameSlotAlignHeadroom(32u, 32u), 0u)
        << "a convention whose stack alignment already meets the request must "
           "reserve nothing — a rule keyed on the literal 16 reserves 16 here and "
           "grows every frame on that ABI for no reason";
    // A coarser frame reserves less for the same request, not more.
    EXPECT_LT(frameSlotAlignHeadroom(64u, 32u), frameSlotAlignHeadroom(64u, 16u));
    // An 8-aligned frame — below every shipped ABI — must still be handled by the
    // same expression rather than by an assumption.
    EXPECT_EQ(frameSlotPlacementAlign(16u, 8u), 8u);
    EXPECT_EQ(frameSlotAlignHeadroom(16u, 8u), 8u);
}
