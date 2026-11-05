// D-LK-MEMBER-UNTYPED-EXTERN-TAKEN-AS-DATA (P69 round 3) — an archive member built by the HOST'S OWN reference
// compiler that reaches a C-library FUNCTION only through the GOT, or only by its address, states no code-vs-data
// kind, and DSS takes the kind from the definition, as ld does.
//
// WHAT WAS WRONG: gcc, clang and DSS's own writer give every undefined ELF symbol STT_NOTYPE, and a Mach-O nlist
// carries no type at all, so such a symbol states its kind only through a CALL relocation (x86_64 PLT32, aarch64
// CALL26, Mach-O BRANCH). The ELF and Mach-O object readers took every other untyped symbol as DATA, so a member
// whose `-fno-plt` call reaches `fputs` through the GOT (`call *fputs@GOTPCREL(%rip)`, R_X86_64_GOTPCRELX; aarch64 a
// GOT pair and `blr`) or that takes `&fputs` (a GOT load everywhere) minted a DATA import, and a program declaring
// `fputs` was refused: ✔MEASURED 2026-09-24, gcc 13.3 `-O2 -fno-plt` members, ELF x86_64 and aarch64, exec and PIE,
// all 12 arms `K_ExternImportAttributeConflict` ("conflicting `isData` ... (false vs true)"), while gcc -no-pie and
// -pie linked the same members to 42. Since P69 round 3 the readers mint such a symbol `Pending`, the archive-member
// binder decides it from the library row that defines it (`decideKindFromTheDefinition`), and the link's import
// dedup adopts the program's own statement.
//
// THE NATIVE WITNESS. Three members, one reference shape each, compiled by the host's `cc` and archived by its `ar`:
//   * `calls.c`   — `fputs(s, stdout)`: under `-fno-plt` an ELF call through the GOT; on Mach-O a BRANCH (clang
//                   keeps the stub call), so there the call states the kind;
//   * `address.c` — `return fputs;`: the address only, a GOT load on every format;
//   * `datum.c`   — `return stdout;`: a library DATUM through the GOT (`-fPIC`), which must still link.
// A DSS `main.c` that CALLS `fputs` links the archive and compares every value with its own; exit 42, stdout
// "member\nmain\n".
//   * Linux (gcc + GNU ar): the host's ISA, exec and PIE — x86_64 on the WSL leg, aarch64 on the VPS leg.
//   * macOS (Apple clang + ar): arm64 natively and x86_64 run through the repo's one cross-arch gate
//     (`crossArchDecisionForThisHost`: Rosetta 2 under `arch -x86_64` on Apple Silicon).
//   * Windows: no ELF or Mach-O reference compiler — skipped, naming it (its COFF members carry a type,
//     `test_coff_object_reader.cpp`).
// The pin that a `Pending` row no definition decides is still refused by name is
// `ImportKindFromDefinition.ALibraryThatStatesNoKindIsRefusedByName`.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"
#include "host_translations.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

constexpr char const* kCalls =
    "#include <stdio.h>\n"
    "int member_calls_fputs(const char *s) { return fputs(s, stdout); }\n";
constexpr char const* kAddress =
    "#include <stdio.h>\n"
    "typedef int (*fputs_fn)(const char *, FILE *);\n"
    "fputs_fn member_fputs_address(void) { return fputs; }\n";
constexpr char const* kDatum =
    "#include <stdio.h>\n"
    "FILE *member_stdout(void) { return stdout; }\n";

// The program DSS compiles: it CALLS `fputs` (a stated function), so a member row that read as data conflicts.
constexpr char const* kMain =
    "#include <stdio.h>\n"
    "typedef int (*fputs_fn)(const char *, FILE *);\n"
    "int member_calls_fputs(const char *s);\n"
    "fputs_fn member_fputs_address(void);\n"
    "FILE *member_stdout(void);\n"
    "int main(void) {\n"
    "    int bad = 0;\n"
    "    if (member_calls_fputs(\"member\\n\") < 0) bad |= 1;\n"
    "    if (member_fputs_address() != fputs) bad |= 2;\n"
    "    if (member_stdout() != stdout) bad |= 4;\n"
    "    if (fputs(\"main\\n\", stdout) < 0) bad |= 8;\n"
    "    fflush(stdout);\n"
    "    return bad == 0 ? 42 : 100 + bad;\n"
    "}\n";

constexpr char const* kBadBits =
    "exit 100 + bits: 1 the member's call of fputs failed, 2 the member's &fputs != the program's, 4 the member's "
    "stdout != the program's, 8 the program's own call failed";

struct NativeArm {
    std::string label;
    std::string memberFlags;            // the host compiler's flags for the three members
    std::vector<char const*> specs;     // the DSS images the archive is linked into
};

[[nodiscard]] std::vector<NativeArm> nativeArms() {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#if defined(__x86_64__)
    return {{"gcc -fno-plt (x86_64)", "-O2 -fPIC -fno-plt",
             {"x86_64:elf64-x86_64-linux-exec", "x86_64:elf64-x86_64-linux-pie"}}};
#else
    return {{"gcc -fno-plt (aarch64)", "-O2 -fPIC -fno-plt",
             {"arm64:elf64-aarch64-linux-exec", "arm64:elf64-aarch64-linux-pie"}}};
#endif
#elif defined(__APPLE__)
    return {{"Apple clang -arch arm64", "-arch arm64 -O2", {"arm64:macho64-arm64-darwin-exec"}},
            {"Apple clang -arch x86_64", "-arch x86_64 -O2", {"x86_64:macho64-x86_64-darwin-exec"}}};
#else
    return {};
#endif
}

