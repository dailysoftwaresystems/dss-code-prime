// ── D-LIR-OUTGOING-ARG-CURSOR-SPLIT-BETWEEN-TWO-PASSES-COLLIDES ──────────────
// ────────────────────────────────────────────────────────────────────────────
//
// THE DEFECT THIS FILE PINS, and it was a LIVE caller-side silent miscompile on
// both shipped pipelines. Two passes laid out ONE call's outgoing-argument area.
// `lowerWideCallArgs` (pre-regalloc) placed each stacked SCALAR and REMOVED it
// from the Call into a `store_outgoing_arg` carrier; `lir_callconv`
// (post-regalloc) then placed each stacked by-value AGGREGATE from a cursor of
// its own — which necessarily restarted at 0, because the scalars it would have
// advanced past were no longer in the operand list. One stacked scalar followed
// by one stacked aggregate was enough for the two to hand out the SAME bytes:
//
//     stur x20, [sp]        <- the scalar
//     ldur x0,  [x19]
//     stur x0,  [sp]        <- the aggregate's first eightbyte, SAME BYTES
//
// with no diagnostic. The route in is a function POINTER, because HIR->MIR
// refuses the CALLEE of this shape (a named residual of
// D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS) — so a direct call cannot
// reach it and the defect survived every corpus arm.
//
// ★★★ WHAT THE REPAIR IS, AND THEREFORE WHAT THESE ARMS ASSERT. ONE pass owns
// every outgoing byte offset: `lowerWideCallArgs`, which is the last tier holding
// the call's COMPLETE argument list. It states each aggregate's offset on the
// carrier (the `Reg, ByValueStackAgg, MemOffset` triple) and stamps
// `kLirInstFlagOutgoingArgsPlaced` on the Call; callconv READS those offsets and
// REFUSES to place anything of its own.
//
//   (A) THE NEGATIVE MISCOMPILE PIN — the real MIR->LIR -> lowerWideCallArgs ->
//       regalloc -> rewrite -> 2addr -> callconv pipeline over the colliding
//       source, asserting that NO outgoing-argument byte is written twice and
//       that every stacked argument lands on the offset its ABI names. This is
//       the arm that goes red the moment the transform mis-fires: under the
//       two-cursor scheme +0 is written by BOTH the scalar and the aggregate.
//   (B) THE STATEMENT ITSELF — the carrier states an offset and the Call carries
//       the placed bit, both read back off the module `lowerWideCallArgs`
//       produced.
//   (C) THE REFUSALS, EXERCISED RATHER THAN READ — a placed Call whose carrier
//       states nothing, and a placed Call with an overflow scalar still on it,
//       are both refused; the SAME modules with the bit clear materialize
//       cleanly, which is what shows the refusal keys on the placement authority
//       and not on the shape.
//   (D) THE VARARG BOUNDARY IS A POSITION, AND THIS PASS RENUMBERS POSITIONS —
//       `fixedOperandCount` counts arg positions, so removing operands moves the
//       boundary. Leaving it unrestated made the SysV variadic vector count (AL)
//       read a vararg as a named argument and emit 0, which is the fpconv1 class
//       of miscompile: the callee's al-gated prologue then never spills its
//       vector arg registers and `va_arg(double)` reads an unwritten slot.
//
// ⚠ THE EXPECTED OFFSETS ARE TYPED OUT, NOT DERIVED. Computing them from the
// same cursor the code walks would move both halves of the comparison together
// and redden nothing — the P23/P25 lesson this directory already records.

#include "core/types/call_payload.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_wide_call_args.hpp"
#include "mir/mir_opcode.hpp"

#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

