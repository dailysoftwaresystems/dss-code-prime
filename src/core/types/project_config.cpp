#include "core/types/project_config.hpp"

#include "core/types/config_document_parse.hpp"  // THE ONE config-document parse
#include "core/types/config_key_vocabulary.hpp"  // isDocumentationKey + DSS_CHECK_KEY_VOCABULARY
#include "core/types/parse_diagnostic.hpp"
#include "program/platform_token.hpp"            // kRunOnPlatformTokens / isValidRunOnToken / runOnTokenList

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace dss {

namespace {

using json = nlohmann::json;

// ── the CLOSED top-level key vocabulary ─────────────────────────────────────
//
// The ONE list. It is a `std::array<std::string_view, N>` (not a C array) so
// it can carry `DSS_CHECK_KEY_VOCABULARY`, the shared compile-time guard from
// `core/types/config_key_vocabulary.hpp`: a hand-written N that exceeds the
// initializer count zero-fills the tail, and the check would then WHITELIST a
// key of `""`; a duplicate entry inflates N so a genuinely missing key looks
// accounted for. Both are silent without the static_assert.
//
// The human-readable "recognized fields" list in the diagnostic is DERIVED
// from this array by `projectConfigKnownKeyList()` — see the header note. Do
// not re-type it anywhere.
constexpr std::array<std::string_view, 14> kKnownKeys = {
    "language", "artifactProfile", "targets", "sources", "output",
    "artifactName", "includes", "defines", "resolveLibraries",
    "stackReserve", "preBuildScripts", "postBuildScripts", "dependsOn",
    "dependencyArtifactCache",
};
DSS_CHECK_KEY_VOCABULARY(kKnownKeys);

// ── the CLOSED `dependencyArtifactCache.eviction` vocabulary ────────────────
//
// The token table and the enumerator table are index-parallel and asserted so
// here, rather than being one `switch` a future enumerator can silently fall
// out of. `parseDependencyArtifactCacheEviction` and the reject message both
// read THIS table, so a token that is accepted is a token the message lists.
constexpr std::array<std::string_view, 2> kEvictionTokens = {
    "prune-superseded", "retain",
};
DSS_CHECK_KEY_VOCABULARY(kEvictionTokens);

constexpr std::array<DependencyArtifactCacheEviction, kEvictionTokens.size()>
    kEvictionValues = {
        DependencyArtifactCacheEviction::PruneSuperseded,
        DependencyArtifactCacheEviction::Retain,
};

// Comma-join a closed-key table for a diagnostic. ONE joiner for every table
// in this file, so no message can list a key set by hand.
template <std::size_t N>
std::string joinKeys(std::array<std::string_view, N> const& keys) {
    std::string out;
    for (auto const& k : keys) {
        if (!out.empty()) out += ", ";
        out += k;
    }
    return out;
}

// The same joiner for a RUNTIME list (a language's declared profiles, a
// format's served set, the shipped formats that serve a profile). One
// separator convention for every list this file puts in front of a user.
std::string joinStrings(std::span<std::string const> items) {
    std::string out;
    for (auto const& s : items) {
        if (!out.empty()) out += ", ";
        out += s;
    }
    return out;
}


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
        // silent drop of the identity the entry exists to state. A
        // `$`-prefixed member is PROSE and is skipped, and the recognized-list
        // in the message is DERIVED from the table — both for the same reasons
        // as the top-level check (see `kKnownKeys`); a nested key set that
        // rejected `$comment` would just reinstate
        // D-AP2-PROJECT-MANIFEST-REJECTS-COMMENT-KEY one level down.
        static constexpr std::array<std::string_view, 2> kEntryKeys = {
            "path", "importName"};
        DSS_CHECK_KEY_VOCABULARY(kEntryKeys);
        if (auto bad = detail::firstUnknownKey(e, kEntryKeys)) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             at + "has unknown member '" + *bad
                             + "' (recognized members: " + joinKeys(kEntryKeys)
                             + ")");
            return false;
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

// ── OPTIONAL `preBuildScripts` / `postBuildScripts` ─────────────────────────
//
// Read an OPTIONAL array of `{"run", "runOn"}` objects. Absent ⇒ empty + no
// error; a present-but-empty `[]` ⇒ empty + no error (declaring no hooks is
// not a mistake). Everything else fails loud `C_MalformedJson`.
//
// `run` is an ARGV VECTOR (`run[0]` is spawned directly), which is why it is
// validated as an ARRAY of non-empty strings and never as a command STRING —
// see the `ScriptEntry` note in the header for why a string would be both a
// portability hole and an injection surface.
//
// `runOn` is OPTIONAL; when present it must be a NON-EMPTY array of tokens
// from `kRunOnPlatformTokens`. An unrecognized token is the whole point of
// this check: `"win32"`, `"macos"`, `"osx"` are the natural guesses, and a
// loader that accepted them would produce a filter matching NO host — a
// pre-build step that silently never runs, and a build that silently skips
// generating its own inputs. So the message names BOTH the offending token
// and the full recognized vocabulary (`runOnTokenList()`, derived from the
// same array the predicate tests).
//
// `key` is the manifest field name (`preBuildScripts` / `postBuildScripts`),
// quoted verbatim in every message so the user can see WHICH of the two script
// arrays failed — one reader, two fields, no duplicated validation.
bool readOptionalScripts(json const& doc,
                         char const* key,
                         std::vector<ScriptEntry>& out,
                         std::string_view label,
                         DiagnosticReporter& rep) {
    out.clear();  // defensive: never inherit a caller's stale contents
    if (!doc.contains(key)) {
        return true;  // absent ⇒ empty; NEVER an error
    }
    json const& v = doc.at(key);
    if (!v.is_array()) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         std::string{"field '"} + key
                         + "' must be an array of {\"run\", \"runOn\"} objects");
        return false;
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
        json const& e = v[i];
        std::string const at = std::string{"field '"} + key + "' entry ["
                             + std::to_string(i) + "] ";
        if (!e.is_object()) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             at + "must be an object {\"run\": [...], "
                                  "\"runOn\": [...]}");
            return false;
        }

        static constexpr std::array<std::string_view, 2> kScriptKeys = {
            "run", "runOn"};
        DSS_CHECK_KEY_VOCABULARY(kScriptKeys);
        if (auto bad = detail::firstUnknownKey(e, kScriptKeys)) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             at + "has unknown member '" + *bad
                             + "' (recognized members: " + joinKeys(kScriptKeys)
                             + ")");
            return false;
        }

        // Read one member as an array of non-empty strings. `what` completes
        // the "must be an array of …" phrasing so each member explains ITSELF.
        auto stringArrayMember = [&](char const* name, std::string const& what,
                                     std::vector<std::string>& dst) -> bool {
            json const& mv = e.at(name);
            if (!mv.is_array()) {
                emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                                 at + "member '" + name
                                 + "' must be an array of " + what);
                return false;
            }
            for (std::size_t j = 0; j < mv.size(); ++j) {
                json const& s = mv[j];
                if (!s.is_string()) {
                    emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                                     at + "member '" + name + "' element ["
                                     + std::to_string(j) + "] must be a string");
                    return false;
                }
                std::string word = s.get<std::string>();
                if (word.empty()) {
                    emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                                     at + "member '" + name + "' element ["
                                     + std::to_string(j)
                                     + "] must be a non-empty string");
                    return false;
                }
                // AN EMBEDDED NUL PASSES EVERY CHECK ABOVE AND THEN CHANGES
                // WHAT RUNS. JSON permits an escaped U+0000 and nlohmann stores
                // the byte in the `std::string`, so `is_string()` is true and
                // `empty()` is false: a `run` word carrying one was a VALID
                // manifest here. The OS then reads an argv word as a
                // NUL-TERMINATED C string: POSIX `execv` hands the child the
                // word truncated at the NUL, and on Windows the NUL ends
                // `lpCommandLine` outright, so every LATER argument disappears.
                // The hook runs, plausibly exits 0, and the build reports
                // success having executed something other than what the
                // manifest says — the silent wrong-work class the rest of this
                // reader exists to reject, with the additional twist that the
                // offending byte is invisible in the author's editor.
                //
                // The word is NOT echoed. It is by definition unprintable, and
                // a message carrying a raw NUL is a message that truncates in
                // whatever reads it next; the two INDICES are what locates it
                // in the file. (`runOn` goes through this helper too and is
                // covered for free — a token containing a NUL matches no entry
                // in the closed vocabulary, and rejecting it here keeps the
                // unknown-token message below from quoting the byte.)
                if (word.find('\0') != std::string::npos) {
                    emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                                     at + "member '" + name + "' element ["
                                     + std::to_string(j)
                                     + "] contains an embedded NUL byte (not "
                                       "echoed — a NUL is unprintable). A "
                                       "manifest word is read as a C string the "
                                       "moment it is used: an argv word is "
                                       "handed to the OS NUL-terminated, so "
                                       "this word would reach the program "
                                       "truncated on POSIX and would end the "
                                       "whole command line on Windows, silently "
                                       "dropping every argument after it.");
                    return false;
                }
                dst.push_back(std::move(word));
            }
            return true;
        };

        ScriptEntry entry;

        // `run` is REQUIRED: an entry with no command is not a hook.
        if (!e.contains("run")) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             at + "is missing required member 'run'");
            return false;
        }
        if (!stringArrayMember(
                "run",
                "non-empty strings (an argv vector: run[0] is the executable, "
                "spawned directly — never a shell command line)",
                entry.run))
            return false;
        if (entry.run.empty()) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             at + "member 'run' must contain at least one "
                                  "entry (run[0] is the executable to spawn)");
            return false;
        }

        // `runOn` is OPTIONAL — absent ⇒ every platform. Present ⇒ non-empty
        // and every token recognized.
        if (e.contains("runOn")) {
            if (!stringArrayMember("runOn",
                                   "platform tokens (recognized: "
                                   + runOnTokenList() + ")",
                                   entry.runOn))
                return false;
            if (entry.runOn.empty()) {
                emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                                 at + "member 'runOn' must contain at least "
                                      "one platform token when present — omit "
                                      "the member to run on every platform");
                return false;
            }
            for (std::size_t j = 0; j < entry.runOn.size(); ++j) {
                if (!isValidRunOnToken(entry.runOn[j])) {
                    emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                                     at + "member 'runOn' element ["
                                     + std::to_string(j)
                                     + "] has unknown platform token '"
                                     + entry.runOn[j] + "' (recognized: "
                                     + runOnTokenList() + ")");
                    return false;
                }
            }
        }

        out.push_back(std::move(entry));
    }
    return true;
}

