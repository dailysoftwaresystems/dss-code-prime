// D-LK-LIBRARY-FUNCTION-ADDRESS-IS-THE-IMAGE-STUB (P69 re-review MAJOR 2) — a DSS pe64 RELOCATABLE object's `&puts`,
// in its code and in its own statics, as the REFERENCE PE linkers bind it.
//
// C 6.5.9p6: two pointers to the same function compare equal. Within one image that must hold under EVERY linker
// that links a DSS object, not only under DSS's own. A COFF object can name a library function's address two ways:
// the function's own name (`puts`), which every PE linker resolves from the import library to the image's import
// THUNK, or the import's IAT entry (`__imp_puts`), which holds the address the loader binds. MSVC `/MD` code loads
// `&puts` from `__imp_puts` while its static initializers keep `puts` — the documented split of its C mode, which
// gives one function two addresses in one image (✔MEASURED 2026-10-06, cl 19.51 /O2 /MD: `code == static` is false,
// probe run 20261006-212638-3ab25596); mingw-w64 gcc 13.2.0 writes `puts` everywhere (`code == static`, -O0 and -O2,
// probe run 20261006-212648-2547f2f2).
//
// TWO TIERS, NEITHER REPLACES THE OTHER (the shape of `test_pe_object_unwind_reference_link.cpp`):
//   * TIER 1 (`PeObjectFunctionAddress`) — HOST-INDEPENDENT: what the production driver writes. Every relocation the
//     object writes against `puts` is the same absolute pointer relocation — its two statics' and the one of the
//     pointer its code loads `&puts` from — and nothing in it names `__imp_puts`.
//   * TIER 2 (`PeObjectFunctionAddressReferenceLinkNative`) — the RUNTIME witness: the SAME objects linked by
//     link.exe, by GNU ld, by lld-link where the host has it, and by DSS itself (the member archived by lib.exe), each
//     image RUN. Under every linker the member's code form, its static and its const static are one value; under
//     DSS's link that value is the one GetProcAddress answers (design c2), and under a foreign linker it is the
//     image's import thunk, never GetProcAddress's (MinGW's meaning). Windows-only; each arm SKIPS, naming the absent
//     toolchain, rather than reddens. The foreign arms keep the C runtime's startup out, so their image starts at
//     the process-ending raw entry of `pe_raw_entry.hpp` -- a third DSS object -- and not at `main`: returning from
//     a raw PE entry ends the thread, not the process (measured there, on this very program).

#include "core/types/diagnostic_reporter.hpp"
#include "program/program.hpp"
#include "pe_raw_entry.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

// The member: `&puts` taken in code (a function of its own, so no optimizer folds it into a comparison), in a
// writable static and in a const static. It never CALLS puts.
constexpr char const* kMember =
    "#include <stdio.h>\n"
    "typedef int (*put_fn)(const char *);\n"
    "put_fn member_static_puts = puts;\n"
    "put_fn const member_const_puts = puts;\n"
    "__attribute__((noinline)) put_fn member_code_puts(void) { return puts; }\n";

// The program, a second DSS object. Exit 42: the member agrees with itself and the value is the image's thunk (a
// foreign link); 43: it agrees with itself and the value is what GetProcAddress answers (DSS's link); otherwise
// 1000 + a bitset: 1 the loader lookup failed, 2 the member's code form differs from its static, 4 from its const
// static, 8 a call through the code form failed.
constexpr char const* kMain =
    "#include <windows.h>\n"
    "typedef int (*put_fn)(const char *);\n"
    "extern put_fn member_static_puts;\n"
    "extern put_fn const member_const_puts;\n"
    "put_fn member_code_puts(void);\n"
    "int main(void) {\n"
    "    HMODULE crt = LoadLibraryA(\"ucrtbase.dll\");\n"
    "    void const *bound = crt ? (void const *)GetProcAddress(crt, \"puts\") : 0;\n"
    "    void const *code = (void const *)member_code_puts();\n"
    "    int bad = 0;\n"
    "    if (bound == 0) bad |= 1;\n"
    "    if (code != (void const *)member_static_puts) bad |= 2;\n"
    "    if (code != (void const *)member_const_puts) bad |= 4;\n"
    "    if (member_code_puts()(\"through the member's code form\") < 0) bad |= 8;\n"
    "    if (bad != 0) return 1000 + bad;\n"
    "    return code == bound ? 43 : 42;\n"
    "}\n";

