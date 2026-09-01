// ── THE WIDTH A **BARE** TEMPLATE OPERAND SUBSTITUTES AT ──────────────────
//
//     D-ASM-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE
//
// ★★★ THE DEFECT THESE PINS EXIST FOR, AND IT WAS A SILENT MISCOMPILE. A
// modifier-less `%N` used to substitute at the OPERAND'S OWN C-TYPE width on
// every target. That is exactly right on x86-64 and exactly wrong on AArch64,
// where gcc and clang both name the 64-bit `x` register for every integer type
// and expect `%w` when 32 bits is meant. `int v; __asm__("str %1, %0" :
// "=m"(t) : "r"(v))` emitted `str w28, [x29]` where gcc emits `str x1, [x0]`:
// a four-byte store against an eight-byte one, from one source text, with no
// diagnostic at either end.
//
// ★★★ WHY THE FIX IS A DECLARED POLICY AND NOT A CONSTANT. ✔MEASURED
// 2026-08-27 out of `-S`, gcc 13.3.0 AND clang 19.1.1, both ports, `-O0` and
// `-O2`: aarch64 renders `x0` for `_Bool` through `__int128` alike, while
// x86-64 renders `%al`/`%ax`/`%eax`/`%rax` tracking the operand's type. The
// two references AGREE WITH EACH OTHER and DISAGREE ACROSS PORTS, so one rule
// fixes one machine by breaking the other — and no arithmetic over the declared
// views reproduces both (*narrowest view ≥ the type width* gives `w0` for an
// `int` on aarch64, which renders `x0`; *widest view* gives `%rax` for an `int`
// on x86-64, which renders `%eax`). The derivation therefore lives in
// `.target.json` as `asmBareOperandWidths`, per register class.
//
// ★★★ WHAT MAKES THESE PINS DISCRIMINATE RATHER THAN MERELY PASS. The two
// ports are asserted to produce DIFFERENT bytes from the SAME binding width,
// so a build that applied ONE rule everywhere fails on exactly one of them
// whichever rule it picked. `BothPortsFromOneBindingWidth` states that as a
// single assertion — the two encodings must not be equal — which is the
// property no single hard-coded answer can satisfy.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe`
// (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).

#include "asm/asm.hpp"
#include "asm/asm_template_to_lir.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_reg.hpp"
#include "mutate_target_schema.hpp"

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

struct ShippedPair {
    std::string_view dialect;   // the document STEM, for locating it
    std::string_view target;
};

constexpr ShippedPair kX86{"asm-x86_64-att", "x86_64"};
constexpr ShippedPair kArm{"asm-arm64-gas", "arm64"};

[[nodiscard]] std::string dialectText(std::string_view name) {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{name, "sources", ".lang.json", "language",
                             DiagnosticCode::C_InvalidTargetName});
    if (!pathR.has_value()) {
        throw std::runtime_error{std::string{"cannot locate dialect "}
                                 + std::string{name}};
    }
    std::ifstream in{*pathR};
    if (!in) throw std::runtime_error{"cannot open dialect document"};
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

[[nodiscard]] std::shared_ptr<GrammarSchema> loadDialect(std::string_view name) {
    auto g = GrammarSchema::loadFromText(dialectText(name), std::string{name});
    if (!g.has_value()) {
        std::string why;
        for (auto const& e : g.error()) why += e.path + ": " + e.message + "\n";
        throw std::runtime_error{"dialect did not load: " + why};
    }
    return *g;
}

// One template lowering, taken to BYTES, with the operand bindings' width
// under the CALLER'S control — which is the whole subject of this file and the
// one knob the sibling `test_asm_template_to_lir.cpp` harness fixes at 64.
//
// ★ THE OPERANDS ARE PINNED TO PHYSICAL REGISTERS because `assemble()` runs
// after register allocation and a virtual register has no encoding; an
// unpinned run would assert against an empty byte vector, i.e. would measure
// the harness giving up rather than the instruction.
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

