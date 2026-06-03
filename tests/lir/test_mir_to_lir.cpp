// ML5 cycle 3a — MIR→LIR isel vertical slice tests.
// Drives the full c-subset → CST → HIR → MIR → LIR pipeline on minimal
// straight-line functions (Arg/Const/Add/Sub/Return) and pins the
// per-opcode lowering shape against the shipped x86_64 target schema.
//
// Same harness style as `tests/mir/test_mir_lowering_c_subset.cpp`: one
// `lowerCSubsetToLir(src)` helper threads each phase's diagnostics so
// assertions can disambiguate which layer flagged a failure.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "lir/lir.hpp"
#include "lir/lir_verifier.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_literal_pool.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "synthetic_fn.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

struct Lowered {
    SemanticModel                    model;
    std::unique_ptr<CstToHirResult>  hir;
    DiagnosticReporter               hirReporter;
    HirToMirResult                   mir;
    DiagnosticReporter               mirReporter;
    std::shared_ptr<TargetSchema>    target;
    DiagnosticReporter               lirReporter;
    MirToLirResult                   lir;
};

[[nodiscard]] Lowered lowerCSubsetToLir(std::string src) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) { ADD_FAILURE() << "loadShipped(c-subset) failed"; std::abort(); }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu    = std::make_shared<CompilationUnit>(std::move(builder).finish());
    auto model = analyze(cu);
    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);
    DiagnosticReporter mirReporter;
    MirLoweringConfig mirCfg;
    mirCfg.globalsAllowFloat = (*loaded)->hirLowering().globalsConstEval.allowFloat;
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter,
                                    &hir->sourceMap, mirCfg);
    auto target = TargetSchema::loadShipped("x86_64");
    if (!target) { ADD_FAILURE() << "loadShipped(x86_64) failed"; std::abort(); }
    DiagnosticReporter lirReporter;
    auto lir = lowerToLir(mir.mir, **target, model.lattice().interner(), lirReporter);
    return Lowered{
        .model       = std::move(model),
        .hir         = std::move(hir),
        .hirReporter = std::move(hirReporter),
        .mir         = std::move(mir),
        .mirReporter = std::move(mirReporter),
        .target      = std::move(*target),
        .lirReporter = std::move(lirReporter),
        .lir         = std::move(lir),
    };
}

// Test-helper: assert every prior phase succeeded so failure messages
// pinpoint the layer that broke. Used by every cycle-3a test below.
void assertUpstreamClean(Lowered const& L) {
    ASSERT_FALSE(L.model.hasErrors())
        << "semantic phase: " << (L.model.diagnostics().all().empty()
            ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok)
        << "HIR lowering: " << (L.hirReporter.all().empty()
            ? "" : L.hirReporter.all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);
}

}  // namespace

TEST(MirToLir, StraightLineAddLowersToLirAddSequence) {
    // The reference vertical slice. `int add(int a, int b) { return a+b; }`
    // → MIR { Arg(0), Arg(1), Add(%0,%1), Return(%2) }
    // → LIR { arg(payload=0), arg(payload=1), add(%0,%1), ret(%2) }.
    auto L = lowerCSubsetToLir("int add(int a, int b) { return a + b; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok)
        << "LIR lowering: " << (L.lirReporter.all().empty()
            ? "" : L.lirReporter.all()[0].actual);

    Lir const& lir = L.lir.lir;
    ASSERT_EQ(lir.moduleFuncCount(), 1u);
    LirFuncId const fn = lir.funcAt(0);
    EXPECT_EQ(lir.funcBlockCount(fn), 1u);

    LirBlockId const bb = lir.funcBlockAt(fn, 0);
    // The block carries: arg(0), arg(1), add, ret — 4 LIR instructions.
    EXPECT_EQ(lir.blockInstCount(bb), 4u);

    auto opOf = [&](std::uint32_t idx) {
        return lir.instOpcode(lir.blockInstAt(bb, idx));
    };
    auto const& sch = *L.target;
    EXPECT_EQ(opOf(0), *sch.opcodeByMnemonic("arg"));
    EXPECT_EQ(opOf(1), *sch.opcodeByMnemonic("arg"));
    EXPECT_EQ(opOf(2), *sch.opcodeByMnemonic("add"));
    EXPECT_EQ(opOf(3), *sch.opcodeByMnemonic("ret"));

    // The Return is the block terminator (per LirBuilder::addReturn).
    EXPECT_EQ(lir.blockTerminator(bb), lir.blockInstAt(bb, 3));

    // Argument-index payloads on the two `arg` insts must be 0 and 1.
    EXPECT_EQ(lir.instPayload(lir.blockInstAt(bb, 0)), 0u);
    EXPECT_EQ(lir.instPayload(lir.blockInstAt(bb, 1)), 1u);
}

TEST(MirToLir, ConstReturnLowersToMovRet) {
    // `int forty_two() { return 42; }`
    // → MIR { Const(42), Return(%0) }
    // → LIR { mov vN, 42 ; ret vN }.
    auto L = lowerCSubsetToLir("int forty_two() { return 42; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);

    Lir const& lir = L.lir.lir;
    ASSERT_EQ(lir.moduleFuncCount(), 1u);
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(bb), 2u);

    auto const& sch = *L.target;
    LirInstId const movId = lir.blockInstAt(bb, 0);
    LirInstId const retId = lir.blockInstAt(bb, 1);
    EXPECT_EQ(lir.instOpcode(movId), *sch.opcodeByMnemonic("mov"));
    EXPECT_EQ(lir.instOpcode(retId), *sch.opcodeByMnemonic("ret"));

    // The mov's source operand is the immediate 42.
    auto const movOperands = lir.instOperands(movId);
    ASSERT_EQ(movOperands.size(), 1u);
    EXPECT_EQ(movOperands[0].kind, LirOperandKind::ImmInt);
    EXPECT_EQ(movOperands[0].immInt32, 42);

    // The ret's value operand references the mov's result register.
    auto const retOperands = lir.instOperands(retId);
    ASSERT_EQ(retOperands.size(), 1u);
    EXPECT_EQ(retOperands[0].kind, LirOperandKind::Reg);
    EXPECT_EQ(retOperands[0].reg, lir.instResult(movId));
}

TEST(MirToLir, SubReturnLowersThreeInstructions) {
    // `int s(int a, int b) { return a - b; }` → 4 LIR insts: arg, arg, sub, ret.
    auto L = lowerCSubsetToLir("int s(int a, int b) { return a - b; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);

    auto const& sch = *L.target;
    Lir const& lir = L.lir.lir;
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(bb), 4u);

    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 2)),
              *sch.opcodeByMnemonic("sub"));
}

TEST(MirToLir, ReturnVoidLowersToBareRet) {
    auto L = lowerCSubsetToLir("void noop() { return; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);

    Lir const& lir = L.lir.lir;
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(bb), 1u);
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 0)),
              *L.target->opcodeByMnemonic("ret"));
    // Bare ret has no operands.
    EXPECT_EQ(lir.instOperands(lir.blockInstAt(bb, 0)).size(), 0u);
}

TEST(MirToLir, MultipleFunctionsEachIsolatedVRegSpace) {
    // Two functions must each restart at vreg 1; the per-function reset of
    // `valueToReg` + the builder's nextVReg counter prevents cross-pollution.
    auto L = lowerCSubsetToLir(
        "int a(int x) { return x; }\n"
        "int b(int y) { return y; }\n");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);

    Lir const& lir = L.lir.lir;
    ASSERT_EQ(lir.moduleFuncCount(), 2u);
    // Each function defines its own argument register independently. The
    // first inst of each must be `arg`.
    for (std::uint32_t i = 0; i < 2; ++i) {
        LirFuncId const fn = lir.funcAt(i);
        LirBlockId const bb = lir.funcBlockAt(fn, 0);
        EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 0)),
                  *L.target->opcodeByMnemonic("arg"));
    }
}

TEST(MirToLir, MulReturnLowersThreeInstructions) {
    auto L = lowerCSubsetToLir("int m(int a, int b) { return a * b; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);

    Lir const& lir = L.lir.lir;
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(bb), 4u);
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 2)),
              *L.target->opcodeByMnemonic("mul"));
}

// Cycle 3a wide-literal coverage (>INT32_MAX) is deferred to cycle 3b's
// synthetic-MIR helper — the c-subset semantic phase rejects out-of-range
// literals before they reach the LIR lowerer, so we can't exercise the
// `fits == false` branch via an end-to-end pipeline yet. The branch
// itself is live code; cycle 3b will land literal-pool wiring + a
// synthetic-MIR fixture that pins it.

TEST(MirToLir, RequiredLirOpcodeMissingFailsLoud) {
    // Synthetic-target test: a schema declaring NO `mov` opcode against a
    // MIR with a `Const` instruction must surface L_RequiredLirOpcodeMissing.
    // Pins the cycle-3a "target schema author shipped an incomplete config"
    // failure mode — without this test the missing-opcode diagnostics are
    // dead code.
    //
    // The synthetic target deliberately omits `mov` AND `arg`; with `ret`
    // present the fallback-seal still works so the LIR module finishes
    // cleanly + the diagnostic surfaces.
    auto incomplete = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"incomplete"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"add","result":"value","minOperands":2,"maxOperands":2},
              {"mnemonic":"ret","result":"none","isTerminator":true,
               "terminatorKind":"return",
               "minOperands":0,"maxOperands":1}
            ]})");
    ASSERT_TRUE(incomplete.has_value());

    // Drive MIR for `int f() { return 1; }`. The Const → mov path will hit
    // the missing-opcode branch.
    auto L = lowerCSubsetToLir("int f() { return 1; }");
    assertUpstreamClean(L);

    DiagnosticReporter rep;
    auto result = lowerToLir(L.mir.mir, **incomplete, L.model.lattice().interner(), rep);
    EXPECT_FALSE(result.ok);
    bool found = false;
    int  missingCount = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_RequiredLirOpcodeMissing) {
            ++missingCount;
            found = true;
        }
    }
    EXPECT_TRUE(found)
        << "schema missing required `mov` opcode must surface "
           "L_RequiredLirOpcodeMissing during MIR Const lowering";
    // Per-mnemonic one-shot: 10k Consts must not produce 10k diagnostics.
    // Test has 1 Const so 1 diagnostic is fine; pin "at most 1 per mnemonic".
    EXPECT_LE(missingCount, 1)
        << "L_RequiredLirOpcodeMissing must fire ONCE per mnemonic, not per inst";
}

