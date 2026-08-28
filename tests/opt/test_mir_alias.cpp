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
#include "opt/analysis/mir_escape.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

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

// c113 (D-CSUBSET-INTRINSIC-BARRIER, audit-F1 + the review correction): the
// region clobber walk is the ONE chokepoint gating Load motion for BOTH
// consumers (CSE reuse-admission in-block slices + cross-block region + the
// LICM loop wrapper all funnel through `mirInstClobbersLoadPtr`), so this
// pins the chokepoint directly. A non-Store MEMORY-CLOBBERING op in the
// region (CompilerBarrier here; Call/IntrinsicCall/AtomicCas by the same
// `opcodeClobbersMemory` positive list) is an OPAQUE clobber — the walk
// must return true even when every Store in the region provably does NOT
// alias the Load pointer. The negative control is DOUBLY load-bearing: its
// block contains a non-aliasing Store (distinct Allocas, Rule 2) AND ends
// in a Return TERMINATOR (hasSideEffects=true, a DCE-liveness flag) — the
// walk must return false, proving terminators/DCE-side-effecting ops are
// NOT clobbers (conflating the flags disables Load motion wholesale: every
// loop body ends in a terminator — the review-caught LICM red).
// RED-on-disable: drop the opcodeClobbersMemory arm → the barrier is
// skipped (not a Store) → the positive half fails; key the arm on
// hasSideEffects instead → the negative control's terminator becomes a
// clobber → the negative half fails.
TEST(MirAlias, RegionWalkTreatsMemoryClobberOpsAsClobber) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    // One builder recipe, two modules: [a, b, store→b] (+ barrier in the
    // second). The Load pointer is `a`; the Store writes through `b`
    // (distinct Alloca → Rule 2 says No), so ONLY the barrier can clobber.
    auto const build = [&](bool withBarrier) {
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
        if (withBarrier) {
            (void)mb.addInst(MirOpcode::CompilerBarrier, {}, InvalidType);
        }
        mb.addReturn();
        struct Out { Mir mir; MirInstId loadPtr; MirBlockId block; };
        return Out{std::move(mb).finish(), a, entry};
    };

    auto const clean = build(/*withBarrier=*/false);
    MirBlockId const cleanRegion[] = {clean.block};
    EXPECT_FALSE(mirAnyMayAliasingStoreInRegion(
        clean.mir, interner, clean.loadPtr, cleanRegion))
        << "negative control: a non-aliasing Store + the block's Return "
           "TERMINATOR (hasSideEffects=true) must NOT clobber — the walk "
           "keys on opcodeClobbersMemory, never the DCE-liveness flag";

    auto const fenced = build(/*withBarrier=*/true);
    MirBlockId const fencedRegion[] = {fenced.block};
    EXPECT_TRUE(mirAnyMayAliasingStoreInRegion(
        fenced.mir, interner, fenced.loadPtr, fencedRegion))
        << "a memory-clobbering non-Store op (CompilerBarrier) must be an "
           "opaque clobber — Loads may not move across the fence";
}

// ── D-OPT-CSE-LOAD-PTR-KEY-UNRESOLVED ────────────────────────────────
// `mirAliasProbeSubstitutionPreservesClobberVerdict` is the licence that lets a
// value-numbering consumer hand the alias gate the CANONICAL pointer id instead
// of the raw operand its key already resolved away. Its docblock states a
// THEOREM; these two tests are its polarity pin and its proof-by-exhaustion.
//
// Half 1 — the predicate's own polarity. Same id ⇒ licensed. Same opcode AND
// same TypeId AND not an `Alloca` ⇒ licensed. A different TypeId, a different
// opcode, or an `Alloca` at either end ⇒ REFUSED — and it is those refusals
// that make the consumer's fail-loud reachable rather than decorative.
TEST(MirAlias, ProbeSubstitutionLicenceRequiresBothOpcodeAndType) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const i64    = interner.primitive(TypeKind::I64);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const ptrI64 = interner.pointer(i64);
    TypeId const voidT  = interner.primitive(TypeKind::Void);
    TypeId const fnSig  = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptrI32);
    MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(z, i32);
    MirInstId const gops[] = {slot, c0};
    MirInstId const gepA = mb.addInst(MirOpcode::Gep, gops, ptrI32);
    MirInstId const gepB = mb.addInst(MirOpcode::Gep, gops, ptrI32);
    MirInstId const gepW = mb.addInst(MirOpcode::Gep, gops, ptrI64);  // wider
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    EXPECT_TRUE(mirAliasProbeSubstitutionPreservesClobberVerdict(mir, gepA, gepA))
        << "identity is always licensed";
    EXPECT_TRUE(mirAliasProbeSubstitutionPreservesClobberVerdict(mir, gepA, gepB))
        << "same opcode + same TypeId is exactly the theorem's premise";
    EXPECT_FALSE(mirAliasProbeSubstitutionPreservesClobberVerdict(mir, gepA, gepW))
        << "different pointee type unlocks Rule 6's No — must be REFUSED";
    EXPECT_FALSE(mirAliasProbeSubstitutionPreservesClobberVerdict(mir, gepA, slot))
        << "different opcode unlocks Rule 2's distinct-Alloca No — must be "
           "REFUSED even though the TypeIds match";

    // The `Alloca` exclusion, which is the ONE case where same-opcode +
    // same-type is NOT enough: substituting one distinct stack slot for another
    // flips a Store through either from Yes (Rule 1) to No (Rule 2).
    MirBuilder mb2;
    mb2.addFunction(fnSig, SymbolId{101});
    MirBlockId const e2 = mb2.createBlock(StructCfMarker::EntryBlock);
    mb2.beginBlock(e2);
    MirInstId const slotP = mb2.addInst(MirOpcode::Alloca, {}, ptrI32);
    MirInstId const slotQ = mb2.addInst(MirOpcode::Alloca, {}, ptrI32);
    mb2.addReturn();
    Mir mir2 = std::move(mb2).finish();
    EXPECT_TRUE(mirAliasProbeSubstitutionPreservesClobberVerdict(mir2, slotP, slotP))
        << "identity stays licensed even for an Alloca";
    EXPECT_FALSE(mirAliasProbeSubstitutionPreservesClobberVerdict(mir2, slotP, slotQ))
        << "two DISTINCT Allocas share opcode and TypeId, yet the substitution "
           "WEAKENS Rule 1's Yes into Rule 2's No — must be REFUSED";
}

