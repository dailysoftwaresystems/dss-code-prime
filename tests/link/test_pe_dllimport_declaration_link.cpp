// P69 (lane `cs`, fold f26b) — A `__declspec(dllimport)` DECLARATION WITH NO `extern` NAMES ANOTHER IMAGE'S OBJECT,
// AND THE PROGRAM READS THAT OBJECT.
//
// `__declspec(dllimport) int v;` at file scope, and the same declaration inside a body, are REFERENCES on both
// references that have the spelling: cl 19.51 and mingw-w64 gcc 13.2.0 write `__imp_v` UNDEFINED and no `v` at all
// (✔MEASURED, lane `cs` probes di cell 7 and di2 cell 6, 2026-10-20/21, windows-x86_64). Before fold f26b DSS made the
// file-scope one a tentative DEFINITION (a common `v` of its own) and the block one an automatic LOCAL read
// uninitialised — two silent wrong programs against a DLL that exports `v`: the image read its own zero, or garbage.
//
// One DLL exports `dss_imported_value = 40` (DSS's own PE DLL writer exports every external definition). Each
// importing unit returns the value plus 2, so the image exits 42 iff its read reached the DLL's object:
//   * DSS builds the image, resolving the import against the DLL (`--resolve-library`), at the baseline and at
//     `--config=release`;
//   * mingw-w64 gcc compiles the SAME unit and links it against the DLL itself;
//   * cl compiles the SAME unit and links it against an import library `lib` makes from a `.def` naming the datum.
// Windows-only; each reference arm SKIPS, naming the absent toolchain, rather than reddens. The DLL's self-name
// (`TheDllDssBuildsIsNamedInItsExportDirectory`) is read from the image's bytes on every host.
//
// The tier pins are `AttributeSecondSpelling.ADllimportDeclarationNamesAnotherImagesDefinition` (the records) and
// `HirLoweringC.ADllimportDeclarationLowersToAnImportNotToStorage` (the import, at either scope).

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/image_request.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/cli_args.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

// The DLL also defines a function: a DLL of data ALONE is refused by DSS's PE writer today ("`.text` is empty"),
// a defect of its own that this pin does not ask about —
// [[D-LK-A-DATA-ONLY-SHARED-LIBRARY-IS-REFUSED-AS-AN-EXEC-WITH-NO-INSTRUCTIONS]], whose fold removes this function.
constexpr char const* kLibraryUnit = "int dss_imported_value = 40;\n"
                                     "int dss_library_value(void) { return dss_imported_value; }\n";

struct ImportingUnit {
    char const* stem;
    char const* text;
    char const* what;
};

constexpr ImportingUnit kImportingUnits[] = {
    {"file_scope",
     "__declspec(dllimport) int dss_imported_value;\n"
     "int main(void) { return dss_imported_value + 2; }\n",
     "a file-scope dllimport declaration with no initializer"},
    {"block_scope",
     "int main(void) { __declspec(dllimport) int dss_imported_value; return dss_imported_value + 2; }\n",
     "a block-scope dllimport declaration with no `extern`"},
};

constexpr CompileConfig kConfigs[] = {CompileConfig::Debug, CompileConfig::Release};

[[nodiscard]] char const* configName(CompileConfig c) { return c == CompileConfig::Release ? "release" : "baseline"; }

void writeText(fs::path const& p, std::string_view text) { std::ofstream(p, std::ios::binary) << text; }

[[nodiscard]] fs::path onlyImageIn(fs::path const& dir) {
    for (auto const& e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".exe") return e.path();
    }
    return {};
}

// DSS builds `<dir>/dsslib.dll`, which exports `dss_imported_value`. False on a failure; every caller asserts.
[[nodiscard]] bool buildLibrary(fs::path const& dir, DiagnosticReporter& rep) {
    writeText(dir / "dsslib.c", kLibraryUnit);
    Program p;
    p.setOutputDir(dir);
    int const rc = p.compileFiles(std::vector<std::string>{(dir / "dsslib.c").string()}, "c",
                                  std::vector<std::string>{"x86_64:pe64-x86_64-windows-dll"}, rep);
    return rc == 0 && fs::exists(dir / "dsslib.dll");
}

