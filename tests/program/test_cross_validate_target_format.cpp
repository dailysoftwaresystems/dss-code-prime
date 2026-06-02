// Plan 14 §3.1 D-LK6-8.2 closure tests — target↔format machine
// cross-validation. Pins:
//   * matching x86_64 + elf64-x86_64-linux-exec passes
//   * matching arm64 + elf64-aarch64-linux-exec passes
//   * mismatched arm64 target + x86_64 format fails with
//     D_TargetFormatMismatch
//   * unknown target name skips the check (defer to format-side
//     validate)
//   * WASM / SPIR-V format kinds skip the check (no machine code)
//   * The closed-enum table actually contains the 2 v1 arches.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "program/cross_validate_target_format.hpp"

#include <gtest/gtest.h>

#include <string_view>

using namespace dss;

namespace {

// Synthesize a TargetSchema with a given `name`. Uses
// `loadFromText` to bypass schema-file constraints.
std::shared_ptr<TargetSchema const>
makeTarget(std::string_view name) {
    std::string const json = std::string{R"({
      "dssTargetVersion": 1,
      "target": {"name": ")"} + std::string{name} + R"("},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })";
    auto r = TargetSchema::loadFromText(json);
    if (!r.has_value()) {
        ADD_FAILURE() << "target load failed";
        return nullptr;
    }
    return *r;
}

// Synthesize an ELF ObjectFormatSchema with a given machine code.
std::shared_ptr<ObjectFormatSchema const>
makeElfFormat(std::uint16_t machine) {
    std::string const json = std::string{R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"synth-elf","kind":"elf"},
      "elf": {"class":"elf64","data":"lsb","machine": )"}
      + std::to_string(machine) + R"(}
    })";
    auto r = ObjectFormatSchema::loadFromText(json);
    if (!r.has_value()) {
        std::string s;
        for (auto const& d : r.error()) s += d.message + "\n";
        ADD_FAILURE() << "format load failed: " << s;
        return nullptr;
    }
    return *r;
}

// Synthesize a PE ObjectFormatSchema with a given machine code.
std::shared_ptr<ObjectFormatSchema const>
makePeFormat(std::uint16_t machine) {
    std::string const json = std::string{R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"synth-pe","kind":"pe"},
      "pe": {"machine": )"} + std::to_string(machine) + R"(}
    })";
    auto r = ObjectFormatSchema::loadFromText(json);
    if (!r.has_value()) {
        std::string s;
        for (auto const& d : r.error()) s += d.message + "\n";
        ADD_FAILURE() << "format load failed: " << s;
        return nullptr;
    }
    return *r;
}

// Synthesize a Mach-O ObjectFormatSchema with a given cputype.
std::shared_ptr<ObjectFormatSchema const>
makeMachOFormat(std::uint32_t cputype) {
    std::string const json = std::string{R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"synth-macho","kind":"macho"},
      "macho": {"cputype": )"} + std::to_string(cputype) + R"(}
    })";
    auto r = ObjectFormatSchema::loadFromText(json);
    if (!r.has_value()) {
        std::string s;
        for (auto const& d : r.error()) s += d.message + "\n";
        ADD_FAILURE() << "format load failed: " << s;
        return nullptr;
    }
    return *r;
}

bool sawCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return true;
    }
    return false;
}

} // namespace

// ── Happy path: matching pairs ────────────────────────────────

TEST(CrossValidateTargetFormat, X86_64ElfMatches) {
    auto target = makeTarget("x86_64");
    auto format = makeElfFormat(62);  // EM_X86_64
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(CrossValidateTargetFormat, Arm64ElfMatches) {
    auto target = makeTarget("arm64");
    auto format = makeElfFormat(183);  // EM_AARCH64
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

// ── The SIGILL-surface fix: mismatch fails loud ───────────────

TEST(CrossValidateTargetFormat, Arm64TargetWithX86_64FormatFailsLoud) {
    // The exact CRITICAL silent-failure scenario from D-LK6-8.2:
    // user supplies `arm64:elf64-x86_64-linux-exec` or a hand-edited
    // format JSON declaring `machine: 62` on an ARM64-targeted file.
    // Pre-D-LK6-8.2 the dispatch silently emitted x86_64 PLT stubs
    // into an ARM64 image → SIGILL. Now fails loud.
    auto target = makeTarget("arm64");
    auto format = makeElfFormat(62);  // mismatched
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_FALSE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::D_TargetMachineCodeMismatch));
}

TEST(CrossValidateTargetFormat, X86_64TargetWithArm64FormatFailsLoud) {
    auto target = makeTarget("x86_64");
    auto format = makeElfFormat(183);
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_FALSE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::D_TargetMachineCodeMismatch));
}

// ── Unknown target name: skip the check ───────────────────────
//
// Target names not in the cross-check table (WASM/SPIR-V/future
// ISAs) defer to format-side validation. Returning true here
// reflects "the check doesn't apply", not "validated".

TEST(CrossValidateTargetFormat, UnknownTargetNameSkipsCheck) {
    auto target = makeTarget("riscv64");  // not in v1 table
    auto format = makeElfFormat(243);     // EM_RISCV
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

// ── Shipped configs cross-validate cleanly ────────────────────

TEST(CrossValidateTargetFormat, ShippedX86_64PairsClean) {
    auto target = TargetSchema::loadShipped("x86_64");
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(target.has_value() && format.has_value());
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(**target, **format, rep));
}

TEST(CrossValidateTargetFormat, ShippedArm64PairsClean) {
    auto target = TargetSchema::loadShipped("arm64");
    auto format = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(target.has_value() && format.has_value());
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(**target, **format, rep));
}

// ── Table coverage: the closed-enum row count + values ────────

