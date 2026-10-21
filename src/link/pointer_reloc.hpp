#pragma once

#include "core/types/strong_ids.hpp"      // RelocationKind
#include "core/types/target_schema.hpp"   // TargetSchema, RelocFormulaKind

#include <cstdint>
#include <optional>

namespace dss::linker {

// The target's ABSOLUTE pointer relocation of `widthBytes` bytes — the one a
// slot holding an address is filled through — found by FORMULA, never by name:
// a Linear, non-pc-relative row of that width that is neither a thread-pointer
// offset nor an image-relative value (both answer the width-and-pc question
// structurally while writing a number in another coordinate space — the reason
// `absoluteRelocKind` in the assembler names both properties). ONE predicate for
// every pass that mints or recognizes a pointer slot: the cross-CU merge, the
// object-carried import slots and the GOT-slot lowering in `linker.cpp` (P68
// round 11: the first two had each spelled `widthBytes == 8 && !pcRelative`
// for themselves, without the two exclusions), and — since P69 — the PE
// writer's loader-bound import slots (design c2), which used to re-spell it.
//
// ★ P69 fold 3c: THE PREDICATE MOVED INTO CORE, with the assembler's twin of it.
// Since `fieldRange` a target may declare several 4-byte absolute rows (x86_64:
// `abs32`, `abs32s`, `addr32`), so "the first Linear absolute row of that width"
// stopped being an answer; `addressSlotRelocationKind` (core/types/
// target_schema.hpp) picks by RANGE, and both this scan and the assembler's
// `absoluteRelocKind` ask it, so the two can no longer disagree.
[[nodiscard]] inline std::optional<RelocationKind>
absolutePointerRelocKind(TargetSchema const& targetSchema,
                         std::uint8_t        widthBytes) {
    return addressSlotRelocationKind(targetSchema.relocations(), widthBytes);
}

}  // namespace dss::linker
