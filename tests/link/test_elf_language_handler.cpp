// P69 fold 3d (lane `xa`): A FOREIGN FUNCTION'S DWARF HANDLER IS CARRIED -- the personality routine its CIE names and
// the language-specific data area its FDE points at.
//
// Until this fold the decoder refused a CIE's `P` and `L` by name, so a C `cleanup` or any C++ function compiled with
// exceptions could not be read at all. Now the decoder reports each as a POINTER FIELD and the ELF reader binds it: the
// routine (through gcc's and clang's `DW.ref` slot when the field is indirect) and {the data item the area's section
// became, the area's offset in it}, carried as a `LanguageHandler` of the DWARF shape.
//
// ★ THE SUBJECTS ARE REFERENCE BYTES. `tests/link/data/elf_cleanup_handler_*.o`, five objects of one C source (its
//   REBUILD lines are in `elf_cleanup_handler.source.c`), each built twice byte-identical, md5 recorded below. Each
//   compiler's spelling of the two fields is different -- that is the point of having five:
//     gcc   x86_64 -fPIC    60aa9135508f86f9527d7e963daddef8  personality 0x9b PC32 -> `DW.ref`, area 0x1b PC32
//     gcc   x86_64 -fno-pic c698894cecf7b451cc9bb25f79efae30  personality 0x03 R_X86_64_32 direct, area 0x03
//     clang x86_64 -fPIC    af46f8390661e47147b962cb22ab971c  as gcc -fPIC
//     clang x86_64 -fno-pic f93c35ed214ba44add54760d0cc3ed61  as gcc -fno-pic
//     gcc   aarch64 -fPIC   78c8ea584d3e08f73af696f43e7fa806  personality 0x9b PREL32 -> `DW.ref`, area 0x1b PREL32
//   gcc's x86_64 objects carry TWO areas, `run`'s and its cold part's (`run.cold`, in `.text.unlikely`), at offsets 0
//   and 8 of `.gcc_except_table` (✔MEASURED, readelf -r: the `.rela.eh_frame` rows at 0x31 and 0x51).
//
// The section the area lives in is named here ONLY to read the reference bytes back; the reader finds it by the role
// the unwind table gives it.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

struct Fixture {
    char const* file;
    char const* target;
    char const* format;
    std::size_t areas;   // how many of its functions carry a data area
};

constexpr Fixture kFixtures[] = {
    {"elf_cleanup_handler_gcc_x86_64_pic.o", "x86_64", "elf64-x86_64-linux", 2},
    {"elf_cleanup_handler_gcc_x86_64_nopic.o", "x86_64", "elf64-x86_64-linux", 2},
    {"elf_cleanup_handler_clang_x86_64_pic.o", "x86_64", "elf64-x86_64-linux", 1},
    {"elf_cleanup_handler_clang_x86_64_nopic.o", "x86_64", "elf64-x86_64-linux", 1},
    {"elf_cleanup_handler_gcc_aarch64_pic.o", "arm64", "elf64-aarch64-linux", 1},
};

[[nodiscard]] std::vector<std::uint8_t> fixture(std::string_view name) {
    auto const    path = dss::test::repoRoot() / "tests" / "link" / "data" / std::string{name};
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        ADD_FAILURE() << "cannot open " << path.string();
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
    std::shared_ptr<ObjectFormatSchema const> format;
};

[[nodiscard]] std::optional<Loaded> load(Fixture const& f) {
    auto t = TargetSchema::loadShipped(f.target);
    auto o = ObjectFormatSchema::loadShipped(f.format);
    if (!t.has_value() || !o.has_value()) {
        ADD_FAILURE() << f.file << ": the shipped target '" << f.target << "' or format '" << f.format
                      << "' did not load";
        return std::nullopt;
    }
    return Loaded{std::move(t).value(), std::move(o).value()};
}

// ── The reference bytes, read back by NAME (the test's own reader, never DSS's) ──
[[nodiscard]] std::uint64_t rd(std::span<std::uint8_t const> b, std::size_t at, unsigned width) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < width; ++i) v |= static_cast<std::uint64_t>(b[at + i]) << (8u * i);
    return v;
}

struct Section {
    std::size_t offset = 0, size = 0;
};

