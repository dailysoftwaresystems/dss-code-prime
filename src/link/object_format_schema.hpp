#pragma once

#include "core/export.hpp"
#include "core/substrate/transparent_string_hash.hpp"
#include "core/types/grammar_schema.hpp"   // ConfigDiagnostic + LoadResult
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"    // EnumNameTable<E,N>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// `ObjectFormatSchema` (plan 14 §2.0 + LK4 substrate). JSON-configured
// object-format descriptor. Mirrors `TargetSchema` for the backend:
// an object format (ELF / PE/COFF / Mach-O / WASM / SPIR-V) is a
// JSON file in `src/dss-config/object-formats/<name>.format.json`,
// NOT C++ code. Adding a new format = drop a new `.format.json`;
// nothing in the linker substrate or engine changes.
//
// The substrate ships the closed-enum vocabulary (section kinds /
// symbol bindings/visibilities / format kinds) + the `relocations[]`
// taxonomy (the format-side half of plan 13 §2.6's reloc unifier:
// format-name → opaque-tag mapping, where the tag is the SAME tag
// the assembler stamps onto `Relocation::kind`).
//
// Per-format JSON rows (sections, dynamic-section entries, load
// commands, etc.) land in their respective LK* cycles — substrate
// holds the contracts.

namespace dss {

// ── Closed-enum vocabulary ────────────────────────────────────────

// One per shipped output format. Substrate-tier — adding a new
// format = JSON file + new enum entry + new walker arm (when the
// engine grows format-specific emission, anchored at LK1+).
//
// `Unknown` is slot 0 (the project's universal invalid-sentinel
// discipline — see `TargetEncodingShape::None`, `RelocationKind{}`,
// strong-ids). A default-constructed `LinkedImage{}` reports
// `format=Unknown`, NOT a spurious ELF identity.
enum class ObjectFormatKind : std::uint8_t {
    Unknown = 0,  // invalid sentinel; default-constructed images
    Elf     = 1,  // Linux + Android
    Pe      = 2,  // Windows + Windows-ARM64 (PE/COFF)
    MachO   = 3,  // macOS + iOS
    Wasm    = 4,  // Web / WASM runtime — enum slot reserved; engine +
                  // JSON arrive in plan 18
    Spirv   = 5,  // GPU shaders — enum slot reserved; engine + JSON
                  // arrive in plan 17
};

inline constexpr EnumNameTable<ObjectFormatKind, 6> kObjectFormatKindTable{{{
    { ObjectFormatKind::Unknown, "unknown" },
    { ObjectFormatKind::Elf,     "elf"     },
    { ObjectFormatKind::Pe,      "pe"      },
    { ObjectFormatKind::MachO,   "macho"   },
    { ObjectFormatKind::Wasm,    "wasm"    },
    { ObjectFormatKind::Spirv,   "spirv"   },
}}};

[[nodiscard]] constexpr std::string_view
objectFormatKindName(ObjectFormatKind k) noexcept {
    return kObjectFormatKindTable.name(k);
}
[[nodiscard]] constexpr std::optional<ObjectFormatKind>
objectFormatKindFromName(std::string_view s) noexcept {
    return kObjectFormatKindTable.fromName(s);
}

// Canonical section taxonomy — format-blind names the engine speaks.
// Each format JSON declares which platform-native section a given
// SectionKind maps to (e.g. ELF `.text` / PE `.text` / Mach-O
// `__TEXT,__text` all map to `SectionKind::Text`). The engine reads
// the kind; per-format JSON owns the name + flags.
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

// Symbol binding — visibility within the linker's symbol-resolution
// algorithm. Local symbols never resolve across translation units;
// Weak symbols defer to Global symbols of the same name.
enum class SymbolBinding : std::uint8_t {
    Local  = 0,
    Global = 1,
    Weak   = 2,
};

inline constexpr EnumNameTable<SymbolBinding, 3> kSymbolBindingTable{{{
    { SymbolBinding::Local,  "local"  },
    { SymbolBinding::Global, "global" },
    { SymbolBinding::Weak,   "weak"   },
}}};

[[nodiscard]] constexpr std::string_view
symbolBindingName(SymbolBinding b) noexcept {
    return kSymbolBindingTable.name(b);
}
[[nodiscard]] constexpr std::optional<SymbolBinding>
symbolBindingFromName(std::string_view s) noexcept {
    return kSymbolBindingTable.fromName(s);
}

// Symbol visibility — affects whether a symbol is exported to other
// images at runtime. Default = exported (subject to binding).
enum class SymbolVisibility : std::uint8_t {
    Default   = 0,
    Hidden    = 1,
    Protected = 2,
    Internal  = 3,
};

inline constexpr EnumNameTable<SymbolVisibility, 4> kSymbolVisibilityTable{{{
    { SymbolVisibility::Default,   "default"   },
    { SymbolVisibility::Hidden,    "hidden"    },
    { SymbolVisibility::Protected, "protected" },
    { SymbolVisibility::Internal,  "internal"  },
}}};

[[nodiscard]] constexpr std::string_view
symbolVisibilityName(SymbolVisibility v) noexcept {
    return kSymbolVisibilityTable.name(v);
}
[[nodiscard]] constexpr std::optional<SymbolVisibility>
symbolVisibilityFromName(std::string_view s) noexcept {
    return kSymbolVisibilityTable.fromName(s);
}

// ── Per-relocation row (format-side half of plan 13 §2.6) ──
//
// The assembler emits a `Relocation{offset, target, kind, addend}`
// where `kind` is the OPAQUE tag declared by the TARGET schema's
// `relocations[]` (plan 13). The linker needs to know which
// PLATFORM-NATIVE relocation NAME that kind maps to (e.g. ELF
// `R_X86_64_PC32`, PE `IMAGE_REL_AMD64_REL32`, Mach-O
// `X86_64_RELOC_BRANCH` are all the same kind in plan 13's reloc
// taxonomy).
//
// `name` is the platform-native name the format engine writes into
// the object file's reloc table. `kind` is the opaque tag the
// assembler stamped — same RelocationKind type as plan 13's
// `TargetRelocationInfo::kind`. The format engine looks up by kind
// (O(1) via `relocationKindIndex`) and writes `name` into the
// output bytes.
//
// `nativeId` is the FORMAT-SPECIFIC numeric tag the format engine
// writes into its relocation record:
//   * ELF: `r_info` low 32 bits, e.g. R_X86_64_PC32 = 2.
//   * PE:  Type field, e.g. IMAGE_REL_AMD64_REL32 = 4.
//   * Mach-O: `r_type` 4-bit nibble in `r_info`, e.g.
//     X86_64_RELOC_BRANCH = 2.
//   * WASM: u32 LEB128 reloc type (fits u32 unchanged).
//   * SPIR-V: no traditional relocation table (decoration-based);
//     the field is unused on SPIR-V format schemas.
// Distinct from the universal `kind` (cross-side join key) — `kind`
// is opaque to all formats; `nativeId` is the actual byte value
// embedded in the format's reloc record. Per-row invariant:
// `nativeId != 0` on every relocation row (slot-0 reserved as the
// invalid sentinel — emitting it would write a format-specific
// `R_*_NONE`-shaped record that consumers treat as a no-op,
// silently dropping the patch).
struct DSS_EXPORT ObjectFormatRelocationInfo {
    std::string    name;            // e.g. "R_X86_64_PC32"
    RelocationKind kind{};          // matches assembler-side tag
    std::uint32_t  nativeId = 0;    // ELF r_info type / PE Type /
                                    // Mach-O r_type / WASM reloc type
                                    // — format-specific wire value
};

// ── Per-section row (plan 14 D-LK4-2) ───────────────────────────
//
// Each format's `sections[]` row maps the UNIVERSAL `SectionKind` (the
// engine speaks this) to the FORMAT-NATIVE section name + structural
// fields (e.g. ELF sh_type / sh_flags / sh_addralign). The substrate
// validates the universal `kind` join key + `name` non-empty +
// `kind` unique cross-row; the format-specific numeric fields are
// stored verbatim and interpreted by the format walker.
//
// **`name` is a single-level identifier** — ELF uses one (`.text`)
// and PE uses one (`.text` / `.rdata`). Mach-O uses a two-level
// `(segment, section)` tuple (`__TEXT,__text`). When LK3 lands the
// Mach-O walker, this struct gains a parallel `segment` field —
// anchored as plan 14 §3.1 **D-LK3-1**. Until then `name` carries
// the section-only identifier and the segment relationship lives
// implicit in the format walker.
struct DSS_EXPORT ObjectFormatSectionInfo {
    SectionKind   kind{};            // universal kind enum
    std::string   name;              // e.g. ".text" / ".rela.text"
                                     // (D-LK3-1: gains `segment` at LK3)
    std::uint32_t type = 0;          // ELF sh_type / PE Characteristics
    std::uint64_t flags = 0;         // ELF sh_flags / PE flags
                                     // / Mach-O S_ATTR_* (LK3)
    std::uint64_t addrAlign = 0;     // sh_addralign or format equivalent
    std::uint64_t entrySize = 0;     // sh_entsize (0 for variable-size)
};

// ── ELF-specific identity block (loaded only when kind == Elf) ──
//
// Mirrors `Elf64_Ehdr::e_ident[EI_CLASS..EI_OSABI]` + `e_machine`.
// Lives on `ObjectFormatData` so the ELF walker reads the values
// from JSON instead of hardcoding them. Each format's identity
// block is a parallel optional sub-struct on `ObjectFormatData`
// (`elf{}` / `pe{}` / future `macho{}`); the architect-anchored
// reason for NOT using `std::variant` is that variant would couple
// every format's identity definition into a single header that
// every walker transitively includes — re-creating the per-arch
// coupling the shape-keyed-walker split was designed to avoid.
struct DSS_EXPORT ElfIdentity {
    std::uint8_t  fileClass = 0;     // ELFCLASS64=2 / ELFCLASS32=1
    std::uint8_t  dataEncoding = 0;  // ELFDATA2LSB=1 / ELFDATA2MSB=2
    std::uint8_t  osabi = 0;         // ELFOSABI_NONE=0 / ELFOSABI_GNU=3 / …
    std::uint8_t  abiVersion = 0;
    std::uint16_t machine = 0;       // e_machine: EM_X86_64=62 / EM_AARCH64=183
};

// ── PE/COFF-specific identity block (loaded only when kind == Pe) ──
//
// Mirrors the load-bearing fields of `IMAGE_FILE_HEADER` (PE/COFF
// spec §3.3). `TimeDateStamp` is omitted: deterministic builds
// require it to be 0 and that's the substrate default already.
// `SizeOfOptionalHeader` is omitted: relocatable `.obj` files use 0
// (the optional header lives only in `.exe`/`.dll`, which arrive
// in LK1 cycle 2 / LK5+).
struct DSS_EXPORT PeIdentity {
    std::uint16_t machine = 0;          // IMAGE_FILE_MACHINE_AMD64=0x8664
                                        // / I386=0x014C / ARM64=0xAA64
    std::uint16_t characteristics = 0;  // file-level flags; conventionally
                                        // 0 for relocatable .obj
};

namespace detail {

struct DSS_EXPORT ObjectFormatData {
    ObjectFormatSchemaId id{};
    std::string          name;         // "elf64-x86_64-linux" etc.
    std::string          version;
    ObjectFormatKind     kind = ObjectFormatKind::Elf;

