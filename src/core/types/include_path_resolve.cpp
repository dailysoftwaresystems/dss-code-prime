#include "core/types/include_path_resolve.hpp"

#include "core/substrate/path_identity.hpp"  // genericSpelling — the ONE lossless
                                             // separator normalisation
#include "core/types/ascii_case.hpp"   // asciiToLower — the ONE folding helper

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

namespace dss {

namespace fs = std::filesystem;

// The FILESYSTEM's own character type: `char` on POSIX, `wchar_t` on Windows.
// Nothing below branches on which — every name comparison in this file happens
// in this type, and the two helpers underneath are written so that the same
// source line is correct for both (see `kDotChar`).
using NativeChar   = fs::path::value_type;
using NativeString = fs::path::string_type;

// ── [[D-PP-INCLUDE-RESOLVER-RELISTS-EVERY-DIRECTORY-PER-RESOLUTION]] ────────
//
// The compile's view of the tree (see `HeaderSearchCache` in the header). One
// mutex: a compile's searches run on one thread today, so it is uncontended, and
// it is what keeps "listed ONCE" true if that ever stops being so.
struct HeaderSearchCache::Impl {
    struct Listing {
        // Every entry name the listing yielded, in enumeration order — exactly the
        // names the per-resolution loops used to compare, a listing that failed to
        // open yielding none and one that failed part-way keeping what it had read.
        std::vector<NativeString>                                      names;
        // The same names, for the byte-exact question (CaseSensitive, root prefix)…
        std::unordered_set<NativeString>                               exact;
        // …and grouped by their ASCII fold, for the CaseInsensitive one.
        std::unordered_map<NativeString, std::vector<std::size_t>>     folded;
        // Why the listing failed, when `dir` IS a directory (or what it reaches
        // cannot be examined) and still could not be listed whole: a listing the
        // case question cannot be answered from. Clear when the listing was read
        // whole, and when `dir` is absent or no directory -- nothing to search.
        std::error_code                                                error;
    };
    // What one candidate REACHES, asked once: an entry, nothing, or -- `error` set --
    // a reach that cannot be examined.
    struct Probe {
        core::EntryKind kind = core::EntryKind::Absent;   // `Absent` too when `error` is set
        std::error_code error;
    };
    // The verdict on a name in a directory that REFUSED ITS LISTING (`unlistedMatch`), by policy and candidate: what
    // it took to learn it is asked once per compile, like a listing.
    struct Unlisted {
        HeaderSearchResult result;
        core::EntryKind    reached = core::EntryKind::Absent;   // what a Found entry reaches
    };
    std::mutex                                  mutex;
    std::unordered_map<NativeString, Listing>   listings;   // by directory
    std::unordered_map<NativeString, Probe>     probes;     // by candidate
    std::unordered_map<NativeString, Unlisted>  unlisted;   // by policy and candidate
};

struct HeaderSearchTally::Impl {
    std::mutex                          mutex;
    std::map<NativeString, std::size_t> listings;
    std::map<NativeString, std::size_t> probes;
};

namespace {

// ── the tally's install point ───────────────────────────────────────────────
// Recording holds `tallyGate()` so a record can never meet a tally being torn
// down; the fast path — no tally — is the one relaxed load below.
[[nodiscard]] std::mutex& tallyGate() {
    static std::mutex m;
    return m;
}
[[nodiscard]] std::atomic<HeaderSearchTally::Impl*>& liveTally() {
    static std::atomic<HeaderSearchTally::Impl*> t{nullptr};
    return t;
}
void tallyRecord(NativeString const& key, bool listing) {
    if (liveTally().load(std::memory_order_relaxed) == nullptr) return;
    std::lock_guard<std::mutex> gate{tallyGate()};
    HeaderSearchTally::Impl* const t = liveTally().load(std::memory_order_relaxed);
    if (t == nullptr) return;
    std::lock_guard<std::mutex> lock{t->mutex};
    ++(listing ? t->listings : t->probes)[key];
}

// The key a directory or candidate is cached under: the spelling as searched,
// with the host's preferred separator. `//server/share/x` and `\\server\share\x`
// name one directory on Windows and read identically; on POSIX `make_preferred`
// is the identity, so nothing is merged there that is not already one string.
// ⚠ NOTHING ELSE IS NORMALIZED, on purpose: folding case would merge directories
// a case-sensitive filesystem keeps apart, and resolving `..` lexically is wrong
// through a symlink. Two spellings of one directory then cost two listings,
// which is the price of never answering for the wrong directory.
[[nodiscard]] NativeString cacheKeyOf(fs::path const& p) {
    fs::path k = p;
    k.make_preferred();
    return k.native();
}

// The listing of `dir` in `c`, read on FIRST use — the caller holds `c.mutex`.
[[nodiscard]] HeaderSearchCache::Impl::Listing const&
listingOf(HeaderSearchCache::Impl& c, fs::path const& dir) {
    NativeString key = cacheKeyOf(dir);
    if (auto const hit = c.listings.find(key); hit != c.listings.end()) return hit->second;
    HeaderSearchCache::Impl::Listing l;
    // THE SUBSTRATE'S LISTING (P69 H6): it lists at any length, and one it could not read whole hands out no name --
    // the library's `directory_iterator` miscounts a directory of 260 characters or more under MinGW's libstdc++ with
    // no error (`core::extendedLengthSpelling`'s header). A name is all a listing answers here, so a link whose reach
    // cannot be examined is listed by its name like any other.
    core::TreeWalkOptions options;
    options.handOutUnreadableLinks = true;
    core::TreeWalk const walk = core::walkTree(
        dir,
        [&l](core::TreeEntry const& entry) {
            l.names.push_back(entry.path.filename().native());
            return core::WalkStep::Prune;
        },
        options);
    std::error_code const ec = walk.error;
    // A failed listing is ABSENCE only when there is no directory there to list (the
    // substrate's answer: nothing, or no directory). A directory -- or a `dir` whose
    // reach cannot be examined -- that could not be listed whole is a listing the
    // search cannot answer from, and it says so (`matchComponent`).
    if (ec) {
        std::error_code       kec;
        core::EntryKind const there = core::reachedKind(dir, kec);
        if (kec) l.error = kec;
        else if (there == core::EntryKind::Directory) l.error = ec;
    }
    l.exact.reserve(l.names.size());
    for (std::size_t i = 0; i < l.names.size(); ++i) {
        l.exact.insert(l.names[i]);
        l.folded[asciiToLower(l.names[i])].push_back(i);
    }
    tallyRecord(key, /*listing=*/true);
    return c.listings.emplace(std::move(key), std::move(l)).first->second;
}

// Does `dir` LIST an entry whose name is `name`, byte for byte? `listingError` is
// set when the listing could not be read whole (and the answer then means nothing).
[[nodiscard]] bool listsExactly(HeaderSearchCache& cache, fs::path const& dir,
                                NativeString const& name, std::error_code& listingError) {
    HeaderSearchCache::Impl& c = cache.impl();
    std::lock_guard<std::mutex> lock{c.mutex};
    auto const& l = listingOf(c, dir);
    listingError  = l.error;
    return l.exact.contains(name);
}

// Every entry of `dir` whose ASCII fold is `folded`, in enumeration order.
// `listingError` is set when the listing could not be read whole.
[[nodiscard]] std::vector<NativeString> foldMatches(HeaderSearchCache& cache, fs::path const& dir,
                                                    NativeString const& folded, std::error_code& listingError) {
    HeaderSearchCache::Impl& c = cache.impl();
    std::lock_guard<std::mutex> lock{c.mutex};
    auto const& l = listingOf(c, dir);
    listingError  = l.error;
    std::vector<NativeString> out;
    if (auto const hit = l.folded.find(folded); hit != l.folded.end()) {
        out.reserve(hit->second.size());
        for (std::size_t const i : hit->second) out.push_back(l.names[i]);
    }
    return out;
}

// What does `candidate` REACH — every link on the way followed — asked ONCE
// per candidate per compile? `error` is set when what it reaches cannot be
// EXAMINED (a looping link, a refusal), and the answer is then no answer
// (`Absent` with the reason): never "nothing" (see
// `HeaderSearchStatus::Unexaminable`). `matchComponent` asks it of EVERY
// candidate, under both policies, and `descend` of a name whose tail no
// candidate named; `descend` decides from that one answer what the search may
// take (no directory), so no caller asks the path again.
//
// ★★ A DANGLING LINK IS NO HEADER, UNDER EITHER POLICY — THE ONE STATEMENT OF IT
// ([[D-PP-HEADER-CASE-DANGLING-SYMLINK-ASYMMETRY]]). ✔MEASURED 2026-10-20: a
// dangling link `x.h` in the first search directory and a real `x.h` in a later
// one — gcc 13.3.0, clang 18.1.3, Apple clang 21.0.0, MinGW-w64 gcc 13.2.0 and
// cl 19.51 each, for `#include "x.h"` and `#include <x.h>` alike, went ON to the
// later directory. The case-sensitive arm used to ask `fs::exists` (a dangling
// link: absent) while the case-insensitive arm took whatever its listing held (a
// dangling link: found) — one tree, two verdicts decided by the TARGET's case
// rule. Both arms now ask this probe. And it asks the substrate, never
// `fs::exists`: libstdc++ on Windows reads a dangling link's OWN attributes and
// calls it `regular` (✔MEASURED 2026-10-20, path_identity.hpp), so the MinGW
// build found the link under BOTH policies.
[[nodiscard]] core::EntryKind reachedOnce(HeaderSearchCache& cache, fs::path const& candidate,
                                          std::error_code& error) {
    HeaderSearchCache::Impl& c = cache.impl();
    std::lock_guard<std::mutex> lock{c.mutex};
    NativeString key = cacheKeyOf(candidate);
    if (auto const hit = c.probes.find(key); hit != c.probes.end()) {
        error = hit->second.error;
        return hit->second.kind;
    }
    HeaderSearchCache::Impl::Probe probe;
    probe.kind = core::reachedKind(candidate, probe.error);
    if (probe.error) probe.kind = core::EntryKind::Absent;
    tallyRecord(key, /*listing=*/false);
    error = probe.error;
    c.probes.emplace(std::move(key), probe);
    return probe.kind;
}

// The other ASCII case of every letter in `name`: the one spelling whose answer tells, where a directory's listing
// cannot be read, how the directory compares case for this name (`decideUnlisted`). One body for every character
// type, as the fold is (`ascii_case.hpp`): `A`-`Z` and `a`-`z` are the same code points in each.
[[nodiscard]] NativeString otherAsciiCase(NativeString const& name) {
    NativeString out = name;
    for (NativeChar& c : out) {
        if (c >= NativeChar{'a'} && c <= NativeChar{'z'}) {
            c = static_cast<NativeChar>(c - (NativeChar{'a'} - NativeChar{'A'}));
        } else if (c >= NativeChar{'A'} && c <= NativeChar{'Z'}) {
            c = static_cast<NativeChar>(c + (NativeChar{'a'} - NativeChar{'A'}));
        }
    }
    return out;
}

// ── A DIRECTORY THAT REFUSES ITS LISTING IS SEARCHED BY NAME, AND THE CASE IS VERIFIED BY NAME ──────────────────
//
// ✔MEASURED 2026-10-28 (probe-reference-cc, linux-x86_64-debug, run 20261029-000115-ff8a36d9, gcc 13.3.0 and clang
// 18.1.3, euid 1000): with `u` searched first holding x.h at mode 0300 (search without read) and a `b/x.h` later, both
// take u/x.h by name. The construct works, so the search takes it too; the case question the listing would have
// answered is asked of the entries instead (the substrate measures what each host tells, path_identity.hpp):
//   * nothing answers to the exact name: the case-sensitive policy has its answer, NotFound -- the host's lookup is
//     never stricter than the policy; the case-insensitive one asks the directory's own case rule -- a folding
//     directory holds no other spelling (NotFound), any other may hold a fold-match only the listing could show;
//   * the name has no letter: it has one spelling, and the lookup is the answer;
//   * the other-case spelling is ANOTHER entry, or none: the directory keeps case, and the lookup of the exact name IS
//     the exact-case answer -- the case-sensitive policy takes it; for the case-insensitive one another entry is a
//     second fold-match (AmbiguousCase), and none leaves a mixed-case one only the listing could rule out;
//   * the other-case spelling is the SAME entry: the directory folds -- the case-sensitive policy takes the entry only
//     when its STORED name is the requested spelling byte for byte, else NotFound; the case-insensitive one takes the
//     one fold-match a folding directory can hold, spelled as it is stored.
// A question this host cannot answer -- the stored name in a folding directory, a directory's own case rule (Linux
// tells neither) -- ends the search there, `Unexaminable`, naming the directory with the reason its listing was
// refused: never a guess, and never "not here". The checkpoint-34 bytes read such a listing as holding NOTHING and
// went on: `-I u -I b` compiled b/x.h, the header of the wrong directory, in silence.
[[nodiscard]] HeaderSearchCache::Impl::Unlisted
decideUnlisted(fs::path const& dir, fs::path const& name, HeaderNameMatching matching, HeaderSearchCache& cache,
               std::error_code const& listingError) {
    bool const            caseSensitive = matching == HeaderNameMatching::CaseSensitive;
    fs::path const        candidate     = dir / name;
    std::error_code       ec;
    core::EntryKind const reaches = reachedOnce(cache, candidate, ec);
    if (ec) return {HeaderSearchResult::unexaminable(candidate, ec)};
    NativeString const other   = otherAsciiCase(name.native());
    bool const         letters = other != name.native();
    if (reaches == core::EntryKind::Absent) {
        if (caseSensitive || !letters) return {HeaderSearchResult::notFound()};
        std::optional<bool> const folds = core::directoryFoldsCase(dir, ec);
        if (ec) return {HeaderSearchResult::unexaminable(dir, ec)};
        if (folds.value_or(false)) return {HeaderSearchResult::notFound()};
        return {HeaderSearchResult::unexaminable(dir, listingError)};
    }
    if (!letters) return {HeaderSearchResult::found(candidate), reaches};
    fs::path const otherSpelling = dir / fs::path{other};
    bool const     folds         = core::sameEntry(candidate, otherSpelling, ec);
    if (ec) return {HeaderSearchResult::unexaminable(otherSpelling, ec)};
    if (!folds) {
        if (caseSensitive) return {HeaderSearchResult::found(candidate), reaches};
        core::EntryKind const second = reachedOnce(cache, otherSpelling, ec);
        if (ec) return {HeaderSearchResult::unexaminable(otherSpelling, ec)};
        if (second == core::EntryKind::Absent) return {HeaderSearchResult::unexaminable(dir, listingError)};
        std::vector<fs::path> both{candidate, otherSpelling};
        std::sort(both.begin(), both.end(),
                  [](fs::path const& a, fs::path const& b) { return a.native() < b.native(); });
        return {HeaderSearchResult::ambiguous(std::move(both))};
    }
    std::optional<NativeString> const stored = core::storedEntryName(candidate, ec);
    if (ec) return {HeaderSearchResult::unexaminable(candidate, ec)};
    if (!stored) return {HeaderSearchResult::unexaminable(dir, listingError)};
    if (caseSensitive) {
        if (*stored != name.native()) return {HeaderSearchResult::notFound()};
        return {HeaderSearchResult::found(candidate), reaches};
    }
    if (asciiToLower(*stored) != asciiToLower(name.native())) return {HeaderSearchResult::notFound()};
    return {HeaderSearchResult::found(dir / fs::path{*stored}), reaches};
}

// `decideUnlisted`, asked once per policy and candidate per compile. `reached` is what a Found entry reaches.
[[nodiscard]] HeaderSearchResult unlistedMatch(fs::path const& dir, fs::path const& name, HeaderNameMatching matching,
                                               HeaderSearchCache& cache, std::error_code const& listingError,
                                               core::EntryKind& reached) {
    HeaderSearchCache::Impl& c   = cache.impl();
    NativeString             key = cacheKeyOf(dir / name);
    key.insert(key.begin(), matching == HeaderNameMatching::CaseSensitive ? NativeChar{'S'} : NativeChar{'I'});
    {
        std::lock_guard<std::mutex> lock{c.mutex};
        if (auto const hit = c.unlisted.find(key); hit != c.unlisted.end()) {
            reached = hit->second.reached;
            return hit->second.result;
        }
    }
    HeaderSearchCache::Impl::Unlisted verdict = decideUnlisted(dir, name, matching, cache, listingError);
    reached                                   = verdict.reached;
    std::lock_guard<std::mutex> lock{c.mutex};
    return c.unlisted.emplace(std::move(key), std::move(verdict)).first->second.result;
}

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
//
// ★★ BOTH ARMS READ `dir` THROUGH THE COMPILE'S CACHE (`listsExactly`,
// `foldMatches`, `reachedOnce` above): the listing and the `exists` probe happen
// once per compile, and every later lookup compares against what they returned.
// The comparisons themselves are unchanged, name for name.
// `reached` is what a Found entry REACHES, from the search's own examination of it -- for `descend`, which decides
// from it what the search may take.
HeaderSearchResult matchComponent(fs::path const& dir, fs::path const& name,
                                  HeaderNameMatching matching,
                                  HeaderSearchCache& cache, core::EntryKind& reached) {
    reached = core::EntryKind::Absent;
    if (matching == HeaderNameMatching::CaseSensitive) {
        // Cheap DEFINITIVE reject first. If no entry answers to these exact
        // bytes even under the host's own (possibly folding) rules, then no
        // byte-exact entry can exist either — the probe is only ever MORE
        // permissive than we are, never less, so a false here is trustworthy.
        // It is also where a dangling link is no header (`reachedOnce`).
        fs::path const candidate = dir / name;
        std::error_code       probeError;
        core::EntryKind const reaches = reachedOnce(cache, candidate, probeError);
        if (probeError) return HeaderSearchResult::unexaminable(candidate, probeError);
        if (reaches == core::EntryKind::Absent) return HeaderSearchResult::notFound();
        // The probe said yes — but a case-INSENSITIVE host may have FOLDED to
        // get there. Verify the REAL on-disk spelling ourselves; this is the
        // arm that stops `#include <Stdio.h>` from silently compiling for an
        // elf target on a Windows/macOS host. A directory that refuses its
        // LISTING is verified by NAME instead (`unlistedMatch`): unable to list
        // is no permission to assume, and no absence either.
        std::error_code listingError;
        bool const      spelled = listsExactly(cache, dir, name.native(), listingError);
        if (listingError) return unlistedMatch(dir, name, matching, cache, listingError, reached);
        if (!spelled) return HeaderSearchResult::notFound();
        reached = reaches;
        return HeaderSearchResult::found(candidate);
    }

    // CaseInsensitive. ALWAYS enumerate — there is no correct `exists()` fast
    // path. On a case-sensitive host BOTH `foo.json` and `Foo.json` can exist;
    // accepting the byte-exact one would hand the answer back to the host,
    // because a case-insensitive host cannot even hold that pair. Every
    // fold-match is collected and >= 2 is a LOUD failure, never a pick.
    // A hit is spelled `dir / <entry>` — the path the listing's iterator yields.
    // A listed entry that REACHES nothing — a dangling link — is no hit: the same
    // probe, and so the same rule, as the case-sensitive arm above (`reachedOnce`).
    // A listing that could not be read whole is verified by NAME (`unlistedMatch`);
    // a fold-match whose reach cannot be examined ENDS the search (`Unexaminable`,
    // named): it is no absence.
    std::vector<fs::path> hits;
    core::EntryKind       hitReaches = core::EntryKind::Absent;   // what the hit reaches, when there is one
    std::error_code       listingError;
    auto const            matches = foldMatches(cache, dir, asciiToLower(name.native()), listingError);
    if (listingError) return unlistedMatch(dir, name, matching, cache, listingError, reached);
    for (NativeString const& entry : matches) {
        fs::path              hit = dir / fs::path{entry};
        std::error_code       probeError;
        core::EntryKind const reaches = reachedOnce(cache, hit, probeError);
        if (probeError) return HeaderSearchResult::unexaminable(std::move(hit), probeError);
        if (reaches == core::EntryKind::Absent) continue;
        hitReaches = reaches;
        hits.push_back(std::move(hit));
    }
    if (hits.empty()) return HeaderSearchResult::notFound();
    if (hits.size() == 1) {
        reached = hitReaches;
        return HeaderSearchResult::found(std::move(hits.front()));
    }
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
//
// ★★ WHAT A SEARCH MAY TAKE IS DECIDED HERE, ONCE, FOR EVERY CALLER: whatever the name REACHES except a DIRECTORY.
// A directory named like the header is no header, and the search goes on past it -- ✔MEASURED 2026-10-28
// (probe-reference-cc, linux-x86_64-debug, run 20261029-000351-99219a41, gcc 13.3.0 and clang 18.1.3): a directory
// `u/x.h` searched first and a header `b/x.h` later -- both take b/x.h, `#include <x.h>`, `#include "x.h"` on -I and
// in the includer's own directory alike; the directory alone -- both say not found. A DEVICE or a NAMED PIPE is
// taken: ✔MEASURED (run 20261029-000903-6a85024c, the same two; run 20261029-002044-5101633a, Apple clang 21.0.0)
// `#include "/dev/null"` compiles in all three and `__has_include` of it is 1; a pipe fed by a writer is read by both
// clangs, and clang takes such a pipe named x.h before a later x.h (gcc opens it too, and waits on it). So no caller
// filters a Found -- the quote pre-scan, the angle search, the import resolver and `#embed` read one answer. The bytes
// before this handed a directory back as Found: the quote pre-scan passed over it with a second look of its own, and
// the angle form THREW out of the preprocessor (`SourceBuffer::fromFile`: "is not a regular file", ✔MEASURED on the
// Mac, run 20261029-002544-d3909fed).
// Whether `descend` takes `.`, `..` and an empty component as STEPS (every spelling Win32 normalizes, and POSIX), or
// as NAMES -- below a verbatim `\\?\` root, whose rest Win32 opens literally (`core::rootSpelling`'s header measures
// it): there a `..` is a name no listing holds, never a step up, and an empty component a name nothing has.
enum class Components : bool { Navigate, Literal };

template <class Range>
HeaderSearchResult descendThrough(fs::path base, Range const& rel, Components components,
                                  HeaderNameMatching matching, HeaderSearchCache& cache) {
    // What `base` reaches, as the search examined it: known once a component is matched (its probe's
    // answer), unknown at the start and after a `..` -- a path no probe named.
    std::optional<core::EntryKind> reaches;
    for (fs::path const& comp : rel) {
        // NATIVE, not `comp.string()`: the requested name is already a path,
        // and narrowing it back would reintroduce the throw/lossy conversion
        // documented on `matchComponent` — on the REQUEST side this time.
        // `rel` was built from the source text ONCE, by the caller.
        NativeString const& name = comp.native();
        if (components == Components::Literal && name.empty()) return HeaderSearchResult::notFound();
        // A `.`/empty component (from a trailing separator, or `a/./b`) moves
        // nowhere and has no on-disk name to match.
        if (components == Components::Navigate && (name.empty() || isDotComponent(name))) continue;
        // `..` is a NAVIGATION component, not a filename: no directory listing
        // ever contains an entry called `..`, so running it through the
        // name-matcher below would turn every `#include "../shared.h"` into a
        // miss. Append it and let the OS resolve it exactly as the pre-policy
        // `dir / rel` did — the case rule still applies to every component that
        // actually names a file.
        if (components == Components::Navigate && isDotDotComponent(name)) {
            base /= comp;
            reaches.reset();
            continue;
        }
        core::EntryKind    reached = core::EntryKind::Absent;
        HeaderSearchResult step    = matchComponent(base, comp, matching, cache, reached);
        if (step.status != HeaderSearchStatus::Found) return step;
        base    = std::move(step.path);
        reaches = reached;
    }
    // A name that ends where no probe looked -- in `..`, or in nothing but `.`s -- is examined ONCE
    // here, through the compile's cache. A reach that cannot be examined ends the search, named, like any
    // candidate's: `d/sub/..` through a `sub` with no search permission is no answer (gcc and clang open it
    // and fail with the OS's reason). The checkpoint-34 bytes handed such a path back as Found, unexamined.
    if (!reaches) {
        std::error_code error;
        reaches = reachedOnce(cache, base, error);
        if (error) return HeaderSearchResult::unexaminable(base, error);
        // Nothing reached -- a `..` through a FILE (`x.h/..`) -- is no header, and the search goes on.
        if (*reaches == core::EntryKind::Absent) return HeaderSearchResult::notFound();
    }
    // A matched component always reaches something (`matchComponent`); a directory is no header.
    if (*reaches == core::EntryKind::Directory) return HeaderSearchResult::notFound();
    return HeaderSearchResult::found(std::move(base));
}

HeaderSearchResult descend(fs::path base, fs::path const& rel,
                           HeaderNameMatching matching, HeaderSearchCache& cache) {
    return descendThrough(std::move(base), rel, Components::Navigate, matching, cache);
}

// ── [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]] — THE BASE A ROOTED NAME IS SEARCHED FROM ──────────────────
//
// ★★★ IT IS THE VOLUME ROOT `core::rootSpelling` PARSES (P69 H6), and nothing else decides it. The base and the
// components below it come from ONE split of the native spelling: `C:\` and `inc\x.h`, `\\server\share\` and
// `inc\x.h`, `\\.\C:\`, `\\?\Volume{G}\`, `\\?\UNC\server\share\`, `/` -- and every component below the volume root
// is case-checked by `descend`, the first one included.
//
// ★★ WHAT IT REPLACED, AND WHY BOTH OF ITS RULES WERE WRONG. `descend` was first handed the library's `root_path()`,
// which is not lossless (`path("//server/share/x.h").root_path()` is `/`, ONE slash, on the MinGW model: the authority
// demoted to a component, the walk asking the local drive root for a directory named after the SERVER). Its
// successor took as the root the leading run of ancestors no directory listing CONTAINS -- asking the filesystem
// where the root ends. It was wrong twice over (the P69 review of H5, F6 and F3; the first pinned by
// `RootedIncludeResolution.AMisCasedFirstComponentIsCaseChecked`): a MIS-CASED FIRST component (`C:\SOURCE\...` for
// `C:\Source\...`) is not listed byte-exactly in its parent, so it joined the "root" and was never case-checked -- the
// wrong-case absolute include the CaseSensitive policy exists to refuse was accepted; and the library's own root of a
// device spelling ends inside its volume (MS STL's `root_name()` of `\\.\C:\x` is `\\.`, ✔MEASURED in
// `core::rootSpelling`'s header), so the run started from a prefix that is no directory. A root is a property of the
// SPELLING, decided once; the filesystem is asked about the components below it.
// (The history: that listing walk's first cut read freed memory -- a reference bound to the `native()` of a temporary
// `fs::path` -- and failed toward a spurious "listed", the whole of an intermittent UNC miss; ✔MEASURED and fixed then.)
//
// Below a VERBATIM root the components are split at `\` alone and taken as NAMES (`Components::Literal`): Win32 opens
// that rest literally, so `\\?\C:\a\..\x.h` names no file (ERROR_INVALID_NAME) and the search finds none, as the OS
// would, where a `..` taken as a step would have found `C:\x.h`.
HeaderSearchResult resolveMaybeAbsolute(fs::path const& rel, fs::path const& dir,
                                        HeaderNameMatching matching,
                                        HeaderSearchCache& cache) {
    if (!isRootedPath(rel)) return descend(dir, rel, matching, cache);
    NativeString const&      spelled = rel.native();
    core::RootSpelling const root    = core::rootSpelling(rel);
    fs::path const           base{spelled.substr(0, root.volume)};
    if (root.kind != core::RootSpelling::Kind::Verbatim)
        return descend(base, fs::path{spelled.substr(root.volume)}, matching, cache);
    std::vector<fs::path> names;
    for (std::size_t at = root.volume; at <= spelled.size();) {
        std::size_t const end = std::min(spelled.find(NativeChar{'\\'}, at), spelled.size());
        names.emplace_back(spelled.substr(at, end - at));
        at = end + 1;
    }
    return descendThrough(base, names, Components::Literal, matching, cache);
}

// Sorted (key, count) pairs out of a tally map, keys back as paths.
[[nodiscard]] std::vector<std::pair<fs::path, std::size_t>>
countsOf(std::map<NativeString, std::size_t> const& m) {
    std::vector<std::pair<fs::path, std::size_t>> out;
    out.reserve(m.size());
    for (auto const& [key, n] : m) out.emplace_back(fs::path{key}, n);
    return out;
}

} // namespace

// ── HeaderSearchCache / HeaderSearchTally ───────────────────────────────────
HeaderSearchCache::HeaderSearchCache() : impl_(std::make_unique<Impl>()) {}
HeaderSearchCache::~HeaderSearchCache() = default;

std::size_t HeaderSearchCache::listings() const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    return impl_->listings.size();
}
std::size_t HeaderSearchCache::probes() const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    return impl_->probes.size();
}