// ── THE SOURCE ──────────────────────────────────────────────────────────────
//
// Nine `long` arguments and then a 16-byte by-value aggregate, called through a
// function POINTER. Under sysv_amd64 six arguments are register-passed and the
// remaining three scalars stack at +0/+8/+16 with the aggregate at +24/+32;
// under aapcs64 eight are register-passed, one scalar stacks at +0 and the
// aggregate follows at +8/+16. Either way a stacked SCALAR precedes a stacked
// AGGREGATE, which is the whole shape.
//
// `long` on every position deliberately: it is 8 bytes, so Apple's natural
// packing and AAPCS64's slot packing agree and this file's expectations are
// about the CURSOR SPLIT and nothing else.
constexpr char const* kScalarThenAggregate =
    "struct S16 { long x; long y; };\n"
    "typedef void (*Fp)(long,long,long,long,long,long,long,long,long,"
    "struct S16);\n"
    "Fp g_fp;\n"
    "long g_v;\n"
    "void caller(void) {\n"
    "    struct S16 s;\n"
    "    s.x = g_v;\n"
    "    s.y = g_v;\n"
    "    g_fp(g_v,g_v,g_v,g_v,g_v,g_v,g_v,g_v,g_v, s);\n"
    "}\n";

// A SysV variadic callee whose SEVENTH fixed argument overflows onto the stack,
// followed by one floating-point vararg that still finds a free vector register.
// The fixed argument is removed from the Call by `lowerWideCallArgs`, so every
// later position shifts down by one — and the vararg boundary shifts with it.
//
// ⚠ The callee is DEFINED here rather than declared `extern`, because this
// directory's fixture threads no FFI map and an extern symbol without a mangled
// name is refused at HIR->MIR. It needs no `va_start`: the vector count is a
// CALLER-side fact, decided entirely by which arguments the caller routes into
// vector argument registers.
constexpr char const* kVariadicWithOverflowingFixedArg =
    "double g_d;\n"
    "long g_l;\n"
    "long g_r;\n"
    "long vsum(long a1,long a2,long a3,long a4,long a5,long a6,long a7,...) {\n"
    "    return a1+a2+a3+a4+a5+a6+a7;\n"
    "}\n"
    "void caller(void) {\n"
    "    g_r = vsum(g_l,g_l,g_l,g_l,g_l,g_l,g_l, g_d);\n"
    "}\n";

struct Pipeline {
    test_support::LoweredLir lowered;
    LirWideCallResult        wide;
    LirLiveness              liveness;
    LirAllocation            alloc;
    LirRewriteResult         rewritten;
    LirTwoAddrLegalizeResult legal;
    LirCallconvResult        cc;
    DiagnosticReporter       reporter;

    explicit Pipeline(test_support::LoweredLir l) : lowered(std::move(l)) {}
};

// The REAL sequence `compile_pipeline.cpp` runs, `lowerWideCallArgs` included —
// which is the point: the defect only exists between that pass and callconv, so
// a fixture that skips it cannot see this row at all.
[[nodiscard]] Pipeline runPipeline(std::string src,
                                   std::shared_ptr<TargetSchema> schema,
                                   std::uint16_t ccIndex) {
    Pipeline p{test_support::lowerCToLir(std::move(src), schema, ccIndex)};
    if (!p.lowered.lir.ok) {
        ADD_FAILURE() << "MIR->LIR lowering failed";
        return p;
    }
    p.wide = lowerWideCallArgs(p.lowered.lir.lir, *schema, ccIndex, p.reporter);
    if (!p.wide.ok) {
        ADD_FAILURE() << "lowerWideCallArgs failed: "
                      << (p.reporter.all().empty() ? std::string{}
                                                   : p.reporter.all()[0].actual);
        return p;
    }
    p.liveness = analyzeLiveness(p.wide.lir);
    p.alloc = allocateRegisters(p.wide.lir, *schema, p.liveness, ccIndex,
                                p.reporter);
    if (!p.alloc.ok()) {
        ADD_FAILURE() << "allocateRegisters failed";
        return p;
    }
    p.rewritten = rewriteWithAllocation(p.wide.lir, *schema, p.alloc, p.reporter);
    if (!p.rewritten.ok) {
        ADD_FAILURE() << "rewriteWithAllocation failed";
        return p;
    }
    p.legal = legalizeTwoAddress(p.rewritten.lir, *schema, p.reporter);
    if (!p.legal.allFunctionsLegalized) {
        ADD_FAILURE() << "legalizeTwoAddress failed";
        return p;
    }
    p.cc = materializeCallingConvention(p.legal.lir, *schema, p.alloc,
                                        p.reporter);
    return p;
}

