// GNU LOCAL REGISTER VARIABLES — `register long x8 __asm__("x8") = 93;` — AND
// THE INLINE-ASM OPERAND THAT READS OR WRITES ONE.
//
//   D-C-LOCAL-REGISTER-VARIABLE-ASM-LABEL-IGNORED
//   D-ASM-MATCHING-CONSTRAINT-DIGIT-READ-AS-A-MACHINE-LETTER
//
// ★★★ THE DEFECT, ✔MEASURED 2026-09-19 THROUGH THE SHIPPED CLI at the P68 round-7
// base: the asm label of a `register` local was IGNORED (the automatic-variable
// warn-and-drop path), so every register-bound operand — the whole shape of a
// Linux syscall wrapper — compiled rc=0 and ran with the value in whatever
// register the allocator chose. gcc 13.3.0 and clang 18.1.3, probed SEPARATELY
// on both shipped targets at -O0 and -O2, bind it (📄 GCC manual, "Local
// Register Variables": *"The only supported use for this feature is to specify
// registers for input and output operands when calling Extended asm"*).
//
// ★★ THE ARMS, EACH A STATEMENT ABOUT THE TIER THAT DECIDES IT, EACH BESIDE A
// NAMED CONTROL:
//   (A) the binding: a register-form operand whose value IS the variable is
//       pinned to its register (HIR → MIR `fixedRegister`, `pinnedByVariable`),
//       and the LIR copies the value into that PHYSICAL register; the control is
//       the same label without `register`, which stays a warn-and-drop.
//   (B) a GNU matching constraint (`"0"`) TIES the input to its output instead
//       of being read as a machine letter, at the INPUT's own width.
//   (C) an input whose variable is NEVER written reads its register AS IT
//       STANDS — nothing is copied in (the `sp` read idiom); the controls are an
//       initialized variable and one also read outside asm.
//   (D) a copy INTO a bound stack pointer is elected by the register-role axis
//       (`sp_copy`), never the class move that reads field 31 as `xzr`.
//   (E) a 16-byte variable continues in the register its target DECLARES
//       (`continuesIn`) — x86-64 `rdx` continues in `rcx`, which no encoding
//       arithmetic yields; a register that continues in nothing refuses by name.
//   (F) a leaf that binds the LINK register saves it (the emitted frame).
//   (G) `svc #imm` is the target's supervisor call with its immediate.
//   (H) every refusal the references share, by CODE, beside an accepted
//       neighbour; and `continuesIn`'s load-time contract.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe`.

#include "asm_region_test_support.hpp"
#include "lowered_lir_fixture.hpp"
#include "mutate_target_schema.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_wide_call_args.hpp"
#include "mir/mir.hpp"
#include "mir/mir_asm_descriptor.hpp"
#include "mir/mir_opcode.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support;

namespace {

[[nodiscard]] std::string summarize(LoweredLir const& r) {
    std::string s;
    for (auto const& d : r.model.diagnostics().all()) s += "\n  sem: " + d.actual;
    for (auto const& d : r.hirReporter.all()) s += "\n  hir: " + d.actual;
    for (auto const& d : r.mirReporter.all()) s += "\n  mir: " + d.actual;
    for (auto const& d : r.lirReporter.all()) s += "\n  lir: " + d.actual;
    return s.empty() ? std::string{"<no diagnostics>"} : s;
}

// Does ANY tier — the semantic model included — report `code`?
[[nodiscard]] bool anyCode(LoweredLir const& r, DiagnosticCode code) {
    for (auto const* rep : {&r.model.diagnostics(), &r.hirReporter,
                            &r.mirReporter, &r.lirReporter}) {
        for (auto const& d : rep->all()) {
            if (d.code == code) return true;
        }
    }
    return false;
}

[[nodiscard]] bool anyText(LoweredLir const& r, std::string_view needle) {
    for (auto const* rep : {&r.model.diagnostics(), &r.hirReporter,
                            &r.mirReporter, &r.lirReporter}) {
        for (auto const& d : rep->all()) {
            if (d.actual.find(needle) != std::string::npos) return true;
        }
    }
    return false;
}

[[nodiscard]] bool anyError(LoweredLir const& r) {
    return r.model.diagnostics().hasErrors() || r.hirReporter.hasErrors()
        || r.mirReporter.hasErrors() || r.lirReporter.hasErrors();
}

// The ONE asm statement of a snippet.
[[nodiscard]] MirInstId theAsm(Mir const& mir) {
    std::vector<MirInstId> found;
    for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
                MirInstId const id = mir.blockInstAt(b, ii);
                if (mir.instOpcode(id) == MirOpcode::InlineAsm) found.push_back(id);
            }
        }
    }
    EXPECT_EQ(found.size(), 1u) << "the snippet carries exactly one asm statement";
    return found.empty() ? InvalidMirInst : found.front();
}

