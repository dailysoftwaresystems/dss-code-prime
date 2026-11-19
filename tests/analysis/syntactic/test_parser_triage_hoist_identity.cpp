// ===========================================================================
// P69 L2b2 (lane `lm`) — THE TRIAGE'S BASE ASKED BEFORE A PROBE CHANGES NO PARSE.
//
// THE PROPERTY. The parser refutes a speculative candidate WITHOUT opening its
// probe when the probe would be refuted by the type-name triage's early base ask
// anyway: a type child entered after a fixed run of single-token elements, whose
// base is a name the binder sketch knows as a VALUE
// (`Parser::Impl::hoistRefutesTheCandidate_`). That is a saving, never a
// decision: every unit of every shipped language parses to the SAME trees with
// the SAME diagnostics, byte for byte, with the hoist on and off. The switch
// (`UnitBuilder::setTriageBaseHoistForTheIdentityWitness`) is read from no
// language document and no CLI flag, and this file is its one caller (a
// repo-guard scan pins that, `test_triage_hoist_switch_has_one_caller`). Off,
// every candidate takes its probe — the path every candidate the hoist does not
// refute takes in every run, not a second parser.
//
// THE UNIVERSE (each language's units counted, and none may be empty):
//   * c: tests/corpus/c, tests/corpus/diagnostics/c, and every `.c` under
//     examples/c (recursively);
//   * toy, tsql-subset: their tests/corpus and tests/corpus/diagnostics files;
//   * asm-x86_64-att, asm-arm64-gas: every `.s` under examples/asm, under BOTH
//     dialects (the identity holds for any pair of input and grammar, a refused
//     one included). The asm core document is not standalone-loadable by design
//     (its header says why), so it is not a third grammar here.
// A unit that fails to preprocess or parse is compared all the same: the claim
// is identity, not acceptance.
//
// NOT VACUOUS. The switch must reach the parser: summed over the c units, the
// unit's own probe count (`CompilationUnit::speculationProbeCount`) is STRICTLY
// larger with the hoist off. Without that, identity would hold trivially for a
// switch that is never forwarded.
//
// RED-ON-DISABLE (tests/mutations/arms.registry):
//   * the hoist ignores the sketch's answer (refutes every identifier-based
//     candidate) — a typedef-led declaration `T x;` reads as an expression: this
//     test reds (and the hoist pins in test_parser_triage_hoist.cpp);
//   * the switch is not forwarded by `parserConfigFor` — the probe counts are
//     equal and the non-vacuity case reds.
// ⚠ MUST run through ctest: the shipped documents, the corpus and the examples
// are found through the repo root.
// ===========================================================================

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/scope_trail.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "core/substrate/path_identity.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace dss;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::shared_ptr<GrammarSchema const> shipped(std::string_view name) {
    auto loaded = GrammarSchema::loadShipped(name);
    EXPECT_TRUE(loaded.has_value()) << "the shipped language document " << name;
    return loaded.has_value() ? *loaded : nullptr;
}

// Every file under `dir` (recursively; a link is never descended) whose
// extension is one of `exts`, sorted, so the order of the comparison — and of a
// failure's report — is fixed. A walk that could not read the whole tree fails
// the case: the universe would be smaller than it says.
[[nodiscard]] std::vector<fs::path> filesUnder(fs::path const& dir,
                                               std::vector<std::string> const& exts) {
    std::vector<fs::path> out;
    auto const walk = dss::core::walkTree(dir, [&](dss::core::TreeEntry const& e) {
        if (e.kind == dss::core::EntryKind::File) {
            fs::path const p{e.at.spelling().generic()};
            if (std::find(exts.begin(), exts.end(), p.extension().string()) != exts.end()) out.push_back(p);
        }
        return dss::core::WalkStep::Continue;
    });
    EXPECT_FALSE(walk.error) << "the walk of " << dir.generic_string() << " stopped at "
                             << walk.where.spelling().generic() << ": " << walk.error.message();
    std::sort(out.begin(), out.end());
    return out;
}

