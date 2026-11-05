// P69 fold 3d (lane xa) -- the ELF and Mach-O neighbours of D-LK-PE-IMAGE-DEFINES-NO-IMAGE-BASE-SYMBOL: the names
// every reference linker of a format defines at the image's own header, for a reference no linked unit defines, are
// defined by DSS's link as the image's document states them (`imageDefinedSymbols`, `imageHeader`). PE's
// `__ImageBase` is `test_pe_image_defined_symbols.cpp`'s; this file holds the other two formats.
//
// ✔MEASURED P69 fold 3d (probe-reference-cc):
//   * ELF (runs 20261104-215456-eb7e3a93, -220426-26508501, -220940-b51e718b on linux-x86_64-debug and
//     20261104-220759-3f7d530f on linux-arm64-debug; gcc 13.3 and clang 18.1 through GNU ld, clang through ld.lld):
//     `__ehdr_start` and `__executable_start` read the image's ELF header in -no-pie, -pie and -static images on both
//     ISAs. In a `-shared -z defs` object `__ehdr_start` links under GNU ld and ld.lld, while `__executable_start`
//     links under ld.lld only -- one working reference makes it required. A unit that defines `__ehdr_start` keeps
//     its own definition.
//   * Mach-O (run 20261104-220054-21df78f8, Apple clang 21 + ld64, -arch arm64 and x86_64): an executable reads
//     MH_MAGIC_64 through `__mh_execute_header` and `___dso_handle`, and a dylib links naming `__mh_dylib_header` or
//     `___dso_handle`. Each CROSS name is undefined: `__mh_dylib_header` from an executable, `__mh_execute_header` from
//     a dylib. (C spells them with one leading underscore fewer: `_mh_execute_header`, `__dso_handle`.)
// Until this fold DSS refused every such program `K_SymbolUndefined`.
//
// THE UNIVERSE (ledger 493): {ELF x86_64, ELF aarch64} x {exec, pie, `.so`} x {`__ehdr_start`, `__executable_start`}
// and {Mach-O arm64, Mach-O x86_64} x {exec, dylib} x {the flavor's own name, `___dso_handle`, the cross name}, each x
// {the reader a unit of the image's own compile, a `-c` object linked later, a static library's member linked
// later}. On EVERY leg each cell links with the name defined in the image -- its bytes name no import of it -- or, for
// a cross name, is refused by name. Where the host runs the image, each RUNS: the reader finds the image's own magic at
// the name, at or below the image's own data, and (for a library) not the loading program's header.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "arm_verdict_ledger.hpp"
#include "host_translations.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

enum class Family : std::uint8_t { Elf, MachO };
enum class Route : std::uint8_t { Unit, Object, Library };

// One image document of the universe, with the relocatable and archive documents its `-c` and library routes compile
// for, and the artifact suffixes DSS writes for each.
struct ImageDoc {
    Family      family;
    char const* arch;      // the CLI's `<arch>` -- also the host-arch token the run gate compares
    char const* image;     // the image document
    char const* object;    // its relocatable sibling
    char const* archive;   // its static-library sibling
    char const* exec;      // the executable document a library's loader is built for
    bool        library;   // a shared object / dylib, loaded by a DSS program rather than run
    char const* suffix;    // the image's suffix ("" for an executable)
};

