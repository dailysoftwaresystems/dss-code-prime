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
// the call's COMPLETE argument list. It stamps `kLirInstFlagOutgoingArgsPlaced`
// on the Call, and callconv REFUSES to place anything of its own. Since P69
// round 4 (D-AS-REGALLOC-WIDE-CALL-AGGREGATE-ADDRESS-OPERANDS) the pass also
// WRITES each stacked aggregate's bytes at its offset, before the Call, so the
// Call the pipeline hands callconv carries no aggregate at all; a carrier that
// states its offset (the `Reg, ByValueStackAgg, MemOffset` triple) now reaches
// callconv only from a hand-built module, which (C) builds.
//
//   (A) THE NEGATIVE MISCOMPILE PIN — the real MIR->LIR -> lowerWideCallArgs ->
//       regalloc -> rewrite -> 2addr -> callconv pipeline over the colliding
//       source, asserting that NO outgoing-argument byte is written twice and
//       that every stacked argument lands on the offset its ABI names. This is
//       the arm that goes red the moment the transform mis-fires: under the
//       two-cursor scheme +0 is written by BOTH the scalar and the aggregate.
//   (B) THE STATEMENT ITSELF — the aggregate's eightbytes are `store_outgoing_arg`
//       carriers at the offsets the cursor gave it, each loaded from its temp,
//       and the Call carries the placed bit and no aggregate — all read back off
//       the module `lowerWideCallArgs` produced.
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
//   (E) A STACKED AGGREGATE'S ADDRESS IS NOT A CALL OPERAND — calls passing
//       eleven and fourteen x87 long doubles (each a SysV MEMORY-class aggregate)
//       compile, every outgoing byte written once, which they did not while each
//       one's address rode the Call into the allocator.
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
                                   std::uint16_t ccIndex,
                                   LongDoubleFormat longDoubleFormat =
                                       LongDoubleFormat::None) {
    Pipeline p{test_support::lowerCToLir(std::move(src), schema, ccIndex,
                                         test_support::LoweringExpectation::Lowers,
                                         longDoubleFormat)};
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
    std::uint32_t           funcIndex = 0;   // the function that makes it
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
                v.funcIndex = f;
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

// What each `store_outgoing_arg` of function `funcIndex` stores, keyed by its
// payload (= its byte offset): when the stored register is the result of a
// general-register `load`, that load's base register id and displacement.
//
// `defCount` is how many instructions of the function DEFINE the stored register.
// The copy mints one fresh register per chunk, so each is defined exactly once;
// a count of two means the fresh register took an id the copied function already
// used — two values in one register, which the allocator would then merge.
struct StoredFrom {
    bool          fromGprLoad = false;
    std::uint32_t baseVReg    = 0;   // the load's base (a virtual register id)
    std::int32_t  disp        = 0;   // the load's MemOffset
    std::uint32_t defCount    = 0;   // definitions of the stored register
};

[[nodiscard]] std::map<std::uint32_t, StoredFrom>
outgoingStoreSources(Lir const& lir, TargetSchema const& schema,
                     std::uint32_t funcIndex) {
    std::map<std::uint32_t, StoredFrom> out;
    auto const storeOp = schema.opcodeByMnemonic("store_outgoing_arg");
    auto const loadOp =
        schema.regClassOpOpcode(TargetRegClass::GPR, RegClassOp::Load);
    if (!storeOp.has_value() || !loadOp.has_value()) {
        ADD_FAILURE() << "the schema declares no store_outgoing_arg / gpr load";
        return out;
    }
    LirFuncId const fn = lir.funcAt(funcIndex);
    // Every definition of every virtual register in the function, counted first:
    // a register id is one counter per function across all classes.
    std::map<std::uint32_t, std::uint32_t> defCounts;
    for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
        LirBlockId const blk = lir.funcBlockAt(fn, b);
        for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
            LirReg const res = lir.instResult(lir.blockInstAt(blk, i));
            if (res.valid() && res.isPhysical == 0) ++defCounts[res.id];
        }
    }
    std::map<std::uint32_t, StoredFrom> defs;   // virtual reg id -> its def
    for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
        LirBlockId const blk = lir.funcBlockAt(fn, b);
        for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
            LirInstId const inst = lir.blockInstAt(blk, i);
            auto const ops = lir.instOperands(inst);
            if (lir.instOpcode(inst) == *storeOp) {
                if (ops.size() != 1 || ops[0].kind != LirOperandKind::Reg) {
                    ADD_FAILURE() << "a store_outgoing_arg that stores no register";
                    continue;
                }
                auto const def = defs.find(ops[0].reg.id);
                StoredFrom from = def != defs.end() ? def->second : StoredFrom{};
                auto const count = defCounts.find(ops[0].reg.id);
                from.defCount = count != defCounts.end() ? count->second : 0u;
                out[lir.instPayload(inst)] = from;
                continue;
            }
            LirReg const res = lir.instResult(inst);
            if (!res.valid() || res.isPhysical != 0) continue;
            StoredFrom from;
            if (lir.instOpcode(inst) == *loadOp && ops.size() == 3
                && ops[0].kind == LirOperandKind::Reg
                && ops[0].reg.isPhysical == 0
                && ops[2].kind == LirOperandKind::MemOffset) {
                from.fromGprLoad = true;
                from.baseVReg    = ops[0].reg.id;
                from.disp        = ops[2].offset;
            }
            defs[res.id] = from;
        }
    }
    return out;
}

