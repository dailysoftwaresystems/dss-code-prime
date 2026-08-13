// Standalone assembly TEXT → LIR (`src/asm/asm_text_to_lir.cpp`).
//
// ★ THE WALKER HAD ZERO TESTS BEFORE THIS FILE. Everything it does was
// exercised only end-to-end through one examples-corpus row, which means every
// refusal arm, every block-model decision and every election was unverified.
//
// ★★ WHAT EACH REFUSAL TEST IS REALLY ASSERTING, AND WHY IT MATTERS THAT THE
// PROCESS SURVIVES. Several of the shapes below (a label falling into another
// label, a block that is created and never opened, an instruction after a
// terminator) sit directly on top of `LirBuilder` calls that `lirFatal` — i.e.
// ABORT THE PROCESS. A process abort is not fail-loud: it prints no span, names
// no file, and destroys every sibling test's verdict in the same binary. So
// each of these tests asserts BOTH the diagnostic code AND — by the mere fact
// that the gtest binary reaches its own exit — that no abort happened. A
// regression that reinstates the abort turns the whole binary red, which is the
// loudest possible signal.

#include "asm_text_fixture.hpp"
#include "asm_test_support.hpp"
#include "asm/asm.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::test_support::asm_text;
using dss::test_support::asm_::countDiagnostics;

namespace {

constexpr auto kAsmCode = DiagnosticCode::A_AsmTextUnsupported;

// A function body written the way gas writes one, with the marker-less
// function-entry directive so the test's subject is the BLOCK MODEL and not
// the token table.
std::string src(std::string body) { return body; }

// Count blocks in the module's single function.
struct FuncShape {
    std::size_t functions = 0;
    std::size_t blocks    = 0;
    std::size_t symbols   = 0;
};

FuncShape shapeOf(AsmTextModule const& m) {
    FuncShape s;
    s.functions = m.lir.moduleFuncCount();
    s.symbols   = m.symbols.size();
    for (std::size_t i = 0; i < s.functions; ++i) {
        auto const fn = m.lir.funcAt(static_cast<std::uint32_t>(i));
        s.blocks += m.lir.funcBlockCount(fn);
    }
    return s;
}

// Mnemonic -> opcode ordinal, through the SCHEMA (the Lir does not carry the
// table). Aborts the test rather than the process on a typo.
std::uint16_t op(TargetSchema const& schema, std::string_view mnemonic) {
    auto const v = schema.opcodeByMnemonic(mnemonic);
    EXPECT_TRUE(v.has_value()) << "unknown mnemonic " << mnemonic;
    return v.value_or(0);
}

// How many times `window` occurs in `bytes`. ⚠ A COUNT, NOT A "CONTAINS": the
// interesting failure for a condition-carrying encoder is a SECOND copy of the
// zero-condition window, which a containment check reports as green.
std::size_t countWindow(std::vector<std::uint8_t> const&    bytes,
                        std::initializer_list<std::uint8_t> window) {
    if (window.size() == 0 || bytes.size() < window.size()) return 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i + window.size() <= bytes.size(); ++i) {
        bool hit = true;
        std::size_t k = 0;
        for (auto const b : window) {
            if (bytes[i + k] != b) { hit = false; break; }
            ++k;
        }
        if (hit) ++n;
    }
    return n;
}

// The encoded stream, for a failure message that shows what WAS emitted.
std::string hex(std::vector<std::uint8_t> const& bytes) {
    std::string out = "bytes:";
    for (auto const b : bytes) out += std::format(" {:02X}", b);
    return out;
}

// Every INDIRECT-BRANCH terminator in `lir`, rendered as
// `<successors>/<blocks in its function>`; empty ⇒ the module holds none.
//
// ★★ THE RATIO IS THE POINT, because the two UNSOUND answers to "what does an
// indirect branch reach" differ in exactly it: `0/N` is the under-approximation
// (control leaves the function, so liveness and regalloc judge every value live
// across the edge dead) and `N/N` is the over-approximation (the compiler
// deciding which labels the author meant). One probe therefore covers both, and
// names WHICH one a regression emitted instead of reporting a bare boolean.
//
// ⚠ KEYED ON THE TARGET'S OWN `TargetTerminatorKind::IndirectBr`, NEVER ON A
// MNEMONIC. `jmp_indirect` is x86_64's spelling and arm64 uses the same name
// today, but a third CPU's would not be — and a probe that quietly saw nothing
// on it would report "" , which is the exact string this file treats as PASS.
std::string indirectBrShapes(TargetSchema const& schema, Lir const& lir) {
    std::string out;
    auto const nf = static_cast<std::uint32_t>(lir.moduleFuncCount());
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        auto const          fn = lir.funcAt(fi);
        std::uint32_t const nb = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            auto const          b  = lir.funcBlockAt(fn, bi);
            std::uint32_t const ni = lir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                auto const* info =
                    schema.opcodeInfo(lir.instOpcode(lir.blockInstAt(b, ii)));
                if (info == nullptr) continue;
                if (info->terminatorKind != TargetTerminatorKind::IndirectBr) {
                    continue;
                }
                if (!out.empty()) out += ' ';
                out += std::format("{}/{}", lir.blockSuccessors(b).size(), nb);
            }
        }
    }
    return out;
}

// The same probe over a lowering RUN, so the assertion reads the same whether
// the walker produced a module or refused. ⚠ "<no module>" is deliberately NOT
// "" — a refusal and a module holding no indirect branch are different facts,
// and collapsing them would let a test pass on the wrong one.
std::string indirectBrShapes(LoweringRun const& run) {
    if (!run.module.has_value()) return "<no module>";
    return indirectBrShapes(*run.target, run.module->lir);
}

// A dialect whose `jmp` names BOTH target opcodes — the honest gas row, and the
// one the shipped `asm-x86_64-att.lang.json` now carries.
nlohmann::json kindSplitJmpDoc() {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    for (auto& r : rows) {
        if (r.spelling == "jmp") r.opcodes = {"jmp", "jmp_indirect"};
    }
    setInstructions(doc, rows);
    return doc;
}

} // namespace

// ── the block model ───────────────────────────────────────────────────────

// ★ ONE FUNCTION, N INTERIOR LABELS ⇒ ONE ModuleSymbol AND N+1 BLOCKS. This is
// the whole point of the function-entry directive: before it, every label
// minted its own LIR function and a `jmp L1` crossed a function boundary.
TEST(AsmTextToLir, InteriorLabelsBecomeBlocksNotFunctions) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".text\n"
        ".globl main\n"
        ".func main\n"
        "main:\n"
        "  jmp L1\n"
        "L1:\n"
        "  jmp L2\n"
        "L2:\n"
        "  jmp L3\n"
        "L3:\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const s = shapeOf(*run->module);
    EXPECT_EQ(s.functions, 1u);
    EXPECT_EQ(s.symbols,   1u) << "one function-entry label ⇒ one ModuleSymbol";
    EXPECT_EQ(s.blocks,    4u) << "entry + three interior labels";
    EXPECT_EQ(run->module->symbols[0].name, "main");
    EXPECT_EQ(run->module->symbols[0].binding, SymbolBinding::Global);
}

// ★ TWO ENTRY MARKERS ⇒ TWO FUNCTIONS, each with its own blocks.
TEST(AsmTextToLir, TwoEntryMarkersMintTwoFunctions) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".func a\n"
        ".func b\n"
        "a:\n"
        "  jmp La\n"
        "La:\n"
        "  ret\n"
        "b:\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);
    auto const s = shapeOf(*run->module);
    EXPECT_EQ(s.functions, 2u);
    EXPECT_EQ(s.symbols,   2u);
    EXPECT_EQ(s.blocks,    3u) << "a: entry + La, b: entry";
    EXPECT_EQ(run->module->symbols[0].name, "a");
    EXPECT_EQ(run->module->symbols[1].name, "b");
    // Not `.globl`'d ⇒ module-private, via the EXISTING SymbolBinding
    // vocabulary rather than a parallel label-kind enum.
    EXPECT_EQ(run->module->symbols[0].binding, SymbolBinding::Local);
}

// ★ THE MARKER IS WHAT SEPARATES `.type main, function` FROM `.type buf,
// object`. Without it, every `.type`d symbol would become a function.
TEST(AsmTextToLir, MarkerDistinguishesFunctionFromOtherTypedSymbols) {
    auto const marked = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n"
        ".type main, function\n"
        "main:\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*marked)) << parseMessages(*marked);
    ASSERT_TRUE(marked->module.has_value()) << messages(*marked);
    EXPECT_EQ(shapeOf(*marked->module).functions, 1u);

    auto const unmarked = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n"
        ".type main, object\n"
        "main:\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*unmarked)) << parseMessages(*unmarked);
    EXPECT_FALSE(unmarked->module.has_value())
        << "a non-function `.type` must not mint a function";
    EXPECT_EQ(countDiagnostics(unmarked->reporter, kAsmCode), 1u);
}

// ★★ NO ENTRY MARKER ⇒ A PRECISE DIAGNOSTIC THAT NAMES THE LABELS. Operator
// ruling: never infer an entry from branch/call targets, never fall back to
// one-label-one-function.
TEST(AsmTextToLir, NoEntryMarkerNamesEveryUnclassifiedLabel) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".text\n"
        "main:\n"
        "  jmp L1\n"
        "L1:\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_EQ(countDiagnostics(run->reporter, kAsmCode), 1u);

    auto const text = messages(*run);
    EXPECT_NE(text.find("'main'"), std::string::npos) << text;
    EXPECT_NE(text.find("'L1'"),  std::string::npos) << text;
    // It must name the DIRECTIVE the reader has to write, taken from the
    // dialect's own rows rather than hard-coded in the engine...
    EXPECT_NE(text.find("'func'"), std::string::npos) << text;
    EXPECT_NE(text.find("'type'"), std::string::npos) << text;
    // ...and both config documents.
    EXPECT_NE(text.find("target 'x86_64'"), std::string::npos) << text;
}

// ★ A DIALECT WITH NO `functionEntry` ROW AT ALL SAYS SO, rather than naming an
// empty directive list.
TEST(AsmTextToLir, DialectWithoutFunctionEntryVerbSaysSo) {
    auto doc = baseDialectDoc();
    setDirectives(doc, {{"text", "sectionText", ""},
                        {"globl", "globalSymbol", ""}});
    auto const run = lowerAsmText(doc, src(".globl main\nmain:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("declares NO 'functionEntry' directive"),
              std::string::npos)
        << messages(*run);
}

// ── conditional branches ──────────────────────────────────────────────────

// ★★ A `jcc` CARRIES TWO BlockRef OPERANDS **AND** TWO RECORDED SUCCESSORS.
// `addCondBr` does not synthesize either one — the walker must mint the
// anonymous fallthrough block itself and pass both edges explicitly. A
// regression that passed only the taken target would leave the encoder writing
// a displacement to whatever operand[1] happened to be.
TEST(AsmTextToLir, CondBranchCarriesBothEdgesAndBothSuccessors) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n"
        ".func main\n"
        "main:\n"
        "  cmpq %rbx, %rax\n"
        "  je Ldone\n"
        "  movq $1, %rax\n"
        "Ldone:\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const& lir = run->module->lir;
    auto const  jcc = op(*run->target, "jcc");
    bool        seen = false;
    auto const  fn   = lir.funcAt(0);
    for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
        auto const blk = lir.funcBlockAt(fn, bi);
        for (std::uint32_t ii = 0; ii < lir.blockInstCount(blk); ++ii) {
            auto const inst = lir.blockInstAt(blk, ii);
            if (lir.instOpcode(inst) != jcc) continue;
            seen = true;
            auto const ops = lir.instOperands(inst);
            ASSERT_EQ(ops.size(), 2u);
            EXPECT_EQ(ops[0].kind, LirOperandKind::BlockRef);
            EXPECT_EQ(ops[1].kind, LirOperandKind::BlockRef);
            EXPECT_NE(ops[0].blockSlot, ops[1].blockSlot);
            auto const succs = lir.blockSuccessors(blk);
            ASSERT_EQ(succs.size(), 2u);
            EXPECT_EQ(succs[0].v, ops[0].blockSlot);
            EXPECT_EQ(succs[1].v, ops[1].blockSlot);
            // The condition rides the payload, resolved from the dialect's
            // `cond` key against the substrate's TargetCondCode vocabulary.
            EXPECT_EQ(lir.instPayload(inst),
                      static_cast<std::uint32_t>(TargetCondCode::Eq));
        }
    }
    EXPECT_TRUE(seen) << "the conditional branch never reached LIR";
}

