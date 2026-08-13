// D-ASM-ARM64-BITWISE-W-FORMS-UNDECLARED + D-ARM64-C-PATH-32BIT-BITWISE-WIDENS-TO-64:
// the byte-exact W/X-form pins for AND / ORR / EOR on aarch64.
//
// ★★★ WHY THIS FILE EXISTS. Before the cycle that added them, arm64's `and` /
// `or` / `xor` each declared exactly ONE encoding variant, and that variant was
// width-ABSENT. `walker_util::variantMatchesInst` treats a zero
// `guardWidthBits` as "matches ANY width", so a width-32 LIR instruction — what
// a C `int a & b` lowers to — matched the 64-bit X-form template and emitted a
// 64-bit instruction for an operation the source declared 32 bits wide. Their
// siblings did not: `not`/`neg`/`shl`/`shr_l`/`shr_a`/`sdiv`/`udiv`/`msub` were
// all width-keyed and all emitted W-forms for the same `int`. ✔MEASURED in ONE
// binary (examples/c-subset/core_bitwise, arm64 debug, before the fix):
//     b_not: 2a3d03fd  mvn w29, w29        ← W-form (`not` declared width:32)
//     b_shl: 1adc23bc  lsl w28, w29, w28   ← W-form (`shl` declared width:32)
//     b_and: 8a1c03bc  and x28, x29, x28   ← X-form — THE DEFECT
//
// ★★ THE TWO FACES OF ONE DEFECT, AND WHY BOTH ARE PINNED HERE. The missing
// W-forms broke two callers at once, and only one of them was visible:
//   * the ASSEMBLY path FAILED LOUD — the dialect had to pin `width: 64` on its
//     `and`/`orr`/`eor` rows, so `and w0, w1, w2` was refused outright by
//     `effectiveWidth` ("the spelling and the registers disagree");
//   * the C path was SILENT — it just emitted the wider instruction.
// A test covering only the dialect would leave the silent half unguarded, so
// `CPathWidth32ElectsTheWForm` below drives the SAME opcode through hand-built
// LIR at width 32, which is the shape `mir_to_lir` produces and the shape that
// has no dialect involvement at all.
//
// ★★ EVERY WORD BELOW IS MEASURED AGAINST REAL BINUTILS, never derived from the
// ARM ARM by hand. `aarch64-linux-gnu-as` + `objdump` 2.42:
//     0a020020  and w0, w1, w2       8a020020  and x0, x1, x2
//     2a020020  orr w0, w1, w2       aa020020  orr x0, x1, x2
//     4a020020  eor w0, w1, w2       ca020020  eor x0, x1, x2
// The W form is the X form with sf (bit 31) cleared, and the register fields sit
// identically in both (`and w3,w4,w5` = 0x0a050083 ⇒ Rm[20:16] Rn[9:5] Rd[4:0]),
// which is why the two variants legitimately share one wire set.
//
// ★ THE PINS ARE EXACT 4-BYTE EQUALITY, NOT A SHAPE CHECK, and that is the whole
// point of the file. A regression that re-widens the 32-bit form is a ONE-BIT
// change, so anything weaker — "is it an AND?", a disassembler-string match, an
// instruction count — stays green through exactly the defect this file exists to
// catch. The X-form pins are here for the same reason in reverse: a fix that
// keyed the widths but swapped the two templates would satisfy any test that
// only pinned the W side.

#include "asm/asm.hpp"
#include "asm_text_fixture.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"

#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::test_support::asm_text::lowerAsmTextWithTarget;
using dss::test_support::asm_text::messages;
using dss::test_support::asm_text::parsedCleanly;
using dss::test_support::asm_text::shippedDialectDoc;