TEST(MirToLir, UnsupportedMirOpcodeFailsLoud) {
    // Cycle 3e lowers Call/IntrinsicCall/GlobalAddr + degenerate
    // ExtractValue/InsertValue. Still-deferred: float comparisons
    // (FCmp*) and SIMD vector ops (VAdd/VSub/etc — reserved post-v1
    // per MirOpcode). Use synthetic-MIR with VAdd as the cleanest
    // still-deferred trigger.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;

    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const i32   = interner.primitive(::dss::TypeKind::I32);
    auto const fnSig = interner.fnSig(std::span<::dss::TypeId const>{}, i32, ::dss::CallConv::CcSysV);

    ::dss::MirBuilder mb;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirLiteralValue lv;
    lv.value = static_cast<std::int64_t>(0);
    lv.core  = ::dss::TypeKind::I32;
    ::dss::MirInstId const zero = mb.addConst(lv, i32);
    std::array<::dss::MirInstId, 2> ops{zero, zero};
    // VAdd is reserved SIMD — not lowered in any cycle yet.
    ::dss::MirInstId const v = mb.addInst(::dss::MirOpcode::VAdd, ops, i32);
    mb.addReturn(v);
    ::dss::Mir m = std::move(mb).finish();

    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, sch, interner, rep);
    EXPECT_FALSE(result.ok);
    bool foundUnsupported = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_UnsupportedLoweringForOpcode) {
            foundUnsupported = true;
            break;
        }
    }
    EXPECT_TRUE(foundUnsupported)
        << "MirOpcode::VAdd (SIMD, reserved) must surface "
           "L_UnsupportedLoweringForOpcode; silent acceptance is a regression";

    // Every LIR block must end in a terminator (the fallback seal
    // covers the case where the unsupported MIR opcode prevented the
    // normal terminator emission). Pin terminator-presence.
    ::dss::Lir const& lir = result.lir;
    for (std::uint32_t i = 0; i < lir.moduleFuncCount(); ++i) {
        LirFuncId const fn = lir.funcAt(i);
        for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
            LirBlockId const bb = lir.funcBlockAt(fn, b);
            LirInstId const term = lir.blockTerminator(bb);
            EXPECT_TRUE(sch.isTerminator(lir.instOpcode(term)))
                << "every LIR block must end in a terminator opcode";
        }
    }
}

// ─── cycle 3b vertical slice: CFG + comparisons ──────────────────────────

TEST(MirToLir, IfElseLowersToCondBrChain) {
    // `int sign(int x) { if (x > 0) return 1; return 0; }`
    // MIR: ICmpSgt + CondBr + return-blocks.
    // LIR: cmp+setcc / cmp+jcc / mov+ret in each branch / mov+ret in the
    // join. Cycle 3b's "lower each MIR op naively" approach (no
    // ICmp+CondBr peephole) is asserted here so the optimizer can later
    // delete the redundant cmp/setcc.
    auto L = lowerCSubsetToLir(
        "int sign(int x) { if (x > 0) return 1; return 0; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok)
        << "If/Else over a comparison must lower cleanly in cycle 3b";

    auto const& sch = *L.target;
    Lir const& lir = L.lir.lir;
    ASSERT_EQ(lir.moduleFuncCount(), 1u);
    LirFuncId const fn = lir.funcAt(0);
    EXPECT_GE(lir.funcBlockCount(fn), 2u);  // entry + at least one branch

    // The entry block ends in a jcc (CondBr lowered).
    LirBlockId const entry = lir.funcEntry(fn);
    LirInstId const entryTerm = lir.blockTerminator(entry);
    EXPECT_EQ(lir.instOpcode(entryTerm), *sch.opcodeByMnemonic("jcc"))
        << "entry block must end in jcc for an if/else";

    // Somewhere in the entry block there's a `cmp` (the CondBr-side compare)
    // and a `setcc` (the ICmpSgt-side materialization).
    bool foundCmp = false, foundSetcc = false;
    auto const cmpOp   = *sch.opcodeByMnemonic("cmp");
    auto const setccOp = *sch.opcodeByMnemonic("setcc");
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        auto const o = lir.instOpcode(lir.blockInstAt(entry, i));
        if (o == cmpOp)   foundCmp   = true;
        if (o == setccOp) foundSetcc = true;
    }
    EXPECT_TRUE(foundCmp)   << "ICmp/CondBr must emit at least one cmp";
    EXPECT_TRUE(foundSetcc) << "ICmpSgt must materialize a bool via setcc";
}

TEST(MirToLir, SignedICmpVariantsLowerWithCorrectSetccPayload) {
    // C-subset's `int` is signed, so the surface-visible comparison ops
    // (`==`/`!=`/`<`/`<=`/`>`/`>=`) lower to the signed conditions only.
    // Each test source feeds the comparison through `if (...)` so the
    // setcc is emitted (CondBr re-fetches via cmp+0; the setcc isn't the
    // immediate predecessor of the jcc — but it MUST appear in the entry
    // block carrying the right condition). Unsigned variants need a
    // synthetic-MIR helper (deferred to cycle 3c).
    struct Case { char const* op; ::dss::TargetCondCode cond; };
    std::array<Case, 6> cases{{
        {"==", ::dss::TargetCondCode::Eq},
        {"!=", ::dss::TargetCondCode::Ne},
        {"<",  ::dss::TargetCondCode::Slt},
        {"<=", ::dss::TargetCondCode::Sle},
        {">",  ::dss::TargetCondCode::Sgt},
        {">=", ::dss::TargetCondCode::Sge},
    }};
    auto const setccOp = []() {
        auto sch = ::dss::TargetSchema::loadShipped("x86_64");
        return *(*sch)->opcodeByMnemonic("setcc");
    }();
    for (auto const& [op, expectedCond] : cases) {
        std::string src = std::string{"int f(int a, int b) { if (a "} +
                          op + " b) return 1; return 0; }";
        auto L = lowerCSubsetToLir(src);
        assertUpstreamClean(L);
        ASSERT_TRUE(L.lir.ok) << "ICmp `" << op << "` must lower cleanly";
        // Find the entry-block setcc and read its payload — pins the
        // `condCodeForICmp` mapping. A regression mapping (say)
        // ICmpEq → Sle would silently pass without this check.
        Lir const& lir = L.lir.lir;
        LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
        bool foundCorrectSetcc = false;
        for (std::uint32_t k = 0; k < lir.blockInstCount(entry); ++k) {
            LirInstId const inst = lir.blockInstAt(entry, k);
            if (lir.instOpcode(inst) != setccOp) continue;
            EXPECT_EQ(lir.instPayload(inst),
                      static_cast<std::uint32_t>(expectedCond))
                << "setcc payload for `" << op << "` must be "
                << ::dss::targetCondCodeName(expectedCond);
            foundCorrectSetcc = true;
            break;
        }
        EXPECT_TRUE(foundCorrectSetcc)
            << "ICmp `" << op << "` must emit a setcc in the entry block";
    }
}

