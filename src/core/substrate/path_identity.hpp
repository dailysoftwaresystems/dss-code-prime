#pragma once

#include "core/export.hpp"

#include <compare>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

// PATH IDENTITY — the one value that every spelling of one file reduces to.
//
// ★★★ WHY THIS IS A TYPE AND NOT A FUNCTION. A canonicalizing FUNCTION returning
// `fs::path` or `std::string` produces a value INDISTINGUISHABLE from a raw one,
// so every map keyed on it is one careless `insert(path.string())` away from
// silently holding two entries for one file. The only thing standing between
// correct and broken is then a lint, and a lint knows only the spellings it was
// taught: `fs::canonical`, `fs::absolute`, `GetFullPathNameW`, `_fullpath`, or a
// hand-rolled string compare all sail past a grep for `weakly_canonical`.
// ⇒ `PathIdentity` is constructible ONLY through `of()`. Converting OUT to a
// path for I/O or for a diagnostic stays free; construction IN is the one gate.
// A raw `fs::path` does not compile as a key of these maps — anywhere, forever,
// with no enumeration to maintain.
//
// ★★★ THE DEFECT THAT MADE THIS NECESSARY, MEASURED 2026-08-18. Every path
// identity in the compiler keyed on `fs::weakly_canonical`, which under
// libstdc++ (the toolchain this repo's Windows build actually uses) returns an
// 8.3 SHORT NAME UNCHANGED and WITH NO ERROR:
//
//     input            : C:\Users\rafae\AppData\Local\Temp\DSS-SC~1
//     exists           : yes
//     weakly_canonical : C:\Users\rafae\AppData\Local\Temp\DSS-SC~1   ec: <none>
//     canonical        : C:\Users\rafae\AppData\Local\Temp\DSS-SC~1   ec: <none>
//
// libstdc++ resolves `.`/`..` and symlinks and has no concept of an 8.3 alias,
// so two spellings of ONE directory survived as TWO keys. The MSVC STL happens
// to normalize them (`GetFinalPathNameByHandleW` under the hood) — which is why
// the CI failure this closes appeared fixed: the property was never HELD, only
// accidentally satisfied by one toolchain. The consequences were live and each
// is a known-bad class: a header reached through both spellings would be
// included TWICE past its `#pragma once` (include-stack re-entry keys on this),
// and one source listed both ways would produce a duplicate CU and a
// duplicate-symbol link error naming no manifest.
//
// ★★ AGNOSTICISM. The `#ifdef _WIN32` below is HOST filesystem behaviour, not
// source-language / target-CPU / object-format identity — the axis the bar
// governs. It sits beside `process_spawn.cpp` and `large_stack_call.cpp` for
// exactly that reason: these are the host-OS shims, and nothing here reads what
// is being COMPILED.

