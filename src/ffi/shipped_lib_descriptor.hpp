#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"   // TypeId

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ── LANGUAGE-NEUTRAL shipped-library FFI descriptor reader ───────────────────
//
// Closes D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC. The shipped system "headers"
// (the /usr/include analogue under `src/dss-config/shippedLibs/<platform>/`)
// are NOT per-language source files; they are a NEUTRAL JSON descriptor read
// by this UNIVERSAL reader. The user-facing UX is unchanged and C-faithful —
// `#include <stdio.h>` still works — but the on-disk shipped artifact is a
// language-agnostic schema, not a c-subset `.h`.
//
// Shape (`stdio.json`):
//   { "header": "stdio.h", "standard": "c89",
//     "library": { "pe": "msvcrt.dll", "elf": "libc.so.6",
//                  "macho": "/usr/lib/libSystem.B.dylib" },
//     "symbols": [
//       { "name": "puts", "signature": "fn(ptr<char>) -> i32",
//         "kind": "function", "linkage": "external" }
//     ] }
//
// Model 3 (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC, 2026-06-09): a descriptor is
// PLATFORM-NEUTRAL — ONE descriptor per header, with a per-OBJECT-FORMAT
// `library` MAP keyed by the canonical `objectFormatKindName` vocabulary
// ("pe"/"elf"/"macho"). The active compilation target's format selects its
// runtime image at resolution time (compile_pipeline), so the SAME descriptor
// serves every target — no per-platform directory selection. This dissolves
// the former per-platform-directory layout + `D-FFI-SHIPPED-LIB-PLATFORM-SELECT`.
//
// PROVENANCE — every shipped symbol traces to header → standard → library →
// (object format). The `header` + `standard` fields make that origin
// FIRST-CLASS data, not just a filename convention: a tool / diagnostic can
// answer "where does `strlen` come from?" from the descriptor alone.
//
//   * `header`    — REQUIRED. The C (or other-language) header this descriptor
//                   represents, e.g. "stdio.h". The angle-include resolver maps
//                   `<stdio.h>` → `stdio.json` by filename; this field is the
//                   authoritative, machine-readable provenance (and should match
//                   the filename stem). A descriptor with no header is a
//                   provenance hole — fail loud.
//   * `standard`  — optional. The language standard the header/symbols belong to
//                   (e.g. "c89" / "c99" / "c11" / "posix"). Provenance only;
//                   carried for tooling, not consumed by lowering.
//   * `library`   — optional per-OBJECT-FORMAT map ("pe"/"elf"/"macho" →
//                   runtime image name). At resolution the active target's
//                   format selects its entry. A map MISSING the active format's
//                   key ⇒ the lowering falls back to the active language's
//                   `externLibraryByFormat[format]` default (so a descriptor MAY
//                   omit a format and inherit the language's default; an entirely
//                   absent map inherits for every format). A key NOT in the
//                   `objectFormatKindFromName` vocabulary is a typo/garbage and
//                   FAILS LOUD on read (F_ShippedLibDescriptorMalformed).
//   * `symbols`   — required, non-empty array.
//   * `name`      — required. The undecorated C identifier (the linker-visible
//                   name is produced downstream by FF4 mangling).
//   * `signature` — required. A hir-text TYPE STRING — a full `fn(...) -> ...`
//                   FnSig for functions, or a value type for an object —
//                   decoded by `dss::parseTypeFromText` into the CALLER's
//                   interner. There is exactly ONE type-text decoder in the
//                   codebase; this reader reuses it (no second grammar).
//   * `kind`      — optional (default "function"). Closed enum: "function" |
//                   "object". Selects the HIR node the lowering synthesizes
//                   (ExternFunction vs ExternGlobal).
//   * `linkage`   — optional (default "external"). Closed enum: "external" |
//                   "weak". Carried for completeness + validated; the FF5
//                   source-declared synthesis path currently emits Strong
//                   linkage uniformly (a shipped symbol is an authoritative
//                   import), so this is a forward-compatible descriptor field.
//
// Agnostic: the reader is a PURE function of (path, interner, typeReg,
// reporter). It branches on NO source language, NO CPU target, NO object
// format — every type is built via the passed `TypeInterner`. It is the same
// reader for every platform's descriptor.