constexpr ImageDoc kDocs[] = {
    {Family::Elf, "x86_64", "elf64-x86_64-linux-exec", "elf64-x86_64-linux", "elf64-x86_64-linux-staticlib",
     "elf64-x86_64-linux-exec", false, ""},
    {Family::Elf, "x86_64", "elf64-x86_64-linux-pie", "elf64-x86_64-linux", "elf64-x86_64-linux-staticlib",
     "elf64-x86_64-linux-exec", false, ""},
    {Family::Elf, "x86_64", "elf64-x86_64-linux-dyn", "elf64-x86_64-linux", "elf64-x86_64-linux-staticlib",
     "elf64-x86_64-linux-exec", true, ".so"},
    {Family::Elf, "arm64", "elf64-aarch64-linux-exec", "elf64-aarch64-linux", "elf64-aarch64-linux-staticlib",
     "elf64-aarch64-linux-exec", false, ""},
    {Family::Elf, "arm64", "elf64-aarch64-linux-pie", "elf64-aarch64-linux", "elf64-aarch64-linux-staticlib",
     "elf64-aarch64-linux-exec", false, ""},
    {Family::Elf, "arm64", "elf64-aarch64-linux-dyn", "elf64-aarch64-linux", "elf64-aarch64-linux-staticlib",
     "elf64-aarch64-linux-exec", true, ".so"},
    {Family::MachO, "arm64", "macho64-arm64-darwin-exec", "macho64-arm64-darwin", "macho64-arm64-darwin-staticlib",
     "macho64-arm64-darwin-exec", false, ""},
    {Family::MachO, "arm64", "macho64-arm64-darwin-dylib", "macho64-arm64-darwin", "macho64-arm64-darwin-staticlib",
     "macho64-arm64-darwin-exec", true, ".dylib"},
    {Family::MachO, "x86_64", "macho64-x86_64-darwin-exec", "macho64-x86_64-darwin",
     "macho64-x86_64-darwin-staticlib", "macho64-x86_64-darwin-exec", false, ""},
    {Family::MachO, "x86_64", "macho64-x86_64-darwin-dylib", "macho64-x86_64-darwin",
     "macho64-x86_64-darwin-staticlib", "macho64-x86_64-darwin-exec", true, ".dylib"},
};

// A name a cell reads: its C spelling, its link-level (mangled) one, and whether the image defines it.
struct HeaderName {
    char const* c;
    char const* linkName;
    bool        defined;
};

[[nodiscard]] std::vector<HeaderName> namesOf(ImageDoc const& d) {
    if (d.family == Family::Elf) {
        return {{"__ehdr_start", "__ehdr_start", true}, {"__executable_start", "__executable_start", true}};
    }
    if (d.library) {
        return {{"_mh_dylib_header", "__mh_dylib_header", true}, {"__dso_handle", "___dso_handle", true},
                {"_mh_execute_header", "__mh_execute_header", false}};
    }
    return {{"_mh_execute_header", "__mh_execute_header", true}, {"__dso_handle", "___dso_handle", true},
            {"_mh_dylib_header", "__mh_dylib_header", false}};
}

// The name a library's LOADER reads for its own header (an executable's own row).
[[nodiscard]] char const* loaderNameOf(ImageDoc const& d) {
    return d.family == Family::Elf ? "__ehdr_start" : "_mh_execute_header";
}

// What each document states: the image documents their own names, the relocatable and archive ones the union.
[[nodiscard]] std::vector<std::string> statedBy(char const* doc) {
    std::string_view const s{doc};
    if (s.starts_with("elf64-")) return {"__ehdr_start", "__executable_start"};
    if (s.ends_with("-exec")) return {"__mh_execute_header", "___dso_handle"};
    if (s.ends_with("-dylib")) return {"__mh_dylib_header", "___dso_handle"};
    return {"__mh_execute_header", "__mh_dylib_header", "___dso_handle"};
}

constexpr Route kRoutes[] = {Route::Unit, Route::Object, Route::Library};

[[nodiscard]] char const* routeName(Route r) {
    return r == Route::Unit ? "unit" : r == Route::Object ? "object" : "library";
}

// The reader unit: the address of `name`, and whether its first four bytes are the format's magic.
[[nodiscard]] std::string readerOf(ImageDoc const& d, HeaderName const& n) {
    std::string const magic = d.family == Family::Elf
                                  ? "h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F'"
                                  : "h[0] == 0xCF && h[1] == 0xFA && h[2] == 0xED && h[3] == 0xFE";
    return std::string{"extern unsigned char const "} + n.c + "[];\n"
         + "unsigned char const *header_of(void) { return " + n.c + "; }\n"
         + "int header_reads(void) { unsigned char const *h = " + n.c + "; return " + magic + "; }\n";
}

