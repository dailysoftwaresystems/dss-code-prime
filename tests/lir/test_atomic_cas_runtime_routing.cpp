// D-C-ATOMIC-COMPOUND-ASSIGNMENT-AND-INCREMENT-ARE-A-LOAD-THEN-A-SEPARATE-STORE —
// the UNDER-ALIGNED `AtomicCas` arm of `mir_to_lir`.
//
// ★★★ WHY THESE PINS EXIST AT ALL. `lowerAtomicCas` was the one atomic access the
// under-aligned routing (`atomicLoweringFor`) never consulted, which was harmless
// while every producer stamped `payload2 = 0`. A read-modify-write on a packed
// member now stamps its provable alignment on its compare-exchange exactly as on
// its load, and the load of such an object goes through the atomics runtime's lock
// table. A NATIVE `lock cmpxchg` / `ldaxr`..`stlxr` committing that value would
// take no part in the runtime's arbitration — and on arm64 the native exclusive
// pair on a misaligned address is the measured SIGBUS class.
//
// ⚠ A RACE WITNESS CANNOT PIN THIS BY ITSELF on the gate's legs: qemu-user does not
// enforce the arm64 alignment check (P53), and a native `lock cmpxchg` on x86 is
// still atomic against other locked instructions. What every host CAN see is the
// EMITTED FORM, which is what these assert — each with its aligned CONTROL, so a
// "fix" that routed every compare-exchange through the runtime reds too.
//
// Lives in its own file because `test_mir_to_lir.cpp` belongs to another P66 lane.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/merge/synth_seh_funclets.hpp"   // MirSehScope
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// `prev = AtomicCas(ptr, expected, desired)` with the lvalue's provable alignment
// on `payload2` (1 = a packed member, 4 = naturally aligned, 0 = unknown).
[[nodiscard]] Mir buildCasFnMir(std::uint32_t provableAlign, TypeInterner& interner) {
    TypeId const i32  = interner.primitive(TypeKind::I32);
    TypeId const i32p = interner.pointer(i32);
    TypeId const params[] = {i32p, i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    // This hand-built module is its own table: nothing beside it names an id it
    // does not define (the under-aligned arms mint the runtime entry's import).
    mb.stateSelfContainedSymbolIds();
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const ptr      = mb.addArg(0, i32p);
    MirInstId const expected = mb.addArg(1, i32);
    MirInstId const desired  = mb.addArg(2, i32);
    MirInstId const casOps[] = {ptr, expected, desired};
    MirInstId const prev = mb.addInst(MirOpcode::AtomicCas, casOps, i32,
                                      /*payload=*/0, MirInstFlags::None,
                                      provableAlign);
    mb.addReturn(prev);
    return std::move(mb).finish();
}

// The elf spelling of the generic entries, as the shipped elf documents declare
// them — with or without the compare-exchange entry.
[[nodiscard]] AtomicsRuntime elfRuntime(bool withCompareExchange) {
    AtomicsRuntime ar;
    ar.role             = RuntimeLibraryRole::AtomicsRuntime;
    ar.libraryPath      = "libatomic.so.1";
    ar.loadMangledName  = "__atomic_load";
    ar.storeMangledName = "__atomic_store";
    if (withCompareExchange) ar.compareExchangeMangledName = "__atomic_compare_exchange";
    return ar;
}

// Count `mnemonic` over EVERY block of function 0 — the arm64 native form is a
// multi-block LL/SC loop. A mnemonic the target does not declare answers -1, so a
// misspelled name can never read as the "zero occurrences" a pin asserts.
[[nodiscard]] int countEverywhere(Lir const& lir, TargetSchema const& sch,
                                  std::string_view mnemonic) {
    auto const want = sch.opcodeByMnemonic(mnemonic);
    if (!want.has_value()) return -1;
    LirFuncId const f = lir.funcAt(0);
    int n = 0;
    for (std::uint32_t b = 0; b < lir.funcBlockCount(f); ++b) {
        LirBlockId const bb = lir.funcBlockAt(f, b);
        for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
            if (lir.instOpcode(lir.blockInstAt(bb, i)) == *want) ++n;
        }
    }
    return n;
}

struct Lowered {
    MirToLirResult       result;
    DiagnosticReporter   rep;
};

