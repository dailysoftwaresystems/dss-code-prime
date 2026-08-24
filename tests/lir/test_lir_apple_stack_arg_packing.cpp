// ── D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED ───────────────────
// ────────────────────────────────────────────────────────────────────────────
//
// THE DEFECT THIS FILE PINS. Every argument that overflowed the argument
// registers was padded to a whole pointer-width slot, on every calling
// convention, because the LIR tier had no way to know how big an argument was:
// `LirReg` is four bytes of class and ordinal, and the overflow cursor was
// literally `index * outgoingSlotSize`. That is correct for AAPCS64, SysV and
// Win64 — and WRONG for Apple's arm64 ABI, whose NAMED SCALARS are packed at
// their own alignment and advanced by their own size. Nothing diagnosed it: DSS
// agreed with DSS in both directions, so it only showed up against a foreign
// compiler, where the caller wrote a `short` at +8 and the callee read it at +2.
//
// ★★ THE RULE HAS THREE AXES AND THEY MEASURABLY DIFFER, which is the whole
// reason the config object is three fields and not a boolean. ✔MEASURED
// 2026-08-24 with Apple clang 21.0.0 on macOS 26.5.2 (`otool -tV` of a
// `-target arm64-apple-macos` object) against `aarch64-linux-gnu-gcc 13.3.0`
// (`objdump -d`) on the same sources:
//   * NAMED SCALARS — Apple packs naturally. `char,short,int,long,char` land at
//     +0,+2,+4,+8,+16; `float,float,double,float` at +0,+4,+8,+16 (the FPR pool
//     obeys the same rule). AAPCS64 puts each at an 8-byte stride.
//   * NAMED AGGREGATES — BOTH round to whole slots. A 3-byte struct then an
//     `int` puts the int at +8 (not +3); a 12-byte struct puts it at +16.
//   * VARIADIC — BOTH use 8-byte slots (Apple's caller emits `str x` at +8/+16).
// A boolean would have encoded Apple's own ABI wrongly on two of three axes.
//
// ★★★ WHY THE EXPECTATIONS ARE WRITTEN AS LITERAL OFFSETS AND NOT DERIVED.
// The P23/P25 lesson, and it applies exactly here: a pin whose expectation is
// computed from the same declaration the code reads moves BOTH HALVES OF THE
// COMPARISON TOGETHER, so deleting the declaration reddens nothing. The offsets
// below are the ABI facts measured above, typed out; the CC is fetched BY NAME
// from the SHIPPED `arm64.target.json`. Remove `stackArgPacking` from the
// `apple_arm64` row and the Apple arms go red while the AAPCS64 arms stay green
// — which is the red-on-disable this row owes.
//
// WHAT EACH ARM ASSERTS:
//   (A) THE CURSOR — the placement rule itself, over the shipped CCs, for all
//       three axes plus the aggregate/scalar interleave.
//   (B) THE ACCESS WIDTH IS PART OF THE PLACEMENT — a naturally-packed datum
//       must be accessed width-exactly, because its neighbour begins inside
//       what a slot-wide access would touch. Slot packing keeps the 64-bit
//       access, which is what makes every other target byte-identical.
//   (C) THE `va_start` OVERFLOW BASE, at the MIR tier, asserting the numbers
//       that were PREDICTED from the re-derived rule and only then measured on
//       real Apple Silicon: +8 for `(…, char, short, ...)` and +16 for
//       `(…, int, char, int, ...)`. Slot packing gives 16 and 24. Keeping the
//       predicted values (rather than "whatever the code says today") is what
//       makes this pin outlive an implementation that merely agrees with itself.
//   (D) END TO END — the real MIR→LIR→regalloc→callconv pipeline, reading the
//       callee's incoming-argument loads off the emitted LIR.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "mir/mir_opcode.hpp"

#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

using namespace dss;

