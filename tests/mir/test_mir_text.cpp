// ML4 — MIR `.dssir` text format round-trip tests.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_asm_descriptor.hpp"
#include "mir/mir_text.hpp"

#include <gtest/gtest.h>

#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    b.addGlobal(i32, SymbolId{10}, litIdx, MirFuncId{}, SymbolBinding::Global,
                SymbolVisibility::Default, /*isConst=*/false,
                MirThreadStorage::Shared);
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
// A function missing its opening `{` used to cascade every subsequent
// token as a malformed diagnostic. Now the parser bails out of the
// function on the missing LBrace; only ONE I_TextMalformed per
// catastrophic structural boundary fires (plus the version/header
// diagnostics from the rest of the input).
TEST(MirText, MissingFunctionLBraceDoesNotCascade) {
    std::string text =
        "dssir 1\n"
        "symbols { %1 \"f\" }\n"
        "module {\n"
        "  function %1 : fn() -> i32\n"   // <-- missing `{`
        "    block %b1 [entry] { return }\n"
        "  }\n"
        "}\n";
    DiagnosticReporter r;
    auto res = parseMir(text, CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok);
    // Count I_TextMalformed diagnostics. Before the fix this was
    // ≥ 5 (cascade). After the fix it should be small (one per
    // structural boundary hit by the malformed input).
    std::size_t nMalformed = 0;
    for (auto const& d : r.all()) {
        if (d.code == DiagnosticCode::I_TextMalformed) ++nMalformed;
    }
    EXPECT_LE(nMalformed, 3u)
        << "missing LBrace cascaded into " << nMalformed << " diagnostics";
}

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

