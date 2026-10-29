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
//
// ★ AND A STRONG ALIAS OF A SELECT-ANY DEFINITION (P69 fold f26b): `__declspec(selectany) int dss_selectany_value
// = 40; extern int dss_strong_alias __attribute__((alias("dss_selectany_value")));` — a weak definition PE writes as
// a COMDAT section of its own, and a strong second name of it. The PE writer refused the unit; clang-cl compiles it,
// writing the alias as an external LABEL inside the definition's COMDAT section (✔MEASURED by lane `xa`,
// 2026-10-20, windows-x86_64), and DSS now writes the same. A second DSS object reads the value through the alias,
// and the image returns it plus 2 — 42 iff the strong name reached the definition's storage — linked by link.exe,
// by lld-link and by DSS's own linker, at the baseline and at `--config=release`. Records:
// `PeWriter.AnAliasOfAnotherBindingIsALabelOfItsBodyUnlessAWeakNameMeetsAPrivateBody` and
// `CompiledSymbolAlias.AStrongAliasOfASelectAnyDefinitionIsANameOfItsBody` (tests/link).

#include "core/types/diagnostic_reporter.hpp"
#include "program/cli_args.hpp"
#include "program/program.hpp"
#include "lld_link_of_any_instance.hpp"
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

// The WEAK alias of the same module-private body (P69 fold f26b, item 10): what mingw-w64 gcc 13.2.0 writes for it,
// and what DSS's pe64 writer writes (✔MEASURED, lane `cs` probe pw): the body under an EXTERNAL default record, the
// weak name a weak external naming it.
constexpr char const* kWeakAliasUnit =
    "static int dss_private_body(void) { return 40; }\n"
    "int dss_exported_body(void) __attribute__((weak, alias(\"dss_private_body\")));\n";

// The three objects of one image: the alias's unit, its caller's, and the raw entry.
[[nodiscard]] bool buildObjects(fs::path const& dir, CompileConfig config, DiagnosticReporter& rep,
                                char const* aliasUnit = kAliasUnit) {
    return buildObj(dir, "alias", aliasUnit, config, rep) && buildObj(dir, "caller", kCallerUnit, config, rep)
        && buildObj(dir, "entry", test::pe_raw_entry::kSource, config, rep);
}

// The select-any definition and its strong alias, and the unit that reads the value through the alias.
constexpr char const* kSelectAnyAliasUnit =
    "__declspec(selectany) int dss_selectany_value = 40;\n"
    "extern int dss_strong_alias __attribute__((alias(\"dss_selectany_value\")));\n";

constexpr char const* kSelectAnyReaderUnit =
    "extern int dss_strong_alias;\n"
    "int main(void) { return dss_strong_alias + 2; }\n";

// The three objects of the select-any image: the definition's unit, its reader's, and the raw entry.
[[nodiscard]] bool buildSelectAnyObjects(fs::path const& dir, CompileConfig config, DiagnosticReporter& rep) {
    return buildObj(dir, "selectany", kSelectAnyAliasUnit, config, rep)
        && buildObj(dir, "reader", kSelectAnyReaderUnit, config, rep)
        && buildObj(dir, "entry", test::pe_raw_entry::kSource, config, rep);
}

void expectTheValueThroughTheAlias(fs::path const& exe, char const* who) {
    auto const r = test_support::runBinary(exe);
    ASSERT_TRUE(r.spawned) << who << ": " << r.diagnostic;
    EXPECT_FALSE(r.timedOut) << who;
    EXPECT_EQ(r.exitCode, 42u) << who
                               << ": the read through the strong alias did not reach the select-any definition";
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

// THE WEAK ALIAS OF A `static` BODY, linked by a reference linker (P69 fold f26b, item 10). The alias fold's review
// premised that the writer refused it; DSS's own COFF reader refuted that (CompiledSymbolAliasObject reads the weak
// name back as a weak definition of the body), and these two read the refutation by the linkers the object is for:
// GNU ld through mingw-w64 gcc 13.2.0, and link.exe. The image runs, 42 only through the weak name.
//
// ★ GNU ld IS HANDED THE CALL IN THE ALIAS'S OWN UNIT. Its PE backend resolves no weak external across objects —
// gcc's own object included (✔MEASURED, the note at the weak-external arm of src/link/format/pe.cpp), and so DSS's
// object with the call in a second object (✔MEASURED, P69 fold f26b, windows-x86_64-debug: the link refused) — and
// links gcc's `weak, alias` only where the reference is in the same translation unit. That is the program it runs,
// so that is the image built here; link.exe takes the cross-object form below.
constexpr char const* kWeakAliasAndCallerUnit =
    "static int dss_private_body(void) { return 40; }\n"
    "int dss_exported_body(void) __attribute__((weak, alias(\"dss_private_body\")));\n"
    "int main(void) { return dss_exported_body() + 2; }\n";

TEST(PeObjectAliasReferenceLinkNative, MingwCallsTheStaticBodyThroughItsWeakAlias) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-weak-alias-ref-link"};
    auto const dir = scratch.path();
    auto const gcc = test_support::native_probe::locateMingwGcc(dir);
    if (gcc.toolAbsent()) GTEST_SKIP() << gcc.detail;
    ASSERT_TRUE(gcc.ok()) << gcc.describe();
    for (CompileConfig const config : kConfigs) {
        SCOPED_TRACE(configName(config));
        DiagnosticReporter rep;
        ASSERT_TRUE(buildObj(dir, "alias", kWeakAliasAndCallerUnit, config, rep)
                    && buildObj(dir, "entry", test::pe_raw_entry::kSource, config, rep))
            << "errs=" << rep.errorCount();
        std::string const exe = std::string{"weak_alias_mingw_"} + configName(config) + ".exe";
        std::string const cmd = "cd /d \"" + dir.string() + "\" && gcc -nostartfiles -e "
                              + test::pe_raw_entry::kSymbol + " -o " + exe
                              + " alias.obj entry.obj -lkernel32 >nul 2>&1";
        ASSERT_EQ(std::system(("\"" + cmd + "\"").c_str()), 0)
            << "MinGW's linker must take the DSS objects: the weak name is a weak external of the body's record";
        auto const r = test_support::runBinary(dir / exe);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "the call through the weak name did not reach the module-private body";
    }
}

