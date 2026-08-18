// The SHIPPED assembly dialect documents, UNMUTATED — `.section NAME` and the
// reserve-with-fill family.
//
// ★★★ WHY A SEPARATE FILE FROM `test_asm_text_to_lir.cpp`, WHICH ALREADY
// COVERS THESE VERBS. That file's fixture REPLACES `assembly.directives` with
// exactly the rows a test needs (`setDirectives`), which is the right posture
// for testing the ENGINE: it can declare a row combination no shipped dialect
// carries, and it is not hostage to a dialect's spelling choices. But it means
// every one of those tests is green whether or not the two SHIPPED documents
// declare the rows at all. The subject here is the opposite one: the
// `asm-x86_64-att.lang.json` and `asm-arm64-gas.lang.json` documents AS THEY
// SHIP, with nothing replaced.
//
// ★★ AND IT IS THE HOST-INDEPENDENT SIBLING OF A RUNTIME WITNESS. The corpus
// examples `asm_x86_64_section_reserve_fill_readback` and
// `asm_arm64_section_reserve_fill_readback` read these very bytes back at
// runtime and turn them into an exit code — which is the end-to-end proof, but
// the arm64 half only runs where `qemu-aarch64` is installed. The `dataItems`
// pin below asserts the SAME bytes on EVERY leg, including hosts that can
// execute neither binary. The bar asks for both and for neither to be
// collapsed into the other: the execution is the witness, this is the guard.
//
// ⚠ THE TWO DIALECTS' DIRECTIVE TEXT HERE IS BYTE-IDENTICAL AND THAT IS THE
// CLAIM. The only per-dialect bytes in the sources below are the
// function-entry marker (`@function` versus `%function`) — everything about
// the sections and the reserves is one string, asserted to produce one result.

#include "asm_text_fixture.hpp"
#include "mutate_target_schema.hpp"
#include "asm/asm.hpp"
#include "core/types/section_kind.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::test_support::asm_text;

namespace {

// The two shipped dialects, paired with the target each one spells and the
// function-entry marker each one writes. Every test below runs over BOTH —
// a mechanism witnessed by one dialect is the weaker claim.
struct ShippedDialect {
    std::string_view language;
    std::string_view target;
    std::string_view functionMarker;
};

constexpr ShippedDialect kX86{"asm-x86_64-att", "x86_64", "@function"};
constexpr ShippedDialect kArm{"asm-arm64-gas", "arm64", "%function"};

// A minimal exported function, spelled with THIS dialect's marker, so the
// directive text that follows has a lowered module to land in.
[[nodiscard]] std::string prologue(ShippedDialect const& d) {
    return std::string{"\t.globl main\n\t.type main, "}
           + std::string{d.functionMarker} + "\nmain:\n\tret\n";
}

// ★★★ THE CORPUS EXAMPLES' DATA BLOCK, VERBATIM. Keeping this string identical
// to what `examples/asm/*_section_reserve_fill_readback/main.s` writes is what
// makes this a guard for those fixtures rather than a test of its own private
// input. It is deliberately NOT parameterised: a helper that generated the
// text could generate it differently from the corpus and stay green.
constexpr std::string_view kCorpusData =
    "\t.section .rodata\n"
    "ro:\n"
    "\t.space\t1, 20\n"
    "\t.skip\t7\n"
    "\t.quad\t7\n"
    "\t.section .data\n"
    "rw:\n"
    "\t.zero\t1, 15\n"
    "\t.space\t7\n"
    "\t.section .bss\n"
    "bz:\n"
    "\t.space\t8, 3\n";

[[nodiscard]] std::unique_ptr<LoweringRun> lowerShipped(
    ShippedDialect const& d, std::string const& source) {
    return lowerAsmText(shippedDialectDoc(d.language), source, d.target);
}

// ══ THE BYTES THE CORPUS READS BACK ════════════════════════════════════════
//
// ★★★ THE EXIT CODE 42 THE CORPUS ASSERTS IS 20 + 7 + 15 + 0, AND THESE ARE
// THOSE FOUR NUMBERS AT THE BYTE TIER. A corpus example proves the bytes
// survive the linker and the loader; it cannot say WHICH byte was wrong when
// it fails, and it cannot run at all on a host without the emulator. This pin
// says both, on every leg.
void expectCorpusDataItems(ShippedDialect const& d) {
    auto const run = lowerShipped(d, prologue(d) + std::string{kCorpusData});
    ASSERT_TRUE(parsedCleanly(*run))
        << d.language << " did not PARSE the corpus data block: "
        << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value())
        << d.language << " refused the corpus data block: " << messages(*run);

    auto const& items = run->module->dataItems;
    ASSERT_EQ(items.size(), 3u) << d.language;

    // `.space 1, 20` + `.skip 7` + `.quad 7` — the fill byte, the unfilled
    // reserve's ZEROS, and an emitData landing at exactly +8 because the two
    // reserves advanced the cursor by exactly 8.
    EXPECT_EQ(items[0].section, DataSectionKind::Rodata)
        << d.language << ": `.section .rodata` must reach the rodata row";
    EXPECT_EQ(items[0].bytes,
              (std::vector<std::uint8_t>{20, 0, 0, 0, 0, 0, 0, 0,
                                          7, 0, 0, 0, 0, 0, 0, 0}))
        << d.language << ": the `.rodata` reserve/fill/quad layout";

    // `.zero 1, 15` + `.space 7` — `.zero` is NOT a fill-less special case.
    EXPECT_EQ(items[1].section, DataSectionKind::Data)
        << d.language << ": `.section .data` must reach the data row";
    EXPECT_EQ(items[1].bytes,
              (std::vector<std::uint8_t>{15, 0, 0, 0, 0, 0, 0, 0}))
        << d.language << ": `.zero`'s fill byte";

    // `.space 8, 3` in a zero-fill section — the size is reserved, the fill
    // has nowhere to live, and `bytes` MUST stay empty (the invariant
    // `validateAssembledData` enforces as K_BssDataHasBytes).
    EXPECT_EQ(items[2].section, DataSectionKind::Bss) << d.language;
    EXPECT_TRUE(items[2].bytes.empty())
        << d.language << ": a zero-fill item must store no file bytes";
    EXPECT_EQ(items[2].reservedSize, 8u) << d.language;

    // ★ THE WARNING IS PART OF THE CONTRACT, NOT NOISE. gas exits 0 with
    // `Warning: ignoring fill value in section '.bss'`; matching a reference
    // assembler means matching the diagnostic too, and the corpus example
    // depends on this arm being a WARNING rather than an error.
    EXPECT_NE(messages(*run).find("the open section is zero-fill"),
              std::string::npos)
        << d.language << ": the dropped fill must be announced: "
        << messages(*run);
}

TEST(AsmShippedDialects, X86AttLowersTheCorpusDataBlockToExactBytes) {
    expectCorpusDataItems(kX86);
}

TEST(AsmShippedDialects, Arm64GasLowersTheCorpusDataBlockToExactBytes) {
    expectCorpusDataItems(kArm);
}

// ══ `.section` FLAGS / TYPE OPERANDS ═══════════════════════════════════════
//
// ★★★ THE DECISION BOTH DIALECT DOCUMENTS STATE, PINNED AS A MESSAGE. Real
// `.section` carries flags and a type — ✔MEASURED, gas accepts
// `.section .rodata,"a",@progbits` rc=0 — and every one of those operands
// decides what the section IS: `"aw"` writable, `"ax"` executable, `@nobits`
// zero-fill. DSS derives all three from the `DataSectionKind`, so honouring
// the NAME while dropping the flags would place data somewhere the source did
// not ask for, with no diagnostic. The refusal is therefore the only arm with
// no silent path, and the MESSAGE is what makes it useful — "unknown
// directive" about a spelling the dialect visibly declares would send the
// reader to the wrong place.
void expectFlagsOperandRefused(ShippedDialect const& d,
                               std::string_view sectionLine,
                               std::string_view namedOperand) {
    auto const run = lowerShipped(
        d, prologue(d) + std::string{sectionLine} + "\nro:\n\t.quad 7\n");
    ASSERT_TRUE(parsedCleanly(*run))
        << d.language << " could not lex/parse " << sectionLine << ": "
        << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value())
        << d.language << ": a flags/type operand was ACCEPTED — the flags "
           "would then be silently dropped, which is the whole defect";
    auto const msg = messages(*run);
    EXPECT_NE(msg.find("models the section NAME and nothing else"),
              std::string::npos)
        << d.language << ": " << msg;
    // ★ THE REFUSAL NAMES THE OPERAND IT WILL NOT INTERPRET. A refusal that
    // said only "too many operands" would leave the reader guessing which one.
    EXPECT_NE(msg.find(namedOperand), std::string::npos)
        << d.language << ": the refusal must quote the offending operand: "
        << msg;
    // ★ AND IT NAMES WHY, so the reader learns the rule rather than the
    // symptom.
    EXPECT_NE(msg.find("writable, executable, zero-fill"), std::string::npos)
        << d.language << ": " << msg;
}

// ⚠ THE OPERAND SPELLING DIFFERS PER DIALECT BECAUSE THE TOKEN TABLES DO, AND
// THAT IS NOT A LOOSENING OF THE TEST. `@` is a token in the AT&T document
// (it writes `.type main, @function`) and is NOT one in the AArch64 document
// (which writes `%function`) — ✔MEASURED 2026-08-13: `.section .rodata,@progbits`
// reaches this refusal on x86_64 and dies at `P000E: got illegal character
// 0x40` on arm64. Each arm below therefore uses a sigil ITS OWN dialect can
// lex, so both reach the directive handler and the SAME message is asserted.
TEST(AsmShippedDialects, X86AttRefusesSectionFlagsAndTypeOperands) {
    expectFlagsOperandRefused(kX86, "\t.section .rodata,@progbits",
                              "@progbits");
}

TEST(AsmShippedDialects, Arm64GasRefusesSectionFlagsAndTypeOperands) {
    expectFlagsOperandRefused(kArm, "\t.section .rodata,%progbits",
                              "%progbits");
}

// ★ A PLAIN INTEGER SECOND OPERAND LEXES IN BOTH DIALECTS, so this arm is the
// one that proves the refusal is about the OPERAND COUNT rather than about a
// sigil neither table happens to carry.
TEST(AsmShippedDialects, BothDialectsRefuseANumericSecondSectionOperand) {
    expectFlagsOperandRefused(kX86, "\t.section .rodata,1", "1");
    expectFlagsOperandRefused(kArm, "\t.section .rodata,1", "1");
}

