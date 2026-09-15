#include "core/types/schema_walker.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace dss {

namespace {

[[noreturn]] void fatal(char const* what) noexcept {
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

SchemaWalker::SchemaWalker(std::shared_ptr<GrammarSchema const> schema,
                           DesyncCallback onDesync) noexcept
    : schema_(std::move(schema))
    , onDesync_(std::move(onDesync)) {}

SchemaWalker::SchemaWalker(SchemaWalker&& other) noexcept
    : schema_(std::move(other.schema_))
    , onDesync_(std::move(other.onDesync_))
    , cursor_(other.cursor_)
    , frames_(std::move(other.frames_))
    , wrapDepth_(other.wrapDepth_)
    , cursorDesynced_(other.cursorDesynced_) {
    if (other.liveMarks_ != 0) {
        fatal("dss::SchemaWalker: move-constructed while a Snapshot is "
              "still outstanding — the Snapshot's back-pointer would aim "
              "at the moved-from walker");
    }
}

SchemaWalker& SchemaWalker::operator=(SchemaWalker&& other) noexcept {
    if (this == &other) return *this;
    if (liveMarks_ != 0 || other.liveMarks_ != 0) {
        fatal("dss::SchemaWalker: move-assigned while a Snapshot is still "
              "outstanding — the Snapshot's back-pointer would aim at a "
              "walker whose state has been replaced");
    }
    schema_         = std::move(other.schema_);
    onDesync_       = std::move(other.onDesync_);
    cursor_         = other.cursor_;
    frames_         = std::move(other.frames_);
    wrapDepth_      = other.wrapDepth_;
    cursorDesynced_ = other.cursorDesynced_;
    return *this;
}

SchemaWalker::~SchemaWalker() noexcept {
    if (liveMarks_ != 0) {
        fatal("dss::SchemaWalker: destroyed while a Snapshot is still "
              "outstanding — that Snapshot's destructor would retire its "
              "mark against freed memory");
    }
}

void SchemaWalker::enterRule(RuleId rule) {
    if (!rule.valid()) {
        fatal("dss::SchemaWalker::enterRule: rule is InvalidRule — "
              "entering an unregistered rule corrupts the parent "
              "stack (next leaveRule pops an invalid parent and "
              "tail-spins silent invalid→invalid advances)");
    }
    SchemaCursor savedParent = cursor_;
    if (savedParent.valid()) {
        const auto routed = schema_->routeToRuleLeaf(savedParent, rule);
        if (routed.valid()) savedParent = routed;
    }
    // Per-frame wrap accounting (plan 05 sub-cycle B): the Pratt
    // walker's `wrapLastChildExprFrame` enters auto-interned wrapper
    // rules (binary / unary / postfix / ternary) for structural
    // reparenting. These wrappers have no body in the schema's
    // position graph — they exist only for tree shape — so the
    // subsequent operator-token `advance` lands on an invalid cursor
    // and would fire a false-positive `cursorDesynced_` latch trip.
    // Tracking per-frame "is this a wrap" via the parallel stack lets
    // `leaveRule` know whether to decrement, and `wrapDepth_` keeps
    // `noteDesync_`'s suppression check O(1).
    bool const isWrap = schema_->isAutoInternedWrapperRule(rule);
    // The running wrap count travels IN the frame so `wrapDepth_` can be
    // checked against the stack in O(1) after a speculative rewind — see
    // `assertWrapDepthMatchesFrames_`.
    frames_.push(WalkerFrame{.parent          = savedParent,
                             .isWrap          = isWrap,
                             .wrapsAtAndBelow = stackWrapDepth_()
                                                + (isWrap ? 1u : 0u)});
    if (isWrap) ++wrapDepth_;
    cursor_ = schema_->enterRule(rule);
    // enterRule on a registered rule always returns a valid cursor.
    // No valid→invalid transition possible here, so no desync check.
}

void SchemaWalker::leaveRule(SourceSpan span,
                             std::optional<RuleId> rule) noexcept {
    if (frames_.empty()) {
        fatal("dss::SchemaWalker::leaveRule: parent stack underflow — "
              "consumer's frame guard must validate balance before "
              "calling the walker (TreeBuilder uses P_BuilderInvariant)");
    }
    WalkerFrame const leaving = frames_.back();
    SchemaCursor savedParent  = leaving.parent;
    frames_.pop();
    // The wrap-depth decrement is INTENTIONALLY deferred until AFTER
    // `noteDesync_` on every exit path. A leave-time valid→invalid cursor
    // transition that occurs while leaving a wrap frame is structurally
    // caused BY the wrap (the schema's routeToRuleLeaf doesn't model
    // wrappers, so leaving one back to the parent's "after wrap" position
    // has no valid graph edge). Keeping `wrapDepth_` elevated through
    // `noteDesync_` lets the suppression check see the wrap responsible
    // for the leave-time transition. The decrement runs at every exit
    // (early-return + main path) so the depth stays balanced with the
    // `enterRule` increment.
    bool const leavingWrap = leaving.isWrap;
    if (!savedParent.valid()) {
        // The parent was pushed invalid via routeToRuleLeaf when no
        // route through AltChoice positions led to the entered rule
        // (legitimate when the schema doesn't model the descent path
        // — the desync from enterRule already fired). Reset cursor;
        // skip schema_->leaveRule on invalid input to avoid double-
        // emission. NOTE: deliberately NO `noteDesync_` call here —
        // Phase 6 review proposed adding one, but empirically the
        // early-return path is taken on legitimate paths where the
        // already-tripped latch is the correct signal AND a fresh
        // valid→invalid signal here would double-count + over-fire
        // the diagnostic. The latch is one-shot per walker for
        // exactly this reason: the FIRST desync signal is the
        // load-bearing one. The frame is already popped; only the wrap
        // depth is left to rebalance.
        cursor_ = SchemaCursor{};
        if (leavingWrap) --wrapDepth_;
        return;
    }
    const bool wasValid = savedParent.valid();
    cursor_ = schema_->leaveRule(savedParent);
    noteDesync_(wasValid, cursor_.valid(), span, rule);
    if (leavingWrap) --wrapDepth_;
}

bool SchemaWalker::advance(SchemaTokenId tok, SourceSpan span,
                           std::optional<RuleId> rule) noexcept {
    const bool wasValid = cursor_.valid();
    cursor_ = schema_->advance(cursor_, tok);
    noteDesync_(wasValid, cursor_.valid(), span, rule);
    return cursor_.valid();
}

SlotKind SchemaWalker::slotKind() const noexcept {
    return schema_->slotKind(cursor_);
}

std::span<SchemaTokenId const> SchemaWalker::expectedSet() const noexcept {
    return schema_->expectedSet(cursor_);
}

bool SchemaWalker::isSpeculativeAlt() const noexcept {
    return schema_->isSpeculativeAlt(cursor_);
}

std::uint16_t SchemaWalker::lookahead() const noexcept {
    return schema_->lookahead(cursor_);
}

bool SchemaWalker::isAtEndOfRule() const noexcept {
    return schema_->isAtEndOfRule(cursor_);
}

bool SchemaWalker::canEndSource() const noexcept {
    return schema_->canEndSource(cursor_);
}

bool SchemaWalker::nullableTail() const noexcept {
    return schema_->nullableTail(cursor_);
}

bool SchemaWalker::takeNullableBranch() noexcept {
    const auto next = schema_->nullableBranch(cursor_);
    if (!next.valid()) return false;
    // No `noteDesync_` call here: "skipping" via a nullable branch
    // is a deliberate cursor mutation that, by construction, lands
    // on a valid position (we rejected invalid above). It is not a
    // valid→invalid transition, so no desync diagnostic is owed.
    cursor_ = next;
    return true;
}

RuleId SchemaWalker::slotRuleRef() const noexcept {
    return schema_->slotRuleRef(cursor_);
}

SchemaWalker::Snapshot::Snapshot(SchemaWalker*        owner,
                                 GrammarSchema const* schemaPtr,
                                 FrameStack::Mark     mark,
                                 SchemaCursor         cursor,
                                 std::uint32_t        wrapDepth,
                                 bool                 cursorDesynced) noexcept
    : owner_(owner)
    , schemaPtr_(schemaPtr)
    , mark_(mark)
    , cursor_(cursor)
    , wrapDepth_(wrapDepth)
    , cursorDesynced_(cursorDesynced) {}

SchemaWalker::Snapshot::Snapshot(Snapshot&& other) noexcept
    : owner_(other.owner_)
    , schemaPtr_(other.schemaPtr_)
    , mark_(other.mark_)
    , cursor_(other.cursor_)
    , wrapDepth_(other.wrapDepth_)
    , cursorDesynced_(other.cursorDesynced_) {
    other.owner_ = nullptr;   // exactly one owner retires the mark
}

SchemaWalker::Snapshot&
SchemaWalker::Snapshot::operator=(Snapshot&& other) noexcept {
    if (this == &other) return *this;
    // Overwriting a live mark retires it first: the state it named is
    // unreachable from here on, and leaking the count would pin the undo
    // journal open for the rest of the parse.
    if (owner_ != nullptr) owner_->releaseMark_();
    owner_          = other.owner_;
    schemaPtr_      = other.schemaPtr_;
    mark_           = other.mark_;
    cursor_         = other.cursor_;
    wrapDepth_      = other.wrapDepth_;
    cursorDesynced_ = other.cursorDesynced_;
    other.owner_    = nullptr;
    return *this;
}

SchemaWalker::Snapshot::~Snapshot() noexcept {
    if (owner_ != nullptr) owner_->releaseMark_();
}

void SchemaWalker::assertWrapDepthMatchesFrames_(char const* where) const noexcept {
    if (wrapDepth_ == stackWrapDepth_()) return;
    std::fprintf(stderr,
                 "dss::SchemaWalker::%s: wrapDepth_ (%u) disagrees with the "
                 "frame stack's wrap count (%u) — the desync-latch "
                 "suppression would fire on the wrong frames\n",
                 where, wrapDepth_, stackWrapDepth_());
    std::abort();
}

SchemaWalker::Snapshot SchemaWalker::snapshot() {
    assertWrapDepthMatchesFrames_("snapshot");
    ++liveMarks_;
    return Snapshot{this, schema_.get(), frames_.mark(), cursor_,
                    wrapDepth_, cursorDesynced_};
}

void SchemaWalker::restore(Snapshot snap) {
    if (snap.schemaPtr_ != schema_.get()) {
        fatal("dss::SchemaWalker::restore: snapshot was produced by a "
              "different walker (schema pointer mismatch) — restoring "
              "would index the wrong schema's position table");
    }
    // A Snapshot is a MARK into THIS walker's undo journal, so restoring
    // one into a sibling walker over the same schema would rewind against
    // a journal that never recorded those frames. The old VALUE shape
    // permitted it; nothing in production ever did it, and it was unsound
    // on its face because the cursor stack mirrors the CONSUMER's frames
    // 1:1 — installing another consumer's stack desynchronizes ours.
    if (snap.owner_ != this) {
        fatal("dss::SchemaWalker::restore: snapshot belongs to a different "
              "walker instance — a snapshot marks a position in ITS OWN "
              "walker's undo journal and is not transferable");
    }
    frames_.rewindTo(snap.mark_);
    cursor_          = snap.cursor_;
    wrapDepth_       = snap.wrapDepth_;
    cursorDesynced_  = snap.cursorDesynced_;
    // ★ THE FRAME STACK CAME BACK FROM THE UNDO JOURNAL AND `wrapDepth_`
    // CAME BACK FROM A SCALAR IN THE SNAPSHOT — two restores, and until this
    // line nothing related them. The old rendition kept the wrap flags in a
    // vector parallel to the cursor stack and checked their LENGTHS agreed
    // here; folding the flag into the frame made that check meaningless and
    // silently retired it. This is the same guard aimed at what is actually
    // restored separately now, and it is O(1) because the frame carries the
    // running count.
    assertWrapDepthMatchesFrames_("restore");
    // `snap` is by value: its destructor retires the mark, which drops the
    // journal once this was the last one outstanding.
}

void SchemaWalker::releaseMark_() noexcept {
    if (liveMarks_ == 0) {
        fatal("dss::SchemaWalker: mark retired more times than it was "
              "taken — a Snapshot released its mark twice");
    }
    if (--liveMarks_ == 0) frames_.discardJournal();
}

void SchemaWalker::noteDesync_(bool wasValid, bool nowValid,
                               SourceSpan span,
                               std::optional<RuleId> rule) noexcept {
    if (cursorDesynced_) return;
    if (!(wasValid && !nowValid)) return;
    // Plan 05 sub-cycle B: suppress the latch while ANY ancestor
    // frame is a Pratt auto-interned wrapper rule. Wrapper rules
    // have NO positions in the schema's graph (the loader skips
    // them in `validateOperatorBodyRules`) — they exist only for
    // tree shape. Cursor advances inside a wrap's body — including
    // those that occur AFTER opening real-grammar follower rules
    // under the wrap (whose cursor traversal still inherits the
    // wrap's invalid-graph context) — are structural noise, not
    // real grammar mismatches. The latch's contract is "real
    // grammar mismatch ONLY"; wrap-induced cursor invalidation is
    // a false positive. The `wrapDepth_ > 0` test is O(1).
    if (wrapDepth_ > 0) return;
    cursorDesynced_ = true;
    if (!onDesync_) return;
    // Callback contract is no-throw (see header). A throwing callback
    // is a contract violation matched by the discipline applied to
    // other walker-contract violations (`enterRule(InvalidRule)`,
    // `leaveRule` underflow, cross-walker `restore`) — fatal-abort
    // rather than silent-swallow, even though the latch has already
    // flipped so the desync record persists.
    try {
        onDesync_(span, rule);
    } catch (...) {
        fatal("dss::SchemaWalker: desync callback threw — "
              "DesyncCallback contract requires no-throw");
    }
}

} // namespace dss
