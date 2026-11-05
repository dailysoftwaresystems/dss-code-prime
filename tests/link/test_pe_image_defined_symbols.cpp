// P69 fold 3d (lane xa) -- D-LK-PE-IMAGE-DEFINES-NO-IMAGE-BASE-SYMBOL: the names every PE linker defines in the image
// it writes, for a reference no linked unit defines, are defined by DSS's link as the image's document states them
// (`imageDefinedSymbols`). PE's is `__ImageBase`, the image's DOS header (`imageHeader`):
//   * cl's dense `switch` keeps its jump table in `.text$mn` as IMAGE_REL_AMD64_ADDR32NB entries (the cases' RVAs)
//     and reads them through an `lea` of `__ImageBase` (`tests/link/data/coff_comdat_switch_cl_{O2,Od}.obj`, cl 19.51;
//     the source states how each was built);
//   * a C program reads the image's own header as `extern IMAGE_DOS_HEADER __ImageBase;`, and a DLL its module
//     handle as `(HINSTANCE)&__ImageBase`.
// ✔MEASURED P69 fold 3d (probe-reference-cc run 20261104-200657-1038ae7c, windows-x86_64-release, cl 19.51, link.exe):
// the C idiom compiles, links and runs to 42; a unit that DEFINES `int __ImageBase = 1;` links too and reads its own
// 1, so a unit's definition is that unit's and the image defines the name only for a reference nothing defines. Until
// this fold DSS refused each such object and unit `K_SymbolUndefined` "undefined symbol '__ImageBase'", and the exec
// and DLL documents declared no ADDR32NB row, so the switch objects' entries could not reach the image either.
//
// ★ THE NAME REACHES EVERY COMPILE WHOSE OUTPUT CAN END IN AN IMAGE. A DSS unit compiled `-c`, or into a static
// library, ends in an image as surely as one compiled into it, so every pe64 document states the name -- the
// relocatable and archive ones for the image their objects end in, the exec and DLL ones for the image they write.
// Each compile then reads a reference to it DIRECT, never through an import slot.
//
// THE UNIVERSE: {an EXE, a DLL} x {the reader a unit of the image's own compile, a `-c` object linked later, a static
// library's member linked later} x {a DSS unit that reads `__ImageBase`, cl's switch object, both}. Each cell links on
// every leg with the name defined in the image, never imported; on Windows x86_64 each RUNS to the image's header
// address (an EXE against the module handle `LoadLibraryA` returns for its own file, a DLL against the one its
// loader's `LoadLibraryA` returns), and the
// cl object's every case reads its table through it. Beside them: the second switch object, a unit that defines the
// name, the documents, and link.exe's and lld-link's programs of the switch objects.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "../core/native_c_probe.hpp"
#include "format_reject_support.hpp"
#include "host_native_target.hpp"
#include "image_dependency_table.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;
namespace reject = dss::link_format::test;

constexpr char const* kExec    = "x86_64:pe64-x86_64-windows-exec";
constexpr char const* kDll     = "x86_64:pe64-x86_64-windows-dll";
constexpr char const* kObject  = "x86_64:pe64-x86_64-windows";
constexpr char const* kArchive = "x86_64:pe64-x86_64-windows-staticlib";

constexpr char const* kSwitchO2 = "coff_comdat_switch_cl_O2.obj";
constexpr char const* kSwitchOd = "coff_comdat_switch_cl_Od.obj";

// ── THE THREE AXES ─────────────────────────────────────────────────────────────────────────────────────────────────
enum class Image : std::uint8_t { Exe, Dll };
enum class Route : std::uint8_t { Unit, Object, Library };
enum class Reader : std::uint8_t { Dss, Cl, Both };

struct Cell {
    Image  image;
    Route  route;
    Reader reader;
};

[[nodiscard]] std::string nameOf(Cell const& c) {
    std::string s = c.image == Image::Exe ? "exe" : "dll";
    s += c.route == Route::Unit ? "_unit" : c.route == Route::Object ? "_object" : "_library";
    s += c.reader == Reader::Dss ? "_dss" : c.reader == Reader::Cl ? "_cl" : "_both";
    return s;
}

[[nodiscard]] std::vector<Cell> theUniverse() {
    std::vector<Cell> cells;
    for (Image const i : {Image::Exe, Image::Dll}) {
        for (Route const r : {Route::Unit, Route::Object, Route::Library}) {
            for (Reader const d : {Reader::Dss, Reader::Cl, Reader::Both}) cells.push_back({i, r, d});
        }
    }
    return cells;
}

