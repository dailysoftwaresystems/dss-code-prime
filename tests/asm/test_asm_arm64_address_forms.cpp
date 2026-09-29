// ★★★ THE AARCH64 ADDRESS FORMS — the PAGE and the PAGE OFFSET of a symbol, a
// constant after a name, the one-word `adr`, a block of the function addressed
// in a field and the location counter (P68 round 9, the aarch64 twins of
// D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER).
//
// Every word and relocation pinned here is the reference's, ✔MEASURED
// 2026-09-23 one instruction per file (lane xa m15/m18): GNU as 2.42 and clang
// 18.1.3 for ELF agree word for word and relocation for relocation; clang 18
// for Darwin writes the same words with the Mach-O spellings (`@PAGE`,
// `@PAGEOFF`) and refuses the ELF ones, as both ELF references refuse its.
// So which spelling is read is keyed on the object format being written —
// `symbolParts[].formatKinds` in the dialect, the kind handed in by the caller
// — and a spelling for the other format is refused BY NAME, naming this
// format's own.
//
// Driven through the SHIPPED dialect and target, unmutated, except where a
// test says it edits the dialect document to state a refusal.

#include "asm_text_fixture.hpp"
#include "asm/asm.hpp"
#include "asm/asm_template_to_lir.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::test_support::asm_text;

namespace {

constexpr std::optional<ObjectFormatKind> kElf   = ObjectFormatKind::Elf;
constexpr std::optional<ObjectFormatKind> kMachO = ObjectFormatKind::MachO;

// One exported `main` holding `body` (then nothing — the body ends it), and a
// data section with a few named items at known offsets.
std::string program(std::string_view body) {
    return std::format(
        "\t.text\n"
        "\t.globl\tmain\n"
        "\t.type\tmain, %function\n"
        "main:\n"
        "{}"
        "\t.data\n"
        "\t.p2align\t4\n"
        "vec:\t.quad\t1, 2\n"
        "msg:\t.quad\t7\n"
        "cnt:\t.long\t5\n", body);
}

struct Assembled {
    std::unique_ptr<LoweringRun>   run;
    std::optional<AssembledModule> module;
    std::string                    assembleMessages;
    // `assemble` keeps a slot for every function even when an instruction in
    // it is refused (the function's bytes are dropped), so a refusal shows up
    // here and not in `ok()`.
    std::size_t                    assembleErrors = 0;

    // Lowered, assembled, and nothing refused on the way.
    [[nodiscard]] bool clean() const {
        return module.has_value() && module->ok() && assembleErrors == 0;
    }
};

Assembled assembleArm(std::string const& source,
                      std::optional<ObjectFormatKind> kind) {
    Assembled a;
    a.run = lowerAsmText(shippedDialectDoc("asm-arm64-gas"), source, "arm64", kind);
    if (!a.run->module.has_value()) return a;
    auto const& lir = a.run->module->lir;
    DiagnosticReporter rep;
    std::vector<MirInstId> lirToMir(lir.instCount(), InvalidMirInst);
    a.module = assemble(lir, *a.run->target, lirToMir, rep);
    a.assembleErrors = rep.errorCount();
    for (auto const& d : rep.all()) a.assembleMessages += d.actual + "\n";
    return a;
}

std::vector<std::uint32_t> wordsOf(AssembledFunction const& fn) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i + 4 <= fn.bytes.size(); i += 4) {
        out.push_back(static_cast<std::uint32_t>(fn.bytes[i])
                      | static_cast<std::uint32_t>(fn.bytes[i + 1]) << 8
                      | static_cast<std::uint32_t>(fn.bytes[i + 2]) << 16
                      | static_cast<std::uint32_t>(fn.bytes[i + 3]) << 24);
    }
    return out;
}

std::uint32_t symbolOf(AsmTextModule const& m, std::string_view name) {
    for (auto const& s : m.symbols) {
        if (s.name == name) return s.symbol.v;
    }
    ADD_FAILURE() << "no module symbol named '" << name << "'";
    return 0;
}

std::uint32_t relocKind(TargetSchema const& t, std::string_view name) {
    auto const* r = t.relocationByName(name);
    EXPECT_NE(r, nullptr) << "the target declares no `" << name << "` relocation";
    return r != nullptr ? r->kind.v : 0u;
}

// One instruction line lowered for `kind`: its first word and the relocations
// at offset 0, or the reason it failed.
struct OneLine {
    bool                          ok = false;
    std::uint32_t                 word = 0;
    std::vector<Relocation>       relocs;
    std::string                   why;
    std::unique_ptr<LoweringRun>  run;
};

OneLine lowerLine(std::string_view line, std::optional<ObjectFormatKind> kind) {
    OneLine out;
    auto a = assembleArm(program(std::format("\t{}\n\tret\n", line)), kind);
    out.why = parseMessages(*a.run) + messages(*a.run) + a.assembleMessages;
    if (!parsedCleanly(*a.run) || !a.clean()) {
        out.run = std::move(a.run);
        return out;
    }
    auto const& fn = a.module->functions.at(0);
    auto const w = wordsOf(fn);
    if (w.empty()) {
        out.why += "(no words)";
        out.run = std::move(a.run);
        return out;
    }
    out.ok   = true;
    out.word = w.front();
    for (auto const& r : fn.relocations) {
        if (r.offset == 0) out.relocs.push_back(r);
    }
    out.run = std::move(a.run);
    return out;
}

