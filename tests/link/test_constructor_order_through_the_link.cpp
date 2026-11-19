// ★★ CONSTRUCTORS OF ONE PRIORITY RUN IN THE UNITS' INPUT ORDER, THEN EACH UNIT'S DEFINITION ORDER -- READ FROM THE
// IMAGE (P69, lane `cs`, fold f26d: the host-toolchain class, its site in the link's merge; main's decision).
//
// THE RULE, ONE FOR EVERY FORMAT: constructors (and destructors, walked back) of an EQUAL priority run in the order
// of their units in the link's input, and within a unit in the order the unit defines them -- the unit's schedule
// order, which is its definition order (`staticInitOrder`, src/link/static_init_tables.cpp, a STABLE sort by
// priority that reads no SymbolId). WHY THIS ORDER (main's decision; the operator may veto it):
//   * DOCUMENTED: no vendor documents a tie. GCC and Clang state only that a smaller priority runs first (no
//     priority being 65535); Microsoft documents the alphabetical merge of the `.CRT$` subsections. Any
//     deterministic order conforms, and a program that leans on a tie leans on unspecified behaviour.
//   * MEASURED (probe-reference-cc, runs 20261116-201121-47a61e0c and 20261116-201510-1d84face): input order then
//     definition order is cl 19.52 + link.exe's (`.CRT$XCU` pointers; DSS's PE runtime is UCRT, cl's), clang 18's on
//     ELF and Apple clang 21's on Mach-O in EVERY cell, and gcc 13.3's on ELF at the default priority.
//   * THE REFUSAL COST, the two dissents: MinGW-w64 gcc 13.2 on PE runs the exact reverse (its runtime's `.ctors`
//     walk -- a runtime DSS does not ship, and cl disagrees with it on the same format); gcc 13.3 on ELF with an
//     EXPLICIT equal priority runs first-DECLARATION order (a constructor prototyped first runs first), where
//     clang on the same format runs definition order.
// Until this fold DSS broke a tie by SymbolId: within a unit that is first-DECLARATION order (the front end numbers
// a unit's symbols at their first declaration), and across units the MERGED id, which the merge minted in the hash
// order of the winners' names -- so the order differed by the standard library that built DSS.
//
// THE CELL, structural, on every leg: units built in memory, each constructor a function whose body no other
// function has, linked through `linker::link` into a pe64 exec; the entry trampoline's calls are read from the
// image's bytes (each `call rel32` from the entry up to the first constructor's body whose target is a
// constructor's body). The RUN of the same rule, where a leg runs the target natively, is
// examples/c/constructor_ties_run_in_input_then_definition_order (and its `_reversed` twin) and
// examples/c/constructor_ties_in_one_unit_run_in_definition_order.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"
#include "link/image_request.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include "../program/pe_image_readers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

ImageRequest const kRequest{.artifactFileName = "constructor_order"};

[[nodiscard]] std::shared_ptr<TargetSchema const> x64() {
    static auto const t = [] {
        auto r = TargetSchema::loadShipped("x86_64");
        return r.has_value() ? std::shared_ptr<TargetSchema const>{*r} : nullptr;
    }();
    return t;
}
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> peExec() {
    static auto const f = [] {
        auto r = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
        return r.has_value() ? std::shared_ptr<ObjectFormatSchema const>{*r} : nullptr;
    }();
    return f;
}

[[nodiscard]] StaticInitSchedule beforeEntryAt(std::uint32_t priority) {
    StaticInitSchedule s;
    s.setPriorityFor(StaticInitPhase::BeforeEntry, priority);
    return s;
}

// A constructor's body: `mov eax, <tag>; ret` -- six bytes no other function of the image holds.
[[nodiscard]] std::vector<std::uint8_t> bodyOf(std::uint32_t tag) {
    std::uint32_t const imm = 0x5AC70000u + tag;
    return {0xB8, static_cast<std::uint8_t>(imm), static_cast<std::uint8_t>(imm >> 8),
            static_cast<std::uint8_t>(imm >> 16), static_cast<std::uint8_t>(imm >> 24), 0xC3};
}

