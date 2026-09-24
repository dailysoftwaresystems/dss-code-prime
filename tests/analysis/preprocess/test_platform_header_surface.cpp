// The shipped `<sys/stat.h>` mode surface and the pe `<direct.h>`/`<dirent.h>`
// header graph are the PLATFORM REFERENCES' — no more, no less. P68 round 9:
// D-CONFIG-SYS-STAT-OVER-DECLARES-S-ISLNK-ON-PE (the coordinator's ruling (a)) and
// the `<sys/stat.h>` family found incomplete while measuring it.
//
// ✔MEASURED 2026-09-23 with `#ifdef` + value probes per name, each reference
// separately:
//   * glibc — gcc 13.3.0 and clang 18.1.3, default gnu mode, x86_64 and aarch64 —
//     defines every POSIX mode name: S_IF* (S_IFLNK 40960, S_IFSOCK 49152),
//     S_ISUID 2048, S_ISGID 1024, S_ISVTX 512, all twelve permission bits
//     (S_IRWXG 56, S_IRWXO 7), S_IREAD/S_IWRITE/S_IEXEC and every S_IS* test;
//   * mingw-w64 gcc 13.2.0 defines S_IFMT/DIR/CHR/BLK(12288)/REG/IFO, the twelve
//     permission bits, S_IREAD/S_IWRITE/S_IEXEC, _S_IFMT/DIR/CHR/IFO/REG,
//     _S_IREAD/_S_IWRITE/_S_IEXEC and S_ISDIR/REG/CHR/BLK/FIFO — and NOT S_IFLNK,
//     S_IFSOCK, S_ISUID, S_ISGID, S_ISVTX, S_ISLNK or S_ISSOCK;
//   * MSVC 14.51 defines S_IFMT/DIR/CHR/REG, S_IREAD/S_IWRITE/S_IEXEC and the _S_*
//     set, and no S_IS* test at all;
//   * Darwin (DOCUMENTED: Apple xnu-12377.121.6 bsd/sys/_types/_s_ifmt.h) is the
//     POSIX set plus S_ISTXT (= S_ISVTX) and S_IFWHT 0160000.
// And the pe header EDGES: `DIR` from a bare `<direct.h>` is rc=1 under mingw and
// rc=2 under MSVC; `<dirent.h>` gives no `DWORD` under mingw and does not exist
// under MSVC; `<windows.h>` makes `malloc`/`free` visible under both.
//
// So on each real pair a translation unit pins, at PREPROCESSING time, every name
// the platform defines (with its value) and every name it does not (`#ifdef` →
// `#error`: an implicit declaration cannot satisfy an `#ifdef`), and the pe graph
// is pinned by refusal: a `DIR` after only `<direct.h>`, and a `DWORD` after only
// `<dirent.h>`, are errors, while `malloc` after `<windows.h>` is not.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/target_format_analysis.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// One mode name: its value where the platform defines it, and on which platforms.
struct ModeName {
    std::string_view name;
    std::optional<int> elfValue;     // glibc
    std::optional<int> machoValue;   // Darwin
    std::optional<int> peValue;      // mingw-w64 (a superset of MSVC's set)
    bool             functionLike = false;   // an S_IS* test: presence only
};
constexpr std::optional<int> kNo = std::nullopt;
constexpr ModeName kModeNames[] = {
    {"S_IFMT",   61440, 61440, 61440},
    {"S_IFDIR",  16384, 16384, 16384},
    {"S_IFCHR",  8192,  8192,  8192},
    {"S_IFBLK",  24576, 24576, 12288},
    {"S_IFREG",  32768, 32768, 32768},
    {"S_IFIFO",  4096,  4096,  4096},
    {"S_IFLNK",  40960, 40960, kNo},
    {"S_IFSOCK", 49152, 49152, kNo},
    {"S_ISUID",  2048,  2048,  kNo},
    {"S_ISGID",  1024,  1024,  kNo},
    {"S_ISVTX",  512,   512,   kNo},
    {"S_ISTXT",  kNo,   512,   kNo},
    {"S_IFWHT",  kNo,   57344, kNo},
    {"S_IRWXU",  448,   448,   448},
    {"S_IRUSR",  256,   256,   256},
    {"S_IWUSR",  128,   128,   128},
    {"S_IXUSR",  64,    64,    64},
    {"S_IRWXG",  56,    56,    56},
    {"S_IRGRP",  32,    32,    32},
    {"S_IWGRP",  16,    16,    16},
    {"S_IXGRP",  8,     8,     8},
    {"S_IRWXO",  7,     7,     7},
    {"S_IROTH",  4,     4,     4},
    {"S_IWOTH",  2,     2,     2},
    {"S_IXOTH",  1,     1,     1},
    {"S_IREAD",  256,   256,   256},
    {"S_IWRITE", 128,   128,   128},
    {"S_IEXEC",  64,    64,    64},
    {"_S_IFMT",  kNo,   kNo,   61440},
    {"_S_IFDIR", kNo,   kNo,   16384},
    {"_S_IFCHR", kNo,   kNo,   8192},
    {"_S_IFIFO", kNo,   kNo,   4096},
    {"_S_IFREG", kNo,   kNo,   32768},
    {"_S_IREAD", kNo,   kNo,   256},
    {"_S_IWRITE", kNo,  kNo,   128},
    {"_S_IEXEC", kNo,   kNo,   64},
    {"S_ISDIR",  1, 1, 1, true},
    {"S_ISREG",  1, 1, 1, true},
    {"S_ISCHR",  1, 1, 1, true},
    {"S_ISBLK",  1, 1, 1, true},
    {"S_ISFIFO", 1, 1, 1, true},
    {"S_ISLNK",  1, 1, kNo, true},
    {"S_ISSOCK", 1, 1, kNo, true},
};