// A `cond-br` row without `cond` is a CONFIG defect and is refused before the
// first statement — otherwise the branch would silently take the target's
// zero-code edge.
TEST(AsmTextToLir, CondBranchRowWithoutCondIsRefused) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    for (auto& r : rows) {
        if (r.spelling == "je") r.cond.clear();
    }
    setInstructions(doc, rows);
    // ★★ THE CHECK MOVED FROM LOAD TIME TO EMIT TIME ON 2026-08-13, so the `.s`
    // now WRITES `je`. It used to say "the `.s` NEVER WRITES `je`. The refusal
    // is about the ROW, so it must fire anyway"; that stopped being expressible
    // when aarch64's `cset x0, eq` proved a row with no `cond` can be a legal
    // OPERAND-carrying spelling rather than a mistake
    // (D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED). ⚠ WHAT THE MOVE MUST NOT
    // COST is the guarantee, and it does not: the condition must arrive from
    // the row OR from an operand, and neither-source is still a refusal naming
    // both — a branch can never silently take the target's zero-code edge.
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  je Lend\nLend:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("neither this dialect's row (no 'cond' key) "
                                  "nor this instruction's operands names one"),
              std::string::npos)
        << messages(*run);
}

// The mirror: a condition on something that is not a conditional branch would
// be silently dropped.
TEST(AsmTextToLir, CondOnNonCondBranchRowIsRefused) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    for (auto& r : rows) {
        if (r.spelling == "movq") r.cond = "eq";
    }
    setInstructions(doc, rows);
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("nowhere to go"), std::string::npos)
        << messages(*run);
}

// A condition name the substrate does not know, and one the TARGET declares no
// encoding for, both fail here rather than at the encoder.
TEST(AsmTextToLir, UnknownConditionNameIsRefusedAtDialectResolution) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    for (auto& r : rows) {
        if (r.spelling == "je") r.cond = "definitely-not-a-cond";
    }
    setInstructions(doc, rows);
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_EQ(countDiagnostics(run->reporter, kAsmCode), 1u);
}

TEST(AsmTextToLir, ConditionTheTargetCannotEncodeIsRefusedAtDialectResolution) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    for (auto& r : rows) {
        // `foeq` is DELIBERATELY absent from x86_64's condCodeEncoding (no
        // single x86 cc realizes ordered-equal), so this is a real
        // dialect/target disagreement rather than a synthetic one.
        if (r.spelling == "je") r.cond = "foeq";
    }
    setInstructions(doc, rows);
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("no encoding for"), std::string::npos)
        << messages(*run);
}

// ── opcode election ───────────────────────────────────────────────────────

// ★★★ ONE SPELLING, THREE TARGET OPCODES, ELECTED BY OPERAND SHAPE. The
// dialect says `movq` MAY be mov / load / store; only the target's own
// `encoding.variants[].guard` says which.
TEST(AsmTextToLir, OneSpellingElectsMovLoadAndStoreByOperandShape) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n"
        ".func main\n"
        "main:\n"
        "  movq %rax, %rcx\n"
        "  movq 8(%rdi), %rax\n"
        "  movq %rax, 16(%rdi)\n"
        "  movq (%rdi,%rsi,4), %rdx\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const& lir = run->module->lir;
    std::vector<std::uint16_t> opcodes;
    auto const fn = lir.funcAt(0);
    auto const blk = lir.funcBlockAt(fn, 0);
    for (std::uint32_t ii = 0; ii < lir.blockInstCount(blk); ++ii) {
        opcodes.push_back(lir.instOpcode(lir.blockInstAt(blk, ii)));
    }
    ASSERT_EQ(opcodes.size(), 5u);
    EXPECT_EQ(opcodes[0], op(*run->target, "mov"));
    EXPECT_EQ(opcodes[1], op(*run->target, "load"));
    EXPECT_EQ(opcodes[2], op(*run->target, "store"));
    EXPECT_EQ(opcodes[3], op(*run->target, "load")) << "base+index+scale arm";
    EXPECT_EQ(opcodes[4], op(*run->target, "ret"));

    // The store's operand list is the post-callconv shape the target declares:
    // [value, base, MemBase(scale), MemOffset(disp)], and it carries NO result
    // register — the "destination must be a register" refusal must not fire.
    auto const storeOps = lir.instOperands(lir.blockInstAt(blk, 2));
    ASSERT_EQ(storeOps.size(), 4u);
    EXPECT_EQ(storeOps[0].kind, LirOperandKind::Reg);
    EXPECT_EQ(storeOps[1].kind, LirOperandKind::Reg);
    EXPECT_EQ(storeOps[2].kind, LirOperandKind::MemBase);
    EXPECT_EQ(storeOps[3].kind, LirOperandKind::MemOffset);
    EXPECT_EQ(storeOps[3].offset, 16);
    EXPECT_EQ(lir.instResult(lir.blockInstAt(blk, 2)).id, InvalidLirReg.id);

    // The indexed load is the 4-operand arm: base, index, scale, disp.
    auto const idxOps = lir.instOperands(lir.blockInstAt(blk, 3));
    ASSERT_EQ(idxOps.size(), 4u);
    EXPECT_EQ(idxOps[2].kind, LirOperandKind::MemBase);
    EXPECT_EQ(idxOps[2].scale, 4u);
}

// ★★ THE PAIR THAT PRESENTS AN IDENTICAL OPERAND-KIND VECTOR. ✔MEASURED:
// `movq (%rsp,%r8,8),%rcx` and `movq %rdx,(%rsp,%r8,8)` — one load, one store —
// both reduce to `[reg, reg, membase, memoffset]`-shaped lists that the
// target's guards cannot tell apart. The discriminator is WHICH SIDE the memory
// operand was written on, and the target's own `result` rule is its
// consequence: an instruction writing to memory produces no value.
TEST(AsmTextToLir, LoadAndStoreWithIdenticalKindVectorsAreStillSeparated) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n"
        ".func main\n"
        "main:\n"
        "  movq (%rsp,%r8,8), %rcx\n"
        "  movq %rdx, (%rsp,%r8,8)\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const& lir = run->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(blk, 0)), op(*run->target, "load"))
        << "memory on the SOURCE side";
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(blk, 1)), op(*run->target, "store"))
        << "memory on the DESTINATION side";
    // The store's indexed arm carries the value first, then the full address.
    auto const st = lir.instOperands(lir.blockInstAt(blk, 1));
    ASSERT_EQ(st.size(), 5u);
    EXPECT_EQ(st[0].reg.id, *run->target->registerByName("rdx"));
    EXPECT_EQ(st[1].reg.id, *run->target->registerByName("rsp"));
    EXPECT_EQ(st[2].reg.id, *run->target->registerByName("r8"));
    EXPECT_EQ(st[3].scale, 8u);
}

// ★ THE OPERAND-ORDER RULE NEEDED NO PER-INSTRUCTION EXCEPTION. AT&T is
// destination-LAST uniformly INCLUDING stores; the store above and the
// two-address `addq` here both fall out of the one dialect-wide fact.
TEST(AsmTextToLir, TwoAddressFormFallsOutOfTheOneOperandOrderFact) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n"
        ".func main\n"
        "main:\n"
        "  addq %rcx, %rax\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);
    auto const& lir = run->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    auto const  add = lir.blockInstAt(blk, 0);
    EXPECT_EQ(lir.instOpcode(add), op(*run->target, "add"));
    auto const ops = lir.instOperands(add);
    // `addq %rcx, %rax` IS `rax = rax + rcx`: the destination is re-read as
    // operand 0 because the target declares `requires2Address`.
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0].kind, LirOperandKind::Reg);
    EXPECT_EQ(ops[0].reg.id, lir.instResult(add).id);
}

// An operand shape no candidate encodes is refused NAMING every candidate and
// why it lost — not "no match".
TEST(AsmTextToLir, UnencodableShapeNamesEveryRejectedCandidate) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    rows.push_back({"movq_bad", {"mov", "load"}, 64, ""});
    setInstructions(doc, rows);
    auto const run = lowerAsmText(doc, src(
        ".globl main\n"
        ".func main\n"
        "main:\n"
        "  movq_bad %rax, %rcx, %rdx\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    auto const text = messages(*run);
    EXPECT_NE(text.find("'mov'"),  std::string::npos) << text;
    EXPECT_NE(text.find("'load'"), std::string::npos) << text;
}

// A row that mixes a terminator with a non-terminator cannot elect at all: the
// two do not take the same KIND of operand list.
TEST(AsmTextToLir, RowMixingControlFlowClassesIsRefused) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    rows.push_back({"mixed", {"mov", "ret"}, 64, ""});
    setInstructions(doc, rows);
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("one spelling cannot denote both"),
              std::string::npos)
        << messages(*run);
}

// ── register width agreement (the `subOf` consumer) ───────────────────────

// ★★ A NARROW SPELLING RESOLVES TO ITS PARENT'S ORDINAL. `%eax` and `%rax` are
// ONE machine register; LIR names it once and carries the width on the
// instruction. Red the moment the `subOf` walk is removed.
TEST(AsmTextToLir, NarrowRegisterResolvesToParentOrdinal) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n"
        ".func main\n"
        "main:\n"
        "  movl %eax, %ecx\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const& lir = run->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    auto const  mov = lir.blockInstAt(blk, 0);
    auto const  rax = *run->target->registerByName("rax");
    auto const  rcx = *run->target->registerByName("rcx");
    auto const  eax = *run->target->registerByName("eax");
    ASSERT_NE(rax, eax) << "the sub-register must have its own ordinal";
    EXPECT_EQ(lir.instResult(mov).id, rcx);
    EXPECT_EQ(lir.instOperands(mov)[0].reg.id, rax);
    // ...and the WIDTH rode the instruction, not the register.
    EXPECT_EQ(lirInstWidthBits(lir.instFlags(mov)), 32);
}

// ★★ WIDTH AGREEMENT IS CHECKED ON **EVERY** DATA REGISTER, NOT ONLY THE
// DESTINATION. gas rejects both `movl %rax,%ecx` and `movl %eax,%rcx`; a check
// on the destination alone accepts the first and silently encodes a 32-bit move
// of a register the programmer named at 64 bits.
TEST(AsmTextToLir, WidthMismatchRefusedOnSourceOperandToo) {
    auto const badSource = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  movl %rax, %ecx\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*badSource)) << parseMessages(*badSource);
    EXPECT_FALSE(badSource->module.has_value())
        << "a 64-bit SOURCE under a 32-bit mnemonic must be refused";
    EXPECT_EQ(countDiagnostics(badSource->reporter, kAsmCode), 1u);
    EXPECT_NE(messages(*badSource).find("register 'rax' is 64 bits"),
              std::string::npos)
        << messages(*badSource);

    auto const badDest = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  movl %eax, %rcx\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*badDest)) << parseMessages(*badDest);
    EXPECT_FALSE(badDest->module.has_value());
    EXPECT_NE(messages(*badDest).find("'rcx' is 64 bits"), std::string::npos)
        << messages(*badDest);

    // And the case where the two registers AGREE with each other but not with
    // the suffix: `movl %rax,%rcx` is uniformly 64-bit under a 32-bit spelling,
    // so the DECLARED width is what disagrees.
    auto const bothWide = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  movl %rax, %rcx\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*bothWide)) << parseMessages(*bothWide);
    EXPECT_FALSE(bothWide->module.has_value());
    EXPECT_NE(messages(*bothWide).find("register operands are 64 bits"),
              std::string::npos)
        << messages(*bothWide);
}

// ⚠ AN ADDRESS REGISTER IS NOT A DATA REGISTER. `movl (%rdi), %eax` is legal
// gas: the base register's width is the ADDRESS width, not the operation width.
// A width rule applied to every register in sight would reject it.
TEST(AsmTextToLir, MemoryBaseRegisterIsExemptFromTheWidthRule) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  movl 8(%rdi), %eax\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);
}

