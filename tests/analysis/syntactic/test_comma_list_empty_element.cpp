// THE EMPTY ELEMENT IN A COMMA-SEPARATED LIST, AND THE LEGAL TRAILING COMMA
// THAT USED TO BE THE SAME `{optional}`.
//
// The c grammar spells the enumerator list (C23 6.7.2.2) and the brace
// initializer list (C23 6.7.10) with the SAME tail shape, and the second was
// copied from the first. Both wrote the tail as
//
//     {repeat: {sequence: [<separator>, {optional: <element>}]}}
//
// where the inner `{optional}` is what admits C23's ONE legal trailing comma —
// the last iteration matches a comma with nothing after it. But it was optional
// on EVERY iteration, so `A, , B` matched as (comma + nothing) then (comma + B)
// and the EMPTY ELEMENT WAS SILENTLY DROPPED. Measured through the real CLI
// before the fix: `enum E { A, , B }; return B;` compiled rc 0 with zero bytes
// on stderr and ran 1, i.e. it parsed as `enum E { A, B }`; `int a[3]={1,,3};`
// compiled rc 0 and ran 130, i.e. `{1,3,0}` — the empty element dropped and
// every later element slid down a position, producing an initializer the author
// never wrote. gcc 13.3.0 `-std=c2x`, clang 18.1.3 `-std=c23` and MSVC 14.51
// `/std:clatest` all refuse both forms, each pointing at the SECOND comma, and
// all three accept both trailing commas.
//
// The fix is entirely in the language document (see the `listSeparator` rule's
// own `$comment` for the mechanism): the separator became a first-class RULE so
// the loop-back edge stops being a bare-token step the cursor's transparent
// token router takes without a decision, and the post-separator element optional
// became SPECULATIVE so D-PARSE-SPECULATIVE-OPTIONAL excludes the loop-back
// continuation from candidate enumeration. Nothing in this file's subject
// matter is compiled in — the mutant for red-on-disable is the DOCUMENT.
//
// WHAT EACH GROUP PINS
//   * the legal trailing comma, in BOTH lists, plus the element COUNT — a
//     "fix" that refuses it is a regression, and one that silently drops the
//     last element passes a no-errors check while corrupting the program.
//   * the empty element, in BOTH lists, refused AT THE OFFENDING COMMA — the
//     same token every reference names.
//   * the shared separator rule reaching BOTH lists — the pin that catches a
//     mutant reverting only one of the two copies.
//   * a long list, so the speculative optional cannot regress into per-element
//     probing or exhaust a speculation budget.

#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace dss;

namespace {

struct Parsed {
    std::shared_ptr<SourceBuffer> src;
    Tree                          tree;
};

[[nodiscard]] NodeId firstNodeWithRule(Tree const& t, std::string_view ruleName) {
    if (!t.hasSchema()) return NodeId{};
    const auto rid = t.schema().rules().find(ruleName);
    if (!rid.valid()) return NodeId{};
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == rid.v) return id;
    }
    return NodeId{};
}

// DIRECT children of `parent` whose rule is `ruleName`. Direct, not
// descendant: every consumer of these two lists (the HIR brace-init lowering's
// `initElementRule` filter, the semantic tier's array-size inference, the
// composite `fieldChildren.rule` scan) selects its elements from the DIRECT
// children, so that is the level a count pin has to assert at.
[[nodiscard]] std::size_t countDirectChildrenWithRule(Tree const& t, NodeId parent,
                                                      std::string_view ruleName) {
    if (!parent.valid() || !t.hasSchema()) return 0;
    const auto rid = t.schema().rules().find(ruleName);
    if (!rid.valid()) return 0;
    std::size_t n = 0;
    for (NodeId c : t.children(parent)) {
        if (t.kind(c) == NodeKind::Internal && t.rule(c).v == rid.v) ++n;
    }
    return n;
}

[[nodiscard]] std::size_t countNodesWithRule(Tree const& t, std::string_view ruleName) {
    if (!t.hasSchema()) return 0;
    const auto rid = t.schema().rules().find(ruleName);
    if (!rid.valid()) return 0;
    std::size_t n = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == rid.v) ++n;
    }
    return n;
}

// The (line, column) of the FIRST error diagnostic, so a refusal can be pinned
// to the exact token rather than merely to "something failed".
struct ErrorSite { bool found = false; std::uint32_t line = 0; std::uint32_t column = 0; };

[[nodiscard]] ErrorSite firstErrorSite(Tree const& t) {
    for (auto const& d : t.diagnostics().all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        const auto lc = t.source().lineCol(d.span.start());
        return ErrorSite{true, lc.line, lc.column};
    }
    return ErrorSite{};
}

} // namespace