// Every SP-relative STORE that lands inside the outgoing-argument area, keyed by
// byte offset and counted. A store's operand list is
// `[valueReg, baseReg, MemBase, MemOffset]`; the saved-register, spill, local
// and by-value-temp writes all sit at or above `outgoingArgAreaSize`, so the
// bound alone separates the outgoing writes from every other frame write.
//
// ★ THE COUNT IS THE POINT. The defect does not move an argument to a wrong
// offset that some other argument vacated — it writes TWO different values to
// the SAME offset and leaves the last one standing. A map from offset to WRITE
// COUNT states that directly.
[[nodiscard]] std::map<std::int32_t, int>
outgoingStoreCounts(Lir const& lir, std::uint32_t funcIndex,
                    std::uint16_t spOrdinal, std::uint32_t outgoingBytes) {
    std::map<std::int32_t, int> out;
    LirFuncId const fn = lir.funcAt(funcIndex);
    for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
        LirBlockId const blk = lir.funcBlockAt(fn, b);
        for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
            auto const ops = lir.instOperands(lir.blockInstAt(blk, i));
            if (ops.size() < 4) continue;
            if (ops[0].kind != LirOperandKind::Reg) continue;
            if (ops[1].kind != LirOperandKind::Reg) continue;
            if (ops[1].reg.isPhysical == 0 || ops[1].reg.id != spOrdinal) continue;
            if (ops[2].kind != LirOperandKind::MemBase) continue;
            if (ops[3].kind != LirOperandKind::MemOffset) continue;
            if (ops[3].offset < 0
                || static_cast<std::uint32_t>(ops[3].offset) >= outgoingBytes)
                continue;
            ++out[ops[3].offset];
        }
    }
    return out;
}

// The one Call instruction in the module, as (flags, payload, operands).
struct CallView {
    bool                    found = false;
    std::uint8_t            flags = 0;
    std::uint32_t           payload = 0;
    std::vector<LirOperand> ops;
};

[[nodiscard]] CallView findTheCall(Lir const& lir, TargetSchema const& schema) {
    CallView v;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        LirFuncId const fn = lir.funcAt(f);
        for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
            LirBlockId const blk = lir.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                LirInstId const inst = lir.blockInstAt(blk, i);
                auto const* info = schema.opcodeInfo(lir.instOpcode(inst));
                if (info == nullptr || !info->isCall) continue;
                v.found   = true;
                v.flags   = lir.instFlags(inst);
                v.payload = lir.instPayload(inst);
                auto const ops = lir.instOperands(inst);
                v.ops.assign(ops.begin(), ops.end());
                return v;
            }
        }
    }
    return v;
}

// Every `store_outgoing_arg` payload (its byte offset) in the module, in order.
[[nodiscard]] std::vector<std::uint32_t>
storeOutgoingPayloads(Lir const& lir, TargetSchema const& schema) {
    std::vector<std::uint32_t> out;
    auto const op = schema.opcodeByMnemonic("store_outgoing_arg");
    if (!op.has_value()) return out;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        LirFuncId const fn = lir.funcAt(f);
        for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
            LirBlockId const blk = lir.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                LirInstId const inst = lir.blockInstAt(blk, i);
                if (lir.instOpcode(inst) == *op)
                    out.push_back(lir.instPayload(inst));
            }
        }
    }
    return out;
}

[[nodiscard]] std::shared_ptr<TargetSchema> loadTarget(char const* name) {
    auto loaded = TargetSchema::loadShipped(name);
    if (!loaded) {
        ADD_FAILURE() << "TargetSchema::loadShipped(" << name << ") failed";
        return nullptr;
    }
    return *loaded;
}

