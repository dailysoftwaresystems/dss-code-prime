// OPT2 cycle 1 — `loadPipelineFromText` + `loadShippedPipeline` tests.
//
// Pins the D-OPT1-PIPELINE-FROM-CONFIG 7-step shape:
//   parse → version → required → optional → enum-resolve → validate → return
// + the D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD contract (unknown
// sub-keys reject loud, not silently load with defaults).
//
// P10 (2026-08-18) — the schedule-tree grammar: `passes` elements are
// pass-name leaves or {"repeat":{count,passes}} / {"fixpoint":{max,
// passes}} nodes; flat documents desugar at load to ONE top-level
// fixpoint; one spelling per document; load-time budgets (count/max
// bounds, depth, total worst-case invocations) all fail loud with
// X_PipelineMalformed.

#include "core/types/parse_diagnostic.hpp"
#include "opt/optimizer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

bool hasCode(std::vector<ConfigDiagnostic> const& diags, DiagnosticCode code) {
    return std::any_of(diags.begin(), diags.end(),
        [code](ConfigDiagnostic const& d) { return d.code == code; });
}

// Message-substring witness — the refusal pins below assert BOTH the
// code (hasCode) and the diagnostic's own words, so a refactor that
// keeps the code but blurs the remediation (e.g. stops naming both
// spellings) still reds.
bool msgHas(std::vector<ConfigDiagnostic> const& diags,
            std::string_view needle) {
    return std::any_of(diags.begin(), diags.end(),
        [needle](ConfigDiagnostic const& d) {
            return d.message.find(needle) != std::string::npos;
        });
}

// Leaf pass sequence in interpreter (depth-first, left-to-right) order
// — the loader-test twin of the engine's walk. For the single-root
// schedules the shipped docs normalize to, this is exactly the
// document's flat pass list.
std::vector<opt::PassId>
leafSequence(opt::OptPipelineNode const& n) {
    std::vector<opt::PassId> out;
    switch (n.kind) {
    case opt::OptPipelineNode::Kind::Leaf:
        out.push_back(n.passId);
        break;
    case opt::OptPipelineNode::Kind::Repeat:
    case opt::OptPipelineNode::Kind::Fixpoint:
        for (auto const& c : n.children) {
            auto sub = leafSequence(c);
            out.insert(out.end(), sub.begin(), sub.end());
        }
        break;
    }
    return out;
}

} // namespace

// Shipped `debug.pipeline.json` loads cleanly + resolves the Identity
// pass. This is the end-to-end sanity pin — fail here means the JSON
// file's shape drifted from the loader. P10: the shipped doc now uses
// the STRUCTURAL spelling (fixpoint{max:1,[Identity]}), which the
// loader normalizes to root Fixpoint{1,[Identity]} — desugar-identical
// to the pre-P10 flat `["Identity"]` document.
TEST(PipelineLoader, ShippedDebugLoadsIdentity) {
    auto r = opt::loadShippedPipeline("debug");
    ASSERT_TRUE(r.has_value()) << "shipped debug.pipeline.json failed to load";
    EXPECT_EQ(r->name, "debug");
    EXPECT_EQ(r->schedule,
              opt::OptPipelineNode::fixpoint(1, {opt::OptPipelineNode::leaf(
                                                     opt::PassId::Identity)}))
        << "debug must desugar to Fixpoint{1, [Identity]} — identical to "
           "the pre-P10 flat document's schedule";
}

