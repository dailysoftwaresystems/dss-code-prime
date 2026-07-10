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
#include "link/object_format_schema.hpp"  // TLS C1: the shipped-format tlsAccess reject pins
#include "lir/lir.hpp"
#include "lir/lir_verifier.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_literal_pool.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mutate_target_schema.hpp"
#include "synthetic_fn.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <variant>
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

// D-CSUBSET-DIVISION-OP-CODEGEN (cycle 10r split, 2026-06-04): signed
// divide. c-subset has only signed int/long → `/` lowers via
// HirOpKind::Div → MirOpcode::SDiv → LIR MnemonicSlot::{SDivPre,
// SDivCore} → the x86 `cqo` + `idiv_op` opcodes (REX.W 0x99 CQO
// sign-extends RAX into RDX:RAX; REX.W 0xF7 /7 IDIV divides
// RDX:RAX by the modrm.rm operand; byte-pinned in
// test_asm_x86_variable.cpp). The Div lowering emits 4 LIR ops:
//   (1) `mov rax_phys, dividend_vreg`   pin dividend into RAX
//   (2) `cqo`                            sign-extend (no result)
//   (3) `idiv_op divisor_vreg`           the divide (no SSA result)
//   (4) `mov result_vreg, rax_phys`      capture quotient
// **Cycle 10r split rationale**: cycle 10q packaged CQO+IDIV into
// a single compound opcode but the encoder's auto-REX prefix was
// overridden by the embedded second 0x48, losing REX.B for
// high-reg divisors → silent miscompile + STATUS_INTEGER_DIVIDE_BY_ZERO.
TEST(MirToLir, SignedDivisionLowersToCqoPlusIDiv) {
    auto L = lowerCSubsetToLir("int q(int a, int b) { return a / b; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);

    Lir const& lir = L.lir.lir;
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    // Expected sequence:
    //   0..1: args
    //   N:   mov rax_phys, a_vreg  (pin dividend)
    //   N+1: cqo                   (sign-extend, no operands)
    //   N+2: idiv_op b_vreg        (the divide, no SSA result)
    //   N+3: mov result, rax_phys  (capture quotient)
    auto const cqoOp     = L.target->opcodeByMnemonic("cqo");
    auto const idivOp    = L.target->opcodeByMnemonic("idiv_op");
    auto const movOp     = L.target->opcodeByMnemonic("mov");
    ASSERT_TRUE(cqoOp.has_value());
    ASSERT_TRUE(idivOp.has_value());
    ASSERT_TRUE(movOp.has_value());
    // Locate the idiv_op inst by opcode scan (position-resilient).
    std::uint32_t idivIdx = lir.blockInstCount(bb);
    for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
        if (lir.instOpcode(lir.blockInstAt(bb, i)) == *idivOp) {
            idivIdx = i;
            break;
        }
    }
    ASSERT_LT(idivIdx, lir.blockInstCount(bb))
        << "MIR SDiv must lower to LIR idiv_op — opcode scan failed.";
    // idiv_op must be preceded by `cqo` (sign-extend), and that by
    // `mov rax, dividend`, and followed by `mov result, rax`.
    ASSERT_GE(idivIdx, 2u);
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, idivIdx - 1)), *cqoOp)
        << "idiv_op must be preceded by `cqo` (REX.W 0x99 sign-extend).";
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, idivIdx - 2)), *movOp)
        << "cqo must be preceded by `mov rax, dividend`.";
    ASSERT_LT(idivIdx + 1, lir.blockInstCount(bb));
    LirInstId const divCapture = lir.blockInstAt(bb, idivIdx + 1);
    EXPECT_EQ(lir.instOpcode(divCapture), *movOp)
        << "idiv_op must be followed by `mov result, rax`.";
    // FC1 (2026-06-10) role-contract symmetry: SDiv must capture the
    // QUOTIENT register (rax, role 'quotient') — the mirror of the
    // SMod rdx assert below. Before this assert, a quotient↔remainder
    // role flip in the JSON left this test green (only the SMod pin
    // went red); now BOTH directions of the flip are pinned.
    {
        auto const ops = L.lir.lir.instOperands(divCapture);
        ASSERT_EQ(ops.size(), 1u);
        ASSERT_EQ(ops[0].kind, LirOperandKind::Reg);
        ASSERT_TRUE(ops[0].reg.isPhysical);
        auto const rax = L.target->registerByName("rax");
        ASSERT_TRUE(rax.has_value());
        EXPECT_EQ(ops[0].reg.id, static_cast<std::uint32_t>(*rax))
            << "SDiv must capture rax (role 'quotient'), never rdx.";
    }
}

// FLAG 1 discrimination test (10r, 2026-06-04): a hand-built MIR
// with a UDiv inst MUST lower to the udiv pre+core slots
// (xor_rdx_zero + div_op = XOR EDX,EDX zero-extend + DIV /6) and
// NOT to the sdiv pre+core slots (cqo + idiv_op = CQO sign-extend
// + IDIV /7). c-subset has no unsigned source today, so this test
// uses a hand-built MIR fixture to exercise the UDiv arm directly.
// Routing UDiv through SDivCore would pass any high-bit-set
// dividend with the wrong sign interpretation (silent miscompile).
TEST(MirToLir, UnsignedDivisionLowersToXorPlusDivNotCqoIDiv) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32 = interner.primitive(TypeKind::I32);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    MirInstId const b = mb.addArg(1, i32);
    MirInstId const divOps[] = {a, b};
    MirInstId const q = mb.addInst(MirOpcode::UDiv, divOps, i32);
    mb.addReturn(q);
    Mir mir = std::move(mb).finish();

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    std::vector<dss::ExternImport> noExterns;
    auto lirR = lowerToLir(mir, **target, interner, rep, noExterns);
    ASSERT_TRUE(lirR.ok);
    Lir const& lir = lirR.lir;

    auto const xorRdxOp = (*target)->opcodeByMnemonic("xor_rdx_zero");
    auto const divOp    = (*target)->opcodeByMnemonic("div_op");
    auto const cqoOp    = (*target)->opcodeByMnemonic("cqo");
    auto const idivOp   = (*target)->opcodeByMnemonic("idiv_op");
    ASSERT_TRUE(xorRdxOp.has_value());
    ASSERT_TRUE(divOp.has_value());
    ASSERT_TRUE(cqoOp.has_value());
    ASSERT_TRUE(idivOp.has_value());
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    bool foundXorRdx = false;
    bool foundDiv    = false;
    bool foundCqo    = false;
    bool foundIdiv   = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
        auto const op = lir.instOpcode(lir.blockInstAt(bb, i));
        if (op == *xorRdxOp) foundXorRdx = true;
        if (op == *divOp)    foundDiv    = true;
        if (op == *cqoOp)    foundCqo    = true;
        if (op == *idivOp)   foundIdiv   = true;
    }
    EXPECT_TRUE(foundXorRdx)
        << "MIR UDiv must emit `xor_rdx_zero` (XOR EDX, EDX) to "
           "zero-extend RAX→RDX:RAX. Missing pre-op would leave RDX "
           "with stale bits → wrong quotient.";
    EXPECT_TRUE(foundDiv)
        << "MIR UDiv must lower to `div_op` (REX.W 0xF7 /6). Missing "
           "core would silently no-op the divide.";
    EXPECT_FALSE(foundCqo)
        << "MIR UDiv MUST NOT emit `cqo` — that sign-extends and is "
           "used only by SDiv. FLAG 1 silent-miscompile guard: any "
           "dividend with the high bit set would interpret as negative.";
    EXPECT_FALSE(foundIdiv)
        << "MIR UDiv MUST NOT emit `idiv_op` — would silently "
           "mis-sign-interpret dividends ≥ INT_MAX (CRITICAL "
           "discriminator for FLAG 1 of cycle 10q's silent-"
           "miscompile guard, preserved through 10r split).";
}

// c117 (D-LK-EXTERN-DATA-IMPORT): a GOT-indirect extern-DATA GlobalAddr
// materializes the OBJECT's address by lea-of-__got-slot + a DEREF load (the
// __got slot holds the dyld-bound object address), and the GlobalAddr→Load
// riprel fold is SUPPRESSED so a C-level load stays a distinct SECOND
// indirection. A bare lea would yield the __got slot ADDRESS where the object
// VALUE was wanted — off by one indirection, a silent miscompile. Always-on
// structural guard for the macho stdout/stderr codegen (runtime witness =
// the `stdio_stream_objects` macho arm). RED-ON-DISABLE: drop the
// externDataGotSymbols_ membership (bare lea) → 1 memory access; keep the
// fold (not suppressed) → the pair folds to ONE riprel load, 0 MemBase.
TEST(MirToLir, GotIndirectExternDataGlobalAddrEmitsLeaThenDeref) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i8         = interner.primitive(TypeKind::I8);
    TypeId const filePtr    = interner.pointer(i8);       // FILE* stand-in
    TypeId const filePtrPtr = interner.pointer(filePtr);  // &stdout : FILE**
    TypeId const params[]   = {i8};                        // one ignored param
    TypeId const fnSig      = interner.fnSig(params, filePtr, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    (void)mb.addArg(0, i8);
    // `return stdout;` — stdout (a data extern) as rvalue = Load(GlobalAddr).
    SymbolId const dataSym{200};
    MirInstId const ga        = mb.addGlobalAddr(dataSym, filePtrPtr);  // &stdout
    MirInstId const loadArgs[] = {ga};
    MirInstId const val       = mb.addInst(MirOpcode::Load, loadArgs, filePtr);
    mb.addReturn(val);
    Mir mir = std::move(mb).finish();

    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    std::vector<dss::ExternImport> externs;
    dss::ExternImport ei;
    ei.symbol      = dataSym;
    ei.mangledName = "___stdoutp";
    ei.libraryPath = "/usr/lib/libSystem.B.dylib";
    ei.isData      = true;
    externs.push_back(ei);
    auto lirR = lowerToLir(mir, **target, interner, rep, externs,
                           ExternCallDispatch::DirectPlt,
                           DataImportBinding::GotIndirect);
    ASSERT_TRUE(lirR.ok);
    Lir const& lir = lirR.lir;
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    bool sawGotSlotSymbol = false;  // an inst carrying SymbolRef(dataSym)
    int  memAccesses      = 0;      // insts with a MemBase operand (base-reg loads)
    for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
        auto const ops = lir.instOperands(lir.blockInstAt(bb, i));
        bool hasSym = false, hasMem = false;
        for (auto const& op : ops) {
            if (op.kind == LirOperandKind::SymbolRef && op.symbolV == dataSym.v) {
                hasSym = true;
            }
            if (op.kind == LirOperandKind::MemBase) hasMem = true;
        }
        if (hasSym) sawGotSlotSymbol = true;
        if (hasMem) ++memAccesses;
    }
    EXPECT_TRUE(sawGotSlotSymbol)
        << "the __got-slot lea must carry a SymbolRef to the data extern.";
    EXPECT_GE(memAccesses, 2)
        << "a got-indirect data extern needs the __got DEREF load (the object "
           "address) BEFORE the C-level load (the object value) — two base-reg "
           "memory accesses; a bare lea gives 1, a folded riprel load gives 0.";
}

// ─── FC1 (V2-4.X, 2026-06-10): SMod/UMod lowering + the role contract ──────

namespace {

// Hand-built single-function MIR `fn(i32, i32) -> i32 { return OP(a, b); }`
// — the UDiv-test pattern (c-subset has no unsigned / the arm64 target has
// no c-subset front-end dependency here, so hand-built MIR exercises the
// lowering arm directly on any target schema).
[[nodiscard]] Mir buildBinFnMir(MirOpcode op, TypeInterner& interner) {
    TypeId const i32 = interner.primitive(TypeKind::I32);
    TypeId const params[] = {i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, i32);
    MirInstId const b = mb.addArg(1, i32);
    MirInstId const ops[] = {a, b};
    MirInstId const r = mb.addInst(op, ops, i32);
    mb.addReturn(r);
    return std::move(mb).finish();
}

// The physical register an inst's single Reg operand names, or nullopt
// (used to pin WHICH implicit output the capture mov reads).
[[nodiscard]] std::optional<std::uint32_t>
singlePhysRegOperand(Lir const& lir, LirInstId inst) {
    auto const ops = lir.instOperands(inst);
    if (ops.size() != 1 || ops[0].kind != LirOperandKind::Reg) {
        return std::nullopt;
    }
    if (!ops[0].reg.isPhysical) return std::nullopt;
    return ops[0].reg.id;
}

}  // namespace

// D-CSUBSET-MOD-OP-CODEGEN closure: `%` lowers through the SAME
// cqo+idiv_op pair as `/` but captures the REMAINDER (RDX, via the
// `outputRoles` role "remainder") instead of the quotient (RAX). The
// capture-register assertion is the OUTPUT-INDEX-CONTRACT's teeth: a
// quotient/remainder flip (the silent-miscompile class the role map
// kills) makes this test red.
TEST(MirToLir, SignedModuloLowersToCqoIdivWithRemainderCapture) {
    auto L = lowerCSubsetToLir("int m(int a, int b) { return a % b; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok)
        << "LIR lowering: " << (L.lirReporter.all().empty()
            ? "" : L.lirReporter.all()[0].actual);

    Lir const& lir = L.lir.lir;
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    auto const cqoOp  = L.target->opcodeByMnemonic("cqo");
    auto const idivOp = L.target->opcodeByMnemonic("idiv_op");
    auto const movOp  = L.target->opcodeByMnemonic("mov");
    ASSERT_TRUE(cqoOp.has_value());
    ASSERT_TRUE(idivOp.has_value());
    ASSERT_TRUE(movOp.has_value());
    std::uint32_t idivIdx = lir.blockInstCount(bb);
    for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
        if (lir.instOpcode(lir.blockInstAt(bb, i)) == *idivOp) {
            idivIdx = i;
            break;
        }
    }
    ASSERT_LT(idivIdx, lir.blockInstCount(bb))
        << "MIR SMod must lower through LIR idiv_op (the x86 remainder "
           "lives in the SAME divide instruction as the quotient).";
    ASSERT_GE(idivIdx, 2u);
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, idivIdx - 1)), *cqoOp)
        << "SMod's idiv_op must be preceded by `cqo` (signed pair — "
           "FLAG 1: never xor_rdx_zero).";
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, idivIdx - 2)), *movOp);
    ASSERT_LT(idivIdx + 1, lir.blockInstCount(bb));
    LirInstId const capture = lir.blockInstAt(bb, idivIdx + 1);
    EXPECT_EQ(lir.instOpcode(capture), *movOp)
        << "idiv_op must be followed by the remainder-capture mov.";

    auto const rdx = L.target->registerByName("rdx");
    auto const rax = L.target->registerByName("rax");
    ASSERT_TRUE(rdx.has_value());
    ASSERT_TRUE(rax.has_value());
    auto const captureSrc = singlePhysRegOperand(lir, capture);
    ASSERT_TRUE(captureSrc.has_value())
        << "the capture mov must read a PHYSICAL register operand.";
    EXPECT_EQ(*captureSrc, static_cast<std::uint32_t>(*rdx))
        << "SMod must capture the REMAINDER register (rdx, role "
           "'remainder') — capturing rax would silently return the "
           "quotient (the exact miscompile class "
           "D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-CONTRACT names).";
    EXPECT_NE(*captureSrc, static_cast<std::uint32_t>(*rax))
        << "SMod capture must NOT read rax (the quotient).";
}

// FLAG-1 mirror for the unsigned remainder (hand-built MIR — unsigned
// is source-unreachable until FC3 lands the width/signedness types):
// UMod routes through xor_rdx_zero + div_op, NEVER cqo/idiv_op, and
// captures RDX.
TEST(MirToLir, UnsignedModuloLowersToXorDivWithRemainderCaptureNotCqoIdiv) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildBinFnMir(MirOpcode::UMod, interner);

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    std::vector<dss::ExternImport> noExterns;
    auto lirR = lowerToLir(mir, **target, interner, rep, noExterns);
    ASSERT_TRUE(lirR.ok);
    Lir const& lir = lirR.lir;

    auto const xorRdxOp = (*target)->opcodeByMnemonic("xor_rdx_zero");
    auto const divOp    = (*target)->opcodeByMnemonic("div_op");
    auto const cqoOp    = (*target)->opcodeByMnemonic("cqo");
    auto const idivOp   = (*target)->opcodeByMnemonic("idiv_op");
    auto const movOp    = (*target)->opcodeByMnemonic("mov");
    ASSERT_TRUE(xorRdxOp.has_value() && divOp.has_value()
                && cqoOp.has_value() && idivOp.has_value()
                && movOp.has_value());
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    std::uint32_t divIdx = lir.blockInstCount(bb);
    bool foundCqo = false;
    bool foundIdiv = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
        auto const op = lir.instOpcode(lir.blockInstAt(bb, i));
        if (op == *divOp)  divIdx = i;
        if (op == *cqoOp)  foundCqo = true;
        if (op == *idivOp) foundIdiv = true;
    }
    ASSERT_LT(divIdx, lir.blockInstCount(bb))
        << "MIR UMod must lower through `div_op`.";
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, divIdx - 1)), *xorRdxOp)
        << "UMod's div_op must be preceded by xor_rdx_zero (zero-extend).";
    EXPECT_FALSE(foundCqo)
        << "UMod MUST NOT emit `cqo` — FLAG 1 sign-interpretation guard.";
    EXPECT_FALSE(foundIdiv)
        << "UMod MUST NOT emit `idiv_op` — FLAG 1 guard.";
    ASSERT_LT(divIdx + 1, lir.blockInstCount(bb));
    LirInstId const capture = lir.blockInstAt(bb, divIdx + 1);
    auto const rdx = (*target)->registerByName("rdx");
    ASSERT_TRUE(rdx.has_value());
    auto const captureSrc = singlePhysRegOperand(lir, capture);
    ASSERT_TRUE(captureSrc.has_value());
    EXPECT_EQ(*captureSrc, static_cast<std::uint32_t>(*rdx))
        << "UMod must capture rdx (role 'remainder').";
}