[[nodiscard]] std::optional<std::uint16_t>
ccIndexByName(TargetSchema const& schema, std::string_view name) {
    for (std::uint16_t i = 0;; ++i) {
        auto const* cc = schema.callingConvention(i);
        if (cc == nullptr) return std::nullopt;
        if (cc->name == name) return i;
    }
}

// ── (A) THE NEGATIVE MISCOMPILE PIN ─────────────────────────────────────────

void expectDisjointOutgoingPlacement(char const* targetName,
                                     char const* ccName,
                                     std::vector<std::int32_t> expected,
                                     std::uint32_t expectedAreaBytes) {
    auto schema = loadTarget(targetName);
    ASSERT_NE(schema, nullptr);
    auto const ccIdx = ccIndexByName(*schema, ccName);
    ASSERT_TRUE(ccIdx.has_value()) << ccName << " is not a declared cc";
    auto const* cc = schema->callingConvention(*ccIdx);
    ASSERT_NE(cc, nullptr);
    ASSERT_TRUE(cc->stackPointer.has_value());

    auto p = runPipeline(kScalarThenAggregate, schema, *ccIdx);
    ASSERT_TRUE(p.cc.ok());
    ASSERT_EQ(p.reporter.errorCount(), 0u)
        << "the colliding shape must COMPILE — refusing it is what this row "
           "replaces: " << (p.reporter.all().empty()
                                ? std::string{}
                                : p.reporter.all()[0].actual);

    auto const* layout = p.cc.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    EXPECT_EQ(layout->outgoingArgAreaSize, expectedAreaBytes)
        << "the reserved outgoing area must cover every stacked argument — a "
           "reservation taken from a cursor that restarted would be short";

    auto const counts = outgoingStoreCounts(p.cc.lir, 0,
                                            cc->stackPointer->ordinal,
                                            layout->outgoingArgAreaSize);

    // ★ THE LOAD-BEARING ASSERTION: no outgoing-argument byte is written twice.
    for (auto const& [offset, n] : counts) {
        EXPECT_EQ(n, 1) << ccName << ": outgoing-argument offset +" << offset
                        << " is written " << n << " times. Two writes to one "
                           "offset is this row's signature — the stacked scalar "
                           "and the aggregate's eightbyte were placed by cursors "
                           "that could not see each other";
    }
    std::vector<std::int32_t> got;
    got.reserve(counts.size());
    for (auto const& [offset, n] : counts) { (void)n; got.push_back(offset); }
    EXPECT_EQ(got, expected)
        << ccName << ": the stacked arguments must occupy exactly the offsets "
                     "this ABI names, in order";
}

TEST(OutgoingArgCursor, SysVStackedScalarsAndAggregateDoNotShareBytes) {
    // sysv_amd64: 6 GPR arg registers, so a7/a8/a9 stack at +0/+8/+16 and the
    // 16-byte aggregate follows at +24/+32. 40 bytes reserved.
    expectDisjointOutgoingPlacement("x86_64", "sysv_amd64",
                                    {0, 8, 16, 24, 32}, 40u);
}

TEST(OutgoingArgCursor, Aapcs64StackedScalarAndAggregateDoNotShareBytes) {
    // aapcs64: 8 GPR arg registers, so only a9 stacks (+0) and the aggregate
    // follows at +8/+16. 24 bytes reserved. Under the two-cursor scheme the
    // aggregate started at +0 and destroyed a9.
    expectDisjointOutgoingPlacement("arm64", "aapcs64", {0, 8, 16}, 24u);
}

TEST(OutgoingArgCursor, AppleArm64StackedScalarAndAggregateDoNotShareBytes) {
    // The Apple convention packs named scalars NATURALLY; every argument here is
    // 8 bytes wide, so its offsets coincide with AAPCS64's — which is deliberate,
    // because it isolates THIS row from the packing row that shares the cursor.
    expectDisjointOutgoingPlacement("arm64", "apple_arm64", {0, 8, 16}, 24u);
}

