// NO SOURCE OF THE TREE MATCHES TEXT WITH `std::regex` -- the engine guard of the class
// tests/test_support/text_scan.hpp states (P69 lane `hm`, fold H6c2; the orchestrator's ledger 645).
//
// ★ THE CLASS. ✔MEASURED on the windows-x86_64-debug leg (P69 lane `cs`, fold f26d): a repo guard that ran
// `std::regex` over whole source files died of a SEGFAULT on the mingw-w64 build. libstdc++'s regex executor recurses
// once per character a match consumes, so its stack depth follows the INPUT -- the input-proportional recursion the
// compiler itself refuses -- and a Windows main thread's smaller stack overflows first. So every text the tree
// matches, in a test or in the compiler, is matched by a loop (text_scan.hpp's scanners: work proportional to the
// text, stack depth constant), and this guard keeps it so.
//
// THE FORMS REFUSED, each wherever it stands:
//   * an inclusion of the header, in any spelling of the directive (`#include <regex>`, `# include<regex>`,
//     `#include "regex"` -- a quoted name falls back to the header's own search);
//   * a name of the header's vocabulary ([re.syn]) qualified by `std` -- `std::regex`, `::std :: smatch`,
//     `std::pmr::smatch`, `std::regex_constants::icase`, a using-declaration (`using std::regex_search;`).
// It has NO exemption list.
//
// SCOPE: every C++ source (*.cpp, *.hpp, *.h, *.inc) under src/, tests/ and integrated_tests/, read with its
// comments and literals blanked (tests/test_support/source_code_only.hpp), so prose that names a form is never read
// as one. The walk is the substrate's, and it is floored per root: a walk that read too little fails, never passes.

#include "core/substrate/path_identity.hpp"   // walkTree and openInputStream: the substrate's walk and reads
#include "repo_root.hpp"
#include "source_code_only.hpp"
#include "text_scan.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

using dss::test::skipSpace;

// The vocabulary of <regex> ([re.syn]): every name the header declares in `std` (and the match results `std::pmr`
// names again).
std::set<std::string, std::less<>> const kRegexNames = {
    "regex_constants",      "regex_error",           "regex_traits",          "basic_regex",
    "regex",                "wregex",                "sub_match",             "csub_match",
    "wcsub_match",          "ssub_match",            "wssub_match",           "match_results",
    "cmatch",               "wcmatch",               "smatch",                "wsmatch",
    "regex_match",          "regex_search",          "regex_replace",         "regex_iterator",
    "cregex_iterator",      "wcregex_iterator",      "sregex_iterator",       "wsregex_iterator",
    "regex_token_iterator", "cregex_token_iterator", "wcregex_token_iterator", "sregex_token_iterator",
    "wsregex_token_iterator"};

// One source the guard reads: where it lies under the repository (generic spelling), its text, and its code.
struct Source {
    std::string relative;
    std::string text;
    std::string code;
};

[[nodiscard]] Source sourceOf(std::string relative, std::string text) {
    std::string code = dss::test::codeOnly(text);
    return Source{std::move(relative), std::move(text), std::move(code)};
}

struct Finding {
    std::string where;   // `<file> line <line>`
    std::string form;    // what was written
};

// Where white space and a `::` after `i` leave the text, white space after it skipped -- or nothing where no `::`
// follows.
[[nodiscard]] std::optional<std::size_t> pastColons(std::string_view s, std::size_t i) {
    std::size_t const colons = skipSpace(s, i);
    if (s.compare(colons, 2, "::") != 0) return std::nullopt;
    return skipSpace(s, colons + 2);
}

