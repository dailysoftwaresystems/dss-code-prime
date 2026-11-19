// P69 fold 3d, lane p69/xa — D-LK-THE-LINK-ORDER-IS-NOT-THE-COMMAND-LINE-ORDER: the link's ORDER is the operator's.
//
// Every answer "the first in link order" is taken in the order the command line names the inputs, a source in its
// own place as the object it compiles to. ✔MEASURED P69 fold 3d (lane `xa`'s probes `order.py`, `elfweak_wsl.sh`,
// `objonly_wsl.sh` and `relink_wsl.sh`, each image run; `main` returns `x[2] * 10 + through_own()` or `w * 10 +
// through_own()`, so 55 is the C unit's definition kept and 11 the named object's):
//   * a select-any `x` in a C unit and in a named COFF object — cl 19.44 + link.exe 14.44, clang-cl 19.1.5 with
//     link.exe and with lld-link, MinGW-w64 gcc 13.2 + GNU ld: `main.c own.c obj` 55, `main.c obj own.c` 11,
//     `obj main.c own.c` 11, `own.c obj main.c` 55 (but link.exe's decided-fork cell of a LARGEST copy, 11);
//   * two weak ELF definitions of `w`, one in a C unit and one in a named object — gcc 13.3 + GNU ld 2.42 and clang
//     18 + ld.lld 18: the same four answers; through `ld -r` / `ld.lld -r` and then a link, the same again;
//   * a build of objects alone (`mainown.o` defines `main` and a weak `w = 5`, `weak1.o` a weak `w = 1`): 1 / 5 by
//     order, both linkers.
// DSS (run 20261022-060344-db59c6d0's dsscp, the tree of fold 3d before this fix) answered 55 in every order of every
// route, and 5 / 5: it took each answer in the order it MADE its units — every compiled unit merged into one held
// before every named object, the object defining the entry first, and object cuIds minted after the units'.
//
// What carries the order: each unit's `AssembledModule::linkPosition` (the operator's position of the input it is)
// and `linkPositionOfDefinition` (of the input each definition came from, where one unit holds several inputs'),
// read through `linkPositionOf`; one comparison of two places, `linker::linkPositionPrecedes`; and every judgement of
// the order asks it — the name election's ties (`linker::CrossCuDef::linkPosition`, read FIRST by
// `resolveCrossCuDefs`) and the section-group keep (`linker.cpp`, `discardLaterSectionGroups`). This binary pins those
// two judgements in-process, then every route of the driver that states the places (the merged build, its image and
// its relocatable link, a build of objects alone, on ELF and PE) on every leg, and the programs where the host runs
// them.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/cross_cu_resolve.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link/symbol_kind.hpp"
#include "program/program.hpp"

#include "host_native_target.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using dss::linker::CrossCuDef;
using dss::linker::resolveCrossCuDefs;

namespace {

namespace fs = std::filesystem;

[[nodiscard]] CrossCuDef def(std::uint32_t cu, std::string name, SymbolBinding binding,
                             std::optional<std::uint32_t> position) {
    CrossCuDef d{std::move(name), binding, LinkedSymbolKey{CompilationUnitId{cu}, SymbolId{1}}};
    d.linkPosition = position;
    return d;
}

[[nodiscard]] std::uint32_t winnerUnit(std::vector<CrossCuDef> const& defs, std::string const& name) {
    auto const r = resolveCrossCuDefs(std::span<CrossCuDef const>{defs});
    auto const it = r.winners.find(name);
    EXPECT_NE(it, r.winners.end()) << name << " has a winner";
    return it == r.winners.end() ? 0u : it->second.cuId.v;
}

}  // namespace

