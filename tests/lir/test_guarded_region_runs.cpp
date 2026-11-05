// A GUARDED REGION IS A SET OF CONTIGUOUS RUNS, ONE SCOPE RECORD PER RUN
// (D-LIR-GUARDED-RANGE-DOES-NOT-COVER-BLOCKS-THE-LOWERING-CREATES) — MIR→LIR's half.
//
// ═══ WHAT IS PINNED ════════════════════════════════════════════════════════
//
// MIR→LIR gives every MIR block its LIR block before it lowers a body, so a block
// it CREATES while lowering an instruction — a phi edge's split, a switch's compare
// chain or table read, an `asm goto`'s edge blocks, the retry loop of an LL/SC
// compare-exchange — is laid out after every one of the function's own blocks. A
// `__try` region's scope record used to name the region's own blocks alone, so the
// created ones were outside the `__try` they were written in (✔MEASURED P69 on pe64:
// a store an `asm goto` makes on its edges, over a no-access page inside a `__try`,
// ended the process with 0xC0000005). Now a created block belongs to the MIR block
// being lowered when it is created, and each region gets, after the record of its
// own blocks, one more record per maximal run of contiguous created blocks whose
// owner is a block of the region. So:
//   * THE EXACT SEQUENCE of a nested pair's records — inner body, inner run, outer
//     body, outer run: the dispatch gives a fault to the FIRST record whose range
//     holds it, a region's own records stay together, and a created block of the
//     inner body is in a run of the inner region AND of the outer one;
//   * the regions come out in the order they are HANDED IN (the order of regions
//     has one owner, mir/merge/synth_seh_funclets.hpp; this tier never sorts);
//   * a run never holds its record's landing block, and never a created block
//     whose owner is outside the region;
//   * a run holds blocks of its own function only;
//   * a region whose blocks made the lowering create nothing has one record;
//   * EVERY created block of a region is in its run, whatever created it (a phi
//     split, a sparse switch's chain, a dense switch's table read);
//   * WHY A RUN NEVER WRITES A REGISTER ITS LANDING BLOCK READS: the one shape that
//     does is a phi above the region fed from inside it — built by hand below, in
//     a created block and in the region's own block alike. No source program has
//     it for a LOCAL, because a function that guards a region keeps its locals in
//     memory (mem2reg promotes nothing there): the two nearest sources are
//     compiled with the shipped release schedule and hold no promoted local's phi,
//     while the same loop without the `__try` does.
//   * WHAT A SOURCE PROGRAM DOES PUT IN A RUN: the phi the MIR lowering makes
//     ITSELF, with no promotion pass involved — for `&&` and `||`, for a scalar
//     `?:` and for `va_arg`. `a && b` is `condbr a → [b], join` with a phi at the
//     join: a two-way predecessor feeding a phi, exactly the edge this tier splits
//     in a block it creates. So a guarded body that holds `p && *p` has a run of
//     its own — the one created block, the split — and the register that split
//     writes is read by the join and by nothing the handler runs (✔MEASURED
//     2026-10-12 on pe64, debug and release, DSS beside cl over 78 cells: the
//     image's scope table holds 2 records for `&&` and for `||`, 4 for a nested
//     pair, and 1 for a scalar `?:` and for `va_arg`, which get no created block
//     there).
//
// The LL/SC creator is pinned beside its lowering (lir/test_atomic_cas_runtime_routing),
// the `asm goto` one and the short-circuit's split through the driver, on the
// produced image (program/test_compile_pipeline), and the runnable witnesses are
// examples/c/seh_asm_goto_output_stored_on_its_edges_is_guarded and
// examples/c/seh_short_circuit_in_a_guarded_body.
//
// THE MUTATIONS THAT HOLD THESE PINS are arms of tests/mutations/arms.registry,
// driven by `dssharness check-mutations`. Each of the nine READ `passed` in the
// sweeps of checkpoint 14 (macos-arm64-debug: run 20261012-151311-82d3dd43 for the
// eight `lir-` arms, run 20261012-145059-ba433418 for the `opt-` one), against the
// SEVEN cases this file held that day. The eighth — the short-circuit's split,
// the last pin below — is younger than those sweeps, and three of the arms are
// DECLARED to redden it too (`lir-no-run-is-emitted_guarded-region-runs`,
// `lir-a-phi-edges-split-is-created-around-the-creator`,
// `opt-mem2reg-promotes-in-a-function-that-guards-a-region`): for that case they
// are WRITTEN, NOT READ. What each arm makes the code do:
//   lir-every-bodys-record-then-every-run
//       the runs emitted after ALL the bodies' records, not after their own region's
//   lir-no-run-is-emitted_guarded-region-runs
//       no run emitted
//   lir-a-run-takes-every-created-block-of-its-function
//       a run that takes every created block of the function
//   lir-a-run-ignores-which-function-a-block-is-in
//       a run that ignores which function a created block is in
//   lir-a-phi-edges-split-is-created-around-the-creator,
//   lir-a-table-read-is-created-around-the-creator,
//   lir-a-chains-default-jump-is-created-around-the-creator,
//   lir-a-chains-next-compare-is-created-around-the-creator
//       a creator that goes around the one place a created block is written down
//       (refused by name on the finished module)
//   opt-mem2reg-promotes-in-a-function-that-guards-a-region
//       mem2reg promoting in a function that guards a region

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "ffi/abi/abi_catalog.hpp"
#include "link/object_format_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/merge/synth_seh_funclets.hpp"   // MirSehScope
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "program/compile_pipeline.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

MirLiteralValue boolLit(bool v) {
    MirLiteralValue lit;
    lit.value = std::uint64_t{v ? 1u : 0u};
    lit.core  = TypeKind::Bool;
    return lit;
}

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

std::string allDiagText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) { out += '['; out += d.actual; out += ']'; }
    return out;
}

// ── THE NESTED-PAIR FIXTURE ───────────────────────────────────────────────────
//
// One function `bool f(i32 x)`, its blocks in the function's own order:
//
//   A    before every region         condbr c → pA , B
//   B    the OUTER body's first       condbr c → pB , C
//   C    the INNER body (one block)   condbr c → pC1, pC2     two edges, two splits
//   Hi   the inner handler            ret false               (the outer body holds it)
//   D    the OUTER body's last        condbr c → pD , E
//   Ho   the outer handler            ret false
//   E    after every region           condbr c → pE , R
//   R                                 ret false
//   pA pB pC1 pD pE                   each opens with a phi fed from its one
//                                     predecessor and returns it
//   pC2                               the same phi, then br D
//
// A `condbr` to a block that holds a phi gets a SPLIT block for that edge — a block
// the lowering creates — and the split of the edge X→pX is the one block whose only
// successor is pX's. That is the oracle below: which created block is whose is read
// off the lowered CFG, never off a count of how many blocks a creator makes.
enum Role : std::uint32_t {
    kA, kB, kC, kHi, kD, kHo, kE, kR, kPA, kPB, kPC1, kPC2, kPD, kPE, kRoleCount
};