// Shipped `release.pipeline.json` declares
// [Identity, Inlining, ConstFold, Mem2Reg, CopyProp, Cse, Licm,
// SimplifyCfg, Dce] under a top-level fixpoint{max:4}.
// Inlining runs EARLY (index 1, right after Identity, before
// ConstFold) so the inlined callee bodies flow through the entire
// downstream battery — ConstFold folds now-constant arguments,
// Mem2Reg promotes any spliced allocas, CSE/LICM/SimplifyCFG/DCE
// clean up — and the fixpoint{max:4} loop re-optimizes (OPT7 cycle
// 28: inlining shipped, bounded by the size threshold).
// LICM sits AFTER CSE (canonicalized SSA graph) and BEFORE SimplifyCFG
// (hoisting unconditional defs into the preheader may expose new
// constant-condition CondBrs the SimplifyCFG pass can fold).
//
// EXACT-SCHEDULE pin (P10): the structural doc must desugar to EXACTLY
// today's schedule — one Fixpoint{4} over the 9 passes in this order —
// which is exactly what the pre-P10 flat document
// (`passes:[...9...], maxIterations:4`) desugared to. The top-level
// `maxIterations` key is GONE from the doc (one-spelling rule).
TEST(PipelineLoader, ShippedReleaseLoadsAllPasses) {
    auto r = opt::loadShippedPipeline("release");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->name, "release");
    ASSERT_EQ(r->schedule.kind, opt::OptPipelineNode::Kind::Fixpoint);
    EXPECT_EQ(r->schedule.count, 4u);
    auto const seq = leafSequence(r->schedule);
    ASSERT_EQ(seq.size(), 9u);
    EXPECT_EQ(seq[0], opt::PassId::Identity);
    EXPECT_EQ(seq[1], opt::PassId::Inlining);
    EXPECT_EQ(seq[2], opt::PassId::ConstFold);
    EXPECT_EQ(seq[3], opt::PassId::Mem2Reg);
    EXPECT_EQ(seq[4], opt::PassId::CopyProp);
    EXPECT_EQ(seq[5], opt::PassId::Cse);
    EXPECT_EQ(seq[6], opt::PassId::Licm);
    EXPECT_EQ(seq[7], opt::PassId::SimplifyCfg);
    EXPECT_EQ(seq[8], opt::PassId::Dce);
    EXPECT_EQ(r->inlineThreshold, 50u);
    // All nine are DIRECT children of the root fixpoint (the collapse
    // rule — no wrapper fixpoint{1} around the doc's single fixpoint).
    ASSERT_EQ(r->schedule.children.size(), 9u);
    for (auto const& c : r->schedule.children) {
        EXPECT_EQ(c.kind, opt::OptPipelineNode::Kind::Leaf);
    }
}

// maxIterations bounds (D-OPT-FIXED-POINT-LOOP), flat spelling:
// rejected when 0 (silent-no-op trap) or > kMaxPipelineIterations
// (non-convergence signal).
TEST(PipelineLoader, MaxIterationsZeroRejects) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": ["Identity"], "maxIterations": 0}})",
        "max-iter-zero.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
}

TEST(PipelineLoader, MaxIterationsOverflowRejects) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": ["Identity"], "maxIterations": 999}})",
        "max-iter-overflow.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
}

TEST(PipelineLoader, MaxIterationsNonIntegerRejects) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": ["Identity"], "maxIterations": "many"}})",
        "max-iter-string.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
}

// Missing maxIterations on a FLAT document desugars to Fixpoint{1}.
TEST(PipelineLoader, MaxIterationsMissingDefaultsToOne) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": ["Identity"]}})",
        "no-max-iter.json");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->schedule,
              opt::OptPipelineNode::fixpoint(
                  1, {opt::OptPipelineNode::leaf(opt::PassId::Identity)}));
}

// ── P10: the schedule-tree grammar ─────────────────────────────────────

