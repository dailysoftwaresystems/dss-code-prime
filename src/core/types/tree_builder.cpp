#include "core/types/tree_builder.hpp"

#include "core/substrate/mint_monotonic_id.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace dss {

namespace {

// Always-on fatal-abort for TreeBuilder invariants. Matches the
// project's `*Fatal` pattern (see tree.cpp's `treeFatal` and the
// substrate `detail::arena` fatal helpers in arena_tag.hpp) so contract
// violations halt loudly in release builds, not just under NDEBUG-off asserts.
[[noreturn]] void tbFatal(char const* what) noexcept {
    std::fputs("dss::TreeBuilder fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

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
// Checkpoint — RAII guard returned by TreeBuilder::checkpoint()
// ─────────────────────────────────────────────────────────────────────────

TreeBuilder::Checkpoint::Checkpoint(TreeBuilder* b, std::uint32_t id) noexcept
    : builder_(b), id_(id) {}

TreeBuilder::Checkpoint::Checkpoint(Checkpoint&& other) noexcept
    : builder_(other.builder_), id_(other.id_), disp_(other.disp_) {
    other.builder_ = nullptr;
    other.id_      = 0;
    other.disp_    = Disposition::Committed;     // moved-from is inert
}

TreeBuilder::Checkpoint&
TreeBuilder::Checkpoint::operator=(Checkpoint&& other) noexcept {
    if (this != &other) {
        // Assigning over a live guard rolls back — same as dtor would.
        if (builder_ && disp_ == Disposition::Pending) {
            builder_->rollbackToId_(id_);
        }
        builder_       = other.builder_;
        id_            = other.id_;
        disp_          = other.disp_;
        other.builder_ = nullptr;
        other.id_      = 0;
        other.disp_    = Disposition::Committed;
    }
    return *this;
}

TreeBuilder::Checkpoint::~Checkpoint() noexcept {
    if (!builder_ || disp_ != Disposition::Pending) return;
    // No-op guard (returned when the speculation-depth cap was hit) has
    // no captured state, so destroying it without commit/rollback is
    // not a bug — the P_MaxSpeculationDepth diagnostic already flagged
    // the cap event. Silently inert.
    if (id_ == TreeBuilder::kNoOpCheckpointId) {
        disp_    = Disposition::Committed;
        builder_ = nullptr;
        return;
    }
    builder_->rollbackToId_(id_);
    disp_ = Disposition::RolledBack;

    // Warning goes through forceReport_ so a pre-checkpoint reporter
    // already at hitCap_ doesn't silently swallow this signal. The
    // forgotten-commit bug must reach the user.
    ParseDiagnostic d;
    d.code     = DiagnosticCode::P_UncommittedCheckpoint;
    d.severity = DiagnosticSeverity::Warning;
    d.buffer   = builder_->source_ ? builder_->source_->id() : InvalidBuffer;
    d.span     = SourceSpan::empty(0);
    d.actual   = "Checkpoint destroyed without commit() or rollback() — "
                 "rolled back as a safe default";
    builder_->forceReport_(std::move(d));
    builder_ = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────
// TreeBuilder
// ─────────────────────────────────────────────────────────────────────────

TreeId TreeBuilder::nextTreeId() noexcept {
    return substrate::mintMonotonicId<TreeId>();
}

TreeBuilder::TreeBuilder(std::shared_ptr<SourceBuffer>        src,
                        std::shared_ptr<GrammarSchema const> schema,
                        DiagnosticReporter::Config           diagConfig,
                        BuilderConfig                        builderConfig)
    : source_(std::move(src))
    , schema_(std::move(schema))
    , reporter_(std::make_unique<DiagnosticReporter>(std::move(diagConfig)))
    , treeId_(nextTreeId())
    , arena_(treeId_)   // emplaces the slot-0 sentinel; stamps treeId_ on every id
    // walker_ embeds the SchemaWalker state machine. The `[this]`-
    // capturing lambda is the desync emission callback — it stays
    // valid because (a) TreeBuilder is non-movable (static_assert in
    // the header pins this), and (b) walker_ is destroyed before
    // earlier members during ~TreeBuilder, so the callback can't
    // fire after `this` becomes unsafe to dereference.
    , walker_(schema_,
              [this](SourceSpan span, std::optional<RuleId> rule) {
                  ParseDiagnostic d;
                  d.code        = DiagnosticCode::P_SchemaCursorDesync;
                  d.severity    = DiagnosticSeverity::Info;
                  d.buffer      = source_ ? source_->id() : InvalidBuffer;
                  d.span        = span;
                  d.ruleContext = rule;
                  d.actual      = "schema cursor went off-track; "
                                  "contextual keyword resolution will "
                                  "stay strict for the remainder of "
                                  "the build";
                  emitDiagnostic_(std::move(d));
              })
    , builderConfig_(builderConfig) {
    // Constructor preconditions: source_ and schema_ must be non-null.
    // We accept a null source in tests of degenerate paths (the diagnostic
    // emit fallback handles it), but a null schema means pushToken has
    // nothing to resolve against — fail fast.
    if (!schema_) tbFatal("schema is null");
    // Slot 0 of the arena is reserved as InvalidNode — every real NodeId
    // starts at 1. The ArenaBuilder emplaces that sentinel at construction.

    // Body-mode defaultToken kinds come from the schema (computed once
    // at load time; shared with the parser). The error + identifier
    // sentinels are cached for the per-token resolveMeaning hot path.
    bodyDefaultTokenKinds_ = &schema_->bodyDefaultTokenKinds();
    errorKind_      = schema_->schemaTokens().find("Error");
    identifierKind_ = schema_->schemaTokens().find("Identifier");

    // Both are predeclared by the loader's `kBuiltinTokenKindNames` list
    // — they MUST resolve. A missing "Error" sentinel would silently
    // turn every Error-kind token into a clean synthesized leaf
    // (resolveMeaning's Path A/B guard compares `preResolved !=
    // errorKind` to gate synthesis); a missing "Identifier" would
    // break the Word fallback + contextual-keyword demotion. Better
    // to halt at construction than corrupt every parse.
    if (!errorKind_.valid())      tbFatal("schema missing predeclared 'Error' token kind");
    if (!identifierKind_.valid()) tbFatal("schema missing predeclared 'Identifier' token kind");
}

NodeId TreeBuilder::emit_(detail::Node n) {
    return arena_.addNode(n);   // appends + stamps treeId_, returns the tagged id
}

bool TreeBuilder::attachToCurrentFrame_(NodeId id) {
    if (open_.empty()) return false;
    pendingChildren_.push_back(id);
    // Backpatch parent on the just-emitted node so HasError propagation
    // can walk via the stored parent link rather than the open-frame stack
    // (frames may have already closed by the time a deep error fires).
    arena_.at(id).parent = open_.back().id;
    return true;
}

// Helper: range of pendingChildren_ belonging to the top open frame.
// Empty if there's no open frame.
[[nodiscard]] std::span<NodeId const> TreeBuilder::topFramePendingChildren_() const noexcept {
    if (open_.empty()) return {};
    auto const start = open_.back().pendingStart;
    return std::span<NodeId const>{pendingChildren_}.subspan(start);
}

void TreeBuilder::emitDiagnostic_(ParseDiagnostic d) {
    // Snapshot the scope stack at error-time. Done here (rather than at
    // call sites) so every diagnostic gets it for free.
    d.scopeStack.assign(scopes_.begin(), scopes_.end());
    reporter_->report(std::move(d));
}

void TreeBuilder::forceReport_(ParseDiagnostic d) {
    d.scopeStack.assign(scopes_.begin(), scopes_.end());
    reporter_->forceReport(std::move(d));
}

void TreeBuilder::reportDiagnostic(ParseDiagnostic d) {
    if (finished_) {
        addBuilderInvariant_(
            "reportDiagnostic() after finish()", d.span);
        return;
    }
    emitDiagnostic_(std::move(d));
}

void TreeBuilder::ingestDiagnostics(std::span<ParseDiagnostic const> diags) {
    if (finished_) {
        addBuilderInvariant_("ingestDiagnostics() after finish()",
                             SourceSpan::empty(0));
        return;
    }
    // Report verbatim: external diagnostics already carry their own buffer,
    // span, and scope. Do NOT route through emitDiagnostic_ — that would
    // overwrite scopeStack with the builder's (empty/irrelevant for lexer
    // diags). Goes through report() so the cap/dedup window still applies.
    for (auto const& d : diags) {
        reporter_->report(d);
    }
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
    // Plan 05 post-close substrate fix: HasError propagation MUST be
    // lazy (frame-close-driven) so speculative rollback doesn't leave
    // pre-existing ancestor flags stuck. The eager walk-up-from-Error-
    // leaf previously implemented here mutated ancestors that existed
    // BEFORE the speculative checkpoint — ancestors the checkpoint's
    // `arena.truncateTo(nodesSize)` rollback cannot un-mutate (the
    // ancestor isn't truncated; its flag is). Result: a failed
    // speculative branch that pushed an Error leaf would corrupt the
    // committed work's HasError flag chain. The frame-close OR-reduce
    // loop in `closeTopFrame_` (around tree_builder.cpp:472-474)
    // correctly OR-reduces HasError from EACH frame's children into
    // the closing parent ONLY when the frame commits — work that
    // rolls back never reaches its close, so its Error leaves never
    // pollute the upper chain. Removing the eager walk here is the
    // correct contract: propagation is the OR-reduce's job, not this
    // function's. The caller's `errNode.flags |= NodeFlags::HasError`
    // on the Error leaf is preserved; that flag's children-to-parent
    // OR-reduction happens at commit time via the natural close path.
    (void)start;
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
    if (!open_.empty()) {
        const auto parentChildren = topFramePendingChildren_();
        if (!parentChildren.empty()) {
            const NodeId lastChild = parentChildren.back();
            opener = SourceSpan::empty(arena_.at(lastChild).span.end());
        }
    }
    n.span = opener;

    const NodeId id = emit_(n);
    if (!open_.empty()) {
        arena_.at(id).parent = open_.back().id;
        // Register the new internal node as a child of the parent frame
        // now, while parent is still open. closeFrame_ flushes the
        // parent's pending range into childIndex_; without this push the
        // subtree would vanish from the tree.
        pendingChildren_.push_back(id);
    }

    // Wraparound is theoretical (4B opens in one build), but it would
    // collide with the "invalid" sentinel and cause spurious LIFO
    // violation diagnostics — fatal-halt early if it ever happens.
    if (nextCookie_ == 0) tbFatal("cookie counter wrapped around");
    const std::uint32_t cookie = nextCookie_++;
    // The new frame owns pendingChildren_ from this point forward.
    const auto pendingStart = static_cast<std::uint32_t>(pendingChildren_.size());
    open_.push_back(Frame{
        .id           = id,
        .rule         = rule,
        .openerSpan   = opener,
        .pendingStart = pendingStart,
        .cookie       = cookie,
    });

    // Walk the schema-cursor state machine in parallel with the open
    // frame. The walker handles the AltChoice → RuleLeaf routing
    // (`routeToRuleLeaf`) internally so closeFrame_'s `leaveRule`
    // resumes cleanly when this rule completes.
    walker_.enterRule(rule);

    return OpenScope{this, cookie};
}

TreeBuilder::OpenScope TreeBuilder::wrapLastChildInFrame(RuleId rule) & {
    if (finished_) {
        addBuilderInvariant_("wrapLastChildInFrame() after finish()",
                             SourceSpan::empty(0));
        return OpenScope{nullptr, 0};
    }
    if (open_.empty()) {
        addBuilderInvariant_(
            "wrapLastChildInFrame() with no open parent frame",
            SourceSpan::empty(0));
        return OpenScope{nullptr, 0};
    }
    // The parent must have at least one child to wrap.
    auto const parentPendingStart = open_.back().pendingStart;
    if (pendingChildren_.size()
        == static_cast<std::size_t>(parentPendingStart)) {
        addBuilderInvariant_(
            "wrapLastChildInFrame() requires at least one pending child",
            SourceSpan::empty(0));
        return OpenScope{nullptr, 0};
    }

    // Detach the subtree from its current parent slot. The arena
    // node and its descendants stay put — we're only moving the
    // parent's pointer.
    const NodeId childToWrap = pendingChildren_.back();
    pendingChildren_.pop_back();

    // Open the new wrapper frame the normal way. `open()` allocates
    // the wrapper's Internal node, attaches it as a pending child of
    // the parent (taking the slot vacated by the pop above), and
    // pushes the new Frame whose `pendingStart` points at the
    // post-attach size of `pendingChildren_`. Wrapped in try/catch so
    // a `std::bad_alloc` on the arena grow doesn't leak `childToWrap`
    // from the parent's pending list (the strong-exception-safety
    // contract `OpenScope` callers expect — "either the wrap landed
    // or the builder state is byte-identical").
    OpenScope guard{nullptr, 0};
    try {
        guard = open(rule);
    } catch (...) {
        pendingChildren_.push_back(childToWrap);
        throw;
    }
    if (open_.empty()) {
        // Defensive: `open()` returned a no-op guard. This is
        // unreachable from here because `finished_` is gated at the
        // top of this method and `open()`'s only no-op path is the
        // `finished_` check. Kept for symmetry with the catch above.
        pendingChildren_.push_back(childToWrap);
        return guard;
    }

    // Re-attach the popped subtree under the wrapper. The wrapper's
    // children region is now `[wrapper.pendingStart, current size)` =
    // `[childToWrap]` — the load-bearing post-condition the rest of
    // this function (parent fixup + span anchor) depends on.
    pendingChildren_.push_back(childToWrap);

    // Fix up the wrapped child's parent pointer and the wrapper's
    // span so subsequent span rollup is sensible. The wrapper now
    // starts where the wrapped subtree starts.
    arena_.at(childToWrap).parent = open_.back().id;
    const auto childSpan = arena_.at(childToWrap).span;
    open_.back().openerSpan = SourceSpan::empty(childSpan.start());
    arena_.at(open_.back().id).span =
        SourceSpan::empty(childSpan.start());

    return guard;
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
        auto const topChildren = topFramePendingChildren_();
        addBuilderInvariant_(
            std::format("close() with unknown cookie {}", cookie),
            topChildren.empty()
                ? open_.back().openerSpan
                : arena_.at(topChildren.back()).span);
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
        detail::Node& node = arena_.at(fr.id);

        // Flush this frame's pending range to childIndex_ contiguously.
        // The range is `pendingChildren_[fr.pendingStart..end)`. After
        // flush, truncate pendingChildren_ back to fr.pendingStart so
        // the parent frame's region is re-exposed at the top.
        const auto firstChild = static_cast<std::uint32_t>(childIndex_.size());
        const auto pendEnd    = static_cast<std::uint32_t>(pendingChildren_.size());
        for (std::uint32_t i = fr.pendingStart; i < pendEnd; ++i) {
            NodeId child = pendingChildren_[i];
            childIndex_.push_back(child);
            // Roll the span up from this child onto our node.
            node.span = SourceSpan::join(node.span, arena_.at(child).span);
            // OR-reduce HasError. The attach paths already propagated
            // eagerly, but synthetic Missing inserted directly into
            // pendingChildren_ by finish() bypasses attachToCurrentFrame_;
            // this loop catches it.
            if (hasError(arena_.at(child).flags)) {
                node.flags |= NodeFlags::HasError;
            }
        }
        const auto childCount = pendEnd - fr.pendingStart;
        pendingChildren_.resize(fr.pendingStart);
        node.firstChild = firstChild;
        node.childCount = childCount;

        const bool isTarget = (fr.cookie == cookie);
        if (!isTarget) {
            // This frame was cascade-closed — its OpenScope is still
            // alive and will eventually call close() once destroyed.
            // Record the cookie so that future call is a clean no-op.
            closedCookies_.insert(fr.cookie);
        }
        open_.pop_back();
        // Invariant: walker_.depth() == open_.size() at every
        // open/close boundary. leaveRule on a non-RuleLeaf saved
        // cursor returns invalid and trips the walker's desync
        // callback — strict-only contextual resolution for the
        // remainder of the build.
        walker_.leaveRule(fr.openerSpan, std::optional<RuleId>{fr.rule});
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

// Universal literal kinds the tokenizer is allowed to pre-resolve.
// These names are predeclared at schema load (see
// kBuiltinTokenKindNames in grammar_schema_json.cpp) so the interner
// always has them. The list is the synthesis-allowlist's "non-body-
// mode" half — body kinds come from the caller's `bodyKinds` set.
//
// Paradigm-specific kinds (`BoolLiteral`/`CharLiteral`/`NullLiteral`)
// were pruned in the 08.55 cleanup; the tokenizer never emits them
// directly — a language that uses them resolves the lexeme via its
// own `tokens`/`keywords` block, which produces a real per-lexeme
// candidate (no synthesis required).
[[nodiscard]] bool isBuiltinLiteralKind(GrammarSchema const& schema,
                                        SchemaTokenId id) noexcept {
    static constexpr std::string_view kLiterals[] = {
        "IntLiteral", "FloatLiteral", "StringLiteral", "CharLiteral",
    };
    const auto name = schema.schemaTokens().name(id);
    for (auto lit : kLiterals) {
        if (name == lit) return true;
    }
    return false;
}

// Synthesize a `ResolvedMeaning` for a tokenizer-pre-resolved kind that
// has no entry in the per-lexeme schema table (numeric literals like
// `5`, body-mode default tokens like `StringChar`). The synthesized
// meaning carries the pre-resolved kind directly and bypasses the
// per-lexeme priority scan.
//
// Scope filter: synthesis honors the schema's per-scope forbid rules
// (`isTokenValidInScope`). Skipping this would let a built-in literal
// forbidden inside a particular scope smuggle past — e.g. a schema
// that declares `forbid: ["IntLiteral"]` inside a Type-position scope
// expects the rejection to flow through `P_UnknownToken` recovery,
// not silently land as a clean leaf. Returns empty `ResolvedMeaning`
// when rejected so the caller falls back to the Word/Error path.
//
// Drift guard: the kind MUST be either a body-mode default OR a known
// built-in literal — the only two cases the tokenizer is licensed to
// pre-resolve without a per-lexeme entry. Anything else means
// tokenizer / schema drift; fatal-abort rather than synthesize a
// silently-wrong leaf in release builds.
[[nodiscard]] ResolvedMeaning makeSyntheticMeaning(
    GrammarSchema const& schema,
    SchemaTokenId preResolved,
    std::unordered_set<SchemaTokenId> const& bodyKinds,
    std::span<ScopeKind const> scopes) noexcept {
    if (!(bodyKinds.contains(preResolved)
          || isBuiltinLiteralKind(schema, preResolved))) {
        tbFatal("resolveMeaning synthesizing for a kind that is neither "
                "a body-mode defaultToken nor a built-in literal — "
                "likely a tokenizer/schema drift bug");
    }
    if (!schema.isTokenValidInScope(preResolved, scopes)) {
        return ResolvedMeaning{};
    }
    ResolvedMeaning out;
    LexemeMeaning synthetic{};
    synthetic.id   = preResolved;
    out.meaning    = synthetic;
    out.matchCount = 1;
    return out;
}

ResolvedMeaning resolveMeaning(GrammarSchema const& schema,
                               std::string_view lexeme,
                               std::span<ScopeKind const> scopes,
                               std::unordered_set<SchemaTokenId> const& bodyKinds,
                               SchemaTokenId errorKind,
                               SchemaTokenId preResolved = InvalidSchemaToken) {
    ResolvedMeaning out;
    const auto candidates = schema.lookupLexeme(lexeme);

    // Path A: tokenizer pre-resolved AND the per-lexeme table is empty
    // for this lexeme (numeric literals, body-mode chars whose byte
    // isn't a declared lexeme). Trust the tokenizer's classification;
    // the slow path has nothing to consider. Error stays excluded so
    // recovery flows through P_UnknownToken.
    if (candidates.empty() && preResolved.valid()) {
        if (preResolved != errorKind) {
            return makeSyntheticMeaning(schema, preResolved, bodyKinds, scopes);
        }
        return out;
    }
    if (candidates.empty()) return out;

    // A candidate survives iff BOTH the schema's per-scope forbid rules
    // AND the candidate's per-meaning scopeRequire allow it.
    auto candidateAllowed = [&](LexemeMeaning const& m) {
        return schema.isTokenValidInScope(m.id, scopes)
            && meaningAllowedByScopeRequire(m, scopes);
    };

    // Fast path: tokenizer pre-resolved a `schemaKind`. Find the named
    // meaning, confirm it survives the scope filter, then still run
    // the same-priority ambiguity scan — bypassing the priority scan
    // must NOT bypass the P_AmbiguousToken warning the builder would
    // otherwise emit. Falls through when the fast path's winner
    // doesn't satisfy (preResolved missing OR scope-rejected) — keeps
    // multi-meaning-with-scope cases (toy's `<` outside-vs-inside-
    // Generic) working.
    if (preResolved.valid()) {
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i].id != preResolved) continue;
            if (!candidateAllowed(candidates[i])) continue;
            out.meaning = candidates[i];
            for (auto const& m : candidates) {
                if (candidateAllowed(m)) ++out.matchCount;
            }
            for (std::size_t j = i + 1; j < candidates.size(); ++j) {
                if (candidates[j].priority != out.meaning.priority) break;
                if (candidateAllowed(candidates[j])) {
                    out.ambiguous = true;
                    break;
                }
            }
            return out;
        }
        // Path B: preResolved is a built-in/body kind that the per-
        // lexeme table doesn't list at all. Trust the tokenizer — the
        // tokenizer knew the mode context, the slow path doesn't.
        // E.g. closing `'` in string-body mode emits StringChar but
        // the schema's `'` lexeme lists StringStart; the body-mode
        // classification must win. Distinct from the scope-rejected
        // case below: if preResolved IS in candidates but scope-
        // rejected, fall through so a sibling scope-allowed candidate
        // can win.
        bool preResolvedInCandidates = false;
        for (auto const& m : candidates) {
            if (m.id == preResolved) { preResolvedInCandidates = true; break; }
        }
        if (!preResolvedInCandidates && preResolved != errorKind) {
            return makeSyntheticMeaning(schema, preResolved, bodyKinds, scopes);
        }
    }

    // Slow path: priority-ordered survivor scan (candidates arrive
    // pre-sorted by priority, lowest first, stable on ties).
    std::optional<std::size_t> winnerIdx;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidateAllowed(candidates[i])) {
            if (!winnerIdx) winnerIdx = i;
            ++out.matchCount;
        }
    }
    if (!winnerIdx) return out;

