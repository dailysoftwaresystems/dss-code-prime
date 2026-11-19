// A SHAPE THAT REACHES ITSELF BEFORE CONSUMING A TOKEN IS REFUSED (C_CircularShape), and the
// loader's fixpoints converge however many passes a grammar asks of them (cycle P69 L2, lane `lm`).
//
// The language-config spec has always documented `C_CircularShape` for a left-recursive shape
// (`a: { sequence: [a, "End"] }`), and the loader never made the refusal: the code's one producer
// was a FOLLOW-set iteration cap. These cases pin the documented rule: the documented example, a
// left recursion behind a prefix or a shape that can match nothing, one through another shape,
// refused by name with the cycle and nothing else; a right-recursive shape, a `repeat` and a
// recursion behind a shape that must consume a token still load. And a FIRST-set chain laid
// against the order the loader iterates the shapes in, which takes one pass per shape, loads with
// its head rule's set complete.

#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

std::string grammarWith(std::string_view shapes) {
    return std::string{R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "LeftRec", "version": "0.0.1", "fileExtensions": [".lrec"] },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    ";":  [{ "kind": "End" }]
  },
  "keywords": [ { "word": "x", "kind": "XKw" } ],
  "syncTokens": [ "End" ],
  "shapes": {
)JSON"} + std::string{shapes} + R"JSON(
  }
})JSON";
}

// The C_CircularShape diagnostics of a load that must fail, as "path: message" lines.
std::vector<std::string> circularShapes(std::string_view shapes) {
    auto const loaded = GrammarSchema::loadFromText(grammarWith(shapes), "<left-recursion>");
    EXPECT_FALSE(loaded.has_value()) << "a left-recursive grammar loaded";
    std::vector<std::string> out;
    if (loaded.has_value()) return out;
    std::string other;
    for (auto const& d : loaded.error()) {
        if (d.code == DiagnosticCode::C_CircularShape) out.push_back(d.path + ": " + d.message);
        // A left recursion through an `alt` (an `optional` is one) makes its branches share a
        // FIRST token — the recursive branch begins with whatever the shape begins with — so the
        // overlap is said beside it (✔MEASURED 2026-10-20: `b` and `c` below). Anything else is not.
        else if (d.code != DiagnosticCode::C_AmbiguousAlternatives)
            other += d.path + ": " + d.message + "\n";
    }
    EXPECT_EQ(other, "") << "the left recursion is the load's one refusal";
    return out;
}

bool loads(std::string_view shapes, std::string& why) {
    auto const loaded = GrammarSchema::loadFromText(grammarWith(shapes), "<left-recursion>");
    if (loaded.has_value()) return true;
    for (auto const& d : loaded.error()) why += d.path + ": " + d.message + "\n";
    return false;
}

}  // namespace

TEST(GrammarLeftRecursion, TheDocumentedExampleIsRefusedByName) {
    auto const said = circularShapes(R"(    "root": { "sequence": [ "a" ] },
    "a": { "sequence": [ "a", "End" ] })");
    ASSERT_EQ(said.size(), 1u);
    EXPECT_NE(said[0].find("/shapes/a: "), std::string::npos) << said[0];
    EXPECT_NE(said[0].find("the shape 'a' can reach itself before consuming any token (a → a)"),
              std::string::npos)
        << said[0];
}

TEST(GrammarLeftRecursion, ALeftRecursionBehindAPrefixThatCanMatchNothingIsRefused) {
    auto const said = circularShapes(R"(    "root": { "sequence": [ "b" ] },
    "b": { "sequence": [ { "optional": "XKw" }, "b", "End" ] })");
    ASSERT_EQ(said.size(), 1u);
    EXPECT_NE(said[0].find("(b → b)"), std::string::npos) << said[0];
}

// A shape that can match nothing stands in front of the recursion: the walk goes past it.
TEST(GrammarLeftRecursion, ALeftRecursionBehindAShapeThatCanMatchNothingIsRefused) {
    auto const said = circularShapes(R"(    "root": { "sequence": [ "e" ] },
    "e": { "sequence": [ "n", "e", "End" ] },
    "n": { "sequence": [ { "optional": "XKw" } ] })");
    ASSERT_EQ(said.size(), 1u);
    EXPECT_NE(said[0].find("(e → e)"), std::string::npos) << said[0];
}

TEST(GrammarLeftRecursion, ALeftRecursionThroughAnotherShapeIsRefusedNamingTheCycle) {
    auto const said = circularShapes(R"(    "root": { "sequence": [ "c" ] },
    "c": { "alt": [ { "sequence": [ "d", "End" ] }, "XKw" ] },
    "d": { "sequence": [ "c" ] })");
    ASSERT_EQ(said.size(), 1u) << "one cycle, reported once";
    // The cycle is named from the shape the walk met first; either rotation names it.
    EXPECT_TRUE(said[0].find("(c → d → c)") != std::string::npos
                || said[0].find("(d → c → d)") != std::string::npos)
        << said[0];
}

TEST(GrammarLeftRecursion, ARightRecursiveShapeAndARepeatStillLoad) {
    std::string why;
    EXPECT_TRUE(loads(R"(    "root": { "sequence": [ "r" ] },
    "r": { "alt": [ { "sequence": [ "XKw", "r" ] }, "End" ] })", why))
        << why;
    EXPECT_TRUE(loads(R"(    "root": { "sequence": [ { "repeat": "s" } ] },
    "s": { "sequence": [ "XKw", "End" ] })", why))
        << why;
    // A shape that must consume a token stands in front of the recursion: the walk stops at it.
    EXPECT_TRUE(loads(R"(    "root": { "sequence": [ "q" ] },
    "q": { "alt": [ { "sequence": [ "t", "q" ] }, "End" ] },
    "t": { "sequence": [ "XKw" ] })", why))
        << why;
}

// The loader iterates the shapes in key order, so a chain of references laid AGAINST that order
// (`r000` refers to `r001`, …, `r299` holds the token) settles one shape per pass: 300 passes for
// the head's FIRST set. The bound is the lattice's height, so every pass is allowed, and the head's
// set is complete.
TEST(GrammarLeftRecursion, AFirstSetChainAgainstTheKeyOrderConvergesComplete) {
    std::string shapes = R"(    "root": { "sequence": [ "r000" ] })";
    char name[8];
    char next[8];
    for (int i = 0; i < 300; ++i) {
        std::snprintf(name, sizeof name, "r%03d", i);
        std::snprintf(next, sizeof next, "r%03d", i + 1);
        shapes += ",\n    \"" + std::string{name} + "\": { \"sequence\": [ \"" +
                  (i == 299 ? std::string{"XKw"} : std::string{next}) + "\" ] }";
    }
    auto const loaded = GrammarSchema::loadFromText(grammarWith(shapes), "<first-chain>");
    ASSERT_TRUE(loaded.has_value()) << loaded.error()[0].message;
    GrammarSchema const& g = **loaded;
    RuleId const head = g.rules().find("r000");
    ASSERT_TRUE(head.valid());
    SchemaTokenId const x = g.schemaTokens().find("XKw");
    EXPECT_TRUE(g.firstSetContains(head, x)) << "the head rule's FIRST set is complete";
    EXPECT_TRUE(g.firstSetContains(g.rules().find("root"), x));
}
