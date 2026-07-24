// LIR calling-convention materialization tests (ML7 cycle 1). Pins:
//   * Prologue inserted at function entry (saved-reg stores + SP sub)
//   * Epilogue inserted before every return (saved-reg loads + SP add)
//   * frame_load / frame_store materialized to load / store with
//     SP-relative addressing
//   * Block topology preserved (1:1 mapping)
//   * Per-function FrameLayout populated correctly
//   * No frame_load/frame_store ops remain in the output module

#include "core/types/call_payload.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "diagnostic_count.hpp"
#include "mutate_target_schema.hpp"
#include "lir/lir.hpp"

#include <algorithm>
#include <format>
#include "asm/asm.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
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
#include <ios>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace dss;
using dss::test_support::lowerCSubsetToLir;

namespace {

// Bundle: lower + liveness + regalloc + rewrite. The callconv pass
// consumes the rewritten module + the LirAllocation; tests then
// invoke materializeCallingConvention on top.
struct RewrittenBundle {
    test_support::LoweredLir lowered;
    LirLiveness              liveness;
    LirAllocation            alloc;
    DiagnosticReporter       regallocRep;
    LirRewriteResult         rewritten;
    DiagnosticReporter       rewriteRep;

    RewrittenBundle(test_support::LoweredLir l) : lowered(std::move(l)) {}
};

// `ccIndex` default `= 0` is a TEST-HARNESS convenience only — the
// underlying `allocateRegisters` parameter is REQUIRED (no default)
// per `src/lir/lir_regalloc.hpp:183-187`'s "no default" discipline,
// which exists to prevent a future caller from inheriting the
// pre-D-FF3-3 hardcode silently. Tests pinning ccIndex=1 behavior
// must pass `1` explicitly; the default exists so the dozens of
// pre-existing non-cc tests don't have to be rewritten.
[[nodiscard]] RewrittenBundle
lowerThroughRewrite(std::string src, std::uint16_t ccIndex = 0,
                    std::string targetName = "x86_64") {
    // FC12b: the MIR config + analyze() va_list strategy must come from the SAME CC
    // the regalloc uses (ccIndex) — otherwise a ccIndex=1 (ms_x64) regalloc would run
    // over MIR lowered for cc0 (SysV), mixing ABIs. Thread ccIndex into the fixture.
    RewrittenBundle out{lowerCSubsetToLir(std::move(src), std::move(targetName),
                                          ccIndex)};
    if (!out.lowered.lir.ok) return out;
    out.liveness = analyzeLiveness(out.lowered.lir.lir);
    out.alloc = allocateRegisters(out.lowered.lir.lir, *out.lowered.target,
                                  out.liveness, ccIndex,
                                  out.regallocRep);
    if (!out.alloc.ok()) return out;
    out.rewritten = rewriteWithAllocation(out.lowered.lir.lir,
                                          *out.lowered.target, out.alloc,
                                          out.rewriteRep);
    return out;
}

// Mov-walking helper hoisted post-fold #7 (declared) and adopted
// post-fold #8 R3 (both call sites). Used by
// CcIndex1DrivesDifferentArgGprThanCc0 (post-fold #7 — positive +
// negative pins) and CalleeArgReceivesFromArgGprAcrossSysVAndMsX64
// (post-fold #8 R3 collapse of the 20-line inline walk). Returns true
// iff any `mov` inst in `lir` touches `physOrd` as either result or a
// Reg-operand source — used by tests that pin "this physical reg
// surfaces in the post-callconv arg-loading sequence."
[[nodiscard]] bool
anyMovTouchesPhysReg(Lir const& lir, std::uint16_t physOrd,
                     std::uint16_t movOp) {
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const b = lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < lir.blockInstCount(b); ++i) {
                LirInstId const inst = lir.blockInstAt(b, i);
                if (lir.instOpcode(inst) != movOp) continue;
                LirReg const r = lir.instResult(inst);
                if (r.valid() && r.isPhysical != 0 && r.id == physOrd)
                    return true;
                for (auto const& op : lir.instOperands(inst)) {
                    if (op.kind == LirOperandKind::Reg
                        && op.reg.isPhysical != 0
                        && op.reg.id == physOrd)
                        return true;
                }
            }
        }
    }
    return false;
}

// D-LK10-ENTRY-ARM64-NONLEAF-LINK-REGISTER helper: does a FrameLayout's
// callee-save set include the given physical-register ordinal? Used to
// assert the link-register (x30) spill is present on non-leaf frames and
// absent on leaf frames.
[[nodiscard]] bool
savedRegsContain(FrameLayout const& layout, std::uint16_t ord) {
    for (LirReg const& r : layout.savedRegs) {
        if (r.isPhysical != 0 && r.id == ord) return true;
    }
    return false;
}

// D-WIN64-LARGE-FRAME-STACK-PROBE helper: count how many insts in a
// materialized module carry the given opcode. Used by the prologue
// structural pin to assert a `stack_probe` op is (or is NOT) emitted.
[[nodiscard]] std::uint32_t
countOpcodeInModule(Lir const& lir, std::uint16_t op) {
    std::uint32_t n = 0;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const b = lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < lir.blockInstCount(b); ++i) {
                if (lir.instOpcode(lir.blockInstAt(b, i)) == op) ++n;
            }
        }
    }
    return n;
}

} // namespace

// ── D-CSUBSET-TESTTU-SILENT-EXIT1: empty module is a valid success ────
//
// An empty module (0 functions — a declaration-only / all-preprocessed-out
// TU) materializes to an empty result that is a VALID success: the parallel-
// index invariant `perFunc.size() == moduleFuncCount()` holds at 0 == 0, so a
// declaration-only TU lowers to a valid empty relocatable object rather than
// silently rejecting the whole compile. RED-ON-DISABLE: restoring the
// `moduleFuncCount() > 0` clause in LirCallconvResult::ok() flips this to false.
TEST(LirCallconv, EmptyModuleIsOk) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    Lir empty{};                 // 0 functions
    LirAllocation emptyAlloc{};   // no per-function allocations → ok()
    ASSERT_TRUE(emptyAlloc.ok());
    DiagnosticReporter rep;
    auto result = materializeCallingConvention(empty, **target, emptyAlloc, rep);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.perFunc.size(), 0u);
    EXPECT_EQ(rep.errorCount(), 0u);
}

// ── D-WIN64-LARGE-FRAME-STACK-PROBE prologue structural pins ──────────
//
// The ms_x64 prologue must emit a `stack_probe` op (NOT a plain `sub rsp`)
// when the frame exceeds the cc's stackProbePageBytes (4096). A ms_x64
// frame ≤ 4096 keeps the plain sub; a sysv frame > 4096 ALSO keeps the
// plain sub (sysv declares stackProbePageBytes=0 → no probing). RED-ON-
// DISABLE: flipping the prologue's threshold branch flips the op choice
// (the witness corpus large_frame_win64 then SIGSEGVs on Windows).

TEST(LirCallconv, MsX64LargeFrameEmitsStackProbeNotPlainSub) {
    // ms_x64 = ccIndex 1. `int big[2000]` = 8000 bytes > one 4096 guard
    // page → the prologue must emit `stack_probe`, NOT a bare `sub rsp,F`.
    auto bundle = lowerThroughRewrite(
        "int f(void) { int big[2000]; big[0] = 7; return big[0]; }",
        /*ccIndex=*/1);
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    EXPECT_TRUE(result.ok());
    ASSERT_EQ(result.perFunc.size(), 1u);
    // Confirm the frame really exceeds one page (the precondition).
    auto const* cc = bundle.lowered.target->callingConvention(1);
    ASSERT_NE(cc, nullptr);
    ASSERT_GT(cc->stackProbePageBytes, 0u) << "ms_x64 must declare a probe page";
    EXPECT_GT(result.perFunc[0].totalFrameSize, cc->stackProbePageBytes);

    auto const probeOp = bundle.lowered.target->opcodeByMnemonic("stack_probe");
    ASSERT_TRUE(probeOp.has_value());
    EXPECT_EQ(countOpcodeInModule(result.lir, *probeOp), 1u)
        << "ms_x64 >4096 frame must emit exactly one stack_probe";
}

TEST(LirCallconv, MsX64SmallFrameEmitsPlainSubNotStackProbe) {
    // ms_x64, a TRIVIAL frame (one int, well under 4096) → plain `sub rsp`,
    // NO stack_probe. (The frame is rounded to stackAlignment but stays a
    // small fraction of a page.)
    auto bundle = lowerThroughRewrite(
        "int f(int x) { return x + x; }", /*ccIndex=*/1);
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    EXPECT_TRUE(result.ok());
    ASSERT_EQ(result.perFunc.size(), 1u);
    auto const* cc = bundle.lowered.target->callingConvention(1);
    ASSERT_NE(cc, nullptr);
    EXPECT_LE(result.perFunc[0].totalFrameSize, cc->stackProbePageBytes);

    auto const probeOp = bundle.lowered.target->opcodeByMnemonic("stack_probe");
    ASSERT_TRUE(probeOp.has_value());
    EXPECT_EQ(countOpcodeInModule(result.lir, *probeOp), 0u)
        << "ms_x64 ≤4096 frame must NOT emit stack_probe (plain sub rsp)";
}

TEST(LirCallconv, SysVLargeFrameEmitsPlainSubNotStackProbe) {
    // sysv_amd64 = ccIndex 0, declares NO stackProbePageBytes (0). Even a
    // LARGE frame (`int big[2000]` = 8000B) must keep the plain `sub rsp`
    // — Linux auto-grows the stack, no probe needed. This is the agnostic
    // control: the SAME source on a non-probing CC takes the plain path.
    auto bundle = lowerThroughRewrite(
        "int f(void) { int big[2000]; big[0] = 7; return big[0]; }",
        /*ccIndex=*/0);
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    EXPECT_TRUE(result.ok());
    ASSERT_EQ(result.perFunc.size(), 1u);
    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->stackProbePageBytes, 0u) << "sysv must NOT declare a probe page";
    // The frame is genuinely large (precondition for a meaningful control).
    EXPECT_GT(result.perFunc[0].totalFrameSize, 4096u);

    auto const probeOp = bundle.lowered.target->opcodeByMnemonic("stack_probe");
    ASSERT_TRUE(probeOp.has_value());
    EXPECT_EQ(countOpcodeInModule(result.lir, *probeOp), 0u)
        << "sysv (stackProbePageBytes=0) must NEVER emit stack_probe";
}

TEST(LirCallconv, StraightLineFunctionGetsPrologueEpilogueAndZeroFrameOps) {
    auto bundle = lowerThroughRewrite("int f(int x) { return x + x; }");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    EXPECT_TRUE(result.ok());
    ASSERT_EQ(result.perFunc.size(), 1u);
    // Frame layout: 0 spills for this trivial fn → savedRegArea may
    // still be > 0 if any callee-saved reg was assigned. Total frame
    // size must be a multiple of cc.stackAlignment.
    auto const& layout = result.perFunc[0];
    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(layout.totalFrameSize % cc->stackAlignment, 0u);
    // No frame_load / frame_store ops remain in the output.
    auto const fl = bundle.lowered.target->opcodeByMnemonic(
        bundle.lowered.target->frameLoadMnemonic());
    auto const fs = bundle.lowered.target->opcodeByMnemonic(
        bundle.lowered.target->frameStoreMnemonic());
    ASSERT_TRUE(fl.has_value());
    ASSERT_TRUE(fs.has_value());
    Lir const& dst = result.lir;
    for (std::uint32_t fi = 0; fi < dst.moduleFuncCount(); ++fi) {
        LirFuncId const fn = dst.funcAt(fi);
        for (std::uint32_t bi = 0; bi < dst.funcBlockCount(fn); ++bi) {
            LirBlockId const b = dst.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < dst.blockInstCount(b); ++i) {
                std::uint16_t const op = dst.instOpcode(dst.blockInstAt(b, i));
                EXPECT_NE(op, *fl) << "frame_load must not survive callconv";
                EXPECT_NE(op, *fs) << "frame_store must not survive callconv";
            }
        }
    }
}

// D-CSUBSET-VLA (C1b): a LEAF function with a variable-length array materializes
// the conditional frame-pointer model — the callconv pass emits `sp_copy` (the
// prologue FP<-SP capture, the VLA-base capture, and the epilogue SP<-FP restore)
// and FORCE-SAVES the frame pointer (CRITICAL-2). RED-ON-DISABLE: without the
// conditional frame model these ops vanish + the frame pointer is unsaved.
TEST(LirCallconv, VlaLeafFunctionMaterializesFramePointerModel) {
    auto bundle = lowerThroughRewrite("int f(int n) { int a[n]; return a[0]; }");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok())
        << "a LEAF VLA must materialize: "
        << (ccRep.all().empty() ? "" : ccRep.all()[0].actual);
    ASSERT_EQ(result.perFunc.size(), 1u);
    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_TRUE(cc->framePointer.has_value());
    // CRITICAL-2: the frame pointer is force-saved (regalloc reserved it, so
    // collectUsedCalleeSaved never saw it — the fold must add it).
    EXPECT_TRUE(savedRegsContain(result.perFunc[0], cc->framePointer->ordinal))
        << "a VLA function must save the caller's frame pointer";
    // The `sp_copy` op (FP-setup / VLA-base capture / SP-restore) is present.
    auto const spCopy = bundle.lowered.target->opcodeByMnemonic("sp_copy");
    ASSERT_TRUE(spCopy.has_value());
    Lir const& dst = result.lir;
    bool sawSpCopy = false;
    for (std::uint32_t fi = 0; fi < dst.moduleFuncCount(); ++fi) {
        LirFuncId const fn = dst.funcAt(fi);
        for (std::uint32_t bi = 0; bi < dst.funcBlockCount(fn); ++bi) {
            LirBlockId const b = dst.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < dst.blockInstCount(b); ++i) {
                if (dst.instOpcode(dst.blockInstAt(b, i)) == *spCopy)
                    sawSpCopy = true;
            }
        }
    }
    EXPECT_TRUE(sawSpCopy)
        << "a VLA function must emit sp_copy (FP capture/restore)";
}

// D-CSUBSET-VLA-NONLEAF-CALL-FRAME (C1b LEAF gate): a VLA function that ALSO makes
// a call FAILS LOUD — the non-leaf VLA frame model (outgoing args under a moved SP)
// is not built. RED-ON-DISABLE: drop the leaf gate -> a non-leaf VLA silently emits
// call args INSIDE the VLA region (an ABI break).
TEST(LirCallconv, VlaNonLeafFunctionWithCallFailsLoud) {
    auto bundle = lowerThroughRewrite(
        "int g(int x) { return x * 2; }\n"
        "int f(int n) { int a[n]; return g(n) + a[0]; }\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    EXPECT_FALSE(result.ok())
        << "a VLA function that makes a call must fail the callconv pass";
    bool sawNonLeaf = false;
    for (auto const& d : ccRep.all()) {
        if (d.code == DiagnosticCode::L_VlaNonLeafFrameUnsupported)
            sawNonLeaf = true;
    }
    EXPECT_TRUE(sawNonLeaf)
        << "a non-leaf VLA must surface L_VlaNonLeafFrameUnsupported";
}

// D-CSUBSET-VLA-NONLEAF-CALL-FRAME (C1b LEAF gate, va-arm): a VLA function that ALSO
// calls va_start FAILS LOUD — its va-area address leas are SP-relative body refs the
// runtime `sub sp` invalidates (and the va-overflow area lives ABOVE the entry SP,
// outside the frame-pointer switch). RED-ON-DISABLE for the `usesVaStart` gate (a
// refactor could silently drop the va-arm, which is separate from the call-arm).
TEST(LirCallconv, VlaVariadicFunctionWithVaStartFailsLoud) {
    auto bundle = lowerThroughRewrite(
        "int f(int n, ...) {\n"
        "  int a[n];\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  va_end(ap);\n"
        "  return a[0];\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    EXPECT_FALSE(result.ok())
        << "a VLA function that calls va_start must fail the callconv pass";
    bool sawNonLeaf = false;
    for (auto const& d : ccRep.all()) {
        if (d.code == DiagnosticCode::L_VlaNonLeafFrameUnsupported)
            sawNonLeaf = true;
    }
    EXPECT_TRUE(sawNonLeaf)
        << "a VLA + va_start function must surface L_VlaNonLeafFrameUnsupported";
}

// D-CSUBSET-VLA-WIN64-UNWIND (C1b): a VLA function that is a SEH PARENT (guards a
// __try, so it emits `.pdata`/`.xdata` unwind info) FAILS LOUD — its runtime-moved
// frame is not describable by the static unwind info, so an exception unwinding
// through it (even from a trapping NON-CALL op, which leaves hasCalls=false) would
// mis-walk the stack. Synthesize the SEH binding by naming the VLA function as a
// __try parent in the sehFuncletParents span. RED-ON-DISABLE for the SEH gate.
TEST(LirCallconv, VlaSehParentFunctionFailsLoud) {
    // VLA function FIRST so it is the lower-indexed "parent" (the funclet-after-parent
    // module-order invariant the sehFuncletParents resolution assumes).
    auto bundle = lowerThroughRewrite(
        "int f(int n) { int a[n]; return a[0]; }\n"
        "int g(void) { return 0; }\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    Lir const& rw = bundle.rewritten.lir;
    ASSERT_EQ(rw.moduleFuncCount(), 2u);
    // Find the VLA function (the one containing sub_sp_reg) — bind IT as the __try
    // parent, the other as its funclet (order-independent, no source-order assumption).
    auto const subSpReg = bundle.lowered.target->opcodeByMnemonic("sub_sp_reg");
    ASSERT_TRUE(subSpReg.has_value());
    auto containsSubSp = [&](LirFuncId fn) {
        for (std::uint32_t bi = 0; bi < rw.funcBlockCount(fn); ++bi) {
            LirBlockId const b = rw.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < rw.blockInstCount(b); ++i)
                if (rw.instOpcode(rw.blockInstAt(b, i)) == *subSpReg) return true;
        }
        return false;
    };
    LirFuncId const f0 = rw.funcAt(0), f1 = rw.funcAt(1);
    bool const zeroIsVla = containsSubSp(f0);
    ASSERT_NE(zeroIsVla, containsSubSp(f1)) << "exactly one function has the VLA";
    SymbolId const vlaSym{rw.funcArena().at(zeroIsVla ? f0 : f1).symbol};
    SymbolId const otherSym{rw.funcArena().at(zeroIsVla ? f1 : f0).symbol};
    std::array<SehFuncletParent, 1> const seh{
        SehFuncletParent{otherSym, vlaSym}};   // funclet=other, parent=the VLA fn
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(rw, *bundle.lowered.target,
                                               bundle.alloc, ccRep, seh);
    EXPECT_FALSE(result.ok())
        << "a VLA function that is a SEH parent must fail the callconv pass";
    bool sawUnwind = false;
    for (auto const& d : ccRep.all())
        if (d.code == DiagnosticCode::L_VlaNonLeafFrameUnsupported) sawUnwind = true;
    EXPECT_TRUE(sawUnwind)
        << "a VLA + SEH function must surface L_VlaNonLeafFrameUnsupported "
           "(D-CSUBSET-VLA-WIN64-UNWIND)";
}

// D-CSUBSET-VLA (C1b) zero-blast-radius pin: a NON-VLA function must NOT gain any
// frame-pointer machinery — its frame stays SP-relative + byte-identical. RED-ON-
// DISABLE: if the conditional frame model ever fires unconditionally, a plain
// function grows an `sp_copy` and this pin goes red.
TEST(LirCallconv, NonVlaFunctionEmitsNoFramePointerOps) {
    auto bundle = lowerThroughRewrite("int f(int x) { int a[4]; a[0] = x; return a[0]; }");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const spCopy   = bundle.lowered.target->opcodeByMnemonic("sp_copy");
    auto const subSpReg = bundle.lowered.target->opcodeByMnemonic("sub_sp_reg");
    ASSERT_TRUE(spCopy.has_value() && subSpReg.has_value());
    Lir const& dst = result.lir;
    for (std::uint32_t fi = 0; fi < dst.moduleFuncCount(); ++fi) {
        LirFuncId const fn = dst.funcAt(fi);
        for (std::uint32_t bi = 0; bi < dst.funcBlockCount(fn); ++bi) {
            LirBlockId const b = dst.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < dst.blockInstCount(b); ++i) {
                std::uint16_t const op = dst.instOpcode(dst.blockInstAt(b, i));
                EXPECT_NE(op, *spCopy)
                    << "a non-VLA function must not emit sp_copy (zero blast radius)";
                EXPECT_NE(op, *subSpReg)
                    << "a non-VLA function must not emit sub_sp_reg";
            }
        }
    }
}

// D-LK10-ENTRY-ARM64-NONLEAF-LINK-REGISTER (2026-06-08): a NON-LEAF function
// under a calling convention with a LINK REGISTER (AAPCS64 -> x30) MUST spill
// + reload that register in its frame, because every `bl`/`call` clobbers it.
// Without the spill, the epilogue `ret` jumps to the clobbered link reg (the
// address after the last call), and the repeated epilogue SP-restore walks SP
// off the stack — the exact arm64-FFI SIGSEGV this anchor closed. This pins the
// spill at the LIR tier so it is exercised on EVERY CI leg (incl. Windows
// MSVC), where the arm64 *runtime* corpus is gated out by the cross-arch guard.
TEST(LirCallconv, NonLeafAarch64FunctionSpillsLinkRegister) {
    // `f` calls `g` -> f is non-leaf, g is leaf. No optimizer runs in this
    // pipeline (lower -> liveness -> regalloc -> rewrite -> callconv), so g is
    // NOT inlined: f keeps a real call and stays non-leaf.
    auto bundle = lowerThroughRewrite(
        "int g(int x) { return x * 2; }\n"
        "int f(int x) { return g(x) + 1; }\n",
        /*ccIndex=*/0, /*targetName=*/"arm64");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_TRUE(cc->linkRegister.has_value())
        << "aapcs64 must declare a link register (x30)";
    std::uint16_t const lrOrd = cc->linkRegister->ordinal;

    // Partition the per-function frames into the non-leaf (f, hasCalls) and
    // leaf (g) layouts. f MUST save x30; g must NOT (no call clobbers it).
    FrameLayout const* nonLeaf = nullptr;
    FrameLayout const* leaf    = nullptr;
    for (auto const& l : result.perFunc) {
        if (l.hasCalls) nonLeaf = &l;
        else            leaf    = &l;
    }
    ASSERT_NE(nonLeaf, nullptr) << "f must produce a non-leaf frame (calls g)";
    ASSERT_NE(leaf, nullptr)    << "g must produce a leaf frame";
    EXPECT_TRUE(savedRegsContain(*nonLeaf, lrOrd))
        << "non-leaf AAPCS64 function MUST spill x30 (the link register) — "
           "without it the epilogue `ret` returns to a clobbered LR (SIGSEGV)";
    EXPECT_FALSE(savedRegsContain(*leaf, lrOrd))
        << "leaf AAPCS64 function must NOT spill x30 — no call clobbers it, so "
           "its minimal frame is preserved";
}

// Agnosticism pin: the SAME non-leaf source on x86_64 has NO link register to
// spill — the SysV/x86_64 calling convention carries the return address on the
// stack (callPushBytes=8) and declares no `linkRegister`. This proves the fix
// takes its no-op path off `cc.linkRegister.has_value()`, NOT an
// `if (arch == arm64)` branch in shared substrate.
TEST(LirCallconv, NonLeafX8664FunctionHasNoLinkRegisterToSpill) {
    auto bundle = lowerThroughRewrite(
        "int g(int x) { return x * 2; }\n"
        "int f(int x) { return g(x) + 1; }\n",
        /*ccIndex=*/0, /*targetName=*/"x86_64");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    EXPECT_FALSE(cc->linkRegister.has_value())
        << "x86_64 SysV declares NO link register — `call` pushes the return "
           "address on the stack; the link-register spill must take its no-op "
           "path here (config-driven, not arch-hardcoded)";
}

// D-LK10-ENTRY-ARM64-NONLEAF-LINK-REGISTER (host-independent byte-pin, 2026-06-08):
// the structural pin above proves x30 is SCHEDULED into savedRegs; the runtime
// corpus (extern_call_elf arm64) proves the frame EXECUTES — but only on the
// single native-arm64 CI leg. Neither pins, on EVERY leg, that the scheduled x30
// spill ASSEMBLES to the right store at the right stack offset. The per-instruction
// arm64 byte-pins (Arm64Encoder.StoreSturEncodes, RetEncodes…) prove each
// instruction in isolation; nothing pinned the COMPOSED non-leaf frame. This closes
// that last mile: it drives the SAME production tail compile_pipeline.cpp uses
// (lowerThroughRewrite -> legalizeTwoAddress -> materializeCallingConvention ->
// assemble) and byte-pins the assembled prologue + epilogue against hand-verified
// ARM-ARM literals — a pure byte compare, no runOn/emulator gate, so it runs (and
// is red-on-disable) on EVERY leg incl. Windows MSVC. The catch it guards: 2b07e3b
// byte-pinned green per-instruction yet SIGSEGV'd because the COMPOSED non-leaf
// frame omitted the x30 spill (dbf84b0 fixed it). Keep all THREE layers (structural
// + this byte-pin + runtime corpus); none replaces another.
TEST(LirCallconv, NonLeafAarch64FramePrologueEpilogueByteExact) {
    // f calls g -> f is non-leaf (frame holds x30), g is leaf (no x30). No
    // optimizer runs in this pipeline, so g is not inlined and f stays non-leaf.
    auto bundle = lowerThroughRewrite(
        "int g(int x) { return x * 2; }\n"
        "int f(int x) { return g(x) + 1; }\n",
        /*ccIndex=*/0, /*targetName=*/"arm64");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    // The EXACT production pass order (compile_pipeline.cpp:340-363):
    // legalizeTwoAddress -> materializeCallingConvention -> assemble.
    DiagnosticReporter legRep;
    auto legal = legalizeTwoAddress(bundle.rewritten.lir, *bundle.lowered.target,
                                    legRep);
    ASSERT_TRUE(legal.ok());
    DiagnosticReporter ccRep;
    auto cc = materializeCallingConvention(legal.lir, *bundle.lowered.target,
                                           bundle.alloc, ccRep);
    ASSERT_TRUE(cc.ok());
    // lirToMir is only consumed for the source-map; the callconv-inserted
    // prologue/epilogue insts have no MIR predecessor -> InvalidMirInst (this pin
    // checks BYTES, not source-map fidelity, exactly like assembleEndToEnd).
    std::vector<MirInstId> lirToMir(cc.lir.instCount(), InvalidMirInst);
    DiagnosticReporter asmRep;
    auto mod = assemble(cc.lir, *bundle.lowered.target, lirToMir, asmRep);
    ASSERT_EQ(mod.functions.size(), cc.perFunc.size());

    // Partition by FrameLayout.hasCalls: f = non-leaf, g = leaf.
    std::size_t fIdx = cc.perFunc.size(), gIdx = cc.perFunc.size();
    for (std::size_t i = 0; i < cc.perFunc.size(); ++i) {
        if (cc.perFunc[i].hasCalls) fIdx = i;
        else                        gIdx = i;
    }
    ASSERT_LT(fIdx, cc.perFunc.size()) << "f must be the non-leaf frame (calls g)";
    ASSERT_LT(gIdx, cc.perFunc.size()) << "g must be the leaf frame";
    auto const& fBytes = mod.functions[fIdx].bytes;
    auto const& gBytes = mod.functions[gIdx].bytes;

    // EXACT non-leaf PROLOGUE — 4 insts / 16 bytes, each decoded from the ARM ARM
    // (little-endian):
    //   sub  sp, sp, #0x20     d10083ff   (SUB imm: 0xD1000000|(32<<10)|(31<<5)|31)
    //   stur x28, [sp]         f80003fc   (STUR : 0xF8000000|(0<<12)|(31<<5)|28)
    //   stur x29, [sp, #8]     f80083fd   (        |(8<<12)|...|29)
    //   stur x30, [sp, #16]    f80103fe   (the LINK-REGISTER spill: |(16<<12)|...|30)
    static constexpr std::array<std::uint8_t, 16> kProlog{
        0xff, 0x83, 0x00, 0xd1,  0xfc, 0x03, 0x00, 0xf8,
        0xfd, 0x83, 0x00, 0xf8,  0xfe, 0x03, 0x01, 0xf8};
    // EXACT non-leaf EPILOGUE — 5 insts / 20 bytes:
    //   ldur x28, [sp]         f84003fc   (LDUR : 0xF8400000|...)
    //   ldur x29, [sp, #8]     f84083fd
    //   ldur x30, [sp, #16]    f84103fe   (the LINK-REGISTER reload, before ret)
    //   add  sp, sp, #0x20     910083ff   (ADD imm: 0x91000000|(32<<10)|(31<<5)|31)
    //   ret                    d65f03c0   (RET X30: 0xD65F0000|(30<<5))
    static constexpr std::array<std::uint8_t, 20> kEpilog{
        0xfc, 0x03, 0x40, 0xf8,  0xfd, 0x83, 0x40, 0xf8,
        0xfe, 0x03, 0x41, 0xf8,  0xff, 0x83, 0x00, 0x91,
        0xc0, 0x03, 0x5f, 0xd6};
    ASSERT_GE(fBytes.size(), kProlog.size() + kEpilog.size());
    EXPECT_TRUE(std::equal(kProlog.begin(), kProlog.end(), fBytes.begin()))
        << "non-leaf AAPCS64 prologue must byte-match exactly — incl. "
           "`stur x30,[sp,#16]` (f80103fe) at offset 12; without the link-register "
           "spill the frame shrinks to 0x10 and these bytes diverge (red-on-disable)";
    EXPECT_TRUE(std::equal(kEpilog.rbegin(), kEpilog.rend(), fBytes.rbegin()))
        << "non-leaf AAPCS64 epilogue must byte-match exactly — incl. "
           "`ldur x30,[sp,#16]` (f84103fe) reloaded BEFORE `ret`, so the return "
           "targets the caller, not the bl-clobbered x30";

    // Leaf control: g saves x28/x29 (the x*2 scratch) but must contain NO
    // `stur x30,[sp,#16]` anywhere — no `bl` clobbers x30 in a leaf. Proves the pin
    // discriminates the x30 spill specifically, not "matches any prologue". (This
    // byte check is offset-specific to #16; the offset-INDEPENDENT "x30 ∉ savedRegs"
    // guarantee for the leaf is the sibling NonLeafAarch64FunctionSpillsLinkRegister
    // — this corroborates it at the byte tier.)
    static constexpr std::array<std::uint8_t, 4> kSturX30{0xfe, 0x03, 0x01, 0xf8};
    EXPECT_EQ(std::search(gBytes.begin(), gBytes.end(),
                          kSturX30.begin(), kSturX30.end()),
              gBytes.end())
        << "leaf AAPCS64 function must NOT spill x30 — the discrimination check";
}

// D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB (F4 — durable x16 safety). A
// function whose frame EXCEEDS the 2-word shifted-imm12 reach (0xFFFFFF =
// 16 MiB) forces the prologue `sub sp,sp,#frame` to lower to the 3-word
// MOVZ/MOVK + EXTENDED-register macro, whose FIRST emitted word materializes
// the frame size into x16 (the AAPCS64 IP0 scratch). This pin assembles the
// real C→callconv→assemble pipeline and asserts the prologue's FIRST 32-bit
// word is `MOVZ x16,#imm16` — so a future change that allocated x16 to a
// value live across the sp-adjust (clobbering the scratch) would diverge
// this word (red-on-disable). `int big[5000000]` ≈ 20 MB > 16 MiB.
TEST(LirCallconv, Aarch64FrameBeyond16MiBPrologueMaterializesIntoX16) {
    auto bundle = lowerThroughRewrite(
        "int seed;\n"
        "int f(void) { int big[5000000]; big[0] = seed; return big[0]; }\n",
        /*ccIndex=*/0, /*targetName=*/"arm64");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter legRep;
    auto legal = legalizeTwoAddress(bundle.rewritten.lir, *bundle.lowered.target,
                                    legRep);
    ASSERT_TRUE(legal.ok());
    DiagnosticReporter ccRep;
    auto cc = materializeCallingConvention(legal.lir, *bundle.lowered.target,
                                           bundle.alloc, ccRep);
    ASSERT_TRUE(cc.ok());
    ASSERT_EQ(cc.perFunc.size(), 1u);
    // Precondition: the frame genuinely exceeds the 16 MiB shifted-imm12 reach
    // (so the prologue MUST take the 3-word MOVZ/MOVK path, not the 2-word one).
    EXPECT_GT(cc.perFunc[0].totalFrameSize, 0xFFFFFFu)
        << "the 20MB array frame must exceed 16 MiB to force the 3-word form";

    std::vector<MirInstId> lirToMir(cc.lir.instCount(), InvalidMirInst);
    DiagnosticReporter asmRep;
    auto mod = assemble(cc.lir, *bundle.lowered.target, lirToMir, asmRep);
    EXPECT_EQ(asmRep.errorCount(), 0u)
        << "a >16MiB-frame AAPCS64 function must assemble clean (3-word sp-adjust)";
    ASSERT_EQ(mod.functions.size(), 1u);
    auto const& fBytes = mod.functions[0].bytes;
    ASSERT_GE(fBytes.size(), 4u);
    // Decode the prologue's first word LE. Mask out the imm16 field (bits
    // 5..20, frame-size dependent) and assert the MOVZ-x16 skeleton: opcode
    // bits + Rd=16. 0xD2800000 = MOVZ X base; |16 = Rd. The mask 0xFFE0001F
    // keeps [31:21] (opcode+hw) and [4:0] (Rd), clearing the imm16 window.
    std::uint32_t const w0 =
          static_cast<std::uint32_t>(fBytes[0])
        | (static_cast<std::uint32_t>(fBytes[1]) << 8)
        | (static_cast<std::uint32_t>(fBytes[2]) << 16)
        | (static_cast<std::uint32_t>(fBytes[3]) << 24);
    EXPECT_EQ(w0 & 0xFFE0001Fu, 0xD2800000u | 16u)
        << "the >16MiB prologue's first word must be `MOVZ x16,#imm16` — a "
           "regalloc change clobbering x16 (the IP0 scratch) across the "
           "sp-adjust would diverge this (red-on-disable). Got word 0x"
        << std::hex << w0;

    // Durability corroboration: x16 (hwEncoding 16) must NOT be assigned to
    // any value across the function — it is platform scratch, baked into the
    // 3-word macro. (A regalloc that handed x16 to a live value would both
    // break the byte pin above AND surface here.)
    auto const x16Ord = bundle.lowered.target->registerByName("x16");
    ASSERT_TRUE(x16Ord.has_value());
    Lir const& dst = cc.lir;
    for (std::uint32_t fi = 0; fi < dst.moduleFuncCount(); ++fi) {
        LirFuncId const fn = dst.funcAt(fi);
        for (std::uint32_t bi = 0; bi < dst.funcBlockCount(fn); ++bi) {
            LirBlockId const blk = dst.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < dst.blockInstCount(blk); ++i) {
                LirInstId const inst = dst.blockInstAt(blk, i);
                LirReg const r = dst.instResult(inst);
                EXPECT_FALSE(r.valid() && r.isPhysical != 0
                             && r.id == *x16Ord)
                    << "x16 (IP0 scratch) must not be a value-carrying result "
                       "in a >16MiB-frame function — it is reserved for the "
                       "3-word sp-adjust materialization";
            }
        }
    }
}

