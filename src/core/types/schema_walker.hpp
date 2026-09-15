#pragma once

#include "core/export.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/schema_cursor.hpp"
#include "core/types/source_span.hpp"
#include "core/types/speculation_trail.hpp"
#include "core/types/strong_ids.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace dss {

// Shared state machine for schema-cursor navigation. Embedded by
// TreeBuilder and (later) the parser so both drive identical dispatch
// — drift between them surfaces as `P_SchemaCursorDesync` or
// mis-shaped diagnostics in E2E.
//
// Owns:
//   - current cursor + saved parent-cursor stack (the per-rule
//     position graph state machine)
//   - one-shot desync latch (so the P_SchemaCursorDesync diagnostic
//     fires at most once per walker instance)
//
// Does NOT own:
//   - frame stack (consumer's responsibility — the cursor stack
//     mirrors the consumer's frames 1:1, but the walker treats them
//     as opaque)
//   - diagnostic emission (consumer registers a callback that fires
//     exactly once on the first valid→invalid cursor transition)
//
// Movable, not copyable. A walker is single-instance per consumer;
// duplication would invalidate the lock-step invariant.
class DSS_EXPORT SchemaWalker {
private:
    // One entry per open frame: the parent cursor to resume on, plus
    // whether THIS frame is a Pratt auto-interned wrapper rule.
    //
    // ★ The wrap flag used to live in a SECOND vector kept parallel to the
    // cursor stack, with a fail-loud check on `restore` that the two lengths
    // agreed. Folding it into the frame makes that invariant
    // unrepresentable rather than merely checked — one push, one pop, no
    // way to desynchronize — and halves the number of undo records a
    // speculative frame close writes.
    // ★★ `wrapsAtAndBelow` IS WHAT MAKES THE SECOND INVARIANT CHECKABLE IN
    // O(1). Folding the flag into the frame made the LENGTH invariant
    // unrepresentable but created a new one in its place: `wrapDepth_` is a
    // scalar carried in the `Snapshot` while `frames_` is rewound from the
    // undo journal, and after a rewind NOTHING related the two. Recomputing
    // `count(isWrap)` would be O(depth) per restore — reintroducing, in the
    // guard, the exact quadratic this design removed — so each frame carries
    // the running count of wrap frames at and below it. The top frame's
    // value IS `count(isWrap)`, so the check is one comparison and
    // `snapshot()` / `restore()` both make it.
    struct WalkerFrame {
        SchemaCursor  parent{};
        bool          isWrap = false;
        std::uint32_t wrapsAtAndBelow = 0;
    };
    using FrameStack = TrailedStack<WalkerFrame>;

public:
    // Fires exactly once per walker instance — on the first
    // valid→invalid cursor transition. Consumer turns this into a
    // diagnostic (TreeBuilder emits P_SchemaCursorDesync info-severity).
    //
    // Contract: the callback MUST NOT throw. The walker invokes it
    // from `noexcept` sites; an escaping exception is a contract
    // violation and the walker fatal-aborts — matching the
    // discipline applied to other walker-contract violations
    // (`enterRule(InvalidRule)`, `leaveRule` underflow, cross-walker
    // `restore`). Production callers (TreeBuilder) wire a pure
    // diagnostic emission that cannot throw.
    using DesyncCallback =
        std::function<void(SourceSpan span, std::optional<RuleId> rule)>;

    explicit SchemaWalker(std::shared_ptr<GrammarSchema const> schema,
                          DesyncCallback onDesync = {}) noexcept;

    SchemaWalker(SchemaWalker const&)            = delete;
    SchemaWalker& operator=(SchemaWalker const&) = delete;
    // Moving a walker that still has a live `Snapshot` outstanding would
    // leave that Snapshot's back-pointer aimed at the moved-FROM husk, so
    // both moves fatal-abort while the live-mark count is non-zero. Same
    // discipline as every other walker-contract violation here.
    SchemaWalker(SchemaWalker&&) noexcept;
    SchemaWalker& operator=(SchemaWalker&&) noexcept;
    // Fatal-aborts if a `Snapshot` is still outstanding: a Snapshot that
    // outlives its walker would touch freed memory from its own destructor,
    // and that is precisely the case this counter can see and a comment
    // cannot prevent.
    ~SchemaWalker() noexcept;

    // ── navigation ──
    //
    // Routes the saved parent through `routeToRuleLeaf` so a later
    // `leaveRule` resumes on a `RuleLeaf` slot rather than the
    // `AltChoice` we entered from. **Precondition**: `rule.valid()`
    // — entering an unregistered rule yields downstream silent-state
    // corruption, so the walker fatal-aborts on this contract
    // violation per the project's fail-loud discipline.
    void enterRule(RuleId rule);