// ── OPTIONAL `dependsOn` ────────────────────────────────────────────────────
//
// Read an OPTIONAL array of `{"path"} | {"git", "ref"?}` objects. Absent ⇒
// empty + no error; a present-but-empty `[]` ⇒ empty + no error. Everything
// else fails loud `C_MalformedJson` — see the `DependencyEntry` note in the
// header for why BOTH sources, NEITHER source, and a `ref` with no `git` are
// each a reject rather than a tolerated shape.
//
// Values are kept VERBATIM (no path resolution, no URL parsing, no filesystem)
// exactly as `sources[]` is — a different lane resolves them.
bool readOptionalDependsOn(json const& doc,
                           char const* key,
                           std::vector<DependencyEntry>& out,
                           std::string_view label,
                           DiagnosticReporter& rep) {
    out.clear();  // defensive: never inherit a caller's stale contents
    if (!doc.contains(key)) {
        return true;  // absent ⇒ empty; NEVER an error
    }
    json const& v = doc.at(key);
    if (!v.is_array()) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         std::string{"field '"} + key
                         + "' must be an array of {\"path\"} or "
                           "{\"git\", \"ref\"} objects");
        return false;
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
        json const& e = v[i];
        std::string const at = std::string{"field '"} + key + "' entry ["
                             + std::to_string(i) + "] ";
        if (!e.is_object()) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             at + "must be an object {\"path\": …} or "
                                  "{\"git\": …, \"ref\": …}");
            return false;
        }

        static constexpr std::array<std::string_view, 3> kDependencyKeys = {
            "path", "git", "ref"};
        DSS_CHECK_KEY_VOCABULARY(kDependencyKeys);
        if (auto bad = detail::firstUnknownKey(e, kDependencyKeys)) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             at + "has unknown member '" + *bad
                             + "' (recognized members: "
                             + joinKeys(kDependencyKeys) + ")");
            return false;
        }

        // Read one OPTIONAL non-empty string member into an optional.
        auto optionalMember = [&](char const* name,
                                  std::optional<std::string>& dst) -> bool {
            if (!e.contains(name)) return true;   // absent ⇒ nullopt
            json const& mv = e.at(name);
            if (!mv.is_string()) {
                emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                                 at + "member '" + name + "' must be a string");
                return false;
            }
            std::string s = mv.get<std::string>();
            if (s.empty()) {
                emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                                 at + "member '" + name
                                 + "' must be a non-empty string");
                return false;
            }
            dst = std::move(s);
            return true;
        };

        DependencyEntry dep;
        if (!optionalMember("path", dep.path)) return false;
        if (!optionalMember("git", dep.git))   return false;
        if (!optionalMember("ref", dep.ref))   return false;

        // EXACTLY ONE source. Both ⇒ ambiguous, and silently preferring one
        // would make the other a no-op the user cannot see. Neither ⇒ the
        // entry names nothing at all.
        if (dep.path && dep.git) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             at + "states both 'path' and 'git' — a dependency "
                                  "has exactly one source; use 'path' for a "
                                  "local checkout or 'git' for a remote one");
            return false;
        }
        if (!dep.path && !dep.git) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             at + "states neither 'path' nor 'git' — a "
                                  "dependency needs exactly one source");
            return false;
        }
        // A `ref` pins a revision INSIDE a git remote. On a local `path` it
        // has no meaning, so accepting it would silently ignore the pin the
        // user wrote — the precise silent-drop class this loader rejects.
        if (dep.ref && !dep.git) {
            emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                             at + "states 'ref' without 'git' — a ref names a "
                                  "revision in a git remote and has no meaning "
                                  "for a local 'path' dependency");
            return false;
        }

        out.push_back(std::move(dep));
    }
    return true;
}