// ★ THE DESUGARING PIN: a flat document and its structural twin load
// to the IDENTICAL tree. The flat twin carries top-level
// maxIterations:4; the structural twin spells the same loop as
// {"fixpoint":{"max":4,...}} and MUST NOT combine it with maxIterations
// (one spelling). This is the load-side half of the sequence-identity
// proof — the desugaring maps the loop nest (iteration × passes) 1:1.
TEST(PipelineLoader, FlatAndStructuralTwinsLoadIdenticalTrees) {
    auto flat = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "twin", "passes": ["ConstFold", "Dce"],
             "maxIterations": 4}})",
        "twin-flat.json");
    auto structural = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "twin", "passes": [
               {"fixpoint": {"max": 4, "passes": ["ConstFold", "Dce"]}}]}})",
        "twin-structural.json");
    ASSERT_TRUE(flat.has_value());
    ASSERT_TRUE(structural.has_value());
    EXPECT_EQ(flat->schedule, structural->schedule)
        << "the structural spelling MUST collapse to the flat spelling's "
           "root fixpoint — otherwise the two documents would not be "
           "desugar-identical and the byte-identity battery would diverge";
    EXPECT_EQ(flat->name, structural->name);
    EXPECT_EQ(flat->inlineThreshold, structural->inlineThreshold);
    EXPECT_EQ(flat->verifyEveryPass, structural->verifyEveryPass);

    opt::OptPipelineNode const expected =
        opt::OptPipelineNode::fixpoint(4, {opt::OptPipelineNode::leaf(
                                               opt::PassId::ConstFold),
                                           opt::OptPipelineNode::leaf(
                                               opt::PassId::Dce)});
    EXPECT_EQ(flat->schedule, expected);
    EXPECT_EQ(structural->schedule, expected);
}

// A MIXED structural document (leaves + a node, or a lone repeat)
// normalizes to root Fixpoint{1, [elements]} — the wrap case.
TEST(PipelineLoader, StructuralDocWrapsInUnitFixpointRoot) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "wrap", "passes": [
               "Identity",
               {"repeat": {"count": 2, "passes": ["ConstFold"]}},
               {"fixpoint": {"max": 3, "passes": ["Dce", "Cse"]}}]}})",
        "wrap.json");
    ASSERT_TRUE(r.has_value());
    opt::OptPipelineNode const expected = opt::OptPipelineNode::fixpoint(
        1, {opt::OptPipelineNode::leaf(opt::PassId::Identity),
            opt::OptPipelineNode::repeat(
                2, {opt::OptPipelineNode::leaf(opt::PassId::ConstFold)}),
            opt::OptPipelineNode::fixpoint(
                3, {opt::OptPipelineNode::leaf(opt::PassId::Dce),
                    opt::OptPipelineNode::leaf(opt::PassId::Cse)})});
    EXPECT_EQ(r->schedule, expected);
}

// NESTED structural nodes parse recursively.
TEST(PipelineLoader, NestedNodesParse) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "nested", "passes": [
               {"fixpoint": {"max": 2, "passes": [
                   "ConstFold",
                   {"repeat": {"count": 3, "passes": ["Cse", "Dce"]}}]}}]}})",
        "nested.json");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->schedule,
              opt::OptPipelineNode::fixpoint(
                  2, {opt::OptPipelineNode::leaf(opt::PassId::ConstFold),
                      opt::OptPipelineNode::repeat(
                          3, {opt::OptPipelineNode::leaf(opt::PassId::Cse),
                              opt::OptPipelineNode::leaf(opt::PassId::Dce)})}));
}

// ★ ONE SPELLING PER DOCUMENT: a structural document REFUSES top-level
// maxIterations — X_PipelineMalformed naming BOTH spellings.
TEST(PipelineLoader, MaxIterationsBesideStructuralNodeRejects) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "both-spellings", "maxIterations": 4, "passes": [
               {"fixpoint": {"max": 2, "passes": ["Identity"]}}]}})",
        "both-spellings.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
    EXPECT_TRUE(msgHas(r.error(), "maxIterations"));
    EXPECT_TRUE(msgHas(r.error(), "fixpoint"))
        << "the refusal must name BOTH spellings so the remediation is "
           "actionable, not just 'malformed'";
    // A `repeat` node triggers the same one-spelling refusal.
    auto r2 = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "both-spellings-repeat", "maxIterations": 4, "passes": [
               {"repeat": {"count": 2, "passes": ["Identity"]}}]}})",
        "both-spellings-repeat.json");
    ASSERT_FALSE(r2.has_value());
    EXPECT_TRUE(hasCode(r2.error(), DiagnosticCode::X_PipelineMalformed));
    EXPECT_TRUE(msgHas(r2.error(), "maxIterations"));
    EXPECT_TRUE(msgHas(r2.error(), "repeat"));
}

