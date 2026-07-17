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

#include <string>
#include <string_view>
#include <vector>

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
  "dataModel": "LP64",
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
        R"({"dssObjectFormatVersion":1,
  "dataModel": "LP64","format":{"name":"x","kind":"notafmt"}})");
    ASSERT_FALSE(r.has_value());
}

// ── FC17.9(e) (D-CSUBSET-LONG-DOUBLE): the optional `longDoubleFormat` axis ──

TEST(ObjectFormatSchemaLoader, LongDoubleFormatParsesClosedSet) {
    // Each valid spelling loads and is exposed via the accessor.
    struct Row { char const* spelling; LongDoubleFormat expected; };
    for (Row const row : {Row{"f64", LongDoubleFormat::F64},
                          Row{"x87-80", LongDoubleFormat::X87_80},
                          Row{"ieee128", LongDoubleFormat::Ieee128}}) {
        std::string json{kElfMinimal};
        json.insert(json.rfind('}'),
                    std::string{",\"longDoubleFormat\":\""} + row.spelling + "\"");
        auto r = ObjectFormatSchema::loadFromText(json);
        ASSERT_TRUE(r.has_value()) << row.spelling;
        EXPECT_EQ((*r)->longDoubleFormat(), row.expected) << row.spelling;
    }
    // Omission = None (the honest undeclared state — wasm/spirv).
    auto r = ObjectFormatSchema::loadFromText(kElfMinimal);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->longDoubleFormat(), LongDoubleFormat::None);
}

TEST(ObjectFormatSchemaLoader, UnknownLongDoubleFormatRejected) {
    // A typo'd spelling must be a HARD reject — silently un-declaring the
    // axis would turn every `long double` on the format into a spurious
    // S_LongDoubleFormatUndeclared (the knob-that-lies).
    std::string json{kElfMinimal};
    json.insert(json.rfind('}'), ",\"longDoubleFormat\":\"x87\"");
    EXPECT_FALSE(ObjectFormatSchema::loadFromText(json).has_value());
}

TEST(ObjectFormatSchemaLoader, ShippedFormatsDeclareTheAbiTruthTable) {
    // The §1 ABI truth table, pinned against the shipped JSONs: pe64 + apple
    // arm64 = f64 (long double IS double); SysV/darwin x86_64 = x87-80;
    // linux arm64 = ieee128; wasm/spirv OMIT (None — declared-only formats).
    struct Row { char const* name; LongDoubleFormat expected; };
    for (Row const row : {
             Row{"pe64-x86_64-windows-exec", LongDoubleFormat::F64},
             Row{"pe64-x86_64-windows", LongDoubleFormat::F64},
             Row{"macho64-arm64-darwin-exec", LongDoubleFormat::F64},
             Row{"macho64-arm64-darwin", LongDoubleFormat::F64},
             Row{"macho64-x86_64-darwin-exec", LongDoubleFormat::X87_80},
             Row{"macho64-x86_64-darwin", LongDoubleFormat::X87_80},
             Row{"elf64-x86_64-linux-exec", LongDoubleFormat::X87_80},
             Row{"elf64-x86_64-linux", LongDoubleFormat::X87_80},
             Row{"elf64-aarch64-linux-exec", LongDoubleFormat::Ieee128},
             Row{"elf64-aarch64-linux", LongDoubleFormat::Ieee128},
             Row{"wasm32-v1", LongDoubleFormat::None},
             Row{"spirv-1.6", LongDoubleFormat::None}}) {
        auto r = ObjectFormatSchema::loadShipped(row.name);
        ASSERT_TRUE(r.has_value()) << row.name;
        EXPECT_EQ((*r)->longDoubleFormat(), row.expected) << row.name;
    }
}

