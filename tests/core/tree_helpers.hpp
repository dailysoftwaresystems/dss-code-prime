#pragma once

// Tiny test-only helpers for walking visible children of a Tree node.
// EXTRACTED in the 08.55 cleanup when `tree_views.hpp` was retired —
// every production caller now goes through direct rule lookups
// (`tree.rule(id) == schema.rules().find("...")`) and direct child
// indexing. Tests retain the visible-child filter helper because
// positional access over visible-only children is a common assertion
// pattern across many tests.

#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"

#include <cstddef>

namespace dss::tests {

[[nodiscard]] inline bool isVisible(NodeFlags f) noexcept {
    return !isEmptySpace(f);
}

// Returns the `index`-th visible (non-EmptySpace) child of `parent`,
// or InvalidNode when `parent` has fewer than (index+1) visible
// children. Zero-allocation; stops walking at the requested index.
[[nodiscard]] inline NodeId nthVisibleChild(Tree const& t, NodeId parent,
                                            std::size_t index) noexcept {
    if (!parent.valid()) return InvalidNode;
    std::size_t seen = 0;
    for (NodeId c : t.children(parent)) {
        if (!isVisible(t.flags(c))) continue;
        if (seen == index) return c;
        ++seen;
    }
    return InvalidNode;
}

} // namespace dss::tests
