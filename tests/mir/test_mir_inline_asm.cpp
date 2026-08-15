// Inline-asm P5 — MIR carriage (plan 29 §4.4).
//
// Four things are pinned here, and each of them is a place where the shipped
// design could regress into a SILENT wrong answer rather than a loud one:
//
//   1. The two opcode ROWS. `InlineAsm` is `Call`-shaped except for `{0, N}`
//      operands, `InlineAsmGoto` is `Switch`-shaped in its successors, and BOTH
//      are in the `opcodeClobbersMemory` positive list. A row edit that dropped
//      the clobber membership would let CSE/LICM move loads across an asm block
//      with nothing to observe it.
//   2. The `ReturnPiece` payload SPLIT. The piece carries an ordinal AND the
//      register class that ordinal indexes; the class is no longer inferred from
//      the MIR type. The discriminating case is a piece whose declared class
//      DISAGREES with `regClassForCoreType(its type)` — the `"=x"`-on-an-integer
//      shape — because on the agreeing cases the old inference and the new
//      carriage are indistinguishable.
//   3. The `addInst` STRUCTURAL BACKSTOP: `InlineAsm` has a dedicated builder and
//      `addInst` refuses it, so a rebuild site that forwards the raw payload
//      aborts instead of naming a descriptor in a pool that does not have one.
//   4. The `asm goto` EDGE-PLACEMENT rule: pieces at successor heads, every
//      piece-bearing edge single-predecessor.

#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"          // TargetRegClass
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_asm_descriptor.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_return_piece_payload.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace dss;

namespace {

MirAsmOperand gprOut(std::string constraint) {
    MirAsmOperand o;
    o.constraint = std::move(constraint);
    o.regClass   = TargetRegClass::GPR;
    return o;
}

MirAsmOperand fprOut(std::string constraint) {
    MirAsmOperand o;
    o.constraint = std::move(constraint);
    o.regClass   = TargetRegClass::FPR;
    return o;
}

// `__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi) ::)` in the shape MIR carries:
// two outputs, no inputs, an explicit clobber.
MirAsmDescriptor twoOutputDescriptor() {
    MirAsmDescriptor d;
    d.templateText = "rdtsc";
    d.isVolatile   = true;
    d.outputs.push_back(gprOut("=a"));
    d.outputs.back().fixedRegister = "rax";
    d.outputs.push_back(gprOut("=d"));
    d.outputs.back().fixedRegister = "rdx";
    d.clobbers.push_back("rcx");
    d.clobbersConditionCodes = true;
    return d;
}

// Every instruction of a module, flattened.
std::vector<MirInstId> allInsts(Mir const& mir) {
    std::vector<MirInstId> out;
    for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
                out.push_back(mir.blockInstAt(b, ii));
            }
        }
    }
    return out;
}

// How many blocks of `f` list `target` among their CFG successors.
std::uint32_t predecessorCount(Mir const& mir, MirFuncId f, MirBlockId target) {
    std::uint32_t n = 0;
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
        MirBlockId const b = mir.funcBlockAt(f, bi);
        for (MirBlockId const s : mir.blockSuccessors(b)) {
            if (s.v == target.v) ++n;
        }
    }
    return n;
}

} // namespace

// ── 1. the opcode rows ──────────────────────────────────────────────────────

// `InlineAsm` takes Call's row SHAPE with ONE deviation, and the deviation is
// the point: Call's minimum operand count is 1 because its first operand is the
// CALLEE, and an asm block has no callee — `__asm__("nop")` has zero operands.
// Pinning `{0, N}` against Call's `{1, N}` is what stops a later "make it
// consistent with Call" edit from making an input-less asm block unbuildable.
TEST(MirInlineAsm, InlineAsmRowIsCallShapedButAcceptsZeroOperands) {
    MirOpcodeInfo const asmRow  = opcodeInfo(MirOpcode::InlineAsm);
    MirOpcodeInfo const callRow = opcodeInfo(MirOpcode::Call);

    EXPECT_EQ(asmRow.minOperands, 0u)
        << "an asm block may have zero inputs; Call's minimum-1 is its CALLEE";
    EXPECT_EQ(callRow.minOperands, 1u)
        << "anti-vacuity: the deviation is only meaningful while Call is {1, N}";
    EXPECT_EQ(asmRow.maxOperands, kMirUnboundedOperands);
    EXPECT_EQ(asmRow.maxOperands, callRow.maxOperands);
    EXPECT_EQ(asmRow.minSuccessors, 0u);
    EXPECT_EQ(asmRow.maxSuccessors, 0u);
    EXPECT_EQ(asmRow.result, MirResultRule::Optional);
    EXPECT_EQ(asmRow.result, callRow.result);
    EXPECT_FALSE(asmRow.isTerminator);
    EXPECT_TRUE(asmRow.hasSideEffects);
    EXPECT_EQ(asmRow.hasSideEffects, callRow.hasSideEffects);
    EXPECT_EQ(mnemonic(MirOpcode::InlineAsm), "inlineasm");
}