// ══ THE QUOTED FLAGS SPELLING ══════════════════════════════════════════════
//
// ★★★ THE EXACT LINE gcc -S WRITES IS REFUSED BY BOTH DIALECTS — AND THE
// REFUSAL COMES FROM A DIFFERENT SITE THAN THE ONE ABOVE, WHICH IS WORTH
// PINNING RATHER THAN DISCOVERING. ✔MEASURED 2026-08-13 through the real CLI:
// `.section .rodata,"a",@progbits` produces `P000E: got illegal character
// 0x22` on BOTH dialects, because neither token table declares a string
// literal. That is fail-loud — rc=1, no artifact, the flags are never applied
// and never ignored — but it is the LEXER talking, not the directive handler,
// so it does not name the flags/type rule.
//
// ★★ WHAT THIS TEST IS REALLY GUARDING IS THE FUTURE. The day a dialect grows
// a string-literal token — `.ascii` / `.asciz` / `.string` all need one — this
// line starts LEXING, and it must then land on the operand-count refusal
// rather than quietly becoming a `.section` whose flags are dropped. The
// assertion is therefore written as the INVARIANT that holds in both worlds:
// this source NEVER produces a module. It is deliberately not an assertion
// about which diagnostic fires, because that is exactly the part that is
// allowed to improve.
void expectQuotedFlagsNeverProducesAModule(ShippedDialect const& d) {
    auto const run = lowerShipped(
        d,
        prologue(d) + "\t.section .rodata,\"a\",@progbits\nro:\n\t.quad 7\n");
    EXPECT_FALSE(run->module.has_value())
        << d.language
        << ": the quoted flags spelling produced a module — its flags would "
           "then have been silently dropped";
    // Fail-closed: SOMETHING must have complained. A run that produced no
    // module and no diagnostic would be the silent arm wearing a green test.
    EXPECT_FALSE(parsedCleanly(*run) && messages(*run).empty())
        << d.language << ": refused with no diagnostic at all";
}

TEST(AsmShippedDialects, X86AttNeverAcceptsTheQuotedFlagsSpelling) {
    expectQuotedFlagsNeverProducesAModule(kX86);
}

TEST(AsmShippedDialects, Arm64GasNeverAcceptsTheQuotedFlagsSpelling) {
    expectQuotedFlagsNeverProducesAModule(kArm);
}

// ══ THE DELEGATION, ON THE SHIPPED DOCUMENTS ═══════════════════════════════
//
// ★★ `.section` MINTS NO SECOND SECTION VOCABULARY — it resolves its operand
// against the dialect's OWN section-opening rows. So `.section .data` and a
// bare `.data` must reach the IDENTICAL row, and `.rodata` — which is
// `operandOnly` in both shipped documents — must be reachable ONLY through
// `.section`. Both halves are asserted here against the real documents,
// because the engine-tier test that already asserts them does so over rows a
// fixture wrote.
void expectDelegationOnShippedDocument(ShippedDialect const& d) {
    auto const run = lowerShipped(
        d, prologue(d)
               + "\t.section .data\nviaName:\n\t.quad 1\n"
                 "\t.data\nviaDirective:\n\t.quad 2\n");
    ASSERT_TRUE(parsedCleanly(*run)) << d.language << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << d.language << messages(*run);
    auto const& items = run->module->dataItems;
    ASSERT_EQ(items.size(), 2u) << d.language;
    EXPECT_EQ(items[0].section, DataSectionKind::Data) << d.language;
    EXPECT_EQ(items[1].section, items[0].section)
        << d.language
        << ": `.section .data` and `.data` must resolve to ONE row — two "
           "section vocabularies is how they start to drift";
}

TEST(AsmShippedDialects, X86AttSectionByNameDelegatesToItsOwnRows) {
    expectDelegationOnShippedDocument(kX86);
}

TEST(AsmShippedDialects, Arm64GasSectionByNameDelegatesToItsOwnRows) {
    expectDelegationOnShippedDocument(kArm);
}

// ★ THE BIDIRECTIONAL HALF, ON THE SHIPPED DOCUMENTS: a bare `.rodata` is what
// the reference assembler calls an unknown pseudo-op, so declaring it as an
// ordinary row would make DSS accept a spelling gas does not. Both documents
// mark it `operandOnly`; this pins that the bare form is refused BY NAME and
// that the refusal points at the form that works.
void expectBareRodataRefused(ShippedDialect const& d) {
    auto const run =
        lowerShipped(d, prologue(d) + "\t.rodata\nro:\n\t.quad 7\n");
    ASSERT_TRUE(parsedCleanly(*run)) << d.language << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value()) << d.language;
    auto const msg = messages(*run);
    EXPECT_NE(msg.find("is a SECTION NAME in this dialect, not a directive"),
              std::string::npos)
        << d.language << ": " << msg;
    EXPECT_NE(msg.find("'section'"), std::string::npos)
        << d.language << ": the refusal must name the directive that DOES "
                         "reach it: "
        << msg;
}

TEST(AsmShippedDialects, X86AttRefusesBareRodata) { expectBareRodataRefused(kX86); }

TEST(AsmShippedDialects, Arm64GasRefusesBareRodata) { expectBareRodataRefused(kArm); }

// ══ THE ZERO-OPERAND INSTRUCTION ROWS (P5 wave 2) ══════════════════════════
//
// ★★★ THE DEFECT THESE ROWS CLOSE IS AN OPCODE WITH NO PRODUCER. Both targets
// declared `nop` (and x86_64 declared `rdtsc`, arm64 `cntvct`) in an earlier
// cycle with their encodings ✔MEASURED against the reference assembler — and NO
// source language could reach any of them. C's front end has no spelling, and
// neither dialect had a row. A capability that exists only in a table is the
// "never connected" shape the arm64 `mov`/W-form row already records once: the
// target grew a variant specifically so a `.s` could write `mov w0, w1`, and
// the `.s` could not.
//
// ★★ THE ASSERTION IS THE ELECTED OPCODE ORDINAL, NOT "IT PARSED". A row whose
// spelling resolved to the WRONG target opcode parses identically and emits a
// different instruction — the failure mode the `ldr`/`ldur` inversion produced
// for a whole cycle. `op()` resolves through the TARGET's own table, so the
// expectation cannot drift from the file it is about.

// Mnemonic → opcode ordinal through the target schema, so the expectation is
// derived from the same table the lowering elects against.
[[nodiscard]] std::uint16_t opOf(TargetSchema const& schema,
                                 std::string_view mnemonic) {
    auto const v = schema.opcodeByMnemonic(mnemonic);
    EXPECT_TRUE(v.has_value()) << "target declares no opcode " << mnemonic;
    return v.value_or(0);
}

// The opcode ordinals of the first function's first block, in order.
[[nodiscard]] std::vector<std::uint16_t> firstBlockOpcodes(LoweringRun const& run) {
    std::vector<std::uint16_t> out;
    auto const& lir = run.module->lir;
    auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
    for (std::size_t i = 0; i < lir.blockInstCount(blk); ++i) {
        out.push_back(lir.instOpcode(lir.blockInstAt(blk, i)));
    }
    return out;
}

// ── the CONFIG half: the row exists and names an opcode the target has ────
//
// ★★★ THIS IS THE ASSERTION THAT NEVER NEEDS WEAKENING, WHICH IS WHY IT IS
// SEPARATE FROM THE LOWERING ARM BELOW. "The dialect declares a producer for
// this target opcode" is a property of two config documents and of nothing
// else: it is true today, it stays true when the engine learns to lower a
// zero-operand instruction, and it goes red the moment a row is deleted or
// repointed. Folding it into the lowering test would have made a CONFIG claim
// hostage to an ENGINE capability — and when that engine arm went red, the
// tempting repair would have been to delete both.
void expectSpellingProducesOpcode(ShippedDialect const& d,
                                  std::string_view spelling,
                                  std::string_view opcodeName) {
    auto const doc = shippedDialectDoc(d.language);
    auto const run = lowerAsmText(doc, prologue(d), d.target);
    ASSERT_TRUE(run->loadErrors.empty())
        << d.language << " did not load: " << parseMessages(*run);

    auto const* row = run->grammar->assembly().instructionBySpelling(spelling);
    ASSERT_NE(row, nullptr)
        << d.language << " declares no '" << spelling
        << "' row — the target opcode '" << opcodeName
        << "' would have NO producer in any source language";
    ASSERT_EQ(row->opcodeNames.size(), 1u)
        << d.language << ": '" << spelling
        << "' must name exactly one candidate — there is nothing to elect over";
    EXPECT_EQ(row->opcodeNames[0], opcodeName)
        << d.language << ": '" << spelling << "' names the wrong target opcode";
    // ...and the name really resolves against the ACTIVE target, so the row
    // cannot be a producer for an opcode that does not exist.
    EXPECT_TRUE(run->target->opcodeByMnemonic(opcodeName).has_value())
        << d.language << ": target '" << d.target << "' declares no '"
        << opcodeName << "'";
    // ⚠ NEITHER ROW MAY CARRY A CONDITION: `resolveRows` refuses a `cond` on an
    // opcode whose encoder reads none, and a stray one would be a silently
    // encoded zero condition on the first target that grows a conditional form.
    EXPECT_TRUE(row->condName.empty())
        << d.language << ": '" << spelling << "' declares a condition";
}

TEST(AsmShippedDialects, X86AttDeclaresProducersForNopAndRdtsc) {
    expectSpellingProducesOpcode(kX86, "nop", "nop");
    expectSpellingProducesOpcode(kX86, "rdtsc", "rdtsc");
}

TEST(AsmShippedDialects, Arm64GasDeclaresAProducerForNop) {
    expectSpellingProducesOpcode(kArm, "nop", "nop");
}

