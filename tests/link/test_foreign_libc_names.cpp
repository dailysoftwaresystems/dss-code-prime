// P69 fold 3e, lane p69/xa -- a FOREIGN object's references to the C library bind to the functions the platform
// EXPORTS, under the names the OBJECT references.
//
//   The class (ledgers 547, 549, 553). A reference compiler's object names each C library call the way ITS OWN
//   headers spell it: glibc's <stdlib.h> gives a C17 object `strtol` and a C2X one `__isoc23_strtol`, its <stdio.h>
//   a C17 object's scanf family `__isoc99_*`, its fortifying headers `__printf_chk` and the other checked entry
//   points, its <ctype.h> the table accessors `__ctype_b_loc` and two more, its strict <signal.h> `__sysv_signal`;
//   the macOS SDK's checked entry points `___*_chk`, its inline <ctype.h> `__DefaultRuneLocale` (a DATUM) and
//   `___maskrune`; MinGW-w64's <stdlib.h> `_exit` for `_Exit`. Each such name was left unbound, or bound to a DSS
//   row's OTHER realization (a C17 object's `strtol` to the C23 body), by both binders. The one home is
//   `realizePlatformExternsByOnBinaryName` (compile_pipeline.cpp): a corpus row realizing to ANOTHER name than the
//   one asked is held back, the reverse index is asked, and a row realizing to the name exactly answers -- each
//   such row named by the platform's export, unversioned (stdlib.json, stdio.json, string.json, ctype.json,
//   signal.json, unistd.json, fcntl.json, inttypes.json).
//
//   The universe was MEASURED 2026-11-05 (lane xa, sb16/probes/libc, revision 3): every function row of the corpus
//   called in a unit of its own, its result and its pointer arguments' arrays escaped so -O2 keeps the call, by gcc
//   13.3 and clang 19 (x86_64 and aarch64; c17 gnu17 c2x gnu2x, and gnu17/gnu2x with a known-size array per pointer),
//   cl 19.51 and clang-cl 19.1.5 (default and /std:clatest) and MinGW-w64 gcc 13.2 (c17 gnu17 c2x gnu2x), and Apple
//   clang 21.0.0 (arm64 and x86_64; c17 gnu17 c2x gnu2x and the SDK's fortify levels 0, default and 2); each name
//   judged against the exports of glibc 2.39, ucrtbase.dll and libSystem.
//
// The fixtures (`tests/link/data/foreign_libc_names.source.c` states how each was built: each twice, byte-identical,
// recording no host path; each one's reference program -- the unit beside a `main` returning its value -- runs to
// 42) are the units of that file, one object per unit and producer. Each is linked by DSS beside a DSS `main` that
// returns what the unit returns.
//
// Every TEST but the `...Native` one runs in-process on every leg: the images are linked and read, never run.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"

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
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

constexpr char const* kPe       = "x86_64:pe64-x86_64-windows-exec";
constexpr char const* kElfX64   = "x86_64:elf64-x86_64-linux-exec";
constexpr char const* kElfArm64 = "arm64:elf64-aarch64-linux-exec";
constexpr char const* kMachO    = "arm64:macho64-arm64-darwin-exec";
constexpr char const* kMain     = "int dss_libc_names(void);\nint main(void) { return dss_libc_names(); }\n";

// One foreign object, the image it is linked into, and the names that image must import: each name the object
// references for the C library, as the object spells it (on-binary, the format's decoration included), and the
// names it must NOT import (the OTHER realization a wrong binding would give the same call).
// A cell DSS cannot link yet states the names its link is refused for (`refusedFor`, each named by a diagnostic of
// `refusedAs`) and the row or fold that owes them (`owedTo`): the cell is pinned REFUSED, so the day DSS binds those
// names the cell reds and moves to the linked expectation -- it is never left out, and never pinned as passing.
struct Cell {
    std::string              fixture;
    char const*              exec;
    std::vector<std::string> imports;
    std::vector<std::string> absent;
    std::vector<std::string> refusedFor = {};
    char const*              owedTo     = nullptr;
    DiagnosticCode           refusedAs  = DiagnosticCode::K_SymbolUndefined;
};

