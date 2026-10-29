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
// ★ ONE STATEMENT, TWO READERS, ONE GRAMMAR, ONE CASE TABLE. The Python reader is `TreeIgnores` in
// `.harness-config/runner/actions/owning-tree/owning-tree.py` (the repository's programs that walk
// a tree ask it); this is the same reading for C++, with the same refusals. Both are asked every
// scenario of `tests/test_support/tree_ignores_cases.json` — this one by
// `tests/core/test_header_name_matching.cpp`, whose repository-wide scan asks it — and each suite
// asks git itself the same paths (`git check-ignore`) under both settings of core.ignorecase.
//   - every rule in order, the last that matches deciding; `!` re-includes;
//   - a trailing `/` matches a directory only — never a symbolic link, which git records as a file;
//   - a `/` at the start or in the middle anchors the rule to the tree's root; a rule with none
//     matches that name at any depth;
//   - a name's pattern is matched against the name's BYTES (its UTF-8), as git's wildmatch matches
//     them: `*` is any run of bytes within one name, `?` one byte, `[...]` a class taking one byte —
//     `!` or `^` first negates it, a `]` right after the opening is itself, `a-z` a range of bytes,
//     a `-` first, last, or right after a range or a POSIX class is itself, a `[:` no `:]` closes
//     before the class's `]` is a `[` member, and the twelve POSIX classes take what git's own ASCII
//     table puts in them (a byte from 0x80 up is in none); a `**` name is any number of directories
//     — one or more at a rule's end — and inside a name it is a `*`;
//   - trailing SPACES are dropped, as git drops them (a tab stays); a byte order mark before the
//     first rule and a carriage return before a line feed are no part of a rule; `#` starts a comment;
//   - a directory left out takes everything under it — nothing below it can be re-included;
//   - `.git`, git's own, is always left out (a directory, or a worktree's `.git` file);
//   - the ROOT's `.gitignore` alone: it is the only one git tracks in this repository.
// ★ CASE, BYTE FOR BYTE, WITH core.ignorecase FALSE — and A PATH A RULE MATCHES UNDER ONE SETTING OF
//   core.ignorecase AND NOT THE OTHER IS REFUSED, naming the path and the rule, on every host (and so
//   is a name that is `.git` only when folded). git folds ASCII letters when the setting is true (a
//   clone's default on Windows and macOS) — the name's byte and a rule's literal, never a class's
//   member — and the two settings disagree both ways (✔MEASURED, git 2.55: `*.LOG` takes `a.log`
//   only folded, `[A]1` takes `A1` only NOT folded). Refusing every such path, this reader gives the
//   one answer every host's git gives.
// ⚠ A RULE THIS READER CANNOT READ AS GIT DOES IS A REFUSAL NAMING ITS LINE — a backslash (an
//   escape), a `[` class never closed, a POSIX class git does not name, an empty name between two
//   `/`, a rule that is nothing but `/` or `!` — and so is a `.gitignore` that cannot be read, is a
//   directory, or is not UTF-8. A PATH this reader cannot answer as git does is a `TreeIgnoreRefusal`
//   naming it: a name holding a backslash (no Windows host can hold one, and a reader there would
//   split it where git on another does not), and the case rule above. A tree with no `.gitignore`
//   declares nothing ignored: only `.git` is left out.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace dss::test_support {