// The shipped `c` document is loaded ONCE for the whole suite: `loadShipped`
// does not cache, so a per-case load would re-read and re-compile the document
// for every one of these parses. A failed load is an ENVIRONMENT failure and
// says nothing about the grammar, so it is reported by `SetUp` as a clean
// per-case failure rather than by aborting the process — this repo's
// `no_abort_in_tests_guard` keeps a per-file inventory of the `std::abort()`
// sites it tolerates, and a new test earns a red rather than a new entry.
class CommaListEmptyElement : public ::testing::Test {
  protected:
    static std::shared_ptr<GrammarSchema const> schema;

    static void SetUpTestSuite() {
        auto loaded = GrammarSchema::loadShipped("c");
        if (loaded.has_value()) schema = *loaded;
    }
    static void TearDownTestSuite() { schema.reset(); }

    void SetUp() override {
        ASSERT_NE(schema, nullptr) << "GrammarSchema::loadShipped(\"c\") failed";
    }

    [[nodiscard]] static Parsed parseC(std::string source) {
        auto src = SourceBuffer::fromString(std::move(source), "<comma-list>");
        Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
        auto [stream, _] = std::move(tk).tokenize();
        Parser p{src, schema, std::move(stream), DiagnosticBudget::libraryDefault()};
        auto result = std::move(p).parse();
        return Parsed{std::move(src), std::move(result.tree)};
    }
};

std::shared_ptr<GrammarSchema const> CommaListEmptyElement::schema;

// ── THE CONTROL: the legal C23 trailing comma, in BOTH lists ──────────────
//
// Pinned FIRST and pinned with a COUNT. `hasErrors() == false` alone would stay
// green for a grammar that quietly dropped the last element, which is the exact
// failure the defect being fixed here consisted of.

TEST_F(CommaListEmptyElement, EnumeratorListTrailingCommaIsAcceptedAndKeepsEveryEnumerator) {
    auto p = parseC("enum E { A, B, C, };\n");
    auto const& t = p.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    const NodeId body = firstNodeWithRule(t, "enumBody");
    ASSERT_TRUE(body.valid());
    EXPECT_EQ(countDirectChildrenWithRule(t, body, "enumerator"), 3u);
}

TEST_F(CommaListEmptyElement, BraceInitListTrailingCommaIsAcceptedAndKeepsEveryElement) {
    auto p = parseC("int a[3] = { 1, 2, 3, };\n");
    auto const& t = p.tree;
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    const NodeId list = firstNodeWithRule(t, "braceInitList");
    ASSERT_TRUE(list.valid());
    EXPECT_EQ(countDirectChildrenWithRule(t, list, "initElement"), 3u);
}

TEST_F(CommaListEmptyElement, NoTrailingCommaIsUnchangedInBothLists) {
    {
        auto p = parseC("enum E { A, B, C };\n");
        auto const& t = p.tree;
        EXPECT_FALSE(t.diagnostics().hasErrors());
        const NodeId body = firstNodeWithRule(t, "enumBody");
        ASSERT_TRUE(body.valid());
        EXPECT_EQ(countDirectChildrenWithRule(t, body, "enumerator"), 3u);
    }
    {
        auto p = parseC("int a[3] = { 1, 2, 3 };\n");
        auto const& t = p.tree;
        EXPECT_FALSE(t.diagnostics().hasErrors());
        const NodeId list = firstNodeWithRule(t, "braceInitList");
        ASSERT_TRUE(list.valid());
        EXPECT_EQ(countDirectChildrenWithRule(t, list, "initElement"), 3u);
    }
}

TEST_F(CommaListEmptyElement, EmptyAndSingletonListsAreUnchanged) {
    {
        auto p = parseC("enum E { A };\n");
        EXPECT_FALSE(p.tree.diagnostics().hasErrors());
    }
    {
        auto p = parseC("enum E { A, };\n");
        EXPECT_FALSE(p.tree.diagnostics().hasErrors());
    }
    {
        auto p = parseC("int a[1] = { 0 };\nint b[1] = { 0, };\nstruct S { int x; };\n"
                        "struct S s = { 0 };\n");
        EXPECT_FALSE(p.tree.diagnostics().hasErrors());
    }
}

TEST_F(CommaListEmptyElement, NestedBracesEachCarryTheirOwnTrailingComma) {
    auto p = parseC("int a[2][2] = { { 1, 2, }, { 3, 4, }, };\n");
    auto const& t = p.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());
    const NodeId outer = firstNodeWithRule(t, "braceInitList");
    ASSERT_TRUE(outer.valid());
    EXPECT_EQ(countDirectChildrenWithRule(t, outer, "initElement"), 2u);
    // Three brace lists: the outer one and the two inner ones.
    EXPECT_EQ(countNodesWithRule(t, "braceInitList"), 3u);
}