// ── TF-C78 (D-CSUBSET-NOINLINE): per-FUNCTION attributes survive the
// text round-trip ────────────────────────────────────────────────────
//
// ★ THIS TEST EXISTS BECAUSE THE ROUND-TRIP USED TO SILENTLY LOSE DATA.
// Before this cycle `emitFunction` printed only `function %sym : <type> {` and
// `parseFunction` called the 2-arg `addFunction`, so EVERY function came back
// (Global, Default): a `static` function re-read as externally visible, a
// `weak` one as strong. Adding `noInline` beside those without fixing them
// would have reproduced the identical defect one field over.
//
// ★ AND NOTE WHY THE EXISTING `roundTrip` HELPER COULD NOT HAVE CAUGHT IT.
// That helper asserts emit→parse→emit BYTE-EQUALITY, which is vacuous for a
// field that was never printed in the first place: both emits omitted it, both
// matched, the suite stayed green. So these assertions read the PARSED MODULE's
// accessors directly rather than comparing text — the only formulation that can
// observe a dropped field.
//
// RED-ON-DISABLE: delete any of the three arguments at `parseFunction`'s
// `addFunction` call (or the matching arm in `appendFuncAttrs`) and the
// corresponding EXPECT below fails. The all-default function pins the other
// direction — the attribute list must be OMITTED when nothing is set, so an
// unconditional emit that changed every existing golden text is caught here.
TEST(MirText, FunctionAttributesSurviveRoundTrip) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const voidTy = ti.primitive(TypeKind::Void);
    TypeId const fnSig  = ti.fnSig(std::span<TypeId const>{}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    // %1 — noinline only (Global/Default otherwise): isolates the new bit.
    (void)b.addFunction(fnSig, SymbolId{1}, SymbolBinding::Global,
                        SymbolVisibility::Default, /*noInline=*/true);
    MirBlockId e1 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e1); b.addReturn();
    // %2 — the three axes at once, each non-default.
    (void)b.addFunction(fnSig, SymbolId{2}, SymbolBinding::Local,
                        SymbolVisibility::Hidden, /*noInline=*/true);
    MirBlockId e2 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e2); b.addReturn();
    // %3 — weak, NOT noinline: proves the flags are independent, not one bit.
    (void)b.addFunction(fnSig, SymbolId{3}, SymbolBinding::Weak,
                        SymbolVisibility::Default, /*noInline=*/false);
    MirBlockId e3 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e3); b.addReturn();
    // %4 — everything default: must round-trip with NO attribute list.
    (void)b.addFunction(fnSig, SymbolId{4});
    MirBlockId e4 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e4); b.addReturn();
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "ni", "localhidden", "weakfn", "plain"};
    DiagnosticReporter r1, r2;
    MirTextContext ctx{&ti, &names};
    std::string const text = emitMir(m, ctx, r1);
    auto parsed = parseMir(text, CompilationUnitId{1}, r2);
    ASSERT_NE(parsed, nullptr);
    ASSERT_TRUE(parsed->ok) << text;
    ASSERT_EQ(parsed->mir.moduleFuncCount(), 4u);

    // Read the PARSED module's per-function metadata back, keyed by symbol so
    // the assertions do not depend on arena ordering.
    auto findBySym = [&](std::uint32_t sym) -> MirFuncId {
        std::size_t const nf = parsed->mir.moduleFuncCount();
        for (std::uint32_t i = 0; i < nf; ++i) {
            MirFuncId const f = parsed->mir.funcAt(i);
            if (parsed->mir.funcSymbol(f).v == sym) return f;
        }
        return MirFuncId{};
    };

    MirFuncId const f1 = findBySym(1);
    ASSERT_TRUE(f1.valid());
    EXPECT_TRUE(parsed->mir.funcNoInline(f1))
        << "a noinline function must come back noinline — a dropped flag here "
           "makes the parsed module freely inlinable";
    EXPECT_EQ(parsed->mir.funcBinding(f1), SymbolBinding::Global);

    MirFuncId const f2 = findBySym(2);
    ASSERT_TRUE(f2.valid());
    EXPECT_EQ(parsed->mir.funcBinding(f2), SymbolBinding::Local)
        << "binding was silently dropped by this round-trip before TF-C78";
    EXPECT_EQ(parsed->mir.funcVisibility(f2), SymbolVisibility::Hidden)
        << "visibility was silently dropped by this round-trip before TF-C78";
    EXPECT_TRUE(parsed->mir.funcNoInline(f2))
        << "all three axes must survive together";

    MirFuncId const f3 = findBySym(3);
    ASSERT_TRUE(f3.valid());
    EXPECT_EQ(parsed->mir.funcBinding(f3), SymbolBinding::Weak);
    EXPECT_FALSE(parsed->mir.funcNoInline(f3))
        << "a weak function must NOT come back noinline — the axes are "
           "independent, not one conflated bit";

    MirFuncId const f4 = findBySym(4);
    ASSERT_TRUE(f4.valid());
    EXPECT_EQ(parsed->mir.funcBinding(f4),    SymbolBinding::Global);
    EXPECT_EQ(parsed->mir.funcVisibility(f4), SymbolVisibility::Default);
    EXPECT_FALSE(parsed->mir.funcNoInline(f4));

    // The all-default function prints NO attribute list, so existing golden
    // text for ordinary functions is byte-unchanged.
    EXPECT_NE(text.find("function %4 : fn() -> void {"), std::string::npos)
        << "an all-default function must emit no `[...]` list:\n" << text;

    // And the emitted text is itself stable (emit → parse → emit).
    MirTextContext ctx2{&parsed->interner, &parsed->symbolNames};
    DiagnosticReporter r3;
    EXPECT_EQ(text, emitMir(parsed->mir, ctx2, r3));
}

