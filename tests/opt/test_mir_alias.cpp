// mir_alias substrate unit tests — pin every rule of `mirMayAlias`
// with a non-tautological fixture. Each rule has a positive AND a
// negative pin: a regression that flips a polarity (or that admits a
// new TypeKind enumerator silently) fails loud.

#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_id.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/analysis/mir_alias.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>

using namespace dss;
using namespace dss::opt::analysis;

namespace {

// Module with N pointer-typed Args at the entry block. Args are NOT
// Allocas, so Rule 2 doesn't fire — the predicate genuinely descends
// to type-based reasoning (Rules 4/5/6).
struct ArgModule {
    Mir       mir;
    MirInstId args[4];
};

ArgModule buildArgModule(
    TypeInterner&             interner,
    std::span<TypeId const>   paramTypes)
{
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig(paramTypes, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);

    ArgModule out{};
    for (std::uint32_t i = 0; i < paramTypes.size() && i < 4; ++i) {
        out.args[i] = mb.addArg(i, paramTypes[i]);
    }
    mb.addReturn();
    out.mir = std::move(mb).finish();
    return out;
}

// Module with N Alloca instructions at the entry block — for Rule 2 tests.
struct AllocaModule {
    Mir       mir;
    MirInstId allocas[4];
};

AllocaModule buildAllocaModule(
    TypeInterner&           interner,
    std::span<TypeId const> elemTypes)
{
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);

    AllocaModule out{};
    for (std::uint32_t i = 0; i < elemTypes.size() && i < 4; ++i) {
        TypeId const ptr = interner.pointer(elemTypes[i]);
        out.allocas[i] = mb.addInst(MirOpcode::Alloca, {}, ptr);
    }
    mb.addReturn();
    out.mir = std::move(mb).finish();
    return out;
}

} // namespace

// ── Rule 1: same SSA id → Yes ────────────────────────────────────────

TEST(MirAlias, Rule1_SamePointerIdReturnsYes) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32 = interner.primitive(TypeKind::I32);
    std::array<TypeId, 1> const elems{i32};
    auto m = buildAllocaModule(interner, elems);

    EXPECT_EQ(mirMayAlias(m.mir, interner, m.allocas[0], m.allocas[0]),
              MirAliasResult::Yes);
    // Strict-mode must not change the same-id verdict.
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.allocas[0], m.allocas[0],
                          StrictTbaa::Yes),
              MirAliasResult::Yes);
}

// ── Rule 2: distinct Allocas → No ────────────────────────────────────

TEST(MirAlias, Rule2_DistinctAllocasReturnNo) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32 = interner.primitive(TypeKind::I32);
    std::array<TypeId, 2> const elems{i32, i32};
    auto m = buildAllocaModule(interner, elems);

    EXPECT_EQ(mirMayAlias(m.mir, interner, m.allocas[0], m.allocas[1]),
              MirAliasResult::No);
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.allocas[0], m.allocas[1],
                          StrictTbaa::Yes),
              MirAliasResult::No);
}

// ── Rule 3: non-pointer-typed SSA values → Maybe ─────────────────────

TEST(MirAlias, Rule3_NonPointerTypedInputsReturnMaybe) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32 = interner.primitive(TypeKind::I32);
    std::array<TypeId, 2> const params{i32, i32};
    auto m = buildArgModule(interner, params);

    // Both args are i32-typed (NOT pointer-typed). Rule 3 short-circuits
    // to Maybe via the !pointeeA.valid() / !pointeeB.valid() check.
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1],
                          StrictTbaa::Yes),
              MirAliasResult::Maybe);
}

// ── Rule 4: either Ptr<Void> → Maybe (even under strict TBAA) ────────

TEST(MirAlias, Rule4_PtrToVoidPointeeReturnsMaybeUnderStrict) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT   = interner.primitive(TypeKind::Void);
    TypeId const i32     = interner.primitive(TypeKind::I32);
    TypeId const ptrVoid = interner.pointer(voidT);
    TypeId const ptrI32  = interner.pointer(i32);
    std::array<TypeId, 2> const params{ptrVoid, ptrI32};
    auto m = buildArgModule(interner, params);

    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1],
                          StrictTbaa::Yes),
              MirAliasResult::Maybe);
    // Symmetric — flip the argument order.
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[1], m.args[0],
                          StrictTbaa::Yes),
              MirAliasResult::Maybe);
}