HeaderSearchTally::HeaderSearchTally() : impl_(std::make_unique<Impl>()) {
    std::lock_guard<std::mutex> gate{tallyGate()};
    Impl* expected = nullptr;
    if (!liveTally().compare_exchange_strong(expected, impl_.get()))
        throw std::logic_error("HeaderSearchTally: another tally is already alive — "
                               "the counts would be split between two observers");
}
HeaderSearchTally::~HeaderSearchTally() {
    std::lock_guard<std::mutex> gate{tallyGate()};
    liveTally().store(nullptr);
}

std::vector<std::pair<fs::path, std::size_t>> HeaderSearchTally::listingsPerDirectory() const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    return countsOf(impl_->listings);
}
std::vector<std::pair<fs::path, std::size_t>> HeaderSearchTally::probesPerCandidate() const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    return countsOf(impl_->probes);
}

// [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]] -- see the header for the
// measurement and for why this is exported rather than repeated per tier.
bool isRootedPath(fs::path const& p) {
    core::RootSpelling::Kind const kind = core::rootSpelling(p).kind;
    return kind != core::RootSpelling::Kind::Relative && kind != core::RootSpelling::Kind::DriveRelative;
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
                                HeaderNameMatching matching, HeaderSearchCache& cache) {
    return resolveMaybeAbsolute(fs::path{relName}, dir, matching, cache);
}