TEST(ObjectFormatSchemaLoader, DuplicateRelocationNameRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
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
  "dataModel": "LP64",
      "format": {"name":"x","kind":"elf"},
      "relocations":[{"name":"R_BAD","kind":0}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ObjectFormatSchemaLoader, DuplicateRelocationKindRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
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
  "dataModel": "LP64",
      "format": {"name":"x","kind":"unknown"},
      "relocations":[]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ObjectFormatSchemaLoader, RelocationsNotArrayRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
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

// -- c171 cross-arch variant-parity formats (arm64 .so / arm64 PIE /
//    x86_64 .dylib) -- load + reloc cross-reference --
//
// The 3 new shared-library-family formats load + validate, and their
// relocation rows unify (BY KIND) with the shipped target schema they
// will be emitted against. Mirrors ShippedArm64ElfReloc's cross-check
// (the linker engine resolves reloc rows by the `kind` tag): a typo in
// either the format JSON or the target JSON would otherwise surface
// only at the first image build on that arch.

TEST(ShippedCrossArchVariantFormats, Aarch64DynLoadsAndRoundTripsReloKinds) {
    auto r = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-dyn");
    ASSERT_TRUE(r.has_value());
    auto const& fmt = **r;
    EXPECT_EQ(fmt.name(), "elf64-aarch64-linux-dyn");
    EXPECT_EQ(fmt.kind(), ObjectFormatKind::Elf);
    EXPECT_EQ(fmt.elf().objectType, ElfObjectType::Dyn);
    EXPECT_EQ(fmt.elf().machine, 183);
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

TEST(ShippedCrossArchVariantFormats, Aarch64PieLoadsAndCarriesEntryCluster) {
    auto r = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-pie");
    ASSERT_TRUE(r.has_value());
    auto const& fmt = **r;
    EXPECT_EQ(fmt.name(), "elf64-aarch64-linux-pie");
    EXPECT_EQ(fmt.kind(), ObjectFormatKind::Elf);
    // A PIE IS ET_DYN -- the discriminator is the entry cluster.
    EXPECT_EQ(fmt.elf().objectType, ElfObjectType::Dyn);
    EXPECT_EQ(fmt.elf().machine, 183);
    EXPECT_EQ(fmt.elf().interpreter, "/lib/ld-linux-aarch64.so.1");
    ASSERT_TRUE(fmt.processExit().has_value());
    EXPECT_EQ(fmt.processExit()->mechanism, ExitMechanism::ByNameImport);
    EXPECT_EQ(fmt.entryCallingConvention(), "aapcs64");
    // reloc rows unify with the arm64 target (same 4 as the .so).
    auto tgtR = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(tgtR.has_value());
    auto const& tgt = **tgtR;
    for (auto const* name : {"call26", "adr_prel_pg_hi21",
                              "add_abs_lo12_nc", "abs64"}) {
        auto const* tri = tgt.relocationByName(name);
        ASSERT_NE(tri, nullptr) << name << " missing from arm64.target.json";
        ASSERT_NE(fmt.relocationByKind(tri->kind), nullptr)
            << "format-side has no row for target name=" << name;
    }
}

TEST(ShippedCrossArchVariantFormats, X86_64DylibLoadsAndRoundTripsReloKinds) {
    auto r = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-dylib");
    ASSERT_TRUE(r.has_value());
    auto const& fmt = **r;
    EXPECT_EQ(fmt.name(), "macho64-x86_64-darwin-dylib");
    EXPECT_EQ(fmt.kind(), ObjectFormatKind::MachO);
    EXPECT_EQ(fmt.macho().filetype, MachOObjectType::Dylib);
    // CPU_TYPE_X86_64 = 0x01000007 = 16777223.
    EXPECT_EQ(fmt.macho().cputype, 0x01000007u);
    auto tgtR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(tgtR.has_value());
    auto const& tgt = **tgtR;
    for (auto const* name : {"rel32", "abs64", "abs32"}) {
        auto const* tri = tgt.relocationByName(name);
        ASSERT_NE(tri, nullptr) << name << " missing from x86_64.target.json";
        auto const* fri = fmt.relocationByKind(tri->kind);
        ASSERT_NE(fri, nullptr)
            << "format-side has no row for kind=" << tri->kind.v
            << " (target name=" << name << ")";
    }
}

// ── D-LK10-ENTRY Slice B (plan 14 §2.13): ProcessExit substrate ──
//
// Tests pin both shipped exec format JSONs carrying the new
// `processExit` block + `entryCallingConvention` field, plus the
// JSON loader's per-arm coverage + cross-field validate() rules.

namespace {
// simplifier FOLD-NOW #2 (7425905 audit fold): the rejection-case
// tests all follow `loadFromText(json) + EXPECT_FALSE` shape. One
// helper collapses ~6 sites; the JSON literal stays inline (it IS
// the test fixture).
inline void expectRejected(std::string_view jsonText,
                            std::string_view why) {
    auto r = ObjectFormatSchema::loadFromText(jsonText);
    EXPECT_FALSE(r.has_value()) << why;
}
}  // namespace

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

// STDIO-FLUSH-AT-EXIT (2026-06-09): the shipped ELF exec formats now
// terminate via libc `exit(3)` (a by-name-import that flushes stdio +
// runs atexit), NOT the raw `exit_group(2)` syscall (which skipped the
// flush). This pin asserts the SHIPPED format's new shape; the Syscall
// MECHANISM itself stays a supported emitter capability, exercised by
// the schema-validation tests below + the synthetic-format syscall pins
// in test_lk10_entry_slice_c.cpp.
//
// RED-on-disable: revert elf64-x86_64-linux-exec.format.json's
// processExit back to `mechanism:"syscall"` (the old exit_group) and
// this test fails on the first EXPECT — the shipped policy is libc exit.
TEST(LK10EntrySliceB, ShippedElfExecExitsViaLibcExitImport) {
    auto r = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(r.has_value());
    auto const& pe = (*r)->processExit();
    ASSERT_TRUE(pe.has_value())
        << "ELF Exec must declare processExit (D-LK10-ENTRY Slice B)";
    EXPECT_EQ(pe->mechanism, ExitMechanism::ByNameImport)
        << "ELF exec must terminate via libc exit(3) (flushes stdio), "
           "NOT the raw exit_group syscall (skips the flush).";
    EXPECT_EQ(pe->importMangledName, "exit")
        << "the canonical C name `exit` — ELF adds no leading underscore";
    EXPECT_EQ(pe->importLibraryPath, "libc.so.6")
        << "libc.so.6 becomes a DT_NEEDED + PLT stub for `exit`";
    EXPECT_EQ((*r)->entryCallingConvention(), "sysv_amd64");
    // The aarch64 ELF exec mirrors the x86_64 form (same libc exit).
    auto rArm = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(rArm.has_value());
    auto const& peArm = (*rArm)->processExit();
    ASSERT_TRUE(peArm.has_value());
    EXPECT_EQ(peArm->mechanism, ExitMechanism::ByNameImport);
    EXPECT_EQ(peArm->importMangledName, "exit");
    EXPECT_EQ(peArm->importLibraryPath, "libc.so.6");
    EXPECT_EQ((*rArm)->entryCallingConvention(), "aapcs64");
}

// ── D-CSUBSET-C11-THREADS-MACHO: `librarySynthesis` block ──────────────
// The synth vehicle + import library the compiler-synthesized <threads.h> shim reads.
// RED-on-disable: delete the macho `librarySynthesis` block and a macho threads compile
// fails loud in synthesizeThreadsShim (recipes present, no vehicle).
TEST(LibrarySynthesis, ShippedMachoArm64ExecDeclaresPthreadVehicle) {
    auto r = ObjectFormatSchema::loadShipped("macho64-arm64-darwin-exec");
    ASSERT_TRUE(r.has_value());
    auto const& ls = (*r)->librarySynthesis();
    ASSERT_TRUE(ls.has_value())
        << "macho arm64 exec must declare a librarySynthesis vehicle (D-CSUBSET-C11-THREADS-MACHO)";
    EXPECT_EQ(ls->vehicle, LibrarySynthVehicle::Pthread);
    EXPECT_EQ(ls->libraryPath, "/usr/lib/libSystem.B.dylib");
}

TEST(LibrarySynthesis, ShippedPeExecDeclaresWin32Vehicle) {
    auto r = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(r.has_value());
    auto const& ls = (*r)->librarySynthesis();
    ASSERT_TRUE(ls.has_value()) << "pe64 exec must declare a librarySynthesis vehicle";
    EXPECT_EQ(ls->vehicle, LibrarySynthVehicle::Win32);
    EXPECT_EQ(ls->libraryPath, "kernel32.dll");
}

// ELF omits the block (glibc exports the C11 thread API directly → the synth map is empty).
TEST(LibrarySynthesis, ShippedElfExecOmitsVehicle) {
    auto r = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE((*r)->librarySynthesis().has_value())
        << "elf must NOT declare a synth vehicle (direct libc FFI)";
}

// A PRESENT block is strict: an unknown vehicle is a fail-loud at load (never a silent
// degrade to "no synth support").
TEST(LibrarySynthesis, UnknownVehicleRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": { "name": "x", "version": "1.0", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "relocations": [],
      "librarySynthesis": { "vehicle": "bogus", "libraryPath": "libc.so.6" }
    })");
    EXPECT_FALSE(r.has_value()) << "an unknown librarySynthesis vehicle must fail loud at load";
}

TEST(LibrarySynthesis, MissingLibraryPathRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": { "name": "x", "version": "1.0", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "relocations": [],
      "librarySynthesis": { "vehicle": "pthread" }
    })");
    EXPECT_FALSE(r.has_value()) << "a librarySynthesis block without libraryPath must fail loud";
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
  "dataModel": "LP64",
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
  "dataModel": "LP64",
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
  "dataModel": "LP64",
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
  "dataModel": "LP64",
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
  "dataModel": "LP64",
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
  "dataModel": "LP64",
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