// Half 2 — the THEOREM by exhaustion. Wherever the licence says yes, every
// clobber verdict is bit-identical for the two probes, over EVERY instruction
// in the module × the full StrictTbaa × charTypesAliasAll matrix. The module
// deliberately mixes Allocas (Rule 2), Args of three different pointee types
// (Rules 4/5/6), Geps (neither), Stores through most of them, and an opaque
// non-Store clobber.
// RED-ON-DISABLE, three independent arms:
//   * drop the TypeId comparison  → the Ptr<I32>/Ptr<I64> Arg pair becomes
//     "licensed" and diverges under strict TBAA (No vs Maybe);
//   * drop the opcode comparison  → the Alloca/Arg pair at the same TypeId
//     becomes "licensed" and diverges on a Store through the OTHER Alloca;
//   * drop the `Alloca` exclusion → the slotA/slotB pair becomes "licensed"
//     and diverges on a Store through either of them (Rule 1 Yes vs Rule 2 No)
//     — the only direction that WEAKENS a clobber verdict.
TEST(MirAlias, ProbeSubstitutionPreservesEveryClobberVerdict) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const i64    = interner.primitive(TypeKind::I64);
    TypeId const charT  = interner.primitive(TypeKind::Char);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const ptrI64 = interner.pointer(i64);
    TypeId const ptrCh  = interner.pointer(charT);
    TypeId const voidT  = interner.primitive(TypeKind::Void);
    TypeId const params[] = {ptrI32, ptrI64, ptrCh, ptrI32};
    TypeId const fnSig  = interner.fnSig(params, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const aI32  = mb.addArg(0, ptrI32);
    MirInstId const aI64  = mb.addArg(1, ptrI64);
    MirInstId const aCh   = mb.addArg(2, ptrCh);
    MirInstId const aI32b = mb.addArg(3, ptrI32);
    MirInstId const slotA = mb.addInst(MirOpcode::Alloca, {}, ptrI32);
    MirInstId const slotB = mb.addInst(MirOpcode::Alloca, {}, ptrI32);
    MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(z, i32);
    MirInstId const gops[] = {slotA, c0};
    MirInstId const gepA = mb.addInst(MirOpcode::Gep, gops, ptrI32);
    MirInstId const gepB = mb.addInst(MirOpcode::Gep, gops, ptrI32);
    MirLiteralValue z64; z64.value = std::int64_t{0}; z64.core = TypeKind::I64;
    MirInstId const c64 = mb.addConst(z64, i64);
    MirLiteralValue zc; zc.value = std::int64_t{0}; zc.core = TypeKind::Char;
    MirInstId const cch = mb.addConst(zc, charT);
    for (auto const& st : {std::array<MirInstId, 2>{c0, slotA},
                           std::array<MirInstId, 2>{c0, slotB},
                           std::array<MirInstId, 2>{c0, aI32},
                           std::array<MirInstId, 2>{c64, aI64},
                           std::array<MirInstId, 2>{cch, aCh},
                           std::array<MirInstId, 2>{c0, aI32b},
                           std::array<MirInstId, 2>{c0, gepA},
                           std::array<MirInstId, 2>{c0, gepB}}) {
        (void)mb.addInst(MirOpcode::Store, st, InvalidType);
    }
    (void)mb.addInst(MirOpcode::CompilerBarrier, {}, InvalidType);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    std::vector<MirInstId> everyInst;
    std::uint32_t const n = mir.blockInstCount(entry);
    for (std::uint32_t i = 0; i < n; ++i) everyInst.push_back(mir.blockInstAt(entry, i));

    MirInstId const probes[] = {aI32, aI64, aCh, aI32b, slotA, slotB, gepA, gepB};
    struct FlagCase { StrictTbaa st; bool ca; };
    FlagCase const flagMatrix[] = {
        {StrictTbaa::No,  true}, {StrictTbaa::No,  false},
        {StrictTbaa::Yes, true}, {StrictTbaa::Yes, false},
    };
    std::size_t licensedPairs = 0;
    for (MirInstId const raw : probes) {
        for (MirInstId const canonical : probes) {
            if (!mirAliasProbeSubstitutionPreservesClobberVerdict(
                    mir, raw, canonical)) {
                continue;
            }
            ++licensedPairs;
            for (auto const [st, ca] : flagMatrix) {
                for (MirInstId const x : everyInst) {
                    EXPECT_EQ(mirInstClobbersLoadPtr(mir, interner, raw, x, st, ca),
                              mirInstClobbersLoadPtr(mir, interner, canonical, x,
                                                     st, ca))
                        << "the licence claimed probe v=" << raw.v
                        << " may be replaced by v=" << canonical.v
                        << ", but their clobber verdicts differ on inst v="
                        << x.v << " (st=" << (st == StrictTbaa::Yes)
                        << " ca=" << ca << ") — the substitution theorem is "
                           "broken (D-OPT-CSE-LOAD-PTR-KEY-UNRESOLVED)";
                }
            }
        }
    }
    // Non-vacuity: 8 identity pairs are free, so the licence must admit MORE
    // than 8 or the sweep proved nothing about substitution at all. The real
    // ones are the two Arg<Ptr<I32>> orderings and the two Gep orderings; the
    // two Alloca orderings must NOT be among them.
    EXPECT_EQ(licensedPairs, 12u)
        << "expected 8 identity + 2 same-type Arg + 2 Gep pairs, and NO Alloca "
           "pair — a different count means the licence's polarity moved";
}

