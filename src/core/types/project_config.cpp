#include "core/types/project_config.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace dss {

namespace {

using json = nlohmann::json;

// Emit an Error-severity diagnostic into `rep` with a `<sourceLabel>: `
// prefix so the user sees which project file failed.
void emitProjectError(DiagnosticReporter& rep,
                      DiagnosticCode code,
                      std::string_view sourceLabel,
                      std::string detail) {
    report(rep, code, DiagnosticSeverity::Error,
           std::string{sourceLabel} + ": " + std::move(detail));
}

// Read a REQUIRED non-empty string field. Returns false (after
// emitting) on absence / wrong-type / empty.
bool readRequiredString(json const& doc,
                        char const* key,
                        std::string& out,
                        std::string_view label,
                        DiagnosticReporter& rep) {
    if (!doc.contains(key)) {
        emitProjectError(rep, DiagnosticCode::C_MissingField, label,
                         std::string{"missing required field '"} + key + "'");
        return false;
    }
    json const& v = doc.at(key);
    if (!v.is_string()) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         std::string{"field '"} + key + "' must be a string");
        return false;
    }
    out = v.get<std::string>();
    if (out.empty()) {
        emitProjectError(rep, DiagnosticCode::C_MissingField, label,
                         std::string{"field '"} + key + "' must be a non-empty string");
        return false;
    }
    return true;
}

// Read a REQUIRED non-empty array of non-empty strings. Returns false
// (after emitting) on absence / wrong-type / empty array / non-string
// or empty entry.
bool readRequiredStringArray(json const& doc,
                             char const* key,
                             std::vector<std::string>& out,
                             std::string_view label,
                             DiagnosticReporter& rep) {
    if (!doc.contains(key)) {
        emitProjectError(rep, DiagnosticCode::C_MissingField, label,
                         std::string{"missing required field '"} + key + "'");
        return false;
    }
    json const& v = doc.at(key);
    if (!v.is_array()) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         std::string{"field '"} + key + "' must be an array of strings");
        return false;
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
        json const& e = v[i];
        if (!e.is_string()) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             std::string{"field '"} + key + "' entry ["
                             + std::to_string(i) + "] must be a string");
            return false;
        }
        std::string s = e.get<std::string>();
        if (s.empty()) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             std::string{"field '"} + key + "' entry ["
                             + std::to_string(i) + "] must be a non-empty string");
            return false;
        }
        out.push_back(std::move(s));
    }
    if (out.empty()) {
        emitProjectError(rep, DiagnosticCode::C_MissingField, label,
                         std::string{"field '"} + key + "' must contain at least one entry");
        return false;
    }
    return true;
}

// Read an OPTIONAL array of non-empty strings. Mirrors
// `readRequiredStringArray` EXCEPT the required-ness: an ABSENT field
// leaves `out` empty and returns true (no diagnostic), and a
// present-but-EMPTY `[]` is allowed (⇒ empty list, no "must contain at
// least one entry" error). A present value that is NOT an array, or an
// entry that is not a non-empty string, still fails loud C_MalformedJson
// (never a silent drop). Used for the OPTIONAL compile-flag arrays
// (`includes` / `defines` / `resolveLibraries`).
bool readOptionalStringArray(json const& doc,
                             char const* key,
                             std::vector<std::string>& out,
                             std::string_view label,
                             DiagnosticReporter& rep) {
    out.clear();  // defensive: never inherit a caller's stale contents
    if (!doc.contains(key)) {
        return true;  // absent ⇒ empty (the caller left `out` empty); no error
    }
    json const& v = doc.at(key);
    if (!v.is_array()) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         std::string{"field '"} + key + "' must be an array of strings");
        return false;
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
        json const& e = v[i];
        if (!e.is_string()) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             std::string{"field '"} + key + "' entry ["
                             + std::to_string(i) + "] must be a string");
            return false;
        }
        std::string s = e.get<std::string>();
        if (s.empty()) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             std::string{"field '"} + key + "' entry ["
                             + std::to_string(i) + "] must be a non-empty string");
            return false;
        }
        out.push_back(std::move(s));
    }
    // A present-but-empty `[]` is ALLOWED (the one difference from
    // readRequiredStringArray) — no "at least one entry" check here.
    return true;
}

