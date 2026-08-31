#include "program/dependency_lockfile.hpp"

#include "core/substrate/path_identity.hpp"       // genericSpelling
#include "core/types/config_document_parse.hpp"   // THE ONE config-document parse
#include "core/types/config_key_vocabulary.hpp"  // isDocumentationKey — the shared `$` carve-out
#include "core/types/parse_diagnostic.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <utility>

namespace dss {

namespace {

namespace fs = std::filesystem;
using json   = nlohmann::json;

constexpr std::string_view kDependenciesKey = "dependencies";

// The closed key sets. Anything else REJECTS, because this document is machine
// written: a key we do not recognise means the file was written by a different
// build of the compiler or edited by hand, and both are states where reading
// half of it and guessing the rest is worse than stopping.
constexpr std::array<std::string_view, 1> kRootKeys  = {kDependenciesKey};
constexpr std::array<std::string_view, 3> kEntryKeys = {"url", "ref",
                                                        "resolvedCommit"};


// EXACTLY ONE diagnostic per failed load, and it always ends with the same
// remediation. The lockfile is a REGENERABLE cache: the correct fix is never to
// repair it by hand (that is how it got broken), it is to delete it and let the
// next build re-resolve. Saying so in every arm is what keeps the hard failure
// from reading as a dead end.
void emitLockfileMalformed(DiagnosticReporter& rep, fs::path const& lockPath,
                           std::string detail) {
    report(rep, DiagnosticCode::C_MalformedJson, DiagnosticSeverity::Error,
           "dependency lockfile '" + core::genericSpelling(lockPath) + "': "
               + std::move(detail)
               + ". This file is written and read by the compiler and must not "
                 "be edited by hand; it records nothing that cannot be "
                 "recomputed, so delete it and re-run to re-resolve every "
                 "dependency. It is NOT treated as an empty cache, because a "
                 "state file the build cannot read is a different fact from a "
                 "state file that is not there yet.");
}

void emitLockfileWriteFailed(DiagnosticReporter& rep, std::string detail) {
    report(rep, DiagnosticCode::D_OutputDirCreateFailed,
           DiagnosticSeverity::Error,
           "dependency lockfile: " + std::move(detail)
               + ". The dependency cache state could not be recorded, so the "
                 "next build would re-acquire every git dependency — check "
                 "permissions and free space on the `.dss-deps` directory.");
}

// A required, non-empty JSON string member. Returns nullopt AFTER reporting.
[[nodiscard]] std::optional<std::string>
requiredString(json const& obj, std::string_view key, DiagnosticReporter& rep,
               fs::path const& lockPath, std::string const& where) {
    auto const it = obj.find(std::string{key});
    if (it == obj.end()) {
        emitLockfileMalformed(rep, lockPath,
                              where + " has no `" + std::string{key} + "`");
        return std::nullopt;
    }
    if (!it->is_string()) {
        emitLockfileMalformed(rep, lockPath, where + "'s `" + std::string{key}
                                                 + "` is not a string");
        return std::nullopt;
    }
    auto value = it->get<std::string>();
    if (value.empty()) {
        emitLockfileMalformed(rep, lockPath, where + "'s `" + std::string{key}
                                                 + "` is empty");
        return std::nullopt;
    }
    return value;
}

} // namespace

std::optional<DependencyLockfile>
DependencyLockfile::load(fs::path const& lockPath, DiagnosticReporter& rep) {
    std::error_code ec;
    // ABSENT ⇒ empty, silently. `exists` rather than a failed open, so a
    // PERMISSION error on an existing file does not masquerade as "no lockfile"
    // — that would be exactly the tolerant fallback U-4 forbids, arriving
    // through the filesystem instead of through the parser.
    if (!fs::exists(lockPath, ec) || ec) return DependencyLockfile{};

    std::ifstream in{lockPath, std::ios::binary};
    if (!in) {
        emitLockfileMalformed(rep, lockPath,
                              "the file exists but could not be opened for "
                              "reading");
        return std::nullopt;
    }
    std::string const text{std::istreambuf_iterator<char>{in},
                           std::istreambuf_iterator<char>{}};

    // THE ONE CONFIG PARSE (`core/types/config_document_parse.hpp`). A raw
    // `json::parse` here accepted a lockfile declaring `url` or
    // `resolvedCommit` TWICE and silently kept the LAST declaration —
    // D-CONFIG-A-DUPLICATE-JSON-KEY-IS-DROPPED-WITHOUT-A-DIAGNOSTIC.
    // ⚠ ON THIS DOCUMENT THE CONSEQUENCE IS THE SHARPEST OF THE ELEVEN
    // INGESTION SITES, which is why it was not left to the next cycle: the
    // build resolves a DIFFERENT SOURCE than the one a reader of the file
    // meets first, with no diagnostic and no diff — and this is the one config
    // document whose whole job is to say, reproducibly, which bytes were used.
    auto parsed = detail::parseConfigDocument(text);
    if (!parsed) {
        emitLockfileMalformed(rep, lockPath,
                              parsed.error().detailTextWithLocus(
                                  "the contents are not valid JSON: "));
        return std::nullopt;
    }
    json doc = std::move(*parsed);
    if (!doc.is_object()) {
        emitLockfileMalformed(rep, lockPath, "the root value is not an object");
        return std::nullopt;
    }
    if (auto const bad = detail::firstUnknownKey(doc, kRootKeys)) {
        emitLockfileMalformed(rep, lockPath,
                              "unrecognized top-level key `" + *bad + "`");
        return std::nullopt;
    }

    auto const deps = doc.find(std::string{kDependenciesKey});
    if (deps == doc.end()) {
        // Required, not defaulted-to-empty. Every document this build writes
        // has the member — including the one recording zero dependencies — so
        // its absence means the file came from somewhere else.
        emitLockfileMalformed(rep, lockPath,
                              "no `dependencies` object (every lockfile this "
                              "compiler writes has one, even when it is empty)");
        return std::nullopt;
    }
    if (!deps->is_object()) {
        emitLockfileMalformed(rep, lockPath, "`dependencies` is not an object");
        return std::nullopt;
    }

    DependencyLockfile out;
    for (auto it = deps->begin(); it != deps->end(); ++it) {
        std::string const& name = it.key();
        if (detail::isDocumentationKey(name)) continue;
        std::string const where = "dependency '" + name + "'";
        if (name.empty()) {
            emitLockfileMalformed(rep, lockPath,
                                  "`dependencies` has an entry with an empty "
                                  "name");
            return std::nullopt;
        }
        json const& entry = it.value();
        if (!entry.is_object()) {
            emitLockfileMalformed(rep, lockPath, where + " is not an object");
            return std::nullopt;
        }
        if (auto const bad = detail::firstUnknownKey(entry, kEntryKeys)) {
            emitLockfileMalformed(rep, lockPath,
                                  where + " has unrecognized key `" + *bad + "`");
            return std::nullopt;
        }

        LockedDependency locked;
        auto const url = requiredString(entry, "url", rep, lockPath, where);
        if (!url) return std::nullopt;
        locked.url = *url;

        auto const commit =
            requiredString(entry, "resolvedCommit", rep, lockPath, where);
        if (!commit) return std::nullopt;
        locked.resolvedCommit = *commit;

        // `ref` is OPTIONAL: absent means the manifest entry declared none.
        // Present-but-not-a-non-empty-string still rejects — an optional member
        // that is allowed to be malformed is not optional, it is unchecked.
        if (auto const refIt = entry.find("ref"); refIt != entry.end()) {
            auto const ref = requiredString(entry, "ref", rep, lockPath, where);
            if (!ref) return std::nullopt;
            locked.ref = *ref;
        }

        out.entries_.emplace(name, std::move(locked));
    }
    return out;
}

std::optional<LockedDependency>
DependencyLockfile::find(std::string const& name) const {
    auto const it = entries_.find(name);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

void DependencyLockfile::record(std::string name, LockedDependency entry) {
    entries_[std::move(name)] = std::move(entry);
}

bool DependencyLockfile::save(fs::path const&     lockPath,
                              DiagnosticReporter& rep) const {
    fs::path const dir = lockPath.parent_path();
    if (!dir.empty()) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        // `create_directories` reports "already exists" as NO error, so a true
        // `ec` here means the directory genuinely could not be established.
        if (ec) {
            emitLockfileWriteFailed(rep, "the directory '" + core::genericSpelling(dir)
                                             + "' could not be created: "
                                             + ec.message());
            return false;
        }
    }

    json doc;
    doc["$comment"] =
        "Managed by dsscp. Records the commit each git dependency was "
        "resolved to so a rebuild is reproducible and offline. Do not edit; "
        "delete it to force a full re-resolve.";
    // Always present, even with zero entries — `load` requires the member, and
    // a writer that omitted it on the empty case would produce a document its
    // own reader rejects.
    doc[std::string{kDependenciesKey}] = json::object();
    for (auto const& [name, entry] : entries_) {
        json row;
        row["url"] = entry.url;
        if (entry.ref) row["ref"] = *entry.ref;
        row["resolvedCommit"]                       = entry.resolvedCommit;
        doc[std::string{kDependenciesKey}][name]    = std::move(row);
    }

    // Two-space indent + a trailing newline: this file lands in a user's
    // project and gets opened. `dump` with an indent is also what `src/lsp/`
    // does for anything a human may read.
    std::string const text = doc.dump(2) + "\n";

    fs::path const tmp = lockPath.parent_path()
                       / (lockPath.filename().string() + ".tmp");
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        if (!out) {
            emitLockfileWriteFailed(rep, "the scratch file '"
                                             + core::genericSpelling(tmp)
                                             + "' could not be opened for "
                                               "writing");
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        // `bad()` covers the write itself; the explicit close below is what
        // surfaces a flush that failed on a full disk, which a destructor would
        // swallow — the exact silent-truncation shape `K_ImageWriteCloseFailed`
        // exists for one tier down.
        out.close();
        if (!out) {
            emitLockfileWriteFailed(rep, "writing the scratch file '"
                                             + core::genericSpelling(tmp)
                                             + "' failed");
            std::error_code rmec;
            fs::remove(tmp, rmec);
            return false;
        }
    }

    std::error_code ec;
    fs::rename(tmp, lockPath, ec);
    if (ec) {
        emitLockfileWriteFailed(rep, "'" + core::genericSpelling(tmp)
                                         + "' could not be renamed over '"
                                         + core::genericSpelling(lockPath)
                                         + "': " + ec.message());
        std::error_code rmec;
        fs::remove(tmp, rmec);
        return false;
    }
    return true;
}

} // namespace dss
