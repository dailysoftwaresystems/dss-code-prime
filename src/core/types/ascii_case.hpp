#pragma once

#include <string>
#include <string_view>

// ASCII case-folding utilities — operator-typed CLI flags / JSON
// discriminator strings benefit from case-insensitive accept. Hoisted
// at LK10 cycle 3 post-fold #2 + D-LK6-1 post-fold #2 from byte-
// identical loops that landed in `program/cli_args.cpp::parseCompileConfig`
// and `core/types/target_schema.cpp::parseRelocFormulaKind` (the LK10
// `--config` precedent the second use cited). 2-consumer state today;
// pre-emptive 3rd-consumer hoist per "best long-term solution only"
// rule + code-simplifier post-fold #2 REQUIRED.
//
// Header-only: each consumer's call site becomes `dss::asciiToLower(s)`.
// ASCII-only: non-ASCII code units (UTF-8 continuation bytes >= 0x80, a
// wchar_t CJK code unit, ...) pass through unchanged. Locale-independent by
// construction.
//
// ★ ONE BODY, SEVERAL SPELLINGS. The fold is templated on the CHARACTER TYPE,
// not on the platform: `A`-`Z` occupy the same code points in `char`,
// `wchar_t`, `char8_t`, `char16_t` and `char32_t`, so a single body folds a
// narrow string and a Windows-wide `std::filesystem::path::string_type`
// identically, with no `#ifdef _WIN32` anywhere. The two non-templated
// spellings below exist ONLY because template argument deduction cannot see
// through `std::string` -> `std::string_view`; they FORWARD, they do not
// re-implement. A second folding loop is exactly how the include resolver's
// narrow and native comparisons would drift apart, which is the defect
// `D-PP-HEADER-CASE-NON-ASCII-NAME-NARROWING-THROW` exists to prevent.

namespace dss {

template <class CharT>
[[nodiscard]] std::basic_string<CharT>
asciiToLower(std::basic_string_view<CharT> s) {
    std::basic_string<CharT> out;
    out.reserve(s.size());
    for (CharT c : s) {
        out.push_back((c >= CharT{'A'} && c <= CharT{'Z'})
                          ? static_cast<CharT>(c + (CharT{'a'} - CharT{'A'}))
                          : c);
    }
    return out;
}

// Narrow spelling — the original signature, kept so `asciiToLower("Foo")` and
// `asciiToLower(someStringView)` still compile unchanged.
[[nodiscard]] inline std::string asciiToLower(std::string_view s) {
    return asciiToLower<char>(s);
}

// Any `basic_string`, notably `std::filesystem::path::string_type` — which is
// `std::string` on POSIX and `std::wstring` on Windows. This is the overload
// that lets the include resolver fold an on-disk name WITHOUT first narrowing
// it (`path::string()` throws on a name the host's narrow encoding cannot
// represent; see the note in `include_path_resolve.cpp`).
template <class CharT, class Traits, class Alloc>
[[nodiscard]] std::basic_string<CharT>
asciiToLower(std::basic_string<CharT, Traits, Alloc> const& s) {
    return asciiToLower<CharT>(std::basic_string_view<CharT>{s.data(), s.size()});
}

} // namespace dss