// D-FFI-DECLARED-IMPORT-NAME — read the OPTIONAL `resolveLibraries` array.
//
// Mirrors `readOptionalStringArray` (absent ⇒ empty + no error; a
// present-but-empty `[]` is allowed; a non-array fails loud) but each ENTRY
// may take EITHER of two shapes:
//
//   "dist/libfoo.so"                                  the PLAIN form
//   {"path": "…/libtcl8.6.dylib",                     the EXTENDED form
//    "importName": "@rpath/libtcl8.6.dylib"}
//
// The plain form is byte-for-byte what every shipped manifest already writes
// and produces an EMPTY `declaredImportName` (nothing stated ⇒ the binary's
// own embedded soname wins downstream). The extended form additionally STATES
// the runtime identity to record, which outranks that soname.
//
// Every degenerate shape fails loud `C_MalformedJson`, never a silent drop:
// a non-string / non-object entry, an empty plain string, a missing or
// non-string or empty `path` / `importName`, and any UNKNOWN key inside the
// object (matching this loader's top-level unknown-key rejection — a typo'd
// `"importname"` must not silently discard the identity).
bool readOptionalResolveLibraries(json const& doc,
                                  char const* key,
                                  std::vector<ResolveLibrarySpec>& out,
                                  std::string_view label,
                                  DiagnosticReporter& rep) {
    out.clear();  // defensive: never inherit a caller's stale contents
    if (!doc.contains(key)) {
        return true;  // absent ⇒ empty (the caller left `out` empty); no error
    }
    json const& v = doc.at(key);
    if (!v.is_array()) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         std::string{"field '"} + key
                         + "' must be an array of strings or "
                           "{\"path\", \"importName\"} objects");
        return false;
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
        json const& e = v[i];
        std::string const at = std::string{"field '"} + key + "' entry ["
                             + std::to_string(i) + "] ";

        // Read one REQUIRED non-empty string member of the entry object.
        auto member = [&](char const* name, std::string& dst) -> bool {
            if (!e.contains(name)) {
                emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                                 at + "is missing required member '" + name
                                 + "'");
                return false;
            }
            json const& mv = e.at(name);
            if (!mv.is_string()) {
                emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                                 at + "member '" + name + "' must be a string");
                return false;
            }
            dst = mv.get<std::string>();
            if (dst.empty()) {
                emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                                 at + "member '" + name
                                 + "' must be a non-empty string");
                return false;
            }
            return true;
        };

        if (e.is_string()) {
            std::string s = e.get<std::string>();
            if (s.empty()) {
                emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                                 at + "must be a non-empty string");
                return false;
            }
            out.push_back(ResolveLibrarySpec{std::move(s), {}});
            continue;
        }
        if (!e.is_object()) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             at + "must be a non-empty string (the library "
                                  "path) or an object {\"path\": …, "
                                  "\"importName\": …}");
            return false;
        }

        // Unknown keys inside the entry object reject, same rule + same
        // rationale as the top-level unknown-key gate: a mistyped member is a
        // silent drop of the identity the entry exists to state.
        static constexpr std::string_view kEntryKeys[] = {"path", "importName"};
        for (auto it = e.begin(); it != e.end(); ++it) {
            std::string const& k = it.key();
            bool known = false;
            for (auto const& known_k : kEntryKeys) {
                if (known_k == k) { known = true; break; }
            }
            if (!known) {
                emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                                 at + "has unknown member '" + k
                                 + "' (recognized members: path, importName)");
                return false;
            }
        }

        // `importName` is REQUIRED here: the object form exists SOLELY to
        // state an identity, so an object without one is a typo or noise and
        // the plain string form says the same thing better. One spelling per
        // meaning, and the degenerate variant rejects rather than aliasing.
        ResolveLibrarySpec spec;
        std::string path;
        if (!member("path", path)) return false;
        if (!member("importName", spec.declaredImportName)) return false;
        spec.path = std::move(path);
        out.push_back(std::move(spec));
    }
    // A present-but-empty `[]` is ALLOWED — no "at least one entry" check.
    return true;
}

} // namespace

