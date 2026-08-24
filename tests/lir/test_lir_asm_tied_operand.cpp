// `"+r"` — A READ-WRITE ASM OPERAND'S TWO HALVES BIND TO **ONE** REGISTER.
// D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE.
//
// ★★★ THE CLAIM. A `+` operand is ONE thing the source wrote and TWO things the
// machine needs. `hir_to_mir.cpp` splits it exactly as GNU does (6.47.2.4): the
// write half stays an OUTPUT (it gets a result piece and a store-back) and a
// matching-constraint INPUT is appended carrying `MirAsmOperand::tiedOutput`,
// whose value LOADS from the very address the store-back writes. This file pins
// what MIR→LIR then owes: both halves must land in the SAME `LirReg`, so the
// register the template names as `%0` is the register the read half was
// materialised into.
//
// ★★ WHY THE CONTROL IS THE LOAD-BEARING HALF. "The two halves share a register"
// is not, on its own, a discriminating property — a lowering that minted one vreg
// for every operand would satisfy it and be catastrophically wrong. So each
// behavioural claim below is a MATCHED PAIR over one bit of difference: the
// UNTIED shape (`"=r"` output + separate `"r"` input) must bind TWO registers,
// and the TIED shape must bind ONE, through the same lowering on the same target.
// `EXPECT_NE` on the control is what gives the `EXPECT_EQ` its meaning.
//
// ★★ AND THE POSITIVE CLAIM IS NOT "THEY ARE EQUAL" BUT "THE TEMPLATE'S REGISTER
// IS THE ONE THE READ HALF WAS WRITTEN INTO". Those differ: a lowering could tie
// the two bindings together and still materialise the value somewhere else, which
// is exactly the read-as-undefined shape this whole arc exists to prevent
// (D-LIR-ASM-UNPINNED-INPUT-NEVER-MATERIALISED shipped that defect for the plain
// `"r"` input path, rc=0 and the wrong answer on both formats and both configs).
// So the assertion walks from the template's instruction BACK to a defining
// `mov`, and from that `mov` back to the `load` that read the variable.
//
// ★ THE REFUSAL ARMS ARE NOT OPTIONAL. Replacing a fail-loud refusal with
// acceptance is the single worst outcome available here, so the shapes that
// remain unrepresentable are pinned as REFUSALS with their diagnostics matched:
// a `+` written in the INPUT section (✔MEASURED: gcc 13.3.0 rejects it too,
// `error: input operand constraint contains '+'`), and — the one that replaces
// the old blanket refusal — a read-write
// OUTPUT whose read half is MISSING. The second cannot be produced by the shipped
// front end, which is precisely why it is built by hand here: it is the guard
// against a future producer, or a rebuild pass, dropping the tied entry. Without
// this arm, deleting that check leaves every test green and the template reading
// a register nothing wrote.
//
// ★★★ THIS FILE CAUGHT WHAT THE CORPUS EXAMPLE MISSED, AND THAT IS THE ARGUMENT
// FOR KEEPING BOTH TIERS. ✔MEASURED 2026-08-17: the FIRST version of
// `examples/c/asm_tied_operand` used a single `"+r"` operand and stayed
// GREEN over a live mutant on BOTH the baseline and the release arm — under the
// mutant the read half's `mov` targets a dead vreg emitted immediately before
// the template, the template's result vreg starts its range right after it, the
// two never overlap, and the linear scan hands the result the very register that
// dead `mov` just filled with the right value. The example was rebuilt around
// TWO tied operands (whose result vregs overlap each other and so cannot both
// inherit) and now reddens both arms. THIS file reddened throughout, because it
// asserts the STRUCTURE — "the register the template reads is the one the read
// half was written into" — which no allocation coincidence can satisfy.
// ⇒ a runtime pin can be defeated by luck; a structural one at the tier that
// makes the decision cannot. Keep both; neither is redundant.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe`
// (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_asm_descriptor.hpp"
#include "mir/mir_opcode.hpp"
#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support;

