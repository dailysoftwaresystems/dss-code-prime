// POSIX's <unistd.h> typedefs, per pair (P69, D-C-UNISTD-H-LACKS-SEVEN-POSIX-TYPEDEFS).
//
// POSIX's <unistd.h> defines size_t, ssize_t, off_t, pid_t, uid_t, gid_t and intptr_t, and
// DSS's refused all seven on ELF and Mach-O and six on pe (✔MEASURED P69). Each is now a
// REFERENCE to its one owner (stddef.h, sys/types.h, stdint.h). This file compiles, for every
// executable pair, a probe that includes <unistd.h> ALONE and asserts each type's identity
// with _Generic — the C type, not merely its width, so `long` and `long long` stay apart on
// LP64 — against the references' measurements:
//   * glibc 2.39 (gcc/clang default mode, both arches): size_t unsigned long, ssize_t long,
//     off_t long, pid_t int, uid_t/gid_t unsigned int, intptr_t long;
//   * Apple clang 21 (both arches): the same, but off_t `long long`;
//   * mingw-w64 13.2.0 (the pe identity): size_t unsigned long long, ssize_t long long, off_t
//     `long` (4 bytes), pid_t `long long` (8 bytes), intptr_t long long; NO uid_t/gid_t.
// pe's off_t and pid_t were WIDTH divergences in sys/types.json (`i64` and a flat `i32`),
// corrected in the same change.
//
// RED-ON-DISABLE: drop one `shippedTypedef` row from unistd.json and every pair's probe fails
// to compile (the name is undeclared); restore pe's old off_t `i64` and the pe probe fails its
// _Static_assert.

#include "core/types/diagnostic_reporter.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

// _Generic codes: 1 int, 2 unsigned int, 3 long, 4 unsigned long, 5 long long, 6 unsigned long long.
struct Pair {
    char const* spec;
    int         size, ssize, off, pid, intptr;
    int         offBytes, pidBytes;
    bool        hasIds;   // uid_t / gid_t declared by the pair's <unistd.h>
};

constexpr Pair kPairs[] = {
    {"x86_64:pe64-x86_64-windows-exec", 6, 5, 3, 5, 5, 4, 8, false},
    {"x86_64:elf64-x86_64-linux-exec", 4, 3, 3, 1, 3, 8, 4, true},
    {"arm64:elf64-aarch64-linux-exec", 4, 3, 3, 1, 3, 8, 4, true},
    {"arm64:macho64-arm64-darwin-exec", 4, 3, 5, 1, 3, 8, 4, true},
    {"x86_64:macho64-x86_64-darwin-exec", 4, 3, 5, 1, 3, 8, 4, true},
};

[[nodiscard]] std::string probeFor(Pair const& p) {
    return std::format(
        "#include <unistd.h>\n"
        "#define KIND(T) _Generic((T)0, int: 1, unsigned int: 2, long: 3, unsigned long: 4, \\\n"
        "                        long long: 5, unsigned long long: 6, default: 0)\n"
        "_Static_assert(KIND(size_t) == {0}, \"size_t\");\n"
        "_Static_assert(KIND(ssize_t) == {1}, \"ssize_t\");\n"
        "_Static_assert(KIND(off_t) == {2}, \"off_t\");\n"
        "_Static_assert(KIND(pid_t) == {3}, \"pid_t\");\n"
        "_Static_assert(KIND(intptr_t) == {4}, \"intptr_t\");\n"
        "_Static_assert(sizeof(off_t) == {5}, \"off_t width\");\n"
        "_Static_assert(sizeof(pid_t) == {6}, \"pid_t width\");\n"
        "{7}"
        "int main(void) {{ return 0; }}\n",
        p.size, p.ssize, p.off, p.pid, p.intptr, p.offBytes, p.pidBytes,
        p.hasIds ? "_Static_assert(KIND(uid_t) == 2, \"uid_t\");\n"
                   "_Static_assert(KIND(gid_t) == 2, \"gid_t\");\n"
                 : "");
}

}  // namespace

TEST(UnistdHTypedefs, EveryPairDeclaresTheReferencesTypesFromUnistdHAlone) {
    for (Pair const& p : kPairs) {
        SCOPED_TRACE(p.spec);
        ScratchDir scratch{Location::InsideRepo, "unistd-typedefs"};
        fs::path const src = scratch.path() / "probe.c";
        std::ofstream(src, std::ios::binary) << probeFor(p);
        DiagnosticReporter rep;
        Program prog;
        prog.setOutputDir(scratch.path());
        int const rc = prog.compileFiles(std::vector<std::string>{src.string()}, "c",
                                         std::vector<std::string>{p.spec}, rep);
        EXPECT_EQ(rc, 0) << (rep.all().empty() ? std::string{} : rep.all().front().actual);
    }
}

// The negative half: pe's <unistd.h> declares no uid_t or gid_t (mingw-w64's has none, and
// MSVC ships no <unistd.h>), so a program that names them as TYPES after including it alone is
// refused there — while the same program compiles on every pair whose reference declares them.
// (An OBJECT named uid_t does not discriminate: DSS lets a user's declaration of a name win over
// the shipped one on every pair, the TF-C89 skip.)
TEST(UnistdHTypedefs, PeDeclaresNoUidTOrGidT) {
    constexpr char const* kSource =
        "#include <unistd.h>\nuid_t u = 1;\ngid_t g = 2;\nint main(void) { return (int)(u + g) - 3; }\n";
    auto compileOn = [&](char const* spec, char const* scratchName) {
        ScratchDir scratch{Location::InsideRepo, scratchName};
        fs::path const src = scratch.path() / "probe.c";
        std::ofstream(src, std::ios::binary) << kSource;
        DiagnosticReporter rep;
        Program prog;
        prog.setOutputDir(scratch.path());
        return prog.compileFiles(std::vector<std::string>{src.string()}, "c",
                                 std::vector<std::string>{spec}, rep);
    };
    EXPECT_NE(compileOn("x86_64:pe64-x86_64-windows-exec", "unistd-typedefs-pe"), 0)
        << "pe's <unistd.h> must not claim uid_t/gid_t — no pe reference declares them";
    // CONTROL: the same program where the references declare both.
    EXPECT_EQ(compileOn("x86_64:elf64-x86_64-linux-exec", "unistd-typedefs-elf"), 0)
        << "on ELF <unistd.h> declares uid_t and gid_t (glibc), so the program compiles";
}
