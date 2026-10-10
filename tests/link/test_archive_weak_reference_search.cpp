// P69 round 4, lane `lm` — D-LK-ARCHIVE-SEARCH-FETCHES-A-MEMBER-FOR-A-WEAK-REFERENCE.
//
// A static link holds a name only as a WEAK reference, and an archive member defines it. Whether the archive search
// FETCHES that member is a property of the members' FORMAT, on which every reference linker of the format agrees
// (✔MEASURED 2026-10-07, probe-reference-cc meta-probes, lane lm):
//   * ELF — no. The System V gABI: "The link editor does not extract archive members to resolve undefined weak
//     symbols." GNU ld 2.42 and ld.lld 18 (x86_64), gcc + GNU ld (aarch64), -no-pie and -pie.
//   * COFF — no, for a weak external whose search policy is NOLIBRARY or ALIAS: link.exe 14.51, lld-link 18, GNU ld's
//     PE linker. A weak external that states SEARCH_LIBRARY asks for the search itself (PE/COFF 5.5.3); link.exe
//     fetches for it, lld-link does not, and the specification decides the fork.
//   * Mach-O — yes. ld64 (Apple clang 21, arm64 and x86_64) fetches it as for a strong reference.
// The members' document states the answer (`archiveWeakReferenceSearch`), and the pull reads it for every weak row
// `linker::weakReferenceAwaitsTheArchiveRule` answers for. Until round 4 the pull followed a weak reference as a
// strong one, so ELF and PE links fetched a member none of their reference linkers fetches.
//
// ROUND 5. A RELOCATABLE link (`-r`) reads the same document, the one its archive-writing sibling is, and gets the
// image link's answer (✔MEASURED 2026-10-08, lane lm): GNU ld 2.42 and ld.lld 18 (x86_64), GNU ld (aarch64) and GNU
// ld's PE linker link it, fetch nothing and leave the weak reference in the artifact; ld64 (arm64 and x86_64) fetches
// the member. Round 4 answered a bare relocatable link from its own document, which states nothing, so the link was
// refused. And a weak reference to a name DSS's runtime realizes from its own source on the link's format (a
// descriptor's `realization.<format>`) is fetched on every format, as a weak reference to a name the platform's own
// library image exports binds: glibc 2.39 and libSystem bind `strtol` and `puts` on their default link (✔MEASURED
// 2026-10-08). Round 4 searched a runtime archive as an operator's, so pe read `strtol` (DSS's body) as NULL beside
// a `puts` that bound (the UCRT's image).
#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/extern_reference_gate.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/pe.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/compile_pipeline.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> format;
};

[[nodiscard]] Loaded load(char const* target, char const* format) {
    Loaded l;
    auto t = TargetSchema::loadShipped(target);
    auto f = ObjectFormatSchema::loadShipped(format);
    EXPECT_TRUE(t.has_value() && f.has_value()) << target << " / " << format;
    if (t.has_value()) l.target = *t;
    if (f.has_value()) l.format = *f;
    return l;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

// The archive: ONE member defining the function `hook` and the function `other` (x86_64 `ret` bodies).
[[nodiscard]] AssembledModule memberDefining(std::string const& hook, std::string const& other) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{1};
    m.expectedFuncCount = 2;
    AssembledFunction h;
    h.symbol = SymbolId{40};
    h.bytes  = {0xC3};
    m.functions.push_back(h);
    AssembledFunction o;
    o.symbol = SymbolId{41};
    o.bytes  = {0xC3};
    m.functions.push_back(o);
    m.symbols.push_back(ModuleSymbol{SymbolId{40}, hook, SymbolBinding::Global, SymbolVisibility::Default});
    m.symbols.push_back(ModuleSymbol{SymbolId{41}, other, SymbolBinding::Global, SymbolVisibility::Default});
    return m;
}

// One import the caller USES (a `call rel32` names it, so the reference gate keeps it).
struct Use {
    std::string   name;
    SymbolBinding binding          = SymbolBinding::Global;
    bool          searchesArchives = false;
};

