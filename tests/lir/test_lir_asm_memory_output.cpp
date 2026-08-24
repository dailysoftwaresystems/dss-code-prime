// ── D-ASM-MEMORY-CONSTRAINT-OUTPUT-FORM-NOT-REALIZED ────────────────────────
// ────────────────────────────────────────────────────────────────────────────
//
// ★★★ WHAT THIS FILE PINS. A memory-form constraint written in the OUTPUT
// section (`"=m"`, `"+m"`, `"=&m"`) was refused BY NAME at HIR→MIR, on every
// shipped cell, while gcc 13.3.0 compiles AND RUNS the same programs on both
// shipped targets at `-O0` and `-O2` (✔MEASURED: exit 42 natively and under
// qemu-aarch64; `movq %rdx, (%rax)` / `str x1, [x0]` behind a call). One
// working reference makes the behaviour REQUIRED.
//
// ★★★ THE MECHANISM, AND WHY IT NEEDED NO NEW VOCABULARY. `"m"` and `"=m"` hand
// the template the identical thing — a register holding the object's ADDRESS —
// and the DIRECTION lives entirely inside the template's own instruction. So a
// memory-form output is NOT a member of `MirAsmDescriptor::outputs`, whose own
// docblock defines that list as the RESULT PIECES (*"output k is result piece
// k"*): it produces none. It is carried in `inputs`, the list that is 1:1 with
// the instruction's MIR operands, and its one operand IS its address. The
// piece-and-store-back carriage an `"=r"` output gets is not merely unnecessary
// for this shape, it is wrong — it would write a captured register back over
// the memory the template already wrote.
//
// ⚠ THE RUNTIME WITNESS IS THE CORPUS EXAMPLE, NOT THIS FILE.
// `examples/c-subset/c_inline_asm_memory_output_operand` asserts a RESULT by
// EXECUTION on every runnable cell at both configs, because the defect this
// feature can regress into — lowering the operand's VALUE where its ADDRESS
// belongs — still compiles rc=0, still emits a memory form and still assembles.
// What THIS file adds is the structural half a run cannot see: that no result
// piece is minted, that no tied read half is synthesized, and that the arms
// which must still REFUSE still do.
//
// ★ WHAT EACH ARM ASSERTS:
//   (A) `"=m"` in the output section reaches LIR clean on BOTH shipped targets
//       — the born-red claim, since every tier used to refuse it.
//   (B) THE CARRIAGE: no `ReturnPiece` anywhere, `outputs` empty, and the
//       operand's descriptor entry binds the MEMORY form.
//   (C) `"+m"` needs NO tied read half — a memory operand's read and write
//       halves are the same memory named by one address register — and the
//       source's spelling survives on `constraint` even though `isReadWrite`
//       (which is the REQUEST for a tie, not a record of the `+`) is clear.
//   (D) A `+` written in the INPUT section is STILL REFUSED, for the memory
//       form as for the register form. ✔MEASURED on gcc 13.3.0, both targets:
//       `error: input operand constraint contains '+'` for `"+m"` and `"+r"`
//       alike. Widening the acceptance to cover it would be the bidirectional
//       half of the conformance rule broken.
//   (E) THE RESULT PIECE'S TYPE COMES FROM THE REGISTER OUTPUT, not from the
//       memory operand that precedes it in the source's OUTPUT SECTION. This is
//       the index space that stopped being one number: the source's output
//       section, the result-piece list and the `ReturnPiece` ordinals were all
//       spelled `outputCount`, and they coincide on every statement whose
//       outputs all bind registers — so a site left behind mis-indexes ONLY
//       when a memory-form output is present.
//   (F) `"=&m"` is accepted: the promise `&` makes is already true for this
//       form, whose bound register is materialised before the template and read
//       by it.
//   (G) TWO memory-form outputs in one statement get TWO operands and still no
//       result piece.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "lowered_lir_fixture.hpp"
#include "mir/mir.hpp"
#include "mir/mir_asm_descriptor.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::test_support;

