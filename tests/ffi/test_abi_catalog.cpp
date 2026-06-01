// Plan 11 FF3 (ABI catalog) tests — `dss::ffi::resolveAbi`.
//
// Pins:
//   * Each shipped (target, format) pair resolves to the right
//     (CallConv, cc-name) tuple.
//   * Anchored-but-unshipped rows fail loud with NoMatchingCcInTarget
//     (catalog says the cc must exist, target.json doesn't ship it).
//   * Operand-stack (WASM) + result-id (SPIR-V) abi-models return
//     CcWasm / CcSpirv with cc=nullptr.
//   * Catalog is unique on (targetName, formatKind) — consteval pin.
//   * Diagnostic-code name round-trips for the 3 new F_Abi* codes.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/abi/abi_catalog.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace dss;
using namespace dss::ffi;

namespace {

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r,
                                    DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == c) ++n;
    return n;
}

} // namespace

// ── Shipped happy paths ──────────────────────────────────────

TEST(FfiAbiCatalog, X86_64ElfResolvesToSysV) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value()) << abiResolveErrorKindName(r.error().kind);
    EXPECT_EQ(r->callingConvention, CallConv::CcSysV);
    ASSERT_NE(r->cc, nullptr);
    EXPECT_EQ(r->cc->name, "sysv_amd64");
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(FfiAbiCatalog, X86_64PeResolvesToMS64) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value()) << abiResolveErrorKindName(r.error().kind);
    EXPECT_EQ(r->callingConvention, CallConv::CcMS64);
    ASSERT_NE(r->cc, nullptr);
    EXPECT_EQ(r->cc->name, "ms_x64");
}

TEST(FfiAbiCatalog, X86_64MachOResolvesToSysV) {
    // Apple's x86_64 ABI is SysV-with-quirks: same cc table as
    // Linux ELF, layout/calling-convention divergence handled
    // by quirks bits (anchored at D-FF3-1 layout cycle).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value()) << abiResolveErrorKindName(r.error().kind);
    EXPECT_EQ(r->callingConvention, CallConv::CcSysV);
    ASSERT_NE(r->cc, nullptr);
    EXPECT_EQ(r->cc->name, "sysv_amd64");
}

TEST(FfiAbiCatalog, Arm64ElfResolvesToAAPCS64) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-aarch64-linux");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value()) << abiResolveErrorKindName(r.error().kind);
    EXPECT_EQ(r->callingConvention, CallConv::CcAAPCS64);
    ASSERT_NE(r->cc, nullptr);
    EXPECT_EQ(r->cc->name, "aapcs64");
}

// ── Catalog anchored but cc not shipped → loud reject ──────

TEST(FfiAbiCatalog, Arm64MachOFailsLoudUntilAppleArm64CcShipped) {
    // The catalog has a row for (arm64, MachO) → "apple_arm64",
    // but arm64.target.json doesn't ship that cc yet. FF3 must
    // fail loud — silent fallback to aapcs64 would silently
    // generate the wrong ABI for Apple Silicon binaries.
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    // Use the shipped x86_64-darwin Mach-O format — cross-validate
    // would reject the (arm64, x86_64-Mach-O) machine-code pair,
    // but resolveAbi runs independently and reads only the format
    // KIND (MachO) for catalog lookup. The catalog row says
    // arm64+MachO needs cc "apple_arm64", which arm64.target.json
    // doesn't ship → NoMatchingCcInTarget.
    auto format = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, AbiResolveErrorKind::NoMatchingCcInTarget);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_AbiNoMatchingCcInTarget), 1u);
}

// ── Unknown catalog tuple → loud reject ────────────────────

TEST(FfiAbiCatalog, UnknownTargetNameFailsLoud) {
    // A target whose name isn't in kAbiCatalog fails loud with
    // UnknownTuple. Synthesize a minimal register-machine target
    // with an unknown name; resolveAbi only reads name + abiModel
    // + callingConventions, not the opcode table.
    auto target = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"riscv64","version":"0.0","abiModel":"register-machine"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, AbiResolveErrorKind::UnknownTuple);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_AbiUnknownTuple), 1u);
}

// ── WASM + SPIR-V (abi-model dispatched, no cc table) ──────

