#pragma once

#include "core/types/target_schema.hpp"

#include <cstdint>
#include <optional>

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
        //   * the one-word ADR  — PC-relative and bounded (±1 MiB), but an
        //                         ADDRESS, not a transfer: no veneer can stand
        //                         nearer to a datum on its behalf;
        //   * the scaled LDST lo12 — the low bits of an absolute address.
        case RelocFormulaKind::Linear:
        case RelocFormulaKind::Aarch64AdrPrelPgHi21:
        case RelocFormulaKind::Aarch64AddAbsLo12:
        case RelocFormulaKind::Aarch64TprelAddHi12:
        case RelocFormulaKind::Aarch64AdrGotPage:
        case RelocFormulaKind::Aarch64Ld64GotLo12:
        case RelocFormulaKind::X86_64GotPcRel:
        case RelocFormulaKind::Aarch64AdrPrelLo21:
        case RelocFormulaKind::Aarch64LdstAbsLo12:
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

// ─────────────────────────────────────────────────────────────────────
// [[D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH]]
// THE WINDOW A RELOCATION ROW'S FIELD CAN ENCODE, IN BYTES
// ─────────────────────────────────────────────────────────────────────
//
// The veneer pass asks one question of two different kinds of field: how far
// may the TARGET (S + A) lie from the PATCH SITE (P) before this row can no
// longer encode it? It asks it of the BRANCH being carried (`call26`: ±128 MiB)
// and of every field of the veneer BODY carrying it (ADRP: ±4 GiB), and the
// body's own reach is the intersection. One function, so the two answers cannot
// be computed two ways.
//
// ⚠ THE WINDOW IS ONE THAT IS ALWAYS ENCODABLE, WHATEVER P IS. A page-relative
// field encodes `((S+A) >> 12) - (P >> 12)`, which for a given byte distance
// varies by one page with P's offset inside its page; the window below is the
// one that fits for EVERY P, so the pass may under-use the last page of reach
// but can never place a field the applier then refuses.
//
// nullopt = the field is not bounded by distance from P at all: an absolute
// address or its low bits, a thread-pointer offset, or a field that addresses a
// GOT SLOT rather than S itself. Every formula is enumerated (no `default`), so
// `-Werror=switch` makes a new one a decision somebody takes here.
struct RelocReach {
    std::int64_t minDelta = 0;  // most negative encodable (S + A - P)
    std::int64_t maxDelta = 0;  // most positive encodable (S + A - P)
};

[[nodiscard]] constexpr std::optional<RelocReach>
relocFieldReach(TargetRelocationInfo const& row) noexcept {
    switch (row.formulaKind) {
        case RelocFormulaKind::Aarch64Call26: {
            auto const g     = branchRelocGeometry(row.formulaKind);
            auto const scale = std::int64_t{1} << g.scaleLog2;
            return RelocReach{branchRelocFieldMin(g) * scale,
                              branchRelocFieldMax(g) * scale};
        }
        case RelocFormulaKind::Aarch64AdrPrelPgHi21: {
            // Page delta in signed 21 bits: [-2^20, 2^20 - 1] pages. For a
            // byte distance d the page delta is floor(d/4096) or one more, so
            // every d in [-2^32, 2^32 - 4097] is encodable for EVERY P.
            constexpr std::int64_t kPage = 4096;
            constexpr std::int64_t kPages = std::int64_t{1} << 20;
            return RelocReach{-kPages * kPage, (kPages - 1) * kPage - 1};
        }
        case RelocFormulaKind::Aarch64AdrPrelLo21: {
            // S + A - P itself, unscaled, in signed 21 bits: ±1 MiB for every P.
            constexpr std::int64_t kHalf = std::int64_t{1} << 20;
            return RelocReach{-kHalf, kHalf - 1};
        }
        case RelocFormulaKind::Linear: {
            // value = S + A - P + addendBias, written in `widthBytes` bytes.
            // Absolute rows, and an 8-byte field, span the address space.
            if (!row.pcRelative || row.widthBytes == 0 || row.widthBytes >= 8)
                return std::nullopt;
            auto const half = std::int64_t{1} << (8 * row.widthBytes - 1);
            return RelocReach{-half - row.addendBias, half - 1 - row.addendBias};
        }
        case RelocFormulaKind::Aarch64AddAbsLo12:     // the low 12 bits of S + A
        case RelocFormulaKind::Aarch64TprelAddHi12:   // a thread-pointer offset
        case RelocFormulaKind::Aarch64AdrGotPage:     // addresses the GOT SLOT
        case RelocFormulaKind::Aarch64Ld64GotLo12:    // addresses the GOT SLOT
        case RelocFormulaKind::X86_64GotPcRel:        // addresses the GOT SLOT
        case RelocFormulaKind::Aarch64LdstAbsLo12:    // the low 12 bits of S + A
            return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace dss::link
