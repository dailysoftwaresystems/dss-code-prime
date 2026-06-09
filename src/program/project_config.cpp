#include "program/project_config.hpp"

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
                             "language, artifactProfile, targets, sources, output)");
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

} // namespace dss