// D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC closure, Rule 1: a target that
// declares a NATIVE result-bearing `sdiv` (arm64 SDIV Xd,Xn,Xm) gets
// exactly ONE LIR op for MIR SDiv — no mov-pin, no pre-op, no
// implicit-register machinery. The SAME lowering arm that emits the
// x86 4-op pair shape selects this by capability probing (no arch
// identity anywhere).
TEST(MirToLir, Arm64SignedDivisionLowersToSingleNativeSdiv) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildBinFnMir(MirOpcode::SDiv, interner);

    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    std::vector<dss::ExternImport> noExterns;
    auto lirR = lowerToLir(mir, **target, interner, rep, noExterns);
    ASSERT_TRUE(lirR.ok)
        << (rep.all().empty() ? "" : rep.all()[0].actual);
    Lir const& lir = lirR.lir;

    auto const sdivOp = (*target)->opcodeByMnemonic("sdiv");
    auto const argOp  = (*target)->opcodeByMnemonic("arg");
    ASSERT_TRUE(sdivOp.has_value());
    ASSERT_TRUE(argOp.has_value());
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    // EXACT shape: arg, arg, sdiv, ret — 4 LIR insts, no extra movs.
    ASSERT_EQ(lir.blockInstCount(bb), 4u)
        << "arm64 SDiv must be ONE native op (arg, arg, sdiv, ret) — "
           "extra instructions mean the x86 implicit-pair shape leaked "
           "onto a native-divide target.";
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 0)), *argOp);
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 1)), *argOp);
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 2)), *sdivOp);
}

// Rule 3 (the generic remainder expansion): arm64 has no hardware
// remainder, so MIR SMod expands to the target-blind arithmetic
// identity rem = n − (n/d)·d over the DECLARED verbs: sdiv, mul, sub
// — exactly three value-bearing ops, in that order.
// D-LIR-MOD-MSUB-FUSION (FC3.5 sweep-c3): arm64 SMod now realizes the
// rem = n − (n/d)·d identity via the FUSED msub (rule 3 preference a)
// — sdiv + msub = 2 compute ops, the shape production compilers emit.
// RED-on-disable lever: disabling the msub preference (or stripping
// the `msub` declaration — see the fallback pin below) returns the
// 3-op mul+sub expansion → the count + opcode asserts here go RED.
TEST(MirToLir, Arm64SignedModuloFusesToSdivMsub) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildBinFnMir(MirOpcode::SMod, interner);

    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    std::vector<dss::ExternImport> noExterns;
    auto lirR = lowerToLir(mir, **target, interner, rep, noExterns);
    ASSERT_TRUE(lirR.ok)
        << (rep.all().empty() ? "" : rep.all()[0].actual);
    Lir const& lir = lirR.lir;

    auto const sdivOp = (*target)->opcodeByMnemonic("sdiv");
    auto const msubOp = (*target)->opcodeByMnemonic("msub");
    ASSERT_TRUE(sdivOp.has_value() && msubOp.has_value());
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    // EXACT shape: arg, arg, sdiv, msub, ret — 5 LIR insts (was 6
    // pre-fusion: arg, arg, sdiv, mul, sub, ret).
    ASSERT_EQ(lir.blockInstCount(bb), 5u)
        << "arm64 SMod must fuse to exactly sdiv+msub.";
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 2)), *sdivOp)
        << "fusion step 1: q = sdiv(n, d).";
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 3)), *msubOp)
        << "fusion step 2: r = msub(q, d, n) = n − q·d.";

    // Operand wiring (the identity's correctness): msub reads
    // (quotient, divisor, dividend) — wired rn/rm/ra so the encoded
    // MSUB computes Ra − Rn·Rm = n − q·d. Any transposition either
    // negates every remainder (n, q·d swap) or computes garbage.
    LirInstId const sdivInst = lir.blockInstAt(bb, 2);
    LirInstId const msubInst = lir.blockInstAt(bb, 3);
    auto const aReg    = lir.instResult(lir.blockInstAt(bb, 0));
    auto const bReg    = lir.instResult(lir.blockInstAt(bb, 1));
    auto const sdivReg = lir.instResult(sdivInst);
    auto const msubOps = lir.instOperands(msubInst);
    ASSERT_EQ(msubOps.size(), 3u);
    EXPECT_EQ(msubOps[0].reg, sdivReg)
        << "msub operand 0 (→ Rn) must be the sdiv quotient.";
    EXPECT_EQ(msubOps[1].reg, bReg)
        << "msub operand 1 (→ Rm) must be the divisor.";
    EXPECT_EQ(msubOps[2].reg, aReg)
        << "msub operand 2 (→ Ra) must be the DIVIDEND (r = n − q·d; "
           "a transposed Ra would negate or corrupt every remainder).";
}

// ── D-LK10-ENTRY-ARM64-WIDE-IMMEDIATE (FC3.5 sweep-c3) ──────────────
// The MOVZ+MOVK wide-immediate ladder. Hand-built `fn() -> T { return
// <const>; }` MIR drives `lowerConst`'s inline path on each target.

namespace {

// Hand-built single-function MIR `fn() -> i64 { return CONST; }`.
[[nodiscard]] Mir buildConstFnMir(std::int64_t value, TypeKind kind,
                                  TypeInterner& interner) {
    TypeId const ty = interner.primitive(kind);
    auto const fnSig = interner.fnSig(std::span<TypeId const>{}, ty,
                                      CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue lit;
    lit.value = value;
    lit.core  = kind;
    MirInstId const c = mb.addConst(lit, ty);
    mb.addReturn(c);
    return std::move(mb).finish();
}

struct ConstChain {
    std::vector<std::uint16_t> opcodes;   // block opcode sequence
    std::vector<std::int32_t>  immValues; // each inst's ImmInt operand (or skip)
    // D-CSUBSET-BITFIELD-WIDE-UNIT: each inst's LiteralIndex pool VALUE
    // (the wide int64 carried for `mov r64, imm64`), or 0 when the inst
    // has no LiteralIndex operand. Parallel-indexed with `opcodes`.
    std::vector<std::uint64_t> litValues;
};

// `constKind` is the literal's declared TypeKind (drives width); when it
// differs from the int64-carried `value`'s natural type the caller is
// exercising a specific width path. For a value that exceeds int32 the
// literal must be carried as a wide constant (the dead-end the FC8
// wide-unit work closed) — the helper builds it the same way regardless.
[[nodiscard]] ConstChain lowerConstChain(std::int64_t value, TypeKind kind,
                                         char const* targetName) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildConstFnMir(value, kind, interner);
    auto target = TargetSchema::loadShipped(targetName);
    EXPECT_TRUE(target.has_value());
    DiagnosticReporter rep;
    std::vector<dss::ExternImport> noExterns;
    auto lirR = lowerToLir(mir, **target, interner, rep, noExterns);
    EXPECT_TRUE(lirR.ok)
        << (rep.all().empty() ? "" : rep.all()[0].actual);
    ConstChain out;
    Lir const& lir = lirR.lir;
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
        LirInstId const inst = lir.blockInstAt(bb, i);
        out.opcodes.push_back(lir.instOpcode(inst));
        std::int32_t imm = 0;
        std::uint64_t litVal = 0;
        for (auto const& op : lir.instOperands(inst)) {
            if (op.kind == LirOperandKind::ImmInt) imm = op.immInt32;
            if (op.kind == LirOperandKind::LiteralIndex) {
                auto const& lit = lir.literalValue(op.litIndex);
                if (auto const* u = std::get_if<std::uint64_t>(&lit.value))
                    litVal = *u;
                else if (auto const* s =
                             std::get_if<std::int64_t>(&lit.value))
                    litVal = static_cast<std::uint64_t>(*s);
            }
        }
        out.immValues.push_back(imm);
        out.litValues.push_back(litVal);
    }
    return out;
}

}  // namespace

// 196608 = 0x30000: chunk0 = 0, chunk1 = 3 — exactly mov #0 +
// movk_lsl16 #3 (+ ret). RED-on-disable lever: with the ladder off the
// lowering emits ONE mov #196608 that fail-louds at encode; at THIS
// tier the chain shape vanishes → count/opcode asserts RED.
TEST(MirToLir, Arm64WideConstSplitsIntoMovzMovkChain) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto const movOp  = (*target)->opcodeByMnemonic("mov");
    auto const mk16   = (*target)->opcodeByMnemonic("movk_lsl16");
    ASSERT_TRUE(movOp.has_value() && mk16.has_value());

    auto const chain = lowerConstChain(196608, TypeKind::I64, "arm64");
    // mov #0, movk_lsl16 #3, ret — exactly 3 insts.
    ASSERT_EQ(chain.opcodes.size(), 3u)
        << "0x30000 must lower as a 2-op MOVZ+MOVK ladder";
    EXPECT_EQ(chain.opcodes[0], *movOp);
    EXPECT_EQ(chain.immValues[0], 0)
        << "chunk 0 (lo16) of 0x30000 is ZERO";
    EXPECT_EQ(chain.opcodes[1], *mk16);
    EXPECT_EQ(chain.immValues[1], 3)
        << "chunk 1 (bits 31:16) of 0x30000 is 3";
}

// A NEGATIVE width-64 constant is the SIGN-EXTENDED pattern — the
// full 4-op ladder (chunk0 + three 0xFFFF-filled high chunks). -2 =
// 0xFFFF_FFFF_FFFF_FFFE.
TEST(MirToLir, Arm64NegativeConstEmitsFullFourOpLadder) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto const movOp = (*target)->opcodeByMnemonic("mov");
    auto const mk16  = (*target)->opcodeByMnemonic("movk_lsl16");
    auto const mk32  = (*target)->opcodeByMnemonic("movk_lsl32");
    auto const mk48  = (*target)->opcodeByMnemonic("movk_lsl48");
    ASSERT_TRUE(movOp.has_value() && mk16.has_value()
             && mk32.has_value() && mk48.has_value());

    auto const chain = lowerConstChain(-2, TypeKind::I64, "arm64");
    ASSERT_EQ(chain.opcodes.size(), 5u)
        << "-2 must lower as the full 4-op ladder (sign-extended "
           "pattern) + ret";
    EXPECT_EQ(chain.opcodes[0], *movOp);
    EXPECT_EQ(chain.immValues[0], 0xFFFE);
    EXPECT_EQ(chain.opcodes[1], *mk16);
    EXPECT_EQ(chain.immValues[1], 0xFFFF);
    EXPECT_EQ(chain.opcodes[2], *mk32);
    EXPECT_EQ(chain.immValues[2], 0xFFFF);
    EXPECT_EQ(chain.opcodes[3], *mk48);
    EXPECT_EQ(chain.immValues[3], 0xFFFF);
}

// imm16-window constants keep the single MOVZ byte-identically — the
// ladder must NOT fire below the ceiling.
TEST(MirToLir, Arm64NarrowConstKeepsSingleMov) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto const movOp = (*target)->opcodeByMnemonic("mov");
    ASSERT_TRUE(movOp.has_value());
    auto const chain = lowerConstChain(0x2345, TypeKind::I64, "arm64");
    ASSERT_EQ(chain.opcodes.size(), 2u)
        << "0x2345 fits MOVZ — single mov + ret, no ladder";
    EXPECT_EQ(chain.opcodes[0], *movOp);
    EXPECT_EQ(chain.immValues[0], 0x2345);
}

// A width-32 (U32-typed) wide constant emits AT MOST the 2-chunk
// ladder — chunks 2/3 do not exist at 32-bit width (the MOVZ seed
// zeroes the whole register; the W-write semantics need no high
// chunks). 0xFFFFFFFF = chunks FFFF/FFFF.
TEST(MirToLir, Arm64Width32WideConstEmitsTwoChunkLadderOnly) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto const movOp = (*target)->opcodeByMnemonic("mov");
    auto const mk16  = (*target)->opcodeByMnemonic("movk_lsl16");
    ASSERT_TRUE(movOp.has_value() && mk16.has_value());
    auto const chain = lowerConstChain(
        static_cast<std::int64_t>(0xFFFFFFFFll), TypeKind::U32, "arm64");
    ASSERT_EQ(chain.opcodes.size(), 3u)
        << "U32 0xFFFFFFFF = mov #0xFFFF + movk_lsl16 #0xFFFF + ret — "
           "no chunk-2/3 movk at 32-bit width";
    EXPECT_EQ(chain.opcodes[0], *movOp);
    EXPECT_EQ(chain.immValues[0], 0xFFFF);
    EXPECT_EQ(chain.opcodes[1], *mk16);
    EXPECT_EQ(chain.immValues[1], 0xFFFF);
}

// The CAPABILITY boundary: x86_64 declares no movk family — its mov
// imm32 form swallows the whole inline range in ONE inst, and the
// ladder must never fire (probe-by-mnemonic, zero `if (arch)`).
TEST(MirToLir, X64WideConstKeepsSingleMovNoLadder) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const movOp = (*target)->opcodeByMnemonic("mov");
    ASSERT_TRUE(movOp.has_value());
    EXPECT_FALSE((*target)->opcodeByMnemonic("movk_lsl16").has_value())
        << "precondition: x86_64 must not declare the arm64 movk family";
    auto const chain = lowerConstChain(196608, TypeKind::I64, "x86_64");
    ASSERT_EQ(chain.opcodes.size(), 2u)
        << "x86 keeps the single mov r, imm32 — the ladder is a "
           "capability of movk-declaring targets only";
    EXPECT_EQ(chain.opcodes[0], *movOp);
    EXPECT_EQ(chain.immValues[0], 196608);
}

// ── D-CSUBSET-BITFIELD-WIDE-UNIT: constants > int32 (the wide-mask
// dead-end the scoping pass found) materialize by CAPABILITY ──
//
// 0xABCDEF1234 = 40-bit, exceeds INT32_MAX, so it does NOT fit the
// inline imm32 carrier — before FC8 it hit the LiteralPool dead-end (no
// encoder consumed LiteralIndex). Now: x86_64 emits ONE `mov r64,
// imm64` (LiteralIndex operand carrying the value); arm64 emits the
// MOVZ + MOVK ladder (chunk0 0x1234, chunk1 0xCDEF, chunk2 0xAB). The
// SAME source const routes to two different shapes purely by declared
// capability — zero `if (arch)`.

// arm64: the wide value (> int32) splits into the ladder. chunks of
// 0xABCDEF1234: [0]=0x1234, [1]=0xCDEF, [2]=0x00AB, [3]=0. So mov
// #0x1234 + movk_lsl16 #0xCDEF + movk_lsl32 #0xAB (+ ret) = 4 insts.
// RED-ON-DISABLE: revert lowerConst's wide-int capability block →
// 0xABCDEF1234 falls to the pool dead-end → lowering fail-loud → lirR.ok
// is false → the `EXPECT_TRUE(lirR.ok)` in the helper goes RED.
TEST(MirToLir, Arm64WideConstAboveInt32SplitsIntoLadder) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto const movOp = (*target)->opcodeByMnemonic("mov");
    auto const mk16  = (*target)->opcodeByMnemonic("movk_lsl16");
    auto const mk32  = (*target)->opcodeByMnemonic("movk_lsl32");
    ASSERT_TRUE(movOp.has_value() && mk16.has_value() && mk32.has_value());

    auto const chain = lowerConstChain(
        static_cast<std::int64_t>(0xABCDEF1234ll), TypeKind::I64, "arm64");
    ASSERT_EQ(chain.opcodes.size(), 4u)
        << "0xABCDEF1234 (>int32) must lower as mov + 2 movk + ret";
    EXPECT_EQ(chain.opcodes[0], *movOp);
    EXPECT_EQ(chain.immValues[0], 0x1234);
    EXPECT_EQ(chain.opcodes[1], *mk16);
    EXPECT_EQ(chain.immValues[1], 0xCDEF);
    EXPECT_EQ(chain.opcodes[2], *mk32);
    EXPECT_EQ(chain.immValues[2], 0x00AB);
}

// x86_64: the SAME wide value (> int32) materializes via the single
// `mov r64, imm64` — its source operand is a LiteralIndex carrying the
// full 64-bit value (the inline imm32 carrier can't hold it). RED-ON-
// DISABLE: revert the wide-int block → pool dead-end → at THIS tier the
// helper's lowering succeeds but emits the (now unencodable) LiteralIndex
// shape with no value capture / wrong opcode-count, and the asm tier
// (the corpus) would fail-loud at encode. Here the litValues capture
// pins the value made it onto the pool-carried operand.
TEST(MirToLir, X64WideConstAboveInt32UsesMovImm64) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const movOp = (*target)->opcodeByMnemonic("mov");
    ASSERT_TRUE(movOp.has_value());
    EXPECT_FALSE((*target)->opcodeByMnemonic("movk_lsl16").has_value())
        << "precondition: x86_64 must not declare the arm64 movk family";

    std::uint64_t const wide = 0xABCDEF1234ULL;
    auto const chain = lowerConstChain(
        static_cast<std::int64_t>(wide), TypeKind::I64, "x86_64");
    ASSERT_EQ(chain.opcodes.size(), 2u)
        << "x86 materializes a >int32 const as ONE mov r64, imm64 + ret";
    EXPECT_EQ(chain.opcodes[0], *movOp);
    // The value rides the literal pool (LiteralIndex operand), NOT the
    // inline imm32 arm.
    EXPECT_EQ(chain.immValues[0], 0)
        << "wide const must NOT ride the inline imm32 operand";
    EXPECT_EQ(chain.litValues[0], wide)
        << "the full 64-bit value must be carried in the literal pool";
}