struct NestedPair {
    std::array<MirBlockId, kRoleCount> block{};
    SymbolId                           symbol{};
};

NestedPair addNestedPair(MirBuilder& mb, TypeInterner& ti, SymbolId symbol) {
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const boolT = ti.primitive(TypeKind::Bool);
    std::array<TypeId, 1> const params{i32};
    mb.addFunction(ti.fnSig(params, boolT, CallConv::CcSysV), symbol);
    NestedPair f;
    f.symbol = symbol;
    f.block[kA] = mb.createBlock(StructCfMarker::EntryBlock);
    for (std::uint32_t r = kB; r < kRoleCount; ++r) {
        f.block[r] = mb.createBlock(StructCfMarker::Linear);
    }
    auto const at = [&](Role r) { return f.block[r]; };

    mb.beginBlock(at(kA));
    MirInstId const x    = mb.addArg(0, i32);
    MirInstId const zero = mb.addConst(i32Lit(0), i32);
    std::array<MirInstId, 2> const cmpOps{x, zero};
    MirInstId const c = mb.addInst(MirOpcode::ICmpSlt, cmpOps, boolT);
    mb.addCondBr(c, at(kPA), at(kB));

    mb.beginBlock(at(kB));
    mb.addCondBr(c, at(kPB), at(kC));
    mb.beginBlock(at(kC));
    mb.addCondBr(c, at(kPC1), at(kPC2));
    mb.beginBlock(at(kHi));
    mb.addReturn(mb.addConst(boolLit(false), boolT));
    mb.beginBlock(at(kD));
    mb.addCondBr(c, at(kPD), at(kE));
    mb.beginBlock(at(kHo));
    mb.addReturn(mb.addConst(boolLit(false), boolT));
    mb.beginBlock(at(kE));
    mb.addCondBr(c, at(kPE), at(kR));
    mb.beginBlock(at(kR));
    mb.addReturn(mb.addConst(boolLit(false), boolT));

    auto const phiBlock = [&](Role self, Role pred, std::optional<Role> onward) {
        mb.beginBlock(at(self));
        std::array<MirPhiIncoming, 1> const in{MirPhiIncoming{c, at(pred)}};
        MirInstId const phi = mb.addPhi(boolT, in);
        if (onward.has_value()) mb.addBr(at(*onward));
        else                    mb.addReturn(phi);
    };
    phiBlock(kPA, kA, std::nullopt);
    phiBlock(kPB, kB, std::nullopt);
    phiBlock(kPC1, kC, std::nullopt);
    phiBlock(kPC2, kC, kD);
    phiBlock(kPD, kD, std::nullopt);
    phiBlock(kPE, kE, std::nullopt);
    return f;
}

SymbolId const kPersonality{900};
SymbolId const kInnerFunclet{901};
SymbolId const kOuterFunclet{902};

MirSehScope scopeOver(NestedPair const& f, Role first, Role last, Role handler,
                      SymbolId funclet) {
    MirSehScope s;
    s.parentFuncSymbol    = f.symbol;
    s.beginBlock          = f.block[first];
    s.endBlock            = f.block[last];
    s.handlerBlock        = f.block[handler];
    s.filterFuncletSymbol = funclet;
    s.personalitySymbol   = kPersonality;
    return s;
}

MirSehScope innerScope(NestedPair const& f) { return scopeOver(f, kC, kC, kHi, kInnerFunclet); }
MirSehScope outerScope(NestedPair const& f) { return scopeOver(f, kB, kD, kHo, kOuterFunclet); }

struct Lowered {
    MirToLirResult     result;
    DiagnosticReporter rep;
};

Lowered lowerWithScopes(Mir const& mir, TypeInterner const& ti, std::span<MirSehScope const> scopes,
                        char const* targetName = "x86_64") {
    auto const target = TargetSchema::loadShipped(targetName);
    EXPECT_TRUE(target.has_value());
    Lowered out;
    if (!target.has_value()) return out;
    out.result = lowerToLir(mir, **target, ti, out.rep, std::vector<ExternImport>{},
                            std::nullopt, std::nullopt, std::nullopt, scopes);
    return out;
}

// The LIR block a MIR block became: the 1:1 pre-pass keeps the function's own order.
std::uint32_t lirOf(Lir const& lir, std::uint32_t funcIndex, std::uint32_t role) {
    return lir.funcBlockAt(lir.funcAt(funcIndex), role).v;
}

// The created block that is the split of the edge into `phiRole`'s block: the one
// block past the function's own whose only successor is that block. 0 when there is
// not exactly one.
std::uint32_t splitInto(Lir const& lir, std::uint32_t funcIndex, std::uint32_t phiRole) {
    LirFuncId const fn = lir.funcAt(funcIndex);
    std::uint32_t const target = lir.funcBlockAt(fn, phiRole).v;
    std::uint32_t found = 0, count = 0;
    for (std::uint32_t k = kRoleCount; k < lir.funcBlockCount(fn); ++k) {
        LirBlockId const b = lir.funcBlockAt(fn, k);
        auto const succs = lir.blockSuccessors(b);
        if (succs.size() == 1 && succs[0].v == target) { found = b.v; ++count; }
    }
    return count == 1 ? found : 0;
}

struct Record {
    std::size_t   funcIndex;
    std::uint32_t begin, end, handler;
    std::uint32_t funclet, personality;
    bool operator==(Record const&) const = default;
};

std::vector<Record> recordsOf(MirToLirResult const& r) {
    std::vector<Record> out;
    for (auto const& d : r.sehScopeDescriptors) {
        out.push_back(Record{d.funcIndex, d.beginLirBlockV, d.endLirBlockV, d.handlerLirBlockV,
                             d.filterFuncletSymbol.v, d.personalitySymbol.v});
    }
    return out;
}

std::string shown(std::vector<Record> const& rs) {
    std::string out;
    for (auto const& r : rs) {
        out += "\n  { fn " + std::to_string(r.funcIndex) + ", blocks " + std::to_string(r.begin)
             + ".." + std::to_string(r.end) + ", handler " + std::to_string(r.handler)
             + ", funclet " + std::to_string(r.funclet) + " }";
    }
    return out;
}

// What the nested pair's four records must be, for function #`fi` of `lir`.
struct Expected {
    Record innerBody, innerRun, outerBody, outerRun;
};

Expected expectedFor(Lir const& lir, std::uint32_t fi) {
    auto const L = [&](Role r) { return lirOf(lir, fi, r); };
    auto const S = [&](Role r) { return splitInto(lir, fi, r); };
    return Expected{
        Record{fi, L(kC), L(kC), L(kHi), kInnerFunclet.v, kPersonality.v},
        Record{fi, S(kPC1), S(kPC2), L(kHi), kInnerFunclet.v, kPersonality.v},
        Record{fi, L(kB), L(kD), L(kHo), kOuterFunclet.v, kPersonality.v},
        Record{fi, S(kPB), S(kPD), L(kHo), kOuterFunclet.v, kPersonality.v},
    };
}

}  // namespace