// D-CSUBSET-ATOMIC-FENCE: the AtomicFence (__sync_synchronize) twin of the
// CompilerBarrier test above — the SAME chokepoint, the SAME doubly-load-bearing
// negative control (a non-aliasing Store + the Return terminator must NOT
// clobber). A standalone CPU fence is a full ordering barrier: the region walk
// must treat it as an opaque clobber even though every Store in the region
// provably does not alias the Load pointer. RED-on-disable: THIS is the test
// that reds when AtomicFence's `opcodeClobbersMemory` membership is dropped
// (mir_opcode.hpp) — without the membership the walk skips the fence (not a
// Store) and the positive half fails; the fence's hasSideEffects flag alone
// canNOT save it (the negative control's terminator proves the walk never keys
// on that flag).
TEST(MirAlias, RegionWalkTreatsAtomicFenceAsClobber) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const ptr   = interner.pointer(i32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    // One builder recipe, two modules: [a, b, store→b] (+ the seq_cst fence in
    // the second). The Load pointer is `a`; the Store writes through `b`
    // (distinct Alloca → Rule 2 says No), so ONLY the fence can clobber.
    auto const build = [&](bool withFence) {
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
        if (withFence) {
            (void)mb.addInst(MirOpcode::AtomicFence, {}, InvalidType,
                             /*payload=*/5);   // seq_cst — the sole producer
        }
        mb.addReturn();
        struct Out { Mir mir; MirInstId loadPtr; MirBlockId block; };
        return Out{std::move(mb).finish(), a, entry};
    };

    auto const clean = build(/*withFence=*/false);
    MirBlockId const cleanRegion[] = {clean.block};
    EXPECT_FALSE(mirAnyMayAliasingStoreInRegion(
        clean.mir, interner, clean.loadPtr, cleanRegion))
        << "negative control: a non-aliasing Store + the block's Return "
           "TERMINATOR (hasSideEffects=true) must NOT clobber — the walk "
           "keys on opcodeClobbersMemory, never the DCE-liveness flag";

    auto const fenced = build(/*withFence=*/true);
    MirBlockId const fencedRegion[] = {fenced.block};
    EXPECT_TRUE(mirAnyMayAliasingStoreInRegion(
        fenced.mir, interner, fenced.loadPtr, fencedRegion))
        << "a standalone CPU fence (AtomicFence) must be an opaque clobber — "
           "Loads may not move across it (D-CSUBSET-ATOMIC-FENCE)";
}

