// `asm goto` — THE LABEL EDGE, THE FALL-THROUGH EDGE, AND RESULT PIECE 0.
// D-LIR-ASM-GOTO-REFUSAL-NAMES-A-CONFIG-GAP-THAT-NO-LONGER-EXISTS.
//
// ★★★ THE CLAIM. An `asm goto` is a TERMINATOR whose branches this file's
// subject does not write: the label edges are bound to blocks through
// `AsmLabelBinding` and taken by the SHARED assembly engine, and only the
// FALL-THROUGH edge is a branch `mir_to_lir` emits itself. That split is what
// makes the shape worth pinning structurally — three of the four properties
// below are invisible to a caller that only checks the exit code, and the
// fourth (result piece 0) is invisible to EVERY tier above the exit code.
//
// ★★ WHY THIS FILE EXISTS BESIDE `examples/c-subset/asm_goto_labels`. The
// corpus example is the end-to-end witness and it is the one that proves the
// program RUNS; but a runtime pin can be satisfied by luck — ✔MEASURED
// 2026-08-17 on this very subsystem, a single-`"+r"` asm example stayed GREEN
// over a live mutant on BOTH arms because the allocator happened to leave the
// right value in the register the template read. A structural assertion at the
// tier that makes the decision cannot be satisfied that way. Keep both.
//
// ★ THE FIXTURE LOWERS REAL c-subset SOURCE, never a hand-built descriptor,
// because the property under test is a RELATION between what the front end
// minted (the spellings) and what this tier bound (the blocks). A hand-typed
// descriptor would be testing the hand-typing —
// `D-TEST-A-PIN-THAT-STUBS-ITS-SUBJECTS-INPUT-IS-TESTING-THE-STUB`.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe`
// (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).

#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support;

namespace {

[[nodiscard]] std::uint16_t opOf(TargetSchema const& t, std::string_view m) {
    auto const i = t.opcodeByMnemonic(m);
    EXPECT_TRUE(i.has_value()) << "target declares no '" << m << "'";
    return i.has_value() ? *i : std::uint16_t{0};
}

// The MIR `InlineAsmGoto` terminator of the only function that carries one.
[[nodiscard]] std::optional<MirInstId> theAsmGoto(Mir const& mir) {
    for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const n = mir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < n; ++ii) {
                MirInstId const id = mir.blockInstAt(b, ii);
                if (mir.instOpcode(id) == MirOpcode::InlineAsmGoto) return id;
            }
        }
    }
    return std::nullopt;
}

// Every LIR instruction whose `lirToMir` provenance is `producer`, in module
// order. ★ PROVENANCE, NOT POSITION: the template's instructions are appended
// by the shared engine into whichever block is open, so "the asm's own block"
// is not a place a positional scan can name.
//
// ⚠ THE ENGINE'S OWN INSTRUCTIONS ARE **NOT** IN THIS SET, and the tests below
// depend on knowing that. `lowerAsmTemplateToLirRun` appends straight into the
// `LirBuilder`, so `mir_to_lir`'s `recordSource` never sees them and
// `lirToMir` carries no entry — a fact `expandInlineAsm`'s own comment states
// (it is why `lirInstEverEmitted_` has to be set by counting template LINES).
// What IS in this set is everything the LOWERING emitted for the statement: the
// per-input materialisation `mov`s, the pinned-output captures, and the
// trailing fall-through `jmp`. That is enough to name the asm's own BLOCK,
// which is how the block-level assertions below find their subject.
[[nodiscard]] std::vector<LirInstId>
instsFrom(MirToLirResult const& r, MirInstId producer) {
    std::vector<LirInstId> out;
    Lir const& lir = r.lir;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const b = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
                LirInstId const id = lir.blockInstAt(b, ii);
                if (id.v < r.lirToMir.size() && r.lirToMir[id.v].v == producer.v) {
                    out.push_back(id);
                }
            }
        }
    }
    return out;
}

// The LIR block holding `inst`.
[[nodiscard]] LirBlockId blockOf(Lir const& lir, LirInstId inst) {
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const b = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
                if (lir.blockInstAt(b, ii).v == inst.v) return b;
            }
        }
    }
    return LirBlockId{};
}

