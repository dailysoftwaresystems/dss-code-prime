// <stdlib.h>'s C23 7.24 surface, pinned PER PAIR
// (P68 round 12, D-C-STDLIB-H-LACKS-THIRTY-FIVE-ISO-NAMES, fold S1).
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// DSS's <stdlib.h> refused 35 of the ISO C23 7.24 names on EVERY pair — among
// them EXIT_SUCCESS, EXIT_FAILURE, RAND_MAX and MB_CUR_MAX (✔MEASURED
// 2026-09-24: `return rand() <= RAND_MAX ? EXIT_SUCCESS + 42 : EXIT_FAILURE;` was
// S_UndeclaredIdentifier ×3 on pe64, ELF x86_64 and Mach-O arm64). Fold S1 adds
// the everyday half. Each of its facts is PER PAIR — a value that is right on one
// C library is wrong on another — so this file pins every one of them on every
// executable pair, through two instruments that each see what the other cannot:
//
//   (1) THE DESCRIPTOR, read for the pair through the real reader with the
//       pair's own facts (the target schema's `wchar_t` core and `char`
//       signedness): every S1 name is declared for the pair, with its value, its
//       accessor, its version. This is the per-pair TRUTH TABLE, in one place,
//       beside the measurement each row came from.
//   (2) THE COMPILER, in process: one probe naming every S1 name compiles to an
//       image on all five pairs, and that image IMPORTS the entry point the table
//       names (a descriptor row the injection path ignored would pass (1) and fail
//       here; a pipeline that bound the wrong name would fail here on EVERY host,
//       not only on the leg that can run the image).
//
// THE PER-PAIR FACTS, ✔MEASURED 2026-09-24:
//   * RAND_MAX is the range of the platform's OWN rand(): 2147483647 in glibc 2.39
//     (<stdlib.h>) and on Apple (a run on both arches), 0x7fff in the UCRT
//     (10.0.26100.0 <stdlib.h>).
//   * MB_CUR_MAX is each library's own accessor as its DEFAULT compile spells it:
//     glibc `(__ctype_get_mb_cur_max ())` (gcc 13.3.0, clang 18.1.3 and
//     aarch64-linux-gnu-gcc under default, -std=c2x, -std=c11 and
//     -D_POSIX_C_SOURCE=200809L), the UCRT `___mb_cur_max_func()` (its datum only
//     under _CRT_DISABLE_PERFCRIT_LOCKS without _DLL), Apple the `__mb_cur_max`
//     DATUM (Apple clang 21 binds it in every mode on both arches; the per-thread
//     `___mb_cur_max()` arm needs <xlocale.h>) — and
//     its TYPE is size_t on every pair: the UCRT and Apple spell int, glibc size_t,
//     and C23 7.24p3 decides the fork for size_t.
//   * quick_exit binds GLIBC_2.24 on both ELF arches: glibc exports a compat
//     instance beside it (GLIBC_2.10 on x86_64, GLIBC_2.17 on aarch64 — the
//     aarch64 BASE version, which an unversioned reference would bind).
//   * at_quick_exit is glibc's libc_nonshared.a stub, so on ELF it maps onto
//     `__cxa_at_quick_exit` (the null DSO handle the atexit mapping passes too);
//     the UCRT's is `_crt_at_quick_exit` (as atexit's is `_crt_atexit`); Apple
//     exports it.
//   * <stddef.h>'s `offsetof` (D-FFI-OFFSETOF-MACRO, corrected in the same fold):
//     the P31 closure shipped the `__builtin_offsetof` intrinsic but never the
//     macro in DSS's OWN <stddef.h>, so `offsetof` was refused on all five pairs;
//     (1) pins the macro row, (2)'s probe uses it.
//
// RED-ON-DISABLE: remove any S1 row from stdlib.json and (2) fails on every pair
// that lost it; bind quick_exit unversioned and (1) fails on both ELF pairs;
// remove the `offsetof` row from stddef.json and both fail on every pair.

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "image_dependency_table.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"
#include "repo_root.hpp"
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
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::Location;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

// One executable pair and what S1 must look like on it.
struct Pair {
    char const*       spec;          // the --target spelling
    char const*       arch;          // the target schema's name
    char const*       formatDoc;     // the object-format document
    char const*       artifact;      // what the build writes
    std::int64_t      randMax;
    char const*       mbCurMax;      // the macro's replacement text
    char const*       mbAccessor;    // the ONE accessor symbol the pair declares
    char const*       quickExitVersion;
    // The decorated names the linked probe must import (the pair's own spelling).
    std::vector<std::string> imports;
};

