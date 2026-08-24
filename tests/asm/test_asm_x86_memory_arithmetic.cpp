// D-TARGET-X86-64-DECLARES-NO-MEMORY-DESTINATION-ARITHMETIC +
// D-ASM-SUB-NATIVE-OPERAND-UNUSABLE-IN-INLINE-ASM — the byte tier.
//
// ★★★ WHAT THIS FILE PINS AND WHY IT PINS BYTES. Both rows were closed by
// adding ROWS to `x86_64.target.json` and SPELLINGS to
// `asm-x86_64-att.lang.json`. The corpus examples beside them prove the results
// survive the linker and the loader; they cannot say WHICH byte was wrong when
// they fail, and they cannot distinguish two encodings that happen to agree on
// the one value the example computes. This file drives the SHIPPED dialect and
// the SHIPPED target UNMUTATED and asserts the exact instruction bytes.
//
// ★★★ EVERY MEMORY-FORM EXPECTATION BELOW IS GNU as 2.42's OWN OUTPUT, not an
// encoding derived from the SDM and then confirmed. Each was measured one
// spelling at a time (`as -o t.o t.s; objdump -d t.o`), which is why the
// displacement is 4096 and the immediate 300: both are outside the short-form
// windows, so gas is forced onto the `disp32` and `imm32` long forms — the only
// ones the x86-variable walker emits. A smaller displacement would have made
// gas pick `disp8` and turned a real disagreement into an expected one.
//
// ⚠ THE REGISTER-DIRECT FORMS ARE **NOT** BYTE-IDENTICAL TO gas, AND SAYING SO
// IS THE POINT RATHER THAN AN EXCUSE. gas prefers x86's special-case short
// encodings where they exist — `addq $300, %rax` becomes `05 id` (the
// accumulator form) instead of `81 /0 id`, `movb %al, %cl` becomes `88 /r`
// (MOV r/m8, r8) instead of `8A /r`, and `movb $42, %cl` becomes `B0+rb ib`.
// Each pair encodes the SAME instruction; DSS emits the general form its
// declared variant names. Those pins therefore assert the DSS encoding, and the
// comment on each says which gas byte string it is equivalent to, so a reader
// can re-derive the equivalence instead of taking it on trust.

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

