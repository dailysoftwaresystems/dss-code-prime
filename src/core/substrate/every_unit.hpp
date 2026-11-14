#pragma once

// ══ THE ONE HOME OF "A SEVERAL-UNIT STEP REPORTS EVERY UNIT BEFORE IT FAILS" (P69 H6b, the review of H6's N1) ══════
//
// THE INVARIANT: a step a build takes for each of several units -- assemble a unit, link an archive's member, merge a
// unit's diagnostics, claim an input file for a language, read an input archive, validate an archive member the
// build writes -- is taken for EVERY unit, in unit order, and a unit that fails never stops the units after it; the
// build fails AFTER the last unit, when any failed. One run then says what is wrong with each unit, where a loop that
// returns at its first failure reports one unit and hides the rest (a second run, after the first unit is fixed, is
// the only way to learn of the next).
//
// The verb was the multi-unit front half's own before it was this one: every unit built, every unit's diagnostics
// merged in unit order, THEN the failure checked. Every several-unit loop of the pipeline takes it.
//
// ⚠ NOT `std::all_of`: `all_of` STOPS at the first false, which is the defect this exists to end. A step reports its
// own unit's diagnostics, answers whether that unit succeeded, and never returns from the build itself; the caller
// fails the build on a false answer, after every unit was asked.
//
// ★ IN core's SUBSTRATE, NOT program/ (P69 fold 3e, the orchestrator's ledger 569): the class has sites below the
// driver -- the link's archive writer validates every member it is handed -- and the link does not depend on the
// program layer, so a home there could not be reached by every site of its class. Every layer reaches core.
//
// Pinned by tests/program/test_every_unit.cpp (both cells for each verb: every unit failing, and a valid unit before
// an invalid one; the growing form also asks a unit a step added).

#include <cstddef>
#include <type_traits>

namespace dss::substrate {

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

// THE GROWING FORM: a search whose units are found while it asks them -- an archive pull, whose member read names the
// references that pull the next members. Asks `step(i)` of every unit from `cursor` while `cursor < size()`, with
// `size()` read again after each unit, so a unit a step appends is asked as well; leaves `cursor` past the last unit
// asked, for a search that adds more and resumes. Answers whether every unit it asked succeeded.
template <class Size, class Step>
    requires std::is_invocable_r_v<std::size_t, Size&> && std::is_invocable_r_v<bool, Step&, std::size_t>
[[nodiscard]] bool runEveryUnitOfAGrowingSet(std::size_t& cursor, Size&& size, Step&& step) {
    bool every = true;
    for (; cursor < size(); ++cursor) {
        every = step(cursor) && every;
    }
    return every;
}

}  // namespace dss::substrate
