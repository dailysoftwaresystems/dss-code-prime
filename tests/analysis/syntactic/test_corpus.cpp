// PA4 corpus stress: load every file under `tests/corpus/<lang>/` and
// drive it through `Parser::parse()` against the shipped grammar.
// Each corpus file gets a sibling `<file>.tree` golden capturing the
// `prettyPrint(tree)` shape — full structural pin, not just
// "no errors". Re-generate goldens via `DSS_REFRESH_GOLDENS=1 ctest`.
// Adding a known-bad corpus file later (with a `<file>.diag` sibling
// listing the expected diagnostic codes) is the next step PA5/PA6 want.

#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/tree_visitor.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

using namespace dss;
namespace fs = std::filesystem;

namespace {

// Walk up from the running test binary's working directory looking
// for the `tests/corpus/` tree. The CI / local-dev cwd is the build
// directory, which is somewhere under the repo root — same pattern
// `GrammarSchema::loadShipped` uses to find shipped configs.
[[nodiscard]] fs::path findCorpusRoot() {
    fs::path cwd = fs::current_path();
    for (int hops = 0; hops < 8; ++hops) {
        const auto candidate = cwd / "tests" / "corpus";
        if (fs::is_directory(candidate)) return candidate;
        if (!cwd.has_parent_path() || cwd == cwd.parent_path()) break;
        cwd = cwd.parent_path();
    }
    ADD_FAILURE() << "could not locate tests/corpus/ from cwd "
                  << fs::current_path().string();
    std::abort();
}

[[nodiscard]] std::string readFile(fs::path const& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        ADD_FAILURE() << "cannot open " << path.string();
        std::abort();
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return std::move(buf).str();
}

// Render the parsed tree as the structural fingerprint the golden
// file captures. AST mode (skip `EmptySpace` trivia) keeps the golden
// readable; visible Token leaves quote their source text.
[[nodiscard]] std::string prettyPrintTree(Tree const& t) {
    std::string out;
    if (!t.root().valid()) return out;
    walkPreOrder(TreeCursor{t, t.root(), CursorMode::Ast},
                 [&](TreeCursor const& c) {
        const int d = c.depth();
        for (int i = 0; i < d; ++i) out += "  ";
        const auto id = c.current();
        if (t.kind(id) == NodeKind::Internal) {
            out += "rule:";
            out += t.rules().name(t.rule(id));
        } else {
            out += "tok:\"";
            out += t.text(id);
            out += '"';
        }
        out += '\n';
    });
    return out;
}

// Golden-file comparison. Each corpus file gets a sibling `.tree`
// capturing the expected `prettyPrintTree(tree)`. To regenerate after
// an intentional grammar change, set the env var
// `DSS_REFRESH_GOLDENS=1` and re-run ctest — the helper rewrites the
// golden in place and skips the byte-compare. Missing golden = fail
// (better to error loudly than silently skip pinning on a new corpus
// addition that forgot the companion file).
void checkGoldenTree(Tree const& t, fs::path const& goldenPath) {
    const std::string actual = prettyPrintTree(t);
    if (std::getenv("DSS_REFRESH_GOLDENS") != nullptr) {
        std::ofstream out{goldenPath, std::ios::binary};
        if (!out) {
            ADD_FAILURE() << "cannot write golden file "
                          << goldenPath.string();
            return;
        }
        out << actual;
        std::cout << "Refreshed " << goldenPath.string() << "\n";
        return;
    }
    if (!fs::exists(goldenPath)) {
        ADD_FAILURE() << "missing golden file " << goldenPath.string()
                      << " — generate it once via "
                         "`DSS_REFRESH_GOLDENS=1 ctest`";
        return;
    }
    std::ifstream in{goldenPath, std::ios::binary};
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string expected = std::move(buf).str();
    EXPECT_EQ(actual, expected)
        << "tree shape diverged from " << goldenPath.filename().string()
        << " — if the change is intentional, re-run with "
           "`DSS_REFRESH_GOLDENS=1` to update the golden";
}

// Parse one corpus file end-to-end and assert no error diagnostics.
// Returns the tree so individual tests can pin extra structural
// properties when they want to.
[[nodiscard]] Tree parseClean(std::string_view configName,
                              fs::path const&  corpusFile) {
    auto loaded = GrammarSchema::loadShipped(configName);
    if (!loaded.has_value()) {
        ADD_FAILURE() << "loadShipped(\"" << configName << "\") failed: "
                      << loaded.error()[0].message;
        std::abort();
    }
    auto schema = *loaded;

    const auto source = readFile(corpusFile);
    auto src = SourceBuffer::fromString(source, corpusFile.string());
    Tokenizer tk{src, schema};
    auto [stream, lexerDiags] = std::move(tk).tokenize();

    EXPECT_TRUE(lexerDiags->all().empty())
        << "tokenizer must produce zero diagnostics on a clean corpus file";

    Parser p{src, schema, std::move(stream)};
    auto result = std::move(p).parse();
    return std::move(result.tree);
}

} // namespace

// ── c-subset ────────────────────────────────────────────────────────────

TEST(Corpus, CSubsetMiniCalcParsesClean) {
    const auto path = findCorpusRoot() / "c-subset" / "mini_calc.c";
    Tree t = parseClean("c-subset", path);

    ASSERT_NE(t.root(), InvalidNode);
    if (t.diagnostics().hasErrors()) {
        std::string detail;
        for (auto const& d : t.diagnostics().all()) {
            detail += std::string{diagnosticCodeName(d.code)} + " at "
                    + std::to_string(d.span.start()) + ".."
                    + std::to_string(d.span.end())
                    + " actual=" + d.actual + "\n";
        }
        ADD_FAILURE() << "diagnostics:\n" << detail;
    }
    EXPECT_FALSE(hasError(t.flags(t.root())));
    checkGoldenTree(t, path.string() + ".tree");
}

// ── tsql-subset ─────────────────────────────────────────────────────────

TEST(Corpus, TsqlSchemaAndDmlParsesClean) {
    const auto path = findCorpusRoot() / "tsql-subset" / "schema_and_dml.sql";
    Tree t = parseClean("tsql-subset", path);

    ASSERT_NE(t.root(), InvalidNode);
    if (t.diagnostics().hasErrors()) {
        std::size_t shown = 0;
        for (auto const& d : t.diagnostics().all()) {
            if (shown++ >= 5) break;
            ADD_FAILURE() << diagnosticCodeName(d.code)
                          << " at " << d.span.start() << ".." << d.span.end()
                          << " actual=" << d.actual;
        }
    }
    checkGoldenTree(t, path.string() + ".tree");
}

// ── toy ─────────────────────────────────────────────────────────────────

TEST(Corpus, ToyDemoParsesClean) {
    const auto path = findCorpusRoot() / "toy" / "demo.toy";
    Tree t = parseClean("toy", path);

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "first diag: "
        << (t.diagnostics().all().empty()
                ? "<none>"
                : diagnosticCodeName(t.diagnostics().all().front().code));
    checkGoldenTree(t, path.string() + ".tree");
}
