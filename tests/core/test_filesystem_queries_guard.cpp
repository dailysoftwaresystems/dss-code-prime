// THE SOURCE TREE ASKS THE SUBSTRATE EVERY FILESYSTEM QUESTION WHOSE ANSWER DEPENDS ON HOW A LINK IS READ -- the
// closure of [[D-CORE-MINGW-BUILDS-WALKS-AND-REMOVALS-THAT-FOLLOW-A-LINK]].
//
// ★ THE CLASS. ✔MEASURED 2026-10-20 and 2026-10-21 (probe-reference-cc, every link made by the OS; runs named in
// the row): the libraries that build DSS answer a LINK differently. libstdc++ on Windows (MinGW-w64) reads a link's
// OWN attributes -- a dangling link is a regular file there, a link to a file is 0 bytes long and was written when
// the link was made, `is_symlink` is never true, `read_symlink` is "not implemented" -- and resolves no link
// (`canonical`, `weakly_canonical` and so `relative` / `proximate` hand it back unchanged); `remove_all` of a
// directory link EMPTIES ITS TARGET there, `permissions` through a link changes the link, and a copy told how to
// treat links (`copy_symlinks`, `skip_symlinks`, `create_symlinks`) reads the link's own kind; of a dangling link,
// `is_other` / `is_fifo` / `is_socket` / `is_block_file` / `is_character_file` / `is_empty` / `equivalent` /
// `hard_link_count` / `last_write_time` answer with no error where MS STL and libc++ report one. MS STL and every
// POSIX library follow the link. So one source gave two answers, and every such question in the scope below is
// asked of the substrate (src/core/substrate/path_identity.hpp), which asks the OS on Windows.
//
// ★ OUTSIDE THE CLASS, MEASURED THE SAME on both Windows libraries (and not refused): `remove` and `rename` (they
// act on the entry itself), `directory_iterator` (its listing; the questions its entries answer ARE refused, as
// members), `create_directory` / `create_directories`, `copy_file`, `copy` with no link option, and the questions
// that read no entry at all (`absolute`, `current_path`, `temp_directory_path`, `space`).
//
// THE FORMS REFUSED, each naming the substrate call to ask instead:
//   * a refused name MENTIONED through a qualifier that is `std::filesystem` -- in any spelling: `::std::filesystem`,
//     any qualifier ending in `filesystem` -- or any ALIAS of it declared anywhere in the scanned tree, an alias of
//     an alias included (a header's alias reaches every file that includes it). A mention, not only a call: taking
//     its address or a using-declaration of it is the same question waiting to be asked;
//   * the same question asked of a `directory_entry` (`.is_directory(`, `->last_write_time(`, ...);
//   * a `copy_options` that reads a link (`copy_symlinks`, `skip_symlinks`, `create_symlinks`);
//   * a using-directive of `std::filesystem` or of an alias of it;
//   * an UNQUALIFIED call of a refused function -- one argument at least, so argument-dependent lookup can reach
//     `std::filesystem` -- in a file whose include closure reaches `<filesystem>` (its quoted includes resolved as
//     the build resolves them: against its own directory, then src/, integrated_tests/ and tests/test_support/).
// It has no exemption list: the substrate is the one directory left out.
//
// SCOPE: every *.cpp and *.hpp under src/ (src/dss-config's C runtime holds no std::filesystem) outside
// src/core/substrate/, and under integrated_tests/ (whose runner compiles the substrate's unit beside it). The
// closure also reads tests/test_support/, which integrated_tests/ includes. Comments and string literals are not
// code, and are not read. ⓘ One quoted include resolves to nothing the guard reads -- the build stamp header
// generated at configure (program/dss_build_stamp_generated.hpp, ✔MEASURED 2026-10-21) -- and it includes no
// `<filesystem>`, so no closure is short for it.

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
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Each refused name and the substrate's answer to it.
std::map<std::string, std::string> const kAsk = {
    {"file_size", "core::reachedFileSize(p, ec)"},
    {"is_regular_file", "core::reachedKind(p, ec) == core::EntryKind::File"},
    {"is_directory", "core::reachedKind(p, ec) == core::EntryKind::Directory"},
    {"exists", "core::reachedKind(p, ec) != core::EntryKind::Absent (the entry itself: core::entryKind)"},
    {"status", "core::reachedKind(p, ec)"},
    {"symlink_status", "core::entryKind(p, ec)"},
    {"canonical", "core::reachedPath(p, ec), or core::PathIdentity for an identity"},
    {"weakly_canonical", "core::reachedPath(p, ec), or core::PathIdentity for an identity"},
    {"is_symlink", "core::entryKind(p, ec) == core::EntryKind::Link"},
    {"is_other", "core::reachedKind(p, ec) == core::EntryKind::Other (the entry itself: core::entryKind)"},
    {"is_fifo", "core::reachedKind(p, ec) == core::EntryKind::Other (the entry itself: core::entryKind)"},
    {"is_socket", "core::reachedKind(p, ec) == core::EntryKind::Other (the entry itself: core::entryKind)"},
    {"is_block_file", "core::reachedKind(p, ec) == core::EntryKind::Other (the entry itself: core::entryKind)"},
    {"is_character_file", "core::reachedKind(p, ec) == core::EntryKind::Other (the entry itself: core::entryKind)"},
    {"is_empty", "core::reachedFileSize(p, ec) for a file, core::walkTree for a directory"},
    {"equivalent", "core::reachedSameFile(a, b, ec)"},
    {"last_write_time", "core::reachedWriteTime(p, ec)"},
    {"hard_link_count", "a substrate question (core:: asks none yet): add it to path_identity.hpp"},
    {"read_symlink", "core::reachedPath(p, ec)"},
    {"remove_all", "core::removeTree(p)"},
    {"permissions", "core::addReachedPermissions(p, perms, ec)"},
    {"relative", "core::reachedRelative(p, base, ec)"},
    {"proximate", "core::reachedRelative(p, base, ec), or p where that is empty"},
    {"recursive_directory_iterator", "core::walkTree(root, visit)"},
    {"copy_symlinks", "a copy that asks the library no link question: copy_file of what core::walkTree hands out"},
    {"skip_symlinks", "a copy that asks the library no link question: copy_file of what core::walkTree hands out"},
    {"create_symlinks", "a copy that asks the library no link question: copy_file of what core::walkTree hands out"},
};

