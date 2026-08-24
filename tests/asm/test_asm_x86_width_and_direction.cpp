// The byte tier for the three shapes gas accepts and DSS used to refuse.
// Anchors:
//   D-ASM-X86-NO-16BIT-IMMEDIATE-SLOT
//   D-ASM-X86-WIDTH-EXTENDING-MOVES-UNSPELLABLE
//   D-ASM-X86-CMP-AGAINST-MEMORY-DIRECTION-IS-UNELECTABLE
//
// ★★★ WHY BYTES AND NOT "DOES IT COMPILE". All three subjects are shapes whose
// WRONG answer also compiles, assembles and links:
//   * a 16-bit immediate wired to the 4-byte slot emits two extra bytes and
//     corrupts everything after it — with no diagnostic anywhere;
//   * `movsbl %cl, %ecx` and `movsbq %cl, %rcx` differ in ONE prefix byte and
//     in the VALUE they produce for any negative input;
//   * `cmpq %r14, mem` and `cmpq mem, %r14` differ in ONE opcode byte and mean
//     opposite operand orders, so a lost direction is a wrong flag result on
//     every signed comparison.
// The corpus example beside this file proves the results survive the linker and
// the loader; it cannot say WHICH byte was wrong. This file drives the SHIPPED
// dialect and the SHIPPED target UNMUTATED and asserts the exact bytes.
//
// ★★★ EVERY EXPECTATION IS GNU as 2.42's OWN OUTPUT unless the comment beside
// it says otherwise, measured one spelling at a time (`as -o t.o t.s;
// objdump -d t.o`). The displacement is 4096 and the immediate 300 for the same
// reason the sibling file states: both are outside the short-form windows, so
// gas is forced onto the disp32 / imm32 long forms the walker emits.
//
// ⚠ THE PLACES DSS IS **NOT** BYTE-IDENTICAL TO gas ARE LISTED, NOT HIDDEN, and
// each was re-decoded with objdump to confirm the two byte strings name the
// SAME instruction:
//   * gas contracts to x86's short forms where they exist (`movw $42, %cx`
//     becomes `66 B9 iw`, `addw $300, %ax` becomes `66 05 iw`); DSS emits the
//     general ModR/M form its declared variant names.
//   * every BYTE-register form carries `forceRexPrefix`, so DSS emits a bare
//     REX (0x40) that gas omits when no extended register is named. That is a
//     CORRECTNESS requirement rather than a size choice: without a REX prefix,
//     byte-register encodings 4..7 name the legacy high bytes ah/ch/dh/bh
//     instead of spl/bpl/sil/dil — a different physical register, silently.

#include "asm/asm.hpp"
#include "asm_text_fixture.hpp"
#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support::asm_text;

