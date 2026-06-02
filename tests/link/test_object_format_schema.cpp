// Object-format substrate tests — plan 14 LK4.
//
// Pins:
//   * Closed-enum Name/FromName round-trip (kObjectFormatKindTable
//     / kSectionKindTable / kSymbolBindingTable /
//     kSymbolVisibilityTable).
//   * JSON loader happy-path: minimal v1 file produces an
//     `ObjectFormatSchema` with the declared name/kind/relocations.
//   * Loader sad-paths: missing version, wrong version, malformed
//     kind, duplicate reloc name, zero-kind reloc, non-array
//     relocations.
//   * `validate()` rejects duplicate kind / zero kind / empty name
//     even when injected through `ObjectFormatData`.
//   * `relocationByKind` / `relocationByName` lookups round-trip and
//     return nullptr on miss.

#include "core/types/parse_diagnostic.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <string_view>

using namespace dss;

// ── Closed-enum round-trip ─────────────────────────────────────────────

TEST(ObjectFormatSchemaEnum, ObjectFormatKindNameRoundTrip) {
    for (auto k : {ObjectFormatKind::Unknown, ObjectFormatKind::Elf,
                   ObjectFormatKind::Pe, ObjectFormatKind::MachO,
                   ObjectFormatKind::Wasm, ObjectFormatKind::Spirv}) {
        auto const name = objectFormatKindName(k);
        ASSERT_FALSE(name.empty());
        auto round = objectFormatKindFromName(name);
        ASSERT_TRUE(round.has_value());
        EXPECT_EQ(*round, k);
    }
    EXPECT_FALSE(objectFormatKindFromName("not-a-format").has_value());
}

TEST(ObjectFormatSchemaEnum, DefaultConstructedKindIsUnknownSentinel) {
    // `Unknown = 0` is the project's universal invalid-sentinel
    // discipline. A default-constructed `ObjectFormatKind{}` must
    // NOT silently claim a real format identity (which would happen
    // if slot 0 were ELF).
    EXPECT_EQ(ObjectFormatKind{}, ObjectFormatKind::Unknown);
}

TEST(ObjectFormatSchemaEnum, SectionKindNameRoundTrip) {
    for (auto k : {SectionKind::Text, SectionKind::Rodata,
                   SectionKind::Data, SectionKind::Bss,
                   SectionKind::Symtab, SectionKind::Strtab,
                   SectionKind::ShStrtab,
                   SectionKind::RelocTable, SectionKind::Dynamic,
                   SectionKind::Note, SectionKind::Debug,
                   SectionKind::Custom}) {
        auto const n = sectionKindName(k);
        ASSERT_FALSE(n.empty());
        EXPECT_EQ(*sectionKindFromName(n), k);
    }
}

TEST(ObjectFormatSchemaEnum, SymbolBindingNameRoundTrip) {
    for (auto b : {SymbolBinding::Local, SymbolBinding::Global,
                   SymbolBinding::Weak}) {
        EXPECT_EQ(*symbolBindingFromName(symbolBindingName(b)), b);
    }
}

TEST(ObjectFormatSchemaEnum, SymbolVisibilityNameRoundTrip) {
    for (auto v : {SymbolVisibility::Default, SymbolVisibility::Hidden,
                   SymbolVisibility::Protected, SymbolVisibility::Internal}) {
        EXPECT_EQ(*symbolVisibilityFromName(symbolVisibilityName(v)), v);
    }
}

// ── JSON loader: happy path ────────────────────────────────────────────

namespace {
constexpr std::string_view kElfMinimal = R"({
  "dssObjectFormatVersion": 1,
  "format": {
    "name": "elf64-x86_64-linux",
    "version": "1.0",
    "kind": "elf"
  },
  "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
  "relocations": [
    { "name": "R_X86_64_PC32",  "kind": 1, "nativeId": 2 },
    { "name": "R_X86_64_PLT32", "kind": 2, "nativeId": 4 }
  ]
})";
} // namespace

