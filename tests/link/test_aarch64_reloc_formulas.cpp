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

TEST(Aarch64AddAbsLo12, IgnoresHighBitsOfPositiveSplusA) {
    auto tgt = loadOneRelocTarget("aarch64_add_abs_lo12");
    ASSERT_NE(tgt, nullptr);

    // Positive symbol VA within UINT32_MAX — only low 12 bits matter
    // (high bits ignored via the `& 0xFFFu` mask). post-fold #2: SA
    // > UINT32_MAX now rejects (silent-failure CRITICAL-1), so the
    // ignore-high-bits contract is pinned within the valid range.
    auto p = applyOneReloc(tgt, 0x91000000u,
                            /*symbolVa*/ 0xDEAD0ABCu,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(readInst(p.text, 0), 0x91000000u | (0xABCu << 10));
}

// ── Post-fold #1: additional coverage ────────────────────────

// pr-test-analyzer Rating 8: Call26 boundary tests
TEST(Aarch64Call26, PatchesMaxPositiveInRange) {
    auto tgt = loadOneRelocTarget("aarch64_call26");
    ASSERT_NE(tgt, nullptr);
    // signed-26-bit max = 0x01FFFFFF → delta = 0x01FFFFFF << 2 = 0x07FFFFFC.
    auto p = applyOneReloc(tgt, 0x94000000u,
                            /*symbolVa*/ 0x07FFFFFC,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(readInst(p.text, 0), 0x94000000u | 0x01FFFFFFu);
}

TEST(Aarch64Call26, PatchesMinNegativeInRange) {
    auto tgt = loadOneRelocTarget("aarch64_call26");
    ASSERT_NE(tgt, nullptr);
    // signed-26-bit min = -0x02000000 → delta = -0x02000000 << 2 = -0x08000000.
    // patchSectionVa=0x10000000, funcOffset=0, target=0x08000000 →
    // delta = -0x08000000 → shifted = -0x02000000 → encoded as 0x02000000 in 26-bit.
    auto p = applyOneReloc(tgt, 0x94000000u,
                            /*symbolVa*/ 0x08000000,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0x10000000,
                            /*funcOffset*/ 0);
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(readInst(p.text, 0), 0x94000000u | 0x02000000u);
}

// pr-test-analyzer Rating 8: AdrPrelPgHi21 boundary tests
TEST(Aarch64AdrPrelPgHi21, MaxPositiveInRange) {
    auto tgt = loadOneRelocTarget("aarch64_adr_prel_pg_hi21");
    ASSERT_NE(tgt, nullptr);
    // signed-21-bit max = 0xFFFFF; symbolVa = 0xFFFFF << 12 + 0 (low) = 0xFFFFF000.
    auto p = applyOneReloc(tgt, 0x90000000u,
                            /*symbolVa*/ 0xFFFFF000u,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    ASSERT_TRUE(p.ok);
    std::uint32_t const v = 0xFFFFFu;
    std::uint32_t const immlo = (v & 0x3u) << 29;
    std::uint32_t const immhi = ((v >> 2) & 0x7FFFFu) << 5;
    EXPECT_EQ(readInst(p.text, 0), 0x90000000u | immlo | immhi);
}

TEST(Aarch64AdrPrelPgHi21, MinNegativeInRange) {
    auto tgt = loadOneRelocTarget("aarch64_adr_prel_pg_hi21");
    ASSERT_NE(tgt, nullptr);
    // signed-21-bit min = -0x100000; patchSectionVa = 0x100000 << 12,
    // symbolVa = 0 → (0>>12) - (0x100000000>>12) = 0 - 0x100000 = -0x100000.
    auto p = applyOneReloc(tgt, 0x90000000u,
                            /*symbolVa*/ 0,
                            /*addend*/   0,
                            /*patchSectionVa*/ static_cast<std::uint64_t>(0x100000) << 12,
                            /*funcOffset*/ 0);
    ASSERT_TRUE(p.ok);
    // -0x100000 in 21-bit two's-complement = 0x100000 (bit 20 set, rest 0).
    std::uint32_t const v = 0x100000u;
    std::uint32_t const immlo = (v & 0x3u) << 29;
    std::uint32_t const immhi = ((v >> 2) & 0x7FFFFu) << 5;
    EXPECT_EQ(readInst(p.text, 0), 0x90000000u | immlo | immhi);
}

// pr-test-analyzer Rating 9: dirty-base reject for AdrPrelPgHi21 + AddAbsLo12
TEST(Aarch64AdrPrelPgHi21, RejectsBaseInstWithDirtyImmlo) {
    auto tgt = loadOneRelocTarget("aarch64_adr_prel_pg_hi21");
    ASSERT_NE(tgt, nullptr);
    // ADRP opcode + bit set in immlo[30:29].
    auto p = applyOneReloc(tgt, 0x90000000u | (1u << 29),
                            /*symbolVa*/ 0x5000,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    EXPECT_FALSE(p.ok);
}

TEST(Aarch64AdrPrelPgHi21, RejectsBaseInstWithDirtyImmhi) {
    auto tgt = loadOneRelocTarget("aarch64_adr_prel_pg_hi21");
    ASSERT_NE(tgt, nullptr);
    // ADRP opcode + bit set in immhi[23:5].
    auto p = applyOneReloc(tgt, 0x90000000u | (1u << 10),
                            /*symbolVa*/ 0x5000,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    EXPECT_FALSE(p.ok);
}

TEST(Aarch64AddAbsLo12, RejectsBaseInstWithDirtyImm12) {
    auto tgt = loadOneRelocTarget("aarch64_add_abs_lo12");
    ASSERT_NE(tgt, nullptr);
    // ADD opcode + bit set in imm12[21:10].
    auto p = applyOneReloc(tgt, 0x91000000u | (1u << 10),
                            /*symbolVa*/ 0x678,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    EXPECT_FALSE(p.ok);
}

// architect Q6 post-fold #1: AddAbsLo12 with negative S+A rejects.
TEST(Aarch64AddAbsLo12, RejectsNegativeSplusA) {
    auto tgt = loadOneRelocTarget("aarch64_add_abs_lo12");
    ASSERT_NE(tgt, nullptr);
    // symbolVa = 0, addend = -1 → SA = -1 → reject (would silently
    // truncate to 0xFFF garbage).
    auto p = applyOneReloc(tgt, 0x91000000u,
                            /*symbolVa*/ 0,
                            /*addend*/   -1,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    EXPECT_FALSE(p.ok);
}

// silent-failure H1 post-fold #1: parseRelocFormulaKind case-insensitive.
TEST(RelocFormulaKind, ParseAcceptsAllCapsAndMixedCase) {
    EXPECT_EQ(parseRelocFormulaKind("LINEAR"),                   RelocFormulaKind::Linear);
    EXPECT_EQ(parseRelocFormulaKind("Linear"),                   RelocFormulaKind::Linear);
    EXPECT_EQ(parseRelocFormulaKind("AARCH64_CALL26"),           RelocFormulaKind::Aarch64Call26);
    EXPECT_EQ(parseRelocFormulaKind("Aarch64_Adr_Prel_Pg_Hi21"), RelocFormulaKind::Aarch64AdrPrelPgHi21);
}

// pr-test-analyzer Rating 7: shipped x86_64 Linear round-trip
TEST(ShippedX86_64Target, LinearRoundTripsAllRows) {
    auto r = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(r.has_value());
    auto const& tgt = **r;
    for (auto const& name : {"rel32", "abs64", "abs32"}) {
        auto const* tri = tgt.relocationByName(name);
        ASSERT_NE(tri, nullptr) << name;
        EXPECT_EQ(tri->formulaKind, RelocFormulaKind::Linear) << name;
    }
}

// architect Q2 post-fold #1: acceptedRelocFormulaList contains every variant.
TEST(RelocFormulaKind, AcceptedListContainsAllVariants) {
    auto const list = acceptedRelocFormulaList();
    EXPECT_NE(list.find("'linear'"),                   std::string::npos);
    EXPECT_NE(list.find("'aarch64_call26'"),           std::string::npos);
    EXPECT_NE(list.find("'aarch64_adr_prel_pg_hi21'"), std::string::npos);
    EXPECT_NE(list.find("'aarch64_add_abs_lo12'"),     std::string::npos);
}

// ── Post-fold #2 (second 7-agent audit) ──────────────────────

// pr-test-analyzer Rating 8: exact format pin for acceptedRelocFormulaList
// (was loose `find()` — regression to space-separated or unquoted output
// would pass silently).
TEST(RelocFormulaKind, AcceptedListIsCommaSpaceQuotedExactly) {
    EXPECT_EQ(acceptedRelocFormulaList(),
              "'linear', 'aarch64_call26', "
              "'aarch64_adr_prel_pg_hi21', 'aarch64_add_abs_lo12'");
}

// pr-test-analyzer Rating 7: whitespace tolerance pinned as reject
// (parseRelocFormulaKind only ASCII-lowercases; doesn't trim).
TEST(RelocFormulaKind, ParseRejectsTrailingWhitespace) {
    EXPECT_EQ(parseRelocFormulaKind("linear "), std::nullopt);
    EXPECT_EQ(parseRelocFormulaKind(" linear"), std::nullopt);
}

// silent-failure CRITICAL-1 post-fold #2: AddAbsLo12 rejects S+A above UINT32_MAX.
TEST(Aarch64AddAbsLo12, RejectsSplusAAboveUint32Max) {
    auto tgt = loadOneRelocTarget("aarch64_add_abs_lo12");
    ASSERT_NE(tgt, nullptr);

    // symbolVa = 0x100000000 (= UINT32_MAX + 1) → reject.
    auto p = applyOneReloc(tgt, 0x91000000u,
                            /*symbolVa*/ 0x100000000ULL,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    EXPECT_FALSE(p.ok);
}

TEST(Aarch64AddAbsLo12, AcceptsSplusAAtUint32Max) {
    auto tgt = loadOneRelocTarget("aarch64_add_abs_lo12");
    ASSERT_NE(tgt, nullptr);

    // symbolVa = UINT32_MAX = 0xFFFFFFFF → low 12 = 0xFFF → patched.
    auto p = applyOneReloc(tgt, 0x91000000u,
                            /*symbolVa*/ 0xFFFFFFFFULL,
                            /*addend*/   0,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(readInst(p.text, 0), 0x91000000u | (0xFFFu << 10));
}

// silent-failure CRITICAL-2 post-fold #2: validate() rule (e) enforced
// even for programmatically-built rows (was JSON-loader-only).
TEST(TargetSchemaValidate, RuleERejectsNonLinearWithPcRelative) {
    // The JSON loader catches this too, but the rule (e) belongs
    // ALSO in validate() for defense-in-depth (catches future
    // variant-reshape constructors / fuzz harnesses / in-memory
    // schemas that bypass the loader).
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

TEST(TargetSchemaValidate, RuleERejectsNonLinearWithAddendBias) {
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"bad"},
      "relocations":[
        { "name": "x", "kind": 1, "formula": "aarch64_call26", "addendBias": 4 }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_FALSE(r.has_value());
}

// pr-test-analyzer Rating 7: AdrPrelPgHi21 negative-S+A pin
// (well-defined: just a normal page-pair shift with arithmetic right shift).
TEST(Aarch64AdrPrelPgHi21, AcceptsNegativeSplusAWhenInRange) {
    auto tgt = loadOneRelocTarget("aarch64_adr_prel_pg_hi21");
    ASSERT_NE(tgt, nullptr);

    // symbolVa = 0, addend = -0x1000 → SA = -0x1000 → (SA>>12)=-1.
    // patchSectionVa = 0, funcOffset = 0 → (P>>12) = 0.
    // value = -1 - 0 = -1 → fits signed 21-bit → 21-bit two's-complement = 0x1FFFFF.
    auto p = applyOneReloc(tgt, 0x90000000u,
                            /*symbolVa*/ 0,
                            /*addend*/   -0x1000,
                            /*patchSectionVa*/ 0,
                            /*funcOffset*/ 0);
    ASSERT_TRUE(p.ok);
    std::uint32_t const v = 0x1FFFFFu;
    std::uint32_t const immlo = (v & 0x3u) << 29;
    std::uint32_t const immhi = ((v >> 2) & 0x7FFFFu) << 5;
    EXPECT_EQ(readInst(p.text, 0), 0x90000000u | immlo | immhi);
}

// pr-test-analyzer Rating 6: direct unit tests for byte_emit positional helpers.
TEST(ByteEmit, ReadU32LEAtRoundTrip) {
    std::vector<std::uint8_t> buf(16, 0);
    dss::link::format::detail::writeU32LEAt(buf, 4, 0xDEADBEEFu);
    EXPECT_EQ(dss::link::format::detail::readU32LEAt(buf, 4), 0xDEADBEEFu);
}

TEST(ByteEmit, ReadU32LEAtAllZerosAllOnes) {
    std::vector<std::uint8_t> buf(8, 0);
    dss::link::format::detail::writeU32LEAt(buf, 0, 0x00000000u);
    EXPECT_EQ(dss::link::format::detail::readU32LEAt(buf, 0), 0x00000000u);
    dss::link::format::detail::writeU32LEAt(buf, 0, 0xFFFFFFFFu);
    EXPECT_EQ(dss::link::format::detail::readU32LEAt(buf, 0), 0xFFFFFFFFu);
}

TEST(ByteEmit, WriteU32LEAtPlacesLittleEndianBytes) {
    std::vector<std::uint8_t> buf(8, 0);
    dss::link::format::detail::writeU32LEAt(buf, 2, 0x12345678u);
    EXPECT_EQ(buf[2], 0x78);
    EXPECT_EQ(buf[3], 0x56);
    EXPECT_EQ(buf[4], 0x34);
    EXPECT_EQ(buf[5], 0x12);
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
