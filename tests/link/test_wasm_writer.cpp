// WebAssembly module writer (LK8 skeleton) — plan 14 LK8 tests.
//
// Pins the substrate plumb-through:
//   * Shipped `wasm32-v1.format.json` loads without diagnostics.
//   * Linker dispatch routes `ObjectFormatKind::Wasm` through the
//     new walker (not the old fail-loud arm at LK8/LK9).
//   * Walker emits exactly the 8-byte WebAssembly v1 preamble
//     (magic '\0asm' + version 1) per WebAssembly spec §5.5.
//   * Non-Wasm schema passed to `wasm::encode` fails loud.
//   * Non-empty `AssembledModule.functions` fails loud (LK8
//     skeleton contract: plan 18's MIR→WAT lowerer is the WASM-
//     byte producer; native-ISA bytes are not consumable here).

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/wasm.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] AssembledModule makeEmptyModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 0;
    return mod;
}

} // namespace

// ── Shipped JSON loads cleanly ───────────────────────────────────────

TEST(WasmFormatJson, ShippedV1SchemaLoadsWithoutDiagnostics) {
    auto r = ObjectFormatSchema::loadShipped("wasm32-v1");
    ASSERT_TRUE(r.has_value()) << "shipped wasm32-v1.format.json must load";
    EXPECT_EQ((*r)->kind(), ObjectFormatKind::Wasm);
    EXPECT_EQ((*r)->name(), "wasm32-v1");
}

TEST(WasmFormatJson, WasmKindWithElfBlockRejected) {
    // Cross-format symmetry (test-analyzer Gap 6 fold): a wasm-
    // kind schema with a stray `elf` block silently dropped before
    // the LK8 cross-kind guard. Now rejected at load.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"wasm-with-elf-block","kind":"wasm"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"rel" }
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(WasmFormatJson, WasmKindWithPeBlockRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"wasm-with-pe-block","kind":"wasm"},
      "pe": { "machine": 34404, "type": "obj" }
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(WasmFormatJson, WasmKindWithMachoBlockRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"wasm-with-macho-block","kind":"wasm"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "object", "flags": 0 }
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(WasmFormatJson, WasmKindWithUniversalFieldRejected) {
    // type-design Q3 fold: WASM has its own section vocabulary
    // (Type / Function / Code sections, plan 18 owned) — a top-
    // level `sections[]` / `relocations[]` / `entryPoint` would
    // be silently ignored by the walker. Validate-reject at load.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"wasm-with-relocations","kind":"wasm"},
      "relocations": [{"name":"R_X86_64_PC32","kind":1,"nativeId":2}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ObjectFormatJsonCrossKind, ElfKindWithPeBlockRejected) {
    // pr-test-analyzer Gap 1 fold: cross-kind guard is generic
    // (5 (kind, block) rules in `kCrossKindRules`); pin the
    // reciprocal arms so a future enum-drift refactor that drops
    // a rule fails fast.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"elf-with-pe-block","kind":"elf"},
      "pe": { "machine": 34404, "type": "obj" }
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ObjectFormatJsonCrossKind, PeKindWithMachoBlockRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"pe-with-macho-block","kind":"pe"},
      "pe": { "machine": 34404, "type": "obj" },
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "object", "flags": 0 }
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ObjectFormatJsonCrossKind, MachoKindWithOptionalHeaderBlockRejected) {
    // The 5th rule pair (Pe → optionalHeader) is the only one
    // not previously exercised — wrap-around to ensure none of
    // the 5 rules are unreachable.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"macho-with-pe-oh-block","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "object", "flags": 0 },
      "optionalHeader": { "magic": 523 }
    })");
    ASSERT_FALSE(r.has_value());
}

// ── Walker emits the 8-byte WebAssembly v1 preamble ──────────────────

TEST(WasmWriter, EmitsMagicAndVersion) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("wasm32-v1");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeEmptyModule();
    DiagnosticReporter rep;
    auto bytes = wasm::encode(mod, **target, **fmt, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(bytes.size(), 8u);
    // magic = "\0asm" (little-endian u32 0x6d736100)
    EXPECT_EQ(bytes[0], 0x00u);
    EXPECT_EQ(bytes[1], 0x61u);  // 'a'
    EXPECT_EQ(bytes[2], 0x73u);  // 's'
    EXPECT_EQ(bytes[3], 0x6du);  // 'm'
    // version = 1 (little-endian u32)
    EXPECT_EQ(bytes[4], 0x01u);
    EXPECT_EQ(bytes[5], 0x00u);
    EXPECT_EQ(bytes[6], 0x00u);
    EXPECT_EQ(bytes[7], 0x00u);
}

