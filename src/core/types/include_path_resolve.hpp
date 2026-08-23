#pragma once

// Shared include-path resolution primitives (FC15c). These are the ONE
// authoritative implementations of the two filesystem searches the include
// machinery performs, so the import resolver (post-parse `#include`) and the
// preprocessor's `__has_include` operator (C 6.10.1 / C23 6.10.1p4) can never
// disagree on "does this header exist". The crux (the silent-miscompile the
// FC15c plan-lock caught): `__has_include` MUST give the SAME answer `#include`
// would, and the ANGLE form's answer is NOT a naive `findInDirs(filename)` --
// DSS ships LANGUAGE-NEUTRAL JSON descriptors (`stdio.json`, not `stdio.h`) on
// the system path, so an angle include maps `<stem>.json` before searching.
// That mapping lives HERE (one chokepoint), called by BOTH sites.
//
// ── D-PP-HEADER-CASE-INSENSITIVE-PE: DSS FOLDS, THE HOST NEVER DOES ────────
//
// Every search below takes a `HeaderNameMatching` policy that the ACTIVE
// OBJECT FORMAT declares (`headerNameMatching` in `*.format.json`), and DSS
// performs the case comparison ITSELF. `std::filesystem::exists` is never the
// arbiter of a case question, because it answers with the HOST filesystem's
// convention: on NTFS/APFS it folds, on ext4 it does not. Handing the decision
// to it made header resolution HOST-dependent in BOTH directions -- a wrong
// REJECT of `<Windows.h>` for a pe64 target built on Linux, and a SILENT WRONG
// ACCEPT of `<Stdio.h>` for an elf target built on Windows. See
// `core/types/header_name_matching.hpp` for the measurements.
//
// Consequences of "DSS folds":
//   * CaseInsensitive ENUMERATES the directory and ASCII-fold-compares. It
//     does NOT take an `exists()` fast path, because on a case-sensitive host
//     BOTH `foo.json` and `Foo.json` can exist and picking the byte-exact one
//     would put the host's convention back in charge (see AmbiguousCase).
//   * CaseSensitive VERIFIES the on-disk spelling byte-for-byte after a hit,
//     so a folding host cannot smuggle `<Stdio.h>` through as `stdio.json`.
//   * The policy applies to EVERY path component, so `<SYS/TYPES.h>` resolves
//     `sys/types.json` under a case-insensitive format and does not under a
//     case-sensitive one.

#include "core/export.hpp"
#include "core/types/header_name_matching.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dss {

// The outcome of one filesystem search. There is DELIBERATELY no
// `operator bool` and no implicit conversion: every caller must look at
// `status`, because a caller that treated `AmbiguousCase` as "not found"
// would silently reinstate the host-dependent resolution this type exists to
// expose. Making the third state un-ignorable at COMPILE time is the point.
enum class HeaderSearchStatus : std::uint8_t {
    Found,          // exactly one on-disk entry matches under the policy
    NotFound,       // no entry matches under the policy
    AmbiguousCase,  // >= 2 DISTINCT entries fold-match — fail loud, never pick
};

struct DSS_EXPORT HeaderSearchResult {
    HeaderSearchStatus                 status = HeaderSearchStatus::NotFound;
    // Set iff `status == Found`.
    std::filesystem::path              path;
    // Set iff `status == AmbiguousCase`: every colliding on-disk path, sorted
    // by generic string so the diagnostic is reproducible run to run.
    std::vector<std::filesystem::path> ambiguousCandidates;

    [[nodiscard]] static HeaderSearchResult found(std::filesystem::path p) {
        return {HeaderSearchStatus::Found, std::move(p), {}};
    }
    [[nodiscard]] static HeaderSearchResult notFound() { return {}; }
    [[nodiscard]] static HeaderSearchResult
    ambiguous(std::vector<std::filesystem::path> candidates) {
        return {HeaderSearchStatus::AmbiguousCase, {}, std::move(candidates)};
    }
};

