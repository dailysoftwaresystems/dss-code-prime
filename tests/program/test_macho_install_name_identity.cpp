// ══ D-LK-MACHO-DYLIB-INSTALL-NAME-IS-ONE-CONSTANT-FOR-EVERY-ARTIFACT ═══════
//
// THE DRIVER-TIER PIN, and it exists because two pins one tier away were both
// TRUE ANSWERS TO AN ADJACENT QUESTION.
//
//   * `ImageLibraryDeps.EveryResolvedLibraryIsRecordedOnEveryFormat`
//     (tests/link/) hand-builds a MIR module carrying two ALREADY-DISTINCT
//     `libraryPath` strings and links it. It tests the WRITER, and the writer
//     was never wrong.
//   * `FfiResolveLibraryRoundTrip.EveryResolvedLibraryReachesTheEmittedDependencyTable`
//     (tests/program/) DOES drive the real driver with two real libraries on
//     three formats — but it STATES each identity with
//     `--resolve-library <path>=<name>`, and its own comment says why: the
//     shipped Mach-O dylib schema declared a single `installName` that "BOTH
//     stand-ins would inherit". The collapse was seen, named, and routed around
//     INSIDE the pin meant to catch it.
//
// So the missing arm is neither "drive the writer" nor "drive the driver". It
// is DRIVE THE DRIVER WITHOUT STATING THE IDENTITY — precedence level 2 of
// `ffi::recordedImportIdentity`, the binary's OWN embedded identity, which is
// the level the collapse lived on. That is what this file does.
//
// ✔MEASURED before the fix, exactly this shape: two distinct DSS-built dylibs
// both named on `--resolve-library` gave rc 0, ZERO diagnostics, and an
// executable recording ONE `LC_LOAD_DYLIB @rpath/libdss.dylib`; the second
// library's symbols were simply absent at load. The ELF control on the
// identical shape recorded TWO correct `DT_NEEDED` entries — which is what
// makes it a defect rather than a design, and why the control is in this file
// and not in a comment.
//
// NOT host-gated and nothing is RUN: "did the recorder tell two libraries
// apart?" is a COMPILE-time judgment about emitted bytes, so both legs stay
// live on every host — the same reasoning `kFormatLegs` gives elsewhere.

#include "core/types/diagnostic_reporter.hpp"
#include "image_dependency_table.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
// ⚠ `strnlen` is POSIX, not ISO C, so `<cstring>` puts it in the GLOBAL
// namespace on every host that has it but is not required to put it in `std`.
// The include was missing entirely and this TU still compiled here, because
// MinGW's gtest headers pull it in transitively — a build that works for a
// reason no line of this file states, and the first leg whose gtest does not
// would fail to compile rather than fail a test.
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
namespace fs = std::filesystem;
using dss::test_support::ScratchDir;
using dss::test_support::Location;

