// HR1: the HIR node vocabulary — open-core invariant, flag algebra, POD layout.

#include "hir/hir_node.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

using dss::ForClause;
using dss::HirFlags;
using dss::HirKind;
using dss::detail::HirNode;

// ── open-core invariant (compile-time) ──
static_assert(static_cast<std::uint32_t>(HirKind::Count_) < 256,
              "core HirKind must fit in [0, 256)");
static_assert(dss::kFirstHirExtensionKind == 256);
// Error is the POD default kind (a default-constructed node is a recovery
// sentinel, never silently a valid Module/Literal).
static_assert(HirKind::Error != HirKind::Module);

// ── requiresValidType classification (compile-time) ──
// The predicate the HR2 verifier sweeps with: expressions + TypeRef carry a
// resolved result type; declarations/statements/sentinels do not.
static_assert(dss::requiresValidType(HirKind::Literal));
static_assert(dss::requiresValidType(HirKind::BinaryOp));
static_assert(dss::requiresValidType(HirKind::TypeRef));
// VarDecl carries its DECLARED type (HR3) — the verifier requires it, since a
// typeless decl can't lower to a sized alloca and HIR is detached from semantic.
static_assert(dss::requiresValidType(HirKind::VarDecl));
static_assert(!dss::requiresValidType(HirKind::Block));
static_assert(!dss::requiresValidType(HirKind::Module));
static_assert(!dss::requiresValidType(HirKind::ReturnStmt));
static_assert(!dss::requiresValidType(HirKind::Error));
static_assert(!dss::requiresValidType(HirKind::Unreachable));
// Pinned deliberately: an Extension node's value-ness lives in its descriptor,
// unknown to the core predicate — so it must NOT be type-required here. If this
// ever flips, an untyped Extension node would wrongly trip H_TypeUnresolved.
static_assert(!dss::requiresValidType(HirKind::Extension));
// HR4 declarations: source-defined decls are type-required; externs are not
// (binary-only FFI ingestion can lack a resolved type), nor are Module/ImportGroup.
static_assert(dss::requiresValidType(HirKind::Function));
static_assert(dss::requiresValidType(HirKind::Global));
static_assert(dss::requiresValidType(HirKind::TypeDecl));
static_assert(!dss::requiresValidType(HirKind::ExternFunction));
static_assert(!dss::requiresValidType(HirKind::ExternGlobal));
static_assert(!dss::requiresValidType(HirKind::Module));
static_assert(!dss::requiresValidType(HirKind::ImportGroup));

// ── childArity single-source-of-truth (compile-time) ──
static_assert(dss::childArity(HirKind::BinaryOp).min == 2 && dss::childArity(HirKind::BinaryOp).max == 2);
static_assert(dss::childArity(HirKind::Ternary).min == 3 && dss::childArity(HirKind::Ternary).max == 3);
static_assert(dss::childArity(HirKind::IfStmt).min == 2 && dss::childArity(HirKind::IfStmt).max == 3);
static_assert(dss::childArity(HirKind::BreakStmt).min == 0 && dss::childArity(HirKind::BreakStmt).max == 0);
static_assert(dss::childArity(HirKind::ExprStmt).min == 1 && dss::childArity(HirKind::ExprStmt).max == 1);
static_assert(dss::childArity(HirKind::Block).max == dss::kUnboundedArity);          // variadic
// HR4 declarations:
static_assert(dss::childArity(HirKind::Function).min == 1
              && dss::childArity(HirKind::Function).max == dss::kUnboundedArity);     // params… + body
static_assert(dss::childArity(HirKind::Global).min == 0 && dss::childArity(HirKind::Global).max == 1);
static_assert(dss::childArity(HirKind::TypeDecl).min == 0 && dss::childArity(HirKind::TypeDecl).max == 0);
static_assert(dss::childArity(HirKind::ExternGlobal).min == 0 && dss::childArity(HirKind::ExternGlobal).max == 0);
static_assert(dss::childArity(HirKind::ExternFunction).max == dss::kUnboundedArity);  // params…, no body
static_assert(dss::childArity(HirKind::Module).max == dss::kUnboundedArity);          // decls…

// ── structured-CF kind predicates (compile-time) ──
static_assert(dss::isLoopKind(HirKind::WhileStmt) && dss::isLoopKind(HirKind::ForStmt));
static_assert(!dss::isLoopKind(HirKind::SwitchStmt));               // switch is not a loop
static_assert(dss::isBranchTargetKind(HirKind::SwitchStmt));        // but break can target it
static_assert(!dss::isBranchTargetKind(HirKind::IfStmt));

