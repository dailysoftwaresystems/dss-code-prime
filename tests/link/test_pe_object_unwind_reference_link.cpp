// D-LK-PE-OBJ-PDATA-FIELDS-COUNT-THE-FUNCTION-OFFSET-TWICE — a DSS pe64
// RELOCATABLE object's unwind tables, as the REFERENCE LINKERS read them.
//
// WHAT WENT WRONG, ✔MEASURED 2026-09-30. `pe::encode`'s Obj arm stamped each
// function's offset IN `.text` into its RUNTIME_FUNCTION's Begin/End fields
// while the IMAGE_REL_AMD64_ADDR32NB named the FUNCTION symbol. COFF has no
// addend column — a linker adds the named symbol's value to the field — so
// every function but the first was described at twice its offset: `lld-link`
// 18 and GNU ld 2.42 both placed one object's `mid` at 0x140001032 for
// 0x140001019 and `main` at 0x14000113E for 0x14000109F. The image then RAN
// (nothing unwinds on the happy path) and RtlLookupFunctionEntry found no
// entry of its own for three of four functions: this file's subject exits 11
// through MinGW gcc 13.2's linker at the pre-fix writer.
//
// WHY THE CLOSED ROW'S WITNESS MISSED IT. D-LK-PE-OBJ-ARM-CARRIES-NO-UNWIND-INFO
// read sizes, relocation types and decoded codes — never where an entry
// STARTS — and its byte pin encoded one function at offset 0, the one case
// where an addend from the function and one from `.text` agree.
//
// TWO TIERS, NEITHER REPLACES THE OTHER (the shape of
// `test_pe_object_data_import_slot.cpp` and `test_macho_ld64_local_collision.cpp`):
//   * TIER 1 (`PeObjectUnwindReferenceLink`) — HOST-INDEPENDENT. Compiles the
//     subject through the production driver and resolves every RUNTIME_FUNCTION
//     field as a linker does (value(symbol) + field). Runs on every leg.
//   * TIER 2 (`PeObjectUnwindReferenceLinkNative`) — the RUNTIME witness. Hands
//     the SAME object to link.exe and to MinGW's linker and RUNS the image; the
//     program asks the OS for each of its own functions' entries. Windows-only,
//     and it SKIPS, naming the absent toolchain, rather than reddens. The image
//     starts at the process-ending raw entry of `pe_raw_entry.hpp` (one more
//     DSS object), not at `main`: returning from a raw PE entry ends the
//     thread, not the process.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"
#include "host_native_target.hpp"
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
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

// The subject. Four framed functions — three of them past `.text` offset 0 —
// and `main` asks the OS, for each, whether the RUNTIME_FUNCTION covering the
// function's first byte begins exactly there. 42 iff all four do; 10 + n
// otherwise (the pre-fix writer gives 11: only the function at offset 0).
constexpr char const* kSubject =
    "#include <windows.h>\n"
    "typedef unsigned long long u64;\n"
    "typedef unsigned int u32;\n"
    "static int dss_first(int x) { volatile int a[8]; a[0] = x; a[7] = 1; return a[0] + a[7]; }\n"
    "static int dss_second(int x) { volatile int a[8]; a[0] = x; return dss_first(a[0]) + 2; }\n"
    "static int dss_third(int x) { volatile int a[8]; a[0] = x; return dss_second(a[0]) + 3; }\n"
    "static int entry_is_its_own(void *fn) {\n"
    "    u64 base = 0;\n"
    "    u64 pc = (u64)fn;\n"
    "    void *rf = RtlLookupFunctionEntry(pc, &base, (void *)0);\n"
    "    if (!rf || base == 0) return 0;\n"
    "    u32 begin = ((u32 *)rf)[0];\n"
    "    u32 end = ((u32 *)rf)[1];\n"
    "    return (u64)begin == pc - base && (u64)end > pc - base;\n"
    "}\n"
    "int main(void) {\n"
    "    if (dss_third(1) != 7) return 90;\n"
    "    int ok = entry_is_its_own((void *)&dss_first) + entry_is_its_own((void *)&dss_second)\n"
    "           + entry_is_its_own((void *)&dss_third) + entry_is_its_own((void *)&main);\n"
    "    return ok == 4 ? 42 : 10 + ok;\n"
    "}\n";