// The string the export directory's Name field points at, read from the image's bytes (PE32+ only, as this writer
// makes it): the optional header's first data directory locates the directory, and the section table maps its RVAs.
// A refusal of the image's shape is a message in the result, so a pin says what it read rather than crashing.
[[nodiscard]] std::string exportDirectoryNameOf(std::string const& b) {
    auto u16 = [&](std::size_t o) -> std::uint32_t {
        return o + 2 <= b.size() ? static_cast<std::uint8_t>(b[o]) | (static_cast<std::uint8_t>(b[o + 1]) << 8) : 0u;
    };
    auto u32 = [&](std::size_t o) -> std::uint32_t { return u16(o) | (u16(o + 2) << 16); };
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') return "<not a PE image>";
    std::size_t const pe = u32(0x3c);
    if (u16(pe + 24) != 0x20b) return "<not PE32+>";
    std::uint32_t const exportRva = u32(pe + 24 + 112);
    if (exportRva == 0) return "<no export directory>";
    std::size_t const sections = pe + 24 + u16(pe + 20);
    auto offsetOf = [&](std::uint32_t rva) -> std::size_t {
        for (std::uint32_t i = 0; i < u16(pe + 6); ++i) {
            std::size_t const s = sections + 40u * i;
            std::uint32_t const va = u32(s + 12), span = std::max(u32(s + 8), u32(s + 16));
            if (rva >= va && rva < va + span) return u32(s + 20) + (rva - va);
        }
        return b.size();
    };
    std::size_t const nameAt = offsetOf(u32(offsetOf(exportRva) + 12));
    if (nameAt >= b.size()) return "<Name outside every section>";
    return std::string{b.c_str() + nameAt};
}
[[nodiscard]] std::string exportDirectoryName(fs::path const& image) {
    std::ifstream in(image, std::ios::binary);
    return exportDirectoryNameOf(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()});
}

void expectTheDllsValue(fs::path const& exe, char const* who, char const* what) {
    auto const r = test_support::runBinary(exe);
    ASSERT_TRUE(r.spawned) << who << ", " << what << ": " << r.diagnostic;
    EXPECT_FALSE(r.timedOut) << who << ", " << what;
    EXPECT_EQ(r.exitCode, 42u) << who << ", " << what
                               << ": the read did not reach the object the DLL exports (40 + 2)";
}

}  // namespace

// ★ P69 (lane `cs`, fold f26b): A DLL NAMES ITSELF IN ITS EXPORT DIRECTORY — the file name of the artifact, as GNU ld
// and link.exe write it (✔MEASURED, lane `cs` probe dn: mingw-w64 gcc `-shared -o dnx.dll` and `cl /LD` both write
// "dnx.dll", and an output written to `sub\dny_renamed.dll` is named "dny_renamed.dll"). GNU ld records that name in a
// program linked against the DLL, so the empty name this writer wrote before the fold made such a program fail to
// load (STATUS_DLL_NOT_FOUND — `MingwReadsTheObjectTheDllExports` below). Built on every host: only the image is read.
//
// RED-ON-DISABLE: write no bytes of the name and both DLLs read "" here.
TEST(PeDllimportDeclarationNative, TheDllDssBuildsIsNamedInItsExportDirectory) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-dll-export-name"};
    auto const dir = scratch.path();
    for (CompileConfig const config : kConfigs) {
        for (char const* stem : {"dsslib", "another_library"}) {
            SCOPED_TRACE(std::string{configName(config)} + ", " + stem);
            auto const out = dir / (std::string{stem} + "_" + configName(config));
            fs::create_directories(out);
            writeText(out / (std::string{stem} + ".c"), kLibraryUnit);
            Program p;
            p.setOutputDir(out);
            p.setCompileConfig(config);
            DiagnosticReporter rep;
            ASSERT_EQ(p.compileFiles(std::vector<std::string>{(out / (std::string{stem} + ".c")).string()}, "c",
                                     std::vector<std::string>{"x86_64:pe64-x86_64-windows-dll"}, rep),
                      0)
                << "DSS must build the DLL; errs=" << rep.errorCount();
            fs::path const dll = out / (std::string{stem} + ".dll");
            ASSERT_TRUE(fs::exists(dll)) << "DSS wrote no " << dll.filename().string();
            EXPECT_EQ(exportDirectoryName(dll), std::string{stem} + ".dll")
                << "the export directory must name the DLL by its own file name";
        }
    }
}

// ★ THE NAME HAS NO FALLBACK: an emission of an image with an export directory that is given no artifact file name
// (`ImageRequest::artifactFileName`) is REFUSED by name — an empty or a fixed Name makes every program GNU ld links
// against the image look for a DLL that is not there. Given one, the image states it. In-process, every host.
// RED-ON-DISABLE: let the writer go on with an empty name, and the first link is accepted, naming no DLL.
TEST(PeDllimportDeclarationNative, ADllEmissionGivenNoArtifactNameIsRefusedByName) {
    auto const target = TargetSchema::loadShipped("x86_64");
    auto const format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-dll");
    ASSERT_TRUE(target.has_value() && format.has_value());
    AssembledModule mod;
    mod.cuId              = CompilationUnitId{1};
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};   // mov eax, 42 ; ret
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_exported", SymbolBinding::Global, SymbolVisibility::Default});
    {
        DiagnosticReporter rep;
        LinkedImage const  img = linker::link(mod, **target, **format, rep);
        EXPECT_FALSE(img.ok()) << "a DLL was written that names no DLL";
        bool named = false;
        for (auto const& d : rep.all()) {
            named = named || (d.code == DiagnosticCode::K_WalkerInputContractViolation
                              && d.actual.find("artifactFileName") != std::string::npos);
        }
        EXPECT_TRUE(named) << "the refusal must name the missing artifact file name";
    }
    {
        DiagnosticReporter rep;
        LinkedImage const  img =
            linker::link(mod, **target, **format, rep, ImageRequest{.artifactFileName = "named_by_request.dll"});
        ASSERT_TRUE(img.ok()) << "errs=" << rep.errorCount();
        EXPECT_EQ(exportDirectoryNameOf(std::string{img.bytes.begin(), img.bytes.end()}), "named_by_request.dll");
    }
}

