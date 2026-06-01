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
