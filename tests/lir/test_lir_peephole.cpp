// LIR PEEPHOLE (plan 22 OPT8) — redundant-copy elimination.
//
// What this file pins, in the order the hazards were measured:
//
//   * THE POSITIVE. A full-width copy of a physical register into itself is
//     deleted, and nothing else in the module moves.
//
//   * ★★★ THE NEGATIVE THAT MATTERS. `zext`, `sext`, `trunc`, `not` and
//     `neg` with their result register EQUAL to their only operand register
//     are NOT deleted. ✔MEASURED over the 585 dumping examples of
//     `examples/c/**` (2026-08-25): 12021 instructions at post-callconv have
//     result == their sole operand, and only 5575 are the class MOVE — the
//     other 6446 are live computation. On x86-64 `mov` (64-bit), `trunc` and
//     `zext` are three DIFFERENT LIR opcodes whose bytes all disassemble as
//     `mov`, so a mnemonic-text or byte-pattern rule deletes all three. The
//     runtime half of this pin is `examples/c/lir_peephole_self_ops`.
//
//   * THE WIDTH GUARD. A NARROW copy into the same register is not an
//     identity: an x86-64 32-bit `mov` zeroes the upper half of the
//     destination. It survives.
//
//   * THE SIDE-STRUCTURE REFUSAL. A copy that names a per-instruction
//     register-constraint entry is kept, because deleting the only namer of
//     a pool entry orphans it and `verifyLirRebuild` reports
//     `L_SideStructureReferenceLost`.
//
//   * THE REAL PIPELINE. `c` source lowered through the ACTUAL chain
//     (MIR→LIR → wide-call-args → liveness → regalloc → rewrite →
//     2-address-legalize) then peepholed: the pass removes a positive number
//     of instructions, every removal is the class MOVE, block topology and
//     function count are preserved, and both `verifyLirRebuild` and
//     `verifyLirPostRegalloc` accept the output.
//
// ⚠ THE FIXTURES HERE BUILD PHYSICAL-REGISTER MODULES BY HAND, which is a
// post-regalloc shape. That is deliberate: the property under test is about
// what the ALLOCATOR left behind, and a virtual-register module cannot
// express it at all.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_peephole.hpp"
#include "lir/lir_reg.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_verifier.hpp"
#include "lir/lir_wide_call_args.hpp"
#include "lowered_lir_fixture.hpp"
#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::test_support::lowerCToLir;

namespace {

// ⚠ THESE THROW RATHER THAN `abort()`, and the difference is the whole
// verdict. `abort()` kills the test PROCESS, so every sibling test in this
// executable loses its result and the harness cannot say which unit failed;
// GoogleTest reports a THROW as a failure of the ONE test that hit it. Same
// discipline as `tests/test_support/repo_root.hpp`, enforced by
// `check-no-abort-in-tests`.
std::shared_ptr<TargetSchema> const& x86Schema() {
    static std::shared_ptr<TargetSchema> const schema = [] {
        auto r = TargetSchema::loadShipped("x86_64");
        if (!r.has_value()) {
            throw std::runtime_error(
                "test environment: TargetSchema::loadShipped(\"x86_64\") "
                "failed - the shipped target documents are not reachable");
        }
        return *r;
    }();
    return schema;
}

std::uint16_t op(std::string_view mnemonic) {
    auto const i = x86Schema()->opcodeByMnemonic(mnemonic);
    if (!i.has_value()) {
        throw std::runtime_error(
            std::string("the shipped x86_64 target declares no opcode '")
            + std::string(mnemonic) + "' - this fixture names it directly");
    }
    return *i;
}

// The ordinal of a named 64-bit GPR in the shipped x86_64 register table.
std::uint32_t ord(std::string_view name) {
    auto const o = x86Schema()->registerByName(name);
    if (!o.has_value()) {
        throw std::runtime_error(
            std::string("the shipped x86_64 target declares no register '")
            + std::string(name) + "' - this fixture names it directly");
    }
    return *o;
}

// `f() { <opcode> pDst, pSrc ; ret pDst }` — one instruction under test in a
// minimal post-regalloc function. `flags` carries the operation width.
[[nodiscard]] Lir
oneOpModule(std::uint16_t opcode, std::string_view dst, std::string_view src,
            std::uint8_t flags = 0, bool withConstraints = false) {
    LirBuilder b{*x86Schema()};
    std::uint32_t ci = 0;
    if (withConstraints) {
        ImplicitRegisterConstraint c;
        c.inputNames     = {"rcx"};
        c.outputNames    = {"rdx"};
        c.clobberedNames = {"rdx"};
        ci = b.regConstraintPoolAdd(std::move(c));
    }
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const d = makePhysicalReg(ord(dst), LirRegClass::GPR);
    LirReg const s = makePhysicalReg(ord(src), LirRegClass::GPR);
    std::array<LirOperand, 1> const ops{LirOperand::makeReg(s)};
    LirInstId const inst = b.addInst(opcode, d, ops, /*payload=*/0, flags);
    if (withConstraints) b.setInstRegConstraints(inst, ci);
    std::array<LirOperand, 1> const retOps{LirOperand::makeReg(d)};
    b.addReturn(op("ret"), retOps);
    return std::move(b).finish();
}

// Every (opcode, result, sole-operand) triple in a module, flattened.
struct InstShape {
    std::uint16_t opcode = 0;
    std::uint32_t result = 0;
};

[[nodiscard]] std::vector<InstShape> shapesOf(Lir const& lir) {
    std::vector<InstShape> out;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(blk); ++ii) {
                LirInstId const in = lir.blockInstAt(blk, ii);
                out.push_back({lir.instOpcode(in), lir.instResult(in).id});
            }
        }
    }
    return out;
}