namespace {

// Every instruction of the module, in block order — the walk both the
// earlyclobber stamper and the constraint attacher perform over the id range
// their expansion minted.
[[nodiscard]] std::vector<LirInstId> allInsts(Lir const& lir) {
    std::vector<LirInstId> out;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const b = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
                out.push_back(lir.blockInstAt(b, ii));
            }
        }
    }
    return out;
}

[[nodiscard]] std::uint16_t opOf(TargetSchema const& t, std::string_view m) {
    auto const i = t.opcodeByMnemonic(m);
    EXPECT_TRUE(i.has_value()) << "target declares no '" << m << "'";
    return i.has_value() ? *i : std::uint16_t{0};
}

// The FIRST instruction carrying `opcode` whose operands include an `ImmInt`
// equal to `imm`. The templates below each contain exactly one such
// instruction, which is what lets the search name the template's own
// instruction without depending on its position.
[[nodiscard]] std::optional<LirInstId>
findWithImm(Lir const& lir, std::uint16_t opcode, std::int64_t imm) {
    for (LirInstId const id : allInsts(lir)) {
        if (lir.instOpcode(id) != opcode) continue;
        for (auto const& o : lir.instOperands(id)) {
            if (o.kind == LirOperandKind::ImmInt
                && static_cast<std::int64_t>(o.immInt32) == imm) {
                return id;
            }
        }
    }
    return std::nullopt;
}

// The instruction that DEFINES `reg` and appears strictly before `before`.
// ⚠ `firstDef` semantics are what matter for the tied register: it is defined
// TWICE (by the materialising `mov` and by the template), so the search must be
// bounded above by the template's own instruction or it would find the template
// again and assert nothing.
[[nodiscard]] std::optional<LirInstId>
findDefBefore(Lir const& lir, LirReg reg, LirInstId before) {
    std::optional<LirInstId> found;
    for (LirInstId const id : allInsts(lir)) {
        if (id.v == before.v) break;
        if (lir.instResult(id) == reg) found = id;
    }
    return found;
}

} // namespace

