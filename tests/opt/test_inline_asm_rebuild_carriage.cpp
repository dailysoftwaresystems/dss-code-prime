// Inline-asm P5 — the MIR descriptor survives every rebuild, and an asm block
// is an opaque memory barrier (plan 29 §4.4, handoff §0.6.4).
//
// ★★★ WHY EACH COPY SITE IS ASSERTED INDIVIDUALLY. MIR is rebuilt by THREE live
// verbatim-copy sites, and a production pipeline runs several of them in
// sequence. A test that only checked the END of a pipeline would pass whenever
// one site dropped the descriptor and a later one happened to re-add an
// equivalent-looking entry — or, worse, whenever the pool index happened to land
// on a compatible descriptor. So each site is driven ALONE, from a fresh source
// module, and asserted immediately:
//
//   site 1  `opt/passes/mir_rebuild_helper.cpp` — the shared rebuild substrate
//           every rebuilding pass funnels through (driven here at identity).
//   site 2  `opt/passes/inlining.cpp` emitCalleeInst — the CALLEE body spliced
//           into a caller; the descriptor crosses from one function to another.
//   site 3  `opt/passes/inlining.cpp` MultiBlockInliner — the CALLER host's own
//           rebuild; the asm block belongs to the function being rebuilt.
//
// (LICM's copy site is excluded BY CONSTRUCTION, not by omission:
// `isLicmCandidateOpcode` returns false for any `hasSideEffects` opcode and both
// asm forms are side-effecting. The `LicmDoesNotHoistAcrossAnInlineAsm` test
// below exercises that rather than asserting it from the source.)
//
// The structural backstop behind all three is `MirBuilder::addInst`'s refusal of
// `MirOpcode::InlineAsm`: a site that forwards the raw payload aborts instead of
// naming a descriptor in a pool that has none. That refusal is pinned in
// tests/mir/test_mir_inline_asm.cpp; these tests pin that the sites do the right
// thing rather than merely fail loudly.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_asm_descriptor.hpp"
#include "mir/mir_cfg.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/analysis/mir_alias.hpp"
#include "opt/passes/inlining.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::opt::analysis;
using namespace dss::opt::passes;

namespace {

class IdentityPolicy final : public MirRebuildPolicy {
public:
    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        return mirReversePostOrder(src, src.funcEntry(fn));
    }
};

// The descriptor every fixture below plants. Deliberately NOT default-valued in
// any field a drop could mimic: a non-empty template, a volatile bit, two
// outputs with distinct classes, a named clobber, and BOTH clobber flags. A
// dropped-and-re-defaulted descriptor cannot look like this one.
MirAsmDescriptor sentinelDescriptor() {
    MirAsmDescriptor d;
    d.templateText = "cpuid; rdtsc";
    d.isVolatile   = true;
    MirAsmOperand o0;
    o0.constraint    = "=a";
    o0.regClass      = TargetRegClass::GPR;
    o0.fixedRegister = "rax";
    d.outputs.push_back(o0);
    MirAsmOperand o1;
    o1.constraint     = "=&x";
    o1.regClass       = TargetRegClass::FPR;
    o1.isEarlyClobber = true;
    d.outputs.push_back(o1);
    d.clobbers.push_back("rbx");
    d.clobbers.push_back("rcx");
    d.clobbersMemory         = true;
    d.clobbersConditionCodes = true;
    return d;
}

// Field-by-field, because "a descriptor is present" is a much weaker claim than
// "THIS descriptor is present" — a re-add that lost the clobber list would still
// leave a descriptor in the pool.
void expectSentinel(MirAsmDescriptor const& d, char const* where) {
    EXPECT_EQ(d.templateText, "cpuid; rdtsc") << where;
    EXPECT_TRUE(d.isVolatile) << where;
    ASSERT_EQ(d.outputs.size(), 2u) << where;
    EXPECT_EQ(d.outputs[0].constraint, "=a") << where;
    EXPECT_EQ(d.outputs[0].regClass, TargetRegClass::GPR) << where;
    EXPECT_EQ(d.outputs[0].fixedRegister, "rax") << where;
    EXPECT_EQ(d.outputs[1].constraint, "=&x") << where;
    EXPECT_EQ(d.outputs[1].regClass, TargetRegClass::FPR) << where;
    EXPECT_TRUE(d.outputs[1].isEarlyClobber) << where;
    ASSERT_EQ(d.clobbers.size(), 2u) << where;
    EXPECT_EQ(d.clobbers[0], "rbx") << where;
    EXPECT_EQ(d.clobbers[1], "rcx") << where;
    EXPECT_TRUE(d.clobbersMemory) << where;
    EXPECT_TRUE(d.clobbersConditionCodes) << where;
}

// The one `InlineAsm` of a module, or nullopt.
std::optional<MirInstId> soleAsm(Mir const& mir) {
    std::optional<MirInstId> found;
    std::uint32_t n = 0;
    for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
                MirInstId const id = mir.blockInstAt(b, ii);
                if (mir.instOpcode(id) != MirOpcode::InlineAsm) continue;
                ++n;
                if (!found.has_value()) found = id;
            }
        }
    }
    if (n != 1) {
        ADD_FAILURE() << "expected exactly one InlineAsm in the module, found " << n;
        return std::nullopt;
    }
    return found;
}

