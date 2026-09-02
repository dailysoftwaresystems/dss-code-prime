#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <vector>

// Mach-O 64-bit relocatable object (.o, MH_OBJECT) writer —
// plan 14 LK3 cycle 1.
//
// Third format walker plugged into the format-blind `link()`
// dispatch. Mirrors LK1's ELF walker + LK2's PE walker — pure free
// function consuming structured input + schema configs, emitting
// bytes, leaving error reporting to the caller-owned reporter.
//
// Cycle scope (minimal valid MH_OBJECT for x86_64-darwin):
//   * `mach_header_64` (32 B) with the identity bytes + filetype =
//     MH_OBJECT.
//   * One `LC_SEGMENT_64` (72 B) wrapping all sections; per Apple
//     convention the segment name is empty for MH_OBJECT (each
//     `section_64.segname` carries the per-section segment name
//     like `__TEXT`).
//   * One `section_64` (80 B) for `__text` with two-level naming
//     (`__TEXT`, `__text`).
//   * `LC_SYMTAB` (24 B) pointing at the symbol table + string
//     table file offsets.
//   * `__text` raw bytes + per-section `relocation_info` table
//     (8 B packed each).
//   * `nlist_64[]` (16 B packed each) — defined extern functions
//     + undefined externs. Mach-O does NOT mandate local-then-
//     global ordering when LC_DYSYMTAB is absent; the substrate
//     emits defined-then-undefined for cross-format consistency.
//   * String table (NUL-seeded; same shape as ELF, distinct from
//     PE's u32-size-prefix form).
//
// No LC_DYSYMTAB this cycle (Apple's ld64 re-derives it). No
// LC_DYLD_INFO / LC_MAIN / LC_LOAD_DYLIB — those are executable-
// side concerns anchored at LK6 (dynamic linking) and a future
// macho-executable cycle.

namespace dss::macho {

// ── The ONE spelling of "does this image request a code signature?" ──
//
// D-LK-MACHO-ADHOC-SIGNATURE-DROPPED-ON-STATIC-ARM. A Mach-O image asks
// for a signature through EITHER of two INDEPENDENT schema keys, and
// they are alternatives rather than a pair:
//
//   * `image.codeSignatureSize` — the legacy placeholder reservation.
//     N zero bytes at the tail of __LINKEDIT plus an LC_CODE_SIGNATURE
//     pointing at them, awaiting a post-link fill.
//   * `image.codeSignature` — the ad-hoc block. The walker BUILDS a
//     real CodeDirectory + SuperBlob and DERIVES the reservation length
//     from it (`adHocCodeSignatureSize`), so no hand-typed size is
//     wanted alongside — `macho_backend`'s validate() refuses the pair.
//
// Every site that must know WHETHER a signature was requested therefore
// has to read BOTH fields. Three sites once spelled that disjunction by
// hand and two of them spelled it `codeSignatureSize != 0` alone, so a
// format whose only request is the ad-hoc block slipped past both and
// had its signature dropped on the static exec arm with NO diagnostic —
// a build that reports success and a binary AMFI refuses at load. One
// named predicate is the fix that a fourth site cannot be born wrong
// against; hand-spelling the disjunction again is the defect returning.
//
// ⚠ THIS ANSWERS PRESENCE, NEVER FLAVOUR. `encodeExecDynamic` chooses
// between the derived ad-hoc blob and the zero-filled placeholder at
// `codeSigReserveSize` and at the fill, and both correctly test
// `codeSignature.has_value()` ALONE. Routing those through this
// predicate would make a legacy placeholder request try to build an
// ad-hoc blob out of an absent block — a regression, not a tidy-up.
[[nodiscard]] constexpr bool
requestsCodeSignature(MachOImage const& im) noexcept {
    return im.codeSignatureSize != 0 || im.codeSignature.has_value();
}

[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter);

} // namespace dss::macho
