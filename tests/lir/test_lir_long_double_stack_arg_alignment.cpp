// ── D-CSUBSET-LONG-DOUBLE-STACK-ARG-ALIGNMENT ───────────────────────────────
// ────────────────────────────────────────────────────────────────────────────
//
// THE DEFECT THIS FILE PINS. The outgoing-stack-argument layout advanced with a
// flat pointer-width stride, so a stacked datum whose OWN alignment exceeds the
// slot began at the next 8-byte boundary rather than its own. On x86_64 SysV an
// x87 `long double` after an ODD number of 8-byte stack arguments therefore
// landed at +8 where the ABI puts it at +16. Nothing diagnosed it: DSS agreed
// with DSS in both directions — caller and callee walked the SAME flat rule — so
// the byte offsets matched and the LD-4 runtime witness passed. It was visible
// only across a FOREIGN boundary, where gcc's callee reads at +16 the bytes DSS's
// caller wrote at +8.
//
// ★★ THE RULE IS AN ALIGNMENT, NOT A SIZE, AND THE TWO SHIPPED ABIS DISAGREE
// ABOUT IT — which is the whole reason it is declared per calling convention
// instead of computed. ✔MEASURED 2026-09-02, gcc 13.3.0 and clang 18.1.3
// agreeing byte-for-byte (x86_64 and aarch64-linux-gnu), callee-side incoming
// offsets read off `-O1 -S` (incoming offset 0 = the first stack argument), each
// datum placed after an ODD number of 8-byte stack args so a pad is observable:
//   * x86_64 SysV — `long double` → +16 (NOT +8). `struct{long double;}` → +16.
//     `struct{long x,y;}`, 16 bytes but align 8 → +8. `struct aligned(16)
//     {long x,y;}` → +16. Two 16-BYTE aggregates, two DIFFERENT offsets: the
//     rule cannot be derived from the datum's size.
//   * AAPCS64 — binary128 `long double` → +16, but `struct aligned(16)
//     {long x,y;}` → +8, NOT +16. A composite's alignment is clamped to the
//     slot there while a fundamental type's is honoured.
// The SAME 16-byte-aligned aggregate lands at +16 on one ABI and +8 on the
// other. That is why the vocabulary is two caps (scalar / aggregate) rather than
// one constant, and it is what makes this a config rule and not an x86 branch.
//
// ★★★ WHY THE EXPECTATIONS ARE WRITTEN AS LITERAL OFFSETS AND NOT DERIVED.
// The P23/P25 lesson, restated by the sibling Apple-packing pin: an expectation
// computed from the same declaration the code reads moves BOTH HALVES OF THE
// COMPARISON TOGETHER, so deleting the declaration reddens nothing. Every number
// below is an ABI fact measured above, typed out; the CC is fetched BY NAME from
// the SHIPPED `.target.json`. Delete `stackArgPacking` from the `sysv_amd64` row
// and the x86_64 arms go red; delete it from `aapcs64` and arm (C) goes red.
//
// WHAT EACH ARM ASSERTS:
//   (A) THE PLACEMENT — a stacked `long double` gets its 16-byte boundary on
//       SysV, and an ODD preceding stack-arg count is what makes the pad
//       observable. Plus the negative: an align-8 datum of the SAME size is NOT
//       moved, so the pad is keyed on alignment.
//   (B) SIZING AGREES WITH PLACEMENT — the reservation the outgoing area gets
//       must include the pad. A placement that inserts a byte the size pre-scan
//       does not account for writes past the end of the reserved area, which is
//       stack corruption: strictly worse than the misplacement being fixed.
//   (C) THE TWO ABIS DIVERGE — the same 16-aligned aggregate, two conventions,
//       two answers. This arm is what refutes "just always 16-align 16 bytes".
//   (D) AN UNDECLARED CONVENTION IS BYTE-IDENTICAL to the old flat rule, stated
//       as a property over every shipped CC rather than measured per target.

#include "core/types/target_schema.hpp"
#include "lir/lir_callconv.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

using namespace dss;

