// HR7 `.dsshir` text-format tests: in-memory byte-identical round-trip across the
// node/type/attribute surface, verify-on-load, parse-error reporting, and a
// golden corpus (DSS_REFRESH_GOLDENS=1 to regenerate, mirroring test_corpus.cpp).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_span.hpp"
#include "core/types/target_schema.hpp"  // OperandKindFilter — the operand-form half of a constraint binding
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
#include "hir/hir_inline_asm.hpp"
#include "hir/hir_text.hpp"
#include "repo_root.hpp"

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
    // Inline-asm P5: the same rethreading, for the same reason. Without it a
    // module carrying an asm descriptor re-emits through the `#<handle>`
    // fallback and byte-identity fails on the SECOND emit -- which would look
    // like a writer bug and is really an un-threaded pool.
    if (ctx.inlineAsmPool) ctx2.inlineAsmPool = &res->inlineAsmPool;

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

    HirBuilder b{"c"};
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

    HirBuilder b{"c"};
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

// The `.dsshir` corpus tree, via the ONE test-side resolver (`repo_root.hpp`:
// $DSS_CONFIG_ROOT → the CMake-baked repo root → the cwd ancestor walk). The
// private cwd walk this replaces resolved nothing in an OUT-OF-TREE build —
// that cwd has no `src/dss-config` in its ancestry — and then called
// `std::abort()`, which kills the whole test BINARY, so one unresolvable corpus
// root cost every sibling test here its verdict. `repoRoot()` throws, and
// GoogleTest reports a throw as a failure of the one running test.
[[nodiscard]] fs::path findHirCorpus() {
    return dss::test::repoRoot() / "tests" / "hir" / "corpus";
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

// ── inline-asm P5 (D-CSUBSET-INLINE-ASM-OPERANDS) ───────────────────────────

// The BARE BARRIER must render EXACTLY as it did before P5 -- a lone
// `inline_asm` line, no descriptor, no children.
// * THIS IS THE COMPATIBILITY PIN, and it is the reason `payload == 0` is the
// sentinel rather than "index 0 is the first descriptor": every pre-P5 golden
// and every consumer that treats `InlineAsm` as a leaf stays byte-identical.
TEST(HirText, InlineAsmBareBarrierStillRendersAsALoneKeyword) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i64 = in.primitive(TypeKind::I64);
    TypeId const sig = in.fnSig({}, i64, CallConv::CcSysV);

    HirBuilder b{"c"};
    HirNodeId const asmN = b.addLeaf(HirKind::InlineAsm);
    HirNodeId const ret  = b.makeReturn(b.makeLiteral(i64, 0));
    HirNodeId const body = b.makeBlock(std::vector<HirNodeId>{asmN, ret});
    HirNodeId const fn   = b.makeFunction(sig, /*symbol=*/1, {}, body);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirLiteralPool pool;
    (void)pool.add(HirLiteralValue{std::int64_t{0}, TypeKind::I64});
    HirTextContext ctx;
    ctx.interner = &in; ctx.symbolNames = &names; ctx.literalPool = &pool;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("\n      inline_asm\n"), std::string::npos)
        << "the barrier form must stay a lone keyword line:\n" << text;
    EXPECT_EQ(text.find("inline_asm #"), std::string::npos)
        << "payload 0 must not render as a handle";
}

