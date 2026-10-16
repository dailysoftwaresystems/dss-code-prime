#pragma once

// WHAT A WALK OF A TREE LEAVES OUT: WHAT GIT IGNORES THERE — the C++ reader of the one statement.
//
// A test that walks this repository FROM ITS ROOT reads the tree's content, and what git ignores
// there is not content: the copies and workers DssHarness keeps in a tree (`.legs/`, `.mutations/`,
// beside `.worktrees/` and `.orchestrators/` — each a whole tree of its own), its run records, a
// session's scratch, a build. A walk of them meets every file twice, and files that vanish under
// it while a sweep or a lane works there. The statement is the tree's own `.gitignore`, which the
// tool's `init` writes its block into and git obeys; no list is kept here.
//
// ★ ONE STATEMENT, TWO READERS, ONE GRAMMAR. The Python reader is `TreeIgnores` in
// `.harness-config/runner/actions/owning-tree/owning-tree.py` (the repository's programs that walk
// a tree ask it); this is the same reading for C++, with the same refusals, pinned case for case
// by `tests/core/test_header_name_matching.cpp`, whose repository-wide scan asks it.
//   - every rule in order, the last that matches deciding; `!` re-includes;
//   - a trailing `/` matches a directory only;
//   - a `/` at the start or in the middle anchors the rule to the tree's root; a rule with none
//     matches that name at any depth;
//   - `*` and `?` stay within one name; `[...]` is a class (`!` or `^` first negates it, `a-z` is a
//     range, a `]` right after the opening is itself); a `**` name is any number of directories —
//     one or more at a rule's end; names and patterns compare as CHARACTERS (UTF-8 code points),
//     case and all, so the answer is the same on every host;
//   - trailing blanks are dropped, as git drops them; a line starting `#` is a comment;
//   - a directory left out takes everything under it — nothing below it can be re-included;
//   - `.git`, git's own, is always left out (a directory, or a worktree's `.git` file);
//   - the ROOT's `.gitignore` alone: it is the only one git tracks in this repository.
// ⚠ A RULE THIS READER CANNOT READ AS GIT DOES IS A REFUSAL NAMING ITS LINE — a backslash (an
//   escape), an unterminated `[`, an empty name between two `/`, a rule that is nothing but `/` or
//   `!` — and so is a `.gitignore` that cannot be read or is not UTF-8. Read some other way, a rule
//   would leave out another set than git does, in silence. A tree with no `.gitignore` declares
//   nothing ignored: only `.git` is left out.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace dss::test_support {

struct TreeIgnoreRule {
    std::string                 text;         // the rule as written, trailing blanks dropped
    int                         line = 0;
    bool                        negated = false;
    bool                        dirOnly = false;
    std::vector<std::u32string> pats;         // its name patterns; "**" among them
};

namespace tree_ignores_detail {

inline constexpr char32_t kBadByte = 0xFFFD;

// UTF-8 to code points; a byte that opens no valid sequence is one U+FFFD (a name the scan could
// not render never reaches here: its callers count it and skip it).
[[nodiscard]] inline std::u32string codePoints(std::string_view s) {
    std::u32string out;
    std::size_t    i = 0;
    while (i < s.size()) {
        auto const    b = static_cast<unsigned char>(s[i]);
        std::size_t   len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > s.size()) {
            out.push_back(kBadByte);
            ++i;
            continue;
        }
        char32_t cp = len == 1 ? b : len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
        bool     ok = true;
        for (std::size_t k = 1; k < len; ++k) {
            auto const c = static_cast<unsigned char>(s[i + k]);
            if ((c >> 6) != 0x2) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (c & 0x3F);
        }
        out.push_back(ok ? cp : kBadByte);
        i += ok ? len : 1;
    }
    return out;
}

// Is `s` well-formed UTF-8 (as Python's strict decoder reads it: no overlong form, no surrogate, nothing past
// U+10FFFF)?
[[nodiscard]] inline bool isUtf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        auto const  b = static_cast<unsigned char>(s[i]);
        std::size_t len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > s.size()) return false;
        char32_t cp = len == 1 ? b : len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
        for (std::size_t k = 1; k < len; ++k) {
            auto const c = static_cast<unsigned char>(s[i + k]);
            if ((c >> 6) != 0x2) return false;
            cp = (cp << 6) | (c & 0x3F);
        }
        char32_t const least = len == 1 ? 0 : len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
        if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

