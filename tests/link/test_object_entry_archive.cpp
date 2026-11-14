// P69 fold 3e, lane p69/xa (the review of fold 3d, X4 and X2): an entry only an archive member defines, and a program
// of two entry names on a format that realizes both.
//
// X4. ✔MEASURED before (lane `xa`, the r36 dsscp): `app.c lib{m}` and `app.o lib{m}`, the program's `main` defined
// only by the archive's member, link and run under GNU ld 2.42 (gcc and clang), ld.lld 18.1.3, MinGW-w64 GNU ld 2.42
// and lld-link 19.1.5, and under link.exe 14.44 with the console subsystem the pe64 exec document states (with no
// subsystem link.exe refuses, LNK1561, choosing its startup from the named objects alone). DSS refused it,
// K_ProgramEntryUndefined (a source) and K_SymbolUndefined (objects). Beside a named input that defines an entry,
// every reference and DSS elect the named one and never pull the member. Now, where no named input defines an entry
// the format realizes, the entry names seed the archive search as the references' startup object does
// (`pullEntryArchiveMembers`), and the member's entry stands in the one election.
//
// X2. ✔MEASURED (lane `xa`): `main` (11) beside `wmain` (22) for pe64-x86_64-windows-exec -- one unit, two units, a
// unit beside an object, two objects -- link.exe 14.44 runs `main` with LNK4067 ("ambiguous entry point; selected
// 'mainCRTStartup'"), lld-link 19.1.5 with "found both wmain and main; using latter", MinGW-w64 GNU ld `main` unless
// `-municode`. DSS refused each as ambiguous. The pe64 exec document now states the precedence (`entryPrecedence`:
// the narrow startup's verbs above the wide one's), and the election takes the one candidate of the highest rank
// with the warning K_ProgramEntryChosenByPrecedence; two of one rank stay ambiguous.
//
// The families, their fixtures and where their images run are object_entry_support.hpp's.

#include "object_entry_support.hpp"

#include "core/substrate/path_identity.hpp"   // extendedLengthSpelling, createDirectories: a path at any length
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <vector>

using namespace dss;
using namespace dss::test_support::object_entry;

namespace {

// The archive member's entry: the fixtures' `main(argc, argv)`, so its program exits 47 as theirs do.
constexpr char const* kMemberMainSource =
    "int helper(void);\n"
    "int main(int argc, char **argv) { return helper() + argc * 10 + (argc > 1 ? argv[1][0] - '0' : 0); }\n";

// Each cell of an entry an archive member defines (`kMemberMainSource`, handed as `--resolve-library`), and what its
// program exits: the member's entry where no named input defines one (47), and the named one where one does (5).
struct ArchiveCell {
    char const*        label;
    std::vector<Input> inputs;
    std::string        language;
    std::uint32_t      exit;
    bool               argumentsRead;   // the elected entry is called with the startup's arguments (an object's)
};

[[nodiscard]] std::vector<ArchiveCell> archiveCellsOf(Family const& f) {
    std::string const helper = objectName(f, "helper");
    return {
        {"one source", {written("helper.c", kHelperSource)}, "c", 47u, true},
        {"two sources", {written("helper.c", kHelperSource), written("other.c", kOtherSource)}, "c", 47u, true},
        {"an object", {fixture(helper)}, "c", 47u, true},
        {"an object, no --language", {fixture(helper)}, "", 47u, true},
        {"an assembly unit", {written("helper.s", helperAsm(f))}, "", 47u, true},
        {"a named object's entry", {written("helper.c", kHelperSource), fixture(objectName(f, "void"))}, "c", 5u, true},
        // A unit's `main(void)` takes no argument, so no accessor is imported for it.
        {"a source's entry", {written("main.c", kMainSource), fixture(helper)}, "c", 5u, false},
    };
}

// The archive of `kMemberMainSource` DSS builds for the family's static-library flavor, in `dir`.
[[nodiscard]] fs::path memberArchive(fs::path const& dir, Family const& f) {
    return buildArtifact(dir, "member_main.c", kMemberMainSource, flavorOf(f, "staticlib"));
}

constexpr char const* kNarrowMain = "int main(void) { return 11; }\n";
constexpr char const* kWideMain   = "int wmain(int argc, unsigned short **argv) { (void)argc; (void)argv; return 22; }\n";

struct PrecedenceCell {
    char const*        label;
    std::vector<Input> inputs;
    std::string        language;
};

// Each pair of `main` and `wmain` a program can hold -- in units, in objects DSS built (`-c`) and in an assembly
// unit, on every route; every reference starts each at `main` (11).
[[nodiscard]] std::vector<PrecedenceCell> precedenceCellsOf(Family const& f, fs::path const& mainObj,
                                                            fs::path const& wmainObj) {
    return {
        {"a unit's main, an object's wmain", {written("main.c", kNarrowMain), copied(wmainObj)}, "c"},
        {"a unit's wmain, an object's main", {written("wmain.c", kWideMain), copied(mainObj)}, "c"},
        {"both objects' beside a unit of neither",
         {written("helper.c", kHelperSource), copied(mainObj), copied(wmainObj)}, "c"},
        {"objects alone, main's first", {copied(mainObj), copied(wmainObj)}, "c"},
        {"objects alone, wmain's first", {copied(wmainObj), copied(mainObj)}, "c"},
        {"objects alone, no --language", {copied(wmainObj), copied(mainObj)}, ""},
        {"two units' main and an object's wmain",
         {written("main.c", kNarrowMain), written("other.c", kOtherSource), copied(wmainObj)}, "c"},
        {"an assembly unit's main, an object's wmain",
         {written("main.s", soleAsm(f, "main", 11u)), copied(wmainObj)}, ""},
    };
}

// The two objects of the precedence cells, built by DSS for the family's relocatable format in `dir`; nullopt after
// a failure, which is reported.
struct EntryObjects {
    fs::path narrow, wide;
};

[[nodiscard]] std::optional<EntryObjects> entryObjects(fs::path const& dir, Family const& f) {
    EntryObjects o{buildArtifact(dir / "main", "main_narrow.c", kNarrowMain, flavorOf(f, "")),
                   buildArtifact(dir / "wmain", "main_wide.c", kWideMain, flavorOf(f, ""))};
    if (o.narrow.empty() || o.wide.empty()) return std::nullopt;
    return o;
}

}  // namespace