TEST(CrossValidateTargetFormat, TableContainsBothV1Arches) {
    auto const table = targetArchMachineCodesTable();
    ASSERT_EQ(table.size(), 2u);
    bool sawX86 = false, sawArm = false;
    for (auto const& row : table) {
        if (row.targetName == "x86_64") {
            EXPECT_EQ(row.elfMachine, 62u);
            EXPECT_EQ(row.peMachine, 0x8664u);
            EXPECT_EQ(row.machoCpuType, 0x01000007u);
            sawX86 = true;
        }
        if (row.targetName == "arm64") {
            EXPECT_EQ(row.elfMachine, 183u);
            EXPECT_EQ(row.peMachine, 0xAA64u);
            EXPECT_EQ(row.machoCpuType, 0x0100000Cu);
            sawArm = true;
        }
    }
    EXPECT_TRUE(sawX86);
    EXPECT_TRUE(sawArm);
}

// ── Diagnostic code name round-trip ───────────────────────────

TEST(CrossValidateTargetFormat, DTargetFormatMismatchNameRoundTrip) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_TargetFormatMismatch),
              "D_TargetFormatMismatch");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::D_TargetFormatMismatch),
              "D000C");
}

// ── Post-fold #1 (pr-test-analyzer P9): PE arm coverage ──────

TEST(CrossValidateTargetFormat, X86_64PeMatches) {
    auto target = makeTarget("x86_64");
    auto format = makePeFormat(0x8664);
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(CrossValidateTargetFormat, Arm64TargetWithX86_64PeFailsLoud) {
    auto target = makeTarget("arm64");
    auto format = makePeFormat(0x8664);
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_FALSE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::D_TargetMachineCodeMismatch));
}

// ── Post-fold #1 (pr-test-analyzer P9): Mach-O arm coverage ───

TEST(CrossValidateTargetFormat, X86_64MachOMatches) {
    auto target = makeTarget("x86_64");
    auto format = makeMachOFormat(0x01000007);
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(CrossValidateTargetFormat, Arm64TargetWithX86_64MachOFailsLoud) {
    auto target = makeTarget("arm64");
    auto format = makeMachOFormat(0x01000007);
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_FALSE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::D_TargetMachineCodeMismatch));
}

// ── Post-fold #1 (silent-failure CRITICAL-1): abiModel cross-check ──

TEST(CrossValidateTargetFormat, RegisterMachineWithWasmFormatFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto wasm = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"synth-wasm","kind":"wasm"}
    })");
    ASSERT_TRUE(wasm.has_value());

    DiagnosticReporter rep;
    EXPECT_FALSE(crossValidateTargetFormat(**target, **wasm, rep));
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::D_TargetAbiModelMismatch));
}

TEST(CrossValidateTargetFormat, RegisterMachineWithSpirvFormatFailsLoud) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto spirv = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"synth-spirv","kind":"spirv"}
    })");
    ASSERT_TRUE(spirv.has_value());

    DiagnosticReporter rep;
    EXPECT_FALSE(crossValidateTargetFormat(**target, **spirv, rep));
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::D_TargetAbiModelMismatch));
}

// ── Post-fold #2 (pr-test-analyzer Gap 1): Unknown-cell deferral ─
// Note: `ObjectFormatKind::Unknown` is the default-sentinel and the
// JSON loader rejects `"kind":"unknown"` at load time, so the
// Unknown arms in `abiModelMatchesFormatKind` + the format-kind
// switch are unreachable via valid user-loaded schemas. The arms
// exist for defense-in-depth against a default-constructed
// ObjectFormatData reaching the function (e.g. a future
// programmatically-constructed schema). Not testable from a JSON
// fixture today; anchored as defensive code.

// ── Post-fold #2 (HIGH-1): whitespace target.name rejected at load ─

TEST(TargetSchemaLoader, WhitespaceOnlyTargetNameRejected) {
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"   "},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(TargetSchemaLoader, LeadingTrailingWhitespaceTargetNameRejected) {
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":" arm64 "},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_FALSE(r.has_value());
}

// ── Post-fold #2 (architect Q3): format.name cross-tier symmetry ─

TEST(ObjectFormatSchemaLoader, EmptyFormatNameRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"","kind":"elf"}
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ObjectFormatSchemaLoader, WhitespaceFormatNameRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":" elf64 ","kind":"elf"}
    })");
    ASSERT_FALSE(r.has_value());
}

// ── Post-fold #2 (HIGH-3): split-code round-trip ───────────────

TEST(CrossValidateTargetFormat, DTargetMachineCodeMismatchNameRoundTrip) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_TargetMachineCodeMismatch),
              "D_TargetMachineCodeMismatch");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::D_TargetMachineCodeMismatch),
              "D000D");
}

TEST(CrossValidateTargetFormat, DTargetAbiModelMismatchNameRoundTrip) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_TargetAbiModelMismatch),
              "D_TargetAbiModelMismatch");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::D_TargetAbiModelMismatch),
              "D000E");
}

// ── Post-fold #1 (CRITICAL-2): empty target name rejected at load ─

TEST(TargetSchemaLoader, EmptyTargetNameRejected) {
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":""},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_FALSE(r.has_value());
}

// ── Post-fold #1: shipped PE / Mach-O cross-validate cleanly ──

TEST(CrossValidateTargetFormat, ShippedX86_64PeMatches) {
    auto target = TargetSchema::loadShipped("x86_64");
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(target.has_value() && format.has_value());
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(**target, **format, rep));
}

TEST(CrossValidateTargetFormat, ShippedX86_64MachOMatches) {
    auto target = TargetSchema::loadShipped("x86_64");
    auto format = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(target.has_value() && format.has_value());
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(**target, **format, rep));
}