void expectLine(std::string_view line, std::optional<ObjectFormatKind> kind,
                std::uint32_t wantWord, std::string_view wantReloc,
                std::string_view wantSymbol, std::int64_t wantAddend) {
    auto const got = lowerLine(line, kind);
    ASSERT_TRUE(got.ok) << "`" << line << "` did not lower: " << got.why;
    EXPECT_EQ(got.word, wantWord) << std::format("`{}`: 0x{:08X}, want 0x{:08X}",
                                                 line, got.word, wantWord);
    ASSERT_EQ(got.relocs.size(), 1u) << "`" << line << "`";
    EXPECT_EQ(got.relocs[0].kind.v, relocKind(*got.run->target, wantReloc))
        << "`" << line << "` wants " << wantReloc;
    EXPECT_EQ(got.relocs[0].target.v, symbolOf(*got.run->module, wantSymbol))
        << "`" << line << "`";
    EXPECT_EQ(got.relocs[0].addend, wantAddend) << "`" << line << "`";
}

void expectRefused(std::string_view line, std::optional<ObjectFormatKind> kind,
                   std::vector<std::string_view> const& mentions) {
    auto const got = lowerLine(line, kind);
    EXPECT_FALSE(got.ok) << "`" << line << "` was accepted";
    for (auto const m : mentions) {
        EXPECT_NE(got.why.find(m), std::string::npos)
            << "`" << line << "`'s refusal does not name '" << m << "':\n" << got.why;
    }
}

}  // namespace

// ══ ELF: gas's and clang's spellings ══════════════════════════════════════════

TEST(AsmArm64AddressForms, AdrpReadsABareSymbolAsItsPageOnElf) {
    // gas and clang for ELF read the operand of `adrp` as the page with no
    // operator: the row's `impliedSymbolPart`.
    expectLine("adrp\tx0, msg", kElf, 0x90000000u, "adr_prel_pg_hi21", "msg", 0);
    expectLine("adrp\tx0, :pg_hi21:msg", kElf, 0x90000000u, "adr_prel_pg_hi21", "msg", 0);
    expectLine("adrp\tx3, :PG_HI21:msg", kElf, 0x90000003u, "adr_prel_pg_hi21", "msg", 0);
}

TEST(AsmArm64AddressForms, Lo12IsThePageOffsetOfAnAddOnElf) {
    expectLine("add\tx0, x0, :lo12:msg", kElf, 0x91000000u, "add_abs_lo12_nc", "msg", 0);
    expectLine("add\tx0, x0, #:lo12:msg", kElf, 0x91000000u, "add_abs_lo12_nc", "msg", 0);
    expectLine("add\tw0, w0, :lo12:msg", kElf, 0x11000000u, "add_abs_lo12_nc", "msg", 0);
    // gas folds an operator's case (`:LO12:`), as it folds every spelling.
    expectLine("add\tx1, x2, :LO12:msg", kElf, 0x91000041u, "add_abs_lo12_nc", "msg", 0);
}

TEST(AsmArm64AddressForms, ALoadOrStoreTakesThePageOffsetScaledByItsAccess) {
    struct Case { std::string_view line; std::uint32_t word; std::string_view reloc; };
    Case const cases[] = {
        {"ldr\tx1, [x0, :lo12:msg]",  0xF9400001u, "ldst64_abs_lo12_nc"},
        {"ldr\tx1, [x0, #:lo12:msg]", 0xF9400001u, "ldst64_abs_lo12_nc"},
        {"ldr\tw1, [x0, :lo12:cnt]",  0xB9400001u, "ldst32_abs_lo12_nc"},
        {"str\tx1, [x0, :lo12:msg]",  0xF9000001u, "ldst64_abs_lo12_nc"},
        {"str\tw1, [x0, :lo12:cnt]",  0xB9000001u, "ldst32_abs_lo12_nc"},
        {"ldr\td0, [x0, :lo12:msg]",  0xFD400000u, "ldst64_abs_lo12_nc"},
        {"ldr\ts0, [x0, :lo12:cnt]",  0xBD400000u, "ldst32_abs_lo12_nc"},
        {"ldr\tq0, [x0, :lo12:vec]",  0x3DC00000u, "ldst128_abs_lo12_nc"},
        {"ldr\th0, [x0, :lo12:cnt]",  0x7D400000u, "ldst16_abs_lo12_nc"},
        {"ldr\tb0, [x0, :lo12:cnt]",  0x3D400000u, "ldst8_abs_lo12_nc"},
    };
    for (auto const& c : cases) {
        std::string_view const sym =
            c.line.find("vec") != std::string_view::npos ? "vec"
            : c.line.find("cnt") != std::string_view::npos ? "cnt" : "msg";
        expectLine(c.line, kElf, c.word, c.reloc, sym, 0);
    }
}

