#include "core/types/tree_builder.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace dss {

// ─────────────────────────────────────────────────────────────────────────
// OpenScope — RAII guard returned by TreeBuilder::open()
// ─────────────────────────────────────────────────────────────────────────

TreeBuilder::OpenScope::OpenScope(TreeBuilder* b, std::uint32_t cookie) noexcept
    : builder_(b), cookie_(cookie) {}

TreeBuilder::OpenScope::OpenScope(OpenScope&& other) noexcept
    : builder_(other.builder_), cookie_(other.cookie_) {
    other.builder_ = nullptr;
    other.cookie_  = 0;
}

TreeBuilder::OpenScope& TreeBuilder::OpenScope::operator=(OpenScope&& other) noexcept {
    if (this != &other) {
        close();                                 // close our current frame first
        builder_       = other.builder_;
        cookie_        = other.cookie_;
        other.builder_ = nullptr;
        other.cookie_  = 0;
    }
    return *this;
}

TreeBuilder::OpenScope::~OpenScope() noexcept {
    close();
}

void TreeBuilder::OpenScope::close() noexcept {
    if (!builder_) return;                       // moved-from or already closed
    TreeBuilder* b = builder_;
    std::uint32_t c = cookie_;
    // Clear self before calling closeFrame_ so any re-entrant close
    // (e.g. via a diagnostic sink) becomes a no-op.
    builder_ = nullptr;
    cookie_  = 0;
    b->closeFrame_(c, /*synthetic*/ false);
}

// ─────────────────────────────────────────────────────────────────────────
// TreeBuilder
// ─────────────────────────────────────────────────────────────────────────

TreeBuilder::TreeBuilder(std::shared_ptr<SourceBuffer>        src,
                        std::shared_ptr<GrammarSchema const> schema,
                        DiagnosticReporter::Config           diagConfig)
    : source_(std::move(src))
    , schema_(std::move(schema))
    , reporter_(std::make_unique<DiagnosticReporter>(std::move(diagConfig))) {
    // Constructor preconditions: source_ and schema_ must be non-null.
    // We accept a null source in tests of degenerate paths (the diagnostic
    // emit fallback handles it), but a null schema means pushToken has
    // nothing to resolve against — fail fast.
    if (!schema_) {
        std::fputs("dss::TreeBuilder fatal: schema is null\n", stderr);
        std::abort();
    }
    // Slot 0 of the arena is reserved as InvalidNode — every real NodeId
    // starts at 1. This matches the convention DSS_STRONG_ID enforces:
    // default-constructed = invalid sentinel.
    nodes_.emplace_back();
    // Initial cursor is invalid — only becomes meaningful on the first
    // open(rule). Until then there's no expected set for pushToken to
    // consult, and contextual-keyword resolution stays strict.
    cursor_ = SchemaCursor{};
}

NodeId TreeBuilder::emit_(detail::Node n) {
    const auto value = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(n);
    return NodeId{value};
}

bool TreeBuilder::attachToCurrentFrame_(NodeId id) {
    if (open_.empty()) return false;
    open_.back().children.push_back(id);
    // Backpatch parent on the just-emitted node so HasError propagation
    // can walk via the stored parent link rather than the open-frame stack
    // (frames may have already closed by the time a deep error fires).
    nodes_[id.v].parent = open_.back().id;
    return true;
}

void TreeBuilder::emitDiagnostic_(ParseDiagnostic d) {
    // Snapshot the scope stack at error-time. Done here (rather than at
    // call sites) so every diagnostic gets it for free.
    d.scopeStack.assign(scopes_.begin(), scopes_.end());
    reporter_->report(std::move(d));
}

void TreeBuilder::noteCursorDesync_(bool wasValid,
                                    bool nowValid,
                                    SourceSpan span,
                                    std::optional<RuleId> rule) {
    if (cursorDesynced_) return;
    if (!(wasValid && !nowValid)) return;
    cursorDesynced_ = true;

    ParseDiagnostic d;
    d.code     = DiagnosticCode::P_SchemaCursorDesync;
    d.severity = DiagnosticSeverity::Info;
    d.buffer   = source_ ? source_->id() : InvalidBuffer;
    d.span     = span;
    d.ruleContext = rule;
    d.actual   = "schema cursor went off-track; contextual keyword "
                 "resolution will stay strict for the remainder of the build";
    emitDiagnostic_(std::move(d));
}