namespace {

// Every diagnostic every tier emitted, joined — the "what actually happened"
// half of a failure message.
[[nodiscard]] std::string summarize(LoweredLir const& r) {
    std::string s;
    for (auto const& d : r.hirReporter.all()) s += "\n  hir: " + d.actual;
    for (auto const& d : r.mirReporter.all()) s += "\n  mir: " + d.actual;
    for (auto const& d : r.lirReporter.all()) s += "\n  lir: " + d.actual;
    return s.empty() ? std::string{"<no diagnostics>"} : s;
}

// ⚠ THE FRONT END MUST HAVE ACCEPTED THE SNIPPET. Without this, an arm that
// asserts "the lowering refused" would go green on a PARSE error and record
// nothing about the decision under test — the vacuous-red twin of a vacuous
// pass.
void requireFrontEndClean(LoweredLir const& r) {
    ASSERT_FALSE(r.model.hasErrors())
        << "the snippet did not get past semantic analysis, so nothing below "
           "is a statement about the LOWERING";
    ASSERT_FALSE(r.hirReporter.hasErrors());
}

[[nodiscard]] std::vector<MirInstId> allMirInsts(Mir const& mir) {
    std::vector<MirInstId> out;
    for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
                out.push_back(mir.blockInstAt(b, ii));
            }
        }
    }
    return out;
}

// The ONE `InlineAsm` instruction of a single-statement snippet. Fails the test
// rather than returning a sentinel: every arm below reads the descriptor, and a
// missing instruction must not read as "the descriptor said nothing".
[[nodiscard]] MirInstId theAsm(Mir const& mir) {
    std::vector<MirInstId> found;
    for (MirInstId const id : allMirInsts(mir)) {
        if (mir.instOpcode(id) == MirOpcode::InlineAsm) found.push_back(id);
    }
    EXPECT_EQ(found.size(), 1u)
        << "the snippet is written to carry exactly one asm statement";
    return found.empty() ? InvalidMirInst : found.front();
}

[[nodiscard]] std::size_t returnPieceCount(Mir const& mir) {
    std::size_t n = 0;
    for (MirInstId const id : allMirInsts(mir)) {
        if (mir.instOpcode(id) == MirOpcode::ReturnPiece) ++n;
    }
    return n;
}

[[nodiscard]] bool bindsMemoryForm(MirAsmOperand const& o) {
    return o.operandKindResolved
           && o.operandKind
                  == static_cast<std::uint8_t>(OperandKindFilter::MemBase);
}

// The two shipped targets, each with the template spellings ITS dialect
// accepts. The constraint letter is the same on both — `m` is what each
// `.target.json` binds to `membase` — and only the mnemonics differ, which is
// the vocabulary/grammar split this whole feature rides on.
struct Arch {
    char const* target;
    char const* store;      // %0 = memory out, %1 = register in
    char const* storeSym;   // the same, on the `%[name]` spelling
    char const* rmw;        // %0 = "+m" (64-bit), %1 = "=&r" scratch
    char const* mixed;      // %0 = "=m" (32-bit), %1 = "=r" (64-bit), %2/%3 in
    char const* twoOut;     // %0, %1 = two "=m"; %2, %3 = two register inputs
};

constexpr Arch kX86{
    "x86_64",
    R"(movl %1, %0)",
    R"(movl %[src], %[dst])",
    R"(movq %0, %1\n\taddq $5, %1\n\tmovq %1, %0)",
    R"(movl %2, %0\n\tmovq %3, %1)",
    R"(movl %2, %0\n\tmovl %3, %1)",
};
constexpr Arch kArm{
    "arm64",
    R"(str %1, %0)",
    R"(str %[src], %[dst])",
    R"(ldr %1, %0\n\tadd %1, %1, #5\n\tstr %1, %0)",
    R"(str %2, %0\n\tmov %1, %3)",
    R"(str %2, %0\n\tstr %3, %1)",
};

} // namespace

// ── (A) `"=m"` REACHES LIR CLEAN ON BOTH SHIPPED TARGETS ────────────────────
TEST(LirAsmMemoryOutput, MemoryFormOutputIsLoweredOnBothShippedTargets) {
    for (Arch const& a : {kX86, kArm}) {
        auto r = lowerCSubsetToLir(
            std::string{"void f(int *p, int v){ __asm__(\""} + a.store
                + "\" : \"=m\"(*p) : \"r\"(v)); }",
            a.target);
        requireFrontEndClean(r);
        EXPECT_FALSE(r.mirReporter.hasErrors())
            << a.target
            << ": `\"=m\"` used to be refused BY NAME here ('binds the memory "
               "form in the OUTPUT section'), about a shape gcc 13.3.0 compiles "
               "and RUNS on both shipped targets. Got:"
            << summarize(r);
        EXPECT_FALSE(r.lirReporter.hasErrors()) << a.target << summarize(r);
    }
}

