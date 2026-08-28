#pragma once

// [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]] — the ONE test fixture that
// produces a path whose spelling begins with a run of TWO OR MORE separators,
// i.e. one that names a location on another machine rather than on this drive.
//
// ★ ONE COPY, USED BY EVERY ARM THAT NEEDS IT. Three files now pin this row
// from different angles -- the resolver's search and the identity key's
// derivation in `tests/core`, and the DRIVER's `-I` threading in
// `tests/program` -- and all three need the same fixture; a second copy would
// let them drift into testing different things under one anchor id.
//
// ⚠ IT LIVES IN `tests/test_support` FOR THAT REASON, NOT IN `tests/core`.
// The third consumer is in another directory, and `tests/test_support` is the
// one place already on every test target's include path -- so a cross-tier
// fixture placed anywhere else buys a copy the moment a second tier needs it.
//
// ★★ NO PLATFORM MACRO, AND THE ANSWER IS MEASURED RATHER THAN ASSUMED. The
// rewrite is attempted and its result is accepted ONLY if the filesystem
// actually resolves it, so a host that cannot serve it says so by returning
// empty rather than by being asked what host it is. A path with no drive-letter
// prefix — every POSIX path — falls out at the first test.
//
// ⚠ AN EMPTY RETURN MUST PRODUCE A LOUD, NAMED `GTEST_SKIP`, never a silent one:
// the property is then UNMEASURED on that leg, which is not the same as passing.

#include <cstddef>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace dss::test_support {

// A UNC spelling of `local`, or EMPTY when this host offers none.
[[nodiscard]] inline std::filesystem::path
uncSpellingOf(std::filesystem::path const& local) {
    namespace fs = std::filesystem;
    fs::path::string_type const& s = local.native();
    if (s.size() < 3 || s[1] != fs::path::value_type{':'}) return {};
    fs::path::string_type unc;
    for (char const c : std::string_view{"//localhost/"})
        unc += static_cast<fs::path::value_type>(c);
    unc += s[0];                        // the drive letter
    unc += fs::path::value_type{'$'};   // its administrative share
    unc += s.substr(2);                 // the rest, separators untouched
    fs::path const  candidate{unc};
    std::error_code ec;
    if (!fs::exists(candidate, ec) || ec) return {};
    return candidate;
}

// How many separators does this spelling START with? A run of two or more is
// what carries an authority on the path models that give one no `root_name()`.
[[nodiscard]] inline std::size_t
leadingSeparatorRun(std::filesystem::path const& p) {
    namespace fs = std::filesystem;
    fs::path::string_type const& s = p.native();
    std::size_t                  n = 0;
    while (n < s.size()
           && (s[n] == fs::path::preferred_separator
               || s[n] == fs::path::value_type{'/'}))
        ++n;
    return n;
}

}  // namespace dss::test_support
