#include "program/input_resolver.hpp"

#include "core/substrate/path_identity.hpp"  // genericSpelling, walkTree
#include "core/types/parse_diagnostic.hpp"

#include <algorithm>
#include <optional>
#include <system_error>

namespace dss {

namespace {

namespace fs = std::filesystem;

void emit(DiagnosticReporter& rep, DiagnosticCode code, std::string msg) {
    dss::report(rep, code, DiagnosticSeverity::Error, std::move(msg));
}

// The WARNING twin. Separate from `emit` rather than a severity parameter on
// it, so that a site cannot silently drop from Error to Warning by editing one
// argument — the two are different contracts and read differently at the call.
void warn(DiagnosticReporter& rep, DiagnosticCode code, std::string msg) {
    dss::report(rep, code, DiagnosticSeverity::Warning, std::move(msg));
}

// Common extension match for both recursive + flat scans. Returns
// true iff the file's extension is in the allow-list.
[[nodiscard]] bool extensionMatches(
        fs::path const& path,
        std::span<std::string const> fileExtensions) {
    auto const ext = path.extension().string();
    return std::any_of(fileExtensions.begin(), fileExtensions.end(),
                       [&](auto const& e) { return e == ext; });
}

} // namespace

bool InputResolver::resolveDirectory(
        fs::path const&                directoryPath,
        std::span<std::string const>   fileExtensions,
        Mode                           mode,
        std::vector<std::string>&      out,
        DiagnosticReporter&            reporter) {
    std::error_code       ec;
    core::EntryKind const root = core::reachedKind(directoryPath, ec);
    // What the directory REACHES cannot be examined (a looping link, a refusal): that is no
    // absence, and saying "does not exist" would send the reader after the wrong cause.
    if (ec) {
        emit(reporter, DiagnosticCode::D_DirectoryScanFailed,
             "InputResolver: what '" + core::genericSpelling(directoryPath)
             + "' reaches cannot be examined: " + ec.message());
        return false;
    }
    if (root != core::EntryKind::Directory) {
        emit(reporter, DiagnosticCode::D_FileNotFound,
             "InputResolver: '" + core::genericSpelling(directoryPath)
             + "' does not exist or is not a directory.");
        return false;
    }

    // ONE walk for both shapes — the substrate's (`core::walkTree`), which never
    // descends a LINK on any build: the flat shape prunes every directory, the
    // recursive one descends every real one. The closed-enum `Mode` keeps the
    // dispatch explicit at the call site (caller passes the policy; resolver
    // doesn't infer from path / cwd / env).
    // ✔MEASURED 2026-10-20 (the substrate's header): libstdc++ on Windows cannot
    // tell a link from what it reaches, so the MinGW build's
    // `recursive_directory_iterator` DESCENDED a directory link the MSVC build's
    // did not, and took a dangling file link for a regular file — one directory,
    // two input lists. A link is an input by what it REACHES: a link to a regular
    // file is that file under its name here; a dangling one is no input.
    // ★ A LINK WHOSE REACH CANNOT BE EXAMINED (a loop, a refusal) is handed out by the walk and
    // asked about ONLY when its name is an input's (the configured extensions): an unrelated one
    // (`docs/self`) leaves the scan whole, as bash, Python's glob and CMake's GLOB_RECURSE list
    // such a tree; one NAMED like a source is an input nobody can read, refused naming it --
    // what gcc and clang answer for a candidate they cannot open (✔MEASURED 2026-10-28, a
    // looping include candidate: both fatal, naming it).
    std::vector<std::string>       matched;
    std::optional<core::TreeEntry> unreadable;
    auto const walk = core::walkTree(directoryPath, [&](core::TreeEntry const& entry) {
        if (entry.kind == core::EntryKind::Directory)
            return mode == Mode::Recursive ? core::WalkStep::Continue : core::WalkStep::Prune;
        if (entry.reachError) {
            if (!extensionMatches(entry.path, fileExtensions)) return core::WalkStep::Continue;
            unreadable = entry;
            return core::WalkStep::Stop;
        }
        if (entry.reaches != core::EntryKind::File) return core::WalkStep::Continue;
        if (extensionMatches(entry.path, fileExtensions)) {
            // Not a message — the INPUT LIST the compiler goes on to open.
            // The walk joins `directoryPath` with each name, so every entry
            // inherits that path's root, authority included.
            matched.push_back(core::genericSpelling(entry.path));
        }
        return core::WalkStep::Continue;
    }, core::TreeWalkOptions{.handOutUnreadableLinks = true});
    if (unreadable) {
        emit(reporter, DiagnosticCode::D_DirectoryScanFailed,
             "InputResolver: '" + core::genericSpelling(unreadable->path)
             + "' is named like an input of '" + core::genericSpelling(directoryPath)
             + "', and what it reaches cannot be examined: " + unreadable->reachError.message());
        return false;
    }
    if (walk.error) {
        // The directory itself, or one under it: the two remediations differ, so
        // the two messages do.
        emit(reporter, DiagnosticCode::D_DirectoryScanFailed,
             walk.where == directoryPath
                 ? "InputResolver: failed to open directory '"
                       + core::genericSpelling(directoryPath) + "': " + walk.error.message()
                 : "InputResolver: directory-scan interrupted after "
                   "partial enumeration of '"
                       + core::genericSpelling(directoryPath) + "' (at '"
                       + core::genericSpelling(walk.where) + "'): " + walk.error.message());
        return false;
    }

    if (matched.empty()) {
        emit(reporter, DiagnosticCode::D_EmptyInput,
             "InputResolver: no files in '"
             + core::genericSpelling(directoryPath)
             + "' match the configured extensions.");
        return false;
    }

    // Deterministic ordering for reproducible builds.
    std::sort(matched.begin(), matched.end());
    // Dedup (cheap insurance against fs iterators that double-yield
    // on some platforms — observed once on Windows symlink edges).
    matched.erase(std::unique(matched.begin(), matched.end()), matched.end());

    out.insert(out.end(),
               std::make_move_iterator(matched.begin()),
               std::make_move_iterator(matched.end()));
    return true;
}

bool InputResolver::validateFiles(
        std::span<std::string const>    inputs,
        std::vector<std::string>&       out,
        DiagnosticReporter&             reporter) {
    // Contract: appends to `out`. Caller owns clearing if the
    // vector is reused across multiple resolve cycles. Mirrors the
    // append-semantics of `resolveDirectory` for consistency.
    // (code-reviewer F5 post-fold: documented explicitly.)
    bool allOk = true;
    out.reserve(out.size() + inputs.size());
    for (auto const& path : inputs) {
        // What the path REACHES, asked of the substrate (a dangling link reaches nothing, on every
        // build); a clean absence is the missing-file arm, as `status` reported it with ENOENT.
        std::error_code       ec;
        core::EntryKind const reached = core::reachedKind(path, ec);
        if (!ec && reached == core::EntryKind::Absent)
            ec = std::make_error_code(std::errc::no_such_file_or_directory);
        bool const isRegular = reached == core::EntryKind::File;
        if (ec) {
            // Distinguish ENOENT (file truly missing — D_FileNotFound)
            // from other filesystem errors (permission denied, broken
            // symlink chain, I/O error — D_DirectoryScanFailed).
            // Different remediations per the codebase's "remediation-
            // distinct codes" rule. (code-reviewer F7 post-fold split.)
            if (ec == std::errc::no_such_file_or_directory) {
                emit(reporter, DiagnosticCode::D_FileNotFound,
                     "InputResolver: source file '" + path
                     + "' does not exist.");
            } else {
                emit(reporter, DiagnosticCode::D_DirectoryScanFailed,
                     "InputResolver: filesystem error checking source "
                     "file '" + path + "': " + ec.message());
            }
            allOk = false;
            continue;
        }
        if (!isRegular) {
            emit(reporter, DiagnosticCode::D_FileNotFound,
                 "InputResolver: source file '" + path
                 + "' exists but is not a regular file (directory, "
                   "device, or socket).");
            allOk = false;
            continue;
        }
        out.push_back(path);
    }
    return allOk;
}

bool InputResolver::checkSearchDirectoriesUsable(
        std::span<std::string const>    directories,
        std::string_view                optionSpelling,
        DiagnosticReporter&             reporter) {
    // See the header for WHY this warns instead of refusing (a measured
    // gcc/MSVC survey) and why absoluteness is never asked.
    std::string const opt{optionSpelling};
    bool allOk = true;
    for (auto const& dir : directories) {
        // An EMPTY argument is its own case and would otherwise be reported
        // as "the current directory is fine": `fs::directory_iterator("")`
        // fails, but `exists("")` is false, so it would land in the absent
        // arm with an empty name in the message and tell the reader nothing.
        if (dir.empty()) {
            warn(reporter, DiagnosticCode::D_FileNotFound,
                 "InputResolver: " + opt + " was given an EMPTY directory "
                 "argument, which names no directory and contributes no "
                 "header to the search path. It is IGNORED; a header that "
                 "was meant to be found through it will be reported as "
                 "missing at its `#include`.");
            allOk = false;
            continue;
        }
        std::error_code ec;
        // ★ ONE QUESTION, ASKED OF THE FILESYSTEM DIRECTLY: can this be
        //   enumerated? The substrate's listing's own failure is the authority,
        //   because it is the exact operation the include search performs
        //   (`core::walkTree`, at any length -- P69 H6). It is asked to stop at
        //   the first entry: listing the directory whole is what proves it
        //   enumerates, and nothing below it is asked.
        //   The `exists` / `is_directory` split below runs only to CLASSIFY
        //   a failure that already happened, so a directory that enumerates
        //   fine is never described by anything but success — and no
        //   pre-check can disagree with the operation it is predicting.
        core::TreeWalkOptions handOut;
        handOut.handOutUnreadableLinks = true;   // as the include search lists: by name
        ec = core::walkTree(fs::path{dir}, [](core::TreeEntry const&) { return core::WalkStep::Stop; }, handOut)
                 .error;
        if (!ec) continue;                     // enumerable — nothing to say

        std::error_code       existsEc;
        core::EntryKind const reached = core::reachedKind(dir, existsEc);
        bool const            present = reached != core::EntryKind::Absent;
        std::error_code const dirEc   = existsEc;
        bool const            isDir   = reached == core::EntryKind::Directory;
        if (!present && !existsEc) {
            warn(reporter, DiagnosticCode::D_FileNotFound,
                 "InputResolver: " + opt + " names '" + dir
                 + "', which does not exist. It contributes no header to "
                   "the search path and is IGNORED; a header that was meant "
                   "to be found through it will be reported as missing at "
                   "its `#include` rather than here.");
        } else if (present && !isDir && !dirEc) {
            warn(reporter, DiagnosticCode::D_DirectoryScanFailed,
                 "InputResolver: " + opt + " names '" + dir
                 + "', which exists but is NOT a directory. It is IGNORED; "
                   "a header that was meant to be found through it will be "
                   "reported as missing at its `#include` rather than here.");
        } else {
            // Present, a directory, and STILL not enumerable — permission
            // denied, an I/O error, an unreachable network authority. This
            // is the arm the row was filed for, and the one whose cause is
            // invisible from a missing-header message. ★ It is NOT passed
            // over by the search: a header search that reaches it ENDS there,
            // naming it (the search reads a listing to answer the case
            // question, and a listing it cannot read is no absence) -- as gcc
            // and clang end theirs at a candidate they cannot open.
            warn(reporter, DiagnosticCode::D_DirectoryScanFailed,
                 "InputResolver: " + opt + " names '" + dir
                 + "', which cannot be enumerated: " + ec.message()
                 + ". A header search that reaches it ends there and is "
                   "refused, naming it; a header found in an earlier "
                   "directory never asks it.");
        }
        allOk = false;
    }
    return allOk;
}

} // namespace dss
