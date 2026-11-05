// <inttypes.h>'s C 7.8 surface, pinned PER PAIR (P69, D-FFI-INTTYPES-H-SHIPS-FOUR-FORMAT-MACROS).
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// DSS's <inttypes.h> shipped four format macros (PRIi64, PRIu64, PRIi32, PRIu32) with
// their length modifiers RESTATED per format, so `printf("%" PRId64, x)` — the idiomatic
// way to print a 64-bit integer — did not compile on any pair (✔MEASURED 2026-09-24).
// Now every PRI/SCN macro is an ALIAS of the `type-format` predefine of its type. Two
// instruments, each seeing what the other cannot:
//
//   (1) THE DESCRIPTOR, read for the pair through the real reader: the full macro set
//       (C23's binary ones on every pair — glibc renders them on ELF, DSS's C23 conversion
//       layer on pe and Mach-O), each an alias of the predefine named for its width and
//       conversion; imaxdiv_t; the six functions and HOW each binds (the four conversions:
//       glibc's C23 entry points on ELF, DSS's runtime source `__dss_isoc23_<name>` on pe
//       and Mach-O; imaxabs and imaxdiv each C library's own).
//   (2) THE COMPILER, in process: a probe that expands the macros through the real
//       preprocessor (their string lengths are the pair's modifiers) and names every
//       function links on all five pairs, and its image imports exactly the entry points
//       (1) promises — on pe and Mach-O the platform's own four conversions, which DSS's
//       runtime reaches under private rows, and never a `__dss_isoc23_` name it defines.
//
// RED-ON-DISABLE: drop an elf `linkName` arm and (2) imports the pre-C23 plain name on
// both ELF pairs; point a PRI alias at the wrong predefine and (1) names it; remove the
// `type-format` rows from c.lang.json and (2) fails to compile on every pair.

#include "analysis/compilation_unit/compilation_unit.hpp"   // predefinedTypeFactsFor — the pair's own facts
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
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::Location;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

// (macro width suffix, the predefine stem of its signed type, of its unsigned type)
struct Width {
    std::string_view suffix, signedStem, unsignedStem;
};
constexpr Width kWidths[] = {
    {"8", "INT8", "UINT8"},             {"16", "INT16", "UINT16"},
    {"32", "INT32", "UINT32"},          {"64", "INT64", "UINT64"},
    {"LEAST8", "INT_LEAST8", "UINT_LEAST8"},    {"LEAST16", "INT_LEAST16", "UINT_LEAST16"},
    {"LEAST32", "INT_LEAST32", "UINT_LEAST32"}, {"LEAST64", "INT_LEAST64", "UINT_LEAST64"},
    {"FAST8", "INT_FAST8", "UINT_FAST8"},       {"FAST16", "INT_FAST16", "UINT_FAST16"},
    {"FAST32", "INT_FAST32", "UINT_FAST32"},    {"FAST64", "INT_FAST64", "UINT_FAST64"},
    {"MAX", "INTMAX", "UINTMAX"},       {"PTR", "INTPTR", "UINTPTR"},
};

// Every macro the header defines (C23's binary ones included, on every pair), with the
// predefine it must alias. Written out from C 7.8.1 — never read from the descriptor.
[[nodiscard]] std::map<std::string, std::string> expectedMacros() {
    std::map<std::string, std::string> out;
    for (Width const& w : kWidths) {
        for (std::string_view c : {"d", "i"}) {
            out[std::format("PRI{}{}", c, w.suffix)] = std::format("__{}_FMT{}__", w.signedStem, c);
            out[std::format("SCN{}{}", c, w.suffix)] = std::format("__{}_FMT{}__", w.signedStem, c);
        }
        for (std::string_view c : {"o", "u", "x", "X"}) {
            out[std::format("PRI{}{}", c, w.suffix)] = std::format("__{}_FMT{}__", w.unsignedStem, c);
        }
        for (std::string_view c : {"o", "u", "x"}) {
            out[std::format("SCN{}{}", c, w.suffix)] = std::format("__{}_FMT{}__", w.unsignedStem, c);
        }
        out[std::format("PRIb{}", w.suffix)] = std::format("__{}_FMTb__", w.unsignedStem);
        out[std::format("PRIB{}", w.suffix)] = std::format("__{}_FMTB__", w.unsignedStem);
        out[std::format("SCNb{}", w.suffix)] = std::format("__{}_FMTb__", w.unsignedStem);
    }
    return out;
}

constexpr char const* kFunctions[] = {"imaxabs", "imaxdiv", "strtoimax", "strtoumax",
                                      "wcstoimax", "wcstoumax"};
constexpr char const* kIsoc23[] = {"strtoimax", "strtoumax", "wcstoimax", "wcstoumax"};

struct Pair {
    char const* spec;
    char const* arch;
    char const* formatDoc;
    char const* artifact;
    // sizeof of the literals, i.e. the pair's modifiers: PRId64, PRIdFAST16, PRIuMAX, SCNd8.
    unsigned    pri64, fast16, umax, scn8;
    std::vector<std::string> imports;
};

