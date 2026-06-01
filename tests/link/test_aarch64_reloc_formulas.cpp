// ARM64 relocation-formula apply tests (D-LK6-1 closure — 2026-06-01).
//
// Pins byte-output of the three non-Linear ARM64 reloc-formula
// variants applied through `applyExecRelocations`:
//   * Aarch64Call26       — BL/B target imm26
//   * Aarch64AdrPrelPgHi21 — ADRP page-pair shift
//   * Aarch64AddAbsLo12    — ADD imm12 low-page
//
// Each test sets up a minimal `AssembledModule` + a synthetic
// `TargetSchema` declaring exactly one relocation kind, runs the
// kernel, and asserts the resulting 4-byte instruction word.
//
// The Linear-arm of the dispatch is regression-covered by the
// existing `test_elf_exec_writer.cpp` x86_64 tests; this file adds
// ARM64-specific coverage that the previous "structured triple"
// shape could not express.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/exec_reloc_apply.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dss;
using namespace dss::link::format;

namespace {

// Synthesize a TargetSchema with exactly one relocation row of the
// named formula kind. Returns shared_ptr<TargetSchema> on success.
std::shared_ptr<TargetSchema const>
loadOneRelocTarget(std::string_view formula, std::uint32_t kind = 1) {
    std::string const json = std::string{R"({
      "dssTargetVersion": 1,
      "target": {"name":"aarch64_test"},
      "relocations":[
        { "name": "test_kind", "kind": )"} + std::to_string(kind)
        + R"(, "formula": ")" + std::string{formula} + R"(" }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })";
    auto r = TargetSchema::loadFromText(json);
    if (!r.has_value()) {
        std::string msg;
        for (auto const& d : r.error()) msg += d.message + "\n";
        ADD_FAILURE() << "target load failed: " << msg;
        return nullptr;
    }
    return *r;
}

struct Patched {
    std::vector<std::uint8_t> text;
    bool                      ok = false;
};

// Run the kernel with a single function, single reloc on the
// in-text instruction word.
Patched applyOneReloc(std::shared_ptr<TargetSchema const> tgt,
                      std::uint32_t baseInstWord,
                      std::uint64_t symbolVa,
                      std::int64_t  addend,
                      std::uint64_t patchSectionVa,
                      std::uint64_t funcOffset) {
    Patched out;
    out.text.resize(funcOffset + 4);
    // assembler emitted base inst at funcOffset (LE)
    out.text[funcOffset + 0] = static_cast<std::uint8_t>(baseInstWord       & 0xFF);
    out.text[funcOffset + 1] = static_cast<std::uint8_t>((baseInstWord >> 8) & 0xFF);
    out.text[funcOffset + 2] = static_cast<std::uint8_t>((baseInstWord >> 16) & 0xFF);
    out.text[funcOffset + 3] = static_cast<std::uint8_t>((baseInstWord >> 24) & 0xFF);

    AssembledModule mod;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes.resize(funcOffset + 4);  // function's own byte slot
    Relocation rel;
    rel.offset = static_cast<std::uint32_t>(funcOffset);
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};
    rel.addend = addend;
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));

    std::vector<std::uint64_t> funcTextStart{0};
    std::unordered_map<SymbolId, std::uint64_t> symbolVaMap{{SymbolId{2}, symbolVa}};

    DiagnosticReporter rep;
    out.ok = applyExecRelocations(
        out.text, mod, funcTextStart, symbolVaMap,
        *tgt, patchSectionVa, "test", rep);
    return out;
}

std::uint32_t readInst(std::vector<std::uint8_t> const& text, std::size_t off) {
    return  static_cast<std::uint32_t>(text[off + 0])
         | (static_cast<std::uint32_t>(text[off + 1]) << 8)
         | (static_cast<std::uint32_t>(text[off + 2]) << 16)
         | (static_cast<std::uint32_t>(text[off + 3]) << 24);
}

} // namespace

// ── parseRelocFormulaKind round-trip ─────────────────────────

TEST(RelocFormulaKind, NameRoundTrip) {
    EXPECT_EQ(relocFormulaName(RelocFormulaKind::Linear),               "linear");
    EXPECT_EQ(relocFormulaName(RelocFormulaKind::Aarch64Call26),        "aarch64_call26");
    EXPECT_EQ(relocFormulaName(RelocFormulaKind::Aarch64AdrPrelPgHi21), "aarch64_adr_prel_pg_hi21");
    EXPECT_EQ(relocFormulaName(RelocFormulaKind::Aarch64AddAbsLo12),    "aarch64_add_abs_lo12");

    EXPECT_EQ(parseRelocFormulaKind("linear"),                   RelocFormulaKind::Linear);
    EXPECT_EQ(parseRelocFormulaKind("aarch64_call26"),           RelocFormulaKind::Aarch64Call26);
    EXPECT_EQ(parseRelocFormulaKind("aarch64_adr_prel_pg_hi21"), RelocFormulaKind::Aarch64AdrPrelPgHi21);
    EXPECT_EQ(parseRelocFormulaKind("aarch64_add_abs_lo12"),     RelocFormulaKind::Aarch64AddAbsLo12);
    EXPECT_EQ(parseRelocFormulaKind("nonsense"),                 std::nullopt);
    EXPECT_EQ(parseRelocFormulaKind(""),                         std::nullopt);
}

