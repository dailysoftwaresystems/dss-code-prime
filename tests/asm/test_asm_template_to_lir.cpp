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
// ⓘ WHAT IS PINNED HERE ABOUT `%N` — UPDATED 2026-08-15, when the placeholder
// surface landed (D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER). This banner
// used to say the `%0` SPELLING was "owned by a concurrent lane and not on disk
// yet"; it is on disk now, and the last section of this file drives it. The
// earlier `%vreg0` tests are KEPT UNCHANGED rather than rewritten: they bind a
// spelling the dialect reads through its ordinary `attRegister`, so they isolate
// the ENGINE's half (binding-table resolution) from the DIALECT's half (the
// placeholder shape), and a failure in one no longer implicates the other.

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

#include <array>
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
    std::optional<Tree>              tree;
    DiagnosticReporter               reporter;
    bool                             parsed = false;
    bool                             ok     = false;
    Lir                              lir;
    std::vector<std::uint8_t>        bytes;
    std::vector<std::uint16_t>       opcodes;   // the emitted block, in order
    std::vector<LirReg>              bindingRegs;   // index-parallel to the bindings
    // Index-parallel to the LABEL bindings the caller asked for: one block per
    // label, whatever number of spellings that label answers to.
    std::vector<LirBlockId>          labelBlocks;
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
// ★★★ `physicalFor` IS INDEX-PARALLEL TO `bindingSpellings`, AND IT EXISTS SO A
// PLACEHOLDER RUN CAN BE TAKEN ALL THE WAY TO BYTES. A vreg-bound template
// lowers correctly and CANNOT be assembled — `assemble()` runs after register
// allocation and a virtual register has no encoding — so every byte assertion
// over a placeholder must pin the operand to a real register first, exactly as
// a register-pinned constraint (`"=a"`) does on x86. An empty entry (or a short
// vector) means "mint a vreg", which is what every pre-existing caller wants.
// ⚠ WITHOUT THIS THE arm64 `mrs %0, cntvct_el0` PIN WAS SILENTLY VACUOUS: it
// asserted equality against an EMPTY byte vector, i.e. it was measuring the
// harness giving up rather than the instruction encoding.
// ★★★ `labelSpellings` IS A LIST **PER LABEL**, NOT A FLAT LIST OF SPELLINGS,
// AND IT MIRRORS THE DESCRIPTOR SHAPE THE REAL CALLER CARRIES. One `asm goto`
// label answers to EVERY spelling the source could name it by — the bracketed
// `%l[done]` and the positional `%l2` are two names for ONE block — so the
// harness creates one block per OUTER entry and binds every spelling in the
// inner list to it. A flat list would have made "two spellings, one block"
// unexpressible, which is exactly the property a positional-plus-symbolic
// template exercises.
//
// ⚠ `templateTerminates` IS THE TEST STATING A PROPERTY OF THE TEMPLATE **IT
// WROTE**, AND IT IS SELF-CHECKING RATHER THAN TRUSTED. A template ending in a
// branch SEALS the entry block, so the harness must not add its own `ret`
// there; one that does not, must get one. Both mistakes abort LOUDLY inside
// `LirBuilder` — "block already terminated" if the flag is too low, "current
// block has no terminator" if it is too high — so a wrong value can never
// produce a quietly wrong CFG for an assertion to read.
[[nodiscard]] std::unique_ptr<TemplateRun>
runTemplateWith(std::shared_ptr<GrammarSchema> dialect, ShippedPair const& p,
                std::string_view                templateText,
                std::vector<std::string> const& bindingSpellings,
                std::vector<std::string> const& physicalFor = {},
                AsmTemplateSurface              surface
                    = AsmTemplateSurface::Extended,
                std::vector<std::vector<std::string>> const& labelSpellings = {},
                bool                            templateTerminates = false) {
    auto run     = std::make_unique<TemplateRun>();
    run->dialect = std::move(dialect);
    auto targetR = TargetSchema::loadShipped(p.target);
    if (!targetR.has_value()) {
        throw std::runtime_error{"cannot load shipped target"};
    }
    run->target = *targetR;

    // ★★★ THE TEMPLATE GOES THROUGH `parseAsmTemplateText`, NOT `UnitBuilder`,
    // AND THAT SUBSTITUTION IS ITSELF PART OF WHAT THIS FILE PINS. `UnitBuilder`
    // reads a buffer as a whole FILE, i.e. in the `main` lexer mode — the `.s`
    // surface, in which `%` is the register sigil and `%0` cannot parse. The
    // template entry starts the tokenizer in the dialect's own
    // `assembly.templateLexerMode`, which is the ONLY reason a placeholder
    // exists at all. A harness that kept `UnitBuilder` would be testing the
    // wrong surface and every placeholder test below would be red for a reason
    // that has nothing to do with the placeholder.
    // ⚠ EVERY RUN IN THIS FILE IS **EXTENDED** UNLESS A TEST SAYS OTHERWISE,
    // and that is the honest default for a harness whose whole subject is
    // operand binding: an extended template is the only kind that HAS operands.
    // The BASIC surface has its own test below, because the two differ in the
    // LEXER and a harness that could only reach one of them would be measuring
    // half the construct.
    run->tree = parseAsmTemplateText(std::string{templateText}, "<template>",
                                     run->dialect, surface,
                                     DiagnosticBudget::libraryDefault(),
                                     run->reporter);
    run->parsed = run->tree.has_value();
    if (!run->parsed) return run;

    LirBuilder builder{*run->target};
    builder.addFunction(SymbolId{1});
    LirBlockId const entry = builder.createBlock();
    builder.beginBlock(entry);

    std::vector<AsmOperandBinding> bindings;
    for (std::size_t i = 0; i < bindingSpellings.size(); ++i) {
        AsmOperandBinding b;
        b.spelling  = bindingSpellings[i];
        b.regClass  = LirRegClass::GPR;
        b.widthBits = 64;
        if (i < physicalFor.size() && !physicalFor[i].empty()) {
            auto const ord = run->target->registerByName(physicalFor[i]);
            if (!ord.has_value()) {
                throw std::runtime_error{"target declares no register "
                                         + physicalFor[i]};
            }
            b.reg = makePhysicalReg(*ord, LirRegClass::GPR);
        } else {
            b.reg = builder.newVReg(LirRegClass::GPR);
        }
        run->bindingRegs.push_back(b.reg);
        bindings.push_back(std::move(b));
    }

    // The `asm goto` targets. ⚠ THE BLOCKS ARE CREATED BEFORE THE LOWERING RUNS
    // AND FILLED AFTER IT **UNCONDITIONALLY**, including on the refusal path:
    // `LirBuilder::closeFunction` aborts on a block that was created and never
    // opened, so a harness that only closed them on success would turn every
    // refusal test into a process abort.
    std::vector<AsmLabelBinding> labelBindings;
    for (auto const& spellings : labelSpellings) {
        LirBlockId const block = builder.createBlock();
        run->labelBlocks.push_back(block);
        for (auto const& s : spellings) {
            AsmLabelBinding b;
            b.spelling = s;
            b.block    = block;
            labelBindings.push_back(std::move(b));
        }
    }

    run->ok = lowerAsmTemplateToLirRun(*run->tree, *run->dialect,
                                       *run->target, bindings, builder,
                                       run->reporter, labelBindings);

    // Close the block the way the embedding language will. ⚠ A TEMPLATE THAT
    // BRANCHES HAS ALREADY SEALED IT — `LirBuilder` seals a block on one
    // terminator and aborts on a second — so the harness adds its own only when
    // the caller said the template does not terminate, or when the lowering
    // REFUSED before it could (a refused `jmp %l[x]` emits nothing at all).
    auto const retOp = run->target->opcodeByMnemonic("ret");
    if (!retOp.has_value()) throw std::runtime_error{"target has no `ret`"};
    if (!templateTerminates || !run->ok) builder.addReturn(*retOp, {});
    for (LirBlockId const block : run->labelBlocks) {
        builder.beginBlock(block);
        builder.addReturn(*retOp, {});
    }
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
            std::vector<std::string> const& bindingSpellings,
            std::vector<std::string> const& physicalFor = {},
            AsmTemplateSurface              surface
                = AsmTemplateSurface::Extended,
            std::vector<std::vector<std::string>> const& labelSpellings = {},
            bool                            templateTerminates = false) {
    return runTemplateWith(loadDialect(p.dialect), p, templateText,
                           bindingSpellings, physicalFor, surface,
                           labelSpellings, templateTerminates);
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
    // `%%vreg0` parses as AT&T's ordinary `attRegister` — through the ESCAPED
    // arm, which is the only register form an extended template has; the target
    // declares no register by that name, so the ONLY way it can resolve is
    // through the binding table. ★ THAT IS WHY THIS TEST IS KEPT ALONGSIDE THE
    // `%0` ONE RATHER THAN REPLACED BY IT: it exercises binding-table resolution
    // through the dialect's ORDINARY register production, so a failure here
    // implicates the ENGINE and a failure in the `%0` test implicates the
    // PLACEHOLDER SHAPE. Two mechanisms, two witnesses.
    auto const run = runTemplate(kX86, "movq %%vreg1, %%vreg0\n",
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
    auto const run = runTemplate(kX86, "movq %%vreg3, %%vreg0\n",
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
// payload or a qualifier at the SEMANTIC tier — sqlite's `src/hwtime.h`
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
// ⚠⚠ THE REGISTERS ARE WRITTEN `%%rax`, NOT `%rax`, AND THAT CHANGED
// 2026-08-15 WITH THE PLACEHOLDER SURFACE — IT IS A CONFORMANCE FIX, NOT AN
// ACCOMMODATION. In an EXTENDED template `%`+letter is an operand MODIFIER, so
// the register sigil must be doubled. ✔MEASURED on gcc 13.3.0 and clang 18.1.3
// (sources fed as base64): `__asm__("xorl %eax,%eax" ::: "eax")` is REJECTED by
// BOTH ("operand number missing after %-letter" / "invalid % escape in inline
// assembly string") while the `%%` form assembles. This file previously wrote
// the single-`%` form because the template was lexed on the `.s` surface, where
// it is correct; it is wrong on the template surface, and the two are now
// genuinely different lexer modes. The FOLD claim below is untouched — only the
// spelling of the register moved.
TEST(AsmTemplateToLir, X86MnemonicAndRegisterFoldToTheReferenceBytes) {
    auto const tail = baselineBytes(kX86);

    auto const lower = foldBytes(kX86, "movq %%rax, %%rcx\n");
    EXPECT_EQ(lower, withTerminator({0x48, 0x8B, 0xC8}, tail))
        << "got " << hex(lower);
    for (auto const* text : {"MOVQ %%RAX, %%RCX\n", "MoVq %%RAX, %%rcx\n",
                             "movq %%RAX, %%rcx\n", "MOVQ %%rax, %%rcx\n"}) {
        auto const upper = foldBytes(kX86, text);
        EXPECT_EQ(upper, lower)
            << "`" << text << "` did not fold to the lowercase twin — got "
            << hex(upper);
    }

    // ★ AND THE 32-BIT ROW, BECAUSE ON AT&T THE WIDTH IS IN BOTH HALVES. A fold
    // that reached the mnemonic but resolved `%EAX` to the 64-bit `rax` would
    // still be green against a mnemonic-only pin; these two encodings differ in
    // LENGTH, so they cannot be confused for one another.
    auto const l32 = foldBytes(kX86, "movl %%eax, %%ecx\n");
    EXPECT_EQ(l32, withTerminator({0x8B, 0xC8}, tail)) << "got " << hex(l32);
    EXPECT_EQ(foldBytes(kX86, "MOVL %%EAX, %%ECX\n"), l32);
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

// ══ THE TEMPLATE PLACEHOLDER SURFACE ══════════════════════════════════════
//
// ★★★ WHAT IS UNDER TEST AND WHY IT IS NOT "DOES `%0` PARSE". The claim is that
// an embedded `__asm__` template and a standalone `.s` are the SAME dialect read
// through TWO LEXICAL SURFACES, and that the difference is carried by a lexer
// mode rather than by a flag anyone can forget. Three properties follow, and
// each is asserted separately below because each can break alone:
//   (1) a placeholder RESOLVES — `%0` denotes the caller's vreg, not a register;
//   (2) a placeholder is UNREACHABLE from a `.s` — the surfaces stay separate;
//   (3) `%%` is consumed as ONE token — the miscompile guard.
//
// ⚠ EVERY TEST HERE DRIVES THE SHIPPED DOCUMENTS UNMUTATED (or mutates exactly
// one key of the real one). A hand-authored dialect would prove the mechanism
// about the stub.

namespace {

// A standalone `.s`, parsed the way a `.s` really is — through `UnitBuilder`,
// i.e. in the `main` lexer mode. ★ THIS IS THE CONTROL FOR EVERY "unreachable
// from a `.s`" CLAIM BELOW, and it has to go through a different entry point
// than the template harness or it would not be testing the separation at all.
[[nodiscard]] std::string standaloneParseMessages(GrammarSchema const& dialect,
                                                  std::shared_ptr<GrammarSchema> owned,
                                                  std::string_view source) {
    UnitBuilder ub{std::move(owned), DiagnosticBudget::libraryDefault()};
    ub.addInMemory(std::string{source}, "<standalone>.s");
    CompilationUnit unit{std::move(ub).finish()};
    (void)dialect;
    std::string out;
    if (unit.trees().empty()) return "<no tree>";
    for (auto const& d : unit.trees()[0].diagnostics().all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += d.actual;
        for (auto const& e : d.expected) { out += ' '; out += e; }
        out += '\n';
    }
    return out;
}

} // namespace

// ── (1) the placeholder resolves ──────────────────────────────────────────

TEST(AsmTemplateToLir, PercentZeroBindsTheCallersVregOnBothDialects) {
    // AT&T writes the destination LAST, arm64 writes it FIRST — so the same
    // claim is spelled two ways on purpose. Running it on ONE dialect would
    // leave "the placeholder happens to land where this dialect's destination
    // is" indistinguishable from "the placeholder resolves".
    struct Case { ShippedPair p; std::string_view text; };
    for (auto const& c : {Case{kX86, "movq %1, %0\n"},
                          Case{kArm, "mov %0, %1\n"}}) {
        auto const run = runTemplate(c.p, c.text, {"%0", "%1"});
        ASSERT_TRUE(run->parsed)
            << c.p.dialect << ": `" << c.text << "` did not PARSE — the "
            << "template lexer mode is not minting the placeholder sigil:\n"
            << messages(*run);
        ASSERT_TRUE(run->ok) << c.p.dialect << ": " << messages(*run);
        ASSERT_EQ(run->opcodes.size(), 2u) << c.p.dialect;

        auto const blk  = run->lir.funcBlockAt(run->lir.funcAt(0), 0);
        auto const inst = run->lir.blockInstAt(blk, 0);
        EXPECT_EQ(run->lir.instOpcode(inst), opcodeOf(*run->target, "mov"))
            << c.p.dialect;

        // ★★ THE EXACT VREGS THE CALLER MINTED, IDENTIFIED BY VALUE. "is a
        // vreg" would stay green if the engine invented a fresh one; the
        // binding is only honoured if the register is the SAME OBJECT the
        // caller handed over, and `%0`/`%1` must not be interchanged.
        ASSERT_EQ(run->bindingRegs.size(), 2u);
        LirReg const result = run->lir.instResult(inst);
        EXPECT_TRUE(result == run->bindingRegs[0])
            << c.p.dialect << ": `%0` did not become binding 0";
        auto const ops = run->lir.instOperands(inst);
        ASSERT_EQ(ops.size(), 1u) << c.p.dialect;
        ASSERT_EQ(ops[0].kind, LirOperandKind::Reg) << c.p.dialect;
        EXPECT_TRUE(ops[0].reg == run->bindingRegs[1])
            << c.p.dialect << ": `%1` did not become binding 1";
    }
}

// ★★★ THE OUT-OF-RANGE ARM, AND ON arm64 IT IS THE ONE THAT MATTERS MOST: this
// target declares the constraint letters `r`/`w`/`m`/`i` and NONE of them pins
// a register, so a placeholder is the ONLY way an aarch64 asm output can be
// named — an index that silently resolved to "some register" would corrupt
// whatever the enclosing function had there, with a clean build log.
TEST(AsmTemplateToLir, PercentThreeWithTwoOperandsFailsLoudNamingTheBoundSet) {
    struct Case { ShippedPair p; std::string_view text; };
    for (auto const& c : {Case{kX86, "movq %3, %0\n"},
                          Case{kArm, "mov %0, %3\n"}}) {
        auto const run = runTemplate(c.p, c.text, {"%0", "%1"});
        ASSERT_TRUE(run->parsed)
            << c.p.dialect << ": `%3` must PARSE and be refused SEMANTICALLY — "
               "a parse error could not name how many operands were bound:\n"
            << messages(*run);
        EXPECT_FALSE(run->ok)
            << c.p.dialect << ": `%3` was accepted with two operands bound";
        auto const msg = messages(*run);
        EXPECT_NE(msg.find("%0"), std::string::npos)
            << c.p.dialect << ": the refusal must name the operands that ARE "
               "bound: " << msg;
        EXPECT_NE(msg.find("2 operand"), std::string::npos)
            << c.p.dialect << ": " << msg;
        expectRefusalNamesThePair(*run, "%3");
    }
}

// ★★★ THE WIDTH VIEW (`%w0` / `%x0`) STATES THE **DECLARED** WIDTH, NOT THE
// BINDING'S OWN — AND THE PROOF IS THAT THE TEMPLATE'S BYTES EQUAL THE
// STANDALONE `.s`'s.
//
// ★★ THE HARNESS MINTS EVERY BINDING AT 64 BITS, WHICH IS WHAT MAKES THIS
// DISCRIMINATE RATHER THAN AGREE BY LUCK. A build that ignored the letter and
// used the binding's own width would encode the 64-bit `mov` for BOTH rows, and
// the narrow row would match the wide `.s` control instead of the narrow one.
// ⚠ AND THE THIRD ASSERTION IS THE ONE A "both compiled fine" TEST WOULD MISS:
// the two views must NOT produce the same bytes. Two rows that both encoded
// 64-bit would satisfy every "equals its own control" check written in the
// obvious order.
TEST(AsmTemplateToLir, AWidthViewStatesTheDeclaredWidthAndNotTheBindingsOwn) {
    auto const narrow =
        runTemplate(kArm, "mov %w0, %w1\n", {"%0", "%1"}, {"x0", "x1"});
    ASSERT_TRUE(narrow->parsed)
        << "`%w0` did not PARSE — the template lexer mode is not minting the "
           "composed width-view lexeme:\n" << messages(*narrow);
    ASSERT_TRUE(narrow->ok) << messages(*narrow);

    auto const wide =
        runTemplate(kArm, "mov %x0, %x1\n", {"%0", "%1"}, {"x0", "x1"});
    ASSERT_TRUE(wide->parsed) << messages(*wide);
    ASSERT_TRUE(wide->ok) << messages(*wide);

    // The SAME two instructions written as a standalone `.s`, which is the one
    // control that cannot be satisfied by a coincidence: the `.s` register
    // spelling is where this pipeline's width has always come from.
    auto const sNarrow = runTemplate(kArm, "mov w0, w1\n", {});
    ASSERT_TRUE(sNarrow->ok) << messages(*sNarrow);
    auto const sWide = runTemplate(kArm, "mov x0, x1\n", {});
    ASSERT_TRUE(sWide->ok) << messages(*sWide);

    EXPECT_EQ(narrow->bytes, sNarrow->bytes)
        << "`mov %w0, %w1` must encode exactly `mov w0, w1`";
    EXPECT_EQ(wide->bytes, sWide->bytes)
        << "`mov %x0, %x1` must encode exactly `mov x0, x1`";
    EXPECT_NE(narrow->bytes, sWide->bytes)
        << "the two views encoded the same instruction — the declared width is "
           "not being read";
}

// ★★★ ONE LETTER, TWO DIALECTS, TWO WIDTHS — THE MEASUREMENT THAT PUTS THE
// TABLE ON THE DIALECT AND NOT IN SHARED SUBSTRATE.
//
// ✔MEASURED 2026-08-24 by execution on gcc 13.3.0, both ports, `-O0` and `-O2`:
// `%w0` renders `w0` (32 bits) under `aarch64-linux-gnu-gcc` and `%ax` (16
// bits) under x86-64 gcc. A single shared letter table would therefore be wrong
// for one of its two readers BY CONSTRUCTION, which is why the letters live in
// each dialect's `assembly.templateModifiers`. This test is that sentence made
// executable: the same three bytes of template text, two dialects, two widths.
TEST(AsmTemplateToLir, TheSameWidthViewLetterMeansADifferentWidthInEachDialect) {
    auto const arm =
        runTemplate(kArm, "mov %w0, %w1\n", {"%0", "%1"}, {"x0", "x1"});
    ASSERT_TRUE(arm->ok) << messages(*arm);
    auto const arm32 = runTemplate(kArm, "mov w0, w1\n", {});
    ASSERT_TRUE(arm32->ok) << messages(*arm32);
    EXPECT_EQ(arm->bytes, arm32->bytes) << "`%w` must be 32 bits on aarch64";

    // The AT&T document declares `w` as the SIXTEEN-bit view, so the same
    // letter must reach `%ax`/`%cx` and the 16-bit mnemonic.
    auto const att =
        runTemplate(kX86, "movw %w1, %w0\n", {"%0", "%1"}, {"rax", "rcx"});
    ASSERT_TRUE(att->parsed) << messages(*att);
    ASSERT_TRUE(att->ok) << messages(*att);
    // ⚠ THE CONTROL IS WRITTEN WITH THE **ESCAPE**, NOT WITH A BARE `%`. Every
    // run in this harness is read in the dialect's TEMPLATE lexer mode, where
    // `%` is the placeholder sigil and not the register sigil — so `%ax` there
    // is a malformed placeholder, and `%%ax` is how a template names a machine
    // register at all (GNU 6.47.2.3). ✔MEASURED: the first draft of this test
    // wrote the bare form and the CONTROL failed with "'cx'", i.e. the control
    // was not the instruction it claimed to be. The aarch64 half needs no
    // escape because that dialect writes no register sigil.
    auto const att16 = runTemplate(kX86, "movw %%cx, %%ax\n", {});
    ASSERT_TRUE(att16->ok) << messages(*att16);
    EXPECT_EQ(att->bytes, att16->bytes) << "`%w` must be 16 bits on x86-64";

    // ⚠ AND THE NEGATIVE HALF, because "equals the 16-bit control" would also
    // hold if the bytes were empty. The 32-bit mnemonic against a 16-bit view is
    // a width disagreement and must be REFUSED by name — which is only true if
    // `%w` really is 16 here.
    auto const mismatched =
        runTemplate(kX86, "movl %w1, %w0\n", {"%0", "%1"}, {"rax", "rcx"});
    ASSERT_TRUE(mismatched->parsed) << messages(*mismatched);
    EXPECT_FALSE(mismatched->ok)
        << "`movl` over a 16-bit view was accepted: " << messages(*mismatched);
    EXPECT_NE(messages(*mismatched).find("16 bits"), std::string::npos)
        << messages(*mismatched);
}

// `%[name]` — GNU 6.47.2.3's SYMBOLIC operand name. The engine holds no `%N`
// convention, so a caller that binds the symbolic spelling gets it resolved by
// exactly the same comparison the numeric one uses. ★ THAT IS THE POINT OF THE
// TEST: it proves the placeholder surface is not a numeric special case.
TEST(AsmTemplateToLir, ASymbolicPlaceholderResolvesThroughTheSameBindingTable) {
    auto const run = runTemplate(kX86, "movq %[src], %[dst]\n",
                                 {"%[dst]", "%[src]"});
    ASSERT_TRUE(run->parsed) << messages(*run);
    ASSERT_TRUE(run->ok) << messages(*run);
    auto const blk  = run->lir.funcBlockAt(run->lir.funcAt(0), 0);
    auto const inst = run->lir.blockInstAt(blk, 0);
    ASSERT_EQ(run->bindingRegs.size(), 2u);
    EXPECT_TRUE(run->lir.instResult(inst) == run->bindingRegs[0]);
}

// ── (1b) the `asm goto` LABEL placeholder ─────────────────────────────────
//
// ★★★ WHAT THESE PIN AND WHAT THEY REPLACE. Until P20 the label form was the
// GRAMMAR HALF ONLY: `AsmOperandBinding` carried a `LirReg` and no block, so a
// label spelling matched no binding and the host refused it, and the test here
// pinned the WORDING of that refusal — because the lowering behaviour was
// identical either way and nothing else could see the difference. The lowering
// exists now, so the sentence that test protected has to be protected by the
// refusal that REMAINS: an UNBOUND label. A label the caller did not bind still
// cannot become a branch, for exactly the reason the old comment gave — the
// block belongs to the embedding language's CFG — so the pin moved rather than
// went away.

// The resolution, in BOTH spellings and on BOTH dialects. ★ THE ASSERTION IS
// THE BLOCK THE BRANCH NAMES, not merely that the template lowered: a `jmp`
// that reached the wrong block would lower, encode, and run the wrong code.
TEST(AsmTemplateToLir, ALabelPlaceholderBranchesToTheBoundBlock) {
    struct Case { ShippedPair p; std::string_view text; std::string_view spelling; };
    for (auto const& c : {Case{kX86, "jmp %l[done]\n", "%l[done]"},
                          Case{kX86, "jmp %l2\n",      "%l2"},
                          Case{kArm, "b %l[done]\n",   "%l[done]"},
                          Case{kArm, "b %l2\n",        "%l2"}}) {
        // ⚠ BOTH SPELLINGS NAME **ONE** LABEL AND THEREFORE ONE BLOCK — the
        // shape a real `asm goto` carries, where the bracketed and positional
        // forms are two names for the same target. Binding them to two blocks
        // would have made "the branch reached the right block" trivially true
        // for whichever spelling was tried.
        auto const run = runTemplate(c.p, c.text, {}, {},
                                     AsmTemplateSurface::Extended,
                                     {{"%l[done]", "%l2"}},
                                     /*templateTerminates=*/true);
        ASSERT_TRUE(run->parsed)
            << c.p.dialect << " / " << c.spelling << ": did not PARSE — the "
               "two-byte `%l` lexeme, the selector, or the bracketed-name shape "
               "is missing:\n" << messages(*run);
        ASSERT_TRUE(run->ok)
            << c.p.dialect << " / " << c.spelling << ":\n" << messages(*run);
        ASSERT_EQ(run->labelBlocks.size(), 1u);

        auto const entry = run->lir.funcBlockAt(run->lir.funcAt(0), 0);
        ASSERT_EQ(run->lir.blockInstCount(entry), 1u)
            << c.p.dialect << " / " << c.spelling
            << ": the template must have emitted exactly the branch";
        auto const term = run->lir.blockTerminator(entry);
        auto const ops  = run->lir.instOperands(term);
        ASSERT_EQ(ops.size(), 1u)
            << c.p.dialect << " / " << c.spelling
            << ": an unconditional branch names exactly one target";
        EXPECT_EQ(ops[0].kind, LirOperandKind::BlockRef)
            << c.p.dialect << " / " << c.spelling
            << ": the label placeholder did not become a BLOCK reference — it "
               "is still being decoded as an operand";
        EXPECT_EQ(ops[0].blockSlot, run->labelBlocks[0].v)
            << c.p.dialect << " / " << c.spelling
            << ": the branch names the wrong block";
        // ★ AND THE CFG EDGE, WHICH IS A SEPARATE FACT FROM THE OPERAND. A
        // BlockRef operand with no recorded successor is a branch the later
        // passes cannot see.
        auto const succs = run->lir.blockSuccessors(entry);
        ASSERT_EQ(succs.size(), 1u) << c.p.dialect << " / " << c.spelling;
        EXPECT_TRUE(succs[0] == run->labelBlocks[0])
            << c.p.dialect << " / " << c.spelling
            << ": the successor edge does not reach the bound block";
    }
}

// ★★★ THE REFUSAL THAT REMAINS, AND IT IS THE ONE THE OLD TEST'S SENTENCE
// SURVIVES IN. A template still declares NO labels of its own: the blocks
// around it belong to the embedding language, and `LirOperand::makeBlockRef`
// names a FUNCTION-LOCAL slot — so a spelling nobody bound must be refused by
// name and must never be bound to "some block anyway". ⚠ THE MESSAGE MUST NAME
// THE BOUND SET, exactly as the unbound-OPERAND refusal does: `%l3` on a
// two-label `asm goto` is the shape this catches, and a generic "no such block"
// would be true and useless.
TEST(AsmTemplateToLir, AnUnboundLabelPlaceholderIsRefusedNamingTheBoundLabels) {
    auto const run = runTemplate(kX86, "jmp %l[done]\n", {}, {},
                                 AsmTemplateSurface::Extended,
                                 {{"%l[other]"}}, /*templateTerminates=*/true);
    ASSERT_TRUE(run->parsed)
        << "`%l[done]` did not PARSE — the two-byte `%l` lexeme or the "
           "bracketed-name shape is missing:\n" << messages(*run);
    EXPECT_FALSE(run->ok) << "an unbound label spelling must be refused";
    expectRefusalNamesThePair(*run, "%l[done]");
    auto const msg = messages(*run);
    EXPECT_NE(msg.find("asm goto"), std::string::npos)
        << "the refusal must name the CONSTRUCT, not merely report an unknown "
           "branch target: " << msg;
    EXPECT_NE(msg.find("%l[other]"), std::string::npos)
        << "the refusal must ENUMERATE the labels that WERE bound — the count "
           "and the list are the whole diagnosis, and only the host can supply "
           "them: " << msg;
    EXPECT_NE(msg.find("block"), std::string::npos)
        << "the refusal must say WHY a template cannot invent one — a label "
           "resolves to a block of the CALLER's function: " << msg;
}

// ★★ AND THE POSITIONAL SPELLING IS MATCHED THE SAME WAY, WHICH IS WHAT STOPS
// "`%l2` parses" FROM BEING MISTAKEN FOR "`%l2` resolves". A template naming
// `%l2` against a label bound only as `%l[done]` must be refused, not silently
// answered by the label that happens to be there.
TEST(AsmTemplateToLir, APositionalLabelSpellingIsNotAnsweredByABracketedBinding) {
    auto const run = runTemplate(kX86, "jmp %l2\n", {}, {},
                                 AsmTemplateSurface::Extended,
                                 {{"%l[done]"}}, /*templateTerminates=*/true);
    ASSERT_TRUE(run->parsed) << messages(*run);
    EXPECT_FALSE(run->ok)
        << "`%l2` resolved against a binding spelled `%l[done]` — the engine is "
           "interpreting the spelling instead of comparing it, and a label's "
           "index would then be decided somewhere below the front end:\n"
        << messages(*run);
    EXPECT_NE(messages(*run).find("%l2"), std::string::npos) << messages(*run);
}

// ★★★ A LABEL IN A NON-BRANCH POSITION IS REFUSED, AND THE MESSAGE NAMES THE
// CONSTRUCT. `movq %l[done], %0` decodes the label as a symbol-valued source,
// so the ordinary instruction path asks the host for its ADDRESS — a different
// question from "which block does this branch to", and one this build answers
// for nothing inside a template. ⚠ THE BINDING IS SUPPLIED, so this cannot pass
// for the unbound refusal above: the label IS bound and the POSITION is what is
// wrong.
TEST(AsmTemplateToLir, ALabelPlaceholderOutsideABranchIsRefusedNamingAsmGoto) {
    auto const run = runTemplate(kX86, "movq %l[done], %0\n", {"%0"}, {},
                                 AsmTemplateSurface::Extended,
                                 {{"%l[done]"}});
    ASSERT_TRUE(run->parsed) << messages(*run);
    EXPECT_FALSE(run->ok)
        << "a bound label was accepted as an instruction SOURCE — that is the "
           "computed-goto construct, which no reference gives `%l` and which "
           "this build does not lower:\n" << messages(*run);
    expectRefusalNamesThePair(*run, "%l[done]");
    auto const msg = messages(*run);
    EXPECT_NE(msg.find("asm goto"), std::string::npos)
        << "the refusal must name the construct the author actually wrote, not "
           "only say that an address cannot be taken: " << msg;
}

// ★★★ THE `spellingCase` SEAM, BOTH SIDES, AGAINST **ONE** DIALECT — the pin
// that stops `decodeRegister` and `decodePlaceholder` from drifting apart.
//
// `AsmLoweringHost::namesRegister`'s contract used to promise host authors that
// "the engine folds before it asks" and that "a binding must be registered in
// folded form". That is true of the REGISTER caller and false of the PLACEHOLDER
// one, and the contract said only the first half — so a host that followed it
// would register `%[out]` for a source-spelled `%[Out]` and get a refusal.
//
// ✔BOTH ARMS RUN ON THE SHIPPED `asciiFolded` AT&T DOCUMENT, which is what makes
// this a statement about the SPLIT rather than two unrelated facts:
//   • a REGISTER written `%RAX` resolves against the target's `rax` (folded);
//   • a PLACEHOLDER written `%[Out]` does NOT resolve against a binding spelled
//     `%[out]` (verbatim) — ✔MEASURED behaviour of gcc, whose symbolic operand
//     names are ordinary case-SENSITIVE C identifiers.
// ⚠ If either convention is "tidied" to match the other, exactly one of these
// two assertions goes red, and the message says which direction was taken.
TEST(AsmTemplateToLir, SpellingCaseSplitsAtThePlaceholderSeam) {
    // The register half: the dialect folds, so the shouted spelling resolves.
    auto const reg = runTemplate(kX86, "movq %%RCX, %%RAX\n", {});
    ASSERT_TRUE(reg->parsed) << messages(*reg);
    ASSERT_TRUE(reg->ok)
        << "a `asciiFolded` dialect refused a shouted REGISTER spelling — the "
           "engine is no longer folding before it asks the host:\n"
        << messages(*reg);

    // The placeholder half: the SAME dialect must NOT fold, so a binding
    // spelled `%[out]` does not answer a template that wrote `%[Out]`.
    auto const ph = runTemplate(kX86, "movq %[in], %[Out]\n",
                                {"%[out]", "%[in]"});
    ASSERT_TRUE(ph->parsed) << messages(*ph);
    EXPECT_FALSE(ph->ok)
        << "`%[Out]` resolved against a binding spelled `%[out]` — the "
           "placeholder spelling is being folded, which silently merges two "
           "distinct GNU operand names into one:\n" << messages(*ph);
    EXPECT_NE(messages(*ph).find("%[Out]"), std::string::npos)
        << messages(*ph);

    // ...and the control: with the binding spelled EXACTLY as written, it does.
    auto const exact = runTemplate(kX86, "movq %[in], %[Out]\n",
                                   {"%[Out]", "%[in]"});
    ASSERT_TRUE(exact->parsed) << messages(*exact);
    EXPECT_TRUE(exact->ok)
        << "an exactly-spelled placeholder binding did not resolve — the arm "
           "above would then be green for the wrong reason:\n"
        << messages(*exact);
}

// ★★★ §4.7.3's "SAME VERB, TWO FRONT ENDS" TEST, NOW WRITABLE. Until the
// placeholder existed, sqlite `hwtime.h`'s arm64 arm could not be spelled at
// all: AArch64 has no register-pinning constraint letter, so the output has
// nowhere to go except a placeholder. The word is the reference assembler's,
// recorded in `arm64.target.json`'s own `fixedWord`.
TEST(AsmTemplateToLir, Arm64MrsThroughAPlaceholderReachesTheCounterWord) {
    auto const tail = baselineBytes(kArm);
    // ★ `%0` IS PINNED TO `x0` SO THE WORD CAN BE ASSERTED AT ALL. A vreg
    // cannot be encoded (assembly runs post-regalloc), and `MRS X0, CNTVCT_EL0`
    // puts Rd in the low five bits — so with x0 the encoded word is exactly the
    // `fixedWord` the target document declares, and a WRONG destination would
    // change the byte this pin reads.
    auto const run  = runTemplate(kArm, "mrs %0, cntvct_el0\n", {"%0"}, {"x0"});
    ASSERT_TRUE(run->parsed)
        << "`mrs %0, cntvct_el0` did not parse:\n" << messages(*run);
    ASSERT_TRUE(run->ok) << messages(*run);
    ASSERT_EQ(run->opcodes.size(), 2u);
    EXPECT_EQ(run->opcodes.front(), opcodeOf(*run->target, "cntvct"))
        << "the placeholder form must reach the SAME zero-operand counter "
           "opcode the standalone `.s` reaches — a generic `mrs` would be a "
           "second shape for one verb";
    ASSERT_EQ(run->bindingRegs.size(), 1u);
    auto const blk  = run->lir.funcBlockAt(run->lir.funcAt(0), 0);
    EXPECT_TRUE(run->lir.instResult(run->lir.blockInstAt(blk, 0))
                == run->bindingRegs[0])
        << "the counter must be read INTO the caller's vreg";
    // 0xD53BE040, little-endian.
    EXPECT_EQ(run->bytes, withTerminator({0x40, 0xE0, 0x3B, 0xD5}, tail))
        << "got " << hex(run->bytes);
}

// ── (2) `%%` names a real register, and only in a template ────────────────

// ★★★ THE LITERAL-PERCENT FORM, WHICH HAD NO SPELLING AT ALL BEFORE THIS.
// ✔MEASURED on gcc 13.3.0 and clang 18.1.3: in an EXTENDED template `%eax` is
// REJECTED by both and `%%eax` assembles to `xorl %eax,%eax`. So this is the
// only way to name a machine register in an extended template, and the negative
// miscompile pin the previous cycle had to write on arm64 becomes writable here.
TEST(AsmTemplateToLir, X86EscapedPercentNamesTheMachineRegister) {
    auto const run = runTemplate(kX86, "movq %%rcx, %%rax\n", {});
    ASSERT_TRUE(run->parsed)
        << "`%%rcx, %%rax` did not parse — `PercentEscape` has no shape:\n"
        << messages(*run);
    ASSERT_TRUE(run->ok) << messages(*run);
    ASSERT_EQ(run->opcodes.size(), 2u);

    auto const blk  = run->lir.funcBlockAt(run->lir.funcAt(0), 0);
    auto const inst = run->lir.blockInstAt(blk, 0);
    EXPECT_EQ(run->lir.instOpcode(inst), opcodeOf(*run->target, "mov"));
    // ★★ PHYSICAL, NOT VIRTUAL — the whole difference from `%0`. A template
    // that resolved `%%rax` through the binding table (or to a fresh vreg)
    // would compile clean and write the wrong register.
    LirReg const result = run->lir.instResult(inst);
    EXPECT_EQ(result.isPhysical, 1u)
        << "`%%rax` must denote the MACHINE register";
    auto const rax = run->target->registerByName("rax");
    ASSERT_TRUE(rax.has_value());
    EXPECT_EQ(result.id, *rax) << "`%%rax` resolved to the wrong register";
}

// ★★★ THE SAME BYTES AS THE `.s`, WHICH IS THE CLAIM THAT MATTERS. `%%rax` in a
// template and `%rax` in a `.s` are the same register written two ways, so the
// two surfaces must reach one encoding. A hand-typed byte constant would pin my
// memory; comparing the two front ends pins the property.
TEST(AsmTemplateToLir, EscapedPercentAndPlainPercentReachIdenticalBytes) {
    auto const tmpl = runTemplate(kX86, "movq %%rcx, %%rax\n", {});
    ASSERT_TRUE(tmpl->ok) << messages(*tmpl);

    auto dialect = loadDialect(kX86.dialect);
    auto targetR = TargetSchema::loadShipped(kX86.target);
    ASSERT_TRUE(targetR.has_value());
    UnitBuilder ub{dialect, DiagnosticBudget::libraryDefault()};
    ub.addInMemory(std::string{"\t.globl main\n\t.type main, "}
                       + std::string{kX86.functionMarker}
                       + "\nmain:\n\tmovq %rcx, %rax\n\tret\n",
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
    auto const obj = assemble(mod->lir, **targetR, lirToMir, asmRep);
    ASSERT_EQ(obj.functions.size(), 1u);
    EXPECT_EQ(tmpl->bytes, obj.functions[0].bytes)
        << "template " << hex(tmpl->bytes) << " vs `.s` "
        << hex(obj.functions[0].bytes);
}

// ★★★ THE SEPARATION, ASSERTED FROM THE OTHER SIDE. gas has no `%%` and rejects
// `%0` as a register name, so a `.s` must refuse BOTH — and it must refuse them
// because the template mode was never entered, not because of a check anyone
// wrote. [[feedback_reference_compilers_are_the_spec]] is bidirectional:
// accepting what no reference accepts is the same defect as refusing what they
// do, and this is the arm that catches it.
// ⚠ THE POSITIONAL LABEL FORM IS IN THE LIST, AND IT HAD TO BE ADDED THE DAY
// IT BECAME GRAMMATICAL (P20). `%l2` was previously unreachable from a `.s` for
// TWO independent reasons — the mode never mints `%l`, AND no shape accepted
// `%l` followed by an index — and widening the shape removed one of them. If
// the remaining reason ever stopped holding, a `.s` would accept a form gas
// rejects, which is the same defect as refusing one it takes.
TEST(AsmTemplateToLir, ThePlaceholderSurfaceIsUnreachableFromAStandaloneFile) {
    for (auto const& p : {kX86, kArm}) {
        auto dialect = loadDialect(p.dialect);
        for (auto const& line : {"\tmovq %0, %0\n", "\tmovq %%rcx, %%rax\n",
                                 "\tmov %l[done], %0\n", "\tmov %l2, %0\n"}) {
            auto const msg = standaloneParseMessages(*dialect, dialect, line);
            EXPECT_FALSE(msg.empty())
                << p.dialect << ": a `.s` ACCEPTED `" << line
                << "` — the template surface has leaked into the `main` lexer "
                   "mode, and no reference assembler takes this input";
        }
    }
}

// ── (3) `%%` is ONE token — the miscompile guard ──────────────────────────

// ★★★ THE DEFECT THIS FORBIDS HAS NO DIAGNOSTIC. ✔MEASURED 2026-08-14 (gcc
// 13.3.0, clang 18.1.3, aarch64-linux-gnu-gcc 13.3.0, sources fed as base64):
// extended `"A%%0B"` emits `A%0B`, and the `%0` that APPEARS is NOT re-read as
// operand 0. Any design that unescaped `%%`→`%` into a buffer and then scanned
// for placeholders would bind `%%0` to operand 0 and silently emit the wrong
// register.
//
// ★★ THE INSTRUMENT IS A DISCRIMINATING PAIR, NOT A LONE REFUSAL. `%0` at a
// given position BINDS; `%%0` at the SAME position — one byte longer — must not.
// Asserting only the second would stay green if the whole placeholder surface
// were broken, which is exactly how a guard stops guarding.
TEST(AsmTemplateToLir, DoublePercentZeroDoesNotBindOperandZero) {
    for (auto const& p : {kX86, kArm}) {
        auto const bound = runTemplate(p, p.dialect == kX86.dialect
                                              ? "movq %1, %0\n"
                                              : "mov %0, %1\n",
                                       {"%0", "%1"});
        ASSERT_TRUE(bound->ok)
            << p.dialect << ": the positive half of the pair must bind — "
               "without it the negative half asserts nothing:\n"
            << messages(*bound);

        auto const escaped = runTemplate(p, p.dialect == kX86.dialect
                                                ? "movq %1, %%0\n"
                                                : "mov %%0, %1\n",
                                         {"%0", "%1"});
        EXPECT_FALSE(escaped->parsed && escaped->ok)
            << p.dialect
            << ": `%%0` was ACCEPTED where `%0` binds operand 0 — the two "
               "percent bytes are being re-read as an escape plus a "
               "placeholder, which is the silent miscompile this pin exists "
               "for:\n" << messages(*escaped);
    }
}

// ── red-on-disable: the rows and keys are READ ────────────────────────────

// ★★★ RED-ON-DISABLE FOR THE ESCAPE ROW, IN BOTH OF THE TWO DIRECTIONS THE
// MECHANISM CAN FAIL — and the first one turned out STRONGER than the arm
// originally written for it, which is worth recording rather than quietly
// keeping.
//
// ⚠ THE INTENDED ARM WAS "delete the row, watch `%%rcx` stop lowering". What
// actually happens is that the DOCUMENT STOPS LOADING: `PercentEscape` is
// reachable ONLY through the template mode, so removing it leaves
// `attRegister`'s escaped arm referencing a kind nothing interns and the loader
// refuses with `/shapes/attRegister: unknown reference 'PercentEscape'`. That is
// a better result than the behavioural one and it is the one asserted — the
// mutant is provably READ, because the loader names the exact shape that
// depended on it, and the row cannot be dropped by accident.
//
// ★★ THE LEVER MOVED WITH THE OWNER (P5c, 2026-08-17 —
// D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER). The dialect
// no longer DECLARES the `%%` row; `asm.lang.json` owns the bytes and the loader
// synthesizes the row by joining them to the KIND this document binds to the
// `templateEscape` role. So the mutation that removes the row is now the removal
// of that BINDING — and the assertion is unchanged, which is the point: the
// consumer still names itself, and the arm now also proves the row is DERIVED
// rather than declared.
TEST(AsmTemplateToLir, DeletingTheEscapeRowIsRefusedAtLoadNamingItsConsumer) {
    auto const without = loadDialectMutated(
        kX86.dialect, [](nlohmann::json& doc) {
            ASSERT_FALSE(doc["lexerModes"]["asm-template"].contains("tokens"))
                << "the dialect declares template-mode tokens of its own again "
                   "— the row is supposed to be synthesized from "
                   "asm.lang.json's semantics.inlineAsmTemplateLexemes";
            std::size_t erased = 0;
            for (auto& [_, ref] : doc["languageReferences"].items()) {
                if (!ref.is_object() || !ref.contains("bindTokens")) continue;
                erased += ref["bindTokens"].erase("templateEscape");
            }
            ASSERT_EQ(erased, 1u)
                << "the shipped document binds no 'templateEscape' role — this "
                   "arm would be green for the wrong reason";
        });
    EXPECT_EQ(without.grammar, nullptr)
        << "the document loaded with the `%%` row deleted — nothing depends on "
           "it, so the escaped register form is not coming from where this "
           "file says it is";
    auto const why = joined(without.loadErrors);
    EXPECT_NE(why.find("PercentEscape"), std::string::npos) << why;
    EXPECT_NE(why.find("attRegister"), std::string::npos)
        << "the refusal must name the SHAPE that depended on the row: " << why;
}

// ★★★ POINTING `templateLexerMode` AT A MODE THAT MINTS NO PLACEHOLDER SIGIL IS
// NOW A **LOAD** REFUSAL, AND THIS TEST MOVED WITH IT (2026-08-15).
//
// ⓘ WHAT IT USED TO ASSERT AND WHY THE CHANGE IS A STRENGTHENING, NOT A LOSS.
// It used to point the key at a `decoy` scanning mode, load the document
// successfully, and then observe that neither `%%rcx` nor `%0` lowered — a
// BEHAVIOURAL demonstration that the surface goes dead. The demonstration was
// real and it is exactly what the loader was missing: the old check tested only
// `defaultToken`, so `"templateLexerMode": "main"` — and this decoy, and a
// `tokens: "default"` mode, and a mode with no `tokens` key — all loaded clean
// and installed the `.s` reading. The loader now asks the general question
// (does this mode mint the kinds FIRST(templateOperandRule) needs?) and refuses
// at LOAD, so the state this test used to reach can no longer be constructed.
// ⚠ THE DECOY IS DELIBERATELY UNCHANGED: it still overrides `@@` and nothing
// else. Making it mint the placeholder rows to keep the old assertion alive
// would have removed the only arm that pins the check's GENERALITY — `main` is
// the obvious instance, and a name-based check would look fixed while leaving
// every other silently-dead mode accepted.
// ★ The behavioural claim it carried is not orphaned; the test directly below
// makes it sharper.
//
// ★★★ AND IT MOVED AGAIN AT P5c (2026-08-17,
// D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER), THIS TIME
// FROM A REFUSAL TO AN IMPOSSIBILITY — which is the stronger of the two and is
// why the test now asserts the OPPOSITE outcome.
//
// ⓘ WHAT CHANGED. The failure this arm guarded was "the named mode does not mint
// the placeholder sigil, so the whole template surface is silently dead". The
// dialect no longer declares those rows at all: the LOADER synthesizes one row
// per template role INTO whatever scanning mode `templateLexerMode` names. A
// named mode therefore CANNOT fail to mint the sigil — not "is checked and
// refused", but cannot. So a decoy that overrides something irrelevant now loads
// clean AND lowers templates, and asserting a refusal here would be asserting
// that the fix did not happen.
// ⚠ THE GUARD IS NOT GONE, IT IS RELOCATED, AND THE RESIDUE IS STILL LIVE: the
// same FIRST-set check now fires when the LANGUAGE declares a role `null` that
// this dialect's grammar needs (see the sibling suite
// `analysis/semantic/test_inline_asm_template_sigil_agreement`), and `main` is
// still refused below because it is not a mode this document declares.
TEST(AsmTemplateToLir, TheLoaderFillsWhicheverScanningModeIsNamed) {
    auto const decoyed = loadDialectMutated(
        kX86.dialect, [](nlohmann::json& doc) {
            // A real scanning mode that overrides something irrelevant — the
            // exact shape that used to make the template surface silently dead.
            doc["lexerModes"]["decoy"]["tokens"]["@@"] =
                nlohmann::json::parse(R"([{"kind": "TypeSigil"}])");
            doc["assembly"]["templateLexerMode"] = "decoy";
        });
    ASSERT_NE(decoyed.grammar, nullptr)
        << "a scanning mode named as the template lexer mode was REFUSED — the "
           "loader is supposed to FILL it from the language's role set, which is "
           "what makes a silently-dead template surface unconstructible: "
        << joined(decoyed.loadErrors);

    // The mode it was pointed at is the mode that got the rows...
    LexerModeId const mode = decoyed.grammar->assembly().templateLexerMode;
    ASSERT_TRUE(mode.valid());
    EXPECT_EQ(decoyed.grammar->findLexerMode("decoy").v, mode.v)
        << "the loaded schema's template mode is not the one the key named";

    // ...and a template still lowers through it, which is the behavioural half.
    auto const moved = runTemplateWith(decoyed.grammar, kX86,
                                       "movq %1, %0\n", {"%0", "%1"});
    ASSERT_TRUE(moved->parsed)
        << "`%0` did not parse with the template mode repointed at a decoy — the "
           "synthesis did not follow the key:\n" << messages(*moved);
    EXPECT_TRUE(moved->ok) << messages(*moved);

    // ...while the mode's OWN unrelated override is untouched: the synthesis
    // ADDS the role rows, it does not replace the table.
    EXPECT_FALSE(decoyed.grammar->lookupLexemeInMode(mode, "@@").empty())
        << "the decoy's own `@@` row was discarded — the synthesis is "
           "overwriting a declaration instead of extending it";
}

// ★★★ AND `main` IS REFUSED BY THE SAME CHECK RATHER THAN BY ITS NAME. This is
// the arm the audit asked for by name, and it is separate from the decoy above
// because the two failure shapes are different: `main` HAS a lexeme table (the
// global one, verbatim) and the decoy has an override of its own. A check that
// tested "is this mode `main`?" would pass the decoy; one that tested "does this
// mode have a `tokens` override?" would pass `main`'s sibling shapes. Only the
// kind-mint question refuses all of them.
TEST(AsmTemplateToLir, TheMainModeIsRefusedAsATemplateLexerMode) {
    for (auto const& p : {kX86, kArm}) {
        auto const mained = loadDialectMutated(
            p.dialect, [](nlohmann::json& doc) {
                doc["assembly"]["templateLexerMode"] = "main";
            });
        EXPECT_EQ(mained.grammar, nullptr)
            << p.dialect << ": `\"templateLexerMode\": \"main\"` loaded clean — "
               "the template entry would hand back the `.s` surface, which is "
               "the exact outcome the unknown-mode refusal exists to prevent";
        auto const why = joined(mained.loadErrors);
        EXPECT_NE(why.find("PlaceholderSigil"), std::string::npos)
            << p.dialect << ": " << why;
    }
}

// ★★★ A `tokens: "default"` MODE IS THE THIRD SHAPE, AND IT IS THE ONE THAT
// LOOKS MOST LIKE A REAL OVERRIDE IN THE DOCUMENT. It is a VERBATIM COPY of the
// global table under a non-main id, so `lexerModeTokens` has an entry for it and
// any check phrased over "does this mode have a table?" passes it — while the
// surface it produces is byte-for-byte the `.s` one.
//
// ★★ STILL REFUSED AT P5c, AND NOW FOR A SHARPER REASON. It is no longer "this
// mode mints no placeholder sigil" (the loader would happily mint one INTO it);
// it is that a verbatim copy of the global table is a SECOND DECLARATION of the
// template sigils — with their `.s` meanings — in the one mode whose table the
// loader owns. Overwriting it would silently discard a statement the author made,
// so the load is refused instead
// (D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER, R1's third
// shape).
TEST(AsmTemplateToLir, ATemplateModeWhoseTokensAreDefaultIsRefusedAtLoad) {
    auto const copied = loadDialectMutated(
        kX86.dialect, [](nlohmann::json& doc) {
            doc["lexerModes"]["global-copy"]["tokens"] = "default";
            doc["assembly"]["templateLexerMode"] = "global-copy";
        });
    EXPECT_EQ(copied.grammar, nullptr)
        << "a `tokens: \"default\"` mode was accepted as the template lexer "
           "mode — it is the global table under another name";
    auto const why = joined(copied.loadErrors);
    EXPECT_NE(why.find("global-copy"), std::string::npos)
        << "the refusal must name the mode: " << why;
    EXPECT_NE(why.find("inlineAsmTemplateLexemes"), std::string::npos)
        << "the refusal must name the OWNER whose rows this table would be a "
           "second copy of: " << why;
}

// ★★★ THE BEHAVIOURAL HALF, SHARPENED: THE ENTRY READS THE **NAMED** MODE'S OWN
// TABLE, NOT THE ONE THAT HAPPENS TO BE CALLED `asm-template`.
//
// ★★ THE DECOY HERE MINTS THE PLACEHOLDER KINDS UNDER **DIFFERENT BYTES**, which
// is what makes this a discrimination rather than a tautology. It satisfies the
// loader's kind-mint check honestly (so the document loads) while binding the
// sigil to `@` instead of `%`. If the template entry read `assembly
// .templateLexerMode` as it claims, `@0` must now lower and `%0` must not; if it
// read a hard-coded mode name — or the shipped table — the opposite would hold.
// ⚠ Same binary, same input text shape, ONE config key: no other experiment in
// this file separates "the mode is consulted" from "the shipped mode works".
// ★★★ AND AT P5c THIS TEST BECAME THE REFUSAL IT USED TO DEMONSTRATE
// (2026-08-17, D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER).
//
// ⓘ WHAT IT USED TO DO: mint `@`→`PlaceholderSigil` and `@l`→
// `PlaceholderLabelSigil` in a decoy mode, repoint `templateLexerMode` at it, and
// show that `@0` then lowered while `%0` went dead. The experiment was exactly
// right about the mechanism it tested — the entry reads the NAMED mode — and it
// is exactly the shape now FORBIDDEN, because a dialect choosing the template
// sigil's spelling is the second owner this row closed. The sigils belong to the
// HOST LANGUAGE (gcc substitutes them before the assembler is involved), so
// changing them is a change to `asm.lang.json`'s role set, not to a dialect.
//
// ★★ SO THE CLAIM SPLIT IN TWO AND BOTH HALVES ARE KEPT.
//   • "the entry reads the NAMED mode's table" → `TheLoaderFillsWhicheverScanning
//     ModeIsNamed` above (repoint the key at a decoy; the rows land THERE, a
//     template lowers through it, and the decoy's own unrelated row survives).
//   • "a dialect can choose the spelling" → REFUSED, here, in the direction that
//     is easiest to miss: not the same BYTES, but the same role-bound KIND under
//     other bytes. A refusal that only matched the shipped bytes would let a
//     dialect re-take ownership just by spelling it differently.
TEST(AsmTemplateToLir, ADialectMintingThePlaceholderKindUnderItsOwnBytesIsRefused) {
    for (auto const& p : {kX86, kArm}) {
        auto const decoyed = loadDialectMutated(
            p.dialect, [](nlohmann::json& doc) {
                doc["lexerModes"]["asm-template"]["tokens"]["@"] =
                    nlohmann::json::parse(R"([{"kind": "PlaceholderSigil"}])");
            });
        EXPECT_EQ(decoyed.grammar, nullptr)
            << p.dialect << ": a dialect minted the placeholder KIND under its "
               "own byte and loaded clean — the template sigils have one owner, "
               "and 'spell it differently' is the same second owner";
        auto const why = joined(decoyed.loadErrors);
        EXPECT_NE(why.find("PlaceholderSigil"), std::string::npos)
            << p.dialect << ": the refusal must name the KIND that gave it away: "
            << why;
        EXPECT_NE(why.find("inlineAsmTemplateLexemes"), std::string::npos)
            << p.dialect << ": the refusal must name the OWNER: " << why;

        // The shipped document is unaffected — the mutation is the only variable.
        auto const shipped = runTemplate(p, "nop\n", {});
        EXPECT_TRUE(shipped->ok) << p.dialect << ": " << messages(*shipped);
    }
}

// ★★★ THE ALL-OR-NOTHING RULE, EXERCISED IN EVERY DIRECTION.
// `templateLexerMode`, `templateOperandRule` and `templateLabelRule` are three
// parts of ONE capability and none does anything alone — a mode with no operand
// rule mints tokens no shape accepts, an operand rule with no mode declares a
// shape whose FIRST token nothing produces, and a surface with no label rule
// cannot tell an `asm goto` target from an operand. All three are SILENT
// failures, so the loader refuses any proper subset; this is the arm that
// proves it does.
// ⚠ THE LOOP RUNS OVER **EVERY** KEY, NOT THE TWO THAT PREDATE P20. A
// third key joined the set, and a test that dropped only the original two would
// have left the new one's half of the rule unexercised — which is exactly how a
// REQUIRE-ALL block quietly becomes a REQUIRE-MOST one.
TEST(AsmTemplateToLir, HalfATemplateSurfaceIsRefusedAtLoadNamingEveryKey) {
    // ⚠ THE SET GREW TO FIVE IN P30 AND THIS LIST HAD TO GROW WITH IT. A test
    // that kept enumerating three would still pass — dropping any of the three
    // is still refused — while asserting NOTHING about the two new members, and
    // the whole point of this pin is that a PROPER SUBSET is refused. The
    // per-arm `ASSERT_TRUE(contains)` below is what keeps a stale name from
    // making an arm vacuous.
    constexpr std::array<char const*, 5> kCapability{
        "templateLexerMode", "templateOperandRule", "templateLabelRule",
        "templateModifierRule", "templateModifiers"};
    for (auto const& p : {kX86, kArm}) {
        for (auto const* dropped : kCapability) {
            auto const half = loadDialectMutated(
                p.dialect, [dropped](nlohmann::json& doc) {
                    ASSERT_TRUE(doc["assembly"].contains(dropped))
                        << "the shipped document does not declare '" << dropped
                        << "' — this arm would be green for the wrong reason";
                    doc["assembly"].erase(dropped);
                });
            EXPECT_EQ(half.grammar, nullptr)
                << p.dialect << ": the document loaded with only part of the "
                   "template surface (dropped '" << dropped << "')";
            auto const why = joined(half.loadErrors);
            for (auto const* key : kCapability) {
                EXPECT_NE(why.find(key), std::string::npos)
                    << p.dialect << " (dropped '" << dropped
                    << "'): the refusal must name EVERY key of the set, so the "
                       "author learns what the capability actually is: " << why;
            }
        }
    }
}

// ★★★ EVERY WAY A WIDTH-VIEW DECLARATION CAN BE INCOHERENT IS REFUSED AT LOAD,
// AND EACH ARM IS **EXERCISED** RATHER THAN READ.
//
// ⚠ THE FIRST ARM IS THE ONE THAT IS NOT OBVIOUS AND IS THE REASON THIS TEST
// EXISTS. The letters are composed with the LANGUAGE's placeholder sigil, so a
// dialect declaring `l` mints the exact bytes `%l` that `asm goto`'s label sigil
// already owns. The tokenizer resolves competing lexemes by LENGTH and the two
// are the same length, so which meaning survives is decided by table iteration
// order — and BOTH outcomes are silent miscompiles (`%l2` decoded as a width
// view of operand 2, or `%l0` refused as an out-of-range label). A refusal
// naming both owners is the only honest answer.
//
// ⚠ THE WIDTH ARM IS NOT PEDANTRY EITHER: `lirInstWidthBits` states
// 8/16/32/64/128 and nothing else (the declarable set is
// `AssemblyConfig::kTemplateModifierWidthBits`, static_assert-tied to the LIR
// flags in `test_asm_class_scoped_modifiers.cpp`), so a declared 24 would be
// carried onto the instruction and read back as 64 — a wrong-width operation
// with a clean build log, which is precisely what the retired `S0067` refusal
// existed to prevent.
TEST(AsmTemplateToLir, AnIncoherentWidthViewDeclarationIsRefusedAtLoad) {
    struct Arm {
        char const* what;
        char const* modifiers;   // JSON for `assembly.templateModifiers`
        char const* mustSay;
    };
    constexpr Arm kArms[] = {
        {"a letter colliding with the label sigil",
         R"([{"letter":"l","widthBits":32}])", "'l'"},
        {"a width LIR cannot state",
         R"([{"letter":"z","widthBits":24}])", "24"},
        {"no letters at all",
         R"([])", "NON-EMPTY"},
        {"one letter, two widths",
         R"([{"letter":"z","widthBits":32},{"letter":"z","widthBits":64}])",
         "declared twice"},
        {"a letter with no width",
         R"([{"letter":"z"}])", "widthBits"},
        {"an unknown key in a row",
         R"([{"letter":"z","widthBits":32,"widthBytes":4}])", "widthBytes"},
    };
    for (auto const& p : {kX86, kArm}) {
        for (auto const& a : kArms) {
            auto const broken = loadDialectMutated(
                p.dialect, [&a](nlohmann::json& doc) {
                    doc["assembly"]["templateModifiers"] =
                        nlohmann::json::parse(a.modifiers);
                });
            EXPECT_EQ(broken.grammar, nullptr)
                << p.dialect << " (" << a.what << "): the document loaded";
            auto const why = joined(broken.loadErrors);
            EXPECT_NE(why.find(a.mustSay), std::string::npos)
                << p.dialect << " (" << a.what
                << "): the refusal must name what is wrong: " << why;
        }
    }
}

// ★★★ A `templateModifierRule` NAMING A RULE THE PLACEHOLDER FAMILY ALREADY
// USES IS REFUSED — the plausible typo, and a totally silent one. Naming the
// OPERAND rule makes the width-view arm match nothing, so every `%w0` would
// decode at the operand's own type width; naming the LABEL rule routes a width
// view to `resolveBranchTarget`, a different host virtual entirely.
TEST(AsmTemplateToLir, ATemplateModifierRuleNamingAnExistingMemberIsRefused) {
    for (auto const& p : {kX86, kArm}) {
        for (char const* clash : {"asmTemplateOperand", "asmTemplateLabelRef"}) {
            auto const strayed = loadDialectMutated(
                p.dialect, [clash](nlohmann::json& doc) {
                    doc["assembly"]["templateModifierRule"] = clash;
                });
            EXPECT_EQ(strayed.grammar, nullptr)
                << p.dialect << ": 'templateModifierRule' = '" << clash
                << "' loaded clean — the width-view arm is then dead or "
                   "mis-routed and nothing says so";
            EXPECT_NE(joined(strayed.loadErrors).find("templateModifierRule"),
                      std::string::npos)
                << p.dialect << ": " << joined(strayed.loadErrors);
        }
    }
}

// ★★★ A `templateLabelRule` THAT NAMES A RULE THE PLACEHOLDER RULE CANNOT
// BEGIN WITH IS REFUSED AT LOAD — the silent no-op the key exists to prevent.
//
// ⚠ THE MUTATION IS THE PLAUSIBLE TYPO, NOT AN ABSURD ONE: the two placeholder
// families SHARE the bracketed symbolic-name rule, so naming it here is the
// mistake a dialect author would actually make. Its effect is total and silent
// — `decodePlaceholder` matches the label rule against the placeholder's own
// child, that child is never the shared name rule, and every `asm goto` target
// would decode as an operand and be refused as an unbound register. A true
// diagnostic, pointing at the wrong thing.
TEST(AsmTemplateToLir, ATemplateLabelRuleOutsideThePlaceholderFamilyIsRefused) {
    for (auto const& p : {kX86, kArm}) {
        auto const strayed = loadDialectMutated(
            p.dialect, [](nlohmann::json& doc) {
                doc["assembly"]["templateLabelRule"] = "asmTemplateSymbolicName";
            });
        EXPECT_EQ(strayed.grammar, nullptr)
            << p.dialect << ": a 'templateLabelRule' naming a rule outside the "
               "placeholder family loaded clean — the label form is then dead "
               "and nothing says so";
        auto const why = joined(strayed.loadErrors);
        EXPECT_NE(why.find("templateLabelRule"), std::string::npos)
            << p.dialect << ": " << why;

        // ★ AND THE SAME-RULE MISTAKE, which FIRST-set containment cannot see
        // (a set is trivially a subset of itself) and which is therefore its
        // own arm rather than a variant of the one above.
        auto const same = loadDialectMutated(
            p.dialect, [](nlohmann::json& doc) {
                doc["assembly"]["templateLabelRule"] =
                    doc["assembly"]["templateOperandRule"];
            });
        EXPECT_EQ(same.grammar, nullptr)
            << p.dialect << ": 'templateLabelRule' naming the same rule as "
               "'templateOperandRule' loaded clean — the label form matches "
               "nothing and is silently dead";
        EXPECT_NE(joined(same.loadErrors).find("templateOperandRule"),
                  std::string::npos)
            << p.dialect << ": the refusal must name the rule it collided with: "
            << joined(same.loadErrors);

        // The shipped document is unaffected — the mutations are the only
        // variables.
        auto const shipped = runTemplate(p, "nop\n", {});
        EXPECT_TRUE(shipped->ok) << p.dialect << ": " << messages(*shipped);
    }
}

// ★★★ A `templateOperandRule` THAT `operandForms` ALSO BINDS IS REFUSED AT LOAD,
// AND THE DEFECT IT CATCHES IS THE QUIETEST ONE ON THIS SURFACE.
//
// ✔TRACED, and the trace is the whole reason the check exists:
// `decodeOperand`'s placeholder test lives INSIDE its `while (mask == 0)`
// descent, so a placeholder rule that carries an operand ROLE makes
// `rolesForRule` non-zero, the loop body never runs, and the placeholder branch
// is DEAD CODE. Every `%0` would then decode as whatever role the dialect bound
// — the key accepted, the surface apparently declared, and nothing
// placeholder-shaped ever happening.
// ⚠ THE MUTATION BINDS THE ROLE THAT IS **NOT** `register`, deliberately: the
// undecidable-pair check next door already refuses two non-register roles on one
// rule, so aliasing onto `immediate` (whose shipped rule is `attImmediate`) is a
// mutation only THIS check can catch.
TEST(AsmTemplateToLir, ATemplateOperandRuleBoundToAnOperandRoleIsRefusedAtLoad) {
    for (auto const& p : {kX86, kArm}) {
        auto const aliased = loadDialectMutated(
            p.dialect, [](nlohmann::json& doc) {
                auto const rule =
                    doc["assembly"]["templateOperandRule"].get<std::string>();
                doc["assembly"]["operandForms"]["immediate"] = rule;
            });
        EXPECT_EQ(aliased.grammar, nullptr)
            << p.dialect << ": the placeholder rule was accepted as an "
               "`operandForms` rule too — the placeholder branch is then "
               "unreachable and the whole key is a silent no-op";
        auto const why = joined(aliased.loadErrors);
        EXPECT_NE(why.find("templateOperandRule"), std::string::npos)
            << p.dialect << ": " << why;
        EXPECT_NE(why.find("immediate"), std::string::npos)
            << p.dialect << ": the refusal must name the ROLE that collided: "
            << why;
    }
}

TEST(AsmTemplateToLir, ATemplateLexerModeNamingNothingIsRefusedAtLoad) {
    auto const bogus = loadDialectMutated(
        kX86.dialect, [](nlohmann::json& doc) {
            doc["assembly"]["templateLexerMode"] = "no-such-mode";
        });
    EXPECT_EQ(bogus.grammar, nullptr)
        << "a 'templateLexerMode' naming no declared mode loaded clean — every "
           "template would then be lexed in `main`, i.e. as a `.s`";
    EXPECT_NE(joined(bogus.loadErrors).find("no-such-mode"), std::string::npos)
        << joined(bogus.loadErrors);
}

// ⚠ A BODY MODE HAS A `defaultToken` AND THE TOKENIZER NEVER REACHES THE
// PER-MODE LEXEME LOOKUP IN ONE — it scans a codepoint at a time to the mode's
// `endsAt`. Naming one here would lex an entire template as comment characters
// and produce not one placeholder, silently. The dialect already ships two body
// modes, so this is not a hypothetical shape.
TEST(AsmTemplateToLir, ATemplateLexerModeThatIsABodyModeIsRefusedAtLoad) {
    auto const body = loadDialectMutated(
        kX86.dialect, [](nlohmann::json& doc) {
            doc["assembly"]["templateLexerMode"] = "line-comment";
        });
    EXPECT_EQ(body.grammar, nullptr)
        << "a BODY mode was accepted as the template lexer mode";
    auto const why = joined(body.loadErrors);
    EXPECT_NE(why.find("line-comment"), std::string::npos) << why;
    EXPECT_NE(why.find("defaultToken"), std::string::npos)
        << "the refusal must say WHY a body mode cannot serve: " << why;
}

// ★★★ AND A DIALECT THAT IMPORTS THE SHARED `.s` SURFACE CANNOT DROP THE
// TEMPLATE CAPABILITY — WHICH IS A REAL CONTRACT CHANGE AT P5c, AND IS RECORDED
// AS ONE RATHER THAN GLOSSED (2026-08-17,
// D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER).
//
// ⓘ WHAT THIS TEST USED TO ASSERT AND WHY IT INVERTED. It said "a dialect that
// declares NEITHER key is legal — it hosts no embedded templates", loaded such a
// document, and then checked that an EXTENDED template parse was refused BY NAME
// while a BASIC one still succeeded. ✔MEASURED that the legality half was true
// only BECAUSE of the defect this row closed: the template placeholder KINDS
// (`PlaceholderSigil`, `PlaceholderLabelSigil`, `PlaceholderNameOpen`/`Close`,
// `PercentEscape`) were interned by the dialect's OWN `asm-template` rows, so
// they existed whether or not the capability was declared. With the rows
// synthesized from the language's role set, the only thing that interns them is
// the capability — and `asm.lang.json`'s `asmLine` closure SPELLS all four holes
// (`asmTemplateOperandRef`, `asmTemplateLabelRef`, `asmTemplateSymbolicName` are
// reachable from `asmOperandItem`), so the bindings are mandatory and now point
// at kinds nothing declares.
// ⇒ For a dialect importing that closure the capability was never really
// optional; "optional" was the duplication wearing a costume. The refusal is
// precise and names every kind, which is the fail-loud half.
// ⚠ WHAT IS LOST, STATED PLAINLY: the runtime arm — `parseAsmTemplateText`
// refusing an EXTENDED template BY NAME on a surface-less dialect, and accepting
// a BASIC one — is no longer constructible from a shipped dialect, because no
// shipped dialect can be surface-less. That guard is still in the code and still
// correct for a dialect that imports a NARROWER entry (one whose closure does not
// reach the template shapes); it is now unexercised, and that is anchored rather
// than deleted.
// ★ TWO STAGES, BECAUSE THE FIRST REFUSAL SHORT-CIRCUITS THE SECOND — and that
// ordering is itself measured rather than assumed. Erasing the capability alone
// orphans the `templateEscape` BINDING, which the merge refuses and which drops
// the whole reference, so the placeholder-kind refusals never get a chance to
// speak. Stage 2 removes that binding too and reads what is underneath.
TEST(AsmTemplateToLir, ADialectImportingTheSharedSurfaceCannotDropTheTemplateCapability) {
    // ── stage 1: the capability alone ──
    // `templateEscape` is spelled by NO shared shape — only by the language's
    // role set, and only for a document that declares the capability — so keeping
    // the binding with the capability gone is a binding that configures nothing,
    // and this loader never lets that pass in silence.
    auto const capOnly = loadDialectMutated(
        kX86.dialect, [](nlohmann::json& doc) {
            doc["assembly"].erase("templateLexerMode");
            doc["assembly"].erase("templateOperandRule");
            doc["assembly"].erase("templateLabelRule");
        });
    EXPECT_EQ(capOnly.grammar, nullptr)
        << "a dialect dropped the template capability and kept its role "
           "bindings, and loaded clean";
    auto const why1 = joined(capOnly.loadErrors);
    EXPECT_NE(why1.find("templateEscape"), std::string::npos)
        << "the orphaned role binding must be named: " << why1;
    EXPECT_NE(why1.find("PercentEscape"), std::string::npos)
        << "the dialect's OWN consumer of the synthesized row must be named — "
           "`attRegister`'s escaped-register arm is what actually breaks: "
        << why1;

    // ── stage 2: the capability AND the role binding ──
    // Now the four template holes the shared `asmLine` closure SPELLS
    // (`asmTemplateOperandRef`, `asmTemplateLabelRef`,
    // `asmTemplateSymbolicName`) are bound to kinds nothing interns, because the
    // synthesized mode was the only thing that declared them.
    auto const alsoBinding = loadDialectMutated(
        kX86.dialect, [](nlohmann::json& doc) {
            doc["assembly"].erase("templateLexerMode");
            doc["assembly"].erase("templateOperandRule");
            doc["assembly"].erase("templateLabelRule");
            std::size_t erased = 0;
            for (auto& [_, ref] : doc["languageReferences"].items()) {
                if (!ref.is_object() || !ref.contains("bindTokens")) continue;
                erased += ref["bindTokens"].erase("templateEscape");
            }
            ASSERT_EQ(erased, 1u);
        });
    EXPECT_EQ(alsoBinding.grammar, nullptr)
        << "a dialect importing the shared `.s` surface loaded with NO template "
           "capability — its template placeholder kinds would then be interned "
           "by nothing, so the shared template shapes resolve against nothing";
    auto const why2 = joined(alsoBinding.loadErrors);
    for (auto const* kind : {"PlaceholderSigil", "PlaceholderLabelSigil",
                             "PlaceholderNameOpen", "PlaceholderNameClose"}) {
        EXPECT_NE(why2.find(kind), std::string::npos)
            << "the refusal must name the undeclared kind '" << kind << "' — the "
               "author has to know WHICH declarations went away with the "
               "capability, not merely that one did: " << why2;
    }

    // The shipped document is unaffected — the erases are the only variables.
    auto const shipped = runTemplate(kX86, "nop\n", {});
    EXPECT_TRUE(shipped->ok) << messages(*shipped);
}

// ★★★ THE BASIC/EXTENDED SPLIT IS A **LEXER** SPLIT, AND THIS IS THE ARM THAT
// PROVES IT RATHER THAN RESTATING THE MEASUREMENT. ✔MEASURED on gcc 13.3.0 and
// clang 18.1.3 (sources fed as base64, with matched positive controls):
//   `__asm__("xorl %eax,%eax")`             BASIC    → BOTH compile, `%` literal
//   `__asm__("xorl %eax,%eax" ::: "eax")`   EXTENDED → BOTH reject
//   `__asm__("xorl %%eax,%%eax" ::: "eax")` EXTENDED → BOTH compile
// So the SAME four bytes must be read two ways depending on whether the
// statement has any operand section at all — and that is a property of the
// CONSTRUCT, which is why the surface is a parameter of the parse entry rather
// than a property of the dialect.
// ⚠ THE PAIR IS THE INSTRUMENT. Asserting only that `%rax` works in a basic
// template would stay green if the extended surface were never selected at all.
TEST(AsmTemplateToLir, TheBasicAndExtendedSurfacesReadOnePercentDifferently) {
    // BASIC: `%rax` IS the register, exactly as in a `.s`.
    auto const basic = runTemplate(kX86, "movq %rax, %rcx\n", {}, {},
                                   AsmTemplateSurface::Basic);
    ASSERT_TRUE(basic->parsed)
        << "a BASIC template refused `%rax` — it is being read on the extended "
           "surface, and gcc and clang both compile this:\n"
        << messages(*basic);
    EXPECT_TRUE(basic->ok) << messages(*basic);

    // EXTENDED: the same four bytes are an operand MODIFIER and must be
    // refused; the doubled form is how a register is named.
    auto const extended = runTemplate(kX86, "movq %rax, %rcx\n", {}, {},
                                      AsmTemplateSurface::Extended);
    EXPECT_FALSE(extended->parsed && extended->ok)
        << "an EXTENDED template ACCEPTED `%rax` — it is being read on the `.s` "
           "surface, and gcc and clang both REJECT this";

    auto const escaped = runTemplate(kX86, "movq %%rax, %%rcx\n", {}, {},
                                     AsmTemplateSurface::Extended);
    ASSERT_TRUE(escaped->parsed) << messages(*escaped);
    ASSERT_TRUE(escaped->ok) << messages(*escaped);

    // ★ AND THE TWO READINGS REACH THE SAME INSTRUCTION, which is what makes
    // the split a LEXICAL one rather than two different features.
    EXPECT_EQ(basic->bytes, escaped->bytes)
        << "basic `%rax` and extended `%%rax` did not encode identically — "
           "they are the same register written two ways";

    // ...and a placeholder is unavailable on the basic surface, which is the
    // lexical mirror of the same fact (`%0` is literal text to gcc there, and
    // the C tier refuses it as S_InlineAsmPlaceholderInBasicTemplate one stage
    // earlier — this arm pins that the DIALECT does not quietly accept it).
    auto const placeholderInBasic =
        runTemplate(kX86, "movq %1, %0\n", {"%0", "%1"}, {},
                    AsmTemplateSurface::Basic);
    EXPECT_FALSE(placeholderInBasic->parsed && placeholderInBasic->ok)
        << "a BASIC template bound a `%0` placeholder — the basic surface must "
           "not have one";
}
