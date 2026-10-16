// P69 (lane `xa`) — D-ASM-TEMPLATE-CANNOT-NAME-A-SYMBOL-OF-THE-EMBEDDING-PROGRAM, the ASM HALF:
// an assembly template names symbols of the program that embeds it (`leaq g(%%rip)`, `adrp x9, g` +
// `add x9, x9, :lo12:g`, `adrp x9, _g@PAGE`, `call f`, `bl f`), and every reference compiler builds and
// runs that (✔MEASURED 2026-10-15: gcc 13.3.0 and clang 18.1.3 on x86_64 ELF, MinGW gcc 13.2.0 on PE,
// Apple clang 21 on arm64 Mach-O, aarch64 gcc 13.3.0 on arm64 ELF; one source and two, -O0 and -O2).
//
// The template engine holds no name table: its CALLER answers through `AsmTemplateProgramNames` — the
// symbol the embedding unit resolved a spelling to, whether that definition is code, and the import of a
// name the unit does not define. These are the INTERFACE's claims, read against a test double that
// stands for the caller: what the engine asks, and what it does with each answer. They do not close the
// row — it closes when the real program's images run, with the lowering of the statement answering
// (the row's MIR half).
#include "asm/asm.hpp"
#include "asm/asm_template_to_lir.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

struct ShippedPair {
    std::string_view dialect;   // the document STEM
    std::string_view target;
    std::string_view reg;       // the register `%0` is pinned to, so the run reaches bytes
};

constexpr ShippedPair kX86{"asm-x86_64-att", "x86_64", "rax"};
constexpr ShippedPair kArm{"asm-arm64-gas", "arm64", "x9"};

[[nodiscard]] std::shared_ptr<GrammarSchema> loadDialect(std::string_view name) {
    auto pathR = findShippedConfig(ShippedConfigLocator{name, "sources", ".lang.json", "language",
                                                        DiagnosticCode::C_InvalidTargetName});
    if (!pathR.has_value()) throw std::runtime_error{"cannot locate dialect " + std::string{name}};
    std::ifstream      in{*pathR};
    std::ostringstream buf;
    buf << in.rdbuf();
    auto g = GrammarSchema::loadFromText(buf.str(), std::string{name});
    if (!g.has_value()) throw std::runtime_error{"dialect did not load: " + std::string{name}};
    return *g;
}

// THE CALLER, as a test double: the names one unit defines (each a symbol, code or data), and every
// import the engine asked for, in order. An import is minted once per spelling, as the caller's rule is.
struct ProgramNames final : AsmTemplateProgramNames {
    struct Defined {
        SymbolId symbol;
        bool     code = false;
    };
    std::map<std::string, Defined, std::less<>>             defined;
    std::vector<std::pair<std::string, AsmTemplateNameUse>> asked;   // every importOf, in order
    std::map<std::string, SymbolId, std::less<>>            imports;
    std::uint32_t                                           next = 900;
    bool                                                    refuseImports = false;

    [[nodiscard]] std::optional<SymbolId> symbolDefinedAs(std::string_view s) const override {
        auto const it = defined.find(s);
        return it == defined.end() ? std::nullopt : std::optional<SymbolId>{it->second.symbol};
    }
    [[nodiscard]] bool definesCode(std::string_view s) const override {
        auto const it = defined.find(s);
        return it != defined.end() && it->second.code;
    }
    [[nodiscard]] SymbolId importOf(std::string const& s, AsmTemplateNameUse use) override {
        asked.emplace_back(s, use);
        if (refuseImports) return SymbolId{};
        auto const it = imports.find(s);
        if (it != imports.end()) return it->second;
        SymbolId const id{next++};
        imports.emplace(s, id);
        return id;
    }
};

