// EVERY PLACE AN ASSEMBLED MODULE HOLDS A SymbolId IS READ BY ITS SCAN -- derived from the TYPES, in the repo (P69
// lane `cs`, fold f26d -- review-csf26c2 N9, the holder axis).
//
// ★ THE CLASS. A link-tier door mints past every id the module holds; the holders are the rows of its table
// (link/fresh_symbol_ids.hpp, `kModuleSymbolIdHolders`). The scan it replaced read 14 of the 19 holders by hand
// (function and data relocation targets, `linkPositionOfDefinition`'s keys, `staticInitSchedule[].symbol` and
// `userEntrySymbol` were missed), and a hand table kept beside a hand visitor is the same shape one member later.
// So the holders are DERIVED here from the types: every struct and class body under src/**/*.hpp is parsed, the walk
// starts at `AssembledModule` and follows every member whose type names a parsed struct (directly, or inside a
// vector / optional / unordered_map / map / array / pair / variant), and every member whose type names `SymbolId` is
// a holder, spelled by its member names (`functions.relocations.target`). The derived set must equal the table's,
// in BOTH directions: a SymbolId member added later is red here, naming its path, and so is a stale row. That each
// row is VISITED, not only declared, is the pin's (test_fresh_symbol_ids.cpp: one cell per row, the module's only
// maximum in that holder).
//
// WHAT THE WALK CANNOT SEE: a raw integer member that holds an id (none in `AssembledModule`, MEASURED 2026-11-14 --
// its `std::uint32_t` members are counts, offsets and indices); a member type defined outside src/**/*.hpp (none:
// every member type of the walked structs resolves). A member declared on a line that also opens a parenthesis is
// taken for a function and not read, as the walk was written; the comparison names any holder that hides that way
// the day the table names it.

#include "core/substrate/path_identity.hpp"   // walkTree
#include "link/fresh_symbol_ids.hpp"          // kModuleSymbolIdHolders
#include "repo_root.hpp"
#include "source_code_only.hpp"               // codeOnly
#include "text_scan.hpp"                      // the linear scanners

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Field {
    std::string type;
    std::string name;
};