// ═════════════════════════════════════════════════════════════════════
// D-OPT-MEMSSA-WALK-PAST-PRECISION — the PROVENANCE + ESCAPE substrate
// (`opt/analysis/mir_escape.hpp`) and `mirMayAlias` Rule 3b.
//
// Rules 1..7 are opcode and TYPE tests. The pair that dominates real C — a
// local array or struct versus a parameter — has IDENTICAL types, so no type
// test can ever separate it; that is the precision ceiling this substrate
// lifts and the reason the anchor's own trigger ("`mirMayAlias` gains real
// precision (points-to / full TBAA / escape analysis)") now fires.
//
// Every test below carries its own negative pin, because a `No` verdict is the
// one direction that can license a stale Load: the escape half is proven
// load-bearing by an ESCAPING twin that must stay Maybe, and the provenance
// half by an `Unknown`-origin twin that must stay Maybe.
// ═════════════════════════════════════════════════════════════════════

namespace {

// The canonical precision shape, built once and shared by the tests below.
//
//   void probe(int *p) {         // `param`  — External provenance
//       int  keep[4];            // `slot`   — a local slot that NEVER escapes
//       int  leak[4];            // `leaked` — a local slot passed to a callee
//       sink(leak);              // the escape
//       *p = 0;                  // a Store through the parameter
//       keep[0] = 0;             // a Store through the non-escaping slot
//       leak[0] = 0;             // a Store through the escaping slot
//   }
struct EscapeShape {
    Mir       mir;
    MirInstId param{};       // Arg 0, Ptr<I32>
    MirInstId gepParam{};    // Gep(param, 0)
    MirInstId slot{};        // Alloca — never escapes
    MirInstId gepSlot{};     // Gep(slot, 0)
    MirInstId castSlot{};    // Bitcast(gepSlot)
    MirInstId leaked{};      // Alloca — escapes via a Call argument
    MirInstId gepLeaked{};   // Gep(leaked, 0)
    MirInstId storeParam{};
    MirInstId storeSlot{};
    MirInstId storeLeaked{};
    MirInstId call{};
};

EscapeShape buildEscapeShape(TypeInterner& interner) {
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const voidT  = interner.primitive(TypeKind::Void);
    TypeId const params[] = {ptrI32};
    TypeId const fnSig  = interner.fnSig(params, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);

    EscapeShape s{};
    s.param  = mb.addArg(0, ptrI32);
    s.slot   = mb.addInst(MirOpcode::Alloca, {}, ptrI32);
    s.leaked = mb.addInst(MirOpcode::Alloca, {}, ptrI32);
    MirLiteralValue z; z.value = std::int64_t{0}; z.core = TypeKind::I32;
    MirInstId const c0 = mb.addConst(z, i32);

    MirInstId const gp[] = {s.param, c0};
    s.gepParam  = mb.addInst(MirOpcode::Gep, gp, ptrI32);
    MirInstId const gs[] = {s.slot, c0};
    s.gepSlot   = mb.addInst(MirOpcode::Gep, gs, ptrI32);
    MirInstId const bc[] = {s.gepSlot};
    s.castSlot  = mb.addInst(MirOpcode::Bitcast, bc, ptrI32);
    MirInstId const gl[] = {s.leaked, c0};
    s.gepLeaked = mb.addInst(MirOpcode::Gep, gl, ptrI32);

    // THE escape: `leaked`'s address becomes a call argument.
    MirInstId const callee = mb.addGlobalAddr(SymbolId{200}, fnSig);
    MirInstId const callOps[] = {callee, s.gepLeaked};
    s.call = mb.addInst(MirOpcode::Call, callOps, InvalidType);

    MirInstId const st0[] = {c0, s.gepParam};
    s.storeParam  = mb.addInst(MirOpcode::Store, st0, InvalidType);
    MirInstId const st1[] = {c0, s.gepSlot};
    s.storeSlot   = mb.addInst(MirOpcode::Store, st1, InvalidType);
    MirInstId const st2[] = {c0, s.gepLeaked};
    s.storeLeaked = mb.addInst(MirOpcode::Store, st2, InvalidType);
    mb.addReturn();

    s.mir = std::move(mb).finish();
    return s;
}

} // namespace

// Provenance survives address arithmetic and pointer re-typing — the whole
// reason Rule 2 (which compares the raw `Alloca` ids) cannot see the shapes
// real C presents. NEGATIVE PIN: a Gep off a PARAMETER must stay External, or
// the walk would be reporting slot provenance for pointers that have none.
TEST(MirEscape, OriginWalksThroughGepAndBitcastButNotThroughAParameter) {
    TypeInterner interner{CompilationUnitId{1}};
    auto s = buildEscapeShape(interner);
    MirPointerEscape const esc{s.mir};

    for (MirInstId const v : {s.slot, s.gepSlot, s.castSlot}) {
        auto const o = esc.originOf(v);
        EXPECT_EQ(o.kind, MirPointerOriginKind::Slot)
            << "v=" << v.v << " is derived from a local slot by address "
               "arithmetic / a Bitcast — provenance must survive both";
        EXPECT_EQ(o.slot.v, s.slot.v)
            << "v=" << v.v << " must name the SLOT it came from, not another";
    }
    EXPECT_EQ(esc.originOf(s.param).kind, MirPointerOriginKind::External);
    EXPECT_EQ(esc.originOf(s.gepParam).kind, MirPointerOriginKind::External)
        << "a Gep off a parameter carries the PARAMETER's provenance — "
           "reporting Slot here would be an unsound No waiting to happen";
    EXPECT_EQ(esc.originOf(s.gepLeaked).kind, MirPointerOriginKind::Slot);
    EXPECT_EQ(esc.originOf(s.gepLeaked).slot.v, s.leaked.v);
}