TEST(ObjectFormatSchemaLoader, MinimalElfLoads) {
    auto result = ObjectFormatSchema::loadFromText(kElfMinimal);
    ASSERT_TRUE(result.has_value()) << "loader rejected minimal-v1 ELF descriptor";
    auto schema = std::move(result).value();
    EXPECT_EQ(schema->name(), "elf64-x86_64-linux");
    EXPECT_EQ(schema->version(), "1.0");
    EXPECT_EQ(schema->kind(), ObjectFormatKind::Elf);
    EXPECT_EQ(schema->relocationCount(), 2u);
    auto const* pc32 = schema->relocationByName("R_X86_64_PC32");
    ASSERT_NE(pc32, nullptr);
    EXPECT_EQ(pc32->kind, RelocationKind{1u});
    auto const* byKind = schema->relocationByKind(RelocationKind{2u});
    ASSERT_NE(byKind, nullptr);
    EXPECT_EQ(byKind->name, "R_X86_64_PLT32");
    EXPECT_EQ(schema->relocationByKind(RelocationKind{99u}), nullptr);
    EXPECT_EQ(schema->relocationByName("not-declared"), nullptr);
}

// ── JSON loader: sad paths ─────────────────────────────────────────────

TEST(ObjectFormatSchemaLoader, MissingVersionRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({"format":{"name":"x","kind":"elf"}})");
    ASSERT_FALSE(r.has_value());
    bool sawVer = false;
    for (auto const& d : r.error()) {
        if (d.code == DiagnosticCode::C_VersionMismatch) sawVer = true;
    }
    EXPECT_TRUE(sawVer);
}

TEST(ObjectFormatSchemaLoader, WrongVersionRejected) {
    auto r = ObjectFormatSchema::loadFromText(
        R"({"dssObjectFormatVersion":2,"format":{"name":"x","kind":"elf"}})");
    ASSERT_FALSE(r.has_value());
}

TEST(ObjectFormatSchemaLoader, UnknownKindRejected) {
    auto r = ObjectFormatSchema::loadFromText(
        R"({"dssObjectFormatVersion":1,"format":{"name":"x","kind":"notafmt"}})");
    ASSERT_FALSE(r.has_value());
}

TEST(ObjectFormatSchemaLoader, DuplicateRelocationNameRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"x","kind":"elf"},
      "relocations":[
        {"name":"R_FOO","kind":1},
        {"name":"R_FOO","kind":2}
      ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ObjectFormatSchemaLoader, ZeroKindRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"x","kind":"elf"},
      "relocations":[{"name":"R_BAD","kind":0}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ObjectFormatSchemaLoader, DuplicateRelocationKindRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"x","kind":"elf"},
      "relocations":[
        {"name":"R_A","kind":7},
        {"name":"R_B","kind":7}
      ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ObjectFormatSchemaLoader, UnknownSentinelKindRejectedByValidate) {
    // `Unknown` round-trips through the EnumNameTable, so a JSON file
    // could in principle declare `"kind": "unknown"`. The validate
    // pass rejects this — `Unknown` is the invalid sentinel, not a
    // real format declaration.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"x","kind":"unknown"},
      "relocations":[]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ObjectFormatSchemaLoader, RelocationsNotArrayRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"x","kind":"elf"},
      "relocations": "oops"
    })");
    ASSERT_FALSE(r.has_value());
}

// ── Shipped ARM64 ELF format.json files (D-LK6-1 sibling cycle) ──
//
// Pins that the shipped ARM64 ELF JSONs load cleanly AND their
// relocation `name` cross-reference keys round-trip with the shipped
// `arm64.target.json` (which the format.json's nativeId mapping
// resolves against). Without these tests, a typo in either file
// (e.g. "R_AARCH64_CALL26" vs "call26" key collision) would only
// surface at link-time on the first ARM64 image build.

