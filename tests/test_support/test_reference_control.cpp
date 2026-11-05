// Self-test for `tests/test_support/reference_control.hpp` (P69 fold 3c; the review of fold 2, F2): how a native
// cell's reference control is judged when it disagrees with its table. A control read wrong fails toward one of two
// silences — a moved reference that never reds (the table goes stale with no witness) or a CI image's toolchain that
// reds a cell DSS answers correctly — so each reading is pinned here, on the words the tables actually carry.

#include "reference_control.hpp"
#include "scoped_env.hpp"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

using dss::test_support::ControlReading;
using dss::test_support::ScopedEnv;
using dss::test_support::StrictArmVerdictsSetting;

TEST(ReferenceControl, TheMeasuredToolchainIsReadOffTheTablesWords) {
    using dss::test_support::measuredVersionMarks;
    EXPECT_EQ(measuredVersionMarks("gcc 13.3.0 with GNU ld 2.42"), (std::vector<std::string>{"13.3.0", "2.42"}));
    EXPECT_EQ(measuredVersionMarks("Apple clang 21.0.0 with ld-1267"),
              (std::vector<std::string>{"21.0.0", "ld-1267"}));
    EXPECT_EQ(measuredVersionMarks("cl 19.44 /std:c11 with link.exe 14.44"),
              (std::vector<std::string>{"19.44", "14.44"}))
        << "`c11` names a language, not a version";
    EXPECT_TRUE(measuredVersionMarks("the host's cc").empty());

    using dss::test_support::onTheMeasuredToolchain;
    std::string const gnu133 = "[`cc --version`: cc (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0; `ld -v`: GNU ld (GNU "
                               "Binutils for Ubuntu) 2.42]";
    std::string const gnu142 = "[`cc --version`: cc (Ubuntu 14.2.0-4ubuntu2) 14.2.0; `ld -v`: GNU ld (GNU Binutils "
                               "for Ubuntu) 2.43.1]";
    std::string const msvc = "[`cl`: Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35229 for x64; `link`: "
                             "Microsoft (R) Incremental Linker Version 14.44.35229.0]";
    EXPECT_TRUE(onTheMeasuredToolchain("gcc 13.3.0 with GNU ld 2.42", gnu133));
    EXPECT_FALSE(onTheMeasuredToolchain("gcc 13.3.0 with GNU ld 2.42", gnu142)) << "another gcc and another ld";
    EXPECT_FALSE(onTheMeasuredToolchain("gcc 13.3.0 with GNU ld 2.42",
                                        "[`cc --version`: cc 13.3.0; `ld -v`: GNU ld 2.41]"))
        << "EVERY tool must be the measured one";
    EXPECT_TRUE(onTheMeasuredToolchain("cl 19.44 with link.exe 14.44", msvc));
    EXPECT_FALSE(onTheMeasuredToolchain("the host's cc", gnu133)) << "words with no version measure no toolchain";
}

TEST(ReferenceControl, ADisagreementReadsByTheToolchainAndTheStrictSetting) {
    using dss::test_support::readDisagreement;
    StrictArmVerdictsSetting off;
    StrictArmVerdictsSetting on;
    on.on = true;
    StrictArmVerdictsSetting malformed;
    malformed.malformed = true;
    EXPECT_EQ(readDisagreement(true, off), ControlReading::ReferenceMoved);
    EXPECT_EQ(readDisagreement(true, on), ControlReading::ReferenceMoved);
    EXPECT_EQ(readDisagreement(false, off), ControlReading::Unmeasured);
    EXPECT_EQ(readDisagreement(false, on), ControlReading::MeasureIt);
    EXPECT_EQ(readDisagreement(false, malformed), ControlReading::MeasureIt) << "an unreadable setting is never off";
}

TEST(ReferenceControl, EachReadingIsAFailureOrACountedLineNeverSilence) {
    using dss::test_support::controlDisagrees;
    using dss::test_support::kStrictArmVerdictsEnv;
    using dss::test_support::unmeasuredControls;
    std::string const measured = "[`cc --version`: cc 13.3.0; `ld -v`: GNU ld 2.42]";
    std::string const another  = "[`cc --version`: cc 14.2.0; `ld -v`: GNU ld 2.43.1]";
    {
        ScopedEnv const notStrict{kStrictArmVerdictsEnv};
        EXPECT_NONFATAL_FAILURE(controlDisagrees("gcc (GNU ld)", "gcc 13.3.0 with GNU ld 2.42", measured, "7", "42"),
                                "reference moved:");
        unsigned const before = unmeasuredControls();
        controlDisagrees("gcc (GNU ld)", "gcc 13.3.0 with GNU ld 2.42", another, "7", "42");
        EXPECT_EQ(unmeasuredControls(), before + 1u) << "an unmeasured toolchain's disagreement is COUNTED";
    }
    {
        ScopedEnv const strict{kStrictArmVerdictsEnv, "1"};
        unsigned const  before = unmeasuredControls();
        EXPECT_NONFATAL_FAILURE(controlDisagrees("gcc (GNU ld)", "gcc 13.3.0 with GNU ld 2.42", another, "7", "42"),
                                "measure it:");
        EXPECT_EQ(unmeasuredControls(), before) << "a strict run fails it rather than counting it";
    }
}
