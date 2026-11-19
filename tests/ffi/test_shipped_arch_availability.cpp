// P69 fold 3f (ledger 563 (A)): A SYMBOL ROW AVAILABLE ON ONE ARCH OF A FORMAT.
//
// The stack protector's guard is exported by the C library on Mach-O, by the PROGRAM INTERPRETER on
// aarch64 ELF, and by nothing on x86_64 ELF (MEASURED, P69 fold 3f: `readelf -V` files gcc's aarch64
// image's `__stack_chk_guard@GLIBC_2.17` under ld-linux-aarch64.so.1; x86_64's canary is %fs-relative).
// A format alone cannot say that, so a symbol row's `availableObjectFormats` may hold an ARCH ENTRY,
// `{"format": "elf", "arch": "arm64"}`, with the image there named by its role. These cells pin the
// reader's decode of it on every pair (the entry matched, not matched, undecided), the row that is
// ABSENT where no entry survives, the image, the surface harvest per pair, and every refusal.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] fs::path writeTemp(ScratchDir const& dir, std::string const& name, std::string const& content) {
    fs::path const p = dir.path() / name;
    std::ofstream(p, std::ios::binary) << content;
    return p;
}

[[nodiscard]] bool said(DiagnosticReporter const& rep, std::string_view needle) {
    for (auto const& d : rep.all())
        if (d.actual.find(needle) != std::string::npos) return true;
    return false;
}

[[nodiscard]] std::string allSaid(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + d.actual;
    return s.empty() ? std::string{"\n  <nothing>"} : s;
}

// The descriptor: `g` on Mach-O (every arch) and on ELF on arm64 alone; `h` on ELF on arm64 alone;
// `k` everywhere -- the control.
constexpr char const* kGuardLike = R"JSON({
    "header": "guardlike.h",
    "library": { "elf": "libc.so.6", "macho": "/usr/lib/libSystem.B.dylib" },
    "symbols": [
        { "name": "g", "signature": "u64", "kind": "object", "linkage": "external",
          "availableObjectFormats": [ "macho", { "format": "elf", "arch": "arm64" } ] },
        { "name": "h", "signature": "fn() -> i32", "kind": "function", "linkage": "external",
          "availableObjectFormats": [ { "format": "elf", "arch": "arm64" } ] },
        { "name": "k", "signature": "fn() -> i32", "kind": "function", "linkage": "external" }
    ]
})JSON";

// A row the read keeps is DECLARED on the pair when its availability -- its own list, else the document's,
// empty meaning every format -- admits the pair's format (the semantic tier's gate); a row whose every entry
// was dropped is not kept at all, and a row kept with an EMPTY list would read as every format.
[[nodiscard]] bool declaredOn(ShippedLibDescriptor const& d, ShippedSymbol const& s,
                              std::optional<ObjectFormatKind> fmt) {
    return !fmt.has_value()
        || objectFormatInAvailabilitySet(
               s.availableObjectFormats.empty() ? d.availableObjectFormats : s.availableObjectFormats, *fmt);
}

// The names the full read declares on (arch, format); `nullopt` when the read refused.
[[nodiscard]] std::optional<std::vector<std::string>>
namesOn(fs::path const& path, std::optional<std::string_view> arch, std::optional<ObjectFormatKind> fmt,
        DiagnosticReporter& rep) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep, DataModel::Lp64, arch, fmt);
    if (!desc.has_value()) return std::nullopt;
    std::vector<std::string> out;
    for (auto const& s : desc->symbols)
        if (declaredOn(*desc, s, fmt)) out.push_back(s.name);
    std::sort(out.begin(), out.end());
    return out;
}

[[nodiscard]] bool reads(fs::path const& path, std::string_view arch, ObjectFormatKind fmt,
                         DiagnosticReporter& rep) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep, DataModel::Lp64, arch, fmt);
    return desc.has_value() && !rep.hasErrors();
}

}  // namespace