namespace {

// The shipped arm64 target, and its two calling conventions BY NAME. Fetching by
// name rather than by index is not tidiness: the two rows differ only in the
// declarations under test, so an index typo would silently compare a row against
// itself and pass.
struct Arm64Ccs {
    std::shared_ptr<TargetSchema>  schema;
    TargetCallingConvention const* aapcs64 = nullptr;
    TargetCallingConvention const* apple   = nullptr;
    std::uint16_t                  appleIndex   = 0;
    std::uint16_t                  aapcs64Index = 0;
};

[[nodiscard]] Arm64Ccs loadArm64() {
    Arm64Ccs out;
    auto loaded = TargetSchema::loadShipped("arm64");
    if (!loaded) {
        ADD_FAILURE() << "TargetSchema::loadShipped(arm64) failed";
        return out;
    }
    out.schema  = *loaded;
    out.aapcs64 = out.schema->callingConventionByName("aapcs64");
    out.apple   = out.schema->callingConventionByName("apple_arm64");
    for (std::uint16_t i = 0;; ++i) {
        auto const* cc = out.schema->callingConvention(i);
        if (cc == nullptr) break;
        if (cc->name == "apple_arm64") out.appleIndex = i;
        if (cc->name == "aapcs64")     out.aapcs64Index = i;
    }
    return out;
}

constexpr std::uint32_t kSlot = 8;

// ── (A) THE CURSOR ──────────────────────────────────────────────────────────

TEST(AppleStackArgPacking, NamedScalarsPackNaturallyOnAppleAndBySlotOnAapcs64) {
    auto const t = loadArm64();
    ASSERT_NE(t.apple, nullptr);
    ASSERT_NE(t.aapcs64, nullptr);

    // ✔MEASURED (otool -tV, Apple clang 21.0.0): char,short,int,long,char.
    std::vector<std::uint32_t> const sizes{1, 2, 4, 8, 1};
    std::vector<std::uint32_t> const appleOffsets{0, 2, 4, 8, 16};
    std::vector<std::uint32_t> const aapcsOffsets{0, 8, 16, 24, 32};

    StackArgCursor appleCursor{*t.apple, kSlot};
    StackArgCursor aapcsCursor{*t.aapcs64, kSlot};
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        EXPECT_EQ(appleCursor.placeNamedScalar(sizes[i]).byteOffset,
                  appleOffsets[i])
            << "apple_arm64 named scalar #" << i << " (size " << sizes[i]
            << "): Apple aligns to the datum's own alignment and advances by its "
               "own size — a whole-slot stride here is the defect this row names";
        EXPECT_EQ(aapcsCursor.placeNamedScalar(sizes[i]).byteOffset,
                  aapcsOffsets[i])
            << "aapcs64 named scalar #" << i << " must keep the 8-byte slot "
               "stride gcc emits — this is the ELF leg and it must not move";
    }
    // The named region ends where the first vararg would begin.
    EXPECT_EQ(appleCursor.slotAlignedBytes(), 24u);
    EXPECT_EQ(aapcsCursor.slotAlignedBytes(), 40u);
}

TEST(AppleStackArgPacking, TheFprPoolObeysTheSameRuleAsTheGprPool) {
    auto const t = loadArm64();
    ASSERT_NE(t.apple, nullptr);
    // ✔MEASURED: float,float,double,float -> +0,+4,+8,+16.
    StackArgCursor c{*t.apple, kSlot};
    EXPECT_EQ(c.placeNamedScalar(4).byteOffset, 0u);
    EXPECT_EQ(c.placeNamedScalar(4).byteOffset, 4u);
    EXPECT_EQ(c.placeNamedScalar(8).byteOffset, 8u);
    EXPECT_EQ(c.placeNamedScalar(4).byteOffset, 16u)
        << "a float after a double is at +16, not +12: the double left the cursor "
           "at 16 and 4-byte alignment does not move it";
}

