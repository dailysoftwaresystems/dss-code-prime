// OPT2 cycle 1 — `loadPipelineFromText` + `loadShippedPipeline` tests.
//
// Pins the D-OPT1-PIPELINE-FROM-CONFIG 7-step shape:
//   parse → version → required → optional → enum-resolve → validate → return
// + the D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD contract (unknown
// sub-keys reject loud, not silently load with defaults).

#include "core/types/parse_diagnostic.hpp"
#include "opt/optimizer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

using namespace dss;

namespace {

bool hasCode(std::vector<ConfigDiagnostic> const& diags, DiagnosticCode code) {
    return std::any_of(diags.begin(), diags.end(),
        [code](ConfigDiagnostic const& d) { return d.code == code; });
}

} // namespace

// Shipped `debug.pipeline.json` loads cleanly + resolves the Identity
// pass. This is the end-to-end sanity pin — fail here means the JSON
// file's shape drifted from the loader.
TEST(PipelineLoader, ShippedDebugLoadsIdentity) {
    auto r = opt::loadShippedPipeline("debug");
    ASSERT_TRUE(r.has_value()) << "shipped debug.pipeline.json failed to load";
    EXPECT_EQ(r->name, "debug");
    ASSERT_EQ(r->passes.size(), 1u);
    EXPECT_EQ(r->passes[0], opt::PassId::Identity);
}

// Shipped `release.pipeline.json` declares
// [Identity, ConstFold, Mem2Reg, CopyProp, Cse, Licm, SimplifyCfg, Dce]
// with maxIterations=4. LICM sits AFTER CSE (canonicalized SSA
// graph) and BEFORE SimplifyCFG (hoisting unconditional defs into
// the preheader may expose new constant-condition CondBrs the
// SimplifyCFG pass can fold).
TEST(PipelineLoader, ShippedReleaseLoadsAllPasses) {
    auto r = opt::loadShippedPipeline("release");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->name, "release");
    ASSERT_EQ(r->passes.size(), 8u);
    EXPECT_EQ(r->passes[0], opt::PassId::Identity);
    EXPECT_EQ(r->passes[1], opt::PassId::ConstFold);
    EXPECT_EQ(r->passes[2], opt::PassId::Mem2Reg);
    EXPECT_EQ(r->passes[3], opt::PassId::CopyProp);
    EXPECT_EQ(r->passes[4], opt::PassId::Cse);
    EXPECT_EQ(r->passes[5], opt::PassId::Licm);
    EXPECT_EQ(r->passes[6], opt::PassId::SimplifyCfg);
    EXPECT_EQ(r->passes[7], opt::PassId::Dce);
    EXPECT_EQ(r->maxIterations, 4u);
}

// maxIterations bounds (D-OPT-FIXED-POINT-LOOP): rejected when 0
// (silent-no-op trap) or > kMaxPipelineIterations (non-convergence
// signal). Missing field defaults to 1.
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

TEST(PipelineLoader, MaxIterationsMissingDefaultsToOne) {
    auto r = opt::loadPipelineFromText(
        R"({"dssPipelineVersion": 1, "pipeline":
            {"name": "x", "passes": ["Identity"]}})",
        "no-max-iter.json");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->maxIterations, 1u);
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

// Empty `passes` array → X_PipelineMalformed. The optimizer engine's
// loop runs zero times on an empty pipeline + returns ok=true with
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
// X_UnknownPassName — those are distinct remediations).
TEST(PipelineLoader, NonStringPassEntryRejects) {
    auto r = opt::loadPipelineFromText(
        R"({ "dssPipelineVersion": 1,
             "pipeline": { "name": "x", "passes": ["Identity", 42] } })",
        "non-string.json");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(hasCode(r.error(), DiagnosticCode::X_PipelineMalformed));
}