// Whether a line is a directive including the header: its CODE (literals blanked) says a directive stands, and its
// TEXT as written names the header -- `<regex>` or `"regex"`, which the code blanks (codeOnly keeps every offset, so
// one names the other).
[[nodiscard]] bool includesRegex(std::string_view code, std::string_view text) {
    std::size_t const hash = skipSpace(code, 0);
    if (hash >= code.size() || code[hash] != '#') return false;
    std::size_t const keyword = skipSpace(code, hash + 1);
    if (!dss::test::wordAt(code, keyword, "include")) return false;
    std::size_t const name = skipSpace(text, keyword + std::string_view{"include"}.size());
    return text.compare(name, 7, "<regex>") == 0 || text.compare(name, 7, "\"regex\"") == 0;
}

// Every name of the header's vocabulary a code line qualifies by `std` -- `std :: NAME`, `std :: pmr :: NAME` -- as
// written without its white space, in order.
[[nodiscard]] std::vector<std::string> qualifiedRegexNames(std::string_view code) {
    std::vector<std::string> out;
    dss::test::forEachIdentifier(code, [&](std::size_t begin, std::size_t end) {
        if (code.substr(begin, end - begin) != "std") return end;
        std::optional<std::size_t> const first = pastColons(code, end);
        if (!first) return end;
        std::size_t      stop = dss::test::identEnd(code, *first);
        std::string_view name = code.substr(*first, stop - *first);
        std::string      form = "std::";
        if (name == "pmr") {
            std::optional<std::size_t> const second = pastColons(code, stop);
            if (!second) return stop;
            form += "pmr::";
            stop = dss::test::identEnd(code, *second);
            name = code.substr(*second, stop - *second);
        }
        if (kRegexNames.count(name) != 0) out.push_back(form + std::string{name});
        return stop;
    });
    return out;
}

// Every refused form across `sources`, in their order and each source's line order.
[[nodiscard]] std::vector<Finding> refusedAcross(std::vector<Source> const& sources) {
    std::vector<Finding> found;
    for (Source const& s : sources) {
        std::size_t line = 1;
        for (std::size_t at = 0; at <= s.code.size(); ++line) {
            std::size_t const newline = s.code.find('\n', at);
            std::size_t const end     = newline == std::string::npos ? s.code.size() : newline;
            std::string_view const code = std::string_view{s.code}.substr(at, end - at);
            std::string_view const text = std::string_view{s.text}.substr(at, end - at);
            std::string const      where = s.relative + " line " + std::to_string(line);
            if (includesRegex(code, text)) found.push_back({where, "#include <regex>"});
            for (std::string& name : qualifiedRegexNames(code)) found.push_back({where, std::move(name)});
            if (newline == std::string::npos) break;
            at = newline + 1;
        }
    }
    return found;
}

[[nodiscard]] std::string spelled(std::vector<Finding> const& found) {
    std::string got;
    for (auto const& f : found) got += " " + f.where + "=" + f.form;
    return got;
}

// A source's bytes, read through the home (H6c): `p` is the walk's LISTED value, never its text re-resolved.
[[nodiscard]] std::string readAll(dss::core::HostPath const& p) {
    // A source the scan cannot open is no clean source: the scan stops, naming it and the OS's reason (H6c2 (d)).
    auto opened = dss::core::openInputStream(p, std::ios::binary);
    if (!opened)
        throw std::runtime_error("the scan could not open '" + p.spelling().generic() + "': "
                                 + dss::core::reasonText(opened.error()));
    return {std::istreambuf_iterator<char>{*opened}, std::istreambuf_iterator<char>{}};
}

} // namespace