// ── the ENGINE half: the invariant that holds in BOTH worlds ──────────────
//
// ★★★ AN ENGINE GAP THIS LANE FOUND AND DOES NOT OWN, PINNED AS THE INVARIANT
// RATHER THAN AS TODAY'S OUTCOME. With the rows above in place, ✔MEASURED
// 2026-08-14 on both dialects: `src/asm/asm_text_to_lir.cpp` REFUSES every
// zero-operand PLAIN (non-terminator) instruction — "'nop' has no operands,
// which this build cannot map onto target opcode(s) 'nop'". The refusal is
// unconditional at the `n == 0` guard, so it is not about `nop`: `ret` works
// only because a TERMINATOR is dispatched by `CfClass` before that guard is
// ever reached. Every zero-operand non-terminator on every target is
// unreachable, which is why both targets' `nop` had no producer to begin with.
// Anchored: D-ASM-ZERO-OPERAND-PLAIN-INSTRUCTION-UNLOWERABLE.
//
// ★★ THE ASSERTION IS DELIBERATELY NOT "IT IS REFUSED". A test pinning today's
// refusal would go red the day the engine is fixed, and the tempting repair
// would be to weaken it — the guard-weakened-by-its-own-subject shape. So the
// arm states what must be true in BOTH worlds: a bare `nop` NEVER silently
// becomes something else. Either it lowers to exactly the `nop` opcode in the
// SAME block, or it is refused with a diagnostic naming the mnemonic. There is
// no third outcome, and the day the engine lands this arm starts exercising the
// first branch with no edit.
void expectBareNopIsNeverSilentlyAnythingElse(ShippedDialect const& d) {
    auto const base = lowerShipped(d, prologue(d));   // `main: ret`
    ASSERT_TRUE(parsedCleanly(*base)) << d.language << parseMessages(*base);
    ASSERT_TRUE(base->module.has_value()) << d.language << messages(*base);
    auto const baseline = firstBlockOpcodes(*base);
    ASSERT_EQ(baseline.size(), 1u)
        << d.language << ": the prologue alone must be exactly `ret`";

    auto const run = lowerShipped(
        d, std::string{"\t.globl main\n\t.type main, "}
               + std::string{d.functionMarker} + "\nmain:\n\tnop\n\tret\n");
    // The line PARSES either way — the gap is in the lowering, not the grammar,
    // and saying so keeps a future reader from hunting in the token table.
    ASSERT_TRUE(parsedCleanly(*run))
        << d.language << " did not PARSE a bare `nop`: " << parseMessages(*run);

    if (run->module.has_value()) {
        auto const ops = firstBlockOpcodes(*run);
        ASSERT_EQ(ops.size(), 2u)
            << d.language
            << ": a `nop` before the `ret` must add exactly ONE instruction to "
               "the SAME block — a second block would mean the lowering closed "
               "the block on a non-terminator";
        EXPECT_EQ(ops[0], opOf(*run->target, "nop"))
            << d.language << ": `nop` elected the wrong target opcode";
        EXPECT_EQ(ops[1], baseline[0])
            << d.language << ": the trailing `ret` changed identity";
        EXPECT_EQ(run->module->lir.funcBlockCount(run->module->lir.funcAt(0)), 1u)
            << d.language << ": the `nop` split the function into blocks";
        return;
    }
    // Refused — then it must be LOUD and it must name what it refused.
    auto const msg = messages(*run);
    EXPECT_FALSE(msg.empty())
        << d.language << ": a bare `nop` produced no module AND no diagnostic";
    EXPECT_NE(msg.find("nop"), std::string::npos)
        << d.language << ": the refusal must name the mnemonic: " << msg;
}

TEST(AsmShippedDialects, X86AttNeverSilentlyMisreadsABareNop) {
    expectBareNopIsNeverSilentlyAnythingElse(kX86);
}

TEST(AsmShippedDialects, Arm64GasNeverSilentlyMisreadsABareNop) {
    expectBareNopIsNeverSilentlyAnythingElse(kArm);
}

// ★ THE BIDIRECTIONAL HALF: arm64 gas has NO `rdtsc` and x86 gas has no
// `cntvct`, so neither dialect may DECLARE the other's spelling. Accepting what
// the reference assembler rejects is the same defect as refusing what it
// accepts, and a copy-pasted row is the obvious way to introduce it.
//
// ⚠ ASSERTED AT THE CONFIG TIER, NOT BY "IT DID NOT LOWER", AND THAT MATTERS
// RIGHT NOW: the engine currently refuses EVERY zero-operand plain instruction
// (see the anchored gap below), so a lowering-tier assertion here would be
// green even if both rows had been copied across. The claim is about the
// vocabulary, so it is made against the vocabulary.
TEST(AsmShippedDialects, NeitherDialectDeclaresTheOtherCounterSpelling) {
    for (auto const& [d, alien] : {std::pair{kArm, std::string_view{"rdtsc"}},
                                   std::pair{kX86, std::string_view{"cntvct"}}}) {
        auto const run = lowerAsmText(shippedDialectDoc(d.language),
                                      prologue(d), d.target);
        ASSERT_TRUE(run->loadErrors.empty()) << parseMessages(*run);
        EXPECT_EQ(run->grammar->assembly().instructionBySpelling(alien), nullptr)
            << d.language << " declares '" << alien
            << "', which its reference assembler does not spell";
    }
}

// ══ POSITIONAL OPERAND SELECTORS ═══════════════════════════════════════════
//
// ★★★ THIS BLOCK REPLACES A PIN THAT ASSERTED THE OPPOSITE, AND THE REPLACEMENT
// IS THE POINT RATHER THAN A CHORE. `Arm64GasDoesNotHalfDeclareTheMrsCounterRead`
// asserted that `mrs x0, cntvct_el0` was REFUSED — the honest state while
// D-ASM-ARM64-SYSTEM-REGISTER-AS-OPERAND-UNMODELLED was open — and its own
// comment named the day it should go red: *"the arm that goes red the day a
// system-register operand role lands and the row can ship"*. It went red on
// exactly that day. What lands is not the operand role it predicted: the
// operand is not an operand at all, it is part of the mnemonic
// (`operandSelectors`, plan 29 §4.7).
//
// ⚠ EVERY WORD BELOW IS THE REFERENCE ASSEMBLER'S. ✔MEASURED 2026-08-14,
// `aarch64-linux-gnu-as` 2.42, one file per line, `objdump -d`. They are not
// derived from `arm64.target.json`'s fixed words — deriving them would make
// this a test of my arithmetic against itself, and the whole `condInvert`
// question (`CSET Xd,cond` is `CSINC Xd,XZR,XZR,invert(cond)`) is precisely
// where that self-agreement would hide a complemented condition.

// The encoded bytes of a one-instruction `main`, through the standalone `.s`
// path. The trailing `ret` is subtracted by comparing against the SAME function
// with the subject line removed, so the pin is exact rather than a prefix.
[[nodiscard]] std::vector<std::uint8_t>
shippedBytes(ShippedDialect const& d, std::string const& body) {
    auto const run = lowerShipped(
        d, std::string{"\t.globl main\n\t.type main, "}
               + std::string{d.functionMarker} + "\nmain:\n" + body + "\tret\n");
    EXPECT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_TRUE(run->module.has_value()) << messages(*run);
    if (!run->module.has_value()) return {};
    DiagnosticReporter     asmRep;
    std::vector<MirInstId> lirToMir(run->module->lir.instCount());
    auto const mod =
        assemble(run->module->lir, *run->target, lirToMir, asmRep);
    EXPECT_EQ(mod.functions.size(), 1u);
    if (mod.functions.size() != 1) return {};
    return mod.functions[0].bytes;
}

// The subject instruction's own word, with the terminator subtracted.
[[nodiscard]] std::uint32_t soleWordOf(ShippedDialect const& d,
                                       std::string const& line) {
    auto const withIt  = shippedBytes(d, line);
    auto const without = shippedBytes(d, "");
    EXPECT_EQ(withIt.size(), without.size() + 4u)
        << "`" << line << "` must add exactly ONE 4-byte word";
    if (withIt.size() != without.size() + 4u) return 0;
    EXPECT_TRUE(std::equal(without.begin(), without.end(),
                           withIt.begin() + 4))
        << "the terminator changed identity — the subtraction is not sound";
    return static_cast<std::uint32_t>(withIt[0])
         | (static_cast<std::uint32_t>(withIt[1]) << 8)
         | (static_cast<std::uint32_t>(withIt[2]) << 16)
         | (static_cast<std::uint32_t>(withIt[3]) << 24);
}

// ★★★ THE COUNTER READ, AT THE BYTE TIER. `arm64.target.json` has declared
// `cntvct` since before this dialect existed and NO source language could emit
// it — a declared-but-unreachable opcode, the shape a capability gap takes when
// nothing asks for it. One selector row makes it writable.
TEST(AsmShippedDialects, Arm64GasMrsCounterReadIsTheMeasuredWord) {
    EXPECT_EQ(soleWordOf(kArm, "\tmrs x0, cntvct_el0\n"), 0xD53BE040u)
        << "gas 2.42 assembles `mrs x0, cntvct_el0` to exactly this word";
}

// ★★★ THE ROW THAT PROVES THE SELECTOR *SELECTS* RATHER THAN BEING IGNORED
// (§4.7.3). `tpidr_el0` is a REAL system register — ✔MEASURED, gas assembles
// `mrs x0, tpidr_el0` happily to d53bd040 — so an implementation that merely
// DROPPED operand 1 would encode the thread-pointer read as the counter read
// and be green here. The refusal is the whole assertion.
TEST(AsmShippedDialects, Arm64GasRefusesAnUnselectedSystemRegister) {
    auto const run = lowerShipped(
        kArm, std::string{"\t.globl main\n\t.type main, "}
                  + std::string{kArm.functionMarker}
                  + "\nmain:\n\tmrs x0, tpidr_el0\n\tret\n");
    ASSERT_TRUE(parsedCleanly(*run))
        << "the line must LEX and PARSE — the refusal is vocabulary, not "
           "syntax: " << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value())
        << "`mrs x0, tpidr_el0` LOWERED — the selector is being dropped rather "
           "than matched, so a thread-pointer read just became a counter read";
    auto const msg = messages(*run);
    EXPECT_NE(msg.find("mrs"), std::string::npos)
        << "the refusal must name the spelling: " << msg;
    EXPECT_NE(msg.find("tpidr_el0"), std::string::npos)
        << "the refusal must name the operand that selected nothing: " << msg;
    EXPECT_NE(msg.find("cntvct_el0"), std::string::npos)
        << "the refusal must name what this dialect DOES accept, or the reader "
           "cannot tell a typo from an unmodelled register: " << msg;
}