[[nodiscard]] std::optional<Section> sectionNamed(std::span<std::uint8_t const> obj, std::string_view want) {
    std::uint64_t const shoff  = rd(obj, 0x28, 8);
    auto const          shnum  = static_cast<std::size_t>(rd(obj, 0x3C, 2));
    auto const          shstr  = static_cast<std::size_t>(rd(obj, 0x3E, 2));
    std::uint64_t const strOff = rd(obj, static_cast<std::size_t>(shoff) + shstr * 64u + 0x18, 8);
    for (std::size_t i = 0; i < shnum; ++i) {
        std::size_t const sh = static_cast<std::size_t>(shoff) + i * 64u;
        std::string       name;
        for (std::size_t k = static_cast<std::size_t>(strOff + rd(obj, sh, 4)); k < obj.size() && obj[k] != 0; ++k) {
            name.push_back(static_cast<char>(obj[k]));
        }
        if (name == want) {
            return Section{static_cast<std::size_t>(rd(obj, sh + 0x18, 8)), static_cast<std::size_t>(rd(obj, sh + 0x20, 8))};
        }
    }
    return std::nullopt;
}

// The `.rela.eh_frame` entry whose r_offset is `fieldOffset`: its byte position in the object, or nullopt.
[[nodiscard]] std::optional<std::size_t> unwindRelocationAt(std::span<std::uint8_t const> obj, std::uint64_t fieldOffset) {
    auto const rela = sectionNamed(obj, ".rela.eh_frame");
    if (!rela.has_value()) return std::nullopt;
    for (std::size_t e = rela->offset; e + 24 <= rela->offset + rela->size; e += 24) {
        if (rd(obj, e, 8) == fieldOffset) return e;
    }
    return std::nullopt;
}

[[nodiscard]] std::string nameOf(AssembledModule const& m, SymbolId id) {
    for (auto const& row : m.symbols) {
        if (row.symbol == id && !row.name.empty()) return row.name;
    }
    for (auto const& ext : m.externImports) {
        if (ext.symbol == id) return ext.mangledName;
    }
    return {};
}

}  // namespace

// Every reference object is READ, and each function its unwind table gives a handler carries one of the DWARF shape:
// the routine `__gcc_personality_v0` (an import the table states is called), both dispatch phases, no inline data, and
// a data area that is the object's own area bytes -- `run`'s at offset 0, gcc's cold part's at 8.
TEST(ElfLanguageHandler, EachReferenceObjectsHandlerIsItsRoutineAndItsArea) {
    for (auto const& f : kFixtures) {
        SCOPED_TRACE(f.file);
        auto const loaded = load(f);
        if (!loaded.has_value()) continue;
        auto const bytes = fixture(f.file);
        if (bytes.empty()) continue;
        DiagnosticReporter rep;
        auto const         mod = elf::readRelocatableObject(bytes, *loaded->target, *loaded->format, rep);
        ASSERT_TRUE(mod.has_value()) << "the reader refused the object:" << diagnosticsOf(rep);

        auto const area = sectionNamed(bytes, ".gcc_except_table");
        ASSERT_TRUE(area.has_value());
        std::vector<std::uint8_t> const areaBytes(bytes.begin() + static_cast<std::ptrdiff_t>(area->offset),
                                                  bytes.begin() + static_cast<std::ptrdiff_t>(area->offset + area->size));
        std::optional<SymbolId> areaItem;
        for (auto const& di : mod->dataItems) {
            if (di.bytes == areaBytes) {
                EXPECT_FALSE(areaItem.has_value()) << "two items hold the area's bytes";
                areaItem = di.symbol;
            }
        }
        ASSERT_TRUE(areaItem.has_value()) << "no data item holds the object's data-area bytes";

        std::size_t              carried = 0;
        std::set<std::int64_t>   offsets;
        bool                     runHasAreaAtZero = false;
        for (auto const& fn : mod->functions) {
            if (!fn.languageHandler.has_value()) continue;
            ++carried;
            LanguageHandler const& h = *fn.languageHandler;
            EXPECT_EQ(nameOf(*mod, h.personality), "__gcc_personality_v0");
            for (auto const& ext : mod->externImports) {
                if (ext.symbol != h.personality) continue;
                EXPECT_FALSE(ext.isData) << "the routine is CALLED, the table states it";
                EXPECT_EQ(ext.kindOrigin, ExternKindOrigin::Stated);
            }
            EXPECT_TRUE(h.exceptionPhase);
            EXPECT_TRUE(h.unwindPhase);
            EXPECT_TRUE(h.data.empty()) << "the DWARF shape carries no inline data";
            EXPECT_TRUE(h.relocations.empty());
            ASSERT_TRUE(h.dataArea.has_value()) << "every handler of these objects has an area";
            EXPECT_EQ(h.dataArea->item, *areaItem);
            offsets.insert(h.dataArea->offset);
            if (nameOf(*mod, fn.symbol) == "run" && h.dataArea->offset == 0) runHasAreaAtZero = true;
            EXPECT_TRUE(fn.cfi.has_value()) << "a handler is reached through the frame description";
        }
        EXPECT_EQ(carried, f.areas) << "functions carrying a handler";
        EXPECT_TRUE(runHasAreaAtZero) << "`run`'s area is the section's first byte";
        if (f.areas == 2) EXPECT_EQ(offsets, (std::set<std::int64_t>{0, 8}));
        EXPECT_TRUE(rep.all().empty()) << diagnosticsOf(rep);
    }
}

