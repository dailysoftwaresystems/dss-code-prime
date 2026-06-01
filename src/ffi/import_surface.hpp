#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Plan 11 FFI substrate — `ImportSurface` is the binary-reader output
// row (FF1) and the C-header-parser output row (FF2) — both writers
// produce this same row type so the consumer (HIR FfiMap population +
// `collectExterns` MIR pre-pass) sees a uniform surface regardless of
// where the metadata came from.
//
// Distinct from `ExternImport` (core/types/extern_import.hpp) — that's
// the link-time row stamped onto an `AssembledModule` AFTER the user's
// source declares `extern fn foo(...)`. `ImportSurface` is upstream:
// it enumerates "what the binary / header CAN expose" so the source-
// translation step can validate the user's `extern` declarations
// against the ABI surface.
//
// Format-blind by construction:
//   * ELF readers populate `mangledName` from the `.dynsym` ST_NAME
//     index'd into `.dynstr`; visibility from `st_other`/`STV_*`;
//     libraryPath from the source `.so` path.
//   * PE readers populate from `.edata` (export table); libraryPath
//     from the source `.dll` path.
//   * Mach-O readers populate from `LC_SYMTAB` n_strx; libraryPath
//     from `LC_LOAD_DYLIB` paths.
//   * FF2 C header parser populates synthetically — mangledName from
//     the C decl's identifier (with per-platform underscoring rules
//     applied per FF4); libraryPath from the user-declared
//     `#pragma comment(lib, ...)` or a project-config side table.
//
// Closed-enum discriminators (visibility / kind / linkage) mirror
// the codebase's pattern (RelocFormulaKind, ObjectFormatKind, etc.):
// adding a new format reader = populate the same enum, no string
// drift.

namespace dss::ffi {

// Symbol visibility from the binary's symbol table. ELF STV_*,
// PE export-table flags, Mach-O n_type bits all map to one of these
// closed values.
enum class SymbolVisibility : std::uint8_t {
    Default   = 0,  // ELF STV_DEFAULT / PE default / Mach-O default
    Hidden    = 1,  // ELF STV_HIDDEN / PE has-no-export / Mach-O N_PEXT
    Protected = 2,  // ELF STV_PROTECTED — semantic between Default + Hidden
    Internal  = 3,  // ELF STV_INTERNAL — strict use-within-DSO
};

// What kind of symbol the binary exposes.
enum class SymbolKind : std::uint8_t {
    Function = 0,  // ELF STT_FUNC; PE export of function entry; Mach-O N_SECT in __text
    Object   = 1,  // ELF STT_OBJECT (data); PE data export; Mach-O N_SECT in __data
    Tls      = 2,  // ELF STT_TLS; PE __declspec(thread); Mach-O thread-local
    NoType   = 3,  // ELF STT_NOTYPE (unknown — common in stripped .so)
};

// Linkage class — only relevant for header-mode (FF2). Binary readers
// (FF1) set this from the binary's bind class.
enum class SymbolLinkage : std::uint8_t {
    External = 0,  // ELF STB_GLOBAL / PE export / Mach-O N_EXT
    Weak     = 1,  // ELF STB_WEAK / PE weak alias / Mach-O N_PEXT|N_WEAK_DEF
    Local    = 2,  // ELF STB_LOCAL (rare in `.dynsym`; common in `.symtab`)
};

// One row in the import surface — describes a single symbol the
// dynamic library exports.
struct DSS_EXPORT ImportSurface {
    std::string      mangledName;   // on-binary symbol name (verbatim)
    std::string      libraryPath;   // owning binary's path/soname
    SymbolKind       kind       = SymbolKind::NoType;
    SymbolVisibility visibility = SymbolVisibility::Default;
    SymbolLinkage    linkage    = SymbolLinkage::External;
    // Reserved for FF2 C-header consumption — populated when an
    // `extern` decl carries a function signature. `std::nullopt`
    // for FF1 binary readers (binaries don't carry C signatures,
    // only mangled names + symbol kinds).
    //
    // Free-form opaque string (e.g. `"int(const char*)"`) — parsed
    // structurally by plan 11's ABI catalog (FF3) when the user's
    // source declaration is validated against this surface. `optional`
    // makes "no signature attached" semantically distinct from "header
    // declared with empty parens" (which is invalid C and should
    // surface as a parse diagnostic). post-fold #1 type-design fix —
    // was a free-form `std::string` with the "empty means absent"
    // convention, an SoT smell that FF2 + FF3 would have inherited.
    std::optional<std::string> cSignature;
};

} // namespace dss::ffi
