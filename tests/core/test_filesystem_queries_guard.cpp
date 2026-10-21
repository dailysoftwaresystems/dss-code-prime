// THE SOURCE TREE ASKS THE SUBSTRATE FOR EVERY FILESYSTEM KIND, SIZE, EXISTENCE AND RESOLUTION -- the closure
// of [[D-CORE-MINGW-BUILDS-WALKS-AND-REMOVALS-THAT-FOLLOW-A-LINK]].
//
// ✔MEASURED 2026-10-20 (run 20261020-030701-a8b31d50, and the cases that row added): the libraries that build
// DSS answer a LINK differently. libstdc++ on Windows reads a link's OWN attributes -- a dangling link is a
// regular file there, a link to a file is 0 bytes long -- and resolves no link (`canonical`, `weakly_canonical`
// hand it back unchanged); MS STL and every POSIX library follow it. So one source gave two answers, and every
// such question in src/ is now asked of the substrate (src/core/substrate/path_identity.hpp: `entryKind`,
// `reachedKind`, `reachedFileSize`, `reachedPath`, `walkTree`, `removeTree`), which asks the OS on Windows.
//
// THIS GUARD refuses the library's form anywhere else, naming the substrate call to use: a namespace-qualified
// `file_size` / `is_regular_file` / `is_directory` / `exists` / `status` / `symlink_status` / `canonical` /
// `weakly_canonical` call (through `std::filesystem` or any alias of it the file declares), the same question
// asked of a `directory_entry`, and a using-declaration or using-directive that would let such a call go
// unqualified. It has no exemption list: the substrate is the one directory left out.
//
// SCOPE: every *.cpp and *.hpp under src/ (src/dss-config's C runtime holds no std::filesystem), outside
// src/core/substrate/. Comments and string literals are not code, and are not read.

#include "core/substrate/path_identity.hpp"   // walkTree: the substrate's own walk, never through a link
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Each refused question and the substrate's answer to it.
std::map<std::string, std::string> const kAsk = {
    {"file_size", "core::reachedFileSize(p, ec)"},
    {"is_regular_file", "core::reachedKind(p, ec) == core::EntryKind::File"},
    {"is_directory", "core::reachedKind(p, ec) == core::EntryKind::Directory"},
    {"exists", "core::reachedKind(p, ec) != core::EntryKind::Absent (the entry itself: core::entryKind)"},
    {"status", "core::reachedKind(p, ec)"},
    {"symlink_status", "core::entryKind(p, ec)"},
    {"canonical", "core::reachedPath(p, ec), or core::PathIdentity for an identity"},
    {"weakly_canonical", "core::reachedPath(p, ec), or core::PathIdentity for an identity"},
};

// The questions a `directory_entry` answers itself (no `canonical` among them).
std::set<std::string> const kEntryMembers = {"file_size", "is_regular_file", "is_directory", "exists", "status",
                                             "symlink_status"};

// Whether the `R` at `i` opens a raw string literal: it begins a token, alone or after an encoding prefix.
[[nodiscard]] bool opensRawLiteral(std::string const& text, std::size_t i) {
    auto const ident = [&](std::size_t k) {
        unsigned char const ch = static_cast<unsigned char>(text[k]);
        return std::isalnum(ch) != 0 || ch == '_';
    };
    std::size_t start = i;
    if (start >= 2 && text.compare(start - 2, 2, "u8") == 0) start -= 2;
    else if (start >= 1 && (text[start - 1] == 'u' || text[start - 1] == 'U' || text[start - 1] == 'L')) start -= 1;
    return start == 0 || !ident(start - 1);
}

// `text` with every comment and every string or character literal blanked to spaces, its newlines kept -- so a
// line number still names the source line, and prose that names a call is never read as one.
[[nodiscard]] std::string codeOnly(std::string const& text) {
    std::string out = text;
    std::size_t i   = 0;
    auto const  blank = [&](std::size_t from, std::size_t to) {
        for (std::size_t k = from; k < to && k < out.size(); ++k)
            if (out[k] != '\n') out[k] = ' ';
    };
    while (i < text.size()) {
        char const c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            std::size_t const end = text.find('\n', i);
            std::size_t const stop = end == std::string::npos ? text.size() : end;
            blank(i, stop);
            i = stop;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            std::size_t const end = text.find("*/", i + 2);
            std::size_t const stop = end == std::string::npos ? text.size() : end + 2;
            blank(i, stop);
            i = stop;
        } else if (c == 'R' && i + 1 < text.size() && text[i + 1] == '"' && opensRawLiteral(text, i)) {
            // A raw string: R"delim( ... )delim"
            std::size_t const open = text.find('(', i + 2);
            if (open == std::string::npos) break;
            std::string const delim = ")" + text.substr(i + 2, open - (i + 2)) + "\"";
            std::size_t const end = text.find(delim, open + 1);
            std::size_t const stop = end == std::string::npos ? text.size() : end + delim.size();
            blank(i, stop);
            i = stop;
        } else if (c == '"' || (c == '\'' && !(i > 0 && std::isalnum(static_cast<unsigned char>(text[i - 1])) != 0))) {
            // (A `'` right after a digit or a letter is a digit separator or a prefixed literal's quote, not
            // the start of a character literal to blank.)
            std::size_t k = i + 1;
            while (k < text.size() && text[k] != c && text[k] != '\n') k += text[k] == '\\' ? 2 : 1;
            std::size_t const stop = std::min(text.size(), k + 1);
            blank(i, stop);
            i = stop;
        } else {
            ++i;
        }
    }
    return out;
}