    // Relocations row — same shape as `TargetSchema::relocations[]`
    // so the reloc-taxonomy unifier (plan 13 §2.6) is symmetric.
    std::vector<ObjectFormatRelocationInfo> relocations;
    substrate::TransparentStringMap<std::uint16_t> relocationNameIndex;
    std::unordered_map<RelocationKind, std::uint16_t> relocationKindIndex;

    // Sections row (D-LK4-2). The walker reads sections by
    // SectionKind; `kind` must be unique across rows so the lookup
    // is unambiguous. `name`/`type`/`flags`/`addrAlign`/`entrySize`
    // are format-specific (interpreted by the walker for its format).
    std::vector<ObjectFormatSectionInfo> sections;
    std::unordered_map<SectionKind, std::uint16_t> sectionKindIndex;

    // Per-format identity sub-blocks. Each is populated ONLY when
    // `kind` matches; otherwise zero-defaulted. The walker reads
    // its arm from JSON; validate() enforces the per-kind
    // populated-ness rule.
    ElfIdentity elf{};
    PeIdentity  pe{};

    // Cross-field invariants:
    //   * relocations: kind != 0, kind unique cross-row, name unique
    //     + non-empty, nativeId != 0 when relocations are non-empty.
    //   * sections: kind unique cross-row, name non-empty.
    //   * ELF identity: when kind == Elf, fileClass / dataEncoding /
    //     machine must all be != 0.
    [[nodiscard]] std::vector<ConfigDiagnostic> validate() const;
};

} // namespace detail

class DSS_EXPORT ObjectFormatSchema {
public:
    explicit ObjectFormatSchema(detail::ObjectFormatData data) noexcept
        : d_(std::move(data)) {}