// Does any operand of the Call belong to a by-value stacked aggregate carrier?
[[nodiscard]] bool callCarriesAnAggregate(CallView const& call) {
    for (auto const& o : call.ops)
        if (o.kind == LirOperandKind::ByValueStackAgg) return true;
    return false;
}

// P69 round 4 (D-AS-REGALLOC-WIDE-CALL-AGGREGATE-ADDRESS-OPERANDS): the aggregate
// is COPIED into the bytes the one cursor placed it at, before the Call, and so
// leaves the Call. The placement this arm used to read off the carrier is now
// read off the copy's own `store_outgoing_arg` payloads.
TEST(OutgoingArgCursor, LowerWideCallArgsCopiesTheAggregateIntoItsPlacedBytes) {
    auto schema = loadTarget("x86_64");
    ASSERT_NE(schema, nullptr);
    auto const ccIdx = ccIndexByName(*schema, "sysv_amd64");
    ASSERT_TRUE(ccIdx.has_value());

    auto p = runPipeline(kScalarThenAggregate, schema, *ccIdx);
    ASSERT_EQ(p.reporter.errorCount(), 0u);

    // The three stacked scalars at +0/+8/+16, then the aggregate's two eightbytes
    // at +24/+32 — where the scalars left the cursor. An aggregate at +0 is the
    // collision this file records.
    EXPECT_EQ(storeOutgoingPayloads(p.wide.lir, *schema),
              (std::vector<std::uint32_t>{0u, 8u, 16u, 24u, 32u}));

    auto const call = findTheCall(p.wide.lir, *schema);
    ASSERT_TRUE(call.found);
    EXPECT_TRUE(lirCallOutgoingArgsArePlaced(call.flags))
        << "the shrunken Call must SAY its outgoing arguments are placed — that "
           "bit is how callconv knows its own cursor must place nothing";
    EXPECT_FALSE(callCarriesAnAggregate(call))
        << "the aggregate's bytes are copied before the Call, so its address "
           "must not ride the Call into the allocator";

    // The two eightbytes are read from ONE temp, at +0 and +8 of it.
    auto const from = outgoingStoreSources(p.wide.lir, *schema, call.funcIndex);
    ASSERT_TRUE(from.contains(24u) && from.contains(32u));
    StoredFrom const lo = from.at(24u);
    StoredFrom const hi = from.at(32u);
    ASSERT_TRUE(lo.fromGprLoad && hi.fromGprLoad)
        << "each outgoing eightbyte of the aggregate is a general-register load "
           "from its temp";
    EXPECT_EQ(lo.baseVReg, hi.baseVReg)
        << "both eightbytes come from the same temp";
    EXPECT_EQ(lo.disp, 0);
    EXPECT_EQ(hi.disp, 8);
    EXPECT_EQ(lo.defCount, 1u)
        << "the register carrying the low eightbyte is defined " << lo.defCount
        << " times: the copy's fresh register took an id the function already used";
    EXPECT_EQ(hi.defCount, 1u)
        << "the register carrying the high eightbyte is defined " << hi.defCount
        << " times: the copy's fresh register took an id the function already used";
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

// ── (E) A STACKED AGGREGATE'S ADDRESS IS NOT A CALL OPERAND ─────────────────
//
// D-AS-REGALLOC-WIDE-CALL-AGGREGATE-ADDRESS-OPERANDS (P69 round 4). A stacked
// by-value aggregate used to stay ON the Call as an (address, ByValueStackAgg)
// carrier for `lir_callconv` to copy after allocation, and its ADDRESS was then
// a register operand of the Call like any other. Under SysV every x87 `long
// double` argument is such an aggregate (MEMORY class), so a call passing many
// held one live address per long double INTO the call: ✔MEASURED with the
// round-4 dsscp, eight ints and eleven long doubles to a variadic callee
// exhausted the rewriter's reload scratch at the call
// (`L_VirtualRegInPostRegalloc`), and fourteen long doubles left callconv no
// free caller-saved register to copy through (`L_CcRegLookupFailed`). Both
// shapes run through the REAL sequence here and must compile, with every
// outgoing byte written once at the offset SysV names. ⚠ TYPED OUT, not
// derived: each long double takes a 16-byte, 16-aligned slot after the stacked
// ints, so the eight-int call's first one lands at +32, not +24.

constexpr char const* kEightIntsElevenLongDoubles =
    "int vsink(char const *fmt, ...) { return fmt != 0; }\n"
    "int caller(void) {\n"
    "    return vsink(\"f\", 1, 2, 3, 4, 5, 6, 7, 8,\n"
    "                 0.5L, 1.0L, 1.5L, 2.0L, 2.5L, 3.0L, 3.5L, 4.0L, 4.5L,\n"
    "                 5.0L, 5.5L);\n"
    "}\n";

constexpr char const* kFourteenLongDoubles =
    "int vsink(char const *fmt, ...) { return fmt != 0; }\n"
    "int caller(void) {\n"
    "    return vsink(\"f\", 0.5L, 1.0L, 1.5L, 2.0L, 2.5L, 3.0L, 3.5L, 4.0L,\n"
    "                 4.5L, 5.0L, 5.5L, 6.0L, 6.5L, 7.0L);\n"
    "}\n";

// `firstLongDouble`: the offset the first long double is placed at; each later
// one 16 bytes on. `ints`: the stacked int offsets before it.
void expectLongDoublesCopiedBeforeTheCall(char const* src,
                                          std::vector<std::int32_t> ints,
                                          std::int32_t firstLongDouble,
                                          std::int32_t longDoubles,
                                          std::uint32_t expectedAreaBytes) {
    auto schema = loadTarget("x86_64");
    ASSERT_NE(schema, nullptr);
    auto const ccIdx = ccIndexByName(*schema, "sysv_amd64");
    ASSERT_TRUE(ccIdx.has_value());
    auto const* cc = schema->callingConvention(*ccIdx);
    ASSERT_NE(cc, nullptr);
    ASSERT_TRUE(cc->stackPointer.has_value());

    auto p = runPipeline(src, schema, *ccIdx, LongDoubleFormat::X87_80);
    ASSERT_TRUE(p.cc.ok())
        << "a call passing many x87 long doubles must COMPILE — gcc and clang "
           "compile it: "
        << (p.reporter.all().empty() ? std::string{} : p.reporter.all()[0].actual);
    ASSERT_EQ(p.reporter.errorCount(), 0u);

    auto const call = findTheCall(p.wide.lir, *schema);
    ASSERT_TRUE(call.found);
    EXPECT_FALSE(callCarriesAnAggregate(call))
        << "no long double's address may ride the Call into the allocator — "
           "that is the operand count this row records exhausting the machine";

    // `caller`, the function that makes the call.
    auto const* layout = p.cc.forFuncByIndex(call.funcIndex);
    ASSERT_NE(layout, nullptr);
    EXPECT_EQ(layout->outgoingArgAreaSize, expectedAreaBytes);

    std::vector<std::int32_t> expected = ints;
    for (std::int32_t i = 0; i < longDoubles; ++i) {
        expected.push_back(firstLongDouble + 16 * i);
        expected.push_back(firstLongDouble + 16 * i + 8);
    }
    auto const counts = outgoingStoreCounts(p.cc.lir, call.funcIndex,
                                            cc->stackPointer->ordinal,
                                            layout->outgoingArgAreaSize);
    for (auto const& [offset, n] : counts) {
        EXPECT_EQ(n, 1) << "outgoing-argument offset +" << offset << " is written "
                        << n << " times";
    }
    std::vector<std::int32_t> got;
    for (auto const& [offset, n] : counts) { (void)n; got.push_back(offset); }
    EXPECT_EQ(got, expected);

    // Each long double's two eightbytes are read from ONE home, at +0 and +8.
    auto const from = outgoingStoreSources(p.wide.lir, *schema, call.funcIndex);
    for (std::int32_t i = 0; i < longDoubles; ++i) {
        auto const lo = static_cast<std::uint32_t>(firstLongDouble + 16 * i);
        ASSERT_TRUE(from.contains(lo) && from.contains(lo + 8u))
            << "long double " << i << " has no outgoing store at +" << lo;
        StoredFrom const a = from.at(lo);
        StoredFrom const b = from.at(lo + 8u);
        EXPECT_TRUE(a.fromGprLoad && b.fromGprLoad)
            << "long double " << i << " is not copied out of its home";
        EXPECT_EQ(a.baseVReg, b.baseVReg) << "long double " << i;
        EXPECT_EQ(a.disp, 0) << "long double " << i;
        EXPECT_EQ(b.disp, 8) << "long double " << i;
        EXPECT_EQ(a.defCount, 1u)
            << "long double " << i << ": its low eightbyte's register is defined "
            << a.defCount << " times — a fresh register inside the function's own "
               "id range";
        EXPECT_EQ(b.defCount, 1u)
            << "long double " << i << ": its high eightbyte's register is defined "
            << b.defCount << " times — a fresh register inside the function's own "
               "id range";
    }
}

TEST(OutgoingArgCursor, EightIntsAndElevenLongDoublesCompileOnSysV) {
    // fmt + five ints in rdi..r9; ints six to eight at +0/+8/+16; the long
    // doubles 16-aligned from +32, the eleventh at +192. 208 bytes reserved.
    expectLongDoublesCopiedBeforeTheCall(kEightIntsElevenLongDoubles, {0, 8, 16},
                                         32, 11, 208u);
}

TEST(OutgoingArgCursor, FourteenLongDoublesCompileOnSysV) {
    // fmt in rdi; fourteen long doubles from +0, the last at +208. 224 bytes.
    expectLongDoublesCopiedBeforeTheCall(kFourteenLongDoubles, {}, 0, 14, 224u);
}

} // namespace