[[nodiscard]] std::vector<Pair> const& pairs() {
    static std::vector<Pair> const all{
        {"x86_64:pe64-x86_64-windows-exec", "x86_64", "pe64-x86_64-windows-exec", "probe.exe",
         32767, "((size_t)___mb_cur_max_func())", "___mb_cur_max_func", "",
         {"___mb_cur_max_func", "div", "ldiv", "lldiv", "atoll", "llabs", "strtoull", "strtof",
          "_Exit", "quick_exit", "_crt_at_quick_exit", "mblen"}},
        {"x86_64:elf64-x86_64-linux-exec", "x86_64", "elf64-x86_64-linux-exec", "probe",
         2147483647, "(__ctype_get_mb_cur_max ())", "__ctype_get_mb_cur_max", "GLIBC_2.24",
         {"__ctype_get_mb_cur_max", "div", "ldiv", "lldiv", "atoll", "llabs", "strtoull",
          "strtof", "_Exit", "quick_exit", "__cxa_at_quick_exit", "mblen", "aligned_alloc"}},
        {"arm64:elf64-aarch64-linux-exec", "arm64", "elf64-aarch64-linux-exec", "probe",
         2147483647, "(__ctype_get_mb_cur_max ())", "__ctype_get_mb_cur_max", "GLIBC_2.24",
         {"__ctype_get_mb_cur_max", "div", "ldiv", "lldiv", "atoll", "llabs", "strtoull",
          "strtof", "_Exit", "quick_exit", "__cxa_at_quick_exit", "mblen", "aligned_alloc"}},
        {"arm64:macho64-arm64-darwin-exec", "arm64", "macho64-arm64-darwin-exec", "probe",
         2147483647, "((size_t)__mb_cur_max)", "__mb_cur_max", "",
         {"___mb_cur_max", "_div", "_ldiv", "_lldiv", "_atoll", "_llabs", "_strtoull",
          "_strtof", "__Exit", "_quick_exit", "_at_quick_exit", "_mblen", "_aligned_alloc"}},
        {"x86_64:macho64-x86_64-darwin-exec", "x86_64", "macho64-x86_64-darwin-exec", "probe",
         2147483647, "((size_t)__mb_cur_max)", "__mb_cur_max", "",
         {"___mb_cur_max", "_div", "_ldiv", "_lldiv", "_atoll", "_llabs", "_strtoull",
          "_strtof", "__Exit", "_quick_exit", "_at_quick_exit", "_mblen", "_aligned_alloc"}},
    };
    return all;
}

// Every S1 function, by the C identifier a program calls.
constexpr char const* kS1Functions[] = {
    "div", "ldiv", "lldiv", "atoll", "llabs", "strtoull", "strtof", "_Exit", "quick_exit",
    "mblen"};

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema =
        []() -> std::shared_ptr<GrammarSchema const> {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded.has_value()) {
            ADD_FAILURE() << "the shipped c language does not load";
            return std::shared_ptr<GrammarSchema const>{};
        }
        return *loaded;
    }();
    return schema;
}

[[nodiscard]] fs::path stdlibDescriptor() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg / "shippedLibs" / "stdlib.json";
}

[[nodiscard]] bool availableOn(ShippedSymbol const& s, std::string_view fmt) {
    if (s.availableObjectFormats.empty()) return true;
    return std::find(s.availableObjectFormats.begin(), s.availableObjectFormats.end(), fmt)
        != s.availableObjectFormats.end();
}

