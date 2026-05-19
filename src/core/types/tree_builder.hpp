#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
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
#include <unordered_set>
#include <vector>

namespace dss {

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

    // ── construction ──
    TreeBuilder(std::shared_ptr<SourceBuffer>        src,
                std::shared_ptr<GrammarSchema const> schema,
                DiagnosticReporter::Config           diagConfig = {});

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
    struct Frame {
        NodeId               id;           // the Internal node being built
        RuleId               rule;
        SourceSpan           openerSpan;   // first source position seen at open() — used as opener for Missing diags
        std::vector<NodeId>  children;     // collected while open; flushed to childIndex on close
        std::uint32_t        cookie;       // matches OpenScope::cookie_
    };

    // ── helpers ──
    [[nodiscard]] NodeId    emit_(detail::Node n);
    void                    closeFrame_(std::uint32_t cookie, bool synthetic) noexcept;
    void                    propagateHasError_(NodeId start) noexcept;
    void                    emitDiagnostic_(ParseDiagnostic d);
    void                    addBuilderInvariant_(std::string actual, SourceSpan span);

    // Emit P_SchemaCursorDesync exactly once per build the first time the
    // schema cursor goes from valid to invalid. `wasValid` is the state
    // before the most recent walk step (advance or leaveRule); `nowValid`
    // is the result. `span` and `rule` populate the diagnostic location.
    void                    noteCursorDesync_(bool wasValid,
                                              bool nowValid,
                                              SourceSpan span,
                                              std::optional<RuleId> rule);

    // Attach a node id to the current frame's children list. Returns false
    // when there's no open frame (caller must have already emitted a
    // P_BuilderInvariant for that case).
    bool                    attachToCurrentFrame_(NodeId id);

    // ── state ──
    std::shared_ptr<SourceBuffer>          source_;
    std::shared_ptr<GrammarSchema const>   schema_;
    std::unique_ptr<DiagnosticReporter>    reporter_;

    std::vector<detail::Node>              nodes_;       // arena under construction
    std::vector<NodeId>                    childIndex_;  // flat children table under construction

    std::vector<Frame>                     open_;        // LIFO open-frame stack
    std::vector<ScopeKind>                 scopes_;      // current scope stack

    // Schema cursor mirroring `open_`. Walked through enterRule on open(),
    // leaveRule on close, and advance on pushToken. Invariant:
    // `cursorStack_.size() == open_.size()` before/after every open/close
    // operation. Goes invalid when the caller drives the builder against
    // the schema's shape; the contextual demotion path treats an invalid
    // cursor as "no expectations known; keep the keyword."
    SchemaCursor                           cursor_;
    std::vector<SchemaCursor>              cursorStack_;
    // One-shot — true after the first valid → invalid cursor transition.
    // Bounds the P_SchemaCursorDesync diagnostic to one emission per
    // build so a long parse that goes off-track doesn't flood the
    // diagnostic stream.
    bool                                   cursorDesynced_ = false;

    // Cookies that have been "closed" by cascade or by finish() but whose
    // OpenScope guards are still alive (and will eventually call close()
    // when destroyed). A subsequent close() for these is a clean no-op
    // rather than a spurious P_BuilderInvariant. Bounded by the number of
    // cascade events and synthetic closes; expected to stay small.
    std::unordered_set<std::uint32_t>      closedCookies_;

    std::uint32_t                          nextCookie_ = 1;   // 0 reserved as "invalid"
    bool                                   finished_   = false;
};

} // namespace dss