[[nodiscard]] bool readsDss(Reader r) { return r != Reader::Cl; }
[[nodiscard]] bool readsCl(Reader r) { return r != Reader::Dss; }

// The DSS reader: the address of the image's header, and whether its first two bytes are the DOS signature "MZ".
constexpr char const* kReaderUnit =
    "extern unsigned short const __ImageBase;\n"
    "void const *dss_base(void) { return &__ImageBase; }\n"
    "int dss_reads(void) { return __ImageBase == 0x5A4D; }\n";

// The checks of a cell's readers against `base`, the image's own header address: 5 is the DSS reader's address
// that is not the header, 6 its bytes that are not "MZ", 10 + k cl's case k read wrong through its table, 9 its
// default.
[[nodiscard]] std::string checksOf(Reader r, std::string_view base) {
    std::string s;
    if (readsDss(r)) {
        s += "    if (dss_base() != (void const *)(" + std::string{base} + ")) return 5;\n";
        s += "    if (!dss_reads()) return 6;\n";
    }
    if (readsCl(r)) {
        s += "    { static int const w[10] = {103, 500, 93, 111, 400, 33, 9, -101, 100, 4};\n"
             "      for (int k = 0; k < 10; ++k) { volatile int kk = k; if (pick(kk, 100) != w[k]) return 10 + k; }\n"
             "      if (pick(10, 100) != -1) return 9; }\n";
    }
    return s + "    return 42;\n";
}

[[nodiscard]] std::string declarationsOf(Reader r) {
    std::string s;
    if (readsDss(r)) s += "extern void const *dss_base(void);\nextern int dss_reads(void);\n";
    if (readsCl(r)) s += "int pick(int, int);\n";
    return s;
}

// The image's own unit: an EXE's `main` checks against its own module, a DLL's `dll_check` against the module handle
// its loader passes. An EXE that reads the header is handed its own file (`argv[1]`) and takes its module handle
// from `LoadLibraryA` of it: the loader returns the handle of a module the process has already loaded (DOCUMENTED,
// LoadLibraryA; MEASURED P69 r4probe/m4, LoadLibraryW of the running EXE's own file is GetModuleHandleW(NULL) from
// either cwd), and the shipped `<windows.h>` declares no `GetModuleHandleA` (READ, windows.json).
[[nodiscard]] std::string driverOf(Cell const& c) {
    if (c.image == Image::Exe) {
        if (!readsDss(c.reader)) return declarationsOf(c.reader) + "int main(void) {\n" + checksOf(c.reader, "") + "}\n";
        return "#include <windows.h>\n" + declarationsOf(c.reader)
             + "int main(int argc, char **argv) {\n"
               "    if (argc < 2) return 2;\n"
               "    HMODULE const self = LoadLibraryA(argv[1]);\n"
               "    if (self == NULL) return 3;\n"
             + checksOf(c.reader, "self") + "}\n";
    }
    return declarationsOf(c.reader) + "int dll_check(void const *module) {\n" + checksOf(c.reader, "module") + "}\n";
}

// What an image's program is run with: an EXE that reads the header its own file, a DLL's loader the DLL.
[[nodiscard]] std::vector<std::string> argumentsOf(Cell const& c, fs::path const& image) {
    if (c.image == Image::Exe && !readsDss(c.reader)) return {};
    return {image.string()};
}

constexpr char const* kDllLoader = R"(#include <windows.h>
typedef int (*check_fn)(void const *);
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    HMODULE const m = LoadLibraryA(argv[1]);
    if (m == NULL) return 3;
    check_fn const check = (check_fn)GetProcAddress(m, "dll_check");
    if (check == NULL) return 4;
    return check((void const *)m);
}
)";
constexpr char const* kCellExit =
    "42 is every reader at the image's header; 5 the DSS reader's address that is not it, 6 its bytes that are not "
    "\"MZ\", 10 + k cl's case k read wrong through its table, 9 its default; 2 no file named, 3 `LoadLibraryA` "
    "refused it, 4 the DLL names no `dll_check`";

// A unit that defines the name: its own definition, as link.exe keeps it.
constexpr char const* kDefined = "int __ImageBase = 1;\nint main(void) { return __ImageBase == 1 ? 42 : 7; }\n";

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

