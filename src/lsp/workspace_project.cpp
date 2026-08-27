#include "lsp/workspace_project.hpp"

#include "core/types/diagnostic_reporter.hpp"
// ★ THE ONE PROJECT-MANIFEST PARSER — now reached DOWNWARD, not across
// (D-LSP-PROJECT-CONFIG-LIVES-ABOVE-ITS-CONSUMERS, closed 2026-08-13).
// `core/types/project_config.hpp` is the very parser `Program::compileProject`
// runs; it used to live in `src/program/`, so this file reached UP into the
// driver tier for it. Writing a second reader of the same document was never an
// option — an editor that disagreed with the compiler about what a manifest
// MEANS would be worse than no editor support — so the parser moved down to the
// tier both readers already depend on.
#include "core/types/project_config.hpp"
#include "core/types/target_schema.hpp"
// ⚠ STILL REACHED ACROSS A LAYER: `TargetSpec::parse` is the driver's
// `<targetName>:<formatName>` splitter and cannot follow `project_config` down,
// because `TargetSpec::outputExtension` binds the type to
// `link/object_format_schema.hpp` and `core` must not depend on `link`.
// Anchored `D-LSP-TARGET-SPEC-SPLITTER-LIVES-ABOVE-ITS-CONSUMERS`; see the note
// in `src/lsp/CMakeLists.txt`.
#include "program/target_spec.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace dss::lsp {

