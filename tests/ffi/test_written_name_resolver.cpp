// P69 fold 3f (ledger 549 (b)): THE ROUTE OF A WRITTEN NAME -- `writtenNameRoutesOn`, the ONE
// function every binder is to ask what a reference written with a name binds to on a pair:
//   1. the row whose C name is W and whose link name here is W: `Direct` (an image) or `Body`
//      (DSS's shipped source);
//   2. else the ONE row whose link name here is W (its C name another);
//   3. else W's C-name row's stated route (`LinkerAlias`, `Refused`), else a descriptor's written
//      name (`Refused`);
//   4. else `Header` (W's renaming row, when it has one, rides along).
// Two rows of other C names naming one link name, and a stated route where step 2 also answers,
// are refused on the reporter (F_ShippedCorpusInvariantBroken) and answered `Header`.
//
// The cells: each step and each refusal over a scratch corpus; the shipped corpus's own answers
// on pe (the seventeen aliases, `access`'s body, `__iob_func`'s refusal, `time`'s header), and
// `strtol` on every real pair; and a sweep of every C name and link name the corpus states, on
// every real pair, that is refused nowhere -- the reverse index's AMBIGUOUS rule, now a refusal,
// MEASURED zero (it must stay zero for the binders to adopt the resolver without a changed answer).

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/named_type_binding.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "diagnostic_count.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "fixture_role_resolver.hpp"   // the ONE test-side runtimeLibraries stand-in
#include "repo_root.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"
#include "shipped_read_pairs.hpp"      // the real pairs a REAL descriptor is read on

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::countCode;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

using Answers = std::unordered_map<std::string, WrittenNameRouteAnswer>;

[[nodiscard]] std::string allSaid(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + d.actual;
    return s.empty() ? std::string{"\n  <nothing>"} : s;
}

[[nodiscard]] char const* kindName(WrittenNameRouteKind k) {
    switch (k) {
        case WrittenNameRouteKind::Direct:      return "Direct";
        case WrittenNameRouteKind::Body:        return "Body";
        case WrittenNameRouteKind::LinkerAlias: return "LinkerAlias";
        case WrittenNameRouteKind::Refused:     return "Refused";
        case WrittenNameRouteKind::Header:      return "Header";
    }
    return "?";
}

// The resolver on one pair, as a binder asks it: the fixture's role table for the pair's family (its
// format's kind on its arch), the pair's facts when given, and a `va_list` binding (stdio.json's
// signatures spell it).
[[nodiscard]] std::optional<Answers> routesOn(std::vector<std::string> const& names, DataModel dm,
                                              std::string_view arch, ObjectFormatKind fmt,
                                              DiagnosticReporter& rep,
                                              ShippedPairFacts const* facts = nullptr) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    std::array<NamedTypeBinding, 1> const named{
        NamedTypeBinding{kVaListDescriptorTypeName, interner.pointer(interner.primitive(TypeKind::Void))}};
    dss::ffi_test::FixtureRoleResolver const roles{fmt, arch};
    return writtenNameRoutesOn(names, interner, typeReg, rep, dm, std::optional<std::string_view>{arch}, fmt,
                               named, &roles, facts);
}

// One answer, checked: its kind, and the C name of the row behind it (empty: none).
void expectRoute(Answers const& a, std::string const& w, WrittenNameRouteKind kind, std::string const& row) {
    auto const it = a.find(w);
    ASSERT_NE(it, a.end()) << "`" << w << "` was not answered";
    EXPECT_STREQ(kindName(it->second.kind), kindName(kind)) << "`" << w << "`'s route";
    EXPECT_EQ(it->second.canonicalName, row) << "`" << w << "`'s row";
}

