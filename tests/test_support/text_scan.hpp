#pragma once

// LINEAR SCANNERS OVER TEXT, for every test that reads source code or a message (P69 lane `cs`, fold f26d). Each is
// a loop: its work is proportional to the text it reads and its stack depth is constant.
//
// ★ WHY THIS FILE, AND NOT `std::regex` (MEASURED on the windows-x86_64-debug leg): a repo guard that ran `std::regex`
// over whole source files died of a SEGFAULT on the mingw-w64 build. libstdc++'s regex executor recurses once per
// character a match consumes (a `\s*` over a blanked comment, a `[^;=]*` over a long statement), so its stack depth
// follows the INPUT, and a Windows main thread's smaller stack overflows first -- the input-proportional recursion
// the compiler itself refuses, in a test. A test that matches text asks this file.

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dss::test {

// `\w`: a letter, a digit or `_`.
[[nodiscard]] inline bool isWordChar(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}
// The first character of an identifier: a letter or `_`.
[[nodiscard]] inline bool isIdentStart(char c) noexcept {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}
// `\s`.
[[nodiscard]] inline bool isSpace(char c) noexcept { return std::isspace(static_cast<unsigned char>(c)) != 0; }
// `\d`.
[[nodiscard]] inline bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

// The first position at or after `i` that is not white space (`\s*`).
[[nodiscard]] inline std::size_t skipSpace(std::string_view s, std::size_t i) noexcept {
    while (i < s.size() && isSpace(s[i])) ++i;
    return i;
}
// One past the run of digits starting at `i` (`\d*`): `i` itself when there is none.
[[nodiscard]] inline std::size_t digitsEnd(std::string_view s, std::size_t i) noexcept {
    while (i < s.size() && isDigit(s[i])) ++i;
    return i;
}
// `\b` at `i`: the characters on its two sides are not both word characters, nor both not.
[[nodiscard]] inline bool boundaryAt(std::string_view s, std::size_t i) noexcept {
    bool const before = i > 0 && isWordChar(s[i - 1]);
    bool const after  = i < s.size() && isWordChar(s[i]);
    return before != after;
}
// One past the identifier `[A-Za-z_]\w*` that BEGINS a word at `i`, or `i` itself when none does.
[[nodiscard]] inline std::size_t identEnd(std::string_view s, std::size_t i) noexcept {
    if (i >= s.size() || !isIdentStart(s[i]) || (i > 0 && isWordChar(s[i - 1]))) return i;
    std::size_t j = i + 1;
    while (j < s.size() && isWordChar(s[j])) ++j;
    return j;
}
// Is `word` at `i` a whole word (`\bword\b`)?
[[nodiscard]] inline bool wordAt(std::string_view s, std::size_t i, std::string_view word) noexcept {
    return s.compare(i, word.size(), word) == 0 && (i == 0 || !isWordChar(s[i - 1]))
        && (i + word.size() >= s.size() || !isWordChar(s[i + word.size()]));
}
// Does `text` hold `word` as a whole word anywhere?
[[nodiscard]] inline bool holdsWord(std::string_view text, std::string_view word) noexcept {
    for (std::size_t q = text.find(word); q != std::string_view::npos; q = text.find(word, q + 1))
        if (wordAt(text, q, word)) return true;
    return false;
}

// Every identifier of `s` in order -- each maximal word run that begins with a letter or `_`. `visit(begin, end)`
// answers where the scan resumes (at least `end`).
template <typename Visit>
void forEachIdentifier(std::string_view s, Visit&& visit) {
    std::size_t i = 0;
    while (i < s.size()) {
        if (!isWordChar(s[i])) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < s.size() && isWordChar(s[j])) ++j;
        if (isIdentStart(s[i])) {
            std::size_t const resume = visit(i, j);
            i = resume > j ? resume : j;
        } else {
            i = j;
        }
    }
}

// The arguments of a bracketed list's inside, split at the commas no `(`, `{`, `[` or `<` encloses.
[[nodiscard]] inline std::vector<std::string> topLevelArguments(std::string_view args) {
    std::vector<std::string> out;
    int                      depth = 0;
    std::string              cur;
    for (char const ch : args) {
        if (ch == '(' || ch == '{' || ch == '[' || ch == '<') ++depth;
        if (ch == ')' || ch == '}' || ch == ']' || ch == '>') --depth;
        if (ch == ',' && depth == 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += ch;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// The digits of the FIRST place in `text` where `before` is followed by one or more digits and then by `after`
// (the language of `before(\d+)after`, both literal), or nothing.
[[nodiscard]] inline std::optional<std::string> digitsBetween(std::string_view text, std::string_view before,
                                                              std::string_view after) {
    for (std::size_t p = text.find(before); p != std::string_view::npos; p = text.find(before, p + 1)) {
        std::size_t const first = p + before.size();
        std::size_t const end   = digitsEnd(text, first);
        if (end > first && text.compare(end, after.size(), after) == 0)
            return std::string{text.substr(first, end - first)};
    }
    return std::nullopt;
}

} // namespace dss::test
