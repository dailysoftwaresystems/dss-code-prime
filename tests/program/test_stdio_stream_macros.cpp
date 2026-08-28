// D-CONFIG-STDIO-STREAM-MACROS-INVISIBLE-ON-ELF — the host-independent pin.
//
// ★★★ THE DIVERGENCE. ISO C 7.23.1 lists `stdin`, `stdout` and `stderr` among
// the MACROS a conforming `<stdio.h>` defines, and both ELF references agree:
// glibc writes the self-referential `#define stdin stdin` for exactly that
// reason. ✔MEASURED 2026-08-27, with a control `#error` that FIRED on both
// instruments so the green arms are not an artifact: `gcc 13.3.0 -std=c2x`
// rc=0, `clang 18.1.3 -std=c23` rc=0. DSS answered FALSE on ELF only —
// `shippedLibs/stdio.json` carried `variants` for `pe` and `macho` alone, so on
// ELF the three existed solely as `symbols`: usable in code, invisible to
// `#ifdef`. Both halves of `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` were broken at
// once.
//
// ★★ WHY THIS EXISTS ALONGSIDE `examples/c/stdio_stream_macros_visible`, WHICH
// LOOKS LIKE THE SAME TEST. It is not, and the difference is exactly the kind of
// gap that lets a config regression ship. The example folds each `#ifdef` into
// an EXIT CODE, so it can only detect the loss on a leg where the artifact
// RUNS — and its ELF arms declare `runOn: ["linux"]`. ✔MEASURED: deleting the
// `elf` variant for `stdout` and re-running the example on WINDOWS leaves it
// GREEN, because the Windows leg only ever runs the `pe` arm. The property is a
// PREPROCESSING fact, and DSS cross-compiles, so it is observable at COMPILE
// time on every host — which is what this file asserts.
//
// ⚠ THE ASSERTION IS `#error`-SHAPED ON PURPOSE. A test that merely compiled
// `fputs(..., stdout)` would pass on ELF today AND before the fix, because the
// three names have always existed as `symbols`. Only `#ifdef` can see the
// difference between a name that exists and a MACRO that exists, and only a
// `#error` turns that into an exit code.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// Each of the three names, asserted separately, so a failure says WHICH stream
// lost its macro instead of only that one did.
constexpr std::string_view kProbe =
    "#include <stdio.h>\n"
    "#ifndef stdin\n"
    "#error \"stdin is not a macro\"\n"
    "#endif\n"
    "#ifndef stdout\n"
    "#error \"stdout is not a macro\"\n"
    "#endif\n"
    "#ifndef stderr\n"
    "#error \"stderr is not a macro\"\n"
    "#endif\n"
    "int main(void) { return 0; }\n";

// ★ THE CONTROL SOURCE. Without it a green above is unreadable: an arm that
// silently stopped compiling the file at all — a changed harness, a swallowed
// `#error`, a target that no longer reaches the preprocessor — would report the
// same rc=0. This one names a macro nothing will ever define, so it MUST fail
// on every target, and if it does not, the arms above proved nothing.
constexpr std::string_view kControl =
    "#include <stdio.h>\n"
    "#ifndef DSS_NO_SUCH_MACRO_EVER\n"
    "#error \"control: this must always fire\"\n"
    "#endif\n"
    "int main(void) { return 0; }\n";

[[nodiscard]] int compileFor(ScratchDir& scratch, std::string_view source,
                             std::string const& spec, DiagnosticReporter& rep) {
    fs::path const src = scratch.path() / "stdio_macro_probe.c";
    {
        std::ofstream out{src, std::ios::binary};
        if (!out.good()) return -1;
        out << source;
    }
    scratch.useAsCwd();
    Program prog;
    return prog.compileFiles({src.generic_string()}, "c", {spec}, rep);
}

[[nodiscard]] bool sawErrorDirective(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::P_PreprocessorErrorDirective) return true;
    }
    return false;
}

// One shipped format of each KIND. The macro's REPLACEMENT legitimately differs
// per kind — a call on pe, a rename on macho, the self-referential form on elf —
// but its EXISTENCE does not, and that is the property.
constexpr std::array<std::string_view, 4> kSpecs{
    "x86_64:elf64-x86_64-linux-exec",
    "arm64:elf64-aarch64-linux-exec",
    "x86_64:pe64-x86_64-windows-exec",
    "x86_64:macho64-x86_64-darwin-exec",
};

} // namespace

TEST(StdioStreamMacros, TheThreeStreamsAreMacrosOnEveryObjectFormat) {
    for (std::string_view spec : kSpecs) {
        ScratchDir         scratch{Location::InsideRepo, "stdio-macro"};
        DiagnosticReporter rep{DiagnosticReporter::Config{}};
        int const          rc = compileFor(scratch, kProbe, std::string{spec}, rep);
        EXPECT_EQ(rc, 0)
            << "target " << spec
            << ": <stdio.h> did not define stdin/stdout/stderr as MACROS. ISO C "
               "7.23.1 requires all three, and gcc 13.3 / clang 18.1 both "
               "answer true on ELF (glibc writes `#define stdin stdin` for "
               "exactly this). Add the missing per-format `variants` entry in "
               "src/dss-config/shippedLibs/stdio.json — the variant selector "
               "has no fallback arm by design, so a format with no matching "
               "variant gets no macro at all "
               "(D-CONFIG-STDIO-STREAM-MACROS-INVISIBLE-ON-ELF)";
        EXPECT_FALSE(sawErrorDirective(rep))
            << "target " << spec << ": an #error fired, so at least one of the "
               "three names is not a macro on this format";
    }
}

TEST(StdioStreamMacros, TheControlProbeFailsOnEveryObjectFormat) {
    for (std::string_view spec : kSpecs) {
        ScratchDir         scratch{Location::InsideRepo, "stdio-macro-ctl"};
        DiagnosticReporter rep{DiagnosticReporter::Config{}};
        int const          rc = compileFor(scratch, kControl, std::string{spec}, rep);
        EXPECT_NE(rc, 0)
            << "target " << spec
            << ": THE CONTROL DID NOT FIRE. A source whose `#error` names a "
               "macro nothing defines compiled cleanly, so this harness is not "
               "reaching the preprocessor on this target and the case above is "
               "vacuous for it";
        EXPECT_TRUE(sawErrorDirective(rep))
            << "target " << spec
            << ": the control failed for some reason OTHER than its `#error`, "
               "so it is not the control it claims to be";
    }
}