TEST(ShippedArm64ElfReloc, ObjectVariantLoadsAndRoundTripsReloKinds) {
    auto r = ObjectFormatSchema::loadShipped("elf64-aarch64-linux");
    ASSERT_TRUE(r.has_value());
    auto const& fmt = **r;
    EXPECT_EQ(fmt.name(), "elf64-aarch64-linux");

    // Every reloc kind declared in arm64.target.json must have a
    // matching format-side row (linker engine resolves by `kind`
    // tag — the cross-reference IS the kind value).
    auto tgtR = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(tgtR.has_value());
    auto const& tgt = **tgtR;
    for (auto const* name : {"call26", "adr_prel_pg_hi21",
                              "add_abs_lo12_nc", "abs64"}) {
        auto const* tri = tgt.relocationByName(name);
        ASSERT_NE(tri, nullptr) << name << " missing from arm64.target.json";
        auto const* fri = fmt.relocationByKind(tri->kind);
        ASSERT_NE(fri, nullptr)
            << "format-side has no row for kind=" << tri->kind.v
            << " (target name=" << name << ")";
    }
}

TEST(ShippedArm64ElfReloc, ExecVariantLoadsAndCarriesExecFields) {
    auto r = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(r.has_value());
    auto const& fmt = **r;
    EXPECT_EQ(fmt.name(), "elf64-aarch64-linux-exec");
    // exec variant carries entryPoint + interpreter + bindNow.
    EXPECT_FALSE(fmt.elf().interpreter.empty());
    EXPECT_TRUE(fmt.elf().bindNow);
    // .text section has virtualAddress populated (sh_addr).
    auto const* text = fmt.sectionByKind(SectionKind::Text);
    ASSERT_NE(text, nullptr);
    EXPECT_GT(text->virtualAddress, 0u);
}

TEST(ShippedArm64ElfReloc, MachineCodeIsEmAarch64) {
    auto r = ObjectFormatSchema::loadShipped("elf64-aarch64-linux");
    ASSERT_TRUE(r.has_value());
    // EM_AARCH64 = 183 per AArch64 ELF psABI.
    EXPECT_EQ((*r)->elf().machine, 183);
}

// ── D-LK10-ENTRY Slice B (plan 14 §2.13): ProcessExit substrate ──
//
// Tests pin both shipped exec format JSONs carrying the new
// `processExit` block + `entryCallingConvention` field, plus the
// JSON loader's per-arm coverage + cross-field validate() rules.

TEST(LK10EntrySliceB, ShippedPeExecHasByNameImportProcessExit) {
    auto r = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(r.has_value());
    auto const& pe = (*r)->processExit();
    ASSERT_TRUE(pe.has_value())
        << "PE Exec must declare processExit (D-LK10-ENTRY Slice B)";
    EXPECT_EQ(pe->mechanism, ExitMechanism::ByNameImport);
    EXPECT_EQ(pe->importLibraryPath, "kernel32.dll");
    EXPECT_EQ(pe->importMangledName, "ExitProcess");
    EXPECT_EQ((*r)->entryCallingConvention(), "ms_x64");
}

TEST(LK10EntrySliceB, ShippedElfExecHasSyscallProcessExit) {
    auto r = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(r.has_value());
    auto const& pe = (*r)->processExit();
    ASSERT_TRUE(pe.has_value())
        << "ELF Exec must declare processExit (D-LK10-ENTRY Slice B)";
    EXPECT_EQ(pe->mechanism, ExitMechanism::Syscall);
    EXPECT_EQ(pe->syscallNumber, 231u);  // Linux x86_64 exit_group
    EXPECT_EQ(pe->syscallNumGpr, "rax");
    ASSERT_EQ(pe->syscallOpcodeBytes.size(), 2u);
    EXPECT_EQ(pe->syscallOpcodeBytes[0], 0x0F);
    EXPECT_EQ(pe->syscallOpcodeBytes[1], 0x05);
    EXPECT_EQ((*r)->entryCallingConvention(), "sysv_amd64");
}

