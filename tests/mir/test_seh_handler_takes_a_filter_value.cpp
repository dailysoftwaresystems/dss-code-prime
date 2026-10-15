// A HANDLER THAT NAMES A VALUE ITS FILTER COMPUTED.
//
// `__except (g == 5) { rc = g + 37; }` — the source shares nothing between the
// filter and the handler, and the MIR does: the filter's entry dominates the
// handler (the handler's one predecessor is the filter's exit), so a pass that
// reuses a dominating computation gives the handler the FILTER's `GlobalAddr g`.
// The filter becomes a function of its own and none of its instructions stay in
// the parent. ✔MEASURED 2026-10-12, pe64, release, on that program: the rebuild of
// the parent ABORTED on the handler's operand (`rewriteOperand: … has no rewrite
// entry`), where the reference compiler runs it and the baseline — which reuses
// nothing — compiled.
//
// `synthesizeSehFunclets` now reads what every block the parent keeps names of a
// filter, and the parent computes those values AGAIN at the head of the handler —
// the first code of the parent that runs after the filter. What it cannot compute
// again (a read of memory, the exception, a call's result at the moment the filter
// ran) is refused by name. No C program reaches that refusal — the passes that
// reuse a computation reuse none of those across a filter's end — so it is pinned
// here, on hand-built MIR, and only here.
//
// ═══ WHY EVERY CASE RUNS THE PASS IN A CHILD FIRST ════════════════════════════
//
// The failure this binary exists for is an ABORT of the process. A case that ran
// the pass in-process would take the whole binary down with it, and no case could
// be named as the one that caught it. So each case first requires a CHILD that
// builds the same module and runs the pass to leave with 0, and looks at the
// result in-process only after that held.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/object_format_kind.hpp"   // SehPersonality, RuntimeLibraryRole
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"   // TypeKind, CallConv
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/merge/synth_seh_funclets.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"   // rederiveStructCfMarkers
#include "mir/mir_verifier.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace dss;

namespace {

constexpr std::uint32_t kParentSym = 100;
constexpr std::uint32_t kGlobalSym = 10;    // `g`, a global another unit defines

MirLiteralValue lit(std::int64_t v, TypeKind core) {
    MirLiteralValue l;
    l.value = v;
    l.core  = core;
    return l;
}

[[nodiscard]] std::optional<MirFuncId> findFuncBySymbol(Mir const& mir, SymbolId sym) {
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v == sym.v) return f;
    }
    return std::nullopt;
}

[[nodiscard]] std::string allDiagText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) { out += '['; out += d.actual; out += ']'; }
    return out;
}

SehPersonality peSehPersonality() {
    SehPersonality p;
    p.role        = RuntimeLibraryRole::UnwindPersonality;
    p.libraryPath = "ucrtbase.dll";
    p.mangledName = "__C_specific_handler";
    return p;
}

[[nodiscard]] std::size_t countIn(Mir const& mir, MirFuncId fn, MirOpcode op) {
    std::size_t n = 0;
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(fn); ++bi) {
        MirBlockId const b = mir.funcBlockAt(fn, bi);
        for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
            if (mir.instOpcode(mir.blockInstAt(b, ii)) == op) ++n;
        }
    }
    return n;
}

// The types and the builder a case writes with.
struct Ctx {
    MirBuilder& mb;
    TypeId      i32, u32, u64, pI32;

    [[nodiscard]] MirInstId k(std::int64_t v) const { return mb.addConst(lit(v, TypeKind::I32), i32); }
    [[nodiscard]] MirInstId k64(std::int64_t v) const { return mb.addConst(lit(v, TypeKind::U64), u64); }
    [[nodiscard]] MirInstId addressOfG() const {
        return mb.addGlobalAddr(SymbolId{kGlobalSym}, pI32);
    }
    [[nodiscard]] MirInstId load(MirInstId p) const {
        return mb.addInst(MirOpcode::Load, std::array<MirInstId, 1>{p}, i32);
    }
    [[nodiscard]] MirInstId two(MirOpcode op, MirInstId a, MirInstId b, TypeId t) const {
        return mb.addInst(op, std::array<MirInstId, 2>{a, b}, t);
    }
    [[nodiscard]] MirInstId one(MirOpcode op, MirInstId a, TypeId t) const {
        return mb.addInst(op, std::array<MirInstId, 1>{a}, t);
    }
};

