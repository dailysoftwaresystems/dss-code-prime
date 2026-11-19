// P69 fold 3d (lane xa) -- the /GS security cookie is SEEDED before any other code of the image runs
// (D-LK-PE-FOREIGN-OBJECT-GS-COOKIE-ROUTINES-UNDEFINED's invariant: "both cookies are seeded by ONE routine before any
// guarded frame runs"). The routine is the one the image's document names, `compilerSupport.
// securityCookieInitializer`, and the call is the FIRST before-entry call of the entry trampoline
// (`injectEntryTrampoline`, src/link/entry_trampoline.cpp) whenever the linked program defines that routine.
//
// Every reference C runtime makes that call before any initializer and before `main`: MSVC's `mainCRTStartup` and
// mingw-w64's, whether or not an object was compiled /GS. Until fold 3d no code of a DSS image called it, so a cl /GS
// object's frames compared against MSVC's shipped x64 cookie, 0x00002B992DDFA232, which protects nothing.
//
// THE UNIVERSE, by document and by object:
//   * every EXEC-flavored pe64 document that states an initializer (enumerated from the shipped documents, never
//     listed): the trampoline calls it first when the program defines it, and calls nothing when the program does not
//     -- the image of a program with no foreign object, which names nothing of the runtime and so links none of its
//     members. The DLL document states one too, and its cells are CARRIED: a DSS DLL runs no code of its own at load
//     ([[D-LK-SHARED-LIBRARY-RUNS-NO-STATIC-INITIALIZERS]]), so its cookie stays the shipped value, while link.exe's
//     DLL entry `_DllMainCRTStartup` seeds it (the Native DLL cell below holds both readings). The staticlib and
//     relocatable documents start nothing.
//   * the objects: `tests/link/data/pe_security_cookie_init_x86_64_pe_cl_MD_{O2,Od}_GS.obj` (cl 19.51 /GS, the source
//     states how each was built), each linked ALONE -- its own `main` is the entry -- by DSS, link.exe and lld-link;
//     each program exits 42 when its cookie was seeded and 7 when it was not. clang-cl's two cells
//     (`..._clangcl_MD_{O2,Od}_GS.obj`) are UNMEASURED: no leg's PATH has a clang-cl (probe-reference-cc run
//     20261104-200657-1038ae7c, windows-x86_64-release, `compiler=absent`).
//   * /GS-: a /GS- object names no routine of the family, but the stack probe's member names the cookie (its
//     `__security_check_cookie` reads it), so a /GS- object with a frame past a page links the initializer and the
//     trampoline calls it, as the reference startups call it for every program. That cell is
//     `AGsOffObjectWhoseFrameNeedsTheProbeLinksTheInitializer` and its Native run, at the end of this file, over
//     `pe_security_cookie_probe_x86_64_pe_cl_MD_O2_GSoff.obj` (cl 19.51 /O2 /GS-, whose frame calls `__chkstk`).
//
// WHICH LEG READS WHAT: the in-process cases run on every leg. The Native cases run on Windows x86_64 alone, and
// windows-x86_64-release (built by MSVC, inside its developer environment) must read EVERY half: there a missing
// link.exe is RED. lld-link's half is skipped only where no lld-link exists, and the case then says so.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"
#include "link/entry_trampoline.hpp"
#include "link/object_format_schema.hpp"
#include "lir/lir.hpp"
#include "program/program.hpp"

#include "../core/native_c_probe.hpp"
#include "host_native_target.hpp"
#include "image_dependency_table.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::linker;