// D-LIR-GLOBALADDR-LOAD-RIPREL-FOLD, the CAPABILITY boundary: arm64's
// FPR load (`fldur`) declares NO [symbol] encoding variant — its
// address materialization is the 2-word ADRP+ADD lea macro, and a
// symbol-relative LDR fold is a DIFFERENT encoding shape (deliberately
// out of scope; see the registry row). The fold predicate must
// therefore NOT fire on arm64: the single-use GlobalAddr keeps its
// lea and the load keeps the [base] form. This is the agnosticism pin
// — the fold is mnemonic-capability-driven, never `if (x86)`.
TEST(MirToLir, Arm64GlobalAddrLoadKeepsLeaPlusBaseFormLoad) {
    TypeInterner interner{CompilationUnitId{1}};
    auto const f64    = interner.primitive(TypeKind::F64);
    auto const ptrF64 = interner.pointer(f64);
    auto const sig = interner.fnSig(std::span<TypeId const>{}, f64,
                                    CallConv::CcSysV);
    MirBuilder mb;
    MirLiteralValue quarter; quarter.value = 0.25;
    quarter.core = TypeKind::F64;
    mb.addFunction(sig, SymbolId{1});
    (void)mb.addGlobal(f64, SymbolId{500}, mb.literalPoolAdd(quarter),
                       MirFuncId{}, SymbolBinding::Global,
                       SymbolVisibility::Default, /*isConst=*/false,
                       MirThreadStorage::Shared);
    MirBlockId const bb = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    MirInstId const addr = mb.addGlobalAddr(SymbolId{500}, ptrF64);
    MirInstId const loadOps[] = {addr};
    MirInstId const c = mb.addInst(MirOpcode::Load, loadOps, f64);
    mb.addReturn(c);
    Mir mir = std::move(mb).finish();

    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    std::vector<dss::ExternImport> noExterns;
    auto lirR = lowerToLir(mir, **target, interner, rep, noExterns);
    ASSERT_TRUE(lirR.ok)
        << (rep.all().empty() ? "" : rep.all()[0].actual);
    Lir const& lir = lirR.lir;

    auto const leaOp   = (*target)->opcodeByMnemonic("lea");
    auto const fldurOp = (*target)->opcodeByMnemonic("fldur");
    ASSERT_TRUE(leaOp.has_value() && fldurOp.has_value());
    LirBlockId const bbL = lir.funcBlockAt(lir.funcAt(0), 0);
    bool sawLea = false;
    bool sawBaseFormLoad = false;
    bool sawSymbolLoad = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(bbL); ++i) {
        LirInstId const inst = lir.blockInstAt(bbL, i);
        auto const op = lir.instOpcode(inst);
        if (op == *leaOp) sawLea = true;
        if (op == *fldurOp) {
            auto const ops = lir.instOperands(inst);
            ASSERT_FALSE(ops.empty());
            if (ops[0].kind == LirOperandKind::Reg) sawBaseFormLoad = true;
            if (ops[0].kind == LirOperandKind::SymbolRef) sawSymbolLoad = true;
        }
    }
    EXPECT_TRUE(sawLea)
        << "arm64 must KEEP the lea (ADRP+ADD) — fldur declares no "
           "[symbol] variant, so the riprel fold must not fire";
    EXPECT_TRUE(sawBaseFormLoad)
        << "the fldur must read through the lea-produced base register";
    EXPECT_FALSE(sawSymbolLoad)
        << "no symbol-operand fldur may be emitted — the assembler has "
           "no encoding for it (the fold would be a guaranteed "
           "A_NoMatchingEncodingVariant)";
}

// The generic mul+sub expansion is the FALLBACK for targets without a
// fused multiply-subtract — and no shipped target reaches it any more
// (x86 realizes remainders at rule 2; arm64 declares msub). This pin
// keeps the fallback alive and correct under the msub-stripped arm64
// schema (the cycle-10k mutation substrate): a regression that breaks
// the expansion (or silently swallows a missing msub) goes RED here.
TEST(MirToLir, Arm64ModuloWithoutMsubFallsBackToSdivMulSub) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildBinFnMir(MirOpcode::SMod, interner);

    auto target = dss::test_support::mutateShippedTargetSchemaJson(
        "arm64", {"msub"});
    ASSERT_TRUE(target.has_value())
        << "stripping msub must not fail the loader (an optional "
           "capability, not a required verb)";
    DiagnosticReporter rep;
    std::vector<dss::ExternImport> noExterns;
    auto lirR = lowerToLir(mir, **target, interner, rep, noExterns);
    ASSERT_TRUE(lirR.ok)
        << (rep.all().empty() ? "" : rep.all()[0].actual);
    Lir const& lir = lirR.lir;

    auto const sdivOp = (*target)->opcodeByMnemonic("sdiv");
    auto const mulOp  = (*target)->opcodeByMnemonic("mul");
    auto const subOp  = (*target)->opcodeByMnemonic("sub");
    ASSERT_TRUE(sdivOp.has_value() && mulOp.has_value() && subOp.has_value());
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    // EXACT pre-fusion shape: arg, arg, sdiv, mul, sub, ret — 6 insts.
    ASSERT_EQ(lir.blockInstCount(bb), 6u)
        << "without msub, SMod must expand to exactly sdiv+mul+sub.";
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 2)), *sdivOp)
        << "expansion step 1: q = sdiv(n, d).";
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 3)), *mulOp)
        << "expansion step 2: t = mul(q, d).";
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(bb, 4)), *subOp)
        << "expansion step 3: r = sub(n, t).";

    // Operand wiring (the identity's correctness): mul reads (sdiv, b);
    // sub reads (a, mul). A transposed sub (t − n instead of n − t)
    // would negate every remainder.
    LirInstId const sdivInst = lir.blockInstAt(bb, 2);
    LirInstId const mulInst  = lir.blockInstAt(bb, 3);
    LirInstId const subInst  = lir.blockInstAt(bb, 4);
    auto const aReg    = lir.instResult(lir.blockInstAt(bb, 0));
    auto const sdivReg = lir.instResult(sdivInst);
    auto const mulReg  = lir.instResult(mulInst);
    auto const mulOps  = lir.instOperands(mulInst);
    ASSERT_EQ(mulOps.size(), 2u);
    EXPECT_EQ(mulOps[0].reg, sdivReg)
        << "mul's first operand must be the sdiv quotient.";
    auto const subOps = lir.instOperands(subInst);
    ASSERT_EQ(subOps.size(), 2u);
    EXPECT_EQ(subOps[0].reg, aReg)
        << "sub's first operand must be the DIVIDEND (n − q·d, not "
           "q·d − n — a transpose negates every remainder).";
    EXPECT_EQ(subOps[1].reg, mulReg)
        << "sub's second operand must be the mul product.";
}

// FLAG-1 mirror inside the expansion: arm64 UMod must expand via
// `udiv` (unsigned), never `sdiv`.
TEST(MirToLir, Arm64UnsignedModuloExpandsViaUdivNotSdiv) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildBinFnMir(MirOpcode::UMod, interner);

    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    std::vector<dss::ExternImport> noExterns;
    auto lirR = lowerToLir(mir, **target, interner, rep, noExterns);
    ASSERT_TRUE(lirR.ok);
    Lir const& lir = lirR.lir;

    auto const sdivOp = (*target)->opcodeByMnemonic("sdiv");
    auto const udivOp = (*target)->opcodeByMnemonic("udiv");
    ASSERT_TRUE(sdivOp.has_value() && udivOp.has_value());
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    bool foundSdiv = false;
    bool foundUdiv = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
        auto const op = lir.instOpcode(lir.blockInstAt(bb, i));
        if (op == *sdivOp) foundSdiv = true;
        if (op == *udivOp) foundUdiv = true;
    }
    EXPECT_TRUE(foundUdiv)
        << "UMod's expansion must divide via `udiv`.";
    EXPECT_FALSE(foundSdiv)
        << "UMod MUST NOT divide via `sdiv` — sign misinterpretation "
           "of high-bit dividends (FLAG 1, expansion arm).";
}

namespace {

// Strip ONE role map off `idiv_op` in the shipped x86_64 schema (the
// cycle-10k in-memory mutation substrate). The lever for the two
// red-on-disable tests below — stripping must NOT fail the loader
// (the role maps are optional; cqo/xor_rdx_zero never declare them),
// only the LOWERING.
[[nodiscard]] std::shared_ptr<TargetSchema>
loadX64WithIdivRoleMapStripped(char const* mapKey) {
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [&](nlohmann::json& doc) {
            for (auto& op : doc.at("opcodes")) {
                if (op.value("mnemonic", "") == "idiv_op") {
                    op.at("implicitRegisters").erase(mapKey);
                }
            }
        });
    if (!mutated.has_value()) {
        ADD_FAILURE() << "role maps are OPTIONAL at load — stripping '"
                      << mapKey << "' must not fail the loader: "
                      << (mutated.error().empty()
                              ? "" : mutated.error()[0].message);
        return nullptr;
    }
    return *mutated;
}

}  // namespace

// RED-ON-DISABLE lever for the role contract: strip `outputRoles`
// from idiv_op (a mutated schema) → the SDiv lowering must FAIL LOUD
// (L_RequiredLirOpcodeMissing naming the missing role), never fall
// back to positional indexing. This is the proof the projection
// actually READS the role map.
TEST(MirToLir, MissingOutputRolesFailsLoudOnDivLowering) {
    auto mutated = loadX64WithIdivRoleMapStripped("outputRoles");
    ASSERT_NE(mutated, nullptr);

    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildBinFnMir(MirOpcode::SDiv, interner);
    DiagnosticReporter rep;
    std::vector<dss::ExternImport> noExterns;
    auto lirR = lowerToLir(mir, *mutated, interner, rep, noExterns);
    EXPECT_FALSE(lirR.ok)
        << "an idiv_op without outputRoles must FAIL the div lowering "
           "(the projection reads BY ROLE — no positional fallback).";
    bool sawRoleDiag = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_RequiredLirOpcodeMissing
            && d.actual.find("quotient") != std::string::npos
            && d.actual.find("outputRoles") != std::string::npos) {
            sawRoleDiag = true;
        }
    }
    EXPECT_TRUE(sawRoleDiag)
        << "the fail-loud diagnostic must name the missing role "
           "('quotient') and the map ('outputRoles').";
}

// The inputRoles twin (plan-lock MUST-FIX 1 — the dividend pin is the
// SAME silent-reorder class as the output projection): strip
// `inputRoles` → fail loud naming 'dividend'.
TEST(MirToLir, MissingInputRolesFailsLoudOnDivLowering) {
    auto mutated = loadX64WithIdivRoleMapStripped("inputRoles");
    ASSERT_NE(mutated, nullptr);

    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildBinFnMir(MirOpcode::SDiv, interner);
    DiagnosticReporter rep;
    std::vector<dss::ExternImport> noExterns;
    auto lirR = lowerToLir(mir, *mutated, interner, rep, noExterns);
    EXPECT_FALSE(lirR.ok);
    bool sawRoleDiag = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_RequiredLirOpcodeMissing
            && d.actual.find("dividend") != std::string::npos
            && d.actual.find("inputRoles") != std::string::npos) {
            sawRoleDiag = true;
        }
    }
    EXPECT_TRUE(sawRoleDiag)
        << "the fail-loud diagnostic must name the missing role "
           "('dividend') and the map ('inputRoles').";
}

// ── FC3.5 sweep-c1: capability-driven shift lowering ────────────────────
// (the D-CSUBSET-32BIT-ALU-FORMS shifts residue — x86 implicit-CL
//  "count" role contract / imm8 constant form / arm64 native LSLV.)

namespace {

// Self-contained probe result for the shift-lowering pins (the later
// FC3-section `GuardProbe` is declared further down this TU; a
// distinct name keeps the anonymous namespace ODR-clean).
struct ShiftProbe {
    ::dss::DiagnosticReporter rep;
    ::dss::Lir                lir;
    bool ok = false;
};

// Hand-build `fn(a, b) -> a OP b` (variable count) or
// `fn(a) -> a OP <countConst>` and lower against `schema`.
[[nodiscard]] ShiftProbe lowerShiftProbe(
        std::shared_ptr<::dss::TargetSchema> const& schema,
        ::dss::MirOpcode op, ::dss::TypeKind k,
        std::optional<std::int64_t> countConst = std::nullopt) {
    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const ty = interner.primitive(k);
    ShiftProbe probe;
    if (schema == nullptr) { return probe; }
    ::dss::MirBuilder mb;
    if (countConst.has_value()) {
        std::array<::dss::TypeId, 1> params{ty};
        mb.addFunction(interner.fnSig(params, ty, ::dss::CallConv::CcSysV),
                       ::dss::SymbolId{1});
        auto const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
        mb.beginBlock(bb);
        auto const a = mb.addArg(0, ty);
        auto const c = mb.addConst(
            ::dss::MirLiteralValue{*countConst, ::dss::TypeKind::I64}, ty);
        std::array<::dss::MirInstId, 2> ops{a, c};
        mb.addReturn(mb.addInst(op, ops, ty));
    } else {
        std::array<::dss::TypeId, 2> params{ty, ty};
        mb.addFunction(interner.fnSig(params, ty, ::dss::CallConv::CcSysV),
                       ::dss::SymbolId{1});
        auto const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
        mb.beginBlock(bb);
        auto const a = mb.addArg(0, ty);
        auto const b = mb.addArg(1, ty);
        std::array<::dss::MirInstId, 2> ops{a, b};
        mb.addReturn(mb.addInst(op, ops, ty));
    }
    ::dss::Mir m = std::move(mb).finish();
    auto result = ::dss::lowerToLir(m, *schema, interner, probe.rep);
    probe.ok  = result.ok;
    probe.lir = std::move(result.lir);
    return probe;
}

struct ShiftInstShape {
    std::size_t operandCount = 0;
    bool        op1IsImm     = false;
    std::int32_t imm         = -1;
};

// Find the first inst with `mnemonic`; also report whether any mov
// writes a PHYSICAL register of `physOrdinal` (the count pin).
[[nodiscard]] std::optional<ShiftInstShape>
findShiftShape(::dss::Lir const& lir, ::dss::TargetSchema const& sch,
               std::string_view mnemonic, std::uint16_t physOrdinal,
               bool& sawPinToPhys) {
    sawPinToPhys = false;
    std::optional<ShiftInstShape> shape;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        auto const fn = lir.funcAt(f);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            auto const bb = lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(bb); ++ii) {
                auto const inst  = lir.blockInstAt(bb, ii);
                auto const* info = sch.opcodeInfo(lir.instOpcode(inst));
                if (info == nullptr) continue;
                auto const result = lir.instResult(inst);
                if (info->mnemonic == "mov" && result.valid()
                    && result.isPhysical != 0
                    && result.id == physOrdinal) {
                    sawPinToPhys = true;
                }
                if (info->mnemonic == mnemonic && !shape.has_value()) {
                    auto const ops = lir.instOperands(inst);
                    ShiftInstShape s;
                    s.operandCount = ops.size();
                    if (ops.size() == 2
                        && ops[1].kind == ::dss::LirOperandKind::ImmInt) {
                        s.op1IsImm = true;
                        s.imm = ops[1].immInt32;
                    }
                    shape = s;
                }
            }
        }
    }
    return shape;
}

} // namespace

TEST(MirToLir, VariableShiftOnX64PinsCountByRoleAndEmitsOneOperandCore) {
    // x86 has no 3-address reg-count shift — the count must reach CL.
    // The lowering pins the count vreg into the ROLE-declared register
    // (mov rcx_phys, count) and emits the core with ONE explicit
    // operand (the value; requires2Address makes it the destination).
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const rcxOrd = (*target)->registerByName("rcx");
    ASSERT_TRUE(rcxOrd.has_value());
    for (auto const& [op, mn] : {
             std::pair{::dss::MirOpcode::Shl,  "shl"},
             std::pair{::dss::MirOpcode::LShr, "shr_l"},
             std::pair{::dss::MirOpcode::AShr, "shr_a"}}) {
        auto probe = lowerShiftProbe(*target, op,
                                     op == ::dss::MirOpcode::LShr
                                         ? ::dss::TypeKind::U64
                                         : ::dss::TypeKind::I64);
        EXPECT_TRUE(probe.ok) << mn << ": "
            << (probe.rep.all().empty() ? "" : probe.rep.all()[0].actual);
        bool sawPin = false;
        auto const shape =
            findShiftShape(probe.lir, **target, mn, *rcxOrd, sawPin);
        ASSERT_TRUE(shape.has_value()) << mn;
        EXPECT_EQ(shape->operandCount, 1u)
            << mn << ": the CL core carries ONE explicit operand";
        EXPECT_TRUE(sawPin)
            << mn << ": the count must be pinned into the role-declared "
                      "register (mov rcx_phys, count)";
    }
}

TEST(MirToLir, ConstantCountShiftOnX64SelectsTheImm8Form) {
    // A MIR-Const count in [0,255] + a declared [reg,imm] variant →
    // the imm8 form: TWO operands, op1 an ImmInt, NO rcx pin.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const rcxOrd = (*target)->registerByName("rcx");
    ASSERT_TRUE(rcxOrd.has_value());
    auto probe = lowerShiftProbe(*target, ::dss::MirOpcode::Shl,
                                 ::dss::TypeKind::I64,
                                 /*countConst=*/3);
    EXPECT_TRUE(probe.ok)
        << (probe.rep.all().empty() ? "" : probe.rep.all()[0].actual);
    bool sawPin = false;
    auto const shape =
        findShiftShape(probe.lir, **target, "shl", *rcxOrd, sawPin);
    ASSERT_TRUE(shape.has_value());
    EXPECT_EQ(shape->operandCount, 2u);
    EXPECT_TRUE(shape->op1IsImm);
    EXPECT_EQ(shape->imm, 3);
    EXPECT_FALSE(sawPin)
        << "an immediate-count shift needs no CL pin";
}

