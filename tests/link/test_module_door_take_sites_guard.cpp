// ★★ THE UNIVERSE OF THE LINK TIER'S TAKE SITES, BY DATA (P69, lane `cs`, fold f26d -- main's ruling on the take
// sites). `repo-guard`: it judges the source tree, the same bytes on every leg.
//
// tests/link/test_module_door_take_sites.cpp holds every take site to the rule at the edge of its id space; it can
// hold only the sites its table names. This guard reads src/ and keeps that table (tests/link/module_door_take_sites.hpp)
// the universe, both ways:
//   * a TAKE SITE src/ states and the table does not hold is refused -- a pass that asks a door for an id has a row,
//     and so a drive, before it lands;
//   * a ROW no site of src/ states is refused -- a site that moved, or whose label changed, takes its row with it;
//   * a site whose label is no literal is refused -- the label is the key, and a key the source does not spell is
//     one neither the table nor a reader of the refusal can match;
//   * one label at two sites of one file is refused -- a refusal names ONE site.
// A TAKE SITE is
//   (a) a member call `take(` -- through `.` or `->` -- of two arguments or more: the module door's
//       `take(reporter, who)` and `take(count, reporter, who)`. Its label is the first string literal of its LAST
//       argument (adjacent literals joined; a `std::format` string as spelled, its holes `{}`). A `take()` of no
//       argument is a space's own step (`SymbolIdEnd::take`, `NewSpaceSymbolIds::take`), which names nobody;
//   (b) the construction of a new space's door -- `NewSpaceSymbolIds name{...}`, `NewSpaceSymbolIds{...}`, or either
//       with parentheses -- whose every take answers to the label its last argument states. The class's own body
//       (its constructor and copy operations) states no site, and neither does a use of the name as a type.
// SCOPE: every *.cpp and *.hpp under src/, its CODE only (`codeOnly` blanks comments and literals and keeps every
// offset), the label read from the raw text at the same offsets.
// NOT JUDGED HERE: that a pass needing several ids on its caller's module takes them at once. The table's drive
// asserts it for every site it holds (refused at every room below the drive's total, the caller's module unchanged),
// and this guard gives every new site a row.

#include "core/substrate/path_identity.hpp"   // walkTree
#include "repo_root.hpp"
#include "source_code_only.hpp"               // codeOnly
#include "text_scan.hpp"                      // the linear scanners

#include "module_door_take_sites.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using dss::test::isIdentStart;
using dss::test::isSpace;
using dss::test::isWordChar;
using dss::test::skipSpace;
using dss::test::wordAt;

struct Site {
    std::string                file;    // from the repository root
    std::size_t                line = 0;
    std::optional<std::string> label;   // nothing: its last argument states no literal
    std::string                spelled; // the call as the source spells it, for the message
};