// ── the width-honesty gate ────────────────────────────────────────────────

// ★★★ THE MEASURED SILENT MISCOMPILE. A width-ABSENT encoding variant matches
// ANY width, so a dialect row declaring 32 elects the target's natural-width
// form and the declared width is dropped on the floor between the dialect
// document and the emitted bytes. The gate refuses the pairing instead.
//
// ⚠ THE VEHICLE CHANGED ON 2026-08-13 AND THE SUBJECT DID NOT. This test used
// to drive the gate with `leal`, because x86's `lea` shipped FIVE variants and
// none carried `guard.width`. Those variants are now width-keyed and two
// 32-bit twins were added (D-ASM-ATT-LEAL-UNREACHABLE-NO-WIDTH-KEYED-LEA), so
// `leal` is REACHABLE and can no longer exercise the gate — the second half of
// this test now pins that reachability instead. The gate itself is driven by
// `jmp`, whose single variant is still width-absent; a target that later keys
// it lights this up as a stale vehicle rather than a silent pass, because the
// EXPECT_FALSE would flip.
TEST(AsmTextToLir, DeclaredWidthWithNoWidthKeyedVariantIsRefused) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    for (auto& r : rows) {
        if (r.spelling == "jmp") r.width = 32;   // `jmp`'s only variant is width-absent
    }
    setInstructions(doc, rows);
    auto const narrow = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  jmp Lend\nLend:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*narrow)) << parseMessages(*narrow);
    EXPECT_FALSE(narrow->module.has_value())
        << "a width the elected variant cannot express must not be dropped";
    EXPECT_NE(messages(*narrow).find("no width-keyed encoding variant"),
              std::string::npos)
        << messages(*narrow);

    // The 64-bit sibling IS honest: an elected width-keyed variant satisfies
    // the gate by construction, and a width-absent one still satisfies it at
    // the width a flags-less LIR instruction already means.
    auto const wide = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  leaq 8(%rdi), %rax\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*wide)) << parseMessages(*wide);
    EXPECT_TRUE(wide->module.has_value()) << messages(*wide);
}

// ── calls ─────────────────────────────────────────────────────────────────

TEST(AsmTextToLir, CallToALocalFunctionEntryIsASymbolRefAndNotATerminator) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".func helper\n"
        ".globl main\n"
        ".func main\n"
        "helper:\n"
        "  ret\n"
        "main:\n"
        "  call helper\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const& lir = run->module->lir;
    auto const  fn  = lir.funcAt(1);
    auto const  blk = lir.funcBlockAt(fn, 0);
    ASSERT_EQ(lir.blockInstCount(blk), 2u) << "a call is NOT a terminator";
    auto const call = lir.blockInstAt(blk, 0);
    EXPECT_EQ(lir.instOpcode(call), op(*run->target, "call"));
    auto const ops = lir.instOperands(call);
    ASSERT_EQ(ops.size(), 1u) << "post-callconv shape: the callee, nothing else";
    EXPECT_EQ(ops[0].kind, LirOperandKind::SymbolRef);
    EXPECT_EQ(ops[0].symbolV, run->module->symbols[0].symbol.v);
}

// ★★★ D-ASM-EXTERNAL-CALL-UNREPRESENTABLE. A call to a name this file does not
// define is an EXTERN, minted with no directive — gas has none (`.extern` is
// documented as accepted and ignored), so requiring one would refuse assembly
// every reference assembler takes.
//
// ★★ THE ROW SHAPE IS THE ASSERTION, not merely "one row exists". `libraryPath`
// EMPTY is the load-bearing field: a `.s` states no owning image, and a
// non-empty one here would be a second owner of the fact the descriptor corpus
// owns — the exact defect UCRT-P4 removed from the C path. Unbound is what a C
// bare prototype produces, and the linker's reference gate already judges it.
TEST(AsmTextToLir, CallToAnUndefinedNameBecomesAnUnboundExternImport) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  call puts\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    ASSERT_EQ(run->module->externImports.size(), 1u);
    auto const& ext = run->module->externImports[0];
    EXPECT_EQ(ext.mangledName, "puts");
    EXPECT_TRUE(ext.libraryPath.empty())
        << "a `.s` names no owning image; inventing one would be a second "
           "owner of what the descriptor corpus owns";
    EXPECT_TRUE(ext.version.empty());
    EXPECT_FALSE(ext.isData) << "the row exists because a CALL named it";
    EXPECT_FALSE(ext.isEagerImport)
        << "nothing shipped this row, so the linker's reference gate must be "
           "free to drop it when unreferenced";

    // The call's operand names exactly that extern — not a dangling SymbolRef.
    auto const& lir = run->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    auto const  ops = lir.instOperands(lir.blockInstAt(blk, 0));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].kind, LirOperandKind::SymbolRef);
    EXPECT_EQ(ops[0].symbolV, ext.symbol.v);

    // ⚠ AND IT MUST NOT COLLIDE WITH A DEFINED SYMBOL. The linker's per-CU
    // `declare()` rejects one SymbolId claimed twice, so an extern reusing a
    // function's id is a link error rather than a lowering one — which is why
    // this is asserted here, where the ids are minted.
    for (auto const& sym : run->module->symbols) {
        EXPECT_NE(sym.symbol.v, ext.symbol.v) << "extern id collides with '"
                                              << sym.name << "'";
    }
}

// Two references to one undefined name are two relocations against ONE dynamic
// symbol. Minting twice would hand the linker a second row (and a second
// `declare()`d SymbolId) for a name the import key would collapse anyway.
TEST(AsmTextToLir, RepeatedCallsToOneUndefinedNameMintOneImport) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n"
        ".func main\n"
        "main:\n"
        "  call puts\n"
        "  call write\n"
        "  call puts\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    ASSERT_EQ(run->module->externImports.size(), 2u);
    // FIRST-REFERENCE order, which is source order — so two runs over one file
    // mint identical ids and emit identical bytes.
    EXPECT_EQ(run->module->externImports[0].mangledName, "puts");
    EXPECT_EQ(run->module->externImports[1].mangledName, "write");
    EXPECT_NE(run->module->externImports[0].symbol.v,
              run->module->externImports[1].symbol.v);

    auto const& lir = run->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(blk), 4u);
    auto const first = lir.instOperands(lir.blockInstAt(blk, 0));
    auto const third = lir.instOperands(lir.blockInstAt(blk, 2));
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(third.size(), 1u);
    EXPECT_EQ(first[0].symbolV, third[0].symbolV)
        << "both `call puts` sites must reference ONE import row";
    EXPECT_EQ(first[0].symbolV, run->module->externImports[0].symbol.v);
}

// ★ THE FAIL-LOUD THAT REMAINS, AND IT IS THE ONE WITH NO HONEST ANSWER. A
// label this file DEFINES but did not mark as a function entry is a BLOCK: it
// has no module symbol, so a call's SymbolRef has nothing to name. Treating it
// as an extern would import a name this very file defines; treating it as a
// function would call into a frame whose prologue never ran.
TEST(AsmTextToLir, CallToAnInteriorBlockLabelIsRefusedByName) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n"
        ".func main\n"
        "main:\n"
        "  jmp Lbody\n"
        "Lbody:\n"
        "  call Lbody\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("'Lbody'"), std::string::npos)
        << messages(*run);
    EXPECT_NE(messages(*run).find("BLOCK inside another function"),
              std::string::npos)
        << messages(*run);
    // ⚠ NOT quietly turned into an import of a name the file defines.
    EXPECT_EQ(countDiagnostics(run->reporter, kAsmCode), 1u);
}

// ── the `lirFatal` guards, each raised to a diagnostic ────────────────────

// A branch into ANOTHER function's block cannot be expressed: LIR block
// references are function-local.
TEST(AsmTextToLir, CrossFunctionBranchIsRefusedNotAborted) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".func a\n"
        ".func b\n"
        "a:\n"
        "  ret\n"
        "b:\n"
        "  jmp Lx\n"
        "Lx:\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const bad = lowerAsmText(baseDialectDoc(), src(
        ".func a\n"
        ".func b\n"
        "a:\n"
        "  jmp Lx\n"
        "b:\n"
        "  jmp Lx\n"
        "Lx:\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*bad)) << parseMessages(*bad);
    EXPECT_FALSE(bad->module.has_value());
    EXPECT_NE(messages(*bad).find("different function"), std::string::npos)
        << messages(*bad);
}

// Falling out of a function's LAST block is refused, because there is no next
// block to branch to and both ways to fill it (`ret` / `unreachable`) are a
// claim about intent.
TEST(AsmTextToLir, FunctionWithNoTerminatorIsRefusedNotAborted) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  movq %rax, %rcx\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_EQ(countDiagnostics(run->reporter, kAsmCode), 1u);
    EXPECT_NE(messages(*run).find("no terminating instruction"),
              std::string::npos)
        << messages(*run);
}

// ★ FALLING INTO A LABEL IS DEFINED, so it is REALIZED as an explicit branch
// rather than refused — unlike falling off the end, it has exactly one meaning
// in every assembler. Adjacent labels are the degenerate case and produce a
// block containing only that branch, which is what two labels at one address
// mean.
TEST(AsmTextToLir, FallthroughIntoALabelBecomesAnExplicitBranch) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n"
        ".func main\n"
        "main:\n"
        "  movq $1, %rax\n"
        "L1:\n"
        "L2:\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const& lir = run->module->lir;
    auto const  fn  = lir.funcAt(0);
    ASSERT_EQ(lir.funcBlockCount(fn), 3u);
    auto const jmp = op(*run->target, "jmp");
    for (std::uint32_t bi = 0; bi + 1 < lir.funcBlockCount(fn); ++bi) {
        auto const blk  = lir.funcBlockAt(fn, bi);
        auto const last = lir.blockInstAt(blk, lir.blockInstCount(blk) - 1);
        EXPECT_EQ(lir.instOpcode(last), jmp)
            << "block " << bi << " must end in the synthesized fallthrough";
        ASSERT_EQ(lir.blockSuccessors(blk).size(), 1u);
    }
    // The degenerate block holds exactly the synthesized branch.
    EXPECT_EQ(lir.blockInstCount(lir.funcBlockAt(fn, 1)), 1u);
}

// An instruction after a terminator with no intervening label has no block.
TEST(AsmTextToLir, InstructionAfterTerminatorIsRefusedNotAborted) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  ret\n  movq %rax, %rcx\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("unreachable"), std::string::npos)
        << messages(*run);
}

// ── the program entry ─────────────────────────────────────────────────────

// ★ THE HEADER SAID `.globl`-EXPORTED AND THE CODE ELECTED ON NAME ALONE.
// Reconciled toward the header: an entry-named label that is not exported is a
// diagnostic, because leaving `userEntrySymbol` unset instead would send the
// trampoline to functions[0].
TEST(AsmTextToLir, EntryNamedLabelMustBeExported) {
    auto const exported = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*exported)) << parseMessages(*exported);
    ASSERT_TRUE(exported->module.has_value()) << messages(*exported);
    EXPECT_TRUE(exported->module->userEntrySymbol.has_value());

    // The same file WITHOUT the export. Deliberately no `.globl`.
    auto const local = lowerAsmText(baseDialectDoc(), src(
        ".func main\nmain:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*local)) << parseMessages(*local);
    EXPECT_FALSE(local->module.has_value());
    EXPECT_NE(messages(*local).find("not exported"), std::string::npos)
        << messages(*local);
}

// ── the loader ────────────────────────────────────────────────────────────

// ★★★ TWO ROLES ON ONE RULE IS LEGAL WHEN ONE OF THEM IS `register`, AND IT IS
// WHAT A SIGIL-LESS ASSEMBLY SYNTAX REQUIRES. ✔MEASURED against a real arm64
// dialect: aarch64 gas writes `mov x0, x1`, `bl helper` and `b Lend` — `x0`,
// `helper` and `Lend` are the SAME TOKEN and no grammar can split them. The
// dialect declares the shape ambiguous by binding both roles to one rule, and
// the LOOKUP decides per operand, exactly as gas does.
TEST(AsmTextToLir, RegisterAndSymbolMayShareOneRuleAndTheLookupDecides) {
    auto doc = baseDialectDoc();
    doc["assembly"]["operandForms"]["scalar"] =
        doc["assembly"]["operandForms"]["register"];
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  movq %rax, %rcx\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value())
        << "a spelling the TARGET declares as a register must decode as one "
        << "even when the rule also carries a symbol role: " << messages(*run);
    auto const& lir = run->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(blk, 0)), op(*run->target, "mov"));
}

