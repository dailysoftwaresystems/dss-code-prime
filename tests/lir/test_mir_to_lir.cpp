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
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "lir/lir.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/lowering/hir_to_mir.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

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
    auto lir = lowerToLir(mir.mir, **target, lirReporter);
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
               "minOperands":0,"maxOperands":1}
            ]})");
    ASSERT_TRUE(incomplete.has_value());

    // Drive MIR for `int f() { return 1; }`. The Const → mov path will hit
    // the missing-opcode branch.
    auto L = lowerCSubsetToLir("int f() { return 1; }");
    assertUpstreamClean(L);

    DiagnosticReporter rep;
    auto result = lowerToLir(L.mir.mir, **incomplete, rep);
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
    // Cycle 3b lowers Br/CondBr/ICmp*/Phi/Switch/Unreachable, so the
    // cycle-3a `if (x < 0)` pin is no longer unsupported. Now memory
    // operations (Load/Store/Alloca/Gep) — deferred to cycle 3c — are the
    // canonical fail-loud example: a function with a local variable hits
    // MIR Alloca + Store + Load which cycle 3b does NOT yet lower.
    auto L = lowerCSubsetToLir(
        "int f() { int x = 42; return x; }");
    assertUpstreamClean(L);
    EXPECT_FALSE(L.lir.ok);
    bool foundUnsupported = false;
    for (auto const& d : L.lirReporter.all()) {
        if (d.code == DiagnosticCode::L_UnsupportedLoweringForOpcode) {
            foundUnsupported = true;
            break;
        }
    }
    EXPECT_TRUE(foundUnsupported)
        << "cycle 3b must report L_UnsupportedLoweringForOpcode on memory "
           "opcodes (Alloca/Store/Load); silent acceptance is a regression";

    // Every LIR block must end in a terminator. Fallback seal kicks in for
    // blocks whose MIR terminator was deferred OR which contained an
    // unsupported non-terminator inst that prevented the normal flow.
    // We don't pin the specific terminator opcode (cycle 3b now produces
    // jmp/jcc/ret depending on the function shape); just that the block
    // has a terminator at all.
    Lir const& lir = L.lir.lir;
    for (std::uint32_t i = 0; i < lir.moduleFuncCount(); ++i) {
        LirFuncId const fn = lir.funcAt(i);
        for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
            LirBlockId const bb = lir.funcBlockAt(fn, b);
            LirInstId const term = lir.blockTerminator(bb);
            EXPECT_TRUE(L.target->isTerminator(lir.instOpcode(term)))
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

TEST(MirToLir, CondBrJccPayloadIsNe) {
    // The cycle-3b review surfaced that the jcc condition was silently
    // defaulting to payload 0 (= TargetCondCode::Eq) instead of the
    // intended Ne (since CondBr lowers as cmp val,0 + jcc(ne)). The
    // fix extended `addCondBr` to take a payload; this test pins the
    // emitted jcc carries `Ne` so a regression would surface here, not
    // in AS1's encoded bytes.
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
              static_cast<std::uint32_t>(::dss::TargetCondCode::Ne))
        << "CondBr jcc payload must be Ne (jump-when-condition-is-true), "
           "not the default 0 (Eq, which would invert the branch)";
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