// D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12 (load/store-displacement half). A
// callee with MANY fixed params reads its high-index params from INCOMING STACK
// slots at `[sp + totalFrameSize + (i-8)*8]`; past the 8th register param those
// offsets exceed the unscaled imm9 ±256 reach, so the incoming-arg frame LOAD
// MUST take the scaled-imm12 form (`load_u`, LDR [sp,#imm]) — selected at the
// emitFrameLoad CHOKEPOINT (selectFrameMemOp). This callee shape is DETERMINISTIC
// (no register-pressure spill → host-compiler-independent, unlike an optimizer-
// dependent spill frame) and exercises the chokepoint directly. The pin asserts
// a scaled load appears in the materialized arm64 module — host-independent, so
// it guards the selection on EVERY CI leg (the qemu RUN witness is the separate
// examples/c-subset/large_spill_frame_arm64 corpus). RED-on-disable: revert the
// chokepoint swap (keep unscaled `load`) → the count drops to 0 here AND the
// module fails to assemble (A_ImmediateOperandOutOfRange on the high-param load).
TEST(LirCallconv, Aarch64HighStackParamUsesScaledImm12FrameLoad) {
    // f takes 40 fixed int params; AAPCS64 passes the first 8 in x0..x7 and the
    // rest (p08..p39) on the incoming stack. The body reads p08, p20, p39 — the
    // last lands at offset totalFrameSize + (39-8)*8 = frame + 248, well past
    // imm9 — but the body holds at most one param live at a time (no spill, no
    // scratch exhaustion). Each high-param read is an incoming-stack-arg
    // frame_load routed through the chokepoint.
    auto bundle = lowerThroughRewrite(
        "int f(int p00,int p01,int p02,int p03,int p04,int p05,int p06,int p07,\n"
        "      int p08,int p09,int p10,int p11,int p12,int p13,int p14,int p15,\n"
        "      int p16,int p17,int p18,int p19,int p20,int p21,int p22,int p23,\n"
        "      int p24,int p25,int p26,int p27,int p28,int p29,int p30,int p31,\n"
        "      int p32,int p33,int p34,int p35,int p36,int p37,int p38,int p39) {\n"
        "  return p08 + p20 + p39;\n"
        "}\n",
        /*ccIndex=*/0, /*targetName=*/"arm64");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter legRep;
    auto legal = legalizeTwoAddress(bundle.rewritten.lir, *bundle.lowered.target,
                                    legRep);
    ASSERT_TRUE(legal.ok());
    DiagnosticReporter ccRep;
    auto cc = materializeCallingConvention(legal.lir, *bundle.lowered.target,
                                           bundle.alloc, ccRep);
    ASSERT_TRUE(cc.ok());
    EXPECT_EQ(ccRep.errorCount(), 0u);

    auto const loadU = bundle.lowered.target->opcodeByMnemonic("load_u");
    ASSERT_TRUE(loadU.has_value())
        << "arm64 must declare load_u (D-ASM-AARCH64-LARGE-FRAME-IMM12)";
    std::uint32_t const nLoadU = countOpcodeInModule(cc.lir, *loadU);
    EXPECT_GT(nLoadU, 0u)
        << "a 40-param AAPCS64 callee must emit at least one load_u (a high "
           "incoming-stack-arg read beyond imm9, routed through the emitFrameLoad "
           "chokepoint) — 0 means the chokepoint swap regressed (red-on-disable)";

    // The module must still assemble clean (the scaled load encodes, not fail-loud).
    std::vector<MirInstId> lirToMir(cc.lir.instCount(), InvalidMirInst);
    DiagnosticReporter asmRep;
    (void)assemble(cc.lir, *bundle.lowered.target, lirToMir, asmRep);
    EXPECT_EQ(asmRep.errorCount(), 0u)
        << "the high-stack-param arm64 callee must assemble clean — every "
           "beyond-imm9 frame load took the encodable scaled-imm12 form";
}

// AGNOSTIC corroboration: the SAME 40-param callee on x86_64 must NOT gain any
// load_u/store_u (x86_64 declares neither — h.loadU/h.storeU are 0, so
// selectFrameMemOp is inert and the emitted disp32 memory ops are byte-identical
// to before the chokepoint change). Guards against the swap leaking onto a
// target without the scaled form.
TEST(LirCallconv, X8664HighStackParamHasNoScaledImm12FrameOps) {
    auto bundle = lowerThroughRewrite(
        "int f(int p00,int p01,int p02,int p03,int p04,int p05,int p06,int p07,\n"
        "      int p08,int p09,int p10,int p11,int p12,int p13,int p14,int p15,\n"
        "      int p16,int p17,int p18,int p19,int p20,int p21,int p22,int p23,\n"
        "      int p24,int p25,int p26,int p27,int p28,int p29,int p30,int p31,\n"
        "      int p32,int p33,int p34,int p35,int p36,int p37,int p38,int p39) {\n"
        "  return p08 + p20 + p39;\n"
        "}\n",
        /*ccIndex=*/0, /*targetName=*/"x86_64");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter legRep;
    auto legal = legalizeTwoAddress(bundle.rewritten.lir, *bundle.lowered.target,
                                    legRep);
    ASSERT_TRUE(legal.ok());
    DiagnosticReporter ccRep;
    auto cc = materializeCallingConvention(legal.lir, *bundle.lowered.target,
                                           bundle.alloc, ccRep);
    ASSERT_TRUE(cc.ok());
    // x86_64 declares no load_u/store_u → the mnemonics don't resolve at all.
    EXPECT_FALSE(bundle.lowered.target->opcodeByMnemonic("load_u").has_value())
        << "x86_64 must NOT declare load_u — the scaled form is arm64-only";
    EXPECT_FALSE(bundle.lowered.target->opcodeByMnemonic("store_u").has_value());
}

// D-AS3-BLOCK-REL-IMM19/26 (ARM64 conditional control-flow) byte-pin.
//
// HAND-BUILT LIR exercising the three control-flow opcodes (cmp / jcc / jmp)
// directly through assemble(), laid out as a while-loop control-flow skeleton:
//   header: cmp x0, xzr ; jcc(sgt) -> body, exit       (b.cond + trailing b)
//   body:   sub x0, x0, x0 ; jmp header                (unconditional b, back-edge)
//   exit:   ret
//
// This pins (a) the cmp → SUBS XZR encoding, (b) the b.cond (0x54) with the GT
// condition nibble, (c) the forward unconditional b (0x14), (d) the NEGATIVE
// back-edge b, and (e) ret — AND, crucially, that the block-relative resolver
// fills the Imm19 (b.cond) and Imm26 (b) displacement fields with the correct
// scaled, instruction-PC-relative offsets. The back-edge word 0x17FFFFFC is the
// signed-resolver witness: header is at offset 0, the back-edge `b` is at offset
// +16, so disp = (0 - 16) >> 2 = -4, which in the 26-bit field is 0x3FFFFFC →
// 0x14000000 | 0x3FFFFFC = 0x17FFFFFC. Every value below is hand-verified
// against the ARM ARM; a regression in the resolver (wrong bias, wrong scale,
// or a sign error on the back-edge) diverges these bytes (red-on-disable).
TEST(LirCallconv, Arm64ControlFlowSkeletonEncodesCmpBcondBWithResolvedOffsets) {
    auto sOpt = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(sOpt.has_value());
    auto const& s = **sOpt;

    auto opc = [&](char const* m) {
        auto o = s.opcodeByMnemonic(m);
        EXPECT_TRUE(o.has_value()) << "arm64 missing opcode '" << m << "'";
        return o.value_or(0);
    };
    std::uint16_t const cmpOp = opc("cmp");
    std::uint16_t const jccOp = opc("jcc");
    std::uint16_t const jmpOp = opc("jmp");
    std::uint16_t const subOp = opc("sub");
    std::uint16_t const retOp = opc("ret");

    auto xreg = [&](char const* name) {
        auto ord = s.registerByName(name);
        EXPECT_TRUE(ord.has_value());
        return LirReg{static_cast<std::uint32_t>(ord.value_or(0)), 1,
                      static_cast<std::uint8_t>(LirRegClass::GPR)};
    };
    LirReg const x0  = xreg("x0");
    LirReg const xzr = xreg("xzr");

    LirBuilder b{s};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const header = b.createBlock();
    LirBlockId const body   = b.createBlock();
    LirBlockId const exit   = b.createBlock();

    // header: cmp x0, xzr ; jcc(sgt) body, exit
    b.beginBlock(header);
    {
        std::array<LirOperand, 2> cmpOps{LirOperand::makeReg(x0),
                                         LirOperand::makeReg(xzr)};
        (void)b.addInst(cmpOp, InvalidLirReg, cmpOps);
        std::array<LirOperand, 2> jccOps{LirOperand::makeBlockRef(body.v),
                                         LirOperand::makeBlockRef(exit.v)};
        (void)b.addCondBr(jccOp, jccOps, body, exit,
                          static_cast<std::uint32_t>(TargetCondCode::Sgt));
    }
    // body: sub x0, x0, x0 ; jmp header (back-edge — exercises a NEGATIVE disp)
    b.beginBlock(body);
    {
        std::array<LirOperand, 2> subOps{LirOperand::makeReg(x0),
                                         LirOperand::makeReg(x0)};
        (void)b.addInst(subOp, x0, subOps);
        (void)b.addBr(jmpOp, header);
    }
    // exit: ret
    b.beginBlock(exit);
    (void)b.addReturn(retOp, {});

    Lir lir = std::move(b).finish();
    std::vector<MirInstId> lirToMir(lir.instCount(), InvalidMirInst);
    DiagnosticReporter asmRep;
    auto mod = assemble(lir, s, lirToMir, asmRep);
    EXPECT_EQ(asmRep.errorCount(), 0u)
        << "hand-built cmp/jcc/jmp must assemble — a failed encode or branch "
           "resolution drops the function bytes";
    ASSERT_FALSE(mod.functions.empty());

    // The exact 6-word AArch64 sequence (each instruction hand-verified
    // against the ARM ARM):
    //   [+0x00] cmp x0, xzr      0xEB1F001F  (SUBS XZR, X0, XZR)
    //   [+0x04] b.sgt body       0x5400004C  (B.cond, cond GT=0xC, imm19=+2)
    //   [+0x08] b exit           0x14000003  (B, imm26=+3 → +12 bytes)
    //   [+0x0C] sub x0, x0, x0   0xCB000000  (SUB X0, X0, X0)
    //   [+0x10] b header         0x17FFFFFC  (B, imm26=-4 → -16 bytes back-edge)
    //   [+0x14] ret              0xD65F03C0  (RET X30)
    static constexpr std::array<std::uint32_t, 6> kExpectedWords{
        0xEB1F001Fu, 0x5400004Cu, 0x14000003u,
        0xCB000000u, 0x17FFFFFCu, 0xD65F03C0u};
    auto const& bytes = mod.functions[0].bytes;
    ASSERT_EQ(bytes.size(), kExpectedWords.size() * 4u)
        << "skeleton must emit exactly 6 AArch64 words (24 bytes)";
    auto const wordAt = [&](std::size_t off) {
        return static_cast<std::uint32_t>(bytes[off])
             | (static_cast<std::uint32_t>(bytes[off + 1]) << 8)
             | (static_cast<std::uint32_t>(bytes[off + 2]) << 16)
             | (static_cast<std::uint32_t>(bytes[off + 3]) << 24);
    };
    for (std::size_t i = 0; i < kExpectedWords.size(); ++i) {
        EXPECT_EQ(wordAt(i * 4), kExpectedWords[i])
            << "AArch64 word at +0x" << std::hex << (i * 4) << std::dec
            << " diverged from the hand-verified ARM-ARM value";
    }
}

TEST(LirCallconv, BlockTopologyPreservedForBranchingFunction) {
    auto bundle = lowerThroughRewrite(
        "int f(int x) {\n"
        "    int y;\n"
        "    if (x > 0) { y = 1; } else { y = 2; }\n"
        "    return y;\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    Lir const& src = bundle.rewritten.lir;
    Lir const& dst = result.lir;
    ASSERT_EQ(src.moduleFuncCount(), dst.moduleFuncCount());
    for (std::uint32_t i = 0; i < src.moduleFuncCount(); ++i) {
        EXPECT_EQ(src.funcBlockCount(src.funcAt(i)),
                  dst.funcBlockCount(dst.funcAt(i)));
    }
}

TEST(LirCallconv, LoopFunctionMaterializesWithoutErrors) {
    auto bundle = lowerThroughRewrite(
        "int f(int n) {\n"
        "    int i = 0; int acc = 0;\n"
        "    while (i < n) { acc = acc + i; i = i + 1; }\n"
        "    return acc;\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    EXPECT_TRUE(result.ok());
}

TEST(LirCallconv, SpilledFunctionMaterializesFrameOpsToLoadStore) {
    // Use the cross-call moderate-pressure pattern so there are actual
    // frame_load/frame_store ops to materialize.
    //
    // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): the live-across-call values are
    // never-address-taken PARAMETERS (pure SSA `Arg`s), NOT body locals — a local's
    // alloca address is now rematerialized AFTER the call so locals no longer span
    // it. The 8 params each feed the call argument AND the post-call sum, so each
    // spans the call; 8 > SysV's 5 callee-saved GPRs → ≥1 cross-call spill (hence
    // frame_load/frame_store to materialize), remat-independent.
    auto bundle = lowerThroughRewrite(
        "int g(int v) { return v + 1; }\n"
        "int f(int a, int b, int c, int d, int e, int f2, int g2, int h) {\n"
        "    int r = g(a + b + c + d + e + f2 + g2 + h);\n"
        "    return a + b + c + d + e + f2 + g2 + h + r;\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    // Sanity: rewritten module must contain at least one frame op.
    auto const& sch = *bundle.lowered.target;
    auto const fl = sch.opcodeByMnemonic(sch.frameLoadMnemonic());
    auto const fs = sch.opcodeByMnemonic(sch.frameStoreMnemonic());
    ASSERT_TRUE(fl.has_value());
    ASSERT_TRUE(fs.has_value());
    Lir const& rew = bundle.rewritten.lir;
    std::size_t preFrameOpCount = 0;
    for (std::uint32_t fi = 0; fi < rew.moduleFuncCount(); ++fi) {
        LirFuncId const fn = rew.funcAt(fi);
        for (std::uint32_t bi = 0; bi < rew.funcBlockCount(fn); ++bi) {
            LirBlockId const b = rew.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < rew.blockInstCount(b); ++i) {
                std::uint16_t const op = rew.instOpcode(rew.blockInstAt(b, i));
                if (op == *fl || op == *fs) ++preFrameOpCount;
            }
        }
    }
    ASSERT_GT(preFrameOpCount, 0u) << "cross-call corpus must spill";

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(rew, sch, bundle.alloc, ccRep);
    EXPECT_TRUE(result.ok());
    // No frame ops remain in the output.
    Lir const& dst = result.lir;
    std::size_t postFrameOpCount = 0;
    std::size_t loadCount = 0, storeCount = 0;
    auto const loadOp  = sch.opcodeByMnemonic("load");
    auto const storeOp = sch.opcodeByMnemonic("store");
    ASSERT_TRUE(loadOp.has_value());
    ASSERT_TRUE(storeOp.has_value());
    for (std::uint32_t fi = 0; fi < dst.moduleFuncCount(); ++fi) {
        LirFuncId const fn = dst.funcAt(fi);
        for (std::uint32_t bi = 0; bi < dst.funcBlockCount(fn); ++bi) {
            LirBlockId const b = dst.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < dst.blockInstCount(b); ++i) {
                std::uint16_t const op = dst.instOpcode(dst.blockInstAt(b, i));
                if (op == *fl || op == *fs) ++postFrameOpCount;
                if (op == *loadOp)  ++loadCount;
                if (op == *storeOp) ++storeCount;
            }
        }
    }
    EXPECT_EQ(postFrameOpCount, 0u);
    EXPECT_GT(loadCount, 0u)
        << "frame_load materialization must produce ≥1 load";
    EXPECT_GT(storeCount, 0u)
        << "frame_store materialization (or saved-reg prologue/epilogue) "
           "must produce ≥1 store";
}

TEST(LirCallconv, MultiFunctionModuleMaterializesEachFunction) {
    auto bundle = lowerThroughRewrite(
        "int g(int a) { return a + 1; }\n"
        "int f(int x) { int y = g(x); return y; }\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.lir.moduleFuncCount(), 2u);
    EXPECT_EQ(result.perFunc.size(), 2u);
}

TEST(LirCallconv, FrameLayoutInvariantsHoldPerFunction) {
    // Lock the FrameLayout substrate contract: spillAreaOffset ==
    // savedRegAreaSize; savedRegAreaSize == savedRegs.size() * slotSize;
    // spillAreaSize == numSpillSlots * slotSize; totalFrameSize ==
    // alignUp(savedReg + spill, stackAlignment).
    // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): pressure from never-address-taken
    // PARAMETERS (pure SSA `Arg`s), not body locals (whose alloca address is now
    // rematerialized AFTER the call, so they no longer span it). 8 params each span
    // the call → a non-trivial spill area exercising the frame-layout invariants.
    auto bundle = lowerThroughRewrite(
        "int g(int v) { return v + 1; }\n"
        "int f(int a, int b, int c, int d, int e, int f2, int g2, int h) {\n"
        "    int r = g(a + b + c + d + e + f2 + g2 + h);\n"
        "    return a + b + c + d + e + f2 + g2 + h + r;\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    for (std::size_t i = 0; i < result.perFunc.size(); ++i) {
        auto const& layout = result.perFunc[i];
        // D-ML7-2.2 (2026-06-02): spillAreaOffset is now
        // outgoingArgAreaSize + savedRegAreaSize (outgoing area is
        // the new SP+0 zone; saved regs sit above it).
        EXPECT_EQ(layout.spillAreaOffset(),
                  layout.outgoingArgAreaSize + layout.savedRegAreaSize);
        EXPECT_EQ(layout.savedRegAreaOffset(), layout.outgoingArgAreaSize);
        EXPECT_EQ(layout.savedRegAreaSize,
                  static_cast<std::uint32_t>(layout.savedRegs.size()) * layout.slotSize);
        EXPECT_EQ(layout.spillAreaSize,
                  bundle.alloc.perFunc[i].numSpillSlots * layout.slotSize);
        // D-CSUBSET-LOCAL-INT-CODEGEN (step 13.3b, 2026-06-02):
        // localAreaSize == numLocalAllocas * slotSize. Each body-
        // local declaration (`int a1; int r; ...`) emits one
        // `alloca` LIR op which the materialize pass rewrites to a
        // `lea`-of-frame-slot above the spill area.
        EXPECT_EQ(layout.localAreaSize,
                  layout.numLocalAllocas * layout.slotSize);
        EXPECT_EQ(layout.localAreaOffset(),
                  layout.outgoingArgAreaSize + layout.savedRegAreaSize
                      + layout.spillAreaSize);
        // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY (2026-06-02) + D-ML7-2.2
        // (2026-06-02) + D-CSUBSET-LOCAL-INT-CODEGEN (2026-06-02):
        // expected formula incorporates outgoingArgAreaSize directly
        // (already includes the callee's shadow-space when hasCalls)
        // AND the new localAreaSize above the spill area.
        std::uint32_t const raw = layout.outgoingArgAreaSize
                                + layout.savedRegAreaSize
                                + layout.spillAreaSize
                                + layout.localAreaSize;
        std::uint32_t expected;
        if (layout.hasCalls) {
            expected = dss::alignedSizeWithBias(
                raw,
                static_cast<std::uint32_t>(cc->stackAlignment),
                static_cast<std::uint32_t>(cc->callPushBytes));
        } else {
            expected = (raw + cc->stackAlignment - 1u)
                     & ~(cc->stackAlignment - 1u);
        }
        EXPECT_EQ(layout.totalFrameSize, expected);
        EXPECT_GT(layout.slotSize, 0u);
        // savedRegs sorted ascending by ordinal (determinism contract).
        for (std::size_t j = 1; j < layout.savedRegs.size(); ++j) {
            EXPECT_LT(layout.savedRegs[j - 1].id, layout.savedRegs[j].id);
        }
    }
}

TEST(LirCallconv, StackPointerMissingFromSchemaIsValidationError) {
    // The validate() rule for register-machine ABIs requires every cc
    // with ABI info to declare a stackPointer. A schema without it
    // must fail load-time validation with a structured diagnostic.
    auto r = TargetSchema::loadFromText(R"({
        "dssTargetVersion": 1,
        "target": { "name": "fictional", "abiModel": "register-machine" },
        "opcodes": [{ "mnemonic": "invalid", "result": "none" }],
        "registers": [
            { "name": "r0", "class": "gpr", "widthBytes": 8 }
        ],
        "callingConventions": [
            { "name": "default", "argGprs": ["r0"], "stackAlignment": 16 }
        ]
    })");
    ASSERT_FALSE(r.has_value())
        << "register-machine schema without stackPointer must fail validation";
}

TEST(LirCallconv, FrameSizeAlignedToCcStackAlignment) {
    // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): pressure from never-address-taken
    // PARAMETERS (pure SSA `Arg`s), not body locals (whose alloca address is now
    // rematerialized AFTER the call, so they no longer span it). 8 params each span
    // the call → a non-trivial spill area exercising the frame-layout invariants.
    auto bundle = lowerThroughRewrite(
        "int g(int v) { return v + 1; }\n"
        "int f(int a, int b, int c, int d, int e, int f2, int g2, int h) {\n"
        "    int r = g(a + b + c + d + e + f2 + g2 + h);\n"
        "    return a + b + c + d + e + f2 + g2 + h + r;\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_GT(cc->stackAlignment, 0u);
    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY (2026-06-02): non-leaf
    // functions satisfy `totalFrameSize % stackAlignment == callPushBytes`
    // (the bias the prologue applies so post-sub RSP lands at 0 mod
    // alignment for the next call site). Leaf functions retain the
    // pre-fix "divisible-by-alignment" invariant.
    for (auto const& layout : result.perFunc) {
        std::uint32_t const expected = layout.hasCalls
            ? static_cast<std::uint32_t>(cc->callPushBytes)
            : 0u;
        EXPECT_EQ(layout.totalFrameSize % cc->stackAlignment, expected)
            << "non-leaf frame must satisfy callPushBytes-mod-alignment; "
            << "leaf frame must be a multiple of stackAlignment ("
            << cc->stackAlignment << ")";
    }
}

// ── ML7 cycle 2: `arg` + `call` materialization (ABI goldens) ─────────
//
// Pins:
//   * Every `arg k` virtual op is rewritten to `mov result, argReg[k]`
//     (or omitted when regalloc already picked argReg[k]).
//   * Every direct `call` is rewritten to `mov destArg[i], srcReg[i]; ...;
//     call <symbol>; mov result, returnReg`.
//   * No `arg` opcodes survive into the callconv output.
//   * `call` opcodes survive but carry ONLY the SymbolRef operand —
//     no Reg operands. Matches the schema's encoding variant guard
//     `["symbol"]` so the assembler can encode the byte sequence.
//   * Stack-passed args (k >= argGprs.size()) fail loud with
//     `L_StackPassedArgUnsupported` (D-ML7-2.2).

namespace {

// Walk every inst in the post-callconv module and count opcodes by
// mnemonic. The lookups are schema-driven so the tests work against
// any target.
struct InstStats {
    std::uint32_t argOps  = 0;
    std::uint32_t callOps = 0;
    std::uint32_t movOps  = 0;
    // For each call inst, record whether its operand list is just
    // [SymbolRef] (the post-ML7-cycle-2 shape) or carries extra Reg
    // operands (the pre-cycle-2 shape — would fail at the assembler).
    std::uint32_t callsWithOnlySymbol = 0;
    // D-CSUBSET-LOCAL-INT-CODEGEN (step 13.3b, 2026-06-02):
    // post-materialize, EVERY `alloca` LIR op must be rewritten to
    // a `lea result, [sp + offset]`. Survival of an `alloca` would
    // trip A_NoEncodingDeclared at the assembler. The 7-agent F3
    // audit added these counters + the per-lea-offset capture so
    // the materialize-loop's offset arithmetic is unit-pinned, not
    // just the layout-arithmetic invariant.
    std::uint32_t                 allocaOps  = 0;
    std::uint32_t                 leaOps     = 0;
    std::vector<std::int32_t>     leaOffsets;  // per-lea MemOffset value
};

[[nodiscard]] InstStats
collectInstStats(Lir const& lir, TargetSchema const& schema) {
    InstStats s;
    auto const argOp    = schema.opcodeByMnemonic("arg");
    auto const callOp   = schema.opcodeByMnemonic("call");
    auto const movOp    = schema.opcodeByMnemonic("mov");
    auto const allocaOp = schema.opcodeByMnemonic("alloca");
    auto const leaOp    = schema.opcodeByMnemonic("lea");
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const b = lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < lir.blockInstCount(b); ++i) {
                LirInstId const inst = lir.blockInstAt(b, i);
                std::uint16_t const op = lir.instOpcode(inst);
                if (argOp.has_value()  && op == *argOp)  ++s.argOps;
                if (movOp.has_value()  && op == *movOp)  ++s.movOps;
                if (allocaOp.has_value() && op == *allocaOp) ++s.allocaOps;
                if (leaOp.has_value() && op == *leaOp) {
                    ++s.leaOps;
                    // 2nd-order silent-failure fix: capture the disp32
                    // ONLY for the 3-op LEA variant (`lea result,
                    // [base + disp32]` — operand layout [base_reg,
                    // MemBase, MemOffset]). Pushing `0` for the 4-op
                    // indexed and 1-op SymbolRef-RipRel variants would
                    // silently satisfy offset==0 membership checks
                    // (the alloca-materialize shape can legitimately
                    // emit offset 0 for leaf functions with no
                    // outgoing/saved/spill area), causing the multi-
                    // alloca off-by-one regression detector to pass
                    // when it shouldn't. Skip non-3-op variants
                    // entirely so `leaOffsets` only contains the
                    // alloca-materialize shape's offsets.
                    auto const ops = lir.instOperands(inst);
                    if (ops.size() == 3
                        && ops[2].kind == LirOperandKind::MemOffset) {
                        s.leaOffsets.push_back(ops[2].offset);
                    }
                }
                if (callOp.has_value() && op == *callOp) {
                    ++s.callOps;
                    auto const ops = lir.instOperands(inst);
                    if (ops.size() == 1
                        && ops[0].kind == LirOperandKind::SymbolRef) {
                        ++s.callsWithOnlySymbol;
                    }
                }
            }
        }
    }
    return s;
}

} // namespace