// A FULL descriptor -- every field populated -- must survive
// emit -> parse -> re-emit byte-identically, and the parse must re-mint the
// pool handle rather than trusting one from the text.
TEST(HirText, InlineAsmDescriptorRoundTripsWithEveryField) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i64 = in.primitive(TypeKind::I64);
    TypeId const sig = in.fnSig({}, i64, CallConv::CcSysV);

    HirInlineAsmPool asmPool;
    HirInlineAsmDescriptor d;
    // ⚠ NO `\n` IN THIS TEMPLATE, AND THE REASON IS A REAL FINDING RATHER
    // THAN A TEST CONVENIENCE. `hir_text`'s shared `quote()` escapes `"` and
    // `\\` and NOTHING ELSE, so a template containing a newline is emitted
    // with a RAW newline inside the quoted string. It still round-trips
    // byte-identically (the lexer accepts it), but the rendered `.dsshir`
    // has a string spanning two lines. That is pre-existing and shared with
    // every `str` literal value, so it is reported rather than changed here.
    // A multi-instruction template covering the escape path lives in the
    // dedicated case below.
    d.templateText           = "mov %0, %1; nop";
    d.outputCount            = 1;
    d.isGoto                 = true;
    d.isExtended             = true;
    d.clobbersMemory         = true;
    d.clobbersConditionCodes = true;
    d.clobbers               = {"rax", "rbx"};
    d.labelOrdinals          = {2, 7};
    // !! THE LABEL ORDINAL AND THE LABEL'S POSITIONAL SPELLING ARE DIFFERENT
    // NUMBERS, and they are deliberately different here. The ordinal is the
    // per-FUNCTION label id (2 and 7 -- whatever the enclosing function handed
    // out); the positional spelling is `operandCount + position` within THIS
    // statement, so with two operands the two labels are `%l2` and `%l3`. A
    // writer that "helpfully" rendered the ordinal as the spelling, or a
    // consumer that re-derived one from the other, is caught by this pair and
    // by nothing else in the suite.
    d.labelSpellings         = {{"%l[done]", "%l2"}, {"%l[again]", "%l3"}};
    {
        HirInlineAsmOperand out;
        out.symbolicName        = "dst";
        out.spellings           = {"%0", "%[dst]"};
        out.constraint          = parseAsmConstraint("=&r").value;
        out.isOutput            = true;
        out.regClassResolved    = true;
        out.regClass            = 3;
        d.operands.push_back(std::move(out));

        HirInlineAsmOperand inp;
        // NO symbolic name was written, so this operand answers to ONE
        // spelling. A one-element group and a two-element group in the same
        // dump is what proves the section is length-driven rather than fixed.
        inp.spellings     = {"%1"};
        inp.constraint    = parseAsmConstraint("a").value;
        inp.fixedRegister = "rax";
        d.operands.push_back(std::move(inp));

        // ★★★ THE THIRD BINDING ARM, AND THE FIELD THIS CASE'S OWN COMMENT
        // PROMISED TO CATCH AND DID NOT. `TargetAsmConstraint::binds` is
        // three-armed; the two operands above exercise `registerClass` and
        // `register`, and until this one existed NOTHING here set
        // `operandKind*` — so the writer could (and did) drop the pair with
        // every assertion in this case still green, because a smaller
        // descriptor still round-trips byte-identically.
        // ⚠ The consequence is not a cosmetic loss: `!regClassResolved &&
        // !operandKindResolved` is byte-identical to "no target was in scope",
        // and `hir_to_mir` refuses that operand saying the letter was never
        // bound to a processor — a FALSE reason for a letter both shipped
        // targets declare (D-ASM-MEMORY-CONSTRAINT-REFUSED-DESPITE-BEING-DECLARED,
        // reproduced one tier over).
        HirInlineAsmOperand mem;
        mem.spellings           = {"%2"};
        mem.constraint          = parseAsmConstraint("m").value;
        mem.operandKindResolved = true;
        mem.operandKind =
            static_cast<std::uint8_t>(OperandKindFilter::MemBase);
        d.operands.push_back(std::move(mem));
    }
    std::uint32_t const handle = asmPool.add(std::move(d));
    EXPECT_EQ(handle, 1u) << "handles are 1-BASED so 0 stays the no-descriptor "
                             "sentinel and cannot be produced by add()";

    HirLiteralPool lits;
    HirBuilder b{"c"};
    std::vector<HirNodeId> const kids{b.makeRef(i64, /*symbol=*/2),
                                      b.makeRef(i64, /*symbol=*/3),
                                      b.makeRef(i64, /*symbol=*/3)};
    HirNodeId const asmN = b.addParent(HirKind::InlineAsm, kids, InvalidType, handle);
    HirNodeId const ret  = b.makeReturn(b.makeLiteral(i64, lits.add(
                              HirLiteralValue{std::int64_t{0}, TypeKind::I64})));
    HirNodeId const body = b.makeBlock(std::vector<HirNodeId>{asmN, ret});
    HirNodeId const fn   = b.makeFunction(sig, /*symbol=*/1, {}, body);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main", "x", "y"};
    HirTextContext ctx;
    ctx.interner = &in; ctx.symbolNames = &names;
    ctx.literalPool = &lits; ctx.inlineAsmPool = &asmPool;
    std::string const text = expectRoundTrip(hir, ctx);

    // Each rendered fact pinned by NAME, so a field silently dropped from the
    // writer reds here rather than surviving as a still-byte-identical
    // round-trip of a SMALLER descriptor (which is what a pure identity check
    // would happily accept).
    EXPECT_NE(text.find(R"(inline_asm "mov %0, %1; nop" { extended goto mem cc)"),
              std::string::npos) << text;
    EXPECT_NE(text.find("outputs 1 operands ("), std::string::npos) << text;
    EXPECT_NE(text.find(R"("=&r" [dst] spells ( "%0", "%[dst]" ) class 3 -> )"),
              std::string::npos) << text;
    EXPECT_NE(text.find(R"("a" spells ( "%1" ) pin "rax" -> )"),
              std::string::npos) << text;
    // Spelled by NAME, not by ordinal — `OperandKindFilter` is open-ended
    // (`imm32` is 1, `membase` 3, and future width filters insert between), so
    // a stored `.dsshir` written with an ordinal would silently mean something
    // else after the next enumerator lands.
    EXPECT_NE(text.find(R"("m" spells ( "%2" ) operand_kind membase -> )"),
              std::string::npos) << text;
    EXPECT_NE(text.find(R"(clobbers ( "rax", "rbx" ))"), std::string::npos) << text;
    EXPECT_NE(
        text.find(
            R"(labels ( L2 spells ( "%l[done]", "%l2" ), L7 spells ( "%l[again]", "%l3" ) ))"),
        std::string::npos) << text;

    // The PARSE must reconstruct the descriptor, not just the bytes.
    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{9}, r);
    ASSERT_TRUE(res->ok);
    ASSERT_EQ(res->inlineAsmPool.size(), 1u);
    auto const& back = res->inlineAsmPool.at(1);
    EXPECT_EQ(back.templateText, "mov %0, %1; nop");
    EXPECT_EQ(back.outputCount, 1u);
    EXPECT_TRUE(back.isGoto);
    EXPECT_TRUE(back.isExtended);
    EXPECT_TRUE(back.clobbersMemory);
    EXPECT_TRUE(back.clobbersConditionCodes);
    EXPECT_TRUE(back.protectsRegisters());
    ASSERT_EQ(back.operands.size(), 3u);
    EXPECT_EQ(back.operands[0].symbolicName, "dst");
    EXPECT_EQ(back.operands[0].constraint.raw, "=&r");
    EXPECT_TRUE(back.operands[0].constraint.isOutput);
    EXPECT_TRUE(back.operands[0].constraint.earlyClobber);
    EXPECT_TRUE(back.operands[0].isOutput);
    EXPECT_EQ(back.operands[0].regClass, 3);
    EXPECT_FALSE(back.operands[1].isOutput)
        << "isOutput is recovered from outputCount, not re-read from the text";
    EXPECT_EQ(back.operands[1].fixedRegister, "rax");
    EXPECT_EQ(back.clobbers, (std::vector<std::string>{"rax", "rbx"}));
    EXPECT_EQ(back.labelOrdinals, (std::vector<std::uint32_t>{2u, 7u}));
    // The spellings are the field every tier below HIR COMPARES against, so a
    // round trip that lost them would hand the next tier an asm statement whose
    // operands answer to no `%N` at all. Asserted by CONTENT, per group.
    EXPECT_EQ(back.operands[0].spellings,
              (std::vector<std::string>{"%0", "%[dst]"}));
    EXPECT_EQ(back.operands[1].spellings, (std::vector<std::string>{"%1"}));
    // ★★ THE VALUE, NOT MERELY "PARSED WITHOUT COMPLAINT". The field this pin
    // exists for drops SILENTLY — a reader that never sets it produces a clean
    // parse of a descriptor that now says something different — so a check for
    // "no diagnostic" would have been green on exactly the defect.
    EXPECT_FALSE(back.operands[0].operandKindResolved)
        << "a class-bound letter resolves NO operand form; `binds` names one arm";
    EXPECT_FALSE(back.operands[1].operandKindResolved);
    EXPECT_TRUE(back.operands[2].operandKindResolved)
        << "the form-bound operand came back as if no target had been in scope — "
           "which is the state that makes hir_to_mir refuse a declared letter "
           "with a false reason";
    EXPECT_EQ(back.operands[2].operandKind,
              static_cast<std::uint8_t>(OperandKindFilter::MemBase));
    EXPECT_FALSE(back.operands[2].regClassResolved);
    EXPECT_TRUE(back.operands[2].fixedRegister.empty());
    EXPECT_EQ(back.labelSpellings,
              (std::vector<std::vector<std::string>>{{"%l[done]", "%l2"},
                                                     {"%l[again]", "%l3"}}));
}

