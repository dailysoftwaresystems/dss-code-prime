// THE ONE HOME OF "A SEVERAL-UNIT STEP REPORTS EVERY UNIT BEFORE IT FAILS" -- src/program/every_unit.hpp's verb
// `runEveryUnit` (P69 H6b, the review of H6's N1; the orchestrator's ledger 550), pinned over both cells: every unit
// failing, and a valid unit before an invalid one (with a valid one after it) -- each unit asked, the step failed --
// and the control, every unit succeeding (pure: no build, no host). Then one call site's cell, the input-language
// claim's; the archive branch's and the archive member link's are in tests/program/test_archive_definition_visibility.cpp
// (EveryUnitsErrorsAreReportedAndABuildAnyUnitFailedWritesNoArchive, AnArchivesRequestIsRefusedOnceForTheWholeArchive).

#include "program/every_unit.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"

#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST(EveryUnit, EveryUnitFailingIsEachAskedAndTheStepFails) {
    std::vector<std::size_t> asked;
    bool const every = dss::runEveryUnit(3, [&](std::size_t i) {
        asked.push_back(i);
        return false;
    });
    EXPECT_FALSE(every) << "three failed units were taken for success";
    EXPECT_EQ(asked, (std::vector<std::size_t>{0, 1, 2})) << "a failed unit stopped the units after it";
}

TEST(EveryUnit, AValidUnitBeforeAnInvalidOneIsAskedAndTheStepFails) {
    std::vector<std::size_t> asked;
    bool const every = dss::runEveryUnit(3, [&](std::size_t i) {
        asked.push_back(i);
        return i != 1;   // valid, invalid, valid
    });
    EXPECT_FALSE(every) << "an invalid unit between two valid ones was taken for success";
    EXPECT_EQ(asked, (std::vector<std::size_t>{0, 1, 2})) << "the invalid unit stopped the valid one after it";
}

TEST(EveryUnit, EveryUnitSucceedingSucceedsTheControl) {
    std::vector<std::size_t> asked;
    EXPECT_TRUE(dss::runEveryUnit(2, [&](std::size_t i) {
        asked.push_back(i);
        return true;
    }));
    EXPECT_EQ(asked, (std::vector<std::size_t>{0, 1}));
    EXPECT_TRUE(dss::runEveryUnit(0, [](std::size_t) { return false; })) << "no unit asked is no unit failed";
}

// THE INPUT-LANGUAGE CLAIM (program.cpp `resolveGrammarForTarget`): with no `--language`, the language the target
// declares must claim every input, and EVERY input it does not claim is named before the build fails -- two inputs of
// extensions no shipped language claims, two D_UnknownFileExtension, each naming its own file.
TEST(EveryUnit, EveryInputTheTargetsLanguageDoesNotClaimIsNamedBeforeTheBuildFails) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "every-unit-claim"};
    std::vector<std::string> sources;
    for (char const* name : {"first.zz1", "second.zz2"}) {
        std::filesystem::path const file = scratch.path() / name;
        std::ofstream(file, std::ios::binary) << "x\n";
        sources.push_back(file.string());
    }
    dss::Program p;
    p.setOutputDir(scratch.path());
    dss::DiagnosticReporter rep;
    EXPECT_NE(p.compileUnits(sources, "", {"x86_64:elf64-x86_64-linux-exec"}, rep), 0)
        << "two inputs no language claims were built";
    std::string said;
    for (auto const& d : rep.all()) said += "\n  " + d.actual;
    for (char const* name : {"first.zz1", "second.zz2"}) {
        bool named = false;
        for (auto const& d : rep.all()) {
            if (d.code == dss::DiagnosticCode::D_UnknownFileExtension && d.actual.find(name) != std::string::npos)
                named = true;
        }
        EXPECT_TRUE(named) << "the unclaimed input " << name << " was not named; the build said:" << said;
    }
}
