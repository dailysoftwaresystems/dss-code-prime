// The object library `dss_test_driver_diagnostics` — see driver_diagnostics_listener.hpp.

#include "driver_diagnostics_listener.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/compilation_unit/unit_finished_observer.hpp"
#include "core/types/diagnostic_reporter.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dss::test_support {

namespace {

struct Frame {
    std::size_t                 id = 0;
    std::vector<DiagnosticCode> codes;
    std::vector<bool>           seen;
};

struct State {
    std::mutex         mutex;
    std::vector<Frame> frames;   // the live AskedDriverCodes scopes, innermost last
    std::size_t        nextId = 1;
};

State& state() {
    static State s;
    return s;
}

// One unit's driver Errors that no live scope asked for; every asked one is marked seen.
[[nodiscard]] std::vector<ParseDiagnostic> unaskedErrors(CompilationUnit const& unit) {
    std::vector<ParseDiagnostic> unasked;
    State& s = state();
    std::lock_guard<std::mutex> lock{s.mutex};
    for (ParseDiagnostic const& d : unit.driverDiagnostics().all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        bool asked = false;
        for (auto f = s.frames.rbegin(); f != s.frames.rend() && !asked; ++f) {
            for (std::size_t i = 0; i < f->codes.size(); ++i) {
                if (f->codes[i] != d.code) continue;
                f->seen[i] = true;
                asked      = true;
                break;
            }
        }
        if (!asked) unasked.push_back(d);
    }
    return unasked;
}

void onUnitFinished(CompilationUnit const& unit) {
    // The failures are added OUTSIDE the lock: ADD_FAILURE reaches gtest's listeners.
    for (ParseDiagnostic const& d : unaskedErrors(unit)) {
        ADD_FAILURE() << "a unit was finished with a DRIVER error the test did not ask for: "
                      << diagnosticCodeName(d.code) << " — " << d.actual
                      << "\n  The unit compiled a different program than its source says. Give "
                         "the unit what the driver gives it (the system dirs, the include "
                         "dirs), drop what it cannot resolve, or — where the error IS the "
                         "subject — declare dss::test_support::AskedDriverCodes{DiagnosticCode::"
                      << diagnosticCodeName(d.code) << "} before the unit is built "
                         "(tests/test_support/driver_diagnostics_listener.hpp).";
    }
}

// The registration lives as long as the binary: a static of this object library.
struct Installed {
    UnitFinishedObservers::Registration registration =
        UnitFinishedObservers::add(&onUnitFinished);
};

Installed const& installed() {
    static Installed const i;
    return i;
}

// The static initializer that installs it before `main` — the stock gtest main or a
// binary's own.
[[maybe_unused]] bool const kInstalledAtStart = installed().registration.active();

}  // namespace

bool driverDiagnosticsObserverInstalled() noexcept { return installed().registration.active(); }

AskedDriverCodes::AskedDriverCodes(std::initializer_list<DiagnosticCode> codes) {
    State& s = state();
    std::lock_guard<std::mutex> lock{s.mutex};
    id_ = s.nextId++;
    s.frames.push_back(Frame{id_, std::vector<DiagnosticCode>(codes),
                             std::vector<bool>(codes.size(), false)});
}

AskedDriverCodes::~AskedDriverCodes() {
    Frame frame;
    bool  nested = true;
    {
        State& s = state();
        std::lock_guard<std::mutex> lock{s.mutex};
        auto const it = std::find_if(s.frames.begin(), s.frames.end(),
                                     [this](Frame const& f) { return f.id == id_; });
        nested = it != s.frames.end() && std::next(it) == s.frames.end();
        if (it == s.frames.end()) {
            ADD_FAILURE() << "an AskedDriverCodes scope closed that the stack does not hold";
            return;
        }
        frame = std::move(*it);
        s.frames.erase(it);
    }
    if (!nested) {
        ADD_FAILURE() << "an AskedDriverCodes scope closed while a scope opened after it was "
                         "still live — the scopes are stacked, innermost closing first";
    }
    for (std::size_t i = 0; i < frame.codes.size(); ++i) {
        if (frame.seen[i]) continue;
        ADD_FAILURE() << "the test asked for the driver error "
                      << diagnosticCodeName(frame.codes[i])
                      << ", and no unit finished while it was asked for carried it — a pin "
                         "that names a code nothing produced asserts nothing";
    }
}

}  // namespace dss::test_support