// THE EXACT SEQUENCE. The scopes are handed in table order — the inner region first
// (each region after every region inside it) — and the records come out: the inner
// body, the inner run, the outer body, the outer run. The weaker order — every
// body's record, then every run — is what a first-match dispatch could survive only
// by accident, and nothing downstream refuses it: this is its pin.
TEST(GuardedRegionRuns, ANestedPairsRecordsAreInnerBodyInnerRunOuterBodyOuterRun) {
    TypeInterner ti{CompilationUnitId{1}};
    MirBuilder mb;
    mb.stateSelfContainedSymbolIds();
    NestedPair const f = addNestedPair(mb, ti, SymbolId{1});
    Mir mir = std::move(mb).finish();
    rederiveStructCfMarkers(mir);
    std::array<MirSehScope, 2> const scopes{innerScope(f), outerScope(f)};
    Lowered const low = lowerWithScopes(mir, ti, scopes);
    ASSERT_TRUE(low.result.ok) << allDiagText(low.rep);
    Lir const& lir = low.result.lir;

    // The fixture's premise, read off the lowered function: six edges were split,
    // each split is found by the block it jumps to, and they were created in the
    // order their owners were lowered — A's, B's, C's two, D's, E's.
    LirFuncId const fn = lir.funcAt(0);
    ASSERT_EQ(lir.funcBlockCount(fn), kRoleCount + 6u) << "one created block per split edge";
    std::array<std::uint32_t, 6> const splits{
        splitInto(lir, 0, kPA), splitInto(lir, 0, kPB), splitInto(lir, 0, kPC1),
        splitInto(lir, 0, kPC2), splitInto(lir, 0, kPD), splitInto(lir, 0, kPE)};
    for (std::uint32_t const s : splits) ASSERT_NE(s, 0u) << "an edge's split block was not found";
    ASSERT_TRUE(std::is_sorted(splits.begin(), splits.end()));
    ASSERT_EQ(splits[5] - splits[0], 5u) << "the six are one stretch at the function's end";

    Expected const want = expectedFor(lir, 0);
    std::vector<Record> const got = recordsOf(low.result);
    std::vector<Record> const sequence{want.innerBody, want.innerRun, want.outerBody, want.outerRun};
    EXPECT_EQ(got, sequence) << "got:" << shown(got) << "\nwant:" << shown(sequence);

    // Said once more as the properties the sequence stands for.
    for (Record const& r : got) {
        EXPECT_FALSE(r.handler >= r.begin && r.handler <= r.end)
            << "a record's range holds its own landing block: " << shown({r});
        EXPECT_LE(r.begin, r.end);
    }
    ASSERT_EQ(got.size(), 4u);
    // The inner run is C's two splits and nothing else; the outer run is every
    // split of B, C and D; A's and E's are in no record at all.
    EXPECT_EQ(got[1].begin, splits[2]);
    EXPECT_EQ(got[1].end, splits[3]);
    EXPECT_EQ(got[3].begin, splits[1]);
    EXPECT_EQ(got[3].end, splits[4]);
    // A CREATED BLOCK OF THE INNER BODY IS UNDER A RECORD OF EACH LANDING AROUND IT.
    // The tier that takes these records asks, per record, whether its blocks define
    // a register that record's landing reads — so a block the inner body made the
    // lowering create is seen against the OUTER landing only if a record of the
    // outer landing holds it. Said by landing, for each of C's two splits.
    for (std::uint32_t const innerCreated : {splits[2], splits[3]}) {
        for (std::uint32_t const landing : {want.innerBody.handler, want.outerBody.handler}) {
            EXPECT_EQ(std::count_if(got.begin(), got.end(),
                                    [&](Record const& r) {
                                        return r.handler == landing && innerCreated >= r.begin
                                            && innerCreated <= r.end;
                                    }),
                      1)
                << "created block " << innerCreated << " of the inner body, landing " << landing;
        }
    }
    for (Record const& r : got) {
        EXPECT_FALSE(splits[0] >= r.begin && splits[0] <= r.end)
            << "a block created for a block BEFORE every region is under a record";
        EXPECT_FALSE(splits[5] >= r.begin && splits[5] <= r.end)
            << "a block created for a block AFTER every region is under a record";
    }
}

// The order of REGIONS is the caller's (the funclet pass owns it). Handed the outer
// region first, the lowering answers the outer body, the outer run, the inner body,
// the inner run: it follows, it never sorts — and each region's records stay
// together whichever way round the regions come.
TEST(GuardedRegionRuns, TheRegionsStayInTheOrderTheyAreHandedIn) {
    TypeInterner ti{CompilationUnitId{1}};
    MirBuilder mb;
    mb.stateSelfContainedSymbolIds();
    NestedPair const f = addNestedPair(mb, ti, SymbolId{1});
    Mir mir = std::move(mb).finish();
    rederiveStructCfMarkers(mir);
    std::array<MirSehScope, 2> const scopes{outerScope(f), innerScope(f)};
    Lowered const low = lowerWithScopes(mir, ti, scopes);
    ASSERT_TRUE(low.result.ok) << allDiagText(low.rep);
    Expected const want = expectedFor(low.result.lir, 0);
    std::vector<Record> const got = recordsOf(low.result);
    std::vector<Record> const sequence{want.outerBody, want.outerRun, want.innerBody, want.innerRun};
    EXPECT_EQ(got, sequence) << "got:" << shown(got) << "\nwant:" << shown(sequence);
}

// THE CONTROL: a region none of whose blocks made the lowering create anything has
// the one record it always had — beside a region that has a run, so the answer is
// not "no run was emitted for anyone".
TEST(GuardedRegionRuns, ARegionWhoseBlocksCreateNothingHasOneRecord) {
    TypeInterner ti{CompilationUnitId{1}};
    MirBuilder mb;
    mb.stateSelfContainedSymbolIds();
    NestedPair const f = addNestedPair(mb, ti, SymbolId{1});
    Mir mir = std::move(mb).finish();
    rederiveStructCfMarkers(mir);
    // `Hi` returns: nothing is created for it. `R` is its landing here.
    std::array<MirSehScope, 2> const scopes{scopeOver(f, kHi, kHi, kR, kInnerFunclet),
                                            scopeOver(f, kC, kC, kHo, kOuterFunclet)};
    Lowered const low = lowerWithScopes(mir, ti, scopes);
    ASSERT_TRUE(low.result.ok) << allDiagText(low.rep);
    Lir const& lir = low.result.lir;
    std::vector<Record> const got = recordsOf(low.result);
    std::vector<Record> const want{
        Record{0, lirOf(lir, 0, kHi), lirOf(lir, 0, kHi), lirOf(lir, 0, kR), kInnerFunclet.v, kPersonality.v},
        Record{0, lirOf(lir, 0, kC), lirOf(lir, 0, kC), lirOf(lir, 0, kHo), kOuterFunclet.v, kPersonality.v},
        Record{0, splitInto(lir, 0, kPC1), splitInto(lir, 0, kPC2), lirOf(lir, 0, kHo), kOuterFunclet.v,
               kPersonality.v},
    };
    EXPECT_EQ(got, want) << "got:" << shown(got) << "\nwant:" << shown(want);
}