// ★ THE ONE SANCTIONED COLLAPSE of this tri-state into an `optional<path>`.
//
// A plain `r.status == Found ? optional{r.path} : nullopt` is a LOSSY
// conversion: it silently turns a fold COLLISION into "not found", and a
// caller that then treats not-found as a soft miss has just reinstated the
// host-dependent resolution this whole type exists to expose. That is not a
// hypothetical — it is exactly the defect the first cut of
// D-PP-HEADER-CASE-INSENSITIVE-PE shipped in `SynthBuilder::resolveQuote`,
// where the collision fell through to a SUPPRESSABLE
// `P_PreprocessorIncludeError` and the directive was dropped, so
// `--suppress`ing that code turned a case collision into a silently missing
// header.
//
// So the collapse is available in exactly one place and it takes the
// ambiguity handler as a REQUIRED argument: you cannot reach the optional
// without having written, at the call site, what happens on a collision. A
// site that genuinely must stay quiet (a speculative pre-scan, where C 6.10p1
// dead-branch inertness forbids reporting) has to say so out loud with a
// lambda a reviewer can see and `grep` can find — it cannot happen by
// omission. `[[nodiscard]]` on the result closes the other half.
template <class OnAmbiguous>
[[nodiscard]] std::optional<std::filesystem::path>
takeFound(HeaderSearchResult const& r, OnAmbiguous&& onAmbiguous) {
    switch (r.status) {
        case HeaderSearchStatus::Found:
            return r.path;
        case HeaderSearchStatus::NotFound:
            return std::nullopt;
        case HeaderSearchStatus::AmbiguousCase:
            std::forward<OnAmbiguous>(onAmbiguous)(r.ambiguousCandidates);
            return std::nullopt;
    }
    return std::nullopt;   // unreachable — every status handled above
}

// Resolve ONE relative name inside ONE directory under `matching`. This is the
// atom every search below is built from, exposed because the preprocessor's
// quote-include search adds its own per-candidate `is_regular_file` filter and
// must NOT re-derive the case rule to do it (that private second resolver was
// exactly how `#include` and `__has_include` drifted apart before FC15c).
//
// `relName` may carry subdirectories (`sys/types.json`); the policy is applied
// to EVERY component. A directory that cannot be enumerated (unreadable, or
// absent) yields NotFound -- the same verdict the pre-policy `exists()` gave.
[[nodiscard]] DSS_EXPORT HeaderSearchResult
resolveInDir(std::filesystem::path const& dir, std::string_view relName,
             HeaderNameMatching matching);

// Search `dirs` for `filename` (a relative header name). First matching dir
// wins. An absolute name resolves against the filesystem directly (the dir
// list is ignored) with the policy applied to every component below the root.
// This is the QUOTE form's includeDirs search and the ANGLE form's systemDirs
// search -- the only difference between the two is WHICH dir list is passed
// and the self-dir prepend (quote-only), handled by the caller (see
// `resolveIncludePath`).
//
// An AmbiguousCase in ANY dir stops the search immediately and is returned:
// continuing to a later dir would make the answer depend on whether the host
// could even represent the collision, which is the defect, not the remedy.
[[nodiscard]] DSS_EXPORT HeaderSearchResult
findInDirs(std::string_view filename, std::span<std::filesystem::path const> dirs,
           HeaderNameMatching matching);

