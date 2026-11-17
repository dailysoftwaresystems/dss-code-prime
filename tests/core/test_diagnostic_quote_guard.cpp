// core/test_diagnostic_quote_guard (repo-guard) — EVERY MESSAGE QUOTES SOURCE TEXT THROUGH THE ONE RENDERING
// ([[D-DIAG-A-DIAGNOSTIC-QUOTES-ITS-OPERAND-WHOLE]], cycle P69 f26d, lane `cs`).
//
// A diagnostic that pastes `tree.text(n)` into its sentence grows with the expression it names: a nest of N
// conditionals printed O(N²) bytes, where both working references stay linear. The one rendering is
// `quotedSourceText` (core/types/diagnostic_quote.hpp). This test reads EVERY source under src/ and holds every
// MESSAGE statement of it to that rendering:
//   * a statement runs from the previous `;`, or a `{` / `}` that ends its line, to its `;` — string and
//     character literals and comments skipped;
//   * it is a MESSAGE statement when, outside its comments, it assigns a diagnostic's `.actual` / `.suggestion` /
//     `.note`, holds a string literal with a space in it (prose), or calls one of the file's emitters;
//   * in a message statement, each read of source text lies inside the argument list of a `quotedSourceText(`
//     call. The readers: a syntax tree's text (`tree.text(`, `tree_.text(`, `tree().text(`) in every file, and
//     the few readers too general for the whole tree in the one file that has them (a lexeme, a tokenizer range).
// A read outside any message statement (a lookup, a comparison, a decoder) is not this test's: it puts no text in
// front of anyone. The CONTROLS: a synthetic source whose raw reads are found and whose quoted ones are not; a
// floor on the sources read; the common readers matching quoted reads in the tree; and each file's own reader
// matching a quoted read in it (a reader spelled wrong would otherwise match nothing and pass).

#include "core/substrate/path_identity.hpp"   // walkTree, openInputStream
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] bool isIdentChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] bool isAlnum(char c) noexcept { return isIdentChar(c) && c != '_'; }

struct Reader {
    std::string_view text;     // `tree.text(`, or an identifier such as `lexeme`
    bool identifier = false;   // an identifier must also end at a non-identifier character
};

struct Scanned {
    std::vector<std::pair<std::size_t, std::size_t>> statements;   // [start, end)
    std::vector<std::pair<std::size_t, std::size_t>> strings;      // a "…" literal's [start, end)
    std::vector<char> code;      // 1 = a code character (not a literal, not a comment)
    std::vector<char> comment;   // 1 = inside a comment
};

[[nodiscard]] Scanned scan(std::string_view t) {
    Scanned s;
    std::size_t const n = t.size();
    s.code.assign(n, 0);
    s.comment.assign(n, 0);
    std::size_t i = 0, start = 0;
    while (i < n) {
        char const c = t[i];
        if (t.substr(i, 2) == "//") {
            std::size_t j = t.find('\n', i);
            if (j == std::string_view::npos) j = n;
            for (std::size_t k = i; k < j; ++k) s.comment[k] = 1;
            i = j;
            continue;
        }
        if (t.substr(i, 2) == "/*") {
            std::size_t j = t.find("*/", i + 2);
            j = (j == std::string_view::npos) ? n : j + 2;
            for (std::size_t k = i; k < j; ++k) s.comment[k] = 1;
            i = j;
            continue;
        }
        if (c == 'R' && t.substr(i, 2) == "R\"" && (i == 0 || !isIdentChar(t[i - 1]))) {
            std::size_t const k = t.find('(', i + 2);
            std::string const close = ")" + std::string{t.substr(i + 2, k - (i + 2))} + "\"";
            std::size_t j = t.find(close, k);
            j = (j == std::string_view::npos) ? n : j + close.size();
            i = j;
            continue;
        }
        if (c == '"' || c == '\'') {
            if (c == '\'' && i > 0 && isAlnum(t[i - 1])) {   // a digit separator, 1'000
                s.code[i] = 1;
                ++i;
                continue;
            }
            std::size_t j = i + 1;
            while (j < n && t[j] != c) j += (t[j] == '\\') ? 2 : 1;
            if (c == '"') s.strings.emplace_back(i, j + 1);
            i = j + 1;
            continue;
        }
        s.code[i] = 1;
        if (c == ';') {
            s.statements.emplace_back(start, i + 1);
            start = i + 1;
        } else if (c == '{' || c == '}') {
            std::size_t e = t.find('\n', i);
            if (e == std::string_view::npos) e = n;
            std::string_view rest = t.substr(i + 1, e - (i + 1));
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t' || rest.front() == '\r'))
                rest.remove_prefix(1);
            while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t' || rest.back() == '\r'))
                rest.remove_suffix(1);
            if (rest.empty() || rest.substr(0, 2) == "//") {
                s.statements.emplace_back(start, i + 1);
                start = i + 1;
            }
        }
        ++i;
    }
    return s;
}