// The goto form is a TERMINATOR with Switch's unbounded successor range and NO
// result. `R::None` is structural, not a restriction: piece capture requires the
// piece to immediately follow its producer and a terminator has nothing after
// it, so EVERY output — output 0 included — must land at a successor head.
TEST(MirInlineAsm, InlineAsmGotoRowIsSwitchShapedInItsSuccessors) {
    MirOpcodeInfo const gotoRow   = opcodeInfo(MirOpcode::InlineAsmGoto);
    MirOpcodeInfo const switchRow = opcodeInfo(MirOpcode::Switch);

    EXPECT_EQ(gotoRow.minOperands, 0u)
        << "like InlineAsm and unlike Switch, an asm goto may have no operands";
    EXPECT_EQ(gotoRow.maxOperands, kMirUnboundedOperands);
    EXPECT_EQ(gotoRow.minSuccessors, 1u);
    EXPECT_EQ(gotoRow.minSuccessors, switchRow.minSuccessors);
    EXPECT_EQ(gotoRow.maxSuccessors, kMirUnboundedSuccessors);
    EXPECT_EQ(gotoRow.maxSuccessors, switchRow.maxSuccessors);
    EXPECT_EQ(gotoRow.result, MirResultRule::None)
        << "a terminator cannot carry a result piece's producer value";
    EXPECT_TRUE(gotoRow.isTerminator);
    EXPECT_TRUE(gotoRow.hasSideEffects);
    EXPECT_EQ(mnemonic(MirOpcode::InlineAsmGoto), "inlineasmgoto");
}

// ★ The membership that IS the opcode's optimizer contract. A template is opaque
// text, so nothing can prove it writes no memory. Dropping either arm lets
// CSE/LICM reorder loads across an asm block, silently — which is why the
// companion region-walk test in tests/opt/ exercises the consequence rather than
// re-reading the flag.
TEST(MirInlineAsm, BothAsmFormsClobberMemory) {
    EXPECT_TRUE(opcodeClobbersMemory(MirOpcode::InlineAsm));
    EXPECT_TRUE(opcodeClobbersMemory(MirOpcode::InlineAsmGoto));
    // Anti-vacuity: the predicate is not simply true for everything
    // side-effecting. ReturnPiece is `hasSideEffects` and is NOT a clobber.
    EXPECT_TRUE(opcodeInfo(MirOpcode::ReturnPiece).hasSideEffects);
    EXPECT_FALSE(opcodeClobbersMemory(MirOpcode::ReturnPiece));
}

// ── 2. the ReturnPiece payload split ────────────────────────────────────────

TEST(MirInlineAsm, ResultPiecePayloadRoundTripsBothFields) {
    for (std::uint32_t ord : {0u, 1u, 7u, return_piece_payload::kOrdinalMax}) {
        for (auto cls : {TargetRegClass::GPR, TargetRegClass::FPR,
                         TargetRegClass::VR}) {
            std::uint32_t const p =
                return_piece_payload::encode(ord, static_cast<std::uint32_t>(cls));
            EXPECT_EQ(return_piece_payload::ordinal(p), ord);
            EXPECT_EQ(static_cast<TargetRegClass>(return_piece_payload::regClass(p)),
                      cls);
        }
    }
    // The two fields do not bleed into each other: a maximal ordinal beside a
    // non-zero class still reads back as both.
    std::uint32_t const packed = return_piece_payload::encode(
        return_piece_payload::kOrdinalMax,
        static_cast<std::uint32_t>(TargetRegClass::VR));
    EXPECT_EQ(return_piece_payload::ordinal(packed),
              return_piece_payload::kOrdinalMax);
    EXPECT_EQ(return_piece_payload::regClass(packed),
              static_cast<std::uint32_t>(TargetRegClass::VR));
}