[[nodiscard]] fs::path writeSrc(fs::path const& dir, std::string_view name,
                                std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

// Compile the subject to a pe64 RELOCATABLE object through the production
// driver. Empty path on a compile failure; every caller asserts.
[[nodiscard]] fs::path buildObj(fs::path const& dir, DiagnosticReporter& rep) {
    auto const src = writeSrc(dir, "rt.c", kSubject);
    auto const out = dir / "rt.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    int const rc = p.compileFiles(std::vector<std::string>{src.string()}, "c",
                                  std::vector<std::string>{
                                      "x86_64:pe64-x86_64-windows"},
                                  rep);
    if (rc != 0) return {};
    auto const obj = out / "rt.obj";
    return fs::exists(obj) ? obj : fs::path{};
}

[[nodiscard]] std::vector<std::uint8_t> readFile(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The image's raw entry (`pe_raw_entry.hpp`), compiled through the production
// driver and placed beside the subject as `<dir>/entry.obj`. False on a compile
// failure; every caller asserts.
[[nodiscard]] bool buildEntryObj(fs::path const& dir, DiagnosticReporter& rep) {
    auto const src = writeSrc(dir, "entry.c", test::pe_raw_entry::kSource);
    auto const out = dir / "entry.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    int const rc = p.compileFiles(std::vector<std::string>{src.string()}, "c",
                                  std::vector<std::string>{
                                      "x86_64:pe64-x86_64-windows"},
                                  rep);
    if (rc != 0 || !fs::exists(out / "entry.obj")) return false;
    fs::copy_file(out / "entry.obj", dir / "entry.obj",
                  fs::copy_options::overwrite_existing);
    return true;
}

// ── The smallest COFF reader tier 1 needs — a WIRE LAYOUT the PE/COFF
//    specification fixes, duplicated here for the reason
//    `test_pe_object_data_import_slot.cpp` gives for its own copy.
[[nodiscard]] std::uint16_t rdU16(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}
[[nodiscard]] std::uint32_t rdU32(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16)
         | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

struct Sec {
    std::string   name;
    std::uint32_t size = 0, raw = 0, relPtr = 0;
    std::uint16_t nRel = 0;
    std::int16_t  ordinal = 0;
};

[[nodiscard]] std::vector<Sec> sections(std::vector<std::uint8_t> const& b) {
    std::vector<Sec> out;
    std::uint16_t const n = rdU16(b, 2), opt = rdU16(b, 16);
    for (std::uint16_t i = 0; i < n; ++i) {
        std::size_t const h = 20u + opt + static_cast<std::size_t>(i) * 40u;
        Sec s;
        for (std::size_t c = 0; c < 8 && b[h + c] != 0; ++c) s.name.push_back(static_cast<char>(b[h + c]));
        s.size = rdU32(b, h + 16);
        s.raw = rdU32(b, h + 20);
        s.relPtr = rdU32(b, h + 24);
        s.nRel = rdU16(b, h + 32);
        s.ordinal = static_cast<std::int16_t>(i + 1);
        out.push_back(std::move(s));
    }
    return out;
}

struct SymAt {
    std::uint32_t value = 0;
    std::int16_t  section = 0;
};

[[nodiscard]] SymAt symAt(std::vector<std::uint8_t> const& b, std::uint32_t idx) {
    std::size_t const rec = static_cast<std::size_t>(rdU32(b, 8)) + static_cast<std::size_t>(idx) * 18u;
    return {rdU32(b, rec + 8), static_cast<std::int16_t>(rdU16(b, rec + 12))};
}

}  // namespace