void write(fs::path const& p, std::string const& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A scratch corpus, repo-shaped (`DSS_CONFIG_ROOT` names the directory CONTAINING src/dss-config):
//   a.json -- `d` (an image's), `b` (DSS's body), `c` -> `_c` with the linker alias, `_c`, `t` -> `_t64`
//             with no route (the header renames it), `_t64`, `s` -> `__iso_s` and `__plat_s` -> `s`;
//   w.json -- a row and the written name `__old`, refused on pe.
[[nodiscard]] fs::path stepsCorpus(ScratchDir const& dir) {
    fs::path const root = dir.path() / "steps";
    fs::path const libs = root / "src" / "dss-config" / "shippedLibs";
    write(root / "src" / "dss-config" / "runtime" / "platform" / "src" / "b.c", "int b(void) { return 0; }\n");
    write(libs / "a.json", R"({ "header": "a.h", "library": { "pe": "alpha.dll" }, "symbols": [
        { "name": "d", "signature": "fn() -> i32" },
        { "name": "b", "signature": "fn() -> i32", "realization": { "pe": { "source": "runtime/platform/src/b.c" } } },
        { "name": "c", "signature": "fn() -> i32", "linkName": "_c", "writtenNameRoute": { "pe": "linkerAlias" } },
        { "name": "_c", "signature": "fn() -> i32" },
        { "name": "t", "signature": "fn() -> i32", "linkName": "_t64" },
        { "name": "_t64", "signature": "fn() -> i32" },
        { "name": "s", "signature": "fn() -> i32", "linkName": "__iso_s" },
        { "name": "__plat_s", "signature": "fn() -> i32", "linkName": "s" } ] })");
    write(libs / "w.json", R"({ "header": "w.h", "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "k", "signature": "fn() -> i32" } ],
        "writtenNames": [ { "name": "__old", "route": { "pe": { "refused": "the old runtime's" } } } ] })");
    return root;
}

}  // namespace

// Each step answers what it owns, in its order.
TEST(WrittenNameResolver, EachStepAnswersTheNameItOwns) {
    ScratchDir dir{Location::Temp, "written-name-resolver"};
    ScopedEnv const env{"DSS_CONFIG_ROOT", stepsCorpus(dir).string()};
    DiagnosticReporter rep;
    auto const a = routesOn({"d", "b", "c", "_c", "t", "s", "__plat_s", "__old", "nope"}, DataModel::Llp64,
                            "x86_64", ObjectFormatKind::Pe, rep);
    ASSERT_TRUE(a.has_value()) << "the scratch corpus was not located";
    EXPECT_FALSE(rep.hasErrors()) << allSaid(rep);
    expectRoute(*a, "d", WrittenNameRouteKind::Direct, "d");                // (1) an image's
    expectRoute(*a, "b", WrittenNameRouteKind::Body, "b");                  // (1) DSS's body
    expectRoute(*a, "_c", WrittenNameRouteKind::Direct, "_c");              // (1)
    expectRoute(*a, "__plat_s", WrittenNameRouteKind::Header, "__plat_s");  // its link name is another
    expectRoute(*a, "s", WrittenNameRouteKind::Direct, "__plat_s");         // (2) the row naming it
    expectRoute(*a, "c", WrittenNameRouteKind::LinkerAlias, "c");           // (3) the stated alias
    expectRoute(*a, "__old", WrittenNameRouteKind::Refused, "");            // (3) a written name
    expectRoute(*a, "t", WrittenNameRouteKind::Header, "t");                // (4) its renaming row rides
    expectRoute(*a, "nope", WrittenNameRouteKind::Header, "");              // (4) no row
    EXPECT_EQ(a->at("c").row.linkName, "_c");
    EXPECT_EQ(a->at("__old").reason, "the old runtime's");
    EXPECT_EQ(a->at("t").row.linkName, "_t64");
    EXPECT_EQ(a->at("b").row.status, ShippedRealizationStatus::ProvidedByShippedSource);
    EXPECT_EQ(a->size(), 9u) << "every name asked is answered";
}

