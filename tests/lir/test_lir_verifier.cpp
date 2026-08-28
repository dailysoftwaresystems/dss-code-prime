// LirVerifier — module SIDE-STRUCTURE integrity rules
// (D-LIR-PER-INST-REG-CONSTRAINTS).
//
// ★★ EVERY TEST HERE EXERCISES THE FAILURE ARM. The rules exist because a
// lost side-structure reference is SILENT — the module stays structurally
// well-formed and the loss surfaces as wrong bytes — so "the rule is
// written down" is worth nothing until the diagnostic has been observed
// coming out of it. Each corrupt module below is hand-built (⚠ NOT
// "HAND-BUILT": `anchor_registry_guard` matches `D-[A-Z0-9-]+` per line
// and would read a phantom anchor `D-BUILT` out of the hyphenated caps —
// it does not scan `tests/` today, but the trap is one policy change
// away, and the same class already minted two phantom anchors from a
// line-wrapped citation in `src/`) through
// `Lir`'s own constructor rather than through `LirBuilder`, because the
// builder's guards make every one of these states unreachable: that is
// the builder doing its job, and it is also why the verifier is the only
// place these can be witnessed.

#include "core/substrate/arena_container.hpp"
#include "core/substrate/mint_monotonic_id.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_literal_pool.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"
#include "lir/lir_verifier.hpp"

#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
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

[[nodiscard]] ImplicitRegisterConstraint sampleConstraint() {
    ImplicitRegisterConstraint c;
    // ⚠ `rax` appears in BOTH `outputs` and `clobbered`, and that is not
    // decoration: `LirBuilder::regConstraintPoolAdd` enforces
    // `outputs ⊆ clobbered` (D-LIR-PER-INSTRUCTION-OUTPUTS-NOT-ENFORCED-SUBSET-OF-CLOBBERED),
    // so a set that declared `rax` an output
    // without clobbering it would abort the process here instead of
    // reaching the verifier rule this fixture exists to exercise.
    c.inputNames        = {"rcx"};
    c.outputNames       = {"rax"};
    c.clobberedNames    = {"rdx", "rax"};
    // Resolved by hand here because this path deliberately BYPASSES
    // `LirBuilder::regConstraintPoolAdd` (which is what normally derives
    // them) — these modules exist precisely to be malformed.
    c.inputOrdinals     = {*x86Schema()->registerByName("rcx")};
    c.outputOrdinals    = {*x86Schema()->registerByName("rax")};
    c.clobberedOrdinals = {*x86Schema()->registerByName("rdx"),
                           *x86Schema()->registerByName("rax")};
    return c;
}

[[nodiscard]] LirLiteralValue i64Literal(std::int64_t v) {
    LirLiteralValue lv;
    lv.value = v;
    lv.core  = TypeKind::I64;
    return lv;
}

// A single-function, single-block module assembled straight from arenas.
// `insts` are appended in order into one block; slot 0 of every arena is
// the reserved sentinel `ArenaBuilder` creates, so the first real
// instruction / block / function all land at index 1 — which is exactly
// what `Lir::funcAt(0)` → `LirFuncId{1}` and `blockInstAt(bb, 0)` →
// `instStart + 0` expect.
[[nodiscard]] Lir handBuildModule(std::vector<detail::LirInst> insts,
                                  std::vector<LirOperand> operandPool,
                                  LirLiteralPool literals,
                                  LirRegConstraintPool constraints) {
    auto const mid = substrate::mintMonotonicId<LirModuleId>();
    substrate::ArenaBuilder<detail::LirInst, LirInstId, LirModuleId> ib{mid};
    for (auto const& i : insts) (void)ib.addNode(i);

    detail::LirBlock blk;
    blk.instStart = 1;  // slot 0 is the arena sentinel
    blk.instCount = static_cast<std::uint32_t>(insts.size());
    blk.succStart = 0;
    blk.succCount = 0;
    blk.func      = 1;
    substrate::ArenaBuilder<detail::LirBlock, LirBlockId, LirModuleId> bb{mid};
    (void)bb.addNode(blk);

    detail::LirFunc fn;
    fn.blockStart = 1;
    fn.blockCount = 1;
    fn.symbol     = 1;
    fn.numVRegs   = 1;
    substrate::ArenaBuilder<detail::LirFunc, LirFuncId, LirModuleId> fb{mid};
    (void)fb.addNode(fn);

    return Lir{x86Schema()->id(),
               std::move(ib).finish(),
               std::move(bb).finish(),
               std::move(fb).finish(),
               std::move(operandPool),
               std::vector<LirBlockId>{},
               std::move(literals),
               std::move(constraints)};
}

