#pragma once

#include "core/types/target_schema.hpp"

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────
// [[D-LK-AARCH64-CALL26-BEYOND-RANGE-HAS-NO-VENEER]]
// THE REACH OF A LINK-TIME BRANCH FIELD, AS DATA
// ─────────────────────────────────────────────────────────────────────
//
// ★★★ THE SAME MOVE THE ASSEMBLER ALREADY MADE, ONE TIER UP, AND FOR THE SAME
// REASON. `walker_util::blockRelFieldGeometry` exists because the assembler's
// range check, its patch write and its escape election each carried their own
// copy of one field's lsb / width / scale, and a disagreement between two of
// them emits a VALID INSTRUCTION WITH THE WRONG DISPLACEMENT. The link tier is
// one step behind that: `applyExecRelocations`'s `Aarch64Call26` arm spells the
// same field as a literal `26` and a bare `>> 2`, and the veneer pass that
// decides whether a call can reach its callee AT ALL needs the identical two
// numbers. Two components, one field, two spellings — so the numbers live here
// once and both read them.
//
// ⚠ `fieldBits == 0` MEANS "NOT A BOUNDED PC-RELATIVE BRANCH", and it is the
// answer for every formula that is not one: a `Linear` displacement is width-
// checked against its row's own `widthBytes`, a page-relative or GOT-relative
// formula is not a branch at all, and none of them can be rescued by aiming
// somewhere nearer. Enumerating every kind rather than writing a `default`
// is deliberate — `-Werror=switch` then makes a NEW formula a decision
// somebody has to make here rather than one that silently answers "no".
namespace dss::link {

struct BranchRelocGeometry {
    std::uint8_t fieldBits;  // signed field width; 0 = not a bounded branch
    std::uint8_t scaleLog2;  // displacement = (S + A - P) >> scaleLog2
    std::uint8_t lsb;        // first bit of the field inside its 32-bit word
};

[[nodiscard]] constexpr BranchRelocGeometry
branchRelocGeometry(RelocFormulaKind k) noexcept {
    switch (k) {
        // AArch64 `BL` / `B` — R_AARCH64_CALL26 and JUMP26 share this formula:
        // `(S + A - P) >> 2`, signed 26 bits at bits 0..25. ±128 MiB.
        case RelocFormulaKind::Aarch64Call26:
            return BranchRelocGeometry{ 26, 2, 0 };
        // Not branches, and not rescuable by standing nearer:
        //   * Linear            — an absolute or PC-relative datum, width-checked
        //                         against the row's own `widthBytes`;
        //   * the ADRP family   — PAGE-relative, a formula whose value is
        //                         `(S>>12) - (P>>12)` and not a displacement at
        //                         all (which is exactly why it cannot be an
        //                         escape for a branch either);
        //   * the lo12 / GOT    — an addressing-mode half, never a transfer.
        case RelocFormulaKind::Linear:
        case RelocFormulaKind::Aarch64AdrPrelPgHi21:
        case RelocFormulaKind::Aarch64AddAbsLo12:
        case RelocFormulaKind::Aarch64TprelAddHi12:
        case RelocFormulaKind::Aarch64AdrGotPage:
        case RelocFormulaKind::Aarch64Ld64GotLo12:
        case RelocFormulaKind::X86_64GotPcRel:
            return BranchRelocGeometry{ 0, 0, 0 };
    }
    // Enum-drift backstop: a formula added without a row here answers "not a
    // branch", which is SAFE (nothing is rescued, the existing refusal stands)
    // but is meant to be caught at build time by `-Werror=switch` above.
    return BranchRelocGeometry{ 0, 0, 0 };
}

[[nodiscard]] constexpr std::int64_t
branchRelocFieldMin(BranchRelocGeometry g) noexcept {
    return g.fieldBits == 0 ? 0 : -(std::int64_t{1} << (g.fieldBits - 1));
}
[[nodiscard]] constexpr std::int64_t
branchRelocFieldMax(BranchRelocGeometry g) noexcept {
    return g.fieldBits == 0 ? -1 : (std::int64_t{1} << (g.fieldBits - 1)) - 1;
}

// How far, in BYTES, this formula's field can reach forward.
[[nodiscard]] constexpr std::int64_t
branchRelocByteReach(RelocFormulaKind k) noexcept {
    auto const g = branchRelocGeometry(k);
    return branchRelocFieldMax(g) << g.scaleLog2;
}

}  // namespace dss::link
