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
// ★ OUTSIDE THE CLASS, MEASURED THE SAME on both Windows libraries (and not refused by the forms below): `remove`
// and `rename` (they act on the entry itself), `directory_iterator` (its listing; the questions its entries answer
// ARE refused, as members), `create_directory` / `create_directories`, `copy_file`, `copy` with no link option, and
// the questions that read no entry at all (`absolute`, `current_path`, `temp_directory_path`, `space`). Under src/
// they are refused all the same, by H6c's family (d) (the last paragraph of this note): each receives TEXT and
// resolves it by the library's own use, which no value can reach.
//
// THE FORMS REFUSED, each naming the substrate call to ask instead:
//   * a refused name MENTIONED through a qualifier that is `std::filesystem` -- in any spelling: `::std::filesystem`,
//     any qualifier ending in `filesystem` -- or any ALIAS of it declared anywhere in the scanned tree, an alias of
//     an alias included (a header's alias reaches every file that includes it). A mention, not only a call: taking
//     its address or a using-declaration of it is the same question waiting to be asked;
//   * the same question asked of a `directory_entry` (`.is_directory(`, `->last_write_time(`, ...), or named
//     through it -- a pointer to the member, a ranges projection (`&fs::directory_entry::is_regular_file`);
//   * a MACRO naming `std::filesystem` (`#define FS std::filesystem`): an alias the scan cannot follow;
//   * a `copy_options` that reads a link (`copy_symlinks`, `skip_symlinks`, `create_symlinks`);
//   * a using-directive of `std::filesystem` or of an alias of it;
//   * an UNQUALIFIED call of a refused function -- one argument at least, so argument-dependent lookup can reach
//     `std::filesystem` -- WHEREVER it stands. Whether it does depends on the argument's type, which a scan of
//     text cannot read, and on what every header the file includes declares, which the scan does not read either
//     (a C++ library spelled `.h`, `<gtest/gtest.h>`, declares namespaces as `<nlohmann/json.hpp>` does). So the
//     form is keyed on the CALL, never on the include (P69 fold H6, the review of H5's N9 and the ruling on H4b's
//     N1): a function of the tree's own that shares a refused name is called qualified, or named otherwise.
// It has no exemption list: the substrate is the one directory left out.
//
// SCOPE: every *.cpp and *.hpp under src/ (src/dss-config's C runtime holds no std::filesystem) outside
// src/core/substrate/, under integrated_tests/ (whose runner compiles the substrate's unit beside it), and under
// tests/examples/ (the in-process examples runner, the CLI runner's twin: the scope is the RUNNERS, not one of
// them -- P69 fold H5, the review of H4b's F4). The scan also reads tests/test_support/, which both runners
// include, for the aliases its headers declare. Comments and string literals are not code, and are not read (tests/test_support/source_code_only.hpp).
//
// ★ WHICH CODE TOUCHES A HOST PATH (P69 H6c): every operation on a host path takes its reading from one home, the
// substrate (src/core/substrate/host_path.hpp's value and path_identity.hpp's operations). Over src/ outside the
// substrate -- the runners keep their own file operations, on names DSS chooses -- five more families are refused,
// each keyed on the CALL and read from the code's identifiers: (a) a Win32 path call, (b) a C-runtime or POSIX path
// call, (c) a stream constructed or opened on a path, (d) a std::filesystem HOST operation the forms above let
// through, (f) the substrate's access header or its test seam. And THE FENCE (ledger 580): the store route's text
// forms, which H6d deletes, are taken at the pinned sites alone.

#include "core/substrate/path_identity.hpp"   // walkTree: the substrate's own walk, never through a link
#include "repo_root.hpp"
#include "source_code_only.hpp"   // codeOnly: the code of a source, its comments and literals blanked
#include "text_scan.hpp"          // the linear scanners

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Each refused name and the substrate's answer to it -- `use` the answer's use (H6c): core::Use::Leaf for the object
// the whole spelling names, core::Use::Container for the directory its names lie in.
std::map<std::string, std::string> const kAsk = {
    {"file_size", "core::reachedFileSize(p, ec)"},
    {"is_regular_file", "core::reachedKind(p, use, ec) == core::EntryKind::File"},
    {"is_directory", "core::reachedKind(p, use, ec) == core::EntryKind::Directory"},
    {"exists", "core::reachedKind(p, use, ec) != core::EntryKind::Absent (the entry itself: core::entryKind)"},
    {"status", "core::reachedKind(p, use, ec)"},
    {"symlink_status", "core::entryKind(p, use, ec)"},
    {"canonical", "core::reachedPath(p, use, ec), or core::PathIdentity::of(p, use) for an identity"},
    {"weakly_canonical", "core::reachedPath(p, use, ec), or core::PathIdentity::of(p, use) for an identity"},
    {"is_symlink", "core::entryKind(p, use, ec) == core::EntryKind::Link"},
    {"is_other", "core::reachedKind(p, use, ec) == core::EntryKind::Other (the entry itself: core::entryKind)"},
    {"is_fifo", "core::reachedKind(p, use, ec) == core::EntryKind::Other (the entry itself: core::entryKind)"},
    {"is_socket", "core::reachedKind(p, use, ec) == core::EntryKind::Other (the entry itself: core::entryKind)"},
    {"is_block_file", "core::reachedKind(p, use, ec) == core::EntryKind::Other (the entry itself: core::entryKind)"},
    {"is_character_file", "core::reachedKind(p, use, ec) == core::EntryKind::Other (the entry itself: core::entryKind)"},
    {"is_empty", "core::reachedFileSize(p, ec) for a file, core::walkTree for a directory"},
    {"equivalent", "core::reachedSameFile(a, b, use, ec)"},
    {"last_write_time", "core::reachedWriteTime(p, use, ec)"},
    {"hard_link_count", "a substrate question (core:: asks none yet): add it to path_identity.hpp"},
    {"read_symlink", "core::reachedPath(p, use, ec)"},
    {"remove_all", "core::removeTree(p)"},
    {"permissions", "core::addReachedPermissions(p, perms, ec)"},
    {"relative", "core::reachedRelative(p, use, base, ec)"},
    {"proximate", "core::reachedRelative(p, use, base, ec), or p where that is empty"},
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
    std::string code = dss::test::codeOnly(text);
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

// ── THE SCAN'S FORMS: each a loop over the text it reads, its work proportional to that text and its stack depth
// constant (tests/test_support/text_scan.hpp says why no test matches text with `std::regex`) ──
using dss::test::isWordChar;
using dss::test::skipSpace;

// One past the run of word characters at `i` (`\w*`): `i` itself where none begins there.
[[nodiscard]] std::size_t wordRunEnd(std::string_view s, std::size_t i) {
    while (i < s.size() && isWordChar(s[i])) ++i;
    return i;
}

// Whether `at` begins a word: no word character stands before it.
[[nodiscard]] bool beginsWord(std::string_view s, std::size_t at) { return at == 0 || !isWordChar(s[at - 1]); }

// One past a qualifier as written at `i` -- an optional leading `::`, a word, then `::` and a word as often as they
// follow, white space about each `::` -- or `i` itself where no word begins one. A `::` no word follows ends it.
[[nodiscard]] std::size_t qualifierEnd(std::string_view s, std::size_t i) {
    std::size_t const first = s.compare(i, 2, "::") == 0 ? skipSpace(s, i + 2) : i;
    std::size_t       end   = wordRunEnd(s, first);
    if (end == first) return i;
    for (;;) {
        std::size_t const colons = skipSpace(s, end);
        if (s.compare(colons, 2, "::") != 0) return end;
        std::size_t const word = skipSpace(s, colons + 2);
        std::size_t const next = wordRunEnd(s, word);
        if (next == word) return end;
        end = next;
    }
}

// The NAME of `:: NAME` after a scope that ends at `i` -- white space about the `::`, NAME a whole word that is one of
// `names` -- as its bounds, or nothing.
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> scopedName(std::string_view s, std::size_t i,
                                                                            std::set<std::string> const& names) {
    std::size_t const colons = skipSpace(s, i);
    if (s.compare(colons, 2, "::") != 0) return std::nullopt;
    std::size_t const name = skipSpace(s, colons + 2);
    std::size_t const end  = wordRunEnd(s, name);
    if (names.count(std::string{s.substr(name, end - name)}) == 0) return std::nullopt;
    return std::pair{name, end};
}

// Every place `word` begins a word in `s`: `visit(at)` reads what follows it and answers where the scan resumes.
template <typename Visit>
void forEachStart(std::string_view s, std::string_view word, Visit&& visit) {
    for (std::size_t at = s.find(word); at != std::string_view::npos;) {
        std::size_t const resume = beginsWord(s, at) ? visit(at) : at + 1;
        at = s.find(word, std::max(resume, at + 1));
    }
}

// Every `SCOPE :: NAME` of a line: SCOPE the word `scope` beginning a word, NAME a whole word of `names`.
[[nodiscard]] std::vector<std::string> scopedBy(std::string_view line, std::string_view scope,
                                                std::set<std::string> const& names) {
    std::vector<std::string> out;
    forEachStart(line, scope, [&](std::size_t at) {
        auto const name = scopedName(line, at + scope.size(), names);
        if (!name) return at + 1;
        out.emplace_back(line.substr(name->first, name->second - name->first));
        return name->second;
    });
    return out;
}

// Every namespace alias a code text declares -- `namespace NAME = QUALIFIER ;` -- as NAME and the last component of
// what it names.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> aliasDeclarations(std::string_view code) {
    std::vector<std::pair<std::string, std::string>> out;
    forEachStart(code, "namespace", [&](std::size_t at) {
        std::size_t const after   = at + std::string_view{"namespace"}.size();
        std::size_t const name    = skipSpace(code, after);
        std::size_t const nameEnd = wordRunEnd(code, name);
        if (name == after || nameEnd == name) return at + 1;
        std::size_t const equals = skipSpace(code, nameEnd);
        if (equals >= code.size() || code[equals] != '=') return at + 1;
        std::size_t const qualifier = skipSpace(code, equals + 1);
        std::size_t const stop      = qualifierEnd(code, qualifier);
        std::size_t const semicolon = skipSpace(code, stop);
        if (stop == qualifier || semicolon >= code.size() || code[semicolon] != ';') return at + 1;
        std::string const target{code.substr(qualifier, stop - qualifier)};
        out.emplace_back(std::string{code.substr(name, nameEnd - name)}, lastComponent(target));
        return semicolon + 1;
    });
    return out;
}

// Every name `std::filesystem` goes by in the tree: every namespace alias of it, and of an alias of it, wherever it
// is declared -- to a fixed point, so the order the files are read in decides nothing.
[[nodiscard]] std::set<std::string> aliasesIn(std::vector<Source> const& sources) {
    std::vector<std::pair<std::string, std::string>> declared;   // (alias, last component of what it names)
    for (auto const& s : sources)
        for (auto& declaration : aliasDeclarations(s.code)) declared.push_back(std::move(declaration));
    std::set<std::string> aliases;
    for (bool grew = true; grew;) {
        grew = false;
        for (auto const& [name, names] : declared) {
            if ((names == "filesystem" || aliases.count(names) != 0) && aliases.insert(name).second) grew = true;
        }
    }
    return aliases;
}

struct Finding {
    std::string where;   // `<file>:<line>`
    std::string call;    // what was written
    std::string ask;     // the substrate call to use
};

// Every using-directive of a line -- `using namespace QUALIFIER` -- as the last component of what it names.
[[nodiscard]] std::vector<std::string> directiveNames(std::string_view line) {
    std::vector<std::string> out;
    forEachStart(line, "using", [&](std::size_t at) {
        std::size_t const after   = at + std::string_view{"using"}.size();
        std::size_t const keyword = skipSpace(line, after);
        if (keyword == after || line.compare(keyword, 9, "namespace") != 0) return at + 1;
        std::size_t const qualifier = skipSpace(line, keyword + 9);
        std::size_t const stop      = qualifierEnd(line, qualifier);
        if (qualifier == keyword + 9 || stop == qualifier) return at + 1;
        out.push_back(lastComponent(std::string{line.substr(qualifier, stop - qualifier)}));
        return stop;
    });
    return out;
}

