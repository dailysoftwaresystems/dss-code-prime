// Inline-asm P5 — the MIR descriptor survives every rebuild, and an asm block
// is an opaque memory barrier (plan 29 §4.4, handoff §0.6.4).
//
// ★★★ WHY EACH COPY SITE IS ASSERTED INDIVIDUALLY. MIR is copied by FOUR live
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
//   site 4  `mir/merge/mir_merge.cpp` FunctionCloner — the CROSS-CU merge; the
//           descriptor crosses from one MODULE to another.
//
// ★★★ SITE 4 WAS MISSING FROM THIS LIST AND FROM THE CLONER, AND THAT COST FOUR
// SQLITE LEGS. This census said "THREE" from the day the opcode landed until
// 2026-08-17, and it was wrong the whole time: it enumerated the OPTIMIZER's
// copy sites, and the cross-CU merge is not an optimizer pass. Nothing here
// noticed, because the merge runs only for N>=2 CUs (`mir_merge.hpp`: the
// single-CU path is kept byte-identical for N==1) and EVERY inline-asm example
// in the corpus was one source file. ✔MEASURED: a 2-TU build carrying any
// `__asm__` aborted the compiler at BOTH configs. `examples/c-subset/
// c_inline_asm_crosscu_merge` is the corpus half of the fix — the first
// example that is both multi-source and asm-carrying.
//
// ⚠ SO THE RULE, STATED WHERE THE NEXT PERSON WILL BE: a new MIR copy site gets
// an arm in its cloner AND a case in this list AND a test below. The count in
// `mir/mir.cpp`'s `addInst` refusal comment is the same census and moves with it.
//
// (LICM's copy site is excluded BY CONSTRUCTION, not by omission:
// `isLicmCandidateOpcode` returns false for any `hasSideEffects` opcode and both
// asm forms are side-effecting. The `LicmDoesNotHoistAcrossAnInlineAsm` test
// below exercises that rather than asserting it from the source.)
//
// The structural backstop behind all four is `MirBuilder::addInst`'s refusal of
// `MirOpcode::InlineAsm`: a site that forwards the raw payload aborts instead of
// naming a descriptor in a pool that has none. That refusal is pinned in
// tests/mir/test_mir_inline_asm.cpp; these tests pin that the sites do the right
// thing rather than merely fail loudly.
//
// ⚠ AND THE BACKSTOP ONLY COVERS THE VALUE FORM. `InlineAsmGoto` is a
// TERMINATOR, so a cloner missing ITS arm never reaches `addInst` — it hits its
// own switch default. That is a fatal too, but a different one, raised by a
// different file, and only if the cloner's default arm is fail-loud. The
// `AnchorSite*` tests at the bottom are what hold the terminator half.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"                  // site 4 (cross-CU merge)
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_lattice.hpp"      // site 4 (cross-CU merge)
#include "mir/merge/mir_merge.hpp"                       // site 4 (cross-CU merge)
#include "mir/mir.hpp"
#include "mir/mir_asm_descriptor.hpp"
#include "mir/mir_cfg.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/analysis/mir_alias.hpp"
#include "opt/passes/dce.hpp"
#include "opt/passes/inlining.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"
#include "opt/passes/simplify_cfg.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace dss;
using namespace dss::opt::analysis;
using namespace dss::opt::passes;

namespace {

class IdentityPolicy final : public MirRebuildPolicy {
public:
    // Mandatory (pure virtual) — a policy that does not name itself does not
    // compile (D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS). A TEST policy
    // names itself after the fixture rather than a pipeline pass, which is the
    // point: if this rebuild ever aborts, the reader is told the abort came from
    // a unit test and not from `Cse`.
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "InlineAsmSite1";
    }

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        return mirReversePostOrder(src, src.funcEntry(fn));
    }
};