// The image's own unit. Both kinds check the reader's header against an object of the image's OWN data: the header is
// the image's first byte, so it sits at or below that object, within the image's span. A library's check also refuses
// the loading program's own header (8), which a reader resolved to the wrong image would find.
constexpr char const* kChecks =
    "    unsigned char const *h = header_of();\n"
    "    unsigned long const at = (unsigned long)h, mine = (unsigned long)&image_anchor;\n"
    "    if (!header_reads()) return 6;\n"
    "    if (at > mine || mine - at > 0x4000000UL) return 5;\n";

[[nodiscard]] std::string driverOf(ImageDoc const& d) {
    std::string s = "unsigned char const *header_of(void);\nint header_reads(void);\nint image_anchor = 1;\n";
    if (!d.library) return s + "int main(void) {\n" + kChecks + "    return 42;\n}\n";
    return s + "int lib_check(unsigned char const *loader_header) {\n" + kChecks
         + "    if (header_of() == loader_header) return 8;\n    return 42;\n}\n";
}

[[nodiscard]] std::string loaderOf(ImageDoc const& d) {
    return std::string{"#include <dlfcn.h>\n"} + "extern unsigned char const " + loaderNameOf(d) + "[];\n"
         + "typedef int (*check_fn)(unsigned char const *);\n"
           "int main(int argc, char **argv) {\n"
           "    if (argc < 2) return 2;\n"
           "    void *lib = dlopen(argv[1], RTLD_NOW);\n"
           "    if (lib == 0) return 3;\n"
           "    check_fn const check = (check_fn)dlsym(lib, \"lib_check\");\n"
           "    if (check == 0) return 4;\n"
           "    return check("
         + loaderNameOf(d) + ");\n}\n";
}

constexpr char const* kCellExit =
    "42 is the reader at the image's own header; 6 the bytes at the name that are not the format's magic, 5 an "
    "address not at or below the image's own data, 8 a library's reader at its loader's header; 2-4 the loader's "
    "own failures";

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& x : rep.all()) s += "\n  " + std::string{diagnosticCodeName(x.code)} + ": " + x.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

[[nodiscard]] fs::path writeUnit(fs::path const& dir, std::string_view stem, std::string_view text) {
    fs::create_directories(dir);
    auto const p = dir / (std::string{stem} + ".c");
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

[[nodiscard]] std::vector<std::uint8_t> bytesOf(fs::path const& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// One DSS compile of `inputs` for `<arch>:<doc>` into `dir/<tag>.out`: the one regular file it wrote with `suffix`
// ("" an executable, which has none), or empty with `rep` holding why.
[[nodiscard]] fs::path dssCompile(fs::path const& dir, std::string_view tag, std::vector<std::string> const& inputs,
                                  std::size_t sources, ImageDoc const& d, char const* doc, std::string_view suffix,
                                  DiagnosticReporter& rep, std::vector<fs::path> const& libraries = {}) {
    auto const out = dir / (std::string{tag} + ".out");
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    if (!libraries.empty()) p.setResolveLibraries(libraries);
    std::vector<std::string> const target{std::string{d.arch} + ":" + doc};
    int const rc = sources > 1u ? p.compileUnits(inputs, "c", target, rep) : p.compileFiles(inputs, "c", target, rep);
    if (rc != 0) return {};
    std::vector<fs::path> written;
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file() && e.path().extension() == suffix) written.push_back(e.path());
    }
    if (written.size() != 1u) {
        ADD_FAILURE() << tag << ": the compile wrote " << written.size() << " `" << suffix << "` files, not one";
        return {};
    }
    return written.front();
}

[[nodiscard]] std::string cellName(ImageDoc const& d, HeaderName const& n, Route r) {
    return std::string{d.image} + " / " + n.linkName + " / " + routeName(r);
}