[[nodiscard]] Lowered lowerCas(TargetSchema const& target, std::uint32_t provableAlign,
                               std::optional<AtomicsRuntime> runtime,
                               TypeInterner& interner) {
    Mir mir = buildCasFnMir(provableAlign, interner);
    Lowered out;
    std::vector<ExternImport> noExterns;
    out.result = lowerToLir(mir, target, interner, out.rep, noExterns,
                            ExternCallDispatch::DirectPlt, std::nullopt,
                            std::nullopt, {}, std::nullopt, std::nullopt,
                            std::nullopt, std::move(runtime));
    return out;
}

[[nodiscard]] bool mintedImport(MirToLirResult const& r, std::string_view name,
                                std::string_view image) {
    for (auto const& e : r.externImports) {
        if (e.mangledName == name) return e.libraryPath == image && !e.isData;
    }
    return false;
}

}  // namespace

TEST(AtomicCasRuntimeRouting, UnderAlignedCasOnATrapsTargetCallsTheRuntimeNotLlsc) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    ASSERT_EQ((*target)->underAlignedAtomicForm(), UnderAlignedAtomicForm::Traps);
    TypeInterner interner{CompilationUnitId{1}};
    auto const L = lowerCas(**target, /*provableAlign=*/1, elfRuntime(true), interner);
    ASSERT_TRUE(L.result.ok) << (L.rep.all().empty() ? "" : L.rep.all()[0].actual);
    EXPECT_EQ(countEverywhere(L.result.lir, **target, "ldaxr"), 0)
        << "RED-ON-DISABLE: an UNDER-ALIGNED compare-exchange must not emit the "
           "native exclusive pair — its load went through the runtime's lock";
    EXPECT_EQ(countEverywhere(L.result.lir, **target, "stlxr"), 0);
    EXPECT_EQ(countEverywhere(L.result.lir, **target, "call"), 1)
        << "one call to the format's generic compare-exchange entry";
    EXPECT_TRUE(mintedImport(L.result, "__atomic_compare_exchange", "libatomic.so.1"))
        << "the lowerer must MINT the import the FORMAT names, bound to its image";
}

TEST(AtomicCasRuntimeRouting, AlignedCasOnATrapsTargetKeepsTheNativeLlscLoop) {
    // THE CONTROL: same target, same runtime, only the alignment differs.
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TypeInterner interner{CompilationUnitId{1}};
    auto const L = lowerCas(**target, /*provableAlign=*/4, elfRuntime(true), interner);
    ASSERT_TRUE(L.result.ok) << (L.rep.all().empty() ? "" : L.rep.all()[0].actual);
    EXPECT_EQ(countEverywhere(L.result.lir, **target, "ldaxr"), 1);
    EXPECT_EQ(countEverywhere(L.result.lir, **target, "stlxr"), 1);
    EXPECT_EQ(countEverywhere(L.result.lir, **target, "call"), 0)
        << "RED-ON-DISABLE: an aligned compare-exchange must not pay for a libcall";
    EXPECT_TRUE(L.result.externImports.empty());
}

TEST(AtomicCasRuntimeRouting, UnderAlignedCasOnALosesAtomicityTargetCallsTheRuntime) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ASSERT_EQ((*target)->underAlignedAtomicForm(), UnderAlignedAtomicForm::LosesAtomicity);
    TypeInterner interner{CompilationUnitId{1}};
    auto const L = lowerCas(**target, /*provableAlign=*/1, elfRuntime(true), interner);
    ASSERT_TRUE(L.result.ok) << (L.rep.all().empty() ? "" : L.rep.all()[0].actual);
    EXPECT_EQ(countEverywhere(L.result.lir, **target, "lock_cmpxchg"), 0)
        << "RED-ON-DISABLE: a runtime-loaded value must not be committed by a "
           "native `lock cmpxchg`, which never takes the runtime's lock";
    EXPECT_EQ(countEverywhere(L.result.lir, **target, "call"), 1);
    EXPECT_TRUE(mintedImport(L.result, "__atomic_compare_exchange", "libatomic.so.1"));
}

TEST(AtomicCasRuntimeRouting, AlignedAndUnknownAlignmentCasKeepTheNativeLockCmpxchg) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    for (std::uint32_t const align : {4u, 0u}) {
        TypeInterner interner{CompilationUnitId{1}};
        auto const L = lowerCas(**target, align, elfRuntime(true), interner);
        ASSERT_TRUE(L.result.ok) << "align " << align;
        EXPECT_EQ(countEverywhere(L.result.lir, **target, "lock_cmpxchg"), 1)
            << "align " << align << ": `payload2 == 0` is the unknown sentinel and "
               "must stay byte-for-byte the native form";
        EXPECT_EQ(countEverywhere(L.result.lir, **target, "call"), 0) << "align " << align;
    }
}

