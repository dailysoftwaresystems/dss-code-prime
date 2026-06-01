// LIR calling-convention materialization tests (ML7 cycle 1). Pins:
//   * Prologue inserted at function entry (saved-reg stores + SP sub)
//   * Epilogue inserted before every return (saved-reg loads + SP add)
//   * frame_load / frame_store materialized to load / store with
//     SP-relative addressing
//   * Block topology preserved (1:1 mapping)
//   * Per-function FrameLayout populated correctly
//   * No frame_load/frame_store ops remain in the output module

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
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

[[nodiscard]] RewrittenBundle
lowerThroughRewrite(std::string src) {
    RewrittenBundle out{lowerCSubsetToLir(std::move(src))};
    if (!out.lowered.lir.ok) return out;
    out.liveness = analyzeLiveness(out.lowered.lir.lir);
    out.alloc = allocateRegisters(out.lowered.lir.lir, *out.lowered.target,
                                  out.liveness, /*ccIndex=*/0,
                                  out.regallocRep);
    if (!out.alloc.ok()) return out;
    out.rewritten = rewriteWithAllocation(out.lowered.lir.lir,
                                          *out.lowered.target, out.alloc,
                                          out.rewriteRep);
    return out;
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
        EXPECT_EQ(layout.spillAreaOffset(), layout.savedRegAreaSize);
        EXPECT_EQ(layout.savedRegAreaSize,
                  static_cast<std::uint32_t>(layout.savedRegs.size()) * layout.slotSize);
        EXPECT_EQ(layout.spillAreaSize,
                  bundle.alloc.perFunc[i].numSpillSlots * layout.slotSize);
        std::uint32_t const raw = layout.savedRegAreaSize + layout.spillAreaSize;
        std::uint32_t const expected = (raw + cc->stackAlignment - 1u)
                                     & ~(cc->stackAlignment - 1u);
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
    for (auto const& layout : result.perFunc) {
        EXPECT_EQ(layout.totalFrameSize % cc->stackAlignment, 0u)
            << "every function's frame size must be a multiple of "
            << cc->stackAlignment;
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
};

[[nodiscard]] InstStats
collectInstStats(Lir const& lir, TargetSchema const& schema) {
    InstStats s;
    auto const argOp  = schema.opcodeByMnemonic("arg");
    auto const callOp = schema.opcodeByMnemonic("call");
    auto const movOp  = schema.opcodeByMnemonic("mov");
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const b = lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < lir.blockInstCount(b); ++i) {
                LirInstId const inst = lir.blockInstAt(b, i);
                std::uint16_t const op = lir.instOpcode(inst);
                if (argOp.has_value()  && op == *argOp)  ++s.argOps;
                if (movOp.has_value()  && op == *movOp)  ++s.movOps;
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

    // The cc's argGprs[0] must resolve via schema.registerByName.
    auto const argGpr0Ord =
        bundle.lowered.target->registerByName(cc->argGprs[0]);
    ASSERT_TRUE(argGpr0Ord.has_value());

    // Walk all movs in the post-callconv module and find at least one
    // whose source is argGprs[0]. This is the arg-materialization mov
    // (UNLESS regalloc happened to pick argGprs[0] as the home reg,
    // in which case there's no mov — also a valid outcome).
    auto const movOp =
        bundle.lowered.target->opcodeByMnemonic("mov");
    ASSERT_TRUE(movOp.has_value());
    bool sawArgGpr0Source = false;
    bool sawArgGpr0Result = false;
    for (std::uint32_t fi = 0; fi < result.lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = result.lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
            LirBlockId const b = result.lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < result.lir.blockInstCount(b); ++i) {
                LirInstId const inst = result.lir.blockInstAt(b, i);
                if (result.lir.instOpcode(inst) != *movOp) continue;
                LirReg const r = result.lir.instResult(inst);
                if (r.valid() && r.isPhysical != 0
                    && r.id == *argGpr0Ord) {
                    sawArgGpr0Result = true;
                }
                for (auto const& op : result.lir.instOperands(inst)) {
                    if (op.kind == LirOperandKind::Reg
                        && op.reg.isPhysical != 0
                        && op.reg.id == *argGpr0Ord) {
                        sawArgGpr0Source = true;
                    }
                }
            }
        }
    }
    EXPECT_TRUE(sawArgGpr0Source || sawArgGpr0Result)
        << "the arg materialization must touch argGprs[0] ("
        << cc->argGprs[0] << ") either as a mov source (arg copy) "
           "or as the regalloc-picked home reg (no-op skipped mov)";
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