[[nodiscard]] std::uint32_t countOp(Lir const& lir, std::uint16_t opcode) {
    std::uint32_t n = 0;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const b = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
                if (lir.instOpcode(lir.blockInstAt(b, ii)) == opcode) ++n;
            }
        }
    }
    return n;
}

// How many terminators a block holds — asserted rather than assumed, because
// "the block was sealed twice" is the shape `LirBuilder` answers with a process
// abort and a test must be able to say it never happened.
[[nodiscard]] std::uint32_t
terminatorsIn(Lir const& lir, TargetSchema const& t, LirBlockId b) {
    std::uint32_t n = 0;
    for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
        if (t.isTerminator(lir.instOpcode(lir.blockInstAt(b, ii)))) ++n;
    }
    return n;
}

[[nodiscard]] std::string firstError(DiagnosticReporter const& r) {
    return r.all().empty() ? std::string{} : r.all()[0].actual;
}

} // namespace

// ── ARM 1: A BRANCHING TEMPLATE GETS **BOTH** EDGES ──────────────────────────
//
// `jne %l[hit]` is a CONDITIONAL branch: the engine's `buildCondBr` names the
// bound label as the taken target and MINTS an anonymous block for the false
// edge, leaving it open. The fall-through edge of the `asm goto` is then a
// branch `mir_to_lir` must emit into that open block — and if it does not, the
// code after the statement is reached by nothing at all.
TEST(LirAsmGotoLabels, ABranchingTemplateGetsBothTheLabelEdgeAndTheFallThrough) {
    auto L = lowerCSubsetToLir(
        "int f(int x){ int r; r = 0;\n"
        "  __asm__ goto (\"cmpl $0, %0\\n\\tjne %l[hit]\" : : \"r\"(x) : \"cc\" : hit);\n"
        "  r = 1; return r;\n"
        "hit: return 2; }",
        "x86_64");
    ASSERT_FALSE(L.model.hasErrors()) << firstError(L.model.diagnostics());
    ASSERT_TRUE(L.mir.ok) << firstError(L.mirReporter);
    ASSERT_TRUE(L.lir.ok)
        << "`asm goto` must LOWER — this is the refusal P20 retired: "
        << firstError(L.lirReporter);

    Lir const&  lir = L.lir.lir;
    auto const  jmp = opOf(*L.target, "jmp");
    auto const  g   = theAsmGoto(L.mir.mir);
    ASSERT_TRUE(g.has_value()) << "the source must lower to a MIR InlineAsmGoto";

    // MIR's own layout: [label0 … labelN-1, FALLTHROUGH]. One label here, so
    // exactly two successors — asserted, because every index below reads it.
    auto const succs = L.mir.mir.blockSuccessors(L.mir.mir.instBlock(*g));
    ASSERT_EQ(succs.size(), 2u)
        << "one label plus the fall-through — the successor layout every "
           "reader of `succs[j]` depends on";

    auto const mine = instsFrom(L.lir, *g);
    ASSERT_FALSE(mine.empty())
        << "the expansion emitted nothing of its own — the input `\"r\"(x)` "
           "must at least be materialised into the register the template names";
    LirBlockId const asmBlock = blockOf(lir, mine.front());
    ASSERT_TRUE(asmBlock.valid());

    // ── (a) THE TEMPLATE'S CONDITIONAL BRANCH SEALS THE ASM'S OWN BLOCK WITH
    // TWO SUCCESSORS. A label placeholder that had resolved to a REGISTER
    // instead (the pre-P20 shape, where `AsmOperandBinding` was the only
    // binding kind) would leave this block with no terminator at all.
    LirInstId const term = lir.blockTerminator(asmBlock);
    ASSERT_TRUE(term.valid()) << "the `asm goto` block must be sealed";
    EXPECT_NE(lir.instOpcode(term), jmp)
        << "`jne %l[hit]` is CONDITIONAL — sealing with an unconditional jump "
           "would choose an edge the programmer did not write";
    auto const asmSuccs = lir.blockSuccessors(asmBlock);
    ASSERT_EQ(asmSuccs.size(), 2u)
        << "the template's conditional branch records BOTH the taken label and "
           "the block minted for its false edge";
    EXPECT_EQ(terminatorsIn(lir, *L.target, asmBlock), 1u);

    // ── (b) ★★★ THE FALL-THROUGH BRANCH EXISTS, AND IT IS THIS STATEMENT'S.
    // `buildCondBr` minted the false-edge block and OPENED it, leaving it
    // EMPTY: without a branch emitted there, the statements after the asm are
    // reachable from nothing. Provenance is what makes this an assertion rather
    // than an observation — an unconditional `jmp` somewhere in the function
    // proves nothing, but one whose `lirToMir` is the `asm goto` terminator can
    // only have come from `sealAsmGotoEdges`.
    LirBlockId const fallBlock = asmSuccs[1];
    LirInstId const  fallTerm  = lir.blockTerminator(fallBlock);
    ASSERT_TRUE(fallTerm.valid())
        << "the block the template's conditional branch minted for its false "
           "edge is the `asm goto` FALL-THROUGH, and it must be sealed";
    EXPECT_EQ(lir.instOpcode(fallTerm), jmp)
        << "the fall-through edge is an unconditional branch onward";
    ASSERT_LT(fallTerm.v, L.lir.lirToMir.size());
    EXPECT_EQ(L.lir.lirToMir[fallTerm.v].v, g->v)
        << "the fall-through branch must be attributed to the `asm goto` "
           "statement — anything else means it came from elsewhere and this "
           "assertion is measuring the wrong instruction";
    EXPECT_EQ(terminatorsIn(lir, *L.target, fallBlock), 1u);

    // ── (c) THE TWO EDGES GO TO DIFFERENT BLOCKS. Binding both to one block
    // would satisfy (a) and (b) and still be a miscompile.
    auto const fallSuccs = lir.blockSuccessors(fallBlock);
    ASSERT_EQ(fallSuccs.size(), 1u);
    EXPECT_NE(asmSuccs[0].v, fallSuccs[0].v)
        << "the label edge and the fall-through edge must reach DIFFERENT "
           "blocks — one target for both is the branch the programmer did not "
           "write";
}

