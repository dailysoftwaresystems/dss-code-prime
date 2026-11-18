// P69 fold 3f (lane `xa`, ledger 549 (b)): A NAME WRITTEN WITHOUT THE PLATFORM'S RENAME BINDS AS THE PLATFORM'S LINK
// BINDS IT -- the binder half of the written-name routes (`ffi::writtenNameRoutesOn`, `stateWrittenNameRoutes`).
//
// `write` stands for the 17 POSIX names the UCRT exports only as `_<name>` (io.json states `linkerAlias` on pe; the
// seventeen are pinned in tests/ffi/test_written_name_routes.cpp): link.exe binds a written `write` through
// oldnames.lib's weak external to `_write`, MinGW-w64's ld through its import library's alias member over the same
// export. ✔MEASURED 2026-11-17 by probe-reference-cc runs 20261117-134551-cb87f606 (cl 19.52 + link.exe 14.52 /VERBOSE) and run 20261117-134619-3841ae20 (MinGW-w64
// gcc 13.2 + ld --trace-symbol), each fixture's program beside `int say(void); int main(void) { return say(); }`:
//   cl /MT `write`        -> OLDNAMES.lib(write.obj) -> libucrt.lib's `_write`; runs to 42
//   cl /MD `__imp_write`  -> OLDNAMES.lib -> `__imp__write`, imported from api-ms-win-crt-stdio-l1-1-0.dll; 42
//   cl /MT, the object's own `/alternatename:write=my_write` -> OLDNAMES' `write` still (my_write is never called); 42
//   MinGW gcc `write`     -> libmsvcrt.a's api-ms-win-crt-stdio member defines `write`; the image imports `_write`; 42
//   MinGW gcc `__iob_func` -> "undefined reference to `__iob_func'": the UCRT link refuses it
// The cells, each linked by DSS beside a DSS `main` (every leg, in-process; the images RUN in the `Native` twin on
// Windows x86_64):
//   C1 a foreign object writing `write` (cl /MT `write`, cl /MD `__imp_write`, MinGW gcc `write`): the image imports
//      `_write` from the C runtime's image and no `write`;
//   C2 DSS's own unit writing `write` through <io.h>: the same;
//   C3 a unit DEFINING `write` beside one writing it (DSS's own, and beside cl /MD's `__imp_write`): the program's
//      definition is called and the image imports no `_write` (the /MD form's meaning is decided: ISO C's one external
//      definition, link.exe /MT and GNU ld call the program's own; link.exe /MD alone calls the platform's -- ✔MEASURED
//      2026-11-17, lane `xa`'s batches GW4 and GM4, the WRITE_HDR unit beside a unit defining `write` as 7: cl /MT
//      exit 1, GNU ld exit 1 ("definition of write" in that unit), cl /MD exit 42 through OLDNAMES' `__imp_write`);
//   C4 a `.s` writing `call write`: linked, importing `_write` (the route decides, not the input kind); and each of
//      the seventeen the corpus states, per name;
//   C5 a user archive whose member defines `_write`, beside the foreign `write` object: the member is not used
//      (✔MEASURED 2026-11-17, GW4 and GM4, a member defining `_write` as 99: link.exe /MT and /MD search u.lib and
//      never load its member, exit 42; GNU ld binds libmsvcrt.a's own `write`, exit 42);
//   C6 an object stating `/alternatename:write=my_write`: the alias stands, and a warning names both;
//   C7 `__iob_func` written by a foreign object and by a `.s`: refused by name with the corpus's reason;
//   C8 on ELF nothing is stated: the `.s` `call write` imports `write` from the C library, as before.
// FIXTURES (tests/link/data/, from tests/link/data/written_name.source.c, each built twice in two directories and
// compared byte for byte, recording no host path):
//   written_name_write_x86_64_pe_cl_MT_O2.obj              1070 bytes, md5 7f1685d887a45595e8302f67d05dca1d
//   written_name_write_x86_64_pe_cl_MD_O2.obj              1083 bytes, md5 7203d2dc579c986e74fd04a612cf3c3e
//   written_name_alternatename_write_x86_64_pe_cl_MT_O2.obj 1208 bytes, md5 93b9d6e9384562898579a07384b1253c
//   written_name_write_x86_64_pe_gcc_O2.obj                 882 bytes, md5 9e681f710b7fc9cafcb225fc1e074d1f
//   written_name_iob_func_x86_64_pe_gcc_O2.obj              775 bytes, md5 633d9595b21cf7dd1d9d3b62b07eafd9

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"