// The index just past the `]` closing the class that opens at pat[at] (a `[`), or npos.
[[nodiscard]] inline std::size_t classEnd(std::u32string_view pat, std::size_t at) {
    std::size_t i = at + 1;
    if (i < pat.size() && (pat[i] == U'!' || pat[i] == U'^')) ++i;
    if (i < pat.size() && pat[i] == U']') ++i;
    while (i < pat.size() && pat[i] != U']') ++i;
    return i < pat.size() ? i + 1 : std::u32string_view::npos;
}

[[nodiscard]] inline bool classHit(std::u32string_view pat, std::size_t at, char32_t ch) {
    std::size_t const end = classEnd(pat, at);
    std::size_t       i = at + 1;
    bool              negate = false;
    if (pat[i] == U'!' || pat[i] == U'^') {
        negate = true;
        ++i;
    }
    bool hit = false;
    bool first = true;
    while (i < end - 1 && (pat[i] != U']' || first)) {
        if (i + 2 < end - 1 && pat[i + 1] == U'-') {
            if (pat[i] <= ch && ch <= pat[i + 2]) hit = true;
            i += 3;
        } else {
            if (pat[i] == ch) hit = true;
            ++i;
        }
        first = false;
    }
    return hit != negate;
}

// One NAME against one name pattern: `*` any run, `?` one character, `[...]` a class. Iterative.
[[nodiscard]] inline bool nameMatches(std::u32string_view pat, std::u32string_view name) {
    std::size_t p = 0, n = 0;
    std::size_t starP = std::u32string_view::npos, starN = 0;
    while (n < name.size()) {
        if (p < pat.size() && pat[p] == U'*') {
            starP = p;
            starN = n;
            ++p;
            continue;
        }
        if (p < pat.size()) {
            char32_t const c = pat[p];
            if (c == U'?') {
                ++p;
                ++n;
                continue;
            }
            if (c == U'[') {
                if (classHit(pat, p, name[n])) {
                    p = classEnd(pat, p);
                    ++n;
                    continue;
                }
            } else if (c == name[n]) {
                ++p;
                ++n;
                continue;
            }
        }
        if (starP == std::u32string_view::npos) return false;
        ++starN;
        p = starP + 1;
        n = starN;
    }
    while (p < pat.size() && pat[p] == U'*') ++p;
    return p == pat.size();
}

// A rule's name patterns against a path's names: a "**" pattern is any number of names (one or more
// when it is the last). The two shapes nearly every rule has are asked directly; any other by a table
// filled from the end — no recursion proportional to the path.
[[nodiscard]] inline bool namesMatch(std::vector<std::u32string> const& pats,
                                     std::vector<std::u32string> const& names) {
    std::size_t const m = pats.size(), n = names.size();
    bool              anyStar = false;
    for (auto const& p : pats) anyStar = anyStar || p == U"**";
    if (!anyStar) {
        if (m != n) return false;
        for (std::size_t i = 0; i < m; ++i)
            if (!nameMatches(pats[i], names[i])) return false;
        return true;
    }
    if (m == 2 && pats[0] == U"**" && pats[1] != U"**") return n >= 1 && nameMatches(pats[1], names[n - 1]);
    std::vector<std::vector<char>> ok(m + 1, std::vector<char>(n + 1, 0));
    ok[m][n] = 1;
    for (std::size_t ii = m; ii-- > 0;) {
        for (std::size_t jj = n + 1; jj-- > 0;) {
            if (pats[ii] == U"**") {
                ok[ii][jj] = (ii == m - 1) ? (jj < n) : (ok[ii + 1][jj] || (jj < n && ok[ii][jj + 1]));
            } else {
                ok[ii][jj] = jj < n && nameMatches(pats[ii], names[jj]) && ok[ii + 1][jj + 1];
            }
        }
    }
    return ok[0][0] != 0;
}

[[nodiscard]] inline std::vector<std::u32string> splitNames(std::string_view rel) {
    std::vector<std::u32string> out;
    std::size_t                 at = 0;
    while (at <= rel.size()) {
        std::size_t const slash = rel.find_first_of("/\\", at);
        std::string_view  part = rel.substr(at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
        if (!part.empty()) out.push_back(codePoints(part));
        if (slash == std::string_view::npos) break;
        at = slash + 1;
    }
    return out;
}

} // namespace tree_ignores_detail

