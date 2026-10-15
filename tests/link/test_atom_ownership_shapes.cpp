// P69 fold 3b (lane `xa`) — THE TWO OWNERSHIP SHAPES FOLD 3a's RANKING CHANGED, on the references' ELF and Mach-O
// objects. D-LK-WEAK-EXTERNAL-BODY-OUTRANKED-A-SELECT-ANY-DEFINITION-BY-LINK-ORDER
//
// Which of several records at one address owns the body is ranked by `outranksAsAtomIdentity`
// (`link/format/object_atom_coverage.hpp`): LINKAGE first (the binding alone), then STRONG before weak, then visible
// before hidden, then the record the object listed first. Fold 3a found the ranking on COFF objects and pinned it
// there (`test_weak_default_record`); two of the sets whose owner it moved are spelled by the other two families'
// compilers too, and were not measured on them:
//
//   A  a MODULE-PRIVATE name and an external HIDDEN name of one datum (`static int l = 7;` with a hidden alias): the
//      body goes to the external one (it went to the lowest id);
//   B  a VISIBLE WEAK name and a HIDDEN STRONG name of one datum (a hidden `int s = 7;` with a weak alias): the body
//      goes to the strong one (it went to the weak one, the only visible name) — and a unit that defines the weak
//      name strongly then wins the NAME without taking the strong name's body.
//
// ✔MEASURED 2026-10-15 on the references' own objects and linkers (`tests/link/data/owner_*.source.c` and
// `owner_*.<arch>.source.s` say how each fixture was built; the logs are lane xa's `sb6/owner/`):
//   * ELF — gcc 13.3.0 and clang 18.1.3 under GNU ld 2.42 and ld.lld 18.1.3 (x86_64), aarch64-linux-gnu-gcc 13.3.0
//     under GNU ld 2.42 run by qemu-aarch64: every cell 42, both object orders and after a relocatable link (`ld -r`)
//     of the shape's object alone (50 of 50 on the probe's objects, 30 of 30 on the committed bytes);
//   * Mach-O — Apple's assembler, ld-1267 (arm64, x86_64) and `-ld_classic` (arm64): A 42 and B alone 42 under all
//     three, both orders. B OVERRIDDEN reads 11 under ld-1267 in both orders, on both architectures: the strong
//     name's reads reach the override, because Apple's ld replaces a weak definition WHOLE, every label at its
//     address included — the Mach-O documents' `supersededDefinition: replacedWhole`, which DSS follows; the
//     deprecated `-ld_classic` alone keeps the strong label's body (42). After `ld -r` of the shape's object alone,
//     B overridden reads 11 under both Apple linkers, and A is REFUSED by both (`Undefined symbols`): Apple's `ld -r`
//     makes a private extern module-private unless told `-keep_private_externs`.
// Each cell here is DSS's link of the same objects (its own `main` beside them, and for B a DSS object defining the
// weak name), both orders, and the same after DSS's relocatable link of the shape's object alone: the table's
// answer where it runs, and on every leg the owner the reader ranks — the record the body's bytes are under.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/compile_pipeline.hpp"
#include "program/program.hpp"
#include "arm_verdict_ledger.hpp"
#include "host_translations.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

void writeText(fs::path const& p, std::string_view text) { std::ofstream(p, std::ios::binary) << text; }

struct Family {
    char const* label;
    char const* os;          // where its images run (`currentHostOs`)
    char const* arch;        // the target, and the cross-arch gate's key
    char const* object;      // the relocatable document
    char const* image;       // the exec document
    char const* suffix;      // the fixture's `<shape>_<suffix>.o`
    char const* prefix;      // the format's C decoration
    char const* emulator;    // the manifest emulator a foreign arch of this OS runs under ("" = none declared)
    unsigned    bOverridden; // the table's B cell with the weak name overridden (the format's `supersededDefinition`)
};

