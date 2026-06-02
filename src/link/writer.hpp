#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "link/linker.hpp"

#include <filesystem>

// Linker image file emission — plan 14 LK10 cycle 1 substrate.
//
// Bridges `link()`'s in-memory `LinkedImage::bytes` to a real
// on-disk artifact. The hermetic-acceptance gate (LK10 full) needs
// this so the build pipeline can produce a runnable `.exe` /
// `.o` / `.dylib` / `.wasm` / `.spv` file without shelling out to
// a system linker; cycle 1 lands the substrate, cycle 2 wires it
// into the driver's `compileFiles` / `compileProject` entry
// points + CLI argument routing (anchored at plan 14 §3 LK10 row).
//
// Namespace `dss::linker` — same namespace as `dss::linker::link()`
// (the linker entry-point function in `linker.hpp`). The D-LK9-2
// rename landed at LK10 cycle 2; both substrate functions now
// share this single namespace.
//
// The function deliberately leaves three concerns to the caller:
//   * Format/target selection — already encoded in the `Linked
//     Image::format` discriminator + the JSON the caller loaded.
//   * File-extension policy — `.exe` vs `.dll` vs `.o` etc. lives
//     in the artifact profile (plan 6) / driver layer, not here.
//     The caller hands us a fully-qualified path.
//   * Parent-directory creation — `std::filesystem::create_
//     directories` is the caller's responsibility. We fail loud
//     rather than silently creating arbitrary paths (a substrate
//     that silently mkdir's would mask config errors that ship
//     binaries to the wrong target dir).

namespace dss::linker {

// Write `image.bytes` to `path`, truncating any existing file.
// Returns `true` iff the bytes landed on disk. On failure, emits
// one of six remediation-distinct K_* codes into `reporter` and
// returns `false`:
//   * `K_ImageNotOk`               — `image.ok() == false`
//   * `K_ImageEmpty`               — `ok() == true` but bytes empty
//   * `K_ImageWriteParentMissing`  — parent dir absent
//   * `K_ImageWriteOpenFailed`     — open() failbit
//   * `K_ImageWriteShort`          — write() mid-stream failbit
//   * `K_ImageWriteCloseFailed`    — close() flush failbit
//
// Three preconditions enforced:
//   * `image.ok()` — parallel-index gate. Writing a half-built
//     image would silently produce a corrupt artifact whose
//     `expectedFuncCount != resolvedFuncCount`; we reject at the
//     write surface.
//   * `!image.bytes.empty()` — every shipping format produces at
//     least a header (8 bytes for WASM, 20 for SPIR-V, 64+ for
//     ELF/PE/MachO). Empty bytes are a substrate failure.
//   * `path.parent_path()` exists (or `path` is in the current
//     working directory).
//
// The format discriminator on `image.format` is NOT consulted to
// pick a file extension — the caller fully owns the path. This
// keeps the substrate format-blind in the same shape as the
// rest of plan 14's substrate.
[[nodiscard]] DSS_EXPORT bool
writeImage(LinkedImage const&             image,
           std::filesystem::path const&   path,
           DiagnosticReporter&            reporter);

} // namespace dss::linker
