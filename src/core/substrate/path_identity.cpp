#include "core/substrate/path_identity.hpp"

#include "core/substrate/os_path_spelling.hpp"   // the OS's spelling of a plain Windows path: the rule's one home

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <winternl.h>   // NtOpenFile's argument types: the one relative open Win32 does not offer
#else
    #include <cerrno>
    #include <dirent.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
    #ifdef __APPLE__
        #include <sys/attr.h>   // getattrlist: an entry's stored name, asked of the entry (storedEntryName)
    #endif
#endif

namespace dss::core {
namespace {

namespace fs = std::filesystem;

// ════ THE ROOT OF A SPELLING, AND ITS ONE NORMALIZATION ════
// (the header states every measurement these follow; both are pure text, in either grammar, on any host)

template <class C>
[[nodiscard]] constexpr bool isAsciiAlpha(C c) {
    return (c >= C('a') && c <= C('z')) || (c >= C('A') && c <= C('Z'));
}

template <class C>
[[nodiscard]] constexpr C asciiLower(C c) {
    return (c >= C('A') && c <= C('Z')) ? static_cast<C>(c - C('A') + C('a')) : c;
}

// `text` is `word` in any ASCII case (Win32 matches `UNC`, `GLOBALROOT` and the reserved device names so).
template <class C>
[[nodiscard]] bool equalsAsciiFolded(std::basic_string_view<C> text, std::string_view word) {
    if (text.size() != word.size()) return false;
    for (std::size_t i = 0; i < word.size(); ++i) {
        if (asciiLower(text[i]) != asciiLower(static_cast<C>(word[i]))) return false;
    }
    return true;
}

template <class C>
[[nodiscard]] constexpr bool separatesIn(C c, PathGrammar grammar, bool verbatim) {
    if (grammar == PathGrammar::Posix) return c == C('/');
    return c == C('\\') || (!verbatim && c == C('/'));
}

// The end of the segment that starts at `at`: the next separator, or the end.
template <class C>
[[nodiscard]] std::size_t segmentEnd(std::basic_string_view<C> s, std::size_t at, PathGrammar grammar, bool verbatim) {
    while (at < s.size() && !separatesIn(s[at], grammar, verbatim)) ++at;
    return at;
}

// Past `count` segments from `at`, each with the separator after it when one is spelled.
template <class C>
[[nodiscard]] std::size_t pastSegments(std::basic_string_view<C> s, std::size_t at, int count, PathGrammar grammar,
                                       bool verbatim) {
    for (int i = 0; i < count && at < s.size(); ++i) {
        at = segmentEnd(s, at, grammar, verbatim);
        if (at < s.size()) ++at;
    }
    return at;
}

template <class C>
[[nodiscard]] RootSpelling parseRoot(std::basic_string_view<C> s, PathGrammar grammar) {
    using Kind = RootSpelling::Kind;
    RootSpelling root;
    if (grammar == PathGrammar::Posix) {
        std::size_t run = 0;
        while (run < s.size() && s[run] == C('/')) ++run;
        if (run == 0) return root;
        root.kind      = run == 2 ? Kind::Authority : Kind::Rooted;
        root.authority = run == 2;
        root.prefix = root.volume = run;
        return root;
    }
    auto const sep = [](C c) { return c == C('\\') || c == C('/'); };
    if (s.size() >= 2 && sep(s[0]) && sep(s[1])) {
        if (s.size() >= 3 && (s[2] == C('.') || s[2] == C('?')) && (s.size() == 3 || sep(s[3]))) {
            // A device prefix: `\\?\` exactly is verbatim, every other spelling of it (`\\.\`, `//./`, `//?/`) is
            // normalized. Its volume is the segment after it -- `C:`, `Volume{G}` -- or, after `UNC`, a server and
            // its share, or, after `GLOBALROOT`, an object directory and the device in it.
            bool const verbatim = s.size() >= 4 && s[0] == C('\\') && s[1] == C('\\') && s[2] == C('?')
                               && s[3] == C('\\');
            std::size_t const p     = std::min<std::size_t>(4, s.size());
            std::size_t const first = segmentEnd(s, p, grammar, verbatim);
            auto const        name  = s.substr(p, first - p);
            bool const        unc   = equalsAsciiFolded(name, "UNC");
            int const         depth = (unc || equalsAsciiFolded(name, "GLOBALROOT")) ? 3 : 1;
            root.kind      = verbatim ? Kind::Verbatim : Kind::Device;
            root.authority = unc;
            root.volume    = pastSegments(s, p, depth, grammar, verbatim);
            root.prefix    = verbatim ? root.volume : p;
            return root;
        }
        root.kind      = Kind::Share;
        root.authority = true;
        root.prefix = root.volume = pastSegments(s, 2, 2, grammar, false);
        return root;
    }
    if (s.size() >= 2 && isAsciiAlpha(s[0]) && s[1] == C(':')) {
        bool const rooted = s.size() >= 3 && sep(s[2]);
        root.kind   = rooted ? Kind::Drive : Kind::DriveRelative;
        root.prefix = root.volume = rooted ? 3 : 2;
        return root;
    }
    if (!s.empty() && sep(s[0])) {
        root.kind   = Kind::Rooted;
        root.prefix = root.volume = 1;
    }
    return root;
}

// What Win32's DOS normalization leaves of one component that is not `.` or `..` (✔MEASURED 2026-11-04,
// probe-reference-cc run 20261104-210843-61c9329b, windows-x86_64-release, GetFullPathNameW and the opened handle
// agreeing on every row): a component a SEPARATOR follows loses ONE trailing period and nothing else (`d.` is `d`,
// `d .` is `d `; `d..`, `d `, `d. ` and `...` stay); the LAST component, with no separator after it, loses every
// trailing period and space (`x.txt..` and `x.txt. .` are `x.txt`; `...` and `   ` vanish, the separator before them
// staying). `.. ` and `. ` are names, not steps. Shares and device paths alike; nothing of it under `\\?\`.
template <class C>
[[nodiscard]] std::basic_string_view<C> dosTrimmed(std::basic_string_view<C> component, bool followed) {
    if (followed) {
        if (component.size() >= 2 && component.back() == C('.') && component[component.size() - 2] != C('.'))
            component.remove_suffix(1);
        return component;
    }
    while (!component.empty() && (component.back() == C('.') || component.back() == C(' ')))
        component.remove_suffix(1);
    return component;
}

// THE ONE NORMALIZATION (`normalizeKeepingRoot`'s, in either grammar): the prefix `parseRoot` names is kept whole --
// its separators the grammar's own, a device prefix its own `\\.\` or `\\?\`, a POSIX run of three or more one `/` --
// and the rest is normalized in components: an empty one and `.` dropped, a `..` taking the name before it and never
// climbing into the prefix (kept, below a relative or drive-relative prefix, as `lexically_normal()` keeps it), each
// name trimmed as Win32 trims it (`dosTrimmed`), every separator the grammar's own. A separator ends the result where
// the grammar's own resolver leaves one (the loop's note); under Win32 a drive spelling's last name `NUL` is the
// device. ★ UNDER WIN32 IT IS GetFullPathNameW's ANSWER, made equal to it over F3's universe and PINNED so on both
// Windows legs (tests/core/test_path_identity.cpp; os_path_spelling.hpp is the OS's side) -- the model a host with
// no Win32 answers by. A verbatim spelling comes back as written; an empty one empty; one that normalizes to nothing
// at all `.`.
template <class C>
[[nodiscard]] std::basic_string<C> normalizeIn(std::basic_string_view<C> s, PathGrammar grammar) {
    using Kind   = RootSpelling::Kind;
    using String = std::basic_string<C>;
    using View   = std::basic_string_view<C>;
    RootSpelling const root = parseRoot(s, grammar);
    if (s.empty() || root.kind == Kind::Verbatim) return String{s};
    C const    sep   = grammar == PathGrammar::Win32 ? C('\\') : C('/');
    bool const win32 = grammar == PathGrammar::Win32;
    String     out;
    if (root.kind == Kind::Device) {
        // The device prefix keeps its own `.` or `?`, its separators the OS's: `//?/C:/x` is `\\?\C:\x` to the OS
        // (✔MEASURED, GetFullPathNameW, run 20261104-222037-6ef9b98e), and `//./C:/x` is `\\.\C:\x`.
        out = String{sep, sep, s[2] == C('?') ? C('?') : C('.'), sep};
    } else if (grammar == PathGrammar::Posix && root.kind == Kind::Rooted) {
        out = String(1, sep);
    } else {
        out = String{s.substr(0, root.prefix)};
        for (C& c : out) {
            if (separatesIn(c, grammar, false)) c = sep;
        }
    }
    bool const        rooted = root.kind != Kind::Relative && root.kind != Kind::DriveRelative;
    std::vector<View> kept;
    bool              trailing      = false;
    bool              sawComponent  = false;
    for (std::size_t at = root.prefix; at < s.size();) {
        std::size_t const end      = segmentEnd(s, at, grammar, false);
        bool const        followed = end < s.size();
        View              name     = s.substr(at, end - at);
        bool const        isDot    = name.size() == 1 && name[0] == C('.');
        bool const        isDotDot = name.size() == 2 && name[0] == C('.') && name[1] == C('.');
        sawComponent               = true;
        // ★ WHERE A SEPARATOR ENDS THE RESULT. Under POSIX, where `lexically_normal()` leaves one: after a last `.` or
        // `..` too. Under Win32, where the OS leaves one (✔MEASURED, GetFullPathNameW, runs 20261104-222037-6ef9b98e
        // and 20261104-223135-5f04ee9f): only where the spelling ENDED in a separator (`C:\a\b\..\` is `C:\a\`), or
        // in a name the trim took away (`C:\d\...` is `C:\d\`) -- a last `.` or `..` leaves none (`C:\d\.` is `C:\d`,
        // `C:\a\b\..` is `C:\a`, `\\server\share\..` is `\\server\share`, `\\.\C:\d\..` is `\\.\C:`), a drive root
        // keeping its own (`C:\d\..` is `C:\`).
        if (name.empty()) {
            trailing = true;
        } else if (isDot || isDotDot) {
            if (isDotDot) {
                bool const takesAName = !kept.empty() && !(kept.back().size() == 2 && kept.back()[0] == C('.')
                                                           && kept.back()[1] == C('.'));
                if (takesAName) kept.pop_back();
                else if (!rooted) kept.push_back(name);
            }
            trailing = !win32 || followed;
        } else {
            if (grammar == PathGrammar::Win32) name = dosTrimmed(name, followed);
            if (name.empty()) {
                trailing = true;
            } else {
                kept.push_back(name);
                trailing = followed;
            }
        }
        if (!followed) break;
        at = end + 1;
    }
    if (!win32 && !kept.empty() && kept.back().size() == 2 && kept.back()[0] == C('.') && kept.back()[1] == C('.'))
        trailing = false;   // `lexically_normal()` ends a path whose last name is `..` with no separator
    // ★ A RESERVED DEVICE NAME, as the OS resolves one: a LAST name that is `NUL` -- any case, with every trailing
    // period, space and `:` dropped -- is the device `\\.\NUL`, below a drive, a rooted, a drive-relative or a
    // relative spelling alike (✔MEASURED, GetFullPathNameW, runs 20261104-222037-6ef9b98e and
    // 20261104-223135-5f04ee9f: `C:\d\NUL`, `C:\d\Nul`, `C:\d\NUL.`, `C:\d\NUL. `, `C:\d\NUL:`, `C:\d\NUL::`,
    // `C:\d\NUL.:`, `C:\d\NUL :`, `C:\d\x\..\NUL`, `C:\NUL`, `NUL`, `d\NUL`, `\d\NUL` and `C:NUL` are `\\.\NUL`, its
    // case kept; `C:\d\NUL.txt`, `C:\d\NUL .txt`, `C:\d\NUL:x`, `C:\d\ NUL`, `C:\d\NUL\` and `C:\d\NUL\x`, a share's
    // `NUL` and every `\\.\` spelling's are names). ⚠ THE ONE NAME EVERY WINDOWS SHARES: this Windows 11 resolves no
    // other (`CON`, `PRN`, `AUX`, `COM1`, `LPT1`, `CONIN$` and `nul.txt` stayed names), where older ones resolved
    // them too (DOCUMENTED, "Naming Files, Paths, and Namespaces") -- so a key on a Windows host is the OS's own
    // (`normalizeKeepingRoot`), and this models the name they all resolve.
    if (win32 && root.kind != Kind::Share && root.kind != Kind::Device && !trailing && !kept.empty()) {
        View last = kept.back();
        while (!last.empty() && (last.back() == C('.') || last.back() == C(' ') || last.back() == C(':')))
            last.remove_suffix(1);
        if (equalsAsciiFolded(last, std::string_view{"NUL"})) return String{sep, sep, C('.'), sep} + String{last};
    }
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (i != 0) out += sep;
        out += kept[i];
    }
    if (trailing && !kept.empty()) out += sep;
    // A share stepped back to its root by a `.` or a `..` is the share with no separator after it, as the OS spells
    // it; a share spelled with one keeps it.
    if (win32 && root.kind == Kind::Share && kept.empty() && sawComponent && !trailing && out.size() > 2
        && out.back() == sep)
        out.pop_back();
    if (out.empty()) out = String(1, C('.'));
    return out;
}

// THE ONE STEP OF EVERY WALK UP (`reachedPath`'s, `expandShortComponents`', `parentWithinRoot`'s): where the head
// before the last name of `s[0, end)` ends -- its separators trimmed, never into the volume root `root` parses, a
// verbatim spelling cut at `\` alone -- or `npos` when `s[0, end)` is the volume root itself and there is nothing
// above it to step to. `stepEnd` is where the trailing separators of `s` end, the same way: a separator after the
// last name names nothing more.
template <class C>
[[nodiscard]] std::size_t stepEnd(std::basic_string_view<C> s, RootSpelling const& root, PathGrammar grammar) {
    bool const  literal = root.kind == RootSpelling::Kind::Verbatim;
    std::size_t end     = s.size();
    while (end > root.volume && separatesIn(s[end - 1], grammar, literal)) --end;
    return end;
}
template <class C>
[[nodiscard]] std::size_t parentEnd(std::basic_string_view<C> s, std::size_t end, RootSpelling const& root,
                                    PathGrammar grammar) {
    if (end <= root.volume) return std::basic_string_view<C>::npos;
    bool const  literal = root.kind == RootSpelling::Kind::Verbatim;
    std::size_t cut     = end;
    while (cut > root.volume && !separatesIn(s[cut - 1], grammar, literal)) --cut;
    while (cut > root.volume && separatesIn(s[cut - 1], grammar, literal)) --cut;
    return cut;
}
// Where the last name of `s[0, end)` begins, past the separators after its head `head`.
template <class C>
[[nodiscard]] std::size_t nameBegin(std::basic_string_view<C> s, std::size_t head, RootSpelling const& root,
                                    PathGrammar grammar) {
    bool const literal = root.kind == RootSpelling::Kind::Verbatim;
    while (head < s.size() && separatesIn(s[head], grammar, literal)) ++head;
    return head;
}

// The reserved device names Win32's DOS namespace maps a component to (DOCUMENTED, Microsoft, "Naming Files, Paths,
// and Namespaces"; `NUL` MEASURED) -- taken with any extension and trailing spaces, as older Windows takes them, which
// keeps a verbatim spelling verbatim wherever a release might read the name as the device.
[[nodiscard]] bool namesAReservedDevice(std::wstring_view component) {
    std::wstring_view stem = component.substr(0, std::min(component.find(L'.'), component.size()));
    while (!stem.empty() && stem.back() == L' ') stem.remove_suffix(1);
    for (std::string_view const word : {"CON", "PRN", "AUX", "NUL"}) {
        if (equalsAsciiFolded(stem, word)) return true;
    }
    if (stem.size() == 4 && (equalsAsciiFolded(stem.substr(0, 3), std::string_view{"COM"})
                             || equalsAsciiFolded(stem.substr(0, 3), std::string_view{"LPT"}))) {
        wchar_t const n = stem[3];
        // ...and the superscript digits one, two and three (U+00B9, U+00B2, U+00B3), which Win32 reserves too.
        return (n >= L'0' && n <= L'9') || n == wchar_t{0x00B9} || n == wchar_t{0x00B2} || n == wchar_t{0x00B3};
    }
    return false;
}

#ifdef _WIN32

// `GetLongPathNameW` for ONE path, or empty when Windows will not answer.
//
// ★ WHAT IS LEFT FOR THIS CALL TO DO. `canonicalIdentityKey` resolves every key through `reachedPath`
// FIRST, and on Windows that is `GetFinalPathNameByHandleW` -- which already rewrites junctions, mount
// points and links and spells every component it resolved in its long form. So for the part of a path
// that EXISTS this expansion changes nothing (and a not-yet-existing tail has no long form to expand
// to). What it still expands is a key `reachedPath` did not produce: the lexical fallback taken when the
// resolution failed or would have relocated an authority. It touches nothing but 8.3 components.
[[nodiscard]] std::wstring dosSpellingOf(std::wstring const& verbatim);

// ⓘ AT ANY LENGTH (P69 H6): an absolute spelling is asked in its extended-length spelling, and the answer, then spelled
// verbatim, is given back in the DOS form wherever that form names the same file -- as `reachedPath` gives its own.
// A relative one is asked as written: its answer is relative, and stays so.
[[nodiscard]] std::wstring longNameOrEmpty(std::wstring const& in) {
    if (in.empty()) return {};
    RootSpelling::Kind const kind = rootSpelling(std::wstring_view{in}, PathGrammar::Win32).kind;
    bool const absolute = kind == RootSpelling::Kind::Drive || kind == RootSpelling::Kind::Share
                       || kind == RootSpelling::Kind::Device;
    std::wstring const asked = absolute ? extendedLengthSpelling(fs::path{in}).native() : in;
    DWORD const n = ::GetLongPathNameW(asked.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring out(n, L'\0');
    DWORD const written = ::GetLongPathNameW(asked.c_str(), out.data(), n);
    // `written >= n` means the path changed under us between the two calls.
    if (written == 0 || written >= n) return {};
    out.resize(written);
    return asked == in ? out : dosSpellingOf(out);
}

// Expand the longest EXISTING prefix and re-attach the remainder verbatim.
//
// ★★★ THIS WALK IS MANDATORY, NOT DEFENSIVE — ✔MEASURED 2026-08-18 on this
// machine, with a standalone probe, before this function was written:
//
//     DSS-SC~1                   ->  dss-scratch-spelling-probe-longname   (expanded)
//     DSS-SC~1\not\yet\built.o   ->  DSS-SC~1\not\yet\built.o              (UNCHANGED)
//
// `GetLongPathNameW` fails WHOLESALE when any component is absent. Calling it on
// the full path alone therefore no-ops on exactly the paths that do not exist
// yet — every output artifact, every not-yet-generated source — while looking
// perfectly correct on every input path that happens to exist. That is a false
// green of the family this project keeps catching: the property appears to hold
// because the cases that would refute it were never exercised.
//
// ★ THE WALK STOPS AT THE VOLUME ROOT `rootSpelling` parses (P69 H6), as `reachedPath`'s does: the library's
// `parent_path()` steps from `\\?\C:\x` onto `\\?\C:` -- the volume's device -- and splits a verbatim spelling at a `/`
// Win32 reads as a character.
[[nodiscard]] std::wstring expandShortComponents(std::wstring const& in) {
    if (auto const whole = longNameOrEmpty(in); !whole.empty()) return whole;

    std::wstring_view const s       = in;
    RootSpelling const      root    = rootSpelling(s, PathGrammar::Win32);
    bool const              literal = root.kind == RootSpelling::Kind::Verbatim;
    // The tail is re-attached from `in` as written, ending at its last name.
    // ⚠ NEVER WITH A SEPARATOR AFTER IT: the first cut of this walk built its tail with `operator/` against an EMPTY
    // tail, which APPENDS a separator, and every key built through it carried one the whole-path fast path above does
    // not produce. ✔MEASURED, ONE path, two keys, differing only in whether the file was there yet:
    //     before it exists : 'C:/.../idtmp/notyet.o/'
    //     after  it exists : 'C:/.../idtmp/notyet.o'
    // That is precisely the two-keys-for-one-file defect this type exists to prevent, on the not-yet-created inputs
    // the header's own note calls a FIRST-CLASS case (output artifacts, a `path` dependency resolved before its
    // directory is confirmed, a cycle key needed for the reject's own message).
    std::size_t const tailEnd   = stepEnd(s, root, PathGrammar::Win32);
    std::size_t       headEnd   = tailEnd;
    std::size_t       tailBegin = tailEnd;
    for (bool first = true;; first = false) {
        // Ask the head -- the whole once more only when separators were trimmed from it.
        if (headEnd == 0) return in;   // a relative spelling with nothing left to ask
        if (!first || headEnd != in.size()) {
            if (auto const expanded = longNameOrEmpty(in.substr(0, headEnd)); !expanded.empty()) {
                std::wstring out = expanded;
                if (tailBegin < tailEnd) {
                    if (!separatesIn(out.back(), PathGrammar::Win32, literal)) out += L'\\';
                    out.append(in, tailBegin, tailEnd - tailBegin);
                }
                return out;
            }
        }
        std::size_t const parent = parentEnd(s, headEnd, root, PathGrammar::Win32);
        if (parent == std::wstring_view::npos) return in;   // nothing of it expands, its volume root included
        tailBegin = nameBegin(s, parent, root, PathGrammar::Win32);
        headEnd   = parent;
    }
}

// The OS's verbatim final path `verbatim`, in the DOS form -- `C:\...`, `\\server\share\...` -- exactly when that form
// names the same file (`dosSpellingNamesTheSameFile`); as it is otherwise, and for a volume with no DOS name.
[[nodiscard]] std::wstring dosSpellingOf(std::wstring const& verbatim) {
    RootSpelling const root = rootSpelling(verbatim, PathGrammar::Win32);
    if (root.kind != RootSpelling::Kind::Verbatim) return verbatim;
    if (!dosSpellingNamesTheSameFile(std::wstring_view{verbatim}.substr(root.volume))) return verbatim;
    if (root.authority) return L"\\\\" + verbatim.substr(8);   // `\\?\UNC\server\share\` -> `\\server\share\`
    bool const drive = root.volume >= 6 && isAsciiAlpha(verbatim[4]) && verbatim[5] == L':';
    return drive ? verbatim.substr(4) : verbatim;
}

#endif  // _WIN32

}  // namespace

RootSpelling rootSpelling(std::wstring_view native, PathGrammar grammar) { return parseRoot(native, grammar); }
RootSpelling rootSpelling(std::string_view native, PathGrammar grammar) { return parseRoot(native, grammar); }
RootSpelling rootSpelling(fs::path const& p) {
    return parseRoot(std::basic_string_view<fs::path::value_type>{p.native()}, kHostPathGrammar);
}

fs::path parentWithinRoot(fs::path const& p) {
    std::basic_string_view<fs::path::value_type> const s{p.native()};
    RootSpelling const root   = parseRoot(s, kHostPathGrammar);
    std::size_t const  parent = parentEnd(s, stepEnd(s, root, kHostPathGrammar), root, kHostPathGrammar);
    if (parent == decltype(s)::npos || parent == 0) return {};
    return fs::path{fs::path::string_type{s.substr(0, parent)}};
}

std::wstring normalizedSpelling(std::wstring_view native, PathGrammar grammar) { return normalizeIn(native, grammar); }
std::string normalizedSpelling(std::string_view native, PathGrammar grammar) { return normalizeIn(native, grammar); }

bool dosSpellingNamesTheSameFile(std::wstring_view tail) {
    if (tail.find(L'/') != std::wstring_view::npos) return false;
    // A tail names the same file in both namespaces when the ONE normalization leaves it as it is (below a volume
    // root, so a `..` has nothing to take but is still no name) and no component is a reserved device's name.
    std::wstring const below = std::wstring{L"\\"} + std::wstring{tail};
    if (normalizeIn(std::wstring_view{below}, PathGrammar::Win32) != below) return false;
    for (std::size_t at = 0; at < tail.size();) {
        std::size_t const end = std::min(tail.find(L'\\', at), tail.size());
        if (namesAReservedDevice(tail.substr(at, end - at))) return false;
        at = end + 1;
    }
    return true;
}

fs::path extendedLengthSpelling(fs::path const& p) {
#ifdef _WIN32
    return fs::path{::dss::substrate::osExtendedSpelling(p.native())};   // the OS's rule, at its one home (the header)
#else
    return p;
#endif
}

fs::path applicationNameSpelling(fs::path const& p) {
#ifdef _WIN32
    return fs::path{::dss::substrate::osApplicationName(p.native())};   // the same home's threshold rule
#else
    return p;
#endif
}

// `lexically_normal()` WITHOUT letting it eat the leading separator RUN.
//
// ★★★ ✔MEASURED, and it is the same loss `generic_string()` performs one step
// later, so the two together destroyed a UNC path twice over
// ([[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]). Printed with `.string()`,
// so no print-side transform can be blamed:
//     input            '\\wsl.localhost\Ubuntu\home\<user>\p\uncprobe.h'
//     lexically_normal '\wsl.localhost\Ubuntu\home\<user>\p\uncprobe.h'
// One separator gone: the authority is now an ordinary directory on the local
// drive root, and the key names a file that does not exist. A run of TWO OR MORE
// leading separators is the one place a "redundant" separator is not redundant,
// on every path model that does not give the authority a `root_name()` of its
// own -- so the run is preserved verbatim (spelled with the model's own
// preferred separator, which is what makes the `//` and `\\` spellings of ONE
// file converge on ONE key) and only the part BELOW it is normalised.
//
// ★★ THIS IS NOT A WINDOWS QUIRK, AND THAT WAS MEASURED RATHER THAN ASSUMED --
// a fix reasoned on one host is a portability claim. On Linux/libstdc++ 13.3.0
// the same path answers `is_absolute()` TRUE with an EMPTY `root_name()`,
// `lexically_normal()` collapses the run just the same, and `weakly_canonical`
// SUCCEEDS returning the collapsed form; the shipped derivation therefore gave
// `//x/share/h` and `/x/share/h` the SAME key there too. POSIX makes a leading
// run of exactly two separators implementation-defined, so keeping the two
// apart is the safe direction on that host as well: a duplicate key costs
// duplicate work, a merged one costs wrong contents.
//
// ★★★ AND THE LIBRARY'S NORMALIZATION IS ASKED OF NOTHING ANY MORE (P69 H6): it is `normalizedSpelling`, ONE rule in
// both grammars, for every kind of root -- the library's `lexically_normal()` keeps what Win32 trims (`x.c.` is the
// file `x.c` to the OS, and was a second key here) and climbs where the OS does not (`\\host\share\..\x` was
// `\\host\x`), and the two Windows libraries do not even agree on where a root ends (`rootSpelling`'s header).
//
// ★ ON A WINDOWS HOST AN ABSOLUTE PLAIN SPELLING IS THE OS'S OWN ANSWER (P69 H6): GetFullPathNameW, through the one
// home of that rule (os_path_spelling.hpp). The normalization is pinned equal to it on both Windows legs, but which
// reserved names it resolves is the release's (this Windows 11 resolves a last `NUL` alone, older ones `CON`, `COM1`,
// ... too), and the OS, not a model of it, says what a plain spelling means. A relative, rooted or drive-relative
// spelling stays the text's -- the OS would resolve it against a directory, which this never does.
fs::path normalizeKeepingRoot(fs::path const& p) {
#ifdef _WIN32
    RootSpelling::Kind const kind = rootSpelling(p).kind;
    if (kind == RootSpelling::Kind::Drive || kind == RootSpelling::Kind::Share || kind == RootSpelling::Kind::Device) {
        if (std::wstring full = ::dss::substrate::osFullPathSpelling(p.native()); !full.empty())
            return fs::path{std::move(full)};
    }
#endif
    return fs::path{normalizeIn(std::basic_string_view<fs::path::value_type>{p.native()}, kHostPathGrammar)};
}

std::string genericSpelling(fs::path const& p) {
    std::string out = p.string();
    if constexpr (fs::path::preferred_separator != fs::path::value_type{'/'}) {
        std::replace(out.begin(), out.end(),
                     static_cast<char>(fs::path::preferred_separator), '/');
    }
    return out;
}

std::string genericSpelling(std::string_view spelling) {
    // Through the `fs::path` form, never a second `std::replace` here: two
    // spellings of one rule are two owners of it, and the one that gets
    // forgotten is the one that stops matching. `fs::path`'s narrow constructor
    // does not touch the bytes, so a stored native spelling round-trips into
    // the same substitution the path form performs.
    return genericSpelling(fs::path{spelling});
}

std::u8string genericSpellingU8(fs::path const& p) {
    // `u8string()` and NOT `generic_u8string()` -- the NATIVE spelling carries
    // the leading run, and the substitution below is the same normalisation
    // without the generic-format conversion that eats it. See the header.
    std::u8string out = p.u8string();
    if constexpr (fs::path::preferred_separator != fs::path::value_type{'/'}) {
        // Every path separator is ASCII, so it is ONE UTF-8 code unit and a
        // per-unit substitution cannot land inside a multi-byte sequence.
        std::replace(out.begin(), out.end(),
                     static_cast<char8_t>(fs::path::preferred_separator),
                     char8_t{'/'});
    }
    return out;
}

fs::path absoluteKeepingRoot(fs::path const& p, std::error_code& ec) {
    // A share, POSIX `//` and a device or verbatim prefix name an AUTHORITY or a NAMESPACE, not a location on the
    // current drive, so there is nothing here to make absolute. See the header for the measurement: `fs::absolute`
    // does not merely fail on these, it succeeds having re-rooted the path onto the local drive.
    switch (rootSpelling(p).kind) {
        case RootSpelling::Kind::Share:
        case RootSpelling::Kind::Authority:
        case RootSpelling::Kind::Device:
        case RootSpelling::Kind::Verbatim:
            ec.clear();
            return p;
        case RootSpelling::Kind::Relative:
        case RootSpelling::Kind::DriveRelative:
        case RootSpelling::Kind::Rooted:
        case RootSpelling::Kind::Drive:
            break;
    }
    return fs::absolute(p, ec);
}

std::string canonicalIdentityKey(fs::path const& p) {
    std::error_code ec;
    return canonicalIdentityKey(p, ec);
}

std::string canonicalIdentityKey(fs::path const& p, std::error_code& ec) {
    ec.clear();
    // The canonicalisation's OWN failure, kept separate from the `ec` this
    // function reports: the two answer different questions (see below), and
    // folding them was what let a model failure masquerade as a host refusal.
    //
    // ★ THE PATH A LINK LEADS TO IS ASKED OF `reachedPath`, NEVER OF THE LIBRARY: libstdc++ on Windows
    // resolves no link (its `weakly_canonical` hands one back unchanged), so the MinGW build keyed a link
    // and the file it reaches as TWO identities where every other build keys one
    // ([[D-CORE-MINGW-BUILDS-WALKS-AND-REMOVALS-THAT-FOLLOW-A-LINK]]). On Windows `reachedPath` is the final
    // path of the opened file's handle -- what MS STL's `weakly_canonical` asks too.
    std::error_code canonEc;
    fs::path const c = reachedPath(p, canonEc);
    if (canonEc) {
        // ⚠ A `weakly_canonical` FAILURE IS NOT EVIDENCE THAT THE FILESYSTEM
        // REFUSED, and this overload's whole contract is that `ec` means exactly
        // that. ✔MEASURED: `weakly_canonical` fails `ENOENT` 200 times out of
        // 200 on every spelling of a UNC path whose `exists()` is TRUE, whose
        // `file_size()` answers, and whose parent enumerates. The library's path
        // model cannot walk the authority; the OS has no such trouble.
        // Forwarding that as a refusal made every caller that fails loud on `ec`
        // -- the project-source de-duplication is one, and its own comment
        // asserts "`weakly_canonical` does NOT error merely because a path does
        // not exist, so this arm means a genuine filesystem failure" -- reject a
        // perfectly readable source. So ASK THE FILESYSTEM DIRECTLY: if it
        // answers at all, whatever the answer, it did not refuse. (Through this file's own question, which answers at
        // any length on Windows -- the library's `exists` does not: `extendedLengthSpelling`'s header.)
        std::error_code probe;
        static_cast<void>(reachedKind(p, probe));
        ec = probe;
    }
    // ★★★ AND A SUCCESS IS NOT EVIDENCE THAT IT ANSWERED FOR THE PATH IT WAS
    // ASKED ABOUT. ✔MEASURED, the OTHER branch of the same model gap, and this
    // one is the collision the row asked about:
    //     input             '//no-such-server/share/x.h'   (exists: false)
    //     weakly_canonical  'C:\no-such-server\share\x.h'  ec: <none>
    // With the authority unmodelled, `weakly_canonical` treats the path as
    // drive-relative and RELOCATES it onto the local drive, reporting success.
    // The key it yields is then BYTE-IDENTICAL to the key of a real local file
    // at `C:\no-such-server\share\x.h` -- two distinct files, ONE identity --
    // which on any of the maps this type keys is a wrong-content hazard rather
    // than a duplicate-work one. (A UNC path that EXISTS takes the `ENOENT`
    // branch above instead, so the two branches together are the whole
    // exposure.) A canonicalisation that took the AUTHORITY away (`rootSpelling`) changed which machine the path
    // names, so it is refused here and the lexical normalisation -- which cannot relocate anything -- is used. A
    // namespace prefix the OS resolved to its drive is no relocation: `\\?\C:\x` and `C:\x` are one file, one key.
    bool const relocated = rootSpelling(p).authority && !rootSpelling(c).authority;
    fs::path const r = (canonEc || relocated) ? normalizeKeepingRoot(p) : c;
#ifdef _WIN32
    // ONE separator convention for every key in the compiler: two sites that key
    // the same file with different separators disagree about identity for a
    // reason no diagnostic can explain. `genericSpelling` and NOT
    // `generic_string()` -- see the header for what the latter deletes.
    return genericSpelling(fs::path{expandShortComponents(r.wstring())});
#else
    return genericSpelling(r);
#endif
}

// ════ THE KIND OF A DIRECTORY ENTRY ITSELF, AND THE WALK AND THE REMOVAL THAT ASK IT ════
// (the header states why the OS is asked on Windows, with the measurement)

namespace {

#ifdef _WIN32

// The answers Windows gives for "nothing is there" -- exactly the set MS STL's `status` reads as
// `not_found` (READ 2026-10-21: `__std_is_file_not_found` in xfilesystem_abi.h, MSVC 14.51.36231), so
// the MinGW build and the MSVC build call the same answers absence; every other refusal is an error.
[[nodiscard]] bool winNotFound(DWORD e) {
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_BAD_NETPATH
        || e == ERROR_INVALID_NAME || e == ERROR_BAD_PATHNAME || e == ERROR_DIRECTORY
        || e == ERROR_NETNAME_DELETED;
}

[[nodiscard]] std::error_code winError(DWORD e) {
    return std::error_code{static_cast<int>(e), std::system_category()};
}

// The spelling EVERY Win32 call of this file takes: what the plain spelling means, at any length
// (`extendedLengthSpelling`; the header measures why).
[[nodiscard]] std::wstring callSpelling(fs::path const& p) { return extendedLengthSpelling(p).native(); }

// What `p` REACHES, read through a handle opened WITHOUT `FILE_FLAG_OPEN_REPARSE_POINT` -- a handle to
// what every link on the way reaches. Nothing when nothing is reached; `ec` set only for a failure
// that is not "nothing there".
[[nodiscard]] std::optional<BY_HANDLE_FILE_INFORMATION> reachedInformation(fs::path const& p, std::error_code& ec) {
    HANDLE const reached = ::CreateFileW(callSpelling(p).c_str(), FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (reached == INVALID_HANDLE_VALUE) {
        DWORD const e = ::GetLastError();
        if (!winNotFound(e)) ec = winError(e);
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    BOOL const  read = ::GetFileInformationByHandle(reached, &info);
    DWORD const e    = read ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(reached);
    if (!read) {
        ec = winError(e);
        return std::nullopt;
    }
    return info;
}

// One entry's kind from what the OS recorded for it: its attributes and, for a reparse point, its tag.
[[nodiscard]] EntryKind kindOfAttributes(DWORD attributes, DWORD reparseTag) {
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        && (reparseTag == static_cast<DWORD>(IO_REPARSE_TAG_SYMLINK)
            || reparseTag == static_cast<DWORD>(IO_REPARSE_TAG_MOUNT_POINT))) {
        return EntryKind::Link;
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? EntryKind::Directory : EntryKind::File;
}

// Every entry of `dir` but `.` and `..`, each classified from its own find record -- the record
// carries the attributes AND a reparse point's tag, so no entry costs a second call.
void listEntries(fs::path const& dir, std::size_t depth, std::vector<TreeEntry>& out, std::error_code& ec) {
    ec.clear();
    fs::path const   pattern = extendedLengthSpelling(dir) / L"*";   // each entry below is named from `dir` as given
    WIN32_FIND_DATAW found{};
    HANDLE const     search = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch,
                                                 nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (search == INVALID_HANDLE_VALUE) {
        ec = winError(::GetLastError());
        return;
    }
    do {
        std::wstring_view const name{found.cFileName};
        if (name == L"." || name == L"..") continue;
        DWORD const     tag  = (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? found.dwReserved0 : 0;
        EntryKind const kind = kindOfAttributes(found.dwFileAttributes, tag);
        out.push_back(TreeEntry{dir / fs::path{std::wstring{name}}, kind, kind, depth});
    } while (::FindNextFileW(search, &found));
    DWORD const last = ::GetLastError();
    ::FindClose(search);
    if (last != ERROR_NO_MORE_FILES) ec = winError(last);
}

// ── THE HANDLE-RELATIVE REMOVAL'S PIECES (the header states why the removal never goes by path) ──

// One owned handle.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    Handle(Handle&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            reset();
            h_ = std::exchange(o.h_, nullptr);
        }
        return *this;
    }
    Handle(Handle const&)            = delete;
    Handle& operator=(Handle const&) = delete;
    ~Handle() { reset(); }
    [[nodiscard]] HANDLE get() const { return h_; }
    [[nodiscard]] explicit operator bool() const { return h_ != nullptr; }
    void reset() {
        if (h_ != nullptr) ::CloseHandle(h_);
        h_ = nullptr;
    }

private:
    HANDLE h_ = nullptr;
};

// `NtOpenFile` and the status translation, from ntdll (loaded in every process): Win32 has no call that
// opens a name RELATIVE to a directory handle, and that relative open is what keeps a swapped-in link
// out of the removal.
struct NtCalls {
    using OpenFile  = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
    using ToWin32   = ULONG(NTAPI*)(LONG);
    OpenFile openFile = nullptr;
    ToWin32  toWin32  = nullptr;
};

[[nodiscard]] NtCalls const& ntCalls() {
    static NtCalls const calls = [] {
        NtCalls c;
        if (HMODULE const ntdll = ::GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr) {
            c.openFile = reinterpret_cast<NtCalls::OpenFile>(reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtOpenFile")));
            c.toWin32  = reinterpret_cast<NtCalls::ToWin32>(
                reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlNtStatusToDosError")));
        }
        return c;
    }();
    return calls;
}

// Open the entry `name` (one component) RELATIVE to the directory handle `dir`, NEVER following a link
// -- the entry itself is what is opened -- demanding `demand` of it (`FILE_DIRECTORY_FILE`, which a link to a
// directory meets as the link, or `FILE_NON_DIRECTORY_FILE`). The open never follows a reparse point, opens
// for backup intent (a directory too) and is synchronous. Not opened: `error` says why -- ERROR_DIRECTORY
// when the entry is not a directory a directory open demanded.
[[nodiscard]] Handle openEntryRelative(HANDLE dir, std::wstring const& name, ACCESS_MASK access, ULONG demand,
                                       DWORD& error) {
    NtCalls const& nt = ntCalls();
    if (nt.openFile == nullptr || nt.toWin32 == nullptr) {
        error = ERROR_PROC_NOT_FOUND;
        return {};
    }
    UNICODE_STRING us{};
    us.Buffer        = const_cast<PWSTR>(name.c_str());
    us.Length        = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    us.MaximumLength = us.Length;
    OBJECT_ATTRIBUTES oa{};
    oa.Length        = sizeof oa;
    oa.RootDirectory = dir;
    oa.ObjectName    = &us;
    oa.Attributes    = OBJ_CASE_INSENSITIVE;   // as every Win32 open names a file
    IO_STATUS_BLOCK io{};
    HANDLE          h = nullptr;
    LONG const status = nt.openFile(&h, access | SYNCHRONIZE, &oa, &io,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT
                                        | FILE_SYNCHRONOUS_IO_NONALERT | demand);
    if (status < 0) {
        error = nt.toWin32(status);
        return {};
    }
    return Handle{h};
}

// The kind of what the handle OPENED (opened without following a link, so a link is a Link).
//
// ⚠ A FILE SYSTEM THAT KEEPS NO REPARSE POINT REFUSES THE QUESTION OF ITS TAG: FAT32 answers
// FileAttributeTagInfo with ERROR_INVALID_PARAMETER (✔MEASURED 2026-11-03 on this host's FAT32 volume's root,
// probe-reference-cc run 20261104-021850-b279d3e9, where its attributes answer), and a removal that took that refusal
// for the entry's left every entry of such a volume in place. Its attributes then decide, when they state no
// reparse point -- an entry with none has no tag to ask. ⚠ UNPINNED: no case reaches this fallback, because no leg
// has a WRITABLE FAT volume to stage entries on (the measurement read only the volume's root).
[[nodiscard]] EntryKind kindOfHandle(HANDLE h, DWORD& error) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (::GetFileInformationByHandleEx(h, FileAttributeTagInfo, &info, sizeof info)) {
        return kindOfAttributes(info.FileAttributes, info.ReparseTag);
    }
    DWORD const                refused = ::GetLastError();
    BY_HANDLE_FILE_INFORMATION basic{};
    if (refused == ERROR_INVALID_PARAMETER && ::GetFileInformationByHandle(h, &basic)
        && (basic.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        return kindOfAttributes(basic.dwFileAttributes, 0);
    }
    error = refused;
    return EntryKind::Other;
}

// Every name the directory `dir` holds but `.` and `..`, listed THROUGH its handle -- the one handle the
// removal also deletes it through, opened with FILE_LIST_DIRECTORY for both -- so the listing reads the
// directory that was opened, whatever its path names now. (A listing handle reopened from a handle opened
// for removal alone is refused: ReOpenFile answers ACCESS_DENIED for a directory, on both Windows builds.)
[[nodiscard]] std::vector<std::wstring> namesThrough(HANDLE dir, DWORD& error) {
    std::vector<std::wstring> names;
    std::vector<LONGLONG>      buffer(8 * 1024);   // 64 KiB, aligned for the records
    FILE_INFO_BY_HANDLE_CLASS  cls = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!::GetFileInformationByHandleEx(dir, cls, buffer.data(),
                                            static_cast<DWORD>(buffer.size() * sizeof(LONGLONG)))) {
            DWORD const e = ::GetLastError();
            if (e != ERROR_NO_MORE_FILES) error = e;
            return names;
        }
        cls = FileIdBothDirectoryInfo;
        auto const* at = reinterpret_cast<unsigned char const*>(buffer.data());
        for (;;) {
            auto const* record = reinterpret_cast<FILE_ID_BOTH_DIR_INFO const*>(at);
            std::wstring_view const name{record->FileName, record->FileNameLength / sizeof(wchar_t)};
            if (name != L"." && name != L"..") names.emplace_back(name);
            if (record->NextEntryOffset == 0) break;
            at += record->NextEntryOffset;
        }
    }
}

// `FileDispositionInfoEx` (Windows 10 RS1), SPELLED BY VALUE because MinGW-w64's FILE_INFO_BY_HANDLE_CLASS
// ends before it at every version: ✔MEASURED 2026-10-28, MinGW-w64 13.2 at _WIN32_WINNT 0x0601, 0x0A00 and 0x0A00
// with NTDDI_VERSION RS5 (probe-reference-cc run 20261028-215651-aa544959). The Windows SDK's headers declare it
// (MSVC 19.51, run 20261028-215632-ef25bfcc), and there the value is the header's, by the assert. Its record and
// its flags are the headers' own on both.
constexpr auto kFileDispositionInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
#ifndef __MINGW32__
static_assert(FileDispositionInfoEx == kFileDispositionInfoEx);
#endif

// Delete the entry the handle opened, through the handle (it must carry DELETE). POSIX semantics where
// the OS has them (the name goes at once) and a read-only file deleted as MS STL's `remove` deletes one;
// otherwise the classic disposition, the read-only attribute cleared first. 0, or why it was refused.
[[nodiscard]] DWORD deleteThrough(HANDLE h) {
    FILE_DISPOSITION_INFO_EX ex{};
    ex.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
             | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
    if (::SetFileInformationByHandle(h, kFileDispositionInfoEx, &ex, sizeof ex)) return ERROR_SUCCESS;
    DWORD const e = ::GetLastError();
    // An OS or a file system without the extended disposition answers one of these; anything else is
    // the deletion's own refusal (a directory not empty, a file in use, access denied).
    if (e != ERROR_INVALID_PARAMETER && e != ERROR_NOT_SUPPORTED && e != ERROR_INVALID_FUNCTION) return e;
    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic)
        && (basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
        basic.FileAttributes &= ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
        if (basic.FileAttributes == 0) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        static_cast<void>(::SetFileInformationByHandle(h, FileBasicInfo, &basic, sizeof basic));
    }
    FILE_DISPOSITION_INFO classic{};
    classic.DeleteFile = TRUE;
    if (::SetFileInformationByHandle(h, FileDispositionInfo, &classic, sizeof classic)) return ERROR_SUCCESS;
    return ::GetLastError();
}

// The access a removal opens an entry with: delete it, read what it is, clear a read-only attribute -- and,
// for a directory, list it through the same handle.
constexpr ACCESS_MASK kRemovalAccess          = DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES;
constexpr ACCESS_MASK kDirectoryRemovalAccess = kRemovalAccess | FILE_LIST_DIRECTORY;

// A count of FILETIME ticks (100 ns since 1601-01-01) as the library's file clock `Clock` reads time. A file
// clock with `from_sys` (libstdc++'s) converts from the system clock, whose epoch is 1970-01-01, 11644473600 s
// later. MS STL's has none: its file clock IS that count -- its `now()` is the system ticks plus
// `__std_fs_file_time_epoch_adjustment`, 1601's distance from 1970 (READ, <chrono>, MSVC 14.51.36231) -- and its
// own `last_write_time` hands the FILETIME count back as it stands.
template <class Clock>
[[nodiscard]] fs::file_time_type fileTimeFromTicks(long long ticksSince1601) {
    using Ticks = std::chrono::duration<long long, std::ratio<1, 10'000'000>>;
    if constexpr (requires { Clock::from_sys(std::chrono::sys_time<Ticks>{}); }) {
        std::chrono::sys_time<Ticks> const sys{Ticks{ticksSince1601 - 116444736000000000LL}};
        return std::chrono::time_point_cast<fs::file_time_type::duration>(Clock::from_sys(sys));
    } else {
        static_assert(std::is_same_v<typename Clock::period, std::ratio<1, 10'000'000>>,
                      "a file clock with no from_sys is read as the FILETIME count, 100 ns ticks");
        return fs::file_time_type{fs::file_time_type::duration{ticksSince1601}};
    }
}

// A FILETIME as the library's file clock reads time.
[[nodiscard]] fs::file_time_type fileTimeOf(FILETIME const& ft) {
    ULARGE_INTEGER u{};
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return fileTimeFromTicks<fs::file_time_type::clock>(static_cast<long long>(u.QuadPart));
}

// A handle to what `p` REACHES (every link on the way followed), or none: nothing reached is no handle
// with `ec` clear.
[[nodiscard]] Handle reachedHandle(fs::path const& p, DWORD access, std::error_code& ec) {
    HANDLE const reached = ::CreateFileW(callSpelling(p).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (reached == INVALID_HANDLE_VALUE) {
        DWORD const e = ::GetLastError();
        if (!winNotFound(e)) ec = winError(e);
        return {};
    }
    return Handle{reached};
}

// A handle to the entry AT `p` itself -- a link there is NOT followed -- or none: nothing there is no handle with
// `ec` clear.
[[nodiscard]] Handle entryHandle(fs::path const& p, DWORD access, std::error_code& ec) {
    HANDLE const entry = ::CreateFileW(callSpelling(p).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                       nullptr);
    if (entry == INVALID_HANDLE_VALUE) {
        DWORD const e = ::GetLastError();
        if (!winNotFound(e)) ec = winError(e);
        return {};
    }
    return Handle{entry};
}

// Whether two open handles are one file. The 128-bit identity first (ReFS needs it), the 64-bit one where the file
// system has no other. An OS without the class (before Windows 8) refuses the call, and the 64-bit identity answers.
[[nodiscard]] bool sameIdentity(Handle const& first, Handle const& second, std::error_code& ec) {
    FILE_ID_INFO idA{}, idB{};
    if (::GetFileInformationByHandleEx(first.get(), FileIdInfo, &idA, sizeof idA)
        && ::GetFileInformationByHandleEx(second.get(), FileIdInfo, &idB, sizeof idB)) {
        return idA.VolumeSerialNumber == idB.VolumeSerialNumber
            && std::equal(std::begin(idA.FileId.Identifier), std::end(idA.FileId.Identifier),
                          std::begin(idB.FileId.Identifier));
    }
    BY_HANDLE_FILE_INFORMATION infoA{}, infoB{};
    if (!::GetFileInformationByHandle(first.get(), &infoA) || !::GetFileInformationByHandle(second.get(), &infoB)) {
        ec = winError(::GetLastError());
        return false;
    }
    return infoA.dwVolumeSerialNumber == infoB.dwVolumeSerialNumber && infoA.nFileIndexHigh == infoB.nFileIndexHigh
        && infoA.nFileIndexLow == infoB.nFileIndexLow;
}

// `FileCaseSensitiveInfo` (Windows 10 RS4), SPELLED BY VALUE because MinGW-w64's header gives the enumerator ANOTHER
// class's number: ✔MEASURED 2026-10-28 (probe-reference-cc, -D_WIN32_WINNT=0x0A00): MinGW-w64 g++ 13.2.0 declares it
// 21 -- the Windows SDK's FileDispositionInfoEx, which with flag 1 asks FILE_DISPOSITION_FLAG_DELETE -- and refuses
// it as a query with error 87 (run 20261029-003600-81043948); cl 19.51's SDK declares 23 (run
// 20261029-003902-59348b54), and through 23 both toolchains answer 0 for a new directory and 1 once it is made
// case-sensitive (runs 20261029-003836-60f9e5a7 and 20261029-003902-59348b54). Its record and flag are the headers' own on both (4 bytes;
// FILE_CS_FLAG_CASE_SENSITIVE_DIR 1).
constexpr auto kFileCaseSensitiveInfo = static_cast<FILE_INFO_BY_HANDLE_CLASS>(23);
#ifndef __MINGW32__
static_assert(FileCaseSensitiveInfo == kFileCaseSensitiveInfo);
#endif

#else

[[nodiscard]] EntryKind kindOfStatus(fs::file_type type) {
    switch (type) {
        case fs::file_type::not_found: return EntryKind::Absent;
        case fs::file_type::directory: return EntryKind::Directory;
        case fs::file_type::symlink: return EntryKind::Link;
        case fs::file_type::regular: return EntryKind::File;
        default: return EntryKind::Other;
    }
}

// Every entry of `dir`, each classified by its own `symlink_status` (the listing's cached type
// where the platform records one). The `ec` test comes BEFORE the end test: a failed increment may
// leave the iterator equal to the end, and a loop that asked "finished?" first would read a listing
// error as a complete listing.
void listEntries(fs::path const& dir, std::size_t depth, std::vector<TreeEntry>& out, std::error_code& ec) {
    ec.clear();
    fs::directory_iterator       it{dir, ec};
    fs::directory_iterator const end;
    while (true) {
        if (ec) return;
        if (it == end) return;
        std::error_code kec;
        auto const      type = it->symlink_status(kec).type();
        // Gone since it was listed is `Absent`; any other failure to read it ends the listing.
        if (kec && type != fs::file_type::not_found) {
            ec = kec;
            return;
        }
        EntryKind const kind = kindOfStatus(type);
        out.push_back(TreeEntry{it->path(), kind, kind, depth});
        it.increment(ec);
    }
}

// ── THE DESCRIPTOR-RELATIVE REMOVAL'S PIECES (the header states why the removal never goes by path) ──

// One owned descriptor.
class Descriptor {
public:
    Descriptor() = default;
    explicit Descriptor(int fd) : fd_(fd) {}
    Descriptor(Descriptor&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Descriptor& operator=(Descriptor&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    Descriptor(Descriptor const&)            = delete;
    Descriptor& operator=(Descriptor const&) = delete;
    ~Descriptor() { reset(); }
    [[nodiscard]] int  get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }
    void reset() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

// The flags a removal opens a directory with: a directory and NEVER a link (a link, or anything that is
// not a directory, refuses the open -- ELOOP or ENOTDIR), and never a wait on a FIFO.
constexpr int kOpenDirectoryOnly = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;

[[nodiscard]] std::error_code posixError(int e) {
    return std::error_code{e, std::generic_category()};
}

// Every name the directory open as `fd` holds but `.` and `..`, listed THROUGH the descriptor (a
// duplicate of it, which `fdopendir` takes), so the listing reads the directory that was opened.
[[nodiscard]] std::vector<std::string> namesThrough(int fd, int& error) {
    std::vector<std::string> names;
    int const listFd = ::dup(fd);
    if (listFd < 0) {
        error = errno;
        return names;
    }
    DIR* const stream = ::fdopendir(listFd);
    if (stream == nullptr) {
        error = errno;
        ::close(listFd);
        return names;
    }
    for (;;) {
        errno                    = 0;
        struct dirent const* ent = ::readdir(stream);
        if (ent == nullptr) {
            if (errno != 0) error = errno;
            break;
        }
        std::string_view const name{ent->d_name};
        if (name != "." && name != "..") names.emplace_back(name);
    }
    ::closedir(stream);
    return names;
}

#endif

// The removal's test seam (the header): empty in every build unless a test sets it.
[[nodiscard]] std::function<void(fs::path const&)>& removeTreeEntryHook() {
    static std::function<void(fs::path const&)> hook;
    return hook;
}

}  // namespace

// ════ THE OPERATIONS A SITE WOULD ASK OF std::filesystem, AT ANY LENGTH (the header states why Win32 on Windows) ════

std::FILE* openFileStream(fs::path const& p, char const* mode) {
#ifdef _WIN32
    std::wstring wideMode;
    for (char const* m = mode; *m != '\0'; ++m) wideMode += static_cast<wchar_t>(static_cast<unsigned char>(*m));
    return ::_wfopen(callSpelling(p).c_str(), wideMode.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

bool createDirectories(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    std::wstring const      spelled = callSpelling(p);
    std::wstring_view const s       = spelled;
    RootSpelling const      root    = rootSpelling(s, PathGrammar::Win32);
    // Every level below the volume root, shortest first: a directory there is passed, a missing one made. A spelling
    // with no extended form (a reserved device name in it) is walked as written: the OS answers for it as it would.
    bool const  literal = root.kind == RootSpelling::Kind::Verbatim;
    bool        made    = false;
    std::size_t at      = std::min(root.volume, s.size());
    while (at < s.size()) {
        std::size_t const end = segmentEnd(s, at, PathGrammar::Win32, literal);
        if (end > at) {
            std::wstring const level{s.substr(0, end)};
            DWORD const        attributes = ::GetFileAttributesW(level.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                if (!::CreateDirectoryW(level.c_str(), nullptr)) {
                    DWORD const e = ::GetLastError();
                    DWORD const now = ::GetFileAttributesW(level.c_str());   // made meanwhile by another hand?
                    if (e != ERROR_ALREADY_EXISTS || now == INVALID_FILE_ATTRIBUTES
                        || (now & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                        ec = winError(e);
                        return made;
                    }
                } else {
                    made = true;
                }
            } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                ec = winError(ERROR_ALREADY_EXISTS);   // a level is a file: what the library's EEXIST says
                return made;
            }
        }
        at = end + 1;
    }
    return made;
#else
    return fs::create_directories(p, ec);
#endif
}

bool removeEntry(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    // The entry itself (a link is opened as the link), deleted through its handle -- the removal `removeTree` makes of
    // each entry, POSIX semantics where the OS has them and a read-only file deleted as MS STL's `remove` deletes one.
    Handle const entry = entryHandle(p, kRemovalAccess, ec);
    if (!entry) return false;   // nothing there (`ec` clear), or the open's refusal (`ec` set)
    if (DWORD const e = deleteThrough(entry.get()); e != ERROR_SUCCESS) {
        ec = winError(e);
        return false;
    }
    return true;
#else
    return fs::remove(p, ec);
#endif
}

void copyFile(fs::path const& from, fs::path const& to, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    if (!::CopyFileW(callSpelling(from).c_str(), callSpelling(to).c_str(), /*bFailIfExists=*/TRUE))
        ec = winError(::GetLastError());
#else
    static_cast<void>(fs::copy_file(from, to, fs::copy_options::none, ec));
#endif
}

EntryKind entryKind(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    std::wstring const spelled = callSpelling(p);
    if (!::GetFileAttributesExW(spelled.c_str(), GetFileExInfoStandard, &data)) {
        DWORD const e = ::GetLastError();
        if (winNotFound(e)) return EntryKind::Absent;
        ec = winError(e);
        return EntryKind::Other;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return kindOfAttributes(data.dwFileAttributes, 0);
    // A reparse point: its TAG decides, read from a handle to the entry itself (never what it reaches).
    HANDLE const entry = ::CreateFileW(spelled.c_str(), FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (entry == INVALID_HANDLE_VALUE) {
        DWORD const e = ::GetLastError();
        if (winNotFound(e)) return EntryKind::Absent;
        ec = winError(e);
        return EntryKind::Other;
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    BOOL const  read = ::GetFileInformationByHandleEx(entry, FileAttributeTagInfo, &info, sizeof info);
    DWORD const e    = read ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(entry);
    if (!read) {
        ec = winError(e);
        return EntryKind::Other;
    }
    return kindOfAttributes(info.FileAttributes, info.ReparseTag);
#else
    auto const type = fs::symlink_status(p, ec).type();
    if (type == fs::file_type::not_found) {
        ec.clear();
        return EntryKind::Absent;
    }
    return ec ? EntryKind::Other : kindOfStatus(type);
#endif
}

EntryKind reachedKind(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    auto const info = reachedInformation(p, ec);
    if (!info) return EntryKind::Absent;
    return (info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? EntryKind::Directory : EntryKind::File;
#else
    auto const type = fs::status(p, ec).type();
    if (type == fs::file_type::not_found) {
        ec.clear();
        return EntryKind::Absent;
    }
    if (ec) return EntryKind::Absent;
    return type == fs::file_type::directory ? EntryKind::Directory
         : type == fs::file_type::regular   ? EntryKind::File
                                            : EntryKind::Other;
#endif
}

bool reachedPipe(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    static_cast<void>(p);
    return false;
#else
    auto const type = fs::status(p, ec).type();
    if (type == fs::file_type::not_found) {
        ec.clear();
        return false;
    }
    return !ec && type == fs::file_type::fifo;
#endif
}

std::optional<std::uintmax_t> reachedFileSize(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    auto const info = reachedInformation(p, ec);
    if (!info || (info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return std::nullopt;
    return (std::uintmax_t{info->nFileSizeHigh} << 32) | info->nFileSizeLow;
#else
    auto const reached = fs::status(p, ec).type();
    if (reached == fs::file_type::not_found) {
        ec.clear();
        return std::nullopt;
    }
    if (ec || reached != fs::file_type::regular) return std::nullopt;
    auto const size = fs::file_size(p, ec);
    if (ec) return std::nullopt;
    return size;
#endif
}

fs::path reachedPath(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    // Absolute and normal WITHOUT re-rooting an authority onto the local drive (the header measures
    // what `fs::absolute` and `lexically_normal` do to a multi-separator root).
    fs::path const whole = normalizeKeepingRoot(absoluteKeepingRoot(p, ec));
    if (ec) return {};
    // The longest leading part that exists, resolved by the OS; what follows it is appended as written.
    // ★ THE WALK STOPS AT THE VOLUME ROOT, WITH ITS SEPARATOR (P69 H6): `rootSpelling` parses where it ends, for every
    // Win32 spelling. The library's parent of `\\?\Volume{G}\x`, `\\?\C:\x` or `\\?\GLOBALROOT\Device\HarddiskVolumeN\x`
    // is the volume WITHOUT its separator, which the OS opens as the volume's DEVICE -- a handle with no file
    // information and no final path, ERROR_INVALID_FUNCTION (✔MEASURED 2026-11-03, probe-reference-cc run
    // 20261104-021850-b279d3e9, MinGW's library and MS STL's alike); the walk never asks it. A path asked AS the device
    // (`\\?\C:`, nothing walked) is asked as written, and stays refused. A verbatim spelling is cut at `\` alone.
    std::wstring const&     w    = whole.native();
    std::wstring_view const s    = w;
    RootSpelling const      root = rootSpelling(s, PathGrammar::Win32);
    std::size_t             headEnd   = stepEnd(s, root, PathGrammar::Win32);   // a trailing separator names nothing
    std::size_t const       restEnd   = headEnd;   // the rest, `w[restBegin, restEnd)`, follows the head as written
    std::size_t             restBegin = headEnd;
    for (;;) {
        std::wstring const head    = w.substr(0, headEnd);
        HANDLE const       reached = ::CreateFileW(callSpelling(fs::path{head}).c_str(), 0,
                                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (reached != INVALID_HANDLE_VALUE) {
            std::wstring resolved(MAX_PATH, L'\0');
            DWORD        n = ::GetFinalPathNameByHandleW(reached, resolved.data(), static_cast<DWORD>(resolved.size()),
                                                         FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (n >= resolved.size()) {
                resolved.resize(n);
                n = ::GetFinalPathNameByHandleW(reached, resolved.data(), static_cast<DWORD>(resolved.size()),
                                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            }
            DWORD e = n == 0 ? ::GetLastError() : ERROR_SUCCESS;
            // A volume with NO DOS name refuses its DOS spelling: its NT name, as MS STL's `canonical`
            // answers it (the header measures both libraries), `\\?\GLOBALROOT` making it a Win32 path.
            bool const ntName = n == 0 && e == ERROR_PATH_NOT_FOUND;
            if (ntName) {
                resolved.assign(MAX_PATH, L'\0');
                n = ::GetFinalPathNameByHandleW(reached, resolved.data(), static_cast<DWORD>(resolved.size()),
                                                FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
                if (n >= resolved.size()) {
                    resolved.resize(n);
                    n = ::GetFinalPathNameByHandleW(reached, resolved.data(), static_cast<DWORD>(resolved.size()),
                                                    FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
                }
                e = n == 0 ? ::GetLastError() : ERROR_SUCCESS;
            }
            ::CloseHandle(reached);
            if (n == 0 || n >= resolved.size()) {
                ec = winError(n == 0 ? e : ERROR_INSUFFICIENT_BUFFER);
                return {};
            }
            resolved.resize(n);
            // The final path is spelled in the OS's own long form: `\\?\C:\...` or `\\?\UNC\server\share\...`
            // -- or, by NT name, `\Device\...`, which `\\?\GLOBALROOT` makes a Win32 path. The rest follows it as
            // written, and the whole is spelled in the DOS form only where that form names the same file.
            if (ntName) resolved = L"\\\\?\\GLOBALROOT" + resolved;
            if (restBegin < restEnd) {
                if (resolved.back() != L'\\') resolved += L'\\';
                resolved.append(w, restBegin, restEnd - restBegin);
            }
            return fs::path{dosSpellingOf(resolved)};
        }
        DWORD const e = ::GetLastError();
        if (!winNotFound(e)) {
            ec = winError(e);
            return {};
        }
        std::size_t const parent = parentEnd(s, headEnd, root, PathGrammar::Win32);
        if (parent == std::wstring_view::npos) return whole;   // nothing of it exists, its volume root included
        restBegin = nameBegin(s, parent, root, PathGrammar::Win32);
        headEnd   = parent;
    }
#else
    return fs::weakly_canonical(p, ec);
#endif
}

fs::path reachedRelative(fs::path const& p, fs::path const& base, std::error_code& ec) {
    fs::path const to = reachedPath(p, ec);
    if (ec) return {};
    fs::path const from = reachedPath(base, ec);
    if (ec) return {};
    // Another root is decided by `rootSpelling`, never by the library's `root_name()` (MinGW's is empty for every
    // share): the two volume roots, spelled with one separator and, on Win32, in one ASCII case, must be one.
    RootSpelling const toRoot   = rootSpelling(to);
    RootSpelling const fromRoot = rootSpelling(from);
    auto const         folded   = [](fs::path::string_type s) {
        for (auto& c : s) {
            if (separatesIn(c, kHostPathGrammar, false)) c = fs::path::preferred_separator;
            if constexpr (kHostPathGrammar == PathGrammar::Win32) c = asciiLower(c);
        }
        return s;
    };
    if (toRoot.kind != fromRoot.kind
        || folded(to.native().substr(0, toRoot.volume)) != folded(from.native().substr(0, fromRoot.volume)))
        return {};
    return fs::path{to.native().substr(toRoot.volume)}.lexically_relative(
        fs::path{from.native().substr(fromRoot.volume)});
}

std::optional<fs::file_time_type> reachedWriteTime(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    Handle const reached = reachedHandle(p, FILE_READ_ATTRIBUTES, ec);
    if (!reached) return std::nullopt;
    FILETIME written{};
    if (!::GetFileTime(reached.get(), nullptr, nullptr, &written)) {
        ec = winError(::GetLastError());
        return std::nullopt;
    }
    return fileTimeOf(written);
#else
    auto const t = fs::last_write_time(p, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) ec.clear();
        return std::nullopt;
    }
    return t;
#endif
}

bool reachedSameFile(fs::path const& a, fs::path const& b, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    Handle const first = reachedHandle(a, FILE_READ_ATTRIBUTES, ec);
    if (!first) return false;
    Handle const second = reachedHandle(b, FILE_READ_ATTRIBUTES, ec);
    if (!second) return false;
    return sameIdentity(first, second, ec);
#else
    for (fs::path const* side : {&a, &b}) {
        auto const reached = fs::status(*side, ec).type();
        if (reached == fs::file_type::not_found) {
            ec.clear();
            return false;
        }
        if (ec) return false;
    }
    bool const same = fs::equivalent(a, b, ec);
    return !ec && same;
#endif
}

void addReachedPermissions(fs::path const& p, fs::perms add, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    Handle const reached = reachedHandle(p, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, ec);
    if (!reached) {
        if (!ec) ec = winError(ERROR_FILE_NOT_FOUND);
        return;
    }
    // Windows records one permission, the read-only attribute: a write bit added clears it, as MS STL
    // does; the other bits have nothing to set.
    if ((add & (fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write)) == fs::perms::none) return;
    FILE_BASIC_INFO basic{};
    if (!::GetFileInformationByHandleEx(reached.get(), FileBasicInfo, &basic, sizeof basic)) {
        ec = winError(::GetLastError());
        return;
    }
    if ((basic.FileAttributes & FILE_ATTRIBUTE_READONLY) == 0) return;
    basic.FileAttributes &= ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
    if (basic.FileAttributes == 0) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (!::SetFileInformationByHandle(reached.get(), FileBasicInfo, &basic, sizeof basic))
        ec = winError(::GetLastError());
#else
    fs::permissions(p, add, fs::perm_options::add, ec);   // follows every link (POSIX `chmod`)
#endif
}

bool sameEntry(fs::path const& a, fs::path const& b, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    Handle const first = entryHandle(a, FILE_READ_ATTRIBUTES, ec);
    if (!first) return false;
    Handle const second = entryHandle(b, FILE_READ_ATTRIBUTES, ec);
    if (!second) return false;
    return sameIdentity(first, second, ec);
#else
    // `lstat`: the entry itself. Nothing there (or a component that is no directory) is no entry.
    auto const look = [&ec](fs::path const& p, struct stat& st) {
        if (::lstat(p.c_str(), &st) == 0) return true;
        int const e = errno;
        if (e != ENOENT && e != ENOTDIR) ec = std::error_code{e, std::generic_category()};
        return false;
    };
    struct stat sa{};
    struct stat sb{};
    if (!look(a, sa) || !look(b, sb)) return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif
}

std::optional<fs::path::string_type> storedEntryName(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    Handle const entry = entryHandle(p, FILE_READ_ATTRIBUTES, ec);
    if (!entry) return std::nullopt;
    // By the NT volume name: only the last component is read, and a volume with no DOS name refuses its DOS
    // spelling (`reachedPath`).
    std::wstring name(MAX_PATH, L'\0');
    DWORD        n = ::GetFinalPathNameByHandleW(entry.get(), name.data(), static_cast<DWORD>(name.size()),
                                                 FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    if (n >= name.size()) {
        name.resize(n);
        n = ::GetFinalPathNameByHandleW(entry.get(), name.data(), static_cast<DWORD>(name.size()),
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    }
    if (n == 0 || n >= name.size()) {
        ec = winError(n == 0 ? ::GetLastError() : ERROR_INSUFFICIENT_BUFFER);
        return std::nullopt;
    }
    name.resize(n);
    std::size_t const cut = name.find_last_of(L'\\');
    return cut == std::wstring::npos ? name : name.substr(cut + 1);
#elif defined(__APPLE__)
    // Asked of the PATH, the last component not followed -- no descriptor, so an entry whose open is refused still
    // answers (the header's table). The reply: its length, the name's reference (its offset counted from the
    // reference itself), then the name, NUL-terminated UTF-8 of at most NAME_MAX characters of up to three bytes.
    struct attrlist request{};
    request.bitmapcount = ATTR_BIT_MAP_COUNT;
    request.commonattr  = ATTR_CMN_NAME;
    alignas(4) char reply[sizeof(std::uint32_t) + sizeof(attrreference_t) + NAME_MAX * 3 + 1] = {};
    if (::getattrlist(p.c_str(), &request, reply, sizeof reply, FSOPT_NOFOLLOW) != 0) {
        int const e = errno;
        if (e != ENOENT && e != ENOTDIR) ec = std::error_code{e, std::generic_category()};
        return std::nullopt;
    }
    attrreference_t ref{};
    std::memcpy(&ref, reply + sizeof(std::uint32_t), sizeof ref);
    std::size_t const at = sizeof(std::uint32_t) + static_cast<std::size_t>(ref.attr_dataoffset);
    if (ref.attr_dataoffset < 0 || at >= sizeof reply) {
        ec = std::make_error_code(std::errc::bad_message);
        return std::nullopt;
    }
    char const* const name = reply + at;
    return std::string{name, ::strnlen(name, sizeof reply - at)};
#else
    static_cast<void>(p);
    return std::nullopt;
#endif
}

std::optional<bool> directoryFoldsCase(fs::path const& dir, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    Handle const reached = reachedHandle(dir, FILE_READ_ATTRIBUTES, ec);
    if (!reached) {
        if (!ec) ec = winError(ERROR_PATH_NOT_FOUND);
        return std::nullopt;
    }
    FILE_CASE_SENSITIVE_INFO info{};
    if (!::GetFileInformationByHandleEx(reached.get(), kFileCaseSensitiveInfo, &info, sizeof info)) {
        ec = winError(::GetLastError());
        return std::nullopt;
    }
    return (info.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) == 0;
#elif defined(__APPLE__)
    errno = 0;
    long const sensitive = ::pathconf(dir.c_str(), _PC_CASE_SENSITIVE);
    if (sensitive < 0) {
        if (errno != 0) ec = std::error_code{errno, std::generic_category()};
        return std::nullopt;
    }
    return sensitive == 0;
#else
    static_cast<void>(dir);
    return std::nullopt;
#endif
}

TreeWalk walkTree(fs::path const& root, std::function<WalkStep(TreeEntry const&)> const& visit,
                  TreeWalkOptions options) {
    TreeWalk walk;
    // One listed directory: its entries, and how many of them the walk has handed out.
    struct Listed {
        std::vector<TreeEntry> entries;
        std::size_t            next = 0;
    };
    std::vector<Listed> work;
    // List `dir` onto the stack; false ends the walk (its error recorded).
    auto const descend = [&](fs::path const& dir, std::size_t depth) {
        Listed          listed;
        std::error_code ec;
        listEntries(dir, depth, listed.entries, ec);
        if (ec) {
            walk.error = ec;
            walk.where = dir;
            return false;
        }
        for (auto& entry : listed.entries) {
            if (entry.kind != EntryKind::Link) continue;
            // A link whose reach cannot be examined reaches nothing usable, and the walk does not know
            // what that entry is: it ends here naming the link, unless the visitor asked to answer it.
            entry.reaches = reachedKind(entry.path, entry.reachError);
            if (entry.reachError && !options.handOutUnreadableLinks) {
                walk.error = entry.reachError;
                walk.where = entry.path;
                return false;
            }
        }
        work.push_back(std::move(listed));
        return true;
    };
    if (!descend(root, 0)) return walk;
    while (!work.empty()) {
        Listed& top = work.back();
        if (top.next == top.entries.size()) {
            work.pop_back();
            continue;
        }
        // A COPY: descending grows `work`, which would leave a reference into it dangling.
        TreeEntry const entry = top.entries[top.next++];
        WalkStep const  step  = visit(entry);
        if (step == WalkStep::Stop) {
            walk.stopped = true;
            return walk;
        }
        if (step == WalkStep::Prune || entry.kind != EntryKind::Directory) continue;
        if (!descend(entry.path, entry.depth + 1)) return walk;
    }
    return walk;
}

TreeRemoval removeTree(fs::path const& target, RemovalOptions const& options) {
    TreeRemoval out;
    // The options' last entry (the header): moved to the end of the TARGET's listing, and handled only
    // when every entry before it was removed -- otherwise the removal ends, leaving it and the target.
    auto const isLastEntry = [&options](fs::path::string_type const& name) {
        return !options.lastEntry.empty() && name == options.lastEntry.native();
    };
    auto const moveLastEntryLast = [&isLastEntry](std::vector<fs::path::string_type>& names) {
        auto const at = std::find_if(names.begin(), names.end(), isLastEntry);
        if (at != names.end()) std::rotate(at, at + 1, names.end());
    };
    // A failure on an entry that is now GONE was beaten to the outcome wanted: it is no failure.
    auto const note = [&out](fs::path const& p, std::error_code const& ec) {
        std::error_code gec;
        if (entryKind(p, gec) == EntryKind::Absent && !gec) return;
        if (out.failures++ == 0) {
            out.firstPath  = p;
            out.firstError = ec;
        }
    };
    auto const& hook = removeTreeEntryHook();
    // An explicit work stack of OPEN directories: each is listed once, through its own handle, its
    // entries handled one by one -- each opened RELATIVE to it, never following a link, and judged by
    // what was opened -- and it is deleted once every entry of that listing has been. A directory's
    // handle stays open while its entries are handled, so every relative open names an entry of the
    // directory that was listed, whatever its path names by then.
#ifdef _WIN32
    struct Open {
        Handle                    dir;
        fs::path                  path;
        std::vector<std::wstring> names;
        std::size_t               next   = 0;
        bool                      listed = false;
    };
    // The target itself, opened as the entry it is (a link is opened as the link) -- a directory by ONE
    // opening that both lists and deletes it. An entry that refuses the listing access is opened again for
    // its deletion alone (a file this process may delete but not read, or one another process holds
    // without sharing reads); a DIRECTORY opened only so cannot be listed, and that refusal is the failure.
    std::wstring const spelledTarget = callSpelling(target);
    auto const openTarget = [&spelledTarget](ACCESS_MASK access, DWORD& error) {
        HANDLE const h = ::CreateFileW(spelledTarget.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        error = h == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
        return Handle{h};
    };
    DWORD  listingRefusal = ERROR_SUCCESS;
    Handle root           = openTarget(kDirectoryRemovalAccess, listingRefusal);
    if (!root) {
        if (winNotFound(listingRefusal)) return out;
        DWORD again = ERROR_SUCCESS;
        root        = openTarget(kRemovalAccess, again);
        if (!root) {
            if (!winNotFound(again)) note(target, winError(again));
            return out;
        }
    }
    DWORD     kindError = ERROR_SUCCESS;
    EntryKind kind      = kindOfHandle(root.get(), kindError);
    if (kindError != ERROR_SUCCESS) {
        note(target, winError(kindError));
        return out;
    }
    if (kind != EntryKind::Directory) {
        if (DWORD const e = deleteThrough(root.get()); e != ERROR_SUCCESS) note(target, winError(e));
        return out;
    }
    if (listingRefusal != ERROR_SUCCESS) {
        note(target, winError(listingRefusal));
        return out;
    }
    std::vector<Open> work;
    work.push_back(Open{std::move(root), target, {}, 0, false});
    while (!work.empty()) {
        Open& top = work.back();
        if (!top.listed) {
            top.listed      = true;
            DWORD listError = ERROR_SUCCESS;
            top.names       = namesThrough(top.dir.get(), listError);
            if (listError != ERROR_SUCCESS) note(top.path, winError(listError));
            if (work.size() == 1) moveLastEntryLast(top.names);   // the target's own listing
            continue;
        }
        if (top.next == top.names.size()) {
            // Every entry of its listing handled: the directory itself, through its own handle.
            DWORD const    e    = deleteThrough(top.dir.get());
            fs::path const path = top.path;
            work.pop_back();   // closes the handle: a classic disposition deletes it now
            if (e != ERROR_SUCCESS) note(path, winError(e));
            continue;
        }
        std::wstring const name  = top.names[top.next++];
        if (work.size() == 1 && out.failures != 0 && isLastEntry(name)) return out;   // it stays, and the target
        fs::path const     entry = top.path / name;
        if (hook) hook(entry);
        // A directory is opened ONCE for its listing and its deletion (a link to a directory meets that open
        // as the link, and is judged a link below); an entry that is no directory refuses it, and is opened
        // for its deletion alone.
        DWORD  openError = ERROR_SUCCESS;
        Handle opened    = openEntryRelative(top.dir.get(), name, kDirectoryRemovalAccess, FILE_DIRECTORY_FILE, openError);
        if (!opened && openError == ERROR_DIRECTORY)
            opened = openEntryRelative(top.dir.get(), name, kRemovalAccess, FILE_NON_DIRECTORY_FILE, openError);
        if (!opened) {
            if (!winNotFound(openError)) note(entry, winError(openError));   // gone already is no failure
            continue;
        }
        kindError = ERROR_SUCCESS;
        kind      = kindOfHandle(opened.get(), kindError);
        if (kindError != ERROR_SUCCESS) {
            note(entry, winError(kindError));
            continue;
        }
        if (kind == EntryKind::Directory) {
            work.push_back(Open{std::move(opened), entry, {}, 0, false});   // `top` is not used past here
            continue;
        }
        // A file, or a link -- the link itself, which is what was opened.
        if (DWORD const e = deleteThrough(opened.get()); e != ERROR_SUCCESS) note(entry, winError(e));
    }
#else
    struct Open {
        Descriptor               dir;
        fs::path                 path;
        std::vector<std::string> names;
        std::size_t              next   = 0;
        bool                     listed = false;
    };
    // The target itself: a directory opened as one, or -- refused as a link or as no directory -- the
    // entry removed as itself (`unlink` never follows a link).
    int const first = ::open(target.c_str(), kOpenDirectoryOnly);
    if (first < 0) {
        int const e = errno;
        if (e == ENOENT) return out;
        if (e == ELOOP || e == ENOTDIR) {
            if (::unlink(target.c_str()) != 0 && errno != ENOENT) note(target, posixError(errno));
            return out;
        }
        note(target, posixError(e));
        return out;
    }
    std::vector<Open> work;
    work.push_back(Open{Descriptor{first}, target, {}, 0, false});
    while (!work.empty()) {
        Open& top = work.back();
        if (!top.listed) {
            top.listed    = true;
            int listError = 0;
            top.names     = namesThrough(top.dir.get(), listError);
            if (listError != 0) note(top.path, posixError(listError));
            if (work.size() == 1) moveLastEntryLast(top.names);   // the target's own listing
            continue;
        }
        if (top.next == top.names.size()) {
            // Every entry of its listing handled: the directory itself, relative to its parent's
            // descriptor -- the root by its path (`rmdir` never follows a link).
            fs::path const path = top.path;
            work.pop_back();   // closes its descriptor
            int const rc = work.empty() ? ::rmdir(path.c_str())
                                        : ::unlinkat(work.back().dir.get(), path.filename().c_str(), AT_REMOVEDIR);
            if (rc != 0 && errno != ENOENT) note(path, posixError(errno));
            continue;
        }
        std::string const name  = top.names[top.next++];
        if (work.size() == 1 && out.failures != 0 && isLastEntry(name)) return out;   // it stays, and the target
        fs::path const    entry = top.path / name;
        if (hook) hook(entry);
        int const opened = ::openat(top.dir.get(), name.c_str(), kOpenDirectoryOnly);
        if (opened >= 0) {
            work.push_back(Open{Descriptor{opened}, entry, {}, 0, false});   // `top` is not used past here
            continue;
        }
        int const e = errno;
        if (e == ENOENT) continue;   // gone already is no failure
        if (e != ELOOP && e != ENOTDIR) {
            note(entry, posixError(e));
            continue;
        }
        // A file, a link (the link itself), a FIFO, a device: removed as itself.
        if (::unlinkat(top.dir.get(), name.c_str(), 0) != 0 && errno != ENOENT) note(entry, posixError(errno));
    }
#endif
    return out;
}

namespace substrate_testing {
void setRemoveTreeEntryHook(std::function<void(fs::path const&)> hook) {
    removeTreeEntryHook() = std::move(hook);
}
}  // namespace substrate_testing

}  // namespace dss::core