// ★ UNKNOWN NODE KIND: an object element declaring neither repeat nor
// fixpoint (seq / when / parallel do not exist — the grammar is CLOSED)
// → X_PipelineMalformed naming the two legal node kinds.
TEST(PipelineLoader, UnknownNodeKindRejects) {
    for (std::string_view kind : {"seq", "when", "parallel"}) {
        // The fixture must be WELL-FORMED JSON — a broken document fails
        // at the json parse step with a different code/message and this
        // test would pass its ASSERTs for the wrong reason. Braces: inner
        // passes [], the node-value object, the node object, the top
        // passes array, pipeline, document.
        auto r = opt::loadPipelineFromText(
            std::string{"{\"dssPipelineVersion\": 1, \"pipeline\": "
                        "{\"name\": \"x\", \"passes\": [\"Identity\", {\""}
                   + kind.data() + std::string{"\": {\"passes\": [\"Dce\"]}}]}}"},
            "unknown-node.json");
        ASSERT_FALSE(r.has_value()) << "node kind '" << kind << "'";
        EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
        EXPECT_TRUE(msgHas(r.error(), "unknown pipeline node kind"))
            << "node kind '" << kind << "'";
    }
}

// A node object declaring BOTH repeat and fixpoint → refusal (exactly
// one of the two).
TEST(PipelineLoader, BothNodeKindsRejects) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": [
               {"repeat": {"count": 2, "passes": ["Identity"]},
                "fixpoint": {"max": 2, "passes": ["Dce"]}}]}})",
        "both-kinds.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
    EXPECT_TRUE(msgHas(r.error(), "exactly ONE"));
}

// Unknown keys on/inside a node object → X_PipelineMalformed
// (D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD extends to the node grammar).
TEST(PipelineLoader, UnknownNodeKeyRejects) {
    // sibling key on the node object
    auto r1 = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": [
               {"repeat": {"count": 2, "passes": ["Identity"]},
                "when": "x86_64"}]}})",
        "node-sibling-key.json");
    ASSERT_FALSE(r1.has_value());
    EXPECT_TRUE(hasCode(r1.error(), DiagnosticCode::X_PipelineMalformed));
    // unknown key inside the node body (a per-step pass param would
    // land here — forbidden)
    auto r2 = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": [
               {"fixpoint": {"max": 2, "passes": ["Identity"],
                             "threshold": 9}}]}})",
        "node-body-key.json");
    ASSERT_FALSE(r2.has_value());
    EXPECT_TRUE(hasCode(r2.error(), DiagnosticCode::X_PipelineMalformed));
    EXPECT_TRUE(msgHas(r2.error(), "unknown key"));
}

// Non-string, non-object element → X_PipelineMalformed.
TEST(PipelineLoader, NonStringNonObjectElementRejects) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1,
             "pipeline": { "name": "x", "passes": ["Identity", 42] } })",
        "numeric-element.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
}

// ★ count/max BOUNDS on node objects: 0 and 33 both reject, mirroring
// the flat maxIterations trio.
TEST(PipelineLoader, NodeCountBoundsReject) {
    for (std::string_view v : {"0", "33"}) {
        auto r = opt::loadPipelineFromText(
            std::string{"{\"dssPipelineVersion\": 1, \"pipeline\": "
                        "{\"name\": \"x\", \"passes\": [{\"repeat\": "
                        "{\"count\": "}
                   + v.data() + std::string{", \"passes\": [\"Identity\"]}}]}}"},
            "count-bound.json");
        ASSERT_FALSE(r.has_value()) << "repeat count " << v;
        EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
        EXPECT_TRUE(msgHas(r.error(), "[1, 32]"));
    }
}