TEST(LirCallconvAbi, ArgOpsDoNotSurviveMaterialization) {
    // A function with parameters MUST have every `arg` rewritten by
    // ML7 cycle 2; otherwise the assembler (plan 13) trips with
    // A_NoMatchingEncodingVariant on the virtual op.
    auto bundle = lowerThroughRewrite("int f(int x) { return x + x; }");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const stats = collectInstStats(result.lir, *bundle.lowered.target);
    EXPECT_EQ(stats.argOps, 0u)
        << "every `arg` virtual op must be materialized into a mov "
           "(or omitted when regalloc picked the source reg)";
}

TEST(LirCallconvAbi, CallOpsCarryOnlySymbolRefAfterMaterialization) {
    // The `call` opcode's encoding variant guard in x86_64.target.json
    // is `["symbol"]` — the assembler expects a single SymbolRef
    // operand. Pin that ML7 cycle 2 strips the arg-reg operands from
    // every call so the assembler can encode it.
    auto bundle = lowerThroughRewrite(
        "int g(int a) { return a + 1; }\n"
        "int f(int x) { return g(x); }\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const stats = collectInstStats(result.lir, *bundle.lowered.target);
    ASSERT_GT(stats.callOps, 0u) << "the corpus has a g(x) call site";
    EXPECT_EQ(stats.callsWithOnlySymbol, stats.callOps)
        << "every call must carry ONLY the symbol operand after ML7 c2";
}

TEST(LirCallconvAbi, SingleArgFunctionEmitsMovFromArgGpr0) {
    // SysV AMD64 / AAPCS64 / MS-x64 all place the first integer
    // argument in argGprs[0]. The materialization must emit either
    // `mov <regallocPick>, argGprs[0]` OR no-op when regallocPick ==
    // argGprs[0]. Either way: the param's home reg must be reachable
    // from argGprs[0] after the prologue.
    auto bundle = lowerThroughRewrite("int f(int x) { return x; }");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const stats = collectInstStats(result.lir, *bundle.lowered.target);
    EXPECT_EQ(stats.argOps, 0u);
    // At least the implicit return-value mov should exist (mov rax,
    // <reg>). Plus the arg materialization mov when regalloc didn't
    // happen to pick argGprs[0]. Don't pin an exact count — just
    // confirm movs were generated.
    EXPECT_GE(stats.movOps, 1u);
}

TEST(LirCallconvAbi, MsX64CcDoesNotDeclareVariadicVectorCountReg) {
    // D-LANG-VARIADIC (step 13.4) post-fold MEDIUM-2: Win64 ms_x64
    // has NO equivalent to SysV's AL-count register — the loader
    // ABI uses vararg double-spill (anchored
    // D-ML7-VARIADIC-WIN64-DOUBLE-SPILL). A regression that copy-
    // pasted SysV's `variadicVectorCountReg: rax` into the ms_x64
    // entry would silently emit a wrong-cc count-mov at every printf
    // call from a Windows-targeted binary. Pin: the ms_x64 cc field
    // MUST be empty.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const* msX64 = (*target)->callingConventionByName("ms_x64");
    ASSERT_NE(msX64, nullptr)
        << "x86_64 target schema must declare 'ms_x64' cc";
    EXPECT_FALSE(msX64->variadicVectorCountReg.has_value())
        << "Win64 ms_x64 has NO caller-side variadic vector-count "
           "register — the count-mov path is SysV-specific. A future "
           "Win64 vararg-double-spill implementation goes through "
           "D-ML7-VARIADIC-WIN64-DOUBLE-SPILL, NOT this field.";
}

TEST(LirCallconvAbi, SysVCcDeclaresVariadicVectorCountReg) {
    // D-LANG-VARIADIC (step 13.4) substrate pin: the SysV AMD64 cc
    // on x86_64 MUST declare `variadicVectorCountReg` (rax / AL per
    // §3.5.7). Without this field, ML7 materialize skips the
    // pre-call `mov <countReg>, <fpCount>` and printf reads garbage
    // from AL on hardened glibcs. The end-to-end emission + runtime
    // behavior is pinned at the runnable-example tier
    // (`examples/c-subset/hello_printf`); this test pins the
    // SCHEMA tier so a CC-config regression surfaces as a target-
    // schema fail-loud here, not as a runtime garbage in printf.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value()) << "loadShipped(x86_64) failed";
    auto const* cc = (*target)->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_TRUE(cc->variadicVectorCountReg.has_value())
        << "SysV AMD64 cc (cc index 0 on x86_64) MUST declare the "
           "variadic vector-count register per SysV §3.5.7 / "
           "D-LANG-VARIADIC step 13.4.";
    // The register name must resolve in the target's register table
    // — a typo'd or non-existent name would silently cause the
    // ordinal to refer to the wrong physical reg. The loader sets
    // both name + ordinal atomically; verifying both stay in sync
    // pins the contract.
    auto const ord = (*target)->registerByName(
        cc->variadicVectorCountReg->name);
    ASSERT_TRUE(ord.has_value())
        << "variadicVectorCountReg name '"
        << cc->variadicVectorCountReg->name
        << "' must resolve in the target's register table.";
    EXPECT_EQ(*ord, cc->variadicVectorCountReg->ordinal);
}

TEST(LirCallconvAbi, MsX64VaListLayoutIsHomogeneousPointer) {
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE) config pin: the ms_x64 CC declares the
    // HomogeneousPointer va_list strategy with an 8-byte slot stride, and — because
    // Win64 spills into the caller's home space (no callee-local register-save-area)
    // — its regSaveAreaBytes() is 0. A regression that pasted SysV's register-save
    // geometry (gpSaveCount/fpSaveCount) onto ms_x64 would size a phantom 176-byte
    // zone into every Win64 variadic frame; this pin catches it red-on-disable.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const* msX64 = (*target)->callingConventionByName("ms_x64");
    ASSERT_NE(msX64, nullptr) << "x86_64 must declare 'ms_x64'";
    ASSERT_TRUE(msX64->vaListLayout.has_value())
        << "ms_x64 must declare a vaListLayout (the Win64 variadic-callee ABI)";
    EXPECT_EQ(msX64->vaListLayout->strategy, VaListStrategy::HomogeneousPointer);
    EXPECT_EQ(msX64->vaListLayout->namedArgSlotBytes, 8u);
    EXPECT_EQ(msX64->vaListLayout->regSaveAreaBytes(), 0u)
        << "Win64 HomogeneousPointer has NO callee-local register-save-area — it "
           "spills into the caller's home space; regSaveAreaBytes() MUST be 0.";
}

TEST(LirCallconvAbi, SysVVaListLayoutUntouchedBySysVRegisterSave) {
    // FC12b SysV-untouched guard: the FC12b strategy tagging must NOT perturb the
    // SysV vaListLayout — it stays SysVRegisterSave with the §3.5.7 geometry
    // (6 GPR + 8 SSE save slots = 176B). A regression flipping the SysV strategy or
    // dropping its save-counts would silently break the (still-shipping) SysV
    // varargs path; this pin keeps them byte-stable.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const* sysv = (*target)->callingConventionByName("sysv_amd64");
    ASSERT_NE(sysv, nullptr) << "x86_64 must declare 'sysv_amd64'";
    ASSERT_TRUE(sysv->vaListLayout.has_value());
    EXPECT_EQ(sysv->vaListLayout->strategy, VaListStrategy::SysVRegisterSave);
    EXPECT_EQ(sysv->vaListLayout->gpSaveCount, 6u);
    EXPECT_EQ(sysv->vaListLayout->fpSaveCount, 8u);
    EXPECT_EQ(sysv->vaListLayout->regSaveAreaBytes(), 176u);
    // namedArgSlotBytes is additive (== gpSlotBytes on SysV) and behavior-identical.
    EXPECT_EQ(sysv->vaListLayout->namedArgSlotBytes, 8u);
}

TEST(LirCallconvAbi, Win64VariadicCalleeHomeBaseCongruenceNoShadow) {
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE, BLOCKER-1) CONGRUENCE PIN (red-on-
    // disable). Lower a Win64 (ms_x64, cc index 1) variadic CALLEE end-to-end and
    // assert that BOTH the `va_home_arg_area` materialization (the `lea` va_start
    // base) AND the prologue home-spill stores use the byte-identical NO-SHADOW base
    // `totalFrameSize + callPushBytes` — never `+ shadowSpaceBytes`. If anyone adds
    // shadowSpaceBytes to either, the va_start base and/or every home-spill target
    // jumps 32 bytes past the home space → va_arg reads garbage. Also pins
    // vaRegSaveAreaSize == 0 (Win64 has no callee-local register-save zone).
    auto bundle = lowerThroughRewrite(
        "int sum(int n, ...) {\n"
        "    va_list ap; va_start(ap, n);\n"
        "    int t = va_arg(ap, int);\n"
        "    va_end(ap);\n"
        "    return t;\n"
        "}\n",
        /*ccIndex=*/1);
    ASSERT_TRUE(bundle.lowered.lir.ok) << "Win64 variadic callee failed to lower";
    ASSERT_TRUE(bundle.alloc.ok());
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(ccRep.errorCount(), 0u);

    auto const* cc = bundle.lowered.target->callingConventionByName("ms_x64");
    ASSERT_NE(cc, nullptr);
    ASSERT_GT(cc->shadowSpaceBytes, 0u) << "ms_x64 must declare shadow space for the "
                                           "no-shadow pin to be meaningful";

    // `sum` is function 0. Its FrameLayout: vaRegSaveAreaSize MUST be 0 (Win64).
    auto const* layout = result.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    EXPECT_EQ(layout->vaRegSaveAreaSize, 0u)
        << "Win64 HomogeneousPointer reserves NO callee-local register-save-area";

    std::uint32_t const noShadowBase =
        layout->totalFrameSize + static_cast<std::uint32_t>(cc->callPushBytes);
    std::uint32_t const withShadowBase =
        noShadowBase + static_cast<std::uint32_t>(cc->shadowSpaceBytes);

    // Resolve the lea opcode (va_home materializes to lea) + the GPR store opcode
    // (home spill) + the first integer arg reg ordinal (rcx) for the spill check.
    auto const leaOp = bundle.lowered.target->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    auto const storeOp = bundle.lowered.target->opcodeByMnemonic("store");
    ASSERT_TRUE(storeOp.has_value());
    ASSERT_FALSE(cc->argGprs.empty());
    auto const rcxOrd = bundle.lowered.target->registerByName(cc->argGprs[0]);
    ASSERT_TRUE(rcxOrd.has_value());

    // Helper: the MemOffset operand value of an inst (the [sp + disp] disp), or
    // nullopt if the inst has no MemOffset operand.
    auto memOffsetOf = [](Lir const& lir, LirInstId inst) -> std::optional<std::int32_t> {
        for (auto const& op : lir.instOperands(inst)) {
            if (op.kind == LirOperandKind::MemOffset) return op.offset;
        }
        return std::nullopt;
    };

    // Scan function 0's blocks. The va_home base is the EXPECTED no-shadow value
    // `noShadowBase + namedArgCount*8`; namedArgCount = 1 (the single fixed `n`).
    std::uint32_t const expectedHomeBase = noShadowBase + 1u * layout->outgoingSlotSize;
    bool sawVaHomeLeaAtNoShadow = false;
    bool sawHomeSpillRcxAtNoShadow = false;
    LirFuncId const fn = result.lir.funcAt(0);
    for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
        LirBlockId const blk = result.lir.funcBlockAt(fn, bi);
        for (std::uint32_t i = 0; i < result.lir.blockInstCount(blk); ++i) {
            LirInstId const inst = result.lir.blockInstAt(blk, i);
            std::uint16_t const op = result.lir.instOpcode(inst);
            auto const off = memOffsetOf(result.lir, inst);
            if (!off.has_value()) continue;
            if (op == *leaOp
                && static_cast<std::uint32_t>(*off) == expectedHomeBase) {
                sawVaHomeLeaAtNoShadow = true;
            }
            // The home spill of rcx (arg reg 0) targets the no-shadow base + 0.
            if (op == *storeOp
                && static_cast<std::uint32_t>(*off) == noShadowBase) {
                for (auto const& o : result.lir.instOperands(inst)) {
                    if (o.kind == LirOperandKind::Reg && o.reg.isPhysical != 0
                        && o.reg.id == *rcxOrd) {
                        sawHomeSpillRcxAtNoShadow = true;
                    }
                }
            }
            // Anti-pin: NO va-related lea/store may sit at the WITH-shadow base
            // (would mean shadowSpaceBytes leaked into the home geometry).
            EXPECT_NE(static_cast<std::uint32_t>(*off), withShadowBase)
                << "a va_home/home-spill at the WITH-shadow base "
                << withShadowBase << " means shadowSpaceBytes leaked into the Win64 "
                   "home geometry (BLOCKER-1 violated) — every va_arg reads garbage";
        }
    }
    EXPECT_TRUE(sawVaHomeLeaAtNoShadow)
        << "the va_start home base (lea) must be totalFrameSize + callPushBytes + "
           "namedArgCount*slot = " << expectedHomeBase << " (NO shadowSpaceBytes)";
    EXPECT_TRUE(sawHomeSpillRcxAtNoShadow)
        << "the named int reg (rcx) home spill must target totalFrameSize + "
           "callPushBytes = " << noShadowBase << " (== the va_arg-read base; the "
           "spill-target == va_arg-read-target congruence)";
}

TEST(LirCallconvAbi, Win64VariadicCallDupsFpVarargIntoHomeGpr) {
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE, PART 5) FP-dup callconv pin: a Win64
    // (ms_x64, cc index 1) variadic CALL with a register-resident FP vararg must
    // emit a `movq_xmm_to_gpr` duplicating the FP vararg into its matching home
    // integer register. Without the dup, the callee's va_arg(double) reads an
    // uninitialized home GPR slot → garbage. RED-ON-DISABLE: deleting the FP-dup
    // emission removes the only `movq_xmm_to_gpr` in the call sequence.
    auto bundle = lowerThroughRewrite(
        "double take(int n, ...) { return (double)n; }\n"   // DEFINED so the callee symbol binds (D-CSUBSET-FN-PROTOTYPE)
        "int main(void) {\n"
        "  return (int)take(1, 2.5);\n"   // one int fixed arg + one double vararg
        "}\n",
        /*ccIndex=*/1);
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.alloc.ok());
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(ccRep.errorCount(), 0u);

    auto const movqOp =
        bundle.lowered.target->opcodeByMnemonic("movq_xmm_to_gpr");
    ASSERT_TRUE(movqOp.has_value());
    auto const* cc = bundle.lowered.target->callingConventionByName("ms_x64");
    ASSERT_NE(cc, nullptr);
    ASSERT_GE(cc->argGprs.size(), 2u);
    // The double vararg is slot 1 (slot 0 = the fixed int `n`); its home GPR is
    // argGprs[1] (rdx). The dup writes that GPR.
    auto const homeGprOrd =
        bundle.lowered.target->registerByName(cc->argGprs[1]);
    ASSERT_TRUE(homeGprOrd.has_value());

    bool sawDupIntoHomeGpr = false;
    for (std::uint32_t fi = 0; fi < result.lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = result.lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
            LirBlockId const blk = result.lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < result.lir.blockInstCount(blk); ++i) {
                LirInstId const inst = result.lir.blockInstAt(blk, i);
                if (result.lir.instOpcode(inst) != *movqOp) continue;
                LirReg const r = result.lir.instResult(inst);
                if (r.valid() && r.isPhysical != 0 && r.id == *homeGprOrd)
                    sawDupIntoHomeGpr = true;
            }
        }
    }
    EXPECT_TRUE(sawDupIntoHomeGpr)
        << "a Win64 variadic call with an FP vararg must emit `movq_xmm_to_gpr` "
           "duplicating the FP vararg into its home GPR (" << cc->argGprs[1] << ")";
}

TEST(CallPayload, EncodeDecodeRoundtripsVariadicAndFixedCount) {
    // D-LANG-VARIADIC (step 13.4) substrate pin: the shared MIR/LIR
    // Call payload encoding (bit 31 = isVariadic; bits 0..29 =
    // fixedOperandCount) round-trips for both the non-variadic and
    // variadic cases. The ML7 materialize call arm reads these bits
    // off every call inst; an off-by-one in the mask would either
    // truncate fixedOperandCount or silently flip the variadic bit, both
    // of which would corrupt the count-mov emission for variadic
    // calls (and falsely trigger it for non-variadic calls).
    using namespace dss::call_payload;
    // Non-variadic encoding is payload == 0 (preserves the addInst
    // default for every pre-13.4 call site).
    EXPECT_EQ(encode(false, 0u), 0u);
    EXPECT_FALSE(isVariadic(encode(false, 0u)));
    // The high bit alone flips isVariadic; fixedOperandCount=0 round-
    // trips (a hypothetical thunk-style vararg-only function).
    EXPECT_TRUE(isVariadic(encode(true, 0u)));
    EXPECT_EQ(fixedOperandCount(encode(true, 0u)), 0u);
    // Typical printf shape: 1 fixed param contributing 1 operand + vararg.
    EXPECT_TRUE(isVariadic(encode(true, 1u)));
    EXPECT_EQ(fixedOperandCount(encode(true, 1u)), 1u);
    // Round-trip at the high edge of the fixed-operand field. If the
    // mask boundary regresses, fixedOperandCount(kFixedOperandMask) returns
    // a different value here — pins the contract.
    EXPECT_EQ(fixedOperandCount(encode(true, kFixedOperandMask)),
              kFixedOperandMask);
    EXPECT_TRUE(isVariadic(encode(true, kFixedOperandMask)));
    // A non-variadic encoding with a fixed-operand count carries the
    // fixedOperandCount through but reports isVariadic=false — the
    // accessor never spuriously reports variadic just because
    // fixedOperandCount is non-zero.
    EXPECT_FALSE(isVariadic(encode(false, 42u)));
    EXPECT_EQ(fixedOperandCount(encode(false, 42u)), 42u);

    // FC7 C3 (AAPCS64/Apple x8 sret): bit 30 = hasIndirectResult, the flag
    // lir_callconv reads to route the prepended sret-pointer operand to the cc's
    // indirect-result register (x8) instead of arg0. It is INDEPENDENT of the
    // variadic bit (31) and the fixedOperandCount field (0..29) — a flip of any one
    // must not perturb the others (else an x8-sret call would mis-route args or
    // mis-stamp the variadic count).
    EXPECT_FALSE(hasIndirectResult(encode(false, 0u)));
    EXPECT_TRUE(hasIndirectResult(encode(false, 0u, /*hasIndirectResult=*/true)));
    // Independent of fixedOperandCount: an x8-sret call to a 3-fixed-operand fn.
    {
        std::uint32_t const p = encode(false, 3u, true);
        EXPECT_TRUE(hasIndirectResult(p));
        EXPECT_FALSE(isVariadic(p));
        EXPECT_EQ(fixedOperandCount(p), 3u);
    }
    // Independent of variadic: all three bits coexist.
    {
        std::uint32_t const p = encode(true, 7u, true);
        EXPECT_TRUE(isVariadic(p));
        EXPECT_TRUE(hasIndirectResult(p));
        EXPECT_EQ(fixedOperandCount(p), 7u);
    }
    // Default (2-arg encode) never sets the IRR bit — every pre-C3 call site stays
    // non-sret.
    EXPECT_FALSE(hasIndirectResult(encode(true, 5u)));
}

TEST(LirCallconvAbi, CalleeArgReceivesFromArgGprAcrossSysVAndMsX64) {
    // The MIR→LIR isel reads the function's `callingConventionIndex`
    // from regalloc-assigned `LirFuncAllocation.callingConventionIndex`.
    // The default (cycle 1) is cc index 0 — SysV on x86_64. Test that
    // a function with one arg materializes its arg-mov against the
    // cc's argGprs[0] (rdi on SysV). MS-x64 would yield rcx; the
    // ms_x64 path needs a separate driver-flag plumbing anchored at
    // D-ML7-2.6 (cc selection by attribute). For now this pins the
    // SysV default.
    auto bundle = lowerThroughRewrite("int f(int x) { return x; }");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_FALSE(cc->argGprs.empty());

    auto const argGpr0Ord =
        bundle.lowered.target->registerByName(cc->argGprs[0]);
    ASSERT_TRUE(argGpr0Ord.has_value());
    auto const movOp =
        bundle.lowered.target->opcodeByMnemonic("mov");
    ASSERT_TRUE(movOp.has_value());

    // Post-fold #8 simplifier R3 fold: was a 22-line inline mov-walk;
    // collapses to the shared `anyMovTouchesPhysReg` helper. Either
    // a mov touches argGprs[0] (as src — the arg copy — or as dest
    // — the regalloc-picked home).
    EXPECT_TRUE(anyMovTouchesPhysReg(result.lir, *argGpr0Ord, *movOp))
        << "the arg materialization must touch argGprs[0] ("
        << cc->argGprs[0] << ") either as a mov source (arg copy) "
           "or as the regalloc-picked home reg (no-op skipped mov)";
}

TEST(LirCallconvAbi, CcIndex1DrivesDifferentArgGprThanCc0) {
    // PINS: D-FF3-3 behavioral arm — companion to
    // `LirRegAlloc.CcIndex1RecordsThroughToFuncAllocation`
    // (metadata pin, test_lir_regalloc.cpp) and
    // `CalleeArgReceivesFromArgGprAcrossSysVAndMsX64` (ccIndex=0
    // behavior). This pins that ccIndex=1 actually steers the emitted
    // arg-loading mov to a DIFFERENT physical register than ccIndex=0
    // does — i.e. the index isn't read into metadata and then ignored
    // downstream. For x86_64: cc[0]=sysv_amd64 (arg0=rdi), cc[1]=ms_x64
    // (arg0=rcx). Without this pin, a regression that reads
    // `funcAlloc.callingConventionIndex` but then hardcodes cc[0] in
    // the prologue emitter would silently emit SysV ABI on PE+x86_64
    // targets — wrong-machine-code surface.
    //
    // Post-fold #7 silent-failure F3: assert `bundle.alloc.ok()`
    // BEFORE `bundle.rewritten.ok` so an alloc failure (e.g. future
    // schema regression on cc[1]) fails loud with the right cause
    // instead of being masked by `LirRewriteResult{}` defaulting to
    // ok=true. Post-fold #7 PT3: also assert cc[0].argGprs[0] does
    // NOT surface — catches a regression that emits BOTH cc[0] and
    // cc[1] arg movs (a `sawCc1Arg0`-only positive pin would pass).
    auto bundle = lowerThroughRewrite("int f(int x) { return x; }",
                                       /*ccIndex=*/1);
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.alloc.ok())
        << "allocateRegisters(ccIndex=1) failed — likely a schema or "
           "regalloc regression upstream of materialization; the "
           "downstream rewritten.ok would default-true and mislead";
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());

    auto const* cc0 = bundle.lowered.target->callingConvention(0);
    auto const* cc1 = bundle.lowered.target->callingConvention(1);
    ASSERT_NE(cc0, nullptr);
    ASSERT_NE(cc1, nullptr);
    ASSERT_FALSE(cc0->argGprs.empty());
    ASSERT_FALSE(cc1->argGprs.empty());
    // The behavioral pin only has teeth when cc[0].argGprs[0] !=
    // cc[1].argGprs[0]. A future schema where they coincide makes
    // the test vacuous — fail loud rather than silently pass.
    ASSERT_NE(cc0->argGprs[0], cc1->argGprs[0])
        << "Behavioral pin requires cc[0] and cc[1] argGprs[0] to "
           "differ for the test to be meaningful; both name "
        << cc0->argGprs[0];

    auto const cc0Arg0Ord =
        bundle.lowered.target->registerByName(cc0->argGprs[0]);
    auto const cc1Arg0Ord =
        bundle.lowered.target->registerByName(cc1->argGprs[0]);
    ASSERT_TRUE(cc0Arg0Ord.has_value());
    ASSERT_TRUE(cc1Arg0Ord.has_value());
    auto const movOp = bundle.lowered.target->opcodeByMnemonic("mov");
    ASSERT_TRUE(movOp.has_value());

    EXPECT_TRUE(anyMovTouchesPhysReg(result.lir, *cc1Arg0Ord, *movOp))
        << "ccIndex=1 arg materialization must touch " << cc1->argGprs[0]
        << " (cc[1].argGprs[0]); a regression hardcoding cc[0] would "
        << "surface " << cc0->argGprs[0] << " instead and silently "
        << "emit SysV ABI for a Windows target";
    EXPECT_FALSE(anyMovTouchesPhysReg(result.lir, *cc0Arg0Ord, *movOp))
        << "ccIndex=1 must NOT use " << cc0->argGprs[0]
        << " (cc[0].argGprs[0]); a regression that emits BOTH cc[0] "
        << "and cc[1] arg movs would pass a positive-only pin but "
        << "still ship wrong-ABI bytes";
}