    ObjectFormatSchema(ObjectFormatSchema const&)            = delete;
    ObjectFormatSchema& operator=(ObjectFormatSchema const&) = delete;
    ObjectFormatSchema(ObjectFormatSchema&&) noexcept        = default;
    ObjectFormatSchema& operator=(ObjectFormatSchema&&) noexcept = default;

    [[nodiscard]] ObjectFormatSchemaId id()      const noexcept { return d_.id; }
    [[nodiscard]] std::string_view     name()    const noexcept { return d_.name; }
    [[nodiscard]] std::string_view     version() const noexcept { return d_.version; }
    [[nodiscard]] ObjectFormatKind     kind()    const noexcept { return d_.kind; }

    // Relocation accessors — symmetric with TargetSchema's. The
    // linker calls `relocationByKind(kind)` to find which
    // platform-native NAME to write into the output bytes for a
    // given assembler-side opaque tag.
    [[nodiscard]] std::span<ObjectFormatRelocationInfo const>
    relocations() const noexcept { return d_.relocations; }

    [[nodiscard]] std::size_t
    relocationCount() const noexcept { return d_.relocations.size(); }

    [[nodiscard]] ObjectFormatRelocationInfo const*
    relocationByKind(RelocationKind kind) const noexcept {
        auto it = d_.relocationKindIndex.find(kind);
        if (it == d_.relocationKindIndex.end()) return nullptr;
        return &d_.relocations[it->second];
    }

