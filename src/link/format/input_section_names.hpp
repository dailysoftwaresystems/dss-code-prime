#pragma once

// ★★★ THE ROW AN INPUT SECTION READS AS (P69 fold 3e, the orchestrator's ledger 577) -- the ONE home of "an input
// section a reference toolchain emits is mapped to its output kind by the format's DATA, and a writer never meets a
// reader-only row". Every relocatable-object reader asks it (ELF's, COFF's, Mach-O's), and nothing else maps an input
// section's name to a row.
//
// Until the fold each reader held its own rule in code: ELF's longest `<row>.` prefix (twice, once for a body and once
// for an anonymous gap), COFF's cut at the first `$` and then the same `.` prefix, the Win64 unwind families' `$` or
// `.` suffix, and Mach-O's exact (segment, section) pair alone -- so every Apple clang object holding a string literal
// (`__TEXT,__cstring`) was refused, K_ObjectSectionNotModelled, and a fourth reader of a format with its own
// convention would have written a fifth rule. The conventions are now the document's (`inputSectionNames`):
//   * `subsectionSeparators` -- the separators by which a producer names a part of a row's section: ELF `.`
//     (`.rodata.str1.1`, `.text.startup`), COFF `$` and `.` (`.text$mn`, `.rdata$r`; mingw-w64's `.text.startup`);
//   * `readAs` -- an input section a reference toolchain emits that no writer of the format writes, and the row it
//     reads as (Apple clang's `__TEXT,__cstring` and `__TEXT,__literal{4,8,16}`, read as `__TEXT,__const`). An entry
//     is never a row (the loader refuses one that is), so a writer, which resolves its rows by kind, never meets it.
// The format's WIRE rules stay its reader's: ELF's sh_flags fallback for a section no name maps, COFF's split of
// `.rdata` by whether the section carries relocations, Mach-O's MH_SUBSECTIONS_VIA_SYMBOLS.

#include "link/object_format_schema.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

namespace dss::link::format {

// Whether `name` is `base` itself or a part of it named under one of the document's separators
// (`<base><separator><anything>`).
[[nodiscard]] inline bool isInputSubsectionOf(ObjectFormatSchema const& schema, std::string_view name,
                                              std::string_view base) noexcept {
    if (name == base) return true;
    if (name.size() <= base.size() || name.substr(0, base.size()) != base) return false;
    InputSectionNames const* const names = schema.inputSectionNames();
    if (names == nullptr) return false;
    std::string_view const rest = name.substr(base.size());
    for (auto const& sep : names->subsectionSeparators) {
        if (rest.size() > sep.size() && rest.substr(0, sep.size()) == sep) return true;
    }
    return false;
}

// The index into `schema.sections()` of the row the input section (`segment`, `name`) reads as:
//   (1) the first row of that (segment, name);
//   (2) else the row the document's `readAs` names for it;
//   (3) else the row of that segment whose name is the LONGEST that the section's is a part of (`isInputSubsectionOf`)
//       -- `.data.rel.ro.local` is `.data.rel.ro`'s, not `.data`'s; of two rows of one name, the first.
// nullopt when none: a section the format's rows do not carry, which its reader's own rule decides.
// `segment` is empty on a format whose sections have none (ELF, COFF).
[[nodiscard]] inline std::optional<std::size_t> inputSectionRow(ObjectFormatSchema const& schema,
                                                                std::string_view segment, std::string_view name) {
    auto const rows = schema.sections();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].segment == segment && rows[i].name == name) return i;
    }
    if (InputSectionNames const* const names = schema.inputSectionNames(); names != nullptr) {
        for (auto const& alias : names->readAs) {
            if (alias.segment != segment || alias.name != name) continue;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (rows[i].segment == alias.rowSegment && rows[i].name == alias.rowName) return i;
            }
        }
    }
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].segment != segment || rows[i].name == name) continue;
        if (!isInputSubsectionOf(schema, name, rows[i].name)) continue;
        if (!best.has_value() || rows[i].name.size() > rows[*best].name.size()) best = i;
    }
    return best;
}

}  // namespace dss::link::format