// ★★★ RED-ON-DISABLE FOR THE KEY ITSELF: delete the `operandSelectors` array
// from the shipped `mrs` row and the line must FAIL LOUD — never fall back to a
// generic match that reads `cntvct_el0` as a register-or-symbol and hands the
// lowering a leftover operand.
//
// ⚠ THE MUTATION IS PROVED TO HAVE LANDED, NOT ASSUMED. `erasedSelector` is
// checked against the shipped document BEFORE the run, so if a future edit
// renames the key or drops the row this test fails as a BROKEN PIN instead of
// passing vacuously — the failure mode a red-on-disable arm exists to have.
TEST(AsmShippedDialects, Arm64GasWithoutTheSelectorRefusesTheCounterRead) {
    auto doc = shippedDialectDoc(kArm.language);
    bool erasedSelector = false;
    for (auto& row : doc["assembly"]["instructions"]) {
        if (row.value("spelling", std::string{}) != "mrs") continue;
        ASSERT_TRUE(row.contains("operandSelectors"))
            << "the shipped `mrs` row carries no 'operandSelectors' — this pin "
               "would be asserting nothing";
        row.erase("operandSelectors");
        erasedSelector = true;
    }
    ASSERT_TRUE(erasedSelector)
        << "the shipped arm64 dialect has no `mrs` row to mutate";

    auto const run = lowerAsmText(
        doc, std::string{"\t.globl main\n\t.type main, "}
                 + std::string{kArm.functionMarker}
                 + "\nmain:\n\tmrs x0, cntvct_el0\n\tret\n",
        kArm.target);
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value())
        << "without the selector, `mrs x0, cntvct_el0` LOWERED — the row is "
           "reaching `cntvct` with a leftover operand against maxOperands: 0, "
           "or the selector is not what made the match";
    auto const msg = messages(*run);
    EXPECT_FALSE(msg.empty()) << "the refusal must be LOUD";
    EXPECT_NE(msg.find("mrs"), std::string::npos)
        << "the refusal must name the spelling: " << msg;
    EXPECT_NE(msg.find(run->grammar->name()), std::string::npos)
        << "the refusal must name the DIALECT: " << msg;
}

// ★★★ §4.7.1 — THE AMBIGUITY REFUSAL, EXERCISED RATHER THAN READ. Add a BARE
// `mrs` row beside the selector one: the bare row matches everything the
// selector row matches, so the two could both take `mrs x0, cntvct_el0` and the
// document must be refused AT LOAD, naming both. Without this check, selectors
// buy the `cntvct` row by planting the `ldr`/`ldur` bug one level up — that
// pair was split precisely because election *"would take the first and silently
// encode LDR where the programmer wrote LDUR"*.
TEST(AsmShippedDialects, Arm64GasRefusesTwoMrsRowsThatCouldBothMatch) {
    auto doc = shippedDialectDoc(kArm.language);
    doc["assembly"]["instructions"].push_back(nlohmann::json::parse(
        R"({"spelling": "mrs", "opcodes": ["cntvct"], "width": 64})"));

    auto const run = lowerAsmText(doc, prologue(kArm), kArm.target);
    ASSERT_FALSE(run->loadErrors.empty())
        << "a bare `mrs` row beside the selector row LOADED CLEAN — election "
           "between them would be first-match, which is exactly the silent "
           "wrong-instruction the split of `ldr`/`ldur` exists to prevent";
    std::string const joined = parseMessages(*run);
    EXPECT_NE(joined.find("mrs"), std::string::npos)
        << "the load error must name the spelling: " << joined;
    // ★ BOTH ROWS, BY PATH. A message naming one row sends the reader to delete
    // whichever it happens to mention; the conflict is between two.
    EXPECT_NE(joined.find("/assembly/instructions/"), std::string::npos)
        << "the load error must locate the rows: " << joined;
}

// ★ AND THE COMPLEMENT, SO THE REFUSAL ABOVE IS NOT JUST "TWO ROWS ARE ALWAYS
// REFUSED": twelve `cset` rows share a spelling and LOAD CLEAN, because their
// selectors separate them at one index. A predicate that refused every
// duplicate spelling would pass the test above and make this file impossible.
TEST(AsmShippedDialects, Arm64GasAcceptsTwelveCsetRowsSharingOneSpelling) {
    auto const run = lowerShipped(kArm, prologue(kArm));
    ASSERT_TRUE(run->loadErrors.empty())
        << "the shipped dialect did not load: " << parseMessages(*run);
    EXPECT_EQ(run->grammar->assembly().instructionRowCount("cset"), 12u)
        << "the twelve gas condition spellings must each have their own row";
    EXPECT_EQ(run->grammar->assembly().instructionRowCount("mrs"), 1u);
}

// ★ A SELECTOR IS A BARE OPERAND, AND `#eq` IS NOT ONE. ✔MEASURED: gas takes
// `cset x0, eq` and REJECTS `cset x0, #eq`. A "last visible token" reading of
// the operand would have matched the immediate form silently, so the engine
// requires the operand's whole visible span to be ONE token.
TEST(AsmShippedDialects, Arm64GasDoesNotSelectThroughAnImmediateSigil) {
    auto const run = lowerShipped(
        kArm, std::string{"\t.globl main\n\t.type main, "}
                  + std::string{kArm.functionMarker}
                  + "\nmain:\n\tcset x0, #eq\n\tret\n");
    ASSERT_TRUE(parsedCleanly(*run))
        << "`#eq` must LEX and PARSE as an immediate: " << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value())
        << "`cset x0, #eq` LOWERED — the selector matched through the "
           "immediate sigil, accepting a line gas rejects";
}

// ★★★ THE TWELVE CONDITIONS × BOTH WIDTHS, EACH PINNED TO THE WORD gas EMITS.
//
// Two independent claims share one table, and neither survives without the
// other half of the row:
//
//   * THE CONDITION PAIRING — the assertion the `b.<cc>` comment's warning is
//     about. gas's `lt/le/gt/ge` are SIGNED and its unsigned peers are
//     `lo/ls/hi/hs`, so a table built by pattern-matching the letters maps `ls`
//     to `sle` and miscompiles every unsigned comparison. Only a per-condition
//     BYTE pin catches that — an opcode-level assertion is green for all twelve
//     no matter how they are paired.
//   * ★★★ THE WIDTH ELECTION (D-ASM-ARM64-SETCC-W-FORM-UNDECLARED, closed
//     2026-08-15). `arm64.target.json` now declares `setcc` at width 64
//     (0x9A9F07E0) AND width 32 (0x1A9F07E0), and the twelve dialect rows
//     declare NO `width`, so the register spelling elects the variant. ⚠ A
//     SINGLE-WIDTH TEST CANNOT SEE THE ELECTION PICKING THE WRONG VARIANT,
//     WHICH IS THE ENTIRE FAILURE MODE: before the width-32 variant existed, a
//     bare row would have derived 32 from `w0`, matched the ONE width-absent
//     variant anyway and emitted the 64-bit word — green against an x-only
//     table. The pair is what makes that impossible.
//
// ⚠ EVERY WORD BELOW WAS RE-MEASURED 2026-08-15 rather than carried over.
// `aarch64-linux-gnu-as` 2.42 + `aarch64-linux-gnu-objdump -d`, all
// twenty-four lines in ONE file so both widths of a condition come off one
// assembler run. The w-form is the x-form with bit 31 (sf) cleared in all
// twelve — asserted here as measured bytes rather than as that rule, because
// the rule is exactly the kind of thing a wrong `fixedWord` would still satisfy.
TEST(AsmShippedDialects, Arm64GasCsetEncodesEveryConditionAtBothWidths) {
    struct Measured {
        std::string_view cc;
        std::uint32_t    xForm;   // `cset x0, <cc>`
        std::uint32_t    wForm;   // `cset w0, <cc>`
    };
    constexpr Measured kMeasured[]{
        {"eq", 0x9A9F17E0u, 0x1A9F17E0u}, {"ne", 0x9A9F07E0u, 0x1A9F07E0u},
        {"lt", 0x9A9FA7E0u, 0x1A9FA7E0u}, {"le", 0x9A9FC7E0u, 0x1A9FC7E0u},
        {"gt", 0x9A9FD7E0u, 0x1A9FD7E0u}, {"ge", 0x9A9FB7E0u, 0x1A9FB7E0u},
        {"lo", 0x9A9F27E0u, 0x1A9F27E0u}, {"ls", 0x9A9F87E0u, 0x1A9F87E0u},
        {"hi", 0x9A9F97E0u, 0x1A9F97E0u}, {"hs", 0x9A9F37E0u, 0x1A9F37E0u},
        {"cc", 0x9A9F27E0u, 0x1A9F27E0u}, {"cs", 0x9A9F37E0u, 0x1A9F37E0u},
    };
    for (auto const& m : kMeasured) {
        EXPECT_EQ(soleWordOf(kArm, std::format("\tcset x0, {}\n", m.cc)),
                  m.xForm)
            << "`cset x0, " << m.cc << "` must encode as gas encodes it";
        EXPECT_EQ(soleWordOf(kArm, std::format("\tcset w0, {}\n", m.cc)),
                  m.wForm)
            << "`cset w0, " << m.cc << "` must encode as gas encodes it — a "
               "value equal to the x-form means the width-32 variant was not "
               "elected and the 64-bit word shipped for a 32-bit source line";
        // ★ STATED SEPARATELY SO A FAILURE SAYS *WHICH* CLAIM BROKE. Two rows
        // that were accidentally the same value would satisfy both EXPECTs
        // above only if the measured table itself were wrong; this says the
        // two spellings are different INSTRUCTIONS, which is the fact the
        // anchor was about.
        EXPECT_NE(m.xForm, m.wForm) << m.cc;
    }
}

// ★★★ THE OTHER DIRECTION, AND IT IS WHAT SEPARATES THESE ROWS FROM A
// MECHANISM THAT ALREADY EXISTED. `asm_template_to_lir.cpp`'s
// `condCodeOfOperand` ALREADY resolves a bare-name operand against
// `kTargetCondCodeTable` when a row declares no `cond` — the mechanism the
// registry records as closing D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED on
// 2026-08-13. ✔MEASURED: it is keyed on the SUBSTRATE's names (`slt`, `ult`,
// `sle`…), and gas writes `lt`, `lo`, `ls`. So a bare `cset` row leaning on it
// would be wrong in BOTH directions on ten of the twelve spellings — refusing
// `cset x0, lt` (which gas takes) and accepting `cset x0, slt` (which gas
// rejects: ✔MEASURED, `aarch64-linux-gnu-as` 2.42 gives "invalid condition at
// operand 2"). The selector rows are keyed on the GAS spelling, so both
// directions come out right, and THAT is the claim this pin makes.
// ⚠ IT GOES RED IF ANYONE "SIMPLIFIES" THE TWELVE ROWS INTO ONE BARE ROW —
// which is the obvious cleanup, and the reason this test is here rather than
// only in a comment.
TEST(AsmShippedDialects, Arm64GasCsetTakesGasSpellingsAndRefusesSubstrateNames) {
    for (auto const gasSpelling : {"eq", "lt", "lo", "ls", "hs"}) {
        auto const run = lowerShipped(
            kArm, std::string{"\t.globl main\n\t.type main, "}
                      + std::string{kArm.functionMarker} + "\nmain:\n\tcset x0, "
                      + gasSpelling + "\n\tret\n");
        EXPECT_TRUE(run->module.has_value())
            << "gas accepts `cset x0, " << gasSpelling << "` and this dialect "
               "must too: " << messages(*run);
    }
    // The substrate's OWN condition names are not gas spellings, and a dialect
    // that accepted them would be accepting lines no reference assembler does.
    for (auto const substrateName : {"slt", "sle", "sgt", "sge", "ult", "uge"}) {
        auto const run = lowerShipped(
            kArm, std::string{"\t.globl main\n\t.type main, "}
                      + std::string{kArm.functionMarker} + "\nmain:\n\tcset x0, "
                      + substrateName + "\n\tret\n");
        EXPECT_FALSE(run->module.has_value())
            << "`cset x0, " << substrateName
            << "` LOWERED — that is a DSS substrate name, not a gas spelling, "
               "and accepting it is the same defect as refusing a real one";
    }
}