struct Finding {
    std::string where;   // `<file>:<line>`
    std::string call;    // what was written
    std::string ask;     // the substrate call to use
};

// Every refused form in one source's code, `file` naming it in each finding.
[[nodiscard]] std::vector<Finding> refusedIn(std::string const& file, std::string const& text) {
    std::vector<std::string> lines;
    {
        std::istringstream in{codeOnly(text)};
        for (std::string line; std::getline(in, line);) lines.push_back(std::move(line));
    }
    // A line where neither a refused name nor `filesystem` appears holds no refused form: only the rest are
    // matched, which keeps a scan of the whole tree to the few lines that can hold one.
    auto const mayHold = [](std::string const& line) {
        if (line.find("filesystem") != std::string::npos) return true;
        for (auto const& entry : kAsk)
            if (line.find(entry.first) != std::string::npos) return true;
        return false;
    };
    // The names `std::filesystem` goes by in this file: itself, and every alias the file declares.
    std::set<std::string> qualifiers = {R"(std\s*::\s*filesystem)"};
    static std::regex const alias{R"(namespace\s+(\w+)\s*=\s*(?:::\s*)?std\s*::\s*filesystem\s*;)"};
    for (auto const& line : lines) {
        if (line.find("filesystem") == std::string::npos) continue;
        for (std::sregex_iterator it{line.begin(), line.end(), alias}, end; it != end; ++it)
            qualifiers.insert((*it)[1].str());
    }
    std::string names;
    for (auto const& entry : kAsk) names += (names.empty() ? "" : "|") + entry.first;
    std::string members;
    for (auto const& name : kEntryMembers) members += (members.empty() ? "" : "|") + name;
    std::string quals;
    for (auto const& q : qualifiers) quals += (quals.empty() ? "" : "|") + q;

    std::regex const qualified{R"re((?:^|[^\w:])(?:)re" + quals + R"re()\s*::\s*()re" + names + R"re()\s*\()re"};
    std::regex const member{R"re((?:\.|->)\s*()re" + members + R"re()\s*\()re"};
    static std::regex const usingAll{R"(using\s+namespace\s+(?:::\s*)?std\s*::\s*filesystem\b)"};
    std::regex const usingOne{R"re(using\s+(?:::\s*)?std\s*::\s*filesystem\s*::\s*()re" + names + R"re()\b)re"};

    std::vector<Finding> out;
    for (std::size_t n = 0; n < lines.size(); ++n) {
        std::string const& line = lines[n];
        if (!mayHold(line)) continue;
        std::string const where = file + ":" + std::to_string(n + 1);
        for (auto const* re : {&qualified, &member, &usingOne}) {
            for (std::sregex_iterator it{line.begin(), line.end(), *re}, end; it != end; ++it) {
                std::string const name = (*it)[1].str();
                out.push_back({where, name, kAsk.at(name)});
            }
        }
        if (std::regex_search(line, usingAll))
            out.push_back({where, "using namespace std::filesystem", "a qualified call, asked of the substrate"});
    }
    return out;
}

// Whether `relative` (a generic path under the repository) is a source this guard reads.
[[nodiscard]] bool isGuarded(std::string const& relative) {
    if (relative.rfind("src/", 0) != 0) return false;
    if (relative.rfind("src/core/substrate/", 0) == 0) return false;
    std::string const ext = fs::path{relative}.extension().string();
    return ext == ".cpp" || ext == ".hpp";
}

[[nodiscard]] std::string readAll(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

} // namespace