// TF-C81 (D-CSUBSET-ALWAYSINLINE): the `alwaysinline` function attribute
// survives the `.dssir` round-trip, and does so INDEPENDENTLY of `noinline`.
//
// Written as its own test rather than folded into the one above because the
// interesting failure is not "the flag was dropped" but "the flag landed in the
// WRONG BIT": `alwaysInline` is the second of two adjacent trailing bools at
// `addFunction`, and printer/parser each name the two keywords separately. A
// fixture that set both flags on one function could not tell a swap from a
// correct carry, so every function here sets EXACTLY ONE of them and the
// assertions check both directions on each.
//
// RED-ON-DISABLE: drop `alwaysInline` from `parseFunction`'s `addFunction` call,
// or the `if (ai)` arm in `appendFuncAttrs`, and the first EXPECT below fails
// while `FunctionAttributesSurviveRoundTrip` stays green.
TEST(MirText, AlwaysInlineAttributeSurvivesRoundTrip) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const voidTy = ti.primitive(TypeKind::Void);
    TypeId const fnSig  = ti.fnSig(std::span<TypeId const>{}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    // %1 — alwaysinline ONLY (Global/Default, noInline clear): isolates the bit.
    (void)b.addFunction(fnSig, SymbolId{1}, SymbolBinding::Global,
                        SymbolVisibility::Default, /*noInline=*/false,
                        /*alwaysInline=*/true);
    MirBlockId e1 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e1); b.addReturn();
    // %2 — noinline ONLY: the mirror. Together with %1 this pins the SWAP.
    (void)b.addFunction(fnSig, SymbolId{2}, SymbolBinding::Global,
                        SymbolVisibility::Default, /*noInline=*/true,
                        /*alwaysInline=*/false);
    MirBlockId e2 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e2); b.addReturn();
    // %3 — alwaysinline COMPOSED with the linkage axes, all non-default.
    (void)b.addFunction(fnSig, SymbolId{3}, SymbolBinding::Local,
                        SymbolVisibility::Hidden, /*noInline=*/false,
                        /*alwaysInline=*/true);
    MirBlockId e3 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e3); b.addReturn();
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "ai", "ni", "localhidden"};
    DiagnosticReporter r1, r2;
    MirTextContext ctx{&ti, &names};
    std::string const text = emitMir(m, ctx, r1);
    auto parsed = parseMir(text, CompilationUnitId{1}, r2);
    ASSERT_NE(parsed, nullptr);
    ASSERT_TRUE(parsed->ok) << text;
    ASSERT_EQ(parsed->mir.moduleFuncCount(), 3u);

    auto findBySym = [&](std::uint32_t sym) -> MirFuncId {
        std::size_t const nf = parsed->mir.moduleFuncCount();
        for (std::uint32_t i = 0; i < nf; ++i) {
            MirFuncId const f = parsed->mir.funcAt(i);
            if (parsed->mir.funcSymbol(f).v == sym) return f;
        }
        return MirFuncId{};
    };

    MirFuncId const f1 = findBySym(1);
    ASSERT_TRUE(f1.valid());
    EXPECT_TRUE(parsed->mir.funcAlwaysInline(f1))
        << "an alwaysinline function must come back alwaysinline — a dropped "
           "flag here silently restores the inliner's size threshold:\n" << text;
    EXPECT_FALSE(parsed->mir.funcNoInline(f1))
        << "and must NOT come back noinline — that swap would invert the "
           "directive into its exact opposite";

    MirFuncId const f2 = findBySym(2);
    ASSERT_TRUE(f2.valid());
    EXPECT_TRUE(parsed->mir.funcNoInline(f2));
    EXPECT_FALSE(parsed->mir.funcAlwaysInline(f2))
        << "the mirror direction of the same swap";

    MirFuncId const f3 = findBySym(3);
    ASSERT_TRUE(f3.valid());
    EXPECT_EQ(parsed->mir.funcBinding(f3),    SymbolBinding::Local);
    EXPECT_EQ(parsed->mir.funcVisibility(f3), SymbolVisibility::Hidden);
    EXPECT_TRUE(parsed->mir.funcAlwaysInline(f3))
        << "all four per-function axes must survive together";
    EXPECT_FALSE(parsed->mir.funcNoInline(f3));

    // The keyword is the text format's own spelling, not the C attribute's.
    EXPECT_NE(text.find("alwaysinline"), std::string::npos)
        << "the printer must emit the `.dssir` keyword:\n" << text;

    MirTextContext ctx2{&parsed->interner, &parsed->symbolNames};
    DiagnosticReporter r3;
    EXPECT_EQ(text, emitMir(parsed->mir, ctx2, r3));
}

