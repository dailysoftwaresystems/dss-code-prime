#include "core/substrate/path_identity.hpp"

#include <algorithm>
#include <system_error>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace dss::core {
namespace {

namespace fs = std::filesystem;

#ifdef _WIN32

// `GetLongPathNameW` for ONE path, or empty when Windows will not answer.
//
// ★ WHY THIS CALL AND NOT `GetFinalPathNameByHandleW`. Both expand 8.3, but the
// handle-based one ALSO rewrites junctions and mount points and prefixes `\\?\`.
// These keys are compared as strings and some of them are printed to operators,
// so the minimal transformation is the correct one: expand short components,
// touch nothing else.
[[nodiscard]] std::wstring longNameOrEmpty(std::wstring const& in) {
    if (in.empty()) return {};
    DWORD const n = ::GetLongPathNameW(in.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring out(n, L'\0');
    DWORD const written = ::GetLongPathNameW(in.c_str(), out.data(), n);
    // `written >= n` means the path changed under us between the two calls.
    if (written == 0 || written >= n) return {};
    out.resize(written);
    return out;
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
[[nodiscard]] std::wstring expandShortComponents(std::wstring const& in) {
    if (auto const whole = longNameOrEmpty(in); !whole.empty()) return whole;

    fs::path const p{in};
    fs::path       head = p;
    fs::path       tail;
    // Walk up until Windows answers for the head. `parent_path()` of a root is
    // the root itself, so compare against the previous value to terminate —
    // `has_parent_path()` alone would spin forever on `C:\`.
    while (true) {
        fs::path const parent = head.parent_path();
        if (parent.empty() || parent == head) return in;  // nothing existed
        // ⚠ NOT `head.filename() / tail` UNCONDITIONALLY: `operator/` with an
        // EMPTY right operand APPENDS A SEPARATOR, so on the first turn of this
        // loop the tail became `notyet.o\` and every key built through this walk
        // carried a trailing separator that the whole-path fast path above does
        // not produce. ✔MEASURED, ONE path, two keys, differing only in whether
        // the file was there yet:
        //     before it exists : 'C:/.../idtmp/notyet.o/'
        //     after  it exists : 'C:/.../idtmp/notyet.o'
        // That is precisely the two-keys-for-one-file defect this type exists to
        // prevent, on the not-yet-created inputs the header's own note calls a
        // FIRST-CLASS case (output artifacts, a `path` dependency resolved
        // before its directory is confirmed, a cycle key needed for the reject's
        // own message).
        tail = tail.empty() ? head.filename() : (head.filename() / tail);
        head = parent;
        if (auto const expanded = longNameOrEmpty(head.wstring());
            !expanded.empty()) {
            fs::path out{expanded};
            out /= tail;
            return out.wstring();
        }
    }
}

#endif  // _WIN32

[[nodiscard]] constexpr bool isSeparator(fs::path::value_type c) {
    return c == fs::path::preferred_separator
        || c == fs::path::value_type{'/'};
}

// How many separators does this spelling START with? A run of 2 or more is the
// only place a "redundant" separator carries meaning, and both transforms below
// are about not destroying it.
[[nodiscard]] std::size_t leadingSeparatorRun(fs::path::string_type const& s) {
    std::size_t n = 0;
    while (n < s.size() && isSeparator(s[n])) ++n;
    return n;
}

}  // namespace

// `lexically_normal()` WITHOUT letting it eat the leading separator RUN.
//
// ★★★ ✔MEASURED, and it is the same loss `generic_string()` performs one step
// later, so the two together destroyed a UNC path twice over
// ([[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]). Printed with `.string()`,
// so no print-side transform can be blamed:
//     input            '\\wsl.localhost\Ubuntu\home\rafael\p\uncprobe.h'
//     lexically_normal '\wsl.localhost\Ubuntu\home\rafael\p\uncprobe.h'
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
// ⚠ A run of 0 or 1 is untouched -- a drive path and a root-relative path both
// take `lexically_normal()` exactly as before, so this changes no local key.
fs::path normalizeKeepingRoot(fs::path const& p) {
    fs::path::string_type const& s    = p.native();
    std::size_t const            lead = leadingSeparatorRun(s);
    if (lead < 2) return p.lexically_normal();
    fs::path::string_type out(lead, fs::path::preferred_separator);
    out += fs::path{s.substr(lead)}.lexically_normal().native();
    return fs::path{out};
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
    // A multi-separator prefix names an AUTHORITY, not a location on the current
    // drive, so there is nothing here to make absolute. See the header for the
    // measurement: `fs::absolute` does not merely fail on these, it succeeds
    // having re-rooted the path onto the local drive.
    if (leadingSeparatorRun(p.native()) >= 2) {
        ec.clear();
        return p;
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
    std::error_code canonEc;
    fs::path const c = fs::weakly_canonical(p, canonEc);
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
        // answers at all, whatever the answer, it did not refuse.
        std::error_code probe;
        static_cast<void>(fs::exists(p, probe));
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
    // exposure.) A canonicalisation that changed the leading separator RUN
    // changed which machine the path names, so it is refused here and the
    // lexical normalisation -- which cannot relocate anything -- is used.
    bool const relocated = leadingSeparatorRun(p.native()) >= 2
                        && leadingSeparatorRun(c.native())
                               != leadingSeparatorRun(p.native());
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

}  // namespace dss::core
