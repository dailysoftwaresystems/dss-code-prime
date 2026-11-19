// ===========================================================================
// P69 L2b2 (lane `lm`) — A PARSE'S SCOPE CHAIN DOES NOT GROW WITH ITS INPUT.
//
// The tree builder's scope chain (scope_trail.hpp) held one node per scope the
// parse ever pushed, committed or rolled back: ✔MEASURED 149,187 nodes at the end
// of sqlite3.c, a parse whose stack is never deeper than its nesting. It now
// holds what can be read — the live stack, the trails kept diagnostics and
// handed-out trails read, and the nodes committed checkpoints keep — so a clean
// program twice as long ends with the SAME chain, whatever its probes pushed and
// rolled back. `ParseResult::scopeChainNodesAtFinish` is the chain's size when
// the walk ends, read before the tree is finished.
//
// RED-ON-DISABLE (tests/mutations/arms.registry): a pop that drops nothing, and
// a rollback that keeps the refuted branch's nodes — the chain then grows with
// every statement.
// ⚠ MUST run through ctest: the shipped c document is found through the repo root.
// ===========================================================================

#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

using namespace dss;

namespace {

// One block of statements that push and pop scopes inside committed and refuted
// probes alike: a typedef-led declaration, casts, compound literals, sizeof of a
// type and of a value, a statement expression, nested braces and a for loop.
[[nodiscard]] std::string program(std::size_t blocks) {
    std::string s = "typedef int T;\nstruct P { int a; int b; };\nint main(void) {\n  int v = 1;\n";
    for (std::size_t i = 0; i < blocks; ++i) {
        s += "  {\n"
             "    T t = (T)v;\n"
             "    int w = (int)sizeof(T) + (int)sizeof(v) + (v);\n"
             "    struct P p = (struct P){ (int)(v), (T)w };\n"
             "    for (T k = 0; k < 2; ++k) { { w = w + (int)k; } }\n"
             "    v = ({ int q = (T)(w + p.a); q; }) + t;\n"
             "  }\n";
    }
    s += "  return v;\n}\n";
    return s;
}

[[nodiscard]] ParseResult parseC(std::string source) {
    auto loaded = GrammarSchema::loadShipped("c");
    EXPECT_TRUE(loaded.has_value()) << "the shipped c language document";
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    ParserConfig cfg;
    if (auto v = schema->maxExpressionDepth())      cfg.maxExpressionDepth = *v;
    if (auto v = schema->maxSpeculationDepth())     cfg.maxSpeculationDepth = *v;
    if (auto v = schema->speculationBudgetFactor()) cfg.speculationBudgetFactor = *v;
    auto src = SourceBuffer::fromString(std::move(source), "<scope-chain-bound>");
    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{std::move(src), std::move(schema), std::move(stream),
             DiagnosticBudget::libraryDefault(), std::move(cfg), std::move(lexDiags)};
    return std::move(p).parse();
}

} // namespace

TEST(ParserScopeChainBound, ACleanProgramTwiceAsLongEndsWithTheSameChain) {
    ParseResult const once  = parseC(program(500));
    ParseResult const twice = parseC(program(1000));
    ASSERT_FALSE(once.tree.diagnostics().hasErrors());
    ASSERT_FALSE(twice.tree.diagnostics().hasErrors());
    std::printf("scope chain at the walk's end: %llu nodes (500 blocks), %llu (1000 blocks); probes %llu, %llu\n",
                static_cast<unsigned long long>(once.scopeChainNodesAtFinish),
                static_cast<unsigned long long>(twice.scopeChainNodesAtFinish),
                static_cast<unsigned long long>(once.speculationProbeCount),
                static_cast<unsigned long long>(twice.speculationProbeCount));
    // Non-vacuous: the program's probes did push, refute and roll back.
    EXPECT_GT(twice.speculationProbeCount, once.speculationProbeCount);
    EXPECT_EQ(twice.scopeChainNodesAtFinish, once.scopeChainNodesAtFinish)
        << "the scope chain grew with the input: a node outlived everything that could read it";
}