#include "host_native_target.hpp"
#include "image_dependency_table.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr char const* kPeExec  = "x86_64:pe64-x86_64-windows-exec";
constexpr char const* kPeLib   = "x86_64:pe64-x86_64-windows-staticlib";
constexpr char const* kElfExec = "x86_64:elf64-x86_64-linux-exec";
constexpr char const* kAsm     = "asm-x86_64-att";

constexpr char const* kSayMain = "int say(void);\nint main(void) { return say(); }\n";

constexpr char const* kObjClMT   = "written_name_write_x86_64_pe_cl_MT_O2.obj";
constexpr char const* kObjClMD   = "written_name_write_x86_64_pe_cl_MD_O2.obj";
constexpr char const* kObjGcc    = "written_name_write_x86_64_pe_gcc_O2.obj";
constexpr char const* kObjAltNm  = "written_name_alternatename_write_x86_64_pe_cl_MT_O2.obj";
constexpr char const* kObjIobGcc = "written_name_iob_func_x86_64_pe_gcc_O2.obj";
constexpr char const* kWriteObjects[] = {kObjClMT, kObjClMD, kObjGcc};

[[nodiscard]] fs::path fixturePath(std::string_view name) {
    return dss::test::repoRoot() / "tests" / "link" / "data" / std::string{name};
}

[[nodiscard]] std::vector<std::uint8_t> bytesOf(fs::path const& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        ADD_FAILURE() << "cannot open " << path.string();
        return {};
    }
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

[[nodiscard]] std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

[[nodiscard]] fs::path writeSource(fs::path const& dir, std::string const& name, std::string_view text) {
    fs::create_directories(dir);
    fs::path const p = dir / name;
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

// DSS's artifact of `inputs` (sources of `language`, objects, archives through --resolve-library) for `spec`: its
// path, or empty with `rep` holding why.
[[nodiscard]] fs::path dssArtifact(fs::path const& dir, std::vector<fs::path> const& inputs, std::string_view language,
                                   std::string_view spec, std::vector<fs::path> const& archives,
                                   DiagnosticReporter& rep) {
    fs::path const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    if (!archives.empty()) p.setResolveLibraries(archives);
    std::vector<std::string> names;
    for (fs::path const& i : inputs) names.push_back(i.string());
    if (p.compileFiles(names, std::string{language}, std::vector<std::string>{std::string{spec}}, rep) != 0) return {};
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file()) return e.path();
    }
    return {};
}

[[nodiscard]] std::vector<std::string> importsOf(fs::path const& image) {
    return test_support::peImportedSymbols(bytesOf(image));
}

[[nodiscard]] bool has(std::vector<std::string> const& names, std::string_view n) {
    return std::find(names.begin(), names.end(), n) != names.end();
}

// `_write` is imported from the C runtime's image, and `write` is not imported at all.
void expectTheLinkNamesImport(fs::path const& image) {
    auto const symbols = importsOf(image);
    EXPECT_TRUE(has(symbols, "_write")) << "the image imports the link name `_write`";
    EXPECT_FALSE(has(symbols, "write")) << "no image of the platform exports `write`";
    std::vector<std::string> libraries;
    for (auto const& l : test_support::peImportedLibraries(bytesOf(image))) libraries.push_back(lower(l));
    EXPECT_TRUE(has(libraries, "ucrtbase.dll")) << "from the C runtime's image";
}

// The `.s` text writing `call <callee>` from `main` (Win64: the 32-byte shadow space and alignment).
[[nodiscard]] std::string asmCalling(std::string_view callee) {
    return std::string{"\t.text\n\t.globl\tmain\n\t.type\tmain, @function\nmain:\n\tsubq\t$40, %rsp\n"}
         + "\tmovl\t$1, %ecx\n\tleaq\tempty(%rip), %rdx\n\txorl\t%r8d, %r8d\n\tcall\t" + std::string{callee} + "\n"
         + "\tmovl\t$42, %eax\n\taddq\t$40, %rsp\n\tret\n\t.data\nempty:\n\t.byte\t0\n";
}

