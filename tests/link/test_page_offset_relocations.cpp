// ★★★ ONE PAGE OFFSET, SCALED BY THE ACCESS THAT USES IT
// (D-LK-MACHO-ARM64-PAGEOFF12-LOAD-PATCHED-AS-AN-ADD, P68 round 9).
//
// A reference compiler reaches an arm64 global as `adrp` + an ADD or a LOAD /
// STORE whose 12-bit offset the linker fills. An ADD's offset is bytes; a load's
// or store's counts ACCESS-SIZED units. ELF says which by the relocation TYPE
// (R_AARCH64_ADD_ABS_LO12_NC, R_AARCH64_LDST8..128_ABS_LO12_NC); Mach-O writes
// ARM64_RELOC_PAGEOFF12 for all of them and ld64 takes the scale from the
// instruction. ✔MEASURED 2026-09-23 at the round-9 base: the Mach-O reader read
// every PAGEOFF12 as the ADD's kind, and a clang object's `ldr w8, [x8,
// _g32@PAGEOFF]` linked as `ldr w8, [x8, #0x60]` for a g32 at page offset 0x18
// — every load wrong but the byte one; and the ELF reader refused a gcc object's
// LDST types outright.
//
// Pinned here: the two real objects read with the kind each load needs, the
// shared decode table word by word, and the loader's refusals of a family that
// could decode one word two ways. The arithmetic is pinned beside the other
// aarch64 formulas (`test_aarch64_reloc_formulas.cpp`), and the images RUN in
// `examples/c/foreign_page_offset_loads_arm64` (macOS and qemu).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include "page_offset_objects.inc"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

struct Shipped {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Shipped loadShipped(std::string const& format) {
    Shipped s;
    if (auto t = TargetSchema::loadShipped("arm64")) s.target = *t;
    if (auto f = ObjectFormatSchema::loadShipped(format)) s.format = *f;
    return s;
}

[[nodiscard]] std::string describe(DiagnosticReporter const& rep) {
    std::string out = "errors=" + std::to_string(rep.errorCount());
    for (auto const& d : rep.all()) out += "\n  " + d.actual;
    return out;
}

[[nodiscard]] AssembledFunction const* functionNamed(AssembledModule const& m,
                                                     std::string const& name) {
    for (auto const& s : m.symbols) {
        if (s.name != name) continue;
        for (auto const& f : m.functions) {
            if (f.symbol == s.symbol) return &f;
        }
    }
    return nullptr;
}

[[nodiscard]] std::uint32_t wordAt(std::vector<std::uint8_t> const& bytes, std::size_t off) {
    return static_cast<std::uint32_t>(bytes.at(off))
         | (static_cast<std::uint32_t>(bytes.at(off + 1)) << 8)
         | (static_cast<std::uint32_t>(bytes.at(off + 2)) << 16)
         | (static_cast<std::uint32_t>(bytes.at(off + 3)) << 24);
}

[[nodiscard]] std::array<std::uint8_t, 4> le(std::uint32_t w) {
    return {static_cast<std::uint8_t>(w), static_cast<std::uint8_t>(w >> 8),
            static_cast<std::uint8_t>(w >> 16), static_cast<std::uint8_t>(w >> 24)};
}

// The target row's name for a kind — how a pin says which arithmetic a
// relocation will get without depending on the kind's number.
[[nodiscard]] std::string kindName(TargetSchema const& t, RelocationKind k) {
    auto const* r = t.relocationInfo(k);
    return r != nullptr ? r->name : "<kind " + std::to_string(k.v) + ">";
}

// Every relocation of `fn` that is not the ADRP half, by the name of its kind.
[[nodiscard]] std::map<std::string, int> pageOffsetKinds(TargetSchema const& t,
                                                         AssembledFunction const& fn) {
    std::map<std::string, int> out;
    for (auto const& r : fn.relocations) {
        auto const name = kindName(t, r.kind);
        if (name == "adr_prel_pg_hi21") continue;
        ++out[name];
    }
    return out;
}

constexpr std::uint32_t kPageOff12 = 0x44000000u;  // ARM64_RELOC_PAGEOFF12, length 2

}  // namespace

// ── the two real objects ──────────────────────────────────────────────────