namespace dss::core {

// The GENERIC (forward-slash) spelling of `p` -- one separator convention for
// every path this compiler compares or prints as a key.
//
// ★★★ THIS EXISTS BECAUSE `path::generic_string()` IS LOSSY AND THE LOSS
// SILENTLY RENAMES THE FILE ([[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]).
// ✔MEASURED with the toolchain that builds DSS (libstdc++ 13.2, MinGW), all
// three printed from the SAME path object:
//     .string()          '\\wsl.localhost\Ubuntu\home\rafael\p\uncprobe.h'
//     .generic_string()  '/wsl.localhost/Ubuntu/home/rafael/p/uncprobe.h'
//     exists()           true
// The leading separator RUN is collapsed to one, so the authority is demoted to
// an ordinary component and the result names a path on the local drive root
// instead. A per-character substitution of the model's OWN
// `preferred_separator` is the same normalisation without the loss.
//
// ★ NO PLATFORM MACRO AND NO UNC SPELLING. The `if constexpr` reads the path
// model's declared separator, so on a host whose separator already is `/` the
// substitution compiles away entirely -- and it must be that character and not
// a literal backslash, because a POSIX filename may legitimately CONTAIN one.
[[nodiscard]] DSS_EXPORT std::string
genericSpelling(std::filesystem::path const& p);

// The same spelling as UTF-8, for the one caller that cannot use the narrow
// form: a diagnostic that must NAME a file whose characters the active code
// page cannot represent.
//
// ★★★ WHY THIS IS OWED SEPARATELY RATHER THAN LEFT AS `generic_u8string()`.
// The two losses are INDEPENDENT and each has already been measured, so a
// caller can only avoid both by having a transform that avoids both.
// `generic_string()` throws on a name the code page cannot encode
// ([[D-PP-HEADER-CASE-NON-ASCII-NAME-NARROWING-THROW]]), and `u8` was the
// repair; but `generic_u8string()` performs the SAME generic-format conversion
// and so eats the leading separator run exactly as its narrow sibling does.
// ✔MEASURED 2026-08-28 on a REACHABLE UNC directory (`exists()` true), one path
// object, both renderings printed side by side:
//     .string()          '//localhost/C$/Source/DailySoftware'   run 2
//     .generic_string()  '/localhost/C$/Source/DailySoftware'    run 1
//     .generic_u8string()'/localhost/C$/Source/DailySoftware'    run 1
// A header-collision report that renames the machine is the same lie as one
// that cannot spell the filename; escaping one and not the other would have
// left the report wrong in the case it was rewritten for.
//
// ⚠ THROWS EXACTLY WHERE `u8string()` DOES, WHICH IS THE POINT. A native name
// can be text no encoding accepts (NTFS permits lone surrogates) and the caller
// owns a code-unit-by-code-unit fallback for it; swallowing the throw here
// would take that fallback away and hand back a name that is not the file's.
[[nodiscard]] DSS_EXPORT std::u8string
genericSpellingU8(std::filesystem::path const& p);

// `fs::absolute` WITHOUT letting it invent a drive for a path that already names
// an AUTHORITY. Same invariant as `genericSpelling` above and as this file's
// private `normalizeKeepingRoot`: never change the leading separator RUN.
//
// ★★★ THE DEFECT THIS EXISTS FOR, ✔MEASURED 2026-08-28 on the toolchain that
// builds DSS (libstdc++ 13.2, MinGW-w64 UCRT), printed from one path object:
//     input        : //wsl.localhost/Ubuntu/home/rafael/p44_unc_inc
//     is_absolute  : false        has_root_name : false   root_name : <empty>
//     absolute()   : C:\wsl.localhost\Ubuntu\home\rafael\p44_unc_inc
//     exists(input): true         exists(absolute()) : false
// It does NOT merely prepend the cwd -- it RE-ROOTS a path naming another
// machine onto the local drive, silently and with no error. This path model
// gives a UNC authority no `root_name()` of its own, so `is_absolute()` is false
// and `absolute()` "repairs" it by supplying a drive that was never asked for.
//
// ★★ THE DISCRIMINATOR IS THE SEPARATOR RUN, NOT `is_absolute()` AND NOT
// `isRootedPath`. A run of ONE (`/foo`) genuinely IS a location on the current
// drive and MUST keep going through `fs::absolute`, which pins the drive that
// was current at resolution time -- skipping it there would be a regression, not
// a fix. A run of TWO OR MORE names an authority or a namespace (`//host/share`,
// `\\?\C:\…` extended-length, `\\.\…` device), none of which has any meaning to
// re-root. That is the same rule, and the same private helper, that
// `normalizeKeepingRoot` already applies to the lexical transform.
//
// ⚠ NOT A WINDOWS-ONLY GUARD. On a model where a UNC path is already absolute,
// `fs::absolute` returns it unchanged, so skipping the call is a no-op there and
// the two hosts agree by construction rather than by a platform branch.
//
// `ec` follows `fs::absolute`: cleared on the preserved path (nothing can fail),
// otherwise whatever the call reports. Callers keep their existing
// on-failure-use-the-raw-path idiom unchanged.
[[nodiscard]] DSS_EXPORT std::filesystem::path
absoluteKeepingRoot(std::filesystem::path const& p, std::error_code& ec);

// `lexically_normal()` WITHOUT letting it eat the leading separator RUN — the
// third member of this file's trio, and the reason all three are exported
// together: a path survives a UNC round trip only if EVERY transform applied to
// it preserves the run, and there are exactly three transforms that do not.
//
// ★★★ ✔MEASURED 2026-08-28, and it is why exporting this was owed. The
// artifact-written report ran ONE path through all three
// (`absolute` -> `lexically_normal` -> `generic_string`) and each removed one
// separator. Fixing only `absolute` moved the reported path from
// `C:\host\share\…` to `/host\share\…`: still wrong, still naming the local
// drive, and now wrong for a DIFFERENT reason. A partial fix here reads exactly
// like a complete one, because both spellings are equally absent from disk.
//
// ⚠ A run of 0 or 1 is untouched and takes `lexically_normal()` exactly as
// before, so this changes no local path.
[[nodiscard]] DSS_EXPORT std::filesystem::path
normalizeKeepingRoot(std::filesystem::path const& p);

// Reduce `p` to its identity. Never throws and never returns empty: a path that
// cannot be resolved degrades to its lexically-normal form, because a degraded
// key is strictly better than an exception out of a resolver and its only cost
// is that two exotic spellings might read as two nodes (a duplicate build, never
// a wrong one).
//
// ⚠ DELIBERATELY `weakly_canonical` AND NOT `canonical`: identity must be
// computable for a path that DOES NOT EXIST YET. Output artifacts, a `path`
// dependency resolved before anything confirmed the directory, and the cycle key
// needed for the reject's own message are all keyed before the file is there.
[[nodiscard]] DSS_EXPORT std::string
canonicalIdentityKey(std::filesystem::path const& p);

// Same, but REPORTING the underlying resolution failure instead of
// swallowing it. `ec` is set only by the filesystem refusing to answer (an
// unreadable parent) -- NEVER by the path merely not existing yet, and never
// by the short-name expansion, whose failure is the ordinary
// does-not-exist-yet case. The returned key is usable either way.
[[nodiscard]] DSS_EXPORT std::string
canonicalIdentityKey(std::filesystem::path const& p, std::error_code& ec);

class PathIdentity {
public:
    // THE ONLY CONSTRUCTOR. See the header note: this is the gate.
    [[nodiscard]] static PathIdentity of(std::filesystem::path const& p) {
        return PathIdentity{canonicalIdentityKey(p)};
    }
    // For callers that must FAIL LOUD on a filesystem refusal rather than
    // degrade. See `canonicalIdentityKey(p, ec)`.
    [[nodiscard]] static PathIdentity of(std::filesystem::path const& p,
                                         std::error_code& ec) {
        return PathIdentity{canonicalIdentityKey(p, ec)};
    }

