#include "core/types/diagnostic_reporter.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

namespace dss {

// ─────────────────────────────────────────────────────────────────────────
// BufferRegistry
// ─────────────────────────────────────────────────────────────────────────

BufferId BufferRegistry::add(std::shared_ptr<SourceBuffer> buf) {
    if (!buf) {
        throw std::invalid_argument("BufferRegistry::add: null buffer");
    }
    const BufferId id = buf->id();
    byId_[id] = std::move(buf);
    return id;
}

SourceBuffer const& BufferRegistry::get(BufferId id) const {
    auto it = byId_.find(id);
    if (it == byId_.end()) {
        throw std::out_of_range(
            std::format("BufferRegistry::get: BufferId {} not registered", id.v));
    }
    return *it->second;
}

std::shared_ptr<SourceBuffer const> BufferRegistry::tryGet(BufferId id) const noexcept {
    auto it = byId_.find(id);
    return (it == byId_.end()) ? nullptr : it->second;
}

// ─────────────────────────────────────────────────────────────────────────
// DiagnosticReporter
// ─────────────────────────────────────────────────────────────────────────

DiagnosticReporter::DiagnosticReporter(Config cfg) noexcept : cfg_(std::move(cfg)) {}

std::span<ParseDiagnostic const> DiagnosticReporter::all() const noexcept {
    return std::span<ParseDiagnostic const>{all_.data(), all_.size()};
}

std::size_t DiagnosticReporter::errorCount() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        all_, [](ParseDiagnostic const& d) { return d.severity == DiagnosticSeverity::Error; }));
}

std::size_t DiagnosticReporter::warningCount() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        all_, [](ParseDiagnostic const& d) { return d.severity == DiagnosticSeverity::Warning; }));
}

std::optional<ParseDiagnostic> DiagnosticReporter::applyPolicy(ParseDiagnostic d) const {
    if (cfg_.policy.suppress.contains(d.code)) {
        return std::nullopt;
    }
    if (auto it = cfg_.policy.overrides.find(d.code); it != cfg_.policy.overrides.end()) {
        d.severity = it->second;
    }
    if (cfg_.policy.warningsAsErrors && d.severity == DiagnosticSeverity::Warning) {
        d.severity = DiagnosticSeverity::Error;
    }
    return d;
}

namespace {
// FNV-1a 64-bit on (code, buffer, span.start, span.end). The reporter's
// dedup window only needs a low-collision identifier, not a cryptographic
// hash — false positives become false drops, which the cap behaviour
// already absorbs.
constexpr std::uint64_t fnv1a64(std::uint64_t seed, std::uint64_t v) noexcept {
    constexpr std::uint64_t prime = 1099511628211ull;
    for (int i = 0; i < 8; ++i) {
        seed ^= (v >> (i * 8)) & 0xFFu;
        seed *= prime;
    }
    return seed;
}

std::uint64_t hashKey(ParseDiagnostic const& d) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ull;
    auto h = fnv1a64(offset, static_cast<std::uint64_t>(d.code));
    h = fnv1a64(h, static_cast<std::uint64_t>(d.buffer.v));
    h = fnv1a64(h, static_cast<std::uint64_t>(d.span.start()));
    h = fnv1a64(h, static_cast<std::uint64_t>(d.span.end()));
    // Include the rule context so per-frame EOF diagnostics (which share
    // a span but have distinct ruleContexts) don't dedup-collapse against
    // each other.
    h = fnv1a64(h, d.ruleContext ? static_cast<std::uint64_t>(d.ruleContext->v) : 0u);
    return h;
}
} // namespace

bool DiagnosticReporter::isRecentDuplicate(ParseDiagnostic const& d) const noexcept {
    if (cfg_.dedupWindow == 0) return false;
    const auto key = hashKey(d);
    return std::ranges::find(recent_, key) != recent_.end();
}

void DiagnosticReporter::report(ParseDiagnostic d) {
    if (hitCap_) return;

    auto filtered = applyPolicy(std::move(d));
    if (!filtered) return;

    if (isRecentDuplicate(*filtered)) {
        return;
    }

    auto& counts = perCode_[filtered->code];
    if (counts >= cfg_.maxPerCode) {
        // Per-code cap: silently coalesce. We don't emit a marker here
        // because per-code coalescing is the normal mode of operation on
        // noisy passes (e.g. P_UnknownToken on a corrupted file); the
        // *global* cap below is what gets the visible marker.
        return;
    }

    if (all_.size() >= cfg_.maxDiagnostics) {
        hitCap_ = true;
        ParseDiagnostic marker{};
        marker.code     = DiagnosticCode::P_TooManyDiagnostics;
        marker.severity = DiagnosticSeverity::Error;
        marker.buffer   = filtered->buffer;
        marker.span     = filtered->span;
        marker.actual   = std::format(
            "reporter cap of {} diagnostics reached; further reports dropped",
            cfg_.maxDiagnostics);
        all_.push_back(std::move(marker));
        return;
    }

    ++counts;
    const auto key = hashKey(*filtered);
    recent_.push_back(key);
    if (recent_.size() > cfg_.dedupWindow) {
        recent_.pop_front();
    }
    all_.push_back(std::move(*filtered));
}

// ─────────────────────────────────────────────────────────────────────────
// Formatting
// ─────────────────────────────────────────────────────────────────────────
//
// Output shape (per plan §5.13):
//
//   error[P0001]: expected ';' or ',' — got '}'
//     --> src/foo.exl:14:23
//      |
//   14 |    var x = 1 + 2 }
//      |                  ^ unexpected token
//      |
//   note: matching opener at src/foo.exl:12:9
//   12 |    var x = (
//      |            ^ scope opened here
//      |
//   scope: Root > Block > Paren
//   hint:  insert ';' before this token
//
// ASCII pipes/arrows chosen over the box-drawing variants in the plan
// sketch for terminal-portability — `--*` and `|` render correctly even
// in stripped CI logs and on terminals without UTF-8.

