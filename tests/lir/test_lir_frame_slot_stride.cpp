// ── D-LIR-FRAME-SLOT-STRIDE-ENUMERATES-CLASSES-INSTEAD-OF-DERIVING ─────────
// ────────────────────────────────────────────────────────────────────────────
//
// THE DEFECT THIS FILE PINS, and it was LIVE rather than latent.
// `computeFrameLayout` sized its two uniform-stride frame areas — the saved-
// register area and the spill area — with `max(widthForClass(GPR),
// widthForClass(FPR))`: a TWO-MEMBER ENUMERATION of a register-class
// vocabulary that has more members than that. arm64 declared `vr` at
// `widthBytes` 16 while its `gpr` and `fpr` were both 8, so the stride came out
// 8 and a spilled VR value was accessed SIXTEEN bytes wide inside an EIGHT-byte
// slot.
//
// ⚠ THAT CONFIG NO LONGER EXISTS, AND THE FILE IS ABOUT THE DERIVATION RATHER
// THAN ABOUT arm64. R1 of design A′ made arm64 declare its SIMD&FP file ONCE,
// so the 16-byte rows are class `fpr` and `max(GPR, FPR)` covers them —
// ✔RE-MEASURED, arm64's floor is now 16 rather than 8, which is a real change
// to every arm64 frame and is exactly what arm (A) checks stays consistent.
// The historical measurement below is kept as the record of WHY the
// enumeration was wrong; arm (C) reproduces the shape on a fixture.
//
// ✔MEASURED 2026-08-23 (cycle P28) on the shipped arm64 release pipeline, two
// `"w"` (VR-class) inline-asm outputs in one function, disassembled with
// `aarch64-linux-gnu-objdump -d`:
//     BEFORE                          AFTER
//     sub  sp, sp, #0x20              sub  sp, sp, #0x40
//     ldur q0, [sp, #16]              ldur q0, [sp, #32]
//     ldur q1, [sp, #24]   <- +8      ldur q1, [sp, #48]   <- +16
// The two 16-byte slots OVERLAPPED by 8 bytes, and the second read 8 bytes PAST
// the top of the frame — AAPCS64 declares `redZoneBytes` 0, so that is the
// caller's stack. rc=0, no diagnostic, both configs.
//
// ★★ WHY THE PIN IS OVER THE DERIVATION AND NOT OVER A NUMBER. Adding `VR` to
// the `max(...)` would fix this disassembly and leave the defect: the next
// class declared wider than the listed ones repeats it exactly. The stride is
// now RAISED to cover every class that actually occupies a slot in THIS
// function, so a class nobody spills cannot inflate a frame and a class
// somebody does spill cannot be missed.
//
// ★ WHAT EACH ARM ASSERTS:
//   (A) THE FLOOR IS INTACT — a function with no wide spill keeps the historic
//       `max(GPR, FPR)` stride, so this fix has ZERO blast radius on every
//       frame in the corpus. Pinned as a byte-level frame claim, not a
//       recomputation of the same expression.
//   (B) THE STRIDE COVERS THE WIDEST OCCUPANT — for every shipped target, the
//       stride a function with a spill of class C gets is ≥ C's register
//       width. Stated over the whole `LirRegClass` vocabulary, so a new class
//       is covered on the day it is declared.
//   (C) THE DEFECT'S OWN SHAPE — ⚠ AND IT IS NO LONGER PRESENT IN THE SHIPPED
//       CORPUS, WHICH THIS ARM SAYS OUT LOUD RATHER THAN QUIETLY PASSING. It
//       required a shipped target to declare a class WIDER than both GPR and
//       FPR, "or this file is pinning nothing". R1 of design A′ folded arm64's
//       16-byte `vr` file INTO `fpr`, so `max(GPR, FPR)` now covers every
//       class on both targets and the precondition is gone. The derivation is
//       therefore pinned by a SYNTHESIZED wider class instead of by the
//       corpus — never by observing that the corpus happens not to need it.

#include "core/types/target_schema.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_reg.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

using namespace dss;

namespace {

constexpr char const* kTargets[] = {"x86_64", "arm64"};

// The LIR class vocabulary's size, taken from the enum's last enumerator so a
// new class is walked on the day it is declared rather than when someone
// remembers a literal.
constexpr std::size_t kRegClassCount =
    static_cast<std::size_t>(LirRegClass::Flags) + 1u;

// The widest register `schema` declares in `cls`, 0 if it declares none. A
// re-statement of the private helper the layout uses — deliberately re-derived
// here from the public register table so the pin does not read the same
// expression the code does.
[[nodiscard]] std::uint32_t widestIn(TargetSchema const& schema,
                                     TargetRegClass cls) {
    std::uint32_t w = 0;
    for (auto const& r : schema.registers()) {
        if (r.regClass == cls) w = std::max(w, static_cast<std::uint32_t>(r.widthBytes));
    }
    return w;
}

} // namespace