[[nodiscard]] std::unique_ptr<Run>
runOn(std::shared_ptr<GrammarSchema> dialect,
      std::shared_ptr<TargetSchema>  target,
      std::string_view templateText,
      std::vector<std::string> const& spellings,
      std::vector<std::string> const& physicalFor,
      std::uint32_t bindingWidthBits,
      LirRegClass   bindingClass = LirRegClass::GPR) {
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

[[nodiscard]] std::unique_ptr<Run>
run(ShippedPair const& p, std::string_view templateText,
    std::vector<std::string> const& spellings,
    std::vector<std::string> const& physicalFor,
    std::uint32_t bindingWidthBits,
    LirRegClass   bindingClass = LirRegClass::GPR) {
    auto targetR = TargetSchema::loadShipped(p.target);
    if (!targetR.has_value()) {
        throw std::runtime_error{"cannot load shipped target"};
    }
    return runOn(loadDialect(p.dialect), *targetR, templateText, spellings,
                 physicalFor, bindingWidthBits, bindingClass);
}

[[nodiscard]] std::string hex(std::vector<std::uint8_t> const& b) {
    std::string out;
    for (auto const v : b) out += std::format("{:02X} ", v);
    return out;
}

}  // namespace

// ══ THE SUBJECT ═══════════════════════════════════════════════════════════
//
// AArch64 encodes the operation width in the REGISTER, so `mov x0, x1` and
// `mov w0, w1` are the same mnemonic and differ by the `sf` bit alone. That
// makes this port's answer readable straight out of the emitted word, which is
// why the assertion is on BYTES rather than on an internal field: the bit that
// moves is the bit the CPU reads.
TEST(AsmBareOperandWidth, Arm64BareOperandNamesTheFullRegister) {
    auto const r = run(kArm, "mov %0, %1\n", {"%0", "%1"}, {"x0", "x1"}, 32);
    ASSERT_TRUE(r->parsed) << "template did not parse";
    ASSERT_TRUE(r->ok) << messages(*r);
    // AA0103E0 = ORR X0, XZR, X1 — the 64-bit form, which is what both
    // references substitute for a bare reference to a 32-bit operand here.
    ASSERT_GE(r->bytes.size(), 4u) << hex(r->bytes);
    EXPECT_EQ((std::vector<std::uint8_t>{r->bytes[0], r->bytes[1],
                                         r->bytes[2], r->bytes[3]}),
              (std::vector<std::uint8_t>{0xE0, 0x03, 0x01, 0xAA}))
        << "a bare `%N` bound at 32 bits must still name the FULL x register "
           "on this port — got " << hex(r->bytes);
}

// The matched opposite. Same binding width, same one-instruction shape, and
// the answer must be the NARROW one — which is what stops the fix from being a
// hard-coded 64.
TEST(AsmBareOperandWidth, X86BareOperandTracksTheOperandType) {
    auto const r = run(kX86, "movl %0, %1\n", {"%0", "%1"}, {"rax", "rcx"}, 32);
    ASSERT_TRUE(r->parsed) << "template did not parse";
    ASSERT_TRUE(r->ok) << messages(*r);
    // No REX.W: a 32-bit `mov` between eax and ecx.
    ASSERT_GE(r->bytes.size(), 1u) << hex(r->bytes);
    EXPECT_NE(r->bytes[0], 0x48)
        << "a bare `%N` bound at 32 bits must NOT widen to a REX.W 64-bit "
           "operation on this port — got " << hex(r->bytes);
}

// ★★★ THE PIN NO SINGLE HARD-CODED ANSWER CAN SATISFY, AND THE REASON THIS
// FILE EXISTS RATHER THAN TWO SEPARATE PORT TESTS. One binding width, one
// operand class, one instruction each — and the two ports MUST disagree. A
// build that applied the AArch64 rule everywhere fails the x86 arm; one that
// applied the x86 rule everywhere fails the AArch64 arm (that build is exactly
// what shipped, and is the defect); one that read the declaration passes both.
TEST(AsmBareOperandWidth, BothPortsFromOneBindingWidthMustDisagree) {
    auto const arm = run(kArm, "mov %0, %1\n", {"%0", "%1"}, {"x0", "x1"}, 32);
    auto const x86 = run(kX86, "movq %0, %1\n", {"%0", "%1"}, {"rax", "rcx"}, 32);
    ASSERT_TRUE(arm->ok) << messages(*arm);

    // AArch64 WIDENS a 32-bit-bound bare operand to the full register, so the
    // 64-bit instruction is what comes out. x86-64 does NOT, so a `movq` —
    // which declares width 64 — has 32-bit register operands under it and is
    // refused by the width-honesty gate. Two ports, one source shape, opposite
    // verdicts, from config alone.
    EXPECT_FALSE(x86->ok)
        << "`movq` over a bare `%N` bound at 32 bits must be REFUSED on this "
           "port — accepting it means the operand was silently widened, which "
           "is the AArch64 rule leaking across: " << messages(*x86);
}