// The escape half itself: dereference and address arithmetic CONTAIN a slot
// address; handing it to a callee publishes it. Both polarities in one shape,
// so a table edit that makes every use escaping (or none) reds this test.
TEST(MirEscape, LoadStoreAndGepContainASlotWhileACallArgumentPublishesIt) {
    TypeInterner interner{CompilationUnitId{1}};
    auto s = buildEscapeShape(interner);
    MirPointerEscape const esc{s.mir};

    EXPECT_FALSE(esc.slotEscapes(s.slot))
        << "`keep` is only Gep'd, Bitcast and stored THROUGH — its address "
           "never leaves the activation";
    EXPECT_TRUE(esc.slotEscapes(s.leaked))
        << "`leak`'s address is a Call argument — the callee can keep it";
    EXPECT_EQ(esc.slotCount(), 2u);
    EXPECT_EQ(esc.escapingSlotCount(), 1u)
        << "exactly one of the two slots escapes — if BOTH or NEITHER do, the "
           "shape stopped discriminating and every verdict below is vacuous";
    EXPECT_EQ(esc.frameObservingFunctionCount(), 0u);
}

// ★ THE PRECISION WIN, and the anchor's trigger made concrete: a Store through
// a PARAMETER cannot clobber a Load from a NON-ESCAPED local slot, at
// IDENTICAL pointee types where every type rule answers Maybe.
// RED-ON-DISABLE: delete Rule 3b from `mirMayAlias` (or the `provablyDisjoint`
// arm behind it) and the first EXPECT flips No -> Maybe.
TEST(MirAlias, Rule3b_NonEscapingSlotVersusAParameterIsNo) {
    TypeInterner interner{CompilationUnitId{1}};
    auto s = buildEscapeShape(interner);
    MirPointerEscape const esc{s.mir};

    for (StrictTbaa const st : {StrictTbaa::No, StrictTbaa::Yes}) {
        for (bool const ca : {false, true}) {
            EXPECT_EQ(mirMayAlias(s.mir, interner, s.gepSlot, s.gepParam,
                                  st, ca, &esc),
                      MirAliasResult::No)
                << "a non-escaped local slot and a parameter can never overlap";
            // NEGATIVE PIN 1 — the substrate is OPT-IN and sound when omitted.
            EXPECT_EQ(mirMayAlias(s.mir, interner, s.gepSlot, s.gepParam, st, ca),
                      MirAliasResult::Maybe)
                << "without the escape analysis the SAME pair must fall to the "
                   "type rules' Maybe — a No here would mean Rule 3b leaked "
                   "into the default path";
            // NEGATIVE PIN 2 — the ESCAPE test is load-bearing, not decoration.
            EXPECT_EQ(mirMayAlias(s.mir, interner, s.gepLeaked, s.gepParam,
                                  st, ca, &esc),
                      MirAliasResult::Maybe)
                << "`leak` ESCAPED into a callee, so the parameter may well "
                   "name it — Rule 3b must NOT fire";
        }
    }
    // The clobber predicate is the surface consumers actually use.
    EXPECT_FALSE(mirInstClobbersLoadPtr(s.mir, interner, s.gepSlot,
                                        s.storeParam, StrictTbaa::No, true, &esc));
    EXPECT_TRUE(mirInstClobbersLoadPtr(s.mir, interner, s.gepSlot, s.storeParam));
    EXPECT_TRUE(mirInstClobbersLoadPtr(s.mir, interner, s.gepLeaked,
                                       s.storeParam, StrictTbaa::No, true, &esc));
    // An opaque clobber stays opaque at ANY provenance precision — Rule 3b is
    // reached only through the Store arm of the predicate.
    EXPECT_TRUE(mirInstClobbersLoadPtr(s.mir, interner, s.gepSlot, s.call,
                                       StrictTbaa::No, true, &esc))
        << "a Call is an opaque memory clobber; provenance never excuses it";
}

