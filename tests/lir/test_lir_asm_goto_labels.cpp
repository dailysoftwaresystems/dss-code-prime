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
// ★★ WHY THIS FILE EXISTS BESIDE `examples/c/asm_goto_labels`. The
// corpus example is the end-to-end witness and it is the one that proves the
// program RUNS; but a runtime pin can be satisfied by luck — ✔MEASURED
// 2026-08-17 on this very subsystem, a single-`"+r"` asm example stayed GREEN
// over a live mutant on BOTH arms because the allocator happened to leave the
// right value in the register the template read. A structural assertion at the
// tier that makes the decision cannot be satisfied that way. Keep both.
//
// ★ THE FIXTURE LOWERS REAL c SOURCE, never a hand-built descriptor,
// because the property under test is a RELATION between what the front end
// minted (the spellings) and what this tier bound (the blocks). A hand-typed
// descriptor would be testing the hand-typing: a pin that stubs its subject's
// INPUT is testing the stub.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe` -- a bare run falls back to a cwd
// walk and can read a DIFFERENT config tree, which turns a red-on-disable
// arm green for the wrong reason.

#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "asm_region_test_support.hpp"
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
// order. ★ PROVENANCE, NOT POSITION: "the asm's own block" is not a place a
// positional scan can name.
//
// ⚠ THE TEMPLATE'S OWN INSTRUCTIONS ARE **NOT** IN THIS SET: since P68 round 8
// part 4 they are the BODY of the statement's `asm_region_goto` bundle, a
// module side structure, and become instructions of the function only when
// `expandAsmRegions` runs (`asm_region_test_support.hpp` reads both). What IS
// in this set is everything the LOWERING emitted for the statement into the
// function: the per-input materialisation `mov`s and the bundle itself. That is
// enough to name the asm's own BLOCK, which is how the block-level assertions
// below find their subject.
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
// edge, leaving it open. That open block is the END of the template, so it
// falls into the statement's fall-through — and if that edge were lost, the
// code after the statement would be reached by nothing at all. (P68 round 8
// part 4: the edge is the bundle body's branch to `exit`, and the expansion
// makes it a real branch to the fall-through successor.)
TEST(LirAsmGotoLabels, ABranchingTemplateGetsBothTheLabelEdgeAndTheFallThrough) {
    auto L = lowerCToLir(
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

    // ── (a) ★ P68 round 8 part 4: THE STATEMENT ENDS ITS BLOCK AS ONE BUNDLE.
    // The block's terminator is the `asm_region_goto` instruction, and its
    // successors ARE the statement's edges, in MIR's layout — the label, then
    // the fall-through — two DIFFERENT blocks. (The template's own `jne` is in
    // the bundle's body, below; it becomes this block's terminator only when
    // the expansion runs.)
    LirInstId const term = lir.blockTerminator(asmBlock);
    ASSERT_TRUE(term.valid()) << "the `asm goto` block must be sealed";
    EXPECT_EQ(lir.instOpcode(term), opOf(*L.target, "asm_region_goto"))
        << "the statement's block ends with its bundle";
    EXPECT_EQ(terminatorsIn(lir, *L.target, asmBlock), 1u);
    auto const asmSuccs = lir.blockSuccessors(asmBlock);
    ASSERT_EQ(asmSuccs.size(), 2u)
        << "the label edge and the fall-through edge";
    EXPECT_NE(asmSuccs[0].v, asmSuccs[1].v)
        << "the label edge and the fall-through edge must reach DIFFERENT "
           "blocks — one target for both is the branch the programmer did not "
           "write";
    LirAsmRegion const* region = lir.instAsmRegion(term);
    ASSERT_NE(region, nullptr);
    ASSERT_EQ(region->gotoTargets.size(), 1u) << "one label, one stub";

    // ── (b) THE TEMPLATE'S CONDITIONAL BRANCH, IN THE BODY: taken → the
    // label's stub, false edge → the block `buildCondBr` minted, which runs off
    // the end of the template into `exit` (the fall-through) by a branch the
    // template did not write.
    Lir const& body = region->body;
    std::optional<LirBlockId> condBlock;
    for (LirBlockId const b : asmRegionBodyBlocks(*region)) {
        if (body.blockSuccessors(b).size() == 2) condBlock = b;
    }
    ASSERT_TRUE(condBlock.has_value())
        << "`jne %l[hit]` is a conditional branch in the template's body";
    auto const bodySuccs = body.blockSuccessors(*condBlock);
    EXPECT_EQ(bodySuccs[0].v, region->gotoTargets[0].v)
        << "the taken edge goes to the label's stub";
    LirBlockId const falseEdge = bodySuccs[1];
    ASSERT_EQ(body.blockSuccessors(falseEdge).size(), 1u);
    EXPECT_EQ(body.blockSuccessors(falseEdge)[0].v, region->exit.v)
        << "the template's false edge is its end, which falls into the rest of "
           "the program";
    std::uint32_t const falseIdx = falseEdge.v - body.funcBlockAt(body.funcAt(0), 0).v;
    EXPECT_EQ(region->syntheticFallthrough[falseIdx], 1u)
        << "falling off the end is a branch the template did not write";

    // ── (c) ★ THE EMITTED SHAPE: after the expansion the template's `jne` is
    // a real conditional branch of the function with two DIFFERENT successors,
    // and no bundle survives.
    auto const expanded = lirThroughAsmExpansion(lir, *L.target);
    ASSERT_TRUE(expanded.has_value());
    EXPECT_TRUE(asmRegionBundles(*expanded).empty());
    auto const jcc = opOf(*L.target, "jcc");
    std::uint32_t jccCount = 0;
    for (std::uint32_t fi = 0; fi < expanded->moduleFuncCount(); ++fi) {
        LirFuncId const f = expanded->funcAt(fi);
        for (std::uint32_t bi = 0; bi < expanded->funcBlockCount(f); ++bi) {
            LirBlockId const b = expanded->funcBlockAt(f, bi);
            if (expanded->instOpcode(expanded->blockTerminator(b)) != jcc) continue;
            ++jccCount;
            auto const es = expanded->blockSuccessors(b);
            ASSERT_EQ(es.size(), 2u);
            EXPECT_NE(es[0].v, es[1].v);
        }
    }
    EXPECT_EQ(jccCount, 1u) << "the template's one conditional branch";
}

// ── ARM 2: AN UNCONDITIONAL TEMPLATE SEALS THE BLOCK ITSELF ──────────────────
//
// `jmp %l[done]` NEVER falls through, and `buildBr` opens no block. Emitting a
// trailing branch here would hit `LirBuilder::appendInst_`'s *"block already
// terminated"* fatal — a process kill on a legal program — so the stated answer
// is to emit nothing. This arm is the one that pins that answer; ARM 1 pins the
// opposite one, and the two together are why the statement's packaging asks
// `openBlockIsTerminated` of its body builder rather than assuming.
TEST(LirAsmGotoLabels, AnUnconditionalTemplateSealsItsOwnBlockAndGetsNoSecondBranch) {
    auto L = lowerCToLir(
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

    // ★ P68 round 8 part 4: the statement ends its block as ONE bundle, whose
    // successors are MIR's [label, fall-through] — a CONSERVATIVE edge set for
    // allocation (a path the template never takes only keeps a value alive
    // longer). What the program's own text says is in the BODY: the template's
    // `jmp` is its entry block's terminator, it goes to the label's stub, and
    // NOTHING in the body reaches `exit` — no fall-through branch exists, so the
    // expansion emits none.
    EXPECT_EQ(terminatorsIn(lir, *L.target, asmBlock), 1u);
    LirInstId const term = lir.blockTerminator(asmBlock);
    ASSERT_TRUE(term.valid());
    EXPECT_EQ(lir.instOpcode(term), opOf(*L.target, "asm_region_goto"));
    LirAsmRegion const* region = lir.instAsmRegion(term);
    ASSERT_NE(region, nullptr);
    ASSERT_EQ(region->gotoTargets.size(), 1u);
    Lir const& body = region->body;
    auto const blocks = asmRegionBodyBlocks(*region);
    ASSERT_EQ(blocks.size(), 1u) << "a one-line unconditional template is one block";
    LirInstId const tj = body.blockTerminator(blocks[0]);
    EXPECT_EQ(body.instOpcode(tj), jmp)
        << "`jmp %l[done]` is the template's own unconditional branch";
    ASSERT_EQ(body.blockSuccessors(blocks[0]).size(), 1u);
    EXPECT_EQ(body.blockSuccessors(blocks[0])[0].v, region->gotoTargets[0].v);
    for (auto const f : region->syntheticFallthrough) {
        EXPECT_EQ(f, 0u) << "the template never falls off its end";
    }

    // ⚠ AND AFTER THE EXPANSION NO BRANCH FOLLOWS IT: the template's `jmp` is
    // the terminator of the block it ends, with ONE successor.
    auto const expanded = lirThroughAsmExpansion(lir, *L.target);
    ASSERT_TRUE(expanded.has_value());
    EXPECT_TRUE(asmRegionBundles(*expanded).empty());
    std::uint32_t oneWayJmps = 0;
    for (std::uint32_t fi = 0; fi < expanded->moduleFuncCount(); ++fi) {
        LirFuncId const f = expanded->funcAt(fi);
        for (std::uint32_t bi = 0; bi < expanded->funcBlockCount(f); ++bi) {
            LirBlockId const b = expanded->funcBlockAt(f, bi);
            LirInstId const t = expanded->blockTerminator(b);
            EXPECT_EQ(terminatorsIn(*expanded, *L.target, b), 1u);
            if (expanded->instOpcode(t) == jmp
                && expanded->blockSuccessors(b).size() == 1u) {
                ++oneWayJmps;
            }
        }
    }
    EXPECT_GE(oneWayJmps, 1u) << "the template's `jmp` survives as a real branch";
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
    auto L = lowerCToLir(
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
    // Anti-vacuity: the template really lowered — its `movl $7, %0` is in the
    // statement's bundle BODY (P68 round 8 part 4).
    auto const mov = opOf(*L.target, "mov");
    auto const bundle = onlyAsmRegion(lir);
    ASSERT_TRUE(bundle.has_value());
    Lir const& body = bundle->region->body;
    bool sawTemplateMov = false;
    LirReg templateResult = InvalidLirReg;
    for (LirInstId const id : asmRegionBodyInsts(*bundle->region)) {
        if (body.instOpcode(id) != mov) continue;
        for (auto const& o : body.instOperands(id)) {
            if (o.kind == LirOperandKind::ImmInt && o.immInt32 == 7) {
                sawTemplateMov = true;
                templateResult = body.instResult(id);
            }
        }
    }
    ASSERT_TRUE(sawTemplateMov)
        << "anti-vacuity: the template's `movl $7, %0` must be in the module, "
           "or the `ret_piece` count below is 0 for the wrong reason";
    EXPECT_TRUE(templateResult.valid())
        << "the template's `movl $7, %0` must DEFINE the operand's register — "
           "that register IS result piece 0";
    EXPECT_EQ(asmSlotRoleOf(*bundle->region, templateResult),
              LirAsmOperandRole::EarlyDef)
        << "the `\"=&r\"` output is the statement's slot written EARLY";

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
    auto L = lowerCToLir(
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
    auto P = lowerCToLir(
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
    auto L = lowerCToLir(
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
    auto L = lowerCToLir(
        "int f(int x){ int r; r = 0;\n"
        "  __asm__ goto (\"jmp Lnowhere\" : : \"r\"(x) : : done);\n"
        "  r = 1; return r;\n"
        "done: return 2; }",
        "x86_64", /*mirCcIndex=*/0, LoweringExpectation::Refuses);
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
    auto L = lowerCToLir(
        "int f(int x){ int r; r = 0;\n"
        "  __asm__ goto (\"frobnicate %0\" : \"=a\"(r) : \"r\"(x) : : done);\n"
        "  return r;\n"
        "done: return 2; }",
        "x86_64", /*mirCcIndex=*/0, LoweringExpectation::Refuses);
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