TEST(MirToLir, CondBrFusesIcmpConditionIntoJccPayload) {
    // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1, 2026-06-03):
    // when CondBr's operand is produced by an ICmp, the lowering
    // FUSES the pair into a single `cmp lhs, rhs; jcc-cond` shape
    // — the jcc's payload carries the ICmp's TargetCondCode
    // directly (Sgt here for `x > 0`), NOT the default cmp-against-
    // zero + Ne pattern (which would read setcc's garbage upper
    // bits and trip the branch wrong-direction).
    //
    // The non-fusable arm (cond from a non-ICmp source) keeps the
    // existing cmp-against-0 + jcc-Ne path; covered by the
    // CondBrJccPayloadIsNeForNonIcmpCond test below.
    auto L = lowerCSubsetToLir(
        "int sign(int x) { if (x > 0) return 1; return 0; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);
    auto const& sch = *L.target;
    Lir const& lir = L.lir.lir;
    LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    LirInstId const term = lir.blockTerminator(entry);
    ASSERT_EQ(lir.instOpcode(term), *sch.opcodeByMnemonic("jcc"));
    EXPECT_EQ(lir.instPayload(term),
              static_cast<std::uint32_t>(::dss::TargetCondCode::Sgt))
        << "CondBr-fused jcc payload must be Sgt (the ICmpSgt's "
           "cond code), NOT the legacy default Ne — D-CSUBSET-"
           "WHILE-LOOP-SUBSTRATE fusion pin";
}

TEST(MirToLir, TernaryProducesPhiResolutionMoves) {
    // c-subset's `?:` lowers to a MIR Phi at the join block (per
    // hir_to_mir.cpp). The cycle-3b phi resolution must emit `mov` at
    // each predecessor BEFORE its terminator, writing the per-arm value
    // into the phi's pre-allocated vreg.
    //
    // Note: the condition path may use MIR Cast (HR's implicit bool
    // coercion), which cycle 3b does NOT yet lower — so `L.lir.ok` may
    // be false. The Phi resolution itself runs INDEPENDENTLY of the
    // condition path's Cast failure (pre-allocation + edge-mov emission
    // happen in their own pass). The test pins the move shape, not the
    // overall lowering success.
    auto L = lowerCSubsetToLir(
        "int f(int c) { return c ? 1 : 2; }");
    assertUpstreamClean(L);
    // Don't require L.lir.ok — Cast on the condition may surface
    // L_UnsupportedLoweringForOpcode without preventing Phi resolution.

    auto const& sch = *L.target;
    auto const movOp = *sch.opcodeByMnemonic("mov");
    Lir const& lir = L.lir.lir;
    LirFuncId const fn = lir.funcAt(0);

    // Phi resolution via parallel-copy-with-temps emits TWO movs per
    // predecessor edge: `mov tmp, src` then `mov phi_reg, tmp`. With
    // two predecessor arms in a ternary, the minimum mov-bearing blocks
    // is 2 (the two arms), and the total mov count is ≥ 4 (2 movs per
    // arm for Phi resolution + the Const-materialization mov per arm).
    int movBearingBlocks = 0;
    int totalMovs = 0;
    for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
        LirBlockId const bb = lir.funcBlockAt(fn, b);
        bool blockHasMov = false;
        for (std::uint32_t k = 0; k < lir.blockInstCount(bb); ++k) {
            if (lir.instOpcode(lir.blockInstAt(bb, k)) == movOp) {
                blockHasMov = true;
                ++totalMovs;
            }
        }
        if (blockHasMov) ++movBearingBlocks;
    }
    EXPECT_GE(movBearingBlocks, 2)
        << "Phi resolution must emit `mov` instructions in ≥2 blocks "
           "(the two predecessor arms of the ternary)";
    // Total movs ≥ 2 (parallel-copy temps for both arms; the const
    // materialization movs may be absorbed by Const fold or merged).
    EXPECT_GE(totalMovs, 2)
        << "parallel-copy phi resolution must emit ≥2 mov instructions "
           "(one per pre/post temp move per arm)";
}

TEST(MirToLir, SwitchLowersToCascadingCompares) {
    auto L = lowerCSubsetToLir(
        "int f(int x) {\n"
        "  switch (x) {\n"
        "    case 1: return 10;\n"
        "    case 2: return 20;\n"
        "    default: return 0;\n"
        "  }\n"
        "}\n");
    assertUpstreamClean(L);
    // Switch lowering uses Alloca/Load/Store for the discriminant only
    // when c-subset's semantic phase actually materializes one; for a
    // raw `switch (x)` over a param the MIR may or may not have a
    // store-then-load. Either way the cycle 3b lowerer must produce the
    // cascading compares. `ok` may be false if the discriminant path
    // hits memory ops (cycle 3c) — assertion is structural.
    auto const& sch = *L.target;
    auto const cmpOp = *sch.opcodeByMnemonic("cmp");
    auto const jccOp = *sch.opcodeByMnemonic("jcc");
    auto const jmpOp = *sch.opcodeByMnemonic("jmp");
    Lir const& lir = L.lir.lir;
    LirFuncId const fn = lir.funcAt(0);
    // Count cmp+jcc pairs sealing blocks (the cascading-compare shape).
    int cmpJccPairs = 0;
    int jmpTerminators = 0;
    for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
        LirBlockId const bb = lir.funcBlockAt(fn, b);
        LirInstId const term = lir.blockTerminator(bb);
        if (lir.instOpcode(term) == jccOp) {
            // Walk backwards to find an adjacent cmp.
            std::uint32_t const n = lir.blockInstCount(bb);
            if (n >= 2 && lir.instOpcode(lir.blockInstAt(bb, n - 2)) == cmpOp) {
                ++cmpJccPairs;
            }
        }
        if (lir.instOpcode(term) == jmpOp) ++jmpTerminators;
    }
    EXPECT_GE(cmpJccPairs, 2)
        << "switch with 2 cases must emit ≥2 cmp+jcc pairs (one per case)";
    EXPECT_GE(jmpTerminators, 1)
        << "switch must emit a `jmp default` terminator in its tail block";
}

// ─── cycle 3c vertical slice: memory ops + cast + wide literals ─────────

TEST(MirToLir, LocalVariableLowersAllocaLoadStore) {
    // `int f() { int x = 42; return x; }` — exercises the cycle-3c
    // memory triad: Alloca + Store + Load + Return. The function uses
    // ALL three new memory opcodes plus the existing cycle-3a/3b
    // mov/ret. After cycle 3c this fully lowers.
    auto L = lowerCSubsetToLir(
        "int f() { int x = 42; return x; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok)
        << "cycle-3c memory ops must lower a local-variable round-trip cleanly";

    auto const& sch = *L.target;
    Lir const& lir = L.lir.lir;
    LirFuncId const fn = lir.funcAt(0);
    LirBlockId const entry = lir.funcEntry(fn);
    // Walk the entry block and verify alloca + store + load all emitted.
    bool foundAlloca = false, foundStore = false, foundLoad = false;
    auto const allocaOp = *sch.opcodeByMnemonic("alloca");
    auto const storeOp  = *sch.opcodeByMnemonic("store");
    auto const loadOp   = *sch.opcodeByMnemonic("load");
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        auto const o = lir.instOpcode(lir.blockInstAt(entry, i));
        if (o == allocaOp) foundAlloca = true;
        if (o == storeOp)  foundStore  = true;
        if (o == loadOp)   foundLoad   = true;
    }
    EXPECT_TRUE(foundAlloca);
    EXPECT_TRUE(foundStore);
    EXPECT_TRUE(foundLoad);
}

TEST(MirToLir, StoreEmitsCorrectOperandShape) {
    // Pin the cycle-3c Store operand convention: [value, base, MemBase,
    // MemOffset]. A regression dropping the MemBase/MemOffset operands
    // or swapping value/base order would silently produce broken
    // addressing-mode encoding downstream.
    auto L = lowerCSubsetToLir(
        "int f() { int x = 7; return x; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);
    auto const storeOp = *L.target->opcodeByMnemonic("store");
    Lir const& lir = L.lir.lir;
    LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    bool foundStore = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        LirInstId const inst = lir.blockInstAt(entry, i);
        if (lir.instOpcode(inst) != storeOp) continue;
        foundStore = true;
        auto const ops = lir.instOperands(inst);
        ASSERT_EQ(ops.size(), 4u);
        EXPECT_EQ(ops[0].kind, LirOperandKind::Reg);        // value
        EXPECT_EQ(ops[1].kind, LirOperandKind::Reg);        // base
        EXPECT_EQ(ops[2].kind, LirOperandKind::MemBase);    // scale
        EXPECT_EQ(ops[3].kind, LirOperandKind::MemOffset);  // displacement
        break;
    }
    EXPECT_TRUE(foundStore);
}

TEST(MirToLir, AllocaResultIsAddressableViaStore) {
    // Cycle-3c Alloca lowering: the LIR alloca's result register flows
    // into the immediately following Store as its `base` operand. This
    // pins the (Alloca result → Store base) wiring; a regression that
    // dropped the result-register propagation would surface here.
    //
    // Payload semantics on Alloca are ML6/ML7-driven (the size and
    // alignment encoding co-designs with frame-layout in cycle 3d);
    // cycle 3c is the pass-through layer.
    auto L = lowerCSubsetToLir(
        "int f() { int x; x = 1; return x; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);
    auto const allocaOp = *L.target->opcodeByMnemonic("alloca");
    auto const storeOp  = *L.target->opcodeByMnemonic("store");
    Lir const& lir = L.lir.lir;
    LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    LirReg allocaResult{};
    bool sawAlloca = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        LirInstId const inst = lir.blockInstAt(entry, i);
        if (lir.instOpcode(inst) == allocaOp) {
            allocaResult = lir.instResult(inst);
            sawAlloca = true;
            EXPECT_TRUE(allocaResult.valid())
                << "Alloca must produce a valid result vreg";
        }
        if (lir.instOpcode(inst) == storeOp && sawAlloca) {
            auto const ops = lir.instOperands(inst);
            ASSERT_EQ(ops.size(), 4u);
            // ops[1] is the base register the Store writes through.
            EXPECT_EQ(ops[1].kind, LirOperandKind::Reg);
            EXPECT_EQ(ops[1].reg, allocaResult)
                << "Store's base operand must reference Alloca's result";
            break;
        }
    }
    EXPECT_TRUE(sawAlloca);
}

