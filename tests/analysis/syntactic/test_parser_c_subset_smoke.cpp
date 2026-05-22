#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
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

struct CSubsetHarness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
    TokenStream                          stream;
};

[[nodiscard]] CSubsetHarness loadAndTokenize(std::string source) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    EXPECT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString(std::move(source), "<csubset-smoke>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();
    return CSubsetHarness{
        .src    = std::move(src),
        .schema = std::move(schema),
        .stream = std::move(stream),
    };
}

[[nodiscard]] bool hasInternalNodeWithRule(Tree const& t,
                                           std::string_view ruleName) {
    if (!t.hasSchema()) return false;
    const auto ruleId = t.schema().rules().find(ruleName);
    if (!ruleId.valid()) return false;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == ruleId.v) return true;
    }
    return false;
}

} // namespace

// Smoke: parser drives c-subset end-to-end on minimal input. Tree-shape
// pinning lives in PA4 corpus tests; this only confirms the
// AltChoice→RuleLeaf search fallback + optional/repeat nullable-skip
// paths work against the real grammar.
TEST(ParserCSubsetSmoke, IntVarDeclWithLiteralInitializer) {
    auto h = loadAndTokenize("int x = 5;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(hasInternalNodeWithRule(t, "topLevel"))
        << "tree must include a topLevel frame";
    EXPECT_TRUE(hasInternalNodeWithRule(t, "typeRef"))
        << "tree must include a typeRef frame (exercises optional-skip)";
}