TEST(PeObjectAliasReferenceLinkNative, LinkExeCallsTheStaticBodyThroughItsWeakAlias) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-weak-alias-ref-link"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const env = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(env.ready()) << env.describe();
    for (CompileConfig const config : kConfigs) {
        SCOPED_TRACE(configName(config));
        DiagnosticReporter rep;
        ASSERT_TRUE(buildObjects(dir, config, rep, kWeakAliasUnit)) << "errs=" << rep.errorCount();
        std::string const exe = std::string{"weak_alias_link_"} + configName(config) + ".exe";
        ASSERT_TRUE(env.run("link /nologo /OUT:" + exe + " /ENTRY:" + test::pe_raw_entry::kSymbol
                            + " /SUBSYSTEM:CONSOLE /NODEFAULTLIB alias.obj caller.obj entry.obj kernel32.lib"))
            << "link.exe must take the DSS objects: the weak name is a weak external of the body's record";
        auto const r = test_support::runBinary(dir / exe);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "the call through the weak name did not reach the module-private body";
    }
}

TEST(PeObjectAliasReferenceLinkNative, LinkExeReadsASelectAnyDefinitionThroughItsStrongAlias) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-selectany-alias-link"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const env = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(env.ready()) << env.describe();
    for (CompileConfig const config : kConfigs) {
        SCOPED_TRACE(configName(config));
        DiagnosticReporter rep;
        ASSERT_TRUE(buildSelectAnyObjects(dir, config, rep))
            << "DSS must write the select-any definition and its strong alias to a PE object; errs="
            << rep.errorCount();
        std::string const exe = std::string{"selectany_link_"} + configName(config) + ".exe";
        ASSERT_TRUE(env.run("link /nologo /OUT:" + exe + " /ENTRY:" + test::pe_raw_entry::kSymbol
                            + " /SUBSYSTEM:CONSOLE /NODEFAULTLIB selectany.obj reader.obj entry.obj kernel32.lib"))
            << "link.exe must take the DSS objects: the strong alias is an external label of the COMDAT section";
        expectTheValueThroughTheAlias(dir / exe, "link.exe");
    }
}

TEST(PeObjectAliasReferenceLinkNative, LldLinkReadsASelectAnyDefinitionThroughItsStrongAlias) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-selectany-alias-lld"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    std::string why;
    auto const lld = test_support::lldLinkOfAnyInstance(dir, why);
    if (lld.empty()) GTEST_SKIP() << "no lld-link on this host: " << why;
    // lld-link reads the import libraries through the developer environment's LIB, as link.exe does.
    auto const env = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(env.ready()) << env.describe();
    for (CompileConfig const config : kConfigs) {
        SCOPED_TRACE(configName(config));
        DiagnosticReporter rep;
        ASSERT_TRUE(buildSelectAnyObjects(dir, config, rep))
            << "DSS must write the select-any definition and its strong alias to a PE object; errs="
            << rep.errorCount();
        std::string const exe = std::string{"selectany_lld_"} + configName(config) + ".exe";
        ASSERT_TRUE(env.run("\"" + lld.string() + "\" /nologo /OUT:" + exe + " /ENTRY:" + test::pe_raw_entry::kSymbol
                            + " /SUBSYSTEM:CONSOLE /NODEFAULTLIB selectany.obj reader.obj entry.obj kernel32.lib"))
            << "lld-link must take the DSS objects: the strong alias is an external label of the COMDAT section";
        expectTheValueThroughTheAlias(dir / exe, "lld-link");
    }
}

TEST(PeObjectAliasReferenceLinkNative, DssLinksTheObjectsOfASelectAnyDefinitionAndItsStrongAlias) {
#if !defined(_WIN32)
    GTEST_SKIP() << "not a Windows host -- the image this links is run, and only Windows runs it";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-selectany-alias-dss"};
    auto const dir = scratch.path();
    for (CompileConfig const config : kConfigs) {
        SCOPED_TRACE(configName(config));
        DiagnosticReporter rep;
        ASSERT_TRUE(buildSelectAnyObjects(dir, config, rep))
            << "DSS must write the select-any definition and its strong alias to a PE object; errs="
            << rep.errorCount();
        // DSS's own link of the two OBJECTS (its own startup calls `main`; the raw entry is not linked).
        auto const out = dir / (std::string{"dss_"} + configName(config));
        fs::create_directories(out);
        Program p;
        p.setOutputDir(out);
        DiagnosticReporter linkRep;
        int const rc = p.compileFiles(
            std::vector<std::string>{(dir / "selectany.obj").string(), (dir / "reader.obj").string()}, "c",
            std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, linkRep);
        ASSERT_EQ(rc, 0) << "DSS must link its own PE objects of the select-any definition and its strong alias; errs="
                         << linkRep.errorCount();
        fs::path exe;
        for (auto const& e : fs::directory_iterator(out)) {
            if (e.path().extension() == ".exe") exe = e.path();
        }
        ASSERT_FALSE(exe.empty()) << "DSS wrote no image";
        expectTheValueThroughTheAlias(exe, "DSS");
    }
#endif
}
