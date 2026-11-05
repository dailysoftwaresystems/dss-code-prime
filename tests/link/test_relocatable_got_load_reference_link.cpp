// P69 review M4 (D-LK-LIBRARY-FUNCTION-ADDRESS-IS-THE-IMAGE-STUB, and half (2) of
// D-LK-MEMBER-DIRECT-LIBRARY-DATUM-BOUND-TO-THE-SLOT) — DSS's RELOCATABLE objects take a library function's
// and a library datum's ADDRESS by a GOT load (`externAddrBinding: got`: R_X86_64_REX_GOTPCRELX,
// R_AARCH64_ADR_GOT_PAGE + R_AARCH64_LD64_GOT_LO12_NC, ARM64_RELOC_GOT_LOAD_PAGE21 + PAGEOFF12,
// X86_64_RELOC_GOT_LOAD), and that encoding is only as good as what the REFERENCE linkers make of it. Every
// earlier witness was DSS reading DSS; here the host's own linker consumes the object.
//
// TWO TIERS, NEITHER REPLACES THE OTHER (the shape of `test_pe_object_unwind_reference_link.cpp` and
// `test_macho_ld64_local_collision.cpp`):
//   * TIER 1 (`RelocatableGotLoadReferenceLink`) — HOST-INDEPENDENT. Compiles the subject through the
//     production driver for the four relocatable formats and reads each object back: every relocation of the
//     two address-taking functions naming the import is a GOT-slot-relative kind, and the static initializer
//     is the target's absolute pointer relocation. That is the encoding tier 2 hands to a foreign linker.
//   * TIER 2 (`RelocatableGotLoadReferenceLinkNative`) — the RUNTIME witness, on the host whose linker it is:
//     gcc with GNU ld, `-no-pie` and `-pie`, on Linux x86_64 (WSL) and Linux aarch64 (the VPS); Apple's ld on
//     the Mac, `-arch arm64` natively and `-arch x86_64` under Rosetta, which the repo's one cross-arch gate
//     launches (`crossArchDecisionForThisHost`, host_translations.hpp). The harness, compiled by that host's
//     own `cc`, compares every value the DSS object hands it with what `dlsym(RTLD_DEFAULT, ...)` answers,
//     and with its own `&puts` / `&stdout`. A host with no such linker SKIPS, naming it; Windows has none
//     (its COFF half is `test_pe_foreign_import_address`).

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/object_format_schema.hpp"
#include "link/pointer_reloc.hpp"
#include "program/program.hpp"
#include "host_translations.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
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

// The subject DSS compiles. Every form a relocatable object can hold an import's address in, plus a call.
constexpr char const* kSubject =
    "#include <stdio.h>\n"
    "typedef int (*put_fn)(const char *);\n"
    "put_fn dss_static_puts = puts;\n"                                                  // data: absolute pointer
    "__attribute__((noinline)) put_fn dss_code_puts(void) { return puts; }\n"            // code: a GOT load
    "__attribute__((noinline)) FILE **dss_stdout_address(void) { return &stdout; }\n"   // a datum's address
    "__attribute__((noinline)) FILE *dss_stdout_value(void) { return stdout; }\n"       // through the GOT, read
    "int dss_calls_puts(void) { return puts(\"dss object\") >= 0; }\n";                 // a call

