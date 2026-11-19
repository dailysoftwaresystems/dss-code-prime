// P69 fold 3f (lane `xa`) -- WHAT A UNIT'S `#pragma comment` ASKS OF ITS LINK, IN THE ACTIVE FORMAT'S FORM.
//
// The compile pipeline turns each request the preprocessor recorded into what the document being written says
// (`stateSourceLinkerRequests`): a LIBRARY is named in a relocatable object's dependent-libraries section, once, in
// the order first stated, where the document states one (ELF's `.deplibs`); an IMAGE takes none, in silence (a DSS
// link binds a library function through its descriptors); a LINKER DIRECTIVE where the document reads no
// directive is WARNED and dropped, as clang does on ELF. Two units of one object name their libraries in unit
// order, each once. And natively, on a Linux x86_64 host with ld.lld: a DSS object compiled from a unit stating
// `#pragma comment(lib, "m")` is linked by ld.lld, which SEARCHES the library -- the image needs libm.so.6 -- and
// runs; the same unit stating nothing links no libm (✔MEASURED P69 fold 3f: clang's object is linked so).
//
// COFF: a unit's `#pragma comment(linker, ...)` is folded and completed through the ONE home the COFF object
// reader's directives go through (`pe::applyUnitLinkerDirectives`), so the object DSS writes reads back as the unit
// asked -- its libraries quoted as cl quotes them (`-defaultlib:"user32"`, the writer's own spelling of the option
// cl writes as `/DEFAULTLIB`), a hide of its own definition Hidden, an
// alternate name a fallback, an image request handed on -- and a unit compiled straight into an image is linked as
// that object would be (its `/STACK:` the image's, a request no DSS image honours refused by name, a library taken
// in silence). Two units of one object state their libraries in unit order, each once, and each unit's options as
// its object would be read back. Natively, on Windows, link.exe searches the library a DSS object names: a
// `GetSystemMetrics` call links and runs with `comment(lib, "user32")` alone, and without it link.exe refuses the
// symbol (✔MEASURED P69 fold 3f, GP1: cl's object links so).
//
// MACH-O: a unit's libraries and options are its object's LC_LINKER_OPTION commands, read back as ld64 reads them
// (`macho::readMachOLinkerOption`) -- a library from either kind, an option ld64 would not take kept and warned by
// the link that makes an image, as ld64 warns it; a unit compiled straight into an image is linked so. Natively, on
// macOS arm64, ld64 searches the library a DSS object names: a `compress2` call links and runs with
// `comment(lib, "z")` alone, and without it ld64 refuses the symbol (✔MEASURED P69 fold 3f, run 20261117-173614-aa64206d: clang's object
// links so).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/object_format_schema.hpp"
#include "program/cli_args.hpp"
#include "program/program.hpp"
#include "arm_verdict_ledger.hpp"   // findOnPath
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"   // a host tool's command (`captureCmd`), link.exe's developer environment

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::test_support;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::vector<std::uint8_t> readFileBytes(fs::path const& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

fs::path writeText(fs::path const& dir, std::string_view name, std::string_view text) {
    fs::path const path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out << text;
    return path;
}

[[nodiscard]] std::string everyDiagnostic(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  (no diagnostics)"} : s;
}

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) n += d.code == code ? 1u : 0u;
    return n;
}

// The one object (`extension`) a compile wrote into `dir`, or an empty path.
[[nodiscard]] fs::path theObjectIn(fs::path const& dir, std::string_view extension = ".o") {
    fs::path found;
    for (auto const& e : fs::directory_iterator(dir))
        if (e.path().extension() == extension) found = found.empty() ? e.path() : fs::path{};
    return found;
}

