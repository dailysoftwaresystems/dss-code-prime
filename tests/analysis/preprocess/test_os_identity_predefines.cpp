// The operating-system and object-format IDENTITY predefines, on every real pair (P68, lane lm, #2).
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// The ELF pairs predefined NONE of the six names every Linux reference predefines
// in its ISO modes (✔MEASURED at the round-10 base, `--dump-predefined-macros`), so a
// program's `#ifdef __linux__` arm — sqlite's pread/pwrite, its memory-mapped I/O and
// `mremap` — was silently skipped, and DSS built a different program than every
// reference builds from the same source.
//
// ★ THE PIN IS THE REFERENCES' ANSWER. ✔MEASURED 2026-09-24 (`-dM -E -x c /dev/null`):
// gcc 13.3.0 and clang 18.1.3, x86_64-linux and aarch64-linux, `-std=c2x`/`-std=c23`,
// define exactly `__linux__`, `__linux`, `__gnu_linux__`, `__unix__`, `__unix` and
// `__ELF__`, each 1 (their GNU modes add `linux` and `unix`, which the ISO modes — and
// DSS — withhold); Apple clang 21.0.0 defines none of the six (`__APPLE__`, `__MACH__`);
// mingw-w64 13.2.0 none (`_WIN32`, `_WIN64`). Every real pair compiles a translation
// unit of `#if` arms stating its platform's set — and, on the ELF pairs, a use of the
// surface each claim implies — through the driver's own two halves, and must be
// diagnostic-free. A pair the table does not know FAILS: a new format is measured
// deliberately, never inherited.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/preprocess/preprocessor.hpp"
#include "analysis/semantic/target_format_analysis.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// The six Linux names and the three families, as the references define them.
constexpr std::array<std::string_view, 6> kLinuxNames{
    "__linux__", "__linux", "__gnu_linux__", "__unix__", "__unix", "__ELF__"};

struct PlatformFacts {
    ObjectFormatKind                 format;
    std::vector<std::string_view>    defined;     // each defined as 1
    std::vector<std::string_view>    undefined;   // each NOT defined
    std::string_view                 source;
};

[[nodiscard]] std::vector<PlatformFacts> const& platforms() {
    static std::vector<PlatformFacts> const v = [] {
        std::vector<std::string_view> const linux{kLinuxNames.begin(), kLinuxNames.end()};
        std::vector<PlatformFacts> out;
        out.push_back({ObjectFormatKind::Elf, linux,
                       {"__APPLE__", "__MACH__", "_WIN32", "_WIN64", "linux", "unix"},
                       "gcc 13.3.0 + clang 18.1.3, x86_64 and aarch64 Linux, ISO modes, MEASURED"});
        std::vector<std::string_view> notLinux = linux;
        notLinux.push_back("linux");
        notLinux.push_back("unix");
        std::vector<std::string_view> machoUndef = notLinux;
        machoUndef.push_back("_WIN32");
        out.push_back({ObjectFormatKind::MachO, {"__APPLE__", "__MACH__"}, machoUndef,
                       "Apple clang 21.0.0, arm64 and x86_64, MEASURED"});
        std::vector<std::string_view> peUndef = notLinux;
        peUndef.push_back("__APPLE__");
        out.push_back({ObjectFormatKind::Pe, {"_WIN32", "_WIN64"}, peUndef,
                       "mingw-w64 gcc 13.2.0, MEASURED"});
        return out;
    }();
    return v;
}

[[nodiscard]] PlatformFacts const* factsFor(ObjectFormatKind f) {
    for (auto const& p : platforms()) {
        if (p.format == f) return &p;
    }
    return nullptr;
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

[[nodiscard]] std::string allErrors(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += "  ";
        out += d.actual;
        out += '\n';
    }
    return out;
}

// The translation unit one pair compiles: its identity in `#if`, and — where Linux is
// claimed — the surface the claim implies, by address and by use.
[[nodiscard]] std::string identityProbe(PlatformFacts const& p) {
    std::string s;
    for (std::string_view const n : p.defined) {
        s += std::format("#if !defined({0}) || ({0}) != 1\n#error \"{0} must be defined as 1 here\"\n#endif\n", n);
    }
    for (std::string_view const n : p.undefined) {
        s += std::format("#ifdef {0}\n#error \"{0} must not be predefined here\"\n#endif\n", n);
    }
    if (p.format == ObjectFormatKind::Elf) {
        s += "#include <unistd.h>\n#include <sys/mman.h>\n#include <sys/ioctl.h>\n#include <fcntl.h>\n"
             "#include <dirent.h>\n"
             "void *dss_linux_surface[] = { (void *)&pread, (void *)&pwrite, (void *)&mmap,\n"
             "    (void *)&munmap, (void *)&mremap, (void *)&ioctl, (void *)&getpid, (void *)&open,\n"
             "    (void *)&opendir, (void *)&readdir, (void *)&closedir, (void *)&unlink };\n"
             "int dss_linux_constants[] = { MREMAP_MAYMOVE, MAP_SHARED, PROT_READ, PROT_WRITE,\n"
             "    O_RDWR, O_CREAT, O_TRUNC, SEEK_SET, STDIN_FILENO };\n";
    }
    s += "int dss_identity_probe;\n";
    return s;
}

