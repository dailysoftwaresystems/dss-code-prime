// P69 fold 3e (lane xa) -- the review of fold 3d's X11: a name the image defines at its own header (`imageDefinedSymbols`,
// `imageHeader`) is reached by the REFERENCES' objects in THREE shapes, and DSS's link binds every one through its ONE
// binding (`linker.cpp`'s `imageDefinedAddressOf`, the address `addImageDefinedSymbolVas` gives the row's id):
//   * DIRECT -- gcc's x86_64 PIE object reads `__ehdr_start` by R_X86_64_PC32 (cl's and clang-cl's REL32 to
//     `__ImageBase` are `test_pe_image_defined_symbols.cpp`'s `Reader::Cl` cells);
//   * THROUGH A GOT SLOT the link fills -- clang's x86_64 PIE object by R_X86_64_REX_GOTPCRELX, aarch64 gcc's PIE object
//     by R_AARCH64_ADR_GOT_PAGE + R_AARCH64_LD64_GOT_LO12_NC;
//   * THROUGH A SLOT THE OBJECT DEFINES -- MinGW-w64 gcc's object reads `__ImageBase` by a REL32 to its own
//     `.refptr.__ImageBase`, whose ADDR64 names the image's name.
// The one shape the link refuses -- a read through an IMPORT slot no library fills -- only a DSS unit lowered for a
// document that does not state the name makes (`test_pe_image_defined_symbols.cpp`).
//
// ✔MEASURED P69 fold 3e (the fixtures' build, recorded in tests/link/data/image_defined_reference.source.c): each
// object's relocations as above, and the reference's own program of it -- the object beside a `main` returning
// `check()`, linked by its own driver (GNU ld, ld.lld's PIE default under clang, MinGW-w64's GNU ld) -- exits 42.
//
// THE UNIVERSE: each shape's reference object x every executable document of its format and ISA (ELF: exec and pie;
// PE: exec). On EVERY leg each cell links beside a DSS `main`, the object's relocations are the shape the cell names
// (read by this file's own walk, not by the readers under test), and the image defines the name -- its bytes import
// no such name. Where the host runs the image, it RUNS to 42: `check()` found the image's own magic at the name.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"

#include "host_native_target.hpp"
#include "image_dependency_table.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

constexpr char const* kMain = "int check(void);\nint main(void) { return check(); }\n";

// One reference object, one executable document it is linked for, and what the object must hold.
struct Cell {
    char const*              fixture;
    char const*              exec;        // the DSS target the image is linked for
    char const*              runsAs;      // the host exec target whose host runs that image
    char const*              name;        // the name the image defines and the object reads
    char const*              shape;       // DIRECT / GOT SLOT / OBJECT'S SLOT, for the trace
    std::vector<std::uint32_t> elfTypes;  // ELF: the relocation types against `name`, exactly
};

constexpr char const* kGccX64   = "image_defined_reference_ehdr_x86_64_elf_gcc_pie_O2.o";
constexpr char const* kClangX64 = "image_defined_reference_ehdr_x86_64_elf_clang_pie_O2.o";
constexpr char const* kGccArm64 = "image_defined_reference_ehdr_arm64_elf_gcc_pie_O2.o";
constexpr char const* kMingw    = "image_defined_reference_imagebase_x86_64_pe_gcc_O2.o";

constexpr std::uint32_t kX64Pc32 = 2, kX64RexGotpcrelx = 42, kA64AdrGotPage = 311, kA64Ld64GotLo12 = 312;

[[nodiscard]] std::vector<Cell> const& cells() {
    static std::vector<Cell> const all{
        // gcc reads each of the four magic bytes through its own R_X86_64_PC32 (✔MEASURED by this file's walk of the
        // fixture, P69 fold 3e, macos-arm64-debug); clang and aarch64 gcc load the address once.
        {kGccX64, "x86_64:elf64-x86_64-linux-exec", "x86_64:elf64-x86_64-linux-exec", "__ehdr_start", "DIRECT",
         {kX64Pc32, kX64Pc32, kX64Pc32, kX64Pc32}},
        {kGccX64, "x86_64:elf64-x86_64-linux-pie", "x86_64:elf64-x86_64-linux-exec", "__ehdr_start", "DIRECT",
         {kX64Pc32, kX64Pc32, kX64Pc32, kX64Pc32}},
        {kClangX64, "x86_64:elf64-x86_64-linux-exec", "x86_64:elf64-x86_64-linux-exec", "__ehdr_start", "GOT SLOT",
         {kX64RexGotpcrelx}},
        {kClangX64, "x86_64:elf64-x86_64-linux-pie", "x86_64:elf64-x86_64-linux-exec", "__ehdr_start", "GOT SLOT",
         {kX64RexGotpcrelx}},
        {kGccArm64, "arm64:elf64-aarch64-linux-exec", "arm64:elf64-aarch64-linux-exec", "__ehdr_start", "GOT SLOT",
         {kA64AdrGotPage, kA64Ld64GotLo12}},
        {kGccArm64, "arm64:elf64-aarch64-linux-pie", "arm64:elf64-aarch64-linux-exec", "__ehdr_start", "GOT SLOT",
         {kA64AdrGotPage, kA64Ld64GotLo12}},
        {kMingw, "x86_64:pe64-x86_64-windows-exec", "x86_64:pe64-x86_64-windows-exec", "__ImageBase", "OBJECT'S SLOT",
         {}},
    };
    return all;
}