TEST(AppleStackArgPacking, VariadicArgsKeepWholeSlotsOnBothConventions) {
    auto const t = loadArm64();
    ASSERT_NE(t.apple, nullptr);
    ASSERT_NE(t.aapcs64, nullptr);
    // ✔MEASURED: Apple's caller emits `str x` at +8,+16 for stacked varargs —
    // 8-byte slots even though its NAMED scalars pack naturally. This is the
    // axis a boolean would have got wrong.
    for (auto const* cc : {t.apple, t.aapcs64}) {
        StackArgCursor c{*cc, kSlot};
        EXPECT_EQ(c.placeVariadic(1).byteOffset, 0u) << cc->name;
        EXPECT_EQ(c.placeVariadic(2).byteOffset, 8u) << cc->name;
        EXPECT_EQ(c.placeVariadic(4).byteOffset, 16u) << cc->name;
    }
}

TEST(AppleStackArgPacking, NamedAggregatesKeepWholeSlotsOnBothConventions) {
    auto const t = loadArm64();
    ASSERT_NE(t.apple, nullptr);
    ASSERT_NE(t.aapcs64, nullptr);
    // ✔MEASURED: a 3-byte struct then an `int` puts the int at +8 on Apple (NOT
    // +3); a 12-byte struct puts it at +16 (NOT +12). The third axis, and the
    // one that keeps Apple's aggregate placement identical to AAPCS64's.
    for (auto const* cc : {t.apple, t.aapcs64}) {
        StackArgCursor small{*cc, kSlot};
        EXPECT_EQ(small.placeNamedAggregate(3), 0u) << cc->name;
        EXPECT_EQ(small.placeNamedScalar(4).byteOffset, 8u)
            << cc->name << ": a scalar after a 3-byte stacked aggregate sits at "
                           "+8 — the aggregate owns a whole slot";
        StackArgCursor big{*cc, kSlot};
        EXPECT_EQ(big.placeNamedAggregate(12), 0u) << cc->name;
        EXPECT_EQ(big.placeNamedScalar(4).byteOffset, 16u)
            << cc->name << ": a 12-byte aggregate rounds to TWO slots";
    }
}

TEST(AppleStackArgPacking, AnUnpackedConventionIsByteIdenticalToTheOldSlotRule) {
    // The byte-identity claim, stated as a property rather than measured per
    // target: for EVERY shipped CC that declares nothing, the cursor reproduces
    // `index * slot` exactly, whatever the sizes handed to it. This is what makes
    // the ELF/PE/Mach-O-x86_64 legs unchanged BY CONSTRUCTION.
    std::size_t exercised = 0;
    for (char const* targetName : {"x86_64", "arm64"}) {
        auto loaded = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(loaded.has_value()) << targetName;
        for (std::uint16_t i = 0;; ++i) {
            auto const* cc = (*loaded)->callingConvention(i);
            if (cc == nullptr) break;
            if (cc->stackArgPacking.namedScalars != StackArgPacking::Slot) continue;
            ++exercised;
            StackArgCursor c{*cc, kSlot};
            for (std::uint32_t k = 0; k < 6; ++k) {
                auto const p = c.placeNamedScalar((k % 4 == 0) ? 1u : 1u << (k % 4));
                EXPECT_EQ(p.byteOffset, k * kSlot)
                    << targetName << '/' << cc->name
                    << ": a CC that declares no packing must keep one whole slot "
                       "per stacked argument, whatever the datum's size";
                EXPECT_EQ(p.widthFlags, 0u)
                    << targetName << '/' << cc->name
                    << ": and must keep the 64-bit access — a narrowed access is "
                       "a different instruction encoding, i.e. a byte change";
            }
        }
    }
    // ⚠ A `continue`-filtered loop passes VACUOUSLY when the filter rejects
    // everything — and "every CC that declares nothing" is exactly the shape that
    // silently becomes an empty set if the field is renamed or the shipped rows
    // change. Three shipped CCs declare nothing today (sysv_amd64, ms_x64,
    // aapcs64); demanding at least that many makes the green mean something.
    EXPECT_GE(exercised, 3u)
        << "the byte-identity property was asserted over " << exercised
        << " calling conventions — a filter that rejects everything passes this "
           "test while proving nothing";
}

// ── (B) THE ACCESS WIDTH IS PART OF THE PLACEMENT ───────────────────────────

