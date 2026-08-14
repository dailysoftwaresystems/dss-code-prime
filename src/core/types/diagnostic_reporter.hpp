#pragma once

#include "core/export.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/strong_ids.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss {

// Per-code severity remapping + suppression. Configured once per
// compilation unit and applied inside DiagnosticReporter::report().
struct DSS_EXPORT DiagnosticPolicy {
    // Override the severity of specific codes (demote P_DeprecatedSyntax to Info,
    // promote P_AmbiguousToken to Error, etc.). Applied before suppress.
    std::unordered_map<DiagnosticCode, DiagnosticSeverity> overrides;

    // Drop these codes silently. Useful for codebases that legitimately
    // exercise a "warning" pattern the language config flags.
    std::unordered_set<DiagnosticCode> suppress;

    // Strict mode: every remaining Warning is promoted to Error after
    // overrides + suppress run. Applied last so explicit override demotions
    // still win.
    bool warningsAsErrors = false;
};

// Resolves BufferId → SourceBuffer. The reporter needs this for multi-
// buffer diagnostics (cross-file includes). For single-file callers,
// constructing a registry with one buffer is a one-liner.
class DSS_EXPORT BufferRegistry {
public:
    // Register a buffer; returns its id (same as buffer->id()). Idempotent.
    BufferId add(std::shared_ptr<SourceBuffer> buf);

    // Aborts if `id` is not registered. Use tryGet() when absence is
    // expected.
    [[nodiscard]] SourceBuffer const& get(BufferId id) const;

    // nullptr if not registered.
    [[nodiscard]] std::shared_ptr<SourceBuffer const> tryGet(BufferId id) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return byId_.size(); }

private:
    std::unordered_map<BufferId, std::shared_ptr<SourceBuffer>> byId_;
};

// Accumulator + formatter for ParseDiagnostics across every phase that
// touches a single compilation unit. Cap, coalesce, policy logic all
// live inside report() so callers can emit freely without worrying
// about producing 10 000 identical messages on a corrupted file.
class DSS_EXPORT DiagnosticReporter {
public:
    struct Config {
        std::size_t       maxDiagnostics = 1000;   // hard cap; overflow emits P_TooManyDiagnostics + stops
        std::size_t       maxPerCode     = 50;     // per-code cap; coalesces beyond this
        std::size_t       dedupWindow    = 4;      // identical (code, buffer, span, rule, actual) within this many recent diags is dropped
        DiagnosticPolicy  policy{};
    };

    DiagnosticReporter() noexcept = default;
    explicit DiagnosticReporter(Config cfg) noexcept;

    // Append a diagnostic. May be dropped (suppress), demoted/promoted
    // (overrides + warningsAsErrors), deduped against the recent window, or
    // coalesced beyond maxPerCode. Once maxDiagnostics is hit the reporter is
    // "capped": further Capped-delivery reports do not land, but they are
    // COUNTED and the count is carried in the P_TooManyDiagnostics marker's
    // text (see `noteCapDrop_`), so the elision is never silent about its
    // size. `DiagnosticDelivery::Guaranteed` diagnostics — and members of
    // `kUnsuppressableCodes` — bypass the cap/dedup gates entirely.
    void report(ParseDiagnostic d);

    [[nodiscard]] std::span<ParseDiagnostic const> all() const noexcept;
    [[nodiscard]] std::size_t errorCount()   const noexcept;
    [[nodiscard]] std::size_t warningCount() const noexcept;
    [[nodiscard]] bool        hasErrors()    const noexcept { return errorCount() > 0; }
    [[nodiscard]] bool        hitCap()       const noexcept { return hitCap_; }

    // How many diagnostics the GLOBAL cap has discarded. Zero until the cap
    // trips; from then on it counts every report the cap ate, INCLUDING the
    // one whose arrival tripped it (that diagnostic is replaced by the
    // marker, not stored). This is the number the marker's prose carries —
    // exposed as well because a caller that wants to say "and N more" in its
    // own summary should not have to parse the marker's text back out.
    [[nodiscard]] std::size_t droppedByCap() const noexcept { return droppedByCap_; }

    [[nodiscard]] Config const& config() const noexcept { return cfg_; }