// The client: `caller` calling every name in `uses` (`call rel32` each, then `ret`).
[[nodiscard]] AssembledModule callerOf(TargetSchema const& target, std::vector<Use> const& uses) {
    auto const* rel32 = target.relocationByName("rel32");
    EXPECT_NE(rel32, nullptr) << "rel32 missing from x86_64.target.json";
    AssembledModule m;
    m.cuId              = CompilationUnitId{2};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    std::uint32_t id = 10;
    for (auto const& u : uses) {
        std::uint32_t const at = static_cast<std::uint32_t>(fn.bytes.size());
        fn.bytes.insert(fn.bytes.end(), {0xE8, 0, 0, 0, 0});
        if (rel32 != nullptr) fn.relocations.push_back(Relocation{at + 1u, SymbolId{id}, rel32->kind, -4});
        ExternImport e;
        e.symbol           = SymbolId{id};
        e.mangledName      = u.name;
        e.binding          = u.binding;
        e.searchesArchives = u.searchesArchives;
        m.externImports.push_back(std::move(e));
        ++id;
    }
    fn.bytes.push_back(0xC3);
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "caller", SymbolBinding::Global, SymbolVisibility::Default});
    return m;
}

// The archive written the way the driver writes one (`<base>-staticlib`), under `dir`.
[[nodiscard]] fs::path archiveOf(fs::path const& dir, char const* staticlibFormat, AssembledModule const& member) {
    auto const L = load("x86_64", staticlibFormat);
    if (!L.target || !L.format) return {};
    std::string const memberName = "hook";
    auto const path = dir / (std::string{"hook-"} + staticlibFormat + ".a");
    DiagnosticReporter rep;
    if (!linkAndWriteStaticArchive(std::span<AssembledModule const>{&member, 1},
                                   std::span<std::string const>{&memberName, 1}, *L.target, *L.format, path, rep)) {
        ADD_FAILURE() << "archive for " << staticlibFormat << " not written:" << diagnosticsOf(rep);
        return {};
    }
    return path;
}

// How many members the pull fetches for `uses` against the archive, linking with the document `format`; nullopt on a
// refusal (its diagnostics in `rep`).
[[nodiscard]] std::optional<std::size_t> pulledWith(TargetSchema const& target, ObjectFormatSchema const& format,
                                                    fs::path const& archive, std::vector<Use> const& uses,
                                                    DiagnosticReporter& rep) {
    AssembledModule const caller = callerOf(target, uses);
    std::vector<fs::path> const archives{archive};
    auto const pulled =
        pullStaticArchiveMembers(std::span<AssembledModule const>{&caller, 1}, archives, {}, target, format, rep);
    if (!pulled.has_value()) return std::nullopt;
    return pulled->size();
}

// The same, linking to the shipped document `linkFormat` (an image's, or a bare relocatable's).
[[nodiscard]] std::optional<std::size_t> pulledFor(char const* linkFormat, fs::path const& archive,
                                                   std::vector<Use> const& uses, DiagnosticReporter& rep) {
    auto const L = load("x86_64", linkFormat);
    if (!L.target || !L.format) return std::nullopt;
    return pulledWith(*L.target, *L.format, archive, uses, rep);
}

// The import row of `m` named `name` (nullptr: none).
[[nodiscard]] ExternImport const* rowNamed(AssembledModule const& m, std::string const& name) {
    for (auto const& e : m.externImports) {
        if (e.mangledName == name) return &e;
    }
    return nullptr;
}

// The shipped document `format` as its file states it, less the key `archiveWeakReferenceSearch`: a members' document
// that leaves the question open.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> withoutTheWeakAnswer(std::string const& format) {
    auto const path = dss::test::configRoot() / "object-formats" / (format + ".format.json");
    std::ifstream in{path};
    auto doc = nlohmann::json::parse(in);
    EXPECT_EQ(doc.erase("archiveWeakReferenceSearch"), 1u) << format << " no longer states the key this test removes";
    auto loaded = ObjectFormatSchema::loadFromText(doc.dump(), format + " - archiveWeakReferenceSearch");
    EXPECT_TRUE(loaded.has_value()) << format << " must load without the key: it is optional until the question arises";
    return loaded.has_value() ? *loaded : nullptr;
}

}  // namespace

