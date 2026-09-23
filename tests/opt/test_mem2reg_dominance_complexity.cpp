// D-OPT-MEM2REG-WHOLE-MODULE-DOMINANCE-PER-FUNCTION — the COMPLEXITY pin.
//
// ★★★ WHAT WENT WRONG. Mem2Reg asks, for each function it promotes in, for
// that ONE function's dominator tree, dominance frontier and dominator
// children. It used to answer by building the predecessor map and all three
// structures FRESH, per function — and every one of those helpers sweeps and
// allocates the WHOLE module. So the pass cost O(functions × module blocks),
// quadratic in the size of the translation unit.
//
// ✔MEASURED before the fix (WSL, Release `dsscp`, `--config=release`, the
// sqlite amalgamation cut at top-level file boundaries into growing prefixes):
// the unit pipeline's first Mem2Reg took 168 ms at 46,784 MIR instructions,
// 2,498 ms at 227,374 and 20,105 ms at 590,178 — 20.1 s of a 33.8 s optimize —
// and every live gdb sample taken inside it sat in `mirBuildPredecessors`,
// `mirDominanceFrontier` or the destructor of their module-sized results.
// gcc -O2 and clang -O2 grow 4.7x and 12.4x over the same prefixes; that pass
// grew 120x. CSE, LICM and the marker re-derivation had been threaded through
// a hoisted predecessor map and `MirDomScratch` long before
// (D-OPT-DOMTREE-SCRATCH-REUSE); Mem2Reg was the caller left behind.
//
// ★★★ WHY THIS PIN COUNTS AND DOES NOT TIME. A stopwatch assertion is sized on
// the machine that wrote it and reds on the slowest leg that runs it —
// `.harness-config/runner/actions/check-wall-clock-in-tests/` refuses new ones.
// What is asserted here is `mirDomSlotsSweptTake()`: the block slots the
// forward-dominator helpers swept on this thread while the pass ran. It is
// deterministic, identical in Debug and Release, and independent of load. It
// is counted INSIDE the shared helpers, so it sees whatever a caller does with
// them — a caller that goes back to the fresh overloads per function cannot
// hide from it.
//
// ★★ THE ARMS, AND WHY NONE OF THEM CAN GO VACUOUS.
//   * PREMISE — every function really reaches the dominance step. Re-derived
//     from the OUTPUT module (one Phi per function, no Alloca left), not read
//     back from the pass's own counters, because an instrument confirming its
//     own premise is how a pin ends up testing its helper. Without this arm, a
//     change that stopped promoting would leave the work arms passing over a
//     pass that never asked for dominance at all.
//   * WORK — the sweep is bounded by a constant times the module's block count.
//   * GROWTH — doubling the module at most doubles the sweep (linear), where
//     the quadratic shape quadruples it.
//   * CONTROL — the promotion DECISIONS are unchanged. Reverting the fix leaves
//     them identical, so this arm stays GREEN under the mutant, which is what
//     makes the work arms' red evidence about COST specifically rather than
//     about "the mutant broke something".

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/mem2reg.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>

using namespace dss;

namespace {

// `n` functions, each `int f(bool c) { int x; if (c) x = 1; else x = 2;
// return x; }` as MIR: a four-block diamond whose single alloca is stored on
// both arms and loaded at the join, so promoting it needs exactly one Phi —
// and therefore the dominance frontier. Every function is identical, so the
// module's size is the ONLY thing that changes between the n and 2n runs.
Mir buildDiamondModule(TypeInterner& interner, std::uint32_t n) {
    TypeId const i32      = interner.primitive(TypeKind::I32);
    TypeId const boolT    = interner.primitive(TypeKind::Bool);
    TypeId const ptr      = interner.pointer(i32);
    TypeId const params[] = {boolT};
    TypeId const fnSig    = interner.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder mb;
    for (std::uint32_t k = 0; k < n; ++k) {
        mb.addFunction(fnSig, SymbolId{100u + k});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const tArm  = mb.createBlock(StructCfMarker::IfThen);
        MirBlockId const fArm  = mb.createBlock(StructCfMarker::IfElse);
        MirBlockId const join  = mb.createBlock(StructCfMarker::IfJoin);

        mb.beginBlock(entry);
        MirInstId const cond = mb.addArg(0, boolT);
        MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptr);
        mb.addCondBr(cond, tArm, fArm);

        MirLiteralValue one; one.value = std::int64_t{1}; one.core = TypeKind::I32;
        MirLiteralValue two; two.value = std::int64_t{2}; two.core = TypeKind::I32;

        mb.beginBlock(tArm);
        MirInstId const c1 = mb.addConst(one, i32);
        MirInstId const s1[] = {c1, slot};
        (void)mb.addInst(MirOpcode::Store, s1, InvalidType);
        mb.addBr(join);

        mb.beginBlock(fArm);
        MirInstId const c2 = mb.addConst(two, i32);
        MirInstId const s2[] = {c2, slot};
        (void)mb.addInst(MirOpcode::Store, s2, InvalidType);
        mb.addBr(join);

        mb.beginBlock(join);
        MirInstId const loadOps[] = {slot};
        MirInstId const ld = mb.addInst(MirOpcode::Load, loadOps, i32);
        mb.addReturn(ld);
    }
    return std::move(mb).finish();
}