// A RUN HOLDS BLOCKS OF ITS OWN FUNCTION. Two functions of one shape; the regions
// are the FIRST function's. A block's place is counted per function, so the second
// function's blocks sit at the very places the regions name — and none of its
// created blocks may come under a record. Then the regions are the SECOND
// function's, and the first one's stay out.
TEST(GuardedRegionRuns, ARunHoldsOnlyBlocksOfItsOwnFunction) {
    for (std::uint32_t const guarded : {0u, 1u}) {
        SCOPED_TRACE(guarded);
        TypeInterner ti{CompilationUnitId{1}};
        MirBuilder mb;
        mb.stateSelfContainedSymbolIds();
        std::array<NestedPair, 2> const fns{addNestedPair(mb, ti, SymbolId{1}),
                                            addNestedPair(mb, ti, SymbolId{2})};
        Mir mir = std::move(mb).finish();
        rederiveStructCfMarkers(mir);
        std::array<MirSehScope, 2> const scopes{innerScope(fns[guarded]), outerScope(fns[guarded])};
        Lowered const low = lowerWithScopes(mir, ti, scopes);
        ASSERT_TRUE(low.result.ok) << allDiagText(low.rep);
        Lir const& lir = low.result.lir;
        ASSERT_EQ(lir.moduleFuncCount(), 2u);
        Expected const want = expectedFor(lir, guarded);
        std::vector<Record> const got = recordsOf(low.result);
        std::vector<Record> const sequence{want.innerBody, want.innerRun, want.outerBody, want.outerRun};
        EXPECT_EQ(got, sequence) << "got:" << shown(got) << "\nwant:" << shown(sequence);
        LirFuncId const own = lir.funcAt(guarded);
        std::uint32_t const first = lir.funcBlockAt(own, 0).v;
        std::uint32_t const last  = lir.funcBlockAt(own, lir.funcBlockCount(own) - 1).v;
        for (Record const& r : got) {
            EXPECT_EQ(r.funcIndex, guarded);
            EXPECT_TRUE(r.begin >= first && r.end <= last)
                << "a record names a block of the other function:" << shown({r});
        }
    }
}

// EVERY CREATED BLOCK OF A REGION IS IN ITS RUN, WHATEVER CREATED IT. The region is
// every block of the function but its entry and its landing, and those two create
// nothing — so the run must be exactly the stretch from the first block past the
// function's own to its last. The body holds each creator a shipped x86_64 lowering
// has for plain MIR: a phi edge's split, a sparse switch's compare chain and a
// dense switch's table read. A creator that wrote its block down nowhere would
// leave the run short of the function's end (and is refused by name besides).
TEST(GuardedRegionRuns, ARegionsRunIsEveryBlockCreatedForItsBlocks) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const boolT = ti.primitive(TypeKind::Bool);
    std::array<TypeId, 1> const params{i32};
    MirBuilder mb;
    mb.stateSelfContainedSymbolIds();   // a dense switch's table symbol is minted
    mb.addFunction(ti.fnSig(params, i32, CallConv::CcSysV), SymbolId{1});
    constexpr std::size_t kDense = 10, kSparse = 3;
    // The function's own order: entry, then the region — `dense`, its ten cases,
    // `sparse`, its three cases, `branch`, `joined`, `done` — then the landing.
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const dense = mb.createBlock(StructCfMarker::Linear);
    std::array<MirBlockId, kDense> denseCase{};
    for (auto& b : denseCase) b = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const sparse = mb.createBlock(StructCfMarker::Linear);
    std::array<MirBlockId, kSparse> sparseCase{};
    for (auto& b : sparseCase) b = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const branch  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const joined  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const done    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const landing = mb.createBlock(StructCfMarker::Linear);

    mb.beginBlock(entry);
    MirInstId const x = mb.addArg(0, i32);
    mb.addBr(dense);

    // Ten dense cases (past the lowering's table floor): a jump table, whose read
    // is a block the lowering creates.
    mb.beginBlock(dense);
    std::array<std::pair<MirInstId, MirBlockId>, kDense> denseCases{};
    for (std::size_t i = 0; i < kDense; ++i) {
        denseCases[i] = {mb.addConst(i32Lit(static_cast<std::int64_t>(i)), i32), denseCase[i]};
    }
    mb.addSwitch(x, denseCases, sparse);
    for (MirBlockId const b : denseCase) { mb.beginBlock(b); mb.addReturn(x); }

    // Three scattered cases: a compare chain.
    mb.beginBlock(sparse);
    std::array<std::pair<MirInstId, MirBlockId>, kSparse> sparseCases{};
    for (std::size_t i = 0; i < kSparse; ++i) {
        sparseCases[i] = {mb.addConst(i32Lit(static_cast<std::int64_t>(1000 * (i + 1))), i32),
                          sparseCase[i]};
    }
    mb.addSwitch(x, sparseCases, branch);
    for (MirBlockId const b : sparseCase) { mb.beginBlock(b); mb.addReturn(x); }

    // A two-way branch into a block that holds a phi: a split for that edge.
    mb.beginBlock(branch);
    std::array<MirInstId, 2> const cmpOps{x, mb.addConst(i32Lit(0), i32)};
    MirInstId const c = mb.addInst(MirOpcode::ICmpSlt, cmpOps, boolT);
    mb.addCondBr(c, joined, done);
    mb.beginBlock(joined);
    std::array<MirPhiIncoming, 1> const in{MirPhiIncoming{x, branch}};
    mb.addReturn(mb.addPhi(i32, in));
    mb.beginBlock(done);
    mb.addReturn(x);
    mb.beginBlock(landing);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir mir = std::move(mb).finish();
    rederiveStructCfMarkers(mir);
    std::uint32_t const ownBlocks = mir.funcBlockCount(mir.funcAt(0));
    ASSERT_EQ(ownBlocks, 1u + 1u + kDense + 1u + kSparse + 3u + 1u);

    MirSehScope scope;
    scope.parentFuncSymbol    = SymbolId{1};
    scope.beginBlock          = dense;
    scope.endBlock            = done;
    scope.handlerBlock        = landing;
    scope.filterFuncletSymbol = kInnerFunclet;
    scope.personalitySymbol   = kPersonality;
    std::array<MirSehScope, 1> const scopes{scope};
    Lowered const low = lowerWithScopes(mir, ti, scopes);
    ASSERT_TRUE(low.result.ok) << allDiagText(low.rep);
    Lir const& lir = low.result.lir;
    LirFuncId const fn = lir.funcAt(0);
    std::uint32_t const total = lir.funcBlockCount(fn);
    ASSERT_GE(total, ownBlocks + 3u)
        << "the fixture must make each creator create: a table read, a compare chain, a split";
    ASSERT_EQ(low.result.jumpTableDescriptors.size(), 1u) << "the dense switch was not lowered to a table";

    // `dense` is the function's second block, `done` its last but one, the landing
    // its last.
    std::uint32_t const firstOfRegion = lirOf(lir, 0, 1);
    std::uint32_t const lastOfRegion  = lirOf(lir, 0, ownBlocks - 2);
    std::uint32_t const landingBlock  = lirOf(lir, 0, ownBlocks - 1);
    std::vector<Record> const got = recordsOf(low.result);
    std::vector<Record> const want{
        Record{0, firstOfRegion, lastOfRegion, landingBlock, kInnerFunclet.v, kPersonality.v},
        Record{0, lir.funcBlockAt(fn, ownBlocks).v, lir.funcBlockAt(fn, total - 1).v, landingBlock,
               kInnerFunclet.v, kPersonality.v},
    };
    EXPECT_EQ(got, want) << "got:" << shown(got) << "\nwant:" << shown(want);
}