// The image of one cell, made by DSS on the cell's route: its path, or empty with `rep` holding why.
//   * Unit: the reader is a second unit of the image's own compile.
//   * Object: both units are compiled `-c` for the relocatable document first; the image is the link of the objects.
//   * Library: the reader is packaged into a static library first; the image's unit is linked against it.
[[nodiscard]] fs::path imageOf(fs::path const& dir, ImageDoc const& d, HeaderName const& n, Route r,
                               DiagnosticReporter& rep) {
    auto const driver = writeUnit(dir, "driver", driverOf(d));
    auto const reader = writeUnit(dir, "reader", readerOf(d, n));
    switch (r) {
        case Route::Unit:
            return dssCompile(dir, "image", {driver.string(), reader.string()}, 2, d, d.image, d.suffix, rep);
        case Route::Object: {
            auto const dObj = dssCompile(dir, "driver_c", {driver.string()}, 1, d, d.object, ".o", rep);
            if (dObj.empty()) return {};
            auto const rObj = dssCompile(dir, "reader_c", {reader.string()}, 1, d, d.object, ".o", rep);
            if (rObj.empty()) return {};
            return dssCompile(dir, "image", {dObj.string(), rObj.string()}, 0, d, d.image, d.suffix, rep);
        }
        case Route::Library: {
            auto const lib = dssCompile(dir, "library", {reader.string()}, 1, d, d.archive, ".a", rep);
            if (lib.empty()) return {};
            return dssCompile(dir, "image", {driver.string()}, 1, d, d.image, d.suffix, rep, {lib});
        }
    }
    return {};
}

[[nodiscard]] std::size_t occurrences(std::vector<std::uint8_t> const& hay, std::string_view needle) {
    std::size_t n = 0;
    for (auto it = hay.begin();
         (it = std::search(it, hay.end(), needle.begin(), needle.end())) != hay.end(); ++it) {
        ++n;
    }
    return n;
}

[[nodiscard]] bool refusedNaming(DiagnosticReporter const& rep, std::string_view name) {
    std::string const quoted = "'" + std::string{name} + "'";
    return std::any_of(rep.all().begin(), rep.all().end(), [&](auto const& x) {
        return x.code == DiagnosticCode::K_SymbolUndefined && x.actual.find(quoted) != std::string::npos;
    });
}

// Whether this host runs a document's images, and under which launcher: the family's own OS, and the arch natively
// or through the host's declared translation (Rosetta 2 runs x86_64 Mach-O on Apple Silicon).
struct RunGate {
    bool                     runs = false;
    std::vector<std::string> launcher;
    bool                     launcherExecsImage = false;
    std::string              why;
};

[[nodiscard]] RunGate runGateFor(ImageDoc const& d) {
    RunGate g;
    std::string const os = test_support::currentHostOs();
    if (os != (d.family == Family::Elf ? "linux" : "darwin")) {
        g.why = "the host is " + os;
        return g;
    }
    if (test_support::currentHostArch() == d.arch) {
        g.runs = true;
        return g;
    }
    auto const x = test_support::crossArchDecisionForThisHost(d.arch, "");
    g.runs = x.runs;
    g.launcher = x.launcherPrefix;
    g.launcherExecsImage = x.launcherExecsImage;
    g.why = x.why;
    return g;
}

}  // namespace

// ══ THE DOCUMENTS ════════════════════════════════════════════════════════════════════════════════════════════════

// Every ELF and Mach-O document states the names, each at the image's header: an image document its flavor's own,
// a relocatable or archive document every name the image its objects end in may define. RED LEVER: drop a row from a
// relocatable document -> it reads one name fewer here, and that name's `-c` cells read it through a slot.
TEST(ImageHeaderSymbols, EveryElfAndMachODocumentStatesItsHeaderNames) {
    std::vector<std::string> docs;
    for (auto const& d : kDocs) {
        for (char const* doc : {d.image, d.object, d.archive}) {
            if (std::find(docs.begin(), docs.end(), doc) == docs.end()) docs.emplace_back(doc);
        }
    }
    ASSERT_EQ(docs.size(), 18u) << "ELF {x86_64, aarch64} x {exec, pie, dyn, relocatable, staticlib}, Mach-O "
                                   "{arm64, x86_64} x {exec, dylib, relocatable, staticlib}";
    for (auto const& name : docs) {
        SCOPED_TRACE(name);
        auto const f = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(f.has_value());
        std::vector<std::string> stated;
        for (auto const& s : (*f)->imageDefinedSymbols()) {
            EXPECT_EQ(s.address, ImageDefinedAddress::ImageHeader) << s.name;
            stated.push_back(s.name);
        }
        EXPECT_EQ(stated, statedBy(name.c_str()));
        EXPECT_EQ((*f)->imageDefinedNames(), stated);
    }
}

