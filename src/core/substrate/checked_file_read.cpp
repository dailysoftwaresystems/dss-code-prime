#include "core/substrate/checked_file_read.hpp"

#include <fstream>
#include <ios>
#include <istream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace dss::core {

namespace {

// One renderer for "n of m bytes", so the two failing arms cannot describe the
// same fact two ways.
[[nodiscard]] std::string byteTally(std::uintmax_t got, std::uintmax_t expected) {
    return std::to_string(got) + " of " + std::to_string(expected) + " bytes";
}

} // namespace

std::expected<std::string, FileReadError>
readStreamChecked(std::istream& in, std::string_view label,
                  std::uintmax_t expectedBytes) {
    std::string out;

    // ── OUTPUT SIDE ──────────────────────────────────────────────────────────
    // Sized in ONE allocation, and read directly into it: no `ostringstream`
    // and no second copy out of it. An allocation failure here is an
    // OUTPUT-side failure and is named as such -- the caller must be able to
    // tell "this machine ran out of memory" from "this file is bad", because
    // the two have nothing to do with each other and only one of them is
    // actionable by editing the config.
    try {
        if (expectedBytes > static_cast<std::uintmax_t>(out.max_size())) {
            throw std::length_error("size exceeds std::string::max_size()");
        }
        out.resize(static_cast<std::size_t>(expectedBytes));
    } catch (std::bad_alloc const&) {
        return std::unexpected(FileReadError{
            FileReadFailure::OutputError,
            "out of memory reading '" + std::string{label} + "' (output side, "
                + std::to_string(expectedBytes) + " bytes requested)"});
    } catch (std::length_error const&) {
        return std::unexpected(FileReadError{
            FileReadFailure::OutputError,
            "cannot hold the contents of '" + std::string{label}
                + "' (output side, " + std::to_string(expectedBytes)
                + " bytes exceeds the maximum a string can address)"});
    }

    // ── INPUT SIDE ───────────────────────────────────────────────────────────
    // `istream::read` (not `<< rdbuf()`) because it is the only one of the two
    // whose failure the caller can SEE: a streambuf that throws mid-read sets
    // `badbit` here, and sets nothing observable on the drain form.
    std::uintmax_t got = 0;
    if (expectedBytes > 0) {
        in.read(out.data(), static_cast<std::streamsize>(expectedBytes));
        got = static_cast<std::uintmax_t>(in.gcount());
    }

    if (in.bad()) {
        return std::unexpected(FileReadError{
            FileReadFailure::InputError,
            "I/O error after open while reading '" + std::string{label}
                + "' (input side, " + byteTally(got, expectedBytes)
                + " obtained). This is a READ failure, not a content error."});
    }

    if (got != expectedBytes) {
        out.resize(static_cast<std::size_t>(got));
        return std::unexpected(FileReadError{
            FileReadFailure::TornRead,
            "torn read of '" + std::string{label} + "': measured "
                + std::to_string(expectedBytes) + " bytes but obtained "
                + std::to_string(got)
                + ". The file was changed by another process while it was being"
                  " read, or the read was cut short. This is a READ failure, not"
                  " a content error."});
    }

    // ── AND THE OTHER DIRECTION ──────────────────────────────────────────────
    // A file that GREW after it was measured reads `expectedBytes` cleanly and
    // looks perfect -- the prefix parses, the tail is gone, nothing complains.
    // That is the same silent truncation from the other side, so probe for one
    // more byte. At a true end of file this costs one `underflow` that returns
    // eof; it never costs a second read of the body.
    if (expectedBytes > 0) {
        char extra = 0;
        in.read(&extra, 1);
        if (in.gcount() > 0) {
            return std::unexpected(FileReadError{
                FileReadFailure::TornRead,
                "torn read of '" + std::string{label} + "': measured "
                    + std::to_string(expectedBytes)
                    + " bytes but the file still had more to give. The file was"
                      " changed by another process while it was being read."
                      " This is a READ failure, not a content error."});
        }
    }

    return out;
}

std::expected<std::string, FileReadError>
readFileChecked(fs::path const& path) {
    // `generic_string()` throughout: one spelling of a path in a diagnostic, on
    // every host. (Several call sites already rendered it this way and their
    // wording is asserted by tests.)
    std::string const shown = path.generic_string();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(FileReadError{
            FileReadFailure::OpenFailed,
            "failed to open '" + shown + "' for reading"});
    }

    // AFTER the open, so a MISSING file reports "failed to open" (what every
    // caller reported before this helper existed) rather than the narrower
    // not-a-regular-file wording. On Windows the open above already refuses a
    // directory; on POSIX it succeeds and the first read fails with EISDIR,
    // which without this check would surface as an empty file.
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return std::unexpected(FileReadError{
            FileReadFailure::NotARegularFile,
            "'" + shown + "' is not a regular file"});
    }

    // ★ MEASURED ON THIS HANDLE, NOT ON THE NAME. `fs::file_size(path)` would
    // re-resolve the path and could measure a DIFFERENT file object than the one
    // already open (a rename between the two calls); seeking the open stream
    // cannot. It is also the cheaper of the two on every host here.
    in.seekg(0, std::ios::end);
    std::streampos const endPos = in.tellg();
    in.seekg(0, std::ios::beg);
    if (!in || endPos < 0) {
        return std::unexpected(FileReadError{
            FileReadFailure::InputError,
            "I/O error after open while measuring '" + shown
                + "' (input side: the stream could not be positioned). This is a"
                  " READ failure, not a content error."});
    }

    return readStreamChecked(in, shown,
                             static_cast<std::uintmax_t>(endPos));
}

} // namespace dss::core