TEST(PeDllimportDeclarationNative, DssReadsTheObjectTheDllExports) {
#if !defined(_WIN32)
    GTEST_SKIP() << "not a Windows host -- the image this builds imports from a DLL and is run";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-dllimport-dss"};
    auto const dir = scratch.path();
    DiagnosticReporter libRep;
    ASSERT_TRUE(buildLibrary(dir, libRep)) << "DSS must build the DLL; errs=" << libRep.errorCount();
    for (CompileConfig const config : kConfigs) {
        for (ImportingUnit const& u : kImportingUnits) {
            SCOPED_TRACE(std::string{configName(config)} + ", " + u.what);
            auto const src = dir / (std::string{u.stem} + ".c");
            writeText(src, u.text);
            auto const out = dir / (std::string{u.stem} + "_" + configName(config));
            fs::create_directories(out);
            Program p;
            p.setOutputDir(out);
            p.setCompileConfig(config);
            p.setResolveLibraries({dir / "dsslib.dll"});
            DiagnosticReporter rep;
            ASSERT_EQ(p.compileFiles(std::vector<std::string>{src.string()}, "c",
                                     std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep),
                      0)
                << "DSS must build the importing image; errs=" << rep.errorCount();
            fs::path const exe = onlyImageIn(out);
            ASSERT_FALSE(exe.empty()) << "DSS wrote no image";
            fs::copy_file(dir / "dsslib.dll", out / "dsslib.dll", fs::copy_options::skip_existing);
            expectTheDllsValue(exe, "DSS", u.what);
        }
    }
#endif
}

TEST(PeDllimportDeclarationNative, MingwReadsTheObjectTheDllExports) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-dllimport-mingw"};
    auto const dir = scratch.path();
    auto const gcc = test_support::native_probe::locateMingwGcc(dir);
    if (gcc.toolAbsent()) GTEST_SKIP() << gcc.detail;
    ASSERT_TRUE(gcc.ok()) << gcc.describe();
    DiagnosticReporter libRep;
    ASSERT_TRUE(buildLibrary(dir, libRep)) << "DSS must build the DLL; errs=" << libRep.errorCount();
    for (ImportingUnit const& u : kImportingUnits) {
        SCOPED_TRACE(u.what);
        writeText(dir / (std::string{u.stem} + ".c"), u.text);
        std::string const exe = std::string{u.stem} + "_mingw.exe";
        std::string const cmd = "cd /d \"" + dir.string() + "\" && gcc -o " + exe + " " + u.stem
                              + ".c dsslib.dll >nul 2>&1";
        ASSERT_EQ(std::system(("\"" + cmd + "\"").c_str()), 0)
            << "mingw-w64 gcc must build the unit against the DLL: the declaration names `__imp_` of its datum";
        expectTheDllsValue(dir / exe, "mingw-w64 gcc", u.what);
    }
}

TEST(PeDllimportDeclarationNative, ClReadsTheObjectTheDllExports) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-dllimport-cl"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const env = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(env.ready()) << env.describe();
    DiagnosticReporter libRep;
    ASSERT_TRUE(buildLibrary(dir, libRep)) << "DSS must build the DLL; errs=" << libRep.errorCount();
    writeText(dir / "dsslib.def", "LIBRARY dsslib.dll\nEXPORTS\n    dss_imported_value DATA\n");
    ASSERT_TRUE(env.run("lib /nologo /machine:x64 /def:dsslib.def /out:dsslib.lib"))
        << "lib.exe must make the DLL's import library from its .def";
    for (ImportingUnit const& u : kImportingUnits) {
        SCOPED_TRACE(u.what);
        writeText(dir / (std::string{u.stem} + ".c"), u.text);
        std::string const exe = std::string{u.stem} + "_cl.exe";
        ASSERT_TRUE(env.run("cl /nologo /W3 " + std::string{u.stem} + ".c /Fe" + exe + " dsslib.lib"))
            << "cl must build the unit against the import library";
        expectTheDllsValue(dir / exe, "cl", u.what);
    }
}