struct Ctor {
    std::string   name;
    std::uint32_t id;    // the unit's own SymbolId of it
    std::uint32_t tag;   // its body's
    std::uint32_t priority = kUnprioritizedStaticInit;
};

// One unit defining `ctors` in that (definition) order, each scheduled at its priority (the default unless stated),
// plus `main` when `withMain`. The ids are the unit's own, given apart from the definition order.
[[nodiscard]] AssembledModule unitOf(std::uint32_t cu, std::vector<Ctor> const& ctors, bool withMain) {
    AssembledModule m;
    m.cuId = CompilationUnitId{cu};
    for (Ctor const& c : ctors) {
        AssembledFunction fn;
        fn.symbol = SymbolId{c.id};
        fn.bytes  = bodyOf(c.tag);
        m.functions.push_back(std::move(fn));
        m.symbols.push_back(ModuleSymbol{SymbolId{c.id}, c.name, SymbolBinding::Global, SymbolVisibility::Default});
        m.staticInitSchedule.push_back(LirStaticInitEntry{SymbolId{c.id}, beforeEntryAt(c.priority)});
    }
    if (withMain) {
        std::uint32_t id = 1;
        for (Ctor const& c : ctors) id = std::max(id, c.id + 1);
        AssembledFunction fn;
        fn.symbol = SymbolId{id};
        fn.bytes  = {0x31, 0xC0, 0xC3};   // xor eax, eax; ret
        m.functions.push_back(std::move(fn));
        m.symbols.push_back(ModuleSymbol{SymbolId{id}, "main", SymbolBinding::Global, SymbolVisibility::Default});
        m.userEntrySymbol = SymbolId{id};
    }
    m.expectedFuncCount = m.functions.size();
    return m;
}

// The constructors the image's entry calls, in the order it calls them, by name; or the reason none could be read.
[[nodiscard]] std::string calledFromTheEntry(std::vector<std::uint8_t> const& img, std::vector<Ctor> const& all) {
    using namespace dss::test_support;
    auto const secs = peSections(img);
    if (secs.empty()) return "<no PE section table>";
    std::size_t const   peOff    = rdU32(img, 0x3C);
    std::uint32_t const entryRva = rdU32(img, peOff + 24 + 16);
    std::size_t const   entry    = peRvaToOffset(secs, entryRva);
    if (entry == 0) return "<the entry maps to no section>";
    std::map<std::size_t, std::string> bodyAt;   // file offset -> constructor
    for (Ctor const& c : all) {
        auto const body = bodyOf(c.tag);
        auto const at   = std::search(img.begin(), img.end(), body.begin(), body.end());
        if (at == img.end()) return "<no body of " + c.name + " in the image>";
        bodyAt.emplace(static_cast<std::size_t>(at - img.begin()), c.name);
    }
    auto const firstBodyAfter = bodyAt.upper_bound(entry);
    std::size_t const stop    = firstBodyAfter == bodyAt.end() ? img.size() : firstBodyAfter->first;
    std::string out;
    for (std::size_t k = entry; k + 5 <= stop; ++k) {
        if (img[k] != 0xE8u) continue;
        auto const rel    = static_cast<std::int32_t>(rdU32(img, k + 1));
        auto const target = static_cast<std::int64_t>(k) + 5 + rel;
        auto const hit    = bodyAt.find(static_cast<std::size_t>(target));
        if (hit != bodyAt.end()) out += (out.empty() ? "" : " ") + hit->second;
    }
    return out;
}