// Every tie the election breaks is the first in the link's order: the place of each definition's input FIRST, the key
// only at one place or at none -- whatever order the definitions are handed in. RED-ON-DISABLE: break the tie by the
// key alone (the kernel before this fold) -> the unit whose key is lower wins though its input stands later.
TEST(LinkOrder, TheElectionsTiesAreTheFirstInTheLinksOrder) {
    struct Case {
        char const*             what;
        std::vector<CrossCuDef> defs;
        std::string             name;
        std::uint32_t           winner;   // the unit whose definition is the name's
    };
    auto common = [](std::uint32_t cu, std::size_t size, std::optional<std::uint32_t> position) {
        CrossCuDef d   = def(cu, "c", SymbolBinding::Global, position);
        d.tentative    = true;
        d.bodySize     = size;
        return d;
    };
    std::vector<Case> const cases{
        // `main.c weak1.o own.c` merged: the unit (cu 1, its source third) beside the object (cu 7, second).
        {"two weak definitions: the earlier place wins over the lower key",
         {def(1, "w", SymbolBinding::Weak, 3), def(7, "w", SymbolBinding::Weak, 2)}, "w", 7},
        {"a stated place precedes none",
         {def(1, "w", SymbolBinding::Weak, std::nullopt), def(9, "w", SymbolBinding::Weak, 5)}, "w", 9},
        {"at one place the key decides",
         {def(3, "w", SymbolBinding::Weak, 4), def(2, "w", SymbolBinding::Weak, 4)}, "w", 2},
        {"with no place on either side the key decides, as before",
         {def(2, "w", SymbolBinding::Weak, std::nullopt), def(1, "w", SymbolBinding::Weak, std::nullopt)}, "w", 1},
        {"two commons of one size: the earlier place", {common(1, 8, 2), common(4, 8, 1)}, "c", 4},
        {"two commons: the larger, wherever it stands", {common(4, 8, 1), common(1, 16, 2)}, "c", 1},
        {"two strong definitions: the one the conflict keeps is the earlier place",
         {def(1, "s", SymbolBinding::Global, 2), def(5, "s", SymbolBinding::Global, 1)}, "s", 5},
    };
    for (auto const& c : cases) {
        SCOPED_TRACE(c.what);
        EXPECT_EQ(winnerUnit(c.defs, c.name), c.winner) << "handed in as written";
        std::vector<CrossCuDef> reversed(c.defs.rbegin(), c.defs.rend());
        EXPECT_EQ(winnerUnit(reversed, c.name), c.winner) << "handed in reversed";
    }
}

// The one comparison of two places: a stated position before none, a lower before a higher, neither at one place.
TEST(LinkOrder, APlacePrecedesAnotherByPositionAndAnyPlacePrecedesNone) {
    using dss::linker::linkPositionPrecedes;
    EXPECT_TRUE(linkPositionPrecedes(1u, 2u));
    EXPECT_FALSE(linkPositionPrecedes(2u, 1u));
    EXPECT_FALSE(linkPositionPrecedes(3u, 3u));
    EXPECT_TRUE(linkPositionPrecedes(7u, std::nullopt));
    EXPECT_FALSE(linkPositionPrecedes(std::nullopt, 0u));
    EXPECT_FALSE(linkPositionPrecedes(std::nullopt, std::nullopt));
}

