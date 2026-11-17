// A FRESH `SymbolId` IS MADE IN ONE FILE -- the repo guard of review-csf26c2 N9 (F10's class), P69 lane `cs`, fold
// f26d.
//
// ★ THE CLASS. A `SymbolId` the program MINTS is unique in its id space only if every space hands out its next id
// through ONE door. `SymbolId` (`DSS_STRONG_ID`) admits a new value only through its explicit `std::uint32_t`
// constructor and a write of its public `v`, and the constructor must stay public (nearly every construction is a
// copy, a parse or a mapping). So a bare mint -- `SymbolId{++maxV}` at one more link site -- compiles everywhere,
// and before this guard only a review could refuse it. 28 minting lines across nine id spaces each computed their own
// next id (`+ 1` seeds that wrap onto 0, a caller's contract against two cursors, exhaustion checked at three of them).
// They now ask src/core/types/symbol_id_cursor.hpp -- the counting cursor (`SymbolIdEnd`) and the index rule
// (`symbolIdOfIndex`) -- and this guard makes every other spelling RED.
//
// THE FORMS REFUSED (each derived from the type: the constructor's argument, or a write of `v`), naming the door:
//   (a) a `SymbolId` construction -- `SymbolId{...}`, `SymbolId(...)`, `SymbolId [const] name{...}` / `name(...)`
//       -- whose argument is ARITHMETIC: it holds any `+` or any `-` but a member arrow's (`++`, `--`, `+=`, `-=`,
//       a binary one, a unary one: `x + -y`, `+x`, `x + *p`, `-1`), or a `size()` / `ssize()` call;
//   (b) the same construction whose argument is a plain name (through casts and parentheses; a member access by its
//       last name) that the construction's own FUNCTION BODY advances: `x++`, `++x`, `x += ...`, `x = ... + ...` --
//       with no name excluded, so a loop index read as an id is refused too (an enumeration of an index space asks
//       the table's reader). Outside every body, the whole file's advances count;
//   (c) a construction IN PLACE -- `emplace`, `emplace_back`, `emplace_front`, `emplace_hint`, `try_emplace`, the
//       direct initializations that reach the explicit constructor from a raw value -- into a receiver whose declared
//       type names `SymbolId` inside template brackets and that THIS SOURCE SEES (a local in its body, a parameter in
//       its function, a member or a namespace-scope name in its own file and in every file that includes that file),
//       whose argument has form (a) or (b) at a position that type constructs a `SymbolId` from: a map's key and
//       mapped value (`emplace` constructs both, `try_emplace` only the mapped one), a `pair` or `tuple` element's own
//       parts, any other container's one element (after a sequence's position). `push_back`, `insert` and
//       `insert_or_assign` take a value that already IS a SymbolId -- copy initialization refuses the explicit
//       constructor -- so whatever they hold was made by a construction (a) or (b) judges where it is written;
//   (d) an arithmetic write of a strong id's `v` (`.v++`, `++x.v`, `.v += ...`, `.v = <form (a)>`) -- of ANY strong
//       id, since text cannot tell `SymbolId`'s `v` from another's.
// A function's body, as text finds it: an outermost brace block that only namespace, class and linkage blocks
// enclose (a lambda's body is its function's); a source whose braces do not balance is read as ONE scope, toward RED.
// ONE HOME, NO EXEMPTION LIST: src/core/types/symbol_id_cursor.hpp, the file every door asks.
//
// WHAT TEXT CANNOT SEE (stated, with the searches that cover it today, MEASURED over src/ -- the N9 home audit's
// instrument, 2026-11-14, and the last search 2026-11-16): a counter carried ACROSS files through a raw-integer
// holder (HIR node payloads, MIR instruction payloads, an asm template's symbol) and wrapped in another file -- a
// builder call with a `++` argument: 1 (a virtual register), a payload assigned a counter: 0; a container that
// constructs through the explicit constructor from such a raw value in another file --
// 43 emplaces from a counter or a size, none of them a SymbolId's but the HIR text's handles, which are an index
// door's; and a counter advanced in ANOTHER function of the file than the one that wraps it (a member stepped by one
// method and read by another) -- every plain-name construction whose file advances the name but whose own body does
// not: 7, each a copy (a structured binding's key x2, a range-for element x3, a parameter, a value read from text).
// Comments and string literals are not code.
//
// SCOPE: every *.cpp and *.hpp under src/; a source's includes are read from its `#include "..."` lines, resolved
// against its own directory and src/.

#include "core/substrate/path_identity.hpp"   // walkTree
#include "repo_root.hpp"
#include "source_code_only.hpp"               // codeOnly
#include "text_scan.hpp"                      // the linear scanners

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using dss::test::boundaryAt;
using dss::test::forEachIdentifier;
using dss::test::holdsWord;
using dss::test::identEnd;
using dss::test::isIdentStart;
using dss::test::isSpace;
using dss::test::isWordChar;
using dss::test::skipSpace;
using dss::test::topLevelArguments;
using dss::test::wordAt;

constexpr char const* kHome = "src/core/types/symbol_id_cursor.hpp";

struct Source {
    std::string              relative;
    std::string              code;       // comments and literals blanked, new-lines kept
    std::vector<std::string> includes;   // each `#include "..."` as spelled
};

struct Finding {
    std::string where;   // <file>:<line>
    std::string form;    // a, b, c or d
    std::string text;
};