TEST(AsmArm64AddressForms, AConstantAfterTheNameIsTheAddend) {
    expectLine("adr\tx0, msg+8", kElf, 0x10000000u, "adr_prel_lo21", "msg", 8);
    expectLine("adrp\tx0, msg+8", kElf, 0x90000000u, "adr_prel_pg_hi21", "msg", 8);
    expectLine("add\tx0, x0, :lo12:msg+8", kElf, 0x91000000u, "add_abs_lo12_nc", "msg", 8);
    expectLine("ldr\tx1, [x0, :lo12:msg-8]", kElf, 0xF9400001u, "ldst64_abs_lo12_nc", "msg", -8);
}

TEST(AsmArm64AddressForms, AdrIsOneWordWithTheLo21Relocation) {
    // ★ `adr` spelled the two-word ADRP+ADD `lea` macro before; both references
    // write one ADR word. The control is the whole function: adr + ret.
    auto a = assembleArm(program("\tadr\tx0, msg\n\tret\n"), kElf);
    ASSERT_TRUE(a.clean())
        << parseMessages(*a.run) << messages(*a.run) << a.assembleMessages;
    auto const w = wordsOf(a.module->functions.at(0));
    ASSERT_EQ(w.size(), 2u) << "`adr x0, msg` must be ONE word";
    EXPECT_EQ(w[0], 0x10000000u);
    EXPECT_EQ(w[1], 0xD65F03C0u);
    ASSERT_EQ(a.module->functions.at(0).relocations.size(), 1u);
    EXPECT_EQ(a.module->functions.at(0).relocations[0].kind.v,
              relocKind(*a.run->target, "adr_prel_lo21"));
    // `adr xzr, main` is gas's 0x1000001F: ADR's Rd reads 31 as the zero
    // register (the `lea` macro had to refuse it — its ADD word reads SP).
    expectLine("adr\txzr, main", kElf, 0x1000001Fu, "adr_prel_lo21", "main", 0);
}

// ══ a block of this function, and the location counter ═══════════════════════

TEST(AsmArm64AddressForms, AdrOfALabelOfItsOwnFunctionIsResolvedAtAssembly) {
    // gas and clang (ELF and Darwin) resolve these at assemble time: no
    // relocation. `b 1f` keeps `1:` from being fallen into, so every word here
    // is the references' own.
    auto a = assembleArm(program("\tadr\tx1, 1f\n\tadr\tx2, 1f+4\n\tb\t1f\n"
                                 "1:\tnop\n\tret\n"),
                         kElf);
    ASSERT_TRUE(a.clean())
        << parseMessages(*a.run) << messages(*a.run) << a.assembleMessages;
    auto const& fn = a.module->functions.at(0);
    auto const w = wordsOf(fn);
    ASSERT_EQ(w.size(), 5u);
    EXPECT_EQ(w[0], 0x10000061u) << "adr x1, 1f: `1:` is 12 bytes on";
    EXPECT_EQ(w[1], 0x10000062u) << "adr x2, 1f+4: 8 bytes to `1:`, plus 4";
    EXPECT_EQ(w[2], 0x14000001u);
    EXPECT_TRUE(fn.relocations.empty())
        << "a label of the same function needs no relocation";
}

TEST(AsmArm64AddressForms, TheLocationCounterIsTheAddressOfItsOwnLine) {
    // `.` is the address of the `adr` itself, and `. ± N` a byte distance from
    // it inside the run of source lines around it: gas's words exactly
    // (✔MEASURED 2026-09-23, gas 2.42).
    struct Case { std::string_view body; std::size_t at; std::uint32_t word; };
    Case const cases[] = {
        {"\tadr\tx7, .\n\tret\n",                 0, 0x10000007u},
        {"\tadr\tx7, .+8\n\tnop\n\tnop\n\tret\n", 0, 0x10000047u},
        {"\tnop\n\tadr\tx7, .-4\n\tret\n",        1, 0x10FFFFE7u},
    };
    for (auto const& c : cases) {
        auto a = assembleArm(program(c.body), kElf);
        ASSERT_TRUE(a.clean())
            << c.body << parseMessages(*a.run) << messages(*a.run) << a.assembleMessages;
        auto const& fn = a.module->functions.at(0);
        auto const w = wordsOf(fn);
        ASSERT_GT(w.size(), c.at);
        EXPECT_EQ(w[c.at], c.word) << c.body;
        EXPECT_TRUE(fn.relocations.empty()) << c.body;
    }
    // ★ MID-BLOCK, `.` IS THE LINE ITSELF AND BEGINS NO BLOCK: the words are
    // gas's exactly (✔MEASURED 2026-09-23, gas 2.42: d503201f 1000000a 10ffffeb
    // d65f03c0). A block begun at each `.` line put a fall-through jump in
    // front of it on the first cut, so `.-4` named that jump instead of the
    // `adr` before it ([[D-ASM-FALLTHROUGH-INTO-A-LABEL-EMITS-A-JUMP]]).
    auto a = assembleArm(program("\tnop\n\tadr\tx10, .\n\tadr\tx11, .-4\n\tret\n"), kElf);
    ASSERT_TRUE(a.clean())
        << parseMessages(*a.run) << messages(*a.run) << a.assembleMessages;
    auto const& fn = a.module->functions.at(0);
    EXPECT_EQ(wordsOf(fn),
              (std::vector<std::uint32_t>{0xD503201Fu, 0x1000000Au, 0x10FFFFEBu,
                                          0xD65F03C0u}));
    EXPECT_TRUE(fn.relocations.empty());
}

