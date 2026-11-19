// P69 fold 3f (ledger 563 (A); main's condition on the open arch vocabulary): EVERY ARCH AND EVERY FORMAT
// A `when` SELECTOR NAMES IS ONE A SHIPPED DOCUMENT DECLARES.
//
// The `when` selector (core/types/variant_when.hpp) compares an arch as TEXT with the pair's target name,
// and its arch vocabulary is OPEN -- it lives only in the target documents -- so an arch no target
// declares matches no pair. That is right for the selector, and it makes a TYPO silent:
// `"arch": "aarch64"` (the ISA's name; the target's is `arm64`) selects nothing on any pair, and the arm
// it guards is dead config that no read refuses. This sweep is the loud half, over the whole shipped
// config tree: every `arch` a `when` object names, and every `arch` of a symbol row's availability entry
// (`{"format": ..., "arch": ...}`, the one selector decides it), is the `target.name` of some shipped
// target document; and every `format` either names is the `format.kind` of some shipped object-format
// document. (A format outside the kind vocabulary is already refused where the selector decodes it; the
// sweep adds the kind that is in the vocabulary and has no shipped document.) A plain string in an
// `availableObjectFormats` list is a format NAME, not a selector, and is not this sweep's.
//
// MEASURED P69 fold 3f (the lane's when_vocab_measure.py over the shipped tree, checkpoint 46 plus this
// fold): 86 JSON documents, 684 `when` objects and 1 availability arch entry; the arches named are arm64
// (21 `when`s and the 1 entry) and x86_64 (29); the formats elf (223 and the entry's), macho (205) and pe
// (184); none undeclared. The floors below sit under those counts, so a walk that reads nothing cannot
// pass.
//
// THE NEGATIVE IS SYNTHESIZED (`AnUndeclaredArchOrFormatIsNamedWithItsPlace`): the same walk, over a
// scratch tree, names every planted name with its document and JSON pointer, and passes the declared
// names planted beside them.
//
// The walk is an explicit worklist, never a recursion on the document's depth.

#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using dss::test_support::Location;
using dss::test_support::ScratchDir;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] json readJson(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return json::parse(in);
}

[[nodiscard]] bool endsWith(std::string const& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The names the shipped documents DECLARE: each target document's `target.name` (the arch vocabulary),
// each object-format document's `format.kind`.
struct Declared {
    std::set<std::string> archs;
    std::set<std::string> kinds;
};

[[nodiscard]] Declared declaredBy(fs::path const& config) {
    Declared d;
    for (auto const& e : fs::directory_iterator{config / "targets"})
        if (endsWith(e.path().filename().string(), ".target.json"))
            d.archs.insert(readJson(e.path()).at("target").at("name").get<std::string>());
    for (auto const& e : fs::directory_iterator{config / "object-formats"})
        if (endsWith(e.path().filename().string(), ".format.json"))
            d.kinds.insert(readJson(e.path()).at("format").at("kind").get<std::string>());
    return d;
}

// One name a selector names that no document declares, and where.
struct Undeclared {
    std::string document;   // relative to the config root, generic spelling
    std::string pointer;    // the JSON pointer of the name
    std::string value;
    bool        isArch = false;
    auto operator<=>(Undeclared const&) const = default;
};

struct Sweep {
    std::size_t                   documents = 0;
    std::size_t                   whens = 0;
    std::size_t                   archEntries = 0;
    std::map<std::string, std::size_t> named;   // axis -> how many names it was given
    std::vector<Undeclared>       undeclared;
};

[[nodiscard]] std::size_t namedOn(Sweep const& r, std::string const& axis) {
    auto const it = r.named.find(axis);
    return it == r.named.end() ? 0 : it->second;
}

// RFC 6901: `~` and `/` in a key are escaped.
[[nodiscard]] std::string pointerToken(std::string const& key) {
    std::string out;
    for (char const c : key) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out += c;
    }
    return out;
}

// One selector object's two axes against the declared names.
void checkAxes(json const& selector, std::string const& document, std::string const& at, Declared const& d,
               Sweep& r) {
    struct Axis {
        char const*                  key;
        std::set<std::string> const* declared;
        bool                         isArch;
    };
    for (Axis const& axis : {Axis{"arch", &d.archs, true}, Axis{"format", &d.kinds, false}}) {
        auto const it = selector.find(axis.key);
        if (it == selector.end()) continue;
        ++r.named[axis.key];
        std::string const value = it->is_string() ? it->get<std::string>() : it->dump();
        if (!axis.declared->contains(value))
            r.undeclared.push_back({document, at + "/" + axis.key, value, axis.isArch});
    }
}