[[nodiscard]] detail::LirInst movInst(std::uint32_t operandStart,
                                      std::uint32_t operandCount,
                                      std::uint32_t regConstraints) {
    detail::LirInst i;
    i.opcode         = op("mov");
    i.result         = makeVirtualReg(1, LirRegClass::GPR);
    i.operandStart   = operandStart;
    i.operandCount   = operandCount;
    i.regConstraints = regConstraints;
    return i;
}

[[nodiscard]] std::size_t countDiags(DiagnosticReporter const& rep,
                                     DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.code == code) ++n;
    }
    return n;
}

// A well-formed reference module built the normal way, used as the
// `before` side of the paired rebuild checks.
[[nodiscard]] Lir buildCleanModule() {
    LirBuilder b{*x86Schema()};
    std::uint32_t const rc = b.regConstraintPoolAdd(sampleConstraint());
    std::uint32_t const lit = b.literalPoolAdd(i64Literal(7));
    (void)b.addFunction(SymbolId{1});
    LirBlockId const e = b.createBlock();
    b.beginBlock(e);
    LirReg const r = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const ops{LirOperand::makeLiteralIndex(lit)};
    LirInstId const mov = b.addInst(op("mov"), r, ops);
    b.setInstRegConstraints(mov, rc);
    std::array<LirOperand, 1> const retOps{LirOperand::makeReg(r)};
    b.addReturn(op("ret"), retOps);
    return std::move(b).finish();
}

} // namespace

// ── dangling references ──────────────────────────────────────────────

TEST(LirVerifierSideStructures, DanglingConstraintHandleFiresIndexDangling) {
    // Handle 1 → pool index 0, but the pool is EMPTY. This is what a
    // rebuild that carried the instructions and forgot the pool leaves
    // behind; reaching the allocator it aborts inside
    // `LirRegConstraintPool::at` with no instruction named.
    Lir const lir = handBuildModule(
        {movInst(/*operandStart=*/0, /*operandCount=*/0,
                 /*regConstraints=*/lirRegConstraintHandleForIndex(0))},
        {}, LirLiteralPool{}, LirRegConstraintPool{});

    DiagnosticReporter rep;
    EXPECT_FALSE(verifyLirText(lir, *x86Schema(), rep));
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_SideStructureIndexDangling), 1u);
}

TEST(LirVerifierSideStructures, DanglingLiteralIndexFiresIndexDangling) {
    // The LITERAL pool's identical exposure — the precedent this carrier
    // was modelled on had no such rule, and inheriting the precedent's
    // bug is not acceptable.
    Lir const lir = handBuildModule(
        {movInst(/*operandStart=*/0, /*operandCount=*/1,
                 /*regConstraints=*/kLirNoRegConstraints)},
        {LirOperand::makeLiteralIndex(5)},
        LirLiteralPool{}, LirRegConstraintPool{});

    DiagnosticReporter rep;
    EXPECT_FALSE(verifyLirText(lir, *x86Schema(), rep));
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_SideStructureIndexDangling), 1u);
}

TEST(LirVerifierSideStructures, AWellFormedHandleAndIndexVerifyClean) {
    // The other half of every failure-arm test: prove the rule is not
    // simply always-on. A green here plus a red above is what makes the
    // pair meaningful.
    LirLiteralPool lits;
    (void)lits.add(i64Literal(1));
    LirRegConstraintPool rcs;
    (void)rcs.add(sampleConstraint());
    Lir const lir = handBuildModule(
        {movInst(0, 1, lirRegConstraintHandleForIndex(0))},
        {LirOperand::makeLiteralIndex(0)},
        std::move(lits), std::move(rcs));

    DiagnosticReporter rep;
    EXPECT_TRUE(verifyLirText(lir, *x86Schema(), rep));
}

