#include "analysis/syntactic/parser.hpp"

#include "analysis/syntactic/pratt_walker.hpp"
#include "core/types/compiled_shape.hpp"
#include "core/types/operator_table.hpp"
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
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_set>
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
    // Non-copyable and non-movable: `bodyDefaultTokenKinds` below is a
    // raw pointer into the schema's owned set. The pointer is bound at
    // ctor time and is stable for the lifetime of this object — moving
    // `Impl` would silently rebind to dangling memory. The current
    // `Parser` holds `Impl` via `unique_ptr` and never moves it, but
    // mirroring `TreeBuilder`'s explicit pin makes that invariant a
    // compile-time contract instead of an unintentional one.
    Impl(Impl const&)            = delete;
    Impl(Impl&&)                 = delete;
    Impl& operator=(Impl const&) = delete;
    Impl& operator=(Impl&&)      = delete;

    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
    TokenStream                          tokens;
    ParserConfig                         config;

    // Cached well-known kinds — looked up once per parse instead of
    // per token. Mirrors `TreeBuilder::errorKind_`'s pattern.
    SchemaTokenId identifierKind;
    SchemaTokenId errorKind;

    // Off-grammar token kinds — every `lexerModes.<name>.defaultToken.kind`
    // declared in the schema. These tokens are emitted by the tokenizer
    // (StringChar, BracketIdChar, CommentChar bodies) but the schema by
    // construction never references them in any shape; they're absorbed
    // into the current frame as leaves without advancing the parser's
    // schema walker. Sourced from `schema->bodyDefaultTokenKinds()`
    // (computed once by the loader; shared with `TreeBuilder`); held as
    // a pointer so the per-iteration drain loop doesn't re-fetch.
    std::unordered_set<SchemaTokenId> const* bodyDefaultTokenKinds = nullptr;

    // Lexer diagnostics from the tokenizer, optionally handed to the parser
    // so the finished Tree owns lexer + parser diagnostics in one stream
    // (08-compilation-unit-plan §2.6 C2-L1). Null when the caller doesn't
    // pass them (today: LSP + most tests). Ingested into the builder's
    // reporter at the start of parse(), before the walk.
    std::unique_ptr<DiagnosticReporter> lexerDiagnostics;

    // Builder lives in Impl (heap) since `TreeBuilder` is non-movable
    // (static_assert) and Impl is constructed in-place.
    std::unique_ptr<TreeBuilder> builder;

    // Frame guards held in declaration order; the back is the
    // innermost open frame.
    std::vector<TreeBuilder::OpenScope> frames;

    // Parallel rule chain — `frameRules[i]` is the RuleId of the
    // frame at the same index in `frames`. Maintained in lock-step
    // with frame push/pop so recovery code can walk the rule chain
    // without going through builder-internal state. Notably, this
    // lets `panicRecover` find the nearest compiled-body ancestor's
    // FOLLOW set when the immediate `currentRule()` is an auto-
    // interned Pratt wrapper (per the active schema's
    // `expr.wrapperRules.{binary,unary,postfix}` — names are
    // config-sourced, none of these wrapper rules have compiled bodies).
    std::vector<RuleId>                 frameRules;

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

    // Pratt-walker recursion depth. Incremented on every recursive
    // call into `DefaultPrattWalker`'s climb routine; fatal-aborts
    // when it would exceed `config.maxExpressionDepth`. Bounds C++
    // stack growth for adversarial right-recursion (deeply nested
    // parens, long right-assoc chains). Same posture as the
    // forward-progress watchdog — better to halt loudly than risk a
    // silent stack overflow.
    std::size_t expressionDepth = 0;

    // Operator-precedence walker, looked up once per `expr`-rule
    // dispatch. Owned by Impl — either moved out of
    // `ParserConfig::prattWalker` (caller-provided override) or
    // default-constructed by `parse()` on first use.
    std::unique_ptr<PrattWalker> prattWalker;

    // Back-pointer to the owning `Parser`. The Pratt-walker dispatch
    // path needs to pass a `Parser&` to `walkExpression` (the
    // public virtual takes `Parser&`, not `Impl&`); using a stable
    // pointer set at ctor time avoids threading the outer reference
    // through every dispatch site.
    Parser* outer = nullptr;

    Impl(std::shared_ptr<SourceBuffer>        s,
         std::shared_ptr<GrammarSchema const> sc,
         TokenStream                          ts,
         ParserConfig                         cfg)
        : src(std::move(s))
        , schema(std::move(sc))
        , tokens(std::move(ts))
        , config{
            cfg.maxSpeculationDepth,
            cfg.maxExpressionDepth,
            cfg.recoveryStrategy,
            cfg.maxSyncScanTokens,
            nullptr,
          }
        , identifierKind(schema->schemaTokens().find("Identifier"))
        , errorKind(schema->schemaTokens().find("Error"))
        , bodyDefaultTokenKinds(&schema->bodyDefaultTokenKinds())
        , walker(schema)
        , prattWalker(std::move(cfg.prattWalker)) {}

    // Default Pratt walker has friend access to drive the parser's
    // token stream, builder, schema walker, and frame stack
    // surgically. User-supplied walkers (passed via
    // `ParserConfig::prattWalker`) can only access the public
    // `Parser` API — none today; the API is YAGNI until a real
    // consumer asks for it.
    friend class DefaultPrattWalker;

    // ── helpers ──────────────────────────────────────────────────────

    void emitDiag(ParseDiagnostic d) {
        builder->reportDiagnostic(std::move(d));
        ++diagsEmitted;
    }

    // Render `actual` for a diagnostic. Three branches:
    //   - Eof core kind → `'<eof>'` (quoted consistently with other
    //     actuals so the renderer's `got 'X'` prose holds).
    //   - non-empty span → the source lexeme bytes.
    //   - zero-width non-Eof token (rare; synthesized recovery) →
    //     the schema token-kind name when known, else `'<unknown>'`.
    [[nodiscard]] std::string renderActual(Token const& tok) const {
        if (tok.coreKind == CoreTokenKind::Eof) return "'<eof>'";
        if (tok.span.start() < tok.span.end()) {
            return std::format("'{}'", src->slice(tok.span));
        }
        const SchemaTokenId kind =
            effectiveKind(tok, identifierKind, errorKind);
        if (kind.valid()) {
            return std::format("'{}'", schema->schemaTokens().name(kind));
        }
        return "'<unknown>'";
    }

    // Render the parser's `expected` list from a schema-token-id
    // set. Skips InvalidSchemaToken sentinels (a malformed
    // compiled rule with a stale id would otherwise render as the
    // misleading empty-quotes string `''`).
    [[nodiscard]] std::vector<std::string>
    renderExpectedNames(std::span<SchemaTokenId const> set) const {
        std::vector<std::string> out;
        out.reserve(set.size());
        for (auto tok : set) {
            if (!tok.valid()) continue;
            out.push_back(std::format("'{}'",
                                      schema->schemaTokens().name(tok)));
        }
        return out;
    }

    // Build + emit a parser-side diagnostic with severity Error,
    // `buffer = src->id()`, and `ruleContext = builder->currentRule()`
    // populated. Centralises the seven-field scaffold previously
    // repeated at each emission site. Optional `expected` argument
    // carries the schema-token-id set the cursor would accept — the
    // renderer prints "expected 'X' or 'Y' — got 'Z'".
    void emitParserError(DiagnosticCode code, SourceSpan span,
                         std::string actual,
                         std::span<SchemaTokenId const> expected = {}) {
        ParseDiagnostic d;
        d.code        = code;
        d.severity    = DiagnosticSeverity::Error;
        d.buffer      = src->id();
        d.span        = span;
        d.ruleContext = frames.empty()
                            ? std::optional<RuleId>{}
                            : std::optional<RuleId>{builder->currentRule()};
        d.actual      = std::move(actual);
        if (!expected.empty()) {
            d.expected = renderExpectedNames(expected);
        }
        emitDiag(std::move(d));
    }

    // Lookup helper: returns true when `kind` is in the FOLLOW set
    // of the nearest enclosing rule that has a non-empty followSet.
    // Walking frames lets recovery succeed inside Pratt wrappers
    // (the schema's `expr.wrapperRules.{binary,unary,postfix}` rules
    // — auto-interned per config, no compiled body, empty followSet)
    // by consulting the surrounding compiled rule. Stops at the
    // first non-empty followSet rather than unioning all ancestors —
    // the innermost compiled context is the precise resync horizon.
    [[nodiscard]] bool followContains(SchemaTokenId kind) const noexcept {
        for (auto it = frameRules.rbegin(); it != frameRules.rend(); ++it) {
            const auto follow = schema->followSetOf(*it);
            if (follow.empty()) continue;
            return std::ranges::find(follow, kind) != follow.end();
        }
        return false;
    }

    // Panic-mode recovery: insert an Error leaf at the bad token,
    // then scan forward until peek is a stopping point:
    //   - EOF — always stops
    //   - lexer Error coreKind — already noisy, don't merge into recovery
    //   - schema-declared `syncTokens` (e.g. `;`, `}`)
    //   - FOLLOW of the nearest compiled enclosing rule
    //   - `maxSyncScanTokens` adversarial-input cap
    //
    // Returns the count consumed (always ≥ 1 so the forward-progress
    // watchdog is satisfied). `SingleToken` mode short-circuits.
    //
    // Tokens consumed beyond the first are not driven through the
    // builder's `pushToken` (would advance the schema walker through
    // off-grammar positions and emit `P_SchemaCursorDesync` per byte)
    // — but TRIVIA is preserved by pushing it through (whitespace +
    // comment nodes survive recovery so editor selections covering
    // the recovered range still see something structurally meaningful).
    std::size_t panicRecover() {
        builder->pushErrorNode(tokens.peek().span);
        (void)tokens.advance();
        if (config.recoveryStrategy == RecoveryStrategy::SingleToken) {
            return 1;
        }

        const auto sync = schema->syncTokens();
        std::size_t consumed = 1;
        while (consumed < config.maxSyncScanTokens) {
            Token const& peek = tokens.peek();
            if (peek.coreKind == CoreTokenKind::Eof)   break;
            if (peek.coreKind == CoreTokenKind::Error) break;
            if (isSkippableTrivia(peek)) {
                builder->pushToken(tokens.advance());
                ++consumed;
                continue;
            }
            const SchemaTokenId kind =
                effectiveKind(peek, identifierKind, errorKind);
            if (std::ranges::find(sync, kind) != sync.end()) break;
            if (followContains(kind)) break;
            (void)tokens.advance();
            ++consumed;
        }
        return consumed;
    }

    // Emit a recovery diagnostic + run panic-mode. The shape is
    // identical at all six dispatch-loop recovery sites, so the
    // helper documents what a "recovery emission" actually is:
    // rich diag (with `expected` from the schema cursor) + Error
    // leaf at the bad token + scan to the next sync/follow point.
    StepOutcome recoverAt(DiagnosticCode code,
                          Token const& peek,
                          std::span<SchemaTokenId const> expected) {
        emitParserError(code, peek.span, renderActual(peek), expected);
        (void)panicRecover();
        return StepOutcome::Continue;
    }

    void closeFrameOnce() noexcept {
        if (frames.empty()) return;
        const RuleId rule = builder->currentRule();
        walker.leaveRule(SourceSpan::empty(0), rule);
        frames.back().close();
        frames.pop_back();
        if (!frameRules.empty()) frameRules.pop_back();
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
                    if (!impl_.frameRules.empty()) {
                        impl_.frameRules.pop_back();
                    }
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

    // Drive the dispatch loop until the parser-frame stack drops back
    // to `targetDepth`. Used by `DefaultPrattWalker` to delegate atom
    // parsing to the schema-driven recursive-descent machinery — the
    // walker opens an atom rule frame and then hands off until that
    // frame closes. Mirrors the inner-loop pattern in
    // `trySpeculativeBranch` minus the failure-mode bookkeeping.
    //
    // On `stepOnce → Done` mid-atom (EOF before the atom frame closed),
    // emits `P_PrematureEndOfInput` and forcibly closes remaining
    // frames down to `targetDepth`. Silent return would leave the
    // walker with an unbalanced frame stack and the caller would close
    // the wrong rule on its own teardown.
    void parseUntilFrameDepth(std::size_t targetDepth) {
        while (frames.size() > targetDepth) {
            if (stepOnce() == StepOutcome::Done) {
                if (frames.size() > targetDepth) {
                    emitParserError(
                        DiagnosticCode::P_PrematureEndOfInput,
                        tokens.peek().span,
                        "premature end of input inside Pratt-walker atom");
                    while (frames.size() > targetDepth) closeFrameOnce();
                }
                return;
            }
        }
    }

    // Open a frame for `rule` (parser side + builder side + parser
    // walker entry), mirroring the inline pattern previously repeated
    // at every walker open site. Pushes to the parallel `frameRules`
    // stack so recovery code can walk the rule chain.
    void openExprFrame(RuleId rule) {
        pushFrameBookkeeping_(builder->open(rule), rule);
    }

    // Left-recursive wrap: open `rule` as a new frame that adopts the
    // current frame's most-recent pending child as its first child.
    // Pratt-walker shim around `TreeBuilder::wrapLastChildInFrame` —
    // identical bookkeeping tail to `openExprFrame`; differs only in
    // the builder call.
    void wrapLastChildExprFrame(RuleId rule) {
        pushFrameBookkeeping_(builder->wrapLastChildInFrame(rule), rule);
    }

    // Frame bookkeeping common to both `openExprFrame` and
    // `wrapLastChildExprFrame`: take ownership of the builder-side
    // OpenScope guard, register the parallel `frameRules` entry the
    // recovery code uses for FOLLOW-set walking, and step the
    // parser-side schema walker into the rule. Same enterRule
    // semantics regardless of which builder operation produced the
    // guard (the cleanup `closeFrameOnce()` is symmetric).
    void pushFrameBookkeeping_(TreeBuilder::OpenScope&& guard, RuleId rule) {
        frames.push_back(std::move(guard));
        frameRules.push_back(rule);
        walker.enterRule(rule);
    }

    // Advance over `peek` as an operator token: push to builder, step
    // the parser walker. Returns false when peek is EOF (caller
    // should treat that as a structural error — see H4 in the PA2
    // review notes); true on a successful push.
    [[nodiscard]] bool pushOperatorToken() {
        Token const& peek = tokens.peek();
        if (peek.coreKind == CoreTokenKind::Eof) return false;
        const SchemaTokenId kind =
            effectiveKind(peek, identifierKind, errorKind);
        const Token opTok = tokens.advance();
        builder->pushToken(opTok);
        walker.advance(kind, opTok.span,
                       std::optional<RuleId>{builder->currentRule()});
        return true;
    }

    // Four-machine snapshot used by `DefaultPrattWalker` to roll back
    // a tentatively-built primary so it can be rebuilt inside a wrap
    // (the schema's `expr.wrapperRules.binary` / `.postfix` rules).
    // Distinct from `SpeculationProbe` —
    // the walker's rollback is always intentional (no commit-vs-fail
    // discipline) and the budget / watchdog re-baseline aren't
    // applicable. Token bookmark + builder checkpoint + schema-walker
    // snapshot + frame depth + diag counter, restored in dtor order.
    struct WalkerSnapshot {
        TokenStream::Bookmark                  tokenBookmark;
        std::optional<TreeBuilder::Checkpoint> builderCp;
        std::optional<SchemaWalker::Snapshot>  walkerSnap;
        std::size_t                            frameDepth;
        std::size_t                            diagsBefore;
    };

    [[nodiscard]] WalkerSnapshot snapForWalker() {
        return WalkerSnapshot{
            tokens.mark(),
            builder->checkpoint(),
            walker.snapshot(),
            frames.size(),
            diagsEmitted,
        };
    }

    // Rolls back the four-machine state to the snapshot. Consumes
    // `s.builderCp` / `s.walkerSnap` (move-only). Callers that want
    // to rollback AGAIN must re-snap before each rollback. Callers
    // that DON'T rollback must call `commitWalkerSnap` before the
    // snapshot goes out of scope — otherwise the embedded
    // `TreeBuilder::Checkpoint` dtor silently rolls back everything
    // built since the snap (the "uncommitted Checkpoint" RAII rule).
    void rollbackForWalker(WalkerSnapshot& s) noexcept {
        while (frames.size() > s.frameDepth) {
            frames.pop_back();
            if (!frameRules.empty()) frameRules.pop_back();
        }
        walker.restore(std::move(*s.walkerSnap));
        builder->rollback(std::move(*s.builderCp));
        tokens.restore(s.tokenBookmark);
        diagsEmitted = s.diagsBefore;
    }

    // Commit the snapshot's builder Checkpoint so its dtor doesn't
    // auto-rollback the post-snap work. Used when the walker decides
    // the tentatively-built primary stands on its own (no op at >=
    // minPrec) and the snap should be discarded.
    void commitWalkerSnap(WalkerSnapshot& s) noexcept {
        if (s.builderCp) builder->commit(std::move(*s.builderCp));
    }

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

        openExprFrame(branch);

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
            emitParserError(DiagnosticCode::P_MissingRequiredChild,
                            tokens.peek().span,
                            "'<eof>'",
                            walker.expectedSet());
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

        // Off-grammar body-token drain. The tokenizer emits leaves
        // for every codepoint of a string body / bracket-id body /
        // comment body (`StringChar`, `BracketIdChar`, `CommentChar`,
        // …). The schema by construction never references those
        // kinds in any shape — the loader rejects shapes that do.
        // They reach the AST as leaves under the current frame but
        // never advance the parser's schema walker. Without this
        // drain, the per-slot dispatch would see a body token,
        // fail the expected-set match, and route to `recoverAt` —
        // turning every string contents into a diagnostic storm.
        // Mirrors `TreeBuilder::pushToken`'s body-mode cursor skip.
        //
        // Positioned AFTER the watchdog snap so a runaway drain
        // can't mask a dispatch stall: the next iteration's snap
        // sees the post-drain (cursor, tokPos, depth) and the
        // watchdog still trips when dispatch fails to advance.
        while (!tokens.isAtEnd()) {
            Token const& bodyPeek = tokens.peek();
            if (bodyPeek.coreKind == CoreTokenKind::Eof) break;
            const SchemaTokenId k =
                effectiveKind(bodyPeek, identifierKind, errorKind);
            if (!bodyDefaultTokenKinds->contains(k)) break;
            const std::size_t before = tokens.position();
            builder->pushToken(tokens.advance());
            if (tokens.position() == before) {
                // Defensive: a non-EOF body token must advance the
                // stream. If the tokenizer ever regressed to emit a
                // zero-width body token (e.g. an unterminatedAs
                // backfill at EOF), looping here would spin until
                // OOM. Abort loudly instead.
                fatal("dss::Parser: body-token drain failed to advance");
            }
        }

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

            return recoverAt(DiagnosticCode::P_UnexpectedToken,
                             peek, expected);
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
                // `expr`-shape rules hand off to the Pratt walker
                // instead of the schema-driven RuleLeaf descent. The
                // walker opens its own `exprRule` frame plus any
                // wrapper frames (the three rules declared in
                // `expr.wrapperRules.{binary,unary,postfix}` — names
                // are config-sourced); on return the frame stack is balanced
                // with entry. The walker is responsible for advancing
                // tokens — the watchdog's next iteration will observe
                // the post-walker (cursor, tokPos, depth) tuple and
                // not trip since at least the cursor moved.
                if (schema->isExprRule(nextRule)) {
                    prattWalker->walkExpression(
                        *outer, nextRule,
                        schema->exprMinPrecedence(nextRule));
                    return StepOutcome::Continue;
                }
                openExprFrame(nextRule);
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
                openExprFrame(nextRule);
                closeFrameOnce();
                return StepOutcome::Continue;
            }

            return recoverAt(DiagnosticCode::P_NoAlternativeMatched,
                             peek, firstSet);
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
                    return recoverAt(DiagnosticCode::P_BacktrackFailed,
                                     peek, expected);
                }

                // Token-leaf branch route (mirror of the non-speculative
                // path at lines 1012-1020). A token-leaf alt branch
                // consumes EXACTLY one token at the alt position — there
                // is no internal failure mode that would warrant
                // speculation (the only outcome is "this token matches
                // the branch's token kind, advance" — the next step's
                // failure happens at the alt's PARENT scope, not inside
                // the branch). So token-leaf branches participate
                // unconditionally in speculative alts; speculation only
                // matters for rule branches whose body may fail mid-
                // parse. Without this route, an alt like operand's
                // `[Identifier, IntLiteral, ..., compoundLiteralExpr,
                // parenExpr]` rejects every token-leaf primary
                // (P_BacktrackFailed even on `int x = 5;`) when the
                // alt is marked speculative.
                const SchemaCursor afterAdvance =
                    schema->advance(walker.cursor(), tokKind);
                if (afterAdvance.valid()) {
                    const Token tok = tokens.advance();
                    builder->pushToken(tok);
                    walker.advance(tokKind, tok.span,
                                   std::optional<RuleId>{builder->currentRule()});
                    return StepOutcome::Continue;
                }

                // Try each candidate RULE branch in declaration order.
                // First success commits and falls through to the
                // next outer iteration; full failure emits one
                // P_BacktrackFailed + consumes for forward progress.
                const auto candidates = candidateBranches(tokKind);
                for (auto const branch : candidates) {
                    if (trySpeculativeBranch(branch)) {
                        return StepOutcome::Continue;
                    }
                }

                return recoverAt(DiagnosticCode::P_BacktrackFailed,
                                 peek, expected);
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

                return recoverAt(DiagnosticCode::P_NoAlternativeMatched,
                                 peek, expected);
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
                // `expr`-shape rules MUST go through the Pratt walker
                // even when discovered via the AltChoice→RuleLeaf
                // scan. Without this, an `expression` reachable from
                // an optional/repeat slot would be parsed as a plain
                // atom and the operator climb never runs (e.g.
                // `return f(x)` where the optional[expression] under
                // returnStmt was hit via this path).
                if (schema->isExprRule(candidate)) {
                    prattWalker->walkExpression(
                        *outer, candidate,
                        schema->exprMinPrecedence(candidate));
                    return StepOutcome::Continue;
                }
                openExprFrame(candidate);
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
            return recoverAt(DiagnosticCode::P_NoAlternativeMatched,
                             peek, expected);
        }

        }   // switch slotKind

        // Unreachable: switch is exhaustive over SlotKind.
        return StepOutcome::Continue;
    }
};