TEST(MirToLir, OutOfImm8RangeConstantCountFallsBackToTheClForm) {
    // A 300 count is C-UB (>= width) but must still ENCODE — the imm8
    // byte can't hold it, so the lowering falls back to the CL form
    // (hardware masks; never a silent truncation to one byte).
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const rcxOrd = (*target)->registerByName("rcx");
    ASSERT_TRUE(rcxOrd.has_value());
    auto probe = lowerShiftProbe(*target, ::dss::MirOpcode::Shl,
                                 ::dss::TypeKind::I64,
                                 /*countConst=*/300);
    EXPECT_TRUE(probe.ok);
    bool sawPin = false;
    auto const shape =
        findShiftShape(probe.lir, **target, "shl", *rcxOrd, sawPin);
    ASSERT_TRUE(shape.has_value());
    EXPECT_EQ(shape->operandCount, 1u);
    EXPECT_TRUE(sawPin);
}

TEST(MirToLir, VariableShiftOnArm64UsesTheNativeThreeAddressForm) {
    // arm64 declares NO implicitRegisters on its shifts (LSLV/LSRV/
    // ASRV are 3-address) → the generic reg,reg form, no pin mov.
    auto target = ::dss::TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto probe = lowerShiftProbe(*target, ::dss::MirOpcode::Shl,
                                 ::dss::TypeKind::I64);
    EXPECT_TRUE(probe.ok)
        << (probe.rep.all().empty() ? "" : probe.rep.all()[0].actual);
    bool sawPin = false;  // probe ordinal irrelevant on arm64; use 0xFFFF
    auto const shape =
        findShiftShape(probe.lir, **target, "shl", 0xFFFF, sawPin);
    ASSERT_TRUE(shape.has_value());
    EXPECT_EQ(shape->operandCount, 2u)
        << "native 3-address: value + count as plain reg operands";
    EXPECT_FALSE(shape->op1IsImm);
}

// RED-ON-DISABLE lever for the count-role contract (the FC1
// MissingInputRolesFailsLoud twin): strip `inputRoles` from the
// shipped shl — the lowering must FAIL LOUD naming the missing role,
// never fall back to a positional or name-based register guess.
TEST(MirToLir, MissingCountRoleFailsLoudOnShiftLowering) {
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            for (auto& op : doc.at("opcodes")) {
                if (op.value("mnemonic", "") == "shl") {
                    op.at("implicitRegisters").erase("inputRoles");
                }
            }
        });
    ASSERT_TRUE(mutated.has_value())
        << "role maps are OPTIONAL at load — stripping must only fail "
           "the LOWERING";
    auto probe = lowerShiftProbe(*mutated, ::dss::MirOpcode::Shl,
                                 ::dss::TypeKind::I64);
    EXPECT_FALSE(probe.ok)
        << "a shl with implicitRegisters but no count role must FAIL "
           "the shift lowering";
    bool sawRoleDiag = false;
    for (auto const& d : probe.rep.all()) {
        if (d.code == DiagnosticCode::L_RequiredLirOpcodeMissing
            && d.actual.find("count") != std::string::npos
            && d.actual.find("inputRoles") != std::string::npos) {
            sawRoleDiag = true;
        }
    }
    EXPECT_TRUE(sawRoleDiag)
        << "the fail-loud diagnostic must name the missing role "
           "('count') and the map ('inputRoles').";
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

// c115 SEH (D-WIN64-SEH-FUNCLETS): the honest c115 boundary — the SEH region
// ops fail LOUD at mir_to_lir on EVERY target (the x64 funclet lowering is c116)
// with the anchor named in the message. RED-on-disable: a `case SehTryBegin:
// return;` no-op would silently drop the region → the exception is never caught.
TEST(MirToLir, SehOpcodesFailLoudCitingC116Anchor) {
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const& sch = **target;

    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const i32   = interner.primitive(::dss::TypeKind::I32);
    auto const fnSig = interner.fnSig(std::span<::dss::TypeId const>{}, i32, ::dss::CallConv::CcSysV);

    // A minimal region skeleton: entry SehTryBegin(id) -> [try, filter];
    // try: SehTryEnd + Br(join); filter: SehFilterReturn(v) -> handler;
    // handler: Br(join); join: return.
    ::dss::MirBuilder mb;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const entry   = mb.createBlock(::dss::StructCfMarker::EntryBlock);
    ::dss::MirBlockId const tryBB   = mb.createBlock(::dss::StructCfMarker::Linear);
    ::dss::MirBlockId const filterBB= mb.createBlock(::dss::StructCfMarker::Linear);
    ::dss::MirBlockId const handBB  = mb.createBlock(::dss::StructCfMarker::Linear);
    ::dss::MirBlockId const joinBB  = mb.createBlock(::dss::StructCfMarker::Linear);
    mb.beginBlock(entry);
    mb.addSehTryBegin(tryBB, filterBB, /*regionId=*/0);
    mb.beginBlock(tryBB);
    mb.addInst(::dss::MirOpcode::SehTryEnd, {}, ::dss::InvalidType, /*payload=*/0);
    mb.addBr(joinBB);
    mb.beginBlock(filterBB);
    ::dss::MirInstId const code = mb.addInst(::dss::MirOpcode::SehExceptionCode, {}, i32);
    mb.addSehFilterReturn(code, handBB, /*regionId=*/0);
    mb.beginBlock(handBB);
    mb.addBr(joinBB);
    mb.beginBlock(joinBB);
    ::dss::MirLiteralValue lv; lv.value = static_cast<std::int64_t>(0); lv.core = ::dss::TypeKind::I32;
    mb.addReturn(mb.addConst(lv, i32));
    ::dss::Mir m = std::move(mb).finish();

    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, sch, interner, rep);
    EXPECT_FALSE(result.ok);
    bool sawAnchor = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_UnsupportedLoweringForOpcode
            && d.actual.find("D-WIN64-SEH-FUNCLETS") != std::string::npos) {
            sawAnchor = true;
        }
    }
    EXPECT_TRUE(sawAnchor)
        << "a SEH region op must fail loud citing D-WIN64-SEH-FUNCLETS (c116)";
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
    // The non-Bool int condition lowers as the truthiness ICmpNe(c, 0)
    // (cst_to_hir's coerceCondition — it used to be a Cast the LIR tier
    // could not lower), which the CondBr fusion machinery handles — so
    // the WHOLE function now lowers cleanly and `L.lir.ok` is required.
    auto L = lowerCSubsetToLir(
        "int f(int c) { return c ? 1 : 2; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok)
        << "ternary with a bare int cond must lower end-to-end (the "
           "truthiness ICmpNe is CondBr-fusable)";

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

TEST(MirToLir, EnumPackedFieldMemoryAccessIsWidthExactToUnderlying) {
    // D-CSUBSET-ENUM-INT-CONVERSION × D-LIR-INT-MEMORY-WIDTH-EXACT: an enum has
    // NO representation of its own (C 6.7.2.2) — it IS its underlying integer
    // (default I32 → 4 bytes). A Load/Store of an enum value into a PACKED
    // struct field must therefore be width-EXACT (kLirInstFlagWidth32), NEVER
    // the width-default 64-bit: an 8-byte access of a 4-byte field overruns it
    // and clobbers the neighbour. `mir_to_lir.cpp::reprKind()` resolves
    // Enum → scalars[0] at the width tier so every width site (Load + Store)
    // sees the underlying int. RED-ON-DISABLE: revert reprKind (Enum falls to
    // the `default` arm → flag 0) and BOTH EXPECT flip. A scalar enum local
    // sits in its own >=8-byte slot and MASKS this — so the struct-FIELD form
    // is the load-bearing pin. This is the HOST-INDEPENDENT structural guard
    // (runs on every CI leg) complementing the enum_value corpus, whose
    // runtime exit-code witness only fires on legs the host/CI can EXECUTE.
    // A plain pointer deref forces the read/write through memory (no struct →
    // no aggregate layout needed): `*p` is a scalar enum store + load. The enum
    // is I32-underlying (4 bytes) — a packed memory access whose width MUST be
    // exact (a scalar enum LOCAL would sit in its own >=8-byte slot and mask a
    // regression, so the through-pointer access is the load-bearing form).
    auto L = lowerCSubsetToLir(
        "enum E { Z, A }; "
        "int f(enum E* p) { *p = A; return (int)*p; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok)
        << "LIR: " << (L.lirReporter.all().empty() ? "" : L.lirReporter.all()[0].actual);
    auto const storeOp = *L.target->opcodeByMnemonic("store");
    auto const loadOp  = *L.target->opcodeByMnemonic("load");
    Lir const& lir = L.lir.lir;
    int stores = 0, loads = 0;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                LirInstId const inst = lir.blockInstAt(blk, i);
                auto const op = lir.instOpcode(inst);
                if (op == storeOp) {
                    ++stores;
                    EXPECT_NE(lir.instFlags(inst) & kLirInstFlagWidth32, 0u)
                        << "enum field STORE must be width-32 (enum → I32 "
                           "underlying), not the width-default 64-bit that "
                           "overruns the 4-byte field and clobbers the neighbour";
                }
                if (op == loadOp) {
                    ++loads;
                    EXPECT_NE(lir.instFlags(inst) & kLirInstFlagWidth32, 0u)
                        << "enum field LOAD must be width-32 (enum → I32 underlying)";
                }
            }
        }
    }
    EXPECT_GE(stores, 1) << "the `p->c = A` enum-field store must reach LIR";
    EXPECT_GE(loads, 1)  << "the `(int)p->c` enum-field load must reach LIR";
}

TEST(MirToLir, AllocaResultIsAddressableViaStore) {
    // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): a body-local's storage
    // address is REMATERIALIZED at each use — the `alloca` op reserves the slot
    // (in scan order), and each USE of the address emits a fresh `lea_frame_slot k`
    // (k = the alloca's 0-based scan-order index) whose result becomes the use's
    // base register. So the Store writes through a `lea_frame_slot` result, NOT the
    // `alloca` result directly. This pins the remat wiring (Store base ← a
    // `lea_frame_slot` re-reference of the local's slot, index 0 for the sole
    // local); a regression that reverted to caching one entry-spanning alloca
    // address vreg, or threaded the wrong slot index, surfaces here.
    auto L = lowerCSubsetToLir(
        "int f() { int x; x = 1; return x; }");
    assertUpstreamClean(L);
    ASSERT_TRUE(L.lir.ok);
    auto const allocaOp      = *L.target->opcodeByMnemonic("alloca");
    auto const storeOp       = *L.target->opcodeByMnemonic("store");
    auto const leaFrameSlotOp = *L.target->opcodeByMnemonic("lea_frame_slot");
    Lir const& lir = L.lir.lir;
    LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    bool sawAlloca = false, sawStore = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        LirInstId const inst = lir.blockInstAt(entry, i);
        if (lir.instOpcode(inst) == allocaOp) {
            sawAlloca = true;
            EXPECT_TRUE(lir.instResult(inst).valid())
                << "Alloca must produce a valid result vreg (it reserves the slot)";
        }
        if (lir.instOpcode(inst) == storeOp && sawAlloca) {
            auto const ops = lir.instOperands(inst);
            ASSERT_EQ(ops.size(), 4u);
            // ops[1] is the base register the Store writes through — now a
            // `lea_frame_slot` re-reference's result, not the alloca's.
            EXPECT_EQ(ops[1].kind, LirOperandKind::Reg);
            LirReg const baseReg = ops[1].reg;
            // The producer of `baseReg` must be a `lea_frame_slot` whose payload
            // is the local's slot index (0 — it is the only/first alloca).
            bool baseFromLeaFrameSlot = false;
            for (std::uint32_t j = 0; j < lir.blockInstCount(entry); ++j) {
                LirInstId const def = lir.blockInstAt(entry, j);
                if (lir.instOpcode(def) == leaFrameSlotOp
                    && lir.instResult(def) == baseReg) {
                    baseFromLeaFrameSlot = true;
                    EXPECT_EQ(lir.instPayload(def), 0u)
                        << "the sole local's lea_frame_slot must carry slot index 0";
                    break;
                }
            }
            EXPECT_TRUE(baseFromLeaFrameSlot)
                << "Store's base must be a lea_frame_slot re-reference of the "
                   "local's slot (the remat'd address), not the alloca result";
            sawStore = true;
            break;
        }
    }
    EXPECT_TRUE(sawAlloca);
    EXPECT_TRUE(sawStore);
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
        // c78 (D-CSUBSET-FLOAT-NEG-ENCODING): x86 has NO native FP-negate, so
        // FNeg capability-dispatches to `fneg_mask` (xorpd xmm,[rip+signmask]) —
        // still an FPR-class result. (arm64 keeps the native `fneg` opcode; this
        // test loads the x86_64 schema, so the realized op here is fneg_mask.)
        {::dss::MirOpcode::FNeg, "fneg_mask", 1},
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
        // FC3 c2 → FC3.5: the zext mnemonic carries TWO width-keyed
        // forms — the Bool 0/1 byte source (x86 movzx r64, r/m8,
        // width-default) and the U32 source (mov r32,r32, width 32;
        // D-CSUBSET-ZEXT-32-TO-64 — see
        // ZExtFromU32SourceLowersCarryingSourceWidth). Narrower
        // sources stay gated at requireNativeIntWidth, and mapCast
        // never mints an I32-source ZExt (signed widening is SExt).
        CastCase{::dss::MirOpcode::ZExt,     "zext",  ::dss::TypeKind::Bool, ::dss::TypeKind::I64, LirRegClass::GPR},
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
    // FC2 Part B: F64 constants reach MIR as promoted rodata globals
    // (GlobalAddr + Load — the HIR→MIR promotion shape); a raw F64
    // `Const` is now a loud MIR→LIR error. Build the phi inputs the
    // promoted way.
    auto const ptrF64 = interner.pointer(f64);
    ::dss::MirLiteralValue lvOne;  lvOne.value  = 1.0;  lvOne.core  = ::dss::TypeKind::F64;
    ::dss::MirLiteralValue lvTwo;  lvTwo.value  = 2.0;  lvTwo.core  = ::dss::TypeKind::F64;
    (void)mb.addGlobal(f64, ::dss::SymbolId{500}, mb.literalPoolAdd(lvOne),
                       ::dss::MirFuncId{}, ::dss::SymbolBinding::Global,
                       ::dss::SymbolVisibility::Default, /*isConst=*/false,
                       MirThreadStorage::Shared);
    (void)mb.addGlobal(f64, ::dss::SymbolId{501}, mb.literalPoolAdd(lvTwo),
                       ::dss::MirFuncId{}, ::dss::SymbolBinding::Global,
                       ::dss::SymbolVisibility::Default, /*isConst=*/false,
                       MirThreadStorage::Shared);
    mb.beginBlock(thenB);
    ::dss::MirInstId const addrOne = mb.addGlobalAddr(::dss::SymbolId{500}, ptrF64);
    std::array<::dss::MirInstId, 1> loadOneOps{addrOne};
    ::dss::MirInstId const constOne = mb.addInst(::dss::MirOpcode::Load, loadOneOps, f64);
    mb.addBr(join);
    mb.beginBlock(elseB);
    ::dss::MirInstId const addrTwo = mb.addGlobalAddr(::dss::SymbolId{501}, ptrF64);
    std::array<::dss::MirInstId, 1> loadTwoOps{addrTwo};
    ::dss::MirInstId const constTwo = mb.addInst(::dss::MirOpcode::Load, loadTwoOps, f64);
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

TEST(MirToLir, FprConstAtMirToLirFailsLoudPointingAtPromotion) {
    // FC2 Part B retired the cycle-3d F64→LirLiteralPool route: it was
    // a DEAD END (no encoding variant consumes a LiteralIndex operand
    // → A_NoMatchingEncodingVariant at assemble time, after regalloc
    // had already burned an FPR). F64 constants are now promoted at
    // HIR→MIR to anonymous rodata globals (GlobalAddr + Load). A raw
    // F64 `Const` reaching MIR→LIR — hand-built MIR, or a future
    // producer that forgets the promotion — must FAIL LOUD naming the
    // contract, never silently re-enter the pool dead-end.
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
    EXPECT_FALSE(result.ok)
        << "an F64 Const at MIR→LIR must fail loud (the LiteralIndex "
           "route is a dead end; promotion happens at HIR→MIR)";
    bool sawPromotionDiag = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_UnsupportedLoweringForOpcode
            && d.actual.find("promoted to anonymous rodata globals")
                   != std::string::npos) {
            sawPromotionDiag = true;
        }
    }
    EXPECT_TRUE(sawPromotionDiag)
        << "the diagnostic must point the producer at the HIR→MIR "
           "promotion contract";
}