[[nodiscard]] Sweep sweep(fs::path const& config, Declared const& d) {
    Sweep r;
    std::vector<fs::path> files;
    for (auto const& e : fs::recursive_directory_iterator{config})
        if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (fs::path const& f : files) {
        ++r.documents;
        json const doc = readJson(f);
        std::string const rel = fs::relative(f, config).generic_string();
        std::vector<std::pair<json const*, std::string>> work{{&doc, std::string{}}};
        while (!work.empty()) {
            auto const [node, at] = work.back();
            work.pop_back();
            if (node->is_object()) {
                for (auto const& item : node->items()) {
                    std::string const here = at + "/" + pointerToken(item.key());
                    json const& v = item.value();
                    if (item.key() == "when" && v.is_object()) {
                        ++r.whens;
                        checkAxes(v, rel, here, d, r);
                    }
                    if (item.key() == "availableObjectFormats" && v.is_array()) {
                        for (std::size_t i = 0; i < v.size(); ++i) {
                            if (!v[i].is_object()) continue;
                            ++r.archEntries;
                            checkAxes(v[i], rel, here + "/" + std::to_string(i), d, r);
                        }
                    }
                    work.emplace_back(&v, here);
                }
            } else if (node->is_array()) {
                for (std::size_t i = 0; i < node->size(); ++i)
                    work.emplace_back(&(*node)[i], at + "/" + std::to_string(i));
            }
        }
    }
    std::sort(r.undeclared.begin(), r.undeclared.end());
    return r;
}

[[nodiscard]] std::string describe(std::vector<Undeclared> const& names, Declared const& d) {
    std::string out;
    for (Undeclared const& u : names) {
        std::set<std::string> const& declared = u.isArch ? d.archs : d.kinds;
        out += "\n  " + u.document + " " + u.pointer + ": the selector names the "
             + (u.isArch ? "arch" : "format") + " '" + u.value + "', which no shipped "
             + (u.isArch ? "target" : "object-format") + " document declares (declared:";
        for (std::string const& n : declared) out += " " + n;
        out += ")";
    }
    return out;
}

void writeText(fs::path const& p, std::string const& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out{p, std::ios::binary};
    out << text;
    ASSERT_TRUE(out.good()) << "did not reach disk: " << p.string();
}

}  // namespace

TEST(WhenSelectorVocabulary, EveryArchAndFormatASelectorNamesIsDeclaredByAShippedDocument) {
    fs::path const config = dss::test::configRoot();
    Declared const d = declaredBy(config);
    ASSERT_FALSE(d.archs.empty()) << "no shipped target document was read";
    ASSERT_FALSE(d.kinds.empty()) << "no shipped object-format document was read";
    Sweep const r = sweep(config, d);
    EXPECT_TRUE(r.undeclared.empty()) << describe(r.undeclared, d);
    // FLOORS, under the counts MEASURED above: a walk that read nothing has nothing to refuse.
    EXPECT_GE(r.documents, 80u) << "the config tree's documents were not walked";
    EXPECT_GE(r.whens, 600u) << "the `when` objects were not reached";
    EXPECT_GE(namedOn(r, "arch"), 40u) << "the arch names were not reached";
    EXPECT_GE(r.archEntries, 1u) << "the availability arch entries were not reached";
}

TEST(WhenSelectorVocabulary, AnUndeclaredArchOrFormatIsNamedWithItsPlace) {
    ScratchDir dir{Location::Temp, "when-vocabulary"};
    fs::path const config = dir.path() / "src" / "dss-config";
    writeText(config / "targets" / "a.target.json", R"({ "target": { "name": "arm64" } })");
    writeText(config / "object-formats" / "e.format.json", R"({ "format": { "kind": "elf" } })");
    writeText(config / "object-formats" / "m.format.json", R"({ "format": { "kind": "macho" } })");
    // Planted: an arch no target declares in a `when` (arm46) and in an availability entry (x86-64), a
    // format outside the kind vocabulary (elff), and -- one directory down -- a kind in the vocabulary that
    // no document declares here (pe). Beside them, the declared names (arm64, elf, macho), unreported.
    writeText(config / "shippedLibs" / "planted.json", R"({ "header": "planted.h",
  "constants": [ { "name": "P", "variants": [
      { "when": { "format": "elf", "arch": "arm46" }, "value": 1, "type": "i32" },
      { "when": { "format": "elff" }, "value": 2, "type": "i32" },
      { "when": { "format": "elf", "arch": "arm64" }, "value": 3, "type": "i32" } ] } ],
  "symbols": [ { "name": "s", "signature": "u64", "kind": "object", "linkage": "external",
                 "availableObjectFormats": [ "macho", { "format": "elf", "arch": "x86-64" } ] } ] })");
    writeText(config / "shippedLibs" / "sys" / "nested.json", R"({ "header": "sys/nested.h",
  "typedefs": [ { "name": "t", "variants": [ { "when": { "format": "pe" }, "type": "i32" },
                                             { "when": { "format": "macho" }, "type": "i64" } ] } ] })");

    Declared const d = declaredBy(config);
    Sweep const r = sweep(config, d);
    std::vector<Undeclared> const expected{
        {"shippedLibs/planted.json", "/constants/0/variants/0/when/arch", "arm46", true},
        {"shippedLibs/planted.json", "/constants/0/variants/1/when/format", "elff", false},
        {"shippedLibs/planted.json", "/symbols/0/availableObjectFormats/1/arch", "x86-64", true},
        {"shippedLibs/sys/nested.json", "/typedefs/0/variants/0/when/format", "pe", false},
    };
    EXPECT_EQ(r.undeclared, expected) << "named:" << describe(r.undeclared, d) << "\nexpected:"
                                      << describe(expected, d);
    EXPECT_EQ(r.whens, 5u);
    EXPECT_EQ(r.archEntries, 1u);
    EXPECT_EQ(namedOn(r, "arch"), 3u);
}
