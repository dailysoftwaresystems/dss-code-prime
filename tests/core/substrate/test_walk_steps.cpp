// A WALK'S BOUND IS THE SIZE OF WHAT IT WALKS (cycle P69 L2, lane `lm`): `substrate::WalkSteps`.
//
// The front end's walks over a tree (a work stack over a subtree, a descent, a climb) and over an
// interned type's spine are bounded by the number of elements they walk, never by a fixed number. On
// a finite acyclic structure such a walk can never take more steps than that. A step past it means
// the graph is CYCLIC, and the process stops loud, naming the walk. These cases synthesize both:
// trees that every walk finishes, however deep or wide, and cycles that each walk shape stops on.
// Each synthesized walk carries a generous bound of its own, so a disarmed stop makes a death case
// fail ("failed to die") rather than hang.

#include "core/substrate/walk_steps.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace {

using dss::substrate::WalkSteps;

// A synthesized node graph: `children[n]` lists node n's children, `parent[n]` its parent (or none).
struct Graph {
    std::vector<std::vector<std::size_t>> children;
    std::vector<std::size_t>              parent;
};
constexpr std::size_t kNone = static_cast<std::size_t>(-1);

// The test's own net, far above every bound below, so a disarmed stop ends the walk instead of hanging.
constexpr std::size_t kNoHang = std::size_t{1} << 22;

Graph chain(std::size_t n) {
    Graph g{std::vector<std::vector<std::size_t>>(n), std::vector<std::size_t>(n, kNone)};
    for (std::size_t i = 0; i + 1 < n; ++i) {
        g.children[i] = {i + 1};
        g.parent[i + 1] = i;
    }
    return g;
}

Graph fullBinary(std::size_t levels) {
    std::size_t const n = (std::size_t{1} << levels) - 1;
    Graph g{std::vector<std::vector<std::size_t>>(n), std::vector<std::size_t>(n, kNone)};
    for (std::size_t i = 0; 2 * i + 2 < n; ++i) {
        g.children[i] = {2 * i + 1, 2 * i + 2};
        g.parent[2 * i + 1] = i;
        g.parent[2 * i + 2] = i;
    }
    return g;
}

// The work-stack shape (`for (WalkSteps steps{…}; !stack.empty(); steps.step())`): the visits taken.
std::size_t walkSubtree(Graph const& g, std::size_t root, char const* name) {
    std::vector<std::size_t> stack{root};
    std::size_t visits = 0;
    for (WalkSteps steps{g.children.size(), name}; !stack.empty() && visits < kNoHang; steps.step()) {
        std::size_t const n = stack.back();
        stack.pop_back();
        ++visits;
        for (std::size_t c : g.children[n]) stack.push_back(c);
    }
    return visits;
}

// The descent shape: to the first child until there is none. Returns the moves taken.
std::size_t descend(Graph const& g, std::size_t from, char const* name) {
    std::size_t moves = 0;
    std::size_t cur = from;
    for (WalkSteps steps{g.children.size(), name}; !g.children[cur].empty() && moves < kNoHang;
         steps.step()) {
        cur = g.children[cur].front();
        ++moves;
    }
    return moves;
}

// The climb shape: to the parent until there is none. Returns the moves taken.
std::size_t climb(Graph const& g, std::size_t from, char const* name) {
    std::size_t moves = 0;
    std::size_t cur = from;
    for (WalkSteps steps{g.parent.size(), name}; g.parent[cur] != kNone && moves < kNoHang;
         steps.step()) {
        cur = g.parent[cur];
        ++moves;
    }
    return moves;
}

}  // namespace

// Every walk of a tree finishes, at any depth and any width: a fixed number in the bound's place
// would have stopped each of these part way and answered as if it had ended.
TEST(WalkSteps, EveryWalkOfATreeFinishesAtAnyDepthOrWidth) {
    Graph const deep = chain(100000);
    EXPECT_EQ(walkSubtree(deep, 0, "the deep walk"), 100000u);
    EXPECT_EQ(descend(deep, 0, "the deep descent"), 99999u);
    EXPECT_EQ(climb(deep, 99999, "the deep climb"), 99999u);

    Graph const wide = fullBinary(17);   // 131071 nodes
    EXPECT_EQ(walkSubtree(wide, 0, "the wide walk"), 131071u);
    EXPECT_EQ(descend(wide, 0, "the wide descent"), 16u);
    EXPECT_EQ(climb(wide, wide.parent.size() - 1, "the wide climb"), 16u);
}

// The bound is what is walked, plus the step taken after the last visit: that many steps pass and
// one more stops the process.
TEST(WalkSteps, TheBoundIsTheElementsAndTheStepAfterTheLastVisit) {
    WalkSteps steps{3, "the counted walk"};
    EXPECT_EQ(steps.taken(), 0u);
    for (int i = 0; i < 4; ++i) steps.step();
    EXPECT_EQ(steps.taken(), 4u);

    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            WalkSteps over(3, "the counted walk");
            for (int i = 0; i < 5; ++i) over.step();
        },
        "dss::substrate fatal: the counted walk: took more steps than what it walks has elements");
}

// A cyclic graph stops every walk shape, loud, naming the walk that met it.
TEST(WalkSteps, ACycleStopsEachWalkShapeNamingTheWalk) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // 0 -> 1 -> 2 -> 0 below, and 0's parent is 2 above: a cycle both ways.
    Graph cyc{{{1}, {2}, {0}}, {2, 0, 1}};
    EXPECT_DEATH((void)walkSubtree(cyc, 0, "the synthesized cycle's walk"),
                 "dss::substrate fatal: the synthesized cycle's walk: .*the graph is cyclic");
    EXPECT_DEATH((void)descend(cyc, 0, "the synthesized cycle's descent"),
                 "dss::substrate fatal: the synthesized cycle's descent: .*the graph is cyclic");
    EXPECT_DEATH((void)climb(cyc, 0, "the synthesized cycle's climb"),
                 "dss::substrate fatal: the synthesized cycle's climb: .*the graph is cyclic");

    // A cycle hanging below an acyclic stretch: the walk is stopped once it is in the cycle.
    Graph tail = chain(50);
    tail.children[49] = {40};
    EXPECT_DEATH((void)walkSubtree(tail, 0, "the cycle below a chain"),
                 "dss::substrate fatal: the cycle below a chain: ");
}