// ── the unreferenced-entry rule (the per-instruction drop) ───────────

TEST(LirVerifierSideStructures, UnreferencedConstraintEntryFiresReferenceLost) {
    // ★ THE SHAPE OF THE REAL SILENT MISCOMPILE. The pool was carried, the
    // instruction's handle was not, so the handle is the perfectly legal
    // `kLirNoRegConstraints`: nothing dangles and no pool shrank. The
    // ONLY trace is the entry nothing points at.
    LirRegConstraintPool rcs;
    (void)rcs.add(sampleConstraint());
    Lir const lir = handBuildModule(
        {movInst(0, 0, kLirNoRegConstraints)},
        {}, LirLiteralPool{}, std::move(rcs));

    DiagnosticReporter rep;
    EXPECT_FALSE(verifyLirText(lir, *x86Schema(), rep));
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_SideStructureReferenceLost), 1u);
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_SideStructureIndexDangling), 0u);
}

TEST(LirVerifierSideStructures, AnUnreferencedLiteralEntryIsNotAnError) {
    // ⚠ The rule is deliberately ASYMMETRIC and this pins the asymmetry so
    // a future "tidy-up" cannot make it symmetric by accident. Literal
    // references ride OPERANDS, which every rebuilding pass copies
    // verbatim, so the literal pool has no analogous per-instruction drop
    // — and MIR→LIR legitimately interns a literal on a path that then
    // declines to emit its instruction. Asserting orphan-freedom there
    // would be a false red, not a net.
    LirLiteralPool lits;
    (void)lits.add(i64Literal(1));
    Lir const lir = handBuildModule(
        {movInst(0, 0, kLirNoRegConstraints)},
        {}, std::move(lits), LirRegConstraintPool{});

    DiagnosticReporter rep;
    EXPECT_TRUE(verifyLirText(lir, *x86Schema(), rep));
}

// ── the paired rebuild check ─────────────────────────────────────────

TEST(LirVerifierSideStructures, PoolShrankFiresOnBothPools) {
    Lir const before = buildCleanModule();
    // The `after`: same instruction shape, both pools EMPTY — a rebuild
    // that never called `copyModuleSideStructures`. Its handle dangles
    // too, which is the point of reporting the shrink FIRST: the shrink
    // explains the dangle.
    Lir const after = handBuildModule(
        {movInst(0, 1, lirRegConstraintHandleForIndex(0))},
        {LirOperand::makeLiteralIndex(0)},
        LirLiteralPool{}, LirRegConstraintPool{});

    DiagnosticReporter rep;
    EXPECT_FALSE(verifyLirRebuild(before, after, "test-pass", rep));
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_SideStructurePoolShrank), 2u)
        << "one per pool — literal and register-constraint";
}

TEST(LirVerifierSideStructures, LostLiteralReferenceFiresReferenceLost) {
    // ★ The only instrument that catches a dropped LITERAL reference: the
    // pool is intact and every surviving index resolves, so neither the
    // shrink rule nor the dangling rule sees it. Only the reference
    // census does.
    Lir const before = buildCleanModule();
    LirLiteralPool lits;
    (void)lits.add(i64Literal(7));
    LirRegConstraintPool rcs;
    (void)rcs.add(sampleConstraint());
    Lir const after = handBuildModule(
        {movInst(/*operandStart=*/0, /*operandCount=*/0,
                 lirRegConstraintHandleForIndex(0))},
        {}, std::move(lits), std::move(rcs));

    DiagnosticReporter rep;
    EXPECT_FALSE(verifyLirRebuild(before, after, "test-pass", rep));
    EXPECT_GT(countDiags(rep, DiagnosticCode::L_SideStructureReferenceLost), 0u);
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_SideStructurePoolShrank), 0u);
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_SideStructureIndexDangling), 0u);
}

