#pragma once

#include "core/types/alignment.hpp"

#include <cstdint>

// ── ONE POLICY FOR A FOREIGN OBJECT'S DECLARED SECTION ALIGNMENT ───────────
//
// D-FORMAT-MACHO-SECTION-ALIGN-EMITTED-RAW-NOT-LOG2 (the read-side half of the
// "is this field an exponent or a count" sweep that row prescribes).
//
// Every object READER faces the same question about the section header it is
// parsing: the producer declared an alignment for these bytes, what does DSS
// carry it as? The ENCODING differs per format and rightly belongs to that
// format's reader --
//
//     ELF     `sh_addralign`                    a raw BYTE COUNT
//     Mach-O  `section_64.align`                a LOG2 EXPONENT
//     PE/COFF `IMAGE_SCN_ALIGN_*BYTES`          (log2 + 1) << 20, class 0 = 16
//
// -- but the POLICY is one fact, and before this header it was written out
// twice in prose and MISSING the third time. ✔MEASURED 2026-08-27: the word
// `Alignment` did not appear in `coff_object_reader.cpp` at all, and neither
// `AssembledData` construction site set `.alignment`, so a COFF `.rdata`
// declaring `IMAGE_SCN_ALIGN_32BYTES` -- which is exactly what BOTH
// mingw-gcc and clang emit for an `_Alignas(32)` object, measured -- arrived
// in the merge at the newtype's default of ONE byte. ⓘ Where that becomes
// OBSERVABLE is narrower than it sounds and is measured rather than argued;
// `coff_object_reader.cpp`'s own note carries the case, and
// `examples/c/staticlib_alignas_carry` is its runtime witness.
//
// ★ THAT IS THE SHAPE THE PARENT ROW WARNS ABOUT: the write side already had
// this defect (`__text` alone taking its own view of `section_64.align`) and it
// was closed by routing every arm through ONE `sectionAlignLog2`. The read side
// had the same duplication with the same hole in it. A fact with an owner does
// not get a second owner; here it had three owners and one absentee.
//
// ── THE POLICY ────────────────────────────────────────────────────────────
//
// A declared section alignment is a RE-LAYOUT HINT, never a correctness input
// on read-back: the merge re-lays-out every item it takes in, so this value
// constrains where an item MAY be placed, and nothing downstream trusts it as a
// statement about where the item already IS. Consequently a value the
// `Alignment` newtype cannot represent (above its 256-byte ceiling, or -- for
// the byte-count encoding -- not a power of two at all) degrades to byte
// alignment rather than failing the read: refusing an object over a hint we do
// not depend on would reject input every reference linker accepts, and 256
// bytes already covers every alignment any producer in this pipeline emits.
//
// ⚠ The degrade direction is deliberate and is the ONLY safe one available
// here: it can only make the merge place an item MORE freely than the producer
// asked, which is visible as a layout difference. The opposite reflex --
// silently substituting some "reasonable" larger alignment for an
// unrepresentable one -- would let an over-aligned item ride at an alignment
// nothing declared, which is a fact about our output that is false.

namespace dss::link::format {

// The policy kernel, for the two formats that already speak in exponents
// (Mach-O directly, PE/COFF after its class-field arithmetic).
//
// `log2 == 0` is alignment 1 -- "no constraint" -- and is the same answer as
// the degrade arm, deliberately: a producer that declared byte alignment and a
// producer that declared something we cannot represent both leave the merge
// free to place the item anywhere.
[[nodiscard]] constexpr Alignment
foreignSectionAlignmentFromLog2(std::uint32_t log2) noexcept {
    // 2^8 = 256 is the newtype's ceiling; a wild exponent (a corrupt or
    // simply larger-than-we-model field) takes the same degrade arm.
    if (log2 == 0u || log2 > 8u) return Alignment{};
    return Alignment::fromBytes(std::uint32_t{1} << log2)
        .value_or(Alignment{});
}

// The policy kernel for the byte-count encoding (ELF `sh_addralign`).
//
// ⚠ NOT `countr_zero` ON THE WAY IN. `sh_addralign` is a free-form integer and
// a non-power-of-two one has NO log2 -- 24 would become exponent 3, i.e. 8
// bytes, a plausible-looking wrong answer rather than a refusal. This is the
// same trap the WRITE side names in `macho.cpp`'s `sectionAlignLog2`, met from
// the other direction, so the pow2 test is `fromBytes`'s and not a caller's.
[[nodiscard]] constexpr Alignment
foreignSectionAlignmentFromByteCount(std::uint64_t bytes) noexcept {
    // 0 and 1 both mean "no constraint" in the gABI.
    if (bytes <= 1u || bytes > 256u) return Alignment{};
    return Alignment::fromBytes(static_cast<std::uint32_t>(bytes))
        .value_or(Alignment{});
}

} // namespace dss::link::format