// The dependent libraries an ELF object of `cpu` / `document` names, read back by DSS's own reader.
[[nodiscard]] std::vector<std::string> librariesOf(fs::path const& object, char const* cpu, char const* document) {
    auto const target = TargetSchema::loadShipped(cpu);
    auto const format = ObjectFormatSchema::loadShipped(document);
    if (!target.has_value() || !format.has_value()) {
        ADD_FAILURE() << "cannot load " << cpu << " / " << document;
        return {};
    }
    auto const bytes = readFileBytes(object);
    DiagnosticReporter rep;
    auto const read = elf::readRelocatableObject(bytes, **target, **format, rep);
    if (!read.has_value()) {
        ADD_FAILURE() << object.string() << " does not read back" << everyDiagnostic(rep);
        return {};
    }
    return read->linkerRequests.dependentLibraries;
}

[[nodiscard]] bool holds(std::vector<std::uint8_t> const& bytes, std::string_view text) {
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
}

}  // namespace

TEST(PragmaCommentLinkRequests, ARelocatableElfObjectNamesEachLibraryOnceInItsSection) {
    struct Pair {
        char const* cpu;
        char const* document;
    };
    for (Pair const& p : {Pair{"x86_64", "elf64-x86_64-linux"}, Pair{"arm64", "elf64-aarch64-linux"}}) {
        SCOPED_TRACE(p.document);
        ScratchDir scratch{Location::InsideRepo, "pragma-comment-object"};
        auto const src = writeText(scratch.path(), "unit.c",
                                   "#pragma comment(lib, \"m\")\n"
                                   "#pragma comment(lib, \"dss lib\")\n"
                                   "#pragma comment(lib, \"m\")\n"
                                   "int unit_answer(void) { return 42; }\n");
        Program prog;
        prog.setOutputDir(scratch.path());
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileFiles({src.string()}, "c", {std::string{p.cpu} + ":" + p.document}, rep), 0)
            << everyDiagnostic(rep);
        EXPECT_EQ(rep.warningCount(), 0u) << everyDiagnostic(rep);
        EXPECT_EQ(librariesOf(scratch.path() / "unit.o", p.cpu, p.document),
                  (std::vector<std::string>{"m", "dss lib"}))
            << "the object does not name each library once, in the order first stated";
    }
}

TEST(PragmaCommentLinkRequests, AnImageNamesNoLibraryAndADirectiveTheFormatCannotReadIsWarnedAndDropped) {
    ScratchDir scratch{Location::InsideRepo, "pragma-comment-image"};
    // An image: the library is taken by no link, in silence.
    {
        auto const src = writeText(scratch.path(), "prog.c",
                                   "#pragma comment(lib, \"m\")\n"
                                   "int main(void) { return 42; }\n");
        Program prog;
        prog.setOutputDir(scratch.path() / "image");
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileFiles({src.string()}, "c", {"x86_64:elf64-x86_64-linux-exec"}, rep), 0)
            << everyDiagnostic(rep);
        EXPECT_EQ(rep.warningCount(), 0u) << "an image link takes no library a unit names, in silence"
                                          << everyDiagnostic(rep);
        fs::path image;
        for (auto const& e : fs::directory_iterator(scratch.path() / "image")) image = e.path();
        ASSERT_FALSE(image.empty());
        EXPECT_FALSE(holds(readFileBytes(image), ".deplibs")) << "the image names a dependent library";
    }
    // A directive ELF reads none of: warned, by name, and dropped; the library beside it is still named.
    {
        auto const src = writeText(scratch.path(), "unit.c",
                                   "#pragma comment(linker, \"/STACK:0x200000\")\n"
                                   "#pragma comment(lib, \"m\")\n"
                                   "int unit_answer(void) { return 42; }\n");
        Program prog;
        prog.setOutputDir(scratch.path() / "object");
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileFiles({src.string()}, "c", {"x86_64:elf64-x86_64-linux"}, rep), 0)
            << everyDiagnostic(rep);
        EXPECT_EQ(countCode(rep, DiagnosticCode::K_LinkerDirectiveIgnored), 1u) << everyDiagnostic(rep);
        bool named = false;
        for (auto const& d : rep.all())
            named = named || (d.code == DiagnosticCode::K_LinkerDirectiveIgnored
                              && d.severity == DiagnosticSeverity::Warning
                              && d.actual.find("/STACK:0x200000") != std::string::npos);
        EXPECT_TRUE(named) << "the dropped directive is not named in a warning" << everyDiagnostic(rep);
        EXPECT_EQ(librariesOf(scratch.path() / "object" / "unit.o", "x86_64", "elf64-x86_64-linux"),
                  (std::vector<std::string>{"m"}));
    }
}