TEST(LirVerifierSideStructures, AFaithfulRebuildVerifiesClean) {
    Lir const before = buildCleanModule();
    LirLiteralPool lits;
    (void)lits.add(i64Literal(7));
    LirRegConstraintPool rcs;
    (void)rcs.add(sampleConstraint());
    Lir const after = handBuildModule(
        {movInst(0, 1, lirRegConstraintHandleForIndex(0))},
        {LirOperand::makeLiteralIndex(0)},
        std::move(lits), std::move(rcs));

    DiagnosticReporter rep;
    EXPECT_TRUE(verifyLirRebuild(before, after, "test-pass", rep));
}

// ★★ THE HOLE THE MODULE-LOCAL RULE CANNOT SEE, AND WHY BOTH CHECKS
// EXIST. When TWO instructions share one pool entry (the pool does not
// dedup, but nothing stops a producer reusing an index) and a rebuild
// drops ONE of them, the entry is still referenced — so the
// unreferenced-entry rule is vacuously satisfied. Only the paired
// reference CENSUS sees the drop. A design that shipped just one of these
// two rules would have a silent case either way.
TEST(LirVerifierSideStructures, DroppingOneOfTwoSharedReferencesIsCaught) {
    LirLiteralPool litsBefore;
    (void)litsBefore.add(i64Literal(7));
    LirRegConstraintPool rcsBefore;
    (void)rcsBefore.add(sampleConstraint());
    Lir const before = handBuildModule(
        {movInst(0, 0, lirRegConstraintHandleForIndex(0)),
         movInst(0, 0, lirRegConstraintHandleForIndex(0))},
        {}, std::move(litsBefore), std::move(rcsBefore));

    LirLiteralPool litsAfter;
    (void)litsAfter.add(i64Literal(7));
    LirRegConstraintPool rcsAfter;
    (void)rcsAfter.add(sampleConstraint());
    Lir const after = handBuildModule(
        {movInst(0, 0, lirRegConstraintHandleForIndex(0)),
         movInst(0, 0, kLirNoRegConstraints)},
        {}, std::move(litsAfter), std::move(rcsAfter));

    // The module-local rule is BLIND here — assert that, so a future
    // "simplification" that deletes the census cannot claim coverage.
    DiagnosticReporter localRep;
    EXPECT_TRUE(verifyLirText(after, *x86Schema(), localRep))
        << "the surviving reference keeps the entry reachable, so the "
           "unreferenced-entry rule cannot see this drop";

    DiagnosticReporter rep;
    EXPECT_FALSE(verifyLirRebuild(before, after, "test-pass", rep));
    EXPECT_GT(countDiags(rep, DiagnosticCode::L_SideStructureReferenceLost), 0u);
}

// A pass that GROWS the module (2-address legalize inserts movs, callconv
// inserts prologue/arg setup) must not trip the reference census — `>=`,
// not `==`, is load-bearing.
TEST(LirVerifierSideStructures, ARebuildThatAddsReferencesVerifiesClean) {
    Lir const before = buildCleanModule();
    LirLiteralPool lits;
    (void)lits.add(i64Literal(7));
    LirRegConstraintPool rcs;
    (void)rcs.add(sampleConstraint());
    Lir const after = handBuildModule(
        {movInst(0, 1, lirRegConstraintHandleForIndex(0)),
         movInst(1, 1, lirRegConstraintHandleForIndex(0))},
        {LirOperand::makeLiteralIndex(0), LirOperand::makeLiteralIndex(0)},
        std::move(lits), std::move(rcs));

    DiagnosticReporter rep;
    EXPECT_TRUE(verifyLirRebuild(before, after, "test-pass", rep));
}