// ── (B) THE STATEMENT ITSELF ────────────────────────────────────────────────

TEST(OutgoingArgCursor, LowerWideCallArgsStatesTheAggregatePlacementOnTheCarrier) {
    auto schema = loadTarget("x86_64");
    ASSERT_NE(schema, nullptr);
    auto const ccIdx = ccIndexByName(*schema, "sysv_amd64");
    ASSERT_TRUE(ccIdx.has_value());

    auto p = runPipeline(kScalarThenAggregate, schema, *ccIdx);
    ASSERT_EQ(p.reporter.errorCount(), 0u);

    // The three stacked scalars became carriers at +0/+8/+16 …
    EXPECT_EQ(storeOutgoingPayloads(p.wide.lir, *schema),
              (std::vector<std::uint32_t>{0u, 8u, 16u}));

    auto const call = findTheCall(p.wide.lir, *schema);
    ASSERT_TRUE(call.found);
    EXPECT_TRUE(lirCallOutgoingArgsArePlaced(call.flags))
        << "the shrunken Call must SAY its outgoing arguments are placed — that "
           "bit is how callconv knows its own cursor must place nothing";

    // … and the aggregate carrier states +24, which is where the scalars left
    // the cursor. A carrier stating 0 is the defect.
    std::span<LirOperand const> const ops{call.ops};
    bool sawCarrier = false;
    for (std::size_t k = 0; k < ops.size(); ++k) {
        if (!lirIsByValueStackAggCarrier(ops, k)) continue;
        sawCarrier = true;
        auto const placed = lirByValueStackAggPlacedOffset(ops, k);
        ASSERT_TRUE(placed.has_value())
            << "the by-value stacked aggregate carrier states no placement";
        EXPECT_EQ(*placed, 24)
            << "the aggregate begins where the three stacked scalars left the "
               "cursor (+24); +0 is the collision this row records";
        EXPECT_EQ(ops[k + 1].byValueAggBytes, 16u);
    }
    EXPECT_TRUE(sawCarrier)
        << "the call must still carry the by-value aggregate — it is not a "
           "register operand at pressure, so only its PLACEMENT moved";
}

// ── (C) THE REFUSALS, EXERCISED ─────────────────────────────────────────────
//
// A post-regalloc module built by hand, so the two states a pipeline cannot
// produce can be reached at all: a Call that CLAIMS its outgoing arguments were
// placed while a carrier states nothing, and one that claims it while an
// argument still overflows. Each is run with the claim and WITHOUT it, and the
// difference is the whole assertion.

struct HandBuilt {
    Lir           lir;
    LirAllocation alloc;
};

// `argCount` GPR arguments followed by an optional by-value aggregate carrier.
[[nodiscard]] HandBuilt
buildCall(TargetSchema const& sch, std::uint32_t gprArgs, bool withAggregate,
          bool statePlacement, bool stampPlacedFlag) {
    HandBuilt out;
    auto const callOp = sch.opcodeByMnemonic("call");
    auto const retOp  = sch.opcodeByMnemonic("ret");
    EXPECT_TRUE(callOp.has_value());
    EXPECT_TRUE(retOp.has_value());

    auto const* cc = sch.callingConvention(0);
    EXPECT_NE(cc, nullptr);

    // Sources drawn from the callee-saved pool so no arg-move cycle arises.
    std::array<char const*, 5> const homes{"rbx", "rbp", "r12", "r13", "r14"};
    std::vector<LirOperand> ops;
    ops.push_back(LirOperand::makeSymbolRef(13));
    for (std::uint32_t i = 0; i < gprArgs; ++i) {
        auto const ord = sch.registerByName(homes[i % homes.size()]);
        EXPECT_TRUE(ord.has_value());
        ops.push_back(LirOperand::makeReg(
            makePhysicalReg(*ord, LirRegClass::GPR)));
    }
    if (withAggregate) {
        auto const ord = sch.registerByName("r15");
        EXPECT_TRUE(ord.has_value());
        ops.push_back(LirOperand::makeReg(
            makePhysicalReg(*ord, LirRegClass::GPR)));
        ops.push_back(LirOperand::makeByValueStackAgg(16));
        if (statePlacement) ops.push_back(LirOperand::makeMemOffset(0));
    }

    LirBuilder b{sch};
    b.addFunction(SymbolId{91});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    b.addInst(*callOp, InvalidLirReg, ops, /*payload=*/0u,
              stampPlacedFlag ? kLirInstFlagOutgoingArgsPlaced
                              : std::uint8_t{0});
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    out.lir = std::move(b).finish();

    out.alloc.perFunc.emplace_back();
    out.alloc.perFunc.back().ok                     = true;
    out.alloc.perFunc.back().originalSymbol         = SymbolId{91};
    out.alloc.perFunc.back().callingConventionIndex = 0;
    out.alloc.perFunc.back().numSpillSlots          = 0;
    return out;
}