TEST(MirToLir, WideLiteralRoutesThroughLiteralPool) {
    // Cycle-3c wide-literal path: int64 values outside int32 range
    // route through the LirLiteralPool. The mov inst's operand carries
    // kind=LiteralIndex pointing at the pool entry.
    //
    // c-subset doesn't naturally produce wide MIR Const (semantics
    // rejects int literals > INT32_MAX), so we build MIR directly via
    // the synthetic-MIR helper. This pins the wide-literal cycle-3c
    // gap the cycle-3a/3b tests couldn't reach end-to-end.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;

    // Build a synthetic MIR module: one function with one block
    // containing a Const(int64_max) + Return.
    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const i64    = interner.primitive(::dss::TypeKind::I64);
    auto const voidT  = interner.primitive(::dss::TypeKind::Void);
    auto const fnSig  = interner.fnSig(std::span<::dss::TypeId const>{}, i64, ::dss::CallConv::CcSysV);
    (void)voidT;
    ::dss::MirBuilder mb;
    ::dss::MirLiteralValue lv;
    lv.value = static_cast<std::int64_t>(0x1234567890ABCDEF);  // > INT32_MAX
    lv.core  = ::dss::TypeKind::I64;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const constInst = mb.addConst(lv, i64);
    mb.addReturn(constInst);
    ::dss::Mir m = std::move(mb).finish();

    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, sch, interner, rep);
    ASSERT_TRUE(result.ok)
        << "wide-literal MIR Const must lower cleanly via the LIR literal pool";

    // Find the mov; its operand kind must be LiteralIndex (not ImmInt).
    Lir const& lir = result.lir;
    LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    bool foundLitMov = false;
    auto const movOp = *sch.opcodeByMnemonic("mov");
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        LirInstId const inst = lir.blockInstAt(entry, i);
        if (lir.instOpcode(inst) != movOp) continue;
        auto const ops = lir.instOperands(inst);
        if (ops.size() != 1) continue;
        if (ops[0].kind == LirOperandKind::LiteralIndex) {
            foundLitMov = true;
            // The pool entry must round-trip the original int64.
            auto const& lirLit = lir.literalValue(ops[0].litIndex);
            auto const* asI64 = std::get_if<std::int64_t>(&lirLit.value);
            ASSERT_NE(asI64, nullptr);
            EXPECT_EQ(*asI64, 0x1234567890ABCDEF);
            break;
        }
    }
    EXPECT_TRUE(foundLitMov)
        << "wide-literal mov must use LirOperandKind::LiteralIndex (not ImmInt)";
    EXPECT_EQ(lir.literalPool().size(), 1u);
}

// ─── cycle 3d: bitwise + float arithmetic + cross-class Bitcast ──────────
//
// `SyntheticFn` / `buildSyntheticFn` were promoted to `synthetic_fn.hpp`
// (ML6 cycle 1, cycle-3e deferral D-3e.7) so the new
// `test_lir_liveness` binary can share the same harness. The shared
// namespace is `dss::test_support` (not `dss::testing` — gtest already
// owns the `::testing` namespace and `using namespace dss;` would
// otherwise make `testing::` ambiguous in the test files).
using ::dss::test_support::SyntheticFn;
using ::dss::test_support::buildSyntheticFn;

TEST(MirToLir, IntegerBitwiseAndShiftLowerToBitwiseOpcodes) {
    // Cycle 3d bitwise lowering: each MIR bitwise/shift op → its named
    // LIR opcode. Synthetic MIR (2 params + bitwise op + return).
    struct Case { ::dss::MirOpcode mir; char const* mnem; };
    std::array<Case, 6> cases{{
        {::dss::MirOpcode::And,  "and"},
        {::dss::MirOpcode::Or,   "or"},
        {::dss::MirOpcode::Xor,  "xor"},
        {::dss::MirOpcode::Shl,  "shl"},
        {::dss::MirOpcode::LShr, "shr_l"},
        {::dss::MirOpcode::AShr, "shr_a"},
    }};
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;
    for (auto const& c : cases) {
        std::array<::dss::TypeKind, 2> paramKinds{::dss::TypeKind::I32, ::dss::TypeKind::I32};
        auto syn = buildSyntheticFn(paramKinds, ::dss::TypeKind::I32,
            [&](::dss::MirBuilder& mb, ::dss::TypeInterner& itn,
                std::vector<::dss::TypeId> const& params, ::dss::TypeId retT) {
                ::dss::MirInstId const a = mb.addArg(0, params[0]);
                ::dss::MirInstId const b = mb.addArg(1, params[1]);
                std::array<::dss::MirInstId, 2> ops{a, b};
                ::dss::MirInstId const op = mb.addInst(c.mir, ops, retT);
                mb.addReturn(op);
                (void)itn;
            });
        ::dss::DiagnosticReporter rep;
        auto const result = ::dss::lowerToLir(syn.mir, sch, syn.interner, rep);
        ASSERT_TRUE(result.ok) << "bitwise " << c.mnem << " must lower cleanly";
        auto const expectedOp = *sch.opcodeByMnemonic(c.mnem);
        ::dss::Lir const& lir = result.lir;
        LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
        bool found = false;
        for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
            if (lir.instOpcode(lir.blockInstAt(entry, i)) == expectedOp) {
                found = true;
                EXPECT_EQ(lir.instResult(lir.blockInstAt(entry, i)).regClass(),
                          LirRegClass::GPR)
                    << "bitwise " << c.mnem << " must produce GPR-class result";
                break;
            }
        }
        EXPECT_TRUE(found) << "missing opcode `" << c.mnem << "`";
    }
}

TEST(MirToLir, IntegerNotAndNegLowerToUnaryOpcodes) {
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;
    struct Case { ::dss::MirOpcode mir; char const* mnem; };
    std::array<Case, 2> cases{{
        {::dss::MirOpcode::Not, "not"},
        {::dss::MirOpcode::Neg, "neg"},
    }};
    for (auto const& c : cases) {
        std::array<::dss::TypeKind, 1> paramKinds{::dss::TypeKind::I32};
        auto syn = buildSyntheticFn(paramKinds, ::dss::TypeKind::I32,
            [&](::dss::MirBuilder& mb, ::dss::TypeInterner&,
                std::vector<::dss::TypeId> const& params, ::dss::TypeId retT) {
                ::dss::MirInstId const a = mb.addArg(0, params[0]);
                std::array<::dss::MirInstId, 1> ops{a};
                mb.addReturn(mb.addInst(c.mir, ops, retT));
            });
        ::dss::DiagnosticReporter rep;
        auto const result = ::dss::lowerToLir(syn.mir, sch, syn.interner, rep);
        ASSERT_TRUE(result.ok) << c.mnem;
        ::dss::Lir const& lir = result.lir;
        LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
        bool found = false;
        for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
            if (lir.instOpcode(lir.blockInstAt(entry, i))
                == *sch.opcodeByMnemonic(c.mnem)) { found = true; break; }
        }
        EXPECT_TRUE(found);
    }
}

TEST(MirToLir, FloatArithmeticLowersToFPRClassResults) {
    // Float arithmetic must produce FPR-class result vregs (cycle 3d's
    // load-bearing claim — the LirRegClass dispatch hinges on
    // `regClassForType` returning FPR for F64). Pin both the opcode
    // mnemonic and the result reg class.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;
    struct Case { ::dss::MirOpcode mir; char const* mnem; std::size_t arity; };
    std::array<Case, 5> cases{{
        {::dss::MirOpcode::FAdd, "fadd", 2},
        {::dss::MirOpcode::FSub, "fsub", 2},
        {::dss::MirOpcode::FMul, "fmul", 2},
        {::dss::MirOpcode::FDiv, "fdiv", 2},
        {::dss::MirOpcode::FNeg, "fneg", 1},
    }};
    for (auto const& c : cases) {
        std::vector<::dss::TypeKind> paramKinds(c.arity, ::dss::TypeKind::F64);
        auto syn = buildSyntheticFn(paramKinds, ::dss::TypeKind::F64,
            [&](::dss::MirBuilder& mb, ::dss::TypeInterner&,
                std::vector<::dss::TypeId> const& params, ::dss::TypeId retT) {
                std::vector<::dss::MirInstId> args;
                for (std::size_t i = 0; i < c.arity; ++i) args.push_back(mb.addArg(static_cast<std::uint32_t>(i), params[i]));
                ::dss::MirInstId const op = mb.addInst(c.mir, args, retT);
                mb.addReturn(op);
            });
        ::dss::DiagnosticReporter rep;
        auto const result = ::dss::lowerToLir(syn.mir, sch, syn.interner, rep);
        ASSERT_TRUE(result.ok) << c.mnem;
        auto const expectedOp = *sch.opcodeByMnemonic(c.mnem);
        ::dss::Lir const& lir = result.lir;
        LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
        bool foundFprResult = false;
        for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
            LirInstId const inst = lir.blockInstAt(entry, i);
            if (lir.instOpcode(inst) == expectedOp) {
                EXPECT_EQ(lir.instResult(inst).regClass(), LirRegClass::FPR)
                    << "float arithmetic `" << c.mnem
                    << "` must produce an FPR-class result";
                foundFprResult = true;
                break;
            }
        }
        EXPECT_TRUE(foundFprResult);
    }
}

TEST(MirToLir, BitcastCrossClassEmitsMovqXClass) {
    // The cycle-3c-anchored cross-class Bitcast hazard: F64 → I64 must
    // emit `movq_xclass` (not plain `mov`) because the source register
    // class differs from the destination class. The cycle-3c lowering
    // unconditionally emitted `mov` regardless of class — silently
    // wrong for cross-class. Cycle 3d closes this via the regClassFor
    // check in `lowerBitcast`.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;
    std::array<::dss::TypeKind, 1> paramKinds{::dss::TypeKind::F64};
    auto syn = buildSyntheticFn(paramKinds, ::dss::TypeKind::I64,
        [&](::dss::MirBuilder& mb, ::dss::TypeInterner&,
            std::vector<::dss::TypeId> const& params, ::dss::TypeId retT) {
            ::dss::MirInstId const a = mb.addArg(0, params[0]);
            std::array<::dss::MirInstId, 1> ops{a};
            mb.addReturn(mb.addInst(::dss::MirOpcode::Bitcast, ops, retT));
        });
    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(syn.mir, sch, syn.interner, rep);
    ASSERT_TRUE(result.ok);
    auto const movqOp = *sch.opcodeByMnemonic("movq_xclass");
    ::dss::Lir const& lir = result.lir;
    LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    bool foundXClass = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        if (lir.instOpcode(lir.blockInstAt(entry, i)) == movqOp) {
            foundXClass = true; break;
        }
    }
    EXPECT_TRUE(foundXClass)
        << "FPR→GPR Bitcast must emit `movq_xclass`, not plain `mov`";
}

