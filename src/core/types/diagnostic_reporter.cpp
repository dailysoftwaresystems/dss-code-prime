#include "core/types/diagnostic_reporter.hpp"

#include "core/types/unsuppressable_codes.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

void BufferRegistry::addAll(BufferRegistry const& other) {
    // Self-merge is a no-op rather than an iterator hazard: `byId_[id] = ...`
    // over the container being iterated would rehash mid-walk.
    if (&other == this) return;
    for (auto const& [id, buf] : other.byId_) {
        if (buf) byId_[id] = buf;
    }
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

DiagnosticReporter::Snapshot DiagnosticReporter::snapshotForRollback() const {
    Snapshot s;
    s.allSize        = all_.size();
    s.perCode        = perCode_;
    s.recent         = recent_;
    s.hitCap         = hitCap_;
    s.droppedByCap   = droppedByCap_;
    s.capMarkerIndex = capMarkerIndex_;
    return s;
}

void DiagnosticReporter::forceReport(ParseDiagnostic d) {
    // The property IS the mechanism — see `DiagnosticDelivery`. Writing the
    // push here a second time is what let the two paths drift apart before.
    d.delivery = DiagnosticDelivery::Guaranteed;
    report(std::move(d));
}

void DiagnosticReporter::truncateTo(Snapshot const& snap) {
    // Restore visible stream length.
    if (snap.allSize < all_.size()) {
        all_.resize(snap.allSize);
    }
    // perCode_, recent_, and hitCap_ are restored wholesale. recent_
    // CANNOT be back-truncated by size alone because pop_front during
    // speculation may have evicted original entries; only the full
    // snapshot is mathematically sound.
    perCode_        = snap.perCode;
    recent_         = snap.recent;
    hitCap_         = snap.hitCap;
    droppedByCap_   = snap.droppedByCap;
    capMarkerIndex_ = snap.capMarkerIndex;
}

std::optional<ParseDiagnostic> DiagnosticReporter::applyPolicy(ParseDiagnostic d) const {
    // D-FF2-UNSUPP refined contract (eb2c6c7 audit-fold 2026-06-01):
    // unsuppressable codes bypass SILENCING (`--suppress` drops +
    // `overrides` demotion) so they always reach `all_`. Elevation
    // (`--warnings-as-errors`) applies UNIFORMLY — it strengthens the
    // signal, never silences it. Strict-mode operators legitimately
    // want Warning-severity unsuppressable codes (like
    // F_BinaryReaderPartialCorruption) promoted to Error so they
    // increment errorCount.
    //
    // Shape note (eb2c6c7 audit-fold): the previously-duplicated
    // warningsAsErrors flip was collapsed into a single end-of-
    // function block. Two literal copies were drift-prone (one in
    // the unsuppressable arm, one after silencing); the single-flip
    // shape makes "elevation is universal; only silencing is gated
    // by unsuppressable" the literal control flow.
    if (!isUnsuppressable(d.code)) {
        if (cfg_.policy.suppress.contains(d.code)) {
            return std::nullopt;
        }
        if (auto it = cfg_.policy.overrides.find(d.code); it != cfg_.policy.overrides.end()) {
            d.severity = it->second;
        }
    }
    if (cfg_.policy.warningsAsErrors && d.severity == DiagnosticSeverity::Warning) {
        d.severity = DiagnosticSeverity::Error;
    }
    return d;
}

namespace {
// FNV-1a 64-bit on (code, buffer, span.start, span.end, ruleContext, actual).
// The reporter's dedup window only needs a low-collision identifier, not a
// cryptographic hash — false positives become false drops, which the cap
// behaviour already absorbs.
constexpr std::uint64_t fnv1a64(std::uint64_t seed, std::uint64_t v) noexcept {
    constexpr std::uint64_t prime = 1099511628211ull;
    for (int i = 0; i < 8; ++i) {
        seed ^= (v >> (i * 8)) & 0xFFu;
        seed *= prime;
    }
    return seed;
}

std::uint64_t fnv1a64Bytes(std::uint64_t seed, std::string const& s) noexcept {
    constexpr std::uint64_t prime = 1099511628211ull;
    for (unsigned char c : s) {
        seed ^= c;
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
    // Include `actual` so diagnostics that share (code, buffer, span, rule) but
    // carry DIFFERENT detail are not coalesced — they convey distinct
    // information: a different missing-file path for D_FileNotFound, or a
    // different node id for an H_* verifier finding whose nodes have no source
    // span (and so all share the empty span). True duplicates carry identical
    // `actual`, so the window still collapses them. (Without this, distinct such
    // findings collide on the key — the deficiency UnitBuilder's driver reporter
    // sidesteps by disabling dedup wholesale.)
    h = fnv1a64Bytes(h, d.actual);
    // d.delivery is INTENTIONALLY excluded: it is a routing decision about
    // this REPORT, not a property of the fact being reported. Two emit sites
    // describing the same fact at the same place must still collapse.
    // d.contextPrefix is INTENTIONALLY excluded — see ParseDiagnostic
    // field comment + D-MERGE-DEDUP-PREFIX-COLLISION fold. Multi-
    // target merge stamps a per-target prefix at the merge point; two
    // targets emitting the structurally-identical diagnostic must
    // collapse at the destination rather than leak through as
    // duplicates-with-different-prefix.
    return h;
}

// THE delivery question, asked in exactly one place: may the reporter's
// VOLUME controls (dedup window, per-code cap, global cap) drop this?
//
// Two independent sources say no, and they answer DIFFERENT questions:
//   * `isUnsuppressable(code)` answers SUPPRESSION — "the user must not be
//     able to silence this". Bypassing the volume gates is a CONSEQUENCE of
//     what such a code is (its emission gates ok / errorCount, so it cannot
//     be allowed to vanish by any route), not the reason it is a member.
//   * `DiagnosticDelivery::Guaranteed` answers DELIVERY directly, for the
//     diagnostics that need only that. Before it existed they had to join
//     the closed table to obtain it, which used membership as a side-channel
//     for a property the table was never defined to carry — and paid for it
//     by loosening the table's one criterion.
[[nodiscard]] bool mustDeliver(ParseDiagnostic const& d) noexcept {
    return d.delivery == DiagnosticDelivery::Guaranteed
        || isUnsuppressable(d.code);
}

// House-style fail-loud sink (see `lineSliceFatal` below). Unreachable by
// construction — `capMarkerIndex_` is written only in the same block that
// sets `hitCap_`, read only while `hitCap_` holds, and restored together
// with it by `truncateTo` — so this exists to make a future edit that
// breaks that pairing abort with its name on it, rather than write past the
// end of `all_` and corrupt the diagnostic stream it was trying to explain.
[[noreturn]] void capMarkerFatal(std::size_t index, std::size_t size) {
    std::fputs("dss::DiagnosticReporter fatal: cap-marker index out of range "
               "(hitCap_ is set but capMarkerIndex_ does not address a stored "
               "diagnostic; this is a compiler bug, not a source error)\n",
               stderr);
    std::fprintf(stderr, "  capMarkerIndex = %zu, all_.size() = %zu\n",
                 index, size);
    std::abort();
}
} // namespace

void DiagnosticReporter::noteCapDrop_() {
    ++droppedByCap_;
    if (capMarkerIndex_ >= all_.size()) {
        capMarkerFatal(capMarkerIndex_, all_.size());
    }
    // ★ THE ELISION MUST STATE ITS OWN SIZE AND ITS OWN REMEDY.
    // "further reports dropped" — the prose this replaced — tells an
    // operator that they are missing something but not whether it is one
    // diagnostic or ten thousand, and not what to change to see it. Those
    // are the two facts that decide what they do next, and the count in
    // particular cannot be recovered afterwards by anyone: a dropped
    // diagnostic leaves nothing behind to count.
    //
    // Rewritten IN PLACE on every drop rather than computed once at the
    // end, because there is no "end" — `all()` is readable at any moment
    // and must never be observed carrying a stale number. `clear()` keeps
    // the string's capacity, so once the count reaches its digit width this
    // stops allocating entirely.
    //
    // ⚠ NAMES BOTH THE CLI FLAG AND THE CONFIG FIELD, AND THE ORDER IS THE
    // POINT. An earlier revision of this block named only the field and said
    // so explicitly, on the correct ground that `--max-diagnostics` DID NOT
    // EXIST and that naming a flag which parses but changes nothing would be a
    // worse defect than the one being fixed. The flag now exists and is wired
    // end-to-end — `program/cli_args.cpp` parses it, `buildReporterConfig`
    // writes it onto this Config, `Program::run` hands that Config to every
    // compile-producing entry point — so the ground for the omission is gone,
    // and leaving the old note in place would make this comment the lie.
    //
    // Why both, given `core/` sits BELOW the driver and is also consumed by the
    // LSP and by embedders, for whom the flag means nothing:
    //   * The flag comes FIRST because the reader is overwhelmingly an operator
    //     at a terminal, and a remedy they cannot type is not a remedy.
    //   * The field comes SECOND because it is true for EVERY consumer,
    //     including the two the flag cannot serve. Each clause names its own
    //     audience, so neither reads as false to the other's reader.
    //   * The alternative — keep this string flag-agnostic and let the CLI
    //     append the hint at its drain site — was rejected on measurement, not
    //     taste: the marker inherits the tripping diagnostic's buffer/span, so
    //     it renders through BOTH `format()` (positioned) and the driver's
    //     buffer-less one-liner. Appending at the drain means composing the
    //     operator-visible sentence in two tiers across two render routes, i.e.
    //     re-creating the second source of truth this whole area exists to
    //     remove, plus a per-code branch in the driver.
    // This is a documentation claim, not a behavioural one: nothing here reads
    // driver state or branches on a consumer identity.
    //
    // ⚠ PURE ASCII, deliberately, and this file already made the same call
    // for the same reason: "ASCII pipes/arrows chosen over the box-drawing
    // variants in the plan sketch for terminal-portability" (see the
    // formatting preamble below). There is a second, sharper reason here —
    // this exact string is byte-compared by a test, no `/utf-8` is passed to
    // MSVC anywhere in the build (grep the CMake), and a non-ASCII literal
    // whose encoding depends on the compiler's idea of the source charset is
    // a cross-toolchain flake waiting for the next CI leg.
    std::string& text = all_[capMarkerIndex_].actual;
    text.clear();
    std::format_to(std::back_inserter(text),
                   "reporter cap of {} diagnostics reached; {} further {} "
                   "dropped and NOT shown. Raise the cap above {} to see {}: "
                   "pass --max-diagnostics=N on the command line, or set "
                   "DiagnosticReporter::Config::maxDiagnostics when embedding.",
                   cfg_.maxDiagnostics,
                   droppedByCap_,
                   droppedByCap_ == 1 ? "diagnostic was" : "diagnostics were",
                   cfg_.maxDiagnostics,
                   droppedByCap_ == 1 ? "it" : "them");
}

bool DiagnosticReporter::isRecentDuplicate(ParseDiagnostic const& d) const noexcept {
    if (cfg_.dedupWindow == 0) return false;
    const auto key = hashKey(d);
    return std::ranges::find(recent_, key) != recent_.end();
}

void DiagnosticReporter::report(ParseDiagnostic d) {
    // Policy first so the unsuppressable check below sees the post-
    // policy code. See `applyPolicy` above for the canonical
    // silencing-vs-elevation contract.
    auto filtered = applyPolicy(std::move(d));
    if (!filtered) return;

    // THE DELIVERY GATE. Bypasses ALL FOUR volume gates below (hitCap_ /
    // dedupWindow / maxPerCode / maxDiagnostics), each of which otherwise
    // drops the diagnostic — re-opening the silent-failure surface that
    // every unsuppressable code, and every Guaranteed-delivery emit site,
    // exists to close. perCode_ is still incremented so accounting stays
    // consistent; dedup/recent_ is skipped (we want every instance
    // counted). See `mustDeliver` above for why the predicate has two
    // arms and why they are not the same question.
    if (mustDeliver(*filtered)) {
        ++perCode_[filtered->code];
        all_.push_back(std::move(*filtered));
        return;
    }

    if (hitCap_) {
        // Counted, not merely discarded: this is the one path on which the
        // information is lost forever, so it is the one path that has to
        // leave a number behind. One increment plus an in-place rewrite on
        // a path that was already throwing the diagnostic away.
        noteCapDrop_();
        return;
    }

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
        // Guaranteed for the same reason every other guaranteed diagnostic
        // is: it is the ONLY statement of its fact. It is ALSO pushed
        // directly rather than re-entered through `report`, which is what
        // makes it un-`--suppress`-able — the notice that diagnostics were
        // hidden must not itself be hideable, or the cap becomes silent
        // again through the front door. The field is set so a reader of
        // this record sees the same delivery contract every other
        // guaranteed diagnostic carries, and so a future refactor that DOES
        // route the marker through `report` keeps the guarantee.
        marker.delivery = DiagnosticDelivery::Guaranteed;
        marker.buffer   = filtered->buffer;
        marker.span     = filtered->span;
        capMarkerIndex_ = all_.size();
        all_.push_back(std::move(marker));
        // The diagnostic that TRIPPED the cap is dropped too — the marker
        // stands in its place, it is not stored alongside it — so it is the
        // first thing the count counts.
        noteCapDrop_();
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

// TF-C80: house-style fail-loud sink for the renderer (see `bufferFatal` in
// source_buffer.cpp). Deliberately NOT <cassert> — this must fire identically
// in release builds, where the crash it replaces was observed.
[[noreturn]] void lineSliceFatal(std::size_t srcSize,
                                 std::uint32_t lineStart,
                                 std::uint32_t lineEnd) {
    std::fputs("dss::DiagnosticReporter fatal: line slice out of range "
               "(the diagnostic renderer computed an inconsistent line "
               "window; this is a compiler bug, not a source error)\n", stderr);
    std::fprintf(stderr, "  buffer size = %zu, lineStart = %u, lineEnd = %u\n",
                 srcSize, lineStart, lineEnd);
    std::abort();
}

// Return the text of the single line containing `byteOffset`, plus its
// 1-based line number, *without* the trailing newline.
struct LineView {
    std::uint32_t line   = 0;
    std::uint32_t column = 0;
    std::string_view text;
};

// TF-C80. The renderer's ONE substring cut, bounds-checked and fail-LOUD.
//
// This replaces a raw `src.substr(lineStart, lineEnd - lineStart)`. A raw
// `std::string_view::substr` with `pos > size()` throws `std::out_of_range`,
// and nothing on the diagnostic-rendering path catches it — so the compiler
// died via `libc++abi: terminating due to uncaught exception` / SIGABRT with
// NO diagnostic text at all. That silently bypassed the project's fail-loud
// discipline: the operator saw an "Abort trap: 6", not an error report.
//
// A diagnostic renderer must never be the thing that kills the compile, so the
// contract is enforced HERE with a named, greppable message instead of an
// anonymous libc++ abort. Reaching `lineSliceFatal` means a CALLER computed an
// inconsistent line window — a compiler bug — and the operator gets told which
// invariant broke and with what numbers, not a bare trap.
[[nodiscard]] std::string_view checkedLineSlice(std::string_view src,
                                                std::uint32_t lineStart,
                                                std::uint32_t lineEnd) {
    if (lineStart > lineEnd || lineEnd > src.size()) {
        lineSliceFatal(src.size(), lineStart, lineEnd);
    }
    return src.substr(lineStart, lineEnd - lineStart);
}

LineView extractLine(SourceBuffer const& buf, std::uint32_t byteOffset) {
    const auto lc = buf.lineCol(byteOffset);
    const auto src = buf.text();

    // TF-C80: CLAMP BEFORE THE WALK. `byteOffset` is NOT guaranteed to be a
    // real offset into `buf`: the preprocessor deliberately mints macro-
    // expansion PRODUCT tokens whose spans start at `buf.text().size() +
    // productOffset` — past end-of-buffer by construction (see
    // preprocessor.cpp `sbTextOf` / `text(Token const&)`, which decode that
    // encoding). When such a token is the subject of a parse error, its span
    // reaches this renderer verbatim.
    //
    // Unclamped, the backward walk below read `src[lineStart - 1]` for
    // lineStart > size() — a HEAP OVER-READ of (byteOffset - size()) bytes
    // past the buffer (MEASURED: up to 67 bytes past a 50-byte file for
    // `#include <_stdio.h>`). It then threw `std::out_of_range:
    // string_view::substr` — an UNCAUGHT exception, i.e. SIGABRT with no
    // diagnostic — whenever a `'\n'` (0x0A) happened to sit in that recycled-
    // heap window, making the abort NON-DETERMINISTIC run to run.
    //
    // `SourceBuffer::lineCol` above ALREADY clamps (`std::min(byteOffset,
    // size())`); this line restores the parity that was missing here, so a
    // past-end span renders the last line rather than corrupting memory.
    const std::uint32_t off = std::min<std::uint32_t>(
        byteOffset, static_cast<std::uint32_t>(src.size()));

    // Find the line's start: walk back from `off` to the previous '\n'
    // (or position 0). The lineCol() column already tells us how far in we
    // are within the line, but we still need the byte index of the line
    // start to slice the source.
    std::uint32_t lineStart = off;
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
        .text   = checkedLineSlice(src, lineStart, lineEnd),
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

    // Underline: column-1 leading spaces, then `^` repeated for the
    // span's byte length (clamped to whatever's left on the line — a
    // multi-line span underlines only the portion on the first line).
    // Empty spans render a single `^` (the smallest sensible cursor).
    const std::size_t pad = (lv.column == 0 ? 0u : lv.column - 1u);
    const std::size_t lineRemainder =
        (pad >= lv.text.size()) ? 0u : (lv.text.size() - pad);
    const std::size_t requestedLen =
        (span.end() > span.start()) ? (span.end() - span.start()) : 1u;
    const std::size_t caretLen = std::max<std::size_t>(
        1u, std::min(requestedLen, lineRemainder));
    std::format_to(std::back_inserter(out),
                   "   | {:>{}}{:^>{}}",
                   "", pad, "", caretLen);
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
        // ★★★ NO `expected` MEANS NOTHING TO CONTRAST WITH, SO THERE IS NO "got".
        // `got X` is half of the pair `expected Y — got X`; printed alone it is a
        // sentence fragment, and for the SEMANTIC band it is an ungrammatical one:
        // those codes carry a full prose sentence in `actual` and leave `expected`
        // empty by design, so the render used to read
        //   error[S006A]: got the inline-asm template references operand `%3` …
        // ⚠ The condition is on `expected`, NOT on the code's band. Keying it on
        // the band would be a second owner of "is this a contrast?" that could
        // disagree with the field the renderer actually reads
        // (D-DIAG-PROSE-MESSAGE-RENDERS-WITH-A-BARE-GOT-PREFIX).
        out += d.actual;
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

// The caller's registry is authoritative; this reporter's own retained
// buffers answer only what it cannot. The two can never disagree — `BufferId`
// comes from ONE process-wide monotonic counter — so the order matters for
// cost, not for correctness.
std::shared_ptr<SourceBuffer const>
DiagnosticReporter::resolveBuffer_(BufferId id,
                                   BufferRegistry const& bufs) const noexcept {
    if (auto buf = bufs.tryGet(id)) return buf;
    return ownBuffers_.tryGet(id);
}

std::string DiagnosticReporter::format(ParseDiagnostic const& d,
                                       BufferRegistry const& bufs) const {
    std::string out;

    // Header line: <severityName>[<codePrefix>]: <contextPrefix><expected/actual prose>
    // The two bracket-rendered fields are distinct concepts: <codePrefix>
    // is the single-letter band (`P`/`D`/`H`/...) from `diagnosticCodePrefix`,
    // while <contextPrefix> is the per-target `[target=...]` stamp set by
    // `program::mergeWithTargetContext`.
    // D-MERGE-DEDUP-PREFIX-COLLISION: contextPrefix is rendered here
    // (not stored in `actual`) so the dedup hash key — which includes
    // `actual` — sees the un-prefixed diagnostic.
    std::format_to(std::back_inserter(out),
                   "{}[{}]: ",
                   severityName(d.severity),
                   diagnosticCodePrefix(d.code));
    out += d.contextPrefix;
    appendExpectedActual(out, d);
    out += '\n';

    // Primary location + caret + source line, if we can resolve the buffer.
    if (auto buf = resolveBuffer_(d.buffer, bufs)) {
        appendLineWithCaret(out, *buf, d.span, "");
    } else {
        std::format_to(std::back_inserter(out),
                       "  --> <unknown-buffer:{}>:offset {}\n   |\n",
                       d.buffer.v, d.span.start());
    }

    // Related locations.
    for (auto const& rel : d.related) {
        std::format_to(std::back_inserter(out), "note: {}\n", rel.note);
        if (auto buf = resolveBuffer_(rel.buffer, bufs)) {
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