TEST(LirCallconvAbi, RejectsStackPassedGprArgLoud) {
    // SysV AMD64 has 6 GPR arg registers (rdi/rsi/rdx/rcx/r8/r9). A
    // 7-arg int function trips `arg 6` which exceeds the pool; the
    // materialization must fail loud with `L_StackPassedArgUnsupported`
    // citing D-ML7-2.2 — not silently miscompile by reading past the
    // argGprs vector.
    auto bundle = lowerThroughRewrite(
        "int f7(int a, int b, int c, int d, int e, int f, int g) {\n"
        "    return g;\n"
        "}\n");
    ASSERT_TRUE(bundle.lowered.lir.ok);
    ASSERT_TRUE(bundle.rewritten.ok);
    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(bundle.rewritten.lir,
                                               *bundle.lowered.target,
                                               bundle.alloc, ccRep);
    EXPECT_FALSE(result.ok())
        << "7-arg function must trip the stack-passed guard";
    bool sawCode = false;
    bool sawAnchor = false;
    for (auto const& d : ccRep.all()) {
        if (d.code == DiagnosticCode::L_StackPassedArgUnsupported) {
            sawCode = true;
            if (d.actual.find("D-ML7-2.2") != std::string::npos) {
                sawAnchor = true;
            }
        }
    }
    EXPECT_TRUE(sawCode)
        << "the 7-arg overflow must surface L_StackPassedArgUnsupported";
    EXPECT_TRUE(sawAnchor)
        << "the diagnostic must cite the D-ML7-2.2 anchor for triage";
}

TEST(LirCallconvAbi, RejectsIndirectCallLoud) {
    // The c-subset frontend doesn't emit function-pointer calls, so
    // we synthesize a minimal LIR module by hand to drive the
    // indirect-call rejection path. The shape: one function with one
    // block whose only inst is `call <reg-callee>` — the callconv
    // pass must trip `L_IndirectCallUnsupported` citing D-ML7-2.4
    // rather than silently emit a malformed `call <reg>` that the
    // assembler can't encode (the schema's call variant guard is
    // `["symbol"]`).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& sch = **target;

    auto const callOp = sch.opcodeByMnemonic("call");
    ASSERT_TRUE(callOp.has_value());
    auto const retOp  = sch.opcodeByMnemonic("ret");
    ASSERT_TRUE(retOp.has_value());

    // Construct a synthetic LIR module: one function, one block with
    // `call <rax_reg>` followed by `ret`.
    LirBuilder b{sch};
    b.addFunction(SymbolId{42});
    LirBlockId const block = b.createBlock();
    b.beginBlock(block);
    auto const raxOrd = sch.registerByName("rax");
    ASSERT_TRUE(raxOrd.has_value());
    LirReg const rax = makePhysicalReg(*raxOrd, LirRegClass::GPR);
    // call <rax> — callee operand is a Reg, not a SymbolRef.
    std::array<LirOperand, 1> callOps{LirOperand::makeReg(rax)};
    b.addInst(*callOp, InvalidLirReg, callOps);
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    // Minimal LirAllocation with one valid per-function entry pointing
    // at cc index 0 (the materializer reads this to pick the cc).
    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                       = true;
    alloc.perFunc.back().originalSymbol           = SymbolId{42};
    alloc.perFunc.back().callingConventionIndex   = 0;
    alloc.perFunc.back().numSpillSlots            = 0;

    DiagnosticReporter ccRep;
    auto result = materializeCallingConvention(lir, sch, alloc, ccRep);
    EXPECT_FALSE(result.ok())
        << "indirect call (reg callee) must trip the loud guard";
    bool sawCode = false;
    bool sawAnchor = false;
    for (auto const& d : ccRep.all()) {
        if (d.code == DiagnosticCode::L_IndirectCallUnsupported) {
            sawCode = true;
            if (d.actual.find("D-ML7-2.4") != std::string::npos) {
                sawAnchor = true;
            }
        }
    }
    EXPECT_TRUE(sawCode)
        << "the reg-callee call must surface L_IndirectCallUnsupported";
    EXPECT_TRUE(sawAnchor)
        << "the diagnostic must cite the D-ML7-2.4 anchor for triage";
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
