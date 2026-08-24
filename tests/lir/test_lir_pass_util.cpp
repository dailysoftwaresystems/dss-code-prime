// Module SIDE-STRUCTURE carry-across tests (D-LIR-PER-INST-REG-CONSTRAINTS).
//
// A `Lir` module carries two by-index side structures beside the
// instruction stream — the wide-literal pool (referenced by `LiteralIndex`
// OPERANDS) and the per-instruction register-constraint pool (referenced
// by `detail::LirInst::regConstraints`). FOUR passes rebuild that stream
// into a fresh `LirBuilder` and must carry both across:
//
//   ✔MEASURED 2026-08-14 (`grep -rn "copyModuleSideStructures" src/`):
//     src/lir/lir_2addr_legalize.cpp
//     src/lir/lir_callconv.cpp
//     src/lir/lir_rewrite.cpp
//     src/lir/lir_wide_call_args.cpp
//   FOUR, not the three two separate documents claimed.
//
// What this file pins:
//   * `copyModuleSideStructures` carries BOTH pools, index-for-index.
//   * The constraint pool AND the per-instruction handle survive EACH of
//     the four passes, asserted after each pass INDIVIDUALLY — a drop in
//     pass 1 restored by luck in pass 4 reads as green if you only assert
//     at the end.
//   * ★★ The handle lands on the RIGHT instruction. Two of the four
//     passes append instructions AFTER the one they rebuilt (the
//     rewriter's spill `frame_store`, callconv's call-result capture
//     move), so "carry onto `lastInst()`" is a wrong-instruction bug that
//     no reference count can see. Both shapes are constructed and pinned.
//   * `carryInstSideData` carries the handle across a rebuild, and the
//     verifier catches the case where a pass forgets to call it.
//   * `LirBuilder::regConstraintPoolAdd` resolves names → ordinals so the
//     two representations of one register set cannot disagree.

#include "core/types/call_payload.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_verifier.hpp"
#include "lir/lir_wide_call_args.hpp"
#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dss;

namespace {

std::shared_ptr<TargetSchema> const& x86Schema() {
    static std::shared_ptr<TargetSchema> const schema = [] {
        auto r = TargetSchema::loadShipped("x86_64");
        if (!r.has_value()) std::abort();  // misconfigured test environment
        return *r;
    }();
    return schema;
}

std::uint16_t op(std::string_view mnemonic) {
    auto const i = x86Schema()->opcodeByMnemonic(mnemonic);
    if (!i.has_value()) std::abort();
    return *i;
}

// A constraint set in the shape an inline-asm statement produces: an
// input pinned to one register, an output in another, a third destroyed.
// Register NAMES only — `regConstraintPoolAdd` derives the ordinals.
//
// ⚠ THE CALLER MUST KEEP `outputs ⊆ clobbered`
// (D-LIR-PER-INSTRUCTION-OUTPUTS-NOT-ENFORCED-SUBSET-OF-CLOBBERED): the builder now rejects an
// output that is not also clobbered, so `out` must name a register the
// `clobber` list covers. The helper does NOT silently repair it — a
// fixture that quietly satisfied the rule its subject enforces would
// stop the rule from ever being exercised by these tests.
[[nodiscard]] ImplicitRegisterConstraint sampleConstraint(
    std::string in, std::string out, std::string clobber) {
    ImplicitRegisterConstraint c;
    c.inputNames     = {std::move(in)};
    c.outputNames    = {std::move(out)};
    c.clobberedNames = {std::move(clobber)};
    return c;
}

[[nodiscard]] LirLiteralValue i64Literal(std::int64_t v) {
    LirLiteralValue lv;
    lv.value = v;
    lv.core  = TypeKind::I64;
    return lv;
}

// `f() { v1 = mov #42; ret v1 }` with a per-instruction constraint set on
// the `mov` and a wide literal in the pool. Small enough that every one
// of the four rebuild passes accepts it, and it exercises BOTH side
// structures at once.
struct SeededModule {
    Lir           lir;
    std::uint32_t constraintIndex = 0;
    std::uint32_t literalIndex    = 0;
};

[[nodiscard]] SeededModule buildSeededModule() {
    LirBuilder b{*x86Schema()};
    SeededModule out{Lir{}};
    out.constraintIndex =
        b.regConstraintPoolAdd(sampleConstraint("rcx", "rdx", "rdx"));
    out.literalIndex = b.literalPoolAdd(i64Literal(0x1122334455667788LL));

    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const r1 = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const movOps{
        LirOperand::makeLiteralIndex(out.literalIndex)};
    LirInstId const mov = b.addInst(op("mov"), r1, movOps);
    b.setInstRegConstraints(mov, out.constraintIndex);
    std::array<LirOperand, 1> const retOps{LirOperand::makeReg(r1)};
    b.addReturn(op("ret"), retOps);
    out.lir = std::move(b).finish();
    return out;
}

// One instruction in a rebuilt module that still names a constraint set,
// with enough context to assert it is the RIGHT instruction — a count
// alone cannot tell a carried handle from a misplaced one.
struct Carrier {
    std::uint16_t opcode       = 0;
    std::uint32_t indexInBlock = 0;
    std::uint32_t handle       = 0;
};

[[nodiscard]] std::vector<Carrier> collectCarriers(Lir const& lir) {
    std::vector<Carrier> out;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const bb = lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
                LirInstId const inst = lir.blockInstAt(bb, i);
                std::uint32_t const h = lir.instRegConstraintHandle(inst);
                if (h == kLirNoRegConstraints) continue;
                out.push_back({lir.instOpcode(inst), i, h});
            }
        }
    }
    return out;
}