// The OTHER half of the label section: a descriptor whose labels carry NO
// spellings at all. That is the honoured-absence case -- a language whose
// `templateLabelPlaceholder` is declared `null` cannot spell a label reference
// -- and it must stay distinguishable from a descriptor whose spellings were
// DROPPED, which is what the writer's `spells` group being length-driven (and
// omitted when empty) buys.
//
// !! IT IS ALSO THE PIN FOR A REAL ROUND-TRIP HAZARD. The writer renders an
// empty group as nothing; the parser must therefore rebuild an empty group
// rather than skipping the entry, or the two lists come back different LENGTHS
// and `HirVerifier::checkInlineAsm` reds on text `emitHir` itself produced.
TEST(HirText, InlineAsmLabelsWithNoSpellingsStillRoundTripOneGroupPerOrdinal) {
    HirInlineAsmPool asmPool;
    HirInlineAsmDescriptor d;
    d.templateText   = "nop";
    d.isGoto         = true;
    d.isExtended     = true;
    d.labelOrdinals  = {4, 5};
    d.labelSpellings = {{}, {}};
    std::uint32_t const handle = asmPool.add(std::move(d));

    HirBuilder b{"c"};
    HirNodeId const asmN = b.addLeaf(HirKind::InlineAsm, InvalidType, handle);
    HirNodeId const body = b.makeBlock(std::vector<HirNodeId>{asmN});
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{body});
    Hir hir = std::move(b).finish(root);

    HirTextContext ctx;
    ctx.inlineAsmPool = &asmPool;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("labels ( L4, L5 )"), std::string::npos)
        << "an empty spelling group renders as NOTHING, not as an empty pair of "
           "parens:\n" << text;

    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{17}, r);
    ASSERT_TRUE(res->ok) << text;
    ASSERT_EQ(res->inlineAsmPool.size(), 1u);
    auto const& back = res->inlineAsmPool.at(1);
    EXPECT_EQ(back.labelOrdinals, (std::vector<std::uint32_t>{4u, 5u}));
    EXPECT_EQ(back.labelSpellings, (std::vector<std::vector<std::string>>{{}, {}}))
        << "one group per ordinal, or the verifier reds on the writer's own "
           "output";
}