std::size_t countOpInModule(Mir const& mir, MirOpcode want) {
    std::size_t n = 0;
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        MirFuncId const f = mir.funcAt(i);
        for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
                if (mir.instOpcode(mir.blockInstAt(b, ii)) == want) ++n;
            }
        }
    }
    return n;
}

struct Measured {
    opt::passes::Mem2RegResult result;
    std::uint64_t              slotsSwept   = 0;
    std::size_t                blockCount   = 0;   // of the INPUT module
    std::size_t                phisOut      = 0;   // re-derived from the OUTPUT
    std::size_t                allocasOut   = 0;
};

Measured runOn(std::uint32_t n) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildDiamondModule(interner, n);
    Measured m;
    m.blockCount = mir.blockCount();
    (void)mirDomSlotsSweptTake();   // zero THIS thread's counter
    DiagnosticReporter rep;
    m.result     = opt::passes::runMem2Reg(mir, interner, rep);
    m.slotsSwept = mirDomSlotsSweptTake();
    m.phisOut    = countOpInModule(mir, MirOpcode::Phi);
    m.allocasOut = countOpInModule(mir, MirOpcode::Alloca);
    return m;
}

// Large enough that the quadratic shape is unmistakable (at 64 functions it
// sweeps ~256 slots per module block), small enough to run in milliseconds.
constexpr std::uint32_t kFunctions = 64;

// The bound, derived rather than tuned. With the fix, one pass sweeps the
// module twice (the hoisted predecessor map, the scratch's one allocation)
// plus a handful of slots per BLOCK of each function (the dominator write
// set, its reset, the frontier and children fills) — about 8 per block on
// this shape. 16 leaves room for a helper to legitimately sweep a little
// more; the quadratic shape sweeps 4 x `kFunctions` = 256 per block here.
constexpr std::uint64_t kSweptPerBlockBound = 16;

} // namespace

TEST(Mem2RegDominanceComplexity, PremiseEveryFunctionReachesTheDominanceStep) {
    Measured const m = runOn(kFunctions);
    ASSERT_TRUE(m.result.ok);
    // Four blocks per function, plus the arena's reserved slot 0.
    EXPECT_EQ(m.blockCount, std::size_t{4} * kFunctions + 1)
        << "the module is not the shape this pin is sized for";
    EXPECT_EQ(m.phisOut, std::size_t{kFunctions})
        << "every function must have needed its join Phi — i.e. asked for its "
           "dominance frontier — or the work arms measure nothing";
    EXPECT_EQ(m.allocasOut, 0u) << "every alloca must have been promoted";
    EXPECT_GT(m.slotsSwept, 0u)
        << "the dominance helpers reported no work at all: the counter is not "
           "wired, and every bound below would pass vacuously";
}

TEST(Mem2RegDominanceComplexity, DominanceWorkIsBoundedByTheModuleNotItsSquare) {
    Measured const m = runOn(kFunctions);
    ASSERT_TRUE(m.result.ok);
    // Printed, not only asserted: the count is the measurement this pin exists
    // to keep honest, and a reader of a `ctest -V` log should not have to
    // re-derive it.
    std::cout << "[mem2reg-dominance] functions=" << kFunctions
              << " moduleBlocks=" << m.blockCount
              << " slotsSwept=" << m.slotsSwept << "\n";
    EXPECT_LE(m.slotsSwept, kSweptPerBlockBound * m.blockCount)
        << "Mem2Reg swept " << m.slotsSwept << " block slots over a module of "
        << m.blockCount << " blocks (" << kFunctions << " functions) — more than "
        << kSweptPerBlockBound << " per block means some function's dominance "
           "answer was computed over the WHOLE module";
}

TEST(Mem2RegDominanceComplexity, DominanceWorkGrowsLinearlyWhenTheModuleDoubles) {
    Measured const small = runOn(kFunctions);
    Measured const big   = runOn(2 * kFunctions);
    ASSERT_TRUE(small.result.ok);
    ASSERT_TRUE(big.result.ok);
    ASSERT_GT(small.slotsSwept, 0u);
    std::cout << "[mem2reg-dominance] slotsSwept " << kFunctions << " functions="
              << small.slotsSwept << ", " << 2 * kFunctions << " functions="
              << big.slotsSwept << "\n";
    // Linear: x2.0 exactly on this shape (every term scales with the module).
    // Quadratic: x4. The 9/4 threshold sits between them with room either way.
    EXPECT_LE(big.slotsSwept * 4, small.slotsSwept * 9)
        << "doubling the module took the dominance sweep from "
        << small.slotsSwept << " to " << big.slotsSwept << " slots";
}

TEST(Mem2RegDominanceComplexity, PromotionDecisionsAreUnchangedControl) {
    Measured const m = runOn(kFunctions);
    ASSERT_TRUE(m.result.ok);
    EXPECT_EQ(m.result.allocasPromoted,  std::size_t{kFunctions});
    EXPECT_EQ(m.result.phisInserted,     std::size_t{kFunctions});
    EXPECT_EQ(m.result.loadsReplaced,    std::size_t{kFunctions});
    EXPECT_EQ(m.result.storesEliminated, std::size_t{2} * kFunctions);
}