// ── ARM 1: THE TIE, THROUGH THE REAL LOWERING ────────────────────────────────
//
// `__asm__("addl $2, %0" : "+r"(x))` — the exact program the anchor names, and
// the one that returned rc=1 with `L_UnsupportedLoweringForOpcode` until the
// blanket `isReadWrite` refusal was replaced by a read of `tiedOutput`.
TEST(LirAsmTiedOperand, ReadWriteOperandBindsBothHalvesToTheOutputsRegister) {
    auto L = lowerCToLir(
        "void f(void){ int x; x = 40; __asm__(\"addl $2, %0\" : \"+r\"(x)); }",
        "x86_64");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
                ? std::string{} : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? std::string{}
                                        : L.mirReporter.all()[0].actual);
    ASSERT_TRUE(L.lir.ok)
        << "a `\"+r\"` operand must LOWER — this is the refusal the anchor "
           "names: "
        << (L.lirReporter.all().empty() ? std::string{}
                                        : L.lirReporter.all()[0].actual);

    Lir const& lir = L.lir.lir;
    auto const addOp = opOf(*L.target, "add");
    auto const movOp = opOf(*L.target, "mov");

    // The template's own instruction: x86's two-address `add` carrying the
    // literal 2 the source wrote.
    auto const add = findWithImm(lir, addOp, 2);
    ASSERT_TRUE(add.has_value())
        << "the template `addl $2, %0` must lower to an `add` carrying the "
           "immediate 2";
    LirReg const tied = lir.instResult(*add);
    ASSERT_TRUE(tied.valid());

    // ── (a) THE TWO-ADDRESS IDENTITY. The core's tie is an operand INDEX on the
    // TARGET OPCODE (`requires2Address`), and x86's `add` declares it at 0. With
    // the asm tie in place the legalizer has nothing to do — result and
    // operand[0] are already the same register — which is what makes the two
    // mechanisms compose instead of fighting.
    auto const ops = lir.instOperands(*add);
    ASSERT_GE(ops.size(), 1u);
    ASSERT_EQ(ops[0].kind, LirOperandKind::Reg);
    EXPECT_EQ(ops[0].reg, tied)
        << "the template writes and reads ONE spelling (`%0`), so the "
           "instruction's result and its tied operand must be one register";

    // ── (b) ★★ THE READ HALF REACHED THAT REGISTER. This is the assertion the
    // whole feature reduces to: a `mov` BEFORE the template defines the very
    // register the template reads. Without the tie, `bindAsmOperand` mints a
    // fresh vreg for the read half and this `mov` targets something the template
    // never names — the register stays undefined, rc=0, wrong answer.
    auto const materialise = findDefBefore(lir, tied, *add);
    ASSERT_TRUE(materialise.has_value())
        << "nothing defines the register the template reads — the read half was "
           "materialised into some OTHER register, which is the "
           "read-as-undefined miscompile this arc exists to prevent";
    EXPECT_EQ(lir.instOpcode(*materialise), movOp)
        << "the read half is materialised by the target's move";

    // ── (c) AND THE VALUE IT MOVED IS THE VARIABLE'S. One more hop back: the
    // `mov`'s source must be defined by a `load`, i.e. the tied read really read
    // memory rather than picking up an unrelated live value.
    auto const movOps = lir.instOperands(*materialise);
    ASSERT_EQ(movOps.size(), 1u);
    ASSERT_EQ(movOps[0].kind, LirOperandKind::Reg);
    auto const loadOp = opOf(*L.target, "load");
    auto const src    = findDefBefore(lir, movOps[0].reg, *materialise);
    ASSERT_TRUE(src.has_value())
        << "the materialised value must itself have a definition";
    EXPECT_EQ(lir.instOpcode(*src), loadOp)
        << "a `\"+r\"` operand's read half LOADS the object's current value — "
           "the store-back then writes the same address, which is what makes "
           "the read and the write provably the same object";
}

// ── ARM 2: THE CONTROL ───────────────────────────────────────────────────────
//
// The same target, the same lowering, one bit of difference: the operand is
// written as a separate `"=r"` output and `"r"` input instead of one `"+r"`.
// Two source operands ⇒ TWO registers. Without this arm, ARM 1 would also pass
// against a lowering that gave every asm operand the same register.
TEST(LirAsmTiedOperand, UntiedOutputAndInputBindToDifferentRegisters) {
    auto L = lowerCToLir(
        "void f(void){ int x; int y; x = 40; y = 0; "
        "__asm__(\"movl %1, %0\" : \"=r\"(y) : \"r\"(x)); }",
        "x86_64");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
                ? std::string{} : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok);
    ASSERT_TRUE(L.lir.ok)
        << (L.lirReporter.all().empty() ? std::string{}
                                        : L.lirReporter.all()[0].actual);

    Lir const& lir   = L.lir.lir;
    auto const movOp = opOf(*L.target, "mov");

    // The template's `mov` is the LAST `mov` whose source register was defined
    // by an earlier `mov` (the input materialisation). Rather than depend on
    // that chain, assert the property directly over every `mov`: no `mov` in
    // this module may have result == operand[0], because nothing here is tied.
    bool sawRegToReg = false;
    for (LirInstId const id : allInsts(lir)) {
        if (lir.instOpcode(id) != movOp) continue;
        auto const ops = lir.instOperands(id);
        if (ops.empty() || ops[0].kind != LirOperandKind::Reg) continue;
        sawRegToReg = true;
        EXPECT_NE(ops[0].reg, lir.instResult(id))
            << "an UNTIED output and input are two operands and must bind two "
               "registers — a lowering that collapsed them would satisfy the "
               "tied assertion for the wrong reason";
    }
    EXPECT_TRUE(sawRegToReg)
        << "anti-vacuity: this control asserts nothing unless the module "
           "actually contains a register-to-register move";
}

