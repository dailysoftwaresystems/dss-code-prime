// HR7 `.dsshir` text-format tests: in-memory byte-identical round-trip across the
// node/type/attribute surface, verify-on-load, parse-error reporting, and a
// golden corpus (DSS_REFRESH_GOLDENS=1 to regenerate, mirroring test_corpus.cpp).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "hir/attributes/diagnostic_info.hpp"
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/attributes/shader_intrinsic.hpp"
#include "hir/attributes/source_span.hpp"
#include "hir/attributes/transpile_hints.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/hir_op.hpp"
#include "hir/hir_text.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == c) ++n;
    return n;
}

// emit -> parse -> re-emit; assert the two emits are byte-identical and the parse
// (verify-on-load included) is clean. Returns the first emit so callers can also
// pin specific substrings.
std::string expectRoundTrip(Hir const& hir, HirTextContext const& ctx) {
    DiagnosticReporter r1;
    std::string const first = emitHir(hir, ctx, r1);
    EXPECT_EQ(countCode(r1, DiagnosticCode::H_TextMalformed), 0u) << "emit produced warnings:\n" << first;

    DiagnosticReporter r2;
    auto res = parseHir(first, CompilationUnitId{7}, r2);
    EXPECT_TRUE(res->ok) << "parse/verify not clean for:\n" << first
                         << "\nfirst diag: "
                         << (res->ok ? "" : std::string{r2.all().empty() ? "" : r2.all()[0].actual});

    HirTextContext ctx2;
    ctx2.interner      = &res->interner;
    ctx2.symbolNames   = &res->symbolNames;
    ctx2.sourceMap     = &res->sourceMap;
    ctx2.ffiMap        = &res->ffiMap;
    ctx2.shaderMap     = &res->shaderMap;
    ctx2.transpileMap  = &res->transpileMap;
    ctx2.diagnosticMap = &res->diagnosticMap;
    // Thread the rebuilt pool so a pooled module re-emits its inline values
    // (byte-identity would otherwise fail when the first emit used a pool).
    if (ctx.literalPool) ctx2.literalPool = &res->literalPool;

    DiagnosticReporter r3;
    std::string const second = emitHir(res->hir, ctx2, r3);
    EXPECT_EQ(first, second) << "round-trip not byte-identical";
    return first;
}

// ── builder helpers ────────────────────────────────────────────────────────

// A function `fn() -> i64 { var x = 1 + 2; return x; }` and its interner/names.
struct ToyModule {
    TypeInterner             interner{CompilationUnitId{1}};
    std::vector<std::string> names{"", "main", "x"};
    HirBuilder               b{"toy"};
};

} // namespace

TEST(HirText, EmitMinimalModule) {
    HirBuilder b{"toy"};
    HirNodeId root = b.makeModule({});
    Hir hir = std::move(b).finish(root);

    HirTextContext ctx;  // no interner/symbols needed for an empty module
    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    EXPECT_NE(text.find("dsshir 1\n"), std::string::npos);
    EXPECT_NE(text.find("module \"toy\" {"), std::string::npos);
    expectRoundTrip(hir, ctx);
}

TEST(HirText, RoundTripArithmeticFunction) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId i64 = in.primitive(TypeKind::I64);
    TypeId sig = in.fnSig({}, i64, CallConv::CcSysV);

    HirBuilder b{"toy"};
    HirNodeId lit0 = b.makeLiteral(i64, 0);
    HirNodeId lit1 = b.makeLiteral(i64, 1);
    HirNodeId sum  = b.makeBinaryOp(HirOpKind::Add, lit0, lit1, i64);
    HirNodeId var  = b.makeVarDecl(i64, /*symbol=*/2, sum);
    HirNodeId ref  = b.makeRef(i64, /*symbol=*/2);
    HirNodeId ret  = b.makeReturn(ref);
    HirNodeId body = b.makeBlock(std::vector<HirNodeId>{var, ret});
    HirNodeId fn   = b.makeFunction(sig, /*symbol=*/1, {}, body);
    HirNodeId root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main", "x"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("binop Add : i64"), std::string::npos);
    EXPECT_NE(text.find("%1 \"main\""), std::string::npos);
    EXPECT_NE(text.find("%2 \"x\""), std::string::npos);
}

TEST(HirText, RoundTripControlFlow) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId i32  = in.primitive(TypeKind::I32);
    TypeId boolT = in.primitive(TypeKind::Bool);
    TypeId voidT = in.primitive(TypeKind::Void);
    TypeId sig  = in.fnSig({}, voidT, CallConv::CcSysV);

    HirBuilder b{"toy"};
    // if (1 < 2) { break; } else { continue; } inside a while, plus a for + switch.
    auto cmp = [&] {
        HirNodeId a = b.makeLiteral(i32, 0), c = b.makeLiteral(i32, 1);
        return b.makeBinaryOp(HirOpKind::Lt, a, c, boolT);
    };
    HirNodeId ifs = b.makeIfStmt(cmp(), b.makeBlock(std::vector<HirNodeId>{b.makeBreak(0)}),
                                 b.makeBlock(std::vector<HirNodeId>{b.makeContinue(0)}));
    HirNodeId whileBody = b.makeBlock(std::vector<HirNodeId>{ifs});
    HirNodeId whileS = b.makeWhileStmt(cmp(), whileBody);

    HirNodeId forBody = b.makeBlock(std::vector<HirNodeId>{b.makeBreak(0)});
    HirNodeId forS = b.makeForStmt(b.makeVarDecl(i32, 2, b.makeLiteral(i32, 0)), cmp(),
                                   std::nullopt, forBody);

    // c60 (Design I-A): switch = [disc, body Block, dispatch arms]. The body holds
    // the case markers (LabelStmts); each arm maps a value to its marker ordinal.
    HirNodeId armV = b.makeLiteral(i32, 5);
    HirNodeId swBody = b.makeBlock(std::vector<HirNodeId>{
        b.makeLabelStmt(0, b.makeBreak(0)),
        b.makeLabelStmt(1, b.makeReturn())});
    HirNodeId arm0 = b.makeCaseArm(armV, /*labelOrdinal=*/0);
    HirNodeId armD = b.makeCaseArm(std::nullopt, /*labelOrdinal=*/1);
    HirNodeId sw = b.makeSwitchStmt(b.makeLiteral(i32, 0), swBody,
                                    std::vector<HirNodeId>{arm0, armD});

    HirNodeId body = b.makeBlock(std::vector<HirNodeId>{whileS, forS, sw, b.makeReturn()});
    HirNodeId fn = b.makeFunction(sig, 1, {}, body);
    HirNodeId root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main", "i"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("while ("), std::string::npos);
    EXPECT_NE(text.find("for {"), std::string::npos);
    EXPECT_NE(text.find("switch ("), std::string::npos);
    EXPECT_NE(text.find("default L1"), std::string::npos);   // c60: dispatch arm form
}

