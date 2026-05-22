#include "analysis/syntactic/parser.hpp"

#include "core/types/compiled_shape.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/schema_walker.hpp"
#include "core/types/source_span.hpp"
#include "core/types/token.hpp"
#include "core/types/tree_builder.hpp"
#include "core/types/tree_node.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace dss {

namespace {

[[noreturn]] void fatal(char const* what) noexcept {
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// Trivia: the builder owns cursor-skip for these tokens; the parser
// pushes them through without consulting the cursor and the parser's
// own walker stays put so the next iteration re-checks the slot
// against the next meaningful token.
[[nodiscard]] bool isSkippableTrivia(Token const& tok) noexcept {
    switch (tok.coreKind) {
    case CoreTokenKind::Whitespace:
    case CoreTokenKind::Newline:
    case CoreTokenKind::LineComment:
    case CoreTokenKind::BlockComment:
        return true;
    default:
        break;
    }
    return isEmptySpace(tok.flags);
}

// Mirror the builder's `pushToken` kind-resolution so dispatch
// (FIRST/expected-set checks) sees the SAME kind the builder will
// resolve to. Covers two builder paths:
//   - Word coreKind with invalid schemaKind → Identifier fallback.
//   - Error coreKind → schema's `Error` token id so downstream
//     diagnostic context reports the actual kind rather than the
//     invalid sentinel.
// Does NOT mirror contextual-keyword demotion or scope-rejected
// meaning resolution: those depend on the cursor's expectedSet +
// live scope stack which the parser doesn't easily replicate before
// the builder runs. Surfacing as `P_SchemaCursorDesync` is the
// designed observability for that drift.
[[nodiscard]] SchemaTokenId effectiveKind(Token const& tok,
                                          SchemaTokenId identifierKind,
                                          SchemaTokenId errorKind) noexcept {
    if (tok.schemaKind.valid()) return tok.schemaKind;
    if (tok.coreKind == CoreTokenKind::Word)  return identifierKind;
    if (tok.coreKind == CoreTokenKind::Error) return errorKind;
    return InvalidSchemaToken;
}

// Kept as a 2-value enum (not `bool`) for call-site readability and
// for the compiler's switch-exhaustiveness warning when a third
// outcome (e.g. graceful watchdog termination) is added in PA2/PA3.
enum class StepOutcome {
    Continue,
    Done,
};

} // namespace

// ── Impl ────────────────────────────────────────────────────────────────

struct Parser::Impl {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
    TokenStream                          tokens;
    ParserConfig                         config;

    // Cached well-known kinds — looked up once per parse instead of
    // per token. Mirrors `TreeBuilder::errorKind_`'s pattern.
    SchemaTokenId identifierKind;
    SchemaTokenId errorKind;

    // Builder lives in Impl (heap) since `TreeBuilder` is non-movable
    // (static_assert) and Impl is constructed in-place.
    std::unique_ptr<TreeBuilder> builder;

    // Frame guards held in declaration order; the back is the
    // innermost open frame.
    std::vector<TreeBuilder::OpenScope> frames;

    // Parser-side walker. The builder embeds its own; the two are
    // driven lock-step by the dispatch loop. Divergence is a load-
    // bearing bug-catcher — surfaces as the builder's
    // `P_SchemaCursorDesync` info diagnostic. The parser's walker
    // takes no desync callback (only the builder emits the
    // diagnostic).
    SchemaWalker walker;

    // Forward-progress watchdog state (carried across iterations).
    SchemaCursor lastCursor{};
    std::size_t  lastTokPos = 0;
    std::size_t  lastDepth  = 0;
    bool         firstIteration = true;

    // Diagnostic-emission counter. Incremented by `emitDiag`; lets
    // the speculation sub-loop detect "branch dispatch emitted a
    // diagnostic" without builder-side introspection. Rollback
    // restores the builder's diagnostic stream but NOT this counter
    // (it's the parser's own metric); restore explicitly on
    // speculation rollback to keep accounting honest.
    std::size_t diagsEmitted = 0;

    // Speculation-nesting counter. Incremented on entry to
    // `trySpeculativeBranch`, decremented on every exit path. Bound
    // by `config.maxSpeculationDepth`; over-cap entry emits
    // `P_MaxSpeculationDepth` and refuses the probe (caller then
    // emits `P_BacktrackFailed` + consumes peek for forward
    // progress). Adversarial input that nests speculative alts
    // indefinitely would otherwise stack-loop the call stack.
    std::size_t speculationDepth = 0;