// ── ARM 2: AN UNCONDITIONAL TEMPLATE SEALS THE BLOCK ITSELF ──────────────────
//
// `jmp %l[done]` NEVER falls through, and `buildBr` opens no block. Emitting a
// trailing branch here would hit `LirBuilder::appendInst_`'s *"block already
// terminated"* fatal — a process kill on a legal program — so the stated answer
// is to emit nothing. This arm is the one that pins that answer; ARM 1 pins the
// opposite one, and the two together are why `openBlockIsTerminated` is asked
// rather than assumed.
TEST(LirAsmGotoLabels, AnUnconditionalTemplateSealsItsOwnBlockAndGetsNoSecondBranch) {
    auto L = lowerCSubsetToLir(
        "int f(int x){ int r; r = 0;\n"
        "  __asm__ goto (\"jmp %l[done]\" : : \"r\"(x) : : done);\n"
        "  r = 1; return r;\n"
        "done: return 2; }",
        "x86_64");
    ASSERT_FALSE(L.model.hasErrors()) << firstError(L.model.diagnostics());
    ASSERT_TRUE(L.mir.ok) << firstError(L.mirReporter);
    ASSERT_TRUE(L.lir.ok) << firstError(L.lirReporter);

    Lir const& lir = L.lir.lir;
    auto const jmp = opOf(*L.target, "jmp");
    auto const g   = theAsmGoto(L.mir.mir);
    ASSERT_TRUE(g.has_value());
    auto const mine = instsFrom(L.lir, *g);
    ASSERT_FALSE(mine.empty())
        << "the input `\"r\"(x)` must be materialised into the register the "
           "template names, so the statement owns at least one instruction";
    LirBlockId const asmBlock = blockOf(lir, mine.front());
    ASSERT_TRUE(asmBlock.valid());

    // ★ THE WHOLE ARM IN ONE NUMBER: exactly ONE terminator. The template's own
    // `jmp` sealed the block, and a second branch emitted for the (unreachable)
    // fall-through would hit `LirBuilder::appendInst_`'s *"block already
    // terminated"* fatal — i.e. this assertion can only fail by the process
    // dying first, which is precisely why the lowering has to ASK rather than
    // assume.
    EXPECT_EQ(terminatorsIn(lir, *L.target, asmBlock), 1u);
    LirInstId const term = lir.blockTerminator(asmBlock);
    ASSERT_TRUE(term.valid());
    EXPECT_EQ(lir.instOpcode(term), jmp)
        << "`jmp %l[done]` is the template's own unconditional branch";
    EXPECT_EQ(lir.blockSuccessors(asmBlock).size(), 1u)
        << "an unconditional branch has ONE successor — the fall-through edge "
           "of this `asm goto` is unreachable by the program's own text, and "
           "recording it as a LIR successor would claim a path that does not "
           "exist";

    // ⚠ AND NO INSTRUCTION OF THIS STATEMENT MAY FOLLOW THE TERMINATOR. The
    // trailing branch is the one this lowering would have emitted; asserting
    // the terminator is the block's LAST instruction is how "it emitted
    // nothing" is stated as a property rather than as a count.
    EXPECT_EQ(lir.blockInstAt(asmBlock, lir.blockInstCount(asmBlock) - 1).v,
              term.v);
}