namespace {

// ── the measured words ────────────────────────────────────────────
constexpr std::uint32_t kAndW = 0x0A020020u;
constexpr std::uint32_t kAndX = 0x8A020020u;
constexpr std::uint32_t kOrrW = 0x2A020020u;
constexpr std::uint32_t kOrrX = 0xAA020020u;
constexpr std::uint32_t kEorW = 0x4A020020u;
constexpr std::uint32_t kEorX = 0xCA020020u;

// AArch64's 32/64 selector. The ONLY bit that separates each pair above.
constexpr std::uint32_t kSfBit = 0x80000000u;

// The `.s` every dialect pin assembles. The six forms in a fixed order with
// DISTINCT operand registers, so each of the six words is UNIQUE in the output —
// which is what lets the red-on-disable pins below make an unambiguous ABSENCE
// claim about one specific word.
constexpr std::string_view kBitwiseSource =
    ".globl main\n"
    ".type main, %function\n"
    "main:\n"
    "  and w0, w1, w2\n"
    "  and x0, x1, x2\n"
    "  orr w0, w1, w2\n"
    "  orr x0, x1, x2\n"
    "  eor w0, w1, w2\n"
    "  eor x0, x1, x2\n"
    "  ret\n";

[[nodiscard]] std::uint32_t wordAt(std::span<std::uint8_t const> b,
                                   std::size_t index) {
    std::size_t const o = index * 4;
    return std::uint32_t{b[o]} | (std::uint32_t{b[o + 1]} << 8)
         | (std::uint32_t{b[o + 2]} << 16) | (std::uint32_t{b[o + 3]} << 24);
}

// ★★ THE ONE MATCHER BOTH DIRECTIONS USE. The positive pins assert a word is
// present at a known index; the red-on-disable pins assert the SAME word is
// absent from the mutant's output. Routing both through this function is what
// makes the absence claim mean the negative of the presence claim — a private
// "did it disappear?" helper is free to drift from the pin it is the negative
// of, and then a mutant that still emits the witness reads as green.
[[nodiscard]] std::size_t countWord(std::span<std::uint8_t const> bytes,
                                    std::uint32_t word) {
    std::size_t n = 0;
    for (std::size_t i = 0; (i + 1) * 4 <= bytes.size(); ++i) {
        if (wordAt(bytes, i) == word) ++n;
    }
    return n;
}

// Lower `kBitwiseSource` through the shipped arm64 gas dialect against `target`
// (which may be a MUTANT) and assemble the single function it defines.
// `assembleOk` reports whether the whole path succeeded; a refusal at EITHER
// tier (text→LIR lowering or encoding) leaves the byte vector empty, which is
// what the red-on-disable pins require.
struct AssembledSource {
    std::vector<std::uint8_t> bytes;
    bool                      loweringOk = false;
    bool                      encodeOk   = false;
    std::string               diagnostics;
};

[[nodiscard]] AssembledSource
assembleBitwiseSource(std::shared_ptr<TargetSchema> target) {
    AssembledSource out;
    auto doc = shippedDialectDoc("asm-arm64-gas");
    auto run = lowerAsmTextWithTarget(doc, kBitwiseSource, std::move(target));

    // A pin that cannot tell "the dialect refused" from "the source did not
    // parse" is a pin that can go green for the wrong reason.
    EXPECT_TRUE(parsedCleanly(*run))
        << "the witness .s must PARSE before any encoding claim is meaningful:\n"
        << dss::test_support::asm_text::parseMessages(*run);

    out.diagnostics = messages(*run);
    if (!run->module.has_value()) return out;
    out.loweringOk = true;

    std::vector<MirInstId> lirToMir(run->module->lir.instCount());
    DiagnosticReporter     rep;
    auto r = assemble(run->module->lir, *run->target, lirToMir, rep);
    out.diagnostics += rep.errorCount() != 0 ? "encode: errors\n" : "";
    if (rep.errorCount() != 0 || r.functions.empty()) return out;
    out.encodeOk = true;
    out.bytes    = r.functions[0].bytes;
    return out;
}

[[nodiscard]] std::shared_ptr<TargetSchema> shippedArm64() {
    auto s = TargetSchema::loadShipped("arm64");
    EXPECT_TRUE(s.has_value()) << "the shipped arm64 target must load";
    return s.has_value() ? *s : nullptr;
}

} // namespace

// ══ the positive pins ══════════════════════════════════════════════

// Exact 4-byte equality for all six forms, in source order.
TEST(Arm64BitwiseWForms, WAndXFormsEncodeTheirMeasuredWords) {
    auto const a = assembleBitwiseSource(shippedArm64());
    ASSERT_TRUE(a.loweringOk) << "text→LIR refused:\n" << a.diagnostics;
    ASSERT_TRUE(a.encodeOk)   << "encoding refused:\n" << a.diagnostics;
    // six bitwise instructions + `ret`, one 4-byte word each.
    ASSERT_EQ(a.bytes.size(), 7u * 4u);

    EXPECT_EQ(wordAt(a.bytes, 0), kAndW) << "and w0, w1, w2";
    EXPECT_EQ(wordAt(a.bytes, 1), kAndX) << "and x0, x1, x2";
    EXPECT_EQ(wordAt(a.bytes, 2), kOrrW) << "orr w0, w1, w2";
    EXPECT_EQ(wordAt(a.bytes, 3), kOrrX) << "orr x0, x1, x2";
    EXPECT_EQ(wordAt(a.bytes, 4), kEorW) << "eor w0, w1, w2";
    EXPECT_EQ(wordAt(a.bytes, 5), kEorX) << "eor x0, x1, x2";
}