// ── OPTIONAL `dependencyArtifactCache` ──────────────────────────────────────
//
// D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C). Absent ⇒
// `nullopt` + no error (every manifest that predates this field must build
// byte-identically). PRESENT ⇒ an object with ALL THREE members; everything
// else fails loud `C_MalformedJson`.
//
// ⚠ NO MEMBER IS OPTIONAL, and this is the `resolveLibraries` object-form rule
// rather than strictness for its own sake. A partial policy is a policy nobody
// declared: `{"enabled": true}` alone would leave the location override and the
// eviction rule to a compiled-in default, which is exactly the "100% config
// driven" property this member exists to establish. And the fully degenerate
// spelling — `{"enabled": false}` with nothing else — says what DELETING the
// key says, so it rejects rather than aliasing.
//
// ⚠ THE ENVIRONMENT IS NOT READ HERE. `rootOverrideVariable` is validated as a
// NON-EMPTY NAME and kept verbatim, exactly as `sources[]` and a dependency
// `path` are: resolving it needs the environment, and this is a pure parser.
bool readOptionalDependencyArtifactCache(
    json const& doc, char const* key,
    std::optional<DependencyArtifactCacheConfig>& out,
    std::string_view label, DiagnosticReporter& rep) {
    out.reset();  // defensive: never inherit a caller's stale contents
    if (!doc.contains(key)) return true;  // absent ⇒ nullopt; no error

    json const& v = doc.at(key);
    std::string const at = std::string{"field '"} + key + "' ";
    if (!v.is_object()) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         at + "must be an object {\"enabled\": …, "
                              "\"rootOverrideVariable\": …, \"eviction\": …}");
        return false;
    }

    // Unknown members reject, same rule + same rationale as the top-level gate
    // and the `resolveLibraries` entry gate: a mistyped member is a silent drop
    // of the very policy the object exists to state. `$`-prefixed members are
    // PROSE and are skipped by `firstUnknownKey`, so a nested `$comment` cannot
    // reinstate D-AP2-PROJECT-MANIFEST-REJECTS-COMMENT-KEY one level down.
    static constexpr std::array<std::string_view, 3> kMemberKeys = {
        "enabled", "rootOverrideVariable", "eviction"};
    DSS_CHECK_KEY_VOCABULARY(kMemberKeys);
    if (auto bad = detail::firstUnknownKey(v, kMemberKeys)) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         at + "has unknown member '" + *bad
                         + "' (recognized members: " + joinKeys(kMemberKeys)
                         + ")");
        return false;
    }

    DependencyArtifactCacheConfig cfg;

    if (!v.contains("enabled")) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         at + "is missing required member 'enabled'");
        return false;
    }
    if (!v.at("enabled").is_boolean()) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         at + "member 'enabled' must be a boolean");
        return false;
    }
    cfg.enabled = v.at("enabled").get<bool>();

    if (!v.contains("rootOverrideVariable")) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         at + "is missing required member "
                              "'rootOverrideVariable' — the cache location "
                              "override is NAMED in configuration rather than "
                              "sniffed from a compiled-in variable, so there is "
                              "no default to fall back to");
        return false;
    }
    if (!v.at("rootOverrideVariable").is_string()) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         at + "member 'rootOverrideVariable' must be a string "
                              "naming an environment variable");
        return false;
    }
    cfg.rootOverrideVariable =
        v.at("rootOverrideVariable").get<std::string>();
    if (cfg.rootOverrideVariable.empty()) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         at + "member 'rootOverrideVariable' must be a "
                              "non-empty environment variable NAME");
        return false;
    }

    if (!v.contains("eviction")) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         at + "is missing required member 'eviction' "
                              "(one of: "
                         + dependencyArtifactCacheEvictionTokenList() + ")");
        return false;
    }
    if (!v.at("eviction").is_string()) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         at + "member 'eviction' must be a string (one of: "
                         + dependencyArtifactCacheEvictionTokenList() + ")");
        return false;
    }
    std::string const eviction = v.at("eviction").get<std::string>();
    bool matched = false;
    for (std::size_t i = 0; i < kEvictionTokens.size(); ++i) {
        if (kEvictionTokens[i] != eviction) continue;
        cfg.eviction = kEvictionValues[i];
        matched      = true;
        break;
    }
    if (!matched) {
        // The message names the offending token AND the accepted set, derived
        // from the one table — the same shape (and the same reason) as the
        // `runOn` platform-token reject: a policy word the loader silently
        // reinterpreted would be a cache behaving under a rule nobody wrote.
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, label,
                         at + "member 'eviction' has unrecognized value '"
                         + eviction + "' (accepted: "
                         + dependencyArtifactCacheEvictionTokenList() + ")");
        return false;
    }

    out = std::move(cfg);
    return true;
}

} // namespace