TEST(PragmaCommentLinkRequests, TwoUnitsOfOneObjectNameTheirLibrariesInUnitOrderEachOnce) {
    ScratchDir scratch{Location::InsideRepo, "pragma-comment-merged"};
    scratch.useAsCwd();
    auto const a = writeText(scratch.path(), "merged_a.c",
                             "#pragma comment(lib, \"z\")\n"
                             "#pragma comment(lib, \"m\")\n"
                             "extern int helper(int x);\n"
                             "int twice(int x) { return helper(x); }\n");
    auto const b = writeText(scratch.path(), "merged_b.c",
                             "#pragma comment(lib, \"m\")\n"
                             "#pragma comment(lib, \"dl\")\n"
                             "int helper(int x) { return x + x; }\n");
    Program prog;
    prog.setOutputDir(scratch.path() / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileUnits({a.generic_string(), b.generic_string()}, "c", {"x86_64:elf64-x86_64-linux"}, rep),
              0)
        << everyDiagnostic(rep);
    fs::path const object = theObjectIn(scratch.path() / "out");
    ASSERT_FALSE(object.empty()) << "the merged route did not write ONE object";
    EXPECT_EQ(librariesOf(object, "x86_64", "elf64-x86_64-linux"), (std::vector<std::string>{"z", "m", "dl"}))
        << "the merged object does not name the units' libraries in unit order, each once";
}

// THE NATIVE CELL: a DSS object compiled from a unit stating `#pragma comment(lib, "m")` is linked by ld.lld
// (through the host's `cc`, `-fuse-ld=lld`), which SEARCHES the library -- the image needs libm.so.6 -- and runs to
// 42; the same unit stating nothing links no libm. `--no-as-needed` keeps a library nothing references in the
// image's needs, so the need is ld.lld's answer to the section alone.
TEST(PragmaCommentLinkRequests, LdLldSearchesTheLibraryADssUnitNames) {
#if defined(__linux__) && defined(__x86_64__)
    std::string const cc  = findOnPath("cc");
    std::string const lld = findOnPath("ld.lld");
    if (cc.empty() || lld.empty())
        GTEST_SKIP() << "this host has no `cc` or no `ld.lld` on PATH (the Ubuntu x86_64 leg has both)";
    ScratchDir scratch{Location::Temp, "pragma-comment-native"};
    auto const linkAndRun = [&](char const* stem, char const* pragma) {
        fs::path const dir = scratch.path() / stem;
        fs::create_directories(dir);
        auto const src = writeText(dir, "unit.c", std::string{pragma} + "int main(void) { return 42; }\n");
        Program prog;
        prog.setOutputDir(dir);
        DiagnosticReporter rep;
        EXPECT_EQ(prog.compileFiles({src.string()}, "c", {"x86_64:elf64-x86_64-linux"}, rep), 0)
            << stem << everyDiagnostic(rep);
        fs::path const exe = dir / "prog";
        // The host's `cc` runs through the shell, both streams kept (`captureCmd`), as every native cell runs a host
        // tool: `runBinary` is for a binary the test PRODUCED -- it makes its path executable first, which a tool the
        // test does not own refuses (chmod of the host's `cc`: EPERM, MEASURED on the Ubuntu leg).
        fs::path const log = dir / "link.log";
        int const linked = std::system(native_probe::captureCmd(
            "\"" + cc + "\" -fuse-ld=lld -Wl,--no-as-needed \"" + (dir / "unit.o").string() + "\" -o \""
                + exe.string() + "\"",
            log).c_str());
        EXPECT_EQ(linked, 0) << stem << ": " << native_probe::tailOf(log, 20, "the link");
        auto const ran = runBinary(exe);
        auto const image = readFileBytes(exe);
        return std::pair<std::uint32_t, bool>{ran.exitCode, holds(image, "libm.so.6")};
    };
    auto const named = linkAndRun("names_m", "#pragma comment(lib, \"m\")\n");
    EXPECT_EQ(named.first, 42u);
    EXPECT_TRUE(named.second) << "ld.lld did not search the library the DSS unit names";
    auto const plain = linkAndRun("names_none", "");
    EXPECT_EQ(plain.first, 42u);
    EXPECT_FALSE(plain.second) << "the control links libm without being asked";
#else
    GTEST_SKIP() << "the cell links an x86_64 Linux object with ld.lld; it runs on Linux x86_64 hosts";
#endif
}