// Arm (i): two DIFFERENT slots reached through address arithmetic. This is
// Rule 2 generalized — Rule 2 compares raw `Alloca` ids and sees only two
// `Gep`s here. NEGATIVE PIN: the SAME slot through two different derivations
// must NOT be disjoint (that pair genuinely may overlap).
TEST(MirAlias, Rule3b_DistinctSlotsThroughGepsAreDisjointButOneSlotIsNot) {
    TypeInterner interner{CompilationUnitId{1}};
    auto s = buildEscapeShape(interner);
    MirPointerEscape const esc{s.mir};

    EXPECT_EQ(mirMayAlias(s.mir, interner, s.gepSlot, s.gepLeaked,
                          StrictTbaa::No, true, &esc),
              MirAliasResult::No)
        << "two different local slots are two different objects even when one "
           "of them escapes — arm (i) does not read the escape bit";
    EXPECT_EQ(mirMayAlias(s.mir, interner, s.gepSlot, s.gepLeaked),
              MirAliasResult::Maybe)
        << "Rule 2 cannot see through the Geps; only Rule 3b can";
    EXPECT_FALSE(esc.provablyDisjoint(s.gepSlot, s.castSlot))
        << "gepSlot and castSlot name the SAME slot — claiming disjointness "
           "here would license a stale Load";
    EXPECT_FALSE(esc.provablyDisjoint(s.gepSlot, s.gepSlot))
        << "a pointer is never disjoint from itself";
}

// A Phi that MERGES slot provenance with a parameter is the lattice's top, and
// top must never yield a No. Also pins the second half: the merged value
// leaving the activation escapes the slot that fed it, so the slot's OWN
// pointers stop being separable from the parameter too.
TEST(MirAlias, Rule3b_PhiMergingASlotAndAParameterIsUnknownAndEscapesTheSlot) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const boolT  = interner.primitive(TypeKind::Bool);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const voidT  = interner.primitive(TypeKind::Void);
    TypeId const params[] = {ptrI32, boolT};
    TypeId const fnSig  = interner.fnSig(params, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const armA  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const armB  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const join  = mb.createBlock(StructCfMarker::ExitBlock);
    mb.beginBlock(entry);
    MirInstId const param = mb.addArg(0, ptrI32);
    MirInstId const cond  = mb.addArg(1, boolT);
    MirInstId const slot  = mb.addInst(MirOpcode::Alloca, {}, ptrI32);
    (void)mb.addCondBr(cond, armA, armB);
    mb.beginBlock(armA);
    (void)mb.addBr(join);
    mb.beginBlock(armB);
    (void)mb.addBr(join);
    mb.beginBlock(join);
    MirPhiIncoming const incs[] = {{slot, armA}, {param, armB}};
    MirInstId const merged = mb.addPhi(ptrI32, incs);
    // The merged pointer LEAVES: it becomes a call argument.
    MirInstId const callee = mb.addGlobalAddr(SymbolId{200}, fnSig);
    MirInstId const callOps[] = {callee, merged};
    (void)mb.addInst(MirOpcode::Call, callOps, InvalidType);
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    MirPointerEscape const esc{mir};
    EXPECT_EQ(esc.originOf(merged).kind, MirPointerOriginKind::Unknown)
        << "Slot join External is the lattice's TOP — a Phi that may carry "
           "either must not be reported as either";
    EXPECT_EQ(mirMayAlias(mir, interner, merged, param,
                          StrictTbaa::No, true, &esc),
              MirAliasResult::Maybe)
        << "an Unknown origin can never produce a No";
    EXPECT_TRUE(esc.slotEscapes(slot))
        << "the merged pointer left the activation and it MAY be the slot's "
           "address — the slot must be treated as escaped";
    EXPECT_EQ(mirMayAlias(mir, interner, slot, param,
                          StrictTbaa::No, true, &esc),
              MirAliasResult::Maybe)
        << "once the slot escapes, the parameter may name it";
}

// The frame-observing hatch. An assembly template can read the frame pointer
// and compute ANY slot address with no SSA edge to observe, so a function
// containing one keeps exactly the pre-Rule-3b precision. NEGATIVE PIN: the
// identical shape WITHOUT the template separates the pair.
TEST(MirEscape, AnInlineAsmTemplateEscapesEverySlotInItsFunction) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const voidT  = interner.primitive(TypeKind::Void);
    TypeId const params[] = {ptrI32};
    TypeId const fnSig  = interner.fnSig(params, voidT, CallConv::CcSysV);

    struct Shape { Mir mir; MirInstId param, slot; };
    auto build = [&](bool withAsm) {
        MirBuilder mb;
        mb.addFunction(fnSig, SymbolId{100});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(entry);
        MirInstId const param = mb.addArg(0, ptrI32);
        MirInstId const slot  = mb.addInst(MirOpcode::Alloca, {}, ptrI32);
        if (withAsm) {
            MirAsmDescriptor d;
            d.templateText = "nop";
            (void)mb.addInlineAsm(std::move(d), {}, InvalidType);
        }
        mb.addReturn();
        return Shape{std::move(mb).finish(), param, slot};
    };

    Shape clean = build(false);
    MirPointerEscape const escClean{clean.mir};
    EXPECT_FALSE(escClean.slotEscapes(clean.slot));
    EXPECT_EQ(escClean.frameObservingFunctionCount(), 0u);
    EXPECT_EQ(mirMayAlias(clean.mir, interner, clean.slot, clean.param,
                          StrictTbaa::No, true, &escClean),
              MirAliasResult::No)
        << "the control arm must separate the pair, or the asm arm below "
           "proves nothing";

    Shape asmed = build(true);
    MirPointerEscape const escAsm{asmed.mir};
    EXPECT_TRUE(escAsm.slotEscapes(asmed.slot))
        << "an opaque template may read the frame pointer and name any slot";
    EXPECT_EQ(escAsm.frameObservingFunctionCount(), 1u);
    EXPECT_EQ(mirMayAlias(asmed.mir, interner, asmed.slot, asmed.param,
                          StrictTbaa::No, true, &escAsm),
              MirAliasResult::Maybe)
        << "a frame-observing function returns to exactly today's precision";
}

