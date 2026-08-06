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
#include "format_reject_support.hpp"   // countAtPath / countWithMessage / rejectSummary

#include <gtest/gtest.h>

#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::errorCount;
using dss::link_format::test::countWithMessage;
using dss::link_format::test::rejectSummary;

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
  "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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
  "dataModel": "LP64","headerNameMatching": "case-sensitive",
  "cSymbolDecoration": { "scheme": "none" },
  "format":{"name":"x","kind":"notafmt"}})");
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

// D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT (TF-C52): the arm64 relocatable +
// static-archive formats declare `externAddrBinding: "got"` AND the two
// GOT-address reloc rows (R_AARCH64_ADR_GOT_PAGE kind 7 nativeId 311,
// R_AARCH64_LD64_GOT_LO12_NC kind 8 nativeId 312 — kind-coherent with
// arm64.target.json's `adr_got_page`/`ld64_got_lo12` rows so the assembler's
// `relocationKind` wire resolves).
TEST(ObjectFormatSchemaLoader, Arm64RelocatableFormatsDeclareGotExternAddr) {
    for (char const* name : {"elf64-aarch64-linux",
                             "elf64-aarch64-linux-staticlib"}) {
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value()) << name;
        ASSERT_TRUE((*r)->externAddrBinding().has_value()) << name;
        EXPECT_EQ(*(*r)->externAddrBinding(), ExternAddrBinding::Got) << name;

        auto const* page = (*r)->relocationByName("R_AARCH64_ADR_GOT_PAGE");
        ASSERT_NE(page, nullptr) << name;
        EXPECT_EQ(page->kind, RelocationKind{7}) << name;
        EXPECT_EQ(page->nativeId, 311u) << name;

        auto const* lo12 = (*r)->relocationByName("R_AARCH64_LD64_GOT_LO12_NC");
        ASSERT_NE(lo12, nullptr) << name;
        EXPECT_EQ(lo12->kind, RelocationKind{8}) << name;
        EXPECT_EQ(lo12->nativeId, 312u) << name;
    }
}

// D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT (TF-C52) NEUTRALITY: the DSS-LINKED
// arm64 formats (exec / pie / dyn) do NOT declare externAddrBinding — they
// materialize an extern's address via copy-relocation (exec) / the c117
// DSS-local __got slot (pie/dyn), never the foreign-linker GOT macro. Pins
// that only the relocatable + static-archive formats opted in (closure gate
// #1: exec/pie/dyn output byte-identical).
TEST(ObjectFormatSchemaLoader, DssLinkedArm64FormatsOmitExternAddrBinding) {
    for (char const* name : {"elf64-aarch64-linux-exec",
                             "elf64-aarch64-linux-pie",
                             "elf64-aarch64-linux-dyn"}) {
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value()) << name;
        EXPECT_FALSE((*r)->externAddrBinding().has_value())
            << name << " must NOT declare externAddrBinding (neutrality).";
    }
}

// D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT (TF-C52): loader round-trip + strict
// closed-enum rejection — "got" parses; a typo fails LOUD (never a silent
// degrade to "no GOT-address support"), mirroring externCallDispatch /
// dataImportBinding.
TEST(ObjectFormatSchemaLoader, ExternAddrBindingRoundTripAndRejectsUnknown) {
    auto ok = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": { "name": "x", "version": "1.0", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 183 },
      "externAddrBinding": "got",
      "relocations":[ {"name":"r","kind":1,"nativeId":1} ]
    })");
    ASSERT_TRUE(ok.has_value());
    ASSERT_TRUE((*ok)->externAddrBinding().has_value());
    EXPECT_EQ(*(*ok)->externAddrBinding(), ExternAddrBinding::Got);

    // Identical schema except the externAddrBinding VALUE is a typo — must
    // fail LOUD (the only difference from the valid schema above, so the
    // rejection is attributable to the closed-enum check, not a missing
    // field).
    auto bad = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": { "name": "x", "version": "1.0", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 183 },
      "externAddrBinding": "plt",
      "relocations":[ {"name":"r","kind":1,"nativeId":1} ]
    })");
    EXPECT_FALSE(bad.has_value())
        << "an unknown externAddrBinding value must fail loud.";
}

TEST(ObjectFormatSchemaLoader, DuplicateRelocationNameRejected) {
    // MEASURED: the original fixture (no `elf` identity block, no
    // `nativeId` on either relocation row) never reached the duplicate-
    // name check at all — BOTH rows were dropped first for a missing
    // `nativeId` (unconditionally required on every format-side
    // relocation row; `loadRelocationsTable`'s `extendRow` callback
    // rejects a row missing it BEFORE the name-uniqueness check ever
    // runs), and the missing `elf` block added three more diagnostics on
    // top of that. Repaired so the duplicate name is the fixture's only
    // remaining defect.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"x","kind":"elf"},
      "elf": {"class":"elf64","data":"lsb","machine":62},
      "relocations":[
        {"name":"R_FOO","kind":1,"nativeId":1},
        {"name":"R_FOO","kind":2,"nativeId":2}
      ]
    })");
    ASSERT_FALSE(r.has_value()) << "duplicate relocation name must reject";
    EXPECT_EQ(countAtPath(r, "/relocations/1/name"), 1u) << rejectSummary(r);
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
}

TEST(ObjectFormatSchemaLoader, ZeroKindRejected) {
    // MEASURED: same `nativeId` gap as DuplicateRelocationNameRejected
    // above — without it the loader drops the row for a missing-field
    // reason before `validateRelocationsTable`'s zero-kind sentinel check
    // ever sees it (a dropped row is never appended to `data.relocations`,
    // so the downstream belt-and-suspenders pass has nothing to inspect).
    // Repaired the same way.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"x","kind":"elf"},
      "elf": {"class":"elf64","data":"lsb","machine":62},
      "relocations":[{"name":"R_BAD","kind":0,"nativeId":1}]
    })");
    ASSERT_FALSE(r.has_value())
        << "relocation kind 0 (the invalid sentinel) must reject";
    EXPECT_EQ(countAtPath(r, "/relocations/0/kind"), 1u) << rejectSummary(r);
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
}

TEST(ObjectFormatSchemaLoader, DuplicateRelocationKindRejected) {
    // MEASURED: same `nativeId` gap as the two tests above — both rows
    // were dropped before the duplicate-kind check ever ran. Repaired the
    // same way (distinct `nativeId` per row; `nativeId` itself carries no
    // uniqueness constraint).
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"x","kind":"elf"},
      "elf": {"class":"elf64","data":"lsb","machine":62},
      "relocations":[
        {"name":"R_A","kind":7,"nativeId":1},
        {"name":"R_B","kind":7,"nativeId":2}
      ]
    })");
    ASSERT_FALSE(r.has_value()) << "duplicate relocation kind must reject";
    EXPECT_EQ(countAtPath(r, "/relocations/1/kind"), 1u) << rejectSummary(r);
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
}

TEST(ObjectFormatSchemaLoader, UnknownSentinelKindRejectedByValidate) {
    // ★ `Unknown` round-trips through the EnumNameTable — the sentinel SPELLS
    // CORRECTLY, so `objectFormatKindFromName("unknown")` SUCCEEDS and the
    // loader's name check cannot catch it. `kind` is the load-bearing
    // dispatcher (the cross-kind identity-block guard, the elf/pe/macho/image
    // block readers, FFI C-mangling, the linker's walker selection,
    // `TargetSchema::charIsUnsigned(ObjectFormatKind)`), so a sentinel kind
    // would match NOTHING everywhere at once and take a default arm silently.
    //
    // Asserts the CODE and the MESSAGE, not merely "the load failed" — this
    // fixture is one dataModel typo away from going green for an unrelated
    // reason, and a bare ASSERT_FALSE could never tell the difference.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"x","kind":"unknown"},
      "relocations":[]
    })");
    ASSERT_FALSE(r.has_value());
    bool sawSentinelDiag = false;
    for (auto const& d : r.error()) {
        if (d.path == "/format/kind"
            && d.code == dss::DiagnosticCode::C_MalformedJson
            && d.message.find("sentinel") != std::string::npos) {
            sawSentinelDiag = true;
        }
    }
    EXPECT_TRUE(sawSentinelDiag)
        << "the rejection must come from the kind check and NAME the sentinel";
}

// ★ ONE DEFECT, ONE DIAGNOSTIC. The sentinel is rejected at the load site and
// the parse STOPS there, so the cross-kind identity-block guard never runs
// against it. Without the early return, `kind: unknown` alongside an `elf`
// block also emits "identity block 'elf' is only meaningful when
// format.kind == 'elf' … fix the block name or the format.kind" — advice that
// points at the BLOCK when the single actual defect is the kind, and that would
// have a reader renaming perfectly good blocks.
//
// RED-ON-DISABLE: drop the `isSelectableObjectFormatKind` early-return in
// `object_format_schema_json.cpp` and the identity-block diagnostic appears
// (validate() still fails the load, so a bare ASSERT_FALSE would NOT red —
// which is exactly why this asserts on message content).
TEST(ObjectFormatSchemaLoader, SentinelKindDoesNotCascadeIntoIdentityBlockNoise) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"x","kind":"unknown"},
      "elf": {"machine": 183, "type": "exec"},
      "relocations":[]
    })");
    ASSERT_FALSE(r.has_value());
    for (auto const& d : r.error()) {
        EXPECT_EQ(d.message.find("identity block"), std::string::npos)
            << "a sentinel kind must not spawn per-block advice: " << d.message;
    }
}