[[nodiscard]] std::size_t instTotal(Lir const& lir) {
    return shapesOf(lir).size();
}

// The whole post-regalloc chain the compile pipeline runs, up to (but not
// including) callconv — the point the peephole occupies.
struct Pipelined {
    test_support::LoweredLir lowered;
    DiagnosticReporter       rep;
    LirWideCallResult        wide;
    LirLiveness              liveness;
    LirAllocation            alloc;
    LirRewriteResult         rewritten;
    LirTwoAddrLegalizeResult legal;

    explicit Pipelined(test_support::LoweredLir l) : lowered(std::move(l)) {}
};

[[nodiscard]] std::unique_ptr<Pipelined> runToLegalize(std::string src) {
    auto p = std::make_unique<Pipelined>(lowerCToLir(std::move(src)));
    if (!p->lowered.lir.ok) return p;
    p->wide = lowerWideCallArgs(p->lowered.lir.lir, *p->lowered.target,
                                /*ccIndex=*/0, p->rep);
    if (!p->wide.ok) return p;
    p->liveness  = analyzeLiveness(p->wide.lir);
    p->alloc     = allocateRegisters(p->wide.lir, *p->lowered.target,
                                     p->liveness, /*ccIndex=*/0, p->rep);
    if (!p->alloc.ok()) return p;
    p->rewritten = rewriteWithAllocation(p->wide.lir, *p->lowered.target,
                                         p->alloc, p->rep);
    if (!p->rewritten.ok) return p;
    p->legal = legalizeTwoAddress(p->rewritten.lir, *p->lowered.target, p->rep);
    return p;
}

} // namespace

// ── THE POSITIVE ────────────────────────────────────────────────────────

TEST(LirPeephole, FullWidthSelfCopyIsDeleted) {
    Lir const before = oneOpModule(op("mov"), "r13", "r13");
    ASSERT_EQ(instTotal(before), 2u);  // the copy + the return

    DiagnosticReporter rep;
    auto const r = runLirPeephole(before, *x86Schema(), rep);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.redundantCopiesRemoved, 1u);
    EXPECT_EQ(instTotal(r.lir), 1u);
    EXPECT_EQ(r.lir.moduleFuncCount(), before.moduleFuncCount());
    EXPECT_EQ(rep.errorCount(), 0u);
    // And the module the pipeline would go on to consume still verifies.
    DiagnosticReporter vrep;
    EXPECT_TRUE(verifyLirRebuild(before, r.lir, "lir-peephole", vrep));
    EXPECT_TRUE(verifyLirPostRegalloc(r.lir, *x86Schema(), vrep));
}

TEST(LirPeephole, CopyBetweenDIFFERENTRegistersSurvives) {
    Lir const before = oneOpModule(op("mov"), "r13", "r14");
    DiagnosticReporter rep;
    auto const r = runLirPeephole(before, *x86Schema(), rep);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.redundantCopiesRemoved, 0u);
    EXPECT_EQ(instTotal(r.lir), instTotal(before));
}

