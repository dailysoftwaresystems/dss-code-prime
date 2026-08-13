// D-ASM-ARM64-MOVZ-W-FORM-UNDECLARED: the byte-exact W/X-form pins for the
// arm64 immediate move (MOVZ), the sibling of `test_asm_arm64_bitwise_w.cpp`.
//
// ★★★ WHY THIS FILE EXISTS, AND HOW IT DIFFERS FROM ITS SIBLING. The bitwise
// cycle closed a SILENT defect: `and`/`or`/`xor` each declared one width-ABSENT
// variant, and `walker_util::variantMatchesInst` treats a zero `guardWidthBits`
// as "matches ANY width", so a width-32 C `int a & b` matched the 64-bit
// template and emitted a 64-bit instruction with no diagnostic. MOVZ's defect
// was the OPPOSITE shape and it is worth stating plainly, because the fix looks
// identical and the justification is not:
//   * MOVZ's two forms are VALUE-IDENTICAL — an X-form MOVZ zeroes all 64 bits,
//     and a W-register write zero-extends to the same 64-bit result — so the C
//     path was never miscompiled here;
//   * what was missing was any way for a caller to SAY width 32. The
//     `asm_elect::variantHonorsDeclaredWidth` gate refuses a width-absent
//     variant elected by a width-KEYED request rather than silently dropping
//     the width, so `mov w0, #5` was a hard error in a hand-written `.s`.
// ⇒ this change buys EXPRESSIVENESS and costs BYTES; the bitwise change bought
// CORRECTNESS. Both are pinned the same way because the regression shape is the
// same one bit either way.
//
// ★★ EVERY WORD BELOW IS MEASURED AGAINST REAL BINUTILS, never derived from the
// ARM ARM by hand. `aarch64-linux-gnu-as` + `aarch64-linux-gnu-objdump` 2.42:
//     528000a0  mov w0, #5        d28000a0  mov x0, #5
//     52800101  mov w1, #8        d2800101  mov x1, #8
//     529fffe3  mov w3, #65535    d29fffe3  mov x3, #65535
//     5280003e  mov w30, #1       d280003e  mov x30, #1
// gas ALIASES the W `mov` onto MOVZ — `movz w1, #8` assembles to the SAME
// 0x52800101. ★ THE `sf`-ONLY RELATIONSHIP WAS CHECKED, NOT ASSUMED: every pair
// above XORs to exactly 0x80000000. That mattered here more than it did for the
// bitwise family, because MOVZ / MOVN / MOVK are separated by `opc` at bits
// 30:29 — a wrong-opc word would still have disassembled as a plausible move.
//
// ★ THE PINS ARE EXACT 4-BYTE EQUALITY, NOT A SHAPE CHECK. A regression that
// re-widens the 32-bit form is a ONE-BIT change, so anything weaker — "is it a
// MOVZ?", a disassembler-string match, an instruction count — stays green
// through exactly the defect this file exists to catch. The X-form pins are
// here for the same reason in reverse: a fix that keyed the widths but SWAPPED
// the two templates would satisfy any test that only pinned the W side.

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
constexpr std::uint32_t kMovzW0_5     = 0x528000A0u;
constexpr std::uint32_t kMovzX0_5     = 0xD28000A0u;
constexpr std::uint32_t kMovzW1_8     = 0x52800101u;
constexpr std::uint32_t kMovzX1_8     = 0xD2800101u;
constexpr std::uint32_t kMovzW3_65535 = 0x529FFFE3u;
constexpr std::uint32_t kMovzX3_65535 = 0xD29FFFE3u;

// The two shipped templates, i.e. the words with every wired field zero.
constexpr std::uint32_t kMovzWBase = 0x52800000u;
constexpr std::uint32_t kMovzXBase = 0xD2800000u;

// AArch64's 32/64 selector. The ONLY bit that separates each pair above.
constexpr std::uint32_t kSfBit = 0x80000000u;

// The `.s` every dialect pin assembles. Three W/X PAIRS, each pair sharing a
// destination register and an immediate so the two words differ ONLY in `sf`,
// and all six words distinct from each other — which is what lets the absence
// pins below make an unambiguous claim about one specific word. The three
// immediates are chosen to walk the imm16 slot rather than repeat it: 5 (the
// shape the anchor names), 8, and 65535 (the slot's measured ceiling).
constexpr std::string_view kMovzSource =
    ".globl main\n"
    ".type main, %function\n"
    "main:\n"
    "  mov w0, #5\n"
    "  mov x0, #5\n"
    "  mov w1, #8\n"
    "  mov x1, #8\n"
    "  mov w3, #65535\n"
    "  mov x3, #65535\n"
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