TEST(PeObjectUnwindReferenceLink, EveryRuntimeFunctionOfACompiledObjectNamesItsOwnStart) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-unwind-ref-link"};
    DiagnosticReporter rep;
    auto const objPath = buildObj(scratch.path(), rep);
    ASSERT_FALSE(objPath.empty()) << "errs=" << rep.errorCount();
    auto const b = readFile(objPath);
    auto const secs = sections(b);
    Sec const* pdata = nullptr;
    Sec const* text = nullptr;
    for (auto const& s : secs) {
        if (s.name == ".pdata") pdata = &s;
        if (s.name == ".text") text = &s;
    }
    ASSERT_NE(pdata, nullptr) << "a pe64 object with framed functions must carry `.pdata`";
    ASSERT_NE(text, nullptr);
    ASSERT_EQ(pdata->size % 12u, 0u);
    std::uint32_t const entries = pdata->size / 12u;
    ASSERT_GE(entries, 4u) << "the subject has at least four framed functions";

    // Relocation at a `.pdata` offset → the named symbol.
    auto relocAt = [&](std::uint32_t off) -> std::optional<std::uint32_t> {
        for (std::uint16_t i = 0; i < pdata->nRel; ++i) {
            std::size_t const r = pdata->relPtr + static_cast<std::size_t>(i) * 10u;
            if (rdU32(b, r) == off) return rdU32(b, r + 4);
        }
        return std::nullopt;
    };
    std::set<std::uint32_t> starts;
    std::size_t pastZero = 0;
    for (std::uint32_t e = 0; e < entries; ++e) {
        std::uint32_t const at = e * 12u;
        auto const beginSym = relocAt(at);
        auto const endSym = relocAt(at + 4u);
        ASSERT_TRUE(beginSym && endSym) << "entry " << e << " must relocate Begin and End";
        auto const fnBegin = symAt(b, *beginSym);
        auto const fnEnd = symAt(b, *endSym);
        ASSERT_EQ(fnBegin.section, text->ordinal);
        std::uint32_t const begin = fnBegin.value + rdU32(b, pdata->raw + at);
        std::uint32_t const end = fnEnd.value + rdU32(b, pdata->raw + at + 4u);
        EXPECT_EQ(begin, fnBegin.value)
            << "entry " << e << ": a linker resolves Begin to 0x" << std::hex << begin
            << " but the function it names starts at 0x" << fnBegin.value
            << " - the addend must be stated FROM the named function";
        EXPECT_GT(end, begin) << "entry " << e << ": [Begin, End) must cover code";
        EXPECT_LE(end, text->size);
        starts.insert(begin);
        if (begin != 0u) ++pastZero;
    }
    EXPECT_EQ(starts.size(), entries) << "every entry must start at a DIFFERENT function";
    EXPECT_GE(pastZero, 3u) << "the pin needs functions past `.text` offset 0 to mean anything";
}

TEST(PeObjectUnwindReferenceLinkNative, LinkExeImageFindsEveryFunctionsOwnEntry) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-unwind-ref-link"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const env = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(env.ready()) << env.describe();

    DiagnosticReporter rep;
    auto const obj = buildObj(dir, rep);
    ASSERT_FALSE(obj.empty()) << "errs=" << rep.errorCount();
    fs::copy_file(obj, dir / "rt.obj", fs::copy_options::overwrite_existing);
    ASSERT_TRUE(buildEntryObj(dir, rep)) << "errs=" << rep.errorCount();
    ASSERT_TRUE(env.run(std::string{"link /nologo /OUT:rt.exe /ENTRY:"} + test::pe_raw_entry::kSymbol
                        + " /SUBSYSTEM:CONSOLE /NODEFAULTLIB rt.obj entry.obj kernel32.lib"))
        << "link.exe must link the DSS object against kernel32.lib alone";
    auto const r = test_support::runBinary(dir / "rt.exe");
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "10 + n means only n of the four functions found their own RUNTIME_FUNCTION "
           "in the image link.exe built; the pre-fix writer gives 11 (the function at "
           "`.text` offset 0 alone), 90 means the subject itself miscomputed";
}