// !! THE NO-POOL ARM IS NOT A SILENT DEGRADATION, and this pin is the
// difference. `lit` with no pool renders `lit #3` and is still recognisably a
// literal; an asm statement rendered without its operands would read as a BARE
// BARRIER -- a different program. So the writer emits an explicit handle form
// AND reports.
TEST(HirText, InlineAsmWithNoPoolReportsRatherThanRenderingABarrier) {
    HirBuilder b{"c"};
    HirNodeId const asmN = b.addLeaf(HirKind::InlineAsm, InvalidType, /*payload=*/4);
    HirNodeId const body = b.makeBlock(std::vector<HirNodeId>{asmN});
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{body});
    Hir hir = std::move(b).finish(root);

    HirTextContext ctx;   // deliberately NO inlineAsmPool
    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    EXPECT_NE(text.find("inline_asm #4"), std::string::npos) << text;
    EXPECT_GT(countCode(r, DiagnosticCode::H_TextMalformed), 0u)
        << "an unresolvable descriptor handle must be REPORTED, never quietly "
           "rendered as the barrier form";
}

// !! THE PIN FOR A BUG THIS FORMAT ALMOST SHIPPED. The descriptor's flags are
// bare keywords and one of them is `goto` -- which is ALSO a statement keyword,
// and this lexer is newline-blind. Before the tail was braced,
//
//     inline_asm "nop"
//     goto L1
//
// parsed the NEXT STATEMENT's `goto` as this asm's goto FLAG and then choked on
// `L1`. Found by reading the grammar rather than by a failing test, which is
// exactly why the test now exists: the shape only breaks when an asm statement
// is IMMEDIATELY FOLLOWED by a statement whose keyword collides, so no
// single-statement round-trip would ever have caught it.
TEST(HirText, InlineAsmDoesNotSwallowAFollowingGotoStatement) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i64 = in.primitive(TypeKind::I64);
    TypeId const sig = in.fnSig({}, i64, CallConv::CcSysV);

    HirInlineAsmPool asmPool;
    HirInlineAsmDescriptor d;
    d.templateText = "nop";          // NO flags and NO sections: the bare tail
    std::uint32_t const handle = asmPool.add(std::move(d));

    HirBuilder b{"c"};
    HirNodeId const asmN  = b.addLeaf(HirKind::InlineAsm, InvalidType, handle);
    HirNodeId const gotoN = b.makeGotoStmt(1);
    HirNodeId const lbl   = b.makeLabelStmt(1, b.makeReturn(std::nullopt));
    HirNodeId const body  = b.makeBlock(std::vector<HirNodeId>{asmN, gotoN, lbl});
    HirNodeId const fn    = b.makeFunction(sig, /*symbol=*/1, {}, body);
    HirNodeId const root  = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx;
    ctx.interner = &in; ctx.symbolNames = &names; ctx.inlineAsmPool = &asmPool;
    std::string const text = expectRoundTrip(hir, ctx);

    // The two statements must survive as TWO statements -- the round-trip check
    // above would already red, but this makes the failure name itself.
    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{11}, r);
    ASSERT_TRUE(res->ok) << text;
    ASSERT_EQ(res->inlineAsmPool.size(), 1u);
    EXPECT_FALSE(res->inlineAsmPool.at(1).isGoto)
        << "the FOLLOWING statement's `goto` was eaten as this asm's goto flag";
    EXPECT_NE(text.find("\n      inline_asm \"nop\"\n"), std::string::npos)
        << "a bare template must emit no brace group at all:\n" << text;
}