namespace {

[[nodiscard]] std::string hex(std::span<std::uint8_t const> b) {
    std::string out;
    for (auto v : b) out += std::format("{:02x}", v);
    return out;
}

[[nodiscard]] std::vector<std::uint8_t>
bytesOfWith(std::shared_ptr<TargetSchema> target, std::string const& line) {
    auto const doc = shippedDialectDoc("asm-x86_64-att");
    auto const src = std::string{"\t.globl main\n\t.type main, @function\n"
                                 "main:\n"} + line + "\tret\n";
    auto const run = lowerAsmTextWithTarget(doc, src, std::move(target));
    EXPECT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    if (!run->module.has_value()) return {};
    DiagnosticReporter     asmRep;
    std::vector<MirInstId> lirToMir(run->module->lir.instCount());
    auto const mod = assemble(run->module->lir, *run->target, lirToMir, asmRep);
    if (mod.functions.size() != 1) return {};
    return mod.functions[0].bytes;
}

[[nodiscard]] std::shared_ptr<TargetSchema> shippedX86() {
    auto t = TargetSchema::loadShipped("x86_64");
    EXPECT_TRUE(t.has_value());
    return t.has_value() ? *t : nullptr;
}

// The subject line's OWN bytes, in hex — the trailing `ret` differenced away by
// assembling the same function WITHOUT the line. The subtraction is what makes
// the pin EXACT rather than a prefix match: a stray extra instruction would
// still fail it.
[[nodiscard]] std::string hexOfWith(std::shared_ptr<TargetSchema> target,
                                    std::string const&            line) {
    auto const withIt  = bytesOfWith(target, "\t" + line + "\n");
    auto const without = bytesOfWith(target, "");
    if (withIt.size() <= without.size()) return "<NOTHING-EMITTED>";
    if (!std::equal(without.begin(), without.end(),
                    withIt.begin() + static_cast<std::ptrdiff_t>(
                        withIt.size() - without.size()))) {
        return "<TERMINATOR-CHANGED>";
    }
    return hex(std::span<std::uint8_t const>{
        withIt.data(), withIt.size() - without.size()});
}

[[nodiscard]] std::string soleHex(std::string const& line) {
    return hexOfWith(shippedX86(), line);
}

struct Refusal { bool refused; std::string why; };

// How many errors the ENCODER reports for `line`. ⚠ SEPARATE FROM `refusalOf`
// ON PURPOSE: the operand-shape refusals live in the text->LIR lowering, but an
// immediate that does not FIT its slot is caught by the walker, one tier later.
// A test that only asked `refusalOf` would read "the lowering accepted it" as
// "nothing refused it" and pass while a truncated immediate shipped.
[[nodiscard]] std::size_t assembleErrorsOf(std::string const& line) {
    auto const doc = shippedDialectDoc("asm-x86_64-att");
    auto const src = std::string{"\t.globl main\n\t.type main, @function\n"
                                 "main:\n\t"} + line + "\n\tret\n";
    auto const run = lowerAsmTextWithTarget(doc, src, shippedX86());
    EXPECT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    if (!run->module.has_value()) return 1;  // refused a tier earlier
    DiagnosticReporter     asmRep;
    std::vector<MirInstId> lirToMir(run->module->lir.instCount());
    (void)assemble(run->module->lir, *run->target, lirToMir, asmRep);
    return asmRep.errorCount();
}

[[nodiscard]] Refusal refusalOf(std::string const& line) {
    auto const doc = shippedDialectDoc("asm-x86_64-att");
    auto const src = std::string{"\t.globl main\n\t.type main, @function\n"
                                 "main:\n\t"} + line + "\n\tret\n";
    auto const run = lowerAsmTextWithTarget(doc, src, shippedX86());
    EXPECT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    return Refusal{!run->module.has_value(), messages(*run)};
}

// Navigate to one opcode's variant list, failing loudly rather than silently
// doing nothing when the opcode is absent.
[[nodiscard]] nlohmann::json* variantsOf(nlohmann::json& doc,
                                         std::string_view mnemonic) {
    for (auto& op : doc.at("opcodes")) {
        if (op.value("mnemonic", std::string{}) == mnemonic) {
            return &op.at("encoding").at("variants");
        }
    }
    return nullptr;
}

} // namespace

// ══ SUBJECT 1 — `cmp` AGAINST MEMORY, BOTH DIRECTIONS ══════════════════════

