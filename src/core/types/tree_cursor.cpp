#include "core/types/tree_cursor.hpp"

#include "core/types/tree.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ranges>
#include <span>
#include <string_view>

namespace dss {

TreeCursor::TreeCursor(Tree const& tree, NodeId start, CursorMode mode) noexcept
    : tree_(&tree), current_(start), mode_(mode) {}

bool TreeCursor::isVisible_(NodeId id) const noexcept {
    if (mode_ == CursorMode::Cst) return true;
    // AST mode: EmptySpace is the ONLY thing that gets skipped. Missing
    // and Synthetic nodes remain visible — IR generation and semantic
    // analysis need to see them (e.g. to report "you didn't write the `)`").
    return !isEmptySpace(tree_->flags(id));
}

// ── position / forwarders ────────────────────────────────────────────────

NodeKind TreeCursor::kind() const noexcept {
    // Invalid position has no kind. Return Internal as a sentinel — same
    // value detail::Node defaults to — rather than aborting via tree_->kind().
    if (!current_.valid()) return NodeKind::Internal;
    return tree_->kind(current_);
}

NodeFlags TreeCursor::flags() const noexcept {
    if (!current_.valid()) return NodeFlags::None;
    return tree_->flags(current_);
}

std::string_view TreeCursor::text() const noexcept {
    if (!current_.valid()) return {};
    return tree_->text(current_);
}

SourceSpan TreeCursor::span() const noexcept {
    if (!current_.valid()) return SourceSpan::empty(0);
    return tree_->span(current_);
}

RuleId TreeCursor::rule() const {
    // Discriminant-asserting forwarder — same contract as Tree::rule().
    return tree_->rule(current_);
}

SchemaTokenId TreeCursor::tokenKind() const {
    return tree_->tokenKind(current_);
}

// ── movement ─────────────────────────────────────────────────────────────

namespace {

// c97 (compile-time-performance arc, MEASURED #1 hotspot): locate `self`
// within its parent's children span in O(log K) instead of O(K).
//
// The builder attaches children strictly in arena-id creation order — every
// attach path (token push, `open()`'s immediate emit, `wrapLastChildInFrame`
// replacing the tail with a NEWER id, `finish()`'s synthetic Missing
// appends) appends a fresh monotonic NodeId — so a parent's `children()`
// span is ASCENDING by construction. The former linear self-search made one
// full sibling walk O(K²): the sqlite amalgamation's ~10⁵-child root put
// 291/294 profile samples inside this scan (15m19s of a 15m28s compile,
// inside the import resolver's pre-order walk).
//
// Sortedness is an OPTIMIZATION assumption, never a correctness input: the
// binary-search result is VERIFIED (`kids[pos] == self`) and any miss —
// e.g. a future tree producer that splices out-of-order — falls back to the
// legacy linear scan, byte-identical behavior. Returns kids.size() when
// `self` is not among `kids` (the legacy "foundSelf never fired" outcome).
[[nodiscard]] std::size_t siblingIndexOf(std::span<NodeId const> kids,
                                         NodeId self) noexcept {
    // Binary search on the ascending-by-construction id order.
    std::size_t lo = 0, hi = kids.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (kids[mid].v < self.v) lo = mid + 1;
        else                      hi = mid;
    }
    if (lo < kids.size() && kids[lo] == self) return lo;
    // Fallback: verify failed (unsorted producer) — legacy linear scan.
    for (std::size_t i = 0; i < kids.size(); ++i) {
        if (kids[i] == self) return i;
    }
    return kids.size();
}

} // namespace

bool TreeCursor::gotoFirstChild() noexcept {
    if (!current_.valid()) return false;
    auto kids = tree_->children(current_);
    for (NodeId k : kids) {
        if (isVisible_(k)) {
            current_ = k;
            return true;
        }
    }
    return false;
}

bool TreeCursor::gotoLastChild() noexcept {
    if (!current_.valid()) return false;
    auto kids = tree_->children(current_);
    // C++23 ranges::reverse_view over the span. Equivalent to a hand-written
    // rbegin/rend loop; chosen for stylistic consistency with the rest of
    // the codebase ("stdlib-first").
    auto it = std::ranges::find_if(
        std::views::reverse(kids),
        [this](NodeId k) { return isVisible_(k); });
    if (it == std::views::reverse(kids).end()) return false;
    current_ = *it;
    return true;
}

