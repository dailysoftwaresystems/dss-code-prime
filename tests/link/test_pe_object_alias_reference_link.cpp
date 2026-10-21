// P69 (lane `cs`) — A GLOBAL ALIAS OF A `static` BODY IN A DSS pe64 RELOCATABLE OBJECT, AS THE REFERENCE
// LINKERS TAKE IT.
//
// `static int dss_private_body(void) { return 40; } int dss_exported_body(void) __attribute__((alias(
// "dss_private_body")));` — the body is module-private, its second name global. mingw-w64 gcc 13.2.0 builds and
// runs it at -O0 and -O2 (✔MEASURED, lane `cs` probe al2 cell 3). The PE writer refused it ("an alias that must
// NOT yield") though neither name is weak: COFF writes the pair as two records at one address, each with its own
// storage class (EXTERNAL for the global name, STATIC for the module-private one), and only a WEAK side carries a
// coalescing policy for the two to differ in.
//
// The object's records are read back on every host by `CompiledSymbolAlias.AnAliasWhoseBindingDiffersFrom
// ItsTargetsWithNoWeakSideIsItsOwnRecord` (tests/link) and `CompiledSymbolAliasObject.EachAliasIsOneMore
// NameOnItsTargetsDefinitionInTheObject` (tests/program); DSS's own linker runs the same declarations in
// examples/c/symbol_alias_second_name (pe64, baseline and release). THIS file is the runtime witness of the
// OBJECT under the reference linkers: a second DSS object calls the alias by its global name, the objects are
// handed to link.exe and to MinGW's linker with the raw entry of `pe_raw_entry.hpp` (no C runtime startup), and
// the image is RUN — 42 iff the call through the global name reached the module-private body. Windows-only;
// each arm SKIPS, naming the absent toolchain, rather than reddens. Both pipelines: the objects are compiled at
// the baseline and at `--config=release` (where the body only the alias names must survive dead-code removal).

#include "core/types/diagnostic_reporter.hpp"
#include "program/cli_args.hpp"
#include "program/program.hpp"
#include "pe_raw_entry.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

constexpr char const* kAliasUnit =
    "static int dss_private_body(void) { return 40; }\n"
    "int dss_exported_body(void) __attribute__((alias(\"dss_private_body\")));\n";

constexpr char const* kCallerUnit =
    "int dss_exported_body(void);\n"
    "int main(void) { return dss_exported_body() + 2; }\n";

constexpr CompileConfig kConfigs[] = {CompileConfig::Debug, CompileConfig::Release};

[[nodiscard]] char const* configName(CompileConfig c) { return c == CompileConfig::Release ? "release" : "baseline"; }

// Compile one C unit to a pe64 RELOCATABLE object `<dir>/<stem>.obj` through the production driver. False on a
// compile failure; every caller asserts.
[[nodiscard]] bool buildObj(fs::path const& dir, std::string_view stem, std::string_view text, CompileConfig config,
                            DiagnosticReporter& rep) {
    auto const src = dir / (std::string{stem} + ".c");
    std::ofstream(src, std::ios::binary) << text;
    auto const out = dir / (std::string{stem} + ".out");
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    p.setCompileConfig(config);
    int const rc = p.compileFiles(std::vector<std::string>{src.string()}, "c",
                                  std::vector<std::string>{"x86_64:pe64-x86_64-windows"}, rep);
    auto const obj = out / (std::string{stem} + ".obj");
    if (rc != 0 || !fs::exists(obj)) return false;
    // The second configuration's object replaces the first's. Removed, then copied: libstdc++'s
    // `copy_file` with `overwrite_existing` refuses an existing target on Windows ("File exists",
    // ✔MEASURED on the windows-x86_64-debug leg, its mingw-w64 GCC 13.2.0 host build).
    auto const dst = dir / (std::string{stem} + ".obj");
    std::error_code gone;
    fs::remove(dst, gone);
    fs::copy_file(obj, dst);
    return true;
}

// The three objects of one image: the alias's unit, its caller's, and the raw entry.
[[nodiscard]] bool buildObjects(fs::path const& dir, CompileConfig config, DiagnosticReporter& rep) {
    return buildObj(dir, "alias", kAliasUnit, config, rep) && buildObj(dir, "caller", kCallerUnit, config, rep)
        && buildObj(dir, "entry", test::pe_raw_entry::kSource, config, rep);
}

}  // namespace

TEST(PeObjectAliasReferenceLinkNative, LinkExeCallsTheStaticBodyThroughItsGlobalAlias) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-alias-ref-link"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const env = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(env.ready()) << env.describe();
    for (CompileConfig const config : kConfigs) {
        SCOPED_TRACE(configName(config));
        DiagnosticReporter rep;
        ASSERT_TRUE(buildObjects(dir, config, rep)) << "errs=" << rep.errorCount();
        std::string const exe = std::string{"alias_link_"} + configName(config) + ".exe";
        ASSERT_TRUE(env.run("link /nologo /OUT:" + exe + " /ENTRY:" + test::pe_raw_entry::kSymbol
                            + " /SUBSYSTEM:CONSOLE /NODEFAULTLIB alias.obj caller.obj entry.obj kernel32.lib"))
            << "link.exe must take the DSS objects: the global alias is an EXTERNAL record of the alias's object";
        auto const r = test_support::runBinary(dir / exe);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "the call through the global name did not reach the module-private body";
    }
}

TEST(PeObjectAliasReferenceLinkNative, MingwCallsTheStaticBodyThroughItsGlobalAlias) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-alias-ref-link"};
    auto const dir = scratch.path();
    auto const gcc = test_support::native_probe::locateMingwGcc(dir);
    if (gcc.toolAbsent()) GTEST_SKIP() << gcc.detail;
    ASSERT_TRUE(gcc.ok()) << gcc.describe();
    for (CompileConfig const config : kConfigs) {
        SCOPED_TRACE(configName(config));
        DiagnosticReporter rep;
        ASSERT_TRUE(buildObjects(dir, config, rep)) << "errs=" << rep.errorCount();
        std::string const exe = std::string{"alias_mingw_"} + configName(config) + ".exe";
        std::string const cmd = "cd /d \"" + dir.string() + "\" && gcc -nostartfiles -e "
                              + test::pe_raw_entry::kSymbol + " -o " + exe
                              + " alias.obj caller.obj entry.obj -lkernel32 >nul 2>&1";
        ASSERT_EQ(std::system(("\"" + cmd + "\"").c_str()), 0)
            << "MinGW's linker must take the DSS objects: the global alias is an EXTERNAL record of its object";
        auto const r = test_support::runBinary(dir / exe);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "the call through the global name did not reach the module-private body";
    }
}
