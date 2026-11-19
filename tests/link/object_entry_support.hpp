#pragma once

// P69 folds 3d and 3e, lane p69/xa: what the three ObjectEntry suites share -- the reference compilers' entry
// objects and the targets DSS links them for, the driver build every cell makes, the family's own assembly, and the
// gate that says where a family's images run. The suites (each its own ctest entry, so one budget never holds them
// all):
//   tests/link/test_object_entry.cpp           an object's entry on every route, beside another entry, and run
//   tests/link/test_object_entry_grammars.cpp  every grammar elects an object's entry; an assembly unit's entry
//   tests/link/test_object_entry_archive.cpp   an archive member's entry; `main` beside `wmain`
//
// The fixtures (`tests/link/data/object_entry_arguments.source.c` states how each was built: each built twice,
// byte-identical, recording no host path; each linked by its reference beside the helper object and run as
// `prog 7 x y` exits 47 (`main(argc, argv)`), 5 (`main(void)`) or 11 (`main(argc, argv, envp)`)):
//   object_entry_{args,void,envp,helper}_x86_64_pe_cl_MD_O2.obj   cl 19.44 /MD + link.exe 14.44
//   object_entry_{args,void,envp,helper}_x86_64_pe_gcc_O2.obj     MinGW-w64 gcc 13.2 + GNU ld (`main` in
//                                                                 `.text.startup`, calling the mingw-w64
//                                                                 runtime's `__main` first)
//   object_entry_{args,void,envp,helper}_x86_64_elf_gcc_O2.o      gcc 13.3 + GNU ld 2.42; linked for the PIE
//                                                                 document too
//   object_entry_{args,void,envp,helper}_x86_64_elf_clang_O2.o    clang 18.1.3 + GNU ld 2.42; the PIE document too
//   object_entry_{args,void,envp,helper}_arm64_elf_gcc_O2.o       aarch64-linux-gnu-gcc 13.3 (run under qemu-aarch64);
//                                                                 the PIE document too
//   object_entry_{args,void,envp,helper}_arm64_macho_clang_O2.o   Apple clang 21.0.0 + ld (macOS 26.6.2)
//   object_entry_{args,void,envp,helper}_x86_64_macho_clang_O2.o  Apple clang 21.0.0 `-arch x86_64` (P69 fold 3e)
//
// An object carries no prototype, so its entry is elected with the RICHEST row of its name that the format realizes
// (for `main`, `argc-argv-envp`), as every reference's startup calls it: a `main(void)` or `main(argc, argv)` reads
// the leading arguments of that call under the C convention.
//
// Every TEST but the `...Native` ones runs in-process on every leg: each image is linked, never run, there. A native
// one runs each family whose images this host executes: its own OS and arch, or an arch the host's own translation
// runs (`host_translations.json`: an x86_64 Mach-O image under Rosetta 2 on an arm64 Mac). A translation the host
// declares but cannot use is a skip naming what is missing, red under DSS_STRICT_ARM_VERDICTS.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "program/program.hpp"

#include "host_translations.hpp"
#include "image_dependency_table.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dss::test_support::object_entry {

namespace fs = std::filesystem;

// One reference compiler's objects, and the executable target DSS links them for.
struct Family {
    char const* tag;            // the fixture name's middle: object_entry_<shape>_<tag>_O2<ext>
    char const* ext;
    char const* execTarget;
    char const* os;             // the OS that runs its images, in the `runOn` vocabulary
    char const* arch;           // the spec's target
    char const* symbolPrefix;   // how the references spell a C name on the format's binaries (ld64 enters `_main`)
};

inline constexpr Family kFamilies[] = {
    {"x86_64_pe_cl_MD", ".obj", "x86_64:pe64-x86_64-windows-exec", "windows", "x86_64", ""},
    {"x86_64_pe_gcc", ".obj", "x86_64:pe64-x86_64-windows-exec", "windows", "x86_64", ""},
    {"x86_64_elf_gcc", ".o", "x86_64:elf64-x86_64-linux-exec", "linux", "x86_64", ""},
    {"x86_64_elf_clang", ".o", "x86_64:elf64-x86_64-linux-exec", "linux", "x86_64", ""},
    {"arm64_elf_gcc", ".o", "arm64:elf64-aarch64-linux-exec", "linux", "arm64", ""},
    {"arm64_macho_clang", ".o", "arm64:macho64-arm64-darwin-exec", "darwin", "arm64", "_"},
    {"x86_64_macho_clang", ".o", "x86_64:macho64-x86_64-darwin-exec", "darwin", "x86_64", "_"},
    {"x86_64_elf_gcc", ".o", "x86_64:elf64-x86_64-linux-pie", "linux", "x86_64", ""},
    {"x86_64_elf_clang", ".o", "x86_64:elf64-x86_64-linux-pie", "linux", "x86_64", ""},
    {"arm64_elf_gcc", ".o", "arm64:elf64-aarch64-linux-pie", "linux", "arm64", ""},
};

