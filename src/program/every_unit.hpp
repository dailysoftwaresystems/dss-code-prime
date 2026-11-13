#pragma once

// ══ THE ONE HOME OF "A SEVERAL-UNIT STEP REPORTS EVERY UNIT BEFORE IT FAILS" (P69 H6b, the review of H6's N1) ══════
//
// THE INVARIANT: a step a build takes for each of several units -- assemble a unit, link an archive's member, merge a
// unit's diagnostics, claim an input file for a language -- is taken for EVERY unit, in unit order, and a unit that
// fails never stops the units after it; the build fails AFTER the last unit, when any failed. One run then says what is
// wrong with each unit, where a loop that returns at its first failure reports one unit and hides the rest (a second
// run, after the first unit is fixed, is the only way to learn of the next).
//
// The verb was the multi-unit front half's own before it was this one: every unit built, every unit's diagnostics
// merged in unit order, THEN the failure checked. Every several-unit loop of the pipeline takes it.
//
// ⚠ NOT `std::all_of`: `all_of` STOPS at the first false, which is the defect this exists to end. A step reports its
// own unit's diagnostics, answers whether that unit succeeded, and never returns from the build itself; the caller
// fails the build on a false answer, after every unit was asked.
//
// Pinned by tests/program/test_every_unit.cpp (both cells: every unit failing, and a valid unit before an invalid one).

#include <cstddef>
#include <type_traits>

namespace dss {

// Asks `step(i)` of EVERY unit `i` in [0, units), in order -- whatever an earlier unit answered -- and answers whether
// every one succeeded.
template <class Step>
    requires std::is_invocable_r_v<bool, Step&, std::size_t>
[[nodiscard]] bool runEveryUnit(std::size_t units, Step&& step) {
    bool every = true;
    for (std::size_t i = 0; i < units; ++i) {
        if (!step(i)) every = false;
    }
    return every;
}

}  // namespace dss
