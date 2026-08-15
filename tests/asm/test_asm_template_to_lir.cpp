// Plan 29 P5 — the EMBEDDED assembly template path (`lowerAsmTemplateToLirRun`).
//
// ★★★ WHAT THIS FILE IS FOR, AND WHY IT DRIVES THE SHIPPED DIALECTS UNMUTATED.
// The claim under test is *"an embedded `__asm__` template reaches bytes through
// the SAME text→LIR engine the standalone `.s` path uses"*. A test that
// hand-authored a stub dialect would prove that claim about the stub — the pin
// would be green with the shipped `asm-x86_64-att.lang.json` broken, missing the
// row, or naming an opcode the target does not declare. So every test below
// loads the SHIPPED dialect document and the SHIPPED target, exactly as the
// compiler does (`asm_text_fixture.hpp`'s posture, and the same anti-rot reason
// `mutate_target_schema.hpp` states for targets).
//
// ⚠ WHAT IS AND IS NOT PINNED HERE ABOUT `%N`. The `%0` SPELLING is a dialect
// token/rule owned by a concurrent lane and is not on disk yet. What this file
// pins is the half the ENGINE owns and the half that makes the spelling work at
// all: a register-position operand resolves through the caller's
// `AsmOperandBinding` table to a caller-supplied VREG, and an operand the caller
// did not bind fails LOUD naming both the template's bound set and the target.
// Those tests bind spellings the shipped grammars already parse
// (`%vreg0` under AT&T's `attRegister`), so they exercise the real mechanism
// today and need no edit when the `%N` rule lands — only an additional spelling.

#include "asm/asm.hpp"
#include "asm/asm_template_to_lir.hpp"
#include "asm/asm_text_to_lir.hpp"
#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// The two shipped dialects, each with the target it is written for. Every test
// that can run on both runs on both — a mechanism that works on one CPU and not
// the other is the failure mode the second dialect exists to catch.
struct ShippedPair {
    std::string_view dialect;         // the document STEM, for locating it
    std::string_view target;
    std::string_view functionMarker;  // this dialect's `.type` marker
};

constexpr ShippedPair kX86{"asm-x86_64-att", "x86_64", "@function"};
constexpr ShippedPair kArm{"asm-arm64-gas", "arm64", "%function"};

// The shipped dialect document's TEXT. ⚠ CONFIG-LEVEL: `dss_add_test` sets
// `DSS_CONFIG_ROOT`, so this file must run through ctest and never as a bare
// `.exe` (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).
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

// ★★★ THE RED-ON-DISABLE DOOR FOR THIS FILE. The shipped document is read from
// disk, ONE key is edited in memory, and the result is loaded as a grammar —
// the `mutate_target_schema.hpp` posture, applied to a dialect. A test that
// hand-authored the mutant instead would prove nothing about the shipped
// document; mutating the real one is what makes "turn the fold off and the
// uppercase form must be refused" a claim about what actually ships.
struct MutatedDialect {
    std::shared_ptr<GrammarSchema> grammar;   // null ⇒ the document was refused
    std::vector<std::string>       loadErrors;
};

[[nodiscard]] MutatedDialect
loadDialectMutated(std::string_view name,
                   std::function<void(nlohmann::json&)> const& mutate) {
    auto doc = nlohmann::json::parse(dialectText(name));
    mutate(doc);
    MutatedDialect out;
    auto g = GrammarSchema::loadFromText(doc.dump(), std::string{name});
    if (g.has_value()) {
        out.grammar = *g;
    } else {
        for (auto const& e : g.error()) {
            out.loadErrors.push_back(e.path + ": " + e.message);
        }
    }
    return out;
}

[[nodiscard]] std::string joined(std::vector<std::string> const& v) {
    std::string out;
    for (auto const& s : v) { out += s; out += '\n'; }
    return out;
}

// One template lowering, taken all the way to BYTES.
//
// ★ THE TRAILING `ret` IS NOT DECORATION AND IS NOT NOISE IN THE ASSERTIONS.
// `LirBuilder::finish()` aborts the process on an unterminated block, and a
// template is emitted MID FUNCTION — it never carries its own terminator. So
// the harness closes the block exactly as the embedding language will, and the
// byte assertions subtract the terminator by comparing against a run of the
// EMPTY template through the identical path (`baselineBytes`).
struct TemplateRun {
    std::shared_ptr<GrammarSchema>   dialect;
    std::shared_ptr<TargetSchema>    target;
    std::unique_ptr<CompilationUnit> unit;
    DiagnosticReporter               reporter;
    bool                             parsed = false;
    bool                             ok     = false;
    Lir                              lir;
    std::vector<std::uint8_t>        bytes;
    std::vector<std::uint16_t>       opcodes;   // the emitted block, in order
};

[[nodiscard]] std::string messages(TemplateRun const& r) {
    std::string out;
    for (auto const& d : r.reporter.all()) { out += d.actual; out += '\n'; }
    return out;
}

