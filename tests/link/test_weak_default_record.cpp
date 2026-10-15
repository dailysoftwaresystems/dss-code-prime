// P69 fold 3 (lane `xa`) — THE DEFAULT RECORD OF A COFF WEAK DEFINITION: NO IMAGE EXPORTS IT, AND IT KEEPS THE BODY.
// D-LK-WEAK-EXTERNAL-BODY-OUTRANKED-A-SELECT-ANY-DEFINITION-BY-LINK-ORDER
//
// COFF spells an overridable weak definition as TWO records: the weak external, which is the program's name, and
// the DEFAULT it defers to — an EXTERNAL record at the body, named after the weak name (gcc `.weak.<name>.<…>`,
// clang and DSS's own writer `.weak.<name>.default[.<…>]`). Until this fold every DSS-compiled weak definition was
// written as a COMDAT, which has no such record. Once a compiled definition reached the writer as the OVERRIDABLE
// kind it is, two things followed, and each is pinned here:
//
//   * A DSS DLL EXPORTED THE DEFAULT RECORD (`.weak.hidden_weak.default.helper`, ✔MEASURED 2026-10-12, the first
//     run of the fold). No reference does: GNU ld 2.42 and ld.lld 19.1.5, each on MinGW gcc 13.2.0's object and on
//     clang 19.1.5's, 4 of 4 DLLs export neither a default record nor anything named after one, and for gcc's weak
//     alias of a STRONG name both export the target and not the `.weak.<alias>.<target>` record beside it. So the
//     COFF reader reads a default record back HIDDEN — asking both its ROLE (the record a weak external's TagIndex
//     names) and its NAME (`.weak.<that weak name>.`): the role alone would hide a default that is a name of the
//     program, the name alone whatever a source chose to call `.weak.…`.
//
//   * HIDDEN, IT LOST THE BODY. Which of several records at one address owns the body was ranked visible-first
//     (`outranksAsAtomIdentity`), so the weak name — the only visible one left — took it, and a relocatable link
//     that gave the weak name to another unit was left with a `static` name first and the default an external name
//     beside it, which the COFF object writer has no record for: refused (✔MEASURED 2026-10-12, the second run of
//     the fold, on `weak_local_name_x86_64_pe_mingw.o`, which had linked the run before). The ranking now asks
//     LINKAGE first (the binding alone), then STRONG before weak, and visibility only among what is left — so what
//     a name is exported as never moves a body.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/object_atom_coverage.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"
#include "repo_root.hpp"
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
#include <vector>

using namespace dss;

namespace {

using link::format::AtomStartCandidate;
using link::format::outranksAsAtomIdentity;

[[nodiscard]] AtomStartCandidate candidate(std::uint32_t id, SymbolBinding binding,
                                           SymbolVisibility visibility = SymbolVisibility::Default) {
    AtomStartCandidate c;
    c.sectionKey  = 1;
    c.offset      = 0;
    c.symbolId    = id;
    c.binding     = binding;
    c.visibility  = visibility;
    c.name        = "n" + std::to_string(id);
    c.sectionName = ".text";
    return c;
}

// `winner` outranks `loser` and not the other way round: the ranking is a strict order, asked both ways.
void expectOutranks(AtomStartCandidate const& winner, AtomStartCandidate const& loser, char const* why) {
    EXPECT_TRUE(outranksAsAtomIdentity(winner, loser)) << why;
    EXPECT_FALSE(outranksAsAtomIdentity(loser, winner)) << why;
}

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> format;
};
[[nodiscard]] Loaded loadPe() {
    Loaded l;
    auto   t = TargetSchema::loadShipped("x86_64");
    auto   f = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    EXPECT_TRUE(t.has_value() && f.has_value());
    if (t.has_value()) l.target = *t;
    if (f.has_value()) l.format = *f;
    return l;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

[[nodiscard]] std::vector<std::uint8_t> referenceObject(char const* name) {
    auto const    path = dss::test::repoRoot() / "tests" / "link" / "data" / name;
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        ADD_FAILURE() << "cannot open " << path.string();
        return {};
    }
    std::string const raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    return std::vector<std::uint8_t>(raw.begin(), raw.end());
}

