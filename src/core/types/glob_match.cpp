#include "core/types/glob_match.hpp"

#include "core/substrate/path_identity.hpp"     // genericSpelling /
                                                // normalizeKeepingRoot
#include "core/types/include_path_resolve.hpp"  // isRootedPath — the ONE
                                                // rooted-path predicate

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <span>

namespace dss {

namespace fs = std::filesystem;

namespace {

// Split `s` on '/' into segments. A leading '/' yields a leading EMPTY segment
// (the POSIX-absolute marker); consecutive `//` yield empty segments. Empty
// segments only ever match an empty pattern segment — real path components never
// produce them — so they are inert for ordinary matching.
std::vector<std::string_view> splitSegments(std::string_view s) {
    std::vector<std::string_view> segs;
    std::size_t start = 0;
    while (true) {
        std::size_t const slash = s.find('/', start);
        if (slash == std::string_view::npos) {
            segs.push_back(s.substr(start));
            break;
        }
        segs.push_back(s.substr(start, slash - start));
        start = slash + 1;
    }
    return segs;
}

// If `pat[p] == '['` opens a WELL-FORMED character class (has a closing ']'),
// return the class length in chars (including both brackets); otherwise return 0
// (an unterminated '[' — the caller treats it as a literal '['). Honors a leading
// '!'/'^' negation and a ']' in the first member position (a literal ']').
std::size_t classLen(std::string_view pat, std::size_t p) {
    std::size_t i = p + 1;
    if (i < pat.size() && (pat[i] == '!' || pat[i] == '^')) ++i;
    if (i < pat.size() && pat[i] == ']') ++i;  // a ']' right here is a literal member
    while (i < pat.size() && pat[i] != ']') ++i;
    if (i >= pat.size()) return 0;             // unterminated ⇒ not a class
    return i - p + 1;                          // through the closing ']'
}

// Does character `c` belong to the class `cls` (== "[...]" including brackets)?
// Parses the inner content: negation, ranges (`a-z`), and literals.
bool classMatch(std::string_view cls, char c) {
    std::string_view const inner = cls.substr(1, cls.size() - 2);  // strip [ ]
    std::size_t i = 0;
    bool negate = false;
    if (!inner.empty() && (inner[0] == '!' || inner[0] == '^')) {
        negate = true;
        i = 1;
    }
    bool matched = false;
    while (i < inner.size()) {
        // A range `X-Y` needs a '-' with a member on each side; a '-' that is
        // last (or a lone trailing member) is a literal '-'.
        if (i + 2 < inner.size() && inner[i + 1] == '-') {
            if (inner[i] <= c && c <= inner[i + 2]) matched = true;
            i += 3;
        } else {
            if (inner[i] == c) matched = true;
            ++i;
        }
    }
    return matched != negate;
}

// Match a single path SEGMENT (no '/') against a single pattern SEGMENT. Handles
// '*' (>=0 chars, WITHIN the segment — it cannot cross '/' because a segment has
// none), '?' (exactly one), and '[...]' classes; every other char is a literal.
// Classic linear wildcard match with single-'*' backtracking.
bool segmentMatch(std::string_view pat, std::string_view s) {
    std::size_t p = 0, si = 0;
    bool haveStar = false;
    std::size_t starP = 0, starS = 0;
    while (si < s.size()) {
        if (p < pat.size() && pat[p] == '*') {
            haveStar = true;
            starP = p++;      // '*' matches zero chars so far
            starS = si;
            continue;
        }
        if (p < pat.size()) {
            char const pc = pat[p];
            std::size_t len = 1;
            bool matched = false;
            if (pc == '?') {
                matched = true;
            } else if (pc == '[') {
                std::size_t const cl = classLen(pat, p);
                if (cl == 0) {
                    matched = (s[si] == '[');  // unterminated '[' ⇒ literal
                } else {
                    matched = classMatch(pat.substr(p, cl), s[si]);
                    len = cl;
                }
            } else {
                matched = (pc == s[si]);
            }
            if (matched) {
                p += len;
                ++si;
                continue;
            }
        }
        // Mismatch (or pattern exhausted with text left): let the last '*'
        // absorb one more character.
        if (haveStar) {
            p = starP + 1;
            si = ++starS;
            continue;
        }
        return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;  // trailing '*'s match empty
    return p == pat.size();
}

// Match a slash-split PATTERN against a slash-split PATH at the SEGMENT level,
// with `**` as a segment-spanning star (matches zero or more whole segments).
// Classic single-'**' backtracking, mirroring `segmentMatch` one level up.
bool matchSegments(std::span<std::string_view const> pat,
                   std::span<std::string_view const> txt) {
    std::size_t pi = 0, ti = 0;
    bool haveStar = false;
    std::size_t starPi = 0, starTi = 0;
    while (ti < txt.size()) {
        if (pi < pat.size() && pat[pi] == "**") {
            haveStar = true;
            starPi = pi++;    // '**' matches zero segments so far
            starTi = ti;
        } else if (pi < pat.size() && segmentMatch(pat[pi], txt[ti])) {
            ++pi;
            ++ti;
        } else if (haveStar) {
            pi = starPi + 1;  // '**' absorbs one more segment
            ti = ++starTi;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == "**") ++pi;  // trailing '**' matches empty
    return pi == pat.size();
}

} // namespace

bool hasGlobMetacharacters(std::string_view pattern) noexcept {
    return pattern.find_first_of("*?[") != std::string_view::npos;
}

bool globMatch(std::string_view pattern, std::string_view path) {
    auto const patSegs = splitSegments(pattern);
    auto const txtSegs = splitSegments(path);
    return matchSegments(std::span<std::string_view const>{patSegs},
                         std::span<std::string_view const>{txtSegs});
}

bool expandGlob(std::string_view pattern,
                std::vector<std::string>& out,
                std::error_code& ec,
                fs::path const& baseDir) {
    ec.clear();

    // AP6 M4/B.3 — RE-BASE A RELATIVE PATTERN, AND ONLY A RELATIVE ONE. An
    // ABSOLUTE base already states where it starts, so `baseDir` must not touch
    // it (re-rooting would silently relocate a pattern that was already exact).
    // An EMPTY `baseDir` leaves the path untouched, which is what makes every
    // pre-AP6 call byte-identical: `fs::path{"src"}` is walked verbatim, i.e.
    // against the process cwd, exactly as before.
    //
    // ⚠ `isRootedPath`, NOT `is_absolute()`
    // ([[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]). ✔MEASURED: a pattern
    // whose root is a MULTI-SEPARATOR authority answers `is_absolute()` FALSE on
    // the toolchain that builds DSS. ⇒ the bare test classifies
    // `\\server\share\src\*.c` as RELATIVE and re-bases it -- exactly the silent
    // relocation the note above says must not happen. Reported as a
    // classification defect, which is what was measured; no glob over such a
    // pattern was run. The ONE exported predicate answers it correctly on every
    // path model.
    auto rebase = [&baseDir](fs::path p) {
        if (baseDir.empty() || isRootedPath(p)) return p;
        // `baseDir / "."` would leave a "." component that `lexically_relative`
        // below then has to reason about; the bare `baseDir` is the same
        // directory with no such component, so say it directly.
        return p == fs::path{"."} ? baseDir : (baseDir / p);
    };

    // Split the pattern at the LAST '/' at or before its first metacharacter: the
    // part up to that slash is the LITERAL base directory to walk; the remainder
    // is the glob tail matched against each file's path RELATIVE to base. This
    // scopes the walk to the pattern's literal prefix (`src/**/*.c` walks `src`,
    // not a sibling `build/`).
    std::size_t const metaPos = pattern.find_first_of("*?[");
    if (metaPos == std::string_view::npos) {
        // No metacharacter — the caller guards on `hasGlobMetacharacters`, so this
        // is defensive: treat the whole thing as a single literal path. It is
        // re-based like every other relative spelling, so this arm cannot become
        // the one place a `baseDir` build silently probes the wrong tree.
        std::error_code fec;
        auto const lit = rebase(fs::path{std::string{pattern}});
        if (fs::exists(lit, fec) && !fec) {
            // `core::genericSpelling` and NOT `lit.generic_string()`: what this
            // pushes is not a message, it is the SOURCE PATH the compiler will
            // open. `rebase` above is careful to leave a multi-separator
            // authority alone; rendering it here with the lossy spelling would
            // hand that care straight back.
            out.push_back(baseDir.empty() ? std::string{pattern}
                                          : core::genericSpelling(lit));
        }
        return true;
    }
    std::size_t const lastSlash = pattern.find_last_of('/', metaPos);
    std::string baseStr;
    std::string tail;
    if (lastSlash == std::string_view::npos) {
        baseStr = ".";                                    // cwd-relative pattern
        tail = std::string{pattern};
    } else {
        baseStr = std::string{pattern.substr(0, lastSlash)};
        if (baseStr.empty()) baseStr = "/";               // pattern rooted at '/'
        tail = std::string{pattern.substr(lastSlash + 1)};
    }
    fs::path const base = rebase(fs::path{baseStr});

    // A base that does not exist / is not a directory ⇒ ZERO matches (NOT an I/O
    // error): the caller owns the zero-match fail-loud. A genuine access error on
    // the probe (e.g. permission on a parent) IS an I/O error → fail loud.
    std::error_code probeEc;
    bool const baseExists = fs::exists(base, probeEc);
    if (probeEc) {
        ec = probeEc;
        return false;
    }
    if (!baseExists) return true;
    bool const baseIsDir = fs::is_directory(base, probeEc);
    if (probeEc) {
        ec = probeEc;
        return false;
    }
    if (!baseIsDir) return true;

    // `**` anywhere in the tail ⇒ unbounded match depth (recursive walk). Without
    // it the depth is fixed at the tail's segment count, so the walk is PRUNED at
    // that depth — which also scopes the fail-loud-on-unreadable-dir to the
    // directories the pattern could actually reach.
    bool const recursive = tail.find("**") != std::string::npos;
    std::size_t const tailSegs =
        1 + static_cast<std::size_t>(std::count(tail.begin(), tail.end(), '/'));

    std::vector<std::string> matched;
    fs::recursive_directory_iterator it(base, fs::directory_options::none, ec);
    if (ec) return false;  // could not open base for iteration ⇒ fail loud
    fs::recursive_directory_iterator const end;
    for (; it != end; it.increment(ec)) {
        if (ec) return false;  // mid-walk I/O error ⇒ fail loud
        std::error_code entEc;
        // Depth-prune for a non-`**` tail: never descend past the pattern depth.
        if (!recursive && it->is_directory(entEc)
            && static_cast<std::size_t>(it.depth()) + 1 >= tailSegs) {
            it.disable_recursion_pending();
        }
        // DELIBERATE asymmetry vs the fatal open / increment errors above: a
        // per-ENTRY stat failure (a broken symlink target, or an entry that
        // vanished / lost permission mid-walk) is SKIPPED, never raised — such an
        // entry is unrelated to the pattern and must not fail the whole glob (that
        // would break globbing on any tree holding an unrelated broken symlink).
        // Only losing visibility of a whole SUBTREE (a directory open / increment
        // error) is fatal.
        entEc.clear();
        if (!it->is_regular_file(entEc)) continue;
        // ⚠ THE TWO RENDERINGS BELOW ARE DELIBERATELY DIFFERENT, AND THE
        // ASYMMETRY IS THE MEASUREMENT. `lexically_relative` yields a path with
        // NO root — ✔MEASURED 2026-08-28 against a reachable UNC base:
        // `//localhost/C$/…/leaf.c` relative to `//localhost/C$/…` is `leaf.c`,
        // leading run 0 — so nothing there can lose an authority and the plain
        // spelling is exact. The MATCH, by contrast, is the absolute source path
        // handed on to be compiled, and it passes through BOTH lossy transforms:
        // measured on that same base, `lexically_normal()` alone already
        // returned `\localhost\C$\…\leaf.c`. A glob over a share would have
        // yielded a whole source list naming a local drive that has none of it.
        std::string const rel = it->path().lexically_relative(base).generic_string();
        if (globMatch(tail, rel)) {
            matched.push_back(
                core::genericSpelling(core::normalizeKeepingRoot(it->path())));
        }
    }

    // Deterministic order for reproducible CU ordering; dedup as cheap insurance
    // against a double-yielding iterator (observed on Windows symlink edges — see
    // InputResolver).
    std::sort(matched.begin(), matched.end());
    matched.erase(std::unique(matched.begin(), matched.end()), matched.end());
    out.insert(out.end(), matched.begin(), matched.end());
    return true;
}

} // namespace dss