// `bindings` are positional (GNU order: outputs then inputs); each names the
// DIALECT SPELLING the template writes for it. `vregClasses` mints one vreg per
// binding in the caller's builder — which is the whole point of the exercise:
// the register the template names must be the caller's, not the target's.
[[nodiscard]] std::unique_ptr<TemplateRun>
runTemplateWith(std::shared_ptr<GrammarSchema> dialect, ShippedPair const& p,
                std::string_view                templateText,
                std::vector<std::string> const& bindingSpellings) {
    auto run     = std::make_unique<TemplateRun>();
    run->dialect = std::move(dialect);
    auto targetR = TargetSchema::loadShipped(p.target);
    if (!targetR.has_value()) {
        throw std::runtime_error{"cannot load shipped target"};
    }
    run->target = *targetR;

    UnitBuilder ub{run->dialect, DiagnosticBudget::libraryDefault()};
    ub.addInMemory(std::string{templateText}, "<template>.s");
    run->unit = std::make_unique<CompilationUnit>(std::move(ub).finish());
    if (run->unit->trees().empty()) {
        throw std::runtime_error{"template produced no tree"};
    }
    run->parsed = true;
    for (auto const& d : run->unit->trees()[0].diagnostics().all()) {
        if (d.severity == DiagnosticSeverity::Error) run->parsed = false;
    }

    LirBuilder builder{*run->target};
    builder.addFunction(SymbolId{1});
    LirBlockId const entry = builder.createBlock();
    builder.beginBlock(entry);

    std::vector<AsmOperandBinding> bindings;
    for (auto const& spelling : bindingSpellings) {
        AsmOperandBinding b;
        b.spelling  = spelling;
        b.regClass  = LirRegClass::GPR;
        b.reg       = builder.newVReg(LirRegClass::GPR);
        b.widthBits = 64;
        bindings.push_back(std::move(b));
    }

    run->ok = lowerAsmTemplateToLirRun(run->unit->trees()[0], *run->dialect,
                                       *run->target, bindings, builder,
                                       run->reporter);

    // Close the block the way the embedding language will. ⚠ NO TEMPLATE IN
    // THIS FILE CONTAINS A TERMINATOR — `LirBuilder` seals a block on one and
    // would abort the process on a second, so adding a terminator here is only
    // sound while that holds. A future test that writes `ret` inside a template
    // must close its own block instead of reaching this line.
    auto const retOp = run->target->opcodeByMnemonic("ret");
    if (!retOp.has_value()) throw std::runtime_error{"target has no `ret`"};
    builder.addReturn(*retOp, {});
    run->lir = std::move(builder).finish();

    auto const blk = run->lir.funcBlockAt(run->lir.funcAt(0), 0);
    for (std::size_t i = 0; i < run->lir.blockInstCount(blk); ++i) {
        run->opcodes.push_back(
            run->lir.instOpcode(run->lir.blockInstAt(blk, i)));
    }

    DiagnosticReporter     asmRep;
    std::vector<MirInstId> lirToMir(run->lir.instCount());
    auto const mod = assemble(run->lir, *run->target, lirToMir, asmRep);
    if (mod.functions.size() == 1) run->bytes = mod.functions[0].bytes;
    return run;
}

[[nodiscard]] std::unique_ptr<TemplateRun>
runTemplate(ShippedPair const& p, std::string_view templateText,
            std::vector<std::string> const& bindingSpellings) {
    return runTemplateWith(loadDialect(p.dialect), p, templateText,
                           bindingSpellings);
}

// The bytes an EMPTY template produces — i.e. the terminator alone. Every byte
// assertion below is `expected ++ baseline`, so the pin is exact rather than a
// prefix match: a stray extra instruction after the subject would still fail.
[[nodiscard]] std::vector<std::uint8_t> baselineBytes(ShippedPair const& p) {
    auto const run = runTemplate(p, "", {});
    EXPECT_TRUE(run->ok) << messages(*run);
    EXPECT_EQ(run->opcodes.size(), 1u) << "the baseline must be `ret` alone";
    return run->bytes;
}

[[nodiscard]] std::vector<std::uint8_t>
withTerminator(std::vector<std::uint8_t> head,
               std::vector<std::uint8_t> const& tail) {
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}

[[nodiscard]] std::string hex(std::span<std::uint8_t const> b) {
    std::string out;
    for (auto const v : b) out += std::format("{:02X} ", v);
    return out;
}

// ★ THE DIALECT NAME IS READ OFF THE LOADED GRAMMAR, NEVER TYPED IN. The
// document's `language` name (`AsmX86_64Att`) is not its file stem
// (`asm-x86_64-att`), and hard-coding either would make this a pin on my memory
// rather than on the diagnostic — and it would rot silently the day a document
// is renamed. Same for the target.
void expectRefusalNamesThePair(TemplateRun const& run, std::string_view what) {
    auto const msg = messages(run);
    EXPECT_NE(msg.find(what), std::string::npos)
        << "the refusal must name what it refused: " << msg;
    EXPECT_NE(msg.find(run.dialect->name()), std::string::npos)
        << "the refusal must name the DIALECT ('" << run.dialect->name()
        << "'): " << msg;
    EXPECT_NE(msg.find(run.target->name()), std::string::npos)
        << "the refusal must name the TARGET ('" << run.target->name()
        << "'): " << msg;
}

[[nodiscard]] std::uint16_t opcodeOf(TargetSchema const& t,
                                     std::string_view name) {
    auto const o = t.opcodeByMnemonic(name);
    EXPECT_TRUE(o.has_value()) << "target declares no opcode '" << name << "'";
    return o.value_or(0);
}

} // namespace

// ── the bytes ─────────────────────────────────────────────────────────────
//
// ★★ THE ASSERTION IS THE ENCODED BYTES, NOT "IT LOWERED". A template that
// reached the right OPCODE and the wrong ENCODING VARIANT is exactly the
// silent-miscompile class this whole subsystem is built to refuse (the measured
// arm64 `mov w0,w1`-encodes-64-bit case), and an opcode-level assertion cannot
// see it. Both words below are the reference assembler's, recorded in the
// target documents' own `$comment`s.

TEST(AsmTemplateToLir, X86NopIsOneNinetyByte) {
    auto const tail = baselineBytes(kX86);
    auto const run  = runTemplate(kX86, "nop\n", {});
    ASSERT_TRUE(run->parsed) << "template did not parse";
    ASSERT_TRUE(run->ok) << messages(*run);
    EXPECT_EQ(run->opcodes.size(), 2u)
        << "a `nop` must add exactly ONE instruction before the terminator";
    EXPECT_EQ(run->opcodes.front(), opcodeOf(*run->target, "nop"));
    EXPECT_EQ(run->bytes, withTerminator({0x90}, tail))
        << "got " << hex(run->bytes);
}

