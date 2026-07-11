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
    // D-CSUBSET-THREAD-LOCAL (TLS C1): the thread-local section pair —
    // exactly the "future kind addition (e.g. a TLS section)" the
    // header docblock anticipated. Each format JSON maps them to its
    // native names (ELF `.tdata`/`.tbss`; PE `.tls`; Mach-O
    // `__DATA,__thread_data`/`__thread_bss`).
    ThreadData = 12, // initialised thread-local TEMPLATE data (.tdata)
    ThreadBss  = 13, // zero-fill thread-local TEMPLATE extent (.tbss)
    // D-CSUBSET-THREAD-LOCAL (TLS C4, Mach-O TLV): the thread-local
    // VARIABLE-DESCRIPTOR section. Mach-O uniquely reaches a thread-local
    // object through a per-variable 3-word `tlv_descriptor` in
    // `__DATA,__thread_vars` (S_THREAD_LOCAL_VARIABLES) — the descriptors
    // are WRITER-synthesized (never `AssembledData` producer output), so
    // this kind is NOT a `DataSectionKind` (`dataSectionKindOf` returns
    // nullopt for it). ELF/PE reach TLS with a tp-relative offset and
    // declare no such section. It exists only so the Mach-O format JSON
    // can name the section + its S_THREAD_LOCAL_VARIABLES flag config-side
    // (like the tdata/tbss rows), keeping the writer free of a hardcoded
    // section flag.
    ThreadVars = 14, // thread-local variable descriptors (Mach-O __thread_vars)
};

inline constexpr EnumNameTable<SectionKind, 15> kSectionKindTable{{{
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
    { SectionKind::ThreadData, "tdata"      },
    { SectionKind::ThreadBss,  "tbss"       },
    { SectionKind::ThreadVars, "tvars"      },
}}};

[[nodiscard]] constexpr std::string_view
sectionKindName(SectionKind k) noexcept {
    return kSectionKindTable.name(k);
}
[[nodiscard]] constexpr std::optional<SectionKind>
sectionKindFromName(std::string_view s) noexcept {
    return kSectionKindTable.fromName(s);
}

// Narrow subset of `SectionKind` that a producer can legitimately
// emit via `AssembledData`. Closes D-LK4-RODATA-SECTION-NARROW.
//
// Of the 14 `SectionKind` values, 9 are walker-synthesized
// (`Text` = executable code; `Symtab`/`Strtab`/`ShStrtab` = symbol
// + name tables; `RelocTable` = relocation entries; `Dynamic` =
// dynamic linking metadata; `Note` = vendor notes; `Debug` =
// DWARF/CodeView; `Custom` = format-specific anything). A producer
// constructing `AssembledData{symbol, SectionKind::Symtab, ...}`
// is semantically nonsense — the assembler doesn't emit symbol
// tables; the linker walker synthesizes them.
//
// The FIVE valid producer-emittable kinds:
//   * `Rodata` — read-only initialised data (string literals,
//                const arrays, vtables).
//   * `Data`   — read-write initialised data (mutable globals).
//   * `Bss`    — zero-fill mutable data (uninitialised globals).
//   * `Tdata`  — initialised THREAD-LOCAL template data: the
//                per-thread-copied initial image of a
//                `thread_local T g = init;` (D-CSUBSET-THREAD-LOCAL).
//   * `Tbss`   — zero-fill THREAD-LOCAL template extent: the
//                per-thread zero-init span of a `thread_local T g;`.
//
// The walker still keys on `SectionKind` (the full enum). The
// `toSectionKind()` conversion is total — every `DataSectionKind`
// value maps to its corresponding `SectionKind`. The reverse
// direction is partial: `dataSectionKindOf()` returns nullopt for
// the 9 walker-synthesized kinds.
enum class DataSectionKind : std::uint8_t {
    Rodata = static_cast<std::uint8_t>(SectionKind::Rodata),
    Data   = static_cast<std::uint8_t>(SectionKind::Data),
    Bss    = static_cast<std::uint8_t>(SectionKind::Bss),
    Tdata  = static_cast<std::uint8_t>(SectionKind::ThreadData),
    Tbss   = static_cast<std::uint8_t>(SectionKind::ThreadBss),
};