// ══ COFF ═════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace {

// The module a COFF object DSS wrote reads back as, through DSS's own reader.
[[nodiscard]] std::optional<AssembledModule> readCoff(fs::path const& object, DiagnosticReporter& rep) {
    auto const target = TargetSchema::loadShipped("x86_64");
    auto const format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    if (!target.has_value() || !format.has_value()) {
        ADD_FAILURE() << "cannot load x86_64 / pe64-x86_64-windows";
        return std::nullopt;
    }
    auto const bytes = readFileBytes(object);
    return pe::readRelocatableObject(bytes, **target, **format, rep);
}

// A PE32+ image's SizeOfStackReserve (the optional header's field at 72).
[[nodiscard]] std::optional<std::uint64_t> stackReserveOf(std::vector<std::uint8_t> const& image) {
    auto const u32 = [&](std::size_t at) -> std::uint32_t {
        return at + 4 <= image.size() ? static_cast<std::uint32_t>(image[at]) | image[at + 1] << 8
                                            | image[at + 2] << 16 | static_cast<std::uint32_t>(image[at + 3]) << 24
                                      : 0u;
    };
    std::size_t const optional = static_cast<std::size_t>(u32(0x3C)) + 24u;
    if (optional + 80u > image.size()) return std::nullopt;
    return static_cast<std::uint64_t>(u32(optional + 72u)) | static_cast<std::uint64_t>(u32(optional + 76u)) << 32;
}

}  // namespace

TEST(PragmaCommentLinkRequests, ACoffObjectReadsBackAsItsUnitAsked) {
    ScratchDir scratch{Location::InsideRepo, "pragma-comment-coff-object"};
    auto const src = writeText(scratch.path(), "unit.c",
                               "#pragma comment(lib, \"user32\")\n"
                               "#pragma comment(lib, \"dss lib\")\n"
                               "#pragma comment(lib, \"user32\")\n"
                               "#pragma comment(linker, \"/STACK:0x200000\")\n"
                               "#pragma comment(linker, \"-exclude-symbols:dss_hidden\")\n"
                               "#pragma comment(linker, \"/alternatename:dss_hook=dss_hook_default\")\n"
                               "int dss_hidden(void) { return 1; }\n"
                               "int dss_hook_default(void) { return 2; }\n"
                               "int dss_hook(void);\n"
                               "int unit_answer(void) { return dss_hidden() + dss_hook(); }\n");
    Program prog;
    prog.setOutputDir(scratch.path());
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileFiles({src.string()}, "c", {"x86_64:pe64-x86_64-windows"}, rep), 0) << everyDiagnostic(rep);
    EXPECT_EQ(rep.warningCount(), 0u) << everyDiagnostic(rep);
    fs::path const object = scratch.path() / "unit.obj";
    auto const bytes = readFileBytes(object);
    // Each library as the writer states every directive (`-defaultlib`, the vocabulary's first prefix and option),
    // the name QUOTED as cl quotes it (run 20261117-143416-a9be43b7), once.
    EXPECT_TRUE(holds(bytes, "-defaultlib:\"user32\"")) << "the object does not name user32, quoted";
    EXPECT_TRUE(holds(bytes, "-defaultlib:\"dss lib\"")) << "a library whose name holds a space is not quoted";
    EXPECT_TRUE(holds(bytes, "/STACK:0x200000")) << "the unit's image request is not handed on";
    DiagnosticReporter readRep;
    auto const read = readCoff(object, readRep);
    ASSERT_TRUE(read.has_value()) << everyDiagnostic(readRep);
    EXPECT_EQ(read->linkerRequests.dependentLibraries, (std::vector<std::string>{"user32", "dss lib"}))
        << "the object does not name each library once, in the order first stated";
    ASSERT_EQ(read->linkerRequests.image.size(), 1u);
    EXPECT_EQ(read->linkerRequests.image[0].spelled, "/STACK:0x200000");
    bool hidden = false;
    for (auto const& s : read->symbols) {
        hidden = hidden || (s.name == "dss_hidden" && s.visibility == SymbolVisibility::Hidden);
    }
    EXPECT_TRUE(hidden) << "the unit's hide of its own definition did not make it Hidden";
    std::string fallback;
    for (auto const& e : read->externImports) {
        if (e.mangledName == "dss_hook") fallback = e.fallbackName;
    }
    EXPECT_EQ(fallback, "dss_hook_default") << "the unit's alternate name gave its reference no fallback";
}