TEST(MirToLir, BitcastCrossClassEmitsMovqXClassReverse) {
    // Reverse direction of `BitcastCrossClassEmitsMovqXClass`
    // (cycle-3e deferral D-3e.8 folded ML6 cycle 1). The lowerer's
    // class-symmetric check at `lowerBitcast` is direction-agnostic
    // — same operand-shape exercises the I64→F64 path with the
    // source class flipped to GPR and the destination class flipped
    // to FPR.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;
    std::array<::dss::TypeKind, 1> paramKinds{::dss::TypeKind::I64};
    auto syn = buildSyntheticFn(paramKinds, ::dss::TypeKind::F64,
        [&](::dss::MirBuilder& mb, ::dss::TypeInterner&,
            std::vector<::dss::TypeId> const& params, ::dss::TypeId retT) {
            ::dss::MirInstId const a = mb.addArg(0, params[0]);
            std::array<::dss::MirInstId, 1> ops{a};
            mb.addReturn(mb.addInst(::dss::MirOpcode::Bitcast, ops, retT));
        });
    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(syn.mir, sch, syn.interner, rep);
    ASSERT_TRUE(result.ok);
    auto const movqOp = *sch.opcodeByMnemonic("movq_xclass");
    ::dss::Lir const& lir = result.lir;
    LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    bool foundXClass = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        if (lir.instOpcode(lir.blockInstAt(entry, i)) == movqOp) {
            foundXClass = true; break;
        }
    }
    EXPECT_TRUE(foundXClass)
        << "GPR→FPR Bitcast must emit `movq_xclass`, not plain `mov`";
}

TEST(MirToLir, BitcastSameClassStaysAsMov) {
    // Positive control: I64 → Ptr (both GPR-class) emits `mov`, not
    // `movq_xclass`. Pins the class-symmetry branch.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;
    std::array<::dss::TypeKind, 1> paramKinds{::dss::TypeKind::I64};
    auto syn = buildSyntheticFn(paramKinds, ::dss::TypeKind::Ptr,
        [&](::dss::MirBuilder& mb, ::dss::TypeInterner&,
            std::vector<::dss::TypeId> const& params, ::dss::TypeId retT) {
            ::dss::MirInstId const a = mb.addArg(0, params[0]);
            std::array<::dss::MirInstId, 1> ops{a};
            mb.addReturn(mb.addInst(::dss::MirOpcode::Bitcast, ops, retT));
        });
    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(syn.mir, sch, syn.interner, rep);
    ASSERT_TRUE(result.ok);
    auto const movqOp = *sch.opcodeByMnemonic("movq_xclass");
    ::dss::Lir const& lir = result.lir;
    LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        EXPECT_NE(lir.instOpcode(lir.blockInstAt(entry, i)), movqOp)
            << "same-class Bitcast must use `mov`, not `movq_xclass`";
    }
}

// ─── cycle 3c review fold-in: cast variant → opcode mapping ─────────────
//
// Cycle-3c review pr-test-analyzer (rating 8): cast lowering had zero
// positive-path coverage — every MIR cast variant could regress its
// mnemonic mapping silently. Each variant is exercised via synthetic
// MIR + opcode-mnemonic assertion below.

namespace {
struct CastCase {
    ::dss::MirOpcode  mirOp;
    char const*       expectedMnemonic;
    ::dss::TypeKind   srcKind;
    ::dss::TypeKind   dstKind;
    LirRegClass       expectedResultClass;
};
}

class MirToLirCastMapping : public ::testing::TestWithParam<CastCase> {};

TEST_P(MirToLirCastMapping, EmitsExpectedMnemonicAndRegClass) {
    auto const param = GetParam();
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;

    // Synthetic MIR: single src-typed arg → cast → return dst-typed value.
    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const srcT = interner.primitive(param.srcKind);
    auto const dstT = interner.primitive(param.dstKind);
    std::array<::dss::TypeId, 1> params{srcT};
    auto const fnSig = interner.fnSig(params, dstT, ::dss::CallConv::CcSysV);

    ::dss::MirBuilder mb;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const argInst = mb.addArg(0, srcT);
    std::array<::dss::MirInstId, 1> castOps{argInst};
    ::dss::MirInstId const castInst = mb.addInst(param.mirOp, castOps, dstT);
    mb.addReturn(castInst);
    ::dss::Mir m = std::move(mb).finish();

    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, sch, interner, rep);
    ASSERT_TRUE(result.ok)
        << "cast (MirOpcode " << static_cast<int>(param.mirOp) << ")"
        << " must lower cleanly via mnemonic `" << param.expectedMnemonic << "`";

    ::dss::Lir const& lir = result.lir;
    LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    auto const expectedOp = *sch.opcodeByMnemonic(param.expectedMnemonic);
    bool foundCast = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        LirInstId const inst = lir.blockInstAt(entry, i);
        if (lir.instOpcode(inst) == expectedOp) {
            foundCast = true;
            EXPECT_EQ(lir.instResult(inst).regClass(), param.expectedResultClass)
                << "cast (MirOpcode " << static_cast<int>(param.mirOp)
                << ") result reg class mismatch";
            break;
        }
    }
    EXPECT_TRUE(foundCast)
        << "cast (MirOpcode " << static_cast<int>(param.mirOp) << ")"
        << " must emit LIR opcode `" << param.expectedMnemonic << "`";
}

INSTANTIATE_TEST_SUITE_P(
    AllCastVariants, MirToLirCastMapping,
    ::testing::Values(
        // Integer casts (cycle 3c) — all GPR result.
        CastCase{::dss::MirOpcode::Trunc,    "trunc", ::dss::TypeKind::I64, ::dss::TypeKind::I32, LirRegClass::GPR},
        CastCase{::dss::MirOpcode::SExt,     "sext",  ::dss::TypeKind::I32, ::dss::TypeKind::I64, LirRegClass::GPR},
        CastCase{::dss::MirOpcode::ZExt,     "zext",  ::dss::TypeKind::I32, ::dss::TypeKind::I64, LirRegClass::GPR},
        CastCase{::dss::MirOpcode::IntToPtr, "mov",   ::dss::TypeKind::I64, ::dss::TypeKind::Ptr, LirRegClass::GPR},
        CastCase{::dss::MirOpcode::PtrToInt, "mov",   ::dss::TypeKind::Ptr, ::dss::TypeKind::I64, LirRegClass::GPR},
        // Bitcast same-class is mov; different test (BitcastCrossClass)
        // pins the cross-class movq_xclass path.
        CastCase{::dss::MirOpcode::Bitcast,  "mov",   ::dss::TypeKind::I64, ::dss::TypeKind::Ptr, LirRegClass::GPR},
        // Cycle 3d float casts. fpcvt handles BOTH FPTrunc + FPExt.
        // FPToSI/FPToUI: float → integer, result is GPR.
        // SIToFP/UIToFP: integer → float, result is FPR.
        // FPTrunc/FPExt: float → float, result is FPR.
        CastCase{::dss::MirOpcode::FPTrunc,  "fpcvt",    ::dss::TypeKind::F64, ::dss::TypeKind::F32, LirRegClass::FPR},
        CastCase{::dss::MirOpcode::FPExt,    "fpcvt",    ::dss::TypeKind::F32, ::dss::TypeKind::F64, LirRegClass::FPR},
        CastCase{::dss::MirOpcode::FPToSI,   "fp_to_si", ::dss::TypeKind::F64, ::dss::TypeKind::I64, LirRegClass::GPR},
        CastCase{::dss::MirOpcode::FPToUI,   "fp_to_ui", ::dss::TypeKind::F64, ::dss::TypeKind::I64, LirRegClass::GPR},
        CastCase{::dss::MirOpcode::SIToFP,   "si_to_fp", ::dss::TypeKind::I64, ::dss::TypeKind::F64, LirRegClass::FPR},
        CastCase{::dss::MirOpcode::UIToFP,   "ui_to_fp", ::dss::TypeKind::I64, ::dss::TypeKind::F64, LirRegClass::FPR}
    ));

TEST(MirToLir, GepDynamicIndexEmitsFourOperandLea) {
    // Cycle 3d added a 2-operand Gep case emitting
    // `lea result, [base + index*1 + 0]` via the 4-operand operand
    // tuple [base_reg, index_reg, MemBase(scale=1), MemOffset(disp=0)].
    // Pin the operand kinds + count so a regression to a different
    // shape surfaces here, not at AS1 encoding time.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;

    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const ptrT = interner.primitive(::dss::TypeKind::Ptr);
    auto const i64  = interner.primitive(::dss::TypeKind::I64);
    std::array<::dss::TypeId, 2> params{ptrT, i64};
    auto const fnSig = interner.fnSig(params, ptrT, ::dss::CallConv::CcSysV);

    ::dss::MirBuilder mb;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const base  = mb.addArg(0, ptrT);
    ::dss::MirInstId const index = mb.addArg(1, i64);
    std::array<::dss::MirInstId, 2> gepOps{base, index};
    ::dss::MirInstId const gepInst = mb.addInst(::dss::MirOpcode::Gep, gepOps, ptrT);
    mb.addReturn(gepInst);
    ::dss::Mir m = std::move(mb).finish();

    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, sch, interner, rep);
    ASSERT_TRUE(result.ok) << "dynamic-index Gep must lower cleanly";

    auto const leaOp = *sch.opcodeByMnemonic("lea");
    ::dss::Lir const& lir = result.lir;
    LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    bool foundLea = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        LirInstId const inst = lir.blockInstAt(entry, i);
        if (lir.instOpcode(inst) != leaOp) continue;
        foundLea = true;
        auto const ops = lir.instOperands(inst);
        ASSERT_EQ(ops.size(), 4u);
        EXPECT_EQ(ops[0].kind, LirOperandKind::Reg);        // base
        EXPECT_EQ(ops[1].kind, LirOperandKind::Reg);        // index
        EXPECT_EQ(ops[2].kind, LirOperandKind::MemBase);    // scale
        EXPECT_EQ(ops[3].kind, LirOperandKind::MemOffset);  // disp
        EXPECT_EQ(ops[2].scale, 1u);
        EXPECT_EQ(ops[3].offset, 0);
        break;
    }
    EXPECT_TRUE(foundLea);
}