[[nodiscard]] std::size_t countConstraintCarriers(Lir const& lir) {
    return collectCarriers(lir).size();
}

// Literal-pool REFERENCES, not entries. A pass that kept the pool but
// dropped a `LiteralIndex` operand leaves every surviving index resolving
// and no pool shrunk, so the reference count is the only place the loss is
// visible — the literal half of what `verifyLirRebuild`'s census does,
// counted here directly so this file's literal pin cannot be satisfied (or
// reddened) by the constraint half.
[[nodiscard]] std::size_t countLiteralRefs(Lir const& lir) {
    std::size_t n = 0;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const bb = lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
                for (auto const& o :
                     lir.instOperands(lir.blockInstAt(bb, i))) {
                    if (o.kind == LirOperandKind::LiteralIndex) ++n;
                }
            }
        }
    }
    return n;
}

[[nodiscard]] std::size_t countOpcode(Lir const& lir, std::uint16_t opcode) {
    std::size_t n = 0;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const bb = lir.funcBlockAt(fn, bi);
            for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
                if (lir.instOpcode(lir.blockInstAt(bb, i)) == opcode) ++n;
            }
        }
    }
    return n;
}

[[nodiscard]] std::size_t countDiags(DiagnosticReporter const& rep,
                                     DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.code == code) ++n;
    }
    return n;
}

// Assert a constraint set came through a copy with its NAMES and its
// resolved ORDINALS intact. Both are checked because they are two
// representations of one fact and no single consumer reads both.
void expectConstraintEq(ImplicitRegisterConstraint const& a,
                        ImplicitRegisterConstraint const& b) {
    EXPECT_EQ(a.inputNames, b.inputNames);
    EXPECT_EQ(a.outputNames, b.outputNames);
    EXPECT_EQ(a.clobberedNames, b.clobberedNames);
    EXPECT_EQ(a.inputOrdinals, b.inputOrdinals);
    EXPECT_EQ(a.outputOrdinals, b.outputOrdinals);
    EXPECT_EQ(a.clobberedOrdinals, b.clobberedOrdinals);
}

// The full per-pass verdict for a module seeded by `buildSeededModule`:
// exactly one carrier, on an instruction of the EXPECTED opcode, resolving
// to the SAME constraint set, with both pools intact and the paired
// rebuild check clean. Every one of the four passes is held to it.
void expectSideStructuresCarried(Lir const& before, Lir const& after,
                                 std::uint16_t expectedCarrierOpcode,
                                 char const* passName) {
    ASSERT_EQ(after.regConstraintPool().size(),
              before.regConstraintPool().size()) << passName;
    ASSERT_EQ(after.literalPool().size(), before.literalPool().size())
        << passName;

    auto const carriers = collectCarriers(after);
    ASSERT_EQ(carriers.size(), collectCarriers(before).size()) << passName;
    ASSERT_EQ(carriers.size(), 1u) << passName;
    EXPECT_EQ(carriers[0].opcode, expectedCarrierOpcode)
        << passName << ": the handle landed on the wrong instruction — a "
           "rebuilt instruction's side data must ride the id `addInst` "
           "returned, never `lastInst()`";

    // The RESOLVED set, not just the count: a handle that survived but
    // pointed at a different entry would satisfy a count-only assertion.
    expectConstraintEq(
        after.regConstraintPool().at(
            lirRegConstraintIndexForHandle(carriers[0].handle)),
        before.regConstraintPool().at(0));

    DiagnosticReporter rep;
    EXPECT_TRUE(verifyLirRebuild(before, after, passName, rep)) << passName;
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_SideStructureReferenceLost), 0u)
        << passName;
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_SideStructureIndexDangling), 0u)
        << passName;
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_SideStructurePoolShrank), 0u)
        << passName;
}

// `f() { v1 = mov #7; v2 = add v1, #3; ret v2 }` with the constraint on
// the `add`. `add` declares `requires2Address`, and v2 != v1, so
// `legalizeTwoAddress` PREPENDS an implicit `mov v2, v1` — the 1 → 2
// mapping whose correspondent is the SECOND instruction.
struct TwoAddressModule {
    Lir           lir;
    std::uint16_t addOpcode = 0;
};

[[nodiscard]] TwoAddressModule buildTwoAddressModule() {
    TwoAddressModule out{Lir{}, op("add")};
    LirBuilder b{*x86Schema()};
    std::uint32_t const ci =
        b.regConstraintPoolAdd(sampleConstraint("rcx", "rdx", "rdx"));
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const v1 = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const movOps{LirOperand::makeImmInt32(7)};
    (void)b.addInst(op("mov"), v1, movOps);
    LirReg const v2 = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 2> const addOps{LirOperand::makeReg(v1),
                                           LirOperand::makeImmInt32(3)};
    LirInstId const add = b.addInst(out.addOpcode, v2, addOps);
    b.setInstRegConstraints(add, ci);
    std::array<LirOperand, 1> const retOps{LirOperand::makeReg(v2)};
    b.addReturn(op("ret"), retOps);
    out.lir = std::move(b).finish();
    return out;
}