struct PairRun {
    std::string preprocessErrors;
    std::string semanticErrors;
    std::string driverErrors;
};

[[nodiscard]] PairRun compileForPair(std::shared_ptr<GrammarSchema const> const& grammar,
                                     TargetSchema const& target,
                                     ObjectFormatSchema const& format,
                                     std::string source) {
    UnitBuilder builder{grammar, DiagnosticBudget::libraryDefault()};
    applySystemDirs(builder, *grammar);
    applyTargetFormatPair(builder, target, format);
    builder.addInMemory(std::move(source), "probe.c");
    auto const cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());
    PairRun r;
    r.driverErrors = allErrors(cu->driverDiagnostics());
    for (auto const& tree : cu->trees()) r.preprocessErrors += allErrors(tree.diagnostics());
    auto const analysis = analyzeForTargetFormat(
        cu, DiagnosticBudget::libraryDefault(), target, format, nullptr);
    r.semanticErrors = allErrors(analysis.model.diagnostics());
    return r;
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        return loaded.has_value() ? *loaded : std::shared_ptr<GrammarSchema const>{};
    }();
    return schema;
}

}  // namespace

// ── EVERY PAIR PRESENTS ITS PLATFORM'S IDENTITY, AND THE CLAIM IS BACKED ─────
TEST(OsIdentityPredefines, EveryPairPresentsItsPlatformsIdentityAndItsSurface) {
    ASSERT_NE(cLanguage(), nullptr);
    std::size_t pairs = 0;
    for (std::string const& targetName : shippedNames("targets", ".target.json")) {
        auto targetR = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(targetR.has_value()) << targetName;
        for (std::string const& formatName : shippedNames("object-formats", ".format.json")) {
            auto formatR = ObjectFormatSchema::loadShipped(formatName);
            ASSERT_TRUE(formatR.has_value()) << formatName;
            if ((*formatR)->targetArch() != (*targetR)->name()) continue;   // not a real pair
            SCOPED_TRACE(targetName + ":" + formatName);
            PlatformFacts const* facts = factsFor((*formatR)->kind());
            if (facts == nullptr) {
                ADD_FAILURE() << "no reference measurement for "
                              << objectFormatKindName((*formatR)->kind())
                              << " — measure gcc/clang/MSVC there and add a row";
                continue;
            }
            ++pairs;
            PairRun const r =
                compileForPair(cLanguage(), **targetR, **formatR, identityProbe(*facts));
            EXPECT_EQ(r.driverErrors, "");
            EXPECT_EQ(r.preprocessErrors, "") << "(" << facts->source << ")";
            EXPECT_EQ(r.semanticErrors, "") << "(" << facts->source << ")";
        }
    }
    // FLOOR: ✔MEASURED — 22 real pairs (13 x86_64 + 9 arm64 formats).
    EXPECT_GE(pairs, 22u) << "the real-pair enumeration collapsed";
}