// ── ARM 3: THE REFUSAL THAT REPLACED THE BLANKET ONE ─────────────────────────
//
// A read-write OUTPUT with NO input entry carrying its read half. The shipped
// front end cannot produce this — it appends the tied entry unconditionally —
// so it is BUILT BY HAND, which is the only way to reach the guard at all. That
// is the point: the guard exists for a future producer (a new source language,
// a rebuild pass that copies field-by-field) that forgets, and a guard nobody
// exercises is one nobody notices deleting.
//
// ⚠ THE DESCRIPTOR MUST STAY OPERAND-ALIGNED. `MirBuilder::
// checkAsmOperandAlignment_` ABORTS (not diagnoses) when `desc.inputs.size()`
// disagrees with the operand count, so "drop the tied input" is expressed as a
// descriptor with ZERO inputs and ZERO operands — the exact shape a producer
// that never learned to synthesize the read half would build.
TEST(LirAsmTiedOperand, ReadWriteOutputWithNoTiedInputIsRefused) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirAsmDescriptor d;
    d.templateText = "addl $2, %0";
    d.isExtended   = true;
    MirAsmOperand out;
    out.constraint  = "+r";
    out.regClass    = TargetRegClass::GPR;
    out.isReadWrite = true;                 // … and NOTHING carries its read half
    d.outputs.push_back(std::move(out));
    (void)mb.addInlineAsm(std::move(d), {}, i32);
    mb.addReturn();
    Mir const mir = std::move(mb).finish();

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter reporter;
    MirToLirResult const r = lowerToLir(mir, **target, interner, reporter);

    EXPECT_FALSE(r.ok)
        << "binding a `+` output with no read half hands the template a "
           "register NOTHING wrote — accepting it is a silent miscompile with a "
           "correct-looking rc=0";
    ASSERT_FALSE(reporter.all().empty()) << "the refusal must be DIAGNOSED";
    std::string const text = reporter.all()[0].actual;
    EXPECT_NE(text.find("no input entry carries its read half"),
              std::string::npos)
        << "the diagnostic must name the missing half rather than the "
           "constraint spelling — the spelling is legal C: " << text;
    EXPECT_NE(text.find("D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE"),
              std::string::npos) << text;
}

// ── ARM 4: `+` IN THE INPUT SECTION ──────────────────────────────────────────
//
// An input entry that is read-write and carries NO tie. `hir_to_mir.cpp` states
// outright that such an operand "gets nothing here, and that is deliberate": it
// already has its read half, what it lacks is a write-back, and there is no
// output entry, no result piece and no lvalue address to invent one from.
// ✔MEASURED: gcc 13.3.0 rejects the same spelling, so refusing is conformance.
TEST(LirAsmTiedOperand, ReadWriteInputWithNoTieIsRefused) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue lit;
    lit.value = std::int64_t{7};
    lit.core  = TypeKind::I32;
    MirInstId const seed = mb.addConst(lit, i32);
    MirAsmDescriptor d;
    d.templateText = "addl $2, %0";
    d.isExtended   = true;
    MirAsmOperand in;
    in.constraint  = "+r";
    in.regClass    = TargetRegClass::GPR;
    in.isReadWrite = true;                  // … in the INPUT list, with no tie
    d.inputs.push_back(std::move(in));
    MirInstId const ops[] = {seed};
    (void)mb.addInlineAsm(std::move(d), ops, InvalidType);
    mb.addReturn();
    Mir const mir = std::move(mb).finish();

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter reporter;
    MirToLirResult const r = lowerToLir(mir, **target, interner, reporter);

    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(reporter.all().empty());
    std::string const text = reporter.all()[0].actual;
    EXPECT_NE(text.find("has no output entry"), std::string::npos) << text;
    EXPECT_NE(text.find("D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE"),
              std::string::npos) << text;
}