std::optional<ProjectConfig>
parseProjectConfig(std::string_view jsonText,
                   std::string_view sourceLabel,
                   DiagnosticReporter& rep) {
    json doc;
    try {
        doc = json::parse(jsonText);
    } catch (json::parse_error const& e) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, sourceLabel,
                         std::string{"invalid JSON — "} + e.what());
        return std::nullopt;
    }
    if (!doc.is_object()) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, sourceLabel,
                         "project config root must be a JSON object");
        return std::nullopt;
    }

    // Reject UNKNOWN top-level keys — a typo on a field name (e.g.
    // "ouput", "languag") would otherwise be a silent drop. The grammar
    // / target / format loaders reject unknown keys for exactly this
    // reason; the project-config loader holds the same fail-loud line.
    // Checked BEFORE the field reads so a typo'd REQUIRED field surfaces
    // as "unknown field 'languag'" (points at the typo) rather than the
    // less-actionable "missing required field 'language'".
    static constexpr std::string_view kKnownKeys[] = {
        "language", "artifactProfile", "targets", "sources", "output",
        "artifactName", "includes", "defines", "resolveLibraries",
        "stackReserve",
    };
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        std::string const& key = it.key();
        bool known = false;
        for (auto const& k : kKnownKeys) {
            if (k == key) { known = true; break; }
        }
        if (!known) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, sourceLabel,
                             "unknown field '" + key + "' (recognized fields: "
                             "language, artifactProfile, targets, sources, output, "
                             "artifactName, includes, defines, resolveLibraries, "
                             "stackReserve)");
            return std::nullopt;
        }
    }

    ProjectConfig pc;
    if (!readRequiredString(doc, "language", pc.language, sourceLabel, rep))
        return std::nullopt;
    if (!readRequiredString(doc, "artifactProfile", pc.artifactProfile, sourceLabel, rep))
        return std::nullopt;
    if (!readRequiredStringArray(doc, "targets", pc.targets, sourceLabel, rep))
        return std::nullopt;
    if (!readRequiredStringArray(doc, "sources", pc.sources, sourceLabel, rep))
        return std::nullopt;

    // The OPTIONAL compile-flag arrays (the file-driven counterparts of the
    // CLI `-I` / `--define` / `--resolve-library`). Absent ⇒ empty (no error);
    // present must be an array of non-empty strings (else C_MalformedJson) —
    // `resolveLibraries` additionally accepts the extended
    // `{"path", "importName"}` object entry (D-FFI-DECLARED-IMPORT-NAME); a
    // present-but-empty `[]` is allowed. `Program::compileProject` threads
    // these (merge/append) onto the Program's current state.
    if (!readOptionalStringArray(doc, "includes", pc.includes, sourceLabel, rep))
        return std::nullopt;
    if (!readOptionalStringArray(doc, "defines", pc.defines, sourceLabel, rep))
        return std::nullopt;
    // D-FFI-DECLARED-IMPORT-NAME: `resolveLibraries` entries are a plain path
    // STRING *or* an extended `{"path", "importName"}` object — its own reader.
    if (!readOptionalResolveLibraries(doc, "resolveLibraries",
                                      pc.resolveLibraries, sourceLabel, rep))
        return std::nullopt;

    // `output` is an OPTIONAL user-authored hint: validate its type when
    // present (fail loud on a malformed value — never a silent no-op),
    // but its path ROUTING is deferred (D-AP2-OUTPUT-ROUTING). Absent ⇒
    // the existing per-target output convention applies downstream.
    if (doc.contains("output")) {
        json const& v = doc.at("output");
        if (!v.is_string()) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, sourceLabel,
                             "field 'output' must be a string");
            return std::nullopt;
        }
        std::string o = v.get<std::string>();
        if (o.empty()) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, sourceLabel,
                             "field 'output' must be a non-empty string when present");
            return std::nullopt;
        }
        pc.output = std::move(o);
    }

    // `artifactName` is an OPTIONAL base NAME for the emitted binary (the
    // per-platform-subdir routing in Program::compileProject reads it). Validate
    // when present, fail loud on a malformed value (never a silent no-op):
    //   * wrong type / empty string            → C_MalformedJson
    //   * contains a path separator ('/' or '\') → C_MalformedJson — it is a
    //     bare NAME, not a path; the DIRECTORY comes from `--output` (+ the
    //     per-format subdir). This is an EARLY, clear parse-time guard for the
    //     common `"dist/app"` mistake.
    // NOTE: this separator check is NOT the containment boundary — a denylist of
    // two chars cannot prove a name stays inside the output dir (a bare ".." has
    // no separator, and a Windows drive-relative "D:app" has none either, yet
    // both escape once joined onto the output dir). The REAL boundary is a
    // lexically-normalized parent-path check at the ROUTING site
    // (compileOneTarget → D_ArtifactNameEscapesOutputDir), where the output dir
    // is known. This loader check just fails the common case earlier + clearer.
    // Absent ⇒ nullopt ⇒ the source stem names the artifact (unchanged).
    if (doc.contains("artifactName")) {
        json const& v = doc.at("artifactName");
        if (!v.is_string()) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, sourceLabel,
                             "field 'artifactName' must be a string");
            return std::nullopt;
        }
        std::string a = v.get<std::string>();
        if (a.empty()) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, sourceLabel,
                             "field 'artifactName' must be a non-empty string when present");
            return std::nullopt;
        }
        if (a.find('/') != std::string::npos || a.find('\\') != std::string::npos) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, sourceLabel,
                             "field 'artifactName' must be a bare file name (no path separators)");
            return std::nullopt;
        }
        pc.artifactName = std::move(a);
    }

    // `stackReserve` (D-SQLITE-PE64-FULL-TIER-STACK-DEPTH) is an OPTIONAL
    // per-PROGRAM stack-reserve request, in BYTES — the file-driven twin of
    // the CLI `--stack-reserve`. It belongs in the MANIFEST because the
    // number is a property of THIS program's deepest call chain (the
    // motivating case: 1000 levels of nested SQL trigger recursion overflow
    // the Windows 1 MiB default), so it travels with the project rather than
    // with an invocation.
    //
    // Validated for SHAPE only here — a positive integer byte count.
    // `is_number_unsigned()` rejects a negative and a float in one check
    // (nlohmann types `-1` as a SIGNED integer and `4.5` as a float), so a
    // `-1` can never wrap into a huge u64. RANGE and ALIGNMENT are NOT
    // decided here: those bounds are declared by the chosen OBJECT FORMAT
    // (`stackReserveControl` in its `.format.json`) and enforced at the
    // linker gate, which also REFUSES a request outright on a format that
    // declares no such capability. Absent ⇒ nullopt ⇒ the format's declared
    // default stands (unchanged behavior).
    if (doc.contains("stackReserve")) {
        json const& v = doc.at("stackReserve");
        if (!v.is_number_unsigned()) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, sourceLabel,
                             "field 'stackReserve' must be a non-negative "
                             "integer byte count (e.g. 4194304 for 4 MiB)");
            return std::nullopt;
        }
        std::uint64_t const n = v.get<std::uint64_t>();
        if (n == 0) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, sourceLabel,
                             "field 'stackReserve' must be greater than zero "
                             "when present — a zero-byte stack reserve cannot "
                             "start a program. Omit the field to take the "
                             "object format's declared default.");
            return std::nullopt;
        }
        pc.stackReserveBytes = n;
    }

    return pc;
}

