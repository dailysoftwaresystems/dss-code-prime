// D-LK-MACHO-X86-64-OBJECT-WROTE-RIP-RELATIVE-OPERANDS-AS-BRANCHES (P69 review MINOR 9) — a DSS x86_64 Mach-O
// OBJECT states each RIP-relative field by what the INSTRUCTION does with it: X86_64_RELOC_BRANCH for a call or
// jump, the X86_64_RELOC_SIGNED family for every other operand (an address `lea`, a constant load, a mask), and a
// GOT form for a load from the GOT.
//
// WHAT WAS WRONG (before P69, HEAD 71648598): `x86_64.target.json` gave `lea`, `movsd_load`, `fneg_mask` and
// `call_indirect_via_extern` the CALL relocation kind `rel32`, which `macho64-x86_64-darwin{,-staticlib}` write
// as X86_64_RELOC_BRANCH (`isCall`) — so every DSS object wrote BRANCH under instructions that branch nowhere. P69
// moved those operands to `riprel32` (the target's own role vocabulary, read by every format), which these
// documents map to X86_64_RELOC_SIGNED; nothing recorded or pinned the repair until this file.
//
//   * TIER 1 (every host): the object compiled through the production driver, every RIP-relative relocation of
//     `__text` judged against the instruction it patches — an `E8`/`E9` field is BRANCH, any other is SIGNED or a
//     GOT form, and at least one of each kind is present.
//   * TIER 2 (macOS): Apple's ld links the object into a harness and RUNS it — under Rosetta on Apple Silicon,
//     launched through the repo's one cross-arch gate (host_translations.hpp); the subject computes through every
//     such operand and exits 42 only when each value is right.

#include "core/types/diagnostic_reporter.hpp"
#include "program/program.hpp"
#include "host_translations.hpp"
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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

constexpr char const* kSubject =
    "static char buf[16] = \"abcdefghijklmno\";\n"
    "static double scale = 2.5;\n"
    "__attribute__((noinline)) static int add_one(int x) { return x + 1; }\n"
    "__attribute__((noinline)) char *buffer_address(void) { return buf; }\n"         // lea of a static
    "__attribute__((noinline)) double scaled(double v) { return -(v * scale); }\n"   // a load + a sign mask
    "int roles_check(void) {\n"
    "    if (buffer_address()[3] != 'd') return 1;\n"
    "    if (scaled(2.0) != -5.0) return 2;\n"
    "    if (add_one(41) != 42) return 3;\n"                                          // a call
    "    return 42;\n"
    "}\n";

constexpr std::uint32_t kLcSegment64 = 0x19;
constexpr std::uint32_t kBranch = 2;   // X86_64_RELOC_BRANCH
// X86_64_RELOC_SIGNED and its -1/-2/-4 variants (the bytes after the field), and the two GOT forms.
constexpr std::uint32_t kSigned[] = {1, 6, 7, 8};
constexpr std::uint32_t kGotLoad = 3, kGot = 4;

[[nodiscard]] std::uint32_t rd32(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

struct TextReloc {
    std::uint32_t address = 0, type = 0;
    bool          pcrel = false;
};
struct ObjectText {
    std::size_t            fileOffset = 0, size = 0;
    std::vector<TextReloc> relocs;
};

// (`__TEXT`, `__text`) of an MH_OBJECT: its bytes and its relocation_info rows.
[[nodiscard]] std::optional<ObjectText> objectText(std::vector<std::uint8_t> const& b) {
    if (b.size() < 32) return std::nullopt;
    std::uint32_t const ncmds = rd32(b, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds && off + 8 <= b.size(); ++i) {
        std::uint32_t const cmd = rd32(b, off), size = rd32(b, off + 4);
        if (cmd == kLcSegment64) {
            std::uint32_t const nsects = rd32(b, off + 64);
            for (std::uint32_t s = 0; s < nsects; ++s) {
                std::size_t const sec = off + 72 + s * 80u;
                std::string const sect{reinterpret_cast<char const*>(&b[sec]), 6};
                std::string const seg{reinterpret_cast<char const*>(&b[sec + 16]), 6};
                if (sect != "__text" || seg != "__TEXT") continue;
                ObjectText t;
                t.size = static_cast<std::size_t>(rd32(b, sec + 40));
                t.fileOffset = rd32(b, sec + 48);
                std::uint32_t const reloff = rd32(b, sec + 56), nreloc = rd32(b, sec + 60);
                for (std::uint32_t r = 0; r < nreloc; ++r) {
                    std::uint32_t const info = rd32(b, reloff + r * 8u + 4u);
                    t.relocs.push_back(TextReloc{rd32(b, reloff + r * 8u), (info >> 28) & 0xFu,
                                                 ((info >> 24) & 1u) != 0u});
                }
                return t;
            }
        }
        if (size == 0) break;
        off += size;
    }
    return std::nullopt;
}

[[nodiscard]] fs::path buildObject(fs::path const& dir, DiagnosticReporter& rep) {
    fs::create_directories(dir);
    { std::ofstream(dir / "roles.c", std::ios::binary) << kSubject; }
    auto const out = dir / "out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    if (p.compileFiles(std::vector<std::string>{(dir / "roles.c").string()}, "c",
                       std::vector<std::string>{"x86_64:macho64-x86_64-darwin"}, rep) != 0) {
        return {};
    }
    auto const obj = out / "roles.o";
    return fs::exists(obj) ? obj : fs::path{};
}