// The symbolic spelling reaches the same binding through a second row. A form
// that travelled only on the positional row would be red here alone.
TEST(LirAsmMemoryOutput, TheSymbolicSpellingReachesTheSameBinding) {
    for (Arch const& a : {kX86, kArm}) {
        auto r = lowerCSubsetToLir(
            std::string{"void f(int *p, int v){ __asm__(\""} + a.storeSym
                + "\" : [dst]\"=m\"(*p) : [src]\"r\"(v)); }",
            a.target);
        requireFrontEndClean(r);
        EXPECT_FALSE(r.mirReporter.hasErrors()) << a.target << summarize(r);
        EXPECT_FALSE(r.lirReporter.hasErrors()) << a.target << summarize(r);
    }
}

// ── (B) THE CARRIAGE: AN ADDRESS OPERAND, NO RESULT PIECE ───────────────────
TEST(LirAsmMemoryOutput, MemoryFormOutputCarriesAnAddressAndMintsNoResultPiece) {
    for (Arch const& a : {kX86, kArm}) {
        auto r = lowerCSubsetToLir(
            std::string{"void f(int *p, int v){ __asm__(\""} + a.store
                + "\" : \"=m\"(*p) : \"r\"(v)); }",
            a.target);
        requireFrontEndClean(r);
        ASSERT_FALSE(r.mirReporter.hasErrors()) << a.target << summarize(r);

        Mir const&      mir   = r.mir.mir;
        MirInstId const asmId = theAsm(mir);
        ASSERT_TRUE(asmId.valid());
        MirAsmDescriptor const& d = mir.asmDescriptor(asmId);

        EXPECT_TRUE(d.outputs.empty())
            << a.target
            << ": `MirAsmDescriptor::outputs` IS the result-piece list, and a "
               "memory-form operand produces no piece — the template writes the "
               "object itself. An entry here would mint a `ReturnPiece` plus a "
               "store-back that overwrites what the template just wrote";
        ASSERT_EQ(d.inputs.size(), 2u)
            << a.target
            << ": the memory-form OUTPUT is carried as the address operand it "
               "is, ahead of the source-written inputs";
        EXPECT_TRUE(bindsMemoryForm(d.inputs[0]))
            << a.target << ": entry 0 must bind the FORM the target declared";
        EXPECT_EQ(d.inputs[0].constraint, "=m")
            << a.target << ": the source's spelling travels verbatim";
        EXPECT_FALSE(bindsMemoryForm(d.inputs[1]))
            << a.target << ": entry 1 is the ordinary `\"r\"` input";

        // The operand's spellings still answer to `%0` — the front end minted
        // them from the SOURCE's outputs-then-inputs numbering, which no tier
        // below recomputes.
        ASSERT_FALSE(d.inputs[0].spellings.empty()) << a.target;
        EXPECT_EQ(d.inputs[0].spellings.front(), "%0")
            << a.target
            << ": moving the entry between the descriptor's two lists must not "
               "move the `%N` the template writes";

        EXPECT_EQ(returnPieceCount(mir), 0u)
            << a.target
            << ": a statement whose only output is memory-form has no result "
               "piece at all — and getting that wrong is a `MirBuilder::"
               "addInlineAsm` process ABORT, not a wrong answer";
        EXPECT_FALSE(mir.instType(asmId).valid())
            << a.target
            << ": no outputs means no result type (the builder aborts on the "
               "mismatch in both directions)";

        // The operand wired to the memory entry must NOT be a Load: the
        // template is handed the OBJECT, and the machine names an object by its
        // ADDRESS. Lowering the VALUE there compiles, assembles, and writes
        // through a wild pointer.
        auto const ops = mir.instOperands(asmId);
        ASSERT_EQ(ops.size(), 2u) << a.target;
        EXPECT_NE(mir.instOpcode(ops[0]), MirOpcode::Load)
            << a.target
            << ": the memory-form operand's value is its ADDRESS. A `Load` here "
               "is the silent miscompile this feature's sibling row was closed "
               "for, arriving through the output direction";
    }
}