// `f() { v1 = call @2; v2 = call @3; v3 = add v1, v2; ret v3 }` with the
// constraint on the FIRST call. v1 must survive the second call, so
// callconv's call arm emits a result-capture move AFTER the call it
// rebuilt — the 1 → (arg setup) + call + (capture) mapping whose
// correspondent is in the MIDDLE.
struct CallModule {
    Lir           lir;
    std::uint16_t callOpcode = 0;
};

[[nodiscard]] CallModule buildCallModule() {
    CallModule out{Lir{}, op("call")};
    LirBuilder b{*x86Schema()};
    std::uint32_t const ci =
        b.regConstraintPoolAdd(sampleConstraint("rcx", "rdx", "rdx"));
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const v1 = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const call1{LirOperand::makeSymbolRef(2)};
    LirInstId const c1 = b.addInst(out.callOpcode, v1, call1,
                                   call_payload::encode(false, 0));
    b.setInstRegConstraints(c1, ci);
    LirReg const v2 = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const call2{LirOperand::makeSymbolRef(3)};
    (void)b.addInst(out.callOpcode, v2, call2, call_payload::encode(false, 0));
    LirReg const v3 = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 2> const addOps{LirOperand::makeReg(v1),
                                           LirOperand::makeReg(v2)};
    (void)b.addInst(op("add"), v3, addOps);
    std::array<LirOperand, 1> const retOps{LirOperand::makeReg(v3)};
    b.addReturn(op("ret"), retOps);
    out.lir = std::move(b).finish();
    return out;
}

// A function with more simultaneously-live values than the target has
// allocatable GPRs, so the allocator MUST spill — which is what makes
// `rewriteWithAllocation` emit a `frame_store` immediately AFTER the
// instruction it just rebuilt.
//
// ⚠ EVERY instruction carries its own constraint set, INCLUDING the `mov`s.
// An earlier draft seeded only the `add`s and the pin did not go red under
// the misplacement mutation: the `add` results are consumed by the very
// next instruction, so none of them ever spills — the spills all belong to
// the long-lived `mov` results. A pin whose subject is never the
// instruction the bug touches measures nothing, which is why the test below
// asserts that a CARRIER is actually followed by a spill store rather than
// that the module contains one somewhere.
struct PressureModule {
    Lir           lir;
    std::uint16_t addOpcode    = 0;
    std::uint16_t movOpcode    = 0;
    std::size_t   carrierCount = 0;
};

[[nodiscard]] PressureModule buildHighPressureModule(std::uint32_t n) {
    PressureModule out{Lir{}, op("add"), op("mov"), 0};
    LirBuilder b{*x86Schema()};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    auto seed = [&](LirInstId inst) {
        b.setInstRegConstraints(
            inst, b.regConstraintPoolAdd(sampleConstraint("rcx", "rdx", "rdx")));
        ++out.carrierCount;
    };
    std::vector<LirReg> vals;
    vals.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        LirReg const v = b.newVReg(LirRegClass::GPR);
        std::array<LirOperand, 1> const movOps{
            LirOperand::makeImmInt32(static_cast<std::int32_t>(i + 1))};
        seed(b.addInst(out.movOpcode, v, movOps));
        vals.push_back(v);
    }
    // Consumed in REVERSE definition order, so every value stays live from
    // its definition to the far end of the function: at the last `mov` all
    // `n` are live at once.
    LirReg acc = vals.back();
    for (std::uint32_t i = n - 1; i-- > 0;) {
        LirReg const next = b.newVReg(LirRegClass::GPR);
        std::array<LirOperand, 2> const addOps{LirOperand::makeReg(acc),
                                               LirOperand::makeReg(vals[i])};
        seed(b.addInst(out.addOpcode, next, addOps));
        acc = next;
    }
    std::array<LirOperand, 1> const retOps{LirOperand::makeReg(acc)};
    b.addReturn(op("ret"), retOps);
    out.lir = std::move(b).finish();
    return out;
}

