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

    // Rule 1 must dominate every other rule across the full polarity
    // matrix — its ptrA.v == ptrB.v early-return precedes Rule 2's
    // alloca check, Rule 5's char-exception, and Rule 6's strict-TBAA.
    for (StrictTbaa const strict : {StrictTbaa::No, StrictTbaa::Yes}) {
        for (bool const charAll : {false, true}) {
            EXPECT_EQ(mirMayAlias(m.mir, interner,
                                  m.allocas[0], m.allocas[0],
                                  strict, charAll),
                      MirAliasResult::Yes)
                << "Rule 1 must short-circuit on same-SSA id (strict="
                << (strict == StrictTbaa::Yes ? "Yes" : "No")
                << ", charAll=" << charAll << ")";
        }
    }
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

// ── Rule 6: distinct primitive pointees + StrictTbaa::Yes → No ───────
//                                + StrictTbaa::No  → Maybe (polarity proof)

TEST(MirAlias, Rule6_DistinctPrimitivePointeesViaArgsUnderStrictTBAA) {
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

// ── Rule 5: character-type exception (C99 §6.5 ¶7) ───────────────────
// A character-typed pointer may alias an object of ANY type, even under
// strict TBAA. This is part of the strict-aliasing rule itself, not an
// opt-out — `char*` punning is the canonical exception that lets
// serializers / hash visitors / memcpy implementations be sound.

TEST(MirAlias, Rule5_CharPointeeAliasesAllUnderStrict) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT  = interner.primitive(TypeKind::Void);
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const charT  = interner.primitive(TypeKind::Char);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const ptrCh  = interner.pointer(charT);
    (void)voidT;
    std::array<TypeId, 2> const params{ptrI32, ptrCh};
    auto m = buildArgModule(interner, params);

    // Even under strict TBAA, char-vs-i32 must stay Maybe (NOT No).
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1],
                          StrictTbaa::Yes),
              MirAliasResult::Maybe)
        << "C99 §6.5 ¶7: char* may alias any object type";
    // Symmetric.
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[1], m.args[0],
                          StrictTbaa::Yes),
              MirAliasResult::Maybe);
}

// Negative polarity: when the language declares charTypesAliasAll=false
// (Rust-like / strict-typed DSL), Rule 5 does NOT fire and the strict-
// TBAA verdict applies even to char vs distinct-primitive pairs. Closes
// D-OPT-MIR-ALIAS-CHAR-EXCEPTION-OVERRIDE via the predicate parameter.
TEST(MirAlias, Rule5_CharExceptionDisabledLetsStrictTBAAReturnNo) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const charT  = interner.primitive(TypeKind::Char);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const ptrCh  = interner.pointer(charT);
    std::array<TypeId, 2> const params{ptrI32, ptrCh};
    auto m = buildArgModule(interner, params);

    // Default charTypesAliasAll=true: Rule 5 fires → Maybe.
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1],
                          StrictTbaa::Yes, /*charTypesAliasAll=*/true),
              MirAliasResult::Maybe);
    // Opt out: char-exception disabled, distinct primitive pointees
    // fall through to Rule 6 strict-TBAA → No.
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1],
                          StrictTbaa::Yes, /*charTypesAliasAll=*/false),
              MirAliasResult::No);
}

// Rule 7 catch-all pin under the strict + char-exception-disabled
// combination. Same-kind char-vs-char must STILL be Maybe (the strict-
// TBAA Rule 6 only fires on DISTINCT primitive kinds — same kind
// falls through). Without this pin, a future regression to
// isDistinctPrimitivePair that drops the `kA != kB` guard would
// silently make same-type char* CSE-mergeable.
//
// Asymmetric coverage: the `charTypesAliasAll=false` arm is what
// guards `isDistinctPrimitivePair`'s `kA != kB` invariant (it routes
// to Rule 6 directly). The `=true` arm only proves Rule 5's same-kind
// short-circuit — that arm becomes dead weight if a future cleanup
// removes Rule 5's pre-position. Both polarities asserted so any
// future review-of-this-test can see the dual purpose explicitly.
TEST(MirAlias, Rule7_SameCharPointeesUnderStrictNoCharExceptionReturnMaybe) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const charT  = interner.primitive(TypeKind::Char);
    TypeId const ptrCh  = interner.pointer(charT);
    std::array<TypeId, 2> const params{ptrCh, ptrCh};
    auto m = buildArgModule(interner, params);

    // Both pointees Char. Even with strict + char-exception disabled,
    // Rule 7's catch-all → Maybe (Rule 6 strict-TBAA requires distinct
    // kinds; same kind doesn't qualify).
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1],
                          StrictTbaa::Yes, /*charTypesAliasAll=*/false),
              MirAliasResult::Maybe);
    // With char-exception enabled, Rule 5 fires → Maybe (the same
    // verdict, different rule).
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1],
                          StrictTbaa::Yes, /*charTypesAliasAll=*/true),
              MirAliasResult::Maybe);
}

