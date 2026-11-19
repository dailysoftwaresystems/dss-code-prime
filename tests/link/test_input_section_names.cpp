// P69 fold 3e (lane `xa`; the orchestrator's ledger 577, D-LK-MACHO-STATIC-SECTION-RELATIVE-RELOC): AN INPUT SECTION A
// REFERENCE TOOLCHAIN EMITS IS MAPPED TO ITS OUTPUT KIND BY THE FORMAT'S DATA, AND A WRITER NEVER MEETS A READER-ONLY ROW.
//
// One function answers which row an input section reads as (`link::format::inputSectionRow`), from the document's
// `inputSectionNames`: its subsection separators (ELF `.`, COFF `$` and `.`), and the names a reference toolchain emits
// that no writer of the format writes (Apple clang's `__TEXT,__cstring` and `__TEXT,__literal{4,8,16}`, read as
// `__TEXT,__const`). Until the fold the ELF, COFF and Win64-unwind readers each held a separator rule in code and the
// Mach-O reader none, so every Apple clang object holding a string literal was refused, K_ObjectSectionNotModelled.
//
// What is pinned:
//   * every shipped document that declares relocations states the vocabulary, and every Mach-O `readAs` name is no
//     row (a writer resolves its rows by kind and never meets one);
//   * the universe of names the references emit -- formats x {string literal, constant literal, plain constant,
//     relocated constant, code, data} -- each read as its row, and the names of another format's convention not;
//   * the loader refuses a `readAs` entry that is a row, one that names no row, and an empty or repeated separator;
//   * Apple clang's objects, both ISAs, 7 cells each (macho_input_sections.source.c, whose comment is the build record):
//     each read whole -- every relocation bound to an atom of the unit, the literal's bytes in an atom (x86_64's
//     anonymous ones made from bytes no symbol owns, reached by section-index relocations) -- and linked beside a DSS
//     `main`; natively, where the host runs Mach-O images, each runs to 42 as the reference's own program does.

#include "object_entry_support.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/input_section_names.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support::object_entry;