// ── One region ──────────────────────────────────────────────────────────────
//   entry   : slot = alloca ; before = 3 + 4 ; SehTryBegin(0) → [body, filter]
//   body    : load slot ; SehTryEnd(0) ; Br(join)
//   filter  : what the case writes — it returns the filter's value ;
//             SehFilterReturn(0) → handler
//   handler : what the case writes ; Br(join)
//   join    : return 0
// `before` is a value of the parent born before the region.
template <typename EmitFilter, typename EmitHandler>
[[nodiscard]] Mir buildOneRegion(TypeInterner& in, EmitFilter emitFilter,
                                 EmitHandler emitHandler) {
    TypeId const i32  = in.primitive(TypeKind::I32);
    TypeId const u32  = in.primitive(TypeKind::U32);
    TypeId const u64  = in.primitive(TypeKind::U64);
    TypeId const pI32 = in.pointer(i32);
    MirBuilder mb;
    mb.stateSelfContainedSymbolIds();   // a hand-built module is its own table
    mb.addFunction(in.fnSig({}, i32, CallConv::CcMS64), SymbolId{kParentSym});
    MirBlockId const entry   = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const body    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const filter  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const handler = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const join    = mb.createBlock(StructCfMarker::Linear);
    Ctx c{mb, i32, u32, u64, pI32};

    mb.beginBlock(entry);
    MirInstId const slot   = mb.addInst(MirOpcode::Alloca, {}, pI32, 4);
    MirInstId const before = c.two(MirOpcode::Add, c.k(3), c.k(4), i32);
    mb.addSehTryBegin(body, filter, 0);

    mb.beginBlock(body);
    (void)c.load(slot);
    mb.addInst(MirOpcode::SehTryEnd, {}, InvalidType, 0);
    mb.addBr(join);

    mb.beginBlock(filter);
    mb.addSehFilterReturn(emitFilter(c, before), handler, 0);

    mb.beginBlock(handler);
    emitHandler(c);
    mb.addBr(join);

    mb.beginBlock(join);
    mb.addReturn(c.k(0));
    Mir mir = std::move(mb).finish();
    rederiveStructCfMarkers(mir);
    return mir;
}

struct Synthesized {
    bool                      ok = false;
    std::vector<MirSehScope>  scopes;
    std::string               text;
};

[[nodiscard]] Synthesized synthesize(Mir& mir, TypeInterner& in) {
    Synthesized s;
    std::vector<ExternImport> ext;
    DiagnosticReporter        rep;
    s.ok = synthesizeSehFunclets(mir, in, ext, peSehPersonality(),
                                 CSymbolDecorationScheme::None,
                                 "pe64-x86_64-windows-exec", s.scopes, rep);
    s.text = allDiagText(rep);
    return s;
}

// Builds the module, runs the pass and LEAVES THE PROCESS: 0 when the pass
// finished and accepted the module, 3 when it refused it. A pass that aborts
// leaves a third way, which is what the caller's `EXPECT_EXIT` reads.
template <typename Build>
[[noreturn]] void synthesizeAndLeave(Build build) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = build(in);
    std::exit(synthesize(mir, in).ok ? 0 : 3);
}

// The handler of each region of the rebuilt parent, by region id: the one
// successor of the block that ends in that region's `SehFilterReturn` (the stub
// standing where the filter stood).
[[nodiscard]] std::unordered_map<std::uint32_t, MirBlockId>
handlersByRegion(Mir const& mir, MirFuncId parent) {
    std::unordered_map<std::uint32_t, MirBlockId> out;
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(parent); ++bi) {
        MirBlockId const b = mir.funcBlockAt(parent, bi);
        MirInstId const term = mir.blockTerminator(b);
        if (mir.instOpcode(term) != MirOpcode::SehFilterReturn) continue;
        auto const succs = mir.blockSuccessors(b);
        if (succs.size() == 1) out.emplace(mir.instPayload(term), succs[0]);
    }
    return out;
}

