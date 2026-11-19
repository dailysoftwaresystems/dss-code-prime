#pragma once

// ══ THE ONE HOME OF "A SEVERAL-UNIT STEP REPORTS EVERY UNIT BEFORE IT FAILS" (P69 H6b; H6c2: ONE loop) ═══════════
//
// THE INVARIANT: a step a build takes for each of several UNITS -- a source file, a `sources` entry, an input or
// object file, an input archive and its members, an operator-named library, a translation unit, the headers of a
// header directory, a target, a dependency and its children, an include or search directory, a workspace's manifests
// and their targets, an argument with the values it takes, a name a step fetches a member for -- is taken for EVERY
// unit, in unit order, and a unit that fails never stops the units after it; the build fails AFTER the last unit, when
// any failed. One run then says what is wrong with each unit, where a loop that returns at its first failure reports
// one unit and hides the rest (a second run, after the first unit is fixed, is the only way to learn of the next).
//
// WHAT A UNIT IS: an element of a set the build acts on that passes through its OWN host operation or tier step (a
// read, a build, an assembly, a fetch), and whose failure does not decide the others'. NOT a unit: a consistency check
// across elements (two rows that disagree are one fact), a search answering one question, a sequence whose later step
// consumes an earlier one's product, one input's or one document's internals, a cumulative bound.
//
// Every loop of the tree over a build's units takes the ONE loop below, and every one whose units report takes the
// net (`namingSilentFailures`).
//
// ★ ONE LOOP, FOUR USES. Between units the loop reads the cursor and the size again, and that is the whole contract:
//   * a FIXED set -- `runEveryUnit`, the one loop over a size captured once;
//   * a GROWING set -- a search whose units are found while it asks them (an archive pull: a member read names the
//     references that pull the next members); the size grows, by a step or between calls, and the cursor is left past
//     the last unit asked, for a search that resumes;
//   * a unit that SPANS ENTRIES -- an argument and the values it takes: a step may move the cursor FORWARD past the
//     entries its own unit spans;
//   * a set that CLOSES -- a walk that ends at its first answer: the size may close at the unit just asked, and no
//     unit after it is asked.
// The loop never moves the cursor back, and never asks a unit at or past the size; a step that moves it BACK is a
// contract violation the loop refuses LOUDLY (`failOnACursorMovedBack`), never a walk that spins.
//
// THE NET'S CONTRACT: a step fails only for a failure it REPORTED ITSELF, and its report names its unit. A step that
// fails having added nothing to the site's count is a substrate-contract violation the build would otherwise fail on
// with nothing said, so the net names it. A unit that meets a failure an EARLIER unit already reported answers TRUE:
// the earlier unit's false already fails the walk. (Names: a reporter's dedup window drops a report that repeats an
// earlier one word for word, so a unit whose report did not name its unit could be named silent.)
//
// ⚠ NOT `std::all_of`: `all_of` STOPS at the first false, which is the defect this exists to end.
//
// ★ IN core's SUBSTRATE, NOT program/: the class has sites below the driver -- the link's archive writer validates
// every member it is handed -- and every layer reaches core.
//
// Pinned by tests/program/test_every_unit.cpp: every form's cells (every unit failing; a valid unit before an invalid
// one; every unit succeeding; a unit a step appends; a resumed search; a unit spanning entries; a set closing at its
// answer; a silent unit after a reporting one, and no reporting one named; a cursor moved back is fatal), and one
// cell per site.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>

namespace dss::substrate {

// The ONE answer of a step that moved the walk's cursor BACK, shaped like the arena fatals (`arena_tag.hpp`): the
// walk would ask a unit again and again, a hang where the contract wants a refusal.
[[noreturn]] inline void failOnACursorMovedBack(std::size_t asked, std::size_t cursor) {
    std::fprintf(stderr,
                 "dss::substrate fatal: runEveryUnitOfAGrowingSet: the step of unit %zu moved the cursor back to %zu "
                 "-- a step may move it only forward, past the entries its own unit spans\n",
                 asked, cursor);
    std::abort();
}

// THE ONE LOOP: asks `step(cursor)` of every unit from `cursor` while `cursor < size()`, with the cursor and `size()`
// read again after each unit; leaves `cursor` past the last unit asked. Answers whether every unit it asked succeeded.
template <class Size, class Step>
    requires std::is_invocable_r_v<std::size_t, Size&> && std::is_invocable_r_v<bool, Step&, std::size_t>
[[nodiscard]] bool runEveryUnitOfAGrowingSet(std::size_t& cursor, Size&& size, Step&& step) {
    bool every = true;
    for (; cursor < size(); ++cursor) {
        std::size_t const asked = cursor;
        every = step(cursor) && every;
        if (cursor < asked) failOnACursorMovedBack(asked, cursor);
    }
    return every;
}

// A FIXED set: the one loop over a size that never changes -- no loop of its own.
template <class Step>
    requires std::is_invocable_r_v<bool, Step&, std::size_t>
[[nodiscard]] bool runEveryUnit(std::size_t units, Step&& step) {
    std::size_t cursor = 0;
    return runEveryUnitOfAGrowingSet(cursor, [units] { return units; }, std::forward<Step>(step));
}

// THE NET, as a STEP, never a loop: wraps a unit's step so the site's count is read before EACH unit, and a unit that
// fails having added nothing to it is named (`onSilentFailure(i)`), in unit order. It serves every form of the one
// loop. The callables are captured by value: a `[&]` lambda is cheap to copy, and no reference outlives the wrapper.
template <class Step, class ErrorCount, class OnSilentFailure>
    requires std::is_invocable_r_v<bool, Step&, std::size_t> && std::is_invocable_r_v<std::size_t, ErrorCount&>
             && std::is_invocable_v<OnSilentFailure&, std::size_t>
[[nodiscard]] auto namingSilentFailures(Step&& step, ErrorCount&& errorCount, OnSilentFailure&& onSilentFailure) {
    return [step = std::forward<Step>(step), errorCount = std::forward<ErrorCount>(errorCount),
            onSilentFailure = std::forward<OnSilentFailure>(onSilentFailure)](std::size_t i) mutable -> bool {
        std::size_t const reported = errorCount();
        if (step(i)) return true;
        if (errorCount() == reported) onSilentFailure(i);
        return false;
    };
}

}  // namespace dss::substrate
