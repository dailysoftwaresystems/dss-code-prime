// HR2: the HirVerifier expression-typing rule — every expression (and TypeRef)
// node must carry a valid TypeId, reported as H_TypeUnresolved. Collect-all,
// recoverable-diagnostic discipline (no abort); HasError nodes are skipped.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/hir_node.hpp"
#include "hir/hir_op.hpp"
#include "hir/hir_verifier.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <type_traits>

using dss::CompilationUnitId;
using dss::DiagnosticCode;
using dss::DiagnosticReporter;
using dss::DiagnosticSeverity;
using dss::encodeOp;
using dss::ForClause;
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

// The verifier holds a reference into the module, so it must not bind to a
// temporary `Hir` in EITHER arity — the HR5 source-map overload's defaulted
// parameter must not reopen the rvalue hole the single-arg deletion closes.
static_assert(std::is_constructible_v<HirVerifier, Hir const&>);
static_assert(!std::is_constructible_v<HirVerifier, Hir&&>);
static_assert(!std::is_constructible_v<HirVerifier, Hir&&, dss::HirSourceMap const*>);

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
    // No source map was supplied, so the diagnostic carries an honest "no
    // location" (InvalidBuffer + empty span) and the node identity lives in the
    // message text — NOT smuggled into the span offset.
    ASSERT_FALSE(reporter.all().empty());
    EXPECT_EQ(reporter.all().front().buffer, dss::InvalidBuffer);
    EXPECT_TRUE(reporter.all().front().span.isEmpty());
    EXPECT_NE(reporter.all().front().actual.find("#" + std::to_string(bad.v)),
              std::string::npos);
}

TEST(HirVerifier, MapsRealSourceSpanWhenSourceMapProvided) {
    // HR5: when a HirSourceMap is supplied, a violating node's diagnostic carries
    // its real (buffer, span) — the IOU HR2 left ("until HR5 threads real source
    // spans") is now closed.
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);

    HirBuilder b{"toy"};
    HirNodeId const lhs = b.makeLiteral(i32);
    HirNodeId const rhs = b.makeLiteral(i32);
    HirNodeId const bad = b.addParent(HirKind::BinaryOp, std::array{lhs, rhs},
                                      dss::InvalidType, encodeOp(HirOpKind::Add));
    Hir h = std::move(b).finish(bad);

    dss::HirSourceMap spans{h};
    dss::BufferId const buf{7};
    dss::SourceSpan const sp = dss::SourceSpan::of(10, 20);
    spans.set(bad, dss::HirSourceLoc{buf, sp});

    DiagnosticReporter reporter;
    EXPECT_FALSE((HirVerifier{h, &spans}.verify(reporter)));
    ASSERT_FALSE(reporter.all().empty());
    EXPECT_EQ(reporter.all().front().buffer, buf);
    EXPECT_EQ(reporter.all().front().span, sp);
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
    // Block / ReturnStmt are not type-required — an InvalidType on them is
    // legitimate and must not trip the expression-typing rule.
    HirBuilder b{"toy"};
    HirNodeId const ret = b.makeReturn();                 // bare return; (no value)
    HirNodeId const blk = b.makeBlock(std::array{ret});
    Hir h = std::move(b).finish(blk);

    DiagnosticReporter reporter;
    EXPECT_TRUE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(reporter.errorCount(), 0u);
}

TEST(HirVerifier, UntypedVarDeclFiresTypeUnresolved) {
    // VarDecl now carries its declared type (HR3) and is type-required: a
    // typeless VarDecl can't lower to a sized alloca, so the verifier flags it.
    HirBuilder b{"toy"};
    HirNodeId const vd  = b.makeVarDecl(dss::InvalidType, /*symbol=*/1);  // no type
    HirNodeId const blk = b.makeBlock(std::array{vd});
    Hir h = std::move(b).finish(blk);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_TypeUnresolved), 1u);
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
    // With no source map the two violations share the empty span, but each
    // carries its own node id in `actual` — and `actual` is part of the dedup
    // key — so the window does NOT collapse the two distinct findings into one.
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_TypeUnresolved), 2u);
}

// ── per-kind child-arity rule (H_VerifierFailure) ──