// A minimal REBUILD, written exactly the way the four production passes
// write theirs: fresh builder → ONE `copyModuleSideStructures` →
// per-instruction re-emit → `carryInstSideData` per instruction.
[[nodiscard]] Lir rebuildModule(Lir const& src, bool carrySideData) {
    LirBuilder b{*x86Schema()};
    lir_pass_util::copyModuleSideStructures(src, b);
    DiagnosticReporter rep;
    for (std::uint32_t fi = 0; fi < src.moduleFuncCount(); ++fi) {
        LirFuncId const srcFn = src.funcAt(fi);
        (void)b.addFunction(src.funcSymbol(srcFn));
        std::unordered_map<std::uint32_t, LirBlockId> srcToDst;
        std::uint32_t const blockCount = src.funcBlockCount(srcFn);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            srcToDst[src.funcBlockAt(srcFn, bi).v] = b.createBlock();
        }
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const srcBlk = src.funcBlockAt(srcFn, bi);
            b.beginBlock(srcToDst[srcBlk.v]);
            for (std::uint32_t ii = 0; ii < src.blockInstCount(srcBlk); ++ii) {
                LirInstId const inst = src.blockInstAt(srcBlk, ii);
                auto const  opcode = src.instOpcode(inst);
                auto const* info   = x86Schema()->opcodeInfo(opcode);
                std::vector<LirOperand> newOps;
                for (auto const& o : src.instOperands(inst)) {
                    newOps.push_back(lir_pass_util::remapBlockRef(o, srcToDst));
                }
                if (info != nullptr && info->isTerminator()) {
                    EXPECT_TRUE(lir_pass_util::emitTerminator(
                        b, opcode, info, src.blockSuccessors(srcBlk), newOps,
                        src.instPayload(inst), src.instFlags(inst), srcToDst,
                        "test-rebuild", rep));
                } else {
                    (void)b.addInst(opcode, src.instResult(inst), newOps,
                                    src.instPayload(inst),
                                    src.instFlags(inst));
                }
                if (carrySideData) {
                    lir_pass_util::carryInstSideData(src, inst, b);
                }
            }
        }
    }
    return std::move(b).finish();
}

// The post-regalloc half of the real pipeline, shared by the tests that
// need a module `rewriteWithAllocation` / `materializeCallingConvention`
// will accept. Returns the allocation alongside, since callconv needs it.
struct Allocated {
    LirAllocation alloc;
    bool          ok = false;
};

[[nodiscard]] Allocated allocate(Lir const& src) {
    LirLiveness const liveness = analyzeLiveness(src);
    DiagnosticReporter rep;
    Allocated out{allocateRegisters(src, *x86Schema(), liveness,
                                    /*ccIndex=*/0, rep),
                  false};
    out.ok = out.alloc.ok();
    return out;
}

} // namespace

// ── the shared copy helper ───────────────────────────────────────────

TEST(LirPassUtil, CopyModuleSideStructuresCarriesBothPoolsIndexForIndex) {
    auto const seeded = buildSeededModule();
    LirBuilder dst{*x86Schema()};
    lir_pass_util::copyModuleSideStructures(seeded.lir, dst);
    (void)dst.addFunction(SymbolId{1});
    LirBlockId const e = dst.createBlock();
    dst.beginBlock(e);
    dst.addReturn(op("ret"), std::span<LirOperand const>{});
    Lir const out = std::move(dst).finish();

    ASSERT_EQ(out.literalPool().size(), seeded.lir.literalPool().size());
    ASSERT_EQ(out.regConstraintPool().size(),
              seeded.lir.regConstraintPool().size());
    EXPECT_EQ(std::get<std::int64_t>(
                  out.literalPool().at(seeded.literalIndex).value),
              0x1122334455667788LL);
    expectConstraintEq(out.regConstraintPool().at(seeded.constraintIndex),
                       seeded.lir.regConstraintPool().at(seeded.constraintIndex));
}

// ── the per-instruction handle ───────────────────────────────────────

TEST(LirPassUtil, CarryInstSideDataPreservesTheHandleAcrossARebuild) {
    auto const seeded = buildSeededModule();
    ASSERT_EQ(countConstraintCarriers(seeded.lir), 1u);

    Lir const rebuilt = rebuildModule(seeded.lir, /*carrySideData=*/true);
    EXPECT_EQ(countConstraintCarriers(rebuilt), 1u);
    EXPECT_EQ(rebuilt.regConstraintPool().size(), 1u);

    DiagnosticReporter rep;
    EXPECT_TRUE(verifyLirRebuild(seeded.lir, rebuilt, "test-rebuild", rep))
        << "a rebuild that copies both pools and carries every per-instruction "
           "handle must verify clean";

    // The RESOLVED set, not just the count: a handle that survived but
    // pointed at a different entry would satisfy a count-only assertion.
    LirFuncId const fn = rebuilt.funcAt(0);
    LirBlockId const bb = rebuilt.funcBlockAt(fn, 0);
    auto const* c = rebuilt.instRegConstraints(rebuilt.blockInstAt(bb, 0));
    ASSERT_NE(c, nullptr);
    expectConstraintEq(*c, seeded.lir.regConstraintPool().at(
                               seeded.constraintIndex));
}

// ★★ THE BACKSTOP, EXERCISED — NOT READ. A rebuild that copies the pools
// but forgets `carryInstSideData` produces a module that is structurally
// perfect: nothing dangles, no pool shrank, the handle is the perfectly
// legal `kLirNoRegConstraints`. The ONLY visible trace is the pool entry
// that no instruction references, which is exactly what the verifier rule
// asserts.
TEST(LirPassUtil, ForgettingCarryInstSideDataIsCaughtByTheVerifier) {
    auto const seeded = buildSeededModule();
    Lir const dropped = rebuildModule(seeded.lir, /*carrySideData=*/false);

    // The silent part: the module looks fine by every other measure.
    EXPECT_EQ(dropped.regConstraintPool().size(), 1u);
    EXPECT_EQ(countConstraintCarriers(dropped), 0u);

    DiagnosticReporter rep;
    EXPECT_FALSE(verifyLirRebuild(seeded.lir, dropped, "test-rebuild", rep));
    EXPECT_GT(countDiags(rep, DiagnosticCode::L_SideStructureReferenceLost), 0u);
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_SideStructureIndexDangling), 0u)
        << "nothing dangles — that is the point: the drop is invisible to "
           "every check except the unreferenced-entry rule";
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_SideStructurePoolShrank), 0u)
        << "no pool shrank either — the shared helper carried both";
}

