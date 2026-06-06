// CompileOptions struct + resolvePipelineName table pin tests.
//
// D-OPT-COMPILE-OPTIONS-STRUCT — defaults match the documented
//   contract (Debug, no override) so existing callers passing `{}`
//   get the historical compile-pipeline behavior.
// D-OPT1-PIPELINE-CONFIG-FROM-COMPILECONFIG — the table maps each
//   CompileConfig enumerator to a distinct shipped pipeline name.
//   Out-of-range ordinals return nullopt (callers fail loud).

#include "program/cli_args.hpp"
#include "program/compile_pipeline.hpp"

#include <gtest/gtest.h>

using namespace dss;

TEST(CompileOptions, DefaultsAreDebugAndNoOverride) {
    CompileOptions opts;
    EXPECT_EQ(opts.config, CompileConfig::Debug);
    EXPECT_EQ(opts.pipelineOverride, nullptr);
}

TEST(ResolvePipelineName, DebugMapsToDebugPipeline) {
    auto const name = resolvePipelineName(CompileConfig::Debug);
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(*name, "debug");
}

TEST(ResolvePipelineName, ReleaseMapsToReleasePipeline) {
    auto const name = resolvePipelineName(CompileConfig::Release);
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(*name, "release");
}

// Out-of-range ordinal — produced by a buggy CLI parser, a future
// enumerator added without a table row, or an FFI numeric cast.
// MUST return nullopt; the caller in compile_pipeline.cpp emits
// X_PipelineNameResolutionFailed at this signal.
TEST(ResolvePipelineName, OutOfRangeOrdinalReturnsNullopt) {
    auto const name = resolvePipelineName(static_cast<CompileConfig>(99));
    EXPECT_FALSE(name.has_value());
}