// ⚠ WHAT IS STILL REFUSED: a pairing NO LOOKUP CAN SETTLE. `register` is the
// only role whose membership the target can answer, so two non-register roles
// on one rule would leave the lowering picking by enumerator order — the silent
// first-match this check replaced.
TEST(AsmTextToLir, TwoNonRegisterRolesOnOneRuleIsALoadError) {
    auto doc = baseDialectDoc();
    doc["assembly"]["operandForms"]["immediate"] =
        doc["assembly"]["operandForms"]["scalar"];
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_FALSE(run->loadErrors.empty())
        << "an undecidable duplicate role binding must be refused at LOAD";
    bool named = false;
    for (auto const& e : run->loadErrors) {
        if (e.find("NEITHER is \'register\'") != std::string::npos) named = true;
    }
    EXPECT_TRUE(named) << parseMessages(*run);
}

// ★ A ROLE THE DIALECT DOES NOT HAVE IS DECLARED ABSENT (null), NOT OMITTED.
// ✔MEASURED: aarch64 gas has no `displaced` form (`disp(base,index,scale)` is
// AT&T syntax) and no `indirect` form (indirectness lives in the mnemonic — `b`
// versus `br`). Forcing it to bind those roles to some unrelated rule would make
// the lowering recognize shapes the dialect cannot write; omitting the key
// silently would be indistinguishable from forgetting it.
TEST(AsmTextToLir, AnAbsentOperandRoleIsDeclaredNullNotOmitted) {
    auto declaredAbsent = baseDialectDoc();
    declaredAbsent["assembly"]["operandForms"]["indirect"] = nullptr;
    auto const ok = lowerAsmText(declaredAbsent, src(
        ".globl main\n.func main\nmain:\n  movq %rax, %rcx\n  ret\n"));
    ASSERT_TRUE(ok->loadErrors.empty())
        << "an explicitly-absent role must load: " << parseMessages(*ok);
    EXPECT_TRUE(ok->module.has_value()) << messages(*ok);

    auto omitted = baseDialectDoc();
    omitted["assembly"]["operandForms"].erase("indirect");
    auto const bad = lowerAsmText(omitted, src(".globl main\n.func main\nmain:\n  ret\n"));
    EXPECT_FALSE(bad->loadErrors.empty())
        << "an OMITTED role must still be a load error";
}

// The legacy `opcode` key names its own replacement rather than reading as a
// typo.
TEST(AsmTextToLir, LegacySingularOpcodeKeyNamesTheRename) {
    auto doc = baseDialectDoc();
    doc["assembly"]["instructions"] = nlohmann::json::array(
        {{{"spelling", "ret"}, {"opcode", "ret"}, {"width", 64}}});
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_FALSE(run->loadErrors.empty());
    bool named = false;
    for (auto const& e : run->loadErrors) {
        if (e.find("was replaced by 'opcodes'") != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named) << parseMessages(*run);
}

// An empty candidate set denotes nothing and is refused at load.
TEST(AsmTextToLir, EmptyOpcodeCandidateSetIsALoadError) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    rows.front().opcodes.clear();
    setInstructions(doc, rows);
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    EXPECT_FALSE(run->loadErrors.empty());
}

// `marker` on a verb that cannot use it would be silently ignored.
TEST(AsmTextToLir, MarkerOnANonFunctionEntryVerbIsALoadError) {
    auto doc = baseDialectDoc();
    setDirectives(doc, {{"text", "sectionText", ""},
                        {"globl", "globalSymbol", "function"},
                        {"func", "functionEntry", ""}});
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_FALSE(run->loadErrors.empty());
    bool named = false;
    for (auto const& e : run->loadErrors) {
        if (e.find("only meaningful on a 'functionEntry'")
            != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named) << parseMessages(*run);
}

// ── width derivation (the suffix-less dialect model) ──────────────────────

// ★★★ A ROW WITH NO `width` READS THE WIDTH OFF ITS REGISTERS. ✔MEASURED on a
// real arm64 dialect: aarch64 encodes the width in the REGISTER, not the
// mnemonic — `add x0,x1,x2` is 64-bit and `add w0,w1,w2` is 32-bit with the
// SAME spelling. A mandatory per-spelling width forces one answer for a
// spelling that has two, and the other one then encodes silently at the wrong
// width.
TEST(AsmTextToLir, AbsentWidthIsDerivedFromTheRegisterOperands) {
    auto const wide = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  movx %rax, %rcx\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*wide)) << parseMessages(*wide);
    ASSERT_TRUE(wide->module.has_value()) << messages(*wide);
    {
        auto const& lir = wide->module->lir;
        auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
        EXPECT_EQ(lirInstWidthBits(lir.instFlags(lir.blockInstAt(blk, 0))), 64);
    }

    auto const narrow = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  movx %eax, %ecx\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*narrow)) << parseMessages(*narrow);
    ASSERT_TRUE(narrow->module.has_value()) << messages(*narrow);
    {
        auto const& lir = narrow->module->lir;
        auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
        // SAME spelling, DIFFERENT width — read off the registers, and it
        // reached the encoder guard as 32 (electing the no-REX.W variant)
        // rather than being dropped.
        EXPECT_EQ(lirInstWidthBits(lir.instFlags(lir.blockInstAt(blk, 0))), 32);
        EXPECT_EQ(lir.instOpcode(lir.blockInstAt(blk, 0)),
                  op(*narrow->target, "mov"));
    }

    // Disagreeing registers under a width-less spelling are still refused —
    // there is no suffix to fall back on, so the operands must agree.
    auto const mixed = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  movx %rax, %ecx\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*mixed)) << parseMessages(*mixed);
    EXPECT_FALSE(mixed->module.has_value());
    EXPECT_NE(messages(*mixed).find("one instruction cannot operate on both"),
              std::string::npos)
        << messages(*mixed);
}

// ★★ A NON-PRODUCING OPCODE KEEPS ITS "DESTINATION" AS AN INPUT. ✔MEASURED:
// without this arm `cmpq %rbx,%rax` produced ONE LIR operand for an opcode that
// takes two, because the destination-position operand was excluded
// unconditionally. And it cannot simply stay in source order either — AT&T
// `cmpq %rbx,%rax` IS Intel `cmp rax,rbx`.
TEST(AsmTextToLir, NonProducingOpcodeKeepsItsDestinationOperandAndPutsItFirst) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  cmpq %rbx, %rax\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);
    auto const& lir = run->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    auto const  cmp = lir.blockInstAt(blk, 0);
    EXPECT_EQ(lir.instOpcode(cmp), op(*run->target, "cmp"));
    auto const ops = lir.instOperands(cmp);
    ASSERT_EQ(ops.size(), 2u) << "a compare reads BOTH operands";
    EXPECT_EQ(ops[0].reg.id, *run->target->registerByName("rax"))
        << "the AT&T destination is the compare\'s LEFT-hand side";
    EXPECT_EQ(ops[1].reg.id, *run->target->registerByName("rbx"));
    EXPECT_EQ(lir.instResult(cmp).id, InvalidLirReg.id)
        << "a compare writes no register";
}

// ── the `subOf` rows themselves ───────────────────────────────────────────

// ★★ A `subOf` ROW CAN NEVER ENTER AN ALLOCATION POOL, AND THIS TEST IS NOW A
// POSITIVE CONTROL RATHER THAN THE ONLY GUARD — the rationale below was rewritten
// on 2026-08-13 when the mechanism underneath it changed.
// It used to say: two independent reasons exist (a `subOf` skip in
// `buildFreeLists`/`pickScratchRegs`, plus the cc-list filter), and this test
// asserts the config half "where it is observable". BOTH skips have since been
// DELETED as unreachable defence — `allocatable` is built EXCLUSIVELY from the
// six calling-convention lists — and a cc list naming a `subOf` register is now
// REFUSED AT CONFIG LOAD (`TargetSchemaData::validate()`'s `checkRefs`).
// ⚠ SO THE FAILURE THIS FILE WOULD SEE HAS MOVED, AND THAT IS WHY THE TEST STAYS:
// a violating shipped config now fails `loadShipped` at the `ASSERT_TRUE` below,
// BEFORE any `EXPECT_NE` runs. The `EXPECT_NE` is therefore no longer the
// enforcement point — it is the control that proves the shipped configs are on
// the legal side of the new rule, and the `subOfRows > 0` check at the end is
// what keeps the aliasing mechanism from quietly losing its consumers again.
TEST(TargetSubRegisters, NoSubRegisterAppearsInAnyCallingConventionList) {
    for (auto const* name : {"x86_64", "arm64"}) {
        auto const schemaR = TargetSchema::loadShipped(name);
        ASSERT_TRUE(schemaR.has_value()) << name;
        auto const& schema = **schemaR;

        std::size_t subOfRows = 0;
        for (auto const& r : schema.registers()) {
            if (r.subOf.empty()) continue;
            ++subOfRows;
            // Resolves, and to a STRICTLY WIDER register of the SAME class —
            // a typo'd parent would otherwise load fine and alias the wrong
            // machine register.
            auto const parent = schema.registerByName(r.subOf);
            ASSERT_TRUE(parent.has_value()) << r.name << " -> " << r.subOf;
            auto const* pinfo = schema.registerInfo(*parent);
            ASSERT_NE(pinfo, nullptr);
            EXPECT_EQ(pinfo->regClass, r.regClass) << r.name;
            EXPECT_GT(pinfo->widthBytes, r.widthBytes) << r.name;
            EXPECT_EQ(pinfo->hwEncoding, r.hwEncoding)
                << r.name << ": a sub-register shares its parent's encoding";

            for (auto const& cc : schema.callingConventions()) {
                for (auto const* list : {&cc.argGprs, &cc.argFprs,
                                         &cc.returnGprs, &cc.returnFprs,
                                         &cc.callerSaved, &cc.calleeSaved}) {
                    for (auto const& n : *list) {
                        EXPECT_NE(n, r.name)
                            << "sub-register '" << r.name << "' is listed in "
                            << "calling convention '" << cc.name << "' — it "
                            << "would become allocatable the moment the "
                            << "subOf skip in lir_regalloc/lir_rewrite was "
                            << "relaxed";
                    }
                }
            }
        }
        EXPECT_GT(subOfRows, 0u)
            << name << " declares no subOf rows — the aliasing mechanism has "
                       "no consumer again";
    }
}

// ★★ arm64's `mov` SHIPPED TWO VARIANTS, NEITHER WIDTH-KEYED, so a width-32
// register move silently encoded the 64-bit `ORR Xd, XZR, Xm`. Both widths are
// now keyed; this pins that the W-form exists and that the X-form did not move.
TEST(TargetSubRegisters, Arm64MovDeclaresBothWidthKeyedRegisterForms) {
    auto const schemaR = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schemaR.has_value());
    auto const& schema = **schemaR;
    auto const  mov    = schema.opcodeByMnemonic("mov");
    ASSERT_TRUE(mov.has_value());
    auto const* info = schema.opcodeInfo(*mov);
    ASSERT_NE(info, nullptr);

    bool sawX = false;
    bool sawW = false;
    for (auto const& v : info->encoding.variants) {
        if (v.operandKinds.size() != 1
            || v.operandKinds[0] != OperandKindFilter::Reg) {
            continue;
        }
        EXPECT_NE(v.guardWidthBits, 0)
            << "a width-absent register-move variant matches ANY width and "
               "would encode 64 bits for a 32-bit move";
        if (v.guardWidthBits == 64) {
            sawX = true;
            EXPECT_EQ(v.tmpl.wordAt(0), 0xAA0003E0u) << "ORR Xd, XZR, Xm";
        }
        if (v.guardWidthBits == 32) {
            sawW = true;
            EXPECT_EQ(v.tmpl.wordAt(0), 0x2A0003E0u) << "ORR Wd, WZR, Wm";
        }
    }
    EXPECT_TRUE(sawX);
    EXPECT_TRUE(sawW);
}

