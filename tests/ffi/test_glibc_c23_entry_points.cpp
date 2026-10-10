// glibc's C23 ENTRY POINTS, bound on both ELF arches (P69, D-C-GLIBC-C23-ENTRY-POINTS-NOT-BOUND).
//
// Under C23, glibc 2.38+ binds the conversions whose meaning C23 changed to their own
// entry points — `strtol` accepts a `0b` prefix, scanf's `%i` does too and `%b` is new —
// and DSS, which is C23, bound the pre-C23 plain names (its plain `sscanf` is glibc's
// LEGACY GNU scanf). ✔MEASURED gcc 13.3.0 -std=c2x on glibc 2.39, x86_64 and aarch64:
// the objects import `__isoc23_<name>@GLIBC_2.38`. This pin compiles a probe that calls
// every such name DSS ships, for both ELF pairs, and reads the IMAGE: each call must
// import `__isoc23_<name>` under a version requirement for GLIBC_2.38, the plain name
// must not be imported, and no OTHER import may carry GLIBC_2.38 — exactly the names in
// the table, so a row that lost its version or a stray versioned row both fail here.
//
// RED-ON-DISABLE: drop one row's `linkName` arm and that name is imported plain; drop its
// `version` arm and it is imported unversioned — each fails on both arches.

#include "core/types/diagnostic_reporter.hpp"
#include "image_dependency_table.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

// The C23 entry points DSS binds on ELF, by the C name a program calls — the FOURTEEN glibc
// 2.39 binds under -std=c2x for the headers DSS ships: <stdlib.h>'s four, <stdio.h>'s whole
// scanf family and <inttypes.h>'s four.
constexpr std::string_view kIsoc23Names[] = {
    "strtol", "strtoll", "strtoul", "strtoull",
    "sscanf", "scanf", "fscanf", "vscanf", "vfscanf", "vsscanf",
    "strtoimax", "strtoumax", "wcstoimax", "wcstoumax",
};

// Compiled and read, never run: the stdin readers sit behind an `argc` no test passes, so
// the probe references every name without ever waiting on input.
constexpr char const* kProbe =
    "#include <inttypes.h>\n"
    "#include <stdarg.h>\n"
    "#include <stddef.h>\n"
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "static int viaVscanf(const char *f, ...) {\n"
    "    va_list ap; va_start(ap, f); int r = vscanf(f, ap); va_end(ap); return r;\n"
    "}\n"
    "static int viaVfscanf(FILE *s, const char *f, ...) {\n"
    "    va_list ap; va_start(ap, f); int r = vfscanf(s, f, ap); va_end(ap); return r;\n"
    "}\n"
    "static int viaVsscanf(const char *in, const char *f, ...) {\n"
    "    va_list ap; va_start(ap, f); int r = vsscanf(in, f, ap); va_end(ap); return r;\n"
    "}\n"
    "int main(int argc, char **argv) {\n"
    "    (void)argv;\n"
    "    char *end = 0;\n"
    "    int got = 0;\n"
    "    long a = strtol(\"0b101\", &end, 0);\n"
    "    long long b = strtoll(\"0b101\", &end, 0);\n"
    "    unsigned long c = strtoul(\"0b101\", &end, 0);\n"
    "    unsigned long long d = strtoull(\"0b101\", &end, 0);\n"
    "    int n = sscanf(\"0b101\", \"%i\", &got) + viaVsscanf(\"0b11\", \"%i\", &got);\n"
    "    wchar_t *wend = 0;\n"
    "    intmax_t m = strtoimax(\"0b101\", &end, 0) + (intmax_t)strtoumax(\"0b1\", &end, 0)\n"
    "        + wcstoimax(L\"0b11\", &wend, 0) + (intmax_t)wcstoumax(L\"0b1\", &wend, 0);\n"
    "    n += (int)m;\n"
    "    if (argc > 9) {\n"
    "        n += scanf(\"%i\", &got) + fscanf(stdin, \"%i\", &got);\n"
    "        n += viaVscanf(\"%i\", &got) + viaVfscanf(stdin, \"%i\", &got);\n"
    "    }\n"
    "    return (int)(a + b + (long long)c + (long long)d) + n + got;\n"
    "}\n";