TEST(PipelineLoader, NodeMaxBoundsReject) {
    for (std::string_view v : {"0", "33"}) {
        auto r = opt::loadPipelineFromText(
            std::string{"{\"dssPipelineVersion\": 1, \"pipeline\": "
                        "{\"name\": \"x\", \"passes\": [{\"fixpoint\": "
                        "{\"max\": "}
                   + v.data() + std::string{", \"passes\": [\"Identity\"]}}]}}"},
            "max-bound.json");
        ASSERT_FALSE(r.has_value()) << "fixpoint max " << v;
        EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
        EXPECT_TRUE(msgHas(r.error(), "[1, 32]"));
    }
}

// ★ DEPTH BUDGET: combinator nesting ≤ 8 (the normalized root
// fixpoint counts as 1). Root + 7 nested repeats = depth 8 (accepted);
// root + 8 = depth 9 (rejected) — both sides of the boundary pinned.
namespace {
// {"repeat":{"count":1,"passes":[ <inner> ]}} — `inner` is either a
// leaf list ("Identity") or another nesting level.
std::string nestedRepeats(unsigned levels) {
    std::string doc =
        "{\"dssPipelineVersion\": 1, \"pipeline\": {\"name\": \"d\", "
        "\"passes\": [";
    for (unsigned i = 0; i < levels; ++i) {
        doc += "{\"repeat\":{\"count\":1,\"passes\":[";
    }
    doc += "\"Identity\"";
    for (unsigned i = 0; i < levels; ++i) {
        doc += "]}}";
    }
    doc += "]}}";
    return doc;
}
} // namespace

TEST(PipelineLoader, DepthBudgetRejectsNineLevels) {
    auto nine = opt::loadPipelineFromText(nestedRepeats(8), "depth-9.json");
    ASSERT_FALSE(nine.has_value());
    EXPECT_TRUE(hasCode(nine.error(), DiagnosticCode::X_PipelineMalformed));
    EXPECT_TRUE(msgHas(nine.error(), "nesting depth"));

    auto eight = opt::loadPipelineFromText(nestedRepeats(7), "depth-8.json");
    ASSERT_TRUE(eight.has_value())
        << "depth 8 (root + 7 nested combinators) is INSIDE the budget — "
           "rejecting it would tighten the rule without a ruling";
}

// ★ TOTAL-BUDGET OVERFLOW via nested repeats: repeat(32,[repeat(32,
// [8 passes])]) unrolls to 32×32×8 = 8192 > 4096 worst-case
// invocations → rejected; a within-budget twin loads.
TEST(PipelineLoader, InvocationBudgetOverflowRejects) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": [
               {"repeat": {"count": 32, "passes": [
                 {"repeat": {"count": 32, "passes": [
                   "Identity","Identity","Identity","Identity",
                   "Identity","Identity","Identity","Identity"]}}]}}]}})",
        "budget-overflow.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
    EXPECT_TRUE(msgHas(r.error(), "4096"));

    // Within budget: repeat(2,[fixpoint(4,[3 passes])]) = 24.
    auto ok = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "y", "passes": [
               {"repeat": {"count": 2, "passes": [
                 {"fixpoint": {"max": 4, "passes": ["Identity","Dce","Cse"]}}]}}]}})",
        "budget-ok.json");
    ASSERT_TRUE(ok.has_value());
}

