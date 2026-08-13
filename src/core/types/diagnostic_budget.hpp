#pragma once

#include "core/types/diagnostic_reporter.hpp"

#include <cstddef>

namespace dss {

// The VOLUME half of `DiagnosticReporter::Config` — the three axes that bound
// how much a reporter accumulates (`maxDiagnostics` / `maxPerCode` /
// `dedupWindow`) — carried down to every PER-TIER reporter that later drains
// into the run-wide aggregation reporter.
//
// ─────────────────────────────────────────────────────────────────────────
// WHY THIS TYPE EXISTS AT ALL (D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE)
// ─────────────────────────────────────────────────────────────────────────
// `--max-diagnostics=N` configures the run-wide reporter. Before this type,
// that was the ONLY reporter it configured: ten front-end construction sites
// built their own reporters from `Config`'s in-class initializers and capped
// at 1000 regardless. ✔MEASURED at the CLI: a TU whose semantic tier stored
// 1400 diagnostics, compiled with `--max-diagnostics=1200` — and with
// `--max-diagnostics=100000` — printed `reporter cap of 1000 diagnostics
// reached; 60 further diagnostics were dropped`, byte-identical to the default
// run. The operator did exactly what the notice told them to do and nothing
// changed. Worse, the number in the message is whichever reporter capped
// FIRST (the marker's prose is frozen at that reporter's `cfg_` and then
// travels verbatim as a `Guaranteed` diagnostic), so it can be wrong in EITHER
// direction: a tier budgeted at 1200 under a run-wide default of 1000 reports
// `1200`.
//
// ★★ THE CONSEQUENCE THAT IS EASY TO RE-DERIVE WRONG, SO READ IT HERE FIRST:
// **A SUB-TIER'S GLOBAL CAP IS REACHABLE ONLY THROUGH TRAFFIC THAT BYPASSES
// THE PER-CODE CAP.** `DiagnosticReporter::report()` checks `maxPerCode` (50)
// BEFORE `maxDiagnostics` (1000), so an ordinary tier is bounded at
// (distinct codes × 50) and never approaches 1000. ✔MEASURED through the real
// CLI: 1400 illegal characters reach stderr as 100 diagnostics, 1400 malformed
// declarations as 53, 1400 `#warning` directives as 50 — every code sitting on
// exactly its per-code cap, and not one of them tripping a global cap at any
// flag value. The only traffic that grows `all_` past 1000 is what
// `mustDeliver` admits ahead of all four volume gates:
// `DiagnosticDelivery::Guaranteed` and `isUnsuppressable(code)` members.
// ⇒ **THE EXEMPTION THAT EXISTS SO A CLASS CAN NEVER BE SILENCED IS THE
// MECHANISM BY WHICH EVERYTHING ELSE IN ITS TIER GETS SILENCED.** ✔MEASURED:
// 1400 `S_StaticAssertFailed` (unsuppressable, 28× its per-code cap) filled a
// tier's budget and the 60 ordinary diagnostics that arrived next were dropped
// wholesale — by a cap the operator could not raise. That is why the budget
// carries `maxPerCode` as well as `maxDiagnostics`: raising only the global
// axis would leave the per-code axis as the thing that decides which
// diagnostics ever get to compete for it.
//
// ─────────────────────────────────────────────────────────────────────────
// WHY A DISTINCT TYPE RATHER THAN PASSING `DiagnosticReporter::Config`
// ─────────────────────────────────────────────────────────────────────────
//  * **`Config` is DEFAULT-CONSTRUCTIBLE, and that is precisely the defect.**
//    `Tokenizer` and `TreeBuilder` already took a `DiagnosticReporter::Config
//    diagConfig = {}` — a default argument that EVERY call site took, so the
//    parameter existed and reached nothing. `DiagnosticBudget` deletes its
//    default constructor and appears with NO default argument at any
//    construction site, so a future tier that forgets the budget is a COMPILE
//    ERROR rather than a silent revert to 1000.
//  * **Policy must NOT travel with the budget.** `Config` also carries
//    `policy` (suppress / severity overrides / `warningsAsErrors`), which is
//    applied at the DRAIN. Applying it a second time at a tier would change
//    control flow, not merely volume: `warningsAsErrors` would flip that
//    tier's own `hasErrors()` for a source that only warns, aborting a
//    pipeline stage that used to proceed. `asConfig()` therefore emits the
//    volume axes and an EMPTY policy, and the type has no policy member to
//    carry one by accident.
//
// The default keeps exactly ONE spelling: `libraryDefault()` is built from
// `DiagnosticReporter::Config{}`, so the in-class initializers there remain
// the sole source of truth and no site in the tree spells `1000` or `50`.
//
// Header-only by design — every member is inline, there is no out-of-line
// symbol, so no `DSS_EXPORT` and no new translation unit.
class DiagnosticBudget {
public:
    // Deleted, not defaulted: "I forgot to thread the budget" and "I want the
    // library defaults" must not be spelled the same way.
    DiagnosticBudget() = delete;

    explicit DiagnosticBudget(DiagnosticReporter::Config const& cfg) noexcept
        : maxDiagnostics_(cfg.maxDiagnostics),
          maxPerCode_(cfg.maxPerCode),
          dedupWindow_(cfg.dedupWindow) {}

    // The library's own defaults, named out loud. For callers that genuinely
    // have no operator budget to hand down — component tests exercising a tier
    // in isolation, and embedders with no command line. A PRODUCTION site on
    // the CLI path that calls this is the regression
    // `DiagnosticBudgetThreading.*` pins against: it compiles, so the guard is
    // the test, and the test names the site.
    [[nodiscard]] static DiagnosticBudget libraryDefault() noexcept {
        return DiagnosticBudget{DiagnosticReporter::Config{}};
    }

    // The tier-local reporter config: this budget's volume axes, EMPTY policy.
    [[nodiscard]] DiagnosticReporter::Config asConfig() const {
        DiagnosticReporter::Config cfg{};
        cfg.maxDiagnostics = maxDiagnostics_;
        cfg.maxPerCode     = maxPerCode_;
        cfg.dedupWindow    = dedupWindow_;
        return cfg;
    }

    // Same budget with the recent-duplicate window disabled. Not a general
    // knob: it exists because `UnitBuilder::driverDiagnostics_` has always
    // built itself with `dedupWindow = 0` deliberately — two different files
    // that are both missing produce diagnostics that hash alike, and a dedup
    // window would eat the second one. Threading the budget must not quietly
    // hand that reporter a window of 4, so the site states its own choice on
    // top of the operator's budget rather than inheriting one it never had.
    [[nodiscard]] DiagnosticBudget withoutDedup() const noexcept {
        DiagnosticBudget b{*this};
        b.dedupWindow_ = 0;
        return b;
    }

    [[nodiscard]] std::size_t maxDiagnostics() const noexcept { return maxDiagnostics_; }
    [[nodiscard]] std::size_t maxPerCode()     const noexcept { return maxPerCode_; }
    [[nodiscard]] std::size_t dedupWindow()    const noexcept { return dedupWindow_; }

    [[nodiscard]] friend bool operator==(DiagnosticBudget const& a,
                                         DiagnosticBudget const& b) noexcept {
        return a.maxDiagnostics_ == b.maxDiagnostics_
            && a.maxPerCode_     == b.maxPerCode_
            && a.dedupWindow_    == b.dedupWindow_;
    }

private:
    std::size_t maxDiagnostics_;
    std::size_t maxPerCode_;
    std::size_t dedupWindow_;
};

} // namespace dss