constexpr char const* kExitMeaning =
    "42 = the member's three forms of &puts are one value, the image's import thunk (a foreign link); 43 = one "
    "value, the address GetProcAddress answers (DSS's own link, design c2); 1000 + a bitset: 1 the loader lookup "
    "failed, 2 the member's code &puts differs from its static, 4 from its const static, 8 a call through it failed";

void writeText(fs::path const& p, std::string_view text) { std::ofstream(p, std::ios::binary) << text; }

// Compile one C source to a pe64 RELOCATABLE object through the production driver, copied to `<dir>/<stem>.obj`.
// Empty path on a compile failure; every caller asserts.
[[nodiscard]] fs::path buildObj(fs::path const& dir, std::string const& stem, std::string_view source,
                                DiagnosticReporter& rep) {
    auto const src = dir / (stem + ".c");
    writeText(src, source);
    auto const out = dir / (stem + ".out");
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    int const rc = p.compileFiles(std::vector<std::string>{src.string()}, "c",
                                  std::vector<std::string>{"x86_64:pe64-x86_64-windows"}, rep);
    if (rc != 0) return {};
    auto const obj = out / (stem + ".obj");
    if (!fs::exists(obj)) return {};
    auto const placed = dir / (stem + ".obj");
    fs::copy_file(obj, placed, fs::copy_options::overwrite_existing);
    return placed;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + d.actual;
    return s;
}

[[nodiscard]] std::vector<std::uint8_t> readFile(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// ── The smallest COFF reader tier 1 needs — a WIRE LAYOUT the PE/COFF specification fixes, duplicated here for
//    the reason `test_pe_object_data_import_slot.cpp` gives for its own copy.
[[nodiscard]] std::uint32_t rdU16(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o] | (b[o + 1] << 8));
}
[[nodiscard]] std::uint32_t rdU32(std::vector<std::uint8_t> const& b, std::size_t o) {
    return rdU16(b, o) | (rdU16(b, o + 2) << 16);
}

[[nodiscard]] std::string symName(std::vector<std::uint8_t> const& b, std::uint32_t idx) {
    std::uint32_t const symPtr = rdU32(b, 8), numSyms = rdU32(b, 12);
    std::size_t const rec = symPtr + static_cast<std::size_t>(idx) * 18u;
    if (b.size() < rec + 18u) return {};
    std::string s;
    if (rdU32(b, rec) == 0u) {
        for (std::size_t i = symPtr + static_cast<std::size_t>(numSyms) * 18u + rdU32(b, rec + 4);
             i < b.size() && b[i] != 0; ++i) {
            s.push_back(static_cast<char>(b[i]));
        }
        return s;
    }
    for (std::size_t k = 0; k < 8 && b[rec + k] != 0; ++k) s.push_back(static_cast<char>(b[rec + k]));
    return s;
}

struct Reloc {
    std::string   section;
    std::uint32_t type = 0;
    std::string   target;
};

[[nodiscard]] std::vector<Reloc> allRelocs(std::vector<std::uint8_t> const& b) {
    std::vector<Reloc> out;
    if (b.size() < 20u) return out;
    std::uint32_t const nSec = rdU16(b, 2);
    for (std::uint32_t i = 0; i < nSec; ++i) {
        std::size_t const h = 20u + static_cast<std::size_t>(i) * 40u;
        if (b.size() < h + 40u) break;
        std::string name;
        for (std::size_t k = 0; k < 8 && b[h + k] != 0; ++k) name.push_back(static_cast<char>(b[h + k]));
        std::uint32_t const relPtr = rdU32(b, h + 24), nRel = rdU16(b, h + 32);
        for (std::uint32_t r = 0; r < nRel; ++r) {
            std::size_t const o = relPtr + static_cast<std::size_t>(r) * 10u;
            if (b.size() < o + 10u) break;
            out.push_back({name, rdU16(b, o + 8), symName(b, rdU32(b, o + 4))});
        }
    }
    return out;
}

