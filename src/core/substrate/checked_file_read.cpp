#include "core/substrate/checked_file_read.hpp"

#include "core/substrate/path_identity.hpp"  // genericSpelling

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <ios>
#include <istream>
#include <limits>
#include <new>
#include <optional>
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

// A STREAM read to its end, in chunks, refused the moment it passes `maxBytes`: at most `maxBytes + 1` bytes are ever
// held, the one past the bound being how it is seen. No byte count to compare against exists (a stream has no size),
// so the only failures are the stream going bad, the bound, and the output side.
[[nodiscard]] std::expected<std::string, FileReadError>
drainStreamChecked(std::istream& in, std::string const& shown, std::uintmax_t maxBytes) {
    constexpr std::uintmax_t kChunk = 64u * 1024u;
    std::string              out;
    try {
        for (;;) {
            // `out.size() <= maxBytes` here, so the room left cannot wrap.
            std::uintmax_t const left = maxBytes - out.size();
            std::size_t const    want = static_cast<std::size_t>(left >= kChunk ? kChunk : left + 1);
            std::size_t const    at   = out.size();
            out.resize(at + want);
            in.read(out.data() + at, static_cast<std::streamsize>(want));
            std::size_t const got = static_cast<std::size_t>(in.gcount());
            out.resize(at + got);
            if (in.bad()) {
                return std::unexpected(FileReadError{
                    FileReadFailure::InputError,
                    "I/O error after open while reading '" + shown + "' (input side, a stream, "
                        + std::to_string(out.size()) + " bytes obtained). This is a READ failure, not a content"
                          " error."});
            }
            if (out.size() > maxBytes) {
                return std::unexpected(FileReadError{
                    FileReadFailure::TooLarge,
                    "'" + shown + "' is a stream (a device or a pipe) that gave more than "
                        + std::to_string(maxBytes) + " bytes, the most its reader holds"});
            }
            if (got < want) return out;   // the stream's end
        }
    } catch (std::bad_alloc const&) {
        return std::unexpected(FileReadError{
            FileReadFailure::OutputError,
            "out of memory reading '" + shown + "' (output side, a stream, " + std::to_string(out.size())
                + " bytes held)"});
    } catch (std::length_error const&) {
        return std::unexpected(FileReadError{
            FileReadFailure::OutputError,
            "cannot hold the contents of '" + shown + "' (output side, a stream past the maximum a string can"
                " address)"});
    }
}

// `readFileChecked` with or without a bound: `maxBytes` absent, a regular file alone is read (the one-argument form).
[[nodiscard]] std::expected<std::string, FileReadError>
readChecked(fs::path const& path, std::optional<std::uintmax_t> maxBytes) {
    // ONE spelling of a path in a diagnostic, on every host. (Several call
    // sites already rendered it this way and their wording is asserted by
    // tests.)
    //
    // ★ `genericSpelling` AND NOT `path.generic_string()`. This is the chokepoint
    // EVERY checked read reports through, so it is the single site where the
    // loss would reach the most messages: `readFileChecked` is handed whatever
    // the caller was given — a CLI argument, a config path, a descriptor — and
    // the four errors below are the only thing an operator sees when one of them
    // cannot be read. The two spellings are byte-identical for every path with a
    // leading separator run below 2, so no asserted wording moves.
    std::string const shown = genericSpelling(path);

    // The OS's reason for a refused open is read from `errno` the open left -- it is cleared first, so a stale one
    // is never reported: gcc and clang name it ("Permission denied"), and so does this.
    errno = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        int const why = errno;
        return std::unexpected(FileReadError{
            FileReadFailure::OpenFailed,
            "failed to open '" + shown + "' for reading"
                + (why != 0 ? " (" + std::generic_category().message(why) + ")" : std::string{})});
    }

    // AFTER the open, so a MISSING file reports "failed to open" (what every
    // caller reported before this helper existed) rather than the narrower
    // not-a-regular-file wording. On Windows the open above already refuses a
    // directory; on POSIX it succeeds and the first read fails with EISDIR,
    // which without this check would surface as an empty file. A STREAM -- a
    // device, a named pipe: `Other` -- is read only where the caller bounds it.
    std::error_code       ec;
    EntryKind const       kind   = reachedKind(path, ec);
    bool const            stream = kind == EntryKind::Other && maxBytes.has_value();
    if (kind != EntryKind::File && !stream) {
        return std::unexpected(FileReadError{
            FileReadFailure::NotARegularFile,
            "'" + shown + "' is not a regular file"});
    }
    if (stream) return drainStreamChecked(in, shown, *maxBytes);

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
    auto const size = static_cast<std::uintmax_t>(endPos);
    if (maxBytes && size > *maxBytes) {
        return std::unexpected(FileReadError{
            FileReadFailure::TooLarge,
            "'" + shown + "' is " + std::to_string(size) + " bytes, over the " + std::to_string(*maxBytes)
                + " its reader holds"});
    }

    return readStreamChecked(in, shown, size);
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
    return readChecked(path, std::nullopt);
}

std::expected<std::string, FileReadError>
readFileChecked(fs::path const& path, std::uintmax_t maxBytes) {
    return readChecked(path, maxBytes);
}

} // namespace dss::core