// ══ The documents ═════════════════════════════════════════════════════════════

// Every archive-member document states its reference linkers' measured answer, and no other document of its family
// states one — an image's and a bare relocatable's link both read the archive document, so the family has one answer.
TEST(ArchiveWeakReferenceSearch, EachMembersDocumentStatesWhatItsLinkersDo) {
    struct Want {
        char const*                                format;
        std::optional<ArchiveWeakReferenceSearch> answer;
    };
    for (auto const& w : {Want{"elf64-x86_64-linux-staticlib", ArchiveWeakReferenceSearch::DoNotFetch},
                          Want{"elf64-aarch64-linux-staticlib", ArchiveWeakReferenceSearch::DoNotFetch},
                          Want{"pe64-x86_64-windows-staticlib", ArchiveWeakReferenceSearch::DoNotFetch},
                          Want{"macho64-x86_64-darwin-staticlib", ArchiveWeakReferenceSearch::FetchMember},
                          Want{"macho64-arm64-darwin-staticlib", ArchiveWeakReferenceSearch::FetchMember},
                          Want{"elf64-x86_64-linux-exec", std::nullopt},
                          Want{"pe64-x86_64-windows-exec", std::nullopt},
                          Want{"macho64-arm64-darwin-exec", std::nullopt},
                          Want{"elf64-x86_64-linux", std::nullopt},
                          Want{"elf64-aarch64-linux", std::nullopt},
                          Want{"pe64-x86_64-windows", std::nullopt},
                          Want{"macho64-x86_64-darwin", std::nullopt},
                          Want{"macho64-arm64-darwin", std::nullopt}}) {
        auto const f = ObjectFormatSchema::loadShipped(w.format);
        ASSERT_TRUE(f.has_value()) << w.format;
        EXPECT_EQ((*f)->archiveWeakReferenceSearch(), w.answer) << w.format;
    }
}

// The key is read from an archive's MEMBER document: on an image it is refused by name, on a bare relocatable
// document too (round 5: its link reads the archive document, so a second copy there could only drift), and so is a
// value outside the closed set. CONTROL: a member document with a value from the set loads.
TEST(ArchiveWeakReferenceSearch, TheKeyIsRefusedOnAnImageAndOutsideItsValues) {
    auto const withKey = [](std::string const& format, nlohmann::json value) {
        auto const path = dss::test::configRoot() / "object-formats" / (format + ".format.json");
        std::ifstream in{path};
        auto doc = nlohmann::json::parse(in);
        doc["archiveWeakReferenceSearch"] = std::move(value);
        return ObjectFormatSchema::loadFromText(doc.dump(), format + " + archiveWeakReferenceSearch");
    };
    auto const onImage = withKey("elf64-x86_64-linux-exec", "doNotFetch");
    ASSERT_FALSE(onImage.has_value()) << "an image document declaring it must be refused";
    bool named = false;
    for (auto const& d : onImage.error()) named = named || d.path == "/archiveWeakReferenceSearch";
    EXPECT_TRUE(named);
    for (char const* relocatable : {"elf64-x86_64-linux", "pe64-x86_64-windows", "macho64-arm64-darwin"}) {
        auto const onRelocatable = withKey(relocatable, "doNotFetch");
        ASSERT_FALSE(onRelocatable.has_value()) << relocatable << ": a second copy of the family's answer";
        bool namedThere = false;
        for (auto const& d : onRelocatable.error()) namedThere = namedThere || d.path == "/archiveWeakReferenceSearch";
        EXPECT_TRUE(namedThere) << relocatable;
    }
    auto const badValue = withKey("pe64-x86_64-windows-staticlib", "fetch");
    ASSERT_FALSE(badValue.has_value()) << "a value outside the closed set must be refused";
    bool listed = false;
    for (auto const& d : badValue.error()) {
        listed = listed || (d.path == "/archiveWeakReferenceSearch"
                            && d.message.find("fetchMember") != std::string::npos);
    }
    EXPECT_TRUE(listed) << "the refusal names the values";
    EXPECT_TRUE(withKey("pe64-x86_64-windows-staticlib", "fetchMember").has_value());
}

