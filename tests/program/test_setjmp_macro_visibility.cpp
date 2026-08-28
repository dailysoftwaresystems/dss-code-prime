// D-CONFIG-SETJMP-MACRO-INVISIBLE-ON-ELF — the host-independent pin.
//
// ★★★ THE DIVERGENCE, AND WHY IT IS NOT THE SAME SHAPE AS ITS SIBLING ROW.
// `setjmp` carried a `pe`-only macro variant in
// `src/dss-config/shippedLibs/setjmp.json`, so `#ifdef setjmp` answered TRUE on
// pe and FALSE on elf and macho — for a name reachable in CODE on all three.
//
// ⚠ ISO DOES NOT SETTLE THIS, AND THIS PIN DELIBERATELY DOES NOT REST ON IT.
// C23 §7.13p1 lists `setjmp` among the macros the header defines, but §7.13p4
// says outright that "It is unspecified whether setjmp is a macro or an
// identifier declared with external linkage". A strictly-ISO reading therefore
// REQUIRES NOTHING HERE. The binding rule is the reference union — and the
// references disagree WITH EACH OTHER, per target, which is the whole reason
// this file asserts a per-format contract instead of one global answer:
//
//   elf   glibc's <setjmp.h> — the `#define setjmp(env) _setjmp (env)` line.
//         ✔MEASURED 2026-08-27: `#ifndef setjmp -> #error` is rc=0 under
//         gcc 13.3.0 -std=c2x AND clang 18.1.3 -std=c23, at c99/c11/c2x/gnu2x
//         alike, with a control `#error` that fired on both instruments.
//   pe    MSVC's <setjmp.h> — `#define setjmp _setjmp`. DSS has always shipped
//         this arm (2-arg `_setjmp(env, 0)` -> ucrtbase `__intrinsic_setjmp`).
//   macho Apple's SDK <setjmp.h> declares `extern int setjmp(jmp_buf);` and
//         carries NO `#define` at all. ✔MEASURED on the operator's macOS host
//         (MacOSX.sdk/usr/include/setjmp.h, clang -std=c2x, same two controls):
//         the probe is rc=1 there.
//
// ⇒ ELF WAS BELOW ITS REFERENCE AND IS FIXED. MACH-O MATCHES ITS OWN REFERENCE
// AND ITS ABSENCE IS ASSERTED HERE AS A REQUIREMENT, not tolerated as a gap:
// sweeping a `macho` arm in "for consistency" would put DSS ABOVE the only
// reference that target has, which is the worse half of the same rule — it
// turns a correct refusal into an invented extension. A reference control must
// match the TARGET, not the host that happens to run it.
//
// ★★ WHY THIS EXISTS ALONGSIDE `examples/c/setjmp_macro_visibility`, WHICH
// LOOKS LIKE THE SAME TEST. It is not. The example folds the contract into an
// EXIT CODE, so it can only detect a loss on a leg where the artifact RUNS, and
// its elf/macho arms declare `runOn: ["linux"]` / `["darwin"]`. On a Windows
// host only the pe arm executes, so the example CANNOT fail for the elf
// regression it exists to catch — lane T measured exactly that hole in the
// sibling stdio row, where deleting an `elf` variant left the example GREEN on
// Windows. Macro visibility is a PREPROCESSING fact and DSS cross-compiles, so
// it is observable at COMPILE time on every host, which is what this file does.
//
// ★★ AND IT IS ASSERTED IN BOTH DIRECTIONS, WHICH IS THE POINT. A test that
// only asked "is it a macro?" would pass on macho for the wrong reason and
// would stay green if someone added a macho arm. Every format is pinned by a
// POSITIVE arm (the probe that must succeed) and a NEGATIVE arm (the probe that
// must fail), so neither a dropped variant nor a swept-in one can pass.

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

// Fires when `setjmp` is NOT a macro. Green means it IS one.
constexpr std::string_view kRequiresMacro =
    "#include <setjmp.h>\n"
    "#ifndef setjmp\n"
    "#error \"setjmp is not a macro\"\n"
    "#endif\n"
    "int main(void) { return 0; }\n";

// Fires when `setjmp` IS a macro. Green means it is NOT one. This is the arm
// that keeps Mach-O honest in the direction a one-sided test cannot see.
constexpr std::string_view kForbidsMacro =
    "#include <setjmp.h>\n"
    "#ifdef setjmp\n"
    "#error \"setjmp is a macro on a format whose reference has none\"\n"
    "#endif\n"
    "int main(void) { return 0; }\n";

// ★ THE INSTRUMENT CONTROLS. Without them neither verdict above is readable: an
// arm that silently stopped reaching the preprocessor — a changed harness, a
// swallowed `#error`, a target that no longer resolves — reports rc=0 exactly
// like a satisfied probe, and an arm that stopped compiling at all reports
// rc!=0 exactly like a fired `#error`. `kControlMustFail` names a macro nothing
// will ever define, so it MUST fail on every target; `kControlMustPass` is the
// bare include, so it MUST succeed on every target.
constexpr std::string_view kControlMustFail =
    "#include <setjmp.h>\n"
    "#ifndef DSS_NO_SUCH_MACRO_EVER\n"
    "#error \"control: this must always fire\"\n"
    "#endif\n"
    "int main(void) { return 0; }\n";

constexpr std::string_view kControlMustPass =
    "#include <setjmp.h>\n"
    "int main(void) { return 0; }\n";