namespace {

// Two libraries with DIFFERENT symbols, so a collapse cannot be masked by the
// consumer happening to find what it needed in whichever library survived.
constexpr std::string_view kAlphaSrc = "int dss_alpha(void) { return 20; }\n";
constexpr std::string_view kBetaSrc  = "int dss_beta(void) { return 22; }\n";
constexpr std::string_view kMainSrc =
    "extern int dss_alpha(void);\n"
    "extern int dss_beta(void);\n"
    "int main(void) { return dss_alpha() + dss_beta(); }\n";

fs::path writeSrc(fs::path const& dir, std::string_view name,
                  std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream f(p);
    f << text;
    return p;
}

[[nodiscard]] std::vector<std::uint8_t> readWholeBinary(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

// Compile one source to one target through the real production driver.
// ⚠ `setResolveLibraries` takes PATHS here, never `<path>=<name>` specs: the
// whole point is that nothing states the identity, so the recorder must read it
// out of each binary.
int buildOne(fs::path const& outDir,
             std::vector<fs::path> const& resolveLibs,
             std::string const& srcPath,
             std::string const& target,
             DiagnosticReporter& rep) {
    Program p;
    p.setOutputDir(outDir);
    if (!resolveLibs.empty()) p.setResolveLibraries(resolveLibs);
    return p.compileFiles(std::vector<std::string>{srcPath},
                          "c", std::vector<std::string>{target}, rep);
}

// The LC_ID_DYLIB names an image declares about ITSELF (there is at most one).
// Distinct from `machoLoadedDylibs`, which reads LC_LOAD_DYLIB — what the image
// says about OTHERS. The collapse is a disagreement between the two, so both
// have to be readable here.
[[nodiscard]] std::vector<std::string>
machoOwnInstallNames(std::vector<std::uint8_t> const& b) {
    std::vector<std::string> out;
    auto u32 = [&](std::size_t at) -> std::uint32_t {
        return static_cast<std::uint32_t>(b[at])
             | (static_cast<std::uint32_t>(b[at + 1]) << 8)
             | (static_cast<std::uint32_t>(b[at + 2]) << 16)
             | (static_cast<std::uint32_t>(b[at + 3]) << 24);
    };
    if (b.size() < 32 || u32(0) != 0xFEEDFACFu) return out;
    std::uint32_t const ncmds = u32(16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (off + 8 > b.size()) break;
        std::uint32_t const cmd     = u32(off);
        std::uint32_t const cmdsize = u32(off + 4);
        if (cmdsize < 8 || off + cmdsize > b.size()) break;
        if (cmd == 0x0Du) {                       // LC_ID_DYLIB
            std::uint32_t const nameOff = u32(off + 8);
            if (nameOff < cmdsize) {
                char const* const s =
                    reinterpret_cast<char const*>(b.data() + off + nameOff);
                out.emplace_back(s, ::strnlen(s, cmdsize - nameOff));
            }
        }
        off += cmdsize;
    }
    return out;
}

[[nodiscard]] bool anyContains(std::vector<std::string> const& all,
                               std::string_view needle) {
    return std::any_of(all.begin(), all.end(), [&](std::string const& s) {
        return s.find(needle) != std::string::npos;
    });
}

[[nodiscard]] std::string render(std::vector<std::string> const& all) {
    std::string out = "{";
    for (auto const& s : all) { out += " '"; out += s; out += "'"; }
    return out + " }";
}

} // namespace

// ── THE MACH-O SUBJECT ─────────────────────────────────────────────────────
TEST(MachoInstallNameIdentity,
     TwoDssBuiltDylibsAreToldApartWithoutStatingTheirIdentities) {
    ScratchDir scratch{Location::InsideRepo, "macho-install-name-identity"};
    auto const dir = scratch.path();
    auto const alphaSrc = writeSrc(dir, "dssalpha.c", kAlphaSrc);
    auto const betaSrc  = writeSrc(dir, "dssbeta.c",  kBetaSrc);
    auto const mainSrc  = writeSrc(dir, "twolib.c",   kMainSrc);

    DiagnosticReporter alphaRep;
    ASSERT_EQ(buildOne(dir, {}, alphaSrc.string(),
                       "arm64:macho64-arm64-darwin-dylib", alphaRep), 0)
        << (alphaRep.all().empty() ? "" : alphaRep.all().front().actual);
    DiagnosticReporter betaRep;
    ASSERT_EQ(buildOne(dir, {}, betaSrc.string(),
                       "arm64:macho64-arm64-darwin-dylib", betaRep), 0)
        << (betaRep.all().empty() ? "" : betaRep.all().front().actual);

    auto const alphaLib = dir / "dssalpha.dylib";
    auto const betaLib  = dir / "dssbeta.dylib";
    ASSERT_TRUE(fs::exists(alphaLib));
    ASSERT_TRUE(fs::exists(betaLib));

    // ★ THE ROOT FACT, asserted before anything downstream of it: each library
    // declares its OWN identity. Two libraries sharing one LC_ID_DYLIB is the
    // defect itself, and everything below is a consequence — pinning only the
    // consequence would leave the cause free to come back in another shape.
    auto const alphaId = machoOwnInstallNames(readWholeBinary(alphaLib));
    auto const betaId  = machoOwnInstallNames(readWholeBinary(betaLib));
    ASSERT_EQ(alphaId.size(), 1u) << render(alphaId);
    ASSERT_EQ(betaId.size(), 1u)  << render(betaId);
    EXPECT_NE(alphaId.front(), betaId.front())
        << "two DSS-built dylibs embedded the SAME LC_ID_DYLIB "
        << render(alphaId) << " — dyld resolves that name once and the second "
           "library's symbols are absent at load";
    EXPECT_NE(alphaId.front().find("dssalpha.dylib"), std::string::npos)
        << render(alphaId);
    EXPECT_NE(betaId.front().find("dssbeta.dylib"), std::string::npos)
        << render(betaId);

    // ★ AND THE CONSEQUENCE, through the REAL recorder with NO stated identity:
    // the executable must record BOTH libraries.
    DiagnosticReporter mainRep;
    ASSERT_EQ(buildOne(dir, {alphaLib, betaLib}, mainSrc.string(),
                       "arm64:macho64-arm64-darwin-exec", mainRep), 0)
        << (mainRep.all().empty() ? "" : mainRep.all().front().actual);
    auto const exe = dir / "twolib";
    ASSERT_TRUE(fs::exists(exe));
    auto const loaded =
        dss::test_support::machoLoadedDylibs(readWholeBinary(exe));
    EXPECT_TRUE(anyContains(loaded, "dssalpha.dylib"))
        << "LC_LOAD_DYLIB set " << render(loaded);
    EXPECT_TRUE(anyContains(loaded, "dssbeta.dylib"))
        << "LC_LOAD_DYLIB set " << render(loaded);
}

// ── THE CONTROL, ON THE FORMAT THAT WAS ALWAYS RIGHT ───────────────────────
//
// Without it, "both libraries were recorded" is equally consistent with "this
// test records whatever it is given". ELF reached the same outcome by a
// DIFFERENT route — a DSS-built `.so` declares no DT_SONAME, so the recorder
// falls through to precedence level 3, the path basename, which was already
// distinct. That difference is the reason the two formats diverged at all, and
// it is why the control belongs beside the subject rather than in prose.
TEST(MachoInstallNameIdentity, ElfControlRecordsBothLibrariesOnTheSameShape) {
    ScratchDir scratch{Location::InsideRepo, "elf-install-name-control"};
    auto const dir = scratch.path();
    auto const alphaSrc = writeSrc(dir, "dssalpha.c", kAlphaSrc);
    auto const betaSrc  = writeSrc(dir, "dssbeta.c",  kBetaSrc);
    auto const mainSrc  = writeSrc(dir, "twolib.c",   kMainSrc);

    DiagnosticReporter alphaRep;
    ASSERT_EQ(buildOne(dir, {}, alphaSrc.string(),
                       "x86_64:elf64-x86_64-linux-dyn", alphaRep), 0)
        << (alphaRep.all().empty() ? "" : alphaRep.all().front().actual);
    DiagnosticReporter betaRep;
    ASSERT_EQ(buildOne(dir, {}, betaSrc.string(),
                       "x86_64:elf64-x86_64-linux-dyn", betaRep), 0)
        << (betaRep.all().empty() ? "" : betaRep.all().front().actual);

    auto const alphaLib = dir / "dssalpha.so";
    auto const betaLib  = dir / "dssbeta.so";
    ASSERT_TRUE(fs::exists(alphaLib));
    ASSERT_TRUE(fs::exists(betaLib));

    DiagnosticReporter mainRep;
    ASSERT_EQ(buildOne(dir, {alphaLib, betaLib}, mainSrc.string(),
                       "x86_64:elf64-x86_64-linux-exec", mainRep), 0)
        << (mainRep.all().empty() ? "" : mainRep.all().front().actual);
    auto const exe = dir / "twolib";
    ASSERT_TRUE(fs::exists(exe));
    auto const needed =
        dss::test_support::elfNeededLibraries(readWholeBinary(exe));
    EXPECT_TRUE(anyContains(needed, "dssalpha.so"))
        << "DT_NEEDED set " << render(needed);
    EXPECT_TRUE(anyContains(needed, "dssbeta.so"))
        << "DT_NEEDED set " << render(needed);
}