// aarch64 ELF's stack-protector canary is exported by the program interpreter (ld-linux-aarch64.so.1), not by the C
// library, and x86_64 glibc exports none (MEASURED 2026-11-13, readelf --dyn-syms): the corpus states availability per
// object format only, so no row can say "aarch64 ELF, the interpreter" yet (ledger 563: fold 3f, an arch selector on
// a row's availability entry carrying its providing image).
constexpr char const* kGuardOwedTo = "fold 3f (ledger 563): aarch64 ELF's canary is the interpreter's export";
// gcc's x86_64 code names glibc's `stdout` directly (`stdout(%rip)`, R_X86_64_PC32), which a link binds only through a
// copy relocation; DSS binds library data through a loader-filled slot and refuses the direct reference by name
// (MEASURED 2026-11-13, K_ImportReferenceUnbindable). The disclosed row of a library datum named directly owes it.
constexpr char const* kCopyRelocationOwedTo =
    "the library datum a unit's code names directly, bound through a copy relocation (the disclosed P1 row)";
// The compiler-support routines (below): H7's class by ledger 553.
constexpr char const* kSupportOwedTo = "fold H7 (ledger 553): the compiler-support routines a foreign object names";

std::vector<std::string> const kStrtoC17{"strtol", "strtoul", "strtoll", "strtoull",
                                         "strtoimax", "strtoumax", "wcstoimax", "wcstoumax"};
std::vector<std::string> const kStrtoC2x{"__isoc23_strtol", "__isoc23_strtoul", "__isoc23_strtoll",
                                         "__isoc23_strtoull", "__isoc23_strtoimax", "__isoc23_strtoumax",
                                         "__isoc23_wcstoimax", "__isoc23_wcstoumax"};
// clang folds three of the eight into the others' calls
std::vector<std::string> const kStrtoClang{"strtol", "strtoimax", "strtoumax", "wcstoimax", "wcstoumax"};
std::vector<std::string> const kScanfC17{"__isoc99_scanf", "__isoc99_fscanf", "__isoc99_sscanf",
                                         "__isoc99_vscanf", "__isoc99_vfscanf", "__isoc99_vsscanf"};
std::vector<std::string> const kFortifyGlibc{
    "__printf_chk", "__fprintf_chk", "__sprintf_chk", "__snprintf_chk", "__vfprintf_chk", "__vsprintf_chk",
    "__vsnprintf_chk", "__fgets_chk", "__fread_chk", "__getcwd_chk", "__gethostname_chk", "__open_2",
    "__read_chk", "__pread_chk", "__readlink_chk", "__realpath_chk", "__mbstowcs_chk", "__wcstombs_chk",
    "__strcat_chk", "__strncat_chk", "__strcpy_chk", "__strncpy_chk", "__stpcpy_chk", "__memcpy_chk",
    "__memmove_chk", "__memset_chk", "__strlcpy_chk", "__strlcat_chk"};
std::vector<std::string> const kCtypeGlibc{"__ctype_b_loc", "__ctype_tolower_loc", "__ctype_toupper_loc"};

[[nodiscard]] std::vector<Cell> elfCells(char const* arch, char const* exec) {
    auto const f = [&](char const* unit, char const* producer) {
        return std::string{"foreign_libc_names_"} + unit + "_" + arch + "_elf_" + producer + "_O2.o";
    };
    // gcc's stack protector (Ubuntu's gcc enables it) guards each of these four units' frames (MEASURED 2026-11-13, the
    // fixtures' symbol tables): on x86_64 the frame calls glibc's reporter, `__stack_chk_fail`, exported by libc.so.6;
    // on aarch64 it also reads the canary, `__stack_chk_guard`, which only the interpreter exports, so those four
    // aarch64 links are refused naming it, owed to fold 3f.
    auto const guarded = [&](std::vector<std::string> names) {
        names.push_back("__stack_chk_fail");
        return names;
    };
    bool const arm64 = std::string_view{arch} == "arm64";
    std::vector<std::string> const refused = arm64 ? std::vector<std::string>{"__stack_chk_guard"}
                                                   : std::vector<std::string>{};
    char const* const owedTo = arm64 ? kGuardOwedTo : nullptr;
    // The scanf units point a standard stream at their file with POSIX's `dup2`, the fortify unit makes its file with
    // `mkstemp` (MEASURED, the fixtures' undefined symbols): unistd.json's and stdlib.json's rows.
    auto const with = [](std::vector<std::string> names, char const* more) {
        names.push_back(more);
        return names;
    };
    // x86_64 gcc's fortify unit names glibc's `stdout` directly (above): refused for it alone.
    Cell const fortify = arm64 ? Cell{f("fortify", "gcc_gnu17"), exec, with(guarded(kFortifyGlibc), "mkstemp"), {},
                                      refused, owedTo}
                               : Cell{f("fortify", "gcc_gnu17"), exec, with(guarded(kFortifyGlibc), "mkstemp"), {},
                                      {"stdout"}, kCopyRelocationOwedTo, DiagnosticCode::K_ImportReferenceUnbindable};
    return {
        {f("strto", "gcc_c17"), exec, guarded(kStrtoC17), {"__isoc23_strtol"}, refused, owedTo},
        {f("strto", "gcc_gnu2x"), exec, guarded(kStrtoC2x), {"strtol"}, refused, owedTo},
        {f("strto", "clang_c17"), exec, kStrtoClang, {"__isoc23_strtol"}},
        {f("scanf", "gcc_c17"), exec, with(guarded(kScanfC17), "dup2"), {"__isoc23_sscanf"}, refused, owedTo},
        {f("scanf", "clang_c17"), exec, with(kScanfC17, "dup2"), {"__isoc23_sscanf"}},
        fortify,
        {f("ctype", "gcc_c17"), exec, kCtypeGlibc, {}},
        {f("ctype", "clang_c17"), exec, kCtypeGlibc, {}},
        {f("signal", "gcc_c17"), exec, {"__sysv_signal", "raise"}, {"signal"}},
        {f("pexit", "gcc_c17"), exec, {"_exit"}, {}},
    };
}