TEST(AsmTemplateToLir, X86RdtscIsZeroFThirtyOne) {
    auto const tail = baselineBytes(kX86);
    auto const run  = runTemplate(kX86, "rdtsc\n", {});
    ASSERT_TRUE(run->parsed) << "template did not parse";
    ASSERT_TRUE(run->ok) << messages(*run);
    EXPECT_EQ(run->opcodes.size(), 2u);
    EXPECT_EQ(run->opcodes.front(), opcodeOf(*run->target, "rdtsc"));
    EXPECT_EQ(run->bytes, withTerminator({0x0F, 0x31}, tail))
        << "got " << hex(run->bytes);
}

TEST(AsmTemplateToLir, Arm64NopIsTheHintZeroWord) {
    auto const tail = baselineBytes(kArm);
    auto const run  = runTemplate(kArm, "nop\n", {});
    ASSERT_TRUE(run->parsed) << "template did not parse";
    ASSERT_TRUE(run->ok) << messages(*run);
    EXPECT_EQ(run->opcodes.size(), 2u);
    EXPECT_EQ(run->opcodes.front(), opcodeOf(*run->target, "nop"));
    // 0xD503201F, little-endian — the canonical HINT #0.
    EXPECT_EQ(run->bytes, withTerminator({0x1F, 0x20, 0x03, 0xD5}, tail))
        << "got " << hex(run->bytes);
}

// ★ THE SAME SPELLING, TWO TARGETS, TWO DIFFERENT WORDS — the one assertion
// that shows the mapping is CONFIG rather than code. If `nop` were wired in
// C++, this pair could not both hold.
TEST(AsmTemplateToLir, OneSpellingTwoTargetsTwoEncodings) {
    auto const x86 = runTemplate(kX86, "nop\n", {});
    auto const arm = runTemplate(kArm, "nop\n", {});
    ASSERT_TRUE(x86->ok) << messages(*x86);
    ASSERT_TRUE(arm->ok) << messages(*arm);
    ASSERT_GE(x86->bytes.size(), 1u);
    ASSERT_GE(arm->bytes.size(), 4u);
    EXPECT_NE(std::vector<std::uint8_t>(x86->bytes.begin(),
                                        x86->bytes.begin() + 1),
              std::vector<std::uint8_t>(arm->bytes.begin(),
                                        arm->bytes.begin() + 1));
}

// ── the multi-line template ───────────────────────────────────────────────

TEST(AsmTemplateToLir, MultiLineTemplateEmitsEveryLineInOrder) {
    auto const tail = baselineBytes(kX86);
    auto const run  = runTemplate(kX86, "nop\nrdtsc\nnop\n", {});
    ASSERT_TRUE(run->parsed) << "template did not parse";
    ASSERT_TRUE(run->ok) << messages(*run);

    auto const nop   = opcodeOf(*run->target, "nop");
    auto const rdtsc = opcodeOf(*run->target, "rdtsc");
    // ★ THE SEQUENCE, NOT THE COUNT. Three instructions in the wrong order is
    // a different program, and a size-only assertion cannot tell them apart.
    EXPECT_EQ(run->opcodes,
              (std::vector<std::uint16_t>{nop, rdtsc, nop,
                                          opcodeOf(*run->target, "ret")}));
    EXPECT_EQ(run->bytes,
              withTerminator({0x90, 0x0F, 0x31, 0x90}, tail))
        << "got " << hex(run->bytes);
}

// ── the operand binding: a register position resolves to the caller's VREG ──

TEST(AsmTemplateToLir, ABoundOperandBecomesTheCallersVreg) {
    // `%vreg0` parses as AT&T's ordinary `attRegister` (RegisterSigil +
    // Identifier); the target declares no register by that name, so the ONLY
    // way it can resolve is through the binding table. That is the mechanism
    // `%0` will use once the dialect declares the spelling.
    auto const run = runTemplate(kX86, "movq %vreg1, %vreg0\n",
                                 {"vreg0", "vreg1"});
    ASSERT_TRUE(run->parsed) << "template did not parse";
    ASSERT_TRUE(run->ok) << messages(*run);
    ASSERT_EQ(run->opcodes.size(), 2u);

    auto const blk  = run->lir.funcBlockAt(run->lir.funcAt(0), 0);
    auto const inst = run->lir.blockInstAt(blk, 0);
    EXPECT_EQ(run->lir.instOpcode(inst), opcodeOf(*run->target, "mov"));

    // ★★ THE STRONGEST PROVABLE PROPERTY: both the RESULT and the SOURCE are
    // VIRTUAL registers, and they are the two the caller minted — in the order
    // the bindings declared, with AT&T's destination-LAST operand order
    // honoured (`movq %vreg1, %vreg0` writes vreg0).
    LirReg const result = run->lir.instResult(inst);
    EXPECT_EQ(result.isPhysical, 0u)
        << "a bound operand must lower to the caller's VREG, not a physical "
           "register";
    auto const ops = run->lir.instOperands(inst);
    ASSERT_EQ(ops.size(), 1u);
    ASSERT_EQ(ops[0].kind, LirOperandKind::Reg);
    EXPECT_EQ(ops[0].reg.isPhysical, 0u);
    EXPECT_FALSE(result == ops[0].reg) << "the two bindings must stay distinct";
}