struct AssembledSource {
    std::vector<std::uint8_t> bytes;
    bool                      loweringOk = false;
    bool                      encodeOk   = false;
    std::string               diagnostics;
};

// Lower `kMovzSource` through the shipped arm64 gas dialect against `target`
// (which may be a MUTANT) and assemble the single function it defines. A
// refusal at EITHER tier (text→LIR lowering or encoding) leaves the byte vector
// empty, which is what the red-on-disable pins require.
[[nodiscard]] AssembledSource
assembleMovzSource(std::shared_ptr<TargetSchema> target) {
    AssembledSource out;
    auto doc = shippedDialectDoc("asm-arm64-gas");
    auto run = lowerAsmTextWithTarget(doc, kMovzSource, std::move(target));

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

// The MOVZ arms of the shipped `mov` opcode: the `["imm32"]` variants that do
// NOT carry the sign axis (those are the two MOVN arms). Returned as
// (guardWidthBits, fixedWord) pairs in declaration order.
[[nodiscard]] std::vector<std::pair<std::uint8_t, std::uint32_t>>
movzArmsOf(TargetSchema const& s) {
    std::vector<std::pair<std::uint8_t, std::uint32_t>> out;
    auto const op = s.opcodeByMnemonic("mov");
    if (!op.has_value()) return out;
    auto const* info = s.opcodeInfo(*op);
    if (info == nullptr) return out;
    for (auto const& v : info->encoding.variants) {
        if (v.negValue) continue;
        if (v.operandKinds.size() != 1) continue;
        if (v.operandKinds[0] != OperandKindFilter::ImmInt) continue;
        out.emplace_back(v.guardWidthBits, v.tmpl.fixedWord);
    }
    return out;
}

} // namespace

// ══ the SHIPPED-SCHEMA pin ═════════════════════════════════════════
//
// ★★★ THIS PIN IS ALSO THE PROOF OF WHICH CONFIG TREE THE PROCESS READ. Every
// other pin in this file reaches the shipped arm64 document through
// `findShippedConfig`, which honours `$DSS_CONFIG_ROOT` and otherwise WALKS THE
// CWD — so a test binary launched from a different tree silently reads that
// tree's config (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE). `ctest`
// sets the variable; a bare `.exe` invocation does not. This test fails loudly
// when the document actually read is not the one under test, which turns a
// mis-rooted run into a red instead of a meaningless green.
TEST(Arm64MovzWForm, ShippedTargetDeclaresBothMovzWidths) {
    auto s = shippedArm64();
    ASSERT_NE(s, nullptr);
    auto const arms = movzArmsOf(*s);
    ASSERT_EQ(arms.size(), 2u)
        << "the shipped arm64 `mov` must declare EXACTLY two non-negative "
           "immediate (MOVZ) arms — one per width";
    EXPECT_EQ(arms[0].first, 64u);
    EXPECT_EQ(arms[0].second, kMovzXBase);
    EXPECT_EQ(arms[1].first, 32u);
    EXPECT_EQ(arms[1].second, kMovzWBase);
}

// ══ the positive pins ══════════════════════════════════════════════

TEST(Arm64MovzWForm, WAndXFormsEncodeTheirMeasuredWords) {
    auto const a = assembleMovzSource(shippedArm64());
    ASSERT_TRUE(a.loweringOk) << "text→LIR refused:\n" << a.diagnostics;
    ASSERT_TRUE(a.encodeOk)   << "encoding refused:\n" << a.diagnostics;
    // six immediate moves + `ret`, one 4-byte word each.
    ASSERT_EQ(a.bytes.size(), 7u * 4u);

    EXPECT_EQ(wordAt(a.bytes, 0), kMovzW0_5)     << "mov w0, #5";
    EXPECT_EQ(wordAt(a.bytes, 1), kMovzX0_5)     << "mov x0, #5";
    EXPECT_EQ(wordAt(a.bytes, 2), kMovzW1_8)     << "mov w1, #8";
    EXPECT_EQ(wordAt(a.bytes, 3), kMovzX1_8)     << "mov x1, #8";
    EXPECT_EQ(wordAt(a.bytes, 4), kMovzW3_65535) << "mov w3, #65535";
    EXPECT_EQ(wordAt(a.bytes, 5), kMovzX3_65535) << "mov x3, #65535";
}