TEST(HirVerifier, WrongFixedArityFires) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);

    HirBuilder b{"toy"};
    // A BinaryOp (arity 2) built — via the raw API — with one child. Typed, so
    // ONLY the arity rule fires (not H_TypeUnresolved).
    HirNodeId const a   = b.makeLiteral(i32);
    HirNodeId const bad = b.addParent(HirKind::BinaryOp, std::array{a}, i32,
                                      encodeOp(HirOpKind::Add));
    Hir h = std::move(b).finish(bad);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_VerifierFailure), 1u);
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_TypeUnresolved), 0u);
}

TEST(HirVerifier, ForStmtChildCountMustMatchClauseMask) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    // Claim Init present (mask bit 0) but supply only the body child — the mask
    // implies 2 children (init + body), so the count disagrees.
    HirNodeId const body = b.makeBlock({});
    HirNodeId const bad  = b.addParent(HirKind::ForStmt, std::array{body}, dss::InvalidType,
                                       static_cast<std::uint32_t>(ForClause::Init));
    (void)i32;
    Hir h = std::move(b).finish(bad);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_VerifierFailure), 1u);
}

TEST(HirVerifier, ForStmtPayloadBitsOutsideMaskFire) {
    HirBuilder b{"toy"};
    HirNodeId const body = b.makeBlock({});
    // payload has a stray high bit beyond the 3 clause bits.
    HirNodeId const bad = b.addParent(HirKind::ForStmt, std::array{body}, dss::InvalidType,
                                      /*payload=*/0b1000u);
    Hir h = std::move(b).finish(bad);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_VerifierFailure), 1u);
}

TEST(HirVerifier, ValuedCaseArmMissingValueFires) {
    HirBuilder b{"toy"};
    // A CaseArm with NO default flag (so it's a valued arm) but zero children —
    // it has no match-value child.
    HirNodeId const bad = b.addParent(HirKind::CaseArm, {}, dss::InvalidType, /*payload=*/0);
    Hir h = std::move(b).finish(bad);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_VerifierFailure), 1u);
}

TEST(HirVerifier, WellFormedControlFlowProducesNoArityFailure) {
    // False-positive guard: a valid for + switch (valued + default arms) module
    // built through the typed helpers must produce ZERO H_VerifierFailure — the
    // dynamic ForStmt/CaseArm checks must not misfire on correct shapes.
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const boolT = ti.primitive(TypeKind::Bool);

    HirNodeId const arm0 = b.makeCaseArm(b.makeLiteral(i32),
                                         std::array{b.makeExprStmt(b.makeLiteral(i32))});
    HirNodeId const arm1 = b.makeCaseArm(std::nullopt,
                                         std::array{b.makeExprStmt(b.makeLiteral(i32))});
    HirNodeId const sw   = b.makeSwitchStmt(b.makeRef(i32, 1), std::array{arm0, arm1});
    HirNodeId const f    = b.makeForStmt(b.makeVarDecl(i32, 2, b.makeLiteral(i32)),
                                         b.makeLiteral(boolT), std::nullopt,
                                         b.makeBlock(std::array{sw}));
    Hir h = std::move(b).finish(f);

    DiagnosticReporter reporter;
    EXPECT_TRUE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_VerifierFailure), 0u);
}

// ── break/continue scoping rule (H_InvalidBreak) ──

namespace {

// while (true) { <stmts> } as a freestanding module; returns the finished Hir.
Hir whileWith(HirBuilder& b, std::span<HirNodeId const> stmts) {
    TypeInterner ti = makeInterner();
    TypeId const boolT = ti.primitive(TypeKind::Bool);
    HirNodeId const cond = b.makeLiteral(boolT);
    HirNodeId const body = b.makeBlock(stmts);
    HirNodeId const wh   = b.makeWhileStmt(cond, body);
    return std::move(b).finish(wh);
}

} // namespace

TEST(HirVerifier, BreakInsideLoopIsValid) {
    HirBuilder b{"toy"};
    HirNodeId const br = b.makeBreak(0);
    Hir h = whileWith(b, std::array{br});

    DiagnosticReporter reporter;
    EXPECT_TRUE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(reporter.errorCount(), 0u);
}

TEST(HirVerifier, BreakIndexOutOfRangeFires) {
    HirBuilder b{"toy"};
    HirNodeId const br = b.makeBreak(1);     // only one enclosing loop (index 0)
    Hir h = whileWith(b, std::array{br});

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_InvalidBreak), 1u);
}

TEST(HirVerifier, BreakWithNoEnclosingTargetFires) {
    HirBuilder b{"toy"};
    HirNodeId const br  = b.makeBreak(0);
    HirNodeId const blk = b.makeBlock(std::array{br});   // no loop/switch at all
    Hir h = std::move(b).finish(blk);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_InvalidBreak), 1u);
}