TEST(AsmArm64AddressForms, AConstantThatLeavesTheRunOfSourceCodeIsRefused) {
    // ★★ `. + N` IS A BYTE DISTANCE IN THE REFERENCE'S LAYOUT, which this
    // build reproduces only across source-for-source code: inside one block,
    // short of its terminator. ✔MEASURED 2026-09-23: `nop; 1: adr x7, .-4` put
    // x7 on a jump this build synthesized in front of `1:`, where gas 2.42 puts
    // it on the `nop` ([[D-ASM-FALLTHROUGH-INTO-A-LABEL-EMITS-A-JUMP]]).
    for (std::string_view const body :
         {"\tnop\n1:\tadr\tx7, .-4\n\tret\n",        // back across a label
          "\tadr\tx7, .+4\n1:\tret\n",                // onto a synthesized jump
          "\tadr\tx7, .+0xffffc\n\tret\n",            // out of the function
          "\tadr\tx2, 1f+4\n\tb\t1f\n1:\tret\n"}) {   // past a label's run
        auto a = assembleArm(program(body), kElf);
        ASSERT_TRUE(a.run->module.has_value())
            << body << parseMessages(*a.run) << messages(*a.run);
        ASSERT_TRUE(a.module.has_value());
        EXPECT_FALSE(a.clean()) << body;
        EXPECT_NE(a.assembleMessages.find("reaches outside the run of source "
                                          "instructions"),
                  std::string::npos)
            << body << a.assembleMessages;
    }
    // CONTROL: inside the run, a label's run included, and onto a terminator a
    // source line wrote — gas's words exactly (✔MEASURED 2026-09-23: `adr x7,
    // .+8` = 0x10000047; `adr x7, .+4` = 0x10000027).
    auto a = assembleArm(program("\tadr\tx7, .+8\n\tnop\n\tnop\n\tret\n"), kElf);
    ASSERT_TRUE(a.clean()) << parseMessages(*a.run) << messages(*a.run)
                           << a.assembleMessages;
    EXPECT_EQ(wordsOf(a.module->functions.at(0)).at(0), 0x10000047u);
    auto r = assembleArm(program("\tadr\tx7, .+4\n\tret\n"), kElf);
    ASSERT_TRUE(r.clean()) << parseMessages(*r.run) << messages(*r.run)
                           << r.assembleMessages;
    EXPECT_EQ(wordsOf(r.module->functions.at(0)).at(0), 0x10000027u);
    auto b = assembleArm(program("\tadr\tx2, 1f+4\n\tb\t1f\n1:\tnop\n\tnop\n\tret\n"),
                         kElf);
    ASSERT_TRUE(b.clean()) << parseMessages(*b.run) << messages(*b.run)
                           << b.assembleMessages;
    EXPECT_EQ(wordsOf(b.module->functions.at(0)).at(0), 0x10000062u)
        << "adr x2, 1f+4: `1:` is 8 bytes on, plus 4";
}

TEST(AsmArm64AddressForms, AConstantAddedToCodeOutsideTheAdrFieldIsRefused) {
    // Outside the field that resolves a location of its own function (where
    // the run is checked), the byte N past a code label is a relocation's
    // addend in the reference's code layout: refused by name, on an entry
    // label, on a page of one, and in a data slot.
    expectRefused("adr\tx0, main+4", kElf, {"'main' plus 4", "CODE"});
    expectRefused("adrp\tx0, main+4096", kElf, {"'main' plus 4096", "CODE"});
    auto a = assembleArm(program("\tret\n") + "slot:\t.quad\tmain+8\n", kElf);
    ASSERT_TRUE(parsedCleanly(*a.run)) << parseMessages(*a.run);
    EXPECT_FALSE(a.run->module.has_value());
    EXPECT_NE(messages(*a.run).find("'main' plus 8"), std::string::npos)
        << messages(*a.run);
    // CONTROL: a data label plus a constant is the reference's everywhere.
    expectLine("adr\tx0, msg+8", kElf, 0x10000000u, "adr_prel_lo21", "msg", 8);
}

TEST(AsmArm64AddressForms, TheLocationCounterNamesNoSymbolAnywhereElse) {
    // It is this instruction's address: a field that takes a location of its
    // own function, a branch target, or a data slot takes it. A page offset
    // of it or a displacement against it would need a relocation against the
    // section at this offset, which this build does not write — refused by
    // name, never an import called `.`.
    expectRefused("add\tx0, x0, :lo12:.", kElf, {"location counter", "page offset"});
    expectRefused("ldr\tx1, [x0, :lo12:.]", kElf, {"location counter"});
}