void writeText(fs::path const& p, std::string_view text) {
    std::ofstream(p, std::ios::binary) << text;
}

[[nodiscard]] int runCapturing(std::string const& cmd, fs::path const& log) {
    return std::system(test_support::native_probe::captureCmd(cmd, log).c_str());
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string all;
    for (auto const& d : rep.all()) all += "\n  " + d.actual;
    return all;
}

}  // namespace

TEST(MemberUntypedExternKindNative, AMemberThatNamesALibraryFunctionOnlyThroughTheGotLinksUnderADssMain) {
    auto const arms = nativeArms();
    if (arms.empty()) {
        GTEST_SKIP() << "no ELF or Mach-O reference compiler on this host (a COFF member's symbols carry a type)";
    }
    test_support::ScratchDir scratch{test_support::Location::Temp, "member-untyped-extern"};
    auto const dir = scratch.path();
    writeText(dir / "check.c", "int main(void){return 0;}\n");
    if (runCapturing("cc -o \"" + (dir / "check").string() + "\" \"" + (dir / "check.c").string() + "\"",
                     dir / "check.txt")
        != 0) {
        GTEST_SKIP() << "the host's `cc` cannot build a trivial program: "
                     << test_support::native_probe::tailOf(dir / "check.txt", 5);
    }
    writeText(dir / "calls.c", kCalls);
    writeText(dir / "address.c", kAddress);
    writeText(dir / "datum.c", kDatum);
    writeText(dir / "main.c", kMain);
    auto const strict = test_support::readStrictArmVerdicts();
    ASSERT_FALSE(strict.malformed) << test_support::kStrictArmVerdictsEnv << "='" << strict.raw
                                   << "' is not a recognised value";
    std::size_t ran = 0;
    for (std::size_t k = 0; k < arms.size(); ++k) {
        auto const& arm = arms[k];
        SCOPED_TRACE(arm.label);
        fs::path const armDir = dir / ("arm" + std::to_string(k));
        fs::create_directories(armDir);
        // The members, by the host's reference compiler, and their archive, by the host's `ar`.
        std::string objects;
        for (char const* member : {"calls", "address", "datum"}) {
            fs::path const obj = armDir / (std::string{member} + ".o");
            std::string const cc = "cc " + arm.memberFlags + " -c -o \"" + obj.string() + "\" \""
                                 + (dir / (std::string{member} + ".c")).string() + "\"";
            ASSERT_EQ(runCapturing(cc, armDir / (std::string{member} + ".txt")), 0)
                << cc << "\n" << test_support::native_probe::tailOf(armDir / (std::string{member} + ".txt"), 10);
            objects += " \"" + obj.string() + "\"";
        }
        fs::path const archive = armDir / "libmembers.a";
        std::string const ar = "ar rcs \"" + archive.string() + "\"" + objects;
        ASSERT_EQ(runCapturing(ar, armDir / "ar.txt"), 0) << ar << "\n"
                                                          << test_support::native_probe::tailOf(armDir / "ar.txt", 10);
        for (char const* spec : arm.specs) {
            SCOPED_TRACE(spec);
            // How THIS host runs the image: natively, or through the one cross-arch gate.
            std::vector<std::string> launcher;
            bool launcherExecsImage = false;
            if (std::string const arch = test_support::specTargetArch(spec);
                arch != test_support::currentHostArch()) {
                auto const gate = test_support::crossArchDecisionForThisHost(arch, /*manifestEmulator=*/"");
                if (!gate.runs) {
                    if (strict.on && test_support::armVerdictIsEnvironmentalSkip(gate.skip)) {
                        ADD_FAILURE() << gate.why;
                    } else {
                        std::cout << "[native-arm] " << arm.label << " " << spec << ": not run — " << gate.why
                                  << "\n";
                    }
                    continue;
                }
                launcher           = gate.launcherPrefix;
                launcherExecsImage = gate.launcherExecsImage;
            }
            fs::path const out = armDir / ("out-" + std::string{test_support::specFormatName(spec)});
            fs::create_directories(out);
            Program p;
            p.setOutputDir(out);
            p.setResolveLibraries(std::vector<fs::path>{archive});
            DiagnosticReporter rep;
            int const rc = p.compileFiles(std::vector<std::string>{(dir / "main.c").string()}, "c",
                                          std::vector<std::string>{spec}, rep);
            bool conflict = false;
            for (auto const& d : rep.all()) {
                if (d.code == DiagnosticCode::K_ExternImportAttributeConflict) conflict = true;
            }
            EXPECT_FALSE(conflict) << "the member's untyped `fputs` was read as a statement:" << diagnosticsOf(rep);
            ASSERT_EQ(rc, 0) << diagnosticsOf(rep);
            fs::path const exe = out / "main";
            ASSERT_TRUE(fs::exists(exe)) << diagnosticsOf(rep);
            auto const r = test_support::runBinary(exe, test_support::kRunBudget, /*captureStdout=*/true, launcher,
                                                   /*programArgs=*/{}, launcherExecsImage);
            ASSERT_TRUE(r.spawned) << r.diagnostic;
            EXPECT_FALSE(r.timedOut);
            EXPECT_EQ(r.exitCode, 42u) << r.capturedStdout << "\n" << kBadBits;
            EXPECT_EQ(r.capturedStdout, "member\nmain\n");
            std::cout << "[native-arm] " << arm.label << " " << spec << ": linked by DSS, exit " << r.exitCode
                      << "\n";
            ++ran;
        }
    }
    if (ran == 0 && !HasFailure()) GTEST_SKIP() << "no arm could run on this host (see the [native-arm] lines)";
}