// Each position of `needle` inside [a, b) of `t`, in order, until `visit` answers false. The search never leaves
// the statement. A `find` from `a` with only `p < b` checked after it runs to the file's end whenever the
// statement holds no match, and over every statement of every source that made the guard quadratic: MEASURED,
// 315 s into ctest's timeout on the mingw debug build. Bounded, the walk is linear in the bytes read.
template <class Visit>
void eachIn(std::string_view t, std::size_t a, std::size_t b, std::string_view needle, Visit&& visit) {
    std::string_view const span = t.substr(a, b - a);
    for (std::size_t p = span.find(needle); p != std::string_view::npos; p = span.find(needle, p + 1))
        if (!visit(a + p)) return;
}

// Does [a, b) assign `.actual` / `.suggestion` / `.note` (`=` or `+=`, never `==`) in code?
[[nodiscard]] bool assignsMessageField(std::string_view t, Scanned const& s, std::size_t a, std::size_t b) {
    bool assigns = false;
    for (std::string_view const f : {std::string_view{".actual"}, std::string_view{".suggestion"},
                                     std::string_view{".note"}}) {
        eachIn(t, a, b, f, [&](std::size_t p) {
            if (!s.code[p]) return true;
            std::size_t q = p + f.size();
            if (q < t.size() && isIdentChar(t[q])) return true;
            while (q < b && (t[q] == ' ' || t[q] == '\t' || t[q] == '\n' || t[q] == '\r')) ++q;
            if (q < b && t[q] == '+') ++q;
            assigns = q < b && t[q] == '=' && (q + 1 >= t.size() || t[q + 1] != '=');
            return !assigns;
        });
        if (assigns) return true;
    }
    return false;
}

[[nodiscard]] bool isMessage(std::string_view t, Scanned const& s, std::size_t a, std::size_t b,
                             std::vector<std::string_view> const& emitters) {
    if (assignsMessageField(t, s, a, b)) return true;
    // the literals are in source order: only those that start inside the statement are read
    auto lit = std::lower_bound(s.strings.begin(), s.strings.end(), a,
                                [](std::pair<std::size_t, std::size_t> const& l, std::size_t at) {
                                    return l.first < at;
                                });
    for (; lit != s.strings.end() && lit->first < b; ++lit) {
        auto const [la, lb] = *lit;
        if (lb <= b && !s.comment[la] && t.substr(la, lb - la).find(' ') != std::string_view::npos) return true;
    }
    bool emits = false;
    for (std::string_view const e : emitters) {
        eachIn(t, a, b, e, [&](std::size_t p) {
            emits = s.code[p] && !(p > 0 && (isIdentChar(t[p - 1]) || t[p - 1] == '.'));
            return !emits;
        });
        if (emits) return true;
    }
    return false;
}

