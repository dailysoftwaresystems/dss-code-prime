// SPIR-V module writer (LK9 skeleton) — plan 14 LK9 tests.
//
// Pins the substrate plumb-through (parallel to LK8 WASM):
//   * Shipped `spirv-1.6.format.json` loads without diagnostics.
//   * Linker dispatch routes `ObjectFormatKind::Spirv` through the
//     new walker (not the old fail-loud arm at LK9).
//   * Walker emits exactly the 20-byte SPIR-V module header per
//     SPIR-V Spec §2.3:
//       word[0] = 0x07230203 (magic)
//       word[1] = 0x00010600 (version 1.6)
//       word[2] = 0          (generator)
//       word[3] = 0          (bound)
//       word[4] = 0          (reserved)
//   * Non-Spirv schema passed to `spirv::encode` fails loud.
//   * Fail-loud guards on non-empty functions / externs / non-zero
//     expectedFuncCount with the new K_WalkerInputContractViolation
//     diagnostic (0x8005, LK8 post-fold).

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/spirv.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

using namespace dss;

namespace {

// Wrapper so vector arguments implicitly convert to span at call
// sites. Uses shared `dss::link_format::test::readU32LE`; the
// `link_format` namespace was originally introduced to avoid
// collision with the historical `dss::link()` free function, now
// renamed to `dss::linker::link()` (D-LK9-2 closed at LK10 cycle
// 2). The `link_format` namespace is kept as-is — renaming it
// would be churn-only and the prefix still cleanly distinguishes
// per-format helpers from the `linker::` substrate.
[[nodiscard]] std::uint32_t readU32LE(std::vector<std::uint8_t> const& bytes,
                                       std::size_t off) {
    return dss::link_format::test::readU32LE(
        std::span<std::uint8_t const>{bytes}, off);
}

[[nodiscard]] AssembledModule makeEmptyModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 0;
    return mod;
}

} // namespace

// ── Shipped JSON loads cleanly ───────────────────────────────────────

TEST(SpirvFormatJson, Shipped1_6SchemaLoadsWithoutDiagnostics) {
    auto r = ObjectFormatSchema::loadShipped("spirv-1.6");
    ASSERT_TRUE(r.has_value()) << "shipped spirv-1.6.format.json must load";
    EXPECT_EQ((*r)->kind(), ObjectFormatKind::Spirv);
    EXPECT_EQ((*r)->name(), "spirv-1.6");
}

TEST(SpirvFormatJson, SpirvKindWithElfBlockRejected) {
    // Cross-kind guard (LK8 post-fold substrate): a spirv-kind
    // schema with a stray `elf` block is rejected at load.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"spirv-with-elf-block","kind":"spirv"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"rel" }
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(SpirvFormatJson, SpirvKindWithUniversalFieldRejected) {
    // Universal-field reject (LK8 post-fold substrate): SPIR-V's
    // intra-module references are <id>s (not native relocations),
    // so a top-level `relocations[]` would be silently dropped.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"spirv-with-relocations","kind":"spirv"},
      "relocations": [{"name":"R_X86_64_PC32","kind":1,"nativeId":2}]
    })");
    ASSERT_FALSE(r.has_value());
}

// ── Walker emits the 20-byte SPIR-V module header ────────────────────

TEST(SpirvWriter, EmitsFiveWordHeader) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("spirv-1.6");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeEmptyModule();
    DiagnosticReporter rep;
    auto bytes = spirv::encode(mod, **target, **fmt, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(bytes.size(), 20u);
    // word[0]: magic 0x07230203 (LE u32 → bytes 0x03, 0x02, 0x23, 0x07)
    EXPECT_EQ(readU32LE(bytes, 0),  0x07230203u);
    // word[1]: version 1.6 — major 1 in bits 16..23, minor 6 in bits 8..15
    EXPECT_EQ(readU32LE(bytes, 4),  0x00010600u);
    // word[2]: generator (unspecified)
    EXPECT_EQ(readU32LE(bytes, 8),  0u);
    // word[3]: bound (no <id>s in skeleton)
    EXPECT_EQ(readU32LE(bytes, 12), 0u);
    // word[4]: reserved (must be 0 per spec §2.3)
    EXPECT_EQ(readU32LE(bytes, 16), 0u);
}

// ── Linker dispatch routes Spirv to the walker ───────────────────────

TEST(SpirvLinkerDispatch, RoutesSpirvKindToWalker) {
    // The format-blind linker engine must dispatch
    // ObjectFormatKind::Spirv to spirv::encode (not the previous
    // LK9 fail-loud arm).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("spirv-1.6");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeEmptyModule();
    DiagnosticReporter rep;
    LinkedImage img = linker::link(mod, **target, **fmt, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(img.bytes.size(), 20u);
    EXPECT_EQ(readU32LE(img.bytes, 0), 0x07230203u);
}

TEST(SpirvLinkerDispatch, EmptyModuleProducesHeaderButOkIsFalse) {
    // Mirrors WASM's `bytes ≠ {} ∧ ok() == false` invariant:
    // the 20-byte header is valid output but ok() requires
    // expectedFuncCount > 0. Plan 17 lifts when functions populate.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("spirv-1.6");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeEmptyModule();
    DiagnosticReporter rep;
    LinkedImage img = linker::link(mod, **target, **fmt, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(img.bytes.size(), 20u);
    EXPECT_FALSE(img.ok());
}

// ── Failure modes (substrate discipline) ─────────────────────────────

TEST(SpirvWriter, NonSpirvFormatFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeEmptyModule();
    DiagnosticReporter rep;
    auto bytes = spirv::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat) saw = true;
    }
    EXPECT_TRUE(saw);
}

TEST(SpirvWriter, NonEmptyFunctionsFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("spirv-1.6");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto bytes = spirv::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_WalkerInputContractViolation
         && d.actual.find("plan 17") != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}

TEST(SpirvWriter, NonEmptyExternImportsFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("spirv-1.6");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeEmptyModule();
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "printf", "libc.so.6"});
    DiagnosticReporter rep;
    auto bytes = spirv::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport
         && d.actual.find("OpExtInstImport") != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}

TEST(SpirvWriter, NonZeroExpectedFuncCountFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("spirv-1.6");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 3;  // mismatch: no functions
    DiagnosticReporter rep;
    auto bytes = spirv::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_WalkerInputContractViolation
         && d.actual.find("expectedFuncCount") != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}