    [[nodiscard]] ObjectFormatRelocationInfo const*
    relocationByName(std::string_view name) const noexcept {
        auto it = d_.relocationNameIndex.find(name);
        if (it == d_.relocationNameIndex.end()) return nullptr;
        return &d_.relocations[it->second];
    }

    // Section accessors — the walker calls `sectionByKind(...)` to
    // resolve format-native name + sh_type / sh_flags / addralign
    // for an emitted section.
    [[nodiscard]] std::span<ObjectFormatSectionInfo const>
    sections() const noexcept { return d_.sections; }

    [[nodiscard]] std::size_t
    sectionCount() const noexcept { return d_.sections.size(); }

    [[nodiscard]] ObjectFormatSectionInfo const*
    sectionByKind(SectionKind kind) const noexcept {
        auto it = d_.sectionKindIndex.find(kind);
        if (it == d_.sectionKindIndex.end()) return nullptr;
        return &d_.sections[it->second];
    }

    // Per-format identity accessors — each is populated only when
    // `kind()` matches that arm. Walker reads the relevant block
    // when it runs.
    [[nodiscard]] ElfIdentity const& elf() const noexcept { return d_.elf; }
    [[nodiscard]] PeIdentity  const& pe()  const noexcept { return d_.pe; }

    // ── Loaders ───────────────────────────────────────────────
    static LoadResult<std::shared_ptr<ObjectFormatSchema>>
    loadFromFile(std::filesystem::path const& path);
    static LoadResult<std::shared_ptr<ObjectFormatSchema>>
    loadShipped(std::string_view name);
    static LoadResult<std::shared_ptr<ObjectFormatSchema>>
    loadFromText(std::string_view jsonText,
                 std::string_view sourceLabel = "<inline>");

private:
    detail::ObjectFormatData d_;
};

} // namespace dss