TEST(MirAlias, Rule5_BytePointeeAliasesAllUnderStrict) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const byteT  = interner.primitive(TypeKind::Byte);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const ptrBy  = interner.pointer(byteT);
    std::array<TypeId, 2> const params{ptrI32, ptrBy};
    auto m = buildArgModule(interner, params);

    // Byte is the MIR-tier byte-addressable companion to Char; same
    // alias-all rule applies (the rationale is byte-addressability,
    // not the C-standard's specific character types).
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[0], m.args[1],
                          StrictTbaa::Yes),
              MirAliasResult::Maybe);
    // Symmetric — Rule 5 must fire regardless of argument order.
    EXPECT_EQ(mirMayAlias(m.mir, interner, m.args[1], m.args[0],
                          StrictTbaa::Yes),
              MirAliasResult::Maybe);
}

// ── Rule 7: same primitive pointees → Maybe (catch-all) ──────────────

TEST(MirAlias, Rule7_SamePrimitivePointeesUnderStrictReturnMaybe) {
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

// ── Rule 7: aggregate vs primitive → Maybe (positive) ────────────────

TEST(MirAlias, Rule7_AggregateVsPrimitivePointeesReturnMaybeUnderStrict) {
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

// ── Mir.aliasingMode substrate (D-OPT-LOAD-ALIAS-ANALYSIS-STRICT-TBAA-WIRING) ──

// Default aliasing mode is Permissive — substrate is sound out of the
// box for any language that doesn't explicitly opt into strict TBAA.
TEST(MirAlias, MirAliasingModeDefaultIsPermissive) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    EXPECT_EQ(mir.aliasingMode(), MirAliasingMode::Permissive);
}

// MirBuilder::setAliasingMode propagates through finish() — the
// frozen Mir reflects what the builder was told. Round-trip pin.
// Default charTypesAliasAll is true (sound out of the box for any
// language that doesn't explicitly opt out).
TEST(MirAlias, MirCharTypesAliasAllDefaultsToTrue) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    EXPECT_TRUE(mir.charTypesAliasAll());
}

// MirBuilder::setCharTypesAliasAll propagates through finish().
TEST(MirAlias, MirBuilderSetCharTypesAliasAllRoundTrips) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.setCharTypesAliasAll(false);
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    EXPECT_FALSE(mir.charTypesAliasAll());
}

TEST(MirAlias, MirBuilderSetAliasingModeRoundTrips) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.setAliasingMode(MirAliasingMode::StrictTBAA);
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    EXPECT_EQ(mir.aliasingMode(), MirAliasingMode::StrictTBAA);
}

// Mir's move ctor preserves the aliasing mode (and resets the source
// to Permissive — matching the reset-to-default discipline used for
// the moved-from arena state).
TEST(MirAlias, MirMoveCtorPreservesAliasingMode) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.setAliasingMode(MirAliasingMode::StrictTBAA);
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    mb.addReturn();
    Mir mir = std::move(mb).finish();
    Mir const moved = std::move(mir);

    EXPECT_EQ(moved.aliasingMode(), MirAliasingMode::StrictTBAA);
    EXPECT_EQ(mir.aliasingMode(), MirAliasingMode::Permissive)
        << "moved-from Mir must reset to Permissive";
}

// Move-assignment symmetry: assignment also propagates + resets. The
// move ctor and move assignment are independently implemented in
// rule-of-5 types, so this is a distinct pin from the ctor test above.
TEST(MirAlias, MirMoveAssignmentPreservesAliasingMode) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mbStrict;
    mbStrict.setAliasingMode(MirAliasingMode::StrictTBAA);
    mbStrict.addFunction(fnSig, SymbolId{100});
    MirBlockId const e1 = mbStrict.createBlock(StructCfMarker::EntryBlock);
    mbStrict.beginBlock(e1);
    mbStrict.addReturn();
    Mir strict = std::move(mbStrict).finish();

    MirBuilder mbPerm;
    mbPerm.addFunction(fnSig, SymbolId{101});
    MirBlockId const e2 = mbPerm.createBlock(StructCfMarker::EntryBlock);
    mbPerm.beginBlock(e2);
    mbPerm.addReturn();
    Mir target = std::move(mbPerm).finish();

    target = std::move(strict);
    EXPECT_EQ(target.aliasingMode(), MirAliasingMode::StrictTBAA);
    EXPECT_EQ(strict.aliasingMode(), MirAliasingMode::Permissive);
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
