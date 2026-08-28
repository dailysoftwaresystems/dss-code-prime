// D-LIR-2ADDR-IGNORES-EMIT-TERMINATOR-FAILURE — A REFUSAL THAT CRASHES IS NOT
// A REFUSAL.
//
// ★★★ THE SUBJECT IS A CLASS, NOT A CALL SITE. Five passes rebuild a `Lir`
// module into a fresh `LirBuilder`, and every one of them emits its
// terminators through the SAME shared dispatch,
// `lir_pass_util::emitTerminator`:
//
//     legalizeTwoAddress            (lir_2addr_legalize.cpp)
//     runLirPeephole                (lir_peephole.cpp)
//     lowerWideCallArgs             (lir_wide_call_args.cpp)
//     rewriteWithAllocation         (lir_rewrite.cpp)
//     materializeCallingConvention  (lir_callconv.cpp)
//
// A refusal from that dispatch appends NOTHING, so the block the pass opened
// ends without a terminator and the builder becomes a loaded gun: the next
// `beginBlock` fatals on "current block has no terminator" and
// `addFunction`/`finish()` fatal inside `closeFunction_`. The anchor was filed
// against ONE of the five; the other four have always had the identical shape
// and had no pin at all, which is the part worth guarding — the next omission
// will not be in the site the row happens to name.
//
// ★★ RED-ON-DISABLE HERE NEEDS NO ASSERTION TO READ IT. The pre-fix
// behaviour is not a failed `EXPECT`, it is `dss::Lir fatal:
// LirBuilder::closeFunction: block's last instruction is not a terminator`
// and a dead process (Windows exit `0xc0000409`). ✔MEASURED on this tree:
// removing the guard from `legalizeTwoAddress` alone killed this suite mid-run.
// So every test below EXERCISES the failure arm; none of them reads it.
//
// ⚠ THE MODULE IS BUILT THROUGH THE REAL `LirBuilder`, NOT FORGED OUT OF
// ARENAS. `LirBuilder::addIndirectBr` accepts an EMPTY target list, and
// `emitTerminator`'s `IndirectBr` arm refuses a zero-successor branch — so the
// refusal is reachable through the builder's own public surface without
// hand-assembling a module the builder would never produce.
//
// ⚠ AND IT IS *NOT* REACHABLE FROM C SOURCE — ✔MEASURED BY EXECUTION, and
// stated here so nobody manufactures a corpus example that exercises nothing.
// THREE independent gates stand above this tier:
//
//   * the front end. `void *g; int f(void){ if (g) goto *g; return 0; }`
//     compiled through the real CLI answers
//     `error[H0009]: computed 'goto *' in a function that takes no label
//     address ('&&label') — there is no valid target`, source-located, exit 1.
//   * `mir_to_lir::lowerIndirectBr` refuses `succs.empty()` with
//     `reportUnsupported` before it ever calls `emitIndirectBr`;
//   * its jump-table site always appends the default block, so that producer
//     cannot emit an empty successor list either.
//
// And no shipped `.target.json` declares a `switch` terminator kind, so the
// dispatch's `Switch` refusal arm is unreachable by configuration too. The
// honest tier for this property is therefore the LIR tier, which is where it
// is pinned — a corpus example would exercise the front-end gate above and
// nothing here.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"
#include "lir/lir_peephole.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_wide_call_args.hpp"

#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace dss;

namespace {

// ⚠ `throw`, NEVER `std::abort()` — same discipline as
// `tests/lir/test_lir_peephole.cpp`, enforced by `check-no-abort-in-tests`,
// and it is this file's own subject applied to its own instrument: an
// `abort()` in a fixture kills the process, so ctest reports an exception and
// the reader learns which TEST died but not which ASSERTION — and every other
// test sharing this executable dies with it. That is "a refusal that crashes
// is not a refusal" on the measuring device. A throw is caught by gtest and
// reported as a named failure of the one test that hit it.
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

// The one input every pass below is fed: a structurally valid module whose
// terminator the shared dispatch REFUSES. `finish()` accepts it (the block
// IS terminated — `jmp_indirect` is a terminator), so this is a module the
// builder genuinely produces; what no rebuild pass can do is re-emit it.
[[nodiscard]] Lir moduleWithZeroSuccessorIndirectBr() {
    LirBuilder b{*x86Schema()};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const addr = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const movOps{LirOperand::makeImmInt32(0)};
    (void)b.addInst(op("mov"), addr, movOps);
    std::array<LirOperand, 1> const brOps{LirOperand::makeReg(addr)};
    (void)b.addIndirectBr(op("jmp_indirect"), brOps,
                          std::span<LirBlockId const>{});
    return std::move(b).finish();
}

// Every diagnostic this reporter holds, concatenated. Streamed into every
// failure below so a red says WHAT was reported instead of only that a count
// was wrong.
[[nodiscard]] std::string allDiagText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        out += d.actual;
        out += '\n';
    }
    return out;
}