// ── survival across each of the FOUR production rebuild passes ───────
//
// ⚠ Asserted after EACH pass individually, on that pass's own output. A
// single end-of-pipeline assertion would let a drop in pass 1 be masked by
// a restore in pass 4.

TEST(LirPassUtil, SideStructuresSurviveLegalizeTwoAddress) {
    auto const seeded = buildSeededModule();
    DiagnosticReporter rep;
    auto const out = legalizeTwoAddress(seeded.lir, *x86Schema(), rep);
    ASSERT_TRUE(out.ok());
    expectSideStructuresCarried(seeded.lir, out.lir, op("mov"),
                                "legalizeTwoAddress");
}

TEST(LirPassUtil, SideStructuresSurviveLowerWideCallArgs) {
    auto const seeded = buildSeededModule();
    DiagnosticReporter rep;
    auto const out = lowerWideCallArgs(seeded.lir, *x86Schema(),
                                       /*ccIndex=*/0, rep);
    ASSERT_TRUE(out.ok);
    expectSideStructuresCarried(seeded.lir, out.lir, op("mov"),
                                "lowerWideCallArgs");
}

TEST(LirPassUtil, SideStructuresSurviveRewriteWithAllocation) {
    auto const seeded = buildSeededModule();
    auto const allocated = allocate(seeded.lir);
    ASSERT_TRUE(allocated.ok);
    DiagnosticReporter rep;
    auto const out = rewriteWithAllocation(seeded.lir, *x86Schema(),
                                           allocated.alloc, rep);
    ASSERT_TRUE(out.ok);
    expectSideStructuresCarried(seeded.lir, out.lir, op("mov"),
                                "rewriteWithAllocation");
}

TEST(LirPassUtil, SideStructuresSurviveMaterializeCallingConvention) {
    auto const seeded = buildSeededModule();
    auto const allocated = allocate(seeded.lir);
    ASSERT_TRUE(allocated.ok);
    DiagnosticReporter rewriteRep;
    auto const rewritten = rewriteWithAllocation(seeded.lir, *x86Schema(),
                                                 allocated.alloc, rewriteRep);
    ASSERT_TRUE(rewritten.ok);
    DiagnosticReporter rep;
    auto const out = materializeCallingConvention(rewritten.lir, *x86Schema(),
                                                  allocated.alloc, rep);
    ASSERT_TRUE(out.ok());
    expectSideStructuresCarried(rewritten.lir, out.lir, op("mov"),
                                "materializeCallingConvention");
}

// ── the handle must land on the RIGHT instruction ────────────────────
//
// ★★★ THE PART A REFERENCE COUNT CANNOT SEE. Three of the four passes map
// one source instruction onto SEVERAL destination instructions, and in two
// of them the correspondent is NOT the last one appended. A carry written
// as `carryInstSideData(src, inst, b)` (the `lastInst()` overload) would
// attach the constraint set to a compiler-synthesized move or spill store:
// the census still counts one reference, every pool index still resolves,
// and the clobber set now guards the wrong instruction.

TEST(LirPassUtil, LegalizeCarriesOntoTheOperationNotTheImplicitCopy) {
    auto const seeded = buildTwoAddressModule();
    ASSERT_EQ(countConstraintCarriers(seeded.lir), 1u);
    std::size_t const movsBefore = countOpcode(seeded.lir, op("mov"));

    DiagnosticReporter rep;
    auto const out = legalizeTwoAddress(seeded.lir, *x86Schema(), rep);
    ASSERT_TRUE(out.ok());

    // The pass must actually have INSERTED the implicit copy, or this test
    // measures nothing.
    ASSERT_EQ(countOpcode(out.lir, op("mov")), movsBefore + 1u)
        << "legalizeTwoAddress did not synthesize the 2-address copy — the "
           "1 → 2 mapping this test exists to pin was never exercised";

    auto const carriers = collectCarriers(out.lir);
    ASSERT_EQ(carriers.size(), 1u);
    EXPECT_EQ(carriers[0].opcode, seeded.addOpcode)
        << "the constraint set belongs to the OPERATION, not to the implicit "
           "copy legalize prepended in front of it";
}

