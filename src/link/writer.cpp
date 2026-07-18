#include "link/writer.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_pass_util.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <system_error>

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
    // `writeBytes` (parent check + open + write + close, all fail-loud).
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
    // Open the file in binary truncate mode. `ofstream` failbit
    // covers permission denied, disk full, "path is a directory",
    // invalid path characters, AND the post-`exists()` TOCTOU
    // case where the parent vanished between the check and the
    // open. The diagnostic enumerates the most-likely causes
    // including the race window.
    std::ofstream out(path,
                      std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out) {
        emit(reporter, DiagnosticCode::K_ImageWriteOpenFailed,
             std::string{"link::writeBytes: failed to open '"}
                 + pathForDiag(path)
                 + "' for binary write (permission denied, path "
                   "is a directory, invalid filename, empty path, "
                   "or parent removed post-stat).");
        return false;
    }
    out.write(reinterpret_cast<char const*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        emit(reporter, DiagnosticCode::K_ImageWriteShort,
             std::string{"link::writeBytes: short write to '"}
                 + pathForDiag(path)
                 + "' (disk full or I/O error).");
        return false;
    }
    out.close();
    if (!out) {
        // close() can fail when buffered writes flush to disk.
        // ofstream destruction would silently swallow this; we
        // surface it as a write failure. The on-disk state is
        // partial / unknown after this.
        emit(reporter, DiagnosticCode::K_ImageWriteCloseFailed,
             std::string{"link::writeBytes: close() failed for '"}
                 + pathForDiag(path)
                 + "' (deferred I/O error on flush — file on disk "
                   "may be incomplete).");
        return false;
    }
    return true;
}

} // namespace dss::linker