    Impl(std::shared_ptr<SourceBuffer>        s,
         std::shared_ptr<GrammarSchema const> sc,
         TokenStream                          ts,
         ParserConfig                         cfg)
        : src(std::move(s))
        , schema(std::move(sc))
        , tokens(std::move(ts))
        , config(cfg)
        , identifierKind(schema->schemaTokens().find("Identifier"))
        , errorKind(schema->schemaTokens().find("Error"))
        , walker(schema) {}

    // ── helpers ──────────────────────────────────────────────────────

    void emitDiag(ParseDiagnostic d) {
        builder->reportDiagnostic(std::move(d));
        ++diagsEmitted;
    }

    // Build + emit a parser-side diagnostic with severity Error,
    // `buffer = src->id()`, and `ruleContext = builder->currentRule()`
    // populated. Centralises the seven-field scaffold previously
    // repeated at each emission site.
    void emitParserError(DiagnosticCode code, SourceSpan span,
                         std::string actual) {
        ParseDiagnostic d;
        d.code        = code;
        d.severity    = DiagnosticSeverity::Error;
        d.buffer      = src->id();
        d.span        = span;
        d.ruleContext = frames.empty()
                            ? std::optional<RuleId>{}
                            : std::optional<RuleId>{builder->currentRule()};
        d.actual      = std::move(actual);
        emitDiag(std::move(d));
    }

    void closeFrameOnce() noexcept {
        if (frames.empty()) return;
        const RuleId rule = builder->currentRule();
        walker.leaveRule(SourceSpan::empty(0), rule);
        frames.back().close();
        frames.pop_back();
    }

    [[nodiscard]] bool isAtSourceEnd() const noexcept {
        return tokens.isAtEnd() || tokens.peek().coreKind == CoreTokenKind::Eof;
    }

    // "Would taking the nullable branch right now silently abandon
    // a non-EOF token by leaving the parser at end-of-source?" When
    // true, the caller must NOT skip — emit recovery instead.
    //
    // Concretely: a token outside every alternative's FIRST that
    // lands at a root-rule nullable-tail position would otherwise
    // terminate the parse without diagnosing. Mid-rule skips don't
    // trigger this because their post-skip cursor stays inside a
    // non-root rule (canEndSource is root-rule-only).
    [[nodiscard]] bool skipWouldAbandonToken(Token const& peek) const noexcept {
        if (peek.coreKind == CoreTokenKind::Eof) return false;
        const auto post = schema->nullableBranch(walker.cursor());
        if (!post.valid()) return false;
        return schema->canEndSource(post);
    }

    // Enumerate candidate branch rules at the current speculative
    // AltChoice: rule ids `r` for which both
    //   (1) `FIRST(r)` contains the upcoming token's effective kind, AND
    //   (2) `routeToRuleLeaf(walker.cursor(), r)` is valid (the alt
    //       structurally allows descending into r).
    // O(rules) per AltChoice miss; acceptable for current grammar sizes.
    [[nodiscard]] std::vector<RuleId>
    candidateBranches(SchemaTokenId tokKind) const {
        std::vector<RuleId> out;
        const auto& interner = schema->rules();
        // Skip `RuleId{0}` — the invalid-sentinel slot — and iterate
        // real rules `[1, interner.size())`.
        for (std::uint32_t r = 1; r < interner.size(); ++r) {
            const RuleId candidate{r};
            const auto firstSet = schema->firstSetOf(candidate);
            if (firstSet.empty()) continue;
            if (std::ranges::find(firstSet, tokKind) == firstSet.end()) continue;
            const auto routed =
                schema->routeToRuleLeaf(walker.cursor(), candidate);
            if (!routed.valid()) continue;
            out.push_back(candidate);
        }
        return out;
    }

