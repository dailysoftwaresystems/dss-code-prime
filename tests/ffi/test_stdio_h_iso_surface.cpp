// <stdio.h>'s C 7.23 surface, pinned PER PAIR (P69, D-C-STDIO-H-LACKS-THIRTEEN-ISO-FUNCTIONS).
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// DSS's <stdio.h> refused thirteen of the C 7.23 functions — tmpfile, tmpnam, setbuf,
// fscanf, scanf, vfscanf, vprintf, vscanf, vsnprintf, vsprintf, vsscanf, fgetpos, fsetpos
// — the type fpos_t and the macros BUFSIZ, FOPEN_MAX, L_tmpnam and TMP_MAX, on every
// pair (✔MEASURED 2026-09-24, P68 round 11). Every one of those facts is PER PAIR, so this
// file pins them on every executable pair through two instruments that each see what the
// other cannot (the <stdlib.h> surface test's shape):
//
//   (1) THE DESCRIPTOR, read for the pair through the real reader: every name declared,
//       each macro's value as the pair's C library defines it, fpos_t's shape, and HOW
//       each function is realized — an import of the C library's own name, glibc's C23
//       entry point (the scanf family: `__isoc23_<name>` @GLIBC_2.38), or, on pe and
//       Mach-O, DSS's runtime source under `__dss_isoc23_<name>` (the UCRT exports none of
//       the printf or scanf family, only their `__stdio_common_v*` cores, and neither it nor
//       libSystem renders C23's conversions, the row
//       D-C-C23-CONVERSIONS-MISSING-ON-THE-UCRT-AND-LIBSYSTEM; test_c23_conversion_bindings
//       pins the private rows that reach them).
//   (2) THE COMPILER, in process: one probe naming every function by address and every
//       macro by value links on all five pairs, and its image imports exactly the entry
//       points (1) promises — on pe and Mach-O none of the family names DSS defines, only
//       the platform primitives and stream lock its runtime reaches through private rows.
//
// THE PER-PAIR FACTS, ✔MEASURED (P69 lane lm, probe-reference-cc; P68 round 11 for glibc):
//   * BUFSIZ 8192 glibc 2.39 / 1024 Apple / 512 UCRT; FOPEN_MAX 16 / 20 / 20; L_tmpnam
//     20 / 1024 / 260; TMP_MAX 238328 / 308915776 / 2147483647 — each an `int`.
//   * _PRINTF_NAN_LEN_MAX (C23 7.23.1, the longest [-]NAN(n-char-sequence) the library's
//     printf writes): glibc defines 4; Apple prints only "nan" (3); the UCRT's longest is
//     "-nan(snan)" (10).
//   * fpos_t: glibc's `_G_fpos_t` (a `long` offset and an `__mbstate_t`, 16 bytes); a
//     `long long` offset on Apple and the UCRT.
//
// RED-ON-DISABLE: drop any of the thirteen rows and (1) and (2) fail on every pair that
// lost it; drop a scanf row's elf `linkName` and (2) imports the pre-C23 plain name on
// both ELF pairs; drop a pe row's `realization` and (1) fails on pe and (2)'s image
// imports a name ucrtbase does not export.

#include "analysis/compilation_unit/compilation_unit.hpp"   // predefinedTypeFactsFor — the pair's own facts
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/named_type_binding.hpp"
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
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::Location;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

// The thirteen functions the row names, by the C identifier a program calls.
constexpr char const* kThirteen[] = {
    "tmpfile", "tmpnam", "setbuf", "fscanf", "scanf", "vfscanf", "vprintf",
    "vscanf", "vsnprintf", "vsprintf", "vsscanf", "fgetpos", "fsetpos"};

// The printf and scanf families — the fourteen names whose pe and Mach-O realization is
// DSS's runtime source.
constexpr char const* kFamily[] = {
    "vfprintf", "vprintf", "fprintf", "printf", "vsprintf", "sprintf", "vsnprintf",
    "snprintf", "vfscanf", "vscanf", "fscanf", "scanf", "vsscanf", "sscanf"};

// The scanf family — glibc's C23 entry points on ELF.
constexpr char const* kScanf[] = {"sscanf", "scanf", "fscanf", "vscanf", "vfscanf", "vsscanf"};