TEST(HirText, RoundTripLiteralValues) {
    // Every HirLiteralValue arm round-trips its VALUE inline (G17): int / uint /
    // char(uint) / float / string. Pin both byte-identity AND that the rebuilt
    // pool carries the decoded values.
    TypeInterner in{CompilationUnitId{1}};
    TypeId i32  = in.primitive(TypeKind::I32);
    TypeId u32  = in.primitive(TypeKind::U32);
    TypeId chr  = in.primitive(TypeKind::Char);
    TypeId f64  = in.primitive(TypeKind::F64);
    TypeId arrc = in.array(chr, 3);                 // "hi" + NUL
    TypeId voidT = in.primitive(TypeKind::Void);
    TypeId sig  = in.fnSig({}, voidT, CallConv::CcSysV);

    HirLiteralPool pool;
    HirBuilder b{"toy"};
    auto litOf = [&](TypeId t, HirLiteralValue v) {
        return b.makeLiteral(t, pool.add(std::move(v)));
    };
    HirNodeId stmts[] = {
        b.makeExprStmt(litOf(i32,  HirLiteralValue{std::int64_t{-7}, TypeKind::I32})),
        b.makeExprStmt(litOf(u32,  HirLiteralValue{std::uint64_t{42}, TypeKind::U32})),
        b.makeExprStmt(litOf(chr,  HirLiteralValue{std::uint64_t{'a'}, TypeKind::Char})),
        b.makeExprStmt(litOf(f64,  HirLiteralValue{double{3.5}, TypeKind::F64})),
        b.makeExprStmt(litOf(arrc, HirLiteralValue{std::string{"hi"}, TypeKind::Char})),
        b.makeReturn(),
    };
    HirNodeId body = b.makeBlock(stmts);
    HirNodeId fn   = b.makeFunction(sig, 1, {}, body);
    HirNodeId root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names; ctx.literalPool = &pool;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("lit int -7 : i32"), std::string::npos) << text;
    EXPECT_NE(text.find("lit uint 42 : u32"), std::string::npos) << text;
    EXPECT_NE(text.find("lit uint 97 : char"), std::string::npos) << text;
    EXPECT_NE(text.find("lit float 3.5 : f64"), std::string::npos) << text;
    EXPECT_NE(text.find("lit str \"hi\" : arr<char, 3>"), std::string::npos) << text;

    // The rebuilt pool carries the decoded values.
    DiagnosticReporter pr;
    auto res = parseHir(text, CompilationUnitId{9}, pr);
    ASSERT_TRUE(res->ok);
    ASSERT_EQ(res->literalPool.size(), 5u);
    EXPECT_EQ(std::get<std::int64_t>(res->literalPool.at(0).value), -7);
    EXPECT_EQ(std::get<std::uint64_t>(res->literalPool.at(2).value), static_cast<std::uint64_t>('a'));
    EXPECT_EQ(std::get<double>(res->literalPool.at(3).value), 3.5);
    EXPECT_EQ(std::get<std::string>(res->literalPool.at(4).value), "hi");
}

TEST(HirText, MalformedLiteralValuesFailLoud) {
    // Each malformed inline literal value must fail loud (res->ok == false),
    // never silently default. Pins the bool/overflow/unknown-tag guards.
    auto parseFails = [](std::string_view body) {
        std::string const text =
            std::string("dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
                        "  function %1 : fn() -> void {\n    block {\n      expr ")
            + std::string(body) + "\n      return\n    }\n  }\n}\n";
        DiagnosticReporter r;
        auto res = parseHir(text, CompilationUnitId{1}, r);
        return res->ok;
    };
    EXPECT_FALSE(parseFails("lit bool maybe : i1"))            << "non-true/false bool must fail";
    EXPECT_FALSE(parseFails("lit uint 99999999999999999999999 : u64")) << "overflow must fail";
    EXPECT_FALSE(parseFails("lit wat 1 : i32"))                << "unknown value tag must fail";
}

TEST(HirText, RoundTripSeqExpr) {
    // A SeqExpr (statements then a yielded value) round-trips through the
    // `seq : type { … yield <expr> }` form. Models a value-yielding desugar.
    TypeInterner in{CompilationUnitId{1}};
    TypeId i32   = in.primitive(TypeKind::I32);
    TypeId voidT = in.primitive(TypeKind::Void);
    TypeId sig   = in.fnSig({}, voidT, CallConv::CcSysV);

    HirBuilder b{"toy"};
    HirNodeId lit = b.makeLiteral(i32, 0);
    HirNodeId vd  = b.makeVarDecl(i32, 2, lit);              // var %2 = lit#0
    HirNodeId seq = b.makeSeqExpr(std::vector<HirNodeId>{vd}, b.makeRef(i32, 2), i32);
    HirNodeId body = b.makeBlock(std::vector<HirNodeId>{b.makeExprStmt(seq), b.makeReturn()});
    HirNodeId fn   = b.makeFunction(sig, 1, {}, body);
    HirNodeId root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main", "tmp"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("seq : i32 {"), std::string::npos);
    EXPECT_NE(text.find("yield ref %2"), std::string::npos);
}

TEST(HirText, RoundTripTypesAndFlags) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId i32 = in.primitive(TypeKind::I32);
    TypeId f32 = in.primitive(TypeKind::F32);
    TypeId vec = in.vector(f32, 4);
    TypeId ptr = in.pointer(i32);
    TypeId arr = in.array(i32, 8);
    TypeId tup = in.tuple(std::vector<TypeId>{i32, f32});
    TypeId strct = in.structType("Foo", std::vector<TypeId>{i32, vec});
    TypeId fnMs = in.fnSig(std::vector<TypeId>{ptr, arr}, tup, CallConv::CcMS64);
    TypeId voidT = in.primitive(TypeKind::Void);
    TypeId sig = in.fnSig({}, voidT, CallConv::CcSysV);

    HirBuilder b{"toy"};
    // a TypeRef per interesting type, with a flag set on one.
    HirNodeId t1 = b.makeTypeRef(vec, HirFlags::ShaderUsable | HirFlags::HostUsable);
    HirNodeId t2 = b.makeTypeRef(strct);
    HirNodeId t3 = b.makeTypeRef(fnMs);
    HirNodeId body = b.makeBlock(std::vector<HirNodeId>{
        b.makeExprStmt(t1), b.makeExprStmt(t2), b.makeExprStmt(t3), b.makeReturn()});
    HirNodeId fn = b.makeFunction(sig, 1, {}, body, HirFlags::Synthetic);
    HirNodeId root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("vec<f32, 4>"), std::string::npos);
    EXPECT_NE(text.find("struct \"Foo\" {i32, vec<f32, 4>}"), std::string::npos);
    EXPECT_NE(text.find("cc ms64"), std::string::npos);
    EXPECT_NE(text.find("[shader,host]"), std::string::npos);
    EXPECT_NE(text.find("function [syn]"), std::string::npos);
}