// ── D-ASM-COND-ALLOWED-ONLY-ON-JCC ────────────────────────────────────────
//
// ★★★ `cond` IS KEYED ON `condCodeFromPayload`, NOT ON `terminatorKind`. The
// two are independent facts about an opcode and only one of them is about the
// condition. Both shipped targets declare exactly two cond-consuming opcodes —
// `jcc` (a cond-br) and `setcc` (`terminatorKind: None`, `result: value`) — so
// the old key coincided with the right answer on one of them and was wrong on
// the other, which is precisely the shape that survives review.
//
// ★★ THE ASSERTION GOES ALL THE WAY TO THE ENCODED BYTE, because "the payload
// is set" and "the encoder read it" are different claims, and only the second
// one is what a `.s` writer gets. x86 realizes the condition as the LOW NIBBLE
// of the second opcode byte (`0F 90+cc`), so the byte IS the condition.
//
// ⚠ AND `setne` IS THE DECIDING HALF, DELIBERATELY. `TargetCondCode::Eq` is 0,
// so a lowering that dropped the payload entirely would still encode `sete`
// correctly by accident — the `sete` window alone cannot tell a threaded
// condition from an unthreaded one. Only the second, non-zero condition can.
TEST(AsmTextToLir, SetccCarriesItsConditionThroughToTheEncodedByte) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    rows.push_back({"sete",  {"setcc"}, 64, "eq"});
    rows.push_back({"setne", {"setcc"}, 64, "ne"});
    setInstructions(doc, rows);

    auto const run = lowerAsmText(doc, src(
        ".globl main\n"
        ".func main\n"
        "main:\n"
        "  cmpq $1, %rax\n"
        "  sete %rcx\n"
        "  setne %rdx\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const& lir = run->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(blk), 4u);

    // TIER 1 — the condition reached the LIR payload, per instruction.
    auto const setE  = lir.blockInstAt(blk, 1);
    auto const setNE = lir.blockInstAt(blk, 2);
    EXPECT_EQ(lir.instOpcode(setE),  op(*run->target, "setcc"));
    EXPECT_EQ(lir.instOpcode(setNE), op(*run->target, "setcc"));
    EXPECT_EQ(lir.instPayload(setE),
              static_cast<std::uint32_t>(TargetCondCode::Eq));
    EXPECT_EQ(lir.instPayload(setNE),
              static_cast<std::uint32_t>(TargetCondCode::Ne));
    // A non-terminator that PRODUCES a value: the destination is the result,
    // and `setcc` takes no value operands (it reads FLAGS).
    EXPECT_TRUE(lir.instOperands(setE).empty());
    EXPECT_NE(lir.instResult(setE), InvalidLirReg);

    // TIER 2 — the ENCODER read it. `0F 90+cc`: eq is nibble 4, ne is nibble 5
    // (x86_64's own `condCodeEncoding`), and ModRM `C0|rm` names the byte
    // register — rcx ⇒ 0xC1, rdx ⇒ 0xC2.
    DiagnosticReporter     rep;
    std::vector<MirInstId> lirToMir(lir.instCount(), InvalidMirInst);
    auto const             asmMod = assemble(lir, *run->target, lirToMir, rep);
    ASSERT_TRUE(asmMod.ok()) << "assembling the lowered module failed";
    ASSERT_EQ(asmMod.functions.size(), 1u);
    auto const& bytes = asmMod.functions[0].bytes;

    EXPECT_EQ(countWindow(bytes, {0x0F, 0x94, 0xC1}), 1u)
        << "expected `sete %cl` — " << hex(bytes);
    EXPECT_EQ(countWindow(bytes, {0x0F, 0x95, 0xC2}), 1u)
        << "expected `setne %dl` — " << hex(bytes);
    // ⚠ THE RED ARM OF THE SAME MEASUREMENT. A dropped payload encodes cond 0
    // for BOTH, i.e. two `0F 94` windows and no `0F 95`; a swapped table
    // produces the two windows against the wrong registers.
    EXPECT_EQ(countWindow(bytes, {0x0F, 0x94}), 1u)
        << "a second `0F 94` means the second condition was defaulted to the "
           "target's zero code — " << hex(bytes);
    EXPECT_EQ(countWindow(bytes, {0x0F, 0x95}), 1u) << hex(bytes);
    EXPECT_EQ(countWindow(bytes, {0x0F, 0x94, 0xC2}), 0u) << hex(bytes);
    EXPECT_EQ(countWindow(bytes, {0x0F, 0x95, 0xC1}), 0u) << hex(bytes);
}

// The REQUIRED half, on a row that is NOT a terminator — the case the old
// `terminatorKind` key could not express at all. A `setcc` row with no `cond`
// would encode the target's zero condition (`sete`) whatever the writer meant.
//
// ★★ THE CHECK MOVED FROM LOAD TIME TO EMIT TIME ON 2026-08-13, AND THE MOVE
// WAS FORCED BY A REAL DIALECT RATHER THAN CHOSEN
// (D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED). aarch64 writes the condition as
// an OPERAND (`cset x0, eq`) where AT&T fuses it into the mnemonic (`sete`), so
// a row with no `cond` is either an operand-carrying spelling or a mistake —
// and only the INSTRUCTION can tell which. Refusing at load made `cset`
// inexpressible. What must NOT weaken is the guarantee, and it does not: the
// condition must arrive from the row OR from an operand, and neither-source is
// still a refusal that names both. So this test now WRITES the instruction.
TEST(AsmTextToLir, CondIsRequiredOnANonTerminatorThatReadsItFromThePayload) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    rows.push_back({"sete", {"setcc"}, 64, ""});   // no `cond`
    setInstructions(doc, rows);
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  sete %rcx\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("neither this dialect's row (no 'cond' key) "
                                  "nor this instruction's operands names one"),
              std::string::npos)
        << messages(*run);
    EXPECT_NE(messages(*run).find("reads a condition code from the "
                                  "instruction payload"),
              std::string::npos)
        << messages(*run);
}

// The REJECTED half now names the encoder fact too — a row whose opcode has no
// condition slot cannot carry one, whatever its terminator shape.
TEST(AsmTextToLir, CondOnARowWhoseOpcodeReadsNoPayloadConditionIsRefused) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    for (auto& r : rows) {
        if (r.spelling == "movq") r.cond = "eq";
    }
    setInstructions(doc, rows);
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("no encoding variant that reads a condition "
                                  "code"),
              std::string::npos)
        << messages(*run);
}

// ★ A ROW WHOSE CANDIDATES DISAGREE ABOUT CONSUMING A CONDITION IS REFUSED, for
// the same reason a row mixing control-flow classes is: whichever way `cond`
// was declared, one of the two elections would be silently wrong.
TEST(AsmTextToLir, RowMixingCondConsumingAndPlainOpcodesIsRefused) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    rows.push_back({"setish", {"setcc", "mov"}, 64, "eq"});
    setInstructions(doc, rows);
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("only one of which reads a condition code"),
              std::string::npos)
        << messages(*run);
}

// ══ D-ASM-NEGATIVE-SCALAR-LOSES-ITS-SIGN ═══════════════════════════════════
//
// ★★★ A LIVE SILENT MISCOMPILE THAT SHIPPED, and the reason it survived a whole
// cycle of otherwise-thorough unit testing is worth stating: not one of the 43
// tests above this line writes a negative number. `decodeOperand` resolves the
// SCALAR role and hands `decodeScalar` the alt WRAPPER (`attScalar`), whose own
// rule is never `negNumber` — so the old node-identity test was false for every
// negative, the "deepest token" probe returned the `IntLiteral` from under
// `attNegNumber`, and the `MinusSign` was dropped with no diagnostic anywhere.
//
// ★★ THE ASSERTION IS ON THE LIR OPERAND VALUE, which is the tier the defect
// lives at. The runnable half is `examples/asm/
// asm_x86_64_negative_immediate_and_displacement` — ✔MEASURED exit 92 fixed,
// 109 under the restored defect.
TEST(AsmTextToLir, NegativeImmediateKeepsItsSign) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n"
        "  movq $-8, %rcx\n"
        "  movq $8, %rdx\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const& lir = run->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(blk), 3u);
    auto const neg = lir.instOperands(lir.blockInstAt(blk, 0));
    auto const pos = lir.instOperands(lir.blockInstAt(blk, 1));
    ASSERT_EQ(neg.size(), 1u);
    ASSERT_EQ(pos.size(), 1u);
    EXPECT_EQ(neg[0].immInt32, -8)
        << "the MinusSign was dropped: `$-8` lowered as " << neg[0].immInt32;
    // ⚠ THE POSITIVE TWIN IS THE CONTROL. Without it a mutation that negated
    // EVERY scalar would satisfy the assertion above.
    EXPECT_EQ(pos[0].immInt32, 8);
}

// The MEMORY half, which is strictly worse than a wrong constant: a dropped
// sign here is a wrong ADDRESS. AT&T writes the displacement OUTSIDE the parens.
TEST(AsmTextToLir, NegativeMemoryDisplacementKeepsItsSign) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n"
        "  movq -8(%rbp), %rax\n"
        "  movq 8(%rbp), %rcx\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const& lir = run->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(blk), 3u);
    // A load is [base, MemBase, MemOffset]; the displacement is the LAST.
    auto const neg = lir.instOperands(lir.blockInstAt(blk, 0));
    auto const pos = lir.instOperands(lir.blockInstAt(blk, 1));
    ASSERT_EQ(neg.size(), 3u);
    ASSERT_EQ(pos.size(), 3u);
    EXPECT_EQ(neg[2].offset, -8) << "`-8(%rbp)` addressed the wrong slot";
    EXPECT_EQ(pos[2].offset, 8);
}

// ══ the arm64 dialect's own scalar surface ═════════════════════════════════
//
// ★★ THE SIGN FIX IS DIALECT-BLIND AND THIS IS THE SECOND SIGNATORY. arm64
// writes its immediate with `#` and its displacement INSIDE the brackets, so
// both halves take a different route through `decodeOperand` than AT&T's.
TEST(AsmTextToLir, Arm64NegativeImmediateAndMemoryOffsetKeepTheirSign) {
    auto doc = shippedDialectDoc("asm-arm64-gas");
    setInstructions(doc, {
        {"mov",  {"mov"},   64, ""},
        {"ldur", {"load"},  64, ""},   // DSS `load` IS the unscaled LDUR form
        {"ret",  {"ret"},   64, ""},
    });
    setDirectives(doc, {{"text", "sectionText", ""},
                        {"globl", "globalSymbol", ""},
                        {"func", "functionEntry", ""}});
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n"
        "  mov x1, #-8\n"
        "  ldur x2, [x29, #-8]\n"
        "  ldur x3, [x29, #8]\n"
        "  ret\n"), "arm64");
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const& lir = run->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(blk), 4u);
    auto const imm = lir.instOperands(lir.blockInstAt(blk, 0));
    ASSERT_EQ(imm.size(), 1u);
    EXPECT_EQ(imm[0].immInt32, -8) << "arm64 `#-8` lost its sign";

    // ★★★ AND THE OFFSET IS A DISPLACEMENT, NOT A SCALE — the second half of
    // the same defect and the one that could not fail loud. ✔MEASURED before
    // the fix: `[x29, #-8]` produced base=x29, disp=0 and scale=8, silently
    // addressing `x29 * 8`. 1/2/4/8 are exactly the LEGAL scales, so the
    // offsets a programmer is most likely to write are precisely the ones that
    // passed the scale validation without a word; `#-16` would have failed loud.
    auto const negLoad = lir.instOperands(lir.blockInstAt(blk, 1));
    auto const posLoad = lir.instOperands(lir.blockInstAt(blk, 2));
    ASSERT_EQ(negLoad.size(), 3u);
    ASSERT_EQ(posLoad.size(), 3u);
    EXPECT_EQ(negLoad[1].scale, 1u) << "MemBase carries the SCALE, which must stay "
                                    "1 — an offset read as a scale is the "
                                    "silent half of this defect";
    EXPECT_EQ(negLoad[2].offset, -8);
    EXPECT_EQ(posLoad[1].scale, 1u);
    EXPECT_EQ(posLoad[2].offset, 8);
}