// ── ELF / Mach-O undefined-symbol readers (the image's import side) ─────────────
[[nodiscard]] std::vector<std::string>
elfUndefinedDynamicSymbols(std::vector<std::uint8_t> const& b) {
    using namespace dss::test_support::image_deps_detail;
    std::vector<std::string> out;
    if (b.size() < 0x40 || !(b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F'))
        return out;
    std::uint64_t const shoff     = rdU64(b, 0x28);
    std::uint16_t const shentsize = rdU16(b, 0x3A);
    std::uint16_t const shnum     = rdU16(b, 0x3C);
    std::uint16_t const shstrndx  = rdU16(b, 0x3E);
    if (shoff == 0 || shentsize < 64 || shnum == 0 || shstrndx >= shnum) return out;
    auto secOff = [&](std::uint16_t i) {
        return static_cast<std::size_t>(shoff) + static_cast<std::size_t>(i) * shentsize;
    };
    if (secOff(shnum) > b.size()) return out;
    std::size_t const shstrBase = static_cast<std::size_t>(rdU64(b, secOff(shstrndx) + 0x18));
    std::size_t symOff = 0, symSize = 0, strOff = 0;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::size_t const o  = secOff(i);
        std::string const nm = rdCStr(b, shstrBase + rdU32(b, o + 0));
        if (nm == ".dynsym") {
            symOff  = static_cast<std::size_t>(rdU64(b, o + 0x18));
            symSize = static_cast<std::size_t>(rdU64(b, o + 0x20));
        } else if (nm == ".dynstr") {
            strOff = static_cast<std::size_t>(rdU64(b, o + 0x18));
        }
    }
    if (symOff == 0 || strOff == 0 || symOff + symSize > b.size()) return out;
    for (std::size_t o = symOff; o + 24 <= symOff + symSize; o += 24) {
        std::uint32_t const stName  = rdU32(b, o + 0);
        std::uint16_t const stShndx = rdU16(b, o + 6);
        if (stShndx == 0 && stName != 0) out.push_back(rdCStr(b, strOff + stName));
    }
    return out;
}

[[nodiscard]] std::vector<std::string>
machoUndefinedSymbols(std::vector<std::uint8_t> const& b) {
    using namespace dss::test_support::image_deps_detail;
    std::vector<std::string> out;
    if (b.size() < 32 || rdU32(b, 0) != 0xFEEDFACFu) return out;
    std::uint32_t const ncmds = rdU32(b, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds && off + 8 <= b.size(); ++i) {
        std::uint32_t const cmd     = rdU32(b, off);
        std::uint32_t const cmdsize = rdU32(b, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x02u && off + 24 <= b.size()) {   // LC_SYMTAB
            std::size_t const symoff = rdU32(b, off + 8);
            std::size_t const nsyms  = rdU32(b, off + 12);
            std::size_t const stroff = rdU32(b, off + 16);
            for (std::size_t k = 0; k < nsyms; ++k) {
                std::size_t const o = symoff + k * 16;
                if (o + 16 > b.size()) break;
                std::uint8_t const nType = b[o + 4];
                if ((nType & 0x0Eu) == 0x00u && (nType & 0x01u) != 0u)   // N_UNDF | N_EXT
                    out.push_back(rdCStr(b, stroff + rdU32(b, o + 0)));
            }
        }
        off += cmdsize;
    }
    return out;
}

[[nodiscard]] std::vector<std::string> importedSymbolsOf(std::vector<std::uint8_t> const& b) {
    if (b.size() >= 4 && b[0] == 0x7F && b[1] == 'E') return elfUndefinedDynamicSymbols(b);
    if (b.size() >= 2 && b[0] == 'M' && b[1] == 'Z') return dss::test_support::peImportedSymbols(b);
    return machoUndefinedSymbols(b);
}

// The probe: every S1 name, functions BY ADDRESS (a name the injection dropped is
// then undeclared, not merely unused) and macros BY VALUE, each also CALLED so the
// image imports it.
constexpr char const* kProbe =
    "#include <stddef.h>\n"
    "#include <stdlib.h>\n"
    "static void h(void) { _Exit(42); }\n"
    "typedef void (*fp)(void);\n"
    "fp const table[] = { (fp)&div, (fp)&ldiv, (fp)&lldiv, (fp)&atoll, (fp)&llabs,\n"
    "    (fp)&strtoull, (fp)&strtof, (fp)&_Exit, (fp)&quick_exit, (fp)&mblen };\n"
    "#if !defined(EXIT_SUCCESS) || !defined(EXIT_FAILURE) || !defined(RAND_MAX) || !defined(MB_CUR_MAX)\n"
    "#error missing\n"
    "#endif\n"
    "int main(void) {\n"
    "    div_t d = div(7, 2); ldiv_t l = ldiv(7L, 2L); lldiv_t ll = lldiv(7LL, 2LL);\n"
    "    size_t const mb = MB_CUR_MAX;\n"
    "    long long v = atoll(\"1\") + llabs(-1LL) + (long long)strtoull(\"1\", 0, 10)\n"
    "        + (long long)strtof(\"1\", 0) + mblen(\"a\", 1)\n"
    "        + d.quot + l.quot + ll.quot + (long long)mb + RAND_MAX + EXIT_SUCCESS + EXIT_FAILURE\n"
    "        + (long long)offsetof(lldiv_t, rem);\n"
    "#if !defined(_WIN32)\n"
    "    void *p = aligned_alloc(16, 16); free(p);\n"
    "#endif\n"
    "    if (at_quick_exit(h) != 0 || table[0] == 0) return (int)v;\n"
    "    quick_exit((int)v);\n"
    "}\n";

}  // namespace