// ── JSON loader rejects bad formula discriminator ─────────────

TEST(TargetSchemaLoader, RejectsInvalidFormulaDiscriminator) {
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"bad"},
      "relocations":[
        { "name": "x", "kind": 1, "formula": "S + A - P >> 2" }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_FALSE(r.has_value());
}

// non-Linear with explicit pcRelative=true → reject (variant
// encodes PC-relativity intrinsically).
TEST(TargetSchemaLoader, RejectsNonLinearWithPcRelative) {
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"bad"},
      "relocations":[
        { "name": "x", "kind": 1, "formula": "aarch64_call26", "pcRelative": true }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(TargetSchemaLoader, RejectsNonLinearWithAddendBias) {
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"bad"},
      "relocations":[
        { "name": "x", "kind": 1, "formula": "aarch64_call26", "addendBias": -4 }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(TargetSchemaLoader, RejectsNonLinearWithWidthBytes8) {
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"bad"},
      "relocations":[
        { "name": "x", "kind": 1, "formula": "aarch64_call26", "widthBytes": 8 }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(TargetSchemaLoader, NonLinearDefaultsWidthBytesTo4) {
    auto tgt = loadOneRelocTarget("aarch64_call26");
    ASSERT_NE(tgt, nullptr);
    auto const* tri = tgt->relocationInfo(RelocationKind{1});
    ASSERT_NE(tri, nullptr);
    EXPECT_EQ(tri->formulaKind, RelocFormulaKind::Aarch64Call26);
    EXPECT_EQ(tri->widthBytes, 4);
}

// ── Aarch64Call26 ───────────────────────────────────────────

TEST(Aarch64Call26, PatchesForwardBranchBits25To0) {
    auto tgt = loadOneRelocTarget("aarch64_call26");
    ASSERT_NE(tgt, nullptr);

    // BL opcode = 0x94000000 (bits 31:26 = 100101, bits 25:0 = 0).
    // patchSectionVa=0x400000, funcOffset=0, symbol VA = 0x400010 →
    // delta = +0x10 → shifted = 4 → OR into inst[25:0] = 0x94000004.
    auto p = applyOneReloc(tgt, 0x94000000u,
                            /*symbolVa*/ 0x400010,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0x400000,
                            /*funcOffset*/ 0);
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(readInst(p.text, 0), 0x94000004u);
}

TEST(Aarch64Call26, PatchesNegativeBranchTwosComplement) {
    auto tgt = loadOneRelocTarget("aarch64_call26");
    ASSERT_NE(tgt, nullptr);

    // Target -16 bytes back; delta = -16 → shifted = -4 → two's-
    // complement 26-bit = 0x03FFFFFC → OR into BL.
    auto p = applyOneReloc(tgt, 0x94000000u,
                            /*symbolVa*/ 0x400000,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0x400000,
                            /*funcOffset*/ 0x10);
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(readInst(p.text, 0x10), 0x94000000u | 0x03FFFFFCu);
}

TEST(Aarch64Call26, RejectsUnalignedTarget) {
    auto tgt = loadOneRelocTarget("aarch64_call26");
    ASSERT_NE(tgt, nullptr);

    // delta = 5 → not 4-aligned → reject.
    auto p = applyOneReloc(tgt, 0x94000000u,
                            /*symbolVa*/ 0x400005,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0x400000,
                            /*funcOffset*/ 0);
    EXPECT_FALSE(p.ok);
}

TEST(Aarch64Call26, RejectsOutOfRange) {
    auto tgt = loadOneRelocTarget("aarch64_call26");
    ASSERT_NE(tgt, nullptr);

    // delta = 0x10000000 (256 MiB) → shifted = 0x04000000 — exceeds
    // signed-26-bit max (0x01FFFFFF). Reject.
    auto p = applyOneReloc(tgt, 0x94000000u,
                            /*symbolVa*/ 0x10000000,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    EXPECT_FALSE(p.ok);
}

TEST(Aarch64Call26, RejectsBaseInstWithDirtyBitfield) {
    auto tgt = loadOneRelocTarget("aarch64_call26");
    ASSERT_NE(tgt, nullptr);

    // BL opcode + a bogus 1-bit in the immediate field — must reject
    // (the linker OR's; pre-set bits would silently corrupt the patch).
    auto p = applyOneReloc(tgt, 0x94000001u,
                            /*symbolVa*/ 0x10,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    EXPECT_FALSE(p.ok);
}

// ── Aarch64AdrPrelPgHi21 ─────────────────────────────────────

TEST(Aarch64AdrPrelPgHi21, EncodesImmloImmhiSplit) {
    auto tgt = loadOneRelocTarget("aarch64_adr_prel_pg_hi21");
    ASSERT_NE(tgt, nullptr);

    // ADRP x0 = 0x90000000 (bits 31:29 = 100, bit 28 = 1, register = 0).
    // Page-pair value of 5 (0b101) → immlo = 01 (bits 30:29),
    // immhi = 0b001 << 5 (bits 23:5 = 0b000...001).
    // patchSectionVa=0, funcOffset=0; (S+A)>>12=5, (P)>>12=0 → value=5.
    auto p = applyOneReloc(tgt, 0x90000000u,
                            /*symbolVa*/ 0x5000,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    ASSERT_TRUE(p.ok);
    // value = 0b101 (5). bits[1:0]=01 → immlo at [30:29]; bits[20:2]=0b001 → immhi at [5..23].
    std::uint32_t const immlo = (5u & 0x3u) << 29;
    std::uint32_t const immhi = ((5u >> 2) & 0x7FFFFu) << 5;
    EXPECT_EQ(readInst(p.text, 0), 0x90000000u | immlo | immhi);
}

TEST(Aarch64AdrPrelPgHi21, RejectsOutOfRange) {
    auto tgt = loadOneRelocTarget("aarch64_adr_prel_pg_hi21");
    ASSERT_NE(tgt, nullptr);

    // value beyond signed-21-bit → reject.
    // (S>>12) = 2^21 = 0x200000; (P>>12) = 0 → value = 0x200000, out of range.
    auto p = applyOneReloc(tgt, 0x90000000u,
                            /*symbolVa*/ static_cast<std::uint64_t>(0x200000) << 12,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    EXPECT_FALSE(p.ok);
}

// ── Aarch64AddAbsLo12 ───────────────────────────────────────

TEST(Aarch64AddAbsLo12, EncodesImm12AtBits21To10) {
    auto tgt = loadOneRelocTarget("aarch64_add_abs_lo12");
    ASSERT_NE(tgt, nullptr);

    // ADD x0, x0, #imm12 = 0x91000000 base.
    // symbolVa = 0x12345678; low 12 = 0x678 → OR into bits[21:10] =
    // 0x678 << 10 = 0x19E000.
    auto p = applyOneReloc(tgt, 0x91000000u,
                            /*symbolVa*/ 0x12345678,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(readInst(p.text, 0), 0x91000000u | (0x678u << 10));
}

TEST(Aarch64AddAbsLo12, IgnoresHighBits) {
    auto tgt = loadOneRelocTarget("aarch64_add_abs_lo12");
    ASSERT_NE(tgt, nullptr);

    // Large symbol VA — only low 12 bits matter (the formula masks
    // unsigned by construction; no range check).
    auto p = applyOneReloc(tgt, 0x91000000u,
                            /*symbolVa*/ 0xFFFFFFFF12345ABC,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(readInst(p.text, 0), 0x91000000u | (0xABCu << 10));
}

// ── Shipped arm64.target.json loads cleanly ──────────────────

TEST(ShippedArm64Target, LoadsAllFourRelocKinds) {
    auto r = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(r.has_value());
    auto const& tgt = **r;

    auto const* call26 = tgt.relocationByName("call26");
    ASSERT_NE(call26, nullptr);
    EXPECT_EQ(call26->formulaKind, RelocFormulaKind::Aarch64Call26);
    EXPECT_EQ(call26->widthBytes, 4);

    auto const* adrp = tgt.relocationByName("adr_prel_pg_hi21");
    ASSERT_NE(adrp, nullptr);
    EXPECT_EQ(adrp->formulaKind, RelocFormulaKind::Aarch64AdrPrelPgHi21);

    auto const* add = tgt.relocationByName("add_abs_lo12_nc");
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->formulaKind, RelocFormulaKind::Aarch64AddAbsLo12);

    auto const* abs64 = tgt.relocationByName("abs64");
    ASSERT_NE(abs64, nullptr);
    EXPECT_EQ(abs64->formulaKind, RelocFormulaKind::Linear);
    EXPECT_EQ(abs64->widthBytes, 8);
}