// ★★★ THE NEGATIVE HALF OF THE SAME MECHANISM, AND IT IS THE ONE THAT MATTERS.
// A template naming an operand the caller never bound must FAIL LOUD. The
// silent arm is the dangerous one: an unresolved operand that fell through to
// "some register" would emit a real instruction reading a register the C
// function never wrote.
TEST(AsmTemplateToLir, AnUnboundOperandIsRefusedNamingTheBoundSet) {
    auto const run = runTemplate(kX86, "movq %vreg3, %vreg0\n",
                                 {"vreg0", "vreg1"});
    ASSERT_TRUE(run->parsed) << "template did not parse";
    EXPECT_FALSE(run->ok) << "an unbound operand must be refused";
    auto const msg = messages(*run);
    EXPECT_NE(msg.find("vreg0"), std::string::npos)
        << "the refusal must name the operands that ARE bound: " << msg;
    EXPECT_NE(msg.find("2 operand"), std::string::npos) << msg;
    expectRefusalNamesThePair(*run, "vreg3");
}

TEST(AsmTemplateToLir, AnUnboundOperandIsRefusedOnArm64Too) {
    // arm64 is sigil-less, so `vreg3` reaches the register role ONLY through
    // the binding table — and an unbound name is an ordinary symbol there,
    // which the shape walk then refuses. Either way the template does not
    // silently assemble, which is the invariant.
    auto const run = runTemplate(kArm, "mov vreg0, vreg3\n",
                                 {"vreg0", "vreg1"});
    ASSERT_TRUE(run->parsed) << "template did not parse";
    EXPECT_FALSE(run->ok) << "an unbound operand must be refused";
    expectRefusalNamesThePair(*run, "vreg3");
}

// ── fail-loud on the vocabulary ───────────────────────────────────────────

TEST(AsmTemplateToLir, AnUnknownMnemonicNamesTheDialectAndTheTarget) {
    auto const run = runTemplate(kX86, "frobnicate\n", {});
    ASSERT_TRUE(run->parsed) << "template did not parse";
    EXPECT_FALSE(run->ok);
    expectRefusalNamesThePair(*run, "frobnicate");
}

TEST(AsmTemplateToLir, AnUnknownMnemonicNamesTheArm64PairToo) {
    auto const run = runTemplate(kArm, "frobnicate\n", {});
    ASSERT_TRUE(run->parsed) << "template did not parse";
    EXPECT_FALSE(run->ok);
    expectRefusalNamesThePair(*run, "frobnicate");
}

// ── what a TEMPLATE structurally cannot carry ─────────────────────────────
//
// ★ THESE ARE CAPABILITY STATEMENTS, NOT STUBS, AND THEY ARE PINNED BECAUSE
// THE SILENT ARM IS A MISCOMPILE. `LirOperand::makeBlockRef` names a
// FUNCTION-LOCAL SLOT: a template branching to its own label would bind to
// whichever block sits at that index in the CALLER's function.

TEST(AsmTemplateToLir, ATemplateLabelIsRefusedRatherThanBound) {
    auto const run = runTemplate(kX86, "Lloop:\n\tnop\n", {});
    ASSERT_TRUE(run->parsed) << "template did not parse";
    EXPECT_FALSE(run->ok) << "a template label must be refused";
    auto const msg = messages(*run);
    EXPECT_NE(msg.find("Lloop"), std::string::npos) << msg;
}

TEST(AsmTemplateToLir, ATemplateDirectiveIsRefusedRatherThanApplied) {
    auto const run = runTemplate(kX86, "\t.data\n", {});
    ASSERT_TRUE(run->parsed) << "template did not parse";
    EXPECT_FALSE(run->ok) << "a template directive must be refused";
    auto const msg = messages(*run);
    EXPECT_NE(msg.find("directive"), std::string::npos) << msg;
}

// ── the engine really is shared ───────────────────────────────────────────
//
// ★★★ THE CLAIM THIS FILE EXISTS FOR, ASSERTED DIRECTLY RATHER THAN IMPLIED.
// A template and a standalone `.s` containing the same instruction must reach
// the SAME BYTES. Two engines would have to be kept in agreement by hand, and
// nothing would notice the day they stopped agreeing.
TEST(AsmTemplateToLir, TemplateAndStandaloneAgreeByteForByte) {
    auto const tmpl = runTemplate(kX86, "nop\nrdtsc\n", {});
    ASSERT_TRUE(tmpl->ok) << messages(*tmpl);

    // The standalone path over the SAME two instructions, through the shipped
    // dialect, its own entry and its own terminator.
    auto dialect = loadDialect(kX86.dialect);
    auto targetR = TargetSchema::loadShipped(kX86.target);
    ASSERT_TRUE(targetR.has_value());

    UnitBuilder ub{dialect, DiagnosticBudget::libraryDefault()};
    ub.addInMemory(std::string{"\t.globl main\n\t.type main, "}
                       + std::string{kX86.functionMarker}
                       + "\nmain:\n\tnop\n\trdtsc\n\tret\n",
                   "<standalone>.s");
    CompilationUnit unit{std::move(ub).finish()};
    ASSERT_FALSE(unit.trees().empty());

    DiagnosticReporter rep;
    auto const mod = lowerAsmTextToLir(unit.trees()[0], *dialect, **targetR,
                                       dialect->assembly().entryLabels, rep);
    ASSERT_TRUE(mod.has_value()) << [&] {
        std::string s;
        for (auto const& d : rep.all()) { s += d.actual; s += '\n'; }
        return s;
    }();

    DiagnosticReporter     asmRep;
    std::vector<MirInstId> lirToMir(mod->lir.instCount());
    auto const asmMod = assemble(mod->lir, **targetR, lirToMir, asmRep);
    ASSERT_EQ(asmMod.functions.size(), 1u);
    EXPECT_EQ(tmpl->bytes, asmMod.functions[0].bytes)
        << "template " << hex(tmpl->bytes) << " vs standalone "
        << hex(asmMod.functions[0].bytes);
}

