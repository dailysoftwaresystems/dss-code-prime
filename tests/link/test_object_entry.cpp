// P69 fold 3d, lane p69/xa: a program's ENTRY defined by an OBJECT that a reference compiler built.
//
//   D-PROGRAM-AN-OBJECTS-ENTRY-IS-CALLED-WITHOUT-ITS-ARGUMENTS: an object's `main` linked for
//     `pe64-x86_64-windows-exec` was called on whatever the argument registers held, because the pre-main init that
//     reads the C runtime's argument accessors was made only for an entry a DSS unit defines. ✔MEASURED (lane `xa`'s
//     `argc_pe.py`, run 24's dsscp), as `prog 7 x y`: cl 19.44's `main(argc, argv)` object exited 1091911680 and its
//     `main(argc, argv, envp)` object exited 12, where link.exe's images exit 47 and 11. The init is now made for the
//     entry wherever it is defined, and it calls the object's entry by name (`realizeForeignEntryShape`).
//   D-PROGRAM-ENTRY-IN-AN-OBJECT-INPUT-BESIDE-A-SOURCE-IS-UNDEFINED: a build naming a source and an object that
//     defines `main` (`dsscp --compile helper.c main.o`, `cc helper.c main.o`) was refused K_ProgramEntryUndefined on
//     every format (MEASURED, PE and ELF x86_64): the election read the units' functions alone. The named objects'
//     entries now stand in the same election (`collectObjectEntryCandidates`), so an object's `main` beside a
//     source's `main` is the ambiguity it is between two units.
//
// An object carries no prototype, so its entry is elected with the RICHEST row of its name that the format realizes
// (for `main`, `argc-argv-envp`), as every reference's startup calls it: a `main(void)` or `main(argc, argv)` reads
// the leading arguments of that call under the C convention.
//
// The fixtures (`tests/link/data/object_entry_arguments.source.c` states how each was built: each built twice,
// byte-identical, recording no host path; each linked by its reference beside the helper object and run as
// `prog 7 x y` exits 47 (`main(argc, argv)`), 5 (`main(void)`) or 11 (`main(argc, argv, envp)`)):
//   object_entry_{args,void,envp,helper}_x86_64_pe_cl_MD_O2.obj   cl 19.44 /MD + link.exe 14.44
//   object_entry_{args,void,envp,helper}_x86_64_pe_gcc_O2.obj     MinGW-w64 gcc 13.2 + GNU ld (`main` in
//                                                                 `.text.startup`, calling the mingw-w64
//                                                                 runtime's `__main` first)
//   object_entry_{args,void,envp,helper}_x86_64_elf_gcc_O2.o      gcc 13.3 + GNU ld 2.42
//   object_entry_{args,void,envp,helper}_x86_64_elf_clang_O2.o    clang 18.1.3 + GNU ld 2.42
//   object_entry_{args,void,envp,helper}_arm64_elf_gcc_O2.o       aarch64-linux-gnu-gcc 13.3 (run under qemu-aarch64)
//   object_entry_{args,void,envp,helper}_arm64_macho_clang_O2.o   Apple clang 21.0.0 + ld (macOS 26.6.2)
//
// Every TEST but the `...Native` one runs in-process on every leg: each image is linked, never run, there. The native
// one runs each family whose executable target is the host's own.

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
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

// One reference compiler's objects, and the executable target DSS links them for.
struct Family {
    char const* tag;          // the fixture name's middle: object_entry_<shape>_<tag>_O2<ext>
    char const* ext;
    char const* execTarget;
};

constexpr Family kFamilies[] = {
    {"x86_64_pe_cl_MD", ".obj", "x86_64:pe64-x86_64-windows-exec"},
    {"x86_64_pe_gcc", ".obj", "x86_64:pe64-x86_64-windows-exec"},
    {"x86_64_elf_gcc", ".o", "x86_64:elf64-x86_64-linux-exec"},
    {"x86_64_elf_clang", ".o", "x86_64:elf64-x86_64-linux-exec"},
    {"arm64_elf_gcc", ".o", "arm64:elf64-aarch64-linux-exec"},
    {"arm64_macho_clang", ".o", "arm64:macho64-arm64-darwin-exec"},
};

// Each entry shape, and what its program exits run as `prog 7 x y` under every reference.
struct Shape {
    char const*   name;
    std::uint32_t exit;
};

constexpr Shape kShapes[] = {{"args", 47u}, {"void", 5u}, {"envp", 11u}};

constexpr char const* kHelperSource = "int helper(void) { return 0; }\n";
constexpr char const* kOtherSource  = "int unused_other(void) { return 3; }\n";
constexpr char const* kMainSource   = "int helper(void);\nint main(void) { return helper() + 5; }\n";