namespace {

[[nodiscard]] std::vector<std::uint8_t> fixture(std::string_view name) {
    std::ifstream in{dss::test::repoRoot() / "tests" / "link" / "data" / std::string{name}, std::ios::binary};
    if (!in.good()) {
        ADD_FAILURE() << "cannot open fixture " << name;
        return {};
    }
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> object;   // pe64-x86_64-windows, the relocatable document
};

[[nodiscard]] Loaded load() {
    Loaded l;
    if (auto t = TargetSchema::loadShipped("x86_64"); t.has_value()) l.target = *t;
    if (auto f = ObjectFormatSchema::loadShipped("pe64-x86_64-windows"); f.has_value()) l.object = *f;
    EXPECT_TRUE(l.target && l.object) << "the shipped x86_64 target and pe64 object documents";
    return l;
}

// The definition `x` of a unit: its symbol and its bytes.
struct Copy {
    SymbolId                  symbol{};
    std::vector<std::uint8_t> bytes;
};

[[nodiscard]] std::optional<Copy> copyOfX(AssembledModule const& m) {
    for (auto const& s : m.symbols) {
        if (s.name != "x") continue;
        for (auto const& d : m.dataItems) {
            if (d.symbol == s.symbol) return Copy{s.symbol, d.bytes};
        }
    }
    return std::nullopt;
}

// The two ANY copies of `x` (`tests/link/data/coff_comdat_any_{small,big}.s`: 12 bytes holding 1, and 16 holding 7,
// at offset 8), read as units, placed as `place` says, linked in `order` into a relocatable artifact and read back:
// the bytes of the ONE `x` the link kept, or none after a failed assertion.
struct Placed {
    std::optional<std::uint32_t> small;          // `any_small`'s unit
    std::optional<std::uint32_t> big;            // `any_big`'s unit
    std::optional<std::uint32_t> smallsX;        // a place stated for `any_small`'s DEFINITION of `x`
    bool                         bigFirst = false;   // the order the link holds the units in
};

[[nodiscard]] std::vector<std::uint8_t> keptX(Placed const& place) {
    auto const l = load();
    if (!l.target || !l.object) return {};
    std::vector<AssembledModule> units;
    for (auto const* name : {"coff_comdat_any_small.obj", "coff_comdat_any_big.obj"}) {
        auto const         bytes = fixture(name);
        DiagnosticReporter rep;
        auto unit = pe::readRelocatableObject(bytes, *l.target, *l.object, rep,
                                              CompilationUnitId{static_cast<std::uint32_t>(units.size() + 1)});
        EXPECT_TRUE(unit.has_value()) << name << diagnosticsOf(rep);
        if (!unit.has_value()) return {};
        units.push_back(std::move(*unit));
    }
    units[0].linkPosition = place.small;
    units[1].linkPosition = place.big;
    if (place.smallsX.has_value()) {
        auto const x = copyOfX(units[0]);
        EXPECT_TRUE(x.has_value()) << "any_small defines x";
        if (!x.has_value()) return {};
        units[0].linkPositionOfDefinition.emplace(x->symbol, *place.smallsX);
    }
    if (place.bigFirst) std::swap(units[0], units[1]);
    DiagnosticReporter rep;
    auto const linked = linker::link(std::span<AssembledModule const>{units}, *l.target, *l.object, rep);
    EXPECT_TRUE(linked.ok()) << diagnosticsOf(rep);
    if (!linked.ok()) return {};
    DiagnosticReporter back;
    auto const artifact = pe::readRelocatableObject(linked.bytes, *l.target, *l.object, back, CompilationUnitId{9});
    EXPECT_TRUE(artifact.has_value()) << diagnosticsOf(back);
    if (!artifact.has_value()) return {};
    auto const x = copyOfX(*artifact);
    EXPECT_TRUE(x.has_value()) << "the artifact defines x";
    return x.has_value() ? x->bytes : std::vector<std::uint8_t>{};
}

[[nodiscard]] std::optional<std::uint32_t> markOf(std::vector<std::uint8_t> const& x) {
    if (x.size() < 12u) return std::nullopt;
    return static_cast<std::uint32_t>(x[8] | (x[9] << 8) | (x[10] << 16) | (x[11] << 24));
}

}  // namespace

// A copy of a section group stands where the input it came from stands, not where the link holds its unit: of two ANY
// copies the link keeps the one whose input comes first in the operator's order. RED-ON-DISABLE: judge the carriers in
// the order the link holds the units (the discard before this fold) -> the unit held first keeps its copy wherever
// its input stood; read no definition's own place -> a unit holding several inputs' definitions stands where its
// first does.
TEST(LinkOrder, AGroupsCopyStandsWhereItsInputStands) {
    struct Case {
        char const*   what;
        Placed        place;
        std::uint32_t mark;   // 1: any_small's copy kept; 7: any_big's
        std::size_t   size;
    };
    std::vector<Case> const cases{
        {"no place stated: the order the link holds them, any_small first", {}, 1, 12},
        {"no place stated: the order the link holds them, any_big first", {std::nullopt, std::nullopt, std::nullopt, true},
         7, 16},
        {"the big copy's input first, its unit held second", {2u, 1u, std::nullopt, false}, 7, 16},
        {"the small copy's input first, its unit held second", {1u, 2u, std::nullopt, true}, 1, 12},
        {"a stated place precedes none, held second", {5u, std::nullopt, std::nullopt, true}, 1, 12},
        // A unit of several inputs (the merge of the compiled units): placed first as a unit, its `x` came from an
        // input after the big copy's.
        {"a definition's own place, past its unit's", {0u, 1u, 3u, false}, 7, 16},
    };
    for (auto const& c : cases) {
        SCOPED_TRACE(c.what);
        auto const x = keptX(c.place);
        ASSERT_FALSE(x.empty());
        EXPECT_EQ(markOf(x), std::optional<std::uint32_t>{c.mark}) << "x[2]: 1 is any_small's copy, 7 any_big's";
        EXPECT_EQ(x.size(), c.size) << "the copy's size";
    }
}