// ═════════════════════════════════════════════════════════════════════
// Rule 1 — the addressing-mode rule
// (D-LIR-VERIFY-MEM-OPERAND-PAIRING-RULE-IS-FALSE).
//
// ★★★ THE ACCEPT ARM RUNS THE **REAL** LOWERING, AND THAT IS THE WHOLE
// POINT OF THIS SECTION. The rule was false for its entire lifetime —
// it demanded a MemBase+MemOffset tail on every load/store/lea while the
// shipped lowering also emits a symbol-addressed form — and it survived
// because its only subjects were modules the rule's own author had
// hand-assembled out of arenas. A hand-built module can only ever
// re-state its author's belief about what the compiler emits. So the
// accept arm below lowers c SOURCE through `lowerToLir` and
// asserts the verifier accepts what the compiler actually produced.
// ⚠ If someone narrows the rule back, THIS is the test that reds.
//
// The reject arms stay hand-built, and necessarily so: the shipped
// lowering never emits a malformed address, so a corrupt module is the
// only way to witness the rule still refusing one.
// ═════════════════════════════════════════════════════════════════════

namespace {

// A `lea`/`load` carrying an arbitrary operand list, for the reject arms.
[[nodiscard]] detail::LirInst memInst(std::string_view mnemonic,
                                      std::uint32_t operandStart,
                                      std::uint32_t operandCount) {
    detail::LirInst i;
    i.opcode         = op(mnemonic);
    i.result         = makeVirtualReg(1, LirRegClass::GPR);
    i.operandStart   = operandStart;
    i.operandCount   = operandCount;
    i.regConstraints = kLirNoRegConstraints;
    return i;
}

[[nodiscard]] bool verifyMemShape(std::string_view mnemonic,
                                  std::vector<LirOperand> ops,
                                  DiagnosticReporter& rep) {
    auto const n = static_cast<std::uint32_t>(ops.size());
    Lir const lir = handBuildModule({memInst(mnemonic, 0, n)}, std::move(ops),
                                    LirLiteralPool{}, LirRegConstraintPool{});
    return verifyLirText(lir, *x86Schema(), rep);
}

} // namespace

TEST(LirVerifierMemAddressing, RealLoweringOfAGlobalReadIsACCEPTED) {
    // ★ The regression the rule's whole lifetime was hiding. `&global` /
    // reading a global lowers to `lea r, [@sym]` — a symbol-addressed
    // operand list with NO MemBase/MemOffset pair. Under the pre-fix rule
    // this module was rejected and `verifyLir` could not be wired at all.
    auto lowered = test_support::lowerCToLir(
        "int g = 7;\n"
        "int addr_of_g_is_read(void) { return g; }\n");
    ASSERT_TRUE(lowered.lir.ok) << "fixture failed to lower the source";

    DiagnosticReporter rep;
    auto const r = verifyLir(lowered.lir.lir, lowered.mir.mir,
                             lowered.model.lattice().interner(),
                             *lowered.target, lowered.lir.lirToMir, rep);
    EXPECT_TRUE(r.ok)
        << "the verifier rejected a module the SHIPPED lowering produced; "
           "the first diagnostic is: "
        << (rep.all().empty() ? std::string{"<none>"} : rep.all().front().actual);
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_MemOperandMalformed), 0u);
}

TEST(LirVerifierMemAddressing, RealLoweringOfComputedGotoIsACCEPTED) {
    // ★★ THE SHAPE THAT DEFEATS THE OBVIOUS FIX. "An operand list ENDING in
    // a lone SymbolRef is the symbol form" is the natural way to write this
    // rule and it is WRONG: `&&label` lowers to `lea r, [@sym ^block]` —
    // `[SymbolRef, BlockRef]` — where the BlockRef binds the synthetic
    // symbol to the block's byte offset, so the SymbolRef is NOT last.
    // Membership, not position, is the property that holds. This test is
    // what stops a future "simplification" from re-introducing the bug in a
    // narrower form.
    auto lowered = test_support::lowerCToLir(
        "int dispatch(void) {\n"
        "  void *t = &&done;\n"
        "  goto *t;\n"
        "done:\n"
        "  return 3;\n"
        "}\n");
    ASSERT_TRUE(lowered.lir.ok) << "fixture failed to lower computed goto";

    DiagnosticReporter rep;
    auto const r = verifyLir(lowered.lir.lir, lowered.mir.mir,
                             lowered.model.lattice().interner(),
                             *lowered.target, lowered.lir.lirToMir, rep);
    EXPECT_TRUE(r.ok)
        << "first diagnostic: "
        << (rep.all().empty() ? std::string{"<none>"} : rep.all().front().actual);
    EXPECT_EQ(countDiags(rep, DiagnosticCode::L_MemOperandMalformed), 0u);
}

