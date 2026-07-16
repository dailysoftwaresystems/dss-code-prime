// `TargetSpec` substrate tests — plan 14 LK10 cycle 2.
//
// Pins:
//   * `parse()` accepts well-formed `"<target>:<format>"` strings.
//   * Rejects no-colon, empty-half, multi-colon (ambiguity).
//   * `outputExtension(...)` covers every shipped (kind, objectType)
//     combination — closed-switch coverage with no silent default
//     for new format kinds.

#include "core/types/diagnostic_reporter.hpp"
#include "link/object_format_schema.hpp"
#include "program/target_spec.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace dss;

// ── parse() ─────────────────────────────────────────────────────────

TEST(TargetSpec, ParsesValidColonSeparatedSpec) {
    auto p = TargetSpec::parse("x86_64:elf64-x86_64-linux");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->targetName, "x86_64");
    EXPECT_EQ(p->formatName, "elf64-x86_64-linux");
}

TEST(TargetSpec, ParsesArm64SpecToo) {
    auto p = TargetSpec::parse("arm64:macho64-arm64-darwin");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->targetName, "arm64");
    EXPECT_EQ(p->formatName, "macho64-arm64-darwin");
}

TEST(TargetSpec, RejectsNoColonWithMissingColonError) {
    auto p = TargetSpec::parse("x86_64elf64");
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(p.error(), TargetSpecError::MissingColon);

    EXPECT_EQ(TargetSpec::parse("").error(),
              TargetSpecError::MissingColon);
    EXPECT_EQ(TargetSpec::parse("nojoy").error(),
              TargetSpecError::MissingColon);
}

TEST(TargetSpec, RejectsEmptyTargetHalfWithSpecificError) {
    auto p = TargetSpec::parse(":elf64-x86_64-linux");
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(p.error(), TargetSpecError::EmptyTargetName);
}

TEST(TargetSpec, RejectsEmptyFormatHalfWithSpecificError) {
    auto p = TargetSpec::parse("x86_64:");
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(p.error(), TargetSpecError::EmptyFormatName);
}

TEST(TargetSpec, RejectsMultipleColonsWithSpecificError) {
    // Two-colon strings are syntactically ambiguous — could be
    // "a:b:c" parsed as (a, b:c) or (a:b, c). Reject so future
    // grammar growth (e.g. a third "platform" axis) doesn't
    // silently consume the wrong colon. Caller can re-encode.
    auto p = TargetSpec::parse("x86_64:elf:linux");
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(p.error(), TargetSpecError::MultipleColons);
    EXPECT_EQ(TargetSpec::parse("a:b:c").error(),
              TargetSpecError::MultipleColons);
}

TEST(TargetSpec, RejectsWhitespaceInEitherHalf) {
    // pr-test-analyzer FOLD-NOW: whitespace in a schema-name half
    // is almost always a CLI/config typo. Silent acceptance would
    // route the surrounding-space name to `loadShipped`, which
    // would then fail with a misleading `D_SchemaLoadFailed` —
    // fail at parse time instead so the operator sees the actual
    // root cause.
    EXPECT_EQ(TargetSpec::parse(" x86_64:elf64-x86_64-linux").error(),
              TargetSpecError::WhitespaceInName);
    EXPECT_EQ(TargetSpec::parse("x86_64 :elf64-x86_64-linux").error(),
              TargetSpecError::WhitespaceInName);
    EXPECT_EQ(TargetSpec::parse("x86_64: elf64-x86_64-linux").error(),
              TargetSpecError::WhitespaceInName);
    EXPECT_EQ(TargetSpec::parse("x86_64:elf64-x86_64-linux ").error(),
              TargetSpecError::WhitespaceInName);
    EXPECT_EQ(TargetSpec::parse("x86 64:elf").error(),
              TargetSpecError::WhitespaceInName);
}

// ── outputExtension() ───────────────────────────────────────────────