// ══ SAME VERB, TWO FRONT ENDS — THE COUNTER READ ═══════════════════════════
//
// ★★★ THE CLAIM PLAN 29 §4.7.3 ASKS FOR, AND WHAT IS AND IS NOT PROVEN HERE.
// The claim is that the dialect and the codegen reach ONE VERB — not that a
// `.s` assembles. A test that only checked the `.s` would prove the row exists,
// not that it is the same instruction the compiler emits, so both halves run
// the SAME instruction through the two callers of the shared engine and the
// encoded word is asserted IDENTICAL.
//
// ⚠⚠ THE SECOND FRONT END IS THE EMBEDDED TEMPLATE PATH, **NOT** C's
// `hwtime.h`, AND THAT SUBSTITUTION IS STATED RATHER THAN GLOSSED.
// ✔MEASURED 2026-08-14 by reading the tree, not by recalling it: `cntvct` has
// NO producer anywhere outside `src/asm/` (`grep -rn cntvct src/ --include=*.cpp
// --include=*.hpp` returns exactly one hit, a comment), and
// `semantic_analyzer.cpp` REFUSES every GNU inline-asm statement carrying a
// payload or a qualifier at the SEMANTIC tier — sqlite's `src/hwtime.h:43`
// parses and is then rejected with one precise diagnostic
// (D-CSUBSET-INLINE-ASM-TEXT is open, and emitting real asm text is a codegen
// question P1 does not touch). So C cannot reach the counter read today, and a
// test asserting it did would be asserting a capability that does not exist.
// ⇒ what IS proven: the two callers of `AsmInstructionLowering` — the
// standalone `.s` walker and `lowerAsmTemplateToLirRun`, which is the path an
// embedded `__asm__` reaches once the semantic gate opens — elect the SAME row
// and emit the SAME word. The C-side half is one gate away and needs no change
// here; when it opens, this test's `runTemplate` arm is the code it will run.
TEST(AsmTemplateToLir, Arm64CounterReadIsOneVerbAcrossBothFrontEnds) {
    auto const tail = baselineBytes(kArm);
    auto const tmpl = runTemplate(kArm, "mrs x0, cntvct_el0\n", {});
    ASSERT_TRUE(tmpl->parsed) << "template did not parse";
    ASSERT_TRUE(tmpl->ok) << messages(*tmpl);

    // ── the STRUCTURAL half: both paths elect the same opcode ROW.
    ASSERT_EQ(tmpl->opcodes.size(), 2u)
        << "`mrs x0, cntvct_el0` must add exactly ONE instruction";
    EXPECT_EQ(tmpl->opcodes.front(), opcodeOf(*tmpl->target, "cntvct"))
        << "the template elected something other than the counter read";
    // 0xD53BE040 little-endian — the word `aarch64-linux-gnu-as` 2.42 emits.
    EXPECT_EQ(tmpl->bytes, withTerminator({0x40, 0xE0, 0x3B, 0xD5}, tail))
        << "got " << hex(tmpl->bytes);

    // ── the STANDALONE `.s` half, through the shipped dialect's own entry.
    auto dialect = loadDialect(kArm.dialect);
    auto targetR = TargetSchema::loadShipped(kArm.target);
    ASSERT_TRUE(targetR.has_value());

    UnitBuilder ub{dialect, DiagnosticBudget::libraryDefault()};
    ub.addInMemory(std::string{"\t.globl main\n\t.type main, "}
                       + std::string{kArm.functionMarker}
                       + "\nmain:\n\tmrs x0, cntvct_el0\n\tret\n",
                   "<standalone>.s");
    CompilationUnit unit{std::move(ub).finish()};
    ASSERT_FALSE(unit.trees().empty());

    DiagnosticReporter rep;
    auto const mod = lowerAsmTextToLir(unit.trees()[0], *dialect, **targetR,
                                       dialect->assembly().entryLabels, rep);
    ASSERT_TRUE(mod.has_value()) << [&] {
        std::string s;
        for (auto const& d : rep.all()) { s += d.actual; s += '\n'; }
        return s;
    }();
    auto const blk = mod->lir.funcBlockAt(mod->lir.funcAt(0), 0);
    ASSERT_GE(mod->lir.blockInstCount(blk), 1u);
    EXPECT_EQ(mod->lir.instOpcode(mod->lir.blockInstAt(blk, 0)),
              opcodeOf(**targetR, "cntvct"))
        << "the `.s` path elected a different opcode row from the template — "
           "the two front ends are not reaching one verb";

    DiagnosticReporter     asmRep;
    std::vector<MirInstId> lirToMir(mod->lir.instCount());
    auto const asmMod = assemble(mod->lir, **targetR, lirToMir, asmRep);
    ASSERT_EQ(asmMod.functions.size(), 1u);
    EXPECT_EQ(tmpl->bytes, asmMod.functions[0].bytes)
        << "template " << hex(tmpl->bytes) << " vs standalone "
        << hex(asmMod.functions[0].bytes);
}

// ★★ THE SELECTOR REFUSAL REACHES THE TEMPLATE PATH TOO. Nothing in the engine
// knows which caller it serves, so this SHOULD hold for free — which is exactly
// why it is asserted rather than assumed: "for free" is a claim about a shared
// code path, and a shared code path is only shared until someone branches it.
TEST(AsmTemplateToLir, Arm64TemplateRefusesAnUnselectedSystemRegister) {
    auto const run = runTemplate(kArm, "mrs x0, tpidr_el0\n", {});
    ASSERT_TRUE(run->parsed) << "template did not parse";
    EXPECT_FALSE(run->ok)
        << "the template lowered `mrs x0, tpidr_el0` — the selector is being "
           "dropped on this caller even though the `.s` caller matches it";
    expectRefusalNamesThePair(*run, "tpidr_el0");
}