// The rows the members' rule decides: a weak reference with no fallback, no common, no directive and no search of its
// own. Every other surviving row joins the search unconditionally.
TEST(ArchiveWeakReferenceSearch, OnlyAPlainWeakReferenceAwaitsTheMembersRule) {
    ExternImport weak;
    weak.mangledName = "hook";
    weak.binding     = SymbolBinding::Weak;
    EXPECT_TRUE(linker::weakReferenceAwaitsTheArchiveRule(weak));
    ExternImport strong = weak;
    strong.binding      = SymbolBinding::Global;
    EXPECT_FALSE(linker::weakReferenceAwaitsTheArchiveRule(strong)) << "a strong reference always joins the search";
    ExternImport withFallback = weak;
    withFallback.fallbackName = "hook_default";
    EXPECT_FALSE(linker::weakReferenceAwaitsTheArchiveRule(withFallback)) << "the fallback rule decides it";
    ExternImport common = weak;
    common.commonSize   = 4;
    EXPECT_FALSE(linker::weakReferenceAwaitsTheArchiveRule(common)) << "a common is a definition";
    ExternImport required        = weak;
    required.requiredByDirective = true;
    EXPECT_FALSE(linker::weakReferenceAwaitsTheArchiveRule(required)) << "/INCLUDE: makes it required";
    ExternImport searching     = weak;
    searching.searchesArchives = true;
    EXPECT_FALSE(linker::weakReferenceAwaitsTheArchiveRule(searching)) << "SEARCH_LIBRARY asks for the search";
}

// ══ The pull ══════════════════════════════════════════════════════════════════

// ELF: a weak reference fetches nothing. CONTROLS: the same name referenced STRONGLY fetches the member, and a weak
// reference beside a strong one to another name the SAME member defines is satisfied by the member that comes in.
TEST(ArchiveWeakReferenceSearch, AnElfLinkFetchesNothingForAWeakReference) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "weak-archive-elf"};
    auto const archive = archiveOf(scratch.path(), "elf64-x86_64-linux-staticlib", memberDefining("hook", "other"));
    ASSERT_FALSE(archive.empty());
    DiagnosticReporter rep;
    auto const weakOnly = pulledFor("elf64-x86_64-linux-exec", archive, {{"hook", SymbolBinding::Weak}}, rep);
    ASSERT_TRUE(weakOnly.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*weakOnly, 0u) << "the gABI: no member is extracted for an undefined weak symbol";
    auto const strongly = pulledFor("elf64-x86_64-linux-exec", archive, {{"hook", SymbolBinding::Global}}, rep);
    ASSERT_TRUE(strongly.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*strongly, 1u) << "CONTROL: a strong reference to the same name fetches the member";
    auto const beside = pulledFor("elf64-x86_64-linux-exec", archive,
                                  {{"hook", SymbolBinding::Weak}, {"other", SymbolBinding::Global}}, rep);
    ASSERT_TRUE(beside.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*beside, 1u) << "CONTROL: the member fetched for `other` comes in once and defines `hook` too";
}

// PE: a weak external that asks no library search fetches nothing; one that states SEARCH_LIBRARY
// (`searchesArchives`) fetches the member, as link.exe does.
TEST(ArchiveWeakReferenceSearch, APeLinkFetchesOnlyForAWeakExternalThatAsksTheSearch) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "weak-archive-pe"};
    auto const archive = archiveOf(scratch.path(), "pe64-x86_64-windows-staticlib", memberDefining("hook", "other"));
    ASSERT_FALSE(archive.empty());
    DiagnosticReporter rep;
    auto const alias = pulledFor("pe64-x86_64-windows-exec", archive, {{"hook", SymbolBinding::Weak}}, rep);
    ASSERT_TRUE(alias.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*alias, 0u) << "NOLIBRARY / ALIAS: link.exe, lld-link and GNU ld fetch nothing";
    auto const library =
        pulledFor("pe64-x86_64-windows-exec", archive, {{"hook", SymbolBinding::Weak, /*searchesArchives=*/true}}, rep);
    ASSERT_TRUE(library.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*library, 1u) << "SEARCH_LIBRARY: the specification's library search, link.exe's fetch";
}