// Every UNDEFINED dynamic symbol of an ELF64 image, with the version its `.gnu.version`
// entry requires (resolved through `.gnu.version_r`); "" for an unversioned import.
[[nodiscard]] std::map<std::string, std::string>
elfImportVersions(std::vector<std::uint8_t> const& b) {
    using namespace dss::test_support::image_deps_detail;
    std::map<std::string, std::string> out;
    if (b.size() < 0x40 || !(b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F')) return out;
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
    std::size_t symOff = 0, symSize = 0, strOff = 0, versymOff = 0, verneedOff = 0;
    std::size_t verneedCount = 0;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::size_t const o  = secOff(i);
        std::string const nm = rdCStr(b, shstrBase + rdU32(b, o + 0));
        std::size_t const at = static_cast<std::size_t>(rdU64(b, o + 0x18));
        if (nm == ".dynsym") {
            symOff  = at;
            symSize = static_cast<std::size_t>(rdU64(b, o + 0x20));
        } else if (nm == ".dynstr") {
            strOff = at;
        } else if (nm == ".gnu.version") {
            versymOff = at;
        } else if (nm == ".gnu.version_r") {
            verneedOff   = at;
            verneedCount = rdU32(b, o + 0x2C);   // sh_info: the number of Verneed entries
        }
    }
    if (symOff == 0 || strOff == 0) return out;
    // versym index → the version name a Vernaux entry gives it.
    std::map<std::uint16_t, std::string> versionOfIndex;
    std::size_t need = verneedOff;
    for (std::size_t n = 0; verneedOff != 0 && n < verneedCount && need + 16 <= b.size(); ++n) {
        std::uint16_t const cnt  = rdU16(b, need + 2);
        std::uint32_t const aux  = rdU32(b, need + 8);
        std::uint32_t const next = rdU32(b, need + 12);
        std::size_t vna = need + aux;
        for (std::uint16_t k = 0; k < cnt && vna + 16 <= b.size(); ++k) {
            std::uint16_t const other = rdU16(b, vna + 6);
            std::uint32_t const name  = rdU32(b, vna + 8);
            std::uint32_t const vnext = rdU32(b, vna + 12);
            versionOfIndex[other] = rdCStr(b, strOff + name);
            if (vnext == 0) break;
            vna += vnext;
        }
        if (next == 0) break;
        need += next;
    }
    std::size_t index = 0;
    for (std::size_t o = symOff; o + 24 <= symOff + symSize; o += 24, ++index) {
        std::uint32_t const stName  = rdU32(b, o + 0);
        std::uint16_t const stShndx = rdU16(b, o + 6);
        if (stShndx != 0 || stName == 0) continue;
        std::string version;
        if (versymOff != 0) {
            std::uint16_t const vs = static_cast<std::uint16_t>(rdU16(b, versymOff + 2 * index) & 0x7FFFu);
            if (auto const it = versionOfIndex.find(vs); it != versionOfIndex.end()) version = it->second;
        }
        out[rdCStr(b, strOff + stName)] = version;
    }
    return out;
}

}  // namespace

TEST(GlibcC23EntryPoints, EveryC23ConversionImportsItsIsoc23EntryPointAtGlibc238) {
    for (char const* spec : {"x86_64:elf64-x86_64-linux-exec", "arm64:elf64-aarch64-linux-exec"}) {
        SCOPED_TRACE(spec);
        ScratchDir scratch{Location::InsideRepo, "glibc-c23-entry-points"};
        fs::path const src = scratch.path() / "probe.c";
        std::ofstream(src, std::ios::binary) << kProbe;
        DiagnosticReporter rep;
        Program prog;
        prog.setOutputDir(scratch.path());
        int const rc = prog.compileFiles(std::vector<std::string>{src.string()}, "c",
                                         std::vector<std::string>{spec}, rep);
        ASSERT_EQ(rc, 0) << (rep.all().empty() ? std::string{} : rep.all().front().actual);
        std::ifstream in(scratch.path() / "probe", std::ios::binary);
        std::vector<std::uint8_t> const bytes{std::istreambuf_iterator<char>(in),
                                              std::istreambuf_iterator<char>()};
        auto const imports = elfImportVersions(bytes);
        ASSERT_FALSE(imports.empty()) << "the reader recovered no import — every check below would be vacuous";

        std::set<std::string> wantVersioned;
        for (std::string_view const name : kIsoc23Names) {
            std::string const entry = "__isoc23_" + std::string{name};
            wantVersioned.insert(entry);
            auto const it = imports.find(entry);
            ASSERT_NE(it, imports.end()) << entry << " is not imported";
            EXPECT_EQ(it->second, "GLIBC_2.38") << entry << " must require glibc's C23 version";
            EXPECT_EQ(imports.count(std::string{name}), 0u)
                << "the pre-C23 plain `" << name << "` must not be imported";
        }
        std::set<std::string> gotVersioned;
        for (auto const& [name, version] : imports) {
            if (version == "GLIBC_2.38") gotVersioned.insert(name);
        }
        EXPECT_EQ(gotVersioned, wantVersioned) << "exactly the C23 entry points require GLIBC_2.38";
    }
}