// Every operand of every instruction of `b` is defined EARLIER IN `b` or in a
// block of `allowedElsewhere` — what "the handler computes it again" means for
// the code that follows. Reports the first operand that is not.
void expectOperandsDefinedBeforeUse(Mir const& mir, MirBlockId b,
                                    std::vector<MirBlockId> const& allowedElsewhere) {
    std::unordered_map<std::uint32_t, std::uint32_t> position;
    for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
        MirInstId const id = mir.blockInstAt(b, ii);
        for (MirInstId const o : mir.instOperands(id)) {
            MirBlockId const where = mir.instBlock(o);
            if (where.v == b.v) {
                auto const at = position.find(o.v);
                EXPECT_TRUE(at != position.end() && at->second < ii)
                    << "instruction " << ii << " of the handler names a value of the "
                       "handler defined at or after it";
                continue;
            }
            bool allowed = false;
            for (MirBlockId const a : allowedElsewhere) allowed = allowed || a.v == where.v;
            EXPECT_TRUE(allowed)
                << "instruction " << ii << " of the handler ('"
                << opcodeInfo(mir.instOpcode(id)).mnemonic << "') names a '"
                << opcodeInfo(mir.instOpcode(o)).mnemonic << "' of block #" << where.v
                << ", which is neither the handler nor a block that runs before it";
        }
        position.emplace(id.v, ii);
    }
}

// `g == 5`, the filter of every positive case: returns the comparison and hands
// back the address it loaded through.
struct FilterOfG {
    MirInstId address;
    MirInstId verdict;
};
[[nodiscard]] FilterOfG filterReadsG(Ctx const& c) {
    MirInstId const address = c.addressOfG();
    MirInstId const verdict = c.two(MirOpcode::ICmpEq, c.load(address), c.k(5), c.i32);
    return FilterOfG{address, verdict};
}

} // namespace

// `__except (g == 5) { … g … }` with the handler loading through the FILTER's
// address. The parent computes the address again as the handler's first
// instruction and loads through that; the funclet keeps its own.
TEST(SehHandlerTakesAFilterValue, TheAddressItsFilterNamedIsComputedAgainAtTheHandlersHead) {
    auto const build = [](TypeInterner& in) {
        MirInstId shared{};
        return buildOneRegion(
            in,
            [&](Ctx& c, MirInstId) {
                FilterOfG const f = filterReadsG(c);
                shared = f.address;
                return f.verdict;
            },
            [&](Ctx& c) { (void)c.load(shared); });
    };
    EXPECT_EXIT(synthesizeAndLeave(build), ::testing::ExitedWithCode(0), "")
        << "the pass did not finish on a handler that names the address its filter "
           "named: a child killed by an abort is the rebuild of the parent meeting "
           "an operand whose instruction went to the funclet";
    if (HasFailure()) return;

    TypeInterner in{CompilationUnitId{1}};
    Mir mir = build(in);
    Synthesized const s = synthesize(mir, in);
    ASSERT_TRUE(s.ok) << s.text;
    ASSERT_EQ(s.scopes.size(), 1u);
    auto const parent = findFuncBySymbol(mir, SymbolId{kParentSym});
    ASSERT_TRUE(parent.has_value());
    auto const handlers = handlersByRegion(mir, *parent);
    ASSERT_EQ(handlers.size(), 1u);
    MirBlockId const handler = handlers.at(0);

    ASSERT_EQ(mir.blockInstCount(handler), 3u) << "the address, the load, the branch";
    MirInstId const address = mir.blockInstAt(handler, 0);
    MirInstId const loaded  = mir.blockInstAt(handler, 1);
    ASSERT_EQ(mir.instOpcode(address), MirOpcode::GlobalAddr)
        << "the handler's first instruction computes the address again";
    EXPECT_EQ(mir.globalAddrSymbol(address).v, kGlobalSym);
    ASSERT_EQ(mir.instOpcode(loaded), MirOpcode::Load);
    EXPECT_EQ(mir.instOperands(loaded)[0].v, address.v)
        << "the handler loads through the address IT computed";
    EXPECT_EQ(countIn(mir, *parent, MirOpcode::GlobalAddr), 1u)
        << "the filter's own address is not the parent's";

    auto const funclet = findFuncBySymbol(mir, s.scopes[0].filterFuncletSymbol);
    ASSERT_TRUE(funclet.has_value());
    EXPECT_EQ(countIn(mir, *funclet, MirOpcode::GlobalAddr), 1u)
        << "the funclet computes the address for itself";

    DiagnosticReporter rep;
    EXPECT_TRUE((MirVerifier{mir, &in}.verify(rep))) << allDiagText(rep);
}

