// P69 fold 3f (ledger 549 (b); main's ruling on `__iob_func`): THE STATED ROUTES OF WRITTEN NAMES.
//
// A reference WRITTEN with a name -- a foreign object's, a `.s` unit's -- binds by what the rows
// derive, except where the platform's LINK decides what no row can say; that is STATED, in one closed
// vocabulary (`StatedWrittenNameRoute`) decoded by one decoder for both places it is stated: a row's
// `writtenNameRoute` and a descriptor's `writtenNames`.
//
// THE UNIVERSE (MEASURED P69 fold 3f, windows-x86_64): oldnames.lib (toolset 14.52.37025, `dumpbin
// /linkermember`) defines 18 aliases whose `_<name>` ucrtbase.dll exports (`dumpbin /exports`: no plain
// name) and the corpus declares on pe; link.exe binds a written plain name through them, and mingw-w64's
// ld through its import library's alias members (`--trace-symbol`). Seventeen state `linkerAlias`; the
// eighteenth, `access`, is a BODY (io_access.c, MinGW's meaning of X_OK). `__iob_func` -- the msvcrt
// runtime's stream accessor -- is exported by no UCRT image and mingw-w64's UCRT link leaves it
// undefined (MEASURED): it is a written name no row declares, refused by name on pe.
//
// These cells pin the data (the seventeen, `access`, `__iob_func`), the decoder's refusals at both
// places, that a written name is READ and never DECLARED (the surface harvests, the value seed and the
// corpus index do not see it), and that no written name is stated in two places across the corpus.
// The binders' half (the alias's fallback, the by-name refusal) lands with the binder.

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/named_type_binding.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "fixture_role_resolver.hpp"   // the ONE test-side runtimeLibraries stand-in
#include "repo_root.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr std::array<std::string_view, 17> kAliased{
    "write", "lseek", "isatty", "unlink", "rmdir", "chdir", "getcwd", "open", "close",
    "read",  "dup",   "dup2",   "getpid", "strdup", "memccpy", "chmod", "mkdir",
};

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

// The oracle over the shipped corpus on one pair, as the driver asks it: a resolver over the format's
// runtime-library table (the rows name their C library by ROLE) and a `va_list` binding (stdio.json's
// signatures spell it; any consistent stand-in works, nothing here reads the id).
[[nodiscard]] std::optional<std::unordered_map<std::string, ShippedSymbolRealization>>
realizeOn(std::vector<std::string> const& names, DataModel dm, ObjectFormatKind fmt, DiagnosticReporter& rep) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    std::array<NamedTypeBinding, 1> const named{
        NamedTypeBinding{kVaListDescriptorTypeName, interner.pointer(interner.primitive(TypeKind::Void))}};
    dss::ffi_test::FixtureRoleResolver const roles{fmt, "x86_64"};
    return realizeShippedExternSymbols(names, interner, typeReg, rep, dm, std::optional<std::string_view>{"x86_64"},
                                       fmt, named, &roles);
}

[[nodiscard]] std::optional<ShippedLibDescriptor> readOnPe(fs::path const& path, DiagnosticReporter& rep) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    return readShippedLibDescriptor(path, interner, typeReg, rep, DataModel::Llp64, "x86_64",
                                    ObjectFormatKind::Pe);
}

[[nodiscard]] json readJson(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return json::parse(in);
}

// The shipped corpus the JSON sweeps here read, installed as the one the oracle and the corpus index
// read (`DSS_CONFIG_ROOT` names the directory CONTAINING src/dss-config), so the two cannot answer
// out of different trees.
[[nodiscard]] std::string shippedRoot() {
    return dss::test::configRoot().parent_path().parent_path().string();
}