// The harness the HOST's compiler builds. It prints which checks failed and exits 42 only when none did (a
// bitset exit would be ambiguous: 2|8|32 is 42).
constexpr char const* kHarness =
    "#define _GNU_SOURCE 1\n"
    "#include <dlfcn.h>\n"
    "#include <stdio.h>\n"
    "typedef int (*put_fn)(const char *);\n"
    "extern put_fn dss_static_puts;\n"
    "put_fn dss_code_puts(void);\n"
    "FILE **dss_stdout_address(void);\n"
    "FILE *dss_stdout_value(void);\n"
    "int dss_calls_puts(void);\n"
    "#if defined(__APPLE__)\n"
    "#define STDOUT_NAME \"__stdoutp\"\n"
    "#else\n"
    "#define STDOUT_NAME \"stdout\"\n"
    "#endif\n"
    "int main(void) {\n"
    "    void *want_puts = dlsym(RTLD_DEFAULT, \"puts\");\n"
    "    void *want_stdout = dlsym(RTLD_DEFAULT, STDOUT_NAME);\n"
    "    int bad = 0;\n"
    "    if (want_puts == 0 || want_stdout == 0) bad |= 1;\n"
    "    if ((void *)dss_code_puts() != want_puts) bad |= 2;\n"
    "    if ((void *)dss_static_puts != want_puts) bad |= 4;\n"
    "    if ((void *)dss_code_puts() != (void *)puts) bad |= 8;\n"
    "    if ((void *)dss_stdout_address() != want_stdout) bad |= 16;\n"
    "    if ((void *)dss_stdout_address() != (void *)&stdout) bad |= 32;\n"
    "    if (dss_stdout_value() != stdout) bad |= 64;\n"
    "    if (!dss_calls_puts()) bad |= 128;\n"
    "    printf(\"bad=%d\\n\", bad);\n"
    "    fflush(stdout);\n"
    "    return bad == 0 ? 42 : 1;\n"
    "}\n";

constexpr char const* kBadBits =
    "bad= bits: 1 a dlsym lookup failed, 2 the object's code &puts != dlsym, 4 its static != dlsym, 8 its code "
    "&puts != the harness's own &puts, 16 its &stdout != dlsym, 32 its &stdout != the harness's, 64 the value it "
    "read through the GOT != the harness's stdout, 128 its call failed";

void writeText(fs::path const& p, std::string_view text) {
    std::ofstream(p, std::ios::binary) << text;
}

[[nodiscard]] std::vector<std::uint8_t> readFile(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Compile the subject to a relocatable object of `spec` through the production driver. Empty on failure; the
// diagnostics land in `rep`.
[[nodiscard]] fs::path buildObject(fs::path const& dir, std::string const& spec, DiagnosticReporter& rep) {
    fs::create_directories(dir);
    auto const src = dir / "subject.c";
    writeText(src, kSubject);
    auto const out = dir / "out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    int const rc = p.compileFiles(std::vector<std::string>{src.string()}, "c", std::vector<std::string>{spec},
                                  rep);
    if (rc != 0) return {};
    auto const obj = out / "subject.o";
    return fs::exists(obj) ? obj : fs::path{};
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string all;
    for (auto const& d : rep.all()) all += "\n  " + d.actual;
    return all;
}

struct Relocatable {
    char const* spec;     // the DSS target spec of the object
    char const* target;   // its target document
    char const* format;   // its relocatable format document
};

constexpr Relocatable kRelocatables[] = {
    {"x86_64:elf64-x86_64-linux", "x86_64", "elf64-x86_64-linux"},
    {"arm64:elf64-aarch64-linux", "arm64", "elf64-aarch64-linux"},
    {"arm64:macho64-arm64-darwin", "arm64", "macho64-arm64-darwin"},
    {"x86_64:macho64-x86_64-darwin", "x86_64", "macho64-x86_64-darwin"},
};

// A Mach-O object spells C names with a leading underscore; an ELF one does not.
[[nodiscard]] bool namesC(std::string_view symbol, std::string_view cName) {
    if (symbol == cName) return true;
    return symbol.size() == cName.size() + 1 && symbol.front() == '_' && symbol.substr(1) == cName;
}

}  // namespace

// ── TIER 1: what the object says, on every host ─────────────────────────────