// One diagnostic, every field a reader can see except the buffer id (assigned per
// builder, so two builders of one file may number it differently; the span's
// offsets and the unit's tree order already place it).
void renderDiagnostic(std::string& out, ParseDiagnostic const& d) {
    out += "  ";
    out += std::to_string(static_cast<int>(d.code));
    out += " sev=" + std::to_string(static_cast<int>(d.severity));
    out += " @" + std::to_string(static_cast<std::uint64_t>(d.span.start())) + ":"
         + std::to_string(static_cast<std::uint64_t>(d.span.end()));
    out += " rule=" + (d.ruleContext ? std::to_string(d.ruleContext->v) : std::string{"-"});
    out += " expected=[";
    for (auto const& e : d.expected) out += e + "|";
    out += "] actual=" + d.actual;
    out += " scopes=[";
    for (ScopeKind const k : d.scopeStack.kinds()) out += std::to_string(static_cast<int>(k)) + ",";
    out += "] related=[";
    for (auto const& r : d.related) {
        out += std::to_string(static_cast<std::uint64_t>(r.span.start())) + ":" + r.note + "|";
    }
    out += "] hint=" + d.suggestion + "\n";
}

// A flat, id-independent rendering of a whole unit: every tree node for node
// (kind, flags, span, parent, rule or token kind, children), every tree's
// diagnostics, then the driver's. NodeIds carry their tree's arena tag, so only
// an id's INDEX is printed.
[[nodiscard]] std::string renderUnit(CompilationUnit const& cu) {
    std::string out;
    out += "trees=" + std::to_string(cu.trees().size()) + "\n";
    for (Tree const& t : cu.trees()) {
        out += "tree root=" + std::to_string(t.root().v) + " nodes="
             + std::to_string(t.nodeCount()) + "\n";
        for (std::uint32_t i = 1;
             t.root().valid() && i < static_cast<std::uint32_t>(t.nodeCount()); ++i) {
            NodeId const id{i, t.root().arenaTag};
            out += "#" + std::to_string(i);
            out += " kind=" + std::to_string(static_cast<int>(t.kind(id)));
            out += " flags=" + std::to_string(static_cast<std::uint32_t>(t.flags(id)));
            out += " span=" + std::to_string(static_cast<std::uint64_t>(t.span(id).start())) + ":"
                 + std::to_string(static_cast<std::uint64_t>(t.span(id).end()));
            out += " parent=" + std::to_string(t.parent(id).v);
            if (t.kind(id) == NodeKind::Internal) {
                out += " rule=" + std::to_string(t.rule(id).v);
            } else if (t.kind(id) == NodeKind::Token) {
                out += " tok=" + std::to_string(t.tokenKind(id).v);
            }
            out += " kids=[";
            for (auto c : t.children(id)) out += std::to_string(c.v) + ",";
            out += "]\n";
        }
        out += "diags:\n";
        for (auto const& d : t.diagnostics().all()) renderDiagnostic(out, d);
    }
    out += "driver:\n";
    for (auto const& d : cu.driverDiagnostics().all()) renderDiagnostic(out, d);
    return out;
}

struct Built {
    std::string   rendering;
    std::uint64_t probes = 0;
};

[[nodiscard]] Built build(std::shared_ptr<GrammarSchema const> const& schema,
                          fs::path const& file, bool hoist) {
    UnitBuilder b{schema, DiagnosticBudget::libraryDefault()};
    if (!hoist) b.setTriageBaseHoistForTheIdentityWitness(false);
    b.addFile(file);
    CompilationUnit cu = std::move(b).finish();
    return Built{renderUnit(cu), cu.speculationProbeCount()};
}

// The first line on which two renderings differ, both sides quoted.
[[nodiscard]] std::string firstDifference(std::string const& on, std::string const& off) {
    std::size_t line = 1, i = 0;
    while (i < on.size() && i < off.size() && on[i] == off[i]) {
        if (on[i] == '\n') ++line;
        ++i;
    }
    auto lineAt = [](std::string const& s, std::size_t at) {
        std::size_t const b = s.rfind('\n', at == 0 ? 0 : at - 1);
        std::size_t const from = (b == std::string::npos) ? 0 : b + 1;
        std::size_t const e = s.find('\n', from);
        return s.substr(from, (e == std::string::npos ? s.size() : e) - from);
    };
    return "line " + std::to_string(line) + "\n  on : " + lineAt(on, i) + "\n  off: " + lineAt(off, i);
}