[[nodiscard]] std::string labelOf(Cell const& c) {
    return std::string{c.fixture} + " -> " + c.exec + " (" + c.shape + ")";
}

[[nodiscard]] fs::path fixturePath(std::string_view name) {
    return dss::test::repoRoot() / "tests" / "link" / "data" / std::string{name};
}

[[nodiscard]] std::vector<std::uint8_t> bytesOf(fs::path const& path) {
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

// The types of an ELF64 little-endian relocatable object's RELA entries whose symbol is `name`, in order.
[[nodiscard]] std::vector<std::uint32_t> elfRelocationTypesAgainst(std::vector<std::uint8_t> const& b,
                                                                   std::string_view name) {
    using namespace test_support::image_deps_detail;
    std::vector<std::uint32_t> out;
    if (b.size() < 64u || b[0] != 0x7F || b[1] != 'E' || b[4] != 2u || b[5] != 1u) return out;
    std::uint64_t const shoff     = rdU64(b, 0x28);
    std::uint16_t const shentsize = rdU16(b, 0x3A);
    std::uint16_t const shnum     = rdU16(b, 0x3C);
    if (shoff == 0u || shentsize < 64u || shoff + std::uint64_t{shnum} * shentsize > b.size()) return out;
    auto const sh = [&](std::uint32_t i) { return static_cast<std::size_t>(shoff + std::uint64_t{i} * shentsize); };
    for (std::uint32_t s = 0; s < shnum; ++s) {
        if (rdU32(b, sh(s) + 4) != 4u /*SHT_RELA*/) continue;
        std::uint32_t const symtab = rdU32(b, sh(s) + 0x28);
        if (symtab >= shnum) continue;
        std::uint32_t const strtab = rdU32(b, sh(symtab) + 0x28);
        if (strtab >= shnum) continue;
        std::size_t const symOff = rdU64(b, sh(symtab) + 0x18), strOff = rdU64(b, sh(strtab) + 0x18);
        std::size_t const relOff = rdU64(b, sh(s) + 0x18), relSize = rdU64(b, sh(s) + 0x20);
        for (std::size_t e = relOff; e + 24u <= relOff + relSize && e + 24u <= b.size(); e += 24u) {
            std::uint64_t const info = rdU64(b, e + 8);
            std::size_t const   sym  = symOff + static_cast<std::size_t>(info >> 32) * 24u;
            if (sym + 24u > b.size()) continue;
            if (rdCStr(b, strOff + rdU32(b, sym)) == name) out.push_back(static_cast<std::uint32_t>(info & 0xFFFFFFFFu));
        }
    }
    return out;
}

// A COFF object's symbol names, split into the defined ones (a section number above 0) and the undefined ones.
struct CoffNames {
    std::set<std::string> defined, undefined;
};

[[nodiscard]] CoffNames coffSymbolNames(std::vector<std::uint8_t> const& b) {
    using namespace test_support::image_deps_detail;
    CoffNames out;
    if (b.size() < 20u) return out;
    std::uint32_t const symtab = rdU32(b, 8), nsyms = rdU32(b, 12);
    std::size_t const   strtab = static_cast<std::size_t>(symtab) + std::size_t{nsyms} * 18u;
    for (std::uint32_t i = 0; i < nsyms; ++i) {
        std::size_t const e = static_cast<std::size_t>(symtab) + std::size_t{i} * 18u;
        if (e + 18u > b.size()) break;
        std::string name;
        if (rdU32(b, e) == 0u) {
            name = rdCStr(b, strtab + rdU32(b, e + 4));
        } else {
            for (std::size_t k = 0; k < 8u && b[e + k] != 0u; ++k) name += static_cast<char>(b[e + k]);
        }
        auto const section = static_cast<std::int16_t>(rdU16(b, e + 12));
        (section > 0 ? out.defined : out.undefined).insert(name);
        i += b[e + 17];   // its auxiliary records
    }
    return out;
}

// What the image imports, by its format: PE's import table, ELF's undefined `.dynsym`.
[[nodiscard]] std::vector<std::string> importsOf(std::vector<std::uint8_t> const& b) {
    if (b.size() >= 2u && b[0] == 'M' && b[1] == 'Z') return test_support::peImportedSymbols(b);
    using namespace test_support::image_deps_detail;
    std::vector<std::string> out;
    if (b.size() < 64u || b[0] != 0x7F || b[1] != 'E' || b[4] != 2u || b[5] != 1u) return out;
    std::uint64_t const shoff     = rdU64(b, 0x28);
    std::uint16_t const shentsize = rdU16(b, 0x3A);
    std::uint16_t const shnum     = rdU16(b, 0x3C);
    if (shoff == 0u || shentsize < 64u || shoff + std::uint64_t{shnum} * shentsize > b.size()) return out;
    auto const sh = [&](std::uint32_t i) { return static_cast<std::size_t>(shoff + std::uint64_t{i} * shentsize); };
    for (std::uint32_t s = 0; s < shnum; ++s) {
        if (rdU32(b, sh(s) + 4) != 11u /*SHT_DYNSYM*/) continue;
        std::uint32_t const link = rdU32(b, sh(s) + 0x28);
        if (link >= shnum) continue;
        std::size_t const symOff = rdU64(b, sh(s) + 0x18), symSize = rdU64(b, sh(s) + 0x20);
        std::size_t const strOff = rdU64(b, sh(link) + 0x18);
        for (std::size_t e = symOff + 24u; e + 24u <= symOff + symSize && e + 24u <= b.size(); e += 24u) {
            if (rdU16(b, e + 6) != 0u /*SHN_UNDEF*/ || rdU32(b, e) == 0u) continue;
            out.push_back(rdCStr(b, strOff + rdU32(b, e)));
        }
    }
    return out;
}

// DSS's image of a DSS `main` beside the cell's object: its path, or empty with `rep` holding why.
[[nodiscard]] fs::path dssImageOf(fs::path const& dir, Cell const& cell, DiagnosticReporter& rep) {
    fs::create_directories(dir);
    auto const mainSrc = dir / "main.c";
    std::ofstream(mainSrc, std::ios::binary) << kMain;
    auto const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    if (p.compileFiles(std::vector<std::string>{mainSrc.string(), fixturePath(cell.fixture).string()}, "c",
                       std::vector<std::string>{cell.exec}, rep)
        != 0) {
        return {};
    }
    fs::path image;
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file() && (image.empty() || e.path().extension() == ".exe")) image = e.path();
    }
    return image;
}

}  // namespace

