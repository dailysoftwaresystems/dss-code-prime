// ML4 — MIR `.dssir` text format round-trip tests.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_text.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

using namespace dss;

namespace {

MirLiteralValue intLit(std::int64_t v, TypeKind core = TypeKind::I32) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = core;
    return lit;
}

// Drive: build a MIR via MirBuilder + a real TypeInterner, emit to
// text, parse back, re-emit, assert byte-equality.
struct RoundTripResult {
    std::string firstEmit;
    std::string secondEmit;
    bool        parseOk = false;
};

RoundTripResult roundTrip(Mir const& mir, TypeInterner const& interner,
                          std::vector<std::string> const& names) {
    DiagnosticReporter r1, r2, r3;
    MirTextContext ctx{&interner, &names};
    std::string first = emitMir(mir, ctx, r1);
    auto parsed = parseMir(first, CompilationUnitId{1}, r2);
    MirTextContext ctx2{&parsed->interner, &parsed->symbolNames};
    std::string second = emitMir(parsed->mir, ctx2, r3);
    return {std::move(first), std::move(second), parsed->ok};
}

} // namespace

TEST(MirText, EmptyModuleRoundTrips) {
    Mir m;
    TypeInterner ti{CompilationUnitId{1}};
    DiagnosticReporter r;
    MirTextContext ctx{&ti};
    std::string out = emitMir(m, ctx, r);
    EXPECT_NE(out.find("dssir 1"), std::string::npos);
    EXPECT_NE(out.find("module {"), std::string::npos);
}

TEST(MirText, MinimalFunctionEmitsHeaderAndFunction) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const voidTy = ti.primitive(TypeKind::Void);
    TypeId const fnSig  = ti.fnSig(std::span<TypeId const>{}, voidTy, CallConv::CcSysV);
    MirBuilder b;
    MirFuncId const f = b.addFunction(fnSig, SymbolId{1});
    (void)f;
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    b.addReturn();
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "main"};
    DiagnosticReporter r;
    MirTextContext ctx{&ti, &names};
    std::string out = emitMir(m, ctx, r);
    EXPECT_NE(out.find("function %1"), std::string::npos);
    EXPECT_NE(out.find("entry"), std::string::npos);
    EXPECT_NE(out.find("return"), std::string::npos);
}

TEST(MirText, StraightLineRoundTripsToByteEqual) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const fnSig = ti.fnSig(std::span<TypeId const>{}, i32, CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const c1 = b.addConst(intLit(40, TypeKind::I32), i32);
    MirInstId const c2 = b.addConst(intLit(2,  TypeKind::I32), i32);
    std::array<MirInstId, 2> const ops{c1, c2};
    MirInstId const sum = b.addInst(MirOpcode::Add, ops, i32);
    b.addReturn(sum);
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "answer"};
    auto rt = roundTrip(m, ti, names);
    EXPECT_TRUE(rt.parseOk);
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
}

TEST(MirText, DiamondWithPhiRoundTrips) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32    = ti.primitive(TypeKind::I32);
    TypeId const boolTy = ti.primitive(TypeKind::Bool);
    TypeId const fnSig  = ti.fnSig(std::span<TypeId const>{}, i32, CallConv::CcSysV);
    (void)boolTy;
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tBB   = b.createBlock(StructCfMarker::IfThen);
    MirBlockId const eBB   = b.createBlock(StructCfMarker::IfElse);
    MirBlockId const join  = b.createBlock(StructCfMarker::IfJoin);
    b.beginBlock(entry);
    MirInstId const c1 = b.addConst(intLit(1, TypeKind::Bool), boolTy);
    b.addCondBr(c1, tBB, eBB);
    b.beginBlock(tBB);
    MirInstId const ct = b.addConst(intLit(10), i32);
    b.addBr(join);
    b.beginBlock(eBB);
    MirInstId const ce = b.addConst(intLit(20), i32);
    b.addBr(join);
    b.beginBlock(join);
    std::array<MirPhiIncoming, 2> const incs{
        MirPhiIncoming{ct, tBB}, MirPhiIncoming{ce, eBB}};
    MirInstId const phi = b.addPhi(i32, incs);
    b.addReturn(phi);
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "diamond"};
    auto rt = roundTrip(m, ti, names);
    EXPECT_TRUE(rt.parseOk);
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
}