TEST(AtomicCasRuntimeRouting, ARuntimeWithoutACompareExchangeEntryRefusesByName) {
    // The format supplies load/store but no compare-exchange: refuse, naming the
    // key — never commit a runtime-loaded value natively.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TypeInterner interner{CompilationUnitId{1}};
    auto const L = lowerCas(**target, /*provableAlign=*/1, elfRuntime(false), interner);
    EXPECT_FALSE(L.result.ok);
    bool named = false;
    for (auto const& d : L.rep.all()) {
        if (d.actual.find("compareExchangeMangledName") != std::string::npos) named = true;
    }
    EXPECT_TRUE(named) << "the refusal must name the missing format key";
    EXPECT_EQ(countEverywhere(L.result.lir, **target, "lock_cmpxchg"), 0)
        << "and it must not emit the native form on the way out";
}

TEST(AtomicCasRuntimeRouting, ATrapsTargetWithNoRuntimeRefusesTheUnderAlignedCas) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TypeInterner interner{CompilationUnitId{1}};
    auto const L = lowerCas(**target, /*provableAlign=*/1, std::nullopt, interner);
    EXPECT_FALSE(L.result.ok)
        << "RED-ON-DISABLE: the native exclusive pair is a certain fault here";
    EXPECT_EQ(countEverywhere(L.result.lir, **target, "ldaxr"), 0);
}

// ── A GUARDED REGION IS A SET OF CONTIGUOUS RUNS: THE LL/SC LOOP IS ONE ──────
// (D-LIR-GUARDED-RANGE-DOES-NOT-COVER-BLOCKS-THE-LOWERING-CREATES)
//
// A `__try` region's first scope record runs over the blocks the region's MIR
// blocks became. The LL/SC retry loop is THREE blocks the lowering creates while
// lowering the compare-exchange — the exclusive load, the exclusive store, and
// `done`, which also receives the REST of the guarded block — laid out after
// every block of the function's own, outside that record. Its load and store
// address the program's object. ✔MEASURED P69 on pe64 for the other creator of
// such a block (an `asm goto` whose output is stored through its object's
// address: the process ended with 0xC0000005 inside a `__try`). So the three are
// a RUN of the region and get a scope record of their own, with the region's
// handler. The rule reads the blocks the lowering creates and no target or
// format name, which is what the two arms below show: the SAME scope over the
// SAME instruction has two records where the lowering creates blocks (arm64) and
// one where it does not (x86_64, one `lock cmpxchg` in the block itself).
// (The statement used to be REFUSED by name; lir/test_guarded_region_runs holds
// the records' order and the other creators.)
constexpr std::uint32_t kGuardedFuncletV     = 501;
constexpr std::uint32_t kGuardedPersonalityV = 500;

// `i32 f(i32* p, i32 e, i32 d)`: the compare-exchange in the guarded block `body`,
// which is entered from `entry` and whose landing is `handler`.
[[nodiscard]] Lowered lowerCasInsideAGuardedBody(TargetSchema const& target,
                                                 TypeInterner& interner) {
    TypeId const i32  = interner.primitive(TypeKind::I32);
    TypeId const i32p = interner.pointer(i32);
    TypeId const params[] = {i32p, i32, i32};
    TypeId const fnSig = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.stateSelfContainedSymbolIds();   // its own table, as `buildCasFnMir`'s
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry   = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const body    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const handler = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirInstId const ptr      = mb.addArg(0, i32p);
    MirInstId const expected = mb.addArg(1, i32);
    MirInstId const desired  = mb.addArg(2, i32);
    mb.addBr(body);
    mb.beginBlock(body);
    MirInstId const casOps[] = {ptr, expected, desired};
    MirInstId const prev = mb.addInst(MirOpcode::AtomicCas, casOps, i32, /*payload=*/0,
                                      MirInstFlags::None, /*provableAlign=*/4);
    mb.addReturn(prev);
    mb.beginBlock(handler);
    mb.addReturn(expected);
    Mir mir = std::move(mb).finish();
    MirSehScope scope;
    scope.parentFuncSymbol    = SymbolId{100};
    scope.beginBlock          = body;
    scope.endBlock            = body;
    scope.handlerBlock        = handler;
    scope.filterFuncletSymbol = SymbolId{kGuardedFuncletV};
    scope.personalitySymbol   = SymbolId{kGuardedPersonalityV};
    std::array<MirSehScope, 1> const scopes{scope};
    Lowered out;
    std::vector<ExternImport> noExterns;
    out.result = lowerToLir(mir, target, interner, out.rep, noExterns,
                            ExternCallDispatch::DirectPlt, std::nullopt,
                            std::nullopt, scopes, std::nullopt, std::nullopt,
                            std::nullopt, elfRuntime(true));
    return out;
}

