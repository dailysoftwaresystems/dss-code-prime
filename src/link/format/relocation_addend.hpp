#pragma once

// ★★★ WHERE A RELOCATABLE OBJECT KEEPS A RELOCATION's ADDEND — THE ONE OWNER.
//
// Two storages exist, and which one a format uses is the format's declared fact
// (`relocationAddends`, `RelocationAddendStorage`):
//
//   * EXPLICIT (ELF RELA): the record's own addend column holds the psABI's
//     FULL implicit addend — DSS's `Relocation::addend` plus the target kind's
//     `addendBias` — because a foreign linker adds no bias of its own
//     (R_X86_64_PC32 is S + A - P);
//   * IN PLACE (COFF, Mach-O): the record has no addend column and the PATCHED
//     FIELD holds DSS's addend itself — the format's own formula carries the
//     bias the target declares (IMAGE_REL_AMD64_REL32 is S + field - (P + 4)).
//     ✔MEASURED 2026-09-23: clang 18.1.3 writes `fc ff ff ff` into the field of
//     `movl $5, counter(%rip)` (COFF REL32 and Mach-O SIGNED_4 alike), and
//     mingw gas writes the symbol's section offset less 4 there.
//
// ★★ EVERY WRITER STAMPS THROUGH `placeRelocationAddend` AND EVERY READER READS
// THROUGH `recoverRelocationAddend`, so a writer and a reader can never
// disagree about where the addend is. They did: the COFF and Mach-O readers
// took every `.text` addend as 0 "because the writer rejects a non-zero `.text`
// addend" — true of DSS's writer alone — and ✔MEASURED 2026-09-23 a
// clang-compiled COFF object linked by DSS ran to 32 where the mingw link of
// the same objects ran to 42.
//
// ⚠ AN IN-PLACE FORMAT CAN HOLD AN ADDEND ONLY IN A PLAIN BYTE FIELD. A
// non-linear kind (an AArch64 instruction immediate: BRANCH26, PAGE21,
// PAGEOFF12) has no field for one — the format would need a separate addend
// record this build does not write — so a non-zero addend there is refused, and
// a reader takes such a field's bits as instruction bits, never as an addend.

#include "asm/asm.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>

namespace dss::link::format {

// What a WRITER does with a relocation's addend: for an explicit format the
// value its record's addend column carries (`column`); for an in-place format
// the addend has been written into `field` and `column` is empty.
struct PlacedRelocationAddend {
    std::optional<std::int64_t> column;
};

// ★ A LINEAR KIND's FIELD IS `widthBytes` LITTLE-ENDIAN BYTES — the plain byte
// field an in-place addend can occupy.
[[nodiscard]] inline bool relocationFieldHoldsAnAddend(
        TargetRelocationInfo const& tri) noexcept {
    return tri.formulaKind == RelocFormulaKind::Linear
        && (tri.widthBytes == 4u || tri.widthBytes == 8u);
}

// Place `addend` where format `storage` keeps it. `field` is the patched field
// (exactly `tri.widthBytes` bytes for a linear kind; ignored for an explicit
// format). Returns why it cannot be placed: a non-zero addend on a field with
// no room for one, or a value the field's width cannot carry.
[[nodiscard]] inline std::expected<PlacedRelocationAddend, std::string>
placeRelocationAddend(RelocationAddendStorage      storage,
                      TargetRelocationInfo const&  tri,
                      std::int64_t                 addend,
                      std::span<std::uint8_t>      field) {
    if (storage == RelocationAddendStorage::Explicit) {
        return PlacedRelocationAddend{addend + tri.addendBias};
    }
    if (!relocationFieldHoldsAnAddend(tri)) {
        if (addend == 0) return PlacedRelocationAddend{};
        return std::unexpected(std::format(
            "relocation '{}' patches an instruction field, which cannot hold "
            "an addend in place, and this relocation carries {}", tri.name,
            addend));
    }
    if (field.size() != tri.widthBytes) {
        return std::unexpected(std::format(
            "relocation '{}' patches a {}-byte field but {} byte(s) were "
            "offered", tri.name, tri.widthBytes, field.size()));
    }
    if (tri.widthBytes == 4u
        && (addend < -2147483648LL || addend > 4294967295LL)) {
        return std::unexpected(std::format(
            "relocation '{}' carries addend {}, which a 4-byte in-place field "
            "cannot hold", tri.name, addend));
    }
    auto const bits = static_cast<std::uint64_t>(addend);
    for (std::size_t b = 0; b < field.size(); ++b) {
        field[b] = static_cast<std::uint8_t>((bits >> (8u * b)) & 0xFFu);
    }
    return PlacedRelocationAddend{};
}

// The addend a relocation READ from an object carries: from the record's addend
// column (explicit) or from the patched field (in place — sign-extended from the
// field's width). An in-place field of a non-linear kind holds instruction bits
// and carries no addend.
[[nodiscard]] inline std::expected<std::int64_t, std::string>
recoverRelocationAddend(RelocationAddendStorage          storage,
                        TargetRelocationInfo const&      tri,
                        std::optional<std::int64_t>      column,
                        std::span<std::uint8_t const>    field) {
    if (storage == RelocationAddendStorage::Explicit) {
        if (!column.has_value()) {
            return std::unexpected(std::format(
                "relocation '{}' was read from a format whose records carry an "
                "addend column, and this record's was not supplied", tri.name));
        }
        return *column - tri.addendBias;
    }
    if (!relocationFieldHoldsAnAddend(tri)) return std::int64_t{0};
    if (field.size() != tri.widthBytes) {
        return std::unexpected(std::format(
            "relocation '{}' patches a {}-byte field and only {} byte(s) of it "
            "lie inside the section", tri.name, tri.widthBytes, field.size()));
    }
    std::uint64_t raw = 0;
    for (std::size_t b = 0; b < field.size(); ++b) {
        raw |= static_cast<std::uint64_t>(field[b]) << (8u * b);
    }
    if (field.size() == 4u) {
        return static_cast<std::int64_t>(static_cast<std::int32_t>(
            static_cast<std::uint32_t>(raw)));
    }
    return static_cast<std::int64_t>(raw);
}

} // namespace dss::link::format