// ── (1) THE PER-PAIR TRUTH TABLE, THROUGH THE REAL READER ───────────────────────
TEST(StdlibHIsoSurface, EveryS1FactHoldsOnEveryExecutablePair) {
    ASSERT_NE(cLanguage(), nullptr);
    fs::path const path = stdlibDescriptor();
    ASSERT_FALSE(path.empty());
    for (Pair const& p : pairs()) {
        SCOPED_TRACE(p.spec);
        auto targetR = TargetSchema::loadShipped(p.arch);
        ASSERT_TRUE(targetR.has_value()) << p.arch;
        auto formatR = ObjectFormatSchema::loadShipped(p.formatDoc);
        ASSERT_TRUE(formatR.has_value()) << p.formatDoc;
        TargetSchema const&       target = **targetR;
        ObjectFormatSchema const& format = **formatR;
        ObjectFormatKind const    kind   = format.kind();
        std::string const         fmt{objectFormatKindName(kind)};
        DataModel const           model  = format.dataModel();

        ShippedPairFacts facts{cLanguage().get(), model, target.charIsUnsigned(kind), {}};
        for (std::string_view const n : target.abiTypedefNames())
            if (auto const core = target.abiTypedefCore(n, kind))
                facts.abiTypedefs.emplace_back(std::string{n}, *core);

        TypeInterner       interner{CompilationUnitId{1}};
        TypeRegistry       typeReg;
        DiagnosticReporter rep;
        auto const desc = readShippedLibDescriptor(path, interner, typeReg, rep, model,
                                                   std::string_view{p.arch}, kind, {},
                                                   nullptr, &facts);
        ASSERT_TRUE(desc.has_value());
        ASSERT_FALSE(rep.hasErrors()) << (rep.all().empty() ? std::string{} : rep.all().front().actual);

        auto constant = [&](std::string_view n) -> ShippedConstant const* {
            for (auto const& c : desc->constants) if (c.name == n) return &c;
            return nullptr;
        };
        auto macro = [&](std::string_view n) -> ShippedMacro const* {
            for (auto const& m : desc->macros) if (m.name == n) return &m;
            return nullptr;
        };
        auto symbol = [&](std::string_view n) -> ShippedSymbol const* {
            for (auto const& s : desc->symbols)
                if (s.name == n && availableOn(s, fmt)) return &s;
            return nullptr;
        };

        // The constants, by value and as `int`.
        for (auto const& [name, want] : {std::pair<char const*, std::int64_t>{"EXIT_SUCCESS", 0},
                                         {"EXIT_FAILURE", 1}, {"RAND_MAX", p.randMax}}) {
            ShippedConstant const* c = constant(name);
            ASSERT_NE(c, nullptr) << name << " is not declared";
            EXPECT_EQ(c->value, want) << name;
            EXPECT_EQ(interner.kind(c->type), TypeKind::I32) << name << " must be an int";
            EXPECT_TRUE(c->preprocessorVisible) << name << " must be visible to #if";
        }

        // MB_CUR_MAX: this pair's accessor, and no other pair's.
        ShippedMacro const* mb = macro("MB_CUR_MAX");
        ASSERT_NE(mb, nullptr) << "MB_CUR_MAX is not declared";
        EXPECT_EQ(mb->replacement, p.mbCurMax);
        for (char const* acc : {"__ctype_get_mb_cur_max", "___mb_cur_max_func", "__mb_cur_max"}) {
            bool const want = std::string_view{acc} == p.mbAccessor;
            EXPECT_EQ(symbol(acc) != nullptr, want)
                << acc << (want ? " must be declared here" : " must not be declared here");
        }

        // Every S1 function is declared for the pair; quick_exit binds its version.
        for (char const* fn : kS1Functions)
            EXPECT_NE(symbol(fn), nullptr) << fn << " is not declared for " << fmt;
        ShippedSymbol const* qe = symbol("quick_exit");
        ASSERT_NE(qe, nullptr);
        EXPECT_EQ(qe->version, p.quickExitVersion)
            << "quick_exit must bind glibc's DEFAULT version, never the compat instance";
        EXPECT_TRUE(qe->noreturn);

        // at_quick_exit: a macro onto the library's own registrar on ELF and pe, a
        // function on Mach-O.
        ShippedMacro const* aqe = macro("at_quick_exit");
        if (kind == ObjectFormatKind::MachO) {
            EXPECT_EQ(aqe, nullptr);
            EXPECT_NE(symbol("at_quick_exit"), nullptr);
        } else {
            ASSERT_NE(aqe, nullptr);
            EXPECT_EQ(aqe->replacement,
                      kind == ObjectFormatKind::Elf
                          ? std::string{"__cxa_at_quick_exit((void (*)(void *))(f), 0)"}
                          : std::string{"_crt_at_quick_exit"});
            EXPECT_NE(symbol(kind == ObjectFormatKind::Elf ? "__cxa_at_quick_exit"
                                                           : "_crt_at_quick_exit"), nullptr);
        }

        // aligned_alloc: imported where the library exports one (not the UCRT).
        EXPECT_EQ(symbol("aligned_alloc") != nullptr, kind != ObjectFormatKind::Pe);

        // div_t / ldiv_t / lldiv_t: declared, `long` 32 bits on LLP64 only.
        for (char const* t : {"div_t", "ldiv_t", "lldiv_t"}) {
            bool found = false;
            for (auto const& td : desc->typedefs) found = found || td.name == t;
            EXPECT_TRUE(found) << t << " is not declared";
        }

        // <stddef.h>'s `offsetof` (D-FFI-OFFSETOF-MACRO, corrected in the same fold): the
        // MACRO, onto the intrinsic — the P31 closure shipped only the intrinsic, and DSS's
        // own <stddef.h> is the one a program includes.
        TypeInterner       sInterner{CompilationUnitId{1}};
        TypeRegistry       sTypeReg;
        DiagnosticReporter sRep;
        auto const stddef = readShippedLibDescriptor(path.parent_path() / "stddef.json", sInterner,
                                                     sTypeReg, sRep, model, std::string_view{p.arch},
                                                     kind, {}, nullptr, &facts);
        ASSERT_TRUE(stddef.has_value());
        ShippedMacro const* off = nullptr;
        for (auto const& m : stddef->macros) if (m.name == "offsetof") off = &m;
        ASSERT_NE(off, nullptr) << "<stddef.h> declares no offsetof";
        ASSERT_TRUE(off->params.has_value());
        EXPECT_EQ(*off->params, (std::vector<std::string>{"type", "member"}));
        EXPECT_EQ(off->replacement, "__builtin_offsetof(type, member)");
    }
}