TEST(HirVerifier, BreakTargetingSwitchIsValid) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    // switch (x) { default: break; } — break CAN target a switch (unlike continue).
    HirNodeId const br   = b.makeBreak(0);
    HirNodeId const arm  = b.makeCaseArm(std::nullopt, std::array{br});
    HirNodeId const disc = b.makeRef(i32, 1);
    HirNodeId const sw   = b.makeSwitchStmt(disc, std::array{arm});
    Hir h = std::move(b).finish(sw);

    DiagnosticReporter reporter;
    EXPECT_TRUE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(reporter.errorCount(), 0u);
}

TEST(HirVerifier, ContinueAtTopLevelFires) {
    HirBuilder b{"toy"};
    HirNodeId const co  = b.makeContinue(0);             // no enclosing loop at all
    HirNodeId const blk = b.makeBlock(std::array{co});
    Hir h = std::move(b).finish(blk);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_InvalidBreak), 1u);
}

TEST(HirVerifier, ContinueInsideLoopIsValid) {
    HirBuilder b{"toy"};
    HirNodeId const co = b.makeContinue(0);
    Hir h = whileWith(b, std::array{co});

    DiagnosticReporter reporter;
    EXPECT_TRUE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(reporter.errorCount(), 0u);
}

TEST(HirVerifier, ContinueTargetingSwitchFires) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    // switch (x) { default: continue; }  — continue 0 resolves to the switch.
    HirNodeId const co   = b.makeContinue(0);
    HirNodeId const arm  = b.makeCaseArm(std::nullopt, std::array{co});
    HirNodeId const disc = b.makeRef(i32, 1);
    HirNodeId const sw   = b.makeSwitchStmt(disc, std::array{arm});
    Hir h = std::move(b).finish(sw);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_InvalidBreak), 1u);
}

TEST(HirVerifier, ContinueSkippingSwitchToOuterLoopIsValid) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const boolT = ti.primitive(TypeKind::Bool);
    // while (..) { switch (x) { default: continue 1; } } — index 1 skips the
    // switch (target 0) and resolves to the while (target 1), a loop: valid.
    HirNodeId const co   = b.makeContinue(1);
    HirNodeId const arm  = b.makeCaseArm(std::nullopt, std::array{co});
    HirNodeId const disc = b.makeRef(i32, 1);
    HirNodeId const sw   = b.makeSwitchStmt(disc, std::array{arm});
    HirNodeId const body = b.makeBlock(std::array{sw});
    HirNodeId const cond = b.makeLiteral(boolT);
    HirNodeId const wh   = b.makeWhileStmt(cond, body);
    Hir h = std::move(b).finish(wh);

    DiagnosticReporter reporter;
    EXPECT_TRUE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(reporter.errorCount(), 0u);
}

TEST(HirVerifier, NestedBreakToOuterLoopIsValid) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const boolT = ti.primitive(TypeKind::Bool);
    // outer while { inner while { break 1 } } — break 1 leaves the outer loop.
    HirNodeId const br    = b.makeBreak(1);
    HirNodeId const inBody = b.makeBlock(std::array{br});
    HirNodeId const inCond = b.makeLiteral(boolT);
    HirNodeId const inner  = b.makeWhileStmt(inCond, inBody);
    HirNodeId const outBody = b.makeBlock(std::array{inner});
    HirNodeId const outCond = b.makeLiteral(boolT);
    HirNodeId const outer   = b.makeWhileStmt(outCond, outBody);
    Hir h = std::move(b).finish(outer);

    DiagnosticReporter reporter;
    EXPECT_TRUE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(reporter.errorCount(), 0u);
}

// ── declaration shape rule (H_VerifierFailure) + declaration typing ──

namespace {
// A valid one-param int->int FnSig for the declaration tests.
TypeId intToIntSig(TypeInterner& ti) {
    TypeId const i32 = ti.primitive(TypeKind::I32);
    return ti.fnSig(std::array{i32}, i32, dss::CallConv::CcSysV);
}
} // namespace

TEST(HirVerifier, FunctionBodyMustBeBlock) {
    HirBuilder b{"c"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    // Last child is an ExprStmt, not a Block — checkDeclarationShape fires.
    HirNodeId const notBody = b.makeExprStmt(b.makeLiteral(i32));
    HirNodeId const fn = b.makeFunction(intToIntSig(ti), 1, {}, notBody);
    Hir h = std::move(b).finish(fn);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_VerifierFailure), 1u);
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_TypeUnresolved), 0u);
}