// test-analyzer H1 (7425905 audit fold): pin the `ExitMechanism::
// None` sentinel rejection path explicitly — `UnknownMechanismRejected`
// only drives the `!m.has_value()` arm via "bogus"; the `*m == None`
// branch at object_format_schema_json.cpp's mechanism-resolve was
// dead-coverage.
TEST(LK10EntrySliceB, MechanismNoneStringRejected) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": { "mechanism": "none" }
    })", "mechanism=\"none\" is the sentinel; loader rejects "
         "explicitly to prevent the sentinel from leaking through");
}

// dim-2 HIGH #1 (7425905 audit fold): pin the missing-key path
// distinct from the wrong-value path.
TEST(LK10EntrySliceB, MechanismKeyOmittedRejected) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": { "syscallNumber": 231 }
    })", "processExit object missing the `mechanism` key entirely "
         "must reject (different path from `mechanism=\"bogus\"`)");
}

// test-analyzer H2 (7425905 audit fold): pin the syscallNumGpr-
// missing arm explicitly.
TEST(LK10EntrySliceB, SyscallArmMissingNumGprRejected) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallOpcodeBytes": [15, 5]
      }
    })", "syscall arm without syscallNumGpr must reject");
}

// dim-2 #4 (7425905 audit fold): pin the empty-array
// syscallOpcodeBytes branch explicitly (separate from omitted).
TEST(LK10EntrySliceB, SyscallOpcodeBytesEmptyArrayRejected) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": []
      }
    })", "syscallOpcodeBytes empty array must reject");
}

