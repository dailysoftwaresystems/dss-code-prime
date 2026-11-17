#pragma once

// ══ THE ONE REPORT OF A UNIT THAT FAILED HAVING SAID NOTHING (P69 H6c2) ═══════════════════════════════════════════
//
// The several-unit net (`namingSilentFailures`, every_unit.hpp) names a unit whose step failed having added nothing to
// the site's count. Every site that reports into a `DiagnosticReporter` names it HERE, in one message under one code
// (`D_CompileUnitNullNoDiagnostic`, the substrate-contract guard's), so a silent unit reads the same at every site: the
// step, the unit, and that a failing step must say why. `namingSilentUnits` is the net bound to a reporter: the count
// is the reporter's error count, read before each unit, and the silent unit is reported here -- the one loop and the
// one net, with nothing of its own.
//
// THE PRECONDITION EVERY SITE STATES (the net's contract, every_unit.hpp): a unit's step fails only for a failure it
// reported itself, and its report names its unit -- a path, a position, a name -- so two units' reports never say the
// same words (the reporter's recent-duplicate window drops a repeat, which the count would read as silence).

#include "core/substrate/every_unit.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace dss::substrate {

// Reports that `step` failed `unit` without reporting any diagnostic.
inline void reportSilentUnit(DiagnosticReporter& reporter, std::string_view step, std::string_view unit) {
    dss::report(reporter, DiagnosticCode::D_CompileUnitNullNoDiagnostic, DiagnosticSeverity::Error,
                std::format("internal: {} failed {} without reporting any diagnostic -- substrate-contract violation: "
                            "a failing step must say why",
                            step, unit));
}

// The net bound to `reporter`: `unitName(i)` names unit i for the silent report. `step` must outlive the wrapper (a
// literal, as every site passes).
template <class Step, class UnitName>
    requires std::is_invocable_r_v<bool, Step&, std::size_t> && std::is_invocable_r_v<std::string, UnitName&, std::size_t>
[[nodiscard]] auto namingSilentUnits(DiagnosticReporter& reporter, std::string_view step, UnitName&& unitName,
                                     Step&& unitStep) {
    return namingSilentFailures(
        std::forward<Step>(unitStep), [&reporter] { return reporter.errorCount(); },
        [&reporter, step, unitName = std::forward<UnitName>(unitName)](std::size_t i) mutable {
            reportSilentUnit(reporter, step, unitName(i));
        });
}

}  // namespace dss::substrate