TEST(HirVerifier, FunctionParamMustBeVarDeclWithoutInit) {
    HirBuilder b{"c"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    // A non-VarDecl "parameter" (a typed Literal) + a VarDecl param WITH an init.
    HirNodeId const badParam  = b.makeLiteral(i32);                        // not a VarDecl
    HirNodeId const initParam = b.makeVarDecl(i32, 9, b.makeLiteral(i32)); // has initializer
    HirNodeId const body = b.makeBlock({});
    HirNodeId const fn = b.makeFunction(intToIntSig(ti), 1,
                                        std::array{badParam, initParam}, body);
    Hir h = std::move(b).finish(fn);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_VerifierFailure), 2u);  // both params
}

TEST(HirVerifier, FunctionWithBadParamAndBadBodyReportsBoth) {
    // COLLECT-ALL guard: a non-VarDecl param AND a non-Block body must each be
    // reported (the rule must not short-circuit after the first violation).
    HirBuilder b{"c"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirNodeId const badParam = b.makeLiteral(i32);            // not a VarDecl
    HirNodeId const notBody  = b.makeExprStmt(b.makeLiteral(i32));  // not a Block
    HirNodeId const fn = b.makeFunction(intToIntSig(ti), 1, std::array{badParam}, notBody);
    Hir h = std::move(b).finish(fn);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_VerifierFailure), 2u);
}

TEST(HirVerifier, ExternFunctionNonVarDeclParamFires) {
    HirBuilder b{"c"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    // An extern "param" that is a typed Literal (not a Block, not a VarDecl) —
    // exercises checkParam's non-VarDecl arm on the ExternFunction path.
    HirNodeId const badParam = b.makeLiteral(i32);
    HirNodeId const ef = b.makeExternFunction(intToIntSig(ti), 6, std::array{badParam});
    Hir h = std::move(b).finish(ef);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_VerifierFailure), 1u);
}

TEST(HirVerifier, ExternFunctionMustNotHaveBody) {
    HirBuilder b{"c"};
    TypeInterner ti = makeInterner();
    HirNodeId const body = b.makeBlock({});   // an extern must not carry a body
    HirNodeId const ef = b.makeExternFunction(intToIntSig(ti), 6, std::array{body});
    Hir h = std::move(b).finish(ef);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_VerifierFailure), 1u);
}

TEST(HirVerifier, WellFormedFunctionAndExternAreClean) {
    HirBuilder b{"c"};
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = intToIntSig(ti);
    HirNodeId const fn = b.makeFunction(fnTy, 1, std::array{b.makeVarDecl(i32, 10)},
                                        b.makeBlock({}));
    HirNodeId const ef = b.makeExternFunction(fnTy, 6, std::array{b.makeVarDecl(i32, 11)});
    HirNodeId const mod = b.makeModule(std::array{fn, ef});
    Hir h = std::move(b).finish(mod);

    DiagnosticReporter reporter;
    EXPECT_TRUE(HirVerifier{h}.verify(reporter));
    EXPECT_EQ(reporter.errorCount(), 0u);
}

TEST(HirVerifier, UntypedSourceDeclarationsFireButExternsDoNot) {
    HirBuilder b{"c"};
    // Function/Global/TypeDecl are type-required; Extern* are not.
    HirNodeId const fn  = b.makeFunction(dss::InvalidType, 1, {}, b.makeBlock({}));
    HirNodeId const g   = b.makeGlobal(dss::InvalidType, 2);
    HirNodeId const td  = b.makeTypeDecl(dss::InvalidType, 3);
    HirNodeId const ef  = b.makeExternFunction(dss::InvalidType, 6, {});   // OK untyped
    HirNodeId const eg  = b.makeExternGlobal(dss::InvalidType, 7);          // OK untyped
    HirNodeId const mod = b.makeModule(std::array{fn, g, td, ef, eg});
    Hir h = std::move(b).finish(mod);

    DiagnosticReporter reporter;
    EXPECT_FALSE(HirVerifier{h}.verify(reporter));
    // Exactly the three source declarations fire; the two externs do not.
    EXPECT_EQ(countCode(reporter, DiagnosticCode::H_TypeUnresolved), 3u);
}