// ══ D-ASM-X86-NO-8BIT-REGISTER-FILE ════════════════════════════════════════
//
// ★★★ THROUGH THE ENCODED BYTE, because "the register resolved" and "the right
// instruction was emitted" are different claims. `sete %al` is the ONLY form
// gas accepts; before the byte-view rows it was unspellable and the only form
// DSS could spell (`sete %rax`) is one gas REJECTS — bidirectional
// non-conformance in the sense [[feedback_reference_compilers_are_the_spec]]
// means it.
TEST(AsmTextToLir, EightBitRegisterSpellingReachesTheByteFormOfSetcc) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    rows.push_back({"sete",  {"setcc"}, 8, "eq"});
    rows.push_back({"setne", {"setcc"}, 8, "ne"});
    setInstructions(doc, rows);

    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n"
        "  cmpq $1, %rax\n"
        "  sete %cl\n"
        "  setne %dl\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const& lir = run->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(blk), 4u);
    auto const setE = lir.blockInstAt(blk, 1);
    // ★ THE NARROW SPELLING RESOLVED TO ITS PARENT ORDINAL: `%cl` and `%rcx`
    // are ONE machine register, and LIR names it once.
    EXPECT_TRUE(lir.instResult(setE).isPhysical);
    EXPECT_EQ(lir.instResult(setE).id,
              run->target->registerByName("rcx").value_or(9999));
    EXPECT_EQ(lir.instFlags(setE) & kLirInstFlagWidth8, kLirInstFlagWidth8)
        << "the byte spelling must carry width 8 on the INSTRUCTION";

    DiagnosticReporter     rep;
    std::vector<MirInstId> lirToMir(lir.instCount(), InvalidMirInst);
    auto const             asmMod = assemble(lir, *run->target, lirToMir, rep);
    ASSERT_TRUE(asmMod.ok());
    ASSERT_EQ(asmMod.functions.size(), 1u);
    auto const& bytes = asmMod.functions[0].bytes;
    EXPECT_EQ(countWindow(bytes, {0x0F, 0x94, 0xC1}), 1u)
        << "expected `sete %cl` — " << hex(bytes);
    EXPECT_EQ(countWindow(bytes, {0x0F, 0x95, 0xC2}), 1u)
        << "expected `setne %dl` — " << hex(bytes);
}

// ★ THE WIDTH RULE STILL BITES ACROSS THE NEW FILE: gas rejects `sete %rax`
// (a 64-bit destination for a byte instruction) and so must this.
TEST(AsmTextToLir, SixtyFourBitSpellingOfAByteInstructionIsRefused) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    rows.push_back({"sete", {"setcc"}, 8, "eq"});
    setInstructions(doc, rows);
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  sete %rax\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("declares operand width 8"), std::string::npos)
        << messages(*run);
}

// ══ D-ASM-ATT-INDIRECT-BRANCH-INEXPRESSIBLE ════════════════════════════════
//
// ★★★ ONE SPELLING, TWO TERMINATOR KINDS, DECIDED BY THE OPERAND'S STAR. The
// row `["jmp", "jmp_indirect"]` used to be refused AT LOAD — "one spelling
// cannot denote both" — which made the honest gas row unwritable. Election now
// spans terminator KINDS on the two-sided key: the DIALECT says whether the
// operand carried its indirect marker, the TARGET says which of its opcodes is
// `terminatorKind: indirect-br`.
TEST(AsmTextToLir, OneSpellingDenotesBothDirectAndIndirectBranch) {
    auto const doc = kindSplitJmpDoc();

    // The DIRECT arm still lowers, which is the half a regression would break
    // silently (the row loading is not enough — it has to still branch).
    auto const direct = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  jmp Lend\nLend:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*direct)) << parseMessages(*direct);
    ASSERT_TRUE(direct->module.has_value()) << messages(*direct);
    auto const& lir = direct->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(blk), 1u);
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(blk, 0)),
              op(*direct->target, "jmp"));

    // The INDIRECT arm reaches `jmp_indirect` — proven by the diagnostic naming
    // it. It still refuses, for the reason the next three tests own.
    auto const indirect = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  jmp *%rax\n"));
    ASSERT_TRUE(parsedCleanly(*indirect)) << parseMessages(*indirect);
    EXPECT_FALSE(indirect->module.has_value());
    EXPECT_NE(messages(*indirect).find("jmp_indirect"), std::string::npos)
        << "the star must have elected the INDIRECT opcode: "
        << messages(*indirect);
}

// ══ D-ASM-INDIRECT-BRANCH-SUCCESSOR-SET-UNDERIVABLE ═════════════════════════
//
// ★★★ THE REFUSAL IS KEYED ON DERIVABILITY, NOT ON THE OPCODE, AND THAT IS A
// LIFETIME CLAIM RATHER THAN A WORDING ONE. `buildIndirectBr` asks
// `derivableIndirectSuccessors()` and refuses only when the answer is EMPTY —
// so when interior-label relocation lands, the same lowering emits the branch
// and only the condition stops holding. A refusal keyed on the opcode
// ("indirect branches are unsupported") would have to be DELETED by that change
// and says nothing true in the meantime.
TEST(AsmTextToLir, IndirectBranchRefusalNamesDerivabilityAndTheMissingInput) {
    // TWO blocks, so "every block" is a shape with something to name — a
    // one-block function would make the over-approximation indistinguishable
    // from the empty one and the test would pass for the wrong reason.
    auto const run = lowerAsmText(kindSplitJmpDoc(), src(
        ".globl main\n.func main\nmain:\n  jmp Lmid\nLmid:\n  jmp *%rax\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_EQ(countDiagnostics(run->reporter, kAsmCode), 1u) << messages(*run);

    std::string const msg = messages(*run);
    // It names the INSTRUCTION — the dialect spelling and the opcode it elected.
    EXPECT_NE(msg.find("'jmp'"), std::string::npos) << msg;
    EXPECT_NE(msg.find("jmp_indirect"), std::string::npos) << msg;
    // It names the FUNCTION whose set could not be derived.
    EXPECT_NE(msg.find("'main'"), std::string::npos) << msg;
    // It says the successor set cannot be DERIVED …
    EXPECT_NE(msg.find("SUCCESSOR SET CANNOT BE DERIVED"), std::string::npos)
        << msg;
    // … and names the MISSING INPUT: an interior-label relocation binding.
    EXPECT_NE(msg.find("binds no relocation"), std::string::npos) << msg;
    EXPECT_NE(msg.find("INTERIOR LABELS"), std::string::npos) << msg;
    // … and the two spellings that would supply it.
    EXPECT_NE(msg.find("`lea`/`adr` of a label"), std::string::npos) << msg;
    EXPECT_NE(msg.find(".quad <label>"), std::string::npos) << msg;

    // ★ AND IT IS NOT AN OPCODE-CAPABILITY REFUSAL. The old shape of this
    // message would have called the instruction unsupported / not lowered,
    // which is the claim that goes stale the day the prerequisite lands.
    EXPECT_EQ(msg.find("unsupported"), std::string::npos)
        << "the refusal must be about the missing INPUT, not about the "
           "instruction being unsupported: " << msg;
    EXPECT_EQ(msg.find("not yet lowered"), std::string::npos) << msg;
}

// ★★ THE INSTRUMENT, PROVEN TO FIRE BEFORE IT IS TRUSTED. The next test's
// load-bearing assertion is that `indirectBrShapes` reports NO indirect branch,
// and a probe that silently saw nothing would report exactly that. So it is
// first run against a module that DOES hold one, with a KNOWN successor count,
// built by driving `LirBuilder::addIndirectBr` directly — the very call the
// walker is refusing to make.
TEST(AsmTextToLir, IndirectBrShapesProbeReportsARealIndirectBranch) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& sch = **targetR;
    auto const movOp    = sch.opcodeByMnemonic("mov");
    auto const jmpIndOp = sch.opcodeByMnemonic("jmp_indirect");
    auto const retOp    = sch.opcodeByMnemonic("ret");
    ASSERT_TRUE(movOp.has_value() && jmpIndOp.has_value() && retOp.has_value());

    LirBuilder b{sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry  = b.createBlock();
    LirBlockId const labelA = b.createBlock();
    LirBlockId const labelB = b.createBlock();
    b.beginBlock(entry);
    LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);
    std::array<LirOperand, 1> const mov{LirOperand::makeImmInt32(0)};
    b.addInst(*movOp, rax, mov);
    std::array<LirOperand, 1> const addr{LirOperand::makeReg(rax)};
    std::array<LirBlockId, 2> const targets{labelA, labelB};
    b.addIndirectBr(*jmpIndOp, addr, targets);
    b.beginBlock(labelA);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    b.beginBlock(labelB);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir const lir = std::move(b).finish();

    // 2 successors out of a 3-block function: neither `0/3` (the
    // under-approximation) nor `3/3` (the over-), which is what makes those two
    // strings meaningful verdicts in the test below.
    EXPECT_EQ(indirectBrShapes(sch, lir), "2/3")
        << "the probe must SEE an indirect branch and report its successor "
           "ratio; if this is empty every absence assertion below is vacuous";
}

// ★★★ THE TWO UNSOUND SHAPES, ASSERTED ABSENT — not merely "a diagnostic
// appeared". A future edit that "fixes" the refusal by handing `addIndirectBr`
// an EMPTY list, or by handing it every block of the function, would still emit
// a diagnostic-free module and would still pass a test that only checked for a
// message. This one reads the LIR.
TEST(AsmTextToLir, IndirectBranchEmitsNeitherUnsoundSuccessorShape) {
    auto const run = lowerAsmText(kindSplitJmpDoc(), src(
        ".globl main\n.func main\nmain:\n  jmp Lmid\nLmid:\n  jmp *%rax\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);

    // `<no module>` is the ONLY acceptable answer. `0/2` would be the
    // under-approximation, `2/2` the over-approximation, and "" would mean a
    // module was built with the indirect branch silently dropped — each is a
    // distinct regression and each fails here by name.
    EXPECT_EQ(indirectBrShapes(*run), "<no module>")
        << "a lowered module here means the successor set was INVENTED: "
           "`0/N` under-approximates the CFG (liveness and regalloc read the "
           "successor pool, so values live across the edge would be judged "
           "dead), `N/N` over-approximates it (this build deciding which "
           "labels the author meant), and an empty report means the branch "
           "was dropped entirely";
}

// ★★★ THE SAME REFUSAL ON A DIFFERENT DIALECT AND A DIFFERENT CPU, WHICH IS
// THE AGNOSTICISM CLAIM MADE CHECKABLE. aarch64 puts indirectness in the
// MNEMONIC (`b` / `br`), so nothing about the AT&T star is involved: the
// dispatch is the TARGET's `terminatorKind: indirect-br` and the refusal is the
// derivation coming up empty. A `if (arch == …)` anywhere on this path would
// have to make one of the two tests disagree with the other.
TEST(AsmTextToLir, DerivabilityRefusalIsDialectAndTargetAgnostic) {
    auto doc = shippedDialectDoc("asm-arm64-gas");
    // TWO spellings, not a kind-split row — this dialect binds `indirect: null`
    // and says so, which is exactly why it needs a second mnemonic instead.
    setInstructions(doc, {{"b", {"jmp"}, 64, ""},
                          {"br", {"jmp_indirect"}, 64, ""},
                          {"ret", {"ret"}, 64, ""}});
    setDirectives(doc, {{"text", "sectionText", ""},
                        {"globl", "globalSymbol", ""},
                        {"func", "functionEntry", ""}});
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  b Lmid\nLmid:\n  br x0\n"), "arm64");
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    std::string const msg = messages(*run);
    EXPECT_NE(msg.find("SUCCESSOR SET CANNOT BE DERIVED"), std::string::npos)
        << msg;
    EXPECT_NE(msg.find("binds no relocation"), std::string::npos) << msg;
    EXPECT_NE(msg.find("'br'"), std::string::npos)
        << "the refusal names THIS dialect's spelling: " << msg;
    EXPECT_EQ(indirectBrShapes(*run), "<no module>")
        << "neither unsound successor shape, on this target either";
}

// ★★ THE STAR IS THE ONLY DISCRIMINATOR, SO DROPPING IT MUST NOT SILENTLY
// PRODUCE A DIRECT BRANCH — and the mirror, adding it, must not silently
// produce one either. Same dialect row, same target, same function shape: the
// two runs differ in ONE character, and they must differ in VERDICT.
TEST(AsmTextToLir, TheStarIsTheOnlyDiscriminatorBetweenTheTwoBranches) {
    auto const doc = kindSplitJmpDoc();

    auto const direct = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  jmp Lmid\nLmid:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*direct)) << parseMessages(*direct);
    ASSERT_TRUE(direct->module.has_value()) << messages(*direct);
    auto const& lir = direct->module->lir;
    auto const  fn  = lir.funcAt(0);
    auto const  b0  = lir.funcBlockAt(fn, 0);
    ASSERT_EQ(lir.blockInstCount(b0), 1u);
    // It went through `addBr`: the DIRECT opcode, and exactly ONE successor —
    // the block the label named. An indirect lowering would show up as the
    // other opcode or as a different successor count.
    EXPECT_EQ(lir.instOpcode(lir.blockInstAt(b0, 0)), op(*direct->target, "jmp"))
        << "a direct `jmp` must elect the DIRECT opcode";
    EXPECT_EQ(lir.blockSuccessors(b0).size(), 1u);
    EXPECT_EQ(lir.blockSuccessors(b0)[0].v, lir.funcBlockAt(fn, 1).v);
    EXPECT_EQ(indirectBrShapes(*direct->target, lir), "")
        << "a direct branch must not lower to an indirect one";

    // One character different, and the verdict flips. ⚠ IT IS NOT ENOUGH THAT
    // THIS RUN FAILS — it must fail on the INDIRECT arm. A regression that lost
    // the star's meaning also refuses, but with `branchTarget`'s "needs a label
    // to branch to", because a register is not a label; that refusal is a
    // DIRECT-branch refusal wearing an indirect instruction's clothes, and a
    // test that only checked "no module" would call it green.
    auto const indirect = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  jmp *%rax\nLmid:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*indirect)) << parseMessages(*indirect);
    ASSERT_FALSE(indirect->module.has_value())
        << "dropping the star's meaning would silently produce a DIRECT "
           "branch to the next label: " << indirectBrShapes(*indirect);
    std::string const msg = messages(*indirect);
    EXPECT_NE(msg.find("jmp_indirect"), std::string::npos)
        << "the star must route to the INDIRECT arm: " << msg;
    EXPECT_NE(msg.find("SUCCESSOR SET CANNOT BE DERIVED"), std::string::npos)
        << msg;
    EXPECT_EQ(msg.find("needs a label to branch to"), std::string::npos)
        << "this is `branchTarget`'s DIRECT-branch refusal — reaching it means "
           "the star stopped discriminating and the instruction was lowered as "
           "a direct branch that merely happened to fail: " << msg;
}