struct Verdict {
    std::vector<std::size_t> rawLines;   // a read in a message statement outside every quote
    std::size_t quoted = 0;              // reads inside a quote
};

[[nodiscard]] Verdict judge(std::string_view t, std::vector<Reader> const& readers,
                            std::vector<std::string_view> const& extraEmitters) {
    std::vector<std::string_view> emitters{"report(", "refuse(", "fail(", "emit(", "emitWith(", "emitWidth(",
                                           "reportAt(", "prove(", "emitDiagnostic_("};
    emitters.insert(emitters.end(), extraEmitters.begin(), extraEmitters.end());
    Scanned const s = scan(t);
    Verdict v;
    for (auto const& [a, b] : s.statements) {
        if (!isMessage(t, s, a, b, emitters)) continue;
        // every `quotedSourceText(` argument list of the statement
        std::vector<std::pair<std::size_t, std::size_t>> quotes;
        constexpr std::string_view kQuote = "quotedSourceText(";
        eachIn(t, a, b, kQuote, [&](std::size_t p) {
            if (!s.code[p] || (p > 0 && isIdentChar(t[p - 1]))) return true;
            std::size_t const open = p + kQuote.size() - 1;
            int depth = 0;
            std::size_t q = open;
            for (; q < b; ++q) {
                if (!s.code[q]) continue;
                if (t[q] == '(') ++depth;
                else if (t[q] == ')' && --depth == 0) break;
            }
            quotes.emplace_back(open, q);
            return true;
        });
        for (Reader const& r : readers) {
            eachIn(t, a, b, r.text, [&](std::size_t p) {
                if (!s.code[p] || (p > 0 && isIdentChar(t[p - 1]))) return true;
                if (r.identifier && p + r.text.size() < t.size() && isIdentChar(t[p + r.text.size()])) return true;
                bool inside = false;
                for (auto const& [o, c] : quotes) inside = inside || (o < p && p < c);
                if (inside) {
                    ++v.quoted;
                } else {
                    std::size_t line = 1;
                    for (std::size_t k = 0; k < p; ++k) line += (t[k] == '\n') ? 1 : 0;
                    v.rawLines.push_back(line);
                }
                return true;
            });
        }
    }
    return v;
}

// The readers EVERY source is held to: a syntax tree's text, by each of the three names a tier gives its tree.
[[nodiscard]] std::vector<Reader> commonReaders() { return {{"tree.text("}, {"tree_.text("}, {"tree().text("}}; }

// A file's OWN readers and emitters, beside the common ones: a name too general to hold the whole tree to (a
// `lexeme` is a variable of every lexer-facing file; `r.slice(` the tokenizer's own range) or an emitter only that
// file defines.
struct FileExtras {
    std::string_view              path;
    std::vector<Reader>           readers;
    std::vector<std::string_view> emitters;
};

[[nodiscard]] std::vector<FileExtras> fileExtras() {
    return {
        {"src/hir/lowering/cst_to_hir.cpp", {}, {"emitH(", "emitHAt(", "unsupported("}},
        {"src/core/types/tree_builder.cpp", {{"lexeme", true}}, {}},
        {"src/tokenizer/tokenizer.cpp", {{"r.slice("}}, {}},
    };
}

