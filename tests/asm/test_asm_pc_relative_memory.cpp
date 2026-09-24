// ★★★ THE PROGRAM COUNTER AS A MEMORY BASE, AND A SYMBOL AS A DISPLACEMENT —
// `leaq msg(%rip), %rax`, the spelling gcc -S writes for every global access
// (D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER, P68 round 9).
//
// Every byte and every relocation below is the REFERENCE's, measured 2026-09-23
// with GNU as 2.42 and clang 18.1.3 (and mingw gas 2.41.90 / llvm-mc 18.1.3 for
// the PE and Mach-O relocations): the bytes agree on every format; the
// relocation's ADDEND is the addend written less the bytes that follow the
// field (`movl $5, counter(%rip)` is R_X86_64_PC32 `counter-8`: the field's own
// -4, which the target's `riprel32` bias carries, and the 4 immediate bytes,
// which the walker subtracts).
//
// Driven through the SHIPPED dialect and target, unmutated, except where a
// test names the one config fact it removes to prove the behaviour rests on it.

#include "asm_text_fixture.hpp"
#include "mutate_target_schema.hpp"
#include "asm/asm.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support::asm_text;

namespace {

std::string hex(std::vector<std::uint8_t> const& bytes) {
    std::string out = "bytes:";
    for (auto const b : bytes) out += std::format(" {:02X}", b);
    return out;
}

// One `main` holding `body`, a `ret`, and a data section with `msg` (8 bytes),
// `counter` (4) and `flag` (1) — the reference probe's own layout.
//
// ⚠ The `ret` carries NO label unless a test asks for one (`retLabel`).
// DSS still emits a `jmp` into a label that is reached by falling through
// (D-ASM-FALLTHROUGH-INTO-A-LABEL-EMITS-A-JUMP), and gas emits none. A label
// here would add 5 bytes that belong to that row, not to this file's
// byte-exact expectations.
std::string program(std::string_view body, std::string_view retLabel = "") {
    return std::format(
        "\t.text\n"
        "\t.globl\tmain\n"
        "\t.type\tmain, @function\n"
        "main:\n"
        "{}"
        "{}\tret\n"
        "\t.data\n"
        "msg:\t.quad\t0x1122334455667788\n"
        "counter:\t.long\t0\n"
        "flag:\t.byte\t0\n", body, retLabel);
}

struct Assembled {
    std::unique_ptr<LoweringRun> run;
    std::optional<AssembledModule> module;
    // `assemble` keeps a slot for every function even when an instruction in
    // it is refused, so a refusal shows up here and not in `ok()`.
    std::size_t assembleErrors = 0;
};

// Lower `source` through the shipped x86 dialect (optionally a MUTATED
// target) and assemble it.
Assembled assembleX86(std::string const& source,
                      std::shared_ptr<TargetSchema> target = nullptr) {
    Assembled a;
    auto const doc = shippedDialectDoc("asm-x86_64-att");
    a.run = target ? lowerAsmTextWithTarget(doc, source, std::move(target))
                   : lowerAsmText(doc, source);
    if (!a.run->module.has_value()) return a;
    auto const& lir = a.run->module->lir;
    DiagnosticReporter rep;
    std::vector<MirInstId> lirToMir(lir.instCount(), InvalidMirInst);
    a.module = assemble(lir, *a.run->target, lirToMir, rep);
    a.assembleErrors = rep.errorCount();
    return a;
}

// The data item whose bytes are exactly `bytes` — how a test names `msg`
// without depending on the order the walker minted symbols in.
SymbolId dataSymbolWithBytes(AsmTextModule const& m,
                             std::vector<std::uint8_t> const& bytes) {
    for (auto const& d : m.dataItems) {
        if (d.bytes == bytes) return d.symbol;
    }
    return SymbolId{};
}

std::vector<std::uint8_t> const kMsgBytes{0x88, 0x77, 0x66, 0x55,
                                          0x44, 0x33, 0x22, 0x11};

RelocationKind riprel32(TargetSchema const& t) {
    auto const* r = t.relocationByName("riprel32");
    EXPECT_NE(r, nullptr) << "the target declares no `riprel32` relocation";
    return r != nullptr ? r->kind : RelocationKind{};
}

// ── the numeric forms: no relocation, the displacement written as-is ──────

TEST(AsmPcRelativeMemory, ANumericDisplacementIsRelativeToTheNextInstruction) {
    auto const a = assembleX86(program("\tleaq\t8(%rip), %rcx\n"));
    ASSERT_TRUE(parsedCleanly(*a.run)) << parseMessages(*a.run);
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    ASSERT_TRUE(a.module.has_value() && a.module->ok());
    auto const& fn = a.module->functions.at(0);
    // ✔ gas 2.42 = clang 18.1.3: 48 8d 0d 08 00 00 00 (mod 00, rm 101), then ret.
    std::vector<std::uint8_t> const want{0x48, 0x8D, 0x0D, 0x08, 0x00, 0x00, 0x00,
                                         0xC3};
    EXPECT_EQ(fn.bytes, want) << hex(fn.bytes);
    EXPECT_TRUE(fn.relocations.empty());
}

TEST(AsmPcRelativeMemory, AnEmptyAndANegativeDisplacementStillCarry32Bits) {
    auto const a = assembleX86(program("\tleaq\t(%rip), %rbx\n"
                                       "\tleaq\t-8(%rip), %r11\n"));
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    ASSERT_TRUE(a.module.has_value() && a.module->ok());
    // ✔ 48 8d 1d 00 00 00 00 and 4c 8d 1d f8 ff ff ff — RIP-relative has no
    // disp8 form and no disp0 form: the mod=00 slot IS the disp32 one.
    std::vector<std::uint8_t> const want{
        0x48, 0x8D, 0x1D, 0x00, 0x00, 0x00, 0x00,
        0x4C, 0x8D, 0x1D, 0xF8, 0xFF, 0xFF, 0xFF,
        0xC3};
    EXPECT_EQ(a.module->functions.at(0).bytes, want)
        << hex(a.module->functions.at(0).bytes);
}

// ── the symbolic forms: a riprel32 relocation, the addend lowered by the
//    bytes after the field ─────────────────────────────────────────────────

TEST(AsmPcRelativeMemory, ASymbolDisplacementIsARipRelativeRelocation) {
    auto const a = assembleX86(program("\tleaq\tmsg(%rip), %rax\n"
                                       "\tmovq\tmsg(%rip), %rsi\n"));
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    ASSERT_TRUE(a.module.has_value() && a.module->ok());
    auto const& fn  = a.module->functions.at(0);
    SymbolId const msg = dataSymbolWithBytes(*a.run->module, kMsgBytes);
    ASSERT_TRUE(msg.valid());
    std::vector<std::uint8_t> const want{
        0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00,   // leaq msg(%rip), %rax
        0x48, 0x8B, 0x35, 0x00, 0x00, 0x00, 0x00,   // movq msg(%rip), %rsi
        0xC3};
    EXPECT_EQ(fn.bytes, want) << hex(fn.bytes);
    ASSERT_EQ(fn.relocations.size(), 2u);
    RelocationKind const kind = riprel32(*a.run->target);
    for (std::size_t i = 0; i < 2; ++i) {
        auto const& r = fn.relocations[i];
        EXPECT_EQ(r.offset, i == 0 ? 3u : 10u);
        EXPECT_EQ(r.target.v, msg.v);
        EXPECT_EQ(r.kind.v, kind.v);
        EXPECT_EQ(r.addend, 0);          // the field ends the instruction
        EXPECT_EQ(r.bytesAfterField, 0u);
    }
}

TEST(AsmPcRelativeMemory, AnImmediateAfterTheFieldIsSubtractedFromTheAddend) {
    auto const a = assembleX86(program("\tmovl\t$5, counter(%rip)\n"));
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    ASSERT_TRUE(a.module.has_value() && a.module->ok());
    auto const& fn = a.module->functions.at(0);
    // ✔ c7 05 <disp32> 05 00 00 00: the displacement BEFORE the immediate.
    std::vector<std::uint8_t> const want{0xC7, 0x05, 0x00, 0x00, 0x00, 0x00,
                                         0x05, 0x00, 0x00, 0x00, 0xC3};
    EXPECT_EQ(fn.bytes, want) << hex(fn.bytes);
    ASSERT_EQ(fn.relocations.size(), 1u);
    EXPECT_EQ(fn.relocations[0].offset, 2u);
    // ✔ gas: R_X86_64_PC32 counter-8 — the kind's -4 plus these -4.
    EXPECT_EQ(fn.relocations[0].addend, -4);
    EXPECT_EQ(fn.relocations[0].bytesAfterField, 4u);
}

TEST(AsmPcRelativeMemory, AConstantAfterTheSymbolIsTheAddendAndItsSignIsKept) {
    auto const a = assembleX86(program("\tmovq\tmsg+4(%rip), %rsi\n"
                                       "\tmovq\tmsg-8(%rip), %rdi\n"));
    ASSERT_TRUE(parsedCleanly(*a.run)) << parseMessages(*a.run);
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    ASSERT_TRUE(a.module.has_value() && a.module->ok());
    auto const& fn = a.module->functions.at(0);
    SymbolId const msg = dataSymbolWithBytes(*a.run->module, kMsgBytes);
    ASSERT_EQ(fn.relocations.size(), 2u);
    EXPECT_EQ(fn.relocations[0].target.v, msg.v);
    EXPECT_EQ(fn.relocations[0].addend, 4);
    // ★ `msg-8` IS msg MINUS 8 — never the number -8 with the name dropped.
    EXPECT_EQ(fn.relocations[1].target.v, msg.v);
    EXPECT_EQ(fn.relocations[1].addend, -8);
}

TEST(AsmPcRelativeMemory, AnInteriorLabelIsBoundToItsBlock) {
    auto const a = assembleX86(program("\tleaq\t1f(%rip), %rdx\n", "1:"));
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    ASSERT_TRUE(a.module.has_value() && a.module->ok());
    auto const& m  = *a.run->module;
    auto const& fn = a.module->functions.at(0);
    ASSERT_EQ(fn.relocations.size(), 1u);
    // The label's symbol reaches the driver's block binding — the channel a
    // data slot naming a code label uses — for block `1:` of `main`.
    ASSERT_EQ(m.blockSymbolBindings.size(), 1u);
    EXPECT_EQ(m.blockSymbolBindings[0].funcIndex, 0u);
    EXPECT_EQ(m.blockSymbolBindings[0].symbol.v, fn.relocations[0].target.v);
    auto const retBlock = m.lir.funcBlockAt(m.lir.funcAt(0), 1);
    EXPECT_EQ(m.blockSymbolBindings[0].lirBlockV, retBlock.v);
}

TEST(AsmPcRelativeMemory, ADataSlotCarriesTheConstantAfterItsSymbol) {
    auto const a = assembleX86(
        "\t.text\n\t.globl\tmain\n\t.type\tmain, @function\n"
        "main:\tret\n"
        "\t.data\n"
        "msg:\t.quad\t0x1122334455667788\n"
        "ptr:\t.quad\tmsg+8\n");
    ASSERT_TRUE(parsedCleanly(*a.run)) << parseMessages(*a.run);
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    SymbolId const msg = dataSymbolWithBytes(*a.run->module, kMsgBytes);
    std::size_t withReloc = 0;
    for (auto const& d : a.run->module->dataItems) {
        for (auto const& r : d.relocations) {
            ++withReloc;
            EXPECT_EQ(r.target.v, msg.v);
            EXPECT_EQ(r.addend, 8);
        }
    }
    EXPECT_EQ(withReloc, 1u);
}

// ── the refusals ───────────────────────────────────────────────────────────

TEST(AsmPcRelativeMemory, TheInstructionPointerIsNotAnOperandAnywhereElse) {
    // ✔ gas 2.42: "`%rip' not allowed with `movq'".
    auto const a = assembleX86(program("\tmovq\t%rip, %rax\n"));
    ASSERT_TRUE(parsedCleanly(*a.run)) << parseMessages(*a.run);
    EXPECT_FALSE(a.run->module.has_value());
    EXPECT_NE(messages(*a.run).find("movq"), std::string::npos)
        << messages(*a.run);
}

TEST(AsmPcRelativeMemory, AnIndexBesideTheInstructionPointerIsRefused) {
    auto const a = assembleX86(program("\tmovq\t8(%rip,%rax), %rcx\n"));
    ASSERT_TRUE(parsedCleanly(*a.run)) << parseMessages(*a.run);
    bool const refused = !a.run->module.has_value() || a.assembleErrors > 0;
    EXPECT_TRUE(refused) << (a.module.has_value() && !a.module->functions.empty()
                                 ? hex(a.module->functions.at(0).bytes)
                                 : std::string{"no module"});
}

TEST(AsmPcRelativeMemory, AConstantAfterASymbolOutsideADisplacementIsRefused) {
    for (std::string_view line : {"\tleaq\tmsg+4, %rax\n",
                                  "\tjmp\tmain+2\n",
                                  "\tcall\tmain+4\n"}) {
        auto const a = assembleX86(program(line));
        ASSERT_TRUE(parsedCleanly(*a.run)) << line << parseMessages(*a.run);
        EXPECT_FALSE(a.run->module.has_value()) << line;
    }
}

// ── red-on-disable: the behaviour rests on the declared facts ──────────────

TEST(AsmPcRelativeMemory, WithoutTheDeclaredBaseTheInstructionPointerIsRefused) {
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) { doc.erase("pcRelativeMemoryBase"); });
    ASSERT_TRUE(mutated.has_value());
    auto const a = assembleX86(program("\tleaq\t8(%rip), %rcx\n"), *mutated);
    ASSERT_TRUE(parsedCleanly(*a.run)) << parseMessages(*a.run);
    EXPECT_FALSE(a.run->module.has_value())
        << "no field accepts the instruction pointer once no memory base "
           "declares it";
}