TEST(PeObjectUnwindReferenceLinkNative, MingwImageFindsEveryFunctionsOwnEntry) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-unwind-ref-link"};
    auto const dir = scratch.path();
    // MinGW's linker, reached through the `gcc` on PATH that reports MinGW-w64's x86-64
    // target (`-nostartfiles -e <the raw entry>` keeps the CRT out: the subject is ONE
    // object, beside the entry's).
    auto const gcc = test_support::native_probe::locateMingwGcc(dir);
    if (gcc.toolAbsent()) GTEST_SKIP() << gcc.detail;
    ASSERT_TRUE(gcc.ok()) << gcc.describe();

    DiagnosticReporter rep;
    auto const obj = buildObj(dir, rep);
    ASSERT_FALSE(obj.empty()) << "errs=" << rep.errorCount();
    fs::copy_file(obj, dir / "rt.obj", fs::copy_options::overwrite_existing);
    ASSERT_TRUE(buildEntryObj(dir, rep)) << "errs=" << rep.errorCount();
    std::string const cmd = "cd /d \"" + dir.string() + "\" && gcc -nostartfiles -e "
                          + test::pe_raw_entry::kSymbol
                          + " -o rt_mingw.exe rt.obj entry.obj -lkernel32 >nul 2>&1";
    ASSERT_EQ(std::system(("\"" + cmd + "\"").c_str()), 0)
        << "MinGW's linker must link the DSS object against kernel32";
    auto const r = test_support::runBinary(dir / "rt_mingw.exe");
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "✔MEASURED at the pre-fix writer through MinGW gcc 13.2: 11 — only the function "
           "at `.text` offset 0 found its own entry; 42 is every function finding its own";
}

// ── PeObjSeh — a `__try` compiled to a pe64 RELOCATABLE object
//    (D-LK-PE-OBJ-ARM-CARRIES-NO-UNWIND-INFO, reopened by P69 fold 3c).
//
// WHAT WENT WRONG, ✔MEASURED by `CoffWeakDefaultRecord`'s first run (fold 3b): the
// relocatable and archive PE documents stated no `sehPersonality`, on an omission comment
// saying `pe::encode`'s Obj arm refused a function carrying a scope table — while the arm
// writes the table as `.xdata` relocations. The absence refused a `__try` before that
// writer was reached (`L_UnsupportedLoweringForOpcode`), where `cl /c` compiles it into an
// object and link.exe links it: one working reference makes it required.
//
// THE TIERS. `PeObjSeh.ATryCompiledToAnObjectNamesItsPersonalityAndItsScopeTable` reads the
// object on every leg: the compile through the CLI's OBJECT door succeeds, the personality
// is an undefined external of the object, and `.xdata` relocates it and the guarded
// function's scope fields image-relatively. The other three link the SAME object and run
// the image where the host is Windows: DSS itself (the object beside a C source, into an
// image, on every leg; RUN on Windows), link.exe beside cl's `main` and MinGW's gcc beside
// its own (each Windows-only, SKIPPING by name when its toolchain is absent). The fault is a
// read of address 0 inside the `__try`: 42 iff the OS dispatched it to the `__except` body
// through the object's scope table; a crash (0xC0000005) means it escaped.
namespace {

constexpr char const* kGuardedSubject =
    "int dss_guarded(int volatile *p) {\n"
    "    int r = 0;\n"
    "    __try { r = *p; } __except (1) { r = -1; }\n"
    "    return r;\n"
    "}\n";

constexpr char const* kGuardedMain =
    "int dss_guarded(int volatile *p);\n"
    "int main(void) {\n"
    "    int volatile v = 20;\n"
    "    if (dss_guarded(&v) != 20) return 90;\n"
    "    return dss_guarded((int volatile *)0) == -1 ? 42 : 11;\n"
    "}\n";

constexpr std::string_view kPersonality = "__C_specific_handler";

constexpr char const* kGuardedExit =
    "42 is the fault dispatched to the object's `__except` body; 90 the unguarded read "
    "miscomputed; 11 the handler ran and returned the wrong value; 0xC0000005 the fault "
    "escaped: the image carries no scope table the OS found for `dss_guarded`";

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

// The guarded subject compiled to a pe64 OBJECT through the production driver's object
// door, copied to `<dir>/seh.obj`; empty on a compile failure (every caller asserts).
[[nodiscard]] fs::path buildGuardedObj(fs::path const& dir, DiagnosticReporter& rep) {
    auto const src = writeSrc(dir, "seh.c", kGuardedSubject);
    auto const out = dir / "seh.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    if (p.compileFiles(std::vector<std::string>{src.string()}, "c",
                       std::vector<std::string>{"x86_64:pe64-x86_64-windows"}, rep) != 0
        || !fs::exists(out / "seh.obj"))
        return {};
    fs::copy_file(out / "seh.obj", dir / "seh.obj", fs::copy_options::overwrite_existing);
    return dir / "seh.obj";
}