// ── Linker dispatch routes Wasm to the walker ────────────────────────

TEST(WasmLinkerDispatch, RoutesWasmKindToWalker) {
    // The format-blind linker engine must dispatch
    // ObjectFormatKind::Wasm to wasm::encode (not the previous
    // fail-loud LK8/LK9 arm).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("wasm32-v1");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeEmptyModule();
    DiagnosticReporter rep;
    LinkedImage img = link(mod, **target, **fmt, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(img.bytes.size(), 8u);
    EXPECT_EQ(img.bytes[0], 0x00u);
    EXPECT_EQ(img.bytes[1], 0x61u);
    EXPECT_EQ(img.bytes[4], 0x01u);
}

TEST(WasmLinkerDispatch, EmptyModuleProducesPreambleButOkIsFalse) {
    // pr-test-analyzer Gap 5 fold (refined): the universal linker
    // contract `LinkedImage::ok() == (expectedFuncCount > 0 &&
    // resolvedFuncCount == expectedFuncCount)` holds for WASM
    // skeletons — `ok() == false` when expectedFuncCount == 0
    // even though bytes are non-empty (the 8-byte preamble).
    // WASM is the ONLY format today where `bytes.size() > 0` and
    // `ok() == false` can co-exist; pin this invariant so a
    // future caller doesn't conflate "got bytes" with "successful
    // link." Plan 18 will lift this once functions populate.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("wasm32-v1");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 0;
    DiagnosticReporter rep;
    LinkedImage img = link(mod, **target, **fmt, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(img.bytes.size(), 8u);
    EXPECT_EQ(img.resolvedFuncCount, 0u);
    EXPECT_EQ(img.expectedFuncCount, 0u);
    EXPECT_FALSE(img.ok())
        << "ok() requires expectedFuncCount > 0; WASM-preamble-only "
           "output has expectedFuncCount=0, so ok() is false even "
           "though bytes are non-empty. This is the WASM-specific "
           "(bytes ≠ {} ∧ ok() == false) case.";
}

// ── Failure modes (substrate discipline) ─────────────────────────────

TEST(WasmWriter, NonWasmFormatFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeEmptyModule();
    DiagnosticReporter rep;
    auto bytes = wasm::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat) saw = true;
    }
    EXPECT_TRUE(saw);
}

TEST(WasmWriter, NonEmptyExternImportsFailsLoud) {
    // silent-failure CRITICAL + type-design fold: WASM has its
    // own Import-section structure (type idx + module+name
    // strings) that doesn't map 1:1 to ExternImport{symbol,
    // mangledName, libraryPath}. Plan 18 owns Import-section
    // emission; LK8 skeleton fails loud rather than silently
    // dropping externImports.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("wasm32-v1");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 0;
    ExternImport ei{SymbolId{99}, "printf", "libc.so.6"};
    mod.externImports.push_back(std::move(ei));
    DiagnosticReporter rep;
    auto bytes = wasm::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport
         && d.actual.find("plan 18") != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}

TEST(WasmWriter, NonZeroExpectedFuncCountFailsLoud) {
    // silent-failure HIGH + test-analyzer Gap 3 fold: a caller
    // who sets expectedFuncCount > 0 but ships empty functions
    // would otherwise produce an 8-byte preamble whose ok() is
    // false only because the parallel-index gate disagrees. Fail
    // loud at the walker so the diagnostic anchors here.
    // Uses K_WalkerInputContractViolation (0x8005, type-design Q1
    // fold) — distinct from K_NoMatchingObjectFormat which signals
    // "no walker for this kind".
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("wasm32-v1");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 3;  // mismatch: no functions vector
    DiagnosticReporter rep;
    auto bytes = wasm::encode(mod, **target, **fmt, rep);
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

TEST(WasmWriter, NonEmptyFunctionsFailsLoud) {
    // LK8 skeleton contract: WASM bytes come from the MIR→WAT
    // lowerer (plan 18), NOT from the native assembler's
    // AssembledModule.functions. A non-empty `functions` vector
    // means the caller routed native-ISA bytes here by mistake —
    // fail loud rather than silently ship a malformed 8-byte
    // module with native bytes invisibly dropped.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("wasm32-v1");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};  // x86_64 `ret`
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto bytes = wasm::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_WalkerInputContractViolation
         && d.actual.find("plan 18") != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}