TEST(PragmaCommentLinkRequests, AUnitCompiledIntoAnImageIsLinkedAsItsObjectWouldBe) {
    ScratchDir scratch{Location::InsideRepo, "pragma-comment-coff-image"};
    // The unit's `/STACK:` is the image's, and its library is taken in silence (a DSS link binds it through its
    // descriptors).
    {
        auto const src = writeText(scratch.path(), "prog.c",
                                   "#pragma comment(linker, \"/STACK:0x300000\")\n"
                                   "#pragma comment(lib, \"user32\")\n"
                                   "int main(void) { return 42; }\n");
        Program prog;
        prog.setOutputDir(scratch.path() / "image");
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileFiles({src.string()}, "c", {"x86_64:pe64-x86_64-windows-exec"}, rep), 0)
            << everyDiagnostic(rep);
        EXPECT_EQ(rep.warningCount(), 0u) << everyDiagnostic(rep);
        fs::path image;
        for (auto const& e : fs::directory_iterator(scratch.path() / "image")) {
            if (e.path().extension() == ".exe") image = e.path();
        }
        ASSERT_FALSE(image.empty());
        EXPECT_EQ(stackReserveOf(readFileBytes(image)), std::optional<std::uint64_t>{0x300000u})
            << "the unit's /STACK: is not the image's";
    }
    // A request no DSS image honours (`/merge:`, the vocabulary's `refused` row) is refused by name, as the link
    // refuses an object's.
    {
        auto const src = writeText(scratch.path(), "merged.c",
                                   "#pragma comment(linker, \"/merge:.dssm=.data\")\n"
                                   "int main(void) { return 42; }\n");
        Program prog;
        prog.setOutputDir(scratch.path() / "refused");
        DiagnosticReporter rep;
        EXPECT_NE(prog.compileFiles({src.string()}, "c", {"x86_64:pe64-x86_64-windows-exec"}, rep), 0);
        bool named = false;
        for (auto const& d : rep.all()) {
            named = named || (d.code == DiagnosticCode::K_LinkerDirectiveUnhonourable
                              && d.actual.find("/merge:.dssm=.data") != std::string::npos);
        }
        EXPECT_TRUE(named) << "the refused request is not named" << everyDiagnostic(rep);
    }
}