// The descriptor every fixture below plants. Deliberately NOT default-valued in
// any field a drop could mimic: a non-empty template, a volatile bit, two
// outputs with distinct classes, a named clobber, and BOTH clobber flags. A
// dropped-and-re-defaulted descriptor cannot look like this one.
//
// ★★ EVERY FIELD THE DESCRIPTOR GAINS MUST BE ADDED HERE, AND THE TWO NEWEST
// ONES SHOW WHY. `isExtended` and `MirAsmOperand::tiedOutput` arrived after this
// fixture was written and were NOT pinned. Today nothing can drop them — every
// rebuild site passes `src.asmDescriptor(id)` WHOLE by value, so new members ride
// along for free — and that is exactly the trap: the pin's strength is a property
// of how the copy happens to be spelled today, not of anything asserted. The
// exposure is a future refactor to a field-by-field copy, which is the silent-drop
// class `mir_asm_descriptor.hpp`'s own docblock exists to guard
// ("A POOL INDEX IS NOT SELF-CARRYING, AND THAT IS THE WHOLE HAZARD").
// ⚠ Both are set to NON-DEFAULT values on purpose: `isExtended` defaults to the
// BASIC surface and `tiedOutput` to `nullopt`, so a dropped field would be
// indistinguishable from a field that was never set if the sentinel used the
// defaults.
MirAsmDescriptor sentinelDescriptor() {
    MirAsmDescriptor d;
    d.templateText = "cpuid; rdtsc";
    d.isVolatile   = true;
    d.isExtended   = true;
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
    // ★ THE TIED INPUT. `tiedOutput` is set on the synthesized INPUT entry that
    // carries a `"+r"` operand's READ half, naming the output it shares a
    // location with — so it belongs on an input and nowhere else. Index 1 rather
    // than 0 deliberately: a drop that re-defaulted the field to `0` instead of
    // `nullopt` would be invisible against output 0.
    MirAsmOperand i0;
    i0.constraint = "r";
    i0.regClass   = TargetRegClass::FPR;
    i0.tiedOutput = 1u;
    d.inputs.push_back(i0);
    d.clobbers.push_back("rbx");
    d.clobbers.push_back("rcx");
    d.clobbersMemory         = true;
    d.clobbersConditionCodes = true;
    return d;
}

// The sentinel declares ONE input, and `MirBuilder::checkAsmOperandAlignment_`
// ABORTS the process on a descriptor/operand-count mismatch. Planting the asm
// through one helper keeps that coupling in a single place: a later change to the
// sentinel's input list cannot leave three call sites silently out of step, and
// the failure mode if it did is an abort with no test name attached.
MirInstId plantSentinelAsm(MirBuilder& mb, TypeId ty) {
    MirLiteralValue v;
    v.value = std::int64_t{7};
    v.core  = TypeKind::I64;
    MirInstId const ops[] = {mb.addConst(v, ty)};
    return mb.addInlineAsm(sentinelDescriptor(), ops, ty);
}

