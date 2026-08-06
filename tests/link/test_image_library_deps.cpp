// Every resolved library must appear in the emitted image's dependency table
// — on ELF, PE, and Mach-O, from ONE shared input.
//
// D-LK-ELF-EMITS-ONE-DT-NEEDED-WHEN-TWO-LIBRARIES-ARE-REFERENCED.
//
// The defect this file exists for was NOT in a writer. `ImportSurface`'s
// contract is "a single symbol the dynamic library EXPORTS"
// (`ffi/import_surface.hpp`), and the PE reader honored it by reading
// `.edata` while the Mach-O reader honored it by walking the export trie /
// filtering `(N_EXT && N_TYPE == N_SECT)`. The ELF reader read every
// `.dynsym` row, and `.dynsym` carries a library's own REFERENCES
// (SHN_UNDEF) beside its definitions. So DSS believed libtcl8.6.so exported
// the 14 zlib names it merely imports; `ingest()`'s first-source-wins then
// bound `deflateBound` to libtcl8.6.so, libz.so.1 reached the linker in no
// `ExternImport.libraryPath` at all, and the ELF writer emitted one
// DT_NEEDED because it was handed one library. MEASURED: the emitted
// dependency set was a function of `--resolve-library` ARGUMENT ORDER.
//
// The fix is in `ffi/binary_readers/elf_reader.cpp` and is pinned at the
// reader (`tests/ffi/test_binary_reader.cpp`) and end-to-end through the
// real driver (`tests/program/test_ffi_resolve_library.cpp`).
//
// THIS file pins the LAST tier — the emitted bytes — and it pins all three
// writers off ONE `AssembledModule`, because the reason the bug survived is
// that nothing ever compared what the three formats did with the same
// resolved-library set. A writer that drops a library now reds here
// immediately, whichever writer it is.
//
// Assertions are on CONTENT (the library NAMES recovered from the image),
// never on a count: a two-entry dependency table naming the wrong library
// twice must fail.
//
// RED-ON-DISABLE: drop the second entry from a writer's emitted library set
// (e.g. `libraryOrder.resize(1)` in `elf.cpp`'s DT_NEEDED collection, or the
// equivalent in `pe.cpp` / `macho.cpp`) and exactly that format's leg reds
// while its siblings stay green.

#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include "image_dependency_table.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::test_support;

namespace {

// ── The shared fixture ───────────────────────────────────────────

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShippedPair(std::string const& targetName,
                                     std::string const& formatName) {
    Loaded out;
    auto t = TargetSchema::loadShipped(targetName);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(" << targetName << ") failed";
        return out;
    }
    out.target = std::move(t).value();
    auto f = ObjectFormatSchema::loadShipped(formatName);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(" << formatName << ") failed";
        return out;
    }
    out.format = std::move(f).value();
    return out;
}

// ONE module, TWO libraries. Both imports are EAGER, which is the shipped-
// descriptor shape and keeps the reference gate from dropping them — the
// point here is what the WRITER does with a two-library import set, not how
// the merge decides which imports survive. The body is a bare `ret` in the
// leg's own encoding, so nothing about this fixture is CPU-specific beyond
// those bytes.
[[nodiscard]] AssembledModule
makeTwoLibraryModule(std::string const& libA, std::string const& libB,
                     std::vector<std::uint8_t> retBytes) {
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = std::move(retBytes);
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_entry",
                                     SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};

    ExternImport a;
    a.symbol         = SymbolId{2};
    a.mangledName    = "dss_alpha_symbol";
    a.libraryPath    = libA;
    a.isEagerImport  = true;
    m.externImports.push_back(std::move(a));

    ExternImport bimp;
    bimp.symbol        = SymbolId{3};
    bimp.mangledName   = "dss_beta_symbol";
    bimp.libraryPath   = libB;
    bimp.isEagerImport = true;
    m.externImports.push_back(std::move(bimp));
    return m;
}

// Each leg names its format, the shipped (target, format) pair, the two
// library spellings that format actually uses, and the extractor that
// recovers that format's dependency table from the emitted bytes.
struct FormatLeg {
    char const* label;
    char const* target;
    char const* format;
    char const* libA;
    char const* libB;
    std::vector<std::uint8_t> ret;
    std::vector<std::string> (*deps)(std::vector<std::uint8_t> const&);
};

