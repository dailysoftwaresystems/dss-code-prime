#include "link/writer.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <system_error>
#include <utility>

// `getpid` — a HOST process id, used only as the seed for a unique temp
// filename (see `processSeed` below). Same include dance as
// `tests/test_support/scratch_dir.hpp`, which established this precedent.
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

// Linker image file emission — plan 14 LK10 cycle 1 substrate
// implementation. See writer.hpp for the contract.

namespace dss::linker {

namespace {

void emit(DiagnosticReporter& reporter,
          DiagnosticCode code,
          std::string msg) {
    dss::report(reporter, code,
                          DiagnosticSeverity::Error,
                          std::move(msg));
}

// Per-process seed for the temp filename. `getpid()` discriminates
// CONCURRENT PROCESSES (two `dss` invocations emitting into one output
// dir); the atomic counter in `writeBytes` discriminates concurrent
// writes WITHIN one process.
//
// Neither is a guarantee on its own, and this is the exact correction
// `tests/test_support/scratch_dir.hpp` records for
// D-TEST-EXAMPLES-RUNNER-PARALLEL-CONTENTION-FLAKE: pids RECYCLE, and a
// killed run can leave a stale artifact at any (pid, counter) pair — 55
// stale scratch dirs were being silently SHARED because the code claimed
// its slot with a call that reports success when the slot already exists.
// So the pid+counter pair here is only the SEED. The claim itself is made
// ATOMICALLY by opening with `std::ios::noreplace` (C++23 P2467R1 —
// `fopen` mode "x", i.e. O_CREAT|O_EXCL), which succeeds only when THIS
// call created the file; a slot we did not create is stepped over, never
// reused.
[[nodiscard]] std::uint64_t processSeed() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

// Windows-safe path-to-string for diagnostic messages. Both
// `path::string()` AND `path::generic_string()` perform narrowing
// from `wchar_t` on MSVC and CAN THROW `std::system_error` when
// the path contains code units that can't be narrowed to the
// current locale's ANSI codepage. A throw inside the failure-
// reporting path would silently abort the writer mid-diagnostic.
//
// Safe approach: try the narrow form first; on throw, fall back
// to `u8string()` (returns `std::u8string`, guaranteed UTF-8, never
// throws per the C++20 spec) and reinterpret to `std::string`.
// (silent-failure-hunter HIGH fold #2, LK10 cycle 1 post-fold #2
// review — the post-fold #1 `generic_string()`-only fix did not
// fully close the Windows throw hazard.)
[[nodiscard]] std::string pathForDiag(std::filesystem::path const& p) {
    try {
        return p.generic_string();
    } catch (...) {
        auto const u8 = p.u8string();
        return std::string(reinterpret_cast<char const*>(u8.data()),
                           u8.size());
    }
}

// Delete the staging temp on a failure path and return a note to APPEND to
// the caller's primary diagnostic. Returns an empty string on success.
//
// Fail-loud discipline: a failure to clean up is itself a real fact (a
// leaked temp sitting next to the artifact), but it is SECONDARY — the
// caller already has a primary error to report and emitting a second
// diagnostic would split one event across two codes. Folding it into the
// primary message keeps one failure == one diagnostic while never hiding
// the leak. Never returns a "success" indication that would let the caller
// report the write as clean.
[[nodiscard]] std::string
removeTempNote(std::filesystem::path const& tempPath) {
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
    if (!ec) {
        return {};
    }
    return std::string{" [additionally: the staging temp '"}
         + pathForDiag(tempPath)
         + "' could NOT be removed (" + ec.message()
         + ") — delete it manually]";
}

} // namespace

bool writeImage(LinkedImage const&             image,
                std::filesystem::path const&   path,
                DiagnosticReporter&            reporter,
                bool                           executable) {
    // Precondition 1: parallel-index gate. Writing an image whose
    // `ok()` is false would silently ship bytes that don't match
    // the expected function count. Fail loud here so a
    // misconfigured build script can't bypass the gate by calling
    // writeImage unconditionally.
    if (!image.ok()) {
        emit(reporter, DiagnosticCode::K_ImageNotOk,
             std::string{"link::writeImage: refusing to write "
                         "image whose ok() is false ("}
                 + "expectedFuncCount="
                 + std::to_string(image.expectedFuncCount)
                 + ", resolvedFuncCount="
                 + std::to_string(image.resolvedFuncCount)
                 + ", bytes.size()="
                 + std::to_string(image.bytes.size())
                 + "). The upstream walker likely emitted a "
                   "diagnostic; check `reporter.errorCount()` "
                   "before calling writeImage.");
        return false;
    }
    // The `ok()` check requires resolvedFuncCount == expectedFuncCount
    // (0 == 0 for a valid EMPTY module); if ok() returned true but bytes
    // are empty, the walker is contract-broken. This is the load-bearing
    // guard for the empty-module case: even a declaration-only TU (0
    // functions) must still produce real object bytes — a valid header +
    // section table — never zero bytes (D-CSUBSET-TESTTU-SILENT-EXIT1).
    // Surface here.
    if (image.bytes.empty()) {
        emit(reporter, DiagnosticCode::K_ImageEmpty,
             std::string{"link::writeImage: LinkedImage.bytes is "
                         "empty despite ok() == true — the walker "
                         "returned success with no output. "
                         "Substrate contract violation; fix the "
                         "walker, not the caller. (Type-design "
                         "split: distinct from K_ImageNotOk which "
                         "signals upstream walker failure that "
                         "already raised a diagnostic.)"});
        return false;
    }
    // Byte-integrity commit -- shared with the raw-bytes producers via
    // `writeBytes` (parent check + sibling-temp claim + write + close +
    // rename-over-target, all fail-loud). The artifact appears at `path`
    // only on full success, and always as a NEW file identity
    // (D-LK-WRITER-TRUNCATES-INSTEAD-OF-RENAMING).
    if (!writeBytes(image.bytes, path, reporter)) {
        return false;
    }
    // D-OUTPUT-EXEC-BIT: an EXECUTABLE-flavor output must carry the POSIX
    // execute bit so the produced binary runs directly (`./out`) without a
    // manual `chmod +x` (qemu's prepare_binprm + the kernel's execve both
    // reject a file lacking `mode & 0111`). Add owner/group/others-exec on
    // top of whatever the umask left (`perm_options::add`); a no-op on
    // Windows, where PE ignores Unix modes. Best-effort by design: the bytes
    // are already safely flushed above, so a failure to set the bit is a
    // WARNING (the artifact is valid — it just needs a manual chmod), NOT a
    // write failure. `executable` is the CALLER's config-driven decision
    // (`ObjectFormatSchema::isImageFlavor()`); this code never inspects the
    // format itself, staying format-blind.
    if (executable) {
        std::error_code ec;
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_exec
                | std::filesystem::perms::group_exec
                | std::filesystem::perms::others_exec,
            std::filesystem::perm_options::add, ec);
        if (ec) {
            dss::report(reporter, DiagnosticCode::K_ImageExecBitFailed,
                        DiagnosticSeverity::Warning,
                        std::string{"link::writeImage: wrote '"}
                            + pathForDiag(path)
                            + "' but could not set its POSIX execute bit ("
                            + ec.message()
                            + "); the binary is valid but needs `chmod +x` to "
                              "run directly.");
        }
    }
    return true;
}

