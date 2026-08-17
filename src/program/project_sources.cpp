#include "program/project_sources.hpp"

#include "core/types/glob_match.hpp"
#include "core/types/parse_diagnostic.hpp"

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

// How the fail-loud messages NAME the base. An empty `baseDir` is the process
// working directory and must say so — "relative to ''" would be an error message
// that describes nothing.
std::string baseLabel(fs::path const& baseDir) {
    return baseDir.empty() ? std::string{"the working directory"}
                           : ("'" + baseDir.generic_string() + "'");
}

} // namespace

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
    std::vector<std::string> expanded;
    expanded.reserve(sources.size());
    for (auto const& entry : sources) {
        if (!hasGlobMetacharacters(entry)) {
            // ★ THE HALF THAT USED TO BE SKIPPED. `expandGlob` re-bases what it
            // walks; a literal never reaches it, so re-basing MUST happen here or
            // the common `"src/lib.c"` form silently resolves against the wrong
            // tree (header, AP6 M4a). An ABSOLUTE literal keeps its own base; an
            // empty `baseDir` keeps the entry's exact characters.
            fs::path const p{entry};
            expanded.push_back(baseDir.empty() || p.is_absolute()
                                   ? entry
                                   : (baseDir / p).generic_string());
            continue;
        }
        std::error_code ec;
        std::size_t const before = expanded.size();
        if (!expandGlob(entry, expanded, ec, baseDir)) {
            emitDriver(rep, DiagnosticCode::D_DirectoryScanFailed,
                       "project sources: filesystem error expanding glob pattern '"
                       + entry + "' (relative to " + baseLabel(baseDir)
                       + "): " + ec.message());
            return std::nullopt;
        }
        if (expanded.size() == before) {
            emitDriver(rep, DiagnosticCode::D_FileNotFound,
                       "project sources: glob pattern '" + entry
                       + "' matched no files (relative to " + baseLabel(baseDir)
                       + ") — a source pattern that matches nothing is an error; "
                         "check the pattern and that the files exist.");
            return std::nullopt;
        }
    }

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
    std::unordered_set<std::string> seen;
    seen.reserve(expanded.size());
    for (auto& s : expanded) {
        std::error_code kec;
        auto const canon = fs::weakly_canonical(fs::path{s}, kec);
        if (kec) {
            emitDriver(rep, DiagnosticCode::D_DirectoryScanFailed,
                       "project sources: filesystem error resolving source path '"
                       + s + "' (relative to " + baseLabel(baseDir) + ") while "
                         "de-duplicating the source list: " + kec.message());
            return std::nullopt;
        }
        if (seen.insert(canon.generic_string()).second) {
            deduped.push_back(std::move(s));
        }
    }
    return deduped;
}

} // namespace dss