// ── (2) THE COMPILER: every name resolves, and the image imports the entry point ─
TEST(StdlibHIsoSurface, TheProbeLinksAndImportsEachPairsEntryPoints) {
    for (Pair const& p : pairs()) {
        SCOPED_TRACE(p.spec);
        ScratchDir scratch{Location::InsideRepo, "stdlib-iso-surface"};
        fs::path const dir = scratch.path();
        fs::path const src = dir / "probe.c";
        std::ofstream(src, std::ios::binary) << kProbe;

        DiagnosticReporter rep;
        Program prog;
        prog.setOutputDir(dir);
        int const rc = prog.compileFiles(std::vector<std::string>{src.string()}, "c",
                                         std::vector<std::string>{p.spec}, rep);
        ASSERT_EQ(rc, 0) << (rep.all().empty() ? std::string{} : rep.all().front().actual);
        fs::path const artifact = dir / p.artifact;
        ASSERT_TRUE(fs::exists(artifact)) << artifact.generic_string();
        std::ifstream in(artifact, std::ios::binary);
        std::vector<std::uint8_t> const bytes{std::istreambuf_iterator<char>(in),
                                              std::istreambuf_iterator<char>()};
        auto const names = importedSymbolsOf(bytes);
        ASSERT_FALSE(names.empty()) << "the reader recovered no import — every check below would be vacuous";
        std::vector<std::string> missing;
        for (std::string const& want : p.imports)
            if (std::find(names.begin(), names.end(), want) == names.end()) missing.push_back(want);
        EXPECT_TRUE(missing.empty())
            << "not imported: [" << dss::test_support::joinDependencies(missing) << "]; imported: ["
            << dss::test_support::joinDependencies(names) << "]";
    }
}