namespace {

[[nodiscard]] std::vector<std::string> shippedDocuments() {
    std::vector<std::string> out;
    for (auto const& e : fs::directory_iterator(dss::test::configRoot() / "object-formats")) {
        std::string const file = e.path().filename().string();
        std::string_view const suffix = ".format.json";
        if (file.ends_with(suffix)) out.push_back(file.substr(0, file.size() - suffix.size()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

[[nodiscard]] nlohmann::json shippedJson(std::string const& document) {
    std::ifstream in{dss::test::configRoot() / "object-formats" / (document + ".format.json")};
    return nlohmann::json::parse(in);
}

// One input section a reference toolchain emits, and the row it reads as (`rowName` empty: none).
struct NameCell {
    char const* segment;
    char const* name;
    char const* rowSegment;
    char const* rowName;
    SectionKind kind;
};

struct DocumentCells {
    char const*           document;
    std::vector<NameCell> cells;
};

[[nodiscard]] std::vector<DocumentCells> nameUniverse() {
    std::vector<NameCell> const elf{
        {"", ".rodata", "", ".rodata", SectionKind::Rodata},
        {"", ".rodata.str1.1", "", ".rodata", SectionKind::Rodata},     // gcc, clang: string literals
        {"", ".rodata.str1.8", "", ".rodata", SectionKind::Rodata},
        {"", ".rodata.cst4", "", ".rodata", SectionKind::Rodata},       // constant literals
        {"", ".rodata.cst8", "", ".rodata", SectionKind::Rodata},
        {"", ".rodata.cst16", "", ".rodata", SectionKind::Rodata},
        {"", ".rodata.check", "", ".rodata", SectionKind::Rodata},      // -fdata-sections
        {"", ".data.rel.ro.local", "", ".data.rel.ro", SectionKind::RelRoConst},   // the longest row
        {"", ".data.rel.ro", "", ".data.rel.ro", SectionKind::RelRoConst},
        {"", ".data.k", "", ".data", SectionKind::Data},
        {"", ".bss.k", "", ".bss", SectionKind::Bss},
        {"", ".text.startup", "", ".text", SectionKind::Text},
        {"", ".text.unlikely", "", ".text", SectionKind::Text},
        {"", ".text.check", "", ".text", SectionKind::Text},            // -ffunction-sections
        {"", ".comment", "", "", SectionKind::Text},                    // no row: the reader's own rule
        {"", ".rodatax", "", "", SectionKind::Text},                    // no separator after the row's name
        {"", ".text$mn", "", "", SectionKind::Text},                    // COFF's group: not ELF's
        {"", "__cstring", "", "", SectionKind::Text},                   // Mach-O's: not ELF's
    };
    std::vector<NameCell> const pe{
        {"", ".text$mn", "", ".text", SectionKind::Text},               // cl, clang-cl
        {"", ".text$x", "", ".text", SectionKind::Text},
        {"", ".rdata$r", "", ".rdata", SectionKind::Rodata},            // the first `.rdata` row: the reader's split
        {"", ".rdata$.refptr.__ImageBase", "", ".rdata", SectionKind::Rodata},   // mingw-w64 gcc
        {"", ".data$r", "", ".data", SectionKind::Data},
        {"", ".bss$x", "", ".bss", SectionKind::Bss},
        {"", ".text.startup", "", ".text", SectionKind::Text},          // mingw-w64 gcc: GNU's part
        {"", ".text.startup.main", "", ".text", SectionKind::Text},
        {"", ".text.unlikely$x", "", ".text", SectionKind::Text},
        {"", ".CRT$XCU", "", "", SectionKind::Text},                    // no row of that name
        {"", ".rdatax", "", "", SectionKind::Text},
        {"", ".drectve", "", "", SectionKind::Text},
        {"", "__cstring", "", "", SectionKind::Text},
    };
    std::vector<NameCell> const macho{
        {"__TEXT", "__cstring", "__TEXT", "__const", SectionKind::Rodata},     // string literals
        {"__TEXT", "__literal4", "__TEXT", "__const", SectionKind::Rodata},    // constant literals
        {"__TEXT", "__literal8", "__TEXT", "__const", SectionKind::Rodata},
        {"__TEXT", "__literal16", "__TEXT", "__const", SectionKind::Rodata},
        {"__TEXT", "__const", "__TEXT", "__const", SectionKind::Rodata},       // a plain constant
        {"__DATA", "__const", "__DATA", "__const", SectionKind::RelRoConst},   // a relocated constant
        {"__DATA", "__data", "__DATA", "__data", SectionKind::Data},
        {"__TEXT", "__text", "__TEXT", "__text", SectionKind::Text},
        {"__DATA", "__cstring", "", "", SectionKind::Text},                    // the pair, never the name alone
        {"__TEXT", "__ustring", "", "", SectionKind::Text},                    // no reference cell emits it
        {"__TEXT", "__const.x", "", "", SectionKind::Text},                    // no separator on Mach-O
        {"", ".rodata.str1.1", "", "", SectionKind::Text},
    };
    std::vector<DocumentCells> out;
    for (char const* d : {"elf64-x86_64-linux", "elf64-x86_64-linux-staticlib", "elf64-aarch64-linux",
                          "elf64-aarch64-linux-staticlib"}) {
        out.push_back({d, elf});
    }
    for (char const* d : {"pe64-x86_64-windows", "pe64-x86_64-windows-staticlib"}) out.push_back({d, pe});
    for (char const* d : {"macho64-arm64-darwin", "macho64-arm64-darwin-staticlib", "macho64-x86_64-darwin",
                          "macho64-x86_64-darwin-staticlib"}) {
        out.push_back({d, macho});
    }
    return out;
}

// The Mach-O reference cells (macho_input_sections.source.c): their fixture name's middle, and the literal text each
// unit's bytes hold (empty: a cell whose constant is no text).
struct MachOCell {
    char const* cell;
    char const* text;
};
constexpr MachOCell kMachOCells[] = {
    {"cstring", "dss-cstring-literal"}, {"cstring_data", "dss-literal-in-data"}, {"literal4", ""}, {"literal8", ""},
    {"literal16", ""}, {"const", ""}, {"relro", "forty-two"},
};

struct MachOIsa {
    char const* arch;
    char const* relocatable;
    Family const* family;   // object_entry_support's family of the ISA's Mach-O images
};

[[nodiscard]] std::vector<MachOIsa> machOIsas() {
    std::vector<MachOIsa> out;
    for (auto const& f : kFamilies) {
        std::string_view const tag{f.tag};
        if (tag == "arm64_macho_clang") out.push_back({"arm64", "macho64-arm64-darwin", &f});
        if (tag == "x86_64_macho_clang") out.push_back({"x86_64", "macho64-x86_64-darwin", &f});
    }
    return out;
}

[[nodiscard]] std::string fixtureOf(MachOIsa const& isa, MachOCell const& c) {
    return std::string{"macho_input_section_"} + c.cell + "_" + isa.arch + "_clang_O2.o";
}

constexpr char const* kMain = "int check(void);\nint main(void) { return check(); }\n";

}  // namespace

// Every leg: every shipped document that declares relocations states how its input sections' names are read, and
// none of its reader-only names is a row -- so `sectionByKind`, which every writer asks, answers the same row it did.
// RED-ON-DISABLE: drop the vocabulary from a document -> that document is named.
TEST(InputSectionNames, EveryShippedDocumentThatDeclaresRelocationsStatesIt) {
    std::size_t stated = 0;
    for (std::string const& document : shippedDocuments()) {
        SCOPED_TRACE(document);
        auto const schema = ObjectFormatSchema::loadShipped(document);
        ASSERT_TRUE(schema.has_value()) << "the shipped document loads";
        if ((*schema)->relocations().empty()) continue;
        InputSectionNames const* const names = (*schema)->inputSectionNames();
        ASSERT_NE(names, nullptr) << "a document whose objects a reader reads states `inputSectionNames`";
        ++stated;
        for (InputSectionAlias const& a : names->readAs) {
            for (auto const& row : (*schema)->sections()) {
                EXPECT_FALSE(row.segment == a.segment && row.name == a.name)
                    << a.name << " is a reader-only name and a row a writer writes";
            }
        }
    }
    EXPECT_EQ(stated, 22u) << "the ten ELF, four PE and eight Mach-O documents";
}

// Every leg: each name a reference toolchain emits reads as its row, in every document of its family a reader is handed,
// and a name of another format's convention reads as none.
// RED-ON-DISABLE: take no subsection separator -> every ELF and PE part reads as none; read no `readAs` entry -> every
// Mach-O literal section reads as none.
TEST(InputSectionNames, EveryInputSectionAReferenceEmitsReadsAsItsRow) {
    for (DocumentCells const& d : nameUniverse()) {
        auto const schema = ObjectFormatSchema::loadShipped(d.document);
        ASSERT_TRUE(schema.has_value()) << d.document;
        for (NameCell const& c : d.cells) {
            SCOPED_TRACE(std::string{d.document} + ": " + c.segment + (*c.segment ? "," : "") + c.name);
            auto const row = link::format::inputSectionRow(**schema, c.segment, c.name);
            if (std::string_view{c.rowName}.empty()) {
                EXPECT_FALSE(row.has_value()) << "read as " << (*schema)->sections()[*row].name;
                continue;
            }
            ASSERT_TRUE(row.has_value()) << "reads as no row";
            auto const& r = (*schema)->sections()[*row];
            EXPECT_EQ(r.segment, c.rowSegment);
            EXPECT_EQ(r.name, c.rowName);
            EXPECT_EQ(r.kind, c.kind);
        }
    }
    // The Win64 unwind families, by the PE document's separators.
    auto const pe = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(pe.has_value());
    EXPECT_TRUE(link::format::isInputSubsectionOf(**pe, ".pdata$x", ".pdata"));
    EXPECT_TRUE(link::format::isInputSubsectionOf(**pe, ".xdata.unlikely", ".xdata"));
    EXPECT_TRUE(link::format::isInputSubsectionOf(**pe, ".xdata", ".xdata"));
    EXPECT_FALSE(link::format::isInputSubsectionOf(**pe, ".xdatax", ".xdata"));
}

// Every leg: the loader judges a `readAs` entry against the rows and each separator by itself.
// RED-ON-DISABLE: let a `readAs` entry name a row -> that document loads.
TEST(InputSectionNames, TheLoaderRefusesAReadAsEntryThatIsARowOrNamesNone) {
    struct Edit {
        char const* label;
        char const* path;   // the JSON pointer the refusal names
        void (*apply)(nlohmann::json&);
    };
    Edit const edits[] = {
        {"a reader-only name that is a row", "/inputSectionNames/readAs/0",
         [](nlohmann::json& d) { d["inputSectionNames"]["readAs"][0]["name"] = "__const"; }},
        {"a name read as no row", "/inputSectionNames/readAs/0",
         [](nlohmann::json& d) { d["inputSectionNames"]["readAs"][0]["row"]["name"] = "__nothing"; }},
        {"one name read as two rows", "/inputSectionNames/readAs/1",
         [](nlohmann::json& d) { d["inputSectionNames"]["readAs"][1]["name"] = "__cstring"; }},
        {"an empty separator", "/inputSectionNames/subsectionSeparators/0",
         [](nlohmann::json& d) { d["inputSectionNames"]["subsectionSeparators"] = nlohmann::json::array({""}); }},
        {"a separator stated twice", "/inputSectionNames/subsectionSeparators/1",
         [](nlohmann::json& d) { d["inputSectionNames"]["subsectionSeparators"] = nlohmann::json::array({".", "."}); }},
        {"a misspelt key", "/inputSectionNames/readsAs",
         [](nlohmann::json& d) { d["inputSectionNames"]["readsAs"] = nlohmann::json::array(); }},
    };
    auto const pristine = shippedJson("macho64-arm64-darwin");
    ASSERT_TRUE(ObjectFormatSchema::loadFromText(pristine.dump(), "macho64-arm64-darwin").has_value())
        << "the control loads";
    for (Edit const& e : edits) {
        SCOPED_TRACE(e.label);
        auto doc = pristine;
        e.apply(doc);
        auto const loaded = ObjectFormatSchema::loadFromText(doc.dump(), std::string{"macho64-arm64-darwin, "} + e.label);
        ASSERT_FALSE(loaded.has_value()) << "the document loads";
        EXPECT_TRUE(std::any_of(loaded.error().begin(), loaded.error().end(),
                                [&](ConfigDiagnostic const& c) { return c.path == e.path; }))
            << "no refusal names " << e.path;
    }
}

// Every leg: each Apple clang object of the universe (both ISAs, 7 cells) is read whole -- every relocation bound to an
// atom of the unit, and a literal's bytes in an atom (x86_64 reaches its literals by section-index relocations, the
// atoms made from bytes no symbol owns) -- and DSS links it beside a DSS `main`, as the reference's own program of it
// links and runs to 42 (the build record).
// RED-ON-DISABLE: read no `readAs` entry -> every cell holding a literal is refused K_ObjectSectionNotModelled;
// make no anonymous atom -> every x86_64 literal cell's relocation names no atom; refuse a section-index relocation ->
// every x86_64 literal cell is refused.
TEST(MachOInputSections, EveryReferenceCellReadsWholeAndLinksBesideADssMain) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "macho-input-sections"};
    std::size_t k = 0;
    for (MachOIsa const& isa : machOIsas()) {
        auto const target = TargetSchema::loadShipped(isa.arch);
        auto const format = ObjectFormatSchema::loadShipped(isa.relocatable);
        ASSERT_TRUE(target.has_value() && format.has_value()) << isa.arch;
        for (MachOCell const& c : kMachOCells) {
            std::string const object = fixtureOf(isa, c);
            SCOPED_TRACE(object);
            std::ifstream in{fixturePath(object), std::ios::binary};
            std::vector<std::uint8_t> const bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            ASSERT_FALSE(bytes.empty()) << "the fixture is missing";
            DiagnosticReporter rep;
            auto const mod = macho::readRelocatableObject(bytes, **target, **format, rep);
            if (!mod.has_value()) {
                ADD_FAILURE() << "the reader refuses the object:" << diagnosticsOf(rep);
                continue;
            }
            std::set<std::uint32_t> atoms;
            for (auto const& f : mod->functions) atoms.insert(f.symbol.v);
            for (auto const& d : mod->dataItems) atoms.insert(d.symbol.v);
            std::size_t relocations = 0;
            auto const judge = [&](auto const& rels) {
                for (auto const& r : rels) {
                    ++relocations;
                    EXPECT_TRUE(atoms.contains(r.target.v)) << "a relocation names no atom of the unit";
                }
            };
            for (auto const& f : mod->functions) judge(f.relocations);
            for (auto const& d : mod->dataItems) judge(d.relocations);
            EXPECT_GT(relocations, 0u) << "the unit's code reaches its constant through a relocation";
            if (*c.text != '\0') {
                std::string_view const text{c.text};
                EXPECT_TRUE(std::any_of(mod->dataItems.begin(), mod->dataItems.end(), [&](auto const& d) {
                    return std::string_view{reinterpret_cast<char const*>(d.bytes.data()), d.bytes.size()}.find(text)
                        != std::string_view::npos;
                })) << "no atom holds the literal '" << text << "'";
            }
            Built built;
            build(scratch.path() / std::to_string(k++), {written("main.c", kMain), fixture(object)},
                  isa.family->execTarget, built);
            EXPECT_EQ(built.rc, 0) << "DSS links the reference's object beside a DSS main:" << built.diagnostics;
            EXPECT_FALSE(built.image.empty()) << "the build writes one image";
        }
    }
}

// Where the host runs Mach-O images (macOS: arm64 natively, x86_64 under the host's own translation): each cell's image
// runs to 42, as the reference's own program of the object does.
TEST(MachOInputSectionsNative, EachImageRunsTo42) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "macho-input-sections-run"};
    NativeFamilies natives;
    std::size_t k = 0;
    for (MachOIsa const& isa : machOIsas()) {
        auto const gate = natives.admit(*isa.family);
        if (!gate) continue;
        for (MachOCell const& c : kMachOCells) {
            SCOPED_TRACE(fixtureOf(isa, c));
            runCell(scratch.path() / std::to_string(k++), *isa.family, *gate,
                    {written("main.c", kMain), fixture(fixtureOf(isa, c))}, 42u);
        }
    }
    if (natives.ranNone()) GTEST_SKIP() << natives.why();
}