void TreeBuilder::addBuilderInvariant_(std::string actual, SourceSpan span) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::P_BuilderInvariant;
    d.severity = DiagnosticSeverity::Error;
    d.buffer   = source_ ? source_->id() : InvalidBuffer;
    d.span     = span;
    d.actual   = std::move(actual);
    emitDiagnostic_(std::move(d));
}

void TreeBuilder::propagateHasError_(NodeId start) noexcept {
    // The starting node already has HasError set by the caller (it IS
    // the error). Walk from its PARENT upward. Stops at the first
    // already-flagged ancestor — the chain is monotone since every
    // prior propagation flagged every ancestor up to root.
    if (!start.valid() || start.v >= nodes_.size()) return;
    NodeId cur = nodes_[start.v].parent;
    while (cur.valid() && cur.v < nodes_.size()) {
        detail::Node& n = nodes_[cur.v];
        if (hasError(n.flags)) break;
        n.flags |= NodeFlags::HasError;
        cur = n.parent;
    }
}

// ── open / close ─────────────────────────────────────────────────────────

TreeBuilder::OpenScope TreeBuilder::open(RuleId rule) & {
    if (finished_) {
        // Release-mode protection — an assert-only guard would silently
        // corrupt state in shipping builds.
        addBuilderInvariant_("open() after finish()", SourceSpan::empty(0));
        return OpenScope{nullptr, 0};
    }

    detail::Node n{};
    n.kind = NodeKind::Internal;
    n.rule = rule;
    // Initial span is empty at the source position we estimate. We use the
    // end of the last-attached child of the current frame (if any) as the
    // anchor, falling back to source-buffer start. Real span rolls up via
    // SourceSpan::join when children land.
    SourceSpan opener = SourceSpan::empty(0);
    if (!open_.empty() && !open_.back().children.empty()) {
        const NodeId lastChild = open_.back().children.back();
        opener = SourceSpan::empty(nodes_[lastChild.v].span.end());
    }
    n.span = opener;

    const NodeId id = emit_(n);
    if (!open_.empty()) {
        nodes_[id.v].parent = open_.back().id;
        // Register the new internal node as a child of the parent frame
        // now, while parent is still open. closeFrame_ computes the
        // parent's firstChild/childCount by walking frame.children;
        // without this push the subtree would vanish from the tree.
        open_.back().children.push_back(id);
    }

    // Wraparound is theoretical (4B opens in one build), but it would
    // collide with the "invalid" sentinel and cause spurious LIFO
    // violation diagnostics — assert early if it ever happens.
    assert(nextCookie_ != 0 && "TreeBuilder::open: cookie wraparound");
    const std::uint32_t cookie = nextCookie_++;
    open_.push_back(Frame{
        .id         = id,
        .rule       = rule,
        .openerSpan = opener,
        .children   = {},
        .cookie     = cookie,
    });

    // Walk the schema cursor in parallel with the open frame. Saving the
    // parent's cursor lets closeFrame_ resume the parent via leaveRule
    // when this rule completes. `leaveRule` requires the saved cursor be
    // at a RuleLeaf slot — if the parent cursor is at an AltChoice (the
    // body of a `repeat`/`optional`/`alt`), route it to the RuleLeaf
    // branch for `rule` first so close() can resume cleanly.
    SchemaCursor savedParent = cursor_;
    if (savedParent.valid()) {
        auto routed = schema_->routeToRuleLeaf(savedParent, rule);
        if (routed.valid()) savedParent = routed;
    }
    cursorStack_.push_back(savedParent);
    cursor_ = schema_->enterRule(rule);

    return OpenScope{this, cookie};
}