// ★★★ THE LOAD-BEARING WITNESS, AND IT IS NOT THE ERROR COUNT.
//
// `EXPECT_GT(rep.errorCount(), 0)` is satisfied by ANY failure — a missing
// opcode, an allocation the pass rejected, a prologue it could not build —
// so a pin resting on it can be green while never reaching the dispatch it
// exists to test. `rewrite` is the concrete trap: `rewriteWithAllocation`
// emits its own "rewriteWithAllocation: function N has no valid allocation"
// error, which contains the pass name and would satisfy a name-only check.
//
// So the assertion is the MESSAGE of the refusal itself, which only
// `lir_pass_util::emitTerminator`'s successor-count arm produces, carrying
// the calling pass's own `passName`. Both halves in one string: the pass
// reached the shared dispatch, AND the dispatch is what turned it back.
[[nodiscard]] bool refusedTheTerminator(DiagnosticReporter const& rep,
                                        std::string_view passName) {
    for (auto const& d : rep.all()) {
        if (d.actual.find("has 0 successors (schema invariant violated)")
                != std::string::npos
            && d.actual.find(passName) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════
// PART 1 — the pass that the anchor names.
// ═════════════════════════════════════════════════════════════════════

TEST(LirTwoAddrLegalizeUnwind, ARefusedTerminatorReportsInsteadOfAborting) {
    Lir const src = moduleWithZeroSuccessorIndirectBr();

    DiagnosticReporter rep;
    // Pre-fix, control does not return from this call — the process dies
    // inside `LirBuilder::finish()`.
    auto const result = legalizeTwoAddress(src, *x86Schema(), rep);

    EXPECT_FALSE(result.ok())
        << "a pass that could not rebuild the module must not report success";
    EXPECT_FALSE(result.allFunctionsLegalized)
        << "the refusal must reach the caller's success channel, not just "
           "the reporter";
    EXPECT_GT(rep.errorCount(), 0u) << "a refusal must be a DIAGNOSTIC";
    EXPECT_TRUE(refusedTheTerminator(rep, "2-address-legalize"))
        << "the diagnostic must carry the shared dispatch's own refusal AND "
           "name the PASS, so the failure is attributable to the legalizer "
           "rather than to the builder it would otherwise have crashed "
           "inside.\nreported:\n" << allDiagText(rep);
}

TEST(LirTwoAddrLegalizeUnwind, TheUnwoundResultCarriesNoHalfBuiltModule) {
    // ★ The unwind must hand back NOTHING consumable. `ok()` is false on
    // both of its clauses — the rebuilt function count never reached
    // `expectedFuncCount`, AND the flag is false — so a caller that checks
    // either one is safe. This is the property that makes the refusal a
    // refusal rather than a half-built module with a warning attached.
    Lir const src = moduleWithZeroSuccessorIndirectBr();
    DiagnosticReporter rep;
    auto const result = legalizeTwoAddress(src, *x86Schema(), rep);

    EXPECT_EQ(result.lir.moduleFuncCount(), 0u);
    EXPECT_EQ(result.expectedFuncCount, src.moduleFuncCount());
    EXPECT_NE(result.lir.moduleFuncCount(), result.expectedFuncCount);
}

TEST(LirTwoAddrLegalizeUnwind, AWellFormedModuleStillLegalizesCleanly) {
    // The other half: the unwind must not have made the pass pessimistic.
    auto lowered = test_support::lowerCToLir(
        "int add(int a, int b) { return a + b; }\n");
    ASSERT_TRUE(lowered.lir.ok);

    DiagnosticReporter rep;
    auto const result = legalizeTwoAddress(lowered.lir.lir, *lowered.target, rep);
    EXPECT_TRUE(result.allFunctionsLegalized);
    EXPECT_EQ(result.lir.moduleFuncCount(), result.expectedFuncCount);
}

// ═════════════════════════════════════════════════════════════════════
// PART 2 — THE OTHER FOUR, which the anchor's own text calls a class and
// which had no pin.
//
// Each drives the SAME refused module through a DIFFERENT pass's real
// entrypoint. The assertion is deliberately the weakest one that is still
// load-bearing — *the call returned at all*, with a diagnostic and a
// not-ok result — because the failure this guards is a process kill, and a
// process kill fails every assertion in the file at once.
// ═════════════════════════════════════════════════════════════════════

TEST(LirRebuildUnwindClass, PeepholeRefusesInsteadOfAborting) {
    Lir const src = moduleWithZeroSuccessorIndirectBr();
    DiagnosticReporter rep;
    auto const result = runLirPeephole(src, *x86Schema(), rep);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.lir.moduleFuncCount(), 0u)
        << "the unwind must not hand back a half-built module";
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(refusedTheTerminator(rep, "lir-peephole"))
        << "reported:\n" << allDiagText(rep);
}

TEST(LirRebuildUnwindClass, WideCallArgLoweringRefusesInsteadOfAborting) {
    Lir const src = moduleWithZeroSuccessorIndirectBr();
    DiagnosticReporter rep;
    auto const result =
        lowerWideCallArgs(src, *x86Schema(), /*ccIndex=*/0, rep);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.lir.moduleFuncCount(), 0u);
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(refusedTheTerminator(rep, "widecall"))
        << "reported:\n" << allDiagText(rep);
}

namespace {

// The two post-regalloc passes need an allocation over the same module.
// ⚠ Produced by the REAL allocator over the REAL liveness, never stubbed:
// a hand-built `LirAllocation` would be a shape those passes never receive,
// and the pin would then be testing the stub.
[[nodiscard]] LirAllocation allocationFor(Lir const& src,
                                          DiagnosticReporter& rep) {
    LirLiveness const lv = analyzeLiveness(src);
    return allocateRegisters(src, *x86Schema(), lv, /*ccIndex=*/0, rep);
}

} // namespace

TEST(LirRebuildUnwindClass, RewriteRefusesInsteadOfAborting) {
    Lir const src = moduleWithZeroSuccessorIndirectBr();
    DiagnosticReporter allocRep;
    LirAllocation const alloc = allocationFor(src, allocRep);

    DiagnosticReporter rep;
    auto const result = rewriteWithAllocation(src, *x86Schema(), alloc, rep);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.lir.moduleFuncCount(), 0u);
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(refusedTheTerminator(rep, "rewrite"))
        << "reported:\n" << allDiagText(rep);
}