// ── WHY A RUN NEVER WRITES A REGISTER ITS LANDING BLOCK READS ────────────────────
//
// The tier that takes these records (LIR liveness) refuses a record whose blocks
// DEFINE a register the landing block has live in: the fault may come before the
// definition or after it, and the handler would read either. So: can a created block
// be such a block?
//
// THE ONE SHAPE THAT IS, built by hand: a phi ABOVE the region, fed from INSIDE it,
// read by the handler. The copy of that edge writes the phi's register inside the
// region — in a created block when the feeding block has two ways out (its edge is
// split), in the region's own block when it has one. It is the VALUE's property, not
// the created block's: both arms are below, and each is the refusal's to name.
namespace {

struct FedPhi {
    Mir        mir;
    MirBlockId header, body, handler, after;
};

// entry:   n = arg0; br header
// header:  i = phi [0, entry] [next, body]; br body            ← above the region
// body:    next = i + 1; (two ways out: condbr next<n → header, after | one: br header)
// handler: ret i                                               ← reads the phi
// after:   ret next
FedPhi buildFedPhi(TypeInterner& ti, bool twoWaysOut) {
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const boolT = ti.primitive(TypeKind::Bool);
    std::array<TypeId, 1> const params{i32};
    MirBuilder mb;
    mb.stateSelfContainedSymbolIds();
    mb.addFunction(ti.fnSig(params, i32, CallConv::CcSysV), SymbolId{1});
    MirBlockId const entry   = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const body    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const handler = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const after   = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirInstId const n    = mb.addArg(0, i32);
    MirInstId const zero = mb.addConst(i32Lit(0), i32);
    mb.addBr(header);
    mb.beginBlock(header);
    MirInstId const i = mb.addPhi(i32);
    mb.addBr(body);
    mb.beginBlock(body);
    std::array<MirInstId, 2> const addOps{i, mb.addConst(i32Lit(1), i32)};
    MirInstId const next = mb.addInst(MirOpcode::Add, addOps, i32);
    if (twoWaysOut) {
        std::array<MirInstId, 2> const cmpOps{next, n};
        mb.addCondBr(mb.addInst(MirOpcode::ICmpSlt, cmpOps, boolT), header, after);
    } else {
        mb.addBr(header);
    }
    mb.addPhiIncoming(i, MirPhiIncoming{zero, entry});
    mb.addPhiIncoming(i, MirPhiIncoming{next, body});
    mb.beginBlock(handler);
    mb.addReturn(i);
    mb.beginBlock(after);
    mb.addReturn(next);
    Mir mir = std::move(mb).finish();
    rederiveStructCfMarkers(mir);
    return FedPhi{std::move(mir), header, body, handler, after};
}

bool blockWrites(Lir const& lir, LirBlockId b, LirReg reg) {
    for (std::uint32_t k = 0; k < lir.blockInstCount(b); ++k) {
        if (lir.instResult(lir.blockInstAt(b, k)) == reg) return true;
    }
    return false;
}

}  // namespace

TEST(GuardedRegionRuns, OnlyAPhiAboveARegionFedFromInsideItIsWrittenInsideTheRegion) {
    for (bool const twoWaysOut : {true, false}) {
        SCOPED_TRACE(twoWaysOut ? "the feeding block has two ways out: its edge is split"
                                : "the feeding block has one way out: the copy is its own");
        TypeInterner ti{CompilationUnitId{1}};
        FedPhi const f = buildFedPhi(ti, twoWaysOut);
        MirSehScope scope;
        scope.parentFuncSymbol    = SymbolId{1};
        scope.beginBlock          = f.body;
        scope.endBlock            = f.body;
        scope.handlerBlock        = f.handler;
        scope.filterFuncletSymbol = kInnerFunclet;
        scope.personalitySymbol   = kPersonality;
        std::array<MirSehScope, 1> const scopes{scope};
        Lowered const low = lowerWithScopes(f.mir, ti, scopes);
        ASSERT_TRUE(low.result.ok) << allDiagText(low.rep);
        Lir const& lir = low.result.lir;
        LirFuncId const fn = lir.funcAt(0);
        constexpr std::uint32_t kHeader = 1, kBody = 2, kHandler = 3, kOwn = 5;

        // The register the handler reads: its `ret`'s operand, which no instruction
        // of the handler block writes first — the phi's.
        LirBlockId const handler = lir.funcBlockAt(fn, kHandler);
        auto const retOps = lir.instOperands(lir.blockTerminator(handler));
        ASSERT_EQ(retOps.size(), 1u);
        ASSERT_EQ(retOps[0].kind, LirOperandKind::Reg);
        LirReg const phiReg = retOps[0].reg;
        ASSERT_FALSE(blockWrites(lir, handler, phiReg)) << "the handler reads the phi's register as it arrives";

        std::vector<Record> const got = recordsOf(low.result);
        if (twoWaysOut) {
            ASSERT_EQ(lir.funcBlockCount(fn), kOwn + 1u) << "one split: the edge body→header";
            LirBlockId const split = lir.funcBlockAt(fn, kOwn);
            ASSERT_EQ(lir.blockSuccessors(split).size(), 1u);
            ASSERT_EQ(lir.blockSuccessors(split)[0].v, lir.funcBlockAt(fn, kHeader).v);
            ASSERT_EQ(got.size(), 2u) << shown(got);
            EXPECT_EQ(got[1].begin, split.v);
            EXPECT_EQ(got[1].end, split.v);
            EXPECT_TRUE(blockWrites(lir, split, phiReg))
                << "the split of the edge into the phi's block writes the phi's register";
            EXPECT_FALSE(blockWrites(lir, lir.funcBlockAt(fn, kBody), phiReg))
                << "a block with two ways out must not write a phi's register itself";
        } else {
            ASSERT_EQ(lir.funcBlockCount(fn), kOwn) << "one way out: nothing is created";
            ASSERT_EQ(got.size(), 1u) << shown(got);
            EXPECT_TRUE(blockWrites(lir, lir.funcBlockAt(fn, kBody), phiReg))
                << "the region's OWN block writes the phi's register: the shape is the value's";
        }
    }
}