TEST(AsmArm64AddressForms, ABranchToTheLocationCounterBranchesToItself) {
    // `b .` is gas's 0x14000000 — an infinite loop on its own address.
    auto a = assembleArm(program("\tcbz\tx0, 1f\n\tb\t.\n1:\tret\n"), kElf);
    ASSERT_TRUE(a.clean())
        << parseMessages(*a.run) << messages(*a.run) << a.assembleMessages;
    bool sawSelf = false;
    for (auto const word : wordsOf(a.module->functions.at(0))) {
        if (word == 0x14000000u) sawSelf = true;
    }
    EXPECT_TRUE(sawSelf) << "`b .` must branch to its own address";
}

TEST(AsmArm64AddressForms, ADataSlotNamingTheLocationCounterIsItsOwnAddress) {
    // gas: `.quad .` is ABS64 against the item, at the slot's own offset.
    auto a = assembleArm(program("\tret\n") + "tbl:\t.quad\t0\n\t.quad\t.\n", kElf);
    ASSERT_TRUE(a.run->module.has_value())
        << parseMessages(*a.run) << messages(*a.run);
    bool found = false;
    for (auto const& d : a.run->module->dataItems) {
        for (auto const& r : d.relocations) {
            if (r.target.v != symbolOf(*a.run->module, "tbl")) continue;
            EXPECT_EQ(r.offset, 8u);
            EXPECT_EQ(r.addend, 8) << "the slot is 8 bytes into `tbl`";
            found = true;
        }
    }
    EXPECT_TRUE(found) << "`.quad .` must relocate against its own item";
}

// ══ Mach-O: clang for Darwin's spellings ══════════════════════════════════════

TEST(AsmArm64AddressForms, MachOSpellsThePageAndItsOffsetWithTheSameWords) {
    expectLine("adrp\tx0, msg@PAGE", kMachO, 0x90000000u, "adr_prel_pg_hi21", "msg", 0);
    expectLine("add\tx0, x0, msg@PAGEOFF", kMachO, 0x91000000u, "add_abs_lo12_nc", "msg", 0);
    expectLine("add\tx0, x0, #msg@PAGEOFF", kMachO, 0x91000000u, "add_abs_lo12_nc", "msg", 0);
    expectLine("ldr\tw1, [x0, cnt@PAGEOFF]", kMachO, 0xB9400001u, "ldst32_abs_lo12_nc", "cnt", 0);
    expectLine("ldr\tq0, [x0, vec@PAGEOFF]", kMachO, 0x3DC00000u, "ldst128_abs_lo12_nc", "vec", 0);
    // clang takes the constant on either side of the operator, and folds case.
    expectLine("adrp\tx0, msg@PAGE+8", kMachO, 0x90000000u, "adr_prel_pg_hi21", "msg", 8);
    expectLine("adrp\tx0, msg+8@PAGE", kMachO, 0x90000000u, "adr_prel_pg_hi21", "msg", 8);
    expectLine("add\tx0, x0, msg@pageoff", kMachO, 0x91000000u, "add_abs_lo12_nc", "msg", 0);
}

// ══ refusals, each by name ═══════════════════════════════════════════════════

TEST(AsmArm64AddressForms, AnOperatorOfTheOtherFormatIsRefusedNamingThisFormatsOwn) {
    expectRefused("add\tx0, x0, msg@PAGEOFF", kElf,
                  {"@PAGEOFF", "macho", "elf", "`:lo12:`"});
    expectRefused("ldr\tx1, [x0, msg@PAGEOFF]", kElf, {"@PAGEOFF", "`:lo12:`"});
    expectRefused("add\tx0, x0, :lo12:msg", kMachO,
                  {":lo12:", "elf", "macho", "`@PAGEOFF`"});
    expectRefused("adrp\tx0, :pg_hi21:msg", kMachO, {":pg_hi21:", "`@PAGE`"});
}

TEST(AsmArm64AddressForms, ABareAdrpOperandIsRefusedWhereNoPageIsImplied) {
    // clang for Darwin: "ADR/ADRP relocations must be GOT relative".
    expectRefused("adrp\tx0, msg", kMachO, {"adrp", "msg", "macho", "`@PAGE`"});
}

TEST(AsmArm64AddressForms, AnOperatorOnTheWrongSideIsRefusedByName) {
    expectRefused("add\tx0, x0, msg:lo12:", kElf, {":lo12:", "after", "before"});
    expectRefused("add\tx0, x0, @PAGEOFF msg", kMachO, {"@PAGEOFF", "before", "after"});
}

TEST(AsmArm64AddressForms, AnUndeclaredOperatorIsRefusedByName) {
    // `:got:` is a relocation of its own (ADR_GOT_PAGE), not a spelling of the
    // page: refused, naming the operators this dialect does declare.
    expectRefused("adrp\tx0, :got:msg", kElf, {":got:", "`:lo12:`", "`@PAGE`"});
}