TEST(AppleStackArgPacking, NaturalPackingNarrowsTheAccessSoNeighboursSurvive) {
    auto const t = loadArm64();
    ASSERT_NE(t.apple, nullptr);
    StackArgCursor c{*t.apple, kSlot};
    auto const c0 = c.placeNamedScalar(1);
    auto const s0 = c.placeNamedScalar(2);
    auto const i0 = c.placeNamedScalar(4);
    auto const l0 = c.placeNamedScalar(8);
    EXPECT_EQ(c0.widthFlags, kLirInstFlagWidth8);
    EXPECT_EQ(s0.widthFlags, kLirInstFlagWidth16);
    EXPECT_EQ(i0.widthFlags, kLirInstFlagWidth32);
    EXPECT_EQ(l0.widthFlags, 0u) << "an 8-byte datum keeps the 64-bit access";
    // The reason, stated as arithmetic: the short BEGINS inside the byte range an
    // 8-byte access of the char would touch.
    EXPECT_LT(s0.byteOffset, c0.byteOffset + kSlot);
    EXPECT_LT(i0.byteOffset, s0.byteOffset + kSlot);
}

// ── (C) THE `va_start` OVERFLOW BASE — PREDICTED, THEN MEASURED ─────────────

// The single `VaOverflowArgAreaAddr` leaf's payload = the byte displacement
// `va_start` applies so `ap` lands past every named param that overflowed onto
// the incoming stack.
[[nodiscard]] std::optional<std::uint32_t>
vaOverflowPayload(Mir const& m) {
    std::optional<std::uint32_t> found;
    for (std::uint32_t f = 0; f < m.moduleFuncCount(); ++f) {
        MirFuncId const fn = m.funcAt(f);
        for (std::uint32_t b = 0; b < m.funcBlockCount(fn); ++b) {
            MirBlockId const blk = m.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < m.blockInstCount(blk); ++i) {
                MirInstId const ix = m.blockInstAt(blk, i);
                if (m.instOpcode(ix) != MirOpcode::VaOverflowArgAreaAddr) continue;
                if (found.has_value()) {
                    ADD_FAILURE() << "more than one VaOverflowArgAreaAddr leaf — "
                                     "the pin would be reading an arbitrary one";
                }
                found = m.instPayload(ix);
            }
        }
    }
    return found;
}

// Eight named ints exhaust x0..x7, so everything after them is on the stack.
constexpr char const* kCharShortVariadic =
    "int pick(int a0,int a1,int a2,int a3,int a4,int a5,int a6,int a7,"
    " char c, short s, ...) {\n"
    "    va_list ap; va_start(ap, s);\n"
    "    int v = va_arg(ap, int);\n"
    "    va_end(ap);\n"
    "    return (int)c + (int)s + v;\n"
    "}\n";

constexpr char const* kIntCharIntVariadic =
    "int pick2(int a0,int a1,int a2,int a3,int a4,int a5,int a6,int a7,"
    " int i0, char c0, int i1, ...) {\n"
    "    va_list ap; va_start(ap, i1);\n"
    "    int v = va_arg(ap, int);\n"
    "    va_end(ap);\n"
    "    return i0 + (int)c0 + i1 + v;\n"
    "}\n";

TEST(AppleStackArgPacking, VaStartBaseIsThePredictedEightForCharThenShort) {
    auto const t = loadArm64();
    ASSERT_NE(t.apple, nullptr);
    auto lowered = test_support::lowerCToLir(kCharShortVariadic, t.schema,
                                             t.appleIndex);
    ASSERT_EQ(lowered.mirReporter.errorCount(), 0u);
    auto const payload = vaOverflowPayload(lowered.mir.mir);
    ASSERT_TRUE(payload.has_value()) << "no va_start overflow leaf was lowered";
    EXPECT_EQ(*payload, 8u)
        << "PREDICTED before it was measured, then confirmed on real Apple "
           "Silicon: the char sits at +0 (1 byte) and the short at +2 (2 bytes), "
           "leaving the cursor at 4, and the first VARARG starts at the next whole "
           "slot — +8. Slot packing gives 16, which is what DSS emitted and what "
           "made every va_arg here read the wrong object.";
}