struct Pair {
    char const*  spec;
    char const*  arch;
    char const*  formatDoc;
    char const*  artifact;
    std::int64_t bufsiz, fopenMax, lTmpnam, tmpMax, nanLenMax;
    TypeKind     fposKind;
    // The decorated names the linked probe must import, and those it must NOT.
    std::vector<std::string> imports;
    std::vector<std::string> notImported;
};

// Mach-O: libSystem's own primitives and stream lock, reached by DSS's runtime under its
// private rows' link names — and none of the family names DSS itself defines.
[[nodiscard]] std::vector<std::string> machoImports() {
    return {"_tmpfile", "_tmpnam", "_setbuf", "_fgetpos", "_fsetpos", "_vfprintf", "_vsprintf",
            "_vsnprintf", "_vfscanf", "_vsscanf", "_flockfile", "_funlockfile"};
}

[[nodiscard]] std::vector<std::string> machoNotImported() {
    std::vector<std::string> out{"_printf", "_fprintf", "_sprintf", "_snprintf", "_vprintf",
                                 "_scanf", "_fscanf", "_vscanf", "_sscanf"};
    for (char const* f : kFamily) out.push_back(std::string{"___dss_isoc23_"} + f);
    return out;
}

[[nodiscard]] std::vector<std::string> elfImports() {
    std::vector<std::string> out{"tmpfile", "tmpnam", "setbuf", "fgetpos", "fsetpos",
                                 "vfprintf", "vprintf", "fprintf", "printf", "vsprintf",
                                 "sprintf", "vsnprintf", "snprintf"};
    for (char const* f : kScanf) out.push_back(std::string{"__isoc23_"} + f);
    return out;
}

[[nodiscard]] std::vector<std::string> peNotImported() {
    std::vector<std::string> out;
    for (char const* f : kFamily) {
        out.emplace_back(f);
        out.push_back(std::string{"__dss_isoc23_"} + f);
    }
    return out;
}