// ★ THE REFUSAL THAT MUST SURVIVE: two candidates on the SAME side of the
// direct/indirect split still cannot share a spelling.
TEST(AsmTextToLir, TwoCandidatesOnOneSideOfTheSplitAreStillRefused) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    rows.push_back({"movjmp", {"mov", "jmp"}, 64, ""});
    setInstructions(doc, rows);
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("one spelling cannot denote both"),
              std::string::npos)
        << messages(*run);
    EXPECT_NE(messages(*run).find("on the same side of that split"),
              std::string::npos)
        << messages(*run);
}

// ★★ A KIND-SPLIT ROW IN A DIALECT WITH NO INDIRECT MARKER IS REFUSED AT LOAD.
// aarch64 puts indirectness in the MNEMONIC (`b` / `br`) and binds the
// `indirect` role to null, so nothing it can write would ever select the second
// opcode — the row would be half dead config.
TEST(AsmTextToLir, KindSplitRowNeedsADialectThatCanWriteTheIndirectMarker) {
    auto doc = shippedDialectDoc("asm-arm64-gas");
    setInstructions(doc, {{"b", {"jmp", "jmp_indirect"}, 64, ""},
                          {"ret", {"ret"}, 64, ""}});
    setDirectives(doc, {{"text", "sectionText", ""},
                        {"globl", "globalSymbol", ""},
                        {"func", "functionEntry", ""}});
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  ret\n"), "arm64");
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("declares no 'indirect' operand form"),
              std::string::npos)
        << messages(*run);
}

// ══ D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED ════════════════════════════
//
// ★★★ THE CONDITION AS AN OPERAND, RESOLVED AGAINST THE EXISTING
// `TargetCondCode` VOCABULARY — no operand role was minted. `operandForms` is
// REQUIRE-ALL, so an eighth role would have been a load error in every dialect
// document that did not mention it; and the reuse rule forbids a
// language-private vocabulary when a substrate one exists.
TEST(AsmTextToLir, Arm64ConditionOperandResolvesAgainstTargetCondCode) {
    auto doc = shippedDialectDoc("asm-arm64-gas");
    setInstructions(doc, {{"cset", {"setcc"}, 64, ""},   // NO row `cond`
                          {"ret",  {"ret"},   64, ""}});
    setDirectives(doc, {{"text", "sectionText", ""},
                        {"globl", "globalSymbol", ""},
                        {"func", "functionEntry", ""}});
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n"
        "  cset x0, eq\n"
        "  cset x1, ne\n"
        "  ret\n"), "arm64");
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const& lir = run->module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    ASSERT_EQ(lir.blockInstCount(blk), 3u);
    auto const csetEq = lir.blockInstAt(blk, 0);
    auto const csetNe = lir.blockInstAt(blk, 1);
    EXPECT_EQ(lir.instPayload(csetEq),
              static_cast<std::uint32_t>(TargetCondCode::Eq));
    // ⚠ `ne` IS THE DECIDING HALF. `TargetCondCode::Eq` is 0, so a lowering
    // that dropped the operand entirely would still encode `cset x0, eq`
    // correctly by accident.
    EXPECT_EQ(lir.instPayload(csetNe),
              static_cast<std::uint32_t>(TargetCondCode::Ne));
    // ★ AND THE CONDITION OPERAND IS REMOVED FROM THE SHAPE. arm64's `setcc`
    // takes ZERO value operands (it reads flags); leaving `eq` in the list
    // would present the elector a shape no declared variant accepts.
    EXPECT_TRUE(lir.instOperands(csetEq).empty());
    EXPECT_NE(lir.instResult(csetEq), InvalidLirReg);
}

// ★ TWO CONDITIONS ON ONE INSTRUCTION IS A REFUSAL, not a first-wins.
TEST(AsmTextToLir, TwoConditionOperandsAreRefused) {
    auto doc = shippedDialectDoc("asm-arm64-gas");
    setInstructions(doc, {{"cset", {"setcc"}, 64, ""},
                          {"ret",  {"ret"},   64, ""}});
    setDirectives(doc, {{"text", "sectionText", ""},
                        {"globl", "globalSymbol", ""},
                        {"func", "functionEntry", ""}});
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  cset x0, eq, ne\n  ret\n"), "arm64");
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("names more than one condition"),
              std::string::npos)
        << messages(*run);
}

// ★ A ROW THAT ALREADY FIXES ITS CONDITION REFUSES AN OPERAND-SPELLED ONE,
// rather than letting the row silently win over what the writer read.
TEST(AsmTextToLir, ConditionInBothTheMnemonicAndAnOperandIsRefused) {
    auto doc = shippedDialectDoc("asm-arm64-gas");
    setInstructions(doc, {{"cseteq", {"setcc"}, 64, "eq"},
                          {"ret",    {"ret"},   64, ""}});
    setDirectives(doc, {{"text", "sectionText", ""},
                        {"globl", "globalSymbol", ""},
                        {"func", "functionEntry", ""}});
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  cseteq x0, ne\n  ret\n"), "arm64");
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("already fixes its condition in the mnemonic"),
              std::string::npos)
        << messages(*run);
}

// ══ D-ASM-FIRST-FUNCTION-TAKES-SYMBOLID-ZERO ═══════════════════════════════
//
// ★★ THE DEFECT WAS INVISIBLE TO EVERY OUTPUT, which is exactly why it needs a
// pin rather than a fix alone. `classifyLabels` minted `SymbolId{0}` for the
// first function and `SymbolId::valid()` reports 0 as the INVALID sentinel;
// ✔MEASURED LATENT at the time (no `.valid()` gate sat on the link/emit path),
// so every emitted object was byte-correct and no test could have noticed. The
// first consumer to write `if (sym.valid())` would have silently dropped
// function 0.
TEST(AsmTextToLir, EverySymbolThisFileMintsIsValid) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n"
        "  call helper\n"
        "  call other\n"
        "  ret\n"
        ".func second\nsecond:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    ASSERT_EQ(run->module->symbols.size(), 2u);
    ASSERT_EQ(run->module->externImports.size(), 2u);
    std::vector<std::uint32_t> seen;
    for (auto const& sym : run->module->symbols) {
        EXPECT_TRUE(sym.symbol.valid())
            << "'" << sym.name << "' took SymbolId{0}, which SymbolId::valid() "
               "reports as the INVALID sentinel";
        seen.push_back(sym.symbol.v);
    }
    for (auto const& ext : run->module->externImports) {
        EXPECT_TRUE(ext.symbol.valid()) << ext.mangledName;
        seen.push_back(ext.symbol.v);
    }
    // ★ AND STILL DISTINCT — the linker's per-CU `declare()` rejects a
    // duplicate, so "start at 1" must not have collapsed two id spaces.
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(std::adjacent_find(seen.begin(), seen.end()), seen.end())
        << "two symbols share an id";
}

// ══ D-ASM-DOT-PREFIXED-LABEL-NOT-DEFINED-BY-CONSUMER ═══════════════════════
//
// ★★★ EVERY `gcc -S` OUTPUT WRITES ITS BRANCH TARGETS AS `.L`-PREFIXED LABELS.
// The shared grammar's `asmDirective` grew an `{optional asmLabelTail}` slot, so
// `.L3:` PARSES; this is the consumer half — a directive node carrying a label
// tail IS a label definition, and routing it to the directive vocabulary is
// what produced `A0008 unknown assembler directive '.L3'`.
TEST(AsmTextToLir, DotPrefixedLabelDefinesABlockNotADirective) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n"
        "  jmp Lend\n"
        ".L3:\n"
        "  movq $1, %rax\n"
        "Lend:\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    // ONE function, and `.L3` is one of its BLOCKS — entry, .L3, Lend.
    auto const shape = shapeOf(*run->module);
    EXPECT_EQ(shape.functions, 1u);
    EXPECT_EQ(shape.symbols, 1u);
    EXPECT_EQ(shape.blocks, 3u)
        << "`.L3:` did not mint a block — it was read as a directive";
}

// ★ THE NAME KEEPS THE INTRODUCER, read from the TREE and never hardcoded as
// ".": a dialect spelling its introducer differently must get a label name that
// matches what its own branches write.
TEST(AsmTextToLir, DotPrefixedLabelKeepsTheIntroducerInItsName) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n"
        ".L3:\n"
        "  ret\n"
        ".L3:\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("label '.L3' is defined more than once"),
              std::string::npos)
        << "the introducer must be part of the name — " << messages(*run);
}

// ══ D-ASM-DIRECTIVE-AFTER-LABEL-ON-ONE-LINE-DROPPED ════════════════════════
//
// ★★ `main: .globl main` SILENTLY DROPPED THE EXPORT. The scan's own comment
// claimed a directive in a label tail was "picked up when the walk reaches it";
// ✔MEASURED FALSE — `walkElements` visits LINE-level elements only, and a
// directive nested in a label tail is never one. The cost was an entry symbol
// with LOCAL linkage and a link failure pointing nowhere near the cause.
TEST(AsmTextToLir, DirectiveOnALabelLineIsApplied) {
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".func main\n"
        "main: .globl main\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);
    ASSERT_EQ(run->module->symbols.size(), 1u);
    EXPECT_EQ(run->module->symbols[0].binding, SymbolBinding::Global)
        << "the `.globl` on the label's own line was dropped";
    EXPECT_TRUE(run->module->userEntrySymbol.has_value());
}