// ── ★★★ THE NEGATIVE THAT MATTERS ──────────────────────────────────────
//
// Each of these is a live computation whose result register happens to equal
// its input register. A rule shaped "result == its only operand ⇒ delete"
// removes every one of them, the module still verifies, and the program
// returns the wrong number. On x86-64 the first three additionally ENCODE to
// bytes a disassembler prints as `mov`.

TEST(LirPeephole, SelfZextIsNotDeleted) {
    Lir const before = oneOpModule(op("zext"), "r15", "r15");
    DiagnosticReporter rep;
    auto const r = runLirPeephole(before, *x86Schema(), rep);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.redundantCopiesRemoved, 0u)
        << "zext with result == operand clears the upper half of the "
           "destination; deleting it is a wrong answer, not a saved "
           "instruction";
    EXPECT_EQ(instTotal(r.lir), instTotal(before));
}

TEST(LirPeephole, SelfSextIsNotDeleted) {
    Lir const before = oneOpModule(op("sext"), "r15", "r15",
                                   kLirInstFlagWidth32);
    DiagnosticReporter rep;
    auto const r = runLirPeephole(before, *x86Schema(), rep);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.redundantCopiesRemoved, 0u);
    EXPECT_EQ(instTotal(r.lir), instTotal(before));
}

TEST(LirPeephole, SelfTruncIsNotDeleted) {
    Lir const before = oneOpModule(op("trunc"), "r14", "r14",
                                   kLirInstFlagWidth32);
    DiagnosticReporter rep;
    auto const r = runLirPeephole(before, *x86Schema(), rep);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.redundantCopiesRemoved, 0u);
    EXPECT_EQ(instTotal(r.lir), instTotal(before));
}

TEST(LirPeephole, SelfNotAndSelfNegAreNotDeleted) {
    for (auto const* mnemonic : {"not", "neg"}) {
        Lir const before = oneOpModule(op(mnemonic), "r12", "r12");
        DiagnosticReporter rep;
        auto const r = runLirPeephole(before, *x86Schema(), rep);
        ASSERT_TRUE(r.ok()) << mnemonic;
        EXPECT_EQ(r.redundantCopiesRemoved, 0u) << mnemonic;
        EXPECT_EQ(instTotal(r.lir), instTotal(before)) << mnemonic;
    }
}

// ★ THE OPCODE TEST IS AN IDENTITY TEST, NOT A NAME TEST — stated as an
// assertion about the SCHEMA rather than about the pass, because it is the
// schema fact the whole rule rests on.
TEST(LirPeephole, TheThreeXVGRPCopyLookalikesAreDistinctOpcodes) {
    auto const mov = x86Schema()->regClassOpOpcode(TargetRegClass::GPR,
                                                   RegClassOp::Move);
    ASSERT_TRUE(mov.has_value());
    EXPECT_EQ(*mov, op("mov"));
    EXPECT_NE(*mov, op("trunc"));
    EXPECT_NE(*mov, op("zext"));
    EXPECT_NE(*mov, op("sext"));
}

// ── THE WIDTH GUARD ────────────────────────────────────────────────────

TEST(LirPeephole, NarrowSelfCopyIsNotDeleted) {
    // A 32-bit `mov` into a 64-bit register zeroes the upper half, so even
    // with source == destination it is not an identity.
    Lir const before = oneOpModule(op("mov"), "r13", "r13",
                                   kLirInstFlagWidth32);
    DiagnosticReporter rep;
    auto const r = runLirPeephole(before, *x86Schema(), rep);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.redundantCopiesRemoved, 0u)
        << "a copy narrower than the register it names writes bits it did "
           "not read";
    EXPECT_EQ(instTotal(r.lir), instTotal(before));
}

// ── THE SIDE-STRUCTURE REFUSAL ─────────────────────────────────────────

TEST(LirPeephole, SelfCopyNamingAConstraintEntryIsKept) {
    Lir const before = oneOpModule(op("mov"), "r13", "r13", /*flags=*/0,
                                   /*withConstraints=*/true);
    DiagnosticReporter rep;
    auto const r = runLirPeephole(before, *x86Schema(), rep);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.redundantCopiesRemoved, 0u)
        << "deleting the only namer of a constraint-pool entry orphans it; "
           "keeping the copy is the fail-safe arm";
    EXPECT_EQ(instTotal(r.lir), instTotal(before));
    DiagnosticReporter vrep;
    EXPECT_TRUE(verifyLirRebuild(before, r.lir, "lir-peephole", vrep));
}