[[nodiscard]] std::vector<Pair> const& pairs() {
    static std::vector<Pair> const all{
        {"x86_64:pe64-x86_64-windows-exec", "x86_64", "pe64-x86_64-windows-exec", "probe.exe",
         512, 20, 260, 2147483647, 10, TypeKind::I64,
         {"tmpfile", "tmpnam", "setbuf", "fgetpos", "fsetpos", "__stdio_common_vfprintf",
          "__stdio_common_vsprintf", "__stdio_common_vfscanf", "__stdio_common_vsscanf",
          "__acrt_iob_func", "_lock_file", "_unlock_file"},
         peNotImported()},
        {"x86_64:elf64-x86_64-linux-exec", "x86_64", "elf64-x86_64-linux-exec", "probe",
         8192, 16, 20, 238328, 4, TypeKind::Struct, elfImports(),
         {"sscanf", "scanf", "fscanf", "vscanf", "vfscanf", "vsscanf"}},
        {"arm64:elf64-aarch64-linux-exec", "arm64", "elf64-aarch64-linux-exec", "probe",
         8192, 16, 20, 238328, 4, TypeKind::Struct, elfImports(),
         {"sscanf", "scanf", "fscanf", "vscanf", "vfscanf", "vsscanf"}},
        {"arm64:macho64-arm64-darwin-exec", "arm64", "macho64-arm64-darwin-exec", "probe",
         1024, 20, 1024, 308915776, 3, TypeKind::I64, machoImports(), machoNotImported()},
        {"x86_64:macho64-x86_64-darwin-exec", "x86_64", "macho64-x86_64-darwin-exec", "probe",
         1024, 20, 1024, 308915776, 3, TypeKind::I64, machoImports(), machoNotImported()},
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

[[nodiscard]] fs::path stdioDescriptor() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg / "shippedLibs" / "stdio.json";
}

// The `va_list` binding every read of stdio.json needs (its v* rows spell the alias). Its
// identity is irrelevant to these pins — none compares a v* signature — so one pointer
// shape serves every pair.
[[nodiscard]] std::array<NamedTypeBinding, 1> vaListBinding(TypeInterner& interner) {
    return {NamedTypeBinding{"va_list", interner.pointer(interner.primitive(TypeKind::Void))}};
}

[[nodiscard]] bool availableOn(ShippedSymbol const& s, std::string_view fmt) {
    if (s.availableObjectFormats.empty()) return true;
    return std::find(s.availableObjectFormats.begin(), s.availableObjectFormats.end(), fmt)
        != s.availableObjectFormats.end();
}

// Every undefined dynamic symbol of an ELF64 image (the import side).
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

// Every undefined external symbol of a Mach-O 64 image.
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

// The probe: every function BY ADDRESS (a row the injection dropped is then undeclared,
// not merely unused), every macro BY VALUE, fpos_t by object — and the functions that do
// not read stdin CALLED, so the pe image links the runtime member and imports its cores.
constexpr char const* kProbe =
    "#include <stdarg.h>\n"
    "#include <stdio.h>\n"
    "typedef void (*fp)(void);\n"
    "fp const table[] = { (fp)&tmpfile, (fp)&tmpnam, (fp)&setbuf, (fp)&fscanf, (fp)&scanf,\n"
    "    (fp)&vfscanf, (fp)&vprintf, (fp)&vscanf, (fp)&vsnprintf, (fp)&vsprintf, (fp)&vsscanf,\n"
    "    (fp)&fgetpos, (fp)&fsetpos, (fp)&vfprintf, (fp)&fprintf, (fp)&printf, (fp)&sprintf,\n"
    "    (fp)&snprintf, (fp)&sscanf };\n"
    "#if !defined(BUFSIZ) || !defined(FOPEN_MAX) || !defined(L_tmpnam) || !defined(TMP_MAX)\n"
    "#error missing\n"
    "#endif\n"
    "static int bounded(char *b, size_t n, const char *f, ...) {\n"
    "    va_list ap; va_start(ap, f); int r = vsnprintf(b, n, f, ap); va_end(ap); return r;\n"
    "}\n"
    "static int readBack(const char *in, const char *f, ...) {\n"
    "    va_list ap; va_start(ap, f); int r = vsscanf(in, f, ap); va_end(ap); return r;\n"
    "}\n"
    "int main(void) {\n"
    "    static char buf[BUFSIZ];\n"
    "    char name[L_tmpnam];\n"
    "    int v = 0;\n"
    "    long long sum = BUFSIZ + FOPEN_MAX + L_tmpnam + (long long)TMP_MAX;\n"
    "    FILE *f = tmpfile();\n"
    "    if (f != 0) {\n"
    "        fpos_t pos;\n"
    "        setbuf(f, buf);\n"
    "        fprintf(f, \"%d\", 7);\n"
    "        if (fgetpos(f, &pos) == 0 && fsetpos(f, &pos) == 0) sum += fscanf(f, \"%d\", &v);\n"
    "        fclose(f);\n"
    "    }\n"
    "    sum += bounded(buf, sizeof buf, \"%d\", 42) + readBack(\"5\", \"%d\", &v)\n"
    "        + sscanf(\"6\", \"%d\", &v) + snprintf(buf, 8, \"%d\", 1) + sprintf(buf, \"%d\", 2)\n"
    "        + (tmpnam(name) != 0) + printf(\"%s\\n\", buf);\n"
    "    return table[0] == 0 ? 1 : (int)(sum & 0x3F);\n"
    "}\n";

}  // namespace