// ★★ THE ALIASES ARE ALIASES — `cc`/`cs` must produce bytes IDENTICAL to
// `lo`/`hs`, which is what an alias MEANS and is why both spellings ship.
TEST(AsmShippedDialects, Arm64GasCsetAliasesEncodeIdentically) {
    EXPECT_EQ(soleWordOf(kArm, "\tcset x0, cc\n"),
              soleWordOf(kArm, "\tcset x0, lo\n"));
    EXPECT_EQ(soleWordOf(kArm, "\tcset x0, cs\n"),
              soleWordOf(kArm, "\tcset x0, hs\n"));
}

// ★★★ AN UNKNOWN CONDITION IS REFUSED BY NAME. `cset x0, al` is a real gas
// spelling this dialect does not model; it must not fall through to a
// register-or-symbol reading and emit a `setcc` with whatever condition
// happened to be in the payload.
TEST(AsmShippedDialects, Arm64GasRefusesAnUnmodelledCsetCondition) {
    auto const run = lowerShipped(
        kArm, std::string{"\t.globl main\n\t.type main, "}
                  + std::string{kArm.functionMarker}
                  + "\nmain:\n\tcset x0, al\n\tret\n");
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value())
        << "`cset x0, al` LOWERED — an unmodelled condition became some other "
           "condition with no diagnostic";
    EXPECT_NE(messages(*run).find("cset"), std::string::npos)
        << messages(*run);
}

// ★★★ RED-ON-DISABLE FOR THE W-FORM (D-ASM-ARM64-SETCC-W-FORM-UNDECLARED).
//
// This test USED TO ASSERT THE REFUSAL — `cset w0, eq` was rejected, because
// `arm64.target.json` declared ONE width-ABSENT `setcc` variant and a
// width-absent variant matches ANY width, so the twelve dialect rows carried
// `width: 64` to force `effectiveWidth` to refuse rather than let the 64-bit
// word ship for a 32-bit source line. The variant now exists and the rows no
// longer pin a width, so the flip to asserting EMISSION is the close, not a
// weakening — the byte pins above are the positive half.
//
// ★★ WHAT THIS ARM ADDS THAT THE BYTE PINS CANNOT: it proves the width-32
// variant is what makes them green. Strip it back out of the shipped target and
// the line must FAIL LOUD — never fall back to the surviving width-64 variant,
// which is the pre-fix silent miscompile arriving by a different route.
//
// ⚠ THE MUTANT IS PROVED TO HAVE BEEN BUILT *AND* READ, all four clauses:
//   (1) `mutateShippedTargetSchemaDoc` THROWS if the document came out
//       byte-identical, so a navigator that reached nothing cannot pass;
//   (2) the variant COUNT is asserted to have dropped by exactly one;
//   (3) the mutant still LOADS (`schemaR.has_value()`), so the refusal below is
//       the election failing and not the schema being rejected;
//   (4) `cset x0, eq` is asserted to STILL emit its measured word THROUGH THE
//       MUTANT — which is what says the run read the mutated schema at all and
//       that only the W arm went missing.
// ⓘ It reads the shipped `.target.json`, so it must run under `ctest`
// (`dss_add_test` sets `DSS_CONFIG_ROOT`; a bare .exe walks the cwd —
// D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).
TEST(AsmShippedDialects, Arm64GasCsetWFormNeedsTheTargetsWidth32Variant) {
    std::size_t before = 0;
    std::size_t after  = 0;
    auto const schemaR = dss::test_support::mutateShippedTargetSchemaDoc(
        "arm64", [&](nlohmann::json& doc) {
            for (auto& op : doc["opcodes"]) {
                auto const it = op.find("mnemonic");
                if (it == op.end() || !it->is_string()) continue;
                if (it->get<std::string>() != "setcc") continue;
                auto& variants = op["encoding"]["variants"];
                before = variants.size();
                for (std::size_t i = 0; i < variants.size(); ++i) {
                    auto const& g = variants[i]["guard"];
                    if (g.contains("width") && g["width"] == 32) {
                        variants.erase(variants.begin()
                                       + static_cast<long>(i));
                        break;
                    }
                }
                after = variants.size();
            }
        });
    ASSERT_EQ(before, 2u)
        << "the shipped `setcc` no longer declares exactly two variants — this "
           "pin's subject moved and it would be asserting something else";
    ASSERT_EQ(after, 1u) << "the width-32 `setcc` variant was not erased";
    ASSERT_TRUE(schemaR.has_value())
        << "the mutant target did not LOAD, so the refusal below would be "
           "about a broken schema rather than about the missing variant";

    auto const body = [](std::string_view line) {
        return std::string{"\t.globl main\n\t.type main, "}
               + std::string{kArm.functionMarker} + "\nmain:\n"
               + std::string{line} + "\tret\n";
    };

    auto const w = lowerAsmTextWithTarget(shippedDialectDoc(kArm.language),
                                          body("\tcset w0, eq\n"), *schemaR);
    ASSERT_TRUE(parsedCleanly(*w)) << parseMessages(*w);
    EXPECT_FALSE(w->module.has_value())
        << "with the width-32 variant stripped, `cset w0, eq` LOWERED — the "
           "election fell back to the width-64 variant and emitted the 64-bit "
           "word for a 32-bit source line, which is the exact miscompile the "
           "old `width: 64` row pin existed to refuse";
    EXPECT_FALSE(messages(*w).empty()) << "the refusal must be LOUD";

    // ★ THE MUTANT WAS READ: the X form is untouched by the mutation and must
    // still assemble to its measured word through the SAME mutated schema. A
    // mutant that had failed to reach the run would make the arm above green
    // for the wrong reason only if this one went red — so the two together are
    // the claim.
    auto const x = lowerAsmTextWithTarget(shippedDialectDoc(kArm.language),
                                          body("\tcset x0, eq\n"), *schemaR);
    ASSERT_TRUE(x->module.has_value())
        << "the mutant refused the X form too, so the mutation removed more "
           "than the W arm: " << messages(*x);
    DiagnosticReporter     rep;
    std::vector<MirInstId> lirToMir(x->module->lir.instCount());
    auto const mod = assemble(x->module->lir, *x->target, lirToMir, rep);
    ASSERT_EQ(mod.functions.size(), 1u);
    ASSERT_GE(mod.functions[0].bytes.size(), 4u);
    auto const& b = mod.functions[0].bytes;
    EXPECT_EQ(static_cast<std::uint32_t>(b[0])
                  | (static_cast<std::uint32_t>(b[1]) << 8)
                  | (static_cast<std::uint32_t>(b[2]) << 16)
                  | (static_cast<std::uint32_t>(b[3]) << 24),
              0x9A9F17E0u)
        << "`cset x0, eq` changed identity under a mutation that only removed "
           "the width-32 variant";
}

// ★★ THE SAME ARGUMENT ON `mrs`, WHERE THE REFERENCE ASSEMBLER AGREES WITH US.
// ✔MEASURED: gas REJECTS `mrs w0, cntvct_el0` ("operand mismatch") — MRS always
// writes an X register. Without the row's `width: 64` the width-absent `cntvct`
// variant would take the 32-bit derivation and emit d53be040 anyway, accepting
// what the reference assembler rejects.
TEST(AsmShippedDialects, Arm64GasMrsWFormIsRefusedAsGasRefusesIt) {
    auto const run = lowerShipped(
        kArm, std::string{"\t.globl main\n\t.type main, "}
                  + std::string{kArm.functionMarker}
                  + "\nmain:\n\tmrs w0, cntvct_el0\n\tret\n");
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value())
        << "`mrs w0, cntvct_el0` LOWERED — gas rejects this line, and "
           "accepting what the reference assembler rejects is the other half "
           "of bidirectional conformance";
}

// ★★★ A SELECTOR AT INDEX 0 SUPPRESSES `destinationFirst` FOR THAT POSITION —
// the clause plan 29 §4.7 requires the KEY to get right even though nothing
// ships it. gas's `msr tpidr_el0, x0` writes the selector FIRST (✔MEASURED,
// d51bd040), and this dialect is `destinationFirst`, so a position-blind key
// would read the SELECTOR as the destination and the real destination as a
// source. No `msr` row ships (nothing needs one, and §A.2 cuts both ways), so
// the clause is exercised with an in-process row of the same SHAPE — which is
// the only way to test a property of the key rather than of a spelling.
//
// ⚠ THE ASSERTION IS THE DESTINATION REGISTER, NOT MERELY "IT LOWERED".
// Reading position 0 as the destination would still lower — it would just
// encode the counter into the wrong register, silently. `rd` is the only thing
// that can tell the two apart, so the pin is the encoded word.
TEST(AsmShippedDialects, Arm64GasASelectorAtIndexZeroIsNotReadAsTheDestination) {
    auto doc = shippedDialectDoc(kArm.language);
    ASSERT_EQ(doc["assembly"].value("operandOrder", std::string{}),
              "destinationFirst")
        << "this pin only means something on a destination-FIRST dialect";
    doc["assembly"]["instructions"].push_back(nlohmann::json::parse(
        R"({"spelling": "rdcnt", "opcodes": ["cntvct"], "width": 64,
            "operandSelectors": [{ "index": 0, "name": "virt" }]})"));

    auto const run = lowerAsmText(
        doc, std::string{"\t.globl main\n\t.type main, "}
                 + std::string{kArm.functionMarker}
                 + "\nmain:\n\trdcnt virt, x1\n\tret\n",
        kArm.target);
    ASSERT_TRUE(run->loadErrors.empty()) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value())
        << "an index-0 selector did not lower: " << messages(*run);

    DiagnosticReporter     asmRep;
    std::vector<MirInstId> lirToMir(run->module->lir.instCount());
    auto const mod =
        assemble(run->module->lir, *run->target, lirToMir, asmRep);
    ASSERT_EQ(mod.functions.size(), 1u);
    ASSERT_GE(mod.functions[0].bytes.size(), 4u);
    auto const& b    = mod.functions[0].bytes;
    auto const  word = static_cast<std::uint32_t>(b[0])
                     | (static_cast<std::uint32_t>(b[1]) << 8)
                     | (static_cast<std::uint32_t>(b[2]) << 16)
                     | (static_cast<std::uint32_t>(b[3]) << 24);
    // 0xD53BE041 — the counter read with rd = x1, i.e. the SECOND written
    // operand. 0xD53BE040 (rd = x0) would mean position 0 was still being read
    // as the destination and the selector merely dropped.
    EXPECT_EQ(word, 0xD53BE041u)
        << "the destination came from the wrong position — an index-0 selector "
           "must be excluded from the destinationFirst reading, not merely "
           "skipped afterwards";
}