// ★ THE WITNESSES ARE UNIQUE, which is the precondition the red-on-disable pins
// below depend on: "this word vanished from the output" only means "this variant
// stopped being elected" when the word had exactly one source to begin with.
TEST(Arm64BitwiseWForms, EachWitnessWordIsUniqueInTheSubject) {
    auto const a = assembleBitwiseSource(shippedArm64());
    ASSERT_TRUE(a.encodeOk) << a.diagnostics;
    for (auto const [word, name] : {std::pair{kAndW, "AND W"},
                                    std::pair{kAndX, "AND X"},
                                    std::pair{kOrrW, "ORR W"},
                                    std::pair{kOrrX, "ORR X"},
                                    std::pair{kEorW, "EOR W"},
                                    std::pair{kEorX, "EOR X"}}) {
        EXPECT_EQ(countWord(a.bytes, word), 1u)
            << name << " must appear exactly once for the absence pins to mean "
               "what they claim";
    }
}

// ★ THE PAIRS DIFFER IN EXACTLY ONE BIT. This pins the RELATIONSHIP rather than
// the two constants independently: an edit that changes one template of a pair
// and not the other trips here even if both remain plausible AArch64 words.
TEST(Arm64BitwiseWForms, EachWPairIsItsXPairWithTheSfBitCleared) {
    EXPECT_EQ(kAndW, kAndX & ~kSfBit);
    EXPECT_EQ(kOrrW, kOrrX & ~kSfBit);
    EXPECT_EQ(kEorW, kEorX & ~kSfBit);

    auto const a = assembleBitwiseSource(shippedArm64());
    ASSERT_TRUE(a.encodeOk) << a.diagnostics;
    EXPECT_EQ(wordAt(a.bytes, 0), wordAt(a.bytes, 1) & ~kSfBit) << "AND pair";
    EXPECT_EQ(wordAt(a.bytes, 2), wordAt(a.bytes, 3) & ~kSfBit) << "ORR pair";
    EXPECT_EQ(wordAt(a.bytes, 4), wordAt(a.bytes, 5) & ~kSfBit) << "EOR pair";
}

// ══ the C-path pin — the SILENT half of the defect ═════════════════

// ★★ NO DIALECT INVOLVED. This builds the LIR shape `mir_to_lir` emits for a C
// `int a & b` — one `and` with `kLirInstFlagWidth32` — and pins the W-form word.
// It is the regression guard for the byte change the operator accepted: before
// the W-forms existed this emitted 0x8A020020, the 64-bit instruction, with no
// diagnostic anywhere.
TEST(Arm64BitwiseWForms, CPathWidth32ElectsTheWForm) {
    auto s = shippedArm64();
    ASSERT_NE(s, nullptr);
    auto const andOp = s->opcodeByMnemonic("and");
    auto const retOp = s->opcodeByMnemonic("ret");
    ASSERT_TRUE(andOp.has_value() && retOp.has_value());
    auto const x0 = s->registerByName("x0");
    auto const x1 = s->registerByName("x1");
    auto const x2 = s->registerByName("x2");
    ASSERT_TRUE(x0.has_value() && x1.has_value() && x2.has_value());

    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const r0{static_cast<std::uint32_t>(*x0), 1, cls};
    LirReg const r1{static_cast<std::uint32_t>(*x1), 1, cls};
    LirReg const r2{static_cast<std::uint32_t>(*x2), 1, cls};

    // Same builder shape at both widths — the ONLY difference is the width flag,
    // so a difference in the emitted word can only come from variant election.
    auto encodeAt = [&](std::uint8_t flags) -> std::uint32_t {
        LirBuilder b{*s};
        (void)b.addFunction(SymbolId{1});
        auto blk = b.createBlock();
        b.beginBlock(blk);
        LirOperand const ops[] = {LirOperand::makeReg(r1),
                                  LirOperand::makeReg(r2)};
        (void)b.addInst(*andOp, r0, ops, /*payload=*/0, flags);
        (void)b.addReturn(*retOp, {});
        Lir lir = std::move(b).finish();

        std::vector<MirInstId> lirToMir(lir.instCount());
        DiagnosticReporter     rep;
        auto r = assemble(lir, *s, lirToMir, rep);
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_FALSE(r.functions.empty());
        if (r.functions.empty() || r.functions[0].bytes.size() < 4) return 0;
        return wordAt(r.functions[0].bytes, 0);
    };

    EXPECT_EQ(encodeAt(kLirInstFlagWidth32), kAndW)
        << "a width-32 `and` must elect the W-form — emitting the X-form here is "
           "the silent widening D-ARM64-C-PATH-32BIT-BITWISE-WIDENS-TO-64 names";
    EXPECT_EQ(encodeAt(/*flags=*/0), kAndX)
        << "a flags-less (width-64) `and` must still elect the X-form";
}