// The DIRECTORY a quote-include resolves against, derived from the NAME of the
// source buffer that CONTAINS the directive. THE ONE derivation: every tier
// that needs an includer directory calls this, so none of them can re-derive it
// differently (the import resolver's directive walk, the preprocessor's
// SynthBuilder scan, its `MacroExpander` includer dir, and its `#embed` origin
// lookup all did, identically and identically wrongly).
//
// ── AN EMPTY PARENT IS THE PROCESS WORKING DIRECTORY, NOT "NO DIRECTORY" ────
//
// D-PP-BARE-RELATIVE-MAIN-PATH-DEFEATS-THE-INCLUDER-DIRECTORY-SEARCH
//
// `fs::path{"main.c"}.parent_path()` is the EMPTY path, and every one of those
// four sites handed that empty path straight to a resolver whose self-dir arm
// is guarded by `if (!includingDir.empty())`. So `dss --compile main.c` -- the
// form a user types most -- SKIPPED the includer-directory arm entirely and
// reported `quote include not found` for a header sitting right beside the
// source, while `./main.c`, `sub/main.c` and an absolute path all resolved it.
// ✔MEASURED through the CLI, one variable changed per arm; gcc resolves the
// bare form, so by the reference-compilers rule the behaviour is REQUIRED.
//
// A name with no directory component names a file in the process working
// directory, so that is what this returns: `.`, spelled relatively on purpose.
// `fs::current_path()` would be equally "correct" and is the wrong answer --
// it would make every header resolved through the self-dir arm carry an
// ABSOLUTE path into `__FILE__`, into diagnostics and into the line-map, so the
// bare form would stop agreeing with the `./main.c` form it is supposed to be
// identical to. `.` makes them byte-identical (`.\h28.h` either way).
//
// An EMPTY `sourceName` is the one input that still yields an empty path, and
// that arm is the reason this is a derivation rather than a `.` substitution
// inside the resolvers: a buffer with no name at all has no including FILE, and
// "there is no including file" must stay distinguishable from "the including
// file lives in the working directory". Pushing the substitution down into
// `resolveIncludePath`/`resolveQuote` would collapse the two, and would need
// the same edit in both -- two copies of one rule, free to drift.
[[nodiscard]] DSS_EXPORT std::filesystem::path
includingDirectoryOf(std::string_view sourceName);

// QUOTE-form (`#include "h"` / `__has_include("h")`) resolution: try the
// including file's own directory FIRST, then each of `includeDirs`. Mirrors C's
// quote-include search order (C 6.10.2p3). An absolute name resolves directly.
// `includingDir` may be empty (no self-dir prepend then) -- that is the "no
// including FILE" case and NOT "the includer has no directory component", which
// `includingDirectoryOf` above resolves to `.` before it ever reaches here.
// This is the SHARED quote search used by both the import resolver and
// `__has_include`.
[[nodiscard]] DSS_EXPORT HeaderSearchResult
resolveIncludePath(std::string_view filename,
                   std::filesystem::path const&                includingDir,
                   std::span<std::filesystem::path const>      includeDirs,
                   HeaderNameMatching                          matching);

// ANGLE-form (`#include <h>` / `__has_include(<h>)`) resolution -- the
// FUNNEL the FC15c plan-lock mandates (one chokepoint, no drift). DSS ships a
// LANGUAGE-NEUTRAL JSON descriptor per system header (`<stdio.h>` ->
// `stdio.json`), NOT a `.h` source file, so the search is NOT `filename` on the
// path: it is the requested path with its extension dropped + `.json`,
// PRESERVING any subdirectory, on `systemDirs` (`<stdio.h>` -> `stdio.json`;
// `<sys/types.h>` -> `sys/types.json`, distinct from `<time.h>` -> `time.json`) --
// agnostic of the requested extension spelling (`<stdio.h>`, `<stdio>` ->
// `stdio.json`). `#include <stdio.h>` and `__has_include(<stdio.h>)` BOTH call
// this so their existence answers always agree.
//
// The `<stem>` -> `.json` rewrite is pure byte slicing and stays that way; the
// CASE question is answered afterwards, by the policy, when the rewritten name
// meets the filesystem. That ordering is what lets ONE `windows.json` serve
// every spelling of `<Windows.h>`: an alias file differing only in case could
// not be checked out at all on NTFS or a default APFS volume, so aliasing is
// not merely discouraged here, it is unrepresentable on two of three hosts.
[[nodiscard]] DSS_EXPORT HeaderSearchResult
resolveSystemDescriptor(std::string_view                       filename,
                        std::span<std::filesystem::path const> systemDirs,
                        HeaderNameMatching                     matching);