// ══ AN ENTRY ONLY AN ARCHIVE MEMBER DEFINES IS PULLED BY THE STARTUP ═══════════════════════════════════════════════

// Every leg, every family: an entry only a member of a static archive (built by DSS for the family's format) defines
// links on every route -- beside one source, two, an object (with and without `--language`) and an assembly unit --
// as every reference links it; beside a named input that defines an entry, that one is elected (the native TEST runs
// each to 47 or 5). RED-ON-DISABLE: never ask the archives for an entry -> every pulling cell is refused
// K_ProgramEntryUndefined.
TEST(ObjectEntry, AnEntryInAnArchiveMemberIsPulledByTheStartup) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object-entry-archive"};
    std::size_t k = 0;
    for (auto const& family : kFamilies) {
        SCOPED_TRACE(labelOf(family));
        fs::path const archive = memberArchive(scratch.path() / ("lib" + std::to_string(k++)), family);
        if (archive.empty()) continue;   // reported
        for (auto const& c : archiveCellsOf(family)) {
            SCOPED_TRACE(std::string{c.label} + ", " + grammarLabel(c.language));
            linkCell(scratch.path() / std::to_string(k++), family, c.inputs, c.language, {archive}, c.argumentsRead);
        }
    }
}

// ══ `main` BESIDE `wmain`: THE FORMAT STATES WHICH STARTS THE PROGRAM ══════════════════════════════════════════════

// Every leg (each image linked, not run): on the pe64 exec document, which realizes both names, every pair of `main`
// and `wmain` links, warned K_ProgramEntryChosenByPrecedence and not refused as ambiguous. RED-ON-DISABLE: remove
// `entryPrecedence` from the pe64 exec document -> every cell is refused K_ProgramEntryAmbiguous.
TEST(ObjectEntry, MainBesideWmainStartsAtMainWhereTheFormatRanksIt) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object-entry-precedence"};
    std::size_t k = 0;
    std::set<std::string> targets;   // the cells build DSS's own objects: one round per executable target
    for (auto const& family : kFamilies) {
        if (!isPe(family)) continue;   // only PE realizes a wide entry
        if (!targets.insert(family.execTarget).second) continue;
        SCOPED_TRACE(labelOf(family));
        auto const objects = entryObjects(scratch.path() / ("obj" + std::to_string(k++)), family);
        if (!objects) continue;   // reported
        for (auto const& c : precedenceCellsOf(family, objects->narrow, objects->wide)) {
            SCOPED_TRACE(c.label);
            Built out;
            build(scratch.path() / std::to_string(k++), c.inputs, family.execTarget, out, c.language);
            EXPECT_EQ(out.rc, 0) << out.diagnostics;
            EXPECT_FALSE(out.image.empty()) << "the build writes one image";
            EXPECT_TRUE(reported(out.rep, DiagnosticCode::K_ProgramEntryChosenByPrecedence)) << out.diagnostics;
            EXPECT_FALSE(reported(out.rep, DiagnosticCode::K_ProgramEntryAmbiguous)) << out.diagnostics;
        }
    }
}

// ══ NATIVE: the elected entry is the one that runs ════════════════════════════════════════════════════════════════

