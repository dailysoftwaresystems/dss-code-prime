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
#include "diagnostic_count.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::countCode;

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

// ── arm64 + Mach-O resolves to apple_arm64 (D-FF3-5 CLOSED) ──

// D-FF3-5 CLOSED (macOS-ARM64 backend cycle): arm64.target.json now
// ships the `apple_arm64` cc row, so the catalog's (arm64, MachO) →
// "apple_arm64" tuple resolves successfully. This test FLIPPED from the
// prior fail-loud assertion (NoMatchingCcInTarget while the row was
// absent) to the happy-path mirror, exactly as the D-FF3-5 anchor's
// trigger instructed ("apple_arm64 cc row ships in arm64.target.json").
// A silent fallback to aapcs64 would have generated the wrong ABI for
// Apple Silicon binaries; resolveAbi resolves the DISTINCT apple_arm64
// cc instead.
TEST(FfiAbiCatalog, Arm64MachOResolvesToAppleArm64) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    // Use the shipped x86_64-darwin Mach-O format — resolveAbi reads
    // only the format KIND (MachO) for the catalog lookup, so the
    // (arm64, MachO) tuple is exercised regardless of the format's CPU.
    auto format = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value()) << abiResolveErrorKindName(r.error().kind);
    EXPECT_EQ(r->callingConvention, CallConv::CcApple);
    ASSERT_NE(r->cc, nullptr);
    EXPECT_EQ(r->cc->name, "apple_arm64");
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_AbiNoMatchingCcInTarget), 0u);
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

TEST(FfiAbiCatalog, CatalogCountIsExactly6PinsAllAnchoredRows) {
    // Pin the EXACT count — a silent addition of a row (e.g. a
    // future arm64+Wasm tuple) should trip the test, not pass
    // unobserved. Bump to N+1 when shipping the Nth (target,
    // format) pair AND add the corresponding row pin below.
    auto rows = abiCatalogTable();
    EXPECT_EQ(rows.size(), 6u);
    // Pin the specific shipped rows exist.
    bool sawX86ElfSysV = false, sawX86PeMS = false;
    bool sawX86MachOSysV = false, sawArmElfAAPCS = false;
    bool sawArmPeMs = false, sawArmMachOApple = false;
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
        // Pin the placeholder CallConv (CcMS64 per D-FF3-4 anchor)
        // explicitly — when D-FF3-4 introduces CcMSARM64 and flips
        // this row, the test trips, forcing either deletion of the
        // anchor or update of this pin (test-analyzer P6 post-fold #3).
        if (r.targetName == "arm64" && r.formatKind == ObjectFormatKind::Pe
            && r.callingConvention == CallConv::CcMS64
            && r.expectedCcName == "ms_arm64") sawArmPeMs = true;
        if (r.targetName == "arm64" && r.formatKind == ObjectFormatKind::MachO
            && r.callingConvention == CallConv::CcApple
            && r.expectedCcName == "apple_arm64") sawArmMachOApple = true;
    }
    EXPECT_TRUE(sawX86ElfSysV);
    EXPECT_TRUE(sawX86PeMS);
    EXPECT_TRUE(sawX86MachOSysV);
    EXPECT_TRUE(sawArmElfAAPCS);
    // Anchored-but-unshipped rows must remain in the catalog (anchor
    // is the design statement that these combos are intended targets,
    // even though target.json doesn't yet ship the matching cc rows).
    EXPECT_TRUE(sawArmPeMs)
        << "anchor row arm64+Pe → ms_arm64 was removed; if the design "
        << "decision has changed, update D-FF3-4 anchor too";
    EXPECT_TRUE(sawArmMachOApple)
        << "anchor row arm64+MachO → apple_arm64 was removed";
}

// ── Defensive arm symmetry ─────────────────────────────────

TEST(FfiAbiCatalog, ResultIdTargetWithPeFormatFailsLoud) {
    // Symmetric mirror of OperandStackTargetWithElfFormatFailsLoud:
    // pins the result-id arm of the abi-model dispatch fires when
    // format-kind disagrees with the abi-model.
    auto target = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"spirv","version":"0.0","abiModel":"result-id"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, AbiResolveErrorKind::FormatAbiModelMismatch);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_AbiFormatAbiModelMismatch), 1u);
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
    EXPECT_EQ(abiResolveErrorKindName(AbiResolveErrorKind::CcRegistersInconsistent),
              "CcRegistersInconsistent");
}

TEST(FfiAbiCatalog, DiagnosticCodeNameRoundTripFAbiCcRegistersInconsistent) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_AbiCcRegistersInconsistent),
              "F_AbiCcRegistersInconsistent");
}

// ── D-FF3-Coherence: defense-in-depth at FF3 boundary ─────────
//
// `TargetSchemaData::validate()` already rejects cc rows with
// unresolvable register names at JSON load (target_schema.cpp:875-923).
// But `TargetSchema`'s ctor is PUBLIC + skips validate (see
// target_schema.hpp:1042) — any caller bypassing the JSON loader
// (test fixtures, future `.dsslir` preamble, fuzz harnesses,
// binary-cache reloads) can construct a schema with a paste-error
// cc. The FF3-tier defensive pass below catches the surface at
// `resolveAbi` regardless of how the schema was constructed.
// post-fold #4 silent-failure C1.
//
// Implementation note: we can't use `loadFromText` for these tests
// (validate() catches at load). We construct the failing fixture by
// loading shipped + then validating the FF3-side pass via a
// schema-loader test seam that exercises the same role-iteration
// code path FF3 uses. Until such a test seam exists, the failing
// path is indirectly pinned via the round-trip of the SHIPPED
// targets through resolveAbi (which must succeed without firing
// the new code).

TEST(FfiAbiCatalog, ResolveAbiAcceptsShippedX86_64CcRegisters) {
    // Negative-of-positive pin: every shipped x86_64 cc must have
    // every register name resolvable via target.registerByName.
    // If a future cc row regresses, this test trips.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value())
        << abiResolveErrorKindName(r.error().kind)
        << ": " << r.error().detail;
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_AbiCcRegistersInconsistent), 0u);
}

TEST(FfiAbiCatalog, ResolveAbiAcceptsShippedArm64CcRegisters) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-aarch64-linux");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value())
        << abiResolveErrorKindName(r.error().kind)
        << ": " << r.error().detail;
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_AbiCcRegistersInconsistent), 0u);
}

// Also pin the schema-loader's existing gate so a future loosening
// shows up as a regression here in addition to whatever direct
// tests cover the loader.
TEST(FfiAbiCatalog, SchemaLoaderRejectsPasteErrorRegistersInCc) {
    // Loader-side gate (the FIRST line of defense): a cc row
    // carrying a register name not in target.registers[] fails
    // loadFromText. If this test breaks, the loader-side check
    // was removed; FF3's defense-in-depth still catches.
    auto badTarget = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"x86_64","version":"0.0","abiModel":"register-machine"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ],
      "registers": [
        {"name":"rdi","class":"gpr","widthBytes":8,"hwEncoding":7},
        {"name":"rsp","class":"gpr","widthBytes":8,"hwEncoding":4}
      ],
      "callingConventions": [
        {
          "name":"sysv_amd64",
          "argGprs":["rdi","x0_paste_error_not_in_register_table"],
          "argFprs":[], "returnGprs":["rdi"], "returnFprs":[],
          "callerSaved":[], "calleeSaved":[],
          "stackAlignment":16, "stackPointer":"rsp"
        }
      ]
    })");
    EXPECT_FALSE(badTarget.has_value())
        << "TargetSchema loader must reject a cc carrying a register "
           "name absent from target.registers[].";
}