TEST(AsmArm64AddressForms, NoFormatStatedReadsNoOperator) {
    expectRefused("add\tx0, x0, :lo12:msg", std::nullopt,
                  {":lo12:", "states no object format"});
}

TEST(AsmArm64AddressForms, AnOffsetFieldNamingAWholeSymbolIsRefused) {
    // clang refuses `ldr x1, [x0, cnt]`; gas writes cnt's SECTION offset with
    // no relocation — the address in no program. Refused, asking for a part.
    expectRefused("ldr\tx1, [x0, msg]", kElf, {"msg", "PART of an address"});
    // And an `add` of a whole symbol has no variant: nothing encodes it.
    expectRefused("add\tx0, x0, msg", kElf, {"add"});
}

TEST(AsmArm64AddressForms, AnAdrTakesTheWholeAddressAndNoPartOfIt) {
    // ✔MEASURED 2026-09-23: gas 2.42 and clang 18 refuse `adr x0, :lo12:msg`,
    // and clang for Darwin `adr x0, msg@PAGEOFF`: ADR's field holds the whole
    // PC-relative address. The part is the operand's own, and no `adr` variant
    // states one (`guard.symbolPart`), so the election finds none.
    expectRefused("adr\tx0, :lo12:msg", kElf, {"adr"});
    expectRefused("adr\tx0, :pg_hi21:msg", kElf, {"adr"});
    expectRefused("adr\tx0, msg@PAGEOFF", kMachO, {"adr"});
}

TEST(AsmArm64AddressForms, ARegisterTakesNoPartAndNoAddend) {
    // A memory BASE is always a register, so a constant after it is refused
    // by name; a register where the offset goes is the register-offset form,
    // which this memory operand does not decode.
    expectRefused("ldr\tx1, [x0+8]", kElf, {"'x0' names a register"});
    expectRefused("ldr\tx1, [x0, x2]", kElf, {"'x2' names a register", "register offset"});
    // `x1+8` where a register or a symbol may stand is the SYMBOL `x1` plus 8
    // (a register takes no constant), which no `add` variant encodes; gas and
    // clang refuse the line too (✔MEASURED 2026-09-23).
    expectRefused("add\tx0, x0, x1+8", kElf, {"add"});
}

TEST(AsmArm64AddressForms, ANameSpelledLikeARegisterIsASymbolWhenItCarriesAPart) {
    // ✔MEASURED 2026-09-23, one line per file: gas 2.42 and clang 18 for ELF
    // assemble these against a data label named `b8` — the byte register's
    // spelling — and clang 18 for Darwin the `@PAGEOFF` one. A register takes
    // no address-part operator and no constant, so a name carrying one is the
    // symbol it spells.
    auto const source = [](std::string_view line) {
        return "\t.text\n\t.globl\tmain\n\t.type\tmain, %function\nmain:\n\t"
             + std::string{line} + "\n\tret\n\t.data\nb8:\t.quad\t1\n";
    };
    struct Case {
        std::string_view line; std::optional<ObjectFormatKind> kind;
        std::uint32_t word; std::string_view reloc; std::int64_t addend;
    };
    Case const cases[] = {
        {"add\tx0, x0, :lo12:b8",   kElf,   0x91000000u, "add_abs_lo12_nc",    0},
        {"ldr\tw0, [x0, :lo12:b8]", kElf,   0xB9400000u, "ldst32_abs_lo12_nc", 0},
        {"adr\tx0, b8+8",           kElf,   0x10000000u, "adr_prel_lo21",      8},
        {"add\tx0, x0, b8@PAGEOFF", kMachO, 0x91000000u, "add_abs_lo12_nc",    0},
    };
    for (auto const& c : cases) {
        auto a = assembleArm(source(c.line), c.kind);
        ASSERT_TRUE(a.clean())
            << c.line << "\n" << parseMessages(*a.run) << messages(*a.run)
            << a.assembleMessages;
        auto const& fn = a.module->functions.at(0);
        EXPECT_EQ(wordsOf(fn).at(0), c.word) << c.line;
        ASSERT_EQ(fn.relocations.size(), 1u) << c.line;
        EXPECT_EQ(fn.relocations[0].kind.v, relocKind(*a.run->target, c.reloc)) << c.line;
        EXPECT_EQ(fn.relocations[0].target.v, symbolOf(*a.run->module, "b8")) << c.line;
        EXPECT_EQ(fn.relocations[0].addend, c.addend) << c.line;
    }
}

TEST(AsmArm64AddressForms, ABareNumericOffsetNeedsNoSigil) {
    // gas and clang both take `[x0, 8]` as `[x0, #8]`: 0xF9400401.
    auto const got = lowerLine("ldr\tx1, [x0, 8]", kElf);
    ASSERT_TRUE(got.ok) << got.why;
    EXPECT_EQ(got.word, 0xF9400401u);
    EXPECT_TRUE(got.relocs.empty());
}