[[nodiscard]] fs::path writeUnit(fs::path const& dir, std::string_view stem, std::string_view text) {
    fs::create_directories(dir);
    auto const p = dir / (std::string{stem} + ".c");
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

// One DSS compile of `inputs` (sources and objects) for `target` into `dir/<tag>.out`: the one file it wrote with
// `extension`, or empty with `rep` holding why. Several sources are several units (`compileUnits`); one is
// `compileFiles`'s. `libraries` are the static libraries the link searches.
[[nodiscard]] fs::path dssCompile(fs::path const& dir, std::string_view tag, std::vector<std::string> const& inputs,
                                  std::size_t sources, char const* target, std::string_view extension,
                                  DiagnosticReporter& rep, std::vector<fs::path> const& libraries = {}) {
    auto const out = dir / (std::string{tag} + ".out");
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    if (!libraries.empty()) p.setResolveLibraries(libraries);
    int const rc = sources > 1u ? p.compileUnits(inputs, "c", std::vector<std::string>{target}, rep)
                                : p.compileFiles(inputs, "c", std::vector<std::string>{target}, rep);
    if (rc != 0) return {};
    std::vector<fs::path> written;
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file() && e.path().extension() == extension) written.push_back(e.path());
    }
    if (written.size() != 1u) {
        ADD_FAILURE() << tag << ": the compile wrote " << written.size() << " `" << extension << "` files, not one";
        return {};
    }
    return written.front();
}

// The image of one cell, made by DSS on the cell's route: its path, or empty with `rep` holding why.
//   * Unit: the readers are inputs of the image's own compile -- the reader unit a second unit, cl's object an
//     object input.
//   * Object: every DSS source is compiled `-c` first; the image is the link of the objects alone.
//   * Library: the readers are packaged into a DSS static library first; the image's unit is linked against it.
[[nodiscard]] fs::path imageOf(fs::path const& dir, Cell const& c, DiagnosticReporter& rep,
                               char const* clObject = kSwitchO2) {
    std::string const tag     = nameOf(c);
    char const* const  target  = c.image == Image::Exe ? kExec : kDll;
    std::string const  ext     = c.image == Image::Exe ? ".exe" : ".dll";
    auto const         driver  = writeUnit(dir / tag, "driver", driverOf(c));
    auto const         reader  = writeUnit(dir / tag, "reader", kReaderUnit);
    std::string const  cl      = fixturePath(clObject).string();
    switch (c.route) {
        case Route::Unit: {
            std::vector<std::string> in{driver.string()};
            if (readsDss(c.reader)) in.push_back(reader.string());
            std::size_t const sources = in.size();
            if (readsCl(c.reader)) in.push_back(cl);
            return dssCompile(dir / tag, "image", in, sources, target, ext, rep);
        }
        case Route::Object: {
            auto const driverObj = dssCompile(dir / tag, "driver_c", {driver.string()}, 1, kObject, ".obj", rep);
            if (driverObj.empty()) return {};
            std::vector<std::string> in{driverObj.string()};
            if (readsDss(c.reader)) {
                auto const readerObj = dssCompile(dir / tag, "reader_c", {reader.string()}, 1, kObject, ".obj", rep);
                if (readerObj.empty()) return {};
                in.push_back(readerObj.string());
            }
            if (readsCl(c.reader)) in.push_back(cl);
            return dssCompile(dir / tag, "image", in, 0, target, ext, rep);
        }
        case Route::Library: {
            std::vector<std::string> members;
            if (readsDss(c.reader)) members.push_back(reader.string());
            std::size_t const sources = members.size();
            if (readsCl(c.reader)) members.push_back(cl);
            auto const lib = dssCompile(dir / tag, "library", members, sources, kArchive, ".lib", rep);
            if (lib.empty()) return {};
            return dssCompile(dir / tag, "image", {driver.string()}, 1, target, ext, rep, {lib});
        }
    }
    return {};
}

void expectNotImported(fs::path const& image, char const* name) {
    auto const imported = test_support::peImportedSymbols(bytesOf(image));
    EXPECT_EQ(std::find(imported.begin(), imported.end(), name), imported.end())
        << name << " is imported; the image defines it";
}

[[nodiscard]] bool onWindowsX64() { return test_support::hostNativeTarget().execTarget == kExec; }

