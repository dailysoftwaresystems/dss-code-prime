#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/schema_walker.hpp"
#include "core/types/scope_kind.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace dss {

// Builder-level knobs that are NOT diagnostic-reporter concerns.
struct DSS_EXPORT BuilderConfig {
    // Hard cap on simultaneously-active checkpoints (depth). Pathological
    // grammars could otherwise pile speculative frames without bound.
    // Exceeded → `P_MaxSpeculationDepth` (one-shot per build) and the
    // returned Checkpoint is a no-op guard (commit + rollback are no-ops
    // and no state was captured).
    std::size_t maxSpeculationDepth = 64;
};

// Schema-aware mutable tree assembler. Constructed with a source buffer +
// GrammarSchema, drives the building of a single Tree, and is consumed by
// `finish() &&`.
//
// Single-threaded by design; not internally synchronized. Concurrent
// access to one instance is undefined.
//
// SCOPE
// -----
// Validates everything *within* an open frame:
//   - lexeme → SchemaTokenId resolution via schema.lookupLexeme
//   - scope-stack filtering via schema.isTokenValidInScope
//   - priority + first-declared tiebreak (P_AmbiguousToken on equal-priority match)
//   - NodeFlags propagation from a meaning's `flagsApplied` set at schema load
//     (which already includes EmptySpace when the config marks it so)
//   - opensScope / closesScope effects on the builder's scope stack
//   - HasError propagation on Error/Missing insertion (immediate parent walk)
//   - Builder-internal invariant violations (P_BuilderInvariant)
//   - Premature-EOF synthesis on finish() with unclosed frames
//
// Sequence-level validation (that `open(rule)` is allowed at the parent's
// current sequence position, and that closing leaves the parent in an
// "all required children seen" state) is the parser's responsibility —
// the builder trusts the caller. Wiring those checks requires extending
// `GrammarSchema` with a navigable compiled shape graph; that lands with
// the parser.
class DSS_EXPORT TreeBuilder {
public:
    // RAII scope guard returned by open(). Destruction closes the node
    // (if not already closed); explicit close() is idempotent. Move-only
    // — copying would leave two guards racing to close the same frame.
    class DSS_EXPORT OpenScope {
    public:
        OpenScope(OpenScope&& other) noexcept;
        OpenScope& operator=(OpenScope&& other) noexcept;
        OpenScope(OpenScope const&)            = delete;
        OpenScope& operator=(OpenScope const&) = delete;
        ~OpenScope() noexcept;

        // Explicit close. Idempotent: second call is a no-op.
        // No-op on a moved-from instance.
        void close() noexcept;

        [[nodiscard]] bool isOpen() const noexcept { return builder_ != nullptr; }

    private:
        friend class TreeBuilder;
        OpenScope(TreeBuilder* b, std::uint32_t cookie) noexcept;
        TreeBuilder*   builder_ = nullptr;
        // Unique per-frame cookie. The builder uses it both to find the
        // matching frame on close (catching LIFO violations — the
        // requested cookie won't be at the top of the open-stack) and to
        // distinguish frames that have already been cascade-closed (those
        // cookies move to a small set so subsequent OpenScope close()s
        // for them no-op cleanly).
        std::uint32_t  cookie_  = 0;
    };

    // Move-only RAII guard from `checkpoint()`. Dtor rolls back if still
    // Pending — silent commit is the wrong default for a backtracking
    // primitive. Precondition: the producing `TreeBuilder` must outlive
    // this guard. Storing a Checkpoint past the builder's destruction
    // is UB. TreeBuilder is non-copyable/non-movable, so the back-
    // pointer stays valid for the builder's lifetime.
    class DSS_EXPORT Checkpoint {
    public:
        enum class Disposition : std::uint8_t { Pending, Committed, RolledBack };

        Checkpoint(Checkpoint&& other) noexcept;
        Checkpoint& operator=(Checkpoint&& other) noexcept;
        Checkpoint(Checkpoint const&)            = delete;
        Checkpoint& operator=(Checkpoint const&) = delete;
        ~Checkpoint() noexcept;

        [[nodiscard]] Disposition disposition() const noexcept { return disp_; }
        [[nodiscard]] bool        isPending()   const noexcept { return disp_ == Disposition::Pending; }

    private:
        friend class TreeBuilder;
        Checkpoint(TreeBuilder* b, std::uint32_t id) noexcept;

        TreeBuilder*  builder_ = nullptr;
        std::uint32_t id_      = 0;        // index into `checkpointStack_`
        Disposition   disp_    = Disposition::Pending;
    };