TEST(AsmArm64AddressForms, ANumericOffsetNeverReachesThePageOffsetForm) {
    // An offset the scaled field cannot hold falls back to the unscaled `ldur`
    // (gas 2.42: `ldr x1, [x0, #3]` = 0xF8403001) — never to the page-offset
    // variant, whose field is the linker's and which states `symbolPart`: it
    // matches only an operand that names that part of a symbol.
    auto const got = lowerLine("ldr\tx1, [x0, #3]", kElf);
    ASSERT_TRUE(got.ok) << got.why;
    EXPECT_EQ(got.word, 0xF8403001u);
    EXPECT_TRUE(got.relocs.empty());
}

TEST(AsmArm64AddressForms, APartOfAnInteriorLabelIsRefusedByName) {
    // ⓘ gas and clang for ELF take `add x0, x0, :lo12:1f` (ADD_ABS_LO12_NC
    // against the section); this build addresses a label of its own function
    // only in a field that takes the block. Refused loudly, naming the label —
    // the gas-surface row lists it.
    expectRefused("add\tx0, x0, :lo12:1f\n1:\tnop", kElf, {"'1f'", "a label inside this function"});
}

// ══ the dialect document ══════════════════════════════════════════════════════

namespace {

std::string loadMessages(std::string_view language,
                         void (*edit)(nlohmann::json&)) {
    auto doc = shippedDialectDoc(language);
    edit(doc);
    auto const r = GrammarSchema::loadFromText(doc.dump(), std::string{language});
    std::string out;
    if (!r.has_value()) {
        for (auto const& e : r.error()) out += e.path + ": " + e.message + "\n";
        if (out.empty()) out = "(refused, no message)";
    }
    return out;
}

}  // namespace

TEST(AsmArm64AddressForms, EveryShippedDialectLoadsAndStatesTheRole) {
    for (std::string_view const lang : {"asm-arm64-gas", "asm-x86_64-att"}) {
        EXPECT_EQ(loadMessages(lang, [](nlohmann::json&) {}), "") << lang;
        auto const doc = shippedDialectDoc(lang);
        EXPECT_TRUE(doc["assembly"]["operandForms"].contains("symbolPart")) << lang;
    }
}

TEST(AsmArm64AddressForms, ADialectThatDoesNotMentionTheRoleIsRefusedByName) {
    for (std::string_view const lang : {"asm-arm64-gas", "asm-x86_64-att"}) {
        auto const msg = loadMessages(lang, [](nlohmann::json& d) {
            d["assembly"]["operandForms"].erase("symbolPart");
            d["assembly"].erase("symbolParts");
        });
        EXPECT_NE(msg.find("'symbolPart' is UNMENTIONED"), std::string::npos)
            << lang << ":\n" << msg;
    }
}

TEST(AsmArm64AddressForms, AFormatKindNoBuildCanHaveIsRefused) {
    auto const bogus = loadMessages("asm-arm64-gas", [](nlohmann::json& d) {
        d["assembly"]["symbolParts"][0]["formatKinds"] = nlohmann::json::array({"elfx"});
    });
    EXPECT_NE(bogus.find("'elfx' is not an object-format kind"), std::string::npos) << bogus;
    auto const sentinel = loadMessages("asm-arm64-gas", [](nlohmann::json& d) {
        d["assembly"]["symbolParts"][0]["formatKinds"] = nlohmann::json::array({"unknown"});
    });
    EXPECT_NE(sentinel.find("'unknown' is not an object-format kind"), std::string::npos)
        << sentinel;
    auto const implied = loadMessages("asm-arm64-gas", [](nlohmann::json& d) {
        for (auto& row : d["assembly"]["instructions"]) {
            if (row.contains("impliedSymbolPart")) {
                row["impliedSymbolPart"]["formatKinds"] = nlohmann::json::array({"coff"});
            }
        }
    });
    EXPECT_NE(implied.find("'coff' is not an object-format kind"), std::string::npos)
        << implied;
}

TEST(AsmArm64AddressForms, TheRoleAndItsSpellingsComeTogether) {
    auto const noList = loadMessages("asm-arm64-gas", [](nlohmann::json& d) {
        d["assembly"].erase("symbolParts");
    });
    EXPECT_NE(noList.find("declares no spelling"), std::string::npos) << noList;
    auto const noRole = loadMessages("asm-arm64-gas", [](nlohmann::json& d) {
        d["assembly"]["operandForms"]["symbolPart"] = nullptr;
    });
    EXPECT_NE(noRole.find("binds no rule to parse them"), std::string::npos) << noRole;
}

TEST(AsmArm64AddressForms, ASpellingIsDeclaredOnceAndNamesAPart) {
    auto const dup = loadMessages("asm-arm64-gas", [](nlohmann::json& d) {
        auto row = d["assembly"]["symbolParts"][0];
        row["spelling"] = ":LO12:";
        d["assembly"]["symbolParts"].push_back(row);
    });
    EXPECT_NE(dup.find("declared twice"), std::string::npos) << dup;
    auto const whole = loadMessages("asm-arm64-gas", [](nlohmann::json& d) {
        d["assembly"]["symbolParts"][0]["part"] = "whole";
    });
    EXPECT_NE(whole.find("'part' must be"), std::string::npos) << whole;
    auto const side = loadMessages("asm-arm64-gas", [](nlohmann::json& d) {
        d["assembly"]["symbolParts"][0]["position"] = "inside";
    });
    EXPECT_NE(side.find("symbolParts"), std::string::npos) << side;
}