TEST(HirText, RoundTripExtensionsAndIntrinsics) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId i32 = in.primitive(TypeKind::I32);
    TypeId voidT = in.primitive(TypeKind::Void);
    TypeId sig = in.fnSig({}, voidT, CallConv::CcSysV);

    HirBuilder b{"glsl"};
    HirIntrinsicId sqrt = b.intrinsicRegistry().registerIntrinsic("math.sqrt", "glsl");
    HirOpId rot = b.opRegistry().registerExtension("APL::Rotate", HirOpArity::Binary, "apl");
    HirKindId barrier = b.registry().registerExtension("ShaderOps::WorkgroupBarrier", "glsl");

    HirNodeId in0 = b.makeLiteral(i32, 0);
    HirNodeId call = b.makeIntrinsicCall(sqrt, std::vector<HirNodeId>{in0}, i32);
    HirNodeId rotE = b.makeBinaryOp(rot, b.makeLiteral(i32, 1), b.makeLiteral(i32, 2), i32);
    HirNodeId bar = b.addLeaf(HirKind::Extension, InvalidType, barrier.v, HirFlags::ShaderUsable);
    HirNodeId body = b.makeBlock(std::vector<HirNodeId>{
        b.makeExprStmt(call), b.makeExprStmt(rotE), bar, b.makeReturn()});
    HirNodeId fn = b.makeFunction(sig, 1, {}, body);
    HirNodeId root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("intrinsics {"), std::string::npos);
    EXPECT_NE(text.find("intrinsic \"math.sqrt\""), std::string::npos);
    EXPECT_NE(text.find("binop ext \"APL::Rotate\""), std::string::npos);
    EXPECT_NE(text.find("ext_node [shader] \"ShaderOps::WorkgroupBarrier\""), std::string::npos);
}

TEST(HirText, RoundTripExternWithFfi) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId u64 = in.primitive(TypeKind::U64);
    TypeId byteP = in.pointer(in.primitive(TypeKind::Byte));
    TypeId sig = in.fnSig(std::vector<TypeId>{u64}, byteP, CallConv::CcSysV);

    HirBuilder b{"c-subset"};
    HirNodeId param = b.makeVarDecl(u64, 2);
    HirNodeId ext = b.makeExternFunction(sig, 1, std::vector<HirNodeId>{param});
    HirNodeId root = b.makeModule(std::vector<HirNodeId>{ext});
    Hir hir = std::move(b).finish(root);

    HirFfiMap ffi{hir};
    ffi.set(ext, FfiMetadata{.mangledName = "malloc", .linkage = FfiLinkage::Strong,
                             .visibility = FfiVisibility::Default, .importLibrary = "libc.so.6"});

    std::vector<std::string> names{"", "malloc", "size"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names; ctx.ffiMap = &ffi;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("@ffi(name \"malloc\", link strong, vis default, lib \"libc.so.6\")"), std::string::npos);
    EXPECT_NE(text.find("extern_function %1"), std::string::npos);
}

TEST(HirText, RoundTripVariadicFnSig) {
    // c14: a variadic extern (`fn(ptr<byte>, i32, ...) -> i32`, the POSIX `open`
    // shape) round-trips through HIR text — the emitter writes the `, ...` marker
    // (scalars[1]) and the parser reads it back as variadic. RED-ON-DISABLE: drop
    // the emitter's `...` clause and the `, ...` vanishes (variadic-ness lost on
    // reparse) — this assertion fails.
    TypeInterner in{CompilationUnitId{1}};
    TypeId i32 = in.primitive(TypeKind::I32);
    TypeId byteP = in.pointer(in.primitive(TypeKind::Byte));
    TypeId sig = in.fnSig(std::vector<TypeId>{byteP, i32}, i32, CallConv::CcSysV, /*isVariadic=*/true);
    ASSERT_TRUE(in.fnIsVariadic(sig));

    HirBuilder b{"c-subset"};
    HirNodeId p0 = b.makeVarDecl(byteP, 2);
    HirNodeId p1 = b.makeVarDecl(i32, 3);
    HirNodeId ext = b.makeExternFunction(sig, 1, std::vector<HirNodeId>{p0, p1});
    HirNodeId root = b.makeModule(std::vector<HirNodeId>{ext});
    Hir hir = std::move(b).finish(root);

    HirFfiMap ffi{hir};
    ffi.set(ext, FfiMetadata{.mangledName = "open", .linkage = FfiLinkage::Strong,
                             .visibility = FfiVisibility::Default, .importLibrary = "libc.so.6"});
    std::vector<std::string> names{"", "open", "path", "flags"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names; ctx.ffiMap = &ffi;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find(", ...) ->"), std::string::npos) << text;
}

TEST(HirText, RoundTripAllSideTables) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId i32 = in.primitive(TypeKind::I32);
    TypeId voidT = in.primitive(TypeKind::Void);
    TypeId sig = in.fnSig({}, voidT, CallConv::CcSysV);

    HirBuilder b{"toy"};
    HirNodeId litA = b.makeLiteral(i32, 0);
    HirNodeId errN = b.addLeaf(HirKind::Error, i32, 0, HirFlags::HasError);
    HirNodeId body = b.makeBlock(std::vector<HirNodeId>{b.makeExprStmt(litA), b.makeExprStmt(errN), b.makeReturn()});
    HirNodeId fn = b.makeFunction(sig, 1, {}, body);
    HirNodeId root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    HirSourceMap src{hir};      src.set(litA, HirSourceLoc{BufferId{3}, SourceSpan::of(16, 42)});
    HirShaderMap shader{hir};   shader.set(fn, ShaderIntrinsic{.stage = ShaderStage::Vertex,
                                                               .builtin = ShaderBuiltin::Position});
    HirTranspileMap tr{hir};    tr.set(litA, TranspileHint{.targetLanguage = "javascript",
                                                           .idiom = TranspileIdiom::TernaryExpr});
    HirDiagnosticMap diag{hir}; diag.set(errN, DiagnosticInfo{.code = DiagnosticCode::H_TypeUnresolved,
                                                              .recovery = HirRecovery::Substituted,
                                                              .origin = litA, .detail = "stand-in"});

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    ctx.sourceMap = &src; ctx.shaderMap = &shader; ctx.transpileMap = &tr; ctx.diagnosticMap = &diag;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("@loc(buf 3, 16..42)"), std::string::npos);
    EXPECT_NE(text.find("@shader(stage vertex, builtin position)"), std::string::npos);
    EXPECT_NE(text.find("@transpile(target \"javascript\", idiom ternary_expr)"), std::string::npos);
    EXPECT_NE(text.find("@diag(code "), std::string::npos);
    EXPECT_NE(text.find("origin "), std::string::npos);

    // The parse must repopulate the maps (and resolve the diag origin to a node).
    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{9}, r);
    EXPECT_EQ(res->sourceMap.size(), 1u);
    EXPECT_EQ(res->shaderMap.size(), 1u);
    EXPECT_EQ(res->transpileMap.size(), 1u);
    EXPECT_EQ(res->diagnosticMap.size(), 1u);
}

