// TypeInterner::representationType — the OBJECT-REPRESENTATION projection
// (D-CSUBSET-NULLPTR-T-DECLARABLE).
//
// ★ WHY THIS FILE LIVES UNDER tests/hir/ AND NOT BESIDE THE OTHER LATTICE TESTS.
// The projection is a lattice query, but it exists for exactly one consumer: the
// semantic→HIR boundary in `cst_to_hir`, which is the tier that must never let a
// `nullptr_t` through as itself. Its sibling pin
// (`HirLoweringC.NullptrTObjectIsProjectedToItsPointerRepresentation`) drives the
// same property through real C source; this one drives the FORMS that source
// cannot reach yet.
//
// ★★ AND THAT IS THE POINT OF THE FILE, NOT AN ASIDE. Bar §A.5's multi-form rule:
// a green suite over a SUBSET of the forms of a "apply X at every form" contract is
// not proof, and an unconsumed substrate's misses are latent BY DEFINITION — the
// test has to CONSTRUCT the consuming shape rather than wait for a front end to
// produce it. ✔MEASURED at 301e2a63: `typeof(<literal>)` — the only spelling C has
// for `nullptr_t`, since `nullptr` is a literal — resolves at the semantic tier
// ONLY for a plain scalar local with an initializer. `typeof(nullptr) *p`,
// `typeof(nullptr) a[3]`, a `typeof(nullptr)` parameter, a `typeof(nullptr)` return
// type and a `typeof(nullptr)` struct member ALL fail before HIR for a reason that
// has nothing to do with `nullptr_t`
// (D-CSUBSET-TYPEOF-VALUE-FORM-RESOLVES-ONLY-FOR-AN-OBJECT-OPERAND). So the Ptr /
// Array / FnSig / qualified arms of the projection have NO reachable C witness
// today, and would ship entirely unexercised without this file.
//
// RED-ON-DISABLE (REMOVE direction): make the `NullptrT` arm of
// `TypeInterner::representationType` return `id` — every EXPECT below that names a
// projected type reds. Narrower mutants: delete the `Ptr` arm (only
// `PointerToNullptrTIsRebuilt` reds), the `Array` arm, the `FnSig` arm, or the
// qualifier peel at the top (only `QualifiedNullptrTKeepsItsQualifierBits` reds) —
// four disjoint reds, which is what proves the arms are independently live rather
// than one arm carrying the file.

#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

using namespace dss;

namespace {

// `void *` — the representation C23 §6.2.5 assigns to `nullptr_t`.
[[nodiscard]] TypeId voidPtr(TypeInterner& ti) {
    return ti.pointer(ti.primitive(TypeKind::Void));
}

} // namespace

// The base case, and the one every other case is built out of.
TEST(TypeRepresentationProjection, NullptrTProjectsToVoidPointer) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const nptr = ti.primitive(TypeKind::NullptrT);
    TypeId const got  = ti.representationType(nptr);

    EXPECT_EQ(got.v, voidPtr(ti).v)
        << "C23 6.2.5: nullptr_t has the size, alignment and representation of "
           "`void *`";
    // ⚠ A PROJECTION, NOT A MERGE. If these ever became one TypeId, `_Generic`
    // could no longer tell `nullptr_t` from `void *` — which gcc 13.3.0 and
    // clang 18.1.3 both can (✔MEASURED: both select the nullptr_t arm).
    EXPECT_NE(nptr.v, got.v)
        << "the semantic IDENTITY must stay distinct from the representation";
    EXPECT_EQ(ti.kind(nptr), TypeKind::NullptrT)
        << "projecting must not mutate the source type";
}

// IDENTITY IS THE COMMON CASE, and it must be identity in the strong sense —
// the same TypeId back, with nothing interned — because that is what makes
// applying this query at a type-entry point a behaviour-preserving edit for
// every program that never mentions `nullptr_t`.
TEST(TypeRepresentationProjection, EveryOtherTypeIsReturnedUnchanged) {
    TypeInterner ti{CompilationUnitId{1}};
    std::array<TypeId, 8> const subjects{
        ti.primitive(TypeKind::I32),
        ti.primitive(TypeKind::F64),
        ti.primitive(TypeKind::Bool),
        ti.primitive(TypeKind::Void),
        ti.pointer(ti.primitive(TypeKind::Char)),
        ti.array(ti.primitive(TypeKind::I16), 4),
        ti.fnSig(std::array{ti.primitive(TypeKind::I32)},
                 ti.primitive(TypeKind::I64), CallConv::CcSysV),
        ti.volatileQualified(ti.primitive(TypeKind::U8)),
    };
    std::size_t const before = ti.size();
    for (TypeId const s : subjects) {
        EXPECT_EQ(ti.representationType(s).v, s.v)
            << "identity is representation for every kind but NullptrT";
    }
    EXPECT_EQ(ti.size(), before)
        << "the identity path must not intern anything -- a projection that "
           "rebuilds unchanged types would churn TypeIds for every program";
}

// `nullptr_t *` — a pointer to the object type. Legal C23 and ✔MEASURED accepted
// and run by gcc 13.3.0 (-std=c2x) and clang 18.1.3 (-std=c23).
TEST(TypeRepresentationProjection, PointerToNullptrTIsRebuilt) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const nptr = ti.primitive(TypeKind::NullptrT);
    TypeId const got  = ti.representationType(ti.pointer(nptr));

    EXPECT_EQ(got.v, ti.pointer(voidPtr(ti)).v) << "nullptr_t * -> void **";
    // Two levels, to prove the walk recurses rather than peeling once.
    EXPECT_EQ(ti.representationType(ti.pointer(ti.pointer(nptr))).v,
              ti.pointer(ti.pointer(voidPtr(ti))).v)
        << "nullptr_t ** -> void ***";
}