// Two rows of other C names naming one link name, and a stated route where step 2 also answers:
// each refused by name, answered `Header`, and a name of neither shape asked beside them is not.
TEST(WrittenNameResolver, TwoRoutesForOneWrittenNameAreRefused) {
    ScratchDir dir{Location::Temp, "written-name-resolver"};
    fs::path const root = dir.path() / "two";
    write(root / "src" / "dss-config" / "shippedLibs" / "x.json",
          R"({ "header": "x.h", "library": { "pe": "alpha.dll" }, "symbols": [
        { "name": "x1", "signature": "fn() -> i32", "linkName": "amb" },
        { "name": "x2", "signature": "fn() -> i32", "linkName": "amb" },
        { "name": "y", "signature": "fn() -> i32", "linkName": "_y", "writtenNameRoute": { "pe": "linkerAlias" } },
        { "name": "_y", "signature": "fn() -> i32" },
        { "name": "z", "signature": "fn() -> i32", "linkName": "y" },
        { "name": "q", "signature": "fn() -> i32" } ] })");
    ScopedEnv const env{"DSS_CONFIG_ROOT", root.string()};
    {
        DiagnosticReporter rep;
        auto const a = routesOn({"amb"}, DataModel::Llp64, "x86_64", ObjectFormatKind::Pe, rep);
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(countCode(rep, DiagnosticCode::F_ShippedCorpusInvariantBroken), 1u) << allSaid(rep);
        EXPECT_NE(allSaid(rep).find("'x1', 'x2'"), std::string::npos) << allSaid(rep);
        expectRoute(*a, "amb", WrittenNameRouteKind::Header, "");
    }
    {
        DiagnosticReporter rep;
        auto const a = routesOn({"y"}, DataModel::Llp64, "x86_64", ObjectFormatKind::Pe, rep);
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(countCode(rep, DiagnosticCode::F_ShippedCorpusInvariantBroken), 1u) << allSaid(rep);
        EXPECT_NE(allSaid(rep).find("its own row states a route"), std::string::npos) << allSaid(rep);
        expectRoute(*a, "y", WrittenNameRouteKind::Header, "y");
    }
    {
        DiagnosticReporter rep;
        auto const a = routesOn({"q", "_y"}, DataModel::Llp64, "x86_64", ObjectFormatKind::Pe, rep);
        ASSERT_TRUE(a.has_value());
        EXPECT_FALSE(rep.hasErrors()) << "a name of neither shape is refused:" << allSaid(rep);
        expectRoute(*a, "q", WrittenNameRouteKind::Direct, "q");
        expectRoute(*a, "_y", WrittenNameRouteKind::Direct, "_y");
    }
}

// The shipped corpus on pe: the seventeen POSIX names take the linker alias to `_<name>`, `access`
// DSS's body, its underscored twin the image's export, `__iob_func` its stated refusal, and `time`
// the header's rename (its row, `_time64`, rides along).
TEST(WrittenNameResolver, TheShippedCorpusAnswersEachRouteOnPe) {
    ScopedEnv const env{"DSS_CONFIG_ROOT", dss::test::configRoot().parent_path().parent_path().string()};
    std::vector<std::string> const aliased{"write", "lseek", "isatty", "unlink", "rmdir", "chdir",
                                           "getcwd", "open", "close", "read", "dup", "dup2",
                                           "getpid", "strdup", "memccpy", "chmod", "mkdir"};
    std::vector<std::string> names = aliased;
    for (std::string const n : {"access", "_access", "__iob_func", "time"}) names.push_back(n);
    DiagnosticReporter rep;
    auto const a = routesOn(names, DataModel::Llp64, "x86_64", ObjectFormatKind::Pe, rep);
    ASSERT_TRUE(a.has_value()) << "the shipped corpus was not located";
    EXPECT_FALSE(rep.hasErrors()) << allSaid(rep);
    for (std::string const& n : aliased) {
        expectRoute(*a, n, WrittenNameRouteKind::LinkerAlias, n);
        EXPECT_EQ(a->at(n).row.linkName, "_" + n) << n;
    }
    expectRoute(*a, "access", WrittenNameRouteKind::Body, "access");
    expectRoute(*a, "_access", WrittenNameRouteKind::Direct, "_access");
    expectRoute(*a, "__iob_func", WrittenNameRouteKind::Refused, "");
    EXPECT_NE(a->at("__iob_func").reason.find("UCRT"), std::string::npos) << a->at("__iob_func").reason;
    expectRoute(*a, "time", WrittenNameRouteKind::Header, "time");
    EXPECT_EQ(a->at("time").row.linkName, "_time64");
}