// Mach-O: ld64 fetches the member for a weak reference as for a strong one.
TEST(ArchiveWeakReferenceSearch, AMachOLinkFetchesTheMemberForAWeakReference) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "weak-archive-macho"};
    auto const archive =
        archiveOf(scratch.path(), "macho64-x86_64-darwin-staticlib", memberDefining("_hook", "_other"));
    ASSERT_FALSE(archive.empty());
    DiagnosticReporter rep;
    auto const weakOnly = pulledFor("macho64-x86_64-darwin-exec", archive, {{"_hook", SymbolBinding::Weak}}, rep);
    ASSERT_TRUE(weakOnly.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*weakOnly, 1u) << "ld64 extracts the member for an undefined weak reference";
}

// A RELOCATABLE link reads its members' document — the archive-writing sibling's, as an image link does — so `-r`
// gets the answer every reference linker of the format gives it (✔MEASURED 2026-10-08): GNU ld and ld.lld fetch
// nothing, GNU ld's PE linker fetches nothing for a weak external that asks no library search, ld64 fetches the
// member. CONTROLS: a strong reference to the same name fetches the member on each, and a COFF weak external that
// states SEARCH_LIBRARY is fetched for.
TEST(ArchiveWeakReferenceSearch, ARelocatableLinkGetsItsMembersAnswer) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "weak-archive-relocatable"};
    DiagnosticReporter rep;
    auto const elf = archiveOf(scratch.path(), "elf64-x86_64-linux-staticlib", memberDefining("hook", "other"));
    ASSERT_FALSE(elf.empty());
    auto const elfWeak = pulledFor("elf64-x86_64-linux", elf, {{"hook", SymbolBinding::Weak}}, rep);
    ASSERT_TRUE(elfWeak.has_value()) << "`ld -r` links this:" << diagnosticsOf(rep);
    EXPECT_EQ(*elfWeak, 0u) << "GNU ld -r and ld.lld -r fetch nothing for a weak reference";
    auto const elfStrong = pulledFor("elf64-x86_64-linux", elf, {{"hook", SymbolBinding::Global}}, rep);
    ASSERT_TRUE(elfStrong.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*elfStrong, 1u) << "CONTROL: a strong reference fetches the member";

    auto const pe = archiveOf(scratch.path(), "pe64-x86_64-windows-staticlib", memberDefining("hook", "other"));
    ASSERT_FALSE(pe.empty());
    auto const peWeak = pulledFor("pe64-x86_64-windows", pe, {{"hook", SymbolBinding::Weak}}, rep);
    ASSERT_TRUE(peWeak.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*peWeak, 0u) << "GNU ld's PE linker -r fetches nothing for a weak function";
    auto const peSearching =
        pulledFor("pe64-x86_64-windows", pe, {{"hook", SymbolBinding::Weak, /*searchesArchives=*/true}}, rep);
    ASSERT_TRUE(peSearching.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*peSearching, 1u) << "CONTROL: SEARCH_LIBRARY asks for the search";

    auto const macho = archiveOf(scratch.path(), "macho64-x86_64-darwin-staticlib", memberDefining("_hook", "_other"));
    ASSERT_FALSE(macho.empty());
    auto const machoWeak = pulledFor("macho64-x86_64-darwin", macho, {{"_hook", SymbolBinding::Weak}}, rep);
    ASSERT_TRUE(machoWeak.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*machoWeak, 1u) << "ld64 -r fetches the member for a weak reference";
}