[[nodiscard]] std::vector<LirInstId> allInsts(Lir const& lir) {
    std::vector<LirInstId> out;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const b = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
                out.push_back(lir.blockInstAt(b, ii));
            }
        }
    }
    return out;
}

[[nodiscard]] std::uint16_t ordinalOf(TargetSchema const& t, std::string_view name) {
    auto const o = t.registerByName(name);
    EXPECT_TRUE(o.has_value()) << "target declares no register '" << name << "'";
    return o.has_value() ? *o : std::uint16_t{0xFFFF};
}

// Every instruction whose RESULT is the physical register `ordinal`.
[[nodiscard]] std::vector<LirInstId> writersOf(Lir const& lir, std::uint16_t ordinal) {
    std::vector<LirInstId> out;
    for (LirInstId const i : allInsts(lir)) {
        LirReg const r = lir.instResult(i);
        if (r.valid() && r.isPhysical != 0 && r.id == ordinal) out.push_back(i);
    }
    return out;
}

[[nodiscard]] std::string mnemonicOf(TargetSchema const& t, Lir const& lir, LirInstId i) {
    auto const* info = t.opcodeInfo(lir.instOpcode(i));
    return info != nullptr ? info->mnemonic : std::string{"<?>"};
}

}  // namespace

// ── (A) THE BINDING ─────────────────────────────────────────────────────────

TEST(LirAsmRegisterVariable, ABoundInputIsPinnedToItsRegisterAndCopiedIntoIt) {
    auto r = lowerCToLir(
        "long f(long k) {\n"
        "    register long v __asm__(\"x9\") = k;\n"
        "    long out;\n"
        "    __asm__ volatile(\"add %0, x9, #2\" : \"=r\"(out) : \"r\"(v));\n"
        "    return out;\n"
        "}\n",
        "arm64");
    ASSERT_FALSE(anyError(r)) << summarize(r);
    MirInstId const a = theAsm(r.mir.mir);
    ASSERT_TRUE(a.valid());
    MirAsmDescriptor const& d = r.mir.mir.asmDescriptor(a);
    ASSERT_EQ(d.inputs.size(), 1u);
    EXPECT_EQ(d.inputs[0].fixedRegister, "x9")
        << "the variable's label names the operand's register";
    EXPECT_TRUE(d.inputs[0].pinnedByVariable);
    EXPECT_FALSE(d.inputs[0].registerAsItStands)
        << "an INITIALIZED variable has a value, and it is copied in";
    // The LIR copies the value INTO the physical register the template names.
    EXPECT_FALSE(writersOf(r.lir.lir, ordinalOf(*r.target, "x9")).empty())
        << "no instruction writes x9 — the binding was dropped" << summarize(r);
}