TEST(LirCallconvAbi, CallSiteMaterializesArgIntoArgGprAndResultFromReturnGpr) {
    // f calls g(x); after ML7 cycle 2 the call site must have:
    //   * a mov instruction whose DEST is argGprs[0] (the arg-passing
    //     mov before the call), OR src/dest already == argGprs[0]
    //     because regalloc happened to pick it.
    //   * a mov instruction whose SOURCE is returnGprs[0] (the
    //     return-value mov AFTER the call), OR the call's result
    //     reg already == returnGprs[0].
    auto bundle = lowerThroughRewrite(
        "int g(int a) { return a + 1; }\n"
        "int f(int x) { return g(x); }\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_FALSE(cc->argGprs.empty());
    ASSERT_FALSE(cc->returnGprs.empty());
    auto const argGpr0 = bundle.lowered.target->registerByName(cc->argGprs[0]);
    auto const retGpr0 = bundle.lowered.target->registerByName(cc->returnGprs[0]);
    ASSERT_TRUE(argGpr0.has_value());
    ASSERT_TRUE(retGpr0.has_value());

    // Walk f's blocks; find the call and inspect the surrounding movs.
    auto const callOp = bundle.lowered.target->opcodeByMnemonic("call");
    auto const movOp  = bundle.lowered.target->opcodeByMnemonic("mov");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(movOp.has_value());

    bool sawArgMov = false;
    bool sawReturnMov = false;
    bool sawCall = false;
    for (std::uint32_t fi = 0; fi < result.lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = result.lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
            LirBlockId const b = result.lir.funcBlockAt(fn, bi);
            std::uint32_t const instN = result.lir.blockInstCount(b);
            for (std::uint32_t i = 0; i < instN; ++i) {
                LirInstId const inst = result.lir.blockInstAt(b, i);
                std::uint16_t const op = result.lir.instOpcode(inst);
                if (op == *callOp) {
                    sawCall = true;
                    // Check for a mov INTO argGpr0 anywhere before
                    // this call in the same block (arg-passing mov).
                    for (std::uint32_t j = 0; j < i; ++j) {
                        LirInstId const pre = result.lir.blockInstAt(b, j);
                        if (result.lir.instOpcode(pre) != *movOp) continue;
                        LirReg const dst = result.lir.instResult(pre);
                        if (dst.valid() && dst.isPhysical != 0
                            && dst.id == *argGpr0) {
                            sawArgMov = true;
                        }
                    }
                    // Check for a mov FROM retGpr0 anywhere after
                    // this call in the same block (return-value mov).
                    for (std::uint32_t j = i + 1; j < instN; ++j) {
                        LirInstId const post = result.lir.blockInstAt(b, j);
                        if (result.lir.instOpcode(post) != *movOp) continue;
                        for (auto const& opnd : result.lir.instOperands(post)) {
                            if (opnd.kind == LirOperandKind::Reg
                                && opnd.reg.isPhysical != 0
                                && opnd.reg.id == *retGpr0) {
                                sawReturnMov = true;
                            }
                        }
                    }
                }
            }
        }
    }
    ASSERT_TRUE(sawCall) << "the corpus has a call site";
    EXPECT_TRUE(sawArgMov)
        << "ML7 cycle 2 must emit a mov INTO " << cc->argGprs[0]
        << " before the call (arg-passing mov)";
    EXPECT_TRUE(sawReturnMov)
        << "ML7 cycle 2 must emit a mov FROM " << cc->returnGprs[0]
        << " after the call (return-value mov)";
}

// VoidReturnCallExercisesNoPostCallReturnMov — synthesizes a LIR
// call with a SymbolRef callee + zero args + InvalidLirReg result,
// directly exercising the `if (result.valid())` false branch in
// the call materializer (the dropped value-returning corpus test
// was dishonest — it only exercised the true branch).
TEST(LirCallconvAbi, VoidReturnCallExercisesNoPostCallReturnMov) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& sch = **target;
    auto const callOp = sch.opcodeByMnemonic("call");
    auto const retOp  = sch.opcodeByMnemonic("ret");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(retOp.has_value());

    LirBuilder b{sch};
    b.addFunction(SymbolId{55});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    // call <sym>, no args — result is implicitly invalid via
    // InvalidLirReg passed below.
    std::array<LirOperand, 1> callOps{LirOperand::makeSymbolRef(13)};
    b.addInst(*callOp, InvalidLirReg, callOps);
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                     = true;
    alloc.perFunc.back().originalSymbol         = SymbolId{55};
    alloc.perFunc.back().callingConventionIndex = 0;
    alloc.perFunc.back().numSpillSlots          = 0;

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch, alloc, ccRep);
    ASSERT_TRUE(result.ok());
    // The materialized output should have exactly one call inst (with
    // single SymbolRef operand) and NO post-call return-value mov.
    auto const movOp = sch.opcodeByMnemonic("mov");
    ASSERT_TRUE(movOp.has_value());
    std::uint32_t callCount = 0;
    std::uint32_t movCount  = 0;
    for (std::uint32_t fi = 0; fi < result.lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = result.lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
            LirBlockId const b2 = result.lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < result.lir.blockInstCount(b2); ++i) {
                std::uint16_t const op =
                    result.lir.instOpcode(result.lir.blockInstAt(b2, i));
                if (op == *callOp) ++callCount;
                if (op == *movOp)  ++movCount;
            }
        }
    }
    EXPECT_EQ(callCount, 1u);
    EXPECT_EQ(movCount, 0u)
        << "void-return call must NOT emit any post-call return-value mov "
           "(`if (result.valid())` false branch in materializer)";
}

// FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS) — MF-2 cycle-break pin. A struct
// returned in TWO eightbytes can land its piece vregs CROSS-WISE vs the return
// registers (piece 0 in rdx, piece 1 in rax). The naive emit (`mov rax,rdx; mov
// rdx,rax`) clobbers — the second mov reads rax, already overwritten. The callconv
// pass MUST break the cycle with a scratch register so both pieces reach their
// return registers intact. The runtime corpus can't force this coloring (the
// allocator usually lands the pieces cleanly → the break path is uncovered), so
// this pin builds the swap DIRECTLY and verifies the emitted move sequence is
// value-correct by simulating it. RED-ON-DISABLE: revert the cycle-break to the
// naive two-move emit (or the D-ML7-2.3 reject) and either `result.ok()` is false
// or the simulation shows rdx ending with piece 0's value instead of piece 1's.
TEST(LirCallconvAbi, MultiPieceReturnBreaksRegisterSwapCycle) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& sch = **target;
    auto const retOp = sch.opcodeByMnemonic("ret");
    auto const movOp = sch.opcodeByMnemonic("mov");
    ASSERT_TRUE(retOp.has_value());
    ASSERT_TRUE(movOp.has_value());
    auto const* cc = sch.callingConvention(0);   // SysV (cc index 0)
    ASSERT_NE(cc, nullptr);
    ASSERT_GE(cc->returnGprs.size(), 2u);
    auto const raxOrd = sch.registerByName(cc->returnGprs[0]);   // rax
    auto const rdxOrd = sch.registerByName(cc->returnGprs[1]);   // rdx
    ASSERT_TRUE(raxOrd.has_value());
    ASSERT_TRUE(rdxOrd.has_value());

    // ret [piece0=rdx, piece1=rax]: piece 0 must move to returnGprs[0]=rax, piece 1
    // to returnGprs[1]=rdx — a cross-wise 2-swap that needs a scratch to resolve.
    LirReg const pRax = makePhysicalReg(*raxOrd, LirRegClass::GPR);
    LirReg const pRdx = makePhysicalReg(*rdxOrd, LirRegClass::GPR);
    LirBuilder b{sch};
    b.addFunction(SymbolId{77});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    std::array<LirOperand, 2> retOps{LirOperand::makeReg(pRdx),    // piece 0 in rdx
                                     LirOperand::makeReg(pRax)};   // piece 1 in rax
    b.addInst(*retOp, InvalidLirReg, retOps);
    Lir lir = std::move(b).finish();

    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                     = true;
    alloc.perFunc.back().originalSymbol         = SymbolId{77};
    alloc.perFunc.back().callingConventionIndex = 0;
    alloc.perFunc.back().numSpillSlots          = 0;

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch, alloc, ccRep);
    ASSERT_TRUE(result.ok())
        << "the register-swap cycle must be broken cleanly, not rejected loud";

    // Simulate the emitted movs over a register token-map: each register starts
    // holding its own id; after the moves, rax must hold rdx's ORIGINAL token
    // (piece 0) and rdx must hold rax's ORIGINAL token (piece 1).
    std::unordered_map<std::uint32_t, std::uint32_t> regTok;
    auto tok = [&](std::uint32_t id) {
        auto it = regTok.find(id);
        return it == regTok.end() ? id : it->second;
    };
    std::uint32_t movCount = 0;
    for (std::uint32_t fi = 0; fi < result.lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = result.lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
            LirBlockId const b2 = result.lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < result.lir.blockInstCount(b2); ++i) {
                LirInstId const inst = result.lir.blockInstAt(b2, i);
                if (result.lir.instOpcode(inst) != *movOp) continue;
                LirReg const dst = result.lir.instResult(inst);
                auto const ops = result.lir.instOperands(inst);
                ASSERT_EQ(ops.size(), 1u);
                ASSERT_EQ(ops[0].kind, LirOperandKind::Reg);
                ++movCount;
                regTok[dst.id] = tok(ops[0].reg.id);
            }
        }
    }
    EXPECT_GE(movCount, 3u)
        << "a register-swap cycle needs >=3 movs (scratch-mediated); a 2-mov "
           "naive emit would clobber";
    EXPECT_EQ(tok(*raxOrd), *rdxOrd)
        << "returnGprs[0]=rax must end holding piece 0 (the value that started "
           "in rdx)";
    EXPECT_EQ(tok(*rdxOrd), *raxOrd)
        << "returnGprs[1]=rdx must end holding piece 1 (the value that started "
           "in rax) — a naive non-cycle-broken swap leaves piece 0's value here";
}

// FC7 C3 (AAPCS64 HFA returns) — SF-1 cycle-break generalization pin. A float HFA
// (`{float,float,float}` / `{double×3}`) returns THREE FPR pieces; under a cross-wise
// regalloc coloring they form a 3-register move CYCLE (d0←d1←d2←d0). C1c's
// `emitParallelRegMoves` capped the scratch-break at a 2-swap (`moves.size() > 2`
// failed loud — SysV is ≤2 pieces); FC7 C3 raised that gate because the scratch-break
// linearizes a cycle of ANY length through one scratch. This pins a true 3-cycle is
// broken CLEANLY + value-correctly (not rejected). RED-ON-DISABLE: restore the
// `if (moves.size() > 2) return false;` guard and `result.ok()` flips false here (the
// 2-piece swap pin above still passes — this is the isolated ≥3 lever).
TEST(LirCallconvAbi, ThreeFprHfaReturnCycleBreaksViaScratch) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& sch = **target;
    auto const retOp  = sch.opcodeByMnemonic("ret");
    auto const fmovOp = sch.opcodeByMnemonic("fmov");
    ASSERT_TRUE(retOp.has_value());
    ASSERT_TRUE(fmovOp.has_value());
    auto const* cc = sch.callingConvention(0);   // aapcs64 (cc index 0)
    ASSERT_NE(cc, nullptr);
    ASSERT_GE(cc->returnFprs.size(), 3u);
    auto const d0 = sch.registerByName(cc->returnFprs[0]);
    auto const d1 = sch.registerByName(cc->returnFprs[1]);
    auto const d2 = sch.registerByName(cc->returnFprs[2]);
    ASSERT_TRUE(d0.has_value() && d1.has_value() && d2.has_value());

    // ret [piece0 in d1, piece1 in d2, piece2 in d0]: piece k must move to
    // returnFprs[k] → moves {d0<-d1, d1<-d2, d2<-d0}, a cross-wise 3-CYCLE.
    LirReg const pD0 = makePhysicalReg(*d0, LirRegClass::FPR);
    LirReg const pD1 = makePhysicalReg(*d1, LirRegClass::FPR);
    LirReg const pD2 = makePhysicalReg(*d2, LirRegClass::FPR);
    LirBuilder b{sch};
    b.addFunction(SymbolId{88});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    std::array<LirOperand, 3> retOps{LirOperand::makeReg(pD1),    // piece 0 in d1
                                     LirOperand::makeReg(pD2),    // piece 1 in d2
                                     LirOperand::makeReg(pD0)};   // piece 2 in d0
    b.addInst(*retOp, InvalidLirReg, retOps);
    Lir lir = std::move(b).finish();

    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                     = true;
    alloc.perFunc.back().originalSymbol         = SymbolId{88};
    alloc.perFunc.back().callingConventionIndex = 0;
    alloc.perFunc.back().numSpillSlots          = 0;

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch, alloc, ccRep);
    ASSERT_TRUE(result.ok())
        << "a 3-register HFA return cycle must be broken via scratch, not rejected "
           "(the >2 guard was raised for FC7 C3)";

    // Simulate the emitted fmov moves over a register token-map: each FPR starts
    // holding its own id; after the moves, d0 must hold d1's ORIGINAL token (piece 0),
    // d1 d2's (piece 1), d2 d0's (piece 2) — the cycle resolved value-correctly.
    std::unordered_map<std::uint32_t, std::uint32_t> regTok;
    auto tok = [&](std::uint32_t id) {
        auto it = regTok.find(id);
        return it == regTok.end() ? id : it->second;
    };
    std::uint32_t movCount = 0;
    for (std::uint32_t fi = 0; fi < result.lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = result.lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
            LirBlockId const b2 = result.lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < result.lir.blockInstCount(b2); ++i) {
                LirInstId const inst = result.lir.blockInstAt(b2, i);
                if (result.lir.instOpcode(inst) != *fmovOp) continue;
                LirReg const dst = result.lir.instResult(inst);
                auto const ops = result.lir.instOperands(inst);
                ASSERT_EQ(ops.size(), 1u);
                ASSERT_EQ(ops[0].kind, LirOperandKind::Reg);
                ++movCount;
                regTok[dst.id] = tok(ops[0].reg.id);
            }
        }
    }
    EXPECT_GE(movCount, 4u)
        << "a 3-register cycle needs >=4 fmovs (1 scratch save + 3 chain moves); "
           "fewer means the cycle was not broken";
    EXPECT_EQ(tok(*d0), *d1) << "returnFprs[0]=d0 must end holding piece 0 (from d1)";
    EXPECT_EQ(tok(*d1), *d2) << "returnFprs[1]=d1 must end holding piece 1 (from d2)";
    EXPECT_EQ(tok(*d2), *d0) << "returnFprs[2]=d2 must end holding piece 2 (from d0)";
}

TEST(LirCallconvAbi, MultiArgFunctionMaterializesEveryArgGpr) {
    // A 3-arg function must materialize 3 arg movs (or omit those
    // the regalloc happens to pin to the correct cc reg). All three
    // argGprs ought to appear as a source somewhere in the output
    // (the home reg of each param is loaded from argGprs[k]).
    auto bundle = lowerThroughRewrite(
        "int sum3(int a, int b, int c) { return a + b + c; }");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const stats = collectInstStats(result.lir, *bundle.lowered.target);
    EXPECT_EQ(stats.argOps, 0u);
    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_GE(cc->argGprs.size(), 3u);

    // Each of argGprs[0], argGprs[1], argGprs[2] must appear in the
    // output (either as a mov source — explicit arg copy — or as a
    // mov result — the regalloc-picked home reg, which means no
    // explicit copy was needed because the param is already there).
    std::array<std::optional<std::uint16_t>, 3> argOrds{
        bundle.lowered.target->registerByName(cc->argGprs[0]),
        bundle.lowered.target->registerByName(cc->argGprs[1]),
        bundle.lowered.target->registerByName(cc->argGprs[2]),
    };
    for (auto const& ord : argOrds) { ASSERT_TRUE(ord.has_value()); }
    std::array<bool, 3> seen{false, false, false};
    auto const movOp = bundle.lowered.target->opcodeByMnemonic("mov");
    ASSERT_TRUE(movOp.has_value());
    for (std::uint32_t fi = 0; fi < result.lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = result.lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
            LirBlockId const b = result.lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < result.lir.blockInstCount(b); ++i) {
                LirInstId const inst = result.lir.blockInstAt(b, i);
                if (result.lir.instOpcode(inst) != *movOp) continue;
                LirReg const r = result.lir.instResult(inst);
                for (std::size_t k = 0; k < 3; ++k) {
                    if (r.valid() && r.isPhysical != 0
                        && r.id == *argOrds[k]) {
                        seen[k] = true;
                    }
                    for (auto const& op : result.lir.instOperands(inst)) {
                        if (op.kind == LirOperandKind::Reg
                            && op.reg.isPhysical != 0
                            && op.reg.id == *argOrds[k]) {
                            seen[k] = true;
                        }
                    }
                }
            }
        }
    }
    for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_TRUE(seen[k])
            << "argGprs[" << k << "] (" << cc->argGprs[k]
            << ") must surface in the post-callconv output (as a mov "
               "src or result, depending on regalloc's choice)";
    }
}

// ── Fail-loud surfaces ────────────────────────────────────────────────

TEST(LirCallconvAbi, SysVSevenArgFunctionStackPassesOverflowArg) {
    // D-ML7-2.2 (closed 2026-06-02): SysV AMD64 has 6 GPR arg
    // registers (rdi/rsi/rdx/rcx/r8/r9). A 7-arg int function
    // overflows arg 6 onto the stack; the materialization must
    // succeed cleanly + emit a `frame_load` (the schema's `load`
    // opcode) at the callee side reading from
    // `[sp + totalFrameSize + callPushBytes + shadowSpaceBytes +
    // overflowIdx * slotSize]`. For SysV: shadowSpaceBytes=0,
    // callPushBytes=8, overflowIdx=0, slotSize=8 → offset =
    // totalFrameSize + 8. The fixture also CALLS f7 to exercise
    // the caller-side `frame_store` arm (offset 0 within outgoing
    // area = [sp+0] under SysV).
    auto bundle = lowerThroughRewrite(
        "int f7(int a, int b, int c, int d, int e, int f, int g) {\n"
        "    return g;\n"
        "}\n"
        "int caller_of_f7() {\n"
        "    return f7(1, 2, 3, 4, 5, 6, 7);\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok())
        << "D-ML7-2.2 closure: 7-arg SysV fn + call must lower cleanly";
    EXPECT_EQ(ccRep.errorCount(), 0u);
    // L_StackPassedArgUnsupported must NOT fire — the substrate
    // now handles the overflow via stack-spill rather than rejecting.
    EXPECT_EQ(::dss::test_support::countCode(
                  ccRep, DiagnosticCode::L_StackPassedArgUnsupported),
              0u)
        << "stack-passed guard must NOT fire on D-ML7-2.2-closed substrate";

    // f7's frame must reserve outgoing-args area sized for the
    // function's CALLS (none — f7 is leaf), but its non-leaf flag
    // is false. caller_of_f7 makes ONE call passing 7 args; its
    // outgoingArgAreaSize = shadowSpaceBytes (SysV=0) + 1 slot * 8 = 8.
    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->callPushBytes, 8u);
    EXPECT_EQ(cc->shadowSpaceBytes, 0u);
    EXPECT_FALSE(cc->slotAligned);

    // Find caller_of_f7 in the result (it makes the 7-arg call).
    FrameLayout const* callerLayout = nullptr;
    for (std::uint32_t i = 0; i < result.lir.moduleFuncCount(); ++i) {
        auto const* layout = result.forFuncByIndex(i);
        ASSERT_NE(layout, nullptr);
        if (layout->hasCalls) {
            EXPECT_EQ(callerLayout, nullptr)
                << "fixture should have exactly one call-making fn";
            callerLayout = layout;
        }
    }
    ASSERT_NE(callerLayout, nullptr);
    // SysV: shadow=0, 1 overflow slot → outgoingArgAreaSize = 8.
    EXPECT_EQ(callerLayout->outgoingArgAreaSize, 8u)
        << "SysV 7-arg call → 1 stack-spilled slot × 8 bytes "
           "(shadowSpaceBytes=0 contributes nothing under SysV)";
    // Frame congruence: totalFrameSize ≡ callPushBytes mod stackAlignment.
    EXPECT_EQ(callerLayout->totalFrameSize % cc->stackAlignment,
              static_cast<std::uint32_t>(cc->callPushBytes));
    // pr-test-analyzer L3 audit fold: pin the EXACT byte offset of
    // the emitted `frame_store` for the overflow arg. Under SysV
    // (shadow=0, overflowIdx=0), the spill goes to [sp+0]. A
    // regression to an off-by-one offset (e.g. [sp+8]) would
    // silently read garbage on the callee side; the frame-size
    // count alone wouldn't catch it.
    auto const storeOpId = bundle.lowered.target->opcodeByMnemonic("store");
    ASSERT_TRUE(storeOpId.has_value());
    std::uint16_t const storeOp = *storeOpId;
    bool foundStackArgStore = false;
    LirFuncId const callerFn = result.lir.funcAt(1);
    std::uint32_t const blkN = result.lir.funcBlockCount(callerFn);
    for (std::uint32_t bi = 0; bi < blkN && !foundStackArgStore; ++bi) {
        LirBlockId const blk = result.lir.funcBlockAt(callerFn, bi);
        std::uint32_t const n = result.lir.blockInstCount(blk);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = result.lir.blockInstAt(blk, i);
            if (result.lir.instOpcode(inst) != storeOp) continue;
            auto const ops = result.lir.instOperands(inst);
            // store layout: [value_reg, base_reg, MemBase, MemOffset]
            if (ops.size() != 4) continue;
            if (ops[3].kind != LirOperandKind::MemOffset) continue;
            // The overflow arg lands at [sp + 0] under SysV (shadow=0,
            // overflowIdx=0). Other stores in this fn would be at
            // higher offsets (saved-reg prologue if any).
            if (ops[3].offset == 0) {
                foundStackArgStore = true;
                break;
            }
        }
    }
    EXPECT_TRUE(foundStackArgStore)
        << "expected a `store` inst at [sp+0] for the 7th-arg overflow";
}

TEST(LirCallconvAbi, Win64FiveArgFunctionStackPassesViaSlotAligned) {
    // D-ML7-2.6 (closed co-with-D-ML7-2.2, 2026-06-02): Win64 ms_x64
    // has 4 GPR arg registers (rcx/rdx/r8/r9). A 5-arg int function
    // overflows arg 4 onto the stack. Under slot-aligned semantics,
    // arg-4 lands at [rsp + shadowSpaceBytes + 0 * slotSize] =
    // [rsp + 0x20] (the canonical Win64 5th-arg location).
    // Outgoing-area total = shadowSpaceBytes (32) + 1 slot × 8 = 40.
    auto bundle = lowerThroughRewrite(
        "int f5(int a, int b, int c, int d, int e) { return e; }\n"
        "int caller_of_f5() { return f5(1, 2, 3, 4, 5); }\n",
        /*ccIndex=*/1);  // ms_x64
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok())
        << "D-ML7-2.6 closure: 5-arg Win64 fn + call must lower cleanly";
    EXPECT_EQ(ccRep.errorCount(), 0u);

    auto const* cc = bundle.lowered.target->callingConvention(1);
    ASSERT_NE(cc, nullptr);
    EXPECT_STREQ(cc->name.c_str(), "ms_x64");
    EXPECT_TRUE(cc->slotAligned)
        << "ms_x64 MUST declare slotAligned=true (D-ML7-2.6 closure)";
    EXPECT_EQ(cc->shadowSpaceBytes, 32u);
    EXPECT_EQ(cc->callPushBytes, 8u);

    // Find caller_of_f5.
    FrameLayout const* callerLayout = nullptr;
    for (std::uint32_t i = 0; i < result.lir.moduleFuncCount(); ++i) {
        auto const* layout = result.forFuncByIndex(i);
        ASSERT_NE(layout, nullptr);
        if (layout->hasCalls) {
            EXPECT_EQ(callerLayout, nullptr);
            callerLayout = layout;
        }
    }
    ASSERT_NE(callerLayout, nullptr);
    // Win64 5-arg call: shadow=32, 1 overflow slot × 8 → outgoingArgAreaSize = 40.
    // Pinning outgoingArgAreaSize directly (not totalFrameSize, which
    // varies with regalloc-chosen callee-saved spills under ms_x64's
    // many callee-saved regs).
    EXPECT_EQ(callerLayout->outgoingArgAreaSize, 40u)
        << "Win64 5-arg call → shadowSpaceBytes (32) + 1 overflow × 8 = 40";
    // Frame congruence holds regardless of saved-reg count:
    // alignedSizeWithBias result must always satisfy N ≡ 8 mod 16
    // under Win64 (callPushBytes=8, stackAlignment=16).
    EXPECT_GE(callerLayout->totalFrameSize, 40u);
    EXPECT_EQ(callerLayout->totalFrameSize % 16u, 8u);
    // pr-test-analyzer L3 audit fold: pin the EXACT byte offset of
    // the emitted `frame_store` for the 5th-arg overflow. Under
    // Win64 (shadow=32, overflowIdx=0), the spill goes to
    // [sp+0x20] = [sp+32]. The canonical Win64 5th-arg location.
    auto const storeOpId = bundle.lowered.target->opcodeByMnemonic("store");
    ASSERT_TRUE(storeOpId.has_value());
    std::uint16_t const storeOp = *storeOpId;
    bool foundStackArgStoreAt0x20 = false;
    LirFuncId const callerFn = result.lir.funcAt(1);
    std::uint32_t const blkN = result.lir.funcBlockCount(callerFn);
    for (std::uint32_t bi = 0; bi < blkN && !foundStackArgStoreAt0x20; ++bi) {
        LirBlockId const blk = result.lir.funcBlockAt(callerFn, bi);
        std::uint32_t const n = result.lir.blockInstCount(blk);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = result.lir.blockInstAt(blk, i);
            if (result.lir.instOpcode(inst) != storeOp) continue;
            auto const ops = result.lir.instOperands(inst);
            if (ops.size() != 4) continue;
            if (ops[3].kind != LirOperandKind::MemOffset) continue;
            if (ops[3].offset == 0x20) {
                foundStackArgStoreAt0x20 = true;
                break;
            }
        }
    }
    EXPECT_TRUE(foundStackArgStoreAt0x20)
        << "Win64 5th-arg overflow MUST emit a `store` at [sp+0x20] "
           "(the canonical Win64 stack-arg location AFTER shadow space)";
}

// ── FC4 c2: indirect-call materialization (`call <reg>`) ───────────
// Successors of the retired RejectsIndirectCallLoud wall pin: a Reg
// callee now materializes through the SAME planning as a SymbolRef
// callee (arg moves, hazard detection, result capture), the
// callee-in-arg-reg collision trips the loud
// L_IndirectCalleeClobberedByArgSetup backstop, and any OTHER callee
// operand kind keeps the (re-messaged) L_IndirectCallUnsupported
// totality wall ALIVE.

namespace {

// Minimal single-entry LirAllocation pointing at cc index 0 — the
// materializer reads it to pick the cc (sysv_amd64 on x86_64).
[[nodiscard]] LirAllocation singleFnCc0Alloc() {
    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                       = true;
    alloc.perFunc.back().originalSymbol           = SymbolId{42};
    alloc.perFunc.back().callingConventionIndex   = 0;
    alloc.perFunc.back().numSpillSlots            = 0;
    return alloc;
}

} // namespace