TEST(AtomicCasRuntimeRouting, AnLlscLoopInsideAGuardedBodyIsARunOfItsRegion) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TypeInterner interner{CompilationUnitId{1}};
    auto const L = lowerCasInsideAGuardedBody(**target, interner);
    ASSERT_TRUE(L.result.ok) << (L.rep.all().empty() ? "" : L.rep.all()[0].actual);
    Lir const& lir = L.result.lir;
    LirFuncId const fn = lir.funcAt(0);
    // The function's own three blocks (entry, body, handler), then the loop's
    // three, created while `body` was lowered: the exclusive load, the exclusive
    // store, and `done`.
    ASSERT_EQ(lir.funcBlockCount(fn), 6u);
    auto const blockV = [&](std::uint32_t k) { return lir.funcBlockAt(fn, k).v; };
    auto const holds = [&](std::uint32_t k, std::string_view mnemonic) {
        auto const want = (*target)->opcodeByMnemonic(mnemonic);
        if (!want.has_value()) return false;
        LirBlockId const b = lir.funcBlockAt(fn, k);
        for (std::uint32_t i = 0; i < lir.blockInstCount(b); ++i) {
            if (lir.instOpcode(lir.blockInstAt(b, i)) == *want) return true;
        }
        return false;
    };
    ASSERT_TRUE(holds(3, "ldaxr")) << "the loop's first block holds the exclusive load";
    ASSERT_TRUE(holds(4, "stlxr")) << "its second the exclusive store";
    ASSERT_TRUE(lir.blockSuccessors(lir.funcBlockAt(fn, 5)).empty())
        << "`done` receives the rest of the guarded block: its return ends the function there";
    ASSERT_EQ(lir.blockSuccessors(lir.funcBlockAt(fn, 1)).size(), 1u)
        << "…so the body's own block only jumps into the loop";
    EXPECT_EQ(lir.blockSuccessors(lir.funcBlockAt(fn, 1))[0].v, blockV(3));

    // TWO records, in this order: the body's own block, then the loop — RED-ON-
    // DISABLE: with the run's record gone, the exclusive load and store, and
    // everything the guarded block does after them, sit in blocks no scope guards.
    auto const& ds = L.result.sehScopeDescriptors;
    ASSERT_EQ(ds.size(), 2u);
    EXPECT_EQ(ds[0].beginLirBlockV, blockV(1));
    EXPECT_EQ(ds[0].endLirBlockV, blockV(1));
    EXPECT_EQ(ds[1].beginLirBlockV, blockV(3)) << "the run begins at the loop's first block";
    EXPECT_EQ(ds[1].endLirBlockV, blockV(5)) << "and ends at `done`";
    for (auto const& d : ds) {
        EXPECT_EQ(d.funcIndex, 0u);
        EXPECT_EQ(d.handlerLirBlockV, blockV(2)) << "every record of a region enters its one handler";
        EXPECT_EQ(d.filterFuncletSymbol.v, kGuardedFuncletV);
        EXPECT_EQ(d.personalitySymbol.v, kGuardedPersonalityV);
    }
}

TEST(AtomicCasRuntimeRouting, ACompareExchangeThatCreatesNoBlockLeavesItsRegionOneRecord) {
    // THE CONTROL, and the half that keeps the rule honest: the same scope over
    // the same instruction on a target whose compare-exchange is ONE instruction
    // in the guarded block itself. Nothing is created, so the region has the one
    // record it always had — a rule that gave "an atomic in a `__try`" a second
    // record would red here.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TypeInterner interner{CompilationUnitId{1}};
    auto const L = lowerCasInsideAGuardedBody(**target, interner);
    ASSERT_TRUE(L.result.ok) << (L.rep.all().empty() ? "" : L.rep.all()[0].actual);
    EXPECT_EQ(countEverywhere(L.result.lir, **target, "lock_cmpxchg"), 1);
    ASSERT_EQ(L.result.lir.funcBlockCount(L.result.lir.funcAt(0)), 3u);
    ASSERT_EQ(L.result.sehScopeDescriptors.size(), 1u);
    EXPECT_EQ(L.result.sehScopeDescriptors[0].beginLirBlockV,
              L.result.sehScopeDescriptors[0].endLirBlockV)
        << "a single-block guarded body: its first and last block are one";
}
