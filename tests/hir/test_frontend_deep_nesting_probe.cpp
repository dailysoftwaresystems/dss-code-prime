// ── THE FRONT-END DEPTH PROBE ────────────────────────────────────────────────
//
// A STAGE-SELECTABLE ceiling instrument for the tokenize → parse → analyze →
// lowerToHir half of the pipeline, driven entirely by the environment so one
// built binary can walk a depth without a rebuild. It is the sibling of the MIR
// tier's probe described in
// D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED,
// and it exists because a ceiling has to be MEASURED per site, before and after,
// on the ORDINARY thread.
//
// ★★ WHY IT PRINTS A MARK PER STAGE. When one of these runs dies it dies by
// EXHAUSTING THE STACK — no `[  FAILED  ]` line, no case name, just a return
// code. The `PROBE-MARK` line printed at the END of each completed stage is what
// turns that into an attribution: the last mark on stdout names the stage that
// SURVIVED, so the death belongs to the next one.
//
// ⚠ TWO INSTRUMENT DEFECTS THE MIR-TIER LANE HIT FIRST AND THIS FILE AVOIDS BY
// CONSTRUCTION:
//   (1) an operator chain written `0+1+1+…` is a CONSTANT EXPRESSION the front
//       end folds to one literal — it measures nothing and reports green. Every
//       chain here starts from a RUNTIME variable.
//   (2) a probe that stops at a stage BEFORE the recursive one reports green for
//       the same reason. So `PROBE-STOP` is asserted by NAME in the transcript,
//       never inferred from the process return code.
//
// ★★ ORDINARY THREAD, ALWAYS. `src/program/program.cpp` builds every CU inside
// `substrate::callOnLargeStack(64 MiB)`, so the identical input measured through
// the CLI reports that everything is fine while a library embedder or an LSP
// crashes. Nothing here calls `callOnLargeStack`, and `analyze` is handed a
// 1 MiB reserve so its own worker cannot hide the thing being measured.
//
// USAGE (through ctest, never a bare .exe):
//   DSS_HS_PROBE_KIND=paren DSS_HS_PROBE_DEPTH=1200 \
//   DSS_HS_PROBE_STOP=hir DSS_HS_PROBE_CAP=100000 \
//   ctest --test-dir build/hs -R hir/test_frontend_deep_nesting_probe -V
// With `DSS_HS_PROBE_KIND` unset the case is a no-op pass, so the ordinary gate
// is unaffected.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/target_schema.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <memory>
#include <string>

using namespace dss;

