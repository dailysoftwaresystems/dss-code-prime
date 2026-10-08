// C23's conversions, bound PER PAIR (P69, D-C-C23-CONVERSIONS-MISSING-ON-THE-UCRT-AND-LIBSYSTEM).
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// C23 gave printf `%b`/`%B` and the `wN`/`wfN` lengths, scanf `%b` and a `0b` form of `%i`,
// and the strto family a `0b` subject. glibc implements them (its printf itself, its scanf
// and strto family as the C23 entry points `__isoc23_<name>` @GLIBC_2.38); the UCRT and
// libSystem implement none of them (✔MEASURED: `[%b]` prints `[b]`, `strtol("0b101", 0, 0)`
// answers 0, and the UCRT kills the process on `%n`). So the 22 public rows bind three
// ways, and this file pins every one of them on every executable pair, through the real
// descriptor reader:
//
//   * ELF: the printf family is glibc's plain name; the scanf family, strtol/strtoul/
//     strtoll/strtoull and strtoimax/strtoumax/wcstoimax/wcstoumax are `__isoc23_<name>`
//     @GLIBC_2.38.
//   * pe and Mach-O: every one binds `__dss_isoc23_<name>`, realized from DSS's runtime
//     source (runtime/platform/src/stdio.c, stdlib_strto.c, inttypes.c) — its own source
//     superseding the C-library import the rest of the header inherits (an EMPTY image for
//     the format) — and the platform is reached only through PRIVATE `__dss_platform_*`
//     rows: the five printf/scanf v-primitives (pe: stdio_ucrt.c over the UCRT's
//     `__stdio_common_v*` cores; Mach-O: libSystem's plain exports), the stream lock pair
//     (pe `_lock_file`/`_unlock_file`, Mach-O `flockfile`/`funlockfile`), the five answers
//     only the platform can give (stdio_ucrt.c / stdio_libsystem.c), and the eight strto
//     functions under their plain names. None of them exists on ELF.
//   * pe's old `strtoll` -> `_strtoi64` MACRO is gone (it answered msvcrt.dll, which predates
//     C99; ucrtbase.dll exports strtoll), and every realization names a unit that exists.
//
// What each binding DOES is witnessed by examples/c/c23_formatted_io on all five pairs; the
// images' import tables by the <stdio.h>, <stdlib.h> and <inttypes.h> surface tests.
//
// RED-ON-DISABLE: drop a pe or macho `realization` arm from any of the 22 rows, or its
// `__dss_isoc23_` linkName arm, and this file names the row on that pair; restore pe's
// `strtoll` macro and it fails on pe.

#include "analysis/compilation_unit/compilation_unit.hpp"   // predefinedTypeFactsFor — the pair's own facts
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/named_type_binding.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "link/object_format_schema.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::ffi;
namespace fs = std::filesystem;