// FC3.5 sweep-c2 (D-CSUBSET-F32-CODEGEN closed): an F32 FAdd now
// LOWERS — the fadd inst must carry the width-32 flag so the
// assembler's variant matcher selects ADDSS (F3 0F 58), never the F64
// ADDSD sibling. The width AXIS is the discrimination pin: a dropped
// F32→width-32 mapping would re-select the 64-bit default and the
// width-keyed F2 variant — double-width arithmetic on F32 values.
TEST(MirToLir, F32FAddLowersWithWidth32Flag) {
    std::array<::dss::TypeKind, 2> paramKinds{
        ::dss::TypeKind::F32, ::dss::TypeKind::F32};
    auto syn = ::dss::test_support::buildSyntheticFn(
        paramKinds, ::dss::TypeKind::F32,
        [](::dss::MirBuilder& mb, ::dss::TypeInterner&,
           std::span<::dss::TypeId const> params, ::dss::TypeId retT) {
            ::dss::MirInstId const a = mb.addArg(0, params[0]);
            ::dss::MirInstId const b = mb.addArg(1, params[1]);
            ::dss::MirInstId const ops[] = {a, b};
            ::dss::MirInstId const r =
                mb.addInst(::dss::MirOpcode::FAdd, ops, retT);
            mb.addReturn(r);
        });
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(syn.mir, **target, syn.interner, rep);
    ASSERT_TRUE(result.ok)
        << (rep.all().empty() ? "" : rep.all()[0].actual);
    EXPECT_EQ(rep.errorCount(), 0u);
    auto const faddOp = (*target)->opcodeByMnemonic("fadd");
    ASSERT_TRUE(faddOp.has_value());
    bool sawWidth32Fadd = false;
    for (std::uint32_t f = 0; f < result.lir.moduleFuncCount(); ++f) {
        auto const fn = result.lir.funcAt(f);
        for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
            auto const bb = result.lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < result.lir.blockInstCount(bb); ++ii) {
                auto const id = result.lir.blockInstAt(bb, ii);
                if (result.lir.instOpcode(id) != *faddOp) continue;
                EXPECT_EQ(::dss::lirInstWidthBits(result.lir.instFlags(id)),
                          32u)
                    << "an F32 FAdd must carry the width-32 flag (ADDSS, "
                       "never the F64 ADDSD sibling)";
                sawWidth32Fadd = true;
            }
        }
    }
    EXPECT_TRUE(sawWidth32Fadd) << "expected a width-32 fadd inst";
}

// The F16/F128 wall stays: no scalar encodings exist at ANY width for
// them, and first-match would otherwise pick a wrong-width form
// (D-TARGET-ENCODING-WIDTH-GUARD). F16 FAdd must still fail loud.
TEST(MirToLir, F16FAddStillFailsLoudViaTheWidthGuard) {
    std::array<::dss::TypeKind, 2> paramKinds{
        ::dss::TypeKind::F16, ::dss::TypeKind::F16};
    auto syn = ::dss::test_support::buildSyntheticFn(
        paramKinds, ::dss::TypeKind::F16,
        [](::dss::MirBuilder& mb, ::dss::TypeInterner&,
           std::span<::dss::TypeId const> params, ::dss::TypeId retT) {
            ::dss::MirInstId const a = mb.addArg(0, params[0]);
            ::dss::MirInstId const b = mb.addArg(1, params[1]);
            ::dss::MirInstId const ops[] = {a, b};
            ::dss::MirInstId const r =
                mb.addInst(::dss::MirOpcode::FAdd, ops, retT);
            mb.addReturn(r);
        });
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(syn.mir, **target, syn.interner, rep);
    EXPECT_FALSE(result.ok)
        << "F16 FAdd must fail loud — no scalar encodings at any width";
    bool sawWidthDiag = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_UnsupportedLoweringForOpcode
            && d.actual.find("D-TARGET-ENCODING-WIDTH-GUARD")
                   != std::string::npos) {
            sawWidthDiag = true;
        }
    }
    EXPECT_TRUE(sawWidthDiag)
        << "the width-gate diagnostic must name "
           "D-TARGET-ENCODING-WIDTH-GUARD";
}

// Shared MIR shape for the FPR-store pair below: f(f64 v, f64* p)
// { Store v -> p; return 0; }. The stored VALUE is FPR-class, so the
// store mnemonic must resolve through the class table.
namespace {
[[nodiscard]] std::pair<::dss::Mir, ::dss::TypeInterner>
buildF64StoreMir() {
    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const f64  = interner.primitive(::dss::TypeKind::F64);
    auto const ptrT = interner.pointer(f64);
    auto const i32  = interner.primitive(::dss::TypeKind::I32);
    ::dss::TypeId const params[] = {f64, ptrT};
    auto const sig = interner.fnSig(params, i32, ::dss::CallConv::CcSysV);
    ::dss::MirBuilder mb;
    mb.addFunction(sig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb =
        mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const v = mb.addArg(0, f64);
    ::dss::MirInstId const p = mb.addArg(1, ptrT);
    ::dss::MirInstId const storeOps[] = {v, p};
    (void)mb.addInst(::dss::MirOpcode::Store, storeOps, ::dss::InvalidType);
    ::dss::MirLiteralValue zero; zero.value = std::int64_t{0};
    zero.core = ::dss::TypeKind::I32;
    mb.addReturn(mb.addConst(zero, i32));
    return {std::move(mb).finish(), std::move(interner)};
}
} // namespace

// FC2 PE-float closure (2026-06-10): the shipped x86_64 schema now
// DECLARES the fpr store (movsd_store, F2 0F 11 /r — landed with its
// first consumer, the ms_x64 callee-saved-xmm prologue spill). A MIR
// Store of an F64 value must lower through THAT mnemonic — never the
// GPR `store` (which would 8-byte-GPR-write the XMM ordinal —
// valid-looking, silently wrong bytes).
TEST(MirToLir, FprStoreLowersViaDeclaredMovsdStore) {
    auto [m, interner] = buildF64StoreMir();
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, **target, interner, rep);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(rep.errorCount(), 0u);

    auto const movsdStoreOp = (*target)->opcodeByMnemonic("movsd_store");
    auto const gprStoreOp   = (*target)->opcodeByMnemonic("store");
    ASSERT_TRUE(movsdStoreOp.has_value());
    ASSERT_TRUE(gprStoreOp.has_value());
    ::dss::Lir const& lir = result.lir;
    ::dss::LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    bool foundMovsdStore = false;
    bool foundGprStore   = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
        auto const op = lir.instOpcode(lir.blockInstAt(bb, i));
        if (op == *movsdStoreOp) foundMovsdStore = true;
        if (op == *gprStoreOp)   foundGprStore   = true;
    }
    EXPECT_TRUE(foundMovsdStore)
        << "the F64 Store must lower to the fpr class's declared "
           "store mnemonic (movsd_store)";
    EXPECT_FALSE(foundGprStore)
        << "the F64 Store must NOT emit the GPR `store` — that form "
           "would silently mis-encode the XMM ordinal";
}

// FC2 Part B (registerClassOps) fail-loud lever, PRESERVED after the
// shipped schema gained its fpr store: erase the row's "store" key
// (the StrippedClassOpsTable pattern) → MIR Store of an F64 value
// must FAIL LOUD naming the class+op, never fall back to the GPR
// `store`. This is the same lever the pre-consumer shipped schema
// exercised by omission.
TEST(MirToLir, FprStoreFailsLoudWithoutDeclaredStoreOp) {
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            doc["registerClassOps"][0].erase("store");
        });
    ASSERT_TRUE(mutated.has_value())
        << "a registerClassOps row with no 'store' slot is legal at "
           "load (trigger discipline) — only the consumer fails";

    auto [m, interner] = buildF64StoreMir();
    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, **mutated, interner, rep);
    EXPECT_FALSE(result.ok);
    bool sawClassDiag = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_RequiredLirOpcodeMissing
            && d.actual.find("'store'") != std::string::npos
            && d.actual.find("'fpr'") != std::string::npos) {
            sawClassDiag = true;
        }
    }
    EXPECT_TRUE(sawClassDiag)
        << "the diagnostic must name the class ('fpr') and the op "
           "('store') so the schema author knows what to declare";
}

// RED-ON-DISABLE lever for the class table being READ: strip the
// registerClassOps section from the shipped x86_64 schema → an F64
// load (FPR-class) must fail loud naming 'load'/'fpr' (the table is
// the ONLY source of the fpr load mnemonic; without it the lowering
// must never silently use the GPR `load`).
TEST(MirToLir, StrippedClassOpsTableFailsLoudOnFprLoad) {
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) { doc.erase("registerClassOps"); });
    ASSERT_TRUE(mutated.has_value())
        << "registerClassOps is OPTIONAL at load — stripping it must "
           "not fail the loader, only the FPR lowering";

    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const f64  = interner.primitive(::dss::TypeKind::F64);
    auto const ptrT = interner.pointer(f64);
    ::dss::TypeId const params[] = {ptrT};
    auto const sig = interner.fnSig(params, f64, ::dss::CallConv::CcSysV);
    ::dss::MirBuilder mb;
    mb.addFunction(sig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb =
        mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const p = mb.addArg(0, ptrT);
    ::dss::MirInstId const loadOps[] = {p};
    ::dss::MirInstId const v =
        mb.addInst(::dss::MirOpcode::Load, loadOps, f64);
    mb.addReturn(v);
    ::dss::Mir m = std::move(mb).finish();

    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(m, **mutated, interner, rep);
    EXPECT_FALSE(result.ok);
    bool sawClassDiag = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_RequiredLirOpcodeMissing
            && d.actual.find("'load'") != std::string::npos
            && d.actual.find("'fpr'") != std::string::npos) {
            sawClassDiag = true;
        }
    }
    EXPECT_TRUE(sawClassDiag);
}

// The FPToSI twin (FC3.5 re-pin — D-CSUBSET-F32-CODEGEN closed): an
// F32-source FPToSI now LOWERS, and the fp_to_si inst must carry the
// SOURCE's width-32 flag (the variant axis keys on the source float
// width — CVTTSS2SI F3 vs CVTTSD2SI F2 — while the RESULT-width
// default would have mis-keyed an I32 result as 32 even for an F64
// source; the override is the pinned behavior).
TEST(MirToLir, F32FpToSiSourceLowersWithSourceWidth32Flag) {
    std::array<::dss::TypeKind, 1> paramKinds{::dss::TypeKind::F32};
    auto syn = ::dss::test_support::buildSyntheticFn(
        paramKinds, ::dss::TypeKind::I32,
        [](::dss::MirBuilder& mb, ::dss::TypeInterner&,
           std::span<::dss::TypeId const> params, ::dss::TypeId retT) {
            ::dss::MirInstId const a = mb.addArg(0, params[0]);
            ::dss::MirInstId const ops[] = {a};
            ::dss::MirInstId const r =
                mb.addInst(::dss::MirOpcode::FPToSI, ops, retT);
            mb.addReturn(r);
        });
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(syn.mir, **target, syn.interner, rep);
    ASSERT_TRUE(result.ok)
        << (rep.all().empty() ? "" : rep.all()[0].actual);
    auto const fpToSiOp = (*target)->opcodeByMnemonic("fp_to_si");
    ASSERT_TRUE(fpToSiOp.has_value());
    bool saw = false;
    for (std::uint32_t f = 0; f < result.lir.moduleFuncCount(); ++f) {
        auto const fn = result.lir.funcAt(f);
        for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
            auto const bb = result.lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < result.lir.blockInstCount(bb); ++ii) {
                auto const id = result.lir.blockInstAt(bb, ii);
                if (result.lir.instOpcode(id) != *fpToSiOp) continue;
                EXPECT_EQ(::dss::lirInstWidthBits(result.lir.instFlags(id)),
                          32u)
                    << "F32-source fp_to_si must carry the SOURCE width";
                saw = true;
            }
        }
    }
    EXPECT_TRUE(saw) << "expected an fp_to_si inst";
}

// The mirror discrimination pin: an F64-source FPToSI with an I32
// RESULT must carry width-64 (the SOURCE axis) — the result-type
// default (I32 → 32) would silently select the F3 CVTTSS2SI form and
// convert an F64 as a single. This is the exact mis-key the
// widthOverride threading exists to prevent.
TEST(MirToLir, F64FpToSiWithI32ResultKeepsSourceWidth64) {
    std::array<::dss::TypeKind, 1> paramKinds{::dss::TypeKind::F64};
    auto syn = ::dss::test_support::buildSyntheticFn(
        paramKinds, ::dss::TypeKind::I32,
        [](::dss::MirBuilder& mb, ::dss::TypeInterner&,
           std::span<::dss::TypeId const> params, ::dss::TypeId retT) {
            ::dss::MirInstId const a = mb.addArg(0, params[0]);
            ::dss::MirInstId const ops[] = {a};
            ::dss::MirInstId const r =
                mb.addInst(::dss::MirOpcode::FPToSI, ops, retT);
            mb.addReturn(r);
        });
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(syn.mir, **target, syn.interner, rep);
    ASSERT_TRUE(result.ok)
        << (rep.all().empty() ? "" : rep.all()[0].actual);
    auto const fpToSiOp = (*target)->opcodeByMnemonic("fp_to_si");
    ASSERT_TRUE(fpToSiOp.has_value());
    bool saw = false;
    for (std::uint32_t f = 0; f < result.lir.moduleFuncCount(); ++f) {
        auto const fn = result.lir.funcAt(f);
        for (std::uint32_t bi = 0; bi < result.lir.funcBlockCount(fn); ++bi) {
            auto const bb = result.lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < result.lir.blockInstCount(bb); ++ii) {
                auto const id = result.lir.blockInstAt(bb, ii);
                if (result.lir.instOpcode(id) != *fpToSiOp) continue;
                EXPECT_EQ(::dss::lirInstWidthBits(result.lir.instFlags(id)),
                          64u)
                    << "F64-source fp_to_si must carry width-64 even with "
                       "an I32 result — the source axis, never the result "
                       "type";
                saw = true;
            }
        }
    }
    EXPECT_TRUE(saw) << "expected an fp_to_si inst";
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
    // D-FFI-EXTERN-CALL-DISPATCH: indirect-slot is the PE/Mach-O IAT model —
    // an extern call dereferences the import POINTER slot via FF 15. (This
    // is what the historical x86_64-PE path used unconditionally; it is now
    // selected by the format's dispatch, not assumed.)
    auto const result = ::dss::lowerToLir(m, sch, interner, rep,
                                          externImports,
                                          ::dss::ExternCallDispatch::IndirectSlot);
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

// ── D-FFI-EXTERN-CALL-DISPATCH: format-driven extern-call shape ──────
//
// The plan-lock CONFIRMED (byte-level) that x86_64-ELF FFI was latently
// broken: the linker points an extern's symbolVa at its PLT STUB (code),
// but `call_indirect_via_extern` (FF 15 = call [ptr]) dereferences those
// code bytes as a function pointer → SIGSEGV. ELF needs a PLAIN DIRECT
// call (E8 / BL) to the stub; PE/Mach-O dereference an IAT/__got pointer
// slot. The shape is a property of the OBJECT FORMAT, not the CPU target
// (the SAME x86_64 needs opposite opcodes under PE vs ELF), so
// `lowerToLir` selects the call opcode from `ExternCallDispatch`. These
// pins prove the selection at the LIR tier; each is RED-on-disable if
// `lowerCall` ignored the dispatch and reverted to FF-15-for-all-externs.
namespace {
struct ExternCallLowering {
    ::dss::MirToLirResult result;
    std::uint32_t         directCalls = 0;  // plain `call` (E8 / BL)
    std::uint32_t         externCalls = 0;  // `call_indirect_via_extern` (FF 15)
};

// Lower a fixed "1 extern + 1 internal + 1 caller calling both" MIR under
// (target, dispatch) and count the caller's call opcodes. Mirrors the
// fixture in `ExternCallEmitsCallIndirectViaExternOpcode` so the dispatch
// is the ONLY variable across the pins.
[[nodiscard]] ExternCallLowering
lowerStdExternFixture(::dss::TargetSchema const& sch,
                      std::optional<::dss::ExternCallDispatch> dispatch,
                      ::dss::DiagnosticReporter& rep,
                      ::dss::CallConv cc = ::dss::CallConv::CcMS64) {
    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const i32  = interner.primitive(::dss::TypeKind::I32);
    auto const ptrT = interner.primitive(::dss::TypeKind::Ptr);
    auto const internalSig = interner.fnSig(
        std::span<::dss::TypeId const>{}, i32, cc);

    constexpr std::uint32_t kExternSym   = 100u;
    constexpr std::uint32_t kInternalSym = 101u;
    constexpr std::uint32_t kCallerSym   = 102u;

    ::dss::MirBuilder mb;
    mb.addFunction(internalSig, ::dss::SymbolId{kInternalSym});
    {
        auto const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
        mb.beginBlock(bb);
        ::dss::MirLiteralValue lv; lv.value = std::int64_t{0};
        lv.core = ::dss::TypeKind::I32;
        mb.addReturn(mb.addConst(lv, i32));
    }
    mb.addFunction(internalSig, ::dss::SymbolId{kCallerSym});
    {
        auto const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
        mb.beginBlock(bb);
        ::dss::MirLiteralValue lv; lv.value = std::int64_t{0};
        lv.core = ::dss::TypeKind::I32;
        ::dss::MirInstId const zero = mb.addConst(lv, i32);
        ::dss::MirInstId const externAddr =
            mb.addGlobalAddr(::dss::SymbolId{kExternSym}, ptrT);
        std::array<::dss::MirInstId, 2> externOps{externAddr, zero};
        ::dss::MirInstId const externResult =
            mb.addInst(::dss::MirOpcode::Call, externOps, i32);
        ::dss::MirInstId const intAddr =
            mb.addGlobalAddr(::dss::SymbolId{kInternalSym}, ptrT);
        std::array<::dss::MirInstId, 1> intOps{intAddr};
        ::dss::MirInstId const intResult =
            mb.addInst(::dss::MirOpcode::Call, intOps, i32);
        std::array<::dss::MirInstId, 2> addOps{externResult, intResult};
        mb.addReturn(mb.addInst(::dss::MirOpcode::Add, addOps, i32));
    }
    ::dss::Mir m = std::move(mb).finish();

    ::dss::ExternImport ext{};
    ext.symbol      = ::dss::SymbolId{kExternSym};
    ext.mangledName = "extern_fn";
    ext.libraryPath = "fictional.lib";
    std::vector<::dss::ExternImport> externImports{ext};

    ExternCallLowering out{
        ::dss::lowerToLir(m, sch, interner, rep, externImports, dispatch),
        0u, 0u};
    auto const callOp         = sch.opcodeByMnemonic("call");
    auto const callIndirectOp = sch.opcodeByMnemonic("call_indirect_via_extern");
    ::dss::Lir const& lir = out.result.lir;
    if (lir.moduleFuncCount() < 2) return out;
    ::dss::LirFuncId const callerFn = lir.funcAt(1);
    ::dss::LirBlockId const entry   = lir.funcEntry(callerFn);
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        auto const op = lir.instOpcode(lir.blockInstAt(entry, i));
        if (callOp && op == *callOp)                 ++out.directCalls;
        if (callIndirectOp && op == *callIndirectOp) ++out.externCalls;
    }
    return out;
}
} // namespace