TEST(PageOffsetRelocations, TheMachOReaderTakesTheScaleFromTheInstruction) {
    auto const sh = loadShipped("macho64-arm64-darwin");
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto const m = macho::readRelocatableObject(dss::test::clangMachOPageOffsetLoadsObject(),
                                                *sh.target, *sh.format, rep);
    ASSERT_TRUE(m.has_value()) << describe(rep);
    auto const* get = functionNamed(*m, "_get");
    ASSERT_NE(get, nullptr);
    // One wire type, five sizes: `ldr q0`, `ldrsb`, `ldrsh`, `ldr w` twice, `ldr d`.
    std::map<std::string, int> const want{
        {"ldst128_abs_lo12_nc", 1}, {"ldst8_abs_lo12_nc", 1}, {"ldst16_abs_lo12_nc", 1},
        {"ldst32_abs_lo12_nc", 2},  {"ldst64_abs_lo12_nc", 1}};
    EXPECT_EQ(pageOffsetKinds(*sh.target, *get), want)
        << "a load's page offset read as the ADD's is the base-round miscompile";
}

TEST(PageOffsetRelocations, TheElfReaderReadsEveryScaledType) {
    auto const sh = loadShipped("elf64-aarch64-linux");
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto const m = elf::readRelocatableObject(dss::test::gccElfPageOffsetLoadsObject(),
                                              *sh.target, *sh.format, rep);
    ASSERT_TRUE(m.has_value()) << "at the round-9 base: 'relocation type 299 ... is not "
                                  "declared'; " << describe(rep);
    auto const* get = functionNamed(*m, "get");
    ASSERT_NE(get, nullptr);
    std::map<std::string, int> const want{
        {"ldst128_abs_lo12_nc", 1}, {"ldst64_abs_lo12_nc", 2}, {"ldst8_abs_lo12_nc", 1},
        {"ldst16_abs_lo12_nc", 1},  {"ldst32_abs_lo12_nc", 1}};
    EXPECT_EQ(pageOffsetKinds(*sh.target, *get), want);
}

// ── the shared wire type, word by word ────────────────────────────────────

TEST(PageOffsetRelocations, EveryLoadAndStoreSizeDecodesToItsOwnScale) {
    auto const sh = loadShipped("macho64-arm64-darwin");
    ASSERT_TRUE(sh.target && sh.format);
    auto const table = sh.format->relocationDecodeTable();
    ASSERT_TRUE(table.has_value()) << table.error();
    // ✔ Each word is a reference assembler's (gas 2.42 = clang 18, 2026-09-23).
    struct Case { std::uint32_t word; char const* what; char const* kind; };
    std::array<Case, 12> const cases{{
        {0x91000000u, "add x0, x0, #lo12",  "add_abs_lo12_nc"},
        {0x39c00108u, "ldrsb w8, [x8]",     "ldst8_abs_lo12_nc"},
        {0x3d400000u, "ldr b0, [x0]",       "ldst8_abs_lo12_nc"},
        {0x79c00129u, "ldrsh w9, [x9]",     "ldst16_abs_lo12_nc"},
        {0x7d400000u, "ldr h0, [x0]",       "ldst16_abs_lo12_nc"},
        {0xb9400001u, "ldr w1, [x0]",       "ldst32_abs_lo12_nc"},
        {0xbd400000u, "ldr s0, [x0]",       "ldst32_abs_lo12_nc"},
        {0xb9000001u, "str w1, [x0]",       "ldst32_abs_lo12_nc"},
        {0xf9400001u, "ldr x1, [x0]",       "ldst64_abs_lo12_nc"},
        {0xf9000001u, "str x1, [x0]",       "ldst64_abs_lo12_nc"},
        {0xfd400000u, "ldr d0, [x0]",       "ldst64_abs_lo12_nc"},
        {0x3dc00000u, "ldr q0, [x0]",       "ldst128_abs_lo12_nc"},
    }};
    for (auto const& c : cases) {
        auto const bytes = le(c.word);
        auto const k = table->decode(kPageOff12, bytes);
        ASSERT_TRUE(k.has_value()) << c.what;
        EXPECT_EQ(kindName(*sh.target, *k), c.kind) << c.what;
    }
}