// ── (C) `"+m"` NEEDS NO TIED READ HALF ──────────────────────────────────────
TEST(LirAsmMemoryOutput, ReadWriteMemoryOutputSynthesizesNoTiedReadHalf) {
    for (Arch const& a : {kX86, kArm}) {
        auto r = lowerCSubsetToLir(
            std::string{"long long f(long long x){ long long t; __asm__(\""}
                + a.rmw + "\" : \"+m\"(x), \"=&r\"(t)); return x; }",
            a.target);
        requireFrontEndClean(r);
        ASSERT_FALSE(r.mirReporter.hasErrors()) << a.target << summarize(r);
        EXPECT_FALSE(r.lirReporter.hasErrors()) << a.target << summarize(r);

        Mir const&      mir   = r.mir.mir;
        MirInstId const asmId = theAsm(mir);
        ASSERT_TRUE(asmId.valid());
        MirAsmDescriptor const& d = mir.asmDescriptor(asmId);

        ASSERT_EQ(d.inputs.size(), 1u)
            << a.target
            << ": the memory-form output is the ONLY operand — a tied read half "
               "appended for it would be a second operand carrying a LOAD the "
               "template never reads through";
        EXPECT_TRUE(bindsMemoryForm(d.inputs[0])) << a.target;
        EXPECT_EQ(d.inputs[0].constraint, "+m")
            << a.target
            << ": the SOURCE's spelling is what diagnostics quote, and it "
               "survives the carriage decision";
        EXPECT_FALSE(d.inputs[0].isReadWrite)
            << a.target
            << ": `isReadWrite` is the REQUEST for a tied read half, not a "
               "record that the source wrote `+`. Leaving it set makes "
               "`tieAsmReadWriteOperands` refuse the statement as 'a `+` "
               "written in the INPUT section' — a refusal whose stated reason "
               "is false about an operand written in the OUTPUT section";
        EXPECT_FALSE(d.inputs[0].tiedOutput.has_value()) << a.target;

        ASSERT_EQ(d.outputs.size(), 1u)
            << a.target << ": the `\"=&r\"` scratch is the one result piece";
        EXPECT_EQ(d.outputs[0].constraint, "=&r") << a.target;
        EXPECT_EQ(returnPieceCount(mir), 0u)
            << a.target
            << ": piece 0 IS the instruction's own value, so a one-piece asm "
               "mints no separate `ReturnPiece`";
    }
}

// ── (D) A `+` IN THE INPUT SECTION IS STILL REFUSED — BOTH FORMS ────────────
TEST(LirAsmMemoryOutput, ReadWriteInTheInputSectionIsStillRefused) {
    for (Arch const& a : {kX86, kArm}) {
        for (char const* letter : {"+m", "+r"}) {
            auto r = lowerCSubsetToLir(
                std::string{"void f(int *p, int v){ __asm__(\""} + a.store
                    + "\" : : \"" + letter + "\"(*p), \"r\"(v)); }",
                a.target);
            requireFrontEndClean(r);
            EXPECT_TRUE(r.mirReporter.hasErrors() || r.lirReporter.hasErrors())
                << a.target << " " << letter
                << ": gcc 13.3.0 refuses this on both shipped targets with "
                   "\"input operand constraint contains '+'\", and accepting "
                   "what no reference accepts is as much a conformance defect "
                   "as refusing what one compiles. Got:"
                << summarize(r);
        }
    }
}

// ── (E) THE RESULT PIECE'S TYPE IS THE REGISTER OUTPUT'S ────────────────────
TEST(LirAsmMemoryOutput, ResultPieceTypeComesFromTheRegisterOutput) {
    for (Arch const& a : {kX86, kArm}) {
        auto r = lowerCSubsetToLir(
            std::string{"long long f(int n, long long w){ int m; long long o; "
                        "__asm__(\""}
                + a.mixed
                + "\" : \"=m\"(m), \"=r\"(o) : \"r\"(n), \"r\"(w)); return o; }",
            a.target);
        requireFrontEndClean(r);
        ASSERT_FALSE(r.mirReporter.hasErrors()) << a.target << summarize(r);
        EXPECT_FALSE(r.lirReporter.hasErrors()) << a.target << summarize(r);

        Mir const&      mir   = r.mir.mir;
        MirInstId const asmId = theAsm(mir);
        ASSERT_TRUE(asmId.valid());
        MirAsmDescriptor const& d = mir.asmDescriptor(asmId);

        ASSERT_EQ(d.outputs.size(), 1u)
            << a.target
            << ": the source's output SECTION has two entries and its "
               "result-piece list has one — they are different index spaces the "
               "moment a memory-form output appears";
        EXPECT_EQ(d.outputs[0].constraint, "=r") << a.target;

        // ⚠ THE CORE KIND, NOT THE `TypeId`. `long long` and the bare `I64`
        // primitive are DIFFERENT interned ids — the spelling is part of a
        // type's identity here (`D-LANG-TYPE-IDENTITY-VOCABULARY`) — so an id
        // comparison against `primitive(I64)` fails on a correct build. ✔That
        // is measured, not assumed: it is how the first version of this arm
        // failed, on both targets, against an implementation that was right.
        auto const& interner = r.model.lattice().interner();
        EXPECT_EQ(interner.kind(mir.instType(asmId)), TypeKind::I64)
            << a.target
            << ": piece 0 is the `\"=r\"` output, a 64-bit object. Taking the "
               "type from the source's output section instead yields the 32-bit "
               "MEMORY operand that precedes it, and the template operand then "
               "runs at the wrong width";
        EXPECT_NE(interner.kind(mir.instType(asmId)), TypeKind::I32) << a.target;
    }
}