// A source's bytes, read through the home: `p` is the walk's LISTED value, never its text re-resolved.
[[nodiscard]] std::string readAll(dss::core::HostPath const& p) {
    std::ifstream in = dss::core::openInputStream(p, std::ios::binary);
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// THE LINE PATTERNS, read by loops (text_scan.hpp), never `std::regex`: a regex match recurses once per character it
// consumes, so its stack follows the input (a repo guard died of it on windows-x86_64-debug). Each is the same
// language as the pattern stated beside it, its alternatives and its lazy repetition tried in the regex's order.

// A struct or class head that opens its body on the line -- the WHOLE line matching
// `^\s*(?:struct|class)\s+(?:DSS_EXPORT\s+)?([A-Za-z_]\w*)\s*(?:final\s*)?(?::[^{]*)?\{\s*$` -- its name, or "".
[[nodiscard]] std::string structHeadName(std::string const& line) {
    using dss::test::isSpace;
    using dss::test::skipSpace;
    std::size_t k = skipSpace(line, 0);
    if (line.compare(k, 6, "struct") == 0) k += 6;
    else if (line.compare(k, 5, "class") == 0) k += 5;
    else return {};
    if (k >= line.size() || !isSpace(line[k])) return {};
    k = skipSpace(line, k);
    // From the name at `at`: `([A-Za-z_]\w*)\s*(?:final\s*)?(?::[^{]*)?\{\s*$`.
    auto const fromName = [&](std::size_t at) -> std::string {
        if (at >= line.size() || !dss::test::isIdentStart(line[at])) return {};
        std::size_t e = at + 1;
        while (e < line.size() && dss::test::isWordChar(line[e])) ++e;
        auto const closes = [&](std::size_t p) {   // `(?::[^{]*)?\{\s*$` at p
            if (p < line.size() && line[p] == ':') {
                std::size_t const brace = line.find('{', p);
                if (brace != std::string::npos && skipSpace(line, brace + 1) == line.size()) return true;
            }
            return p < line.size() && line[p] == '{' && skipSpace(line, p + 1) == line.size();
        };
        std::size_t const p = skipSpace(line, e);
        if (line.compare(p, 5, "final") == 0 && closes(skipSpace(line, p + 5))) return line.substr(at, e - at);
        return closes(p) ? line.substr(at, e - at) : std::string{};
    };
    if (line.compare(k, 10, "DSS_EXPORT") == 0 && k + 10 < line.size() && isSpace(line[k + 10]))
        if (std::string name = fromName(skipSpace(line, k + 10)); !name.empty()) return name;
    return fromName(k);
}

// Does the rest of a member line, from the white space at `e`, read
// `\s+([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:\{[^;]*\}|=[^;]*)?;`? Its name when it does, else "".
[[nodiscard]] std::string memberNameFrom(std::string const& line, std::size_t e) {
    using dss::test::skipSpace;
    std::size_t k = skipSpace(line, e);
    if (k == e || k >= line.size() || !dss::test::isIdentStart(line[k])) return {};
    std::size_t n = k + 1;
    while (n < line.size() && dss::test::isWordChar(line[n])) ++n;
    std::size_t p = skipSpace(line, n);
    if (p < line.size() && line[p] == '[') {
        std::size_t const close = line.find(']', p + 1);
        if (close == std::string::npos) return {};
        p = skipSpace(line, close + 1);
    }
    std::size_t const semi = line.find(';', p);
    bool ends = p < line.size() && line[p] == ';';
    if (!ends && p < line.size() && line[p] == '=') ends = semi != std::string::npos;
    if (!ends && p < line.size() && line[p] == '{' && semi != std::string::npos)
        ends = line[semi - 1] == '}' && semi - 1 > p;   // a `}` right before the first `;`
    return ends ? line.substr(k, n - k) : std::string{};
}

// A data member's (type, name) -- `regex_search` of
// `^\s*((?:const\s+)?[A-Za-z_][\w:<>,\s*&]*?[\w>*&])\s+([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:\{[^;]*\}|=[^;]*)?;`:
// the type is the SHORTEST that the rest of the line completes, with `const ` taken first.
[[nodiscard]] std::optional<Field> memberOf(std::string const& line) {
    using dss::test::isSpace;
    using dss::test::isWordChar;
    std::size_t const s = dss::test::skipSpace(line, 0);
    auto const middle = [](char c) {
        return isWordChar(c) || c == ':' || c == '<' || c == '>' || c == ',' || isSpace(c) || c == '*' || c == '&';
    };
    auto const last = [](char c) { return isWordChar(c) || c == '>' || c == '*' || c == '&'; };
    // The first end `e` (exclusive) of a type starting at `x`: x is `[A-Za-z_]`, every character
    // after it is `middle`, the last is `last`, and white space follows.
    auto const shortest = [&](std::size_t x) -> std::optional<Field> {
        if (x >= line.size() || !dss::test::isIdentStart(line[x])) return std::nullopt;
        for (std::size_t e = x + 2; e < line.size(); ++e) {
            if (!middle(line[e - 1])) return std::nullopt;   // the type cannot run past it
            if (!last(line[e - 1]) || !isSpace(line[e])) continue;
            if (std::string name = memberNameFrom(line, e); !name.empty()) return Field{line.substr(s, e - s), name};
        }
        return std::nullopt;
    };
    if (line.compare(s, 5, "const") == 0 && s + 5 < line.size() && isSpace(line[s + 5])) {
        if (auto f = shortest(dss::test::skipSpace(line, s + 5))) return f;
    }
    return shortest(s);
}

// Every struct / class body of every header under src/: its data members, as the walk reads them.
[[nodiscard]] std::map<std::string, std::vector<Field>> parsedStructs(fs::path const& root) {
    std::map<std::string, std::vector<Field>> out;
    auto const walk = dss::core::walkTree(root / "src", [&](dss::core::TreeEntry const& entry) {
        if (entry.kind != dss::core::EntryKind::File || !entry.relative.generic().ends_with(".hpp"))
            return dss::core::WalkStep::Continue;
        std::string const code = dss::test::codeOnly(readAll(entry.at));
        std::istringstream lines{code};
        std::vector<std::pair<std::string, int>> stack;   // (struct, depth inside it)
        int depth = 0;
        for (std::string line; std::getline(lines, line);) {
            if (std::string const head = structHeadName(line); !head.empty()) {
                stack.emplace_back(head, depth + 1);
                out.try_emplace(head);
            } else if (!stack.empty() && depth == stack.back().second && line.find('(') == std::string::npos) {
                if (std::optional<Field> const f = memberOf(line)) {
                    std::string const& type = f->type;
                    bool const declaresNoMember = type.starts_with("return") || type.starts_with("using")
                                               || type.starts_with("typedef") || type.starts_with("static")
                                               || type.starts_with("friend");
                    if (!declaresNoMember) out[stack.back().first].push_back(*f);
                }
            }
            depth += static_cast<int>(std::count(line.begin(), line.end(), '{'))
                   - static_cast<int>(std::count(line.begin(), line.end(), '}'));
            while (!stack.empty() && depth < stack.back().second) stack.pop_back();
        }
        return dss::core::WalkStep::Continue;
    });
    EXPECT_FALSE(walk.error) << "the walk of src/ stopped at " << walk.where.spelling().generic();
    return out;
}

// Every identifier of a type's spelling (`[A-Za-z_]\w*` scanned left to right, a match inside a digit-led run
// included, as the regex iterator finds it).
[[nodiscard]] std::vector<std::string> wordsOf(std::string const& type) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < type.size();) {
        if (!dss::test::isIdentStart(type[i])) {
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        while (j < type.size() && dss::test::isWordChar(type[j])) ++j;
        out.push_back(type.substr(i, j - i));
        i = j;
    }
    return out;
}