// ── (1) THE PER-PAIR TRUTH TABLE, THROUGH THE REAL READER ───────────────────────
TEST(StdioHIsoSurface, EveryFactHoldsOnEveryExecutablePair) {
    ASSERT_NE(cLanguage(), nullptr);
    fs::path const path = stdioDescriptor();
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

        PredefinedTypeFacts const typeFacts = predefinedTypeFactsFor(target, format);
        ShippedPairFacts const facts{cLanguage().get(), typeFacts.dataModel, typeFacts.charIsUnsigned,
                                     typeFacts.abiTypedefs, typeFacts.longDoubleFormat};
        TypeInterner       interner{CompilationUnitId{1}};
        TypeRegistry       typeReg;
        DiagnosticReporter rep;
        auto const vaList = vaListBinding(interner);
        auto const desc = readShippedLibDescriptor(path, interner, typeReg, rep, format.dataModel(),
                                                   std::string_view{p.arch}, kind, vaList, nullptr,
                                                   &facts);
        ASSERT_TRUE(desc.has_value());
        ASSERT_FALSE(rep.hasErrors()) << (rep.all().empty() ? std::string{} : rep.all().front().actual);

        auto symbol = [&](std::string_view n) -> ShippedSymbol const* {
            ShippedSymbol const* found = nullptr;
            for (auto const& s : desc->symbols) {
                if (s.name != n || !availableOn(s, fmt)) continue;
                EXPECT_EQ(found, nullptr) << n << " is declared twice for " << fmt;
                found = &s;
            }
            return found;
        };
        for (char const* fn : kThirteen)
            EXPECT_NE(symbol(fn), nullptr) << fn << " is not declared for " << fmt;

        // How each family member is realized on this pair.
        for (char const* fn : kFamily) {
            ShippedSymbol const* s = symbol(fn);
            ASSERT_NE(s, nullptr) << fn;
            EXPECT_TRUE(s->synthesize.empty()) << fn << ": the stdio recipes were retired in P69";
            bool const isScanf = std::find_if(std::begin(kScanf), std::end(kScanf), [&](char const* n) {
                                     return std::string_view{n} == fn;
                                 }) != std::end(kScanf);
            if (kind == ObjectFormatKind::Pe || kind == ObjectFormatKind::MachO) {
                ASSERT_TRUE(s->realization.contains(fmt)) << fn;
                EXPECT_EQ(s->realization.at(fmt), "runtime/platform/src/stdio.c") << fn;
                EXPECT_EQ(s->linkName, std::string{"__dss_isoc23_"} + fn) << fn;
                ASSERT_TRUE(s->library.contains(fmt))
                    << fn << ": the row's own source must supersede the inherited cLibrary import";
                EXPECT_TRUE(s->library.at(fmt).empty()) << fn;
            } else if (isScanf) {
                EXPECT_EQ(s->linkName, std::string{"__isoc23_"} + fn) << fn;
                EXPECT_EQ(s->version, "GLIBC_2.38") << fn;
                EXPECT_TRUE(s->realization.empty()) << fn;
            } else {
                EXPECT_TRUE(s->linkName.empty()) << fn;
                EXPECT_TRUE(s->version.empty()) << fn;
                EXPECT_TRUE(s->realization.empty()) << fn;
            }
        }

        // The five macros, by value, as `int`, visible to #if.
        for (auto const& [name, want] : {std::pair<char const*, std::int64_t>{"BUFSIZ", p.bufsiz},
                                         {"FOPEN_MAX", p.fopenMax}, {"L_tmpnam", p.lTmpnam},
                                         {"TMP_MAX", p.tmpMax},
                                         {"_PRINTF_NAN_LEN_MAX", p.nanLenMax}}) {
            ShippedConstant const* c = nullptr;
            for (auto const& k : desc->constants) if (k.name == name) c = &k;
            ASSERT_NE(c, nullptr) << name << " is not declared";
            EXPECT_EQ(c->value, want) << name;
            EXPECT_EQ(interner.kind(c->type), TypeKind::I32) << name << " must be an int";
            EXPECT_TRUE(c->preprocessorVisible) << name << " must be visible to #if";
        }

        // fpos_t: the library's own object shape.
        ShippedTypedef const* fpos = nullptr;
        for (auto const& t : desc->typedefs) if (t.name == "fpos_t") fpos = &t;
        ASSERT_NE(fpos, nullptr) << "fpos_t is not declared";
        EXPECT_EQ(interner.kind(fpos->type), p.fposKind);
    }
}

// ── (2) THE COMPILER: every name resolves, and the image imports the entry points ─
TEST(StdioHIsoSurface, TheProbeLinksAndImportsEachPairsEntryPoints) {
    for (Pair const& p : pairs()) {
        SCOPED_TRACE(p.spec);
        ScratchDir scratch{Location::InsideRepo, "stdio-iso-surface"};
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
        std::vector<std::string> missing, stray;
        for (std::string const& want : p.imports)
            if (std::find(names.begin(), names.end(), want) == names.end()) missing.push_back(want);
        for (std::string const& never : p.notImported)
            if (std::find(names.begin(), names.end(), never) != names.end()) stray.push_back(never);
        EXPECT_TRUE(missing.empty())
            << "not imported: [" << dss::test_support::joinDependencies(missing) << "]; imported: ["
            << dss::test_support::joinDependencies(names) << "]";
        EXPECT_TRUE(stray.empty())
            << "imported, and must not be: [" << dss::test_support::joinDependencies(stray) << "]";
    }
}