// ★ AND `cset`, so the selector mechanism is witnessed on BOTH callers with
// BOTH of the shipped selector rows rather than with one of each.
TEST(AsmTemplateToLir, Arm64TemplateLowersCsetThroughItsSelector) {
    auto const tail = baselineBytes(kArm);
    auto const run  = runTemplate(kArm, "cset x0, ls\n", {});
    ASSERT_TRUE(run->parsed) << "template did not parse";
    ASSERT_TRUE(run->ok) << messages(*run);
    EXPECT_EQ(run->opcodes.front(), opcodeOf(*run->target, "setcc"));
    // 0x9A9F87E0 little-endian — gas's word for the UNSIGNED `ls`, which a
    // letter-matching cond table would have encoded as the signed `le`
    // (0x9A9FC7E0). The two differ, so this pin can tell them apart.
    EXPECT_EQ(run->bytes, withTerminator({0xE0, 0x87, 0x9F, 0x9A}, tail))
        << "got " << hex(run->bytes);
}

// ★★★ AND THE W-FORM ON THIS CALLER TOO (D-ASM-ARM64-SETCC-W-FORM-UNDECLARED,
// closed 2026-08-15). The width is elected off the REGISTER SPELLING, and the
// election runs in the shared engine — so a template writing `cset w0, ls`
// must reach the target's width-32 `setcc` variant exactly as the `.s` caller
// does. ⚠ THE PAIR IS THE ASSERTION, NOT THE W ROW ALONE: an engine that
// ignored the width would emit the X word here and be green against any pin
// that did not also state what the X word is.
// ✔MEASURED 2026-08-15, `aarch64-linux-gnu-as` 2.42: `cset x0, ls` 9a9f87e0,
// `cset w0, ls` 1a9f87e0 (little-endian below).
TEST(AsmTemplateToLir, Arm64TemplateElectsTheCsetWidthFromTheRegister) {
    auto const tail = baselineBytes(kArm);

    auto const x = runTemplate(kArm, "cset x0, ls\n", {});
    ASSERT_TRUE(x->parsed) << "template did not parse";
    ASSERT_TRUE(x->ok) << messages(*x);
    EXPECT_EQ(x->bytes, withTerminator({0xE0, 0x87, 0x9F, 0x9A}, tail))
        << "got " << hex(x->bytes);

    auto const w = runTemplate(kArm, "cset w0, ls\n", {});
    ASSERT_TRUE(w->parsed) << "template did not parse";
    ASSERT_TRUE(w->ok) << messages(*w);
    EXPECT_EQ(w->bytes, withTerminator({0xE0, 0x87, 0x9F, 0x1A}, tail))
        << "the W spelling did not elect the width-32 variant — got "
        << hex(w->bytes);
}

// ══ CASE FOLDING — D-ASM-DIALECT-MNEMONIC-MATCH-IS-CASE-SENSITIVE ═══════════
//
// ★★★ THE CLAIM: in a dialect declaring `spellingCase: asciiFolded`, an
// UPPERCASE line and its lowercase twin elect the SAME opcode row and assemble
// to BYTE-IDENTICAL output, on all FOUR folded surfaces — mnemonic, register,
// operand selector and directive. (The directive half runs in
// `test_asm_shipped_dialects.cpp`, because a template legitimately refuses
// directives; the other three are here, where bytes are already the currency.)
//
// ★★ EVERY PAIR PINS THE REFERENCE BYTES, NOT MERELY "THE TWO AGREE". Two runs
// that agreed on the WRONG encoding would satisfy an equality-only assertion
// perfectly — and "the fold reached the register table and came back with a
// different register" is exactly what a case-insensitive lookup can get wrong.
// So each arm is compared against the word `as` / `aarch64-linux-gnu-as` 2.42
// actually emitted for that exact text, and then the arms to each other.
//
// ✔MEASURED 2026-08-15 (rc + `objdump -d` / `aarch64-linux-gnu-objdump -d`):
//   x86_64  `movq %rax,%rcx` = `MOVQ %RAX,%RCX` = `MoVq %RAX,%rcx` -> 48 89 c1
//           `movl %eax,%ecx` = `MOVL %EAX,%ECX`                    -> 89 c1
//   arm64   `mov x0,x1`      = `MOV X0,X1`      = `MoV x0,X1`      -> aa0103e0
//           `mov w0,w1`      = `MOV W0,W1`                         -> 2a0103e0
//           `mrs x0,cntvct_el0` = `MRS X0,CNTVCT_EL0`              -> d53be040
//           `cset x0,eq`     = `CSET X0,EQ`                        -> 9a9f17e0

namespace {

// One template run, asserted to have parsed and lowered, reduced to its bytes.
[[nodiscard]] std::vector<std::uint8_t>
foldBytes(ShippedPair const& p, std::string_view text) {
    auto const run = runTemplate(p, text, {});
    EXPECT_TRUE(run->parsed) << "template did not parse: " << text;
    EXPECT_TRUE(run->ok) << text << " — " << messages(*run);
    return run->bytes;
}

} // namespace