constexpr Family kFamilies[] = {
    {"ELF x86_64", "linux", "x86_64", "elf64-x86_64-linux", "elf64-x86_64-linux-exec", "x86_64_elf", "", "", 42},
    {"ELF aarch64", "linux", "arm64", "elf64-aarch64-linux", "elf64-aarch64-linux-exec", "aarch64_elf", "",
     "qemu-aarch64", 42},
    {"Mach-O arm64", "darwin", "arm64", "macho64-arm64-darwin", "macho64-arm64-darwin-exec", "arm64_macho", "_", "",
     11},
    {"Mach-O x86_64", "darwin", "x86_64", "macho64-x86_64-darwin", "macho64-x86_64-darwin-exec", "x86_64_macho", "_",
     "", 11},
};

[[nodiscard]] std::string specOf(char const* target, char const* format) {
    return std::string{target} + ":" + format;
}

[[nodiscard]] fs::path fixture(char const* shape, Family const& fam) {
    return dss::test::repoRoot() / "tests" / "link" / "data" / (std::string{shape} + "_" + fam.suffix + ".o");
}

// The one regular file under `dir` whose extension is `ext` (empty: a file with none).
[[nodiscard]] fs::path onlyFileIn(fs::path const& dir, std::string_view ext) {
    fs::path    found;
    std::size_t n = 0;
    if (fs::exists(dir)) {
        for (auto const& e : fs::directory_iterator(dir)) {
            if (e.is_regular_file() && e.path().extension().string() == ext) {
                found = e.path();
                ++n;
            }
        }
    }
    return n == 1u ? found : fs::path{};
}

// DSS builds `inputs` (absolute paths) for `spec` into `<out>`; the artifact of extension `ext`, or empty.
[[nodiscard]] fs::path dssBuilds(std::vector<fs::path> const& inputs, std::string const& spec, fs::path const& out,
                                 std::string_view ext, DiagnosticReporter& rep) {
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    std::vector<std::string> paths;
    for (auto const& i : inputs) paths.push_back(i.string());
    if (p.compileFiles(paths, "c", std::vector<std::string>{spec}, rep) != 0) return {};
    return onlyFileIn(out, ext);
}

// How THIS host runs `fam`'s images: nullopt when it does not (said, under the strict-arm rule).
struct Runner {
    std::vector<std::string> launcher;
    bool                     launcherExecsImage = false;
};
[[nodiscard]] std::optional<Runner> runnerFor(Family const& fam) {
    if (test_support::currentHostOs() != fam.os) return std::nullopt;   // another OS's image: built, never run
    Runner r;
    if (fam.arch != test_support::currentHostArch()) {
        auto const gate = test_support::crossArchDecisionForThisHost(fam.arch, fam.emulator);
        if (!gate.runs) {
            auto const strict = test_support::readStrictArmVerdicts();
            if (strict.on && test_support::armVerdictIsEnvironmentalSkip(gate.skip)) {
                ADD_FAILURE() << fam.label << ": " << gate.why;
            } else {
                std::cout << "[native-arm] " << fam.label << ": not run — " << gate.why << "\n";
            }
            return std::nullopt;
        }
        r.launcher           = gate.launcherPrefix;
        r.launcherExecsImage = gate.launcherExecsImage;
    }
    return r;
}

[[nodiscard]] std::optional<std::uint32_t> ran(Runner const& r, fs::path const& image) {
    auto const res = test_support::runBinary(image, test_support::kRunBudget, /*captureStdout=*/false, r.launcher,
                                             /*programArgs=*/{}, r.launcherExecsImage);
    EXPECT_TRUE(res.spawned) << image << ": " << res.diagnostic;
    EXPECT_FALSE(res.timedOut) << image;
    if (!res.spawned || res.timedOut) return std::nullopt;
    return res.exitCode;
}

constexpr char const* kMainA =
    "extern int dss_owned_hidden;\nint dss_read_owned_local(void);\n"
    "int main(void) {\n  int r = 0;\n  if (dss_owned_hidden != 7) r |= 1;\n  dss_owned_hidden = 9;\n"
    "  if (dss_read_owned_local() != 9) r |= 2;\n  return r ? 10 + r : 42;\n}\n";
constexpr char const* kMainB =
    "int dss_read_owned_strong(void);\nint dss_read_owned_weak(void);\nextern int dss_owned_weak;\n"
    "int main(void) {\n  int r = 0;\n  if (dss_read_owned_strong() != 7) r |= 1;\n  if (dss_owned_weak != 9) r |= 2;\n"
    "  if (dss_read_owned_weak() != 9) r |= 4;\n  return r ? 10 + r : 42;\n}\n";
