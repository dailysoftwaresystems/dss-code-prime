#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>

// ── THE ONE CHECKED WHOLE-FILE READ ────────────────────────────────────────────
//
// D-CORE-SHIPPED-CONFIG-LOADERS-DRAIN-A-STREAM-WITHOUT-CHECKING-IT
//
// ★★★ WHY THIS IS A SHARED FUNCTION AND NOT A CONVENTION. Every loader in this
// tree used to open an `ifstream`, check the OPEN, then write `buf << in.rdbuf()`
// and hand `buf.str()` to a JSON parser. ✔MEASURED at the commit this landed on:
// NINE such drains in `src/`, of which SIX checked nothing at all. A read that
// stopped early therefore reached the parser as a SHORT DOCUMENT, and the
// operator was told the config's CONTENTS were malformed when the truth was that
// the READ failed. An I/O error is not a parse error and must never be reported
// as one. A per-site `if (in.bad())` is not the fix — it is the same omission
// waiting for the tenth loader — so the drain itself lives here, once.
//
// ★★★ AND THE PER-SITE CHECK THAT THREE LOADERS ALREADY CARRIED WAS VACUOUS.
// ✔MEASURED (`scratchpad/p32/lane-b/stream_probe.cpp`, GCC 13.2 ucrt, libstdc++):
// `std::ostream::operator<<(std::streambuf*)` extracts through the STREAMBUF
// POINTER, so it never touches the `istream` OBJECT's state. `in.bad()` after
// `buf << in.rdbuf()` is FALSE in every case, including one where the streambuf
// THROWS mid-read:
//
//     normal file            in.bad=0   out.fail=0 out.bad=0
//     EMPTY file             in.bad=0   out.fail=1 out.bad=0   <- failbit, not bad
//     short-then-EOF buf     in.bad=0   out.fail=0 out.bad=0   <- SILENT truncation
//     throwing buf           in.bad=0   out.fail=1 out.bad=0   <- fail, NOT bad
//
// ⇒ Three conclusions that shape this interface:
//   * `in.bad()` around a `rdbuf()` drain asserts NOTHING. The three "checked"
//     sites were checking a bit the drain cannot set.
//   * `out.bad()` misses a THROWING streambuf (that is `failbit`), and
//     `out.fail()` cannot be used in its place because an EMPTY file sets
//     `failbit` too ("no characters inserted"). An empty config is a content
//     question, not a read failure.
//   * A short read that reports EOF -- which is exactly what `basic_filebuf`
//     produces when the underlying OS read FAILS, since libstdc++ maps a read
//     error to `eof()` rather than throwing -- is INDISTINGUISHABLE from a real
//     end of file at the iostream level. No combination of stream bits can see
//     it. **The only detector is a byte count.**
//
// ⇒ So this reads through `istream::read` (which DOES set `badbit` when the
// streambuf throws -- ✔MEASURED, same probe) and compares the bytes obtained
// against the size measured on the SAME OPEN HANDLE. A mismatch in either
// direction is a torn read and is refused BY NAME.
//
// ★ THE SIZE COMPARISON IS ALSO THE FIELD FIX for
// D-TEST-SHIPPED-CONFIG-READ-FROM-A-TREE-ANOTHER-PROCESS-IS-WRITING: a neighbour
// rewriting a shipped `.json` IN PLACE opens a short-file window, and a reader
// caught inside it now says "the file changed under the reader" instead of
// emitting a parse error against a prefix.
//
// ⚠ BINARY IS LOAD-BEARING, NOT A HABIT. In text mode a Windows CRLF file yields
// FEWER bytes than its size, so every such read would report itself torn. The
// open is unconditionally `std::ios::binary`, which is also what a byte-exact
// consumer (`#embed`, a golden, a source buffer) requires anyway.

namespace dss::core {

// WHY AN ENUM AND NOT JUST A MESSAGE: callers map into four different diagnostic
// vocabularies (`ConfigDiagnostic`, `DiagnosticReporter`, a `HeaderReadError`,
// and a thrown `runtime_error`), and several must keep their EXISTING open-failure
// wording for tests that assert on it. The kind lets a caller branch on WHAT
// failed without re-deriving it from the message text.
enum class FileReadFailure : std::uint8_t {
    OpenFailed,       // the open itself failed (missing, permissions, sharing)
    NotARegularFile,  // opened, but it is a directory / device / fifo
    InputError,       // a genuine mid-read failure: the stream went bad
    TornRead,         // the bytes obtained disagree with the size measured
    OutputError,      // allocation failure building the result (output side)
};

struct DSS_EXPORT FileReadError {
    FileReadFailure kind{FileReadFailure::OpenFailed};
    // Already NAMES the read, the path, and which side failed. A caller may
    // prefix its own context ("shipped-lib descriptor: ") but must not restate
    // the failure -- one wording, one place.
    std::string     message;
};

// Read `path` whole, or say precisely why not. Never returns a partial prefix.
[[nodiscard]] DSS_EXPORT std::expected<std::string, FileReadError>
readFileChecked(std::filesystem::path const& path);

// The SEAM the pins drive. `expectedBytes` is the size the caller measured for
// this exact stream; `label` names the thing being read in the diagnostic.
//
// ★ Required, not optional: the byte count IS the only detector for a silently
// truncated read (see the probe above), so a call site with no size to compare
// against would be the unchecked drain again under a new name.
[[nodiscard]] DSS_EXPORT std::expected<std::string, FileReadError>
readStreamChecked(std::istream& in, std::string_view label,
                  std::uintmax_t expectedBytes);

} // namespace dss::core
