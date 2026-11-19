#pragma once

#include "core/types/alignment.hpp"

#include <cstdint>
#include <optional>

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
// A declared section alignment is a CORRECTNESS INPUT. Each reader states an
// item's alignment as its section's (and the linker narrows it by the item's
// offset in the section), and the merge places the item there: C's `_Alignas`
// is a guarantee the program observes, so an alignment read as less than the
// producer declared is a miscompile, not a layout difference. So every
// alignment the `Alignment` newtype holds -- each power of two up to
// `Alignment::kMaxBytes` -- is carried as declared, and a field that is no
// such alignment (a byte count that is not a power of two, which the gABI
// does not allow, or an exponent or count past 2^31) is NO ANSWER (`nullopt`):
// the reader refuses the object by name rather than place its bytes at an
// alignment nothing declared.
//
// ★ UNTIL P69 BOTH HELPERS DEGRADED EVERY ALIGNMENT ABOVE 256 TO ONE BYTE, on
// the argument that a declared alignment is "a re-layout hint, never a
// correctness input". ✔MEASURED 2026-10-15 (the windows-x86_64 finals of the
// re-review of fold 3b, and a probe with the same dsscp): a DSS PE object's
// `_Alignas(4096) char v;` -- a `.bss` definition its section declares 2**12
// -- linked by DSS landed at an address that is not a multiple of 4096, with
// its object alone or beside a common it outranks, while link.exe placed the
// same object at 4096 in the same run (`TentativeDefinitionsNative`, its cl
// arm). 256 was the newtype's old ceiling, kept here as a policy
// after P63 lifted the newtype's to 2^31; mingw-w64 gcc and clang emit
// `IMAGE_SCN_ALIGN_*` classes up to 8192, so the degrade met real input.

namespace dss::link::format {

// The policy kernel, for the two formats that already speak in exponents
// (Mach-O directly, PE/COFF after its class-field arithmetic). `log2 == 0` is
// alignment 1 -- "no constraint". An exponent past 31 is no alignment an
// `Alignment` holds: nullopt, and the reader refuses it by name.
[[nodiscard]] constexpr std::optional<Alignment>
foreignSectionAlignmentFromLog2(std::uint32_t log2) noexcept {
    if (log2 > 31u) return std::nullopt;
    return Alignment::fromBytes(std::uint32_t{1} << log2);
}

// The policy kernel for the byte-count encoding (ELF `sh_addralign`).
//
// ⚠ NOT `countr_zero` ON THE WAY IN. `sh_addralign` is a free-form integer and
// a non-power-of-two one has NO log2 -- 24 would become exponent 3, i.e. 8
// bytes, a plausible-looking wrong answer rather than a refusal. This is the
// same trap the WRITE side names in `macho.cpp`'s `sectionAlignLog2`, met from
// the other direction, so the pow2 test is `fromBytes`'s and not a caller's.
// 0 and 1 both mean "no constraint" in the gABI, which allows only those and
// powers of two.
[[nodiscard]] constexpr std::optional<Alignment>
foreignSectionAlignmentFromByteCount(std::uint64_t bytes) noexcept {
    if (bytes <= 1u) return Alignment{};
    if (bytes > Alignment::kMaxBytes) return std::nullopt;
    return Alignment::fromBytes(static_cast<std::uint32_t>(bytes));
}

} // namespace dss::link::format