// ★★ TF-C85: the `nooptimize` axis survives the `.dssir` round trip, and lands
// in the RIGHT bit. Three adjacent trailing bools at every `addFunction` call
// means a transposition compiles silently, so the fixture sets exactly ONE flag
// per function and asserts the other two are clear — the anti-swap discipline
// `mir.hpp`'s addFunction comment asks callers to preserve.
TEST(MirText, NoOptimizeAttributeSurvivesRoundTrip) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const voidTy = ti.primitive(TypeKind::Void);
    TypeId const fnSig  = ti.fnSig(std::span<TypeId const>{}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    // %1 — nooptimize ONLY.
    (void)b.addFunction(fnSig, SymbolId{1}, SymbolBinding::Global,
                        SymbolVisibility::Default, /*noInline=*/false,
                        /*alwaysInline=*/false, /*noOptimize=*/true);
    MirBlockId e1 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e1); b.addReturn();
    // %2 — noinline ONLY: the neighbour that must not be confused with it.
    (void)b.addFunction(fnSig, SymbolId{2}, SymbolBinding::Global,
                        SymbolVisibility::Default, /*noInline=*/true,
                        /*alwaysInline=*/false, /*noOptimize=*/false);
    MirBlockId e2 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e2); b.addReturn();
    // %3 — nooptimize COMPOSED with both linkage axes, all non-default.
    (void)b.addFunction(fnSig, SymbolId{3}, SymbolBinding::Local,
                        SymbolVisibility::Hidden, /*noInline=*/false,
                        /*alwaysInline=*/false, /*noOptimize=*/true);
    MirBlockId e3 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e3); b.addReturn();
    // %4 — every flag DEFAULT: must print no attribute list at all, which is
    // what pins the printer's "everything default -> print nothing" early return
    // against silently dropping the new axis from that condition.
    (void)b.addFunction(fnSig, SymbolId{4});
    MirBlockId e4 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e4); b.addReturn();
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "no", "ni", "localhidden", "plain"};
    DiagnosticReporter r1, r2;
    MirTextContext ctx{&ti, &names};
    std::string const text = emitMir(m, ctx, r1);
    auto parsed = parseMir(text, CompilationUnitId{1}, r2);
    ASSERT_NE(parsed, nullptr);
    ASSERT_TRUE(parsed->ok) << text;
    ASSERT_EQ(parsed->mir.moduleFuncCount(), 4u);

    auto findBySym = [&](std::uint32_t sym) -> MirFuncId {
        std::size_t const nf = parsed->mir.moduleFuncCount();
        for (std::uint32_t i = 0; i < nf; ++i) {
            MirFuncId const f = parsed->mir.funcAt(i);
            if (parsed->mir.funcSymbol(f).v == sym) return f;
        }
        return MirFuncId{};
    };

    MirFuncId const f1 = findBySym(1);
    ASSERT_TRUE(f1.valid());
    EXPECT_TRUE(parsed->mir.funcNoOptimize(f1))
        << "a nooptimize function must come back nooptimize — a dropped flag "
           "here silently re-enables every pass on it:\n" << text;
    EXPECT_FALSE(parsed->mir.funcNoInline(f1));
    EXPECT_FALSE(parsed->mir.funcAlwaysInline(f1));

    MirFuncId const f2 = findBySym(2);
    ASSERT_TRUE(f2.valid());
    EXPECT_TRUE(parsed->mir.funcNoInline(f2));
    EXPECT_FALSE(parsed->mir.funcNoOptimize(f2))
        << "the mirror direction of the same swap";

    MirFuncId const f3 = findBySym(3);
    ASSERT_TRUE(f3.valid());
    EXPECT_EQ(parsed->mir.funcBinding(f3),    SymbolBinding::Local);
    EXPECT_EQ(parsed->mir.funcVisibility(f3), SymbolVisibility::Hidden);
    EXPECT_TRUE(parsed->mir.funcNoOptimize(f3))
        << "all the per-function axes must survive together";

    MirFuncId const f4 = findBySym(4);
    ASSERT_TRUE(f4.valid());
    EXPECT_FALSE(parsed->mir.funcNoOptimize(f4))
        << "an unmarked function must not acquire the flag";

    EXPECT_NE(text.find("nooptimize"), std::string::npos)
        << "the printer must emit the `.dssir` keyword:\n" << text;

    MirTextContext ctx2{&parsed->interner, &parsed->symbolNames};
    DiagnosticReporter r3;
    EXPECT_EQ(text, emitMir(parsed->mir, ctx2, r3));
}

