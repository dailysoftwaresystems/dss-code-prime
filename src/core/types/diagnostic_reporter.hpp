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
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss {

// ── THE ONE SPELLING OF A DIAGNOSTIC CODE ──────────────────────────────────
//   D-DIAG-TWO-CODE-RENDERINGS
//
// ★★★ EVERY SURFACE THAT RENDERS A DIAGNOSTIC CODE FOR A READER SPELLS IT
// `severity[<diagnosticCodeName>]: ` — the SYMBOLIC NAME, in the bracket,
// followed by a colon and a space. No exceptions, and the exceptions are what
// this contract exists to forbid.
//
// ⭐ WHAT WENT WRONG WITHOUT IT. One diagnostic concept had TWO rendered forms,
// and which one a reader got depended on a property they could not see:
// `program.cpp`'s `drainDiagnosticsToStderr` routes on `d.buffer.valid()`, so a
// diagnostic WITH a source buffer came out `error[P0009]: …` (the 4-hex-digit
// band form, colon) while a buffer-LESS one came out `error[K_SymbolUndefined] …`
// (the symbolic name, NO colon). ★★ A census matching `[A-Z][0-9A-F]{4}` was
// therefore not "undercounting" buffer-less diagnostics — it COULD NOT SEE THEM.
// ⟲RELAYED from the row (not re-measured when this contract was written): 133 of
// 189 per-TU runs in the sqlite corpus arc emitted only the invisible form. The
// MECHANISM was re-verified here by reproducing both renderings through the
// shipped CLI; it is the FIGURE that is second-hand. Either way it is the
// mechanism by which a measurement can report a confident number having never
// looked at most of its population.
//
// ★★ WHY THE NAME AND NOT THE HEX — the row that filed this proposed the hex
// band form, and MEASUREMENT at closing time refuted it on four counts:
//   (1) FIVE of the six render surfaces ALREADY spoke the name (this file's
//       `format` header was the ONLY hex one): the CLI buffer-less one-liner,
//       `dump_predefined_macros`' two emitters, the LSP `code` field, and
//       `lsp/workspace_project`. Unifying on the name is a ONE-SITE change and
//       makes the terminal and the IDE agree with NO LSP edit at all; unifying
//       on hex would have moved four sites and split them again.
//   (2) `diagnosticCodeName` is this project's stated ALLOCATION ORACLE — a
//       `default:`-less switch, exact and TOTAL. `diagnosticCodePrefix` is a
//       lossy projection whose own header documents that an unallocated family
//       renders under a non-letter (`?3000`) precisely so scrapers cannot trust
//       it. The identity is the enumerator; the hex is a rendering of it.
//   (3) NO INFORMATION IS LOST: the name carries the same band as its prefix
//       (`P_`, `S_`, `K_`, `D_`), so the tier is still legible, and the code is
//       strictly more self-describing than `P0009`.
//   (4) `integrated_tests` — the harness that drives the SHIPPED CLI — links
//       only nlohmann_json, NOT core. ✔MEASURED: it can WRITE a symbolic name
//       from a manifest but can never COMPUTE a hex without a fourth
//       hand-maintained copy of the nibble table, which is exactly the
//       third-copy drift hazard already anchored as D-CONFIG-COMMENT-CLAIM-ROT.
//       Choosing hex would have made the CLI surface unaddressable by its own
//       harness.
//
// ⚠ THE ROUTING SPLIT ITSELF STAYS, AND IT IS RIGHT. A buffer-less driver-tier
// diagnostic has no location, and forcing it through the positioned renderer
// would print a bogus `<unknown-buffer:0>` line and a spurious `got ` prefix.
// What changed is WHICH TOKEN IDENTIFIES THE CODE and the punctuation after it —
// never the layout. See `drainDiagnosticsToStderr`'s routing comment.
//
// ⚠ THE HEX IS NOT LOST AS AN INTERFACE: `--suppress=<code>` accepts BOTH the
// symbolic name and the `0x____` form (`cli_args.cpp` `parseSuppressCode`), so a
// reader who sees `error[P_UnexpectedToken]` can act on it directly and a
// caller holding a hex value still resolves.
//
// ⚠ ONE DELIBERATE PLACE STILL PRINTS BOTH SPELLINGS, AND IT IS NOT AN
// OVERSIGHT — DO NOT "FIX" IT. The elision marker built by `noteElisionText_`
// renders `P0009 (P_UnexpectedToken) diagnostics were ELIDED …` in its PROSE.
// That is prose ABOUT A DIFFERENT CODE than the one in its own bracket
// (`P_DiagnosticsElided`), not a second spelling of its own; and it is the one
// place in compiler output that shows the name↔hex correspondence, which is
// what keeps plan 00 §0.3's table reachable from a log now that headers no
// longer carry the hex. The contract above governs the BRACKET — the token that
// identifies the diagnostic being reported — not every mention of a code
// anywhere in a message.
//
// The empty-prose fallback below is the other half of the same contract.
// ────────────────────────────────────────────────────────────────────────────

// What a diagnostic's message body says when it carries no `expected`, no
// `actual` and no `suggestion`.
//
// ★ IT MUST NOT BE THE CODE. Before this constant, both the CLI
// (`appendExpectedActual`) and the LSP (`composeMessage`) fell back to
// `diagnosticCodeName(d.code)`, which put a SECOND copy of the code on a line
// that already names it in the bracket. Under the two-rendering split that was
// hazard (H2) on the row — one line carrying two spellings of one code, which a
// de-duplicating census double-counts unless it normalises first. Unifying the
// bracket on the name did not remove it: it turned two spellings into the SAME
// spelling twice, which a name-keyed census double-counts just as happily.
// A body that states a fact instead of echoing the header removes the hazard
// outright, and reads better: the LSP already carries `code` as its own
// structured field, so repeating it in `message` was pure noise there.
//
// ⚠ Deliberately ASCII and deliberately not containing `[`, `]` or a code-shaped
// token, so no scraper can mistake it for one.
inline constexpr std::string_view kNoDiagnosticDetailRecorded =
    "(no further detail was recorded for this diagnostic)";

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

    // Register every buffer `other` holds. Idempotent per id, exactly like
    // `add` — a buffer both registries already share is registered once.
    // ★ IT EXISTS SO A BUFFER CAN TRAVEL WITH ITS DIAGNOSTICS ACROSS A
    // REPORTER MERGE. `DiagnosticReporter` retains the buffers minted for its
    // own diagnostics (see `sourceBuffers()`), and a merge that carried the
    // diagnostics but not the buffers would hand the destination a diagnostic
    // whose source is unreachable — the exact `<unknown-buffer:N>` render this
    // pair of facilities exists to end.
    void addAll(BufferRegistry const& other);

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

    // ★★★ D-DIAG-MAXPERCODE-SILENT-COALESCE, LIMB (C): THE FLOOR MUST BE
    // MACHINE-READABLE, NOT ONLY HUMAN-READABLE.
    //
    // The arc's failure was never that a human missed a marker. It was that a
    // SCRIPT counted `50` and reported `50`. `scripts/corpus-census` hard-coded
    // `PER_CODE_CAP = 50` as a hand-copy of this class's default and asked "is
    // this number suspiciously round?" — a detection method with no
    // false-negative bound, and a mirror that silently mis-labels floors as
    // totals the moment the default moves.
    //
    // So the elision is exposed as DATA as well as prose. A consumer can now
    // ask "was any figure in this run a floor?" and get a yes/no, without
    // regex-sniffing for round numbers and without knowing what the cap is.
    struct ElisionLedger {
        std::size_t coalesced   = 0;   // dropped by the per-code cap
        std::size_t deduped     = 0;   // dropped by the recent-duplicate window
        std::size_t markerIndex = 0;   // index into all()

        [[nodiscard]] constexpr std::size_t total() const noexcept {
            return coalesced + deduped;
        }
    };

    // What this reporter elided for `code` — zeroes when nothing was elided.
    [[nodiscard]] ElisionLedger elisionFor(DiagnosticCode code) const noexcept;

    // True iff ANY code's reported count in this reporter is a FLOOR rather
    // than a total. The one question a census actually wants answered.
    [[nodiscard]] bool anyCountIsAFloor() const noexcept { return !elided_.empty(); }

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
        // The per-code elision ledger travels for EXACTLY the reason
        // `capMarkerIndex` does: each row holds an index INTO `all_`, which
        // `truncateTo` shrinks. Restoring the counts without the indices — or
        // the indices without the counts — would leave a marker index pointing
        // past the end, and the next elision on that code would rewrite a
        // diagnostic that is not its marker, or run off the vector. Whole-map
        // capture is the only sound shape here, same as `recent`.
        std::unordered_map<DiagnosticCode, ElisionLedger> elided;
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
    // `fn` is invoked once per LOCATION with mutable references; a no-op
    // `fn` (or one that leaves non-synth diagnostics untouched) is harmless.
    // Only `buffer`/`span` are mutable -- code/severity/message are
    // unchanged. The recent-duplicate hash window is NOT rebuilt (it tracks
    // already-admitted diagnostics; remap runs after admission).
    //
    // ★★ EVERY LOCATION MEANS EVERY LOCATION -- THE PRIMARY *AND* EACH
    // `related` ENTRY. A `RelatedLocation` is a (buffer, span) pair of the
    // SAME diagnostic ("previously declared here"), minted from the same tree
    // and therefore in the same coordinate system as the primary; a remap that
    // moved only the primary left the note behind in coordinates the primary no
    // longer uses. ✔MEASURED through the CLI before this was added
    // ([[D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED]]): a redeclaration whose
    // FIRST declaration lives in an `#include`d header rendered its primary at
    // the main file and its `note:` at the main file TOO -- naming a line of
    // main.c that holds different text, because the note's real home is the
    // header. The primary-only loop is why: it was never a policy that notes
    // keep synth coordinates, it was an omission that nothing could see.
    // Symmetric by construction now -- one `fn`, applied to both, so no future
    // caller can fix one half and not the other.
    template <class F>
    void remapBuffers(F&& fn) {
        for (ParseDiagnostic& d : all_) {
            fn(d.buffer, d.span);
            for (RelatedLocation& r : d.related) fn(r.buffer, r.span);
        }
    }

    // ★★★ RE-ANCHOR A RANGE OF ALREADY-ADMITTED DIAGNOSTICS ONTO A NEW PRIMARY
    // LOCUS, demoting each one's current locus to a related location.
    //
    // WHY THIS EXISTS (D-ASM-TEMPLATE-DIAGNOSTIC-DOES-NOT-NAME-THE-C-STATEMENT):
    // an embedded-assembly template's diagnostics are raised against the
    // mid-compile-minted `<inline asm>` fragment buffer by code the CALLER
    // cannot intercept (the grammar machinery and the asm engine report through
    // the shared `DiagnosticReporter&`). Post-admission mutation is the only
    // interception point that adds no interface — the precedent `remapBuffers`
    // set for the preprocessor's line-map. ✔MEASURED, gcc 13.3 and clang 18:
    // BOTH references make the C statement the PRIMARY locus of an inline-asm
    // error (clang keeps the template as a `note:` block; gcc echoes its text),
    // so the fragment locus is DEMOTED, never dropped.
    //
    // `from` is an index into `all()`: every diagnostic at index >= `from` gets
    // (a) its current (buffer, span) appended to `related` with `note` — ONLY IF
    // the buffer is valid; the buffer, not the span, is the discriminator,
    // because a zero-width span at a real buffer is a genuine locus (a Missing
    // node at a position) that must demote, while an invalid buffer means the
    // diagnostic never had a locus and there is nothing to demote — and then
    // (b) its primary set to (`newBuffer`, `newSpan`).
    //
    // Runs AFTER admission, exactly like `remapBuffers`: the dedup window is
    // not rebuilt (duplicates were already coalesced on their original keys),
    // no element is inserted or removed (so the cap marker's index stays
    // valid), and `from > all().size()` is a no-op — a caller snapshots
    // `all().size()` before a phase and reanchors after it, whatever the phase
    // reported in between.
    void reanchorFrom(std::size_t     from,
                      BufferId        newBuffer,
                      SourceSpan      newSpan,
                      std::string_view note);

    // ★★★ THE BUFFERS THIS REPORTER'S OWN DIAGNOSTICS POINT INTO — the
    // retention that makes a MID-PIPELINE fragment renderable.
    //
    // ⚠ THE PROBLEM IT SOLVES, ✔MEASURED 2026-08-17 THROUGH THE CLI. A
    // `ParseDiagnostic` carries a `BufferId`, never the buffer; the two travel
    // separately and are re-paired at the driver, which builds its
    // `BufferRegistry` from the compiled units (`cu.trees()` +
    // `cu.auxiliaryBuffers()`). That re-pairing reaches every buffer that
    // exists BEFORE the compile — and NO buffer minted during it. An embedded
    // assembly template is parsed at the LIR tier out of a C string literal,
    // so its `SourceBuffer` is minted mid-compile, is reachable from no
    // `CompilationUnit`, and on the parse-FAILURE path dies with the `Tree`
    // that held it. Both of its diagnostic families rendered as
    //     error[P0001]: expected 'LineEnd' — got '@'
    //       --> <unknown-buffer:6>:offset 13
    // — a forwarded parse error AND (measured, and not what the anchor
    // predicted) every `A_AsmTextUnsupported` refusal the lowering itself
    // raises, which stamps the same fragment buffer from `tree_.source()`.
    //
    // ★ SO THE PAIRING MOVES TO THE OBJECT THAT ALREADY SPANS THE GAP. The
    // reporter is handed to the fragment parse and outlives it; retaining the
    // buffer HERE, at the moment it is minted and before a single token is
    // read, makes it survive the failure path by construction rather than by a
    // caller remembering to drain something. `format()` consults this store
    // when the caller-supplied registry cannot resolve — so a reporter can
    // always render its own diagnostics, whatever registry it is handed,
    // including an empty one.
    // ⚠ IT CANNOT MIS-RESOLVE. `BufferId` is minted from ONE process-wide
    // monotonic counter (`substrate::mintMonotonicId<BufferId>`), so an id that
    // resolves here and an id that resolves in the driver's registry are the
    // same buffer; the fallback can only ever ADD an answer where there was
    // none.
    // ⓘ REUSES `BufferRegistry` RATHER THAN A SECOND CONTAINER: registration,
    // idempotency and lookup already have an owner, and a parallel
    // `vector<shared_ptr<SourceBuffer>>` would be a second spelling of it.
    // ⓘ NOT ROLLED BACK BY `truncateTo`. A speculative parse that registers a
    // buffer and is then rewound leaves a retained buffer no diagnostic names —
    // which costs a map entry and renders nothing. Rolling it back would risk
    // the opposite (a live diagnostic losing its source), and that failure is
    // silent while this one is not.
    [[nodiscard]] BufferRegistry&       sourceBuffers()       noexcept { return ownBuffers_; }
    [[nodiscard]] BufferRegistry const& sourceBuffers() const noexcept { return ownBuffers_; }

    // Pretty-printers. The registry resolves BufferId → SourceBuffer so
    // multi-file diagnostics (related-locations spanning includes, future)
    // format correctly. `bufs` WINS; `sourceBuffers()` is consulted only for
    // an id it does not carry.
    [[nodiscard]] std::string formatAll(BufferRegistry const& bufs) const;
    [[nodiscard]] std::string format(ParseDiagnostic const& d,
                                     BufferRegistry const& bufs) const;

private:
    // Apply policy in this order: suppress → override → warningsAsErrors.
    // Returns std::nullopt if the diagnostic should be dropped.
    [[nodiscard]] std::optional<ParseDiagnostic> applyPolicy(ParseDiagnostic d) const;

    [[nodiscard]] bool isRecentDuplicate(ParseDiagnostic const& d) const noexcept;

    // `bufs` first, then this reporter's own retained buffers. ONE helper so
    // the primary location and every related location resolve identically — a
    // fallback applied to one and not the other is how a related note keeps
    // printing `<unknown-buffer>` next to a correctly rendered primary.
    [[nodiscard]] std::shared_ptr<SourceBuffer const>
    resolveBuffer_(BufferId id, BufferRegistry const& bufs) const noexcept;

    // Record that the global cap ate one more diagnostic, and REWRITE the
    // marker's prose so it always states the running total. The rewrite is
    // in place into the marker's existing string, so after the first few
    // drops it reallocates nothing — which is what makes "count on the
    // already-dropping path" affordable enough to do unconditionally
    // instead of leaving the number to be recovered later (it cannot be:
    // once a diagnostic is dropped there is nothing left to count).
    void noteCapDrop_();

    // ★★★ D-DIAG-MAXPERCODE-SILENT-COALESCE — THE PER-CODE ELISION LEDGER.
    //
    // The global cap got a marker; the two gates that actually fire in an
    // ordinary build did not. `noteElision_` gives them one, and gives BOTH of
    // them the SAME one — deliberately, because they are the same fact from a
    // reader's point of view ("N diagnostics of this kind exist that you are
    // not being shown") and two markers for one code would reproduce exactly
    // the noise the gates exist to suppress.
    //
    // Minted ONCE per code, then rewritten in place as the counts grow —
    // `noteCapDrop_`'s shape, for `noteCapDrop_`'s reason: `all()` is readable
    // at any moment and must never be observed carrying a stale number.
    enum class ElisionKind { PerCodeCap, DedupWindow };
    void noteElision_(ParseDiagnostic const& d, ElisionKind kind);

    // Rewrite one code's marker prose from its current ledger row. Split out
    // so minting and updating cannot compose the sentence differently — the
    // second-source-of-truth failure this whole area exists to remove.
    void rewriteElisionMarker_(DiagnosticCode code);

    // Absent from the map == nothing elided for that code, which is the
    // overwhelmingly common case, so a build that never saturates pays one
    // empty-map probe per elision-free report and nothing else.
    std::unordered_map<DiagnosticCode, ElisionLedger> elided_;

    Config                                  cfg_{};
    std::vector<ParseDiagnostic>            all_;
    std::unordered_map<DiagnosticCode, std::size_t> perCode_;
    std::deque<std::uint64_t>               recent_;      // hash of (code, buffer, span, rule, actual) sliding window
    bool                                    hitCap_ = false;
    std::size_t                             droppedByCap_   = 0;
    std::size_t                             capMarkerIndex_ = 0;  // index into all_; valid only while hitCap_
    BufferRegistry                          ownBuffers_{};        // see sourceBuffers()
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