TEST(AsmX86WidthAndDirection, CompareAgainstMemoryEncodesEachDirectionOnce) {
    // ★★★ THE PAIR IS THE ASSERTION. Each expectation alone would pass under a
    // mechanism that had lost the direction and encoded both spellings the same
    // way; only the pair sees it. 0x39 is `cmp mem, reg` and 0x3B is
    // `cmp reg, mem` — the x86 "direction bit" — and the two spellings build
    // the BYTE-IDENTICAL LIR operand list `[reg, reg, membase, memoffset]`.
    struct Row { char const* written; char const* bytes; };
    static constexpr Row kRows[] = {
        {"cmpq %r14, 4096(%r15)",  "4d39b700100000"},
        {"cmpq 4096(%r15), %r14",  "4d3bb700100000"},
        {"cmpl %r14d, 4096(%r15)", "4539b700100000"},
        {"cmpl 4096(%r15), %r14d", "453bb700100000"},
        {"cmpw %ax, 4096(%r15)",   "6641398700100000"},
        {"cmpw 4096(%r15), %ax",   "66413b8700100000"},
    };
    for (auto const& r : kRows) {
        EXPECT_EQ(soleHex(r.written), r.bytes) << r.written;
    }
    // The byte forms carry the deliberate bare REX; gas emits `41388700100000`
    // and `413a8700100000` for these two (same instruction, no REX needed for
    // %al). ⚠ The REX here is 0x41 in BOTH: REX.B for %r15 is required, and
    // forceRexPrefix adds no further bit.
    EXPECT_EQ(soleHex("cmpb %al, 4096(%r15)"), "41388700100000");
    EXPECT_EQ(soleHex("cmpb 4096(%r15), %al"), "413a8700100000");

    for (auto const& pair : {std::pair{"cmpq %r14, 4096(%r15)",
                                       "cmpq 4096(%r15), %r14"},
                             std::pair{"cmpw %ax, 4096(%r15)",
                                       "cmpw 4096(%r15), %ax"}}) {
        EXPECT_NE(soleHex(pair.first), soleHex(pair.second))
            << "the two directions of " << pair.first
            << " must not encode identically";
    }
}

// ══ SUBJECT 2 — THE 2-BYTE IMMEDIATE SLOT AND THE FAMILY IT UNBLOCKED ══════

TEST(AsmX86WidthAndDirection, SixteenBitImmediatesEmitExactlyTwoBytes) {
    // ★ THE VALUE 300 IS THE ASSERTION, not decoration: it does not fit one
    // byte, so an `imm8` slot could only encode it by truncating (and would
    // still assemble); and a 4-byte slot would append two extra zero bytes,
    // shifting everything after it. Only a 2-byte slot gives these lengths.
    EXPECT_EQ(soleHex("movw $300, %cx"), "66c7c12c01");
    EXPECT_EQ(soleHex("addw $300, %cx"), "6681c12c01");
    EXPECT_EQ(soleHex("subw $300, %cx"), "6681e92c01");
    EXPECT_EQ(soleHex("andw $300, %cx"), "6681e12c01");
    EXPECT_EQ(soleHex("orw $300, %cx"),  "6681c92c01");
    EXPECT_EQ(soleHex("xorw $300, %cx"), "6681f12c01");
    EXPECT_EQ(soleHex("cmpw $300, %cx"), "6681f92c01");
    // Memory destinations — byte-identical to gas.
    EXPECT_EQ(soleHex("movw $42, 4096(%r15)"),  "6641c787001000002a00");
    EXPECT_EQ(soleHex("addw $300, 4096(%r15)"), "66418187001000002c01");
    EXPECT_EQ(soleHex("subw $300, 4096(%r15)"), "664181af001000002c01");
    EXPECT_EQ(soleHex("cmpw $300, 4096(%r15)"), "664181bf001000002c01");
}