    // Move-only RAII guard for one speculative-branch probe.
    //
    // Mirrors `TreeBuilder::Checkpoint`'s "rollback unless committed"
    // discipline across all four state machines a speculative probe
    // touches: token stream, builder, walker, parser frames + the
    // delta-basis bookkeeping (diag counter, desync latch baseline,
    // speculation-depth counter, watchdog tuple). The probe owns the
    // invariant; the caller drives the inner dispatch loop and either
    // calls `commit()` on success or lets the dtor restore on any
    // failure path. Returning from the loop without `commit()`
    // (including via exception, `return false`, or an early `goto`)
    // structurally guarantees a complete restore — the four-machine
    // restore is no longer convention.
    class SpeculationProbe {
    public:
        explicit SpeculationProbe(Impl& impl)
            : impl_(impl)
            , bookmark_(impl.tokens.mark())
            , cp_(impl.builder->checkpoint())
            , walkerSnap_(impl.walker.snapshot())
            , targetDepth_(impl.frames.size())
            , diagsBefore_(impl.diagsEmitted)
            , probeStartPos_(impl.tokens.position())
            // Capture the desync latch as a delta baseline.
            // `SchemaWalker`'s latch is one-shot for the walker's
            // lifetime — once tripped by an earlier dispatch path
            // (contextual-keyword demotion, prior mismatched
            // advance), `isDesynced()` returns true permanently.
            // Comparing the absolute value would auto-fail every
            // speculation probe after the first parse-wide desync;
            // the delta `nowDesynced && !desyncedBefore` only fails
            // when *this branch* tripped the latch.
            , desyncedBefore_(impl.walker.isDesynced())
            // Budget: 16× the schema's declared lookahead. The
            // declared lookahead is the disambiguation distance,
            // not a total-cost bound, so multiply by a generous
            // factor to let the branch make legitimate progress
            // (descents, token consumption) while still aborting
            // adversarial input.
            , budget_(static_cast<std::size_t>(
                  impl.walker.lookahead() > 0
                      ? impl.walker.lookahead() * 16u
                      : 64u)) {
            ++impl_.speculationDepth;
        }

        SpeculationProbe(SpeculationProbe const&)            = delete;
        SpeculationProbe& operator=(SpeculationProbe const&) = delete;
        SpeculationProbe(SpeculationProbe&&)                 = delete;
        SpeculationProbe& operator=(SpeculationProbe&&)      = delete;

        ~SpeculationProbe() noexcept {
            if (!committed_) {
                // Order matters: drop local `OpenScope` guards
                // BEFORE `builder->rollback`. Rollback restores the
                // builder's open-frame stack from the checkpoint
                // snapshot, including its cookie-tracking set; an
                // `OpenScope` destructor running afterwards would
                // call `closeFrame_` on a cookie the builder has
                // discarded, tripping its invariant checker.
                // Popping first lets each destructor close cleanly
                // against a still-live cookie, and the subsequent
                // rollback then overwrites all the close-side
                // effects with the snapshot.
                while (impl_.frames.size() > targetDepth_) {
                    impl_.frames.pop_back();
                }
                impl_.walker.restore(std::move(*walkerSnap_));
                impl_.builder->rollback(std::move(*cp_));
                impl_.tokens.restore(bookmark_);
                impl_.diagsEmitted = diagsBefore_;
            }
            --impl_.speculationDepth;
        }

        [[nodiscard]] std::size_t targetDepth() const noexcept {
            return targetDepth_;
        }

        [[nodiscard]] bool isDesynced() const noexcept {
            return impl_.walker.isDesynced() && !desyncedBefore_;
        }

        [[nodiscard]] bool exceededBudget() const noexcept {
            return impl_.tokens.position() - probeStartPos_ > budget_;
        }

        [[nodiscard]] bool emittedDiag() const noexcept {
            return impl_.diagsEmitted > diagsBefore_;
        }

        // Commit the probe. After this the dtor is a no-op (beyond
        // the speculation-depth decrement) and the four-machine
        // state stays at its post-commit position. Also re-baselines
        // the outer watchdog tuple — the pre-speculation snapshot is
        // stale (cursor/tokPos/depth moved across multiple tokens),
        // and resetting `firstIteration` alone would hide the rare
        // case where post-commit state happens to equal pre-
        // speculation state.
        void commit() noexcept {
            impl_.builder->commit(std::move(*cp_));
            impl_.lastCursor = impl_.walker.cursor();
            impl_.lastTokPos = impl_.tokens.position();
            impl_.lastDepth  = impl_.frames.size();
            committed_       = true;
        }