[[nodiscard]] int compileFor(ScratchDir& scratch, std::string_view source,
                             std::string const& spec, DiagnosticReporter& rep) {
    fs::path const src = scratch.path() / "setjmp_macro_probe.c";
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

// One shipped format of each KIND, plus a second architecture on elf so the
// fact is pinned as a FORMAT property rather than an x86_64 one. `macroRequired`
// is this target's own reference toolchain's answer, measured above — it is the
// contract, not an observation of DSS.
struct Leg {
    std::string_view spec;
    bool             macroRequired;
    std::string_view reference;
};

constexpr std::array<Leg, 4> kLegs{{
    {"x86_64:elf64-x86_64-linux-exec", true,
     "glibc's <setjmp.h> `#define setjmp(env) _setjmp (env)`; gcc 13.3 and clang 18.1 both rc=0"},
    {"arm64:elf64-aarch64-linux-exec", true,
     "same glibc header on the aarch64 sysroot — the fact is per-FORMAT, not per-arch"},
    {"x86_64:pe64-x86_64-windows-exec", true,
     "MSVC <setjmp.h> `#define setjmp _setjmp`"},
    {"x86_64:macho64-x86_64-darwin-exec", false,
     "Apple SDK <setjmp.h> declares `extern int setjmp(jmp_buf);` with NO #define — MEASURED on the operator's macOS host"},
}};

} // namespace

TEST(SetjmpMacroVisibility, SetjmpIsAMacroExactlyWhereItsTargetsReferenceMakesItOne) {
    for (Leg const& leg : kLegs) {
        // POSITIVE arm: `#ifndef setjmp -> #error`.
        {
            ScratchDir         scratch{Location::InsideRepo, "setjmp-macro"};
            DiagnosticReporter rep{DiagnosticReporter::Config{}};
            int const rc = compileFor(scratch, kRequiresMacro,
                                      std::string{leg.spec}, rep);
            if (leg.macroRequired) {
                EXPECT_EQ(rc, 0)
                    << "target " << leg.spec
                    << ": <setjmp.h> did not make `setjmp` a MACRO, but this "
                       "target's own reference does — "
                    << leg.reference
                    << ". Add the missing per-format `variants` entry in "
                       "src/dss-config/shippedLibs/setjmp.json. The variant "
                       "selector has no fallback arm by design, so a format "
                       "with no arm silently gets no macro.";
            } else {
                EXPECT_NE(rc, 0)
                    << "target " << leg.spec
                    << ": `setjmp` IS a macro, but this target's own reference "
                       "has none — "
                    << leg.reference
                    << ". A `macho` variant was most likely swept in for "
                       "consistency with elf/pe; that puts DSS ABOVE the only "
                       "reference this target has and must be reverted.";
                EXPECT_TRUE(sawErrorDirective(rep))
                    << "target " << leg.spec
                    << ": the compile failed, but not at the `#error` this "
                       "probe placed — the verdict above is not about macro "
                       "visibility.";
            }
        }
        // NEGATIVE arm: `#ifdef setjmp -> #error`. The same fact read from the
        // other side, so neither answer can be produced by an unrelated failure.
        {
            ScratchDir         scratch{Location::InsideRepo, "setjmp-macro"};
            DiagnosticReporter rep{DiagnosticReporter::Config{}};
            int const rc = compileFor(scratch, kForbidsMacro,
                                      std::string{leg.spec}, rep);
            if (leg.macroRequired) {
                EXPECT_NE(rc, 0)
                    << "target " << leg.spec
                    << ": the `#ifdef setjmp` probe did NOT fire, so `setjmp` "
                       "is not a macro here — contradicting the positive arm "
                       "above and this target's reference (" << leg.reference << ").";
                EXPECT_TRUE(sawErrorDirective(rep))
                    << "target " << leg.spec
                    << ": the compile failed, but not at this probe's `#error`.";
            } else {
                EXPECT_EQ(rc, 0)
                    << "target " << leg.spec
                    << ": `setjmp` is a macro on a format whose reference has "
                       "none (" << leg.reference << ").";
            }
        }
    }
}

TEST(SetjmpMacroVisibility, TheProbeInstrumentIsLiveOnEveryLeg) {
    for (Leg const& leg : kLegs) {
        {
            ScratchDir         scratch{Location::InsideRepo, "setjmp-macro"};
            DiagnosticReporter rep{DiagnosticReporter::Config{}};
            int const rc = compileFor(scratch, kControlMustFail,
                                      std::string{leg.spec}, rep);
            EXPECT_NE(rc, 0)
                << "target " << leg.spec
                << ": the control `#error` did not fire, so every green verdict "
                   "in this file is unreadable — a probe that never reaches the "
                   "preprocessor reports rc=0 exactly like a satisfied one.";
            EXPECT_TRUE(sawErrorDirective(rep))
                << "target " << leg.spec
                << ": the control failed for some reason OTHER than its "
                   "`#error`, so it is not testing what it claims to.";
        }
        {
            ScratchDir         scratch{Location::InsideRepo, "setjmp-macro"};
            DiagnosticReporter rep{DiagnosticReporter::Config{}};
            int const rc = compileFor(scratch, kControlMustPass,
                                      std::string{leg.spec}, rep);
            EXPECT_EQ(rc, 0)
                << "target " << leg.spec
                << ": a bare `#include <setjmp.h>` does not compile, so every "
                   "RED verdict in this file is unreadable — the header is "
                   "failing for a reason that has nothing to do with macros.";
        }
    }
}