[[nodiscard]] std::size_t lineAt(std::string const& code, std::size_t offset) {
    auto const end = code.begin() + static_cast<std::ptrdiff_t>(offset);
    return 1u + static_cast<std::size_t>(std::count(code.begin(), end, '\n'));
}

// The text between the bracket at `open` and its match (exclusive), or npos-length on an unbalanced one.
[[nodiscard]] std::string balancedFrom(std::string const& code, std::size_t open) {
    char const o = code[open];
    char const c = o == '{' ? '}' : ')';
    int depth = 0;
    for (std::size_t k = open; k < code.size(); ++k) {
        if (code[k] == o) ++depth;
        else if (code[k] == c && --depth == 0) return code.substr(open + 1, k - open - 1);
    }
    return code.substr(open + 1);
}

// ── THE SCANNERS: linear passes over the code (text_scan.hpp's primitives), no `std::regex` ──────────────

// (a)'s test: any `+`, any `-` that does not begin a member arrow `->`, or a `size()` / `ssize()` call -- a superset
// of every binary and unary spelling, so the guard refuses whatever the retired tier guard (`SynthSymbolFloor`'s:
// any `+` inside the braces) refused.
[[nodiscard]] bool isArithmetic(std::string const& arg) {
    std::size_t const n = arg.size();
    for (std::size_t p = 0; p < n; ++p) {
        if (arg[p] == '+') return true;                                  // + ++ += and a unary plus
        if (arg[p] == '-' && (p + 1 >= n || arg[p + 1] != '>')) return true;   // - -- -= -1, never ->
    }
    for (std::size_t q = arg.find("size"); q != std::string::npos; q = arg.find("size", q + 1)) {
        bool const startHere  = q == 0 || !isWordChar(arg[q - 1]);
        bool const startAtS   = q > 0 && arg[q - 1] == 's' && (q == 1 || !isWordChar(arg[q - 2]));
        if (!startHere && !startAtS) continue;
        std::size_t const after = skipSpace(arg, q + 4);
        if (after < n && arg[after] == '(') return true;
    }
    return false;
}