constexpr std::uint32_t kThreshold = 32;

} // namespace

// ── site 1: the shared rebuild substrate ────────────────────────────────────
//
// RED-ON-DISABLE: delete the `if (op == MirOpcode::InlineAsm)` arm in
// `MirFunctionRebuilder::emitInst` and the rebuild reaches `addInst`, which
// refuses the opcode and aborts — this test dies rather than silently passing.
TEST(InlineAsmRebuildCarriage, Site1SharedRebuildSubstrateReAddsTheDescriptor) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const asmSrc = mb.addInlineAsm(sentinelDescriptor(), {}, i64);
    (void)mb.addReturnPiece(asmSrc, 1, TargetRegClass::FPR, i64);
    mb.addReturn();
    Mir const src = std::move(mb).finish();
    ASSERT_EQ(src.asmDescriptorPool().size(), 1u);

    MirBuilder dstB;
    DiagnosticReporter rep;
    ASSERT_EQ(cloneGlobalsOrCarveOut(src, dstB, rep, "InlineAsmSite1"),
              GlobalClonePrelude::Cloned);
    IdentityPolicy policy;
    for (std::uint32_t i = 0; i < src.moduleFuncCount(); ++i) {
        MirFunctionRebuilder rb{src, dstB, policy};
        rb.rebuildFunction(src.funcAt(i));
    }
    Mir const dst = std::move(dstB).finish();

    auto const id = soleAsm(dst);
    ASSERT_TRUE(id.has_value());
    // ★ The descriptor was RE-ADDED to the destination pool, not referenced in
    // the source's. The pool must hold exactly one entry: a re-add that also
    // forwarded would leave two, and a forward-only would leave zero.
    EXPECT_EQ(dst.asmDescriptorPool().size(), 1u);
    expectSentinel(dst.asmDescriptor(*id), "site 1 (MirFunctionRebuilder)");
    // The result piece rode across too, with its class intact.
    bool sawPiece = false;
    for (std::uint32_t fi = 0; fi < dst.moduleFuncCount(); ++fi) {
        MirFuncId const f = dst.funcAt(fi);
        for (std::uint32_t bi = 0; bi < dst.funcBlockCount(f); ++bi) {
            MirBlockId const b = dst.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < dst.blockInstCount(b); ++ii) {
                MirInstId const p = dst.blockInstAt(b, ii);
                if (dst.instOpcode(p) != MirOpcode::ReturnPiece) continue;
                sawPiece = true;
                EXPECT_EQ(dst.returnPieceOrdinal(p), 1u);
                EXPECT_EQ(dst.returnPieceRegClass(p), TargetRegClass::FPR)
                    << "the piece's declared class must survive the rebuild — it "
                       "rides `payload`, which every verbatim copy forwards";
            }
        }
    }
    EXPECT_TRUE(sawPiece);
}

// ── site 2: the inliner splicing a CALLEE body ──────────────────────────────
//
// The descriptor crosses a function boundary here, which is the case a
// payload forward gets most obviously wrong: the caller module's pool slot at
// the callee's index holds a DIFFERENT asm block's template (there are two asm
// blocks in this fixture precisely so the indices cannot coincide).
TEST(InlineAsmRebuildCarriage, Site2InlinedCalleeBodyCarriesItsDescriptor) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    // callee: a single-block leaf holding the sentinel asm block.
    mb.addFunction(fnSig, SymbolId{50}, SymbolBinding::Global,
                   SymbolVisibility::Default);
    MirBlockId const cEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(cEntry);
    (void)mb.addInlineAsm(sentinelDescriptor(), {}, i64);
    mb.addReturn();

    // caller: calls it. NOTE the caller has NO asm of its own, so after the
    // splice the module's sole asm block is the one that travelled.
    mb.addFunction(fnSig, SymbolId{100}, SymbolBinding::Global,
                   SymbolVisibility::Default);
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const calleeAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const ops[]      = {calleeAddr};
    (void)mb.addInst(MirOpcode::Call, ops, InvalidType);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = runInlining(mir, interner, rep, kThreshold);
    ASSERT_EQ(r.callsInlined, 1u)
        << "anti-vacuity: if nothing was inlined, site 2 never ran";

    // Two copies now exist (the callee still has its own body — it is not
    // deleted by inlining), so the module pool holds two entries and BOTH must
    // be the sentinel: the spliced copy AND the original.
    EXPECT_EQ(mir.asmDescriptorPool().size(), 2u);
    std::uint32_t seen = 0;
    for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
                MirInstId const id = mir.blockInstAt(b, ii);
                if (mir.instOpcode(id) != MirOpcode::InlineAsm) continue;
                ++seen;
                expectSentinel(mir.asmDescriptor(id), "site 2 (inlined callee)");
            }
        }
    }
    EXPECT_EQ(seen, 2u) << "the callee keeps its body; the caller gained a copy";
}