// A path this reader cannot answer as git does (see the header's last block).
struct TreeIgnoreRefusal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace tree_ignores_detail {

enum class TokenKind : std::uint8_t { Literal, AnyByte, Star, Class };
enum class ItemKind : std::uint8_t { Member, Range, Posix };

struct ClassItem {
    ItemKind      kind = ItemKind::Member;
    unsigned char low = 0;    // the member, or a range's low end
    unsigned char high = 0;   // a range's high end
    std::string   posix;      // a POSIX class's name
};

struct Token {
    TokenKind              kind = TokenKind::Literal;
    unsigned char          literal = 0;
    bool                   negated = false;
    std::vector<ClassItem> items;
};

// One name of a rule; `anyDirs` marks a `**` name.
struct NamePattern {
    bool               anyDirs = false;
    std::vector<Token> tokens;
};

[[nodiscard]] inline bool asciiUpper(unsigned char b) { return b >= 'A' && b <= 'Z'; }
[[nodiscard]] inline bool asciiLower(unsigned char b) { return b >= 'a' && b <= 'z'; }
[[nodiscard]] inline bool asciiAlpha(unsigned char b) { return asciiUpper(b) || asciiLower(b); }
[[nodiscard]] inline bool asciiDigit(unsigned char b) { return b >= '0' && b <= '9'; }
[[nodiscard]] inline unsigned char asciiFold(unsigned char b) {
    return asciiUpper(b) ? static_cast<unsigned char>(b + 32) : b;
}

// git's own table (sane_ctype, as wildmatch asks it): ASCII only. Under core.ignorecase the name's
// byte is already folded to lower case, which is why `upper` also takes a lower-case byte. An
// unknown name answers `false` and sets `known` false.
[[nodiscard]] inline bool posixHit(std::string_view name, unsigned char b, bool fold, bool* known = nullptr) {
    if (known != nullptr) *known = true;
    if (name == "alnum") return asciiAlpha(b) || asciiDigit(b);
    if (name == "alpha") return asciiAlpha(b);
    if (name == "blank") return b == '\t' || b == ' ';
    if (name == "cntrl") return b < 32 || b == 127;
    if (name == "digit") return asciiDigit(b);
    if (name == "graph") return b >= 33 && b <= 126;
    if (name == "lower") return asciiLower(b);
    if (name == "print") return b >= 32 && b <= 126;
    if (name == "punct") return b >= 33 && b <= 126 && !asciiAlpha(b) && !asciiDigit(b);
    if (name == "space") return b == '\t' || b == '\n' || b == '\r' || b == ' ';
    if (name == "upper") return asciiUpper(b) || (fold && asciiLower(b));
    if (name == "xdigit") return asciiDigit(b) || (b >= 'A' && b <= 'F') || (b >= 'a' && b <= 'f');
    if (known != nullptr) *known = false;
    return false;
}

// The class opening at pat[at] (a `[`), as wildmatch reads it. Returns the index just past its `]`,
// or npos with `why` saying why git could not read it.
[[nodiscard]] inline std::size_t readClass(std::string_view pat, std::size_t at, Token& out, std::string& why) {
    std::size_t const n = pat.size();
    std::size_t       i = at + 1;
    auto const        byteAt = [&](std::size_t k) { return k < n ? static_cast<int>(static_cast<unsigned char>(pat[k])) : -1; };
    int               c = byteAt(i);
    if (c == '^') c = '!';
    out.kind = TokenKind::Class;
    out.negated = c == '!';
    if (out.negated) c = byteAt(++i);
    int prev = 0;
    while (true) {
        if (c < 0) {
            why = "opens a `[` class it never closes";
            return std::string_view::npos;
        }
        if (c == '-' && prev != 0 && i + 1 < n && pat[i + 1] != ']') {
            ++i;
            out.items.push_back({ItemKind::Range, static_cast<unsigned char>(prev),
                                 static_cast<unsigned char>(pat[i]), {}});
            c = 0;  // a `-` right after a range is itself
        } else if (c == '[' && i + 1 < n && pat[i + 1] == ':') {
            std::size_t j = i + 2;
            while (j < n && pat[j] != ']') ++j;
            if (j >= n) {
                why = "opens a `[` class it never closes";
                return std::string_view::npos;
            }
            if (j < i + 3 || pat[j - 1] != ':') {
                out.items.push_back({ItemKind::Member, '[', 0, {}});  // no `:]`: `[` is a member, `:` the next
            } else {
                std::string const name(pat.substr(i + 2, j - 1 - (i + 2)));
                bool              known = false;
                (void)posixHit(name, 0, false, &known);
                if (!known) {
                    why = "names the class [:" + name + ":], which git does not know";
                    return std::string_view::npos;
                }
                out.items.push_back({ItemKind::Posix, 0, 0, name});
                i = j;
                c = 0;  // a `-` right after a POSIX class is itself
            }
        } else {
            out.items.push_back({ItemKind::Member, static_cast<unsigned char>(c), 0, {}});
        }
        prev = c;
        c = byteAt(++i);
        if (c == ']') return i + 1;
    }
}

// One name of a rule as tokens; false, with `why`, when git could not read it.
[[nodiscard]] inline bool readNamePattern(std::string_view name, NamePattern& out, std::string& why) {
    if (name == "**") {
        out.anyDirs = true;
        return true;
    }
    std::size_t i = 0;
    while (i < name.size()) {
        char const c = name[i];
        if (c == '*') {
            if (out.tokens.empty() || out.tokens.back().kind != TokenKind::Star) out.tokens.push_back({TokenKind::Star, 0, false, {}});
            ++i;
        } else if (c == '?') {
            out.tokens.push_back({TokenKind::AnyByte, 0, false, {}});
            ++i;
        } else if (c == '[') {
            Token             cls;
            std::size_t const end = readClass(name, i, cls, why);
            if (end == std::string_view::npos) return false;
            out.tokens.push_back(std::move(cls));
            i = end;
        } else {
            out.tokens.push_back({TokenKind::Literal, static_cast<unsigned char>(c), false, {}});
            ++i;
        }
    }
    return true;
}

// Does one token take the name's byte `t`? Under `fold`, as wildmatch folds: the name's byte and a
// literal of the rule to lower case, a range also asked with the byte in upper case — a class's member
// is never folded.
[[nodiscard]] inline bool byteHit(Token const& tok, unsigned char t, bool fold) {
    if (tok.kind == TokenKind::AnyByte) return true;
    if (fold) t = asciiFold(t);
    if (tok.kind == TokenKind::Literal) return t == (fold ? asciiFold(tok.literal) : tok.literal);
    bool hit = false;
    for (auto const& item : tok.items) {
        if (item.kind == ItemKind::Member) {
            hit = t == item.low;
        } else if (item.kind == ItemKind::Range) {
            hit = (item.low <= t && t <= item.high)
                  || (fold && asciiLower(t) && item.low <= t - 32 && t - 32 <= item.high);
        } else {
            hit = posixHit(item.posix, t, fold);
        }
        if (hit) break;
    }
    return hit != tok.negated;
}

// One NAME (bytes) against one name pattern. Iterative: a `*` backtracks to its last start only.
[[nodiscard]] inline bool nameMatches(NamePattern const& pat, std::string_view name, bool fold) {
    auto const&       toks = pat.tokens;
    std::size_t       p = 0, n = 0;
    std::size_t       starP = std::string_view::npos, starN = 0;
    while (n < name.size()) {
        if (p < toks.size() && toks[p].kind == TokenKind::Star) {
            starP = p;
            starN = n;
            ++p;
            continue;
        }
        if (p < toks.size() && byteHit(toks[p], static_cast<unsigned char>(name[n]), fold)) {
            ++p;
            ++n;
            continue;
        }
        if (starP == std::string_view::npos) return false;
        ++starN;
        p = starP + 1;
        n = starN;
    }
    while (p < toks.size() && toks[p].kind == TokenKind::Star) ++p;
    return p == toks.size();
}

// A rule's name patterns against a path's names: a `**` pattern is any number of names (one or more
// when it is the last). The two shapes nearly every rule has are asked directly; any other by a table
// filled from the end — no recursion proportional to the path.
[[nodiscard]] inline bool namesMatch(std::vector<NamePattern> const& pats, std::vector<std::string> const& names,
                                     bool fold) {
    std::size_t const m = pats.size(), n = names.size();
    bool              anyStar = false;
    for (auto const& p : pats) anyStar = anyStar || p.anyDirs;
    if (!anyStar) {
        if (m != n) return false;
        for (std::size_t i = 0; i < m; ++i)
            if (!nameMatches(pats[i], names[i], fold)) return false;
        return true;
    }
    if (m == 2 && pats[0].anyDirs && !pats[1].anyDirs) return n >= 1 && nameMatches(pats[1], names[n - 1], fold);
    std::vector<std::vector<char>> ok(m + 1, std::vector<char>(n + 1, 0));
    ok[m][n] = 1;
    for (std::size_t ii = m; ii-- > 0;) {
        for (std::size_t jj = n + 1; jj-- > 0;) {
            if (pats[ii].anyDirs) {
                ok[ii][jj] = (ii == m - 1) ? (jj < n) : (ok[ii + 1][jj] || (jj < n && ok[ii][jj + 1]));
            } else {
                ok[ii][jj] = jj < n && nameMatches(pats[ii], names[jj], fold) && ok[ii + 1][jj + 1];
            }
        }
    }
    return ok[0][0] != 0;
}

// The `/`-separated names of a tree-relative path; a backslash is part of a name, never a separator.
[[nodiscard]] inline std::vector<std::string> splitNames(std::string_view rel) {
    std::vector<std::string> out;
    std::size_t              at = 0;
    while (at <= rel.size()) {
        std::size_t const slash = rel.find('/', at);
        std::string_view  part = rel.substr(at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
        if (!part.empty()) out.emplace_back(part);
        if (slash == std::string_view::npos) break;
        at = slash + 1;
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

[[nodiscard]] inline std::string folded(std::string s) {
    for (auto& c : s) c = static_cast<char>(asciiFold(static_cast<unsigned char>(c)));
    return s;
}

} // namespace tree_ignores_detail

struct TreeIgnoreRule {
    std::string                                  text;   // the rule as written, trailing spaces dropped
    int                                          line = 0;
    bool                                         negated = false;
    bool                                         dirOnly = false;
    std::vector<tree_ignores_detail::NamePattern> pats;  // its name patterns; a `**` name among them
};

struct TreeIgnores {
    std::vector<TreeIgnoreRule> rules;
    std::string                 refusal;  // non-empty: the `.gitignore` cannot be read as git reads it

    // The rule that leaves this path out — its ancestors already walked — or "" when it stays. A path
    // this reader cannot answer as git does throws `TreeIgnoreRefusal` naming it.
    [[nodiscard]] std::string decide(std::vector<std::string> const& names, bool isDir) const {
        std::string shown;
        for (auto const& name : names) shown += (shown.empty() ? "" : "/") + name;
        for (auto const& name : names)
            if (name == ".git") return ".git";
        for (auto const& name : names) {
            if (name.find('\\') != std::string::npos)
                throw TreeIgnoreRefusal("the name '" + name + "' in " + shown
                                        + " holds a backslash: no Windows host can hold it, and a reader on one "
                                          "splits it where git on another does not, so the walk is refused");
        }
        for (auto const& name : names)
            if (tree_ignores_detail::folded(name) == ".git")
                throw TreeIgnoreRefusal("the path " + shown + " holds '" + name
                                        + "', which git takes for its own directory only when case is folded "
                                          "(core.ignorecase true): hosts would answer differently");
        std::vector<std::string> low;
        low.reserve(names.size());
        for (auto const& name : names) low.push_back(tree_ignores_detail::folded(name));
        TreeIgnoreRule const* decided = nullptr;
        for (auto const& rule : rules) {
            if (rule.dirOnly && !isDir) continue;
            bool const hit = tree_ignores_detail::namesMatch(rule.pats, names, false);
            if (hit != tree_ignores_detail::namesMatch(rule.pats, low, true))
                throw TreeIgnoreRefusal("the rule '" + rule.text + "' (line " + std::to_string(rule.line) + ") "
                                        + (hit ? "matches" : "fails") + " the path " + shown + " only when case is "
                                        + (hit ? "NOT folded" : "folded")
                                        + ": git answers it one way under core.ignorecase false and the other "
                                          "under true, so the walk is refused");
            if (hit) decided = rule.negated ? nullptr : &rule;
        }
        return decided ? decided->text : std::string{};
    }

    // The rule that leaves the tree-relative, `/`-joined path `rel` out — its own, or a parent
    // directory's — or "". Throws `TreeIgnoreRefusal` as `decide` does.
    [[nodiscard]] std::string leavesOut(std::string_view rel, bool isDir) const {
        auto const names = tree_ignores_detail::splitNames(rel);
        for (std::size_t depth = 1; depth < names.size(); ++depth) {
            std::vector<std::string> const head(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(depth));
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
            tree_ignores_detail::NamePattern pat;
            std::string                      why;
            if (!tree_ignores_detail::readNamePattern(name, pat, why)) return refuse(why);
            rule.pats.push_back(std::move(pat));
            if (slash == std::string_view::npos) break;
            start = slash + 1;
        }
        if (!anchored) {
            tree_ignores_detail::NamePattern anyDirs;
            anyDirs.anyDirs = true;
            rule.pats.insert(rule.pats.begin(), std::move(anyDirs));
        }
        out.rules.push_back(std::move(rule));
    }
    return out;
}

// What a walk of `tree` leaves out, read from its root `.gitignore`. No file: nothing but `.git`. A
// `.gitignore` that is a directory, cannot be opened, or fails while it is read is a refusal, as Python
// refuses one: a reader that took an empty read for an empty file would leave out nothing in silence.
[[nodiscard]] inline TreeIgnores readTreeIgnores(std::filesystem::path const& tree) {
    std::filesystem::path const file = tree / ".gitignore";
    auto const                  refused = [&](std::string const& why) {
        TreeIgnores out;
        out.refusal = file.generic_string() + " " + why + ": what a walk of the tree leaves out is unknown";
        return out;
    };
    std::error_code    ec;
    auto const         status = std::filesystem::status(file, ec);
    if (status.type() == std::filesystem::file_type::not_found) return {};
    if (ec) return refused("cannot be read (" + ec.message() + ")");
    if (std::filesystem::is_directory(status)) return refused("cannot be read (it is a directory)");
    std::ifstream in(file, std::ios::binary);
    if (!in) return refused("cannot be read (it does not open)");
    std::string const text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) return refused("cannot be read (the read failed)");
    if (!tree_ignores_detail::isUtf8(text)) return refused("is not UTF-8");
    return readTreeIgnoreRules(text, file.generic_string());
}

} // namespace dss::test_support