TEST(MirToLir, ExternCallOnDirectPltFormatEmitsPlainCall) {
    // direct-plt (ELF): the extern call is a PLAIN DIRECT `call` (E8 on
    // x86_64) to the linker-synthesized PLT stub — the SAME opcode as an
    // internal call — NOT the FF-15 indirect form. RED-on-disable: if
    // lowerCall ignored the dispatch (old FF-15-for-all-externs), the
    // extern call would be `call_indirect_via_extern` and externCalls==1.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ::dss::DiagnosticReporter rep;
    auto const lowered = lowerStdExternFixture(
        **target, ::dss::ExternCallDispatch::DirectPlt, rep);
    ASSERT_TRUE(lowered.result.ok) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(lowered.directCalls, 2u)
        << "under direct-plt BOTH the extern and the internal call use the "
           "plain `call` opcode (direct branch; the linker's PLT stub does "
           "the GOT indirection)";
    EXPECT_EQ(lowered.externCalls, 0u)
        << "direct-plt must NOT emit call_indirect_via_extern (FF 15) — that "
           "would dereference the ELF PLT stub's code bytes as a pointer";
}

// ARM64 + direct-plt call-WITH-RESULT lowering (D-LK10-ENTRY-ARM64-FFI-ISEL).
// The extern-call SELECTION is arch-AGNOSTIC (`lowerCall` has no target
// branch); this pin proves the FULL ARM64 lowering of a call-with-result
// completes with NO L_RequiredLirOpcodeMissing — the gap a prior cycle hit
// closed once the ARM64 `lea` (ADRP+ADD) landed (the GlobalAddr callee
// materialization). Same fixture as the x86_64 direct-plt pin, under the
// arm64 schema + AAPCS64.
TEST(MirToLir, Arm64ExternCallWithResultLowersUnderDirectPlt) {
    auto target = ::dss::TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    ::dss::DiagnosticReporter rep;
    auto const lowered = lowerStdExternFixture(
        **target, ::dss::ExternCallDispatch::DirectPlt, rep,
        ::dss::CallConv::CcAAPCS64);
    ASSERT_TRUE(lowered.result.ok) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(lowered.directCalls, 2u)
        << "ARM64 direct-plt: both the extern and internal call lower to the "
           "plain `call` (BL) opcode";
    EXPECT_EQ(lowered.externCalls, 0u)
        << "ARM64 has no call_indirect_via_extern — forced onto direct-plt";
}

TEST(MirToLir, ExternImportsWithNoDispatchFailLoud) {
    // No silent default: a module with extern imports under a format that
    // declared NO externCallDispatch (nullopt) fails loud at lowering —
    // a silent fall-through to either shape would miscompile one import
    // model. RED-on-disable if the ctor guard's nullopt arm is removed.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ::dss::DiagnosticReporter rep;
    auto const lowered = lowerStdExternFixture(**target, std::nullopt, rep);
    EXPECT_FALSE(lowered.result.ok)
        << "extern imports under a nullopt dispatch must NOT lower cleanly";
    EXPECT_GT(rep.errorCount(), 0u)
        << "the no-dispatch guard must fire (L_RequiredLirOpcodeMissing)";
}

TEST(MirToLir, IndirectSlotWithoutOpcodeFailsLoud) {
    // Symmetric guard arm: `indirect-slot` requires a
    // `call_indirect_via_extern` opcode. ARM64 declares none, so an
    // indirect-slot format on ARM64 fails loud rather than silently
    // mis-lowering. RED-on-disable if the indirect-slot guard arm is removed.
    auto target = ::dss::TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    ::dss::DiagnosticReporter rep;
    auto const lowered = lowerStdExternFixture(
        **target, ::dss::ExternCallDispatch::IndirectSlot, rep);
    EXPECT_FALSE(lowered.result.ok)
        << "indirect-slot on a target lacking call_indirect_via_extern must "
           "NOT lower cleanly";
    EXPECT_GT(rep.errorCount(), 0u)
        << "the indirect-slot-missing-opcode guard must fire";
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

// ── FC3 width gate + width axis (D-CSUBSET-32BIT-ALU-FORMS) ─────────────
//
// c1 erected the gate (every shipped integer encoding was 64-bit-wide;
// narrower DEFINED semantics failed loud). c2 ships the 32-bit forms +
// the width axis: I32/U32 now LOWER, carrying `kLirInstFlagWidth32` on
// the LIR instruction so the encoder picks the 32-bit variants. The
// still-unencoded widths (8/16/128-bit + Bool-ALU) stay gated, and the
// conversion mnemonics gate the (source/result) shapes they do not
// realize.

namespace {

// Hand-build `fn(k, k) -> k { return a OP b; }` and lower it.
struct GuardProbe {
    ::dss::DiagnosticReporter rep;
    ::dss::Lir                lir;
    bool ok = false;
};

[[nodiscard]] GuardProbe lowerBinaryProbe(::dss::TypeKind k,
                                          ::dss::MirOpcode op) {
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    EXPECT_TRUE(target.has_value());
    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const ty = interner.primitive(k);
    std::array<::dss::TypeId, 2> params{ty, ty};
    auto const fnSig = interner.fnSig(params, ty, ::dss::CallConv::CcSysV);
    ::dss::MirBuilder mb;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb =
        mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const a = mb.addArg(0, ty);
    ::dss::MirInstId const b = mb.addArg(1, ty);
    std::array<::dss::MirInstId, 2> ops{a, b};
    ::dss::MirInstId const r = mb.addInst(op, ops, ty);
    mb.addReturn(r);
    ::dss::Mir m = std::move(mb).finish();
    GuardProbe probe;
    auto result = ::dss::lowerToLir(m, **target, interner, probe.rep);
    probe.ok  = result.ok;
    probe.lir = std::move(result.lir);
    return probe;
}

[[nodiscard]] bool sawAnchor(::dss::DiagnosticReporter const& rep,
                             std::string_view anchor) {
    for (auto const& d : rep.all()) {
        if (d.actual.find(anchor) != std::string::npos) return true;
    }
    return false;
}

// Collect the LIR width (in bits) of every instruction whose mnemonic
// matches, across the whole module. Empty = mnemonic never emitted.
[[nodiscard]] std::vector<unsigned>
widthsOfMnemonic(::dss::Lir const& lir, ::dss::TargetSchema const& sch,
                 std::string_view mnemonic) {
    std::vector<unsigned> out;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        auto const fn = lir.funcAt(f);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            auto const bb = lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(bb); ++ii) {
                auto const inst  = lir.blockInstAt(bb, ii);
                auto const* info = sch.opcodeInfo(lir.instOpcode(inst));
                if (info != nullptr && info->mnemonic == mnemonic) {
                    out.push_back(::dss::lirInstWidthBits(
                        lir.instFlags(inst)));
                }
            }
        }
    }
    return out;
}

} // namespace

TEST(MirToLir, U32AddLowersCarryingTheThirtyTwoBitWidthFlag) {
    // c2 FLIP of the c1 gate test `U32AddFailsLoudCitingNativeWidthGuard`:
    // U32 arithmetic now lowers, and the add instruction must carry the
    // 32-bit width flag — the lever the encoder's variant guards match
    // on. Width-perturbation red-on-disable: force `widthFlagsForType`
    // to 0 and this pin goes RED (and the u32_wraparound corpus exit
    // flips 42→7 — defined wraparound computed 64-wide).
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto probe = lowerBinaryProbe(::dss::TypeKind::U32, ::dss::MirOpcode::Add);
    EXPECT_TRUE(probe.ok)
        << "U32 add must lower — c2 shipped the 32-bit ALU forms";
    auto const widths = widthsOfMnemonic(probe.lir, **target, "add");
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_EQ(widths[0], 32u)
        << "the U32 add must carry kLirInstFlagWidth32 so the encoder "
           "selects the 32-bit (no-REX.W) variant";
}

TEST(MirToLir, I64AddKeepsTheSixtyFourBitDefaultWidth) {
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto probe = lowerBinaryProbe(::dss::TypeKind::I64, ::dss::MirOpcode::Add);
    EXPECT_TRUE(probe.ok);
    auto const widths = widthsOfMnemonic(probe.lir, **target, "add");
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_EQ(widths[0], 64u)
        << "64-bit-typed code must keep the default width (back-compat: "
           "it matches the pre-existing width-keyed 64 variants)";
}

TEST(MirToLir, I32SignedDivisionThreadsWidthThroughPreAndCoreOps) {
    // The implicit-register pair must select the 32-bit forms COHERENTLY:
    // an I32 SDiv lowers pre=cqo (encoding CDQ at width 32) + core=
    // idiv_op (32-bit F7 /7) — a 64-bit CQO paired with a 32-bit IDIV
    // (or vice versa) would sign-extend the WRONG register half.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto probe = lowerBinaryProbe(::dss::TypeKind::I32, ::dss::MirOpcode::SDiv);
    EXPECT_TRUE(probe.ok);
    auto const preWidths  = widthsOfMnemonic(probe.lir, **target, "cqo");
    auto const coreWidths = widthsOfMnemonic(probe.lir, **target, "idiv_op");
    ASSERT_EQ(preWidths.size(), 1u);
    ASSERT_EQ(coreWidths.size(), 1u);
    EXPECT_EQ(preWidths[0], 32u);
    EXPECT_EQ(coreWidths[0], 32u);
}

TEST(MirToLir, U16DivAndI16MulAlsoGate) {
    EXPECT_FALSE(lowerBinaryProbe(::dss::TypeKind::U16,
                                  ::dss::MirOpcode::UDiv).ok);
    EXPECT_FALSE(lowerBinaryProbe(::dss::TypeKind::I16,
                                  ::dss::MirOpcode::Mul).ok);
}

TEST(MirToLir, U32CompareLowersWithThirtyTwoBitCmpWidth) {
    // c2 FLIP of the c1 gate test `U32CompareOperandsGate`: a U32
    // compare now lowers, and the `cmp` instruction's width follows
    // the OPERANDS' type (the result is Bool — width 64 would read
    // the sign-garbage upper bits of a bitcast-from-negative operand).
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const u32  = interner.primitive(::dss::TypeKind::U32);
    auto const b1   = interner.primitive(::dss::TypeKind::Bool);
    std::array<::dss::TypeId, 2> params{u32, u32};
    auto const fnSig = interner.fnSig(params, b1, ::dss::CallConv::CcSysV);
    ::dss::MirBuilder mb;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb =
        mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const a = mb.addArg(0, u32);
    ::dss::MirInstId const b = mb.addArg(1, u32);
    std::array<::dss::MirInstId, 2> ops{a, b};
    ::dss::MirInstId const r =
        mb.addInst(::dss::MirOpcode::ICmpUgt, ops, b1);
    mb.addReturn(r);
    ::dss::Mir m = std::move(mb).finish();
    ::dss::DiagnosticReporter rep;
    auto result = ::dss::lowerToLir(m, **target, interner, rep);
    EXPECT_TRUE(result.ok);
    auto const widths = widthsOfMnemonic(result.lir, **target, "cmp");
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_EQ(widths[0], 32u);
    // The Bool-producing setcc/zext pair stays width-default.
    auto const zextWidths = widthsOfMnemonic(result.lir, **target, "zext");
    ASSERT_EQ(zextWidths.size(), 1u);
    EXPECT_EQ(zextWidths[0], 64u);
}

// ── audit-residue sweep c1: the FUSED ICmp+CondBr cmp width pin ─────────
// D-AUDIT-FUSED-CMP-WIDTH-PIN: lowerCondBr's ICmp-fusion arm emits its
// OWN `cmp lhs, rhs` (immediately before the jcc) — a SEPARATE emit
// site from lowerICmp's value-path cmp (which the U32Compare… pin
// above covers). The fused cmp's width must follow the ICmp OPERANDS'
// type, the same FC3-c2 rule. Because the 32-bit producers zero the
// upper 32 bits (x86 no-REX.W auto-zero-extend / arm64 W-forms), a
// width-64 regression at that ONE site silently mis-branches every
// `int x = a - b; if (x < 0)` (a cmp64 reads 0x00000000FFFFFFxx as
// POSITIVE) while the value-path width pins stay green — the highest-
// traffic compare shape in C. Runtime witness:
// examples/c-subset/fused_negative_compare (42 ↔ 7 exit divergence).
// The shape below makes the CondBr the ICmpSlt's ONLY consumer, so the
// fusion precondition holds; the jcc payload carrying Slt (not the
// non-fused arm's Ne) PROVES the fusion arm actually fired.

namespace {

struct FusedCmpShape {
    bool          lowered    = false;  // lowerToLir ok
    bool          shapeFound = false;  // entry block ends `cmp ; jcc`
    unsigned      fusedCmpWidthBits = 0;
    std::uint32_t jccPayload = 0;
};

// f(k a, k b) { if (a < b) return 7; return 9; } — ICmpSlt's only
// consumer is the CondBr (the fusion precondition; mirrors the FCmp
// fusion tests' buildFcmpBranchFn with integer operands).
[[nodiscard]] FusedCmpShape
lowerFusedSltShape(std::string_view targetName, ::dss::TypeKind k) {
    FusedCmpShape out;
    auto target = ::dss::TargetSchema::loadShipped(targetName);
    if (!target.has_value()) {
        ADD_FAILURE() << "loadShipped(" << targetName << ") failed";
        return out;
    }
    std::array<::dss::TypeKind, 2> paramKinds{k, k};
    auto syn = test_support::buildSyntheticFn(
        paramKinds, ::dss::TypeKind::I32,
        [&](::dss::MirBuilder& mb, ::dss::TypeInterner& in,
            std::span<::dss::TypeId const> params, ::dss::TypeId retT) {
            ::dss::MirInstId const a = mb.addArg(0, params[0]);
            ::dss::MirInstId const b = mb.addArg(1, params[1]);
            std::array<::dss::MirInstId, 2> ops{a, b};
            ::dss::MirInstId const c = mb.addInst(
                ::dss::MirOpcode::ICmpSlt, ops,
                in.primitive(::dss::TypeKind::Bool));
            ::dss::MirBlockId const thenB =
                mb.createBlock(::dss::StructCfMarker::IfThen);
            ::dss::MirBlockId const elseB =
                mb.createBlock(::dss::StructCfMarker::IfElse);
            mb.addCondBr(c, thenB, elseB);
            ::dss::MirLiteralValue v7;
            v7.value = static_cast<std::int64_t>(7);
            v7.core  = ::dss::TypeKind::I32;
            mb.beginBlock(thenB);
            mb.addReturn(mb.addConst(v7, retT));
            ::dss::MirLiteralValue v9;
            v9.value = static_cast<std::int64_t>(9);
            v9.core  = ::dss::TypeKind::I32;
            mb.beginBlock(elseB);
            mb.addReturn(mb.addConst(v9, retT));
        });
    ::dss::DiagnosticReporter rep;
    auto const result =
        ::dss::lowerToLir(syn.mir, **target, syn.interner, rep);
    if (!result.ok) return out;
    out.lowered = true;
    auto const& sch = **target;
    ::dss::Lir const& lir = result.lir;
    auto const jccOp = sch.opcodeByMnemonic("jcc");
    auto const cmpOp = sch.opcodeByMnemonic("cmp");
    if (!jccOp.has_value() || !cmpOp.has_value()) return out;
    ::dss::LirBlockId const entry = lir.funcEntry(lir.funcAt(0));
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        ::dss::LirInstId const inst = lir.blockInstAt(entry, i);
        if (lir.instOpcode(inst) != *jccOp) continue;
        // The fused cmp is emitted IMMEDIATELY before the jcc (phi-edge
        // moves precede it; none exist in this shape).
        if (i == 0) return out;
        ::dss::LirInstId const prev = lir.blockInstAt(entry, i - 1);
        if (lir.instOpcode(prev) != *cmpOp) return out;
        out.shapeFound        = true;
        out.fusedCmpWidthBits =
            ::dss::lirInstWidthBits(lir.instFlags(prev));
        out.jccPayload        = lir.instPayload(inst);
        return out;
    }
    return out;
}

} // namespace

TEST(MirToLir, FusedI32CompareCondBrCmpCarriesThirtyTwoBitWidth) {
    // The width-threading site is TARGET-BLIND (shared lowering), so
    // pin both shipped targets — each schema's cmp carries width-keyed
    // variants (x86 no-REX.W 39 / arm64 SUBS W-form 0x6B00001F) that
    // the assembler selects from this flag.
    for (auto const* targetName : {"x86_64", "arm64"}) {
        SCOPED_TRACE(targetName);
        auto const s = lowerFusedSltShape(targetName, ::dss::TypeKind::I32);
        ASSERT_TRUE(s.lowered);
        ASSERT_TRUE(s.shapeFound)
            << "entry block must end `cmp ; jcc` — the fused shape";
        EXPECT_EQ(s.jccPayload,
                  static_cast<std::uint32_t>(::dss::TargetCondCode::Slt))
            << "jcc payload must be the ICmpSlt cc — proves the FUSION "
               "arm fired (the non-fused arm carries Ne)";
        EXPECT_EQ(s.fusedCmpWidthBits, 32u)
            << "the FUSED cmp over I32 operands must read 32 bits — "
               "width-64 here reads zero-extended upper bits and calls "
               "a negative int positive (D-AUDIT-FUSED-CMP-WIDTH-PIN)";
    }
}