// ★★ TF-C92 (D-CSUBSET-NO-SANITIZE-THREAD): the `nosanitizethread` axis survives
// the `.dssir` round trip, and lands in the RIGHT bit. FOUR adjacent trailing bools
// at every `addFunction` call now, so a transposition compiles silently; the fixture
// sets exactly ONE flag per function and asserts the other three are clear.
//
// ★ THIS TEST CARRIES MORE WEIGHT THAN ITS THREE SIBLINGS, AND THE REASON IS
// STRUCTURAL, NOT STYLISTIC. `noinline`/`alwaysinline`/`nooptimize` each also reach
// an optimizer pass whose OUTPUT changes when the flag is lost, so a behavioural
// test can corroborate them. `no_sanitize_thread` reaches NO pass — MEASURED,
// `grep -rni sanitiz src/` is empty — so the printer/parser pair IS the feature's
// only surface. If this round trip drops the flag, the fact is unrecoverable and
// nothing else in the suite notices.
//
// RED-ON-DISABLE: drop `noSanitizeThread` from `parseFunction`'s `addFunction` call
// (let the 8th parameter default to false) → the first EXPECT_TRUE below fails and
// `NoOptimizeAttributeSurvivesRoundTrip` stays green. Delete the `if (ns)` arm in
// `appendFuncAttrs` → the keyword vanishes, so both the flag assertions AND the
// `text.find` assertion fail. Forget `!ns` in the printer's all-default early return
// → function %1 (whose only non-default axis is this one) prints no `[...]` list at
// all and comes back clean.
TEST(MirText, NoSanitizeThreadAttributeSurvivesRoundTrip) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const voidTy = ti.primitive(TypeKind::Void);
    TypeId const fnSig  = ti.fnSig(std::span<TypeId const>{}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    // %1 — nosanitizethread ONLY. Its whole point is that this is the SOLE
    // non-default axis: it is what forces the printer's all-default early return to
    // account for the new flag, and what would silently print nothing if it did not.
    (void)b.addFunction(fnSig, SymbolId{1}, SymbolBinding::Global,
                        SymbolVisibility::Default, /*noInline=*/false,
                        /*alwaysInline=*/false, /*noOptimize=*/false,
                        /*noSanitizeThread=*/true);
    MirBlockId e1 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e1); b.addReturn();
    // %2 — nooptimize ONLY: the IMMEDIATE neighbour in the argument list, so a
    // one-position transposition fails both directions at once.
    (void)b.addFunction(fnSig, SymbolId{2}, SymbolBinding::Global,
                        SymbolVisibility::Default, /*noInline=*/false,
                        /*alwaysInline=*/false, /*noOptimize=*/true,
                        /*noSanitizeThread=*/false);
    MirBlockId e2 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e2); b.addReturn();
    // %3 — nosanitizethread COMPOSED with both linkage axes AND with `noinline`.
    // ★ THE COMPOSITION IS DELIBERATE AND IS ITS OWN CLAIM: unlike the
    // noinline/always_inline pair this axis contradicts nothing, so a function may
    // carry both, and the printer must emit both keywords in one `[...]` list.
    (void)b.addFunction(fnSig, SymbolId{3}, SymbolBinding::Local,
                        SymbolVisibility::Hidden, /*noInline=*/true,
                        /*alwaysInline=*/false, /*noOptimize=*/false,
                        /*noSanitizeThread=*/true);
    MirBlockId e3 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e3); b.addReturn();
    // %4 — every flag DEFAULT: prints no attribute list, so existing golden text
    // for ordinary functions stays byte-unchanged by this cycle.
    (void)b.addFunction(fnSig, SymbolId{4});
    MirBlockId e4 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e4); b.addReturn();
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "ns", "no", "localhiddenni", "plain"};
    DiagnosticReporter r1, r2;
    MirTextContext ctx{&ti, &names};
    std::string const text = emitMir(m, ctx, r1);
    auto parsed = parseMir(text, CompilationUnitId{1}, r2);
    ASSERT_NE(parsed, nullptr);
    ASSERT_TRUE(parsed->ok) << text;
    ASSERT_EQ(parsed->mir.moduleFuncCount(), 4u);

    auto findBySym = [&](std::uint32_t sym) -> MirFuncId {
        std::size_t const nf = parsed->mir.moduleFuncCount();
        for (std::uint32_t i = 0; i < nf; ++i) {
            MirFuncId const f = parsed->mir.funcAt(i);
            if (parsed->mir.funcSymbol(f).v == sym) return f;
        }
        return MirFuncId{};
    };

    MirFuncId const f1 = findBySym(1);
    ASSERT_TRUE(f1.valid());
    EXPECT_TRUE(parsed->mir.funcNoSanitizeThread(f1))
        << "a nosanitizethread function must come back nosanitizethread — a "
           "dropped flag here erases the ONLY record DSS keeps of the source's "
           "thread-sanitizer exclusion:\n" << text;
    EXPECT_FALSE(parsed->mir.funcNoOptimize(f1))
        << "and must NOT come back nooptimize — the adjacent-argument swap";
    EXPECT_FALSE(parsed->mir.funcNoInline(f1));
    EXPECT_FALSE(parsed->mir.funcAlwaysInline(f1));

    MirFuncId const f2 = findBySym(2);
    ASSERT_TRUE(f2.valid());
    EXPECT_TRUE(parsed->mir.funcNoOptimize(f2));
    EXPECT_FALSE(parsed->mir.funcNoSanitizeThread(f2))
        << "the mirror direction of the same swap";

    MirFuncId const f3 = findBySym(3);
    ASSERT_TRUE(f3.valid());
    EXPECT_EQ(parsed->mir.funcBinding(f3),    SymbolBinding::Local);
    EXPECT_EQ(parsed->mir.funcVisibility(f3), SymbolVisibility::Hidden);
    EXPECT_TRUE(parsed->mir.funcNoSanitizeThread(f3))
        << "all five per-function axes must survive together";
    EXPECT_TRUE(parsed->mir.funcNoInline(f3))
        << "and the two COMPOSE — this axis contradicts nothing, so both keywords "
           "must round-trip on one function";
    EXPECT_FALSE(parsed->mir.funcNoOptimize(f3));

    MirFuncId const f4 = findBySym(4);
    ASSERT_TRUE(f4.valid());
    EXPECT_FALSE(parsed->mir.funcNoSanitizeThread(f4))
        << "an unmarked function must not acquire the flag";
    EXPECT_NE(text.find("function %4 : fn() -> void {"), std::string::npos)
        << "an all-default function must still emit NO `[...]` list — the new "
           "axis must not have leaked into the early-return condition's inverse:\n"
        << text;

    EXPECT_NE(text.find("nosanitizethread"), std::string::npos)
        << "the printer must emit the `.dssir` keyword — this IS the sink:\n" << text;

    MirTextContext ctx2{&parsed->interner, &parsed->symbolNames};
    DiagnosticReporter r3;
    EXPECT_EQ(text, emitMir(parsed->mir, ctx2, r3));
}