    // ── construction ──
    TreeBuilder(std::shared_ptr<SourceBuffer>        src,
                std::shared_ptr<GrammarSchema const> schema,
                DiagnosticReporter::Config           diagConfig    = {},
                BuilderConfig                        builderConfig = {});

    // Process-wide monotonic allocator. Public so test helpers that
    // fabricate trees outside `TreeBuilder` (e.g. `RawTreeBuilder`) can
    // share the same counter and never collide.
    [[nodiscard]] static TreeId nextTreeId() noexcept;

    // TreeId minted at construction. Stamped onto every NodeId this
    // builder emits so cross-tree usage of those ids aborts loudly.
    [[nodiscard]] TreeId treeId() const noexcept { return treeId_; }

    // Single-use: copy/move would leave dangling OpenScope owners.
    TreeBuilder(TreeBuilder const&)            = delete;
    TreeBuilder& operator=(TreeBuilder const&) = delete;
    TreeBuilder(TreeBuilder&&)                 = delete;
    TreeBuilder& operator=(TreeBuilder&&)      = delete;

    // ── shape construction ──
    //
    // Returns a move-only RAII guard. Forgetting to close is impossible —
    // the destructor handles it. Closing out-of-order (LIFO violation)
    // emits P_BuilderInvariant and forces a cascade close of every frame
    // above the offending one.
    //
    // The `&`-qualifier disqualifies rvalue builders: writing
    // `auto s = TreeBuilder{src, sch}.open(R);` would leave `s` holding a
    // raw back-pointer into a temporary, so we refuse it at compile time.
    [[nodiscard]] OpenScope open(RuleId rule) &;

    // Resolve + attach a token leaf to the current frame. EmptySpace tokens
    // are flagged via NodeFlags::EmptySpace; opensScope/closesScope tokens
    // mutate the scope stack here. With no open frame, emits
    // P_BuilderInvariant and drops the token.
    void pushToken(Token const& tok);

    // Explicit error production (the parser knows it's wrong without trying
    // a pushToken). Inserts an Error leaf at `span`, emits P_UnexpectedToken
    // with the expected/scope information, and propagates HasError up to root.
    void pushError(SourceSpan                   span,
                   std::optional<RuleId>        expectedRule  = std::nullopt,
                   std::optional<SchemaTokenId> expectedToken = std::nullopt,
                   std::string_view             note          = {});

    // Insert an Error leaf at `span` WITHOUT emitting a diagnostic — the
    // caller has already emitted a richer one through `reportDiagnostic`
    // (e.g. a panic-mode P_UnexpectedToken carrying the full
    // `expectedSet` rendered by the parser). Propagates HasError to root.
    void pushErrorNode(SourceSpan span);

    // Public gateway for diagnostics the parser layer needs to emit that
    // don't fit `pushError`'s "unexpected token + Error leaf" shape —
    // notably `P_MissingRequiredChild`, `P_NoAlternativeMatched`, and
    // `P_BacktrackFailed`. The diagnostic flows through the same
    // reporter that handles every other tree-side diagnostic, so the
    // dedup, ordering, and severity rules apply consistently. The
    // builder stamps the active scope stack onto the diagnostic; the
    // caller fills `code`, `severity`, `buffer`, `span`, `expected`,
    // `actual`, `ruleContext`, and `related`.
    void reportDiagnostic(ParseDiagnostic d);

    // ── scope stack (validated against schema in pushToken) ──
    void pushScope(ScopeKind kind);
    void popScope();    // emits P_BuilderInvariant on underflow
    [[nodiscard]] ScopeKind                   currentScope() const noexcept;
    [[nodiscard]] std::span<ScopeKind const>  scopeStack()   const noexcept;

    // ── frame introspection (used by parsers driving recovery) ──
    // Returns InvalidRule if no frame is currently open. Reading from a
    // finished builder is allowed but returns InvalidRule (the stack is
    // empty post-finish).
    [[nodiscard]] RuleId       currentRule()  const noexcept;

    // Snapshot full builder state (arena, child-index, pending children,
    // open frames, scope stack, cursor + stack, cookie counter, closed-
    // cookie set, desync latch, watchdog latch, reporter accumulator).
    // Closing an outer frame inside speculation emits `P_BuilderInvariant`.
    // Over-cap produces a no-op guard (`id == kNoOpCheckpointId`); commit/
    // rollback on it are safe no-ops.
    [[nodiscard]] Checkpoint checkpoint();
    void                     commit(Checkpoint&& cp) noexcept;
    void                     rollback(Checkpoint&& cp) noexcept;

    // ── finalize ──
    // Single-use: consumes the builder. Any frames still open are closed
    // synthetically and emit one P_PrematureEndOfInput per unclosed frame
    // (related-linked to each opener). The returned Tree contains every
    // diagnostic collected during the build, accessible via tree.diagnostics().
    Tree finish() &&;