namespace {

namespace fs = std::filesystem;

constexpr char const* kExec = "x86_64:pe64-x86_64-windows-exec";
constexpr char const* kDll  = "x86_64:pe64-x86_64-windows-dll";

constexpr char const* kCookieObjects[] = {"pe_security_cookie_init_x86_64_pe_cl_MD_O2_GS.obj",
                                          "pe_security_cookie_init_x86_64_pe_cl_MD_Od_GS.obj"};
constexpr char const* kSeededExit = "42 is the cookie seeded before `main`; 7 is the cookie the runtime's object ships";

// The documents that state an initializer and start no program, each with what becomes of its cells. A document
// added to that set fails the enumeration below until it is named here.
struct NonExecDocument {
    char const* name;
    char const* cells;
};
constexpr NonExecDocument kNonExecDocuments[] = {
    {"pe64-x86_64-windows-dll", "CARRIED: a DSS DLL runs no code of its own at load "
                                "(D-LK-SHARED-LIBRARY-RUNS-NO-STATIC-INITIALIZERS)"},
    {"pe64-x86_64-windows-staticlib", "starts nothing: a static library's members are linked by another image"},
    {"pe64-x86_64-windows", "starts nothing: a relocatable object is linked by another image"},
};

// ── the in-process half: the trampoline ──────────────────────────────────────────────────────────────────────────

constexpr std::uint32_t kEntry       = 1;
constexpr std::uint32_t kInitializer = 2;
constexpr std::uint32_t kCtor        = 3;

// A program of `ret`-only functions: the entry (1), a before-entry static initializer (3), and -- when `initializer`
// says how it is bound -- a definition of the document's initializer name (2).
[[nodiscard]] AssembledModule programOf(std::string const& initializerName,
                                        std::optional<SymbolBinding> initializer) {
    AssembledModule mod;
    auto const add = [&](std::uint32_t id, std::string name, SymbolBinding binding) {
        AssembledFunction fn;
        fn.symbol = SymbolId{id};
        fn.bytes  = {0xC3};   // ret
        mod.functions.push_back(std::move(fn));
        ModuleSymbol row;
        row.symbol  = SymbolId{id};
        row.name    = std::move(name);
        row.binding = binding;
        mod.symbols.push_back(std::move(row));
    };
    add(kEntry, "main", SymbolBinding::Global);
    if (initializer.has_value()) add(kInitializer, initializerName, *initializer);
    add(kCtor, "ctor", SymbolBinding::Local);
    mod.expectedFuncCount = mod.functions.size();
    mod.userEntrySymbol   = SymbolId{kEntry};
    StaticInitSchedule before;
    before.setPriorityFor(StaticInitPhase::BeforeEntry, kUnprioritizedStaticInit);
    mod.staticInitSchedule = {LirStaticInitEntry{SymbolId{kCtor}, before}};
    return mod;
}

// The program's own functions the trampoline calls, in the order it calls them.
[[nodiscard]] std::vector<std::uint32_t> callsOfTheTrampoline(AssembledModule const& m) {
    std::vector<Relocation> relocs = m.functions.front().relocations;
    std::sort(relocs.begin(), relocs.end(),
              [](Relocation const& a, Relocation const& b) { return a.offset < b.offset; });
    std::vector<std::uint32_t> out;
    for (auto const& r : relocs) {
        if (r.target.v == kEntry || r.target.v == kInitializer || r.target.v == kCtor) out.push_back(r.target.v);
    }
    return out;
}

[[nodiscard]] std::string namesOf(std::vector<std::uint32_t> const& calls) {
    std::string s;
    for (auto const c : calls) {
        s += c == kEntry ? " entry" : c == kInitializer ? " initializer" : " ctor";
    }
    return s.empty() ? std::string{" (none)"} : s;
}

// A shipped target whose calling conventions hold the document's entry convention -- the pairing the documents
// themselves imply, never a name written here.
[[nodiscard]] std::shared_ptr<TargetSchema const> targetFor(ObjectFormatSchema const& format) {
    auto const root = dss::test::findConfigRoot();
    if (!root) return nullptr;
    for (auto const& e : fs::directory_iterator(*root / "targets")) {
        auto const leaf = e.path().filename().string();
        auto const at   = leaf.find(".target.json");
        if (!e.is_regular_file() || at == std::string::npos) continue;
        auto t = TargetSchema::loadShipped(leaf.substr(0, at));
        if (!t.has_value()) continue;
        if ((*t)->callingConventionByName(format.entryCallingConvention()) != nullptr) return *t;
    }
    return nullptr;
}

struct StatedDocument {
    std::string                               name;
    std::shared_ptr<ObjectFormatSchema const> format;
};

// Every shipped document that states a security-cookie initializer, enumerated from the directory.
[[nodiscard]] std::vector<StatedDocument> documentsStatingAnInitializer() {
    std::vector<StatedDocument> out;
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return out;
    }
    for (auto const& e : fs::directory_iterator(*root / "object-formats")) {
        auto const leaf = e.path().filename().string();
        auto const at   = leaf.find(".format.json");
        if (!e.is_regular_file() || at == std::string::npos) continue;
        auto const name = leaf.substr(0, at);
        auto       f    = ObjectFormatSchema::loadShipped(name);
        EXPECT_TRUE(f.has_value()) << name << " does not load";
        if (!f.has_value() || !(*f)->compilerSupport().has_value()) continue;
        out.push_back(StatedDocument{name, *f});
    }
    return out;
}