[[nodiscard]] std::string linkAndRead(std::vector<AssembledModule> const& units, std::vector<Ctor> const& all) {
    auto const t = x64();
    auto const f = peExec();
    if (!t || !f) return "<the shipped x86_64 target or pe64 exec format did not load>";
    DiagnosticReporter rep;
    auto const image = linker::link(std::span<AssembledModule const>{units}, *t, *f, rep, kRequest);
    if (!image.ok()) {
        std::string why = "<the link refused:";
        for (auto const& d : rep.all()) why += " " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
        return why + ">";
    }
    return calledFromTheEntry(image.bytes, all);
}

// The three of one unit, defined `wa`, `wb`, `wc` -- numbered as the front end numbers a unit whose `wc` is
// PROTOTYPED before the others are defined (first declaration first): `wc` 1, `wa` 2, `wb` 3.
std::vector<Ctor> const kW = {{"wa", 2, 21}, {"wb", 3, 22}, {"wc", 1, 23}};

}  // namespace

// ACROSS UNITS: the units' input order, in either order the link is given them; within the unit of three, its
// definition order whatever its ids. CONTROL: the same units in the reverse order run reversed.
TEST(ConstructorOrderThroughTheLink, EqualPrioritiesRunInTheUnitsInputOrderThenTheirDefinitionOrder) {
    std::vector<Ctor> all;
    std::vector<std::vector<Ctor>> one;
    for (std::uint32_t k = 1; k <= 4; ++k) one.push_back({Ctor{std::format("ctor_u{}", k), 1, k}});
    for (auto const& u : one) all.push_back(u.front());
    all.insert(all.end(), kW.begin(), kW.end());
    {
        std::vector<AssembledModule> units;
        for (std::uint32_t k = 0; k < 4; ++k) units.push_back(unitOf(k + 1, one[k], false));
        units.push_back(unitOf(5, kW, false));
        units.push_back(unitOf(6, {}, true));
        EXPECT_EQ(linkAndRead(units, all), "ctor_u1 ctor_u2 ctor_u3 ctor_u4 wa wb wc")
            << "the units in input order u1 u2 u3 u4 w";
    }
    {
        std::vector<AssembledModule> units;
        units.push_back(unitOf(5, kW, false));
        for (std::uint32_t k = 4; k-- > 0;) units.push_back(unitOf(k + 1, one[k], false));
        units.push_back(unitOf(6, {}, true));
        EXPECT_EQ(linkAndRead(units, all), "wa wb wc ctor_u4 ctor_u3 ctor_u2 ctor_u1")
            << "the units in input order w u4 u3 u2 u1";
    }
}

// WITHIN ONE UNIT, linked alone (no merge renumbers it): its definition order, not its ids' order -- the unit
// numbered `wc` first, as the front end numbers a constructor prototyped before the others are defined.
TEST(ConstructorOrderThroughTheLink, OneUnitsConstructorsRunInItsDefinitionOrderNotItsIdsOrder) {
    std::vector<AssembledModule> const units{unitOf(1, kW, true)};
    EXPECT_EQ(linkAndRead(units, kW), "wa wb wc");
}

// MANY OF ONE PRIORITY in one unit, numbered against their definition order, and one of a SMALLER priority defined
// after them all: the sort must move that one to the front, and the tie keeps all forty in place behind it. Forty
// is more than any standard library's small-range insertion pass covers, and the moved one makes every library's
// partition step move elements, so only a STABLE sort keeps them.
TEST(ConstructorOrderThroughTheLink, FortyConstructorsOfOnePriorityKeepTheirDefinitionOrder) {
    std::vector<Ctor> many;
    std::string       expected = "early";
    for (std::uint32_t k = 1; k <= 40; ++k) {
        many.push_back(Ctor{std::format("c{}", k), 42 - k, 100 + k});
        expected += " " + std::format("c{}", k);
    }
    many.push_back(Ctor{"early", 1, 200, 101});
    std::vector<AssembledModule> const units{unitOf(1, many, true)};
    EXPECT_EQ(linkAndRead(units, many), expected);
}