HeaderSearchResult findInDirs(std::string_view              filename,
                              std::span<fs::path const>     dirs,
                              HeaderNameMatching            matching,
                              HeaderSearchCache&            cache) {
    fs::path const rel{filename};
    // [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]: `isRootedPath`, never a
    // bare `is_absolute()` -- see that predicate for the measurement.
    if (isRootedPath(rel)) return resolveMaybeAbsolute(rel, {}, matching, cache);
    for (fs::path const& dir : dirs) {
        HeaderSearchResult r = descend(dir, rel, matching, cache);
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
                                      HeaderNameMatching            matching,
                                      HeaderSearchCache&            cache) {
    fs::path const rel{filename};
    // [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]: a UNC include answers
    // FALSE to `is_absolute()` on this build and was therefore searched against
    // the include dirs as though it were a relative name.
    if (isRootedPath(rel)) return resolveMaybeAbsolute(rel, {}, matching, cache);
    if (!includingDir.empty()) {
        HeaderSearchResult r = descend(includingDir, rel, matching, cache);
        if (r.status != HeaderSearchStatus::NotFound) return r;
    }
    return findInDirs(filename, includeDirs, matching, cache);
}

HeaderSearchResult resolveSystemDescriptor(std::string_view          filename,
                                           std::span<fs::path const> systemDirs,
                                           HeaderNameMatching        matching,
                                           HeaderSearchCache&        cache) {
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
    return findInDirs(descriptorName, systemDirs, matching, cache);
}

AngleIncludeResolution resolveAngleInclude(std::string_view          filename,
                                           std::span<fs::path const> systemDirs,
                                           std::span<fs::path const> includeDirs,
                                           HeaderNameMatching        matching,
                                           HeaderSearchCache&        cache) {
    // 1. Descriptor FIRST — the DSS neutral `<stem>.json` model. Existence of the
    //    descriptor FILE is the gate here; per-format availability is the caller's
    //    verdict (so an existing-but-unavailable descriptor still returns
    //    Descriptor and does NOT fall through to a source header).
    HeaderSearchResult desc =
        resolveSystemDescriptor(filename, systemDirs, matching, cache);
    switch (desc.status) {
        case HeaderSearchStatus::Found:
            return {AngleIncludeKind::Descriptor, std::move(desc.path), {}, {}};
        case HeaderSearchStatus::AmbiguousCase:
            // The import resolver re-resolves the descriptor half and owns its
            // loud report; keeping this arm DISTINCT is what lets the caller
            // stay silent here without the collision going unreported.
            return {AngleIncludeKind::AmbiguousDescriptor, {},
                    std::move(desc.ambiguousCandidates), {}};
        case HeaderSearchStatus::Unexaminable:
            // The search ENDS here: the source half is never asked. The import
            // resolver re-resolves the descriptor half and reports it.
            return {AngleIncludeKind::UnexaminableDescriptor, desc.path, {}, desc};
        case HeaderSearchStatus::NotFound:
            break;
    }
    // 2. Source fallback — a REAL header on the -I includeDirs. The angle form does
    //    NOT search the including file's own directory (C 6.10.2p2), so this is
    //    `includeDirs` ONLY, never a self-dir prepend (that distinction is what the
    //    quote form's `resolveIncludePath` adds; angle omits it by construction).
    HeaderSearchResult src = findInDirs(filename, includeDirs, matching, cache);
    switch (src.status) {
        case HeaderSearchStatus::Found:
            return {AngleIncludeKind::Source, std::move(src.path), {}, {}};
        case HeaderSearchStatus::AmbiguousCase:
            // NOTHING downstream re-resolves the `-I` source half — the import
            // resolver's angle arm calls `resolveSystemDescriptor` alone — so
            // the caller of THIS function is the only tier that can report it.
            return {AngleIncludeKind::AmbiguousSource, {},
                    std::move(src.ambiguousCandidates), {}};
        case HeaderSearchStatus::Unexaminable:
            // The same ownership as the collision above: the PP alone reports it.
            return {AngleIncludeKind::UnexaminableSource, src.path, {}, src};
        case HeaderSearchStatus::NotFound:
            break;
    }
    // 3. Total miss — the caller fails loud (F_ShippedHeaderNotFound).
    return {AngleIncludeKind::NotFound, {}, {}, {}};
}

std::string describeUnexaminable(HeaderSearchResult const& r) {
    return "'" + core::genericSpelling(r.path) + "' cannot be examined (" + r.error.message() + ")";
}

} // namespace dss