    // Opaque restore token used exclusively by TreeBuilder::Checkpoint.
    // `recent_` is a sliding window with pop_front, so size-only capture
    // would lose front entries to speculative pushes; truncateTo would
    // then restore speculative residue rather than pre-checkpoint state.
    // Full-deque snapshot is the only mathematically sound shape.
    // Fields are private; only TreeBuilder reads them via friendship.
    class DSS_EXPORT Snapshot {
    private:
        friend class DiagnosticReporter;
        std::size_t                                       allSize     = 0;
        std::unordered_map<DiagnosticCode, std::size_t>   perCode;
        std::deque<std::uint64_t>                         recent;
        bool                                              hitCap      = false;
        // The cap's two derived fields travel with `hitCap` because they
        // are only meaningful together. `capMarkerIndex` in particular is
        // an index INTO `all_`, which `truncateTo` shrinks — restoring
        // `hitCap` without it would leave the marker index dangling past
        // the end and the next cap drop would write out of bounds.
        std::size_t                                       droppedByCap    = 0;
        std::size_t                                       capMarkerIndex  = 0;
    };
    [[nodiscard]] Snapshot snapshotForRollback() const;
    void                   truncateTo(Snapshot const& snap);

    // Append a diagnostic bypassing the global cap and dedup window.
    // Reserved for builder-invariant signals that the cap MUST NOT
    // silently swallow (forgotten-commit warning, internal-error
    // notifications). `policy` (suppress/override/warningsAsErrors)
    // still applies — bypass concerns only the cap, not the user's
    // explicit filtering choices. Per-code counters still increment.
    //
    // This is `DiagnosticDelivery::Guaranteed` under a caller-friendly
    // name, and it is now implemented as exactly that rather than as a
    // second body that pushes to `all_` on its own. The duplicate body was
    // a standing hazard: it was the SECOND place the "policy applies,
    // volume controls do not" rule was written down, so the two could
    // disagree and only one of them would be the one a given caller hit.
    void forceReport(ParseDiagnostic d);

    // Rewrite the (buffer, span) of EVERY accumulated diagnostic through
    // `fn`. The C preprocessor pass (FC13) uses this to remap diagnostics
    // off the synthesized buffer back onto the real header/main file via its
    // line-map: `fn` inspects the buffer id and, when it is the synth buffer,
    // overwrites both the buffer id and the span with the resolved origin.
    // `fn` is invoked once per diagnostic with mutable references; a no-op
    // `fn` (or one that leaves non-synth diagnostics untouched) is harmless.
    // Only `buffer`/`span` are mutable -- code/severity/message are
    // unchanged. The recent-duplicate hash window is NOT rebuilt (it tracks
    // already-admitted diagnostics; remap runs after admission).
    template <class F>
    void remapBuffers(F&& fn) {
        for (ParseDiagnostic& d : all_) fn(d.buffer, d.span);
    }

    // Pretty-printers. The registry resolves BufferId → SourceBuffer so
    // multi-file diagnostics (related-locations spanning includes, future)
    // format correctly.
    [[nodiscard]] std::string formatAll(BufferRegistry const& bufs) const;
    [[nodiscard]] std::string format(ParseDiagnostic const& d,
                                     BufferRegistry const& bufs) const;

private:
    // Apply policy in this order: suppress → override → warningsAsErrors.
    // Returns std::nullopt if the diagnostic should be dropped.
    [[nodiscard]] std::optional<ParseDiagnostic> applyPolicy(ParseDiagnostic d) const;

    [[nodiscard]] bool isRecentDuplicate(ParseDiagnostic const& d) const noexcept;

    // Record that the global cap ate one more diagnostic, and REWRITE the
    // marker's prose so it always states the running total. The rewrite is
    // in place into the marker's existing string, so after the first few
    // drops it reallocates nothing — which is what makes "count on the
    // already-dropping path" affordable enough to do unconditionally
    // instead of leaving the number to be recovered later (it cannot be:
    // once a diagnostic is dropped there is nothing left to count).
    void noteCapDrop_();

    Config                                  cfg_{};
    std::vector<ParseDiagnostic>            all_;
    std::unordered_map<DiagnosticCode, std::size_t> perCode_;
    std::deque<std::uint64_t>               recent_;      // hash of (code, buffer, span, rule, actual) sliding window
    bool                                    hitCap_ = false;
    std::size_t                             droppedByCap_   = 0;
    std::size_t                             capMarkerIndex_ = 0;  // index into all_; valid only while hitCap_
};

// Ergonomic free function for the common "construct a ParseDiagnostic
// with code/severity/actual + dispatch through the reporter" shape.
// Hoisted out of `lir/lir_pass_util.{hpp,cpp}` at LK10 cycle 3 post-fold
// #2 (D-LK10-8 closure): the shim is intrinsically tier-agnostic (it
// walks no LIR types) and was forcing driver-tier consumers like
// `program/input_resolver.cpp` to #include LIR headers for a one-line
// helper. Now lives at the canonical home so every tier — LIR, AS,
// link, driver — imports from the same place. Inline-defined so no
// new translation unit is required.
inline void report(DiagnosticReporter& reporter, DiagnosticCode code,
                   DiagnosticSeverity severity, std::string actual) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = severity;
    d.actual   = std::move(actual);
    reporter.report(std::move(d));
}

} // namespace dss