// The cells of the COMPILER-SUPPORT routines a foreign object names beside its library calls (ledger 553: H7's
// class, MEASURED here as one cell each): aarch64 gcc's and clang's binary128 `long double` routines, which DSS
// binds for its own code from the target's `wideFloatSoftcalls` table, and Apple clang's stack probe.
std::vector<std::string> const kTfRoutines{"__extenddftf2", "__multf3", "__trunctfdf2"};

[[nodiscard]] std::vector<Cell> const& cells() {
    static std::vector<Cell> const all = [] {
        std::vector<Cell> c;
        for (auto& e : elfCells("x86_64", kElfX64)) c.push_back(std::move(e));
        for (auto& e : elfCells("arm64", kElfArm64)) c.push_back(std::move(e));
        // PE: cl's and clang-cl's /MD objects name each call's import slot (`__imp_strtol`), MinGW-w64's the call
        // itself; ucrtbase.dll exports every name. MinGW's gnu2x object is byte-identical to its c17 one (its
        // headers redirect nothing), and so is its POSIX `_exit` unit to its `_Exit` one.
        std::vector<Cell> const pe{
            {"foreign_libc_names_strto_x86_64_pe_cl_MD_O2.obj", kPe, kStrtoC17, {}},
            {"foreign_libc_names_strto_x86_64_pe_cl_MD_clatest_O2.obj", kPe, kStrtoC17, {}},
            {"foreign_libc_names_strto_x86_64_pe_clangcl_MD_O2.obj", kPe, kStrtoClang, {}},
            {"foreign_libc_names_strto_x86_64_pe_gcc_c17_O2.o", kPe, kStrtoC17, {}},
            {"foreign_libc_names_exit_x86_64_pe_cl_MD_O2.obj", kPe, {"_Exit"}, {}},
            {"foreign_libc_names_exit_x86_64_pe_gcc_c17_O2.o", kPe, {"_exit"}, {}},
            {"foreign_libc_names_ctype_x86_64_pe_cl_MD_O2.obj", kPe,
             {"isalnum", "isalpha", "isblank", "iscntrl", "isdigit", "isgraph", "islower", "isprint", "ispunct",
              "isspace", "isupper", "isxdigit", "tolower", "toupper"},
             {}},
            {"foreign_libc_names_ctype_x86_64_pe_gcc_c17_O2.o", kPe,
             {"isalnum", "isalpha", "isblank", "iscntrl", "isgraph", "islower", "isprint", "ispunct", "isspace",
              "isupper", "isxdigit", "tolower", "toupper"},
             {}},
        };
        c.insert(c.end(), pe.begin(), pe.end());
        // Mach-O arm64: Apple clang's objects; every name `_`-decorated. Its gnu2x strto object is byte-identical to
        // the c17 one (libSystem's <stdlib.h> redirects nothing). The SDK's default fortify level checks the writes
        // and sprintf; with _FORTIFY_SOURCE=0 the same unit names every function plainly, and the printf family's
        // C rows realize to DSS's C23 bodies, so those imports are the platform's own functions.
        std::vector<Cell> const macho{
            {"foreign_libc_names_strto_arm64_macho_clang_c17_O2.o", kMachO,
             {"_strtol", "_strtoimax", "_strtoumax", "_wcstoimax", "_wcstoumax"}, {}},
            {"foreign_libc_names_scanf_arm64_macho_clang_c17_O2.o", kMachO,
             {"_scanf", "_fscanf", "_sscanf", "_vscanf", "_vfscanf", "_vsscanf", "_dup2"}, {}},
            {"foreign_libc_names_fortify_arm64_macho_clang_gnu17_O2.o", kMachO,
             {"___memccpy_chk", "___memcpy_chk", "___memmove_chk", "___memset_chk", "___sprintf_chk",
              "___strcat_chk", "___strcpy_chk", "___strlcat_chk", "___strlcpy_chk", "___strncat_chk",
              "___strncpy_chk", "_printf", "_fprintf", "_snprintf", "_vfprintf", "_vsnprintf", "_vsprintf",
              "___stdoutp", "___stack_chk_fail", "___stack_chk_guard", "_mkstemp"},
             {}},
            {"foreign_libc_names_fortify0_arm64_macho_clang_gnu17_O2.o", kMachO,
             {"_printf", "_fprintf", "_sprintf", "_snprintf", "_vfprintf", "_vsprintf", "_vsnprintf", "_memccpy",
              "_memcpy", "_memmove", "_memset", "_strcat", "_strcpy", "_strlcat", "_strlcpy", "_strncat", "_strncpy",
              "___stack_chk_fail", "___stack_chk_guard", "_mkstemp"},
             {}},
            {"foreign_libc_names_ctype_arm64_macho_clang_c17_O2.o", kMachO,
             {"__DefaultRuneLocale", "___maskrune", "___tolower", "___toupper"}, {}},
            {"foreign_libc_names_signal_arm64_macho_clang_c17_O2.o", kMachO, {"_signal", "_raise"}, {}},
            {"foreign_libc_names_exit_arm64_macho_clang_c17_O2.o", kMachO, {"__Exit"}, {}},
            {"foreign_libc_names_pexit_arm64_macho_clang_c17_O2.o", kMachO, {"__exit"}, {}},
        };
        c.insert(c.end(), macho.begin(), macho.end());
        // The compiler-support cells (above), each refused naming exactly the routines it calls, owed to H7: DSS
        // binds no compiler-support routine for a foreign object yet (MEASURED 2026-11-13: K_SymbolUndefined for each).
        std::vector<Cell> const support{
            {"foreign_libc_names_ldouble_arm64_elf_gcc_c17_O2.o", kElfArm64, kTfRoutines, {}, kTfRoutines,
             kSupportOwedTo},
            {"foreign_libc_names_ldouble_arm64_elf_clang_c17_O2.o", kElfArm64, kTfRoutines, {}, kTfRoutines,
             kSupportOwedTo},
            {"foreign_libc_names_stackprobe_arm64_macho_clang_c17_O2.o", kMachO, {"___chkstk_darwin"}, {},
             {"___chkstk_darwin"}, kSupportOwedTo},
        };
        c.insert(c.end(), support.begin(), support.end());
        return c;
    }();
    return all;
}

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