// ══ an inline-asm template reads the same operators under the same key ══════

namespace {

struct TemplateOutcome {
    bool        parsed  = false;
    bool        lowered = false;
    std::string diagnostics;
};

TemplateOutcome lowerArmTemplate(std::string_view text,
                                 std::optional<ObjectFormatKind> kind) {
    TemplateOutcome out;
    auto grammar = GrammarSchema::loadFromText(shippedDialectDoc("asm-arm64-gas").dump(),
                                               "asm-arm64-gas");
    auto target = TargetSchema::loadShipped("arm64");
    if (!grammar.has_value() || !target.has_value()) {
        ADD_FAILURE() << "the shipped dialect or target did not load";
        return out;
    }
    DiagnosticReporter rep;
    auto tree = parseAsmTemplateText(std::string{text}, "<template>", *grammar,
                                     AsmTemplateSurface::Extended,
                                     DiagnosticBudget::libraryDefault(), rep);
    out.parsed = tree.has_value();
    if (!out.parsed) {
        for (auto const& d : rep.all()) out.diagnostics += d.actual + "\n";
        return out;
    }
    LirBuilder builder{**target};
    builder.addFunction(SymbolId{1});
    LirBlockId const entry = builder.createBlock();
    builder.beginBlock(entry);
    AsmOperandBinding ob;
    ob.spelling  = "%0";
    ob.regClass  = LirRegClass::GPR;
    ob.widthBits = 64;
    ob.reg       = makePhysicalReg(*(*target)->registerByName("x9"), LirRegClass::GPR);
    std::vector<AsmOperandBinding> bindings{ob};
    out.lowered = lowerAsmTemplateToLirRun(*tree, **grammar, **target, bindings,
                                           builder, rep, {}, nullptr, kind);
    for (auto const& d : rep.all()) out.diagnostics += d.actual + "\n";
    return out;
}

}  // namespace

TEST(AsmArm64AddressForms, ATemplateTakesTheLocationCounterAsItsOwnAddress) {
    // The location counter is the instruction's own address, known to the
    // encoder, so a template takes it with no label model at all — on either
    // format (clang for Darwin assembles `adr x7, .` too, ✔MEASURED
    // 2026-09-23).
    for (auto const kind : {kElf, kMachO}) {
        auto const t = lowerArmTemplate("adr %0, .+8", kind);
        ASSERT_TRUE(t.parsed) << t.diagnostics;
        EXPECT_TRUE(t.lowered) << t.diagnostics;
    }
}

TEST(AsmArm64AddressForms, ATemplateReadsAnOperatorOnlyUnderItsOwnFormat) {
    // ⓘ A template names no symbol's address yet (the template host refuses
    // every one), so what differs by format is WHICH refusal: on ELF `:lo12:`
    // is read and the name is refused as a template symbol; on Mach-O the
    // operator itself is refused, naming `@PAGEOFF`. `@PAGEOFF` the reverse.
    auto const elfLo12 = lowerArmTemplate("add %0, %0, :lo12:msg", kElf);
    ASSERT_TRUE(elfLo12.parsed) << elfLo12.diagnostics;
    EXPECT_FALSE(elfLo12.lowered);
    EXPECT_EQ(elfLo12.diagnostics.find("spells the page offset"), std::string::npos)
        << "ELF reads `:lo12:`:\n" << elfLo12.diagnostics;

    auto const machoLo12 = lowerArmTemplate("add %0, %0, :lo12:msg", kMachO);
    ASSERT_TRUE(machoLo12.parsed) << machoLo12.diagnostics;
    EXPECT_FALSE(machoLo12.lowered);
    EXPECT_NE(machoLo12.diagnostics.find("`@PAGEOFF`"), std::string::npos)
        << machoLo12.diagnostics;

    auto const machoPageoff = lowerArmTemplate("add %0, %0, msg@PAGEOFF", kMachO);
    ASSERT_TRUE(machoPageoff.parsed) << machoPageoff.diagnostics;
    EXPECT_FALSE(machoPageoff.lowered);
    EXPECT_EQ(machoPageoff.diagnostics.find("spells the page offset"), std::string::npos)
        << "Mach-O reads `@PAGEOFF`:\n" << machoPageoff.diagnostics;

    auto const elfPageoff = lowerArmTemplate("add %0, %0, msg@PAGEOFF", kElf);
    ASSERT_TRUE(elfPageoff.parsed) << elfPageoff.diagnostics;
    EXPECT_FALSE(elfPageoff.lowered);
    EXPECT_NE(elfPageoff.diagnostics.find("`:lo12:`"), std::string::npos)
        << elfPageoff.diagnostics;
}