TEST(LirPassUtil, RewriteCarriesOntoTheInstructionNotItsSpillStore) {
    auto const seeded = buildHighPressureModule(24);
    ASSERT_EQ(countConstraintCarriers(seeded.lir), seeded.carrierCount);

    auto const allocated = allocate(seeded.lir);
    ASSERT_TRUE(allocated.ok);
    DiagnosticReporter rep;
    auto const out = rewriteWithAllocation(seeded.lir, *x86Schema(),
                                           allocated.alloc, rep);
    ASSERT_TRUE(out.ok);

    auto const carriers = collectCarriers(out.lir);
    ASSERT_EQ(carriers.size(), seeded.carrierCount);

    // The verdict first, so a broken carry reports its own cause rather
    // than the exercise check's.
    for (auto const& c : carriers) {
        EXPECT_TRUE(c.opcode == seeded.addOpcode || c.opcode == seeded.movOpcode)
            << "a constraint set landed on a spill store — the rewriter emits "
               "`frame_store` AFTER the instruction it rebuilt, so the carry "
               "must be bound to that instruction's own id, not to whatever "
               "the iteration appended last";
    }

    // ★ THE EXERCISE CHECK, and it has to be this specific: at least one
    // CARRYING instruction must be immediately followed by its spill store.
    // "the module contains a frame_store somewhere" is NOT enough — the
    // spills can all belong to instructions that carry nothing, and then a
    // misplaced carry is invisible and this test is decorative. (That was
    // the first draft, and a mutation proved it green.)
    LirFuncId const fn = out.lir.funcAt(0);
    LirBlockId const bb = out.lir.funcBlockAt(fn, 0);
    std::uint16_t const frameStore = op("frame_store");
    std::size_t carriersFollowedByASpillStore = 0;
    for (auto const& c : carriers) {
        if (c.indexInBlock + 1u >= out.lir.blockInstCount(bb)) continue;
        if (out.lir.instOpcode(out.lir.blockInstAt(bb, c.indexInBlock + 1u))
            == frameStore) {
            ++carriersFollowedByASpillStore;
        }
    }
    ASSERT_GT(carriersFollowedByASpillStore, 0u)
        << "no constraint-carrying instruction is followed by its spill store "
           "— either nothing spilled (raise the live-value count) or the "
           "carriers ARE the spill stores (see the opcode failures above)";
    DiagnosticReporter vRep;
    EXPECT_TRUE(verifyLirRebuild(seeded.lir, out.lir, "rewrite", vRep));
}

TEST(LirPassUtil, CallconvCarriesOntoTheCallNotItsResultCapture) {
    auto const seeded = buildCallModule();
    ASSERT_EQ(countConstraintCarriers(seeded.lir), 1u);

    auto const allocated = allocate(seeded.lir);
    ASSERT_TRUE(allocated.ok);
    DiagnosticReporter rewriteRep;
    auto const rewritten = rewriteWithAllocation(seeded.lir, *x86Schema(),
                                                 allocated.alloc, rewriteRep);
    ASSERT_TRUE(rewritten.ok);
    auto const rewrittenCarriers = collectCarriers(rewritten.lir);
    ASSERT_EQ(rewrittenCarriers.size(), 1u);
    EXPECT_EQ(rewrittenCarriers[0].opcode, seeded.callOpcode);

    DiagnosticReporter rep;
    auto const out = materializeCallingConvention(rewritten.lir, *x86Schema(),
                                                  allocated.alloc, rep);
    ASSERT_TRUE(out.ok());

    auto const carriers = collectCarriers(out.lir);
    ASSERT_EQ(carriers.size(), 1u);
    EXPECT_EQ(carriers[0].opcode, seeded.callOpcode)
        << "the constraint set landed on the return-value capture move — "
           "callconv emits it AFTER the call it rebuilt, so the carry must "
           "use the id `addInst` returned, not `lastInst()`";
    // The capture move must actually exist, or the ordering hazard this
    // test is about was never present in the output.
    LirFuncId const fn = out.lir.funcAt(0);
    LirBlockId const bb = out.lir.funcBlockAt(fn, 0);
    ASSERT_LT(carriers[0].indexInBlock + 1u, out.lir.blockInstCount(bb));
    EXPECT_EQ(out.lir.instOpcode(
                  out.lir.blockInstAt(bb, carriers[0].indexInBlock + 1u)),
              op("mov"))
        << "callconv did not emit a result-capture move after the call — the "
           "1 → (setup) + call + (capture) mapping was not exercised";
}

// ── builder-side invariants ──────────────────────────────────────────

TEST(LirPassUtil, RegConstraintPoolAddResolvesNamesToOrdinals) {
    LirBuilder b{*x86Schema()};
    // Ordinals deliberately supplied WRONG: the builder re-derives them,
    // so a caller cannot desynchronise the two representations.
    ImplicitRegisterConstraint c = sampleConstraint("rcx", "rdx", "rdx");
    c.inputOrdinals = {9999};
    c.outputOrdinals.clear();
    std::uint32_t const idx = b.regConstraintPoolAdd(std::move(c));
    (void)b.addFunction(SymbolId{1});
    LirBlockId const e = b.createBlock();
    b.beginBlock(e);
    b.addReturn(op("ret"), std::span<LirOperand const>{});
    Lir const lir = std::move(b).finish();

    auto const& stored = lir.regConstraintPool().at(idx);
    ASSERT_EQ(stored.inputOrdinals.size(), 1u);
    ASSERT_EQ(stored.outputOrdinals.size(), 1u);
    ASSERT_EQ(stored.clobberedOrdinals.size(), 1u);
    EXPECT_EQ(stored.inputOrdinals[0], *x86Schema()->registerByName("rcx"));
    EXPECT_EQ(stored.outputOrdinals[0], *x86Schema()->registerByName("rdx"));
    EXPECT_EQ(stored.clobberedOrdinals[0], *x86Schema()->registerByName("rdx"));
}

