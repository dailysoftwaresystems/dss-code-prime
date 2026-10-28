// A MONOTONE FIXPOINT'S BOUND IS ITS LATTICE'S HEIGHT (cycle P69 L2, lane `lm`):
// `substrate::FixpointPasses`.
//
// Three of the grammar loader's fixpoints stopped at a fixed 10000 passes in SILENCE and went on
// with the sets of that pass. A fixpoint that only adds changes a bit on every pass but its last,
// so its lattice's height plus one bounds its passes exactly; a pass past it took a bit back, and
// the process stops, naming the fixpoint. These cases synthesize both: a monotone fixpoint far past
// the old 10000 passes that finishes, the bound's exactness, and a fixpoint that is not monotone.

#include "core/substrate/fixpoint_passes.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace {

using dss::substrate::FixpointPasses;

// A chain of `bits` flags where flag i is set once flag i+1 is, and only the last starts set: each
// pass, read from the front, sets exactly one more flag. It takes `bits` passes to set them all
// and one more to see nothing change. `takeBack` clears flag 0 again whenever it is set (a pass
// that is not monotone). Returns the passes taken.
std::size_t chainFixpoint(std::size_t bits, bool takeBack, char const* name) {
    std::vector<bool> set(bits, false);
    set[bits - 1] = true;
    std::size_t lowestSet = bits - 1;   // every flag at or above it is set
    bool changed = true;
    FixpointPasses passes{bits, name};
    std::size_t const net = 4 * bits + 16;   // the test's own net: a disarmed stop ends, never hangs
    while (changed && passes.taken() < net) {
        passes.pass();
        changed = false;
        // The one flag this pass can set: the one below the lowest set flag.
        if (lowestSet > 0 && set[lowestSet] && !set[lowestSet - 1]) {
            set[--lowestSet] = true;
            changed = true;
        }
        if (takeBack && set[0]) {
            set[0] = false;
            set[1] = false;
            lowestSet = 2;
            changed = true;
        }
    }
    return passes.taken();
}

}  // namespace

TEST(FixpointPasses, AMonotoneFixpointFarPastTheOldTenThousandPassesFinishes) {
    EXPECT_EQ(chainFixpoint(20000, false, "the long chain"), 20000u)
        << "19999 passes that each set a flag, and the one that sets none";
}

TEST(FixpointPasses, TheBoundIsTheLatticesBitsAndOnePass) {
    FixpointPasses passes{3, "the counted fixpoint"};
    for (int i = 0; i < 4; ++i) passes.pass();
    EXPECT_EQ(passes.taken(), 4u);

    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            FixpointPasses over(3, "the counted fixpoint");
            for (int i = 0; i < 5; ++i) over.pass();
        },
        "dss::substrate fatal: the counted fixpoint: took more passes than its lattice has bits \\(3\\)");
}

TEST(FixpointPasses, APassThatTakesABitBackStopsTheProcessNamingTheFixpoint) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH((void)chainFixpoint(8, true, "the synthesized non-monotone fixpoint"),
                 "dss::substrate fatal: the synthesized non-monotone fixpoint: .*so a pass is not monotone");
}