// ★ THE WITNESSES ARE UNIQUE, which is the precondition the absence pins below
// depend on: "this word vanished from the output" only means "this variant
// stopped being elected" when the word had exactly one source to begin with.
TEST(Arm64MovzWForm, EachWitnessWordIsUniqueInTheSubject) {
    auto const a = assembleMovzSource(shippedArm64());
    ASSERT_TRUE(a.encodeOk) << a.diagnostics;
    for (auto const [word, name] : {std::pair{kMovzW0_5,     "MOVZ W0 #5"},
                                    std::pair{kMovzX0_5,     "MOVZ X0 #5"},
                                    std::pair{kMovzW1_8,     "MOVZ W1 #8"},
                                    std::pair{kMovzX1_8,     "MOVZ X1 #8"},
                                    std::pair{kMovzW3_65535, "MOVZ W3 #65535"},
                                    std::pair{kMovzX3_65535, "MOVZ X3 #65535"}}) {
        EXPECT_EQ(countWord(a.bytes, word), 1u)
            << name << " must appear exactly once for the absence pins to mean "
               "what they claim";
    }
}

// ★ THE PAIRS DIFFER IN EXACTLY ONE BIT. This pins the RELATIONSHIP rather than
// the six constants independently: an edit that changes one template of a pair
// and not the other trips here even if both remain plausible AArch64 words.
TEST(Arm64MovzWForm, EachWPairIsItsXPairWithTheSfBitCleared) {
    EXPECT_EQ(kMovzW0_5,     kMovzX0_5     & ~kSfBit);
    EXPECT_EQ(kMovzW1_8,     kMovzX1_8     & ~kSfBit);
    EXPECT_EQ(kMovzW3_65535, kMovzX3_65535 & ~kSfBit);
    EXPECT_EQ(kMovzWBase,    kMovzXBase    & ~kSfBit);

    auto const a = assembleMovzSource(shippedArm64());
    ASSERT_TRUE(a.encodeOk) << a.diagnostics;
    EXPECT_EQ(wordAt(a.bytes, 0), wordAt(a.bytes, 1) & ~kSfBit) << "#5 pair";
    EXPECT_EQ(wordAt(a.bytes, 2), wordAt(a.bytes, 3) & ~kSfBit) << "#8 pair";
    EXPECT_EQ(wordAt(a.bytes, 4), wordAt(a.bytes, 5) & ~kSfBit) << "#65535 pair";
}

// ══ the C-path pin — the byte change the operator accepted ═════════