TEST(ObjectFormatSchemaLoader, RelocationsNotArrayRejected) {
    // MEASURED: the `elf` identity block is required so the non-array
    // `relocations` value is the fixture's only defect (without it, the
    // missing elf.class / elf.data / elf.machine fields added three more
    // diagnostics).
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"x","kind":"elf"},
      "elf": {"class":"elf64","data":"lsb","machine":62},
      "relocations": "oops"
    })");
    ASSERT_FALSE(r.has_value()) << "'relocations' as a non-array must reject";
    EXPECT_EQ(countAtPath(r, "/relocations"), 1u) << rejectSummary(r);
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
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
//
// UPGRADED (subsumption-sweep hardening, see the vacuity history at the top
// of format_reject_support.hpp): a bare `EXPECT_FALSE(r.has_value())` says
// only "SOMETHING was wrong" — it never says WHICH rule fired. When
// D-LK10-ENTRY added a new load rule, every exec-flavored negative fixture
// in this file was instantly rejected for the NEW reason instead of the one
// it was written to pin, and the suite stayed green while every one of
// those tests went vacuous in place. Taking a `path` forces the test to
// name which diagnostic fired, so the NEXT new rule that starts rejecting a
// fixture for an unrelated reason reds this line instead of hiding behind
// it. `totalErrors` is the other half of the repair: it pins that the load
// fails for ONLY its own defect (or its own defect plus an explicitly
// named, MEASURED knock-on) — never a defect plus a silently-accumulating
// pile of unrelated ones.
inline void expectRejectedAtPath(std::string_view jsonText,
                                 std::string_view path,
                                 std::string_view why,
                                 std::size_t      totalErrors = 1u) {
    auto r = ObjectFormatSchema::loadFromText(jsonText);
    ASSERT_FALSE(r.has_value()) << why;
    EXPECT_EQ(countAtPath(r, path), 1u) << why << " -- " << rejectSummary(r);
    EXPECT_EQ(errorCount(r), totalErrors) << why << " -- " << rejectSummary(r);
}
}  // namespace

// D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3) REPOINTED THIS PAIR from kernel32
// `ExitProcess` to ucrtbase `exit`, and the pin is re-aimed, not relaxed —
// still two EXACT string equalities, so a drift back to the OS primitive
// reds on the first one.
//
// WHY the shipped format had to change: the old spine delivered C's
// return-from-main semantics only as a SIDE EFFECT of msvcrt's DLL-detach
// handler draining the app's onexit table. ucrtbase's detach flushes streams
// but does NOT run that table (in a real MSVC program `exit()` drains it, in
// the static startup code). So once stdlib.json's `atexit` became
// `_crt_atexit`, an `ExitProcess` spine would have left every registered
// handler UNCALLED — silently, with no diagnostic. This row and that macro
// are one change and must stay in lock-step.
//
// RED-on-disable: revert pe64-x86_64-windows-exec.format.json's processExit
// to kernel32/ExitProcess and BOTH string EXPECTs fail here — plus
// examples/c-subset/shipped_atexit goes red end-to-end on the Windows leg,
// which is the runtime witness this unit pin stands in for.
TEST(LK10EntrySliceB, ShippedPeExecHasByNameImportProcessExit) {
    auto r = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(r.has_value());
    auto const& pe = (*r)->processExit();
    ASSERT_TRUE(pe.has_value())
        << "PE Exec must declare processExit (D-LK10-ENTRY Slice B)";
    EXPECT_EQ(pe->mechanism, ExitMechanism::ByNameImport);
    EXPECT_EQ(pe->importLibraryPath, "ucrtbase.dll")
        << "the pe spine terminates through the CRT the program is bound to, "
           "NOT kernel32 (D-FFI-PE-CRT-UCRT-MIGRATION P3)";
    EXPECT_EQ(pe->importMangledName, "exit")
        << "the canonical C name `exit` — pe adds no leading underscore; this "
           "is what drains the `_crt_atexit` table before the streams flush";
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
    // MEASURED: the original fixture omitted `dataModel` and
    // `headerNameMatching` (both REQUIRED on every format), so the load
    // failed for those two reasons in addition to the unknown vehicle.
    // Added so the vehicle check is the fixture's only remaining defect.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": { "name": "x", "version": "1.0", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "relocations": [],
      "librarySynthesis": { "vehicle": "bogus", "libraryPath": "libc.so.6" }
    })");
    ASSERT_FALSE(r.has_value())
        << "an unknown librarySynthesis vehicle must fail loud at load";
    EXPECT_EQ(countAtPath(r, "/librarySynthesis/vehicle"), 1u) << rejectSummary(r);
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
}

TEST(LibrarySynthesis, MissingLibraryPathRejected) {
    // MEASURED: same dataModel/headerNameMatching gap as UnknownVehicleRejected.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": { "name": "x", "version": "1.0", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "relocations": [],
      "librarySynthesis": { "vehicle": "pthread" }
    })");
    ASSERT_FALSE(r.has_value())
        << "a librarySynthesis block without libraryPath must fail loud";
    EXPECT_EQ(countAtPath(r, "/librarySynthesis/libraryPath"), 1u) << rejectSummary(r);
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
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
    // MEASURED knock-on: this fixture's `elf` block declares no `type`, so
    // it defaults to ET_REL (not exec-flavored). `processExit` itself is
    // present and valid, so besides the pairing rule this test pins on
    // `/entryCallingConvention`, validate()'s INDEPENDENT "processExit is
    // only legal on exec-flavored formats" rule also fires on
    // `/processExit` — a second, legitimate rejection from the same
    // deliberately-minimal (non-exec) fixture.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      }
    })", "/entryCallingConvention",
        "processExit without entryCallingConvention must reject "
        "at validate() — the trampoline emitter needs both", 2u);
}

TEST(LK10EntrySliceB, EntryCcWithoutProcessExitRejected) {
    // MEASURED knock-on: the mirror image of ProcessExitWithoutEntryCcRejected
    // above — this ET_REL-default fixture declares `entryCallingConvention`
    // alone, so besides the pairing rule this test pins on `/processExit`,
    // validate()'s independent "entryCallingConvention is only legal on
    // exec-flavored formats" rule also fires on `/entryCallingConvention`.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64"
    })", "/processExit",
        "entryCallingConvention without processExit must reject "
        "— the fields are paired (D-LK10-ENTRY §2.13)", 2u);
}

TEST(LK10EntrySliceB, UnknownMechanismRejected) {
    // MEASURED knock-on: an unrecognized mechanism means `processExit` is
    // never stored (the loader drops the whole block once its own
    // mechanism is invalid), so validate() ALSO fires its
    // processExit/entryCallingConvention pairing rule (`/processExit`) and
    // its exec-flavor rule (`/entryCallingConvention`, since this
    // ET_REL-default fixture is not exec-flavored) — two legitimate
    // knock-ons alongside the mechanism rejection this test pins.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": { "mechanism": "bogus" }
    })", "/processExit/mechanism",
        "unknown processExit.mechanism must reject — closed-enum "
        "vocabulary is the only accepted set", 3u);
}

TEST(LK10EntrySliceB, SyscallArmMissingNumberRejected) {
    // Same 2 knock-ons as UnknownMechanismRejected above: a rejected
    // syscall arm leaves `processExit` unstored, so the pairing +
    // exec-flavor validate() rules fire against the ET_REL-default
    // fixture too.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      }
    })", "/processExit/syscallNumber",
        "syscall arm requires syscallNumber", 3u);
}

TEST(LK10EntrySliceB, ByNameImportArmMissingLibraryRejected) {
    // Same knock-on shape, on the PE side: pe.objectType defaults to Obj
    // (not exec), so the pairing + exec-flavor rules fire alongside the
    // missing-field rejection this test pins.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth", "version": "0.1", "kind": "pe" },
      "pe": { "machine": 34404, "characteristics": 34 },
      "entryCallingConvention": "ms_x64",
      "processExit": {
        "mechanism": "by-name-import",
        "importMangledName": "ExitProcess"
      }
    })", "/processExit/importLibraryPath",
        "by-name-import arm requires importLibraryPath", 3u);
}

TEST(LK10EntrySliceB, OpcodeByteOutOfRangeRejected) {
    // Same 2 knock-ons as SyscallArmMissingNumberRejected.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 256]
      }
    })", "/processExit/syscallOpcodeBytes/1",
        "syscallOpcodeBytes entries must fit in u8 (0..255)", 3u);
}

// test-analyzer H1 (7425905 audit fold): pin the `ExitMechanism::
// None` sentinel rejection path explicitly — `UnknownMechanismRejected`
// only drives the `!m.has_value()` arm via "bogus"; the `*m == None`
// branch at object_format_schema_json.cpp's mechanism-resolve was
// dead-coverage.
TEST(LK10EntrySliceB, MechanismNoneStringRejected) {
    // Same 2 knock-ons as UnknownMechanismRejected: mechanism="none" is
    // rejected at the loader, so processExit is never stored and the
    // pairing + exec-flavor validate() rules fire too.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": { "mechanism": "none" }
    })", "/processExit/mechanism",
        "mechanism=\"none\" is the sentinel; loader rejects "
        "explicitly to prevent the sentinel from leaking through", 3u);
}

