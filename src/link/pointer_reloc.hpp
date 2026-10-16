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
[[nodiscard]] inline std::optional<RelocationKind>
absolutePointerRelocKind(TargetSchema const& targetSchema,
                         std::uint8_t        widthBytes) {
    for (auto const& r : targetSchema.relocations()) {
        if (r.formulaKind == RelocFormulaKind::Linear && r.widthBytes == widthBytes
            && !r.pcRelative && !r.tls && !r.imageRelative) {
            return r.kind;
        }
    }
    return std::nullopt;
}

}  // namespace dss::linker