void TreeBuilder::closeFrame_(std::uint32_t cookie, bool /*synthetic*/) noexcept {
    // Fast path: this cookie has already been finalized by an earlier
    // cascade-close or by finish(). The OpenScope guard is calling
    // close() after its frame is gone — quietly accept and reclaim the
    // tracking slot.
    if (auto it = closedCookies_.find(cookie); it != closedCookies_.end()) {
        closedCookies_.erase(it);
        return;
    }
    if (open_.empty()) {
        // Reached only via a stale OpenScope whose cookie was never
        // recorded (shouldn't happen — but better than UB).
        return;
    }

    // Find the frame matching `cookie`. Normally it's the top of the
    // open-stack; if not, an OpenScope was closed out-of-order (LIFO
    // violation) — emit P_BuilderInvariant and cascade-close every frame
    // above it.
    auto it = open_.rbegin();
    for (; it != open_.rend(); ++it) {
        if (it->cookie == cookie) break;
    }
    if (it == open_.rend()) {
        addBuilderInvariant_(
            std::format("close() with unknown cookie {}", cookie),
            open_.back().children.empty()
                ? open_.back().openerSpan
                : nodes_[open_.back().children.back().v].span);
        return;
    }

    if (it != open_.rbegin()) {
        addBuilderInvariant_(
            "OpenScope closed out of LIFO order; cascading close",
            it->openerSpan);
    }

    // Cascade-close every frame above (inclusive of) `it`. Each cascade-
    // closed frame is finalized normally — children flushed, span rolled
    // up, HasError percolated from any erroring child.
    while (true) {
        Frame& fr = open_.back();
        detail::Node& node = nodes_[fr.id.v];

        const auto firstChild = static_cast<std::uint32_t>(childIndex_.size());
        for (NodeId child : fr.children) {
            childIndex_.push_back(child);
            // Roll the span up from this child onto our node.
            node.span = SourceSpan::join(node.span, nodes_[child.v].span);
            // OR-reduce HasError. The attach paths already propagated
            // eagerly, but synthetic Missing inserted directly into
            // fr.children by finish() bypasses attachToCurrentFrame_;
            // this loop catches it.
            if (hasError(nodes_[child.v].flags)) {
                node.flags |= NodeFlags::HasError;
            }
        }
        node.firstChild = firstChild;
        node.childCount = static_cast<std::uint32_t>(fr.children.size());

        const bool isTarget = (fr.cookie == cookie);
        if (!isTarget) {
            // This frame was cascade-closed — its OpenScope is still
            // alive and will eventually call close() once destroyed.
            // Record the cookie so that future call is a clean no-op.
            closedCookies_.insert(fr.cookie);
        }
        open_.pop_back();
        // Invariant: cursorStack_.size() == open_.size() at every
        // open/close boundary. leaveRule on a non-RuleLeaf saved cursor
        // returns invalid and trips noteCursorDesync_ — strict-only
        // contextual resolution for the remainder of the build.
        if (!cursorStack_.empty()) {
            SchemaCursor savedParent = cursorStack_.back();
            cursorStack_.pop_back();
            const bool wasValid = savedParent.valid();
            cursor_ = schema_->leaveRule(savedParent);
            noteCursorDesync_(wasValid, cursor_.valid(), fr.openerSpan,
                              std::optional<RuleId>{fr.rule});
        } else {
            cursor_ = SchemaCursor{};
        }
        if (isTarget) break;
        if (open_.empty()) break;
    }
}

// ── pushToken ────────────────────────────────────────────────────────────