constexpr char const* kDssWrites =
    "#include <io.h>\nint main(void) { return write(1, \"\", 0) == 7 ? 42 : 1; }\n";
constexpr char const* kDssDefinesWrite =
    "int write(int fd, const void *buf, unsigned n) { (void)fd; (void)buf; (void)n; return 7; }\n";
constexpr char const* kDssCallsSayDefinesWrite =
    "int say(void);\n"
    "int write(int fd, const void *buf, unsigned n) { (void)fd; (void)buf; (void)n; return 0; }\n"
    "int main(void) { return say(); }\n";
constexpr char const* kDssArchiveMember =
    "int _write(int fd, const void *buf, unsigned n) { (void)fd; (void)buf; (void)n; return 99; }\n";

}  // namespace

// ══ C1: A FOREIGN OBJECT'S WRITTEN `write` ══════════════════════════════════════════════════════════════════════════
TEST(WrittenNameBinding, AForeignObjectsWrittenAliasImportsTheLinkName) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "written-name-routes-c1"};
    for (char const* name : kWriteObjects) {
        SCOPED_TRACE(name);
        fs::path const dir  = scratch.path() / name;
        fs::path const main = writeSource(dir, "main.c", kSayMain);
        DiagnosticReporter rep;
        auto const image = dssArtifact(dir, {main, fixturePath(name)}, "c", kPeExec, {}, rep);
        ASSERT_FALSE(image.empty()) << "DSS links the object as link.exe and GNU ld do:" << diagnosticsOf(rep);
        expectTheLinkNamesImport(image);
    }
}

// ══ C2: DSS'S OWN WRITTEN `write` (through <io.h>, which renames nothing) ═══════════════════════════════════════════
TEST(WrittenNameBinding, DssOwnWrittenAliasImportsTheLinkName) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "written-name-routes-c2"};
    fs::path const src = writeSource(scratch.path(), "main.c", kDssWrites);
    DiagnosticReporter rep;
    auto const image = dssArtifact(scratch.path(), {src}, "c", kPeExec, {}, rep);
    ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
    expectTheLinkNamesImport(image);
}

// ══ C3: THE PROGRAM'S OWN DEFINITION OF THE WRITTEN NAME WINS (F9) ═════════════════════════════════════════════════
TEST(WrittenNameBinding, AProgramDefiningTheWrittenNameImportsNoLinkName) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "written-name-routes-c3"};
    {
        SCOPED_TRACE("DSS's two units");
        fs::path const dir = scratch.path() / "dss";
        fs::path const tu1 = writeSource(dir, "defines.c", kDssDefinesWrite);
        fs::path const tu2 = writeSource(dir, "main.c", kDssWrites);
        DiagnosticReporter rep;
        auto const image = dssArtifact(dir, {tu1, tu2}, "c", kPeExec, {}, rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        EXPECT_FALSE(has(importsOf(image), "_write")) << "the program's `write` is called: no `_write` is imported";
        EXPECT_FALSE(has(importsOf(image), "write"));
    }
    {
        SCOPED_TRACE("cl /MD's `__imp_write` beside DSS's definition");
        fs::path const dir = scratch.path() / "md";
        fs::path const tu1 = writeSource(dir, "main.c", kDssCallsSayDefinesWrite);
        DiagnosticReporter rep;
        auto const image = dssArtifact(dir, {tu1, fixturePath(kObjClMD)}, "c", kPeExec, {}, rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        EXPECT_FALSE(has(importsOf(image), "_write"))
            << "the program's own definition, through a pointer of the image's own (the fold's LNK4217 answer); "
               "link.exe /MD alone calls the platform's -- decided: ISO C, link.exe /MT and GNU ld";
    }
}

// ══ C4 / C8: A `.s` WRITING `call write` ═══════════════════════════════════════════════════════════════════════════
TEST(WrittenNameBinding, AnAssemblyUnitsWrittenAliasLinksOnPeAndStatesNothingOnElf) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "written-name-routes-c4"};
    fs::path const src = writeSource(scratch.path(), "writes.s", asmCalling("write"));
    {
        SCOPED_TRACE(kPeExec);
        DiagnosticReporter rep;
        auto const image = dssArtifact(scratch.path() / "pe", {src}, kAsm, kPeExec, {}, rep);
        ASSERT_FALSE(image.empty()) << "the route decides, not the input kind:" << diagnosticsOf(rep);
        expectTheLinkNamesImport(image);
    }
    {
        SCOPED_TRACE(kElfExec);
        DiagnosticReporter rep;
        auto const image = dssArtifact(scratch.path() / "elf", {src}, kAsm, kElfExec, {}, rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        auto const bytes = bytesOf(image);
        EXPECT_EQ(test_support::peImportedSymbols(bytes).size(), 0u) << "an ELF image has no PE import table";
        EXPECT_EQ(rep.errorCount(), 0u);
    }
}