struct Tally {
    std::size_t   units = 0;
    std::uint64_t probesOn = 0;
    std::uint64_t probesOff = 0;
};

void compareAll(std::string_view language, std::vector<fs::path> const& files, Tally& tally) {
    auto const schema = shipped(language);
    ASSERT_NE(schema, nullptr);
    for (fs::path const& f : files) {
        Built const on  = build(schema, f, /*hoist=*/true);
        Built const off = build(schema, f, /*hoist=*/false);
        ++tally.units;
        tally.probesOn  += on.probes;
        tally.probesOff += off.probes;
        EXPECT_LE(on.probes, off.probes) << language << " " << f.string()
                                         << ": the hoist OPENED a probe the run without it did not";
        if (on.rendering != off.rendering) {
            ADD_FAILURE() << language << " " << f.string()
                          << ": the hoist changed the unit at " << firstDifference(on.rendering, off.rendering);
        }
    }
}

} // namespace

TEST(ParserTriageHoistIdentity, EveryCUnitIsTheSameWithTheHoistOnAndOff) {
    fs::path const root = dss::test::repoRoot();
    std::vector<fs::path> files = filesUnder(root / "tests/corpus/c", {".c"});
    std::size_t const corpus = files.size();
    std::vector<fs::path> const diag = filesUnder(root / "tests/corpus/diagnostics/c", {".c"});
    std::vector<fs::path> const examples = filesUnder(root / "examples/c", {".c"});
    ASSERT_GT(corpus, 0u) << "tests/corpus/c holds no unit";
    ASSERT_GT(diag.size(), 0u) << "tests/corpus/diagnostics/c holds no unit";
    ASSERT_GT(examples.size(), 0u) << "examples/c holds no unit";
    files.insert(files.end(), diag.begin(), diag.end());
    files.insert(files.end(), examples.begin(), examples.end());
    Tally tally;
    compareAll("c", files, tally);
    EXPECT_EQ(tally.units, files.size());
    // NOT VACUOUS: the switch reached the parser, and the hoist refuted something.
    EXPECT_LT(tally.probesOn, tally.probesOff)
        << "the c units opened " << tally.probesOn << " probes with the hoist and " << tally.probesOff
        << " without: the switch does not reach the parser, or the hoist refutes nothing";
    std::printf("c: %zu units, probes on %llu, off %llu\n", tally.units,
                static_cast<unsigned long long>(tally.probesOn),
                static_cast<unsigned long long>(tally.probesOff));
}

TEST(ParserTriageHoistIdentity, EveryToyAndTsqlUnitIsTheSameWithTheHoistOnAndOff) {
    fs::path const root = dss::test::repoRoot();
    struct Lang {
        char const*              name;
        char const*              dir;
        std::vector<std::string> exts;
    };
    for (Lang const& l : {Lang{"toy", "toy", {".toy"}}, Lang{"tsql-subset", "tsql-subset", {".sql", ".tsql"}}}) {
        SCOPED_TRACE(l.name);
        std::vector<fs::path> files = filesUnder(root / "tests/corpus" / l.dir, l.exts);
        std::vector<fs::path> const diag = filesUnder(root / "tests/corpus/diagnostics" / l.dir, l.exts);
        ASSERT_GT(files.size(), 0u) << "tests/corpus/" << l.dir << " holds no unit";
        ASSERT_GT(diag.size(), 0u) << "tests/corpus/diagnostics/" << l.dir << " holds no unit";
        files.insert(files.end(), diag.begin(), diag.end());
        Tally tally;
        compareAll(l.name, files, tally);
        EXPECT_EQ(tally.units, files.size());
    }
}

TEST(ParserTriageHoistIdentity, EveryAsmUnitIsTheSameUnderBothDialectsWithTheHoistOnAndOff) {
    fs::path const root = dss::test::repoRoot();
    std::vector<fs::path> const files = filesUnder(root / "examples/asm", {".s", ".S"});
    ASSERT_GT(files.size(), 0u) << "examples/asm holds no unit";
    for (char const* dialect : {"asm-x86_64-att", "asm-arm64-gas"}) {
        SCOPED_TRACE(dialect);
        Tally tally;
        compareAll(dialect, files, tally);
        EXPECT_EQ(tally.units, files.size());
    }
}