// ── THE OS FAMILIES ARE MUTUALLY EXCLUSIVE, AND THE GROUP IS WHAT SAYS SO ────
// `c.lang.json` pins `__linux__`, `__APPLE__` and `_WIN32` as a mutual-exclusion
// group: no reference compiler targets two operating systems at once. The shipped
// matrix is clean (`Preprocessor.PeIdentityNoShippedTargetFormatCoDefinesAnExclusiveGroup`
// sweeps every group over every target × format), so the FIRING path is driven here on
// the shipped text with one gate widened — and, as the red-on-disable, with the group
// removed as well, where the same co-definition is silently accepted.
namespace {

[[nodiscard]] std::string shippedCText() {
    std::ifstream in(dss::test::configRoot() / "sources" / "c.lang.json", std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The shipped text with one exact span replaced (it must occur exactly once).
[[nodiscard]] std::string edited(std::string text, std::string_view from, std::string_view to) {
    auto const at = text.find(from);
    if (at == std::string::npos || text.find(from, at + 1) != std::string::npos) {
        ADD_FAILURE() << "the shipped c config no longer holds exactly one '" << from << "'";
        return {};
    }
    text.replace(at, from.size(), to);
    return text;
}

constexpr std::string_view kLinuxGate =
    "\"name\": \"__linux__\",           \"kind\": \"constant\", \"value\": \"1\", "
    "\"availableObjectFormats\": [\"elf\"],";
constexpr std::string_view kLinuxGateOnPe =
    "\"name\": \"__linux__\",           \"kind\": \"constant\", \"value\": \"1\", "
    "\"availableObjectFormats\": [\"elf\", \"pe\"],";
constexpr std::string_view kOsGroupMacros =
    "\"macros\": [\"__linux__\", \"__APPLE__\", \"_WIN32\"]";

}  // namespace

TEST(OsIdentityPredefines, TwoOsFamiliesOnOnePairAreRefusedByTheGroup) {
    std::string const text = edited(shippedCText(), kLinuxGate, kLinuxGateOnPe);
    ASSERT_FALSE(text.empty());
    auto loaded = GrammarSchema::loadFromText(text, "<linux-on-pe>");
    ASSERT_TRUE(loaded.has_value()) << (loaded.error().empty() ? "" : loaded.error()[0].message);
    auto const& pp = (*loaded)->preprocess();
    auto const merged = mergePredefinedMacros(pp.predefinedMacros, {}, {}, ObjectFormatKind::Pe,
                                              pp.mutuallyExclusivePredefinedMacros);
    ASSERT_EQ(merged.conflicts.size(), 1u)
        << "`__linux__` beside `_WIN32` on pe must be refused, once";
    std::string const& why = merged.conflicts.front();
    EXPECT_NE(why.find("__linux__"), std::string::npos) << why;
    EXPECT_NE(why.find("_WIN32"), std::string::npos) << why;
    EXPECT_NE(why.find(objectFormatKindName(ObjectFormatKind::Pe)), std::string::npos)
        << "the refusal names the offending format: " << why;
    EXPECT_NE(why.find("two operating systems at once"), std::string::npos)
        << "the group's reason is quoted: " << why;
    EXPECT_TRUE(merged.effective.empty()) << "a refused identity leaves no usable list";
    // The same text on ELF: one family, no conflict.
    auto const elf = mergePredefinedMacros(pp.predefinedMacros, {}, {}, ObjectFormatKind::Elf,
                                           pp.mutuallyExclusivePredefinedMacros);
    EXPECT_TRUE(elf.conflicts.empty());
}

TEST(OsIdentityPredefines, RedOnDisableWithoutTheGroupTheCoDefinitionIsAccepted) {
    // The group's member list emptied of its conflicting pair: `__APPLE__` alone is a
    // one-member list the loader refuses, so the red-on-disable keeps a VALID group that
    // no longer names `__linux__` or `_WIN32`.
    std::string text = edited(shippedCText(), kLinuxGate, kLinuxGateOnPe);
    ASSERT_FALSE(text.empty());
    text = edited(std::move(text), kOsGroupMacros, "\"macros\": [\"__APPLE__\", \"__DSS_NO_SUCH_OS__\"]");
    ASSERT_FALSE(text.empty());
    auto loaded = GrammarSchema::loadFromText(text, "<linux-on-pe-no-group>");
    ASSERT_TRUE(loaded.has_value()) << (loaded.error().empty() ? "" : loaded.error()[0].message);
    auto const& pp = (*loaded)->preprocess();
    auto const merged = mergePredefinedMacros(pp.predefinedMacros, {}, {}, ObjectFormatKind::Pe,
                                              pp.mutuallyExclusivePredefinedMacros);
    EXPECT_TRUE(merged.conflicts.empty())
        << "with the group disarmed the impossible identity is silently accepted — which is "
           "why the group exists";
}

// ── THE CONTROL: A WRONG EXPECTATION IS SEEN ─────────────────────────────────
TEST(OsIdentityPredefines, AWrongExpectationIsRefused) {
    ASSERT_NE(cLanguage(), nullptr);
    auto targetR = TargetSchema::loadShipped("x86_64");
    auto formatR = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(targetR.has_value() && formatR.has_value());
    PlatformFacts wrong = *factsFor(ObjectFormatKind::Pe);
    wrong.defined.push_back("__linux__");   // no Windows reference defines it
    PairRun const r = compileForPair(cLanguage(), **targetR, **formatR, identityProbe(wrong));
    EXPECT_NE(r.preprocessErrors + r.semanticErrors, "")
        << "a wrong expectation compiled clean: the probe checks nothing";
}