// The macro a line DEFINES when its definition names `filesystem` (`#define FS std::filesystem`), or nothing.
[[nodiscard]] std::optional<std::string> macroNamingFilesystem(std::string_view line) {
    std::size_t const hash = skipSpace(line, 0);
    if (hash >= line.size() || line[hash] != '#') return std::nullopt;
    std::size_t const keyword = skipSpace(line, hash + 1);
    if (line.compare(keyword, 6, "define") != 0) return std::nullopt;
    std::size_t const name = skipSpace(line, keyword + 6);
    std::size_t const end  = wordRunEnd(line, name);
    if (name == keyword + 6 || end == name || !dss::test::holdsWord(line.substr(end), "filesystem")) return std::nullopt;
    return std::string{line.substr(name, end - name)};
}

// Every call of a refused FUNCTION in a line that passes an argument -- `NAME ( ARG`: NAME a whole word, ARG's first
// character neither white space nor `)` -- as where NAME begins and NAME. What stands before it is the caller's to read.
[[nodiscard]] std::vector<std::pair<std::size_t, std::string>> callsIn(std::string_view line) {
    std::vector<std::pair<std::size_t, std::string>> out;
    dss::test::forEachIdentifier(line, [&](std::size_t begin, std::size_t end) {
        std::string       name{line.substr(begin, end - begin)};
        std::size_t const open = skipSpace(line, end);
        if (!beginsWord(line, begin) || kFunctions.count(name) == 0 || open >= line.size() || line[open] != '(')
            return end;
        std::size_t const argument = skipSpace(line, open + 1);
        if (argument < line.size() && line[argument] != ')') out.emplace_back(begin, std::move(name));
        return end;
    });
    return out;
}

// The scan's forms for a set of sources: `aliases` are that set's names for `std::filesystem`. Each form refused by
// the NAME its match ends in answers every such name of a line, in order.
struct Scanner {
    explicit Scanner(std::set<std::string> aliasNames) : aliases{std::move(aliasNames)} {
        for (auto const& entry : kAsk) names.insert(entry.first);
        qualifiers = aliases;
        qualifiers.insert("filesystem");
    }
    std::set<std::string> aliases;
    std::set<std::string> names;
    std::set<std::string> qualifiers;   // `filesystem` (so any qualifier that ends in it) and every alias of it

    // `Q :: NAME`: Q a name of std::filesystem beginning a word, NAME a refused name.
    [[nodiscard]] std::vector<std::string> qualified(std::string_view line) const {
        std::vector<std::string> out;
        dss::test::forEachIdentifier(line, [&](std::size_t begin, std::size_t end) {
            if (qualifiers.count(std::string{line.substr(begin, end - begin)}) == 0) return end;
            auto const name = scopedName(line, end, names);
            if (!name) return end;
            out.emplace_back(line.substr(name->first, name->second - name->first));
            return name->second;
        });
        return out;
    }
    // `.NAME (` or `->NAME (`: a question a `directory_entry` answers itself.
    [[nodiscard]] std::vector<std::string> member(std::string_view line) const {
        std::vector<std::string> out;
        for (std::size_t i = 0; i < line.size(); ++i) {
            std::size_t const after = line[i] == '.' ? i + 1 : line.compare(i, 2, "->") == 0 ? i + 2 : 0;
            if (after == 0) continue;
            std::size_t const name = skipSpace(line, after);
            std::size_t const end  = wordRunEnd(line, name);
            std::size_t const open = skipSpace(line, end);
            if (kEntryMembers.count(std::string{line.substr(name, end - name)}) == 0 || open >= line.size()
                || line[open] != '(')
                continue;
            out.emplace_back(line.substr(name, end - name));
            i = open;   // the scan resumes past the `(`
        }
        return out;
    }
    // `copy_options :: OPTION`: a copy option that reads a link.
    [[nodiscard]] std::vector<std::string> option(std::string_view line) const {
        return scopedBy(line, "copy_options", kLinkOptions);
    }
    // `directory_entry :: NAME`: a question named through the entry (a pointer to the member, a projection).
    [[nodiscard]] std::vector<std::string> projection(std::string_view line) const {
        return scopedBy(line, "directory_entry", kEntryMembers);
    }
};

// Every refused form in one source's code.
[[nodiscard]] std::vector<Finding> refusedIn(Source const& source, Scanner const& scan) {
    auto const& aliases = scan.aliases;
    auto const& names   = scan.names;

    // A line where no refused name and no `using` appears holds no refused form: only the rest are scanned, which
    // keeps a scan of the whole tree to the few lines that can hold one.
    auto const mayHold = [&](std::string const& line) {
        if (line.find("using") != std::string::npos || line.find("filesystem") != std::string::npos) return true;
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
        for (auto const form : {&Scanner::qualified, &Scanner::member, &Scanner::option, &Scanner::projection}) {
            for (std::string const& name : (scan.*form)(line)) out.push_back({where, name, kAsk.at(name)});
        }
        for (std::string const& last : directiveNames(line)) {
            if (last == "filesystem" || aliases.count(last) != 0)
                out.push_back({where, "using namespace " + last, "a qualified call, asked of the substrate"});
        }
        if (std::optional<std::string> const name = macroNamingFilesystem(line))
            out.push_back({where, "#define " + *name + " naming filesystem",
                           "a namespace alias (`namespace x = std::filesystem;`), which the scan follows"});
        // Keyed on the CALL, wherever it stands (the scope note): whether argument-dependent lookup reaches
        // std::filesystem from here is a question of types and of unread headers, which this scan cannot answer.
        for (auto const& [at, name] : callsIn(line)) {
            // Qualified or a member: not argument-dependent lookup (a qualified mention is refused above).
            std::string before = line.substr(0, at);
            while (!before.empty() && std::isspace(static_cast<unsigned char>(before.back())) != 0) before.pop_back();
            auto const endsWith = [&](char const* tail) {
                std::string const t{tail};
                return before.size() >= t.size() && before.compare(before.size() - t.size(), t.size(), t) == 0;
            };
            if (endsWith(".") || endsWith("->") || endsWith("::")) continue;
            out.push_back(
                {where, name + " (unqualified: argument-dependent lookup may reach std::filesystem)", kAsk.at(name)});
        }
    }
    return out;
}

// Whether `relative` (a generic path under the repository) is a source this guard judges.
[[nodiscard]] bool isGuarded(std::string const& relative) {
    std::string const ext = fs::path{relative}.extension().string();
    if (ext != ".cpp" && ext != ".hpp") return false;
    if (relative.rfind("integrated_tests/", 0) == 0) return true;
    if (relative.rfind("tests/examples/", 0) == 0) return true;
    return relative.rfind("src/", 0) == 0 && relative.rfind("src/core/substrate/", 0) != 0;
}