TEST(AsmX86WidthAndDirection, SixteenBitImmediateTakesTheWholeSignedAndUnsignedWindow) {
    // ★★ THE SLOT ACCEPTS THE UNION OF BOTH READINGS OF THE SAME 16 BITS, and
    // THAT much is a MEASURED conformance fact rather than a convenience: ✔GNU
    // as 2.42 assembles `movw $-1, %cx` AND `movw $65535, %cx` to the IDENTICAL
    // `66 b9 ff ff`. A range admitting only one of them would refuse input the
    // reference takes.
    EXPECT_EQ(soleHex("movw $-1, %cx"), "66c7c1ffff");
    EXPECT_EQ(soleHex("movw $65535, %cx"), "66c7c1ffff");
    // ⚠⚠ THE INTERIOR OF THE WINDOW IS CONFORMANCE; ITS EDGE IS NOT, AND AN
    // EARLIER DRAFT OF THIS COMMENT PRESENTED BOTH AS ONE MEASURED FACT. What
    // was measured there was DSS's own behaviour. ✔RE-MEASURED against GNU as
    // 2.42, one spelling at a time, and the reference DISAGREES at the edge:
    //   `mov $0x10000, %cx` → gas rc=0, `66 b9 00 00`, warning "0x10000
    //                         shortened to 0x0"
    //   `mov $-32769, %cx`  → gas rc=0, `66 b9 ff 7f`, NO DIAGNOSTIC AT ALL
    // DSS refuses both with `A_ImmediateOperandOutOfRange`. So the assertions
    // below pin a DIVERGENCE, not an agreement, and they are written to keep
    // pinning DSS's current behaviour rather than to bless it.
    // ★ WHY THE DIVERGENCE IS NOT RESOLVED HERE: the two standing rules point
    // opposite ways. One working reference makes an input REQUIRED (bar §A.3b);
    // fail-loud forbids silently keeping the low half of a value that has no
    // 2-byte representation, which is exactly what gas does — it drops 0x8000
    // of magnitude on the negative case without saying anything. That fork is
    // an OPERATOR decision, filed as
    // [[D-ASM-X86-IMMEDIATE-WINDOW-REFUSES-WHAT-GAS-TRUNCATES]] (§B, OPEN). ⛔ Do
    // not close it by widening the window — the row says so, and widening is
    // the arm that adopts gas's silent truncation.
    // ★ The range check lives in the ENCODER, not the lowering — `refusalOf`
    // would report "not refused" here and the pin would assert nothing.
    EXPECT_EQ(refusalOf("movw $65536, %cx").refused, false)
        << "the LOWERING accepts it — the range check is the encoder's";
    EXPECT_GT(assembleErrorsOf("movw $65536, %cx"), 0u)
        << "DSS refuses an immediate wider than the 2-byte slot rather than "
           "silently keeping its low half (gas truncates it to $0 with a "
           "warning). The fork is an OPEN operator decision -- see "
           "D-ASM-X86-IMMEDIATE-WINDOW-REFUSES-WHAT-GAS-TRUNCATES; "
           "if this red is deliberate, change the ROW first";
    EXPECT_GT(assembleErrorsOf("movw $-32769, %cx"), 0u)
        << "and the negative end too, where gas truncates to $0x7fff with NO "
           "diagnostic at all — same open row, same instruction";
}

TEST(AsmX86WidthAndDirection, TheEightAndSixteenBitAluFamilyIsComplete) {
    // §A.2 — the slot did not land alone. Every shape the 32/64-bit rows carry
    // exists at both narrow widths: register-direct, register-immediate,
    // memory SOURCE and memory DESTINATION.
    // Width 16, register-direct — byte-identical to gas.
    EXPECT_EQ(soleHex("addw %ax, %cx"), "6601c1");
    EXPECT_EQ(soleHex("subw %ax, %cx"), "6629c1");
    EXPECT_EQ(soleHex("andw %ax, %cx"), "6621c1");
    EXPECT_EQ(soleHex("orw %ax, %cx"),  "6609c1");
    EXPECT_EQ(soleHex("xorw %ax, %cx"), "6631c1");
    EXPECT_EQ(soleHex("cmpw %ax, %cx"), "6639c1");
    EXPECT_EQ(soleHex("notw %cx"),      "66f7d1");
    EXPECT_EQ(soleHex("negw %cx"),      "66f7d9");
    EXPECT_EQ(soleHex("imulw %ax, %cx"), "660fafc8");
    // Width 8, register-direct — gas omits the bare REX (`00c1`, `28c1`, …);
    // see the file header for why DSS emits it.
    EXPECT_EQ(soleHex("addb %al, %cl"), "4000c1");
    EXPECT_EQ(soleHex("subb %al, %cl"), "4028c1");
    EXPECT_EQ(soleHex("andb %al, %cl"), "4020c1");
    EXPECT_EQ(soleHex("orb %al, %cl"),  "4008c1");
    EXPECT_EQ(soleHex("xorb %al, %cl"), "4030c1");
    EXPECT_EQ(soleHex("cmpb %al, %cl"), "4038c1");
    EXPECT_EQ(soleHex("notb %cl"),      "40f6d1");
    EXPECT_EQ(soleHex("negb %cl"),      "40f6d9");
    EXPECT_EQ(soleHex("addb $5, %cl"),  "4080c105");
    // Memory source and memory destination at both widths — byte-identical.
    EXPECT_EQ(soleHex("addw 4096(%r15), %ax"), "6641038700100000");
    EXPECT_EQ(soleHex("addb 4096(%r15), %al"), "41028700100000");
    EXPECT_EQ(soleHex("addw %ax, 4096(%r15)"), "6641018700100000");
    EXPECT_EQ(soleHex("addb %al, 4096(%r15)"), "41008700100000");
    EXPECT_EQ(soleHex("addb $5, 4096(%r15)"),  "4180870010000005");
    EXPECT_EQ(soleHex("notw 4096(%r15)"),      "6641f79700100000");
    EXPECT_EQ(soleHex("negb 4096(%r15)"),      "41f69f00100000");
    EXPECT_EQ(soleHex("imulw 4096(%r15), %ax"), "66410faf8700100000");
}