[[nodiscard]] std::vector<fs::path> shippedDescriptors() {
    std::vector<fs::path> out;
    for (auto const& e : fs::recursive_directory_iterator{dss::test::configRoot() / "shippedLibs"})
        if (e.is_regular_file() && e.path().extension() == ".json") out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

// The seventeen state the alias on pe and carry it to the realization the binders read; `access` is a
// body with no stated route; on elf no row states one. And the corpus states it on these seventeen rows
// and no other (read off every shipped descriptor, never retyped from this list).
TEST(WrittenNameRoutes, TheSeventeenRenamedPosixNamesStateTheLinkerAliasOnPeAlone) {
    ScopedEnv const env{"DSS_CONFIG_ROOT", shippedRoot()};
    std::vector<std::string> names;
    std::set<std::string>    aliased;
    for (std::string_view const n : kAliased) {
        names.emplace_back(n);
        aliased.emplace(n);
    }
    names.emplace_back("access");
    {
        DiagnosticReporter rep;
        auto const pe = realizeOn(names, DataModel::Llp64, ObjectFormatKind::Pe, rep);
        ASSERT_TRUE(pe.has_value()) << "the shipped corpus was not located";
        EXPECT_FALSE(rep.hasErrors()) << allSaid(rep);
        for (std::string_view const n : kAliased) {
            auto const it = pe->find(std::string{n});
            ASSERT_NE(it, pe->end()) << n << " is not declared on pe";
            EXPECT_EQ(it->second.status, ShippedRealizationStatus::Realized) << n;
            EXPECT_EQ(it->second.linkName, "_" + std::string{n}) << n;
            ASSERT_TRUE(it->second.statedRoute.has_value()) << n << " states no route on pe";
            EXPECT_EQ(it->second.statedRoute->route, WrittenNameRoute::LinkerAlias) << n;
        }
        auto const access = pe->find("access");
        ASSERT_NE(access, pe->end());
        EXPECT_EQ(access->second.status, ShippedRealizationStatus::ProvidedByShippedSource)
            << "`access` on pe is DSS's body";
        EXPECT_FALSE(access->second.statedRoute.has_value()) << "a body states no route";
    }
    {
        DiagnosticReporter rep;
        auto const elf = realizeOn(names, DataModel::Lp64, ObjectFormatKind::Elf, rep);
        ASSERT_TRUE(elf.has_value());
        for (auto const& [n, real] : *elf)
            EXPECT_FALSE(real.statedRoute.has_value()) << n << " states a route on elf";
    }
    std::set<std::string> stated;
    for (fs::path const& p : shippedDescriptors()) {
        json const doc = readJson(p);
        if (!doc.contains("symbols")) continue;
        for (json const& s : doc.at("symbols")) {
            if (!s.is_object() || !s.contains("writtenNameRoute")) continue;
            stated.insert(s.at("name").get<std::string>());
            EXPECT_EQ(s.at("writtenNameRoute"), json({{"pe", "linkerAlias"}}))
                << s.at("name").get<std::string>() << " in " << p.filename().string();
        }
    }
    EXPECT_EQ(stated, aliased)
        << "the rows stating a route are not exactly the measured seventeen";
}

// Every place the rows derive a route, or the vocabulary has no word, is refused by name.
TEST(WrittenNameRoutes, AStatedRouteIsRefusedWhereTheRowsDeriveOne) {
    ScratchDir dir{Location::Temp, "written-name-routes"};
    // One row `dupe` in a document whose `library` is `docLibrary` (an image on pe unless a case says
    // otherwise), with `rowExtra`'s keys.
    auto body = [](std::string_view docLibrary, std::string_view rowExtra) {
        return std::string{R"({ "header": "w.h", "library": )"} + std::string{docLibrary}
             + R"(, "symbols": [ { "name": "dupe", "signature": "fn(i32) -> i32", "kind": "function", )"
             + R"("linkage": "external", )" + std::string{rowExtra} + " } ] }";
    };
    std::string_view const onPe = R"({ "pe": "alpha.dll" })";
    {   // the two words, accepted
        DiagnosticReporter rep;
        auto const desc = readOnPe(
            writeTemp(dir, "alias.json", body(onPe, R"("linkName": "_dupe", "writtenNameRoute": { "pe": "linkerAlias" })")),
            rep);
        ASSERT_TRUE(desc.has_value() && !rep.hasErrors()) << allSaid(rep);
        ASSERT_EQ(desc->symbols.size(), 1u);
        auto const it = desc->symbols[0].writtenNameRoutes.find("pe");
        ASSERT_NE(it, desc->symbols[0].writtenNameRoutes.end());
        EXPECT_EQ(it->second.route, WrittenNameRoute::LinkerAlias);
    }
    {
        DiagnosticReporter rep;
        auto const desc = readOnPe(
            writeTemp(dir, "refused.json",
                      body(onPe, R"("linkName": "_dupe", "writtenNameRoute": { "pe": { "refused": "why" } })")),
            rep);
        ASSERT_TRUE(desc.has_value() && !rep.hasErrors()) << allSaid(rep);
        auto const it = desc->symbols[0].writtenNameRoutes.find("pe");
        ASSERT_NE(it, desc->symbols[0].writtenNameRoutes.end());
        EXPECT_EQ(it->second.route, WrittenNameRoute::Refused);
        EXPECT_EQ(it->second.reason, "why");
    }
    struct Case {
        char const* stem;
        std::string doc;
        char const* needle;
    };
    std::vector<Case> const cases{
        {"not_an_object", body(onPe, R"("linkName": "_dupe", "writtenNameRoute": "linkerAlias")"),
         "must be a non-empty object"},
        {"unknown_word", body(onPe, R"("linkName": "_dupe", "writtenNameRoute": { "pe": "direct" })"),
         "states an unknown route on 'pe'"},
        {"empty_reason", body(onPe, R"("linkName": "_dupe", "writtenNameRoute": { "pe": { "refused": "" } })"),
         "states an unknown route on 'pe'"},
        {"unknown_format", body(onPe, R"("linkName": "_dupe", "writtenNameRoute": { "coff": "linkerAlias" })"),
         "names the unknown object format 'coff'"},
        {"not_here", body(onPe, R"("availableObjectFormats": ["pe"], "linkName": "_dupe", "writtenNameRoute": { "elf": "linkerAlias" })"),
         "where the row does not exist"},
        {"own_export", body(onPe, R"("writtenNameRoute": { "pe": "linkerAlias" })"),
         "where its link name is its C name"},
        {"no_image", body(R"({ "elf": "libc.so.6" })", R"("linkName": "_dupe", "writtenNameRoute": { "pe": "linkerAlias" })"),
         "falls back to the row's EXPORT there"},
    };
    for (Case const& c : cases) {
        DiagnosticReporter rep;
        auto const desc = readOnPe(writeTemp(dir, std::string{c.stem} + ".json", c.doc), rep);
        EXPECT_TRUE(!desc.has_value() || rep.hasErrors()) << c.stem << " must be refused";
        EXPECT_TRUE(said(rep, c.needle)) << c.stem << " must be refused saying: " << c.needle << allSaid(rep);
    }
}

// An alias row is the row under ANOTHER name, and that name is its own export (the reader clears the
// link name), so the row's stated route is not the alias's: the rows derive the alias's route.
TEST(WrittenNameRoutes, AnAliasRowIsItsOwnExportAndStatesNoRoute) {
    ScratchDir dir{Location::Temp, "written-name-routes"};
    DiagnosticReporter rep;
    auto const desc = readOnPe(writeTemp(dir, "aliased.json", R"({ "header": "a.h", "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "x", "signature": "i32", "kind": "object", "linkName": "_x",
                       "writtenNameRoute": { "pe": "linkerAlias" }, "aliases": { "pe": ["y"] } } ] })"),
                               rep);
    ASSERT_TRUE(desc.has_value() && !rep.hasErrors()) << allSaid(rep);
    ASSERT_EQ(desc->symbols.size(), 2u);
    EXPECT_EQ(desc->symbols[0].name, "x");
    EXPECT_TRUE(desc->symbols[0].writtenNameRoutes.contains("pe")) << "the canonical row lost its route";
    EXPECT_EQ(desc->symbols[1].name, "y");
    EXPECT_EQ(desc->symbols[1].aliasOf, "x");
    EXPECT_TRUE(desc->symbols[1].linkName.empty());
    EXPECT_TRUE(desc->symbols[1].writtenNameRoutes.empty()) << "the alias row carries the row's stated route";
}