// Every path from `root` to a member whose type names `SymbolId`, by member names. An explicit stack; a struct is
// re-entered on every path, never within its own chain.
[[nodiscard]] std::set<std::string> holdersFrom(std::map<std::string, std::vector<Field>> const& structs,
                                                std::string const& root) {
    std::set<std::string> out;
    std::vector<std::tuple<std::string, std::string, std::set<std::string>>> work{{root, "", {root}}};
    while (!work.empty()) {
        auto [name, path, chain] = work.back();
        work.pop_back();
        auto const it = structs.find(name);
        if (it == structs.end()) continue;
        for (Field const& f : it->second) {
            std::string const here = path.empty() ? f.name : path + "." + f.name;
            // A holder's type NAMES `SymbolId` as a word -- never a longer name that begins with it:
            // `SymbolIdEnd`, the end a module carries, is the space's counter, not an id it holds.
            std::vector<std::string> const words = wordsOf(f.type);
            if (std::ranges::find(words, "SymbolId") != words.end()) out.insert(here);
            for (std::string const& t : words) {
                if (t != "SymbolId" && structs.contains(t) && !chain.contains(t)) {
                    auto next = chain;
                    next.insert(t);
                    work.emplace_back(t, here, std::move(next));
                }
            }
        }
    }
    return out;
}

} // namespace

// THE TYPES AND THE TABLE AGREE, both directions.
TEST(ModuleSymbolIdHolders, TheScansTableIsEveryHolderTheTypesDeclare) {
    auto const structs = parsedStructs(dss::test::repoRoot());
    ASSERT_TRUE(structs.contains("AssembledModule")) << "the walk found no AssembledModule: the parse is blind";
    std::set<std::string> const derived = holdersFrom(structs, "AssembledModule");
    std::set<std::string> table;
    for (auto const& row : dss::linker::kModuleSymbolIdHolders) {
        EXPECT_TRUE(table.insert(std::string{row.path}).second) << row.path << " is a row twice";
    }
    for (std::string const& p : derived)
        EXPECT_TRUE(table.contains(p)) << p << " holds a SymbolId, and the scan has no row for it";
    for (std::string const& p : table)
        EXPECT_TRUE(derived.contains(p)) << p << " is a row of the scan, and no member of that path holds a SymbolId";
    EXPECT_GE(derived.size(), 19u) << "the walk found fewer holders than it did at its birth (19, 2026-11-14)";
}

// THE WALK ITSELF, on synthetic headers: a holder directly, through a vector, an optional and a map key, a struct
// reached twice by two paths, a recursive struct walked once per chain, and the members it must not read.
TEST(ModuleSymbolIdHolders, TheWalkFindsEveryHolderAndNothingElse) {
    std::map<std::string, std::vector<Field>> const structs = {
        {"Root", {{"SymbolId", "own"}, {"std::vector<Inner>", "inners"}, {"std::optional<Inner>", "maybe"},
                  {"std::unordered_map<SymbolId, std::uint32_t>", "positions"}, {"std::uint32_t", "count"},
                  {"Node", "tree"}, {"SymbolIdEnd", "end"}}},   // a counter whose name begins with the word
        {"Inner", {{"SymbolId", "target"}, {"std::uint32_t", "offset"}}},
        {"Node", {{"std::vector<Node>", "kids"}, {"SymbolId", "label"}}},
    };
    std::set<std::string> const expected = {"own",          "inners.target", "maybe.target", "positions",
                                            "tree.label"};
    EXPECT_EQ(holdersFrom(structs, "Root"), expected);
}