TEST(RelocatableGotLoadReferenceLink, EveryAddressOfAnImportInADssObjectIsAGotLoad) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "got-load-ref-link"};
    for (auto const& r : kRelocatables) {
        SCOPED_TRACE(r.spec);
        auto tgt = TargetSchema::loadShipped(r.target);
        auto fmt = ObjectFormatSchema::loadShipped(r.format);
        ASSERT_TRUE(tgt.has_value() && fmt.has_value());
        ASSERT_TRUE((*fmt)->externAddrBinding() == ExternAddrBinding::Got)
            << "the subject of this witness: the relocatable document declares externAddrBinding: got";
        DiagnosticReporter rep;
        auto const obj = buildObject(scratch.path() / r.format, r.spec, rep);
        ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
        auto const bytes = readFile(obj);
        DiagnosticReporter readRep;
        std::optional<AssembledModule> const m =
            std::string_view{r.format}.starts_with("elf")
                ? elf::readRelocatableObject(bytes, **tgt, **fmt, readRep)
                : macho::readRelocatableObject(bytes, **tgt, **fmt, readRep);
        ASSERT_TRUE(m.has_value()) << diagnosticsOf(readRep);
        auto const nameOf = [&](SymbolId id) -> std::string {
            for (auto const& s : m->symbols) {
                if (s.symbol == id) return s.name;
            }
            for (auto const& e : m->externImports) {
                if (e.symbol == id) return e.mangledName;
            }
            return {};
        };
        auto const gotRelative = [&](Relocation const& rel) {
            auto const* tri = (*tgt)->relocationInfo(rel.kind);
            return tri != nullptr && relocFormulaFacts(tri->formulaKind).isGotSlotRelative;
        };
        // The two address-taking functions: every relocation naming the import is a GOT load.
        struct Taker {
            char const* function;
            char const* import;
        };
        for (Taker const t : {Taker{"dss_code_puts", "puts"}, Taker{"dss_stdout_address", "stdout"}}) {
            SCOPED_TRACE(t.function);
            AssembledFunction const* fn = nullptr;
            for (auto const& f : m->functions) {
                if (namesC(nameOf(f.symbol), t.function)) fn = &f;
            }
            ASSERT_NE(fn, nullptr) << "the object defines the function";
            std::size_t naming = 0;
            for (auto const& rel : fn->relocations) {
                std::string const target = nameOf(rel.target);
                // Darwin's <stdio.h> spells `stdout` as `__stdoutp`.
                bool const isImport = namesC(target, t.import)
                                   || (std::string_view{t.import} == "stdout" && namesC(target, "__stdoutp"));
                if (!isImport) continue;
                ++naming;
                EXPECT_TRUE(gotRelative(rel)) << "a reference to '" << target << "' that is not a GOT load";
            }
            EXPECT_GE(naming, 1u) << "the function names the import";
        }
        // The static initializer: one absolute pointer relocation to `puts`.
        AssembledData const* item = nullptr;
        for (auto const& d : m->dataItems) {
            if (namesC(nameOf(d.symbol), "dss_static_puts")) item = &d;
        }
        ASSERT_NE(item, nullptr);
        ASSERT_EQ(item->relocations.size(), 1u);
        EXPECT_TRUE(namesC(nameOf(item->relocations[0].target), "puts"));
        auto const absPtr = linker::absolutePointerRelocKind(**tgt, 8);
        ASSERT_TRUE(absPtr.has_value());
        EXPECT_EQ(item->relocations[0].kind, *absPtr);
    }
}

// ── TIER 2: what the host's own linker makes of it ──────────────────────────

namespace {

// An arm whose ISA is not the host's RUNS through the repo's one cross-arch gate (`crossArchDecisionForThisHost`,
// host_translations.hpp): the host's own translation (Rosetta 2 under `arch -x86_64` on Apple Silicon), launched by
// the command that table declares, or a skip that names what is missing — red under DSS_STRICT_ARM_VERDICTS when the
// machine, not the declaration, is what lacks it. No arm says "translated" itself.
struct NativeArm {
    std::string label;
    char const* spec;           // the object DSS writes
    std::string linkFlags;      // the reference link of the harness and the object
};

[[nodiscard]] std::vector<NativeArm> nativeArms() {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#if defined(__x86_64__)
    char const* const spec = "x86_64:elf64-x86_64-linux";
    std::string const isa = "x86_64";
#else
    char const* const spec = "arm64:elf64-aarch64-linux";
    std::string const isa = "aarch64";
#endif
    std::vector<NativeArm> arms{{"GNU ld -no-pie (" + isa + ")", spec, "-fno-pie -no-pie"},
                                {"GNU ld -pie (" + isa + ")", spec, "-fPIE -pie"}};
    // lld where the host has it (the second reference linker the relocatable documents' comments name).
    if (std::system("command -v ld.lld >/dev/null 2>&1") == 0) {
        arms.push_back({"ld.lld -no-pie (" + isa + ")", spec, "-fuse-ld=lld -fno-pie -no-pie"});
        arms.push_back({"ld.lld -pie (" + isa + ")", spec, "-fuse-ld=lld -fPIE -pie"});
    }
    return arms;
#elif defined(__APPLE__)
    return {{"Apple ld -arch arm64", "arm64:macho64-arm64-darwin", "-arch arm64"},
            {"Apple ld -arch x86_64", "x86_64:macho64-x86_64-darwin", "-arch x86_64"}};
#else
    return {};
#endif
}

[[nodiscard]] int runCapturing(std::string const& cmd, fs::path const& log) {
    return std::system(test_support::native_probe::captureCmd(cmd, log).c_str());
}

}  // namespace