TEST(MirToLir, PhiResolutionUsesFprClassForFloatPhi) {
    // Cycle-3d review (code-reviewer H2 + type-design + test-analyzer
    // rating 9): prepassAllocatePhis previously hardcoded GPR for ALL
    // phi results, silently mis-classing F64 phis. emitPhiMovesForEdge
    // similarly hardcoded GPR for the parallel-copy temps.
    //
    // This test pins both: an F64-typed Phi MUST produce an FPR-class
    // result vreg, and its parallel-copy temps MUST also be FPR-class.
    // A regression to GPR would fail both assertions immediately rather
    // than silently propagating to AS1's wrong-class register encoding.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;

    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const f64  = interner.primitive(::dss::TypeKind::F64);
    auto const boolT = interner.primitive(::dss::TypeKind::Bool);
    std::array<::dss::TypeId, 1> params{boolT};
    auto const fnSig = interner.fnSig(params, f64, ::dss::CallConv::CcSysV);

    // Build: int param `c` → if (c) return 1.0 else 2.0 (diamond CFG
    // with a Phi at the join). The Phi is F64-typed; the lowering must
    // place it in FPR-class.
    ::dss::MirBuilder mb;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const entry = mb.createBlock(::dss::StructCfMarker::EntryBlock);
    ::dss::MirBlockId const thenB = mb.createBlock(::dss::StructCfMarker::IfThen);
    ::dss::MirBlockId const elseB = mb.createBlock(::dss::StructCfMarker::IfElse);
    ::dss::MirBlockId const join  = mb.createBlock(::dss::StructCfMarker::IfJoin);
    mb.beginBlock(entry);
    ::dss::MirInstId const cond = mb.addArg(0, boolT);
    mb.addCondBr(cond, thenB, elseB);
    mb.beginBlock(thenB);
    ::dss::MirLiteralValue lvOne;  lvOne.value  = 1.0;  lvOne.core  = ::dss::TypeKind::F64;
    ::dss::MirInstId const constOne = mb.addConst(lvOne, f64);
    mb.addBr(join);
    mb.beginBlock(elseB);
    ::dss::MirLiteralValue lvTwo;  lvTwo.value  = 2.0;  lvTwo.core  = ::dss::TypeKind::F64;
    ::dss::MirInstId const constTwo = mb.addConst(lvTwo, f64);
    mb.addBr(join);
    mb.beginBlock(join);
    ::dss::MirInstId const phi = mb.addPhi(f64);
    mb.addPhiIncoming(phi, ::dss::MirPhiIncoming{constOne, thenB});
    mb.addPhiIncoming(phi, ::dss::MirPhiIncoming{constTwo, elseB});
    mb.addReturn(phi);
    ::dss::Mir m = std::move(mb).finish();

    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, sch, interner, rep);
    ASSERT_TRUE(result.ok) << "FPR-typed phi must lower cleanly";

    ::dss::Lir const& lir = result.lir;
    auto const retOp = *sch.opcodeByMnemonic("ret");

    // The Return's operand must be a register, and that register must
    // be FPR-class (the phi's pre-allocated vreg, now FPR per the fix).
    LirFuncId const fn = lir.funcAt(0);
    for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
        LirBlockId const bb = lir.funcBlockAt(fn, b);
        LirInstId const term = lir.blockTerminator(bb);
        if (lir.instOpcode(term) != retOp) continue;
        auto const ops = lir.instOperands(term);
        ASSERT_EQ(ops.size(), 1u);
        EXPECT_EQ(ops[0].kind, LirOperandKind::Reg);
        EXPECT_EQ(ops[0].reg.regClass(), LirRegClass::FPR)
            << "F64 phi result must be FPR-class — regression to GPR "
               "would silently mis-class downstream consumers";
        return;
    }
    ADD_FAILURE() << "no ret block found";
}

TEST(MirToLir, WideLiteralStringRoutesThroughLiteralPool) {
    // Parallel to WideLiteralRoutesThroughLiteralPool but for the string
    // variant (rating 7 from pr-test-analyzer). Closes the cycle-3c
    // float/string-untested gap; double defers to cycle 3d's FPR class.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;

    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const ptrT  = interner.primitive(::dss::TypeKind::Ptr);
    auto const fnSig = interner.fnSig(std::span<::dss::TypeId const>{}, ptrT, ::dss::CallConv::CcSysV);
    ::dss::MirBuilder mb;
    ::dss::MirLiteralValue lv;
    lv.value = std::string{"hello world"};
    lv.core  = ::dss::TypeKind::Ptr;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const constInst = mb.addConst(lv, ptrT);
    mb.addReturn(constInst);
    ::dss::Mir m = std::move(mb).finish();

    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, sch, interner, rep);
    ASSERT_TRUE(result.ok);
    ::dss::Lir const& lir = result.lir;
    ASSERT_EQ(lir.literalPool().size(), 1u);
    auto const& lirLit = lir.literalValue(0);
    auto const* asStr  = std::get_if<std::string>(&lirLit.value);
    ASSERT_NE(asStr, nullptr);
    EXPECT_EQ(*asStr, "hello world");
}

TEST(MirToLir, WideLiteralDoubleRoutesThroughFprLiteralPool) {
    // Cycle 3d enabled double-literal lowering: F64 MIR Const flows
    // through the LirLiteralPool with an FPR-class result register
    // (the cycle-3c stub previously fail-loud-deferred this case
    // because no FPR-class machinery existed). Pin the result is FPR.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;

    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const f64   = interner.primitive(::dss::TypeKind::F64);
    auto const fnSig = interner.fnSig(std::span<::dss::TypeId const>{}, f64, ::dss::CallConv::CcSysV);
    ::dss::MirBuilder mb;
    ::dss::MirLiteralValue lv;
    lv.value = 3.14;
    lv.core  = ::dss::TypeKind::F64;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const constInst = mb.addConst(lv, f64);
    mb.addReturn(constInst);
    ::dss::Mir m = std::move(mb).finish();

    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, sch, interner, rep);
    ASSERT_TRUE(result.ok)
        << "F64 literal must lower cleanly via FPR-class + LirLiteralPool";

    // The pool must contain the double, and the mov's result must be
    // FPR-class (regClassForType maps F64 → FPR).
    ::dss::Lir const& lir = result.lir;
    ASSERT_EQ(lir.literalPool().size(), 1u);
    auto const* asDbl = std::get_if<double>(&lir.literalValue(0).value);
    ASSERT_NE(asDbl, nullptr);
    EXPECT_DOUBLE_EQ(*asDbl, 3.14);

    auto const movOp = *sch.opcodeByMnemonic("mov");
    LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    bool foundFprMov = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        LirInstId const inst = lir.blockInstAt(entry, i);
        if (lir.instOpcode(inst) != movOp) continue;
        auto const ops = lir.instOperands(inst);
        if (ops.size() == 1 && ops[0].kind == LirOperandKind::LiteralIndex) {
            EXPECT_EQ(lir.instResult(inst).regClass(), LirRegClass::FPR)
                << "double literal must materialize into an FPR-class vreg";
            foundFprMov = true;
            break;
        }
    }
    EXPECT_TRUE(foundFprMov);
}

// ─── cycle 3e: Calls + Aggregates + LirVerifier ─────────────────────────

TEST(MirToLir, DirectCallEmitsCallOpcode) {
    // Cycle-3e Call lowering: GlobalAddr → mov(symbolRef); Call(callee,
    // args...) → call(callee_reg, arg_regs...). c-subset's `g() { f(); }`
    // emits this MIR shape.
    auto L = lowerCSubsetToLir(
        "int f(int x) { return x; }\n"
        "int g(int y) { return f(y); }\n");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok)
        << "direct call must lower cleanly in cycle 3e";

    auto const& sch = *L.target;
    Lir const& lir = L.lir.lir;
    // Both functions exist.
    EXPECT_EQ(lir.moduleFuncCount(), 2u);

    auto const leaOp  = *sch.opcodeByMnemonic("lea");
    auto const callOp = *sch.opcodeByMnemonic("call");
    // D-LK4-RODATA-PRODUCER (2026-06-02): GlobalAddr now lowers to
    // `lea result, SymbolRef` (RIP-relative form) instead of the
    // prior `mov result, SymbolRef`. The lea encoding has a real
    // 1-operand variant on the assembler side; the prior `mov`
    // shape tripped `A_NoMatchingEncodingVariant` at assemble time
    // for any non-call-peepholed use of a GlobalAddr.
    // The 2nd function `g` must contain:
    //   - lea result, symbolRef(f)   ← GlobalAddr(f)
    //   - call calleeReg, argReg     ← the actual Call
    LirFuncId const gFn = lir.funcAt(1);
    LirBlockId const entry = lir.funcEntry(gFn);
    bool foundGlobalAddrLea = false, foundCall = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        LirInstId const inst = lir.blockInstAt(entry, i);
        auto const op = lir.instOpcode(inst);
        if (op == leaOp) {
            auto const ops = lir.instOperands(inst);
            if (ops.size() == 1 && ops[0].kind == LirOperandKind::SymbolRef) {
                foundGlobalAddrLea = true;
            }
        }
        if (op == callOp) foundCall = true;
    }
    EXPECT_TRUE(foundGlobalAddrLea)
        << "GlobalAddr must emit `lea result, symbolRef(symId)` "
           "(RIP-relative form, D-LK4-RODATA-PRODUCER 2026-06-02)";
    EXPECT_TRUE(foundCall)
        << "Call must emit the `call` opcode";
}

