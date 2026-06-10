// V2-4 Part B — golden-DIAGNOSTIC harness.
//
// Makes parser/semantic diagnostic POSITIONS regression-proof BY
// CONSTRUCTION. Every malformed source under `tests/corpus/diagnostics/
// <lang>/` is parsed + semantically analyzed; EVERY emitted diagnostic
// (lexer/parser `P_*` from the trees + semantic `S_*` from `analyze()`)
// is rendered to a `<file>.diag` golden as
//     <DiagnosticCodeName> <sLine>:<sCol>-<eLine>:<eCol>[ related=[...]]
// (1-based line:col, half-open span end), sorted to a total order. A
// regression that mis-stamps a diagnostic's `span` (or a related
// location's) changes the rendered line:col → the golden mismatches →
// RED. Adding coverage for a new code/language is a pure file-drop +
// `DSS_REFRESH_GOLDENS=1 ctest` — the harness is the by-construction
// chokepoint, so "every code" grows trivially (no test-code edits).
//
// AGNOSTIC: the `<lang>` subdir name IS the shipped-schema name; the
// harness DISCOVERS dirs/files and drives each through the shipped
// grammar — there is NO hardcoded language list or `if (lang == …)`.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"

#include "golden_file.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
namespace fs = std::filesystem;

namespace {

// Render one (buffer, span) as `<sLine>:<sCol>-<eLine>:<eCol>` (1-based,
// half-open end), resolving the BufferId through the registry built from
// the CU's trees. A diagnostic whose buffer isn't registered (should not
// happen for a single-CU corpus file) renders a loud sentinel so a
// silent mis-wire surfaces in the golden rather than passing.
[[nodiscard]] std::string renderSpan(BufferId buffer, SourceSpan span,
                                     BufferRegistry const& bufs) {
    auto buf = bufs.tryGet(buffer);
    if (!buf) {
        return std::format("<unregistered-buffer:{}>@{}..{}",
                           buffer.v, span.start(), span.end());
    }
    const auto s = buf->lineCol(span.start());
    const auto e = buf->lineCol(span.end());
    return std::format("{}:{}-{}:{}", s.line, s.column, e.line, e.column);
}

// Render one diagnostic: the code name + primary span + (if any) the
// `related=[…]` locations (which carry their OWN positions — a position
// regression in a related location must flip the golden too).
[[nodiscard]] std::string renderDiagnostic(ParseDiagnostic const& d,
                                           BufferRegistry const&  bufs) {
    std::string out = std::format("{} {}",
                                  diagnosticCodeName(d.code),
                                  renderSpan(d.buffer, d.span, bufs));
    if (!d.related.empty()) {
        out += " related=[";
        for (std::size_t i = 0; i < d.related.size(); ++i) {
            if (i > 0) out += ',';
            out += renderSpan(d.related[i].buffer, d.related[i].span, bufs);
        }
        out += ']';
    }
    return out;
}

// Build the CU for one malformed corpus file, collect EVERY diagnostic
// (parser/lexer from the trees + semantic from `analyze()`), and render
// them to the sorted golden text. `analyze()` is run UNCONDITIONALLY,
// including on a parse-error tree: the analyzer is defensive on partial
// trees (it guards `!tree.root().valid()`, bounds-checks every child
// descent, and skips trivia), so this is safe + deterministic — and it
// is REQUIRED, because an `S_*` corpus file with a recovered `P_*` would
// otherwise drop its semantic coverage.
[[nodiscard]] std::string renderDiagnosticGolden(std::string_view langName,
                                                 fs::path const&  sourceFile) {
    auto loaded = GrammarSchema::loadShipped(langName);
    if (!loaded.has_value()) {
        ADD_FAILURE() << "loadShipped(\"" << langName << "\") failed for "
                      << sourceFile.string();
        return {};
    }
    const std::string source = dss::test_support::readFile(sourceFile);

    UnitBuilder builder{*loaded};
    builder.addInMemory(source, sourceFile.filename().string());
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());

    // BufferId -> source buffer, for line:col resolution (reuses the
    // Part-A registry + `Tree::sourceShared()`).
    BufferRegistry bufs;
    for (auto const& tree : cu->trees()) {
        if (auto s = tree.sourceShared()) bufs.add(std::move(s));
    }

    std::vector<std::string> lines;
    for (auto const& tree : cu->trees()) {
        for (auto const& d : tree.diagnostics().all()) {
            lines.push_back(renderDiagnostic(d, bufs));
        }
    }
    SemanticModel const model = analyze(cu);
    for (auto const& d : model.diagnostics().all()) {
        lines.push_back(renderDiagnostic(d, bufs));
    }

    // A corpus file that produces ZERO diagnostics is an authoring
    // mistake (it belongs in the clean-parse corpus, not here) — fail
    // loud rather than pin an empty golden that proves nothing.
    if (lines.empty()) {
        ADD_FAILURE() << "diagnostic-corpus file produced NO diagnostics: "
                      << sourceFile.string()
                      << " (a malformed-source corpus file MUST emit ≥1)";
    }

    // Total order over the exact rendered bytes — deterministic across
    // runs/platforms even for two diagnostics at the same position.
    std::ranges::sort(lines);

    std::string out;
    for (auto const& l : lines) { out += l; out += '\n'; }
    return out;
}

} // namespace

// One data-driven test: discover every `tests/corpus/diagnostics/<lang>/`
// subdir (the dir name = the shipped-schema name) and drive every
// non-`.diag` source file through the harness against its `<file>.diag`
// golden. Zero dirs / zero files / a missing golden all FAIL LOUD.
TEST(DiagnosticCorpus, EveryMalformedFilePinsCodesAndPositions) {
    const fs::path root = dss::test_support::findCorpusRoot() / "diagnostics";
    ASSERT_TRUE(fs::is_directory(root))
        << "missing diagnostic-corpus root " << root.string();

    // Deterministic order: sort discovered dirs + files.
    std::vector<fs::path> langDirs;
    for (auto const& e : fs::directory_iterator{root}) {
        if (e.is_directory()) langDirs.push_back(e.path());
    }
    std::ranges::sort(langDirs);
    ASSERT_FALSE(langDirs.empty())
        << "no language subdirs under " << root.string()
        << " — the harness must test something";

    std::size_t filesChecked = 0;
    for (auto const& dir : langDirs) {
        const std::string lang = dir.filename().string();
        std::vector<fs::path> sources;
        for (auto const& e : fs::directory_iterator{dir}) {
            if (e.is_regular_file() && e.path().extension() != ".diag") {
                sources.push_back(e.path());
            }
        }
        std::ranges::sort(sources);
        for (auto const& src : sources) {
            SCOPED_TRACE(src.string());
            const std::string actual = renderDiagnosticGolden(lang, src);
            dss::test_support::checkGoldenText(
                actual, fs::path{src.string() + ".diag"});
            ++filesChecked;
        }
        // Orphan-golden guard: a `.diag` whose source was renamed/deleted
        // must not silently rot (it would no longer be pinned by any
        // source). Every `.diag` must have a live source sibling.
        for (auto const& e : fs::directory_iterator{dir}) {
            if (e.is_regular_file() && e.path().extension() == ".diag") {
                const fs::path srcSibling = dir / e.path().stem();  // strip ".diag"
                EXPECT_TRUE(fs::exists(srcSibling))
                    << "orphan golden (no source sibling): " << e.path().string();
            }
        }
    }
    EXPECT_GT(filesChecked, 0u)
        << "discovered language dirs but ZERO source files — nothing pinned";
}