// What the relocatable link writes: the weak reference it fetched nothing for is still a WEAK undefined name of the
// artifact, so the image link that takes the artifact decides it (and leaves it unresolved, with nothing to define
// it). CONTROL: the artifact does not define the name.
TEST(ArchiveWeakReferenceSearch, ARelocatableArtifactKeepsTheWeakReferenceNothingWasFetchedFor) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "weak-archive-relocatable-artifact"};
    auto const archive = archiveOf(scratch.path(), "elf64-x86_64-linux-staticlib", memberDefining("hook", "other"));
    ASSERT_FALSE(archive.empty());
    auto const L = load("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(L.target && L.format);
    std::vector<AssembledModule> modules{callerOf(*L.target, {{"hook", SymbolBinding::Weak}})};
    std::vector<fs::path> const archives{archive};
    DiagnosticReporter rep;
    auto pulled = pullStaticArchiveMembers(modules, archives, {}, *L.target, *L.format, rep);
    ASSERT_TRUE(pulled.has_value()) << diagnosticsOf(rep);
    ASSERT_TRUE(pulled->empty());
    auto const artifact = linker::link(std::span<AssembledModule const>{modules}, *L.target, *L.format, rep);
    ASSERT_TRUE(artifact.ok()) << diagnosticsOf(rep);
    auto const back = elf::readRelocatableObject(artifact.bytes, *L.target, *L.format, rep);
    ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
    auto const* row = rowNamed(*back, "hook");
    ASSERT_NE(row, nullptr) << "the reference is gone from the artifact";
    EXPECT_EQ(row->binding, SymbolBinding::Weak) << "the reference must stay weak: a strong one fails the image link";
    for (auto const& ms : back->symbols) EXPECT_NE(ms.name, "hook") << "CONTROL: nothing was fetched to define it";
}

// A members' document that states no answer leaves the question to a refusal, by name, the moment it arises — a weak
// reference whose name a member defines. No shipped family leaves it open (round 5: a bare relocatable link reads
// its sibling's), so the open document is a shipped members' document less the key, which the pull reads as the
// link's own. CONTROL: a weak reference no member defines asks nothing, and the same link stands.
TEST(ArchiveWeakReferenceSearch, AnUnstatedAnswerIsRefusedByNameWhenTheQuestionArises) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "weak-archive-unstated"};
    auto const archive = archiveOf(scratch.path(), "elf64-x86_64-linux-staticlib", memberDefining("hook", "other"));
    ASSERT_FALSE(archive.empty());
    auto const L = load("x86_64", "elf64-x86_64-linux-staticlib");
    ASSERT_TRUE(L.target && L.format);
    ASSERT_TRUE(L.format->archiveWeakReferenceSearch().has_value()) << "the shipped document states it";
    auto const open = withoutTheWeakAnswer("elf64-x86_64-linux-staticlib");
    ASSERT_NE(open, nullptr);
    ASSERT_FALSE(open->archiveWeakReferenceSearch().has_value());
    DiagnosticReporter refusedRep;
    auto const refused = pulledWith(*L.target, *open, archive, {{"hook", SymbolBinding::Weak}}, refusedRep);
    EXPECT_FALSE(refused.has_value()) << "an unstated answer must not be guessed";
    EXPECT_NE(diagnosticsOf(refusedRep).find("'archiveWeakReferenceSearch'"), std::string::npos)
        << diagnosticsOf(refusedRep);
    EXPECT_NE(diagnosticsOf(refusedRep).find("'hook' is a WEAK reference"), std::string::npos)
        << diagnosticsOf(refusedRep);
    DiagnosticReporter controlRep;
    auto const control = pulledWith(*L.target, *open, archive, {{"absent", SymbolBinding::Weak}}, controlRep);
    ASSERT_TRUE(control.has_value()) << diagnosticsOf(controlRep);
    EXPECT_EQ(*control, 0u);
    DiagnosticReporter statedRep;
    auto const stated = pulledWith(*L.target, *L.format, archive, {{"hook", SymbolBinding::Weak}}, statedRep);
    ASSERT_TRUE(stated.has_value()) << "CONTROL: the shipped document answers:" << diagnosticsOf(statedRep);
    EXPECT_EQ(*stated, 0u);
}