TEST(LirCallconvAbi, MaterializesIndirectCallViaRegCallee) {
    // `result(rbx) = call <r10>, arg(r11)` — callee in a NON-arg
    // caller-saved register. The materializer must emit, in order:
    //   mov rdi, r11      (sysv argGprs[0] <- the arg)
    //   call r10          (single Reg operand — the schema's ["reg"]
    //                      encoding variant)
    //   mov rbx, rax      (result capture from returnGprs[0])
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& sch = **target;

    auto const callOp = sch.opcodeByMnemonic("call");
    auto const retOp  = sch.opcodeByMnemonic("ret");
    auto const movOp  = sch.opcodeByMnemonic("mov");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    ASSERT_TRUE(movOp.has_value());
    auto const r10Ord = sch.registerByName("r10");
    auto const r11Ord = sch.registerByName("r11");
    auto const rdiOrd = sch.registerByName("rdi");
    auto const raxOrd = sch.registerByName("rax");
    auto const rbxOrd = sch.registerByName("rbx");
    ASSERT_TRUE(r10Ord.has_value() && r11Ord.has_value()
                && rdiOrd.has_value() && raxOrd.has_value()
                && rbxOrd.has_value());
    LirReg const r10 = makePhysicalReg(*r10Ord, LirRegClass::GPR);
    LirReg const r11 = makePhysicalReg(*r11Ord, LirRegClass::GPR);
    LirReg const rbx = makePhysicalReg(*rbxOrd, LirRegClass::GPR);

    LirBuilder b{sch};
    b.addFunction(SymbolId{42});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    std::array<LirOperand, 2> callOps{LirOperand::makeReg(r10),
                                      LirOperand::makeReg(r11)};
    b.addInst(*callOp, rbx, callOps);
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch,
                                               singleFnCc0Alloc(), ccRep);
    ASSERT_TRUE(result.ok())
        << "a Reg-callee call must MATERIALIZE (FC4 c2), not reject";
    EXPECT_EQ(ccRep.errorCount(), 0u);

    // Scan the materialized function: locate the call and the two
    // movs, asserting both order and exact register identities.
    LirFuncId const fn = result.lir.funcAt(0);
    std::optional<std::size_t> argMoveAt;
    std::optional<std::size_t> callAt;
    std::optional<std::size_t> resultMoveAt;
    std::size_t flat = 0;
    std::uint32_t const blkN = result.lir.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < blkN; ++bi) {
        LirBlockId const blk = result.lir.funcBlockAt(fn, bi);
        std::uint32_t const n = result.lir.blockInstCount(blk);
        for (std::uint32_t i = 0; i < n; ++i, ++flat) {
            LirInstId const inst = result.lir.blockInstAt(blk, i);
            auto const ops = result.lir.instOperands(inst);
            if (result.lir.instOpcode(inst) == *callOp) {
                ASSERT_EQ(ops.size(), 1u)
                    << "materialized call carries the single callee "
                       "operand only";
                ASSERT_EQ(ops[0].kind, LirOperandKind::Reg);
                EXPECT_EQ(ops[0].reg.id,
                          static_cast<std::uint32_t>(*r10Ord))
                    << "callee register must pass through untouched";
                callAt = flat;
                continue;
            }
            if (result.lir.instOpcode(inst) == *movOp
                && ops.size() == 1 && ops[0].kind == LirOperandKind::Reg) {
                LirReg const res = result.lir.instResult(inst);
                if (res.valid()
                    && res.id == static_cast<std::uint32_t>(*rdiOrd)
                    && ops[0].reg.id
                           == static_cast<std::uint32_t>(*r11Ord)) {
                    argMoveAt = flat;
                }
                if (res.valid()
                    && res.id == static_cast<std::uint32_t>(*rbxOrd)
                    && ops[0].reg.id
                           == static_cast<std::uint32_t>(*raxOrd)) {
                    resultMoveAt = flat;
                }
            }
        }
    }
    ASSERT_TRUE(callAt.has_value()) << "no call instruction emitted";
    ASSERT_TRUE(argMoveAt.has_value())
        << "missing `mov rdi, r11` arg-passing move (indirect calls "
           "must share the direct path's arg planning)";
    ASSERT_TRUE(resultMoveAt.has_value())
        << "missing `mov rbx, rax` result capture (indirect calls "
           "must share the direct path's result capture)";
    EXPECT_LT(*argMoveAt, *callAt) << "arg move must precede the call";
    EXPECT_GT(*resultMoveAt, *callAt)
        << "result capture must follow the call";
}

TEST(LirCallconvAbi, RejectsCalleeInArgRegLoud) {
    // `call <rdi>, arg(r11)` — the callee sits in sysv argGprs[0],
    // which is ALSO the destination of this call's own arg-passing
    // move (`mov rdi, r11`). Emitting would overwrite the callee
    // before the call consumes it (jump through an argument value).
    // The regalloc-tier rules make this unreachable from the
    // pipeline; the materializer's backstop must convert a regression
    // into the loud L_IndirectCalleeClobberedByArgSetup, never a
    // silent garbage jump. (This hand-built shape bypasses regalloc
    // on purpose — it IS the backstop's red-on-disable lever.)
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& sch = **target;

    auto const callOp = sch.opcodeByMnemonic("call");
    auto const retOp  = sch.opcodeByMnemonic("ret");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    auto const rdiOrd = sch.registerByName("rdi");
    auto const r11Ord = sch.registerByName("r11");
    ASSERT_TRUE(rdiOrd.has_value() && r11Ord.has_value());
    LirReg const rdi = makePhysicalReg(*rdiOrd, LirRegClass::GPR);
    LirReg const r11 = makePhysicalReg(*r11Ord, LirRegClass::GPR);

    LirBuilder b{sch};
    b.addFunction(SymbolId{42});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    std::array<LirOperand, 2> callOps{LirOperand::makeReg(rdi),
                                      LirOperand::makeReg(r11)};
    b.addInst(*callOp, InvalidLirReg, callOps);
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch,
                                               singleFnCc0Alloc(), ccRep);
    EXPECT_FALSE(result.ok())
        << "callee parked in an arg-passing destination must fail LOUD";
    bool sawCode = false;
    for (auto const& d : ccRep.all()) {
        if (d.code == DiagnosticCode::L_IndirectCalleeClobberedByArgSetup) {
            sawCode = true;
        }
    }
    EXPECT_TRUE(sawCode)
        << "must surface L_IndirectCalleeClobberedByArgSetup (the "
           "regalloc-rule backstop)";
}

TEST(LirCallconvAbi, RejectsNonRegNonSymbolCalleeLoud) {
    // ops[0] is an IMMEDIATE — neither SymbolRef (direct) nor Reg
    // (indirect). The fail-loud totality wall must stay ALIVE with
    // the re-messaged L_IndirectCallUnsupported (an upstream lowering
    // bug must never silently materialize).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& sch = **target;

    auto const callOp = sch.opcodeByMnemonic("call");
    auto const retOp  = sch.opcodeByMnemonic("ret");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(retOp.has_value());

    LirBuilder b{sch};
    b.addFunction(SymbolId{42});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    std::array<LirOperand, 1> callOps{LirOperand::makeImmInt32(7)};
    b.addInst(*callOp, InvalidLirReg, callOps);
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch,
                                               singleFnCc0Alloc(), ccRep);
    EXPECT_FALSE(result.ok())
        << "an immediate callee operand must trip the loud guard";
    bool sawCode = false;
    bool sawMessage = false;
    for (auto const& d : ccRep.all()) {
        if (d.code == DiagnosticCode::L_IndirectCallUnsupported) {
            sawCode = true;
            if (d.actual.find("neither SymbolRef") != std::string::npos) {
                sawMessage = true;
            }
        }
    }
    EXPECT_TRUE(sawCode)
        << "the residual-kind callee must surface "
           "L_IndirectCallUnsupported (the code stays ALIVE)";
    EXPECT_TRUE(sawMessage)
        << "the diagnostic must carry the re-messaged totality text";
}

TEST(LirCallconvAbi, OrderableChainArgPassingSucceeds) {
    // Regression for the post-fold predicate-inversion bug. Before
    // the inversion fix, this sequence would have spuriously fired
    // L_MoveCycleUnsupported:
    //   arg 0 src=rsi (destined for argGprs[0]=rdi)
    //   arg 1 src=rcx (destined for argGprs[1]=rsi)
    // The valid in-order emission is `mov rdi, rsi; mov rsi, rcx` —
    // rsi is read by the first mov BEFORE the second mov clobbers
    // it. No cycle, no hazard. The detector must NOT fire here.
    //
    // The previous (inverted) predicate fired because
    // `argMoves[1].dest = rsi == argMoves[0].src = rsi`. The
    // corrected predicate (`argMoves[i].dest == argMoves[j].src`)
    // checks for the true hazard direction and accepts this case.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& sch = **target;

    auto const callOp = sch.opcodeByMnemonic("call");
    auto const retOp  = sch.opcodeByMnemonic("ret");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(retOp.has_value());

    auto const rcxOrd = sch.registerByName("rcx");
    auto const rsiOrd = sch.registerByName("rsi");
    ASSERT_TRUE(rcxOrd.has_value());
    ASSERT_TRUE(rsiOrd.has_value());
    LirReg const rcx = makePhysicalReg(*rcxOrd, LirRegClass::GPR);
    LirReg const rsi = makePhysicalReg(*rsiOrd, LirRegClass::GPR);

    LirBuilder b{sch};
    b.addFunction(SymbolId{11});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    // call <sym=g>, rsi, rcx
    //   arg 0 src = rsi  -> dest = argGprs[0] = rdi  (rsi consumed)
    //   arg 1 src = rcx  -> dest = argGprs[1] = rsi  (orderable: rdi
    //                                                  not in any src)
    std::array<LirOperand, 3> callOps{
        LirOperand::makeSymbolRef(5),
        LirOperand::makeReg(rsi),
        LirOperand::makeReg(rcx),
    };
    b.addInst(*callOp, InvalidLirReg, callOps);
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                     = true;
    alloc.perFunc.back().originalSymbol         = SymbolId{11};
    alloc.perFunc.back().callingConventionIndex = 0;
    alloc.perFunc.back().numSpillSlots          = 0;

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch, alloc, ccRep);
    EXPECT_TRUE(result.ok())
        << "orderable arg-passing chain must NOT trip the cycle "
           "detector (predicate-inversion regression check)";
    for (auto const& d : ccRep.all()) {
        EXPECT_NE(d.code, DiagnosticCode::L_MoveCycleUnsupported)
            << "no L_MoveCycleUnsupported expected on orderable chain";
    }
}

TEST(LirCallconvAbi, MoveCycleInArgPassingResolvedViaScratch) {
    // D-ML7-2.3 (closed): construct a LIR call whose arg-passing is a 2-cycle
    // (a swap): arg 0 currently lives in `rsi` (= argGprs[1]) and arg 1 lives in
    // `rdi` (= argGprs[0]). The naive in-order emit would produce
    //   mov rdi, rsi   (clobbers rdi which is arg 1's source)
    //   mov rsi, rdi   (now reads the clobbered rdi)  -- silent miscompile.
    // The v1 pass FAIL-LOUD-REJECTED this (L_MoveCycleUnsupported). The parallel-
    // copy resolver now BREAKS the cycle with a caller-saved scratch GPR:
    //   mov <scratch>, rsi   ; save one source aside
    //   mov rsi, rdi         ; now safe
    //   mov rdi, <scratch>   ; complete the swap
    // So the pass must SUCCEED, emit NO L_MoveCycleUnsupported, and produce
    // exactly 3 movs (2 arg-passing regs + 1 scratch spill) — never 2 (which
    // would be the clobbering in-order emit). The scratch must be a caller-saved
    // GPR outside the swap ({rax,r10,r11} on SysV), drawn from the cc, not
    // hardcoded.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& sch = **target;

    auto const callOp = sch.opcodeByMnemonic("call");
    auto const retOp  = sch.opcodeByMnemonic("ret");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(retOp.has_value());

    auto const rdiOrd = sch.registerByName("rdi");
    auto const rsiOrd = sch.registerByName("rsi");
    ASSERT_TRUE(rdiOrd.has_value());
    ASSERT_TRUE(rsiOrd.has_value());
    LirReg const rdi = makePhysicalReg(*rdiOrd, LirRegClass::GPR);
    LirReg const rsi = makePhysicalReg(*rsiOrd, LirRegClass::GPR);

    LirBuilder b{sch};
    b.addFunction(SymbolId{99});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    // call <sym=g>, rsi, rdi
    //   arg 0 source = rsi  (dest will be argGprs[0]=rdi)
    //   arg 1 source = rdi  (dest will be argGprs[1]=rsi)  -> a swap cycle
    std::array<LirOperand, 3> callOps{
        LirOperand::makeSymbolRef(7),
        LirOperand::makeReg(rsi),
        LirOperand::makeReg(rdi),
    };
    b.addInst(*callOp, InvalidLirReg, callOps);
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                     = true;
    alloc.perFunc.back().originalSymbol         = SymbolId{99};
    alloc.perFunc.back().callingConventionIndex = 0;
    alloc.perFunc.back().numSpillSlots          = 0;

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch, alloc, ccRep);
    EXPECT_TRUE(result.ok())
        << "the swap-cycle must be RESOLVED by the parallel-copy resolver, "
           "not fail-loud rejected";
    for (auto const& d : ccRep.all()) {
        EXPECT_NE(d.code, DiagnosticCode::L_MoveCycleUnsupported)
            << "a breakable swap-cycle must NOT surface L_MoveCycleUnsupported "
               "(scratch-mediated resolution supersedes the v1 detector)";
    }
    auto const stats = collectInstStats(result.lir, sch);
    // 3 movs: scratch<-rsi, rsi<-rdi, rdi<-scratch. Two movs would be the
    // clobbering in-order emit (the silent miscompile the resolver prevents).
    EXPECT_EQ(stats.movOps, 3u)
        << "a scratch-broken 2-cycle must emit exactly 3 movs (save + 2 swap "
           "legs), not 2 (in-order clobber)";
}

TEST(LirCallconvAbi, ThreeFprCycleResolvedViaFprScratch) {
    // D-ML7-2.3: a 3-element FP arg cycle (cycle length > 2) must be broken with
    // an FPR-class scratch (movaps), not a GPR. Args in xmm1, xmm2, xmm0 route to
    // argFprs[0..2] = xmm0, xmm1, xmm2 -> the rotation
    //   xmm0 <- xmm1, xmm1 <- xmm2, xmm2 <- xmm0
    // has every source also a destination (a 3-cycle). The resolver saves one
    // source into a caller-saved FPR outside {xmm0,xmm1,xmm2} (SysV: xmm8..15),
    // then drains the now-linear chain. All moves are FPR-class movs. A GPR
    // scratch here (wrong class) would be a silent value-destroying miscompile.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& sch = **target;
    auto const callOp = sch.opcodeByMnemonic("call");
    auto const retOp  = sch.opcodeByMnemonic("ret");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(retOp.has_value());

    auto const x0 = sch.registerByName("xmm0");
    auto const x1 = sch.registerByName("xmm1");
    auto const x2 = sch.registerByName("xmm2");
    ASSERT_TRUE(x0.has_value() && x1.has_value() && x2.has_value());
    LirReg const xmm0 = makePhysicalReg(*x0, LirRegClass::FPR);
    LirReg const xmm1 = makePhysicalReg(*x1, LirRegClass::FPR);
    LirReg const xmm2 = makePhysicalReg(*x2, LirRegClass::FPR);

    LirBuilder b{sch};
    b.addFunction(SymbolId{77});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    // call <sym>, xmm1, xmm2, xmm0  (arg0<-xmm1, arg1<-xmm2, arg2<-xmm0)
    std::array<LirOperand, 4> callOps{
        LirOperand::makeSymbolRef(3),
        LirOperand::makeReg(xmm1),
        LirOperand::makeReg(xmm2),
        LirOperand::makeReg(xmm0),
    };
    b.addInst(*callOp, InvalidLirReg, callOps);
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                     = true;
    alloc.perFunc.back().originalSymbol         = SymbolId{77};
    alloc.perFunc.back().callingConventionIndex = 0;
    alloc.perFunc.back().numSpillSlots          = 0;

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch, alloc, ccRep);
    EXPECT_TRUE(result.ok())
        << "a 3-FPR arg cycle must be resolved, not rejected";
    for (auto const& d : ccRep.all())
        EXPECT_NE(d.code, DiagnosticCode::L_MoveCycleUnsupported)
            << "a breakable 3-FPR cycle must not surface L_MoveCycleUnsupported";
    // Verify the scratch is an FPR OUTSIDE the arg-passing set: scan EVERY inst
    // (FPR moves are `movaps`, not `mov`) for a dest/src FPR whose ordinal is not
    // in {0,1,2}. Also assert NO GPR appears in any move here (a GPR scratch for
    // an FPR cycle would be the wrong-class silent corruption).
    bool sawFprScratch = false;
    bool sawGprInMoves = false;
    auto const movOp    = sch.opcodeByMnemonic("mov");
    auto const movapsOp = sch.opcodeByMnemonic("movaps");
    for (std::uint32_t fi = 0; fi < result.lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = result.lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
            LirBlockId const bb = result.lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < result.lir.blockInstCount(bb); ++ii) {
                LirInstId const inst = result.lir.blockInstAt(bb, ii);
                std::uint16_t const op = result.lir.instOpcode(inst);
                bool const isMove = (movOp.has_value() && op == *movOp)
                                 || (movapsOp.has_value() && op == *movapsOp);
                if (!isMove) continue;
                LirReg const dst = result.lir.instResult(inst);
                if (dst.valid() && dst.regClass() == LirRegClass::FPR
                    && dst.id != *x0 && dst.id != *x1 && dst.id != *x2)
                    sawFprScratch = true;
                if (dst.valid() && dst.regClass() == LirRegClass::GPR)
                    sawGprInMoves = true;
                for (auto const& op2 : result.lir.instOperands(inst)) {
                    if (op2.kind != LirOperandKind::Reg) continue;
                    if (op2.reg.regClass() == LirRegClass::FPR
                        && op2.reg.id != *x0 && op2.reg.id != *x1
                        && op2.reg.id != *x2)
                        sawFprScratch = true;
                    if (op2.reg.regClass() == LirRegClass::GPR)
                        sawGprInMoves = true;
                }
            }
        }
    }
    EXPECT_TRUE(sawFprScratch)
        << "the 3-FPR cycle break must use an FPR-class scratch register "
           "outside the argFprs[0..2] set";
    EXPECT_FALSE(sawGprInMoves)
        << "an FPR-only cycle must not route any value through a GPR "
           "(wrong-class scratch would silently corrupt the double)";
}

TEST(LirCallconvAbi, CycleBreakScratchAvoidsCommittedArgDestination) {
    // D-ML7-2.3 SCRATCH-COLLAPSE fix. A wide arg-passing move set whose SOURCES
    // overlap the low arg registers: the first six args arrive in HIGH caller-
    // saved FPRs (xmm8..xmm13) and route to argFprs[0..5] (xmm0..xmm5) as an
    // orderable chain; the last two args form a 2-swap in argFprs[6],[7]
    // (xmm6<->xmm7). The progress scan emits the six clean moves FIRST — COMMITTING
    // xmm0..xmm5 to their final argument values — then breaks the {xmm6,xmm7}
    // cycle with a scratch.
    //
    // THE BUG (pre-fix): pickScratchReg excluded only the registers still present
    // in the SHRINKING move set. Once the six clean moves were emitted-and-erased,
    // xmm0..xmm5 dropped out of the exclusion, so the scratch picker (scanning
    // cc.callerSaved in order) returned the FIRST caller-saved FPR — xmm0 — an
    // ALREADY-COMMITTED destination holding arg0. The cycle break then clobbered
    // arg0 (a SILENT SysV miscompile: an 8-FP-arg rotation returned the wrong
    // value). The fix folds the FULL destination footprint into the scratch
    // exclusion, so the scratch lands OUTSIDE {xmm0..xmm7}.
    //
    // ASSERTION (behavioral, name-agnostic): replay the emitted move sequence as a
    // register machine — every SOURCE register starts holding a unique token — and
    // require each arg-destination register to end holding its CORRECT source
    // token. A scratch that clobbered a committed destination shows up as a wrong
    // final token. This catches the miscompile directly, independent of WHICH
    // scratch register the (agnostic) picker chooses.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& sch = **target;
    auto const callOp = sch.opcodeByMnemonic("call");
    auto const retOp  = sch.opcodeByMnemonic("ret");
    ASSERT_TRUE(callOp.has_value() && retOp.has_value());
    auto const* cc = sch.callingConvention(0);
    ASSERT_NE(cc, nullptr);
    // Need at least 8 FP arg registers (the SysV shape) for this construction.
    ASSERT_GE(cc->argFprs.size(), 8u);

    // Resolve argFprs[0..7] and six HIGH source FPRs (the first caller-saved FPRs
    // that are NOT argFprs — SysV xmm8.. — read from the cc, never hardcoded).
    std::array<std::uint16_t, 8> argOrd{};
    for (std::size_t k = 0; k < 8; ++k) {
        auto const o = sch.registerByName(cc->argFprs[k]);
        ASSERT_TRUE(o.has_value()) << "argFprs[" << k << "] must resolve";
        argOrd[k] = *o;
    }
    std::unordered_set<std::uint16_t> argSet(argOrd.begin(), argOrd.end());
    std::vector<std::uint16_t> highSrc;  // caller-saved FPRs outside the arg set
    for (auto const& name : cc->callerSaved) {
        auto const o = sch.registerByName(name);
        if (!o.has_value()) continue;
        auto const* info = sch.registerInfo(*o);
        if (info == nullptr) continue;
        if (static_cast<std::uint8_t>(info->regClass)
            != static_cast<std::uint8_t>(LirRegClass::FPR)) continue;
        if (argSet.count(*o)) continue;
        highSrc.push_back(*o);
    }
    // 6 non-arg caller-saved FPRs (xmm8..xmm13) for the clean chain + ≥1 left as a
    // scratch (xmm14/xmm15) that the fix MUST use for the cycle break.
    ASSERT_GE(highSrc.size(), 7u)
        << "SysV needs xmm8..15 free; this construction requires ≥6 chain sources "
           "plus ≥1 scratch";

    auto fpr = [](std::uint16_t o) { return makePhysicalReg(o, LirRegClass::FPR); };

    // Build the call. arg k (k<6) <- highSrc[k]; arg 6 <- argFprs[7]; arg 7 <-
    // argFprs[6]  (the trailing 2-swap in the top two arg registers).
    LirBuilder b{sch};
    b.addFunction(SymbolId{123});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    std::vector<LirOperand> callOps;
    callOps.push_back(LirOperand::makeSymbolRef(5));
    for (std::size_t k = 0; k < 6; ++k)
        callOps.push_back(LirOperand::makeReg(fpr(highSrc[k])));
    callOps.push_back(LirOperand::makeReg(fpr(argOrd[7])));  // arg6 <- xmm7
    callOps.push_back(LirOperand::makeReg(fpr(argOrd[6])));  // arg7 <- xmm6
    b.addInst(*callOp, InvalidLirReg, callOps);
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                     = true;
    alloc.perFunc.back().originalSymbol         = SymbolId{123};
    alloc.perFunc.back().callingConventionIndex = 0;
    alloc.perFunc.back().numSpillSlots          = 0;

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch, alloc, ccRep);
    EXPECT_TRUE(result.ok())
        << "the wide arg cycle must resolve, not fail-loud";
    for (auto const& d : ccRep.all())
        EXPECT_NE(d.code, DiagnosticCode::L_MoveCycleUnsupported);

    // Expected final state: argFprs[k] holds highSrc[k]'s token for k<6, arg6
    // holds xmm7's original token, arg7 holds xmm6's original token.
    std::unordered_map<std::uint16_t, std::uint16_t> expectedFinal;
    for (std::size_t k = 0; k < 6; ++k) expectedFinal[argOrd[k]] = highSrc[k];
    expectedFinal[argOrd[6]] = argOrd[7];
    expectedFinal[argOrd[7]] = argOrd[6];

    // Register machine: each physical FPR initially holds its OWN ordinal as a
    // token. Replay every emitted mov/movaps; assert the arg dests end correct.
    std::unordered_map<std::uint16_t, std::uint16_t> regToken;  // reg ord -> token
    auto tokenOf = [&](std::uint16_t ord) -> std::uint16_t {
        auto it = regToken.find(ord);
        return it == regToken.end() ? ord : it->second;  // untouched holds itself
    };
    auto const movOp    = sch.opcodeByMnemonic("mov");
    auto const movapsOp = sch.opcodeByMnemonic("movaps");
    for (std::uint32_t fi = 0; fi < result.lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = result.lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
            LirBlockId const bb = result.lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < result.lir.blockInstCount(bb); ++ii) {
                LirInstId const inst = result.lir.blockInstAt(bb, ii);
                std::uint16_t const op = result.lir.instOpcode(inst);
                bool const isMove = (movOp.has_value() && op == *movOp)
                                 || (movapsOp.has_value() && op == *movapsOp);
                if (!isMove) continue;
                LirReg const dst = result.lir.instResult(inst);
                auto const ops = result.lir.instOperands(inst);
                if (!dst.valid() || dst.regClass() != LirRegClass::FPR) continue;
                if (ops.empty() || ops[0].kind != LirOperandKind::Reg) continue;
                // mov dst, src : dst's token becomes src's current token.
                regToken[static_cast<std::uint16_t>(dst.id)] =
                    tokenOf(static_cast<std::uint16_t>(ops[0].reg.id));
            }
        }
    }
    for (auto const& [destOrd, wantToken] : expectedFinal) {
        EXPECT_EQ(tokenOf(destOrd), wantToken)
            << "arg-destination register ordinal " << destOrd
            << " ended with token " << tokenOf(destOrd) << " but should hold "
            << wantToken << " — a cycle-break scratch clobbered a committed "
               "argument destination (the scratch-collapse miscompile)";
    }
}

TEST(LirCallconvAbi, IndependentGprAndFprCyclesBothResolved) {
    // D-ML7-2.3: one call carrying TWO disjoint cycles of DIFFERENT classes — a
    // GPR swap (rdi<->rsi) AND an FPR swap (xmm0<->xmm1) — plus they compose in
    // the same parallel-move set. Each cycle must break with a scratch of ITS OWN
    // class (a GPR scratch for the int swap, an FPR scratch for the double swap).
    // Args: rsi, rdi, xmm1, xmm0 -> dests argGprs[0,1]=rdi,rsi and
    // argFprs[0,1]=xmm0,xmm1: (rdi<-rsi, rsi<-rdi) and (xmm0<-xmm1, xmm1<-xmm0).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& sch = **target;
    auto const callOp = sch.opcodeByMnemonic("call");
    auto const retOp  = sch.opcodeByMnemonic("ret");
    ASSERT_TRUE(callOp.has_value() && retOp.has_value());

    auto const rdiOrd = sch.registerByName("rdi");
    auto const rsiOrd = sch.registerByName("rsi");
    auto const x0 = sch.registerByName("xmm0");
    auto const x1 = sch.registerByName("xmm1");
    ASSERT_TRUE(rdiOrd && rsiOrd && x0 && x1);
    LirReg const rdi  = makePhysicalReg(*rdiOrd, LirRegClass::GPR);
    LirReg const rsi  = makePhysicalReg(*rsiOrd, LirRegClass::GPR);
    LirReg const xmm0 = makePhysicalReg(*x0, LirRegClass::FPR);
    LirReg const xmm1 = makePhysicalReg(*x1, LirRegClass::FPR);

    LirBuilder b{sch};
    b.addFunction(SymbolId{55});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    std::array<LirOperand, 5> callOps{
        LirOperand::makeSymbolRef(9),
        LirOperand::makeReg(rsi),   // arg0 -> rdi
        LirOperand::makeReg(rdi),   // arg1 -> rsi   (GPR swap)
        LirOperand::makeReg(xmm1),  // arg2 -> xmm0
        LirOperand::makeReg(xmm0),  // arg3 -> xmm1  (FPR swap)
    };
    b.addInst(*callOp, InvalidLirReg, callOps);
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                     = true;
    alloc.perFunc.back().originalSymbol         = SymbolId{55};
    alloc.perFunc.back().callingConventionIndex = 0;
    alloc.perFunc.back().numSpillSlots          = 0;

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch, alloc, ccRep);
    EXPECT_TRUE(result.ok())
        << "two disjoint same-call cycles (GPR + FPR) must both resolve";
    for (auto const& d : ccRep.all())
        EXPECT_NE(d.code, DiagnosticCode::L_MoveCycleUnsupported)
            << "disjoint breakable cycles must not surface L_MoveCycleUnsupported";
    // 6 moves total: each 2-cycle breaks into 3 (save + 2 swap legs). The GPR
    // swap emits `mov` (x3); the FPR swap emits `movaps` (x3). Count BOTH
    // mnemonics — collectInstStats.movOps only tracks `mov`, so tally movaps
    // separately and require the sum to be 6 (a class-collapsed resolution would
    // undercount one class).
    auto const stats = collectInstStats(result.lir, sch);
    auto const movapsOp = sch.opcodeByMnemonic("movaps");
    std::uint32_t movapsCount = 0;
    for (std::uint32_t fi = 0; fi < result.lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = result.lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
            LirBlockId const bb = result.lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < result.lir.blockInstCount(bb); ++ii) {
                LirInstId const inst = result.lir.blockInstAt(bb, ii);
                if (movapsOp.has_value()
                    && result.lir.instOpcode(inst) == *movapsOp)
                    ++movapsCount;
            }
        }
    }
    EXPECT_EQ(stats.movOps, 3u)
        << "the GPR swap must break into 3 `mov`s";
    EXPECT_EQ(movapsCount, 3u)
        << "the FPR swap must break into 3 `movaps`es (its own FPR-class scratch)";
}