// ── the image half ───────────────────────────────────────────────────────────────────────────────────────────────

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

// DSS's image of `inputs` (paths) for `target`: its path, or empty with `rep` holding why.
[[nodiscard]] fs::path dssImageOf(fs::path const& dir, std::vector<std::string> const& inputs, DiagnosticReporter& rep,
                                  char const* target) {
    auto const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    if (p.compileFiles(inputs, "c", std::vector<std::string>{target}, rep) != 0) return {};
    std::string const want = std::string_view{target} == kDll ? ".dll" : ".exe";
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file() && e.path().extension() == want) return e.path();
    }
    return {};
}

void copyFixture(std::string_view name, fs::path const& dir) {
    auto const bytes = bytesOf(fixturePath(name));
    ASSERT_FALSE(bytes.empty());
    std::ofstream(dir / std::string{name}, std::ios::binary)
        .write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] bool onWindowsX64() { return test_support::hostNativeTarget().execTarget == kExec; }

// The MSVC-built leg runs inside the developer environment that built it, so its link.exe is never absent there.
constexpr bool kBuiltByMsvc =
#if defined(_MSC_VER) && !defined(__clang__)
    true;
#else
    false;
#endif

constexpr char const* kEveryHalfLeg =
    "windows-x86_64-release (built by MSVC, in its developer environment) must read every half";

}  // namespace

// ══ THE TRAMPOLINE, ON EVERY LEG ═════════════════════════════════════════════════════════════════════════════════

// Every exec-flavored document that states an initializer: a program that defines it calls it FIRST -- before the
// static initializers, before the entry. RED-ON-DISABLE: emit no call of the initializer -> the calls read
// `ctor entry`.
TEST(PeSecurityCookieInit, EachExecDocumentThatStatesAnInitializerCallsItFirst) {
    auto const documents = documentsStatingAnInitializer();
    std::size_t exec = 0;
    for (auto const& d : documents) {
        SCOPED_TRACE(d.name);
        if (!d.format->isExecFlavor()) continue;
        ++exec;
        auto const target = targetFor(*d.format);
        ASSERT_NE(target, nullptr) << "no shipped target holds the document's entry convention";
        AssembledModule m = programOf(d.format->compilerSupport()->securityCookieInitializerMangledName,
                                      SymbolBinding::Global);
        DiagnosticReporter rep;
        ASSERT_TRUE(injectEntryTrampoline(m, *target, *d.format, rep)) << diagnosticsOf(rep);
        bool const runsSchedule = d.format->staticInitRunner() == StaticInitRunner::EntryTrampoline;
        auto const want = runsSchedule ? std::vector<std::uint32_t>{kInitializer, kCtor, kEntry}
                                       : std::vector<std::uint32_t>{kInitializer, kEntry};
        EXPECT_EQ(callsOfTheTrampoline(m), want)
            << "the trampoline calls" << namesOf(callsOfTheTrampoline(m)) << "; the cookie is seeded FIRST, before "
               "any initializer or the entry can run a guarded frame";
    }
    EXPECT_GE(exec, 1u) << "no exec-flavored document states an initializer: the enumeration read nothing";
}

// A program that does not define the initializer -- a program with no foreign object links no member of the runtime
// -- calls nothing more than its initializers and its entry, and a FILE-LOCAL function of that name is that unit's
// own, never the routine. RED-ON-DISABLE: call the name whatever binds it -> the file-local row is called.
TEST(PeSecurityCookieInit, AProgramThatDefinesNoInitializerCallsNone) {
    for (auto const& d : documentsStatingAnInitializer()) {
        SCOPED_TRACE(d.name);
        if (!d.format->isExecFlavor()) continue;
        auto const target = targetFor(*d.format);
        ASSERT_NE(target, nullptr);
        bool const runsSchedule = d.format->staticInitRunner() == StaticInitRunner::EntryTrampoline;
        auto const want = runsSchedule ? std::vector<std::uint32_t>{kCtor, kEntry} : std::vector<std::uint32_t>{kEntry};
        auto const& name = d.format->compilerSupport()->securityCookieInitializerMangledName;
        for (std::optional<SymbolBinding> const binding : {std::optional<SymbolBinding>{},
                                                           std::optional<SymbolBinding>{SymbolBinding::Local}}) {
            SCOPED_TRACE(binding.has_value() ? "a file-local function of the name" : "no function of the name");
            AssembledModule m = programOf(name, binding);
            DiagnosticReporter rep;
            ASSERT_TRUE(injectEntryTrampoline(m, *target, *d.format, rep)) << diagnosticsOf(rep);
            EXPECT_EQ(callsOfTheTrampoline(m), want) << "the trampoline calls" << namesOf(callsOfTheTrampoline(m));
        }
    }
}