namespace {

// Resolve `lexeme` against the schema, filtered by the current scope
// stack. Returns the winning meaning + an "ambiguous" flag indicating
// whether multiple equal-priority survivors existed (so the caller can
// emit P_AmbiguousToken).
struct ResolvedMeaning {
    LexemeMeaning meaning{};
    bool          ambiguous = false;
    std::size_t   matchCount = 0;
};

// Per-meaning scope filter — the LexemeMeaning::scopeRequire field on
// a token entry. Check order is `forbid → topMustBe → outermost → anyOf`;
// first failure rejects the meaning.
//
//   forbid    — none of these scopes may be on the stack.
//   topMustBe — innermost active scope must equal this kind.
//   outermost — bottom-of-stack scope must equal this kind.
//   anyOf     — at least one of these must be on the stack (empty = no constraint).
//
// Distinct from `schema.isTokenValidInScope` which enforces the schema's
// top-level `scopes.validity[].forbid` rules (per-scope, not per-token).
// Both checks are AND'd in `resolveMeaning`.
[[nodiscard]] bool meaningAllowedByScopeRequire(
    LexemeMeaning const& m,
    std::span<ScopeKind const> scopes) noexcept {
    auto const& sr = m.scopeRequire;
    // forbid
    for (ScopeKind f : sr.forbid) {
        for (ScopeKind active : scopes) {
            if (f == active) return false;
        }
    }
    // topMustBe — innermost is the last element (stack grows back).
    if (sr.topMustBe.has_value()) {
        if (scopes.empty() || scopes.back() != *sr.topMustBe) return false;
    }
    // outermost — bottom-of-stack is the first element.
    if (sr.outermost.has_value()) {
        if (scopes.empty() || scopes.front() != *sr.outermost) return false;
    }
    // anyOf — at least one of the listed must be active.
    if (!sr.anyOf.empty()) {
        bool any = false;
        for (ScopeKind required : sr.anyOf) {
            for (ScopeKind active : scopes) {
                if (required == active) { any = true; break; }
            }
            if (any) break;
        }
        if (!any) return false;
    }
    return true;
}

ResolvedMeaning resolveMeaning(GrammarSchema const& schema,
                               std::string_view lexeme,
                               std::span<ScopeKind const> scopes) {
    ResolvedMeaning out;
    const auto candidates = schema.lookupLexeme(lexeme);
    if (candidates.empty()) return out;

    // A candidate survives iff BOTH the schema's per-scope forbid rules
    // AND the candidate's per-meaning scopeRequire allow it.
    auto candidateAllowed = [&](LexemeMeaning const& m) {
        return schema.isTokenValidInScope(m.id, scopes)
            && meaningAllowedByScopeRequire(m, scopes);
    };

    // Candidates arrive pre-sorted by priority (lowest first, stable on
    // ties). Find the first that survives.
    std::optional<std::size_t> winnerIdx;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidateAllowed(candidates[i])) {
            if (!winnerIdx) winnerIdx = i;
            ++out.matchCount;
        }
    }
    if (!winnerIdx) return out;

    out.meaning = candidates[*winnerIdx];

    // Ambiguity: another scope-valid candidate exists with the same
    // priority as the winner. First-declared still wins, but we want to
    // surface this as P_AmbiguousToken.
    for (std::size_t i = *winnerIdx + 1; i < candidates.size(); ++i) {
        if (candidates[i].priority != out.meaning.priority) break;
        if (candidateAllowed(candidates[i])) {
            out.ambiguous = true;
            break;
        }
    }
    return out;
}

} // namespace