// Mirror the `TreeBuilder` pin so a future refactor that tries to
// store `Impl` by value (rather than via `unique_ptr`) fails at compile
// time instead of silently breaking the `bodyDefaultTokenKinds` raw
// pointer's lifetime contract.
static_assert(!std::is_move_constructible_v<Parser::Impl>,
              "Parser::Impl must remain non-movable — its "
              "bodyDefaultTokenKinds pointer is bound at ctor time and a "
              "move would silently rebind to dangling state");

Parser::Parser(std::shared_ptr<SourceBuffer>        src,
               std::shared_ptr<GrammarSchema const> schema,
               TokenStream                          tokens,
               ParserConfig                         config,
               std::unique_ptr<DiagnosticReporter>  lexerDiagnostics) {
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
    if (config.maxExpressionDepth == 0) {
        fatal("dss::Parser::Parser: maxExpressionDepth must be >= 1");
    }
    if (config.maxSyncScanTokens == 0) {
        fatal("dss::Parser::Parser: maxSyncScanTokens must be >= 1");
    }
    impl_ = std::make_unique<Impl>(std::move(src),
                                   std::move(schema),
                                   std::move(tokens),
                                   std::move(config));
    impl_->lexerDiagnostics = std::move(lexerDiagnostics);
    impl_->outer = this;
}