TEST(AppleStackArgPacking, VaStartBaseIsThePredictedSixteenForIntCharInt) {
    auto const t = loadArm64();
    ASSERT_NE(t.apple, nullptr);
    auto lowered = test_support::lowerCToLir(kIntCharIntVariadic, t.schema,
                                             t.appleIndex);
    ASSERT_EQ(lowered.mirReporter.errorCount(), 0u);
    auto const payload = vaOverflowPayload(lowered.mir.mir);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(*payload, 16u)
        << "int@0 (4), char@4 (1), int@8 (4) leaves the cursor at 12, rounded to "
           "16. Slot packing gives 24.";
}

TEST(AppleStackArgPacking, TheAapcs64VaStartBaseKeepsTheSlotAnswer) {
    // The CONTROL, and it is the ELF leg's guarantee: the same two sources under
    // AAPCS64 must still get 16 and 24. If a change to the packing rule moved
    // these, the arm64 ELF corpus would have moved with them.
    auto const t = loadArm64();
    ASSERT_NE(t.aapcs64, nullptr);
    auto a = test_support::lowerCToLir(kCharShortVariadic, t.schema,
                                       t.aapcs64Index);
    ASSERT_EQ(a.mirReporter.errorCount(), 0u);
    auto const pa = vaOverflowPayload(a.mir.mir);
    ASSERT_TRUE(pa.has_value());
    EXPECT_EQ(*pa, 16u) << "two stacked named scalars, one 8-byte slot each";

    auto b = test_support::lowerCToLir(kIntCharIntVariadic, t.schema,
                                       t.aapcs64Index);
    ASSERT_EQ(b.mirReporter.errorCount(), 0u);
    auto const pb = vaOverflowPayload(b.mir.mir);
    ASSERT_TRUE(pb.has_value());
    EXPECT_EQ(*pb, 24u) << "three stacked named scalars";
}

// ── (D) END TO END, THROUGH THE REAL PASSES ─────────────────────────────────

struct FrameMemOp {
    std::string   mnemonic;
    std::int32_t  offset;
    std::uint8_t  widthFlags;
};

// Every load/store the emitted LIR carries, with its MemOffset and width. The
// callee's incoming-argument reads are the ones at `totalFrameSize + k`.
[[nodiscard]] std::vector<FrameMemOp>
collectFrameMemOps(Lir const& lir, TargetSchema const& schema) {
    std::vector<FrameMemOp> out;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        LirFuncId const fn = lir.funcAt(f);
        for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
            LirBlockId const blk = lir.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                LirInstId const inst = lir.blockInstAt(blk, i);
                auto const* info = schema.opcodeInfo(lir.instOpcode(inst));
                if (info == nullptr) continue;
                std::optional<std::int32_t> off;
                for (auto const& o : lir.instOperands(inst)) {
                    if (o.kind == LirOperandKind::MemOffset) off = o.offset;
                }
                if (!off.has_value()) continue;
                out.push_back(FrameMemOp{std::string{info->mnemonic}, *off,
                                         lir.instFlags(inst)});
            }
        }
    }
    return out;
}

struct Pipeline {
    test_support::LoweredLir     lowered;
    LirLiveness                  liveness;
    LirAllocation                alloc;
    LirRewriteResult             rewritten;
    LirCallconvResult            cc;
    DiagnosticReporter           reporter;

    explicit Pipeline(test_support::LoweredLir l) : lowered(std::move(l)) {}
};