std::optional<ProjectConfig>
loadProjectConfig(std::filesystem::path const& path,
                  DiagnosticReporter& rep) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        emitProjectError(rep, DiagnosticCode::D_FileNotFound, path.string(),
                         "project config file could not be opened");
        return std::nullopt;
    }
    std::string text{std::istreambuf_iterator<char>{in},
                     std::istreambuf_iterator<char>{}};
    // The open() check above only proves the file opened; a hard I/O
    // error mid-read (disk error, removable/network FS vanishing) would
    // otherwise hand a SILENTLY-TRUNCATED `text` to the parser. `bad()`
    // is the hard-error bit (eofbit is normal at end-of-stream), so this
    // fails loud only on a real read failure.
    if (in.bad()) {
        emitProjectError(rep, DiagnosticCode::D_FileNotFound, path.string(),
                         "I/O error while reading project config file");
        return std::nullopt;
    }
    return parseProjectConfig(text, path.string(), rep);
}

bool artifactProfileSupported(std::span<std::string const> declared,
                              std::string_view profile) noexcept {
    for (auto const& p : declared) {
        if (p == profile) return true;
    }
    // Empty set ⇒ false (fail-closed). A language must declare ≥1
    // profile to be project-buildable (plan 06 §2.1 trajectory).
    return false;
}