// AND NO SOURCE PROGRAM HAS THAT SHAPE FOR A LOCAL. A phi above a region fed from
// inside it is a local the optimizer promoted across the region — and a function
// that guards a region keeps every local in memory (mem2reg promotes nothing in it:
// the handler must read the value at the fault, not the one at the region's entry).
// The two nearest sources — a loop variable the handler reads, written on one arm of
// a branch inside the guarded body; the output of an `asm goto` in the body, read by
// the handler — are compiled with the SHIPPED release schedule and hold no phi (their
// bodies hold no `&&`, `||`, `?:` or `va_arg`, the four operators the lowering gives
// a phi of its own — the next pin's subject); their handler reads through an
// `alloca`. THE CONTROL is the same loop with no `__try`: there the schedule
// promotes, and the phi is there to be counted.
// (A jump OUT of a guarded body is not among these sources: ✔MEASURED writing this
// pin, a `continue` that leaves one is refused by name today
// (D-CSUBSET-SEH-EARLY-EXIT). The day that row closes, such an edge — a write in
// the body, then a jump to a block above the region — is the third source this pin
// must hold.)
namespace {

struct Toolchain {
    std::shared_ptr<GrammarSchema>      grammar;
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
    std::uint16_t                       ccIndex = 0;
};

std::optional<Toolchain> loadPe64() {
    auto grammarR = GrammarSchema::loadShipped("c");
    auto targetR  = TargetSchema::loadShipped("x86_64");
    auto formatR  = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    EXPECT_TRUE(grammarR.has_value());
    EXPECT_TRUE(targetR.has_value());
    EXPECT_TRUE(formatR.has_value());
    if (!grammarR || !targetR || !formatR) return std::nullopt;
    DiagnosticReporter rep;
    auto const abi = dss::ffi::resolveAbi(**targetR, **formatR, rep);
    EXPECT_TRUE(abi.has_value() && abi->cc != nullptr) << allDiagText(rep);
    if (!abi.has_value() || abi->cc == nullptr) return std::nullopt;
    auto const ccSpan = (*targetR)->callingConventions();
    Toolchain tc{*grammarR, *targetR, *formatR, 0};
    tc.ccIndex = static_cast<std::uint16_t>(std::distance(ccSpan.data(), abi->cc));
    return tc;
}

struct FunctionFacts {
    bool        found = false;
    std::size_t phis = 0;
    std::size_t regions = 0;                 // blocks that end with the region-opening terminator
    std::size_t handlerLoadsOfAnAlloca = 0;  // over every region's handler block
};

// What the unit schedule of `config` leaves of the function `name`.
FunctionFacts factsOf(Toolchain const& tc, CompileConfig config, std::string const& source,
                      std::string_view name) {
    UnitBuilder builder{tc.grammar, DiagnosticBudget::libraryDefault()};
    dss::applySystemDirs(builder, *tc.grammar);
    builder.addInMemory(source, "guarded.c");
    CompilationUnit cu = std::move(builder).finish();
    DiagnosticReporter rep;
    CompileOptions opts{DiagnosticBudget::libraryDefault()};
    opts.config = config;
    auto cuMir = buildCuMir(cu, *tc.grammar, *tc.target, *tc.format, tc.ccIndex, rep, opts);
    EXPECT_TRUE(cuMir.has_value()) << allDiagText(rep);
    FunctionFacts facts;
    if (!cuMir.has_value()) return facts;
    EXPECT_EQ(rep.errorCount(), 0u) << allDiagText(rep);
    Mir const& mir = cuMir->mir;
    for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
        MirFuncId const fn = mir.funcAt(fi);
        SymbolRecord const* const rec = cuMir->model.recordFor(mir.funcSymbol(fn));
        if (rec == nullptr || rec->name != name) continue;
        facts.found = true;
        for (std::uint32_t bi = 0; bi < mir.funcBlockCount(fn); ++bi) {
            MirBlockId const b = mir.funcBlockAt(fn, bi);
            std::uint32_t const n = mir.blockInstCount(b);
            for (std::uint32_t k = 0; k < n; ++k) {
                if (mir.instOpcode(mir.blockInstAt(b, k)) == MirOpcode::Phi) ++facts.phis;
            }
            if (n == 0) continue;
            MirInstId const term = mir.blockInstAt(b, n - 1);
            if (mir.instOpcode(term) == MirOpcode::SehTryBegin) ++facts.regions;
            if (mir.instOpcode(term) != MirOpcode::SehFilterReturn) continue;
            auto const succs = mir.blockSuccessors(b);
            if (succs.empty()) continue;
            MirBlockId const handler = succs[0];
            for (std::uint32_t k = 0; k < mir.blockInstCount(handler); ++k) {
                MirInstId const inst = mir.blockInstAt(handler, k);
                if (mir.instOpcode(inst) != MirOpcode::Load) continue;
                auto const ops = mir.instOperands(inst);
                if (!ops.empty() && mir.instOpcode(ops[0]) == MirOpcode::Alloca) {
                    ++facts.handlerLoadsOfAnAlloca;
                }
            }
        }
    }
    return facts;
}

// `@GUARD@` / `@HANDLE@` open and close the guarded statement; the control replaces
// them so the same statements run unguarded.
std::string withGuard(std::string text, bool guarded) {
    auto const replace = [&](std::string_view key, std::string_view with) {
        for (std::size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + with.size())) {
            text.replace(at, key.size(), with);
        }
    };
    replace("@GUARD@", guarded ? "__try {" : "{");
    replace("@HANDLE@", guarded ? "} __except (1) {" : "} if (n < 0) {");
    return text;
}

}  // namespace

