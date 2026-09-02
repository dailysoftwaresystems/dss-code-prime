// THE SSE / SCALAR-FP INSTRUCTION ROWS OF THE AT&T DIALECT
// (D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME).
//
// ★★★ WHAT THESE PINS ARE FOR. `x86_64.target.json` binds the constraint letter
// `x` to the `fpr` register class, and until cycle P45 this dialect's
// `instructions[]` table spelled NONE of the target's eighteen FP/SSE opcodes —
// so `"x"` could be BOUND but no template could name an instruction that uses
// its operand. ✔MEASURED at the CLI before the rows landed: `__asm__("nop" :
// "=x"(r))` compiled rc=0 while `movsd`/`addsd` returned `A_AsmTextUnsupported
// … unknown mnemonic`.
//
// ★★★ THE FAILURE MODE THESE PINS EXIST TO CATCH IS A WIDTH, NOT AN ABSENCE.
// The `sd`/`ss` suffix pair is ONE target opcode under two width-keyed guards —
// F2 (scalar double, width 64) and F3 (scalar single, width 32) — sharing the
// same two opcode bytes. A row declared at the wrong width therefore still
// assembles and still runs; it computes on the wrong half of the register, with
// nothing to see in a build log. Every pin below reads the MANDATORY PREFIX,
// which is the only byte that separates the pair.
//
// ⚠ THE OPERANDS ARE PINNED TO PHYSICAL xmm REGISTERS. `assemble()` runs after
// register allocation and a virtual register has no encoding, so an unpinned
// run would assert against an empty byte vector — i.e. would measure the
// harness giving up rather than the instruction.

#include "asm/asm.hpp"
#include "asm/asm_template_to_lir.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_reg.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

constexpr std::string_view kDialect = "asm-x86_64-att";
constexpr std::string_view kTarget  = "x86_64";

[[nodiscard]] std::string dialectText() {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{kDialect, "sources", ".lang.json", "language",
                             DiagnosticCode::C_InvalidTargetName});
    if (!pathR.has_value()) {
        throw std::runtime_error{"cannot locate the shipped AT&T dialect"};
    }
    std::ifstream in{*pathR};
    if (!in) throw std::runtime_error{"cannot open the dialect document"};
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

[[nodiscard]] std::shared_ptr<GrammarSchema> loadDialect() {
    auto g = GrammarSchema::loadFromText(dialectText(), std::string{kDialect});
    if (!g.has_value()) {
        std::string why;
        for (auto const& e : g.error()) why += e.path + ": " + e.message + "\n";
        throw std::runtime_error{"dialect did not load: " + why};
    }
    return *g;
}

// ── THE CONFIG-TIER MUTATOR, OVER THE **SHIPPED** DOCUMENT ──────────────────
//
// ★★ IT READS THE SHIPPED FILE AND MUTATES THAT, never a hand-authored
// stand-in: a synthetic dialect would prove something about the synthetic
// document. The `.target.json` twin of this helper lives in
// `tests/test_support/mutate_target_schema.hpp`; the language-document side has
// no shared owner yet, and this file is its only consumer.
//
// ⚠ THE MUTATION MUST BE IN THE **REMOVE** DIRECTION. An ADD-direction mutant
// (declaring an extra row) stays green when the real config LOSES the feature,
// which is the direction that actually regresses.
[[nodiscard]] std::optional<std::shared_ptr<GrammarSchema>>
mutateShippedDialectDoc(std::function<void(nlohmann::json&)> const& edit) {
    auto doc = nlohmann::json::parse(dialectText());
    auto const before = doc.dump();
    edit(doc);
    if (doc.dump() == before) {
        throw std::runtime_error{
            "the mutation changed NOTHING — the pin below would be vacuous"};
    }
    auto g = GrammarSchema::loadFromText(doc.dump(), std::string{kDialect});
    if (!g.has_value()) return std::nullopt;
    return *g;
}

