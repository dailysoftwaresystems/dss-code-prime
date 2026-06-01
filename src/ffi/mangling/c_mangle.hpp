#pragma once

#include "core/export.hpp"
#include "link/object_format_schema.hpp"

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
//                                applies this for BOTH 32-bit and
//                                64-bit Mach-O; the convention is
//                                arch-agnostic
//   ObjectFormatKind::Wasm    → no decoration (uses import-namespace
//                                instead of name mangling)
//   ObjectFormatKind::Spirv   → no decoration (SPIR-V has no C ABI)
//   ObjectFormatKind::Unknown → no decoration (defensive default)
//
// Source-language agnostic: this is C-name mangling (FF4's plan
// row). C++/Rust mangling is post-v1 (FF7/FF8) and lives in its
// own file. Target-arch agnostic: the decoration rule depends on
// ObjectFormatKind only — both x86 and ARM Mach-O use the
// underscore convention.

namespace dss::ffi {

// Decorate a canonical C identifier with the per-platform
// linker-visible prefix/suffix. Returns the decorated name.
// Pure function — caller assigns the result back into
// `ImportSurface::mangledName` (FFI ingestion side) or feeds it
// to the linker's import-resolution path (FFI export side).
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
// and a missing prefix is rarely the user's bug.
[[nodiscard]] DSS_EXPORT std::string
unapplyCMangling(std::string_view decoratedName, ObjectFormatKind format);

// Test-exposed: does this format add a leading underscore for C
// symbols? Returns false for ELF / Wasm / SPIR-V / Unknown.
// True for MachO. PE is currently false in v1 (PE64 only); the
// 32-bit PE branch lands when D-FF4-1 fires.
[[nodiscard]] DSS_EXPORT bool
cFormatAddsLeadingUnderscore(ObjectFormatKind format) noexcept;

} // namespace dss::ffi