// ⚠⚠ THE x86 PIN IS DSS'S ENCODING, NOT gas'S, AND THE DIFFERENCE IS REAL,
// MEASURED, AND NOT A DEFECT. x86 spells a register→register MOV two redundant
// ways: `89 /r` (MOV r/m64, r64) and `8B /r` (MOV r64, r/m64). ✔MEASURED
// 2026-08-15 — gas emits `48 89 c1` for `movq %rax, %rcx`, DSS's variant
// election emits `48 8b c8`, and `objdump -d` decodes BOTH as `mov %rax,%rcx`
// (likewise `89 c1` / `8b c8` at 32 bits). Same instruction, same length,
// different redundant encoding — an encoder choice the target's own guard table
// makes, and nothing to do with case.
// ⇒ pinning gas's bytes here would fail for a reason that has no bearing on
// case folding, so the pin states what THIS compiler emits — and the case claim
// is carried by the arm-to-arm equality plus the 32-vs-64 discrimination below,
// neither of which the encoding choice touches.
TEST(AsmTemplateToLir, X86MnemonicAndRegisterFoldToTheReferenceBytes) {
    auto const tail = baselineBytes(kX86);

    auto const lower = foldBytes(kX86, "movq %rax, %rcx\n");
    EXPECT_EQ(lower, withTerminator({0x48, 0x8B, 0xC8}, tail))
        << "got " << hex(lower);
    for (auto const* text : {"MOVQ %RAX, %RCX\n", "MoVq %RAX, %rcx\n",
                             "movq %RAX, %rcx\n", "MOVQ %rax, %rcx\n"}) {
        auto const upper = foldBytes(kX86, text);
        EXPECT_EQ(upper, lower)
            << "`" << text << "` did not fold to the lowercase twin — got "
            << hex(upper);
    }

    // ★ AND THE 32-BIT ROW, BECAUSE ON AT&T THE WIDTH IS IN BOTH HALVES. A fold
    // that reached the mnemonic but resolved `%EAX` to the 64-bit `rax` would
    // still be green against a mnemonic-only pin; these two encodings differ in
    // LENGTH, so they cannot be confused for one another.
    auto const l32 = foldBytes(kX86, "movl %eax, %ecx\n");
    EXPECT_EQ(l32, withTerminator({0x8B, 0xC8}, tail)) << "got " << hex(l32);
    EXPECT_EQ(foldBytes(kX86, "MOVL %EAX, %ECX\n"), l32);
    EXPECT_NE(l32, lower) << "the 32- and 64-bit forms must differ, or the "
                             "width half of this pin asserts nothing";
}

TEST(AsmTemplateToLir, Arm64MnemonicAndRegisterFoldToTheReferenceWord) {
    auto const tail = baselineBytes(kArm);

    // 0xAA0103E0 little-endian.
    auto const lower = foldBytes(kArm, "mov x0, x1\n");
    EXPECT_EQ(lower, withTerminator({0xE0, 0x03, 0x01, 0xAA}, tail))
        << "got " << hex(lower);
    for (auto const* text : {"MOV X0, X1\n", "MoV x0, X1\n", "mov X0, x1\n"}) {
        auto const upper = foldBytes(kArm, text);
        EXPECT_EQ(upper, lower)
            << "`" << text << "` did not fold to the lowercase twin — got "
            << hex(upper);
    }

    // ★★★ THE W-FORM IS THE SHARPEST HALF OF THIS PIN. On aarch64 the REGISTER
    // is the only thing carrying the width — `mov` and `MOV` are one mnemonic
    // either way — so `MOV W0, W1` encoding `aa0103e0` instead of `2a0103e0`
    // would be a fold that reached the register table and came back with the
    // WRONG register, silently, on a clean build. That is the same shape as the
    // measured `mov w0,w1`-encodes-64-bit miscompile this dialect was born
    // from, and only a pin that states BOTH words can see it.
    auto const lw = foldBytes(kArm, "mov w0, w1\n");
    EXPECT_EQ(lw, withTerminator({0xE0, 0x03, 0x01, 0x2A}, tail))
        << "got " << hex(lw);
    EXPECT_EQ(foldBytes(kArm, "MOV W0, W1\n"), lw)
        << "the uppercase W-form did not reach the width-32 register";
    EXPECT_NE(lw, lower) << "the W and X words must differ, or the width half "
                            "of this pin asserts nothing";
}

// ★★ THE SELECTOR SURFACE — the one the anchor row says must fold WITH the
// mnemonic or not at all. Both shipped selector families run: a system register
// (`mrs`) and a condition (`cset`).
TEST(AsmTemplateToLir, Arm64OperandSelectorsFoldWithTheirMnemonic) {
    auto const tail = baselineBytes(kArm);

    // 0xD53BE040 — the counter read.
    auto const mrsLower = foldBytes(kArm, "mrs x0, cntvct_el0\n");
    EXPECT_EQ(mrsLower, withTerminator({0x40, 0xE0, 0x3B, 0xD5}, tail))
        << "got " << hex(mrsLower);
    for (auto const* text : {"MRS X0, CNTVCT_EL0\n", "mrs x0, CNTVCT_EL0\n",
                             "MRS x0, CntVct_El0\n"}) {
        EXPECT_EQ(foldBytes(kArm, text), mrsLower)
            << "`" << text << "` did not reach the counter row";
    }

    // 0x9A9F17E0 — `cset x0, eq`. ★ THE CONDITION IS A SELECTOR *AND* names a
    // `TargetCondCode`, so a fold that matched the row but lost the condition
    // would land on a different word. The pin is the word, not the row.
    auto const csetLower = foldBytes(kArm, "cset x0, eq\n");
    EXPECT_EQ(csetLower, withTerminator({0xE0, 0x17, 0x9F, 0x9A}, tail))
        << "got " << hex(csetLower);
    EXPECT_EQ(foldBytes(kArm, "CSET X0, EQ\n"), csetLower);
    EXPECT_EQ(foldBytes(kArm, "cset x0, EQ\n"), csetLower)
        << "the SELECTOR alone was not folded — a mnemonic-only fold leaves "
           "`CSET X0, EQ` failing one token to the right, which is the "
           "half-fix this row exists to refuse";

    // ⚠ AND FOLDING MUST NOT BLUR THE ROWS IT IS SUPPOSED TO CHOOSE BETWEEN.
    // `ls` and `eq` are different conditions in any case.
    EXPECT_NE(foldBytes(kArm, "CSET X0, LS\n"), csetLower)
        << "`LS` and `EQ` reached the same row — the fold is matching more "
           "than case";
}