// The documents that state an initializer and start nothing are each named, with what becomes of their cells.
TEST(PeSecurityCookieInit, EachDocumentThatStatesAnInitializerAndStartsNothingIsNamed) {
    for (auto const& d : documentsStatingAnInitializer()) {
        if (d.format->isExecFlavor()) continue;
        auto const named = std::find_if(std::begin(kNonExecDocuments), std::end(kNonExecDocuments),
                                        [&](NonExecDocument const& n) { return d.name == n.name; });
        EXPECT_NE(named, std::end(kNonExecDocuments))
            << d.name << " states an initializer and starts no program: name what becomes of its cells";
    }
}

// ══ THE IMAGES ═══════════════════════════════════════════════════════════════════════════════════════════════════

// Every leg: DSS links each cl /GS object ALONE (its own `main` is the entry), and the cookie and its routines are
// defined in the image -- none imported.
TEST(PeSecurityCookieInit, DssLinksEachClObjectAlone) {
    for (auto const* object : kCookieObjects) {
        SCOPED_TRACE(object);
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-cookie-init-link"};
        DiagnosticReporter rep;
        auto const image = dssImageOf(scratch.path(), {fixturePath(object).string()}, rep, kExec);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        auto const imported = test_support::peImportedSymbols(bytesOf(image));
        for (auto const* n : {"__security_cookie", "__security_check_cookie", "__security_init_cookie",
                              "__GSHandlerCheck"}) {
            EXPECT_EQ(std::find(imported.begin(), imported.end(), n), imported.end())
                << n << " is imported; the compilerSupport runtime defines it in the image";
        }
    }
}

// Windows x86_64: each object's program, linked alone by DSS, by link.exe and by lld-link, runs with its cookie
// seeded. ✔MEASURED, the reference's own reading: cl /O2 /GS /MD compiled, linked by link.exe and run, exit 42
// (probe-reference-cc run 20261104-200657-1038ae7c, windows-x86_64-release). RED-ON-DISABLE: emit no call of the
// initializer -> DSS's programs exit 7.
TEST(PeSecurityCookieInitNative, EachObjectRunsSeededUnderDssLinkExeAndLldLink) {
    if (!onWindowsX64()) GTEST_SKIP() << "the programs run only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-cookie-init-native"};
    auto const dir = scratch.path();
    for (auto const* object : kCookieObjects) {
        SCOPED_TRACE(object);
        DiagnosticReporter rep;
        auto const image = dssImageOf(dir / ("dss_" + std::string{object}), {fixturePath(object).string()}, rep, kExec);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "DSS: " << kSeededExit;
    }
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent() && !kBuiltByMsvc) GTEST_SKIP() << "DSS's half was read; link.exe's was NOT: " << msvc.detail
                                                         << ". " << kEveryHalfLeg;
    ASSERT_TRUE(msvc.ok()) << msvc.describe() << ". " << kEveryHalfLeg;
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    auto const lld = test_support::native_probe::locateLldLink(dir);
    ASSERT_TRUE(lld.ok() || lld.toolAbsent()) << lld.describe();
    std::vector<std::pair<std::string, char const*>> linkers{{"link", "link.exe"}};
    if (lld.ok()) linkers.emplace_back("\"" + lld.lldLink.string() + "\"", "lld-link");
    std::size_t k = 0;
    for (auto const* object : kCookieObjects) {
        copyFixture(object, dir);
        for (auto const& [linker, who] : linkers) {
            SCOPED_TRACE(std::string{who} + ", " + object);
            std::string const exe = "ref" + std::to_string(k++) + ".exe";
            ASSERT_TRUE(tools.run(linker + " /nologo /OUT:" + exe + " " + object)) << who << " links the object alone";
            auto const r = test_support::runBinary(dir / exe);
            ASSERT_TRUE(r.spawned) << r.diagnostic;
            EXPECT_FALSE(r.timedOut);
            EXPECT_EQ(r.exitCode, 42u) << who << ": " << kSeededExit;
        }
    }
    if (linkers.size() == 1u) {
        GTEST_SKIP() << "DSS's and link.exe's halves were read; lld-link's was NOT: " << lld.detail;
    }
}