[[nodiscard]] std::optional<AssembledModule> readCoff(Loaded const& L, std::span<std::uint8_t const> bytes) {
    DiagnosticReporter rep;
    auto               m = pe::readRelocatableObject(bytes, *L.target, *L.format, rep, CompilationUnitId{1});
    EXPECT_TRUE(m.has_value()) << diagnosticsOf(rep);
    return m;
}

// One unit linked into a relocatable artifact — the unit as DSS's own COFF writer writes it.
[[nodiscard]] std::vector<std::uint8_t> writtenByDss(Loaded const& L, AssembledModule const& unit) {
    std::vector<AssembledModule> const mods{unit};
    DiagnosticReporter                 rep;
    auto const out = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
    EXPECT_TRUE(out.ok()) << diagnosticsOf(rep);
    return out.ok() ? out.bytes : std::vector<std::uint8_t>{};
}

[[nodiscard]] ModuleSymbol const* rowNamed(AssembledModule const& m, std::string_view name) {
    for (auto const& s : m.symbols) {
        if (s.name == name) return &s;
    }
    return nullptr;
}
[[nodiscard]] ModuleSymbol const* firstRowOf(AssembledModule const& m, SymbolId id) {
    for (auto const& s : m.symbols) {
        if (s.symbol == id) return &s;
    }
    return nullptr;
}

// What every weak definition of a module read from COFF must say: the weak name is the program's (overridable,
// of default visibility), and the record its bytes are under — the FIRST row of its body — is its default:
// external, named after it, hidden. Returns how many weak names it judged.
[[nodiscard]] std::size_t judgeEveryWeakDefinition(AssembledModule const& m) {
    std::size_t judged = 0;
    for (auto const& weak : m.symbols) {
        if (weak.binding != SymbolBinding::Weak) continue;
        SCOPED_TRACE("the weak name '" + weak.name + "'");
        ++judged;
        EXPECT_EQ(weak.weakKind, std::optional<WeakDefinitionKind>{WeakDefinitionKind::Overridable});
        EXPECT_EQ(weak.visibility, SymbolVisibility::Default) << "the weak name is the name of the program";
        auto const* first = firstRowOf(m, weak.symbol);
        EXPECT_NE(first, nullptr);
        if (first == nullptr) continue;
        EXPECT_TRUE(std::string_view{first->name}.starts_with(".weak." + weak.name + "."))
            << "the body's first row must be the default record, the record the bytes belong to; it is '"
            << first->name << "'";
        EXPECT_EQ(first->binding, SymbolBinding::Global) << "the default is an EXTERNAL record";
        EXPECT_EQ(first->visibility, SymbolVisibility::Hidden) << "and no image exports it";
    }
    return judged;
}

// CU 1: a strong function `anchor`, and a datum `shared` whose one name is an OVERRIDABLE weak definition — what
// a unit DSS compiled `__attribute__((weak)) int shared = 7;` in hands the writer.
[[nodiscard]] AssembledModule unitWithAnOverridableWeakDatum() {
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    AssembledFunction f;
    f.symbol = SymbolId{1};
    f.bytes  = {0x31, 0xC0, 0xC3};
    m.functions.push_back(std::move(f));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "anchor", SymbolBinding::Global, SymbolVisibility::Default});
    AssembledData d;
    d.symbol    = SymbolId{2};
    d.section   = DataSectionKind::Data;
    d.bytes     = {7, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    m.dataItems.push_back(std::move(d));
    ModuleSymbol row{SymbolId{2}, "shared", SymbolBinding::Weak, SymbolVisibility::Default};
    row.weakKind = WeakDefinitionKind::Overridable;
    m.symbols.push_back(std::move(row));
    m.expectedFuncCount = m.functions.size();
    return m;
}

