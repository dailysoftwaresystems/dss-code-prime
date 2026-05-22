// PA4 corpus stress: load every file under `tests/corpus/<lang>/` and
// drive it through `Parser::parse()` against the shipped grammar.
// The contract for each corpus file is "parses cleanly OR with a
// pinned diagnostic set"; PA4 ships only clean-parse files. Adding a
// known-bad corpus file later (with a `*.diag` sibling listing the
// expected diagnostic codes) is the next step PA5/PA6 want.

#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
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
}