// ── Rule 5: distinct primitive pointees + StrictTbaa::Yes → No ───────
//                                + StrictTbaa::No  → Maybe (polarity proof)

TEST(MirAlias, Rule5_DistinctPrimitivePointeesViaArgsUnderStrictTBAA) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const i64    = interner.primitive(TypeKind::I64);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const ptrI64 = interner.pointer(i64);
    std::array<TypeId, 2> const params{ptrI32, ptrI64};
    auto m = buildArgModule(interner, params);

    // Strict TBAA → No (precision win).
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1],
                          StrictTbaa::Yes),
              MirAliasResult::No);
    // Commutativity — flip the argument order under strict.
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[1], m.args[0],
                          StrictTbaa::Yes),
              MirAliasResult::No);
    // StrictTbaa::No (default) → Maybe (polarity proof for the flag).
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1]),
              MirAliasResult::Maybe);
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1],
                          StrictTbaa::No),
              MirAliasResult::Maybe);
}

// ── Rule 6: same primitive pointees → Maybe (catch-all) ──────────────

TEST(MirAlias, Rule6_SamePrimitivePointeesUnderStrictReturnMaybe) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const ptrI32 = interner.pointer(i32);
    std::array<TypeId, 2> const params{ptrI32, ptrI32};
    auto m = buildArgModule(interner, params);

    // Even strict TBAA can't say No for same-type distinct pointers.
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1],
                          StrictTbaa::Yes),
              MirAliasResult::Maybe);
}

// ── Rule 6: aggregate vs primitive → Maybe (positive) ────────────────

TEST(MirAlias, Rule6_AggregateVsPrimitivePointeesReturnMaybeUnderStrict) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32 = interner.primitive(TypeKind::I32);
    std::array<TypeId, 1> const structFields{i32};
    TypeId const structT  = interner.structType("S", structFields);
    TypeId const ptrI32   = interner.pointer(i32);
    TypeId const ptrStruct = interner.pointer(structT);
    std::array<TypeId, 2> const params{ptrI32, ptrStruct};
    auto m = buildArgModule(interner, params);

    // Strict TBAA must NOT fire on aggregate-vs-primitive (Rule 5 keyed
    // on isPrimitiveNonVoid; aggregate fails it; Rule 6 catches → Maybe).
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1],
                          StrictTbaa::Yes),
              MirAliasResult::Maybe);
}

// ── Ref vs Ptr (both indirection kinds carry pointee → both walk
// through mirPointeeType) ────────────────────────────────────────────

TEST(MirAlias, RefAndPtrBothExtractPointee) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const i64    = interner.primitive(TypeKind::I64);
    TypeId const refI32 = interner.reference(i32);
    TypeId const ptrI64 = interner.pointer(i64);
    std::array<TypeId, 2> const params{refI32, ptrI64};
    auto m = buildArgModule(interner, params);

    // Rule 5 distinguishes via pointee TypeKind; works equally on Ref
    // and Ptr because mirPointeeType walks both.
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1],
                          StrictTbaa::Yes),
              MirAliasResult::No);
}

// ── Symmetry property test (Rule 5 commutativity proven; widen
// coverage so future asymmetric rules can't slip through) ────────────

TEST(MirAlias, PredicateIsSymmetric) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT   = interner.primitive(TypeKind::Void);
    TypeId const i32     = interner.primitive(TypeKind::I32);
    TypeId const i64     = interner.primitive(TypeKind::I64);
    TypeId const ptrVoid = interner.pointer(voidT);
    TypeId const ptrI32  = interner.pointer(i32);
    TypeId const ptrI64  = interner.pointer(i64);
    std::array<TypeId, 3> const params{ptrVoid, ptrI32, ptrI64};
    auto m = buildArgModule(interner, params);

    StrictTbaa const modes[] = {StrictTbaa::No, StrictTbaa::Yes};
    for (StrictTbaa const mode : modes) {
        for (std::uint32_t i = 0; i < 3; ++i) {
            for (std::uint32_t j = 0; j < 3; ++j) {
                auto const ab = mirMayAlias(m.mir, interner,
                                            m.args[i], m.args[j], mode);
                auto const ba = mirMayAlias(m.mir, interner,
                                            m.args[j], m.args[i], mode);
                EXPECT_EQ(ab, ba)
                    << "asymmetry at (i=" << i << ", j=" << j
                    << ", strict=" << (mode == StrictTbaa::Yes) << ")";
            }
        }
    }
}