// ── ARM 3: ★ RESULT PIECE 0 — THE SILENT ONE ─────────────────────────────────
//
// `Call`'s "output 0 IS the instruction's own value" rule does NOT hold for
// `InlineAsmGoto`: its `opcodeInfo` row is `R::None`, because a terminator has
// nothing after it to carry a result, and `MirBuilder::addInlineAsmGoto` places
// a `ReturnPiece` for EVERY output — ordinal 0 included — at each successor's
// head. If the expansion followed `Call`'s rule anyway, ordinal 0's piece would
// MISS the `asmPieceReg_` lookup in `lowerReturnPiece` and fall through to the
// `MnemonicSlot::RetPiece` capture, which reads the CALLING CONVENTION'S return
// register instead of the one the `"=r"` constraint named.
//
// ★★ THE ASSERTION IS THE ABSENCE OF A `ret_piece`, AND THAT IS EXACTLY THE
// DISCRIMINATOR: the defect's whole signature is that a `ret_piece` gets
// emitted for a producer that is not a call. Nothing else in this module can
// emit one (there is no struct-returning call), so the count is 0 iff piece 0
// was published — and the emitted-`mov` check below is the anti-vacuity half,
// without which "no ret_piece" would also be true of a module where the asm
// never lowered at all.
TEST(LirAsmGotoLabels, ResultPieceZeroIsPublishedByTheExpansionNotByTheCallConvention) {
    auto L = lowerCSubsetToLir(
        "int f(int x){ int r; r = 0;\n"
        "  __asm__ goto (\"movl $7, %0\\n\\tcmpl $0, %1\\n\\tjne %l2\"\n"
        "                : \"=&r\"(r) : \"r\"(x) : \"cc\" : hit);\n"
        "  return r;\n"
        "hit: return 100; }",
        "x86_64");
    ASSERT_FALSE(L.model.hasErrors()) << firstError(L.model.diagnostics());
    ASSERT_TRUE(L.mir.ok) << firstError(L.mirReporter);
    ASSERT_TRUE(L.lir.ok)
        << "the POSITIONAL label spelling `%l2` must resolve — its index base "
           "is `#outputs + #source-inputs + labelPosition`, so with one output "
           "and one input the label is index 2: "
        << firstError(L.lirReporter);

    Lir const& lir = L.lir.lir;
    // Anti-vacuity: the template really lowered — its `movl $7, %0` is here.
    auto const mov = opOf(*L.target, "mov");
    bool sawTemplateMov = false;
    LirReg templateResult = InvalidLirReg;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const b = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
                LirInstId const id = lir.blockInstAt(b, ii);
                if (lir.instOpcode(id) != mov) continue;
                for (auto const& o : lir.instOperands(id)) {
                    if (o.kind == LirOperandKind::ImmInt && o.immInt32 == 7) {
                        sawTemplateMov = true;
                        templateResult = lir.instResult(id);
                    }
                }
            }
        }
    }
    ASSERT_TRUE(sawTemplateMov)
        << "anti-vacuity: the template's `movl $7, %0` must be in the module, "
           "or the `ret_piece` count below is 0 for the wrong reason";
    EXPECT_TRUE(templateResult.valid())
        << "the template's `movl $7, %0` must DEFINE the operand's register — "
           "that register IS result piece 0";

    EXPECT_EQ(countOp(lir, opOf(*L.target, "ret_piece")), 0u)
        << "an `asm goto` output must be read out of the register its "
           "constraint named. A `ret_piece` here means ordinal 0 missed the "
           "expansion's own publication and is being captured from the calling "
           "convention's return register instead — a silent miscompile with no "
           "diagnostic at any tier";
}