[[nodiscard]] bool namesSymbol(std::vector<std::uint8_t> const& b, std::string_view name) {
    std::uint32_t const numSyms = rdU32(b, 12);
    for (std::uint32_t i = 0; i < numSyms; ++i) {
        if (symName(b, i) == name) return true;
    }
    return false;
}

[[nodiscard]] std::string dumpRelocs(std::vector<Reloc> const& rs) {
    std::string s;
    for (auto const& r : rs) s += "\n  " + r.section + " type=" + std::to_string(r.type) + " -> " + r.target;
    return s;
}

constexpr std::uint32_t kAddr64 = 1;   // IMAGE_REL_AMD64_ADDR64

}  // namespace

// ══ TIER 1 — host-independent: what the driver writes ══

TEST(PeObjectFunctionAddress, EveryReferenceToALibraryFunctionsAddressIsTheSamePointerRelocation) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-fn-address"};
    auto const dir = scratch.path();
    DiagnosticReporter rep;
    auto const obj = buildObj(dir, "member", kMember, rep);
    ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
    auto const bytes = readFile(obj);
    auto const relocs = allRelocs(bytes);
    EXPECT_FALSE(namesSymbol(bytes, "__imp_puts"))
        << "the object must not reach `&puts` through the IAT name: every PE linker hands its static `{puts}` the "
           "import THUNK, so a code form read from `__imp_puts` would be a second address of one function"
        << dumpRelocs(relocs);
    std::size_t addr64ToPuts = 0, otherToPuts = 0;
    for (auto const& r : relocs) {
        if (r.target != "puts") continue;
        (r.type == kAddr64 ? addr64ToPuts : otherToPuts) += 1;
    }
    EXPECT_EQ(otherToPuts, 0u)
        << "no relocation but the absolute pointer one may name `puts`: a REL32 against it is a displacement that "
           "can only reach the thunk, and the member never calls it"
        << dumpRelocs(relocs);
    EXPECT_EQ(addr64ToPuts, 3u)
        << "exactly three IMAGE_REL_AMD64_ADDR64 against `puts`: the writable static, the const static, and the "
           "pointer the code loads its `&puts` from — so a linker binds all three the same way"
        << dumpRelocs(relocs);
}

// ══ TIER 2 — the runtime witness, Windows only ══