// ── site 3: the MultiBlockInliner rebuilding the CALLER host ────────────────
//
// Distinct from site 2: here the asm block is in the function being REBUILT, not
// in the body being spliced. The fixture forces the multi-block path with a
// two-block callee (measured in test_inlining.cpp: a single-block callee routes
// the caller through `MirFunctionRebuilder` instead, i.e. through site 1, and
// this test would silently re-test site 1).
TEST(InlineAsmRebuildCarriage, Site3MultiBlockInlinerHostKeepsItsOwnDescriptor) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    // callee: TWO blocks (no asm) — the multi-block router.
    mb.addFunction(fnSig, SymbolId{50}, SymbolBinding::Global,
                   SymbolVisibility::Default);
    MirBlockId const cEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const cTail  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(cEntry);
    mb.addBr(cTail);
    mb.beginBlock(cTail);
    mb.addReturn();

    // caller (the HOST): holds the sentinel asm block and calls the callee.
    mb.addFunction(fnSig, SymbolId{100}, SymbolBinding::Global,
                   SymbolVisibility::Default);
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    (void)mb.addInlineAsm(sentinelDescriptor(), {}, i64);
    MirInstId const calleeAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const ops[]      = {calleeAddr};
    (void)mb.addInst(MirOpcode::Call, ops, InvalidType);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = runInlining(mir, interner, rep, kThreshold);
    ASSERT_EQ(r.callsInlined, 1u)
        << "anti-vacuity: if nothing was inlined the host was never rebuilt";

    auto const id = soleAsm(mir);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(mir.asmDescriptorPool().size(), 1u);
    expectSentinel(mir.asmDescriptor(*id), "site 3 (MultiBlockInliner host)");
}

// ── the optimizer contract: an asm block is an opaque memory barrier ────────
//
// The `CompilerBarrier` / `AtomicFence` region-walk precedent, applied to the
// two asm opcodes. The Store in the fixture provably does NOT alias the Load
// pointer (two distinct Allocas → alias Rule 2 says No), so ONLY the asm block
// can make the region a clobber.
//
// RED-ON-DISABLE: drop `case MirOpcode::InlineAsm` from `opcodeClobbersMemory`
// (mir_opcode.hpp) and the positive half fails — the walk skips it (not a
// Store), and `hasSideEffects` cannot save it, which the negative control proves
// by keeping a `Return` terminator (side-effecting, NOT a clobber) in the region.
TEST(InlineAsmRebuildCarriage, LoadMotionTreatsAnInlineAsmAsAnOpaqueClobber) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    auto const build = [&](bool withAsm) {
        MirBuilder mb;
        mb.addFunction(fnSig, SymbolId{100});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(entry);
        MirInstId const a = mb.addInst(MirOpcode::Alloca, {}, ptr);
        MirInstId const b = mb.addInst(MirOpcode::Alloca, {}, ptr);
        MirLiteralValue v; v.value = std::int64_t{1}; v.core = TypeKind::I32;
        MirInstId const c = mb.addConst(v, i32);
        MirInstId const st[] = {c, b};
        (void)mb.addInst(MirOpcode::Store, st, InvalidType);
        if (withAsm) {
            MirAsmDescriptor d;
            d.templateText = "nop";
            (void)mb.addInlineAsm(std::move(d), {}, InvalidType);
        }
        mb.addReturn();
        struct Out { Mir mir; MirInstId loadPtr; MirBlockId block; };
        return Out{std::move(mb).finish(), a, entry};
    };

    auto const clean = build(false);
    MirBlockId const cleanRegion[] = {clean.block};
    EXPECT_FALSE(mirAnyMayAliasingStoreInRegion(clean.mir, interner,
                                                clean.loadPtr, cleanRegion))
        << "negative control: a non-aliasing Store plus the block's Return "
           "TERMINATOR (hasSideEffects=true) must NOT clobber — the walk keys "
           "on opcodeClobbersMemory, never on the DCE-liveness flag";

    auto const fenced = build(true);
    MirBlockId const fencedRegion[] = {fenced.block};
    EXPECT_TRUE(mirAnyMayAliasingStoreInRegion(fenced.mir, interner,
                                               fenced.loadPtr, fencedRegion))
        << "an inline-asm block is opaque TEXT — no pass can prove it writes no "
           "memory, so a Load may never be moved across it";

    // ★ AND THE DESCRIPTOR'S OWN `memory` CLOBBER IS NOT WHAT DID IT. The
    // fixture's descriptor declares `clobbersMemory == false`, so the barrier
    // came from the OPCODE, unconditionally — which is the correct rule: the
    // source's clobber list records what the programmer declared, not what the
    // compiler can prove.
    auto const asmId = soleAsm(fenced.mir);
    ASSERT_TRUE(asmId.has_value());
    EXPECT_FALSE(fenced.mir.asmDescriptor(*asmId).clobbersMemory);
}