TEST(MirToLir, FusedI64CompareCondBrCmpKeepsSixtyFourBitWidth) {
    // The I64 contrast pin: native-width operands must keep the
    // 64-bit default (flags 0) — guards against over-rotating the fix
    // into a blanket width-32.
    for (auto const* targetName : {"x86_64", "arm64"}) {
        SCOPED_TRACE(targetName);
        auto const s = lowerFusedSltShape(targetName, ::dss::TypeKind::I64);
        ASSERT_TRUE(s.lowered);
        ASSERT_TRUE(s.shapeFound)
            << "entry block must end `cmp ; jcc` — the fused shape";
        EXPECT_EQ(s.jccPayload,
                  static_cast<std::uint32_t>(::dss::TargetCondCode::Slt));
        EXPECT_EQ(s.fusedCmpWidthBits, 64u)
            << "I64-operand fused cmp must stay width-64";
    }
}

// ── FC3 c2: conversion-shape gates ──────────────────────────────────────
// Each conversion mnemonic has an INHERENT (source, dest) width pair;
// the shapes with no realization must fail loud at MIR→LIR, never let
// first-match pick a wrong-width encoding.

namespace {

// Hand-build `fn(src) -> dst { return (dst)a; }` with the given cast
// opcode and lower it.
[[nodiscard]] GuardProbe lowerCastProbe(::dss::MirOpcode castOp,
                                        ::dss::TypeKind src,
                                        ::dss::TypeKind dst) {
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    EXPECT_TRUE(target.has_value());
    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    auto const srcTy = interner.primitive(src);
    auto const dstTy = interner.primitive(dst);
    std::array<::dss::TypeId, 1> params{srcTy};
    auto const fnSig = interner.fnSig(params, dstTy, ::dss::CallConv::CcSysV);
    ::dss::MirBuilder mb;
    mb.addFunction(fnSig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb =
        mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const a = mb.addArg(0, srcTy);
    std::array<::dss::MirInstId, 1> ops{a};
    ::dss::MirInstId const r = mb.addInst(castOp, ops, dstTy);
    mb.addReturn(r);
    ::dss::Mir m = std::move(mb).finish();
    GuardProbe probe;
    auto result = ::dss::lowerToLir(m, **target, interner, probe.rep);
    probe.ok  = result.ok;
    probe.lir = std::move(result.lir);
    return probe;
}

} // namespace

TEST(MirToLir, TruncToThirtyTwoLowersCarryingWidthFlag) {
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto probe = lowerCastProbe(::dss::MirOpcode::Trunc,
                                ::dss::TypeKind::I64, ::dss::TypeKind::U32);
    EXPECT_TRUE(probe.ok)
        << "Trunc to a 32-bit result is realized (x86 mov r32,r32 = "
           "C's mod-2^32 conversion)";
    auto const widths = widthsOfMnemonic(probe.lir, **target, "trunc");
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_EQ(widths[0], 32u)
        << "the trunc inst must carry the 32-bit width — its encoding "
           "variant is width-keyed so non-32 trunc matches nothing";
}

TEST(MirToLir, TruncToSixteenAndEightNowRealizedViaPromotedMov) {
    // D-CSUBSET-SUBNATIVE-ALU-FORMS (was TruncToSixteenAndEightStayGated): a
    // Trunc whose result is I16/U16/I8/U8 routes through registerOpWidthFlags →
    // the PROMOTED width-32 `mov` (low bits kept); the narrowing realizes at the
    // byte/half-exact consumer. RED-ON-DISABLE: drop I16/U8 from the Trunc result
    // gate and these flip back to fail-loud (D-CSUBSET-32BIT-ALU-FORMS).
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto p16 = lowerCastProbe(::dss::MirOpcode::Trunc,
                              ::dss::TypeKind::I64, ::dss::TypeKind::I16);
    EXPECT_TRUE(p16.ok) << "Trunc to I16 realized via the promoted width-32 mov";
    auto const widths = widthsOfMnemonic(p16.lir, **target, "trunc");
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_EQ(widths[0], 32u)
        << "the narrowing Trunc carries the PROMOTED width 32 (low bits kept)";
    EXPECT_TRUE(lowerCastProbe(::dss::MirOpcode::Trunc,
                               ::dss::TypeKind::I64,
                               ::dss::TypeKind::U8).ok)
        << "Trunc to U8 likewise realized";
}

TEST(MirToLir, ZExtFromU32SourceLowersCarryingSourceWidth) {
    // FC3.5 sweep-c1 FLIP of `ZExtFromU32SourceStaysGated`
    // (D-CSUBSET-ZEXT-32-TO-64 closed): the U32→U64 zero-extend is now
    // realized — the ZExt arm threads the SOURCE type's width onto the
    // LIR inst so the encoder picks the width-32 form (x86 `mov r32,
    // r32`, auto-zero-extending) instead of the Bool byte-widener
    // (movzx r64, r/m8 — which would silently read ONE byte of the
    // U32). Red-on-disable: revert the source-width threading in the
    // ZExt dispatch arm and this width pin goes RED (the inst falls
    // back to the result-type rule = width 64).
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto probe = lowerCastProbe(::dss::MirOpcode::ZExt,
                                ::dss::TypeKind::U32, ::dss::TypeKind::U64);
    EXPECT_TRUE(probe.ok)
        << "ZExt from U32 is realized (x86 mov r32,r32 width-32 form): "
        << (probe.rep.all().empty() ? "" : probe.rep.all()[0].actual);
    auto const widths = widthsOfMnemonic(probe.lir, **target, "zext");
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_EQ(widths[0], 32u)
        << "the U32-source zext must carry the SOURCE width (32) — "
           "the encoding variants are width-keyed";
}

TEST(MirToLir, ZExtFromBoolKeepsDefaultWidthAndNarrowSourcesNowRealized) {
    // The Bool 0/1 source keeps the width-default byte widener (existing
    // consumers — lowerICmp's setcc→zext pair — stay byte-identical). The
    // narrow U16 source is NOW realized (D-CSUBSET-SUBNATIVE-ALU-FORMS):
    // it carries the SOURCE width 16, selecting the width-keyed movzx r/m16
    // (x86) / UXTH (arm64). RED-ON-DISABLE: drop U16 from the ZExt source
    // gate and the u16 probe fails loud again.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto boolProbe = lowerCastProbe(::dss::MirOpcode::ZExt,
                                    ::dss::TypeKind::Bool,
                                    ::dss::TypeKind::I64);
    EXPECT_TRUE(boolProbe.ok);
    auto const widths = widthsOfMnemonic(boolProbe.lir, **target, "zext");
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_EQ(widths[0], 64u)
        << "Bool-source zext keeps the 64-default (movzx byte form)";
    auto u16Probe = lowerCastProbe(::dss::MirOpcode::ZExt,
                                   ::dss::TypeKind::U16,
                                   ::dss::TypeKind::U64);
    EXPECT_TRUE(u16Probe.ok)
        << "ZExt from U16 realized (movzx r/m16 / UXTH)";
    auto const u16Widths = widthsOfMnemonic(u16Probe.lir, **target, "zext");
    ASSERT_EQ(u16Widths.size(), 1u);
    EXPECT_EQ(u16Widths[0], 16u)
        << "the U16-source zext carries the SOURCE width 16 (width-keyed)";
}

TEST(MirToLir, SExtFromI16AndI32SourcesBothRealized) {
    // D-CSUBSET-SUBNATIVE-ALU-FORMS (was SExtFromI16SourceStaysGated…): an I16
    // source now selects the width-16 movsx r/m16 / SXTH form; I32 keeps movsxd
    // / SXTW (the 32-bit window). RED-ON-DISABLE: drop I16 from the SExt source
    // gate and the I16 probe fails loud again.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto p16 = lowerCastProbe(::dss::MirOpcode::SExt,
                              ::dss::TypeKind::I16, ::dss::TypeKind::I64);
    EXPECT_TRUE(p16.ok) << "SExt from I16 realized (movsx r/m16 / SXTH)";
    auto const w16 = widthsOfMnemonic(p16.lir, **target, "sext");
    ASSERT_EQ(w16.size(), 1u);
    EXPECT_EQ(w16[0], 16u) << "the I16-source sext carries the SOURCE width 16";
    EXPECT_TRUE(lowerCastProbe(::dss::MirOpcode::SExt,
                               ::dss::TypeKind::I32,
                               ::dss::TypeKind::I64).ok);
}

// ─── D-CSUBSET-CHAR-STRING-VALUE-CODEGEN + D-CSUBSET-CHAR-INT-WIDENING ───
// The width-flag pins for `char` value codegen. Each is RED-ON-DISABLE on a
// specific derivation in mir_to_lir.cpp; together with the byte-encoding
// (tests/asm) and the runtime corpus (examples/c-subset/char_value), they
// cover the char byte forms flag → bytes → exit end-to-end. Width flags are
// target-blind (set in MIR→LIR), so x86_64 is a sufficient witness here.

TEST(MirToLir, CharToIntSExtCarriesByteSourceWidth8) {
    // char→int widening selects the BYTE source form (movsx r/m8 / SXTB),
    // discriminated by the source width-8 flag. RED-ON-DISABLE: drop the
    // Char arm of widthFlagsForType (source → 8) or the SExt gate's Char
    // arm → either the gate rejects (probe !ok) or the width is not 8.
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto probe = lowerCastProbe(::dss::MirOpcode::SExt,
                                ::dss::TypeKind::Char, ::dss::TypeKind::I32);
    ASSERT_TRUE(probe.ok)
        << "char→int SExt is realized (movsx r/m8): "
        << (probe.rep.all().empty() ? "" : probe.rep.all()[0].actual);
    auto const widths = widthsOfMnemonic(probe.lir, **target, "sext");
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_EQ(widths[0], 8u)
        << "the Char-source SExt must carry the byte SOURCE width (8) so the "
           "encoder picks movsx r/m8, not movsxd r/m32 (which sign-extends "
           "from bit 31 — the WRONG bit for a 1-byte char)";
}

TEST(MirToLir, IntToCharTruncLowersAtPromotedWidth32) {
    // int→char (D-CSUBSET-CHAR-INT-WIDENING, the narrowing direction) is
    // realized through the width-32 `mov r32,r32` (registerOpWidthFlags
    // collapses the char byte width → 32; the narrowing-to-1-byte is lazy,
    // at the next byte-exact consumer). RED-ON-DISABLE: drop the Char arm of
    // the Trunc gate → probe !ok; drop the registerOpWidthFlags collapse →
    // the trunc carries width-8, which has NO trunc variant (the width pin
    // below would read 8, and the asm encoder would fail loud).
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto probe = lowerCastProbe(::dss::MirOpcode::Trunc,
                                ::dss::TypeKind::I32, ::dss::TypeKind::Char);
    ASSERT_TRUE(probe.ok)
        << "int→char Trunc is realized at the promoted width-32 form: "
        << (probe.rep.all().empty() ? "" : probe.rep.all()[0].actual);
    auto const widths = widthsOfMnemonic(probe.lir, **target, "trunc");
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_EQ(widths[0], 32u)
        << "a Char-result Trunc plumbs at the promoted width 32 (mov r32,r32), "
           "never the byte width — there is no 8-bit trunc form";
}

TEST(MirToLir, CharLoadIsByteExactWidth8) {
    // A char LOAD must be byte-exact (movzx r/m8 / LDURB) — a 64-bit load of
    // a 1-byte string/array element over-reads 7 bytes. RED-ON-DISABLE: drop
    // the Char arm of memAccessWidthFlags → the load carries width-64.
    std::array<::dss::TypeKind, 1> paramKinds{::dss::TypeKind::Ptr};
    auto syn = ::dss::test_support::buildSyntheticFn(
        paramKinds, ::dss::TypeKind::Char,
        [](::dss::MirBuilder& mb, ::dss::TypeInterner&,
           std::span<::dss::TypeId const> params, ::dss::TypeId retT) {
            ::dss::MirInstId const p = mb.addArg(0, params[0]);
            std::array<::dss::MirInstId, 1> ops{p};
            ::dss::MirInstId const r =
                mb.addInst(::dss::MirOpcode::Load, ops, retT);  // retT = Char
            mb.addReturn(r);
        });
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(syn.mir, **target, syn.interner, rep);
    ASSERT_TRUE(result.ok)
        << (rep.all().empty() ? "" : rep.all()[0].actual);
    auto const widths = widthsOfMnemonic(result.lir, **target, "load");
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_EQ(widths[0], 8u) << "a char load must be byte-exact (width 8)";
}

TEST(MirToLir, CharStoreIsByteExactWidth8) {
    // A char STORE writes EXACTLY 1 byte (mov r/m8,r8 / STURB) — a 64-bit
    // store clobbers 7 neighbours. The width keys on the stored VALUE's type
    // (operand[0]). RED-ON-DISABLE: drop the Char arm of memAccessWidthFlags.
    std::array<::dss::TypeKind, 2> paramKinds{::dss::TypeKind::Ptr,
                                              ::dss::TypeKind::Char};
    auto syn = ::dss::test_support::buildSyntheticFn(
        paramKinds, ::dss::TypeKind::I32,
        [](::dss::MirBuilder& mb, ::dss::TypeInterner&,
           std::span<::dss::TypeId const> params, ::dss::TypeId retT) {
            ::dss::MirInstId const p = mb.addArg(0, params[0]);  // Ptr base
            ::dss::MirInstId const c = mb.addArg(1, params[1]);  // Char value
            std::array<::dss::MirInstId, 2> ops{c, p};           // [value, base]
            // Store yields no value; its byte width keys on the stored
            // VALUE's type (operand[0] = the Char arg), not a result type.
            mb.addInst(::dss::MirOpcode::Store, ops, ::dss::InvalidType);
            ::dss::MirLiteralValue lv;
            lv.value = static_cast<std::int64_t>(0);
            lv.core  = ::dss::TypeKind::I32;
            ::dss::MirInstId const z = mb.addConst(lv, retT);
            mb.addReturn(z);
        });
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(syn.mir, **target, syn.interner, rep);
    ASSERT_TRUE(result.ok)
        << (rep.all().empty() ? "" : rep.all()[0].actual);
    auto const widths = widthsOfMnemonic(result.lir, **target, "store");
    ASSERT_EQ(widths.size(), 1u);
    EXPECT_EQ(widths[0], 8u) << "a char store must be byte-exact (width 8)";
}

TEST(MirToLir, CharConstMaterializesAtPromotedWidthNotByte) {
    // A char CONSTANT materializes through a register mov at the PROMOTED
    // width (registerOpWidthFlags collapses char → 32) — there is no 8-bit
    // mov-imm form, and a char value lives low-bits-only in a full register.
    // RED-ON-DISABLE: revert lowerConst to widthFlagsForType → the const mov
    // carries width-8 (no variant → a stray width-8 plumbing mov appears).
    auto syn = ::dss::test_support::buildSyntheticFn(
        std::span<::dss::TypeKind const>{}, ::dss::TypeKind::Char,
        [](::dss::MirBuilder& mb, ::dss::TypeInterner&,
           std::span<::dss::TypeId const>, ::dss::TypeId retT) {
            ::dss::MirLiteralValue lv;
            lv.value = static_cast<std::int64_t>(65);  // 'A'
            lv.core  = ::dss::TypeKind::Char;
            ::dss::MirInstId const k = mb.addConst(lv, retT);  // retT = Char
            mb.addReturn(k);
        });
    auto target = ::dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ::dss::DiagnosticReporter rep;
    auto const result = ::dss::lowerToLir(syn.mir, **target, syn.interner, rep);
    ASSERT_TRUE(result.ok)
        << (rep.all().empty() ? "" : rep.all()[0].actual);
    auto const widths = widthsOfMnemonic(result.lir, **target, "mov");
    ASSERT_FALSE(widths.empty()) << "the char const must materialize via a mov";
    for (unsigned const w : widths) {
        EXPECT_NE(w, 8u)
            << "a char const/plumbing mov must NOT carry the byte width — it "
               "promotes to a register width (32/64); width-8 belongs only to "
               "the byte load/store/sext forms";
    }
}

TEST(MirToLir, CSubsetSourceTypesThreadWidthToLirFlags) {
    // SOURCE-tier width-threading pin: the c-subset front end's
    // `unsigned int`/`int` (32-bit) vs `long long` (64-bit) typing
    // must arrive at the LIR width flag — composing with the byte
    // pins (tests/asm/test_asm_width_axis.cpp: flag → bytes) and the
    // runtime corpus (u32_wraparound: program → exit), this covers
    // source → bytes end-to-end.
    {
        auto L = lowerCSubsetToLir(
            "unsigned int f(unsigned int a, unsigned int b) "
            "{ return a + b; }");
        assertUpstreamClean(L);
        auto const w = widthsOfMnemonic(L.lir.lir, *L.target, "add");
        ASSERT_EQ(w.size(), 1u);
        EXPECT_EQ(w[0], 32u);
    }
    {
        // Plain `int` now ALSO computes at 32 bits — true C int
        // semantics (the c1 64-wide exemption was conforming via
        // signed-overflow UB; the 32-bit forms are exact).
        auto L = lowerCSubsetToLir(
            "int g(int a, int b) { return a + b; }");
        assertUpstreamClean(L);
        auto const w = widthsOfMnemonic(L.lir.lir, *L.target, "add");
        ASSERT_EQ(w.size(), 1u);
        EXPECT_EQ(w[0], 32u);
    }
    {
        auto L = lowerCSubsetToLir(
            "long long h(long long a, long long b) { return a + b; }");
        assertUpstreamClean(L);
        auto const w = widthsOfMnemonic(L.lir.lir, *L.target, "add");
        ASSERT_EQ(w.size(), 1u);
        EXPECT_EQ(w[0], 64u);
    }
    {
        // FC3.5 (D-CSUBSET-ZEXT-32-TO-64): the C-source widening
        // conversion `unsigned int` → `unsigned long long` mints a
        // ZExt whose LIR width is the SOURCE's 32 — the front-end
        // composition of ZExtFromU32SourceLowersCarryingSourceWidth.
        auto L = lowerCSubsetToLir(
            "unsigned long long w(unsigned int a) "
            "{ return (unsigned long long)a; }");
        assertUpstreamClean(L);
        auto const w = widthsOfMnemonic(L.lir.lir, *L.target, "zext");
        ASSERT_EQ(w.size(), 1u);
        EXPECT_EQ(w[0], 32u)
            << "the U32→U64 widening's zext must carry the source "
               "width so the encoder picks mov r32,r32 over movzx";
    }
}