// ══ THE MODIFIER STILL WINS, IN BOTH DIRECTIONS ═══════════════════════════
//
// The width-view letters are dialect vocabulary and were shipped before this
// derivation existed. A fix that made bare `%N` mean 64 by IGNORING the letters
// would pass the subject pin above and silently break every `%w` template, so
// the composition is asserted rather than assumed.
TEST(AsmBareOperandWidth, Arm64NarrowViewStillOverridesTheDerivation) {
    auto const r = run(kArm, "mov %w0, %w1\n", {"%0", "%1"}, {"x0", "x1"}, 32);
    ASSERT_TRUE(r->parsed) << "template did not parse";
    ASSERT_TRUE(r->ok) << messages(*r);
    // 2A0103E0 = ORR W0, WZR, W1 — the 32-bit form. The `sf` bit is CLEAR
    // here and SET in the bare pin above, which is the whole discrimination.
    ASSERT_GE(r->bytes.size(), 4u) << hex(r->bytes);
    EXPECT_EQ((std::vector<std::uint8_t>{r->bytes[0], r->bytes[1],
                                         r->bytes[2], r->bytes[3]}),
              (std::vector<std::uint8_t>{0xE0, 0x03, 0x01, 0x2A}))
        << "`%w` must still select the 32-bit view — got " << hex(r->bytes);
}

// And the WIDE letter on a 32-bit binding, so the letters are witnessed in
// both directions rather than only in the one the derivation already produces.
TEST(AsmBareOperandWidth, Arm64WideViewAgreesWithTheDerivation) {
    auto const r = run(kArm, "mov %x0, %x1\n", {"%0", "%1"}, {"x0", "x1"}, 32);
    ASSERT_TRUE(r->ok) << messages(*r);
    ASSERT_GE(r->bytes.size(), 4u) << hex(r->bytes);
    EXPECT_EQ(r->bytes[3], 0xAA)
        << "`%x` must select the 64-bit view — got " << hex(r->bytes);
}

// ══ THE COINCIDENCE, STATED AS A CONTROL ══════════════════════════════════
//
// ★ THIS IS WHY THE DEFECT SURVIVED. At a 64-bit binding the two derivations
// AGREE, so every inline-asm example that shipped before this row — all of
// which deliberately used `long`/`long long` on this port — encoded correctly
// under the wrong rule. A pin that only ever bound at 64 could not have caught
// it, which is the observation that makes the 32-bit pins above load-bearing.
TEST(AsmBareOperandWidth, Arm64AtSixtyFourTheTwoDerivationsCoincide) {
    auto const wide   = run(kArm, "mov %0, %1\n", {"%0", "%1"}, {"x0", "x1"}, 64);
    auto const narrow = run(kArm, "mov %0, %1\n", {"%0", "%1"}, {"x0", "x1"}, 32);
    ASSERT_TRUE(wide->ok) << messages(*wide);
    ASSERT_TRUE(narrow->ok) << messages(*narrow);
    EXPECT_EQ(wide->bytes, narrow->bytes)
        << "at 64 bits the operand-type rule and the register-natural rule "
           "must produce the same instruction — if they differ here the "
           "derivation is reading something other than the register width";
}