// Every finding over a set of sources: the aliases are the whole set's, the findings the guarded sources' alone.
[[nodiscard]] std::vector<Finding> refusedAcross(std::vector<Source> const& sources) {
    Scanner const        scan{aliasesIn(sources)};
    std::vector<Finding> found;
    for (auto const& s : sources) {
        if (!isGuarded(s.relative)) continue;
        auto more = refusedIn(s, scan);
        found.insert(found.end(), more.begin(), more.end());
    }
    return found;
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


// =====================================================================================================================
// WHICH CODE TOUCHES A HOST PATH (P69 H6c). Every family below is keyed on the CALL and read from the code's
// identifiers -- a name, the qualifier it is written with, and what stands before it -- never from a line's shape.
// =====================================================================================================================

// (a) The Win32 calls that take a path or hand one out (A4's vocabulary, the plan's W1). A name of kWin32Stems is
// refused bare and with its `A` or `W` suffix, a name of kWin32Names as it is, and a name that begins with one of
// kWin32Prefixes whatever follows it. SetFileInformationByHandle is refused at every call: whether a path rides it
// is its information class's (`FileRenameInfo`, `FileLinkInfo`), which the line that calls it need not spell.
std::set<std::string, std::less<>> const kWin32Stems = {
    "CopyFile",           "CopyFileEx",          "CreateDirectory",       "CreateDirectoryEx",
    "CreateFile",         "CreateFileMapping",   "CreateHardLink",        "CreateNamedPipe",
    "CreateProcess",      "CreateProcessAsUser", "CreateSymbolicLink",    "DeleteFile",
    "FindFirstChangeNotification", "FindFirstFile", "FindFirstFileEx",    "GetBinaryType",
    "GetCompressedFileSize", "GetCurrentDirectory", "GetDiskFreeSpace",   "GetDiskFreeSpaceEx",
    "GetFileAttributes",  "GetFileAttributesEx", "GetFileVersionInfo",    "GetFileVersionInfoEx",
    "GetFileVersionInfoSize", "GetFileVersionInfoSizeEx", "GetFinalPathNameByHandle", "GetFullPathName",
    "GetLongPathName",    "GetModuleFileName",   "GetModuleFileNameEx",   "GetShortPathName",
    "GetTempFileName",    "GetTempPath",         "GetTempPath2",          "GetVolumeInformation",
    "GetVolumePathName",  "LoadImage",           "LoadLibrary",           "LoadLibraryEx",
    "MoveFile",           "MoveFileEx",          "MoveFileWithProgress",  "PathFileExists",
    "RemoveDirectory",    "ReplaceFile",         "SHCreateDirectory",     "SHCreateDirectoryEx",
    "SHFileOperation",    "SHGetFolderPath",     "SHGetFolderPathAndAppend", "SHGetSpecialFolderPath",
    "SearchPath",         "SetCurrentDirectory", "SetFileAttributes",     "ShellExecute",
    "ShellExecuteEx"};
std::set<std::string, std::less<>> const kWin32Names = {
    "CopyFile2",    "CreateFile2",  "FindFirstFileNameTransactedW", "FindFirstFileNameW", "NtCreateFile",
    "NtOpenFile",   "OpenFile",     "OpenFileById", "SHGetKnownFolderPath", "SetFileInformationByHandle",
    "ZwCreateFile", "ZwOpenFile",   "_lcreat",      "_lopen"};
std::vector<std::string_view> const kWin32Prefixes = {"GetPrivateProfile", "RtlDosPathNameToNtPathName",
                                                      "WritePrivateProfile"};

// (b) The C-runtime and POSIX calls that take a path (A4's vocabulary, the plan's W2), wide and narrow: a name of
// kCrtNames as it is, and a name that begins with one of kCrtPrefixes. `open` and `creat` share their names with
// the tree's own functions, so they are keyed apart (the plan's B3): refused when the global scope qualifies them
// (`::open(`), or when their argument list names an `O_*` flag. Every other name is refused as W2 keys it -- `std::
// remove` among them, which is also the algorithm's name: the tree erases with `std::erase` / `std::ranges::remove`.
std::set<std::string, std::less<>> const kCrtNames = {
    "_access",  "_chdir",   "_chmod",    "_fsopen",    "_fullpath", "_getcwd",   "_mkdir",     "_open",
    "_popen",   "_rmdir",   "_sopen",    "_sopen_s",   "_unlink",   "_waccess",  "_waccess_s", "_wchdir",
    "_wchmod",  "_wfopen",  "_wfopen_s", "_wfreopen",  "_wfreopen_s", "_wfsopen", "_wfullpath", "_wgetcwd",
    "_wmkdir",  "_wopen",   "_wopendir", "_wpopen",    "_wremove",  "_wrename",  "_wrmdir",    "_wsopen",
    "_wsopen_s", "_wsystem", "_wtmpnam", "_wunlink",   "access",    "chdir",     "chmod",      "creat",
    "dlopen",   "fopen",    "freopen",   "fstatat",    "getcwd",    "link",      "lstat",      "mkdir",
    "mkdirat",  "mkdtemp",  "open",      "openat",     "opendir",   "popen",     "posix_spawn", "posix_spawnp",
    "readlink", "realpath", "remove",    "rename",     "rmdir",     "stat",      "stat64",     "symlink",
    "system",   "tmpfile",  "tmpnam",    "truncate",   "unlink",    "unlinkat"};
std::vector<std::string_view> const kCrtPrefixes = {"_findfirst", "_stat",  "_utime", "_wexec",   "_wfindfirst",
                                                    "_wspawn",    "_wstat", "_wutime", "execl",   "execv",
                                                    "mkostemp",   "mkstemp", "utime"};

// (c) The library's file streams: constructed on a path, or opened on one, they read the path's text by the
// library's own rule, which no value can reach.
std::set<std::string, std::less<>> const kStreamTypes = {
    "basic_filebuf", "basic_fstream", "basic_ifstream", "basic_ofstream", "filebuf",  "fstream",
    "ifstream",      "ofstream",      "wfilebuf",       "wfstream",       "wifstream", "wofstream"};

// (d) The std::filesystem HOST operations the link questions (kAsk) let through, each with the home's answer: every
// library call receives TEXT and resolves it by the library's own use, which no value can reach (the plan's 5.4).
std::map<std::string, std::string, std::less<>> const kHostOperations = {
    {"absolute", "core::absoluteKeepingRoot(p, use, ec)"},
    {"copy", "core::copyFile(from, to, ec), a tree's files through core::walkTree"},
    {"copy_file", "core::copyFile(from, to, ec)"},
    {"copy_symlink", "a substrate operation (core:: has none yet): add it to path_identity.hpp"},
    {"create_directories", "core::createDirectories(p, ec)"},
    {"create_directory", "core::createDirectories(p, ec)"},
    {"create_directory_symlink", "a substrate operation (core:: has none yet): add it to path_identity.hpp"},
    {"create_hard_link", "a substrate operation (core:: has none yet): add it to path_identity.hpp"},
    {"create_symlink", "a substrate operation (core:: has none yet): add it to path_identity.hpp"},
    {"current_path", "core::currentDirectory(ec)"},
    {"directory_entry", "core::walkTree(root, visit): the entries it hands out are the substrate's"},
    {"directory_iterator", "core::walkTree(root, visit)"},
    {"remove", "core::removeEntry(p, ec)"},
    {"rename", "core::renameEntry(from, to, ec)"},
    {"resize_file", "a substrate operation (core:: has none yet): add it to path_identity.hpp"},
    {"space", "a substrate question (core:: asks none yet): add it to path_identity.hpp"},
    {"temp_directory_path", "core::temporaryDirectory(ec)"},
};
// The host operations an UNQUALIFIED call can reach by argument-dependent lookup through a path argument. `copy`,
// `remove`, `rename` and `space` are left out of this form: they are also the algorithms' and the C library's names
// (family (b) reads the last two) and the tree's own local functions' (a `space(c)` lambda), so only their qualified
// mention is refused; `temp_directory_path` takes no path.
std::set<std::string, std::less<>> const kHostOperationsByLookup = {
    "absolute",         "copy_file",        "copy_symlink",   "create_directories", "create_directory",
    "create_directory_symlink", "create_hard_link", "create_symlink", "current_path",   "resize_file"};

// The keywords after which a name begins an expression: before any other identifier it is a declaration's name.
std::set<std::string, std::less<>> const kExpressionKeywords = {
    "alignof", "and",    "and_eq", "bitand", "bitor", "case", "co_await", "co_return", "co_yield", "compl", "delete",
    "do",      "else",   "new",    "not",    "not_eq", "or",  "or_eq",    "return",    "sizeof",   "throw", "typeid",
    "xor",     "xor_eq"};

std::string const kAskHostCall =
    "the substrate's operation on a HostPath (core::openHandle, core::openDescriptor, core::openFileStream, "
    "core::removeEntry, core::renameEntry, core::spawnAndWait*), or a new one in src/core/substrate/";
std::string const kAskStream = "core::openInputStream(p, mode) or core::openOutputStream(p, mode)";

[[nodiscard]] bool isIdentifierChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The last character before `k` that is no space, or npos.
[[nodiscard]] std::size_t lastBefore(std::string const& code, std::size_t k) {
    while (k > 0) {
        --k;
        if (std::isspace(static_cast<unsigned char>(code[k])) == 0) return k;
    }
    return std::string::npos;
}

// The first character from `k` on that is no space (code.size() when none is).
[[nodiscard]] std::size_t firstFrom(std::string const& code, std::size_t k) {
    while (k < code.size() && std::isspace(static_cast<unsigned char>(code[k])) != 0) ++k;
    return k;
}

// The identifier whose last character is at `last`, and where it begins.
[[nodiscard]] std::pair<std::string_view, std::size_t> identifierEndingAt(std::string const& code, std::size_t last) {
    std::size_t begin = last + 1;
    while (begin > 0 && isIdentifierChar(code[begin - 1])) --begin;
    return {std::string_view{code}.substr(begin, last + 1 - begin), begin};
}

// How a name is written: bare, after the global scope (`::open`), after a NAMED scope (`std::fopen`,
// `DependencyCache::open` -- `qualifier` names it), or as a member (`in.open`, `p->open` -- `object` names what
// stands before the `.` or `->`, empty when that is no identifier). `start` is where the written name begins.
enum class Written { Bare, Global, Named, Member };
struct NameContext {
    Written          how;
    std::string_view qualifier;
    std::string_view object;
    std::size_t      start;
};

[[nodiscard]] NameContext contextOf(std::string const& code, std::size_t at) {
    std::size_t const k = lastBefore(code, at);
    if (k != std::string::npos && k >= 1 && code[k] == ':' && code[k - 1] == ':') {
        std::size_t const j = lastBefore(code, k - 1);
        if (j != std::string::npos && isIdentifierChar(code[j])) {
            auto const [qualifier, begin] = identifierEndingAt(code, j);
            // An expression keyword is no scope: `return ::open(p)` is a call after the GLOBAL scope.
            if (kExpressionKeywords.count(qualifier) == 0) return {Written::Named, qualifier, {}, begin};
        }
        return {Written::Global, {}, {}, k - 1};
    }
    bool const arrow = k != std::string::npos && k >= 1 && code[k] == '>' && code[k - 1] == '-';
    if (k != std::string::npos && (code[k] == '.' || arrow)) {
        std::size_t const o = lastBefore(code, arrow ? k - 1 : k);
        std::string_view object;
        if (o != std::string::npos && isIdentifierChar(code[o])) object = identifierEndingAt(code, o).first;
        return {Written::Member, {}, object, at};
    }
    return {Written::Bare, {}, {}, at};
}

// Whether the name written from `start` is a DECLARATION's: an identifier that is no expression keyword stands
// before it (`void truncate(`, `LinkedImage link(`, a declaration whose type ends the line before).
[[nodiscard]] bool namesADeclaration(std::string const& code, std::size_t start) {
    std::size_t const k = lastBefore(code, start);
    if (k == std::string::npos || !isIdentifierChar(code[k])) return false;
    std::string_view const word = identifierEndingAt(code, k).first;
    return std::isdigit(static_cast<unsigned char>(word.front())) == 0 && kExpressionKeywords.count(word) == 0;
}

// The text between a call's parentheses when `(` follows the name ending at `end`, or nullopt when none does.
[[nodiscard]] std::optional<std::string_view> callArguments(std::string const& code, std::size_t end) {
    std::size_t const open = firstFrom(code, end);
    if (open >= code.size() || code[open] != '(') return std::nullopt;
    int depth = 0;
    for (std::size_t k = open; k < code.size(); ++k) {
        char const c = code[k];
        if (c == '(' || c == '[' || c == '{') ++depth;
        else if ((c == ')' || c == ']' || c == '}') && --depth == 0)
            return std::string_view{code}.substr(open + 1, k - open - 1);
    }
    return std::string_view{code}.substr(open + 1);
}

[[nodiscard]] bool isBlank(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
}

// Whether `args` names an `O_*` flag (`O_RDONLY`, `O_WRONLY | O_CREAT`): the POSIX open's own argument.
[[nodiscard]] bool namesAnOpenFlag(std::string_view args) {
    for (std::size_t k = args.find("O_"); k != std::string_view::npos; k = args.find("O_", k + 2)) {
        bool const begins = k == 0 || !isIdentifierChar(args[k - 1]);
        if (begins && k + 2 < args.size() && std::isupper(static_cast<unsigned char>(args[k + 2])) != 0) return true;
    }
    return false;
}

[[nodiscard]] bool beginsWithOneOf(std::string_view name, std::vector<std::string_view> const& prefixes) {
    return std::any_of(prefixes.begin(), prefixes.end(), [&](std::string_view p) { return name.substr(0, p.size()) == p; });
}

[[nodiscard]] bool isWin32PathCall(std::string_view name) {
    if (kWin32Names.count(name) != 0 || kWin32Stems.count(name) != 0) return true;
    bool const suffixed = name.size() > 1 && (name.back() == 'A' || name.back() == 'W');
    return (suffixed && kWin32Stems.count(name.substr(0, name.size() - 1)) != 0) || beginsWithOneOf(name, kWin32Prefixes);
}

[[nodiscard]] bool isCrtPathCall(std::string_view name) {
    return kCrtNames.count(name) != 0 || beginsWithOneOf(name, kCrtPrefixes);
}

// What follows a stream type that ends at `end`: the name it declares (empty for none -- a temporary, a template
// argument) and where the declaration goes on after it.
struct StreamUse {
    std::string_view declared;
    std::size_t      next;
};

[[nodiscard]] StreamUse streamUseAfter(std::string const& code, std::size_t end) {
    std::size_t k = firstFrom(code, end);
    if (k < code.size() && code[k] == '<') {   // `std::basic_ifstream<char>`
        for (int depth = 0; k < code.size(); ++k) {
            if (code[k] == '<') ++depth;
            else if (code[k] == '>' && --depth == 0) break;
        }
        k = firstFrom(code, std::min(code.size(), k + 1));
    }
    for (bool more = true; more;) {   // `&`, `*`, `const`, `volatile` before the declared name
        more = false;
        while (k < code.size() && (code[k] == '&' || code[k] == '*')) {
            k    = firstFrom(code, k + 1);
            more = true;
        }
        for (std::string_view const word : {std::string_view{"const"}, std::string_view{"volatile"}}) {
            if (code.compare(k, word.size(), word) == 0
                && (k + word.size() >= code.size() || !isIdentifierChar(code[k + word.size()]))) {
                k = firstFrom(code, k + word.size());
                more = true;
            }
        }
    }
    std::size_t e = k;
    while (e < code.size() && isIdentifierChar(code[e])) ++e;
    if (e == k) return {{}, k};
    return {std::string_view{code}.substr(k, e - k), firstFrom(code, e)};
}

// Every identifier of `code` with where it begins, numbers left out.
template <typename Visit>
void forEachIdentifier(std::string const& code, Visit&& visit) {
    for (std::size_t i = 0; i < code.size();) {
        if (!isIdentifierChar(code[i])) {
            ++i;
            continue;
        }
        std::size_t e = i;
        while (e < code.size() && isIdentifierChar(code[e])) ++e;
        if (std::isdigit(static_cast<unsigned char>(code[i])) == 0) visit(std::string_view{code}.substr(i, e - i), i);
        i = e;
    }
}

// Every name the sources DECLARE with a stream type -- a variable, a parameter, a member: a member a header
// declares is opened in its .cpp, so the names are the whole set's, as the aliases are.
[[nodiscard]] std::set<std::string, std::less<>> streamNamesIn(std::vector<Source> const& sources) {
    std::set<std::string, std::less<>> names;
    for (auto const& s : sources) {
        forEachIdentifier(s.code, [&](std::string_view name, std::size_t at) {
            if (kStreamTypes.count(name) == 0) return;
            NameContext const ctx = contextOf(s.code, at);
            if (ctx.how != Written::Named || ctx.qualifier != "std") return;
            StreamUse const use = streamUseAfter(s.code, at + name.size());
            if (!use.declared.empty()) names.emplace(use.declared);
        });
    }
    return names;
}

// One finding of a host-path family: which family, where, what was written, and what to ask instead.
struct HostFinding {
    char        family;
    std::string where;
    std::string call;
    std::string ask;
};

// What the host-path families read from the whole set of sources: its names for std::filesystem and its streams.
struct HostScope {
    std::set<std::string>              aliases;
    std::set<std::string, std::less<>> streams;
};

// The include a directive line names (`"x/y.hpp"` or `<x/y.hpp>`) from its TEXT -- code blanks a quoted name.
[[nodiscard]] std::string includedName(std::string const& codeLine, std::string const& textLine) {
    std::size_t const hash = firstFrom(codeLine, 0);
    if (hash >= codeLine.size() || codeLine[hash] != '#') return {};
    std::size_t const word = firstFrom(codeLine, hash + 1);
    if (codeLine.compare(word, 7, "include") != 0) return {};
    std::size_t const open = textLine.find_first_of("\"<", word + 7);
    if (open == std::string::npos) return {};
    std::size_t const close = textLine.find(textLine[open] == '"' ? '"' : '>', open + 1);
    return close == std::string::npos ? std::string{} : textLine.substr(open + 1, close - open - 1);
}

// Every host-path call of families (a)-(d) and (f) in one source's code.
[[nodiscard]] std::vector<HostFinding> hostPathCallsIn(Source const& source, HostScope const& scope) {
    std::string const& code = source.code;
    std::vector<std::size_t> lineStarts{0};
    for (std::size_t k = 0; k < code.size(); ++k)
        if (code[k] == '\n') lineStarts.push_back(k + 1);
    auto const where = [&](std::size_t at) {
        auto const line = std::upper_bound(lineStarts.begin(), lineStarts.end(), at) - lineStarts.begin();
        return source.relative + ":" + std::to_string(line);
    };
    std::vector<HostFinding> out;
    forEachIdentifier(code, [&](std::string_view name, std::size_t at) {
        bool const win32 = isWin32PathCall(name);
        bool const crt   = isCrtPathCall(name);
        bool const named = kStreamTypes.count(name) != 0 || name == "open" || kHostOperations.count(name) != 0
                           || name == "substrate_testing";
        if (!win32 && !crt && !named) return;
        NameContext const ctx   = contextOf(code, at);
        std::size_t const end   = at + name.size();
        auto const        args  = callArguments(code, end);
        bool const        call  = args.has_value() && !namesADeclaration(code, ctx.start);
        bool const        plain = ctx.how == Written::Bare || ctx.how == Written::Global;
        bool const        isStd = ctx.how == Written::Named && ctx.qualifier == "std";
        if (win32 && plain && call)
            out.push_back({'a', where(at), std::string{name}, kAskHostCall});
        bool const keyedApart = name == "open" || name == "creat";
        if (crt && (plain || isStd) && call && (!keyedApart || ctx.how == Written::Global || namesAnOpenFlag(*args)))
            out.push_back({'b', where(at), std::string{name}, kAskHostCall});
        if (kStreamTypes.count(name) != 0 && isStd) {
            StreamUse const   use = streamUseAfter(code, end);
            std::size_t const k   = use.next;
            bool const opens = k < code.size() && (code[k] == '(' || code[k] == '{')
                               && firstFrom(code, k + 1) < code.size()
                               && code[firstFrom(code, k + 1)] != (code[k] == '(' ? ')' : '}');
            if (opens) out.push_back({'c', where(at), "std::" + std::string{name} + " on a path", kAskStream});
        }
        if (name == "open" && ctx.how == Written::Member && args.has_value() && scope.streams.count(ctx.object) != 0)
            out.push_back({'c', where(at), std::string{ctx.object} + ".open", kAskStream});
        bool const filesystem = ctx.how == Written::Named
                                && (ctx.qualifier == "filesystem" || scope.aliases.count(std::string{ctx.qualifier}) != 0);
        if (kHostOperations.count(name) != 0 && filesystem)
            out.push_back({'d', where(at), std::string{name}, kHostOperations.find(name)->second});
        else if (kHostOperationsByLookup.count(name) != 0 && ctx.how == Written::Bare && call && !isBlank(*args))
            out.push_back({'d', where(at), std::string{name} + " (unqualified: argument-dependent lookup may reach "
                                           "std::filesystem)", kHostOperations.find(name)->second});
        if (name == "substrate_testing")
            out.push_back({'f', where(at), "substrate_testing", "a test's seam: it is named under tests/ alone"});
    });
    auto const codeLines = linesOf(code);
    auto const textLines = linesOf(source.text);
    for (std::size_t n = 0; n < codeLines.size() && n < textLines.size(); ++n) {
        std::string const name = includedName(codeLines[n], textLines[n]);
        bool const access = name == "host_path_access.hpp"
                            || (name.size() > 21 && name.compare(name.size() - 21, 21, "/host_path_access.hpp") == 0);
        if (access)
            out.push_back({'f', source.relative + ":" + std::to_string(n + 1), "#include " + name,
                           "the substrate's operations (path_identity.hpp, process_spawn.hpp): the access header "
                           "is the home's own"});
    }
    return out;
}

// Whether `relative` is a source the host-path families judge: src/ outside the substrate (H6c's ruled scope; the
// runners keep their own file operations, on DSS-chosen names).
[[nodiscard]] bool isJudgedForHostPaths(std::string const& relative) {
    return isGuarded(relative) && relative.rfind("src/", 0) == 0;
}

// Every host-path finding over a set of sources: the aliases and the streams are the whole set's, the findings the
// judged sources' alone, in the order the sources stand.
[[nodiscard]] std::vector<HostFinding> hostPathCallsAcross(std::vector<Source> const& sources) {
    HostScope const scope{aliasesIn(sources), streamNamesIn(sources)};
    std::vector<HostFinding> found;
    for (auto const& s : sources) {
        if (!isJudgedForHostPaths(s.relative)) continue;
        auto more = hostPathCallsIn(s, scope);
        std::stable_sort(more.begin(), more.end(), [](HostFinding const& a, HostFinding const& b) {
            return std::stoul(a.where.substr(a.where.rfind(':') + 1)) < std::stoul(b.where.substr(b.where.rfind(':') + 1));
        });
        found.insert(found.end(), more.begin(), more.end());
    }
    return found;
}

// `found`'s findings of `family` as `<line>=<call>` words, or `<file> line <line>=<call>` when `withFile`.
[[nodiscard]] std::string spelledFamily(std::vector<HostFinding> const& found, char family, bool withFile) {
    std::string got;
    for (auto const& f : found) {
        if (f.family != family) continue;
        std::size_t const colon = f.where.rfind(':');
        std::string const line  = f.where.substr(colon + 1);
        got += " " + (withFile ? f.where.substr(0, colon) + " line " + line : line) + "=" + f.call;
    }
    return got;
}

// THE FENCE (ledger 580). Until H6d makes the stores' values typed, six forms (`storeRouteNormalized`,
// `storeRouteAbsolute`, `storeRouteReachedPath`, `storeRouteCurrentDirectory`, `storeRouteRunningExecutable`,
// `storeRouteTemporaryDirectory`) and
// two members (`TreeEntry::storeRoutePath`, `TreeWalk::storeRouteWhere`) hand a store the TEXT it keeps today: an
// escape, which any site could take. So every use is pinned -- by file, form and count, over every source the
// guard reads outside the substrate -- and a use outside the pin, or a count other than its pinned one, reds. H6d
// deletes the forms, this pin and the cases that read it.
using StoreRoutePin = std::map<std::string, std::map<std::string, std::size_t>>;
StoreRoutePin const kStoreRoutePin = {
    {"src/core/types/config_path_walk.cpp",
     {{"storeRouteAbsolute", 1}, {"storeRouteCurrentDirectory", 1}, {"storeRouteNormalized", 2}, {"storeRoutePath", 1}, {"storeRouteReachedPath", 1}, {"storeRouteRunningExecutable", 1}}},
    {"src/core/types/glob_match.cpp",
     {{"storeRouteNormalized", 1}, {"storeRoutePath", 3}, {"storeRouteWhere", 1}}},
    {"src/core/types/predefined_macro_json.cpp",
     {{"storeRoutePath", 1}}},
    {"src/ffi/ingest.cpp",
     {{"storeRoutePath", 1}}},
    {"src/ffi/shipped_lib_descriptor.cpp",
     {{"storeRouteNormalized", 1}, {"storeRoutePath", 5}, {"storeRouteWhere", 1}}},
    {"src/lsp/workspace_project.cpp",
     {{"storeRouteAbsolute", 1}, {"storeRoutePath", 1}}},
    {"src/program/input_resolver.cpp",
     {{"storeRoutePath", 1}}},
    {"src/program/program.cpp",
     {{"storeRouteAbsolute", 1}, {"storeRouteCurrentDirectory", 1}, {"storeRoutePath", 1}, {"storeRouteTemporaryDirectory", 1}}},
};

// Every use of a store-route form or member in `sources` outside the substrate, by file and form.
[[nodiscard]] StoreRoutePin storeRouteUsesIn(std::vector<Source> const& sources) {
    StoreRoutePin uses;
    for (auto const& s : sources) {
        if (s.relative.rfind("src/core/substrate/", 0) == 0) continue;
        forEachIdentifier(s.code, [&](std::string_view name, std::size_t) {
            if (name.substr(0, 10) == "storeRoute") ++uses[s.relative][std::string{name}];
        });
    }
    return uses;
}

// Every way `uses` departs from `pin`: a form taken where none is pinned, or a count other than the pinned one.
[[nodiscard]] std::vector<std::string> fenceBreaches(StoreRoutePin const& uses, StoreRoutePin const& pin) {
    std::set<std::pair<std::string, std::string>> keys;
    for (auto const* map : {&uses, &pin})
        for (auto const& [file, forms] : *map)
            for (auto const& form : forms) keys.emplace(file, form.first);
    auto const count = [](StoreRoutePin const& map, std::string const& file, std::string const& form) -> std::size_t {
        auto const f = map.find(file);
        if (f == map.end()) return 0;
        auto const n = f->second.find(form);
        return n == f->second.end() ? 0 : n->second;
    };
    std::vector<std::string> breaches;
    for (auto const& [file, form] : keys) {
        std::size_t const taken = count(uses, file, form);
        std::size_t const fenced = count(pin, file, form);
        if (taken != fenced)
            breaches.push_back(file + ": " + form + " taken " + std::to_string(taken) + " time(s), pinned "
                               + std::to_string(fenced));
    }
    return breaches;
}

// The sources under `tops` (directories under the repository), read through the home: each a LISTED value.
[[nodiscard]] std::vector<Source> sourcesUnder(fs::path const& root, std::vector<std::string> const& tops) {
    std::vector<Source> sources;
    for (std::string const& top : tops) {
        auto const walk = dss::core::walkTree(root / top, [&](dss::core::TreeEntry const& entry) {
            if (entry.kind != dss::core::EntryKind::File) return dss::core::WalkStep::Continue;
            std::string const ext = entry.at.extension().string();
            if (ext == ".cpp" || ext == ".hpp")
                sources.push_back(sourceOf(top + "/" + entry.relative.generic(), readAll(entry.at)));
            return dss::core::WalkStep::Continue;
        });
        EXPECT_FALSE(walk.error) << "the walk of " << top << "/ stopped at " << walk.where.spelling().generic() << ": "
                                 << walk.error.message();
    }
    return sources;
}

// ── THE HOME'S OPERATIONS AS ONE LIST (P69 H6c2, T3): every operation a header of the home declares is a row of
// `DSS_HOST_PATH_OPERATIONS` (src/core/substrate/host_path.hpp), and every row is declared by the header it names.
// The headers are the list's own (its `header` column), read by data. An OPERATION is a declaration of a function at
// namespace or class scope (a definition's head included) that is: a public member of a value type, whatever its
// signature; a friend of one; or any other whose declaration names a path (a value type, or a `::path`). A row names
// the operation's own name: a constructor its class's, an operator its operator-function-id (`operator<<`). ──

// The value types of the home: a public member of one is an operation whatever its signature.
std::set<std::string, std::less<>> const kValueTypes = {"HostPath", "PathText", "ListedRelative", "PathIdentity"};

// `code` with every preprocessor directive blanked, its continuation lines too, the new-lines kept.
[[nodiscard]] std::string withoutDirectives(std::string code) {
    bool inDirective = false;
    for (std::size_t lineStart = 0; lineStart < code.size();) {
        std::size_t const end   = std::min(code.find('\n', lineStart), code.size());
        std::size_t const first = dss::test::skipSpace(std::string_view{code}.substr(0, end), lineStart);
        if (inDirective || (first < end && code[first] == '#')) {
            std::size_t last = end;
            while (last > lineStart && (code[last - 1] == '\r' || code[last - 1] == ' ')) --last;
            inDirective = last > lineStart && code[last - 1] == '\\';
            for (std::size_t k = lineStart; k < end; ++k) code[k] = ' ';
        }
        lineStart = end + 1;
    }
    return code;
}

// Whether a declaration names a path: a value type as a whole word, or `path` after a `::`.
[[nodiscard]] bool namesAPath(std::string_view declaration) {
    bool found = false;
    dss::test::forEachIdentifier(declaration, [&](std::size_t begin, std::size_t end) {
        std::string_view const word = declaration.substr(begin, end - begin);
        if (kValueTypes.count(word) != 0) found = true;
        if (word == "path") {
            std::size_t k = begin;
            while (k > 0 && dss::test::isSpace(declaration[k - 1])) --k;
            if (k >= 2 && declaration.compare(k - 2, 2, "::") == 0) found = true;
        }
        return end;
    });
    return found;
}

// The name a declaration declares a FUNCTION by -- the identifier or operator-function-id before its parameter list,
// the first `(` outside every bracket, `<...>` and `[[...]]` -- and where it stands; or nothing (a variable, a type,
// a data member).
struct Declared {
    std::string name;
    std::size_t at;
};
[[nodiscard]] std::optional<Declared> declaredFunction(std::string_view d) {
    static std::set<std::string, std::less<>> const kNotANameBeforeItsList = {
        "decltype", "alignas", "noexcept", "requires", "sizeof", "static_assert", "explicit", "__attribute__",
        "__declspec"};
    int         angle = 0;
    std::size_t i     = 0;
    while (i < d.size()) {
        char const c = d[i];
        if (d.compare(i, 2, "[[") == 0) {   // an attribute: to its `]]`
            std::size_t const close = d.find("]]", i + 2);
            i = close == std::string_view::npos ? d.size() : close + 2;
            continue;
        }
        if (dss::test::isIdentStart(c) && dss::test::boundaryAt(d, i)) {
            std::size_t const end  = dss::test::identEnd(d, i);
            std::string_view  word = d.substr(i, end - i);
            if (word == "operator") {   // the operator-function-id: `operator` and the operator's own characters
                std::size_t       k  = dss::test::skipSpace(d, end);
                std::size_t const op = k;
                if (d.compare(k, 2, "()") == 0) k += 2;
                else while (k < d.size() && d[k] != '(' && !dss::test::isSpace(d[k])) ++k;
                std::size_t const open = dss::test::skipSpace(d, k);
                if (angle == 0 && open < d.size() && d[open] == '(')
                    return Declared{"operator" + std::string{d.substr(op, k - op)}, i};
                i = k;
                continue;
            }
            std::size_t const next = dss::test::skipSpace(d, end);
            if (next < d.size() && d[next] == '(') {
                if (angle == 0 && kNotANameBeforeItsList.count(word) == 0) {
                    std::size_t k = i;   // a destructor's `~` names no operation of its own
                    while (k > 0 && dss::test::isSpace(d[k - 1])) --k;
                    if (k > 0 && d[k - 1] == '~') return std::nullopt;
                    return Declared{std::string{word}, i};
                }
                // `decltype(...)`, `alignas(...)` or a call inside a template argument: past its parentheses
                int depth = 0;
                for (i = next; i < d.size(); ++i) {
                    if (d[i] == '(') ++depth;
                    if (d[i] == ')' && --depth == 0) break;
                }
                ++i;
                continue;
            }
            i = end;
            continue;
        }
        if (c == '<') ++angle;
        if (c == '>' && angle > 0) --angle;
        if (c == '(' || c == '=' || c == '{') return std::nullopt;   // a list, an initializer: no function named first
        ++i;
    }
    return std::nullopt;
}

// The name a class head declares: the last identifier before its template arguments, its bases or its end, a
// leading `template <...>` passed (`struct std::hash<dss::core::PathIdentity>` -> `hash`; `class DSS_EXPORT HostPath
// final` -> `HostPath`).
[[nodiscard]] std::string classNameOf(std::string_view head) {
    static std::set<std::string, std::less<>> const kNotItsName = {"class", "struct", "union", "final", "DSS_EXPORT"};
    std::size_t from = dss::test::skipSpace(head, 0);
    if (dss::test::wordAt(head, from, "template")) {   // past its parameter list's `>`
        int depth = 0;
        for (std::size_t k = from; k < head.size(); ++k) {
            if (head[k] == '<') ++depth;
            if (head[k] == '>' && --depth == 0) {
                from = k + 1;
                break;
            }
        }
    }
    std::size_t stop = head.size();
    for (std::size_t k = from; k < head.size(); ++k) {
        bool const single = head[k] == ':' && head.compare(k, 2, "::") != 0 && (k == 0 || head[k - 1] != ':');
        if (head[k] == '<' || single) {
            stop = k;
            break;
        }
    }
    std::string name;
    std::string_view const named = head.substr(from, stop - from);
    dss::test::forEachIdentifier(named, [&](std::size_t b, std::size_t e) {
        if (kNotItsName.count(named.substr(b, e - b)) == 0) name = std::string{named.substr(b, e - b)};
        return e;
    });
    return name;
}

// Every operation a header's code declares, by the rule above.
[[nodiscard]] std::set<std::string> operationsDeclaredIn(std::string const& headerCode) {
    std::string const code = withoutDirectives(headerCode);
    struct Scope {
        enum class Kind { Namespace, Class, Body } kind;
        bool valueType = false;
        bool publicNow = true;
    };
    std::vector<Scope>    scopes{{Scope::Kind::Namespace}};
    std::set<std::string> out;
    std::size_t           statement = 0;   // where the statement being read began
    int                   parens    = 0;
    auto const            judge     = [&](std::size_t stop) {
        std::string_view const d     = std::string_view{code}.substr(statement, stop - statement);
        Scope const&           scope = scopes.back();
        if (scope.kind == Scope::Kind::Body) return;
        std::optional<Declared> const declared = declaredFunction(d);
        if (!declared) return;
        // The declared name itself is no path the declaration names (a value type's constructor).
        std::string signature{d};
        for (std::size_t k = 0; k < declared->name.size(); ++k) signature[declared->at + k] = ' ';
        bool const isFriend = dss::test::holdsWord(d, "friend");
        if ((scope.valueType && (scope.publicNow || isFriend)) || namesAPath(signature)) out.insert(declared->name);
    };
    for (std::size_t i = 0; i < code.size(); ++i) {
        char const c = code[i];
        if (c == '(') ++parens;
        if (c == ')') --parens;
        if (parens != 0) continue;
        if (c == ':' && scopes.back().kind == Scope::Kind::Class && code.compare(i, 2, "::") != 0
            && (i == 0 || code[i - 1] != ':')) {
            std::string_view const head = std::string_view{code}.substr(statement, i - statement);
            for (char const* access : {"public", "private", "protected"}) {
                std::size_t const at = dss::test::skipSpace(head, 0);
                if (dss::test::wordAt(head, at, access) && dss::test::skipSpace(head, at + std::strlen(access)) == head.size()) {
                    scopes.back().publicNow = std::string_view{access} == "public";
                    statement               = i + 1;
                }
            }
            continue;
        }
        if (c == ';') {
            judge(i);
            statement = i + 1;
        } else if (c == '{') {
            std::string_view const head = std::string_view{code}.substr(statement, i - statement);
            std::size_t const      at   = dss::test::skipSpace(head, 0);
            Scope                  next{Scope::Kind::Body};
            if (scopes.back().kind != Scope::Kind::Body) {
                bool const isClass  = dss::test::holdsWord(head, "class") || dss::test::holdsWord(head, "struct");
                bool const isEnum   = dss::test::holdsWord(head, "enum");
                bool const function = declaredFunction(head).has_value();
                if (dss::test::wordAt(head, at, "namespace") || dss::test::wordAt(head, at, "extern")) {
                    next.kind = Scope::Kind::Namespace;
                } else if (isClass && !isEnum && !function) {
                    next.kind      = Scope::Kind::Class;
                    next.publicNow = !dss::test::holdsWord(head, "class");
                    next.valueType = kValueTypes.count(classNameOf(head)) != 0;
                } else if (function) {
                    judge(i);   // a definition's head declares its function too
                }
            }
            scopes.push_back(next);
            statement = i + 1;
        } else if (c == '}') {
            if (scopes.size() > 1) scopes.pop_back();
            statement = i + 1;
        }
    }
    return out;
}

} // namespace