// Rule 3b sits AFTER Rule 3's pointer-ness screen, so a caller exploring
// alias-ness on a NON-pointer SSA value still gets Maybe rather than a No
// derived from a value that does not name storage at all.
TEST(MirAlias, Rule3b_DoesNotFireBehindRule3sPointernessScreen) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32    = interner.primitive(TypeKind::I32);
    TypeId const ptrI32 = interner.pointer(i32);
    TypeId const voidT  = interner.primitive(TypeKind::Void);
    TypeId const fnSig  = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, ptrI32);
    MirLiteralValue z; z.value = std::int64_t{7}; z.core = TypeKind::I32;
    MirInstId const scalar = mb.addConst(z, i32);   // NOT pointer-typed
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    MirPointerEscape const esc{mir};
    EXPECT_FALSE(esc.slotEscapes(slot));
    EXPECT_EQ(mirMayAlias(mir, interner, slot, scalar,
                          StrictTbaa::No, true, &esc),
              MirAliasResult::Maybe)
        << "Rule 3 screens non-pointer operands FIRST; Rule 3b must not "
           "answer a question about a value that names no storage";
}

// ── Completeness of the two classification tables, CHECKED not argued ──
//
// Both tables are whitelists whose DEFAULT arm is the conservative answer, so
// an opcode added to the enum without touching mir_escape.hpp loses precision
// and can never gain an unsound No. These sweeps assert exactly that shape
// over the WHOLE `MirOpcode` enum, so the property is a test result rather
// than a promise in a comment.
TEST(MirEscape, EveryUseOutsideTheWhitelistIsClassifiedEscapes) {
    std::size_t nonEscaping = 0;
    std::size_t escaping    = 0;
    for (std::uint16_t raw = 0;
         raw < static_cast<std::uint16_t>(MirOpcode::Count_); ++raw) {
        auto const op = static_cast<MirOpcode>(raw);
        for (std::size_t i = 0; i < 8; ++i) {
            bool const whitelisted =
                (op == MirOpcode::Phi)
                || ((op == MirOpcode::Gep || op == MirOpcode::Bitcast) && i == 0)
                || ((op == MirOpcode::Load || op == MirOpcode::AtomicLoad
                     || op == MirOpcode::AtomicCas) && i == 0)
                || ((op == MirOpcode::Store || op == MirOpcode::AtomicStore) && i == 1)
                || (op == MirOpcode::ICmpEq  || op == MirOpcode::ICmpNe
                 || op == MirOpcode::ICmpSlt || op == MirOpcode::ICmpSle
                 || op == MirOpcode::ICmpSgt || op == MirOpcode::ICmpSge
                 || op == MirOpcode::ICmpUlt || op == MirOpcode::ICmpUle
                 || op == MirOpcode::ICmpUgt || op == MirOpcode::ICmpUge);
            bool const escapes =
                mirPointerUseKind(op, i) == MirPointerUseKind::Escapes;
            EXPECT_EQ(escapes, !whitelisted)
                << "opcode ordinal " << raw << " operand " << i
                << ": every position outside the whitelist MUST classify as "
                   "Escapes — the default arm is what makes an unknown opcode "
                   "lose precision instead of soundness";
            (escapes ? escaping : nonEscaping) += 1;
        }
    }
    EXPECT_GT(nonEscaping, 0u);
    EXPECT_GT(escaping, nonEscaping)
        << "the table must be a WHITELIST — if the non-escaping positions ever "
           "outnumber the escaping ones its polarity has inverted";
}