[[nodiscard]] ProgramNames aUnitDefiningGAndF() {
    ProgramNames n;
    n.defined.emplace("g", ProgramNames::Defined{SymbolId{77}, false});
    n.defined.emplace("f", ProgramNames::Defined{SymbolId{78}, true});
    n.defined.emplace("_g", ProgramNames::Defined{SymbolId{79}, false});   // a Mach-O spelling, verbatim
    return n;
}

struct Run {
    bool                    parsed = false;
    bool                    ok     = false;
    std::string             messages;
    std::vector<Relocation> relocations;   // of the assembled template, when it lowered
    std::shared_ptr<TargetSchema> target;

    [[nodiscard]] std::vector<std::uint32_t> targets() const {
        std::vector<std::uint32_t> out;
        for (auto const& r : relocations) out.push_back(r.target.v);
        return out;
    }
};

// One template, lowered into a one-function builder with `%0` pinned to a real register; `labels` are
// `asm goto` label spellings, each its own block. The template's body is discarded on a refusal, as the
// real caller discards it.
[[nodiscard]] Run lower(ShippedPair const& p, std::string_view text, AsmTemplateProgramNames* names,
                        std::optional<ObjectFormatKind> kind, std::vector<std::string> const& labels = {}) {
    Run run;
    auto dialect = loadDialect(p.dialect);
    auto targetR = TargetSchema::loadShipped(std::string{p.target});
    if (!targetR.has_value()) throw std::runtime_error{"cannot load the shipped target"};
    run.target = *targetR;
    DiagnosticReporter rep;
    auto tree = parseAsmTemplateText(std::string{text}, "<template>", dialect, AsmTemplateSurface::Extended,
                                     DiagnosticBudget::libraryDefault(), rep);
    run.parsed = tree.has_value();
    if (run.parsed) {
        LirBuilder builder{*run.target};
        builder.addFunction(SymbolId{1});
        LirBlockId const entry = builder.createBlock();
        builder.beginBlock(entry);
        AsmOperandBinding b;
        b.spelling  = "%0";
        b.regClass  = LirRegClass::GPR;
        b.widthBits = 64;
        auto const ord = run.target->registerByName(std::string{p.reg});
        if (!ord.has_value()) throw std::runtime_error{"the target declares no " + std::string{p.reg}};
        b.reg = makePhysicalReg(*ord, LirRegClass::GPR);
        std::vector<AsmOperandBinding> bindings{b};
        std::vector<AsmLabelBinding>   labelBindings;
        std::vector<LirBlockId>        labelBlocks;
        for (auto const& s : labels) {
            LirBlockId const block = builder.createBlock();
            labelBlocks.push_back(block);
            labelBindings.push_back(AsmLabelBinding{s, block});
        }
        run.ok = lowerAsmTemplateToLirRun(*tree, *dialect, *run.target, bindings, builder, rep, labelBindings,
                                          nullptr, kind, names);
        if (run.ok) {
            auto const retOp = run.target->opcodeByMnemonic("ret");
            if (!retOp.has_value()) throw std::runtime_error{"the target has no `ret`"};
            builder.addReturn(*retOp, {});
            for (LirBlockId const block : labelBlocks) {
                builder.beginBlock(block);
                builder.addReturn(*retOp, {});
            }
            Lir                    lir = std::move(builder).finish();
            DiagnosticReporter     asmRep;
            std::vector<MirInstId> lirToMir(lir.instCount());
            auto const             mod = assemble(lir, *run.target, lirToMir, asmRep);
            if (mod.functions.size() == 1) run.relocations = mod.functions[0].relocations;
            for (auto const& d : asmRep.all()) run.messages += "[assemble] " + d.actual + "\n";
        }
    }
    for (auto const& d : rep.all()) run.messages += d.actual + "\n";
    return run;
}

[[nodiscard]] bool says(Run const& r, std::string_view words) {
    return r.messages.find(words) != std::string::npos;
}

}  // namespace

// ══ a name the embedding unit DEFINES is its symbol ═════════════════════════════════════════════════════