// ANGLE-form resolution VERDICT (D-INCLUDE-ANGLE-SOURCE-FALLBACK): what an
// `#include <h>` / `__has_include(<h>)` resolves to, in priority order.
// ★ THE TWO AMBIGUOUS ARMS ARE SEPARATE ON PURPOSE, AND THE REASON IS A
// DIAGNOSTIC-OWNERSHIP ONE. The angle form searches two different places, and
// only ONE of them is visible to both tiers:
//   * the DESCRIPTOR half (`<stem>.json` on systemDirs) is re-resolved by the
//     post-parse import resolver, which owns the loud report for it;
//   * the SOURCE half (a real header on the `-I` path) is NEVER re-resolved
//     there — the import resolver's angle arm calls `resolveSystemDescriptor`
//     alone — so the PREPROCESSOR is the only tier that can report it.
// A single merged `AmbiguousCase` made that distinction unrepresentable, and
// the result was a source-half collision surfacing as
// `F_ShippedHeaderNotFound` — loud, but naming the wrong defect, while the
// diagnostic whose entire purpose is to list the colliding paths never fired.
enum class AngleIncludeKind {
    Descriptor,           // a shipped `<stem>.json` on systemDirs (the DSS neutral model)
    Source,               // NO descriptor, but a REAL source header on the -I includeDirs
    NotFound,             // neither — a fatal miss (fail-loud F_ShippedHeaderNotFound)
    AmbiguousDescriptor,  // >=2 fold-matching `<stem>.json` — import resolver reports
    AmbiguousSource,      // >=2 fold-matching -I headers — the PP is the only reporter
};

struct AngleIncludeResolution {
    AngleIncludeKind      kind;
    std::filesystem::path path;  // resolved descriptor/source path; empty otherwise
    // Set iff kind is one of the Ambiguous* arms (see
    // HeaderSearchResult::ambiguousCandidates).
    std::vector<std::filesystem::path> ambiguousCandidates;
};

// The ONE angle-include resolver funnel (FC15c + D-INCLUDE-ANGLE-SOURCE-FALLBACK).
// SHARED by the preprocessor's angle `#include <h>` arm AND its
// `__has_include(<h>)` operator so their "does this header exist" answers can
// never drift. Priority (C 6.10.2p2 + the DSS neutral descriptor model):
//   1. Descriptor — `resolveSystemDescriptor(filename, systemDirs)` (`<stem>.json`).
//      Keys on descriptor-FILE existence ALONE; per-format availability is the
//      CALLER's authoritative verdict — an existing-but-format-unavailable
//      descriptor STILL returns Descriptor (flowing the caller's unchanged
//      availability path); it does NOT fall through to a source header.
//   2. Source — NO descriptor, but `filename` names a REAL source header on the
//      `-I` `includeDirs` (`findInDirs`). The angle form does NOT search the
//      including file's OWN directory (C 6.10.2p2, UNLIKE quote) — `includeDirs`
//      alone, never a self-dir prepend. This is the real-header fallback that lets
//      an angle `<sqlite3.h>`/`<sqlite3ext.h>` (a header on the -I path with no
//      shipped descriptor) resolve, matching standard C's angle-include search.
//   3. NotFound — neither. The include is a fatal miss (the caller emits the
//      unsuppressable F_ShippedHeaderNotFound).
//   4. AmbiguousDescriptor / AmbiguousSource — a fold collision at step 1 or
//      step 2. Reported, never resolved; kept DISTINCT because different tiers
//      own the report (see the enum's note).
[[nodiscard]] DSS_EXPORT AngleIncludeResolution
resolveAngleInclude(std::string_view                       filename,
                    std::span<std::filesystem::path const> systemDirs,
                    std::span<std::filesystem::path const> includeDirs,
                    HeaderNameMatching                     matching);

} // namespace dss