// CU 1: a strong datum `first`, a second name `second` on the same bytes with `binding`.
[[nodiscard]] AssembledModule unitWithTwoNamesOnOneDatum(std::string const& first, std::string const& second,
                                                         SymbolBinding secondBinding) {
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    AssembledData d;
    d.symbol    = SymbolId{1};
    d.section   = DataSectionKind::Data;
    d.bytes     = {7, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    m.dataItems.push_back(std::move(d));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, first, SymbolBinding::Global, SymbolVisibility::Default});
    ModuleSymbol alias{SymbolId{1}, second, secondBinding, SymbolVisibility::Default};
    if (secondBinding == SymbolBinding::Weak) alias.weakKind = WeakDefinitionKind::Overridable;
    m.symbols.push_back(std::move(alias));
    return m;
}

}  // namespace

// ── WHICH RECORD OWNS A BODY SEVERAL RECORDS NAME ───────────────────────────────────────────────────────────────

TEST(AtomIdentityRanking, ANameALinkerResolvesOutranksAModulePrivateOneWhateverItsVisibility) {
    auto const local = candidate(1, SymbolBinding::Local);
    for (SymbolVisibility v : {SymbolVisibility::Default, SymbolVisibility::Protected, SymbolVisibility::Hidden,
                               SymbolVisibility::Internal}) {
        expectOutranks(candidate(9, SymbolBinding::Global, v), local,
                       "a strong external name is the one a static link resolves, hidden or not");
        expectOutranks(candidate(9, SymbolBinding::Weak, v), local,
                       "a weak external name is the one a static link resolves, hidden or not");
    }
}

TEST(AtomIdentityRanking, AStrongNameOutranksAWeakOneWhateverTheirVisibility) {
    for (SymbolVisibility strongVis : {SymbolVisibility::Default, SymbolVisibility::Hidden}) {
        for (SymbolVisibility weakVis : {SymbolVisibility::Default, SymbolVisibility::Hidden}) {
            expectOutranks(candidate(9, SymbolBinding::Global, strongVis), candidate(1, SymbolBinding::Weak, weakVis),
                           "the strong record is the one the weak one defers to");
        }
    }
}

TEST(AtomIdentityRanking, AVisibleNameOutranksAHiddenOneOfItsStrength) {
    expectOutranks(candidate(9, SymbolBinding::Global), candidate(1, SymbolBinding::Global, SymbolVisibility::Hidden),
                   "the name a source wrote, ahead of plumbing beside it");
    expectOutranks(candidate(9, SymbolBinding::Weak), candidate(1, SymbolBinding::Weak, SymbolVisibility::Hidden),
                   "the name a source wrote, ahead of plumbing beside it");
}

TEST(AtomIdentityRanking, OtherwiseTheRecordTheObjectListedFirstOwnsTheBody) {
    for (SymbolBinding b : {SymbolBinding::Local, SymbolBinding::Global, SymbolBinding::Weak}) {
        expectOutranks(candidate(1, b), candidate(9, b), "the lowest id, so that the answer is total");
    }
}

// The shape that lost its body: MinGW gcc's weak alias of a `static` — the `static` name, the weak external, and
// the weak external's EXTERNAL default record, all at one address — with the default read back hidden.
TEST(AtomIdentityRanking, AHiddenDefaultRecordKeepsTheBodyBesideItsWeakNameAndAStaticOne) {
    std::vector<AtomStartCandidate> const set{candidate(3, SymbolBinding::Local),
                                              candidate(7, SymbolBinding::Weak),
                                              candidate(5, SymbolBinding::Global, SymbolVisibility::Hidden)};
    std::vector<std::uint32_t> owner;
    DiagnosticReporter         rep;
    ASSERT_TRUE(link::format::resolveEqualOffsetAtomAliases(set, owner, "the test", rep)) << diagnosticsOf(rep);
    ASSERT_EQ(owner.size(), set.size());
    for (std::size_t i = 0; i < set.size(); ++i) {
        EXPECT_EQ(owner[i], 5u) << "candidate " << set[i].symbolId << " must name the default record's body";
    }
}

// ── WHAT THE COFF READER SAYS OF A DEFAULT RECORD ───────────────────────────────────────────────────────────────

