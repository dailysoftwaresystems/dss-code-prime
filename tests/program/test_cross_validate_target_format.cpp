// Plan 14 §3.1 D-LK6-8.2 closure tests — target↔format cross-validation.
//
// ⚠ THE SUBJECT OF THIS FILE CHANGED IN P44 AND THE OLD HEADER DESCRIBED A
// MECHANISM THAT NO LONGER EXISTS. D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES
// deleted `kTargetArchMachineCodes` — a C++ table keyed on (target name ×
// format kind) that was the SECOND OWNER of "arm64 is EM_AARCH64 = 183" — and
// `abiModelMatchesFormatKind`, which decided pairing legality from format
// IDENTITY. Both checks now compare two DECLARATIONS, which is what a validator
// is for. Pins:
//   * matching (target name, format `targetArch`) passes; mismatched fails with
//     D_TargetMachineCodeMismatch — the code keeps its name because the failure
//     CLASS is unchanged, even though no machine NUMBER is compared any more;
//   * a register-machine target paired with a format declaring
//     `cCallingConvention: "none"` fails with D_TargetAbiModelMismatch, and the
//     inverse — the execution-model claim now read off two required keys;
//   * a format declaring no `targetArch` makes no claim and skips the check
//     (the deferral the deleted table had for a target it had no row for);
//   * a brand-new arch is JSON-ONLY — no C++ row, and the wrong pairing still
//     fails loud;
//   * the shipped corpus is self-consistent about arch ↔ machine number.

#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "program/cross_validate_target_format.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

// ⚠ THESE HELPERS NOW VARY `targetArch`, NOT THE MACHINE NUMBER, AND THE
// CHANGE IS THE SUBJECT OF THE ANCHOR RATHER THAN AN ADAPTATION TO IT.
// D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES deleted
// `kTargetArchMachineCodes`, the C++ table that was the second owner of "arm64
// is EM_AARCH64 = 183". With one owner left there is nothing for the validator
// to compare a NUMBER against, so the pairing question is asked the way the
// documents state it: the format declares WHICH target it serves and the
// validator compares that name to the target's own. The machine field is kept
// in each fixture so the document stays realistic; it is simply no longer what
// decides the pairing.
// Synthesize an ELF ObjectFormatSchema claiming to serve `arch`.
std::shared_ptr<ObjectFormatSchema const>
makeElfFormat(std::uint16_t machine, std::string_view arch) {
    std::string const json = std::string{R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "cCallingConvention": { "convention": "sysv_amd64" },
      "outputExtension": ".o",
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "targetArch": ")"} + std::string{arch} + R"(",
      "format": {"name":"synth-elf","kind":"elf"},
      "elf": {"class":"elf64","data":"lsb","machine": )"
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
makePeFormat(std::uint16_t machine, std::string_view arch) {
    std::string const json = std::string{R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "cCallingConvention": { "convention": "ms_x64" },
      "outputExtension": ".obj",
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "targetArch": ")"} + std::string{arch} + R"(",
      "format": {"name":"synth-pe","kind":"pe"},
      "pe": {"machine": )" + std::to_string(machine) + R"(}
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
makeMachOFormat(std::uint32_t cputype, std::string_view arch) {
    std::string const json = std::string{R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
      "cCallingConvention": { "convention": "apple_arm64" },
      "outputExtension": ".o",
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "targetArch": ")"} + std::string{arch} + R"(",
      "format": {"name":"synth-macho","kind":"macho"},
      "macho": {"cputype": )" + std::to_string(cputype) + R"(}
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
    auto format = makeElfFormat(62, "x86_64");  // EM_X86_64
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(CrossValidateTargetFormat, Arm64ElfMatches) {
    auto target = makeTarget("arm64");
    auto format = makeElfFormat(183, "arm64");  // EM_AARCH64
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
    auto format = makeElfFormat(62, "x86_64");  // claims x86_64, paired with arm64
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_FALSE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::D_TargetMachineCodeMismatch));
}

TEST(CrossValidateTargetFormat, X86_64TargetWithArm64FormatFailsLoud) {
    auto target = makeTarget("x86_64");
    auto format = makeElfFormat(183, "arm64");
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_FALSE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::D_TargetMachineCodeMismatch));
}