std::span<std::string_view const>
dependencyArtifactCacheEvictionTokens() noexcept {
    return std::span<std::string_view const>{kEvictionTokens};
}

std::string dependencyArtifactCacheEvictionTokenList() {
    return joinKeys(kEvictionTokens);
}

std::span<std::string_view const> projectConfigKnownKeys() noexcept {
    return std::span<std::string_view const>{kKnownKeys};
}

std::string projectConfigKnownKeyList() { return joinKeys(kKnownKeys); }

std::optional<ProjectConfig>
parseProjectConfig(std::string_view jsonText,
                   std::string_view sourceLabel,
                   DiagnosticReporter& rep) {
    // THE ONE CONFIG PARSE (`core/types/config_document_parse.hpp`) —
    // D-CONFIG-A-DUPLICATE-JSON-KEY-IS-DROPPED-WITHOUT-A-DIAGNOSTIC. The
    // manifest is the document a USER hand-edits most often, so it is the one
    // most likely to grow a second `language` or `target` on a copy-paste — and
    // the loser is the declaration nearer the top of the file, the one the user
    // is looking at.
    auto parsed = detail::parseConfigDocument(jsonText);
    if (!parsed) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, sourceLabel,
                         parsed.error().detailTextWithLocus("invalid JSON — "));
        return std::nullopt;
    }
    json doc = std::move(*parsed);
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
    //
    // TWO defects closed here, in the check that was about to grow by three
    // keys:
    //
    // (1) DRIFT. The recognized-field list was written TWICE — the table and a
    //     hand-typed string literal in the message — with nothing asserting
    //     they agreed. The message is now DERIVED from the table
    //     (`projectConfigKnownKeyList()`, the `registeredArtifactProfileList()`
    //     pattern), and the table itself is a `std::array` under
    //     `DSS_CHECK_KEY_VOCABULARY`, so an under-filled or duplicated table is
    //     a COMPILE error rather than a whitelisted `""` key.
    //
    // (2) `$comment`. ✅ CLOSES D-AP2-PROJECT-MANIFEST-REJECTS-COMMENT-KEY.
    //     ✔MEASURED before this change: a `--project` manifest carrying
    //     `$comment` was REJECTED (`unknown field '$comment' (recognized
    //     fields: …)`) while shipped FFI descriptors ACCEPT it and the
    //     convention is documented for them — the same `$`-prefixed convention
    //     blessed in one schema and required-absent in the sibling, with
    //     nothing stating the difference, so a project manifest could not
    //     carry its own provenance. `firstUnknownKey` now skips every
    //     `$`-prefixed key via `dss::detail::isDocumentationKey`, the SAME
    //     shared predicate the grammar / target / format loaders use — not a
    //     one-off `$comment` special case, because a `$comment`-only carve-out
    //     would still reject `$sourcesComment` and re-open the inconsistency
    //     for the next annotation someone writes. The closed set is NOT
    //     relaxed: a non-`$` typo still fails loud, exactly as before.
    if (auto bad = detail::firstUnknownKey(doc, kKnownKeys)) {
        emitProjectError(rep, DiagnosticCode::C_MalformedJson, sourceLabel,
                         "unknown field '" + *bad + "' (recognized fields: "
                         + projectConfigKnownKeyList() + ")");
        return std::nullopt;
    }

    ProjectConfig pc;
    if (!readRequiredString(doc, "language", pc.language, sourceLabel, rep))
        return std::nullopt;
    if (!readRequiredString(doc, "artifactProfile", pc.artifactProfile, sourceLabel, rep))
        return std::nullopt;

    // `targets[]` is REQUIRED of every profile, `module` included — see the
    // re-taken decision on `parseProjectConfig` in the header. A module is a
    // library that happens to be consumed by source-merge, and COMPILING one
    // needs a target even where nothing is linked.
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

    // The OPTIONAL build-lifecycle hooks + project prerequisites. All three
    // ABSENT ⇒ EMPTY, never an error (the overwhelmingly common manifest
    // declares none of them, and must stay silent); a present-but-empty `[]`
    // is likewise fine. Every malformed ENTRY fails loud `C_MalformedJson` —
    // a hook that silently never runs, or a dependency whose `ref` pin is
    // silently ignored, is worse than a reject the user can act on.
    //
    // Shape only: this loader neither SPAWNS a script nor RESOLVES a
    // dependency, and it keeps every argv word and path VERBATIM exactly as
    // `sources[]` does (the pure-parser rule on `parseProjectConfig`).
    if (!readOptionalScripts(doc, "preBuildScripts", pc.preBuildScripts,
                             sourceLabel, rep))
        return std::nullopt;
    if (!readOptionalScripts(doc, "postBuildScripts", pc.postBuildScripts,
                             sourceLabel, rep))
        return std::nullopt;
    if (!readOptionalDependsOn(doc, "dependsOn", pc.dependsOn, sourceLabel, rep))
        return std::nullopt;

    // D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C): the
    // cross-build dependency artifact cache policy. Absent ⇒ nullopt ⇒ no cache
    // at all, so every manifest written before this field builds byte-
    // identically. Shape only here — WHERE the cache lives (the named override
    // variable's value, the per-user default chain) and WHETHER a given build
    // consults it are decided by `src/program/`, which has the environment, the
    // config root and the target this parser deliberately lacks.
    if (!readOptionalDependencyArtifactCache(doc, "dependencyArtifactCache",
                                             pc.dependencyArtifactCache,
                                             sourceLabel, rep))
        return std::nullopt;

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
        msg = "artifact profile '" + std::string{profile}
            + "' is not supported by language '" + std::string{language}
            + "' (supported: " + joinStrings(declared) + ").";
    }
    report(rep, DiagnosticCode::D_ArtifactProfileNotSupported,
           DiagnosticSeverity::Error, std::move(msg));
    return false;
}