// A source's bytes, read through the home: `p` is the walk's LISTED value, never its text re-resolved.
[[nodiscard]] std::string readAll(dss::core::HostPath const& p) {
    std::ifstream in = dss::core::openInputStream(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// THE TREE: every source under src/ -- not a list of the files known to quote today, so a file that starts
// pasting source text into a message is read the day it does.
TEST(DiagnosticQuoteGuard, EveryMessageQuotesSourceTextThroughTheOneRendering) {
    std::filesystem::path const                      root = dss::test::repoRoot();
    std::vector<std::pair<std::string, std::string>> sources;   // (relative, text)
    auto const walk = dss::core::walkTree(root / "src", [&](dss::core::TreeEntry const& entry) {
        if (entry.kind != dss::core::EntryKind::File) return dss::core::WalkStep::Continue;
        std::string const relative = "src/" + entry.relative.generic();
        if (relative.ends_with(".cpp") || relative.ends_with(".hpp"))
            sources.emplace_back(relative, readAll(entry.at));
        return dss::core::WalkStep::Continue;
    });
    ASSERT_FALSE(walk.error) << "the walk of src/ stopped at " << walk.where.spelling().generic() << ": "
                             << walk.error.message();
    EXPECT_GE(sources.size(), 400u) << "the guard read only " << sources.size() << " sources";

    std::vector<FileExtras> const extras = fileExtras();
    std::size_t                   commonQuoted = 0;
    std::vector<std::string_view> extrasSeen;
    for (auto const& [relative, text] : sources) {
        std::vector<Reader>           readers = commonReaders();
        std::vector<std::string_view> emitters;
        FileExtras const*             own = nullptr;
        for (FileExtras const& e : extras)
            if (e.path == relative) own = &e;
        if (own != nullptr) {
            readers.insert(readers.end(), own->readers.begin(), own->readers.end());
            emitters = own->emitters;
            extrasSeen.push_back(own->path);
        }
        Verdict const v = judge(text, readers, emitters);
        std::string lines;
        for (std::size_t l : v.rawLines) lines += " " + std::to_string(l);
        EXPECT_TRUE(v.rawLines.empty())
            << relative << ": " << v.rawLines.size() << " message statement read(s) of source text outside "
            << "`quotedSourceText`, at line(s)" << lines << " — a diagnostic names source text through the one "
            << "rendering (core/types/diagnostic_quote.hpp), never whole";
        commonQuoted += judge(text, commonReaders(), emitters).quoted;
        // THE CONTROL PER OWN READER: a reader that matches nothing would pass its file.
        if (own != nullptr && !own->readers.empty()) {
            EXPECT_GT(judge(text, own->readers, emitters).quoted, 0u)
                << relative << ": no quoted read found — is its reader still `" << own->readers.front().text << "`?";
        }
    }
    // THE CONTROLS: the common readers match quoted reads somewhere (a misspelt reader would pass the tree), and
    // every file named for its own readers or emitters was read (a moved file would drop them in silence).
    EXPECT_GT(commonQuoted, 0u) << "no quoted read of `tree.text(` / `tree_.text(` / `tree().text(` in src/";
    for (FileExtras const& e : extras) {
        EXPECT_NE(std::find(extrasSeen.begin(), extrasSeen.end(), e.path), extrasSeen.end())
            << e.path << " was not read: it moved, and its own readers and emitters went with it";
    }
}

TEST(DiagnosticQuoteGuard, TheScanFindsARawReadAndPassesAQuotedOne) {
    std::string_view const src =
        "void f() {\n"                                                                   // 1
        "    d.actual = std::string{tree.text(n)};\n"                                    // 2  raw: an actual
        "    report(c, std::format(\"x {}\", quotedSourceText(tree.text(n))));\n"         // 3  quoted
        "    auto const name = tree.text(n);\n"                                          // 4  no message
        "    // d.actual = tree.text(n); \"a b\"\n"                                      // 5  a comment
        "    emit(c, n, std::format(\"'{}' is wrong\", tree.text(m)));\n"                 // 6  raw: prose
        "    refuse(n, quotedSourceText(k ? tree.text(a) : std::string_view{\"b\"}));\n"  // 7  quoted
        "    if (tree.text(x) == \";\") return;\n"                                       // 8  no message
        "    d.suggestion += tree.text(z);\n"                                            // 9  raw: a suggestion
        "}\n";
    Verdict const v = judge(src, {{"tree.text("}}, {});
    EXPECT_EQ(v.rawLines, (std::vector<std::size_t>{2, 6, 9}));
    EXPECT_EQ(v.quoted, 2u);
}

} // namespace