namespace {

// Render the first diagnostic a `DiagnosticReporter`-based loader produced.
// `project_config.cpp` routes its prose through `ParseDiagnostic::actual` (via
// the shared `dss::report` shim), and every message it builds is already
// prefixed with the source label — so this is the whole user-facing text.
[[nodiscard]] std::string firstReported(DiagnosticReporter const& rep) {
    auto const all = rep.all();
    if (all.empty()) {
        // The loader returned failure without reporting. That is a defect in
        // the loader, not a state to paper over — say so rather than emit an
        // empty reason the operator cannot act on.
        return "the project-config loader failed without reporting a reason";
    }
    std::string out{diagnosticCodeName(all[0].code)};
    out += ": ";
    out += all[0].actual;
    return out;
}

// Render the first `ConfigDiagnostic` from a `LoadResult` failure.
[[nodiscard]] std::string firstConfigDiag(
    std::vector<ConfigDiagnostic> const& diags) {
    if (diags.empty()) return "no diagnostic was produced";
    std::string out{diagnosticCodeName(diags[0].code)};
    out += ": ";
    out += diags[0].message;
    return out;
}

[[nodiscard]] std::string joinPaths(std::vector<fs::path> const& paths) {
    std::string out;
    for (auto const& p : paths) {
        if (!out.empty()) out += ", ";
        out += p.string();
    }
    return out;
}

[[nodiscard]] std::string joinNames(std::vector<std::string> const& names) {
    std::string out;
    for (auto const& n : names) {
        if (!out.empty()) out += ", ";
        out += n;
    }
    return out;
}

// Collect every `*.dss-project.json` directly inside `root`, sorted.
// SORTED, not directory order: two developers on two filesystems must get the
// same answer out of the same tree, and the union below only stays
// order-independent if the inputs are.
void collectManifests(fs::path const& root, std::vector<fs::path>& out) {
    std::error_code ec;
    // Non-throwing overload: an unreadable / absent root contributes nothing
    // and is reported by the caller as "no manifest found", with the root
    // named. Throwing here would take down the LSP message loop for a mistyped
    // rootUri.
    for (auto const& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if (name.size() < kProjectFileSuffix.size()) continue;
        if (name.compare(name.size() - kProjectFileSuffix.size(),
                         kProjectFileSuffix.size(),
                         kProjectFileSuffix) != 0) continue;
        out.push_back(entry.path());
    }
}

[[nodiscard]] int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::string_view workspaceProjectErrorName(
    WorkspaceProjectErrorKind kind) noexcept {
    switch (kind) {
        case WorkspaceProjectErrorKind::NoWorkspaceRoot:
            return "NoWorkspaceRoot";
        case WorkspaceProjectErrorKind::ProjectFileNotFound:
            return "ProjectFileNotFound";
        case WorkspaceProjectErrorKind::ProjectFileLoadFailed:
            return "ProjectFileLoadFailed";
        case WorkspaceProjectErrorKind::TargetSpecMalformed:
            return "TargetSpecMalformed";
        case WorkspaceProjectErrorKind::TargetConfigLoadFailed:
            return "TargetConfigLoadFailed";
        case WorkspaceProjectErrorKind::TargetDeclaresNoAssemblyLanguage:
            return "TargetDeclaresNoAssemblyLanguage";
    }
    return "Unknown";
}

std::optional<fs::path> pathFromFileUri(std::string_view uri) {
    constexpr std::string_view kScheme = "file://";
    if (uri.size() < kScheme.size()) return std::nullopt;
    if (uri.compare(0, kScheme.size(), kScheme) != 0) return std::nullopt;
    auto rest = uri.substr(kScheme.size());

    // Authority. `file:///path` (empty) and `file://localhost/path` are the two
    // spellings clients emit. A real remote host is NOT a local directory, so
    // it is refused rather than silently treated as one.
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    const auto authority = rest.substr(0, slash);
    if (!authority.empty() && authority != "localhost") return std::nullopt;
    rest = rest.substr(slash);

    std::string decoded;
    decoded.reserve(rest.size());
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size()) {
            const int hi = hexValue(rest[i + 1]);
            const int lo = hexValue(rest[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(rest[i]);
    }

    // `/C:/dir` is the URI spelling of the Windows path `C:/dir`. The leading
    // slash is part of the URI grammar, not of the path.
    if (decoded.size() >= 3 && decoded[0] == '/'
        && (std::isalpha(static_cast<unsigned char>(decoded[1])) != 0)
        && decoded[2] == ':') {
        decoded.erase(0, 1);
    }
    if (decoded.empty()) return std::nullopt;

    // A URI path is percent-encoded UTF-8 BY DEFINITION (RFC 3986 §2.5). The
    // `std::string` ctor of `fs::path` interprets bytes in the platform's
    // NARROW encoding, which on Windows is the ACP — so a non-ASCII workspace
    // path would round-trip to mojibake and the manifest scan would silently
    // find nothing. The `u8string` ctor is the one that states the encoding.
    return fs::path(std::u8string(
        reinterpret_cast<char8_t const*>(decoded.data()), decoded.size()));
}

// ── D-LSP-POSITIONS-RESOLVED-IN-SYNTHESIZED-PREPROCESSOR-COORDINATES ────────
//
// The EXACT INVERSE of `pathFromFileUri`, and it lives beside it for the reason
// this repo keeps re-learning: two halves of one encoding, written apart, drift.
//
// ⚠ IT DID NOT EXIST IN `src/` AT ALL. Only `tests/lsp/lsp_test_helpers.hpp`
// had one, so production had no way to NAME a file other than the one the
// request came in on — which is precisely why `locationJson` hardcoded the
// request's uri and a definition inside a header was reported as if it were in
// the open document. A helper that exists only in the test tree is a helper the
// product cannot use, and the test then measures its own copy.
//
// The three shapes that have to round-trip, all covered by
// `WorkspaceProject.FileUriRoundTrip`:
//   * percent-encoding — every byte outside the RFC 3986 unreserved set is
//     encoded, so a space or a `#` in a path cannot terminate the uri early;
//   * WINDOWS DRIVE LETTERS — `C:/dir` is `file:///C:/dir`: the URI grammar's
//     leading slash is added here and stripped there. `:` is deliberately NOT
//     encoded, matching what every LSP client emits;
//   * UNC — `//server/share` is `file://server/share`, so the authority is the
//     server rather than empty. `pathFromFileUri` refuses a non-`localhost`
//     authority, which is correct for INBOUND (a remote host is not a local
//     directory) and is why a UNC path round-trips to a uri but not back.
//     Stated, not silently asymmetric.
//
// A path is percent-encoded UTF-8 BY DEFINITION (RFC 3986 §2.5), so the bytes
// come from `u8string()` — the narrow `string()` would emit ACP bytes on
// Windows and produce mojibake in the editor, the mirror of the note above.
std::string fileUriFromPath(std::string_view path) {
    // Text in, uri out: the caller keeps no filesystem surface. See the header
    // for why this overload exists (check-path-identity, and a caller that was
    // building an fs::path only to hand it straight back here).
    return fileUriFromPath(fs::path{std::string{path}});
}

std::string fileUriFromPath(fs::path const& path) {
    const std::u8string u8 = path.generic_u8string();
    std::string_view bytes(reinterpret_cast<char const*>(u8.data()), u8.size());

    std::string out = "file://";
    // UNC: `//server/share` — the server IS the authority, so the leading
    // separator(s) are consumed by the `file://` prefix already emitted.
    //
    // ⚠ DETECTED FROM `root_name()`, NOT from the generic string, and that is a
    // MEASURED correction rather than a preference. This first tested
    // `bytes[0] == '/' && bytes[1] == '/'` and produced `file:///server/...`
    // for `//server/share/x.c`: on this toolchain `generic_u8string()` renders
    // the UNC root with a SINGLE leading slash, so the double slash the check
    // was looking for is not there to find. `root_name()` is the API that
    // actually models "this path has an authority", and it survives the
    // normalisation.
    const std::u8string rootName = path.root_name().u8string();
    const bool unc = rootName.size() > 1
                  && (rootName[0] == u8'/' || rootName[0] == u8'\\');
    if (unc) {
        // Skip however many leading separators this platform's rendering kept.
        while (!bytes.empty() && (bytes.front() == '/' || bytes.front() == '\\')) {
            bytes.remove_prefix(1);
        }
    } else if (bytes.empty() || bytes.front() != '/') {
        // A drive-letter or relative path: the URI grammar wants an empty
        // authority plus a root slash.
        out += '/';
    }

    auto unreserved = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || c == '-' || c == '.' || c == '_' || c == '~'
            // Structural, and must NOT be encoded: `/` separates segments and
            // `:` follows a Windows drive letter in the form clients send.
            || c == '/' || c == ':';
    };
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (char const ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        if (unreserved(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

WorkspacePreferenceResult resolveWorkspaceLanguagePreference(
    std::span<fs::path const> workspaceRoots) {
    if (workspaceRoots.empty()) {
        return std::unexpected(WorkspaceProjectError{
            WorkspaceProjectErrorKind::NoWorkspaceRoot,
            "the client named no workspace folder (no `workspaceFolders`, "
            "`rootUri` or `rootPath` in `initialize`), so there is no "
            "directory to look for a `" + std::string{kProjectFileSuffix}
                + "` project file in"});
    }

    std::vector<fs::path> manifests;
    for (auto const& root : workspaceRoots) collectManifests(root, manifests);
    std::sort(manifests.begin(), manifests.end());

    if (manifests.empty()) {
        std::string roots;
        for (auto const& r : workspaceRoots) {
            if (!roots.empty()) roots += ", ";
            roots += r.string();
        }
        return std::unexpected(WorkspaceProjectError{
            WorkspaceProjectErrorKind::ProjectFileNotFound,
            "no `" + std::string{kProjectFileSuffix} + "` project file was "
            "found directly inside the workspace root(s) [" + roots
                + "]; without one the editor has no compile target and cannot "
                  "choose between languages that claim the same file extension"});
    }

    WorkspaceLanguagePreference pref;
    pref.projectFiles = manifests;

    for (auto const& manifest : manifests) {
        // ★ THE SHARED PARSER, NOT A SECOND READER. Its closed key vocabulary,
        // its required-field rules and its diagnostics are the compiler's — an
        // editor that accepted a manifest the compiler rejects (or vice versa)
        // would be a worse bug than the one this cycle is fixing.
        DiagnosticReporter rep;
        auto cfg = loadProjectConfig(manifest, rep);
        if (!cfg.has_value()) {
            return std::unexpected(WorkspaceProjectError{
                WorkspaceProjectErrorKind::ProjectFileLoadFailed,
                "project file `" + manifest.string() + "` could not be loaded — "
                    + firstReported(rep)});
        }

        for (auto const& spec : cfg->targets) {
            // ★ THE DRIVER'S OWN SPLITTER. `<targetName>:<formatName>` is the
            // driver's convention and `TargetSpec::parse` is where it lives,
            // including the four remediation-distinct rejection modes.
            auto parsed = TargetSpec::parse(spec);
            if (!parsed.has_value()) {
                return std::unexpected(WorkspaceProjectError{
                    WorkspaceProjectErrorKind::TargetSpecMalformed,
                    "project file `" + manifest.string() + "` declares target `"
                        + spec + "` which is not a `<targetName>:<formatName>` "
                        "spec ("
                        + std::string{targetSpecErrorName(parsed.error())} + ")"});
            }

            auto target = TargetSchema::loadShipped(parsed->targetName);
            if (!target.has_value()) {
                return std::unexpected(WorkspaceProjectError{
                    WorkspaceProjectErrorKind::TargetConfigLoadFailed,
                    "project file `" + manifest.string() + "` declares target `"
                        + parsed->targetName
                        + "` whose `<name>.target.json` could not be loaded — "
                        + firstConfigDiag(target.error())});
            }

            const auto language = (*target)->defaultAssemblyLanguage();
            if (language.empty()) {
                return std::unexpected(WorkspaceProjectError{
                    WorkspaceProjectErrorKind::TargetDeclaresNoAssemblyLanguage,
                    "target `" + parsed->targetName + "` (declared by project "
                        "file `" + manifest.string() + "`) declares no "
                        "`defaultAssemblyLanguage`, so it cannot say which "
                        "source language spells its assembly; add the key to "
                        "its `.target.json`"});
            }

            // Dedup by NAME: two targets that prefer the SAME language are not
            // an ambiguity, and counting them twice would make every
            // multi-format workspace look ambiguous.
            if (std::find(pref.languages.begin(), pref.languages.end(), language)
                == pref.languages.end()) {
                pref.languages.emplace_back(language);
            }
        }
    }

    return pref;
}

std::string describeUnresolvedSchema(
    std::string_view                 fileExtension,
    SchemaResolveError const&        schemaError,
    WorkspacePreferenceResult const& preference) {
    std::string out{"no language service for `"};
    out += fileExtension;
    out += "` — ";
    out += schemaError.detail;

    if (schemaError.kind != SchemaResolveErrorKind::AmbiguousExtension) {
        return out;
    }
    // Only an AMBIGUOUS extension is a question the workspace could have
    // answered. Attaching the workspace reason to `NoExtensionMatch` or
    // `ShippedDir*` would point the operator at the manifest when the real fix
    // is a missing language / an undiscoverable config directory.
    if (preference.has_value()) {
        out += " The workspace project file(s) [";
        out += joinPaths(preference->projectFiles);
        out += "] prefer [";
        out += joinNames(preference->languages);
        out += "].";
    } else {
        out += " The workspace could not name a preferred language [";
        out += workspaceProjectErrorName(preference.error().kind);
        out += "]: ";
        out += preference.error().detail;
        out += '.';
    }
    return out;
}

} // namespace dss::lsp