// dim-2 HIGH #1 (7425905 audit fold): pin the missing-key path
// distinct from the wrong-value path.
TEST(LK10EntrySliceB, MechanismKeyOmittedRejected) {
    // Same 2 knock-ons.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": { "syscallNumber": 231 }
    })", "/processExit/mechanism",
        "processExit object missing the `mechanism` key entirely "
        "must reject (different path from `mechanism=\"bogus\"`)", 3u);
}

// test-analyzer H2 (7425905 audit fold): pin the syscallNumGpr-
// missing arm explicitly.
TEST(LK10EntrySliceB, SyscallArmMissingNumGprRejected) {
    // Same 2 knock-ons.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallOpcodeBytes": [15, 5]
      }
    })", "/processExit/syscallNumGpr",
        "syscall arm without syscallNumGpr must reject", 3u);
}

// dim-2 #4 (7425905 audit fold): pin the empty-array
// syscallOpcodeBytes branch explicitly (separate from omitted).
TEST(LK10EntrySliceB, SyscallOpcodeBytesEmptyArrayRejected) {
    // Same 2 knock-ons.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": []
      }
    })", "/processExit/syscallOpcodeBytes",
        "syscallOpcodeBytes empty array must reject", 3u);
}

// test-analyzer M1 (7425905 audit fold): pin the ByNameImport
// missing-mangled-name arm.
TEST(LK10EntrySliceB, ByNameImportArmMissingMangledNameRejected) {
    // Same knock-on shape as ByNameImportArmMissingLibraryRejected.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth", "version": "0.1", "kind": "pe" },
      "pe": { "machine": 34404, "characteristics": 34 },
      "entryCallingConvention": "ms_x64",
      "processExit": {
        "mechanism": "by-name-import",
        "importLibraryPath": "kernel32.dll"
      }
    })", "/processExit/importMangledName",
        "by-name-import arm without importMangledName must reject", 3u);
}

// silent-failure H1 (7425905 audit fold): pin the isExecFlavor gate.
// Relocatable formats (.o / Obj / Object) cannot have entry
// trampolines; declaring processExit/entryCallingConvention on a
// relocatable schema is meaningless dead data.
TEST(LK10EntrySliceB, ProcessExitOnRelocatableFormatRejected) {
    // MEASURED: `processExit` and `entryCallingConvention` are BOTH
    // present and individually valid, so validate()'s "only legal on
    // exec-flavored formats" rule fires against EACH of them independently
    // (elf.type is explicitly "rel" here) — `/processExit` (this test's
    // own pin) plus `/entryCallingConvention` as the legitimate second
    // half of the same rule pair.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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
    })", "/processExit",
        "processExit on ET_REL must reject — only exec-flavored "
        "formats have an entry trampoline", 2u);
}

// dim-2 HIGH #2 (7425905 audit fold): entryCallingConvention with
// whitespace silently passed schema-load before the audit;
// loader now rejects.
TEST(LK10EntrySliceB, EntryCcLeadingWhitespaceRejected) {
    // SPECIAL CASE — not routed through expectRejectedAtPath. The leading
    // space is rejected by the LOADER (a whitespace check on the raw
    // string) AND, because a rejected value is never stored, validate()'s
    // processExit/entryCallingConvention pairing rule ALSO fires — at the
    // SAME JSON pointer `/entryCallingConvention`, with a different
    // message. `countAtPath` cannot tell the two apart (MEASURED: it
    // returns 2 at that path), so this uses `countWithMessage` on wording
    // unique to the loader's whitespace check instead — exactly the
    // two-rules-one-pointer situation `format_reject_support.hpp`
    // documents `countWithMessage` for. The third diagnostic
    // (`/processExit`, "only legal on exec-flavored formats") is the same
    // ET_REL-default knock-on seen throughout this suite.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth", "version": "0.1", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
      "entryCallingConvention": " sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      }
    })");
    ASSERT_FALSE(r.has_value())
        << "leading whitespace in entryCallingConvention must reject "
           "— would silently fail at Slice C callingConventionByName";
    EXPECT_EQ(countWithMessage(r, "no whitespace"), 1u) << rejectSummary(r);
    EXPECT_EQ(errorCount(r), 3u) << rejectSummary(r);
}

// dim-2 HIGH #3 (7425905 audit fold): Wasm/SPIR-V format kinds
// have no trampoline emitter; processExit/entryCallingConvention
// are meaningless on those formats. Defense-in-depth.
TEST(LK10EntrySliceB, WasmFormatRejectsProcessExit) {
    // SPECIAL CASE — not routed through expectRejectedAtPath. A
    // well-formed `processExit` block on a wasm format is independently
    // rejected TWICE at the same pointer `/processExit`: once by the
    // wasm/spirv universal-field guard in the loader ("must not declare a
    // top-level 'processExit' field") and once by validate()'s generic
    // "processExit is only legal on exec-flavored formats" rule (wasm is
    // never exec-flavored). `countAtPath` cannot distinguish them
    // (MEASURED: 2 at that path), so this pins the wasm-specific wording
    // via `countWithMessage`. The pairing rule also fires a third,
    // legitimate diagnostic at `/entryCallingConvention` (declared nowhere
    // in this fixture, so `processExit` alone trips it). The processExit
    // block itself is deliberately COMPLETE and valid (unlike the
    // original minimal `{"mechanism":"syscall"}`) so it carries no
    // incidental "missing syscall field" noise of its own — the whole
    // point here is the wasm/exec-flavor rejection, not a malformed arm.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth-wasm", "version": "0.1", "kind": "wasm" },
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      }
    })");
    ASSERT_FALSE(r.has_value())
        << "WASM format must reject `processExit` — no trampoline "
           "emitter applies on operand-stack ABIs";
    EXPECT_EQ(countWithMessage(r, "must not declare a top-level"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(errorCount(r), 3u) << rejectSummary(r);
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
    // Same knock-on shape as LK10EntrySliceB.UnknownMechanismRejected:
    // this ET_REL-default fixture's valid processExit +
    // entryCallingConvention are independently rejected by validate()'s
    // "only legal on exec-flavored formats" rule.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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
    })", "/processArgs/mechanism",
        "unknown processArgs.mechanism must reject — closed-enum "
        "vocabulary, a typo can never silently skip argument setup", 3u);
}

TEST(ProcessArgsSubstrate, MechanismNoneStringRejected) {
    // Same knock-on shape.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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
    })", "/processArgs/mechanism",
        "mechanism=\"none\" is the sentinel; absence is encoded by "
        "omitting the block (the ProcessExit discipline)", 3u);
}

TEST(ProcessArgsSubstrate, StackVectorMissingArgcOffsetRejected) {
    // Same knock-on shape.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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
    })", "/processArgs/argcStackOffset",
        "stack-vector arm requires an explicit argcStackOffset — a "
        "silent default would read argc from the wrong slot", 3u);
}

TEST(ProcessArgsSubstrate, StackVectorMissingArgvOffsetRejected) {
    // Same knock-on shape.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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
    })", "/processArgs/argvStackOffset",
        "stack-vector arm requires an explicit argvStackOffset", 3u);
}

TEST(ProcessArgsSubstrate, OffsetBeyondInt32Rejected) {
    // The offsets feed a MemOffset LIR operand (int32 displacement);
    // a value above 2^31-1 would wrap negative at the cast. Same knock-on
    // shape as the sibling ProcessArgsSubstrate tests above.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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
    })", "/processArgs/argvStackOffset",
        "an offset beyond int32 must reject at load — it would wrap "
        "negative in the trampoline's memory displacement", 3u);
}

TEST(ProcessArgsSubstrate, ProcessArgsWithoutProcessExitRejected) {
    // processArgs rides the trampoline emitter, which requires a
    // declared processExit — a processArgs-only format would be dead
    // config whose argument setup silently never emits.
    //
    // MEASURED knock-on: this fixture IS genuinely exec-flavored (ET_EXEC
    // + pageAlign + a Text section with a non-zero virtualAddress), so it
    // does NOT hit the "only legal on exec-flavored formats" knock-on the
    // sibling ProcessArgsSubstrate tests above do. Instead it hits the
    // INDEPENDENT "exec-flavored format must declare processExit" rule
    // (the ⇒ half of D-LK10-ENTRY's biconditional) — also at
    // `/processExit`, alongside the `/processArgs` pairing rule this test
    // pins. Both trace to the same root fact (no processExit declared),
    // so 2 is the honest measured count.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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
    })", "/processArgs",
        "processArgs without processExit must reject — the argument "
        "materialization is emitted by the entry trampoline "
        "(D-RUNTIME-MAIN-ARGC-ARGV pairing rule)", 2u);
}

TEST(ProcessArgsSubstrate, ProcessArgsOnRelocatableFormatRejected) {
    // MEASURED: `processExit`, `entryCallingConvention` AND `processArgs`
    // are all present and individually valid, so validate()'s "only legal
    // on exec-flavored formats" rule fires against BOTH `/processExit`
    // and `/entryCallingConvention` in addition to the `/processArgs`
    // rejection this test pins — three independent firings of
    // structurally the same rule, one per field that declares entry-
    // trampoline machinery on an explicit ET_REL.
    expectRejectedAtPath(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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
    })", "/processArgs",
        "processArgs on ET_REL must reject — relocatable artifacts "
        "have no entry trampoline to materialize arguments in", 3u);
}