TEST(AsmTemplateProgramNames, ADisplacementRelativeToADefinedNameIsItsSymbol) {
    auto names = aUnitDefiningGAndF();
    auto const r = lower(kX86, "leaq g(%%rip), %0", &names, ObjectFormatKind::Elf);
    ASSERT_TRUE(r.parsed) << r.messages;
    ASSERT_TRUE(r.ok) << r.messages;
    EXPECT_EQ(r.targets(), (std::vector<std::uint32_t>{77u}))
        << "the displacement must be relative to the symbol the unit resolved `g` to:\n" << r.messages;
    EXPECT_TRUE(names.asked.empty()) << "a defined name is never imported";
}

TEST(AsmTemplateProgramNames, ACallOfADefinedFunctionIsItsSymbol) {
    for (auto const& [pair, text] : {std::pair{kX86, "call f"}, std::pair{kArm, "bl f"}}) {
        SCOPED_TRACE(text);
        auto names = aUnitDefiningGAndF();
        auto const r = lower(pair, text, &names, ObjectFormatKind::Elf);
        ASSERT_TRUE(r.parsed) << r.messages;
        ASSERT_TRUE(r.ok) << r.messages;
        EXPECT_EQ(r.targets(), (std::vector<std::uint32_t>{78u})) << r.messages;
        EXPECT_TRUE(names.asked.empty());
    }
}

// The page and the page offset of a defined name, each format's spelling — the template writes the
// on-binary name, so Mach-O's `_g` is asked about verbatim (the unit's resolution of it is the MIR half's).
TEST(AsmTemplateProgramNames, ThePageAndItsOffsetOfADefinedNameAreItsSymbolOnEachFormat) {
    struct Cell {
        char const*      text;
        ObjectFormatKind kind;
        std::uint32_t    symbol;
    };
    for (Cell const c : {Cell{"adrp %0, g\n\tadd %0, %0, :lo12:g", ObjectFormatKind::Elf, 77u},
                         Cell{"adrp %0, _g@PAGE\n\tadd %0, %0, _g@PAGEOFF", ObjectFormatKind::MachO, 79u}}) {
        SCOPED_TRACE(c.text);
        auto names = aUnitDefiningGAndF();
        auto const r = lower(kArm, c.text, &names, c.kind);
        ASSERT_TRUE(r.parsed) << r.messages;
        ASSERT_TRUE(r.ok) << r.messages;
        EXPECT_EQ(r.targets(), (std::vector<std::uint32_t>{c.symbol, c.symbol})) << r.messages;
        EXPECT_TRUE(names.asked.empty());
    }
}

// ══ a name the unit does NOT define is an import, with what the reference states ═══════════════════════

TEST(AsmTemplateProgramNames, AnUndefinedNameIsImportedVerbatimAndACallStatesCode) {
    auto names = aUnitDefiningGAndF();
    auto const r = lower(kX86, "call ext_fn\n\tleaq ext_datum(%%rip), %0", &names, ObjectFormatKind::Elf);
    ASSERT_TRUE(r.parsed) << r.messages;
    ASSERT_TRUE(r.ok) << r.messages;
    ASSERT_EQ(names.asked.size(), 2u) << r.messages;
    EXPECT_EQ(names.asked[0].first, "ext_fn");
    EXPECT_EQ(names.asked[0].second, AsmTemplateNameUse::Call) << "a call states CODE";
    EXPECT_EQ(names.asked[1].first, "ext_datum");
    EXPECT_EQ(names.asked[1].second, AsmTemplateNameUse::Address)
        << "an address states nothing; the definition the link finds decides";
    EXPECT_EQ(r.targets(), (std::vector<std::uint32_t>{900u, 901u})) << r.messages;
}