// THE TREE: no source in scope asks the library a question its answer to depends on how it reads a link.
TEST(FilesystemQueriesGuard, TheSourceTreeAsksTheSubstrateForEveryFilesystemQuery) {
    fs::path const      root = dss::test::repoRoot();
    std::vector<Source> sources;
    for (char const* top : {"src", "integrated_tests", "tests/examples", "tests/test_support"}) {
        auto const walk = dss::core::walkTree(root / top, [&](dss::core::TreeEntry const& entry) {
            if (entry.kind != dss::core::EntryKind::File) return dss::core::WalkStep::Continue;
            // Each entry a LISTED value (H6c): its names below the top as listed, its extension as text to match.
            std::string const relative = std::string{top} + "/" + entry.relative.generic();
            std::string const ext      = entry.at.extension().string();
            if (ext == ".cpp" || ext == ".hpp") sources.push_back(sourceOf(relative, readAll(entry.at)));
            return dss::core::WalkStep::Continue;
        });
        ASSERT_FALSE(walk.error) << "the walk of " << top << "/ stopped at " << walk.where.spelling().generic() << ": "
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
    EXPECT_TRUE(std::any_of(sources.begin(), sources.end(), [](Source const& s) {
        return s.relative == "tests/examples/examples_runner.cpp";
    })) << "tests/examples/examples_runner.cpp was not read: the scope is short";
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
                 "auto s = stdfs::copy_options::recursive; auto t = stdfs::file_status{};\n"
                 "auto u = std::ranges::count_if(stdfs::directory_iterator{p}, &stdfs::directory_entry::is_regular_file);\n"   // 25
                 "#define FSYS std::filesystem\n"),                                                   // 26
    };
    auto const        found = refusedAcross(sources);
    std::string const got   = spelled(found, /*withFile=*/false);
    std::string const want =
        " 2=exists 3=file_size 4=is_regular_file 5=is_symlink 6=last_write_time 7=equivalent 8=relative "
        "9=permissions 10=remove_all 11=recursive_directory_iterator 12=is_regular_file 13=last_write_time "
        "14=copy_symlinks 15=exists 16=using namespace filesystem 17=using namespace fsx "
        "18=exists (unqualified: argument-dependent lookup may reach std::filesystem) 25=is_regular_file "
        "26=#define FSYS naming filesystem";
    EXPECT_EQ(got, want) << "the scan refused " << found.size() << " form(s):" << got;
}