TEST(HirText, RoundTripComputeWorkgroup) {
    // Non-default workgroup dims must round-trip (regression: emitter used commas
    // the parser couldn't consume between the three integers).
    TypeInterner in{CompilationUnitId{1}};
    TypeId voidT = in.primitive(TypeKind::Void);
    TypeId sig = in.fnSig({}, voidT, CallConv::CcSysV);

    HirBuilder b{"glsl"};
    HirNodeId body = b.makeBlock(std::vector<HirNodeId>{b.makeReturn()});
    HirNodeId fn = b.makeFunction(sig, 1, {}, body, HirFlags::ShaderUsable);
    HirNodeId root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    HirShaderMap shader{hir};
    shader.set(fn, ShaderIntrinsic{.stage = ShaderStage::Compute,
                                   .workgroup = ShaderWorkgroupSize{8, 4, 2},
                                   .binding = ShaderResourceBinding{1, 3}});
    std::vector<std::string> names{"", "cs"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names; ctx.shaderMap = &shader;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("wg 8 4 2"), std::string::npos);
    EXPECT_NE(text.find("binding 1:3"), std::string::npos);
}

TEST(HirText, ParseMalformedEnumReports) {
    // An unrecognized enum name must report, not silently coerce to a default.
    DiagnosticReporter r;
    auto res = parseHir(
        "dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
        "  @ffi(link bogus)\n  extern_global %1 : i32\n}\n",
        CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GT(countCode(r, DiagnosticCode::H_TextMalformed), 0u);
}

TEST(HirText, ParseStuckTokenDoesNotHang) {
    // A stray punctuation token inside a brace list must be reported and skipped,
    // never spin (regression: the progress guard was dead). Reaching the assert
    // at all proves termination.
    DiagnosticReporter r;
    auto res = parseHir("dsshir 1\nmodule \"toy\" {\n  $ % :\n}\n", CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GT(countCode(r, DiagnosticCode::H_TextMalformed), 0u);
}

TEST(HirText, ParseVersionMismatch) {
    DiagnosticReporter r;
    auto res = parseHir("dsshir 99\nmodule \"x\" {\n}\n", CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_TextVersionMismatch), 1u);
}

TEST(HirText, ParseMalformedReports) {
    DiagnosticReporter r;
    auto res = parseHir("dsshir 1\nmodule \"x\" {\n  @@@ garbage\n}\n", CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GT(countCode(r, DiagnosticCode::H_TextMalformed), 0u);
}

TEST(HirText, ParseUnknownSymbolReports) {
    // %9 referenced but only %1 declared.
    DiagnosticReporter r;
    auto res = parseHir(
        "dsshir 1\nsymbols {\n  %1 \"a\"\n}\nmodule \"toy\" {\n"
        "  global %9 : i32\n}\n",
        CompilationUnitId{1}, r);
    EXPECT_GT(countCode(r, DiagnosticCode::H_TextUnknownName), 0u);
}

TEST(HirText, VerifyOnLoadCatchesUntypedExpr) {
    // A literal with InvalidType -> emits `: invalid` -> verify reports H_TypeUnresolved.
    TypeInterner in{CompilationUnitId{1}};
    TypeId voidT = in.primitive(TypeKind::Void);
    TypeId sig = in.fnSig({}, voidT, CallConv::CcSysV);

    HirBuilder b{"toy"};
    HirNodeId bad = b.makeLiteral(InvalidType, 0);  // untyped expression
    HirNodeId body = b.makeBlock(std::vector<HirNodeId>{b.makeExprStmt(bad), b.makeReturn()});
    HirNodeId fn = b.makeFunction(sig, 1, {}, body);
    HirNodeId root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    DiagnosticReporter r0;
    std::string const text = emitHir(hir, ctx, r0);

    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{2}, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GT(countCode(r, DiagnosticCode::H_TypeUnresolved), 0u);
}

// ── golden corpus ────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] fs::path findHirCorpus() {
    fs::path cwd = fs::current_path();
    for (int hops = 0; hops < 8; ++hops) {
        auto const cand = cwd / "tests" / "hir" / "corpus";
        if (fs::is_directory(cand)) return cand;
        if (!cwd.has_parent_path() || cwd == cwd.parent_path()) break;
        cwd = cwd.parent_path();
    }
    ADD_FAILURE() << "could not locate tests/hir/corpus from " << fs::current_path().string();
    std::abort();
}

[[nodiscard]] bool goldenRefreshRequested() {
    char const* raw = std::getenv("DSS_REFRESH_GOLDENS");
    if (raw == nullptr) return false;
    std::string_view const v{raw};
    if (v == "1" || v == "true" || v == "TRUE" || v == "yes") return true;
    return false;
}

[[nodiscard]] std::string readFile(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) { ADD_FAILURE() << "cannot open " << p.string(); std::abort(); }
    std::ostringstream buf; buf << in.rdbuf();
    std::string s = std::move(buf).str();
    // Normalize CRLF→LF: `emitHir` always writes LF, and `.dsshir` carries no
    // legitimate `\r`. A Windows checkout with core.autocrlf=true can rewrite the
    // LF-in-repo golden to CRLF on disk despite the `.gitattributes eol=lf`, so
    // the byte-compare must be line-ending agnostic to stay green on every runner.
    std::erase(s, '\r');
    return s;
}

} // namespace