// ── THE EMPTY MODULE ───────────────────────────────────────────────────

TEST(LirPeephole, EmptyModuleIsASuccess) {
    Lir const empty{};
    DiagnosticReporter rep;
    auto const r = runLirPeephole(empty, *x86Schema(), rep);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.redundantCopiesRemoved, 0u);
    EXPECT_EQ(rep.errorCount(), 0u);
}

// ── THE REAL PIPELINE ──────────────────────────────────────────────────

TEST(LirPeephole, RealLoweringLosesOnlyClassMovesAndStillVerifies) {
    // A shape the allocator routinely leaves redundant copies in: several
    // live values, a narrowing, and a call boundary.
    auto p = runToLegalize(
        "unsigned long long g(unsigned long long a, unsigned long long b);\n"
        "unsigned f(unsigned long long a, unsigned long long b) {\n"
        "  unsigned long long s = a + b;\n"
        "  unsigned long long t = g(s, a);\n"
        "  return (unsigned)(t ^ b);\n"
        "}\n");
    ASSERT_TRUE(p->lowered.lir.ok);
    ASSERT_TRUE(p->legal.ok());

    auto const before = shapesOf(p->legal.lir);
    DiagnosticReporter rep;
    auto const r = runLirPeephole(p->legal.lir, *p->lowered.target, rep);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(rep.errorCount(), 0u);

    // Structure is preserved.
    EXPECT_EQ(r.lir.moduleFuncCount(), p->legal.lir.moduleFuncCount());
    for (std::uint32_t fi = 0; fi < r.lir.moduleFuncCount(); ++fi) {
        EXPECT_EQ(r.lir.funcBlockCount(r.lir.funcAt(fi)),
                  p->legal.lir.funcBlockCount(p->legal.lir.funcAt(fi)));
    }

    // Exactly `redundantCopiesRemoved` instructions went, and every one of
    // them was the class MOVE. Asserted as a multiset difference rather than
    // by re-running the rule, so a rule that changed its mind would show up.
    auto const after = shapesOf(r.lir);
    ASSERT_EQ(before.size(), after.size() + r.redundantCopiesRemoved);
    auto const movOpcode = *p->lowered.target->regClassOpOpcode(
        TargetRegClass::GPR, RegClassOp::Move);
    std::size_t movBefore = 0, movAfter = 0;
    for (auto const& s : before) if (s.opcode == movOpcode) ++movBefore;
    for (auto const& s : after)  if (s.opcode == movOpcode) ++movAfter;
    EXPECT_EQ(movBefore - movAfter, r.redundantCopiesRemoved)
        << "every deleted instruction must be the register class's copy";

    // ⚠ Not `EXPECT_GT(removed, 0)` on this ONE function: which registers the
    // allocator hands out is not this test's contract. The corpus-level claim
    // (5486 removals over `examples/c/**`) is measured by the artifact A/B in
    // `scratchpad/p36/lane-g/`, and `examples/c/lir_peephole_self_ops` is the
    // runtime pin. What IS this test's contract is that whatever went, went
    // for the right reason — asserted above — and that the result verifies.
    DiagnosticReporter vrep;
    EXPECT_TRUE(verifyLirRebuild(p->legal.lir, r.lir, "lir-peephole", vrep));
    EXPECT_TRUE(verifyLirPostRegalloc(r.lir, *p->lowered.target, vrep));
    EXPECT_EQ(vrep.errorCount(), 0u);
}