// test-analyzer M1 (7425905 audit fold): pin the ByNameImport
// missing-mangled-name arm.
TEST(LK10EntrySliceB, ByNameImportArmMissingMangledNameRejected) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth", "version": "0.1", "kind": "pe" },
      "pe": { "machine": 34404, "characteristics": 34 },
      "entryCallingConvention": "ms_x64",
      "processExit": {
        "mechanism": "by-name-import",
        "importLibraryPath": "kernel32.dll"
      }
    })", "by-name-import arm without importMangledName must reject");
}

// silent-failure H1 (7425905 audit fold): pin the isExecFlavor gate.
// Relocatable formats (.o / Obj / Object) cannot have entry
// trampolines; declaring processExit/entryCallingConvention on a
// relocatable schema is meaningless dead data.
TEST(LK10EntrySliceB, ProcessExitOnRelocatableFormatRejected) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth-obj", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62,
               "type": "rel" },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      }
    })", "processExit on ET_REL must reject — only exec-flavored "
         "formats have an entry trampoline");
}

// dim-2 HIGH #2 (7425905 audit fold): entryCallingConvention with
// whitespace silently passed schema-load before the audit;
// loader now rejects.
TEST(LK10EntrySliceB, EntryCcLeadingWhitespaceRejected) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": " sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      }
    })", "leading whitespace in entryCallingConvention must reject "
         "— would silently fail at Slice C callingConventionByName");
}