[[nodiscard]] fs::path fixturePath(std::string const& name) {
    return dss::test::repoRoot() / "tests" / "link" / "data" / name;
}

[[nodiscard]] std::string objectName(Family const& f, std::string_view shape) {
    return "object_entry_" + std::string{shape} + "_" + f.tag + "_O2" + f.ext;
}

[[nodiscard]] bool isPe(Family const& f) { return std::string_view{f.execTarget}.find(":pe") != std::string_view::npos; }

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

[[nodiscard]] bool reported(DiagnosticReporter const& rep, DiagnosticCode code) {
    auto const& all = rep.all();
    return std::any_of(all.begin(), all.end(), [&](auto const& d) { return d.code == code; });
}

// One input of a build: a fixture copied in, or a C source written.
struct Input {
    std::string name;     // the file's name in the build directory
    char const* source;   // nullptr: `name` is a fixture
};

struct Built {
    int         rc = -1;
    std::string diagnostics;
    fs::path    image;
    DiagnosticReporter rep;
};

// DSS's driver builds `inputs`, in that order, for `target` in `dir`: one input through `compileFiles`, several
// through `compileUnits` (the CLI's own routing by the count of named files).
void build(fs::path const& dir, std::vector<Input> const& inputs, std::string const& target, Built& out) {
    fs::create_directories(dir);
    std::vector<std::string> paths;
    for (auto const& in : inputs) {
        auto const path = dir / in.name;
        if (in.source != nullptr) {
            std::ofstream(path, std::ios::binary) << in.source;
        } else {
            std::error_code ec;
            fs::copy_file(fixturePath(in.name), path, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                ADD_FAILURE() << "cannot copy fixture " << in.name << ": " << ec.message();
                return;
            }
        }
        paths.push_back(path.string());
    }
    auto const outDir = dir / "dss.out";
    fs::create_directories(outDir);
    Program p;
    p.setOutputDir(outDir);
    std::vector<std::string> const targets{target};
    out.rc = paths.size() == 1u ? p.compileFiles(paths, "c", targets, out.rep)
                                : p.compileUnits(paths, "c", targets, out.rep);
    out.diagnostics = diagnosticsOf(out.rep);
    std::vector<fs::path> files;
    for (auto const& e : fs::directory_iterator(outDir)) {
        if (e.is_regular_file()) files.push_back(e.path());
    }
    for (auto const& f : files) {
        if (f.extension() == ".exe") out.image = f;
    }
    if (out.image.empty() && files.size() == 1u) out.image = files.front();
}

// The routes an object's entry is linked through: objects alone (both orders, so the entry's object is not the
// first), beside one source (the single-unit route), beside two (the merged one).
struct Route {
    char const*        label;
    std::vector<Input> inputs;
};

[[nodiscard]] std::vector<Route> routesOf(Family const& f, std::string_view shape) {
    std::string const entry  = objectName(f, shape);
    std::string const helper = objectName(f, "helper");
    return {
        {"objects alone, the entry's first", {{entry, nullptr}, {helper, nullptr}}},
        {"objects alone, the entry's last", {{helper, nullptr}, {entry, nullptr}}},
        {"beside one source", {{"helper.c", kHelperSource}, {entry, nullptr}}},
        {"beside two sources", {{"helper.c", kHelperSource}, {entry, nullptr}, {"other.c", kOtherSource}}},
    };
}

[[nodiscard]] std::vector<std::uint8_t> bytesOf(fs::path const& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// A PE image whose entry is called with its arguments imports the C runtime's accessors of them: the count and the
// narrow vector (`__p___argc`, `__p___argv`), and the environment (`_get_initial_narrow_environment`), each once.
void expectTheArgumentAccessorsImported(fs::path const& image) {
    auto const symbols = test_support::peImportedSymbols(bytesOf(image));
    for (char const* accessor : {"__p___argc", "__p___argv", "_get_initial_narrow_environment"}) {
        EXPECT_EQ(test_support::dependencyOccurrences(symbols, accessor), 1u)
            << "the image imports " << accessor << " once; it imports: " << test_support::joinDependencies(symbols);
    }
}

// ONE CELL, one function: a cell's ASSERT ends the cell, never the test, so one family's refusal cannot hide what
// every later family does (an ASSERT in the TEST's own loops stopped at the first red one).

// One route of one family's shape: it links, writes one image, and a PE image imports the argument accessors.
void linkCell(fs::path const& dir, Family const& family, Route const& route) {
    Built built;
    build(dir, route.inputs, family.execTarget, built);
    ASSERT_EQ(built.rc, 0) << built.diagnostics;
    ASSERT_FALSE(built.image.empty()) << "the build writes one image";
    if (isPe(family)) expectTheArgumentAccessorsImported(built.image);
}

// The same, and its image, run as `prog 7 x y`, exits as the reference's program of the same objects does.
void runCell(fs::path const& dir, Family const& family, Route const& route, Shape const& shape) {
    Built built;
    build(dir, route.inputs, family.execTarget, built);
    ASSERT_EQ(built.rc, 0) << built.diagnostics;
    ASSERT_FALSE(built.image.empty());
    auto const run = test_support::runBinary(built.image, test_support::kRunBudget,
                                             /*captureStdout=*/false, /*launcherPrefix=*/{},
                                             /*programArgs=*/{"7", "x", "y"});
    ASSERT_TRUE(run.spawned) << run.diagnostic;
    EXPECT_FALSE(run.timedOut);
    EXPECT_EQ(run.exitCode, shape.exit) << "the reference's program of the same objects exits " << shape.exit;
}

}  // namespace

