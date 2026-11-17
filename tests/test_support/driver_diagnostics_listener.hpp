#pragma once

// ══ A UNIT'S DRIVER ERROR FAILS THE TEST THAT BUILT IT, UNLESS THE TEST ASKED FOR IT ══
// (P69 L2b2, lane `lm`)
//
// A CompilationUnit carries the DRIVER's answer — an `#include` that resolved nowhere
// (F_ShippedHeaderNotFound), a descriptor that would not read — beside its trees, and
// the driver reads it before it analyzes anything. A test fixture that hands the unit on
// without reading it compiles a DIFFERENT program than its source says, in silence:
// ✔MEASURED 2026-11-05 (the Mac leg, host run 20261105-194136-d2e3e46a and the static
// scan of every TEST): 62 TESTs whose `#include <stdarg.h>` resolved nowhere ran on the
// header-less program, and read as testing the header.
//
// THE HOME, so no future fixture can forget it: every test binary links the object
// library `dss_test_driver_diagnostics` (through `dss_add_test`, and the examples
// runner's own target). Its static initializer registers ONE observer with the
// production `UnitFinishedObservers` (src/analysis/compilation_unit/
// unit_finished_observer.hpp), which `UnitBuilder::finish()` notifies with every unit
// any builder in the process finishes. For each driver diagnostic of severity Error the
// observer ADDS A FAILURE — at once, on the thread that finished the unit, so the
// failure belongs to the statement that built it — unless a live `AskedDriverCodes`
// names its code. A unit finished outside any test (a suite's set-up) fails the way
// gtest attributes any failure there: to that suite, or to the program.
//
// A test that EXPECTS a driver error declares it, before the unit is built:
//     dss::test_support::AskedDriverCodes asked{DiagnosticCode::F_ShippedHeaderNotFound};
// Scopes nest; a code is matched against every live one. A scope whose code no unit
// carried while it lived ALSO fails the test (the absence is the asked-for error's
// silent twin: a pin that names a code nothing produced asserts nothing).

#include "core/types/parse_diagnostic.hpp"

#include <cstddef>
#include <initializer_list>

namespace dss::test_support {

class AskedDriverCodes {
public:
    AskedDriverCodes(std::initializer_list<DiagnosticCode> codes);
    ~AskedDriverCodes();
    AskedDriverCodes(AskedDriverCodes const&)            = delete;
    AskedDriverCodes& operator=(AskedDriverCodes const&) = delete;

private:
    std::size_t id_ = 0;   // this scope's frame on the stack, found when it closes
};

// Whether this binary's observer is registered: the static initializer of the object
// library ran. Asked by the library's own pin, and by a binary with its own `main`.
[[nodiscard]] bool driverDiagnosticsObserverInstalled() noexcept;

}  // namespace dss::test_support