// ★ THE SELECTOR NEVER REACHES THE TARGET. `cntvct` declares maxOperands: 0 and
// `setcc` likewise; if the selector were passed through as an operand, BOTH
// would fail election. The byte pins above already imply it — this states it at
// the LIR tier so a failure says WHICH half broke.
TEST(AsmShippedDialects, Arm64GasSelectorOperandsNeverBecomeLirOperands) {
    for (auto const& [line, opcode] :
         {std::pair{std::string_view{"\tmrs x0, cntvct_el0\n"},
                    std::string_view{"cntvct"}},
          std::pair{std::string_view{"\tcset x0, eq\n"},
                    std::string_view{"setcc"}}}) {
        auto const run = lowerShipped(
            kArm, std::string{"\t.globl main\n\t.type main, "}
                      + std::string{kArm.functionMarker} + "\nmain:\n"
                      + std::string{line} + "\tret\n");
        ASSERT_TRUE(run->module.has_value()) << line << ": " << messages(*run);
        auto const& lir = run->module->lir;
        auto const  blk = lir.funcBlockAt(lir.funcAt(0), 0);
        ASSERT_EQ(lir.blockInstCount(blk), 2u) << line;
        auto const inst = lir.blockInstAt(blk, 0);
        EXPECT_EQ(lir.instOpcode(inst), opOf(*run->target, opcode)) << line;
        EXPECT_EQ(lir.instOperands(inst).size(), 0u)
            << line << ": the selector reached the target as an operand — "
                       "`" << opcode << "` declares maxOperands: 0";
    }
}