// One `.s` line, assembled through the SHIPPED dialect and the SHIPPED target,
// with the trailing `ret` subtracted by differencing against the same function
// without the line. The subtraction is what makes the pin EXACT rather than a
// prefix match: a stray extra instruction would still fail it.
[[nodiscard]] std::vector<std::uint8_t>
bytesOfWith(std::shared_ptr<TargetSchema> target, std::string const& line) {
    auto const doc  = shippedDialectDoc("asm-x86_64-att");
    auto const src  = std::string{"\t.globl main\n\t.type main, @function\n"
                                 "main:\n"} + line + "\tret\n";
    auto const run  = lowerAsmTextWithTarget(doc, src, std::move(target));
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

// The subject line's OWN bytes, in hex — the terminator differenced away.
[[nodiscard]] std::string soleHex(std::string const& line) {
    auto const target  = shippedX86();
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

// Whether the shipped pipeline REFUSES `line`, and the message it gave.
struct Refusal { bool refused; std::string why; };

[[nodiscard]] Refusal refusalOfWith(std::shared_ptr<TargetSchema> target,
                                    std::string const&            line) {
    auto const doc = shippedDialectDoc("asm-x86_64-att");
    auto const src = std::string{"\t.globl main\n\t.type main, @function\n"
                                 "main:\n\t"} + line + "\n\tret\n";
    auto const run = lowerAsmTextWithTarget(doc, src, std::move(target));
    EXPECT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    return Refusal{!run->module.has_value(), messages(*run)};
}

[[nodiscard]] Refusal refusalOf(std::string const& line) {
    return refusalOfWith(shippedX86(), line);
}

// ══ MEMORY DESTINATION — the row's own subject ═════════════════════════════

TEST(AsmX86MemoryArithmetic, ImmediateIntoMemoryMatchesTheReferenceAssembler) {
    // ✔gas 2.42, measured: `498187001000002c010000` / `418187001000002c010000`.
    EXPECT_EQ(soleHex("addq $300, 4096(%r15)"), "498187001000002c010000");
    EXPECT_EQ(soleHex("addl $300, 4096(%r15)"), "418187001000002c010000");
    EXPECT_EQ(soleHex("subq $300, 4096(%r15)"), "4981af001000002c010000");
    EXPECT_EQ(soleHex("subl $300, 4096(%r15)"), "4181af001000002c010000");
    EXPECT_EQ(soleHex("andq $300, 4096(%r15)"), "4981a7001000002c010000");
    EXPECT_EQ(soleHex("andl $300, 4096(%r15)"), "4181a7001000002c010000");
    EXPECT_EQ(soleHex("orq $300, 4096(%r15)"),  "49818f001000002c010000");
    EXPECT_EQ(soleHex("orl $300, 4096(%r15)"),  "41818f001000002c010000");
    EXPECT_EQ(soleHex("xorq $300, 4096(%r15)"), "4981b7001000002c010000");
    EXPECT_EQ(soleHex("xorl $300, 4096(%r15)"), "4181b7001000002c010000");
}

TEST(AsmX86MemoryArithmetic, RegisterIntoMemoryMatchesTheReferenceAssembler) {
    // ★ THE ModR/M.reg FIELD CARRIES THE **SOURCE** HERE AND THE DESTINATION IN
    // the memory-SOURCE test below; r14 is used on purpose so REX.R is set and
    // a swapped wiring would be visible in the prefix byte as well as in
    // ModR/M.
    EXPECT_EQ(soleHex("addq %r14, 4096(%r15)"),  "4d01b700100000");
    EXPECT_EQ(soleHex("addl %r14d, 4096(%r15)"), "4501b700100000");
    EXPECT_EQ(soleHex("subq %r14, 4096(%r15)"),  "4d29b700100000");
    EXPECT_EQ(soleHex("subl %r14d, 4096(%r15)"), "4529b700100000");
    EXPECT_EQ(soleHex("andq %r14, 4096(%r15)"),  "4d21b700100000");
    EXPECT_EQ(soleHex("orq %r14, 4096(%r15)"),   "4d09b700100000");
    EXPECT_EQ(soleHex("xorq %r14, 4096(%r15)"),  "4d31b700100000");
}

TEST(AsmX86MemoryArithmetic, UnaryAgainstMemoryMatchesTheReferenceAssembler) {
    // The one shape with NO leading source operand — the memory reference is
    // both the only operand and the destination.
    EXPECT_EQ(soleHex("notq 4096(%r15)"), "49f79700100000");
    EXPECT_EQ(soleHex("notl 4096(%r15)"), "41f79700100000");
    EXPECT_EQ(soleHex("negq 4096(%r15)"), "49f79f00100000");
    EXPECT_EQ(soleHex("negl 4096(%r15)"), "41f79f00100000");
}

// ══ MEMORY SOURCE — the other direction, and the reason the two are separate
//    opcodes rather than two variants of one ════════════════════════════════

TEST(AsmX86MemoryArithmetic, MemorySourceMatchesTheReferenceAssembler) {
    EXPECT_EQ(soleHex("addq 4096(%r15), %r14"),   "4d03b700100000");
    EXPECT_EQ(soleHex("addl 4096(%r15), %r14d"),  "4503b700100000");
    EXPECT_EQ(soleHex("subq 4096(%r15), %r14"),   "4d2bb700100000");
    EXPECT_EQ(soleHex("andq 4096(%r15), %r14"),   "4d23b700100000");
    EXPECT_EQ(soleHex("orq 4096(%r15), %r14"),    "4d0bb700100000");
    EXPECT_EQ(soleHex("xorq 4096(%r15), %r14"),   "4d33b700100000");
    EXPECT_EQ(soleHex("imulq 4096(%r15), %r14"),  "4d0fafb700100000");
    EXPECT_EQ(soleHex("imull 4096(%r15), %r14d"), "450fafb700100000");
}

// ★★★ THE PAIR THAT MAKES THE DIRECTION A CLAIM RATHER THAN A COINCIDENCE.
// `subq %r14, mem` and `subq mem, %r14` build BYTE-IDENTICAL LIR operand lists
// — `[reg, reg, MemBase, MemOffset]` either way — and mean opposite things.
// They stay distinguishable only because the memory-destination row is a
// `result: none` opcode (`sub_mem`) elected from the consumer set, while the
// memory-source row is a variant on the producer `sub`. This test asserts the
// two never collapse: same operands, different opcode byte, 0x29 vs 0x2B.
TEST(AsmX86MemoryArithmetic, TheTwoDirectionsEncodeDifferentInstructions) {
    auto const toMemory   = soleHex("subq %r14, 4096(%r15)");
    auto const fromMemory = soleHex("subq 4096(%r15), %r14");
    EXPECT_NE(toMemory, fromMemory);
    EXPECT_EQ(toMemory,   "4d29b700100000");
    EXPECT_EQ(fromMemory, "4d2bb700100000");
}

// ══ REGISTER-DIRECT IMMEDIATE ALU — the hole beside the memory rows ════════

TEST(AsmX86MemoryArithmetic, BitwiseImmediateAgainstARegisterIsEncodable) {
    // ✔BEFORE: `andq $300, %rax` was refused — `and`/`or`/`xor` declared
    // reg-reg variants only, while `add`/`sub`/`cmp` had had their imm32
    // siblings since FC3. gas assembles all six.
    // ⚠ DSS EMITS `81 /digit id`; gas emits the ACCUMULATOR short form
    // (`andq $300, %rax` → `48252c010000`). Same instruction, different
    // encoding — see this file's banner.
    EXPECT_EQ(soleHex("andq $300, %rax"), "4881e02c010000");
    EXPECT_EQ(soleHex("orq $300, %rax"),  "4881c82c010000");
    EXPECT_EQ(soleHex("xorq $300, %rax"), "4881f02c010000");
    EXPECT_EQ(soleHex("andl $300, %eax"), "81e02c010000");
    EXPECT_EQ(soleHex("orl $300, %eax"),  "81c82c010000");
    EXPECT_EQ(soleHex("xorl $300, %eax"), "81f02c010000");
}

// ══ SUB-NATIVE OPERANDS ════════════════════════════════════════════════════

TEST(AsmX86MemoryArithmetic, ByteAndWordRegisterMovesAreEncodable) {
    // ⚠ DSS emits `8A /r` (MOV r8, r/m8) with a forced REX; gas prefers the
    // reversed `88 /r` (`movb %al, %cl` → `88c1`). Both write cl from al.
    // ★ THE FORCED REX IS A CORRECTNESS REQUIREMENT, NOT A SIZE COST: without
    // it, byte-register encodings 4..7 name `ah`/`ch`/`dh`/`bh` rather than
    // `spl`/`bpl`/`sil`/`dil`, i.e. a DIFFERENT physical register, silently.
    // The bare `40` prefix on the al/cl pair is that rule showing its work.
    EXPECT_EQ(soleHex("movb %al, %cl"), "408ac8");
    EXPECT_EQ(soleHex("movb %r14b, %r15b"), "458afe");
    // The 16-bit form needs NO REX — the 0x66 operand-size prefix selects the
    // width, and the high-byte alias hazard is byte-register-only. The prefix
    // MUST precede REX (x86 decode reads a mandatory prefix after REX as a
    // plain legacy prefix, selecting a different instruction).
    EXPECT_EQ(soleHex("movw %ax, %cx"), "668bc8");
    EXPECT_EQ(soleHex("movw %r14w, %r15w"), "66458bfe");
    // ⚠ gas emits `b12a` (B0+rb ib); DSS emits `C6 /0 ib` + the forced REX.
    EXPECT_EQ(soleHex("movb $42, %cl"), "40c6c12a");
}

TEST(AsmX86MemoryArithmetic, NarrowMemoryMovesMatchTheReferenceAssembler) {
    // ★★★ THE MEMORY-SOURCE ROWS ARE `load_subreg`, NOT `load`. The target's
    // width-8/width-16 `load` variants are MOVZX (`0F B6` / `0F B7`), which
    // ZERO the destination's upper bits; gas spells those `movzbl`/`movzwl`.
    // `movb (%r15), %cl` is `8A /r` and leaves them alone. These four are gas's
    // own bytes, so the distinction is measured rather than argued.
    EXPECT_EQ(soleHex("movb 4096(%r15), %cl"), "418a8f00100000");
    EXPECT_EQ(soleHex("movw 4096(%r15), %cx"), "66418b8f00100000");
    EXPECT_EQ(soleHex("movb %cl, 4096(%r15)"), "41888f00100000");
    EXPECT_EQ(soleHex("movw %cx, 4096(%r15)"), "6641898f00100000");
}

// ══ WHAT IS STILL REFUSED, AND WITH WHICH REASON ═══════════════════════════

TEST(AsmX86MemoryArithmetic, CompareWithAnImmediateAgainstMemoryIsEncodable) {
    // ★ THE HALF OF `cmp`-against-memory THAT IS SAFELY EXPRESSIBLE, and the
    // reason it is safe is checkable rather than asserted: the memory-SOURCE
    // operand list always BEGINS with the destination register, so it can never
    // be `[imm32, …]`. The register-operand pair below has no such separator.
    // ✔gas 2.42's own bytes.
    EXPECT_EQ(soleHex("cmpq $300, 4096(%r15)"), "4981bf001000002c010000");
    EXPECT_EQ(soleHex("cmpl $300, 4096(%r15)"), "4181bf001000002c010000");
}

TEST(AsmX86MemoryArithmetic, CompareWithARegisterAgainstMemoryEncodesBothDirections) {
    // ★★★ THIS PIN REPLACES A REFUSAL PIN, AND THE REPLACEMENT IS THE WHOLE
    // POINT OF THE ROW IT CLOSES. Until 2026-08-23 both spellings below were
    // refused on purpose: `cmp` produces no value, so its dialect row lists no
    // producer, so the engine offered the SAME candidate set to the
    // memory-destination operand list and to the memory-source one — and those
    // two lists are BYTE-IDENTICAL while meaning `mem - reg` and `reg - mem`.
    // Declaring either direction would have silently encoded the other for the
    // opposite spelling. The direction now rides the INSTRUCTION
    // (`kLirInstFlagMemoryIsDestination`) and the target's two variants are
    // keyed on it (`guard.memoryDestination`), so the two spellings reach two
    // different opcode bytes. Anchor:
    // D-ASM-X86-CMP-AGAINST-MEMORY-DIRECTION-IS-UNELECTABLE.
    // ✔Every expectation below is gas 2.42's own output.
    // ⚠ THE TWO EXPECTATIONS DIFFER IN EXACTLY ONE BYTE — 0x39 vs 0x3B — which
    // is why they are pinned as a PAIR: a mechanism that lost the direction
    // would make them equal, and either one alone would still pass.
    EXPECT_EQ(soleHex("cmpq %r14, 4096(%r15)"), "4d39b700100000");
    EXPECT_EQ(soleHex("cmpq 4096(%r15), %r14"), "4d3bb700100000");
    EXPECT_EQ(soleHex("cmpl %r14d, 4096(%r15)"), "4539b700100000");
    EXPECT_EQ(soleHex("cmpl 4096(%r15), %r14d"), "453bb700100000");
    EXPECT_NE(soleHex("cmpq %r14, 4096(%r15)"),
              soleHex("cmpq 4096(%r15), %r14"))
        << "the two directions must not encode identically";
}

TEST(AsmX86MemoryArithmetic, SixteenBitImmediateNowHasItsSlot) {
    // ★★ ALSO A REPLACED REFUSAL PIN. `movw $42, %cx` is `66 C7 /0 iw` — a
    // TWO-byte immediate — and the x86-variable walker declared no 2-byte
    // immediate slot, so the spelling stayed fail-loud rather than emit the
    // 4-byte one and corrupt the instruction stream
    // (D-ASM-X86-NO-16BIT-IMMEDIATE-SLOT). `EncodingSlotKind::Imm16Bytes` is
    // that slot. ⚠ NOT byte-identical to gas here, and the difference is
    // recorded rather than hidden: gas contracts to the `+rw` short form
    // `66 b9 2a 00`; DSS emits the general ModR/M form its variant declares.
    // Both decode to `mov $0x2a,%cx` (✔re-decoded with objdump).
    EXPECT_EQ(soleHex("movw $42, %cx"), "66c7c12a00");
    // ★ THE IMMEDIATE IS TWO BYTES, AND THIS IS THE PIN THAT SAYS SO: a value
    // above 0xFF would look identical under a 1-byte slot only by truncation,
    // and a 4-byte slot would append two extra zero bytes.
    EXPECT_EQ(soleHex("movw $300, %cx"), "66c7c12c01");
    // Memory destination, byte-identical to gas.
    EXPECT_EQ(soleHex("movw $42, 4096(%r15)"), "6641c787001000002a00");
    // The whole 16-bit ALU-immediate family the slot was blocking.
    EXPECT_EQ(soleHex("addw $300, %cx"), "6681c12c01");
    EXPECT_EQ(soleHex("subw $300, %cx"), "6681e92c01");
    EXPECT_EQ(soleHex("andw $300, %cx"), "6681e12c01");
    EXPECT_EQ(soleHex("orw $300, %cx"),  "6681c92c01");
    EXPECT_EQ(soleHex("xorw $300, %cx"), "6681f12c01");
    EXPECT_EQ(soleHex("cmpw $300, %cx"), "6681f92c01");
    EXPECT_EQ(soleHex("addw $300, 4096(%r15)"), "66418187001000002c01");
}

TEST(AsmX86MemoryArithmetic, MixedRegisterWidthsStayRefused) {
    // The guard that keeps the new narrow rows from becoming a way to spell a
    // width-crossing move. gas rejects `movb %eax, %cl` too, so the refusal is
    // conformance rather than a limitation.
    auto const r = refusalOf("movb %eax, %cl");
    EXPECT_TRUE(r.refused) << r.why;
    EXPECT_NE(r.why.find("is 32 bits wide but register"), std::string::npos)
        << r.why;
}

// ══ RED-ON-DISABLE — the config-level mutants ══════════════════════════════
//
// ★★★ THE STRONG MUTANT HERE IS NOT "DELETE THE ROW". Deleting it restores the
// diagnostic this work started from, which every pin above would notice for the
// wrong reason. The mutants below keep the row and make it encode the WRONG
// INSTRUCTION — the shape that compiles, assembles, links and computes a wrong
// answer with no diagnostic anywhere. Each one is applied to the SHIPPED
// document in memory (`mutateShippedTargetSchemaDoc`, which throws if the edit
// was a no-op), so a navigator that missed its container cannot pass silently.
// ⚠ CONFIG-LEVEL: this file MUST run through ctest — `dss_add_test` sets
// `DSS_CONFIG_ROOT`, while a bare `.exe` walks the cwd and would read whichever
// tree the shell stands in (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).

namespace {

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

TEST(AsmX86MemoryArithmetic, CrossingTheMemoryDestinationWiresChangesTheBytes) {
    // M1 — the value register and the memory BASE swap slots. The variant still
    // loads, still elects, still assembles: `addq %r14, 4096(%r15)` becomes an
    // instruction that adds r15 into memory at r14. A pure "does it compile"
    // test is green through this; only the bytes see it.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            auto* vs = variantsOf(doc, "add_mem");
            ASSERT_NE(vs, nullptr) << "x86_64 declares no `add_mem`";
            for (auto& v : *vs) {
                auto const& kinds = v.at("guard").at("operandKinds");
                if (kinds.size() != 4 || kinds[0] != "reg") continue;
                if (v.at("guard").value("width", 0) != 64) continue;
                v.at("wires")[0]["slotKind"] = "modrm.rm.mem";
                v.at("wires")[1]["slotKind"] = "modrm.reg";
            }
        });
    ASSERT_TRUE(mutated.has_value());
    auto const good = soleHex("addq %r14, 4096(%r15)");
    auto const bad  = hex(bytesOfWith(*mutated, "\taddq %r14, 4096(%r15)\n"));
    EXPECT_EQ(good, "4d01b700100000");
    EXPECT_NE(bad, "") << "the mutant must still emit an instruction — a mutant "
                          "that refuses proves only that it was refused";
    EXPECT_NE(bad.find("4d01"), std::string::npos)
        << "the mutant must still be an ADD (same opcode byte): " << bad;
    EXPECT_NE(bad, good + "c3")
        << "the crossed wiring must change the bytes, or the pin above is "
           "asserting nothing";
}

TEST(AsmX86MemoryArithmetic, FlippingTheDirectionByteIsCaughtByTheBytePin) {
    // M2 — `add_mem`'s register form keeps every wire and swaps ONE opcode
    // byte, 0x01 (ADD r/m64, r64) → 0x03 (ADD r64, r/m64). The instruction then
    // adds MEMORY INTO THE REGISTER while the source text says the opposite:
    // it assembles, it runs, and the memory operand is never written. This is
    // the exact silent-miscompile shape the corpus example's non-commutative
    // arms exist to catch at run time and this pin catches at the byte tier.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            auto* vs = variantsOf(doc, "add_mem");
            ASSERT_NE(vs, nullptr) << "x86_64 declares no `add_mem`";
            for (auto& v : *vs) {
                auto& tmpl = v.at("template");
                if (tmpl.at("opcode") == nlohmann::json::array({1})) {
                    tmpl["opcode"] = nlohmann::json::array({3});
                }
            }
        });
    ASSERT_TRUE(mutated.has_value());
    auto const bad = hex(bytesOfWith(*mutated, "\taddq %r14, 4096(%r15)\n"));
    EXPECT_NE(bad, "") << "the mutant must still emit an instruction";
    EXPECT_NE(bad.find("4d03b700100000"), std::string::npos)
        << "the mutant must encode the OPPOSITE direction: " << bad;
    EXPECT_EQ(soleHex("addq %r14, 4096(%r15)"), "4d01b700100000")
        << "and the shipped document must still encode the right one";
}