TEST(PageOffsetRelocations, AWordNoRowDecodesIsRefusedByReason) {
    auto const sh = loadShipped("macho64-arm64-darwin");
    ASSERT_TRUE(sh.target && sh.format);
    auto const table = sh.format->relocationDecodeTable();
    ASSERT_TRUE(table.has_value()) << table.error();
    using Miss = RelocationDecodeTable::Miss;
    auto const ret = le(0xd65f03c0u);
    auto const k1 = table->decode(kPageOff12, ret);
    ASSERT_FALSE(k1.has_value());
    EXPECT_EQ(k1.error(), Miss::NoInstruction) << "a `ret` has no page-offset field";
    std::array<std::uint8_t, 2> const shortSite{0x00, 0x00};
    auto const k2 = table->decode(kPageOff12, shortSite);
    ASSERT_FALSE(k2.has_value());
    EXPECT_EQ(k2.error(), Miss::SiteTooShort);
    auto const k3 = table->decode(0x7u, ret);
    ASSERT_FALSE(k3.has_value());
    EXPECT_EQ(k3.error(), Miss::Undeclared);
}

// ── the loader refuses a family that could decode one word two ways ──────

namespace {

[[nodiscard]] nlohmann::json shippedMachODoc() {
    std::ifstream f{dss::test::configRoot() / "object-formats" / "macho64-arm64-darwin.format.json",
                    std::ios::binary};
    return nlohmann::json::parse(f);
}

[[nodiscard]] nlohmann::json& rowOfKind(nlohmann::json& doc, int kind) {
    for (auto& r : doc.at("relocations")) {
        if (r.value("kind", 0) == kind) return r;
    }
    throw std::runtime_error{"no relocation row of kind " + std::to_string(kind)};
}

// The loader's messages for `doc`, joined; empty when it loads.
[[nodiscard]] std::string loadErrors(nlohmann::json const& doc) {
    auto const r = ObjectFormatSchema::loadFromText(doc.dump());
    if (r.has_value()) return {};
    std::string out;
    for (auto const& d : r.error()) out += d.message + "\n";
    return out.empty() ? std::string{"<refused with no message>"} : out;
}

}  // namespace

TEST(PageOffsetRelocations, TheShippedFamilyLoads) {
    EXPECT_EQ(loadErrors(shippedMachODoc()), "");
}

TEST(PageOffsetRelocations, ARowWithoutAPatternBesidePatternedOnesIsRefused) {
    auto doc = shippedMachODoc();
    rowOfKind(doc, 3).erase("decodeWhenInstruction");
    auto const e = loadErrors(doc);
    EXPECT_NE(e.find("declares no pattern"), std::string::npos) << e;
}

TEST(PageOffsetRelocations, TwoRowsThatCanDecodeOneWordAreRefused) {
    auto doc = shippedMachODoc();
    // The whole load/store class for the 16-bit row: it now also matches every
    // 32-bit word.
    rowOfKind(doc, 13)["decodeWhenInstruction"] =
        nlohmann::json::array({{{"mask", 0x3B000000u}, {"value", 0x39000000u}}});
    auto const e = loadErrors(doc);
    EXPECT_NE(e.find("both decode the word"), std::string::npos) << e;
}

TEST(PageOffsetRelocations, APatternThatTestsNothingIsRefused) {
    {
        auto doc = shippedMachODoc();
        rowOfKind(doc, 14)["decodeWhenInstruction"] =
            nlohmann::json::array({{{"mask", 0u}, {"value", 0u}}});
        auto const e = loadErrors(doc);
        EXPECT_NE(e.find("non-zero mask"), std::string::npos) << e;
    }
    {
        auto doc = shippedMachODoc();
        rowOfKind(doc, 14)["decodeWhenInstruction"] =
            nlohmann::json::array({{{"mask", 0xFB000000u}, {"value", 0xB9000001u}}});
        auto const e = loadErrors(doc);
        EXPECT_NE(e.find("value inside it"), std::string::npos) << e;
    }
}

TEST(PageOffsetRelocations, APatternEntryRefusesAnUnknownKeyAndKeepsItsProse) {
    auto doc = shippedMachODoc();
    rowOfKind(doc, 14)["decodeWhenInstruction"][0]["zzProbeKey"] = 1;
    auto const e = loadErrors(doc);
    EXPECT_NE(e.find("unknown key 'zzProbeKey'"), std::string::npos) << e;
    // CONTROL: a `$` documentation key is prose, as everywhere in the config.
    auto prose = shippedMachODoc();
    rowOfKind(prose, 14)["decodeWhenInstruction"][0]["$zzComment"] = "a note";
    EXPECT_EQ(loadErrors(prose), "");
}

TEST(PageOffsetRelocations, APatternedRowCarriesNoOtherDecodeDeclaration) {
    auto doc = shippedMachODoc();
    rowOfKind(doc, 15)["isCall"] = true;
    auto const e = loadErrors(doc);
    EXPECT_NE(e.find("decoded by instruction"), std::string::npos) << e;
}