    // Converting OUT is free — for I/O, and for a diagnostic an operator reads.
    //
    // ★★★ PREFERRED SEPARATORS, DELIBERATELY, AND THE KEY KEEPS GENERIC ONES.
    // The two answer different questions: `string()` is the IDENTITY and must be
    // uniform, while `path()` exists to be handed BACK to the filesystem, to a
    // spawned process, or to an operator. ✔MEASURED 2026-08-18: returning the
    // generic form here broke three native-probe tests with
    // `'""C:' is not recognized as an internal command` — the probe builds a
    // `.bat` path from a canonicalized base plus a natively-joined suffix, and
    // `cmd.exe` cannot run a forward-slashed path. ⚠ NOTHING ELSE NOTICED,
    // because the Win32 APIs accept `/` perfectly well; only the shell does not.
    [[nodiscard]] std::filesystem::path path() const {
        return std::filesystem::path{key_}.make_preferred();
    }
    [[nodiscard]] std::string const& string() const noexcept { return key_; }
    [[nodiscard]] std::string_view    view() const noexcept { return key_; }

    friend bool operator==(PathIdentity const& a,
                           PathIdentity const& b) noexcept {
        return a.key_ == b.key_;
    }
    friend std::strong_ordering operator<=>(PathIdentity const& a,
                                            PathIdentity const& b) noexcept {
        return a.key_ <=> b.key_;
    }

private:
    explicit PathIdentity(std::string key) : key_(std::move(key)) {}
    std::string key_;
};

}  // namespace dss::core

template <>
struct std::hash<dss::core::PathIdentity> {
    [[nodiscard]] std::size_t
    operator()(dss::core::PathIdentity const& p) const noexcept {
        return std::hash<std::string>{}(p.string());
    }
};
