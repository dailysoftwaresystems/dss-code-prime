#pragma once

#include "core/export.hpp"
#include "core/types/target_schema.hpp"   // EnumNameTable<E,N>

#include <cstdint>
#include <optional>
#include <string_view>

// Canonical object-format taxonomy — the closed-enum vocabulary the
// substrate engine speaks for the OUTPUT image kind (ELF / PE/COFF /
// Mach-O / WASM / SPIR-V). Each `.format.json` declares ONE
// `ObjectFormatKind` in its top-level `kind` field; the engine reads
// the kind, per-format JSON owns the on-disk byte layout.
//
// **Cross-tier vocabulary**: this header lives under `core/types/`
// rather than `src/link/` so non-linker layers can speak the kind
// without pulling in `link/object_format_schema.hpp`'s full
// 800-LOC substrate. Concrete callers:
//   * `src/ffi/`     — FF4 C-mangling dispatches on
//                      `ObjectFormatKind` (per-format
//                      leading-underscore rule);
//                      `synthesizeFfiFromSourceDecls` reports the
//                      kind in `F_FfiNoImportLibraryForFormat`
//                      diagnostics.
//   * `src/core/types/grammar_schema_json.cpp` — semantic config
//                      loader validates the per-language
//                      `externLibraryByFormat` map keys against
//                      this enum's name table at language-load time
//                      (catches a typo like `"pee"` instead of
//                      `"pe"` at config load rather than at compile
//                      time).
// Same extraction precedent as `section_kind.hpp` (which left
// `link/object_format_schema.hpp` as the umbrella header so every
// existing consumer continues to work without changing its
// `#include`).
//
// **Sentinel discipline**: `Unknown = 0` is the project's universal
// invalid sentinel (mirrors `TargetEncodingShape::None`,
// `RelocationKind{}`, strong-ids); a default-constructed value
// reports `Unknown`, NOT a spurious ELF identity.

namespace dss {

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

} // namespace dss
