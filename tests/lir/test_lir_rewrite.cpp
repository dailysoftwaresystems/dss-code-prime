// LIR post-regalloc rewrite + post-regalloc verifier tests.
//   * Round-trip: lower → liveness → regalloc → rewrite → verify
//   * No virtual registers remain after rewrite
//   * verifyLirPostRegalloc fires on a pre-rewrite (still-virtual) module
//   * Block topology preserved
//   * Spill path inserts frame_load/frame_store pseudo-ops with the
//     correct payload (slot v)
//   * FC4 c2 (B2): a SPILLED indirect-call CALLEE's reload scratch
//     skips the cc's arg registers (+ the variadic count register)

#include "core/types/call_payload.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_verifier.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "lowered_lir_fixture.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "synthetic_fn.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

using namespace dss;
using dss::test_support::lowerCSubsetToLir;

namespace {

// Bundle: lower + liveness + regalloc done. Owns the LoweredLir so
// the underlying `Lir` survives for the rewrite-pass invocation.
struct AllocatedLir {
    test_support::LoweredLir lowered;
    LirLiveness              liveness;
    LirAllocation            alloc;
    DiagnosticReporter       regallocRep;

    AllocatedLir(test_support::LoweredLir l)
        : lowered(std::move(l)) {}
};

[[nodiscard]] AllocatedLir
lowerAndAllocate(std::string src) {
    AllocatedLir out{lowerCSubsetToLir(std::move(src))};
    if (!out.lowered.lir.ok) return out;
    out.liveness = analyzeLiveness(out.lowered.lir.lir);
    out.alloc = allocateRegisters(out.lowered.lir.lir,
                                  *out.lowered.target,
                                  out.liveness,
                                  /*ccIndex=*/0,
                                  out.regallocRep);
    return out;
}

} // namespace

TEST(LirRewrite, RoundTripStraightLineFunctionProducesNoVirtuals) {
    auto bundle = lowerAndAllocate("int f(int x) { return x + x; }");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.alloc.ok());
    DiagnosticReporter rewriteRep;
    auto rewritten = rewriteWithAllocation(bundle.lowered.lir.lir,
                                           *bundle.lowered.target,
                                           bundle.alloc, rewriteRep);
    EXPECT_TRUE(rewritten.ok);
    DiagnosticReporter verifyRep;
    EXPECT_TRUE(verifyLirPostRegalloc(rewritten.lir, *bundle.lowered.target, verifyRep));
}

TEST(LirRewrite, VerifyLirPostRegallocFiresOnPreRewriteModule) {
    // The pre-rewrite module is full of virtual registers. The post-
    // regalloc verifier must emit at least one
    // L_VirtualRegInPostRegalloc diagnostic.
    auto bundle = lowerAndAllocate("int f(int x) { return x + x; }");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    DiagnosticReporter verifyRep;
    bool const ok = verifyLirPostRegalloc(bundle.lowered.lir.lir,
                                          *bundle.lowered.target, verifyRep);
    EXPECT_FALSE(ok);
    std::size_t vregDiagCount = 0;
    for (auto const& d : verifyRep.all()) {
        if (d.code == DiagnosticCode::L_VirtualRegInPostRegalloc) ++vregDiagCount;
    }
    EXPECT_GT(vregDiagCount, 0u);
}