constexpr char const* kMainBAlone =
    "int dss_read_owned_strong(void);\nint dss_read_owned_weak(void);\nextern int dss_owned_weak;\n"
    "int main(void) {\n  int r = 0;\n  if (dss_read_owned_strong() != 7) r |= 1;\n  if (dss_owned_weak != 7) r |= 2;\n"
    "  dss_owned_weak = 9;\n  if (dss_read_owned_strong() != 9) r |= 4;\n  return r ? 10 + r : 42;\n}\n";
constexpr char const* kOverride = "int dss_owned_weak = 9;\n";

}  // namespace

// The READER's answer, on every leg: both names are rows of one body, and the body's bytes are under the record the
// ranking chooses — A's external hidden name, B's strong hidden one.
TEST(AtomOwnershipShapes, TheReaderGivesTheBodyToTheRecordTheRankingChooses) {
    struct Shape {
        char const* file;
        char const* owner;
        char const* other;
    };
    for (Family const& fam : kFamilies) {
        SCOPED_TRACE(fam.label);
        auto const t = TargetSchema::loadShipped(fam.arch);
        auto const f = ObjectFormatSchema::loadShipped(fam.object);
        ASSERT_TRUE(t.has_value() && f.has_value());
        for (Shape const& s : {Shape{"owner_static_hidden", "dss_owned_hidden", "dss_owned_local"},
                               Shape{"owner_weak_strong", "dss_owned_strong", "dss_owned_weak"}}) {
            SCOPED_TRACE(s.file);
            fs::path const   obj = fixture(s.file, fam);
            ASSERT_TRUE(fs::exists(obj)) << obj;
            DiagnosticReporter rep;
            auto const         mods = readObjectInputModules(std::span<fs::path const>{&obj, 1}, **t, **f, rep);
            ASSERT_TRUE(mods.has_value() && mods->size() == 1u) << diagnosticsOf(rep);
            AssembledModule const& m = mods->front();
            std::string const      owner = std::string{fam.prefix} + s.owner;
            std::string const      other = std::string{fam.prefix} + s.other;
            ModuleSymbol const*    ownerRow = nullptr;
            ModuleSymbol const*    otherRow = nullptr;
            for (auto const& row : m.symbols) {
                if (row.name == owner) ownerRow = &row;
                if (row.name == other) otherRow = &row;
            }
            ASSERT_NE(ownerRow, nullptr) << owner;
            ASSERT_NE(otherRow, nullptr) << other << " — the shape needs both names; a fixture that lost one proves "
                                                    "nothing about which owns the body";
            EXPECT_EQ(ownerRow->symbol, otherRow->symbol) << "both names are rows of ONE body";
            ModuleSymbol const* first = nullptr;
            for (auto const& row : m.symbols) {
                if (row.symbol == ownerRow->symbol) {
                    first = &row;
                    break;
                }
            }
            ASSERT_NE(first, nullptr);
            EXPECT_EQ(first->name, owner) << "the body's first record — its owner — must be the one the ranking "
                                             "chooses (linkage, then strength, then visibility)";
        }
    }
}