namespace {

// Return the text of the single line containing `byteOffset`, plus its
// 1-based line number, *without* the trailing newline.
struct LineView {
    std::uint32_t line   = 0;
    std::uint32_t column = 0;
    std::string_view text;
};

LineView extractLine(SourceBuffer const& buf, std::uint32_t byteOffset) {
    const auto lc = buf.lineCol(byteOffset);
    const auto src = buf.text();
    // Find the line's start: walk back from byteOffset to the previous '\n'
    // (or position 0). The lineCol() column already tells us how far in we
    // are within the line, but we still need the byte index of the line
    // start to slice the source.
    std::uint32_t lineStart = byteOffset;
    while (lineStart > 0 && src[lineStart - 1] != '\n') {
        --lineStart;
    }
    std::uint32_t lineEnd = lineStart;
    while (lineEnd < src.size() && src[lineEnd] != '\n' && src[lineEnd] != '\r') {
        ++lineEnd;
    }
    return LineView{
        .line   = lc.line,
        .column = lc.column,
        .text   = src.substr(lineStart, lineEnd - lineStart),
    };
}

void appendLineWithCaret(std::string& out,
                         SourceBuffer const& buf,
                         SourceSpan span,
                         std::string_view caretNote) {
    const auto lv = extractLine(buf, span.start());

    // Header: --> name:line:col
    std::format_to(std::back_inserter(out),
                   "  --> {}:{}:{}\n",
                   buf.name(), lv.line, lv.column);
    std::format_to(std::back_inserter(out), "   |\n");

    // Source line.
    std::format_to(std::back_inserter(out),
                   "{:>2} | {}\n", lv.line, lv.text);

    // Caret line. Column is 1-based; the `|` gutter prints "<space> | "
    // (5 chars) so the caret column inside the line text starts after
    // those 5 chars. column-1 leading spaces, then ^.
    const std::size_t pad = (lv.column == 0 ? 0u : lv.column - 1u);
    std::format_to(std::back_inserter(out),
                   "   | {:>{}}^",
                   "", pad);
    if (!caretNote.empty()) {
        std::format_to(std::back_inserter(out), " {}", caretNote);
    }
    out += "\n   |\n";
}

void appendExpectedActual(std::string& out, ParseDiagnostic const& d) {
    if (!d.expected.empty()) {
        std::format_to(std::back_inserter(out), "expected ");
        for (std::size_t i = 0; i < d.expected.size(); ++i) {
            if (i > 0) {
                out += (i + 1 == d.expected.size()) ? " or " : ", ";
            }
            out += d.expected[i];
        }
        if (!d.actual.empty()) {
            std::format_to(std::back_inserter(out), " — got {}", d.actual);
        }
    } else if (!d.actual.empty()) {
        std::format_to(std::back_inserter(out), "got {}", d.actual);
    } else {
        out += diagnosticCodeName(d.code);
    }
}

void appendScopeStack(std::string& out, ParseDiagnostic const& d) {
    if (d.scopeStack.empty()) return;
    out += "scope: ";
    for (std::size_t i = 0; i < d.scopeStack.size(); ++i) {
        if (i > 0) out += " > ";
        out += scopeName(d.scopeStack[i]);
    }
    out += '\n';
}

} // namespace

std::string DiagnosticReporter::format(ParseDiagnostic const& d,
                                       BufferRegistry const& bufs) const {
    std::string out;

    // Header line: severity[prefix]: expected/actual prose
    std::format_to(std::back_inserter(out),
                   "{}[{}]: ",
                   severityName(d.severity),
                   diagnosticCodePrefix(d.code));
    appendExpectedActual(out, d);
    out += '\n';

    // Primary location + caret + source line, if we can resolve the buffer.
    if (auto buf = bufs.tryGet(d.buffer)) {
        appendLineWithCaret(out, *buf, d.span, "");
    } else {
        std::format_to(std::back_inserter(out),
                       "  --> <unknown-buffer:{}>:offset {}\n   |\n",
                       d.buffer.v, d.span.start());
    }

    // Related locations.
    for (auto const& rel : d.related) {
        std::format_to(std::back_inserter(out), "note: {}\n", rel.note);
        if (auto buf = bufs.tryGet(rel.buffer)) {
            appendLineWithCaret(out, *buf, rel.span, "");
        } else {
            std::format_to(std::back_inserter(out),
                           "  --> <unknown-buffer:{}>:offset {}\n   |\n",
                           rel.buffer.v, rel.span.start());
        }
    }

    appendScopeStack(out, d);

    if (!d.suggestion.empty()) {
        std::format_to(std::back_inserter(out), "hint:  {}\n", d.suggestion);
    }

    return out;
}

std::string DiagnosticReporter::formatAll(BufferRegistry const& bufs) const {
    // Sort by (buffer, span, severity) so the rendered output reads in
    // source order regardless of report-call order. We render into a
    // copy of the index list to keep all_ stable.
    std::vector<std::size_t> order(all_.size());
    std::iota(order.begin(), order.end(), 0u);
    std::ranges::sort(order, [this](std::size_t a, std::size_t b) {
        auto const& da = all_[a];
        auto const& db = all_[b];
        if (da.buffer != db.buffer) return da.buffer < db.buffer;
        if (da.span   != db.span)   return da.span   < db.span;
        return static_cast<int>(da.severity) > static_cast<int>(db.severity);
    });

    std::string out;
    for (std::size_t idx : order) {
        out += format(all_[idx], bufs);
        out += '\n';
    }
    return out;
}

} // namespace dss
