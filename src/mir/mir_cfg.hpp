#pragma once

// Shared MIR CFG helpers — reachability + traversal utilities consumed
// by both the verifier (ML3) and the optimizer (DCE block-reachability).
//
// Extracted from `src/mir/mir_verifier.cpp` when the second consumer
// (the DCE pass) arrived. The verifier `#include`s this header and
// removes its local copy; the substrate is now shared at one source
// of truth so a CFG-walk fix improves both consumers simultaneously.

#include "mir/mir.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace dss {

// Reverse post-order traversal of the CFG starting at `entry`.
// Unreachable blocks are excluded by construction — every block in the
// returned vector is forward-reachable from `entry` via `blockSuccessors`.
// Returns an empty vector for an invalid `entry`.
//
// Used by the verifier (D-OPT1-VERIFY-AFTER-EVERY-PASS) to scope
// per-block invariants to reachable blocks, AND by the DCE pass to
// identify dead (unreachable) blocks — anything not in this list is
// safe to elide.
[[nodiscard]] inline std::vector<MirBlockId>
mirReversePostOrder(Mir const& mir, MirBlockId entry) {
    std::vector<MirBlockId> order;
    if (!entry.valid()) return order;
    std::unordered_set<std::uint32_t> visited;
    struct Frame { MirBlockId block; std::size_t nextSucc; };
    std::vector<Frame> stack;
    auto push = [&](MirBlockId b) {
        if (b.valid() && visited.insert(b.v).second) {
            stack.push_back({b, 0});
        }
    };
    push(entry);
    while (!stack.empty()) {
        Frame& top = stack.back();
        auto const succs = mir.blockSuccessors(top.block);
        if (top.nextSucc < succs.size()) {
            MirBlockId const s = succs[top.nextSucc++];
            push(s);
        } else {
            order.push_back(top.block);
            stack.pop_back();
        }
    }
    std::reverse(order.begin(), order.end());
    return order;
}

} // namespace dss
