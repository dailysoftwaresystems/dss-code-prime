#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "link/linker.hpp"

#include <cstdint>
#include <filesystem>
#include <span>

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

// Write `image.bytes` to `path`, REPLACING any existing file with a new
// file identity (see `writeBytes` below — the artifact is staged in a
// sibling temp and renamed over the target; it is never truncated in
// place). Returns `true` iff the bytes landed on disk. On failure, emits
// one of six remediation-distinct K_* codes into `reporter` and
// returns `false`:
//   * `K_ImageNotOk`               — `image.ok() == false`
//   * `K_ImageEmpty`               — `ok() == true` but bytes empty
//   * `K_ImageWriteParentMissing`  — parent dir absent
//   * `K_ImageWriteOpenFailed`     — no filename component, temp-create
//                                    failbit, or the commit rename failed
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
//
// `executable` (D-OUTPUT-EXEC-BIT): when true, the written file is
// marked executable (POSIX — adds owner/group/others exec via
// std::filesystem::permissions) so a produced binary runs directly
// (`./out`) without a manual `chmod +x`. The CALLER decides this from
// CONFIG — `ObjectFormatSchema::isImageFlavor()` (an exec/image-flavor
// output), never an arch/format identity branch — so `writeImage` stays
// format-blind (it switches on a bool, not the format). A no-op on
// Windows (PE ignores Unix modes). Default false: a plain artifact
// (.o / non-image output) is written at the host umask, unchanged. If
// the exec-bit add fails (rare — e.g. a read-only FS) a WARNING
// `K_ImageExecBitFailed` surfaces but the call still returns true: the
// bytes are valid, the exec bit a best-effort convenience (distinct
// from the byte-integrity `K_ImageWrite*` errors).
[[nodiscard]] DSS_EXPORT bool
writeImage(LinkedImage const&             image,
           std::filesystem::path const&   path,
           DiagnosticReporter&            reporter,
           bool                           executable = false);

// Commit a raw byte buffer to `path`. The byte-integrity core `writeImage`
// delegates to AFTER its LinkedImage-shape preconditions -- factored out so
// other artifact producers that are NOT a `LinkedImage` (the c163 `ar`
// static-archive writer -- an archive has no `ok()`/function-count contract)
// reuse the SAME fail-loud discipline. Returns true iff every byte landed on
// disk; on failure emits one of the same four remediation-distinct K_* codes
// into `reporter`:
//   * `K_ImageWriteParentMissing`  -- parent dir absent
//   * `K_ImageWriteOpenFailed`     -- path has no filename component, the
//                                     staging temp could not be created, or
//                                     the commit rename failed
//   * `K_ImageWriteShort`          -- write() mid-stream failbit
//   * `K_ImageWriteCloseFailed`    -- close() flush failbit
//
// ATOMIC REPLACE, NEW IDENTITY (D-LK-WRITER-TRUNCATES-INSTEAD-OF-RENAMING).
// The bytes are staged in a uniquely-named SIBLING temp (`<path>.dsstmp-*`,
// claimed with `std::ios::noreplace`) and then renamed over `path`. The
// artifact is therefore never truncated in place, and every write produces a
// NEW file identity -- required because macOS attaches an exec DENY to a
// REUSED inode, killing a rebuilt binary at that inode (exit 137) while a
// byte-identical copy runs. Consequences callers should know: `path` appears
// with complete bytes or not at all (a failed write leaves the PREVIOUS
// artifact intact and untouched); the new file carries fresh permission bits
// at the host umask rather than inheriting the old file's; and if `path` is a
// symlink or hard link the rename replaces THAT NAME rather than writing
// through to the shared file. Reasoning and the cross-platform determination
// are in writer.cpp at the fix site.
// Does NOT gate on `bytes.empty()` (that is a LinkedImage-specific contract --
// an empty write is a legitimate raw-bytes request) and never sets the POSIX
// execute bit (an archive / relocatable output is not executable; `writeImage`
// owns the exec-bit step for image-flavor outputs). Parent-directory creation
// stays the caller's responsibility (the substrate never auto-mkdir's).
[[nodiscard]] DSS_EXPORT bool
writeBytes(std::span<std::uint8_t const> bytes,
           std::filesystem::path const&  path,
           DiagnosticReporter&           reporter);

} // namespace dss::linker