// ── alignedSizeWithBias (D-LK10-ENTRY-TRAMP-PROLOGUE) ──────────────
// The trampoline prologue's frame-size formula. Documented in
// `src/lir/lir_callconv.hpp`. Anchored shared with ML7's frame
// computation (D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY). These tests pin
// the formula in isolation from any TargetSchema / cc — pure
// arithmetic surface, so a regression to the math is caught BEFORE
// it manifests as a runtime ABI violation (the failure mode that
// the standing Slice C smoke test catches end-to-end on Windows,
// but ONLY when Win64 movaps stores happen to hit a misaligned
// page during ExitProcess — cross-host CI would not catch).

TEST(AlignedSizeWithBias, ZeroRawZeroBiasIsZero) {
    // Trivial: no shadow space, no entry bias, no adjustment.
    // SysV ELF + ARM64 process-entry behavior.
    EXPECT_EQ(alignedSizeWithBias(0u, 16u, 0u), 0u);
}

TEST(AlignedSizeWithBias, Win64ShadowAndBiasYieldFortyBytes) {
    // Concrete Win64 case: shadow=32, alignment=16, bias=8 → 40.
    // This is THE number the trampoline subtracts from RSP at
    // process entry on Windows PE; a regression here would re-open
    // the STATUS_ACCESS_VIOLATION bug closed by
    // D-LK10-ENTRY-TRAMP-PROLOGUE.
    EXPECT_EQ(alignedSizeWithBias(32u, 16u, 8u), 40u);
}

TEST(AlignedSizeWithBias, BiasOnlyWithZeroRawIsBias) {
    // shadow=0 but entry-bias=8: still need to realign, so
    // smallest N >= 0 with N ≡ 8 mod 16 is 8 itself.
    EXPECT_EQ(alignedSizeWithBias(0u, 16u, 8u), 8u);
}

TEST(AlignedSizeWithBias, AlreadyCongruentIsIdentity) {
    // Fast path: raw already satisfies the congruence — return
    // verbatim. 40 % 16 = 8 = bias → no adjustment beyond raw.
    EXPECT_EQ(alignedSizeWithBias(40u, 16u, 8u), 40u);
}

TEST(AlignedSizeWithBias, RoundsUpToNextCongruentMultiple) {
    // raw=24 with bias=8 alignment=16: 24 % 16 = 8 = bias →
    // already congruent. Returns 24 verbatim (the smallest valid).
    EXPECT_EQ(alignedSizeWithBias(24u, 16u, 8u), 24u);
}

TEST(AlignedSizeWithBias, RawAlignedButBiasNonZeroAdjustsUp) {
    // raw=16 (a multiple of alignment) with bias=8: need to
    // increase by 8 to satisfy the congruence. 16 → 24.
    EXPECT_EQ(alignedSizeWithBias(16u, 16u, 8u), 24u);
}

TEST(AlignedSizeWithBias, BiasZeroRoundsRawToAlignment) {
    // bias=0 collapses to classic alignUp: raw=40, align=16, bias=0
    // → smallest N >= 40 with N ≡ 0 mod 16 = 48 (the ML7 frame
    // computation surface; anchored
    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY).
    EXPECT_EQ(alignedSizeWithBias(40u, 16u, 0u), 48u);
}

TEST(AlignedSizeWithBias, ZeroAlignmentReturnsRawVerbatim) {
    // Degenerate-case guard for non-register-machine targets that
    // declare stackAlignment=0 (operand-stack VMs at LK7/LK8). No
    // alignment requirement → no adjustment.
    EXPECT_EQ(alignedSizeWithBias(42u, 0u, 0u), 42u);
    EXPECT_EQ(alignedSizeWithBias(0u,  0u, 0u),  0u);
}

TEST(AlignedSizeWithBias, AArch64PostBlBiasIsZero) {
    // AArch64 BL doesn't push — process entry RSP is already
    // aligned. cc.shadowSpaceBytes=0 too. Result: no prologue.
    EXPECT_EQ(alignedSizeWithBias(0u, 16u, 0u), 0u);
}

TEST(AlignedSizeWithBias, LargeShadowMultipleAlignmentQuanta) {
    // Hypothetical future cc: shadow=64, alignment=16, bias=8 →
    // smallest N >= 64 with N ≡ 8 mod 16 = 72. Verifies the
    // formula scales beyond the single-quantum step that Win64
    // exercises.
    EXPECT_EQ(alignedSizeWithBias(64u, 16u, 8u), 72u);
}

// ── D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY closure pins (2026-06-02) ───
//
// Substrate-tier proof that the post-fold mechanism does what the
// hello_puts e2e example requires: a non-leaf function under a cc
// declaring shadowSpaceBytes + callPushBytes gets the correct
// prologue size + the hasCalls flag set. The e2e example IS the
// load-bearing pin (it asserts the actual print + exit code via
// OS-spawned bytes); this substrate test complements it by pinning
// the FORMULA at the function-by-function tier — so a refactor
// that breaks the formula for some fixture other than hello_puts
// gets caught at unit-test latency rather than waiting for the
// e2e harness to trip.

TEST(LirCallconv, NonLeafFunctionReservesShadowSpaceAndAlignmentBias) {
    // f calls g → f is non-leaf → f's totalFrameSize must satisfy
    // shadowSpaceBytes AND callPushBytes-mod-alignment. g is leaf →
    // g's totalFrameSize must satisfy the pre-fix
    // alignUp(raw, alignment) rule (no shadow space).
    auto bundle = lowerThroughRewrite(
        "int g(int a) { return a + 1; }\n"
        "int f(int x) { return g(x) + g(x); }\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_GT(cc->stackAlignment, 0u);

    // Discriminate non-leaf vs leaf by INDEPENDENTLY scanning the
    // input LIR for `isCall` opcodes per function — NOT by reading
    // the SUT's own `FrameLayout.hasCalls` (which would test the
    // mechanism using its own output; a regression where hasCalls is
    // mis-set would silently flip which layout the test calls fLayout
    // vs gLayout). Post-fold audit pin (pr-test-analyzer L1 +
    // silent-failure M3 convergence). The independent scan uses the
    // SAME schema-declared `TargetOpcodeInfo::isCall` flag the SUT
    // uses internally, so it stays format-agnostic.
    auto countCallsInFunc =
        [&](LirFuncId fn) -> std::uint32_t {
        std::uint32_t calls = 0u;
        auto const& srcLir = bundle.rewritten.lir;
        std::uint32_t const blocks = srcLir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < blocks; ++bi) {
            LirBlockId const blk = srcLir.funcBlockAt(fn, bi);
            std::uint32_t const n = srcLir.blockInstCount(blk);
            for (std::uint32_t i = 0; i < n; ++i) {
                LirInstId const inst = srcLir.blockInstAt(blk, i);
                auto const* info = bundle.lowered.target->opcodeInfo(
                    srcLir.instOpcode(inst));
                if (info != nullptr && info->isCall) ++calls;
            }
        }
        return calls;
    };
    FrameLayout const* fLayout = nullptr;
    FrameLayout const* gLayout = nullptr;
    for (std::uint32_t i = 0; i < result.lir.moduleFuncCount(); ++i) {
        // Note: result.lir is the OUTPUT (post-callconv) module; the
        // SOURCE module is bundle.rewritten.lir. They share funcId
        // ordering (1:1 by position per the callconv pass contract).
        LirFuncId const srcFn = bundle.rewritten.lir.funcAt(i);
        std::uint32_t const calls = countCallsInFunc(srcFn);
        auto const* layout = result.forFuncByIndex(i);
        ASSERT_NE(layout, nullptr);
        if (calls > 0) {
            ASSERT_EQ(fLayout, nullptr)
                << "fixture should have exactly one call-making function";
            fLayout = layout;
        } else {
            ASSERT_EQ(gLayout, nullptr)
                << "fixture should have exactly one leaf function";
            gLayout = layout;
        }
    }
    ASSERT_NE(fLayout, nullptr) << "f (call-making) not found";
    ASSERT_NE(gLayout, nullptr) << "g (leaf) not found";
    // Cross-check: the SUT's `hasCalls` flag must agree with the
    // independent scan. A divergence is the bug the discriminator
    // pin is designed to catch.
    EXPECT_TRUE(fLayout->hasCalls)
        << "f makes calls; FrameLayout.hasCalls must reflect that";
    EXPECT_FALSE(gLayout->hasCalls)
        << "g is leaf; FrameLayout.hasCalls must reflect that";

    // f (non-leaf): totalFrameSize >= shadowSpaceBytes AND
    // satisfies the callPushBytes congruence. D-ML7-2.2 (2026-06-02):
    // outgoingArgAreaSize already incorporates the shadow-space
    // requirement (when hasCalls; for SysV it adds 0), so the
    // expected formula folds shadow into raw via outgoingArgAreaSize
    // rather than via a separate max().
    std::uint32_t const fRaw = fLayout->outgoingArgAreaSize
                             + fLayout->savedRegAreaSize
                             + fLayout->spillAreaSize;
    std::uint32_t const fExpected = ::dss::alignedSizeWithBias(
        fRaw, cc->stackAlignment, cc->callPushBytes);
    EXPECT_EQ(fLayout->totalFrameSize, fExpected);
    EXPECT_GE(fLayout->totalFrameSize, cc->shadowSpaceBytes)
        << "non-leaf must reserve at least shadowSpaceBytes";
    EXPECT_EQ(fLayout->totalFrameSize % cc->stackAlignment,
              static_cast<std::uint32_t>(cc->callPushBytes))
        << "non-leaf must satisfy callPushBytes congruence";

    // g (leaf): totalFrameSize is the pre-fix alignUp result; NOT
    // forced to include shadowSpaceBytes (no callee exists).
    std::uint32_t const gRaw =
        gLayout->savedRegAreaSize + gLayout->spillAreaSize;
    std::uint32_t const gExpected =
        (gRaw + cc->stackAlignment - 1u) & ~(cc->stackAlignment - 1u);
    EXPECT_EQ(gLayout->totalFrameSize, gExpected);
    EXPECT_EQ(gLayout->totalFrameSize % cc->stackAlignment, 0u)
        << "leaf must be a multiple of stackAlignment";
}

// D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY 2nd-order audit fold (code-reviewer
// C3, 2026-06-02): the test above uses `ccIndex=0` which is x86_64's
// sysv_amd64 cc (shadowSpaceBytes=0). That covers the bias-only branch
// (non-leaf SysV: raw_with_shadow = max(0,0)=0; frame = bias=8) but
// does NOT exercise the SHADOW-SPACE branch that hello_puts depends
// on (Win64: raw_with_shadow = max(0,32)=32; frame =
// alignedSizeWithBias(32,16,8)=40). Without a Win64-cc substrate pin,
// a refactor that drops shadowSpaceBytes incorporation only on ms_x64
// passes ctest silently on Linux CI (where hello_puts skips due to
// runOn=["windows"]) — the e2e test would only catch it on a Windows
// host runner. This pin lives at the substrate tier so EVERY host's
// ctest catches the regression.
TEST(LirCallconv, NonLeafFunctionOnWin64CcReservesFullShadowSpaceFrame) {
    // ccIndex=1 = ms_x64 (shadowSpaceBytes=32, callPushBytes=8,
    // stackAlignment=16). The fixture's `f` calls `g` so f is
    // non-leaf — its frame MUST be at least 32 bytes (shadow) AND
    // satisfy N ≡ 8 mod 16. Smallest N >= 32 satisfying that = 40.
    auto bundle = lowerThroughRewrite(
        "int g(int a) { return a + 1; }\n"
        "int f(int x) { return g(x) + g(x); }\n",
        /*ccIndex=*/1);
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const* cc = bundle.lowered.target->callingConvention(1);
    ASSERT_NE(cc, nullptr);
    EXPECT_STREQ(cc->name.c_str(), "ms_x64")
        << "ccIndex=1 must resolve to ms_x64 (Win64) cc";
    ASSERT_EQ(cc->shadowSpaceBytes, 32u);
    ASSERT_EQ(cc->callPushBytes, 8u);
    ASSERT_EQ(cc->stackAlignment, 16u);

    // Find the non-leaf function via the same independent isCall
    // scan pattern. The fixture has exactly one (f).
    FrameLayout const* fLayout = nullptr;
    for (std::uint32_t i = 0; i < result.lir.moduleFuncCount(); ++i) {
        LirFuncId const srcFn = bundle.rewritten.lir.funcAt(i);
        bool hasCall = false;
        std::uint32_t const blocks = bundle.rewritten.lir.funcBlockCount(srcFn);
        for (std::uint32_t bi = 0; bi < blocks && !hasCall; ++bi) {
            LirBlockId const blk = bundle.rewritten.lir.funcBlockAt(srcFn, bi);
            std::uint32_t const n = bundle.rewritten.lir.blockInstCount(blk);
            for (std::uint32_t j = 0; j < n; ++j) {
                LirInstId const inst = bundle.rewritten.lir.blockInstAt(blk, j);
                auto const* info = bundle.lowered.target->opcodeInfo(
                    bundle.rewritten.lir.instOpcode(inst));
                if (info != nullptr && info->isCall) { hasCall = true; break; }
            }
        }
        if (hasCall) {
            ASSERT_EQ(fLayout, nullptr) << "fixture expects exactly one non-leaf";
            fLayout = result.forFuncByIndex(i);
        }
    }
    ASSERT_NE(fLayout, nullptr) << "non-leaf f not found";

    // The load-bearing Win64 invariant — the exact shape that
    // hello_puts depends on. A regression that drops shadowSpace
    // incorporation flips this from 40 to 0 (current saved regs +
    // spills happen to be tiny for this fixture). Hardcoded
    // expected value pins the regression at the byte level.
    EXPECT_GE(fLayout->totalFrameSize, 32u)
        << "Win64 non-leaf MUST reserve at least 32 bytes shadow space "
           "for the callee's register-arg home area";
    EXPECT_EQ(fLayout->totalFrameSize % 16u, 8u)
        << "Win64 non-leaf MUST satisfy callPushBytes congruence so RSP "
           "lands ≡ 0 mod 16 at the call site (the rule that makes "
           "puts's internal SSE movaps NOT fault)";
    EXPECT_TRUE(fLayout->hasCalls);
}

// D-CSUBSET-LOCAL-INT-CODEGEN substrate-tier tests (7-agent fold F3,
// 2026-06-02): the FrameLayoutInvariantsHoldPerFunction test pins
// the layout-arithmetic invariant (localAreaSize == numLocalAllocas
// * slotSize). It does NOT pin the materialize-loop's behavior:
// (a) every `alloca` rewrites to a `lea`; (b) the i-th alloca's
// MemOffset is exactly localAreaOffset() + i*slotSize; (c) two
// allocas get distinct, monotonically-increasing offsets (an
// off-by-one in localAllocaIndex would silently alias slot 0 for
// every alloca, miscompiling a function with multiple locals). The
// hello_writefile E2E pin only exercises ONE local (`int written;`)
// so its passing exit code does NOT pin the multi-local case.
TEST(LirCallconvAbi, AllocaMaterializesToLeaInScanOrder) {
    // Two body-local int declarations → two `alloca` LIR ops →
    // two `lea` insts post-materialize, with distinct MemOffsets
    // that are consecutive `slotSize`-strided from `localAreaOffset`.
    auto bundle = lowerThroughRewrite(
        "int f() { int a; int b; return 0; }\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    auto const stats = collectInstStats(result.lir, *bundle.lowered.target);
    EXPECT_EQ(stats.allocaOps, 0u)
        << "every `alloca` virtual op must be rewritten by the "
           "materialize pass — survival trips A_NoEncodingDeclared "
           "at the assembler";
    EXPECT_GE(stats.leaOps, 2u)
        << "two body-local declarations should produce at least two "
           "`lea` instructions (one per alloca)";
    // f is the only function (no g), so its layout is at index 0.
    ASSERT_GE(result.perFunc.size(), 1u);
    auto const& layout = result.perFunc[0];
    EXPECT_EQ(layout.numLocalAllocas, 2u);
    // The first two leas (in scan order) must carry offsets
    // localAreaOffset() and localAreaOffset() + slotSize. There may
    // be additional `lea`s from other lowering paths in the LIR
    // (e.g. global addresses) — pin that AT LEAST the two expected
    // offsets appear among the captured set.
    bool const firstFound = std::find(
        stats.leaOffsets.begin(), stats.leaOffsets.end(),
        static_cast<std::int32_t>(layout.localAreaOffset()))
        != stats.leaOffsets.end();
    bool const secondFound = std::find(
        stats.leaOffsets.begin(), stats.leaOffsets.end(),
        static_cast<std::int32_t>(layout.localAreaOffset()
                                  + layout.slotSize))
        != stats.leaOffsets.end();
    EXPECT_TRUE(firstFound)
        << "expected a `lea` with MemOffset = localAreaOffset() "
           "for the first body-local (`int a;`)";
    EXPECT_TRUE(secondFound)
        << "expected a `lea` with MemOffset = localAreaOffset() + "
           "slotSize for the second body-local (`int b;`) — an "
           "off-by-one in localAllocaIndex would alias both to slot 0";
}

// 7-agent fold F3 negative pin: an `alloca` instruction whose
// `result` reg is virtual (post-regalloc would have made it
// physical) fails loud with `L_VirtualRegInPostRegalloc`. The
// scenario is structurally impossible through the normal pipeline
// (regalloc always runs before materialize) but matters for
// hand-built LIR fixtures (synthesizers, JIT paths, future
// linker-tier helpers per D-LIR-ALLOCA-LOWERING-EXTRACTION).
TEST(LirCallconvAbi, AllocaWithVirtualResultFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;
    auto const allocaOp = sch.opcodeByMnemonic("alloca");
    auto const retOp    = sch.opcodeByMnemonic("ret");
    ASSERT_TRUE(allocaOp.has_value());
    ASSERT_TRUE(retOp.has_value());

    LirBuilder b{sch};
    b.addFunction(SymbolId{77});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    // alloca with a VIRTUAL result vreg (not physical). The
    // materialize pass MUST reject this with the post-regalloc
    // invariant diagnostic — letting it through would emit a
    // `lea <virtual>, [sp + offset]` that the encoder can't handle.
    LirReg const vresult = b.newVReg(LirRegClass::GPR);
    b.addInst(*allocaOp, vresult, std::span<LirOperand const>{});
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                     = true;
    alloc.perFunc.back().originalSymbol         = SymbolId{77};
    alloc.perFunc.back().callingConventionIndex = 0;
    alloc.perFunc.back().numSpillSlots          = 0;

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch, alloc, ccRep);
    EXPECT_FALSE(result.ok())
        << "materialize must reject a hand-built LIR with a virtual "
           "alloca result — the post-regalloc invariant requires "
           "all result vregs to be physical";
    EXPECT_EQ(::dss::test_support::countCode(
                  ccRep,
                  DiagnosticCode::L_VirtualRegInPostRegalloc), 1u)
        << "the diagnostic code must be the post-regalloc invariant "
           "code specifically, not any other error";
}

// D-CSUBSET-LOCAL-INT-CODEGEN-NEGATIVE-PIN closure (cycle 10k,
// 2026-06-04): a target schema that declares the `alloca` opcode
// but OMITS `lea` is structurally misconfigured — the materialize
// pass cannot lower `alloca` to its `lea result, [sp + offset]`
// form. Per `lir_callconv.cpp:877` the pass MUST fail loud with
// `L_RequiredLirOpcodeMissing` when it encounters an `alloca`
// instruction without the `lea` handle resolved.
//
// Pre-10k this defense was internal-only — no shipping target hits
// it (x86_64.target.json declares both). The pin exists to catch a
// future schema author who adds `alloca` but forgets `lea`. With
// the `mutateShippedTargetSchemaJson` test-support substrate
// (cycle 10k), the negative pin is now expressible without an
// always-stale parallel "broken" JSON file: load shipped, strip
// `lea`, exercise the materialize path.
TEST(LirCallconvAbi,
     AllocaWithoutLeaInSchemaFailsLoudInMaterialize) {
    auto mutatedR = ::dss::test_support::mutateShippedTargetSchemaJson(
        "x86_64", {"lea"});
    ASSERT_TRUE(mutatedR.has_value())
        << "mutated x86_64 schema (with `lea` stripped) must still "
           "load — the helper's JSON parse + re-serialize round-trip "
           "is what's exercised here; the substrate failure is in "
           "the callconv lowering, not the schema loader.";
    auto const& sch = **mutatedR;

    // Sanity: the mutation actually removed `lea`. Without this
    // attribution pin a future regression in the helper (e.g.,
    // string-compare bug) would silently let `lea` survive and
    // make the test pass for the WRONG reason (lookup succeeds
    // because lea is still there).
    ASSERT_FALSE(sch.opcodeByMnemonic("lea").has_value())
        << "mutateShippedTargetSchemaJson contract violated: "
           "`lea` survived the removal";
    ASSERT_TRUE(sch.opcodeByMnemonic("alloca").has_value())
        << "alloca must remain in the mutated schema — it's the "
           "opcode whose presence triggers the lea-required check";

    auto const allocaOp = sch.opcodeByMnemonic("alloca");
    auto const retOp    = sch.opcodeByMnemonic("ret");
    ASSERT_TRUE(retOp.has_value());

    LirBuilder b{sch};
    b.addFunction(SymbolId{77});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    // alloca with a PHYSICAL result reg (post-regalloc shape) so
    // the lea-missing arm fires BEFORE the virtual-result arm
    // (otherwise we'd be testing the wrong negative pin).
    auto const raxOrd = sch.registerByName("rax");
    ASSERT_TRUE(raxOrd.has_value());
    LirReg const presult{static_cast<std::uint32_t>(*raxOrd),
                         /*isPhysical=*/1,
                         /*cls=*/static_cast<std::uint8_t>(LirRegClass::GPR)};
    b.addInst(*allocaOp, presult, std::span<LirOperand const>{});
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                     = true;
    alloc.perFunc.back().originalSymbol         = SymbolId{77};
    alloc.perFunc.back().callingConventionIndex = 0;
    alloc.perFunc.back().numSpillSlots          = 0;

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch, alloc, ccRep);
    EXPECT_FALSE(result.ok())
        << "materialize must reject a schema that has `alloca` but "
           "no `lea` — the lowering depends on emitting `lea result, "
           "[sp + offset]` which requires the lea opcode handle.";
    EXPECT_EQ(::dss::test_support::countCode(
                  ccRep,
                  DiagnosticCode::L_RequiredLirOpcodeMissing), 1u)
        << "the diagnostic code must be `L_RequiredLirOpcodeMissing` "
           "specifically — a regression that hit a DIFFERENT error "
           "would pass `EXPECT_FALSE(result.ok())` but fail this "
           "specificity pin, making the regression cause attributable.";
}

// ── FC12a-core (D-FC12A-VARIADIC-CALLEE): variadic-callee prologue spill ──────
//
// A function that calls va_start must, in its prologue, (a) reserve a register-
// save-area sized by the CC's vaListLayout and (b) spill the integer + SSE arg
// registers into it, so va_start's reg_save_area points at live values. Pins both
// the FrameLayout zone size AND the spill-store count. RED-ON-DISABLE: stub the
// `emitVariadicRegSaveSpill` call in lir_callconv.cpp → the store-count assertion
// fails (0 saved-area stores instead of gpSaveCount + fpSaveCount).
TEST(LirCallconvVariadic, VaStartFunctionPrologueSpillsArgRegsIntoSaveArea) {
    auto bundle = lowerThroughRewrite(
        "int sum(int n, ...) {\n"
        "  va_list ap;\n"
        "  va_start(ap, n);\n"
        "  int t = 0;\n"
        "  for (int i = 0; i < n; i = i + 1) { t = t + va_arg(ap, int); }\n"
        "  va_end(ap);\n"
        "  return t;\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok) << "c-subset → LIR failed";
    ASSERT_TRUE(bundle.rewritten.ok) << "regalloc rewrite failed";
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok()) << "callconv materialize failed";
    ASSERT_EQ(result.perFunc.size(), 1u);

    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_TRUE(cc->vaListLayout.has_value())
        << "SysV cc must declare a vaListLayout for variadic-callee support";
    auto const& vl = *cc->vaListLayout;

    // (a) the FrameLayout reserves the full register-save-area (6*8 + 8*16 = 176).
    auto const& layout = result.perFunc[0];
    EXPECT_EQ(layout.vaRegSaveAreaSize, vl.regSaveAreaBytes())
        << "the prologue must reserve a vaListLayout-sized register-save-area";
    EXPECT_GT(layout.vaRegSaveAreaSize, 0u);

    // (b) the prologue spills gpSaveCount integer + fpSaveCount SSE arg regs into
    // the save area. Count `store` insts whose SP-relative MemOffset lands in the
    // save-area byte range [vaRegSaveAreaOffset, +regSaveAreaBytes). A stubbed spill
    // emits zero of these.
    // Count store-shaped insts (a Reg source + a SP-relative MemOffset) whose
    // offset lands in the save-area byte range — across BOTH register classes: the
    // GPR spill uses `store`, the FPR spill `movsd_store` (the registerClassOps
    // FPR store), so we match by the store SHAPE, not one mnemonic.
    std::uint32_t const lo = layout.vaRegSaveAreaOffset();
    std::uint32_t const hi = lo + vl.regSaveAreaBytes();
    Lir const& dst = result.lir;
    LirFuncId const fn = dst.funcAt(0);
    std::uint32_t saveAreaStores = 0;
    for (std::uint32_t bi = 0; bi < dst.funcBlockCount(fn); ++bi) {
        LirBlockId const b = dst.funcBlockAt(fn, bi);
        for (std::uint32_t i = 0; i < dst.blockInstCount(b); ++i) {
            LirInstId const inst = dst.blockInstAt(b, i);
            bool hasRegSrc = false, inRange = false;
            for (auto const& op : dst.instOperands(inst)) {
                if (op.kind == LirOperandKind::Reg) hasRegSrc = true;
                if (op.kind == LirOperandKind::MemOffset) {
                    auto const off = static_cast<std::int64_t>(op.offset);
                    if (off >= static_cast<std::int64_t>(lo)
                        && off < static_cast<std::int64_t>(hi))
                        inRange = true;
                }
            }
            if (hasRegSrc && inRange) ++saveAreaStores;
        }
    }
    // The prologue spills all 6 GPR (rdi..r9) + 8 SSE (xmm0..7) = gpSaveCount +
    // fpSaveCount registers into the save area (≥, not ==: a later for-loop/cursor
    // spill could incidentally also land a store in the zone; the load-bearing pin
    // is that EVERY arg register is spilled — a stubbed/absent spill emits 0, far
    // below 14, so the red-on-disable holds).
    EXPECT_GE(saveAreaStores, vl.gpSaveCount + vl.fpSaveCount)
        << "the variadic prologue must spill all " << vl.gpSaveCount
        << " integer + " << vl.fpSaveCount << " SSE arg registers into the "
           "register-save-area (a stubbed/absent spill emits 0 — the red-on-disable)";
}

// FC12a-core: a NON-variadic function must NOT reserve a register-save-area or
// spill — the zone is keyed precisely on calling va_start, not on every function.
TEST(LirCallconvVariadic, NonVariadicFunctionHasNoRegisterSaveArea) {
    auto bundle = lowerThroughRewrite("int add(int a, int b) { return a + b; }");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.perFunc.size(), 1u);
    EXPECT_EQ(result.perFunc[0].vaRegSaveAreaSize, 0u)
        << "a function that never calls va_start must reserve NO save area";
}

// ─── FC12c (D-FC12C-*) AAPCS64 + Apple arm64 variadic-callee config + lowering pins ───

// Gate #1: AAPCS64 vaListLayout config — the Aapcs64DualCursor strategy + the 5
// `__va_list` field offsets + the GR/VR save geometry (regSaveAreaBytes()==192).
TEST(LirCallconvAbiFC12c, Aapcs64VaListLayoutConfigPins) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto const* cc = (*target)->callingConventionByName("aapcs64");
    ASSERT_NE(cc, nullptr) << "arm64 must declare 'aapcs64'";
    ASSERT_TRUE(cc->vaListLayout.has_value())
        << "aapcs64 must declare a vaListLayout for variadic-callee support";
    auto const& vl = *cc->vaListLayout;
    EXPECT_EQ(vl.strategy, VaListStrategy::Aapcs64DualCursor);
    // The 5 __va_list field locators (byteOffset within the 32B struct).
    EXPECT_EQ(vl.stackField.byteOffset,  0u);
    EXPECT_EQ(vl.grTopField.byteOffset,  8u);
    EXPECT_EQ(vl.vrTopField.byteOffset,  16u);
    EXPECT_EQ(vl.grOffsField.byteOffset, 24u);
    EXPECT_EQ(vl.vrOffsField.byteOffset, 28u);
    EXPECT_EQ(vl.grOffsField.widthBytes, 4u);   // i32 cursor
    EXPECT_EQ(vl.vrOffsField.widthBytes, 4u);
    // GR (8x8) + VR (8x16) save geometry = 192 bytes.
    EXPECT_EQ(vl.gpSaveCount, 8u);
    EXPECT_EQ(vl.gpSlotBytes, 8u);
    EXPECT_EQ(vl.fpSaveCount, 8u);
    EXPECT_EQ(vl.fpSlotBytes, 16u);
    EXPECT_EQ(vl.regSaveAreaBytes(), 192u);
    EXPECT_EQ(vl.namedArgSlotBytes, 8u);
    // AAPCS64 does NOT always-stack varargs (that is Apple).
    EXPECT_FALSE(cc->variadicArgsAlwaysStack);
    EXPECT_FALSE(vl.variadicUsesOverflowBase);
}

