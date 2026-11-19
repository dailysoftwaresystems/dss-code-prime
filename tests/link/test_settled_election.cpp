// P69 (lane `cs`, cs-28) — `linker::resolveSettlingCarriedNames`, THE ELECTION THAT SETTLES A DEFINITION'S OWN
// NAME BEFORE ITS OTHER NAMES RUN, over synthetic elections (no merge, no link): the one statement of the rule —
// the cross-unit MIR merge calls it (an alias of a definition), and so does the unit linker (another external label
// of a definition). Each case states the definitions by hand, with the shipped link
// documents' own answers (`stateTheLinksAnswers`), and reads what the function elects and what it withdraws:
//   * WITHDRAWAL — a STRONG name carried by a copy the link discards whole leaves the election when the copy loses,
//     and the name goes to what else defines it; where the document KEEPS a superseded definition's bytes (ELF),
//     nothing is withdrawn; a WEAK name is never withdrawn;
//   * THE SETTLED ORDER — a carried name is judged only once its carrier's own name is settled: a carrier that
//     lost its name only to a name later withdrawn is elected again first, and wins;
//   * A CYCLE LEFT LIVE — carried names whose carriers each run under the other's name stay in the election,
//     which refuses the twice defined names itself.

#include "core/types/symbol_attrs.hpp"
#include "link/common_yield.hpp"
#include "link/cross_cu_resolve.hpp"
#include "link/object_format_schema.hpp"
#include "link/symbol_kind.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace dss;
using dss::linker::CarriedName;
using dss::linker::CrossCuDef;
using dss::linker::resolveSettlingCarriedNames;

