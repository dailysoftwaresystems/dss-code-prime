#pragma once

// ── A MONOTONE FIXPOINT'S BOUND IS ITS LATTICE'S HEIGHT (cycle P69 L2, lane `lm`) ──────────────
//
// A fixpoint iterated in passes over a finite lattice, where every pass only adds (a set only
// grows, a flag only goes from false to true), changes at least one bit on every pass but the
// last. It therefore takes at most as many passes as the lattice has bits, plus the one pass
// that changes nothing. A pass past that means a pass took a bit back: the iteration is not
// monotone. That is a bug of the code that iterates, never of its input, and the process stops
// loud, naming the fixpoint.
//
// A FIXED number in that place (`iters++ < 10000`) ended the iteration as if it had converged
// when it had not, and three of the grammar loader's four fixpoints then went on with the sets
// they held at that pass, in silence. `FixpointPasses` replaces every such cap.
//
// Usage. Count a pass at its start:
//
//   bool changed = true;
//   for (substrate::FixpointPasses passes{rules * (tokens + 1), "the FIRST-set fixpoint"}; changed;) {
//       passes.pass();
//       changed = false;
//       …
//   }

#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace dss::substrate {

// The ONE answer of a fixpoint that took more passes than its lattice has bits, shaped like
// the arena fatals (`arena_tag.hpp`): the fixpoint's name first, so a death test pins which.
[[noreturn]] inline void failOnNonMonotoneFixpoint(char const* fixpoint, std::size_t bits) {
    std::fprintf(stderr, "dss::substrate fatal: %s: took more passes than its lattice has bits "
                         "(%zu) and one, so a pass is not monotone\n", fixpoint, bits);
    std::abort();
}

class FixpointPasses {
public:
    // `latticeBits` is the height of the lattice the fixpoint climbs: the bits its sets and
    // flags can gain in all. `fixpoint` names it in the one failure.
    FixpointPasses(std::size_t latticeBits, char const* fixpoint) noexcept
        : bits_{latticeBits}, fixpoint_{fixpoint} {}

    // Counts the pass about to run. The pass past the lattice's height plus one stops the process.
    void pass() {
        if (++taken_ > bits_ + 1) failOnNonMonotoneFixpoint(fixpoint_, bits_);
    }
    // The passes counted so far.
    [[nodiscard]] std::size_t taken() const noexcept { return taken_; }

private:
    std::size_t bits_;
    std::size_t taken_ = 0;
    char const* fixpoint_;
};

} // namespace dss::substrate