TEST(RelocatableGotLoadReferenceLinkNative, TheHostsLinkerBindsEveryGotLoadToWhatDlsymAnswers) {
    auto const arms = nativeArms();
    if (arms.empty()) {
        GTEST_SKIP() << "no ELF or Mach-O reference linker on this host (its COFF half is "
                        "test_pe_foreign_import_address)";
    }
    test_support::ScratchDir scratch{test_support::Location::Temp, "got-load-ref-link-native"};
    auto const dir = scratch.path();
    // The host's own C compiler — a shim with no toolchain behind it is ABSENT, not broken.
    writeText(dir / "check.c", "int main(void){return 0;}\n");
    if (runCapturing("cc -o \"" + (dir / "check").string() + "\" \"" + (dir / "check.c").string() + "\"",
                     dir / "check.txt")
        != 0) {
        GTEST_SKIP() << "the host's `cc` cannot build a trivial program: "
                     << test_support::native_probe::tailOf(dir / "check.txt", 5);
    }
    writeText(dir / "main.c", kHarness);
    auto const strict = test_support::readStrictArmVerdicts();
    ASSERT_FALSE(strict.malformed) << test_support::kStrictArmVerdictsEnv << "='" << strict.raw
                                   << "' is not a recognised value";
    for (std::size_t k = 0; k < arms.size(); ++k) {
        auto const& arm = arms[k];
        SCOPED_TRACE(arm.label);
        // How THIS host runs the arm's image: natively, or through the one cross-arch gate.
        std::vector<std::string> launcher;
        bool launcherExecsImage = false;
        if (std::string const arch = test_support::specTargetArch(arm.spec);
            arch != test_support::currentHostArch()) {
            auto const gate = test_support::crossArchDecisionForThisHost(arch, /*manifestEmulator=*/"");
            if (!gate.runs) {
                if (strict.on && test_support::armVerdictIsEnvironmentalSkip(gate.skip)) {
                    ADD_FAILURE() << arm.label << ": " << gate.why;
                } else {
                    std::cout << "[native-arm] " << arm.label << ": not run — " << gate.why << "\n";
                }
                continue;
            }
            launcher           = gate.launcherPrefix;
            launcherExecsImage = gate.launcherExecsImage;
        }
        DiagnosticReporter rep;
        fs::path const armDir = dir / ("arm" + std::to_string(k));
        auto const obj = buildObject(armDir, arm.spec, rep);
        ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
        fs::path const exe = armDir / "prog";
        fs::path const linkLog = armDir / "link.txt";
#if defined(__APPLE__)
        std::string const libs;
#else
        std::string const libs = " -ldl";
#endif
        std::string const link = "cc " + arm.linkFlags + " -O1 -o \"" + exe.string() + "\" \""
                               + (dir / "main.c").string() + "\" \"" + obj.string() + "\"" + libs;
        ASSERT_EQ(runCapturing(link, linkLog), 0)
            << "the reference linker refused DSS's object: " << link << "\n"
            << test_support::native_probe::tailOf(linkLog, 20);
        auto const r = test_support::runBinary(exe, test_support::kRunBudget, /*captureStdout=*/true, launcher,
                                               /*programArgs=*/{}, launcherExecsImage);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << r.capturedStdout << "\n" << kBadBits;
        EXPECT_NE(r.capturedStdout.find("dss object"), std::string::npos) << r.capturedStdout;
        EXPECT_NE(r.capturedStdout.find("bad=0"), std::string::npos) << r.capturedStdout;
        // Name every arm that RAN in the passing log too, so a run's record says which reference linkers
        // consumed the object (the lld arms exist only where the host has ld.lld).
        std::cout << "[native-arm] " << arm.label << ": linked by the reference linker, exit " << r.exitCode
                  << "\n";
    }
}