bool enforceArtifactProfileFormat(std::span<std::string const> served,
                                  std::string_view profile,
                                  std::string_view formatName,
                                  std::span<std::string const> servingFormats,
                                  DiagnosticReporter& rep) {
    // Same generic membership predicate as the language gate — only the
    // diagnostic code + message differ (remediation-distinct: fix the
    // .lang.json [language gate] vs pick a different target/format or ship
    // the backend [this format gate]).
    if (artifactProfileSupported(served, profile)) return true;

    // What the CHOSEN format does serve, for the "and here is what you asked"
    // half of both messages. The empty served set is discriminated in the
    // wording (a relocatable / backend-less format serves nothing) — it is not
    // a separate decision; the decision was the single predicate above.
    std::string const servedClause =
        served.empty() ? std::string{"serves no artifact profiles"}
                       : "serves: " + joinStrings(served);

    // ── THE SPLIT (closes
    //    D-AP3-UNSERVED-PROFILE-MISREPORTS-AS-A-FORMAT-MISMATCH) ─────────────
    //
    // Which of the two rejects is TRUE is decided by the measurement the
    // caller supplies, never by this tier guessing: a profile no shipped
    // format serves cannot be fixed by picking another target, and telling its
    // author to pick one would send them through the entire target matrix
    // after a format that does not exist.
    if (servingFormats.empty()) {
        report(rep, DiagnosticCode::D_ArtifactProfileNoServingFormat,
               DiagnosticSeverity::Error,
               "artifact profile '" + std::string{profile}
               + "' is registered but no shipped object format implements it "
                 "yet — the format you chose ('" + std::string{formatName}
               + "') " + servedClause + ", and no other shipped format serves '"
               + std::string{profile}
               + "' either. No choice of target can build this profile until a "
                 "backend that emits it ships; build a different profile, or "
                 "ship that backend.");
        return false;
    }

    report(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch,
           DiagnosticSeverity::Error,
           "artifact profile '" + std::string{profile}
           + "' is not served by object format '" + std::string{formatName}
           + "' (" + servedClause + "). Object formats that DO serve '"
           + std::string{profile} + "': " + joinStrings(servingFormats)
           + " — choose a target whose object format is one of those.");
    return false;
}

} // namespace dss