TEST(ProcessArgsSubstrate, WasmFormatRejectsProcessArgs) {
    // SPECIAL CASE — mirrors LK10EntrySliceB.WasmFormatRejectsProcessExit.
    // A well-formed `processArgs` block on a wasm format is rejected
    // THREE times at the SAME pointer `/processArgs`: the wasm/spirv
    // universal-field guard ("must not declare a top-level 'processArgs'
    // field"), validate()'s "processArgs without processExit" pairing
    // rule (no processExit is declared in this fixture), and validate()'s
    // "only legal on exec-flavored formats" rule. `countAtPath` cannot
    // distinguish these (MEASURED: 3 at that path), so this pins the
    // wasm-specific wording via `countWithMessage`.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": { "name": "synth-wasm", "version": "0.1", "kind": "wasm" },
      "processArgs": { "mechanism": "stack-vector",
                       "argcStackOffset": 0,
                       "argvStackOffset": 8 }
    })");
    ASSERT_FALSE(r.has_value())
        << "WASM format must reject `processArgs` — no trampoline "
           "emitter applies on operand-stack ABIs";
    EXPECT_EQ(countWithMessage(r, "must not declare a top-level"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(errorCount(r), 3u) << rejectSummary(r);
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
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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
    // D-PP-HEADER-CASE-INSENSITIVE-PE: like `dataModel`, the header-name case
    // rule is REQUIRED and its zero value is the INVALID sentinel, so a
    // hand-built ObjectFormatData must set it or `validate()` reports it (which
    // is the point — see HeaderNameMatchingIsRequired).
    data.headerNameMatching = HeaderNameMatching::CaseSensitive;
    // D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN: the same story one axis over —
    // REQUIRED, zero == the INVALID sentinel, and enforced on THIS (hand-built,
    // loader-bypassing) path by validate() alone. Left unset, the `archive`
    // guard below would still fire but `relProblems.empty()` would red for a
    // reason that has nothing to do with containers.
    data.cSymbolDecoration.scheme = CSymbolDecorationScheme::None;
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
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
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

// ── D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: `stackReserveControl` ─────────────
//
// WHETHER a format can carry a per-PROGRAM stack reserve, WHERE it lands
// (the closed `vehicle` verb) and the legal request range. PRESENCE is the
// capability — the linker gate asks `stackReserveControl().has_value()`,
// never a format identity — so the shipped pins below are the "capability,
// not kind" statement: the pe64 **exec** declares it while its same-kind
// **dll** sibling (identical header struct, but the loader ignores a DLL's
// copy) declares nothing.
//
// A PRESENT block is strict: the vehicle is a closed verb, all three bounds
// are REQUIRED, and every internal-coherence rule is a LOAD reject. A
// defaulted or silently-degraded block would admit an absurd request, and a
// vehicle whose walker never runs would silently DROP one — the two failure
// modes this whole capability exists to eliminate.

namespace {

// `base` + a `stackReserveControl` block, spliced into a schema that is
// otherwise VALID (the positive control below asserts the base loads
// cleanly with a good block). Every rejection is therefore attributable to
// the block alone — nothing else in the document can be at fault.
std::string peWithStackReserveControl(std::string_view blockJson) {
    return std::string{R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "LLP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"synth-pe","version":"1.0","kind":"pe"},
      "pe": {"machine": 34404},
      "stackReserveControl": )"} + std::string{blockJson} + R"(
    })";
}

std::string elfWithStackReserveControl(std::string_view blockJson) {
    return std::string{R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"synth-elf","version":"1.0","kind":"elf"},
      "elf": {"class":"elf64","data":"lsb","machine":62},
      "stackReserveControl": )"} + std::string{blockJson} + R"(
    })";
}

// The one block every rejection case is a single-field mutation of.
constexpr std::string_view kValidPeStackReserveBlock = R"({
        "vehicle": "pe-optional-header",
        "minimumBytes": 65536,
        "maximumBytes": 4294967296,
        "granularityBytes": 4096
      })";

// Reject + EXACT attribution: the load fails AND carries exactly one
// diagnostic at `path`, coded `code`. Stronger than the file's
// `expectRejected` (bare `EXPECT_FALSE`), which would still pass if the
// loader rejected the document for a completely unrelated reason.
void expectRejectedAt(std::string_view jsonText, std::string_view path,
                      DiagnosticCode code, std::string_view why) {
    auto r = ObjectFormatSchema::loadFromText(jsonText);
    ASSERT_FALSE(r.has_value()) << why;
    std::size_t n = 0;
    for (auto const& d : r.error()) {
        if (d.path == path) {
            ++n;
            EXPECT_EQ(d.code, code) << why << " (at " << path << ")";
        }
    }
    EXPECT_EQ(n, 1u) << why << " -- expected exactly one diagnostic at "
                     << path;
}

}  // namespace

TEST(StackReserveControl, ShippedPeExecDeclaresCapabilityWithExactBounds) {
    // The SHIPPED numbers, pinned field-by-field. minimumBytes = the
    // Windows 64 KiB allocation granularity; maximumBytes = 4 GiB (a
    // reserve is VIRTUAL address space, so the cap exists to fail a
    // fat-fingered value loud rather than ship an unstartable image);
    // granularityBytes = one page. RED-on-disable: edit any number in
    // pe64-x86_64-windows-exec.format.json and this fails before the
    // changed range can silently admit or refuse a request.
    auto r = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(r.has_value());
    auto const src = (*r)->stackReserveControl();
    ASSERT_TRUE(src.has_value())
        << "pe64 exec must declare stackReserveControl -- without it every "
           "per-program request fails K_FormatLacksStackReserveControl";
    EXPECT_EQ(src->vehicle, StackReserveVehicle::PeOptionalHeader);
    EXPECT_EQ(src->minimumBytes, 65536ull);
    EXPECT_EQ(src->maximumBytes, 4294967296ull);
    EXPECT_EQ(src->granularityBytes, 4096ull);
    // The capability declares the RANGE; the optional header declares the
    // DEFAULT a request overrides. Both must hold for the override
    // semantics to mean anything.
    EXPECT_EQ((*r)->peOptionalHeader().sizeOfStackReserve, 1048576ull)
        << "1 MiB (the MSVC/Windows convention) is the default a request "
           "replaces";
    // The declared default must itself be inside the declared range —
    // otherwise the no-request path emits a value the request path would
    // refuse.
    EXPECT_GE((*r)->peOptionalHeader().sizeOfStackReserve, src->minimumBytes);
    EXPECT_LE((*r)->peOptionalHeader().sizeOfStackReserve, src->maximumBytes);
}

TEST(StackReserveControl, ShippedSiblingsDeclareNoCapability) {
    // "Capability, not kind." The pe64 **dll** is the load-bearing row: it
    // is the SAME `kind: pe` as the exec and carries the SAME
    // IMAGE_OPTIONAL_HEADER64.SizeOfStackReserve field, yet declares
    // NOTHING — the Windows loader ignores a DLL's copy, so writing it
    // would be a silent no-op knob. The `.obj` has no optional header at
    // all, and no ELF/Mach-O image field carries a stack SIZE
    // (PT_GNU_STACK encodes EXECUTABILITY; the size is a runtime
    // `ulimit -s` property).
    for (auto const* name : {"pe64-x86_64-windows-dll",
                             "pe64-x86_64-windows",
                             "pe64-x86_64-windows-staticlib",
                             "elf64-x86_64-linux-exec",
                             "elf64-aarch64-linux-exec",
                             "elf64-x86_64-linux",
                             "macho64-x86_64-darwin-exec",
                             "macho64-arm64-darwin-exec"}) {
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value()) << name;
        EXPECT_FALSE((*r)->stackReserveControl().has_value())
            << name << " must NOT declare stackReserveControl -- a request "
                       "against it has to fail LOUD, never be dropped";
    }
    // Spelled out: same kind, same header field, opposite answer.
    auto dll = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-dll");
    ASSERT_TRUE(dll.has_value());
    EXPECT_EQ((*dll)->kind(), ObjectFormatKind::Pe);
    EXPECT_EQ((*dll)->peOptionalHeader().sizeOfStackReserve, 1048576ull)
        << "the dll HAS the field (it just isn't honoured) -- which is "
           "precisely why the capability cannot be inferred from the kind";
}

TEST(StackReserveControl, VehicleEnumRoundTrip) {
    EXPECT_EQ(stackReserveVehicleName(StackReserveVehicle::PeOptionalHeader),
              "pe-optional-header");
    EXPECT_EQ(stackReserveVehicleFromName("pe-optional-header"),
              std::optional{StackReserveVehicle::PeOptionalHeader});
    // A vehicle ships only WITH the walker arm that writes it — the
    // natural second row (Mach-O LC_MAIN.stacksize) is NOT vocabulary yet.
    EXPECT_FALSE(stackReserveVehicleFromName("macho-lc-main").has_value());
    EXPECT_FALSE(stackReserveVehicleFromName("").has_value());
}