// Every leg: each reference object holds its shape's relocations against the name, and DSS links it beside a DSS
// `main` into every executable document of its format and ISA, the image defining the name (no import of it).
// Every cell is judged, whatever an earlier one did. RED-ON-DISABLE: let the link bind no name the image defines
// (`imageDefinedAddressOf` answering none) -> every cell is refused, the name undefined.
TEST(ImageDefinedReferenceShapes, EveryReferenceShapeLinksWithTheNameDefinedInTheImage) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "image-defined-reference-shapes"};
    std::size_t k = 0;
    for (Cell const& c : cells()) {
        SCOPED_TRACE(labelOf(c));
        auto const object = bytesOf(fixturePath(c.fixture));
        if (object.empty()) continue;   // reported
        if (c.elfTypes.empty()) {
            CoffNames const names = coffSymbolNames(object);
            std::string const slot = std::string{".refptr."} + c.name;
            EXPECT_TRUE(names.defined.contains(slot)) << "the object defines its own slot `" << slot << "`";
            EXPECT_TRUE(names.undefined.contains(c.name)) << "the object leaves `" << c.name << "` to the image";
        } else {
            EXPECT_EQ(elfRelocationTypesAgainst(object, c.name), c.elfTypes)
                << "the object reaches `" << c.name << "` by the cell's shape";
        }
        DiagnosticReporter rep;
        auto const image = dssImageOf(scratch.path() / std::to_string(k++), c, rep);
        if (image.empty()) {
            ADD_FAILURE() << "DSS links the reference's object beside a DSS main:" << diagnosticsOf(rep);
            continue;
        }
        auto const imports = importsOf(bytesOf(image));
        EXPECT_EQ(test_support::dependencyOccurrences(imports, c.name), 0u)
            << "the image defines `" << c.name << "`; it imports: " << test_support::joinDependencies(imports);
    }
}

// Where the host runs a cell's image (Windows x86_64: PE; Linux x86_64 and arm64: ELF): each runs to 42, as the
// reference's own program of the object does -- `check()` read the image's own magic at the name.
TEST(ImageDefinedReferenceShapesNative, EachImageRunsTo42) {
    auto const host = std::string{test_support::hostNativeTarget().execTarget};
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "image-defined-reference-shapes-run"};
    std::size_t k = 0, ran = 0;
    for (Cell const& c : cells()) {
        if (host != c.runsAs) continue;
        SCOPED_TRACE(labelOf(c));
        DiagnosticReporter rep;
        auto const image = dssImageOf(scratch.path() / std::to_string(k++), c, rep);
        if (image.empty()) {
            ADD_FAILURE() << "DSS links the reference's object beside a DSS main:" << diagnosticsOf(rep);
            continue;
        }
        auto const r = test_support::runBinary(image, test_support::kRunBudget);
        if (!r.spawned) {
            ADD_FAILURE() << r.diagnostic;
            continue;
        }
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "check() found no magic at the name: 1";
        ++ran;
    }
    if (ran == 0u) GTEST_SKIP() << "no cell's image runs on this host (" << host << ")";
}