// ══ C4, PER NAME: EVERY ROW THE CORPUS STATES AS THE PLATFORM'S ALIAS ════════════════════════════════════════════
// `write` stands for the seventeen in the cells above; here each is linked itself (a template generalizes code, not
// the measurement): a `.s` calling the WRITTEN name on pe imports the row's link name and never the written one.
// The names are read off every shipped descriptor (a row stating `writtenNameRoute: {pe: linkerAlias}`), never
// retyped; their link names are the corpus's own (`_<name>`, pinned by tests/ffi/test_written_name_routes.cpp).
TEST(WrittenNameBinding, EveryStatedAliasImportsItsLinkNameAndNeverTheWrittenName) {
    std::set<std::string> aliased;
    for (auto const& e : fs::recursive_directory_iterator{dss::test::configRoot() / "shippedLibs"}) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        std::ifstream in{e.path(), std::ios::binary};
        json const doc = json::parse(in);
        if (!doc.contains("symbols")) continue;
        for (json const& s : doc.at("symbols")) {
            if (s.is_object() && s.contains("writtenNameRoute") && s.at("writtenNameRoute") == json({{"pe", "linkerAlias"}}))
                aliased.insert(s.at("name").get<std::string>());
        }
    }
    ASSERT_EQ(aliased.size(), 17u) << "the corpus states the alias on the seventeen it was measured on";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "written-name-binding-each"};
    for (std::string const& name : aliased) {
        SCOPED_TRACE(name);
        fs::path const dir = scratch.path() / name;
        DiagnosticReporter rep;
        auto const image = dssArtifact(dir, {writeSource(dir, "calls.s", asmCalling(name))}, kAsm, kPeExec, {}, rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        auto const symbols = importsOf(image);
        EXPECT_TRUE(has(symbols, "_" + name)) << "the image imports the row's link name";
        EXPECT_FALSE(has(symbols, name)) << "no image of the platform exports the written name";
    }
}

// ══ C5: A USER ARCHIVE DEFINING THE LINK NAME IS NOT USED FOR THE WRITTEN NAME ════════════════════════════════════
TEST(WrittenNameBinding, AnArchiveDefiningTheLinkNameIsNotUsedForTheWrittenName) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "written-name-routes-c5"};
    fs::path const libDir = scratch.path() / "lib";
    fs::path const member = writeSource(libDir, "member.c", kDssArchiveMember);
    DiagnosticReporter libRep;
    auto const archive = dssArtifact(libDir, {member}, "c", kPeLib, {}, libRep);
    ASSERT_FALSE(archive.empty()) << diagnosticsOf(libRep);
    fs::path const dir  = scratch.path() / "exe";
    fs::path const main = writeSource(dir, "main.c", kSayMain);
    DiagnosticReporter rep;
    auto const image = dssArtifact(dir, {main, fixturePath(kObjGcc)}, "c", kPeExec, {archive}, rep);
    ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
    expectTheLinkNamesImport(image);   // the member's `_write` would have left no import of it
}