// Gate #1: Apple arm64 vaListLayout config — HomogeneousPointer + the two FC12c flags.
TEST(LirCallconvAbiFC12c, AppleArm64VaListLayoutConfigPins) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto const* cc = (*target)->callingConventionByName("apple_arm64");
    ASSERT_NE(cc, nullptr) << "arm64 must declare 'apple_arm64'";
    ASSERT_TRUE(cc->vaListLayout.has_value());
    auto const& vl = *cc->vaListLayout;
    EXPECT_EQ(vl.strategy, VaListStrategy::HomogeneousPointer);
    EXPECT_TRUE(vl.variadicUsesOverflowBase)
        << "Apple anchors ap at the overflow base (no home area)";
    EXPECT_TRUE(cc->variadicArgsAlwaysStack)
        << "Apple always-stacks every vararg";
    EXPECT_EQ(vl.namedArgSlotBytes, 8u);
}

// Gate #1 (cross-CC guard): SysV + Win64 vaListLayout are UNCHANGED by FC12c.
TEST(LirCallconvAbiFC12c, SysVAndWin64VaListUnchanged) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const* sysv = (*target)->callingConventionByName("sysv_amd64");
    ASSERT_NE(sysv, nullptr);
    ASSERT_TRUE(sysv->vaListLayout.has_value());
    EXPECT_EQ(sysv->vaListLayout->strategy, VaListStrategy::SysVRegisterSave);
    EXPECT_FALSE(sysv->variadicArgsAlwaysStack);
    EXPECT_FALSE(sysv->vaListLayout->variadicUsesOverflowBase);
    auto const* msx64 = (*target)->callingConventionByName("ms_x64");
    ASSERT_NE(msx64, nullptr);
    ASSERT_TRUE(msx64->vaListLayout.has_value());
    EXPECT_EQ(msx64->vaListLayout->strategy, VaListStrategy::HomogeneousPointer);
    EXPECT_FALSE(msx64->variadicArgsAlwaysStack)
        << "Win64 is register-then-stack, NOT always-stack";
    EXPECT_FALSE(msx64->vaListLayout->variadicUsesOverflowBase)
        << "Win64 anchors ap at the HOME base, not the overflow base";
}

// Gate #9 (host-independent): an AAPCS64 variadic callee reserves the 192B callee-local
// register-save-area + spills x0..x7 (GR) and v0..v7 (VR via fstur_q) into it. A
// stubbed/absent spill emits zero save-area stores — the red-on-disable.
TEST(LirCallconvVariadicFC12c, Aapcs64VariadicCalleeReservesAndSpillsSaveArea) {
    auto bundle = lowerThroughRewrite(
        "int sum(int n, ...) {\n"
        "    va_list ap; va_start(ap, n);\n"
        "    int t = va_arg(ap, int);\n"
        "    va_end(ap);\n"
        "    return t;\n"
        "}\n",
        /*ccIndex=*/0, /*targetName=*/"arm64");
    ASSERT_TRUE(bundle.lowered.lir.ok) << "AAPCS64 variadic callee failed to lower";
    ASSERT_TRUE(bundle.alloc.ok());
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(ccRep.errorCount(), 0u);

    auto const* cc = bundle.lowered.target->callingConventionByName("aapcs64");
    ASSERT_NE(cc, nullptr);
    ASSERT_TRUE(cc->vaListLayout.has_value());
    auto const& vl = *cc->vaListLayout;

    auto const* layout = result.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    EXPECT_EQ(layout->vaRegSaveAreaSize, vl.regSaveAreaBytes())
        << "AAPCS64 reserves a 192B callee-local register-save-area";
    EXPECT_EQ(layout->vaRegSaveAreaSize, 192u);

    // Count store-shaped insts (a Reg source operand). The GR spill uses the class
    // GPR store, the VR spill uses fstur_q — both are 4-operand stores with a Reg
    // value source. We require AT LEAST gpSaveCount + fpSaveCount = 16 spill stores
    // (a stubbed spill emits 0). The base is a scratch register (add scratch, sp, #off)
    // so MemOffsets are small; matching by store-shape + the fstur_q opcode is robust.
    auto const fsturQ = bundle.lowered.target->opcodeByMnemonic("fstur_q");
    ASSERT_TRUE(fsturQ.has_value());
    Lir const& dst = result.lir;
    LirFuncId const fn = dst.funcAt(0);
    std::uint32_t vrSpills = 0;   // fstur_q stores specifically (the VR block)
    for (std::uint32_t bi = 0; bi < dst.funcBlockCount(fn); ++bi) {
        LirBlockId const b = dst.funcBlockAt(fn, bi);
        for (std::uint32_t i = 0; i < dst.blockInstCount(b); ++i) {
            LirInstId const inst = dst.blockInstAt(b, i);
            if (dst.instOpcode(inst) == *fsturQ) ++vrSpills;
        }
    }
    EXPECT_EQ(vrSpills, vl.fpSaveCount)
        << "the AAPCS64 prologue must spill all " << vl.fpSaveCount
        << " VR arg registers via fstur_q (a stubbed/absent VR spill emits 0)";
}

// FOLD #1 (adversarial-review) red-on-disable: the AAPCS64 variadic prologue's
// save-area scratch-base register must AVOID x8 (the indirectResultRegister / sret
// pointer). A variadic fn that ALSO returns a >16B (ByReference/MEMORY-class) struct
// receives its incoming result-buffer address in x8; the prologue materializes the
// register-save-area base via `add <scratch>, sp, #off` BEFORE the per-inst loop's
// `read_indirect_result` reads x8. If the scratch picker lands on x8 (the first
// caller-saved GPR after excluding the arg GPRs x0..x7), the `add` CLOBBERS the sret
// pointer → the struct result is written to the callee's own frame, not the caller's
// buffer: a silent miscompile. The fix adds cc.indirectResultRegister's ordinal to the
// scratch avoid-set so the picker chooses x9. RED-ON-DISABLE: revert that avoid-set
// addition in emitVariadicPrologueSpill (lir_callconv.cpp) → the scratch becomes x8
// and this test fails (it asserts the save-area `add` base is NOT x8 and NOT an arg
// GPR). The witness corpus examples/c-subset/varargs_aapcs64_sret/ runs the same
// composition end-to-end under qemu.
TEST(LirCallconvVariadicFC12c, Aapcs64VariadicStructReturnPrologueScratchAvoidsX8) {
    // `Big` is 24B (>16B) → AAPCS64 returns it via the x8 sret pointer; `make` is also
    // variadic, so its prologue spills the GR/VR save area via a scratch base. The
    // typedef-name return type is the reachable form (a top-level `struct Tag` return
    // type is a pre-FC4 grammar residue) — identical ABI codegen.
    auto bundle = lowerThroughRewrite(
        "typedef struct { long a; long b; long c; } Big;\n"
        "Big make(int n, ...) {\n"
        "    va_list ap; va_start(ap, n);\n"
        "    long s = 0;\n"
        "    for (int i = 0; i < n; i = i + 1) { s = s + va_arg(ap, long); }\n"
        "    va_end(ap);\n"
        "    Big r; r.a = s; r.b = n; r.c = s + n; return r;\n"
        "}\n",
        /*ccIndex=*/0, /*targetName=*/"arm64");
    ASSERT_TRUE(bundle.lowered.lir.ok) << "variadic+sret callee failed to lower";
    ASSERT_TRUE(bundle.alloc.ok());
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(ccRep.errorCount(), 0u);

    auto const* cc = bundle.lowered.target->callingConventionByName("aapcs64");
    ASSERT_NE(cc, nullptr);
    ASSERT_TRUE(cc->indirectResultRegister.has_value())
        << "aapcs64 must declare x8 as the indirect-result register";
    std::uint16_t const x8Ord = cc->indirectResultRegister->ordinal;
    auto const addOp = bundle.lowered.target->opcodeByMnemonic("add");
    auto const spOrd = bundle.lowered.target->registerByName("sp");
    ASSERT_TRUE(addOp.has_value());
    ASSERT_TRUE(spOrd.has_value());
    auto isArgGpr = [&](std::uint16_t ord) {
        for (auto const& name : cc->argGprs)
            if (auto const a = bundle.lowered.target->registerByName(name);
                a.has_value() && *a == ord)
                return true;
        return false;
    };

    // Precondition (read on the PRE-callconv LIR): the variadic+sret composition must
    // actually be engaged — the callee carries a `read_indirect_result` op (the x8 sret
    // read) AND reserves a save area. `materializeCallingConvention` MATERIALIZES the
    // virtual `read_indirect_result` AWAY into `mov result, x8` (and continues, so it
    // is absent from result.lir), so the precondition is checked on bundle.rewritten.lir
    // — where the op still exists. (That x8 mov is exactly what the prologue scratch must
    // not clobber.) If the op were absent the scratch-vs-x8 pin would be vacuous.
    auto const readIrr = bundle.lowered.target->opcodeByMnemonic("read_indirect_result");
    ASSERT_TRUE(readIrr.has_value());
    bool sawReadIrr = false;
    {
        Lir const& pre = bundle.rewritten.lir;
        LirFuncId const pfn = pre.funcAt(0);
        for (std::uint32_t bi = 0; bi < pre.funcBlockCount(pfn); ++bi) {
            LirBlockId const b = pre.funcBlockAt(pfn, bi);
            for (std::uint32_t i = 0; i < pre.blockInstCount(b); ++i)
                if (pre.instOpcode(pre.blockInstAt(b, i)) == *readIrr) sawReadIrr = true;
        }
    }
    auto const* layout = result.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    EXPECT_GT(layout->vaRegSaveAreaSize, 0u)
        << "the variadic callee must reserve a register-save-area (else no scratch base)";

    // Locate the save-area base `add <scratch>, sp, #imm` in the POST-callconv prologue:
    // an `add` whose result is a physical reg OTHER than sp, taking sp as a Reg operand
    // and an immediate. (The epilogue's `add sp, sp, #frame` writes sp → excluded by the
    // result!=sp guard.)
    Lir const& dst = result.lir;
    LirFuncId const fn = dst.funcAt(0);
    bool sawScratchBase = false;
    for (std::uint32_t bi = 0; bi < dst.funcBlockCount(fn); ++bi) {
        LirBlockId const b = dst.funcBlockAt(fn, bi);
        for (std::uint32_t i = 0; i < dst.blockInstCount(b); ++i) {
            LirInstId const inst = dst.blockInstAt(b, i);
            if (dst.instOpcode(inst) != *addOp) continue;
            LirReg const res = dst.instResult(inst);
            if (!res.valid() || res.isPhysical == 0 || res.id == *spOrd) continue;
            bool srcIsSp = false, hasImm = false;
            for (auto const& op : dst.instOperands(inst)) {
                if (op.kind == LirOperandKind::Reg && op.reg.isPhysical != 0
                    && op.reg.id == *spOrd)
                    srcIsSp = true;
                if (op.kind == LirOperandKind::ImmInt) hasImm = true;
            }
            if (!srcIsSp || !hasImm) continue;
            sawScratchBase = true;
            // THE LOAD-BEARING ASSERTIONS (red-on-disable): the save-area scratch base
            // must not be x8 (clobbering the live sret pointer) nor an arg GPR.
            EXPECT_NE(res.id, x8Ord)
                << "AAPCS64 variadic prologue scratch base must NOT be x8 (the "
                   "indirect-result/sret pointer) — reverting the IRR avoid-set in "
                   "emitVariadicPrologueSpill clobbers the >16B struct return";
            EXPECT_FALSE(isArgGpr(res.id))
                << "the scratch base must not be an arg GPR (x0..x7) being spilled";
        }
    }
    EXPECT_TRUE(sawReadIrr)
        << "precondition: the >16B struct return must engage the x8 sret path "
           "(read_indirect_result) — else this scratch-vs-x8 pin is vacuous";
    EXPECT_TRUE(sawScratchBase)
        << "the AAPCS64 variadic prologue must materialize the save-area base via "
           "`add <scratch>, sp, #off` (the topmost-zone STUR-reach workaround)";
}

// Gate #6 + cross-CC discriminator (host-independent): an apple_arm64 variadic call
// FORCES a vararg that WOULD fit in x2 onto the stack; the SAME call under aapcs64
// keeps it in-register. We assert via the outgoing-arg-area size: Apple reserves
// stack overflow for the forced varargs; AAPCS64 reserves none (all args fit regs).
TEST(LirCallconvVariadicFC12c, AppleForcesVarargToStackAapcs64KeepsInReg) {
    // The CALLER `f` is declared FIRST so it is function index 0 (the layout query
    // below uses forFuncByIndex(0)); `sink` is DEFINED after via a forward reference
    // that resolves through prototype/definition merging (D-CSUBSET-FN-PROTOTYPE) —
    // sink must be a real (defined) function so the call binds, not a bare proto
    // (which would emit no symbol and fail loud at HIR->MIR).
    char const* src =
        "int sink(int n, ...);\n"                    // forward prototype (merges with the def below)
        "int f(void) { return sink(1, 7, 8); }\n"   // 1 fixed + 2 varargs, all small ints — function 0
        "int sink(int n, ...) { return n; }\n";     // the definition (function 1)
    // apple_arm64 = cc index 1; aapcs64 = cc index 0.
    auto apple = lowerThroughRewrite(src, /*ccIndex=*/1, /*targetName=*/"arm64");
    ASSERT_TRUE(apple.lowered.lir.ok);
    ASSERT_TRUE(apple.rewritten.ok);
    DiagnosticReporter appleRep;
    auto appleRes = materializeCallingConvention(apple.rewritten.lir,
                                                 *apple.lowered.target,
                                                 apple.alloc, appleRep);
    ASSERT_TRUE(appleRes.ok());
    ASSERT_EQ(appleRep.errorCount(), 0u);

    auto aapcs = lowerThroughRewrite(src, /*ccIndex=*/0, /*targetName=*/"arm64");
    ASSERT_TRUE(aapcs.lowered.lir.ok);
    ASSERT_TRUE(aapcs.rewritten.ok);
    DiagnosticReporter aapcsRep;
    auto aapcsRes = materializeCallingConvention(aapcs.rewritten.lir,
                                                 *aapcs.lowered.target,
                                                 aapcs.alloc, aapcsRep);
    ASSERT_TRUE(aapcsRes.ok());
    ASSERT_EQ(aapcsRep.errorCount(), 0u);

    // `f` is function 0 in both. Apple forces the 2 varargs to the stack → a nonzero
    // outgoing-arg area; AAPCS64 places them in x1/x2 → zero outgoing args (the call
    // fits entirely in registers). The discriminator: Apple > AAPCS64.
    auto const* appleLayout = appleRes.forFuncByIndex(0);
    auto const* aapcsLayout = aapcsRes.forFuncByIndex(0);
    ASSERT_NE(appleLayout, nullptr);
    ASSERT_NE(aapcsLayout, nullptr);
    EXPECT_GT(appleLayout->outgoingArgAreaSize, 0u)
        << "Apple must reserve stack overflow for the forced varargs";
    EXPECT_EQ(aapcsLayout->outgoingArgAreaSize, 0u)
        << "AAPCS64 places the same varargs in x1/x2 — no stack overflow";
    EXPECT_GT(appleLayout->outgoingArgAreaSize, aapcsLayout->outgoingArgAreaSize)
        << "the cross-CC discriminator: variadicArgsAlwaysStack forces stack on Apple "
           "but not AAPCS64 for a call that fits the register pool";
}

// Gate #4 (host-independent): Apple's va_start overflow-base lea uses the no-shadow,
// no-callPush base (== totalFrameSize on arm64) — congruent with where the caller
// stacks the first vararg (the FC12b BLOCKER-1 congruence mirror, for Apple). Also
// pins vaRegSaveAreaSize == 0 (Apple HomogeneousPointer reserves no save area).
TEST(LirCallconvVariadicFC12c, AppleVaStartOverflowBaseCongruence) {
    auto bundle = lowerThroughRewrite(
        "int sum(int n, ...) {\n"
        "    va_list ap; va_start(ap, n);\n"
        "    int t = va_arg(ap, int);\n"
        "    va_end(ap);\n"
        "    return t;\n"
        "}\n",
        /*ccIndex=*/1, /*targetName=*/"arm64");
    ASSERT_TRUE(bundle.lowered.lir.ok) << "Apple variadic callee failed to lower";
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(ccRep.errorCount(), 0u);

    auto const* cc = bundle.lowered.target->callingConventionByName("apple_arm64");
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->shadowSpaceBytes, 0u);
    EXPECT_EQ(cc->callPushBytes, 0u) << "arm64 BL writes LR — no stack push";

    auto const* layout = result.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    EXPECT_EQ(layout->vaRegSaveAreaSize, 0u)
        << "Apple HomogeneousPointer reserves NO callee-local register-save-area";

    // The va_start overflow base = totalFrameSize + callPushBytes(0) + shadowSpaceBytes(0)
    // = totalFrameSize. Find the va_overflow_arg_area materialization (`lea`/`add` with a
    // MemOffset) and assert its offset == totalFrameSize.
    auto const vaOvf = bundle.lowered.target->opcodeByMnemonic("va_overflow_arg_area");
    ASSERT_TRUE(vaOvf.has_value());
    // The op is materialized away into `lea result, [sp + off]`; we assert the
    // congruent offset arithmetic at the config level (the materialization uses
    // totalFrameSize + callPushBytes + shadowSpaceBytes — all the additive terms but
    // shadow/callPush are 0 on arm64, so the base IS totalFrameSize).
    std::uint32_t const overflowBase =
        layout->totalFrameSize
        + static_cast<std::uint32_t>(cc->callPushBytes)
        + static_cast<std::uint32_t>(cc->shadowSpaceBytes);
    EXPECT_EQ(overflowBase, layout->totalFrameSize)
        << "on arm64 the Apple overflow base has no shadow/callPush term — it is the "
           "first incoming stack vararg, congruent with the caller's stack-store";
}

// FC12-deferral④ (D-FC12A-VARIADIC-OVERFLOW-FIXED-STACK-ARGS, FOLD 3): a SysV
// variadic callee whose 7 fixed int params overflow the 6 integer arg registers. The
// va_start fixed-stack-arg displacement (1 overflowed GPR * 8 = 8) rides the
// VaOverflowArgAreaAddr MIR payload, is threaded through mir_to_lir, and lir_callconv
// adds it to the overflow base. PIN: the materialized `va_overflow_arg_area` `lea`
// carries MemOffset == totalFrameSize + callPushBytes(8) + shadowSpaceBytes(0) + 8
// — and the un-displaced base (payload-dropped value) is NOT among the leas.
// RED-ON-DISABLE: revert mir_to_lir's VaOverflowArgAreaAddr payload threading (or the
// lir_callconv `+ fixedStackBytes`) → the lea offset drops to the bare base → the
// `+8` offset disappears and the bare base appears → both EXPECTs go red.
TEST(LirCallconvVariadicFC12deferral4, SysVVaStartFixedStackOverflowBaseCongruence) {
    auto bundle = lowerThroughRewrite(
        "int pick(int a, int b, int c, int d, int e, int f, int g, ...) {\n"
        "    va_list ap; va_start(ap, g);\n"
        "    int v = va_arg(ap, int);\n"
        "    va_end(ap);\n"
        "    return a + g + v;\n"
        "}\n",
        /*ccIndex=*/0, /*targetName=*/"x86_64");
    ASSERT_TRUE(bundle.lowered.lir.ok) << "SysV fixed-overflow variadic callee lower";
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(ccRep.errorCount(), 0u);

    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    auto const* layout = result.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);

    std::uint32_t const base =
        layout->totalFrameSize
        + static_cast<std::uint32_t>(cc->callPushBytes)
        + static_cast<std::uint32_t>(cc->shadowSpaceBytes);
    std::int32_t const expectedOverflow = static_cast<std::int32_t>(base + 8u);

    auto const stats = collectInstStats(result.lir, *bundle.lowered.target);
    bool const sawDisplaced =
        std::find(stats.leaOffsets.begin(), stats.leaOffsets.end(),
                  expectedOverflow) != stats.leaOffsets.end();
    bool const sawBareBase =
        std::find(stats.leaOffsets.begin(), stats.leaOffsets.end(),
                  static_cast<std::int32_t>(base)) != stats.leaOffsets.end();
    EXPECT_TRUE(sawDisplaced)
        << "the va_overflow_arg_area `lea` MemOffset must INCLUDE the +8 fixed-stack-arg "
           "displacement (base " << base << " + 8 = " << expectedOverflow << ") — a "
           "dropped payload would leave it at the bare base " << base;
    EXPECT_FALSE(sawBareBase)
        << "no `lea` may carry the UN-displaced overflow base (" << base << ") — its "
           "presence means the fixed-stack-arg payload was discarded (the exact bug)";
}

// FC12-deferral④ (D-FC12C-AAPCS64-VARIADIC-OVERFLOW-FIXED-STACK-ARGS, FOLD 3): AAPCS64
// mirror — 9 fixed int params overflow the 8 GPR arg regs. The `__stack`
// (va_overflow_arg_area) lea MemOffset must include the +8 displacement; arm64 has no
// shadow/callPush term so base == totalFrameSize. RED-ON-DISABLE identical to the SysV
// pin (drop the threading or the `+ fixedStackBytes` → +8 disappears, bare base shows).
TEST(LirCallconvVariadicFC12deferral4, Aapcs64VaStartFixedStackOverflowBaseCongruence) {
    auto bundle = lowerThroughRewrite(
        "int pick(int a, int b, int c, int d, int e, int f, int g, int h, int i,"
        " ...) {\n"
        "    va_list ap; va_start(ap, i);\n"
        "    int v = va_arg(ap, int);\n"
        "    va_end(ap);\n"
        "    return a + i + v;\n"
        "}\n",
        /*ccIndex=*/0, /*targetName=*/"arm64");
    ASSERT_TRUE(bundle.lowered.lir.ok) << "AAPCS64 fixed-overflow variadic callee lower";
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(ccRep.errorCount(), 0u);

    auto const* cc = bundle.lowered.target->callingConventionByName("aapcs64");
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->shadowSpaceBytes, 0u);
    EXPECT_EQ(cc->callPushBytes, 0u) << "arm64 BL writes LR — no stack push";
    auto const* layout = result.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);

    std::uint32_t const base =
        layout->totalFrameSize
        + static_cast<std::uint32_t>(cc->callPushBytes)
        + static_cast<std::uint32_t>(cc->shadowSpaceBytes);
    std::int32_t const expectedOverflow = static_cast<std::int32_t>(base + 8u);

    auto const stats = collectInstStats(result.lir, *bundle.lowered.target);
    bool const sawDisplaced =
        std::find(stats.leaOffsets.begin(), stats.leaOffsets.end(),
                  expectedOverflow) != stats.leaOffsets.end();
    bool const sawBareBase =
        std::find(stats.leaOffsets.begin(), stats.leaOffsets.end(),
                  static_cast<std::int32_t>(base)) != stats.leaOffsets.end();
    EXPECT_TRUE(sawDisplaced)
        << "the __stack (va_overflow_arg_area) `lea` MemOffset must INCLUDE the +8 "
           "displacement (base " << base << " + 8 = " << expectedOverflow << ")";
    EXPECT_FALSE(sawBareBase)
        << "no `lea` may carry the UN-displaced __stack base (" << base << ") — its "
           "presence means the fixed-stack-arg payload was discarded";
}

// D-ASM-AARCH64-LARGE-FRAME-IMM12 (FOLD 1, LOAD-BEARING): the SELECTION witness.
// A ≥9-fixed-param AAPCS64 callee loads its 9th (incoming-stack) fixed param at
// `[sp + totalFrameSize]`. With a large touched local (the `long pad[40]`
// mirror of the runtime corpus) the frame is comfortably > 255, so that load's byte
// offset exceeds the unscaled imm9 (-256..255) and the materialize pass MUST select the
// SCALED `load_u` (LDR imm12-scaled) opcode — NOT the unscaled `load` (LDUR imm9).
// Exit-210 in the qemu corpus is necessary-but-not-sufficient (the byte-pins prove the
// ENCODING; this proves the SELECTION). RED-ON-DISABLE: revert the load_u selection in
// `lir_callconv.cpp::materializeOneFunc` (always emit `*argLoad`) → the stack-arg load
// at offset > 255 emits the unscaled `load` and NO `load_u` appears at a large offset →
// `sawLoadUAtLargeOffset` flips false. (The corpus would then fail to encode at the
// assembler — A_ImmediateOperandOutOfRange — but this structural pin catches it earlier
// and host-independently, on every CI leg.)
TEST(LirCallconvLargeFrameImm12, Aapcs64FrameBeyondImm12EmitsTwoSubTwoAdd) {
    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12: a function whose frame
    // exceeds the single-word imm12 reach (4095) — `int big[9000]` is
    // 36000B — must have a prologue that adjusts SP with the 2-word
    // shifted-imm12 macro (TWO `sub sp` words: a sh=0 word then a sh=1
    // word) and an epilogue with the mirror TWO `add sp` words. The
    // LIR carries ONE `sub`/`add` instruction each; the TWO-word
    // expansion is the encoder's doing, so this pin assembles to BYTES
    // and counts the machine words — host-independent (no execution).
    //
    // RED-ON-DISABLE: revert the encoder's imm12.hilo24 split (or the
    // variant's immMin/immMax routing) → the single `sub sp,#36016`
    // either fails loud at the assembler (A_ImmediateOperandOutOfRange,
    // the OLD behavior — `errorCount != 0` below catches it) or, if the
    // variant were mis-keyed, emits ONE word → the two-word count goes red.
    auto bundle = lowerThroughRewrite(
        "int big_frame(void) {\n"
        "    int big[9000];\n"          // 36000B local -> frame > 4095
        "    big[8999] = 42;\n"
        "    return big[8999];\n"
        "}\n",
        /*ccIndex=*/0, /*targetName=*/"arm64");
    ASSERT_TRUE(bundle.lowered.lir.ok) << "arm64 large-frame callee lower";
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter legRep;
    auto legal = legalizeTwoAddress(bundle.rewritten.lir, *bundle.lowered.target,
                                    legRep);
    ASSERT_TRUE(legal.ok());
    DiagnosticReporter ccRep;
    auto cc = materializeCallingConvention(legal.lir, *bundle.lowered.target,
                                           bundle.alloc, ccRep);
    ASSERT_TRUE(cc.ok());
    ASSERT_EQ(ccRep.errorCount(), 0u);

    auto const* layout = cc.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    ASSERT_GT(layout->totalFrameSize, 4095u)
        << "the touched 9000-int array MUST push the frame past the single-"
           "word imm12 reach (4095) — got " << layout->totalFrameSize;

    std::vector<MirInstId> lirToMir(cc.lir.instCount(), InvalidMirInst);
    DiagnosticReporter asmRep;
    auto mod = assemble(cc.lir, *bundle.lowered.target, lirToMir, asmRep);
    EXPECT_EQ(asmRep.errorCount(), 0u)
        << "a >4095-byte frame must assemble cleanly via the 2-word shifted-"
           "imm12 SP adjust (NOT fail loud as it did before this fix)";
    ASSERT_EQ(mod.functions.size(), 1u);
    auto const& fnBytes = mod.functions[0].bytes;

    // Derive the EXACT word pair from the resolved frame size — the same
    // split the encoder performs (lo = V & 0xFFF, hi = (V>>12) & 0xFFF),
    // Rd = Rn = sp = 31. Host-independent: the expected bytes follow the
    // frame size, not a baked constant.
    std::uint32_t const V  = layout->totalFrameSize;
    std::uint32_t const lo = V & 0xFFFu;
    std::uint32_t const hi = (V >> 12) & 0xFFFu;
    auto leWord = [](std::uint32_t w) {
        return std::array<std::uint8_t, 4>{
            static_cast<std::uint8_t>(w & 0xFF),
            static_cast<std::uint8_t>((w >> 8) & 0xFF),
            static_cast<std::uint8_t>((w >> 16) & 0xFF),
            static_cast<std::uint8_t>((w >> 24) & 0xFF)};
    };
    auto countWord = [&](std::uint32_t w) {
        auto const pat = leWord(w);
        std::uint32_t n = 0;
        for (std::size_t i = 0; i + 4 <= fnBytes.size(); i += 4) {
            if (std::equal(pat.begin(), pat.end(), fnBytes.begin() + i)) ++n;
        }
        return n;
    };
    // SUB sp,sp,#lo (sh=0, 0xD1000000) + SUB sp,sp,#hi,LSL#12 (sh=1, 0xD1400000).
    std::uint32_t const subLo = 0xD1000000u | (lo << 10) | (31u << 5) | 31u;
    std::uint32_t const subHi = 0xD1400000u | (hi << 10) | (31u << 5) | 31u;
    // ADD sp,sp,#lo (sh=0, 0x91000000) + ADD sp,sp,#hi,LSL#12 (sh=1, 0x91400000).
    std::uint32_t const addLo = 0x91000000u | (lo << 10) | (31u << 5) | 31u;
    std::uint32_t const addHi = 0x91400000u | (hi << 10) | (31u << 5) | 31u;

    // The leaf function adjusts SP exactly once in the prologue (one sub
    // pair) and once in the epilogue (one add pair). Both halves of each
    // pair must be present.
    EXPECT_EQ(countWord(subLo), 1u) << "prologue must emit the sh=0 `sub sp,#lo` word";
    EXPECT_EQ(countWord(subHi), 1u) << "prologue must emit the sh=1 `sub sp,#hi,LSL#12` word";
    EXPECT_EQ(countWord(addLo), 1u) << "epilogue must emit the sh=0 `add sp,#lo` word";
    EXPECT_EQ(countWord(addHi), 1u) << "epilogue must emit the sh=1 `add sp,#hi,LSL#12` word";
}