// ★★★ THE DISCRIMINATING CASE, and the reason the split exists. An integer-typed
// piece whose CONSTRAINT selects an FP register (`"=x"` on an `int`) reads back
// FPR. Before the split the class was re-derived as
// `regClassForCoreType(the MIR type)`, which for I64 is GPR — so the piece would
// have indexed the integer return pool while the asm block left the value in an
// SSE register, with no diagnostic anywhere on the path.
//
// RED-ON-DISABLE: make `returnPieceRegClass` return
// `regClassForCoreType(interner.kind(instType(id)))` and this assertion reports
// GPR for an FPR-declared piece. Note the pin is written on a case where the two
// answers DISAGREE — a GPR piece of an I64 type would pass under either rule and
// would have tested the coincidence, not the mapping.
TEST(MirInlineAsm, ResultPieceCarriesAClassIndependentOfItsType) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    ASSERT_EQ(regClassForCoreType(TypeKind::I64), TargetRegClass::GPR)
        << "anti-vacuity: the type-derived answer must really be GPR, else this "
           "test cannot tell the two rules apart";

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);

    MirAsmDescriptor d;
    d.templateText = "movd %1, %0";
    d.outputs.push_back(fprOut("=x"));      // SSE output …
    d.outputs.push_back(fprOut("=x"));
    MirInstId const asmId = mb.addInlineAsm(std::move(d), {}, i64);  // … of I64 type
    MirInstId const piece =
        mb.addReturnPiece(asmId, /*ordinal=*/1, TargetRegClass::FPR, i64);
    mb.addReturn();
    Mir const mir = std::move(mb).finish();

    EXPECT_EQ(mir.returnPieceOrdinal(piece), 1u);
    EXPECT_EQ(mir.returnPieceRegClass(piece), TargetRegClass::FPR)
        << "the class the PRODUCER declared, not the one the type implies";
    EXPECT_EQ(regClassForCoreType(interner.kind(mir.instType(piece))),
              TargetRegClass::GPR)
        << "and the two genuinely disagree here — that is what makes the "
           "assertion above discriminating";
}

// The Call path keeps carrying its class too, so the split is not an asm-only
// side channel that the shipped struct-return path quietly bypasses.
TEST(MirInlineAsm, CallResultPiecesCarryTheirClassAsWell) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const f64   = interner.primitive(TypeKind::F64);
    TypeId const fnSig = interner.fnSig({}, i64, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const callee  = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const ops[]   = {callee};
    MirInstId const call    = mb.addInst(MirOpcode::Call, ops, i64);
    MirInstId const gprPiece = mb.addReturnPiece(call, 1, TargetRegClass::GPR, i64);
    MirInstId const fprPiece = mb.addReturnPiece(call, 0, TargetRegClass::FPR, f64);
    mb.addReturn(call);
    Mir const mir = std::move(mb).finish();

    EXPECT_EQ(mir.returnPieceRegClass(gprPiece), TargetRegClass::GPR);
    EXPECT_EQ(mir.returnPieceOrdinal(gprPiece), 1u);
    // ★ The two pieces share ORDINAL 0/1 across DIFFERENT pools — the per-class
    // counting the ordinal has always meant. Without the class stored, "ordinal
    // 0" alone cannot say which register it is.
    EXPECT_EQ(mir.returnPieceRegClass(fprPiece), TargetRegClass::FPR);
    EXPECT_EQ(mir.returnPieceOrdinal(fprPiece), 0u);
}

// ── 3. the structural backstops ─────────────────────────────────────────────