// ══ C6: THE ALIAS STANDS OVER AN OBJECT'S OWN FALLBACK, AND THE DROPPED ONE IS NAMED ═══════════════════════════════
TEST(WrittenNameBinding, TheAliasStandsOverAnObjectsOwnFallback) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "written-name-routes-c6"};
    fs::path const main = writeSource(scratch.path(), "main.c", kSayMain);
    DiagnosticReporter rep;
    auto const image = dssArtifact(scratch.path(), {main, fixturePath(kObjAltNm)}, "c", kPeExec, {}, rep);
    ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
    expectTheLinkNamesImport(image);
    bool warned = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_LinkerDirectiveUnhonourable && d.severity == DiagnosticSeverity::Warning
            && d.actual.find("my_write") != std::string::npos && d.actual.find("_write") != std::string::npos)
            warned = true;
    }
    EXPECT_TRUE(warned) << "the dropped fallback is named beside the one that stands:" << diagnosticsOf(rep);
}

// ══ C7: A WRITTEN NAME THE CORPUS REFUSES IS REFUSED BY NAME, WITH ITS REASON ══════════════════════════════════════
TEST(WrittenNameBinding, ARefusedWrittenNameIsRefusedByNameWithItsReason) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "written-name-routes-c7"};
    auto const expectRefused = [](DiagnosticReporter const& rep) {
        bool named = false;
        for (auto const& d : rep.all()) {
            if (d.code == DiagnosticCode::F_ShippedSymbolUnavailableForTarget
                && d.actual.find("__iob_func") != std::string::npos && d.actual.find("msvcrt") != std::string::npos)
                named = true;
        }
        EXPECT_TRUE(named) << "refused by name, with the reason the corpus states:" << diagnosticsOf(rep);
    };
    {
        SCOPED_TRACE("a foreign object");
        fs::path const dir  = scratch.path() / "object";
        fs::path const main = writeSource(dir, "main.c", kSayMain);
        DiagnosticReporter rep;
        EXPECT_TRUE(dssArtifact(dir, {main, fixturePath(kObjIobGcc)}, "c", kPeExec, {}, rep).empty());
        expectRefused(rep);
    }
    {
        SCOPED_TRACE("a `.s`");
        fs::path const dir = scratch.path() / "asm";
        fs::path const src = writeSource(dir, "iob.s", asmCalling("__iob_func"));
        DiagnosticReporter rep;
        EXPECT_TRUE(dssArtifact(dir, {src}, kAsm, kPeExec, {}, rep).empty());
        expectRefused(rep);
    }
}

// ══ THE IMAGES RUN (Windows x86_64) ════════════════════════════════════════════════════════════════════════════════
TEST(WrittenNameBindingNative, EachImageRunsAsTheReferenceLinkersProgramDoes) {
    if (test_support::hostNativeTarget().execTarget != kPeExec) {
        GTEST_SKIP() << "the images run only where the host is Windows x86_64";
    }
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "written-name-routes-run"};
    auto const runsTo = [](fs::path const& image, unsigned expected, std::string_view why) {
        ASSERT_FALSE(image.empty());
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, expected) << why;
    };
    for (char const* name : {kObjClMT, kObjClMD, kObjGcc, kObjAltNm}) {
        SCOPED_TRACE(name);
        fs::path const dir  = scratch.path() / name;
        fs::path const main = writeSource(dir, "main.c", kSayMain);
        DiagnosticReporter rep;
        runsTo(dssArtifact(dir, {main, fixturePath(name)}, "c", kPeExec, {}, rep), 42u,
               "the C runtime's `_write` returned 0, as under the object's own reference linker");
    }
    {
        SCOPED_TRACE("DSS's two units");
        fs::path const dir = scratch.path() / "dss";
        DiagnosticReporter rep;
        runsTo(dssArtifact(dir, {writeSource(dir, "defines.c", kDssDefinesWrite), writeSource(dir, "main.c", kDssWrites)},
                           "c", kPeExec, {}, rep),
               42u, "the program's own `write` (7) is called");
    }
    {
        SCOPED_TRACE("the `.s`");
        fs::path const dir = scratch.path() / "asm";
        DiagnosticReporter rep;
        runsTo(dssArtifact(dir, {writeSource(dir, "writes.s", asmCalling("write"))}, kAsm, kPeExec, {}, rep), 42u,
               "the C runtime's `_write` is called and returns");
    }
}