[[nodiscard]] std::size_t lineAt(std::string const& code, std::size_t offset) {
    return 1u + static_cast<std::size_t>(std::count(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
}

// The index of the bracket closing the one at `open`, over ( [ { -- or npos.
[[nodiscard]] std::size_t closingOf(std::string const& code, std::size_t open) {
    int depth = 0;
    for (std::size_t k = open; k < code.size(); ++k) {
        char const c = code[k];
        if (c == '(' || c == '[' || c == '{') ++depth;
        if (c == ')' || c == ']' || c == '}') {
            if (--depth == 0) return k;
        }
    }
    return std::string::npos;
}

// The arguments inside (open, close): how many, and where the last one begins.
struct Arguments {
    std::size_t count     = 0;
    std::size_t lastBegin = 0;
};
[[nodiscard]] Arguments argumentsOf(std::string const& code, std::size_t open, std::size_t close) {
    Arguments out{0, open + 1};
    int  depth = 0;
    bool any   = false;
    for (std::size_t k = open + 1; k < close; ++k) {
        char const c = code[k];
        if (c == '(' || c == '[' || c == '{') ++depth;
        if (c == ')' || c == ']' || c == '}') --depth;
        if (c == ',' && depth == 0) {
            ++out.count;
            out.lastBegin = k + 1;
            continue;
        }
        if (!isSpace(c)) any = true;
    }
    if (any) ++out.count;
    return out;
}

// The first string literal of raw[from, to), adjacent literals joined -- read from the RAW text, skipping its
// comments. Nothing when the span states none.
[[nodiscard]] std::optional<std::string> firstLiteral(std::string const& raw, std::size_t from, std::size_t to) {
    std::optional<std::string> out;
    std::size_t                k = from;
    while (k < to) {
        if (raw.compare(k, 2, "//") == 0) {
            std::size_t const nl = raw.find('\n', k);
            k = nl == std::string::npos ? to : nl + 1;
            continue;
        }
        if (raw.compare(k, 2, "/*") == 0) {
            std::size_t const end = raw.find("*/", k + 2);
            k = end == std::string::npos ? to : end + 2;
            continue;
        }
        if (raw[k] == '"') {
            std::string piece;
            std::size_t j = k + 1;
            while (j < to && raw[j] != '"') {
                if (raw[j] == '\\' && j + 1 < to) {
                    char const e = raw[j + 1];
                    piece += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    j += 2;
                    continue;
                }
                piece += raw[j++];
            }
            out = out.value_or(std::string{}) + piece;
            k = j + 1;
            continue;
        }
        if (out.has_value() && !isSpace(raw[k])) break;   // the literal sequence ended
        ++k;
    }
    return out;
}

[[nodiscard]] std::string oneLine(std::string const& raw, std::size_t from, std::size_t to) {
    std::string s;
    bool        space = false;
    for (std::size_t k = from; k < to && k < raw.size(); ++k) {
        if (isSpace(raw[k])) {
            space = true;
            continue;
        }
        if (space && !s.empty()) s += ' ';
        space = false;
        s += raw[k];
    }
    return s;
}

[[nodiscard]] Site siteAt(std::string const& file, std::string const& raw, std::string const& code, std::size_t word,
                          std::size_t close, Arguments const& a) {
    return Site{file, lineAt(code, word), firstLiteral(raw, a.lastBegin, close), oneLine(raw, word, close + 1)};
}

constexpr std::string_view kNewSpace = "NewSpaceSymbolIds";

// Every take site one source states, in source order.
[[nodiscard]] std::vector<Site> sitesIn(std::string const& file, std::string const& raw) {
    std::string const code = dss::test::codeOnly(raw);
    std::vector<std::pair<std::size_t, Site>> found;
    // (a) `.take(` / `->take(` of two arguments or more.
    for (std::size_t q = code.find("take"); q != std::string::npos; q = code.find("take", q + 1)) {
        if (!wordAt(code, q, "take")) continue;
        std::size_t b = q;
        while (b > 0 && isSpace(code[b - 1])) --b;
        bool const member = (b >= 1 && code[b - 1] == '.') || (b >= 2 && code.compare(b - 2, 2, "->") == 0);
        if (!member) continue;
        std::size_t const open = skipSpace(code, q + 4);
        if (open >= code.size() || code[open] != '(') continue;
        std::size_t const close = closingOf(code, open);
        if (close == std::string::npos) continue;
        Arguments const a = argumentsOf(code, open, close);
        if (a.count < 2) continue;
        found.emplace_back(q, siteAt(file, raw, code, q, close, a));
    }
    // (b) a new space's door, constructed -- outside the class's own body.
    std::vector<std::pair<std::size_t, std::size_t>> classBodies;
    std::vector<std::size_t>                         uses;
    for (std::size_t q = code.find(kNewSpace); q != std::string::npos; q = code.find(kNewSpace, q + 1)) {
        if (!wordAt(code, q, kNewSpace)) continue;
        std::size_t b = q;
        while (b > 0 && isSpace(code[b - 1])) --b;
        std::size_t a = b;
        while (a > 0 && isWordChar(code[a - 1])) --a;
        std::string_view const before{code.data() + a, b - a};
        if (before == "class" || before == "struct") {
            std::size_t const brace = skipSpace(code, q + kNewSpace.size());
            if (brace < code.size() && code[brace] == '{') classBodies.emplace_back(brace, closingOf(code, brace));
            continue;
        }
        uses.push_back(q);
    }
    for (std::size_t const q : uses) {
        bool const inBody = std::any_of(classBodies.begin(), classBodies.end(),
                                        [&](auto const& body) { return q > body.first && q < body.second; });
        if (inBody) continue;
        std::size_t open = skipSpace(code, q + kNewSpace.size());
        if (open < code.size() && isIdentStart(code[open])) {
            std::size_t name = open;
            while (name < code.size() && isWordChar(code[name])) ++name;
            if (std::string_view{code.data() + open, name - open} == "const") continue;   // `NewSpaceSymbolIds const&`
            open = skipSpace(code, name);
        }
        if (open >= code.size() || (code[open] != '{' && code[open] != '(')) continue;   // a type, not a construction
        std::size_t const close = closingOf(code, open);
        if (close == std::string::npos) continue;
        found.emplace_back(q, siteAt(file, raw, code, q, close, argumentsOf(code, open, close)));
    }
    std::ranges::sort(found, {}, &std::pair<std::size_t, Site>::first);
    std::vector<Site> out;
    for (auto& [at, s] : found) out.push_back(std::move(s));
    return out;
}

// What the universe refuses: the sites against the table, both ways.
[[nodiscard]] std::vector<std::string> refusals(std::vector<Site> const& sites,
                                                std::span<dss::test::LinkTakeSite const> table) {
    std::vector<std::string>                              out;
    std::map<std::pair<std::string, std::string>, std::vector<std::size_t>> at;
    for (Site const& s : sites) {
        if (!s.label.has_value()) {
            out.push_back(s.file + ", line " + std::to_string(s.line) + ": a take site whose label is no literal: `" +
                          s.spelled + "`");
            continue;
        }
        at[{s.file, *s.label}].push_back(s.line);
    }
    std::set<std::pair<std::string, std::string>> rows;
    for (auto const& r : table) {
        std::pair<std::string, std::string> const key{std::string{r.file}, std::string{r.label}};
        if (!rows.insert(key).second) out.push_back("a row the table states twice: " + key.first + " `" + key.second + "`");
    }
    for (auto const& [key, lines] : at) {
        if (lines.size() > 1) {
            std::string where;
            for (std::size_t const l : lines) where += " " + std::to_string(l);
            out.push_back(key.first + ": the label `" + key.second + "` stands at " + std::to_string(lines.size()) +
                          " sites (lines" + where + "): a refusal must name one");
        }
        if (!rows.contains(key))
            out.push_back(key.first + ", line " + std::to_string(lines.front()) + ": a take site the table does not hold: `" +
                          key.second + "` -- give it a row, and a drive in test_module_door_take_sites.cpp");
    }
    for (auto const& key : rows)
        if (!at.contains(key)) out.push_back("a row no site of src/ states: " + key.first + " `" + key.second + "`");
    return out;
}

[[nodiscard]] std::string listed(std::vector<std::string> const& v) {
    std::string s;
    for (auto const& x : v) s += "\n  " + x;
    return s;
}

[[nodiscard]] std::string readAll(dss::core::HostPath const& p) {
    // A source the scan cannot open is no clean source: the scan stops, naming it and the OS's reason (H6c2 (d)).
    auto opened = dss::core::openInputStream(p, std::ios::binary);
    if (!opened)
        throw std::runtime_error("the scan could not open '" + p.spelling().generic() + "': "
                                 + dss::core::reasonText(opened.error()));
    return {std::istreambuf_iterator<char>{*opened}, std::istreambuf_iterator<char>{}};
}

}  // namespace

// THE UNIVERSE: every take site src/ states is a row of the table, and every row a site.
TEST(ModuleDoorTakeSitesGuard, EveryTakeSiteOfSrcIsARowAndEveryRowASite) {
    auto const        root = dss::test::repoRoot();
    std::vector<Site> sites;
    std::size_t       read = 0;
    auto const walk = dss::core::walkTree(root / "src", [&](dss::core::TreeEntry const& entry) {
        if (entry.kind != dss::core::EntryKind::File) return dss::core::WalkStep::Continue;
        std::string const relative = "src/" + entry.relative.generic();
        if (relative.ends_with(".cpp") || relative.ends_with(".hpp")) {
            ++read;
            for (Site& s : sitesIn(relative, readAll(entry.at))) sites.push_back(std::move(s));
        }
        return dss::core::WalkStep::Continue;
    });
    ASSERT_FALSE(walk.error) << "the walk of src/ stopped at " << walk.where.spelling().generic() << ": "
                             << walk.error.message();
    EXPECT_GE(read, 400u) << "the guard read only " << read << " sources";
    auto const refused = refusals(sites, dss::test::kLinkTakeSites);
    EXPECT_TRUE(refused.empty()) << refused.size() << " take site(s) and row(s) out of the universe:" << listed(refused);
}

// THE SCAN ITSELF, on synthetic sources: every site form once -- a literal, a format string split over adjacent
// literals and lines, a label no literal states, a call through a pointer, a new space's door named and temporary --
// and the forms it must not read: the words in a comment and a string, a `take()` of no argument, a free `take`, the
// door's own class body, and the class name used as a type.
TEST(ModuleDoorTakeSitesGuard, TheScanFindsEverySiteFormAndNothingElse) {
    std::string const src =
        "// ids.take(reporter, \"x: in a comment\") is no site\n"                             // 1
        "char const* s = \"ids.take(reporter, \\\"x: in a string\\\")\";\n"                   // 2
        "void f(ModuleSymbolIds& ids, DiagnosticReporter& reporter, std::string_view who) {\n" // 3
        "  auto a = ids.take(reporter, \"x: a literal\");\n"                                  // 4  site
        "  auto b = ModuleSymbolIds{m}.take(2, reporter,\n"                                   // 5  site
        "      std::format(\"x: the {} id(s)\" \" of a pair\", 2));\n"                         // 6
        "  auto c = ids.take(reporter, who);\n"                                               // 7  site, no literal
        "  auto d = end.take();\n"                                                            // 8
        "  auto e = p->take(reporter, /* the label */ \"x: through a pointer\");\n"           // 9  site
        "  NewSpaceSymbolIds fresh{SymbolIdEnd{}, reporter, \"x: a new space\"};\n"           // 10 site
        "  use(NewSpaceSymbolIds(SymbolIdEnd{}, reporter, \"x: a temporary\"));\n"            // 11 site
        "  take(reporter, \"x: a free function\");\n"                                         // 12
        "}\n"                                                                                 // 13
        "class NewSpaceSymbolIds {\n"                                                         // 14
        "    NewSpaceSymbolIds(SymbolIdEnd start, DiagnosticReporter& reporter, std::string_view who) noexcept;\n"
        "    NewSpaceSymbolIds(NewSpaceSymbolIds const&) = delete;\n"                         // 16
        "};\n"                                                                                // 17
        "void g(NewSpaceSymbolIds& ids, NewSpaceSymbolIds const& other);\n";                  // 18
    std::string got;
    for (Site const& s : sitesIn("src/x/a.cpp", src))
        got += " " + std::to_string(s.line) + "=" + (s.label.has_value() ? *s.label : std::string{"<none>"});
    EXPECT_EQ(got, " 4=x: a literal 5=x: the {} id(s) of a pair 7=<none> 9=x: through a pointer 10=x: a new space "
                   "11=x: a temporary");
}

// THE JUDGEMENT, both ways, on a synthetic universe: a site the table lacks, a row no site states, a label no literal
// states, one label at two sites, a row stated twice -- each named once; and the agreeing pair is refused nothing.
TEST(ModuleDoorTakeSitesGuard, TheJudgementRefusesEachWayOutOfTheUniverseByName) {
    std::vector<Site> const sites = {
        {"src/x/a.cpp", 4, "x: held", "ids.take(reporter, \"x: held\")"},
        {"src/x/a.cpp", 7, "x: not held", "ids.take(reporter, \"x: not held\")"},
        {"src/x/a.cpp", 9, std::nullopt, "ids.take(reporter, who)"},
        {"src/x/b.cpp", 2, "x: twice", "ids.take(reporter, \"x: twice\")"},
        {"src/x/b.cpp", 5, "x: twice", "ids.take(reporter, \"x: twice\")"},
    };
    dss::test::LinkTakeSite const table[] = {
        {"src/x/a.cpp", "x: held"},
        {"src/x/b.cpp", "x: twice"},
        {"src/x/a.cpp", "x: no site"},
        {"src/x/b.cpp", "x: twice"},
    };
    EXPECT_EQ(listed(refusals(sites, table)),
              listed({"src/x/a.cpp, line 9: a take site whose label is no literal: `ids.take(reporter, who)`",
                      "a row the table states twice: src/x/b.cpp `x: twice`",
                      "src/x/a.cpp, line 7: a take site the table does not hold: `x: not held` -- give it a row, and a drive "
                      "in test_module_door_take_sites.cpp",
                      "src/x/b.cpp: the label `x: twice` stands at 2 sites (lines 2 5): a refusal must name one",
                      "a row no site of src/ states: src/x/a.cpp `x: no site`"}));
    dss::test::LinkTakeSite const agreeing[] = {{"src/x/a.cpp", "x: held"}};
    EXPECT_TRUE(refusals({sites.front()}, agreeing).empty()) << "CONTROL: a site and its row agree";
}