[[nodiscard]] std::optional<int> valueFor(ModeName const& m, ObjectFormatKind f) {
    switch (f) {
        case ObjectFormatKind::Elf:   return m.elfValue;
        case ObjectFormatKind::MachO: return m.machoValue;
        case ObjectFormatKind::Pe:    return m.peValue;
        default:                      return std::nullopt;
    }
}

[[nodiscard]] std::string statProbe(ObjectFormatKind f) {
    std::string s = "#include <sys/types.h>\n#include <sys/stat.h>\n";
    for (auto const& m : kModeNames) {
        std::optional<int> const v = valueFor(m, f);
        if (!v.has_value()) {
            s += std::format("#ifdef {0}\n#error \"{0} is defined by no reference here\"\n#endif\n",
                             m.name);
            continue;
        }
        s += std::format("#ifndef {0}\n#error \"{0} is not defined\"\n#endif\n", m.name);
        if (!m.functionLike) {
            s += std::format("#if {0} != {1}\n#error \"{0} is not {1}\"\n#endif\n", m.name, *v);
        }
    }
    s += "int dss_stat_probe;\n";
    return s;
}

[[nodiscard]] std::vector<std::string> shippedNames(std::string_view dir,
                                                    std::string_view suffix) {
    std::vector<std::string> out;
    for (auto const& e :
         std::filesystem::directory_iterator{dss::test::configRoot() / dir}) {
        std::string const fn = e.path().filename().string();
        auto const at = fn.find(suffix);
        if (at == std::string::npos) continue;
        out.push_back(fn.substr(0, at));
    }
    return out;
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        return loaded.has_value() ? *loaded : std::shared_ptr<GrammarSchema const>{};
    }();
    return schema;
}

[[nodiscard]] std::string errorsOf(TargetSchema const& target, ObjectFormatSchema const& format,
                                   std::string source) {
    UnitBuilder builder{cLanguage(), DiagnosticBudget::libraryDefault()};
    applySystemDirs(builder, *cLanguage());
    applyTargetFormatPair(builder, target, format);
    builder.addInMemory(std::move(source), "probe.c");
    auto const cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());
    std::string out;
    auto const collect = [&](DiagnosticReporter const& rep) {
        for (auto const& d : rep.all()) {
            if (d.severity == DiagnosticSeverity::Error) out += "  " + d.actual + "\n";
        }
    };
    collect(cu->driverDiagnostics());
    for (auto const& tree : cu->trees()) collect(tree.diagnostics());
    auto const analysis = analyzeForTargetFormat(cu, DiagnosticBudget::libraryDefault(),
                                                 target, format, nullptr);
    collect(analysis.model.diagnostics());
    return out;
}

}  // namespace

// ── THE MODE SURFACE, ON EVERY REAL PAIR ────────────────────────────────────
TEST(PlatformHeaderSurface, SysStatDefinesExactlyWhatThePlatformDefines) {
    ASSERT_NE(cLanguage(), nullptr);
    std::size_t pairs = 0;
    for (std::string const& targetName : shippedNames("targets", ".target.json")) {
        auto targetR = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(targetR.has_value()) << targetName;
        for (std::string const& formatName : shippedNames("object-formats", ".format.json")) {
            auto formatR = ObjectFormatSchema::loadShipped(formatName);
            ASSERT_TRUE(formatR.has_value()) << formatName;
            if ((*formatR)->targetArch() != (*targetR)->name()) continue;
            ObjectFormatKind const k = (*formatR)->kind();
            if (k != ObjectFormatKind::Elf && k != ObjectFormatKind::MachO
                && k != ObjectFormatKind::Pe) {
                continue;
            }
            SCOPED_TRACE(targetName + ":" + formatName);
            ++pairs;
            EXPECT_EQ(errorsOf(**targetR, **formatR, statProbe(k)), "");
        }
    }
    EXPECT_GE(pairs, 22u) << "the real-pair enumeration collapsed";
}

// ── THE pe HEADER GRAPH: WHAT <direct.h> AND <dirent.h> DO NOT BRING ────────
TEST(PlatformHeaderSurface, ThePeDirectAndDirentHeadersPullOnlyWhatTheReferencesPull) {
    ASSERT_NE(cLanguage(), nullptr);
    auto targetR = TargetSchema::loadShipped("x86_64");
    auto formatR = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(targetR.has_value() && formatR.has_value());
    // The controls: each header's OWN surface, and the one edge both references have.
    EXPECT_EQ(errorsOf(**targetR, **formatR,
                       "#include <direct.h>\nint (*f)(const char *) = _mkdir;\n"), "");
    EXPECT_EQ(errorsOf(**targetR, **formatR, "#include <dirent.h>\nstatic DIR *d;\n"), "");
    EXPECT_EQ(errorsOf(**targetR, **formatR,
                       "#include <windows.h>\nvoid (*f)(void *) = free;\nstatic DWORD w;\n"),
              "")
        << "<windows.h> -> <stdlib.h> is an edge BOTH Windows references have";
    // The removed edges: refused, as mingw (rc=1) and MSVC (rc=2) refuse them.
    EXPECT_NE(errorsOf(**targetR, **formatR, "#include <direct.h>\nstatic DIR *d;\n"), "")
        << "<direct.h> must not reach DIR (no Windows reference does)";
    EXPECT_NE(errorsOf(**targetR, **formatR, "#include <dirent.h>\nstatic DWORD w;\n"), "")
        << "<dirent.h> must not reach <windows.h> (mingw's does not; MSVC has none)";
}