// ══ EVERY ROUTE LINKS AN OBJECT'S ENTRY ════════════════════════════════════════════════════════════════════════════

// Every leg: each reference's entry object links on every route, for its own executable target; a PE image imports
// the C runtime's argument accessors its init calls the entry with. RED-ON-DISABLE: make no init for an entry another
// input defines -> each PE image imports none of the accessors; leave the named objects' entries out of a unit's
// election -> every route beside a source is refused K_ProgramEntryUndefined.
TEST(ObjectEntry, EveryRouteLinksAnObjectsEntryForItsFormat) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object-entry"};
    std::size_t k = 0;
    for (auto const& family : kFamilies) {
        for (auto const& shape : kShapes) {
            for (auto const& route : routesOf(family, shape.name)) {
                SCOPED_TRACE(std::string{family.tag} + " " + shape.name + ": " + route.label);
                linkCell(scratch.path() / std::to_string(k++), family, route);
            }
        }
    }
}

// Every leg: an entry an object defines beside one a source defines is refused as two entries are, and so are two
// objects' entries; neither build writes an image. RED-ON-DISABLE: leave the objects' entries out of the units'
// election -> the source's `main` is elected and the build links with two definitions of the entry's name.
TEST(ObjectEntry, AnObjectsEntryBesideAnotherEntryIsAmbiguous) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object-entry-ambiguous"};
    std::size_t k = 0;
    for (auto const& family : kFamilies) {
        std::string const args = objectName(family, "args");
        std::string const none = objectName(family, "void");
        std::string const helper = objectName(family, "helper");
        struct Case {
            char const*        label;
            std::vector<Input> inputs;
        };
        std::vector<Case> const cases{
            {"a source's main and an object's", {{"main.c", kMainSource}, {args, nullptr}, {helper, nullptr}}},
            {"two sources and an object's main",
             {{"main.c", kMainSource}, {"other.c", kOtherSource}, {args, nullptr}, {helper, nullptr}}},
            {"two objects' main", {{args, nullptr}, {none, nullptr}, {helper, nullptr}}},
        };
        for (auto const& c : cases) {
            SCOPED_TRACE(std::string{family.tag} + ": " + c.label);
            Built built;
            build(scratch.path() / std::to_string(k++), c.inputs, family.execTarget, built);
            EXPECT_NE(built.rc, 0) << "the build is refused";
            EXPECT_TRUE(reported(built.rep, DiagnosticCode::K_ProgramEntryAmbiguous)) << built.diagnostics;
            EXPECT_TRUE(built.image.empty()) << "no image is written";
        }
    }
}

// ══ NATIVE: each program receives its arguments ═══════════════════════════════════════════════════════════════════

// Where the family's executable target is the host's own: each image, run as `prog 7 x y`, exits as the reference's
// program of the same objects does (47, 5, 11) on every route. RED-ON-DISABLE: make no init for an entry another input
// defines -> a PE program's `main(argc, argv)` reads garbage (MEASURED 1091911680) and `main(argc, argv, envp)` exits
// 12.
TEST(ObjectEntryNative, EachProgramReceivesItsArgumentsAsTheReferencesDo) {
    auto const host = std::string{test_support::hostNativeTarget().execTarget};
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object-entry-run"};
    std::size_t k = 0, ran = 0;
    for (auto const& family : kFamilies) {
        if (host != family.execTarget) continue;
        for (auto const& shape : kShapes) {
            for (auto const& route : routesOf(family, shape.name)) {
                SCOPED_TRACE(std::string{family.tag} + " " + shape.name + ": " + route.label);
                runCell(scratch.path() / std::to_string(k++), family, route, shape);
                ++ran;
            }
        }
    }
    if (ran == 0u) GTEST_SKIP() << "no family's executable target is this host's (" << host << ")";
}