// The names a refused link's diagnostics of `code` refuse (K_SymbolUndefined: each undefined name; and the first name
// each quotes for another code, `'stdout' is a DATUM ...`), sorted.
[[nodiscard]] std::vector<std::string> namesRefusedAs(DiagnosticReporter const& rep, DiagnosticCode code) {
    std::vector<std::string> out;
    for (auto const& d : rep.all()) {
        if (d.code != code) continue;
        auto const open = d.actual.find('\'');
        auto const close = open == std::string::npos ? open : d.actual.find('\'', open + 1);
        out.push_back(close == std::string::npos ? d.actual : d.actual.substr(open + 1, close - open - 1));
    }
    std::sort(out.begin(), out.end());
    return out;
}

[[nodiscard]] std::string joined(std::vector<std::string> const& names) {
    std::string s;
    for (auto const& n : names) s += (s.empty() ? "`" : ", `") + n + "`";
    return s;
}

// The names an ELF64 little-endian image's `.dynsym` states UNDEFINED (section index 0): the references its loader
// binds. Read by the test's own walk of the section headers, not by the readers under test.
[[nodiscard]] std::vector<std::string> elfUndefinedDynamicSymbols(std::vector<std::uint8_t> const& b) {
    using namespace test_support::image_deps_detail;
    std::vector<std::string> out;
    if (b.size() < 64u || b[0] != 0x7F || b[1] != 'E' || b[4] != 2u || b[5] != 1u) return out;
    std::uint64_t const shoff     = rdU64(b, 0x28);
    std::uint16_t const shentsize = rdU16(b, 0x3A);
    std::uint16_t const shnum     = rdU16(b, 0x3C);
    if (shoff == 0u || shentsize < 64u || shoff + std::uint64_t{shnum} * shentsize > b.size()) return out;
    auto const sh = [&](std::uint32_t i) { return static_cast<std::size_t>(shoff + std::uint64_t{i} * shentsize); };
    for (std::uint32_t s = 0; s < shnum; ++s) {
        if (rdU32(b, sh(s) + 4) != 11u /*SHT_DYNSYM*/) continue;
        std::uint32_t const link = rdU32(b, sh(s) + 0x28);
        if (link >= shnum) continue;
        std::size_t const symOff = rdU64(b, sh(s) + 0x18), symSize = rdU64(b, sh(s) + 0x20);
        std::size_t const strOff = rdU64(b, sh(link) + 0x18);
        for (std::size_t e = symOff + 24u; e + 24u <= symOff + symSize && e + 24u <= b.size(); e += 24u) {
            if (rdU16(b, e + 6) != 0u /*SHN_UNDEF*/ || rdU32(b, e) == 0u) continue;
            out.push_back(rdCStr(b, strOff + rdU32(b, e)));
        }
    }
    return out;
}