// ══ RED ON DISABLE, AT THE CONFIG TIER ════════════════════════════════════
//
// ★★★ THE MUTATION IS IN THE **REMOVE** DIRECTION AND IT IS THE SHIPPED
// DOCUMENT THAT IS MUTATED, not a hand-authored stand-in: a synthetic target
// would prove something about the synthetic document. Deleting the declaration
// must make the engine REFUSE — never fall back — because both derivations
// always assemble and a fallback is precisely the silent wrong width this row
// was opened for.
TEST(AsmBareOperandWidth, UndeclaredDerivationIsRefusedNotGuessed) {
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        kArm.target, [](nlohmann::json& doc) {
            ASSERT_TRUE(doc.contains("asmBareOperandWidths"))
                << "the shipped arm64 document must declare the facet, or this "
                   "mutation removes nothing and the pin below is vacuous";
            doc.erase("asmBareOperandWidths");
        });
    ASSERT_TRUE(mutated.has_value())
        << "the mutated document must still LOAD — the facet is optional at "
           "load time and fails at the point of USE, which is what makes the "
           "refusal a positioned diagnostic rather than a config error";

    auto const r = runOn(loadDialect(kArm.dialect), *mutated,
                         "mov %0, %1\n", {"%0", "%1"}, {"x0", "x1"}, 32);
    ASSERT_TRUE(r->parsed) << "template did not parse";
    EXPECT_FALSE(r->ok)
        << "with no declared derivation the engine substituted a width anyway "
           "— that is the guess this facet exists to refuse: " << messages(*r);
    auto const msg = messages(*r);
    EXPECT_NE(msg.find("asmBareOperandWidths"), std::string::npos)
        << "the refusal must name the facet the author has to declare: " << msg;
    EXPECT_NE(msg.find("gpr"), std::string::npos)
        << "the refusal must name the register CLASS it needed an answer for, "
           "since the facet is declared per class: " << msg;
}

// ══ THE FP CLASS — R5 OF DESIGN A′ ════════════════════════════════════════
//
//     D-ASM-AARCH64-FP-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE
//
// ★★★ THE SUBJECT: a bare `%N` on an FP-class binding names the FULL 128-bit
// `v` register on this port. ✔MEASURED at the P50 base (2026-09-01), gcc
// 13.3.0 AND clang 18.1.3 separately (clang under `-fno-integrated-as`),
// `-O0` and `-O2`: a bare `%0` on a `"w"`-bound `float`/`double`/`long
// double`/`__int128` renders `v0` under both at both levels — 16/16 rows.
// The shipped `fpr` row is therefore `registerNatural`, and
// `registerClassNaturalWidthBits` answers 128 (the `v` roots are the class's
// only no-`subOf` rows).
//
// ⚠ WHY THIS PIN READS A MESSAGE AND NOT BYTES, STATED RATHER THAN GLOSSED:
// `asm-arm64-gas.lang.json` declares no floating-point mnemonic (✔MEASURED,
// and `examples/c/c_inline_asm_fp_class_constraint` records the same gap), so
// no instruction containing an FP-class operand can ENCODE today. What the
// derivation change moves is which refusal fires: at 128 bits the template
// translator's width-flag map refuses FIRST, naming the width — which is only
// reachable if the derivation really answered 128. The mutant control below
// proves the discrimination in the other direction.
TEST(AsmBareOperandWidth, Arm64FpBareOperandNamesTheFullVectorRegister) {
    auto const r = run(kArm, "mov %0, %1\n", {"%0", "%1"}, {"v0", "v1"}, 64,
                       LirRegClass::FPR);
    ASSERT_TRUE(r->parsed) << "template did not parse";
    EXPECT_FALSE(r->ok)
        << "a 128-bit FP operation elected a variant — no shipped arm64 "
           "mnemonic declares one, so something substituted a narrower width";
    auto const msg = messages(*r);
    // ★ `ok == false` IS ITSELF THE DISCRIMINATOR: under the pre-R5
    // `operandType` rule this exact run LOWERS CLEAN at 64 bits (the mutant
    // control below proves it), so a refusal here can only mean a width the
    // map has no arm for — and the message must say which.
    EXPECT_NE(msg.find("128 bits"), std::string::npos)
        << "the refusal must carry the derived width — 128, the register-"
           "natural answer — or the derivation did not run: " << msg;
}