TEST(GuardedRegionRuns, ASourceFunctionThatGuardsARegionHoldsNoPromotedLocalsPhi) {
    auto const tc = loadPe64();
    ASSERT_TRUE(tc.has_value());
    // The loop variables `i` and `r` are written on one arm each of a branch
    // inside the guarded body, and the handler reads `i`: unguarded, each is a phi
    // at the loop's head and another where the arms join.
    std::string const walk =
        "int walk(int *p, int n) {\n"
        "    int r = 0;\n"
        "    int i = 0;\n"
        "    while (i < n) {\n"
        "        @GUARD@\n"
        "            if (p[i] == 7) { i += 2; } else { r += p[i]; }\n"
        "        @HANDLE@\n"
        "            r += i;\n"
        "        }\n"
        "        i += 1;\n"
        "    }\n"
        "    return r;\n"
        "}\n"
        "int main(void) { int cell[4] = {1, 7, 3, 4}; return walk(cell, 4); }\n";
    // The output of an `asm goto` written in the guarded body, read by the handler.
    std::string const capture =
        "int capture(int *p, int n) {\n"
        "    int out = 0;\n"
        "    int rc = 0;\n"
        "    @GUARD@\n"
        "        __asm__ goto (\"movl $7, %0\" : \"=r\"(out) : : \"cc\" : other);\n"
        "        rc += p[n];\n"
        "    other:\n"
        "        rc += 2;\n"
        "    @HANDLE@\n"
        "        rc = out;\n"
        "    }\n"
        "    return rc;\n"
        "}\n"
        "int main(void) { int cell = 1; return capture(&cell, 0); }\n";

    for (CompileConfig const config : {CompileConfig::Debug, CompileConfig::Release}) {
        SCOPED_TRACE(config == CompileConfig::Release ? "release" : "debug");
        FunctionFacts const guardedWalk = factsOf(*tc, config, withGuard(walk, true), "walk");
        ASSERT_TRUE(guardedWalk.found);
        EXPECT_EQ(guardedWalk.regions, 1u) << "the fixture must guard a region";
        EXPECT_EQ(guardedWalk.phis, 0u)
            << "a function that guards a region holds a phi: a local was promoted across the "
               "region, and the copy that feeds it is a write its handler can read";
        EXPECT_GE(guardedWalk.handlerLoadsOfAnAlloca, 1u)
            << "the handler must read the loop variable from its home in the frame";

        FunctionFacts const guardedCapture = factsOf(*tc, config, withGuard(capture, true), "capture");
        ASSERT_TRUE(guardedCapture.found);
        EXPECT_EQ(guardedCapture.regions, 1u);
        EXPECT_EQ(guardedCapture.phis, 0u);
        EXPECT_GE(guardedCapture.handlerLoadsOfAnAlloca, 1u)
            << "the handler must read the `asm goto`'s output from its home in the frame";
    }
    // THE CONTROL: with no region, the release schedule promotes the loop variable.
    FunctionFacts const plainWalk = factsOf(*tc, CompileConfig::Release, withGuard(walk, false), "walk");
    ASSERT_TRUE(plainWalk.found);
    EXPECT_EQ(plainWalk.regions, 0u);
    EXPECT_GE(plainWalk.phis, 1u)
        << "CONTROL: the release schedule must promote the unguarded loop's variables — "
           "otherwise a count of zero above says nothing about the region";
}

// ── WHAT A SOURCE PROGRAM DOES PUT IN A RUN: A SHORT-CIRCUIT'S EDGE SPLIT ─────────
//
// The promotion pass is not the only maker of a phi. The MIR lowering gives `&&` and
// `||` one of its own — `a && b` is `condbr a → [b], join` and a phi at the join —
// and that `condbr` is a two-way predecessor feeding a phi: the edge this tier
// splits, in a block it creates while it lowers a block OF THE GUARDED BODY. So the
// split is in a run of the region, under a record of its own; before runs existed it
// lay past every range the `__try` guarded.
//
// What makes that sound is read here off the real pipeline (source → the unit's MIR
// under the shipped schedule → the funclet synthesis → this tier), for the operator
// as a CONDITION and as a VALUE, `&&` and `||`, debug and release:
//   * the function holds a phi (the premise: without one there is no split to place);
//   * its region has exactly TWO records — its own blocks, then ONE run of ONE
//     created block, with the region's own handler;
//   * that block is the split: its one successor is a block that holds a phi;
//   * the register the split writes for that phi is read by the join and by NO
//     instruction of the handler block — the handler reads the local from its home
//     in the frame, so a fault before or after the split shows it the same value.
// THE CONTROL is the same function with no short-circuit in the body: one record,
// no phi — so the second record above is the operator's and nothing else's.
namespace {

struct LoweredSource {
    bool                       ok = false;
    std::optional<CuMirModule> unit;        // owns the interner the lowered module's types name
    MirToLirResult             lowered;
    std::uint32_t              funcIndex = 0;   // the function's index in the module (MIR and LIR alike)
    std::uint32_t              ownBlocks = 0;   // its MIR blocks: the 1:1 pre-pass keeps their order
    std::vector<std::uint32_t> phiBlocks;       // the positions of its blocks that hold a phi
    std::size_t                regions = 0;
    std::size_t                handlerLoadsOfAnAlloca = 0;
};

// The unit schedule of `config` over `source`, then what the driver's lower half does
// to a guarded module before this tier (the funclet synthesis, which also lays each
// guarded body out contiguously), then this tier — for the function `name`.
LoweredSource lowerSource(Toolchain const& tc, CompileConfig config, std::string const& source,
                          std::string_view name) {
    LoweredSource out;
    UnitBuilder builder{tc.grammar, DiagnosticBudget::libraryDefault()};
    dss::applySystemDirs(builder, *tc.grammar);
    builder.addInMemory(source, "guarded.c");
    CompilationUnit cu = std::move(builder).finish();
    DiagnosticReporter rep;
    CompileOptions opts{DiagnosticBudget::libraryDefault()};
    opts.config = config;
    out.unit = buildCuMir(cu, *tc.grammar, *tc.target, *tc.format, tc.ccIndex, rep, opts);
    EXPECT_TRUE(out.unit.has_value()) << allDiagText(rep);
    if (!out.unit.has_value()) return out;
    EXPECT_EQ(rep.errorCount(), 0u) << allDiagText(rep);
    CuMirModule& unit = *out.unit;
    TypeInterner& interner = unit.model.lattice().interner();

    std::vector<MirSehScope> scopes;
    bool const synthesized = synthesizeSehFunclets(unit.mir, interner, unit.externImports,
                                                   tc.format->sehPersonality(), unit.cSymbolDecoration,
                                                   tc.format->name(), scopes, rep);
    EXPECT_TRUE(synthesized) << allDiagText(rep);
    if (!synthesized) return out;

    Mir const& mir = unit.mir;
    bool found = false;
    for (std::uint32_t fi = 0; fi < mir.moduleFuncCount() && !found; ++fi) {
        MirFuncId const fn = mir.funcAt(fi);
        SymbolRecord const* const rec = unit.model.recordFor(mir.funcSymbol(fn));
        if (rec == nullptr || rec->name != name) continue;
        found = true;
        out.funcIndex = fi;
        out.ownBlocks = mir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < out.ownBlocks; ++bi) {
            MirBlockId const b = mir.funcBlockAt(fn, bi);
            std::uint32_t const n = mir.blockInstCount(b);
            bool holdsPhi = false;
            for (std::uint32_t k = 0; k < n; ++k) {
                if (mir.instOpcode(mir.blockInstAt(b, k)) == MirOpcode::Phi) holdsPhi = true;
            }
            if (holdsPhi) out.phiBlocks.push_back(bi);
            if (n == 0) continue;
            MirInstId const term = mir.blockInstAt(b, n - 1);
            if (mir.instOpcode(term) == MirOpcode::SehTryBegin) ++out.regions;
            if (mir.instOpcode(term) != MirOpcode::SehFilterReturn) continue;
            auto const succs = mir.blockSuccessors(b);
            if (succs.empty()) continue;
            for (std::uint32_t k = 0; k < mir.blockInstCount(succs[0]); ++k) {
                MirInstId const inst = mir.blockInstAt(succs[0], k);
                if (mir.instOpcode(inst) != MirOpcode::Load) continue;
                auto const ops = mir.instOperands(inst);
                if (!ops.empty() && mir.instOpcode(ops[0]) == MirOpcode::Alloca) ++out.handlerLoadsOfAnAlloca;
            }
        }
    }
    EXPECT_TRUE(found) << "the unit must define `" << name << "`";
    if (!found) return out;

