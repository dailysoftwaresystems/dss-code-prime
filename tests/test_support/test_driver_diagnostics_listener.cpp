// P69 L2b2 (lane `lm`): the pin of the fixture home — `dss_test_driver_diagnostics`
// (driver_diagnostics_listener.hpp) on the production `UnitFinishedObservers`. Each link
// of the chain is asked in the binary itself: `finish()` notifies a registration; the
// library's observer turns an UNASKED driver Error into a failure of the test that built
// the unit; an ASKED one is the test's own; an asked code no unit carried fails too.
// The failures are caught with gtest's own `EXPECT_NONFATAL_FAILURE`, which works here
// because the observer adds them on the thread that finished the unit, at once.

#include "driver_diagnostics_listener.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/compilation_unit/unit_finished_observer.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

namespace {

using dss::CompilationUnit;
using dss::DiagnosticBudget;
using dss::DiagnosticCode;
using dss::UnitBuilder;
using dss::UnitFinishedObservers;
using dss::test_support::AskedDriverCodes;

// A unit built the way the blind fixtures built theirs: NO system dir, so its angle
// include resolves nowhere and the driver reports it on the unit.
void buildUnitWithAnUnresolvedInclude() {
    UnitBuilder b{dss::test_support::shippedSchemaOrThrow("c"), DiagnosticBudget::libraryDefault()};
    b.addInMemory("#include <zz_no_such_shipped_header.h>\nint main(void) { return 0; }\n",
                  "<mem>");
    (void)std::move(b).finish();
}

void buildACleanUnit() {
    UnitBuilder b{dss::test_support::shippedSchemaOrThrow("c"), DiagnosticBudget::libraryDefault()};
    b.addInMemory("int main(void) { return 0; }\n", "<mem>");
    (void)std::move(b).finish();
}

void askForACodeAndBuildACleanUnit() {
    AskedDriverCodes asked{DiagnosticCode::F_ShippedHeaderNotFound};
    buildACleanUnit();
}

TEST(DriverDiagnosticsListener, TheObserverIsInstalledBeforeAnyTestRuns) {
    EXPECT_TRUE(dss::test_support::driverDiagnosticsObserverInstalled())
        << "the object library's static initializer did not register its observer";
}

TEST(DriverDiagnosticsListener, EveryFinishedUnitIsHeardByALiveRegistration) {
    std::size_t heard = 0;
    {
        auto const registration =
            UnitFinishedObservers::add([&heard](CompilationUnit const&) { ++heard; });
        buildACleanUnit();
        buildACleanUnit();
    }
    EXPECT_EQ(heard, 2u) << "UnitBuilder::finish notifies every registration, once per unit";
    buildACleanUnit();
    EXPECT_EQ(heard, 2u) << "an ended registration hears nothing";
}

TEST(DriverDiagnosticsListener, AnUnaskedDriverErrorFailsTheTestThatBuiltTheUnit) {
    EXPECT_NONFATAL_FAILURE(buildUnitWithAnUnresolvedInclude(),
                            "a DRIVER error the test did not ask for");
}

TEST(DriverDiagnosticsListener, AnAskedDriverErrorIsTheTestsOwn) {
    AskedDriverCodes asked{DiagnosticCode::F_ShippedHeaderNotFound};
    buildUnitWithAnUnresolvedInclude();   // no failure: the test asked for exactly this
}

TEST(DriverDiagnosticsListener, AnAskedCodeThatNoUnitCarriedFailsTheTest) {
    EXPECT_NONFATAL_FAILURE(askForACodeAndBuildACleanUnit(),
                            "no unit finished while it was asked for carried it");
}

TEST(DriverDiagnosticsListener, AScopeAsksForItsCodesOnlyWhileItLives) {
    {
        AskedDriverCodes asked{DiagnosticCode::F_ShippedHeaderNotFound};
        buildUnitWithAnUnresolvedInclude();
    }
    EXPECT_NONFATAL_FAILURE(buildUnitWithAnUnresolvedInclude(),
                            "a DRIVER error the test did not ask for");
}

}  // namespace
