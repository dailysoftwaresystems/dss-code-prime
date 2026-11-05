// P69 fold 3d (lane `xa`): AN IMAGE'S `.eh_frame` STATES EACH FUNCTION'S HANDLER -- its CIE names the personality
// routine, its FDE points at the language-specific data area.
//
// The ELF reader carries a foreign function's DWARF handler (`test_elf_language_handler.cpp`); until this fold every
// writer refused it, because `buildEhFrame` wrote one "zR" CIE and FDEs with no augmentation data. Now it writes one CIE
// per handler shape ("zR", "zPR", "zPLR") and records each personality and data-area field, and the dynamic ELF and
// Mach-O writers fill them once every address is known: direct pc-relative, the routine's own address (a function of
// the image, or an import's call stub) and the area's first byte.
//
// ★ THE IMAGE IS READ BACK THROUGH THE DECODER THE READER USES, NEVER THROUGH THE WRITER'S OWN RECORDS: each field's
//   target is computed from the bytes (the field's address plus the value stored in it) and compared with what the
//   image holds there -- the routine's FDE, the stub section, the area's bytes.
//
// The flavors that write no handler -- a relocatable object (the reference linkers accept only the indirect pointer
// there, through a slot the writer does not write yet) and an image that imports nothing (no `.eh_frame` at all) --
// refuse one by name, and so do the field fills whose target is no address the dispatcher could use.

#include "asm/asm.hpp"
#include "core/types/cfi.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/dwarf_cfi.hpp"
#include "link/format/dwarf_cfi_decode.hpp"
#include "link/format/elf.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/macho.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include "macho_test_support.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

[[nodiscard]] std::uint64_t rd(std::span<std::uint8_t const> b, std::size_t at, unsigned width) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < width; ++i) v |= static_cast<std::uint64_t>(b[at + i]) << (8u * i);
    return v;
}

[[nodiscard]] std::shared_ptr<TargetSchema const> x86() {
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "the shipped x86_64 target did not load";
        return {};
    }
    return std::move(t).value();
}