// DSS's links of the same objects, both orders, and after DSS's relocatable link of the shape's object alone: built
// on every leg, run where the family runs, 42 as under every reference linker.
TEST(AtomOwnershipShapes, DssLinksEachShapeAsTheReferenceLinkersDo) {
    test_support::ScratchDir scratch{test_support::Location::Temp, "atom-ownership"};
    auto const               dir = scratch.path();
    writeText(dir / "main_a.c", kMainA);
    writeText(dir / "main_b.c", kMainB);
    writeText(dir / "main_b_alone.c", kMainBAlone);
    writeText(dir / "override.c", kOverride);
    for (Family const& fam : kFamilies) {
        SCOPED_TRACE(fam.label);
        std::string const fl = fam.suffix;
        DiagnosticReporter overrideRep;
        fs::path const     override = dssBuilds({dir / "override.c"}, specOf(fam.arch, fam.object),
                                                dir / ("override-" + fl), ".o", overrideRep);
        ASSERT_FALSE(override.empty()) << diagnosticsOf(overrideRep);
        auto const runner = runnerFor(fam);
        for (bool const relinked : {false, true}) {
            SCOPED_TRACE(relinked ? "after DSS's relocatable link of the shape's object" : "the reference's object");
            auto const shapeObject = [&](char const* shape) -> fs::path {
                if (!relinked) return fixture(shape, fam);
                DiagnosticReporter rep;
                fs::path const     r = dssBuilds({fixture(shape, fam)}, specOf(fam.arch, fam.object),
                                                 dir / (std::string{"relinked-"} + shape + "-" + fl), ".o", rep);
                EXPECT_FALSE(r.empty()) << "DSS's relocatable link of " << shape << " alone:" << diagnosticsOf(rep);
                return r;
            };
            fs::path const a = shapeObject("owner_static_hidden");
            fs::path const b = shapeObject("owner_weak_strong");
            ASSERT_FALSE(a.empty() || b.empty());
            struct Cell {
                char const*           name;
                std::vector<fs::path> inputs;
                unsigned              table;
            };
            std::vector<Cell> const cells{
                {"A_object_first", {a, dir / "main_a.c"}, 42u},
                {"A_main_first", {dir / "main_a.c", a}, 42u},
                {"B_owner_then_override", {dir / "main_b.c", b, override}, fam.bOverridden},
                {"B_override_then_owner", {dir / "main_b.c", override, b}, fam.bOverridden},
                {"B_alone", {dir / "main_b_alone.c", b}, 42u},
            };
            for (Cell const& cell : cells) {
                SCOPED_TRACE(cell.name);
                DiagnosticReporter rep;
                fs::path const     image = dssBuilds(cell.inputs, specOf(fam.arch, fam.image),
                                                     dir / (std::string{"img-"} + cell.name + "-" + fl
                                                            + (relinked ? "-r" : "")),
                                                     "", rep);
                ASSERT_FALSE(image.empty()) << "DSS must link the shape as every reference linker does:"
                                            << diagnosticsOf(rep);
                if (!runner) continue;
                if (auto const exit = ran(*runner, image)) {
                    EXPECT_EQ(*exit, cell.table)
                        << "A: 11/12/13 = the two names are two objects; B: 11 = the strong name's reads reach the "
                           "override (its body replaced whole), 12 = the override did not win the weak name, 14 = the "
                           "object's own reference through the weak name did not follow it";
                }
            }
        }
    }
}

// ══ What becomes of a superseded definition's OTHER names — the document's three answers ═══════════════════════════
//
// A definition that loses one name (`contested`, weak, won by another unit's strong one) may carry another that no
// other definition holds (`kept_name`). What that name names afterwards is the link document's
// `supersededDefinition`, never a format's name: under `keepsItsBytes` (ELF) the definition stands for it; under
// `replacedWhole` (Mach-O — the table's B overridden cell above, 11) it names the WINNER of the lost name; under
// `discardedWhole` (PE select-any; ✔MEASURED 2026-10-15, lld-link 19.1.5 and GNU ld 2.42 refuse the link naming the
// label, link.exe 14.44 links garbage) it is left with no definition, and a unit that imports it is refused by name.
// One link, three documents that differ in that key alone, in-process on every leg.
namespace {

// `mov eax, 42 ; ret`, `mov eax, 11 ; ret` and `mov eax, 13 ; ret`: unique in each image, so a byte search tells which
// bodies the link kept without asking a single name.
std::vector<std::uint8_t> const kLosingBody{0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
std::vector<std::uint8_t> const kWinningBody{0xB8, 0x0B, 0x00, 0x00, 0x00, 0xC3};
std::vector<std::uint8_t> const kOtherWinningBody{0xB8, 0x0D, 0x00, 0x00, 0x00, 0xC3};

// Unit `cu`: one function, `bytes`, under the rows `names` (one SymbolId: the alias representation).
[[nodiscard]] AssembledModule definerOf(std::uint32_t cu, std::vector<std::uint8_t> bytes,
                                        std::vector<std::pair<std::string, SymbolBinding>> const& names, bool entry) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{cu};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = std::move(bytes);
    m.functions.push_back(std::move(fn));
    for (auto const& [name, binding] : names) {
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, name, binding, SymbolVisibility::Default});
    }
    if (entry) m.userEntrySymbol = SymbolId{1};
    return m;
}