TEST(OutgoingArgCursor, CallconvRefusesAnUnplacedCarrierOnAPlacedCall) {
    auto schema = loadTarget("x86_64");
    ASSERT_NE(schema, nullptr);

    // THE FAILURE ARM: the Call says its outgoing arguments were placed, and the
    // carrier states nothing. Placing it here would use a cursor that never saw
    // the scalars `lowerWideCallArgs` removed.
    auto bad = buildCall(*schema, /*gprArgs=*/2, /*withAggregate=*/true,
                         /*statePlacement=*/false, /*stampPlacedFlag=*/true);
    DiagnosticReporter badRep;
    auto const badResult =
        materializeCallingConvention(bad.lir, *schema, bad.alloc, badRep);
    EXPECT_FALSE(badResult.ok());
    EXPECT_GT(badRep.errorCount(), 0u)
        << "an unplaced carrier on a placed call must be REFUSED, not re-placed";

    // THE CONTROL: byte-identical module with the claim removed. A module that
    // never went through `lowerWideCallArgs` has exactly one cursor — callconv's
    // — and must still materialize.
    auto good = buildCall(*schema, /*gprArgs=*/2, /*withAggregate=*/true,
                          /*statePlacement=*/false, /*stampPlacedFlag=*/false);
    DiagnosticReporter goodRep;
    auto const goodResult =
        materializeCallingConvention(good.lir, *schema, good.alloc, goodRep);
    EXPECT_TRUE(goodResult.ok())
        << "with no placement claim callconv owns the only cursor and must "
           "still place the aggregate: "
        << (goodRep.all().empty() ? std::string{} : goodRep.all()[0].actual);
    EXPECT_EQ(goodRep.errorCount(), 0u);
}

TEST(OutgoingArgCursor, CallconvRefusesAnOverflowScalarOnAPlacedCall) {
    auto schema = loadTarget("x86_64");
    ASSERT_NE(schema, nullptr);
    auto const* cc = schema->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_EQ(cc->name, "sysv_amd64");
    std::uint32_t const overflowing =
        static_cast<std::uint32_t>(cc->argGprs.size()) + 1u;

    // THE FAILURE ARM: one argument past the GPR pool, on a Call that claims its
    // outgoing arguments were already placed. `lowerWideCallArgs` would have
    // removed it; that it is still here means the two arg walks disagree.
    auto bad = buildCall(*schema, overflowing, /*withAggregate=*/false,
                         /*statePlacement=*/false, /*stampPlacedFlag=*/true);
    DiagnosticReporter badRep;
    auto const badResult =
        materializeCallingConvention(bad.lir, *schema, bad.alloc, badRep);
    EXPECT_FALSE(badResult.ok());
    EXPECT_GT(badRep.errorCount(), 0u)
        << "a stacked scalar on a placed call must be REFUSED — placing it from "
           "a restarted cursor is how the two passes overlap";

    // THE CONTROL: the same overflow with no placement claim still materializes.
    auto good = buildCall(*schema, overflowing, /*withAggregate=*/false,
                          /*statePlacement=*/false, /*stampPlacedFlag=*/false);
    DiagnosticReporter goodRep;
    auto const goodResult =
        materializeCallingConvention(good.lir, *schema, good.alloc, goodRep);
    EXPECT_TRUE(goodResult.ok())
        << (goodRep.all().empty() ? std::string{} : goodRep.all()[0].actual);
    EXPECT_EQ(goodRep.errorCount(), 0u);
}