Parser::~Parser() = default;

// ── parse() ─────────────────────────────────────────────────────────────

ParseResult Parser::parse() && {
    auto& I = *impl_;

    I.builder = std::make_unique<TreeBuilder>(I.src, I.schema);

    // Fold the tokenizer's lexer diagnostics into the builder's reporter
    // before the walk, so the finished Tree owns lexer + parser diagnostics
    // in one stream (08-compilation-unit-plan §2.6 C2-L1). No-op when the
    // caller didn't pass them.
    if (I.lexerDiagnostics) {
        I.builder->ingestDiagnostics(I.lexerDiagnostics->all());
    }

    // Default-construct the Pratt walker if the caller didn't inject
    // one through `ParserConfig::prattWalker`. Always-present so the
    // `isExprRule` dispatch path in `stepOnce` can unconditionally
    // invoke `prattWalker->walkExpression`.
    if (!I.prattWalker) {
        I.prattWalker = std::make_unique<DefaultPrattWalker>();
    }

    const RuleId rootRule = I.schema->rootCursor().rule();
    if (!rootRule.valid()) {
        fatal("dss::Parser::parse: schema has no root rule");
    }

    I.openExprFrame(rootRule);

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

// ── DefaultPrattWalker ──────────────────────────────────────────────────
//
// Schema-driven operator-precedence climber. Produces a right-recursive
// tree wrapping operator results in the three rules declared by the
// schema's `expr.wrapperRules.{binary,unary,postfix}` block — names
// are config-sourced (loader auto-interns them when any `expr` shape
// is declared). Left- vs. right-associativity is encoded in the
// operator table, not in the tree's structural nesting — a downstream
// semantic pass reads associativity from
// `operatorTable().lookup(kind, Infix)->associativity`.
//
// Each `parseExpressionAt(minPrec)` call:
//   1. Takes a four-machine snapshot (tokens, builder, walker, frames,
//      diag counter).
//   2. Parses a primary (prefix-op + nested expression OR the atom).
//   3. Loops: while peek is an op at >= minPrec, rolls back to the
//      snapshot, opens a wrapper, rebuilds LHS via recursion with
//      minPrec = op.prec + 1 (so LHS doesn't gobble this same op),
//      pushes the op, recurses RHS at op.prec (right-recursive).
//      Each iteration strictly extends the consumed range — no loop.
namespace {

// Bundles the wrapper rule ids resolved once per `walkExpression`
// entry. Threading these through avoids re-doing the same `rules()
// .find(...)` lookup at every climb level + makes the dependency on
// the loader's auto-intern explicit. Field names mirror the
// schema-side `ExprWrapperRules` (binary/unary/postfix) so there's
// no translation layer between schema and walker.
struct PrattRules {
    RuleId atom;
    RuleId binary;
    RuleId unary;
    RuleId postfix;
    RuleId ternary;   // optional (invalid when the language has no `?:`)
};

// Push trivia tokens through the builder until peek is meaningful.
// Mirrors the main dispatch loop's TokenLeaf-trivia passthrough so
// whitespace lands inside the wrapper frame that's currently open.
void pumpTrivia(Parser::Impl& I) {
    while (!I.tokens.isAtEnd() && isSkippableTrivia(I.tokens.peek())) {
        I.builder->pushToken(I.tokens.advance());
    }
}

void parseExpressionAt(Parser::Impl& I, PrattRules const& rules,
                       std::int32_t minPrec);

// Emit one primary as a child of the current frame: either a
// prefix-op + nested expression, or the schema's atom rule.
void parsePrimary(Parser::Impl& I, PrattRules const& rules,
                  std::int32_t minPrec) {
    pumpTrivia(I);
    Token const& peek = I.tokens.peek();
    const SchemaTokenId kind =
        effectiveKind(peek, I.identifierKind, I.errorKind);

    const auto prefix = I.schema->operatorTable().lookup(
        kind, OperatorArity::Prefix);
    if (prefix && prefix->precedence >= minPrec) {
        I.openExprFrame(rules.unary);
        // pushOperatorToken returns false only on EOF, which can't
        // happen here — we already inspected peek via effectiveKind
        // and got a real Prefix entry from the operator table.
        (void)I.pushOperatorToken();
        // Recurse at the prefix's own precedence so tighter ops bind
        // inside the operand (e.g. `-a * b` parses as
        // unary[-, binary[a, *, b]] when `*` is tighter — wrapper
        // rule names come from `expr.wrapperRules`).
        parseExpressionAt(I, rules, prefix->precedence);
        I.closeFrameOnce();
        return;
    }

    const std::size_t depthBeforeAtom = I.frames.size();
    I.openExprFrame(rules.atom);
    I.parseUntilFrameDepth(depthBeforeAtom);
}

void parseExpressionAt(Parser::Impl& I, PrattRules const& rules,
                       std::int32_t minPrec) {
    if (I.expressionDepth >= I.config.maxExpressionDepth) {
        fatal("dss::DefaultPrattWalker: expression recursion depth "
              "exceeded ParserConfig::maxExpressionDepth — deeply "
              "nested parens or right-associative chains would have "
              "blown the C++ call stack");
    }
    ++I.expressionDepth;
    struct DepthGuard {
        std::size_t& depth;
        ~DepthGuard() { --depth; }
    } guard{I.expressionDepth};

    auto snap = I.snapForWalker();
    parsePrimary(I, rules, minPrec);

    while (true) {
        pumpTrivia(I);
        Token const& peek = I.tokens.peek();
        if (peek.coreKind == CoreTokenKind::Eof) {
            I.commitWalkerSnap(snap);
            return;
        }
        const SchemaTokenId kind =
            effectiveKind(peek, I.identifierKind, I.errorKind);

        const auto infix = I.schema->operatorTable().lookup(
            kind, OperatorArity::Infix);
        const auto postfix = I.schema->operatorTable().lookup(
            kind, OperatorArity::Postfix);
        const auto ternary = I.schema->operatorTable().lookup(
            kind, OperatorArity::Ternary);

        const bool postfixInClimb =
            postfix && postfix->precedence >= minPrec;
        const bool infixInClimb =
            infix && infix->precedence >= minPrec;
        // A ternary operator only participates when the schema also declared a
        // `wrapperRules.ternary` rule to wrap it; without one the `?` falls
        // through and the parent surfaces a parse error (rather than silently
        // dropping it).
        const bool ternaryInClimb =
            ternary && ternary->precedence >= minPrec && rules.ternary.valid();

        if (!postfixInClimb && !infixInClimb && !ternaryInClimb) {
            I.commitWalkerSnap(snap);
            return;
        }

        // Postfix wins ties with infix at the same precedence — it
        // binds the lhs alone (no further operand to gather), so
        // structurally it's the more local binding. Grouped postfix
        // (`grouped.has_value()`) extends the simple `++` case with a
        // body-rule frame parsed between opener and closer.
        //
        // Postfix is LEFT-associative: iterative wrapping via
        // `wrapLastChildExprFrame` wraps the previously-built primary
        // (or a prior chain wrap) as the new postfix-wrapper's first
        // child. The pre-primary `snap` is INTENTIONALLY NOT advanced
        // across postfix iterations — a later same-level infix iter
        // (`f(a) + g(b)`) needs that snap to roll the entire chain
        // back and rebuild it inside the binary-wrapper via the
        // recursive LHS parse at `prec + 1`. Using rollback-replay
        // (infix's strategy) for postfix would lose iter-1's wrap on iter-2.
        if (postfixInClimb) {
            I.wrapLastChildExprFrame(rules.postfix);
            if (!I.pushOperatorToken()) {
                // Truly defensive: peek was non-Eof when we entered
                // this iteration, so pushOperatorToken should have
                // succeeded. If it fails here something has corrupted
                // the token stream — emit a diagnostic so the failure
                // is observable rather than producing a silently
                // half-built wrap.
                I.emitParserError(
                    DiagnosticCode::P_PrematureEndOfInput,
                    peek.span,
                    "expression ended before postfix operator");
                I.closeFrameOnce();
                I.commitWalkerSnap(snap);
                return;
            }
            if (postfix->followerRule.has_value()) {
                // D5.1: follower-rule postfix — operator + exactly one
                // occurrence of a named rule (e.g. `.field` is `DotOp` +
                // `memberFollower` wrapping `Identifier`). No closer; the
                // rule's own shape terminates the body. Mutually exclusive
                // with `grouped` (loader enforces).
                RuleId const fr = *postfix->followerRule;
                if (I.schema->isExprRule(fr)) {
                    I.prattWalker->walkExpression(
                        *I.outer, fr,
                        I.schema->exprMinPrecedence(fr));
                } else {
                    const std::size_t bodyDepth = I.frames.size();
                    I.openExprFrame(fr);
                    I.parseUntilFrameDepth(bodyDepth);
                }
                I.closeFrameOnce();
                continue;
            }
            if (postfix->grouped) {
                auto const& gp = *postfix->grouped;
                // Type-level invariant pinned at the deref site: the
                // loader (`grammar_schema_json.cpp`) only constructs
                // `grouped` when `endsAtId.valid()`, but the struct
                // shape doesn't enforce that on aggregate init. Catch
                // any future producer that bypasses the loader.
                if (!gp.endsAt.valid()) {
                    fatal("dss::DefaultPrattWalker: grouped postfix "
                          "entry has invalid endsAt — loader contract "
                          "violated");
                }
                // Grouped postfix: drive the body until the closer.
                // An expr-rule body (e.g. `[expression]` for indexing)
                // must run through the Pratt walker so operator climb
                // applies inside the brackets; calling `openExprFrame`
                // directly would skip precedence and produce a flat
                // tree. Same routing rule as `stepOnce`'s AltChoice→
                // RuleLeaf path for expr-rules.
                if (gp.bodyRule.valid()) {
                    if (I.schema->isExprRule(gp.bodyRule)) {
                        I.prattWalker->walkExpression(
                            *I.outer, gp.bodyRule,
                            I.schema->exprMinPrecedence(gp.bodyRule));
                    } else {
                        const std::size_t bodyDepth = I.frames.size();
                        I.openExprFrame(gp.bodyRule);
                        I.parseUntilFrameDepth(bodyDepth);
                    }
                }
                pumpTrivia(I);
                Token const& closerPeek = I.tokens.peek();
                const SchemaTokenId closerKind = effectiveKind(
                    closerPeek, I.identifierKind, I.errorKind);
                if (closerKind.v != gp.endsAt.v) {
                    // Closer missing. Emit the diagnostic AND drop an
                    // Error leaf at the missing-closer position so the
                    // HasError flag propagates up through the
                    // postfix wrapper and the tree carries the
                    // structural signal (not just a sidecar diagnostic).
                    // We do NOT consume `closerPeek` — the parent
                    // dispatch resumes from it (typically `recoverAt`
                    // scans to the next sync token).
                    I.emitParserError(
                        DiagnosticCode::P_MissingRequiredChild,
                        closerPeek.span,
                        I.renderActual(closerPeek),
                        std::span<SchemaTokenId const>{&gp.endsAt, 1});
                    I.builder->pushErrorNode(closerPeek.span);
                } else {
                    (void)I.pushOperatorToken();
                }
            }
            I.closeFrameOnce();
            // Snap intentionally NOT advanced — see header comment
            // above the postfix branch. A subsequent infix iteration
            // at the same level needs the original pre-primary snap
            // so its rollback-replay can wipe this postfix wrap and
            // rebuild the chain inside the binary-wrapper frame.
            continue;
        }

        // Ternary (mixfix `cond ? then : else`). Like infix, it gathers the
        // already-built primary as its condition via rollback-replay, then
        // parses the middle clause (to the `:` separator) and the else operand.
        // The middle parses at minPrec 0 — between `?` and `:` anything binds
        // (assignment, even a nested ternary); the climb naturally stops at `:`
        // (which carries no operator-table entry). The else parses at the
        // ternary's own precedence → right-associative (`a?b:c?d:e` = a?b:(c?d:e)).
        if (ternaryInClimb) {
            I.rollbackForWalker(snap);
            snap = I.snapForWalker();
            I.openExprFrame(rules.ternary);
            parseExpressionAt(I, rules, ternary->precedence + 1);   // condition
            pumpTrivia(I);
            if (!I.pushOperatorToken()) {                            // `?`
                I.emitParserError(DiagnosticCode::P_PrematureEndOfInput, peek.span,
                                  "expression ended before ternary '?'");
                I.closeFrameOnce();
                I.commitWalkerSnap(snap);
                return;
            }
            parseExpressionAt(I, rules, 0);                          // then (middle)
            pumpTrivia(I);
            Token const& midPeek = I.tokens.peek();
            const SchemaTokenId midKind =
                effectiveKind(midPeek, I.identifierKind, I.errorKind);
            if (!ternary->ternaryMiddle || midKind.v != ternary->ternaryMiddle->v) {
                // Missing `:`. Emit a diagnostic + an Error leaf so HasError
                // propagates through the ternary wrapper; don't consume midPeek
                // (parent recovery resumes from it).
                I.emitParserError(DiagnosticCode::P_MissingRequiredChild, midPeek.span,
                                  "ternary expression is missing its ':' separator");
                I.builder->pushErrorNode(midPeek.span);
                I.closeFrameOnce();
                I.commitWalkerSnap(snap);
                return;
            }
            (void)I.pushOperatorToken();                             // `:`
            parseExpressionAt(I, rules, ternary->precedence);        // else
            I.closeFrameOnce();
            continue;
        }

        // Infix path keeps the right-recursive rollback-replay: roll
        // back to before the primary was built, open the binary
        // wrapper, and rebuild the primary inside it. Re-snap to the
        // post-rollback state so a chained infix at lower precedence
        // can do the same dance.
        I.rollbackForWalker(snap);
        snap = I.snapForWalker();

        I.openExprFrame(rules.binary);
        // LHS: strictly tighter (op.prec + 1) so it doesn't gobble
        // this same-prec op (which would loop forever).
        parseExpressionAt(I, rules, infix->precedence + 1);
        pumpTrivia(I);
        if (!I.pushOperatorToken()) {
            I.emitParserError(
                DiagnosticCode::P_PrematureEndOfInput,
                peek.span,
                "expression ended before infix operator");
            I.closeFrameOnce();
            I.commitWalkerSnap(snap);
            return;
        }
        // RHS uses op.prec, so same-prec ops fold into a nested
        // binary-wrapper (right-recursive shape; left-assoc is conveyed
        // only via the operator table).
        parseExpressionAt(I, rules, infix->precedence);
        I.closeFrameOnce();
    }
}

} // namespace

void DefaultPrattWalker::walkExpression(Parser& parser,
                                        RuleId        exprRule,
                                        std::int32_t  minPrec) {
    auto& I = *parser.impl_;

    // Resolve atom + wrapper rules ONCE. Mid-climb the builder's
    // current rule is whichever wrapper we're inside, so
    // `exprAtom(currentRule())` would return InvalidRule.
    //
    // The wrapper rule names come from the language's
    // `expr.wrapperRules` schema block (08.55 cleanup) — the
    // engine no longer hardcodes any names. The loader has
    // already validated all three are present and interned;
    // `exprWrapperRules(exprRule)` returns a `.valid()` bundle
    // here for any rule the loader compiled as `isExprRule`.
    const auto wrapperPack = I.schema->exprWrapperRules(exprRule);
    PrattRules const rules{
        .atom    = I.schema->exprAtom(exprRule),
        .binary  = wrapperPack.binary,
        .unary   = wrapperPack.unary,
        .postfix = wrapperPack.postfix,
        .ternary = wrapperPack.ternary,   // invalid unless the schema declared one
    };
    if (!rules.atom.valid()) {
        fatal("dss::DefaultPrattWalker::walkExpression: exprRule's "
              "atom is InvalidRule — schema didn't compile this rule "
              "as `expr`-kind");
    }
    // Sanity check: the loader validated `wrapperRules` at config-
    // load time (`C_MissingWrapperRules`), so an unreachable case
    // by the time we get here. Keeping the guard catches a loader-
    // walker disagreement (e.g. a future loader bug that interned
    // wrapper names but forgot to record them on the owning rule).
    if (!wrapperPack.valid()) {
        fatal("dss::DefaultPrattWalker::walkExpression: schema did "
              "not record `expr.wrapperRules` for this rule — the "
              "loader's wrapper-rule pass disagreed with the parser "
              "(loader bug)");
    }

    I.openExprFrame(exprRule);
    parseExpressionAt(I, rules, minPrec);
    I.closeFrameOnce();
}

} // namespace dss