// The merged route on COFF: two units' libraries in unit order, each once, and each unit's options applied to the
// one module as its object would be read back -- the second unit's hide makes its definition Hidden, the first unit's
// alternate name gives its reference a fallback.
TEST(PragmaCommentLinkRequests, TwoCoffUnitsOfOneObjectStateTheirRequestsInUnitOrder) {
    ScratchDir scratch{Location::InsideRepo, "pragma-comment-coff-merged"};
    scratch.useAsCwd();
    auto const a = writeText(scratch.path(), "merged_a.c",
                             "#pragma comment(lib, \"user32\")\n"
                             "#pragma comment(lib, \"gdi32\")\n"
                             "#pragma comment(linker, \"/alternatename:dss_hook=dss_hook_default\")\n"
                             "int dss_hook(void);\n"
                             "int dss_hook_default(void) { return 2; }\n"
                             "extern int helper(int x);\n"
                             "int twice(int x) { return helper(x) + dss_hook(); }\n");
    auto const b = writeText(scratch.path(), "merged_b.c",
                             "#pragma comment(lib, \"gdi32\")\n"
                             "#pragma comment(lib, \"advapi32\")\n"
                             "#pragma comment(linker, \"-exclude-symbols:helper\")\n"
                             "int helper(int x) { return x + x; }\n");
    Program prog;
    prog.setOutputDir(scratch.path() / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileUnits({a.generic_string(), b.generic_string()}, "c", {"x86_64:pe64-x86_64-windows"}, rep),
              0)
        << everyDiagnostic(rep);
    EXPECT_EQ(rep.warningCount(), 0u) << everyDiagnostic(rep);
    fs::path const object = theObjectIn(scratch.path() / "out", ".obj");
    ASSERT_FALSE(object.empty()) << "the merged route did not write ONE object";
    DiagnosticReporter readRep;
    auto const read = readCoff(object, readRep);
    ASSERT_TRUE(read.has_value()) << everyDiagnostic(readRep);
    EXPECT_EQ(read->linkerRequests.dependentLibraries, (std::vector<std::string>{"user32", "gdi32", "advapi32"}))
        << "the merged object does not name the units' libraries in unit order, each once";
    bool hidden = false;
    for (auto const& s : read->symbols) {
        hidden = hidden || (s.name == "helper" && s.visibility == SymbolVisibility::Hidden);
    }
    EXPECT_TRUE(hidden) << "the second unit's hide of its definition did not make it Hidden";
    std::string fallback;
    for (auto const& e : read->externImports) {
        if (e.mangledName == "dss_hook") fallback = e.fallbackName;
    }
    EXPECT_EQ(fallback, "dss_hook_default") << "the first unit's alternate name gave its reference no fallback";
}

// ══ MACH-O ═══════════════════════════════════════════════════════════════════════════════════════════════════════

namespace {

// The module a Mach-O object DSS wrote reads back as, through DSS's own reader.
[[nodiscard]] std::optional<AssembledModule> readMachO(fs::path const& object, DiagnosticReporter& rep) {
    auto const target = TargetSchema::loadShipped("arm64");
    auto const format = ObjectFormatSchema::loadShipped("macho64-arm64-darwin");
    if (!target.has_value() || !format.has_value()) {
        ADD_FAILURE() << "cannot load arm64 / macho64-arm64-darwin";
        return std::nullopt;
    }
    auto const bytes = readFileBytes(object);
    return macho::readRelocatableObject(bytes, **target, **format, rep);
}

}  // namespace

// A unit's libraries -- `comment(lib, ...)` and a `comment(linker, "-l...")` alike, as ld64 reads either -- and its
// other options are named in its object's LC_LINKER_OPTION commands, read back as ld64 reads them: the one string
// `-framework CoreFoundation` kept, and warned by the link that makes an image, as ld64 warns it.
TEST(PragmaCommentLinkRequests, AMachOObjectNamesItsUnitsLibrariesAndOptions) {
    ScratchDir scratch{Location::InsideRepo, "pragma-comment-macho-object"};
    auto const src = writeText(scratch.path(), "unit.c",
                               "#pragma comment(lib, \"z\")\n"
                               "#pragma comment(lib, \"dss lib\")\n"
                               "#pragma comment(linker, \"-framework CoreFoundation\")\n"
                               "#pragma comment(linker, \"-lm\")\n"
                               "#pragma comment(lib, \"z\")\n"
                               "int unit_answer(void) { return 42; }\n");
    Program prog;
    prog.setOutputDir(scratch.path());
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileFiles({src.string()}, "c", {"arm64:macho64-arm64-darwin"}, rep), 0) << everyDiagnostic(rep);
    EXPECT_EQ(rep.warningCount(), 0u) << "a relocatable object says nothing of what it hands on"
                                      << everyDiagnostic(rep);
    DiagnosticReporter readRep;
    auto const read = readMachO(scratch.path() / "unit.o", readRep);
    ASSERT_TRUE(read.has_value()) << everyDiagnostic(readRep);
    EXPECT_EQ(read->linkerRequests.dependentLibraries, (std::vector<std::string>{"z", "dss lib", "m"}))
        << "the object does not name each library once, in the order first stated";
    EXPECT_EQ(read->linkerRequests.linkerOptions,
              (std::vector<std::vector<std::string>>{{"-framework CoreFoundation"}}));
    EXPECT_EQ(read->linkerRequests.warnings.size(), 1u);
}