// The compilers' own objects (`tests/link/data`): MinGW gcc 13.2.0 and clang 19.1.5 for a weak alias of a
// `static` (a datum and a function), and MinGW gcc for a weak definition in a unit with no strong one (its
// default is `.weak.dss_tentative.` — the name ends at the dot).
TEST(CoffWeakDefaultRecord, ACompilersDefaultRecordReadsBackHiddenAndOwnsTheBody) {
    auto const L = loadPe();
    ASSERT_TRUE(L.target && L.format);
    struct Fixture {
        char const* file;
        std::size_t weakNames;
    };
    for (auto const& fx : {Fixture{"weak_local_name_x86_64_pe_mingw.o", 2}, Fixture{"weak_local_name_x86_64_pe_clang.obj", 2},
                           Fixture{"tentative_weak_x86_64_pe_mingw.o", 1}}) {
        SCOPED_TRACE(fx.file);
        auto const bytes = referenceObject(fx.file);
        ASSERT_FALSE(bytes.empty());
        auto const object = readCoff(L, bytes);
        ASSERT_TRUE(object.has_value());
        EXPECT_EQ(judgeEveryWeakDefinition(*object), fx.weakNames);
    }
}

TEST(CoffWeakDefaultRecord, DssOwnDefaultRecordReadsBackHiddenAndOwnsTheBody) {
    auto const L = loadPe();
    ASSERT_TRUE(L.target && L.format);
    auto const bytes = writtenByDss(L, unitWithAnOverridableWeakDatum());
    ASSERT_FALSE(bytes.empty());
    auto const object = readCoff(L, bytes);
    ASSERT_TRUE(object.has_value());
    EXPECT_EQ(judgeEveryWeakDefinition(*object), 1u);
    auto const* weak = rowNamed(*object, "shared");
    ASSERT_NE(weak, nullptr);
    auto const* first = firstRowOf(*object, weak->symbol);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->name, ".weak.shared.default.anchor")
        << "the writer names the default after the weak name and the unit's first strong definition";
    auto const* anchor = rowNamed(*object, "anchor");
    ASSERT_NE(anchor, nullptr);
    EXPECT_EQ(anchor->visibility, SymbolVisibility::Default) << "the control: an ordinary definition stays visible";
}

// The ROLE alone must not hide a record: a weak alias of a STRONG name is written as a weak external whose
// default is the target's own record, and the target is a name of the program.
TEST(CoffWeakDefaultRecord, ADefaultThatIsANameOfTheProgramStaysVisible) {
    auto const L = loadPe();
    ASSERT_TRUE(L.target && L.format);
    auto const bytes = writtenByDss(L, unitWithTwoNamesOnOneDatum("target", "w", SymbolBinding::Weak));
    ASSERT_FALSE(bytes.empty());
    auto const object = readCoff(L, bytes);
    ASSERT_TRUE(object.has_value());
    auto const* target = rowNamed(*object, "target");
    auto const* weak   = rowNamed(*object, "w");
    ASSERT_NE(target, nullptr);
    ASSERT_NE(weak, nullptr);
    EXPECT_EQ(weak->binding, SymbolBinding::Weak) << "the control: the weak name was written as a weak external";
    EXPECT_EQ(weak->symbol, target->symbol) << "the control: and `target` is the record it defers to";
    EXPECT_EQ(target->binding, SymbolBinding::Global);
    EXPECT_EQ(target->visibility, SymbolVisibility::Default)
        << "`target` is the default of `w` and is not named after it: it is the program's name and stays exported";
}

// The NAME alone must not hide a record either: a definition a source called `.weak.<…>.<…>` that no weak
// external defers to is an ordinary definition — and so is a second strong name of such a body.
TEST(CoffWeakDefaultRecord, ARecordNamedLikeADefaultThatNoWeakNameDefersToStaysVisible) {
    auto const L = loadPe();
    ASSERT_TRUE(L.target && L.format);
    auto const bytes =
        writtenByDss(L, unitWithTwoNamesOnOneDatum(".weak.shared.other", ".weak.shared.second", SymbolBinding::Global));
    ASSERT_FALSE(bytes.empty());
    auto const object = readCoff(L, bytes);
    ASSERT_TRUE(object.has_value());
    for (char const* name : {".weak.shared.other", ".weak.shared.second"}) {
        SCOPED_TRACE(name);
        auto const* row = rowNamed(*object, name);
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->binding, SymbolBinding::Global);
        EXPECT_EQ(row->visibility, SymbolVisibility::Default);
    }
}