TEST(MirToLir, NativeWidthsStayAllowedThroughTheGate) {
    // U64 unsigned compute IS native-width (defined wraparound holds in
    // 64-bit registers) and I32 signed rides the signed-overflow-is-UB
    // exemption — the existing corpus' status quo. The gate must not
    // overfire on either.
    EXPECT_TRUE(lowerBinaryProbe(::dss::TypeKind::U64,
                                 ::dss::MirOpcode::Add).ok);
    EXPECT_TRUE(lowerBinaryProbe(::dss::TypeKind::I32,
                                 ::dss::MirOpcode::Add).ok);
    EXPECT_TRUE(lowerBinaryProbe(::dss::TypeKind::I64,
                                 ::dss::MirOpcode::Mul).ok);
}

TEST(MirToLir, F32AddLowersAndExoticFloatWidthsStayWalled) {
    // FC3.5 sweep-c2 (D-CSUBSET-F32-CODEGEN CLOSED): F32 arithmetic now
    // LOWERS through the width axis (ADDSS / arm64 FADD-S). The wall
    // narrows to the genuinely-unencoded widths: F16/F128 must still
    // REJECT via the width guard, never first-match a wrong-width form
    // (D-TARGET-ENCODING-WIDTH-GUARD).
    EXPECT_TRUE(lowerBinaryProbe(::dss::TypeKind::F32,
                                 ::dss::MirOpcode::FAdd).ok);
    EXPECT_TRUE(lowerBinaryProbe(::dss::TypeKind::F64,
                                 ::dss::MirOpcode::FDiv).ok)
        << "fdiv gained DIVSD — the NaN-construction path";
    EXPECT_TRUE(lowerBinaryProbe(::dss::TypeKind::F32,
                                 ::dss::MirOpcode::FDiv).ok)
        << "fdiv gained DIVSS";
    auto probe16 = lowerBinaryProbe(::dss::TypeKind::F16,
                                    ::dss::MirOpcode::FAdd);
    EXPECT_FALSE(probe16.ok);
    EXPECT_TRUE(sawAnchor(probe16.rep, "D-TARGET-ENCODING-WIDTH-GUARD"));
    auto probe128 = lowerBinaryProbe(::dss::TypeKind::F128,
                                     ::dss::MirOpcode::FDiv);
    EXPECT_FALSE(probe128.ok);
    EXPECT_TRUE(sawAnchor(probe128.rep, "D-TARGET-ENCODING-WIDTH-GUARD"));
}

// ══ TLS C1 (D-CSUBSET-THREAD-LOCAL): the thread-local GlobalAddr arm ══
//
// A THREAD-LOCAL symbol's "address" is per-thread — tp + tpoff(sym),
// tpoff a link-time constant the walker resolves by bit-casting the
// SIGNED offset into symbolVa. EVERY VA-shaped materialization (the
// plain lea, the riprel fold) is therefore a silent wrong-address
// access for it. These pins fix the lowering shape (tlsbase + the
// 2-op tpoff lea), the M-5 fold exclusion (a TLS GlobalAddr+Load that
// WOULD fold if the symbol were ordinary must NOT, while an identical
// non-TLS control DOES), and the fail-loud ladder for the un-landed
// legs (no tlsAccess block → K_FormatLacksThreadLocalSupport 0x8015;
// local-exec on a target without the tlsbase row → the arm64
// L_RequiredLirOpcodeMissing gate; pe-indexed/macho-tlv → 0x8015
// until their cycles land).

namespace {

// Module: thread-local F64 global (symbol 500) + an IDENTICAL non-TLS
// control global (symbol 501); one function loading BOTH through
// single-use GlobalAddrs. F64 deliberately: the x86_64 FPR load
// (movsd_load) DECLARES the [symbol] riprel variant, so the control's
// GlobalAddr+Load pair FOLDS — which makes the thread-local twin the
// exact "would fold if the symbol were non-TLS" shape M-5 pins.
struct TlsLoweredModule {
    ::dss::TypeInterner interner{::dss::CompilationUnitId{1}};
    ::dss::Mir          mir;
};

[[nodiscard]] TlsLoweredModule buildTlsPlusControlModule() {
    TlsLoweredModule out;
    auto& interner = out.interner;
    auto const f64    = interner.primitive(::dss::TypeKind::F64);
    auto const ptrF64 = interner.pointer(f64);
    auto const sig = interner.fnSig(std::span<::dss::TypeId const>{}, f64,
                                    ::dss::CallConv::CcSysV);
    ::dss::MirBuilder mb;
    ::dss::MirLiteralValue quarter; quarter.value = 0.25;
    quarter.core = ::dss::TypeKind::F64;
    ::dss::MirLiteralValue half; half.value = 0.5;
    half.core = ::dss::TypeKind::F64;
    mb.addFunction(sig, ::dss::SymbolId{1});
    (void)mb.addGlobal(f64, ::dss::SymbolId{500}, mb.literalPoolAdd(quarter),
                       ::dss::MirFuncId{}, ::dss::SymbolBinding::Global,
                       ::dss::SymbolVisibility::Default, /*isConst=*/false,
                       MirThreadStorage::PerThread);
    (void)mb.addGlobal(f64, ::dss::SymbolId{501}, mb.literalPoolAdd(half),
                       ::dss::MirFuncId{}, ::dss::SymbolBinding::Global,
                       ::dss::SymbolVisibility::Default, /*isConst=*/false,
                       MirThreadStorage::Shared);
    ::dss::MirBlockId const bb =
        mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    ::dss::MirInstId const tlsAddr =
        mb.addGlobalAddr(::dss::SymbolId{500}, ptrF64);
    ::dss::MirInstId const tlsLoadOps[] = {tlsAddr};
    ::dss::MirInstId const tlsVal =
        mb.addInst(::dss::MirOpcode::Load, tlsLoadOps, f64);
    ::dss::MirInstId const ctlAddr =
        mb.addGlobalAddr(::dss::SymbolId{501}, ptrF64);
    ::dss::MirInstId const ctlLoadOps[] = {ctlAddr};
    ::dss::MirInstId const ctlVal =
        mb.addInst(::dss::MirOpcode::Load, ctlLoadOps, f64);
    ::dss::MirInstId const addOps[] = {tlsVal, ctlVal};
    ::dss::MirInstId const sum =
        mb.addInst(::dss::MirOpcode::FAdd, addOps, f64);
    mb.addReturn(sum);
    out.mir = std::move(mb).finish();
    return out;
}

// The ELF-Linux local-exec config (the shipped elf64-x86_64-linux-exec
// values) as a hand-held struct for direct lowerToLir invocations.
[[nodiscard]] ::dss::TlsAccessInfo elfLocalExecTls() {
    ::dss::TlsAccessInfo info{};
    info.model             = ::dss::TlsAccessModel::LocalExec;
    info.segmentPrefixByte = 0x64;  // fs
    info.baseDisplacement  = 0;     // fs:[0]
    return info;
}

[[nodiscard]] bool sawDiagnosticCode(::dss::DiagnosticReporter const& rep,
                                     ::dss::DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return true;
    }
    return false;
}

} // namespace

TEST(MirToLirTls, X64ThreadLocalLowersToTlsBasePlusTpoffLea) {
    auto m = buildTlsPlusControlModule();
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto lirR = lowerToLir(m.mir, **target, m.interner, rep, {},
                           std::nullopt, std::nullopt, elfLocalExecTls());
    ASSERT_TRUE(lirR.ok) << (rep.all().empty() ? "" : rep.all()[0].actual);
    Lir const& lir = lirR.lir;

    auto const tlsbaseOp = (*target)->opcodeByMnemonic("tlsbase");
    auto const leaOp     = (*target)->opcodeByMnemonic("lea");
    ASSERT_TRUE(tlsbaseOp.has_value() && leaOp.has_value());

    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    std::optional<LirReg> tpReg;
    std::optional<LirReg> tlsAddrReg;
    bool sawTlsLea = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
        LirInstId const inst = lir.blockInstAt(bb, i);
        auto const op = lir.instOpcode(inst);
        if (op == *tlsbaseOp) {
            // tlsbase %tp — payload carries the CONFIG segment byte
            // (0x64 fs), the MemOffset operand the CONFIG tp-slot
            // displacement (0 = fs:[0]).
            EXPECT_FALSE(tpReg.has_value())
                << "exactly one tlsbase for the single TLS access";
            EXPECT_EQ(lir.instPayload(inst), 0x64u);
            auto const ops = lir.instOperands(inst);
            ASSERT_EQ(ops.size(), 1u);
            ASSERT_EQ(ops[0].kind, LirOperandKind::MemOffset);
            EXPECT_EQ(ops[0].offset, 0);
            tpReg = lir.instResult(inst);
        }
        if (op == *leaOp) {
            auto const ops = lir.instOperands(inst);
            if (ops.size() == 2 && ops[0].kind == LirOperandKind::Reg
                && ops[1].kind == LirOperandKind::SymbolRef
                && ops[1].symbolV == 500u) {
                sawTlsLea = true;
                ASSERT_TRUE(tpReg.has_value())
                    << "the tpoff lea must FOLLOW its tlsbase";
                EXPECT_EQ(ops[0].reg, *tpReg)
                    << "the lea's base must be the tlsbase-produced tp";
                tlsAddrReg = lir.instResult(inst);
            }
        }
    }
    EXPECT_TRUE(tpReg.has_value()) << "the tlsbase op must be emitted";
    EXPECT_TRUE(sawTlsLea)
        << "the 2-op [Reg, SymbolRef(500)] tpoff lea must be emitted";

    // The C-level load of the TLS value reads THROUGH the computed
    // address register — never a symbol-form load of 500.
    auto const fldOp = (*target)->regClassOpOpcode(TargetRegClass::FPR,
                                                   RegClassOp::Load);
    ASSERT_TRUE(fldOp.has_value());
    bool sawTlsBaseFormLoad = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
        LirInstId const inst = lir.blockInstAt(bb, i);
        if (lir.instOpcode(inst) != *fldOp) continue;
        auto const ops = lir.instOperands(inst);
        if (!ops.empty() && ops[0].kind == LirOperandKind::Reg
            && tlsAddrReg.has_value() && ops[0].reg == *tlsAddrReg) {
            sawTlsBaseFormLoad = true;
        }
    }
    EXPECT_TRUE(sawTlsBaseFormLoad)
        << "the TLS value load must be the [base] form reading the "
           "tlsbase+lea address";
}

TEST(MirToLirTls, X64ThreadLocalNeverRiprelFoldsWhileControlDoes) {
    // ★ audit M-5, red-on-disable: symbol 500 (thread-local) and 501
    // (control) have IDENTICAL single-use GlobalAddr+Load shapes; the
    // control MUST fold to ONE [symbol] riprel load, the TLS twin MUST
    // NOT (its symbolVa is a bit-cast tpoff — a folded riprel load
    // would read a garbage absolute address). Disabling the
    // thread-local routing/exclusion in lowerGlobalAddr /
    // globalAddrRiprelFoldsIntoLoad folds 500 too → the
    // no-symbol-form-load-of-500 assertion goes RED.
    auto m = buildTlsPlusControlModule();
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto lirR = lowerToLir(m.mir, **target, m.interner, rep, {},
                           std::nullopt, std::nullopt, elfLocalExecTls());
    ASSERT_TRUE(lirR.ok) << (rep.all().empty() ? "" : rep.all()[0].actual);
    Lir const& lir = lirR.lir;

    auto const fldOp = (*target)->regClassOpOpcode(TargetRegClass::FPR,
                                                   RegClassOp::Load);
    ASSERT_TRUE(fldOp.has_value());
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    bool controlFolded  = false;
    bool tlsSymbolLoad  = false;
    for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
        LirInstId const inst = lir.blockInstAt(bb, i);
        if (lir.instOpcode(inst) != *fldOp) continue;
        auto const ops = lir.instOperands(inst);
        if (ops.size() == 1 && ops[0].kind == LirOperandKind::SymbolRef) {
            if (ops[0].symbolV == 501u) controlFolded = true;
            if (ops[0].symbolV == 500u) tlsSymbolLoad = true;
        }
    }
    EXPECT_TRUE(controlFolded)
        << "the NON-TLS control must riprel-fold (proves the fold is "
           "live — without this, the TLS assertion below could pass "
           "vacuously because the fold itself regressed)";
    EXPECT_FALSE(tlsSymbolLoad)
        << "the THREAD-LOCAL twin must NEVER riprel-fold — a folded "
           "[rip+sym] load would resolve the bit-cast tpoff as a VA "
           "(audit M-5 silent wrong-address class)";
}

TEST(MirToLirTls, X64ThreadLocalUnderShippedPe64FormatFailsLoud0x8015) {
    // The REAL shipped pe64 format declares no tlsAccess (its TLS
    // machinery is the PE TLS cycle) — a thread-local access under it
    // must fail EXACTLY K_FormatLacksThreadLocalSupport (0x8015),
    // never lower through the ordinary (process-shared) global path.
    auto pe = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(pe.has_value());
    ASSERT_FALSE((*pe)->tlsAccess().has_value())
        << "precondition: pe64 must not declare tlsAccess yet";
    auto m = buildTlsPlusControlModule();
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto lirR = lowerToLir(m.mir, **target, m.interner, rep, {},
                           std::nullopt, std::nullopt, (*pe)->tlsAccess());
    EXPECT_FALSE(lirR.ok);
    EXPECT_TRUE(sawDiagnosticCode(
        rep, DiagnosticCode::K_FormatLacksThreadLocalSupport))
        << "a thread-local access under a tlsAccess-less format must "
           "fire K_FormatLacksThreadLocalSupport (0x8015)";
    EXPECT_TRUE(sawAnchor(rep, "D-CSUBSET-THREAD-LOCAL"));
}

TEST(MirToLirTls, Arm64ThreadLocalUnderShippedFormatFailsLoud0x8015First) {
    // PRODUCTION arm64 order pin: the shipped elf64-aarch64-linux-exec
    // declares no tlsAccess, so the FORMAT gate (0x8015) fires FIRST —
    // before the target's missing-tlsbase-opcode gate is ever
    // consulted. Deterministic: the format check precedes the opcode
    // probes in lowerThreadLocalGlobalAddr.
    auto fmt = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(fmt.has_value());
    ASSERT_FALSE((*fmt)->tlsAccess().has_value())
        << "precondition: arm64-ELF must not declare tlsAccess until C2";
    auto m = buildTlsPlusControlModule();
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto lirR = lowerToLir(m.mir, **target, m.interner, rep, {},
                           std::nullopt, std::nullopt, (*fmt)->tlsAccess());
    EXPECT_FALSE(lirR.ok);
    EXPECT_TRUE(sawDiagnosticCode(
        rep, DiagnosticCode::K_FormatLacksThreadLocalSupport));
    EXPECT_FALSE(sawDiagnosticCode(
        rep, DiagnosticCode::L_RequiredLirOpcodeMissing))
        << "the format gate must fire BEFORE (instead of) the opcode "
           "gate on the production arm64 leg";
}

TEST(MirToLirTls, Arm64LocalExecConfigFailsLoudMissingTlsBaseOpcode) {
    // The OPCODE gate: hand a local-exec config to the arm64 target
    // (which declares NO tlsbase row until TLS C2) — the lowering must
    // fail EXACTLY L_RequiredLirOpcodeMissing naming tlsbase, the
    // generic un-landed-target-leg gate.
    auto m = buildTlsPlusControlModule();
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    ASSERT_FALSE((*target)->opcodeByMnemonic("tlsbase").has_value())
        << "precondition: arm64 must not declare tlsbase until C2";
    DiagnosticReporter rep;
    auto lirR = lowerToLir(m.mir, **target, m.interner, rep, {},
                           std::nullopt, std::nullopt, elfLocalExecTls());
    EXPECT_FALSE(lirR.ok);
    bool sawTlsBaseMissing = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_RequiredLirOpcodeMissing
            && d.actual.find("tlsbase") != std::string::npos) {
            sawTlsBaseMissing = true;
        }
    }
    EXPECT_TRUE(sawTlsBaseMissing)
        << "arm64 + local-exec must fail loud on the missing 'tlsbase' "
           "opcode row (L_RequiredLirOpcodeMissing)";
}

TEST(MirToLirTls, PeIndexedModelDeclaredButUnlandedFailsLoud) {
    // Closed-verb arm: `pe-indexed` is declared vocabulary whose
    // LOWERING lands with the PE TLS cycle — until then it must fail
    // loud (0x8015-class), never emit a silently-wrong local-exec
    // sequence (the two models disagree on every byte after the tp
    // read).
    auto m = buildTlsPlusControlModule();
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ::dss::TlsAccessInfo pe{};
    pe.model             = ::dss::TlsAccessModel::PeIndexed;
    pe.segmentPrefixByte = 0x65;
    pe.baseDisplacement  = 0x58;
    DiagnosticReporter rep;
    auto lirR = lowerToLir(m.mir, **target, m.interner, rep, {},
                           std::nullopt, std::nullopt, pe);
    EXPECT_FALSE(lirR.ok);
    EXPECT_TRUE(sawDiagnosticCode(
        rep, DiagnosticCode::K_FormatLacksThreadLocalSupport));
    EXPECT_TRUE(sawAnchor(rep, "pe-indexed"))
        << "the reject must name the declared-but-unlanded model";
}