namespace dss {

class DiagnosticReporter;
class TypeInterner;
class TypeRegistry;

namespace ffi {

// What kind of symbol the descriptor declares — selects the HIR extern node
// the CST→HIR lowering synthesizes. Closed enum (descriptor-local; deliberately
// decoupled from the FF1/FF2 `import_surface.hpp` SymbolKind, whose Tls/NoType
// members carry binary-reader semantics that don't apply to a neutral
// descriptor). Default is `Function`.
enum class ShippedSymbolKind : std::uint8_t {
    Function = 0,  // → makeExternFunction (the FnSig lives in `signature`)
    Object   = 1,  // → makeExternGlobal   (a data symbol; `signature` is its type)
};

// Symbol linkage as declared in the descriptor. Closed enum (descriptor-local).
// Default is `External`. Validated on read; carried for forward-compatibility.
enum class ShippedSymbolLinkage : std::uint8_t {
    External = 0,
    Weak     = 1,
};

// One decoded symbol. `signature` is already interned into the caller's
// interner (never InvalidType — a signature that fails to decode is a hard
// error that aborts the whole read, so a returned descriptor's symbols always
// carry valid types).
struct DSS_EXPORT ShippedSymbol {
    std::string          name;
    TypeId               signature;
    ShippedSymbolKind    kind    = ShippedSymbolKind::Function;
    ShippedSymbolLinkage linkage = ShippedSymbolLinkage::External;
};

// A decoded shipped-library descriptor. `header` is the authoritative
// provenance (which header these symbols come from); `standard` is optional
// provenance; `library` is a per-OBJECT-FORMAT map ("pe"/"elf"/"macho" → image
// name) that MAY be empty or omit a format (the resolution then falls back to
// the language's per-format default). Keys are validated against the
// `objectFormatKindFromName` vocabulary on read — an unknown key fails loud.
struct DSS_EXPORT ShippedLibDescriptor {
    std::string                header;    // REQUIRED provenance, e.g. "stdio.h".
    std::string                standard;  // optional provenance, e.g. "c89".
    // Per-object-format runtime image, keyed by `objectFormatKindName`
    // ("pe"/"elf"/"macho"). The compile pipeline selects the active target's
    // entry; a missing key inherits `externLibraryByFormat[format]`.
    std::unordered_map<std::string, std::string> library;
    std::vector<ShippedSymbol> symbols;
};

// Read + decode the neutral descriptor at `path`, interning each symbol's
// `signature` type into `interner` (+ `typeReg` for `ext<>` kinds). PURE —
// no language/target/format branch.
//
// Returns std::nullopt and emits at least one Error-severity diagnostic on
// ANY failure:
//   * file unreadable / not valid JSON / wrong top-level shape / missing or
//     wrong-typed required key / unknown key / unrecognized kind|linkage enum
//     → `F_ShippedLibDescriptorMalformed`.
//   * a symbol's `signature` failed to decode (`parseTypeFromText` returned
//     InvalidType) → `F_ShippedLibUnsupportedType`. CRITICAL: such a symbol is
//     NEVER returned with InvalidType — the whole read fails so no extern is
//     ever synthesized with an unresolved type.
//
// On success the returned descriptor is fully populated and every symbol's
// `signature` is a valid TypeId in `interner`.
[[nodiscard]] DSS_EXPORT std::optional<ShippedLibDescriptor>
readShippedLibDescriptor(std::filesystem::path const& path,
                         TypeInterner&                interner,
                         TypeRegistry&                typeReg,
                         DiagnosticReporter&          reporter);

} // namespace ffi
} // namespace dss