namespace {

using Kind = WeakDefinitionKind;

[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> shipped(char const* name) {
    auto f = ObjectFormatSchema::loadShipped(name);
    EXPECT_TRUE(f.has_value()) << name;
    return f.has_value() ? *f : nullptr;
}

[[nodiscard]] LinkedSymbolKey key(std::uint32_t cu, std::uint32_t sym) {
    return LinkedSymbolKey{CompilationUnitId{cu}, SymbolId{sym}};
}

// One definition, with the link document's answers for its kind stated as every door states them.
[[nodiscard]] CrossCuDef def(ObjectFormatSchema const& format, std::uint32_t cu, std::uint32_t sym, char const* name,
                             SymbolBinding binding, std::optional<Kind> kind = std::nullopt) {
    CrossCuDef d{name, binding, key(cu, sym)};
    linker::stateTheLinksAnswers(d, format, kind);
    return d;
}

[[nodiscard]] std::size_t countNamed(auto const& records, std::string const& name) {
    return static_cast<std::size_t>(
        std::count_if(records.begin(), records.end(), [&](auto const& r) { return r.name == name; }));
}

}  // namespace

// Cell (i) of probe mj1: copy w of unit 2 loses `w` to unit 1's; the alias `m` it carries leaves the election, and
// `m` goes to unit 1's weak definition. ELF keeps a superseded definition's bytes: nothing is withdrawn there.
TEST(SettledElection, ANameCarriedByACopyThatLosesIsWithdrawnWhereTheLinkDiscardsTheCopyWhole) {
    for (char const* const document : {"pe64-x86_64-windows-exec", "elf64-x86_64-linux-exec"}) {
        SCOPED_TRACE(document);
        auto const format = shipped(document);
        ASSERT_NE(format, nullptr);
        bool const pe = std::string{document}.starts_with("pe");
        std::optional<Kind> const copyKind = pe ? std::optional{Kind::SelectAny} : std::optional{Kind::Overridable};
        std::vector<CrossCuDef> const defs{
            def(*format, 1, 1, "w", SymbolBinding::Weak, copyKind),        // [0] the winning copy
            def(*format, 1, 2, "m", SymbolBinding::Weak, Kind::Overridable),  // [1] unit 1's weak m
            def(*format, 2, 1, "w", SymbolBinding::Weak, copyKind),        // [2] the losing copy
            def(*format, 2, 2, "m", SymbolBinding::Global),                // [3] unit 2's alias m, carried by [2]
        };
        std::vector<CarriedName> const carried{CarriedName{3, "w", key(2, 1), copyKind}};
        auto const settled = resolveSettlingCarriedNames(defs, carried, *format);
        ASSERT_EQ(settled.resolution.winners.count("w"), 1u);
        EXPECT_EQ(settled.resolution.winners.at("w"), key(1, 1));
        ASSERT_EQ(settled.resolution.winners.count("m"), 1u);
        if (pe) {
            EXPECT_EQ(settled.withdrawn, std::vector<std::size_t>{3})
                << "the name the losing copy carries was not withdrawn";
            EXPECT_EQ(settled.resolution.winners.at("m"), key(1, 2))
                << "`m` did not go to the definition left once the carried name was withdrawn";
        } else {
            EXPECT_TRUE(settled.withdrawn.empty()) << "a link that keeps the loser's bytes withdrew a name of it";
            EXPECT_EQ(settled.resolution.winners.at("m"), key(2, 2))
                << "where the loser's bytes stay, the strong name they carry wins";
        }
        EXPECT_TRUE(settled.resolution.conflicts.empty());
    }
    // Two copies each carrying `m` (cell (ii)): the losing copy's `m` is withdrawn, and the two are no conflict.
    auto const pe = shipped("pe64-x86_64-windows-exec");
    ASSERT_NE(pe, nullptr);
    std::vector<CrossCuDef> const two{
        def(*pe, 1, 1, "w", SymbolBinding::Weak, Kind::SelectAny),
        def(*pe, 1, 2, "m", SymbolBinding::Global),
        def(*pe, 2, 1, "w", SymbolBinding::Weak, Kind::SelectAny),
        def(*pe, 2, 2, "m", SymbolBinding::Global),
    };
    std::vector<CarriedName> const both{CarriedName{1, "w", key(1, 1), Kind::SelectAny},
                                        CarriedName{3, "w", key(2, 1), Kind::SelectAny}};
    auto const settled = resolveSettlingCarriedNames(two, both, *pe);
    EXPECT_EQ(settled.withdrawn, std::vector<std::size_t>{3});
    EXPECT_TRUE(settled.resolution.conflicts.empty()) << "the losing copy's `m` was a second definition";
    ASSERT_EQ(settled.resolution.winners.count("m"), 1u);
    EXPECT_EQ(settled.resolution.winners.at("m"), key(1, 2));
    // An entry index past the definitions carries nothing.
    std::vector<CarriedName> const stray{CarriedName{99, "w", key(2, 1), Kind::SelectAny}};
    EXPECT_TRUE(resolveSettlingCarriedNames(two, stray, *pe).withdrawn.empty());
}

// The carrier of `m` (unit 3's copy of `y`) loses `y` in a first election only to unit 2's name `y` — which is
// itself carried by unit 2's copy of `x`, a copy that loses `x` to unit 1's. Judged in one pass, `m` would be
// withdrawn with a carrier that in fact wins: the settled order withdraws unit 2's `y` first, elects again, and
// unit 3's copy holds `y`, so `m` stays and wins over unit 1's weak `m`.
TEST(SettledElection, ACarriedNameIsJudgedOnlyOnceItsCarriersOwnNameIsSettled) {
    auto const pe = shipped("pe64-x86_64-windows-exec");
    ASSERT_NE(pe, nullptr);
    std::vector<CrossCuDef> const defs{
        def(*pe, 1, 1, "x", SymbolBinding::Weak, Kind::SelectAny),    // [0] x's winning copy
        def(*pe, 2, 1, "x", SymbolBinding::Weak, Kind::SelectAny),    // [1] x's losing copy
        def(*pe, 2, 2, "y", SymbolBinding::Weak, Kind::SelectAny),    // [2] y, carried by [1]: the lower key of y's
        def(*pe, 3, 1, "y", SymbolBinding::Weak, Kind::SelectAny),    // [3] y's copy that carries m
        def(*pe, 3, 2, "m", SymbolBinding::Global),                   // [4] m, carried by [3]
        def(*pe, 1, 3, "m", SymbolBinding::Weak, Kind::Overridable),  // [5] unit 1's weak m
    };
    // [2] is a weak entry of the select-any kind, which goes with its carrier's section (a second label of it).
    std::vector<CarriedName> const carried{CarriedName{2, "x", key(2, 1), Kind::SelectAny, Kind::SelectAny},
                                           CarriedName{4, "y", key(3, 1), Kind::SelectAny}};
    // The first election alone gives `y` to [2] — the trap a single pass falls into.
    auto const naive = linker::resolveCrossCuDefs(defs);
    ASSERT_EQ(naive.winners.at("y"), key(2, 2));
    auto const settled = resolveSettlingCarriedNames(defs, carried, *pe);
    EXPECT_EQ(settled.withdrawn, std::vector<std::size_t>{2})
        << "a carried name was judged before its carrier's own name was settled";
    EXPECT_EQ(settled.resolution.winners.at("y"), key(3, 1)) << "y was not elected again after the withdrawal";
    EXPECT_EQ(settled.resolution.winners.at("m"), key(3, 2)) << "m was withdrawn with a carrier that wins";
}

// The review of fold f26a, F1 (probe wk1, cells 3 and 4): a WEAK alias `m` of unit 2's losing copy of `w`, of PE's
// overridable kind (which the link does not discard whole), is the weak definition of `m` it is — never withdrawn with
// the copy, so as the only definition of `m` it wins the name (the link then reads the winning copy of `w` through
// it, clang's weak external whose default is `w` by name). CONTROL: the same alias STRONG goes with its copy, and `m`
// has no definition left. (A weak carried entry of the select-any kind goes with its copy:
// ACarriedNameIsJudgedOnlyOnceItsCarriersOwnNameIsSettled withdraws one.)
TEST(SettledElection, AWeakNameACopyCarriesIsNeverWithdrawn) {
    auto const pe = shipped("pe64-x86_64-windows-exec");
    ASSERT_NE(pe, nullptr);
    for (SymbolBinding const binding : {SymbolBinding::Weak, SymbolBinding::Global}) {
        bool const weak = binding == SymbolBinding::Weak;
        SCOPED_TRACE(weak ? "a weak alias" : "a strong alias");
        std::optional<Kind> const aliasKind = weak ? std::optional{Kind::Overridable} : std::nullopt;
        std::vector<CrossCuDef> const defs{
            def(*pe, 1, 1, "w", SymbolBinding::Weak, Kind::SelectAny),  // [0] the winning copy
            def(*pe, 2, 1, "w", SymbolBinding::Weak, Kind::SelectAny),  // [1] the losing copy
            def(*pe, 2, 2, "m", binding, aliasKind),                    // [2] the alias, carried by [1]
        };
        std::vector<CarriedName> const carried{CarriedName{2, "w", key(2, 1), Kind::SelectAny, aliasKind}};
        auto const settled = resolveSettlingCarriedNames(defs, carried, *pe);
        EXPECT_EQ(settled.resolution.winners.at("w"), key(1, 1));
        if (weak) {
            EXPECT_TRUE(settled.withdrawn.empty()) << "a weak name was withdrawn with the copy that carries it";
            ASSERT_EQ(settled.resolution.winners.count("m"), 1u) << "the weak alias left the election of its name";
            EXPECT_EQ(settled.resolution.winners.at("m"), key(2, 2));
        } else {
            EXPECT_EQ(settled.withdrawn, std::vector<std::size_t>{2}) << "the strong name stayed with its lost copy";
            EXPECT_EQ(settled.resolution.winners.count("m"), 0u);
        }
    }
}

// Probe mj1's cycle: unit 1's copy of `y` carries `x`, unit 2's copy of `x` carries `y`. Each carried name waits on
// the other; both stay, and the election refuses `x` and `y` as a copy beside a strong definition — the references'
// "duplicate symbol" for this very pair.
TEST(SettledElection, CarriedNamesThatWaitOnEachOtherStayInTheElection) {
    auto const pe = shipped("pe64-x86_64-windows-exec");
    ASSERT_NE(pe, nullptr);
    std::vector<CrossCuDef> const defs{
        def(*pe, 1, 1, "y", SymbolBinding::Weak, Kind::SelectAny),
        def(*pe, 1, 2, "x", SymbolBinding::Global),
        def(*pe, 2, 1, "x", SymbolBinding::Weak, Kind::SelectAny),
        def(*pe, 2, 2, "y", SymbolBinding::Global),
    };
    std::vector<CarriedName> const carried{CarriedName{1, "y", key(1, 1), Kind::SelectAny},
                                           CarriedName{3, "x", key(2, 1), Kind::SelectAny}};
    auto const settled = resolveSettlingCarriedNames(defs, carried, *pe);
    EXPECT_TRUE(settled.withdrawn.empty()) << "names that wait on each other were withdrawn on a guess";
    EXPECT_EQ(countNamed(settled.resolution.collisions, "x"), 1u);
    EXPECT_EQ(countNamed(settled.resolution.collisions, "y"), 1u);
}