// THE TREE: no C++ source under src/, tests/ or integrated_tests/ includes <regex> or names its vocabulary.
TEST(NoRegexGuard, NoSourceOfTheTreeMatchesTextWithStdRegex) {
    fs::path const                     root = dss::test::repoRoot();
    std::vector<Source>                sources;
    std::map<std::string, std::size_t> read;
    for (char const* top : {"src", "tests", "integrated_tests"}) {
        auto const walk = dss::core::walkTree(root / top, [&](dss::core::TreeEntry const& entry) {
            if (entry.kind != dss::core::EntryKind::File) return dss::core::WalkStep::Continue;
            std::string const ext = entry.at.extension().string();
            if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".inc") return dss::core::WalkStep::Continue;
            sources.push_back(sourceOf(std::string{top} + "/" + entry.relative.generic(), readAll(entry.at)));
            ++read[top];
            return dss::core::WalkStep::Continue;
        });
        ASSERT_FALSE(walk.error) << "the walk of " << top << "/ stopped at " << walk.where.spelling().generic() << ": "
                                 << walk.error.message();
    }
    // Not a vacuous pass: each root holds the sources it held at the guard's birth (src 552, tests 765,
    // integrated_tests 1, P69 H6c2), and the guard reads itself.
    EXPECT_GE(read["src"], 400u) << "the guard read only " << read["src"] << " sources under src/";
    EXPECT_GE(read["tests"], 500u) << "the guard read only " << read["tests"] << " sources under tests/";
    EXPECT_GE(read["integrated_tests"], 1u) << "the guard read no source under integrated_tests/";
    EXPECT_TRUE(std::any_of(sources.begin(), sources.end(), [](Source const& s) {
        return s.relative == "tests/core/test_no_regex_guard.cpp";
    })) << "the guard did not read itself: the scope is short";
    auto const  found = refusedAcross(sources);
    std::string listed;
    for (auto const& f : found) listed += "\n  " + f.where + ": `" + f.form + "` -- match it with a loop (text_scan.hpp)";
    EXPECT_TRUE(found.empty()) << found.size() << " use(s) of <regex> in the tree:" << listed;
}

// THE SCAN ITSELF, on synthetic sources holding every refused form, each in a context it must and must not read: the
// directive in every spelling, a qualified name of every shape -- and the same words in a comment, a string, a raw
// string, another header's name, another qualifier, another word, and a test of the header's presence.
TEST(NoRegexGuard, TheScanRefusesEveryFormItNamesAndNothingElse) {
    std::vector<Source> const sources = {
        sourceOf("src/a.cpp", "#include <regex>\n"),
        sourceOf("src/b.cpp", "  #  include\t<regex>\n#include<regex>\n#include \"regex\"\n"),
        sourceOf("tests/c.cpp", "std::regex const r{\"x\"};\nauto m = ::std :: smatch{};\n"),
        sourceOf("tests/d.cpp", "using std::regex_search;\nstd::pmr::smatch p;\nint f = std::regex_constants::icase;\n"),
        sourceOf("integrated_tests/e.cpp", "std::vector<std::sregex_iterator> v; std::basic_regex<char> w;\n"),
        // Not refused: prose, literals, other headers, other qualifiers, other words.
        sourceOf("src/f.cpp", "// #include <regex> and std::regex in a comment\n/* std::smatch */\n"),
        sourceOf("src/g.cpp", R"~(auto s = "std::regex";
auto t = R"(
#include <regex>
std::wregex)";
)~"),
        sourceOf("src/h.cpp", "#include <regex_utils.hpp>\n#include <regexp>\n#include \"my/regex\"\n"),
        sourceOf("src/i.cpp", "mystd::regex a; stdx::regex b; dss::regex c; std::regexp d; std::string regex;\n"),
        sourceOf("src/j.cpp", "#if __has_include(<regex>)\n#endif\nint regex_search(int); std::pmr::string k;\n"),
    };
    EXPECT_EQ(spelled(refusedAcross(sources)),
              " src/a.cpp line 1=#include <regex>"
              " src/b.cpp line 1=#include <regex> src/b.cpp line 2=#include <regex> src/b.cpp line 3=#include <regex>"
              " tests/c.cpp line 1=std::regex tests/c.cpp line 2=std::smatch"
              " tests/d.cpp line 1=std::regex_search tests/d.cpp line 2=std::pmr::smatch"
              " tests/d.cpp line 3=std::regex_constants"
              " integrated_tests/e.cpp line 1=std::sregex_iterator integrated_tests/e.cpp line 1=std::basic_regex");
}