struct TreeIgnores {
    std::vector<TreeIgnoreRule> rules;
    std::string                 refusal;  // non-empty: the `.gitignore` cannot be read as git reads it

    // The rule that leaves this path out — its ancestors already walked — or "" when it stays.
    [[nodiscard]] std::string decide(std::vector<std::u32string> const& names, bool isDir) const {
        for (auto const& name : names)
            if (name == U".git") return ".git";
        TreeIgnoreRule const* decided = nullptr;
        for (auto const& rule : rules) {
            if (rule.dirOnly && !isDir) continue;
            if (tree_ignores_detail::namesMatch(rule.pats, names)) decided = rule.negated ? nullptr : &rule;
        }
        return decided ? decided->text : std::string{};
    }

    // The rule that leaves the tree-relative path `rel` out — its own, or a parent directory's — or "".
    [[nodiscard]] std::string leavesOut(std::string_view rel, bool isDir) const {
        auto const names = tree_ignores_detail::splitNames(rel);
        for (std::size_t depth = 1; depth < names.size(); ++depth) {
            std::vector<std::u32string> const head(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(depth));
            if (auto why = decide(head, true); !why.empty()) return why;
        }
        return names.empty() ? std::string{} : decide(names, isDir);
    }
};

// The rules of a `.gitignore`'s text, in order; `refusal` names the first line this reader cannot read.
[[nodiscard]] inline TreeIgnores readTreeIgnoreRules(std::string_view text, std::string_view source) {
    TreeIgnores out;
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    int         number = 0;
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t const nl = text.find('\n', at);
        std::string_view  line = text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        at = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        ++number;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        while (!line.empty() && line.back() == ' ') line.remove_suffix(1);
        if (line.empty()) continue;
        auto refuse = [&](std::string_view why) {
            out.refusal = std::string(source) + " line " + std::to_string(number) + ": the rule '" + std::string(line)
                          + "' " + std::string(why) + ", so it cannot be read as git reads it";
            out.rules.clear();
            return out;
        };
        if (line.find('\\') != std::string_view::npos) return refuse("holds a backslash (an escape)");
        TreeIgnoreRule rule;
        rule.text = std::string(line);
        rule.line = number;
        std::string_view body = line;
        rule.negated = body.front() == '!';
        if (rule.negated) body.remove_prefix(1);
        rule.dirOnly = !body.empty() && body.back() == '/';
        if (rule.dirOnly) body.remove_suffix(1);
        bool const anchored = body.find('/') != std::string_view::npos;
        if (!body.empty() && body.front() == '/') body.remove_prefix(1);
        if (body.empty()) return refuse("names nothing");
        std::size_t start = 0;
        while (true) {
            std::size_t const slash = body.find('/', start);
            std::string_view  name = body.substr(start, slash == std::string_view::npos ? std::string_view::npos
                                                                                         : slash - start);
            if (name.empty()) return refuse("names nothing between two separators");
            std::u32string const pat = tree_ignores_detail::codePoints(name);
            for (std::size_t open = pat.find(U'['); open != std::u32string::npos;) {
                std::size_t const end = tree_ignores_detail::classEnd(pat, open);
                if (end == std::u32string_view::npos) return refuse("opens a `[` class it never closes");
                open = pat.find(U'[', end);
            }
            rule.pats.push_back(pat);
            if (slash == std::string_view::npos) break;
            start = slash + 1;
        }
        if (!anchored) rule.pats.insert(rule.pats.begin(), U"**");
        out.rules.push_back(std::move(rule));
    }
    return out;
}

// What a walk of `tree` leaves out, read from its root `.gitignore`. No file: nothing but `.git`.
[[nodiscard]] inline TreeIgnores readTreeIgnores(std::filesystem::path const& tree) {
    std::filesystem::path const file = tree / ".gitignore";
    std::error_code             ec;
    if (!std::filesystem::exists(file, ec) && !ec) return {};
    std::ifstream in(file, std::ios::binary);
    if (ec || !in) {
        TreeIgnores refused;
        refused.refusal = file.generic_string() + " cannot be read: what a walk of the tree leaves out is unknown";
        return refused;
    }
    std::string const text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (!tree_ignores_detail::isUtf8(text)) {
        TreeIgnores refused;
        refused.refusal = file.generic_string() + " is not UTF-8: what a walk of the tree leaves out is unknown";
        return refused;
    }
    return readTreeIgnoreRules(text, file.generic_string());
}

} // namespace dss::test_support