// A link format that has NO archive-writing sibling (wasm32-v1, spirv-1.6) cannot say whose members an archive
// holds, so the question is refused by name where it arises — never answered by the link's own document, which is
// what a non-image document did until round 5 (the refusal then came one step later, from a backend with no member
// reader, under another code).
TEST(ArchiveWeakReferenceSearch, ALinkFormatWithNoArchiveSiblingIsRefusedByName) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "weak-archive-no-sibling"};
    auto const archive = archiveOf(scratch.path(), "elf64-x86_64-linux-staticlib", memberDefining("hook", "other"));
    ASSERT_FALSE(archive.empty());
    for (char const* const format : {"wasm32-v1", "spirv-1.6"}) {
        SCOPED_TRACE(format);
        DiagnosticReporter rep;
        auto const pulled = pulledFor(format, archive, {{"hook", SymbolBinding::Weak}}, rep);
        EXPECT_FALSE(pulled.has_value()) << "a document with no archive sibling answered for an archive's members";
        EXPECT_NE(diagnosticsOf(rep).find("archive-writing sibling"), std::string::npos) << diagnosticsOf(rep);
    }
}

// ══ A name the runtime realizes ═══════════════════════════════════════════════

// To a program, DSS's runtime is the platform's library: a weak reference to a name whose body a descriptor states
// DSS ships as source on the link's format binds — the member that defines it is fetched — whatever the members'
// document says of an operator's archive. The corpus is asked by name (the shipped descriptors' `realization`
// rows): `strtol` and `memset_explicit` on pe, `memset_explicit` alone on ELF, where glibc's image exports `strtol`.
// CONTROLS: `hook`, which no descriptor declares, follows the members' document on both; and `strtol` on ELF does
// too — the same name, the platform's own image's there, so an operator's archive that defines it is not searched
// for a weak reference, as GNU ld does not search one.
TEST(ArchiveWeakReferenceSearch, AWeakReferenceToANameTheRuntimeRealizesIsFetched) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "weak-archive-runtime-name"};
    DiagnosticReporter rep;
    auto const pe = archiveOf(scratch.path(), "pe64-x86_64-windows-staticlib", memberDefining("strtol", "hook"));
    ASSERT_FALSE(pe.empty());
    auto const peRuntime = pulledFor("pe64-x86_64-windows-exec", pe, {{"strtol", SymbolBinding::Weak}}, rep);
    ASSERT_TRUE(peRuntime.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*peRuntime, 1u) << "pe: DSS ships `strtol`'s body, so the weak reference binds";
    auto const peHook = pulledFor("pe64-x86_64-windows-exec", pe, {{"hook", SymbolBinding::Weak}}, rep);
    ASSERT_TRUE(peHook.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*peHook, 0u) << "CONTROL: a name no descriptor declares follows the members' document";
    auto const peRelocatable = pulledFor("pe64-x86_64-windows", pe, {{"strtol", SymbolBinding::Weak}}, rep);
    ASSERT_TRUE(peRelocatable.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*peRelocatable, 1u) << "a relocatable link asks the same question";

    auto const elfC23 =
        archiveOf(scratch.path(), "elf64-x86_64-linux-staticlib", memberDefining("memset_explicit", "hook"));
    ASSERT_FALSE(elfC23.empty());
    auto const elfRuntime =
        pulledFor("elf64-x86_64-linux-exec", elfC23, {{"memset_explicit", SymbolBinding::Weak}}, rep);
    ASSERT_TRUE(elfRuntime.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*elfRuntime, 1u) << "ELF: DSS ships `memset_explicit`'s body (glibc 2.39 has none)";
    auto const elfHook = pulledFor("elf64-x86_64-linux-exec", elfC23, {{"hook", SymbolBinding::Weak}}, rep);
    ASSERT_TRUE(elfHook.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*elfHook, 0u) << "CONTROL: a name no descriptor declares follows the members' document";

    std::filesystem::remove(elfC23);
    auto const elfLibc = archiveOf(scratch.path(), "elf64-x86_64-linux-staticlib", memberDefining("strtol", "hook"));
    ASSERT_FALSE(elfLibc.empty());
    auto const elfImageName = pulledFor("elf64-x86_64-linux-exec", elfLibc, {{"strtol", SymbolBinding::Weak}}, rep);
    ASSERT_TRUE(elfImageName.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(*elfImageName, 0u) << "CONTROL: on ELF `strtol` is libc.so.6's, not a body DSS ships";
}