// `__iob_func` is READ -- its route on pe is refused, with the reason -- and DECLARED nowhere: not a
// row of the full read, not in the surface harvest, the value seed or the corpus index, so DSS's
// <stdio.h> declares it on no pair, as UCRT's own <stdio.h> does not.
TEST(WrittenNameRoutes, AWrittenNameIsReadAndNeverDeclared) {
    ScopedEnv const env{"DSS_CONFIG_ROOT", shippedRoot()};
    auto const onPe = shippedWrittenNameRouteOn("__iob_func", ObjectFormatKind::Pe);
    ASSERT_TRUE(onPe.has_value()) << "no route is stated for `__iob_func` on pe";
    EXPECT_EQ(onPe->route, WrittenNameRoute::Refused);
    EXPECT_NE(onPe->reason.find("UCRT"), std::string::npos) << onPe->reason;
    EXPECT_FALSE(shippedWrittenNameRouteOn("__iob_func", ObjectFormatKind::Elf).has_value());

    fs::path const stdio = dss::test::configRoot() / "shippedLibs" / "stdio.json";
    for (ObjectFormatKind const fmt : {ObjectFormatKind::Pe, ObjectFormatKind::Elf, ObjectFormatKind::MachO}) {
        std::string const f{objectFormatKindName(fmt)};
        DiagnosticReporter rep;
        auto const surface = shippedSurfaceNamesForFormat(stdio, fmt, rep, "x86_64");
        ASSERT_TRUE(surface.has_value()) << f << allSaid(rep);
        EXPECT_EQ(std::count(surface->begin(), surface->end(), "__iob_func"), 0) << "the surface on " << f;
        EXPECT_GE(std::count(surface->begin(), surface->end(), "printf"), 1) << "the control on " << f;
        auto const seed = readShippedLibValueNames(stdio, fmt, rep, "x86_64");
        ASSERT_TRUE(seed.has_value()) << f << allSaid(rep);
        EXPECT_EQ(std::count(seed->begin(), seed->end(), "__iob_func"), 0) << "the value seed on " << f;
        EXPECT_EQ(shippedLibraryFunctionProvidedOnFormat("__iob_func", fmt, "x86_64"), std::optional<bool>{false})
            << "the corpus index on " << f;
    }
    auto const formats = collectShippedExternSymbolFormats();
    ASSERT_TRUE(formats.has_value());
    EXPECT_FALSE(formats->contains("__iob_func")) << "the availability view names the written name";

    // The full read: the list is carried, and no row of the name is.
    ScratchDir dir{Location::Temp, "written-names"};
    DiagnosticReporter rep;
    auto const desc = readOnPe(writeTemp(dir, "listed.json", R"({ "header": "l.h", "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "k", "signature": "fn() -> i32" } ],
        "writtenNames": [ { "name": "__old", "route": { "pe": { "refused": "the old runtime's" } } } ] })"),
                               rep);
    ASSERT_TRUE(desc.has_value() && !rep.hasErrors()) << allSaid(rep);
    ASSERT_EQ(desc->writtenNames.size(), 1u);
    EXPECT_EQ(desc->writtenNames[0].name, "__old");
    for (auto const& s : desc->symbols) EXPECT_NE(s.name, "__old") << "a written name was declared as a row";
}