    // Pop the saved parent cursor and resume by calling
    // GrammarSchema::leaveRule on it. **Precondition**: parent stack
    // non-empty — underflow is a consumer-side frame-balance bug; the
    // walker fatal-aborts rather than silently masking it.
    //
    // Stays `noexcept` although the pop now appends an undo record while
    // a speculative mark is live, and appending can allocate. That is
    // deliberate: an allocation failure here terminates, and terminating
    // is the correct outcome — the alternative, an unjournaled pop,
    // would let a later rollback restore a frame stack with the wrong
    // contents, which is exactly the silent corruption the fail-loud rule
    // forbids. The failure path is `[[noreturn]]`; the desync callback
    // dispatch and the schema's methods are `noexcept` as before.
    void leaveRule(SourceSpan span, std::optional<RuleId> rule) noexcept;

    // Advance the current cursor by `tok`. Returns whether the cursor
    // is still valid AFTER the advance; consumers may use this to
    // skip expected-set lookups on the slow path. `span` + `rule`
    // populate the desync diagnostic if one fires. `noexcept` for the
    // same reason as `leaveRule`.
    bool advance(SchemaTokenId tok, SourceSpan span,
                 std::optional<RuleId> rule) noexcept;

    // ── introspection ──
    [[nodiscard]] SchemaCursor                  cursor()         const noexcept { return cursor_; }
    [[nodiscard]] SlotKind                      slotKind()       const noexcept;
    [[nodiscard]] std::span<SchemaTokenId const> expectedSet()   const noexcept;
    [[nodiscard]] bool                          isSpeculativeAlt() const noexcept;
    [[nodiscard]] std::uint16_t                 lookahead()      const noexcept;
    [[nodiscard]] bool                          isAtEndOfRule()  const noexcept;
    [[nodiscard]] bool                          canEndSource()   const noexcept;
    [[nodiscard]] bool                          nullableTail()   const noexcept;
    [[nodiscard]] RuleId                        slotRuleRef()    const noexcept;

    // Step past a nullable AltChoice without consuming a token. The
    // parser calls this when an optional/repeat AltChoice doesn't
    // match the next token but the cursor's `nullableTail` is true —
    // taking the AltChoice's nullable branch lets the cursor advance
    // to whatever follows the optional. No-op if the cursor isn't at
    // a nullable AltChoice (the parser should emit
    // `P_NoAlternativeMatched` and consume in that case).
    bool takeNullableBranch() noexcept;
    [[nodiscard]] std::size_t                   depth()          const noexcept { return frames_.size(); }
    [[nodiscard]] bool                          isDesynced()     const noexcept { return cursorDesynced_; }

    // ── speculation (TreeBuilder::Checkpoint and the parser's
    //                 SpeculationProbe) ──
    //
    // Opaque O(1) restore token for the walker's full state.
    //
    // ★★ IT IS A MARK, NOT A COPY, AND THAT IS THE POINT. This used to
    // carry a COPY of the cursor stack, which made one speculative probe
    // cost O(depth) bytes and D nested probes cost Θ(D²) — the measured
    // wall behind the shipped speculation ceiling. It now carries the
    // frame stack's trail mark plus the three scalars that are not
    // reconstructible from it, and the stack itself is rewound through
    // its journal (see `core/types/speculation_trail.hpp`).
    //
    // Consequences of being a mark, all of them deliberate:
    //   - A Snapshot belongs to ONE walker. Restoring it into a different
    //     walker fatal-aborts. The previous VALUE shape allowed a
    //     cross-walker restore over the same schema, which was never a
    //     production path and was unsound on its face: the cursor stack
    //     mirrors the CONSUMER's frames 1:1, so installing another
    //     walker's stack desynchronizes the consumer that owns it.
    //   - A Snapshot must not outlive its walker. The walker's destructor
    //     fatal-aborts while any Snapshot is outstanding, so that mistake
    //     is loud rather than a use-after-free.
    //   - Marks retire in LIFO order, which is what both consumers do; a
    //     stale mark is refused by the trail rather than silently
    //     resizing a stack to a length it never had.
    //
    // Still includes the one-shot desync latch, so a speculative branch
    // that tripped desync can be rolled back and the post-rollback
    // emission of legitimate desync events still fires.
    //
    // Default construction is deleted so a stray uninitialized
    // Snapshot cannot reach `restore()` — every Snapshot must
    // originate from a live `snapshot()` call.
    class DSS_EXPORT Snapshot {
    public:
        Snapshot()                           = delete;
        Snapshot(Snapshot const&)            = delete;
        Snapshot& operator=(Snapshot const&) = delete;
        Snapshot(Snapshot&&) noexcept;
        Snapshot& operator=(Snapshot&&) noexcept;
        // Retires this mark with the owning walker. When the last
        // outstanding mark retires, the walker drops its undo journal —
        // which is the ONLY thing that keeps the journal from growing for
        // the life of the parse, and the reason this is RAII rather than
        // an explicit `release()` a caller could forget on the commit
        // path.
        ~Snapshot() noexcept;