// Windows x86_64: the DLL cells, CARRIED to D-LK-SHARED-LIBRARY-RUNS-NO-STATIC-INITIALIZERS. One unit reading its
// image's cookie beside a cl /GS object (lane hm's, which names the family), as a DLL: link.exe's, whose entry
// `_DllMainCRTStartup` seeds the cookie at load (42), and DSS's, which runs no code at load, so its cookie is the
// shipped value (7). Each is loaded by a DSS program that calls `dll_cookie`. The day the shared-library row closes,
// DSS's DLL reads 42 and this case's DSS line is changed with it.
TEST(PeSecurityCookieInitNative, ADllsCookieIsSeededByLinkExeAndCarriedForDss) {
    if (!onWindowsX64()) GTEST_SKIP() << "the DLLs load only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-cookie-init-dll"};
    auto const dir = scratch.path();
    constexpr char const* kGsObject = "pe_compiler_support_x86_64_pe_cl_MD_O2_GS.obj";
    constexpr char const* kDllUnit  = R"(#include <stdint.h>
extern uintptr_t __security_cookie;
int cs_guarded(char const *);
int dll_cookie(void) {
    return cs_guarded("abc") == 'a' + 'b' + 'c' && __security_cookie != (uintptr_t)0x00002B992DDFA232ull ? 42 : 7;
}
)";
    constexpr char const* kLoader = R"(#include <windows.h>
typedef int (*reader)(void);
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    HMODULE const m = LoadLibraryA(argv[1]);
    if (m == NULL) return 3;
    reader const r = (reader)GetProcAddress(m, "dll_cookie");
    if (r == NULL) return 4;
    return r();
}
)";
    std::ofstream(dir / "dll.c", std::ios::binary) << kDllUnit;
    std::ofstream(dir / "loader.c", std::ios::binary) << kLoader;
    DiagnosticReporter lrep;
    auto const loader = dssImageOf(dir / "loader", {(dir / "loader.c").string()}, lrep, kExec);
    ASSERT_FALSE(loader.empty()) << diagnosticsOf(lrep);

    DiagnosticReporter drep;
    auto const dssDll =
        dssImageOf(dir / "dssdll", {(dir / "dll.c").string(), fixturePath(kGsObject).string()}, drep, kDll);
    ASSERT_FALSE(dssDll.empty()) << diagnosticsOf(drep);
    auto const fromDss = test_support::runBinary(loader, test_support::kRunBudget, false, {}, {dssDll.string()});
    ASSERT_TRUE(fromDss.spawned) << fromDss.diagnostic;
    EXPECT_EQ(fromDss.exitCode, 7u)
        << "CARRIED (D-LK-SHARED-LIBRARY-RUNS-NO-STATIC-INITIALIZERS): DSS's DLL runs nothing at load, so its cookie "
           "is the shipped value; 3 or 4 is a DLL that did not load or export `dll_cookie`";

    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent() && !kBuiltByMsvc) GTEST_SKIP() << "DSS's DLL was read; link.exe's was NOT: " << msvc.detail
                                                         << ". " << kEveryHalfLeg;
    ASSERT_TRUE(msvc.ok()) << msvc.describe() << ". " << kEveryHalfLeg;
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    copyFixture(kGsObject, dir);
    ASSERT_TRUE(tools.run("cl /nologo /c /O2 /GS- /MD dll.c /Fodll.obj")) << "cl compiles the DLL's unit";
    ASSERT_TRUE(tools.run(std::string{"link /nologo /DLL /EXPORT:dll_cookie /OUT:ref.dll dll.obj "} + kGsObject))
        << "link.exe links the DLL";
    auto const fromLinkExe =
        test_support::runBinary(loader, test_support::kRunBudget, false, {}, {(dir / "ref.dll").string()});
    ASSERT_TRUE(fromLinkExe.spawned) << fromLinkExe.diagnostic;
    EXPECT_EQ(fromLinkExe.exitCode, 42u) << "link.exe's DLL entry seeds the cookie at load";
}