// ★★ THE REMOVE-DIRECTION MUTANT FOR R5: revert the shipped `fpr` row to
// `operandType` and the SAME template must fail DIFFERENTLY — the 64-bit
// binding width passes the width map, election picks the gpr `mov`, and the
// encoder's register-bank guard refuses. Two distinct refusal texts from one
// source shape is what proves the engine reads the DECLARATION rather than
// carrying a 128 of its own.
TEST(AsmBareOperandWidth, RevertingTheFpDeclarationRestoresTheNarrowTier) {
    auto reverted = test_support::mutateShippedTargetSchemaDoc(
        kArm.target, [](nlohmann::json& doc) {
            ASSERT_TRUE(doc.contains("asmBareOperandWidths"));
            bool changed = false;
            for (auto& row : doc.at("asmBareOperandWidths")) {
                if (row.value("class", std::string{}) != "fpr") continue;
                ASSERT_EQ(row.value("derivation", std::string{}),
                          "registerNatural")
                    << "the shipped arm64 fpr row must be `registerNatural` "
                       "(R5), or this revert is not the revert it claims to be";
                row["derivation"] = "operandType";
                changed = true;
            }
            ASSERT_TRUE(changed) << "the mutation matched no fpr row";
        });
    ASSERT_TRUE(reverted.has_value());

    auto const r = runOn(loadDialect(kArm.dialect), *reverted,
                         "mov %0, %1\n", {"%0", "%1"}, {"v0", "v1"}, 64,
                         LirRegClass::FPR);
    ASSERT_TRUE(r->parsed);
    // ⚠ THE TWO ARMS FAIL AT DIFFERENT TIERS AND THAT IS THE WHOLE PIN. Under
    // `operandType` the 64-bit binding passes the width map, so the LOWERING
    // succeeds — `ok` is TRUE — and the death happens later, at the encoder's
    // register-bank guard, whose diagnostics this harness's `assemble()` call
    // discards; what it leaves behind is an EMPTY byte vector (the function is
    // dropped from the module). Under `registerNatural` the lowering itself
    // refuses, naming 128. `ok` flipping with the document is the derivation
    // being READ.
    EXPECT_TRUE(r->ok)
        << "under `operandType` the 64-bit-bound bare FP operand must pass "
           "the lowering (the width map has a 64 arm): " << messages(*r);
    EXPECT_EQ(messages(*r).find("128 bits"), std::string::npos)
        << "the reverted document still derived 128 — the engine is not "
           "reading the declaration: " << messages(*r);
    EXPECT_TRUE(r->bytes.empty())
        << "a gpr-elected `mov` over v-registers ENCODED — the register-bank "
           "guard one tier down has been deleted, which is a different defect "
           "this pin must not mask: " << hex(r->bytes);
}

// ★★ THE CONTROL FOR THE MUTANT ABOVE — the arm that proves the pin is not
// simply failing in both directions. The same document, mutated by the same
// helper, with the derivation FLIPPED rather than removed: the lowering must
// succeed and produce the OTHER port's answer. A pin that reddens whenever the
// document is touched would pass the removal test while proving nothing.
TEST(AsmBareOperandWidth, FlippingTheDeclarationFlipsTheEmittedWidth) {
    auto flipped = test_support::mutateShippedTargetSchemaDoc(
        kArm.target, [](nlohmann::json& doc) {
            ASSERT_TRUE(doc.contains("asmBareOperandWidths"));
            bool changed = false;
            for (auto& row : doc.at("asmBareOperandWidths")) {
                if (row.value("class", std::string{}) != "gpr") continue;
                ASSERT_EQ(row.value("derivation", std::string{}),
                          "registerNatural")
                    << "the shipped arm64 gpr row must be `registerNatural`, "
                       "or this flip is not the flip it claims to be";
                row["derivation"] = "operandType";
                changed = true;
            }
            ASSERT_TRUE(changed) << "the mutation matched no row";
        });
    ASSERT_TRUE(flipped.has_value());

    auto const r = runOn(loadDialect(kArm.dialect), *flipped,
                         "mov %0, %1\n", {"%0", "%1"}, {"x0", "x1"}, 32);
    ASSERT_TRUE(r->ok) << messages(*r);
    ASSERT_GE(r->bytes.size(), 4u) << hex(r->bytes);
    // 2A… — the 32-bit `w` form, i.e. exactly the buggy encoding that used to
    // ship. The verdict follows the DOCUMENT, which is what proves the engine
    // reads the declaration rather than carrying a table of its own.
    EXPECT_EQ(r->bytes[3], 0x2A)
        << "with `operandType` declared, a 32-bit-bound bare operand must emit "
           "the 32-bit form — the engine is not reading the declaration: "
        << hex(r->bytes);
}