TEST(AsmPcRelativeMemory, TheLoaderRefusesAProgramCounterEveryFieldWouldTake) {
    // Strip the role: the register would then fit every field by default and
    // encode as RBP — refused at load, by name.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            for (auto& r : doc["registers"]) {
                if (r.value("name", "") == "rip") {
                    r["encodingRole"] = "generalRegister";
                    r["encodingRoleIsDefault"] = true;
                }
                if (r.value("name", "") == "rbp") {
                    r["encodingRole"] = "instructionPointer";
                    r.erase("encodingRoleIsDefault");
                }
            }
        });
    ASSERT_FALSE(mutated.has_value());
    std::string all;
    for (auto const& e : mutated.error()) all += e.path + ": " + e.message + "\n";
    EXPECT_NE(all.find("/pcRelativeMemoryBase/register"), std::string::npos) << all;
}

TEST(AsmPcRelativeMemory, TheLoaderRefusesASlotWithNoPcRelativeForm) {
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            doc["pcRelativeMemoryBase"]["memoryBaseSlots"] =
                nlohmann::json::array({"modrm.rm.mem", "modrm.rm"});
        });
    ASSERT_FALSE(mutated.has_value());
    std::string all;
    for (auto const& e : mutated.error()) all += e.path + ": " + e.message + "\n";
    EXPECT_NE(all.find("/pcRelativeMemoryBase/memoryBaseSlots/1"),
              std::string::npos) << all;
}

TEST(AsmPcRelativeMemory, TheLoaderRefusesAnAbsoluteRelocationForTheBase) {
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            doc["pcRelativeMemoryBase"]["symbolicDisplacementRelocation"] = "abs32";
        });
    ASSERT_FALSE(mutated.has_value());
    std::string all;
    for (auto const& e : mutated.error()) all += e.path + ": " + e.message + "\n";
    EXPECT_NE(all.find("/pcRelativeMemoryBase/symbolicDisplacementRelocation"),
              std::string::npos) << all;
}

TEST(AsmPcRelativeMemory, TheLoaderRefusesAnAllocatableProgramCounter) {
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            doc["callingConventions"][0]["callerSaved"].push_back("rip");
        });
    ASSERT_FALSE(mutated.has_value());
    std::string all;
    for (auto const& e : mutated.error()) all += e.path + ": " + e.message + "\n";
    EXPECT_NE(all.find("program counter"), std::string::npos) << all;
}

} // namespace