// ── D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold opcode-selection pins ─
//
// MIR→LIR's `lowerCall` MUST pick `call_indirect_via_extern` (FF 15
// disp32 — indirect through IAT slot) when the GlobalAddr callee's
// SymbolId is in the caller-supplied `externImports` list, and `call`
// (E8 disp32 — direct rel32) otherwise. Direct call to an extern
// would execute the IAT slot's BYTES as code — guaranteed SEGV
// (the 2nd half of the hello_puts 0xC0000005 the cycle closed).
// Substrate-tier pin (test-analyzer C1 fold): without this, a
// refactor of the opcode-selection branch could regress silently
// even if the e2e hello_puts pin still passes by accident.

TEST(MirToLir, ExternCallEmitsCallIndirectViaExternOpcode) {
    // Build a tiny MIR with ONE extern import + ONE internal function
    // + ONE caller that calls BOTH. After lowerToLir(externImports):
    //   * call to extern symbol → `call_indirect_via_extern` opcode
    //   * call to internal symbol → `call` opcode
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;

    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const i32   = interner.primitive(::dss::TypeKind::I32);
    auto const ptrT  = interner.primitive(::dss::TypeKind::Ptr);
    auto const externSig = interner.fnSig(
        std::array<::dss::TypeId const, 1>{ptrT}, i32,
        ::dss::CallConv::CcMS64);
    auto const internalSig = interner.fnSig(
        std::span<::dss::TypeId const>{}, i32, ::dss::CallConv::CcMS64);
    (void)externSig;  // referenced via GlobalAddr semantic, not by signature lookup

    constexpr std::uint32_t kExternSym = 100u;
    constexpr std::uint32_t kInternalSym = 101u;
    constexpr std::uint32_t kCallerSym = 102u;

    ::dss::MirBuilder mb;

    // Internal function: just returns 0.
    mb.addFunction(internalSig, ::dss::SymbolId{kInternalSym});
    {
        auto const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
        mb.beginBlock(bb);
        ::dss::MirLiteralValue lv;
        lv.value = static_cast<std::int64_t>(0);
        lv.core  = ::dss::TypeKind::I32;
        ::dss::MirInstId const zero = mb.addConst(lv, i32);
        mb.addReturn(zero);
    }

    // Caller function: calls both extern + internal.
    mb.addFunction(internalSig, ::dss::SymbolId{kCallerSym});
    {
        auto const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
        mb.beginBlock(bb);
        ::dss::MirLiteralValue lv;
        lv.value = static_cast<std::int64_t>(0);
        lv.core  = ::dss::TypeKind::I32;
        ::dss::MirInstId const zero = mb.addConst(lv, i32);
        // Call extern: callee is GlobalAddr(externSym).
        ::dss::MirInstId const externAddr =
            mb.addGlobalAddr(::dss::SymbolId{kExternSym}, ptrT);
        std::array<::dss::MirInstId, 2> externOps{externAddr, zero};
        ::dss::MirInstId const externResult = mb.addInst(
            ::dss::MirOpcode::Call, externOps, i32);
        // Call internal: callee is GlobalAddr(internalSym).
        ::dss::MirInstId const intAddr =
            mb.addGlobalAddr(::dss::SymbolId{kInternalSym}, ptrT);
        std::array<::dss::MirInstId, 1> intOps{intAddr};
        ::dss::MirInstId const intResult = mb.addInst(
            ::dss::MirOpcode::Call, intOps, i32);
        // Return sum-or-anything to keep the values live.
        std::array<::dss::MirInstId, 2> addOps{externResult, intResult};
        ::dss::MirInstId const sum = mb.addInst(
            ::dss::MirOpcode::Add, addOps, i32);
        mb.addReturn(sum);
    }
    ::dss::Mir m = std::move(mb).finish();

    // Build the externImports list: kExternSym only.
    ::dss::ExternImport ext{};
    ext.symbol      = ::dss::SymbolId{kExternSym};
    ext.mangledName = "extern_fn";
    ext.libraryPath = "fictional.dll";
    std::vector<::dss::ExternImport> externImports{ext};

    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, sch, interner, rep,
                                          externImports);
    ASSERT_TRUE(result.ok)
        << "extern-call MIR must lower cleanly with externImports passed";
    EXPECT_EQ(rep.errorCount(), 0u);

    auto const callOp = *sch.opcodeByMnemonic("call");
    auto const callIndirectOp =
        *sch.opcodeByMnemonic("call_indirect_via_extern");
    ASSERT_NE(callOp, callIndirectOp);

    // Caller is the 2nd function (index 1 — internal first, caller
    // second). Inspect its entry block for the two call opcodes.
    Lir const& lir = result.lir;
    ASSERT_EQ(lir.moduleFuncCount(), 2u);
    LirFuncId const callerFn = lir.funcAt(1);
    LirBlockId const entry = lir.funcEntry(callerFn);
    std::uint32_t directCalls = 0u;
    std::uint32_t externCalls = 0u;
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        LirInstId const inst = lir.blockInstAt(entry, i);
        auto const op = lir.instOpcode(inst);
        if (op == callOp)          ++directCalls;
        if (op == callIndirectOp)  ++externCalls;
    }
    // The fixture issues ONE call to internal + ONE call to extern.
    // Pin EXACTLY ONE of each — a refactor that lowers extern-as-direct
    // would produce 2 direct + 0 extern (the silent regression).
    EXPECT_EQ(directCalls, 1u)
        << "call to internal symbol must lower to `call` (E8 disp32)";
    EXPECT_EQ(externCalls, 1u)
        << "call to extern symbol must lower to `call_indirect_via_extern` "
           "(FF 15 disp32) — direct E8 would SEGV at runtime";
}

TEST(MirToLir, NoExternImportsAllCallsLowerAsDirectCall) {
    // Inverse of the above: with `externImports={}` the lowerer must
    // NOT mis-classify ANY call as extern. Every call lowers as the
    // direct `call` opcode.
    auto L = lowerCSubsetToLir(
        "int g(int a) { return a; }\n"
        "int f(int x) { return g(x); }\n");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);
    auto const& sch = *L.target;
    auto const callOp = *sch.opcodeByMnemonic("call");
    auto const callIndirectOp =
        *sch.opcodeByMnemonic("call_indirect_via_extern");

    Lir const& lir = L.lir.lir;
    std::uint32_t directCalls = 0u;
    std::uint32_t externCalls = 0u;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        std::uint32_t const bn = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < bn; ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                LirInstId const inst = lir.blockInstAt(blk, i);
                auto const op = lir.instOpcode(inst);
                if (op == callOp)         ++directCalls;
                if (op == callIndirectOp) ++externCalls;
            }
        }
    }
    EXPECT_GT(directCalls, 0u)
        << "module-internal calls must lower as `call`";
    EXPECT_EQ(externCalls, 0u)
        << "with no externImports passed, no call must lower as "
           "`call_indirect_via_extern`";
}

TEST(MirToLir, VoidCallProducesNoResultReg) {
    // A call to a void-returning function has no result vreg. Pin that
    // the LIR `call` inst's result is InvalidLirReg.
    auto L = lowerCSubsetToLir(
        "void noop() {}\n"
        "void main_() { noop(); }\n");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);
    auto const callOp = *L.target->opcodeByMnemonic("call");
    Lir const& lir = L.lir.lir;
    LirFuncId const mainFn = lir.funcAt(1);
    LirBlockId const entry = lir.funcEntry(mainFn);
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        LirInstId const inst = lir.blockInstAt(entry, i);
        if (lir.instOpcode(inst) != callOp) continue;
        EXPECT_FALSE(lir.instResult(inst).valid())
            << "void-returning Call must have InvalidLirReg result";
        return;
    }
    ADD_FAILURE() << "no call inst found";
}

TEST(LirVerifier, AcceptsCleanCSubsetPipelines) {
    // Smoke test: every c-subset corpus example that lowers cleanly
    // through cycles 3a-3e must also pass the LirVerifier without
    // any new diagnostics. This is the regression-lock for the
    // "vreg-class-vs-MIR-type consistency" rule the cycle-3d review
    // surfaced — a future regression to the cycle-3d FPR-class
    // plumbing fixes would now fail the verifier even if the unit
    // tests didn't catch it.
    auto L = lowerCSubsetToLir(
        "int add(int a, int b) { return a + b; }\n"
        "int sign(int x) { if (x > 0) return 1; return 0; }\n");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);

    ::dss::DiagnosticReporter rep;
    auto const r = ::dss::verifyLir(L.lir.lir, L.mir.mir,
                                    L.model.lattice().interner(),
                                    *L.target, L.lir.lirToMir, rep);
    EXPECT_TRUE(r.ok)
        << "LirVerifier must accept a clean cycle-3a-3d c-subset pipeline";
}

// ─── cycle 3e fix-up: aggregate ops + IntrinsicCall + verifier negatives ──