// ══ WHY A `%N` OPERAND PLACEHOLDER IS NOT A SIBLING ALT ════════════════════
//
// ★★★ MEASURED, NOT INFERRED — AND IT IS THE REASON THE TEMPLATE SURFACE IS
// STILL OPEN. Carrying an embedded `__asm__` template through this dialect
// needs `%0` to be an OPERAND. The obvious shape is one more arm on
// `attOperand`, and it CANNOT LOAD: `%` is already `RegisterSigil` here (it is
// `TypeSigil` in the arm64 sibling — the same collision one byte over), so a
// `%`+number arm and the existing `%`+name arm share a FIRST token and
// `detectAmbiguousAlternatives` refuses the document. The cursor would
// otherwise "silently take the first branch", which for a placeholder means
// binding it as a register whose name is a digit.
//
// ★★ AND THE REFERENCE COMPILERS AGREE THE TWO SURFACES DIFFER, WHICH IS WHY
// THIS IS A DESIGN FACT AND NOT A LOADER LIMITATION. ✔MEASURED 2026-08-14,
// sources fed as base64 so no shell quoting could alter a byte: `__asm__("xorl
// %eax, %eax")` — the BASIC form, and byte-identical to a `.s` line — compiles
// on gcc 13.3.0 and clang 18.1.3 and emits `%` literally; the EXTENDED form
// `__asm__("xorl %eax,%eax" ::: "eax")` is an ERROR on BOTH ("operand number
// missing after %-letter" / "invalid % escape in inline assembly string"),
// because in an extended template `%`+letter is an operand MODIFIER. So `%eax`
// is a register in a `.s` and a hard error in a template: one token table
// cannot hold both meanings, exactly as `#` (comment here, immediate on arm64)
// could not be held by one table across dialects.
//
// ⇒ this arm exercises the refusal rather than reading it, so the claim in the
// report is a measurement. It goes RED the day the collision is resolved —
// which is precisely when it should be revisited.
void expectPercentNumberAltIsRefused(ShippedDialect const& d,
                                     char const* operandRule,
                                     char const* sigilKind) {
    auto doc = shippedDialectDoc(d.language);
    doc["shapes"]["asmOperandPlaceholderProbe"] = nlohmann::json::parse(
        std::string{R"({"sequence": [")"} + sigilKind + R"(", "IntLiteral"]})");
    doc["shapes"][operandRule]["alt"].push_back("asmOperandPlaceholderProbe");

    auto const run = lowerAsmText(doc, prologue(d), d.target);
    ASSERT_FALSE(run->loadErrors.empty())
        << d.language
        << ": a `" << sigilKind
        << "`+IntLiteral arm beside the existing `" << sigilKind
        << "`+name arm LOADED CLEAN — the FIRST-set overlap check is gone, and "
           "a `%0` placeholder would now silently parse as whichever arm the "
           "cursor reached first";
    const bool named = std::ranges::any_of(
        run->loadErrors, [&](std::string const& e) {
            return e.find("share FIRST token") != std::string::npos
                && e.find(sigilKind) != std::string::npos;
        });
    EXPECT_TRUE(named)
        << d.language << ": the refusal must name the shared FIRST token. Got: "
        << parseMessages(*run);
}

TEST(AsmShippedDialects, X86AttCannotExpressAPercentNumberOperandAsASiblingAlt) {
    expectPercentNumberAltIsRefused(kX86, "attOperand", "RegisterSigil");
}

TEST(AsmShippedDialects, Arm64GasCannotExpressAPercentNumberOperandAsASiblingAlt) {
    expectPercentNumberAltIsRefused(kArm, "armOperand", "TypeSigil");
}

// ══ `%%` LEFT THE GLOBAL TABLE — THE BIDIRECTIONAL HALF ════════════════════
//
// ★★★ WHAT MOVED AND WHY, BECAUSE THIS TEST USED TO ASSERT THE OPPOSITE.
// From 2026-08-14 to 2026-08-15 `%%` was a row of the dialect's GLOBAL `tokens`
// table with NO shape referencing it, and the arm here pinned that a `.s`
// containing `%%0` read the two bytes as ONE token. That was the correct
// interim state — and it stopped being correct the moment `attRegister` grew
// its `PercentEscape Identifier` arm: with the row still global, that arm would
// have made a standalone `.s` ACCEPT `%%eax`, and gas rejects it.
// (D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER — kept on ONE line, because a
// name wrapped across a comment break is a name no grep and no anchor guard
// can find.)
// ⇒ the row now lives in `lexerModes.asm-template.tokens`, so the escaped form
// is reachable from an embedded TEMPLATE and unreachable from a `.s` BY
// CONSTRUCTION rather than by a check that could be forgotten.
//
// ★★ WHAT THIS FILE PINS IS THEREFORE THE **SEPARATION**, NOT THE BINDING. The
// miscompile guard proper — `%%0` must not bind operand 0 where `%0` does —
// needs the template surface and lives in `tests/asm/test_asm_template_to_lir.cpp`
// (`DoublePercentZeroDoesNotBindOperandZero`, plus the row-deletion arm). Here
// the question is the one only a `.s` can ask: does the escape leak into the
// file surface? [[feedback_reference_compilers_are_the_spec]] is bidirectional,
// so accepting what no reference assembler accepts is the same class of defect
// as refusing what they do.
//
// ⚠ THE RED-ON-DISABLE RUNS IN THE ACCEPTING DIRECTION, WHICH IS THE UNUSUAL
// ONE AND THE RIGHT ONE HERE. Deleting a row cannot break a property that
// depends on the row's ABSENCE, so the mutant PUTS THE ROW BACK in the global
// table and asserts the reading changes. A test that mutated in the other
// direction would be asserting nothing at all.
void expectDoublePercentIsNotAFileToken(ShippedDialect const& d) {
    // A directive operand position, so the two bytes are read in a real
    // context rather than at end of input.
    auto const source = std::string{"\t.quad %%0\n"};

    auto const shipped = lowerAsmText(shippedDialectDoc(d.language), source,
                                      d.target);
    ASSERT_TRUE(shipped->loadErrors.empty()) << parseMessages(*shipped);
    auto const shippedMsg = parseMessages(*shipped);
    ASSERT_FALSE(shippedMsg.empty())
        << d.language
        << ": `%%0` was ACCEPTED in a `.s` — no reference assembler spells "
           "`%%`, so this input must be refused in the file surface";
    EXPECT_EQ(shippedMsg.find("%%"), std::string::npos)
        << d.language
        << ": a `.s` read `%%` as ONE token — the escape row has leaked back "
           "into the global table, and `%%eax` would now parse in a file gas "
           "rejects.\n  got: " << shippedMsg;

    // ── RED-ON-DISABLE: put the row back in the GLOBAL table ──
    // ⚠ AND ASSERT IT IS NOT ALREADY THERE FIRST. Without that check this arm
    // would be green on a document that never moved the row, i.e. green for
    // exactly the state it exists to forbid.
    auto mutated = shippedDialectDoc(d.language);
    ASSERT_FALSE(mutated["tokens"].contains("%%"))
        << d.language << ": the shipped document still declares `%%` in its "
           "GLOBAL token table — the arm above is asserting nothing";
    // ⚠ THE SECOND PRECONDITION MOVED WITH THE OWNER (P5c, 2026-08-17 —
    // D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER). It used to
    // read `lexerModes["asm-template"]["tokens"]["%%"]` out of this document; the
    // dialect no longer declares that row — `asm.lang.json` owns the bytes and
    // the loader synthesizes the row from them, joined to the KIND bound here.
    // So the "the escape is not simply gone" check is now the CAPABILITY plus the
    // role BINDING, which are this document's whole share of it.
    ASSERT_TRUE(mutated["assembly"].contains("templateLexerMode"))
        << d.language << ": the shipped document declares no template lexer mode "
           "— there is no template surface for the escape to live in";
    {
        bool bound = false;
        for (auto const& [_, ref] : mutated["languageReferences"].items()) {
            if (!ref.is_object() || !ref.contains("bindTokens")) continue;
            bound = bound || ref["bindTokens"].contains("templateEscape");
        }
        ASSERT_TRUE(bound)
            << d.language << ": the shipped document binds no 'templateEscape' "
               "role, so no escape row is synthesized anywhere — the escape IS "
               "simply gone and the arm above is asserting nothing";
    }
    mutated["tokens"]["%%"] = nlohmann::json::parse(
        R"([{"kind": "PercentEscape"}])");
    auto const leaked = lowerAsmText(mutated, source, d.target);
    ASSERT_TRUE(leaked->loadErrors.empty()) << parseMessages(*leaked);
    auto const leakedMsg = parseMessages(*leaked);
    ASSERT_FALSE(leakedMsg.empty()) << d.language;

    // THE DIFFERENTIAL: with the row global the two bytes are consumed
    // together, so the parser reports a DIFFERENT token from the single-byte
    // sigil it reports without it. A mutation that did not change the reading
    // would mean the scanner is not consulting the table this pin is about.
    // ⚠ THE ASSERTION IS INEQUALITY OF THE TWO READINGS, NOT THE PRESENCE OF
    // `%%` IN THE MESSAGE, AND THAT IS A CORRECTION RATHER THAN A WEAKENING.
    // The stronger-looking form was written first and is WRONG on x86: with the
    // row global, `attRegister`'s escaped arm CONSUMES the `%%` and the parser
    // then complains about the `0` that follows, so the message quotes `0` and
    // never `%%` — the reading did change, and a `%%`-substring test would have
    // called that no change. Comparing the two readings is the claim; quoting a
    // particular byte back was a proxy for it that does not hold on both
    // dialects.
    EXPECT_NE(shippedMsg, leakedMsg)
        << d.language
        << ": adding `%%` to the GLOBAL table did NOT change how a `.s` reads "
           "`%%0` — this pin is asserting nothing.\n  shipped: " << shippedMsg;
}

TEST(AsmShippedDialects, X86AttDoesNotLexDoublePercentInAFile) {
    expectDoublePercentIsNotAFileToken(kX86);
}

TEST(AsmShippedDialects, Arm64GasDoesNotLexDoublePercentInAFile) {
    expectDoublePercentIsNotAFileToken(kArm);
}

// ★★★ THE SHARPEST FORM OF THE SAME CLAIM, AND IT ONLY EXISTS ON x86 BECAUSE
// ONLY x86 HAS A REGISTER SIGIL TO ESCAPE. The generic arm above compares two
// REFUSAL MESSAGES; this one compares a refusal with an ACCEPTANCE, which is
// the actual defect the row's placement prevents: with `%%` in the GLOBAL
// table, `attRegister`'s escaped arm becomes reachable from a `.s` and DSS
// starts accepting `movq %%rcx, %%rax` in a file — input gas rejects outright.
// ✔ [[feedback_reference_compilers_are_the_spec]] is bidirectional, so that is
// a defect of the same weight as refusing what the reference accepts.
TEST(AsmShippedDialects, X86AttWouldAcceptAnEscapedRegisterInAFileIfTheRowLeaked) {
    constexpr std::string_view kEscapedRegisterLine = "\tmovq %%rcx, %%rax\n";

    auto const shipped = lowerAsmText(
        shippedDialectDoc(kX86.language),
        prologue(kX86) + std::string{kEscapedRegisterLine} + "\tret\n",
        kX86.target);
    ASSERT_TRUE(shipped->loadErrors.empty()) << parseMessages(*shipped);
    EXPECT_FALSE(parsedCleanly(*shipped))
        << "a `.s` ACCEPTED `%%rcx, %%rax` — the escaped register form has "
           "leaked out of the template surface, and gas rejects this input";

    auto mutated = shippedDialectDoc(kX86.language);
    ASSERT_FALSE(mutated["tokens"].contains("%%"))
        << "the shipped document declares `%%` globally — the arm above is "
           "asserting nothing";
    mutated["tokens"]["%%"] = nlohmann::json::parse(
        R"([{"kind": "PercentEscape"}])");
    auto const leaked = lowerAsmText(
        mutated, prologue(kX86) + std::string{kEscapedRegisterLine} + "\tret\n",
        kX86.target);
    ASSERT_TRUE(leaked->loadErrors.empty()) << parseMessages(*leaked);
    EXPECT_TRUE(parsedCleanly(*leaked))
        << "putting `%%` back in the GLOBAL table did NOT make the `.s` accept "
           "the escaped register — then the row's PLACEMENT is not what keeps "
           "the two surfaces apart, and this pin asserts nothing: "
        << parseMessages(*leaked);
}

// ★ AND THE BIDIRECTIONAL HALF: `%%` is TEMPLATE vocabulary, so a `.s` file
// containing it must still FAIL — gas has no `%%`. Declaring the lexeme buys
// the lexing property WITHOUT widening what a standalone file is allowed to
// say, because no shape references the kind.
TEST(AsmShippedDialects, NeitherDialectAcceptsDoublePercentInAStandaloneFile) {
    for (auto const& d : {kX86, kArm}) {
        auto const run = lowerShipped(
            d, std::string{"\t.globl main\n\t.type main, "}
                   + std::string{d.functionMarker}
                   + "\nmain:\n\tmov %%eax, %%eax\n\tret\n");
        EXPECT_FALSE(parsedCleanly(*run))
            << d.language
            << " PARSED `%%eax` in a `.s` — no reference assembler accepts it";
    }
}

// ══ CASE FOLDING, THE HALVES ONLY A `.s` CAN WITNESS ═══════════════════════
// (D-ASM-DIALECT-MNEMONIC-MATCH-IS-CASE-SENSITIVE.)
//
// ★★★ THREE THINGS LIVE HERE AND NOWHERE ELSE. The byte-identical mnemonic /
// register / selector pairs are in `test_asm_template_to_lir.cpp`, where bytes
// are the currency; a template legitimately REFUSES directives and has no label
// model, so the DIRECTIVE surface and the three NON-folding surfaces can only be
// witnessed through the standalone walker — and the load-time ambiguity refusal
// can only be witnessed by mutating a shipped document.
//
// ✔MEASURED 2026-08-15, `as` 2.42 and `aarch64-linux-gnu-as` 2.42:
//   FOLDS:     `.TEXT` `.GLOBL` `.TYPE` `.SECTION` `.QUAD` `.DATA` `.BSS`
//              `.ZERO` (and arm64 `.XWORD`) — every one rc=0.
//   DOES NOT:  the `.type` MARKER — `@FUNCTION` / `@Function` / `%FUNCTION` are
//              all rc=1 *"unrecognized symbol type"*, in the very line whose
//              `.TYPE` is accepted;
//              the `.section` NAME operand — `.section .RODATA` is rc=0 and
//              opens a section CALLED `.RODATA`, so a file writing both names
//              gets TWO 8-byte sections in `objdump -h`;
//              SYMBOL / LABEL names — `foo:` and `FOO:` are two defined symbols
//              and `jmp FOO` against `foo:` leaves `FOO` `*UND*`.

// ★★ THE WHOLE DIRECTIVE VOCABULARY IN UPPERCASE, ASSERTED BYTE-IDENTICAL TO
// ITS LOWERCASE TWIN. ⚠ THE MARKER STAYS LOWERCASE IN BOTH ARMS — that is not
// an oversight, it is the measurement: gas folds `.TYPE` and refuses
// `@FUNCTION`, so a test that uppercased the marker too would be asserting
// something the reference rejects. The control for that is the next test.
TEST(AsmShippedDialects, DirectiveSpellingsFoldOnBothDialects) {
    for (auto const& d : {kX86, kArm}) {
        std::string const marker{d.functionMarker};
        auto const lowerSrc =
            "\t.text\n\t.globl main\n\t.type main, " + marker
            + "\nmain:\n\tret\n"
              "\t.section .rodata\nro:\n\t.quad 7\n"
              "\t.data\nrw:\n\t.zero 4\n";
        auto const upperSrc =
            "\t.TEXT\n\t.GLOBL main\n\t.TYPE main, " + marker
            + "\nmain:\n\tret\n"
              "\t.SECTION .rodata\nro:\n\t.QUAD 7\n"
              "\t.DATA\nrw:\n\t.ZERO 4\n";

        auto const lo = lowerShipped(d, lowerSrc);
        ASSERT_TRUE(parsedCleanly(*lo)) << d.language << ": " << parseMessages(*lo);
        ASSERT_TRUE(lo->module.has_value())
            << d.language << ": the lowercase control was refused: "
            << messages(*lo);

        auto const up = lowerShipped(d, upperSrc);
        ASSERT_TRUE(parsedCleanly(*up)) << d.language << ": " << parseMessages(*up);
        ASSERT_TRUE(up->module.has_value())
            << d.language
            << ": an all-uppercase directive vocabulary was REFUSED, and gas "
               "accepts every one of these spellings: " << messages(*up);

        // ★ THE ASSERTION IS THE EMITTED DATA, NOT "IT LOWERED". A directive
        // that folded to the WRONG row — `.DATA` reaching the rodata row, say —
        // would lower perfectly and put the bytes in the wrong section.
        auto const& a = lo->module->dataItems;
        auto const& b = up->module->dataItems;
        ASSERT_EQ(a.size(), b.size()) << d.language;
        ASSERT_EQ(a.size(), 2u) << d.language << ": expected `ro` and `rw`";
        for (std::size_t i = 0; i < a.size(); ++i) {
            EXPECT_EQ(a[i].section, b[i].section)
                << d.language << ": data item " << i
                << " landed in a different section under the uppercase "
                   "directives — the fold reached the wrong row";
            EXPECT_EQ(a[i].bytes, b[i].bytes) << d.language << ": item " << i;
            EXPECT_EQ(a[i].reservedSize, b[i].reservedSize)
                << d.language << ": item " << i;
        }
        EXPECT_EQ(a[0].section, DataSectionKind::Rodata) << d.language;
        EXPECT_EQ(a[1].section, DataSectionKind::Data) << d.language;

        // The exported entry survived `.GLOBL` + `.TYPE`.
        EXPECT_TRUE(up->module->userEntrySymbol.has_value())
            << d.language
            << ": `.GLOBL`/`.TYPE` did not export the entry — the fold reached "
               "the spelling but lost the verb";
    }
}

// ⛔ CONTROL 1 — THE `.type` MARKER IS NOT A FOLDED SURFACE. gas accepts `.TYPE`
// and refuses `@FUNCTION` in the SAME line, so DSS must too. ⚠ This is the seam
// most likely to be "fixed" by someone extending the fold one comparison
// further; the refusal is what stops that.
TEST(AsmShippedDialects, TheFunctionEntryMarkerStaysCaseSensitive) {
    for (auto const& d : {kX86, kArm}) {
        std::string marker{d.functionMarker};
        std::string shouted = marker;
        for (auto& c : shouted) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        ASSERT_NE(shouted, marker) << d.language << ": the marker has no "
                                      "letters, so this control is vacuous";

        // The lowercase control must still mark the function.
        auto const ok = lowerShipped(
            d, "\t.globl main\n\t.type main, " + marker + "\nmain:\n\tret\n");
        ASSERT_TRUE(ok->module.has_value())
            << d.language << ": " << messages(*ok);
        ASSERT_TRUE(ok->module->userEntrySymbol.has_value()) << d.language;

        auto const bad = lowerShipped(
            d, "\t.globl main\n\t.type main, " + shouted + "\nmain:\n\tret\n");
        ASSERT_TRUE(parsedCleanly(*bad))
            << d.language << ": the shouted marker must still PARSE — a lex "
                             "refusal would prove nothing: "
            << parseMessages(*bad);
        // gas: rc=1, "unrecognized symbol type". DSS: the marker did not match,
        // so nothing marked a function and the file has a label with no
        // function-entry directive — a refusal either way, never a silent mark.
        EXPECT_FALSE(bad->module.has_value())
            << d.language << ": `" << shouted
            << "` was accepted as a function-entry marker — the fold has "
               "reached the MARKER, which gas rejects outright";
    }
}

// ⛔ CONTROL 2 — THE `.section` NAME OPERAND IS NOT A FOLDED SURFACE. ✔MEASURED:
// `.section .RODATA` is rc=0 in gas and opens a DIFFERENT section. DSS models
// only the section kinds it declares, so it refuses the unmodelled name — what
// it must never do is silently route it into `.rodata`, which is the one
// outcome that differs from the reference with no diagnostic.
TEST(AsmShippedDialects, TheSectionNameOperandStaysCaseSensitive) {
    for (auto const& d : {kX86, kArm}) {
        auto const ok = lowerShipped(
            d, prologue(d) + "\t.section .rodata\nro:\n\t.quad 7\n");
        ASSERT_TRUE(ok->module.has_value())
            << d.language << ": the lowercase control was refused: "
            << messages(*ok);
        ASSERT_EQ(ok->module->dataItems.size(), 1u) << d.language;
        ASSERT_EQ(ok->module->dataItems[0].section, DataSectionKind::Rodata)
            << d.language;

        auto const bad = lowerShipped(
            d, prologue(d) + "\t.section .RODATA\nro:\n\t.quad 7\n");
        ASSERT_TRUE(parsedCleanly(*bad))
            << d.language << ": " << parseMessages(*bad);
        EXPECT_FALSE(bad->module.has_value())
            << d.language
            << ": `.section .RODATA` resolved to the rodata row — gas opens a "
               "SEPARATE section called `.RODATA` for that text, so folding "
               "the name puts these bytes somewhere the reference does not";
    }
}

// ⛔ CONTROL 3 — SYMBOL NAMES STAY CASE-SENSITIVE, and this is the control the
// anchor row calls for by name: folding a label MERGES TWO REAL DEFINITIONS,
// which is a miscompile and not a conformance fix.
TEST(AsmShippedDialects, SymbolNamesStayCaseSensitive) {
    for (auto const& d : {kX86, kArm}) {
        // TWO data labels differing only in case, with DIFFERENT payloads. If
        // names folded, they would be one symbol and one of the two values
        // would be lost.
        auto const run = lowerShipped(
            d, prologue(d)
                   + "\t.section .rodata\nfoo:\n\t.quad 1\nFOO:\n\t.quad 2\n");
        ASSERT_TRUE(parsedCleanly(*run)) << d.language << ": "
                                         << parseMessages(*run);
        ASSERT_TRUE(run->module.has_value())
            << d.language << ": `foo:` beside `FOO:` was refused — gas defines "
                             "both: " << messages(*run);
        auto const& items = run->module->dataItems;
        ASSERT_EQ(items.size(), 2u)
            << d.language
            << ": `foo:` and `FOO:` produced " << items.size()
            << " data item(s) — they have been merged into one symbol";
        EXPECT_NE(items[0].symbol.v, items[1].symbol.v)
            << d.language << ": the two labels share one SymbolId";
        EXPECT_EQ(items[0].bytes,
                  (std::vector<std::uint8_t>{1, 0, 0, 0, 0, 0, 0, 0}))
            << d.language;
        EXPECT_EQ(items[1].bytes,
                  (std::vector<std::uint8_t>{2, 0, 0, 0, 0, 0, 0, 0}))
            << d.language;
    }
}

// ★★ THE SAME CONTROL AT THE ENTRY-ELECTION TIER, WHICH IS WHERE FOLDING A
// SYMBOL WOULD DO THE MOST DAMAGE. `entryLabels: ["main"]` is dialect config
// matched against a PROGRAM label, so it is a symbol comparison and must stay
// exact: if it folded, a file whose only exported function is `MAIN` would be
// silently elected as the program entry.
TEST(AsmShippedDialects, TheEntryLabelMatchStaysCaseSensitive) {
    for (auto const& d : {kX86, kArm}) {
        std::string const marker{d.functionMarker};
        auto const lower = lowerShipped(
            d, "\t.globl main\n\t.type main, " + marker + "\nmain:\n\tret\n");
        ASSERT_TRUE(lower->module.has_value())
            << d.language << ": " << messages(*lower);
        EXPECT_TRUE(lower->module->userEntrySymbol.has_value())
            << d.language << ": `main` must be elected as the entry";

        auto const upper = lowerShipped(
            d, "\t.globl MAIN\n\t.type MAIN, " + marker + "\nMAIN:\n\tret\n");
        ASSERT_TRUE(parsedCleanly(*upper)) << d.language << ": "
                                           << parseMessages(*upper);
        ASSERT_TRUE(upper->module.has_value())
            << d.language << ": a function called `MAIN` is perfectly legal "
                             "assembly and must lower: " << messages(*upper);
        EXPECT_FALSE(upper->module->userEntrySymbol.has_value())
            << d.language
            << ": `MAIN` was elected as the program entry — the entry-label "
               "match is folding, and a symbol match must not";
    }
}

// ══ THE LOAD-TIME AMBIGUITY REFUSAL, UNDER FOLDING ═════════════════════════
//
// ★★★ THE HALF OF THIS FIX THAT IS EASIEST TO FORGET AND WORST TO OMIT. With
// the engine folding and the LOADER still deduplicating on raw bytes, a `mov`
// row and a `MOV` row would both load perfectly clean and both match
// `mov x0, x1` — re-creating the §4.7.1 ambiguity through the door the fold
// itself opened. It is the same defect the bare-`mrs`-beside-selector test
// above refuses, arriving by a different route.
TEST(AsmShippedDialects, TwoInstructionRowsDifferingOnlyInCaseAreRefused) {
    for (auto const& d : {kX86, kArm}) {
        auto doc = shippedDialectDoc(d.language);
        // Clone a row that really is in the shipped table and SHOUT its
        // spelling — so the pin cannot pass by naming a mnemonic that is not
        // there. `ret` ships in both dialects.
        nlohmann::json shouted;
        for (auto const& row : doc["assembly"]["instructions"]) {
            if (row.value("spelling", std::string{}) != "ret") continue;
            shouted = row;
            shouted["spelling"] = "RET";
            break;
        }
        ASSERT_FALSE(shouted.is_null())
            << d.language << ": no shipped `ret` row to clone — this pin would "
                             "be asserting nothing";
        doc["assembly"]["instructions"].push_back(shouted);

        auto const run = lowerAsmText(doc, prologue(d), d.target);
        ASSERT_FALSE(run->loadErrors.empty())
            << d.language
            << ": a `RET` row beside `ret` LOADED CLEAN under 'spellingCase': "
               "'asciiFolded' — both rows would match the same line, which is "
               "the ambiguity the duplicate refusal exists to prevent";
        auto const why = parseMessages(*run);
        EXPECT_NE(why.find("RET"), std::string::npos)
            << d.language << ": the refusal must name the row: " << why;
        // ★ AND IT MUST SAY *WHY* TWO VISIBLY DIFFERENT STRINGS ARE ONE
        // SPELLING. "'RET' has two rows" beside a table containing exactly one
        // `RET` reads as a compiler bug; naming the policy is the diagnosis.
        EXPECT_NE(why.find("spellingCase"), std::string::npos)
            << d.language
            << ": the refusal must name the policy that made them one: " << why;
    }
}

TEST(AsmShippedDialects, TwoDirectiveRowsDifferingOnlyInCaseAreRefused) {
    for (auto const& d : {kX86, kArm}) {
        auto doc = shippedDialectDoc(d.language);
        nlohmann::json shouted;
        for (auto const& row : doc["assembly"]["directives"]) {
            if (row.value("spelling", std::string{}) != "globl") continue;
            shouted = row;
            shouted["spelling"] = "GLOBL";
            break;
        }
        ASSERT_FALSE(shouted.is_null())
            << d.language << ": no shipped `globl` row to clone";
        doc["assembly"]["directives"].push_back(shouted);

        auto const run = lowerAsmText(doc, prologue(d), d.target);
        ASSERT_FALSE(run->loadErrors.empty())
            << d.language
            << ": a `GLOBL` row beside `globl` LOADED CLEAN — under folding the "
               "later row can never apply, which is the dead config this "
               "refusal names";
        EXPECT_NE(parseMessages(*run).find("GLOBL"), std::string::npos)
            << parseMessages(*run);
    }
}

// ★ AND THE COMPLEMENT, so the two refusals above are not just "any second row
// is refused": under `spellingCase: sensitive` the SAME documents load clean,
// because `ret` and `RET` are then two different spellings. This is what makes
// the pair a statement about the POLICY rather than about duplicate rows.
TEST(AsmShippedDialects, UnderSensitiveCaseTheShoutedRowsAreTwoSpellings) {
    for (auto const& d : {kX86, kArm}) {
        auto doc = shippedDialectDoc(d.language);
        doc["assembly"]["spellingCase"] = "sensitive";
        nlohmann::json shouted;
        for (auto const& row : doc["assembly"]["instructions"]) {
            if (row.value("spelling", std::string{}) != "ret") continue;
            shouted = row;
            shouted["spelling"] = "RET";
            break;
        }
        ASSERT_FALSE(shouted.is_null()) << d.language;
        doc["assembly"]["instructions"].push_back(shouted);

        auto const run = lowerAsmText(doc, prologue(d), d.target);
        EXPECT_TRUE(run->loadErrors.empty())
            << d.language
            << ": `ret` and `RET` were refused as ONE spelling even though this "
               "document declares 'sensitive' — the duplicate check is folding "
               "unconditionally rather than under the declared policy: "
            << parseMessages(*run);
    }
}

} // namespace