namespace {

struct Family {
    char const*                    header;   // the descriptor file
    char const*                    unit;     // the pe/macho realization
    std::vector<std::string_view>  plainOnElf;
    std::vector<std::string_view>  isoc23OnElf;
};

[[nodiscard]] std::vector<Family> const& families() {
    static std::vector<Family> const all{
        {"stdio.json", "runtime/platform/src/stdio.c",
         {"printf", "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf", "vsprintf", "vsnprintf"},
         {"scanf", "fscanf", "sscanf", "vscanf", "vfscanf", "vsscanf"}},
        {"stdlib.json", "runtime/platform/src/stdlib_strto.c", {}, {"strtol", "strtoul", "strtoll", "strtoull"}},
        {"inttypes.json", "runtime/platform/src/inttypes.c", {},
         {"strtoimax", "strtoumax", "wcstoimax", "wcstoumax"}},
    };
    return all;
}

// One private row: its descriptor, its C name, and how pe and Mach-O realize it — a unit
// (`peUnit`/`machoUnit`) or an import under a link name (`peLink`/`machoLink`, "" = its own).
struct Private {
    char const* header;
    char const* name;
    char const* peUnit;
    char const* peLink;
    char const* machoUnit;
    char const* machoLink;
};

constexpr char const* kUcrt      = "runtime/platform/src/stdio_ucrt.c";
constexpr char const* kLibSystem = "runtime/platform/src/stdio_libsystem.c";

constexpr Private kPrivate[] = {
    {"stdio.json", "__dss_platform_vfprintf", kUcrt, "", "", "vfprintf"},
    {"stdio.json", "__dss_platform_vsprintf", kUcrt, "", "", "vsprintf"},
    {"stdio.json", "__dss_platform_vsnprintf", kUcrt, "", "", "vsnprintf"},
    {"stdio.json", "__dss_platform_vfscanf", kUcrt, "", "", "vfscanf"},
    {"stdio.json", "__dss_platform_vsscanf", kUcrt, "", "", "vsscanf"},
    {"stdio.json", "__dss_platform_lock_file", "", "_lock_file", "", "flockfile"},
    {"stdio.json", "__dss_platform_unlock_file", "", "_unlock_file", "", "funlockfile"},
    {"stdio.json", "__dss_platform_printf_performs_n", kUcrt, "", kLibSystem, ""},
    {"stdio.json", "__dss_platform_printf_positional", kUcrt, "", kLibSystem, ""},
    {"stdio.json", "__dss_platform_prefix_without_digits_is_zero", kUcrt, "", kLibSystem, ""},
    {"stdio.json", "__dss_platform_length_extension", kUcrt, "", kLibSystem, ""},
    {"stdio.json", "__dss_platform_conversion_extension", kUcrt, "", kLibSystem, ""},
    {"stdlib.json", "__dss_platform_strtol", "", "strtol", "", "strtol"},
    {"stdlib.json", "__dss_platform_strtoul", "", "strtoul", "", "strtoul"},
    {"stdlib.json", "__dss_platform_strtoll", "", "strtoll", "", "strtoll"},
    {"stdlib.json", "__dss_platform_strtoull", "", "strtoull", "", "strtoull"},
    {"inttypes.json", "__dss_platform_strtoimax", "", "strtoimax", "", "strtoimax"},
    {"inttypes.json", "__dss_platform_strtoumax", "", "strtoumax", "", "strtoumax"},
    {"inttypes.json", "__dss_platform_wcstoimax", "", "wcstoimax", "", "wcstoimax"},
    {"inttypes.json", "__dss_platform_wcstoumax", "", "wcstoumax", "", "wcstoumax"},
};

struct Pair {
    char const* spec;
    char const* arch;
    char const* formatDoc;
};

constexpr Pair kPairs[] = {
    {"x86_64:pe64-x86_64-windows-exec", "x86_64", "pe64-x86_64-windows-exec"},
    {"x86_64:elf64-x86_64-linux-exec", "x86_64", "elf64-x86_64-linux-exec"},
    {"arm64:elf64-aarch64-linux-exec", "arm64", "elf64-aarch64-linux-exec"},
    {"arm64:macho64-arm64-darwin-exec", "arm64", "macho64-arm64-darwin-exec"},
    {"x86_64:macho64-x86_64-darwin-exec", "x86_64", "macho64-x86_64-darwin-exec"},
};

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

// Every descriptor this file reads, decoded for one pair through the real reader.
struct Decoded {
    std::string                          fmt;
    ObjectFormatKind                     kind{};
    std::vector<std::string>             headers;
    std::vector<ShippedLibDescriptor>    descs;
};

[[nodiscard]] Decoded decodeFor(Pair const& p, fs::path const& shippedLibs, TypeInterner& interner,
                                TypeRegistry& typeReg) {
    Decoded out;
    auto targetR = TargetSchema::loadShipped(p.arch);
    auto formatR = ObjectFormatSchema::loadShipped(p.formatDoc);
    if (!targetR.has_value() || !formatR.has_value()) {
        ADD_FAILURE() << "the pair's target or format document does not load: " << p.spec;
        return out;
    }
    out.kind = (*formatR)->kind();
    out.fmt  = std::string{objectFormatKindName(out.kind)};
    PredefinedTypeFacts const typeFacts = predefinedTypeFactsFor(**targetR, **formatR);
    ShippedPairFacts const facts{cLanguage().get(), typeFacts.dataModel, typeFacts.charIsUnsigned,
                                 typeFacts.abiTypedefs, typeFacts.longDoubleFormat};
    // The v* rows spell `va_list`; its identity is irrelevant here (no signature is compared).
    std::array<NamedTypeBinding, 1> const vaList{
        NamedTypeBinding{"va_list", interner.pointer(interner.primitive(TypeKind::Void))}};
    for (char const* header : {"stdio.json", "stdlib.json", "inttypes.json"}) {
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(shippedLibs / header, interner, typeReg, rep,
                                             (*formatR)->dataModel(), std::string_view{p.arch}, out.kind,
                                             vaList, nullptr, &facts);
        if (!desc.has_value() || rep.hasErrors()) {
            ADD_FAILURE() << header << " does not load for " << p.spec << ": "
                          << (rep.all().empty() ? std::string{} : rep.all().front().actual);
            continue;
        }
        out.headers.emplace_back(header);
        out.descs.push_back(std::move(*desc));
    }
    return out;
}

[[nodiscard]] ShippedLibDescriptor const* descOf(Decoded const& d, std::string_view header) {
    for (std::size_t i = 0; i < d.headers.size(); ++i)
        if (d.headers[i] == header) return &d.descs[i];
    return nullptr;
}

// The ONE row of `name` the pair can see (a second is a declaration-order hazard).
[[nodiscard]] ShippedSymbol const* rowOf(ShippedLibDescriptor const& desc, std::string_view name,
                                         std::string_view fmt) {
    ShippedSymbol const* found = nullptr;
    for (auto const& s : desc.symbols) {
        if (s.name != name || !availableOn(s, fmt)) continue;
        EXPECT_EQ(found, nullptr) << name << " has two rows on " << fmt;
        found = &s;
    }
    return found;
}

}  // namespace

