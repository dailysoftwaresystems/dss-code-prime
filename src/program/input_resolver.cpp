#include "program/input_resolver.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <algorithm>
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
    std::error_code ec;
    if (!fs::exists(directoryPath, ec) || !fs::is_directory(directoryPath, ec)) {
        emit(reporter, DiagnosticCode::D_FileNotFound,
             "InputResolver: '" + directoryPath.generic_string()
             + "' does not exist or is not a directory.");
        return false;
    }

    // Two iterator shapes — recursive and flat — share the same
    // extension-filter + ec-handling. The closed-enum `Mode` keeps
    // the dispatch explicit at the call site (caller passes the
    // policy; resolver doesn't infer from path / cwd / env).
    std::vector<std::string> matched;
    auto const captureMatches = [&](auto& iter) -> bool {
        using IterT = std::decay_t<decltype(iter)>;
        IterT const end{};
        for (; iter != end; iter.increment(ec)) {
            if (ec) {
                emit(reporter, DiagnosticCode::D_DirectoryScanFailed,
                     "InputResolver: directory-scan interrupted after "
                     "partial enumeration of '"
                     + directoryPath.generic_string() + "': "
                     + ec.message());
                return false;
            }
            if (!iter->is_regular_file()) continue;
            if (extensionMatches(iter->path(), fileExtensions)) {
                matched.push_back(iter->path().generic_string());
            }
        }
        return true;
    };

    bool scanOk = false;
    if (mode == Mode::Recursive) {
        fs::recursive_directory_iterator it(directoryPath, ec);
        if (ec) {
            emit(reporter, DiagnosticCode::D_DirectoryScanFailed,
                 "InputResolver: failed to open directory '"
                 + directoryPath.generic_string() + "': " + ec.message());
            return false;
        }
        scanOk = captureMatches(it);
    } else {
        fs::directory_iterator it(directoryPath, ec);
        if (ec) {
            emit(reporter, DiagnosticCode::D_DirectoryScanFailed,
                 "InputResolver: failed to open directory '"
                 + directoryPath.generic_string() + "': " + ec.message());
            return false;
        }
        scanOk = captureMatches(it);
    }
    if (!scanOk) return false;

    if (matched.empty()) {
        emit(reporter, DiagnosticCode::D_EmptyInput,
             "InputResolver: no files in '"
             + directoryPath.generic_string()
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
        std::error_code ec;
        bool const isRegular = fs::is_regular_file(path, ec);
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
        //   enumerated? `directory_iterator`'s own failure is the authority,
        //   because it is the exact operation the include search performs.
        //   The `exists` / `is_directory` split below runs only to CLASSIFY
        //   a failure that already happened, so a directory that enumerates
        //   fine is never described by anything but success — and no
        //   pre-check can disagree with the operation it is predicting.
        fs::directory_iterator const probe(dir, ec);
        if (!ec) continue;                     // enumerable — nothing to say

        std::error_code       existsEc;
        bool const            present = fs::exists(dir, existsEc);
        std::error_code       dirEc;
        bool const            isDir   = fs::is_directory(dir, dirEc);
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
            // invisible from a missing-header message.
            warn(reporter, DiagnosticCode::D_DirectoryScanFailed,
                 "InputResolver: " + opt + " names '" + dir
                 + "', which cannot be enumerated: " + ec.message()
                 + ". It contributes no header to the search path and is "
                   "IGNORED; a header that was meant to be found through it "
                   "will be reported as missing at its `#include` rather "
                   "than here.");
        }
        allOk = false;
    }
    return allOk;
}

} // namespace dss