// The names a 64-bit Mach-O image's LC_SYMTAB states undefined and external (N_UNDF | N_EXT): what dyld binds.
[[nodiscard]] std::vector<std::string> machoUndefinedSymbols(std::vector<std::uint8_t> const& b) {
    using namespace test_support::image_deps_detail;
    std::vector<std::string> out;
    if (b.size() < 32u || rdU32(b, 0) != 0xFEEDFACFu) return out;
    std::uint32_t const ncmds = rdU32(b, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds && off + 8u <= b.size(); ++i) {
        std::uint32_t const cmd = rdU32(b, off), cmdsize = rdU32(b, off + 4);
        if (cmdsize == 0u) break;
        if (cmd == 0x02u /*LC_SYMTAB*/ && off + 24u <= b.size()) {
            std::size_t const symoff = rdU32(b, off + 8), nsyms = rdU32(b, off + 12), stroff = rdU32(b, off + 16);
            for (std::size_t k = 0; k < nsyms && symoff + k * 16u + 16u <= b.size(); ++k) {
                std::uint8_t const nType = b[symoff + k * 16u + 4u];
                if ((nType & 0x0Eu) == 0x00u && (nType & 0x01u) != 0u)
                    out.push_back(rdCStr(b, stroff + rdU32(b, symoff + k * 16u)));
            }
        }
        off += cmdsize;
    }
    return out;
}

// What the image imports, by its format: PE's import table, ELF's undefined `.dynsym`, Mach-O's undefined externals.
[[nodiscard]] std::vector<std::string> importsOf(std::vector<std::uint8_t> const& b) {
    if (b.size() >= 2u && b[0] == 'M' && b[1] == 'Z') return test_support::peImportedSymbols(b);
    if (b.size() >= 4u && b[0] == 0x7F && b[1] == 'E') return elfUndefinedDynamicSymbols(b);
    return machoUndefinedSymbols(b);
}

// DSS's image of a DSS `main` beside the cell's object: its path, or empty with `rep` holding why.
[[nodiscard]] fs::path dssImageOf(fs::path const& dir, Cell const& cell, DiagnosticReporter& rep) {
    fs::create_directories(dir);
    auto const mainSrc = dir / "main.c";
    std::ofstream(mainSrc, std::ios::binary) << kMain;
    auto const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    if (p.compileFiles(std::vector<std::string>{mainSrc.string(), fixturePath(cell.fixture).string()}, "c",
                       std::vector<std::string>{cell.exec}, rep)
        != 0) {
        return {};
    }
    fs::path image;
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file() && (image.empty() || e.path().extension() == ".exe")) image = e.path();
    }
    return image;
}