TEST(AsmX86WidthAndDirection, ImmediateIntoMemoryHasItsOwnOpcodeAtEveryWidth) {
    // `store` takes a REGISTER source, so `mov $imm, mem` had no opcode at all
    // before `mov_mem`. Byte-identical to gas at all four widths.
    EXPECT_EQ(soleHex("movq $5, 4096(%r15)"),  "49c7870010000005000000");
    EXPECT_EQ(soleHex("movl $5, 4096(%r15)"),  "41c7870010000005000000");
    EXPECT_EQ(soleHex("movw $42, 4096(%r15)"), "6641c787001000002a00");
    EXPECT_EQ(soleHex("movb $42, 4096(%r15)"), "41c687001000002a");
}

TEST(AsmX86WidthAndDirection, ThreeOperandImulWithAnImmediateEncodes) {
    // D-ASM-X86-IMUL-IMMEDIATE-FORM-UNDECLARED, found while declaring the
    // narrow ALU family: `mul` declared only register and memory-source forms,
    // so every `imul $imm, …` spelling was refused at every width.
    EXPECT_EQ(soleHex("imulq $300, %rcx, %rdx"),  "4869d12c010000");
    EXPECT_EQ(soleHex("imull $300, %ecx, %edx"),  "69d12c010000");
    EXPECT_EQ(soleHex("imulw $300, %cx, %dx"),    "6669d12c01");
    // ★ The two-operand SHORTHAND reaches the same variant family through the
    // engine's existing two-address retry, which prepends the destination.
    EXPECT_EQ(soleHex("imulq $300, %rax"), "4869c02c010000");
    EXPECT_EQ(soleHex("imull $300, %ecx"), "69c92c010000");
    EXPECT_EQ(soleHex("imulw $300, %cx"),  "6669c92c01");
}

// ══ SUBJECT 3 — THE WIDTH-EXTENDING MOVES ══════════════════════════════════