    // Read-only introspection (mostly for tests).
    [[nodiscard]] std::size_t openFrameCount() const noexcept;
    [[nodiscard]] bool        hasFinished()    const noexcept { return finished_; }

private:
    // ── per-open-frame state ──
    //
    // Children collected while a frame is open live in the shared
    // `pendingChildren_` vector, at the contiguous range
    // `[pendingStart, pendingChildren_.size())`. On close, that range
    // is flushed to `childIndex_` in order and the staging vector is
    // truncated back to `pendingStart` (the parent frame's region is
    // re-exposed at the top). Storing children as offsets into one
    // vector keeps the per-frame footprint small and makes speculative
    // rollback an integer truncation rather than N per-frame resizes.
    struct Frame {
        NodeId               id;           // the Internal node being built
        RuleId               rule;
        SourceSpan           openerSpan;   // first source position seen at open() — used as opener for Missing diags
        std::uint32_t        pendingStart; // index into pendingChildren_ where this frame's children begin
        std::uint32_t        cookie;       // matches OpenScope::cookie_
    };

    // ── speculative checkpoint state ──
    //
    // One CheckpointSnapshot per outstanding `checkpoint()` call. The
    // Checkpoint guard carries an index into `checkpointStack_`; commit
    // and rollback pop entries above that index (cascade-cleanup if a
    // caller forgot to commit/rollback an inner checkpoint before an
    // outer one). Snapshots are O(open_frames + cursorStack.size()
    // + closedCookies_.size()) — small, but not zero, so unlimited
    // depth is gated by BuilderConfig::maxSpeculationDepth.
    struct CheckpointSnapshot {
        std::size_t                       nodesSize;
        std::size_t                       childIndexSize;
        std::size_t                       pendingChildrenSize;
        std::vector<Frame>                openFrames;          // snapshot copy
        std::vector<ScopeKind>            scopes;
        // SchemaWalker::Snapshot is non-default-constructible by
        // design (every instance must originate from `snapshot()`).
        // Wrap in std::optional so CheckpointSnapshot can be built
        // field-by-field; populated immediately by `checkpoint()`.
        std::optional<SchemaWalker::Snapshot> walker;
        std::uint32_t                     nextCookie;
        std::unordered_set<std::uint32_t> closedCookies;
        bool                              maxSpeculationDepthReached;
        DiagnosticReporter::Snapshot      reporterSnap;
    };

    // ── helpers ──
    [[nodiscard]] NodeId    emit_(detail::Node n);
    void                    closeFrame_(std::uint32_t cookie, bool synthetic) noexcept;
    void                    propagateHasError_(NodeId start) noexcept;
    void                    emitDiagnostic_(ParseDiagnostic d);
    // Like emitDiagnostic_ but bypasses the reporter's cap so signals
    // that callers MUST see (e.g. P_UncommittedCheckpoint from the
    // Checkpoint dtor) aren't silently swallowed by an at-cap reporter.
    void                    forceReport_(ParseDiagnostic d);
    void                    addBuilderInvariant_(std::string actual, SourceSpan span);

    // Attach a node id to the current frame's children list. Returns false
    // when there's no open frame (caller must have already emitted a
    // P_BuilderInvariant for that case).
    bool                    attachToCurrentFrame_(NodeId id);

    // View of `pendingChildren_` covering the top open frame's region.
    // Empty when no frame is open. Read-only.
    [[nodiscard]] std::span<NodeId const> topFramePendingChildren_() const noexcept;

    // Restore builder state from the snapshot at `checkpointStack_[id-1]`
    // (id is 1-based; id=0 is the no-op marker). Drops the snapshot and
    // any deeper ones. Used by both rollback() and the Checkpoint
    // destructor's defensive fallback. No-op for the no-op marker id=0.
    void                    rollbackToId_(std::uint32_t id) noexcept;

    // Drop the snapshot at the given id and any deeper ones, WITHOUT
    // restoring state. Used by commit(). No-op for the no-op marker.
    void                    commitToId_(std::uint32_t id) noexcept;

    // ── state ──
    std::shared_ptr<SourceBuffer>          source_;
    std::shared_ptr<GrammarSchema const>   schema_;
    std::unique_ptr<DiagnosticReporter>    reporter_;
    TreeId                                 treeId_;

    std::vector<detail::Node>              nodes_;       // arena under construction
    std::vector<NodeId>                    childIndex_;  // flat children table under construction

