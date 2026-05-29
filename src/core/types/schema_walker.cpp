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

SchemaWalker::SchemaWalker(SchemaWalker&&) noexcept            = default;
SchemaWalker& SchemaWalker::operator=(SchemaWalker&&) noexcept = default;

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
    cursorStack_.push_back(savedParent);
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
    wrapFrameFlags_.push_back(isWrap);
    if (isWrap) ++wrapDepth_;
    cursor_ = schema_->enterRule(rule);
    // enterRule on a registered rule always returns a valid cursor.
    // No valid→invalid transition possible here, so no desync check.
}

void SchemaWalker::leaveRule(SourceSpan span,
                             std::optional<RuleId> rule) noexcept {
    if (cursorStack_.empty()) {
        fatal("dss::SchemaWalker::leaveRule: parent stack underflow — "
              "consumer's frame guard must validate balance before "
              "calling the walker (TreeBuilder uses P_BuilderInvariant)");
    }
    SchemaCursor savedParent = cursorStack_.back();
    cursorStack_.pop_back();
    // The wrap-flag pop / depth decrement is INTENTIONALLY deferred
    // until AFTER `noteDesync_` on every exit path. A leave-time
    // valid→invalid cursor transition that occurs while leaving a
    // wrap frame is structurally caused BY the wrap (the schema's
    // routeToRuleLeaf doesn't model wrappers, so leaving one back to
    // the parent's "after wrap" position has no valid graph edge).
    // Keeping the wrap flag in `wrapFrameFlags_` AND `wrapDepth_`
    // elevated through `noteDesync_` lets the suppression check see
    // the wrap responsible for the leave-time transition. The
    // decrement runs at every exit (early-return + main path) so the
    // depth stays balanced with the `enterRule` increment.
    bool const leavingWrap =
        !wrapFrameFlags_.empty() && wrapFrameFlags_.back();
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
        // load-bearing one. Pop the wrap state for balance.
        cursor_ = SchemaCursor{};
        if (!wrapFrameFlags_.empty()) wrapFrameFlags_.pop_back();
        if (leavingWrap) --wrapDepth_;
        return;
    }
    const bool wasValid = savedParent.valid();
    cursor_ = schema_->leaveRule(savedParent);
    noteDesync_(wasValid, cursor_.valid(), span, rule);
    if (!wrapFrameFlags_.empty()) wrapFrameFlags_.pop_back();
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

SchemaWalker::Snapshot::Snapshot(GrammarSchema const*      schemaPtr,
                                 SchemaCursor              cursor,
                                 std::vector<SchemaCursor> cursorStack,
                                 std::vector<bool>         wrapFrameFlags,
                                 std::uint32_t             wrapDepth,
                                 bool                      cursorDesynced) noexcept
    : schemaPtr_(schemaPtr)
    , cursor_(cursor)
    , cursorStack_(std::move(cursorStack))
    , wrapFrameFlags_(std::move(wrapFrameFlags))
    , wrapDepth_(wrapDepth)
    , cursorDesynced_(cursorDesynced) {}

SchemaWalker::Snapshot SchemaWalker::snapshot() const {
    return Snapshot{schema_.get(), cursor_, cursorStack_, wrapFrameFlags_,
                    wrapDepth_, cursorDesynced_};
}

void SchemaWalker::restore(Snapshot snap) {
    if (snap.schemaPtr_ != schema_.get()) {
        fatal("dss::SchemaWalker::restore: snapshot was produced by a "
              "different walker (schema pointer mismatch) — restoring "
              "would index the wrong schema's position table");
    }
    // Invariant guard: `wrapFrameFlags_` mirrors `cursorStack_` 1:1
    // at all times. A snapshot with mismatched lengths is impossible
    // through the private ctor, but make the contract observable —
    // matches the fail-loud discipline applied to every other walker
    // invariant (`enterRule(InvalidRule)`, `leaveRule` underflow,
    // cross-walker restore).
    if (snap.wrapFrameFlags_.size() != snap.cursorStack_.size()) {
        fatal("dss::SchemaWalker::restore: snapshot wrapFrameFlags / "
              "cursorStack size mismatch — invariant violated");
    }
    cursor_          = snap.cursor_;
    cursorStack_     = std::move(snap.cursorStack_);
    wrapFrameFlags_  = std::move(snap.wrapFrameFlags_);
    wrapDepth_       = snap.wrapDepth_;
    cursorDesynced_  = snap.cursorDesynced_;
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