// ══ THE LINK, ON EVERY LEG ═══════════════════════════════════════════════════════════════════════════════════════

// Every cell links with its name defined in the image, whose bytes then name no import of it; every cross cell is
// refused by its name. RED-ON-DISABLE: resolve no stated name to the image -> each defined cell is refused,
// "undefined symbol '<name>'"; read no stated name direct in a compile -> its reference takes a slot; give the
// relocatable documents no row -> the `-c` and library cells' objects import the name.
TEST(ImageHeaderSymbols, EveryCellLinksWithItsNameDefinedOrTheCrossNameRefused) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "image-header-cells"};
    std::size_t k = 0;
    for (auto const& d : kDocs) {
        for (auto const& n : namesOf(d)) {
            for (Route const r : kRoutes) {
                SCOPED_TRACE(cellName(d, n, r));
                DiagnosticReporter rep;
                auto const image = imageOf(scratch.path() / std::to_string(k++), d, n, r, rep);
                if (!n.defined) {
                    EXPECT_TRUE(image.empty()) << "the image does not define the cross name";
                    EXPECT_TRUE(refusedNaming(rep, n.linkName)) << diagnosticsOf(rep);
                    continue;
                }
                ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
                EXPECT_EQ(occurrences(bytesOf(image), n.linkName), 0u)
                    << "the image names " << n.linkName << ": an import of it, where the image defines it";
            }
        }
    }
}

// ══ THE PROGRAMS, WHERE THE HOST RUNS THEM ═══════════════════════════════════════════════════════════════════════

// Every defined cell whose image this host runs RUNS to the image's own header: an executable alone, a library
// through a DSS program that loads it and passes its own header. Linux reads the ELF cells of its ISA; macOS the
// Mach-O cells, x86_64 through its translation.
TEST(ImageHeaderSymbolsNative, EveryCellRunsToItsImagesOwnHeader) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "image-header-native"};
    std::size_t ran = 0, k = 0;
    std::string notRun;
    for (auto const& d : kDocs) {
        auto const gate = runGateFor(d);
        if (!gate.runs) {
            notRun += std::string{"\n  "} + d.image + ": " + gate.why;
            continue;
        }
        auto const run42 = [&](fs::path const& image, std::vector<std::string> const& args) {
            auto const r = test_support::runBinary(image, test_support::kRunBudget, false, gate.launcher, args,
                                                   gate.launcherExecsImage);
            ASSERT_TRUE(r.spawned) << r.diagnostic;
            EXPECT_FALSE(r.timedOut);
            EXPECT_EQ(r.exitCode, 42u) << kCellExit;
        };
        fs::path loader;
        if (d.library) {
            DiagnosticReporter lrep;
            auto const dir = scratch.path() / ("loader" + std::to_string(k++));
            auto const unit = writeUnit(dir, "loader", loaderOf(d));
            loader = dssCompile(dir, "image", {unit.string()}, 1, d, d.exec, "", lrep);
            ASSERT_FALSE(loader.empty()) << d.image << ": the loader" << diagnosticsOf(lrep);
        }
        for (auto const& n : namesOf(d)) {
            if (!n.defined) continue;
            for (Route const r : kRoutes) {
                SCOPED_TRACE(cellName(d, n, r));
                DiagnosticReporter rep;
                auto const image = imageOf(scratch.path() / std::to_string(k++), d, n, r, rep);
                ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
                if (d.library) run42(loader, {fs::absolute(image).string()});
                else run42(image, {});
                ++ran;
            }
        }
    }
    if (ran == 0u) GTEST_SKIP() << "this host runs none of the images:" << notRun;
    if (!notRun.empty()) std::cout << "[ image-header ] the cells of these documents were linked, NOT run:" << notRun
                                   << "\n";
}
