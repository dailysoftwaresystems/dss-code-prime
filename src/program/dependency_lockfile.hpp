#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

// dependency_lockfile — `.dss-deps/dss-lock.json`, the cache's ONE state of
// record.
//
// ★ ONE FILE FOR THE WHOLE GRAPH, AND NO PER-DEPENDENCY STATE FILE. This is the
// decision, not an implementation convenience. B.4 needs, per dependency, the
// `(url, ref)` that produced a checkout and the commit that checkout was left
// at; the obvious alternative — a small state file beside each
// `.dss-deps/<name>/` — puts the SAME fact in two places the moment anything
// aggregates them, and two places that must agree is the drift hazard this
// codebase rejects everywhere else (see `unsuppressable_codes.hpp` on "a second
// table listing codes is a second source of truth"). There is exactly one other
// piece of state, and we never write it: the checkout's own `HEAD`, read
// through `git rev-parse`. HEAD is git's file, maintained by git, and reading
// it is how U-3 catches a checkout somebody moved by hand — a state file we
// wrote could only ever tell us what we already believed.
//
// MACHINE-WRITTEN, MACHINE-READ, NEVER HAND-EDITED (U-1). The document carries
// a `$comment` saying so, which round-trips because the reader applies the
// codebase-wide `$`-documentation-key carve-out.
//
// ── ABSENT vs. UNPARSEABLE ARE DIFFERENT FACTS (U-4) ─────────────────────────
// ABSENT ⇒ an EMPTY lockfile and NO diagnostic. A first build has no lockfile
// and a miss is definitional; a diagnostic there would fire on every clean
// checkout of every project.
// PRESENT-BUT-UNPARSEABLE ⇒ `C_MalformedJson` and the load FAILS. Treating it
// as a miss is the tolerant fallback that hides a failure: the build would
// silently re-acquire everything, overwrite the damaged file, and never say
// that the state it was asked to reproduce was unreadable. U-4 chose
// `C_MalformedJson` because it is the established malformed-JSON band, and the
// remediation ("delete the file; it is a regenerable cache") goes in the
// message.
//
// STRICTNESS IS THE VERSION GUARD, and there is deliberately no version field.
// Every key is closed and every required member is checked, so a document this
// build does not understand — a future shape, a truncated write, a hand-edit —
// rejects loudly through the one path above rather than being half-read. A
// `lockfileVersion` integer would add a field whose only reader is that same
// reject, and would not catch a document that kept the version and changed a
// member's meaning.
//
// AGNOSTIC: names no source language, target processor or object format.

namespace dss {

// One recorded acquisition. The key it is stored under is the DERIVED cache
// name (`deriveDependencyCacheName`), i.e. the `.dss-deps/<name>` directory.
//
// Every field has a reader on the cache's hit path: `url` and `ref` because a
// lock entry that does not match the manifest entry that is asking is NOT a hit
// (the same `(url, ref)` pair 0xD020 discriminates collisions on), and
// `resolvedCommit` because the hit test is `revParse(HEAD) == resolvedCommit`.
struct DSS_EXPORT LockedDependency {
    std::string                url;
    // nullopt ⇒ the manifest entry declared no `ref`, which is a DIFFERENT
    // state from a ref spelled "" and is stored as an absent JSON member rather
    // than an empty string, so no sentinel has to be agreed on.
    std::optional<std::string> ref;
    std::string                resolvedCommit;

    friend bool operator==(LockedDependency const&,
                           LockedDependency const&) = default;
};

class DSS_EXPORT DependencyLockfile {
public:
    // Read `lockPath`. See the ABSENT vs. UNPARSEABLE note above.
    // nullopt ⇒ a `C_MalformedJson` was emitted and the caller must ABANDON the
    // build; it must not fall back to an empty lockfile.
    [[nodiscard]] static std::optional<DependencyLockfile>
    load(std::filesystem::path const& lockPath, DiagnosticReporter& rep);

    // The entry recorded under `name`, or nullopt. Returns a COPY — three short
    // strings — so a caller cannot hold a reference across a `record` that
    // rehashes the map.
    [[nodiscard]] std::optional<LockedDependency>
    find(std::string const& name) const;

    // Record (or replace) `name`'s entry. Replacement is the normal case: a
    // moving ref refreshed under `--force-git-cache` lands here with a new
    // commit, and that IS the update the flag exists to perform.
    void record(std::string name, LockedDependency entry);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // Write `lockPath`, creating its parent directory if needed. Returns false
    // after emitting a diagnostic.
    //
    // ★ WRITE-TEMP-THEN-RENAME, NOT TRUNCATE-IN-PLACE, AND THE ARTIFACT
    // WRITER'S DISCIPLINE DOES NOT TRANSFER HERE. `reportArtifactWritten`'s
    // exclusive-create rule exists because a build output must never silently
    // land on top of something else; this file is the OPPOSITE contract — it
    // MUST be replaced on every build that resolves anything, so exclusive
    // create would fail on the second build of every project.
    // Given that it must be overwritten, the choice is truncate-in-place vs.
    // temp+rename, and it is decided by what a HALF-WRITTEN file costs here: by
    // U-4 above, an unparseable lockfile is a HARD BUILD FAILURE. A truncate
    // leaves a window in which the path holds neither the old nor the new
    // document, so a Ctrl-C or a full disk in that window would leave the
    // project unbuildable until the operator finds and deletes a file inside a
    // git-ignored directory they have never heard of. `rename` is
    // replace-in-one-step on every host we build for (`MoveFileExW` with
    // `MOVEFILE_REPLACE_EXISTING` on Windows, `::rename` on POSIX), so the path
    // always holds ONE COMPLETE DOCUMENT — the old one or the new one.
    //
    // The temp file sits beside the lockfile (same directory, so the rename is
    // same-volume and cannot degrade into a copy) under a FIXED name: a crashed
    // build leaves exactly one stale scratch file that the next build
    // overwrites, rather than an accumulating pile of uniquely-named ones.
    //
    // Failure is reported as `D_OutputDirCreateFailed`. That code is the
    // driver's own "mkdir failure", split out of `D_FileNotFound` precisely so a
    // tool routing on codes can tell "the driver could not establish the place
    // it needed to write" from "your input is missing" — and every arm here
    // (create the directory, open the scratch file, write it, rename it) has
    // that one remediation: fix permissions or space under `.dss-deps/`. Same
    // remediation, same code, per this codebase's own rule that codes split on
    // remediation.
    [[nodiscard]] bool save(std::filesystem::path const& lockPath,
                            DiagnosticReporter&          rep) const;

private:
    // `std::map`, not `unordered_map`, so the written document's key order is
    // the same on every host and every run. A lockfile whose byte content
    // depended on hash iteration order would show a spurious diff on every
    // build and would defeat anyone diffing two machines' caches.
    std::map<std::string, LockedDependency> entries_;
};

} // namespace dss