// ★ The CSE-substitution stability lemma's premise, swept over the whole enum.
//
// `mirAliasProbeSubstitutionPreservesClobberVerdict`'s theorem holds at Rule 3b
// precision only because origin is a function of (opcode, origin of operand 0)
// for every opcode a value-numbering pass may MERGE. The two opcodes for which
// that is false — `Alloca` (origin is its own identity) and `Phi` (origin is a
// join over the phi pool, which no operand list carries) — must therefore be
// excluded from merge candidacy. The condition below MIRRORS `cse.cpp`'s
// `isCseCandidateOpcode`; if that gate is ever relaxed, this sweep is the test
// that reds.
TEST(MirEscape, EveryValueNumberingCandidateHasOperandDeterminedOrigin) {
    std::size_t candidates = 0;
    for (std::uint16_t raw = 0;
         raw < static_cast<std::uint16_t>(MirOpcode::Count_); ++raw) {
        auto const op = static_cast<MirOpcode>(raw);
        if (op == MirOpcode::Invalid) continue;
        // `isCseCandidateOpcode` (cse.cpp), restated from public predicates.
        if (isTerminator(op)) continue;
        if (isPhi(op)) continue;
        if (opcodeInfo(op).hasSideEffects) continue;
        if (op == MirOpcode::Alloca) continue;
        ++candidates;
        EXPECT_TRUE(mirPointerOriginIsOperandDetermined(op))
            << "opcode ordinal " << raw << " is a value-numbering candidate "
               "whose origin is NOT determined by (opcode, operand-0 origin) — "
               "merging two of them could join different provenances behind an "
               "identical key, and Rule 3b would then answer differently for "
               "the raw and the canonical probe";
    }
    EXPECT_GT(candidates, 8u)
        << "the sweep found almost no candidates — the restated gate has "
           "drifted from cse.cpp and this test is asserting nothing";
    EXPECT_FALSE(mirPointerOriginIsOperandDetermined(MirOpcode::Alloca))
        << "Alloca's origin IS its own identity — the lemma's excluded case";
    EXPECT_FALSE(mirPointerOriginIsOperandDetermined(MirOpcode::Phi))
        << "Phi's origin is a join over the phi pool — the lemma's other "
           "excluded case";
}

// The FOUR-argument substitution licence: at Rule 3b precision, equal opcode +
// equal TypeId is no longer sufficient on its own, and passing the escape
// analysis restores the theorem's `==` conclusion. Both halves are pinned:
// the licence REFUSES the differing-origin pair, and wherever it still says
// yes the verdicts are bit-identical over every instruction × the flag matrix.
TEST(MirAlias, ProbeSubstitutionUnderEscapeRequiresEqualOrigins) {
    TypeInterner interner{CompilationUnitId{1}};
    auto s = buildEscapeShape(interner);
    MirPointerEscape const esc{s.mir};

    // `gepSlot` and `gepParam` are both `Gep` at the same result TypeId.
    EXPECT_TRUE(mirAliasProbeSubstitutionPreservesClobberVerdict(
                    s.mir, s.gepSlot, s.gepParam))
        << "the THREE-argument form is the TYPE-level licence and still says "
           "yes here — that is exactly why the four-argument form exists";
    EXPECT_FALSE(mirAliasProbeSubstitutionPreservesClobberVerdict(
                     s.mir, s.gepSlot, s.gepParam, &esc))
        << "their ORIGINS differ (a non-escaped slot vs a parameter), so at "
           "Rule 3b precision the substitution flips a Store's verdict";
    EXPECT_TRUE(mirAliasProbeSubstitutionPreservesClobberVerdict(
                    s.mir, s.gepSlot, s.gepSlot, &esc));

    std::vector<MirInstId> everyInst;
    MirBlockId const entry = s.mir.instBlock(s.slot);
    std::uint32_t const n = s.mir.blockInstCount(entry);
    for (std::uint32_t i = 0; i < n; ++i) {
        everyInst.push_back(s.mir.blockInstAt(entry, i));
    }
    MirInstId const probes[] = {s.param, s.gepParam, s.slot, s.gepSlot,
                                s.castSlot, s.leaked, s.gepLeaked};
    struct FlagCase { StrictTbaa st; bool ca; };
    FlagCase const flagMatrix[] = {
        {StrictTbaa::No,  true}, {StrictTbaa::No,  false},
        {StrictTbaa::Yes, true}, {StrictTbaa::Yes, false},
    };
    std::size_t licensed = 0;
    std::size_t refused  = 0;
    for (MirInstId const raw : probes) {
        for (MirInstId const canonical : probes) {
            if (!mirAliasProbeSubstitutionPreservesClobberVerdict(
                    s.mir, raw, canonical, &esc)) {
                ++refused;
                continue;
            }
            ++licensed;
            for (auto const [st, ca] : flagMatrix) {
                for (MirInstId const x : everyInst) {
                    EXPECT_EQ(mirInstClobbersLoadPtr(s.mir, interner, raw, x,
                                                     st, ca, &esc),
                              mirInstClobbersLoadPtr(s.mir, interner, canonical,
                                                     x, st, ca, &esc))
                        << "the four-argument licence claimed probe v=" << raw.v
                        << " may be replaced by v=" << canonical.v
                        << ", but their clobber verdicts differ on inst v="
                        << x.v << " under Rule 3b";
                }
            }
        }
    }
    EXPECT_GT(refused, 0u)
        << "if the licence refuses nothing it is not discriminating origins";
    EXPECT_GE(licensed, std::size(probes))
        << "identity must always be licensed";
}