// ══ RED-ON-DISABLE, against the REAL shipped target ════════════════
//
// ★★★ FAIL-CLOSED, AND EVERY CLAUSE IS LOAD-BEARING. Each pin below proves four
// things about the mutant, because a red-on-disable that skips any one of them
// can report success over a mutation that never took effect:
//   1. the mutant TEXT DIFFERS from the shipped text — compared as full content,
//      never by line count (a reformat changes lines and mutates nothing; and a
//      full-text comparison is strictly stronger than a digest, since it cannot
//      collide);
//   2. the mutant still PARSES — a mutant that fails to load would make every
//      downstream assertion vacuously true, which is the classic false red;
//   3. the surviving variant set is SMALLER BY EXACTLY ONE — the mutation hit
//      what it aimed at and nothing else;
//   4. the witness word is ABSENT from the mutant's output, measured with
//      `countWord` — the SAME matcher the positive pins use.

namespace {

// Strip the width-32 variant from one opcode of the shipped arm64 target.
// Returns the mutant schema plus the evidence clauses 1–3 above need.
struct VariantMutant {
    std::shared_ptr<TargetSchema> schema;
    bool        parsed          = false;
    bool        textDiffers     = false;
    std::size_t variantsBefore  = 0;
    std::size_t variantsAfter   = 0;
    std::string loadErrors;
};

[[nodiscard]] VariantMutant stripWidth32Variant(std::string_view mnemonic) {
    VariantMutant out;
    std::string   shippedText;
    std::string   mutantText;

    auto schemaR = dss::test_support::mutateShippedTargetSchemaDoc(
        "arm64", [&](nlohmann::json& doc) {
            shippedText = doc.dump();
            for (auto& op : doc["opcodes"]) {
                auto it = op.find("mnemonic");
                if (it == op.end() || !it->is_string()) continue;
                if (it->get<std::string>() != mnemonic) continue;
                auto& variants = op["encoding"]["variants"];
                out.variantsBefore = variants.size();
                for (std::size_t i = 0; i < variants.size(); ++i) {
                    auto const& g = variants[i]["guard"];
                    if (g.contains("width") && g["width"] == 32) {
                        variants.erase(variants.begin()
                                       + static_cast<long>(i));
                        break;
                    }
                }
                out.variantsAfter = variants.size();
            }
            mutantText = doc.dump();
        });

    out.textDiffers = !shippedText.empty() && shippedText != mutantText;
    out.parsed      = schemaR.has_value();
    if (schemaR.has_value()) {
        out.schema = *schemaR;
    } else {
        for (auto const& d : schemaR.error()) {
            out.loadErrors += d.path + ": " + d.message + "\n";
        }
    }
    return out;
}

} // namespace

TEST(Arm64BitwiseWForms, StrippingAWFormVariantRemovesItsWitnessWord) {
    struct Case {
        std::string_view mnemonic;   // the TARGET opcode name, not the spelling
        std::uint32_t    witnessW;
        std::uint32_t    survivingX;
    };
    // ⚠ THE TARGET NAMES, NOT THE gas SPELLINGS — DSS calls them `or`/`xor`
    // where gas writes `orr`/`eor`, which is the whole reason the dialect
    // carries an `instructions[]` mapping table.
    constexpr Case kCases[] = {
        {"and", kAndW, kAndX},
        {"or",  kOrrW, kOrrX},
        {"xor", kEorW, kEorX},
    };

    for (auto const& c : kCases) {
        SCOPED_TRACE(std::string{"opcode "} + std::string{c.mnemonic});
        auto const m = stripWidth32Variant(c.mnemonic);

        // 1 — the mutation actually changed the document's CONTENT.
        EXPECT_TRUE(m.textDiffers)
            << "the mutant text is identical to the shipped text — the mutation "
               "did not take, so anything below would be a false red";
        // 3 — and it removed exactly one variant.
        EXPECT_EQ(m.variantsBefore, 2u)
            << "expected the shipped opcode to declare BOTH widths";
        EXPECT_EQ(m.variantsAfter, 1u)
            << "the mutation must remove exactly one variant";
        // 2 — the mutant is still a loadable schema.
        ASSERT_TRUE(m.parsed)
            << "the mutant must still PARSE, else the absence check below is "
               "vacuous:\n" << m.loadErrors;
        ASSERT_NE(m.schema, nullptr);

        // 4 — the witness is GONE, by the same matcher the positive pins use.
        auto const a = assembleBitwiseSource(m.schema);
        EXPECT_EQ(countWord(a.bytes, c.witnessW), 0u)
            << "the W-form witness survived removal of the only variant that "
               "can encode it — the pin is not measuring what it claims";

        // ★ AND THE REFUSAL IS LOUD. With no width-32 variant the dialect's
        // width-32 request matches nothing, so election fails and the whole
        // source is refused — it must NOT silently fall back to the X-form.
        EXPECT_FALSE(a.encodeOk)
            << "the mutant assembled cleanly — a missing width-keyed variant "
               "must fail loud, never widen silently";
        EXPECT_FALSE(a.diagnostics.empty())
            << "a refusal must say something";
    }
}