void TreeBuilder::pushToken(Token const& tok) {
    if (finished_) {
        addBuilderInvariant_("pushToken after finish()", tok.span);
        return;
    }
    if (!source_) {
        addBuilderInvariant_(
            "pushToken called on a builder with null source buffer",
            tok.span);
        return;
    }
    if (open_.empty()) {
        addBuilderInvariant_(
            "pushToken called with no open frame; token dropped",
            tok.span);
        return;
    }

    const std::string_view lexeme = source_->slice(tok.span);

    // Resolve the schema meaning. Three paths:
    //  1. Direct lexeme match (operator / punctuation / keyword).
    //  2. Word fallback to Identifier when the lexeme isn't a keyword.
    //  3. No match at all → Error leaf + P_UnknownToken.
    ResolvedMeaning resolved = resolveMeaning(
        *schema_, lexeme, std::span<ScopeKind const>{scopes_});

    if (resolved.matchCount == 0 && tok.coreKind == CoreTokenKind::Word) {
        // Fallback: alphanumeric word that isn't a keyword → Identifier.
        // "Identifier" is pre-interned at schema load time, so a const
        // lookup against the frozen interner always succeeds.
        LexemeMeaning ident{};
        ident.id = schema_->schemaTokens().find("Identifier");
        resolved.meaning   = ident;
        resolved.matchCount = ident.id.valid() ? 1 : 0;
    }

    if (resolved.matchCount == 0) {
        // No meaning matched. Emit P_UnknownToken with the actual lexeme
        // text. Insert an Error leaf so the resulting tree still spans
        // every input byte.
        ParseDiagnostic d;
        d.code     = DiagnosticCode::P_UnknownToken;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = source_->id();
        d.span     = tok.span;
        d.actual   = std::format("'{}'", lexeme);
        if (!open_.empty()) d.ruleContext = open_.back().rule;
        emitDiagnostic_(std::move(d));

        detail::Node errNode{};
        errNode.kind  = NodeKind::Error;
        errNode.flags = NodeFlags::HasError;
        errNode.span  = tok.span;
        const NodeId id = emit_(errNode);
        attachToCurrentFrame_(id);
        propagateHasError_(id);
        return;
    }

    if (resolved.ambiguous) {
        // Multiple equal-priority survivors. First-declared still won
        // (deterministic), but we report so the user can disambiguate
        // the config (or the source itself).
        ParseDiagnostic d;
        d.code     = DiagnosticCode::P_AmbiguousToken;
        d.severity = DiagnosticSeverity::Warning;
        d.buffer   = source_->id();
        d.span     = tok.span;
        d.actual   = std::format("'{}'", lexeme);
        if (!open_.empty()) d.ruleContext = open_.back().rule;
        emitDiagnostic_(std::move(d));
    }

    // Contextual-keyword demotion. When the winning meaning is a soft
    // keyword (`contextual: true` in JSON, OR every keyword under
    // `reservedWordPolicy: "contextual"`), the schema cursor's
    // expectedSet decides whether the keyword survives. Outside the
    // expected set the lexeme degrades to Identifier and an info-level
    // P_ContextualKeywordResolution diagnostic records the demotion.
    //
    // Conservative fallback: when the cursor is invalid (no shape graph
    // position known, e.g. the builder hasn't entered any rule yet, or
    // an earlier advance went off-track) the keyword stays — same as
    // strict policy. This avoids silently turning every soft keyword
    // into an identifier when the parser temporarily diverges from the
    // schema during error recovery.
    if (resolved.meaning.contextual) {
        // Defense-in-depth: loader rejects `contextual: true` on the
        // Identifier kind (see grammar_schema_json.cpp), but anyone
        // fabricating a LexemeMeaning by hand would skip that check.
        // The demotion target equals the source → infinite no-op
        // identity; abort early with a clear message.
        const auto identId = schema_->schemaTokens().find("Identifier");
        if (resolved.meaning.id.v == identId.v) {
            std::fputs("dss::TreeBuilder fatal: contextual meaning resolved to "
                       "Identifier (the demotion target); config bypassed the "
                       "loader's C_MissingField check\n", stderr);
            std::abort();
        }
    }
    if (resolved.meaning.contextual && cursor_.valid()) {
        const auto expected = schema_->expectedSet(cursor_);
        bool inExpected = false;
        for (auto const& t : expected) {
            if (t.v == resolved.meaning.id.v) { inExpected = true; break; }
        }
        if (!inExpected) {
            const auto demotedFrom = resolved.meaning.id;
            const auto identId     = schema_->schemaTokens().find("Identifier");
            LexemeMeaning ident{};
            ident.id = identId;
            resolved.meaning = ident;

            ParseDiagnostic d;
            d.code     = DiagnosticCode::P_ContextualKeywordResolution;
            d.severity = DiagnosticSeverity::Info;
            d.buffer   = source_->id();
            d.span     = tok.span;
            d.actual   = std::format("'{}' as Identifier (demoted from {})",
                                     lexeme,
                                     schema_->schemaTokens().name(demotedFrom));
            if (!open_.empty()) d.ruleContext = open_.back().rule;
            emitDiagnostic_(std::move(d));
        }
    }

    // Apply scope effects from the resolved meaning. Order vs. leaf-
    // attach doesn't change the current token's tree placement (the leaf
    // lands in the open frame regardless), but performing the scope
    // mutation here keeps the snapshot inside any diagnostic emitted by
    // a follow-on token consistent with this token's effect.
    if (resolved.meaning.opensScope != ScopeKind::None) {
        pushScope(resolved.meaning.opensScope);
    }
    if (resolved.meaning.closesScope) {
        popScope();    // emits P_BuilderInvariant on underflow
    }

    // Emit the leaf.
    detail::Node leaf{};
    leaf.kind      = NodeKind::Token;
    leaf.tokenKind = resolved.meaning.id;
    leaf.flags     = resolved.meaning.flagsApplied;
    leaf.span      = tok.span;
    const NodeId id = emit_(leaf);
    attachToCurrentFrame_(id);

    // Walk the schema cursor by the resolved token kind. EmptySpace
    // tokens never appear in the schema's expected sets — whitespace is
    // off-grammar by design — so don't advance through them; doing so
    // would invalidate the cursor on every space and trip the desync
    // diagnostic on every otherwise-clean parse. For non-EmptySpace
    // tokens, if `advance` returns invalid (token not expected at this
    // position) the cursor becomes invalid for the remainder of the
    // build, future contextual resolutions stay strict per the fallback
    // above, and the first valid → invalid transition surfaces one
    // P_SchemaCursorDesync.
    if (!isEmptySpace(resolved.meaning.flagsApplied)) {
        const bool wasValid = cursor_.valid();
        cursor_ = schema_->advance(cursor_, resolved.meaning.id);
        noteCursorDesync_(wasValid, cursor_.valid(), tok.span,
                          open_.empty()
                              ? std::optional<RuleId>{}
                              : std::optional<RuleId>{open_.back().rule});
    }
}

