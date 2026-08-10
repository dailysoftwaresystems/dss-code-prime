#include "core/types/include_path_resolve.hpp"

#include "core/types/ascii_case.hpp"   // asciiToLower — the ONE folding helper

#include <algorithm>
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

// Resolve a name that may be absolute or relative-to-`dir`, applying the
// policy to every component the SOURCE wrote. Directory-list entries
// (`-I` dirs, systemDirs) are NOT case-checked: they come from the driver and
// the build config, not from a header name in the program text.
HeaderSearchResult resolveMaybeAbsolute(fs::path const& rel, fs::path const& dir,
                                        HeaderNameMatching matching) {
    if (rel.is_absolute()) {
        return descend(rel.root_path(), rel.relative_path(), matching);
    }
    return descend(dir, rel, matching);
}

} // namespace

HeaderSearchResult resolveInDir(fs::path const& dir, std::string_view relName,
                                HeaderNameMatching matching) {
    return resolveMaybeAbsolute(fs::path{relName}, dir, matching);
}

HeaderSearchResult findInDirs(std::string_view              filename,
                              std::span<fs::path const>     dirs,
                              HeaderNameMatching            matching) {
    fs::path const rel{filename};
    if (rel.is_absolute()) return resolveMaybeAbsolute(rel, {}, matching);
    for (fs::path const& dir : dirs) {
        HeaderSearchResult r = descend(dir, rel, matching);
        // An ambiguity anywhere ends the search: falling through to a later
        // dir would silently prefer whichever host could represent the tree.
        if (r.status != HeaderSearchStatus::NotFound) return r;
    }
    return HeaderSearchResult::notFound();
}

HeaderSearchResult resolveIncludePath(std::string_view              filename,
                                      fs::path const&               includingDir,
                                      std::span<fs::path const>     includeDirs,
                                      HeaderNameMatching            matching) {
    fs::path const rel{filename};
    if (rel.is_absolute()) return resolveMaybeAbsolute(rel, {}, matching);
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
    std::string const descriptorName = relStem.generic_string() + ".json";
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