// A COFF symbol record's name: inline (8 bytes) or an offset into the string table.
[[nodiscard]] std::string symName(std::vector<std::uint8_t> const& b, std::uint32_t idx) {
    std::size_t const table = rdU32(b, 8);
    std::size_t const rec = table + static_cast<std::size_t>(idx) * 18u;
    std::string n;
    if (rdU32(b, rec) == 0u) {
        std::size_t p = table + static_cast<std::size_t>(rdU32(b, 12)) * 18u + rdU32(b, rec + 4);
        while (p < b.size() && b[p] != 0) n.push_back(static_cast<char>(b[p++]));
        return n;
    }
    for (std::size_t c = 0; c < 8 && b[rec + c] != 0; ++c) n.push_back(static_cast<char>(b[rec + c]));
    return n;
}

struct NamedReloc {
    std::string   symbol;
    std::uint16_t type = 0;
};

[[nodiscard]] std::vector<NamedReloc> relocsOf(std::vector<std::uint8_t> const& b, Sec const& s) {
    std::vector<NamedReloc> out;
    for (std::uint16_t i = 0; i < s.nRel; ++i) {
        std::size_t const r = s.relPtr + static_cast<std::size_t>(i) * 10u;
        out.push_back({symName(b, rdU32(b, r + 4)), rdU16(b, r + 8)});
    }
    return out;
}

}  // namespace

TEST(PeObjSeh, ATryCompiledToAnObjectNamesItsPersonalityAndItsScopeTable) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-obj-seh"};
    DiagnosticReporter rep;
    auto const objPath = buildGuardedObj(scratch.path(), rep);
    ASSERT_FALSE(objPath.empty())
        << "a `__try` compiled for the relocatable PE document must compile (cl /c does):" << diagnosticsOf(rep);
    auto const b = readFile(objPath);

    // The personality: an UNDEFINED external of the object (section 0, class EXTERNAL),
    // which the final link binds.
    std::size_t undefPersonality = 0;
    std::uint32_t const nSyms = rdU32(b, 12);
    for (std::uint32_t i = 0; i < nSyms; i += 1u + b[rdU32(b, 8) + static_cast<std::size_t>(i) * 18u + 17u]) {
        if (symName(b, i) != kPersonality) continue;
        auto const at = symAt(b, i);
        EXPECT_EQ(at.section, 0) << "the personality is no object's to define";
        EXPECT_EQ(b[rdU32(b, 8) + static_cast<std::size_t>(i) * 18u + 16u], 2u) << "IMAGE_SYM_CLASS_EXTERNAL";
        ++undefPersonality;
    }
    EXPECT_EQ(undefPersonality, 1u) << "the object names its personality exactly once";

    Sec const* xdata = nullptr;
    auto const secs = sections(b);
    for (auto const& s : secs) {
        if (s.name == ".xdata") xdata = &s;
    }
    ASSERT_NE(xdata, nullptr) << "a guarded function's UNWIND_INFO lives in `.xdata`";
    std::size_t toPersonality = 0, toGuarded = 0;
    for (auto const& r : relocsOf(b, *xdata)) {
        SCOPED_TRACE(r.symbol);
        EXPECT_EQ(r.type, 3u) << "IMAGE_REL_AMD64_ADDR32NB: every UNWIND_INFO field is image-relative";
        if (r.symbol == kPersonality) ++toPersonality;
        if (r.symbol == "dss_guarded") ++toGuarded;
    }
    EXPECT_EQ(toPersonality, 1u) << "UNWIND_INFO's exception-handler field names the personality";
    EXPECT_GE(toGuarded, 2u) << "the scope record's guarded range (Begin, End) names the function it lies in";
}