namespace {

// Build the LIR shape `mir_to_lir` emits for a C constant materialization —
// ONE `mov` whose single operand is an ImmInt32 — at the given width flags, and
// return the encoded word. NO DIALECT INVOLVED: this is the shape that reaches
// the encoder from the C front end, which has no `.s` anywhere in its path.
[[nodiscard]] std::uint32_t
encodeConstMov(TargetSchema const& s, std::int32_t imm, std::uint8_t flags,
               std::string_view destReg, DiagnosticReporter& rep) {
    auto const movOp = s.opcodeByMnemonic("mov");
    auto const retOp = s.opcodeByMnemonic("ret");
    EXPECT_TRUE(movOp.has_value() && retOp.has_value());
    if (!movOp.has_value() || !retOp.has_value()) return 0;
    auto const dst = s.registerByName(destReg);
    EXPECT_TRUE(dst.has_value());
    if (!dst.has_value()) return 0;

    auto const  cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const rd{static_cast<std::uint32_t>(*dst), 1, cls};

    LirBuilder b{s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {LirOperand::makeImmInt32(imm)};
    (void)b.addInst(*movOp, rd, ops, /*payload=*/0, flags);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    auto r = assemble(lir, s, lirToMir, rep);
    if (r.functions.empty() || r.functions[0].bytes.size() < 4) return 0;
    return wordAt(r.functions[0].bytes, 0);
}

} // namespace

// ★★ THE REGRESSION GUARD FOR THE ACCEPTED BYTE CHANGE. Before the W-form
// existed, a width-32 constant emitted 0xD28000A0 — the 64-bit MOVZ — which was
// value-identical and therefore invisible. It now emits 0x528000A0, and the
// flags-less case must STILL emit the X-form: a fix that keyed the widths but
// swapped the templates would satisfy a test that pinned only the W side.
TEST(Arm64MovzWForm, CPathWidth32ElectsTheWForm) {
    auto s = shippedArm64();
    ASSERT_NE(s, nullptr);

    DiagnosticReporter rep32;
    EXPECT_EQ(encodeConstMov(*s, 5, kLirInstFlagWidth32, "x0", rep32),
              kMovzW0_5)
        << "a width-32 constant `mov` must elect the W-form MOVZ";
    EXPECT_EQ(rep32.errorCount(), 0u);

    DiagnosticReporter rep64;
    EXPECT_EQ(encodeConstMov(*s, 5, /*flags=*/0, "x0", rep64), kMovzX0_5)
        << "a flags-less (width-64) constant `mov` must still elect the X-form";
    EXPECT_EQ(rep64.errorCount(), 0u);

    // And the pair relationship holds through the C path too, not only through
    // the dialect — the two are separate election sites.
    DiagnosticReporter repA, repB;
    EXPECT_EQ(encodeConstMov(*s, 65535, kLirInstFlagWidth32, "x3", repA),
              kMovzW3_65535);
    EXPECT_EQ(encodeConstMov(*s, 65535, /*flags=*/0, "x3", repB),
              kMovzX3_65535);
}

// ══ RED-ON-DISABLE, against the REAL shipped target ════════════════
//
// ★★★ FAIL-CLOSED, AND EVERY CLAUSE IS LOAD-BEARING. A red-on-disable that
// skips any one of these can report success over a mutation that never took
// effect:
//   0. the document the mutator READ already carried the subject — proving the
//      bytes under test reached this process, not merely some tree on disk;
//   1. the mutant TEXT DIFFERS from the shipped text, compared as full content
//      and never by line count (a reformat changes lines and mutates nothing);
//   2. the mutant still PARSES — a mutant that fails to load makes every
//      downstream assertion vacuously true, the classic false red;
//   3. the surviving variant set is SMALLER BY EXACTLY ONE — the mutation hit
//      what it aimed at and nothing else;
//   4. the witness word is ABSENT from the mutant's output, measured with
//      `countWord`, the SAME matcher the positive pins use.

namespace {

struct MovzMutant {
    std::shared_ptr<TargetSchema> schema;
    bool        parsed              = false;
    bool        textDiffers         = false;
    bool        sawWidth32MovzInDoc = false;   // clause 0
    std::size_t variantsBefore      = 0;
    std::size_t variantsAfter       = 0;
    std::string loadErrors;
};

// Is this JSON variant object a non-negative `["imm32"]` (MOVZ) arm at `width`?
// `width == 0` means "the width key is absent".
[[nodiscard]] bool isMovzArmAt(nlohmann::json const& v, int width) {
    auto const& g = v["guard"];
    if (g.value("negValue", false)) return false;
    if (!g.contains("operandKinds")) return false;
    auto const& k = g["operandKinds"];
    if (k.size() != 1 || k[0] != "imm32") return false;
    int const w = g.contains("width") ? g["width"].get<int>() : 0;
    return w == width;
}

// Apply `edit` to the shipped arm64 `mov` opcode's variant array and rebuild
// the schema, collecting the fail-closed evidence.
template <typename EditFn>
[[nodiscard]] MovzMutant mutateMovVariants(EditFn&& edit) {
    MovzMutant  out;
    std::string shippedText;
    std::string mutantText;

    auto schemaR = dss::test_support::mutateShippedTargetSchemaDoc(
        "arm64", [&](nlohmann::json& doc) {
            shippedText = doc.dump();
            for (auto& op : doc["opcodes"]) {
                auto it = op.find("mnemonic");
                if (it == op.end() || !it->is_string()) continue;
                if (it->get<std::string>() != "mov") continue;
                auto& variants = op["encoding"]["variants"];
                out.variantsBefore = variants.size();
                // Clause 0: the DOCUMENT THIS PROCESS READ must already carry
                // the width-32 MOVZ arm. If it does not, the tree that was
                // read is not the tree under test and every claim below is
                // about someone else's config.
                for (auto const& v : variants) {
                    if (isMovzArmAt(v, 32)) out.sawWidth32MovzInDoc = true;
                }
                edit(variants);
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

// Remove the width-32 MOVZ arm. Leaves the X arm width-KEYED, i.e. the shape
// the fix ships minus its new half.
[[nodiscard]] MovzMutant stripWidth32Movz() {
    return mutateMovVariants([](nlohmann::json& variants) {
        for (std::size_t i = 0; i < variants.size(); ++i) {
            if (isMovzArmAt(variants[i], 32)) {
                variants.erase(variants.begin() + static_cast<long>(i));
                return;
            }
        }
    });
}

// Reconstruct the PRE-FIX schema exactly: remove the width-32 MOVZ arm AND
// un-key the X arm back to width-absent.
[[nodiscard]] MovzMutant restorePreFixMovz() {
    return mutateMovVariants([](nlohmann::json& variants) {
        for (std::size_t i = 0; i < variants.size(); ++i) {
            if (isMovzArmAt(variants[i], 32)) {
                variants.erase(variants.begin() + static_cast<long>(i));
                break;
            }
        }
        for (auto& v : variants) {
            if (isMovzArmAt(v, 64)) v["guard"].erase("width");
        }
    });
}

} // namespace

// ★★ MUTANT A — remove the W arm, keep the X arm width-keyed. The witness must
// VANISH and the refusal must be LOUD: with no width-32 arm the width-32
// request matches nothing at all, so the shipped shape can never silently widen
// even if the W arm is deleted by a future edit.
TEST(Arm64MovzWForm, StrippingTheWFormArmRemovesItsWitnessWord) {
    auto const m = stripWidth32Movz();

    // 0 — the bytes under test reached this process.
    ASSERT_TRUE(m.sawWidth32MovzInDoc)
        << "the arm64 document this process READ does not declare a width-32 "
           "MOVZ arm — the config tree under test was not the tree read "
           "(run through ctest, which sets DSS_CONFIG_ROOT; a bare .exe walks "
           "the cwd)";
    // 1 — the mutation actually changed the document's CONTENT.
    EXPECT_TRUE(m.textDiffers)
        << "the mutant text is identical to the shipped text — the mutation "
           "did not take, so anything below would be a false red";
    // 3 — and it removed exactly one variant.
    EXPECT_EQ(m.variantsBefore, 6u)
        << "the shipped `mov` declares 2 reg arms + 2 MOVN arms + 2 MOVZ arms";
    EXPECT_EQ(m.variantsAfter, 5u)
        << "the mutation must remove exactly one variant";
    // 2 — the mutant is still a loadable schema.
    ASSERT_TRUE(m.parsed)
        << "the mutant must still PARSE, else the absence check below is "
           "vacuous:\n" << m.loadErrors;
    ASSERT_NE(m.schema, nullptr);
    EXPECT_EQ(movzArmsOf(*m.schema).size(), 1u);

    // 4 — every W witness is GONE, by the same matcher the positive pins use.
    auto const a = assembleMovzSource(m.schema);
    for (auto const w : {kMovzW0_5, kMovzW1_8, kMovzW3_65535}) {
        EXPECT_EQ(countWord(a.bytes, w), 0u)
            << "a W-form witness survived removal of the only variant that "
               "can encode it — the pin is not measuring what it claims";
    }
    EXPECT_FALSE(a.encodeOk)
        << "the mutant assembled cleanly — a missing width-keyed variant must "
           "fail loud, never widen silently";
    EXPECT_FALSE(a.diagnostics.empty()) << "a refusal must say something";

    // And the C path refuses too, rather than falling back to the X form.
    DiagnosticReporter rep;
    std::uint32_t const w =
        encodeConstMov(*m.schema, 5, kLirInstFlagWidth32, "x0", rep);
    EXPECT_NE(rep.errorCount(), 0u)
        << "a width-32 constant with no width-32 arm must fail loud";
    EXPECT_NE(w, kMovzX0_5)
        << "the C path silently widened to the X form — the exact silent "
           "widening this cycle's width keys exist to prevent";
}

// ★★★ MUTANT B — THE NON-VACUOUS HALF, and the reason Mutant A alone is not
// enough. Mutant A's output is EMPTY, so `countWord(...) == 0` is true for
// every conceivable word; the surrounding clauses are what stop that being
// meaningless. This mutant reconstructs the PRE-FIX schema instead (W arm gone,
// X arm width-ABSENT again), which still assembles — so the absence of
// 0x528000A0 is measured over REAL BYTES, and the word that appears in its
// place is the historical defect itself.
TEST(Arm64MovzWForm, PreFixSchemaReproducesTheOldBytesAndTheOldRefusal) {
    auto const m = restorePreFixMovz();

    ASSERT_TRUE(m.sawWidth32MovzInDoc)
        << "the arm64 document this process READ does not declare a width-32 "
           "MOVZ arm — wrong config tree (see Mutant A's note)";
    EXPECT_TRUE(m.textDiffers);
    EXPECT_EQ(m.variantsBefore, 6u);
    EXPECT_EQ(m.variantsAfter, 5u);
    ASSERT_TRUE(m.parsed)
        << "the pre-fix schema must still load — it is the schema that "
           "shipped until this cycle:\n" << m.loadErrors;
    ASSERT_NE(m.schema, nullptr);
    {
        auto const arms = movzArmsOf(*m.schema);
        ASSERT_EQ(arms.size(), 1u);
        EXPECT_EQ(arms[0].first, 0u) << "the surviving MOVZ arm is width-ABSENT";
    }

    // The C path: REAL BYTES, and they are the wrong ones — the width-absent
    // arm matches the width-32 request and encodes the 64-bit word with no
    // diagnostic. This is the silent behaviour the shipped schema replaces.
    DiagnosticReporter rep;
    std::uint32_t const w =
        encodeConstMov(*m.schema, 5, kLirInstFlagWidth32, "x0", rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "the pre-fix schema encoded this shape silently — that is the point";
    EXPECT_EQ(w, kMovzX0_5)
        << "the pre-fix schema must reproduce the OLD word; if it does not, "
           "this mutant is not the historical schema and proves nothing";
    EXPECT_NE(w, kMovzW0_5)
        << "the W witness must be ABSENT from the pre-fix output — measured "
           "over real bytes, not over an empty buffer";

    // The dialect: the ORIGINAL refusal, verbatim in kind. `mov w0, #5` elects
    // the width-absent arm, and `variantHonorsDeclaredWidth` refuses it.
    auto const a = assembleMovzSource(m.schema);
    EXPECT_FALSE(a.encodeOk)
        << "the pre-fix schema must still refuse `mov w0, #5` — that refusal "
           "is what D-ASM-ARM64-MOVZ-W-FORM-UNDECLARED named";
    EXPECT_NE(a.diagnostics.find("width"), std::string::npos)
        << "the refusal must be the WIDTH-honesty one, not some other error:\n"
        << a.diagnostics;
    EXPECT_EQ(countWord(a.bytes, kMovzW0_5), 0u);
}

// ★★★ AND THE SHAPE THIS CYCLE COULD NOT HAVE TAKEN, pinned so the next reader
// does not try it. Adding a width-32 arm while LEAVING the X arm width-absent
// looks like the smaller change and is REFUSED BY THE LOADER: `validate()`
// rejects a width-keyed guard mixed with a width-absent same-kind sibling,
// because first-match dispatch makes one of them shadow the other. That is why
// the X arm had to be keyed to 64 in the same edit — it is not a stylistic
// choice, it is the only loadable shape.
TEST(Arm64MovzWForm, AWidthAbsentMovzBesideAWidthKeyedOneIsRefusedAtLoad) {
    auto const m = mutateMovVariants([](nlohmann::json& variants) {
        for (auto& v : variants) {
            if (isMovzArmAt(v, 64)) v["guard"].erase("width");
        }
    });
    ASSERT_TRUE(m.sawWidth32MovzInDoc);
    EXPECT_TRUE(m.textDiffers);
    EXPECT_EQ(m.variantsBefore, m.variantsAfter)
        << "this mutant removes nothing — it only un-keys one guard";
    EXPECT_FALSE(m.parsed)
        << "a width-absent MOVZ arm beside a width-32 sibling LOADED — the "
           "ambiguity guard that forces both arms to be keyed is gone";
    EXPECT_NE(m.loadErrors.find("width-absent"), std::string::npos)
        << "the refusal must be the same-kind width-mix one:\n" << m.loadErrors;
}
