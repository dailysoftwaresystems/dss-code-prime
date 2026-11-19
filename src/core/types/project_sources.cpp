#include "core/types/project_sources.hpp"

#include "core/substrate/every_unit.hpp"    // runEveryUnit -- every entry, then every source, before the list fails
#include "core/substrate/path_identity.hpp"
#include "core/substrate/silent_unit.hpp"   // namingSilentUnits -- a unit that fails having said nothing is named

#include "core/types/glob_match.hpp"
#include "core/types/include_path_resolve.hpp"  // isRootedPath — the ONE rooted-path predicate
#include "core/types/parse_diagnostic.hpp"

#include <set>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace dss {

namespace fs = std::filesystem;

namespace {

// The driver-tier emit shape (`program.cpp`'s `emitDriver`): a buffer-less D_*
// error. Duplicated as a two-line local rather than exported, because exporting
// it would make an internal spelling into a cross-unit contract for no gain.
void emitDriver(DiagnosticReporter& rep, DiagnosticCode code, std::string msg) {
    dss::report(rep, code, DiagnosticSeverity::Error, std::move(msg));
}

// How the fail-loud messages NAME the base. An empty `baseDir` is a manifest
// named with no directory component, whose directory IS the process working
// directory, and the message must say so — "relative to ''" would be an error
// message that describes nothing.
std::string baseLabel(fs::path const& baseDir) {
    return baseDir.empty()
               ? std::string{"the manifest's directory, which is the working directory"}
               : ("the manifest's directory '" + core::genericSpelling(baseDir) + "'");
}

} // namespace

fs::path manifestDirectoryOf(fs::path const& manifestPath) {
    return manifestPath.parent_path();
}

fs::path resolveManifestPath(fs::path const& manifestDirectory,
                             fs::path const& entry) {
    // ⚠ `isRootedPath`, NOT `is_absolute()`. ✔MEASURED 2026-08-28 on a
    // REACHABLE UNC directory: such a path answers `is_absolute()` FALSE, so an
    // entry naming another machine was classified RELATIVE and re-based,
    // producing `C://localhost/C$/…` — the manifest's drive glued in front of an
    // authority.
    if (manifestDirectory.empty() || isRootedPath(entry)) return entry;
    // `.` names the manifest's directory ITSELF, so say it directly:
    // `manifestDirectory / "."` would carry a `.` component that every consumer
    // (the glob walk's `lexically_relative` first) would then have to reason
    // about. This case used to live in the glob walk's private copy of the rule.
    if (entry == fs::path{"."}) return manifestDirectory;
    return manifestDirectory / entry;
}

std::string resolveManifestPathSpelling(fs::path const& manifestDirectory,
                                        std::string const& entry) {
    // `core::genericSpelling`, NOT `generic_string()`: the latter collapses a
    // leading separator RUN, so even a correctly-rooted `//host/share` result
    // came out naming the local drive (the other half of the 2026-08-28
    // measurement above). An entry the rule leaves alone keeps its characters.
    fs::path const p{entry};
    if (manifestDirectory.empty() || isRootedPath(p)) return entry;
    return core::genericSpelling(resolveManifestPath(manifestDirectory, p));
}