bool writeBytes(std::span<std::uint8_t const> bytes,
                std::filesystem::path const&  path,
                DiagnosticReporter&           reporter) {
    // Precondition: the path names a FILE. `rename()` needs a real final
    // component, and an empty one (an unset config field passing "") could
    // never name an artifact. Reject up front so a bad path can never leave
    // a staging temp behind in the current working directory.
    if (path.filename().empty()) {
        emit(reporter, DiagnosticCode::K_ImageWriteOpenFailed,
             std::string{"link::writeBytes: refusing to write to '"}
                 + pathForDiag(path)
                 + "' — the path has no filename component (empty path, or "
                   "a path ending in a directory separator). The caller must "
                   "supply a fully-qualified output file path.");
        return false;
    }
    // Precondition: parent directory exists.
    auto const parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        bool const exists = std::filesystem::exists(parent, ec);
        if (ec) {
            emit(reporter, DiagnosticCode::K_ImageWriteParentMissing,
                 std::string{"link::writeBytes: failed to stat "
                             "parent directory '"}
                     + pathForDiag(parent) + "': " + ec.message());
            return false;
        }
        if (!exists) {
            emit(reporter, DiagnosticCode::K_ImageWriteParentMissing,
                 std::string{"link::writeBytes: parent directory "
                             "does not exist: '"}
                     + pathForDiag(parent)
                     + "'. The substrate does not auto-create "
                       "directories — the caller must call "
                       "std::filesystem::create_directories before "
                       "writing.");
            return false;
        }
    }
    // ─────────────────────────────────────────────────────────────────
    // D-LK-WRITER-TRUNCATES-INSTEAD-OF-RENAMING — write a SIBLING TEMP, then
    // RENAME it over the target. DO NOT "simplify" this back to opening
    // `path` with `std::ios::trunc`. Why, MEASURED on macOS 26.5.2:
    //
    // `trunc` REUSES the target's existing inode. A consumer that rebuilds
    // repeatedly to one stable output path therefore keeps handing the OS
    // the SAME inode, and that inode eventually acquires a per-inode exec
    // DENY (`AMFI ... bailing out`; `AppleSystemPolicy: Security policy
    // would not allow process`). The artifact then dies with exit 137
    // SIGKILL and no output — while a byte-identical copy at another path
    // runs fine. The controls that isolated this: a hard link (different
    // path, SAME inode) is equally dead, so the deny is INODE-bound, not
    // path-bound; an in-place `cp` over the soured path stays dead, but a
    // `mv` of a fresh temp over it runs. `codesign -f -s -` appearing to
    // "repair" it was always the RENAME — codesign writes a temp and
    // renames (inode 20615856 -> 20615857), never the signature.
    //
    // A NEW FILE IDENTITY per write is the cure, and it is the established
    // convention (codesign, every editor's atomic-save), not a workaround.
    //
    // This is HOST FILESYSTEM behaviour, not target-format behaviour: it is
    // deliberately identical for ELF / PE / Mach-O / wasm / SPIR-V and for
    // every arch. Nothing here may ever branch on `image.format`.
    //
    // SIBLING TEMP IS MANDATORY. The temp is built by appending an ASCII
    // suffix to `path` itself, so it is a sibling BY CONSTRUCTION — there is
    // no separate directory expression a later edit could point at
    // `/tmp`/`$TMPDIR`. That matters on BOTH hosts: a temp on a different
    // filesystem turns POSIX `rename(2)` into EXDEV, and on Windows MSVC's
    // `std::filesystem::rename` is MoveFileExW with MOVEFILE_COPY_ALLOWED,
    // which "simulates the move by using the CopyFile and DeleteFile
    // functions" — silently copying INTO the existing target and
    // reintroducing exactly this defect.
    //
    // CROSS-PLATFORM, determined by reading the sources rather than
    // assuming: [fs.op.rename] specifies rename "as if by POSIX rename",
    // and "if new_p is an existing non-directory file, new_p is removed" —
    // so replace-over-existing is the STANDARD's contract, not a POSIX
    // extra. MSVC's STL implements it as
    // `MoveFileExW(src, dst, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)`
    // (microsoft/STL `stl/src/filesystem.cpp`), and MOVEFILE_REPLACE_EXISTING
    // replaces an existing target. So plain `std::filesystem::rename` is
    // correct on every host we build for and needs no `_WIN32` arm. The one
    // residual Windows difference — it does not use POSIX-semantics rename,
    // so a target held OPEN by another process (a running .exe, a scanner)
    // refuses to be replaced — is NOT a regression: the `trunc` open this
    // replaces failed on the very same input one step earlier. It is
    // handled the same way every other failure here is: loudly, with the
    // temp cleaned up. The rename diagnostic names that cause explicitly.
    //
    // KNOWN, ACCEPTED CONSEQUENCES (inherent to getting a new identity):
    //   * Overwriting no longer inherits the old file's permission bits;
    //     the artifact is created at the host umask. `writeImage` re-applies
    //     the load-bearing one (D-OUTPUT-EXEC-BIT) after this returns.
    //   * If `path` is a symlink or a hard link, the rename replaces THAT
    //     NAME rather than writing through to the shared file. That is the
    //     point — writing through is what reuses the inode.
    // ─────────────────────────────────────────────────────────────────

    // Claim a staging temp ATOMICALLY. `std::ios::noreplace` (C++23
    // P2467R1) is `fopen` mode "x" — O_CREAT|O_EXCL — so the open succeeds
    // only when THIS call created the file. That check-and-create is atomic
    // in the OS, making it race-free against concurrent processes AND
    // making it step over a stale temp left by a killed run instead of
    // silently sharing it (the `scratch_dir.hpp` lesson; see `processSeed`).
    static std::atomic<std::uint64_t> tempCounter{0};
    auto const                        seed = processSeed();
    constexpr std::uint32_t           kMaxClaimAttempts = 1000;

    std::filesystem::path tempPath;
    std::ofstream         out;
    for (std::uint32_t attempt = 0;; ++attempt) {
        if (attempt >= kMaxClaimAttempts) {
            emit(reporter, DiagnosticCode::K_ImageWriteOpenFailed,
                 std::string{"link::writeBytes: could not claim a unique "
                             "staging temp next to '"}
                     + pathForDiag(path) + "' after "
                     + std::to_string(kMaxClaimAttempts)
                     + " attempts. Stale '.dsstmp-*' files are accumulating "
                       "in that directory — an earlier run was killed before "
                       "it could clean up. Delete them and re-run.");
            return false;
        }
        // Sibling by construction: the target path plus an ASCII suffix.
        // `operator+=` concatenates in the path's NATIVE encoding, so this
        // never narrows a wide path (the MSVC throw hazard `pathForDiag`
        // documents) and never touches the directory portion.
        auto candidate = path;
        candidate += ".dsstmp-" + std::to_string(seed) + "-"
                   + std::to_string(tempCounter.fetch_add(1));

        std::ofstream claim(
            candidate,
            std::ios::binary | std::ios::out | std::ios::noreplace);
        if (claim) {
            tempPath = std::move(candidate);
            out      = std::move(claim);
            break;
        }
        // The open failed, and the two causes need opposite responses.
        // If the candidate EXISTS, another writer (or a stale temp) holds
        // that slot — take the next one. Otherwise this is a real error
        // (permission denied, parent removed post-stat, path component is
        // not a directory, invalid filename, disk full) and spinning would
        // only turn it into a slow, misleading failure. Fail loud now.
        std::error_code eec;
        bool const      taken = std::filesystem::exists(candidate, eec);
        if (!eec && taken) {
            continue;
        }
        emit(reporter, DiagnosticCode::K_ImageWriteOpenFailed,
             std::string{"link::writeBytes: failed to create the staging "
                         "temp '"}
                 + pathForDiag(candidate)
                 + "' for binary write (permission denied, a path component "
                   "is not a directory, invalid filename, or parent removed "
                   "post-stat). The artifact '"
                 + pathForDiag(path) + "' was NOT written.");
        return false;
    }

    out.write(reinterpret_cast<char const*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        emit(reporter, DiagnosticCode::K_ImageWriteShort,
             std::string{"link::writeBytes: short write to the staging temp "
                         "for '"}
                 + pathForDiag(path) + "' (disk full or I/O error). The "
                   "previous artifact at that path is unchanged."
                 + removeTempNote(tempPath));
        return false;
    }
    out.close();
    if (!out) {
        // close() can fail when buffered writes flush to disk.
        // ofstream destruction would silently swallow this; we
        // surface it as a write failure. Because the bytes were staged in a
        // temp, the artifact at `path` is still the PREVIOUS good one — the
        // partial file is the temp, and it is removed.
        emit(reporter, DiagnosticCode::K_ImageWriteCloseFailed,
             std::string{"link::writeBytes: close() failed for the staging "
                         "temp for '"}
                 + pathForDiag(path)
                 + "' (deferred I/O error on flush — the staged bytes are "
                   "incomplete). The previous artifact at that path is "
                   "unchanged." + removeTempNote(tempPath));
        return false;
    }

    // COMMIT. Same-directory rename over the target: atomic on POSIX,
    // MOVEFILE_REPLACE_EXISTING on Windows. A failure here must NEVER be
    // reported as success — that would leave the stale previous artifact in
    // place while the caller believes it shipped fresh bytes, which is the
    // precise silent-failure class this substrate exists to prevent.
    std::error_code rec;
    std::filesystem::rename(tempPath, path, rec);
    if (rec) {
        emit(reporter, DiagnosticCode::K_ImageWriteOpenFailed,
             std::string{"link::writeBytes: staged the bytes but could not "
                         "rename the temp over '"}
                 + pathForDiag(path) + "': " + rec.message()
                 + ". The artifact was NOT updated — any file at that path "
                   "is the PREVIOUS build's output. Likely causes: the "
                   "target is a directory, the target or its directory is "
                   "not writable, the parent was removed mid-write, or (on "
                   "Windows) the target is currently open/running and so "
                   "cannot be replaced."
                 + removeTempNote(tempPath));
        return false;
    }
    return true;
}

} // namespace dss::linker