// AN UNQUALIFIED CALL IS REFUSED WHEREVER IT STANDS (P69 fold H6, the review of H5's N9): whether argument-dependent
// lookup reaches `std::filesystem` depends on the argument's type and on every header the file includes -- a C++
// library spelled `.h` among them -- which no scan of text reads. So, unqualified and with an argument, a refused
// name is refused in every judged file whatever it includes; with no argument, or with a qualifier of its own, it
// is no argument-dependent lookup of a filesystem function and passes; a qualified mention is refused everywhere.
TEST(FilesystemQueriesGuard, AnUnqualifiedCallIsRefusedWhereverItStands) {
    std::vector<Source> const sources = {
        sourceOf("src/a/paths.hpp", "#pragma once\n#include <filesystem>\n"),
        sourceOf("src/c/near.cpp", "#include \"a/paths.hpp\"\nbool f(P const& p) { return exists(p); }\n"),
        sourceOf("integrated_tests/far.cpp", "#include <string>\nauto g(P const& p) { return status(p); }\n"),
        sourceOf("src/d/apart.cpp",
                 "#include <string>\n"
                 "bool h(P const& p) { return exists(p) && status(p); }\n"
                 "auto i(P const& p) { return std::filesystem::status(p); }\n"),
        // In a comment a call is not code.
        sourceOf("src/e/commented.cpp", "// bool j(P const& p) { return exists(p); }\nint j2();\n"),
        // No argument, or a qualifier of its own: not argument-dependent lookup of a filesystem function.
        sourceOf("src/f/nullary.cpp", "#include <filesystem>\nint k() { return status() + n::exists(2); }\n"),
        // A C++ library spelled `.h`, which declares namespaces: refused as anywhere else.
        sourceOf("src/h/library.cpp", "#include <gtest/gtest.h>\nbool n(P const& p) { return exists(p); }\n"),
        // A C or OS header alone: refused the same -- the scan reads no header, so it assumes none.
        sourceOf("src/i/os.cpp", "#include <sys/stat.h>\nbool o(P const& p) { return exists(p); }\n"),
        // Read for its aliases, never judged.
        sourceOf("tests/test_support/helper.hpp", "#pragma once\ninline bool q(P const& p) { return exists(p); }\n"),
    };
    std::string const got = spelled(refusedAcross(sources), /*withFile=*/true);
    std::string const want =
        " src/c/near.cpp line 2=exists (unqualified: argument-dependent lookup may reach std::filesystem)"
        " integrated_tests/far.cpp line 2=status (unqualified: argument-dependent lookup may reach std::filesystem)"
        " src/d/apart.cpp line 2=exists (unqualified: argument-dependent lookup may reach std::filesystem)"
        " src/d/apart.cpp line 2=status (unqualified: argument-dependent lookup may reach std::filesystem)"
        " src/d/apart.cpp line 3=status"
        " src/h/library.cpp line 2=exists (unqualified: argument-dependent lookup may reach std::filesystem)"
        " src/i/os.cpp line 2=exists (unqualified: argument-dependent lookup may reach std::filesystem)";
    EXPECT_EQ(got, want);
}