TEST(LirAsmRegisterVariable, TheSameLabelWithoutRegisterIsAWarningAndNoBinding) {
    // THE CONTROL: an automatic variable with an asm label and NO `register`
    // keeps the warn-and-drop both references give it — so (A)'s pin is the
    // `register` facet's doing, not the label's.
    auto r = lowerCToLir(
        "long f(long k) {\n"
        "    long v __asm__(\"x9\") = k;\n"
        "    long out;\n"
        "    __asm__ volatile(\"add %0, %1, #2\" : \"=r\"(out) : \"r\"(v));\n"
        "    return out;\n"
        "}\n",
        "arm64");
    ASSERT_FALSE(anyError(r)) << summarize(r);
    EXPECT_TRUE(anyCode(r, DiagnosticCode::S_AsmLabelOnAutomaticVariable))
        << summarize(r);
    MirAsmDescriptor const& d = r.mir.mir.asmDescriptor(theAsm(r.mir.mir));
    ASSERT_EQ(d.inputs.size(), 1u);
    EXPECT_TRUE(d.inputs[0].fixedRegister.empty());
    EXPECT_FALSE(d.inputs[0].pinnedByVariable);
}

TEST(LirAsmRegisterVariable, AnOutputAndAReadWriteOperandBindTheirRegisters) {
    auto r = lowerCToLir(
        "long f(long k) {\n"
        "    register long o __asm__(\"r11\");\n"
        "    register long io __asm__(\"r12\") = k;\n"
        "    __asm__ volatile(\"movq $42, %%r11\\n\\taddq $1, %%r12\""
        " : \"=r\"(o), \"+r\"(io));\n"
        "    return o + io;\n"
        "}\n",
        "x86_64");
    ASSERT_FALSE(anyError(r)) << summarize(r);
    MirAsmDescriptor const& d = r.mir.mir.asmDescriptor(theAsm(r.mir.mir));
    ASSERT_EQ(d.outputs.size(), 2u);
    EXPECT_EQ(d.outputs[0].fixedRegister, "r11");
    EXPECT_EQ(d.outputs[1].fixedRegister, "r12");
    // the synthesized read half of `+` shares the output's register
    bool tiedReadPinned = false;
    for (auto const& in : d.inputs) {
        if (in.tiedOutput.has_value() && *in.tiedOutput == 1u
            && in.fixedRegister == "r12") {
            tiedReadPinned = true;
        }
    }
    EXPECT_TRUE(tiedReadPinned) << "the `+` operand's read half reads r12 too";
}

// ── (B) A MATCHING CONSTRAINT IS A TIE ───────────────────────────────────────

TEST(LirAsmRegisterVariable, AMatchingConstraintTiesTheInputToTheOutputItNames) {
    auto r = lowerCToLir(
        "long f(long a, long b) {\n"
        "    long out;\n"
        "    __asm__(\"add %0, %0, %2\" : \"=r\"(out) : \"0\"(a), \"r\"(b));\n"
        "    return out;\n"
        "}\n",
        "arm64");
    ASSERT_FALSE(anyError(r)) << summarize(r);
    MirAsmDescriptor const& d = r.mir.mir.asmDescriptor(theAsm(r.mir.mir));
    ASSERT_EQ(d.outputs.size(), 1u);
    ASSERT_EQ(d.inputs.size(), 2u);
    ASSERT_TRUE(d.inputs[0].tiedOutput.has_value())
        << "\"0\" names output 0; it was read as a machine letter";
    EXPECT_EQ(*d.inputs[0].tiedOutput, 0u);
    EXPECT_TRUE(d.inputs[0].matchesOutput)
        << "a WRITTEN matching constraint keeps its own width and spellings";
    EXPECT_FALSE(d.inputs[0].spellings.empty())
        << "the source wrote it, so its `%1` names it";
    EXPECT_FALSE(d.inputs[1].tiedOutput.has_value())
        << "the control: a letter-bound input is no tie";
    EXPECT_TRUE(d.outputs[0].isReadWrite)
        << "the matched output is now one half of a read-write pair";
}