TEST(FfiAbiCatalog, OperandStackTargetWithWasmFormatResolvesNullCc) {
    auto target = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"wasm32","version":"0.0","abiModel":"operand-stack"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("wasm32-v1");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value()) << abiResolveErrorKindName(r.error().kind);
    EXPECT_EQ(r->callingConvention, CallConv::CcWasm);
    EXPECT_EQ(r->cc, nullptr);
}

TEST(FfiAbiCatalog, ResultIdTargetWithSpirvFormatResolvesNullCc) {
    auto target = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"spirv","version":"0.0","abiModel":"result-id"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("spirv-1.6");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value()) << abiResolveErrorKindName(r.error().kind);
    EXPECT_EQ(r->callingConvention, CallConv::CcSpirv);
    EXPECT_EQ(r->cc, nullptr);
}

// ── Defensive: abi-model ↔ format-kind disagreement ────────

TEST(FfiAbiCatalog, OperandStackTargetWithElfFormatFailsLoud) {
    // crossValidateTargetFormat catches this upstream, but FF3 is
    // defensive — if someone calls resolveAbi directly bypassing
    // the cross-validator, the mismatch fires loud.
    auto target = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"wasm32","version":"0.0","abiModel":"operand-stack"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, AbiResolveErrorKind::FormatAbiModelMismatch);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_AbiFormatAbiModelMismatch), 1u);
}

// ── Catalog coverage / contract pins ───────────────────────

TEST(FfiAbiCatalog, CatalogHasShippedTuples) {
    auto rows = abiCatalogTable();
    EXPECT_GE(rows.size(), 4u);  // at minimum the 4 currently-shipped pairs
    // Pin the specific shipped rows exist.
    bool sawX86ElfSysV = false, sawX86PeMS = false;
    bool sawX86MachOSysV = false, sawArmElfAAPCS = false;
    for (auto const& r : rows) {
        if (r.targetName == "x86_64" && r.formatKind == ObjectFormatKind::Elf
            && r.callingConvention == CallConv::CcSysV
            && r.expectedCcName == "sysv_amd64") sawX86ElfSysV = true;
        if (r.targetName == "x86_64" && r.formatKind == ObjectFormatKind::Pe
            && r.callingConvention == CallConv::CcMS64
            && r.expectedCcName == "ms_x64") sawX86PeMS = true;
        if (r.targetName == "x86_64" && r.formatKind == ObjectFormatKind::MachO
            && r.callingConvention == CallConv::CcSysV
            && r.expectedCcName == "sysv_amd64") sawX86MachOSysV = true;
        if (r.targetName == "arm64" && r.formatKind == ObjectFormatKind::Elf
            && r.callingConvention == CallConv::CcAAPCS64
            && r.expectedCcName == "aapcs64") sawArmElfAAPCS = true;
    }
    EXPECT_TRUE(sawX86ElfSysV);
    EXPECT_TRUE(sawX86PeMS);
    EXPECT_TRUE(sawX86MachOSysV);
    EXPECT_TRUE(sawArmElfAAPCS);
}

// ── Diagnostic code name round-trips ───────────────────────

TEST(FfiAbiCatalog, DiagnosticCodeNameRoundTripFAbiUnknownTuple) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_AbiUnknownTuple),
              "F_AbiUnknownTuple");
}
TEST(FfiAbiCatalog, DiagnosticCodeNameRoundTripFAbiNoMatchingCcInTarget) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_AbiNoMatchingCcInTarget),
              "F_AbiNoMatchingCcInTarget");
}
TEST(FfiAbiCatalog, DiagnosticCodeNameRoundTripFAbiFormatAbiModelMismatch) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_AbiFormatAbiModelMismatch),
              "F_AbiFormatAbiModelMismatch");
}

TEST(FfiAbiCatalog, AbiResolveErrorKindNameRoundTrip) {
    EXPECT_EQ(abiResolveErrorKindName(AbiResolveErrorKind::UnknownTuple),
              "UnknownTuple");
    EXPECT_EQ(abiResolveErrorKindName(AbiResolveErrorKind::NoMatchingCcInTarget),
              "NoMatchingCcInTarget");
    EXPECT_EQ(abiResolveErrorKindName(AbiResolveErrorKind::FormatAbiModelMismatch),
              "FormatAbiModelMismatch");
}