// `nullptr_t [3]`, plus the two NEGATIVE array lengths. The sentinels are the
// reason the length travels verbatim instead of being read as a count: -1 is a
// flexible array member and -2 a VLA, and turning either into a 0-element array
// would silently change the type's kind.
TEST(TypeRepresentationProjection, ArrayOfNullptrTKeepsItsLengthSentinel) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const nptr = ti.primitive(TypeKind::NullptrT);

    EXPECT_EQ(ti.representationType(ti.array(nptr, 3)).v,
              ti.array(voidPtr(ti), 3).v);

    TypeId const fam = ti.representationType(ti.incompleteArray(nptr));
    EXPECT_TRUE(ti.isIncompleteArray(fam))
        << "a flexible array member must stay incomplete after projection";
    EXPECT_EQ(ti.operands(fam)[0].v, voidPtr(ti).v);

    TypeId const vla = ti.representationType(ti.vlaArray(nptr));
    EXPECT_TRUE(ti.isVlaArray(vla))
        << "a variable-length array must stay a VLA after projection";
    EXPECT_EQ(ti.operands(vla)[0].v, voidPtr(ti).v);
}

// A function taking and returning `nullptr_t` — the shape behind
// `static int f(typeof(nullptr) p)` and `static typeof(nullptr) r(void)`, the two
// cases that aborted the compiler at 301e2a63.
TEST(TypeRepresentationProjection, FnSigParamsAndResultAreBothProjected) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const nptr = ti.primitive(TypeKind::NullptrT);
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const vp   = voidPtr(ti);

    // Result only.
    EXPECT_EQ(ti.representationType(
                  ti.fnSig(std::array{i32}, nptr, CallConv::CcSysV)).v,
              ti.fnSig(std::array{i32}, vp, CallConv::CcSysV).v);
    // Param only, and NOT the first param — a projection that stopped at
    // operand 0 would stay green on a one-param signature.
    EXPECT_EQ(ti.representationType(
                  ti.fnSig(std::array{i32, nptr}, i32, CallConv::CcSysV)).v,
              ti.fnSig(std::array{i32, vp}, i32, CallConv::CcSysV).v);

    // The CALLING CONVENTION and the VARIADIC flag must survive: they live in the
    // FnSig's scalar pool, and rebuilding through the 3-arg overload (or with a
    // hard-coded cc) would silently retarget the call.
    TypeId const variadic =
        ti.fnSig(std::array{nptr}, i32, CallConv::CcMS64, /*isVariadic=*/true);
    TypeId const projected = ti.representationType(variadic);
    EXPECT_EQ(projected.v,
              ti.fnSig(std::array{vp}, i32, CallConv::CcMS64, true).v);
    EXPECT_TRUE(ti.fnIsVariadic(projected)) << "the variadic flag must survive";
    EXPECT_EQ(ti.scalars(projected)[0], ti.scalars(variadic)[0])
        << "the calling convention must survive";
}

// `volatile nullptr_t` / `_Atomic nullptr_t`. `kind()` sees THROUGH a qualifier
// skin, so the projection has to peel and re-apply it explicitly; forgetting the
// re-apply would drop the qualifier — a loss-of-volatility miscompile arriving
// through the type system rather than through an access site.
TEST(TypeRepresentationProjection, QualifiedNullptrTKeepsItsQualifierBits) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const nptr = ti.primitive(TypeKind::NullptrT);

    TypeId const vol = ti.representationType(ti.volatileQualified(nptr));
    EXPECT_TRUE(ti.isVolatileQualified(vol));
    EXPECT_EQ(ti.stripVolatile(vol).v, voidPtr(ti).v);

    TypeId const both =
        ti.representationType(ti.atomicQualified(ti.volatileQualified(nptr)));
    EXPECT_TRUE(ti.isVolatileQualified(both));
    EXPECT_TRUE(ti.isAtomicQualified(both))
        << "BOTH bits of the qualifier mask must survive, not just the one the "
           "re-apply happened to name";
    EXPECT_EQ(ti.stripVolatile(both).v, voidPtr(ti).v);
}

// The projection STOPS at a nominal boundary. Rebuilding `struct S` with a
// projected member would mint a DIFFERENT struct — nominal identity is part of a
// composite's content — so a `nullptr_t` member keeps its declared type on the
// composite and is projected where the member is ACCESSED. Nothing is lost: the
// layout tier already sizes the member as a pointer.
TEST(TypeRepresentationProjection, NominalCompositesAreNotRebuilt) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const nptr = ti.primitive(TypeKind::NullptrT);
    TypeId const s =
        ti.structType("S", std::array{ti.primitive(TypeKind::I32), nptr});

    EXPECT_EQ(ti.representationType(s).v, s.v)
        << "a struct is returned unchanged -- projecting its fields would mint a "
           "different nominal type";
    EXPECT_EQ(ti.operands(s)[1].v, nptr.v)
        << "the member keeps its declared type; the ACCESS projects it";
    // A pointer TO that struct is likewise unchanged (the recursion reaches the
    // nominal boundary and stops there, rather than rebuilding through it).
    EXPECT_EQ(ti.representationType(ti.pointer(s)).v, ti.pointer(s).v);
}

// An INVALID type passes straight through. Every declared type this query is
// applied to may be `InvalidType` (the semantic tier could not resolve it), and
// an unguarded lattice read of one aborts the process
// ("TypeInterner::get: TypeId out of range") rather than failing loud.
TEST(TypeRepresentationProjection, InvalidTypePassesThrough) {
    TypeInterner ti{CompilationUnitId{1}};
    EXPECT_FALSE(ti.representationType(InvalidType).valid());
}