// ── ARM 4: THE SYMBOLIC OPERAND NAME REACHES THE TEMPLATE ────────────────────
//
// `%[in]` and `%[out]` are the SECOND spelling of an operand that already
// answers to `%0`/`%1`. ✔MEASURED before P20: this exact statement was refused
// — *"'%[in]' names neither a register this target declares nor one of the 2
// operand(s) bound to this assembly template ('%0', '%1')"* — because the
// binding table carried one row per operand rather than one per SPELLING.
TEST(LirAsmGotoLabels, ASymbolicOperandNameIsOneMoreBindingRowForTheSameRegister) {
    auto L = lowerCSubsetToLir(
        "int f(int a){ int out; out = 0;\n"
        "  __asm__ (\"movl %[in], %[out]\" : [out] \"=r\"(out) : [in] \"r\"(a));\n"
        "  return out; }",
        "x86_64");
    ASSERT_FALSE(L.model.hasErrors()) << firstError(L.model.diagnostics());
    ASSERT_TRUE(L.mir.ok) << firstError(L.mirReporter);
    ASSERT_TRUE(L.lir.ok)
        << "a `%[name]` operand reference must resolve: " << firstError(L.lirReporter);

    // The positional form of the SAME operands must still resolve — the
    // symbolic row is an ADDITION, and a lowering that replaced the positional
    // row with it would pass the arm above and break every existing template.
    auto P = lowerCSubsetToLir(
        "int f(int a){ int out; out = 0;\n"
        "  __asm__ (\"movl %1, %0\" : [out] \"=r\"(out) : [in] \"r\"(a));\n"
        "  return out; }",
        "x86_64");
    ASSERT_TRUE(P.lir.ok)
        << "a NAMED operand still answers to its positional spelling — GNU "
           "binds both (6.47.2.3): " << firstError(P.lirReporter);
}

// ── ARM 5: A REGISTER-PINNED OUTPUT IS CAPTURED **ON THE EDGES** ─────────────
//
// `expandInlineAsm` reads every pinned output out of its physical register
// immediately after the template. An `asm goto` template ENDS the block, so
// there is no "after" — the captures move to the head of each edge, and the
// label edges get an interposed block to hold them.
//
// ★ THE ASSERTION IS THAT THE VALUE LEAVES THE PHYSICAL REGISTER AT ALL. A
// lowering that published the pinned register itself as result piece 0 would
// compile, and the value would survive right up until anything else wanted
// `rax` — the class of bug that shows up as a wrong answer three cycles later
// in a program that merely got bigger.
TEST(LirAsmGotoLabels, APinnedOutputIsCapturedOutOfItsRegisterOnEveryEdge) {
    auto L = lowerCSubsetToLir(
        "int f(int x){ int r; r = 0;\n"
        "  __asm__ goto (\"movl $9, %0\\n\\tcmpl $0, %1\\n\\tjne %l2\"\n"
        "                : \"=a\"(r) : \"r\"(x) : \"cc\" : hit);\n"
        "  return r;\n"
        "hit: return 100; }",
        "x86_64");
    ASSERT_FALSE(L.model.hasErrors()) << firstError(L.model.diagnostics());
    ASSERT_TRUE(L.mir.ok) << firstError(L.mirReporter);
    ASSERT_TRUE(L.lir.ok)
        << "a register-pinned output on an `asm goto` must lower: "
        << firstError(L.lirReporter);

    Lir const& lir = L.lir.lir;
    auto const mov = opOf(*L.target, "mov");
    auto const g   = theAsmGoto(L.mir.mir);
    ASSERT_TRUE(g.has_value());

    // One capture per EDGE — the fall-through's, emitted into the block the
    // template left open, and one per label edge in its interposed block. Both
    // write the SAME vreg, because the MIR reads the piece in one place.
    std::vector<LirInstId> captures;
    for (LirInstId const id : instsFrom(L.lir, *g)) {
        if (lir.instOpcode(id) != mov) continue;
        auto const ops = lir.instOperands(id);
        if (ops.size() != 1 || ops[0].kind != LirOperandKind::Reg) continue;
        if (ops[0].reg.isPhysical == 0) continue;   // an input materialisation
        captures.push_back(id);
    }
    ASSERT_EQ(captures.size(), 2u)
        << "one capture per edge — the label edge and the fall-through — and a "
           "template that ends the block has no other place to put them";
    EXPECT_EQ(lir.instResult(captures[0]).isPhysical, 0u)
        << "the capture's DESTINATION is a vreg: the whole point is that the "
           "value stops living in the physical register the constraint named";
    EXPECT_EQ(lir.instResult(captures[0]), lir.instResult(captures[1]))
        << "both edges must capture into ONE register — the MIR reads the piece "
           "in a single place, so two registers would make one path read a "
           "value nothing on it ever wrote";
}