// Matched, not matched, undecided -- and a row of arch entries alone is ABSENT where none survives,
// never "every format".
TEST(ShippedArchAvailability, AnArchEntryIsAvailableOnTheArchItNamesAlone) {
    ScratchDir dir{Location::Temp, "shipped-arch"};
    auto const path = writeTemp(dir, "guardlike.json", kGuardLike);
    struct Case {
        std::optional<std::string_view> arch;
        ObjectFormatKind                fmt;
        std::vector<std::string>        want;
    };
    std::vector<Case> const cases{
        {"arm64",      ObjectFormatKind::Elf,   {"g", "h", "k"}},
        {"x86_64",     ObjectFormatKind::Elf,   {"k"}},
        {std::nullopt, ObjectFormatKind::Elf,   {"k"}},
        {"arm64",      ObjectFormatKind::MachO, {"g", "k"}},
        {"x86_64",     ObjectFormatKind::MachO, {"g", "k"}},
    };
    for (Case const& c : cases) {
        DiagnosticReporter rep;
        auto const got = namesOn(path, c.arch, c.fmt, rep);
        std::string const pair = std::string{c.arch.value_or("<no arch>")} + " / "
                               + std::string{objectFormatKindName(c.fmt)};
        ASSERT_TRUE(got.has_value()) << pair << allSaid(rep);
        EXPECT_FALSE(rep.hasErrors()) << pair << allSaid(rep);
        EXPECT_EQ(*got, c.want)
            << "on " << pair << " an arch entry must admit its format on its arch alone, and a row "
               "whose every entry is dropped must be absent";
    }
}

// The entry's `image` is the row's import there, named by its role: recorded beside a `library` role
// entry, on the pair it matches only.
TEST(ShippedArchAvailability, AnArchEntrysImageIsTheRowsRoleOnThePairItMatches) {
    ScratchDir dir{Location::Temp, "shipped-arch"};
    auto const path = writeTemp(dir, "imaged.json", R"JSON({
        "header": "imaged.h",
        "library": { "macho": { "role": "cLibrary" } },
        "symbols": [
            { "name": "g", "signature": "u64", "kind": "object", "linkage": "external",
              "availableObjectFormats": [ "macho",
                  { "format": "elf", "arch": "arm64", "image": { "role": "atomicsRuntime" } } ] }
        ]
    })JSON");
    auto roleOn = [&](std::string_view arch, ObjectFormatKind fmt) -> std::optional<RuntimeLibraryRole> {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep, DataModel::Lp64, arch, fmt);
        EXPECT_TRUE(desc.has_value()) << allSaid(rep);
        EXPECT_FALSE(rep.hasErrors()) << allSaid(rep);
        if (!desc.has_value()) return std::nullopt;
        for (auto const& s : desc->symbols) {
            if (s.name != "g" || !declaredOn(*desc, s, fmt)) continue;
            auto const it = s.libraryRoles.find(std::string{objectFormatKindName(fmt)});
            if (it == s.libraryRoles.end()) return RuntimeLibraryRole::None;
            return it->second;
        }
        return std::nullopt;   // the row is not declared on this pair
    };
    EXPECT_EQ(roleOn("arm64", ObjectFormatKind::Elf), RuntimeLibraryRole::AtomicsRuntime)
        << "the matched arch entry's image must be the row's role on elf";
    EXPECT_EQ(roleOn("x86_64", ObjectFormatKind::Elf), std::nullopt)
        << "on x86_64 elf the row must be absent";
    EXPECT_EQ(roleOn("arm64", ObjectFormatKind::MachO), RuntimeLibraryRole::None)
        << "on Mach-O the row inherits the document's library, and names no role of its own";
}