// The refused names that are FUNCTIONS: what an unqualified call can reach by argument-dependent lookup.
std::set<std::string> const kFunctions = {
    "file_size",      "is_regular_file", "is_directory",   "exists",          "status",
    "symlink_status", "canonical",       "weakly_canonical", "is_symlink",    "is_other",
    "is_fifo",        "is_socket",       "is_block_file",  "is_character_file", "is_empty",
    "equivalent",     "last_write_time", "hard_link_count", "read_symlink",   "remove_all",
    "permissions",    "relative",        "proximate"};

// The questions a `directory_entry` answers itself.
std::set<std::string> const kEntryMembers = {
    "file_size", "is_regular_file", "is_directory",      "exists",          "status",
    "symlink_status", "is_symlink", "is_other",          "is_fifo",         "is_socket",
    "is_block_file",  "is_character_file", "last_write_time", "hard_link_count"};

// The `copy_options` that read a link.
std::set<std::string> const kLinkOptions = {"copy_symlinks", "skip_symlinks", "create_symlinks"};

// The directories a quoted include is resolved against after the including file's own (the build's include roots
// for the scanned targets).
char const* const kIncludeRoots[] = {"src", "integrated_tests", "tests/test_support"};

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
// line number still names the source line, an offset still names the same character, and prose that names a call
// is never read as one.
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

[[nodiscard]] std::vector<std::string> linesOf(std::string const& text) {
    std::vector<std::string> lines;
    std::istringstream       in{text};
    for (std::string line; std::getline(in, line);) lines.push_back(std::move(line));
    return lines;
}

// One source the guard reads: where it lies under the repository (generic spelling), its text, and its code.
struct Source {
    std::string relative;
    std::string text;
    std::string code;
};

[[nodiscard]] Source sourceOf(std::string relative, std::string text) {
    std::string code = codeOnly(text);
    return Source{std::move(relative), std::move(text), std::move(code)};
}

// The last `::`-separated component of a qualifier as written (`::std :: filesystem` -> `filesystem`).
[[nodiscard]] std::string lastComponent(std::string const& qualifier) {
    std::string squeezed;
    for (char const c : qualifier)
        if (std::isspace(static_cast<unsigned char>(c)) == 0) squeezed += c;
    std::size_t const at = squeezed.rfind("::");
    return at == std::string::npos ? squeezed : squeezed.substr(at + 2);
}