// THE CONTROL: a handler that names nothing of its filter is copied as it stands —
// nothing is made at its head.
TEST(SehHandlerTakesAFilterValue, AHandlerThatNamesNothingOfItsFilterIsCopiedAsItStands) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildOneRegion(
        in, [](Ctx& c, MirInstId) { return filterReadsG(c).verdict; },
        [](Ctx& c) { (void)c.load(c.addressOfG()); });
    Synthesized const s = synthesize(mir, in);
    ASSERT_TRUE(s.ok) << s.text;
    auto const parent = findFuncBySymbol(mir, SymbolId{kParentSym});
    ASSERT_TRUE(parent.has_value());
    auto const handlers = handlersByRegion(mir, *parent);
    ASSERT_EQ(handlers.size(), 1u);
    EXPECT_EQ(mir.blockInstCount(handlers.at(0)), 3u)
        << "its own address, its load, its branch — and nothing before them";
    EXPECT_EQ(countIn(mir, *parent, MirOpcode::GlobalAddr), 1u);
}

// The handler names `g`'s second element — an address the filter COMPUTED from
// `g`'s — and a sum the filter made from a value born before the region. Each
// brings its whole computation, operands first; the value born before the region
// is the parent's already and is named, not made.
TEST(SehHandlerTakesAFilterValue, AValueTheFilterComputedBringsItsWholeComputation) {
    auto const build = [](TypeInterner& in) {
        MirInstId element{};
        MirInstId sum{};
        return buildOneRegion(
            in,
            [&](Ctx& c, MirInstId before) {
                // &g[2], spelled as integer arithmetic on the address.
                MirInstId const asInt = c.one(MirOpcode::PtrToInt, c.addressOfG(), c.u64);
                MirInstId const moved = c.two(MirOpcode::Add, asInt, c.k64(8), c.u64);
                element               = c.one(MirOpcode::IntToPtr, moved, c.pI32);
                sum                   = c.two(MirOpcode::Add, before, c.k(1), c.i32);
                MirInstId const both  = c.two(MirOpcode::Add, c.load(element), sum, c.i32);
                return c.two(MirOpcode::ICmpEq, both, c.k(48), c.i32);
            },
            [&](Ctx& c) { (void)c.two(MirOpcode::Add, c.load(element), sum, c.i32); });
    };
    EXPECT_EXIT(synthesizeAndLeave(build), ::testing::ExitedWithCode(0), "")
        << "the pass did not finish on a handler that names values its filter "
           "computed from other values";
    if (HasFailure()) return;

    TypeInterner in{CompilationUnitId{1}};
    Mir mir = build(in);
    Synthesized const s = synthesize(mir, in);
    ASSERT_TRUE(s.ok) << s.text;
    auto const parent = findFuncBySymbol(mir, SymbolId{kParentSym});
    ASSERT_TRUE(parent.has_value());
    auto const handlers = handlersByRegion(mir, *parent);
    ASSERT_EQ(handlers.size(), 1u);
    MirBlockId const handler = handlers.at(0);
    MirBlockId const entry   = mir.funcBlockAt(*parent, 0);

    // Made: the address, its integer form, the 8, their sum, the pointer — and the
    // 1 and the sum over `before`. Then the handler's own load, add and branch.
    EXPECT_EQ(mir.blockInstCount(handler), 10u);
    expectOperandsDefinedBeforeUse(mir, handler, {entry});

    // The handler's own two instructions are its last before the branch, and they
    // name what the handler made.
    MirInstId const own  = mir.blockInstAt(handler, 8);
    MirInstId const load = mir.blockInstAt(handler, 7);
    ASSERT_EQ(mir.instOpcode(load), MirOpcode::Load);
    ASSERT_EQ(mir.instOpcode(own), MirOpcode::Add);
    MirInstId const elementNew = mir.instOperands(load)[0];
    ASSERT_EQ(mir.instOpcode(elementNew), MirOpcode::IntToPtr);
    EXPECT_EQ(mir.instBlock(elementNew).v, handler.v);
    MirInstId const movedNew = mir.instOperands(elementNew)[0];
    ASSERT_EQ(mir.instOpcode(movedNew), MirOpcode::Add);
    MirInstId const asIntNew = mir.instOperands(movedNew)[0];
    ASSERT_EQ(mir.instOpcode(asIntNew), MirOpcode::PtrToInt);
    MirInstId const addressNew = mir.instOperands(asIntNew)[0];
    ASSERT_EQ(mir.instOpcode(addressNew), MirOpcode::GlobalAddr);
    EXPECT_EQ(mir.instBlock(addressNew).v, handler.v);

    MirInstId const sumNew = mir.instOperands(own)[1];
    ASSERT_EQ(mir.instOpcode(sumNew), MirOpcode::Add);
    EXPECT_EQ(mir.instBlock(sumNew).v, handler.v) << "the sum is made in the handler";
    MirInstId const beforeNew = mir.instOperands(sumNew)[0];
    EXPECT_EQ(mir.instOpcode(beforeNew), MirOpcode::Add);
    EXPECT_EQ(mir.instBlock(beforeNew).v, entry.v)
        << "the value born before the region is named where the parent has it, "
           "not made a second time";

    DiagnosticReporter rep;
    EXPECT_TRUE((MirVerifier{mir, &in}.verify(rep))) << allDiagText(rep);
}