TEST(MirText, GlobalWithLiteralInitRoundTrips) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32 = ti.primitive(TypeKind::I32);
    MirBuilder b;
    MirBuilder b2;
    // Need a literal pool entry for the constant-init global.
    std::uint32_t const litIdx = b.literalPoolAdd(intLit(42, TypeKind::I32));
    b.addGlobal(i32, SymbolId{10}, litIdx);
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "", "", "", "", "", "", "", "", "", "g"};
    auto rt = roundTrip(m, ti, names);
    EXPECT_TRUE(rt.parseOk);
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
    EXPECT_NE(rt.firstEmit.find("global %10"), std::string::npos);
    EXPECT_NE(rt.firstEmit.find("lit int 42"), std::string::npos);
}

TEST(MirText, MissingVersionEmitsVersionMismatch) {
    DiagnosticReporter r;
    auto res = parseMir("dssir 999\nmodule { }\n", CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.code == DiagnosticCode::I_TextVersionMismatch) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(MirText, MalformedHeaderEmitsMalformedDiagnostic) {
    DiagnosticReporter r;
    auto res = parseMir("not-a-header\n", CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.code == DiagnosticCode::I_TextMalformed) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(MirText, EmptyModuleParseRoundTripsToEmpty) {
    DiagnosticReporter r;
    auto res = parseMir("dssir 1\nmodule { }\n", CompilationUnitId{1}, r);
    EXPECT_TRUE(res->ok);
    EXPECT_EQ(res->mir.moduleFuncCount(), 0u);
}

// Switch terminator round-trip — case/default arrow parsing was wrong
// in cycle 1 (used Minus instead of Arrow); this test pins the fix.
TEST(MirText, SwitchTerminatorRoundTrips) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const fnSig = ti.fnSig(std::span<TypeId const>{}, i32, CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const c0    = b.createBlock(StructCfMarker::SwitchCase);
    MirBlockId const c1    = b.createBlock(StructCfMarker::SwitchCase);
    MirBlockId const def   = b.createBlock(StructCfMarker::SwitchCase);
    MirBlockId const join  = b.createBlock(StructCfMarker::SwitchJoin);
    b.beginBlock(entry);
    MirInstId const disc = b.addConst(intLit(1), i32);
    MirInstId const v0   = b.addConst(intLit(0), i32);
    MirInstId const v1   = b.addConst(intLit(1), i32);
    std::array<std::pair<MirInstId, MirBlockId>, 2> const cases{
        std::pair<MirInstId, MirBlockId>{v0, c0},
        std::pair<MirInstId, MirBlockId>{v1, c1}};
    b.addSwitch(disc, cases, def);
    b.beginBlock(c0);  b.addBr(join);
    b.beginBlock(c1);  b.addBr(join);
    b.beginBlock(def); b.addBr(join);
    b.beginBlock(join); b.addReturn(disc);
    Mir m = std::move(b).finish();
    std::vector<std::string> names{"", "sw"};
    auto rt = roundTrip(m, ti, names);
    EXPECT_TRUE(rt.parseOk);
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
}

// Loop round-trip — LoopHeader / LoopExit markers must survive
// (the cycle-1 emitter rendered them but had no test).
TEST(MirText, LoopRoundTrips) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32    = ti.primitive(TypeKind::I32);
    TypeId const boolTy = ti.primitive(TypeKind::Bool);
    TypeId const fnSig  = ti.fnSig(std::span<TypeId const>{}, i32, CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry  = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const header = b.createBlock(StructCfMarker::LoopHeader);
    MirBlockId const body   = b.createBlock(StructCfMarker::Linear);
    MirBlockId const exit   = b.createBlock(StructCfMarker::LoopExit);
    b.beginBlock(entry);  b.addBr(header);
    b.beginBlock(header);
    MirInstId const c1 = b.addConst(intLit(1, TypeKind::Bool), boolTy);
    b.addCondBr(c1, body, exit);
    b.beginBlock(body);   b.addBr(header);  // back-edge
    b.beginBlock(exit);
    MirInstId const rv = b.addConst(intLit(0), i32);
    b.addReturn(rv);
    Mir m = std::move(b).finish();
    std::vector<std::string> names{"", "loop"};
    auto rt = roundTrip(m, ti, names);
    EXPECT_TRUE(rt.parseOk);
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
}

// Aggregate literal round-trip — `MirAggregateValue` nested rendering
// (ML2 cycle 6 const-fold output for struct literals).
TEST(MirText, AggregateLiteralRoundTrips) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32   = ti.primitive(TypeKind::I32);
    std::array<TypeId, 2> const fields{i32, i32};
    TypeId const pointTy = ti.structType("Point", fields);
    TypeId const fnSig   = ti.fnSig(std::span<TypeId const>{}, pointTy, CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    // Build an aggregate literal: { 1, 2 } typed as Struct.
    MirAggregateValue agg;
    agg.fields.push_back(intLit(1));
    agg.fields.push_back(intLit(2));
    MirLiteralValue aggLit;
    aggLit.core  = TypeKind::Struct;
    aggLit.value = std::move(agg);
    MirInstId const cv = b.addConst(std::move(aggLit), pointTy);
    b.addReturn(cv);
    Mir m = std::move(b).finish();
    std::vector<std::string> names{"", "makePoint"};
    auto rt = roundTrip(m, ti, names);
    EXPECT_TRUE(rt.parseOk);
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
}

// Pointer + Array type round-trip.
TEST(MirText, PointerAndArrayTypesRoundTrip) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32      = ti.primitive(TypeKind::I32);
    TypeId const ptrI32   = ti.pointer(i32);
    TypeId const arrI32x4 = ti.array(i32, 4);
    std::array<TypeId, 2> const params{ptrI32, arrI32x4};
    TypeId const fnSig    = ti.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const c = b.addConst(intLit(7), i32);
    b.addReturn(c);
    Mir m = std::move(b).finish();
    std::vector<std::string> names{"", "f"};
    auto rt = roundTrip(m, ti, names);
    EXPECT_TRUE(rt.parseOk);
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
    EXPECT_NE(rt.firstEmit.find("ptr<i32>"), std::string::npos);
    EXPECT_NE(rt.firstEmit.find("arr<i32, 4>"), std::string::npos);
}

// Symbol-name table with unnamed symbol — fallback to bare `%N` quote
// must still round-trip.
// A malformed numeric literal in an `int` literal must emit
// I_TextMalformed (was silently zero in cycle 1). The text
// `lit int abc` has a non-numeric token where an integer is
// expected; the new parseNumber<T> helper catches this.
TEST(MirText, MalformedNumericLiteralEmitsDiagnostic) {
    // Build a malformed body. The lexer will tokenize `abc` as Ident,
    // not Integer, so the parser's `lit int` branch will fail at the
    // `lex_.take()` of the value token. But for any tokenized-as-
    // Integer-but-out-of-range case (e.g. 9999999999999999999 as int32),
    // parseNumber emits the malformed diagnostic. Use that case:
    std::string text =
        "dssir 1\n"
        "symbols { %1 \"f\" }\n"
        "module {\n"
        "  function %1 : fn() -> i32 {\n"
        "    block %b1 [entry] {\n"
        "      %v2 = const : i32 (lit int 999999999999999999999 : i32)\n"
        "      return %v2\n"
        "    }\n"
        "  }\n"
        "}\n";
    DiagnosticReporter r;
    auto res = parseMir(text, CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok);
    bool foundMalformed = false;
    for (auto const& d : r.all()) {
        if (d.code == DiagnosticCode::I_TextMalformed
         && d.actual.find("int literal") != std::string::npos) {
            foundMalformed = true; break;
        }
    }
    EXPECT_TRUE(foundMalformed)
        << "out-of-range int literal must emit I_TextMalformed";
}

TEST(MirText, UnnamedSymbolRoundTrips) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const voidTy = ti.primitive(TypeKind::Void);
    TypeId const fnSig  = ti.fnSig(std::span<TypeId const>{}, voidTy, CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{42});  // sym 42, no name supplied
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    b.addReturn();
    Mir m = std::move(b).finish();
    std::vector<std::string> emptyNames;  // no names at all
    auto rt = roundTrip(m, ti, emptyNames);
    EXPECT_TRUE(rt.parseOk);
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
    EXPECT_NE(rt.firstEmit.find("%42 \"\""), std::string::npos);
}
