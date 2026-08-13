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

#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <initializer_list>
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
    // ⚠ THE `.s` NEVER WRITES `je`. The refusal is about the ROW, so it must
    // fire anyway — a config that is "fine until someone uses it" is the shape
    // this check exists to reject.
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("declares no 'cond'"), std::string::npos)
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

// ★★★ THE MEASURED SILENT MISCOMPILE. x86's `lea` declares FOUR encoding
// variants and NOT ONE carries `guard.width`, and the shared matcher treats a
// width-absent guard as matching ANY width — so `leal` would have elected the
// REX.W (64-bit) form and encoded a 64-bit effective address with no
// diagnostic anywhere. The gate refuses the pairing instead.
TEST(AsmTextToLir, DeclaredWidthWithNoWidthKeyedVariantIsRefused) {
    auto const narrow = lowerAsmText(baseDialectDoc(), src(
        ".globl main\n.func main\nmain:\n  leal 8(%rdi), %eax\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*narrow)) << parseMessages(*narrow);
    EXPECT_FALSE(narrow->module.has_value())
        << "a width the elected variant cannot express must not be dropped";
    EXPECT_NE(messages(*narrow).find("no width-keyed encoding variant"),
              std::string::npos)
        << messages(*narrow);

    // The 64-bit sibling IS honest: a width-absent variant encodes at the
    // width a flags-less LIR instruction already means.
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

// ★★ A `subOf` ROW CAN NEVER ENTER AN ALLOCATION POOL, AND THERE ARE TWO
// INDEPENDENT REASONS — this test pins the one that lives in CONFIG. Both
// `buildFreeLists` (lir_regalloc.cpp) and `pickScratchRegs` (lir_rewrite.cpp)
// skip a non-empty `subOf`, AND every pool is additionally filtered to names
// the active calling convention lists. A future edit that put `eax` into a
// `callerSaved` list would leave only the first guard standing — so the config
// half is asserted here, where it is observable.
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
TEST(AsmTextToLir, CondIsRequiredOnANonTerminatorThatReadsItFromThePayload) {
    auto doc  = baseDialectDoc();
    auto rows = baseInstructions();
    rows.push_back({"sete", {"setcc"}, 64, ""});   // no `cond`
    setInstructions(doc, rows);
    // ⚠ THE `.s` NEVER WRITES `sete`: the refusal is about the ROW.
    auto const run = lowerAsmText(doc, src(".globl main\n.func main\nmain:\n  ret\n"));
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("declares no 'cond'"), std::string::npos)
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