// dim-2 HIGH #3 (7425905 audit fold): Wasm/SPIR-V format kinds
// have no trampoline emitter; processExit/entryCallingConvention
// are meaningless on those formats. Defense-in-depth.
TEST(LK10EntrySliceB, WasmFormatRejectsProcessExit) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth-wasm", "version": "0.1", "kind": "wasm" },
      "processExit": { "mechanism": "syscall" }
    })", "WASM format must reject `processExit` — no trampoline "
         "emitter applies on operand-stack ABIs");
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

// ── D-RUNTIME-MAIN-ARGC-ARGV (c88): ProcessArgs substrate ───────────
//
// The shipped elf64-*-linux-exec formats declare a `processArgs`
// stack-vector block (SysV AMD64 psABI §3.4.1 / AAPCS64 Linux: argc at
// [sp+0], the in-place argv vector at sp+8) so the entry trampoline
// materializes main's argc/argv. Loader coverage mirrors the
// ProcessExit discipline: closed-enum mechanism, explicit required
// offsets, sentinel rejection, exec-flavor + processExit pairing.

TEST(ProcessArgsSubstrate, ShippedElfExecsDeclareStackVector) {
    for (auto const* name : {"elf64-x86_64-linux-exec",
                             "elf64-aarch64-linux-exec"}) {
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value()) << name;
        auto const& pa = (*r)->processArgs();
        ASSERT_TRUE(pa.has_value())
            << name << " must declare processArgs — without it, "
            "main(argc,argv) reads process-entry register garbage "
            "(D-RUNTIME-MAIN-ARGC-ARGV; the c87 sqlite3 shell crash).";
        EXPECT_EQ(pa->mechanism, ArgsMechanism::StackVector) << name;
        EXPECT_EQ(pa->argcStackOffset, 0u)
            << name << ": argc is the word AT the process-entry SP";
        EXPECT_EQ(pa->argvStackOffset, 8u)
            << name << ": the in-place argv vector starts one machine "
            "word past argc on both LP64 Linux ABIs";
    }
}

TEST(ProcessArgsSubstrate, ShippedMachoExecsDeclareNoneAndPeDeclaresCrtOutParam) {
    // Mach-O: the LC_MAIN entry is CALLED by dyld with argc/argv already in the
    // argument registers — pass-through (no block) IS the correct mechanism. Pin
    // the deliberate absence so an accidental stack-vector block (reading a stack
    // that holds no argv there) fails here before it ships a wild-pointer argv.
    for (auto const* name : {"macho64-x86_64-darwin-exec",
                             "macho64-arm64-darwin-exec"}) {
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value()) << name;
        EXPECT_FALSE((*r)->processArgs().has_value())
            << name << " must NOT declare processArgs — dyld already places "
            "argc/argv in the argument registers at the Mach-O entry.";
    }
    // PE (c111, D-RUNTIME-PE-MAIN-ARGS): unlike ELF/Mach-O, the Windows OS entry
    // receives NO C argument vector, so pe64 DOES declare processArgs — the CRT
    // out-parameter mechanism whose synthesized pre-main init fetches argc/argv via
    // an msvcrt export. Pin the declared shape (mechanism + the wide/narrow export
    // names + the import library) so a descriptor edit that drops or mistypes it
    // fails here before it ships an entry that reads register garbage for argv.
    auto pe = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(pe.has_value());
    auto const& pa = (*pe)->processArgs();
    ASSERT_TRUE(pa.has_value())
        << "pe64 must declare processArgs (the CRT out-parameter args mechanism)";
    EXPECT_EQ(pa->mechanism, ArgsMechanism::CrtOutParam);
    EXPECT_EQ(pa->crtWideArgvFn, "__wgetmainargs");
    EXPECT_EQ(pa->crtNarrowArgvFn, "__getmainargs");
    EXPECT_EQ(pa->crtLibraryPath, "msvcrt.dll");
}