    // Staging vector for children of open frames. Each open Frame owns the
    // range `[pendingStart, pendingChildren_.size())`. On close, the
    // range is flushed to `childIndex_` and the staging vector truncates
    // back to the parent's range. `attachToCurrentFrame_` appends here.
    std::vector<NodeId>                    pendingChildren_;

    std::vector<Frame>                     open_;        // LIFO open-frame stack
    std::vector<ScopeKind>                 scopes_;      // current scope stack

    // Schema-cursor state machine mirroring `open_`. Walked through
    // enterRule on open(), leaveRule on close, and advance on
    // pushToken. Invariant: `walker_.depth() == open_.size()` before
    // and after every open/close operation. The walker owns the
    // one-shot P_SchemaCursorDesync latch + emits via the callback
    // wired in our ctor (so the parser, when it embeds its own
    // walker, gets the same latched behavior).
    SchemaWalker                           walker_;

    // Body-mode `defaultToken.kind` set — consulted on the per-token
    // resolveMeaning path to skip the schema cursor advance for
    // off-grammar body tokens. Sourced from `schema_->bodyDefaultTokenKinds()`
    // (single source of truth shared with the parser); we hold a pointer
    // so the per-token hot path doesn't re-fetch the reference.
    //
    // Flat union across all modes (no per-mode awareness): if mode A's
    // default is `Foo` and mode B's default is also `Foo`, both skip
    // correctly; if mode A's `Foo` collides with a Foo declared
    // outside its body the cursor-skip still fires. Defense-in-depth
    // for the latter lives at the loader (`C_BodyDefaultKindInShape`)
    // which rejects shapes / scope-forbid entries naming a body-default
    // kind.
    std::unordered_set<SchemaTokenId> const* bodyDefaultTokenKinds_ = nullptr;

    // Schema's "Error" + "Identifier" SchemaTokenIds, cached at ctor so
    // the per-token resolveMeaning + contextual-keyword paths don't
    // re-walk the interner. Predeclared by the loader (see
    // kBuiltinTokenKindNames); the ctor fatal-aborts if either is
    // missing rather than letting silent drift (e.g. an Error-kind
    // token would synthesize into a clean leaf instead of triggering
    // recovery).
    SchemaTokenId                          errorKind_{};
    SchemaTokenId                          identifierKind_{};

    // Cookies that have been "closed" by cascade or by finish() but whose
    // OpenScope guards are still alive (and will eventually call close()
    // when destroyed). A subsequent close() for these is a clean no-op
    // rather than a spurious P_BuilderInvariant. Bounded by the number of
    // cascade events and synthetic closes; expected to stay small.
    std::unordered_set<std::uint32_t>      closedCookies_;

    std::uint32_t                          nextCookie_ = 1;   // 0 reserved as "invalid"
    bool                                   finished_   = false;

    // ── speculative state ──
    //
    // Each entry carries its own id. Lookup by id is a linear search
    // (depth ≤ maxSpeculationDepth, typically ≤ 64 — trivially fast and
    // cache-friendly). Index-arithmetic shortcuts (e.g. `firstId =
    // nextCheckpointId_ - stack.size()`) silently break when `commitToId_`
    // truncates from the middle of the stack on inner-commit-then-outer-
    // rollback sequences, so the stable invariant is "each snapshot owns
    // its id." `kNoOpCheckpointId` is the sentinel returned by
    // `checkpoint()` when the cap is reached; both commit/rollback paths
    // short-circuit on it.
    static constexpr std::uint32_t kNoOpCheckpointId = 0;

    BuilderConfig                          builderConfig_{};
    std::vector<std::pair<std::uint32_t,
                          CheckpointSnapshot>>  checkpointStack_;
    std::uint32_t                          nextCheckpointId_ = 1;
    // One-shot per build; snapshotted by Checkpoint so rollback restores
    // it (otherwise a speculative branch that tripped the cap would
    // permanently silence the post-rollback emission of legitimate cap
    // events).
    bool                                   maxSpeculationDepthReached_ = false;
};

// TreeBuilder's ctor wires a `[this]`-capturing lambda into the
// embedded `SchemaWalker walker_` as the desync emission callback.
// If TreeBuilder becomes movable, the destination's `walker_` (moved
// from the source) keeps a lambda whose `this` still points at the
// source — silently dangling once the source is destructed. Keep it
// non-movable until the capture is restructured (e.g. take a `this`
// pointer at every callback invocation rather than at construction).
static_assert(!std::is_move_constructible_v<TreeBuilder>,
              "TreeBuilder must stay non-movable while walker_'s "
              "desync callback captures `this` by reference");
static_assert(!std::is_copy_constructible_v<TreeBuilder>,
              "TreeBuilder must stay non-copyable — single-use by design");

} // namespace dss