// ── A format that makes NO CLAIM skips the pairing check ──────
//
// ⚠ THIS TEST'S SUBJECT MOVED WITH THE ANCHOR, AND THE OLD SUBJECT NO LONGER
// EXISTS. It used to be "a target name absent from `kTargetArchMachineCodes`
// defers to format-side validation" — but that table is DELETED
// (D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES), so there is no membership
// to be absent from. The DEFERRAL it protected survives, moved into the
// document: a format that declares no `targetArch` states nothing about which
// target it serves, and the check does not apply. Returning true here still
// reflects "the check doesn't apply", never "validated".
TEST(CrossValidateTargetFormat, AFormatDeclaringNoTargetArchSkipsTheCheck) {
    auto target = makeTarget("riscv64");
    // No `targetArch` key at all — deliberately, and it is the whole subject.
    auto format = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "cCallingConvention": { "convention": "sysv_amd64" },
      "outputExtension": ".o",
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"synth-elf-noclaim","kind":"elf"},
      "elf": {"class":"elf64","data":"lsb","machine": 243}
    })");
    ASSERT_TRUE(target && format.has_value());
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(*target, **format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(CrossValidateTargetFormat, ANewArchIsJsonOnly) {
    // ★ THE PROPERTY THE DELETED TABLE COST US, NOW PINNED. Adding RISC-V used
    // to require a C++ row in `kTargetArchMachineCodes` — this file's own header
    // said so in as many words: "Adding a new arch (RISC-V, PPC64, MIPS) = add a
    // row to `kTargetArchMachineCodes` + the format JSONs declare the matching
    // machine value." Two edits, one of them a rebuild. It is now ONE edit, in
    // JSON, and this test is that claim executed: a target and a format that no
    // C++ in this repository has ever heard of pair cleanly.
    auto target = makeTarget("riscv64");
    auto match  = makeElfFormat(243, "riscv64");
    ASSERT_TRUE(target && match);
    DiagnosticReporter ok;
    EXPECT_TRUE(crossValidateTargetFormat(*target, *match, ok));
    EXPECT_EQ(ok.errorCount(), 0u);

    // CONTROL: the same brand-new arch still REFUSES the wrong format. Without
    // this the test above would pass on a validator that accepted everything.
    auto wrong = makeElfFormat(243, "ppc64");
    ASSERT_TRUE(wrong);
    DiagnosticReporter bad;
    EXPECT_FALSE(crossValidateTargetFormat(*target, *wrong, bad));
    EXPECT_TRUE(sawCode(bad, DiagnosticCode::D_TargetMachineCodeMismatch));
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

// ★ WHAT REPLACED `TableContainsBothV1Arches`. That test asserted the deleted
// C++ table held two rows with the right machine numbers — i.e. it pinned the
// ENGINE'S COPY of a fact the shipped documents also carried, which is the
// second-owner shape the anchor exists to remove; deleting the table deleted
// the thing it measured. What actually needs pinning is the property the table
// enforced: that the shipped corpus is internally consistent about which
// machine number identifies which arch. So the corpus is asked, not a table.
//
// ⚠ THIS IS A NARROWING AND IT IS STATED, NOT HIDDEN. A per-file typo (one
// arm64 ELF descriptor saying 62) is caught here. A SYSTEMATIC error (every
// arm64 ELF descriptor saying 62) is not — the old table would have caught it.
// The residual is covered, deliberately and elsewhere: `tests/link/
// test_elf_writer.cpp` pins the shipped descriptor's `elf().machine` and the
// emitted `e_machine` byte, and no arm64 image would execute on any leg of the
// cross-leg gate. It is a real trade — ONE OWNER for a fact, against one class
// of systematic authoring error — and the project has now made it the same way
// three times (kCManglingRules, kAbiCatalog, this).
TEST(CrossValidateTargetFormat, EveryShippedFormatDeclaresTargetArchConsistently) {
    auto dirR = findShippedConfigDir("object-formats");
    ASSERT_TRUE(dirR.has_value());

    // (targetArch, format kind) -> (machine number, the file that declared it)
    std::map<std::pair<std::string, std::string>,
             std::pair<std::uint64_t, std::string>> seen;
    std::size_t declared = 0;
    std::size_t compared = 0;

    for (auto const& e : std::filesystem::directory_iterator{*dirR}) {
        auto const fn = e.path().filename().string();
        constexpr std::string_view kSuffix = ".format.json";
        if (fn.size() <= kSuffix.size()
            || fn.compare(fn.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
            continue;
        }
        std::string const name = fn.substr(0, fn.size() - kSuffix.size());
        auto format = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(format.has_value()) << name;
        std::string const arch{(*format)->targetArch()};
        EXPECT_FALSE(arch.empty())
            << name << " declares no `targetArch` — every SHIPPED format must "
                       "state which target it serves, or the pairing check "
                       "silently does not apply to it";
        if (arch.empty()) continue;
        ++declared;

        std::string const kindName{objectFormatKindName((*format)->kind())};
        std::optional<std::uint64_t> machine;
        if (kindName == "elf")   machine = (*format)->elf().machine;
        if (kindName == "pe")    machine = (*format)->pe().machine;
        if (kindName == "macho") machine = (*format)->macho().cputype;
        if (!machine.has_value()) continue;   // wasm / spirv carry no number

        auto const key = std::pair{arch, kindName};
        auto const [it, fresh] = seen.try_emplace(key, std::pair{*machine, name});
        if (!fresh) {
            ++compared;
            EXPECT_EQ(*machine, it->second.first)
                << name << " declares targetArch " << arch << " with machine "
                << *machine << ", but " << it->second.second
                << " declares the same arch and kind with " << it->second.first
                << ". One of the two is a typo — an image whose machine number "
                   "disagrees with the arch it claims will not execute.";
        }
    }
    // ⚠ A sweep that compared nothing passes. 24 formats ship and they fall
    // into six (arch, kind) groups, so there are strictly more files than
    // groups and the comparison count cannot legitimately be zero.
    EXPECT_GE(declared, 24u) << "only " << declared << " shipped format(s) "
                                "declared a targetArch";
    EXPECT_GE(compared, 12u) << "only " << compared << " cross-file "
                                "comparison(s) were made — a sweep that pairs "
                                "nothing proves nothing";
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
    auto format = makePeFormat(0x8664, "x86_64");
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(CrossValidateTargetFormat, Arm64TargetWithX86_64PeFailsLoud) {
    auto target = makeTarget("arm64");
    auto format = makePeFormat(0x8664, "x86_64");
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_FALSE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::D_TargetMachineCodeMismatch));
}

// ── Post-fold #1 (pr-test-analyzer P9): Mach-O arm coverage ───

TEST(CrossValidateTargetFormat, X86_64MachOMatches) {
    auto target = makeTarget("x86_64");
    auto format = makeMachOFormat(0x01000007, "x86_64");
    ASSERT_TRUE(target && format);
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(*target, *format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(CrossValidateTargetFormat, Arm64TargetWithX86_64MachOFailsLoud) {
    auto target = makeTarget("arm64");
    auto format = makeMachOFormat(0x01000007, "x86_64");
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
      "cSymbolDecoration": { "scheme": "none" },
      "cCallingConvention": { "convention": "none" },
      "outputExtension": ".wasm",
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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
      "cSymbolDecoration": { "scheme": "none" },
      "cCallingConvention": { "convention": "none" },
      "outputExtension": ".wasm",
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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
      "cSymbolDecoration": { "scheme": "none" },
      "cCallingConvention": { "convention": "none" },
      "outputExtension": ".o",
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"","kind":"elf"}
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ObjectFormatSchemaLoader, WhitespaceFormatNameRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "cCallingConvention": { "convention": "none" },
      "outputExtension": ".o",
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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

// -- c171 cross-arch variant-parity formats: shipped pairs cross-validate --
// The arm64 .so + arm64 PIE cross-validate against the arm64 target;
// the x86_64 .dylib against the x86_64 target. Mirrors ShippedArm64PairsClean
// / ShippedX86_64MachOMatches -- the machine code (elf.machine / macho.cputype)
// must unify with the target's ISA row.

TEST(CrossValidateTargetFormat, ShippedArm64DynMatches) {
    auto target = TargetSchema::loadShipped("arm64");
    auto format = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-dyn");
    ASSERT_TRUE(target.has_value() && format.has_value());
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(**target, **format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(CrossValidateTargetFormat, ShippedArm64PieMatches) {
    auto target = TargetSchema::loadShipped("arm64");
    auto format = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-pie");
    ASSERT_TRUE(target.has_value() && format.has_value());
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(**target, **format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(CrossValidateTargetFormat, ShippedX86_64DylibMatches) {
    auto target = TargetSchema::loadShipped("x86_64");
    auto format = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-dylib");
    ASSERT_TRUE(target.has_value() && format.has_value());
    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateTargetFormat(**target, **format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}
