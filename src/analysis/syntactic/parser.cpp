#include "analysis/syntactic/parser.hpp"

#include "analysis/syntactic/pratt_walker.hpp"
#include "core/types/compiled_shape.hpp"
#include "core/types/declarator_walk.hpp"
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
#include <string>
#include <string_view>
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

    // FC2 binder sketch — the parser-side scoped name→kind(type|value)
    // map fed by `semantics.declarations` + `semantics.scopes` (the SAME
    // vocabulary the semantic analyzer consumes; ONE declaration, two
    // consumers). Consulted by the speculative type-name commit triage
    // (`typeNameCommitApproved_`); recorded into at binder-rule frame
    // close (`closeFrameOnce`). Inert (`!enabled()`) for languages with
    // no declarations — toy/tsql pay nothing. Snapshot/restored across
    // speculation exactly like the other four state machines.
    BinderSketch sketch;

    // Forward-progress watchdog state (carried across iterations).
    SchemaCursor lastCursor{};
    std::size_t  lastTokPos = 0;
    std::size_t  lastDepth  = 0;
    bool         firstIteration = true;

    // FC4 c1: the all-candidates-failed fallback REPLAY's one-shot
    // bookkeeping (see the speculative AltChoice arm). A replayed branch
    // that fails without net consumption unwinds back to the same alt at
    // the same position; the (cursor, tokPos) pair gates the second
    // attempt into the P_BacktrackFailed forward-progress hatch instead.
    bool         replayedFallback_ = false;
    SchemaCursor lastReplayCursor_{};
    std::size_t  lastReplayTokPos_ = 0;

    // True iff the PREVIOUS dispatch step performed a recovery (panic scan
    // or missing-rule synthesis). Lets `synthesizeMissingRule` SUPPRESS its
    // diagnostic when it fires immediately after another recovery — i.e. when
    // a stop-point is reached while still resyncing the SAME broken region, so
    // one structural error yields one diagnostic (the file-level cascade-bound
    // quality bar) rather than a fresh "missing X" at every resync horizon.
    // Captured + restored across speculative probes (like `diagsEmitted`), so
    // a rolled-back probe cannot leave it stale.
    bool         stepRecovered_ = false;

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

    // Pratt-walker recursion depth. Incremented on every entry to
    // `parseExpressionAt` (the single chokepoint all expression-
    // deepening funnels through) and decremented by its RAII guard on
    // return. When it would exceed `config.maxExpressionDepth` the
    // walker emits a positioned `P_ExpressionTooDeep` diagnostic and
    // recovers (it does NOT abort). Bounds C++ stack growth for
    // adversarial right-recursion (deeply nested parens, long right-
    // assoc chains, deep prefix/ternary) — fail loud + recover rather
    // than risk a silent stack overflow.
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
            std::move(cfg.seedGlobalTypeNames),
          }
        , identifierKind(schema->schemaTokens().find("Identifier"))
        , errorKind(schema->schemaTokens().find("Error"))
        , bodyDefaultTokenKinds(&schema->bodyDefaultTokenKinds())
        , walker(schema)
        , sketch(*schema)
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

    // A token is a recovery "stop-point" when it belongs to an ENCLOSING
    // context: it is a schema-declared `syncTokens` member, or it is in the
    // FOLLOW set of the nearest enclosing compiled rule. This is exactly the
    // condition `panicRecover` uses to STOP its forward scan — lifted into a
    // named predicate so the scan loop and the missing-production branch in
    // `recoverAt` share ONE definition of "stop-point" (no second copy of
    // the rule). Grammar-driven: `syncTokens` + FOLLOW sets are config /
    // schema data, never a hardcoded token or language identity.
    [[nodiscard]] bool isStopPoint(SchemaTokenId kind) const noexcept {
        const auto sync = schema->syncTokens();
        if (std::ranges::find(sync, kind) != sync.end()) return true;
        return followContains(kind);
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
            if (isStopPoint(kind)) break;
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
        stepRecovered_ = true;
        return StepOutcome::Continue;
    }

    // Recover an OVER-DEEP expression: the Pratt walker's recursion
    // (nested parens, right-assoc RHS chains, prefix operands, ternary
    // clauses) reached `config.maxExpressionDepth`. Emit ONE positioned
    // `P_ExpressionTooDeep` at the offending token, drop an Error leaf,
    // and panic-scan forward to the next stop-point so the recursion
    // UNWINDS with forward progress instead of recursing one level
    // deeper and blowing the C++ call stack. Idempotent across the
    // unwind: only the deepest frame trips the guard (parents are
    // suspended mid-operand and resume their climb loop on a non-
    // operator peek), so exactly one diagnostic surfaces. The Error
    // leaf propagates HasError up through every wrapper frame.
    void recoverExpressionTooDeep_(Token const& peek) {
        // No `expected` set — the reporter renders `got <actual>`, so
        // `actual` carries the explanation plus the offending lexeme.
        emitParserError(
            DiagnosticCode::P_ExpressionTooDeep, peek.span,
            std::format(
                "{} — expression nesting is too deep (exceeds the "
                "maximum expression depth of {})",
                renderActual(peek), config.maxExpressionDepth));
        (void)panicRecover();
        stepRecovered_ = true;
    }

    // Recover a MISSING required RULE: the dispatch needs to descend into
    // `rule` but `peek` can't start it, `rule` isn't nullable, AND `peek` is
    // a STOP-POINT (a sync token or in an enclosing open frame's FOLLOW) — so
    // `peek` belongs to an enclosing context, NOT to `rule`. Panic-consuming
    // it (the `recoverAt` else-path) would SWALLOW a token the enclosing
    // grammar still needs — e.g. the statement terminator `;`, or a block
    // closer `}` whose `closesScope` must fire to balance the scope stack.
    // (That swallow is the `int x = ;` bug: with the `initValue` rule absent,
    // panic recovery consumed the block's `}` via `tokens.advance()` instead
    // of `pushToken`, so the scope never closed → "scope stack non-empty at
    // finish" + a Missing-node cascade.)
    //
    // The fix synthesizes `rule` as an EMPTY error subtree (open it, attach an
    // Error leaf, close it). Opening+closing the rule advances the PARENT's
    // schema cursor PAST this RuleLeaf slot (the same mechanism the nullable-
    // skip below uses), leaving the parent frame open to consume `peek` at its
    // next slot — so `varDeclHead` proceeds to its `EndStatement` and eats the
    // `;`, and the block later eats and scope-closes its `}`. `peek` is NOT
    // consumed.
    //
    // `emitDiagnostic` is false when this synthesis fires WHILE already
    // resyncing (the prior step recovered) — the broken region was already
    // diagnosed, so re-reporting "missing X" at every resync horizon would
    // violate the one-diagnostic-per-broken-region bar. The Error leaf is
    // still attached (HasError propagates), so the region stays marked errored
    // even when the diagnostic text is suppressed.
    //
    // Scoped narrowly (required RuleLeaf miss at a stop-point, non-speculative)
    // so the garbage path — a non-stop-point bad token, e.g. a stray `@` in an
    // expression — keeps its existing panic-scan-and-resync behavior untouched.
    void synthesizeMissingRule(RuleId rule, Token const& peek,
                               std::span<SchemaTokenId const> firstSet,
                               bool emitDiagnostic) {
        if (emitDiagnostic) {
            emitParserError(DiagnosticCode::P_NoAlternativeMatched,
                            peek.span, renderActual(peek), firstSet);
        }
        openExprFrame(rule);
        builder->pushErrorNode(peek.span);
        closeFrameOnce();
        stepRecovered_ = true;
    }

    void closeFrameOnce() noexcept {
        if (frames.empty()) return;
        const RuleId rule = builder->currentRule();
        // FC2 binder sketch: extract the declared name(s) BEFORE the frame
        // closes (its direct children are still in the builder's pending
        // staging area), but RECORD them after any scope this rule itself
        // owns has closed — a scope-opening declaration (e.g. a struct)
        // binds its NAME in the ENCLOSING scope, while its members were
        // bound into its own scope as their frames closed earlier.
        // (FC4 c1: a declarator-mode row's initDeclarator LIST yields
        // MULTIPLE bindings — `typedef int A, *B;` binds both — hence the
        // vector; legacy rows yield 0 or 1.)
        std::vector<std::pair<std::string, bool>> binderNames;
        if (sketch.enabled()) {
            if (auto const* decl = sketch.binderFor(rule)) {
                binderNames = extractBinderNames_(*decl);
            }
        }
        walker.leaveRule(SourceSpan::empty(0), rule);
        frames.back().close();
        frames.pop_back();
        if (!frameRules.empty()) frameRules.pop_back();
        if (sketch.enabled()) {
            if (sketch.isScopeRule(rule)) sketch.closeScope();
            for (auto& [name, isType] : binderNames) {
                sketch.record(std::move(name), isType);
            }
        }
    }

    // ── FC2 binder-sketch name extraction ───────────────────────────────
    //
    // Mirrors the semantic analyzer's declRoleChildren + extractNameNode
    // convention (visible non-EmptySpace children; optional specifier-
    // prefix strip; positional name child; Self single-wrapper descend /
    // LastIdentifier DFS) — but reads the BUILDER's in-progress arena at
    // frame-close time instead of a frozen Tree. ONE config declaration
    // (`semantics.declarations`), two consumers.

    // Resolve the name-bearing leaf below `node` per `mode`.
    //   Self           — descend single-visible-child wrappers to the leaf.
    //   LastIdentifier — DFS; the LAST identifierToken leaf wins.
    [[nodiscard]] NodeId resolveNameNode_(NodeId node,
                                          NameMatchMode mode) const {
        if (mode == NameMatchMode::Self) {
            while (builder->nodeKind(node) == NodeKind::Internal) {
                NodeId      only{};
                std::size_t count = 0;
                for (NodeId c : builder->nodeChildren(node)) {
                    if (isEmptySpace(builder->nodeFlags(c))) continue;
                    only = c;
                    if (++count > 1) break;
                }
                if (count != 1) return node;   // 0 or many → stop here
                node = only;
            }
            return node;
        }
        // LastIdentifier — track the latest identifier in source order.
        NodeId              found{};
        std::vector<NodeId> stack{node};
        while (!stack.empty()) {
            const NodeId cur = stack.back();
            stack.pop_back();
            if (builder->nodeKind(cur) == NodeKind::Token
                && builder->nodeTokenKind(cur).v == identifierKind.v) {
                found = cur;
            }
            const auto cs = builder->nodeChildren(cur);
            for (auto it = cs.rbegin(); it != cs.rend(); ++it) {
                if (!isEmptySpace(builder->nodeFlags(*it))) {
                    stack.push_back(*it);
                }
            }
        }
        return found;
    }

    // FC4 c1: the TreeBuilder-substrate adapter for the SHARED declarator
    // name-extraction walk (`core/types/declarator_walk.hpp` — the one
    // walk implementation, two substrates). Valid because the about-to-
    // close frame's pending children are CLOSED nodes (children ranges
    // finalized), so the builder's node accessors are exact below them.
    struct BuilderDeclaratorView {
        TreeBuilder const& b;
        [[nodiscard]] NodeKind kind(NodeId n) const { return b.nodeKind(n); }
        [[nodiscard]] RuleId rule(NodeId n) const { return b.nodeRule(n); }
        [[nodiscard]] SchemaTokenId tokenKind(NodeId n) const {
            return b.nodeTokenKind(n);
        }
        [[nodiscard]] bool isVisible(NodeId n) const {
            return !isEmptySpace(b.nodeFlags(n));
        }
        [[nodiscard]] std::span<NodeId const> children(NodeId n) const {
            return b.nodeChildren(n);
        }
    };

    // Extract every (name, isType) binding from the ABOUT-TO-CLOSE top
    // frame — a binder rule per the sketch's view. The frame's direct
    // children are its pending staging range; everything BELOW the top
    // level is already closed (children ranges finalized), so descent is
    // safe. Returns empty when the name child is structurally absent or
    // not a plain identifier leaf (e.g. an errored declaration) — the
    // sketch then simply has no entry and the name reads as Unknown, the
    // safe direction (semantic analysis remains the authority).
    //
    // Legacy rows yield 0 or 1 entries; FC4 declarator-mode rows yield one
    // entry PER named declarator below the carrier child (abstract
    // declarators contribute nothing — a legal outcome).
    [[nodiscard]] std::vector<std::pair<std::string, bool>>
    extractBinderNames_(BinderSketch::BinderDecl const& decl) const {
        std::vector<std::pair<std::string, bool>> out;
        std::vector<NodeId> kids;
        for (NodeId c : builder->currentFramePendingChildren()) {
            if (!isEmptySpace(builder->nodeFlags(c))) kids.push_back(c);
        }
        // Specifier-prefix strip (mirrors semantic's declRoleChildren) —
        // positional indices stay stable whether or not specifiers
        // (`static`, `__attribute__((...))`) are present.
        if (decl.specifierPrefixRule.valid() && !kids.empty()
            && builder->nodeKind(kids.front()) == NodeKind::Internal
            && builder->nodeRule(kids.front()).v
                   == decl.specifierPrefixRule.v) {
            kids.erase(kids.begin());
        }
        if (decl.declaratorMode) {
            // FC4 c1: the shared declarator walk over the builder
            // substrate. The sketch only enrolls declarator-mode rows when
            // the schema declares the `declarators` block (ctor invariant),
            // so the dereference is guarded-by-construction; the null check
            // keeps the miss loud-by-absence rather than UB.
            auto const& dcOpt = schema->semantics().declarators;
            if (!dcOpt.has_value() || decl.carrierChild >= kids.size()) {
                return out;
            }
            BuilderDeclaratorView const view{*builder};
            std::vector<NodeId> declarators;
            collectDeclarators(view, kids[decl.carrierChild], *dcOpt,
                               declarators);
            for (NodeId d : declarators) {
                const NodeId nameNode = declaratorNameNode(view, d, *dcOpt);
                if (!nameNode.valid()) continue;   // abstract — no binding
                out.emplace_back(
                    std::string{src->slice(builder->nodeSpan(nameNode))},
                    decl.isType);
            }
            return out;
        }
        if (decl.nameChild >= kids.size()) return out;
        const NodeId nameNode =
            resolveNameNode_(kids[decl.nameChild], decl.nameMatch);
        if (!nameNode.valid()
            || builder->nodeKind(nameNode) != NodeKind::Token
            || builder->nodeTokenKind(nameNode).v != identifierKind.v) {
            return out;
        }
        out.emplace_back(std::string{src->slice(builder->nodeSpan(nameNode))},
                         decl.isType);
        return out;
    }

    // ── FC2 type-name commit triage ─────────────────────────────────────

    // Collect non-trivia LEAF nodes under `node` (pre-order), stopping
    // once `out.size() > cap` — the triage only ever distinguishes
    // "exactly one" from "more". Error/Missing leaves count as content
    // (a non-identifier form → rule-1 commit; semantic diagnoses).
    void collectLeavesBelow_(NodeId node, std::vector<NodeId>& out,
                             std::size_t cap) const {
        if (out.size() > cap) return;
        const NodeKind k = builder->nodeKind(node);
        if (k != NodeKind::Internal) {
            if (!isEmptySpace(builder->nodeFlags(node))) out.push_back(node);
            return;
        }
        for (NodeId c : builder->nodeChildren(node)) {
            if (isEmptySpace(builder->nodeFlags(c))) continue;
            collectLeavesBelow_(c, out, cap);
            if (out.size() > cap) return;
        }
    }

    // Decide whether a STRUCTURALLY-successful speculative probe of a
    // `commitRequiresTypeName`-guarded branch may COMMIT. Generic — the
    // guarded rule, its type child, the identifier token, the binder
    // vocabulary, and the operator table are all config-sourced.
    //
    //   (1) type child is NOT a lone identifier (keyword base, struct
    //       tag, pointer star, const, …) → COMMIT: those forms cannot
    //       be expressions, so no value reading competes.
    //   (2) lone identifier the sketch knows as a TYPE → COMMIT.
    //   (3) lone identifier the sketch knows as a VALUE → ROLLBACK
    //       (the value reading is the meaning; shadowing-aware).
    //   (4) lone identifier the sketch has NO entry for → POLARITY-
    //       dependent (FC4 c1 M4, the D2a decision):
    //       * PreferType (the FC2 default) — COMMIT iff the token after
    //         the type position (the first token of the following
    //         operand subtree) could NOT continue a value reading —
    //         i.e. it is not an infix/postfix/ternary operator per the
    //         operator table. Ternary is included beyond the plan's
    //         "binary/postfix" wording because `(a) ? b : c` is a
    //         continuable value reading too; the test is "could the
    //         parenthesized value parse keep going", derived ENTIRELY
    //         from the operator table — never a hardcoded token list.
    //         When it IS an operator: ROLLBACK to the value reading and
    //         hand an AmbiguousTypeNameCandidate out via `outCandidate`
    //         for the compilation unit's cross-file oracle (one-shot
    //         seeded reparse). The CALLER records it after the probe's
    //         rollback so it survives (the probe restore would otherwise
    //         erase it with the rest of the sketch delta).
    //       * RequireKnownType (C 6.7.6.3p11 — `T (name)` in parameter
    //         position is a parenthesized declarator unless `name` is a
    //         visible typedef) — NEVER commit on Unknown: always roll
    //         back AND record the candidate, so a cross-file typedef in
    //         the guarded position still resolves on the oracle reparse.
    //       Rules 1-3 are polarity-INDEPENDENT: a keyword-led type child
    //       commits always (rule 1 — it cannot be an expression), a
    //       sketch-KNOWN Type commits, a sketch-KNOWN Value rolls back.
    //
    // Returning false lets the caller's SpeculationProbe restore all
    // five machines; the alt's next candidate branch then parses the
    // value reading.
    [[nodiscard]] bool typeNameCommitApproved_(
        RuleId branch, RuleId typeRule, TypeNameCommitPolarity polarity,
        std::optional<AmbiguousTypeNameCandidate>& outCandidate) {
        // The just-built branch subtree is the parent frame's newest
        // pending child (the branch frame closed inside the probe loop).
        const auto pending = builder->currentFramePendingChildren();
        if (pending.empty()) return false;   // cannot verify → rollback
        const NodeId branchNode = pending.back();
        if (builder->nodeKind(branchNode) != NodeKind::Internal
            || builder->nodeRule(branchNode).v != branch.v) {
            return false;   // not the branch subtree — refuse to guess
        }

        // Locate the type child + the first INTERNAL sibling after it
        // (the operand subtree; the token(s) between are the closer).
        NodeId typeChild{};
        NodeId operandSibling{};
        for (NodeId c : builder->nodeChildren(branchNode)) {
            if (isEmptySpace(builder->nodeFlags(c))) continue;
            if (!typeChild.valid()) {
                if (builder->nodeKind(c) == NodeKind::Internal
                    && builder->nodeRule(c).v == typeRule.v) {
                    typeChild = c;
                }
                continue;
            }
            if (builder->nodeKind(c) == NodeKind::Internal) {
                operandSibling = c;
                break;
            }
        }
        if (!typeChild.valid()) {
            // The guard names a rule that did not materialize in the
            // built subtree — a schema-authoring inconsistency. Refuse
            // the commit (the alt's other branches still get their
            // shot); never guess a type parse we cannot inspect.
            return false;
        }

        // Rule 1 — not a lone identifier → commit.
        std::vector<NodeId> typeLeaves;
        collectLeavesBelow_(typeChild, typeLeaves, /*cap=*/1);
        if (typeLeaves.size() != 1
            || builder->nodeKind(typeLeaves[0]) != NodeKind::Token
            || builder->nodeTokenKind(typeLeaves[0]).v != identifierKind.v) {
            return true;
        }

        // Rules 2 + 3 — the sketch knows the name.
        const SourceSpan nameSpan = builder->nodeSpan(typeLeaves[0]);
        const std::string_view name = src->slice(nameSpan);
        switch (sketch.lookup(name)) {
        case BinderSketch::NameKind::Type:    return true;
        case BinderSketch::NameKind::Value:   return false;
        case BinderSketch::NameKind::Unknown: break;
        }

        // Rule 4 — unknown name. Strict polarity first (FC4 c1 M4):
        // RequireKnownType never commits an unknown — roll back to the
        // competing reading and record the candidate so the CU oracle's
        // seeded reparse gives a cross-file typedef its second chance.
        if (polarity == TypeNameCommitPolarity::RequireKnownType) {
            outCandidate = AmbiguousTypeNameCandidate{
                .name = std::string{name},
                .span = nameSpan,
            };
            return false;
        }

        // PreferType — follower-operator test.
        SchemaTokenId followerKind{};
        if (operandSibling.valid()) {
            std::vector<NodeId> fl;
            collectLeavesBelow_(operandSibling, fl, /*cap=*/0);
            if (!fl.empty()
                && builder->nodeKind(fl[0]) == NodeKind::Token) {
                followerKind = builder->nodeTokenKind(fl[0]);
            }
        }
        auto const& ops = schema->operatorTable();
        const bool followerIsOperator = followerKind.valid()
            && (ops.lookup(followerKind, OperatorArity::Infix).has_value()
                || ops.lookup(followerKind, OperatorArity::Postfix).has_value()
                || ops.lookup(followerKind, OperatorArity::Ternary).has_value());
        if (!followerIsOperator) {
            return true;   // value reading could not continue → commit
        }
        outCandidate = AmbiguousTypeNameCandidate{
            .name = std::string{name},
            .span = nameSpan,
        };
        return false;
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

    // The effective kind of the `n`-th SIGNIFICANT (non-trivia) token at
    // or after the current stream position, looking through skippable
    // trivia exactly as the AltChoice dispatch does. `n == 0` is the
    // upcoming significant token (the alt already skipped leading trivia,
    // so it equals the dispatched `tokKind`). Returns the Eof kind once
    // the stream is exhausted (peek past end is idempotent-Eof). Used by
    // the speculative-alt predictive prune to compare input offsets >0
    // against a candidate's FIRST_k prefix.
    [[nodiscard]] SchemaTokenId peekSignificantKind(std::size_t n) const noexcept {
        std::size_t seen = 0;
        for (std::size_t raw = 0; ; ++raw) {
            Token const& t = tokens.peek(raw);
            if (t.coreKind == CoreTokenKind::Eof) {
                return effectiveKind(t, identifierKind, errorKind);
            }
            if (isSkippableTrivia(t)) continue;
            if (seen == n) return effectiveKind(t, identifierKind, errorKind);
            ++seen;
        }
    }

    // Enumerate candidate branch rules at the current speculative
    // AltChoice: the alt's RuleLeaf branches `r` (depth-first through
    // nested AltChoice positions) for which all of
    //   (1) `FIRST(r)` contains the upcoming token's effective kind, AND
    //   (2) the LL(k) PREDICTIVE PREFIX of `r` accepts the next k
    //       significant tokens (k = the alt's declared `lookahead`), AND
    //   (3) `routeToRuleLeaf(walker.cursor(), r)` is valid (the alt
    //       structurally allows descending into r).
    //
    // (2) is the bounded-lookahead PREDICTIVE PRUNE that replaces the
    // C cast-vs-paren ambiguity's unbounded backtracking. A candidate
    // whose FIRST_k prefix provably cannot match the upcoming tokens is
    // dropped BEFORE it speculates — so a deeply-nested `((((0))))` no
    // longer runs (and panic-recovers) a doomed `castExpr` probe at every
    // level: token[+1] == `(` ∉ FIRST(castTypeRef) prunes `castExpr` and
    // `compoundLiteralExpr`, leaving only `parenExpr` (O(N) total, not
    // O(N²)). The genuine `(Identifier)` ambiguity is NOT pruned —
    // token[+1] == Identifier ∈ FIRST(castTypeRef) keeps `castExpr`
    // speculating so the binder triage (`commitRequiresTypeName`) decides.
    //
    // The prefix is a SOUND over-approximation (see
    // `CompiledRule::predictivePrefix`): each recorded offset is the EXACT
    // admissible-token set at that input position, so the prune can never
    // drop a candidate that could legitimately match. It is fully
    // config-derived — the engine walks FIRST sets + the position table
    // and names no token, rule, or language.
    //
    // CONTRACT: surviving candidates probe in DECLARED grammar order
    // — author-controlled, never interner/name order. The list comes
    // from the AltChoice's compiled branch table (JSON-array order via
    // `altRuleBranches`); sourcing it from the rule interner instead
    // (the pre-FC4 behavior) made probe priority an accident of rule
    // NAMING — interner ids are assigned in alphabetical key order —
    // silently overriding the author's declared branch order whenever
    // two branches structurally accept the same input.
    // `applyPrune` selects the candidate set's purpose:
    //   true  → the SPECULATION set: the predictive prune is applied, so the
    //           probe loop / unique-production descent only sees branches
    //           whose FIRST_k can still match. This is the O(N) lever.
    //   false → the STRUCTURAL set: only the 1-token FIRST gate + routing,
    //           NO predictive prune. This is the set the all-fail fallback
    //           REPLAY draws its declared-last diagnostic target from — the
    //           replay deliberately runs a doomed branch for its precise
    //           errors, so the prune (which removes branches that can't match
    //           LATER tokens) must NOT strip the replay's target. Sourcing
    //           the replay target from the pruned set would, for input that
    //           every branch's FIRST_k rejects, leave the fallback with no
    //           target and surface an opaque P_BacktrackFailed instead
    //           (pinned by ParserSpeculation.BacktrackFailedAndRecoveryOn-
    //           BogusInput).
    [[nodiscard]] std::vector<RuleId>
    candidateBranches(SchemaTokenId tokKind, std::size_t lookahead,
                      bool applyPrune) const {
        std::vector<RuleId> out;
        for (RuleId const candidate
             : schema->altRuleBranches(walker.cursor())) {
            const auto firstSet = schema->firstSetOf(candidate);
            if (firstSet.empty()) continue;
            if (std::ranges::find(firstSet, tokKind) == firstSet.end()) continue;
            if (applyPrune && predictivePrefixPrunes(candidate, lookahead))
                continue;
            const auto routed =
                schema->routeToRuleLeaf(walker.cursor(), candidate);
            if (!routed.valid()) continue;
            out.push_back(candidate);
        }
        return out;
    }

    // True iff `candidate`'s LL(k) predictive prefix provably CANNOT match
    // the upcoming significant tokens, so the candidate may be pruned
    // before speculating. Bounded by `k` (the alt's `lookahead`, the
    // author-declared disambiguation distance) AND by the candidate's own
    // prefix length (which self-terminates at its first variable-width
    // element). Offset 0 is skipped — the FIRST gate in `candidateBranches`
    // already covers it; only offsets >=1 add discriminating power.
    //
    // Sound by construction: a candidate is pruned ONLY when an OBSERVED
    // token at some defined offset lies OUTSIDE that offset's exact
    // admissible set. An empty prefix (no multi-token discriminator) never
    // prunes — the function returns false immediately.
    //
    // PRECONDITION (contextual / scope-sensitive kinds) — anchor
    // D-PARSE-PREDICTIVE-PRUNE-CONTEXTUAL-KEYWORD. The observed kind comes
    // from `peekSignificantKind` → `effectiveKind`, which by design does NOT
    // model contextual-keyword demotion: a soft keyword (a `contextual: true`
    // entry, or ANY keyword under `reservedWordPolicy: "contextual"`) still
    // reports its KEYWORD kind here, even though the builder may later demote
    // it to Identifier against the live cursor expectedSet + scope stack. So
    // at an offset whose admissible set holds the demotion target (Identifier)
    // the prune WOULD wrongly drop a candidate that the demoted token matches.
    // ✅ CLOSED (cycle 13): the loop body SKIPS the prune at any offset whose
    // observed token `schema->isContextualKind(got)` — a soft keyword / any
    // keyword under `reservedWordPolicy: "contextual"` (the loader derives the
    // token-id-keyed `contextualKinds` set from the per-LexemeMeaning `contextual`
    // flags, the query that didn't exist before). So the prune never drops a
    // candidate a contextual demotion would match. EMPTY contextualKinds for a
    // non-contextual grammar (every shipped c-subset speculative alt) ⇒ the
    // deep-nest O(N) win is unaffected. Pinned by the synthetic contextual-keyword
    // speculative-alt schemas in test_parser_speculation.cpp (RED-on-disable).
    [[nodiscard]] bool
    predictivePrefixPrunes(RuleId candidate, std::size_t k) const noexcept {
        const std::size_t prefixLen = schema->predictivePrefixLen(candidate);
        if (prefixLen < 2 || k < 2) return false;  // nothing beyond FIRST
        const std::size_t bound = std::min(prefixLen, k);
        for (std::size_t i = 1; i < bound; ++i) {
            const auto admissible = schema->predictivePrefixAt(candidate, i);
            if (admissible.empty()) continue;  // no constraint at this offset
            const SchemaTokenId got = peekSignificantKind(i);
            // D-PARSE-PREDICTIVE-PRUNE-CONTEXTUAL-KEYWORD: a CONTEXTUAL observed
            // kind (a soft keyword) may DEMOTE to Identifier in the builder, so
            // its `effectiveKind` here understates what the candidate can match.
            // Skip the prune at this offset rather than wrongly drop a candidate
            // the demoted token would in fact match (a silent mis-parse). O(1);
            // empty contextualKinds for non-contextual grammars ⇒ deep-nest O(N)
            // win preserved.
            if (schema->isContextualKind(got)) continue;
            if (std::ranges::find(admissible, got) == admissible.end()) {
                return true;  // observed token outside the exact set ⇒ prune
            }
        }
        return false;
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
                      : 64u))
            , stepRecoveredBefore_(impl.stepRecovered_)
            // FC4 c1: the forward-progress watchdog tuple is probe state
            // too. Without restoring it, a rolled-back probe leaves
            // `last*` pointing at a position INSIDE the discarded branch
            // — and a later non-speculative dispatch (the all-fail
            // fallback REPLAY) that legitimately re-reaches that same
            // (cursor, tokPos, depth) trips the watchdog on real
            // progress. Same restore discipline as diagsEmitted /
            // stepRecovered_.
            , lastCursorBefore_(impl.lastCursor)
            , lastTokPosBefore_(impl.lastTokPos)
            , lastDepthBefore_(impl.lastDepth)
            , firstIterationBefore_(impl.firstIteration)
            // FC2: the binder sketch is the FIFTH machine a probe
            // touches (binder-rule frames closing inside the branch
            // record bindings; the type-name triage records
            // candidates). A rolled-back probe must leak neither —
            // same discipline as diagsEmitted / stepRecovered_.
            , sketchSnap_(impl.sketch.snapshot()) {
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
                impl_.stepRecovered_ = stepRecoveredBefore_;
                impl_.lastCursor     = lastCursorBefore_;
                impl_.lastTokPos     = lastTokPosBefore_;
                impl_.lastDepth      = lastDepthBefore_;
                impl_.firstIteration = firstIterationBefore_;
                impl_.sketch.restore(std::move(sketchSnap_));
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
            // FC4 c1: the post-commit tuple just recorded is EXACTLY what
            // the next dispatch iteration snaps when the committed branch
            // leaves the cursor at a non-End slot with no token consumed
            // in between (e.g. a committed declarator base suffix landing
            // on the suffix-repeat AltChoice) — the watchdog would trip on
            // legitimate progress. Skip exactly ONE comparison; a true
            // stall still trips on the following iteration, so the
            // watchdog stays sound.
            impl_.firstIteration = true;
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
        bool                                   stepRecoveredBefore_;
        SchemaCursor                           lastCursorBefore_;
        std::size_t                            lastTokPosBefore_;
        std::size_t                            lastDepthBefore_;
        bool                                   firstIterationBefore_;
        BinderSketch::Snapshot                 sketchSnap_;
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
        // FC2 binder sketch: a `semantics.scopes` rule's frame is a
        // lexical scope — the SAME rule-driven scope model the semantic
        // analyzer builds, mirrored parser-side. (Probe rollback pops
        // frames without closeFrameOnce; the sketch snapshot restore
        // covers those opens wholesale.)
        if (sketch.enabled() && sketch.isScopeRule(rule)) {
            sketch.openScope(rule);
        }
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

        // FC2: an ambiguous type-name candidate produced by the commit
        // triage must SURVIVE the rollback of its own probe (the
        // rolled-back value reading is the chosen parse; the candidate
        // is the oracle's input for the cross-file second chance). The
        // probe's RAII restore covers the sketch — so the triage hands
        // the candidate OUT and it is recorded after the probe
        // destructs, into the restored sketch. An ENCLOSING probe that
        // later rolls back erases it correctly (the winning parse
        // re-encounters and re-records the site) — convergent to
        // exactly the surviving parse's candidates. (The Pratt walker
        // itself records each site exactly once: its wrap-in-place
        // climb never rolls back.)
        std::optional<AmbiguousTypeNameCandidate> rolledBackCandidate;

        const bool committed = [&]() -> bool {
        SpeculationProbe probe{*this};

        // FC4 c1: an `expr`-shape BRANCH (e.g. c-subset forInitAmbig's
        // `expression`) must hand off to the Pratt walker exactly like the
        // RuleLeaf dispatch does — `openExprFrame` would compile the branch
        // as its transparent atom reference, parse ONLY the first operand,
        // and silently COMMIT a truncated expression (`i = 9` consuming
        // just `i`). The walker balances its own frames; failure surfaces
        // via the probe's diagnostic/desync deltas and rolls back so the
        // alt's next candidate gets its shot.
        if (schema->isExprRule(branch)) {
            prattWalker->walkExpression(*outer, branch,
                                        schema->exprMinPrecedence(branch));
            if (probe.emittedDiag())   return false;
            if (probe.isDesynced())    return false;
            probe.commit();
            return true;
        }

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

        // FC2 type-name commit guard: a `commitRequiresTypeName`-marked
        // branch commits only per the generic triage (lone-identifier
        // type names need binder-sketch / follower evidence; every other
        // type form commits). The guard's declared POLARITY (FC4 c1 M4)
        // selects the unknown-name arm. Returning false lets the probe's
        // RAII restore roll the branch back and the caller try the alt's
        // next candidate — the value reading.
        if (const RuleId typeRule = schema->typeNameCommitRule(branch);
            typeRule.valid()) {
            if (!typeNameCommitApproved_(branch, typeRule,
                                         schema->typeNameCommitPolarity(branch),
                                         rolledBackCandidate)) {
                return false;
            }
        }

        probe.commit();
        return true;
        }();   // probe destructs here — rollback restored the sketch

        if (!committed && rolledBackCandidate.has_value()) {
            sketch.recordCandidate(std::move(*rolledBackCandidate));
        }
        return committed;
    }

    // ── one dispatch iteration ─────────────────────────────────────

    [[nodiscard]] StepOutcome stepOnce() {
        // Snapshot whether the PREVIOUS step recovered, then clear it: any
        // step that recovers (panic scan / missing-rule synthesis) re-sets it
        // before returning. `synthesizeMissingRule` reads this snapshot to
        // suppress a redundant diagnostic when it fires while still resyncing
        // the same broken region.
        const bool prevStepRecovered = stepRecovered_;
        stepRecovered_ = false;

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
            std::string const stallDetail = std::format(
                "parser forward-progress watchdog tripped at slot kind {}, "
                "core kind {}, schema kind {}, frame depth {}, token pos {}, "
                "rule '{}'",
                static_cast<int>(walker.slotKind()),
                static_cast<int>(tokens.peek().coreKind),
                tokens.peek().schemaKind.v,
                frames.size(),
                tokens.position(),
                frameRules.empty()
                    ? std::string{"<none>"}
                    : std::string{schema->rules().name(frameRules.back())});
            emitParserError(DiagnosticCode::P_RecoveryStalled,
                            tokens.peek().span, stallDetail);
            // The fatal abort happens before the diagnostic drain can
            // print, so the detail rides the abort message too.
            fatal(stallDetail.c_str());
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
            bool matches = !expected.empty()
                && std::ranges::find(expected, tokKind) != expected.end();

            // D-PARSE-PREDICTIVE-PRUNE-CONTEXTUAL-KEYWORD (probe-demotion
            // half): a CONTEXTUAL keyword whose own kind is not admissible
            // here DEMOTES to the identifier kind when THAT is admissible —
            // exactly the builder's `pushToken` demotion (tree_builder.cpp
            // "contextual keyword resolution"). The parser's match gate
            // tests the UN-demoted `tokKind` (effectiveKind by design does
            // not model the demotion), so without this mirror the gate
            // rejects the token (`recoverAt`) BEFORE the builder ever
            // demotes — and a speculative candidate the demotion would
            // satisfy is lost (its probe emits P_UnexpectedToken → fails).
            // Mirror it: consume the token AND advance the parser's walker
            // with the SAME demoted kind the builder's walker takes, so the
            // two cursors stay in lockstep (no spurious desync). Gated on
            // `identifierKind` actually being admissible — if it is not, the
            // demotion would not match either, so `recoverAt` is correct.
            // O(1); for a non-contextual grammar `isContextualKind` is always
            // false ⇒ this arm never fires ⇒ zero behavior change.
            SchemaTokenId advanceKind = tokKind;
            if (!matches && schema->isContextualKind(tokKind)
                && std::ranges::find(expected, identifierKind)
                       != expected.end()) {
                matches     = true;
                advanceKind = identifierKind;
            }

            if (matches) {
                const Token tok = tokens.advance();
                builder->pushToken(tok);
                walker.advance(advanceKind, tok.span,
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

            // Required rule `nextRule` can't start here. If `peek` is a
            // stop-point it belongs to an enclosing context (the statement
            // terminator / block closer) — synthesize the missing rule and
            // let the parent consume `peek`, instead of panic-consuming a
            // token the enclosing grammar needs (the `int x = ;` scope-
            // imbalance fix). Non-stop-point garbage keeps the panic path.
            if (speculationDepth == 0 && isStopPoint(tokKind)) {
                synthesizeMissingRule(nextRule, peek, firstSet,
                                      /*emitDiagnostic=*/!prevStepRecovered);
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
                // First success commits and falls through to the next outer
                // iteration. The SPECULATION set applies the LL(k) predictive
                // prune (k = the alt's declared lookahead) so doomed probes
                // never run.
                const auto candidates =
                    candidateBranches(tokKind, walker.lookahead(),
                                      /*applyPrune=*/true);

                // LL(k) UNIQUE-PRODUCTION DIRECT DESCENT. When the
                // predictive prune leaves EXACTLY ONE viable candidate and
                // that candidate carries no commit-time triage
                // (`commitRequiresTypeName`), there is nothing left to
                // disambiguate — the lookahead has already SELECTED the
                // production. Descend into it directly, exactly as the
                // non-speculative RuleLeaf path would, WITHOUT a
                // speculation probe.
                //
                // This is the load-bearing half of the deep-nest fix: it
                // is what makes a pruned `(((…)))` actually O(N). The prune
                // removes `castExpr`/`compoundLiteralExpr`, but the lone
                // survivor `parenExpr` still RECURSES (its body is
                // `( expression )`), and probing it at every one of N
                // levels — each probe driving the inner parse to its frame
                // close, nested N deep — is the super-linear blowup. A
                // direct descent recurses ONCE per level with no probe,
                // no per-level checkpoint, and no re-drive: O(N) total.
                //
                // Sound because the prune is a sound over-approximation:
                // every OTHER branch was proven unable to match the
                // upcoming tokens, so the survivor is the only possible
                // parse — committing to it discards nothing. A surviving
                // candidate WITH a commit guard is excluded (it keeps
                // speculating so the binder triage runs: the genuine
                // `(Identifier)` cast-vs-paren case where both `castExpr`
                // and `parenExpr` survive is unaffected, and a lone
                // guarded survivor still gets its triage). Fully generic —
                // no token, rule, or language is named.
                if (candidates.size() == 1
                    && !schema->typeNameCommitRule(candidates.front())
                            .valid()) {
                    const RuleId only = candidates.front();
                    if (schema->isExprRule(only)) {
                        prattWalker->walkExpression(
                            *outer, only, schema->exprMinPrecedence(only));
                    } else {
                        openExprFrame(only);
                    }
                    return StepOutcome::Continue;
                }

                for (auto const branch : candidates) {
                    if (trySpeculativeBranch(branch)) {
                        return StepOutcome::Continue;
                    }
                }

                // FC4 c1: every candidate failed. At the OUTERMOST level,
                // REPLAY the declared-LAST candidate non-speculatively so
                // its precise diagnostics land where the user can act on
                // them — the declared order makes the last branch the
                // language's fallback reading (c-subset declOrExprStmt's
                // exprStmt), and pre-FC4 that reading was a direct alt
                // branch whose errors surfaced directly. Pure rollback
                // would bury the real error (`a ? b ;` → "missing ':'")
                // under an opaque P_BacktrackFailed.
                //
                // ONE-SHOT per (cursor, token position): a replayed branch
                // that fails WITHOUT net token consumption unwinds back to
                // this very alt with the same peek — a second replay would
                // loop until the forward-progress watchdog aborts. The
                // re-entry falls through to the P_BacktrackFailed +
                // consume hatch, which guarantees progress exactly as the
                // pre-replay contract did. Inside a NESTED probe the
                // rollback semantics stay pure (the outer probe owns the
                // failure), so no replay fires there either.
                //
                // The replay target is the declared-last of the STRUCTURAL
                // (1-token-FIRST-gated, UN-pruned) candidate set, not the
                // pruned speculation set: the predictive prune can legitimately
                // empty the speculation set (every branch's FIRST_k rejects the
                // later tokens, e.g. `A X ;` where neither `A B ;` nor `A C ;`
                // matches at offset 1), but the fallback must still replay a
                // branch to surface its precise diagnostic instead of an opaque
                // P_BacktrackFailed.
                const auto structuralCandidates = candidateBranches(
                    tokKind, walker.lookahead(), /*applyPrune=*/false);
                if (speculationDepth == 0 && !structuralCandidates.empty()
                    && !(replayedFallback_
                         && lastReplayCursor_ == walker.cursor()
                         && lastReplayTokPos_ == tokens.position())) {
                    replayedFallback_  = true;
                    lastReplayCursor_  = walker.cursor();
                    lastReplayTokPos_  = tokens.position();
                    const RuleId fallback = structuralCandidates.back();
                    if (schema->isExprRule(fallback)) {
                        prattWalker->walkExpression(
                            *outer, fallback,
                            schema->exprMinPrecedence(fallback));
                        return StepOutcome::Continue;
                    }
                    openExprFrame(fallback);
                    return StepOutcome::Continue;
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

            // AltChoice → RuleLeaf branch route, candidates sourced
            // from the alt's compiled branch table in DECLARED grammar
            // order (same contract as `candidateBranches`: speculative
            // candidates probe in DECLARED grammar order — author-
            // controlled, never interner/name order). Non-speculative
            // alts are loader-checked for FIRST-set overlap
            // (C_AmbiguousAlternatives), so at most one branch admits
            // `tokKind` here; declared order keeps the tie-break
            // uniform with the speculative path regardless.
            for (RuleId const candidate
                 : schema->altRuleBranches(walker.cursor())) {
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

    // FC2: seed cross-file type names into the binder sketch's global
    // scope BEFORE any frame opens — the compilation-unit oracle's
    // channel for the one-shot ambiguous-candidate reparse.
    for (auto& seed : I.config.seedGlobalTypeNames) {
        I.sketch.seedGlobalType(std::move(seed));
    }
    I.config.seedGlobalTypeNames.clear();

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

    // FC2 sidecars: the ambiguous type-name sites this parse rolled
    // back on (the CU oracle's input) + the global-scope TYPE names it
    // bound (the oracle's harvest surface). Both empty for binder-less
    // languages.
    return ParseResult{
        .tree               = std::move(*I.builder).finish(),
        .typeNameCandidates = I.sketch.takeCandidates(),
        .globalTypeNames    = I.sketch.globalTypeNames(),
    };
}

// ── DefaultPrattWalker ──────────────────────────────────────────────────
//
// Schema-driven operator-precedence climber. Produces a STRUCTURALLY
// associative tree wrapping operator results in the rules declared by
// the schema's `expr.wrapperRules.{binary,unary,postfix,ternary}`
// block — names are config-sourced (loader auto-interns them when any
// `expr` shape is declared). The operator table's declared
// `associativity` is consumed HERE, by the walker, when it picks the
// RHS minimum precedence for an infix wrap: Left (and None, the
// loader's omitted-field default) chains build ITERATIVELY — O(n)
// wraps, no per-operator recursion, stack-friendly — nesting LEFT;
// Right-assoc chains recurse via the RHS parse at the operator's own
// precedence, nesting RIGHT.
//
// Each `parseExpressionAt(minPrec)` call:
//   1. Parses a primary (prefix-op + nested expression OR the atom).
//   2. Loops: while peek is an op at >= minPrec, wraps the already-
//      built last child in the matching wrapper frame
//      (`TreeBuilder::wrapLastChildInFrame`) and parses the operator's
//      remaining operands inside the still-open wrapper:
//        - infix: RHS at `prec + 1` for Left/None (the next same-prec
//          op returns to THIS loop and wraps THIS wrapper → left
//          nesting), at `prec` for Right (the RHS recursion consumes
//          the chain → right nesting).
//        - postfix: no further operand (or a grouped/follower body).
//        - ternary: middle at 0, else at `prec` (right-assoc chain).
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
    // Depth guard: recursion occurs only through OPERANDS (nested
    // parens, prefix operands, grouped-postfix bodies, ternary
    // clauses) and RIGHT-associative RHS chains — Left/None-assoc
    // chains build iteratively in the climb loop below and never
    // deepen the C++ stack. This is the SINGLE chokepoint every
    // deepening path funnels through: prefix operands, the paren/atom
    // re-entry (parsePrimary -> parseUntilFrameDepth -> stepOnce ->
    // walkExpression -> here), ternary clauses, and infix RHS all
    // recurse back into parseExpressionAt, so the cap covers them ALL
    // by construction. On trip we FAIL LOUD with a positioned
    // diagnostic + recover (Error leaf + panic-scan + graceful unwind)
    // rather than recursing one level deeper into a C++ stack overflow.
    //
    // PERF (D-PARSE-DEEP-NEST-RECURSION-MEMORY): this per-OPERAND host
    // recursion is the source of the residual ~exp-1.7 WALL-CLOCK super-
    // linearity on deeply-nested input (e.g. `((((0))))`). The parse WORK is
    // O(N) — a flat `0+0+…+0` chain of the same node count parses with a FLAT
    // per-element cost (pinned by `FlatChainParseWorkIsLinear`) — so the
    // residual is a memory-hierarchy constant (live call-stack working set +
    // strided unwind re-access of the depth-indexed frame/cursor vectors),
    // NOT an algorithmic O(N²) term. It is bounded/moot: `maxExpressionDepth`
    // caps depth (default 256) and the whole downstream frontend is itself
    // recursion-bound to that cap (D-PARSE-DEEP-FRONTEND-STACK), so flattening
    // it would need an explicit-stack iterative rewrite of the WHOLE frontend
    // for zero in-cap benefit. Deferred (trigger-gated) — see the registry.
    if (I.expressionDepth >= I.config.maxExpressionDepth) {
        I.recoverExpressionTooDeep_(I.tokens.peek());
        return;
    }
    ++I.expressionDepth;
    struct DepthGuard {
        std::size_t& depth;
        ~DepthGuard() { --depth; }
    } guard{I.expressionDepth};

    parsePrimary(I, rules, minPrec);

    // Operator climb — wrap-in-place for ALL three arms (postfix /
    // ternary / infix): the already-built previous child is adopted by
    // the new wrapper via `wrapLastChildExprFrame`, and the remaining
    // operands parse inside the still-open wrapper. There is no
    // snapshot / rollback-replay here (the former WalkerSnapshot
    // mechanism is deleted). Behavior preservation vs that design:
    //   (a) diagnostics: the replay rolled diagnostics back with the
    //       builder (TreeBuilder::CheckpointSnapshot carries a
    //       DiagnosticReporter::Snapshot) and re-emitted them on the
    //       replay — emitting ONCE under wrap-in-place produces the
    //       identical diagnostic stream.
    //   (b) binder sketch: snapshot restore was truncate-to-count and
    //       the replay re-recorded the same bindings/candidates ⇒
    //       convergent — recording once reaches the same final state.
    std::vector<Token> heldTrivia;
    auto const placeHeldTrivia = [&I, &heldTrivia] {
        for (Token const& t : heldTrivia) I.builder->pushToken(t);
    };
    while (true) {
        // Hold-then-place trivia: collect the trivia run (same
        // token-level classification `pumpTrivia` uses) WITHOUT
        // pushing it, so the wrap decision below sees the real
        // expression subtree — never a whitespace/comment leaf — as
        // the open frame's last pending child (`f (x)` must wrap `f`,
        // not the space). If an arm fires, the held run is pushed into
        // the just-opened wrapper, before the operator token; if the
        // loop exits, it's pushed into the current frame. Both
        // reproduce the token-ordered leaf stream byte-for-byte.
        heldTrivia.clear();
        while (!I.tokens.isAtEnd() && isSkippableTrivia(I.tokens.peek())) {
            heldTrivia.push_back(I.tokens.advance());
        }

        Token const& peek = I.tokens.peek();
        if (peek.coreKind == CoreTokenKind::Eof) {
            placeHeldTrivia();
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
            placeHeldTrivia();
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
        // child.
        if (postfixInClimb) {
            I.wrapLastChildExprFrame(rules.postfix);
            placeHeldTrivia();
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
            continue;
        }

        // Ternary (mixfix `cond ? then : else`). Wraps the already-built
        // chain in place as its condition, then parses the middle clause
        // (to the `:` separator) and the else operand inside the wrapper.
        // The middle parses at minPrec 0 — between `?` and `:` anything binds
        // (assignment, even a nested ternary); the climb naturally stops at `:`
        // (which carries no operator-table entry). The else parses at the
        // ternary's own precedence → right-associative (`a?b:c?d:e` = a?b:(c?d:e)).
        if (ternaryInClimb) {
            I.wrapLastChildExprFrame(rules.ternary);
            placeHeldTrivia();
            if (!I.pushOperatorToken()) {                            // `?`
                // Defensive — same contract as the postfix arm's
                // pushOperatorToken failure path.
                I.emitParserError(DiagnosticCode::P_PrematureEndOfInput, peek.span,
                                  "expression ended before ternary '?'");
                I.closeFrameOnce();
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
                return;
            }
            (void)I.pushOperatorToken();                             // `:`
            parseExpressionAt(I, rules, ternary->precedence);        // else
            I.closeFrameOnce();
            continue;
        }

        // Infix: wrap the already-built chain in place as the LHS,
        // then parse the RHS inside the still-open wrapper. The RHS
        // minimum precedence encodes the operator's DECLARED
        // associativity:
        //   - Left (and None, the loader's omitted-field default):
        //     prec + 1 — the next same-prec operator does NOT bind
        //     inside the RHS; it returns to this loop and wraps THIS
        //     wrapper → chains nest LEFT, iteratively.
        //   - Right: prec — the next same-prec operator binds inside
        //     the RHS recursion → chains nest RIGHT.
        I.wrapLastChildExprFrame(rules.binary);
        placeHeldTrivia();
        if (!I.pushOperatorToken()) {
            // Defensive — same contract as the postfix arm's
            // pushOperatorToken failure path.
            I.emitParserError(
                DiagnosticCode::P_PrematureEndOfInput,
                peek.span,
                "expression ended before infix operator");
            I.closeFrameOnce();
            return;
        }
        const std::int32_t rhsMin =
            (infix->associativity == OperatorAssoc::Right)
                ? infix->precedence
                : infix->precedence + 1;
        parseExpressionAt(I, rules, rhsMin);
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