// Every name `std::filesystem` goes by in the tree: every namespace alias of it, and of an alias of it, wherever it
// is declared -- to a fixed point, so the order the files are read in decides nothing.
[[nodiscard]] std::set<std::string> aliasesIn(std::vector<Source> const& sources) {
    static std::regex const alias{R"(\bnamespace\s+(\w+)\s*=\s*((?:::\s*)?\w+(?:\s*::\s*\w+)*)\s*;)"};
    std::vector<std::pair<std::string, std::string>> declared;   // (alias, last component of what it names)
    for (auto const& s : sources) {
        if (s.code.find("namespace") == std::string::npos) continue;
        for (std::sregex_iterator it{s.code.begin(), s.code.end(), alias}, end; it != end; ++it)
            declared.emplace_back((*it)[1].str(), lastComponent((*it)[2].str()));
    }
    std::set<std::string> aliases;
    for (bool grew = true; grew;) {
        grew = false;
        for (auto const& [name, names] : declared) {
            if ((names == "filesystem" || aliases.count(names) != 0) && aliases.insert(name).second) grew = true;
        }
    }
    return aliases;
}

// The sources whose include closure reaches `#include <filesystem>`: those that include it, and every source that
// includes one of them, by a walk over the reversed include graph (a work list, never recursion).
[[nodiscard]] std::set<std::string> exposedIn(std::vector<Source> const& sources) {
    std::set<std::string> known;
    for (auto const& s : sources) known.insert(s.relative);
    static std::regex const directive{R"(^\s*#\s*include\s*)"};
    static std::regex const header{R"re(^\s*#\s*include\s*([<"])([^>"]+)[>"])re"};
    std::map<std::string, std::vector<std::string>> includedBy;
    std::vector<std::string>                         work;
    for (auto const& s : sources) {
        auto const code = linesOf(s.code);
        auto const text = linesOf(s.text);
        for (std::size_t n = 0; n < code.size() && n < text.size(); ++n) {
            // The directive must be CODE (not in a comment or a raw string); its header name, a literal the code
            // view blanks, is read from the text at the same place.
            if (!std::regex_search(code[n], directive)) continue;
            std::smatch m;
            if (!std::regex_search(text[n], m, header)) continue;
            std::string const name = m[2].str();
            if (m[1].str() == "<") {
                if (name == "filesystem") work.push_back(s.relative);
                continue;
            }
            std::vector<std::string> bases{fs::path{s.relative}.parent_path().generic_string()};
            for (char const* root : kIncludeRoots) bases.emplace_back(root);
            for (auto const& base : bases) {
                std::string const candidate = (fs::path{base} / name).lexically_normal().generic_string();
                if (known.count(candidate) != 0) {
                    includedBy[candidate].push_back(s.relative);
                    break;
                }
            }
        }
    }
    std::set<std::string> exposed;
    while (!work.empty()) {
        std::string const file = work.back();
        work.pop_back();
        if (!exposed.insert(file).second) continue;
        for (auto const& includer : includedBy[file]) work.push_back(includer);
    }
    return exposed;
}

struct Finding {
    std::string where;   // `<file>:<line>`
    std::string call;    // what was written
    std::string ask;     // the substrate call to use
};

[[nodiscard]] std::string alternation(std::set<std::string> const& names) {
    std::string out;
    for (auto const& n : names) out += (out.empty() ? "" : "|") + n;
    return out;
}

// The scan's patterns, built ONCE for a set of sources: `aliases` are that set's names for `std::filesystem`.
struct Scanner {
    explicit Scanner(std::set<std::string> aliasNames) : aliases{std::move(aliasNames)} {
        for (auto const& entry : kAsk) names.insert(entry.first);
        std::set<std::string> qualifiers = aliases;
        qualifiers.insert("filesystem");
        qualified   = std::regex{R"re((?:^|[^\w])(?:)re" + alternation(qualifiers) + R"re()\s*::\s*()re"
                               + alternation(names) + R"re()\b)re"};
        member      = std::regex{R"re((?:\.|->)\s*()re" + alternation(kEntryMembers) + R"re()\s*\()re"};
        option      = std::regex{R"re(\bcopy_options\s*::\s*()re" + alternation(kLinkOptions) + R"re()\b)re"};
        unqualified = std::regex{R"re(\b()re" + alternation(kFunctions) + R"re()\s*\(\s*[^\s)])re"};
        directive   = std::regex{R"(\busing\s+namespace\s+((?:::\s*)?\w+(?:\s*::\s*\w+)*))"};
    }
    std::set<std::string> aliases;
    std::set<std::string> names;
    std::regex            qualified, member, option, unqualified, directive;
};

