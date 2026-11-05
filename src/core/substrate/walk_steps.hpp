#pragma once

// ── A WALK'S BOUND IS THE SIZE OF WHAT IT WALKS (cycle P69 L2, lane `lm`) ─────────────────────
//
// A walk over a finite acyclic structure takes at most as many steps as the structure has
// elements. Such a structure is a tree's nodes (a work stack over a subtree, a descent to a
// child, a climb to a parent) or an interned type's operand DAG. In a tree each node has one
// parent, so a walk that pushes a node's children once per visit visits each node at most
// once, and a descent or a climb moves strictly. A step past that count means the graph is
// CYCLIC. That is memory corruption or a builder bug, never the program's fault, and the
// process stops loud.
//
// A FIXED number in that place (`guard < 8192`) answered as if the walk had ended when it had
// not. Each such walk read a construct past its number wrong, in silence: an attribute not
// collected, a bound not captured, a refusal skipped. `WalkSteps` replaces every one.
//
// Usage. The counter is the loop's increment, so `continue` counts a step too:
//
//   for (substrate::WalkSteps steps{tree.nodeCount(), "the attribute walk"}; !stack.empty();
//        steps.step()) {
//       …
//   }

#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace dss::substrate {

// The ONE answer of a walk that took more steps than what it walks has elements. It is
// shaped like the arena fatals (`arena_tag.hpp`): the walk's name first, so a death test
// pins WHICH walk stopped.
[[noreturn]] inline void failOnCyclicWalk(char const* walk) {
    std::fprintf(stderr, "dss::substrate fatal: %s: took more steps than what it walks has "
                         "elements, so the graph is cyclic\n", walk);
    std::abort();
}

class WalkSteps {
public:
    // `elements` is the size of what the walk walks: `Tree::nodeCount()`,
    // `TreeBuilder::nodeCount()`, `TypeInterner::size()`, `Hir::nodeCount()`. `walk` names it
    // in the one failure.
    WalkSteps(std::size_t elements, char const* walk) noexcept
        : bound_{elements + 1}, walk_{walk} {}

    void step() {
        if (++taken_ > bound_) failOnCyclicWalk(walk_);
    }
    // The steps taken so far. It is 0 on the first visit.
    [[nodiscard]] std::size_t taken() const noexcept { return taken_; }

private:
    std::size_t bound_;
    std::size_t taken_ = 0;
    char const* walk_;
};

} // namespace dss::substrate
