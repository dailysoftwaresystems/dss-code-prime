// HR2: the HirVerifier expression-typing rule — every expression (and TypeRef)
// node must carry a valid TypeId, reported as H_TypeUnresolved. Collect-all,
// recoverable-diagnostic discipline (no abort); HasError nodes are skipped.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/hir_node.hpp"
#include "hir/hir_op.hpp"
#include "hir/hir_verifier.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

using dss::CompilationUnitId;
using dss::DiagnosticCode;
using dss::DiagnosticReporter;
using dss::DiagnosticSeverity;
using dss::encodeOp;
using dss::Hir;
using dss::HirBuilder;
using dss::HirFlags;
using dss::HirKind;
using dss::HirKindId;
using dss::HirNodeId;
using dss::HirOpKind;
using dss::HirVerifier;
using dss::ParseDiagnostic;
using dss::TypeId;
using dss::TypeInterner;
using dss::TypeKind;

namespace {

TypeInterner makeInterner() { return TypeInterner{CompilationUnitId{1}}; }

std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode c) {
    std::size_t n = 0;
    for (ParseDiagnostic const& d : r.all()) {
        if (d.code == c) ++n;
    }
    return n;
}

} // namespace

TEST(HirVerifier, FullyTypedModulePassesClean) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);

    HirBuilder b{"toy"};
    HirNodeId const lhs = b.makeLiteral(i32, 1);
    HirNodeId const rhs = b.makeLiteral(i32, 2);
    HirNodeId const add = b.makeBinaryOp(HirOpKind::Add, lhs, rhs, i32);
    HirNodeId const ret = b.addParent(HirKind::ReturnStmt, std::array{add});
    HirNodeId const blk = b.addParent(HirKind::Block, std::array{ret});
    HirNodeId const mod = b.addParent(HirKind::Module, std::array{blk});
    Hir h = std::move(b).finish(mod);

    DiagnosticReporter reporter;
    EXPECT_TRUE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(reporter.errorCount(), 0u);
}

TEST(HirVerifier, UntypedExpressionFiresTypeUnresolved) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);

    HirBuilder b{"toy"};
    HirNodeId const lhs = b.makeLiteral(i32);
    HirNodeId const rhs = b.makeLiteral(i32);
    // An untyped BinaryOp — built through the raw addParent (the typed helper
    // can't express it). InvalidType is the default.
    HirNodeId const bad = b.addParent(HirKind::BinaryOp, std::array{lhs, rhs},
                                      dss::InvalidType, encodeOp(HirOpKind::Add));
    Hir h = std::move(b).finish(bad);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_TypeUnresolved), 1u);
    // The offending node id is stashed in the span offset (HR2 stand-in until
    // HR5 threads real source spans).
    ASSERT_FALSE(reporter.all().empty());
    EXPECT_EQ(reporter.all().front().span.start(), bad.v);
}

TEST(HirVerifier, UntypedTypeRefFires) {
    HirBuilder b{"toy"};
    HirNodeId const tr = b.addLeaf(HirKind::TypeRef, dss::InvalidType);
    Hir h = std::move(b).finish(tr);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_TypeUnresolved), 1u);
}

TEST(HirVerifier, HasErrorNodeIsSkipped) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);

    HirBuilder b{"toy"};
    HirNodeId const lhs = b.makeLiteral(i32);
    HirNodeId const rhs = b.makeLiteral(i32);
    // Untyped BUT flagged HasError — cascade suppression: the missing type is a
    // downstream effect of an already-reported fault, so the verifier stays quiet.
    HirNodeId const bad = b.addParent(HirKind::BinaryOp, std::array{lhs, rhs},
                                      dss::InvalidType, encodeOp(HirOpKind::Add),
                                      HirFlags::HasError);
    Hir h = std::move(b).finish(bad);

    DiagnosticReporter reporter;
    EXPECT_TRUE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_TypeUnresolved), 0u);
}

TEST(HirVerifier, UntypedStatementDoesNotFire) {
    // VarDecl / Block are not expression kinds — an InvalidType on them is
    // legitimate and must not trip the expression-typing rule.
    HirBuilder b{"toy"};
    HirNodeId const vd  = b.addLeaf(HirKind::VarDecl);
    HirNodeId const blk = b.addParent(HirKind::Block, std::array{vd});
    Hir h = std::move(b).finish(blk);

    DiagnosticReporter reporter;
    EXPECT_TRUE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(reporter.errorCount(), 0u);
}

TEST(HirVerifier, UntypedExtensionNodeIsSkipped) {
    // An Extension node is NOT type-required (requiresValidType(Extension) ==
    // false — value-ness lives in the descriptor, not the core predicate), so an
    // untyped one must pass cleanly rather than trip H_TypeUnresolved.
    HirBuilder b{"L"};
    HirKindId const widget = b.registry().registerExtension("L::Widget", "L");
    HirNodeId const ext = b.addLeaf(HirKind::Extension, dss::InvalidType, widget.v);
    Hir h = std::move(b).finish(ext);

    DiagnosticReporter reporter;
    EXPECT_TRUE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(reporter.errorCount(), 0u);
}

TEST(HirVerifier, RefusesToCertifyCleanWhenReporterIsCapped) {
    // A reporter capped by a prior phase silently drops further report() calls,
    // so the error-count delta can't prove "no violation" — verify() must NOT
    // hand back a false all-clear even for a genuinely clean module.
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 2;
    DiagnosticReporter reporter{cfg};
    for (int i = 0; i < 3; ++i) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::H_TypeUnresolved;
        d.severity = DiagnosticSeverity::Error;
        d.span     = dss::SourceSpan::empty(static_cast<dss::ByteOffset>(i + 1));
        reporter.report(std::move(d));
    }
    ASSERT_TRUE(reporter.hitCap());

    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirBuilder b{"toy"};
    HirNodeId const lit = b.makeLiteral(i32);   // a perfectly clean, typed module
    Hir h = std::move(b).finish(lit);

    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
}

TEST(HirVerifier, EveryUntypedExpressionIsReportedNotCoalesced) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);

    HirBuilder b{"toy"};
    HirNodeId const a = b.makeLiteral(i32);
    HirNodeId const c = b.makeLiteral(i32);
    HirNodeId const d = b.makeLiteral(i32);
    HirNodeId const e = b.makeLiteral(i32);
    HirNodeId const bad1 = b.addParent(HirKind::BinaryOp, std::array{a, c},
                                       dss::InvalidType, encodeOp(HirOpKind::Add));
    HirNodeId const bad2 = b.addParent(HirKind::BinaryOp, std::array{d, e},
                                       dss::InvalidType, encodeOp(HirOpKind::Sub));
    HirNodeId const blk = b.addParent(HirKind::Block, std::array{bad1, bad2});
    Hir h = std::move(b).finish(blk);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    // Distinct span offsets (each node's id) keep the reporter's dedup window
    // from collapsing the two violations into one.
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_TypeUnresolved), 2u);
}