TEST(AsmX86WidthAndDirection, EveryExtendingMoveEncodesItsOwnDestinationWidth) {
    // ★★★ THE 32-BIT AND 64-BIT DESTINATIONS ARE DIFFERENT INSTRUCTIONS, not
    // different lengths. `movsbl %cl, %ecx` sign-extends to 32 bits and then
    // ZEROES bits 63:32; `movsbq %cl, %rcx` sign-extends to 64. For any
    // negative byte those are different VALUES, so encoding one as the other is
    // a wrong answer with no diagnostic — which is exactly why the destination
    // width picks the OPCODE rather than being dropped.
    // Byte-identical to gas except where the bare REX is noted.
    EXPECT_EQ(soleHex("movzbl %cl, %ecx"), "400fb6c9");  // gas: 0fb6c9
    EXPECT_EQ(soleHex("movzwl %cx, %ecx"), "0fb7c9");
    EXPECT_EQ(soleHex("movsbl %cl, %ecx"), "400fbec9");  // gas: 0fbec9
    EXPECT_EQ(soleHex("movswl %cx, %ecx"), "0fbfc9");
    EXPECT_EQ(soleHex("movzbq %cl, %rcx"), "480fb6c9");
    EXPECT_EQ(soleHex("movzwq %cx, %rcx"), "480fb7c9");
    EXPECT_EQ(soleHex("movsbq %cl, %rcx"), "480fbec9");
    EXPECT_EQ(soleHex("movswq %cx, %rcx"), "480fbfc9");
    EXPECT_EQ(soleHex("movslq %ecx, %rcx"), "4863c9");
    EXPECT_EQ(soleHex("movzbw %cl, %cx"), "66400fb6c9");  // gas: 660fb6c9
    EXPECT_EQ(soleHex("movsbw %cl, %cx"), "66400fbec9");  // gas: 660fbec9
    // The four pairs that would collapse if the destination width were lost.
    EXPECT_NE(soleHex("movsbl %cl, %ecx"), soleHex("movsbq %cl, %rcx"));
    EXPECT_NE(soleHex("movswl %cx, %ecx"), soleHex("movswq %cx, %rcx"));
    EXPECT_NE(soleHex("movzbl %cl, %ecx"), soleHex("movzbq %cl, %rcx"));
    EXPECT_NE(soleHex("movzbw %cl, %cx"),  soleHex("movzbl %cl, %ecx"));
}

TEST(AsmX86WidthAndDirection, ExtendingMovesTakeAMemorySource) {
    // gcc emits these constantly; all byte-identical to gas.
    EXPECT_EQ(soleHex("movzbl 4096(%r15), %ecx"), "410fb68f00100000");
    EXPECT_EQ(soleHex("movsbl 4096(%r15), %ecx"), "410fbe8f00100000");
    EXPECT_EQ(soleHex("movslq 4096(%r15), %rcx"), "49638f00100000");
    EXPECT_EQ(soleHex("movzbq 4096(%r15), %rcx"), "490fb68f00100000");
    EXPECT_EQ(soleHex("movswq 4096(%r15), %rcx"), "490fbf8f00100000");
}

// ══ WHAT STAYS REFUSED, AND WITH THE TRUE REASON ═══════════════════════════

TEST(AsmX86WidthAndDirection, TheWrongDestinationWidthIsStillRefused) {
    // ★★★ THE CONFORMANCE RUNS BOTH WAYS, AND THIS IS THE HALF THAT PROVES THE
    // TWO-WIDTH MECHANISM DID NOT SIMPLY DELETE THE AGREEMENT CHECK. ✔gas 2.42
    // REJECTS `movzbl %cl, %rcx` (its destination is a `long`, not a `quad`)
    // and REJECTS `movl %rax, %ecx`; accepting either would be the same defect
    // as refusing a spelling gas takes.
    for (auto const& line : {std::string{"movzbl %cl, %rcx"},
                             std::string{"movswl %cx, %rcx"},
                             std::string{"movsbq %cl, %ecx"}}) {
        auto const r = refusalOf(line);
        EXPECT_TRUE(r.refused) << line << " must not lower: " << r.why;
        EXPECT_NE(r.why.find("widens a"), std::string::npos)
            << line << " must name the two widths that disagree: " << r.why;
    }
    // The SINGLE-width guard is untouched — this is the message that guard
    // gives, and it must not have been widened into the two-width one.
    auto const single = refusalOf("movl %rax, %ecx");
    EXPECT_TRUE(single.refused) << single.why;
    EXPECT_NE(single.why.find("is 64 bits wide but register"),
              std::string::npos) << single.why;
}

TEST(AsmX86WidthAndDirection, TheByteImulFormGasRejectsIsStillRefused) {
    // ✔gas 2.42 rejects `imulb %al, %cl` — x86 has no two-operand byte IMUL.
    // Declaring one would accept input no reference assembler takes.
    auto const r = refusalOf("imulb %al, %cl");
    EXPECT_TRUE(r.refused) << r.why;
}