TEST(StackReserveControl, ValidBlockLoadsAndIsExposedVerbatim) {
    // The positive control for every rejection case below: this exact
    // base + block loads cleanly, so each sad path differs from a LOADING
    // document in exactly one field.
    auto r = ObjectFormatSchema::loadFromText(
        peWithStackReserveControl(kValidPeStackReserveBlock));
    ASSERT_TRUE(r.has_value())
        << "the sad-path base schema must itself be valid";
    auto const src = (*r)->stackReserveControl();
    ASSERT_TRUE(src.has_value());
    EXPECT_EQ(src->vehicle, StackReserveVehicle::PeOptionalHeader);
    EXPECT_EQ(src->minimumBytes, 65536ull);
    EXPECT_EQ(src->maximumBytes, 4294967296ull);
    EXPECT_EQ(src->granularityBytes, 4096ull);

    // Omitting the block entirely is the honest "no capability" state —
    // never an error, never a defaulted-in block.
    auto none = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "LLP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"synth-pe","version":"1.0","kind":"pe"},
      "pe": {"machine": 34404}
    })");
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE((*none)->stackReserveControl().has_value());
}

TEST(StackReserveControl, UnknownVehicleRejected) {
    expectRejectedAt(
        peWithStackReserveControl(R"({
        "vehicle": "pe-optional-headers",
        "minimumBytes": 65536,
        "maximumBytes": 4294967296,
        "granularityBytes": 4096
      })"),
        "/stackReserveControl/vehicle", DiagnosticCode::C_MalformedJson,
        "an unknown vehicle must fail loud -- a silent degrade to 'no "
        "capability' would turn every request into a spurious refusal");
}

TEST(StackReserveControl, MissingVehicleRejected) {
    expectRejectedAt(
        peWithStackReserveControl(R"({
        "minimumBytes": 65536,
        "maximumBytes": 4294967296,
        "granularityBytes": 4096
      })"),
        "/stackReserveControl/vehicle", DiagnosticCode::C_MissingField,
        "vehicle is REQUIRED -- a defaulted vehicle would route a request "
        "to a walker arm the format never claimed");
}

TEST(StackReserveControl, EachMissingBoundRejected) {
    // All three bounds are REQUIRED: a capability that does not state its
    // own legal range cannot validate a request, and a defaulted range
    // would silently admit an absurd value. One arm per bound so a
    // regression that drops only ONE required-check is caught.
    struct Row { char const* block; char const* path; char const* why; };
    for (Row const row : {
             Row{R"({
        "vehicle": "pe-optional-header",
        "maximumBytes": 4294967296,
        "granularityBytes": 4096
      })",
                 "/stackReserveControl/minimumBytes",
                 "minimumBytes is required"},
             Row{R"({
        "vehicle": "pe-optional-header",
        "minimumBytes": 65536,
        "granularityBytes": 4096
      })",
                 "/stackReserveControl/maximumBytes",
                 "maximumBytes is required"},
             Row{R"({
        "vehicle": "pe-optional-header",
        "minimumBytes": 65536,
        "maximumBytes": 4294967296
      })",
                 "/stackReserveControl/granularityBytes",
                 "granularityBytes is required"}}) {
        expectRejectedAt(peWithStackReserveControl(row.block), row.path,
                         DiagnosticCode::C_MissingField, row.why);
    }
}

TEST(StackReserveControl, NegativeBoundRejectedNeverWrapsToHugeU64) {
    // The `is_number_unsigned` pin. nlohmann types `-1` as a SIGNED
    // integer, so a `get<std::uint64_t>()` on it would WRAP to
    // 18446744073709551615 and silently declare a 16-exabyte maximum. One
    // arm per bound — the check lives in a shared lambda, but a
    // regression could plausibly bypass it for a single call site.
    for (auto const* key : {"minimumBytes", "maximumBytes",
                            "granularityBytes"}) {
        std::string block = R"({
        "vehicle": "pe-optional-header",
        "minimumBytes": 65536,
        "maximumBytes": 4294967296,
        "granularityBytes": 4096
      })";
        // Retarget the one field under test to -1.
        auto const pos = block.find(std::string{"\""} + key + "\": ");
        ASSERT_NE(pos, std::string::npos) << key;
        auto const valueStart = block.find(':', pos) + 2u;
        auto const valueEnd   = block.find_first_of(",\n}", valueStart);
        block.replace(valueStart, valueEnd - valueStart, "-1");
        expectRejectedAt(peWithStackReserveControl(block),
                         std::string{"/stackReserveControl/"} + key,
                         DiagnosticCode::C_MalformedJson,
                         "a NEGATIVE bound must reject -- it must never wrap "
                         "into a huge u64");
    }
}

TEST(StackReserveControl, ZeroGranularityRejected) {
    expectRejectedAt(
        peWithStackReserveControl(R"({
        "vehicle": "pe-optional-header",
        "minimumBytes": 65536,
        "maximumBytes": 4294967296,
        "granularityBytes": 0
      })"),
        "/stackReserveControl/granularityBytes",
        DiagnosticCode::C_MalformedJson,
        "granularityBytes 0 is a modulo-by-zero at the request gate and "
        "admits nothing");
}

TEST(StackReserveControl, ZeroMinimumRejected) {
    expectRejectedAt(
        peWithStackReserveControl(R"({
        "vehicle": "pe-optional-header",
        "minimumBytes": 0,
        "maximumBytes": 4294967296,
        "granularityBytes": 4096
      })"),
        "/stackReserveControl/minimumBytes", DiagnosticCode::C_MalformedJson,
        "a zero-byte stack reserve cannot start a program");
}

TEST(StackReserveControl, MaximumBelowMinimumRejected) {
    expectRejectedAt(
        peWithStackReserveControl(R"({
        "vehicle": "pe-optional-header",
        "minimumBytes": 65536,
        "maximumBytes": 4096,
        "granularityBytes": 4096
      })"),
        "/stackReserveControl/maximumBytes", DiagnosticCode::C_MalformedJson,
        "an inverted range admits NO value -- every request would be "
        "refused for the wrong reason");
}

TEST(StackReserveControl, MinimumNotMultipleOfGranularityRejected) {
    expectRejectedAt(
        peWithStackReserveControl(R"({
        "vehicle": "pe-optional-header",
        "minimumBytes": 65537,
        "maximumBytes": 4294967296,
        "granularityBytes": 4096
      })"),
        "/stackReserveControl/minimumBytes", DiagnosticCode::C_MalformedJson,
        "a minimumBytes off the granularity makes the smallest legal "
        "request unreachable");
}

TEST(StackReserveControl, PeVehicleOnElfSchemaRejected) {
    // Vehicle ↔ format.kind coherence. A vehicle NAMES a structure of ONE
    // image format, so `pe-optional-header` on an ELF schema is dead
    // config whose request the ELF walker would silently DROP — the exact
    // silent-drop this capability exists to prevent. RED-on-disable:
    // delete the kVehicleKinds table walk in object_format_schema_json.cpp
    // and this schema loads, arming a stack-reserve request that never
    // reaches any header.
    expectRejectedAt(elfWithStackReserveControl(kValidPeStackReserveBlock),
                     "/stackReserveControl/vehicle",
                     DiagnosticCode::C_MalformedJson,
                     "a PE vehicle on an ELF schema must fail loud");
}

TEST(StackReserveControl, WasmSchemaRejectsTheWholeBlock) {
    // WASM/SPIR-V have no image field of this shape at all (a WASM
    // module's stack is the embedder's), so the block joins the
    // universal-field reject list: a top-level declaration is dead data
    // the walker would silently ignore.
    expectRejectedAt(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "ILP32",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"synth-wasm","version":"0.1","kind":"wasm"},
      "stackReserveControl": {
        "vehicle": "pe-optional-header",
        "minimumBytes": 65536,
        "maximumBytes": 4294967296,
        "granularityBytes": 4096
      }
    })",
                     "/stackReserveControl", DiagnosticCode::C_MalformedJson,
                     "a wasm schema must reject stackReserveControl -- no "
                     "such image field exists to carry it");
}

TEST(StackReserveControl, NonObjectBlockRejected) {
    expectRejectedAt(peWithStackReserveControl("4194304"),
                     "/stackReserveControl",
                     DiagnosticCode::C_MalformedJson,
                     "a scalar in place of the block must fail loud rather "
                     "than be read as a bare byte count");
}

// ─────────────────────────────────────────────────────────────────────────────
// The FORMAT loader's closed root-key vocabulary (the last of the three loader
// families to be closed; the language family closed in TF-C72, the target
// family in TF-C74) — plus the pin that keeps bare-`char` signedness OUT of
// this schema for good (TF-C75, D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] bool anyHasMalformedJson(
    std::vector<ConfigDiagnostic> const& diags) {
    for (auto const& d : diags) {
        if (d.code == DiagnosticCode::C_MalformedJson) return true;
    }
    return false;
}

// True iff SOME diagnostic message mentions `needle`. Used to pin that the typo
// discriminator NAMES the offending key — a rejection that does not say which
// key is wrong sends the reader back to diffing 24 files by hand.
[[nodiscard]] bool anyMessageMentions(
    std::vector<ConfigDiagnostic> const& diags, std::string_view needle) {
    for (auto const& d : diags) {
        if (d.message.find(needle) != std::string::npos) return true;
        if (d.path.find(needle) != std::string::npos) return true;
    }
    return false;
}

// kElfMinimal with one extra root key spliced in.
[[nodiscard]] std::string withRootKey(std::string_view keyAndValue) {
    std::string json{kElfMinimal};
    json.insert(json.rfind('}'), std::string{","} + std::string{keyAndValue});
    return json;
}

}  // namespace