bool TreeCursor::gotoNextSibling() noexcept {
    if (!current_.valid()) return false;
    const NodeId parent = tree_->parent(current_);
    if (!parent.valid()) return false;
    auto kids = tree_->children(parent);
    // c97: O(log K) self-location (verified binary search — see
    // `siblingIndexOf`), then advance to the next VISIBLE sibling exactly
    // as before. `self` absent from `kids` (corrupt parent link) keeps the
    // legacy `return false`.
    const std::size_t selfIdx = siblingIndexOf(kids, current_);
    if (selfIdx >= kids.size()) return false;   // self not among kids — legacy outcome
    for (std::size_t j = selfIdx + 1; j < kids.size(); ++j) {
        if (isVisible_(kids[j])) {
            current_ = kids[j];
            return true;
        }
    }
    return false;
}

bool TreeCursor::gotoPrevSibling() noexcept {
    if (!current_.valid()) return false;
    const NodeId parent = tree_->parent(current_);
    if (!parent.valid()) return false;
    auto kids = tree_->children(parent);
    // c97: O(log K) self-location (verified binary search), then scan
    // BACKWARD for the nearest visible sibling — the same node the former
    // forward candidate-tracking pass selected. `self` absent from `kids`
    // returns kids.size() → the loop below never runs → legacy false.
    const std::size_t selfIdx = siblingIndexOf(kids, current_);
    if (selfIdx >= kids.size()) return false;   // self not among kids — legacy outcome
    for (std::size_t j = selfIdx; j > 0; --j) {
        if (isVisible_(kids[j - 1])) {
            current_ = kids[j - 1];
            return true;
        }
    }
    return false;
}

bool TreeCursor::gotoParent() noexcept {
    if (!current_.valid()) return false;
    NodeId p = tree_->parent(current_);
    if (!p.valid()) return false;
    // In AST mode, a parent might itself be EmptySpace (rare — e.g. a
    // config that models "blank-line group" as an Internal node with the
    // flag set). Walk up to the first visible ancestor. The loop is
    // capped by tree.nodeCount() iterations so a corrupt parent-cycle
    // produces a clean failure instead of hanging.
    if (mode_ == CursorMode::Ast) {
        const std::size_t cap = tree_->nodeCount();
        std::size_t hops = 0;
        while (p.valid() && !isVisible_(p)) {
            if (++hops > cap) {
                std::fputs("dss::TreeCursor fatal: gotoParent detected parent-chain cycle\n", stderr);
                std::abort();
            }
            p = tree_->parent(p);
        }
        if (!p.valid()) return false;
    }
    current_ = p;
    return true;
}

// ── bookmark / restore ───────────────────────────────────────────────────

TreeCursor::Bookmark TreeCursor::mark() const noexcept {
    return Bookmark{tree_, current_, tree_->id()};
}

void TreeCursor::restore(Bookmark saved) noexcept {
    // Distinguish three failure modes — each gets its own stderr message
    // so triage is unambiguous.
    if (!saved.valid()) {
        std::fputs("dss::TreeCursor fatal: restore() with invalid Bookmark\n", stderr);
        std::abort();
    }
    if (saved.tree() != tree_) {
        std::fputs("dss::TreeCursor fatal: restore() with cross-tree Bookmark\n", stderr);
        std::abort();
    }
    if (saved.treeId() != tree_->id()) {
        // Same heap address, different TreeId — the Tree we point at was
        // destroyed and rebuilt (or replaced). The Bookmark's NodeId may
        // be out of range or refer to a totally different node.
        std::fputs("dss::TreeCursor fatal: restore() with stale Bookmark (TreeId mismatch)\n", stderr);
        std::abort();
    }
    current_ = saved.id();
}

// ── convenience ──────────────────────────────────────────────────────────

bool TreeCursor::isAtLeaf() const noexcept {
    if (!current_.valid()) return true;       // no movement possible from an invalid position
    auto kids = tree_->children(current_);
    if (mode_ == CursorMode::Cst) {
        return kids.empty();
    }
    // AST: leaf iff no visible child.
    for (NodeId k : kids) {
        if (isVisible_(k)) return false;
    }
    return true;
}

int TreeCursor::depth() const noexcept {
    if (!current_.valid()) return 0;
    const std::size_t cap = tree_->nodeCount();
    int d = 0;
    NodeId cur = current_;
    std::size_t hops = 0;
    while (true) {
        const NodeId p = tree_->parent(cur);
        if (!p.valid()) break;
        if (++hops > cap) {
            // Corrupt parent chain (cycle). Signal via negative depth
            // rather than aborting — depth() is `noexcept` and used in
            // diagnostic-rendering paths where aborting would compound
            // the original problem.
            return -1;
        }
        ++d;
        cur = p;
    }
    return d;
}

} // namespace dss