// Each entry shape, and what its program exits run as `prog 7 x y` under every reference.
struct Shape {
    char const*   name;
    std::uint32_t exit;
};

inline constexpr Shape kShapes[] = {{"args", 47u}, {"void", 5u}, {"envp", 11u}};

inline constexpr char const* kHelperSource = "int helper(void) { return 0; }\n";
inline constexpr char const* kOtherSource  = "int unused_other(void) { return 3; }\n";
inline constexpr char const* kMainSource   = "int helper(void);\nint main(void) { return helper() + 5; }\n";

[[nodiscard]] inline fs::path fixturePath(std::string const& name) {
    return dss::test::repoRoot() / "tests" / "link" / "data" / name;
}

[[nodiscard]] inline std::string objectName(Family const& f, std::string_view shape) {
    return "object_entry_" + std::string{shape} + "_" + f.tag + "_O2" + f.ext;
}

[[nodiscard]] inline std::string labelOf(Family const& f) { return std::string{f.tag} + " -> " + f.execTarget; }

[[nodiscard]] inline bool isPe(Family const& f) {
    return std::string_view{f.execTarget}.find(":pe") != std::string_view::npos;
}

[[nodiscard]] inline bool isArm64(Family const& f) { return std::string_view{f.arch} == "arm64"; }

// The spec of another flavor of the family's format: "x86_64:pe64-x86_64-windows-exec" -> "...-staticlib", or the
// relocatable one for an empty `flavor`.
[[nodiscard]] inline std::string flavorOf(Family const& f, std::string_view flavor) {
    std::string_view const exec{f.execTarget};
    std::string base{exec.substr(0, exec.rfind('-'))};
    return flavor.empty() ? base : base + "-" + std::string{flavor};
}

// The target's own assembly dialect, by the name `--language` takes: the language a build with no `--language`
// resolves (`defaultAssemblyLanguage`). "" after a failure, which is reported.
[[nodiscard]] inline std::string dialectOf(Family const& f) {
    auto const target = TargetSchema::loadShipped(f.arch);
    if (!target.has_value()) {
        ADD_FAILURE() << "cannot load the shipped target " << f.arch;
        return {};
    }
    return std::string{(*target)->defaultAssemblyLanguage()};
}

[[nodiscard]] inline std::string grammarLabel(std::string const& g) { return g.empty() ? "no --language" : g; }

[[nodiscard]] inline std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

[[nodiscard]] inline bool reported(DiagnosticReporter const& rep, DiagnosticCode code) {
    auto const& all = rep.all();
    return std::any_of(all.begin(), all.end(), [&](auto const& d) { return d.code == code; });
}

// The text of the first diagnostic of `code`, "" when none was reported.
[[nodiscard]] inline std::string textOf(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return d.actual;
    }
    return {};
}

// One input of a build: a fixture copied in, a source written, or a file this test built copied in.
struct Input {
    std::string                name;   // the file's name in the build directory
    std::optional<std::string> text;   // the text to write; nullopt: copied from `from`, else a fixture
    fs::path                   from;   // a file this test built
};

[[nodiscard]] inline Input fixture(std::string name) { return {std::move(name), std::nullopt, {}}; }
[[nodiscard]] inline Input written(std::string name, std::string text) { return {std::move(name), std::move(text), {}}; }
[[nodiscard]] inline Input copied(fs::path const& path) { return {path.filename().string(), std::nullopt, path}; }

struct Built {
    int                rc = -1;
    std::string        diagnostics;
    fs::path           image;
    DiagnosticReporter rep;
};

