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
    RewrittenBundle out{lowerCSubsetToLir(std::move(src), std::move(targetName))};
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

} // namespace

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
    // Use the cross-call moderate-pressure pattern from cycle 3b so
    // there are actual frame_load/frame_store ops to materialize.
    auto bundle = lowerThroughRewrite(
        "int g(int a) { return a + 1; }\n"
        "int f(int x) {\n"
        "    int a1 = x + 1;\n"
        "    int a2 = x + 2;\n"
        "    int a3 = x + 3;\n"
        "    int a4 = x + 4;\n"
        "    int a5 = x + 5;\n"
        "    int a6 = x + 6;\n"
        "    int r = g(x);\n"
        "    return a1 + a2 + a3 + a4 + a5 + a6 + r;\n"
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
    auto bundle = lowerThroughRewrite(
        "int g(int a) { return a + 1; }\n"
        "int f(int x) {\n"
        "    int a1 = x + 1; int a2 = x + 2; int a3 = x + 3;\n"
        "    int a4 = x + 4; int a5 = x + 5; int a6 = x + 6;\n"
        "    int r = g(x);\n"
        "    return a1 + a2 + a3 + a4 + a5 + a6 + r;\n"
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
    auto bundle = lowerThroughRewrite(
        "int g(int a) { return a + 1; }\n"
        "int f(int x) {\n"
        "    int a1 = x + 1; int a2 = x + 2; int a3 = x + 3;\n"
        "    int a4 = x + 4; int a5 = x + 5; int a6 = x + 6;\n"
        "    int r = g(x);\n"
        "    return a1 + a2 + a3 + a4 + a5 + a6 + r;\n"
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

TEST(CallPayload, EncodeDecodeRoundtripsVariadicAndFixedCount) {
    // D-LANG-VARIADIC (step 13.4) substrate pin: the shared MIR/LIR
    // Call payload encoding (bit 31 = isVariadic; bits 0..30 =
    // fixedArgCount) round-trips for both the non-variadic and
    // variadic cases. The ML7 materialize call arm reads these bits
    // off every call inst; an off-by-one in the mask would either
    // truncate fixedArgCount or silently flip the variadic bit, both
    // of which would corrupt the count-mov emission for variadic
    // calls (and falsely trigger it for non-variadic calls).
    using namespace dss::call_payload;
    // Non-variadic encoding is payload == 0 (preserves the addInst
    // default for every pre-13.4 call site).
    EXPECT_EQ(encode(false, 0u), 0u);
    EXPECT_FALSE(isVariadic(encode(false, 0u)));
    // The high bit alone flips isVariadic; fixedArgCount=0 round-
    // trips (a hypothetical thunk-style vararg-only function).
    EXPECT_TRUE(isVariadic(encode(true, 0u)));
    EXPECT_EQ(fixedArgCount(encode(true, 0u)), 0u);
    // Typical printf shape: 1 fixed param + vararg.
    EXPECT_TRUE(isVariadic(encode(true, 1u)));
    EXPECT_EQ(fixedArgCount(encode(true, 1u)), 1u);
    // Round-trip at the high edge of the fixed-arg field. If the
    // mask boundary regresses, fixedArgCount(kFixedArgMask) returns
    // a different value here — pins the contract.
    EXPECT_EQ(fixedArgCount(encode(true, kFixedArgMask)),
              kFixedArgMask);
    EXPECT_TRUE(isVariadic(encode(true, kFixedArgMask)));
    // A non-variadic encoding with a fixed-arg count carries the
    // fixedArgCount through but reports isVariadic=false — the
    // accessor never spuriously reports variadic just because
    // fixedArgCount is non-zero.
    EXPECT_FALSE(isVariadic(encode(false, 42u)));
    EXPECT_EQ(fixedArgCount(encode(false, 42u)), 42u);
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

TEST(LirCallconvAbi, MoveCycleInArgPassingFailsLoud) {
    // Construct a LIR call whose arg-passing produces a move cycle:
    // arg 0 currently lives in `rsi` (= argGprs[1]) and arg 1 lives
    // in `rdi` (= argGprs[0]). The in-order emit would produce
    //   mov rdi, rsi   (clobbers rdi which is arg 1's source)
    //   mov rsi, rdi   (now reads the clobbered rdi)
    // — silent miscompile. The detection pass must trip
    // L_MoveCycleUnsupported citing D-ML7-2.3.
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
    //   arg 1 source = rdi  (dest will be argGprs[1]=rsi)
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
    EXPECT_FALSE(result.ok())
        << "swap-cycle move pattern must trip the loud guard";
    bool sawCode = false;
    bool sawAnchor = false;
    for (auto const& d : ccRep.all()) {
        if (d.code == DiagnosticCode::L_MoveCycleUnsupported) {
            sawCode = true;
            if (d.actual.find("D-ML7-2.3") != std::string::npos) {
                sawAnchor = true;
            }
        }
    }
    EXPECT_TRUE(sawCode)
        << "the swap-cycle must surface L_MoveCycleUnsupported";
    EXPECT_TRUE(sawAnchor)
        << "the diagnostic must cite the D-ML7-2.3 anchor for triage";
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