// A handler field's relocation must state exactly the field: a linear row of the field's width and application (a
// row's addend bias is in the RELA addend, so gcc -fPIC's R_X86_64_PC32, the `riprel32` row, states a pc-relative
// field, as the first test reads). gcc -fno-pic's personality field (0x13, R_X86_64_32) made R_X86_64_64 is 8 bytes over a 4-byte field; gcc
// -fPIC's area field (0x31, R_X86_64_PC32) made R_X86_64_32 is absolute over a pc-relative one. Each is refused naming
// the field, never bound at an address the unwinder would not read.
TEST(ElfLanguageHandler, AHandlerFieldsRelocationOfAnotherWidthOrApplicationIsRefusedByName) {
    struct Case {
        char const*   file;
        std::uint64_t field;
        std::uint32_t type;
        char const*   names;
    };
    Case const cases[] = {
        {"elf_cleanup_handler_gcc_x86_64_nopic.o", 0x13, 1 /* R_X86_64_64 */, "patches the personality field"},
        {"elf_cleanup_handler_gcc_x86_64_pic.o", 0x31, 10 /* R_X86_64_32 */,
         "patches the language-specific data area field"},
    };
    for (auto const& c : cases) {
        SCOPED_TRACE(c.file);
        Fixture const fx{c.file, "x86_64", "elf64-x86_64-linux", 0};
        auto const    loaded = load(fx);
        if (!loaded.has_value()) continue;
        auto bytes = fixture(c.file);
        if (bytes.empty()) continue;
        auto const at = unwindRelocationAt(bytes, c.field);
        ASSERT_TRUE(at.has_value()) << "the fixture's field relocation is where readelf put it";
        for (unsigned i = 0; i < 4; ++i) bytes[*at + 8 + i] = static_cast<std::uint8_t>((c.type >> (8u * i)) & 0xFFu);
        DiagnosticReporter rep;
        auto const         mod = elf::readRelocatableObject(bytes, *loaded->target, *loaded->format, rep);
        EXPECT_FALSE(mod.has_value()) << "a field relocation of the wrong shape was bound";
        EXPECT_NE(diagnosticsOf(rep).find(c.names), std::string::npos) << diagnosticsOf(rep);
    }
}

// Every relocation of the unwind section is one a record's field claims. gcc -fno-pic's second area relocation moved
// one byte back (0x51 -> 0x50) patches no field -- and leaves the cold part's area field unrelocated and zero, which
// names NO area -- so the object is refused naming the stray relocation rather than read without it.
TEST(ElfLanguageHandler, AnUnwindRelocationNoFieldClaimsIsRefusedByName) {
    Fixture const fx{"elf_cleanup_handler_gcc_x86_64_nopic.o", "x86_64", "elf64-x86_64-linux", 0};
    auto const    loaded = load(fx);
    ASSERT_TRUE(loaded.has_value());
    auto bytes = fixture(fx.file);
    ASSERT_FALSE(bytes.empty());
    auto const at = unwindRelocationAt(bytes, 0x51);
    ASSERT_TRUE(at.has_value()) << "the cold part's area relocation is where readelf put it";
    bytes[*at] = 0x50;
    DiagnosticReporter rep;
    auto const         mod = elf::readRelocatableObject(bytes, *loaded->target, *loaded->format, rep);
    EXPECT_FALSE(mod.has_value()) << "a relocation no field claims was dropped";
    EXPECT_NE(diagnosticsOf(rep).find("which is no field a CIE or an FDE of it declares"), std::string::npos)
        << diagnosticsOf(rep);
}