// ★★★ THE BACKSTOP THE THREE REBUILD SITES REST ON. A verbatim `addInst` copy
// forwards `instPayload` — which for an asm block is an index into the SOURCE
// module's descriptor pool. Refusing the opcode here converts "the clobber list
// was silently dropped" into "the build stops and names the builder to use".
TEST(MirInlineAsmDeath, AddInstRefusesInlineAsmAndNamesItsDedicatedBuilder) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);
    EXPECT_DEATH({
        MirBuilder mb;
        mb.addFunction(fnSig, SymbolId{100});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(entry);
        (void)mb.addInst(MirOpcode::InlineAsm, {}, InvalidType, /*payload=*/0);
    }, "has a dedicated builder");
}

// The goto form is caught one layer earlier — by the terminator refusal — so the
// pair of them leaves no `addInst` route to either asm opcode.
TEST(MirInlineAsmDeath, AddInstRefusesInlineAsmGotoAsATerminator) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);
    EXPECT_DEATH({
        MirBuilder mb;
        mb.addFunction(fnSig, SymbolId{100});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(entry);
        (void)mb.addInst(MirOpcode::InlineAsmGoto, {}, InvalidType);
    }, "is a terminator");
}

// A class of `None` is what a caller that never chose would pass — it is the
// enum's default-constructed value. Silently treating it as GPR would rebuild
// exactly the else-branch default this split removed.
TEST(MirInlineAsmDeath, AddReturnPieceRefusesAnUnstatedRegisterClass) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const fnSig = interner.fnSig({}, i64, CallConv::CcSysV);
    EXPECT_DEATH({
        MirBuilder mb;
        mb.addFunction(fnSig, SymbolId{100});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(entry);
        MirInstId const callee = mb.addGlobalAddr(SymbolId{50}, fnSig);
        MirInstId const ops[]  = {callee};
        MirInstId const call   = mb.addInst(MirOpcode::Call, ops, i64);
        (void)mb.addReturnPiece(call, 1, TargetRegClass{}, i64);
    }, "must be stated, never defaulted");
}

// A piece anchored to something that has no result registers at all.
TEST(MirInlineAsmDeath, AddReturnPieceRefusesANonMultiResultProducer) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const fnSig = interner.fnSig({}, i64, CallConv::CcSysV);
    EXPECT_DEATH({
        MirBuilder mb;
        mb.addFunction(fnSig, SymbolId{100});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(entry);
        MirInstId const notAProducer = mb.addGlobalAddr(SymbolId{50}, fnSig);
        (void)mb.addReturnPiece(notAProducer, 0, TargetRegClass::GPR, i64);
    }, "multi-result producer");
}

// The descriptor's input list and the operand list are one fact seen twice.
TEST(MirInlineAsmDeath, AddInlineAsmRefusesAnOperandCountThatContradictsTheDescriptor) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);
    EXPECT_DEATH({
        MirBuilder mb;
        mb.addFunction(fnSig, SymbolId{100});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(entry);
        MirInstId const v = mb.addInst(MirOpcode::Alloca, {}, i64);
        MirAsmDescriptor d;
        d.templateText = "nop";
        MirInstId const ops[] = {v};       // one operand, zero declared inputs
        (void)mb.addInlineAsm(std::move(d), ops, InvalidType);
    }, "declares 0 input");
}

// ── 4. the descriptor pool ──────────────────────────────────────────────────

TEST(MirInlineAsm, DescriptorSurvivesTheFreezeAndIsReadableFromTheInstruction) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const fnSig = interner.fnSig({}, i64, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const asmId = mb.addInlineAsm(twoOutputDescriptor(), {}, i64);
    MirInstId const piece = mb.addReturnPiece(asmId, 1, TargetRegClass::GPR, i64);
    mb.addReturn(asmId);
    Mir const mir = std::move(mb).finish();

    ASSERT_EQ(mir.instOpcode(asmId), MirOpcode::InlineAsm);
    MirAsmDescriptor const& d = mir.asmDescriptor(asmId);
    EXPECT_EQ(d.templateText, "rdtsc");
    EXPECT_TRUE(d.isVolatile);
    ASSERT_EQ(d.outputs.size(), 2u);
    EXPECT_EQ(d.outputs[0].constraint, "=a");
    EXPECT_EQ(d.outputs[0].fixedRegister, "rax");
    EXPECT_EQ(d.outputs[1].fixedRegister, "rdx");
    ASSERT_EQ(d.clobbers.size(), 1u);
    EXPECT_EQ(d.clobbers[0], "rcx");
    EXPECT_TRUE(d.clobbersConditionCodes);
    EXPECT_FALSE(d.clobbersMemory)
        << "the SOURCE declared no `memory` clobber — which is a different fact "
           "from the opcode's unconditional opcodeClobbersMemory membership";
    EXPECT_EQ(mir.asmDescriptorPool().size(), 1u);
    // The piece is anchored to the asm block, exactly as a Call's piece is.
    ASSERT_EQ(mir.instOperands(piece).size(), 1u);
    EXPECT_EQ(mir.instOperands(piece)[0].v, asmId.v);
}

