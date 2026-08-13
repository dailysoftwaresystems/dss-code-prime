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
#include "asm/asm.hpp"
#include "core/types/section_kind.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
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

} // namespace