// ★ EMPTY BODIES inside nodes reject (existing empty-pipeline
// discipline, extended to every node — an empty body would run
// nothing). Includes the NESTED-empty shape.
TEST(PipelineLoader, EmptyNodeBodyRejects) {
    auto r1 = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": [
               {"repeat": {"count": 2, "passes": []}}]}})",
        "empty-repeat.json");
    ASSERT_FALSE(r1.has_value());
    EXPECT_TRUE(hasCode(r1.error(), DiagnosticCode::X_PipelineMalformed));
    EXPECT_TRUE(msgHas(r1.error(), "at least one entry"));

    auto r2 = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": [
               {"fixpoint": {"max": 3, "passes": ["Identity",
                 {"repeat": {"count": 1, "passes": []}}]}}]}})",
        "empty-nested.json");
    ASSERT_FALSE(r2.has_value());
    EXPECT_TRUE(hasCode(r2.error(), DiagnosticCode::X_PipelineMalformed));
    EXPECT_TRUE(msgHas(r2.error(), "at least one entry"))
        << "an empty body nested inside another node's body must reject "
           "too — the check is per-node, not only top-level";
}

// A fixpoint node with an unknown pass name in its body →
// X_UnknownPassName with node-path context.
TEST(PipelineLoader, UnknownPassNameInsideNodeRejects) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": [
               {"fixpoint": {"max": 2, "passes": ["MadeUpPass"]}}]}})",
        "unknown-in-node.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_UnknownPassName));
}

// inlineThreshold bounds (OPT7 cycle 28 cost model) — MIRRORS the
// maxIterations trio. Rejected when 0 (silent refuse-all trap) or
// > kMaxInlineThreshold; non-integer rejects; missing defaults to
// kDefaultInlineThreshold; a valid in-range value parses.
TEST(PipelineLoader, InlineThresholdZeroRejects) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": ["Identity"], "inlineThreshold": 0}})",
        "inline-thresh-zero.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
}

TEST(PipelineLoader, InlineThresholdOverflowRejects) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": ["Identity"], "inlineThreshold": 100001}})",
        "inline-thresh-overflow.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
}

TEST(PipelineLoader, InlineThresholdNonIntegerRejects) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": ["Identity"], "inlineThreshold": "big"}})",
        "inline-thresh-string.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
}

TEST(PipelineLoader, InlineThresholdDefaultsWhenAbsent) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": ["Identity"]}})",
        "no-inline-thresh.json");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->inlineThreshold, opt::kDefaultInlineThreshold);
}

TEST(PipelineLoader, InlineThresholdInRange) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": ["Identity"], "inlineThreshold": 7}})",
        "inline-thresh-7.json");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->inlineThreshold, 7u);
}

// SHIP pin (complementary to the exact-list ShippedReleaseLoadsAllPasses
// above): the shipped `release.pipeline.json` CONTAINS `PassId::Inlining`
// — a walk over the loaded tree, robust to future reordering. OPT7
// cycle 28 shipped inlining in release (bounded by inlineThreshold).
// RED-on-disable: removing "Inlining" from release.json makes the
// membership fail this assertion.
TEST(PipelineLoader, ShippedReleaseContainsInlining) {
    auto r = opt::loadShippedPipeline("release");
    ASSERT_TRUE(r.has_value());
    auto const passes = leafSequence(r->schedule);
    EXPECT_NE(std::find(passes.begin(), passes.end(), opt::PassId::Inlining),
              passes.end())
        << "the shipped release pipeline MUST contain Inlining (OPT7 cycle "
           "28 ships inlining, bounded by the inlineThreshold cost model)";
}

// D-OPT1-PASS-DUP-POLICY: a pipeline declaring the SAME pass twice
// (e.g. `[ConstFold, ConstFold]`) is structurally legal. The
// pipeline-level fixed-point loop already lets a pass re-run; declaring the
// duplicate explicitly is the OPT2b "transpile-readable" use case
// where const-fold runs first, peephole rewrites land between, then
// const-fold runs again to fold the rewrites' constants. The loader
// MUST accept this without complaint.
TEST(PipelineLoader, DuplicatePassesInPipelineAreLegal) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "double-const-fold",
             "passes": ["ConstFold", "ConstFold"]}})",
        "dup-passes.json");
    ASSERT_TRUE(r.has_value()) << "duplicate PassIds in a pipeline must load";
    EXPECT_EQ(r->name, "double-const-fold");
    auto const seq = leafSequence(r->schedule);
    ASSERT_EQ(seq.size(), 2u);
    EXPECT_EQ(seq[0], opt::PassId::ConstFold);
    EXPECT_EQ(seq[1], opt::PassId::ConstFold);
}

