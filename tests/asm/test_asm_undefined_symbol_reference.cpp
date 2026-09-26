// ★★★ A `.s` NAMES A SYMBOL IT DOES NOT DEFINE, BY ADDRESS, AND STATES NO KIND
// (D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL, P68 round 9).
//
// ✔MEASURED 2026-09-23 with GNU as 2.42: `leaq sib_data(%rip)` and `leaq
// sib_fn(%rip)` are the same R_X86_64_PC32 against a NOTYPE undefined symbol,
// and `.quad sib_fn` is an R_X86_64_64 against one. The assembler records the
// NAME and nothing about whether it is code or data; the linker takes that from
// the definition it resolves.
//
// So the lowering mints the import with its kind PENDING
// (`ExternKindOrigin::Pending`) for an address operand, a memory displacement
// or a data slot; a CALL states CODE (`Stated`), and a call to a name an earlier
// address left pending settles it. What the link does with a pending row is
// pinned beside the linker (`tests/link/test_import_kind_from_definition.cpp`).
//
// Driven through the SHIPPED dialect and target, unmutated.

#include "asm_text_fixture.hpp"
#include "asm/asm.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

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

// One exported `main` holding `body` and a `ret`, then `data` verbatim.
std::string program(std::string_view body, std::string_view data = "") {
    return std::format(
        "\t.text\n"
        "\t.globl\tmain\n"
        "\t.type\tmain, @function\n"
        "main:\n"
        "{}"
        "\tret\n"
        "{}", body, data);
}

struct Assembled {
    std::unique_ptr<LoweringRun>   run;
    std::optional<AssembledModule> module;
    std::size_t                    assembleErrors = 0;
};

Assembled assembleX86(std::string const& source) {
    Assembled a;
    a.run = lowerAsmText(shippedDialectDoc("asm-x86_64-att"), source);
    if (!a.run->module.has_value()) return a;
    auto const& lir = a.run->module->lir;
    DiagnosticReporter rep;
    std::vector<MirInstId> lirToMir(lir.instCount(), InvalidMirInst);
    a.module = assemble(lir, *a.run->target, lirToMir, rep);
    a.assembleErrors = rep.errorCount();
    return a;
}

// The ONE import row named `name`; a second row of the same name is itself a
// failure (a name is imported once, whatever references it).
ExternImport const* importNamed(AsmTextModule const& m, std::string_view name) {
    ExternImport const* found = nullptr;
    for (auto const& e : m.externImports) {
        if (e.mangledName != name) continue;
        EXPECT_EQ(found, nullptr) << "'" << name << "' was imported twice";
        found = &e;
    }
    return found;
}

RelocationKind kindNamed(TargetSchema const& t, std::string_view name) {
    auto const* r = t.relocationByName(name);
    EXPECT_NE(r, nullptr) << "the target declares no `" << name << "` relocation";
    return r != nullptr ? r->kind : RelocationKind{};
}

}  // namespace

// ── an address states no kind ──────────────────────────────────────────────

TEST(AsmUndefinedSymbolReference, AnAddressOperandMintsAnImportWhoseKindIsPending) {
    auto const a = assembleX86(program("\tleaq\text(%rip), %rax\n"));
    ASSERT_TRUE(parsedCleanly(*a.run)) << parseMessages(*a.run);
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    ASSERT_TRUE(a.module.has_value() && a.module->ok());
    ExternImport const* ext = importNamed(*a.run->module, "ext");
    ASSERT_NE(ext, nullptr) << "an address of an undefined name must become an import";
    EXPECT_EQ(ext->kindOrigin, ExternKindOrigin::Pending)
        << "an address operand says nothing about code or data";
    EXPECT_TRUE(ext->libraryPath.empty()) << "a `.s` states no owning image";
    // The instruction relocates against THAT row, with the reference's own
    // RIP-relative kind: the same bytes gas writes.
    auto const& fn = a.module->functions.at(0);
    std::vector<std::uint8_t> const want{0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3};
    EXPECT_EQ(fn.bytes, want);
    ASSERT_EQ(fn.relocations.size(), 1u);
    EXPECT_EQ(fn.relocations[0].offset, 3u);
    EXPECT_EQ(fn.relocations[0].target.v, ext->symbol.v);
    EXPECT_EQ(fn.relocations[0].kind.v, kindNamed(*a.run->target, "riprel32").v);
    EXPECT_EQ(fn.relocations[0].addend, 0);
}

