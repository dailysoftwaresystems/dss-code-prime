#pragma once

// What a path REACHES, asked of the substrate -- the ONE copy, shared by both corpus runners
// (`integrated_tests/runner.cpp`, which drives the compiler as a subprocess, and `tests/examples/examples_runner.cpp`,
// which runs it in-process).
//
// ★ EVERY QUESTION A RUNNER ASKS OF A PATH IS ASKED OF THE SUBSTRATE (src/core/substrate/path_identity.hpp), never of
// the library: libstdc++ on Windows answers a link from the LINK's own attributes -- a link to a file is 0 bytes long
// and was written when the link was made, a dangling link is a regular file (✔MEASURED 2026-10-20 and 2026-10-21,
// the substrate's header) -- so the MinGW build of a runner read another file than the one it named. The two runners
// used to disagree on exactly that: the CLI runner asked the substrate, the in-process one the library, so a
// `prebuiltLibraries` entry that is a file link failed one runner ("file_size 0 bytes, read N") and passed the other
// (P69 fold H5, the review of H4b's finding F4). One home means one answer; tests/core/test_filesystem_queries_guard
// judges both runners, so neither can ask the library again.
//
// These keep the library's `error_code` contract: a size or a time of a path that reaches no file is an ERROR, named.
// The header references the substrate and the standard library alone: the two runners link different sets
// (`dss_examples_runner` links dsscp-lib; `integrated_tests` compiles the substrate's unit beside itself), and both
// hold the substrate.

#include "core/substrate/path_identity.hpp"

#include <cstdint>
#include <filesystem>
#include <system_error>

namespace dss::test_support {

// Whether `p` reaches anything; `ec` set when nobody could tell.
[[nodiscard]] inline bool reachesSomething(std::filesystem::path const& p, std::error_code& ec) {
    return dss::core::reachedKind(p, ec) != dss::core::EntryKind::Absent;
}

// Whether `p` reaches an entry of `kind`; false (with `ec` set) when nobody could tell.
[[nodiscard]] inline bool reachesKind(std::filesystem::path const& p, dss::core::EntryKind kind,
                                      std::error_code& ec) {
    return dss::core::reachedKind(p, ec) == kind && !ec;
}

// Why `p` has no size or time to give, when the substrate answered "nothing".
[[nodiscard]] inline std::error_code whyNoFile(std::filesystem::path const& p) {
    std::error_code kec;
    auto const      kind = dss::core::reachedKind(p, kec);
    if (kec) return kec;
    if (kind == dss::core::EntryKind::Absent) return std::make_error_code(std::errc::no_such_file_or_directory);
    if (kind == dss::core::EntryKind::Directory) return std::make_error_code(std::errc::is_a_directory);
    return std::make_error_code(std::errc::not_supported);
}

// The size of the regular file `p` reaches; `ec` set (and 0) when it reaches none.
[[nodiscard]] inline std::uintmax_t reachedSize(std::filesystem::path const& p, std::error_code& ec) {
    auto const size = dss::core::reachedFileSize(p, ec);
    if (!size && !ec) ec = whyNoFile(p);
    return size.value_or(0);
}

// When the file `p` reaches was last written; `ec` set when it reaches nothing.
[[nodiscard]] inline std::filesystem::file_time_type writtenAt(std::filesystem::path const& p, std::error_code& ec) {
    auto const when = dss::core::reachedWriteTime(p, ec);
    if (!when && !ec) ec = whyNoFile(p);
    return when.value_or(std::filesystem::file_time_type{});
}

}  // namespace dss::test_support