TEST(LirVerifierMemAddressing, BothAddressingModesAreACCEPTED) {
    // The two forms, spelled directly, so the accepted set is pinned even
    // if the lowering later stops emitting one of them.
    for (auto const& [what, ops] :
         std::vector<std::pair<char const*, std::vector<LirOperand>>>{
             {"base+displacement",
              {LirOperand::makeReg(makeVirtualReg(2, LirRegClass::GPR)),
               LirOperand::makeMemBase(1), LirOperand::makeMemOffset(0)}},
             {"indexed base+displacement",
              {LirOperand::makeReg(makeVirtualReg(2, LirRegClass::GPR)),
               LirOperand::makeReg(makeVirtualReg(3, LirRegClass::GPR)),
               LirOperand::makeMemBase(8), LirOperand::makeMemOffset(-16)}},
             {"bare symbol", {LirOperand::makeSymbolRef(1)}},
             {"register + symbol (TLS tp+tpoff)",
              {LirOperand::makeReg(makeVirtualReg(2, LirRegClass::GPR)),
               LirOperand::makeSymbolRef(1)}},
             {"symbol + blockref (&&label)",
              {LirOperand::makeSymbolRef(1), LirOperand::makeBlockRef(1)}},
         }) {
        DiagnosticReporter rep;
        EXPECT_TRUE(verifyMemShape("lea", ops, rep)) << what;
        EXPECT_EQ(countDiags(rep, DiagnosticCode::L_MemOperandMalformed), 0u)
            << what;
    }
}

TEST(LirVerifierMemAddressing, MalformedAddressesAreSTILLREJECTED) {
    // ⚠ THE ANTI-DEGENERATION PIN. Teaching the rule a second form is one
    // edit away from teaching it to accept anything, so every shape that
    // is NOT one of the two must still be refused — including the two
    // MIXED shapes, which are what a half-rewritten address looks like and
    // which a "ends with SymbolRef OR ends with the pair" rule would wave
    // through in both directions.
    auto const gpr = [](std::uint32_t i) {
        return LirOperand::makeReg(makeVirtualReg(i, LirRegClass::GPR));
    };
    for (auto const& [what, ops] :
         std::vector<std::pair<char const*, std::vector<LirOperand>>>{
             {"no address at all", {gpr(2)}},
             {"empty operand list", {}},
             {"immediates where the pair belongs",
              {gpr(2), LirOperand::makeImmInt32(0), LirOperand::makeImmInt32(0)}},
             {"MemBase with no MemOffset", {gpr(2), LirOperand::makeMemBase(1)}},
             {"the pair REVERSED",
              {gpr(2), LirOperand::makeMemOffset(0), LirOperand::makeMemBase(1)}},
             {"pair present but not TERMINAL",
              {gpr(2), LirOperand::makeMemBase(1), LirOperand::makeMemOffset(0),
               gpr(3)}},
             {"MIXED: symbol appended to a base+displacement address",
              {gpr(2), LirOperand::makeMemBase(1), LirOperand::makeMemOffset(0),
               LirOperand::makeSymbolRef(1)}},
             {"MIXED: base+displacement appended to a symbol address",
              {LirOperand::makeSymbolRef(1), LirOperand::makeMemBase(1),
               LirOperand::makeMemOffset(0)}},
         }) {
        DiagnosticReporter rep;
        EXPECT_FALSE(verifyMemShape("lea", ops, rep)) << what;
        EXPECT_EQ(countDiags(rep, DiagnosticCode::L_MemOperandMalformed), 1u)
            << what;
    }
}