// No dedup: two textually identical asm statements are two statements, and
// keeping their identity is what makes an index-by-index rebuild lossless.
TEST(MirInlineAsm, IdenticalDescriptorsGetDistinctPoolSlots) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirAsmDescriptor a; a.templateText = "nop";
    MirAsmDescriptor b; b.templateText = "nop";
    MirInstId const i1 = mb.addInlineAsm(std::move(a), {}, InvalidType);
    MirInstId const i2 = mb.addInlineAsm(std::move(b), {}, InvalidType);
    mb.addReturn();
    Mir const mir = std::move(mb).finish();

    EXPECT_EQ(mir.asmDescriptorPool().size(), 2u);
    EXPECT_NE(mir.asmDescriptorIndex(i1), mir.asmDescriptorIndex(i2));
}

// ── 5. the `asm goto` edge-placement rule ───────────────────────────────────

// With NO outputs there is nothing to place, so nothing is interposed: the
// terminator's successor set IS the label set, literally. This is the arm that
// makes the split arm below non-vacuous — without it, "a split happened" could
// just mean "the builder always splits everything".
TEST(MirInlineAsm, AsmGotoWithoutOutputsBranchesStraightToItsLabels) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    MirFuncId const f = mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const l0    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const l1    = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirAsmDescriptor d;
    d.templateText = "jmp %l0";
    MirBlockId const labels[] = {l0, l1};
    auto const r = mb.addInlineAsmGoto(std::move(d), {}, labels);
    ASSERT_EQ(r.edges.size(), 2u);
    EXPECT_FALSE(r.edges[0].split);
    EXPECT_FALSE(r.edges[1].split);
    EXPECT_EQ(r.edges[0].successor.v, l0.v);
    EXPECT_EQ(r.edges[1].successor.v, l1.v);
    mb.beginBlock(l0); mb.addReturn();
    mb.beginBlock(l1); mb.addReturn();
    Mir const mir = std::move(mb).finish();

    auto const succ = mir.blockSuccessors(entry);
    ASSERT_EQ(succ.size(), 2u);
    EXPECT_EQ(succ[0].v, l0.v);
    EXPECT_EQ(succ[1].v, l1.v);
    EXPECT_EQ(mir.funcBlockCount(f), 3u) << "no landing block was interposed";
}