// ── pushError ────────────────────────────────────────────────────────────

void TreeBuilder::pushError(SourceSpan                   span,
                            std::optional<RuleId>        expectedRule,
                            std::optional<SchemaTokenId> expectedToken,
                            std::string_view             note) {
    if (finished_) {
        addBuilderInvariant_("pushError after finish()", span);
        return;
    }
    if (open_.empty()) {
        // Mirror pushToken's invariant: no open frame means the Error
        // node would be orphaned (no parent, never referenced by any
        // child-index slot). Emit P_BuilderInvariant instead of leaking
        // an unreachable node into the arena.
        addBuilderInvariant_(
            "pushError called with no open frame; diagnostic dropped",
            span);
        return;
    }

    ParseDiagnostic d;
    d.code     = DiagnosticCode::P_UnexpectedToken;
    d.severity = DiagnosticSeverity::Error;
    d.buffer   = source_ ? source_->id() : InvalidBuffer;
    d.span     = span;
    if (expectedRule)  d.expected.push_back(std::string{schema_->rules().name(*expectedRule)});
    if (expectedToken) d.expected.push_back(std::string{schema_->schemaTokens().name(*expectedToken)});
    if (!note.empty()) d.actual = std::string{note};
    d.ruleContext = open_.back().rule;
    emitDiagnostic_(std::move(d));

    detail::Node errNode{};
    errNode.kind  = NodeKind::Error;
    errNode.flags = NodeFlags::HasError;
    errNode.span  = span;
    const NodeId id = emit_(errNode);
    attachToCurrentFrame_(id);
    propagateHasError_(id);
}

// ── scope stack ──────────────────────────────────────────────────────────

void TreeBuilder::pushScope(ScopeKind kind) {
    if (finished_) {
        addBuilderInvariant_("pushScope after finish()", SourceSpan::empty(0));
        return;
    }
    scopes_.push_back(kind);
}

void TreeBuilder::popScope() {
    if (finished_) {
        addBuilderInvariant_("popScope after finish()", SourceSpan::empty(0));
        return;
    }
    if (scopes_.empty()) {
        // Underflow — a recovered close that didn't match anything we
        // opened. P_BuilderInvariant + silent recovery (the rest of the
        // build can keep going).
        SourceSpan at = open_.empty()
            ? SourceSpan::empty(0)
            : (open_.back().children.empty()
               ? open_.back().openerSpan
               : nodes_[open_.back().children.back().v].span);
        addBuilderInvariant_("popScope on empty scope stack", at);
        return;
    }
    scopes_.pop_back();
}

ScopeKind TreeBuilder::currentScope() const noexcept {
    return scopes_.empty() ? ScopeKind::None : scopes_.back();
}

std::span<ScopeKind const> TreeBuilder::scopeStack() const noexcept {
    return scopes_;
}

RuleId TreeBuilder::currentRule() const noexcept {
    return open_.empty() ? InvalidRule : open_.back().rule;
}

std::size_t TreeBuilder::openFrameCount() const noexcept {
    return open_.size();
}

// ── finalize ─────────────────────────────────────────────────────────────