namespace {

// The DSS-built objects every foreign arm links: the member, the program, and the image's raw entry
// (`pe_raw_entry.hpp`: it calls `main` and ends the PROCESS with what `main` returns).
struct DssObjects {
    fs::path    member;
    fs::path    main;
    fs::path    entry;
    std::string diagnostics;
    [[nodiscard]] bool ok() const { return !member.empty() && !main.empty() && !entry.empty(); }
};

[[nodiscard]] DssObjects buildDssObjects(fs::path const& dir) {
    DssObjects o;
    DiagnosticReporter rep;
    o.member = buildObj(dir, "member", kMember, rep);
    o.main = buildObj(dir, "main", kMain, rep);
    o.entry = buildObj(dir, "entry", test::pe_raw_entry::kSource, rep);
    o.diagnostics = diagnosticsOf(rep);
    return o;
}

// The two halves of a foreign link line that name the image's entry: `/ENTRY:<symbol>` for link.exe and
// lld-link, `-e <symbol>` for GNU ld. One spelling of the symbol, `pe_raw_entry.hpp`'s.
[[nodiscard]] std::string msEntryOption() { return std::string{"/ENTRY:"} + test::pe_raw_entry::kSymbol; }
[[nodiscard]] std::string gnuEntryOption() { return std::string{"-e "} + test::pe_raw_entry::kSymbol; }

// One linker line in `dir`, its output KEPT in `<dir>/<logName>`: a link that fails says why in the assertion
// (`tailOf`), not only that it failed. `tools` set: the line runs under the process's MSVC developer environment
// (link.exe and lld-link need its LIB); unset: under this process's own (MinGW's gcc).
[[nodiscard]] bool linkCapturing(test_support::native_probe::MsvcTools const* tools, fs::path const& dir,
                                 std::string const& cmd, std::string const& logName) {
    namespace np = test_support::native_probe;
    std::string const line = np::captureCmd("cd /d \"" + dir.string() + "\" && " + cmd, dir / logName);
    if (tools != nullptr) return tools->ready() && np::systemUnder(tools->env, line) == 0;
    return std::system(line.c_str()) == 0;
}

[[nodiscard]] std::string linkOutput(fs::path const& dir, std::string const& logName, std::string_view who) {
    return test_support::native_probe::tailOf(dir / logName, 30, who);
}

// lld-link from a Visual Studio instance carrying the LLVM component — ANY instance, not only the one the cl
// toolchain was found in (`-latest` picks one; this host's LLVM component lives in another), at
// `<instance>\VC\Tools\Llvm\x64\bin\lld-link.exe`. Empty, with `why` set, when no instance carries it.
[[nodiscard]] fs::path lldLinkOfAnyInstance(fs::path const& work, std::string& why) {
    fs::path const vswhere{"C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"};
    std::error_code ec;
    if (!fs::is_regular_file(vswhere, ec)) {
        why = "no Visual Studio Installer (vswhere.exe) on this machine";
        return {};
    }
    fs::path const listed = work / "llvm-instances.txt";
    std::string const q = "\"\"" + vswhere.string() + "\" -products * -requires "
                          "Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath > \""
                        + listed.string() + "\"\"";
    if (std::system(q.c_str()) != 0) {
        why = "vswhere.exe is present but the query for the LLVM component failed: `" + q + "`";
        return {};
    }
    std::ifstream in{listed};
    std::string seen;
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        fs::path const lld = fs::path{line} / "VC" / "Tools" / "Llvm" / "x64" / "bin" / "lld-link.exe";
        if (fs::is_regular_file(lld, ec)) return lld;
        seen += " `" + lld.string() + "`";
    }
    why = seen.empty() ? std::string{"no Visual Studio instance carries Microsoft.VisualStudio.Component.VC.Llvm.Clang"}
                       : "vswhere listed instances with the LLVM component, but none holds lld-link.exe:" + seen;
    return {};
}

void expectOneAddress(fs::path const& exe, unsigned want, char const* who) {
    auto const r = test_support::runBinary(exe);
    ASSERT_TRUE(r.spawned) << who << ": " << r.diagnostic;
    EXPECT_FALSE(r.timedOut) << who;
    EXPECT_EQ(r.exitCode, want) << who << " — " << kExitMeaning;
}

}  // namespace

TEST(PeObjectFunctionAddressReferenceLinkNative, LinkExeGivesTheMembersEveryFormOfPutsOneAddress) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-fn-address-link"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    auto const objs = buildDssObjects(dir);
    ASSERT_TRUE(objs.ok()) << objs.diagnostics;
    // `/NODEFAULTLIB` + `/ENTRY:` keep the CRT out: the subject is the two DSS objects and the import libraries,
    // and the image starts at the process-ending raw entry (`entry.obj`).
    ASSERT_TRUE(linkCapturing(&tools, dir, "link /nologo /OUT:fa_link.exe " + msEntryOption()
                              + " /SUBSYSTEM:CONSOLE /NODEFAULTLIB main.obj member.obj entry.obj kernel32.lib ucrt.lib",
                              "link.txt"))
        << "link.exe must link the DSS objects against kernel32.lib and ucrt.lib"
        << linkOutput(dir, "link.txt", "link.exe");
    expectOneAddress(dir / "fa_link.exe", 42u, "link.exe");
}