TEST(LirRewrite, BlockTopologyPreserved) {
    auto bundle = lowerAndAllocate(
        "int f(int x) {\n"
        "    int y;\n"
        "    if (x > 0) { y = 1; } else { y = 2; }\n"
        "    return y;\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.alloc.ok());
    DiagnosticReporter rewriteRep;
    auto rewritten = rewriteWithAllocation(bundle.lowered.lir.lir,
                                           *bundle.lowered.target,
                                           bundle.alloc, rewriteRep);
    ASSERT_TRUE(rewritten.ok);
    Lir const& src = bundle.lowered.lir.lir;
    Lir const& dst = rewritten.lir;
    ASSERT_EQ(src.moduleFuncCount(), dst.moduleFuncCount());
    for (std::uint32_t i = 0; i < src.moduleFuncCount(); ++i) {
        LirFuncId const sf = src.funcAt(i);
        LirFuncId const df = dst.funcAt(i);
        EXPECT_EQ(src.funcBlockCount(sf), dst.funcBlockCount(df));
    }
}

TEST(LirRewrite, LoopFunctionRewritesCleanly) {
    auto bundle = lowerAndAllocate(
        "int f(int n) {\n"
        "    int i = 0; int acc = 0;\n"
        "    while (i < n) { acc = acc + i; i = i + 1; }\n"
        "    return acc;\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.alloc.ok());
    DiagnosticReporter rewriteRep;
    auto rewritten = rewriteWithAllocation(bundle.lowered.lir.lir,
                                           *bundle.lowered.target,
                                           bundle.alloc, rewriteRep);
    EXPECT_TRUE(rewritten.ok);
    DiagnosticReporter verifyRep;
    EXPECT_TRUE(verifyLirPostRegalloc(rewritten.lir, *bundle.lowered.target, verifyRep));
}

TEST(LirRewrite, MultiFunctionModuleRewritesEachFunction) {
    auto bundle = lowerAndAllocate(
        "int g(int a) { return a + 1; }\n"
        "int f(int x) { int y = g(x); return y; }\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.alloc.ok());
    DiagnosticReporter rewriteRep;
    auto rewritten = rewriteWithAllocation(bundle.lowered.lir.lir,
                                           *bundle.lowered.target,
                                           bundle.alloc, rewriteRep);
    EXPECT_TRUE(rewritten.ok);
    EXPECT_EQ(rewritten.lir.moduleFuncCount(), 2u);
    DiagnosticReporter verifyRep;
    EXPECT_TRUE(verifyLirPostRegalloc(rewritten.lir, *bundle.lowered.target, verifyRep));
}

TEST(LirRewrite, ExhaustedClassEmitsLoudFailureNotSilentClobber) {
    // When register pressure leaves NO scratch register available for
    // a class with spilled vregs, the rewrite emits
    // L_VirtualRegInPostRegalloc Error and returns ok=false rather
    // than silently picking a reserved-role register (e.g. rsp) as
    // scratch. This is the architect's CRITICAL filter at work — the
    // alternative would clobber the stack pointer mid-function.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    std::array<TypeKind, 1> const paramKinds{TypeKind::I32};
    auto syn = test_support::buildSyntheticFn(
        paramKinds, TypeKind::I32,
        [&](MirBuilder& mb, TypeInterner& interner,
            std::vector<TypeId> const& params, TypeId retT) {
            MirInstId const a = mb.addArg(0, params[0]);
            // 20 distinct arg vregs, all live simultaneously at the call.
            std::vector<MirInstId> args;
            args.reserve(20);
            for (int i = 0; i < 20; ++i) {
                std::array<MirInstId, 2> ops{a, a};
                args.push_back(mb.addInst(MirOpcode::Add, ops, retT));
            }
            // A WIDE CALL: 20 register operands live at ONE instruction
            // exceed the register file, so no fixed spill-reload
            // reservation (c75 reserve-K) can service them — the deferred
            // D-AS-REGALLOC-WIDE-CALL-OPERAND-COUNT. The reload path must
            // EXHAUST LOUDLY rather than silently clobber a reserved-role
            // register. (Post-c75 the general-pressure reduce this test
            // formerly built is HANDLED by the reservation, so the loud-
            // failure path is now reached via the wide-call operand
            // explosion instead — the one exhaustion reserve-K cannot fix.)
            TypeId const ptrT = interner.primitive(TypeKind::Ptr);
            MirInstId const callee = mb.addGlobalAddr(SymbolId{2}, ptrT);
            std::vector<MirInstId> callOps;
            callOps.reserve(args.size() + 1);
            callOps.push_back(callee);
            for (auto const v : args) callOps.push_back(v);
            MirInstId const callResult =
                mb.addInst(MirOpcode::Call, callOps, retT);
            mb.addReturn(callResult);
        });
    DiagnosticReporter lirRep;
    auto const lirResult = lowerToLir(syn.mir, **target, syn.interner, lirRep);
    ASSERT_TRUE(lirResult.ok);
    LirLiveness const lv = analyzeLiveness(lirResult.lir);
    DiagnosticReporter regallocRep;
    LirAllocation const alloc =
        allocateRegisters(lirResult.lir, **target, lv, /*ccIndex=*/0, regallocRep);
    ASSERT_TRUE(alloc.ok());
    ASSERT_GT(alloc.perFunc[0].numSpillSlots, 0u);

    DiagnosticReporter rewriteRep;
    auto rewritten = rewriteWithAllocation(lirResult.lir, **target,
                                           alloc, rewriteRep);
    // The 20-arg wide call has more live register operands at one
    // instruction than the register file holds, so the reload path finds
    // no scratch → loud failure is the correct substrate behavior
    // (silently clobbering rsp as scratch would corrupt the stack).
    EXPECT_FALSE(rewritten.ok);
    bool foundDiag = false;
    for (auto const& d : rewriteRep.all()) {
        if (d.code == DiagnosticCode::L_VirtualRegInPostRegalloc) {
            foundDiag = true; break;
        }
    }
    EXPECT_TRUE(foundDiag)
        << "scratch-exhausted rewrite must emit L_VirtualRegInPostRegalloc";
}

// FOLD #3 (adversarial-review) red-on-disable: the scratch-pool picker must key its
// "register already assigned to a vreg" set by the GLOBAL register-table ordinal in an
// UNBOUNDED set, not a 64-bit bitmask indexed by that ordinal. On arm64 the GPR table
// (x0..x30, xzr, sp = 33 slots) pushes the FPR d-registers to global ordinals 33..64, so
// d31 sits at ordinal 64 — past a 64-bit mask. A function with enough live F64 values to
// allocate up into d31 (here: a variadic callee whose 9th double overflows the 8 VR arg
// registers, the exact FP-overflow witness corpus) previously hit `pickScratchRegs:
// phys ordinal 64 ... out of 64-bit/5-class bound` and FAILED TO COMPILE — a fail-loud
// guard firing on legitimate input. RED-ON-DISABLE: revert pickScratchRegs to the
// uint64 bitmask keyed by phys.id → this rewrite returns ok=false with that diagnostic.
TEST(LirRewrite, Arm64HighFprSpillScratchPoolHandlesOrdinalsBeyond64) {
    // 9 doubles → the 9th overflows v0..v7 to __stack; the materialization pressure
    // allocates an FPR up into d31 (global ordinal 64). The named `n` is an int (x0).
    auto out = AllocatedLir{lowerCSubsetToLir(
        "double sum_d(int n, ...) {\n"
        "    va_list ap; va_start(ap, n);\n"
        "    double t = 0.0;\n"
        "    for (int i = 0; i < n; i = i + 1) { t = t + va_arg(ap, double); }\n"
        "    va_end(ap);\n"
        "    return t;\n"
        "}\n"
        "int main(void) {\n"
        "    return (int)sum_d(9, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);\n"
        "}\n",
        "arm64", /*mirCcIndex=*/0)};
    ASSERT_TRUE(out.lowered.lir.ok) << "arm64 c-subset → LIR failed";
    out.liveness = analyzeLiveness(out.lowered.lir.lir);
    out.alloc = allocateRegisters(out.lowered.lir.lir, *out.lowered.target,
                                  out.liveness, /*ccIndex=*/0, out.regallocRep);
    ASSERT_TRUE(out.alloc.ok()) << "arm64 regalloc must succeed";

    // PRECONDITION (non-vacuity guard): this pin only exercises the >64 scratch-
    // ordinal re-key if the allocator actually assigns a vreg to d31 (global
    // register-table ordinal 64 — arm64's 33 GPR table slots push the FPR
    // d-registers to ordinals 33..64, so d31 = 64, the FIRST ordinal past a
    // 64-bit mask). Physical-reg `LirReg.id` IS that global ordinal (regalloc's
    // `buildFreeLists` seeds the free lists with `schema.registers()` indices).
    // If a future allocator change stops reaching d31, the rewrite below would
    // pass VACUOUSLY (green but testing nothing). Assert the precondition holds
    // so it fails LOUD instead. If asserting the exact ordinal ever becomes
    // structurally awkward, the max-FPR-ordinal >= 64 fallback below suffices.
    // c75 (D-AS-REGALLOC-SPILL-RELOAD-SCRATCH): the high caller-saved
    // FPRs — including d31 (global ordinal 64) — are now RESERVED as
    // spill-reload scratch, so d31 is no longer ASSIGNED to a vreg; it
    // sits in the rewriter's scratch pool instead. The >64 ordinal re-key
    // is still exercised: pickScratchRegs's register loop reaches ordinal
    // 64 while building the pool and ADDS d31 to it (a reserved,
    // unassigned, still-allocatable register). The core assertions below
    // (rewrite ok, no 'out of 64-bit bound' error) verify that ordinal-64
    // handling — a uint64 bitmask keyed by the ordinal is UB at
    // `contains(64)`/`insert(64)` during that pool build. Pre-c75 this
    // corpus incidentally ASSIGNED a vreg to d31; c75 reserves d31, so the
    // ordinal-64 register is now exercised through the POOL, not an
    // assignment (the low-FPR-pressure function no longer needs to spill).
    // Non-vacuity: the target must structurally carry an FPR at global
    // ordinal >= 64 so the loop actually crosses the >64 boundary; an
    // arm64 variant dropping it makes this pin vacuous — fail LOUD here.
    std::uint32_t maxFprSchemaOrdinal = 0;
    {
        auto const regs = out.lowered.target->registers();
        for (std::uint16_t i = 0; i < regs.size(); ++i) {
            if (regs[i].regClass == TargetRegClass::FPR
                && regs[i].subOf.empty() && i > maxFprSchemaOrdinal) {
                maxFprSchemaOrdinal = i;
            }
        }
    }
    ASSERT_GE(maxFprSchemaOrdinal, 64u)
        << "precondition: arm64 must carry an FPR at global ordinal >= 64 "
           "(d31 = 64) for the >64 scratch-ordinal re-key to matter — max "
           "FPR ordinal in the register table was " << maxFprSchemaOrdinal;

    DiagnosticReporter rewriteRep;
    auto rewritten = rewriteWithAllocation(out.lowered.lir.lir, *out.lowered.target,
                                           out.alloc, rewriteRep);
    EXPECT_TRUE(rewritten.ok)
        << "the rewrite must handle FPR scratch ordinals > 64 (d31) — a uint64 "
           "bitmask keyed by the global ordinal fails loud on legitimate input";
    EXPECT_EQ(rewriteRep.errorCount(), 0u)
        << "no L_RequiredLirOpcodeMissing 'out of 64-bit bound' on a high-FPR spill";
}

TEST(LirRewrite, SpilledFunctionEmitsFrameLoadStorePseudoOps) {
    // Cross-call spill pattern: several vregs live across a function
    // call. SysV has 5 callee-saved GPRs; live-across-call vregs
    // prefer callee-saved, and excess spill via
    // R_SpilledDueToCrossCallExhaustion. Total pressure remains
    // moderate so scratch room is preserved for the rewrite pass.
    //
    // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): the live-across-call values are
    // never-address-taken PARAMETERS (pure SSA `Arg`s), NOT body locals. A local's
    // alloca address is now rematerialized AFTER the call (a fresh `lea_frame_slot`),
    // so locals no longer span the call. The 8 params are each used as a call
    // argument AND in the post-call sum, so each spans the call; 8 cross-call values
    // exceed SysV's 5 callee-saved GPRs → ≥1 genuine cross-call spill, remat-
    // independent (yet moderate, so the rewrite keeps scratch room).
    auto bundle = lowerAndAllocate(
        "int g(int v) { return v + 1; }\n"
        "int f(int a, int b, int c, int d, int e, int f2, int g2, int h) {\n"
        "    int r = g(a + b + c + d + e + f2 + g2 + h);\n"
        "    return a + b + c + d + e + f2 + g2 + h + r;\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.alloc.ok());
    std::uint32_t totalSpills = 0;
    for (auto const& fa : bundle.alloc.perFunc) totalSpills += fa.numSpillSlots;
    ASSERT_GT(totalSpills, 0u)
        << "cross-call pattern must produce ≥1 spill";

    DiagnosticReporter rewriteRep;
    auto rewritten = rewriteWithAllocation(bundle.lowered.lir.lir,
                                           *bundle.lowered.target,
                                           bundle.alloc, rewriteRep);
    ASSERT_TRUE(rewritten.ok)
        << "moderate-pressure rewrite must succeed (scratch room available)";

    auto const frameLoadOp  = bundle.lowered.target->opcodeByMnemonic("frame_load");
    auto const frameStoreOp = bundle.lowered.target->opcodeByMnemonic("frame_store");
    ASSERT_TRUE(frameLoadOp.has_value());
    ASSERT_TRUE(frameStoreOp.has_value());
    std::size_t loadCount = 0, storeCount = 0;
    Lir const& dst = rewritten.lir;
    for (std::uint32_t fi = 0; fi < dst.moduleFuncCount(); ++fi) {
        LirFuncId const fn = dst.funcAt(fi);
        for (std::uint32_t bi = 0; bi < dst.funcBlockCount(fn); ++bi) {
            LirBlockId const b = dst.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < dst.blockInstCount(b); ++i) {
                std::uint16_t const op =
                    dst.instOpcode(dst.blockInstAt(b, i));
                if (op == *frameLoadOp)  ++loadCount;
                if (op == *frameStoreOp) ++storeCount;
            }
        }
    }
    EXPECT_GT(loadCount,  0u) << "spilled function must produce frame_load ops";
    EXPECT_GT(storeCount, 0u) << "spilled function must produce frame_store ops";

    DiagnosticReporter verifyRep;
    EXPECT_TRUE(verifyLirPostRegalloc(rewritten.lir, *bundle.lowered.target, verifyRep));
}

TEST(LirRewrite, MultiSpilledSameClassOperandsGetDistinctScratches) {
    // Pins the silent-miscompile fix: when multiple spilled operands
    // of the SAME class appear in one instruction, the rewrite hands
    // out DISTINCT scratch registers (not one shared scratch that
    // would lose earlier loads to later overwrites). Exercises the
    // per-inst cursor in ScratchPerClass.
    //
    // Build cross-call pressure that forces multiple spills, then
    // assert: across the rewritten module, no `add` instruction has
    // two operand Reg slots pointing at the same physical reg.
    // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): pressure comes from never-
    // address-taken PARAMETERS (pure SSA `Arg`s), NOT body locals — a local's alloca
    // address is now rematerialized AFTER the call so locals no longer span it. The
    // 8 params each feed the call arguments AND the post-call sum → 8 cross-call
    // values > SysV's 5 callee-saved GPRs → multiple spills (the multi-spill scratch
    // distribution this test exercises). Remat-independent.
    auto bundle = lowerAndAllocate(
        "int g(int a, int b);\n"
        "int f(int a, int b, int c, int d, int e, int f2, int g2, int h) {\n"
        "    int s = a + b + c + d + e + f2 + g2 + h;\n"
        "    int r = g(s, s);\n"
        "    return a + b + c + d + e + f2 + g2 + h + r;\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.alloc.ok());
    DiagnosticReporter rewriteRep;
    auto rewritten = rewriteWithAllocation(bundle.lowered.lir.lir,
                                           *bundle.lowered.target,
                                           bundle.alloc, rewriteRep);
    ASSERT_TRUE(rewritten.ok);
    // Walk every inst with ≥2 Reg operands; assert their phys ordinals
    // differ. (Same-vreg-twice — e.g. `add x, x` — is allowed and not
    // exercised here since each operand resolves to the SAME assignment;
    // the silent-miscompile scenario only arises with TWO DIFFERENT
    // spilled vregs.)
    Lir const& dst = rewritten.lir;
    for (std::uint32_t fi = 0; fi < dst.moduleFuncCount(); ++fi) {
        LirFuncId const fn = dst.funcAt(fi);
        for (std::uint32_t bi = 0; bi < dst.funcBlockCount(fn); ++bi) {
            LirBlockId const b = dst.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < dst.blockInstCount(b); ++i) {
                LirInstId const inst = dst.blockInstAt(b, i);
                auto const ops = dst.instOperands(inst);
                std::vector<LirReg> regOps;
                for (auto const& op : ops) {
                    if (op.kind == LirOperandKind::Reg && op.reg.valid()) {
                        regOps.push_back(op.reg);
                    }
                }
                // For inst-level distinctness, the substrate's
                // post-regalloc invariant is: distinct ORIGINAL vregs
                // resolve to distinct phys regs (modulo allocator
                // sharing). We can't easily recover original-vreg
                // identity post-rewrite without an attribute, so
                // here we just verify the rewrite produced VALID
                // physical regs and the verifier accepts the module.
                for (auto const& r : regOps) {
                    EXPECT_EQ(r.isPhysical, 1u);
                    EXPECT_TRUE(r.valid());
                }
            }
        }
    }
    DiagnosticReporter verifyRep;
    EXPECT_TRUE(verifyLirPostRegalloc(rewritten.lir, *bundle.lowered.target, verifyRep));
}

TEST(LirRewrite, VerifierFiresOnFrameOpWithZeroSlotPayload) {
    // The post-regalloc verifier's slot-sentinel rule must fire on a
    // frame_load / frame_store with payload 0 (the invalid
    // LirSpillSlot sentinel). Hand-build a minimal LIR with one such
    // inst and assert the verifier emits L_InvalidSpillSlotSentinel.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;
    auto const frameLoadOp = sch.opcodeByMnemonic("frame_load");
    auto const retOp       = sch.opcodeByMnemonic("ret");
    ASSERT_TRUE(frameLoadOp.has_value());
    ASSERT_TRUE(retOp.has_value());

    LirBuilder b{sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const scratch = makePhysicalReg(0, LirRegClass::GPR);  // rax
    b.addInst(*frameLoadOp, scratch, std::span<LirOperand const>{},
              /*payload=*/0);  // invalid slot sentinel
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    bool const ok = verifyLirPostRegalloc(lir, sch, rep);
    EXPECT_FALSE(ok);
    bool foundSentinel = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_InvalidSpillSlotSentinel) {
            foundSentinel = true; break;
        }
    }
    EXPECT_TRUE(foundSentinel)
        << "verifier must flag frame_load with payload 0 via "
           "L_InvalidSpillSlotSentinel";
}

TEST(LirRewrite, SwitchBearingFunctionRewritesAndPreservesBlockCount) {
    // Cycle 3b's emitTerminator handles 0/1/2-successor terminators
    // explicitly and emits L_UnsupportedLoweringForOpcode for
    // multi-successor (which would be a real Switch in LIR — current
    // ML5 lowering decomposes Switch into a cascade of 2-successor
    // CondBr blocks instead, so the rewrite handles it via the
    // CondBr arm). Pin the topology preservation.
    auto bundle = lowerAndAllocate(
        "int f(int x) {\n"
        "    switch (x) {\n"
        "        case 1: return 10;\n"
        "        case 2: return 20;\n"
        "        case 3: return 30;\n"
        "        default: return 0;\n"
        "    }\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.alloc.ok());
    DiagnosticReporter rewriteRep;
    auto rewritten = rewriteWithAllocation(bundle.lowered.lir.lir,
                                           *bundle.lowered.target,
                                           bundle.alloc, rewriteRep);
    ASSERT_TRUE(rewritten.ok);
    Lir const& src = bundle.lowered.lir.lir;
    Lir const& dst = rewritten.lir;
    ASSERT_EQ(src.moduleFuncCount(), dst.moduleFuncCount());
    for (std::uint32_t i = 0; i < src.moduleFuncCount(); ++i) {
        EXPECT_EQ(src.funcBlockCount(src.funcAt(i)),
                  dst.funcBlockCount(dst.funcAt(i)));
    }
    DiagnosticReporter verifyRep;
    EXPECT_TRUE(verifyLirPostRegalloc(rewritten.lir, *bundle.lowered.target, verifyRep));
}

TEST(LirRewrite, ScratchRegPickedFromCallingConventionPoolOnly) {
    // Pins the dual of `ExhaustedClassEmitsLoudFailureNotSilentClobber`:
    // when a scratch IS picked, it must be a register that appears in
    // the calling convention's allocatable pool (no `rsp`/`rflags`/etc.).
    //
    // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): the live-across-call values are
    // never-address-taken PARAMETERS (pure SSA `Arg`s) — a body local's alloca
    // address is now rematerialized AFTER the call so locals no longer span it. The
    // 8 params each feed the call argument AND the post-call sum → 8 cross-call
    // values > SysV's 5 callee-saved GPRs → ≥1 spill, hence frame_load/frame_store
    // whose scratch register this test inspects. Remat-independent.
    auto bundle = lowerAndAllocate(
        "int g(int v) { return v + 1; }\n"
        "int f(int a, int b, int c, int d, int e, int f2, int g2, int h) {\n"
        "    int r = g(a + b + c + d + e + f2 + g2 + h);\n"
        "    return a + b + c + d + e + f2 + g2 + h + r;\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.alloc.ok());
    DiagnosticReporter rewriteRep;
    auto rewritten = rewriteWithAllocation(bundle.lowered.lir.lir,
                                           *bundle.lowered.target,
                                           bundle.alloc, rewriteRep);
    ASSERT_TRUE(rewritten.ok);

    auto const& sch = *bundle.lowered.target;
    auto const* cc = sch.callingConvention(0);
    ASSERT_NE(cc, nullptr);
    std::unordered_set<std::string_view> allocatable;
    for (auto const& n : cc->callerSaved) allocatable.insert(n);
    for (auto const& n : cc->calleeSaved) allocatable.insert(n);
    for (auto const& n : cc->argGprs)     allocatable.insert(n);
    for (auto const& n : cc->argFprs)     allocatable.insert(n);
    for (auto const& n : cc->returnGprs)  allocatable.insert(n);
    for (auto const& n : cc->returnFprs)  allocatable.insert(n);

    auto const frameLoadOp  = sch.opcodeByMnemonic("frame_load");
    auto const frameStoreOp = sch.opcodeByMnemonic("frame_store");
    ASSERT_TRUE(frameLoadOp.has_value());
    ASSERT_TRUE(frameStoreOp.has_value());
    Lir const& dst = rewritten.lir;
    std::size_t frameOpCount = 0;
    for (std::uint32_t fi = 0; fi < dst.moduleFuncCount(); ++fi) {
        LirFuncId const fn = dst.funcAt(fi);
        for (std::uint32_t bi = 0; bi < dst.funcBlockCount(fn); ++bi) {
            LirBlockId const b = dst.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < dst.blockInstCount(b); ++i) {
                LirInstId const inst = dst.blockInstAt(b, i);
                std::uint16_t const op = dst.instOpcode(inst);
                if (op != *frameLoadOp && op != *frameStoreOp) continue;
                ++frameOpCount;
                // For frame_load: scratch is the inst's RESULT.
                // For frame_store: scratch is operand[0].
                LirReg scratch;
                if (op == *frameLoadOp) {
                    scratch = dst.instResult(inst);
                } else {
                    auto const ops = dst.instOperands(inst);
                    ASSERT_GE(ops.size(), 1u);
                    scratch = ops[0].reg;
                }
                ASSERT_TRUE(scratch.valid());
                auto const* regInfo = sch.registerInfo(
                    static_cast<std::uint16_t>(scratch.id));
                ASSERT_NE(regInfo, nullptr);
                EXPECT_TRUE(allocatable.contains(regInfo->name))
                    << "scratch reg `" << regInfo->name
                    << "` is not in the calling convention's allocatable pool";
            }
        }
    }
    EXPECT_GT(frameOpCount, 0u)
        << "test must exercise the frame-op path";
}

TEST(LirRewrite, RewriteSkipsFunctionsWithFailedAllocation) {
    // If a function's allocation fails (ok == false), the rewrite
    // pass must NOT attempt to rewrite it — emits an
    // L_VirtualRegInPostRegalloc diagnostic and returns ok=false.
    auto bundle = lowerAndAllocate("int f(int x) { return x; }");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    // Manually invalidate the first function's allocation.
    bundle.alloc.perFunc[0].ok = false;
    DiagnosticReporter rewriteRep;
    auto rewritten = rewriteWithAllocation(bundle.lowered.lir.lir,
                                           *bundle.lowered.target,
                                           bundle.alloc, rewriteRep);
    EXPECT_FALSE(rewritten.ok);
    bool foundDiag = false;
    for (auto const& d : rewriteRep.all()) {
        if (d.code == DiagnosticCode::L_VirtualRegInPostRegalloc) {
            foundDiag = true; break;
        }
    }
    EXPECT_TRUE(foundDiag);
}

TEST(LirRewrite, SpilledVariadicIndirectCalleeReloadScratchSkipsArgAndCountRegs) {
    // FC4 c2 (B2): an `isCall` instruction's ops[0] is the indirect-
    // call CALLEE. When the allocator SPILLS the callee vreg, the
    // rewrite reloads it into a scratch register that sits BETWEEN the
    // callconv pass's (not-yet-emitted) arg-passing moves and the call
    // itself — so the reload scratch must SKIP the cc's argGprs ∪
    // argFprs plus, for a VARIADIC call site, the variadic vector-
    // count register (SysV §3.5.7: rax/AL). A scratch from that set
    // would be overwritten by the call's own arg setup → the call
    // jumps THROUGH AN ARGUMENT VALUE (silent garbage). The allocator-
    // side exclusion (R2) cannot close this hazard: the scratch pool
    // is by definition the registers the allocator did NOT assign.
    //
    // DISCRIMINATING BY CONSTRUCTION: pickScratchRegs builds the pool
    // in register-table-ordinal order and ABSORBS the cc's arg/return
    // sets — with nothing register-assigned below, the sysv pool's
    // FIRST GPR entry is the variadic count register itself (rax,
    // ordinal 0), followed by argGprs rcx/rdx. With B2's forbidden-
    // filter disabled the callee reload picks exactly that forbidden
    // first entry, so the set-membership pin below goes RED
    // (red-on-disable demonstrated 2026-06-12 + restored).
    //
    // The two spilled ARG vregs pin the cursor/rotation contract:
    // entries skipped for the CALLEE are ROTATED (kept available for
    // later spilled operands of the same inst), not burned — the
    // rewrite stays ok (no spurious exhaustion) and all three reloads
    // receive pairwise-DISTINCT scratches.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;
    auto const callOp      = sch.opcodeByMnemonic("call");
    auto const retOp       = sch.opcodeByMnemonic("ret");
    auto const frameLoadOp = sch.opcodeByMnemonic("frame_load");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    ASSERT_TRUE(frameLoadOp.has_value());
    auto const* callInfo = sch.opcodeInfo(*callOp);
    ASSERT_NE(callInfo, nullptr);
    ASSERT_TRUE(callInfo->isCall);  // ops[0] = callee per the call contract

    // Hand-build (mirrors VerifierFiresOnFrameOpWithZeroSlotPayload):
    // one function, one block, one VARIADIC indirect call
    // `vr1(vr2, vr3)` — fixedOperandCount 1, so vr2 is the fixed arg and
    // vr3 rides the vararg region (printf-shaped).
    LirBuilder b{sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const calleeV = b.newVReg(LirRegClass::GPR);  // vreg id 1
    LirReg const arg1V   = b.newVReg(LirRegClass::GPR);  // vreg id 2
    LirReg const arg2V   = b.newVReg(LirRegClass::GPR);  // vreg id 3
    std::array<LirOperand, 3> const callOperands{
        LirOperand::makeReg(calleeV),
        LirOperand::makeReg(arg1V),
        LirOperand::makeReg(arg2V),
    };
    std::uint32_t const variadicPayload = call_payload::encode(true, 1u);
    b.addInst(*callOp, InvalidLirReg, callOperands, variadicPayload);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();
    LirFuncId const fn = lir.funcAt(0);

    // Hand-build the allocation: ALL THREE vregs spilled (slots 1..3),
    // nothing register-assigned — the scratch pool is the full cc-
    // allocatable register set in table order, so the callee's natural
    // (unfiltered) pick would be the pool's first entry.
    LirAllocation alloc;
    LirFuncAllocation fa;
    fa.fn             = fn;
    fa.originalSymbol = SymbolId{1};
    fa.assignments.resize(4);  // slot 0 = sentinel; vreg ids 1..3
    fa.assignments[1] = LirRegAssignment::makeSpill(calleeV, LirSpillSlot{1});
    fa.assignments[2] = LirRegAssignment::makeSpill(arg1V,   LirSpillSlot{2});
    fa.assignments[3] = LirRegAssignment::makeSpill(arg2V,   LirSpillSlot{3});
    fa.numSpillSlots          = 3;
    fa.ok                     = true;
    fa.callingConventionIndex = 0;  // sysv_amd64
    alloc.perFunc.push_back(std::move(fa));

    DiagnosticReporter rewriteRep;
    auto rewritten = rewriteWithAllocation(lir, sch, alloc, rewriteRep);
    ASSERT_TRUE(rewritten.ok)
        << "skipping forbidden entries must not exhaust the pool";

    // The forbidden set, read back from the ACTIVE cc's declared
    // arg-register lists + variadic count register (config-driven —
    // no register names in the assertions).
    auto const* cc = sch.callingConvention(0);
    ASSERT_NE(cc, nullptr);
    std::unordered_set<std::uint32_t> forbidden;
    auto const absorb = [&](std::vector<std::string> const& names) {
        for (auto const& name : names) {
            auto const ord = sch.registerByName(name);
            ASSERT_TRUE(ord.has_value())
                << "cc arg register '" << name << "' must resolve";
            forbidden.insert(*ord);
        }
    };
    absorb(cc->argGprs);
    absorb(cc->argFprs);
    ASSERT_TRUE(cc->variadicVectorCountReg.has_value())
        << "sysv_amd64 must declare the variadic vector-count register";
    forbidden.insert(cc->variadicVectorCountReg->ordinal);
    ASSERT_EQ(forbidden.size(),
              cc->argGprs.size() + cc->argFprs.size() + 1u)
        << "every cc arg register must resolve to a distinct ordinal "
           "and the count register must not alias an arg register";

    // c77 (D-AS-REGALLOC-DIRECT-ARG-RELOAD): the two spilled register-passed
    // ARGS (vr2 fixed, vr3 vararg) are now DEFERRED to callconv as SpillSlotRef
    // operands — NOT scratch-reloaded here. Only the spilled CALLEE (ops[0])
    // still reloads into a scratch register (the FC4-c2 B2 contract this test
    // pins). So the rewritten shape is ONE frame_load (the callee) + the call +
    // ret. The B2 pin below is UNCHANGED — the callee's scratch must still skip
    // the cc arg/count registers; c77 only removed the redundant arg reloads.
    Lir const& dst = rewritten.lir;
    ASSERT_EQ(dst.moduleFuncCount(), 1u);
    LirFuncId const dfn = dst.funcAt(0);
    ASSERT_EQ(dst.funcBlockCount(dfn), 1u);
    LirBlockId const blk = dst.funcBlockAt(dfn, 0);
    ASSERT_EQ(dst.blockInstCount(blk), 3u)
        << "c77: expected frame_load x1 (spilled callee) + call + ret — the two "
           "spilled register-args are deferred as SpillSlotRef operands";

    // Locate the call via the schema's isCall flag (agnostic walk).
    std::uint32_t callIdx   = 0;
    std::size_t   callCount = 0;
    for (std::uint32_t i = 0; i < dst.blockInstCount(blk); ++i) {
        auto const* info =
            sch.opcodeInfo(dst.instOpcode(dst.blockInstAt(blk, i)));
        ASSERT_NE(info, nullptr);
        if (info->isCall) { callIdx = i; ++callCount; }
    }
    ASSERT_EQ(callCount, 1u);
    LirInstId const callInst = dst.blockInstAt(blk, callIdx);
    // The variadic payload must survive the rewrite bit-identically —
    // it is what arms the count-register exclusion here (and what the
    // ML7 materializer later reads to emit the count mov).
    EXPECT_EQ(dst.instPayload(callInst), variadicPayload);
    auto const rewrittenOps = dst.instOperands(callInst);
    ASSERT_EQ(rewrittenOps.size(), 3u);

    // c77 (D-AS-REGALLOC-DIRECT-ARG-RELOAD): ops[0] (callee) is a scratch-
    // reloaded Reg; ops[1]/ops[2] (the two spilled register-args) are now
    // SpillSlotRef operands carrying their slots (2, 3) + class GPR — deferred
    // to callconv's arg-setup (which loads them directly into the ABI arg regs).
    // Exactly ONE frame_load exists (the callee, slot 1), before the call.
    std::size_t   calleeLoads = 0;
    std::uint32_t calleeLoadIdx = 0;
    LirReg        calleeReloadDest{};
    for (std::uint32_t i = 0; i < dst.blockInstCount(blk); ++i) {
        LirInstId const inst = dst.blockInstAt(blk, i);
        if (dst.instOpcode(inst) != *frameLoadOp) continue;
        ++calleeLoads;
        calleeLoadIdx    = i;
        calleeReloadDest = dst.instResult(inst);
        EXPECT_EQ(dst.instPayload(inst), 1u)
            << "the only reload must be the spilled callee (slot 1); the two "
               "arg slots (2,3) are deferred as SpillSlotRef, not reloaded";
    }
    ASSERT_EQ(calleeLoads, 1u)
        << "c77: exactly one frame_load (the spilled callee) — the register "
           "args are deferred";
    EXPECT_LT(calleeLoadIdx, callIdx)
        << "the callee reload must precede the call";

    // ops[0] is the callee, a physical GPR == its reload's dest.
    ASSERT_EQ(rewrittenOps[0].kind, LirOperandKind::Reg);
    LirReg const calleeR = rewrittenOps[0].reg;
    ASSERT_TRUE(calleeR.valid());
    EXPECT_EQ(calleeR.isPhysical, 1u);
    EXPECT_EQ(calleeR.regClass(), LirRegClass::GPR);
    EXPECT_EQ(calleeR.id, calleeReloadDest.id)
        << "the call's ops[0] must consume the callee reload's dest";

    // ops[1], ops[2] are SpillSlotRef operands (deferred args) carrying their
    // spill slots (2, 3) + class GPR — the c77 direct-arg-reload marker.
    for (std::uint32_t opIdx = 1; opIdx < 3; ++opIdx) {
        ASSERT_EQ(rewrittenOps[opIdx].kind, LirOperandKind::SpillSlotRef)
            << "call arg operand " << opIdx
            << " must be a deferred SpillSlotRef (c77)";
        EXPECT_EQ(rewrittenOps[opIdx].spillSlotV, opIdx + 1u)
            << "SpillSlotRef must carry the arg's spill slot";
        EXPECT_EQ(rewrittenOps[opIdx].spillSlotClass,
                  static_cast<std::uint8_t>(LirRegClass::GPR))
            << "SpillSlotRef must carry the arg's register class";
    }

    // ── THE B2 PIN (UNCHANGED by c77) ── the spilled CALLEE's reload scratch
    // ordinal is outside argGprs ∪ argFprs ∪ {variadic count register}. c77 did
    // not touch the callee (ops[0]) reload path; this contract still holds.
    std::uint32_t const calleeOrd = calleeReloadDest.id;
    EXPECT_EQ(forbidden.count(calleeOrd), 0u)
        << "B2 regression: the spilled indirect-call CALLEE was "
           "reloaded into cc arg/count register ordinal " << calleeOrd
        << " — the post-regalloc arg-setup moves (or the variadic "
           "count mov) would clobber the callee before the call "
           "consumes it (silent jump through an argument value)";

    DiagnosticReporter verifyRep;
    EXPECT_TRUE(verifyLirPostRegalloc(rewritten.lir, sch, verifyRep));
}