[[nodiscard]] nlohmann::json shippedDocument(char const* name) {
    std::ifstream in{dss::test::configRoot() / "object-formats" / (std::string{name} + ".format.json")};
    return nlohmann::json::parse(in);
}

constexpr char const* kPeDocuments[] = {"pe64-x86_64-windows-exec", "pe64-x86_64-windows-dll", "pe64-x86_64-windows",
                                        "pe64-x86_64-windows-staticlib"};

}  // namespace

// ══ THE DOCUMENTS ════════════════════════════════════════════════════════════════════════════════════════════════

// Every pe64 document states `__ImageBase` at the header of the image it ends in, and the EXE and DLL documents
// declare the ADDR32NB row the relocatable document does. RED LEVER: drop the key from the relocatable document ->
// its row reads none here, and `EveryCellLinks...`'s `-c` cells read the name through a slot.
TEST(PeImageDefinedSymbols, EveryPe64DocumentStatesTheImageBase) {
    for (char const* name : kPeDocuments) {
        SCOPED_TRACE(name);
        auto const f = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(f.has_value());
        auto const stated = (*f)->imageDefinedSymbols();
        ASSERT_EQ(stated.size(), 1u);
        EXPECT_EQ(stated[0].name, "__ImageBase");
        EXPECT_EQ(stated[0].address, ImageDefinedAddress::ImageHeader);
        EXPECT_EQ((*f)->imageDefinedNames(), std::vector<std::string>{"__ImageBase"});
    }
    for (char const* name : {"pe64-x86_64-windows-exec", "pe64-x86_64-windows-dll"}) {
        SCOPED_TRACE(name);
        auto const f = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(f.has_value());
        auto const rows = (*f)->relocations();
        EXPECT_TRUE(std::any_of(rows.begin(), rows.end(), [](ObjectFormatRelocationInfo const& r) {
            return r.name == "IMAGE_REL_AMD64_ADDR32NB" && r.kind == RelocationKind{6} && r.nativeId == 3u;
        })) << "the image documents read and write the image-relative field cl's tables hold";
    }
}

// The loader refuses every malformed row by the row's own path, on an image document and on a relocatable one alike.
TEST(PeImageDefinedSymbols, TheLoaderRefusesEveryMalformedRow) {
    struct Bad {
        char const*    what;
        nlohmann::json rows;
        char const*    path;
    };
    std::vector<Bad> const bad{
        {"a row with no name", nlohmann::json::array({{{"address", "imageHeader"}}}), "/imageDefinedSymbols/0/name"},
        {"an empty name", nlohmann::json::array({{{"name", ""}, {"address", "imageHeader"}}}),
         "/imageDefinedSymbols/0/name"},
        {"an address the vocabulary does not hold", nlohmann::json::array({{{"name", "__ImageBase"}, {"address", "base"}}}),
         "/imageDefinedSymbols/0/address"},
        {"a name stated twice",
         nlohmann::json::array({{{"name", "__ImageBase"}, {"address", "imageHeader"}},
                                {{"name", "__ImageBase"}, {"address", "imageHeader"}}}),
         "/imageDefinedSymbols/1/name"},
    };
    for (char const* name : {"pe64-x86_64-windows-exec", "pe64-x86_64-windows"}) {
        for (auto const& b : bad) {
            SCOPED_TRACE(std::string{name} + ": " + b.what);
            auto doc = shippedDocument(name);
            doc["imageDefinedSymbols"] = b.rows;
            auto const r = ObjectFormatSchema::loadFromText(doc.dump(), std::string{name} + ", changed");
            EXPECT_EQ(reject::countAtPath(r, b.path), 1u) << reject::rejectSummary(r);
        }
    }
}

// ══ THE LINK, ON EVERY LEG ═══════════════════════════════════════════════════════════════════════════════════════

// Every cell of the universe links, with `__ImageBase` defined in the image, never imported. RED-ON-DISABLE: resolve
// no stated name to the image -> each cell is refused, "undefined symbol '__ImageBase'"; read no stated name direct
// in a compile -> its reference takes an import slot, which the image link refuses.
TEST(PeImageDefinedSymbols, EveryCellLinksWithTheImageBaseDefinedInTheImage) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-image-base-cells"};
    for (Cell const& c : theUniverse()) {
        SCOPED_TRACE(nameOf(c));
        DiagnosticReporter rep;
        auto const image = imageOf(scratch.path(), c, rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        expectNotImported(image, "__ImageBase");
    }
}