// ── (D) THE VARARG BOUNDARY IS A POSITION ───────────────────────────────────

TEST(OutgoingArgCursor, RemovingAFixedArgRenumbersTheVarargBoundary) {
    auto schema = loadTarget("x86_64");
    ASSERT_NE(schema, nullptr);
    auto const ccIdx = ccIndexByName(*schema, "sysv_amd64");
    ASSERT_TRUE(ccIdx.has_value());

    auto p = runPipeline(kVariadicWithOverflowingFixedArg, schema, *ccIdx);
    ASSERT_TRUE(p.cc.ok());
    ASSERT_EQ(p.reporter.errorCount(), 0u);

    auto const call = findTheCall(p.wide.lir, *schema);
    ASSERT_TRUE(call.found);
    EXPECT_TRUE(call_payload::isVariadic(call.payload));
    // Seven fixed arguments; the seventh overflowed and left the operand list,
    // so six fixed positions remain and the boundary must say six. Leaving it at
    // seven makes the FP vararg (new position 6) test `6 >= 7` and read as a
    // NAMED argument.
    EXPECT_EQ(call_payload::fixedOperandCount(call.payload), 6u)
        << "the vararg boundary counts POSITIONS, and this pass renumbered them";

    // END TO END: the SysV variadic vector count. The one FP vararg landed in a
    // vector arg register, so the count register must be set to 1. Zero is the
    // fpconv1 class of miscompile — the callee's al-gated prologue skips saving
    // its vector arg registers and `va_arg(double)` reads an unwritten slot.
    ASSERT_TRUE(p.lowered.target->callingConvention(*ccIdx)
                    ->variadicVectorCountReg.has_value());
    std::uint16_t const countOrd = p.lowered.target->callingConvention(*ccIdx)
                                       ->variadicVectorCountReg->ordinal;
    // Taken from the last count-register IMMEDIATE that precedes a call, rather
    // than from any immediate into that register — SysV's count register is also
    // its return register, so "an immediate into rax" answers a different
    // question (a function returning a constant writes one too).
    std::optional<std::int32_t> countImm;
    for (std::uint32_t f = 0; f < p.cc.lir.moduleFuncCount(); ++f) {
        LirFuncId const fn = p.cc.lir.funcAt(f);
        for (std::uint32_t b = 0; b < p.cc.lir.funcBlockCount(fn); ++b) {
            LirBlockId const blk = p.cc.lir.funcBlockAt(fn, b);
            std::optional<std::int32_t> pending;
            for (std::uint32_t i = 0; i < p.cc.lir.blockInstCount(blk); ++i) {
                LirInstId const inst = p.cc.lir.blockInstAt(blk, i);
                auto const* info = schema->opcodeInfo(p.cc.lir.instOpcode(inst));
                if (info != nullptr && info->isCall) {
                    if (pending.has_value()) countImm = pending;
                    pending.reset();
                    continue;
                }
                LirReg const res = p.cc.lir.instResult(inst);
                if (res.isPhysical == 0 || res.id != countOrd) continue;
                auto const ops = p.cc.lir.instOperands(inst);
                if (ops.size() == 1 && ops[0].kind == LirOperandKind::ImmInt)
                    pending = ops[0].immInt32;
            }
        }
    }
    ASSERT_TRUE(countImm.has_value())
        << "the variadic vector-count register is never set";
    EXPECT_EQ(*countImm, 1)
        << "one FP vararg reached a vector arg register, so the count is 1; 0 "
           "means the boundary was read against the shrunken list";
}

} // namespace
