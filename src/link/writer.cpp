#include "link/writer.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_pass_util.hpp"

#include <fstream>
#include <ios>
#include <string>

// Linker image file emission — plan 14 LK10 cycle 1 substrate
// implementation. See writer.hpp for the contract.

namespace dss::linker {

namespace {

void emit(DiagnosticReporter& reporter,
          DiagnosticCode code,
          std::string msg) {
    lir_pass_util::report(reporter, code,
                          DiagnosticSeverity::Error,
                          std::move(msg));
}

// Windows-safe path-to-string for diagnostic messages. `path::
// string()` performs narrowing from `wchar_t` on Windows and
// throws `std::system_error` when the path contains code units
// that can't be narrowed to the current locale's ANSI codepage.
// A throw inside the failure-reporting path would silently abort
// the writer mid-diagnostic. `generic_string()` returns the path
// as UTF-8-style narrow string and does not throw on non-ANSI
// content (silent-failure-hunter MEDIUM fold, LK10 cycle 1 post-
// fold review).
[[nodiscard]] std::string pathForDiag(std::filesystem::path const& p) {
    return p.generic_string();
}

} // namespace

bool writeImage(LinkedImage const&             image,
                std::filesystem::path const&   path,
                DiagnosticReporter&            reporter) {
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
    // The `ok()` check requires expectedFuncCount > 0 AND
    // resolvedFuncCount == expectedFuncCount; if ok() returned
    // true but bytes are empty, the walker is contract-broken.
    // Surface here.
    if (image.bytes.empty()) {
        emit(reporter, DiagnosticCode::K_ImageNotOk,
             std::string{"link::writeImage: LinkedImage.bytes is "
                         "empty despite ok() == true — the walker "
                         "produced no output. Substrate invariant "
                         "violation."});
        return false;
    }
    // Precondition 2: parent directory exists.
    auto const parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        bool const exists = std::filesystem::exists(parent, ec);
        if (ec) {
            emit(reporter, DiagnosticCode::K_ImageWriteParentMissing,
                 std::string{"link::writeImage: failed to stat "
                             "parent directory '"}
                     + pathForDiag(parent) + "': " + ec.message());
            return false;
        }
        if (!exists) {
            emit(reporter, DiagnosticCode::K_ImageWriteParentMissing,
                 std::string{"link::writeImage: parent directory "
                             "does not exist: '"}
                     + pathForDiag(parent)
                     + "'. The substrate does not auto-create "
                       "directories — the caller must call "
                       "std::filesystem::create_directories before "
                       "writeImage.");
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
             std::string{"link::writeImage: failed to open '"}
                 + pathForDiag(path)
                 + "' for binary write (permission denied, path "
                   "is a directory, invalid filename, empty path, "
                   "or parent removed post-stat).");
        return false;
    }
    out.write(reinterpret_cast<char const*>(image.bytes.data()),
              static_cast<std::streamsize>(image.bytes.size()));
    if (!out) {
        emit(reporter, DiagnosticCode::K_ImageWriteShort,
             std::string{"link::writeImage: short write to '"}
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
             std::string{"link::writeImage: close() failed for '"}
                 + pathForDiag(path)
                 + "' (deferred I/O error on flush — file on disk "
                   "may be incomplete).");
        return false;
    }
    return true;
}

} // namespace dss::linker