// A unit compiled straight into a Mach-O image: its library is taken in silence (a DSS link binds it through its
// descriptors), and the option ld64 would not take from its object is warned, by name, as ld64 warns it.
TEST(PragmaCommentLinkRequests, AMachOUnitCompiledIntoAnImageWarnsWhereLd64Would) {
    ScratchDir scratch{Location::InsideRepo, "pragma-comment-macho-image"};
    auto const src = writeText(scratch.path(), "prog.c",
                               "#pragma comment(lib, \"z\")\n"
                               "#pragma comment(linker, \"-framework CoreFoundation\")\n"
                               "int main(void) { return 42; }\n");
    Program prog;
    prog.setOutputDir(scratch.path() / "image");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileFiles({src.string()}, "c", {"arm64:macho64-arm64-darwin-exec"}, rep), 0)
        << everyDiagnostic(rep);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_LinkerDirectiveIgnored), 1u) << everyDiagnostic(rep);
    bool named = false;
    for (auto const& d : rep.all()) {
        named = named || (d.code == DiagnosticCode::K_LinkerDirectiveIgnored
                          && d.severity == DiagnosticSeverity::Warning
                          && d.actual.find("'-framework CoreFoundation'") != std::string::npos);
    }
    EXPECT_TRUE(named) << "the option ld64 would warn of is not named" << everyDiagnostic(rep);
}

// THE NATIVE CELL (macOS arm64): ld64 (through the host's `cc`) links a DSS object that calls zlib's `compress2`
// and names `z` by `#pragma comment(lib, ...)` alone -- the library is on no command line -- and the program runs to
// 42; the same unit naming nothing is refused by ld64 (undefined `_compress2`). ✔MEASURED P69 fold 3f: clang's object
// links so.
TEST(PragmaCommentLinkRequests, Ld64SearchesTheLibraryADssUnitNames) {
#if defined(__APPLE__) && defined(__aarch64__)
    std::string const cc = findOnPath("cc");
    if (cc.empty()) GTEST_SKIP() << "this host has no `cc` on PATH (the macOS arm64 leg has one)";
    ScratchDir scratch{Location::Temp, "pragma-comment-native-macho"};
    auto const compileAndLink = [&](char const* stem, char const* pragma) {
        fs::path const dir = scratch.path() / stem;
        fs::create_directories(dir);
        auto const src = writeText(
            dir, "unit.c",
            std::string{pragma}
                + "int compress2(unsigned char *d, unsigned long *dl, const unsigned char *s, unsigned long sl,\n"
                  "              int level);\n"
                  "int main(void) {\n"
                  "    unsigned char out[64];\n"
                  "    unsigned long n = sizeof out;\n"
                  "    return compress2(out, &n, (const unsigned char *)\"dss\", 3, 6) == 0 ? 42 : 1;\n"
                  "}\n");
        Program prog;
        prog.setOutputDir(dir);
        DiagnosticReporter rep;
        EXPECT_EQ(prog.compileFiles({src.string()}, "c", {"arm64:macho64-arm64-darwin"}, rep), 0)
            << stem << everyDiagnostic(rep);
        fs::path const exe = dir / "prog";
        // The host's `cc` through the shell (`captureCmd`), as in the ELF cell above: `runBinary` would make the
        // host's own tool executable first, which a tool the test does not own refuses.
        fs::path const log = dir / "link.log";
        int const linked = std::system(native_probe::captureCmd(
            "\"" + cc + "\" \"" + (dir / "unit.o").string() + "\" -o \"" + exe.string() + "\"", log).c_str());
        return std::tuple{linked, exe, native_probe::tailOf(log, 20, "the link")};
    };
    auto const [named, namedExe, namedSaid] = compileAndLink("names_z", "#pragma comment(lib, \"z\")\n");
    ASSERT_EQ(named, 0) << "ld64 did not link the DSS object that names z: " << namedSaid;
    auto const ran = runBinary(namedExe);
    ASSERT_TRUE(ran.spawned) << ran.diagnostic;
    EXPECT_EQ(ran.exitCode, 42u);
    auto const [plain, plainExe, plainSaid] = compileAndLink("names_none", "");
    EXPECT_NE(plain, 0) << "ld64 linked compress2 with no library naming it -- the control does not hold: "
                        << plainSaid;
#else
    GTEST_SKIP() << "the cell links a DSS Mach-O object with ld64; it runs on macOS arm64 hosts";
#endif
}