// ══ EVERY ROUTE OF THE DRIVER ════════════════════════════════════════════════════════════════════════════════════════
//
// The driver states each input's place (`program.cpp`, the partition of the operator's inputs into sources and
// objects; `compile_pipeline.cpp`, the link's composition): a named object its own, a unit compiled alone its source's,
// and the merge of the compiled units each definition's source's (`MergedMirModule::unitOfDefinition`). The cells
// below are the measured tables, built by DSS's own driver from the operator's order:
//   * ELF, the C units beside `tests/link/data/elf_link_order_weak1_gcc_O2.o` (a weak `w = 1`; the source and its
//     REBUILD lines: `elf_link_order.source.c`), linked `-r`, then read: GNU ld 2.42 -r and ld.lld 18.1.3 -r (each
//     then linked by gcc 13.3 and run) keep the unit's `w` with the unit first, the object's with the object first;
//   * ELF, the two objects alone (`..._mainown_gcc_O2.o` holds `main` and a weak `w = 5`): the first one's `w`;
//   * PE, the C units beside `coff_comdat_any_small.obj` (a select-any `x` with x[2] = 1) linked `-r` (MinGW-w64
//     GNU ld -r, then gcc 13.2, each program run): the first copy on the command line.
// And where the host runs the image (Linux x86_64 for ELF, Windows x86_64 for PE), DSS's program of each order runs
// to the exit the references' programs gave.