Tree TreeBuilder::finish() && {
    if (finished_) {
        // Release-mode protection. We can't usefully return a Tree from
        // a moved-from state, so abort with a clear message — this is a
        // caller bug, not recoverable state.
        std::fputs("dss::TreeBuilder fatal: finish() called twice\n", stderr);
        std::abort();
    }
    finished_ = true;

    // Detect "leftover scope" — a non-empty scope stack at finish means
    // some closer (a `}`, `)`, etc.) never came. Surface it so the user
    // sees the imbalance rather than relying solely on per-frame EOF
    // diagnostics.
    if (!scopes_.empty()) {
        addBuilderInvariant_(
            std::format("scope stack non-empty at finish ({} unbalanced)",
                        scopes_.size()),
            SourceSpan::empty(source_ ? source_->size() : 0));
    }

    // Synthesize Missing for unclosed shapes + emit one P_PrematureEndOfInput
    // per unclosed frame. The deepest frame is reported first (it's the
    // most-specific complaint); each older (outer) frame's diagnostic
    // attaches the deeper-frame openers as related-locations.
    if (!open_.empty()) {
        SourceSpan eofSpan = SourceSpan::empty(source_ ? source_->size() : 0);
        std::vector<RelatedLocation> ancestors;
        ancestors.reserve(open_.size());

        // Walk from deepest (back) to outermost (front). Insert a Missing
        // child marker on each open frame so the tree still has a complete
        // structure that downstream passes can iterate without surprise.
        for (auto it = open_.rbegin(); it != open_.rend(); ++it) {
            detail::Node miss{};
            miss.kind   = NodeKind::Error;
            miss.flags  = NodeFlags::Missing | NodeFlags::Synthetic | NodeFlags::HasError;
            miss.span   = eofSpan;
            miss.parent = it->id;
            const NodeId mid = emit_(miss);
            it->children.push_back(mid);

            // Each frame's diagnostic gets the same EOF span but a
            // distinct ruleContext — the reporter's dedup hash includes
            // ruleContext, so per-frame diagnostics don't collapse.
            ParseDiagnostic d;
            d.code        = DiagnosticCode::P_PrematureEndOfInput;
            d.severity    = DiagnosticSeverity::Error;
            d.buffer      = source_ ? source_->id() : InvalidBuffer;
            d.span        = eofSpan;
            d.ruleContext = it->rule;
            d.actual      = std::format("end of input while inside rule '{}'",
                                        schema_->rules().name(it->rule));
            d.related     = ancestors;
            emitDiagnostic_(std::move(d));

            // After emitting, append this frame's opener to `ancestors`
            // so the next (outer) diagnostic links back to it.
            ancestors.push_back({source_ ? source_->id() : InvalidBuffer,
                                 it->openerSpan,
                                 std::format("inside rule '{}' opened here",
                                             schema_->rules().name(it->rule))});

            propagateHasError_(mid);
        }

        // Close every still-open frame synthetically. closeFrame_ marks
        // cascade-closed (non-target) cookies in closedCookies_ already,
        // but the *target* cookie of each call (the top of the stack) is
        // not — we mark it manually here so the corresponding OpenScope's
        // eventual destructor finds it and no-ops cleanly.
        while (!open_.empty()) {
            const std::uint32_t topCookie = open_.back().cookie;
            closeFrame_(topCookie, /*synthetic*/ true);
            closedCookies_.insert(topCookie);
        }
    }

    // Mint a monotonic TreeId so attribute side-tables can disambiguate
    // values keyed against this tree.
    static std::atomic<std::uint32_t> sTreeIdCounter{0};

    // If open() was never called, the arena holds only the sentinel.
    // Emit an empty Tree with no root rather than claiming NodeId{1} —
    // which doesn't exist and would crash on access.
    const bool neverOpened = (nodes_.size() <= 1);

    detail::TreeData td;
    td.source     = source_;
    // The shared_ptr aliasing constructor lets us share the schema's
    // lifetime while exposing its RuleInterner directly. This keeps Tree's
    // rules() identical to schema().rules() — same namespace, same memory.
    td.rules      = std::shared_ptr<RuleInterner const>(schema_, &schema_->rules());
    td.schema     = schema_;
    td.diagnostics = std::move(reporter_);
    td.id          = TreeId{++sTreeIdCounter};

    if (neverOpened) {
        // Drop the lone sentinel: Tree's invariant is "nodes empty XOR
        // root valid". Empty arena + invalid root is a well-formed
        // empty Tree.
        td.nodes.clear();
        td.childIndex.clear();
        td.root = InvalidNode;
    } else {
        td.nodes       = std::move(nodes_);
        td.childIndex  = std::move(childIndex_);
        td.root        = NodeId{1};      // first real node = the root open()
    }

    return Tree{std::move(td)};
}

} // namespace dss