// Every refused form in one source's code; `exposed` says whether its include closure reaches `<filesystem>`.
[[nodiscard]] std::vector<Finding> refusedIn(Source const& source, Scanner const& scan, bool exposed) {
    auto const& aliases     = scan.aliases;
    auto const& names       = scan.names;
    auto const& qualified   = scan.qualified;
    auto const& member      = scan.member;
    auto const& option      = scan.option;
    auto const& unqualified = scan.unqualified;
    auto const& directive   = scan.directive;

    // A line where no refused name and no `using` appears holds no refused form: only the rest are matched, which
    // keeps a scan of the whole tree to the few lines that can hold one.
    auto const mayHold = [&](std::string const& line) {
        if (line.find("using") != std::string::npos) return true;
        for (auto const& n : names)
            if (line.find(n) != std::string::npos) return true;
        return false;
    };
    std::vector<Finding> out;
    auto const           lines = linesOf(source.code);
    for (std::size_t n = 0; n < lines.size(); ++n) {
        std::string const& line = lines[n];
        if (!mayHold(line)) continue;
        std::string const where = source.relative + ":" + std::to_string(n + 1);
        for (auto const* re : {&qualified, &member, &option}) {
            for (std::sregex_iterator it{line.begin(), line.end(), *re}, end; it != end; ++it) {
                std::string const name = (*it)[1].str();
                out.push_back({where, name, kAsk.at(name)});
            }
        }
        for (std::sregex_iterator it{line.begin(), line.end(), directive}, end; it != end; ++it) {
            std::string const last = lastComponent((*it)[1].str());
            if (last == "filesystem" || aliases.count(last) != 0)
                out.push_back({where, "using namespace " + last, "a qualified call, asked of the substrate"});
        }
        if (!exposed) continue;
        for (std::sregex_iterator it{line.begin(), line.end(), unqualified}, end; it != end; ++it) {
            // Qualified or a member: not argument-dependent lookup (a qualified mention is refused above).
            std::string before = line.substr(0, static_cast<std::size_t>(it->position(0)));
            while (!before.empty() && std::isspace(static_cast<unsigned char>(before.back())) != 0) before.pop_back();
            auto const endsWith = [&](char const* tail) {
                std::string const t{tail};
                return before.size() >= t.size() && before.compare(before.size() - t.size(), t.size(), t) == 0;
            };
            if (endsWith(".") || endsWith("->") || endsWith("::")) continue;
            std::string const name = (*it)[1].str();
            out.push_back({where, name + " (unqualified, reached by argument-dependent lookup)", kAsk.at(name)});
        }
    }
    return out;
}

// Whether `relative` (a generic path under the repository) is a source this guard judges.
[[nodiscard]] bool isGuarded(std::string const& relative) {
    std::string const ext = fs::path{relative}.extension().string();
    if (ext != ".cpp" && ext != ".hpp") return false;
    if (relative.rfind("integrated_tests/", 0) == 0) return true;
    return relative.rfind("src/", 0) == 0 && relative.rfind("src/core/substrate/", 0) != 0;
}

// Every finding over a set of sources: the aliases and the closure are the whole set's, the findings the guarded
// sources' alone.
[[nodiscard]] std::vector<Finding> refusedAcross(std::vector<Source> const& sources) {
    Scanner const        scan{aliasesIn(sources)};
    auto const           exposed = exposedIn(sources);
    std::vector<Finding> found;
    for (auto const& s : sources) {
        if (!isGuarded(s.relative)) continue;
        auto more = refusedIn(s, scan, exposed.count(s.relative) != 0);
        found.insert(found.end(), more.begin(), more.end());
    }
    return found;
}

[[nodiscard]] std::string readAll(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// `found` as `<line>=<call>` words, or `<file> line <line>=<call>` when `withFile` -- the file and its line
// spelled apart, as words, so the expectations below cite no position in any file.
[[nodiscard]] std::string spelled(std::vector<Finding> const& found, bool withFile) {
    std::string got;
    for (auto const& f : found) {
        std::size_t const colon = f.where.find(':');
        std::string const line  = f.where.substr(colon + 1);
        got += " " + (withFile ? f.where.substr(0, colon) + " line " + line : line) + "=" + f.call;
    }
    return got;
}

} // namespace