TEST(ProcessArgsSubstrate, UnknownMechanismRejected) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      },
      "processArgs": { "mechanism": "bogus" }
    })", "unknown processArgs.mechanism must reject — closed-enum "
         "vocabulary, a typo can never silently skip argument setup");
}

TEST(ProcessArgsSubstrate, MechanismNoneStringRejected) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      },
      "processArgs": { "mechanism": "none" }
    })", "mechanism=\"none\" is the sentinel; absence is encoded by "
         "omitting the block (the ProcessExit discipline)");
}

TEST(ProcessArgsSubstrate, StackVectorMissingArgcOffsetRejected) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      },
      "processArgs": { "mechanism": "stack-vector",
                       "argvStackOffset": 8 }
    })", "stack-vector arm requires an explicit argcStackOffset — a "
         "silent default would read argc from the wrong slot");
}

TEST(ProcessArgsSubstrate, StackVectorMissingArgvOffsetRejected) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      },
      "processArgs": { "mechanism": "stack-vector",
                       "argcStackOffset": 0 }
    })", "stack-vector arm requires an explicit argvStackOffset");
}

TEST(ProcessArgsSubstrate, OffsetBeyondInt32Rejected) {
    // The offsets feed a MemOffset LIR operand (int32 displacement);
    // a value above 2^31-1 would wrap negative at the cast.
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      },
      "processArgs": { "mechanism": "stack-vector",
                       "argcStackOffset": 0,
                       "argvStackOffset": 2147483648 }
    })", "an offset beyond int32 must reject at load — it would wrap "
         "negative in the trampoline's memory displacement");
}

TEST(ProcessArgsSubstrate, ProcessArgsWithoutProcessExitRejected) {
    // processArgs rides the trampoline emitter, which requires a
    // declared processExit — a processArgs-only format would be dead
    // config whose argument setup silently never emits.
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "entryPoint": "",
      "elf": {
        "class": "elf64", "data": "lsb", "osabi": "sysv", "machine": 62,
        "type": "exec", "pageAlign": 4096,
        "interpreter": "/lib64/ld-linux-x86-64.so.2", "bindNow": true
      },
      "sections": [
        { "kind": "text", "name": ".text", "type": 1, "flags": 6,
          "addrAlign": 16, "entrySize": 0, "virtualAddress": 4198400 }
      ],
      "processArgs": { "mechanism": "stack-vector",
                       "argcStackOffset": 0,
                       "argvStackOffset": 8 }
    })", "processArgs without processExit must reject — the argument "
         "materialization is emitted by the entry trampoline "
         "(D-RUNTIME-MAIN-ARGC-ARGV pairing rule)");
}

TEST(ProcessArgsSubstrate, ProcessArgsOnRelocatableFormatRejected) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth-obj", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62,
               "type": "rel" },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      },
      "processArgs": { "mechanism": "stack-vector",
                       "argcStackOffset": 0,
                       "argvStackOffset": 8 }
    })", "processArgs on ET_REL must reject — relocatable artifacts "
         "have no entry trampoline to materialize arguments in");
}

TEST(ProcessArgsSubstrate, WasmFormatRejectsProcessArgs) {
    expectRejected(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth-wasm", "version": "0.1", "kind": "wasm" },
      "processArgs": { "mechanism": "stack-vector",
                       "argcStackOffset": 0,
                       "argvStackOffset": 8 }
    })", "WASM format must reject `processArgs` — no trampoline "
         "emitter applies on operand-stack ABIs");
}

TEST(ProcessArgsSubstrate, ArgsMechanismEnumRoundTrip) {
    EXPECT_EQ(argsMechanismName(ArgsMechanism::StackVector),
              "stack-vector");
    EXPECT_EQ(argsMechanismName(ArgsMechanism::None), "none");
    EXPECT_EQ(argsMechanismName(ArgsMechanism::CrtOutParam), "crt-out-param");
    EXPECT_EQ(argsMechanismFromName("stack-vector"),
              std::optional{ArgsMechanism::StackVector});
    EXPECT_EQ(argsMechanismFromName("crt-out-param"),
              std::optional{ArgsMechanism::CrtOutParam});
    EXPECT_FALSE(argsMechanismFromName("bogus").has_value());
}