    // The funclet synthesis imports the format's personality routine, so the module
    // holds an import and its calls take the call-site form the format declares.
    out.lowered = lowerToLir(mir, *tc.target, interner, rep, unit.externImports,
                             tc.format->externCallDispatch(), std::nullopt, std::nullopt, scopes);
    EXPECT_TRUE(out.lowered.ok) << allDiagText(rep);
    out.ok = out.lowered.ok;
    return out;
}

// The position, in the function's own block order, of the LIR block with id `.v == v`;
// the block count when there is none.
std::uint32_t positionOf(Lir const& lir, LirFuncId fn, std::uint32_t v) {
    for (std::uint32_t k = 0; k < lir.funcBlockCount(fn); ++k) {
        if (lir.funcBlockAt(fn, k).v == v) return k;
    }
    return lir.funcBlockCount(fn);
}

bool blockReads(Lir const& lir, LirBlockId b, LirReg reg) {
    for (std::uint32_t k = 0; k < lir.blockInstCount(b); ++k) {
        for (LirOperand const& op : lir.instOperands(lir.blockInstAt(b, k))) {
            if (op.kind == LirOperandKind::Reg && op.reg == reg) return true;
        }
    }
    return false;
}

std::string guardedUse(std::string_view body) {
    return std::string{"int use(int *p) {\n"
                       "    int t = -1;\n"
                       "    __try {\n"
                       "        "} + std::string{body} + "\n"
           "    } __except (1) {\n"
           "        t += 100;\n"
           "    }\n"
           "    return t;\n"
           "}\n"
           "int main(void) { int cell = 5; return use(&cell); }\n";
}

}  // namespace

TEST(GuardedRegionRuns, AShortCircuitInAGuardedBodyIsSplitInARunItsHandlerNeverReads) {
    auto const tc = loadPe64();
    ASSERT_TRUE(tc.has_value());
    struct Arm {
        char const* label;
        char const* body;
    };
    constexpr std::array<Arm, 4> arms{{
        {"`&&` as a condition", "if (p && *p) t = 1; else t = 2;"},
        {"`||` as a condition", "if (!p || *p) t = 1; else t = 2;"},
        {"`&&` as a value", "t = 10 + (p && *p);"},
        {"`||` as a value", "t = 10 + (!p || *p);"},
    }};
    for (CompileConfig const config : {CompileConfig::Debug, CompileConfig::Release}) {
        for (Arm const& arm : arms) {
            SCOPED_TRACE(std::string{config == CompileConfig::Release ? "release: " : "debug: "} + arm.label);
            LoweredSource const low = lowerSource(*tc, config, guardedUse(arm.body), "use");
            ASSERT_TRUE(low.ok);
            EXPECT_EQ(low.regions, 1u) << "the fixture must guard one region";
            ASSERT_GE(low.phiBlocks.size(), 1u)
                << "THE PREMISE: a short-circuit operator is lowered to a phi with no promotion "
                   "pass involved — with none there is no edge to split and this pin reads nothing";
            EXPECT_GE(low.handlerLoadsOfAnAlloca, 1u)
                << "the handler must read the local from its home in the frame";

            Lir const& lir = low.lowered.lir;
            LirFuncId const fn = lir.funcAt(low.funcIndex);
            std::vector<Record> records;
            for (Record const& r : recordsOf(low.lowered)) {
                if (r.funcIndex == low.funcIndex) records.push_back(r);
            }
            ASSERT_EQ(records.size(), 2u)
                << "a guarded short-circuit is its region's own blocks and ONE run — the split of "
                   "the operator's edge:" << shown(records);
            Record const& body = records[0];
            Record const& run  = records[1];
            EXPECT_LT(positionOf(lir, fn, body.begin), low.ownBlocks);
            EXPECT_LT(positionOf(lir, fn, body.end), low.ownBlocks);
            EXPECT_EQ(run.handler, body.handler) << "a run enters its region's own handler";
            EXPECT_EQ(run.funclet, body.funclet) << "and is filtered by its region's own filter";
            EXPECT_EQ(run.begin, run.end) << "one created block: the split" << shown(records);
            std::uint32_t const splitAt = positionOf(lir, fn, run.begin);
            ASSERT_LT(splitAt, lir.funcBlockCount(fn));
            EXPECT_GE(splitAt, low.ownBlocks)
                << "a run holds blocks this tier CREATED, laid out after the function's own";

            LirBlockId const split = lir.funcBlockAt(fn, splitAt);
            auto const succs = lir.blockSuccessors(split);
            ASSERT_EQ(succs.size(), 1u) << "a split has its edge's one target";
            std::uint32_t const joinAt = positionOf(lir, fn, succs[0].v);
            EXPECT_TRUE(std::find(low.phiBlocks.begin(), low.phiBlocks.end(), joinAt) != low.phiBlocks.end())
                << "the run's block must be the split of an edge INTO A PHI's block (position "
                << joinAt << ")";
            ASSERT_LT(joinAt, lir.funcBlockCount(fn));
            LirBlockId const join    = lir.funcBlockAt(fn, joinAt);
            LirBlockId const handler = lir.funcBlockAt(fn, positionOf(lir, fn, run.handler));

            // The phi's register: what the split writes and the join reads.
            std::size_t phiRegisters = 0;
            for (std::uint32_t k = 0; k < lir.blockInstCount(split); ++k) {
                LirReg const written = lir.instResult(lir.blockInstAt(split, k));
                if (!written.valid() || !blockReads(lir, join, written)) continue;
                ++phiRegisters;
                EXPECT_FALSE(blockReads(lir, handler, written))
                    << "the handler reads a register the run writes: a fault before the split and "
                       "one after it would show the handler two different values";
            }
            EXPECT_GE(phiRegisters, 1u)
                << "the split must write, for the join, the register the phi became";
        }
    }
    // THE CONTROL: the same guarded function with no short-circuit in its body.
    for (CompileConfig const config : {CompileConfig::Debug, CompileConfig::Release}) {
        SCOPED_TRACE(config == CompileConfig::Release ? "release: the control" : "debug: the control");
        LoweredSource const low = lowerSource(*tc, config, guardedUse("t = 50 + *p;"), "use");
        ASSERT_TRUE(low.ok);
        EXPECT_EQ(low.regions, 1u);
        EXPECT_TRUE(low.phiBlocks.empty())
            << "CONTROL: a guarded body with no short-circuit holds no phi (no local is promoted)";
        std::size_t records = 0;
        for (Record const& r : recordsOf(low.lowered)) {
            if (r.funcIndex == low.funcIndex) ++records;
        }
        EXPECT_EQ(records, 1u) << "CONTROL: with nothing created for its blocks, a region has one record";
    }
}