TEST(LirRebuildUnwindClass, CallconvRefusesInsteadOfAborting) {
    Lir const src = moduleWithZeroSuccessorIndirectBr();
    DiagnosticReporter allocRep;
    LirAllocation const alloc = allocationFor(src, allocRep);

    DiagnosticReporter rep;
    auto const result =
        materializeCallingConvention(src, *x86Schema(), alloc, rep);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.lir.moduleFuncCount(), 0u);
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(refusedTheTerminator(rep, "callconv"))
        << "reported:\n" << allDiagText(rep);
}

// ═════════════════════════════════════════════════════════════════════
// PART 3 — THE CLASS FIX ITSELF: the builder poison.
//
// ★★★ WHY THE FOUR PINS ABOVE ARE NOT THE FIX. They prove the five callers
// as they stand today. They cannot prove the SIXTH pass, and they cannot see
// the failure mode the anchor actually describes: a caller that READS the
// `[[nodiscard]]` bool, records a failure flag, and keeps driving the
// builder. That compiles clean, satisfies the attribute, and still aborts.
//
// So `lir_pass_util::emitTerminator` now POISONS the builder on every
// refusal, and a poisoned `finish()` yields an EMPTY module instead of a
// process kill. The tests here drive that directly — no pass in between —
// so the guarantee is pinned where it lives rather than through five
// witnesses that could each be right for their own reason.
// ═════════════════════════════════════════════════════════════════════

