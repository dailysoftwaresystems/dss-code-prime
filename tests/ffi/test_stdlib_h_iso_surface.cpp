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
//   * strtoull binds glibc's C23 entry point `__isoc23_strtoull` (@GLIBC_2.38) on both
//     ELF arches, as gcc -std=c2x does (P69, D-C-GLIBC-C23-ENTRY-POINTS-NOT-BOUND); the
//     probe's ELF import lists name it. On pe and Mach-O it is DSS's C23 entry point
//     `__dss_isoc23_strtoull` (runtime/platform/src/stdlib_strto.c, P69
//     D-C-C23-CONVERSIONS-MISSING-ON-THE-UCRT-AND-LIBSYSTEM), defined in the image, which
//     reaches the platform's own strtoull through a private row — so the plain
//     `strtoull` / `_strtoull` the probe's pe and Mach-O lists name is still imported,
//     now by DSS's runtime rather than by the probe.
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

#include "analysis/compilation_unit/compilation_unit.hpp"   // predefinedTypeFactsFor — the pair's own facts
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
         {"__ctype_get_mb_cur_max", "div", "ldiv", "lldiv", "atoll", "llabs", "__isoc23_strtoull",
          "strtof", "_Exit", "quick_exit", "__cxa_at_quick_exit", "mblen", "aligned_alloc"}},
        {"arm64:elf64-aarch64-linux-exec", "arm64", "elf64-aarch64-linux-exec", "probe",
         2147483647, "(__ctype_get_mb_cur_max ())", "__ctype_get_mb_cur_max", "GLIBC_2.24",
         {"__ctype_get_mb_cur_max", "div", "ldiv", "lldiv", "atoll", "llabs", "__isoc23_strtoull",
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
    "    void *p = aligned_alloc(16, 16); free(p);\n"
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

        // The pair's facts from their one owner (P68 round 12, S2a-2a): this built them
        // by hand and left out the long-double format a signature arm may key on.
        PredefinedTypeFacts const typeFacts = predefinedTypeFactsFor(target, format);
        ShippedPairFacts const facts{cLanguage().get(), typeFacts.dataModel, typeFacts.charIsUnsigned,
                                     typeFacts.abiTypedefs, typeFacts.longDoubleFormat};

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

        // aligned_alloc: declared on every pair since P69 — the library's export where it
        // has one, DSS's runtime source on pe (the UCRT exports none; test (3) below).
        EXPECT_NE(symbol("aligned_alloc"), nullptr);

        // div_t / ldiv_t / lldiv_t: declared, and each member the width its C type has on
        // this pair's data model — `long` is 32 bits on LLP64 only. (This comment used to
        // claim the width while the code checked only the name — a round-12 audit finding,
        // D-AUDIT-P68-ROUND-12-MINOR-FINDINGS; P69 makes the check.)
        struct DivType {
            char const* name;
            TypeKind    lp64;
            TypeKind    llp64;
        };
        for (DivType const& d : {DivType{"div_t", TypeKind::I32, TypeKind::I32},
                                 DivType{"ldiv_t", TypeKind::I64, TypeKind::I32},
                                 DivType{"lldiv_t", TypeKind::I64, TypeKind::I64}}) {
            TypeId ty{};
            for (auto const& td : desc->typedefs)
                if (td.name == d.name) ty = td.type;
            ASSERT_TRUE(ty.valid()) << d.name << " is not declared";
            auto const members = interner.operands(ty);
            ASSERT_EQ(members.size(), 2u) << d.name << ": {quot, rem}";
            TypeKind const want = model == DataModel::Llp64 ? d.llp64 : d.lp64;
            for (TypeId const m : members)
                EXPECT_EQ(interner.kind(m), want) << d.name << " on " << fmt;
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

// ── (3) THE REST OF C23's <stdlib.h>, PER PAIR (P69 row 1 of the same anchor) ─────
//
// The names round 12 left, through the real reader on every executable pair:
//   * mbtowc, wctomb, mbstowcs, wcstombs, strtold — the platform's own export on every
//     pair (✔MEASURED P69: glibc 2.39, the UCRT through MSVC 19.51 and mingw-w64 13.2.0,
//     libSystem), each typed with the PAIR's wchar_t — u16 on pe, i32 on ELF x86_64 and
//     Mach-O, u32 on ELF aarch64 (stddef.json's ABI typedef) — and long double;
//   * strfromd, strfromf, strfroml — glibc's exports (@GLIBC_2.25) on ELF;
//     runtime/platform/src/stdlib_strfrom.c on pe and Mach-O, whose libraries export none;
//   * free_sized, free_aligned_sized — runtime/platform/src/stdlib.c on EVERY pair (no
//     platform exports either);
//   * aligned_alloc — the platform's on ELF and Mach-O, runtime/platform/src/
//     stdlib_aligned_alloc.c on pe (the UCRT exports none);
//   * C23 7.24p2's once trio — once_flag (a REFERENCE to <threads.h>'s, its one owner),
//     ONCE_FLAG_INIT and call_once — each IDENTICAL to threads.json's for the pair: the
//     macro's replacement, the typedef's type, and the call_once row's signature, link
//     name and realization (DSS's runtime/platform/src/threads_once.c on pe, libSystem's
//     pthread_once on Mach-O, glibc's call_once on ELF). Two descriptors declare the trio,
//     so this is the test that keeps them one declaration.
// RED-ON-DISABLE: drop any row and it reds on every pair that lost it; give strfromd an
// ELF realization and both ELF pairs red; let stdlib.json's Mach-O ONCE_FLAG_INIT drift
// from threads.json's ({816954554}, the PTHREAD_ONCE_INIT signature) and both Mach-O pairs red.
TEST(StdlibHIsoSurface, TheRestOfC23sStdlibIsDeclaredAndBoundOnEveryPair) {
    ASSERT_NE(cLanguage(), nullptr);
    fs::path const path = stdlibDescriptor();
    ASSERT_FALSE(path.empty());
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    for (Pair const& p : pairs()) {
        SCOPED_TRACE(p.spec);
        auto targetR = TargetSchema::loadShipped(p.arch);
        ASSERT_TRUE(targetR.has_value()) << p.arch;
        auto formatR = ObjectFormatSchema::loadShipped(p.formatDoc);
        ASSERT_TRUE(formatR.has_value()) << p.formatDoc;
        ObjectFormatKind const kind = (*formatR)->kind();
        std::string const      fmt{objectFormatKindName(kind)};
        bool const             pe    = kind == ObjectFormatKind::Pe;
        bool const             elf   = kind == ObjectFormatKind::Elf;
        bool const             arm64 = std::string_view{p.arch} == "arm64";
        PredefinedTypeFacts const typeFacts = predefinedTypeFactsFor(**targetR, **formatR);
        ShippedPairFacts const facts{cLanguage().get(), typeFacts.dataModel, typeFacts.charIsUnsigned,
                                     typeFacts.abiTypedefs, typeFacts.longDoubleFormat};

        // ONE interner for both descriptors, so a type both spell is one TypeId.
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        auto read = [&](fs::path const& file) -> std::optional<ShippedLibDescriptor> {
            DiagnosticReporter rep;
            auto desc = readShippedLibDescriptor(file, interner, typeReg, rep, (*formatR)->dataModel(),
                                                 std::string_view{p.arch}, kind, {}, nullptr, &facts);
            EXPECT_FALSE(rep.hasErrors()) << file.filename().string() << ": "
                                          << (rep.all().empty() ? std::string{} : rep.all().front().actual);
            return desc;
        };
        auto const stdlib  = read(path);
        auto const threads = read(path.parent_path() / "threads.json");
        ASSERT_TRUE(stdlib.has_value());
        ASSERT_TRUE(threads.has_value());
        auto row = [&](ShippedLibDescriptor const& d, std::string_view n) -> ShippedSymbol const* {
            ShippedSymbol const* found = nullptr;
            for (auto const& s : d.symbols) {
                if (s.name != n || !availableOn(s, fmt)) continue;
                EXPECT_EQ(found, nullptr) << n << " has two rows on " << fmt;
                found = &s;
            }
            return found;
        };
        // The unit a row is realized from on THIS pair, or "" for an import.
        auto unitOf = [&](ShippedSymbol const& s) -> std::string {
            auto const r = s.realization.find(fmt);
            return r == s.realization.end() ? std::string{} : r->second;
        };

        // The imports, with the pair's wchar_t and long double.
        TypeKind const wcharKind = pe ? TypeKind::U16 : (elf && arm64 ? TypeKind::U32 : TypeKind::I32);
        TypeKind const ldKind    = pe || (arm64 && !elf) ? TypeKind::F64
                                                         : (arm64 ? TypeKind::F128 : TypeKind::F80);
        for (char const* n : {"mbtowc", "wctomb", "mbstowcs", "wcstombs", "strtold"}) {
            SCOPED_TRACE(n);
            ShippedSymbol const* s = row(*stdlib, n);
            ASSERT_NE(s, nullptr) << n << " is not declared on " << fmt;
            EXPECT_EQ(unitOf(*s), "") << "every platform exports it";
            EXPECT_TRUE(s->linkName.empty());
        }
        auto pointeeKind = [&](TypeId ptr) {
            auto const ops = interner.operands(ptr);
            return ops.empty() ? TypeKind::Void : interner.kind(ops[0]);
        };
        ShippedSymbol const* mbtowc = row(*stdlib, "mbtowc");
        ShippedSymbol const* strtold = row(*stdlib, "strtold");
        ASSERT_NE(mbtowc, nullptr);
        ASSERT_NE(strtold, nullptr);
        auto const mbParams = interner.fnArgumentParams(mbtowc->signature);
        ASSERT_FALSE(mbParams.empty());
        EXPECT_EQ(pointeeKind(mbParams[0]), wcharKind) << "mbtowc's wchar_t * is the pair's wchar_t";
        EXPECT_EQ(interner.kind(interner.fnResult(strtold->signature)), ldKind)
            << "strtold returns the pair's long double";

        // The realized names.
        for (char const* n : {"strfromd", "strfromf", "strfroml"}) {
            ShippedSymbol const* s = row(*stdlib, n);
            ASSERT_NE(s, nullptr) << n;
            EXPECT_EQ(unitOf(*s), elf ? std::string{} : std::string{"runtime/platform/src/stdlib_strfrom.c"}) << n;
        }
        for (char const* n : {"free_sized", "free_aligned_sized"}) {
            ShippedSymbol const* s = row(*stdlib, n);
            ASSERT_NE(s, nullptr) << n;
            EXPECT_EQ(unitOf(*s), "runtime/platform/src/stdlib.c") << n;
        }
        ShippedSymbol const* aa = row(*stdlib, "aligned_alloc");
        ASSERT_NE(aa, nullptr);
        EXPECT_EQ(unitOf(*aa), pe ? std::string{"runtime/platform/src/stdlib_aligned_alloc.c"} : std::string{});
        for (ShippedSymbol const* s : {row(*stdlib, "strfromd"), row(*stdlib, "free_sized"), aa}) {
            std::string const unit = s == nullptr ? std::string{} : unitOf(*s);
            if (!unit.empty()) EXPECT_TRUE(fs::exists(*cfg / unit)) << unit << " does not exist";
        }

        // The once trio: one declaration, written in two descriptors.
        ShippedMacro const* init = nullptr;
        ShippedMacro const* initT = nullptr;
        for (auto const& m : stdlib->macros) if (m.name == "ONCE_FLAG_INIT") init = &m;
        for (auto const& m : threads->macros) if (m.name == "ONCE_FLAG_INIT") initT = &m;
        ASSERT_NE(init, nullptr) << "<stdlib.h> declares no ONCE_FLAG_INIT";
        ASSERT_NE(initT, nullptr);
        EXPECT_EQ(init->replacement, initT->replacement);
        EXPECT_EQ(init->replacement, kind == ObjectFormatKind::MachO ? "{816954554}" : "{0}");
        TypeId flag{};
        TypeId flagT{};
        for (auto const& t : stdlib->typedefs) if (t.name == "once_flag") flag = t.type;
        for (auto const& t : threads->typedefs) if (t.name == "once_flag") flagT = t.type;
        ASSERT_TRUE(flag.valid()) << "<stdlib.h> declares no once_flag";
        EXPECT_EQ(flag, flagT) << "once_flag is <threads.h>'s, never a second definition";
        ShippedSymbol const* once  = row(*stdlib, "call_once");
        ShippedSymbol const* onceT = row(*threads, "call_once");
        ASSERT_NE(once, nullptr) << "<stdlib.h> declares no call_once";
        ASSERT_NE(onceT, nullptr);
        EXPECT_EQ(once->signature, onceT->signature);
        EXPECT_EQ(once->linkName, onceT->linkName);
        EXPECT_EQ(unitOf(*once), unitOf(*onceT));
        EXPECT_TRUE(once->synthesize.empty()) << "call_once is no synthesized recipe since P69";
        EXPECT_EQ(unitOf(*once), pe ? std::string{"runtime/platform/src/threads_once.c"} : std::string{});
        EXPECT_EQ(once->linkName, kind == ObjectFormatKind::MachO ? std::string{"pthread_once"} : std::string{});
        // pe's unit reaches kernel32 through threads.json's one private row, on pe only.
        ShippedSymbol const* ioe = row(*threads, "__dss_platform_init_once_execute_once");
        if (pe) {
            ASSERT_NE(ioe, nullptr);
            EXPECT_EQ(ioe->linkName, "InitOnceExecuteOnce");
            EXPECT_EQ(unitOf(*ioe), "");
        } else {
            EXPECT_EQ(ioe, nullptr) << "only pe's call_once needs the platform's one-time primitive";
        }
    }
}