// A unit that is written again states its hidden default in the object's own directives, and the next reader
// meets it hidden BEFORE it decides which record owns the body. The default must keep the body through every
// round, and the unit must still be writable: with the body under the weak name the writer would mint the
// default's name a second time and refuse the object for holding it already.
TEST(CoffWeakDefaultRecord, WrittenAndReadAgainTheDefaultStillOwnsTheBody) {
    auto const L = loadPe();
    ASSERT_TRUE(L.target && L.format);
    struct Start {
        char const*               what;
        std::vector<std::uint8_t> bytes;
        std::size_t               weakNames;
    };
    std::vector<Start> starts;
    starts.push_back({"MinGW gcc's object", referenceObject("weak_local_name_x86_64_pe_mingw.o"), 2});
    starts.push_back({"clang's object", referenceObject("weak_local_name_x86_64_pe_clang.obj"), 2});
    starts.push_back({"DSS's own object", writtenByDss(L, unitWithAnOverridableWeakDatum()), 1});
    for (auto const& start : starts) {
        SCOPED_TRACE(start.what);
        ASSERT_FALSE(start.bytes.empty());
        std::vector<std::uint8_t> bytes = start.bytes;
        for (int round = 1; round <= 3; ++round) {
            SCOPED_TRACE("round " + std::to_string(round));
            auto const object = readCoff(L, bytes);
            ASSERT_TRUE(object.has_value());
            EXPECT_EQ(judgeEveryWeakDefinition(*object), start.weakNames);
            bytes = writtenByDss(L, *object);
            ASSERT_FALSE(bytes.empty()) << "the unit as read must be writable again";
        }
    }
}

// ══ The unwind records of an OVERRIDABLE function name its BYTES, never its weak name ═══════════════════════════════
//
// P69 fold 3b (the review of fold 3a, F1). A function DSS compiles `__attribute__((weak))` is written on PE as a WEAK
// EXTERNAL: its name a reference another object's definition may win, its body under a default record of its own.
// The `.pdata` RUNTIME_FUNCTION this writer states for the body must name the DEFAULT record: written against the
// weak name, a link beside a strong definition of it would give the image a RUNTIME_FUNCTION at the OVERRIDE's address
// carrying THIS body's unwind codes — a program that links and unwinds wrong at its first exception. Every `.pdata`
// image-relative relocation is read off the object DSS writes: none names the weak external, the body's name its
// default record, and the strong function beside it names itself (the control that the relocations are read at all).
namespace {

struct CoffPdataRelocation {
    std::uint32_t offset = 0;
    std::uint16_t type   = 0;
    std::string   symbol;
};

[[nodiscard]] std::uint32_t le32(std::span<std::uint8_t const> b, std::size_t at) {
    return at + 4 <= b.size() ? static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8
                                    | static_cast<std::uint32_t>(b[at + 2]) << 16
                                    | static_cast<std::uint32_t>(b[at + 3]) << 24
                              : 0u;
}
[[nodiscard]] std::uint16_t le16(std::span<std::uint8_t const> b, std::size_t at) {
    return at + 2 <= b.size() ? static_cast<std::uint16_t>(b[at] | b[at + 1] << 8) : std::uint16_t{0};
}

// Every relocation of the object's `.pdata` section, with the name of the record it names (PE/COFF §5.2, §5.4).
[[nodiscard]] std::vector<CoffPdataRelocation> pdataRelocationsOf(std::span<std::uint8_t const> obj) {
    std::vector<CoffPdataRelocation> out;
    if (obj.size() < 20) return out;
    std::uint16_t const nSections   = le16(obj, 2);
    std::uint32_t const symTable    = le32(obj, 8);
    std::uint32_t const nSymbols    = le32(obj, 12);
    std::uint16_t const optHeader   = le16(obj, 16);
    std::size_t const   strings     = static_cast<std::size_t>(symTable) + std::size_t{nSymbols} * 18u;
    auto const          nameOfIndex = [&](std::uint32_t index) -> std::string {
        std::size_t const at = static_cast<std::size_t>(symTable) + std::size_t{index} * 18u;
        if (at + 18 > obj.size()) return {};
        if (le32(obj, at) == 0u) {   // a long name: an offset into the string table
            std::size_t p = strings + le32(obj, at + 4);
            std::string n;
            while (p < obj.size() && obj[p] != 0) n.push_back(static_cast<char>(obj[p++]));
            return n;
        }
        std::string n;
        for (std::size_t k = 0; k < 8 && obj[at + k] != 0; ++k) n.push_back(static_cast<char>(obj[at + k]));
        return n;
    };
    for (std::uint16_t s = 0; s < nSections; ++s) {
        std::size_t const hdr = 20u + optHeader + std::size_t{s} * 40u;
        if (hdr + 40 > obj.size()) break;
        std::string name;
        for (std::size_t k = 0; k < 8 && obj[hdr + k] != 0; ++k) name.push_back(static_cast<char>(obj[hdr + k]));
        if (name != ".pdata") continue;
        std::uint32_t const relocs  = le32(obj, hdr + 24);
        std::uint16_t const nRelocs = le16(obj, hdr + 32);
        for (std::uint16_t r = 0; r < nRelocs; ++r) {
            std::size_t const at = static_cast<std::size_t>(relocs) + std::size_t{r} * 10u;
            out.push_back(CoffPdataRelocation{le32(obj, at), le16(obj, at + 8), nameOfIndex(le32(obj, at + 4))});
        }
    }
    return out;
}

}  // namespace