// ══ RED ON DISABLE ═════════════════════════════════════════════════════════
//
// ★★★ THE MUTANT IS THE SHIPPED DOCUMENT WITH ONE KEY CHANGED, and it is proven
// READ by what the compiler DOES with it: under `sensitive` the identical
// uppercase text must be REFUSED BY NAME. Were the fold hardcoded in the engine
// — the shape the anchor row forbids — every assertion below would fail, because
// the uppercase arms would keep lowering and keep producing bytes.

TEST(AsmTemplateToLir, TurningTheFoldOffRefusesTheUppercaseMnemonic) {
    for (auto const& p : {kX86, kArm}) {
        auto const strict = loadDialectMutated(
            p.dialect, [](nlohmann::json& doc) {
                doc["assembly"]["spellingCase"] = "sensitive";
            });
        ASSERT_NE(strict.grammar, nullptr)
            << p.dialect << ": the STRICT mutant must still be a VALID "
               "document — a load failure here would make the refusal below "
               "green for the wrong reason: " << joined(strict.loadErrors);

        // The lowercase form still lowers, so the mutant is not simply broken.
        auto const ok = runTemplateWith(strict.grammar, p, "nop\n", {});
        ASSERT_TRUE(ok->ok) << p.dialect << ": " << messages(*ok);

        auto const bad = runTemplateWith(strict.grammar, p, "NOP\n", {});
        ASSERT_TRUE(bad->parsed)
            << p.dialect << ": `NOP` must still PARSE — a lex-level refusal "
               "would prove nothing about the SPELLING MATCH";
        EXPECT_FALSE(bad->ok)
            << p.dialect << ": `NOP` lowered under 'spellingCase': 'sensitive'"
               " — the fold is not coming from the dialect key, so something "
               "hardcoded is doing it";
        expectRefusalNamesThePair(*bad, "NOP");
    }
}

TEST(AsmTemplateToLir, TurningTheFoldOffRefusesTheUppercaseRegister) {
    auto const strict = loadDialectMutated(
        kArm.dialect, [](nlohmann::json& doc) {
            doc["assembly"]["spellingCase"] = "sensitive";
        });
    ASSERT_NE(strict.grammar, nullptr) << joined(strict.loadErrors);

    // ⚠ THE MNEMONIC IS LOWERCASE HERE ON PURPOSE. It isolates the REGISTER
    // surface: a refusal of `MOV X0, X1` could be the mnemonic's, which would
    // leave the register half unwitnessed.
    auto const bad = runTemplateWith(strict.grammar, kArm, "mov X0, X1\n", {});
    ASSERT_TRUE(bad->parsed) << "`mov X0, X1` must still parse";
    EXPECT_FALSE(bad->ok)
        << "`X0` resolved under 'spellingCase': 'sensitive' — the register "
           "fold is not coming from the dialect key";

    auto const good = runTemplateWith(strict.grammar, kArm, "mov x0, x1\n", {});
    EXPECT_TRUE(good->ok) << messages(*good);
}

TEST(AsmTemplateToLir, TurningTheFoldOffRefusesTheUppercaseSelector) {
    auto const strict = loadDialectMutated(
        kArm.dialect, [](nlohmann::json& doc) {
            doc["assembly"]["spellingCase"] = "sensitive";
        });
    ASSERT_NE(strict.grammar, nullptr) << joined(strict.loadErrors);

    // Mnemonic and register lowercase; only the SELECTOR is uppercase.
    auto const bad =
        runTemplateWith(strict.grammar, kArm, "mrs x0, CNTVCT_EL0\n", {});
    ASSERT_TRUE(bad->parsed);
    EXPECT_FALSE(bad->ok)
        << "`CNTVCT_EL0` selected the counter row under 'sensitive' — the "
           "selector fold is not coming from the dialect key";
    // ★ AND IT IS THE SELECTOR ARM OF THE REFUSAL, NOT "UNKNOWN MNEMONIC":
    // `mrs` IS declared, and saying otherwise would send a reader to add a row
    // that is already there.
    expectRefusalNamesThePair(*bad, "CNTVCT_EL0");
}

// ★★★ THE KEY IS REQUIRED, NOT DEFAULTED — red-on-disable one tier up. A
// dialect that simply OMITS `spellingCase` must not load at all: a default
// would silently pick a case policy for a document that never stated one, in
// whichever direction the default happened to go.
TEST(AsmTemplateToLir, ADialectThatOmitsSpellingCaseIsRefusedAtLoad) {
    for (auto const& p : {kX86, kArm}) {
        auto const missing = loadDialectMutated(
            p.dialect, [](nlohmann::json& doc) {
                doc["assembly"].erase("spellingCase");
            });
        EXPECT_EQ(missing.grammar, nullptr)
            << p.dialect << ": the document loaded with no 'spellingCase' — "
               "the key is being defaulted somewhere";
        EXPECT_NE(joined(missing.loadErrors).find("spellingCase"),
                  std::string::npos)
            << p.dialect << ": the refusal must NAME the missing key: "
            << joined(missing.loadErrors);
    }
}

TEST(AsmTemplateToLir, AnUnknownSpellingCaseNamesTheClosedSet) {
    auto const bogus = loadDialectMutated(
        kX86.dialect, [](nlohmann::json& doc) {
            doc["assembly"]["spellingCase"] = "caseInsensitive";
        });
    EXPECT_EQ(bogus.grammar, nullptr)
        << "an unknown 'spellingCase' loaded clean";
    auto const why = joined(bogus.loadErrors);
    EXPECT_NE(why.find("caseInsensitive"), std::string::npos) << why;
    // The closed set, read out of the same table the loader validates against
    // — a hand-typed list here would rot the day a third policy lands.
    for (auto const& [name, v] : kAsmSpellingCaseNames) {
        (void)v;
        EXPECT_NE(why.find(name), std::string::npos)
            << "the refusal must list '" << name << "': " << why;
    }
}