[[nodiscard]] std::vector<FormatLeg> allLegs() {
    std::vector<std::uint8_t> const x86Ret{0xC3};
    std::vector<std::uint8_t> const armRet{0xC0, 0x03, 0x5F, 0xD6};
    return {
        {"elf-exec",   "x86_64", "elf64-x86_64-linux-exec",
         "libalpha.so.1", "libbeta.so.1", x86Ret, &elfNeededLibraries},
        {"pe-exec",    "x86_64", "pe64-x86_64-windows-exec",
         "alpha.dll", "beta.dll", x86Ret, &peImportedLibraries},
        {"macho-exec", "arm64",  "macho64-arm64-darwin-exec",
         "/usr/lib/libalpha.dylib", "/usr/lib/libbeta.dylib",
         armRet, &machoLoadedDylibs},
    };
}

// ══ The shared pin ════════════════════════════════════════════════════════
//
// EVERY resolved library reaches the emitted dependency table, on EVERY
// shipped image format, from the same two-library import set.
//
// Each leg runs in its own subroutine so a leg that ABORTS (a fail-loud link,
// a `gtest` ASSERT_) costs only that leg's verdict. A loop body with ASSERT_
// in it returns from the whole TEST, which would silently stop testing every
// format after the first one to break -- measured while demonstrating
// red-on-disable: a PE-writer mutation aborted before the Mach-O leg ran at
// all, so a green Mach-O column would have meant "never executed".
void checkEveryLibraryRecorded(FormatLeg const& leg) {
    auto loaded = loadShippedPair(leg.target, leg.format);
    ASSERT_TRUE(loaded.target && loaded.format);

    auto const mod = makeTwoLibraryModule(leg.libA, leg.libB, leg.ret);
    DiagnosticReporter rep;
    auto const image =
        linker::link(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(rep.hasErrors())
        << "a two-library import set must link clean; first diagnostic: "
        << (rep.all().empty() ? "" : rep.all().front().actual);
    ASSERT_FALSE(image.bytes.empty());

    auto const recorded = leg.deps(image.bytes);
    // CONTENT, not count: both NAMES must be there. A dependency table
    // holding the right NUMBER of wrong entries fails here.
    EXPECT_EQ(dependencyOccurrences(recorded, leg.libA), 1u)
        << "'" << leg.libA << "' must appear exactly once in the emitted "
           "dependency table; got [" << joinDependencies(recorded) << "]";
    EXPECT_EQ(dependencyOccurrences(recorded, leg.libB), 1u)
        << "'" << leg.libB << "' must appear exactly once in the emitted "
           "dependency table; got [" << joinDependencies(recorded) << "] -- this is "
           "the D-LK-ELF-EMITS-ONE-DT-NEEDED shape: the SECOND library is "
           "the one that goes missing";
}

}  // namespace

TEST(ImageLibraryDeps, EveryResolvedLibraryIsRecordedOnEveryFormat) {
    for (auto const& leg : allLegs()) {
        SCOPED_TRACE(leg.label);
        checkEveryLibraryRecorded(leg);
    }
}

// The extractors themselves must be able to FAIL. If a parser silently
// returned {} on every input, the pin above would be satisfied by nothing at
// all... except that it asserts PRESENCE, so an empty result reds it. This
// test is the other half: the extractor must not be reading some incidental
// string, so a module with ONE library must yield a table that contains that
// library and NOT the other spelling.
//
// Per-leg subroutine for the same reason as above.
namespace {
void checkExtractorReportsOnlyRecorded(FormatLeg const& leg) {
    auto loaded = loadShippedPair(leg.target, leg.format);
    ASSERT_TRUE(loaded.target && loaded.format);

    // Both imports bind to libA; libB is never referenced by this module.
    auto const mod = makeTwoLibraryModule(leg.libA, leg.libA, leg.ret);
    DiagnosticReporter rep;
    auto const image =
        linker::link(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(rep.hasErrors())
        << (rep.all().empty() ? "" : rep.all().front().actual);
    ASSERT_FALSE(image.bytes.empty());

    auto const recorded = leg.deps(image.bytes);
    EXPECT_EQ(dependencyOccurrences(recorded, leg.libA), 1u)
        << "two imports from ONE library collapse to ONE dependency row; "
           "got [" << joinDependencies(recorded) << "]";
    EXPECT_EQ(dependencyOccurrences(recorded, leg.libB), 0u)
        << "a library this module never referenced must not appear; got ["
        << joinDependencies(recorded) << "]";
}
}  // namespace

TEST(ImageLibraryDeps, ExtractorsReportOnlyTheLibrariesActuallyRecorded) {
    for (auto const& leg : allLegs()) {
        SCOPED_TRACE(leg.label);
        checkExtractorReportsOnlyRecorded(leg);
    }
}
