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

} // namespace dss