// ★ THE ANTI-RESURRECTION PIN (TF-C75). Bare-`char` signedness briefly lived
// HERE as a `charSignedness` tri-state, in parallel with the target's own
// `charIsUnsigned` — two sources of truth for one fact, reconciled by a free
// function. It was collapsed onto the TARGET, which now declares the whole
// (processor × platform) fact in one key.
//
// The format side had to go rather than the target side: `elf` serves BOTH
// aarch64 (unsigned) and x86_64 (signed), so any flat value here is a lie on
// one of them, and making it honest would force all 24 format files to
// enumerate CPU architectures — a layering inversion.
//
// This test is what stops it coming back QUIETLY. Without the closed
// vocabulary, a re-added `"charSignedness": "signed"` would load perfectly
// clean, be read by nothing, and sit in the tree looking authoritative while
// the real answer came from somewhere else entirely. RED-ON-DISABLE: put
// `charSignedness` back into `kFormatDocumentKeys` and this fails.
TEST(FormatRootKeyVocabulary, CharSignednessIsRejectedOnAFormatSchema) {
    auto r = ObjectFormatSchema::loadFromText(
        withRootKey(R"("charSignedness":"signed")"));
    ASSERT_FALSE(r.has_value())
        << "bare-`char` signedness is declared ENTIRELY on the target "
           "(`charIsUnsigned`), so a format schema re-declaring it must be "
           "REJECTED — a second source of truth that nothing reads is worse "
           "than none";
    EXPECT_TRUE(anyHasMalformedJson(r.error()));
    EXPECT_TRUE(anyMessageMentions(r.error(), "charSignedness"))
        << "the diagnostic must NAME the offending key";
}

// ★ This is the pin that makes every OTHER format key honest, and the format
// family is the highest-stakes of the three: its keys carry SILENT-MISCOMPILE
// semantics. Before TF-C75 the loader read every root key through a bare
// `doc.contains(…)` and ignored unknowns, so a typo'd `"bitFieldStrateg"` would
// have loaded perfectly clean, never been read, and the engine would have
// silently fallen back to the TARGET's bit-field strategy — a wrong-LAYOUT
// miscompile with no diagnostic.
//
// RED-ON-DISABLE: delete the `kFormatDocumentKeys` loop in
// object_format_schema_json.cpp and this test fails while the rest of the file
// still passes.
TEST(FormatRootKeyVocabulary, UnknownRootKeyRejectedAndNamed) {
    auto r = ObjectFormatSchema::loadFromText(
        withRootKey(R"("dataModle":"LP64")"));
    ASSERT_FALSE(r.has_value())
        << "a misspelled root key must be REJECTED — silently ignoring it makes "
           "every optional format key a knob that can lie";
    EXPECT_TRUE(anyHasMalformedJson(r.error()));
    EXPECT_TRUE(anyMessageMentions(r.error(), "dataModle"))
        << "the diagnostic must NAME the offending key";
}

// The same guard over a sibling key whose silent no-op is a wrong-LAYOUT
// miscompile rather than a wrong-value one — the class the vocabulary protects
// is the whole key set, not just any one member.
TEST(FormatRootKeyVocabulary, MisspelledBitFieldStrategyRejected) {
    auto r = ObjectFormatSchema::loadFromText(
        withRootKey(R"("bitFieldStrateg":"gnu_packed")"));
    ASSERT_FALSE(r.has_value())
        << "a typo'd bitFieldStrategy would silently fall back to the TARGET's "
           "strategy — a wrong-layout miscompile with no diagnostic";
    EXPECT_TRUE(anyHasMalformedJson(r.error()));
}

// The `$`-prefix documentation carve-out is MANDATORY, not decorative: the
// shipped format files use `$comment` / `$…Comment` heavily (MEASURED: 20
// distinct `$`-prefixed root keys across the 24 shipped files, `$comment` in
// all 24), so without it this guard would reject every shipped format.
TEST(FormatRootKeyVocabulary, DollarPrefixedRootKeysStillAccepted) {
    auto r = ObjectFormatSchema::loadFromText(
        withRootKey(R"("$comment":"prose, not config",
                       "$dataModelComment":"why LP64 here")"));
    ASSERT_TRUE(r.has_value())
        << "`$`-prefixed keys are the codebase-wide documentation convention "
           "and must survive the typo discriminator";
}

// EVERY shipped format must load clean under the closed vocabulary — all 24,
// not a sample. This is exactly the multi-site class where a green subset hides
// the miss: the regression it catches is adding a root key to a `.format.json`
// without adding it to `kFormatDocumentKeys`, or vice versa.
TEST(FormatRootKeyVocabulary, AllShippedFormatsLoadCleanUnderClosedVocabulary) {
    constexpr std::string_view kAll[] = {
        "elf64-aarch64-linux-dyn",      "elf64-aarch64-linux-exec",
        "elf64-aarch64-linux-pie",      "elf64-aarch64-linux-staticlib",
        "elf64-aarch64-linux",          "elf64-x86_64-linux-dyn",
        "elf64-x86_64-linux-exec",      "elf64-x86_64-linux-pie",
        "elf64-x86_64-linux-staticlib", "elf64-x86_64-linux",
        "macho64-arm64-darwin-dylib",   "macho64-arm64-darwin-exec",
        "macho64-arm64-darwin-staticlib", "macho64-arm64-darwin",
        "macho64-x86_64-darwin-dylib",  "macho64-x86_64-darwin-exec",
        "macho64-x86_64-darwin-staticlib", "macho64-x86_64-darwin",
        "pe64-x86_64-windows-dll",      "pe64-x86_64-windows-exec",
        "pe64-x86_64-windows-staticlib", "pe64-x86_64-windows",
        "spirv-1.6",                    "wasm32-v1"};
    static_assert(std::size(kAll) == 24,
                  "all 24 shipped .format.json files must be listed — a sample "
                  "is exactly how a multi-site miss stays green");
    for (auto const name : kAll) {
        auto r = ObjectFormatSchema::loadShipped(name);
        EXPECT_TRUE(r.has_value())
            << name << ".format.json must load clean under the closed "
                       "root-key vocabulary";
    }
}

// ═════════════════════════════════════════════════════════════════════
// D-LK10-ENTRY entry-gate fold — EVERY exec-flavored shipped format
// declares `processExit`. The whole population, not a sample: this is
// exactly the multi-site class where a green subset hides the miss, and
// the miss it hides is a SILENT one. MEASURED 2026-08-05, before the
// validate() rule landed: `macho64-x86_64-darwin-exec` declared no
// `processExit`, and `int main(void){return 42;}` built against it
// produced rc=0, a 4162-byte artifact and `LC_MAIN entryoff=0x1000`
// pointing at `main`'s own `48 81 ec 10 00 00 00` (`sub rsp,0x10`)
// prologue — the image entry was the user function, with ZERO
// diagnostics.
//
// The sweep is over the SAME 24-name list above (single source of truth
// for "what ships"), filtered by the schema's own `isExecFlavor()`
// predicate rather than by a hand-kept list of exec names — so a 25th
// format added without `processExit` reds HERE, and one added WITH it
// needs no edit to this test.
//
// ★ WHAT THIS DOES NOT PROVE: nothing about the ELF ET_DYN (PIE) arm of
// `isExecFlavor()`. That arm qualifies only via `elfDynPieShape`, which
// contains `processExit.has_value()`, so for the two `-pie` formats the
// assertion below is a TAUTOLOGY — it cannot fail, and it is not
// evidence. Their real guard is the all-or-none entry-cluster rule
// (`clusterCount`, object_format_schema.cpp) — ★ AT CONFIG LOAD, which is
// the only tier this sweep reaches, since every format here arrives via
// `loadShipped` and therefore through `validate()`. That qualifier is
// load-bearing: the same sentence WITHOUT it appears at the linker and
// walker tiers, where `clusterCount` does NOT run (an in-memory schema
// never passes through `validate()`), and there a hand-built 3-of-4
// ET_DYN is invisible to `isExecFlavor()` and `processExit()` alike. Its
// guard there is the ELF walker's own PARTIAL-PIE belt (`elf.cpp`,
// `encodeElfExecDynamic`), MEASURED by
// `EntryGateFold.ElfWalkerRefusesHandBuiltEtDynPartialEntryCluster`.
// Said out loud so the row count here is never read as coverage it is not.
// ═════════════════════════════════════════════════════════════════════
TEST(FormatRootKeyVocabulary, EveryShippedExecFlavorFormatDeclaresProcessExit) {
    constexpr std::string_view kAll[] = {
        "elf64-aarch64-linux-dyn",      "elf64-aarch64-linux-exec",
        "elf64-aarch64-linux-pie",      "elf64-aarch64-linux-staticlib",
        "elf64-aarch64-linux",          "elf64-x86_64-linux-dyn",
        "elf64-x86_64-linux-exec",      "elf64-x86_64-linux-pie",
        "elf64-x86_64-linux-staticlib", "elf64-x86_64-linux",
        "macho64-arm64-darwin-dylib",   "macho64-arm64-darwin-exec",
        "macho64-arm64-darwin-staticlib", "macho64-arm64-darwin",
        "macho64-x86_64-darwin-dylib",  "macho64-x86_64-darwin-exec",
        "macho64-x86_64-darwin-staticlib", "macho64-x86_64-darwin",
        "pe64-x86_64-windows-dll",      "pe64-x86_64-windows-exec",
        "pe64-x86_64-windows-staticlib", "pe64-x86_64-windows",
        "spirv-1.6",                    "wasm32-v1"};
    static_assert(std::size(kAll) == 24,
                  "all 24 shipped .format.json files must be listed — a sample "
                  "is exactly how a multi-site miss stays green");
    std::size_t execSeen = 0;
    for (auto const name : kAll) {
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value())
            << name << " must load — a format that cannot load cannot be "
                       "swept, which would silently shrink this population";
        auto const& fmt = **r;
        if (!fmt.isExecFlavor()) {
            // The negative half, and it is not filler: `processExit` on a
            // NON-exec format is the ⇐ half of the same biconditional and
            // is likewise rejected at load. Asserting both directions here
            // is what makes this a sweep of the RULE rather than of one
            // key's presence.
            EXPECT_FALSE(fmt.processExit().has_value())
                << name << " is not exec-flavored and must NOT declare "
                           "`processExit` (the ⇐ half of the ⟺ rule)";
            continue;
        }
        ++execSeen;
        EXPECT_TRUE(fmt.processExit().has_value())
            << name << " is EXEC-FLAVORED and must declare `processExit`. "
                       "DSS always synthesises an entry trampoline on an "
                       "exec-flavored format; without a declared mechanism "
                       "the linker used to SKIP injection silently and the "
                       "image entry became the module's first function.";
        // The inseparable partner — validate()'s §2.13 pairing rule. A
        // format passing the line above while failing this one would have
        // a mechanism the emitter cannot build a call for.
        EXPECT_FALSE(fmt.entryCallingConvention().empty())
            << name << " declares `processExit` and must therefore declare "
                       "`entryCallingConvention` (§2.13 pairing rule)";
    }
    // Guards the sweep itself: if `isExecFlavor()` were ever broken to
    // return false everywhere, every EXPECT above would vacuously pass and
    // this test would report success while checking nothing. 7 = the two
    // `-exec` ELFs + the two `-pie` ELFs + the two darwin `-exec` +
    // pe64-x86_64-windows-exec (COUNTED from the list above, 2026-08-05).
    EXPECT_EQ(execSeen, 7u)
        << "the exec-flavored population must be 7 of the 24 shipped "
           "formats — a different number means either a format was added/"
           "removed without updating this sweep, or `isExecFlavor()` "
           "changed meaning and every assertion above just went vacuous";
}