    private:
        friend class SchemaWalker;
        // `owner_` is a raw observer pointer used for identity comparison
        // in `restore()` and for the mark retirement in `~Snapshot`; it is
        // nulled when this Snapshot is moved from. `schemaPtr_` is
        // likewise never dereferenced — it exists so a cross-SCHEMA
        // restore is refused with the message that names that specific
        // confusion, before the coarser cross-walker check fires.
        Snapshot(SchemaWalker*        owner,
                 GrammarSchema const* schemaPtr,
                 FrameStack::Mark     mark,
                 SchemaCursor         cursor,
                 std::uint32_t        wrapDepth,
                 bool                 cursorDesynced) noexcept;

        SchemaWalker*        owner_;
        GrammarSchema const* schemaPtr_;
        FrameStack::Mark     mark_;
        SchemaCursor         cursor_;
        std::uint32_t        wrapDepth_;
        bool                 cursorDesynced_;
    };

    // Non-const: taking a mark ARMS the frame stack's undo journal and
    // registers an outstanding mark with this walker.
    [[nodiscard]] Snapshot snapshot();
    void                   restore(Snapshot snap);

private:
    // Latch: emit on the first valid→invalid edge only; snapshot/
    // restore re-arms via the stored `cursorDesynced` field, so a
    // speculative branch that tripped desync rolls back and the post-
    // rollback walk can fire legitimate desync events again.
    void noteDesync_(bool wasValid, bool nowValid,
                     SourceSpan span, std::optional<RuleId> rule) noexcept;

    // Retire one outstanding mark. Drops the frame stack's undo journal
    // when the count reaches zero — the walker is the only party that
    // knows no rewind can still be asked for.
    void releaseMark_() noexcept;

    // `count(isWrap)` over the whole frame stack, in O(1) — the top frame
    // carries the running total. Empty stack means zero.
    [[nodiscard]] std::uint32_t stackWrapDepth_() const noexcept {
        return frames_.empty() ? 0u : frames_.back().wrapsAtAndBelow;
    }

    // Fatal-abort unless `wrapDepth_` agrees with the frame stack. The two
    // are restored from DIFFERENT places — a scalar in the `Snapshot` and
    // the frame stack's undo journal — so nothing but this relates them
    // across a rewind, and a `wrapDepth_` that came back too low would
    // silently re-arm the desync latch inside a wrapper rule and turn
    // structural cursor noise into a fabricated `P_SchemaCursorDesync`.
    // Called at `snapshot()` and after the rewind in `restore()`, the two
    // points where the pairing is established and re-established. NOT called
    // inside `leaveRule`: the wrap-depth decrement there is DELIBERATELY
    // deferred past `noteDesync_`, so between the pop and the decrement the
    // two legitimately disagree by one.
    void assertWrapDepthMatchesFrames_(char const* where) const noexcept;

    std::shared_ptr<GrammarSchema const> schema_;
    DesyncCallback                       onDesync_;
    SchemaCursor                         cursor_{};
    // The saved parent-cursor stack, one entry per open frame, carrying
    // the per-frame "this frame is a Pratt auto-interned wrapper rule"
    // flag alongside the cursor. Pushed by `enterRule` (the flag true iff
    // `schema_->isAutoInternedWrapperRule(rule)`); popped by `leaveRule`
    // AFTER `noteDesync_` so a leave-time valid→invalid transition still
    // sees the wrap frame responsible. `wrapDepth_` counts the trues for
    // O(1) `noteDesync_` suppression — wrapper rules have no body in the
    // schema's position graph, so cursor advances within them (including
    // in follower rules opened under the wrap whose cursor inherits the
    // wrap's invalid-graph context) are structural-only noise rather than
    // a real grammar mismatch. The latch is suppressed whenever ANY
    // ancestor frame on the stack is a wrap (`wrapDepth_ > 0`). Plan 05
    // post-close sub-cycle B. The per-frame FLAG (rather than just the
    // counter) is needed because `leaveRule`'s decrement must know
    // whether the SPECIFIC frame being left was a wrap, and overloading
    // the API's `std::optional<RuleId> rule` parameter (today a diag
    // hint) with semantic meaning would be a fragile contract change.
    FrameStack                           frames_;
    std::uint32_t                        wrapDepth_ = 0;
    bool                                 cursorDesynced_ = false;
    // Outstanding `Snapshot`s. Non-zero forbids moving the walker (their
    // back-pointers would dangle) and forbids destroying it; zero is what
    // lets `frames_` drop its undo journal.
    std::size_t                          liveMarks_ = 0;
};

} // namespace dss