[[nodiscard]] std::vector<std::string> plainImports(std::string_view prefix) {
    std::vector<std::string> out;
    for (char const* f : kFunctions) out.push_back(std::string{prefix} + f);
    return out;
}

[[nodiscard]] std::vector<std::string> elfImports() {
    std::vector<std::string> out{"imaxabs", "imaxdiv"};
    for (char const* f : kIsoc23) out.push_back(std::string{"__isoc23_"} + f);
    return out;
}

[[nodiscard]] std::vector<Pair> const& pairs() {
    // glibc: int64_t, int_fast16_t, uintmax_t are long / unsigned long ("ld", "ld", "lu");
    // mingw-w64 (the pe pair's identity) and Apple: int64_t long long, int_fast16_t short;
    // uintmax_t unsigned long long on the UCRT, unsigned long on Apple. int8_t is signed
    // char everywhere ("hhd"). sizeof counts the NUL.
    static std::vector<Pair> const all{
        {"x86_64:pe64-x86_64-windows-exec", "x86_64", "pe64-x86_64-windows-exec", "probe.exe",
         4, 3, 4, 4, plainImports("")},
        {"x86_64:elf64-x86_64-linux-exec", "x86_64", "elf64-x86_64-linux-exec", "probe",
         3, 3, 3, 4, elfImports()},
        {"arm64:elf64-aarch64-linux-exec", "arm64", "elf64-aarch64-linux-exec", "probe",
         3, 3, 3, 4, elfImports()},
        {"arm64:macho64-arm64-darwin-exec", "arm64", "macho64-arm64-darwin-exec", "probe",
         4, 3, 3, 4, plainImports("_")},
        {"x86_64:macho64-x86_64-darwin-exec", "x86_64", "macho64-x86_64-darwin-exec", "probe",
         4, 3, 3, 4, plainImports("_")},
    };
    return all;
}

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

[[nodiscard]] bool availableOn(ShippedSymbol const& s, std::string_view fmt) {
    if (s.availableObjectFormats.empty()) return true;
    return std::find(s.availableObjectFormats.begin(), s.availableObjectFormats.end(), fmt)
        != s.availableObjectFormats.end();
}

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

// The probe: the macros through the real preprocessor (their sizes ARE the pair's
// modifiers), C23's binary ones on every pair, and every function by address and called.
[[nodiscard]] std::string probeFor(Pair const& p) {
    return std::format(
        "#include <inttypes.h>\n"
        "#include <stddef.h>\n"
        "_Static_assert(sizeof(PRId64) == {0}, \"PRId64\");\n"
        "_Static_assert(sizeof(PRIdFAST16) == {1}, \"PRIdFAST16\");\n"
        "_Static_assert(sizeof(PRIuMAX) == {2}, \"PRIuMAX\");\n"
        "_Static_assert(sizeof(SCNd8) == {3}, \"SCNd8\");\n"
        "#ifndef PRIb32\n#error \"PRIb32 must be defined on every pair\"\n#endif\n"
        "_Static_assert(sizeof(PRIb32) == 2 && sizeof(SCNbMAX) == {2}, \"binary\");\n"
        "typedef void (*fp)(void);\n"
        "fp const table[] = {{ (fp)&imaxabs, (fp)&imaxdiv, (fp)&strtoimax, (fp)&strtoumax,\n"
        "    (fp)&wcstoimax, (fp)&wcstoumax }};\n"
        "int main(void) {{\n"
        "    char *e = 0; wchar_t *we = 0;\n"
        "    imaxdiv_t q = imaxdiv(7, 2);\n"
        "    intmax_t v = imaxabs(-1) + q.quot + q.rem + strtoimax(\"1\", &e, 10)\n"
        "        + (intmax_t)strtoumax(\"1\", &e, 10) + wcstoimax(L\"1\", &we, 10)\n"
        "        + (intmax_t)wcstoumax(L\"1\", &we, 10);\n"
        "    return table[0] == 0 ? 1 : (int)v;\n"
        "}}\n",
        p.pri64, p.fast16, p.umax, p.scn8);
}

}  // namespace