// Missing version → X_PipelineVersionMismatch. The version gate is the
// load-bearing fence between this build and a future incompatible
// pipeline format.
TEST(PipelineLoader, MissingVersionRejects) {
    auto r = opt::loadPipelineFromText(
        R"({ "pipeline": { "name": "x", "passes": ["Identity"] } })",
        "missing-version.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineVersionMismatch));
}

// Wrong version → X_PipelineVersionMismatch. Future v2+ schema lands
// here.
TEST(PipelineLoader, WrongVersionRejects) {
    auto r = opt::loadPipelineFromText(
        R"({ "dssPipelineVersion": 2, "pipeline": { "name": "x", "passes": [] } })",
        "wrong-version.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineVersionMismatch));
}

// Unknown pass name → X_UnknownPassName. The config-load-time analog
// of X_UnknownPassId. Catches typos + drift between JSON and the
// PassId enum.
TEST(PipelineLoader, UnknownPassNameRejects) {
    auto r = opt::loadPipelineFromText(
        R"({ "dssPipelineVersion": 1,
             "pipeline": { "name": "x", "passes": ["TotallyMadeUp"] } })",
        "unknown-pass.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_UnknownPassName));
}

// Unknown top-level key → X_PipelineMalformed.
// D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD discipline.
TEST(PipelineLoader, UnknownTopKeyRejects) {
    auto r = opt::loadPipelineFromText(
        R"({ "dssPipelineVersion": 1,
             "pipeline": { "name": "x", "passes": [] },
             "junk": "extra" })",
        "extra-key.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
}

// Unknown sub-key under `pipeline` → X_PipelineMalformed.
TEST(PipelineLoader, UnknownPipelineSubKeyRejects) {
    auto r = opt::loadPipelineFromText(
        R"({ "dssPipelineVersion": 1,
             "pipeline": { "name": "x", "passes": [], "extra": 1 } })",
        "extra-subkey.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
}

// Resolution failure: an unknown pipeline name → X_PipelineNameResolutionFailed.
TEST(PipelineLoader, UnknownNameResolutionFails) {
    auto r = opt::loadShippedPipeline("nonexistent-pipeline-xyz");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(),
                        DiagnosticCode::X_PipelineNameResolutionFailed));
}

// Empty `passes` array → X_PipelineMalformed. The optimizer engine
// would run zero passes on an empty pipeline + return ok=true with
// passesRun=0 — silently signalling "ran an optimizer" when nothing
// happened. The loader rejects load-time to prevent this.
TEST(PipelineLoader, EmptyPassesArrayRejects) {
    auto r = opt::loadPipelineFromText(
        R"({ "dssPipelineVersion": 1,
             "pipeline": { "name": "empty", "passes": [] } })",
        "empty-passes.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
}

// Malformed JSON (parse_error catch path) → C_MalformedJson.
TEST(PipelineLoader, MalformedJsonRejects) {
    auto r = opt::loadPipelineFromText(
        R"({ "dssPipelineVersion": 1, "pipeline": )",  // truncated
        "broken.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// Non-string entry in passes array → X_PipelineMalformed (NOT
// X_UnknownPassName — those are distinct remediations). Kept from the
// pre-P10 suite via the non-string/non-object arm.
TEST(PipelineLoader, NonStringPassEntryRejects) {
    auto r = opt::loadPipelineFromText(
        R"({ "dssPipelineVersion": 1,
             "pipeline": { "name": "x", "passes": ["Identity", true] } })",
        "non-string.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
}