TEST(MirToLir, ExtractValueZeroIndexLowersToLoad) {
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;

    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const ptrT = interner.primitive(::dss::TypeKind::Ptr);
    auto const i32  = interner.primitive(::dss::TypeKind::I32);
    std::array<::dss::TypeId, 1> params{ptrT};
    auto const fnSig = interner.fnSig(params, i32, ::dss::CallConv::CcSysV);

    ::dss::MirBuilder mb;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const agg = mb.addArg(0, ptrT);
    ::dss::MirLiteralValue zero;  zero.value = static_cast<std::int64_t>(0);
                                  zero.core  = ::dss::TypeKind::I32;
    ::dss::MirInstId const idx = mb.addConst(zero, i32);
    std::array<::dss::MirInstId, 2> ops{agg, idx};
    ::dss::MirInstId const ext = mb.addInst(::dss::MirOpcode::ExtractValue, ops, i32);
    mb.addReturn(ext);
    ::dss::Mir m = std::move(mb).finish();

    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, sch, interner, rep);
    ASSERT_TRUE(result.ok)
        << "ExtractValue with zero-index Const must lower cleanly";
    auto const loadOp = *sch.opcodeByMnemonic("load");
    ::dss::Lir const& lir = result.lir;
    LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    bool foundLoad = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        if (lir.instOpcode(lir.blockInstAt(entry, i)) == loadOp) {
            foundLoad = true; break;
        }
    }
    EXPECT_TRUE(foundLoad)
        << "zero-index ExtractValue must emit `load`";
}

TEST(MirToLir, ExtractValueNonZeroIndexDefersWithDiagnostic) {
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;

    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const ptrT = interner.primitive(::dss::TypeKind::Ptr);
    auto const i32  = interner.primitive(::dss::TypeKind::I32);
    std::array<::dss::TypeId, 1> params{ptrT};
    auto const fnSig = interner.fnSig(params, i32, ::dss::CallConv::CcSysV);
    ::dss::MirBuilder mb;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const agg = mb.addArg(0, ptrT);
    // Non-zero index — cycle 3e MUST fail loud, deferring to ML6
    // frame-layout for type-driven field offsets.
    ::dss::MirLiteralValue one;  one.value = static_cast<std::int64_t>(1);
                                 one.core  = ::dss::TypeKind::I32;
    ::dss::MirInstId const idx = mb.addConst(one, i32);
    std::array<::dss::MirInstId, 2> ops{agg, idx};
    ::dss::MirInstId const ext = mb.addInst(::dss::MirOpcode::ExtractValue, ops, i32);
    mb.addReturn(ext);
    ::dss::Mir m = std::move(mb).finish();

    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, sch, interner, rep);
    EXPECT_FALSE(result.ok)
        << "non-zero index ExtractValue must defer with L_Unsupported";
    bool found = false;
    for (auto const& d : rep.all()) {
        if (d.code == ::dss::DiagnosticCode::L_UnsupportedLoweringForOpcode) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(MirToLir, ExtractValueZeroIndexAcceptsUintLiteralVariant) {
    // Cycle 3e review (silent-failure + type-design): the cycle-3e
    // first cut only accepted `int64_t` zero — `uint64_t 0` (legal MIR
    // text round-trip output) silently fell through to fail-loud. The
    // `isZeroIntegerLiteral` helper fixes this.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;
    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const ptrT = interner.primitive(::dss::TypeKind::Ptr);
    auto const i32  = interner.primitive(::dss::TypeKind::I32);
    std::array<::dss::TypeId, 1> params{ptrT};
    auto const fnSig = interner.fnSig(params, i32, ::dss::CallConv::CcSysV);
    ::dss::MirBuilder mb;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const agg = mb.addArg(0, ptrT);
    // uint64_t variant of zero — MUST now be accepted (cycle-3e fix-up).
    ::dss::MirLiteralValue uzero;  uzero.value = static_cast<std::uint64_t>(0);
                                   uzero.core  = ::dss::TypeKind::U32;
    ::dss::MirInstId const idx = mb.addConst(uzero, i32);
    std::array<::dss::MirInstId, 2> ops{agg, idx};
    ::dss::MirInstId const ext = mb.addInst(::dss::MirOpcode::ExtractValue, ops, i32);
    mb.addReturn(ext);
    ::dss::Mir m = std::move(mb).finish();
    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, sch, interner, rep);
    EXPECT_TRUE(result.ok)
        << "uint64_t-0 index must be accepted as zero (was silently rejected "
           "in cycle 3e first cut)";
}

TEST(MirToLir, IntrinsicCallLowersToIntrinsicCallOpcode) {
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;

    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const i32  = interner.primitive(::dss::TypeKind::I32);
    std::array<::dss::TypeId, 1> params{i32};
    auto const fnSig = interner.fnSig(params, i32, ::dss::CallConv::CcSysV);
    ::dss::MirBuilder mb;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const arg = mb.addArg(0, i32);
    std::array<::dss::MirInstId, 1> ops{arg};
    ::dss::MirInstId const intr = mb.addInst(::dss::MirOpcode::IntrinsicCall, ops, i32, /*payload=*/42);
    mb.addReturn(intr);
    ::dss::Mir m = std::move(mb).finish();

    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, sch, interner, rep);
    ASSERT_TRUE(result.ok);
    auto const intrOp = *sch.opcodeByMnemonic("intrinsic_call");
    ::dss::Lir const& lir = result.lir;
    LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    bool found = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        LirInstId const inst = lir.blockInstAt(entry, i);
        if (lir.instOpcode(inst) == intrOp) {
            found = true;
            EXPECT_EQ(lir.instPayload(inst), 42u)
                << "IntrinsicCall payload must carry the intrinsic id";
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(LirVerifier, FiresOnSwitchBearingFunctionsAfterMapPlumbing) {
    // Cycle-3e review: the cycle-3e FIRST CUT walked MIR vs LIR
    // POSITIONALLY by block index. cycle-3b Switch lowering creates
    // extra LIR blocks (per-case "next-compare" blocks), causing the
    // verifier to silently bail out on `funcBlockCount` mismatch —
    // architect-flagged HIGH (silent-failure-hunter rated CRITICAL).
    //
    // The fix-up plumbs a `lirToMir` mapping through MirToLirResult;
    // the verifier walks LIR insts and uses the mapping per-inst.
    // This test pins: even a switch-bearing c-subset function passes
    // the verifier WITHOUT being silently skipped.
    auto L = lowerCSubsetToLir(
        "int f(int x) {\n"
        "  switch (x) {\n"
        "    case 1: return 10;\n"
        "    case 2: return 20;\n"
        "    default: return 0;\n"
        "  }\n"
        "}\n");
    assertUpstreamClean(L);
    ::dss::DiagnosticReporter rep;
    auto const r = ::dss::verifyLir(L.lir.lir, L.mir.mir,
                                    L.model.lattice().interner(),
                                    *L.target, L.lir.lirToMir, rep);
    EXPECT_TRUE(r.ok)
        << "LirVerifier must run cleanly on switch-bearing functions "
           "(positional-walk silent-skip closed by lirToMir mapping)";
}

TEST(MirToLir, LinkRegisterUnknownNameRejectedNegativePath) {
    // Negative path for the cycle-3a-deferred linkRegister ordinal cache:
    // if the name does not resolve, the loader must reject the schema
    // (NOT silently produce a `nullopt` ordinal). Pins the "atomic
    // population" invariant of the new `LinkRegisterRef` struct.
    auto r = ::dss::TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"arm64"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"x0","class":"gpr","widthBytes":8}],
            "callingConventions":[
              {"name":"aapcs","argGprs":["x0"],"linkRegister":"nonexistent",
               "stackAlignment":16}
            ]})");
    ASSERT_FALSE(r.has_value())
        << "linkRegister naming an undeclared register must reject the schema";
    bool found = false;
    for (auto const& d : r.error()) {
        if (d.code == ::dss::DiagnosticCode::C_MalformedJson) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(MirToLir, WhileLoopLowersWithBackEdge) {
    // While loop produces a header block with a Phi (for the loop-carried
    // value when used) + a back-edge from the latch. Phi resolution must
    // insert a `mov` at the latch BEFORE its jmp back to the header.
    //
    // c-subset model: `while (i < n) { i = i + 1; }` lowers (via ML2's
    // alloca-backed locals model) to header-cmp + body-add + back-edge.
    // The latch's terminator is a jmp; Phi resolution emits `mov` before
    // it. Cycle 3a's alloca-backed model means there may not be a literal
    // MIR Phi here (the loop carries via Load/Store), but the CFG with
    // back-edge must still produce a valid LIR with a jmp terminator on
    // the latch.
    auto L = lowerCSubsetToLir(
        "int sum(int n) {\n"
        "  int s = 0;\n"
        "  while (s < n) s = s + 1;\n"
        "  return s;\n"
        "}\n");
    assertUpstreamClean(L);
    // The body uses Alloca/Load/Store (cycle 3c) for `s` — fail-loud
    // expected, but the CFG topology + jcc terminator must still emit.
    // Pin only the terminator-shape invariant.
    Lir const& lir = L.lir.lir;
    ASSERT_EQ(lir.moduleFuncCount(), 1u);
    LirFuncId const fn = lir.funcAt(0);
    // Every block must end in a terminator (substrate guarantees this; the
    // assertion catches a refactor that drops the fallback seal).
    for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
        LirBlockId const bb = lir.funcBlockAt(fn, b);
        EXPECT_TRUE(L.target->isTerminator(lir.instOpcode(lir.blockTerminator(bb))));
    }
}