[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> shipped(std::string_view name) {
    auto f = ObjectFormatSchema::loadShipped(name);
    if (!f.has_value()) {
        ADD_FAILURE() << "the shipped format '" << name << "' did not load";
        return {};
    }
    return std::move(f).value();
}

// ── The module under test ─────────────────────────────────────────────────────────────────────────────────────────
//
//   #0 thrower      handler: routine #1 (a function of the image), area at offset 2 of the table
//   #1 personality  no handler -- it IS the routine
//   #2 thrower_too  handler: routine #1, area at offset 0 -- the same shape as #0, so the same CIE
//   #3 cleans_up    handler: the imported routine, no area -- another CIE
// and the table, a read-only data item with no name, as the reader makes `.gcc_except_table`.
constexpr std::uint32_t kThrower = 1, kPersonality = 2, kThrowerToo = 3, kCleansUp = 4;
constexpr std::uint32_t kTable = 10, kImported = 20;
std::vector<std::uint8_t> const kTableBytes = {0xff, 0x9b, 0x0d, 0x01, 0x08, 0x10, 0x00, 0x00, 0x2a, 0x00};

struct Spec {
    bool          withImport   = true;   // #3 and the import
    bool          importIsData = false;  // the link binds the routine as a DATA import
    std::int64_t  areaOffset   = 2;      // #0's area
    char const*   importName   = "__gcc_personality_v0";
    char const*   importLib    = "libgcc_s.so.1";
};

[[nodiscard]] AssembledModule makeModule(TargetSchema const& target, Spec const& s) {
    std::uint16_t const rsp = *target.registerByName("rsp");
    auto const          withCfi = [&](std::uint32_t sym, std::size_t len) {
        AssembledFunction fn;
        fn.symbol = SymbolId{sym};
        fn.bytes.assign(len, 0x90);
        fn.bytes.back() = 0xC3;
        CfiFunction cfi;
        cfi.codeLength = static_cast<std::uint32_t>(len);
        cfi.initial    = CfiInitialState{rsp, 8, -8, std::nullopt};
        cfi.ops        = {CfiOp{1, CfiOpKind::DefCfaOffset, {}, {}, 16, 0u}};
        fn.cfi         = std::move(cfi);
        return fn;
    };
    auto const handler = [](std::uint32_t routine, std::optional<std::int64_t> area) {
        LanguageHandler h;
        h.personality    = SymbolId{routine};
        h.exceptionPhase = true;
        h.unwindPhase    = true;
        if (area.has_value()) h.dataArea = DataAreaRef{SymbolId{kTable}, *area};
        return h;
    };
    AssembledModule m;
    m.functions.push_back(withCfi(kThrower, 0x20));
    m.functions.back().languageHandler = handler(kPersonality, s.areaOffset);
    m.functions.push_back(withCfi(kPersonality, 0x10));
    m.functions.push_back(withCfi(kThrowerToo, 0x18));
    m.functions.back().languageHandler = handler(kPersonality, 0);
    if (s.withImport) {
        m.functions.push_back(withCfi(kCleansUp, 0x14));
        m.functions.back().languageHandler = handler(kImported, std::nullopt);
        ExternImport routine;
        routine.symbol      = SymbolId{kImported};
        routine.mangledName = s.importName;
        routine.libraryPath = s.importLib;
        routine.isData      = s.importIsData;
        m.externImports.push_back(routine);
    }
    m.expectedFuncCount = static_cast<decltype(m.expectedFuncCount)>(m.functions.size());
    char const* const names[] = {"thrower", "personality", "thrower_too", "cleans_up"};
    for (std::size_t i = 0; i < m.functions.size(); ++i) {
        m.symbols.push_back(ModuleSymbol{m.functions[i].symbol, names[i], SymbolBinding::Global, SymbolVisibility::Default});
    }
    AssembledData table;
    table.symbol  = SymbolId{kTable};
    table.section = DataSectionKind::Rodata;
    table.bytes   = kTableBytes;
    m.dataItems.push_back(std::move(table));
    return m;
}

// ── An image, as the checks below read it ─────────────────────────────────────────────────────────────────────────
struct Image {
    std::vector<std::uint8_t>                                     bytes;
    std::uint64_t                                                 ehVa = 0, ehOff = 0, ehSize = 0;
    std::uint64_t                                                 stubsVa = 0, stubsSize = 0;
    std::function<std::optional<std::uint64_t>(std::uint64_t va)> fileOffsetOf;
};

// ELF: every section by its header; `.plt` holds the import stubs.
[[nodiscard]] std::optional<Image> readElf(std::vector<std::uint8_t> bytes) {
    struct Sec {
        std::string   name;
        std::uint32_t type;
        std::uint64_t addr, off, size;
    };
    auto const          b      = std::span<std::uint8_t const>{bytes};
    std::uint64_t const shoff  = rd(b, 0x28, 8);
    auto const          shnum  = static_cast<std::size_t>(rd(b, 0x3C, 2));
    auto const          shstr  = static_cast<std::size_t>(rd(b, 0x3E, 2));
    std::uint64_t const strOff = rd(b, static_cast<std::size_t>(shoff) + shstr * 64u + 0x18, 8);
    std::vector<Sec>    secs;
    for (std::size_t i = 0; i < shnum; ++i) {
        std::size_t const sh = static_cast<std::size_t>(shoff) + i * 64u;
        Sec               s;
        for (std::size_t k = static_cast<std::size_t>(strOff + rd(b, sh, 4)); k < bytes.size() && bytes[k] != 0; ++k) {
            s.name.push_back(static_cast<char>(bytes[k]));
        }
        s.type = static_cast<std::uint32_t>(rd(b, sh + 4, 4));
        s.addr = rd(b, sh + 0x10, 8);
        s.off  = rd(b, sh + 0x18, 8);
        s.size = rd(b, sh + 0x20, 8);
        secs.push_back(std::move(s));
    }
    Image img;
    bool  eh = false, plt = false;
    for (auto const& s : secs) {
        if (s.name == ".eh_frame") {
            img.ehVa   = s.addr;
            img.ehOff  = s.off;
            img.ehSize = s.size;
            eh         = true;
        }
        if (s.name == ".plt") {
            img.stubsVa   = s.addr;
            img.stubsSize = s.size;
            plt           = true;
        }
    }
    if (!eh || !plt) {
        ADD_FAILURE() << "the image has no " << (eh ? ".plt" : ".eh_frame");
        return std::nullopt;
    }
    img.fileOffsetOf = [secs](std::uint64_t va) -> std::optional<std::uint64_t> {
        for (auto const& s : secs) {
            if (s.type == 8u /* SHT_NOBITS */ || s.addr == 0u) continue;
            if (va >= s.addr && va < s.addr + s.size) return s.off + (va - s.addr);
        }
        return std::nullopt;
    };
    img.bytes = std::move(bytes);
    return img;
}

// Mach-O: `__TEXT` maps its file bytes from its own file offset; `__stubs` holds the import stubs.
[[nodiscard]] std::optional<Image> readMachO(std::vector<std::uint8_t> bytes) {
    auto const b    = std::span<std::uint8_t const>{bytes};
    auto const seg  = macho::test::findSegment(b, "__TEXT");
    auto const eh   = macho::test::findSection(b, "__TEXT", "__eh_frame");
    auto const stub = macho::test::findSection(b, "__TEXT", "__stubs");
    if (!seg || !eh || !stub) {
        ADD_FAILURE() << "the image lacks __TEXT, its __eh_frame or its __stubs";
        return std::nullopt;
    }
    Image img;
    img.ehVa      = rd(b, *eh + 32, 8);
    img.ehSize    = rd(b, *eh + 40, 8);
    img.ehOff     = rd(b, *eh + 48, 4);
    img.stubsVa   = rd(b, *stub + 32, 8);
    img.stubsSize = rd(b, *stub + 40, 8);
    std::uint64_t const vm = rd(b, *seg + 24, 8), vmSize = rd(b, *seg + 32, 8), fileOff = rd(b, *seg + 40, 8);
    img.fileOffsetOf = [vm, vmSize, fileOff](std::uint64_t va) -> std::optional<std::uint64_t> {
        if (va < vm || va >= vm + vmSize) return std::nullopt;
        return fileOff + (va - vm);
    };
    img.bytes = std::move(bytes);
    return img;
}

// The checks, one image at a time.
void expectHandlersStated(Image const& img, TargetSchema const& target) {
    auto const b = std::span<std::uint8_t const>{img.bytes};
    ASSERT_LE(img.ehOff + img.ehSize, img.bytes.size());
    DiagnosticReporter rep;
    auto const         decoded = link::format::decodeEhFrame(
        b.subspan(static_cast<std::size_t>(img.ehOff), static_cast<std::size_t>(img.ehSize)),
        link::format::dwarfRegisterMappingOf(target), "the test's read", rep);
    ASSERT_TRUE(decoded.has_value()) << "the image's .eh_frame does not decode:" << diagnosticsOf(rep);
    ASSERT_EQ(decoded->fdes.size(), 4u) << "one FDE per function";
    auto const& fdes  = decoded->fdes;
    auto const  start = [&](std::size_t k) {
        return img.ehVa + fdes[k].initialLocationFieldOffset + static_cast<std::uint64_t>(fdes[k].storedInitialLocation);
    };
    auto const target_ = [&](link::format::DecodedPointerField const& f) {
        return img.ehVa + f.fieldOffset + static_cast<std::uint64_t>(f.stored);
    };
    auto const areaBytesAt = [&](std::uint64_t va, std::size_t n) -> std::vector<std::uint8_t> {
        auto const off = img.fileOffsetOf(va);
        if (!off.has_value() || *off + n > img.bytes.size()) return {};
        return {img.bytes.begin() + static_cast<std::ptrdiff_t>(*off),
                img.bytes.begin() + static_cast<std::ptrdiff_t>(*off + n)};
    };

    // #0: the routine is #1, the area is the table's bytes from offset 2.
    ASSERT_TRUE(fdes[0].personality.has_value()) << "#0's CIE names no routine";
    EXPECT_EQ(fdes[0].personality->encoding, 0x1bu) << "an image's personality field is direct pc-relative sdata4";
    EXPECT_EQ(target_(*fdes[0].personality), start(1)) << "#0's routine is not function #1's first byte";
    ASSERT_TRUE(fdes[0].dataArea.has_value()) << "#0's FDE points at no area";
    EXPECT_EQ(fdes[0].dataArea->encoding, 0x1bu);
    EXPECT_EQ(areaBytesAt(target_(*fdes[0].dataArea), kTableBytes.size() - 2),
              std::vector<std::uint8_t>(kTableBytes.begin() + 2, kTableBytes.end()))
        << "#0's area is not the table's bytes from offset 2";

    // #1: no handler.
    EXPECT_FALSE(fdes[1].personality.has_value());
    EXPECT_FALSE(fdes[1].dataArea.has_value());

    // #2: the same shape as #0 -- the same CIE, so the same personality field -- and its own area.
    ASSERT_TRUE(fdes[2].personality.has_value());
    EXPECT_EQ(fdes[2].personality->fieldOffset, fdes[0].personality->fieldOffset)
        << "two FDEs naming one routine with an area each do not share a CIE";
    ASSERT_TRUE(fdes[2].dataArea.has_value());
    EXPECT_EQ(areaBytesAt(target_(*fdes[2].dataArea), kTableBytes.size()), kTableBytes);

    // #3: the imported routine -- its call stub -- and no area, in a CIE of its own.
    ASSERT_TRUE(fdes[3].personality.has_value());
    EXPECT_NE(fdes[3].personality->fieldOffset, fdes[0].personality->fieldOffset);
    std::uint64_t const stubTarget = target_(*fdes[3].personality);
    EXPECT_TRUE(stubTarget >= img.stubsVa && stubTarget < img.stubsVa + img.stubsSize)
        << "the imported routine's field names 0x" << std::hex << stubTarget << ", outside the stubs [0x"
        << img.stubsVa << ", 0x" << img.stubsVa + img.stubsSize << ")";
    EXPECT_FALSE(fdes[3].dataArea.has_value());
    EXPECT_TRUE(rep.all().empty()) << diagnosticsOf(rep);
}

constexpr char const* kArtifact = "fixture.dylib";

}  // namespace