namespace {

// 1 MiB — the ORDINARY thread's own size. `analyze` otherwise reserves 64 MiB on
// a dedicated worker, which would absorb a per-level recursion under test.
constexpr std::size_t kOrdinaryThreadReserveBytes = std::size_t{1} * 1024 * 1024;

// `analyze` runs its own implementation on a dedicated worker, and `DSS_HS_PROBE_
// ANALYZE_MIB` sizes that worker ALONE. It defaults to the ordinary thread's
// 1 MiB, which is what makes the semantic pass measurable here at all. Raising it
// is legitimate ONLY when the stage under test is a LATER one: `lowerToHir` runs
// on the caller's (ordinary) thread whatever this is set to, so a bigger analyze
// reserve moves the SEMANTIC ceiling out of the way without touching the HIR
// tier's. It can never make a HIR-tier ceiling look better than it is.

[[nodiscard]] std::string envOr(char const* name, std::string fallback) {
    char const* v = std::getenv(name);
    return (v == nullptr || *v == '\0') ? std::move(fallback) : std::string{v};
}

// Flush after every mark: a stack overflow terminates the process without
// running atexit, so a buffered mark would be LOST and the attribution with it.
void mark(std::string const& what) {
    std::fprintf(stdout, "PROBE-MARK %s\n", what.c_str());
    std::fflush(stdout);
}

// ── THE FIXTURES ────────────────────────────────────────────────────────────
// Each names the AXIS its depth follows, because that is what decides whether a
// real corpus can reach it. sqlite's amalgamation nests shallowly (paren 18,
// brace 13, struct-in-struct 4, star-run 3) and LISTS long (1765 initializer
// elements in one brace, 357 statements in one block), so a `leftchain` depth is
// reachable from a real program in a way a `structnest` depth is not.
[[nodiscard]] std::string fixture(std::string const& kind, int d) {
    std::string s;
    if (kind == "paren") {
        // AXIS: paren NESTING. `x` is a runtime variable, so nothing folds.
        s = "int main(void){ int x=0; return ";
        s.append(static_cast<std::size_t>(d), '(');
        s += "x";
        s.append(static_cast<std::size_t>(d), ')');
        s += "; }";
        return s;
    }
    if (kind == "leftchain") {
        // AXIS: LIST LENGTH via a LEFT-ASSOCIATIVE chain — the CST is left-deep,
        // so the leftmost spine is `d` levels. This is the sqlite-shaped axis.
        // Starts from a runtime variable: `x+1+1+…` is NOT constant-folded.
        s = "int main(void){ int x=0; return x";
        for (int i = 0; i < d; ++i) s += "+1";
        s += "; }";
        return s;
    }
    if (kind == "castchain") {
        // AXIS: cast NESTING (a left-deep type-name triage feeder).
        s = "int main(void){ int x=0; return ";
        for (int i = 0; i < d; ++i) s += "(int)";
        s += "x; }";
        return s;
    }
    if (kind == "unknowncast") {
        // AXIS: the LEFTMOST SPINE of a LEFT-ASSOCIATIVE chain, reached through
        // the parser's FC2 type-name commit triage. `a` is an UNKNOWN name, so the
        // triage falls to rule 4 and runs the follower-operator test, which walks
        // the operand subtree's leftmost spine looking for its first leaf. That
        // spine is `d` levels deep for a `d`-term left-assoc chain — the axis a
        // real corpus reaches (sqlite: 1765 elements in one list, 357 statements
        // in one block) while its NESTING stays at 18.
        s = "int main(void){ int x=0; return (a)(x";
        for (int i = 0; i < d; ++i) s += "+1";
        s += "); }";
        return s;
    }
    if (kind == "ptrtype") {
        // AXIS: POINTER TYPE depth (`ptr<ptr<…<i32>…>>`). The star run itself is
        // walked iteratively by the front end, so the depth lands on whatever
        // walks the resulting TYPE.
        s = "int ";
        s.append(static_cast<std::size_t>(d), '*');
        s += "p;\nint main(void){ return 0; }";
        return s;
    }
    if (kind == "structnest") {
        // AXIS: struct-in-struct NESTING (the aggregate TYPE tree).
        for (int i = 0; i < d; ++i) s += "struct S" + std::to_string(i) + " { ";
        s += "int x;";
        for (int i = d - 1; i >= 0; --i) s += " } m" + std::to_string(i) + ";";
        s += "\nstruct S0 g;\nint main(void){ return 0; }";
        return s;
    }
    if (kind == "initnest") {
        // AXIS: initializer BRACE nesting.
        for (int i = 0; i < d; ++i) s += "struct S" + std::to_string(i) + " { ";
        s += "int x;";
        for (int i = d - 1; i >= 0; --i) s += " } m" + std::to_string(i) + ";";
        s += "\nstruct S0 g = ";
        s.append(static_cast<std::size_t>(d), '{');
        s += "3";
        s.append(static_cast<std::size_t>(d), '}');
        s += ";\nint main(void){ return 0; }";
        return s;
    }
    if (kind == "initnestarray") {
        // AXIS: initializer BRACE nesting again, but over ARRAY levels rather
        // than struct levels — `int g[1][1]…[1] = {{{…3…}}}`. It asks the same
        // question of `lowerBraceInit` in ONE declaration instead of `d` of
        // them, which separates the brace lowerer's cost from the cost of
        // parsing `d` struct definitions.
        s = "int g";
        for (int i = 0; i < d; ++i) s += "[1]";
        s += " = ";
        s.append(static_cast<std::size_t>(d), '{');
        s += "3";
        s.append(static_cast<std::size_t>(d), '}');
        s += ";\nint main(void){ return 0; }";
        return s;
    }
    if (kind == "complitnest") {
        // AXIS: brace nesting spelled with a COMPOUND LITERAL at every level —
        // `struct S0 g = (struct S0){ (struct S1){ … } };`. It is the OTHER way
        // a `{` nests, and it does NOT stay inside the brace-init work stack: the
        // compound-literal arm lives in `lowerExpr`, so each level alternates
        // `lowerBraceInit` driver -> `lowerExpr` driver -> `lowerBraceInit`
        // driver. Whether that alternation costs host frames per level is a
        // MEASUREMENT, not a reading, which is what this kind is for.
        // ⚠ File scope cannot take one (a compound literal is not a constant
        // expression), so the initialized object is a LOCAL.
        for (int i = 0; i < d; ++i) s += "struct S" + std::to_string(i) + " { ";
        s += "int x;";
        for (int i = d - 1; i >= 0; --i) s += " } m" + std::to_string(i) + ";";
        s += "\nint main(void){ struct S0 g = ";
        for (int i = 0; i < d; ++i) s += "(struct S" + std::to_string(i) + "){";
        s += "3";
        s.append(static_cast<std::size_t>(d), '}');
        // `g` is deliberately not read: the member chain that would reach the
        // leaf is a SECOND depth axis (`deep_lvalue_chain`'s), and mixing it in
        // would stop this fixture answering about brace nesting alone.
        s += "; return 0; }";
        return s;
    }
    if (kind == "arrayzero") {
        // AXIS: ARRAY-of-array TYPE nesting, reached through the ZERO-FILL path.
        // ONE brace level, `d` array levels — the array analogue of
        // `structzero`, and the CONTROL that separates the brace-nesting
        // recursion `initnestarray` measures from the per-level ARRAY TYPE walk
        // it also drives. An array element type is projected THROUGH by
        // `TypeInterner::representationType` (a nominal struct is not), so this
        // is the shape that isolates that projection.
        s = "int g";
        for (int i = 0; i < d; ++i) s += "[1]";
        s += " = {};\nint main(void){ return 0; }";
        return s;
    }
    if (kind == "structzero") {
        // AXIS: aggregate TYPE nesting, reached through the ZERO-FILL path. One
        // brace level, `d` type levels: every omitted slot is filled by
        // `synthZeroOrError` and assembled by `flattenInitSlot`, so this isolates
        // those two from the brace-NESTING recursion `initnest` measures.
        for (int i = 0; i < d; ++i) s += "struct S" + std::to_string(i) + " { ";
        s += "int x;";
        for (int i = d - 1; i >= 0; --i) s += " } m" + std::to_string(i) + ";";
        s += "\nstruct S0 g = {};\nint main(void){ return 0; }";
        return s;
    }
    if (kind == "initlist") {
        // AXIS: initializer LIST LENGTH inside ONE brace — sqlite's real shape
        // (1765 elements in one `fts5.c` initializer).
        s = "int g[" + std::to_string(d) + "] = {";
        for (int i = 0; i < d; ++i) { if (i) s += ","; s += "1"; }
        s += "};\nint main(void){ return 0; }";
        return s;
    }
    if (kind == "stmtlist") {
        // AXIS: STATEMENT LIST LENGTH inside ONE block (sqlite: 357).
        s = "int main(void){ int x=0; ";
        for (int i = 0; i < d; ++i) s += "x=x+1; ";
        s += "return x; }";
        return s;
    }
    if (kind == "blocknest") {
        // AXIS: block NESTING.
        s = "int main(void){ int x=0; ";
        for (int i = 0; i < d; ++i) s += "{ ";
        s += "x=1;";
        for (int i = 0; i < d; ++i) s += " }";
        s += " return x; }";
        return s;
    }
    if (kind == "fncall") {
        // AXIS: nested CALL depth — the postfix grouped-body arm.
        s = "int f(int a){ return a; }\nint main(void){ int x=0; return ";
        for (int i = 0; i < d; ++i) s += "f(";
        s += "x";
        s.append(static_cast<std::size_t>(d), ')');
        s += "; }";
        return s;
    }
    // ⚠ A THROW, NEVER `abort()`: an abort kills the whole process and every
    // sibling here loses its verdict — the exact signature this file's marks
    // exist to tell APART from a stack overflow (`no_abort_in_tests_guard`
    // enforces the same rule). An unknown kind must never silently measure
    // nothing, so it is loud either way.
    throw std::runtime_error{"probe: unknown DSS_HS_PROBE_KIND '" + kind + "'"};
}

} // namespace