// ★★★ THE PLACEMENT RULE. With outputs, every edge gets a landing block whose
// HEAD holds that edge's pieces and whose only predecessor is the asm block —
// i.e. no piece ever sits on a critical edge. Following each landing block's
// single `Br` recovers exactly the label set the source named.
//
// RED-ON-DISABLE: make `addInlineAsmGoto` push `label` instead of `landing` and
// the pieces have nowhere legal to go — the successor blocks are then shared,
// `predecessorCount` reports 2 for a shared label, and the head assertion fails
// because the label block's first instruction is not a ReturnPiece.
TEST(MirInlineAsm, AsmGotoWithOutputsPlacesPiecesAtSinglePredecessorSuccessorHeads) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    MirFuncId  const f     = mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const pre   = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const l0    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const l1    = mb.createBlock(StructCfMarker::Linear);

    // ★ `l0` IS DELIBERATELY GIVEN A SECOND PREDECESSOR. `entry` branches to it
    // directly as well as reaching it through the asm goto, so the asm→l0 edge
    // is genuinely CRITICAL — which is the case the rule exists for. Without
    // this the test would pass on a CFG where every label happened to be
    // exclusive to its edge, i.e. it would test the coincidence, not the rule.
    mb.beginBlock(entry);
    MirLiteralValue condLit; condLit.value = true; condLit.core = TypeKind::Bool;
    MirInstId const cond = mb.addConst(condLit, interner.primitive(TypeKind::Bool));
    mb.addCondBr(cond, pre, l0);

    mb.beginBlock(pre);
    MirAsmDescriptor d;
    d.templateText = "cmp %1, %2; je %l1";
    d.outputs.push_back(gprOut("=r"));
    d.outputs.push_back(fprOut("=x"));
    MirBlockId const labels[] = {l0, l1};
    auto const r = mb.addInlineAsmGoto(std::move(d), {}, labels);
    ASSERT_EQ(r.edges.size(), 2u);
    MirInstId const asmId = r.terminator;

    std::vector<MirInstId> headPieces;
    for (auto const& e : r.edges) {
        ASSERT_TRUE(e.split) << "an asm goto WITH outputs must land them on an "
                                "edge block, never in the label block";
        EXPECT_NE(e.successor.v, e.label.v);
        mb.beginBlock(e.successor);
        headPieces.push_back(
            mb.addReturnPiece(asmId, 0, TargetRegClass::GPR, i64));
        headPieces.push_back(
            mb.addReturnPiece(asmId, 1, TargetRegClass::FPR, i64));
        mb.addBr(e.label);
    }
    mb.beginBlock(l0); mb.addReturn();
    mb.beginBlock(l1); mb.addReturn();
    Mir const mir = std::move(mb).finish();

    // (a) the terminator has one successor per label …
    auto const succ = mir.blockSuccessors(pre);
    ASSERT_EQ(succ.size(), 2u);
    // (b) … and following each successor's `Br` recovers exactly the label set.
    std::vector<std::uint32_t> reached;
    for (MirBlockId const s : succ) {
        EXPECT_EQ(predecessorCount(mir, f, s), 1u)
            << "a block holding result pieces must be reached ONLY from the asm "
               "goto — otherwise the pieces run on a path that did not produce "
               "them (the critical-edge miscompile)";
        // head = the pieces, in ordinal order; tail = the branch on to the label.
        ASSERT_EQ(mir.blockInstCount(s), 3u);
        MirInstId const p0 = mir.blockInstAt(s, 0);
        MirInstId const p1 = mir.blockInstAt(s, 1);
        EXPECT_EQ(mir.instOpcode(p0), MirOpcode::ReturnPiece);
        EXPECT_EQ(mir.instOpcode(p1), MirOpcode::ReturnPiece);
        EXPECT_EQ(mir.returnPieceOrdinal(p0), 0u);
        EXPECT_EQ(mir.returnPieceRegClass(p0), TargetRegClass::GPR);
        EXPECT_EQ(mir.returnPieceRegClass(p1), TargetRegClass::FPR);
        EXPECT_EQ(mir.instOperands(p0)[0].v,
                  mir.blockTerminator(pre).v)
            << "each piece is anchored to the asm goto itself";
        auto const onward = mir.blockSuccessors(s);
        ASSERT_EQ(onward.size(), 1u);
        reached.push_back(onward[0].v);
    }
    std::vector<std::uint32_t> const expected{l0.v, l1.v};
    EXPECT_EQ(reached, expected)
        << "the EFFECTIVE successor set equals the label set, in source order";

    EXPECT_EQ(mir.instOpcode(mir.blockTerminator(pre)), MirOpcode::InlineAsmGoto);
    // ★ ANTI-VACUITY FOR THE WHOLE TEST: `l0` really does have two predecessors
    // (the entry CondBr and the landing block), so the asm→l0 edge really was
    // critical. If this ever reads 1, the fixture stopped exercising the case
    // and the single-predecessor assertions above became free.
    EXPECT_EQ(predecessorCount(mir, f, l0), 2u)
        << "the fixture must keep l0 multi-predecessor, else the split is "
           "untested";
    EXPECT_EQ(mir.funcBlockCount(f), 6u)
        << "entry + pre + 2 landing blocks + l0 + l1";
}
