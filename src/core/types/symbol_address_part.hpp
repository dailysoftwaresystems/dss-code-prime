#pragma once

#include "core/types/enum_name_table.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace dss {

// ★★ WHICH PART OF A SYMBOL'S ADDRESS AN OPERAND DENOTES (P68 round 9, the
// aarch64 twins of D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER).
//
// An arm64 program addresses a symbol in two halves: `adrp` takes the 4 KiB
// PAGE of its address and an `add` or a load/store takes the OFFSET within that
// page. Which half an operand names is the program's statement — gas and
// clang-ELF spell it `:pg_hi21:msg` (or a bare `adrp` operand) and `:lo12:msg`,
// clang-Darwin `msg@PAGE` and `msg@PAGEOFF` (✔MEASURED 2026-09-23, each
// refusing the other's spelling) — and it decides which relocation the field
// takes. It is ISA vocabulary, not a spelling: the DIALECT maps spellings onto
// it per object-format kind, and a target's encoding variant states which part
// its symbolic field encodes (`guard.symbolPart`), so neither side names the
// other's words. It rides the LIR operand (`LirOperand::symbolAddressPart`), so
// the one variant matcher sees it wherever a symbol reaches an encoding.
//   * `Whole`      — the address itself: `adr x0, msg`, `.quad msg`, x86
//                    `msg(%rip)`. The default, and the only part a variant
//                    that states none accepts.
//   * `Page`       — its page, `(S + A) >> 12` relative to the PC's page.
//   * `PageOffset` — its offset in the page, `(S + A) & 0xFFF`.
enum class SymbolAddressPart : std::uint8_t {
    Whole      = 0,
    Page       = 1,
    PageOffset = 2,
};

inline constexpr EnumNameTable<SymbolAddressPart, 3> kSymbolAddressPartTable{{{
    { SymbolAddressPart::Whole,      "whole"      },
    { SymbolAddressPart::Page,       "page"       },
    { SymbolAddressPart::PageOffset, "pageOffset" },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kSymbolAddressPartTable);

[[nodiscard]] constexpr std::string_view
symbolAddressPartName(SymbolAddressPart p) noexcept {
    return kSymbolAddressPartTable.name(p);
}
[[nodiscard]] constexpr std::optional<SymbolAddressPart>
symbolAddressPartFromName(std::string_view s) noexcept {
    return kSymbolAddressPartTable.fromName(s);
}

// The part in words, for a diagnostic ("the page offset of ..."). The table's
// names are config vocabulary (`pageOffset`); a sentence wants prose.
[[nodiscard]] constexpr std::string_view
symbolAddressPartPhrase(SymbolAddressPart p) noexcept {
    switch (p) {
    case SymbolAddressPart::Whole:      return "whole address";
    case SymbolAddressPart::Page:       return "page";
    case SymbolAddressPart::PageOffset: return "page offset";
    }
    return "part";
}

} // namespace dss