// The surface harvest asks the same question per pair, and its memo is keyed by the pair: the same
// bytes asked on two arches of one format answer twice.
TEST(ShippedArchAvailability, TheSurfaceAndTheValueSeedAnswerPerPair) {
    ScratchDir dir{Location::Temp, "shipped-arch"};
    auto const path = writeTemp(dir, "guardlike_surface.json", kGuardLike);
    ShippedDescriptorReadScope const scope;
    auto has = [](std::optional<std::vector<std::string>> const& names, std::string_view n) {
        return names.has_value() && std::find(names->begin(), names->end(), n) != names->end();
    };
    DiagnosticReporter rep;
    auto const onArm = shippedSurfaceNamesForFormat(path, ObjectFormatKind::Elf, rep, "arm64");
    auto const onX86 = shippedSurfaceNamesForFormat(path, ObjectFormatKind::Elf, rep, "x86_64");
    auto const onNone = shippedSurfaceNamesForFormat(path, ObjectFormatKind::Elf, rep);
    EXPECT_FALSE(rep.hasErrors()) << allSaid(rep);
    EXPECT_TRUE(has(onArm, "g") && has(onArm, "h"))
        << "the surface on arm64 elf must name the rows its arch entries admit";
    EXPECT_FALSE(has(onX86, "g") || has(onX86, "h"))
        << "the surface on x86_64 elf must not name them -- a memo keyed by the format alone answers "
           "with arm64's";
    EXPECT_FALSE(has(onNone, "g") || has(onNone, "h"))
        << "an asker with no arch leaves an arch entry undecided";
    EXPECT_TRUE(has(onX86, "k") && has(onNone, "k"));

    DiagnosticReporter seedRep;
    auto const seedArm = readShippedLibValueNames(path, ObjectFormatKind::Elf, seedRep, "arm64");
    auto const seedX86 = readShippedLibValueNames(path, ObjectFormatKind::Elf, seedRep, "x86_64");
    auto const seedAll = readShippedLibValueNames(path, std::nullopt, seedRep);
    EXPECT_FALSE(seedRep.hasErrors()) << allSaid(seedRep);
    EXPECT_TRUE(has(seedArm, "h"));
    EXPECT_FALSE(has(seedX86, "h"));
    EXPECT_TRUE(has(seedAll, "h")) << "with no format nothing is gated";
}

// Every malformed shape is refused by name, on every pair.
TEST(ShippedArchAvailability, AMalformedArchEntryIsRefusedByName) {
    ScratchDir dir{Location::Temp, "shipped-arch"};
    struct Case {
        char const* stem;
        char const* list;       // the symbol's `availableObjectFormats`
        char const* needle;
    };
    std::vector<Case> const cases{
        {"unknown_key", R"([ { "format": "elf", "arch": "arm64", "version": "x" } ])", "has unknown key 'version'"},
        {"no_arch", R"([ { "format": "elf" } ])", "must name both 'format' and 'arch'"},
        {"no_format", R"([ { "arch": "arm64" } ])", "must name both 'format' and 'arch'"},
        {"image_literal", R"([ { "format": "elf", "arch": "arm64", "image": "libc.so.6" } ])",
         "'image' must be exactly"},
        {"image_unknown_role", R"([ { "format": "elf", "arch": "arm64", "image": { "role": "loader" } } ])",
         "'image' must be exactly"},
        {"string_and_arch", R"([ "elf", { "format": "elf", "arch": "arm64" } ])",
         "both as a string entry (every arch) and in an arch entry"},
        {"two_on_one_pair", R"([ { "format": "elf", "arch": "arm64" }, { "format": "elf", "arch": "arm64" } ])",
         "has two arch entries that can both match one pair"},
        {"unknown_format", R"([ { "format": "coff", "arch": "arm64" } ])", "coff"},
    };
    for (Case const& c : cases) {
        std::string const body = std::string{R"JSON({ "header": "bad.h", "library": { "elf": "libc.so.6" },
            "symbols": [ { "name": "g", "signature": "u64", "kind": "object", "linkage": "external",
                           "availableObjectFormats": )JSON"} + c.list + " } ] }";
        auto const path = writeTemp(dir, std::string{c.stem} + ".json", body);
        for (std::string_view const arch : {std::string_view{"arm64"}, std::string_view{"x86_64"}}) {
            DiagnosticReporter rep;
            EXPECT_FALSE(reads(path, arch, ObjectFormatKind::Elf, rep))
                << c.stem << " on " << arch << " must be refused";
            EXPECT_TRUE(said(rep, c.needle))
                << c.stem << " on " << arch << " must be refused saying: " << c.needle << allSaid(rep);
        }
    }
}