TEST(LirPeephole, RealLoweringRemovesRedundantCopiesSomewhereInTheCorpusShape) {
    // The existence claim, stated over a SET of shapes rather than one
    // function (a universal claim is per-example; an existence claim needs a
    // corpus). If NONE of these leaves a redundant copy the pass is dead
    // code and the 4% artifact measurement could not have happened.
    //
    // ⚠ EACH SHAPE CARRIES A CALLER WITH CONSTANT ARGUMENTS, and that is not
    // decoration. ✔MEASURED 2026-08-25: the callee alone (`f` by itself)
    // produced ZERO redundant copies through this harness, while the same
    // source plus a `main` that calls it produced six — the copies the
    // allocator leaves are concentrated in the argument-materialization and
    // result-capture code at the CALL SITE, not in the arithmetic. A first
    // version of this test omitted the caller and failed for that reason.
    static constexpr char const* kSources[] = {
        "int g(int);\n"
        "int f(int a, int b) { int r = a > b ? a - b : b - a;"
        " return r + g(r); }\n"
        "int main(void) { return f(3, 4); }\n",
        "unsigned f(unsigned long long v){ return (unsigned)(v >> 3) ^ 7u; }\n"
        "int main(void){ return (int)f(99); }\n",
        "unsigned long long f(unsigned long long a){ return a * a + a; }\n"
        "int main(void){ return (int)f(5); }\n",
        "long long g(long long);\n"
        "long long f(long long a){ return g(a) + g(a + 1); }\n"
        "int main(void){ return (int)f(2); }\n",
    };
    std::size_t total = 0;
    for (auto const* src : kSources) {
        auto p = runToLegalize(src);
        if (!p->lowered.lir.ok || !p->legal.ok()) continue;
        DiagnosticReporter rep;
        auto const r = runLirPeephole(p->legal.lir, *p->lowered.target, rep);
        ASSERT_TRUE(r.ok()) << src;
        total += r.redundantCopiesRemoved;
    }
    EXPECT_GT(total, 0u)
        << "the real allocator leaves redundant copies on every shipped "
           "corpus; a zero here means the rule stopped matching them";
}

// ── THE 128-BIT OPERATION WIDTH ────────────────────────────────
//
// These live here because this is where the defect was FOUND -- the peephole's
// width guard could not match a full-width `movaps %xmm12,%xmm12` because 128
// was unsayable -- but what they pin is the SUBSTRATE, not the peephole. Both
// halves have to hold for the fix to mean anything: LIR must be able to CARRY
// the width, and a target must be able to DECLARE a variant guarded on it.

TEST(LirPeephole, LirCanExpressA128BitOperationWidth) {
    EXPECT_EQ(lirInstWidthBits(kLirInstFlagWidth128), 128u);
    // The width flags stay mutually exclusive and the narrower ones still win,
    // so no existing instruction's width can be changed by the new bit -- the
    // 8/16 arms are tested BEFORE 128 in the decode.
    EXPECT_EQ(lirInstWidthBits(0), 64u);
    EXPECT_EQ(lirInstWidthBits(kLirInstFlagWidth32), 32u);
    EXPECT_EQ(lirInstWidthBits(kLirInstFlagWidth16), 16u);
    EXPECT_EQ(lirInstWidthBits(kLirInstFlagWidth8), 8u);
    // And it does not collide with the three non-width flags in the byte.
    for (std::uint8_t const other : {kLirInstFlagEarlyClobberResult,
                                     kLirInstFlagMemoryIsDestination,
                                     kLirInstFlagOutgoingArgsPlaced}) {
        EXPECT_EQ(kLirInstFlagWidth128 & other, 0)
            << "the 128-bit width flag overlaps another flag in the byte";
    }
}

TEST(LirPeephole, ATargetMayDeclareA128BitEncodingVariantGuard) {
    // Before the widening the loader REJECTED this document with \"'width' must
    // be the integer 8, 16, 32, or 64\", so a 128-bit vector encoding could not
    // be described at all. Mutating the FPR class MOVE is deliberate: it is the
    // exact row whose full-register copy the peephole cannot currently prove.
    auto r = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            for (auto& op : doc.at("opcodes")) {
                if (!op.is_object()) continue;
                if (op.value("mnemonic", std::string{}) != "movaps") continue;
                for (auto& v : op.at("encoding").at("variants")) {
                    v.at("guard")["width"] = 128;
                }
            }
        });
    ASSERT_TRUE(r.has_value())
        << "a target declaring a 128-bit encoding-variant guard must LOAD";
    auto const movaps = (*r)->opcodeByMnemonic("movaps");
    ASSERT_TRUE(movaps.has_value());
    auto const* info = (*r)->opcodeInfo(*movaps);
    ASSERT_NE(info, nullptr);
    ASSERT_FALSE(info->encoding.variants.empty());
    EXPECT_EQ(info->encoding.variants[0].guardWidthBits, 128u)
        << "the declared 128-bit guard must survive the load as 128, not be "
           "truncated or silently dropped";
}