// THE TREE: no source in scope asks the library a question its answer to depends on how it reads a link.
TEST(FilesystemQueriesGuard, TheSourceTreeAsksTheSubstrateForEveryFilesystemQuery) {
    fs::path const      root = dss::test::repoRoot();
    std::vector<Source> sources;
    for (char const* top : {"src", "integrated_tests", "tests/test_support"}) {
        auto const walk = dss::core::walkTree(root / top, [&](dss::core::TreeEntry const& entry) {
            if (entry.kind != dss::core::EntryKind::File) return dss::core::WalkStep::Continue;
            std::string const relative = entry.path.lexically_relative(root).generic_string();
            std::string const ext      = entry.path.extension().string();
            if (ext == ".cpp" || ext == ".hpp") sources.push_back(sourceOf(relative, readAll(entry.path)));
            return dss::core::WalkStep::Continue;
        });
        ASSERT_FALSE(walk.error) << "the walk of " << top << "/ stopped at " << walk.where.generic_string() << ": "
                                 << walk.error.message();
    }
    std::size_t const judged =
        static_cast<std::size_t>(std::count_if(sources.begin(), sources.end(), [](Source const& s) {
            return isGuarded(s.relative);
        }));
    // Not a vacuous pass: the tree holds hundreds of sources in scope (535 under src/ at the guard's birth,
    // 2026-10-20), and integrated_tests/ is read too.
    EXPECT_GE(judged, 400u) << "the guard judged only " << judged << " sources";
    EXPECT_TRUE(std::any_of(sources.begin(), sources.end(), [](Source const& s) {
        return s.relative == "integrated_tests/runner.cpp";
    })) << "integrated_tests/runner.cpp was not read: the scope is short";
    auto const  found = refusedAcross(sources);
    std::string listed;
    for (auto const& f : found) listed += "\n  " + f.where + ": `" + f.call + "` -- ask the substrate: " + f.ask;
    EXPECT_TRUE(found.empty()) << found.size() << " filesystem question(s) asked of the library outside "
                               << "src/core/substrate/:" << listed;
}

// THE SCAN ITSELF, on synthetic sources holding every refused form once, each in a context it must and must not
// read: every qualifier spelling (std::filesystem, ::std::filesystem, an alias, an alias of an alias declared in a
// header), a mention that is no call, an entry's member, a link option, a using-declaration and the
// using-directives -- and the same words inside a comment, a string and a raw string, plus calls the guard does not
// refuse.
TEST(FilesystemQueriesGuard, TheScanRefusesEveryFormItNamesAndNothingElse) {
    std::vector<Source> const sources = {
        sourceOf("src/x/aliases.hpp",
                 "#include <filesystem>\n"
                 "namespace stdfs = std::filesystem;\n"
                 "namespace fsx = stdfs;\n"),
        sourceOf("src/x/user.cpp",
                 "#include \"aliases.hpp\"\n"
                 "bool a(std::filesystem::path const& p) { return std::filesystem::exists(p); }\n"   // 2
                 "auto b(stdfs::path const& p) { return ::std::filesystem::file_size(p); }\n"         // 3
                 "bool c(fsx::path const& p) { return fsx::is_regular_file(p); }\n"                   // 4
                 "auto d = &stdfs::is_symlink;\n"                                                      // 5
                 "auto e(stdfs::path const& p) { return stdfs::last_write_time(p); }\n"               // 6
                 "bool f(stdfs::path const& a, stdfs::path const& b) { return fsx::equivalent(a, b); }\n"   // 7
                 "auto g(stdfs::path const& p) { return stdfs::relative(p, p); }\n"                   // 8
                 "void h(stdfs::path const& p) { stdfs::permissions(p, stdfs::perms::all); }\n"       // 9
                 "void i(stdfs::path const& p) { stdfs::remove_all(p); }\n"                           // 10
                 "using It = stdfs::recursive_directory_iterator;\n"                                  // 11
                 "bool j(stdfs::directory_entry const& e) { return e.is_regular_file(); }\n"         // 12
                 "auto k(stdfs::directory_iterator const& it) { return it->last_write_time(); }\n"   // 13
                 "auto l = stdfs::copy_options::copy_symlinks;\n"                                      // 14
                 "using std::filesystem::exists;\n"                                                    // 15
                 "using namespace std::filesystem;\n"                                                  // 16
                 "using namespace fsx;\n"                                                              // 17
                 "bool m(stdfs::path const& p) { return exists(p); }\n"                               // 18
                 "// stdfs::exists(p) in a comment, /* fs::status(p) */ too\n"
                 "char const* n = \"std::filesystem::exists(p) in a string\";\n"
                 "char const* o = R\"x(stdfs::file_size(p) in a raw string)x\";\n"
                 "void q(stdfs::path const& p) { stdfs::create_directories(p); stdfs::absolute(p); stdfs::remove(p); }\n"
                 "bool r(Cache& c) { return c.contains(1) && myexists(2) && ns::exists_like(3) && x.exists_in(4); }\n"
                 "auto s = stdfs::copy_options::recursive; auto t = stdfs::file_status{};\n"),
    };
    auto const        found = refusedAcross(sources);
    std::string const got   = spelled(found, /*withFile=*/false);
    std::string const want =
        " 2=exists 3=file_size 4=is_regular_file 5=is_symlink 6=last_write_time 7=equivalent 8=relative "
        "9=permissions 10=remove_all 11=recursive_directory_iterator 12=is_regular_file 13=last_write_time "
        "14=copy_symlinks 15=exists 16=using namespace filesystem 17=using namespace fsx "
        "18=exists (unqualified, reached by argument-dependent lookup)";
    EXPECT_EQ(got, want) << "the scan refused " << found.size() << " form(s):" << got;
}