// THE ONE DIRECTORY LEFT OUT is the substrate; C++ sources under src/ and integrated_tests/ are judged.
TEST(FilesystemQueriesGuard, TheSubstrateIsTheOneDirectoryLeftOut) {
    EXPECT_TRUE(isGuarded("src/core/types/glob_match.cpp"));
    EXPECT_TRUE(isGuarded("src/program/program.hpp"));
    EXPECT_TRUE(isGuarded("integrated_tests/runner.cpp")) << "the CLI runner is judged too";
    EXPECT_TRUE(isGuarded("tests/examples/examples_runner.cpp")) << "and the in-process runner, its twin";
    EXPECT_FALSE(isGuarded("src/core/substrate/path_identity.cpp")) << "the substrate is where the OS is asked";
    EXPECT_FALSE(isGuarded("src/dss-config/runtime/platform/src/dirent.c"));
    EXPECT_FALSE(isGuarded("tests/core/test_header_name_matching.cpp"));
    EXPECT_FALSE(isGuarded("tests/test_support/stage_tree.hpp")) << "read for its aliases, not judged";
    EXPECT_TRUE(isGuarded("src/core/substrate_like/x.cpp")) << "only the substrate directory itself is left out";
}

// ════ WHICH CODE TOUCHES A HOST PATH (P69 H6c) ════════════════════════════════════════════════════════════════════

namespace {

// The sources the host-path cases read, read ONCE for the binary: every top the link family reads.
[[nodiscard]] std::vector<Source> const& treeSources() {
    static std::vector<Source> const sources =
        sourcesUnder(dss::test::repoRoot(), {"src", "integrated_tests", "tests/examples", "tests/test_support"});
    return sources;
}

}  // namespace

// THE TREE: no source under src/ outside the substrate touches a host path but through the home -- no Win32 path
// call, no C-runtime or POSIX path call, no stream on a path, no std::filesystem host operation, and neither the
// access header nor the test seam.
TEST(FilesystemQueriesGuard, TheSourceTreeTouchesAHostPathOnlyThroughItsHome) {
    auto const&       sources = treeSources();
    std::size_t const judged  = static_cast<std::size_t>(std::count_if(
        sources.begin(), sources.end(), [](Source const& s) { return isJudgedForHostPaths(s.relative); }));
    // Not a vacuous pass: src/ holds hundreds of sources outside the substrate (517 when H6c widened the guard).
    EXPECT_GE(judged, 400u) << "the host-path families judged only " << judged << " sources";
    auto const  found = hostPathCallsAcross(sources);
    std::string listed;
    for (auto const& f : found)
        listed += "\n  (" + std::string(1, f.family) + ") " + f.where + ": `" + f.call + "` -- ask the home: " + f.ask;
    EXPECT_TRUE(found.empty()) << found.size() << " host path operation(s) outside src/core/substrate/:" << listed;
}

// THE TREE'S FENCE: every store-route use stands where the pin says, as many times as it says.
TEST(FilesystemQueriesGuard, TheSourceTreeTakesTheStoreRouteOnlyAtItsPinnedSites) {
    auto const uses = storeRouteUsesIn(treeSources());
    EXPECT_FALSE(uses.empty()) << "no store-route use was read: the fence reads nothing";
    std::string listed;
    for (auto const& b : fenceBreaches(uses, kStoreRoutePin)) listed += "\n  " + b;
    EXPECT_TRUE(listed.empty()) << "the store route is taken outside its pin (H6d types the stores; until then a "
                                   "store site is pinned, and no other site takes the route):"
                                << listed;
}

// (a) A WIN32 PATH CALL: bare or after the global scope, every name of the vocabulary with its suffixes and its
// prefixes -- and not a member, a scope's own function, a longer name, a declaration, a mention, prose, the home or
// a runner.
TEST(FilesystemQueriesGuard, AWin32PathCallIsRefusedOutsideTheHome) {
    std::vector<Source> const sources = {
        sourceOf("src/a/win.cpp",
                 "#include <windows.h>\n"
                 "void* a(wchar_t const* p) { return ::CreateFileW(p, 0, 0, nullptr, 3, 0, nullptr); }\n"   // 2
                 "unsigned long b(wchar_t const* p) { return GetFileAttributesW(p); }\n"                     // 3
                 "int c(void* h, void* i) { return SetFileInformationByHandle(h, FileRenameInfo, i, 8); }\n"  // 4
                 "int d() { return CreateProcessW(nullptr, cmd, nullptr, nullptr, 0, 0, nullptr, nullptr, &s, &i); }\n"
                 "long e() { return RtlDosPathNameToNtPathName_U_WithStatus(p, &n, nullptr, nullptr); }\n"   // 6
                 "int f() { return GetPrivateProfileStringW(s, k, d, b, n, p) + GetTempPath2W(n, b); }\n"     // 7
                 "int g(Mock& m) { return m.CreateFileW(p) + Mock::DeleteFileW(p) + CreateFileWrapper(p); }\n"
                 "int h(Mock* m) { return m->RemoveDirectoryW(p); }\n"
                 "// ::CreateFileW(p) in a comment\n"
                 "char const* j = \"DeleteFileW(p) in a string\";\n"
                 "HANDLE WINAPI CreateFileW(wchar_t const* p);\n"
                 "auto k = &::MoveFileExW;\n"
                 "int l(wchar_t const* p) {\n"
                 "    return CopyFileExW(p,\n"                                                               // 15
                 "                       q, nullptr, nullptr, nullptr, 0);\n"
                 "}\n"),
        sourceOf("src/core/substrate/home.cpp",
                 "void* m(wchar_t const* p) { return ::CreateFileW(p, 0, 0, nullptr, 3, 0, nullptr); }\n"),
        sourceOf("integrated_tests/runner_like.cpp",
                 "void* n(wchar_t const* p) { return ::CreateFileW(p, 0, 0, nullptr, 3, 0, nullptr); }\n"),
    };
    std::string const got = spelledFamily(hostPathCallsAcross(sources), 'a', /*withFile=*/true);
    std::string const want =
        " src/a/win.cpp line 2=CreateFileW src/a/win.cpp line 3=GetFileAttributesW"
        " src/a/win.cpp line 4=SetFileInformationByHandle src/a/win.cpp line 5=CreateProcessW"
        " src/a/win.cpp line 6=RtlDosPathNameToNtPathName_U_WithStatus src/a/win.cpp line 7=GetPrivateProfileStringW"
        " src/a/win.cpp line 7=GetTempPath2W src/a/win.cpp line 15=CopyFileExW";
    EXPECT_EQ(got, want);
}

// (b) A C-RUNTIME OR POSIX PATH CALL, `open` and `creat` keyed apart (B3): the global scope's or one with an `O_*`
// flag, on one line or across two. NEGATIVE, each a form MEASURED in the tree: a member, a scope's own function,
// a local `open(braceList, object)`, `guard = open(rule)`, `open(ctx, ...)`, the declarations `void truncate(` and
// `LinkedImage link(` (its type on the line before too), and the tree's own `link` called qualified.
TEST(FilesystemQueriesGuard, ACRuntimePathCallIsRefusedOutsideTheHome) {
    std::vector<Source> const sources = {
        sourceOf("src/b/crt.cpp",
                 "#include <fcntl.h>\n"
                 "int a(char const* p) { return ::open(p, O_RDONLY); }\n"                                    // 2
                 "int b(char const* p) { return open(p, O_WRONLY | O_CREAT, 0666); }\n"                      // 3
                 "int c(char const* p) { return ::creat(p, 0644); }\n"                                       // 4
                 "std::FILE* d(char const* p) { return std::fopen(p, \"rb\"); }\n"                            // 5
                 "std::FILE* e(wchar_t const* p) { return _wfopen(p, L\"rb\"); }\n"                           // 6
                 "int f(char const* p) { return ::unlink(p) + _wstat64(w, &st); }\n"                          // 7
                 "int g(char const* p) { return open(p,\n"                                                    // 8
                 "                                   O_RDONLY); }\n"
                 "void h(Cache& x) { x.open(); y->open(z); }\n"
                 "void i() { DependencyCache::open(dir); TreeBuilder::open(kind); }\n"
                 "void j() { auto open = [](auto&, auto&) {}; open(braceList, object); }\n"
                 "void k() { guard = open(rule); }\n"
                 "void l() { open(ctx, schema); }\n"
                 "void truncate(std::size_t n) { size_ = n; }\n"
                 "LinkedImage link(std::span<Module const> modules, Schema const& s) {\n"
                 "    return dss::linker::link(std::span<Module const>{&m, 1}, s); }\n"
                 "[[nodiscard]] DSS_EXPORT LinkedImage\n"
                 "link(Module const& m);\n"
                 "int m(char const* p) { return remove(p) + std::rename(p, q); }\n"                           // 20
                 "void n(char const* p) { if (p) {} else unlink(p); }\n"                                      // 21
                 "// ::open(p, O_RDONLY) in a comment\n"
                 "struct stat st; int o(Cache& c) { return c.stat(1); }\n"),
        sourceOf("src/core/substrate/home.cpp", "int q(char const* p) { return ::open(p, O_RDONLY); }\n"),
    };
    std::string const got = spelledFamily(hostPathCallsAcross(sources), 'b', /*withFile=*/false);
    std::string const want = " 2=open 3=open 4=creat 5=fopen 6=_wfopen 7=unlink 7=_wstat64 8=open 20=remove 20=rename "
                             "21=unlink";
    EXPECT_EQ(got, want);
}

// (c) A STREAM ON A PATH: a stream type constructed with an argument (a variable, a temporary, a template's), or a
// name the sources declare as a stream -- a header's member included -- opened. NEGATIVE: the home's stream taken
// by value, the MEASURED `sv.open(rule)` and `documents_.open(uri, text)` (no stream), a stream made with no
// argument, a string stream, prose and the home.
TEST(FilesystemQueriesGuard, AStreamOnAPathIsOpenedOnlyByTheHome) {
    std::vector<Source> const sources = {
        sourceOf("src/c/log.hpp", "#pragma once\n#include <fstream>\nstruct Log { std::ofstream out_; };\n"),
        sourceOf("src/c/streams.cpp",
                 "#include \"log.hpp\"\n"
                 "void a(P const& p) { std::ifstream in(p, std::ios::binary); }\n"                             // 2
                 "void b(P const& p) { auto o = std::ofstream{p}; }\n"                                         // 3
                 "void c(W const& w) { std::wifstream const x(w); }\n"                                         // 4
                 "void d(P const& p) { std::fstream s; s.open(p); }\n"                                         // 5
                 "void e(Log& l, P const& p) { l.out_.open(p, std::ios::app); }\n"                             // 6
                 "void f(std::basic_ifstream<char>& r, P const& p) { r.open(p); }\n"                           // 7
                 "void g(P const& p) { std::filebuf fb; fb.open(p, std::ios::in); }\n"                         // 8
                 "void h(P const& p) { std::ifstream in = core::openInputStream(p, std::ios::binary); }\n"
                 "void i(Builder& sv) { sv.open(rule); }\n"
                 "void j(Store& documents_) { documents_.open(uri, text); }\n"
                 "void k() { std::ifstream none; std::ofstream{}; std::ostringstream os(text); }\n"
                 "// std::ifstream in(p); in a comment\n"
                 "char const* m = \"std::ofstream o(p)\";\n"),
        sourceOf("src/core/substrate/home.cpp",
                 "std::ifstream openInputStream(P const& p) { return std::ifstream(p); }\n"),
    };
    std::string const got = spelledFamily(hostPathCallsAcross(sources), 'c', /*withFile=*/false);
    std::string const want = " 2=std::ifstream on a path 3=std::ofstream on a path 4=std::wifstream on a path "
                             "5=s.open 6=out_.open 7=r.open 8=fb.open";
    EXPECT_EQ(got, want);
}