namespace {

constexpr char const* kElfMain = "extern int w;\nint through_own(void);\nint main(void) { return w * 10 + through_own(); }\n";
constexpr char const* kElfOwn  = "int w __attribute__((weak)) = 5;\nint through_own(void) { return w; }\n";
constexpr char const* kPeMain  = "extern int x[];\nint through_own(void);\nint main(void) { return x[2] * 10 + through_own(); }\n";
constexpr char const* kPeOwn   = "__declspec(selectany) int x[3] = {0, 0, 5};\nint through_own(void) { return x[2]; }\n";

constexpr char const* kWeak1   = "elf_link_order_weak1_gcc_O2.o";
constexpr char const* kMainOwn = "elf_link_order_mainown_gcc_O2.o";
constexpr char const* kAnySmall = "coff_comdat_any_small.obj";

// One order of a build: the operator's inputs, each a C unit's text (`main` or `own`) or a fixture's name.
struct Order {
    char const*              label;
    std::vector<char const*> inputs;   // "main", "own" or a fixture
    std::uint32_t            unitsValue;   // what the kept definition holds: the unit's 5, the object's 1
};

// The four orders of `main`, `own` and one object, with whose definition the references keep.
[[nodiscard]] std::vector<Order> fourOrdersWith(char const* object) {
    return {{"the unit before the object", {"main", "own", object}, 5},
            {"the object between the units", {"main", object, "own"}, 1},
            {"the object first", {object, "main", "own"}, 1},
            {"main last", {"own", object, "main"}, 5}};
}

struct Built {
    int         rc = -1;
    std::string diagnostics;
    fs::path    artifact;
};

// DSS's driver builds `order` for `target` in `dir`: each C unit written as `<name>.c`, each fixture copied in.
[[nodiscard]] Built buildOrder(fs::path const& dir, Order const& order, char const* mainText, char const* ownText,
                               std::string const& target) {
    Built                    out;
    std::vector<std::string> inputs;
    for (auto const* in : order.inputs) {
        std::string_view const name{in};
        if (name == "main" || name == "own") {
            auto const path = dir / (std::string{name} + ".c");
            std::ofstream(path, std::ios::binary) << (name == "main" ? mainText : ownText);
            inputs.push_back(path.string());
        } else {
            auto const bytes = fixture(name);
            if (bytes.empty()) return out;
            auto const path = dir / std::string{name};
            std::ofstream(path, std::ios::binary)
                .write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            inputs.push_back(path.string());
        }
    }
    auto const outDir = dir / "dss.out";
    fs::create_directories(outDir);
    Program            p;
    p.setOutputDir(outDir);
    DiagnosticReporter rep;
    out.rc          = p.compileUnits(inputs, "c", std::vector<std::string>{target}, rep);
    out.diagnostics = diagnosticsOf(rep);
    for (auto const& e : fs::directory_iterator(outDir)) {
        if (e.is_regular_file()) out.artifact = e.path();
    }
    return out;
}

// The value of the four-byte datum `name`'s definition holds at `offset` in a relocatable artifact, read back.
[[nodiscard]] std::optional<std::uint32_t> datumOf(fs::path const& artifact, char const* formatName, char const* name,
                                                   std::size_t offset) {
    std::ifstream                    in{artifact, std::ios::binary};
    std::vector<std::uint8_t> const  bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto const                       target = TargetSchema::loadShipped("x86_64");
    auto const                       format = ObjectFormatSchema::loadShipped(formatName);
    EXPECT_TRUE(target.has_value() && format.has_value()) << formatName;
    if (!target.has_value() || !format.has_value()) return std::nullopt;
    DiagnosticReporter                  rep;
    std::optional<AssembledModule> const m =
        std::string_view{formatName}.starts_with("elf")
            ? elf::readRelocatableObject(bytes, **target, **format, rep, CompilationUnitId{1})
            : pe::readRelocatableObject(bytes, **target, **format, rep, CompilationUnitId{1});
    EXPECT_TRUE(m.has_value()) << "the artifact reads back:" << diagnosticsOf(rep);
    if (!m.has_value()) return std::nullopt;
    std::size_t definitions = 0;
    std::optional<std::uint32_t> value;
    for (auto const& s : m->symbols) {
        if (s.name != name) continue;
        for (auto const& d : m->dataItems) {
            if (d.symbol != s.symbol || d.bytes.size() < offset + 4u) continue;
            ++definitions;
            value = static_cast<std::uint32_t>(d.bytes[offset] | (d.bytes[offset + 1] << 8) |
                                               (d.bytes[offset + 2] << 16) | (d.bytes[offset + 3] << 24));
        }
    }
    EXPECT_EQ(definitions, 1u) << "the artifact defines " << name << " once";
    return value;
}

// The four bytes the DEFINED symbol `name` of an ELF64 little-endian SHARED OBJECT names, read through the image's
// `.dynsym` -- the table the loader binds by, so its address is the one every program reads -- by a reader of the
// test's own, so the image is read as any tool reads it, not by the readers under test. The image's `.symtab` is no
// witness: it names no data symbol in an image (D-LINK-ELF-IMAGE-NO-DATA-SYMBOLS-IN-SYMTAB, MEASURED on the exec).
// A row of the name that is undefined (the image's own reference to a definition its loader may replace) is not the
// definition and is passed over; exactly one defined row is required.
[[nodiscard]] std::optional<std::uint32_t> sharedObjectDatumOf(fs::path const& image, std::string_view name) {
    std::ifstream                   in{image, std::ios::binary};
    std::vector<std::uint8_t> const b{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto const u16 = [&](std::size_t o) { return static_cast<std::uint32_t>(b[o] | (b[o + 1] << 8)); };
    auto const u32 = [&](std::size_t o) { return u16(o) | (u16(o + 2) << 16); };
    auto const u64 = [&](std::size_t o) { return static_cast<std::uint64_t>(u32(o)) | (std::uint64_t{u32(o + 4)} << 32); };
    if (b.size() < 64u || b[0] != 0x7F || b[1] != 'E' || b[4] != 2u || b[5] != 1u) {
        ADD_FAILURE() << image.string() << " is no ELF64 little-endian file";
        return std::nullopt;
    }
    std::uint64_t const shoff = u64(0x28);
    std::uint32_t const shentsize = u16(0x3A), shnum = u16(0x3C);
    if (shoff == 0u || shentsize < 64u || shoff + std::uint64_t{shnum} * shentsize > b.size()) {
        ADD_FAILURE() << image.string() << " states no section headers in the file";
        return std::nullopt;
    }
    auto const sh = [&](std::uint32_t i) { return static_cast<std::size_t>(shoff + std::uint64_t{i} * shentsize); };
    std::size_t                  defined = 0;
    std::optional<std::uint32_t> value;
    for (std::uint32_t s = 0; s < shnum; ++s) {
        if (u32(sh(s) + 4) != 11u /*SHT_DYNSYM*/) continue;
        std::size_t const symOff = u64(sh(s) + 0x18), symSize = u64(sh(s) + 0x20);
        std::size_t const strOff = u64(sh(u32(sh(s) + 0x28)) + 0x18);
        for (std::size_t e = symOff; e + 24u <= symOff + symSize && e + 24u <= b.size(); e += 24u) {
            std::string sym;
            for (std::size_t p = strOff + u32(e); p < b.size() && b[p] != 0u; ++p) sym.push_back(static_cast<char>(b[p]));
            std::uint32_t const shndx = u16(e + 6);
            if (sym != name || shndx == 0u /*SHN_UNDEF: a reference*/) continue;
            ++defined;
            if (shndx >= shnum || u32(sh(shndx) + 4) == 8u /*SHT_NOBITS*/) continue;
            std::uint64_t const addr = u64(e + 8), base = u64(sh(shndx) + 0x10), off = u64(sh(shndx) + 0x18);
            std::size_t const   at   = static_cast<std::size_t>(off + (addr - base));
            if (addr >= base && at + 4u <= b.size()) value = u32(at);
        }
    }
    EXPECT_EQ(defined, 1u) << image.string() << "'s .dynsym defines " << name << " once";
    if (!value.has_value()) ADD_FAILURE() << image.string() << "'s .dynsym names no readable definition of " << name;
    return value;
}

}  // namespace

// Every leg: DSS's relocatable link of each order of the C units and a named ELF object keeps the weak `w` the
// references keep -- the first on the command line. RED-ON-DISABLE: state no place (the driver before this fold) ->
// the merged units, held first, keep theirs in every order.
TEST(LinkOrderRoutes, AMergedBuildsRelocatableLinkKeepsTheFirstWeakDefinitionOnTheCommandLine) {
    for (auto const& order : fourOrdersWith(kWeak1)) {
        SCOPED_TRACE(order.label);
        dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "link-order-elf-r"};
        auto const built = buildOrder(scratch.path(), order, kElfMain, kElfOwn, "x86_64:elf64-x86_64-linux");
        ASSERT_EQ(built.rc, 0) << built.diagnostics;
        ASSERT_FALSE(built.artifact.empty());
        EXPECT_EQ(datumOf(built.artifact, "elf64-x86_64-linux", "w", 0), std::optional<std::uint32_t>{order.unitsValue})
            << "w: 5 is the C unit's definition, 1 the object's";
    }
}