// ══ THE /GS- CELL: A STACK PROBE LINKS THE INITIALIZER (ledger 483) ═════════════════════════════════════════════════
//
// `pe_security_cookie_probe_x86_64_pe_cl_MD_O2_GSoff.obj` (cl 19.51 /O2 /GS-, the source states how it was built)
// names no routine of the /GS family; its one frame is past a page, so its prologue calls `__chkstk`. DSS's probe is
// in the runtime's assembly member beside `__security_check_cookie`, which reads `__security_cookie`, whose member
// defines `__security_init_cookie` (READ: `runtime/platform/src/pe_compiler_support_x86_64.s` and `.c`). So the
// program DEFINES the initializer, and the trampoline's rule -- a program that defines it calls it first
// (`EachExecDocumentThatStatesAnInitializerCallsItFirst`) -- calls it, as every reference startup does for every
// program. The witness that the initializer's member is in the image: it alone of the program calls
// `QueryPerformanceCounter` (its seed), which the image then imports, while the same `main` with no foreign object
// links no member of the runtime and imports no such name.
namespace {

constexpr char const* kProbeObject = "pe_security_cookie_probe_x86_64_pe_cl_MD_O2_GSoff.obj";
constexpr char const* kProbeMain   = "int probe_touch(int);\nint main(void) { return probe_touch(1); }\n";
constexpr char const* kAloneMain   = "int probe_touch(int k) { return k + 41; }\nint main(void) { return probe_touch(1); }\n";

[[nodiscard]] std::size_t occurrences(std::vector<std::uint8_t> const& hay, std::string_view needle) {
    std::size_t n = 0;
    auto it = hay.begin();
    while ((it = std::search(it, hay.end(), needle.begin(), needle.end())) != hay.end()) {
        ++n;
        ++it;
    }
    return n;
}

[[nodiscard]] fs::path probeImage(fs::path const& dir, char const* main, bool withObject, DiagnosticReporter& rep) {
    fs::create_directories(dir);
    auto const src = dir / "main.c";
    std::ofstream(src, std::ios::binary) << main;
    std::vector<std::string> inputs{src.string()};
    if (withObject) inputs.push_back(fixturePath(kProbeObject).string());
    return dssImageOf(dir, inputs, rep, kExec);
}

}  // namespace

// Every leg: the object names `__chkstk` and nothing of the /GS family; DSS links it beside a C `main`, and the image
// imports the initializer's seed, `QueryPerformanceCounter`, which the same `main` without the object does not.
// RED-ON-DISABLE: give the probe a member of its own that names no cookie -> the image imports no seed.
TEST(PeSecurityCookieInit, AGsOffObjectWhoseFrameNeedsTheProbeLinksTheInitializer) {
    auto const object = bytesOf(fixturePath(kProbeObject));
    ASSERT_FALSE(object.empty());
    EXPECT_GE(occurrences(object, "__chkstk"), 1u) << "the object's frame calls the stack probe";
    EXPECT_EQ(occurrences(object, "__security_"), 0u) << "a /GS- object names no routine of the /GS family";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-cookie-init-probe"};
    DiagnosticReporter rep;
    auto const image = probeImage(scratch.path() / "probe", kProbeMain, true, rep);
    ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
    auto const imported = test_support::peImportedSymbols(bytesOf(image));
    EXPECT_NE(std::find(imported.begin(), imported.end(), "QueryPerformanceCounter"), imported.end())
        << "the initializer's member is in the image: it alone calls `QueryPerformanceCounter`";
    DiagnosticReporter alone;
    auto const control = probeImage(scratch.path() / "alone", kAloneMain, false, alone);
    ASSERT_FALSE(control.empty()) << diagnosticsOf(alone);
    auto const controlImports = test_support::peImportedSymbols(bytesOf(control));
    EXPECT_EQ(std::find(controlImports.begin(), controlImports.end(), "QueryPerformanceCounter"),
              controlImports.end())
        << "THE NEGATIVE: a program with no foreign object links no member of the runtime";
}

// Windows x86_64: the program runs, its initializer called before `main` (42).
TEST(PeSecurityCookieInitNative, AGsOffObjectWhoseFrameNeedsTheProbeRuns) {
    if (!onWindowsX64()) GTEST_SKIP() << "the program runs only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-cookie-init-probe-native"};
    DiagnosticReporter rep;
    auto const image = probeImage(scratch.path(), kProbeMain, true, rep);
    ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
    auto const r = test_support::runBinary(image, test_support::kRunBudget);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u) << "42 is the probe's frame touched at both ends";
}