// ── THE 22 PUBLIC ROWS ───────────────────────────────────────────────────────
TEST(C23ConversionBindings, EveryPublicRowBindsItsPairsC23EntryPoint) {
    ASSERT_NE(cLanguage(), nullptr);
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    for (Pair const& p : kPairs) {
        SCOPED_TRACE(p.spec);
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        Decoded const d = decodeFor(p, *cfg / "shippedLibs", interner, typeReg);
        ASSERT_EQ(d.descs.size(), 3u);
        std::size_t seen = 0;
        for (Family const& f : families()) {
            ShippedLibDescriptor const* desc = descOf(d, f.header);
            ASSERT_NE(desc, nullptr) << f.header;
            std::vector<std::string_view> names = f.plainOnElf;
            names.insert(names.end(), f.isoc23OnElf.begin(), f.isoc23OnElf.end());
            for (std::string_view const name : names) {
                SCOPED_TRACE(std::string{name});
                ++seen;
                ShippedSymbol const* s = rowOf(*desc, name, d.fmt);
                ASSERT_NE(s, nullptr) << name << " is not declared on " << d.fmt;
                EXPECT_TRUE(s->synthesize.empty()) << "no compiler-synthesized body remains";
                bool const plain = std::find(f.plainOnElf.begin(), f.plainOnElf.end(), name) != f.plainOnElf.end();
                if (d.kind == ObjectFormatKind::Elf) {
                    // The realization map is per format and carries the pe/macho arms on
                    // every read; what matters is that it names no source for THIS format.
                    EXPECT_FALSE(s->realization.contains(d.fmt)) << "glibc implements it";
                    if (plain) {
                        EXPECT_TRUE(s->linkName.empty()) << "glibc's printf renders C23 itself";
                        EXPECT_TRUE(s->version.empty());
                    } else {
                        EXPECT_EQ(s->linkName, "__isoc23_" + std::string{name});
                        EXPECT_EQ(s->version, "GLIBC_2.38");
                    }
                    continue;
                }
                EXPECT_EQ(s->linkName, "__dss_isoc23_" + std::string{name})
                    << "the platform's plain name is never redefined";
                EXPECT_TRUE(s->version.empty());
                auto const r = s->realization.find(d.fmt);
                ASSERT_NE(r, s->realization.end()) << "no DSS realization on " << d.fmt;
                EXPECT_EQ(r->second, f.unit);
                EXPECT_TRUE(fs::exists(*cfg / r->second)) << r->second << " does not exist";
                ASSERT_TRUE(s->library.contains(d.fmt))
                    << "the row's own source must supersede the inherited C-library import";
                EXPECT_TRUE(s->library.at(d.fmt).empty());
            }
        }
        EXPECT_EQ(seen, 22u) << "the 14 printf/scanf rows, 4 <stdlib.h> and 4 <inttypes.h> conversions";

        // pe's msvcrt-era strtoll -> _strtoi64 macro is gone on every pair.
        ShippedLibDescriptor const* stdlib = descOf(d, "stdlib.json");
        ASSERT_NE(stdlib, nullptr);
        for (auto const& m : stdlib->macros)
            EXPECT_NE(m.name, "strtoll") << "strtoll is a function (ucrtbase exports it), never a macro";
    }
}

// ── THE PRIVATE ROWS: the platform, reached only under the implementation's names ─
TEST(C23ConversionBindings, ThePlatformIsReachedOnlyThroughPrivateRows) {
    ASSERT_NE(cLanguage(), nullptr);
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    for (Pair const& p : kPairs) {
        SCOPED_TRACE(p.spec);
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        Decoded const d = decodeFor(p, *cfg / "shippedLibs", interner, typeReg);
        ASSERT_EQ(d.descs.size(), 3u);
        for (Private const& row : kPrivate) {
            SCOPED_TRACE(row.name);
            ShippedLibDescriptor const* desc = descOf(d, row.header);
            ASSERT_NE(desc, nullptr) << row.header;
            ShippedSymbol const* s = rowOf(*desc, row.name, d.fmt);
            if (d.kind == ObjectFormatKind::Elf) {
                EXPECT_EQ(s, nullptr) << "glibc needs no private row";
                continue;
            }
            ASSERT_NE(s, nullptr) << row.name << " is not declared on " << d.fmt;
            bool const pe       = d.kind == ObjectFormatKind::Pe;
            std::string const unit = pe ? row.peUnit : row.machoUnit;
            std::string const link = pe ? row.peLink : row.machoLink;
            EXPECT_EQ(s->linkName, link);
            if (unit.empty()) {
                EXPECT_FALSE(s->realization.contains(d.fmt)) << "an import of the platform's own export";
            } else {
                auto const r = s->realization.find(d.fmt);
                ASSERT_NE(r, s->realization.end());
                EXPECT_EQ(r->second, unit);
                EXPECT_TRUE(fs::exists(*cfg / r->second)) << r->second << " does not exist";
            }
        }
    }
}