// The multi-instruction template the case above deliberately did NOT use.
// A real asm template is usually several instructions joined by `\n\t`, so the
// round trip has to survive one -- and it does, byte-identically. What it does
// NOT do is ESCAPE the newline: `hir_text`'s shared `quote()` escapes only `"`
// and `\`, so the emitted `.dsshir` carries a string literal spanning two
// lines. Pre-existing and shared with every `str` literal value; pinned here as
// the CURRENT behaviour so a later change to `quote()` reds deliberately
// instead of silently reflowing every asm template in every golden.
TEST(HirText, InlineAsmTemplateWithANewlineStillRoundTripsByteIdentically) {
    HirInlineAsmPool asmPool;
    HirInlineAsmDescriptor d;
    d.templateText = "nop\n\tnop";
    std::uint32_t const handle = asmPool.add(std::move(d));

    HirBuilder b{"c"};
    HirNodeId const asmN = b.addLeaf(HirKind::InlineAsm, InvalidType, handle);
    HirNodeId const body = b.makeBlock(std::vector<HirNodeId>{asmN});
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{body});
    Hir hir = std::move(b).finish(root);

    HirTextContext ctx;
    ctx.inlineAsmPool = &asmPool;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("nop\n\tnop"), std::string::npos)
        << "the newline is emitted RAW today (quote() escapes only \" and \\):\n"
        << text;

    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{13}, r);
    ASSERT_TRUE(res->ok) << text;
    ASSERT_EQ(res->inlineAsmPool.size(), 1u);
    EXPECT_EQ(res->inlineAsmPool.at(1).templateText, "nop\n\tnop")
        << "the template must survive the round trip byte for byte";
}

// ── D-HIR-TEXT-INLINE-ASM-OPERAND-KIND-DROPPED-IN-TRANSIT ───────────────────
//
// The REFUSAL half of the operand-form clause. A name has a failure arm and an
// ordinal does not, which is the whole reason this field is spelled rather than
// numbered; this asserts the arm exists and names what it accepts, so a
// hand-edited `.dsshir` cannot mint an operand form no build defines.
//
// ⚠ THE POSITIVE ARM ALONE WOULD NOT SAY THIS. A reader that silently ignored an
// unrecognized spelling would still pass every assertion in
// `InlineAsmDescriptorRoundTripsWithEveryField`, and would drop the binding for
// exactly the reason that row exists.
TEST(HirText, InlineAsmOperandKindThatNamesNoFormIsRefusedWithTheAcceptedSet) {
    std::string const text =
        "dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
        "  function %1 : fn() -> void {\n    block {\n"
        "      inline_asm \"nop %0\" { extended outputs 0 operands ( \"m\" "
        "operand_kind not_a_form -> lit int 0 : i32 ) }\n"
        "      return\n    }\n  }\n}\n";
    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{31}, r);
    EXPECT_FALSE(res->ok);
    std::string all;
    for (auto const& d : r.all()) { all += d.actual; all += '\n'; }
    EXPECT_NE(all.find("unknown inline-asm operand kind 'not_a_form'"),
              std::string::npos) << all;
    EXPECT_NE(all.find("'membase'"), std::string::npos)
        << "the refusal must name the accepted set:\n" << all;
    EXPECT_NE(all.find("'imm32'"), std::string::npos) << all;
}

