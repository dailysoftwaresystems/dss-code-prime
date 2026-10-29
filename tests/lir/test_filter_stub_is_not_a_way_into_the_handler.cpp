// THE BLOCK LEFT WHERE A `__try` FILTER STOOD LEADS NOWHERE.
//
// A filter becomes a function of its own; the function that holds the `__try`
// keeps ONE block in its place, a stub `[Const ; SehFilterReturn → handler]`
// that no instruction ever reaches — the guarded body jumps over it, and the
// handler is entered by the unwinder. MIR→LIR used to seal that stub with
// `jmp handler` "so the handler keeps a predecessor". Nothing branches to the
// stub, so the block order liveness computes appends it LAST, and whatever is
// live into the handler was then live OUT of a block at the function's end:
// every value a handler reads held its register across everything laid out
// after the handler. ✔MEASURED 2026-10-12 on pe64, both pipelines: a function
// whose handler reads a parameter and which then adds two call results saved
// FOUR non-volatile registers in a 120-byte frame; the same function with a
// handler that reads nothing saved three in 104.
//
// The stub is sealed as what it is — a block control does not leave. This pins
// the lowering (the stub has no successor; the handler has no NORMAL predecessor
// at all) and what liveness then says: nothing is live into the stub, while the
// handler still has what it reads, through the guarded run's exceptional edge.
//
// A binary of its own: the lowering's and the liveness tier's binaries are each
// the claim runner of many mutation arms, whose case counts a new case moves.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/core_type.hpp"   // TypeKind, CallConv
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "lir/lir_guarded_regions.hpp"   // guardedRegionsAt
#include "lir/lir_liveness.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/merge/synth_seh_funclets.hpp"   // MirSehScope
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::string allDiagText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) { out += '['; out += d.actual; out += ']'; }
    return out;
}

struct Lowered {
    MirToLirResult     result;
    DiagnosticReporter rep;
};

// `i32 f(i32 a)`, as the funclet pass leaves the function that holds a `__try`:
//   entry   : a = Arg 0 ; slot = alloca ; SehTryBegin(0) → [body, stub]
//   body    : load slot ; SehTryEnd(0) ; Br(join)
//   stub    : Const 0 ; SehFilterReturn(0) → handler
//   handler : store a → slot ; Br(join)        — reads `a`, born before the region
//   join    : return (load slot)
[[nodiscard]] Lowered lowerAFunctionWithAStub(TargetSchema const& target,
                                              TypeInterner& interner) {
    TypeId const i32  = interner.primitive(TypeKind::I32);
    TypeId const pI32 = interner.pointer(i32);
    TypeId const params[] = {i32};
    MirBuilder mb;
    mb.stateSelfContainedSymbolIds();
    mb.addFunction(interner.fnSig(params, i32, CallConv::CcSysV), SymbolId{100});
    MirBlockId const entry   = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const body    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const stub    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const handler = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const join    = mb.createBlock(StructCfMarker::Linear);

    mb.beginBlock(entry);
    MirInstId const a    = mb.addArg(0, i32);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, pI32, 4);
    mb.addSehTryBegin(body, stub, 0);

    mb.beginBlock(body);
    (void)mb.addInst(MirOpcode::Load, std::array<MirInstId, 1>{slot}, i32);
    mb.addInst(MirOpcode::SehTryEnd, {}, InvalidType, 0);
    mb.addBr(join);

    mb.beginBlock(stub);
    MirLiteralValue zero;
    zero.value = std::int64_t{0};
    zero.core  = TypeKind::I32;
    mb.addSehFilterReturn(mb.addConst(std::move(zero), i32), handler, 0);

    mb.beginBlock(handler);
    (void)mb.addInst(MirOpcode::Store, std::array<MirInstId, 2>{a, slot}, InvalidType);
    mb.addBr(join);

    mb.beginBlock(join);
    mb.addReturn(mb.addInst(MirOpcode::Load, std::array<MirInstId, 1>{slot}, i32));
    Mir mir = std::move(mb).finish();

    MirSehScope scope;
    scope.parentFuncSymbol    = SymbolId{100};
    scope.beginBlock          = body;
    scope.endBlock            = body;
    scope.handlerBlock        = handler;
    scope.filterFuncletSymbol = SymbolId{501};
    scope.personalitySymbol   = SymbolId{500};
    std::array<MirSehScope, 1> const scopes{scope};
    Lowered out;
    std::vector<ExternImport> noExterns;
    out.result = lowerToLir(mir, target, interner, out.rep, noExterns,
                            ExternCallDispatch::DirectPlt, std::nullopt, std::nullopt,
                            scopes);
    return out;
}

[[nodiscard]] bool holdsNothing(VRegBitset const& set) {
    for (std::uint64_t const word : set.bits) {
        if (word != 0) return false;
    }
    return true;
}

} // namespace