// DSS's driver builds `inputs`, in that order, for `target` in `dir` under `language` ("" names none: the target's
// assembly dialect), with `archives` as `--resolve-library` inputs: one input through `compileFiles`, several
// through `compileUnits` (the CLI's own routing by the count of named files).
inline void build(fs::path const& dir, std::vector<Input> const& inputs, std::string const& target, Built& out,
                  std::string const& language = "c", std::vector<fs::path> const& archives = {}) {
    fs::create_directories(dir);
    std::vector<std::string> paths;
    for (auto const& in : inputs) {
        auto const path = dir / in.name;
        if (in.text.has_value()) {
            std::ofstream(path, std::ios::binary) << *in.text;
        } else {
            std::error_code ec;
            fs::copy_file(in.from.empty() ? fixturePath(in.name) : in.from, path,
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                ADD_FAILURE() << "cannot copy " << in.name << ": " << ec.message();
                return;
            }
        }
        paths.push_back(path.string());
    }
    auto const outDir = dir / "dss.out";
    fs::create_directories(outDir);
    Program p;
    p.setOutputDir(outDir);
    if (!archives.empty()) p.setResolveLibraries(archives);
    std::vector<std::string> const targets{target};
    out.rc = paths.size() == 1u ? p.compileFiles(paths, language, targets, out.rep)
                                : p.compileUnits(paths, language, targets, out.rep);
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

// DSS builds one C source for `target` in `dir` and returns the one file it writes: an object for a relocatable
// target, an archive for a static-library one. Empty after a failure, which is reported.
[[nodiscard]] inline fs::path buildArtifact(fs::path const& dir, std::string const& name, std::string const& text,
                                            std::string const& target) {
    Built b;
    build(dir, {written(name, text)}, target, b);
    if (b.rc != 0 || b.image.empty()) {
        ADD_FAILURE() << "DSS cannot build " << name << " for " << target << b.diagnostics;
        return {};
    }
    return b.image;
}

// ── The family's own assembly ────────────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline std::string functionHead(Family const& f, std::string const& symbol) {
    return "\t.text\n\t.globl\t" + symbol + "\n\t.type\t" + symbol + (isArm64(f) ? ", %function\n" : ", @function\n")
         + symbol + ":\n";
}

// `helper()` returns 0.
[[nodiscard]] inline std::string helperAsm(Family const& f) {
    std::string const h = std::string{f.symbolPrefix} + "helper";
    return functionHead(f, h) + (isArm64(f) ? "\tmov\tx0, #0\n\tret\n" : "\tmovq\t$0, %rax\n\tret\n");
}

// `main()` returns helper() + 5. On x86_64 the call is made with the stack 16-byte aligned and the Win64 home area
// reserved, so one text serves every format; on arm64 the link register is kept in a scratch register the helper
// (a leaf that writes x0 alone) leaves untouched.
[[nodiscard]] inline std::string mainAsm(Family const& f) {
    std::string const m = std::string{f.symbolPrefix} + "main";
    std::string const h = std::string{f.symbolPrefix} + "helper";
    return functionHead(f, m)
         + (isArm64(f) ? "\tmov\tx9, x30\n\tbl\t" + h + "\n\tmov\tx30, x9\n\tadd\tx0, x0, #5\n\tret\n"
                       : "\tsubq\t$40, %rsp\n\tcall\t" + h + "\n\taddq\t$40, %rsp\n\taddq\t$5, %rax\n\tret\n");
}

// `<name>()` returns `value` and calls nothing.
[[nodiscard]] inline std::string soleAsm(Family const& f, std::string_view name, unsigned value) {
    std::string const s = std::string{f.symbolPrefix} + std::string{name};
    return functionHead(f, s)
         + (isArm64(f) ? "\tmov\tx0, #" + std::to_string(value) + "\n\tret\n"
                       : "\tmovq\t$" + std::to_string(value) + ", %rax\n\tret\n");
}

