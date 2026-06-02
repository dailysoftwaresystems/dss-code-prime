#pragma once

#include "core/export.hpp"
#include "core/types/target_schema.hpp"   // EnumNameTable<E,N>

#include <cstdint>
#include <optional>
#include <string_view>

// Canonical section taxonomy — format-blind names the substrate
// engine speaks. Each format JSON declares which platform-native
// section a given `SectionKind` maps to (e.g. ELF `.text` / PE
// `.text` / Mach-O `__TEXT,__text` all map to `SectionKind::Text`).
// The engine reads the kind; per-format JSON owns the name + flags.
//
// **Cross-tier vocabulary**: this header lives under `core/types/`
// rather than `src/link/` so the upstream assembler (`src/asm/`)
// can tag its `AssembledData` outputs with the same kind enum the
// downstream linker walkers consume. Both layers MUST speak the
// same section vocabulary so a future kind addition (e.g. a TLS
// section) lands at one point of truth — not duplicated as an
// `AssembledDataKind` on the asm side and a `SectionKind` on the
// link side. Extracted from `link/object_format_schema.hpp` at the
// D-LK4-RODATA-SUBSTRATE slice when `AssembledData` first needed the
// vocabulary.
//
// **Adding a new kind**: append to the enum AND to
// `kSectionKindTable` AND to every format JSON's `sections[]` rows
// that need it. The JSON-side `kind` field is the on-disk
// vocabulary; the C++ enum is its in-memory mirror. Drift between
// them is caught by the loader's `sectionKindFromName` lookup
// returning `nullopt` (fail-loud).
namespace dss {

enum class SectionKind : std::uint8_t {
    Text       = 0,  // executable code
    Rodata     = 1,  // read-only data
    Data       = 2,  // initialised mutable data
    Bss        = 3,  // zero-initialised mutable data
    Symtab     = 4,  // symbol table
    Strtab     = 5,  // symbol-name string table
    ShStrtab   = 6,  // section-name string table (ELF .shstrtab;
                     // distinct from Strtab — the consumer code
                     // path is "find names of OTHER sections" vs
                     // "find symbol names")
    RelocTable = 7,  // relocation entries
    Dynamic    = 8,  // ELF .dynamic / PE .idata / Mach-O LC_DYLD_INFO
    Note       = 9,  // build-id / vendor notes
    Debug      = 10, // DWARF / CodeView debug info
    Custom     = 11, // anything else the format JSON names
};

inline constexpr EnumNameTable<SectionKind, 12> kSectionKindTable{{{
    { SectionKind::Text,       "text"       },
    { SectionKind::Rodata,     "rodata"     },
    { SectionKind::Data,       "data"       },
    { SectionKind::Bss,        "bss"        },
    { SectionKind::Symtab,     "symtab"     },
    { SectionKind::Strtab,     "strtab"     },
    { SectionKind::ShStrtab,   "shstrtab"   },
    { SectionKind::RelocTable, "reloc"      },
    { SectionKind::Dynamic,    "dynamic"    },
    { SectionKind::Note,       "note"       },
    { SectionKind::Debug,      "debug"      },
    { SectionKind::Custom,     "custom"     },
}}};

[[nodiscard]] constexpr std::string_view
sectionKindName(SectionKind k) noexcept {
    return kSectionKindTable.name(k);
}
[[nodiscard]] constexpr std::optional<SectionKind>
sectionKindFromName(std::string_view s) noexcept {
    return kSectionKindTable.fromName(s);
}

} // namespace dss