TEST(CoffWeakDefaultRecord, AnOverridableFunctionsUnwindRecordsNameItsBytesNeverItsWeakName) {
    auto const L = loadPe();
    ASSERT_TRUE(L.target && L.format);
    test_support::ScratchDir scratch{test_support::Location::Temp, "weak-pdata"};
    auto const               dir = scratch.path();
    std::ofstream(dir / "hook.c", std::ios::binary)
        << "__attribute__((weak)) int dss_hook(int x) { volatile int pad[4]; pad[x & 3] = x; return pad[0] + 1; }\n"
           "int dss_caller(void) { return dss_hook(3); }\n";
    for (CompileConfig const config : {CompileConfig::Debug, CompileConfig::Release}) {
        SCOPED_TRACE(config == CompileConfig::Debug ? "debug" : "release");
        auto const out = dir / (config == CompileConfig::Debug ? "debug" : "release");
        std::filesystem::create_directories(out);
        Program prog;
        prog.setOutputDir(out);
        prog.setCompileConfig(config);
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileFiles({(dir / "hook.c").string()}, "c",
                                    std::vector<std::string>{"x86_64:pe64-x86_64-windows"}, rep),
                  0)
            << diagnosticsOf(rep);
        std::filesystem::path obj;
        for (auto const& e : std::filesystem::directory_iterator(out)) {
            if (e.is_regular_file() && e.path().extension() == ".obj") obj = e.path();
        }
        ASSERT_FALSE(obj.empty()) << "the object DSS wrote";
        std::ifstream                   in{obj, std::ios::binary};
        std::string const               raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        std::vector<std::uint8_t> const bytes(raw.begin(), raw.end());
        auto const relocs = pdataRelocationsOf(bytes);
        ASSERT_FALSE(relocs.empty()) << "both functions carry unwind records";
        std::size_t toDefault = 0, toCaller = 0;
        for (auto const& r : relocs) {
            SCOPED_TRACE(r.symbol);
            EXPECT_EQ(r.type, 3u) << "IMAGE_REL_AMD64_ADDR32NB: an image-relative field";
            EXPECT_NE(r.symbol, "dss_hook")
                << "a RUNTIME_FUNCTION of the weak body written against its weak NAME describes the override's "
                   "address with this body's unwind codes wherever another object defines the name";
            if (r.symbol.starts_with(".weak.dss_hook.")) ++toDefault;
            if (r.symbol == "dss_caller") ++toCaller;
        }
        EXPECT_EQ(toDefault, 2u) << "the body's RUNTIME_FUNCTION names its begin and its end by the default record";
        EXPECT_EQ(toCaller, 2u) << "CONTROL: the strong function's names itself";
    }
}