bool enforceArtifactProfile(std::span<std::string const> declared,
                            std::string_view profile,
                            std::string_view language,
                            DiagnosticReporter& rep) {
    if (artifactProfileSupported(declared, profile)) return true;

    std::string msg;
    if (declared.empty()) {
        // Empty-set sub-case — discriminated in the MESSAGE only; the
        // SUPPORT decision is the single predicate above (no separate
        // policy branch). Do not enumerate the registered vocabulary
        // here — that list is owned by the grammar loader
        // (kRegisteredArtifactProfiles); re-listing it would risk drift.
        msg = "language '" + std::string{language}
            + "' declares no artifact profiles — it cannot be built via a "
              "project config until its .lang.json declares one or more "
              "'artifactProfiles[]' entries (requested profile: '"
            + std::string{profile} + "').";
    } else {
        std::string list;
        for (auto const& p : declared) {
            if (!list.empty()) list += ", ";
            list += p;
        }
        msg = "artifact profile '" + std::string{profile}
            + "' is not supported by language '" + std::string{language}
            + "' (supported: " + list + ").";
    }
    report(rep, DiagnosticCode::D_ArtifactProfileNotSupported,
           DiagnosticSeverity::Error, std::move(msg));
    return false;
}

bool enforceArtifactProfileFormat(std::span<std::string const> served,
                                  std::string_view profile,
                                  std::string_view formatName,
                                  DiagnosticReporter& rep) {
    // Same generic membership predicate as the language gate — only the
    // diagnostic code + message differ (remediation-distinct: fix the
    // .lang.json [language gate] vs pick a different target/format or ship
    // the backend [this format gate]).
    if (artifactProfileSupported(served, profile)) return true;

    std::string msg;
    if (served.empty()) {
        // Empty served-set sub-case (message-only discrimination; the
        // decision is the single predicate above). A relocatable/object
        // format, or a format whose backend isn't shipped, serves nothing.
        msg = "object format '" + std::string{formatName}
            + "' serves no artifact profiles — it cannot produce profile '"
            + std::string{profile} + "'. Choose a target whose object format "
              "produces this profile (or ship the backend that emits it).";
    } else {
        std::string list;
        for (auto const& p : served) {
            if (!list.empty()) list += ", ";
            list += p;
        }
        msg = "artifact profile '" + std::string{profile}
            + "' is not served by object format '" + std::string{formatName}
            + "' (serves: " + list + ").";
    }
    report(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch,
           DiagnosticSeverity::Error, std::move(msg));
    return false;
}

} // namespace dss