namespace {

// ── Two regions, the second inside the first one's handler ──────────────────
//   entry    : slot = alloca ; SehTryBegin(0) → [body0, filter0]
//   body0    : load slot ; SehTryEnd(0) ; Br(join)
//   filter0  : a = &g ; (load a == 5) ; SehFilterReturn(0) → handler0
//   handler0 : SehTryBegin(1) → [body1, filter1]
//   body1    : load slot ; SehTryEnd(1) ; Br(after)
//   filter1  : n = (u64)a ; (n != 0) ; SehFilterReturn(1) → handler1
//   handler1 : n + n ; Br(after)
//   after    : Br(join)
//   join     : return 0
// The inner filter's `n` is computed from the OUTER filter's `a`, and the inner
// handler names `n`.
[[nodiscard]] Mir buildInnerHandlerNamesTheOuterFiltersValue(TypeInterner& in) {
    TypeId const i32  = in.primitive(TypeKind::I32);
    TypeId const u32  = in.primitive(TypeKind::U32);
    TypeId const u64  = in.primitive(TypeKind::U64);
    TypeId const pI32 = in.pointer(i32);
    MirBuilder mb;
    mb.stateSelfContainedSymbolIds();
    mb.addFunction(in.fnSig({}, i32, CallConv::CcMS64), SymbolId{kParentSym});
    MirBlockId const entry    = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const body0    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const filter0  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const handler0 = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const body1    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const filter1  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const handler1 = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const after    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const join     = mb.createBlock(StructCfMarker::Linear);
    Ctx c{mb, i32, u32, u64, pI32};

    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, pI32, 4);
    mb.addSehTryBegin(body0, filter0, 0);

    mb.beginBlock(body0);
    (void)c.load(slot);
    mb.addInst(MirOpcode::SehTryEnd, {}, InvalidType, 0);
    mb.addBr(join);

    mb.beginBlock(filter0);
    FilterOfG const outer = filterReadsG(c);
    mb.addSehFilterReturn(outer.verdict, handler0, 0);

    mb.beginBlock(handler0);
    mb.addSehTryBegin(body1, filter1, 1);

    mb.beginBlock(body1);
    (void)c.load(slot);
    mb.addInst(MirOpcode::SehTryEnd, {}, InvalidType, 1);
    mb.addBr(after);

    mb.beginBlock(filter1);
    MirInstId const n = c.one(MirOpcode::PtrToInt, outer.address, u64);
    mb.addSehFilterReturn(c.two(MirOpcode::ICmpNe, n, c.k64(0), i32), handler1, 1);

    mb.beginBlock(handler1);
    (void)c.two(MirOpcode::Add, n, n, u64);
    mb.addBr(after);

    mb.beginBlock(after);
    mb.addBr(join);

    mb.beginBlock(join);
    mb.addReturn(c.k(0));
    Mir mir = std::move(mb).finish();
    rederiveStructCfMarkers(mir);
    return mir;
}

} // namespace