TEST(LirPassUtilDeathTest, RegConstraintPoolAddRejectsAnUnknownRegisterName) {
    LirBuilder b{*x86Schema()};
    EXPECT_DEATH(
        (void)b.regConstraintPoolAdd(sampleConstraint("rax", "rax", "nosuchreg")),
        "not in the target schema's register table");
}

// ── `outputs ⊆ clobbered` on the PER-INSTRUCTION carrier ─────────────
//
// D-LIR-PER-INSTRUCTION-OUTPUTS-NOT-ENFORCED-SUBSET-OF-CLOBBERED. The
// register allocator's forbidden set is `inputs ∪ clobbered` and OUTPUTS
// ARE DELIBERATELY OMITTED (`lir_regalloc.cpp` says so outright: the op
// reads its operands before it writes its outputs, so a same-register
// overlap is fine). That omission is safe ONLY because every output is
// also a clobber — a rule the `.target.json` loader enforces for the
// per-OPCODE carrier and which, until this cycle, NOTHING enforced for
// the per-INSTRUCTION one. A lowering that pinned an output without
// clobbering it produced a constraint the allocator would not exclude,
// and a value live in that register died with no diagnostic.
//
// ⚠ THE POSITIVE CONTROL IS HALF THE TEST. A refusal arm alone passes on
// a builder that refuses EVERY constraint set, which would break the
// whole per-instruction carrier while looking like a hardened one.
TEST(LirPassUtilDeathTest, RegConstraintPoolAddRejectsAnOutputThatIsNotClobbered) {
    LirBuilder b{*x86Schema()};
    EXPECT_DEATH(
        // `rax` is declared an implicit OUTPUT while only `rdx` is
        // clobbered — the exact shape an inline-asm lowering produces
        // when it materialises a register-pinned `"=a"` output and
        // forgets to enter it into the clobber set.
        (void)b.regConstraintPoolAdd(sampleConstraint("rcx", "rax", "rdx")),
        "is not in this constraint's clobbered set");
}

TEST(LirPassUtil, RegConstraintPoolAddAcceptsAnOutputThatIsAlsoClobbered) {
    LirBuilder b{*x86Schema()};
    // The idiv shape: `rdx` is an implicit output AND a clobber, which is
    // legal and must stay legal — the fix must not pessimise the compound
    // ops whose output legitimately aliases a declared register.
    ImplicitRegisterConstraint c = sampleConstraint("rcx", "rdx", "rdx");
    c.clobberedNames.emplace_back("rax");   // a clobber that is NOT an output
    std::uint32_t const idx = b.regConstraintPoolAdd(std::move(c));

    (void)b.addFunction(SymbolId{1});
    LirBlockId const e = b.createBlock();
    b.beginBlock(e);
    b.addReturn(op("ret"), std::span<LirOperand const>{});
    Lir const lir = std::move(b).finish();

    auto const& stored = lir.regConstraintPool().at(idx);
    ASSERT_EQ(stored.outputOrdinals.size(), 1u);
    EXPECT_EQ(stored.outputOrdinals[0], *x86Schema()->registerByName("rdx"));
    EXPECT_EQ(stored.clobberedOrdinals.size(), 2u)
        << "the rule is a SUBSET rule — a clobber that is not an output must "
           "still be accepted";
    EXPECT_FALSE(stored.firstOutputNotClobbered().has_value());
}

// The predicate itself, exercised on the shape that made the rule
// necessary and on the one that satisfies it. `firstOutputNotClobbered`
// is what a producer fed by USER text must call BEFORE the builder — the
// builder aborts, and a malformed program owes a diagnostic — so it is a
// contract in its own right, not an implementation detail of the abort.
TEST(LirPassUtil, FirstOutputNotClobberedNamesTheOffendingIndex) {
    ImplicitRegisterConstraint c;
    c.outputOrdinals    = {7, 9};
    c.clobberedOrdinals = {7};
    auto const bad = c.firstOutputNotClobbered();
    ASSERT_TRUE(bad.has_value());
    EXPECT_EQ(*bad, 1u) << "the FIRST unclobbered output is index 1, not 0";

    c.clobberedOrdinals.push_back(9);
    EXPECT_FALSE(c.firstOutputNotClobbered().has_value());
}

TEST(LirPassUtilDeathTest, SetInstRegConstraintsRejectsAnOutOfRangeIndex) {
    LirBuilder b{*x86Schema()};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const e = b.createBlock();
    b.beginBlock(e);
    LirReg const r = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const ops{LirOperand::makeImmInt32(1)};
    LirInstId const inst = b.addInst(op("mov"), r, ops);
    EXPECT_DEATH(b.setInstRegConstraints(inst, 0), "pool index out of range");
}

TEST(LirPassUtilDeathTest, LastInstRejectsAnEmptyArena) {
    LirBuilder b{*x86Schema()};
    EXPECT_DEATH((void)b.lastInst(), "no instruction has been appended");
}

// ── the LITERAL pool's own survival, still pinned after the rename ───
//
// The rename `copyLiteralPool` → `copyModuleSideStructures` must not have
// quietly narrowed what the literal half does. Asserted on each of the
// four passes' own output, same discipline as the constraint pool above.