TEST_F(CommaListEmptyElement, DesignatedInitializersKeepTheirTrailingComma) {
    auto p = parseC("struct S { int x; int y; };\n"
                    "struct S s = { .x = 1, .y = 2, };\n");
    auto const& t = p.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());
    const NodeId list = firstNodeWithRule(t, "braceInitList");
    ASSERT_TRUE(list.valid());
    EXPECT_EQ(countDirectChildrenWithRule(t, list, "initElement"), 2u);
}

// ── THE DEFECT: an empty element, refused AT THE OFFENDING COMMA ──────────

TEST_F(CommaListEmptyElement, EmptyEnumeratorIsRefusedAtTheSecondComma) {
    // Column 13 is the SECOND comma — the same column gcc, clang and MSVC each
    // report for this line.
    auto p = parseC("enum E { A, , B };\n");
    auto const& t = p.tree;
    EXPECT_TRUE(t.diagnostics().hasErrors());
    const auto site = firstErrorSite(t);
    ASSERT_TRUE(site.found);
    EXPECT_EQ(site.line, 1u);
    EXPECT_EQ(site.column, 13u);
}

TEST_F(CommaListEmptyElement, EmptyInitializerElementIsRefusedAtTheSecondComma) {
    // `int a[3]={1,,3};` — column 13 is the second comma, matching all three
    // references. Before the fix this compiled rc 0 and RAN `{1,3,0}`.
    auto p = parseC("int a[3]={1,,3};\n");
    auto const& t = p.tree;
    EXPECT_TRUE(t.diagnostics().hasErrors());
    const auto site = firstErrorSite(t);
    ASSERT_TRUE(site.found);
    EXPECT_EQ(site.line, 1u);
    EXPECT_EQ(site.column, 13u);
}

TEST_F(CommaListEmptyElement, EmptyElementIsRefusedInEveryPosition) {
    for (std::string_view src : {
             "enum E { A, , B };\n",       // interior
             "enum E { A,, };\n",          // before the legal trailing comma
             "enum E { A, , };\n",
             "enum E { , A };\n",          // leading (already refused before)
             "enum E { , };\n",
             "int a[4]={1,,3,4};\n",
             "int a[4]={1,2,,};\n",
             "int a[4]={,1,2,3};\n",
             "int a[2][2]={{1,,2},{3,4}};\n",
             "struct S { int x; int y; };\nstruct S s = { .x = 1, , .y = 2 };\n",
         }) {
        auto p = parseC(std::string{src});
        EXPECT_TRUE(p.tree.diagnostics().hasErrors())
            << "accepted an empty list element: " << src;
    }
}

// ── THE SHARED SHAPE: one separator rule, reaching BOTH lists ─────────────
//
// The two lists are one defect in two places. A mutant that reverts only one of
// them leaves the other's behaviour tests green, so the structural fact — that
// the SAME `listSeparator` rule is what both lists descend into — is pinned
// directly.

TEST_F(CommaListEmptyElement, TheSeparatorRuleIsSharedByBothLists) {
    {
        auto p = parseC("enum E { A, B, C };\n");
        auto const& t = p.tree;
        ASSERT_TRUE(t.hasSchema());
        ASSERT_TRUE(t.schema().rules().find("listSeparator").valid())
            << "the shared list-separator rule is gone from the document";
        const NodeId body = firstNodeWithRule(t, "enumBody");
        ASSERT_TRUE(body.valid());
        EXPECT_EQ(countDirectChildrenWithRule(t, body, "listSeparator"), 2u)
            << "the enumerator list is not routing its separators through the "
               "shared rule";
    }
    {
        auto p = parseC("int a[3] = { 1, 2, 3 };\n");
        auto const& t = p.tree;
        const NodeId list = firstNodeWithRule(t, "braceInitList");
        ASSERT_TRUE(list.valid());
        EXPECT_EQ(countDirectChildrenWithRule(t, list, "listSeparator"), 2u)
            << "the initializer list is not routing its separators through the "
               "shared rule";
    }
}

// ── COST: the speculative optional must stay O(1) per element ─────────────
//
// The post-separator optional is speculative, and speculation in this parser has
// both a per-probe token budget and a depth ceiling. A long list is where a
// regression into per-element probing would show up as a refusal rather than as
// slowness, so it is pinned as a correctness fact, not left to a benchmark.

TEST_F(CommaListEmptyElement, ALongListWithATrailingCommaStillParses) {
    constexpr int kElements = 2000;
    std::string src = "int a[2000] = {";
    for (int i = 0; i < kElements; ++i) src += "0,";   // includes the trailing comma
    src += "};\n";
    auto p = parseC(std::move(src));
    auto const& t = p.tree;
    EXPECT_FALSE(t.diagnostics().hasErrors());
    const NodeId list = firstNodeWithRule(t, "braceInitList");
    ASSERT_TRUE(list.valid());
    EXPECT_EQ(countDirectChildrenWithRule(t, list, "initElement"),
              static_cast<std::size_t>(kElements));
}