[[nodiscard]] std::string labelOf(Cell const& cell) { return std::string{cell.fixture} + " -> " + cell.exec; }

}  // namespace

// Every leg: each foreign object, linked by DSS beside a DSS `main`, is linked, and its image imports every name the
// object references for the C library EXACTLY ONCE, under the object's own spelling, and none of the names a wrong
// binding of the same calls would give. A cell owed to a later fold is refused, naming exactly the one name it owes.
// Every cell is judged, whatever an earlier one did: a failing cell reports and the next one runs.
// RED-ON-DISABLE: hold no forward row back in `realizePlatformExternsByOnBinaryName` -> the C17 strto objects'
// `strtol` is answered by the C23 row, and the image imports `__isoc23_strtol` (ELF) or defines DSS's body (PE,
// Mach-O) in its place; drop a row (`__ctype_b_loc`) -> the link refuses that name undefined.
TEST(ForeignLibcNames, EachForeignObjectImportsTheLibraryNamesItReferences) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "foreign-libc-names"};
    std::size_t k = 0;
    for (auto const& cell : cells()) {
        SCOPED_TRACE(labelOf(cell));
        DiagnosticReporter rep;
        auto const image = dssImageOf(scratch.path() / std::to_string(k++), cell, rep);
        if (!cell.refusedFor.empty()) {
            auto expected = cell.refusedFor;
            std::sort(expected.begin(), expected.end());
            EXPECT_TRUE(image.empty()) << "owed to " << cell.owedTo << ": DSS links this object now, so the cell "
                                       << "moves to the linked expectation (it imports " << joined(expected) << ")";
            EXPECT_EQ(namesRefusedAs(rep, cell.refusedAs), expected)
                << "owed to " << cell.owedTo << ": the link is refused for " << joined(expected)
                << " and nothing else:" << diagnosticsOf(rep);
            if (cell.refusedAs != DiagnosticCode::K_SymbolUndefined) {
                EXPECT_TRUE(namesRefusedAs(rep, DiagnosticCode::K_SymbolUndefined).empty())
                    << "owed to " << cell.owedTo << ": no name is left undefined beside it:" << diagnosticsOf(rep);
            }
            continue;
        }
        if (image.empty()) {
            ADD_FAILURE() << "the reference links this object beside a main:" << diagnosticsOf(rep);
            continue;
        }
        auto const imports = importsOf(bytesOf(image));
        for (auto const& name : cell.imports) {
            EXPECT_EQ(test_support::dependencyOccurrences(imports, name), 1u)
                << "the image imports `" << name << "` once; it imports: " << test_support::joinDependencies(imports);
        }
        for (auto const& name : cell.absent) {
            EXPECT_EQ(test_support::dependencyOccurrences(imports, name), 0u)
                << "the image imports no `" << name << "`; it imports: " << test_support::joinDependencies(imports);
        }
    }
}

// Where the host runs a cell's executables (Windows x86_64: PE; Linux x86_64 and arm64: ELF; macOS arm64: Mach-O):
// each image runs to 42, as the reference's own program of the same unit does -- every call answered as the
// platform's own function answers it (a C17 `strtol` reads "0b1" as 0, a C23 one as 1).
TEST(ForeignLibcNamesNative, EachImageRunsTo42) {
    auto const host = std::string{test_support::hostNativeTarget().execTarget};
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "foreign-libc-names-run"};
    std::size_t k = 0, ran = 0;
    for (auto const& cell : cells()) {
        // A cell owed to a later fold links nothing to run; the in-process TEST pins its refusal on every leg.
        if (host != cell.exec || !cell.refusedFor.empty()) continue;
        SCOPED_TRACE(labelOf(cell));
        DiagnosticReporter rep;
        auto const image = dssImageOf(scratch.path() / std::to_string(k++), cell, rep);
        if (image.empty()) {
            ADD_FAILURE() << "the reference links this object beside a main:" << diagnosticsOf(rep);
            continue;
        }
        auto const r = test_support::runBinary(image, test_support::kRunBudget, /*captureStdout=*/true);
        if (!r.spawned) {
            ADD_FAILURE() << r.diagnostic;
            continue;
        }
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "the unit's own check that failed, by its exit code (the source's table); "
                                      "stdout: "
                                   << r.capturedStdout;
        ++ran;
    }
    if (ran == 0u) GTEST_SKIP() << "no cell's executable runs on this host (" << host << ")";
}