// The plain name `arg` reduces to through casts and parentheses (a member access by its last name), or "".
// The casts: the language of `\b(?:static_cast|std::uint32_t|uint32_t)\s*(?:<[^<>]*>)?\s*\(`, each replaced by `(`;
// the name: `^(?:[A-Za-z_]\w*(?:\.|->))*([A-Za-z_]\w*)$` over what is left without parentheses and white space.
[[nodiscard]] std::string plainNameOf(std::string const& arg) {
    std::string stripped;
    std::size_t i = 0;
    while (i < arg.size()) {
        bool replaced = false;
        if (boundaryAt(arg, i) || i == 0) {
            for (std::string_view const w : {std::string_view{"static_cast"}, std::string_view{"std::uint32_t"},
                                             std::string_view{"uint32_t"}}) {
                if (arg.compare(i, w.size(), w) != 0 || (i > 0 && isWordChar(arg[i - 1]))) continue;
                std::size_t j = skipSpace(arg, i + w.size());
                if (j < arg.size() && arg[j] == '<') {
                    std::size_t k = j + 1;
                    while (k < arg.size() && arg[k] != '<' && arg[k] != '>') ++k;
                    if (k < arg.size() && arg[k] == '>') j = skipSpace(arg, k + 1);
                }
                if (j < arg.size() && arg[j] == '(') {
                    stripped += '(';
                    i = j + 1;
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) stripped += arg[i++];
    }
    std::string squeezed;
    for (char const ch : stripped)
        if (ch != '(' && ch != ')' && !isSpace(ch)) squeezed += ch;
    std::size_t k = 0;
    std::string last;
    while (true) {
        std::size_t const e = identEnd(squeezed, k);
        if (e == k) return {};
        last = squeezed.substr(k, e - k);
        if (e == squeezed.size()) return last;
        if (squeezed[e] == '.') k = e + 1;
        else if (squeezed.compare(e, 2, "->") == 0) k = e + 2;
        else return {};
    }
}

// Every name a source advances: `x++`, `++x`, `x += ...`, `x -= ...`, `x = ... + ...` -- the languages of
// `\b(ident)\s*(?:\+\+|--)`, `(?:\+\+|--)\s*(ident)\b`, `\b(ident)\s*[+\-]=` and `\b(ident)\s*=(?!=)\s*([^;=]*)`
// (the last scanned left to right, resuming after each match, as a regex iterator does).
[[nodiscard]] std::set<std::string> advancedNames(std::string const& code) {
    std::set<std::string> out;
    std::size_t const n = code.size();
    forEachIdentifier(code, [&](std::size_t b, std::size_t e) -> std::size_t {
        std::size_t const a = skipSpace(code, e);
        if (a + 1 < n && (code[a] == '+' || code[a] == '-') && (code[a + 1] == code[a] || code[a + 1] == '='))
            out.insert(code.substr(b, e - b));   // x++ x-- x+= x-=
        return e;
    });
    for (std::size_t p = 0; p + 1 < n; ++p) {
        if (!((code[p] == '+' && code[p + 1] == '+') || (code[p] == '-' && code[p + 1] == '-'))) continue;
        std::size_t const a = skipSpace(code, p + 2);
        std::size_t const e = identEnd(code, a);
        if (e > a) {   // ++x --x: resume after the name, as the iterator does; on no match, the next position
            out.insert(code.substr(a, e - a));
            p = e - 1;
        }
    }
    forEachIdentifier(code, [&](std::size_t b, std::size_t e) -> std::size_t {
        std::size_t const a = skipSpace(code, e);
        if (a >= n || code[a] != '=' || (a + 1 < n && code[a + 1] == '=')) return e;
        std::size_t r = a + 1;
        while (r < n && code[r] != ';' && code[r] != '=') ++r;
        if (isArithmetic(code.substr(a + 1, r - (a + 1)))) out.insert(code.substr(b, e - b));
        return r;   // resume after the match, as the iterator does
    });
    return out;
}

// ── SCOPE: a function's body, as text can find it ──────────────────────────────────────────────────────────────
// An outermost CODE block: a brace block that only namespace, type and linkage blocks enclose and that is not one
// of them itself -- a function's body with every lambda and block inside it, or a namespace-scope initializer. A
// block's kind is read from its head, the text back to the nearest `;`, `{` or `}`: a head naming `namespace` opens
// a namespace; a head with no parenthesis that names `class`, `struct`, `union` or `enum`, or begins with `extern`,
// opens a type or a linkage block; every other head opens code. A source whose braces do not balance has no bodies,
// and its whole text is then the one scope (toward RED).
struct Body {
    std::size_t open;    // the `{`
    std::size_t close;   // its `}`
};

struct Bodies {
    std::vector<Body> spans;   // in order, disjoint
    bool              balanced = false;

    // The body holding `offset`, or nullptr outside every one (and always, for a source that does not balance).
    [[nodiscard]] Body const* holding(std::size_t offset) const {
        if (!balanced) return nullptr;
        auto const it = std::upper_bound(spans.begin(), spans.end(), offset,
                                         [](std::size_t o, Body const& b) { return o < b.open; });
        if (it == spans.begin()) return nullptr;
        Body const& b = *(it - 1);
        return offset <= b.close ? &b : nullptr;
    }
};

[[nodiscard]] Bodies codeBodies(std::string const& c) {
    Bodies            out;
    std::vector<bool> containers;   // per open block: a namespace, type or linkage block?
    std::size_t       codeDepth = 0;
    std::size_t       bodyOpen  = 0;
    for (std::size_t p = 0; p < c.size(); ++p) {
        if (c[p] == '{') {
            if (codeDepth > 0) {
                containers.push_back(false);
                ++codeDepth;
                continue;
            }
            std::size_t h = p;
            while (h > 0 && c[h - 1] != ';' && c[h - 1] != '{' && c[h - 1] != '}') --h;
            std::string const head   = c.substr(h, p - h);
            bool const        parens = head.find_first_of("()") != std::string::npos;
            bool const container     = holdsWord(head, "namespace")
                                || (!parens
                                    && (holdsWord(head, "class") || holdsWord(head, "struct")
                                        || holdsWord(head, "union") || holdsWord(head, "enum")
                                        || wordAt(head, skipSpace(head, 0), "extern")));
            containers.push_back(container);
            if (!container) {
                codeDepth = 1;
                bodyOpen  = p;
            }
        } else if (c[p] == '}') {
            if (containers.empty()) return {};
            bool const container = containers.back();
            containers.pop_back();
            if (!container && --codeDepth == 0) out.spans.push_back({bodyOpen, p});
        }
    }
    if (!containers.empty()) return {};
    out.balanced = true;
    return out;
}

// ── THE CONTAINERS: every name declared with a type that names `SymbolId` inside template brackets ─────────────
// The declaration `<…SymbolId…> [const] [&] name` ended by `;`, `=`, `{` or `(` (a variable or a member), or by `,`
// or `)` (a parameter), within one run of the code free of `;`, `{`, `}`, `(` and `)`; the last `>` of the run is
// tried first. Its TYPE is kept: the template the `>` closes (`vector`, `unordered_map`, …) and that template's
// arguments, so an insertion is judged at the argument that constructs a SymbolId and no other.
struct Container {
    std::string              name;
    std::string              templ;       // the template's own name
    std::vector<std::string> arguments;   // its template arguments, top level
    std::size_t              at = 0;      // the declared name's offset
    bool                     parameter = false;
};

// The template a `>` at `close` ends: its own name and its arguments (empty when no `<` matches in [from, close)).
[[nodiscard]] std::pair<std::string, std::vector<std::string>> templateClosedAt(std::string const& c,
                                                                               std::size_t from, std::size_t close) {
    int depth = 0;
    for (std::size_t k = close + 1; k-- > from;) {
        if (c[k] == '>') ++depth;
        else if (c[k] == '<' && --depth == 0) {
            std::size_t e = k;
            while (e > from && isSpace(c[e - 1])) --e;
            std::size_t b = e;
            while (b > from && isWordChar(c[b - 1])) --b;
            return {c.substr(b, e - b), topLevelArguments(c.substr(k + 1, close - k - 1))};
        }
    }
    return {};
}

[[nodiscard]] std::vector<Container> containersDeclaredIn(std::string const& c) {
    std::vector<Container> out;
    auto const isStop = [](char ch) { return ch == ';' || ch == '{' || ch == '}' || ch == '(' || ch == ')'; };
    for (std::size_t p = c.find("SymbolId"); p != std::string::npos; p = c.find("SymbolId", p + 1)) {
        if (!wordAt(c, p, "SymbolId")) continue;
        std::size_t left   = p;
        bool        opened = false;
        while (left > 0 && !isStop(c[left - 1])) {
            --left;
            opened = opened || c[left] == '<';
        }
        if (!opened) continue;
        std::size_t right = p + 8;
        while (right < c.size() && !isStop(c[right])) ++right;
        for (std::size_t g = right; g-- > p + 8;) {
            if (c[g] != '>') continue;
            std::size_t nameAt = std::string::npos;
            bool        parameter = false;
            auto const  tail = [&](bool withConst) -> std::string {
                std::size_t k = skipSpace(c, g + 1);
                if (withConst) {
                    if (c.compare(k, 5, "const") != 0) return {};
                    k = skipSpace(c, k + 5);
                }
                if (k < c.size() && c[k] == '&') ++k;
                k = skipSpace(c, k);
                if (k >= c.size() || !isIdentStart(c[k])) return {};
                std::size_t e = k + 1;
                while (e < c.size() && isWordChar(c[e])) ++e;
                std::size_t const t = skipSpace(c, e);
                if (t >= c.size()) return {};
                if (c[t] == ';' || c[t] == '=' || c[t] == '{' || c[t] == '(' || c[t] == ',' || c[t] == ')') {
                    nameAt    = k;
                    parameter = c[t] == ',' || c[t] == ')';
                    return c.substr(k, e - k);
                }
                return {};
            };
            std::string name = tail(true);
            if (name.empty()) name = tail(false);
            if (name.empty()) continue;
            auto [templ, arguments] = templateClosedAt(c, left, g);
            out.push_back({std::move(name), std::move(templ), std::move(arguments), nameAt, parameter});
            break;
        }
    }
    return out;
}

// Where a container is seen: a parameter in the body its parameter list opens (none for a bare declaration), a
// name declared in a body in that body, any other (a member, a namespace-scope name) in its whole file -- and in
// every file that includes that file (`fileScope`).
[[nodiscard]] bool visibleAt(Container const& k, std::string const& c, Bodies const& bodies, std::size_t offset) {
    if (!bodies.balanced) return true;
    if (k.parameter) {
        auto const it = std::upper_bound(bodies.spans.begin(), bodies.spans.end(), k.at,
                                         [](std::size_t o, Body const& b) { return o < b.open; });
        if (it == bodies.spans.end()) return false;
        std::size_t const semi = c.find(';', k.at);
        if (semi != std::string::npos && semi < it->open) return false;   // a declaration: no body follows
        return it->open <= offset && offset <= it->close;
    }
    Body const* const declaredIn = bodies.holding(k.at);
    if (declaredIn == nullptr) return true;
    return declaredIn->open <= offset && offset <= declaredIn->close;
}

[[nodiscard]] bool fileScope(Container const& k, Bodies const& bodies) {
    return !k.parameter && bodies.holding(k.at) == nullptr;
}

// ── WHICH ARGUMENTS OF AN INSERTION CONSTRUCT A SymbolId, by the container's declared type ─────────────────────
// Only the emplace family constructs in place, which is DIRECT initialization and so reaches the explicit
// constructor from a raw value: `push_back`, `insert` and `insert_or_assign` take a value that already IS one (copy
// initialization refuses the explicit constructor), so any SymbolId they hold was made by a construction (a) or (b)
// judges where it is written. In a map, the key is the first template argument and the mapped value the second:
// `emplace` constructs both, `try_emplace` only the mapped value (its key is taken as a key, never constructed).
// A sequence's `emplace` takes its position first. An element that is a `pair` or a `tuple` is constructed argument
// by argument. Every other container constructs its one element from all the arguments.
[[nodiscard]] bool namesSymbolId(std::string const& type) { return holdsWord(type, "SymbolId"); }

[[nodiscard]] bool isMapTemplate(std::string const& templ) { return templ.find("map") != std::string::npos; }

[[nodiscard]] bool isSequenceTemplate(std::string const& templ) {
    return templ == "vector" || templ == "deque" || templ == "list" || templ.ends_with("_vector");
}

// For each call argument, whether it constructs a SymbolId.
[[nodiscard]] std::vector<bool> constructingArguments(Container const& k, std::string_view method, std::size_t argc) {
    std::vector<bool> out(argc, false);
    auto const arg = [&](std::size_t i) -> std::string const& {
        static std::string const none;
        return i < k.arguments.size() ? k.arguments[i] : none;
    };
    // An element type built argument by argument (`pair`, `tuple`), or from all of them.
    auto const element = [&](std::string const& type, std::size_t first) {
        std::string const squeezed = type;
        std::size_t const lt       = squeezed.find('<');
        std::size_t       nameEnd  = lt == std::string::npos ? squeezed.size() : lt;
        while (nameEnd > 0 && isSpace(squeezed[nameEnd - 1])) --nameEnd;
        std::size_t nameBegin = nameEnd;
        while (nameBegin > 0 && isWordChar(squeezed[nameBegin - 1])) --nameBegin;
        std::string const own = squeezed.substr(nameBegin, nameEnd - nameBegin);
        std::size_t const gt  = squeezed.rfind('>');
        if ((own == "pair" || own == "tuple") && lt != std::string::npos && gt != std::string::npos && gt > lt) {
            auto const parts = topLevelArguments(squeezed.substr(lt + 1, gt - lt - 1));
            for (std::size_t i = first; i < argc; ++i)
                out[i] = (i - first < parts.size() && namesSymbolId(parts[i - first])) || i - first >= parts.size();
            return;
        }
        if (namesSymbolId(type))
            for (std::size_t i = first; i < argc; ++i) out[i] = true;
    };
    if (method == "try_emplace") {
        if (isMapTemplate(k.templ)) element(arg(1), 1);
        return out;
    }
    if (method == "emplace" || method == "emplace_hint") {
        std::size_t const first = method == "emplace_hint" ? 1u : 0u;
        if (isMapTemplate(k.templ)) {
            if (first < argc) out[first] = namesSymbolId(arg(0));
            for (std::size_t i = first + 1; i < argc; ++i) out[i] = namesSymbolId(arg(1));
            return out;
        }
        element(arg(0), isSequenceTemplate(k.templ) ? first + 1 : first);
        return out;
    }
    element(arg(0), 0);   // emplace_back, emplace_front
    return out;
}

// The bracket a `SymbolId` construction opens at `p` (the word), or npos -- the language of
// `\bSymbolId\s*(?:const\s+)?(?:[A-Za-z_]\w*\s*)?([\{\(])`, its alternatives tried in the regex's order.
[[nodiscard]] std::size_t constructionBracket(std::string const& c, std::size_t p) {
    std::size_t const base = skipSpace(c, p + 8);
    auto const bracketAt = [&](std::size_t k) { return k < c.size() && (c[k] == '{' || c[k] == '('); };
    auto const afterIdent = [&](std::size_t k) -> std::size_t {   // `[A-Za-z_]\w*\s*` at k, or npos
        if (k >= c.size() || !isIdentStart(c[k])) return std::string::npos;
        std::size_t e = k + 1;
        while (e < c.size() && isWordChar(c[e])) ++e;
        return skipSpace(c, e);
    };
    if (c.compare(base, 5, "const") == 0 && base + 5 < c.size() && isSpace(c[base + 5])) {
        std::size_t const k = skipSpace(c, base + 5);
        if (std::size_t const t = afterIdent(k); t != std::string::npos && bracketAt(t)) return t;
        if (bracketAt(k)) return k;
    }
    if (std::size_t const t = afterIdent(base); t != std::string::npos && bracketAt(t)) return t;
    if (bracketAt(base)) return base;
    return std::string::npos;
}

// Every refused form in one source. `included` holds the file-scope containers of every source it includes;
// `judged`, when given, counts the in-place insertions (c) judged -- into a container seen here, at an argument
// that constructs a SymbolId.
[[nodiscard]] std::vector<Finding> refusedIn(Source const& s, std::vector<Container> const& included,
                                            std::size_t* judged = nullptr) {
    std::vector<Finding> found;
    std::string const& c = s.code;
    std::size_t const n = c.size();
    Bodies const bodies = codeBodies(c);
    // (b)'s scope: the names the construction's own BODY advances; outside every body, the file's.
    std::set<std::string> const        fileWide = advancedNames(c);
    std::vector<std::set<std::string>> perBody(bodies.spans.size());
    std::vector<bool>                  read(bodies.spans.size(), false);
    auto const advancedAt = [&](std::size_t offset) -> std::set<std::string> const& {
        Body const* const b = bodies.holding(offset);
        if (b == nullptr) return fileWide;
        std::size_t const k = static_cast<std::size_t>(b - bodies.spans.data());
        if (!read[k]) {
            perBody[k] = advancedNames(c.substr(b->open, b->close + 1 - b->open));
            read[k]    = true;
        }
        return perBody[k];
    };
    auto const judge = [&](std::string const& arg, std::size_t offset) -> char const* {
        if (isArithmetic(arg)) return "a";
        std::string const name = plainNameOf(arg);
        return !name.empty() && advancedAt(offset).contains(name) ? "b" : nullptr;
    };
    auto const at = [&](std::size_t offset) { return s.relative + ":" + std::to_string(lineAt(c, offset)); };
    // (a) and (b): a construction. `SymbolId` with NO boundary after it, as the language states: a word that
    // continues (`SymbolIdEnd{...}`) reads its rest as the declared name's place, so a cursor seeded by arithmetic
    // (`SymbolIdEnd{n + 1}`) is judged too.
    for (std::size_t p = c.find("SymbolId"); p != std::string::npos; p = c.find("SymbolId", p + 1)) {
        if (p > 0 && isWordChar(c[p - 1])) continue;
        std::size_t const open = constructionBracket(c, p);
        if (open == std::string::npos) continue;
        std::string const arg = balancedFrom(c, open);
        if (char const* form = judge(arg, open)) found.push_back({at(open), form, "SymbolId{" + arg + "}"});
    }
    // (c): a construction in place into a SymbolId container seen here --
    // `\b(ident)\s*(?:\.|->)\s*(?:emplace|emplace_back|emplace_front|emplace_hint|try_emplace)\s*(\()`.
    std::vector<Container> const own = containersDeclaredIn(c);
    static std::set<std::string, std::less<>> const kInPlace{"emplace", "emplace_back", "emplace_front",
                                                             "emplace_hint", "try_emplace"};
    forEachIdentifier(c, [&](std::size_t b, std::size_t e) -> std::size_t {
        std::size_t k = skipSpace(c, e);
        if (k < n && c[k] == '.') k += 1;
        else if (c.compare(k, 2, "->") == 0) k += 2;
        else return e;
        k = skipSpace(c, k);
        std::size_t const me = identEnd(c, k);
        if (me == k || !kInPlace.contains(std::string_view{c}.substr(k, me - k))) return e;
        std::size_t const open = skipSpace(c, me);
        if (open >= n || c[open] != '(') return e;
        std::string const receiver = c.substr(b, e - b);
        std::string const method   = c.substr(k, me - k);
        std::vector<std::string> const args = topLevelArguments(balancedFrom(c, open));
        auto const refuses = [&](Container const& con) {
            if (con.name != receiver) return false;
            std::vector<bool> const constructs = constructingArguments(con, method, args.size());
            if (judged != nullptr && std::find(constructs.begin(), constructs.end(), true) != constructs.end())
                ++*judged;
            for (std::size_t i = 0; i < args.size(); ++i)
                if (constructs[i] && judge(args[i], open) != nullptr) {
                    found.push_back({at(open), "c", receiver + " <- " + args[i]});
                    return true;
                }
            return false;
        };
        bool done = false;
        for (Container const& con : own)
            if (!done && visibleAt(con, c, bodies, open)) done = refuses(con);
        for (Container const& con : included)
            if (!done) done = refuses(con);
        return open + 1;   // resume after the match
    });
    // (d): an arithmetic step of a `v` -- `\.v\s*(?:\+\+|--|[+\-]=)` or
    // `(?:\+\+|--)\s*[A-Za-z_]\w*(?:(?:\.|->)\w+)*\.v\b` (the longest chain that ends in `.v`), left to right.
    for (std::size_t p = 0; p < n; ++p) {
        if (c[p] == '.' && p + 1 < n && c[p + 1] == 'v') {
            std::size_t const a = skipSpace(c, p + 2);
            if (a + 1 < n && (c[a] == '+' || c[a] == '-') && (c[a + 1] == c[a] || c[a + 1] == '=')) {
                found.push_back({at(p), "d", c.substr(p, a + 2 - p)});
                p = a + 1;
                continue;
            }
        }
        if (p + 1 < n && ((c[p] == '+' && c[p + 1] == '+') || (c[p] == '-' && c[p + 1] == '-'))) {
            std::size_t const a = skipSpace(c, p + 2);
            std::size_t k = identEnd(c, a);
            if (k == a) continue;   // no match here: the next position is tried
            std::size_t endOfV = std::string::npos;
            while (true) {   // the chain `(?:(?:\.|->)\w+)*`, noting each `.v` whole word
                std::size_t m = k;
                bool dot = false;
                if (m < n && c[m] == '.') { m += 1; dot = true; }
                else if (c.compare(m, 2, "->") == 0) m += 2;
                else break;
                std::size_t e = m;
                while (e < n && isWordChar(c[e])) ++e;
                if (e == m) break;
                if (dot && e == m + 1 && c[m] == 'v') endOfV = e;
                k = e;
            }
            if (endOfV != std::string::npos) {
                found.push_back({at(p), "d", c.substr(p, endOfV - p)});
                p = endOfV - 1;
            }
        }
    }
    // (d): an arithmetic write of a `v` -- `\.v\s*=(?!=)\s*([^;]*);`.
    for (std::size_t p = c.find(".v"); p != std::string::npos; p = c.find(".v", p + 1)) {
        std::size_t const a = skipSpace(c, p + 2);
        if (a >= n || c[a] != '=' || (a + 1 < n && c[a + 1] == '=')) continue;
        std::size_t const semi = c.find(';', a + 1);
        if (semi == std::string::npos) continue;
        if (isArithmetic(c.substr(a + 1, semi - (a + 1)))) found.push_back({at(p), "d", c.substr(p, semi + 1 - p)});
        p = semi;
    }
    return found;
}

// ── WHAT A SOURCE INCLUDES ───────────────────────────────────────────────────────────────────────────────────
// Each `#include "…"` of the raw text (the code blanks the literal), as spelled.
[[nodiscard]] std::vector<std::string> includeSpellings(std::string const& text) {
    std::vector<std::string> out;
    std::size_t              line = 0;
    while (line < text.size()) {
        std::size_t const end = std::min(text.find('\n', line), text.size());
        std::size_t       k   = line;
        while (k < end && (text[k] == ' ' || text[k] == '\t')) ++k;
        if (k < end && text[k] == '#') {
            ++k;
            while (k < end && (text[k] == ' ' || text[k] == '\t')) ++k;
            if (text.compare(k, 7, "include") == 0) {
                k += 7;
                while (k < end && (text[k] == ' ' || text[k] == '\t')) ++k;
                if (k < end && text[k] == '"') {
                    std::size_t const close = text.find('"', k + 1);
                    if (close != std::string::npos && close < end) out.push_back(text.substr(k + 1, close - k - 1));
                }
            }
        }
        line = end + 1;
    }
    return out;
}

// `a/b/../c` as `a/c`; `.` segments dropped.
[[nodiscard]] std::string normalized(std::string const& path) {
    std::vector<std::string> parts;
    std::size_t              b = 0;
    while (b <= path.size()) {
        std::size_t const e = std::min(path.find('/', b), path.size());
        std::string const part = path.substr(b, e - b);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        b = e + 1;
    }
    std::string out;
    for (auto const& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

[[nodiscard]] bool isGuarded(std::string const& relative) {
    if (relative == kHome) return false;
    bool const code = relative.ends_with(".cpp") || relative.ends_with(".hpp");
    return code && relative.starts_with("src/");
}

// Every finding over a set of sources. Each guarded source is judged with the file-scope containers of every source
// it includes, to a fixed point -- each `#include "..."` resolved against the source's own directory, then src/.
// `judged`, when given, counts the in-place insertions (c) judged.
[[nodiscard]] std::vector<Finding> refusedAcross(std::vector<Source> const& sources, std::size_t* judged = nullptr) {
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < sources.size(); ++i) index.emplace(sources[i].relative, i);
    std::vector<std::vector<Container>>   fileScoped(sources.size());
    std::vector<std::vector<std::size_t>> includes(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        Source const& s      = sources[i];
        Bodies const  bodies = codeBodies(s.code);
        for (Container& k : containersDeclaredIn(s.code))
            if (fileScope(k, bodies)) fileScoped[i].push_back(std::move(k));
        std::string const dir = s.relative.substr(0, s.relative.rfind('/') + 1);
        for (std::string const& spelled : s.includes) {
            for (std::string const& candidate : {normalized(dir + spelled), normalized("src/" + spelled)}) {
                if (auto const it = index.find(candidate); it != index.end()) {
                    includes[i].push_back(it->second);
                    break;
                }
            }
        }
    }
    std::vector<Finding> found;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!isGuarded(sources[i].relative)) continue;
        std::vector<Container>   seen;
        std::vector<bool>        reached(sources.size(), false);
        std::vector<std::size_t> work = includes[i];
        reached[i]                    = true;
        while (!work.empty()) {
            std::size_t const j = work.back();
            work.pop_back();
            if (reached[j]) continue;
            reached[j] = true;
            seen.insert(seen.end(), fileScoped[j].begin(), fileScoped[j].end());
            for (std::size_t const k : includes[j])
                if (!reached[k]) work.push_back(k);
        }
        auto const here = refusedIn(sources[i], seen, judged);
        found.insert(found.end(), here.begin(), here.end());
    }
    return found;
}

[[nodiscard]] Source sourceOf(std::string relative, std::string const& text) {
    return Source{std::move(relative), dss::test::codeOnly(text), includeSpellings(text)};
}

// A source's bytes, read through the home: `p` is the walk's LISTED value, never its text re-resolved.
[[nodiscard]] std::string readAll(dss::core::HostPath const& p) {
    // A source the scan cannot open is no clean source: the scan stops, naming it and the OS's reason (H6c2 (d)).
    auto opened = dss::core::openInputStream(p, std::ios::binary);
    if (!opened)
        throw std::runtime_error("the scan could not open '" + p.spelling().generic() + "': "
                                 + dss::core::reasonText(opened.error()));
    return {std::istreambuf_iterator<char>{*opened}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string listed(std::vector<Finding> const& found) {
    std::string out;
    for (auto const& f : found) out += "\n  " + f.where + " (" + f.form + "): `" + f.text + "`";
    return out;
}

} // namespace

// THE TREE: no source under src/ but the cursor's file makes a fresh SymbolId.
TEST(SymbolIdMintsGuard, EveryFreshSymbolIdIsMadeByTheCursorsFile) {
    fs::path const      root = dss::test::repoRoot();
    std::vector<Source> sources;
    auto const walk = dss::core::walkTree(root / "src", [&](dss::core::TreeEntry const& entry) {
        if (entry.kind != dss::core::EntryKind::File) return dss::core::WalkStep::Continue;
        // Each entry a LISTED value: its names below src/ as listed, its bytes read by that reference.
        std::string const relative = "src/" + entry.relative.generic();
        if (relative.ends_with(".cpp") || relative.ends_with(".hpp"))
            sources.push_back(sourceOf(relative, readAll(entry.at)));
        return dss::core::WalkStep::Continue;
    });
    ASSERT_FALSE(walk.error) << "the walk of src/ stopped at " << walk.where.spelling().generic() << ": "
                             << walk.error.message();
    EXPECT_GE(sources.size(), 400u) << "the guard read only " << sources.size() << " sources";
    EXPECT_TRUE(std::any_of(sources.begin(), sources.end(), [](Source const& s) { return s.relative == kHome; }))
        << kHome << " was not read: the one home moved";
    std::size_t judged = 0;
    auto const  found  = refusedAcross(sources, &judged);
    EXPECT_TRUE(found.empty()) << found.size() << " fresh SymbolId(s) made outside " << kHome
                               << " -- take the space's door (SymbolIdEnd::take, symbolIdOfIndex):" << listed(found);
    // The in-place rule REACHES the tree (MEASURED 109 when it landed): a receiver it can no longer see, or a type it
    // no longer reads, would leave (c) judging nothing while the guard stayed green.
    EXPECT_GE(judged, 80u) << "(c) judged only " << judged << " in-place insertion(s) into a SymbolId container";
}

// THE SCAN ITSELF, on synthetic sources: every refused form once, the same words in a comment and a string, and the
// copies, parses and mappings it must accept -- among them the four places its scope and its types decide: a name
// another FUNCTION advances, a map's arithmetic VALUE, a `push_back` of an id, and a same-named container another
// file declares but this one does not see.
TEST(SymbolIdMintsGuard, TheScanRefusesEveryFormItNamesAndNothingElse) {
    std::vector<Source> const sources = {
        sourceOf("src/x/decl.hpp", "struct M { std::vector<SymbolId> held; std::unordered_map<SymbolId, int> at; };\n"),
        sourceOf("src/x/mint.cpp",
                 "#include \"x/decl.hpp\"\n"                                              // 1
                 "void f(M& m, std::uint32_t maxV, std::uint32_t n) {\n"                 // 2
                 "  SymbolId const a{++maxV};\n"                                         // 3  (a)
                 "  SymbolId b = SymbolId{maxV + 1};\n"                                  // 4  (a)
                 "  auto c = SymbolId(static_cast<std::uint32_t>(rows.size()));\n"       // 5  (a)
                 "  for (std::uint32_t i = 1; i < n; ++i) use(SymbolId{i});\n"           // 6  (b)
                 "  m.held.emplace_back(maxV++);\n"                                      // 7  (c), the included member
                 "  a.v += 1;\n"                                                         // 8  (d)
                 "  ++b.v;\n"                                                            // 9  (d)
                 "  c.v = maxV + 2;\n"                                                   // 10 (d)
                 "  m.at.emplace(n + 1, 0);\n"                                           // 11 (c), a map's key
                 "  std::unordered_map<int, SymbolId> back;\n"                           // 12
                 "  back.try_emplace(0, n - 1);\n"                                       // 13 (c), its mapped value
                 "}\n"                                                                   // 14
                 "void h(std::vector<SymbolId>& out, std::uint32_t k) { out.emplace_back(k + 1); }\n"     // 15 (c)
                 "void k(std::uint32_t x, std::uint32_t y, std::uint32_t const* p) {\n"                   // 16
                 "  use(SymbolId{x + -y}, SymbolId{+x}, SymbolId{x + *p}, SymbolId{x + ::kBase});\n"     // 17 (a) x4
                 "}\n"),
        sourceOf("src/x/copy.cpp",
                 "// SymbolId{++maxV} in a comment is no mint\n"                          // 1
                 "char const* s = \"SymbolId{maxV + 1}\";\n"                              // 2
                 "#include \"x/decl.hpp\"\n"                                              // 3
                 "void step(std::uint32_t& v) { ++v; }\n"                                // 4  another body's advance
                 "SymbolId g(M& m, std::uint32_t v, SymbolId s) {\n"                     // 5
                 "  SymbolId const copy{v};\n"                                           // 6
                 "  SymbolId const mapped{mergedIdFor(0, s)};\n"                         // 7
                 "  SymbolId const parsed{static_cast<std::uint32_t>(read)};\n"          // 8
                 "  SymbolId const tls{kTlsIndexReservedSymbolIdValue};\n"               // 9
                 "  ++preds[s.v];\n"                                                     // 10
                 "  bool const same = s.v == v;\n"                                       // 11
                 "  m.at.emplace(s, v + 1);\n"                                           // 12 the mapped int
                 "  m.held.push_back(ids[v + 1]);\n"                                     // 13 an id already
                 "  return SymbolId{};\n"                                                // 14
                 "}\n"),
        sourceOf("src/x/unrelated.cpp",
                 "void u(std::vector<int>& held, std::uint32_t n) { held.emplace_back(n + 1); }\n"   // 1 not decl's
                 "void w(std::uint32_t n) { std::vector<int> out; out.emplace_back(n + 1); }\n"),    // 2 not h's
    };
    auto const found = refusedAcross(sources);
    std::string got;
    for (auto const& f : found) {
        std::size_t const colon = f.where.find(':');
        got += " " + f.where.substr(colon + 1) + f.form + "@" + f.where.substr(0, colon);
    }
    EXPECT_EQ(got, " 3a@src/x/mint.cpp 4a@src/x/mint.cpp 5a@src/x/mint.cpp 6b@src/x/mint.cpp 17a@src/x/mint.cpp "
                   "17a@src/x/mint.cpp 17a@src/x/mint.cpp 17a@src/x/mint.cpp 7c@src/x/mint.cpp 11c@src/x/mint.cpp "
                   "13c@src/x/mint.cpp 15c@src/x/mint.cpp 8d@src/x/mint.cpp 9d@src/x/mint.cpp 10d@src/x/mint.cpp")
        << listed(found);
}

// THE ONE HOME: the cursor's file is the only source left out, and it does make the ids (the exemption is the home,
// not a hiding place: scanned, it is refused).
TEST(SymbolIdMintsGuard, TheCursorsFileIsTheOneSourceLeftOut) {
    EXPECT_FALSE(isGuarded(kHome));
    EXPECT_TRUE(isGuarded("src/core/types/strong_ids.hpp"));
    EXPECT_TRUE(isGuarded("src/link/linker.cpp"));
    EXPECT_TRUE(isGuarded("src/core/types/symbol_id_cursor_extra.hpp")) << "only the one file is left out";
    EXPECT_FALSE(isGuarded("tests/core/test_symbol_id_cursor.cpp")) << "tests build ids of their own";
    std::string const home = readAll(dss::test::repoRoot() / kHome);
    ASSERT_FALSE(home.empty());
    EXPECT_FALSE(refusedIn(sourceOf("src/the-home-scanned.hpp", home), {}).empty())
        << "the home makes no fresh id: the guard's one exemption has no reason";
}
