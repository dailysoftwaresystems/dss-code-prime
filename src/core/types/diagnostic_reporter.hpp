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
        std::size_t       dedupWindow    = 4;      // identical (code, buffer, span) within this many recent diags is dropped
        DiagnosticPolicy  policy{};
    };

    DiagnosticReporter() noexcept = default;
    explicit DiagnosticReporter(Config cfg) noexcept;

    // Append a diagnostic. May be silently dropped (suppress), demoted/
    // promoted (overrides + warningsAsErrors), deduped against the recent
    // window, or coalesced beyond maxPerCode. Once maxDiagnostics is hit
    // the reporter is "capped" and only the single P_TooManyDiagnostics
    // marker is appended; further report() calls are no-ops.
    void report(ParseDiagnostic d);

    [[nodiscard]] std::span<ParseDiagnostic const> all() const noexcept;
    [[nodiscard]] std::size_t errorCount()   const noexcept;
    [[nodiscard]] std::size_t warningCount() const noexcept;
    [[nodiscard]] bool        hasErrors()    const noexcept { return errorCount() > 0; }
    [[nodiscard]] bool        hitCap()       const noexcept { return hitCap_; }

    [[nodiscard]] Config const& config() const noexcept { return cfg_; }

    // ── Snapshot / restore for speculative rollback ───────────────────
    //
    // Opaque-by-convention. The caller (TreeBuilder::Checkpoint) takes
    // a Snapshot via `snapshotForRollback`, runs speculative work, and
    // either discards the Snapshot (commit) or passes it to `truncateTo`
    // (rollback). The Snapshot is only meaningful for the reporter
    // instance it came from; passing it to a different reporter or
    // using it after non-monotonic reporter mutations is undefined.
    //
    // hitCap_ is a one-way latch in normal operation. Rollback re-opens
    // it so speculative branches that tripped the cap don't permanently
    // silence the post-rollback reporter.
    struct Snapshot {
        std::size_t                                       allSize;
        std::unordered_map<DiagnosticCode, std::size_t>   perCode;
        std::size_t                                       recentSize;
        bool                                              hitCap;
    };
    [[nodiscard]] Snapshot snapshotForRollback() const;
    void                   truncateTo(Snapshot const& snap);

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

    Config                                  cfg_{};
    std::vector<ParseDiagnostic>            all_;
    std::unordered_map<DiagnosticCode, std::size_t> perCode_;
    std::deque<std::uint64_t>               recent_;      // hash of (code, buffer, span) sliding window
    bool                                    hitCap_ = false;
};

} // namespace dss