// ═════════════════════════════════════════════════════════════════════════════
// TF-C97 (D-PP-FORMAT-DATA-MODEL-PREDEFINES) — the FORMAT's own predefined
// macros: the C-visible face of `dataModel`.
//
// A format schema may now declare `predefinedMacros`, the same key name and the
// same shared entry grammar the language and target loaders use. The shipped
// population is DERIVED FROM `dataModel`: LP64 formats declare `__LP64__` and
// `_LP64`; LLP64 and ILP32 formats declare NEITHER, and that negative is the
// whole reason the channel lives at the format layer rather than the target's
// (one CPU — x86_64 — is LP64 under elf64/macho64 and LLP64 under pe64).
//
// ★ THE FAILURE MODE THESE TESTS EXIST FOR is not "the macro is wrong", it is
// "the key does nothing". A config key that is written but never READ presents
// as a feature that silently no-ops, which is why the first test below asserts
// the PARSED ROWS and not merely that the file loads.
// ═════════════════════════════════════════════════════════════════════════════

namespace {

// A row of the shipped predefine shape, for comparison against a loaded schema.
struct MacroRow {
    std::string_view name;
    std::string_view value;
};

// The rows a LOADED schema exposes, flattened for order-sensitive comparison.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
macroRowsOf(ObjectFormatSchema const& s) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto const& pm : s.predefinedMacros()) out.emplace_back(pm.name, pm.value);
    return out;
}

}  // namespace

// ★ THE "IS THE KEY ACTUALLY READ?" PIN, and the reason it is FIRST.
//
// The registry row that opened this anchor recorded the format loader as having
// NO closed root-key set, so a typo'd key would be silently ignored and the
// feature would present as "does nothing". MEASURED at TF-C97: that is no longer
// true — TF-C75 closed the format vocabulary (`kFormatDocumentKeys`) — but the
// deeper hazard survives the guard, because a key can be in the vocabulary and
// still be parsed by nobody. This test closes THAT: it asserts the loader
// produced ROWS, with their names and VALUES, from the JSON text.
//
// RED-ON-DISABLE: delete the `predefinedMacros` parse block in
// object_format_schema_json.cpp (leaving the name in `kFormatDocumentKeys`, so
// the file still loads clean) and this fails while every other test here passes
// — which is exactly the shape of the silent no-op being prevented.
TEST(FormatPredefinedMacros, KeyIsPARSEDNotMerelyAccepted) {
    auto r = ObjectFormatSchema::loadFromText(withRootKey(
        R"("predefinedMacros":[
             {"name":"__PROBE_A__","kind":"constant","value":"1"},
             {"name":"__PROBE_B__","kind":"constant","value":"7"}])"));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(macroRowsOf(**r),
              (std::vector<std::pair<std::string, std::string>>{
                  {"__PROBE_A__", "1"}, {"__PROBE_B__", "7"}}))
        << "the loader must PARSE the rows (names AND values, in declaration "
           "order) — merely accepting the key would make it a knob that lies";
}

// A schema that declares no `predefinedMacros` exposes an EMPTY span, never a
// synthesized default. Absence is the LLP64/ILP32 answer, so a loader that
// invented rows here would silently define `__LP64__` on Windows.
TEST(FormatPredefinedMacros, AbsentKeyIsEmptyNeverSynthesized) {
    auto r = ObjectFormatSchema::loadFromText(kElfMinimal);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE((*r)->predefinedMacros().empty())
        << "the loader must never synthesize a macro from `dataModel` — the "
           "spelling is a C-family fact and belongs in config, not in the "
           "object-format tier";
}

// The typo discriminator over THIS key. Without it, `"predefinedMacro"` (no
// trailing s) would load clean and the whole feature would no-op in silence —
// the precise trap the anchor row warned about.
TEST(FormatPredefinedMacros, MisspelledKeyRejectedAndNamed) {
    auto r = ObjectFormatSchema::loadFromText(withRootKey(
        R"("predefinedMacro":[{"name":"__X__","kind":"constant","value":"1"}])"));
    ASSERT_FALSE(r.has_value())
        << "a misspelled predefine key must be REJECTED — silently ignoring it "
           "presents as 'the feature does nothing', not as an error";
    EXPECT_TRUE(anyHasMalformedJson(r.error()));
    EXPECT_TRUE(anyMessageMentions(r.error(), "predefinedMacro"))
        << "the diagnostic must NAME the offending key";
}

// The entry grammar is INHERITED from `parsePredefinedMacroArray`, not
// re-implemented — so the language loader's rules apply here for free. Pinned on
// two of them (the Constant⇒`value` requirement and the within-array duplicate
// reject) because a copy-pasted parser is exactly what would drift.
TEST(FormatPredefinedMacros, SharedEntryGrammarIsInherited) {
    auto missingValue = ObjectFormatSchema::loadFromText(withRootKey(
        R"("predefinedMacros":[{"name":"__X__","kind":"constant"}])"));
    EXPECT_FALSE(missingValue.has_value())
        << "a 'constant' entry without `value` must be rejected, exactly as on "
           "the language and target sides";

    auto duplicate = ObjectFormatSchema::loadFromText(withRootKey(
        R"("predefinedMacros":[{"name":"__X__","kind":"constant","value":"1"},
                               {"name":"__X__","kind":"constant","value":"2"}])"));
    EXPECT_FALSE(duplicate.has_value())
        << "a duplicate name WITHIN one array must be rejected — the effective "
           "value would otherwise depend on which seed site iterated last";

    auto notAnArray = ObjectFormatSchema::loadFromText(
        withRootKey(R"("predefinedMacros":{"name":"__X__"})"));
    EXPECT_FALSE(notAnArray.has_value())
        << "'predefinedMacros' must be an array";
    EXPECT_TRUE(anyHasMalformedJson(notAnArray.error()));
}

