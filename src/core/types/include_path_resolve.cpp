#include "core/types/include_path_resolve.hpp"

#include "core/substrate/path_identity.hpp"  // genericSpelling — the ONE lossless
                                             // separator normalisation
#include "core/types/ascii_case.hpp"   // asciiToLower — the ONE folding helper

#include <algorithm>
#include <vector>
#include <utility>

namespace dss {

namespace fs = std::filesystem;

namespace {

// The FILESYSTEM's own character type: `char` on POSIX, `wchar_t` on Windows.
// Nothing below branches on which — every name comparison in this file happens
// in this type, and the two helpers underneath are written so that the same
// source line is correct for both (see `kDotChar`).
using NativeChar   = fs::path::value_type;
using NativeString = fs::path::string_type;

// `.` is in C's basic character set, so `NativeChar{'.'}` is the SAME code
// point whichever type `NativeChar` turns out to be. That is what lets the
// navigation test in `descend` stay platform-agnostic with no `L` prefix and
// no `#ifdef`.
constexpr NativeChar kDotChar = NativeChar{'.'};

[[nodiscard]] bool isDotComponent(NativeString const& s) {
    return s.size() == 1 && s[0] == kDotChar;
}
[[nodiscard]] bool isDotDotComponent(NativeString const& s) {
    return s.size() == 2 && s[0] == kDotChar && s[1] == kDotChar;
}

// Match ONE path component `name` inside `dir` under `matching`.
//
// ★ The whole point of this function is that IT decides, not the host. Read
// each arm as "what would a filesystem with THIS convention answer", never as
// "what does this machine's filesystem answer".
//
// ★★ EVERY COMPARISON IS ON THE NATIVE STRING, NEVER ON A NARROWED COPY
// (`D-PP-HEADER-CASE-NON-ASCII-NAME-NARROWING-THROW`). `path::string()` would
// convert a Windows `wchar_t` directory entry to `char`, and that conversion
// is not a formality — MEASURED on this project's own two Windows toolchains,
// same host, ACP 1252:
//   * MS STL 14.51 THROWS `std::system_error` ("no mapping for the Unicode
//     character in the target multi-byte code page") for ANY name the active
//     code page cannot represent — a plain CJK filename does it. `main()`
//     installs no handler, so that is a process ABORT out of the PREPROCESSOR,
//     not a diagnostic.
//   * libstdc++ 13.2 (the MinGW gate toolchain) throws `filesystem_error`
//     ("Cannot convert character sequence") for a name NTFS accepts but UTF-16
//     cannot encode — a lone surrogate, which `CreateFileW` and `std::ofstream`
//     both create happily.
//   * And for names that DO convert the two disagree on the RESULT (CP1252
//     `EF` vs UTF-8 `C3 AF` for `naive` with a diaeresis), so a narrowed
//     comparison makes header resolution depend on which C++ compiler built
//     DSS — the same host-dependence class this whole axis removes, one layer
//     down. Worse than the throw, because it is silent.
// The comparison never needed the narrow form: the conversion was pure loss.
// `name` therefore arrives as an `fs::path`, built ONCE by the caller from the
// requested (source-text) name, and only its `native()` is ever compared.
HeaderSearchResult matchComponent(fs::path const& dir, fs::path const& name,
                                  HeaderNameMatching matching) {
    std::error_code ec;
    if (matching == HeaderNameMatching::CaseSensitive) {
        // Cheap DEFINITIVE reject first. If no entry answers to these exact
        // bytes even under the host's own (possibly folding) rules, then no
        // byte-exact entry can exist either — `exists()` is only ever MORE
        // permissive than we are, never less, so a false here is trustworthy.
        fs::path const candidate = dir / name;
        if (!fs::exists(candidate, ec)) return HeaderSearchResult::notFound();
        // `exists()` said yes — but a case-INSENSITIVE host may have FOLDED to
        // get there. Verify the REAL on-disk spelling ourselves; this is the
        // arm that stops `#include <Stdio.h>` from silently compiling for an
        // elf target on a Windows/macOS host. A directory we cannot enumerate
        // fails CLOSED (NotFound): unable to verify is not permission to
        // assume, and the caller's miss diagnostic is loud.
        for (fs::directory_iterator it{dir, ec}, end; !ec && it != end;
             it.increment(ec)) {
            if (it->path().filename().native() == name.native())
                return HeaderSearchResult::found(candidate);
        }
        return HeaderSearchResult::notFound();
    }

    // CaseInsensitive. ALWAYS enumerate — there is no correct `exists()` fast
    // path. On a case-sensitive host BOTH `foo.json` and `Foo.json` can exist;
    // accepting the byte-exact one would hand the answer back to the host,
    // because a case-insensitive host cannot even hold that pair. Every
    // fold-match is collected and >= 2 is a LOUD failure, never a pick.
    NativeString const wanted = asciiToLower(name.native());
    std::vector<fs::path> hits;
    for (fs::directory_iterator it{dir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (asciiToLower(it->path().filename().native()) == wanted)
            hits.push_back(it->path());
    }
    if (hits.empty()) return HeaderSearchResult::notFound();
    if (hits.size() == 1) return HeaderSearchResult::found(std::move(hits.front()));
    // Deterministic order, so the collision diagnostic reads the same run to
    // run. Ordered on the NATIVE string for the same reason the matching is:
    // `generic_string()` here would throw on precisely the names a collision
    // report exists to NAME. Every hit came from the same `dir`, so ordering by
    // native code unit orders them by filename.
    std::sort(hits.begin(), hits.end(),
              [](fs::path const& a, fs::path const& b) {
                  return a.native() < b.native();
              });
    return HeaderSearchResult::ambiguous(std::move(hits));
}

// Walk `base` down every component of `rel` under `matching`. `rel` must be
// relative; the caller splits an absolute name into (root_path, remainder).
HeaderSearchResult descend(fs::path base, fs::path const& rel,
                           HeaderNameMatching matching) {
    for (fs::path const& comp : rel) {
        // NATIVE, not `comp.string()`: the requested name is already a path,
        // and narrowing it back would reintroduce the throw/lossy conversion
        // documented on `matchComponent` — on the REQUEST side this time.
        // `rel` was built from the source text ONCE, by the caller.
        NativeString const& name = comp.native();
        // A `.`/empty component (from a trailing separator, or `a/./b`) moves
        // nowhere and has no on-disk name to match.
        if (name.empty() || isDotComponent(name)) continue;
        // `..` is a NAVIGATION component, not a filename: no directory listing
        // ever contains an entry called `..`, so running it through the
        // name-matcher below would turn every `#include "../shared.h"` into a
        // miss. Append it and let the OS resolve it exactly as the pre-policy
        // `dir / rel` did — the case rule still applies to every component that
        // actually names a file.
        if (isDotDotComponent(name)) { base /= comp; continue; }
        HeaderSearchResult step = matchComponent(base, comp, matching);
        if (step.status != HeaderSearchStatus::Found) return step;
        base = std::move(step.path);
    }
    return HeaderSearchResult::found(std::move(base));
}

// ── [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]] ───────────────────────
//
// ★★★ THE INTERMITTENCY IS ROOT-CAUSED AND IT WAS NEVER THE FILESYSTEM. The
// first cut of this file made a UNC include resolve 23 times in 30 and nobody
// could say why the other 7 missed; every filesystem primitive here was hammered
// in isolation (>2000 operations, 0 failures) and `--jobs 1` did not move the
// rate, so the cause was correctly stated as "inside DSS, not root-caused". IT
// WAS UNDEFINED BEHAVIOUR IN `isListedInItsParent` BELOW — a reference bound to
// the `native()` of a TEMPORARY `fs::path` — and the named lead
// (`weakly_canonical` failing on UNC, degrading `core::PathIdentity`) is
// REFUTED as its cause: the miss happens in `rootPrefixOf`, strictly before any
// identity key is computed. See that function for the measurement.

// The ROOT PREFIX of a rooted path, plus the components below it.
//
// ★★★ WHY THE MODEL'S OWN SPLIT CANNOT BE USED, MEASURED RATHER THAN ARGUED.
// `descend` used to be handed `rel.root_path()` and `rel.relative_path()`, and
// that split IS NOT LOSSLESS on every path model:
//     path("//server/share/x.h").root_path()     -> "/"   (ONE slash)
//     path("//server/share/x.h").relative_path() -> "server/share/x.h"
//     root_path() / relative_path()              -> "/server/share/x.h"  != input
// The authority is demoted to an ordinary component, so the walk restarted from
// the bare separator, asked for a directory named after the SERVER, found
// nothing, and reported the HEADER missing. The same split round-trips fine for
// `C:/...` and for a relative path -- which is exactly why every local test
// stayed green while the cross-host route was dead.
//
// ★★ THE RULE, AND IT ASKS THE FILESYSTEM RATHER THAN THE PLATFORM. A root
// prefix is exactly the part of a path that NO DIRECTORY LISTING CONTAINS: a
// drive letter is not an entry in any directory, the bare separator is not, and
// neither is a server nor the share below it. So the root prefix is the DEEPEST
// ancestor whose own parent cannot be enumerated -- and the model's `root_path()`
// when there is no such ancestor. ✔MEASURED walking a real UNC header on this
// host: the server alone does not exist; the SHARE exists but its parent cannot
// be enumerated (that is the boundary); every ancestor below it has an
// enumerable parent AND is listed in it. On a local drive path EVERY ancestor is
// listed in its own parent, so the boundary stays at the drive root and local
// behaviour is byte-for-byte what it was -- a wrong-case absolute include is
// still rejected at the first component.
//
// ⚠ NO PLATFORM MACRO, NO UNC SPELLING, AND DELIBERATELY NOT A SECOND COPY OF
// `platformModelsUncRoots` (src/lsp/workspace_project.cpp, cycle P43). That
// predicate asks "does this path type model an authority as a root NAME", which
// is the right question for URI round-tripping and the wrong one here: this code
// does not care how the authority is modelled, only where the part of the path
// that can be case-checked BEGINS. Two notions of "does this platform model UNC
// roots" would be the two-oracles problem; there is still exactly one, and this
// is not it.
//
// The ancestors come from the `parent_path()` chain, which ✔MEASURED preserves
// the input exactly in every case tried (both UNC spellings, a drive path, a
// relative path) -- unlike accumulating `base /= component`, which loses the
// authority and flips separators besides.
// Is `p` an ENTRY IN ITS OWN PARENT'S LISTING? That is the operational form of
// "a directory listing contains this". A server, a share, a drive letter and the
// bare separator all answer NO; every ordinary directory and file answers YES.
// Byte-exact on the native string, and a fold-match would only mean "this IS an
// ordinary component", which is the safe direction.
//
// ★★★ `want` IS A VALUE AND MUST NEVER BE A REFERENCE AGAIN. This line shipped
// as `NativeString const& want = p.filename().native();` and that is UNDEFINED
// BEHAVIOUR: `filename()` returns an `fs::path` BY VALUE, `native()` hands back
// a reference INTO that temporary, and binding a reference to the RESULT OF A
// CALL does not extend the temporary's lifetime the way binding to the
// temporary itself would. The path died at the end of that statement and the
// loop below then compared every directory entry against freed memory.
//
// ✔MEASURED — the whole of this row's unexplained intermittency, in one A/B
// inside a single process, same path, only the binding differing:
//     path                shipped (reference)   fixed (value)
//     //wsl.localhost     YES 141/200           YES   0/200
//     \\wsl.localhost     YES  86/200           YES   0/200
//     C:\Users            YES 200/200           YES 200/200
// ⓘ THE DIVERGENCE ABOVE IS MEASURED; THE MECHANISM BELOW IS INFERRED, and the
// fix does not rest on it. The freed buffer is very likely reused by
// `directory_iterator`'s own allocation and comes back holding an ENTRY NAME, so
// the comparison matches on the first entry -- which fits the observed
// "answers YES immediately". Whatever the allocator actually did, reading freed
// memory has no defined answer. It fails toward YES,
// which for the shallowest ancestor of a UNC path collapses `rootPrefixOf`'s
// boundary to 0, sends `descend` at `root_path()` -- ONE separator, the local
// drive root -- and reports the header missing.
//
// ★★ WHY ONLY UNC PATHS EVER SHOWED IT, which is also why a local control was
// inert at 15/15 in every arm and could never have caught this. On a drive path
// EVERY ancestor genuinely IS listed in its parent, so a spurious YES and the
// true YES agree; the boundary lands one step apart and both splits resolve. A
// UNC path is the one shape where the correct answer at the shallowest ancestor
// is NO and a spurious YES changes the verdict.
[[nodiscard]] bool isListedInItsParent(fs::path const& p) {
    std::error_code ec;
    NativeString const want = p.filename().native();
    if (want.empty()) return false;
    for (fs::directory_iterator it{p.parent_path(), ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (it->path().filename().native() == want) return true;
    }
    return false;
}

[[nodiscard]] fs::path rootPrefixOf(fs::path const& full, fs::path& below) {
    std::vector<fs::path> chain;   // deepest first
    for (fs::path q = full; !q.empty() && q != q.parent_path();
         q = q.parent_path()) {
        chain.push_back(q);
    }
    below.clear();
    if (chain.empty()) return full.root_path();
    // The LEADING RUN of ancestors that no listing contains. It stops at the
    // first ordinary component, so a MISSING header -- whose own last component
    // is also unlisted -- can never be swallowed into the "root": that component
    // is at the END of the path, never in the leading run.
    std::size_t boundary = 0;
    for (std::size_t shallow = 0; shallow < chain.size(); ++shallow) {
        if (isListedInItsParent(chain[chain.size() - 1 - shallow])) break;
        boundary = shallow + 1;
    }
    fs::path base = (boundary == 0) ? full.root_path()
                                    : chain[chain.size() - boundary];
    // \u26a0 THE GUARD THAT KEEPS A MISSING HEADER MISSING, AND IT IS STRUCTURAL ON
    // PURPOSE. A path on an unreachable server has NO listed ancestor at all, so
    // the run would swallow the WHOLE path, leave nothing to verify, and
    // `descend` would answer "found" for a file nobody can open. If the run
    // consumed every component it is not a root prefix: fall back to the model's
    // own `root_path()`, which walks and fails exactly as it did before.
    //
    // \u2605\u2605 IT ASKS THE FILESYSTEM NOTHING, AND THAT IS THE POINT. The first
    // version guarded with `is_directory(base)`, and \u2714MEASURED over 30 compiles
    // the UNC arms failed 6 times: a cold SMB connection makes a probe on a UNC
    // ancestor fail transiently, "could not tell" was read as "not a directory",
    // and the fallback then lost the header. A decision that must be STABLE
    // cannot rest on a call that fails for reasons that are not about the path.
    if (boundary >= chain.size()) {
        base     = full.root_path();
        boundary = 0;
    }
    for (std::size_t shallow = boundary; shallow < chain.size(); ++shallow) {
        below /= chain[chain.size() - 1 - shallow].filename();
    }
    return base;
}

// Resolve a name that may be absolute or relative-to-`dir`, applying the
// policy to every component the SOURCE wrote. Directory-list entries
// (`-I` dirs, systemDirs) are NOT case-checked: they come from the driver and
// the build config, not from a header name in the program text.
HeaderSearchResult resolveMaybeAbsolute(fs::path const& rel, fs::path const& dir,
                                        HeaderNameMatching matching) {
    if (isRootedPath(rel)) {
        fs::path below;
        fs::path const base = rootPrefixOf(rel, below);
        return descend(base, below, matching);
    }
    return descend(dir, rel, matching);
}

} // namespace

// [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]] -- see the header for the
// measurement and for why this is exported rather than repeated per tier.
bool isRootedPath(fs::path const& p) {
    return p.is_absolute() || p.has_root_directory();
}

// D-C-ATOMICS-RUNTIME-IS-OURS-ON-PE64 lifted this out of
// `shipped_lib_descriptor.cpp`, where it was a file-local lambda, when a SECOND
// config document kind gained the same key. See the header for why a duplicated
// containment check is the shape that rots.
bool shippedConfigRelativePathEscapes(std::string_view spelling) {
    if (spelling.empty()) return true;
    if (spelling.find('\\') != std::string_view::npos) return true;
    fs::path const asPath{std::string{spelling}};
    // `isRootedPath`, NOT `is_absolute() || has_root_name()` — the measurement
    // is in the header, and the hole it closes admitted `//host/share/evil.c`.
    if (isRootedPath(asPath)) return true;
    for (auto const& seg : asPath) {
        if (seg == ".." || seg == ".") return true;
    }
    return false;
}

HeaderSearchResult resolveInDir(fs::path const& dir, std::string_view relName,
                                HeaderNameMatching matching) {
    return resolveMaybeAbsolute(fs::path{relName}, dir, matching);
}

HeaderSearchResult findInDirs(std::string_view              filename,
                              std::span<fs::path const>     dirs,
                              HeaderNameMatching            matching) {
    fs::path const rel{filename};
    // [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]: `isRootedPath`, never a
    // bare `is_absolute()` -- see that predicate for the measurement.
    if (isRootedPath(rel)) return resolveMaybeAbsolute(rel, {}, matching);
    for (fs::path const& dir : dirs) {
        HeaderSearchResult r = descend(dir, rel, matching);
        // An ambiguity anywhere ends the search: falling through to a later
        // dir would silently prefer whichever host could represent the tree.
        if (r.status != HeaderSearchStatus::NotFound) return r;
    }
    return HeaderSearchResult::notFound();
}

fs::path includingDirectoryOf(std::string_view sourceName) {
    // No NAME at all -> no including FILE. Stays empty so the callers' self-dir
    // guard skips the arm; see the header for why that case must not collapse
    // into the working-directory one.
    if (sourceName.empty()) return {};
    fs::path const dir = fs::path{sourceName}.parent_path();
    if (!dir.empty()) return dir;
    // A bare name (`main.c`) names a file in the PROCESS WORKING DIRECTORY.
    // `.` is a real directory the searches below can enumerate; the empty path
    // is not -- MEASURED on this project's own toolchain, `fs::exists("h.h")`
    // is cwd-relative and answers TRUE, but `fs::directory_iterator{fs::path{}}`
    // fails with "Not a directory", so `descend` returns NotFound down BOTH the
    // CaseSensitive and CaseInsensitive arms. That is why removing the callers'
    // `!includingDir.empty()` guard would not have fixed anything: the defect is
    // the derivation, not the guard.
    // `.` is in C's basic character set, so no `L`-prefix / `#ifdef` is needed
    // for the native-`wchar_t` build (same reasoning as `kDotChar` above).
    return fs::path{"."};
}

HeaderSearchResult resolveIncludePath(std::string_view              filename,
                                      fs::path const&               includingDir,
                                      std::span<fs::path const>     includeDirs,
                                      HeaderNameMatching            matching) {
    fs::path const rel{filename};
    // [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]: a UNC include answers
    // FALSE to `is_absolute()` on this build and was therefore searched against
    // the include dirs as though it were a relative name.
    if (isRootedPath(rel)) return resolveMaybeAbsolute(rel, {}, matching);
    if (!includingDir.empty()) {
        HeaderSearchResult r = descend(includingDir, rel, matching);
        if (r.status != HeaderSearchStatus::NotFound) return r;
    }
    return findInDirs(filename, includeDirs, matching);
}

HeaderSearchResult resolveSystemDescriptor(std::string_view          filename,
                                           std::span<fs::path const> systemDirs,
                                           HeaderNameMatching        matching) {
    // `<stem>.json`, PRESERVING any subdirectory so a POSIX `sys/*` header maps
    // to a distinct descriptor and never collides with a top-level header of the
    // same stem: `<sys/types.h>` -> `sys/types.json`, `<sys/time.h>` ->
    // `sys/time.json` (DISTINCT from `<time.h>` -> `time.json`). A flat header
    // keeps its flat name (`<stdio.h>` -> `stdio.json`). Agnostic of the requested
    // extension (`<stdio.h>`, `<stdio>` both -> `stdio.json`). This is the SINGLE
    // FC15c funnel every consumer shares (import_resolver typed-surface +
    // preprocessor macro inject + `__has_include`), so they stay in lock-step.
    //
    // The rewrite is pure byte slicing and is CASE-PRESERVING: `<Windows.h>`
    // becomes `Windows.json`, and it is `matching` -- not the host -- that then
    // decides whether that name reaches `windows.json`.
    fs::path const requested{filename};
    fs::path const relStem = requested.parent_path() / requested.stem();
    // ⚠ `core::genericSpelling` AND NOT `relStem.generic_string()`
    // ([[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]). ✔MEASURED in a live
    // trace of the quote->angle fallback: `generic_string()` collapses the
    // leading separator RUN, so the rewritten name for a rooted request came out
    // as `/server/share/x.json` -- one separator -- and the search that followed
    // was aimed at the LOCAL DRIVE ROOT. It missed here, but the shape is a
    // silent WRONG ACCEPT: a same-named descriptor happening to sit under
    // `C:\server\share\` would have been spliced for a request that named
    // another machine. The rewrite must stay pure byte slicing (the header says
    // so) -- it just must not slice the root off.
    std::string const descriptorName = core::genericSpelling(relStem) + ".json";
    return findInDirs(descriptorName, systemDirs, matching);
}

AngleIncludeResolution resolveAngleInclude(std::string_view          filename,
                                           std::span<fs::path const> systemDirs,
                                           std::span<fs::path const> includeDirs,
                                           HeaderNameMatching        matching) {
    // 1. Descriptor FIRST — the DSS neutral `<stem>.json` model. Existence of the
    //    descriptor FILE is the gate here; per-format availability is the caller's
    //    verdict (so an existing-but-unavailable descriptor still returns
    //    Descriptor and does NOT fall through to a source header).
    HeaderSearchResult desc = resolveSystemDescriptor(filename, systemDirs, matching);
    switch (desc.status) {
        case HeaderSearchStatus::Found:
            return {AngleIncludeKind::Descriptor, std::move(desc.path), {}};
        case HeaderSearchStatus::AmbiguousCase:
            // The import resolver re-resolves the descriptor half and owns its
            // loud report; keeping this arm DISTINCT is what lets the caller
            // stay silent here without the collision going unreported.
            return {AngleIncludeKind::AmbiguousDescriptor, {},
                    std::move(desc.ambiguousCandidates)};
        case HeaderSearchStatus::NotFound:
            break;
    }
    // 2. Source fallback — a REAL header on the -I includeDirs. The angle form does
    //    NOT search the including file's own directory (C 6.10.2p2), so this is
    //    `includeDirs` ONLY, never a self-dir prepend (that distinction is what the
    //    quote form's `resolveIncludePath` adds; angle omits it by construction).
    HeaderSearchResult src = findInDirs(filename, includeDirs, matching);
    switch (src.status) {
        case HeaderSearchStatus::Found:
            return {AngleIncludeKind::Source, std::move(src.path), {}};
        case HeaderSearchStatus::AmbiguousCase:
            // NOTHING downstream re-resolves the `-I` source half — the import
            // resolver's angle arm calls `resolveSystemDescriptor` alone — so
            // the caller of THIS function is the only tier that can report it.
            return {AngleIncludeKind::AmbiguousSource, {},
                    std::move(src.ambiguousCandidates)};
        case HeaderSearchStatus::NotFound:
            break;
    }
    // 3. Total miss — the caller fails loud (F_ShippedHeaderNotFound).
    return {AngleIncludeKind::NotFound, {}, {}};
}

} // namespace dss