// On each shipped target: the stub's block has NO successor, the handler has no
// normal predecessor, and liveness — handed the guarded run — finds nothing live
// into the stub while the handler still has the value it reads.
TEST(FilterStub, TheBlockLeftWhereAFilterStoodLeadsNowhere) {
    for (char const* targetName : {"x86_64", "arm64"}) {
        SCOPED_TRACE(targetName);
        auto target = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(target.has_value());
        TypeInterner interner{CompilationUnitId{1}};
        Lowered L = lowerAFunctionWithAStub(**target, interner);
        ASSERT_TRUE(L.result.ok) << allDiagText(L.rep);
        ASSERT_EQ(L.result.sehScopeDescriptors.size(), 1u) << allDiagText(L.rep);
        Lir const& lir = L.result.lir;
        ASSERT_EQ(lir.moduleFuncCount(), 1u);
        LirFuncId const fn = lir.funcAt(0);
        std::uint32_t const handlerV = L.result.sehScopeDescriptors[0].handlerLirBlockV;

        // Who branches to each block, by the blocks' own successor lists.
        std::unordered_map<std::uint32_t, std::uint32_t> branchedToBy;
        std::uint32_t const nb = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            for (LirBlockId const s : lir.blockSuccessors(lir.funcBlockAt(fn, bi))) {
                ++branchedToBy[s.v];
            }
        }
        // The stub is the one block that is neither the entry nor the handler
        // and that nothing branches to.
        std::vector<LirBlockId> unreached;
        for (std::uint32_t bi = 1; bi < nb; ++bi) {
            LirBlockId const b = lir.funcBlockAt(fn, bi);
            if (b.v != handlerV && branchedToBy[b.v] == 0) unreached.push_back(b);
        }
        ASSERT_EQ(unreached.size(), 1u)
            << "the premise: exactly one block of the lowered function besides the "
               "handler has no block branching to it — the stub";
        LirBlockId const stub = unreached.front();

        EXPECT_TRUE(lir.blockSuccessors(stub).empty())
            << "the stub branches to " << lir.blockSuccessors(stub).size()
            << " block(s): control never leaves the block left where a filter "
               "stood, and a successor on it carries what is live into that block "
               "to the end of the function";
        EXPECT_EQ(branchedToBy[handlerV], 0u)
            << "the handler has a NORMAL predecessor: its way in is the unwinder's, "
               "which liveness reads off the guarded run";

        DiagnosticReporter rep;
        auto const regions = guardedRegionsAt(std::span<LirBlockRebuild const>{},
                                              L.result.sehScopeDescriptors, rep);
        ASSERT_TRUE(regions.has_value()) << allDiagText(rep);
        auto const lv = analyzeLiveness(lir, *regions, rep);
        ASSERT_TRUE(lv.has_value()) << allDiagText(rep);
        LirFuncLiveness const& flow = lv->perFunc[0];
        std::size_t stubAt = flow.blockOrder.size(), handlerAt = flow.blockOrder.size();
        for (std::size_t i = 0; i < flow.blockOrder.size(); ++i) {
            if (flow.blockOrder[i].v == stub.v) stubAt = i;
            if (flow.blockOrder[i].v == handlerV) handlerAt = i;
        }
        ASSERT_LT(stubAt, flow.blockOrder.size());
        ASSERT_LT(handlerAt, flow.blockOrder.size());
        EXPECT_FALSE(holdsNothing(flow.liveIn[handlerAt]))
            << "the premise: the handler reads a value born before the region, and "
               "the guarded run's exceptional edge carries it there";
        EXPECT_TRUE(holdsNothing(flow.liveIn[stubAt]))
            << "a value is live into the stub: it is then held to wherever the stub "
               "is laid out, which is the end of the function";
        EXPECT_LT(handlerAt, stubAt)
            << "the handler is ordered before the stub — the stub is what nothing "
               "reaches, so it comes last";
    }
}

// THE CONTROL, which the seal of the stub does not move. Handed the guarded run,
// liveness carries the value the handler reads INTO the handler, on each shipped
// target, whatever the stub is sealed with: the handler's way in is the guarded
// run's exceptional edge. It is the premise of the claim above, held as a case of
// its own — a change that took the handler's value away would be named here, and
// one that only gave the stub a successor is not.
TEST(FilterStub, TheHandlerHasWhatItReadsByTheGuardedRunsEdge) {
    for (char const* targetName : {"x86_64", "arm64"}) {
        SCOPED_TRACE(targetName);
        auto target = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(target.has_value());
        TypeInterner interner{CompilationUnitId{1}};
        Lowered L = lowerAFunctionWithAStub(**target, interner);
        ASSERT_TRUE(L.result.ok) << allDiagText(L.rep);
        ASSERT_EQ(L.result.sehScopeDescriptors.size(), 1u) << allDiagText(L.rep);
        std::uint32_t const handlerV = L.result.sehScopeDescriptors[0].handlerLirBlockV;

        DiagnosticReporter rep;
        auto const regions = guardedRegionsAt(std::span<LirBlockRebuild const>{},
                                              L.result.sehScopeDescriptors, rep);
        ASSERT_TRUE(regions.has_value()) << allDiagText(rep);
        auto const lv = analyzeLiveness(L.result.lir, *regions, rep);
        ASSERT_TRUE(lv.has_value()) << allDiagText(rep);
        LirFuncLiveness const& flow = lv->perFunc[0];
        std::size_t handlerAt = flow.blockOrder.size();
        for (std::size_t i = 0; i < flow.blockOrder.size(); ++i) {
            if (flow.blockOrder[i].v == handlerV) handlerAt = i;
        }
        ASSERT_LT(handlerAt, flow.blockOrder.size());
        EXPECT_FALSE(holdsNothing(flow.liveIn[handlerAt]))
            << "the handler reads a value born before the region, and the guarded "
               "run's exceptional edge carries it into the handler";
    }
}