TEST(TargetSpec, OutputExtensionElfObjForShippedRelocSchema) {
    auto r = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(r.has_value());
    TargetSpec spec{"x86_64", "elf64-x86_64-linux"};
    EXPECT_EQ(spec.outputExtension(**r), ".o");
}

TEST(TargetSpec, OutputExtensionElfExecHasNoExtension) {
    auto r = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(r.has_value());
    TargetSpec spec{"x86_64", "elf64-x86_64-linux-exec"};
    EXPECT_EQ(spec.outputExtension(**r), "");
}

TEST(TargetSpec, OutputExtensionElfDynSoAndPieNoExtension) {
    // c151 (D-LK1-4 PIE half): the TWO ET_DYN sub-shapes name
    // differently — a shared library takes `.so`; a PIE is an
    // EXECUTABLE and takes the exec convention (no extension,
    // `gcc -pie hello.c -o prog` names it `prog`). Discriminated by
    // the schema's entry cluster (processExit presence), never a
    // format-name check.
    auto so = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(so.has_value());
    TargetSpec soSpec{"x86_64", "elf64-x86_64-linux-dyn"};
    EXPECT_EQ(soSpec.outputExtension(**so), ".so");
    auto pie = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-pie");
    ASSERT_TRUE(pie.has_value());
    TargetSpec pieSpec{"x86_64", "elf64-x86_64-linux-pie"};
    EXPECT_EQ(pieSpec.outputExtension(**pie), "");
}

TEST(TargetSpec, OutputExtensionPeObjForShippedObjSchema) {
    auto r = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(r.has_value());
    TargetSpec spec{"x86_64", "pe64-x86_64-windows"};
    EXPECT_EQ(spec.outputExtension(**r), ".obj");
}

TEST(TargetSpec, OutputExtensionPeExeForShippedExecSchema) {
    auto r = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(r.has_value());
    TargetSpec spec{"x86_64", "pe64-x86_64-windows-exec"};
    EXPECT_EQ(spec.outputExtension(**r), ".exe");
}

TEST(TargetSpec, OutputExtensionMachOObjectForShippedObjectSchema) {
    auto r = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    ASSERT_TRUE(r.has_value());
    TargetSpec spec{"x86_64", "macho64-x86_64-darwin"};
    EXPECT_EQ(spec.outputExtension(**r), ".o");
}

TEST(TargetSpec, OutputExtensionMachOExecuteHasNoExtension) {
    auto r = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(r.has_value());
    TargetSpec spec{"x86_64", "macho64-x86_64-darwin-exec"};
    EXPECT_EQ(spec.outputExtension(**r), "");
}

TEST(TargetSpec, OutputExtensionWasmForShippedWasm) {
    auto r = ObjectFormatSchema::loadShipped("wasm32-v1");
    ASSERT_TRUE(r.has_value());
    TargetSpec spec{"x86_64", "wasm32-v1"};
    EXPECT_EQ(spec.outputExtension(**r), ".wasm");
}

TEST(TargetSpec, OutputExtensionSpvForShippedSpirv) {
    auto r = ObjectFormatSchema::loadShipped("spirv-1.6");
    ASSERT_TRUE(r.has_value());
    TargetSpec spec{"x86_64", "spirv-1.6"};
    EXPECT_EQ(spec.outputExtension(**r), ".spv");
}

TEST(TargetSpec, OutputExtensionUnknownKindReturnsEmpty) {
    // pr-test-analyzer FOLD-NOW: pin the closed-switch sentinel
    // arm. Real linker dispatch rejects `Unknown` schemas before
    // outputExtension is consulted, but the closed switch's
    // exhaustiveness is a substrate-tier contract that this test
    // guards against future drift (e.g. someone adding a new arm
    // without an Unknown fallback).
    dss::detail::ObjectFormatData data;
    data.kind = ObjectFormatKind::Unknown;
    data.name = "synthetic-unknown";
    ObjectFormatSchema synth{std::move(data)};
    TargetSpec spec{"x86_64", "synthetic-unknown"};
    EXPECT_EQ(spec.outputExtension(synth), "");
}