// TF-C78: an UNRECOGNIZED function attribute FAILS LOUD rather than being
// skipped. A parser that silently ignored an unknown name is precisely how a
// dropped field goes unnoticed for a long time (see the test above), so the
// permissive direction is closed by construction.
TEST(MirText, UnknownFunctionAttributeIsMalformed) {
    char const* text =
        "dssir 1\n"
        "module {\n"
        "  function %1 : fn() -> void [frobnicate] {\n"
        "    block %b0 [entry] {\n"
        "      ret\n"
        "    }\n"
        "  }\n"
        "}\n";
    DiagnosticReporter r;
    auto res = parseMir(text, CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok)
        << "an unknown function attribute must not parse clean";
    EXPECT_GT(r.errorCount(), 0u);
}

// ── inline asm in the text format ───────────────────────────────────────────
//
// ★★★ THIS FILE HAD **ZERO** COVERAGE OF EITHER ASM OPCODE, IN EITHER
// DIRECTION, UNTIL 2026-08-19 (D-MIR-TEXT-INLINE-ASM-RENDERS-A-POOL-INDEX-AND-NO-EDGES).
// Both fell into the writer's `default:` arm, which rendered the
// operands and then the raw `instPayload` — an index into the module's
// `MirAsmDescriptorPool`, meaningless once the text leaves the module — and,
// because `default:` renders no successors, an `asm goto` printed with **no CFG
// edges at all**. Nothing asserted against either.
//
// ⚠ THE ONE-WAY-NESS IS NOT THE DEFECT AND THIS TEST PINS IT AS CORRECT. The
// parser REFUSES both mnemonics by name and states why (the descriptor carries
// text and constraints this format does not spell), so the dump is deliberately
// not re-readable. A one-way dump is a choice; a dump that silently drops a
// terminator's edges while looking complete is not.
TEST(MirText, InlineAsmGotoRendersEveryEdgeAndNamesTheUnspelledDescriptor) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const voidT = ti.primitive(TypeKind::Void);
    TypeId const fnSig = ti.fnSig(std::span<TypeId const>{}, voidT, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const l0    = b.createBlock(StructCfMarker::Linear);
    MirBlockId const l1    = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(entry);
    MirAsmDescriptor d;
    d.templateText   = "jmp %l[a]";
    d.labelSpellings = {{"%l[a]"}, {"%l[b]"}};
    MirBlockId const labels[] = {l0, l1};
    auto const r = b.addInlineAsmGoto(std::move(d), {}, labels);
    ASSERT_TRUE(r.terminator.valid());
    MirBlockId const cont = r.continuation();
    b.beginBlock(l0);   b.addReturn();
    b.beginBlock(l1);   b.addReturn();
    b.beginBlock(cont); b.addReturn();
    Mir const m = std::move(b).finish();

    DiagnosticReporter rep;
    std::vector<std::string> names{"", "f"};
    MirTextContext ctx{&ti, &names};
    std::string const text = emitMir(m, ctx, rep);

    // Every edge is rendered, and the fall-through is LABELLED rather than left
    // to be inferred from its position — a reader checking the convention should
    // not have to have remembered it.
    EXPECT_NE(text.find(std::format("%b{}", l0.v)), std::string::npos)
        << "label 0's edge is missing from the dump:\n" << text;
    EXPECT_NE(text.find(std::format("%b{}", l1.v)), std::string::npos)
        << "label 1's edge is missing from the dump:\n" << text;
    EXPECT_NE(text.find(std::format("fallthrough %b{}", cont.v)), std::string::npos)
        << "the fall-through edge is missing or unlabelled:\n" << text;
    EXPECT_NE(text.find("<asm-descriptor-unspelled>"), std::string::npos)
        << "the absent descriptor must be NAMED, not replaced by a number:\n" << text;
    // ★★ THE MARKER IS ONE TOKEN BECAUSE A PHRASE BROKE THE PROCESS.
    // ✔MEASURED while writing this test: spelling it
    // `<descriptor not spelled by this format>` aborted the run with
    // `addInst: opcode 'not' takes [1, 1] operands but got 0`. The parser refuses
    // the `inlineasm*` mnemonic and its recovery then re-tokenizes the rest of the
    // line, where the bare word `not` IS a real MIR opcode — so the WRITER was
    // handing the PARSER a valid instruction inside human prose. Asserting that no
    // space follows the marker is what stops a future edit from reintroducing one.
    EXPECT_EQ(text.find("<asm-descriptor-unspelled "), std::string::npos)
        << "the marker must stay a single token — the parser re-tokenizes the "
           "tail of a refused instruction:\n" << text;

    // ★ THE ANTI-REGRESSION HALF. The old rendering was `payload <pool index>`;
    // a raw index reads as data and is not one. Asserting its ABSENCE is what
    // makes reverting the writer arm visible here rather than only in review.
    EXPECT_EQ(text.find("payload "), std::string::npos)
        << "a raw descriptor-pool index is back in the dump:\n" << text;

    // ⓘ THE PARSER'S REFUSAL IS THE DECLARED BEHAVIOUR, pinned so a future
    // round-trip cannot be added on one side only.
    DiagnosticReporter back;
    auto parsed = parseMir(text, CompilationUnitId{1}, back);
    EXPECT_FALSE(parsed->ok)
        << "the text format does not spell an asm descriptor; the parser must "
           "refuse rather than build a module missing one";
}