TEST(PeObjectFunctionAddressReferenceLinkNative, LldLinkGivesTheMembersEveryFormOfPutsOneAddress) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-fn-address-lld"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    std::string why;
    auto const lld = lldLinkOfAnyInstance(dir, why);
    if (lld.empty()) GTEST_SKIP() << "no lld-link on this host: " << why;
    // lld-link reads the import libraries through the developer environment's LIB, as link.exe does.
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    auto const objs = buildDssObjects(dir);
    ASSERT_TRUE(objs.ok()) << objs.diagnostics;
    ASSERT_TRUE(linkCapturing(&tools, dir, "\"" + lld.string() + "\" /nologo /OUT:fa_lld.exe " + msEntryOption()
                              + " /SUBSYSTEM:CONSOLE /NODEFAULTLIB main.obj member.obj entry.obj kernel32.lib ucrt.lib",
                              "lld.txt"))
        << "lld-link must link the DSS objects against kernel32.lib and ucrt.lib"
        << linkOutput(dir, "lld.txt", "lld-link");
    expectOneAddress(dir / "fa_lld.exe", 42u, "lld-link");
}

TEST(PeObjectFunctionAddressReferenceLinkNative, GnuLdGivesTheMembersEveryFormOfPutsOneAddress) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-fn-address-gnu"};
    auto const dir = scratch.path();
#if defined(_WIN32)
    if (std::system("where gcc >nul 2>&1") != 0) {
        GTEST_SKIP() << "no MinGW `gcc` on PATH -- the GNU ld arm is inert on this host";
    }
#else
    // `where` and `nul` are cmd.exe's: on a POSIX shell `>nul` creates a FILE named `nul`, and a MinGW toolchain
    // is not this host's anyway.
    GTEST_SKIP() << "not a Windows host -- the GNU ld arm is inert here";
#endif
    auto const objs = buildDssObjects(dir);
    ASSERT_TRUE(objs.ok()) << objs.diagnostics;
    // `-nostartfiles -e <the raw entry>` keeps the CRT's startup out; MinGW's default libraries supply puts (ucrt)
    // and the kernel32 imports.
    ASSERT_TRUE(linkCapturing(nullptr, dir, "gcc -nostartfiles " + gnuEntryOption()
                              + " -o fa_gnu.exe main.obj member.obj entry.obj -lkernel32", "gnu.txt"))
        << "GNU ld must link the DSS objects" << linkOutput(dir, "gnu.txt", "GNU ld");
    expectOneAddress(dir / "fa_gnu.exe", 42u, "GNU ld");
}

TEST(PeObjectFunctionAddressReferenceLinkNative, ADssLinkOfTheSameMemberGivesTheAddressTheLoaderBinds) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-fn-address-dss"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    DiagnosticReporter memberRep;
    auto const member = buildObj(dir, "member", kMember, memberRep);
    ASSERT_FALSE(member.empty()) << diagnosticsOf(memberRep);
    // The SAME object the foreign arms link, archived by lib.exe, so DSS reads it back as a member.
    ASSERT_TRUE(linkCapturing(&tools, dir, "lib /nologo /OUT:member.lib member.obj", "lib.txt"))
        << "lib.exe must archive the DSS object" << linkOutput(dir, "lib.txt", "lib.exe");
    writeText(dir / "main.c", kMain);
    auto const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    p.setResolveLibraries(std::vector<fs::path>{dir / "member.lib"});
    DiagnosticReporter rep;
    int const rc = p.compileFiles(std::vector<std::string>{(dir / "main.c").string()}, "c",
                                  std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    ASSERT_EQ(rc, 0) << "DSS must link its own pe64 object back as an archive member:" << diagnosticsOf(rep);
    expectOneAddress(out / "main.exe", 43u, "DSS");
}