TEST(HirText, GoldenCorpus) {
    fs::path const root = findHirCorpus();
    bool sawAny = false;
    for (auto const& entry : fs::directory_iterator(root)) {
        if (entry.path().extension() != ".dsshir") continue;
        sawAny = true;
        std::string const input = readFile(entry.path());

        DiagnosticReporter r;
        auto res = parseHir(input, CompilationUnitId{1}, r);
        EXPECT_TRUE(res->ok) << "corpus file did not parse/verify cleanly: " << entry.path().string()
                             << (r.all().empty() ? "" : ("\n" + r.all()[0].actual));

        HirTextContext ctx;
        ctx.interner = &res->interner; ctx.symbolNames = &res->symbolNames;
        ctx.sourceMap = &res->sourceMap; ctx.ffiMap = &res->ffiMap; ctx.shaderMap = &res->shaderMap;
        ctx.transpileMap = &res->transpileMap; ctx.diagnosticMap = &res->diagnosticMap;
        ctx.literalPool = &res->literalPool;   // thread like the side-tables (empty for #index corpus)
        DiagnosticReporter r2;
        std::string const out = emitHir(res->hir, ctx, r2);

        fs::path golden = entry.path(); golden += ".golden";
        if (goldenRefreshRequested()) {
            std::ofstream o{golden, std::ios::binary}; o << out;
            ADD_FAILURE() << "Refreshed " << golden.string()
                          << " — refresh is developer-only; the test fails by design.";
            continue;
        }
        if (!fs::exists(golden)) {
            ADD_FAILURE() << "missing golden " << golden.string()
                          << " — generate via DSS_REFRESH_GOLDENS=1";
            continue;
        }
        EXPECT_EQ(out, readFile(golden)) << "canonical emit diverged for " << entry.path().filename().string();

        // And the canonical output must itself round-trip byte-identically.
        DiagnosticReporter r3;
        auto res2 = parseHir(out, CompilationUnitId{2}, r3);
        EXPECT_TRUE(res2->ok);
        HirTextContext ctx2;
        ctx2.interner = &res2->interner; ctx2.symbolNames = &res2->symbolNames;
        ctx2.sourceMap = &res2->sourceMap; ctx2.ffiMap = &res2->ffiMap; ctx2.shaderMap = &res2->shaderMap;
        ctx2.transpileMap = &res2->transpileMap; ctx2.diagnosticMap = &res2->diagnosticMap;
        ctx2.literalPool = &res2->literalPool;
        DiagnosticReporter r4;
        EXPECT_EQ(out, emitHir(res2->hir, ctx2, r4));
    }
    EXPECT_TRUE(sawAny) << "no .dsshir corpus files found under " << root.string();
}

// ── parseTypeFromText: standalone type-string decoder ────────────────────────
//
// `parseTypeFromText` exposes the module parser's SINGLE `parseType` production
// as a public entry that interns into a CALLER-provided interner/registry. The
// tests walk the produced type STRUCTURALLY via the interner's accessors (never
// a string compare), so they pin the decoded shape, not the spelling.

// (1) `fn(ptr<char>) -> i32` decodes to EXACTLY: FnSig / result I32 / one param /
// param Ptr / pointee Char. Inspected via fnResult/fnParams + raw operands().
TEST(ParseTypeFromText, DecodesFnPtrCharToI32) {
    TypeInterner interner{CompilationUnitId{42}};
    TypeRegistry reg;
    DiagnosticReporter rep;

    TypeId const fn = parseTypeFromText("fn(ptr<char>) -> i32", interner, reg, rep);

    ASSERT_TRUE(fn.valid());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(interner.kind(fn), TypeKind::FnSig);

    // operands=[result, params...] — verify both via the decoders and the raw
    // operand span so the storage convention itself is pinned.
    auto const ops = interner.operands(fn);
    ASSERT_EQ(ops.size(), 2u);                       // result + exactly one param
    EXPECT_EQ(interner.kind(ops[0]), TypeKind::I32); // operands[0] == result
    EXPECT_EQ(interner.kind(ops[1]), TypeKind::Ptr); // operands[1] == sole param

    EXPECT_EQ(interner.kind(interner.fnResult(fn)), TypeKind::I32);
    auto const params = interner.fnParams(fn);
    ASSERT_EQ(params.size(), 1u);
    TypeId const param = params[0];
    ASSERT_EQ(interner.kind(param), TypeKind::Ptr);

    auto const pointee = interner.operands(param);   // ptr<T>: operands=[T]
    ASSERT_EQ(pointee.size(), 1u);
    EXPECT_EQ(interner.kind(pointee[0]), TypeKind::Char);
}

// (2) Representative types each decode to the right structure AND intern into the
// caller's interner (the produced TypeId is owned by the caller's CU, proving the
// result is reusable in that CU's IR — not built in a throwaway interner).
TEST(ParseTypeFromText, RoundTripsViaInterner) {
    TypeInterner interner{CompilationUnitId{9}};
    TypeRegistry reg;
    DiagnosticReporter rep;

    // primitive — `interner.kind(i32)` only succeeds if `i32` is a TypeId of THIS
    // interner's arena (a foreign id trips the arena bounds/tag guard), so a clean
    // structural read here is itself the proof the result interned into the
    // caller's interner rather than a throwaway.
    TypeId const i32 = parseTypeFromText("i32", interner, reg, rep);
    ASSERT_TRUE(i32.valid());
    EXPECT_EQ(interner.kind(i32), TypeKind::I32);
    EXPECT_TRUE(interner.operands(i32).empty());

    // ptr<char>
    TypeId const pc = parseTypeFromText("ptr<char>", interner, reg, rep);
    ASSERT_TRUE(pc.valid());
    ASSERT_EQ(interner.kind(pc), TypeKind::Ptr);
    auto const pointee = interner.operands(pc);
    ASSERT_EQ(pointee.size(), 1u);
    EXPECT_EQ(interner.kind(pointee[0]), TypeKind::Char);

    // fn sig — structural check + interning provenance
    TypeId const fn = parseTypeFromText("fn(ptr<char>) -> i32", interner, reg, rep);
    ASSERT_TRUE(fn.valid());
    ASSERT_EQ(interner.kind(fn), TypeKind::FnSig);
    EXPECT_EQ(interner.kind(interner.fnResult(fn)), TypeKind::I32);
    ASSERT_EQ(interner.fnParams(fn).size(), 1u);
    EXPECT_EQ(interner.kind(interner.fnParams(fn)[0]), TypeKind::Ptr);

    // canonicalization: the `ptr<char>` interned standalone is the SAME TypeId as
    // the fn sig's param — one decoder, one interner, structural sharing holds.
    EXPECT_EQ(interner.fnParams(fn)[0], pc);

    EXPECT_EQ(rep.errorCount(), 0u);
}

// C99 _Complex (D-CSUBSET-COMPLEX, M1): `complex<f64>` decodes to a Complex over F64,
// and the `fn(complex<f64>) -> f64` signature form (the __builtin_creal/cimag decode
// path) parses to an FnSig with a Complex param — the codec that lets the shipped-lib
// builtin `signature` spell a genuine Complex type.
TEST(ParseTypeFromText, RoundTripsComplex) {
    TypeInterner interner{CompilationUnitId{11}};
    TypeRegistry reg;
    DiagnosticReporter rep;

    TypeId const cd = parseTypeFromText("complex<f64>", interner, reg, rep);
    ASSERT_TRUE(cd.valid());
    EXPECT_EQ(interner.kind(cd), TypeKind::Complex);
    ASSERT_EQ(interner.operands(cd).size(), 1u);
    EXPECT_EQ(interner.kind(interner.operands(cd)[0]), TypeKind::F64);
    EXPECT_TRUE(interner.scalars(cd).empty());

    // The __builtin_complex result form `fn(f64, f64) -> complex<f64>`.
    TypeId const mk = parseTypeFromText("fn(f64, f64) -> complex<f64>", interner, reg, rep);
    ASSERT_TRUE(mk.valid());
    ASSERT_EQ(interner.kind(mk), TypeKind::FnSig);
    EXPECT_EQ(interner.kind(interner.fnResult(mk)), TypeKind::Complex);
    // Canonicalization: the standalone `complex<f64>` == the fn result.
    EXPECT_EQ(interner.fnResult(mk), cd);
    EXPECT_EQ(rep.errorCount(), 0u);
}