[[nodiscard]] std::vector<std::uint8_t> readFile(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string all;
    for (auto const& d : rep.all()) all += "\n  " + d.actual;
    return all;
}

}  // namespace

TEST(MachOX86_64RipRelativeRoles, EveryRipRelativeFieldIsStatedByWhatItsInstructionDoes) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "macho-rip-roles"};
    DiagnosticReporter rep;
    auto const obj = buildObject(scratch.path(), rep);
    ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
    auto const bytes = readFile(obj);
    auto const text = objectText(bytes);
    ASSERT_TRUE(text.has_value()) << "the object carries (__TEXT,__text)";
    std::size_t branches = 0, signedOperands = 0;
    for (auto const& r : text->relocs) {
        if (!r.pcrel) continue;
        ASSERT_GE(r.address, 1u);
        std::uint8_t const opcode = bytes[text->fileOffset + r.address - 1];
        SCOPED_TRACE(::testing::Message() << "relocation at __text+0x" << std::hex << r.address);
        if (opcode == 0xE8 || opcode == 0xE9) {
            EXPECT_EQ(r.type, kBranch) << "a call or jump field is a BRANCH";
            ++branches;
            continue;
        }
        bool const isSigned = std::find(std::begin(kSigned), std::end(kSigned), r.type) != std::end(kSigned);
        EXPECT_TRUE(isSigned || r.type == kGotLoad || r.type == kGot)
            << "an operand that branches nowhere (r_type " << r.type << ") must be SIGNED or a GOT form — "
               "BRANCH under a `lea` tells ld a branch is there";
        signedOperands += isSigned ? 1u : 0u;
    }
    EXPECT_GE(branches, 1u) << "`add_one(41)` is a call";
    EXPECT_GE(signedOperands, 2u) << "the `lea` of `buf` and the load of `scale` are operands";
}

TEST(MachOX86_64RipRelativeRolesNative, AppleLdLinksAndRunsTheObject) {
#if !defined(__APPLE__)
    GTEST_SKIP() << "Apple's ld runs on macOS only";
#else
    test_support::ScratchDir scratch{test_support::Location::Temp, "macho-rip-roles-native"};
    auto const dir = scratch.path();
    DiagnosticReporter rep;
    auto const obj = buildObject(dir / "dss", rep);
    ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
    { std::ofstream(dir / "main.c", std::ios::binary) << "int roles_check(void);\nint main(void) { return roles_check(); }\n"; }
    auto const exe = dir / "roles";
    auto const log = dir / "link.txt";
    std::string const link = "cc -arch x86_64 -o \"" + exe.string() + "\" \"" + (dir / "main.c").string() + "\" \""
                           + obj.string() + "\"";
    int const rc = std::system(test_support::native_probe::captureCmd(link, log).c_str());
    if (rc != 0) {
        // Only a missing toolchain is a skip: a shim with nothing behind it cannot build a trivial program either.
        { std::ofstream(dir / "check.c", std::ios::binary) << "int main(void){return 0;}\n"; }
        if (std::system(("cc -arch x86_64 -o \"" + (dir / "check").string() + "\" \"" + (dir / "check.c").string()
                         + "\" >/dev/null 2>&1")
                            .c_str())
            != 0) {
            GTEST_SKIP() << "no x86_64-capable Apple toolchain on this Mac";
        }
    }
    ASSERT_EQ(rc, 0) << "Apple's ld refused DSS's x86_64 object" << test_support::native_probe::tailOf(log, 20);
    // The RUN goes through the repo's one cross-arch gate (host_translations.hpp): natively on an x86_64 Mac, under
    // the translation this host declares (Rosetta 2, `arch -x86_64`) on Apple Silicon, and otherwise not at all —
    // a skip that names what is missing, red under DSS_STRICT_ARM_VERDICTS when the machine lacks it.
    std::vector<std::string> launcher;
    bool launcherExecsImage = false;
    if (test_support::currentHostArch() != "x86_64") {
        auto const strict = test_support::readStrictArmVerdicts();
        ASSERT_FALSE(strict.malformed) << test_support::kStrictArmVerdictsEnv << "='" << strict.raw
                                       << "' is not a recognised value";
        auto const gate = test_support::crossArchDecisionForThisHost("x86_64", /*manifestEmulator=*/"");
        if (!gate.runs) {
            if (strict.on && test_support::armVerdictIsEnvironmentalSkip(gate.skip)) FAIL() << gate.why;
            GTEST_SKIP() << gate.why;
        }
        launcher           = gate.launcherPrefix;
        launcherExecsImage = gate.launcherExecsImage;
    }
    auto const r = test_support::runBinary(exe, test_support::kRunBudget, /*captureStdout=*/false, launcher,
                                           /*programArgs=*/{}, launcherExecsImage);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u) << "1 the lea of a static, 2 the constant load and its mask, 3 the call";
#endif
}