// ── (C) THE DEFECT'S OWN SHAPE, SYNTHESIZED ─────────────────────────────────
//
// ⚠⚠ WHAT THIS ARM USED TO ASSERT AND WHY IT COULD NOT SURVIVE UNCHANGED. It
// was `AShippedTargetDeclaresAClassWiderThanItsGprAndFpr`, and it walked both
// shipped targets requiring SOME class wider than `max(GPR, FPR)` — "arm64's
// `vr` (widthBytes 16, against gpr/fpr at 8) is the case that made the
// enumeration wrong". ✔That was a true measurement of a config that declared
// arm64's SIMD&FP file TWICE. R1 of design A′ declared it ONCE: the 16-byte
// rows are class `fpr` now, arm64 declares no `vr` register at all, and
// `max(GPR, FPR)` covers every class on both targets. The precondition the
// whole file rested on is therefore ABSENT FROM THE CORPUS.
//
// ★★★ WHICH MAKES THE DERIVATION SOMETHING ONLY A SYNTHESIZED NEGATIVE CAN
// PIN. If this arm were merely deleted, arms (A) and (B) would still pass on a
// corpus where the two-member `max(GPR, FPR)` is accidentally right — exactly
// the vacuous green the original arm existed to prevent, arrived at by the
// opposite route. So the wider class is BUILT here: a fixture target whose
// `vr` rows are 32 bytes against 8-byte gpr/fpr, which the derivation must
// cover and the old enumeration could not.
TEST(LirFrameSlotStride, AWiderClassRaisesTheStrideAboveTheGprFprFloor) {
    // ⓘ Written as a document rather than a mutated shipped target: the point
    // is a class the corpus does NOT have, so there is nothing to mutate.
    auto s = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"wideVectorFixture"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[
              {"name":"x0","class":"gpr","widthBytes":8,"hwEncoding":0},
              {"name":"sp","class":"gpr","widthBytes":8,"hwEncoding":1},
              {"name":"f0","class":"fpr","widthBytes":8,"hwEncoding":0},
              {"name":"z0","class":"vr","widthBytes":32,"hwEncoding":0}],
            "callingConventions":[
              {"name":"cc","argGprs":["x0"],"argFprs":["f0"],
               "stackPointer":"sp","stackAlignment":16}
            ]})",
        "<inline>");
    ASSERT_TRUE(s.has_value())
        << "the fixture target must load, or this arm measures nothing";

    auto const floorWidth = std::max(widestIn(**s, TargetRegClass::GPR),
                                     widestIn(**s, TargetRegClass::FPR));
    ASSERT_EQ(floorWidth, 8u);
    ASSERT_EQ(widestIn(**s, TargetRegClass::VR), 32u)
        << "the fixture must really declare a class wider than both of the two "
           "the old expression named — that width IS the precondition";

    // The floor is untouched by a class nobody spills…
    EXPECT_EQ(frameSlotStrideForClasses(**s, {}), floorWidth)
        << "a class that occupies no slot must not inflate every frame";
    EXPECT_EQ(frameSlotStrideForClasses(**s, {LirRegClass::FPR}), floorWidth);
    // …and RAISED by one that does. `max(GPR, FPR)` answers 8 here, which is
    // the arm64 `ldur q0,[sp,#16]` / `ldur q1,[sp,#24]` overlap this file
    // records, reproduced on a config that still has the shape.
    EXPECT_EQ(frameSlotStrideForClasses(**s, {LirRegClass::VR}), 32u)
        << "the stride did not cover the widest OCCUPANT — two 32-byte slots "
           "at an 8-byte stride overlap by 24 bytes and the second reads past "
           "the top of the frame";
}

// ── (A) THE FLOOR IS INTACT ─────────────────────────────────────────────────
TEST(LirFrameSlotStride, AFunctionWithNoWideSpillKeepsTheHistoricStride) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const* cc = (*s)->callingConvention(0);
        ASSERT_NE(cc, nullptr);

        auto const historic = std::max(widestIn(**s, TargetRegClass::GPR),
                                       widestIn(**s, TargetRegClass::FPR));
        ASSERT_GT(historic, 0u);
        EXPECT_EQ(frameSlotStrideForClasses(**s, {}), historic)
            << "a function that spills nothing and saves nothing must keep the "
               "stride every frame in the corpus was laid out with — this fix "
               "is not allowed to move a single existing frame byte";
        // The GPR class alone must not lower it either: x86_64's FPR (xmm, 16)
        // is wider than its GPR (8), and shrinking to 8 would re-open the local
        // -alloca alignment the floor protects.
        EXPECT_EQ(frameSlotStrideForClasses(**s, {LirRegClass::GPR}), historic);
    }
}

// ── (B) THE STRIDE COVERS THE WIDEST OCCUPANT ───────────────────────────────
TEST(LirFrameSlotStride, TheStrideCoversEveryClassThatOccupiesASlot) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());

        for (std::size_t i = 0; i < kRegClassCount; ++i) {
            auto const cls = static_cast<LirRegClass>(i);
            SCOPED_TRACE(std::string{lirRegClassName(cls)});
            auto const declared =
                widestIn(**s, static_cast<TargetRegClass>(i));
            if (declared == 0) continue;  // class not declared on this target
            EXPECT_GE(frameSlotStrideForClasses(**s, {cls}), declared)
                << "a value of this class occupies a frame slot, so the slot "
                   "stride must be at least as wide as the register — a "
                   "narrower stride makes slot k and slot k+1 overlap, which "
                   "is the arm64 `ldur q0,[sp,#16]` / `ldur q1,[sp,#24]` "
                   "measurement this file records";
        }
    }
}