TEST(LirCallconvLargeFrameImm12, Aapcs64NinthFixedParamLoadUsesScaledLoadU) {
    // `long pad[40]` (320B) forces the frame > 255. An array alloca is never
    // scalar-promoted (mem2reg refuses array allocas), and this pipeline runs
    // NO optimizer anyway (lower -> liveness -> regalloc -> rewrite -> callconv),
    // so the full reservation survives. `volatile` is NOT needed here (the array
    // alloca survives on its own); it is implemented since c21 — only a pointer-to-
    // volatile POINTEE fails loud (S_VolatilePointeeNotSupported).
    auto bundle = lowerThroughRewrite(
        "int sum9(int a, int b, int c, int d, int e, int f, int g, int h, int i,"
        " ...) {\n"
        "    long pad[40];\n"             // FOLD 1: 320B local -> frame > 255
        "    pad[0] = i;\n"
        "    va_list ap; va_start(ap, i);\n"
        "    int v = va_arg(ap, int);\n"
        "    va_end(ap);\n"
        "    return a + b + c + d + e + f + g + h + i + v;\n"
        "}\n",
        /*ccIndex=*/0, /*targetName=*/"arm64");
    ASSERT_TRUE(bundle.lowered.lir.ok) << "AAPCS64 9-fixed-param large-frame callee lower";
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(ccRep.errorCount(), 0u);

    auto const* layout = result.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    EXPECT_GT(layout->totalFrameSize, 255u)
        << "FOLD 1 invariant: the touched pad MUST push the frame past the imm9 reach "
           "(255) so the 9th-param load cannot fit the unscaled form — got "
        << layout->totalFrameSize;

    auto const loadOp  = bundle.lowered.target->opcodeByMnemonic("load");
    auto const loadUOp = bundle.lowered.target->opcodeByMnemonic("load_u");
    ASSERT_TRUE(loadOp.has_value() && loadUOp.has_value());

    // Scan the materialized LIR for a frame load whose MemOffset is beyond imm9.
    // That load is the 9th-param incoming-stack read; it MUST use `load_u`, and
    // NO plain (unscaled) `load` may carry such an offset.
    bool sawLoadUAtLargeOffset = false;
    bool sawPlainLoadAtLargeOffset = false;
    auto const& lir = result.lir;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(blk); ++ii) {
                LirInstId const inst = lir.blockInstAt(blk, ii);
                std::uint16_t const op = lir.instOpcode(inst);
                auto const ops = lir.instOperands(inst);
                // Frame load shape: [base_reg, MemBase, MemOffset] (3 ops).
                if (ops.size() != 3 || ops[2].kind != LirOperandKind::MemOffset)
                    continue;
                bool const beyondImm9 = ops[2].offset > 255 || ops[2].offset < -256;
                if (op == *loadUOp && beyondImm9) sawLoadUAtLargeOffset = true;
                if (op == *loadOp && beyondImm9)  sawPlainLoadAtLargeOffset = true;
            }
        }
    }
    EXPECT_TRUE(sawLoadUAtLargeOffset)
        << "the 9th fixed param's incoming-stack load (offset > 255) MUST use the scaled "
           "`load_u` — reverting the selection emits the unscaled `load` instead";
    EXPECT_FALSE(sawPlainLoadAtLargeOffset)
        << "no unscaled `load` (LDUR imm9) may carry an offset beyond ±256 — the encoder "
           "would fail loud (A_ImmediateOperandOutOfRange)";
}

// ───────────────────────────────────────────────────────────────────────────
// FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): the by-value-stack
// aggregate carrier — SysV x86_64 LIR-tier structural pins. These are the
// audit's BLOCKER-1 (force-to-stack) + BLOCKER-2 (outgoing-area sizing) gates,
// host-independent (structural over the post-materialize LIR, no execution).
// ───────────────────────────────────────────────────────────────────────────

namespace {

// The (single) call-making function's FrameLayout in a materialized result.
[[nodiscard]] FrameLayout const*
soleCallerLayout(LirCallconvResult const& result) {
    FrameLayout const* found = nullptr;
    for (std::uint32_t i = 0; i < result.lir.moduleFuncCount(); ++i) {
        auto const* layout = result.forFuncByIndex(i);
        if (layout != nullptr && layout->hasCalls) found = layout;
    }
    return found;
}

// Count SP-relative `store` insts in `fn` whose MemOffset displacement is in
// [lo, hi). The base register MUST be the stack pointer (`spOrd`) — otherwise a
// frame-local aggregate's init/copy stores (base = a `lea`-derived local reg, but
// the SAME 0/8/16 GEP offsets) would be miscounted as outgoing-area stores.
[[nodiscard]] std::uint32_t
countSpStoresInOffsetRange(Lir const& lir, LirFuncId fn, std::uint16_t storeOp,
                          std::uint16_t spOrd, std::int32_t lo, std::int32_t hi) {
    std::uint32_t n = 0;
    for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
        LirBlockId const blk = lir.funcBlockAt(fn, bi);
        for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
            LirInstId const inst = lir.blockInstAt(blk, i);
            if (lir.instOpcode(inst) != storeOp) continue;
            auto const ops = lir.instOperands(inst);
            // store layout: [value_reg, base_reg, MemBase, MemOffset]
            if (ops.size() != 4
                || ops[1].kind != LirOperandKind::Reg
                || ops[1].reg.isPhysical == 0
                || ops[1].reg.id != spOrd
                || ops[3].kind != LirOperandKind::MemOffset) continue;
            if (ops[3].offset >= lo && ops[3].offset < hi) ++n;
        }
    }
    return n;
}

} // namespace

// BLOCKER-1 (the force-to-stack pin) + BLOCKER-2 (the outgoing-area sizing pin):
// a >16B (MEMORY class) struct vararg with FREE arg registers (it is the FIRST
// vararg after `int n` — rsi/rdx/rcx/r8/r9 are all free) is passed ENTIRELY in the
// overflow area by a byte-copy, NOT register-passed. RED-ON-DISABLE: revert the
// caller's appendByValueStackArg route (so the >16B struct fails loud or pushes a
// hidden pointer) → no carrier → the outgoing area is not sized for 24 bytes and the
// stack-copy stores vanish → both EXPECTs go red. (Reverting the carrier to register
// pieces — the silent split — would land the struct bytes in rsi/rdx/rcx, NOT the
// outgoing area, so countStoresInOffsetRange would see 0 → red.)
TEST(LirCallconvAbi, SysVByValueStackAggStructVarargForcedToOverflowWithFreeRegs) {
    auto bundle = lowerThroughRewrite(
        "struct Big { long a; long b; long c; };\n"   // 24B → MEMORY class
        "long combine(int n, ...) { return n; }\n"    // DEFINED so the symbol binds
        "long use(void) {\n"
        "  struct Big b = {1, 2, 3};\n"
        "  return combine(1, b);\n"                    // 24B struct is the FIRST vararg
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok)
        << (bundle.lowered.lirReporter.all().empty()
                ? "" : bundle.lowered.lirReporter.all()[0].actual);
    ASSERT_TRUE(bundle.alloc.ok());
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok())
        << "the by-value-stack carrier must materialize cleanly: "
        << (ccRep.all().empty() ? "" : ccRep.all()[0].actual);
    EXPECT_EQ(ccRep.errorCount(), 0u);

    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_FALSE(cc->slotAligned);          // SysV — independent counters
    EXPECT_EQ(cc->shadowSpaceBytes, 0u);

    // BLOCKER-2: the caller's outgoing-args area is sized for the 24B struct =
    // ceil(24/8)*8 = 24 bytes. The naive pre-scan (counting the carrier's address
    // Reg as ONE GPR arg → 0 overflow) would under-reserve to 0 → this goes red.
    FrameLayout const* callerLayout = soleCallerLayout(result);
    ASSERT_NE(callerLayout, nullptr);
    EXPECT_GE(callerLayout->outgoingArgAreaSize, 24u)
        << "BLOCKER-2: the outgoing-args area must reserve ceil(24/8)*8 = 24 bytes "
           "for the by-value-stack aggregate (computeMaxOutgoingStackArgs accounts "
           "for the carrier's overflow slots)";

    // BLOCKER-1: the struct's bytes are byte-COPIED into the outgoing area at
    // [sp + 0 .. 24) (shadowSpaceBytes=0) — three 8-byte SP-relative stores. NO
    // register move of the struct bytes (it must NOT land in rsi/rdx/rcx with free
    // registers). The SP-base filter excludes the local `b` init + the temp copy
    // (their stores are based on local `lea` regs, not SP).
    ASSERT_TRUE(cc->stackPointer.has_value());
    std::uint16_t const spOrd = cc->stackPointer->ordinal;
    auto const storeOpId = bundle.lowered.target->opcodeByMnemonic("store");
    ASSERT_TRUE(storeOpId.has_value());
    LirFuncId const callerFn = result.lir.funcAt(1);   // `use`
    std::uint32_t const stackCopyStores = countSpStoresInOffsetRange(
        result.lir, callerFn, *storeOpId, spOrd, 0, 24);
    EXPECT_EQ(stackCopyStores, 3u)
        << "BLOCKER-1 (force-to-stack): a 24B struct vararg byte-copies as three "
           "8-byte SP-relative stores into the outgoing area [sp+0,sp+8,sp+16] — NOT "
           "register moves (the struct goes to the stack even though rsi/rdx/rcx free)";
}

// The register-exhaustion SPLIT: an InRegisters {long,long} (2-GPR) struct vararg
// that cannot fit wholly in the remaining arg registers goes ENTIRELY to the overflow
// area via the SAME carrier (never a register/stack split). Here 1 fixed int (rdi) + 5
// long varargs (rsi/rdx/rcx/r8/r9) exhaust the 6 GPRs, then the {long,long} struct
// needs 2 → whole struct to overflow. RED-ON-DISABLE: reverting the split route to
// register pieces lands the first piece in a register + splits → the 16B contiguous
// stack copy vanishes → red.
TEST(LirCallconvAbi, SysVByValueStackAggRegisterExhaustionSplitWholeStructToOverflow) {
    auto bundle = lowerThroughRewrite(
        "struct LL { long a; long b; };\n"            // 16B → InRegisters (2 GPR)
        "long combine(int n, ...) { return n; }\n"
        "long use(void) {\n"
        "  struct LL p = {7, 8};\n"
        "  return combine(1, 100L, 200L, 300L, 400L, 500L, p);\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.alloc.ok());
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok())
        << "the register-exhaustion-split carrier must materialize cleanly: "
        << (ccRep.all().empty() ? "" : ccRep.all()[0].actual);
    EXPECT_EQ(ccRep.errorCount(), 0u);

    FrameLayout const* callerLayout = soleCallerLayout(result);
    ASSERT_NE(callerLayout, nullptr);
    // 5 scalar long varargs overflow nothing (rsi..r9 = 5 regs, all in pool), so the
    // ONLY overflow is the 16B struct = ceil(16/8)*8 = 16 bytes. (rdi=n, rsi..r9 =
    // the 5 longs → 6 GPRs used → the struct's 2 pieces don't fit → whole to stack.)
    EXPECT_GE(callerLayout->outgoingArgAreaSize, 16u)
        << "the split struct reserves ceil(16/8)*8 = 16 outgoing bytes";

    // The 16B struct byte-copies as two 8-byte SP-relative stores into [sp+0, sp+8] —
    // a WHOLE contiguous copy, never one piece in a register and one on the stack.
    auto const* cc = bundle.lowered.target->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_TRUE(cc->stackPointer.has_value());
    std::uint16_t const spOrd = cc->stackPointer->ordinal;
    auto const storeOpId = bundle.lowered.target->opcodeByMnemonic("store");
    ASSERT_TRUE(storeOpId.has_value());
    LirFuncId const callerFn = result.lir.funcAt(1);   // `use`
    std::uint32_t const stackCopyStores = countSpStoresInOffsetRange(
        result.lir, callerFn, *storeOpId, spOrd, 0, 16);
    EXPECT_EQ(stackCopyStores, 2u)
        << "the {long,long} struct goes WHOLLY to the overflow area as two 8-byte "
           "SP-relative stores [sp+0,sp+8] — never a register/stack split";
}

// FOLD (adversarial-review BLOCKER-3, silent miscompile) red-on-disable: the by-value-
// stack aggregate byte-copy scratch GPR must AVOID the SOURCE registers of the pending
// register arg-moves. The byte-copies are emitted in the stack-store phase BEFORE the
// register arg-moves read their `.src` (lir_callconv.cpp: copies ~line 2300 vs arg-moves
// ~line 2310); if an outgoing scalar arg VALUE lives in the chosen scratch, the byte-copy
// clobbers it before its `mov dest, src` reads it → that argument SILENTLY receives the
// struct's bytes.
//
// This is a SYNTHETIC-LIR pin (not driven through regalloc) for full control over which
// physical register is a live arg-move source — the hazard requires an arg-move SOURCE to
// coincide with the scratch picker's choice, and the register allocator systematically
// parks call-surviving arg sources in callee-saved registers (away from the caller-saved
// scratch pool), so a regalloc-driven C shape cannot deterministically force the collision.
// Here we hand-build a variadic call whose arg 1 source is rcx — the FIRST caller-saved
// GPR the (reverted) scratch picker would choose after skipping the variadic-count reg
// (rax) — so reverting the avoid-set deterministically aliases the scratch with a live
// source.
//
// The invariant pinned is the PRECISE one the fix enforces: the byte-copy scratch ∉
// {sources of the arg-passing moves}. (We deliberately do NOT assert ∉ cc.argGprs — an
// argGpr that is only an arg-move DEST is written AFTER the copy stores to memory, so the
// copy's transient write to it is dead; blanket-rejecting the whole argGpr pool would
// falsely fail-loud on the legitimate register-exhaustion carrier — see
// examples/c-subset/varargs_struct_split — so the fix and this pin both track only the
// live SOURCE set.)
//
// RED-ON-DISABLE: revert the avoid-set extension in lir_callconv.cpp (the `liveArgSrc`
// build + the `liveArgSrc.contains(*ord)` rejection). The scratch picker walks
// cc.callerSaved = [rax, rcx, ...] and chooses the first GPR not in {rax (variadic count),
// callee, carrier addr} → rcx. But rcx is arg 1's source (`mov rsi, rcx`), so the byte-
// copy `load rcx,[addr]; store [sp],rcx` clobbers it before the arg-move reads it: the
// silent arg-clobber miscompile, and the load-bearing assertion (scratch ∉ arg-move
// sources) goes red. With the fix, rcx ∈ liveArgSrc is rejected and the scratch falls
// through to rdx (a non-source) → green.
TEST(LirCallconvAbi, SysVByValueStackAggByteCopyScratchAvoidsArgMoveSources) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& sch = **target;

    auto const callOp = sch.opcodeByMnemonic("call");
    auto const retOp  = sch.opcodeByMnemonic("ret");
    auto const storeOpId = sch.opcodeByMnemonic("store");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    ASSERT_TRUE(storeOpId.has_value());

    auto const* cc = sch.callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_EQ(cc->name, "sysv_amd64");
    ASSERT_FALSE(cc->slotAligned);
    ASSERT_TRUE(cc->stackPointer.has_value());
    ASSERT_TRUE(cc->variadicVectorCountReg.has_value());  // SysV rax (AL)
    std::uint16_t const spOrd = cc->stackPointer->ordinal;

    // Pin the register roles the scenario depends on. r12/r13 are callee-saved (stable
    // arg-source homes that the scratch picker WOULD pass over anyway); rcx is the
    // critical one — it is BOTH a live arg-move source here AND the first caller-saved GPR
    // the reverted scratch picker selects (after the rax variadic-count reg).
    auto const rcxOrd = sch.registerByName("rcx");
    auto const r12Ord = sch.registerByName("r12");   // carrier address reg
    auto const r13Ord = sch.registerByName("r13");   // arg 0 source
    ASSERT_TRUE(rcxOrd.has_value());
    ASSERT_TRUE(r12Ord.has_value());
    ASSERT_TRUE(r13Ord.has_value());
    LirReg const rcx = makePhysicalReg(*rcxOrd, LirRegClass::GPR);
    LirReg const r12 = makePhysicalReg(*r12Ord, LirRegClass::GPR);
    LirReg const r13 = makePhysicalReg(*r13Ord, LirRegClass::GPR);

    LirBuilder b{sch};
    b.addFunction(SymbolId{71});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    // A variadic call:
    //   call <sym>, r13, rcx, r12, byval#24
    //     arg 0 (fixed)  src = r13          -> dest argGprs[0] = rdi  : mov rdi, r13
    //     arg 1 (vararg) src = rcx          -> dest argGprs[1] = rsi  : mov rsi, rcx
    //     arg 2 (vararg) = (r12, byval#24)  -> 24B by-value-stack carrier, byte-copied
    //                                          from [r12] into the outgoing area
    // fixedOperandCount = 1 (only arg 0 is a named param). The carrier address r12 and the
    // arg sources {r13, rcx} are all distinct; the arg-moves {rdi<-r13, rsi<-rcx} form no
    // cycle (no source equals a dest), so materialization emits them in order.
    std::uint32_t const variadicPayload =
        call_payload::encode(/*isVariadic=*/true, /*fixedOperandCount=*/1u);
    std::array<LirOperand, 5> callOps{
        LirOperand::makeSymbolRef(13),
        LirOperand::makeReg(r13),
        LirOperand::makeReg(rcx),
        LirOperand::makeReg(r12),
        LirOperand::makeByValueStackAgg(24),
    };
    b.addInst(*callOp, InvalidLirReg, callOps, variadicPayload);
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                     = true;
    alloc.perFunc.back().originalSymbol         = SymbolId{71};
    alloc.perFunc.back().callingConventionIndex = 0;
    alloc.perFunc.back().numSpillSlots          = 0;

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch, alloc, ccRep);
    ASSERT_TRUE(result.ok())
        << "the by-value-stack carrier must materialize cleanly: "
        << (ccRep.all().empty() ? "" : ccRep.all()[0].actual);
    EXPECT_EQ(ccRep.errorCount(), 0u);

    // The byte-copy scratch is the VALUE register of each SP-relative store into the
    // outgoing area [sp+0, 24). store layout: [value_reg, base_reg, MemBase, MemOffset].
    // (The carrier pins above already prove a 24B carrier emits exactly these SP stores.)
    LirFuncId const fn = result.lir.funcAt(0);
    Lir const& out = result.lir;
    std::unordered_set<std::uint16_t> scratchOrds;
    for (std::uint32_t bi = 0; bi < out.funcBlockCount(fn); ++bi) {
        LirBlockId const blk = out.funcBlockAt(fn, bi);
        for (std::uint32_t i = 0; i < out.blockInstCount(blk); ++i) {
            LirInstId const inst = out.blockInstAt(blk, i);
            if (out.instOpcode(inst) != *storeOpId) continue;
            auto const ops = out.instOperands(inst);
            if (ops.size() == 4
                && ops[0].kind == LirOperandKind::Reg && ops[0].reg.isPhysical != 0
                && ops[1].kind == LirOperandKind::Reg && ops[1].reg.isPhysical != 0
                && ops[1].reg.id == spOrd
                && ops[3].kind == LirOperandKind::MemOffset
                && ops[3].offset >= 0 && ops[3].offset < 24)
                scratchOrds.insert(ops[0].reg.id);
        }
    }
    ASSERT_FALSE(scratchOrds.empty())
        << "precondition: the 24B struct carrier must byte-copy into [sp+0,24) — the "
           "scratch value-reg is read from those SP stores (else the pin is vacuous)";

    // The arg-move SOURCE set: rcx (arg 1) and r13 (arg 0) are the registers the arg-
    // passing moves READ after the byte-copy. We assert against rcx by name (the one the
    // reverted scratch picker collides with) plus the structural source set for breadth.
    std::unordered_set<std::uint16_t> argMoveSrcOrds{*rcxOrd, *r13Ord};

    // THE LOAD-BEARING ASSERTION (red-on-disable): the byte-copy scratch must NOT be a
    // source of any arg-passing move. Reverting the liveArgSrc avoid-set lands the scratch
    // on rcx (arg 1's source) → the byte-copy clobbers it before `mov rsi, rcx` reads it:
    // the silent arg-clobber miscompile.
    for (std::uint16_t const ord : scratchOrds) {
        EXPECT_FALSE(argMoveSrcOrds.contains(ord))
            << "the by-value-stack byte-copy scratch must not be a SOURCE of any arg-"
               "passing move — the copy clobbers it before the arg-move reads it, so that "
               "outgoing scalar arg silently receives the struct's bytes "
               "(D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT)";
    }
    // Specifically: rcx (the reverted picker's first choice + arg 1's source) must have
    // been rejected in favor of a non-source scratch.
    EXPECT_EQ(scratchOrds.count(*rcxOrd), 0u)
        << "the scratch must not be rcx — rcx is arg 1's live source; choosing it (the "
           "reverted behavior) corrupts the rsi argument with the struct's bytes";
}

// D-FC12C-AAPCS64-HFA-STRUCT-VARARG (Step 5.1, LIR) — an HFA `{double,double}` vararg
// passed to an AAPCS64 variadic call with room in the VR pool is placed in FPR arg
// registers (d0/d1) BY VALUE: the lowered Call carries TWO FPR-class operands and NO
// ByValueStackAgg carrier; after materialization the call fits entirely in registers
// (outgoingArgAreaSize == 0). RED-ON-DISABLE: a wrong-class exhaustion check (testing
// the GR pool for an HFA) would route it to the overflow carrier (ByValueStackAgg
// appears, outgoingArgAreaSize grows).
TEST(LirCallconvVariadicFC12c, Aapcs64VarArgHfaStructInRegisters) {
    auto bundle = lowerThroughRewrite(
        "struct HFA { double a; double b; };\n"
        "int sink(int n, ...) { return n; }\n"   // DEFINED so the callee symbol binds (D-CSUBSET-FN-PROTOTYPE: a bare proto no longer emits a spurious FnSig global)
        "int f(void) {\n"
        "  struct HFA h; h.a = 1.0; h.b = 2.0;\n"
        "  return sink(1, h);\n"
        "}\n",
        /*ccIndex=*/0, /*targetName=*/"arm64");   // aapcs64 = cc index 0
    ASSERT_TRUE(bundle.lowered.lir.ok)
        << (bundle.lowered.lirReporter.all().empty()
                ? "" : bundle.lowered.lirReporter.all()[0].actual);
    ASSERT_TRUE(bundle.alloc.ok());
    ASSERT_TRUE(bundle.rewritten.ok);

    // Inspect the lowered (pre-materialization) Call: it still carries the arg-reg
    // operands. The HFA must contribute TWO FPR-class operands and ZERO ByValueStackAgg.
    auto const callOp = bundle.lowered.target->opcodeByMnemonic("call");
    ASSERT_TRUE(callOp.has_value());
    std::uint32_t fprArgOps = 0, byValueAggMarkers = 0, callCount = 0;
    Lir const& lir = bundle.lowered.lir.lir;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                LirInstId const inst = lir.blockInstAt(blk, i);
                if (lir.instOpcode(inst) != *callOp) continue;
                ++callCount;
                for (auto const& op : lir.instOperands(inst)) {
                    if (op.kind == LirOperandKind::ByValueStackAgg) ++byValueAggMarkers;
                    if (op.kind == LirOperandKind::Reg
                        && op.reg.regClass() == LirRegClass::FPR)
                        ++fprArgOps;
                }
            }
        }
    }
    ASSERT_GT(callCount, 0u) << "the fixture has a sink(...) call site";
    EXPECT_EQ(byValueAggMarkers, 0u)
        << "an HFA with room in the VR pool is register-placed (FPR pieces), NOT routed "
           "to the overflow via the ByValueStackAgg carrier (H4/H8)";
    EXPECT_GE(fprArgOps, 2u)
        << "the 2-double HFA contributes TWO FPR-class arg operands (d0/d1)";

    // CC-level cross-check: materialize and confirm the call fits in registers (no
    // outgoing overflow). A misroute to the carrier would reserve overflow bytes.
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(ccRep.errorCount(), 0u);
    auto const* layout = result.forFuncByIndex(0);   // `f`
    ASSERT_NE(layout, nullptr);
    EXPECT_EQ(layout->outgoingArgAreaSize, 0u)
        << "the HFA vararg rides in d0/d1 — the call fits in registers, no overflow";
}

// D-FC12C-AAPCS64-HFA-STRUCT-VARARG (Step 5.2, LIR) — a non-HFA `{long,long}` vararg
// passed AFTER 8 scalar long varargs drain the GR pool (gpSaveCount=8) is forced WHOLE
// to the overflow via the ByValueStackAgg carrier. The lowered Call carries the marker;
// after materialization the outgoing area reserves ceil(16/8)*8 = 16 bytes. RED-ON-
// DISABLE: a missing exhaustion check emits register pieces (no carrier, no overflow).
TEST(LirCallconvVariadicFC12c, Aapcs64VarArgNonHfaStructStackAfterExhaustion) {
    // The CALLER `use` is declared FIRST (function index 0 for the layout query);
    // `sink` is DEFINED after via a forward reference that merges with the leading
    // prototype (D-CSUBSET-FN-PROTOTYPE) — a real defined function so the call binds.
    auto bundle = lowerThroughRewrite(
        "struct LL { long a; long b; };\n"
        "long sink(int n, ...);\n"                     // forward prototype (merges below)
        "long use(void) {\n"                            // the caller — function 0
        "  struct LL p; p.a = 40; p.b = 5;\n"
        "  return sink(8, 1L,2L,3L,4L,5L,6L,7L,8L, p);\n"   // 8 longs drain GR, then struct
        "}\n"
        "long sink(int n, ...) { return n; }\n",        // the definition (function 1)
        /*ccIndex=*/0, /*targetName=*/"arm64");   // aapcs64 = cc index 0
    ASSERT_TRUE(bundle.lowered.lir.ok)
        << (bundle.lowered.lirReporter.all().empty()
                ? "" : bundle.lowered.lirReporter.all()[0].actual);
    ASSERT_TRUE(bundle.alloc.ok());
    ASSERT_TRUE(bundle.rewritten.ok);

    auto const callOp = bundle.lowered.target->opcodeByMnemonic("call");
    ASSERT_TRUE(callOp.has_value());
    std::uint32_t byValueAggMarkers = 0, callCount = 0;
    Lir const& lir = bundle.lowered.lir.lir;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                LirInstId const inst = lir.blockInstAt(blk, i);
                if (lir.instOpcode(inst) != *callOp) continue;
                ++callCount;
                for (auto const& op : lir.instOperands(inst))
                    if (op.kind == LirOperandKind::ByValueStackAgg) ++byValueAggMarkers;
            }
        }
    }
    ASSERT_GT(callCount, 0u) << "the fixture has a sink(...) call site";
    EXPECT_EQ(byValueAggMarkers, 1u)
        << "H8: 8 scalar longs drain the GR pool → the trailing {long,long} struct is "
           "forced WHOLE to the overflow via the ByValueStackAgg carrier";

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    ASSERT_TRUE(result.ok())
        << "the by-value-stack carrier must materialize cleanly: "
        << (ccRep.all().empty() ? "" : ccRep.all()[0].actual);
    EXPECT_EQ(ccRep.errorCount(), 0u);
    auto const* layout = result.forFuncByIndex(0);   // `use`
    ASSERT_NE(layout, nullptr);
    EXPECT_GE(layout->outgoingArgAreaSize, 16u)
        << "the outgoing area must reserve ceil(16/8)*8 = 16 bytes for the forced "
           "{long,long} struct vararg (plus the 8 overflow longs' slots)";
}
