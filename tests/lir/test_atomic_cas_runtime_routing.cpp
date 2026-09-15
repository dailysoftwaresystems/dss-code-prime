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