// THE NATIVE CELL (Windows): link.exe links a DSS object that calls `GetSystemMetrics` and names user32 by
// `#pragma comment(lib, ...)` alone -- the library is on no command line -- and the program runs to 42; the same
// unit naming nothing is refused by link.exe (LNK2019). The image enters at `main` and leaves through `ExitProcess`
// (a DSS object carries no C runtime startup of its own).
TEST(PragmaCommentLinkRequests, LinkExeSearchesTheLibraryADssUnitNames) {
#if defined(_WIN32)
    ScratchDir scratch{Location::InsideRepo, "pragma-comment-native-coff"};
    auto const dir = scratch.path();
    namespace np = test_support::native_probe;
    auto const msvc = np::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = np::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    auto const compile = [&](char const* stem, char const* pragma) {
        fs::path const out = dir / stem;
        fs::create_directories(out);
        auto const src = writeText(out, "unit.c",
                                   std::string{pragma}
                                       + "int GetSystemMetrics(int index);\n"
                                         "void ExitProcess(unsigned code);\n"
                                         "int main(void) {\n"
                                         "    ExitProcess(GetSystemMetrics(0) > 0 ? 42u : 1u);\n"
                                         "    return 0;\n"
                                         "}\n");
        Program prog;
        prog.setOutputDir(out);
        DiagnosticReporter rep;
        EXPECT_EQ(prog.compileFiles({src.string()}, "c", {"x86_64:pe64-x86_64-windows"}, rep), 0)
            << stem << everyDiagnostic(rep);
        return out;
    };
    auto const named = compile("names_user32", "#pragma comment(lib, \"user32\")\n");
    ASSERT_TRUE(tools.run("link /nologo /SUBSYSTEM:CONSOLE /ENTRY:main /OUT:named.exe \""
                          + (named / "unit.obj").string() + "\" kernel32.lib"))
        << "link.exe did not link the DSS object that names user32";
    auto const ran = runBinary(dir / "named.exe");
    ASSERT_TRUE(ran.spawned) << ran.diagnostic;
    EXPECT_EQ(ran.exitCode, 42u);
    auto const plain = compile("names_none", "");
    EXPECT_FALSE(tools.run("link /nologo /SUBSYSTEM:CONSOLE /ENTRY:main /OUT:plain.exe \""
                           + (plain / "unit.obj").string() + "\" kernel32.lib"))
        << "link.exe linked GetSystemMetrics with no library naming it -- the control does not hold";
#else
    GTEST_SKIP() << "the cell links a DSS COFF object with link.exe; it runs on Windows hosts";
#endif
}