    out.meaning = candidates[*winnerIdx];
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
    //  1. Direct lexeme match (operator / punctuation / keyword). When
    //     `tok.schemaKind` is valid (tokenizer pre-resolved) the
    //     resolveMeaning fast-path short-circuits the priority scan.
    //  2. Word fallback to Identifier when the lexeme isn't a keyword.
    //  3. No match at all → Error leaf + P_UnknownToken.
    ResolvedMeaning resolved = resolveMeaning(
        *schema_, lexeme, std::span<ScopeKind const>{scopes_},
        *bodyDefaultTokenKinds_, errorKind_, tok.schemaKind);

    if (resolved.matchCount == 0 && tok.coreKind == CoreTokenKind::Word) {
        // Fallback: alphanumeric word that isn't a keyword → Identifier.
        // Identifier is validated at ctor (see identifierKind_); the
        // lookup against the frozen interner always succeeds.
        LexemeMeaning ident{};
        ident.id            = identifierKind_;
        resolved.meaning    = ident;
        resolved.matchCount = 1;
    }

    if (resolved.matchCount == 0) {
        // No meaning matched. Emit P_UnknownToken with the actual lexeme
        // text. Insert an Error leaf so the resulting tree still spans
        // every input byte.
        ParseDiagnostic d;
        d.code        = DiagnosticCode::P_UnknownToken;
        d.severity    = DiagnosticSeverity::Error;
        d.buffer      = source_->id();
        d.span        = tok.span;
        d.actual      = std::format("'{}'", lexeme);
        d.ruleContext = open_.back().rule;
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
        d.code        = DiagnosticCode::P_AmbiguousToken;
        d.severity    = DiagnosticSeverity::Warning;
        d.buffer      = source_->id();
        d.span        = tok.span;
        d.actual      = std::format("'{}'", lexeme);
        d.ruleContext = open_.back().rule;
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
        if (resolved.meaning.id.v == identifierKind_.v) {
            tbFatal("contextual meaning resolved to Identifier (the "
                    "demotion target); config bypassed the loader's "
                    "C_MissingField check");
        }
    }
    if (resolved.meaning.contextual && walker_.cursor().valid()) {
        const auto expected = walker_.expectedSet();
        bool inExpected = false;
        for (auto const& t : expected) {
            if (t.v == resolved.meaning.id.v) { inExpected = true; break; }
        }
        if (!inExpected) {
            const auto demotedFrom = resolved.meaning.id;
            LexemeMeaning ident{};
            ident.id         = identifierKind_;
            resolved.meaning = ident;

            ParseDiagnostic d;
            d.code        = DiagnosticCode::P_ContextualKeywordResolution;
            d.severity    = DiagnosticSeverity::Info;
            d.buffer      = source_->id();
            d.span        = tok.span;
            d.actual      = std::format("'{}' as Identifier (demoted from {})",
                                        lexeme,
                                        schema_->schemaTokens().name(demotedFrom));
            d.ruleContext = open_.back().rule;
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

    // Emit the leaf. The effective flag set is the OR of the schema
    // meaning's `flagsApplied` (set declaratively on the lexeme entry)
    // and the tokenizer-supplied `tok.flags` (e.g. `EmptySpace` on
    // body-mode emissions where the LexerMode declared
    // `defaultToken.flags`). Both sources of flag intent reach the
    // AST; closes the v2-gap-catalog row 3 path end-to-end.
    const NodeFlags effectiveFlags = resolved.meaning.flagsApplied | tok.flags;
    detail::Node leaf{};
    leaf.kind      = NodeKind::Token;
    leaf.tokenKind = resolved.meaning.id;
    leaf.flags     = effectiveFlags;
    leaf.span      = tok.span;
    const NodeId id = emit_(leaf);
    attachToCurrentFrame_(id);

    // Walk the schema cursor by the resolved token kind. EmptySpace
    // tokens never appear in the schema's expected sets — whitespace is
    // off-grammar by design — so don't advance through them; doing so
    // would invalidate the cursor on every space and trip the desync
    // diagnostic on every otherwise-clean parse.
    //
    // Body-mode defaultToken kinds (StringChar, CommentChar,
    // BracketIdChar — whatever a `lexerModes.<name>.defaultToken.kind`
    // declares) are off-grammar by construction: the schema never
    // references them in any shape. Skipping the cursor advance for
    // them avoids one spurious desync per body codepoint without
    // requiring the schema author to flag them as EmptySpace (which
    // would also remove them from the AST — wrong for string contents).
    //
    // For non-EmptySpace, non-body-mode tokens, if `advance` returns
    // invalid (token not expected at this position) the cursor becomes
    // invalid for the remainder of the build, future contextual
    // resolutions stay strict per the fallback above, and the first
    // valid → invalid transition surfaces one P_SchemaCursorDesync.
    const bool isOffGrammar =
        isEmptySpace(effectiveFlags)
        || bodyDefaultTokenKinds_->contains(resolved.meaning.id);
    if (!isOffGrammar) {
        walker_.advance(resolved.meaning.id, tok.span,
                        std::optional<RuleId>{open_.back().rule});
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

void TreeBuilder::pushErrorNode(SourceSpan span) {
    if (finished_) {
        addBuilderInvariant_("pushErrorNode after finish()", span);
        return;
    }
    if (open_.empty()) {
        addBuilderInvariant_(
            "pushErrorNode called with no open frame; node dropped",
            span);
        return;
    }
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
        SourceSpan at = SourceSpan::empty(0);
        if (!open_.empty()) {
            const auto topChildren = topFramePendingChildren_();
            at = topChildren.empty()
                ? open_.back().openerSpan
                : arena_.at(topChildren.back()).span;
        }
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
    // per unclosed frame. Process innermost-out so each Missing child is
    // appended to the top frame's pendingChildren_ range (the only one
    // currently accessible) and the frame is closed before moving on to
    // the next outer one. Each outer frame's diagnostic attaches the
    // deeper-frame openers as related-locations.
    if (!open_.empty()) {
        SourceSpan eofSpan = SourceSpan::empty(source_ ? source_->size() : 0);
        std::vector<RelatedLocation> ancestors;
        ancestors.reserve(open_.size());

        while (!open_.empty()) {
            Frame& fr = open_.back();
            detail::Node miss{};
            miss.kind   = NodeKind::Error;
            miss.flags  = NodeFlags::Missing | NodeFlags::Synthetic | NodeFlags::HasError;
            miss.span   = eofSpan;
            miss.parent = fr.id;
            const NodeId mid = emit_(miss);
            pendingChildren_.push_back(mid);

            // Each frame's diagnostic gets the same EOF span but a
            // distinct ruleContext — the reporter's dedup hash includes
            // ruleContext, so per-frame diagnostics don't collapse.
            ParseDiagnostic d;
            d.code        = DiagnosticCode::P_PrematureEndOfInput;
            d.severity    = DiagnosticSeverity::Error;
            d.buffer      = source_ ? source_->id() : InvalidBuffer;
            d.span        = eofSpan;
            d.ruleContext = fr.rule;
            d.actual      = std::format("end of input while inside rule '{}'",
                                        schema_->rules().name(fr.rule));
            d.related     = ancestors;
            emitDiagnostic_(std::move(d));

            ancestors.push_back({source_ ? source_->id() : InvalidBuffer,
                                 fr.openerSpan,
                                 std::format("inside rule '{}' opened here",
                                             schema_->rules().name(fr.rule))});

            propagateHasError_(mid);

            // Close this frame synthetically. closeFrame_ marks cascade-
            // closed (non-target) cookies in closedCookies_ already, but
            // the *target* cookie (this frame) is not — mark it manually
            // so the matching OpenScope's eventual destructor no-ops.
            const std::uint32_t cookie = fr.cookie;
            closeFrame_(cookie, /*synthetic*/ true);
            closedCookies_.insert(cookie);
        }
    }

    // TreeId was minted at construction (treeId_) so every NodeId emit_
    // produced already carries its tag. Reuse it here for td.id and the
    // root NodeId so the bundle round-trips with consistent tagging.

    // If open() was never called, the arena holds only the sentinel.
    // Emit an empty Tree with no root rather than claiming NodeId{1} —
    // which doesn't exist and would crash on access.
    const bool neverOpened = (arena_.size() <= 1);

    detail::TreeData td;
    td.source     = source_;
    // The shared_ptr aliasing constructor lets us share the schema's
    // lifetime while exposing its RuleInterner directly. This keeps Tree's
    // rules() identical to schema().rules() — same namespace, same memory.
    td.rules      = std::shared_ptr<RuleInterner const>(schema_, &schema_->rules());
    td.schema     = schema_;
    td.diagnostics = std::move(reporter_);

    using NodeArena = substrate::ArenaContainer<detail::Node, NodeId, TreeId>;
    if (neverOpened) {
        // Drop the lone sentinel: Tree's invariant is "nodes empty XOR
        // root valid". Empty arena + invalid root is a well-formed empty
        // Tree. The arena still carries treeId_ so the empty tree keeps its id.
        td.arena      = NodeArena{std::vector<detail::Node>{}, treeId_};
        td.childIndex.clear();
        td.root        = InvalidNode;
    } else {
        td.arena       = std::move(arena_).finish();  // freeze: ArenaBuilder → ArenaContainer
        td.childIndex  = std::move(childIndex_);
        td.root        = NodeId{1, treeId_.v};  // first real node = the root open()
    }

    return Tree{std::move(td)};
}

// ── checkpoint / commit / rollback ───────────────────────────────────────

TreeBuilder::Checkpoint TreeBuilder::checkpoint() {
    // Past the cap, return a no-op guard (id = kNoOpCheckpointId).
    // commit/rollback on it short-circuit; the dtor recognizes the
    // sentinel and skips the forgotten-commit warning.
    if (checkpointStack_.size() >= builderConfig_.maxSpeculationDepth) {
        if (!maxSpeculationDepthReached_) {
            maxSpeculationDepthReached_ = true;
            ParseDiagnostic d;
            d.code     = DiagnosticCode::P_MaxSpeculationDepth;
            d.severity = DiagnosticSeverity::Error;
            d.buffer   = source_ ? source_->id() : InvalidBuffer;
            d.span     = SourceSpan::empty(0);
            d.actual   = std::format(
                "speculation depth cap ({}) reached; further checkpoints are no-ops",
                builderConfig_.maxSpeculationDepth);
            emitDiagnostic_(std::move(d));
        }
        return Checkpoint{this, kNoOpCheckpointId};
    }

    CheckpointSnapshot snap;
    snap.nodesSize                  = arena_.size();
    snap.childIndexSize             = childIndex_.size();
    snap.pendingChildrenSize        = pendingChildren_.size();
    snap.openFrames                 = open_;
    snap.scopes                     = scopes_;
    snap.walker                     = walker_.snapshot();
    snap.nextCookie                 = nextCookie_;
    snap.closedCookies              = closedCookies_;
    snap.maxSpeculationDepthReached = maxSpeculationDepthReached_;
    snap.reporterSnap               = reporter_->snapshotForRollback();

    const auto id = nextCheckpointId_++;
    checkpointStack_.emplace_back(id, std::move(snap));
    return Checkpoint{this, id};
}

void TreeBuilder::commit(Checkpoint&& cp) noexcept {
    if (!cp.builder_ || cp.disp_ != Checkpoint::Disposition::Pending) return;
    commitToId_(cp.id_);
    cp.disp_   = Checkpoint::Disposition::Committed;
    cp.builder_ = nullptr;
}

void TreeBuilder::rollback(Checkpoint&& cp) noexcept {
    if (!cp.builder_ || cp.disp_ != Checkpoint::Disposition::Pending) return;
    rollbackToId_(cp.id_);
    cp.disp_   = Checkpoint::Disposition::RolledBack;
    cp.builder_ = nullptr;
}

void TreeBuilder::commitToId_(std::uint32_t id) noexcept {
    if (id == kNoOpCheckpointId) return;
    // Linear search by id (depth ≤ maxSpeculationDepth, typically ≤ 64
    // — trivially fast and cache-friendly vs. a hashmap). The invariant
    // is "each snapshot owns its id"; index arithmetic against the
    // stack's size silently breaks on inner-commit-then-outer-rollback
    // sequences, so we search instead.
    auto it = std::ranges::find_if(checkpointStack_,
        [id](auto const& e) { return e.first == id; });
    if (it == checkpointStack_.end()) {
        // Stale id — either already finalized or never belonged to
        // this builder. Caller bug; surface it loudly.
        addBuilderInvariant_(
            std::format("commit() with stale or unknown Checkpoint id {}", id),
            SourceSpan::empty(0));
        return;
    }
    // If this isn't the top of the stack, the caller committed an outer
    // checkpoint while an inner one is still Pending. Inner snapshots
    // would silently leak — the user almost certainly didn't intend
    // this. Surface and cascade-discard.
    if (it + 1 != checkpointStack_.end()) {
        addBuilderInvariant_(
            "commit() on outer Checkpoint while inner Checkpoint is still "
            "Pending; inner speculative state will be cascade-discarded",
            SourceSpan::empty(0));
    }
    checkpointStack_.erase(it, checkpointStack_.end());
}

void TreeBuilder::rollbackToId_(std::uint32_t id) noexcept {
    if (id == kNoOpCheckpointId) return;
    auto it = std::ranges::find_if(checkpointStack_,
        [id](auto const& e) { return e.first == id; });
    if (it == checkpointStack_.end()) {
        addBuilderInvariant_(
            std::format("rollback() with stale or unknown Checkpoint id {}", id),
            SourceSpan::empty(0));
        return;
    }

    // Move the target snapshot out before erasing so we don't restore
    // from a soon-to-be-destroyed object. Inner snapshots (if any)
    // become irrelevant — they cover speculative state that the rollback
    // is about to undo wholesale.
    CheckpointSnapshot snap = std::move(it->second);
    checkpointStack_.erase(it, checkpointStack_.end());

    // Restore in topological order: arena first, then dependent vectors,
    // then the cursor/scope/cookie state, then the reporter.
    arena_.truncateTo(snap.nodesSize);
    childIndex_.resize(snap.childIndexSize);
    pendingChildren_.resize(snap.pendingChildrenSize);
    open_                       = std::move(snap.openFrames);
    scopes_                     = std::move(snap.scopes);
    walker_.restore(std::move(*snap.walker));
    nextCookie_                 = snap.nextCookie;
    closedCookies_              = std::move(snap.closedCookies);
    maxSpeculationDepthReached_ = snap.maxSpeculationDepthReached;
    reporter_->truncateTo(snap.reporterSnap);
}

} // namespace dss