TEST(AsmUndefinedSymbolReference, AMemoryDisplacementKeepsItsConstantAgainstTheImport) {
    auto const a = assembleX86(program("\tmovl\text+4(%rip), %eax\n"));
    ASSERT_TRUE(parsedCleanly(*a.run)) << parseMessages(*a.run);
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    ASSERT_TRUE(a.module.has_value() && a.module->ok());
    ExternImport const* ext = importNamed(*a.run->module, "ext");
    ASSERT_NE(ext, nullptr);
    EXPECT_EQ(ext->kindOrigin, ExternKindOrigin::Pending);
    auto const& fn = a.module->functions.at(0);
    ASSERT_EQ(fn.relocations.size(), 1u);
    EXPECT_EQ(fn.relocations[0].target.v, ext->symbol.v);
    // ✔ gas: R_X86_64_PC32 ext+0 -- the written +4 less the field's own 4,
    // which the kind's bias carries; DSS's addend is the written constant.
    EXPECT_EQ(fn.relocations[0].addend, 4);
}

TEST(AsmUndefinedSymbolReference, ADataSlotMintsAPendingImportAndKeepsItsAddend) {
    auto const a = assembleX86(program("", "\t.data\nslot:\t.quad\text+8\n"));
    ASSERT_TRUE(parsedCleanly(*a.run)) << parseMessages(*a.run);
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    ExternImport const* ext = importNamed(*a.run->module, "ext");
    ASSERT_NE(ext, nullptr) << "a data slot naming an undefined name must become an import";
    EXPECT_EQ(ext->kindOrigin, ExternKindOrigin::Pending);
    std::size_t slots = 0;
    for (auto const& d : a.run->module->dataItems) {
        for (auto const& r : d.relocations) {
            ++slots;
            EXPECT_EQ(r.target.v, ext->symbol.v);
            EXPECT_EQ(r.kind.v, kindNamed(*a.run->target, "abs64").v);
            EXPECT_EQ(r.addend, 8) << "the slot holds ext+8, never ext";
        }
    }
    EXPECT_EQ(slots, 1u);
}

// ── a call states code, and settles an earlier address ────────────────────

TEST(AsmUndefinedSymbolReference, ACallStatesCode) {
    auto const a = assembleX86(program("\tcall\text\n"));
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    ExternImport const* ext = importNamed(*a.run->module, "ext");
    ASSERT_NE(ext, nullptr);
    EXPECT_EQ(ext->kindOrigin, ExternKindOrigin::Stated);
    EXPECT_FALSE(ext->isData);
}

TEST(AsmUndefinedSymbolReference, ALaterCallSettlesAnEarlierAddress) {
    auto const a = assembleX86(program("\tleaq\text(%rip), %rax\n"
                                       "\tcall\text\n"));
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    ASSERT_EQ(a.run->module->externImports.size(), 1u)
        << "one name, one import, however many references";
    ExternImport const* ext = importNamed(*a.run->module, "ext");
    ASSERT_NE(ext, nullptr);
    EXPECT_EQ(ext->kindOrigin, ExternKindOrigin::Stated)
        << "the call states CODE for the whole file";
    EXPECT_FALSE(ext->isData);
}

TEST(AsmUndefinedSymbolReference, AnAddressAfterACallLeavesTheCallsAnswer) {
    auto const a = assembleX86(program("\tcall\text\n"
                                       "\tleaq\text(%rip), %rax\n",
                                       "\t.data\nslot:\t.quad\text\n"));
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    ASSERT_EQ(a.run->module->externImports.size(), 1u);
    ExternImport const* ext = importNamed(*a.run->module, "ext");
    ASSERT_NE(ext, nullptr);
    EXPECT_EQ(ext->kindOrigin, ExternKindOrigin::Stated)
        << "an address states nothing, so it cannot unsettle what a call stated";
    EXPECT_FALSE(ext->isData);
}

// ── the control: a name the file DOES define is never imported ────────────

TEST(AsmUndefinedSymbolReference, ANameTheFileDefinesIsNotAnImport) {
    auto const a = assembleX86(program("\tleaq\there(%rip), %rax\n",
                                       "\t.data\nhere:\t.long\t7\n"
                                       "slot:\t.quad\there\n"));
    ASSERT_TRUE(parsedCleanly(*a.run)) << parseMessages(*a.run);
    ASSERT_TRUE(a.run->module.has_value()) << messages(*a.run);
    EXPECT_TRUE(a.run->module->externImports.empty())
        << "a defined label must bind to itself, not to an import";
}
