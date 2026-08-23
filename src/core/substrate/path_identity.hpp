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