TEST(AsmX86MemoryArithmetic, RemovingTheByteMoveVariantIsRefusedLoudly) {
    // M3 — the fail-LOUD half. Strip `mov`'s width-8 variant and `movb %al,
    // %cl` must be refused with the shape diagnostic, not encoded at some
    // other width. This is the mutant whose value is that the refusal is a
    // REFUSAL rather than a fallback.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            auto* vs = variantsOf(doc, "mov");
            ASSERT_NE(vs, nullptr) << "x86_64 declares no `mov`";
            for (auto it = vs->begin(); it != vs->end();) {
                auto const& g = it->at("guard");
                if (g.at("operandKinds").size() == 1
                    && g.at("operandKinds")[0] == "reg"
                    && g.value("width", 0) == 8) {
                    it = vs->erase(it);
                } else {
                    ++it;
                }
            }
        });
    ASSERT_TRUE(mutated.has_value());
    // ★ THE REFUSAL IS WHOLE-MODULE, AND THAT IS THE STRONGER ANSWER. An
    // earlier draft of this pin expected the mutant to emit "the terminator
    // alone"; ✔MEASURED, it emits NOTHING — `lowerAsmTextToLir` returns no
    // module at all once an instruction cannot be lowered, so the byte move
    // cannot be silently dropped from an otherwise-complete function either.
    auto const bad = refusalOfWith(*mutated, "movb %al, %cl");
    EXPECT_TRUE(bad.refused)
        << "stripping the width-8 `mov` variant must REFUSE the byte move, "
           "never encode it at another width";
    EXPECT_NE(bad.why.find("no candidate target opcode encodes that shape"),
              std::string::npos)
        << "and the refusal must name the shape: " << bad.why;
    EXPECT_EQ(soleHex("movb %al, %cl"), "408ac8")
        << "and the shipped document must still encode it";
}

} // namespace