// The decoder reads a handler field only in an encoding whose width it knows -- the absolute and pc-relative 4- and
// 8-byte ones, indirect or not -- and refuses any other BY THE ENCODING, rather than read the field at a guessed width
// and name the wrong routine or area. gcc -fno-pic's CIE ("zPLR": the personality's encoding at byte 0x12 of
// `.eh_frame`, the areas' at 0x17, both 0x03 udata4) with either byte made 0x01 (uleb128) is refused naming it.
TEST(ElfLanguageHandler, AHandlerFieldInAnEncodingTheDecoderDoesNotReadIsRefusedByName) {
    struct Case {
        std::size_t at;   // in `.eh_frame`
        char const* names;
    };
    Case const cases[] = {
        {0x12, "names its personality routine in pointer encoding 0x01"},
        {0x17, "states its FDEs' language-specific data areas in pointer encoding 0x01"},
    };
    Fixture const fx{"elf_cleanup_handler_gcc_x86_64_nopic.o", "x86_64", "elf64-x86_64-linux", 0};
    auto const    loaded = load(fx);
    ASSERT_TRUE(loaded.has_value());
    for (auto const& c : cases) {
        SCOPED_TRACE(c.names);
        auto bytes = fixture(fx.file);
        ASSERT_FALSE(bytes.empty());
        auto const eh = sectionNamed(bytes, ".eh_frame");
        ASSERT_TRUE(eh.has_value());
        ASSERT_EQ(std::string(bytes.begin() + static_cast<std::ptrdiff_t>(eh->offset + 9),
                              bytes.begin() + static_cast<std::ptrdiff_t>(eh->offset + 13)),
                  "zPLR")
            << "the fixture's CIE is where readelf put it";
        ASSERT_EQ(static_cast<unsigned>(bytes[eh->offset + c.at]), 0x03u)
            << "the field's encoding byte is udata4, as readelf reads it";
        bytes[eh->offset + c.at] = 0x01u;
        DiagnosticReporter rep;
        auto const         mod = elf::readRelocatableObject(bytes, *loaded->target, *loaded->format, rep);
        EXPECT_FALSE(mod.has_value()) << "a handler field of an unread encoding was read";
        EXPECT_NE(diagnosticsOf(rep).find(c.names), std::string::npos) << diagnosticsOf(rep);
    }
}

// An FDE's augmentation data is exactly what its CIE accounts for -- one area field, or none -- since bytes nothing
// reads could be a handler's data. gcc -fno-pic's first FDE (at 0x20 of `.eh_frame`; its augmentation length, 4, at
// 0x30) made to claim 5 bytes is refused naming the two lengths.
TEST(ElfLanguageHandler, AnFdeWhoseAugmentationDataIsNotOneAreaFieldIsRefusedByName) {
    Fixture const fx{"elf_cleanup_handler_gcc_x86_64_nopic.o", "x86_64", "elf64-x86_64-linux", 0};
    auto const    loaded = load(fx);
    ASSERT_TRUE(loaded.has_value());
    auto bytes = fixture(fx.file);
    ASSERT_FALSE(bytes.empty());
    auto const eh = sectionNamed(bytes, ".eh_frame");
    ASSERT_TRUE(eh.has_value());
    ASSERT_EQ(static_cast<unsigned>(bytes[eh->offset + 0x30]), 0x04u)
        << "the first FDE's augmentation length is where readelf put it";
    bytes[eh->offset + 0x30] = 0x05u;
    DiagnosticReporter rep;
    auto const         mod = elf::readRelocatableObject(bytes, *loaded->target, *loaded->format, rep);
    EXPECT_FALSE(mod.has_value()) << "an FDE's unaccounted augmentation byte was read past";
    EXPECT_NE(diagnosticsOf(rep).find("carries 5 bytes of augmentation data where its CIE accounts for 4"),
              std::string::npos)
        << diagnosticsOf(rep);
}