std::optional<std::vector<std::string>>
expandAndDedupProjectSources(std::vector<std::string> const& sources,
                             fs::path const&                 baseDir,
                             DiagnosticReporter&             rep) {
    // ── PHASE 1: entries → files ────────────────────────────────────────────
    //
    // D-AP2-SOURCES-GLOB: expand any glob pattern BEFORE the multi-vs-single-CU
    // routing count is taken — so a `"src/**/*.c"` entry routes EXACTLY as if its
    // matches had been listed literally (a 2-match glob ⇒ 2 concrete sources ⇒
    // count 2 ⇒ `compileUnits`). A driver pre-pass, NOT the loader:
    // `parseProjectConfig` stays a pure JSON parser (it holds the raw pattern),
    // the filesystem side lives here.
    //   * LITERAL entry (no `* ? [` metacharacter) — kept as written, only
    //     RE-BASED (see the header: re-basing literals is AP6 M4a, and with an
    //     empty `baseDir` this is the pre-AP6 verbatim keep, byte for byte). A
    //     missing literal still fails DOWNSTREAM at CU build and is NOT newly
    //     rejected here.
    //   * GLOB entry — expanded against the filesystem. Matches are sorted
    //     (deterministic CU order). ZERO matches is a FAIL-LOUD error
    //     (`D_FileNotFound` naming the pattern) — a source pattern that names
    //     nothing is a mistake, not an empty no-op. A mid-expansion filesystem
    //     I/O error fails loud (`D_DirectoryScanFailed`).
    // ★ EVERY ENTRY IS EXPANDED, AND EVERY SOURCE IDENTIFIED, OR SAYS WHY NOT,
    // BEFORE THE LIST FAILS (`substrate::runEveryUnit`, the several-unit class's
    // one home, P69 H6c2): an entry that errs or matches nothing never stops
    // the entries after it, and phase 2 runs over every source phase 1
    // produced; the list fails after phase 2 when either phase failed. Netted:
    // each refusal names its pattern or its source. An entry or a source the
    // list names twice meets the failure its first mention reported, and
    // answers true -- the first said it, and fails the list.
    std::vector<std::string> expanded;
    expanded.reserve(sources.size());
    std::set<std::string> refused;   // the patterns and sources whose failure is said
    bool const everyEntryExpanded = substrate::runEveryUnit(
        sources.size(),
        substrate::namingSilentUnits(
            rep, "the project sources expansion",
            [&](std::size_t e) { return "sources entry '" + sources[e] + "'"; },
            [&](std::size_t e) {
        std::string const& entry = sources[e];
        if (!hasGlobMetacharacters(entry)) {
            // ★ THE HALF THAT USED TO BE SKIPPED. `expandGlob` re-bases what it
            // walks; a literal never reaches it, so re-basing MUST happen here or
            // the common `"src/lib.c"` form silently resolves against the wrong
            // tree (header, AP6 M4a) — through THE one base rule, so a literal
            // source, an `includes` entry and a hook program cannot be re-based
            // three different ways.
            expanded.push_back(resolveManifestPathSpelling(baseDir, entry));
            return true;
        }
        if (refused.contains(entry)) return true;   // said once, by its first mention
        GlobError ec;
        std::size_t const before = expanded.size();
        if (!expandGlob(entry, expanded, ec, baseDir)) {
            emitDriver(rep, DiagnosticCode::D_DirectoryScanFailed,
                       "project sources: filesystem error expanding glob pattern '"
                       + entry + "' (relative to " + baseLabel(baseDir)
                       + "): '" + core::genericSpelling(ec.where) + "' could not be read: "
                       + core::reasonText(ec.code));
            refused.insert(entry);
            return false;
        }
        if (expanded.size() == before) {
            emitDriver(rep, DiagnosticCode::D_FileNotFound,
                       "project sources: glob pattern '" + entry
                       + "' matched no files (relative to " + baseLabel(baseDir)
                       + ") — a source pattern that matches nothing is an error; "
                         "check the pattern and that the files exist.");
            refused.insert(entry);
            return false;
        }
        return true;
    }));

    // ── PHASE 2: cross-entry de-duplication ─────────────────────────────────
    //
    // A file matched by TWO overlapping entries — two overlapping globs
    // (`src/*.c` + `src/**/*.c`), or a literal alongside a glob that also matches
    // it — must compile ONCE, not once per entry. Without this, `compileUnits`
    // would build a DUPLICATE CU per repeat ⇒ a duplicate-symbol LINK error the
    // diagnostic can't tie back to the manifest. A redundant overlap should just
    // work — the UNION of unique files, each compiled once — matching
    // build-system expectations.
    //
    // The key is `weakly_canonical`, NOT `lexically_normal` (AP6 §3.4): the
    // spellings that must collapse are no longer just `./main.c` vs `main.c` but
    // ABSOLUTE vs RELATIVE, which lexical normalization cannot see through
    // because it never learns the cwd. `weakly_canonical` also resolves the
    // existing prefix's symlinks, so two routes to one file collapse as well.
    // FIRST occurrence wins (deterministic order — and the artifact NAME rides on
    // `[0]`, see the header) and keeps its ORIGINAL string; only later duplicates
    // are dropped.
    //
    // A canonicalization I/O error FAILS LOUD rather than falling back to the
    // weaker key: the fallback's failure mode is a duplicate CU and a
    // duplicate-symbol link error two tiers away from its cause, which is the
    // exact defect this key exists to prevent — silently re-introducing it on an
    // unreadable path would make the guarantee conditional on something the
    // manifest author cannot see. `weakly_canonical` does NOT error merely
    // because a path does not exist, so this arm means a genuine filesystem
    // failure (an unreadable parent), never "not generated yet".
    std::vector<std::string> deduped;
    deduped.reserve(expanded.size());
    std::unordered_set<core::PathIdentity> seen;
    seen.reserve(expanded.size());
    bool const everySourceIdentified = substrate::runEveryUnit(
        expanded.size(),
        substrate::namingSilentUnits(
            rep, "the project sources de-duplication",
            [&](std::size_t i) { return "source '" + expanded[i] + "'"; },
            [&](std::size_t i) {
        std::string& s = expanded[i];
        if (refused.contains(s)) return true;   // said once, by its first mention
        std::error_code kec;
        auto const id = core::PathIdentity::of(fs::path{s}, core::Use::Leaf, kec);   // a source FILE
        if (kec) {
            emitDriver(rep, DiagnosticCode::D_DirectoryScanFailed,
                       "project sources: filesystem error resolving source path '"
                       + s + "' (relative to " + baseLabel(baseDir) + ") while "
                         "de-duplicating the source list: " + core::reasonText(kec));
            refused.insert(s);
            return false;
        }
        if (seen.insert(id).second) {
            deduped.push_back(std::move(s));
        }
        return true;
    }));
    if (!everyEntryExpanded || !everySourceIdentified) return std::nullopt;
    return deduped;
}

} // namespace dss