[[nodiscard]] constexpr SectionKind
toSectionKind(DataSectionKind d) noexcept {
    return static_cast<SectionKind>(static_cast<std::uint8_t>(d));
}

[[nodiscard]] constexpr std::optional<DataSectionKind>
dataSectionKindOf(SectionKind k) noexcept {
    switch (k) {
        case SectionKind::Rodata:     return DataSectionKind::Rodata;
        case SectionKind::Data:       return DataSectionKind::Data;
        case SectionKind::Bss:        return DataSectionKind::Bss;
        case SectionKind::ThreadData: return DataSectionKind::Tdata;
        case SectionKind::ThreadBss:  return DataSectionKind::Tbss;
        default:                      return std::nullopt;
    }
}

// D-CSUBSET-THREAD-LOCAL (TLS C1, audit fold M-3): the ONE zero-fill
// predicate. `Bss` and `Tbss` share the "reserves memory extent, stores
// NO file bytes" wire semantics (size lives in `reservedSize` /
// sh_size, `bytes` stays EMPTY by invariant); every other producer
// kind is file-backed. Before Tbss existed, three chokepoints tested
// `== DataSectionKind::Bss` EXACTLY (`AssembledData::sizeInSection`,
// `buildExecDataSection`'s layout branch, `validateAssembledData`'s
// no-bytes invariant) — each would have silently mis-handled a Tbss
// item (treating it as file-backed reads 0 bytes where reservedSize
// was the real span). All three now route through this predicate so
// a future zero-fill kind lands at ONE point of truth.
[[nodiscard]] constexpr bool isZeroFill(DataSectionKind d) noexcept {
    return d == DataSectionKind::Bss || d == DataSectionKind::Tbss;
}

// Round-trip pins (silent-failure F-1 + type-design Q5 fold,
// 8-agent audit on D-LK4-RODATA-SECTION-NARROW). `toSectionKind`
// is a raw `static_cast<SectionKind>(uint8_t)` — fast, but the
// numeric round-trip would silently break if a future maintainer
// rebased the explicit values on either enum without touching the
// other. These compile-time assertions pin both the totality of
// `toSectionKind` and the round-trip via `dataSectionKindOf`,
// catching drift at build time before any walker mis-routes bytes.
static_assert(toSectionKind(DataSectionKind::Rodata) == SectionKind::Rodata);
static_assert(toSectionKind(DataSectionKind::Data)   == SectionKind::Data);
static_assert(toSectionKind(DataSectionKind::Bss)    == SectionKind::Bss);
static_assert(toSectionKind(DataSectionKind::Tdata)  == SectionKind::ThreadData);
static_assert(toSectionKind(DataSectionKind::Tbss)   == SectionKind::ThreadBss);
static_assert(dataSectionKindOf(SectionKind::Rodata) == DataSectionKind::Rodata);
static_assert(dataSectionKindOf(SectionKind::Data)   == DataSectionKind::Data);
static_assert(dataSectionKindOf(SectionKind::Bss)    == DataSectionKind::Bss);
static_assert(dataSectionKindOf(SectionKind::ThreadData) == DataSectionKind::Tdata);
static_assert(dataSectionKindOf(SectionKind::ThreadBss)  == DataSectionKind::Tbss);
// The zero-fill predicate covers EXACTLY the two no-file-bytes kinds.
static_assert(!isZeroFill(DataSectionKind::Rodata));
static_assert(!isZeroFill(DataSectionKind::Data));
static_assert( isZeroFill(DataSectionKind::Bss));
static_assert(!isZeroFill(DataSectionKind::Tdata));
static_assert( isZeroFill(DataSectionKind::Tbss));

[[nodiscard]] constexpr std::string_view
dataSectionKindName(DataSectionKind d) noexcept {
    return sectionKindName(toSectionKind(d));
}

[[nodiscard]] constexpr std::optional<DataSectionKind>
dataSectionKindFromName(std::string_view s) noexcept {
    if (s == "rodata") return DataSectionKind::Rodata;
    if (s == "data")   return DataSectionKind::Data;
    if (s == "bss")    return DataSectionKind::Bss;
    if (s == "tdata")  return DataSectionKind::Tdata;
    if (s == "tbss")   return DataSectionKind::Tbss;
    return std::nullopt;
}

} // namespace dss
