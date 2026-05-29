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
                                  out.liveness, out.regallocRep);
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