// Every leg: DSS's ELF shared object of each order -- an image, linked by the image link's own election -- defines the
// weak `w` the references' programs read (read through the image's `.dynsym`, so no host runs it). RED-ON-DISABLE:
// state no place -> the merged units, held first, keep theirs (MEASURED at run 21's bytes, `dyn_dss.py`: 5 in all
// four orders).
TEST(LinkOrderRoutes, AMergedBuildsSharedObjectDefinesTheFirstWeakDefinitionOnTheCommandLine) {
    for (auto const& order : fourOrdersWith(kWeak1)) {
        SCOPED_TRACE(order.label);
        dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "link-order-elf-dyn"};
        auto const built = buildOrder(scratch.path(), order, kElfMain, kElfOwn, "x86_64:elf64-x86_64-linux-dyn");
        ASSERT_EQ(built.rc, 0) << built.diagnostics;
        ASSERT_FALSE(built.artifact.empty());
        EXPECT_EQ(sharedObjectDatumOf(built.artifact, "w"), std::optional<std::uint32_t>{order.unitsValue})
            << "w: 5 is the C unit's definition, 1 the object's";
    }
}

namespace {

std::vector<Order> const kObjectsAlone = {Order{"the weak object first", {kWeak1, kMainOwn}, 1},
                                          Order{"the object defining main first", {kMainOwn, kWeak1}, 5}};

}  // namespace