TEST(FrontendDeepNestingProbe, WalksTheConfiguredDepth) {
    std::string const kind = envOr("DSS_HS_PROBE_KIND", "");
    if (kind.empty()) {
        GTEST_SKIP() << "DSS_HS_PROBE_KIND unset — probe idle";
    }
    int const depth = std::atoi(envOr("DSS_HS_PROBE_DEPTH", "100").c_str());
    std::string const stop = envOr("DSS_HS_PROBE_STOP", "hir");
    std::size_t const cap = static_cast<std::size_t>(
        std::atoll(envOr("DSS_HS_PROBE_CAP", "1000000").c_str()));

    mark("BEGIN kind=" + kind + " depth=" + std::to_string(depth)
         + " stop=" + stop + " cap=" + std::to_string(cap));

    auto loaded = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(loaded.has_value());
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    auto t = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(t.has_value());
    std::shared_ptr<TargetSchema const> target = *t;

    auto srcBuf = SourceBuffer::fromString(fixture(kind, depth), "<hsprobe>");
    Tokenizer tk{srcBuf, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    mark("TOKENIZE");
    if (stop == "tokenize") return;

    ParserConfig pcfg;
    pcfg.maxExpressionDepth = cap;
    // `DSS_HS_PROBE_SPEC` lifts the SPECULATION cap (`ParserConfig::
    // maxSpeculationDepth`). It is a separate axis from `maxExpressionDepth`:
    // it counts nested speculative probes, which (since P60) live on the
    // parser's heap `specStack` and are bounded by the MEMORY a probe's
    // checkpoint costs rather than by any host recursion. Left unset the C++
    // fallback of 64 stands — NOT the shipped c value, which only
    // `parserConfigFor` (compilation_unit.cpp) reads from `c.lang.json`; a
    // probe that must see the shipped 2048 sets this explicitly.
    if (std::string const spec = envOr("DSS_HS_PROBE_SPEC", ""); !spec.empty()) {
        pcfg.maxSpeculationDepth =
            static_cast<std::size_t>(std::atoll(spec.c_str()));
    }
    Parser p{srcBuf, schema, std::move(stream), DiagnosticBudget::libraryDefault(),
             std::move(pcfg), std::move(lexDiags)};
    ParseResult result = std::move(p).parse();
    mark("PARSE errors=" + std::to_string(
             result.tree.diagnostics().hasErrors() ? 1 : 0));
    if (stop == "parse") return;

    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addTree(std::move(result.tree));
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel model =
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                LongDoubleFormat::None, target.get(),
                static_cast<std::size_t>(
                    std::atoll(envOr("DSS_HS_PROBE_ANALYZE_MIB", "1").c_str()))
                    * 1024 * 1024);
    mark("ANALYZE errors=" + std::to_string(model.hasErrors() ? 1 : 0));
    if (stop == "analyze") return;

    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);
    mark("HIR errors=" + std::to_string(hirReporter.errorCount())
         + " nodes=" + std::to_string(hir->hir.nodeCount()));
}