// (3) Truncated text (`fn(ptr<`) returns InvalidType AND emits ≥1 error. RED-on-
// disable: if the decoder silently handed back a partial type, `valid()` would be
// true (or no error would be reported) and this fails.
TEST(ParseTypeFromText, MalformedReturnsInvalidAndDiagnoses) {
    TypeInterner interner{CompilationUnitId{3}};
    TypeRegistry reg;
    DiagnosticReporter rep;

    TypeId const bad = parseTypeFromText("fn(ptr<", interner, reg, rep);

    EXPECT_FALSE(bad.valid());        // never a partial type
    EXPECT_EQ(bad, InvalidType);
    EXPECT_GE(rep.errorCount(), 1u);  // the malformed text is reported

    // A trailing-token form is malformed too (a standalone type is exactly one
    // type): the input must be fully consumed.
    DiagnosticReporter rep2;
    TypeId const trailing = parseTypeFromText("i32 i32", interner, reg, rep2);
    EXPECT_FALSE(trailing.valid());
    EXPECT_GE(rep2.errorCount(), 1u);
}

// c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): the type-text codec carries per-field
// EXPLICIT offsets (`struct "X" { T @off, ... }`) for an overlapping FFI layout.
// (1) PARSE: the offsets reach the interner; (2) IDENTITY: an offset-bearing struct
// is a DISTINCT TypeId from the same field-types with no offsets — the property that
// keeps the shipped `structs` entry and the bare typedef (both carrying the offsets)
// collapsed to ONE TypeId while never aliasing a naturally-laid-out struct.
TEST(ParseTypeFromText, ExplicitFieldOffsetsParseAndForkIdentity) {
    TypeInterner interner{CompilationUnitId{11}};
    TypeRegistry reg;
    DiagnosticReporter rep;

    TypeId const withOff = parseTypeFromText(
        "struct \"U\" { u64 @0, u32 @0, u32 @4 }", interner, reg, rep);
    ASSERT_TRUE(withOff.valid());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(interner.kind(withOff), TypeKind::Struct);
    EXPECT_TRUE(interner.hasExplicitOffsets(withOff));
    EXPECT_EQ(interner.explicitFieldOffset(withOff, 0), std::optional<std::uint64_t>{0});
    EXPECT_EQ(interner.explicitFieldOffset(withOff, 1), std::optional<std::uint64_t>{0});
    EXPECT_EQ(interner.explicitFieldOffset(withOff, 2), std::optional<std::uint64_t>{4});

    // Same name + same field types, NO offsets → a different interned type.
    TypeId const noOff = parseTypeFromText(
        "struct \"U\" { u64, u32, u32 }", interner, reg, rep);
    ASSERT_TRUE(noOff.valid());
    EXPECT_FALSE(interner.hasExplicitOffsets(noOff));
    EXPECT_NE(withOff, noOff)
        << "an explicit-offset struct must not alias its natural-layout twin";

    // Re-parsing the SAME offset text canonicalizes to the SAME TypeId (the
    // structs-block-vs-typedef collapse the field-scope injection relies on).
    TypeId const withOff2 = parseTypeFromText(
        "struct \"U\" { u64 @0, u32 @0, u32 @4 }", interner, reg, rep);
    EXPECT_EQ(withOff, withOff2);

    // A partial offset set (mix of `@` and none) is malformed, never a half-layout.
    DiagnosticReporter repBad;
    TypeId const mixed = parseTypeFromText(
        "struct \"U\" { u64 @0, u32, u32 @4 }", interner, reg, repBad);
    EXPECT_FALSE(mixed.valid());
    EXPECT_GE(repBad.errorCount(), 1u);
}

// D-CSUBSET-MEMBER-ALIGNAS: the type-text codec carries per-field member-alignas
// overrides (`struct "X" { T ~align, ... }`). (1) PARSE: the aligns reach the
// interner; (2) IDENTITY: an align-bearing struct is a DISTINCT TypeId from the same
// field-types with no aligns; (3) the `~` marker never collides with the offset `@`.
TEST(ParseTypeFromText, MemberAlignsParseAndForkIdentity) {
    TypeInterner interner{CompilationUnitId{13}};
    TypeRegistry reg;
    DiagnosticReporter rep;

    TypeId const withAlign = parseTypeFromText(
        "struct \"S\" { i32 ~16 }", interner, reg, rep);
    ASSERT_TRUE(withAlign.valid());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(interner.kind(withAlign), TypeKind::Struct);
    EXPECT_TRUE(interner.hasExplicitAligns(withAlign));
    EXPECT_EQ(interner.explicitFieldAlign(withAlign, 0), 16u);

    // Same name + same field types, NO aligns → a different interned type.
    TypeId const noAlign = parseTypeFromText(
        "struct \"S\" { i32 }", interner, reg, rep);
    ASSERT_TRUE(noAlign.valid());
    EXPECT_FALSE(interner.hasExplicitAligns(noAlign));
    EXPECT_NE(withAlign, noAlign)
        << "a member-aligned struct must not alias its natural-alignment twin";

    // Re-parsing the SAME align text canonicalizes to the SAME TypeId.
    TypeId const withAlign2 = parseTypeFromText(
        "struct \"S\" { i32 ~16 }", interner, reg, rep);
    EXPECT_EQ(withAlign, withAlign2);

    // A partial align set (mix of `~` and none) is malformed, never a half-layout.
    DiagnosticReporter repBad;
    TypeId const mixed = parseTypeFromText(
        "struct \"M\" { i32 ~16, i32 }", interner, reg, repBad);
    EXPECT_FALSE(mixed.valid());
    EXPECT_GE(repBad.errorCount(), 1u);

    // Mixing `@` offsets and `~` aligns on the SAME struct is malformed (the two
    // channels are mutually exclusive — offsets override alignment wholesale).
    DiagnosticReporter repMix;
    TypeId const both = parseTypeFromText(
        "struct \"B\" { i32 @0, i32 ~16 }", interner, reg, repMix);
    EXPECT_FALSE(both.valid());
    EXPECT_GE(repMix.errorCount(), 1u);
}

