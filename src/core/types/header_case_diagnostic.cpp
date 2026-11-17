#include "core/types/header_case_diagnostic.hpp"

#include "core/substrate/path_identity.hpp"  // genericSpellingU8 -- lossless AND
                                             // authority-preserving

#include <cstdint>
#include <cstdio>
#include <exception>
#include <type_traits>
#include <utility>

namespace dss {

namespace fs = std::filesystem;

namespace {

// Render an ON-DISK path into the narrow diagnostic text.
//
// ★ NOT `generic_string()` — `D-PP-HEADER-CASE-NON-ASCII-NAME-NARROWING-THROW`.
// On Windows that runs the native `wchar_t` name through the ACTIVE CODE PAGE,
// which MEASURABLY THROWS `std::system_error` on MS STL for any name the code
// page cannot represent (a plain CJK filename at ACP 1252), and produces
// TOOLCHAIN-DEPENDENT bytes for the names it can (CP1252 vs libstdc++'s UTF-8).
// A diagnostic whose entire job is to NAME the colliding files must not abort
// the process on exactly the names that are hardest to name, and its text must
// not depend on which C++ compiler built DSS. The `u8` form is lossless for
// every well-formed name and identical on every toolchain.
//
// ★ AND NOT `generic_u8string()` EITHER — `core::genericSpellingU8`. ✔MEASURED
// 2026-08-28 on a REACHABLE UNC directory: the `u8` sibling performs the SAME
// generic-format conversion as the narrow one and eats the leading separator
// run just as it does (`//localhost/C$/…` rendered `/localhost/C$/…`). The two
// losses are independent — one renames the CHARACTERS, the other renames the
// MACHINE — so escaping the first while keeping the second would have left this
// report wrong in the very case it was rewritten for: a header collision on a
// network share names two files on a drive that has neither.
//
// A native name can be text no encoding accepts (NTFS permits lone
// surrogates), and such a file still has to be IDENTIFIABLE and still has to
// stay DISTINCT from its neighbour or the report becomes a lie. Since P69 H6c2
// (T4) the rendering itself is TOTAL and LOSSLESS -- WTF-8, which round-trips
// every native name -- so the code-unit escape that stood here in a catch went
// with the throw.
[[nodiscard]] std::string renderCandidatePath(fs::path const& p) {
    std::u8string const u8 = core::genericSpellingU8(p);
    return std::string(reinterpret_cast<char const*>(u8.data()), u8.size());
}

} // namespace

std::string
headerCaseAmbiguityMessage(std::string_view          requested,
                           std::span<fs::path const> candidates) {
    // Why this is fatal rather than a pick: a directory holding two names that
    // differ only by case cannot exist on NTFS or on a default APFS/HFS+
    // volume at all, so ANY choice made here would resolve differently
    // depending on which host performed the build — the exact host-dependence
    // the `headerNameMatching` axis removes. Choosing "the exact match" would
    // be the same bug one layer down.
    std::string msg{"header name '"};
    msg += requested;
    msg += "' matches more than one file under the target's case-insensitive "
           "header-name convention; resolution would depend on the BUILD "
           "HOST's filesystem. Candidates:";
    for (fs::path const& c : candidates) {
        msg += "\n  ";
        msg += renderCandidatePath(c);
    }
    msg += "\nRemedy: rename so the names differ by more than ASCII case (a "
           "case-only pair cannot be checked out on Windows or default macOS).";
    return msg;
}

ParseDiagnostic
makeHeaderCaseAmbiguityDiagnostic(BufferId buffer, SourceSpan span,
                                  std::string_view          requested,
                                  std::span<fs::path const> candidates) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::F_HeaderNameCaseAmbiguous;
    d.severity = DiagnosticSeverity::Error;
    d.buffer   = buffer;
    d.span     = span;
    d.actual   = headerCaseAmbiguityMessage(requested, candidates);
    return d;
}

void reportHeaderCaseAmbiguity(DiagnosticReporter& reporter, BufferId buffer,
                               SourceSpan span, std::string_view requested,
                               std::span<fs::path const> candidates) {
    reporter.report(
        makeHeaderCaseAmbiguityDiagnostic(buffer, span, requested, candidates));
}

} // namespace dss