// ── (1) THE PER-PAIR TRUTH TABLE, THROUGH THE REAL READER ───────────────────────
TEST(InttypesHSurface, EveryFactHoldsOnEveryExecutablePair) {
    ASSERT_NE(cLanguage(), nullptr);
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    fs::path const path = *cfg / "shippedLibs" / "inttypes.json";
    for (Pair const& p : pairs()) {
        SCOPED_TRACE(p.spec);
        auto targetR = TargetSchema::loadShipped(p.arch);
        ASSERT_TRUE(targetR.has_value()) << p.arch;
        auto formatR = ObjectFormatSchema::loadShipped(p.formatDoc);
        ASSERT_TRUE(formatR.has_value()) << p.formatDoc;
        ObjectFormatKind const kind = (*formatR)->kind();
        std::string const      fmt{objectFormatKindName(kind)};
        bool const             elf = (kind == ObjectFormatKind::Elf);
        PredefinedTypeFacts const typeFacts = predefinedTypeFactsFor(**targetR, **formatR);
        ShippedPairFacts const facts{cLanguage().get(), typeFacts.dataModel, typeFacts.charIsUnsigned,
                                     typeFacts.abiTypedefs, typeFacts.longDoubleFormat};
        TypeInterner       interner{CompilationUnitId{1}};
        TypeRegistry       typeReg;
        DiagnosticReporter rep;
        auto const desc = readShippedLibDescriptor(path, interner, typeReg, rep, (*formatR)->dataModel(),
                                                   std::string_view{p.arch}, kind, {}, nullptr, &facts);
        ASSERT_TRUE(desc.has_value());
        ASSERT_FALSE(rep.hasErrors()) << (rep.all().empty() ? std::string{} : rep.all().front().actual);

        std::map<std::string, std::string> got;
        for (auto const& m : desc->macros) got[m.name] = m.replacement;
        auto const want = expectedMacros();
        EXPECT_EQ(got.size(), want.size()) << "the macro set is exactly C 7.8.1's for this pair";
        for (auto const& [name, predefine] : want) {
            auto const it = got.find(name);
            if (it == got.end()) {
                ADD_FAILURE() << name << " is not defined";
                continue;
            }
            EXPECT_EQ(it->second, predefine) << name;
        }

        bool typedefFound = false;
        for (auto const& t : desc->typedefs) typedefFound = typedefFound || t.name == "imaxdiv_t";
        EXPECT_TRUE(typedefFound) << "imaxdiv_t is not declared";

        for (char const* fn : kFunctions) {
            ShippedSymbol const* s = nullptr;
            for (auto const& sym : desc->symbols)
                if (sym.name == fn && availableOn(sym, fmt)) s = &sym;
            ASSERT_NE(s, nullptr) << fn << " is not declared for " << fmt;
            bool const c23 = std::find_if(std::begin(kIsoc23), std::end(kIsoc23), [&](char const* n) {
                                 return std::string_view{n} == fn;
                             }) != std::end(kIsoc23);
            if (elf && c23) {
                EXPECT_EQ(s->linkName, std::string{"__isoc23_"} + fn) << fn;
                EXPECT_EQ(s->version, "GLIBC_2.38") << fn;
                // The per-format map carries the pe/macho arms on every read; ELF's own
                // entry is what must be absent.
                EXPECT_FALSE(s->realization.contains(fmt)) << fn << ": glibc's own C23 entry point";
            } else if (c23) {
                EXPECT_EQ(s->linkName, std::string{"__dss_isoc23_"} + fn) << fn;
                EXPECT_TRUE(s->version.empty()) << fn;
                ASSERT_TRUE(s->realization.contains(fmt)) << fn << ": DSS's C23 entry point on " << fmt;
                EXPECT_EQ(s->realization.at(fmt), "runtime/platform/src/inttypes.c") << fn;
            } else {
                EXPECT_TRUE(s->linkName.empty()) << fn;
                EXPECT_TRUE(s->version.empty()) << fn;
                EXPECT_TRUE(s->realization.empty()) << fn << ": every C library exports it";
            }
        }
    }
}

// ── (2) THE COMPILER: the macros expand to the pair's modifiers, the image imports ─
TEST(InttypesHSurface, TheProbeLinksAndImportsEachPairsEntryPoints) {
    for (Pair const& p : pairs()) {
        SCOPED_TRACE(p.spec);
        bool const elf = std::string_view{p.formatDoc}.starts_with("elf");
        ScratchDir scratch{Location::InsideRepo, "inttypes-surface"};
        fs::path const dir = scratch.path();
        fs::path const src = dir / "probe.c";
        std::ofstream(src, std::ios::binary) << probeFor(p);

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
        for (std::string const& w : p.imports)
            if (std::find(names.begin(), names.end(), w) == names.end()) missing.push_back(w);
        EXPECT_TRUE(missing.empty())
            << "not imported: [" << dss::test_support::joinDependencies(missing) << "]; imported: ["
            << dss::test_support::joinDependencies(names) << "]";
        if (elf) {
            for (char const* f : kIsoc23)
                EXPECT_EQ(std::count(names.begin(), names.end(), std::string{f}), 0)
                    << "the pre-C23 plain `" << f << "` must not be imported";
        } else {
            // The plain name imported above is the PLATFORM's function, reached by DSS's
            // runtime through its private row; the C23 entry point itself is DSS's own.
            std::string const decoration = p.imports.front().starts_with("_") ? "_" : "";
            for (char const* f : kIsoc23)
                EXPECT_EQ(std::count(names.begin(), names.end(), decoration + "__dss_isoc23_" + f), 0)
                    << "DSS defines `__dss_isoc23_" << f << "`: it must never be imported";
        }
    }
}