TEST(LirAsmRegisterVariable, AMatchingConstraintKeepsTheInputsOwnWidth) {
    // ✔MEASURED, both references, both targets: an `int` input matched to a
    // `long` output is materialized at ITS width — a tie at the output's width
    // would be refused here as a `+` width disagreement.
    auto r = lowerCToLir(
        "long f(int a) {\n"
        "    long out;\n"
        "    __asm__(\"mov %0, %0\" : \"=r\"(out) : \"0\"(a));\n"
        "    return out;\n"
        "}\n",
        "arm64");
    ASSERT_FALSE(anyError(r)) << summarize(r);
    MirAsmDescriptor const& d = r.mir.mir.asmDescriptor(theAsm(r.mir.mir));
    ASSERT_EQ(d.inputs.size(), 1u);
    EXPECT_TRUE(d.inputs[0].matchesOutput);
}

// ── (C) AN UNWRITTEN VARIABLE IS READ AS IT STANDS ──────────────────────────

TEST(LirAsmRegisterVariable, AnUnwrittenVariableIsReadAsItStandsWithNoCopy) {
    auto r = lowerCToLir(
        "unsigned long f(void) {\n"
        "    register unsigned long sp __asm__(\"sp\");\n"
        "    unsigned long out;\n"
        "    __asm__ volatile(\"mov %0, %1\" : \"=r\"(out) : \"r\"(sp));\n"
        "    return out;\n"
        "}\n",
        "arm64");
    ASSERT_FALSE(anyError(r)) << summarize(r);
    MirAsmDescriptor const& d = r.mir.mir.asmDescriptor(theAsm(r.mir.mir));
    ASSERT_EQ(d.inputs.size(), 1u);
    EXPECT_EQ(d.inputs[0].fixedRegister, "sp");
    EXPECT_TRUE(d.inputs[0].registerAsItStands)
        << "no initializer and no use but this input: the variable IS sp";
    EXPECT_TRUE(writersOf(r.lir.lir, ordinalOf(*r.target, "sp")).empty())
        << "nothing may be copied INTO sp — the variable's slot holds an "
           "indeterminate value (✔MEASURED exit 139 when it was)";
}

TEST(LirAsmRegisterVariable, AnInitializedOrOtherwiseUsedVariableIsCopiedIn) {
    // CONTROL 1: an initializer is a write, so the value is copied in.
    auto init = lowerCToLir(
        "unsigned long f(unsigned long k) {\n"
        "    register unsigned long v __asm__(\"x10\") = k;\n"
        "    unsigned long out;\n"
        "    __asm__ volatile(\"mov %0, %1\" : \"=r\"(out) : \"r\"(v));\n"
        "    return out;\n"
        "}\n",
        "arm64");
    ASSERT_FALSE(anyError(init)) << summarize(init);
    EXPECT_FALSE(init.mir.mir.asmDescriptor(theAsm(init.mir.mir))
                     .inputs[0].registerAsItStands);
    // CONTROL 2: a use that is not a bare asm input (here an ordinary read)
    // disqualifies the proof — the variable is then an ordinary automatic.
    auto used = lowerCToLir(
        "unsigned long f(void) {\n"
        "    register unsigned long v __asm__(\"x10\");\n"
        "    unsigned long out;\n"
        "    __asm__ volatile(\"mov %0, %1\" : \"=r\"(out) : \"r\"(v));\n"
        "    return out + v;\n"
        "}\n",
        "arm64");
    ASSERT_FALSE(anyError(used)) << summarize(used);
    EXPECT_FALSE(used.mir.mir.asmDescriptor(theAsm(used.mir.mir))
                     .inputs[0].registerAsItStands);
    // CONTROL 3: an assignment is a write even when the only READ is the input.
    auto assigned = lowerCToLir(
        "unsigned long f(unsigned long k) {\n"
        "    register unsigned long v __asm__(\"x10\");\n"
        "    unsigned long out;\n"
        "    v = k;\n"
        "    __asm__ volatile(\"mov %0, %1\" : \"=r\"(out) : \"r\"(v));\n"
        "    return out;\n"
        "}\n",
        "arm64");
    ASSERT_FALSE(anyError(assigned)) << summarize(assigned);
    EXPECT_FALSE(assigned.mir.mir.asmDescriptor(theAsm(assigned.mir.mir))
                     .inputs[0].registerAsItStands);
}

// ── (D) A COPY INTO sp IS ELECTED ───────────────────────────────────────────