// D-CSUBSET-PACKED: the type-text codec carries the whole-composite `packed` flag
// (`struct "X" packed { ... }`). (1) PARSE: packed reaches the interner; (2)
// IDENTITY: a packed struct is a DISTINCT TypeId from the same fields non-packed;
// (3) packed COMBINES with `~align` markers (a packed struct with an alignas member);
// (4) packed unions round-trip too.
TEST(ParseTypeFromText, PackedParseAndForkIdentity) {
    TypeInterner interner{CompilationUnitId{14}};
    TypeRegistry reg;
    DiagnosticReporter rep;

    TypeId const packed = parseTypeFromText(
        "struct \"S\" packed { i8, i32 }", interner, reg, rep);
    ASSERT_TRUE(packed.valid());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(interner.kind(packed), TypeKind::Struct);
    EXPECT_TRUE(interner.isPacked(packed));

    // Same name + same field types, NOT packed → a DISTINCT interned type.
    TypeId const plain = parseTypeFromText(
        "struct \"S\" { i8, i32 }", interner, reg, rep);
    ASSERT_TRUE(plain.valid());
    EXPECT_FALSE(interner.isPacked(plain));
    EXPECT_NE(packed, plain)
        << "a packed struct must not alias its padded twin";

    // Re-parsing the SAME packed text canonicalizes to the SAME TypeId.
    TypeId const packed2 = parseTypeFromText(
        "struct \"S\" packed { i8, i32 }", interner, reg, rep);
    EXPECT_EQ(packed, packed2);

    // packed COMBINES with a member alignas (`~<align>`): both round-trip.
    TypeId const packedAligned = parseTypeFromText(
        "struct \"P\" packed { i8 ~1, i32 ~4 }", interner, reg, rep);
    ASSERT_TRUE(packedAligned.valid());
    EXPECT_TRUE(interner.isPacked(packedAligned));
    EXPECT_TRUE(interner.hasExplicitAligns(packedAligned));
    EXPECT_EQ(interner.explicitFieldAlign(packedAligned, 1), 4u);

    // A packed UNION round-trips its packed flag.
    TypeId const packedUnion = parseTypeFromText(
        "union \"U\" packed { i8, i32 }", interner, reg, rep);
    ASSERT_TRUE(packedUnion.valid());
    EXPECT_EQ(interner.kind(packedUnion), TypeKind::Union);
    EXPECT_TRUE(interner.isPacked(packedUnion));
}

// D-CSUBSET-PACKED: the `packed` marker ROUND-TRIPS through emit — a packed struct
// in a fn signature emits ` packed` and re-parses packed (emit→parse→emit symmetric),
// so a HIR text round-trip / reintern never silently drops packed.
TEST(HirText, PackedFlagRoundTrip) {
    TypeInterner in{CompilationUnitId{1}};
    std::array<TypeId, 2> const fields{
        in.primitive(TypeKind::Char), in.primitive(TypeKind::U32)};
    TypeId const s = in.forwardComposite(TypeKind::Struct, "S", /*declSiteKey=*/42);
    in.completeComposite(s, fields, /*packed=*/true);
    TypeId const ptrS = in.pointer(s);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    std::array<TypeId, 1> const params{ptrS};
    TypeId const sig = in.fnSig(params, voidTy, CallConv::CcSysV);

    HirBuilder b{"toy"};
    HirNodeId const body = b.makeBlock(std::vector<HirNodeId>{});
    HirNodeId const fn   = b.makeFunction(sig, /*symbol=*/1, {}, body);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    EXPECT_NE(text.find("struct \"S\" packed"), std::string::npos) << text;
    expectRoundTrip(hir, ctx);   // emit→parse→emit byte-identical
}

// c107: the offset syntax ROUND-TRIPS through emit (a struct-returning fn signature
// carries the struct text). emit → parse → emit is byte-identical, and the emitted
// text spells `@4` — so a HIR text round-trip (verify-on-load / reintern) preserves
// the overlapping layout instead of forking the TypeId.
TEST(HirText, ExplicitFieldOffsetsRoundTrip) {
    TypeInterner in{CompilationUnitId{1}};
    std::array<TypeId, 3> const fields{
        in.primitive(TypeKind::U64), in.primitive(TypeKind::U32),
        in.primitive(TypeKind::U32)};
    std::array<std::int64_t, 0> const noWidths{};
    std::array<std::uint64_t, 3> const offs{0, 0, 4};
    TypeId const ov = in.structType("U", fields, noWidths, offs);
    TypeId const ptrOv = in.pointer(ov);
    // A VOID fn TAKING ptr<overlap struct> — the struct text appears in the param
    // list, and a void return lets the body be empty (no fall-through verifier trip).
    TypeId const voidTy = in.primitive(TypeKind::Void);
    std::array<TypeId, 1> const params{ptrOv};
    TypeId const sig = in.fnSig(params, voidTy, CallConv::CcSysV);

    HirBuilder b{"toy"};
    HirNodeId const body = b.makeBlock(std::vector<HirNodeId>{});
    HirNodeId const fn   = b.makeFunction(sig, /*symbol=*/1, {}, body);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    EXPECT_NE(text.find("u64 @0"), std::string::npos) << text;
    EXPECT_NE(text.find("u32 @4"), std::string::npos) << text;
    expectRoundTrip(hir, ctx);   // emit→parse→emit byte-identical (parse+emit symmetric)
}

// D-CSUBSET-MEMBER-ALIGNAS: the member-alignas syntax ROUND-TRIPS through emit (a
// struct-taking fn signature carries the struct text). emit → parse → emit is
// byte-identical, the emitted text spells `~16`, and — critically — a re-parse of the
// emitted text preserves align==16 (a lost `~` would fork the TypeId, dropping the
// declared alignment).
TEST(HirText, MemberAlignsRoundTrip) {
    TypeInterner in{CompilationUnitId{1}};
    std::array<TypeId, 1>        const fields{in.primitive(TypeKind::I32)};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 1> const aligns{16};
    TypeId const s     = in.structType("S", fields, noWidths, noOffs, aligns);
    TypeId const ptrS  = in.pointer(s);
    // A VOID fn TAKING ptr<aligned struct> — the struct text appears in the param
    // list, and a void return lets the body be empty (no fall-through verifier trip).
    TypeId const voidTy = in.primitive(TypeKind::Void);
    std::array<TypeId, 1> const params{ptrS};
    TypeId const sig = in.fnSig(params, voidTy, CallConv::CcSysV);

    HirBuilder b{"toy"};
    HirNodeId const body = b.makeBlock(std::vector<HirNodeId>{});
    HirNodeId const fn   = b.makeFunction(sig, /*symbol=*/1, {}, body);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    EXPECT_NE(text.find("i32 ~16"), std::string::npos) << text;
    expectRoundTrip(hir, ctx);   // emit→parse→emit byte-identical

    // Re-parse the emitted struct text directly and confirm the align survives.
    TypeRegistry reg;
    DiagnosticReporter rep;
    TypeId const reparsed = parseTypeFromText(
        "struct \"S\" { i32 ~16 }", in, reg, rep);
    EXPECT_TRUE(in.hasExplicitAligns(reparsed));
    EXPECT_EQ(in.explicitFieldAlign(reparsed, 0), 16u);
}