// The form a real target actually produces, driven as TEXT rather than as a
// hand-built descriptor: both shipped targets declare `"i"` → `imm32`, and the
// front end resolves it to `operandKindResolved` with no register class at all.
// A writer/reader pair that dropped the field would hand `hir_to_mir` an operand
// indistinguishable from one analyzed with no target in scope.
TEST(HirText, InlineAsmImmediateFormOperandSurvivesTheTextTier) {
    std::string const text =
        "dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
        "  function %1 : fn() -> void {\n    block {\n"
        "      inline_asm \"nop %0\" { extended outputs 0 operands ( \"i\" "
        "operand_kind imm32 -> lit int 7 : i32 ) }\n"
        "      return\n    }\n  }\n}\n";
    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{32}, r);
    ASSERT_TRUE(res->ok);
    ASSERT_EQ(res->inlineAsmPool.size(), 1u);
    auto const& d = res->inlineAsmPool.at(1);
    ASSERT_EQ(d.operands.size(), 1u);
    EXPECT_TRUE(d.operands[0].operandKindResolved);
    EXPECT_EQ(d.operands[0].operandKind,
              static_cast<std::uint8_t>(OperandKindFilter::ImmInt));
    EXPECT_FALSE(d.operands[0].regClassResolved)
        << "a form-bound letter resolves no register class — and the pair of "
           "flags is what tells that apart from 'no target was in scope'";
}

// D-HIR-TEXT-INLINE-ASM-REGISTER-CLASS-ORDINAL-IS-UNVALIDATED
// The `class <N>` sibling of the operand-form clause. Found while adding that
// clause: this position read an arbitrary integer straight onto the wire, so a
// hand-written `.dsshir` could mint a `TargetRegClass` no build defines and load
// CLEAN — `bindAsmOperand`'s only guard is `cls == None`, which a garbage ordinal
// passes on its way to a register file that does not exist.
//
// ⓘ THE WIRE FORM IS STILL AN ORDINAL and deliberately so: it is what the writer
// emits and what stored goldens carry. Only the acceptance changed.
TEST(HirText, InlineAsmRegisterClassOrdinalOutsideTheEnumIsRefused) {
    std::string const text =
        "dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
        "  function %1 : fn() -> void {\n    block {\n"
        "      inline_asm \"nop %0\" { extended outputs 0 operands ( \"r\" "
        "class 200 -> lit int 0 : i32 ) }\n"
        "      return\n    }\n  }\n}\n";
    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{33}, r);
    EXPECT_FALSE(res->ok);
    std::string all;
    for (auto const& d : r.all()) { all += d.actual; all += '\n'; }
    EXPECT_NE(all.find("names no TargetRegClass"), std::string::npos) << all;
    EXPECT_NE(all.find("'gpr'"), std::string::npos)
        << "the refusal must name the accepted set:\n" << all;
}

// The matched positive, so the arm above cannot be green because the position
// stopped accepting anything at all. A class the enum DOES define still loads
// and still round-trips its value.
TEST(HirText, InlineAsmRegisterClassOrdinalInsideTheEnumStillLoads) {
    std::string const text =
        "dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
        "  function %1 : fn() -> void {\n    block {\n"
        "      inline_asm \"nop %0\" { extended outputs 0 operands ( \"r\" "
        "class 1 -> lit int 0 : i32 ) }\n"
        "      return\n    }\n  }\n}\n";
    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{34}, r);
    ASSERT_TRUE(res->ok);
    ASSERT_EQ(res->inlineAsmPool.size(), 1u);
    ASSERT_EQ(res->inlineAsmPool.at(1).operands.size(), 1u);
    EXPECT_TRUE(res->inlineAsmPool.at(1).operands[0].regClassResolved);
    EXPECT_EQ(res->inlineAsmPool.at(1).operands[0].regClass,
              static_cast<std::uint8_t>(TargetRegClass::GPR));
}