TEST(LirAsmRegisterVariable, ACopyIntoABoundStackPointerIsTheStackPointerCopy) {
    auto r = lowerCToLir(
        "unsigned long f(unsigned long k) {\n"
        "    register unsigned long sp __asm__(\"sp\") = k;\n"
        "    unsigned long out;\n"
        "    __asm__ volatile(\"mov %0, %1\" : \"=r\"(out) : \"r\"(sp));\n"
        "    return out;\n"
        "}\n",
        "arm64");
    ASSERT_FALSE(anyError(r)) << summarize(r);
    auto const writers = writersOf(r.lir.lir, ordinalOf(*r.target, "sp"));
    ASSERT_EQ(writers.size(), 1u)
        << "the initialized variable's value is copied into sp exactly once";
    EXPECT_EQ(mnemonicOf(*r.target, r.lir.lir, writers.front()), "sp_copy")
        << "the class `move` (ORR) reads field 31 as xzr — ✔MEASURED `mov xzr, "
           "x15`, a no-op — and the role axis must elect the ADD form";
    // THE CONTROL: an ordinary register keeps the class move.
    auto x = lowerCToLir(
        "unsigned long f(unsigned long k) {\n"
        "    register unsigned long v __asm__(\"x10\") = k;\n"
        "    unsigned long out;\n"
        "    __asm__ volatile(\"mov %0, %1\" : \"=r\"(out) : \"r\"(v));\n"
        "    return out;\n"
        "}\n",
        "arm64");
    ASSERT_FALSE(anyError(x)) << summarize(x);
    auto const xw = writersOf(x.lir.lir, ordinalOf(*x.target, "x10"));
    ASSERT_FALSE(xw.empty());
    EXPECT_EQ(mnemonicOf(*x.target, x.lir.lir, xw.front()), "mov");
}

// ── (E) A 16-BYTE VARIABLE CONTINUES IN THE DECLARED REGISTER ───────────────

TEST(LirAsmRegisterVariable, ASixteenByteVariableContinuesInTheDeclaredRegister) {
    struct Case { char const* target; char const* first; char const* second; };
    // x86-64 rdx continues in rcx: gcc's register order, NOT encoding order
    // (rdx's hwEncoding is 2, rbx's 3) — the arithmetic reading would say rbx.
    for (Case const c : {Case{"arm64", "x4", "x5"}, Case{"arm64", "x7", "x8"},
                         Case{"x86_64", "rax", "rdx"},
                         Case{"x86_64", "rdx", "rcx"}}) {
        std::string const tmpl = std::string{c.target} == "arm64"
            ? "add %0, " + std::string{c.first} + ", " + c.second
            : "leaq (%%" + std::string{c.first} + ",%%" + c.second + "), %0";
        auto r = lowerCToLir(
            "long f(__int128 k) {\n"
            "    register __int128 v __asm__(\"" + std::string{c.first} + "\") = k;\n"
            "    long out;\n"
            "    __asm__ volatile(\"" + tmpl + "\" : \"=r\"(out) : \"r\"(v));\n"
            "    return out;\n"
            "}\n",
            c.target);
        ASSERT_FALSE(anyError(r)) << c.first << summarize(r);
        EXPECT_FALSE(writersOf(r.lir.lir, ordinalOf(*r.target, c.first)).empty())
            << c.first << ": the low half is loaded into the bound register";
        EXPECT_FALSE(writersOf(r.lir.lir, ordinalOf(*r.target, c.second)).empty())
            << c.first << ": the high half must land in '" << c.second
            << "', the register the target declares it continues in";
    }
}

