// P69 fold 3f, lane `xa`: AN EXTERN DATUM IS READ THROUGH THE BINDING ITS FORMAT STATES, where that binding is the
// ONLY road to it -- an ELF executable's `got-indirect` `dataImportBinding`.
//
// ★ WHY ELF, AND WHY ITS OWN BINARY. The build half captures the format's `dataImportBinding` for the unit it lowers
// (program/test_driver_argument_supply.cpp's route pins). On pe64 the `indirect-slot` call dispatch puts EVERY extern
// import in a slot whatever that binding says, and every Mach-O executable and dylib document also states
// `externAddrBinding: "got"`, which routes every extern ADDRESS through its `__got` slot whatever it says (READ, the
// documents and mir_to_lir.cpp): ✔MEASURED, a build half that captured NO binding SURVIVED on both, at checkpoint
// 39's windows-x86_64-debug sweep and at checkpoint 44's macos-arm64-debug sweep (run 20261114-072646-c37ef201, 44
// cases, none red). The ELF exec and pie documents state no `externAddrBinding`, so there the captured `got-indirect`
// is the one rule that reaches a shared library's datum through its GOT slot. This binary holds that cell alone, so
// the arm that claims it names a runner no other arm's case count rides on.
//
// ★ THE ORACLE IS THE C LIBRARY'S OWN ANSWER. `stdout` read through its slot is libc's FILE, whose descriptor
// `fileno` reports as 1. One indirection short, the program holds the SLOT's address instead, and `fileno` of that
// is not the standard output's (or the link refuses a direct reference to a shared object's datum). The earlier ELF
// twin compared `stdout` against `stderr`, which two slot addresses pass too; this one cannot be passed that way.
//
// ★ LIVE WHERE IT RUNS. Every host builds the image and checks the import premise; an ELF host RUNS it on its own
// architecture, exec and pie.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "host_native_target.hpp"
#include "pe_image_readers.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using dss::DiagnosticReporter;
using dss::Program;
using dss::test_support::hostNativeTarget;
using dss::test_support::Location;
using dss::test_support::readAllBytes;
using dss::test_support::ScratchDir;

namespace {

constexpr char const* kCheckSource =
    "#include <stdio.h>\n"
    "int dib_check(void) {\n"
    "    FILE *viaImport = stdout;\n"
    "    if (viaImport == 0) return 2;\n"
    "    if (fileno(viaImport) != 1) return 4;\n"
    "    return 42;\n"
    "}\n";
constexpr char const* kMainSource =
    "extern int dib_check(void);\n"
    "int main(void) { return dib_check(); }\n";

enum class Route : std::uint8_t { SoleUnit, MergedUnits };

[[nodiscard]] std::string_view routeName(Route r) noexcept {
    return r == Route::SoleUnit ? "the sole-CU route" : "the merged route";
}

[[nodiscard]] fs::path written(fs::path const& dir, std::string_view name, std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream out(p, std::ios::binary);
    out << text;
    return p;
}

// The sole-CU route compiles ONE source holding both texts, under the first name; the merged route compiles the two
// as two units of one program. The artifact is named after the first source on both.
[[nodiscard]] int buildOnRoute(Route route, fs::path const& dir, std::string const& spec, fs::path const& out,
                               DiagnosticReporter& rep) {
    Program prog;
    prog.setOutputDir(out);
    if (route == Route::SoleUnit) {
        auto const one = written(dir, "dataimport_a.c", std::string{kCheckSource} + "\n" + kMainSource);
        return prog.compileUnits({one.generic_string()}, "c", {spec}, rep);
    }
    auto const a = written(dir, "dataimport_a.c", kCheckSource);
    auto const b = written(dir, "dataimport_b.c", kMainSource);
    return prog.compileUnits({a.generic_string(), b.generic_string()}, "c", {spec}, rep);
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{dss::diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

[[nodiscard]] std::size_t occurrences(std::vector<std::uint8_t> const& hay, std::string_view needle) {
    std::size_t n = 0;
    for (auto it = hay.begin(); (it = std::search(it, hay.end(), needle.begin(), needle.end())) != hay.end(); ++it) {
        ++n;
    }
    return n;
}

// The host's ELF executable flavors (exec and pie of its own architecture) where the host is an ELF one, and x86_64's
// elsewhere, where the images are built and never run.
struct ElfTargets {
    std::vector<std::string> specs;
    bool                     runHere = false;
};

[[nodiscard]] ElfTargets elfTargets() {
    std::string exec{hostNativeTarget().execTarget};
    bool const elfHost = exec.find(":elf64-") != std::string::npos;
    if (!elfHost) exec = "x86_64:elf64-x86_64-linux-exec";
    std::string pie = exec.substr(0, exec.rfind('-')) + "-pie";
    return {{exec, pie}, elfHost};
}

void readsTheDatumThroughItsBinding(Route const route) {
    SCOPED_TRACE(routeName(route));
    auto const targets = elfTargets();
    ScratchDir scratch{Location::InsideRepo, "extern-data-binding-elf"};
    scratch.useAsCwd();
    std::size_t k = 0;
    for (std::string const& spec : targets.specs) {
        SCOPED_TRACE(spec);
        auto const dir = scratch.path() / std::to_string(k++);
        fs::create_directories(dir);
        auto const out = dir / "out";
        DiagnosticReporter rep;
        ASSERT_EQ(buildOnRoute(route, dir, spec, out, rep), 0)
            << "the ELF extern DATA build must succeed" << diagnosticsOf(rep);
        auto const exe = out / "dataimport_a";
        auto const img = readAllBytes(exe);
        ASSERT_FALSE(img.empty()) << "the ELF artifact must exist: " << exe.string();
        // The premise: the datum is really a shared library's, named in the image's dynamic symbols, so the binding
        // this pin is about really was consulted.
        EXPECT_GE(occurrences(img, "stdout"), 1u) << "the emitted image must import the extern DATA object";
        if (!targets.runHere) continue;
        auto const r = dss::test_support::runBinary(exe, dss::test_support::kRunBudget);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_FALSE(r.timedOut) << r.diagnostic;
        EXPECT_EQ(r.exitCode, 42u)
            << "the ELF extern DATA read through its GOT slot: exit 4 is the AGREEMENT rung, `fileno` of what the "
               "program read as `stdout` is not the standard output's, which is what a dropped `dataImportBinding` "
               "produces -- the address materialization stops one indirection short and yields the GOT SLOT's "
               "address instead of the FILE's. Exit 2 would mean the slot was never filled";
    }
    if (!targets.runHere) {
        GTEST_SKIP() << "an ELF image RUNS only on an ELF host. The build and import-premise assertions above ran.";
    }
}

}  // namespace

TEST(ExternDataBindingRun, SingleCuRouteReadsAnExternDatumThroughItsBindingOnElf) {
    readsTheDatumThroughItsBinding(Route::SoleUnit);
}
TEST(ExternDataBindingRun, MergedMultiCuRouteReadsAnExternDatumThroughItsBindingOnElf) {
    readsTheDatumThroughItsBinding(Route::MergedUnits);
}