// A region inside a handler: the inner handler names a value of the INNER filter
// that is computed from a value of the OUTER one. Each filter's value is made at
// the head of ITS handler — the outer handler stands before the inner one — and
// the inner one names the outer's.
TEST(SehHandlerTakesAFilterValue, AValueOfTheFilterAroundIsMadeAtThatFiltersHandler) {
    EXPECT_EXIT(synthesizeAndLeave(&buildInnerHandlerNamesTheOuterFiltersValue),
                ::testing::ExitedWithCode(0), "")
        << "the pass did not finish on a region inside a handler whose own handler "
           "names a value computed from the outer filter's";
    if (HasFailure()) return;

    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildInnerHandlerNamesTheOuterFiltersValue(in);
    Synthesized const s = synthesize(mir, in);
    ASSERT_TRUE(s.ok) << s.text;
    ASSERT_EQ(s.scopes.size(), 2u);
    auto const parent = findFuncBySymbol(mir, SymbolId{kParentSym});
    ASSERT_TRUE(parent.has_value());
    auto const handlers = handlersByRegion(mir, *parent);
    ASSERT_EQ(handlers.size(), 2u);
    MirBlockId const outer = handlers.at(0);
    MirBlockId const inner = handlers.at(1);

    ASSERT_EQ(mir.blockInstCount(outer), 2u) << "the address, then the inner region's opener";
    MirInstId const address = mir.blockInstAt(outer, 0);
    ASSERT_EQ(mir.instOpcode(address), MirOpcode::GlobalAddr)
        << "the outer filter's address is made at the OUTER handler's head";
    EXPECT_EQ(mir.instOpcode(mir.blockTerminator(outer)), MirOpcode::SehTryBegin);

    ASSERT_EQ(mir.blockInstCount(inner), 3u) << "the integer form, the handler's sum, the branch";
    MirInstId const n = mir.blockInstAt(inner, 0);
    ASSERT_EQ(mir.instOpcode(n), MirOpcode::PtrToInt)
        << "the inner filter's value is made at the INNER handler's head";
    EXPECT_EQ(mir.instOperands(n)[0].v, address.v)
        << "and it is computed from the address the outer handler made";
    EXPECT_EQ(countIn(mir, *parent, MirOpcode::GlobalAddr), 1u);
    EXPECT_EQ(countIn(mir, *parent, MirOpcode::PtrToInt), 1u);

    // Each funclet computes what it names for itself.
    for (MirSehScope const& scope : s.scopes) {
        auto const funclet = findFuncBySymbol(mir, scope.filterFuncletSymbol);
        ASSERT_TRUE(funclet.has_value());
        EXPECT_EQ(countIn(mir, *funclet, MirOpcode::GlobalAddr), 1u);
    }

    DiagnosticReporter rep;
    EXPECT_TRUE((MirVerifier{mir, &in}.verify(rep))) << allDiagText(rep);
}

// What the parent cannot compute again is REFUSED BY NAME, before the rebuild
// meets it: the exception's code, a read of memory, and a sum over such a read —
// each is a fact about the moment the filter ran. (No C program reaches this: the
// source shares no temporary between a filter and its handler, and no pass reuses
// a read or the exception across a filter's end.)
TEST(SehHandlerTakesAFilterValue, WhatTheParentCannotComputeAgainIsRefusedByName) {
    struct Case {
        char const*      what;
        int              shape;
        std::string_view mnemonic;
    };
    constexpr std::array<Case, 3> cases{{
        {"the handler names the exception code the filter read", 0, "'seh_exception_code'"},
        {"the handler names a value the filter loaded", 1, "'load'"},
        {"the handler names a sum the filter made over a load", 2, "'load'"},
    }};
    for (Case const& k : cases) {
        SCOPED_TRACE(k.what);
        auto const build = [&](TypeInterner& in) {
            MirInstId shared{};
            TypeId    sharedType{};
            return buildOneRegion(
                in,
                [&](Ctx& c, MirInstId) {
                    if (k.shape == 0) {
                        shared     = c.mb.addInst(MirOpcode::SehExceptionCode, {}, c.u32);
                        sharedType = c.u32;
                        MirInstId const av =
                            c.mb.addConst(lit(0xC0000005, TypeKind::U32), c.u32);
                        return c.two(MirOpcode::ICmpEq, shared, av, c.i32);
                    }
                    MirInstId const loaded = c.load(c.addressOfG());
                    sharedType = c.i32;
                    shared = k.shape == 1 ? loaded
                                          : c.two(MirOpcode::Add, loaded, c.k(1), c.i32);
                    return c.two(MirOpcode::ICmpEq, shared, c.k(5), c.i32);
                },
                [&](Ctx& c) { (void)c.two(MirOpcode::Add, shared, shared, sharedType); });
        };
        EXPECT_EXIT(synthesizeAndLeave(build), ::testing::ExitedWithCode(3), "")
            << "the pass must REFUSE this module and return — a child that left "
               "with 0 accepted it, a child killed by an abort met it in the rebuild";
        if (HasFailure()) continue;

        TypeInterner in{CompilationUnitId{1}};
        Mir mir = build(in);
        Synthesized const s = synthesize(mir, in);
        EXPECT_FALSE(s.ok);
        EXPECT_NE(s.text.find("its function names a value that the filter computes"),
                  std::string::npos) << s.text;
        EXPECT_NE(s.text.find(k.mnemonic), std::string::npos) << s.text;
        EXPECT_NE(s.text.find("SEH region 0"), std::string::npos) << s.text;
        EXPECT_TRUE(s.scopes.empty()) << "a refused module must hand out no record";
    }
}