// THE TREE: no source outside the substrate asks the library.
TEST(FilesystemQueriesGuard, TheSourceTreeAsksTheSubstrateForEveryFilesystemQuery) {
    fs::path const root = dss::test::repoRoot();
    std::vector<Finding> found;
    std::size_t          read = 0;
    auto const walk = dss::core::walkTree(root / "src", [&](dss::core::TreeEntry const& entry) {
        if (entry.kind != dss::core::EntryKind::File) return dss::core::WalkStep::Continue;
        std::string const relative = entry.path.lexically_relative(root).generic_string();
        if (!isGuarded(relative)) return dss::core::WalkStep::Continue;
        ++read;
        auto more = refusedIn(relative, readAll(entry.path));
        found.insert(found.end(), more.begin(), more.end());
        return dss::core::WalkStep::Continue;
    });
    ASSERT_FALSE(walk.error) << "the walk of src/ stopped at " << walk.where.generic_string() << ": "
                             << walk.error.message();
    // Not a vacuous pass: the tree holds hundreds of sources (535 at the guard's birth, 2026-10-20).
    EXPECT_GE(read, 400u) << "the guard read only " << read << " sources under " << (root / "src").generic_string();
    std::string listed;
    for (auto const& f : found) listed += "\n  " + f.where + ": `" + f.call + "` -- ask the substrate: " + f.ask;
    EXPECT_TRUE(found.empty()) << found.size() << " filesystem question(s) asked of the library outside "
                               << "src/core/substrate/:" << listed;
}

// THE SCAN ITSELF, on a synthetic source holding every refused form once, each in a context it must and must not
// read: the qualified call through `std::filesystem` and through an alias, the entry's member, the using forms --
// and the same words inside a comment, a string and a raw string, plus a call the guard does not refuse.
TEST(FilesystemQueriesGuard, TheScanRefusesEveryFormItNamesAndNothingElse) {
    std::string const source =
        "namespace stdfs = std::filesystem;\n"
        "bool a(std::filesystem::path const& p) { return std::filesystem::exists(p); }\n"   // 2: exists
        "auto b(stdfs::path const& p) { return stdfs::file_size(p); }\n"                     // 3: file_size
        "bool c(stdfs::path const& p) { return stdfs::is_regular_file(p); }\n"               // 4: is_regular_file
        "bool d(stdfs::path const& p) { return stdfs::is_directory(p); }\n"                  // 5: is_directory
        "auto e(stdfs::path const& p) { return stdfs::status(p); }\n"                        // 6: status
        "auto f(stdfs::path const& p) { return stdfs::symlink_status(p); }\n"                // 7: symlink_status
        "auto g(stdfs::path const& p) { return stdfs::canonical(p); }\n"                     // 8: canonical
        "auto h(stdfs::path const& p) { return stdfs::weakly_canonical(p); }\n"              // 9: weakly_canonical
        "bool i(stdfs::directory_entry const& e) { return e.is_regular_file(); }\n"         // 10: is_regular_file
        "bool j(stdfs::directory_iterator const& it) { return it->is_directory(); }\n"      // 11: is_directory
        "using std::filesystem::exists;\n"                                                   // 12: exists
        "using namespace std::filesystem;\n"                                                 // 13: using namespace
        "// stdfs::exists(p) in a comment, /* fs::status(p) */ too\n"
        "char const* k = \"std::filesystem::exists(p) in a string\";\n"
        "char const* l = R\"x(stdfs::file_size(p) in a raw string)x\";\n"
        "void m(stdfs::path const& p) { stdfs::create_directories(p); stdfs::absolute(p); }\n"
        "bool n(Cache& c) { return c.contains(1) && myexists(2) && ns::exists_like(3); }\n";
    auto const        found = refusedIn("synthetic.cpp", source);
    std::string       got;
    for (auto const& f : found) got += " " + f.where.substr(f.where.find(':') + 1) + "=" + f.call;
    std::string const want = " 2=exists 3=file_size 4=is_regular_file 5=is_directory 6=status 7=symlink_status "
                             "8=canonical 9=weakly_canonical 10=is_regular_file 11=is_directory 12=exists "
                             "13=using namespace std::filesystem";
    EXPECT_EQ(got, want) << "the scan refused " << found.size() << " of the 12 forbidden forms:" << got;
}

// THE ONE DIRECTORY LEFT OUT is the substrate, and only C++ sources under src/ are read.
TEST(FilesystemQueriesGuard, TheSubstrateIsTheOneDirectoryLeftOut) {
    EXPECT_TRUE(isGuarded("src/core/types/glob_match.cpp"));
    EXPECT_TRUE(isGuarded("src/program/program.hpp"));
    EXPECT_FALSE(isGuarded("src/core/substrate/path_identity.cpp")) << "the substrate is where the OS is asked";
    EXPECT_FALSE(isGuarded("src/dss-config/runtime/platform/src/dirent.c"));
    EXPECT_FALSE(isGuarded("tests/core/test_header_name_matching.cpp"));
    EXPECT_TRUE(isGuarded("src/core/substrate_like/x.cpp")) << "only the substrate directory itself is left out";
}