[[nodiscard]] Pipeline runPipeline(std::string src,
                                   std::shared_ptr<TargetSchema> schema,
                                   std::uint16_t ccIndex) {
    Pipeline p{test_support::lowerCToLir(std::move(src), std::move(schema),
                                         ccIndex)};
    if (!p.lowered.lir.ok) {
        ADD_FAILURE() << "MIR->LIR lowering failed";
        return p;
    }
    p.liveness = analyzeLiveness(p.lowered.lir.lir);
    p.alloc = allocateRegisters(p.lowered.lir.lir, *p.lowered.target,
                                p.liveness, ccIndex, p.reporter);
    if (!p.alloc.ok()) {
        ADD_FAILURE() << "allocateRegisters failed";
        return p;
    }
    p.rewritten = rewriteWithAllocation(p.lowered.lir.lir, *p.lowered.target,
                                        p.alloc, p.reporter);
    if (!p.rewritten.ok) {
        ADD_FAILURE() << "rewriteWithAllocation failed";
        return p;
    }
    p.cc = materializeCallingConvention(p.rewritten.lir, *p.lowered.target,
                                        p.alloc, p.reporter);
    return p;
}

// A NON-variadic callee whose 9th..11th named params are stacked scalars of
// three different sizes. The callee reads each one from the incoming area.
constexpr char const* kStackedNamedScalars =
    "int sink(int a0,int a1,int a2,int a3,int a4,int a5,int a6,int a7,"
    " char c, short s, int i) {\n"
    "    return (int)c + (int)s + i;\n"
    "}\n";

TEST(AppleStackArgPacking, CalleeReadsStackedNamedScalarsAtThePackedOffsets) {
    auto const t = loadArm64();
    ASSERT_NE(t.apple, nullptr);
    auto p = runPipeline(kStackedNamedScalars, t.schema, t.appleIndex);
    ASSERT_TRUE(p.cc.ok());
    ASSERT_EQ(p.reporter.errorCount(), 0u);
    auto const* layout = p.cc.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    auto const base = static_cast<std::int32_t>(layout->totalFrameSize);
    auto const ops  = collectFrameMemOps(p.cc.lir, *t.schema);

    auto has = [&](std::int32_t off, std::uint8_t w) {
        for (auto const& o : ops)
            if (o.offset == off && o.widthFlags == w) return true;
        return false;
    };
    // ✔MEASURED shape: char@+0 (1 byte), short@+2 (2 bytes), int@+4 (4 bytes).
    EXPECT_TRUE(has(base + 0, kLirInstFlagWidth8))
        << "the stacked `char` is read as ONE byte at the incoming base";
    EXPECT_TRUE(has(base + 2, kLirInstFlagWidth16))
        << "the stacked `short` is at +2 — an 8-byte read at +0 would have "
           "consumed it, and an 8-byte read AT +2 would consume the int";
    EXPECT_TRUE(has(base + 4, kLirInstFlagWidth32))
        << "the stacked `int` is at +4";
    // And the slot-packed offsets must NOT appear: their presence is the defect.
    EXPECT_FALSE(has(base + 8, 0)) << "+8 is the SLOT-packed position of the "
                                      "second stacked scalar — it must be gone";
    EXPECT_FALSE(has(base + 16, 0)) << "+16 is the slot-packed third position";
}

TEST(AppleStackArgPacking, TheAapcs64CalleeKeepsTheEightByteSlotReads) {
    auto const t = loadArm64();
    ASSERT_NE(t.aapcs64, nullptr);
    auto p = runPipeline(kStackedNamedScalars, t.schema, t.aapcs64Index);
    ASSERT_TRUE(p.cc.ok());
    ASSERT_EQ(p.reporter.errorCount(), 0u);
    auto const* layout = p.cc.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    auto const base = static_cast<std::int32_t>(layout->totalFrameSize);
    auto const ops  = collectFrameMemOps(p.cc.lir, *t.schema);
    auto has = [&](std::int32_t off, std::uint8_t w) {
        for (auto const& o : ops)
            if (o.offset == off && o.widthFlags == w) return true;
        return false;
    };
    // The ELF leg: gcc puts them at +0/+8/+16 with full-slot reads, and DSS
    // already matched that. It must not move.
    EXPECT_TRUE(has(base + 0, 0));
    EXPECT_TRUE(has(base + 8, 0));
    EXPECT_TRUE(has(base + 16, 0));
    EXPECT_FALSE(has(base + 2, kLirInstFlagWidth16))
        << "AAPCS64 must NOT pack naturally — this is the leg that stays "
           "byte-identical to gcc";
}

}  // namespace