// Every leg: a relocatable link of objects alone keeps the weak `w` of the object named first (GNU ld 2.42 -r and
// ld.lld 18.1.3 -r, MEASURED: 1 / 5). NOT RED at the driver before this fold, and not an arm's pin: a relocatable
// document realizes no entry, so no object was elected for defining one and the first named object led the link
// anyway (MEASURED: run 23 passed it). It guards the route. The executable of objects alone -- whose entry-defining
// object the driver made the link's first unit, the defect -- runs in `LinkOrderRoutesNative`: an image's `.symtab`
// names no datum to read it by on every leg (D-LINK-ELF-IMAGE-NO-DATA-SYMBOLS-IN-SYMTAB), and an executable states
// no `.dynsym` definition of it.
TEST(LinkOrderRoutes, AnObjectOnlyRelocatableLinkKeepsTheFirstObjectsWeakDefinition) {
    for (auto const& order : kObjectsAlone) {
        SCOPED_TRACE(order.label);
        dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "link-order-objects-r"};
        auto const built = buildOrder(scratch.path(), order, kElfMain, kElfOwn, "x86_64:elf64-x86_64-linux");
        ASSERT_EQ(built.rc, 0) << built.diagnostics;
        ASSERT_FALSE(built.artifact.empty());
        EXPECT_EQ(datumOf(built.artifact, "elf64-x86_64-linux", "w", 0), std::optional<std::uint32_t>{order.unitsValue})
            << "the relocatable link: w 1 is the weak object's, 5 the main object's";
    }
}

// Every leg: DSS's relocatable PE link of each order of the C units and a named COMDAT copy keeps the first copy on
// the command line. RED-ON-DISABLE: state no definition's place -> the merged units' stated copy, held first, is kept.
TEST(LinkOrderRoutes, AMergedBuildsRelocatablePeLinkKeepsTheFirstCopyOnTheCommandLine) {
    for (auto const& order : fourOrdersWith(kAnySmall)) {
        SCOPED_TRACE(order.label);
        dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "link-order-pe-r"};
        auto const built = buildOrder(scratch.path(), order, kPeMain, kPeOwn, "x86_64:pe64-x86_64-windows");
        ASSERT_EQ(built.rc, 0) << built.diagnostics;
        ASSERT_FALSE(built.artifact.empty());
        EXPECT_EQ(datumOf(built.artifact, "pe64-x86_64-windows", "x", 8), std::optional<std::uint32_t>{order.unitsValue})
            << "x[2]: 5 is the C unit's copy, 1 the object's";
    }
}

// Where the host runs the image: DSS's program of each order exits as the references' did (55: the unit's
// definition, 11: the object's; the objects alone, 1 or 5).
TEST(LinkOrderRoutesNative, EachOrdersProgramReadsTheDefinitionTheReferencesRead) {
    auto const host = std::string{dss::test_support::hostNativeTarget().execTarget};
    bool const elf  = host == "x86_64:elf64-x86_64-linux-exec";
    bool const pe   = host == "x86_64:pe64-x86_64-windows-exec";
    if (!elf && !pe) GTEST_SKIP() << "the images run only where the host is Linux x86_64 or Windows x86_64";
    std::vector<Order> orders = fourOrdersWith(elf ? kWeak1 : kAnySmall);
    if (elf) orders.insert(orders.end(), kObjectsAlone.begin(), kObjectsAlone.end());
    for (auto const& order : orders) {
        SCOPED_TRACE(order.label);
        dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "link-order-run"};
        auto const built = buildOrder(scratch.path(), order, elf ? kElfMain : kPeMain, elf ? kElfOwn : kPeOwn, host);
        ASSERT_EQ(built.rc, 0) << built.diagnostics;
        ASSERT_FALSE(built.artifact.empty());
        auto const run = dss::test_support::runBinary(built.artifact);
        ASSERT_TRUE(run.spawned) << run.diagnostic;
        EXPECT_FALSE(run.timedOut);
        bool const objectsAlone = order.inputs.size() == 2u;
        std::uint32_t const want = objectsAlone ? order.unitsValue : order.unitsValue * 11u;
        EXPECT_EQ(run.exitCode, want) << "55 the unit's definition, 11 the object's; objects alone, 1 or 5";
    }
}
