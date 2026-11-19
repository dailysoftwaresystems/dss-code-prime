#pragma once

#include <cstdint>

// ── WHAT A LINK PASS ANSWERS (P69 f26e, B1 A4) ───────────────────────────────
// A pass of the link that may rewrite the units it is given answers one of
// three things, and the answer is a VALUE its caller switches on:
//   * `Unchanged` -- the pass had nothing to do: its output storage is
//     untouched, and the caller links what it gave the pass;
//   * `Rewritten` -- the output storage holds the rewritten unit(s), which the
//     caller links in place of its input;
//   * `Refused`   -- the pass reported, by name, why the link cannot go on: the
//     output storage is not link input, and the caller stops.
// It replaces a `bool` whose `false` meant both "rewritten" and "refused" and
// whose caller told the two apart by the reporter's error count -- a refusal
// carried by a side channel, which a pass that refused without a report, or a
// caller that read the count at the wrong moment, turned into a link of the
// wrong input. The passes: `discardLaterSectionGroups`,
// `allocateCommonDefinitions`, `bindFallbackReferences`,
// `restateWeakNameReferencesByRow`, `rejectOrDropUnreferencedExterns`,
// `materializeObjectImportSlots` (`link/linker.cpp`),
// `bindPcRelativeImportAddressUnits` (`link/pc_relative_import_address.hpp`)
// and `lowerGotSlotReferences` (`link/got_slots.hpp`).
//
// The type is `[[nodiscard]]`, so a caller that drops the answer drops the
// refusal with it and does not compile: the root CMakeLists makes a discarded
// `[[nodiscard]]` value an error on every compiler, and tests/link pins it with
// a unit that must fail to build (`discarded_link_pass_outcome.cpp`).
namespace dss::linker {

enum class [[nodiscard]] LinkPassOutcome : std::uint8_t {
    Unchanged,
    Rewritten,
    Refused,
};

}  // namespace dss::linker