namespace {

// The SysV x87 `long double`: 16 bytes of storage, 16-byte alignment.
constexpr std::uint32_t kLongDoubleBytes = 16;
constexpr std::uint32_t kLongDoubleAlign = 16;
constexpr std::uint32_t kSlot            = 8;

struct Shipped {
    std::shared_ptr<TargetSchema>  schema;
    TargetCallingConvention const* cc = nullptr;
};

// Fetch a shipped target's CC BY NAME. By name and not by index for the reason
// the sibling pin states: the rows differ only in the declarations under test, so
// an index typo would silently compare a row against itself and pass.
[[nodiscard]] Shipped loadCc(char const* target, char const* ccName) {
    Shipped out;
    auto loaded = TargetSchema::loadShipped(target);
    if (!loaded) {
        ADD_FAILURE() << "TargetSchema::loadShipped(" << target << ") failed";
        return out;
    }
    out.schema = *loaded;
    out.cc     = out.schema->callingConventionByName(ccName);
    if (out.cc == nullptr)
        ADD_FAILURE() << target << " declares no calling convention '" << ccName << "'";
    return out;
}

// ── (A) THE PLACEMENT ───────────────────────────────────────────────────────

TEST(LongDoubleStackArgAlignment, ALongDoubleAfterAnOddStackArgCountIsSixteenAligned) {
    auto const t = loadCc("x86_64", "sysv_amd64");
    ASSERT_NE(t.cc, nullptr);

    // ✔MEASURED (gcc 13.3.0 + clang 18.1.3, `f(long a1..a7, long double)`): a7 is
    // the first stack argument at incoming +0, and the `long double` is read with
    // `fldt 24(%rsp)` = incoming +16. The flat 8-byte stride this row records put
    // it at +8.
    StackArgCursor odd{*t.cc, kSlot};
    EXPECT_EQ(odd.placeNamedScalar(kSlot).byteOffset, 0u)
        << "the first stacked 8-byte argument";
    EXPECT_EQ(odd.placeNamedAggregate(kLongDoubleBytes, kLongDoubleAlign), 16u)
        << "a stacked long double after an ODD 8-byte-arg count must skip to the "
           "16-byte boundary — this is the anchor's whole subject";

    // ✔MEASURED (`f(long a1..a8, long double)`): with the cursor ALREADY at 16 no
    // pad is owed, and the long double still reads at incoming +16. An EVEN count
    // is the control that shows the pad is conditional rather than constant.
    StackArgCursor even{*t.cc, kSlot};
    EXPECT_EQ(even.placeNamedScalar(kSlot).byteOffset, 0u);
    EXPECT_EQ(even.placeNamedScalar(kSlot).byteOffset, 8u);
    EXPECT_EQ(even.placeNamedAggregate(kLongDoubleBytes, kLongDoubleAlign), 16u)
        << "an already-16-aligned cursor owes no pad";
}

TEST(LongDoubleStackArgAlignment, TheSameSizeWithAnEightByteAlignmentIsNotMoved) {
    auto const t = loadCc("x86_64", "sysv_amd64");
    ASSERT_NE(t.cc, nullptr);

    // ✔MEASURED: `f(long a1..a7, struct{long x,y;})` reads the struct at incoming
    // +8 — SIXTEEN BYTES, but alignment 8, so it is NOT padded. This is the arm
    // that refutes a size-keyed rule: over-aligning it would be a byte-offset
    // mismatch in the OTHER direction, i.e. a new silent miscompile rather than a
    // fix.
    StackArgCursor c{*t.cc, kSlot};
    EXPECT_EQ(c.placeNamedScalar(kSlot).byteOffset, 0u);
    EXPECT_EQ(c.placeNamedAggregate(/*aggBytes=*/16, /*aggAlign=*/8), 8u)
        << "a 16-BYTE, 8-ALIGNED aggregate keeps the flat stride";

    // A carrier that states NO alignment (0) is the pre-existing producer, and it
    // must land exactly where it always did. Silence is not an over-alignment
    // request.
    StackArgCursor unstated{*t.cc, kSlot};
    EXPECT_EQ(unstated.placeNamedScalar(kSlot).byteOffset, 0u);
    EXPECT_EQ(unstated.placeNamedAggregate(/*aggBytes=*/16, /*aggAlign=*/0), 8u)
        << "an unstated alignment floors at the slot";
}

// ── (B) SIZING AGREES WITH PLACEMENT ────────────────────────────────────────

TEST(LongDoubleStackArgAlignment, TheReservationIncludesThePad) {
    auto const t = loadCc("x86_64", "sysv_amd64");
    ASSERT_NE(t.cc, nullptr);

    // `computeMaxOutgoingStackArgs` sizes the outgoing area from THIS value while
    // `lowerWideCallArgs` places from the same object. If the two disagreed by the
    // pad, the caller would store the long double past the end of the area it
    // reserved — over its own frame. One slot arg (8) + pad (8) + the long double
    // (16) = 32 bytes reserved, not 24.
    StackArgCursor c{*t.cc, kSlot};
    (void)c.placeNamedScalar(kSlot);
    (void)c.placeNamedAggregate(kLongDoubleBytes, kLongDoubleAlign);
    EXPECT_EQ(c.bytes(), 32u)
        << "8 (the stacked scalar) + 8 (the alignment pad) + 16 (the long double)";
    EXPECT_EQ(c.slotAlignedBytes(), 32u)
        << "already a whole number of slots";
}

// ── (C) THE TWO ABIS DIVERGE ────────────────────────────────────────────────

TEST(LongDoubleStackArgAlignment, TheAggregateCapDiffersBetweenSysvAndAapcs64) {
    auto const x86 = loadCc("x86_64", "sysv_amd64");
    auto const arm = loadCc("arm64", "aapcs64");
    ASSERT_NE(x86.cc, nullptr);
    ASSERT_NE(arm.cc, nullptr);

    // ✔MEASURED, the SAME source compiled for both targets: `struct
    // __attribute__((aligned(16))){long x,y;}` after one stacked 8-byte argument
    // reads at incoming +16 on x86_64 SysV and at incoming +8 on AAPCS64, where a
    // COMPOSITE's alignment is clamped to the slot. Two ABIs, one datum, two
    // answers — so the rule cannot be a constant in the cursor.
    StackArgCursor onX86{*x86.cc, kSlot};
    (void)onX86.placeNamedScalar(kSlot);
    EXPECT_EQ(onX86.placeNamedAggregate(/*aggBytes=*/16, /*aggAlign=*/16), 16u)
        << "SysV honours a 16-byte aggregate alignment";

    StackArgCursor onArm{*arm.cc, kSlot};
    (void)onArm.placeNamedScalar(kSlot);
    EXPECT_EQ(onArm.placeNamedAggregate(/*aggBytes=*/16, /*aggAlign=*/16), 8u)
        << "AAPCS64 clamps a COMPOSITE's alignment to the slot — this arm is what "
           "proves the rule is read from the config and not hardcoded";

    // The SCALAR axis, however, agrees: AAPCS64 rounds the NSAA up to a
    // FUNDAMENTAL type's own alignment, so a binary128 `long double` gets +16.
    StackArgCursor armScalar{*arm.cc, kSlot};
    (void)armScalar.placeNamedScalar(kSlot);
    EXPECT_EQ(armScalar.placeNamedScalar(/*naturalBytes=*/16).byteOffset, 16u)
        << "AAPCS64 honours a FUNDAMENTAL type's 16-byte alignment";
}

// ── (D) AN UNDECLARED CONVENTION IS UNCHANGED ───────────────────────────────

TEST(LongDoubleStackArgAlignment, AnUndeclaredConventionKeepsTheFlatStride) {
    // Win64 declares no caps: it passes anything wider than a slot BY REFERENCE,
    // so no stacked datum out-aligns the slot. Stated as a property over the whole
    // argument-size domain rather than at one point, which is what makes the
    // "every other target is byte-identical" claim structural.
    auto const t = loadCc("x86_64", "ms_x64");
    ASSERT_NE(t.cc, nullptr);
    EXPECT_EQ(t.cc->stackArgPacking.maxScalarAlignment, 0u);
    EXPECT_EQ(t.cc->stackArgPacking.maxAggregateAlignment, 0u);

    for (std::uint32_t align : {0u, 1u, 2u, 4u, 8u, 16u, 32u}) {
        StackArgCursor c{*t.cc, kSlot};
        EXPECT_EQ(c.placeNamedScalar(kSlot).byteOffset, 0u) << "align=" << align;
        EXPECT_EQ(c.placeNamedAggregate(kLongDoubleBytes, align), kSlot)
            << "align=" << align
            << ": an undeclared cap honours NO over-alignment, whatever the datum "
               "asks for";
    }
}

}  // namespace