TEST(LirAsmRegisterVariable, ARegisterThatContinuesInNothingRefusesAPairByName) {
    // ✔MEASURED: gcc refuses `__int128` on x30; clang ACCEPTS it and silently
    // drops the high half. The control is (E): the same shape on x4 lowers.
    auto r = lowerCToLir(
        "long f(__int128 k) {\n"
        "    register __int128 v __asm__(\"x30\") = k;\n"
        "    long out;\n"
        "    __asm__ volatile(\"mov %0, x30\" : \"=r\"(out) : \"r\"(v));\n"
        "    return out;\n"
        "}\n",
        "arm64", 0, LoweringExpectation::Refuses);
    EXPECT_TRUE(anyCode(r, DiagnosticCode::L_UnsupportedLoweringForOpcode))
        << summarize(r);
    EXPECT_TRUE(anyText(r, "continuesIn")) << summarize(r);
}

TEST(LirAsmRegisterVariable, APairOverlappingAnotherBoundInputIsRefused) {
    auto r = lowerCToLir(
        "long f(__int128 k, long j) {\n"
        "    register __int128 v __asm__(\"x4\") = k;\n"
        "    register long w __asm__(\"x5\") = j;\n"
        "    long out;\n"
        "    __asm__ volatile(\"add %0, x4, x5\" : \"=r\"(out) : \"r\"(v), \"r\"(w));\n"
        "    return out;\n"
        "}\n",
        "arm64", 0, LoweringExpectation::Refuses);
    EXPECT_TRUE(anyText(r, "overlap")) << summarize(r);
}

TEST(LirAsmRegisterVariable, ALetterPinnedPairIsStillRefused) {
    // A LETTER pin never continues: both references refuse `"a"(__int128)`.
    auto r = lowerCToLir(
        "long f(__int128 k) {\n"
        "    long out;\n"
        "    __asm__ volatile(\"movq %%rax, %0\" : \"=r\"(out) : \"a\"(k));\n"
        "    return out;\n"
        "}\n",
        "x86_64", 0, LoweringExpectation::Refuses);
    EXPECT_TRUE(anyText(r, "pins ONE")) << summarize(r);
}

// ── (F) A LEAF THAT BINDS THE LINK REGISTER SAVES IT ────────────────────────