// ── ARM 6: THE REFUSALS, WHICH MUST NOT BECOME ACCEPTANCE ────────────────────
//
// Replacing a fail-loud refusal with a silent branch is the worst outcome
// available here, so the two shapes that remain unbindable are pinned WITH
// their diagnostics: a `.s`-style label inside a template (nothing bound it)
// and a label placeholder used as a VALUE rather than as a branch target.
TEST(LirAsmGotoLabels, AnUnboundBranchTargetIsRefusedNamingTheBoundLabels) {
    auto L = lowerCSubsetToLir(
        "int f(int x){ int r; r = 0;\n"
        "  __asm__ goto (\"jmp Lnowhere\" : : \"r\"(x) : : done);\n"
        "  r = 1; return r;\n"
        "done: return 2; }",
        "x86_64");
    ASSERT_FALSE(L.model.hasErrors()) << firstError(L.model.diagnostics());
    ASSERT_TRUE(L.mir.ok) << firstError(L.mirReporter);
    EXPECT_FALSE(L.lir.ok)
        << "a template branching to a label NOBODY bound must be refused — a "
           "LIR block reference is function-local, so binding one anyway names "
           "whichever block sits at that index in the caller";
    bool named = false;
    for (auto const& d : L.lirReporter.all()) {
        if (d.actual.find("`asm goto` label(s) bound to this assembly template")
            != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named)
        << "the refusal must name the BOUND label set — the label list is the "
           "embedding language's, so only its host can enumerate it, and the "
           "count is the whole diagnosis; got: " << firstError(L.lirReporter);
}

// ── ARM 7: A REFUSAL AFTER THE CAPTURE BLOCKS WERE MINTED STILL **REPORTS** ──
//
// ★★ THIS ARM EXERCISES A FAILURE PATH RATHER THAN READING IT. The interposed
// capture blocks are created BEFORE the template runs, because the label
// bindings have to name them — and `LirBuilder::closeFunction` treats a block
// that was created and never opened as a PROCESS ABORT, not as a leak. So a
// refusal raised between the two (here: an unlowerable mnemonic, with a pinned
// output so the blocks exist) has to seal them on the way out.
// ⚠ Getting this wrong does not fail this test, it KILLS the process running
// it — which is exactly why it is pinned as an arm rather than argued in a
// comment. ✔MEASURED 2026-08-19 with `sealOrphanedAsmCaptureBlocks` neutered:
// `LirBuilder::closeFunction: block created but never `beginBlock`'d`, on a
// program whose only defect was a typo'd instruction.
TEST(LirAsmGotoLabels, ARefusalAfterTheCaptureBlocksExistIsReportedNotAborted) {
    auto L = lowerCSubsetToLir(
        "int f(int x){ int r; r = 0;\n"
        "  __asm__ goto (\"frobnicate %0\" : \"=a\"(r) : \"r\"(x) : : done);\n"
        "  return r;\n"
        "done: return 2; }",
        "x86_64");
    ASSERT_FALSE(L.model.hasErrors()) << firstError(L.model.diagnostics());
    ASSERT_TRUE(L.mir.ok) << firstError(L.mirReporter);
    EXPECT_FALSE(L.lir.ok)
        << "an instruction the dialect does not declare must be refused";
    bool named = false;
    for (auto const& d : L.lirReporter.all()) {
        if (d.actual.find("unknown mnemonic 'frobnicate'") != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named)
        << "the diagnostic must be the dialect's own, forwarded — reaching this "
           "line at all is the arm's real assertion; got: "
        << firstError(L.lirReporter);
}