// ── (F) `"=&m"` IS ACCEPTED ─────────────────────────────────────────────────
TEST(LirAsmMemoryOutput, EarlyClobberOnTheMemoryFormIsAccepted) {
    for (Arch const& a : {kX86, kArm}) {
        auto r = lowerCSubsetToLir(
            std::string{"void f(int *p, int v){ __asm__(\""} + a.store
                + "\" : \"=&m\"(*p) : \"r\"(v)); }",
            a.target);
        requireFrontEndClean(r);
        EXPECT_FALSE(r.mirReporter.hasErrors())
            << a.target
            << ": gcc 13.3.0 compiles `\"=&m\"` on both shipped targets "
               "(✔MEASURED). The promise `&` makes is already true for this "
               "form: the bound register holds the ADDRESS, is materialised "
               "before the template and is read by it, so its live range covers "
               "the template and overlaps every other operand's. Got:"
            << summarize(r);
        EXPECT_FALSE(r.lirReporter.hasErrors()) << a.target << summarize(r);
    }
}

// ── (G) TWO MEMORY-FORM OUTPUTS IN ONE STATEMENT ────────────────────────────
TEST(LirAsmMemoryOutput, TwoMemoryOutputsGetTwoOperandsAndStillNoResultPiece) {
    for (Arch const& a : {kX86, kArm}) {
        auto r = lowerCSubsetToLir(
            std::string{"void f(int *p, int *q, int u, int v){ __asm__(\""}
                + a.twoOut
                + "\" : \"=m\"(*p), \"=m\"(*q) : \"r\"(u), \"r\"(v)); }",
            a.target);
        requireFrontEndClean(r);
        ASSERT_FALSE(r.mirReporter.hasErrors()) << a.target << summarize(r);
        EXPECT_FALSE(r.lirReporter.hasErrors()) << a.target << summarize(r);

        Mir const&      mir   = r.mir.mir;
        MirInstId const asmId = theAsm(mir);
        ASSERT_TRUE(asmId.valid());
        MirAsmDescriptor const& d = mir.asmDescriptor(asmId);

        EXPECT_TRUE(d.outputs.empty()) << a.target;
        ASSERT_EQ(d.inputs.size(), 4u)
            << a.target
            << ": two address operands ahead of the two source-written inputs";
        EXPECT_TRUE(bindsMemoryForm(d.inputs[0])) << a.target;
        EXPECT_TRUE(bindsMemoryForm(d.inputs[1])) << a.target;
        ASSERT_FALSE(d.inputs[0].spellings.empty()) << a.target;
        ASSERT_FALSE(d.inputs[1].spellings.empty()) << a.target;
        EXPECT_EQ(d.inputs[0].spellings.front(), "%0") << a.target;
        EXPECT_EQ(d.inputs[1].spellings.front(), "%1")
            << a.target
            << ": the two memory outputs keep the source's own numbering, in "
               "source order — a carriage that appended them in any other order "
               "would bind `%1` to the first object";

        auto const ops = mir.instOperands(asmId);
        ASSERT_EQ(ops.size(), 4u) << a.target;
        EXPECT_NE(ops[0].v, ops[1].v)
            << a.target
            << ": two objects, two addresses — one shared operand would write "
               "both values into one of them";
        EXPECT_EQ(returnPieceCount(mir), 0u) << a.target;
    }
}