// cl's other switch object (/Od) beside a C `main`, and a unit that defines the name, which links with its own.
TEST(PeImageDefinedSymbols, TheSecondSwitchObjectAndAUnitThatDefinesTheNameLink) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-image-base-units"};
    {
        DiagnosticReporter rep;
        auto const image = imageOf(scratch.path() / "od", Cell{Image::Exe, Route::Unit, Reader::Cl}, rep, kSwitchOd);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        expectNotImported(image, "__ImageBase");
    }
    DiagnosticReporter rep;
    auto const unit  = writeUnit(scratch.path() / "defined", "defined", kDefined);
    auto const image = dssCompile(scratch.path() / "defined", "image", {unit.string()}, 1, kExec, ".exe", rep);
    ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
}

// ══ THE PROGRAMS, ON WINDOWS x86_64 ══════════════════════════════════════════════════════════════════════════════

// Every cell RUNS to the image's header address, as do the second switch object and the defining unit (its own 1);
// link.exe's and lld-link's programs of the switch objects, beside a cl `main`, are read beside DSS's.
TEST(PeImageDefinedSymbolsNative, EveryCellRunsToTheImagesHeaderAddress) {
    if (!onWindowsX64()) GTEST_SKIP() << "the programs run only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-image-base-native"};
    auto const dir = scratch.path();
    auto const run42 = [](fs::path const& image, std::vector<std::string> const& args, char const* what) {
        auto const r = test_support::runBinary(image, test_support::kRunBudget, false, {}, args);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << what;
    };
    DiagnosticReporter lrep;
    auto const loaderUnit = writeUnit(dir / "loader", "loader", kDllLoader);
    auto const loader     = dssCompile(dir / "loader", "image", {loaderUnit.string()}, 1, kExec, ".exe", lrep);
    ASSERT_FALSE(loader.empty()) << diagnosticsOf(lrep);
    for (Cell const& c : theUniverse()) {
        SCOPED_TRACE(nameOf(c));
        DiagnosticReporter rep;
        auto const image = imageOf(dir, c, rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        run42(c.image == Image::Exe ? image : loader, argumentsOf(c, image), kCellExit);
    }
    {
        DiagnosticReporter rep;
        auto const image = imageOf(dir / "od", Cell{Image::Exe, Route::Unit, Reader::Cl}, rep, kSwitchOd);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        run42(image, {}, kCellExit);
    }
    {
        DiagnosticReporter rep;
        auto const unit  = writeUnit(dir / "defined", "defined", kDefined);
        auto const image = dssCompile(dir / "defined", "image", {unit.string()}, 1, kExec, ".exe", rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        run42(image, {}, "42 is the unit's own definition, read where the image's would have been");
    }
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << "DSS's programs were read; link.exe's were NOT: " << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    std::ofstream(dir / "refmain.c", std::ios::binary) << driverOf(Cell{Image::Exe, Route::Unit, Reader::Cl});
    ASSERT_TRUE(tools.run("cl /nologo /c /O2 /GS- /MD refmain.c /Forefmain.obj")) << "cl compiles the C main";
    auto const lld = test_support::native_probe::locateLldLink(dir);
    ASSERT_TRUE(lld.ok() || lld.toolAbsent()) << lld.describe();
    std::vector<std::pair<std::string, char const*>> linkers{{"link", "link.exe"}};
    if (lld.ok()) linkers.emplace_back("\"" + lld.lldLink.string() + "\"", "lld-link");
    std::size_t k = 0;
    for (auto const* object : {kSwitchO2, kSwitchOd}) {
        auto const bytes = bytesOf(fixturePath(object));
        ASSERT_FALSE(bytes.empty());
        std::ofstream(dir / object, std::ios::binary)
            .write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        for (auto const& [linker, who] : linkers) {
            SCOPED_TRACE(std::string{who} + ", " + object);
            std::string const exe = "ref" + std::to_string(k++) + ".exe";
            ASSERT_TRUE(tools.run(linker + " /nologo /OUT:" + exe + " refmain.obj " + object + " kernel32.lib"))
                << who << " links the switch object beside the cl `main`";
            run42(dir / exe, {}, kCellExit);
        }
    }
    if (linkers.size() == 1u) {
        GTEST_SKIP() << "DSS's and link.exe's programs were read; lld-link's were NOT: " << lld.detail;
    }
}