// ══ D-ASM-NO-DATA-DEFINING-DIRECTIVE ═══════════════════════════════════════
//
// ★★★ THE VERBS BIND `DataSectionKind` AND `AssembledData` — the SAME row type
// and the SAME section taxonomy the C path's `lowerMirGlobalsToDataItems`
// produces. No parallel vocabulary was minted; `core/types/section_kind.hpp`
// was placed under `core/types/` for exactly this reason.
std::vector<DirRow> dataDirectives() {
    auto rows = baseDirectives();
    rows.push_back({"rodata", "sectionData", ""});
    rows.push_back({"data",   "sectionData", ""});
    rows.push_back({"bss",    "sectionData", ""});
    rows.push_back({"byte",   "emitData",    ""});
    rows.push_back({"quad",   "emitData",    ""});
    rows.push_back({"zero",   "reserveZeroBytes", ""});
    return rows;
}

void setDataDirectives(nlohmann::json& doc) {
    setDirectives(doc, dataDirectives());
    auto& arr = doc["assembly"]["directives"];
    for (auto& row : arr) {
        auto const sp = row["spelling"].get<std::string>();
        if (sp == "rodata") row["section"] = "rodata";
        if (sp == "data")   row["section"] = "data";
        if (sp == "bss")    row["section"] = "bss";
        if (sp == "byte")   row["unitBytes"] = 1;
        if (sp == "quad")   row["unitBytes"] = 8;
    }
}

TEST(AsmTextToLir, DataDirectivesProduceAssembledDataItems) {
    auto doc = baseDialectDoc();
    setDataDirectives(doc);
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  ret\n"
        ".rodata\n"
        ".globl msg\n"
        "msg:\n"
        "  .byte 72, 105, 0\n"
        ".data\n"
        "counter:\n"
        "  .quad 4660\n"
        ".bss\n"
        "scratch:\n"
        "  .zero 16\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    auto const& items = run->module->dataItems;
    ASSERT_EQ(items.size(), 3u) << "one item per data LABEL";

    EXPECT_EQ(items[0].section, DataSectionKind::Rodata);
    EXPECT_EQ(items[0].bytes, (std::vector<std::uint8_t>{72, 105, 0}));
    EXPECT_EQ(items[0].alignment.bytes(), 1u);

    EXPECT_EQ(items[1].section, DataSectionKind::Data);
    // 4660 = 0x1234, little-endian in an 8-byte unit.
    EXPECT_EQ(items[1].bytes,
              (std::vector<std::uint8_t>{0x34, 0x12, 0, 0, 0, 0, 0, 0}));
    EXPECT_EQ(items[1].alignment.bytes(), 8u)
        << "alignment is derived from the widest element the item carries";

    // ★ A ZERO-FILL SECTION RESERVES AN EXTENT AND STORES NO FILE BYTES — the
    // invariant `validateAssembledData` enforces, routed through the shared
    // `isZeroFill` predicate rather than an `== Bss` test.
    EXPECT_EQ(items[2].section, DataSectionKind::Bss);
    EXPECT_TRUE(items[2].bytes.empty());
    EXPECT_EQ(items[2].reservedSize, 16u);
    EXPECT_EQ(items[2].sizeInSection(), 16u);

    // Every item is well-formed by the linker's own validator.
    DiagnosticReporter rep;
    EXPECT_TRUE(validateAssembledData(items, rep));

    // ★ AND EACH DATA LABEL IS A MODULE SYMBOL, with the binding read from the
    // SAME `.globl` set the function arm reads — `msg` exported, the other two
    // local.
    bool sawMsg = false;
    for (auto const& sym : run->module->symbols) {
        if (sym.name != "msg") continue;
        sawMsg = true;
        EXPECT_EQ(sym.binding, SymbolBinding::Global);
    }
    EXPECT_TRUE(sawMsg) << "the data label minted no ModuleSymbol";
    for (auto const& sym : run->module->symbols) {
        if (sym.name == "counter" || sym.name == "scratch") {
            EXPECT_EQ(sym.binding, SymbolBinding::Local) << sym.name;
        }
    }
}

// ★ AN INSTRUCTION IN A DATA SECTION IS REFUSED. LIR places code in text only,
// so it would be emitted as if the data section were code.
TEST(AsmTextToLir, InstructionInsideADataSectionIsRefused) {
    auto doc = baseDialectDoc();
    setDataDirectives(doc);
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  ret\n"
        ".data\n"
        "  movq $1, %rax\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("DATA section is open"), std::string::npos)
        << messages(*run);
}

// ★ AND DATA IN THE TEXT SECTION IS REFUSED, the other direction: bytes written
// there would land where instructions go and be executed.
TEST(AsmTextToLir, DataWithNoOpenDataSectionIsRefused) {
    auto doc = baseDialectDoc();
    setDataDirectives(doc);
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  ret\n"
        "  .quad 1\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("no data section is open"), std::string::npos)
        << messages(*run);
}

// ★ BYTES IN A ZERO-FILL SECTION ARE REFUSED — they would be silently dropped
// by the wire format, which reserves the size without storing them.
TEST(AsmTextToLir, EmittingBytesIntoAZeroFillSectionIsRefused) {
    auto doc = baseDialectDoc();
    setDataDirectives(doc);
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  ret\n"
        ".bss\n"
        "b:\n  .quad 1\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("the open section is zero-fill"),
              std::string::npos)
        << messages(*run);
}

// ★ A VALUE THAT DOES NOT FIT ITS UNIT IS REFUSED, not truncated. Both signed
// and unsigned readings are accepted (`.byte 255` and `.byte -1` are the same
// byte and gas takes either), which is why the bound is a union rather than one
// interval.
TEST(AsmTextToLir, DataValueWiderThanItsUnitIsRefused) {
    auto doc = baseDialectDoc();
    setDataDirectives(doc);
    auto const ok = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  ret\n"
        ".rodata\nb:\n  .byte 255, -128\n"));
    ASSERT_TRUE(parsedCleanly(*ok)) << parseMessages(*ok);
    ASSERT_TRUE(ok->module.has_value()) << messages(*ok);
    ASSERT_EQ(ok->module->dataItems.size(), 1u);
    EXPECT_EQ(ok->module->dataItems[0].bytes,
              (std::vector<std::uint8_t>{0xFF, 0x80}));

    auto const bad = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  ret\n"
        ".rodata\nb:\n  .byte 256\n"));
    ASSERT_TRUE(parsedCleanly(*bad)) << parseMessages(*bad);
    EXPECT_FALSE(bad->module.has_value());
    EXPECT_NE(messages(*bad).find("does not fit the 1 byte(s)"),
              std::string::npos)
        << messages(*bad);
}

// ★ A SYMBOL-VALUED DATA ITEM IS REFUSED. The bytes would otherwise be whatever
// the address happened to be at compile time; a real one needs a relocation
// this build does not reach from assembly.
TEST(AsmTextToLir, SymbolValuedDataIsRefusedNotGuessed) {
    auto doc = baseDialectDoc();
    setDataDirectives(doc);
    auto const run = lowerAsmText(doc, src(
        ".globl main\n.func main\nmain:\n  ret\n"
        ".rodata\ntable:\n  .quad main\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("needs a relocation this build does not "
                                  "reach from assembly yet"),
              std::string::npos)
        << messages(*run);
}

// ── the `assembly.directives[]` loader contract ────────────────────────────

TEST(AsmTextToLir, SectionDataDirectiveRequiresASectionKey) {
    auto doc = baseDialectDoc();
    setDataDirectives(doc);
    for (auto& row : doc["assembly"]["directives"]) {
        if (row["spelling"] == "rodata") row.erase("section");
    }
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_FALSE(run->loadErrors.empty()) << "a sectionData row with no section "
                                             "must be a LOAD error";
    EXPECT_NE(parseMessages(*run).find("'section' is required"),
              std::string::npos)
        << parseMessages(*run);
}

TEST(AsmTextToLir, UnknownDataSectionNameIsALoadError) {
    auto doc = baseDialectDoc();
    setDataDirectives(doc);
    for (auto& row : doc["assembly"]["directives"]) {
        if (row["spelling"] == "rodata") row["section"] = "readonly";
    }
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_FALSE(run->loadErrors.empty());
    EXPECT_NE(parseMessages(*run).find("unknown data section 'readonly'"),
              std::string::npos)
        << parseMessages(*run);
}

TEST(AsmTextToLir, EmitDataDirectiveRequiresAUnitBytesKey) {
    auto doc = baseDialectDoc();
    setDataDirectives(doc);
    for (auto& row : doc["assembly"]["directives"]) {
        if (row["spelling"] == "quad") row.erase("unitBytes");
    }
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_FALSE(run->loadErrors.empty());
    EXPECT_NE(parseMessages(*run).find("'unitBytes' is required"),
              std::string::npos)
        << parseMessages(*run);
}

TEST(AsmTextToLir, UnitBytesOnANonEmitDataVerbIsALoadError) {
    auto doc = baseDialectDoc();
    setDataDirectives(doc);
    for (auto& row : doc["assembly"]["directives"]) {
        if (row["spelling"] == "rodata") row["unitBytes"] = 4;
    }
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_FALSE(run->loadErrors.empty());
    EXPECT_NE(parseMessages(*run).find("only meaningful on an 'emitData'"),
              std::string::npos)
        << parseMessages(*run);
}

TEST(AsmTextToLir, UnitBytesMustBeOneTwoFourOrEight) {
    auto doc = baseDialectDoc();
    setDataDirectives(doc);
    for (auto& row : doc["assembly"]["directives"]) {
        if (row["spelling"] == "quad") row["unitBytes"] = 3;
    }
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_FALSE(run->loadErrors.empty());
    EXPECT_NE(parseMessages(*run).find("is not one of 1, 2, 4, 8"),
              std::string::npos)
        << parseMessages(*run);
}

// ══ D-ASM-ATT-LEAL-UNREACHABLE-NO-WIDTH-KEYED-LEA ══════════════════════════
//
// ★★ THE ONLY DIFFERENCE BETWEEN `leaq` AND `leal` ON THIS ISA IS REX.W, which
// is exactly why a width-absent guard could not tell them apart: `leal` elected
// the REX.W form, and the width-honesty gate then refused it — so a `leal` row
// was UNSHIPPABLE rather than wrong. Keying the five shipped variants at 64 and
// adding the two 32-bit addressing twins is what makes it shippable, and the
// assertion goes to the BYTES because "it lowered" and "it emitted the 32-bit
// form" are different claims.
TEST(AsmTextToLir, LealReachesTheNonRexWFormAndLeaqKeepsRexW) {
    // ⓘ `leal` is already in the shared fixture vocabulary (at width 32) — it
    // was declared there when the gate that REFUSED it was written.
    auto const run = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n"
        "  leal 8(%rdi), %eax\n"
        "  leaq 8(%rdi), %rcx\n"
        "  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);

    DiagnosticReporter     rep;
    auto const&            lir = run->module->lir;
    std::vector<MirInstId> lirToMir(lir.instCount(), InvalidMirInst);
    auto const             asmMod = assemble(lir, *run->target, lirToMir, rep);
    ASSERT_TRUE(asmMod.ok());
    ASSERT_EQ(asmMod.functions.size(), 1u);
    auto const& bytes = asmMod.functions[0].bytes;

    // The encoder always writes the disp32 memory form, so:
    //   `leal 8(%rdi),%eax` = 8D 87 08 00 00 00      (ModRM 87 = mod=10 reg=0 rm=7)
    //   `leaq 8(%rdi),%rcx` = 48 8D 8F 08 00 00 00   (REX.W + ModRM 8F = reg=1 rm=7)
    EXPECT_EQ(countWindow(bytes, {0x8D, 0x87, 0x08, 0x00, 0x00, 0x00}), 1u)
        << "expected a REX-less 32-bit LEA — " << hex(bytes);
    EXPECT_EQ(countWindow(bytes, {0x48, 0x8D, 0x8F, 0x08, 0x00, 0x00, 0x00}), 1u)
        << "expected the REX.W 64-bit LEA — " << hex(bytes);
    // ⚠ THE RED ARM: if `leal` had elected the REX.W variant there would be TWO
    // `48 8D` windows and no bare `8D 47`.
    EXPECT_EQ(countWindow(bytes, {0x48, 0x8D}), 1u) << hex(bytes);
}