    private:
        Impl&                                  impl_;
        TokenStream::Bookmark                  bookmark_;
        // `TreeBuilder::Checkpoint` and `SchemaWalker::Snapshot` are
        // move-only and non-default-constructible by design (every
        // instance must originate from the producer); wrap in
        // `std::optional` so the destructor can move-out into
        // rollback/restore exactly once.
        std::optional<TreeBuilder::Checkpoint> cp_;
        std::optional<SchemaWalker::Snapshot>  walkerSnap_;
        std::size_t                            targetDepth_;
        std::size_t                            diagsBefore_;
        std::size_t                            probeStartPos_;
        bool                                   desyncedBefore_;
        std::size_t                            budget_;
        bool                                   committed_ = false;
    };

    // Try one speculative branch. Opens the branch frame inside a
    // `SpeculationProbe` and drives `stepOnce` repeatedly until
    // either the branch's frame closes (`frames.size() ==
    // probe.targetDepth()` → success → commit + return true) or a
    // failure signal fires:
    //   - probe.isDesynced()    → grammar mismatch this branch
    //   - probe.exceededBudget()→ 16 × `walker.lookahead()` safety
    //                             bound; a branch that doesn't close
    //                             within budget is unlikely to be
    //                             the right choice
    //   - probe.emittedDiag()   → branch dispatch raised
    //                             P_NoAlternativeMatched /
    //                             P_UnexpectedToken /
    //                             P_MissingRequiredChild
    //   - stepOnce → Done       → reached EOF mid-branch; needs more
    //                             input
    // Any return-without-commit path triggers the probe's RAII
    // restore (tokens + builder + walker + frames + diag counter).
    [[nodiscard]] bool trySpeculativeBranch(RuleId branch) {
        if (speculationDepth >= config.maxSpeculationDepth) {
            emitParserError(
                DiagnosticCode::P_MaxSpeculationDepth,
                tokens.peek().span,
                std::format(
                    "parser speculation depth {} exceeds configured cap {}",
                    speculationDepth, config.maxSpeculationDepth));
            return false;
        }

        SpeculationProbe probe{*this};

        frames.push_back(builder->open(branch));
        walker.enterRule(branch);

        while (frames.size() > probe.targetDepth()) {
            if (probe.isDesynced())     return false;
            if (probe.exceededBudget()) return false;
            // Note: do NOT bail on `isAtSourceEnd && !walker.canEndSource()`
            // — the branch's last iteration legitimately runs with peek
            // == Eof when the cursor is at End and `closeFrameOnce` is
            // the next action. Premature-EOF mid-branch is caught by
            // stepOnce's own EOF handler, which emits
            // P_MissingRequiredChild → the post-stepOnce diag recheck
            // below fires.

            const auto outcome = stepOnce();
            if (outcome == StepOutcome::Done) {
                // Done at the outer parser level only when
                // canEndSource is true AND tokens drained — but
                // we're inside an unclosed branch frame here, so
                // this signals the branch needs more input than is
                // available. Treat as failure.
                return false;
            }
            // Post-step rechecks: stepOnce may have emitted a
            // diagnostic and closed the branch frame in the same
            // iteration (peek=EOF mid-rule). Without these the loop
            // exits via `frames.size() == targetDepth` and commits a
            // half-built branch. Mirror the loop-head checks.
            if (probe.emittedDiag()) return false;
            if (probe.isDesynced())  return false;
        }

        probe.commit();
        return true;
    }

    // ── one dispatch iteration ─────────────────────────────────────