// Remove one `assembly.instructions[]` row by spelling. Refuses loudly if the
// spelling is absent, so a renamed row cannot silently make a pin vacuous.
[[nodiscard]] std::function<void(nlohmann::json&)>
removeInstructionRow(std::string spelling) {
    return [spelling](nlohmann::json& doc) {
        auto& rows = doc.at("assembly").at("instructions");
        std::size_t removed = 0;
        for (auto it = rows.begin(); it != rows.end();) {
            if (it->value("spelling", std::string{}) == spelling) {
                it = rows.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        if (removed != 1) {
            throw std::runtime_error{
                "expected exactly one `" + spelling
                + "` row in the shipped dialect, found "
                + std::to_string(removed)};
        }
    };
}

struct Run {
    std::shared_ptr<GrammarSchema> dialect;
    std::shared_ptr<TargetSchema>  target;
    DiagnosticReporter             reporter;
    bool                           parsed = false;
    bool                           ok     = false;
    std::vector<std::uint8_t>      bytes;
};

[[nodiscard]] std::string messages(Run const& r) {
    std::string out;
    for (auto const& d : r.reporter.all()) { out += d.actual; out += '\n'; }
    return out;
}

[[nodiscard]] std::string hex(std::vector<std::uint8_t> const& b) {
    std::string out;
    for (auto const v : b) out += std::format("{:02X} ", v);
    return out;
}

// One template lowering taken to BYTES, with every operand bound FPR-class at a
// caller-chosen width — the two knobs this file's subject turns.
[[nodiscard]] std::unique_ptr<Run>
runOn(std::shared_ptr<GrammarSchema> dialect,
      std::shared_ptr<TargetSchema>  target,
      std::string_view               templateText,
      std::vector<std::string> const& spellings,
      std::vector<std::string> const& physicalFor,
      std::uint32_t                   bindingWidthBits,
      LirRegClass bindingClass = LirRegClass::FPR) {
    auto run     = std::make_unique<Run>();
    run->dialect = std::move(dialect);
    run->target  = std::move(target);

    auto tree = parseAsmTemplateText(std::string{templateText}, "<template>",
                                     run->dialect,
                                     AsmTemplateSurface::Extended,
                                     DiagnosticBudget::libraryDefault(),
                                     run->reporter);
    run->parsed = tree.has_value();
    if (!run->parsed) return run;

    LirBuilder builder{*run->target};
    builder.addFunction(SymbolId{1});
    LirBlockId const entry = builder.createBlock();
    builder.beginBlock(entry);

    std::vector<AsmOperandBinding> bindings;
    for (std::size_t i = 0; i < spellings.size(); ++i) {
        AsmOperandBinding b;
        b.spelling  = spellings[i];
        b.regClass  = bindingClass;
        b.widthBits = bindingWidthBits;
        auto const ord = run->target->registerByName(physicalFor.at(i));
        if (!ord.has_value()) {
            throw std::runtime_error{"target declares no register "
                                     + physicalFor.at(i)};
        }
        b.reg = makePhysicalReg(*ord, bindingClass);
        bindings.push_back(std::move(b));
    }

    run->ok = lowerAsmTemplateToLirRun(*tree, *run->dialect, *run->target,
                                       bindings, builder, run->reporter);

    auto const retOp = run->target->opcodeByMnemonic("ret");
    if (!retOp.has_value()) throw std::runtime_error{"target has no `ret`"};
    builder.addReturn(*retOp, {});
    Lir lir = std::move(builder).finish();

    DiagnosticReporter     asmRep;
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto const mod = assemble(lir, *run->target, lirToMir, asmRep);
    if (mod.functions.size() == 1) run->bytes = mod.functions[0].bytes;
    return run;
}

[[nodiscard]] std::shared_ptr<TargetSchema> shippedTarget() {
    auto t = TargetSchema::loadShipped(kTarget);
    if (!t.has_value()) throw std::runtime_error{"cannot load shipped x86_64"};
    return *t;
}

// Two xmm operands, `%0` -> xmm0 and `%1` -> xmm1.
[[nodiscard]] std::unique_ptr<Run>
runXmm(std::string_view templateText, std::uint32_t widthBits) {
    return runOn(loadDialect(), shippedTarget(), templateText,
                 {"%0", "%1"}, {"xmm0", "xmm1"}, widthBits);
}

}  // namespace

// ══ THE SUBJECT: THE WIDTH-KEYED PREFIX ═══════════════════════════════════

// ADDSD xmm, xmm — F2 0F 58 /r. The F2 is the whole claim: it is what makes
// this the SCALAR DOUBLE variant rather than the scalar single one.
TEST(AsmX86SseDialectRows, AddsdElectsTheScalarDoubleVariant) {
    auto const r = runXmm("addsd %0, %1\n", 64);
    ASSERT_TRUE(r->parsed) << "template did not parse: " << messages(*r);
    ASSERT_TRUE(r->ok) << messages(*r);
    ASSERT_GE(r->bytes.size(), 3u) << hex(r->bytes);
    EXPECT_EQ(r->bytes[0], 0xF2) << "the scalar-DOUBLE mandatory prefix is the "
                                    "only byte separating addsd from addss: "
                                 << hex(r->bytes);
    EXPECT_EQ(r->bytes[1], 0x0F) << hex(r->bytes);
    EXPECT_EQ(r->bytes[2], 0x58) << hex(r->bytes);
}

// ADDSS xmm, xmm — F3 0F 58 /r. Same opcode bytes, different prefix.
TEST(AsmX86SseDialectRows, AddssElectsTheScalarSingleVariant) {
    auto const r = runXmm("addss %0, %1\n", 32);
    ASSERT_TRUE(r->parsed) << "template did not parse: " << messages(*r);
    ASSERT_TRUE(r->ok) << messages(*r);
    ASSERT_GE(r->bytes.size(), 3u) << hex(r->bytes);
    EXPECT_EQ(r->bytes[0], 0xF3) << hex(r->bytes);
    EXPECT_EQ(r->bytes[1], 0x0F) << hex(r->bytes);
    EXPECT_EQ(r->bytes[2], 0x58) << hex(r->bytes);
}

// ★★ THE DISCRIMINATOR, STATED AS A COMPARISON RATHER THAN AS TWO CONSTANTS.
// The two encodings must differ in EXACTLY the prefix byte — if a future edit
// made the width axis select something else as well, or nothing at all, this
// arm reddens where two independent constant pins would both still pass.
TEST(AsmX86SseDialectRows, TheSuffixPairDiffersOnlyInTheMandatoryPrefix) {
    auto const sd = runXmm("addsd %0, %1\n", 64);
    auto const ss = runXmm("addss %0, %1\n", 32);
    ASSERT_TRUE(sd->ok) << messages(*sd);
    ASSERT_TRUE(ss->ok) << messages(*ss);
    ASSERT_EQ(sd->bytes.size(), ss->bytes.size())
        << "sd=" << hex(sd->bytes) << " ss=" << hex(ss->bytes);
    ASSERT_GE(sd->bytes.size(), 1u);
    EXPECT_NE(sd->bytes[0], ss->bytes[0])
        << "the two widths produced the SAME prefix — one of the rows is "
           "declared at the wrong width: " << hex(sd->bytes);
    for (std::size_t i = 1; i < sd->bytes.size(); ++i) {
        EXPECT_EQ(sd->bytes[i], ss->bytes[i])
            << "byte " << i << " differs beyond the prefix: sd="
            << hex(sd->bytes) << " ss=" << hex(ss->bytes);
    }
}

// ★★★ THE ASYMMETRY THAT A TABLE BUILT BY PATTERN WOULD HAVE GOT WRONG.
// Every arithmetic row above follows F2 (double) / F3 (single). The COMPARES do
// not: ✔MEASURED against GNU as 2.42, `ucomisd` is 66 0F 2E and `ucomiss` is
// 0F 2E with NO mandatory prefix at all. An author matching the arithmetic
// pattern would have given `ucomiss` an F3 and encoded a different instruction.
TEST(AsmX86SseDialectRows, UcomissHasNoMandatoryPrefixWhereUcomisdHasSixtySix) {
    auto const sd = runXmm("ucomisd %0, %1\n", 64);
    ASSERT_TRUE(sd->ok) << messages(*sd);
    ASSERT_GE(sd->bytes.size(), 3u) << hex(sd->bytes);
    EXPECT_EQ(sd->bytes[0], 0x66) << hex(sd->bytes);
    EXPECT_EQ(sd->bytes[1], 0x0F) << hex(sd->bytes);
    EXPECT_EQ(sd->bytes[2], 0x2E) << hex(sd->bytes);

    auto const ss = runXmm("ucomiss %0, %1\n", 32);
    ASSERT_TRUE(ss->ok) << messages(*ss);
    ASSERT_GE(ss->bytes.size(), 2u) << hex(ss->bytes);
    EXPECT_EQ(ss->bytes[0], 0x0F)
        << "ucomiss must carry NO mandatory prefix — an F3 here would be a "
           "different instruction: " << hex(ss->bytes);
    EXPECT_EQ(ss->bytes[1], 0x2E) << hex(ss->bytes);
}

// MOVAPS xmm, xmm — 0F 28 /r, no prefix. The one row in the block that declares
// no `width`, because its encoding guard declares none either.
TEST(AsmX86SseDialectRows, MovapsIsThePrefixlessRegisterMove) {
    auto const r = runXmm("movaps %0, %1\n", 64);
    ASSERT_TRUE(r->parsed) << messages(*r);
    ASSERT_TRUE(r->ok) << messages(*r);
    ASSERT_GE(r->bytes.size(), 2u) << hex(r->bytes);
    EXPECT_EQ(r->bytes[0], 0x0F) << hex(r->bytes);
    EXPECT_EQ(r->bytes[1], 0x28) << hex(r->bytes);
}

// ══ THE REGISTER-TO-REGISTER SCALAR MOVE ══════════════════════════════════
//
// ★★★ `movsd_reg` IS A SEPARATE OPCODE AND THIS PIN IS WHAT SEPARATES IT FROM
// ITS NEIGHBOUR. Until it was declared, `movsd %xmm1, %xmm0` — which GNU as
// accepts — was refused here, because `movsd_load`/`movsd_store` guard on
// MEMORY operand shapes only. The only reachable register move was `movaps`,
// and the two are NOT interchangeable: ✔MEASURED by execution under gcc 13.3.0
// with two doubles packed in one xmm, `movsd` leaves the destination's HIGH
// LANE unchanged (111) while `movaps` CLOBBERS it (222). A binding that reached
// the register move through `movaps` would silently destroy the upper half of
// every destination while producing correct-looking low-lane results.
//
// So this arm asserts the F2/F3 0F 10 form specifically, and would redden on
// 0F 28 — which is the exact substitution the near-miss list below forbids.
TEST(AsmX86SseDialectRows, MovsdRegisterFormIsItsOwnOpcodeNotMovaps) {
    auto const sd = runXmm("movsd %0, %1\n", 64);
    ASSERT_TRUE(sd->parsed) << messages(*sd);
    ASSERT_TRUE(sd->ok)
        << "the register-to-register `movsd` must lower — `movsd_reg` is "
           "declared: " << messages(*sd);
    ASSERT_GE(sd->bytes.size(), 3u) << hex(sd->bytes);
    EXPECT_EQ(sd->bytes[0], 0xF2) << hex(sd->bytes);
    EXPECT_EQ(sd->bytes[1], 0x0F) << hex(sd->bytes);
    EXPECT_EQ(sd->bytes[2], 0x10)
        << "0x28 here would be MOVAPS — a full-register copy that clobbers the "
           "destination's high lane, which MOVSD leaves alone: " << hex(sd->bytes);

    auto const ss = runXmm("movss %0, %1\n", 32);
    ASSERT_TRUE(ss->ok) << messages(*ss);
    ASSERT_GE(ss->bytes.size(), 3u) << hex(ss->bytes);
    EXPECT_EQ(ss->bytes[0], 0xF3) << hex(ss->bytes);
    EXPECT_EQ(ss->bytes[1], 0x0F) << hex(ss->bytes);
    EXPECT_EQ(ss->bytes[2], 0x10) << hex(ss->bytes);
}

// ★★ THE DISCRIMINATOR STATED AS A COMPARISON: `movsd` and `movaps` are both
// register-to-register FP moves of the same operands, and they must NOT produce
// the same bytes. Two independent constant pins would each still pass if one
// spelling were silently re-bound to the other's opcode; this arm would not.
TEST(AsmX86SseDialectRows, MovsdAndMovapsDoNotEncodeTheSame) {
    auto const sd = runXmm("movsd %0, %1\n", 64);
    auto const ap = runXmm("movaps %0, %1\n", 64);
    ASSERT_TRUE(sd->ok) << messages(*sd);
    ASSERT_TRUE(ap->ok) << messages(*ap);
    EXPECT_NE(sd->bytes, ap->bytes)
        << "`movsd` and `movaps` encoded identically — one has been bound to "
           "the other's opcode, and the high-lane merge semantics of MOVSD are "
           "silently gone: " << hex(sd->bytes);
}

// ⚠ THE MEMORY FORMS MUST SURVIVE THE NEW REGISTER FORM. `movsd` now names
// THREE opcodes and the target's guards choose between them by operand-list
// length; if the [reg] guard ever shadowed the memory shapes, a load would
// silently become a register copy. This arm is the anti-shadow control.
TEST(AsmX86SseDialectRows, TheRegisterFormDoesNotShadowTheMemoryForms) {
    auto const doc = nlohmann::json::parse(dialectText());
    for (auto const& row : doc.at("assembly").at("instructions")) {
        if (row.value("spelling", std::string{}) != "movsd") continue;
        auto const ops = row.at("opcodes");
        EXPECT_EQ(ops.size(), 3u)
            << "`movsd` must name the load, the store AND the register form";
        bool load = false, store = false, reg = false;
        for (auto const& o : ops) {
            if (o == "movsd_load")  load  = true;
            if (o == "movsd_store") store = true;
            if (o == "movsd_reg")   reg   = true;
        }
        EXPECT_TRUE(load && store && reg)
            << "the three shapes of `movsd` are not all reachable";
    }
}

// ══ THE NEAR-MISS RULE ════════════════════════════════════════════════════
//
// ★★★ GNU as ACCEPTS ALL SEVEN OF THESE AND THE TARGET DECLARES NO TEMPLATE
// THAT EMITS THEIR BYTES, so they are deliberately UNDECLARED. Binding any of
// them to the nearest existing opcode would emit different bytes under the same
// source text — `movapd` on `movaps` drops the 0x66 and silently becomes a
// packed-SINGLE move. That is the silent wrong instruction this table exists to
// prevent, so the refusal is the correct behaviour and is pinned as such.
//
// ⚠ THE LIST IS THE FULL SEVEN, not a sample. A partial list would let a future
// edit quietly bind one of the unlisted ones to a neighbour.
TEST(AsmX86SseDialectRows, NearMissSpellingsRefuseRatherThanBindToTheNeighbour) {
    for (auto const* spelling : {"movapd", "movups", "comisd", "comiss",
                                 "cvtsi2ss", "xorpd", "xorps"}) {
        auto const r = runXmm(std::string{spelling} + " %0, %1\n", 64);
        EXPECT_FALSE(r->ok)
            << spelling << " lowered, but the target declares no encoding for "
                           "it — it must have bound to a NEIGHBOURING opcode "
                           "and is now emitting different bytes than the "
                           "source text names: " << hex(r->bytes);
    }
}

// ══ RED ON DISABLE, AT THE CONFIG TIER ════════════════════════════════════

// ★★★ THE MUTANT: remove the `addsd` row from the SHIPPED document. The
// template must then be refused — the engine must not fall back to a
// similarly-named target opcode, which is exactly what the `A_AsmTextUnsupported`
// refusal text promises it will never do.
TEST(AsmX86SseDialectRows, RemovingTheShippedAddsdRowRefusesTheTemplate) {
    auto mutated = mutateShippedDialectDoc(removeInstructionRow("addsd"));
    ASSERT_TRUE(mutated.has_value())
        << "the mutated dialect must still LOAD — removing one instruction row "
           "is a smaller vocabulary, not a malformed document";

    auto const r = runOn(*mutated, shippedTarget(), "addsd %0, %1\n",
                         {"%0", "%1"}, {"xmm0", "xmm1"}, 64);
    EXPECT_FALSE(r->ok)
        << "with no `addsd` row the template lowered anyway — the engine is "
           "guessing an opcode from the spelling rather than reading this "
           "table: " << hex(r->bytes);
    auto const msg = messages(*r);
    EXPECT_NE(msg.find("addsd"), std::string::npos)
        << "the refusal must quote the spelling it could not resolve: " << msg;
}

// ★★ THE CONTROL FOR THE MUTANT ABOVE — the arm that proves the pin is not
// simply reddening whenever the document is touched. The same helper removes a
// DIFFERENT row from the same block, and `addsd` must still lower and still
// emit the F2 form. Without this arm, a mutant that broke the document wholesale
// would pass the removal test while proving nothing about `addsd` in particular.
TEST(AsmX86SseDialectRows, RemovingADifferentRowLeavesAddsdWorking) {
    auto mutated = mutateShippedDialectDoc(removeInstructionRow("divsd"));
    ASSERT_TRUE(mutated.has_value());

    auto const r = runOn(*mutated, shippedTarget(), "addsd %0, %1\n",
                         {"%0", "%1"}, {"xmm0", "xmm1"}, 64);
    ASSERT_TRUE(r->ok)
        << "removing `divsd` broke `addsd` — the mutation is not surgical and "
           "the removal pin above proves nothing: " << messages(*r);
    ASSERT_GE(r->bytes.size(), 3u) << hex(r->bytes);
    EXPECT_EQ(r->bytes[0], 0xF2) << hex(r->bytes);

    // …and the row that WAS removed must now refuse, which is what makes this
    // arm a control rather than a second copy of the happy path.
    auto const gone = runOn(*mutated, shippedTarget(), "divsd %0, %1\n",
                            {"%0", "%1"}, {"xmm0", "xmm1"}, 64);
    EXPECT_FALSE(gone->ok)
        << "`divsd` still lowered after its row was removed: "
        << hex(gone->bytes);
}

// ⚠ THE ANTI-VACUITY ARM. Every pin above would pass trivially against a
// dialect that had never declared these rows if the harness silently stopped
// resolving them. This asserts the shipped document really carries the block,
// so a future edit that deletes it reddens HERE with a sentence naming the
// cause rather than only through a byte comparison.
TEST(AsmX86SseDialectRows, TheShippedDialectActuallyDeclaresTheSseBlock) {
    auto const doc = nlohmann::json::parse(dialectText());
    auto const& rows = doc.at("assembly").at("instructions");
    std::vector<std::string> want{"addsd",   "addss",     "subsd",  "subss",
                                  "mulsd",   "mulss",     "divsd",  "divss",
                                  "ucomisd", "ucomiss",   "movaps", "movsd",
                                  "movss",   "cvtsd2ss",  "cvtss2sd",
                                  "cvttsd2si", "cvttss2si",
                                  "cvtsi2sdq", "cvtsi2sdl"};
    for (auto const& w : want) {
        bool found = false;
        for (auto const& row : rows) {
            if (row.value("spelling", std::string{}) == w) { found = true; break; }
        }
        EXPECT_TRUE(found)
            << "the shipped AT&T dialect no longer declares `" << w
            << "` — the `x` constraint has lost part of the vocabulary that "
               "made it usable (D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-"
               "INSTRUCTION-CAN-NAME)";
    }
}

// ★★ THE `destWidth` PIN. The four width-CHANGING conversions are refused by
// the width-honesty gate when declared with `width` alone — ✔MEASURED,
// `cvtsd2ss` gives `register '%1' is 64 bits wide but register '%0' is 32 bits`.
// They carry `width` + `destWidth`, the same pair the integer `movslq`/`movzbl`
// rows have used since 2026-08-23. This arm asserts the shipped rows still
// carry it, because losing the key turns a working conversion into a refusal.
TEST(AsmX86SseDialectRows, WidthChangingConversionsDeclareDestWidth) {
    auto const doc = nlohmann::json::parse(dialectText());
    auto const& rows = doc.at("assembly").at("instructions");
    std::vector<std::string> const changing{"cvtsd2ss", "cvtss2sd",
                                            "cvttss2si", "cvtsi2sdl"};
    std::vector<std::string> const same{"cvttsd2si", "cvtsi2sdq"};
    for (auto const& row : rows) {
        auto const sp = row.value("spelling", std::string{});
        for (auto const& c : changing) {
            if (sp != c) continue;
            EXPECT_TRUE(row.contains("destWidth"))
                << sp << " changes width between its source and destination "
                         "and must declare `destWidth`, or the width-honesty "
                         "gate refuses it";
        }
        for (auto const& s : same) {
            if (sp != s) continue;
            EXPECT_FALSE(row.contains("destWidth"))
                << s << " has the SAME width on both sides — a `destWidth` "
                        "here would state a difference that does not exist";
        }
    }
}