TEST(PeObjSeh, DssLinksATryCompiledToAnObjectAndTheImageCatchesTheFault) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-obj-seh"};
    auto const dir = scratch.path();
    DiagnosticReporter rep;
    ASSERT_FALSE(buildGuardedObj(dir, rep).empty()) << diagnosticsOf(rep);
    auto const mainSrc = writeSrc(dir, "main.c", kGuardedMain);
    auto const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    DiagnosticReporter linkRep;
    ASSERT_EQ(p.compileUnits(std::vector<std::string>{(dir / "seh.obj").string(), mainSrc.string()}, "c",
                             std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, linkRep),
              0)
        << "DSS must link its own guarded object into an image:" << diagnosticsOf(linkRep);
    fs::path image;
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file() && e.path().extension() == ".exe") image = e.path();
    }
    ASSERT_FALSE(image.empty()) << "the image DSS linked";
    if (test_support::hostNativeTarget().execTarget != "x86_64:pe64-x86_64-windows-exec")
        GTEST_SKIP() << "linked; the image runs only where the host is Windows x86_64";
    auto const r = test_support::runBinary(image);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u) << kGuardedExit;
}

TEST(PeObjSeh, LinkExeLinksATryCompiledToAnObjectAndTheImageCatchesTheFault) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-obj-seh"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const env = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(env.ready()) << env.describe();

    DiagnosticReporter rep;
    ASSERT_FALSE(buildGuardedObj(dir, rep).empty()) << diagnosticsOf(rep);
    (void)writeSrc(dir, "main.c", kGuardedMain);
    ASSERT_TRUE(env.run("cl /nologo /c main.c")) << "cl must compile the caller";
    ASSERT_TRUE(env.run("link /nologo /OUT:seh_link.exe main.obj seh.obj"))
        << "link.exe must link the DSS object, binding its personality from the CRT cl's object names";
    auto const r = test_support::runBinary(dir / "seh_link.exe");
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u) << kGuardedExit;
}

TEST(PeObjSeh, MingwLinksATryCompiledToAnObjectAndTheImageCatchesTheFault) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-obj-seh"};
    auto const dir = scratch.path();
    auto const gcc = test_support::native_probe::locateMingwGcc(dir);
    if (gcc.toolAbsent()) GTEST_SKIP() << gcc.detail;
    ASSERT_TRUE(gcc.ok()) << gcc.describe();

    DiagnosticReporter rep;
    ASSERT_FALSE(buildGuardedObj(dir, rep).empty()) << diagnosticsOf(rep);
    (void)writeSrc(dir, "main.c", kGuardedMain);
    std::string const cmd = "cd /d \"" + dir.string() + "\" && gcc -o seh_mingw.exe main.c seh.obj >nul 2>&1";
    ASSERT_EQ(std::system(("\"" + cmd + "\"").c_str()), 0)
        << "MinGW's gcc must link the DSS object, binding its personality from its own CRT";
    auto const r = test_support::runBinary(dir / "seh_mingw.exe");
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u) << kGuardedExit;
}