// `strtol` on every real pair: its C-name row renames it (C23's entry point, or DSS's body), and the
// platform's own `strtol` -- the C17 function a foreign object compiled against the platform's headers
// means -- is the private row whose link name it is (fold 3e's reverse arm, the resolver's step 2).
TEST(WrittenNameResolver, StrtolIsThePlatformRowOnEveryRealPair) {
    ScopedEnv const env{"DSS_CONFIG_ROOT", dss::test::configRoot().parent_path().parent_path().string()};
    std::size_t asked = 0;
    for (auto const& pair : dss::test_support::shippedReadPairs()) {
        SCOPED_TRACE(pair.label());
        ShippedPairFacts const facts = pair.pairFacts();
        DiagnosticReporter rep;
        auto const a = routesOn({"strtol"}, pair.dataModel(), pair.arch, pair.kind, rep, &facts);
        ASSERT_TRUE(a.has_value());
        EXPECT_FALSE(rep.hasErrors()) << allSaid(rep);
        expectRoute(*a, "strtol", WrittenNameRouteKind::Direct, "__dss_platform_strtol");
        ++asked;
    }
    EXPECT_GE(asked, 5u) << "the real pairs collapsed";
}

// Every C name and every link name the shipped corpus states, asked on every real pair: refused
// nowhere (MEASURED zero ambiguous link names and zero stated routes a row's link name contradicts),
// and every route the corpus has is seen -- so the sweep compared something. ANY error of its reads
// fails it: a descriptor dropped on a pair answers nothing, which counted as zero refusals once
// (MEASURED 2026-11-17, d272: stdlib.json unread on arm64 ELF, its role unanswered by the fixture).
TEST(WrittenNameResolver, NoWrittenNameTheCorpusStatesIsRefusedOnAnyRealPair) {
    ScopedEnv const env{"DSS_CONFIG_ROOT", dss::test::configRoot().parent_path().parent_path().string()};
    auto const every = collectShippedExternSymbolFormats();
    ASSERT_TRUE(every.has_value());
    std::map<std::string, std::size_t> seen;   // route kind -> answers over all pairs
    std::size_t pairs = 0;
    for (auto const& pair : dss::test_support::shippedReadPairs()) {
        SCOPED_TRACE(pair.label());
        ShippedPairFacts const facts = pair.pairFacts();
        // The written names a binder may meet on this pair: every C name, and every link name a row
        // realizes to here.
        std::set<std::string> written;
        for (auto const& entry : *every) written.insert(entry.first);
        {
            TypeInterner interner{CompilationUnitId{1}};
            TypeRegistry typeReg;
            std::array<NamedTypeBinding, 1> const named{NamedTypeBinding{
                kVaListDescriptorTypeName, interner.pointer(interner.primitive(TypeKind::Void))}};
            dss::ffi_test::FixtureRoleResolver const roles{pair.kind, pair.arch};
            std::vector<std::string> const cNames(written.begin(), written.end());
            DiagnosticReporter rep;
            auto const rows = realizeShippedExternSymbols(cNames, interner, typeReg, rep, pair.dataModel(),
                                                          pair.activeTarget(), pair.kind, named, &roles, &facts);
            ASSERT_TRUE(rows.has_value());
            EXPECT_FALSE(rep.hasErrors()) << "every descriptor reads on the pair:" << allSaid(rep);
            for (auto const& [cName, r] : *rows)
                if (!r.linkName.empty()) written.insert(r.linkName);
        }
        DiagnosticReporter rep;
        auto const a = routesOn(std::vector<std::string>(written.begin(), written.end()), pair.dataModel(),
                                pair.arch, pair.kind, rep, &facts);
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(countCode(rep, DiagnosticCode::F_ShippedCorpusInvariantBroken), 0u) << allSaid(rep);
        EXPECT_FALSE(rep.hasErrors()) << "and nothing else is said:" << allSaid(rep);
        EXPECT_EQ(a->size(), written.size()) << "every written name is answered";
        for (auto const& [w, answer] : *a) ++seen[kindName(answer.kind)];
        ++pairs;
    }
    EXPECT_GE(pairs, 5u) << "the real pairs collapsed";
    EXPECT_GE(seen["Direct"], 500u) << "the sweep answered almost nothing directly";
    EXPECT_GE(seen["Body"], 20u) << "no shipped body was reached";
    EXPECT_GE(seen["LinkerAlias"], 17u) << "the seventeen aliases were not reached";
    EXPECT_GE(seen["Header"], 1u) << "no renaming header was reached";
}