// ══ RED-ON-DISABLE — WRONG-ANSWER MUTANTS ══════════════════════════════════
//
// ★★★ THE MUTANTS ARE NOT "DELETE THE CAPABILITY". Deleting it restores the
// diagnostic this work started from, which every pin above would notice for the
// wrong reason. Each mutant below keeps the row and makes it encode a DIFFERENT
// INSTRUCTION — the shape that compiles, assembles, links and computes a wrong
// answer with no diagnostic anywhere.
// ⚠ CONFIG-LEVEL: this file MUST run through ctest — `dss_add_test` sets
// `DSS_CONFIG_ROOT`, while a bare `.exe` walks the cwd and would read whichever
// tree the shell stands in
// (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).

TEST(AsmX86WidthAndDirection, SwappingTheDirectionOpcodeBytesMakesEachSpellingTheOther) {
    // M1 — keep both variants, keep the axis, and SWAP the two opcode bytes
    // (0x39 <-> 0x3B) on `cmp`'s width-64 memory pair. The document loads, both
    // spellings still elect their own variant, and each now encodes the OTHER
    // one's instruction: `cmpq %r14, mem` compares `r14 - mem` where the source
    // says `mem - r14`. Every flag a signed comparison sets is inverted, and
    // nothing anywhere reports it.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            auto* vs = variantsOf(doc, "cmp");
            ASSERT_NE(vs, nullptr) << "x86_64 declares no `cmp`";
            for (auto& v : *vs) {
                auto const& g = v.at("guard");
                if (!g.contains("memoryDestination")) continue;
                if (g.value("width", 0) != 64) continue;
                auto& op = v.at("template").at("opcode");
                if (op == nlohmann::json::array({0x39})) {
                    op = nlohmann::json::array({0x3B});
                } else if (op == nlohmann::json::array({0x3B})) {
                    op = nlohmann::json::array({0x39});
                }
            }
        });
    ASSERT_TRUE(mutated.has_value())
        << "the mutant must still LOAD — a mutant the loader refuses proves "
           "only that it was refused";
    EXPECT_EQ(soleHex("cmpq %r14, 4096(%r15)"), "4d39b700100000");
    EXPECT_EQ(soleHex("cmpq 4096(%r15), %r14"), "4d3bb700100000");
    EXPECT_EQ(hexOfWith(*mutated, "cmpq %r14, 4096(%r15)"), "4d3bb700100000")
        << "the memory-DESTINATION spelling must now encode the memory-SOURCE "
           "instruction";
    EXPECT_EQ(hexOfWith(*mutated, "cmpq 4096(%r15), %r14"), "4d39b700100000")
        << "and the memory-SOURCE spelling the memory-DESTINATION one";
}

TEST(AsmX86WidthAndDirection, ErasingTheMemoryDirectionAxisIsRefusedAtLoad) {
    // ★ THE COMPLEMENT OF THE MUTANT ABOVE, and it pins the loader rather than
    // the encoder: with the axis gone from one of the pair, the two variants
    // share operandKinds AND width AND immediate domain, so the SHADOWING check
    // refuses the document — first-match would make the second unreachable and
    // one spelling would silently take the other's variant. ⚠ This is why the
    // wrong-answer mutant above swaps BYTES instead of erasing the axis: the
    // erase never gets past the loader, so it could only ever prove that the
    // loader said no.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            auto* vs = variantsOf(doc, "cmp");
            ASSERT_NE(vs, nullptr) << "x86_64 declares no `cmp`";
            for (auto& v : *vs) {
                auto& g = v.at("guard");
                if (!g.contains("memoryDestination")) continue;
                if (g.at("memoryDestination") == false) {
                    g.erase("memoryDestination");
                }
            }
        });
    EXPECT_FALSE(mutated.has_value())
        << "a memory-direction pair with the axis erased from one half must be "
           "a LOAD-TIME refusal (the shadowing rule)";
}