// ── POD layout (compile-time) ──
// Parent links live in a parallel array in `Hir` (not in the POD) so the
// scan-hot kind/type sweeps stay dense in cache; the 32-byte budget reflects
// that.
static_assert(std::is_trivially_copyable_v<HirNode>);
static_assert(sizeof(HirNode) <= 32, "HirNode layout budget is 32 bytes");

TEST(HirNode, DefaultIsErrorRecoverySentinel) {
    HirNode n;
    EXPECT_EQ(n.kind, HirKind::Error);
    EXPECT_EQ(n.flags, HirFlags::None);
    EXPECT_FALSE(n.typeId.valid());
    EXPECT_EQ(n.childStart, 0u);
    EXPECT_EQ(n.childCount, 0u);
    EXPECT_EQ(n.payload, 0u);
}

TEST(HirFlags, BitwiseAlgebra) {
    HirFlags f = HirFlags::HasError | HirFlags::ShaderUsable;
    EXPECT_TRUE(any(f));
    EXPECT_TRUE(has(f, HirFlags::HasError));
    EXPECT_TRUE(has(f, HirFlags::ShaderUsable));
    EXPECT_FALSE(has(f, HirFlags::Synthetic));
    EXPECT_FALSE(has(f, HirFlags::HostUsable));
    EXPECT_TRUE(dss::hasError(f));

    // None is the identity / empty set.
    EXPECT_FALSE(any(HirFlags::None));
    EXPECT_FALSE(dss::hasError(HirFlags::None));

    // |= accumulates; & masks; ~ complements.
    f |= HirFlags::Synthetic;
    EXPECT_TRUE(has(f, HirFlags::Synthetic));
    HirFlags only = f & HirFlags::HasError;
    EXPECT_EQ(only, HirFlags::HasError);
    EXPECT_FALSE(has(~HirFlags::HasError, HirFlags::HasError));
}

TEST(HirKind, ExtensionMarkerIsACoreMember) {
    // The Extension marker itself is a CORE kind (< 256); the concrete extension
    // kind it stands in for is a registry id >= 256, carried elsewhere.
    EXPECT_LT(static_cast<std::uint32_t>(HirKind::Extension), 256u);
}

TEST(ForClause, MaskHelpers) {
    ForClause const m = ForClause::Init | ForClause::Update;  // init + update, no cond
    EXPECT_TRUE(has(m, ForClause::Init));
    EXPECT_FALSE(has(m, ForClause::Cond));
    EXPECT_TRUE(has(m, ForClause::Update));
    EXPECT_EQ(clauseCount(m), 2u);
    EXPECT_EQ(clauseCount(ForClause::None), 0u);
    EXPECT_EQ(clauseCount(ForClause::Init | ForClause::Cond | ForClause::Update), 3u);
}

TEST(HirKind, RequiresValidTypeCoversTheWholeExpressionGroup) {
    // All 17 expression kinds (plan §2.2) require a resolved type.
    for (HirKind k : {HirKind::Literal, HirKind::Ref, HirKind::Call,
                      HirKind::IntrinsicCall, HirKind::BinaryOp, HirKind::UnaryOp,
                      HirKind::Cast, HirKind::MemberAccess, HirKind::Index,
                      HirKind::Swizzle, HirKind::ConstructAggregate, HirKind::Ternary,
                      HirKind::LogicalAnd, HirKind::LogicalOr, HirKind::SizeOf,
                      HirKind::AddressOf, HirKind::Deref}) {
        EXPECT_TRUE(dss::requiresValidType(k))
            << "expression kind ordinal " << static_cast<unsigned>(k);
    }
    // A sampling of kinds that must NOT require a type (Module + extern decls
    // are untyped; Function/Global/TypeDecl ARE type-required as of HR4).
    for (HirKind k : {HirKind::Module, HirKind::ExternFunction, HirKind::ExternGlobal,
                      HirKind::ImportGroup, HirKind::IfStmt, HirKind::WhileStmt,
                      HirKind::AssignStmt, HirKind::ExprStmt, HirKind::Extension,
                      HirKind::Error}) {
        EXPECT_FALSE(dss::requiresValidType(k))
            << "non-type-required kind ordinal " << static_cast<unsigned>(k);
    }
}
