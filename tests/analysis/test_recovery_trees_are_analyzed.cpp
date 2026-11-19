// ★★ P69 (lane `cs`, fold f26e; main's ruling, ledger 806) — EVERY RECOVERY TREE WE HOLD IS
// ANALYZED TO THE END.
//
// THE INVARIANT: an analysis that may run on a tree that did not parse reads a node's rule
// only where the node IS a rule node. The CLI analyzes no unit that failed to parse
// (`D_LaterPhasesNotRun`), but the LSP server and the diagnostic corpus do, and a process
// abort (`Tree::rule on non-Internal node`, `Tree::tokenKind on non-Token node`) is the one
// outcome neither survives: the server dies with the editor's document, and the corpus binary
// loses every fixture's verdict. ✔MEASURED in fold f26e: a C23 attribute item broken after its
// namespace (`[[gnu::]]`) aborted `analyze()` in `standardItemClause` at seven positions.
//
// THE UNIVERSE is every recovery shape the repository already holds, discovered by the data,
// never listed here:
//   * every source of the diagnostic corpus, `tests/corpus/diagnostics/<lang>/` (each
//     language its own directory, the directory name the shipped schema's);
//   * every example whose manifest expects a diagnostic of the parse family (`P_`), the
//     examples that deliberately do not parse, `examples/<lang>/<name>/expected.json`.
// Each is built as the corpus driver builds it — the shipped schema, the CU on the large
// worker stack — and analyzed in a CHILD process, so one abort is a named red for that input
// and every other input still reports.
#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/substrate/large_stack_call.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"

#include "golden_file.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace dss;
namespace fs = std::filesystem;

namespace {

struct RecoveryInput {
    std::string lang;     // the shipped schema's name
    fs::path    source;
};

void addCorpusInputs(std::vector<RecoveryInput>& out) {
    fs::path const root = dss::test_support::findCorpusRoot() / "diagnostics";
    if (!fs::is_directory(root)) return;
    for (auto const& d : fs::directory_iterator{root}) {
        if (!d.is_directory()) continue;
        for (auto const& e : fs::directory_iterator{d.path()}) {
            if (e.is_regular_file() && e.path().extension() != ".diag") {
                out.push_back({d.path().filename().string(), e.path()});
            }
        }
    }
}

// An example is a recovery input when its manifest expects a diagnostic of the parse family
// (the `P_` codes: the lexer's, the preprocessor's and the parser's).
void addExampleInputs(std::vector<RecoveryInput>& out) {
    auto const repo = dss::test::findRepoRoot();
    if (!repo.has_value()) return;
    fs::path const root = *repo / "examples";
    if (!fs::is_directory(root)) return;
    for (auto const& langDir : fs::directory_iterator{root}) {
        if (!langDir.is_directory()) continue;
        for (auto const& ex : fs::directory_iterator{langDir.path()}) {
            fs::path const manifest = ex.path() / "expected.json";
            if (!ex.is_directory() || !fs::is_regular_file(manifest)) continue;
            std::ifstream in{manifest};
            nlohmann::json const m = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
            if (m.is_discarded() || !m.is_object()) continue;
            auto const exp = m.find("expectDiagnostics");
            auto const src = m.find("source");
            auto const lang = m.find("language");
            if (exp == m.end() || !exp->is_array() || src == m.end() || !src->is_string()
                || lang == m.end() || !lang->is_string()) {
                continue;
            }
            bool parseFamily = false;
            for (nlohmann::json const& d : *exp) {
                auto const c = d.find("code");
                if (c != d.end() && c->is_string() && c->get<std::string>().starts_with("P_")) {
                    parseFamily = true;
                    break;
                }
            }
            if (parseFamily) {
                out.push_back({lang->get<std::string>(), ex.path() / src->get<std::string>()});
            }
        }
    }
}

// Build and analyze one input; the exit status is the child's verdict (0: analyzed to the end).
[[noreturn]] void analyzeToTheEnd(RecoveryInput const& in) {
    auto loaded = GrammarSchema::loadShipped(in.lang);
    if (!loaded.has_value()) std::exit(4);
    std::string const source = dss::test_support::readFile(in.source);
    auto cu = dss::substrate::callOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes,
        [&]() -> std::shared_ptr<CompilationUnit> {
            UnitBuilder builder{*loaded, DiagnosticBudget::libraryDefault()};
            builder.addInMemory(source, in.source.filename().string());
            return std::make_shared<CompilationUnit>(std::move(builder).finish());
        });
    SemanticModel const model = analyze(cu, DiagnosticBudget::libraryDefault());
    (void)model;
    std::exit(0);
}

} // namespace

TEST(RecoveryTrees, EveryOneWeHoldIsAnalyzedToTheEnd) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    std::vector<RecoveryInput> inputs;
    addCorpusInputs(inputs);
    std::size_t const fromCorpus = inputs.size();
    addExampleInputs(inputs);
    ASSERT_GT(fromCorpus, 0u) << "the diagnostic corpus holds no source: nothing is analyzed";
    ASSERT_GT(inputs.size(), fromCorpus)
        << "no example expects a parse-family diagnostic: the examples' half is empty";
    std::ranges::sort(inputs, {}, [](RecoveryInput const& r) { return r.source; });
    for (RecoveryInput const& in : inputs) {
        EXPECT_EXIT(analyzeToTheEnd(in), ::testing::ExitedWithCode(0), "")
            << in.lang << " :: " << in.source.parent_path().filename().string() << "/"
            << in.source.filename().string()
            << ": a recovery tree must be analyzed to the end";
    }
}