// The dynamic ELF image states every handler: each routine and each area, read back through the decoder.
TEST(EhFrameHandlerWriters, TheDynamicElfImageStatesEachHandler) {
    auto const t = x86();
    auto const f = shipped("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(t && f);
    DiagnosticReporter rep;
    auto bytes = elf::encode(makeModule(*t, Spec{}), *t, *f, rep);
    ASSERT_FALSE(bytes.empty()) << diagnosticsOf(rep);
    auto const img = readElf(std::move(bytes));
    ASSERT_TRUE(img.has_value());
    expectHandlersStated(*img, *t);
}

// The dynamic Mach-O image states every handler the same way, in `__eh_frame`.
TEST(EhFrameHandlerWriters, TheDynamicMachOImageStatesEachHandler) {
    auto const t = x86();
    auto const f = shipped("macho64-x86_64-darwin-dylib");
    ASSERT_TRUE(t && f);
    Spec s;
    s.importName = "___gcc_personality_v0";
    s.importLib  = "/usr/lib/libSystem.B.dylib";
    DiagnosticReporter rep;
    auto bytes = macho::encode(makeModule(*t, s), *t, *f, rep, ImageRequest{.artifactFileName = kArtifact});
    ASSERT_FALSE(bytes.empty()) << diagnosticsOf(rep);
    auto const img = readMachO(std::move(bytes));
    ASSERT_TRUE(img.has_value());
    expectHandlersStated(*img, *t);
}

// A REFERENCE object's handler reaches the image: gcc -fPIC's `run` and its cold part, read by the ELF reader (the
// routine through gcc's `DW.ref` slot, the areas in `.gcc_except_table`), are LINKED beside a unit that defines the
// `sink` and `cleaned` they name, into a shared object whose `.eh_frame` names the routine's call stub and points each
// FDE at the object's own area bytes -- `run`'s at offset 0 of the table, the cold part's at 8. The link puts the
// object second, so its ids move into the merged arena and each area must move with them. The subject is
// `tests/link/data/elf_cleanup_handler_gcc_x86_64_pic.o` (60aa9135508f86f9527d7e963daddef8; its source and REBUILD
// lines in `elf_cleanup_handler.source.c`).
TEST(EhFrameHandlerWriters, AReferenceObjectsHandlerReachesTheImage) {
    auto const t   = x86();
    auto const rel = shipped("elf64-x86_64-linux");
    auto const dyn = shipped("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(t && rel && dyn);
    std::vector<std::uint8_t> obj;
    {
        auto const    path = dss::test::repoRoot() / "tests" / "link" / "data" / "elf_cleanup_handler_gcc_x86_64_pic.o";
        std::ifstream in{path, std::ios::binary};
        ASSERT_TRUE(in.good()) << "cannot open " << path.string();
        obj.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    }
    // The object's `.gcc_except_table`, read by name with the test's own reader.
    std::vector<std::uint8_t> table;
    {
        auto const          b      = std::span<std::uint8_t const>{obj};
        std::uint64_t const shoff  = rd(b, 0x28, 8);
        auto const          shnum  = static_cast<std::size_t>(rd(b, 0x3C, 2));
        auto const          shstr  = static_cast<std::size_t>(rd(b, 0x3E, 2));
        std::uint64_t const strOff = rd(b, static_cast<std::size_t>(shoff) + shstr * 64u + 0x18, 8);
        for (std::size_t i = 0; i < shnum; ++i) {
            std::size_t const sh = static_cast<std::size_t>(shoff) + i * 64u;
            std::string       name;
            for (std::size_t k = static_cast<std::size_t>(strOff + rd(b, sh, 4)); k < obj.size() && obj[k] != 0; ++k) {
                name.push_back(static_cast<char>(obj[k]));
            }
            if (name != ".gcc_except_table") continue;
            auto const off = static_cast<std::size_t>(rd(b, sh + 0x18, 8)), size = static_cast<std::size_t>(rd(b, sh + 0x20, 8));
            table.assign(obj.begin() + static_cast<std::ptrdiff_t>(off), obj.begin() + static_cast<std::ptrdiff_t>(off + size));
        }
    }
    ASSERT_GE(table.size(), 9u) << "the reference object's area table is where readelf put it";

    DiagnosticReporter rep;
    auto               mod = elf::readRelocatableObject(obj, *t, *rel, rep);
    ASSERT_TRUE(mod.has_value()) << diagnosticsOf(rep);
    mod->cuId = CompilationUnitId{2};
    // A shared object binds each import to a library; the reader states none (the link's library search does).
    for (auto& ex : mod->externImports) {
        if (ex.mangledName == "__gcc_personality_v0" || ex.mangledName == "_Unwind_Resume") ex.libraryPath = "libgcc_s.so.1";
    }
    // The unit that defines what `run` names: `sink` (a function) and `cleaned` (an int).
    AssembledModule defines;
    defines.cuId = CompilationUnitId{1};
    {
        AssembledFunction sink;
        sink.symbol = SymbolId{1};
        sink.bytes  = {0xC3};
        defines.functions.push_back(std::move(sink));
        AssembledData cleaned;
        cleaned.symbol  = SymbolId{2};
        cleaned.section = DataSectionKind::Data;
        cleaned.bytes.assign(4, 0);
        defines.dataItems.push_back(std::move(cleaned));
        defines.symbols.push_back(ModuleSymbol{SymbolId{1}, "sink", SymbolBinding::Global, SymbolVisibility::Default});
        defines.symbols.push_back(ModuleSymbol{SymbolId{2}, "cleaned", SymbolBinding::Global, SymbolVisibility::Default});
        defines.expectedFuncCount = 1;
    }
    std::vector<AssembledModule> units;
    units.push_back(std::move(defines));
    units.push_back(std::move(*mod));
    auto linked = linker::link(std::span<AssembledModule const>{units}, *t, *dyn, rep,
                               ImageRequest{.artifactFileName = "libhandler.so"});
    ASSERT_TRUE(linked.ok() && !linked.bytes.empty())
        << "the reference object's handler did not reach the image:" << diagnosticsOf(rep);
    auto const img = readElf(std::move(linked.bytes));
    ASSERT_TRUE(img.has_value());

    auto const b       = std::span<std::uint8_t const>{img->bytes};
    auto const decoded = link::format::decodeEhFrame(
        b.subspan(static_cast<std::size_t>(img->ehOff), static_cast<std::size_t>(img->ehSize)),
        link::format::dwarfRegisterMappingOf(*t), "the test's read", rep);
    ASSERT_TRUE(decoded.has_value()) << diagnosticsOf(rep);
    std::vector<std::uint64_t> areas;
    for (auto const& fde : decoded->fdes) {
        if (!fde.dataArea.has_value()) continue;
        ASSERT_TRUE(fde.personality.has_value()) << "an FDE with an area whose CIE names no routine";
        std::uint64_t const routine =
            img->ehVa + fde.personality->fieldOffset + static_cast<std::uint64_t>(fde.personality->stored);
        EXPECT_TRUE(routine >= img->stubsVa && routine < img->stubsVa + img->stubsSize)
            << "the imported routine's field names no call stub";
        areas.push_back(img->ehVa + fde.dataArea->fieldOffset + static_cast<std::uint64_t>(fde.dataArea->stored));
    }
    ASSERT_EQ(areas.size(), 2u) << "`run` and its cold part each carry an area";
    std::sort(areas.begin(), areas.end());
    EXPECT_EQ(areas[1] - areas[0], 8u) << "the cold part's area is 8 bytes past `run`'s, as in the object";
    auto const off = img->fileOffsetOf(areas[0]);
    ASSERT_TRUE(off.has_value()) << "the area field names no byte of the image";
    ASSERT_LE(*off + table.size(), img->bytes.size());
    EXPECT_TRUE(std::equal(table.begin(), table.end(), img->bytes.begin() + static_cast<std::ptrdiff_t>(*off)))
        << "the reference object's handler did not reach the image: the bytes `run`'s area field names are not the "
           "object's area table";
}

// A relocatable object and an image that imports nothing state no handler: each refuses one by name, naming what the
// flavor lacks.
TEST(EhFrameHandlerWriters, AFlavorThatWritesNoHandlerRefusesOneByName) {
    auto const t = x86();
    ASSERT_TRUE(t);
    struct Case {
        char const* format;
        bool        withImport;
        char const* says;
    };
    Case const cases[] = {
        {"elf64-x86_64-linux", true,
         "elf::encode (ET_REL): function #0 carries the handler routine its object named for it, and a relocatable "
         "object's `.eh_frame` names a personality routine only through a slot of its own"},
        {"elf64-x86_64-linux-exec", false,
         "elf::encode (ET_EXEC): function #0 carries the handler routine its object named for it, and an image that "
         "imports nothing is written with no `.eh_frame`"},
        {"macho64-x86_64-darwin", true,
         "macho::encode (MH_OBJECT): function #0 carries the handler routine its object named for it, and this "
         "writer's relocatable object carries no `__eh_frame`"},
    };
    for (auto const& c : cases) {
        SCOPED_TRACE(c.format);
        auto const f = shipped(c.format);
        ASSERT_TRUE(f);
        Spec s;
        s.withImport = c.withImport;
        DiagnosticReporter rep;
        auto const         bytes = std::string_view{c.format}.starts_with("macho")
                                       ? macho::encode(makeModule(*t, s), *t, *f, rep)
                                       : elf::encode(makeModule(*t, s), *t, *f, rep);
        EXPECT_TRUE(bytes.empty()) << "a handler was written where no `.eh_frame` states it";
        EXPECT_NE(diagnosticsOf(rep).find(c.says), std::string::npos) << diagnosticsOf(rep);
    }
    // The Mach-O executable that imports nothing: the shipped document minus its signature request, which is what
    // lets the static arm be reached at all.
    auto const unsignedExec = macho::test::loadUnsignedExec("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(unsignedExec);
    Spec s;
    s.withImport = false;
    DiagnosticReporter rep;
    EXPECT_TRUE(macho::encode(makeModule(*t, s), *t, *unsignedExec, rep).empty())
        << "a handler was written where no `__eh_frame` states it";
    EXPECT_NE(diagnosticsOf(rep).find("macho::encode (MH_EXECUTE, imports nothing): function #0 carries the handler "
                                      "routine its object named for it, and an image that imports nothing is written "
                                      "with no `__eh_frame`"),
              std::string::npos)
        << diagnosticsOf(rep);
}

// A field is filled only with an address the dispatcher can use: a routine the link binds as a DATA import (its entry
// is the slot holding the address) and an area past its item's bytes are each refused by name, before the image.
TEST(EhFrameHandlerWriters, AFieldWithNoUsableTargetIsRefusedByName) {
    auto const t = x86();
    auto const f = shipped("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(t && f);
    {
        Spec s;
        s.importIsData = true;
        DiagnosticReporter rep;
        EXPECT_TRUE(elf::encode(makeModule(*t, s), *t, *f, rep).empty()) << "a routine bound as a data import was written";
        EXPECT_NE(diagnosticsOf(rep).find("function #3's handler names routine '__gcc_personality_v0', which this "
                                          "link binds as a data import"),
                  std::string::npos)
            << diagnosticsOf(rep);
    }
    {
        Spec s;
        s.areaOffset = static_cast<std::int64_t>(kTableBytes.size());
        DiagnosticReporter rep;
        EXPECT_TRUE(elf::encode(makeModule(*t, s), *t, *f, rep).empty()) << "an area outside its item was written";
        EXPECT_NE(diagnosticsOf(rep).find("function #0's handler names a data area at offset 10 of symbol #10, whose "
                                          "10 file byte(s) do not hold it"),
                  std::string::npos)
            << diagnosticsOf(rep);
    }
}

// A handler is reached only through its function's frame description: `buildEhFrame` refuses one on a function that
// carries none, rather than write a CIE no FDE names.
TEST(EhFrameHandlerWriters, AHandlerWithNoFrameDescriptionIsRefused) {
    auto const t = x86();
    ASSERT_TRUE(t);
    std::uint16_t const                     rsp = *t->registerByName("rsp");
    std::vector<std::optional<CfiFunction>> cfi(2);
    cfi[0] = CfiFunction{};
    cfi[0]->codeLength = 4;
    cfi[0]->initial    = CfiInitialState{rsp, 8, -8, std::nullopt};
    std::vector<std::optional<link::format::EhFrameHandler>> handlers(2);
    handlers[1] = link::format::EhFrameHandler{7, false};
    DiagnosticReporter rep;
    EXPECT_FALSE(link::format::buildEhFrame(cfi, link::format::dwarfRegisterMappingOf(*t), 8, rep, handlers).has_value());
    EXPECT_NE(diagnosticsOf(rep).find("function #1 names a handler and carries no call-frame information"),
              std::string::npos)
        << diagnosticsOf(rep);
}