[[nodiscard]] inline std::vector<std::uint8_t> bytesOf(fs::path const& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// A PE image whose entry is called with its arguments imports the C runtime's accessors of them: the count and the
// narrow vector (`__p___argc`, `__p___argv`), and the environment (`_get_initial_narrow_environment`), each once.
inline void expectTheArgumentAccessorsImported(fs::path const& image) {
    auto const symbols = peImportedSymbols(bytesOf(image));
    for (char const* accessor : {"__p___argc", "__p___argv", "_get_initial_narrow_environment"}) {
        EXPECT_EQ(dependencyOccurrences(symbols, accessor), 1u)
            << "the image imports " << accessor << " once; it imports: " << joinDependencies(symbols);
    }
}

// ── Where a family's images run ──────────────────────────────────────────────────────────────────────────────────

// Whether this host executes a family's images, and under what launcher: its own OS and arch natively, or another
// arch through the host's own translation (the repository's one cross-arch gate, host_translations.hpp). `missing`
// names a translation this host declares but cannot use.
struct RunGate {
    bool                     here = false;
    std::vector<std::string> launcher;
    bool                     launcherExecsImage = false;
    std::string              missing;
};

[[nodiscard]] inline RunGate runGateFor(Family const& f) {
    RunGate g;
    std::string const os = currentHostOs(), arch = currentHostArch();
    if (os != f.os) return g;
    if (arch == f.arch) {
        g.here = true;
        return g;
    }
    if (hostTranslationFor(shippedHostTranslations(), os, arch, f.arch) == nullptr) {
        return g;   // no translation: the family's images are another host's
    }
    auto const d = crossArchDecisionForThisHost(f.arch, /*manifestEmulator=*/"");
    if (!d.runs) {
        g.missing = d.why;
        return g;
    }
    g.here               = true;
    g.launcher           = d.launcherPrefix;
    g.launcherExecsImage = d.launcherExecsImage;
    return g;
}

// A native TEST's loop over the families: each family whose images this host runs, with its gate. A declared
// translation the host cannot use is a failure under strict arm verdicts and a skip otherwise; the TEST skips when
// no family ran (`why`).
class NativeFamilies {
public:
    NativeFamilies() : strict_{readStrictArmVerdicts()} {
        if (strict_.malformed) {
            ADD_FAILURE() << kStrictArmVerdictsEnv << "='" << strict_.raw << "' is not a recognised value";
        }
    }
    // The gate of `f` when this host runs its images, else nullopt (a declared but unusable translation reported).
    [[nodiscard]] std::optional<RunGate> admit(Family const& f) {
        RunGate g = runGateFor(f);
        if (!g.missing.empty()) {
            if (strict_.on) ADD_FAILURE() << labelOf(f) << ": " << g.missing;
            skipped_ += "\n  " + labelOf(f) + ": " + g.missing;
            return std::nullopt;
        }
        if (!g.here) return std::nullopt;
        ++ran_;
        return g;
    }
    [[nodiscard]] bool ranNone() const noexcept { return ran_ == 0u; }
    [[nodiscard]] std::string why() const {
        return "no family's images run on this host (" + currentHostOs() + " " + currentHostArch() + ")" + skipped_;
    }

private:
    StrictArmVerdictsSetting strict_;
    std::size_t              ran_ = 0;
    std::string              skipped_;
};

// ONE CELL, one function: a cell's ASSERT ends the cell, never the test, so one family's refusal cannot hide what
// every later family does (an ASSERT in the TEST's own loops stopped at the first red one).

// One build: it links and writes one image; where `argumentsRead` (the elected entry is called with the arguments
// the format's startup passes), a PE image imports the argument accessors.
inline void linkCell(fs::path const& dir, Family const& family, std::vector<Input> const& inputs,
                     std::string const& language = "c", std::vector<fs::path> const& archives = {},
                     bool argumentsRead = true) {
    Built built;
    build(dir, inputs, family.execTarget, built, language, archives);
    ASSERT_EQ(built.rc, 0) << built.diagnostics;
    ASSERT_FALSE(built.image.empty()) << "the build writes one image";
    if (argumentsRead && isPe(family)) expectTheArgumentAccessorsImported(built.image);
}

// The same, and its image, run as `prog 7 x y` under the family's gate, exits as the references' program of the same
// inputs does.
inline void runCell(fs::path const& dir, Family const& family, RunGate const& gate, std::vector<Input> const& inputs,
                    std::uint32_t exit, std::string const& language = "c",
                    std::vector<fs::path> const& archives = {}) {
    Built built;
    build(dir, inputs, family.execTarget, built, language, archives);
    ASSERT_EQ(built.rc, 0) << built.diagnostics;
    ASSERT_FALSE(built.image.empty());
    auto const run = runBinary(built.image, kRunBudget, /*captureStdout=*/false, gate.launcher,
                               /*programArgs=*/{"7", "x", "y"}, gate.launcherExecsImage);
    ASSERT_TRUE(run.spawned) << run.diagnostic;
    EXPECT_FALSE(run.timedOut);
    EXPECT_EQ(run.exitCode, exit) << "the references' program of the same inputs exits " << exit;
}

// A build refused with `code`, writing no image; where `mustName` is given, the refusal's text names it.
inline void refusedCell(fs::path const& dir, Family const& family, std::vector<Input> const& inputs,
                        DiagnosticCode code, std::string const& language, std::string const& mustName = {}) {
    Built built;
    build(dir, inputs, family.execTarget, built, language);
    EXPECT_NE(built.rc, 0) << "the build is refused";
    EXPECT_TRUE(reported(built.rep, code)) << built.diagnostics;
    EXPECT_TRUE(built.image.empty()) << "no image is written";
    if (!mustName.empty()) {
        EXPECT_NE(textOf(built.rep, code).find(mustName), std::string::npos)
            << "the refusal names " << mustName << built.diagnostics;
    }
}

}  // namespace dss::test_support::object_entry