// Unit `cu`: the program's entry, which calls the extern `name` once (`call rel32 ; ret`).
[[nodiscard]] AssembledModule importerOf(std::uint32_t cu, std::string name) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{cu};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};   // the x86_64 target's rel32
    rel.addend = 0;
    fn.relocations.push_back(rel);
    m.functions.push_back(std::move(fn));
    ExternImport ext;
    ext.symbol      = SymbolId{2};
    ext.mangledName = std::move(name);
    m.externImports.push_back(std::move(ext));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "entry", SymbolBinding::Global, SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    return m;
}

// The shipped ELF x86_64 image document with `supersededDefinition` set to `answer` — the key, and nothing else.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> imageDocumentAnswering(char const* answer) {
    auto const    path = dss::test::configRoot() / "object-formats" / "elf64-x86_64-linux-exec.format.json";
    std::ifstream in{path};
    auto          doc            = nlohmann::json::parse(in);
    doc["supersededDefinition"] = answer;
    auto const loaded = ObjectFormatSchema::loadFromText(doc.dump(), std::string{"elf64-x86_64-linux-exec, "} + answer);
    EXPECT_TRUE(loaded.has_value()) << answer;
    return loaded.has_value() ? *loaded : nullptr;
}

[[nodiscard]] bool holds(std::vector<std::uint8_t> const& image, std::vector<std::uint8_t> const& body) {
    return std::search(image.begin(), image.end(), body.begin(), body.end()) != image.end();
}

[[nodiscard]] bool reported(DiagnosticReporter const& rep, DiagnosticCode code, std::string_view naming) {
    for (auto const& d : rep.all()) {
        if (d.code == code && d.actual.find(naming) != std::string::npos) return true;
    }
    return false;
}

[[nodiscard]] std::optional<LinkedSymbolKey> definitionOfTheReference(LinkedImage const& img, std::uint32_t cu) {
    for (auto const& r : img.resolvedCrossCuRefs) {
        if (r.reference == LinkedSymbolKey{CompilationUnitId{cu}, SymbolId{2}}) return r.definition;
    }
    return std::nullopt;
}

}  // namespace