// (d) A HOST OPERATION OF std::filesystem the link family lets through: mentioned through any of its names (an alias
// declared in a header included), or called unqualified with an argument where only argument-dependent lookup
// tells it from the tree's own. NEGATIVE: the library's types and options that are no operation, the MEASURED local
// `space(c)`, an algorithm's `copy`, a member, another scope's function, the home's own answer and the home.
TEST(FilesystemQueriesGuard, AHostOperationIsAskedOfTheHome) {
    std::vector<Source> const sources = {
        sourceOf("src/d/ops.hpp", "#pragma once\nnamespace stdfs = std::filesystem;\nnamespace fs = stdfs;\n"),
        sourceOf("src/d/ops.cpp",
                 "#include \"ops.hpp\"\n"
                 "void a(P const& p) { std::filesystem::create_directories(p); }\n"                         // 2
                 "auto b(P const& p) { return stdfs::absolute(p); }\n"                                        // 3
                 "void c(P const& p) { stdfs::remove(p); fs::rename(p, p); }\n"                               // 4
                 "auto d() { return std::filesystem::current_path() / std::filesystem::temp_directory_path(); }\n"
                 "void e(P const& p) { fs::copy_file(p, p); fs::copy(p, p); fs::resize_file(p, 0); }\n"       // 6
                 "void f(P const& p) { for (auto const& x : fs::directory_iterator{p}) use(x); }\n"           // 7
                 "void g(P const& p) { fs::directory_entry const e{p}; }\n"                                    // 8
                 "auto h(P const& p) { return fs::space(p).available; }\n"                                     // 9
                 "void i(P const& p) { create_directories(p); copy_file(p, p); }\n"                            // 10
                 "using std::filesystem::create_symlink;\n"                                                    // 11
                 "auto j = fs::copy_options::recursive; auto k = fs::file_status{}; fs::space_info l{};\n"
                 "auto m(std::string const& x) { return fs::path{x} / fs::path{\"y\"}; }\n"
                 "bool n(std::string const& s, std::size_t i) { return space(s[i]); }\n"
                 "void o(V const& v, O out) { copy(v.begin(), v.end(), out); }\n"
                 "void q(X& x) { x.create_directories(p); Paths::absolute(p); current_path(); }\n"
                 "bool r(P const& p, std::error_code& ec) { return core::createDirectories(p, ec); }\n"),
        sourceOf("src/core/substrate/home.cpp", "void s(P const& p) { std::filesystem::create_directories(p); }\n"),
    };
    std::string const got = spelledFamily(hostPathCallsAcross(sources), 'd', /*withFile=*/false);
    std::string const want =
        " 2=create_directories 3=absolute 4=remove 4=rename 5=current_path 5=temp_directory_path 6=copy_file 6=copy "
        "6=resize_file 7=directory_iterator 8=directory_entry 9=space "
        "10=create_directories (unqualified: argument-dependent lookup may reach std::filesystem) "
        "10=copy_file (unqualified: argument-dependent lookup may reach std::filesystem) 11=create_symlink";
    EXPECT_EQ(got, want);
}

// (f) THE HOME'S OWN: the access header (host_path_access.hpp), which hands out reference spellings, and the test
// seam (`substrate_testing`), refused outside src/core/substrate/ -- in either include spelling, and in a directory
// that only shares the substrate's prefix. NEGATIVE: the value's own header, prose, and the substrate itself.
TEST(FilesystemQueriesGuard, TheAccessHeaderAndTheTestSeamStayInTheHome) {
    std::vector<Source> const sources = {
        sourceOf("src/f/user.cpp",
                 "#include \"core/substrate/host_path_access.hpp\"\n"                                            // 1
                 "#include <core/substrate/host_path_access.hpp>\n"                                              // 2
                 "auto a(V const& v) { return dss::core::substrate_testing::referenceForTest(v, Use::Leaf); }\n"  // 3
                 "#include \"core/substrate/host_path.hpp\"\n"
                 "// #include \"core/substrate/host_path_access.hpp\" in a comment\n"
                 "char const* s = \"substrate_testing\";\n"),
        sourceOf("src/core/substrate/process_spawn.cpp",
                 "#include \"core/substrate/host_path_access.hpp\"\nnamespace substrate_testing {}\n"),
        sourceOf("src/core/substrate_like/x.cpp", "#include \"host_path_access.hpp\"\n"),                     // 1
    };
    std::string const got = spelledFamily(hostPathCallsAcross(sources), 'f', /*withFile=*/true);
    std::string const want = " src/f/user.cpp line 1=#include core/substrate/host_path_access.hpp"
                             " src/f/user.cpp line 2=#include core/substrate/host_path_access.hpp"
                             " src/f/user.cpp line 3=substrate_testing"
                             " src/core/substrate_like/x.cpp line 1=#include host_path_access.hpp";
    EXPECT_EQ(got, want);
}

// THE FENCE ITSELF (ledger 580): a pinned set of uses passes; a use in a file the pin does not name, one more use
// of a pinned form, and a pinned use that is gone each red -- and prose, the substrate's own definitions and the
// home's other names count for nothing.
TEST(FilesystemQueriesGuard, TheFenceRefusesEveryStoreRouteUseItDoesNotPin) {
    StoreRoutePin const pin = {
        {"src/store/a.cpp", {{"storeRouteNormalized", 1}, {"storeRoutePath", 1}}},
        {"src/store/b.cpp", {{"storeRouteWhere", 1}}},
    };
    std::string const a = "auto x = core::storeRouteNormalized(p); auto y = e.storeRoutePath; // storeRoutePath\n";
    std::string const b = "auto z = w.storeRouteWhere; char const* s = \"storeRouteWhere\"; auto r = core::route(p);\n";
    std::string const home = "fs::path storeRoutePath; fs::path storeRouteNormalized(fs::path const& p);\n";
    auto const breaches = [&](std::vector<Source> const& sources) {
        std::string got;
        for (auto const& line : fenceBreaches(storeRouteUsesIn(sources), pin)) got += "[" + line + "]";
        return got;
    };
    EXPECT_EQ(breaches({sourceOf("src/store/a.cpp", a), sourceOf("src/store/b.cpp", b),
                        sourceOf("src/core/substrate/path_identity.cpp", home)}),
              "")
        << "the pinned uses, and only they, must pass";
    EXPECT_EQ(breaches({sourceOf("src/store/a.cpp", a), sourceOf("src/store/b.cpp", b),
                        sourceOf("src/other/c.cpp", "auto q = e.storeRoutePath;\n")}),
              "[src/other/c.cpp: storeRoutePath taken 1 time(s), pinned 0]")
        << "a use in a file the pin does not name";
    EXPECT_EQ(breaches({sourceOf("src/store/a.cpp", a + "auto again = core::storeRouteNormalized(q);\n"),
                        sourceOf("src/store/b.cpp", b)}),
              "[src/store/a.cpp: storeRouteNormalized taken 2 time(s), pinned 1]")
        << "one more use of a pinned form";
    EXPECT_EQ(breaches({sourceOf("src/store/a.cpp", a), sourceOf("src/store/b.cpp", "auto z = 1;\n")}),
              "[src/store/b.cpp: storeRouteWhere taken 0 time(s), pinned 1]")
        << "a pinned use that is gone: the pin is the census, and H6d lowers it with every store it types";
}

// THE HOME'S OPERATIONS AS ONE LIST (T3): every operation each listed header declares is a row, and every row is
// declared by the header it names -- the headers are the list's own, read by data.
TEST(FilesystemQueriesGuard, EveryOperationOfTheHomeIsInTheList) {
    fs::path const                               root = dss::test::repoRoot() / "src" / "core" / "substrate";
    std::map<std::string, std::set<std::string>> rows;   // header stem -> the names its rows give
    for (auto const& op : dss::core::kHostPathOperations) rows[op.header].insert(op.name);
    ASSERT_GE(rows.size(), 5u) << "the list names " << rows.size() << " headers";
    std::string missing;
    std::size_t declaredInAll = 0;
    for (auto const& [header, names] : rows) {
        std::string const text = readAll(dss::core::HostPath{root / (header + ".hpp")});
        ASSERT_FALSE(text.empty()) << "src/core/substrate/" << header << ".hpp was not read";
        std::set<std::string> const declared = operationsDeclaredIn(dss::test::codeOnly(text));
        declaredInAll += declared.size();
        for (auto const& d : declared)
            if (names.count(d) == 0) missing += "\n  " + header + ".hpp declares `" + d + "`, which no row names";
        for (auto const& n : names)
            if (declared.count(n) == 0) missing += "\n  the row `" + n + "` names no operation " + header + ".hpp declares";
    }
    EXPECT_GE(declaredInAll, 80u) << "the scan read only " << declaredInAll << " operations";
    EXPECT_TRUE(missing.empty()) << "DSS_HOST_PATH_OPERATIONS and the headers disagree:" << missing;
}

// THE OPERATION SCAN ITSELF, on a synthetic header holding every declaration it must read and every one it must not.
TEST(FilesystemQueriesGuard, TheOperationScanReadsEveryOperationAndNothingElse) {
    std::string const header = R"~(#pragma once
#define LIST(X) \
    X("listed", h, Spelling) \
    X(notThis(HostPath), h, Spelling)
namespace dss::core {
class HostPath {
public:
    HostPath() = default;
    explicit HostPath(std::filesystem::path p);
    ~HostPath();
    [[nodiscard]] Spelling spelling() const;
    [[nodiscard]] std::size_t size() const noexcept { return helper(text_).size(); }
    friend std::ostream& operator<<(std::ostream& os, HostPath const& p);
    HostPath& operator=(HostPath const&) = default;
private:
    [[nodiscard]] int hidden() const;
    static HostPath madeHere(int origin);
    std::filesystem::path text_;
};
struct TreeEntry {
    HostPath at;
    int      kind = compute(3);
    [[nodiscard]] bool isFile() const;
};
enum class Kind : int { A, B };
[[nodiscard]] DSS_EXPORT HostPath reachedPath(HostPath const& p, Use use, std::error_code& ec);
[[nodiscard]] DSS_EXPORT std::filesystem::path programPath(std::string_view utf8);
DSS_EXPORT void walk(HostPath const& root, std::function<Step(Entry const&)> const& visit, Options o = {});
template <typename Visit> void forEachName(std::string const& path, Visit&& v);
[[nodiscard]] constexpr bool takesText(std::string_view text) noexcept;
inline decltype(auto) typed(fs::path const& p) { return p; }
inline constexpr int kTable[] = {LIST(ROW)};
}
template <>
struct std::hash<dss::core::HostPath> {
    std::size_t operator()(dss::core::HostPath const& p) const noexcept;
};
)~";
    std::set<std::string> const want = {"HostPath",    "spelling", "size",     "operator<<",  "operator=",
                                        "madeHere",    "reachedPath", "programPath", "walk",   "typed",
                                        "operator()"};
    EXPECT_EQ(operationsDeclaredIn(dss::test::codeOnly(header)), want);
}