namespace {

// The compile pipeline's own order from MIR→LIR to the emitted frame.
[[nodiscard]] std::optional<Lir> emitted(std::string const& src,
                                         std::string const& targetName) {
    auto target = TargetSchema::loadShipped(targetName);
    if (!target) { ADD_FAILURE() << "loadShipped failed"; return std::nullopt; }
    std::shared_ptr<TargetSchema> schema = std::move(*target);
    auto lowered = lowerCToLir(src, schema, 0);
    if (!lowered.lir.ok) { ADD_FAILURE() << "MIR->LIR refused"; return std::nullopt; }
    DiagnosticReporter rep;
    auto wide = lowerWideCallArgs(lowered.lir.lir, *schema, 0, rep);
    if (!wide.ok) { ADD_FAILURE() << "wide-call lowering refused"; return std::nullopt; }
    auto const liveness = analyzeLiveness(wide.lir);
    auto const alloc = allocateRegisters(wide.lir, *schema, liveness, 0, rep);
    if (!alloc.ok()) { ADD_FAILURE() << "allocation refused"; return std::nullopt; }
    auto rewritten = rewriteWithAllocation(wide.lir, *schema, alloc, rep);
    if (!rewritten.ok) { ADD_FAILURE() << "rewrite refused"; return std::nullopt; }
    auto legal = legalizeTwoAddress(rewritten.lir, *schema, rep);
    if (!legal.ok()) { ADD_FAILURE() << "legalize refused"; return std::nullopt; }
    // P68 round 8 part 4: the peephole, then the inline-asm bundles become their
    // bodies — the order `compile_pipeline` runs, so callconv sees the
    // template's own instructions (and every register they name) exactly as
    // the real build does.
    auto peeped = runLirPeephole(legal.lir, *schema, rep);
    if (!peeped.ok()) { ADD_FAILURE() << "peephole refused"; return std::nullopt; }
    auto expanded = expandAsmRegions(peeped.lir, *schema, rep);
    if (!expanded.ok) { ADD_FAILURE() << "asm-region expansion refused"; return std::nullopt; }
    auto cc = materializeCallingConvention(expanded.lir, *schema, alloc, rep);
    if (!cc.ok()) { ADD_FAILURE() << "callconv refused"; return std::nullopt; }
    EXPECT_EQ(rep.errorCount(), 0u);
    return std::move(cc.lir);
}

// Is the physical register `ordinal` STORED (read as an operand by an
// instruction producing no result — the prologue's save)?
[[nodiscard]] bool isStored(Lir const& lir, std::uint16_t ordinal) {
    for (LirInstId const i : allInsts(lir)) {
        if (lir.instResult(i).valid()) continue;
        for (auto const& o : lir.instOperands(i)) {
            if (o.kind == LirOperandKind::Reg && o.reg.isPhysical != 0
                && o.reg.id == ordinal) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

TEST(LirAsmRegisterVariable, ALeafThatBindsTheLinkRegisterSavesIt) {
    auto t = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(t.has_value());
    std::uint16_t const lr = ordinalOf(**t, "x30");
    // ✔MEASURED exit 139 before the fold: the pin's `mov x30, …` replaced the
    // return address of a leaf that saved nothing.
    auto const bound = emitted(
        "long f(long k) {\n"
        "    register long v __asm__(\"x30\") = k;\n"
        "    long out;\n"
        "    __asm__ volatile(\"add %0, x30, #2\" : \"=r\"(out) : \"r\"(v));\n"
        "    return out;\n"
        "}\n",
        "arm64");
    ASSERT_TRUE(bound.has_value());
    EXPECT_TRUE(isStored(*bound, lr))
        << "a leaf whose body writes the link register must save it";
    // THE CONTROL: the same leaf binding an ordinary register saves no x30.
    auto const plain = emitted(
        "long f(long k) {\n"
        "    register long v __asm__(\"x9\") = k;\n"
        "    long out;\n"
        "    __asm__ volatile(\"add %0, x9, #2\" : \"=r\"(out) : \"r\"(v));\n"
        "    return out;\n"
        "}\n",
        "arm64");
    ASSERT_TRUE(plain.has_value());
    EXPECT_FALSE(isStored(*plain, lr))
        << "a leaf that names no link register keeps its minimal frame";
}

// ── (G) THE SUPERVISOR CALL ──────────────────────────────────────────────────

TEST(LirAsmRegisterVariable, SvcIsTheSupervisorCallWithItsImmediate) {
    for (auto const& [text, imm] : {std::pair<char const*, std::int32_t>{"svc #0", 0},
                                    std::pair<char const*, std::int32_t>{"svc #0x80", 128}}) {
        auto r = lowerCToLir(
            std::string{"void f(void) {\n"
                        "    register long x8 __asm__(\"x8\") = 172;\n"
                        "    register long x0 __asm__(\"x0\");\n"
                        "    __asm__ volatile(\""} + text +
            "\" : \"=r\"(x0) : \"r\"(x8) : \"memory\");\n"
            "}\n",
            "arm64");
        ASSERT_FALSE(anyError(r)) << text << summarize(r);
        // P68 round 8 part 4: the template is the statement bundle's BODY.
        auto const bundle = onlyAsmRegion(r.lir.lir);
        ASSERT_TRUE(bundle.has_value()) << text;
        Lir const& body = bundle->region->body;
        bool found = false;
        for (LirInstId const i : asmRegionBodyInsts(*bundle->region)) {
            if (mnemonicOf(*r.target, body, i) != "syscall") continue;
            auto const ops = body.instOperands(i);
            ASSERT_EQ(ops.size(), 1u) << text << ": one immediate operand";
            EXPECT_EQ(ops[0].kind, LirOperandKind::ImmInt);
            EXPECT_EQ(ops[0].immInt32, imm) << text;
            found = true;
        }
        EXPECT_TRUE(found) << text << ": no `syscall` instruction was emitted";
    }
}

// ── (H) THE REFUSALS BOTH REFERENCES SHARE ─────────────────────────────────

TEST(LirAsmRegisterVariable, AnUnknownRegisterNameIsRefusedAtTheDeclaration) {
    auto r = lowerCToLir(
        "long f(long k) {\n"
        "    register long v __asm__(\"x99\") = k;\n"
        "    return v;\n"
        "}\n",
        "arm64");
    EXPECT_TRUE(anyCode(r, DiagnosticCode::S_AsmRegisterNameUnknown)) << summarize(r);
}

TEST(LirAsmRegisterVariable, TheAddressOfARegisterObjectIsRefused) {
    for (char const* src : {
             "long f(long k) { register long v = k; long *p = &v; return *p; }\n",
             "struct s { long a; };\n"
             "long f(long k) { register struct s v; v.a = k; long *p = &v.a; return *p; }\n",
             "long f(register long k) { long *p = &k; return *p; }\n"}) {
        auto r = lowerCToLir(src, "arm64", 0, LoweringExpectation::Refuses);
        EXPECT_TRUE(anyCode(r, DiagnosticCode::S_AddressOfRegisterObject))
            << src << summarize(r);
    }
    // THE CONTROL: the same address of an ordinary local and parameter.
    auto ok = lowerCToLir(
        "long f(long k) { long v = k; long *p = &v; long *q = &k; return *p + *q; }\n",
        "arm64");
    EXPECT_FALSE(anyError(ok)) << summarize(ok);
}

TEST(LirAsmRegisterVariable, ABoundRegisterInTheClobberListIsRefused) {
    auto r = lowerCToLir(
        "long f(long k) {\n"
        "    register long v __asm__(\"x9\") = k;\n"
        "    long out;\n"
        "    __asm__ volatile(\"add %0, x9, #2\" : \"=r\"(out) : \"r\"(v) : \"x9\");\n"
        "    return out;\n"
        "}\n",
        "arm64", 0, LoweringExpectation::Refuses);
    EXPECT_TRUE(anyCode(r, DiagnosticCode::S_InlineAsmBoundRegisterConflict))
        << summarize(r);
}

TEST(LirAsmRegisterVariable, TwoVariablesOnOneRegisterAreRefused) {
    auto r = lowerCToLir(
        "long f(long k, long j) {\n"
        "    register long a __asm__(\"x9\") = k;\n"
        "    register long b __asm__(\"x9\") = j;\n"
        "    long out;\n"
        "    __asm__ volatile(\"add %0, %1, %2\" : \"=r\"(out) : \"r\"(a), \"r\"(b));\n"
        "    return out;\n"
        "}\n",
        "arm64", 0, LoweringExpectation::Refuses);
    EXPECT_TRUE(anyCode(r, DiagnosticCode::S_InlineAsmBoundRegisterConflict))
        << summarize(r);
}

TEST(LirAsmRegisterVariable, ALetterThatCannotHoldTheBoundRegisterIsRefused) {
    auto r = lowerCToLir(
        "long f(long k) {\n"
        "    register long v __asm__(\"rbx\") = k;\n"
        "    long out;\n"
        "    __asm__ volatile(\"movq %1, %0\" : \"=r\"(out) : \"a\"(v));\n"
        "    return out;\n"
        "}\n",
        "x86_64", 0, LoweringExpectation::Refuses);
    EXPECT_TRUE(anyCode(r, DiagnosticCode::S_InlineAsmBoundRegisterConflict))
        << summarize(r);
}

TEST(LirAsmRegisterVariable, ContinuesInMustNameAFullRegisterOfTheSameClass) {
    // THE LOAD-TIME CONTRACT: each mutant re-points x0's `continuesIn`, and the
    // shipped document is the control (it loads — every other pin here uses it).
    for (char const* bad : {"nosuch", "w1", "d1"}) {
        auto const r = mutateShippedTargetSchemaDoc("arm64", [bad](nlohmann::json& doc) {
            for (auto& row : doc.at("registers")) {
                if (row.contains("name")
                    && row.at("name").get<std::string>() == "x0") {
                    row["continuesIn"] = bad;
                }
            }
        });
        EXPECT_FALSE(r.has_value())
            << "`continuesIn: \"" << bad << "\"` must be refused at load";
    }
}
