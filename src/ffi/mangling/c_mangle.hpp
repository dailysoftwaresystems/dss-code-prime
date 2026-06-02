#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

// Plan 11 FF4 — C name mangling (per-platform underscoring).
// Symmetric apply/unapply functions transform a canonical C
// identifier to/from its per-platform linker-visible decorated
// form.
//
// Convention (v1):
//   ObjectFormatKind::Elf     → no decoration (canonical C identifier)
//   ObjectFormatKind::Pe      → no decoration (PE64 only ships today;
//                                32-bit cdecl `_func` / stdcall
//                                `_func@N` deferred until first 32-bit
//                                PE target)
//   ObjectFormatKind::MachO   → leading underscore (`_printf`) — Apple
//                                convention. The decoration rule
//                                reads `ObjectFormatKind` only,
//                                which is bitness-agnostic by design:
//                                Apple uses the same convention on
//                                32-bit and 64-bit Mach-O so no
//                                bitness axis is needed.
//   ObjectFormatKind::Wasm    → no decoration (uses import-namespace
//                                instead of name mangling)
//   ObjectFormatKind::Spirv   → no decoration (SPIR-V has no C ABI)
//   ObjectFormatKind::Unknown → no decoration (defensive default)
//
// Source-language agnostic: this is C-name mangling (FF4's plan
// row). C++/Rust mangling is post-v1 (FF7/FF8) and lives in its
// own file.

namespace dss::ffi {

// Decorate a canonical C identifier with the per-platform
// linker-visible prefix/suffix. Returns the decorated name.
// Pure function — caller assigns the result back into
// `ImportSurface::mangledName` (FFI ingestion side) or feeds it
// to the linker's import-resolution path (FFI export side).
//
// Mechanical: `applyCMangling("_x", MachO)` returns `"__x"`. The
// function applies the format rule blindly — caller passes
// CANONICAL (undecorated) names. No dedup, no idempotence.
//
// Empty input → empty output (callers gate empties upstream;
// FF4 does not synthesize a name).
[[nodiscard]] DSS_EXPORT std::string
applyCMangling(std::string_view canonicalName, ObjectFormatKind format);

// Inverse of `applyCMangling`: strip the per-platform decoration
// to recover the canonical C identifier. Used by FF1 binary
// readers when ingesting a library whose symbol names are
// already-decorated (Mach-O `_printf` → `printf` for HIR-side
// matching against the user's source `extern int printf(...)`).
//
// Conservative: if the input does NOT carry the expected
// decoration (e.g. a Mach-O symbol `printf` without underscore),
// the function returns the input unchanged rather than
// fabricating semantics — operators usually ship clean libraries
// and a missing prefix is rarely the user's bug. (Strict-mode
// variant that errors on missing-prefix is anchored at
// D-FF4-3 — pairs with FF5 ingest where the format-kind is
// known authoritative.)
//
// Empty input → empty output (mirrors `applyCMangling`).
[[nodiscard]] DSS_EXPORT std::string
unapplyCMangling(std::string_view decoratedName, ObjectFormatKind format);

// Test-exposed: does this format add a leading underscore for C
// symbols? Returns false for ELF / Wasm / SPIR-V / Unknown.
// True for MachO. PE is currently false in v1 (PE64 only); the
// 32-bit PE branch lands when D-FF4-1 fires.
[[nodiscard]] DSS_EXPORT bool
cFormatAddsLeadingUnderscore(ObjectFormatKind format) noexcept;

// Closed-set failure modes for `unapplyCManglingStrict`.
enum class MangleErrorKind : std::uint8_t {
    MissingExpectedPrefix = 0,  // format expects decoration; input lacks it
    Count_                      // table-size sentinel — keep LAST
};

struct DSS_EXPORT MangleError {
    MangleErrorKind kind = MangleErrorKind::MissingExpectedPrefix;
    std::string     detail;
};

[[nodiscard]] DSS_EXPORT std::string_view
    mangleErrorKindName(MangleErrorKind k) noexcept;

// Strict-mode inverse of `applyCMangling`: returns an error if the
// `decoratedName` does NOT carry the per-format decoration the rule
// expects. Used by FF5 ingest where the format-kind is authoritative
// — a Mach-O binary's `.dynsym` entry that lacks the leading `_` is
// a structural anomaly worth surfacing rather than the conservative
// pass-through that `unapplyCMangling` does.
//
// For formats with no decoration (ELF/Pe/Wasm/Spirv/Unknown), strict
// mode is structurally a no-op: input passes through unchanged and
// success is returned. The strict check only fires for decorated
// formats (MachO today; PE32 cdecl post-D-FF4-1).
//
// Empty input → empty output success (mirrors `applyCMangling`
// empty-input contract; an empty name is never "decorated" so there's
// nothing to enforce). `reporter` receives an `F_*` diagnostic on
// the error path.
[[nodiscard]] DSS_EXPORT std::expected<std::string, MangleError>
unapplyCManglingStrict(std::string_view    decoratedName,
                       ObjectFormatKind    format,
                       DiagnosticReporter& reporter);

} // namespace dss::ffi