TEST(LK10EntrySliceB, EntryCallingConventionResolvesAgainstTarget) {
    // Cross-schema invariant: the shipped exec format's
    // `entryCallingConvention` string must resolve to a declared
    // cc on the target schema. Catches a regression where the
    // format JSON declares a typo'd cc name that would silently
    // fail at Slice C trampoline-build time.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto pe = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(pe.has_value());
    auto const* msCc = (*target)->callingConventionByName(
        (*pe)->entryCallingConvention());
    ASSERT_NE(msCc, nullptr)
        << "PE Exec's entryCallingConvention 'ms_x64' must resolve "
           "on the x86_64 target schema's callingConventions[]";
    auto elf = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(elf.has_value());
    auto const* sysvCc = (*target)->callingConventionByName(
        (*elf)->entryCallingConvention());
    ASSERT_NE(sysvCc, nullptr)
        << "ELF Exec's entryCallingConvention 'sysv_amd64' must "
           "resolve on the x86_64 target schema";
}

TEST(LK10EntrySliceB, ProcessExitWithoutEntryCcRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      }
    })");
    EXPECT_FALSE(r.has_value())
        << "processExit without entryCallingConvention must reject "
           "at validate() — the trampoline emitter needs both";
}

TEST(LK10EntrySliceB, EntryCcWithoutProcessExitRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64"
    })");
    EXPECT_FALSE(r.has_value())
        << "entryCallingConvention without processExit must reject "
           "— the fields are paired (D-LK10-ENTRY §2.13)";
}

TEST(LK10EntrySliceB, UnknownMechanismRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": { "mechanism": "bogus" }
    })");
    EXPECT_FALSE(r.has_value())
        << "unknown processExit.mechanism must reject — closed-enum "
           "vocabulary is the only accepted set";
}

TEST(LK10EntrySliceB, SyscallArmMissingNumberRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      }
    })");
    EXPECT_FALSE(r.has_value())
        << "syscall arm requires syscallNumber";
}

TEST(LK10EntrySliceB, ByNameImportArmMissingLibraryRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": { "name": "synth", "version": "0.1", "kind": "pe" },
      "pe": { "machine": 34404, "characteristics": 34 },
      "entryCallingConvention": "ms_x64",
      "processExit": {
        "mechanism": "by-name-import",
        "importMangledName": "ExitProcess"
      }
    })");
    EXPECT_FALSE(r.has_value())
        << "by-name-import arm requires importLibraryPath";
}

TEST(LK10EntrySliceB, OpcodeByteOutOfRangeRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 256]
      }
    })");
    EXPECT_FALSE(r.has_value())
        << "syscallOpcodeBytes entries must fit in u8 (0..255)";
}

TEST(LK10EntrySliceB, RelocatableFormatOmitsProcessExitOk) {
    // The .o (ET_REL) schema correctly omits processExit +
    // entryCallingConvention (relocatable artifacts are not
    // executable; no trampoline applies). Verify the loader
    // accepts this absence without error.
    auto r = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE((*r)->processExit().has_value());
    EXPECT_TRUE((*r)->entryCallingConvention().empty());
}

TEST(LK10EntrySliceB, ExitMechanismEnumRoundTrip) {
    EXPECT_EQ(exitMechanismName(ExitMechanism::Syscall), "syscall");
    EXPECT_EQ(exitMechanismName(ExitMechanism::ByNameImport),
              "by-name-import");
    EXPECT_EQ(exitMechanismName(ExitMechanism::None), "none");
    EXPECT_EQ(exitMechanismFromName("syscall"),
              std::optional{ExitMechanism::Syscall});
    EXPECT_EQ(exitMechanismFromName("by-name-import"),
              std::optional{ExitMechanism::ByNameImport});
    EXPECT_FALSE(exitMechanismFromName("bogus").has_value());
}