// ════ THE OS'S REASON IS RENDERED BY ITS ONE HOME (P69 H6c2 (e), main's ruling 684) ═══════════════════════════════════
// A host primitive that fails answers the OS's reason, and every diagnostic built on it renders that reason through the
// ONE rendering, `core::reasonText` (src/core/substrate/path_identity.cpp): the code AND the system's text. Outside that
// home, on CODE-ONLY text (comments and literals blanked), no source of src/ may render a reason another way:
//  R1  a call of a member named `message`, with ANY argument list (`.message(`, `->message(`) -- an error_code's text
//      without its number; lexical, so it refuses ANY receiver: it fails toward LOUD, and a future member of that name
//      is renamed, never excepted;
//  R2  BY TYPE: `<name>.what()` / `<name>->what()` where the file binds <name> as a `::system_error`,
//      `::filesystem_error` or `::failure` (a handler's parameter or a declaration, the type spelled after `::`);
//  R3  a SELF-RENDERER of an OS code: `FormatMessage[AW]`, `strerror[_r|_s]`, `_strerror[_s]`, `_wcserror[_s]`,
//      `strerrordesc_np`, `strerrorname_np`, called;
//  R4  BY TYPE: a `std::error_code` the file declares by name, streamed (`<< name`) or handed whole to
//      `std::format` / `std::vformat` / `std::print` / `std::println` -- a code rendered as "category:value".
// NO EXEMPTION LIST: the one file not judged is the home itself, where R1 must find the home's own rendering (the
// control that the scan sees a real call). STATED BLINDNESS: R2 and R4 see only names the SAME file declares with the
// type spelled -- not an `auto`, not a member declared in another file, not a template parameter, not a `using` of the
// bare type name; a `catch (std::exception const&)` handler's `what()` has no type in the text (pinned below AS
// blindness). R1 and R3 are lexical and see every call.
namespace {

constexpr std::string_view kReasonHome = "src/core/substrate/path_identity.cpp";

struct ReasonFinding {
    std::string relative;
    std::size_t line = 0;
    std::string rule;
    std::string text;
};

[[nodiscard]] std::size_t reasonLineAt(std::string const& code, std::size_t at) {
    return static_cast<std::size_t>(std::count(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(at), '\n')) + 1;
}

// The last character before `k` that is not a space (npos when none).
[[nodiscard]] std::size_t reasonCodeBefore(std::string const& code, std::size_t k) {
    while (k > 0) {
        --k;
        if (!dss::test::isSpace(code[k])) return k;
    }
    return std::string::npos;
}

// Does a member access (`.` or `->`) end just before the word at `b`? Answers the receiver's last character's index,
// or npos.
[[nodiscard]] std::size_t reasonMemberAccessBefore(std::string const& code, std::size_t b) {
    std::size_t const k = reasonCodeBefore(code, b);
    if (k == std::string::npos) return std::string::npos;
    if (code[k] == '.') return reasonCodeBefore(code, k);
    if (code[k] == '>' && k > 0 && code[k - 1] == '-') return reasonCodeBefore(code, k - 1);
    return std::string::npos;
}

[[nodiscard]] bool reasonCalledAt(std::string const& code, std::size_t end) {
    std::size_t const k = dss::test::skipSpace(code, end);
    return k < code.size() && code[k] == '(';
}

// The identifier ENDING at `last` (inclusive), or empty.
[[nodiscard]] std::string_view reasonIdentifierEndingAt(std::string const& code, std::size_t last) {
    if (last == std::string::npos || !dss::test::isWordChar(code[last])) return {};
    std::size_t b = last;
    while (b > 0 && dss::test::isWordChar(code[b - 1])) --b;
    if (!dss::test::isIdentStart(code[b])) return {};
    return std::string_view{code}.substr(b, last + 1 - b);
}

// The names the file declares with one of `typeWords` spelled before them (`std::error_code const& why`,
// `catch (std::filesystem::filesystem_error const& e)`): the identifier after the type word, its cv and reference
// marks skipped. With `afterScope`, the type word must follow `::` (`std::system_error`, `std::ios_base::failure`).
[[nodiscard]] std::set<std::string, std::less<>> reasonNamesDeclaredAs(std::string const& code,
                                                                       std::vector<std::string_view> const& typeWords,
                                                                       bool afterScope) {
    std::set<std::string, std::less<>> names;
    dss::test::forEachIdentifier(code, [&](std::size_t b, std::size_t e) {
        std::string_view const word{code.data() + b, e - b};
        if (std::find(typeWords.begin(), typeWords.end(), word) == typeWords.end()) return e;
        if (afterScope) {
            std::size_t const k = reasonCodeBefore(code, b);
            if (k == std::string::npos || k == 0 || code[k] != ':' || code[k - 1] != ':') return e;
        }
        std::size_t k = e;
        for (;;) {
            k = dss::test::skipSpace(code, k);
            if (k < code.size() && (code[k] == '&' || code[k] == '*')) {
                ++k;
                continue;
            }
            if (dss::test::wordAt(code, k, "const")) {
                k += 5;
                continue;
            }
            break;
        }
        std::size_t const end = dss::test::identEnd(code, k);
        if (end > k) names.emplace(code.substr(k, end - k));
        return e;
    });
    return names;
}

[[nodiscard]] std::vector<ReasonFinding> reasonRenderingsIn(Source const& source) {
    std::string const&         code = source.code;
    std::vector<ReasonFinding> found;
    auto const report = [&](std::size_t at, char const* rule) {
        std::size_t const lineStart = code.rfind('\n', at) == std::string::npos ? 0 : code.rfind('\n', at) + 1;
        std::size_t const lineEnd   = code.find('\n', at);
        std::string       text      = code.substr(lineStart, lineEnd == std::string::npos ? std::string::npos
                                                                                          : lineEnd - lineStart);
        found.push_back({source.relative, reasonLineAt(code, at), rule, std::move(text)});
    };
    static std::vector<std::string_view> const kSelfRenderers = {
        "FormatMessage", "FormatMessageA", "FormatMessageW", "strerror", "strerror_r", "strerror_s", "_strerror",
        "_strerror_s",   "_wcserror",      "_wcserror_s",    "strerrordesc_np", "strerrorname_np"};
    static std::vector<std::string_view> const kFormatters = {"format", "vformat", "print", "println"};
    auto const errorCodes = reasonNamesDeclaredAs(code, {"error_code"}, false);
    auto const exceptions = reasonNamesDeclaredAs(code, {"system_error", "filesystem_error", "failure"}, true);
    dss::test::forEachIdentifier(code, [&](std::size_t b, std::size_t e) {
        std::string_view const word{code.data() + b, e - b};
        if (word == "message" && reasonMemberAccessBefore(code, b) != std::string::npos && reasonCalledAt(code, e))
            report(b, "R1");   // R1: a member named `message`, called
        else if (word == "what" && reasonCalledAt(code, e)) {
            std::size_t const receiverEnd = reasonMemberAccessBefore(code, b);
            if (receiverEnd != std::string::npos && exceptions.contains(reasonIdentifierEndingAt(code, receiverEnd)))
                report(b, "R2");   // R2: the what() of an OS-reason exception the file names
        } else if (std::find(kSelfRenderers.begin(), kSelfRenderers.end(), word) != kSelfRenderers.end()
                   && reasonCalledAt(code, e))
            report(b, "R3");   // R3: a self-renderer of an OS code
        else if (std::find(kFormatters.begin(), kFormatters.end(), word) != kFormatters.end() && reasonCalledAt(code, e)
                 && reasonMemberAccessBefore(code, b) == std::string::npos) {
            std::size_t const open  = dss::test::skipSpace(code, e);
            int               depth = 0;
            std::size_t       close = open;
            for (; close < code.size(); ++close) {
                if (code[close] == '(' || code[close] == '[' || code[close] == '{') ++depth;
                else if ((code[close] == ')' || code[close] == ']' || code[close] == '}') && --depth == 0) break;
            }
            for (std::string const& argument :
                 dss::test::topLevelArguments(std::string_view{code}.substr(open + 1, close - open - 1))) {
                std::size_t const a = dss::test::skipSpace(argument, 0);
                std::size_t const z = dss::test::identEnd(argument, a);
                if (z > a && dss::test::skipSpace(argument, z) == argument.size()
                    && errorCodes.contains(std::string_view{argument}.substr(a, z - a)))
                    report(b, "R4");   // R4: a declared error_code handed whole to a formatter
            }
        }
        return e;
    });
    for (std::size_t at = code.find("<<"); at != std::string::npos; at = code.find("<<", at + 2)) {
        std::size_t const a = dss::test::skipSpace(code, at + 2);
        std::size_t const z = dss::test::identEnd(code, a);
        if (z == a || !errorCodes.contains(std::string_view{code}.substr(a, z - a))) continue;
        std::size_t const next = dss::test::skipSpace(code, z);
        if (next < code.size() && (code[next] == '(' || code[next] == '.' || code[next] == '-' || code[next] == '['))
            continue;   // a call, a member or an element of it: not the code itself
        report(at, "R4");   // R4: a declared error_code streamed
    }
    std::sort(found.begin(), found.end(),
              [](ReasonFinding const& x, ReasonFinding const& y) { return x.line < y.line; });
    return found;
}

[[nodiscard]] std::string reasonSpelled(std::vector<ReasonFinding> const& found) {
    std::string out;
    for (ReasonFinding const& f : found)
        out += "\n  " + f.relative + ":" + std::to_string(f.line) + " " + f.rule + ": " + f.text;
    return out;
}

}  // namespace

// THE TREE: every source of src/ outside the home renders an OS reason through `core::reasonText` -- zero findings.
// Its controls: the walk read at least 400 sources (a non-vacuity floor, never an exemption: every file read is
// judged), and the home was read and R1 finds its own rendering there (the scan sees a real call).
TEST(FilesystemQueriesGuard, EveryOsReasonIsRenderedByTheOneHome) {
    std::vector<Source> const sources = sourcesUnder(dss::test::repoRoot(), {"src"});
    ASSERT_GE(sources.size(), 400u) << "the walk of src/ read too little to judge";
    std::vector<ReasonFinding> found;
    bool                       homeRead = false;
    for (Source const& source : sources) {
        if (source.relative == kReasonHome) {
            homeRead = true;
            auto const own = reasonRenderingsIn(source);
            EXPECT_TRUE(std::any_of(own.begin(), own.end(), [](ReasonFinding const& f) { return f.rule == "R1"; }))
                << "control: R1 does not see the home's own `reason.message()`";
            continue;
        }
        auto const here = reasonRenderingsIn(source);
        found.insert(found.end(), here.begin(), here.end());
    }
    EXPECT_TRUE(homeRead) << "the home " << kReasonHome << " was not read";
    EXPECT_TRUE(found.empty()) << found.size() << " reason(s) rendered outside the home -- render each through "
                               << "core::reasonText(<the error_code>):" << reasonSpelled(found);
}

// THE SCAN, ON A SYNTHESIZED NEGATIVE: every refused form is found ONCE, by its rule, on its line; every near miss is
// found NOWHERE -- the home's call, a comment, a literal, other names -- and the stated blindness (a base handler's
// `what()`, an `auto` code streamed) is pinned AS blindness, so a scan that learns to see it must say so here.
TEST(FilesystemQueriesGuard, TheReasonScanFindsEveryRefusedFormAndNoNearMiss) {
    std::vector<std::string> const lines = {
        "void a(std::error_code const& ec) { use(ec.message()); }",                                          // 1 R1
        "void b(Holder* p) { use(p->message(3)); }",                                                         // 2 R1
        "void c(int e) { use(std::generic_category().message(e)); }",                                        // 3 R1
        "void d() { try { f(); } catch (std::system_error const& e) { use(e.what()); } }",                   // 4 R2
        "void d2() { try { f(); } catch (std::filesystem::filesystem_error const& fe) { use(fe.what()); } }",  // 5 R2
        "void g(wchar_t* t) { FormatMessageW(0, nullptr, 5, 0, t, 0, nullptr); }",                           // 6 R3
        "void h() { use(std::strerror(errno)); }",                                                           // 7 R3
        "void i() { std::error_code ec; out << ec; }",                                                       // 8 R4
        "void j() { std::error_code why; use(std::format(\"{}\", why)); }",                                  // 9 R4
        "void k(std::error_code const& ec) { use(core::reasonText(ec)); }",                                  // 10
        "// ec.message(), strerror(errno), out << ec",                                                       // 11
        "void l() { use(\"strerror(errno) and e.what() and ec.message()\"); }",                              // 12
        "void m() { try { f(); } catch (std::exception const& any) { use(any.what()); } }",                  // 13 R2 blind
        "void n() { use(messages()); use(messageCount(3)); use(o.messageText()); }",                         // 14
        "void p() { auto code = f(); out << code; }",                                                        // 15 R4 blind
        "void q() { std::error_code ec; out << ec.value(); use(std::format(\"{}\", ec.value())); }",         // 16
        "void r(Settings const& s) { use(s.message); }",                                                     // 17
    };
    std::string text;
    for (std::string const& l : lines) text += l + "\n";
    auto const found = reasonRenderingsIn(sourceOf("src/fixture/reasons.cpp", text));
    std::vector<std::pair<std::size_t, std::string>> got;
    for (ReasonFinding const& f : found) got.emplace_back(f.line, f.rule);
    std::vector<std::pair<std::size_t, std::string>> const want = {
        {1, "R1"}, {2, "R1"}, {3, "R1"}, {4, "R2"}, {5, "R2"}, {6, "R3"}, {7, "R3"}, {8, "R4"}, {9, "R4"}};
    EXPECT_EQ(got, want) << reasonSpelled(found);
}