TEST(AtomOwnershipShapes, ASupersededDefinitionsOtherNameFollowsItsDocument) {
    auto const t = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(t.has_value());
    LinkedSymbolKey const losing{CompilationUnitId{1}, SymbolId{1}};
    LinkedSymbolKey const winning{CompilationUnitId{2}, SymbolId{1}};
    auto const loser = [] {
        return definerOf(1, kLosingBody, {{"contested", SymbolBinding::Weak}, {"kept_name", SymbolBinding::Global}},
                         false);
    };
    auto const winner = [](bool entry) {
        return definerOf(2, kWinningBody, {{"contested", SymbolBinding::Global}}, entry);
    };

    // keepsItsBytes: the definition stands for the name it alone holds, and the importer reaches its bytes.
    {
        auto const f = imageDocumentAnswering("keepsItsBytes");
        ASSERT_NE(f, nullptr);
        std::vector<AssembledModule> mods{loser(), winner(false), importerOf(3, "kept_name")};
        DiagnosticReporter           rep;
        LinkedImage const            img = linker::link(std::span<AssembledModule const>{mods}, **t, *f, rep);
        ASSERT_TRUE(img.ok()) << diagnosticsOf(rep);
        ASSERT_TRUE(img.resolvedGlobalDefs.contains("kept_name"));
        EXPECT_EQ(img.resolvedGlobalDefs.at("kept_name"), losing);
        EXPECT_EQ(definitionOfTheReference(img, 3), std::optional{losing});
        EXPECT_TRUE(holds(img.bytes, kLosingBody)) << "keepsItsBytes: the bytes stay";
    }
    // replacedWhole: the name names the winner of the lost one, and the definition's bytes are gone.
    {
        auto const f = imageDocumentAnswering("replacedWhole");
        ASSERT_NE(f, nullptr);
        std::vector<AssembledModule> mods{loser(), winner(false), importerOf(3, "kept_name")};
        DiagnosticReporter           rep;
        LinkedImage const            img = linker::link(std::span<AssembledModule const>{mods}, **t, *f, rep);
        ASSERT_TRUE(img.ok()) << diagnosticsOf(rep);
        ASSERT_TRUE(img.resolvedGlobalDefs.contains("kept_name"));
        EXPECT_EQ(img.resolvedGlobalDefs.at("kept_name"), winning)
            << "replacedWhole: every label of the replaced definition names the winner";
        EXPECT_EQ(definitionOfTheReference(img, 3), std::optional{winning});
        EXPECT_FALSE(holds(img.bytes, kLosingBody)) << "replacedWhole: the definition is replaced with its labels";
        EXPECT_TRUE(holds(img.bytes, kWinningBody));
    }
    // replacedWhole, the definition's names lost to TWO definitions: no one winner for the other name — refused.
    {
        auto const f = imageDocumentAnswering("replacedWhole");
        ASSERT_NE(f, nullptr);
        std::vector<AssembledModule> mods{
            definerOf(1, kLosingBody,
                      {{"contested", SymbolBinding::Weak},
                       {"contested_too", SymbolBinding::Weak},
                       {"kept_name", SymbolBinding::Global}},
                      false),
            winner(false), definerOf(4, kOtherWinningBody, {{"contested_too", SymbolBinding::Global}}, false),
            importerOf(3, "kept_name")};
        DiagnosticReporter rep;
        LinkedImage const  img = linker::link(std::span<AssembledModule const>{mods}, **t, *f, rep);
        EXPECT_FALSE(img.ok());
        EXPECT_TRUE(reported(rep, DiagnosticCode::K_CrossCuMergeUnsupported, "'kept_name'")) << diagnosticsOf(rep);
    }
    // discardedWhole: the name goes with the definition — an importer is refused naming it, and a link that does not
    // need it links without it.
    {
        auto const f = imageDocumentAnswering("discardedWhole");
        ASSERT_NE(f, nullptr);
        std::vector<AssembledModule> mods{loser(), winner(false), importerOf(3, "kept_name")};
        DiagnosticReporter           rep;
        LinkedImage const            img = linker::link(std::span<AssembledModule const>{mods}, **t, *f, rep);
        EXPECT_FALSE(img.ok());
        EXPECT_TRUE(reported(rep, DiagnosticCode::K_SymbolUndefined, "'kept_name'")) << diagnosticsOf(rep);

        std::vector<AssembledModule> unneeded{loser(), winner(true)};
        DiagnosticReporter           rep2;
        LinkedImage const            img2 = linker::link(std::span<AssembledModule const>{unneeded}, **t, *f, rep2);
        ASSERT_TRUE(img2.ok()) << diagnosticsOf(rep2);
        EXPECT_FALSE(img2.resolvedGlobalDefs.contains("kept_name")) << "discardedWhole: the name has no definition";
        EXPECT_FALSE(holds(img2.bytes, kLosingBody));
    }
    // A RELOCATABLE link keeps the definition under its other name whatever the document answers: ✔MEASURED
    // 2026-10-15, `ld -r` of a `.weak_definition` with a visible strong label beside an override of the weak name
    // keeps the strong label on its own bytes under ld-1267 and `-ld_classic` (an image of the artifact reads 79).
    {
        auto const    path = dss::test::configRoot() / "object-formats" / "elf64-x86_64-linux.format.json";
        std::ifstream in{path};
        auto          doc            = nlohmann::json::parse(in);
        doc["supersededDefinition"] = "replacedWhole";
        auto const f = ObjectFormatSchema::loadFromText(doc.dump(), "elf64-x86_64-linux, replacedWhole");
        ASSERT_TRUE(f.has_value());
        std::vector<AssembledModule> mods{loser(), winner(false)};
        DiagnosticReporter           rep;
        LinkedImage const            obj = linker::link(std::span<AssembledModule const>{mods}, **t, **f, rep);
        ASSERT_TRUE(obj.ok()) << diagnosticsOf(rep);
        ASSERT_TRUE(obj.resolvedGlobalDefs.contains("kept_name"));
        EXPECT_EQ(obj.resolvedGlobalDefs.at("kept_name"), losing) << "a relocatable link keeps the label on its body";
        EXPECT_TRUE(holds(obj.bytes, kLosingBody));
    }
}