// ── AP3: object-format `artifactProfiles[]` (which profiles a format SERVES) ──

namespace {
// A minimal loadable ELF format with a custom `artifactProfiles` array spliced
// in. Mirrors the cross-validate test's makeElfFormat shape.
std::string elfWithArtifactProfiles(std::string_view profilesArrayJson) {
    return std::string{R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"synth-elf","kind":"elf"},
      "elf": {"class":"elf64","data":"lsb","machine":62},
      "artifactProfiles": )"} + std::string{profilesArrayJson} + R"(
    })";
}
bool sawCodeOFS(auto const& diags, DiagnosticCode c) {
    for (auto const& d : diags) if (d.code == c) return true;
    return false;
}
} // namespace

TEST(ObjectFormatArtifactProfiles, ParsesDeclaredSet) {
    auto r = ObjectFormatSchema::loadFromText(
        elfWithArtifactProfiles(R"(["cli", "lib"])"));
    ASSERT_TRUE(r.has_value());
    auto const served = (*r)->artifactProfiles();
    ASSERT_EQ(served.size(), 2u);
    EXPECT_EQ(served[0], "cli");
    EXPECT_EQ(served[1], "lib");
}

TEST(ObjectFormatArtifactProfiles, AbsentIsEmptyServesNothing) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"synth-elf","kind":"elf"},
      "elf": {"class":"elf64","data":"lsb","machine":62}
    })");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE((*r)->artifactProfiles().empty());
}

// RED-on-disable for the shared-vocab validation (MUST-FIX 2): a typo'd
// profile name a format declares must fail loud at load — disable the
// `isRegisteredArtifactProfile` check and this load would (wrongly) succeed.
TEST(ObjectFormatArtifactProfiles, UnregisteredProfileRejected) {
    auto r = ObjectFormatSchema::loadFromText(
        elfWithArtifactProfiles(R"(["clii"])"));
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(sawCodeOFS(r.error(),
                           DiagnosticCode::C_UnknownArtifactProfile));
}

TEST(ObjectFormatArtifactProfiles, DuplicateProfileRejected) {
    auto r = ObjectFormatSchema::loadFromText(
        elfWithArtifactProfiles(R"(["cli", "cli"])"));
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(sawCodeOFS(r.error(), DiagnosticCode::C_ConflictingField));
}

TEST(ObjectFormatArtifactProfiles, NonArrayRejected) {
    auto r = ObjectFormatSchema::loadFromText(
        elfWithArtifactProfiles(R"("cli")"));
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(sawCodeOFS(r.error(),
                           DiagnosticCode::C_UnknownArtifactProfile));
}

// Integration with the SHIPPED formats: the 4 runnable exec formats serve
// `cli`; the relocatable `.o` format serves nothing (fail-closed).
TEST(ObjectFormatArtifactProfiles, ShippedExecFormatServesCli) {
    auto r = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(r.has_value());
    auto const served = (*r)->artifactProfiles();
    ASSERT_EQ(served.size(), 1u);
    EXPECT_EQ(served[0], "cli");
}

TEST(ObjectFormatArtifactProfiles, ShippedRelocatableFormatServesNothing) {
    auto r = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE((*r)->artifactProfiles().empty());
}

// ── D-FF1-AR-STATICLIB-DRIVER-WIRING (c171): the `container` axis ──────────
//
// The `ObjectFormatContainer` (single/archive) substrate: the 5 shipped
// staticlib formats load + validate as Archives, the validate() guard rejects
// `archive` on an image flavor (RED-ON-DISABLE), the default is Single, and an
// unknown `container` spelling fails loud at load.

namespace {
// True iff this format's served artifact-profile set contains "staticlib".
[[nodiscard]] bool servesStaticlib(ObjectFormatSchema const& fmt) {
    for (auto const& p : fmt.artifactProfiles()) {
        if (p == "staticlib") return true;
    }
    return false;
}
// Count validate() problems whose JSON-pointer path == `want`.
[[nodiscard]] std::size_t
countProblemPath(std::vector<ConfigDiagnostic> const& problems,
                 std::string_view want) {
    std::size_t n = 0;
    for (auto const& d : problems) {
        if (d.path == want) ++n;
    }
    return n;
}
}  // namespace