TEST(AsmTemplateProgramNames, AnImportTheCallerCannotMintFailsTheTemplateByName) {
    auto names          = aUnitDefiningGAndF();
    names.refuseImports = true;
    auto const r = lower(kX86, "call ext_fn", &names, ObjectFormatKind::Elf);
    ASSERT_TRUE(r.parsed) << r.messages;
    EXPECT_FALSE(r.ok) << "an operand with no symbol behind it must fail the run";
    EXPECT_TRUE(says(r, "'ext_fn'")) << r.messages;
    EXPECT_TRUE(says(r, "neither defines nor could import")) << r.messages;
}

// ══ what is never a name of the program ════════════════════════════════════════════════════════════════

TEST(AsmTemplateProgramNames, ALabelOfTheTemplateIsNotACalleeAndIsNeverImported) {
    for (char const* text : {"1:\n\tnop\n\tcall 1b", "Lhere:\n\tnop\n\tcall Lhere"}) {
        SCOPED_TRACE(text);
        auto names = aUnitDefiningGAndF();
        auto const r = lower(kX86, text, &names, ObjectFormatKind::Elf);
        ASSERT_TRUE(r.parsed) << r.messages;
        EXPECT_FALSE(r.ok) << r.messages;
        EXPECT_TRUE(says(r, "a label this assembly template defines")) << r.messages;
        EXPECT_TRUE(names.asked.empty()) << "a label of the template was asked about as a program name";
    }
}

TEST(AsmTemplateProgramNames, AnAsmGotoLabelIsNeverAProgramNameInAnAddressOrACall) {
    for (char const* text : {"movq %l[done], %0", "call %l[done]", "call %l[nope]"}) {
        SCOPED_TRACE(text);
        auto names = aUnitDefiningGAndF();
        auto const r = lower(kX86, text, &names, ObjectFormatKind::Elf, {"%l[done]"});
        ASSERT_TRUE(r.parsed) << r.messages;
        EXPECT_FALSE(r.ok) << r.messages;
        EXPECT_TRUE(says(r, "asm goto")) << r.messages;
        EXPECT_TRUE(names.asked.empty())
            << "a label placeholder was imported as a name no object can define:\n" << r.messages;
    }
}

// A constant added to a FUNCTION the unit defines is refused, as in a `.s`; added to data it is the
// symbol plus the constant.
TEST(AsmTemplateProgramNames, AConstantAddedToADefinedFunctionIsRefusedAndToDataIsCarried) {
    auto names = aUnitDefiningGAndF();
    auto const code = lower(kX86, "leaq f+4(%%rip), %0", &names, ObjectFormatKind::Elf);
    ASSERT_TRUE(code.parsed) << code.messages;
    EXPECT_FALSE(code.ok) << code.messages;
    EXPECT_TRUE(says(code, "'f' plus 4")) << code.messages;

    auto const data = lower(kX86, "leaq g+4(%%rip), %0", &names, ObjectFormatKind::Elf);
    ASSERT_TRUE(data.parsed) << data.messages;
    ASSERT_TRUE(data.ok) << data.messages;
    EXPECT_EQ(data.targets(), (std::vector<std::uint32_t>{77u})) << data.messages;
}

// ══ no names handed in: every name of the program is REFUSED, loudly, by name ═══════════════════════════

TEST(AsmTemplateProgramNames, WithNoNamesHandedInEveryProgramNameIsRefusedSayingSo) {
    for (auto const& [pair, text] : {std::pair{kX86, "leaq g(%%rip), %0"}, std::pair{kX86, "call f"},
                                     std::pair{kArm, "adrp %0, g"}}) {
        SCOPED_TRACE(text);
        auto const r = lower(pair, text, nullptr, ObjectFormatKind::Elf);
        ASSERT_TRUE(r.parsed) << r.messages;
        EXPECT_FALSE(r.ok) << "a name of the program resolved to something with no names handed in:\n"
                           << r.messages;
        EXPECT_TRUE(says(r, "assembly TEMPLATE")) << r.messages;
        EXPECT_TRUE(says(r, "no names of that program")) << r.messages;
        EXPECT_TRUE(says(r, "neither imported nor guessed")) << r.messages;
    }
}