// A document's list and a macro's list name formats only: an arch entry there is refused by name.
TEST(ShippedArchAvailability, AnArchEntryOnADocumentOrAMacroIsRefusedByName) {
    ScratchDir dir{Location::Temp, "shipped-arch"};
    auto const docLevel = writeTemp(dir, "doc_level.json", R"JSON({
        "header": "doclevel.h",
        "availableObjectFormats": [ { "format": "elf", "arch": "arm64" } ],
        "library": { "elf": "libc.so.6" },
        "symbols": [ { "name": "k", "signature": "fn() -> i32", "kind": "function", "linkage": "external" } ]
    })JSON");
    auto const macroLevel = writeTemp(dir, "macro_level.json", R"JSON({
        "header": "macrolevel.h",
        "library": { "elf": "libc.so.6" },
        "macros": [ { "name": "M", "availableObjectFormats": [ { "format": "elf", "arch": "arm64" } ],
                      "variants": [ { "when": { "format": "elf" }, "replacement": "1" } ] } ]
    })JSON");
    for (fs::path const& path : {docLevel, macroLevel}) {
        DiagnosticReporter rep;
        EXPECT_FALSE(reads(path, "arm64", ObjectFormatKind::Elf, rep)) << path.filename().string();
        EXPECT_TRUE(said(rep, "is a SYMBOL row's availability"))
            << path.filename().string() << " must refuse the arch entry by name" << allSaid(rep);
    }
}

// The corpus INDEX -- what `__has_builtin` asks, read without decoding a signature -- answers an arch
// entry through the ONE selector the full read decides it by: on every pair asked, its answer for `h`
// (a function on ELF on arm64 alone) and for the control `k` (everywhere) equals the realization
// oracle's, the binder's own question. Driven through `DSS_CONFIG_ROOT` over a scratch corpus, so it
// runs under ctest.
TEST(ShippedArchAvailability, TheCorpusIndexAnswersAnArchEntryAsTheFullReadDoes) {
    ScratchDir dir{Location::Temp, "shipped-arch-index"};
    fs::path const libs = dir.path() / "src" / "dss-config" / "shippedLibs";
    fs::create_directories(libs);
    {
        std::ofstream out{libs / "guardlike.json", std::ios::binary};
        out << kGuardLike;
        ASSERT_TRUE(out.good()) << "the scratch descriptor did not reach disk";
    }
    ScopedEnv const env{"DSS_CONFIG_ROOT", dir.path().string()};
    struct Ask {
        std::optional<std::string_view> arch;
        ObjectFormatKind                fmt;
        char const*                     name;
        bool                            provided;
    };
    std::vector<Ask> const asks{
        {"arm64", ObjectFormatKind::Elf, "h", true},
        {"x86_64", ObjectFormatKind::Elf, "h", false},
        {std::nullopt, ObjectFormatKind::Elf, "h", false},
        {"arm64", ObjectFormatKind::MachO, "h", false},
        {"x86_64", ObjectFormatKind::Elf, "k", true},
        {std::nullopt, ObjectFormatKind::Elf, "k", true},
    };
    for (Ask const& a : asks) {
        std::string const pair = std::string{objectFormatKindName(a.fmt)} + "/"
                               + std::string{a.arch.value_or("<no arch>")};
        std::optional<bool> const index = shippedLibraryFunctionProvidedOnFormat(a.name, a.fmt, a.arch);
        ASSERT_TRUE(index.has_value()) << "the index did not locate the scratch corpus";
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        std::array<std::string, 1> const names{std::string{a.name}};
        auto const realized =
            realizeShippedExternSymbols(names, interner, typeReg, rep, DataModel::Lp64, a.arch, a.fmt);
        ASSERT_TRUE(realized.has_value()) << "the oracle did not locate the scratch corpus";
        EXPECT_FALSE(rep.hasErrors()) << pair << allSaid(rep);
        auto const it = realized->find(a.name);
        bool const full = it != realized->end() && it->second.isFunction
                       && (it->second.status == ShippedRealizationStatus::Realized
                           || it->second.status == ShippedRealizationStatus::NoLibraryForFormat
                           || it->second.status == ShippedRealizationStatus::ProvidedByShippedSource);
        EXPECT_EQ(full, a.provided) << "the full read of `" << a.name << "` on " << pair;
        EXPECT_EQ(*index, full) << "the index answers " << (*index ? "true" : "false") << " for `" << a.name
                                << "` on " << pair << " where the full read answers "
                                << (full ? "true" : "false");
    }
}