TEST(StaticLibraryFormats, AllFiveShippedStaticLibFormatsLoadValidateAndAreArchives) {
    for (auto const* name : {"elf64-x86_64-linux-staticlib",
                             "elf64-aarch64-linux-staticlib",
                             "macho64-x86_64-darwin-staticlib",
                             "macho64-arm64-darwin-staticlib",
                             "pe64-x86_64-windows-staticlib"}) {
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value()) << name;
        auto const& fmt = **r;
        EXPECT_EQ(fmt.container(), ObjectFormatContainer::Archive) << name;
        EXPECT_TRUE(fmt.isStaticArchive()) << name;
        // An archive bundles RELOCATABLE members — never an image flavor
        // (the two predicates are mutually exclusive by validate()).
        EXPECT_FALSE(fmt.isImageFlavor()) << name;
        // Serves exactly the `staticlib` artifact profile.
        EXPECT_TRUE(servesStaticlib(fmt)) << name;
    }
}

// RED-ON-DISABLE for the container validate() guard: `container: archive`
// REQUIRES a relocatable member type. Build a minimal valid ELF format by
// value and toggle ONLY elf.objectType between Exec (image, rejected) and Rel
// (relocatable, accepted) — the guard fires on the former and not the latter.
TEST(StaticLibraryFormats, ArchiveContainerRejectedOnImageFlavor) {
    dss::detail::ObjectFormatData data;
    data.name             = "synth-staticlib";
    data.kind             = ObjectFormatKind::Elf;
    data.dataModel        = DataModel::Lp64;
    data.elf.fileClass    = 2;   // ELFCLASS64
    data.elf.dataEncoding = 1;   // ELFDATA2LSB
    data.elf.machine      = 62;  // EM_X86_64
    data.container        = ObjectFormatContainer::Archive;

    // Negative: an image flavor (ET_EXEC) — archive-on-image fails with
    // EXACTLY one `/container` problem coded C_MalformedJson.
    data.elf.objectType = ElfObjectType::Exec;
    auto const execProblems = data.validate();
    EXPECT_EQ(countProblemPath(execProblems, "/container"), 1u)
        << "container: archive on an ELF ET_EXEC (image) must fail at /container";
    for (auto const& d : execProblems) {
        if (d.path == "/container") {
            EXPECT_EQ(d.code, DiagnosticCode::C_MalformedJson);
        }
    }

    // Positive: flip to a RELOCATABLE member (ET_REL) — the SAME archive
    // container is now valid; NO `/container` problem (and the otherwise-
    // minimal ELF ET_REL schema validates cleanly, so the list is empty).
    data.elf.objectType = ElfObjectType::Rel;
    auto const relProblems = data.validate();
    EXPECT_EQ(countProblemPath(relProblems, "/container"), 0u)
        << "container: archive on an ELF ET_REL is valid — no /container problem";
    EXPECT_TRUE(relProblems.empty())
        << "a minimal ELF ET_REL archive format must validate with no problems";
}

TEST(StaticLibraryFormats, ContainerDefaultsToSingle) {
    // Any pre-c171 relocatable format omits `container` — it defaults to
    // Single (byte-identical to before the axis existed).
    auto r = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->container(), ObjectFormatContainer::Single);
    EXPECT_FALSE((*r)->isStaticArchive());
}

TEST(StaticLibraryFormats, UnknownContainerSpellingFailsLoud) {
    // An unknown `container` spelling is a HARD load reject at `/container`
    // (closed enum; a silent fallback to Single would ship a mis-declared
    // static-lib format as a lone object).
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"synth-elf","kind":"elf"},
      "elf": {"class":"elf64","data":"lsb","machine":62},
      "container": "bogus"
    })");
    ASSERT_FALSE(r.has_value());
    bool sawContainer = false;
    for (auto const& d : r.error()) {
        if (d.path == "/container") {
            EXPECT_EQ(d.code, DiagnosticCode::C_MalformedJson);
            sawContainer = true;
        }
    }
    EXPECT_TRUE(sawContainer)
        << "an unknown container spelling must fail loud at /container";
}