TEST(LirBuilderPoison, AFinishAfterARefusedTerminatorYieldsAnEmptyModule) {
    // Reproduce, by hand, EXACTLY what a pass that forgot to bail does:
    // call the shared dispatch, ignore the answer, and call `finish()` on a
    // builder whose open block never got its terminator.
    LirBuilder b{*x86Schema()};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const addr = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const movOps{LirOperand::makeImmInt32(0)};
    (void)b.addInst(op("mov"), addr, movOps);

    std::unordered_map<std::uint32_t, LirBlockId> const srcToDst;
    std::array<LirOperand, 1> const brOps{LirOperand::makeReg(addr)};
    DiagnosticReporter rep;
    std::uint16_t const jmpInd = op("jmp_indirect");
    bool const emitted = lir_pass_util::emitTerminator(
        b, jmpInd, x86Schema()->opcodeInfo(jmpInd),
        std::span<LirBlockId const>{}, brOps, /*payload=*/0, /*flags=*/0,
        srcToDst, "poison-pin", rep);

    ASSERT_FALSE(emitted) << "the fixture must actually drive a REFUSAL — a "
                             "dispatch that accepted this would make every "
                             "assertion below vacuous";
    EXPECT_GT(rep.errorCount(), 0u) << "the refusal is reported BEFORE the "
                                       "poison; the poison replaces the abort, "
                                       "never the diagnostic";
    EXPECT_TRUE(b.poisoned())
        << "the dispatch must mark the builder abandoned, so a caller that "
           "ignores the bool cannot drive it into `closeFunction_`'s abort";

    // Pre-poison this line is `dss::Lir fatal: LirBuilder::closeFunction:
    // block's last instruction is not a terminator` and a dead process.
    Lir const out = std::move(b).finish();
    EXPECT_EQ(out.moduleFuncCount(), 0u)
        << "a poisoned builder must yield an EMPTY module — the same value "
           "every rebuild pass's own unwind returns, so a forgotten bail and "
           "a correct bail are indistinguishable to the caller";
}

TEST(LirBuilderPoison, APoisonedBuilderSurvivesBeingDrivenFurther) {
    // ★ The abort has THREE reachable doors, not one: `finish()`,
    // `addFunction` (which closes the previous function), and `beginBlock`
    // ("current block has no terminator"). A pass that merely notes the
    // failure walks straight into the second and third on its next loop
    // iteration, so closing only `finish()` would leave the process kill
    // reachable by the commonest mistake of all — carrying on.
    LirBuilder b{*x86Schema()};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    LirBlockId const next  = b.createBlock();
    b.beginBlock(entry);
    LirReg const addr = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const movOps{LirOperand::makeImmInt32(0)};
    (void)b.addInst(op("mov"), addr, movOps);

    std::unordered_map<std::uint32_t, LirBlockId> const srcToDst;
    std::array<LirOperand, 1> const brOps{LirOperand::makeReg(addr)};
    DiagnosticReporter rep;
    std::uint16_t const jmpInd = op("jmp_indirect");
    ASSERT_FALSE(lir_pass_util::emitTerminator(
        b, jmpInd, x86Schema()->opcodeInfo(jmpInd),
        std::span<LirBlockId const>{}, brOps, 0, 0, srcToDst, "poison-pin",
        rep));

    // Each of these was a process kill before the poison.
    b.beginBlock(next);
    (void)b.addFunction(SymbolId{2});
    Lir const out = std::move(b).finish();
    EXPECT_EQ(out.moduleFuncCount(), 0u);
}

TEST(LirBuilderPoison, ACleanRebuildIsNeverPoisoned) {
    // ⚠ THE OTHER DIRECTION, and it is the one that would make the fix a
    // silent-miscompile machine: if `poisoned()` could be set by an
    // ACCEPTED terminator, `finish()` would quietly return an empty module
    // and every downstream count check would read it as "the pass dropped
    // its functions" — or worse, an `expectedFuncCount == 0` caller would
    // read it as success. Drive the real lowering of real C through a real
    // pass and pin that nothing was poisoned and the module survived.
    auto lowered = test_support::lowerCToLir(
        "int add(int a, int b) { return a + b; }\n");
    ASSERT_TRUE(lowered.lir.ok);

    DiagnosticReporter rep;
    auto const result =
        legalizeTwoAddress(lowered.lir.lir, *lowered.target, rep);
    ASSERT_TRUE(result.allFunctionsLegalized);
    EXPECT_GT(result.lir.moduleFuncCount(), 0u)
        << "an accepted rebuild must still produce its functions";
    EXPECT_EQ(result.lir.moduleFuncCount(), result.expectedFuncCount);

    // And a builder nobody refused anything on reports clean.
    LirBuilder b{*x86Schema()};
    EXPECT_FALSE(b.poisoned());
}