    [[nodiscard]] StepOutcome stepOnce() {
        // Termination at root: `canEndSource` returns true only at
        // the root rule's nullable-tail end position; combined with
        // Eof on the input, we're done.
        if (walker.canEndSource() && isAtSourceEnd()) {
            return StepOutcome::Done;
        }

        // All frames closed (e.g. nullable-skip ran the root rule
        // to completion before EOF): there's no work left to do.
        // Without this check the End-slot dispatch below loops on
        // an empty-frame no-op `closeFrameOnce` forever, since the
        // watchdog sits after the End handler and cursor + tok-pos
        // + depth never change.
        if (frames.empty()) {
            return StepOutcome::Done;
        }

        // End-slot dispatch precedes the Eof handler: closing a
        // frame is always correct at End regardless of remaining
        // input. Conflating End with "premature EOF" emits
        // spurious P_MissingRequiredChild for every cleanly-closing
        // frame at end-of-stream.
        if (walker.slotKind() == SlotKind::End) {
            closeFrameOnce();
            return StepOutcome::Continue;
        }

        // Premature EOF: cursor still expects something but the
        // stream is exhausted. `tokens.advance()` at end is
        // idempotent (same Eof forever), so consuming wouldn't make
        // progress — close the current frame instead. `finish()`
        // will synthesize per-frame `P_PrematureEndOfInput` if a
        // frame is closed without seeing its required body.
        if (tokens.peek().coreKind == CoreTokenKind::Eof
            && !walker.canEndSource()) {
            if (frames.empty()) return StepOutcome::Done;
            emitParserError(
                DiagnosticCode::P_MissingRequiredChild,
                tokens.peek().span,
                "premature end of input — schema cursor expected more");
            closeFrameOnce();
            return StepOutcome::Continue;
        }

        // ── watchdog ──
        const SchemaCursor curCursor = walker.cursor();
        const std::size_t  curTokPos = tokens.position();
        const std::size_t  curDepth  = frames.size();
        if (!firstIteration
            && curCursor == lastCursor
            && curTokPos == lastTokPos
            && curDepth  == lastDepth) {
            emitParserError(
                DiagnosticCode::P_RecoveryStalled,
                tokens.peek().span,
                std::format(
                    "parser forward-progress watchdog tripped at slot kind {}, "
                    "core kind {}, schema kind {}, frame depth {}",
                    static_cast<int>(walker.slotKind()),
                    static_cast<int>(tokens.peek().coreKind),
                    tokens.peek().schemaKind.v,
                    frames.size()));
            fatal("dss::Parser: forward-progress watchdog tripped");
        }
        lastCursor = curCursor;
        lastTokPos = curTokPos;
        lastDepth  = curDepth;
        firstIteration = false;

        // ── per-slot dispatch ──
        const SlotKind slot = walker.slotKind();
        const Token&   peek = tokens.peek();

        switch (slot) {

        case SlotKind::End: {
            // Unreachable: End-slot dispatch is hoisted above the
            // watchdog because closing a frame consumes no tokens —
            // the watchdog's (cursor, tokPos, depth) tuple changes
            // only via the depth component. Two consecutive End
            // iterations with identical tuples would mis-trigger the
            // watchdog if dispatch went through the post-watchdog
            // path. Kept here for switch exhaustiveness; collapsing
            // these into one site silently breaks the watchdog.
            closeFrameOnce();
            return StepOutcome::Continue;
        }

        case SlotKind::TokenLeaf: {
            if (isSkippableTrivia(peek)) {
                builder->pushToken(tokens.advance());
                return StepOutcome::Continue;
            }

            const auto expected = walker.expectedSet();
            const SchemaTokenId tokKind =
                effectiveKind(peek, identifierKind, errorKind);
            const bool matches = !expected.empty()
                && std::ranges::find(expected, tokKind) != expected.end();

            if (matches) {
                const Token tok = tokens.advance();
                builder->pushToken(tok);
                walker.advance(tokKind, tok.span,
                               std::optional<RuleId>{builder->currentRule()});
                return StepOutcome::Continue;
            }

            const SchemaTokenId expectedTok =
                expected.empty() ? InvalidSchemaToken : expected.front();
            builder->pushError(peek.span,
                               std::nullopt,
                               expectedTok.valid()
                                   ? std::optional<SchemaTokenId>{expectedTok}
                                   : std::nullopt,
                               "token does not match expected schema slot");
            // `pushError` routes its diagnostic directly through the
            // builder's reporter, bypassing `emitDiag`, so the
            // parser's `diagsEmitted` counter — which gates
            // speculative-branch rollback detection — must be
            // incremented by hand to preserve that contract. Note
            // this only mirrors the parser's deliberate-emission
            // sites; trivia `pushToken` paths that emit builder-side
            // diagnostics (P_UnknownToken / P_SchemaCursorDesync /
            // P_AmbiguousToken / P_ContextualKeywordResolution) do
            // NOT flow through this counter — known gap, accepted
            // because those codes signal informational drift rather
            // than commit-blocking errors.
            ++diagsEmitted;
            (void)tokens.advance();
            return StepOutcome::Continue;
        }

        case SlotKind::RuleLeaf: {
            if (isSkippableTrivia(peek)) {
                builder->pushToken(tokens.advance());
                return StepOutcome::Continue;
            }

            const RuleId        nextRule = walker.slotRuleRef();
            const SchemaTokenId tokKind  =
                effectiveKind(peek, identifierKind, errorKind);
            const auto firstSet = schema->firstSetOf(nextRule);
            const bool tokInFirst = !firstSet.empty()
                && std::ranges::find(firstSet, tokKind) != firstSet.end();

            if (tokInFirst) {
                frames.push_back(builder->open(nextRule));
                walker.enterRule(nextRule);
                return StepOutcome::Continue;
            }

            if (schema->isNullable(nextRule)
                && !skipWouldAbandonToken(peek)) {
                // Skip-nullable: synthesize by enter+leave. Mirrors
                // the AltChoice nullable-skip guard — without the
                // abandon-check a nullable RuleLeaf reachable from
                // root depth would silently drop a non-EOF token at
                // end-of-root. Shipped grammars compile their
                // optionals to AltChoice positions rather than
                // nullable RuleLeaf references, so this path is rare
                // in practice; the guard exists for grammar-author
                // safety.
                frames.push_back(builder->open(nextRule));
                walker.enterRule(nextRule);
                closeFrameOnce();
                return StepOutcome::Continue;
            }

            emitParserError(
                DiagnosticCode::P_NoAlternativeMatched,
                peek.span,
                std::format(
                    "token kind {} is not in FIRST({}) and the rule is "
                    "not nullable",
                    static_cast<int>(tokKind.v),
                    static_cast<int>(nextRule.v)));
            (void)tokens.advance();
            return StepOutcome::Continue;
        }

        case SlotKind::AltChoice: {
            if (isSkippableTrivia(peek)) {
                builder->pushToken(tokens.advance());
                return StepOutcome::Continue;
            }

            const SchemaTokenId tokKind =
                effectiveKind(peek, identifierKind, errorKind);
            const auto expected = walker.expectedSet();
            const bool inUnion = !expected.empty()
                && std::ranges::find(expected, tokKind) != expected.end();

            if (walker.isSpeculativeAlt()) {
                if (!inUnion) {
                    // Nullable-skip guarded by `skipAcceptsPeek`: a
                    // speculative alt's nullable-tail is "skip the
                    // loop". Take it only when post-skip can progress
                    // on peek (or peek is EOF), so a token outside
                    // every branch's FIRST gets diagnosed rather than
                    // silently abandoned.
                    if (walker.nullableTail()
                        && !skipWouldAbandonToken(peek)
                        && walker.takeNullableBranch()) {
                        return StepOutcome::Continue;
                    }
                    emitParserError(
                        DiagnosticCode::P_BacktrackFailed,
                        peek.span,
                        "token not in any speculative branch's FIRST set");
                    (void)tokens.advance();
                    return StepOutcome::Continue;
                }

                // Try each candidate branch in declaration order.
                // First success commits and falls through to the
                // next outer iteration; full failure emits one
                // P_BacktrackFailed + consumes for forward progress.
                const auto candidates = candidateBranches(tokKind);
                for (auto const branch : candidates) {
                    if (trySpeculativeBranch(branch)) {
                        return StepOutcome::Continue;
                    }
                }

                emitParserError(
                    DiagnosticCode::P_BacktrackFailed,
                    peek.span,
                    "every speculative branch failed to commit");
                (void)tokens.advance();
                return StepOutcome::Continue;
            }

            // Non-speculative AltChoice.
            if (!inUnion) {
                // optional/repeat skip: take the nullable branch
                // ONLY when the post-skip cursor can actually do
                // something with `peek` (or is at EOF). Otherwise the
                // skip silently abandons a non-matchable token instead
                // of emitting recovery output. Pinned by
                // `BrokenPath_TokenNotInAnyFirstSet` (toy: `;` at
                // root, where FIRST(statement) lacks EndCommand and
                // post-skip would be end-of-root anyway — but `;` is
                // not EOF, so emit + consume) vs. c-subset's
                // legitimate `int x = 5;` shape (optional pointer-spec
                // skipped to land on a real downstream slot).
                if (walker.nullableTail()
                    && !skipWouldAbandonToken(peek)
                    && walker.takeNullableBranch()) {
                    return StepOutcome::Continue;
                }

                emitParserError(
                    DiagnosticCode::P_NoAlternativeMatched,
                    peek.span,
                    "no AltChoice branch FIRST-set contains the next token");
                (void)tokens.advance();
                return StepOutcome::Continue;
            }

            // Try TokenLeaf-branch route first via the schema's
            // transparent `advance` (which only handles AltChoice
            // → TokenLeaf, not AltChoice → RuleLeaf).
            const SchemaCursor afterAdvance =
                schema->advance(walker.cursor(), tokKind);
            if (afterAdvance.valid()) {
                const Token tok = tokens.advance();
                builder->pushToken(tok);
                walker.advance(tokKind, tok.span,
                               std::optional<RuleId>{builder->currentRule()});
                return StepOutcome::Continue;
            }

            // AltChoice → RuleLeaf branch route. The schema's
            // public API doesn't enumerate an AltChoice's branch
            // RuleIds, so we linear-scan the rule interner for a
            // candidate whose FIRST contains tokKind and for which
            // `routeToRuleLeaf` accepts from the current cursor.
            // O(rules) per AltChoice miss; acceptable for current
            // grammar sizes.
            const auto& interner = schema->rules();
            for (std::uint32_t r = 1; r < interner.size(); ++r) {
                const RuleId candidate{r};
                const auto firstSet = schema->firstSetOf(candidate);
                if (firstSet.empty()) continue;
                if (std::ranges::find(firstSet, tokKind) == firstSet.end()) continue;
                const auto routed = schema->routeToRuleLeaf(
                    walker.cursor(), candidate);
                if (!routed.valid()) continue;
                frames.push_back(builder->open(candidate));
                walker.enterRule(candidate);
                return StepOutcome::Continue;
            }

            // Unreachable for any schema that passed loader
            // validation: a token in `expectedSet` must be routable
            // via `schema->advance` (AltChoice→TokenLeaf) or via the
            // RuleLeaf FIRST-set scan above. Reaching this branch
            // means the loader's union-vs-routing consistency check
            // is broken; the P_NoAlternativeMatched here is a
            // fail-safe surfacing the invariant violation, not a
            // recovery path.
            emitParserError(
                DiagnosticCode::P_NoAlternativeMatched,
                peek.span,
                "AltChoice routing failed for a token in the union — schema invariant violation");
            (void)tokens.advance();
            return StepOutcome::Continue;
        }

        }   // switch slotKind

        // Unreachable: switch is exhaustive over SlotKind.
        return StepOutcome::Continue;
    }
};

Parser::Parser(std::shared_ptr<SourceBuffer>        src,
               std::shared_ptr<GrammarSchema const> schema,
               TokenStream                          tokens,
               ParserConfig                         config) {
    // Fail-fast preconditions: a null schema or source means the
    // caller mis-constructed the parser. Asserting in the ctor (not
    // `parse()`) catches the bug at the construction site rather
    // than one method call later. The ctor is therefore NOT noexcept
    // — `make_unique<Impl>` can throw and the fatal-aborts here
    // signal violations the caller cannot recover from.
    if (!schema) fatal("dss::Parser::Parser: schema is null");
    if (!src)    fatal("dss::Parser::Parser: source buffer is null");
    if (config.maxSpeculationDepth == 0) {
        fatal("dss::Parser::Parser: maxSpeculationDepth must be >= 1");
    }
    impl_ = std::make_unique<Impl>(std::move(src),
                                   std::move(schema),
                                   std::move(tokens),
                                   config);
}

Parser::~Parser() = default;

// ── parse() ─────────────────────────────────────────────────────────────

ParseResult Parser::parse() && {
    auto& I = *impl_;

    I.builder = std::make_unique<TreeBuilder>(I.src, I.schema);

    const RuleId rootRule = I.schema->rootCursor().rule();
    if (!rootRule.valid()) {
        fatal("dss::Parser::parse: schema has no root rule");
    }

    I.frames.push_back(I.builder->open(rootRule));
    I.walker.enterRule(rootRule);

    I.lastCursor = I.walker.cursor();
    I.lastTokPos = I.tokens.position();
    I.lastDepth  = I.frames.size();
    I.firstIteration = true;

    // Drain head-of-stream trivia so dispatch sees a meaningful
    // token on iteration 0.
    while (!I.tokens.isAtEnd() && isSkippableTrivia(I.tokens.peek())) {
        I.builder->pushToken(I.tokens.advance());
    }

    while (true) {
        if (I.stepOnce() == StepOutcome::Done) break;
    }

    // Close remaining frames (root in the happy path) before
    // `finish()` so the builder doesn't synthesize spurious
    // premature-close diagnostics.
    while (!I.frames.empty()) {
        I.closeFrameOnce();
    }

    return ParseResult{std::move(*I.builder).finish()};
}

} // namespace dss