// ★ THE SHIPPED-POPULATION PIN, BOTH DIRECTIONS, OVER ALL 24 FILES.
//
// The expectation is keyed on each schema's OWN `dataModel()` — never on its
// name — because that is the rule the config author applied and the only rule
// that stays true when a 25th format lands. A `if (name.starts_with("macho"))`
// here would pass today and encode the agnosticism break this cycle exists to
// avoid.
//
// BOTH directions matter and the NEGATIVE matters more: an LP64-only assertion
// would stay green if a maintainer pasted the rows into the pe64 files, which is
// the miscompile (Windows headers taking their LP64 arms against a 32-bit
// `long`) that motivated putting the channel on the format at all.
//
// RED-ON-DISABLE: delete the `predefinedMacros` key from any single LP64
// `.format.json` (its leg reds), or add it to any pe64/wasm/spirv file (that leg
// reds). MEASURED both ways during TF-C97.
TEST(FormatPredefinedMacros, ShippedPopulationFollowsDataModelNotFormatName) {
    constexpr std::string_view kAll[] = {
        "elf64-aarch64-linux-dyn",      "elf64-aarch64-linux-exec",
        "elf64-aarch64-linux-pie",      "elf64-aarch64-linux-staticlib",
        "elf64-aarch64-linux",          "elf64-x86_64-linux-dyn",
        "elf64-x86_64-linux-exec",      "elf64-x86_64-linux-pie",
        "elf64-x86_64-linux-staticlib", "elf64-x86_64-linux",
        "macho64-arm64-darwin-dylib",   "macho64-arm64-darwin-exec",
        "macho64-arm64-darwin-staticlib", "macho64-arm64-darwin",
        "macho64-x86_64-darwin-dylib",  "macho64-x86_64-darwin-exec",
        "macho64-x86_64-darwin-staticlib", "macho64-x86_64-darwin",
        "pe64-x86_64-windows-dll",      "pe64-x86_64-windows-exec",
        "pe64-x86_64-windows-staticlib", "pe64-x86_64-windows",
        "spirv-1.6",                    "wasm32-v1"};
    static_assert(std::size(kAll) == 24,
                  "all 24 shipped .format.json files must be listed — a sample "
                  "is exactly how a multi-site miss stays green");

    // The LP64 rows, as the shipped files spell them. Stated ONCE here so the
    // expectation is a single fact rather than 18 restatements.
    constexpr MacroRow kLp64Rows[] = {{"__LP64__", "1"}, {"_LP64", "1"}};

    std::size_t lp64Seen = 0, otherSeen = 0;
    for (auto const name : kAll) {
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value()) << name;
        auto const& s = **r;
        auto const rows = macroRowsOf(s);

        if (s.dataModel() == DataModel::Lp64) {
            ++lp64Seen;
            std::vector<std::pair<std::string, std::string>> want;
            for (auto const& m : kLp64Rows) {
                want.emplace_back(std::string{m.name}, std::string{m.value});
            }
            EXPECT_EQ(rows, want)
                << name << " declares dataModel LP64, so it must predefine "
                           "BOTH __LP64__ and _LP64 as 1";
            // UNGATED: the rows apply on every object format this FILE serves
            // (the file already IS the format). A stray
            // `availableObjectFormats` here would silently drop them.
            for (auto const& pm : s.predefinedMacros()) {
                EXPECT_TRUE(pm.availableObjectFormats.empty())
                    << name << ": a format's OWN rows must be ungated — the "
                               "file is already the gate";
                EXPECT_EQ(pm.kind, PredefinedMacroKind::Constant) << name;
                EXPECT_FALSE(pm.isFunctionLike) << name;
            }
        } else {
            ++otherSeen;
            EXPECT_TRUE(rows.empty())
                << name << " declares dataModel "
                << dataModelName(s.dataModel())
                << ", so it must predefine NEITHER __LP64__ nor _LP64 — the "
                   "negative is the whole reason this channel lives on the "
                   "format instead of the target";
        }
    }
    // MEASURED at TF-C97: 18 LP64 (10 elf + 8 macho), 6 not (4 pe LLP64 + 2
    // ILP32 skeletons). Pinned so a file added or dropped without a decision
    // shows up here rather than passing vacuously.
    EXPECT_EQ(lp64Seen, 18u);
    EXPECT_EQ(otherSeen, 6u);
}

// ── D-PP-HEADER-CASE-INSENSITIVE-PE: `headerNameMatching` ────────────────────
//
// The header-NAME case rule is a REQUIRED, closed-enum root key. Required
// rather than optional-with-default is the load-bearing part: an optional key
// would let a future pe/macho format file silently regress to case-sensitive
// matching and reject `<Windows.h>` again with nothing in the tree to say why.
// A new format file that omits it must fail at LOAD, and these pin that.

namespace {
// The minimal ELF document these pins vary ONE key of, so every rejection is
// attributable to that key and not to a missing sibling.
[[nodiscard]] std::string headerCaseFormatJson(std::string_view matchingLine) {
    return std::string{R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "LP64",
      )"} + std::string{matchingLine} + R"(
      "format": { "name": "hnm-stub", "version": "1.0", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 }
    })";
}
} // namespace

TEST(ObjectFormatSchemaLoader, HeaderNameMatchingIsRequired) {
    // The CONTROL: identical document WITH the key loads clean, so the
    // rejection below is attributable to the omission alone.
    auto ok = ObjectFormatSchema::loadFromText(
        headerCaseFormatJson(R"("headerNameMatching": "case-sensitive",)"));
    ASSERT_TRUE(ok.has_value());

    auto missing = ObjectFormatSchema::loadFromText(headerCaseFormatJson(""));
    ASSERT_FALSE(missing.has_value())
        << "a format file that omits headerNameMatching must fail at LOAD — a "
           "silent default is how a new pe/macho file would regress to "
           "case-sensitive matching unnoticed";
    bool sawMissingField = false;
    for (auto const& d : missing.error()) {
        if (d.code == DiagnosticCode::C_MissingField
            && d.path == "/headerNameMatching") sawMissingField = true;
    }
    EXPECT_TRUE(sawMissingField)
        << "the rejection must NAME the missing key, not just fail";
}

TEST(ObjectFormatSchemaLoader, HeaderNameMatchingIsAClosedVocabulary) {
    // Both legal spellings parse and are EXPOSED (parsed, not merely accepted).
    auto cs = ObjectFormatSchema::loadFromText(
        headerCaseFormatJson(R"("headerNameMatching": "case-sensitive",)"));
    ASSERT_TRUE(cs.has_value());
    EXPECT_EQ((*cs)->headerNameMatching(), HeaderNameMatching::CaseSensitive);

    auto ci = ObjectFormatSchema::loadFromText(
        headerCaseFormatJson(R"("headerNameMatching": "case-insensitive",)"));
    ASSERT_TRUE(ci.has_value());
    EXPECT_EQ((*ci)->headerNameMatching(), HeaderNameMatching::CaseInsensitive);

    // A typo'd VALUE fails LOUD — never a silent degrade to one of the two.
    for (char const* bad : {R"("headerNameMatching": "caseinsensitive",)",
                            R"("headerNameMatching": "insensitive",)",
                            R"("headerNameMatching": "Case-Insensitive",)",
                            R"("headerNameMatching": true,)"}) {
        EXPECT_FALSE(ObjectFormatSchema::loadFromText(headerCaseFormatJson(bad))
                         .has_value())
            << "unknown headerNameMatching value must fail loud: " << bad;
    }
    // A typo'd KEY is caught by the closed root-key vocabulary, not ignored.
    EXPECT_FALSE(
        ObjectFormatSchema::loadFromText(
            headerCaseFormatJson(R"("headerNameMatchng": "case-sensitive",)"))
            .has_value());
}

// Every shipped format declares the key, and its value matches the platform's
// real filesystem convention. This is a CONFIG pin, deliberately expressed as a
// table here rather than as engine logic — the engine must never derive the
// rule from the format kind (that would be the identity branch the agnosticism
// bar forbids), so the only place the kind↔rule correspondence may be written
// down is a test that reads the shipped files.
TEST(ObjectFormatSchemaLoader, EveryShippedFormatDeclaresHeaderNameMatching) {
    constexpr std::string_view kAll[] = {
        "elf64-aarch64-linux-dyn",      "elf64-aarch64-linux-exec",
        "elf64-aarch64-linux-pie",      "elf64-aarch64-linux-staticlib",
        "elf64-aarch64-linux",          "elf64-x86_64-linux-dyn",
        "elf64-x86_64-linux-exec",      "elf64-x86_64-linux-pie",
        "elf64-x86_64-linux-staticlib", "elf64-x86_64-linux",
        "macho64-arm64-darwin-dylib",   "macho64-arm64-darwin-exec",
        "macho64-arm64-darwin-staticlib", "macho64-arm64-darwin",
        "macho64-x86_64-darwin-dylib",  "macho64-x86_64-darwin-exec",
        "macho64-x86_64-darwin-staticlib", "macho64-x86_64-darwin",
        "pe64-x86_64-windows-dll",      "pe64-x86_64-windows-exec",
        "pe64-x86_64-windows-staticlib", "pe64-x86_64-windows",
        "spirv-1.6",                    "wasm32-v1"};
    static_assert(std::size(kAll) == 24,
                  "all 24 shipped .format.json files must be listed");

    std::size_t insensitive = 0, sensitive = 0;
    for (auto const name : kAll) {
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value()) << name;
        auto const m = (*r)->headerNameMatching();
        ASSERT_NE(m, HeaderNameMatching::Invalid) << name;
        switch ((*r)->kind()) {
            case ObjectFormatKind::Pe:
            case ObjectFormatKind::MachO:
                EXPECT_EQ(m, HeaderNameMatching::CaseInsensitive)
                    << name << ": Windows (NTFS) and default macOS (APFS/HFS+) "
                               "fold header names, and their SDKs rely on it";
                ++insensitive;
                break;
            default:
                EXPECT_EQ(m, HeaderNameMatching::CaseSensitive)
                    << name << ": POSIX matches byte-exact; the declared-only "
                               "skeletons take the conservative value";
                ++sensitive;
                break;
        }
    }
    // 4 pe + 8 macho fold; 10 elf + spirv + wasm32 do not. Pinned so a file
    // added or dropped without a decision shows up here rather than vacuously.
    EXPECT_EQ(insensitive, 12u);
    EXPECT_EQ(sensitive, 12u);
}