TEST(LirPassUtil, LiteralPoolStillSurvivesEachRebuildPassAfterTheRename) {
    auto const seeded = buildSeededModule();
    auto const& src = seeded.lir;
    ASSERT_EQ(src.literalPool().size(), 1u);

    // ⚠ The REFERENCE must survive too, not merely the entry: a pass that
    // kept the pool but dropped the `LiteralIndex` operand leaves the pool
    // intact and every surviving index resolving, so the count is the only
    // place the loss shows. Counted directly rather than through
    // `verifyLirRebuild`, so this pin measures the LITERAL half only — its
    // constraint half belongs to the four `SideStructuresSurvive*` tests, and
    // a pin that reds for two different reasons names one of them wrongly.
    // (It previously counted diagnostics on a reporter nothing had written
    // to, which is a check that cannot fail.)
    auto expectLiteralIntact = [](Lir const& before, Lir const& after,
                                  char const* what) {
        ASSERT_EQ(after.literalPool().size(), 1u) << what;
        EXPECT_EQ(std::get<std::int64_t>(after.literalPool().at(0).value),
                  0x1122334455667788LL) << what;
        EXPECT_EQ(countLiteralRefs(after), countLiteralRefs(before)) << what;
    };

    DiagnosticReporter rep2addr;
    auto const legalized = legalizeTwoAddress(src, *x86Schema(), rep2addr);
    ASSERT_TRUE(legalized.ok());
    expectLiteralIntact(src, legalized.lir, "legalizeTwoAddress");

    DiagnosticReporter repWide;
    auto const wide = lowerWideCallArgs(src, *x86Schema(), 0, repWide);
    ASSERT_TRUE(wide.ok);
    expectLiteralIntact(src, wide.lir, "lowerWideCallArgs");

    auto const allocated = allocate(src);
    ASSERT_TRUE(allocated.ok);
    DiagnosticReporter repRewrite;
    auto const rewritten =
        rewriteWithAllocation(src, *x86Schema(), allocated.alloc, repRewrite);
    ASSERT_TRUE(rewritten.ok);
    expectLiteralIntact(src, rewritten.lir, "rewriteWithAllocation");

    DiagnosticReporter repCc;
    auto const cc = materializeCallingConvention(rewritten.lir, *x86Schema(),
                                                 allocated.alloc, repCc);
    ASSERT_TRUE(cc.ok());
    expectLiteralIntact(rewritten.lir, cc.lir, "materializeCallingConvention");
}

// ★★ THE PAIRED REBUILD CHECK RUN AGAINST THE **REAL** PIPELINE, on a
// module a real lowering produced rather than one this file hand-built.
//
// The hand-built modules above are a handful of instructions; the four
// passes barely have to do anything to them, so they cannot witness a
// reference dropped by (say) callconv's arg materialization. A lowered
// function that genuinely carries a wide literal through regalloc,
// rewrite and callconv can.
TEST(LirPassUtil, RealPipelineRebuildsPreserveEverySideStructureReference) {
    auto lowered = test_support::lowerCSubsetToLir(
        "long long f(long long x) { return x + 0x1122334455667788LL; }");
    ASSERT_TRUE(lowered.lir.ok);
    Lir const& src = lowered.lir.lir;
    TargetSchema const& sch = *lowered.target;
    ASSERT_GT(src.literalPool().size(), 0u)
        << "the fixture must actually carry a wide literal, or this test "
           "measures nothing";

    DiagnosticReporter repWide;
    auto const wide = lowerWideCallArgs(src, sch, /*ccIndex=*/0, repWide);
    ASSERT_TRUE(wide.ok);
    DiagnosticReporter vWide;
    EXPECT_TRUE(verifyLirRebuild(src, wide.lir, "wide-call-args", vWide))
        << "lowerWideCallArgs dropped a side-structure reference";

    LirLiveness const liveness = analyzeLiveness(wide.lir);
    DiagnosticReporter allocRep;
    LirAllocation const alloc =
        allocateRegisters(wide.lir, sch, liveness, /*ccIndex=*/0, allocRep);
    ASSERT_TRUE(alloc.ok());

    DiagnosticReporter repRewrite;
    auto const rewritten = rewriteWithAllocation(wide.lir, sch, alloc, repRewrite);
    ASSERT_TRUE(rewritten.ok);
    DiagnosticReporter vRewrite;
    EXPECT_TRUE(verifyLirRebuild(wide.lir, rewritten.lir, "rewrite", vRewrite))
        << "rewriteWithAllocation dropped a side-structure reference";

    DiagnosticReporter repCc;
    auto const cc = materializeCallingConvention(rewritten.lir, sch, alloc, repCc);
    ASSERT_TRUE(cc.ok());
    DiagnosticReporter vCc;
    EXPECT_TRUE(verifyLirRebuild(rewritten.lir, cc.lir, "callconv", vCc))
        << "materializeCallingConvention dropped a side-structure reference";

    DiagnosticReporter rep2addr;
    auto const legalized = legalizeTwoAddress(cc.lir, sch, rep2addr);
    ASSERT_TRUE(legalized.ok());
    DiagnosticReporter v2addr;
    EXPECT_TRUE(verifyLirRebuild(cc.lir, legalized.lir, "2addr", v2addr))
        << "legalizeTwoAddress dropped a side-structure reference";
}