// ARGUMENT-DEPENDENT LOOKUP reaches `std::filesystem` only where it is declared: an unqualified call is refused in
// a file whose include closure reaches `<filesystem>` -- including it itself, through a header that includes it,
// and through a header that includes that one -- and nowhere else, while a qualified mention is refused everywhere.
TEST(FilesystemQueriesGuard, AnUnqualifiedCallIsRefusedWhereTheIncludeClosureReachesFilesystem) {
    std::vector<Source> const sources = {
        sourceOf("src/a/paths.hpp", "#pragma once\n#include <filesystem>\n"),
        sourceOf("src/b/more.hpp", "#pragma once\n#include \"a/paths.hpp\"\n"),
        sourceOf("src/c/near.cpp", "#include \"../b/more.hpp\"\nbool f(P const& p) { return exists(p); }\n"),
        sourceOf("integrated_tests/far.cpp", "#include \"b/more.hpp\"\nauto g(P const& p) { return status(p); }\n"),
        sourceOf("src/d/apart.cpp",
                 "#include <string>\n"
                 "bool h(P const& p) { return exists(p) && status(p); }\n"
                 "auto i(P const& p) { return std::filesystem::status(p); }\n"),
        // In a comment the directive includes nothing.
        sourceOf("src/e/commented.cpp", "// #include <filesystem>\nbool j(P const& p) { return exists(p); }\n"),
        // No argument, or a qualifier of its own: not argument-dependent lookup of a filesystem function.
        sourceOf("src/f/nullary.cpp", "#include <filesystem>\nint k() { return status() + n::exists(2); }\n"),
        // The closure's first step: a file that includes `<filesystem>` itself.
        sourceOf("src/g/direct.cpp", "#include <filesystem>\nbool m(P const& p) { return is_empty(p); }\n"),
    };
    std::string const got = spelled(refusedAcross(sources), /*withFile=*/true);
    std::string const want =
        " src/c/near.cpp line 2=exists (unqualified, reached by argument-dependent lookup)"
        " integrated_tests/far.cpp line 2=status (unqualified, reached by argument-dependent lookup)"
        " src/d/apart.cpp line 3=status"
        " src/g/direct.cpp line 2=is_empty (unqualified, reached by argument-dependent lookup)";
    EXPECT_EQ(got, want);
}

// THE ONE DIRECTORY LEFT OUT is the substrate; C++ sources under src/ and integrated_tests/ are judged.
TEST(FilesystemQueriesGuard, TheSubstrateIsTheOneDirectoryLeftOut) {
    EXPECT_TRUE(isGuarded("src/core/types/glob_match.cpp"));
    EXPECT_TRUE(isGuarded("src/program/program.hpp"));
    EXPECT_TRUE(isGuarded("integrated_tests/runner.cpp")) << "the CLI runner is judged too";
    EXPECT_FALSE(isGuarded("src/core/substrate/path_identity.cpp")) << "the substrate is where the OS is asked";
    EXPECT_FALSE(isGuarded("src/dss-config/runtime/platform/src/dirent.c"));
    EXPECT_FALSE(isGuarded("tests/core/test_header_name_matching.cpp"));
    EXPECT_FALSE(isGuarded("tests/test_support/stage_tree.hpp")) << "read for the closure, not judged";
    EXPECT_TRUE(isGuarded("src/core/substrate_like/x.cpp")) << "only the substrate directory itself is left out";
}