TEST(AsmX86WidthAndDirection, WideningTheImmediateSlotCorruptsTheStream) {
    // M2 — re-point `mov`'s width-16 immediate wire from the 2-byte slot to the
    // 4-byte one. The document loads, the spelling still elects, and the
    // instruction gains TWO trailing bytes that the CPU decodes as the start of
    // the NEXT instruction. Nothing refuses it.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            auto* vs = variantsOf(doc, "mov");
            ASSERT_NE(vs, nullptr) << "x86_64 declares no `mov`";
            for (auto& v : *vs) {
                if (v.at("guard").value("width", 0) != 16) continue;
                for (auto& w : v.at("wires")) {
                    if (w.at("slotKind") == "imm16.bytes") {
                        w["slotKind"] = "imm32";
                    }
                }
            }
        });
    ASSERT_TRUE(mutated.has_value());
    auto const good = soleHex("movw $300, %cx");
    auto const bad  = hexOfWith(*mutated, "movw $300, %cx");
    EXPECT_EQ(good, "66c7c12c01");
    EXPECT_EQ(bad, "66c7c12c010000")
        << "the widened slot must emit FOUR immediate bytes where the "
           "instruction takes two; got " << bad;
    EXPECT_EQ(good.size() + 4, bad.size())
        << "exactly two extra bytes, which is what corrupts everything after "
           "this instruction";
}

TEST(AsmX86WidthAndDirection, LosingTheDestinationWidthMakesAnExtendingMoveWrong) {
    // M3 — swap `sext32`'s width-8 opcode `0F BE` (movsbl, 32-bit destination)
    // for `0F B6` (movzbl, ZERO-extending). The instruction still assembles,
    // still writes %ecx, and produces a DIFFERENT VALUE for every negative
    // byte: 0x00000080 instead of 0xFFFFFF80. A "does it compile" test is green
    // straight through it.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            auto* vs = variantsOf(doc, "sext32");
            ASSERT_NE(vs, nullptr) << "x86_64 declares no `sext32`";
            for (auto& v : *vs) {
                auto& tmpl = v.at("template");
                if (tmpl.at("opcode") == nlohmann::json::array({15, 190})) {
                    tmpl["opcode"] = nlohmann::json::array({15, 182});
                }
            }
        });
    ASSERT_TRUE(mutated.has_value());
    auto const good = soleHex("movsbl %cl, %ecx");
    auto const bad  = hexOfWith(*mutated, "movsbl %cl, %ecx");
    EXPECT_EQ(good, "400fbec9");
    EXPECT_EQ(bad, "400fb6c9")
        << "the mutant must still emit an instruction, and it must be the "
           "ZERO-extending one; got " << bad;
    EXPECT_NE(bad, good);
}

TEST(AsmX86WidthAndDirection, DroppingTheResultSlotIsRefusedAtLoad) {
    // ★★★ THIS PIN EXISTS BECAUSE THE DEFECT IT DESCRIBES WAS WRITTEN, SHIPPED
    // INTO A LOCAL BUILD AND CAUGHT ONLY BY A BYTE DIFF —
    // D-TARGET-PRODUCER-VARIANT-WITHOUT-A-RESULT-SLOT-ENCODES-REGISTER-ZERO.
    // A value-producing x86-variable variant with no `resultSlot` used to LOAD
    // CLEAN and emit register field 0 for its result, so every
    // `movw $42, %cx` / `%dx` / `%r15w` produced the SAME bytes — `%ax` — with
    // rc=0 and no diagnostic. The loader now refuses it, and this pin is what
    // keeps the refusal.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            auto* vs = variantsOf(doc, "mov");
            ASSERT_NE(vs, nullptr) << "x86_64 declares no `mov`";
            for (auto& v : *vs) {
                if (v.at("guard").value("width", 0) != 16) continue;
                if (v.at("guard").at("operandKinds")
                    != nlohmann::json::array({"imm32"})) continue;
                v.erase("resultSlot");
            }
        });
    EXPECT_FALSE(mutated.has_value())
        << "a producing x86-variable variant with no resultSlot must be a "
           "LOAD-TIME refusal — it silently encodes register 0 otherwise";
}