// The list's own refusals: one written name stated in two places, twice, as an alias with no row, or
// in another shape.
TEST(WrittenNameRoutes, AWrittenNameStatedTwiceIsRefused) {
    ScratchDir dir{Location::Temp, "written-names"};
    auto doc = [](std::string_view list) {
        return std::string{R"({ "header": "l.h", "library": { "pe": "alpha.dll" },
            "symbols": [ { "name": "k", "signature": "fn() -> i32", "linkName": "_k",
                           "writtenNameRoute": { "pe": "linkerAlias" } } ],
            "writtenNames": )"} + std::string{list} + " }";
    };
    struct Case {
        char const* stem;
        std::string text;
        char const* needle;
    };
    std::vector<Case> const cases{
        {"a_row", doc(R"([ { "name": "k", "route": { "pe": { "refused": "x" } } } ])"),
         "is a row's C name in this descriptor"},
        {"twice", doc(R"([ { "name": "w", "route": { "pe": { "refused": "x" } } }, { "name": "w", "route": { "pe": { "refused": "y" } } } ])"),
         "names 'w' twice"},
        {"alias", doc(R"([ { "name": "w", "route": { "pe": "linkerAlias" } } ])"), "falls back to a ROW's export"},
        {"no_route", doc(R"([ { "name": "w" } ])"), "must be {\"name\""},
        {"not_a_list", doc(R"({ "name": "w" })"), "must be an array"},
    };
    for (Case const& c : cases) {
        DiagnosticReporter rep;
        auto const desc = readOnPe(writeTemp(dir, std::string{c.stem} + ".json", c.text), rep);
        EXPECT_TRUE(!desc.has_value() || rep.hasErrors()) << c.stem << " must be refused";
        EXPECT_TRUE(said(rep, c.needle)) << c.stem << " must be refused saying: " << c.needle << allSaid(rep);
    }
}

// Across the shipped corpus no written name is a row's C name or link name in ANY descriptor (one
// written name, one place), and at least the one MEASURED written name is stated.
TEST(WrittenNameRoutes, NoWrittenNameIsStatedInTwoPlacesAcrossTheCorpus) {
    std::set<std::string> rowNames;
    std::vector<std::pair<std::string, std::string>> written;   // name, document
    for (fs::path const& p : shippedDescriptors()) {
        json const doc = readJson(p);
        if (doc.contains("symbols")) {
            for (json const& s : doc.at("symbols")) {
                if (!s.is_object() || !s.contains("name")) continue;
                rowNames.insert(s.at("name").get<std::string>());
                if (!s.contains("linkName")) continue;
                json const& ln = s.at("linkName");
                if (ln.is_string()) rowNames.insert(ln.get<std::string>());
                else if (ln.is_object() && ln.contains("variants"))
                    for (json const& arm : ln.at("variants"))
                        if (arm.contains("value") && arm.at("value").is_string())
                            rowNames.insert(arm.at("value").get<std::string>());
            }
        }
        if (doc.contains("writtenNames"))
            for (json const& w : doc.at("writtenNames"))
                written.emplace_back(w.at("name").get<std::string>(), p.filename().string());
    }
    ASSERT_GE(written.size(), 1u) << "no written name is stated: `__iob_func`'s refusal is gone";
    std::set<std::string> seen;
    for (auto const& [name, document] : written) {
        EXPECT_FALSE(rowNames.contains(name)) << name << " (" << document << ") is also a row's C or link name";
        EXPECT_TRUE(seen.insert(name).second) << name << " is stated by two descriptors";
    }
}