// ── D-LANG-TYPE-IDENTITY-VOCABULARY: the vocabulary-tag text round-trip ─────
//
// `hir_text` is the ONE type-text codec — it serves `.dsshir` dumps AND every
// shipped-descriptor `signature`/field/typedef spelling. If the EMIT side drops
// the vocabulary tag, or the PARSE side ignores it, a `long` silently
// re-collapses onto the anonymous `int` at every text boundary (a `.dsshir`
// reload, a static-link merge) and an FFI descriptor's `ptr<u64 "unsigned long">`
// stops matching the very C type it models.
//
// RED-ON-DISABLE: delete the tag emission and the emitted text loses the quoted
// name (and the decoded TypeId collapses onto the anonymous one); delete the tag
// parse and the reparsed type is anonymous, so the two emits differ.

TEST(ParseTypeFromText, VocabularyTagRoundTripsAndStaysDistinct) {
    TypeInterner interner{CompilationUnitId{13}};
    TypeRegistry reg;
    DiagnosticReporter rep;

    // The tagged form and the bare form are DIFFERENT types at the SAME
    // representation — the entire point of the split.
    TypeId const tagged = parseTypeFromText("i64 \"long\"", interner, reg, rep);
    TypeId const anon   = parseTypeFromText("i64", interner, reg, rep);
    ASSERT_TRUE(tagged.valid() && anon.valid());
    EXPECT_NE(tagged.v, anon.v);
    EXPECT_EQ(interner.kind(tagged), TypeKind::I64);
    EXPECT_EQ(std::string{interner.vocabularyName(tagged)}, "long");
    EXPECT_TRUE(interner.vocabularyName(anon).empty())
        << "the BARE core spells the ANONYMOUS representative — that is what "
           "`int`/`short`/`char` are, so it must never acquire a tag";
    EXPECT_TRUE(interner.sameRepresentation(tagged, anon));

    // Two spellings of the same tag dedup to ONE TypeId; a different tag does not.
    EXPECT_EQ(parseTypeFromText("i64 \"long\"", interner, reg, rep).v, tagged.v);
    EXPECT_NE(parseTypeFromText("i64 \"long long\"", interner, reg, rep).v, tagged.v);

    // The tag survives NESTED positions — the shape a descriptor actually uses
    // (`ptr<...>` out-params, struct fields, FnSig params/results).
    TypeId const pl = parseTypeFromText("ptr<u64 \"unsigned long\">", interner, reg, rep);
    ASSERT_TRUE(pl.valid());
    ASSERT_EQ(interner.kind(pl), TypeKind::Ptr);
    EXPECT_EQ(std::string{interner.vocabularyName(interner.operands(pl)[0])},
              "unsigned long");
    EXPECT_NE(pl.v, parseTypeFromText("ptr<u64>", interner, reg, rep).v)
        << "`unsigned long *` and an anonymous `u64 *` are NOT the same pointer "
           "type — this inequality is what makes LPDWORD match `unsigned long *`";

    TypeId const st = parseTypeFromText(
        "struct \"timeval\" {i64 \"long\", i64 \"long\"}", interner, reg, rep);
    ASSERT_TRUE(st.valid());
    ASSERT_EQ(interner.kind(st), TypeKind::Struct);
    ASSERT_EQ(interner.operands(st).size(), 2u);
    EXPECT_EQ(std::string{interner.vocabularyName(interner.operands(st)[0])}, "long");
    EXPECT_NE(st.v, parseTypeFromText("struct \"timeval\" {i64, i64}",
                                      interner, reg, rep).v)
        << "the SAME tag with differently-tagged fields is a DIFFERENT struct — "
           "the cross-descriptor divergence that produced an include-order-"
           "dependent member-access failure";

    TypeId const fn = parseTypeFromText(
        "fn(ptr<i32 \"long\">, i32) -> i64 \"long long\"", interner, reg, rep);
    ASSERT_TRUE(fn.valid());
    EXPECT_EQ(std::string{interner.vocabularyName(interner.fnResult(fn))},
              "long long");
    EXPECT_EQ(rep.errorCount(), 0u);
}

// The EMIT side, through the full `.dsshir` emit→parse→emit stability check —
// the half `tests/hir/test_hir_text.cpp` never exercised. A tagged primitive
// must print its tag AND survive the reparse; the untagged control must stay
// anonymous (a blanket "always print a tag" would break every existing dump).
TEST(HirText, VocabularyTagEmitsAndSurvivesReparse) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const lng   = in.primitive(TypeKind::I64, "long");
    TypeId const ull   = in.primitive(TypeKind::U64, "unsigned long long");
    TypeId const anon  = in.primitive(TypeKind::I64);          // the control
    TypeId const ptrL  = in.pointer(lng);
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const sig   = in.fnSig({}, voidT, CallConv::CcSysV);

    HirBuilder b{"toy"};
    HirNodeId const t1 = b.makeTypeRef(lng);
    HirNodeId const t2 = b.makeTypeRef(ull);
    HirNodeId const t3 = b.makeTypeRef(anon);
    HirNodeId const t4 = b.makeTypeRef(ptrL);
    HirNodeId const body = b.makeBlock(std::vector<HirNodeId>{
        b.makeExprStmt(t1), b.makeExprStmt(t2), b.makeExprStmt(t3),
        b.makeExprStmt(t4), b.makeReturn()});
    HirNodeId const fn   = b.makeFunction(sig, 1, {}, body);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    // Byte-identical emit→parse→emit: if the tag were emitted but not parsed,
    // the SECOND emit would print the anonymous form and this fails.
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("i64 \"long\""), std::string::npos)
        << "the vocabulary tag must be EMITTED — text that carries only the "
           "representation cannot express identity:\n" << text;
    EXPECT_NE(text.find("u64 \"unsigned long long\""), std::string::npos) << text;
    EXPECT_NE(text.find("ptr<i64 \"long\">"), std::string::npos) << text;

    // The untagged control prints BARE — zero churn for every existing dump.
    DiagnosticReporter r;
    std::string const only = emitHir(hir, ctx, r);
    HirBuilder b2{"toy"};
    HirNodeId const c1   = b2.makeTypeRef(anon);
    HirNodeId const cb   = b2.makeBlock(std::vector<HirNodeId>{
        b2.makeExprStmt(c1), b2.makeReturn()});
    HirNodeId const cfn  = b2.makeFunction(sig, 1, {}, cb);
    HirNodeId const croot = b2.makeModule(std::vector<HirNodeId>{cfn});
    Hir ctlHir = std::move(b2).finish(croot);
    DiagnosticReporter r2;
    std::string const ctlText = emitHir(ctlHir, ctx, r2);
    EXPECT_EQ(ctlText.find('"' + std::string{"long"}), std::string::npos)
        << "an ANONYMOUS primitive must print with no tag at all:\n" << ctlText;
    (void)only;
}