// Field-by-field, because "a descriptor is present" is a much weaker claim than
// "THIS descriptor is present" — a re-add that lost the clobber list would still
// leave a descriptor in the pool.
void expectSentinel(MirAsmDescriptor const& d, char const* where) {
    EXPECT_EQ(d.templateText, "cpuid; rdtsc") << where;
    EXPECT_TRUE(d.isVolatile) << where;
    // The BASIC/EXTENDED surface: carried, never reconstructed downstream —
    // `:::` with every section empty is unreconstructable from the sections
    // alone, and a lowering that guessed it rejected valid code.
    EXPECT_TRUE(d.isExtended) << where;
    ASSERT_EQ(d.outputs.size(), 2u) << where;
    EXPECT_EQ(d.outputs[0].constraint, "=a") << where;
    EXPECT_EQ(d.outputs[0].regClass, TargetRegClass::GPR) << where;
    EXPECT_EQ(d.outputs[0].fixedRegister, "rax") << where;
    EXPECT_EQ(d.outputs[1].constraint, "=&x") << where;
    EXPECT_EQ(d.outputs[1].regClass, TargetRegClass::FPR) << where;
    EXPECT_TRUE(d.outputs[1].isEarlyClobber) << where;
    ASSERT_EQ(d.inputs.size(), 1u) << where;
    EXPECT_EQ(d.inputs[0].constraint, "r") << where;
    EXPECT_EQ(d.inputs[0].regClass, TargetRegClass::FPR) << where;
    // ASSERT on the engagement first: a dropped `tiedOutput` is `nullopt`, and
    // reading `.value()` off it would abort the process instead of failing the
    // test with a message naming the site.
    ASSERT_TRUE(d.inputs[0].tiedOutput.has_value()) << where;
    EXPECT_EQ(*d.inputs[0].tiedOutput, 1u) << where;
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
    MirInstId const asmSrc = plantSentinelAsm(mb, i64);
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
    (void)plantSentinelAsm(mb, i64);
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
    (void)plantSentinelAsm(mb, i64);
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

// ── site 4: the CROSS-CU merge cloning between MODULES ──────────────────────
//
// The site the census forgot. Sites 1-3 are all OPTIMIZER rebuilds, and they
// share a property that hid this one: they rebuild a module into another module
// of THE SAME COMPILATION. `mergeCuMirs` folds N per-CU modules into one, so the
// descriptor pool it clones INTO belongs to a module that never saw the source
// CU at all — the pool-index forward is at its most wrong here, and it was the
// arm that did not exist.
//
// ⚠ THE FIXTURE MUST STAY 2-CU. `mir_merge.hpp` states the driver rule: the
// single-CU path is kept byte-identical for N==1 and `mergeCuMirs` runs only for
// N>=2. A one-CU version of this test would exercise nothing and still pass.
namespace {

// SymbolId → name, for `MergeCuInput::nameOf`. The merge keys cross-CU matching
// on this exactly as a linker keys on the on-binary symbol name.
std::function<std::string(SymbolId)>
namerOf(std::unordered_map<std::uint32_t, std::string> m) {
    return [m = std::move(m)](SymbolId s) {
        auto const it = m.find(s.v);
        return it == m.end() ? std::string{} : it->second;
    };
}

// `f`'s body, built into `mb`: one block returning the value of an inline asm.
// `descriptorTemplate` names the template text so two CUs can carry DISTINCT
// descriptors and the second test can tell them apart.
void addAsmCarryingFunc(MirBuilder& mb, TypeId i64, TypeId sig, SymbolId sym,
                        MirAsmDescriptor descriptor) {
    mb.addFunction(sig, sym, SymbolBinding::Global, SymbolVisibility::Default);
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirLiteralValue v;
    v.value = std::int64_t{7};
    v.core  = TypeKind::I64;
    MirInstId const ops[] = {mb.addConst(v, i64)};
    mb.addReturn(mb.addInlineAsm(std::move(descriptor), ops, i64));
}

} // namespace

TEST(InlineAsmRebuildCarriage, Site4CrossCuMergeReAddsTheDescriptor) {
    // CU0: `main` calls extern `f` — the cross-CU reference that makes the merge
    // do real work on the module carrying the asm.
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i64_0 = in0.primitive(TypeKind::I64);
    TypeId const sig0  = in0.fnSig({}, i64_0, CallConv::CcSysV);
    Mir mir0;
    {
        MirBuilder mb;
        mb.addFunction(sig0, SymbolId{100}, SymbolBinding::Global,
                       SymbolVisibility::Default);
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        MirInstId const fAddr = mb.addGlobalAddr(SymbolId{10}, sig0);
        MirInstId const ops[] = {fAddr};
        mb.addReturn(mb.addInst(MirOpcode::Call, ops, i64_0));
        mir0 = std::move(mb).finish();
    }
    std::vector<ExternImport> const ext0 = {ExternImport{SymbolId{10}, "f", "libc.so"}};

    // CU1: `f` carries the sentinel asm.
    TypeInterner in1{CompilationUnitId{2}};
    TypeId const i64_1 = in1.primitive(TypeKind::I64);
    TypeId const sig1  = in1.fnSig({}, i64_1, CallConv::CcSysV);
    Mir mir1;
    {
        MirBuilder mb;
        addAsmCarryingFunc(mb, i64_1, sig1, SymbolId{50}, sentinelDescriptor());
        mir1 = std::move(mb).finish();
    }
    // Anti-vacuity: the SOURCE really does carry one descriptor. If the fixture
    // ever stopped planting an asm, every assertion below would pass on a module
    // that has nothing to carry.
    ASSERT_EQ(mir1.asmDescriptorPool().size(), 1u);

    std::vector<MergeCuInput> const cus = {
        MergeCuInput{&mir0, &in0, namerOf({{100, "main"}, {10, "f"}}), ext0},
        MergeCuInput{&mir1, &in1, namerOf({{50, "f"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

    auto const id = soleAsm(merged->mir);
    ASSERT_TRUE(id.has_value());
    // Exactly one: a re-add that also forwarded the old payload, or an arm that
    // ran twice, would leave a second entry behind.
    EXPECT_EQ(merged->mir.asmDescriptorPool().size(), 1u);
    expectSentinel(merged->mir.asmDescriptor(*id), "site 4 (cross-CU merge)");
}

// ── site 4, sharpened: TWO CUs, TWO descriptors, no crossed wires ────────────
//
// The test above proves the descriptor SURVIVES. This one proves each asm names
// ITS OWN. With one descriptor in play, index 0 in the source pool is also index
// 0 in the merged pool, so a raw payload forward would land on the right entry BY
// COINCIDENCE and look correct. Two CUs each carrying an asm removes the
// coincidence: CU1's descriptor cannot be at index 0 of the merged pool, so a
// forwarded index names CU0's `nop` — the silent wrong-descriptor outcome that
// `addInst`'s refusal exists to prevent, here asserted positively rather than
// left to the abort.
TEST(InlineAsmRebuildCarriage, Site4CrossCuMergeKeepsEachCusDescriptorWithItsOwnAsm) {
    // CU0: `main` carries a DISTINCT, deliberately minimal descriptor.
    MirAsmDescriptor other;
    other.templateText = "nop";
    other.isExtended   = false;

    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i64_0 = in0.primitive(TypeKind::I64);
    TypeId const sig0  = in0.fnSig({}, i64_0, CallConv::CcSysV);
    Mir mir0;
    {
        MirBuilder mb;
        mb.addFunction(sig0, SymbolId{100}, SymbolBinding::Global,
                       SymbolVisibility::Default);
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        (void)mb.addInlineAsm(std::move(other), {}, InvalidType);
        MirInstId const fAddr = mb.addGlobalAddr(SymbolId{10}, sig0);
        MirInstId const ops[] = {fAddr};
        mb.addReturn(mb.addInst(MirOpcode::Call, ops, i64_0));
        mir0 = std::move(mb).finish();
    }
    std::vector<ExternImport> const ext0 = {ExternImport{SymbolId{10}, "f", "libc.so"}};

    TypeInterner in1{CompilationUnitId{2}};
    TypeId const i64_1 = in1.primitive(TypeKind::I64);
    TypeId const sig1  = in1.fnSig({}, i64_1, CallConv::CcSysV);
    Mir mir1;
    {
        MirBuilder mb;
        addAsmCarryingFunc(mb, i64_1, sig1, SymbolId{50}, sentinelDescriptor());
        mir1 = std::move(mb).finish();
    }
    // Anti-vacuity: BOTH sources carry exactly one descriptor each, and they are
    // at the SAME index (0) in their own pools — which is precisely why a
    // forwarded index cannot be right for both after the merge.
    ASSERT_EQ(mir0.asmDescriptorPool().size(), 1u);
    ASSERT_EQ(mir1.asmDescriptorPool().size(), 1u);

    std::vector<MergeCuInput> const cus = {
        MergeCuInput{&mir0, &in0, namerOf({{100, "main"}, {10, "f"}}), ext0},
        MergeCuInput{&mir1, &in1, namerOf({{50, "f"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

    Mir const& mm = merged->mir;
    EXPECT_EQ(mm.asmDescriptorPool().size(), 2u)
        << "both CUs' descriptors must reach the merged pool";

    // Walk every asm and bucket it by the template its instruction NAMES.
    std::vector<std::string> templatesSeen;
    for (std::uint32_t fi = 0; fi < mm.moduleFuncCount(); ++fi) {
        MirFuncId const f = mm.funcAt(fi);
        for (std::uint32_t bi = 0; bi < mm.funcBlockCount(f); ++bi) {
            MirBlockId const b = mm.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mm.blockInstCount(b); ++ii) {
                MirInstId const id = mm.blockInstAt(b, ii);
                if (mm.instOpcode(id) != MirOpcode::InlineAsm) continue;
                templatesSeen.push_back(mm.asmDescriptor(id).templateText);
            }
        }
    }
    ASSERT_EQ(templatesSeen.size(), 2u);
    // One of each, and neither instruction may name the other's descriptor.
    std::size_t nops = 0, rdtscs = 0;
    for (auto const& t : templatesSeen) {
        if (t == "nop") ++nops;
        else if (t == "cpuid; rdtsc") ++rdtscs;
        else ADD_FAILURE() << "unexpected template text: " << t;
    }
    EXPECT_EQ(nops, 1u) << "CU0's asm must still name CU0's descriptor";
    EXPECT_EQ(rdtscs, 1u) << "CU1's asm must still name CU1's descriptor";
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

// ── the ANCHOR: an `asm goto` output piece survives every rebuild ────────────
//
// ★★★ A DIFFERENT FACT FROM EVERYTHING ABOVE, AND IT WAS A PROCESS KILL.
// The tests above pin that the DESCRIPTOR survives a copy. These pin that the
// producer→piece ANCHOR does — a strictly separate carriage, because a
// `ReturnPiece`'s single operand names its producer and, for an `asm goto` WITH
// OUTPUTS, that producer is the block's TERMINATOR. Every clone site keys its
// operand translation off an old→new map; a site that declines to record a
// terminator therefore cannot translate that anchor, and
// (D-OPT-ASM-GOTO-WITH-OUTPUTS-ABORTS-THE-MIR-REBUILDER) it did not fail — it
// called `std::abort`.
//
// ✔MEASURED 2026-08-17 through the real CLI, at the pristine HEAD these tests
// were written against:
//   `__asm__ goto ("movl $42,%0\n\tjmp %l[done]" : "=r"(a) : : "cc" : done);`
//   --config=release  → exit **127**, "MirFunctionRebuilder fatal: rewriteOperand:
//                       old MirInstId v=2 has no rewrite entry"
//   --config=debug    → exit 1 with a clean `L_UnsupportedLoweringForOpcode`
// and the two controls isolated it exactly: the SAME statement with NO outputs
// was clean at both configs, and the output-bearing form was clean at debug. The
// `DSS_OPT_TRACE` log named the pass — Identity/ConstFold/Mem2Reg/CopyProp/Cse/
// Licm all rebuilt it (they took the hook's `true` default); `SimplifyCfg`, one
// of the two policies that answered `false`, was the first to die.
//
// ⚠ WHY THESE ARE POSITIVE ASSERTIONS AND NOT MERELY "IT DID NOT CRASH". A
// process abort already fails a gtest binary loudly, so a bare drive-it-and-see
// test would look identical whether the anchor was translated CORRECTLY or
// merely translated. Each test therefore asserts the rebuilt piece's operand IS
// the rebuilt module's own `InlineAsmGoto` — an anchor re-pointed at a stale or
// wrong instruction is the silent half of this defect and is what the identity
// assertion catches.

namespace {

// An `asm goto` with ONE output, its piece placed by the builder's own
// edge-placement rule at the head of the interposed landing block. Returns the
// terminator so a caller can assert against it.
//
// ★ Through `addInlineAsmGoto` (the rule's owner), never by hand-placing the
// piece: a fixture that built the shape itself would be testing the fixture's
// idea of the shape, not the one the front end ships.
MirInstId plantAsmGotoWithOneOutput(MirBuilder& mb, TypeId ty,
                                    MirBlockId label) {
    MirAsmDescriptor d;
    d.templateText = "movl $42,%0; jmp %l[done]";
    d.isVolatile   = true;
    d.isExtended   = true;
    MirAsmOperand o0;
    o0.constraint = "=r";
    o0.regClass   = TargetRegClass::GPR;
    d.outputs.push_back(o0);
    MirBlockId const labels[] = {label};
    auto res = mb.addInlineAsmGoto(std::move(d), {}, labels);
    for (auto const& e : res.edges) {
        if (!e.split) continue;
        mb.beginBlock(e.successor);
        (void)mb.addReturnPiece(res.terminator, 0, TargetRegClass::GPR, ty);
        mb.addBr(e.label);
    }
    return res.terminator;
}

// Assert the module holds exactly one `InlineAsmGoto` and exactly one
// `ReturnPiece`, and that the piece's operand IS that terminator.
void expectPieceAnchoredToTheGoto(Mir const& mir, char const* where) {
    std::vector<MirInstId> gotos;
    std::vector<MirInstId> pieces;
    for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
                MirInstId const id = mir.blockInstAt(b, ii);
                if (mir.instOpcode(id) == MirOpcode::InlineAsmGoto) gotos.push_back(id);
                if (mir.instOpcode(id) == MirOpcode::ReturnPiece)   pieces.push_back(id);
            }
        }
    }
    ASSERT_EQ(gotos.size(), 1u) << where;
    ASSERT_EQ(pieces.size(), 1u)
        << where << ": the output piece must SURVIVE — dropping it is the "
                    "silently-wrong alternative to the abort this pins";
    auto const ops = mir.instOperands(pieces[0]);
    ASSERT_EQ(ops.size(), 1u) << where;
    EXPECT_EQ(ops[0], gotos[0])
        << where << ": the piece must anchor to THIS module's own asm goto — a "
                    "stale or wrong anchor is the silent half of the defect";
}

} // namespace

// ── anchor site 1a: the shared substrate, driven by the pass that died ──────
//
// SimplifyCfg is not a stand-in here: it is the exact policy whose
// `recordTerminatorInRewrite() { return false; }` killed the release compile.
//
// RED-ON-DISABLE: restore that override on `SimplifyCfgPolicy` (or make
// `MirFunctionRebuilder::emitTerminator`'s `remember` conditional again) and
// this test ABORTS the gtest binary in `rewriteOperand`.
TEST(InlineAsmRebuildCarriage, AnchorSite1aSimplifyCfgKeepsTheAsmGotoPieceAnchor) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const done  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    (void)plantAsmGotoWithOneOutput(mb, i64, done);
    mb.beginBlock(done);
    mb.addReturn();
    Mir mir = std::move(mb).finish();
    expectPieceAnchoredToTheGoto(mir, "pre-pass fixture");

    DiagnosticReporter rep;
    (void)runSimplifyCfg(mir, interner, rep);
    expectPieceAnchoredToTheGoto(mir, "after SimplifyCfg");
}

// ── anchor site 1b: the other policy that answered `false` ──────────────────
//
// Dce answered `false` for cost ("saves a few hash inserts"). It is pinned
// SEPARATELY from SimplifyCfg rather than assumed equivalent: the two reach the
// shared `emitTerminator` through different `selectBlocks` sets, and a fix that
// somehow only served the RPO-reachable walk would pass one and fail the other.
//
// ⚠ ALSO AN ANTI-DROP PIN. `ReturnPiece` is `hasSideEffects`, so DCE must KEEP
// it. Skipping the piece would be the "trade an abort for a silently dropped
// block" outcome, so `expectPieceAnchoredToTheGoto` asserting exactly one piece
// is load-bearing here, not decoration.
TEST(InlineAsmRebuildCarriage, AnchorSite1bDceKeepsTheAsmGotoPieceAnchor) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const done  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    (void)plantAsmGotoWithOneOutput(mb, i64, done);
    mb.beginBlock(done);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    (void)runDce(mir, interner, rep);
    expectPieceAnchoredToTheGoto(mir, "after Dce");
}

// ── anchor site 2: the inliner splicing a CALLEE that contains the asm goto ──
//
// ★★ A SECOND CLONER, FOUND BY PROBING RATHER THAN BY READING. The shared
// substrate fix does NOT reach this path — `Inlining` splices callee bodies with
// its own walk and its own `local` map. ✔MEASURED 2026-08-17, with the substrate
// already fixed, a static callee holding an `asm goto` with an output, inlined
// into `main`, still exited **127** at release with a DIFFERENT abort:
// "callee operand v=4 (callee funcId v=1) has no local mapping during splice".
// Same defect, different map — which is why this row could not be closed on the
// substrate test alone.
TEST(InlineAsmRebuildCarriage, AnchorSite2InlinedCalleeKeepsTheAsmGotoPieceAnchor) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    // callee: entry holds the asm goto; `done` is its landing target. The asm
    // goto seals its own block, so this is the MULTI-BLOCK splice — the path
    // that aborted.
    mb.addFunction(fnSig, SymbolId{50}, SymbolBinding::Global,
                   SymbolVisibility::Default);
    MirBlockId const cEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const cDone  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(cEntry);
    (void)plantAsmGotoWithOneOutput(mb, i64, cDone);
    mb.beginBlock(cDone);
    mb.addReturn();

    // caller: no asm of its own, so the copy that appears in it is the one that
    // travelled across the function boundary.
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
        << "anti-vacuity: if nothing was inlined the callee splice never ran";

    // Both copies (the callee keeps its body) must be internally consistent —
    // each piece anchored to the goto in ITS OWN function, never across.
    //
    // ★★ THE PER-FUNCTION BODY RUNS THROUGH A **VOID CALLABLE**, AND THAT IS A
    // CORRECTNESS PROPERTY OF THE TEST, NOT A STYLE CHOICE. A bare `ASSERT_*`
    // expands to `return;` FROM THE ENCLOSING FUNCTION, so written inline in this
    // loop the first arm to fail would abort the whole TEST — the second function
    // would never be examined and the `checked == 2` anti-vacuity guard below
    // would never execute either. That is exactly the shape where a safe arm
    // masks a dangerous one (the same vacuity another lane measured this cycle).
    // Routing each arm through a `void` lambda makes `ASSERT_*` return from THE
    // ARM, so every function is always visited and every failure is reported.
    std::size_t checked = 0;
    auto checkFunction = [&](std::uint32_t fi, MirInstId gotoId,
                             std::vector<MirInstId> const& pieces) {
        ASSERT_EQ(pieces.size(), 1u) << "function ordinal " << fi;
        auto const pops = mir.instOperands(pieces[0]);
        ASSERT_EQ(pops.size(), 1u) << "function ordinal " << fi;
        EXPECT_EQ(pops[0], gotoId)
            << "function ordinal " << fi
            << ": the spliced piece must anchor to the asm goto in the SAME "
               "function — an anchor left pointing into the callee is the "
               "cross-function version of this defect";
    };
    for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        MirInstId gotoId{};
        std::vector<MirInstId> pieces;
        for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
                MirInstId const id = mir.blockInstAt(b, ii);
                if (mir.instOpcode(id) == MirOpcode::InlineAsmGoto) gotoId = id;
                if (mir.instOpcode(id) == MirOpcode::ReturnPiece) pieces.push_back(id);
            }
        }
        if (!gotoId.valid()) continue;
        checkFunction(fi, gotoId, pieces);
        ++checked;
    }
    EXPECT_EQ(checked, 2u)
        << "both the original callee body and the spliced copy must be checked";
}

// ── anchor site 3: the MultiBlockInliner rebuilding the CALLER host ─────────
//
// The asm goto is in the function being REBUILT, not in the body being spliced —
// the third clone site, and the one whose `remember` had to be added to the
// CALLER terminator switch rather than the callee one.
TEST(InlineAsmRebuildCarriage, AnchorSite3MultiBlockHostKeepsTheAsmGotoPieceAnchor) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    // callee: TWO blocks, no asm — the multi-block router (site 3's precondition,
    // measured in test_inlining.cpp: a single-block callee would route the host
    // through MirFunctionRebuilder and silently re-test site 1).
    mb.addFunction(fnSig, SymbolId{50}, SymbolBinding::Global,
                   SymbolVisibility::Default);
    MirBlockId const cEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const cTail  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(cEntry);
    mb.addBr(cTail);
    mb.beginBlock(cTail);
    mb.addReturn();

    // caller (the HOST): calls the callee, THEN carries the asm goto.
    mb.addFunction(fnSig, SymbolId{100}, SymbolBinding::Global,
                   SymbolVisibility::Default);
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const mDone  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(mEntry);
    MirInstId const calleeAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const ops[]      = {calleeAddr};
    (void)mb.addInst(MirOpcode::Call, ops, InvalidType);
    (void)plantAsmGotoWithOneOutput(mb, i64, mDone);
    mb.beginBlock(mDone);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    DiagnosticReporter rep;
    auto const r = runInlining(mir, interner, rep, kThreshold);
    ASSERT_EQ(r.callsInlined, 1u)
        << "anti-vacuity: if nothing was inlined the host was never rebuilt";
    expectPieceAnchoredToTheGoto(mir, "site 3 (MultiBlockInliner host)");
}

// ── anchor site 4: the CROSS-CU merge cloning the asm goto between MODULES ──
//
// The terminator half of site 4, and it failed DIFFERENTLY from the value half,
// which is why it is pinned separately rather than assumed covered.
// `emitValue`'s missing `InlineAsm` arm fell through to `addInst`, which REFUSES
// the opcode — a loud abort naming the builder. `emitTerminator` had no
// `InlineAsmGoto` arm at all, so it never reached `addInst`: it hit its own
// switch default, `dss::mergeCuMirs fatal: CU 0 terminator opcode 79 marked
// isTerminator but has no clone arm`. ✔MEASURED 2026-08-17 through the real CLI:
// a 2-TU build of an `asm goto` exited **127** there, where the SAME source as
// ONE TU exited 1 with a clean `L_UnsupportedLoweringForOpcode` refusal. The
// merge gap was converting a correct, reportable refusal into a process abort.
//
// ⚠ THIS IS REACHABLE AT THE MIR TIER AND NOWHERE ELSE TODAY, which is exactly
// why the pin lives here and not in `examples/`. `asm goto` does not lower to
// LIR yet (D-LIR-ASM-GOTO-REFUSAL-NAMES-A-CONFIG-GAP-THAT-NO-LONGER-EXISTS), so
// no runnable corpus example can reach the merge with one — a unit test is the
// only instrument that can hold this arm, and without it the arm would be
// unwitnessed until the day asm goto starts lowering.
//
// The assertion is POSITIVE, not "it did not crash": `cloneInlineAsmGoto` is
// what must be called (never `addInlineAsmGoto`, which owns the edge-placement
// rule and would interpose a SECOND landing block on top of the cloned one), and
// the merge must record the terminator in its value map so the piece at the head
// of the landing block can still name it.
TEST(InlineAsmRebuildCarriage, AnchorSite4CrossCuMergeKeepsTheAsmGotoPieceAnchor) {
    // CU0: `main` calls extern `f`.
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i64_0  = in0.primitive(TypeKind::I64);
    TypeId const void_0 = in0.primitive(TypeKind::Void);
    TypeId const sig0   = in0.fnSig({}, void_0, CallConv::CcSysV);
    Mir mir0;
    {
        MirBuilder mb;
        mb.addFunction(sig0, SymbolId{100}, SymbolBinding::Global,
                       SymbolVisibility::Default);
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        MirInstId const fAddr = mb.addGlobalAddr(SymbolId{10}, sig0);
        MirInstId const ops[] = {fAddr};
        (void)mb.addInst(MirOpcode::Call, ops, InvalidType);
        mb.addReturn();
        mir0 = std::move(mb).finish();
    }
    (void)i64_0;
    std::vector<ExternImport> const ext0 = {ExternImport{SymbolId{10}, "f", "libc.so"}};

    // CU1: `f` carries the `asm goto` WITH an output — the shape whose piece
    // anchors to the TERMINATOR.
    TypeInterner in1{CompilationUnitId{2}};
    TypeId const i64_1  = in1.primitive(TypeKind::I64);
    TypeId const void_1 = in1.primitive(TypeKind::Void);
    TypeId const sig1   = in1.fnSig({}, void_1, CallConv::CcSysV);
    Mir mir1;
    {
        MirBuilder mb;
        mb.addFunction(sig1, SymbolId{50}, SymbolBinding::Global,
                       SymbolVisibility::Default);
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const done  = mb.createBlock(StructCfMarker::Linear);
        mb.beginBlock(entry);
        (void)plantAsmGotoWithOneOutput(mb, i64_1, done);
        mb.beginBlock(done);
        mb.addReturn();
        mir1 = std::move(mb).finish();
    }
    // Anti-vacuity: the SOURCE really carries the shape being tested.
    expectPieceAnchoredToTheGoto(mir1, "pre-merge fixture (CU1)");

    std::vector<MergeCuInput> const cus = {
        MergeCuInput{&mir0, &in0, namerOf({{100, "main"}, {10, "f"}}), ext0},
        MergeCuInput{&mir1, &in1, namerOf({{50, "f"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

    expectPieceAnchoredToTheGoto(merged->mir, "after the cross-CU merge");
    // The descriptor made the crossing too — the terminator arm re-adds it to
    // the merged pool exactly as the value arm does.
    EXPECT_EQ(merged->mir.asmDescriptorPool().size(), 1u);
    // ★ EXACTLY ONE landing block's worth of edges: `cloneInlineAsmGoto` takes
    // the successors VERBATIM. If this arm ever called `addInlineAsmGoto`
    // instead, the placement rule would run a second time and interpose ANOTHER
    // landing block, which this count is what notices.
    std::uint32_t brCount = 0;
    for (std::uint32_t fi = 0; fi < merged->mir.moduleFuncCount(); ++fi) {
        MirFuncId const f = merged->mir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < merged->mir.funcBlockCount(f); ++bi) {
            MirBlockId const b = merged->mir.funcBlockAt(f, bi);
            if (merged->mir.instOpcode(merged->mir.blockTerminator(b)) == MirOpcode::Br) {
                ++brCount;
            }
        }
    }
    EXPECT_EQ(brCount, 1u)
        << "one interposed landing block, cloned verbatim — a second one means "
           "the merge re-ran the edge-placement rule instead of cloning";
}