// Where the host runs the family's images: each archive cell's program exits as the references' does (the member's
// entry 47, a named one beside it 5), and on PE every `main` beside a `wmain` runs `main` (11, `wmain` 22).
// RED-ON-DISABLE: as the in-process TESTs', whose builds these are; and enter a PE program of both names at `wmain`
// -> every precedence cell exits 22.
TEST(ObjectEntryNative, ArchiveMembersAndPrecedenceRunAsTheReferencesDo) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object-entry-archive-run"};
    NativeFamilies natives;
    std::size_t k = 0;
    std::set<std::string> targets;   // the precedence cells build DSS's own objects: one round per executable target
    for (auto const& family : kFamilies) {
        auto const gate = natives.admit(family);
        if (!gate) continue;
        fs::path const archive = memberArchive(scratch.path() / ("lib" + std::to_string(k++)), family);
        if (!archive.empty()) {
            for (auto const& c : archiveCellsOf(family)) {
                SCOPED_TRACE(labelOf(family) + ", an archive's member: " + c.label + ", " + grammarLabel(c.language));
                runCell(scratch.path() / std::to_string(k++), family, *gate, c.inputs, c.exit, c.language, {archive});
            }
        }
        if (!isPe(family) || !targets.insert(family.execTarget).second) continue;
        auto const objects = entryObjects(scratch.path() / ("obj" + std::to_string(k++)), family);
        if (!objects) continue;   // reported
        for (auto const& c : precedenceCellsOf(family, objects->narrow, objects->wide)) {
            SCOPED_TRACE(labelOf(family) + ", main beside wmain: " + c.label);
            runCell(scratch.path() / std::to_string(k++), family, *gate, c.inputs, 11u, c.language);
        }
    }
    if (natives.ranNone()) GTEST_SKIP() << natives.why();
}

// ══ THE ENTRY'S ARCHIVE AT ANY LENGTH ═════════════════════════════════════════════════════════════════════════════

// Windows (the 260-character limit is Win32's; every other host compiles this TEST and has no such limit to pin): the
// archive the startup's entry is pulled from is opened at any length, as every Win32 file call of a build takes its
// path (`core::extendedLengthSpelling`; tests/program/test_artifact_at_any_length.cpp pins the object input's): an
// archive at 300 characters or more whose member alone defines `main`, beside a source of none, links and its image
// runs to 42, as the control below 260 does. Fold 3e's entry pull (`pullEntryArchiveMembers`, new in the fold) opened
// its archives under their plain spelling, the one call of the program tier that did.
// RED-ON-DISABLE: open the entry's archive under its plain spelling -> the 300-character cell is refused.
TEST(ObjectEntryNative, AnEntrysArchiveAtAnyLengthIsPulledAndRuns) {
#if defined(_WIN32)
    struct Cell {
        char const* name;
        std::size_t atLeast;   // 0: the control, below 260
    };
    test_support::ScratchDir scratch{test_support::Location::Temp, "object-entry-archive-any-length"};
    fs::path const archive = buildArtifact(scratch.path() / "lib", "member_main.c", "int main(void) { return 42; }\n",
                                           "x86_64:pe64-x86_64-windows-staticlib");
    ASSERT_FALSE(archive.empty());   // reported
    std::ifstream in{core::extendedLengthSpelling(archive), std::ios::binary};
    std::string const bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    ASSERT_FALSE(bytes.empty()) << "the archive DSS built was not read";
    std::size_t k = 0;
    for (Cell const cell : {Cell{"below 260 (the control)", 0}, Cell{"300 characters or more", 300}}) {
        SCOPED_TRACE(cell.name);
        fs::path dir = scratch.path() / ("at" + std::to_string(k++));
        while (dir.native().size() < cell.atLeast) dir /= std::string(40, 'a');
        std::error_code ec;
        static_cast<void>(core::createDirectories(dir, ec));
        ASSERT_FALSE(ec) << ec.message();
        fs::path const placed = dir / archive.filename();
        if (cell.atLeast != 0) EXPECT_GE(placed.native().size(), 300u) << "the archive's path is not past the limit";
        {
            std::ofstream out{core::extendedLengthSpelling(placed), std::ios::binary};
            out << bytes;
            ASSERT_TRUE(out.good()) << "the archive was not placed";
        }
        Built built;
        build(scratch.path() / ("link" + std::to_string(k++)), {written("helper.c", kHelperSource)},
              "x86_64:pe64-x86_64-windows-exec", built, "c", {placed});
        ASSERT_EQ(built.rc, 0) << "the entry's archive at " << cell.name << " was not pulled:" << built.diagnostics;
        ASSERT_FALSE(built.image.empty()) << "the build writes one image";
        auto const run = test_support::runBinary(built.image);
        ASSERT_TRUE(run.spawned) << run.diagnostic;
        EXPECT_FALSE(run.timedOut);
        EXPECT_EQ(run.exitCode, 42u);
    }
#else
    GTEST_SKIP() << "the 260-character limit is Win32's: this host has none to pin";
#endif
}