// ══ COFF: the policy read, written back, and folded ══════════════════════════

namespace {

// The Auxiliary Format 3 Characteristics of the weak external named `name` in a COFF object (nullopt: none).
[[nodiscard]] std::optional<std::uint32_t> weakExternalPolicy(std::vector<std::uint8_t> const& obj,
                                                              std::string const& name) {
    auto const rd = [&](std::size_t off, int width) {
        std::uint64_t v = 0;
        for (int i = 0; i < width; ++i) v |= static_cast<std::uint64_t>(obj[off + i]) << (i * 8);
        return v;
    };
    std::size_t const symtab = rd(8, 4);
    std::size_t const count  = rd(12, 4);
    std::size_t const strtab = symtab + count * 18u;
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t const at = symtab + i * 18u;
        std::string n;
        if (rd(at, 4) == 0) {
            for (std::size_t p = strtab + rd(at + 4, 4); p < obj.size() && obj[p] != 0; ++p) n.push_back(char(obj[p]));
        } else {
            for (std::size_t k = 0; k < 8 && obj[at + k] != 0; ++k) n.push_back(char(obj[at + k]));
        }
        std::uint8_t const storageClass = obj[at + 16];
        std::uint8_t const aux          = obj[at + 17];
        if (n == name && storageClass == 105 && aux >= 1) return static_cast<std::uint32_t>(rd(at + 18 + 4, 4));
        i += aux;
    }
    return std::nullopt;
}

}  // namespace

// DSS's COFF writer gives a weak reference ALIAS (3) and one whose row asks the search SEARCH_LIBRARY (2); the reader
// reads 2 back as `searchesArchives` and 3 as not. And two objects' rows of one name fold to a row that asks the
// search when either does (the link merge's OR-combine).
TEST(ArchiveWeakReferenceSearch, ACoffWeakExternalsLibrarySearchIsReadWrittenAndFolded) {
    auto const L = load("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(L.target && L.format);
    for (bool const searches : {false, true}) {
        AssembledModule m = callerOf(*L.target, {{"hook", SymbolBinding::Weak, searches}});
        DiagnosticReporter rep;
        auto const obj = pe::encode(m, *L.target, *L.format, rep);
        ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
        EXPECT_EQ(weakExternalPolicy(obj, "hook"), std::optional<std::uint32_t>{searches ? 2u : 3u})
            << (searches ? "SEARCH_LIBRARY" : "ALIAS");
        auto const back = pe::readRelocatableObject(obj, *L.target, *L.format, rep);
        ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
        auto const* row = rowNamed(*back, "hook");
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->binding, SymbolBinding::Weak);
        EXPECT_EQ(row->searchesArchives, searches);
    }
    // The fold: one object asks the search, the other does not — the relocatable artifact's one row asks it.
    std::vector<AssembledModule> mods{callerOf(*L.target, {{"hook", SymbolBinding::Weak, false}}),
                                      callerOf(*L.target, {{"hook", SymbolBinding::Weak, true}})};
    mods[1].cuId               = CompilationUnitId{3};
    mods[1].symbols[0].name    = "caller2";
    DiagnosticReporter foldRep;
    auto const folded = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, foldRep);
    ASSERT_TRUE(folded.ok()) << diagnosticsOf(foldRep);
    EXPECT_EQ(weakExternalPolicy(folded.bytes, "hook"), std::optional<std::uint32_t>{2u})
        << "one object's SEARCH_LIBRARY is the folded row's";
}
