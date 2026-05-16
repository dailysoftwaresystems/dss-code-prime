#pragma once

#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"

#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

// Walk helpers over TreeCursor. Header-only — pure template glue around
// the cursor's movement primitives.
//
// The walks themselves allocate nothing — state is the cursor plus one int.
// They are subtree-bounded: a walk never ascends past the cursor's start node,
// so walking one declaration cannot leak into the surrounding file. Mode is
// the cursor's responsibility — pass a CST cursor to see every node, an AST
// cursor to skip EmptySpace leaves.
//
// Visitor signatures (auto-detected via `if constexpr`):
//   - `void (TreeCursor const&)`       — treated as if it returned `Continue`.
//   - `WalkAction (TreeCursor const&)` — explicit per-node control.

namespace dss {

enum class WalkAction : std::uint8_t {
    Continue,
    SkipChildren,  // pre-order only — ignored in post-order (children have
                   // already been visited by the time the parent is seen).
    Stop,
};

namespace detail::visitor {

template <typename F>
[[nodiscard]] WalkAction invokeVisit(F&& f, TreeCursor const& c) {
    using R = std::invoke_result_t<F, TreeCursor const&>;
    if constexpr (std::is_void_v<R>) {
        std::invoke(std::forward<F>(f), c);
        return WalkAction::Continue;
    } else {
        static_assert(std::is_same_v<R, WalkAction>,
                      "Visitor must return either void or dss::WalkAction");
        return std::invoke(std::forward<F>(f), c);
    }
}

} // namespace detail::visitor

template <typename F>
void walkPreOrder(TreeCursor cursor, F&& visit) {
    if (!cursor.current().valid()) return;

    int depthFromStart = 0;
    while (true) {
        const WalkAction a = detail::visitor::invokeVisit(visit, cursor);
        if (a == WalkAction::Stop) return;

        if (a != WalkAction::SkipChildren && cursor.gotoFirstChild()) {
            ++depthFromStart;
            continue;
        }

        // Ascend until we find a sibling or hit the start node. Never try a
        // sibling at depth 0 — that would visit the start node's siblings,
        // escaping the requested subtree.
        while (true) {
            if (depthFromStart == 0) return;
            if (cursor.gotoNextSibling()) break;
            if (!cursor.gotoParent()) return;
            --depthFromStart;
        }
    }
}

template <typename F>
void walkPreOrder(Tree const& t, NodeId start, F&& visit) {
    walkPreOrder(TreeCursor{t, start, CursorMode::Cst}, std::forward<F>(visit));
}

template <typename F>
void walkPreOrder(Tree const& t, F&& visit) {
    walkPreOrder(t.cursor(), std::forward<F>(visit));
}

template <typename F>
void walkPostOrder(TreeCursor cursor, F&& visit) {
    if (!cursor.current().valid()) return;

    // Descend to the leftmost leaf inside the start subtree; post-order
    // visits leaves before parents, so the first visit happens here.
    int depthFromStart = 0;
    while (cursor.gotoFirstChild()) ++depthFromStart;

    while (true) {
        const WalkAction a = detail::visitor::invokeVisit(visit, cursor);
        if (a == WalkAction::Stop) return;

        // At depth 0 we've just visited the start node; trying its sibling
        // would escape the requested subtree.
        if (depthFromStart == 0) return;
        if (cursor.gotoNextSibling()) {
            while (cursor.gotoFirstChild()) ++depthFromStart;
            continue;
        }
        if (!cursor.gotoParent()) return;
        --depthFromStart;
    }
}

template <typename F>
void walkPostOrder(Tree const& t, NodeId start, F&& visit) {
    walkPostOrder(TreeCursor{t, start, CursorMode::Cst}, std::forward<F>(visit));
}

template <typename F>
void walkPostOrder(Tree const& t, F&& visit) {
    walkPostOrder(t.cursor(), std::forward<F>(visit));
}

} // namespace dss
