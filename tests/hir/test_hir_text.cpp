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
#include "hir/hir_verifier.hpp"   // the verdict pins: a finding no text can spell
#include "repo_root.hpp"

#include "core/substrate/checked_file_read.hpp"   // the ONE checked whole-file read

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
    // v2 header + `buffers` section: both are file CONTENT, so a re-emit that
    // dropped them would not be a round trip. Threaded UNCONDITIONALLY (not
    // behind an `if (ctx.producer.empty())` guard) because the empty producer is
    // a legitimate VALUE — `producer ""` — not an absent field.
    ctx2.producer      = res->producer;
    ctx2.bufferNames   = &res->bufferNames;
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
    EXPECT_NE(text.find("dsshir 5\nproducer \"\"\n"), std::string::npos);
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
            std::string("dsshir 5\nproducer \"\"\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
                        "  function %1 : fn() -> void {\n    block {\n      expr ")
            + std::string(body) + "\n      return void\n    }\n  }\n}\n";
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

// D-C-ATOMIC-COMPOUND-ASSIGNMENT-AND-INCREMENT-ARE-A-LOAD-THEN-A-SEPARATE-STORE:
// `rmw %<sym> : <type> (<target>, <update>)` round-trips, and its PAYLOAD symbol —
// the binding the update reads the observed value through — reaches the
// artifact's symbol table. ★ That second half is the one a writer could drop and
// still produce parseable text: without `carriesSymbol` the handle would name a
// symbol the table never defines, and the update's `ref` to it would read back
// as a DIFFERENT symbol than the node binds.
// RED-ON-DISABLE: remove `ReadModifyWrite` from `carriesSymbol` → the round trip
// or the `"old"` name assertion reds; remove the parser's `Rmw` arm → it refuses
// its own writer's keyword.
TEST(HirText, RoundTripReadModifyWrite) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId i32   = in.primitive(TypeKind::I32);
    TypeId voidT = in.primitive(TypeKind::Void);
    TypeId sig   = in.fnSig({}, voidT, CallConv::CcSysV);

    HirBuilder b{"toy"};
    HirNodeId target = b.makeRef(i32, 2);                  // the object
    HirNodeId oldRd  = b.makeRef(i32, 3);                  // the observed value
    HirNodeId one    = b.makeLiteral(i32, 0);
    HirNodeId update = b.makeBinaryOp(HirOpKind::Add, oldRd, one, i32);
    HirNodeId rmw    = b.makeReadModifyWrite(target, update, 3, i32);
    HirNodeId body   = b.makeBlock(std::vector<HirNodeId>{b.makeExprStmt(rmw), b.makeReturn()});
    HirNodeId fn     = b.makeFunction(sig, 1, {}, body);
    HirNodeId root   = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main", "g", "old"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("rmw %"), std::string::npos) << text;
    EXPECT_NE(text.find("\"old\""), std::string::npos)
        << "the read-modify-write's binding must be in the symbol table\n" << text;
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

// P68 round 8 (lane `ht`, part 2): a signature that KEEPS a parameter of type void —
// the C front end's `void f(void v);`, gcc's meaning, a type distinct from
// `void(void)` — is spelled `fn(void) -> void`, and `fn() -> void` is the signature
// with no parameter at all. The text must keep them apart in BOTH directions: a
// writer that dropped the void, or a reader that normalized `fn(void)` to no
// parameters, would silently merge two types the front end holds distinct (and a
// same-TU `void f(void)` beside `void f(void v)` is "conflicting types" — gcc).
TEST(HirText, AVoidParameterIsSpelledAndReadDistinctFromNoParameter) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const withVoid = in.fnSig(std::array{voidT}, voidT, CallConv::CcSysV);
    TypeId const none     = in.fnSig(std::span<TypeId const>{}, voidT, CallConv::CcSysV);
    ASSERT_NE(withVoid, none);

    HirBuilder b{"c"};
    HirNodeId const f = b.makeExternFunction(withVoid, 1, std::span<HirNodeId const>{});
    HirNodeId const g = b.makeExternFunction(none, 2, std::span<HirNodeId const>{});
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{f, g});
    Hir hir = std::move(b).finish(root);
    std::vector<std::string> names{"", "f", "g"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("extern_function %1 : fn(void) -> void"), std::string::npos) << text;
    EXPECT_NE(text.find("extern_function %2 : fn() -> void"), std::string::npos) << text;

    // …and the reader's one `parseType` production interns them as two signatures,
    // the first with exactly one parameter, of type void.
    TypeInterner rin{CompilationUnitId{2}};
    TypeRegistry reg;
    DiagnosticReporter rep;
    TypeId const readVoid = parseTypeFromText("fn(void) -> void", rin, reg, rep);
    TypeId const readNone = parseTypeFromText("fn() -> void", rin, reg, rep);
    ASSERT_TRUE(readVoid.valid() && readNone.valid());
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_NE(readVoid, readNone);
    ASSERT_EQ(rin.fnParams(readVoid).size(), 1u);
    EXPECT_EQ(rin.kind(rin.fnParams(readVoid)[0]), TypeKind::Void);
    EXPECT_EQ(rin.fnParams(readNone).size(), 0u);
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
    // ⚠ `buf 1`, though the map was set with `BufferId{3}` — and the substitution
    // is the POINT, not an accident. A `.dsshir` names buffers by an
    // ARTIFACT-LOCAL handle (1..N over the buffers this module actually uses),
    // exactly as it names symbols by `%1..%N`, because `BufferId` is a
    // PROCESS-GLOBAL monotonic counter: printing it put "how many files this
    // process had opened first" into the artifact bytes and made two emissions of
    // one input differ. The handle is a function of the module alone.
    EXPECT_NE(text.find("@loc(buf 1, 16..42)"), std::string::npos) << text;
    // And it is DEFINED by the file, so a reader is never handed an ordinal with
    // no row behind it.
    EXPECT_NE(text.find("buffers {\n  buf 1 \"\"\n}"), std::string::npos) << text;
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
        "dsshir 5\nproducer \"\"\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
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
    auto res = parseHir("dsshir 5\nproducer \"\"\nmodule \"toy\" {\n  $ % :\n}\n", CompilationUnitId{1}, r);
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
    auto res = parseHir("dsshir 5\nproducer \"\"\nmodule \"x\" {\n  @@@ garbage\n}\n", CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GT(countCode(r, DiagnosticCode::H_TextMalformed), 0u);
}

TEST(HirText, ParseUnknownSymbolReports) {
    // %9 referenced but only %1 declared.
    DiagnosticReporter r;
    auto res = parseHir(
        "dsshir 5\nproducer \"\"\nsymbols {\n  %1 \"a\"\n}\nmodule \"toy\" {\n"
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

// ── A READ SIGNATURE THE VERIFIER CANNOT USE: REFUSED BY NAME, NEVER A KILL ──
//
// ✔MEASURED P68 (lane `ht`): `parseHir` ABORTED the process — `dss::substrate
// fatal: TypeInterner::get: TypeId out of range`, raised by
// `HirVerifier::checkReturnCompleteness` reading the kind of an UNRESOLVED
// signature result — by two roads: a signature whose parse RECOVERED from a
// stray token (pure v4 grammar does it: `fn(ptr<ptr<garbage junk>>) -> void`),
// and a CLEAN parse of `fn() -> invalid`. Two more signature shapes verified
// CLEAN: an unresolved parameter, and a function typed as something that is not
// a signature at all. An arm that CAN kill runs in a death-test CHILD, so a
// regression is a named red here — not a dead binary hiding every test after it,
// which is how the kill was first seen (a red-on-disable mutant of this file).
namespace {

std::string moduleWithDecls(std::string_view decls) {
    return std::string{"dsshir 5\nproducer \"\"\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"}
         + std::string{decls} + "}\n";
}

std::string moduleWithFunctionTyped(std::string_view type) {
    return moduleWithDecls(std::string{"  function %1 : "} + std::string{type}
                           + " {\n    block {\n    }\n  }\n");
}

// A death-test child's verdict, as its exit code — one code per way to fail, so
// the parent says WHICH one happened. A KILL is none of them, and what a kill
// looks like differs by leg — ✔MEASURED P68 with the kill mutant in place: WSL
// Release, terminated by SIGABRT; MSVC Release, exit status 3; MinGW Debug, exit
// status -1073740791 (0xC0000409, the fail-fast status).
enum ReadVerdict : int {
    kRefusedByName      = 0,    // refused, and a diagnostic names the cause
    kAccepted           = 11,   // the reader AND the verifier both passed it
    kRefusedButUnnamed  = 12,   // refused, but no diagnostic names the cause
    kVerifierCascade    = 13,   // the verifier ran on a tree the reader refused
};
constexpr char const* kReadVerdictLegend =
    "exit 11 = ACCEPTED; 12 = refused without naming the cause; 13 = the verifier "
    "ran on a tree the reader had already refused; anything else = the process was "
    "KILLED (a signal on POSIX; exit status 3 or -1073740791 = 0xC0000409 on Windows)";

[[noreturn]] void exitWithReadVerdict(std::string const& text, std::string_view cause,
                                      bool readerRefuses) {
    DiagnosticReporter r;
    auto const res = parseHir(text, CompilationUnitId{91}, r);
    if (res->ok) std::exit(kAccepted);
    bool named = false;
    bool verifierSpoke = false;
    for (auto const& d : r.all()) {
        if (d.actual.find(cause) != std::string::npos) named = true;
        if (d.code == DiagnosticCode::H_TypeUnresolved
            || d.code == DiagnosticCode::H_VerifierFailure) verifierSpoke = true;
    }
    if (!named) std::exit(kRefusedButUnnamed);
    if (readerRefuses && verifierSpoke) std::exit(kVerifierCascade);
    std::exit(kRefusedByName);
}

// Every diagnostic of `code` a clean in-process read of `text` produced.
std::vector<std::string> readAndCollect(std::string const& text, DiagnosticCode code,
                                        bool& ok) {
    DiagnosticReporter r;
    auto const res = parseHir(text, CompilationUnitId{92}, r);
    ok = res->ok;
    std::vector<std::string> out;
    for (auto const& d : r.all()) {
        if (d.code == code) out.push_back(d.actual);
    }
    return out;
}

} // namespace

// The reader's own refusal is the WHOLE answer: the tree error recovery built is
// never handed to the verifier, so nothing the verifier could say follows it.
TEST(HirTextSignatureRead, ASignatureTheReaderRecoveredFromIsRefusedByTheReaderAloneAndNeverKills) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    struct Arm { char const* what; char const* type; char const* says; };
    std::array<Arm, 6> const arms{{
        {"a stray token inside a nested pointer", "fn(ptr<ptr<garbage junk>>) -> void",
         "unknown type 'garbage'"},
        {"a stray token in the first of two parameters", "fn(ptr<garbage junk>, i32) -> void",
         "unknown type 'garbage'"},
        {"an inline composite carrying a bit-field width",
         "fn(ptr<struct \"S\" {u32 bits 3}>) -> void", "DEFINED ONCE"},
        {"an inline union carrying member alignments",
         "fn(ptr<union \"U\" {char ~0, i32 ~8}>) -> void", "DEFINED ONCE"},
        {"an inline composite carrying a whole-composite alignment",
         "fn(ptr<struct \"A\" packed aligned 16 {char, i32}>) -> void", "DEFINED ONCE"},
        {"an inline composite carrying a pack cap",
         "fn(ptr<struct \"P\" pack 2 {char, i64}>) -> void", "DEFINED ONCE"},
    }};
    for (Arm const& arm : arms) {
        std::string const text = moduleWithFunctionTyped(arm.type);
        EXPECT_EXIT(exitWithReadVerdict(text, arm.says, /*readerRefuses=*/true),
                    testing::ExitedWithCode(kRefusedByName), "")
            << arm.what << " — " << kReadVerdictLegend << ":\n" << text;
    }
}

// Every token of `fn() -> invalid` is one the writer prints, so the READER takes
// it — and it is the verifier that must refuse it, by name, without dying.
TEST(HirTextSignatureRead, AnUnresolvedResultIsRefusedByNameAndNeverKills) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    std::string const text = moduleWithFunctionTyped("fn() -> invalid");
    EXPECT_EXIT(exitWithReadVerdict(text, "no resolved type for its result",
                                    /*readerRefuses=*/false),
                testing::ExitedWithCode(kRefusedByName), "")
        << kReadVerdictLegend << ":\n" << text;
    if (HasFailure()) return;   // the child proved the read survives; only then read here
    bool ok = true;
    auto const found = readAndCollect(text, DiagnosticCode::H_TypeUnresolved, ok);
    EXPECT_FALSE(ok);
    ASSERT_EQ(found.size(), 1u) << text;
    EXPECT_NE(found[0].find("Function %1"), std::string::npos) << found[0];
}

TEST(HirTextSignatureRead, AnUnresolvedParameterIsRefusedByName) {
    struct Arm { char const* what; std::string text; char const* says; };
    std::array<Arm, 2> const arms{{
        {"a function", moduleWithFunctionTyped("fn(invalid) -> void"),
         "Function %1 (hir node #2) declares a signature with no resolved type for "
         "parameter 1 of 1"},
        {"a TYPED extern", moduleWithDecls("  extern_function %1 : fn(i32, invalid) -> void {\n  }\n"),
         "ExternFunction %1 (hir node #1) declares a signature with no resolved type for "
         "parameter 2 of 2"},
    }};
    for (Arm const& arm : arms) {
        bool ok = true;
        auto const found = readAndCollect(arm.text, DiagnosticCode::H_TypeUnresolved, ok);
        EXPECT_FALSE(ok) << arm.what << " with an unresolved parameter verified CLEAN:\n"
                         << arm.text;
        ASSERT_EQ(found.size(), 1u) << arm.what << ":\n" << arm.text;
        EXPECT_EQ(found[0], arm.says) << arm.what;
    }
    // CONTROLS: the same shapes resolved read clean, and an UNTYPED extern stays
    // legal — binary-only ingestion yields one, and nothing here may refuse it.
    for (std::string const& text : {moduleWithFunctionTyped("fn(i32) -> void"),
                                    moduleWithDecls("  extern_function %1 : fn(i32, i64) -> void {\n  }\n"),
                                    moduleWithDecls("  extern_function %1 {\n  }\n")}) {
        DiagnosticReporter r;
        EXPECT_TRUE(parseHir(text, CompilationUnitId{93}, r)->ok)
            << "a well-formed control was refused:\n" << text
            << (r.all().empty() ? std::string{} : r.all()[0].actual);
    }
}

TEST(HirTextSignatureRead, AFunctionTypedAsSomethingOtherThanASignatureIsRefusedByName) {
    struct Arm { char const* what; std::string text; char const* says; };
    std::array<Arm, 2> const arms{{
        {"a function typed `i32`", moduleWithFunctionTyped("i32"),
         "Function %1 (hir node #2) has a type of kind I32, not a function signature "
         "(FnSig)"},
        {"a typed extern typed `ptr<i32>`",
         moduleWithDecls("  extern_function %1 : ptr<i32> {\n  }\n"),
         "ExternFunction %1 (hir node #1) has a type of kind Ptr, not a function "
         "signature (FnSig)"},
    }};
    for (Arm const& arm : arms) {
        bool ok = true;
        auto const found = readAndCollect(arm.text, DiagnosticCode::H_VerifierFailure, ok);
        EXPECT_FALSE(ok) << arm.what << " verified CLEAN:\n" << arm.text;
        ASSERT_EQ(found.size(), 1u) << arm.what << ":\n" << arm.text;
        EXPECT_EQ(found[0], arm.says) << arm.what;
    }
}

// ONE diagnostic per declaration, naming every hole — pinned by a COUNT, because
// N lines for one broken signature is exactly the cascade the rule must not add.
TEST(HirTextSignatureRead, ASignatureWithSeveralHolesIsOneDiagnosticNamingEachHole) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    std::string const text = moduleWithFunctionTyped("fn(invalid, i32, invalid) -> invalid");
    // ⚠ Its RESULT is unresolved too — the shape that killed the process — so the
    // read must first prove it survives, in a child. Reading it in-process first
    // made the red-on-disable of that kill abort THIS binary (✔MEASURED P68: the
    // death-test arm above went red by name, and then this test killed the run).
    EXPECT_EXIT(exitWithReadVerdict(text, "no resolved type for its result",
                                    /*readerRefuses=*/false),
                testing::ExitedWithCode(kRefusedByName), "")
        << kReadVerdictLegend << ":\n" << text;
    if (HasFailure()) return;   // only a read proven to survive is repeated in-process
    bool ok = true;
    auto const found = readAndCollect(text, DiagnosticCode::H_TypeUnresolved, ok);
    EXPECT_FALSE(ok);
    ASSERT_EQ(found.size(), 1u) << "three holes in ONE signature must be ONE diagnostic:\n" << text;
    EXPECT_EQ(found[0], "Function %1 (hir node #2) declares a signature with no resolved "
                        "type for its result, parameter 1 of 3, parameter 3 of 3");
}

// ★ "DID THE READER REFUSE" IS THE READER'S OWN COUNT, NOT THE REPORTER'S DELTA.
// The reporter's dedup window drops a diagnostic identical to a recent one, so
// reading ONE malformed text twice into ONE reporter leaves the second read with
// no new stored error — and the delta called that text accepted.
TEST(HirTextSignatureRead, ARefusalIsCountedByTheReaderNotByTheReportersDelta) {
    std::string const text = moduleWithFunctionTyped("fn(ptr<garbage>) -> void");
    DiagnosticReporter shared;
    ASSERT_FALSE(parseHir(text, CompilationUnitId{94}, shared)->ok);
    std::size_t const afterFirst = shared.errorCount();
    auto const second = parseHir(text, CompilationUnitId{95}, shared);
    // THE PREMISE, MEASURED: the reporter really stored nothing new. If the dedup
    // window ever stops dropping the repeat, this arm stops testing the delta and
    // must say so rather than pass on a question it no longer asks.
    EXPECT_EQ(shared.errorCount(), afterFirst) << "premise gone: the repeat was stored";
    EXPECT_FALSE(second->ok)
        << "a text the reader REFUSED read as accepted, because the reporter stored "
           "no new error for it";

    // The standalone type path, the same way: `i32 i32` (a trailing token) is a
    // half-built `i32` that must never escape as a valid type.
    TypeInterner in{CompilationUnitId{96}};
    TypeRegistry reg;
    DiagnosticReporter tr;
    ASSERT_FALSE(parseTypeFromText("i32 i32", in, reg, tr).valid());
    std::size_t const typeErrors = tr.errorCount();
    EXPECT_FALSE(parseTypeFromText("i32 i32", in, reg, tr).valid())
        << "the second read of the same malformed type text escaped as a valid type";
    EXPECT_EQ(tr.errorCount(), typeErrors) << "premise gone: the repeat was stored";
}

// ── THE VERIFIER'S VERDICT IS ITS OWN COUNT, JUDGED BY THE POLICY ────────────
//
// ✔MEASURED P68 (lane `ht`): `HirVerifier::verify` answered "clean" from the
// reporter's error delta, and a reporter drops a diagnostic identical to a recent
// one without storing it — so a module the verifier refused, read twice into ONE
// reporter, verified clean the second time. The verdict is now the verifier's own
// count of the findings the reporter's POLICY makes Errors
// (`DiagnosticReporter::effectiveSeverity`), and every Error code the verifier
// emits is unsuppressable. Each arm below reads into ONE reporter, twice.
namespace {

struct TwoReads {
    bool first  = true;
    bool second = true;
    std::size_t errorsAfterFirst  = 0;
    std::size_t errorsAfterSecond = 0;
};

[[nodiscard]] TwoReads readTwiceIntoOneReporter(std::string const& text,
                                                DiagnosticReporter& shared) {
    TwoReads t;
    t.first            = parseHir(text, CompilationUnitId{101}, shared)->ok;
    t.errorsAfterFirst = shared.errorCount();
    t.second            = parseHir(text, CompilationUnitId{102}, shared)->ok;
    t.errorsAfterSecond = shared.errorCount();
    return t;
}

[[nodiscard]] std::size_t countOf(DiagnosticReporter const& r, DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == c) ++n;
    return n;
}

// A `break` with no enclosing loop or switch: the READER takes it, the VERIFIER
// refuses it (`H_InvalidBreak`).
std::string strayBreakModule() {
    return moduleWithDecls(
        "  function %1 : fn() -> void {\n    block {\n      break\n    }\n  }\n");
}

// A statement after an unconditional terminator: ISO-valid dead code, which the
// verifier reports as a WARNING (`H_UnreachableCode`) — its one Warning finding.
std::string deadCodeModule() {
    return moduleWithDecls(
        "  function %1 : fn() -> void {\n    block {\n      return void\n"
        "      return void\n    }\n  }\n");
}

} // namespace

TEST(HirTextVerdict, AVerifierRefusalFailsEveryReadIntoOneReporter) {
    DiagnosticReporter shared;
    TwoReads const t = readTwiceIntoOneReporter(strayBreakModule(), shared);
    EXPECT_FALSE(t.first) << "the control: the verifier must refuse a stray break";
    EXPECT_FALSE(t.second)
        << "a module the VERIFIER refused read as accepted the second time";
}

TEST(HirTextVerdict, ACleanModuleReadTwiceIntoOneReporterPassesBothTimes) {
    std::string const clean = moduleWithDecls(
        "  function %1 : fn() -> void {\n    block {\n      return void\n    }\n  }\n");
    DiagnosticReporter shared;
    TwoReads const t = readTwiceIntoOneReporter(clean, shared);
    EXPECT_TRUE(t.first);
    EXPECT_TRUE(t.second) << "a clean module must stay clean on a second read";
    EXPECT_EQ(shared.errorCount(), 0u);
}

// ★ THE PIN THAT NEEDS THE OWN COUNT BY ITSELF. Under `--warnings-as-errors` the
// dead-code WARNING is promoted to an Error — and `H_UnreachableCode` is not
// unsuppressable, so the reporter still drops the second read's copy as a recent
// duplicate. Only a verdict that counts the POLICY's answer sees it.
TEST(HirTextVerdict, APromotedWarningFailsEveryReadIntoOneReporter) {
    DiagnosticReporter::Config cfg;
    cfg.policy.warningsAsErrors = true;
    DiagnosticReporter shared{cfg};
    TwoReads const t = readTwiceIntoOneReporter(deadCodeModule(), shared);
    EXPECT_FALSE(t.first) << "under --warnings-as-errors the dead-code warning is an error";
    // THE PREMISE, MEASURED: the reporter really stored nothing new the second time.
    EXPECT_EQ(t.errorsAfterSecond, t.errorsAfterFirst)
        << "premise gone: the promoted repeat was stored";
    EXPECT_FALSE(t.second)
        << "the promoted warning failed the first read and passed the second: the "
           "verdict read the reporter's delta, which the dedup window had emptied";
}

TEST(HirTextVerdict, ASuppressedWarningIsNotAnErrorEvenUnderWarningsAsErrors) {
    DiagnosticReporter::Config cfg;
    cfg.policy.warningsAsErrors = true;
    cfg.policy.suppress.insert(DiagnosticCode::H_UnreachableCode);
    DiagnosticReporter shared{cfg};
    TwoReads const t = readTwiceIntoOneReporter(deadCodeModule(), shared);
    EXPECT_TRUE(t.first) << "a warning the operator suppressed is not an error";
    EXPECT_TRUE(t.second);
    EXPECT_EQ(countOf(shared, DiagnosticCode::H_UnreachableCode), 0u)
        << "a suppressed warning must print nothing";
}

TEST(HirTextVerdict, WithoutWarningsAsErrorsDeadCodeReadsCleanWithItsWarningShown) {
    DiagnosticReporter r;
    auto const res = parseHir(deadCodeModule(), CompilationUnitId{103}, r);
    EXPECT_TRUE(res->ok) << "ISO-valid dead code must not fail the read";
    ASSERT_EQ(countOf(r, DiagnosticCode::H_UnreachableCode), 1u)
        << "the dead-code warning must still be shown";
    for (auto const& d : r.all()) {
        if (d.code == DiagnosticCode::H_UnreachableCode)
            EXPECT_EQ(d.severity, DiagnosticSeverity::Warning);
    }
}

// ★ A SUPPRESS LIST CANNOT SILENCE A STRUCTURAL REFUSAL — the three codes no C
// source reaches (✔MEASURED P68: the semantic tier refuses a stray `break` first,
// and only a `.dsshir` read or direct construction builds a shader call cycle or
// an unminted intrinsic), each pinned where it CAN be reached. The six a C source
// reaches are pinned end to end in `tests/program/test_emit_hir_mode.cpp`.
TEST(HirTextVerdict, ASuppressListCannotSilenceARefusalOnlyAReadOrABuilderReaches) {
    auto const suppressing = [](DiagnosticCode code) {
        DiagnosticReporter::Config cfg;
        cfg.policy.suppress.insert(code);
        return cfg;
    };
    // (1) H_InvalidBreak — through the reader.
    {
        DiagnosticReporter r{suppressing(DiagnosticCode::H_InvalidBreak)};
        EXPECT_FALSE(parseHir(strayBreakModule(), CompilationUnitId{104}, r)->ok)
            << "H_InvalidBreak: a suppress list let the refused module read clean";
        EXPECT_EQ(countOf(r, DiagnosticCode::H_InvalidBreak), 1u)
            << "H_InvalidBreak: the refusal was silenced by the suppress list";
    }
    // (2) H_ShaderViolation — through the reader: a shader function calling itself.
    {
        std::string const cycle = moduleWithDecls(
            "  function [shader] %1 : fn() -> void {\n    block {\n"
            "      expr call : void (ref %1 : fn() -> void)\n      return void\n"
            "    }\n  }\n");
        DiagnosticReporter r{suppressing(DiagnosticCode::H_ShaderViolation)};
        EXPECT_FALSE(parseHir(cycle, CompilationUnitId{105}, r)->ok)
            << "H_ShaderViolation: a suppress list let the refused module read clean";
        EXPECT_EQ(countOf(r, DiagnosticCode::H_ShaderViolation), 1u)
            << "H_ShaderViolation: the refusal was silenced by the suppress list\n"
            << cycle << (r.all().empty() ? std::string{} : r.all()[0].actual);
    }
    // (3) H_UnknownIntrinsic — no text can spell it (the reader refuses an
    // undeclared intrinsic NAME itself), so it is built directly.
    {
        TypeInterner in{CompilationUnitId{106}};
        TypeId const i32 = in.primitive(TypeKind::I32);
        HirBuilder b{"toy"};
        HirNodeId const call = b.makeIntrinsicCall(/*intrinsicId=*/7u,
                                                   std::array{b.makeLiteral(i32)}, i32);
        Hir const hir = std::move(b).finish(call);
        DiagnosticReporter r{suppressing(DiagnosticCode::H_UnknownIntrinsic)};
        EXPECT_FALSE(HirVerifier{hir}.verify(r))
            << "H_UnknownIntrinsic: a suppress list let the refused module verify clean";
        EXPECT_EQ(countOf(r, DiagnosticCode::H_UnknownIntrinsic), 1u)
            << "H_UnknownIntrinsic: the refusal was silenced by the suppress list";
    }
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
    // `std::abort()` here killed the whole binary, so one unreadable golden
    // cost every sibling test its verdict. THROW -- GoogleTest reports an
    // escaping exception as a failure of the ONE running test. The read itself
    // goes through the ONE checked read, so a golden that reads SHORT is named
    // as a torn read instead of surfacing as a golden mismatch.
    auto text = dss::core::readFileChecked(p);
    if (!text) {
        throw std::runtime_error("golden: " + std::move(text).error().message);
    }
    std::string s = *std::move(text);
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
        // v2: the producer and the buffer table are file CONTENT, so a re-emit
        // that did not thread them back would not be a round trip — it would be
        // a re-emit with two fields dropped.
        ctx.producer = res->producer; ctx.bufferNames = &res->bufferNames;
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
        ctx2.producer = res2->producer; ctx2.bufferNames = &res2->bufferNames;
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

// D-CSUBSET-PER-MEMBER-PACKED: the PER-FIELD packed marker round-trips. A struct with
// ONE packed member emits ` packed` after THAT member's type and re-parses with the
// flag on that member alone. ★ Without this the flag would cross the text boundary
// ABSENT, and the loss is invisible to every size check: MEASURED (gcc 13.3.0 + clang
// 18.1.3, x86_64 and aarch64), `{char a; int z <pk>; double d;}` and the undecorated
// struct are BOTH sizeof 16 / _Alignof 8 — only z's offset differs, 1 vs 4.
TEST(HirText, PerFieldPackedMarkerRoundTrips) {
    TypeInterner in{CompilationUnitId{1}};
    std::array<TypeId, 3> const fields{in.primitive(TypeKind::Char),
                                       in.primitive(TypeKind::I32),
                                       in.primitive(TypeKind::F64)};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 0> const noOffs{};
    std::array<std::uint32_t, 0> const noAligns{};
    std::array<std::uint8_t, 3>  const flags{0, 1, 0};
    TypeId const s = in.forwardComposite(TypeKind::Struct, "S", /*declSiteKey=*/77);
    in.completeComposite(s, fields, /*packed=*/false, noWidths, noOffs, noAligns,
                         /*explicitAlign=*/0, /*maxFieldAlign=*/0, flags);
    TypeId const ptrS   = in.pointer(s);
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
    // The marker sits on the MEMBER, not after the struct name — a whole-composite
    // packed would emit `struct "S" packed {`, which is a different layout.
    EXPECT_EQ(text.find("struct \"S\" packed"), std::string::npos) << text;
    EXPECT_NE(text.find("i32 packed"), std::string::npos) << text;
    expectRoundTrip(hir, ctx);   // emit -> parse -> emit byte-identical
}

// D-CSUBSET-PER-MEMBER-PACKED: the codec's PARSE direction, plus the identity fork.
// `struct "S" { i32 packed }` and `struct "S" packed { i32 }` are DIFFERENT layouts
// and must never collapse onto one TypeId (they use distinct forward key spaces).
TEST(ParseTypeFromText, PerFieldPackedParseAndForkIdentity) {
    TypeInterner interner{CompilationUnitId{15}};
    TypeRegistry reg;
    DiagnosticReporter rep;

    TypeId const perField = parseTypeFromText(
        "struct \"S\" { i8, i32 packed }", interner, reg, rep);
    ASSERT_TRUE(perField.valid());
    EXPECT_EQ(interner.kind(perField), TypeKind::Struct);
    EXPECT_FALSE(interner.isPacked(perField));
    EXPECT_TRUE(interner.hasFieldPacked(perField));
    EXPECT_FALSE(interner.isFieldPacked(perField, 0));
    EXPECT_TRUE(interner.isFieldPacked(perField, 1));

    // Canonical: the same text parses to the same TypeId.
    TypeId const again = parseTypeFromText(
        "struct \"S\" { i8, i32 packed }", interner, reg, rep);
    EXPECT_EQ(perField, again);

    // FORK: the whole-composite spelling of the same fields is a DISTINCT type.
    TypeId const whole = parseTypeFromText(
        "struct \"S\" packed { i8, i32 }", interner, reg, rep);
    ASSERT_TRUE(whole.valid());
    EXPECT_NE(perField, whole);
    EXPECT_TRUE(interner.isPacked(whole));
    EXPECT_FALSE(interner.hasFieldPacked(whole));

    // FORK: the undecorated spelling is a THIRD distinct type.
    TypeId const plain = parseTypeFromText(
        "struct \"S\" { i8, i32 }", interner, reg, rep);
    ASSERT_TRUE(plain.valid());
    EXPECT_NE(perField, plain);
    EXPECT_NE(whole, plain);
    EXPECT_FALSE(interner.hasFieldPacked(plain));

    // A per-field packed COMBINES with a member alignas marker, in that order.
    TypeId const both = parseTypeFromText(
        "struct \"P\" { i8 ~1, i32 ~2 packed }", interner, reg, rep);
    ASSERT_TRUE(both.valid());
    EXPECT_TRUE(interner.hasExplicitAligns(both));
    EXPECT_TRUE(interner.hasFieldPacked(both));
    EXPECT_EQ(interner.explicitFieldAlign(both, 1), 2u);
    EXPECT_TRUE(interner.isFieldPacked(both, 1));

    // A UNION member carries it too, and forks from the whole-composite spelling.
    TypeId const uPerField = parseTypeFromText(
        "union \"U\" { i8, i32 packed }", interner, reg, rep);
    ASSERT_TRUE(uPerField.valid());
    EXPECT_EQ(interner.kind(uPerField), TypeKind::Union);
    EXPECT_FALSE(interner.isPacked(uPerField));
    EXPECT_TRUE(interner.isFieldPacked(uPerField, 1));
    TypeId const uWhole = parseTypeFromText(
        "union \"U\" packed { i8, i32 }", interner, reg, rep);
    EXPECT_NE(uPerField, uWhole);

    // FAIL LOUD: a packed field cannot also carry an explicit offset — the interner
    // ABORTS on that pair, so the text layer must refuse it with a diagnostic first.
    DiagnosticReporter repBad;
    TypeId const bad = parseTypeFromText(
        "struct \"B\" { i8 @0, i32 @4 packed }", interner, reg, repBad);
    EXPECT_FALSE(bad.valid());
    EXPECT_GE(repBad.errorCount(), 1u);
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
        "dsshir 5\nproducer \"\"\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
        "  function %1 : fn() -> void {\n    block {\n"
        "      inline_asm \"nop %0\" { extended outputs 0 operands ( \"m\" "
        "operand_kind not_a_form -> lit int 0 : i32 ) }\n"
        "      return void\n    }\n  }\n}\n";
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
        "dsshir 5\nproducer \"\"\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
        "  function %1 : fn() -> void {\n    block {\n"
        "      inline_asm \"nop %0\" { extended outputs 0 operands ( \"i\" "
        "operand_kind imm32 -> lit int 7 : i32 ) }\n"
        "      return void\n    }\n  }\n}\n";
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
        "dsshir 5\nproducer \"\"\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
        "  function %1 : fn() -> void {\n    block {\n"
        "      inline_asm \"nop %0\" { extended outputs 0 operands ( \"r\" "
        "class 200 -> lit int 0 : i32 ) }\n"
        "      return void\n    }\n  }\n}\n";
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
        "dsshir 5\nproducer \"\"\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
        "  function %1 : fn() -> void {\n    block {\n"
        "      inline_asm \"nop %0\" { extended outputs 0 operands ( \"r\" "
        "class 1 -> lit int 0 : i32 ) }\n"
        "      return void\n    }\n  }\n}\n";
    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{34}, r);
    ASSERT_TRUE(res->ok);
    ASSERT_EQ(res->inlineAsmPool.size(), 1u);
    ASSERT_EQ(res->inlineAsmPool.at(1).operands.size(), 1u);
    EXPECT_TRUE(res->inlineAsmPool.at(1).operands[0].regClassResolved);
    EXPECT_EQ(res->inlineAsmPool.at(1).operands[0].regClass,
              static_cast<std::uint8_t>(TargetRegClass::GPR));
}

// ── COMPOSITES: DEFINED ONCE, REFERENCED EVERYWHERE (format v5) ──────────────
//
// ★★★ v5 moved every struct and union into the artifact's `types` section. Until
// v4 a composite was spelled STRUCTURALLY AT EVERY USE — its whole field list
// inline, through every pointer — so an artifact's type text was (type mentions)
// × (the reachable type graph). ✔MEASURED (P68 round 7, lane `cr`, WSL Release):
// the sqlite amalgamation's first 1.18 MB took 275.9 G instructions under
// `--emit-hir` and died `std::bad_alloc`, against 3.23 G for the whole compile of
// the same file. Every reference IR measured (clang's named `%struct.X = type
// {…}`, gcc's `@N` tree-dump handles, MSVC's CodeView type indices) defines a
// composite once and names it by handle; this format now does the same.
//
// The arms below keep every property the v3 cycle arms pinned — a self-reference,
// a mutually recursive pair, a recursive union beside an opaque one, the layout
// channels on a recursive composite, a cycle that closes through a qualifier skin
// — and each now asserts the v5 spelling: ONE definition per composite and
// `type <H>` at every mention, the mention that closes a cycle included. A cycle
// is no longer a special case, so no arm here needs a cycle guard to survive.

namespace {

// Build a two-function module over `in` whose parameter types are `p0`/`p1`.
// Shared by the arms below so each one is about the TYPE it builds and not about
// module scaffolding.
[[nodiscard]] Hir twoFnModule(TypeInterner& in, TypeId p0, TypeId p1) {
    HirBuilder b{"toy"};
    std::array<TypeId, 1> const a0{p0};
    std::array<TypeId, 1> const a1{p1};
    TypeId const v   = in.primitive(TypeKind::Void);
    HirNodeId const f = b.makeFunction(in.fnSig(a0, v, CallConv::CcSysV), 1, {},
                                       b.makeBlock(std::vector<HirNodeId>{}));
    HirNodeId const g = b.makeFunction(in.fnSig(a1, v, CallConv::CcSysV), 2, {},
                                       b.makeBlock(std::vector<HirNodeId>{}));
    return std::move(b).finish(b.makeModule(std::vector<HirNodeId>{f, g}));
}

// The `types { … }` section of an artifact, verbatim — empty when there is none.
[[nodiscard]] std::string typesSectionOf(std::string const& text) {
    std::size_t const at = text.find("\ntypes {\n");
    if (at == std::string::npos) return {};
    std::size_t const end = text.find("\n}\n", at + 1);
    return end == std::string::npos ? std::string{} : text.substr(at + 1, end + 3 - (at + 1));
}

// How many times `needle` occurs in `text`.
[[nodiscard]] std::size_t occurrences(std::string const& text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) ++count;
    return count;
}

// The pointee of function `fnIndex`'s first parameter, in a PARSED module.
[[nodiscard]] TypeId firstParamPointee(HirParseResult const& res, std::size_t fnIndex) {
    auto const decls = res.hir.moduleDecls(res.hir.root());
    TypeId const param = res.interner.fnParams(res.hir.functionSignature(decls[fnIndex]))[0];
    return res.interner.operands(param)[0];
}

}  // namespace

// The commonest shape in C, `struct S { int v; struct S *next; }`: its type graph
// is CYCLIC, which v2 refused, v3 spelled with a `rec <H>` back-reference, and v5
// makes ordinary — `ptr<type 1>` inside `type 1`'s own definition is a reference
// like any other, so nothing expands and nothing can expand forever.
TEST(HirText, ASelfReferentialStructIsOneTableEntryThatNamesItself) {
    TypeInterner in{CompilationUnitId{77}};
    TypeId const s = in.forwardComposite(TypeKind::Struct, "S", /*declSiteKey=*/7);
    std::array<TypeId, 2> const fields{in.primitive(TypeKind::I32), in.pointer(s)};
    in.completeComposite(s, fields, /*packed=*/false);

    HirBuilder b{"toy"};
    HirNodeId const body = b.makeBlock(std::vector<HirNodeId>{});
    std::array<TypeId, 1> const params{in.pointer(s)};
    TypeId const sig = in.fnSig(params, in.primitive(TypeKind::Void), CallConv::CcSysV);
    HirNodeId const fn   = b.makeFunction(sig, /*symbol=*/1, {}, body);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "f"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    // ⚠ FATAL: under a writer mutant the helper below would re-emit from a refused
    // parse, and a dead process takes every later arm's verdict with it.
    ASSERT_FALSE(r.hasErrors()) << text;
    // THE WHOLE ARTIFACT, byte for byte: one definition, the self-reference a
    // plain `type 1`, and the signature naming the same handle.
    EXPECT_EQ(text,
              "dsshir 5\n"
              "producer \"\"\n"
              "types {\n"
              "  type 1 = struct \"S\" {i32, ptr<type 1>}\n"
              "}\n"
              "symbols {\n"
              "  %1 \"f\"\n"
              "}\n"
              "module \"toy\" {\n"
              "  function %1 : fn(ptr<type 1>) -> void {\n"
              "    block {\n"
              "    }\n"
              "  }\n"
              "}\n");
    EXPECT_EQ(expectRoundTrip(hir, ctx), text);

    // ★ AND THE TYPE'S IDENTITY SURVIVED AS ONE TYPE: the parameter's pointee and
    // the pointee of its own `next` field are ONE rebuilt TypeId, completed.
    DiagnosticReporter pr;
    auto res = parseHir(text, CompilationUnitId{78}, pr);
    ASSERT_TRUE(res->ok);
    TypeId const rebuilt = firstParamPointee(*res, 0);
    ASSERT_EQ(res->interner.kind(rebuilt), TypeKind::Struct);
    EXPECT_FALSE(res->interner.isIncompleteComposite(rebuilt))
        << "the forward mint must have been COMPLETED, not left opaque";
    TypeId const selfField = res->interner.operands(res->interner.operands(rebuilt)[1])[0];
    EXPECT_EQ(selfField.v, rebuilt.v)
        << "the self-reference must resolve to the SAME TypeId, not a second copy";
}

// Sharing is not a cycle, and it no longer costs a second spelling either: a
// composite mentioned twice as a field, and again through a pointer, is DEFINED
// once and REFERENCED three times.
TEST(HirText, ACompositeMentionedManyTimesIsDefinedExactlyOnce) {
    TypeInterner in{CompilationUnitId{78}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const inner = in.forwardComposite(TypeKind::Struct, "Inner", 8);
    std::array<TypeId, 1> const innerFields{i32};
    in.completeComposite(inner, innerFields, false);
    TypeId const outer = in.forwardComposite(TypeKind::Struct, "Outer", 9);
    std::array<TypeId, 2> const outerFields{inner, inner};   // the SAME type twice
    in.completeComposite(outer, outerFields, false);

    Hir hir = twoFnModule(in, in.pointer(outer), in.pointer(inner));
    std::vector<std::string> names{"", "f", "g"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    ASSERT_FALSE(r.hasErrors()) << text;
    EXPECT_EQ(typesSectionOf(text),
              "types {\n"
              "  type 1 = struct \"Outer\" {type 2, type 2}\n"
              "  type 2 = struct \"Inner\" {i32}\n"
              "}\n") << text;
    EXPECT_EQ(occurrences(text, "struct \"Inner\""), 1u) << text;
    EXPECT_NE(text.find("function %1 : fn(ptr<type 1>) -> void {"), std::string::npos) << text;
    EXPECT_NE(text.find("function %2 : fn(ptr<type 2>) -> void {"), std::string::npos) << text;
    EXPECT_EQ(expectRoundTrip(hir, ctx), text);
}

// ★★ MUTUAL RECURSION DECIDED THE v3 DESIGN, AND IN v5 IT IS JUST TWO ENTRIES.
// v3 spelled `B` two ways in one artifact (standing inside `A`, and standing alone),
// which is why its handle had to be artifact-global. v5 spells every composite
// exactly ONCE, so the two-spellings problem cannot arise; what is still asserted
// is the identity the handle buys — one TypeId per composite after the read.
TEST(HirText, MutuallyRecursiveCompositesAreTwoEntriesThatNameEachOther) {
    TypeInterner in{CompilationUnitId{81}};
    TypeId const a = in.forwardComposite(TypeKind::Struct, "A", /*declSiteKey=*/11);
    TypeId const b = in.forwardComposite(TypeKind::Struct, "B", /*declSiteKey=*/12);
    std::array<TypeId, 1> const aFields{in.pointer(b)};
    std::array<TypeId, 1> const bFields{in.pointer(a)};
    in.completeComposite(a, aFields, /*packed=*/false);
    in.completeComposite(b, bFields, /*packed=*/false);

    Hir hir = twoFnModule(in, in.pointer(a), in.pointer(b));
    std::vector<std::string> names{"", "f", "g"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    ASSERT_FALSE(r.hasErrors()) << text;
    EXPECT_EQ(typesSectionOf(text),
              "types {\n"
              "  type 1 = struct \"A\" {ptr<type 2>}\n"
              "  type 2 = struct \"B\" {ptr<type 1>}\n"
              "}\n") << text;
    EXPECT_EQ(expectRoundTrip(hir, ctx), text);

    DiagnosticReporter pr;
    auto res = parseHir(text, CompilationUnitId{82}, pr);
    ASSERT_TRUE(res->ok) << text;
    TypeId const aRebuilt = firstParamPointee(*res, 0);
    TypeId const bRebuilt = firstParamPointee(*res, 1);
    EXPECT_EQ(res->interner.name(aRebuilt), "A");
    EXPECT_EQ(res->interner.name(bRebuilt), "B");
    TypeId const bViaA = res->interner.operands(res->interner.operands(aRebuilt)[0])[0];
    EXPECT_EQ(bViaA.v, bRebuilt.v) << "`B` was rebuilt twice — the handle is not doing its job";
    TypeId const aViaB = res->interner.operands(res->interner.operands(bRebuilt)[0])[0];
    EXPECT_EQ(aViaB.v, aRebuilt.v);
}

// ★★ THE HANDLE, NOT THE CONTENT, IS A COMPOSITE'S IDENTITY IN THE READER. Two
// scopes may each define a `struct S { int v; }`: two types that share a name and
// a body. The writer gives them two handles; a reader that interned by CONTENT
// would fold them into ONE TypeId, and the re-emission — which numbers distinct
// TypeIds — would then write one entry where the artifact had two.
TEST(HirText, TwoCompositesSharingANameAndABodyStayTwoTypes) {
    TypeInterner in{CompilationUnitId{83}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    std::array<TypeId, 1> const body{i32};
    TypeId const s1 = in.forwardComposite(TypeKind::Struct, "S", /*declSiteKey=*/51);
    TypeId const s2 = in.forwardComposite(TypeKind::Struct, "S", /*declSiteKey=*/52);
    in.completeComposite(s1, body, false);
    in.completeComposite(s2, body, false);
    ASSERT_NE(s1.v, s2.v) << "the premise: two decl sites are two types";

    Hir hir = twoFnModule(in, in.pointer(s1), in.pointer(s2));
    std::vector<std::string> names{"", "f", "g"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    ASSERT_FALSE(r.hasErrors()) << text;
    EXPECT_EQ(typesSectionOf(text),
              "types {\n"
              "  type 1 = struct \"S\" {i32}\n"
              "  type 2 = struct \"S\" {i32}\n"
              "}\n") << text;
    EXPECT_EQ(expectRoundTrip(hir, ctx), text);

    DiagnosticReporter pr;
    auto res = parseHir(text, CompilationUnitId{84}, pr);
    ASSERT_TRUE(res->ok) << text;
    EXPECT_NE(firstParamPointee(*res, 0).v, firstParamPointee(*res, 1).v)
        << "two handles came back as ONE type — the reader keyed on content";
}

// A RECURSIVE union beside an INCOMPLETE one. `opaque` is an ordinary table entry
// now: an incomplete `union Tag;` must not come back as `{}`, which is a LEGAL
// COMPLETE zero-member union — a silent size change.
TEST(HirText, RecursiveAndOpaqueUnionsAreBothTableEntries) {
    TypeInterner in{CompilationUnitId{85}};
    TypeId const u = in.forwardComposite(TypeKind::Union, "U", /*declSiteKey=*/21);
    std::array<TypeId, 2> const uFields{in.primitive(TypeKind::I32), in.pointer(u)};
    in.completeComposite(u, uFields, /*packed=*/false);
    TypeId const opaque = in.forwardComposite(TypeKind::Union, "Tag", /*declSiteKey=*/22);
    ASSERT_TRUE(in.isIncompleteComposite(opaque));

    Hir hir = twoFnModule(in, in.pointer(u), in.pointer(opaque));
    std::vector<std::string> names{"", "f", "g"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    ASSERT_FALSE(r.hasErrors()) << text;
    EXPECT_EQ(typesSectionOf(text),
              "types {\n"
              "  type 1 = union \"U\" {i32, ptr<type 1>}\n"
              "  type 2 = union \"Tag\" opaque\n"
              "}\n") << text;
    EXPECT_EQ(expectRoundTrip(hir, ctx), text);

    DiagnosticReporter pr;
    auto res = parseHir(text, CompilationUnitId{86}, pr);
    ASSERT_TRUE(res->ok);
    EXPECT_TRUE(res->interner.isIncompleteComposite(firstParamPointee(*res, 1)))
        << "the opaque tag came back COMPLETE — the ABI drop this arm exists for";
    EXPECT_FALSE(res->interner.isIncompleteComposite(firstParamPointee(*res, 0)));
}

// A recursive composite carrying layout channels — the definition, not a use site,
// is where they are spelled, and a cycle does not route them anywhere else.
TEST(HirText, ARecursiveCompositeCarriesPackedAndMemberAlignsInItsDefinition) {
    TypeInterner in{CompilationUnitId{87}};
    TypeId const s = in.forwardComposite(TypeKind::Struct, "P", /*declSiteKey=*/31);
    std::array<TypeId, 2> const fields{in.primitive(TypeKind::Char), in.pointer(s)};
    in.completeComposite(s, fields, /*packed=*/true);

    TypeInterner in2{CompilationUnitId{88}};
    TypeId const t = in2.forwardComposite(TypeKind::Struct, "A", /*declSiteKey=*/32);
    std::array<TypeId, 2> const tFields{in2.primitive(TypeKind::Char), in2.pointer(t)};
    std::array<std::uint32_t, 2> const aligns{0u, 8u};
    std::span<std::int64_t const>  const noW{};
    std::span<std::uint64_t const> const noO{};
    in2.completeComposite(t, tFields, /*packed=*/false, noW, noO, aligns);

    std::vector<std::string> names{"", "f", "g"};
    for (auto const& [interner, want] :
         std::vector<std::pair<TypeInterner*, std::string>>{
             {&in,  "types {\n  type 1 = struct \"P\" packed {char, ptr<type 1>}\n}\n"},
             {&in2, "types {\n  type 1 = struct \"A\" {char ~0, ptr<type 1> ~8}\n}\n"}}) {
        TypeId const root = (interner == &in) ? s : t;
        Hir hir = twoFnModule(*interner, interner->pointer(root), interner->pointer(root));
        HirTextContext ctx; ctx.interner = interner; ctx.symbolNames = &names;
        DiagnosticReporter r;
        std::string const text = emitHir(hir, ctx, r);
        ASSERT_FALSE(r.hasErrors()) << text;
        EXPECT_EQ(typesSectionOf(text), want) << text;
        EXPECT_EQ(expectRoundTrip(hir, ctx), text);
    }
}

// ⚠ A CYCLE THAT CLOSES THROUGH A QUALIFIER SKIN — `struct S { volatile struct S
// *next; }`, an intrusive list touched from a signal handler. The skin stays at
// the USE (`volatile<type 1>`); the table holds the material composite once.
TEST(HirText, ACycleThroughAVolatileSkinKeepsTheSkinAtTheUse) {
    TypeInterner in{CompilationUnitId{89}};
    TypeId const s = in.forwardComposite(TypeKind::Struct, "S", /*declSiteKey=*/41);
    std::array<TypeId, 2> const fields{in.primitive(TypeKind::I32),
                                       in.pointer(in.volatileQualified(s))};
    in.completeComposite(s, fields, /*packed=*/false);

    Hir hir = twoFnModule(in, in.pointer(s), in.pointer(in.volatileQualified(s)));
    std::vector<std::string> names{"", "f", "g"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    ASSERT_FALSE(r.hasErrors()) << text;
    EXPECT_EQ(typesSectionOf(text),
              "types {\n"
              "  type 1 = struct \"S\" {i32, ptr<volatile<type 1>>}\n"
              "}\n") << text;
    EXPECT_NE(text.find("function %2 : fn(ptr<volatile<type 1>>) -> void {"),
              std::string::npos) << "the skin must stay at the use\n" << text;
    EXPECT_EQ(expectRoundTrip(hir, ctx), text);
}

// ★★★ EVERY LAYOUT CHANNEL A COMPOSITE CARRIES TRAVELS — FOUR OF THEM DID NOT IN v4.
//
// A channel the text drops is a layout the reader rebuilds DIFFERENTLY while the
// round trip stays byte-identical (the re-emission spells the stripped type exactly
// as the first emission did), so only a comparison of the rebuilt CHANNELS can see
// it — which is what this arm does, channel by channel. v4 dropped bit-field
// widths, a whole-composite `aligned(N)` and a `#pragma pack(N)` cap (all three
// listed in the doc's §5.2 as known losses), and — listed nowhere — a per-member
// alignment on a UNION member, which the semantic analyzer does complete.
TEST(HirText, EveryLayoutChannelOfACompositeTravelsThroughItsDefinition) {
    TypeInterner in{CompilationUnitId{61}};
    TypeId const u32 = in.primitive(TypeKind::U32);
    TypeId const u8  = in.primitive(TypeKind::U8);
    TypeId const chr = in.primitive(TypeKind::Char);
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const i64 = in.primitive(TypeKind::I64);
    std::span<std::int64_t const>  const noW{};
    std::span<std::uint64_t const> const noO{};
    std::span<std::uint32_t const> const noA{};

    // (1) bit-field widths, the zero-width packing break included.
    TypeId const bits = in.forwardComposite(TypeKind::Struct, "Bits", 1);
    std::array<TypeId, 4> const bitsF{u32, u32, u32, u32};
    std::array<std::int64_t, 4> const bitsW{3, 0, 5, kNotBitfield};
    in.completeComposite(bits, bitsF, false, bitsW);
    // (2) a whole-composite `aligned(16)` on a packed struct (`packed, aligned(16)`).
    TypeId const al = in.forwardComposite(TypeKind::Struct, "Al", 2);
    std::array<TypeId, 2> const alF{chr, i32};
    in.completeComposite(al, alF, /*packed=*/true, noW, noO, noA, /*explicitAlign=*/16);
    // (3) a `#pragma pack(2)` cap.
    TypeId const pk = in.forwardComposite(TypeKind::Struct, "Pk", 3);
    std::array<TypeId, 2> const pkF{chr, i64};
    in.completeComposite(pk, pkF, false, noW, noO, noA, 0, /*maxFieldAlign=*/2);
    // (4) a member alignment on a UNION member.
    TypeId const ua = in.forwardComposite(TypeKind::Union, "UA", 4);
    std::array<TypeId, 2> const uaF{chr, i32};
    std::array<std::uint32_t, 2> const uaA{0u, 8u};
    in.completeComposite(ua, uaF, false, noW, noO, uaA);
    // (5) a union carrying bit-fields, `aligned(8)` and a `pack(4)` cap at once.
    TypeId const ub = in.forwardComposite(TypeKind::Union, "UB", 5);
    std::array<TypeId, 2> const ubF{u32, u8};
    std::array<std::int64_t, 2> const ubW{7, kNotBitfield};
    in.completeComposite(ub, ubF, false, ubW, noO, noA, 8, 4);

    HirBuilder b{"toy"};
    std::array<TypeId, 5> const params{in.pointer(bits), in.pointer(al), in.pointer(pk),
                                       in.pointer(ua), in.pointer(ub)};
    TypeId const sig = in.fnSig(params, in.primitive(TypeKind::Void), CallConv::CcSysV);
    HirNodeId const fn = b.makeFunction(sig, 1, {}, b.makeBlock(std::vector<HirNodeId>{}));
    Hir hir = std::move(b).finish(b.makeModule(std::vector<HirNodeId>{fn}));

    std::vector<std::string> names{"", "f"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    ASSERT_FALSE(r.hasErrors()) << text;
    EXPECT_EQ(typesSectionOf(text),
              "types {\n"
              "  type 1 = struct \"Bits\" {u32 bits 3, u32 bits 0, u32 bits 5, u32}\n"
              "  type 2 = struct \"Al\" packed aligned 16 {char, i32}\n"
              "  type 3 = struct \"Pk\" pack 2 {char, i64}\n"
              "  type 4 = union \"UA\" {char ~0, i32 ~8}\n"
              "  type 5 = union \"UB\" aligned 8 pack 4 {u32 bits 7, u8}\n"
              "}\n") << text;
    EXPECT_EQ(expectRoundTrip(hir, ctx), text);

    // ★ THE HALF THE BYTES CANNOT SHOW: every channel, read back off the rebuilt
    // composites, equals the original's.
    DiagnosticReporter pr;
    auto res = parseHir(text, CompilationUnitId{62}, pr);
    ASSERT_TRUE(res->ok) << text;
    TypeInterner const& back = res->interner;
    auto const paramsBack = back.fnParams(res->hir.functionSignature(
        res->hir.moduleDecls(res->hir.root())[0]));
    ASSERT_EQ(paramsBack.size(), 5u);
    std::array<TypeId, 5> const original{bits, al, pk, ua, ub};
    for (std::size_t k = 0; k < 5; ++k) {
        SCOPED_TRACE(std::string{in.name(original[k])});
        TypeId const o = original[k];
        TypeId const n = back.operands(paramsBack[k])[0];
        EXPECT_EQ(back.kind(n), in.kind(o));
        EXPECT_EQ(back.isPacked(n), in.isPacked(o));
        EXPECT_EQ(back.explicitCompositeAlign(n), in.explicitCompositeAlign(o));
        EXPECT_EQ(back.maxFieldAlign(n), in.maxFieldAlign(o));
        EXPECT_EQ(back.hasExplicitAligns(n), in.hasExplicitAligns(o));
        auto const fo = in.operands(o);
        ASSERT_EQ(back.operands(n).size(), fo.size());
        for (std::size_t i = 0; i < fo.size(); ++i) {
            EXPECT_EQ(back.fieldBitWidth(n, i), in.fieldBitWidth(o, i)) << "field " << i;
            EXPECT_EQ(back.explicitFieldAlign(n, i), in.explicitFieldAlign(o, i)) << "field " << i;
        }
    }
}

// ── the refusals: a `types` entry or reference the writer cannot produce ─────
//
// The table bought a representation, not a licence to accept anything. Each arm is
// text the v5 writer cannot produce, so accepting it would mean rebuilding a module
// the writer would re-spell differently — or, for the layout channels, handing
// `completeComposite` a combination it ABORTS on, which a reader of text it did
// not write must never do.
TEST(HirText, ATypesEntryOrReferenceTheWriterCannotProduceIsRefusedByName) {
    struct Arm { char const* what; char const* types; char const* type; char const* says; };
    std::array<Arm, 14> const arms{{
        {"a reference to no entry", "  type 1 = struct \"S\" {i32}\n", "type 2",
         "names no entry"},
        {"a reference with no table at all", "", "ptr<type 1>", "names no entry"},
        {"the 0 handle", "  type 0 = struct \"S\" {i32}\n", "i32", "1-based"},
        {"a handle past 32 bits", "  type 4294967296 = struct \"S\" {i32}\n", "i32",
         "1-based"},
        {"one handle, two bodies",
         "  type 1 = struct \"S\" {i32}\n  type 1 = struct \"S\" {i64}\n", "type 1",
         "defined twice"},
        {"an entry that is not a composite", "  type 1 = enum \"E\"\n", "i32",
         "a `struct` or a `union`"},
        {"an unrepresentable composite alignment",
         "  type 1 = struct \"S\" aligned 3 {i32}\n", "type 1", "not a composite alignment"},
        {"a zero pack cap", "  type 1 = struct \"S\" pack 0 {i32}\n", "type 1",
         "not a composite alignment"},
        {"offsets mixed with member aligns",
         "  type 1 = struct \"S\" {i32 @0, i32 ~4}\n", "type 1", "cannot mix"},
        {"a packed composite with explicit offsets",
         "  type 1 = struct \"S\" packed {i32 @0}\n", "type 1", "cannot also be packed"},
        {"explicit offsets with bit-field widths",
         "  type 1 = struct \"S\" {u32 @0 bits 3, u32 @0}\n", "type 1",
         "cannot also carry bit-field widths"},
        {"an INLINE struct in a module", "", "struct \"S\" {i32}", "DEFINED ONCE"},
        {"an INLINE opaque union in a module", "", "union \"U\" opaque", "DEFINED ONCE"},
        {"a v3 back-reference in a module",
         "  type 1 = struct \"S\" {i32, ptr<rec 1>}\n", "type 1", "DEFINED ONCE"},
    }};
    auto const wrap = [](char const* types, char const* ty) {
        std::string s{"dsshir 5\nproducer \"\"\n"};
        if (*types != '\0') s += std::string{"types {\n"} + types + "}\n";
        s += "symbols {\n  %1 \"S\"\n}\nmodule \"toy\" {\n  type_decl %1 : ";
        return s + ty + "\n}\n";
    };
    for (Arm const& arm : arms) {
        std::string const text = wrap(arm.types, arm.type);
        DiagnosticReporter r;
        auto res = parseHir(text, CompilationUnitId{87}, r);
        EXPECT_FALSE(res->ok) << arm.what << " was ACCEPTED:\n" << text;
        bool named = false;
        for (auto const& d : r.all()) {
            if (d.actual.find(arm.says) != std::string::npos) named = true;
        }
        EXPECT_TRUE(named) << arm.what << ": the refusal did not name its cause\n" << text;
    }
    // CONTROLS: the same shapes, well-formed, are accepted — so "everything is
    // refused" is ruled out and each arm above is attributable to its one defect.
    // The second control references an entry defined AFTER the one that names it,
    // which is the ordinary case the head pre-scan exists for.
    for (auto const& [types, ty] : std::array<std::pair<char const*, char const*>, 2>{{
             {"  type 1 = struct \"S\" {i32, ptr<type 1>}\n", "type 1"},
             {"  type 1 = struct \"A\" {ptr<type 2>}\n  type 2 = union \"B\" {i32}\n",
              "type 1"}}}) {
        DiagnosticReporter cr;
        auto ok = parseHir(wrap(types, ty), CompilationUnitId{88}, cr);
        EXPECT_TRUE(ok->ok) << "the well-formed control was refused:\n" << wrap(types, ty)
                            << (cr.all().empty() ? std::string{} : cr.all()[0].actual);
    }
}

// ★★ THE STANDALONE TYPE TEXT IS UNCHANGED, AND IT REFUSES A MODULE HANDLE BY NAME.
// `parseTypeFromText` decodes every shipped FFI descriptor signature, and a
// descriptor has no `types` table — so `type <H>` there is the wrong GRAMMAR, not a
// misspelling, and the refusal must say so ("module-only") rather than report an
// unknown identifier. The inline forms it has always taken are its controls.
TEST(ParseTypeFromText, AModuleTypeHandleIsRefusedByNameAndTheInlineFormsStillParse) {
    TypeInterner in{CompilationUnitId{90}};
    TypeRegistry reg;
    for (std::string_view const text : {std::string_view{"type 3"},
                                        std::string_view{"ptr<type 1>"},
                                        std::string_view{"fn(type 2) -> void"}}) {
        SCOPED_TRACE(std::string{text});
        DiagnosticReporter r;
        TypeId const t = parseTypeFromText(text, in, reg, r);
        EXPECT_FALSE(t.valid()) << "a module handle decoded in a standalone text";
        bool named = false;
        for (auto const& d : r.all()) {
            if (d.actual.find("module-only") != std::string::npos
                && d.actual.find("types") != std::string::npos) named = true;
        }
        EXPECT_TRUE(named) << "the refusal did not say the handle is module-only; first: "
                           << (r.all().empty() ? std::string{} : r.all()[0].actual);
    }
    for (std::string_view const text :
         {std::string_view{"struct \"S\" {i32, i32}"},
          std::string_view{"struct \"FILE\" opaque"},
          std::string_view{"struct \"N\" rec 1 {i32, ptr<rec 1>}"},
          std::string_view{"union \"U\" packed {i8 packed, i32}"},
          std::string_view{"ptr<struct \"T\" {i32 @0, i32 @0}>"}}) {
        SCOPED_TRACE(std::string{text});
        DiagnosticReporter r;
        TypeId const t = parseTypeFromText(text, in, reg, r);
        EXPECT_TRUE(t.valid()) << "an inline standalone form stopped parsing";
        EXPECT_EQ(r.errorCount(), 0u);
    }
}

// The v3 back-reference lives on in the standalone text, with every refusal it
// always had. (These arms were the module refusals in v3; a module refuses `rec`
// wholesale now, and the arm above pins that.)
TEST(ParseTypeFromText, TheRecBackReferenceStillRefusesWhatItAlwaysRefused) {
    struct Arm { char const* what; char const* type; char const* says; };
    std::array<Arm, 4> const arms{{
        {"a back-reference to no open composite", "ptr<rec 3>", "no ENCLOSING"},
        {"a back-reference to a CLOSED composite",
         "tuple<struct \"S\" rec 1 {ptr<rec 1>}, rec 1>", "no ENCLOSING"},
        {"the 0 handle", "struct \"S\" rec 0 {i32}", "1-based"},
        {"one handle, two different bodies",
         "tuple<struct \"S\" rec 1 {ptr<rec 1>}, struct \"S\" rec 1 {i32, ptr<rec 1>}>",
         "DIFFERENT content"},
    }};
    for (Arm const& arm : arms) {
        TypeInterner in{CompilationUnitId{91}};
        TypeRegistry reg;
        DiagnosticReporter r;
        TypeId const t = parseTypeFromText(arm.type, in, reg, r);
        EXPECT_FALSE(t.valid()) << arm.what << " was ACCEPTED";
        bool named = false;
        for (auto const& d : r.all()) {
            if (d.actual.find(arm.says) != std::string::npos) named = true;
        }
        EXPECT_TRUE(named) << arm.what << ": the refusal did not name its cause";
    }
    TypeInterner in{CompilationUnitId{92}};
    TypeRegistry reg;
    DiagnosticReporter cr;
    TypeId const ok = parseTypeFromText("struct \"S\" rec 1 {i32, ptr<rec 1>}", in, reg, cr);
    ASSERT_TRUE(ok.valid()) << (cr.all().empty() ? std::string{} : cr.all()[0].actual);
    EXPECT_EQ(in.operands(in.operands(ok)[1])[0].v, ok.v)
        << "the standalone back-reference no longer closes the cycle on itself";
}

// ── THE COUNT: type-spelling work is O(mentions + graph), never their product ─
//
// ★★★ PINNED BY A COUNT, NEVER A STOPWATCH. `hirTextTypeNodesSpelledTake` counts
// the type NODES the writer renders. The module below mentions a composite `N`
// times, and that composite reaches a chain of `G` composites. Under v5 every
// mention is two nodes (`ptr<`, `type 1`) and every composite's definition is its
// own field list, so the count is EXACTLY 2 + 2N + (3G − 2): the chain's G − 1
// links are `{i32, ptr<type k+1>}` (3 nodes), its tail `{i32}` (1 node), plus the
// function signature's `fn` and `void`. v4 re-expanded the whole chain at every
// mention — 2N · (3G − 1)-ish — so doubling N doubled the G term; here it must
// add 2N and nothing else.
namespace {

struct ChainModule {
    std::unique_ptr<TypeInterner> interner =
        std::make_unique<TypeInterner>(CompilationUnitId{95});
    std::vector<std::string> names{"", "f"};
    std::optional<Hir>       hir;
};

// `f(struct S0 *p1, …, struct S0 *pN)`, where S0 → S1 → … → S(G−1) by pointer.
[[nodiscard]] std::unique_ptr<ChainModule> chainModule(std::size_t mentions,
                                                       std::size_t graph) {
    auto m = std::make_unique<ChainModule>();
    TypeInterner& in = *m->interner;
    TypeId const i32 = in.primitive(TypeKind::I32);
    std::vector<TypeId> chain;
    for (std::size_t k = 0; k < graph; ++k) {
        chain.push_back(in.forwardComposite(TypeKind::Struct, "S" + std::to_string(k),
                                            1000 + k));
    }
    for (std::size_t k = 0; k < graph; ++k) {
        if (k + 1 < graph) {
            std::array<TypeId, 2> const f{i32, in.pointer(chain[k + 1])};
            in.completeComposite(chain[k], f, false);
        } else {
            std::array<TypeId, 1> const f{i32};
            in.completeComposite(chain[k], f, false);
        }
    }
    std::vector<TypeId> const params(mentions, in.pointer(chain[0]));
    TypeId const sig = in.fnSig(params, in.primitive(TypeKind::Void), CallConv::CcSysV);
    HirBuilder b{"toy"};
    HirNodeId const fn = b.makeFunction(sig, 1, {}, b.makeBlock(std::vector<HirNodeId>{}));
    m->hir.emplace(std::move(b).finish(b.makeModule(std::vector<HirNodeId>{fn})));
    return m;
}

// The writer's type-node count for one emission of a chain module.
[[nodiscard]] std::uint64_t spelledNodes(std::size_t mentions, std::size_t graph) {
    auto const m = chainModule(mentions, graph);
    HirTextContext ctx; ctx.interner = m->interner.get(); ctx.symbolNames = &m->names;
    DiagnosticReporter r;
    (void)hirTextTypeNodesSpelledTake();            // a clean slate for this thread
    std::string const text = emitHir(*m->hir, ctx, r);
    std::uint64_t const n = hirTextTypeNodesSpelledTake();
    EXPECT_FALSE(r.hasErrors()) << text;
    return n;
}

}  // namespace

TEST(HirTextTypeSpellingCost, DoublingTheMentionsAddsOnlyTheMentions) {
    constexpr std::size_t kMentions = 64;
    constexpr std::size_t kGraph    = 32;
    std::uint64_t const n1 = spelledNodes(kMentions, kGraph);
    std::uint64_t const n2 = spelledNodes(2 * kMentions, kGraph);
    std::cout << "[type-spelling] N=" << kMentions << " G=" << kGraph << " -> " << n1
              << "; N=" << 2 * kMentions << " -> " << n2 << "\n";
    // The closed form, exactly: signature (2) + mentions (2N) + definitions (3G − 2).
    EXPECT_EQ(n1, 2u + 2u * kMentions + (3u * kGraph - 2u));
    // THE PROPERTY: the extra mentions cost their own two nodes each, and the graph
    // is not re-spelled for any of them.
    EXPECT_EQ(n2 - n1, 2u * kMentions);
}

TEST(HirTextTypeSpellingCost, DoublingTheGraphAddsOnlyTheGraph) {
    constexpr std::size_t kMentions = 64;
    constexpr std::size_t kGraph    = 32;
    std::uint64_t const n1 = spelledNodes(kMentions, kGraph);
    std::uint64_t const n2 = spelledNodes(kMentions, 2 * kGraph);
    std::cout << "[type-spelling] G=" << kGraph << " -> " << n1 << "; G=" << 2 * kGraph
              << " -> " << n2 << "\n";
    // The graph's extra G composites cost three nodes each, independent of N.
    EXPECT_EQ(n2 - n1, 3u * kGraph);
}

// CONTROL: the count is a real measurement of THIS writer, not a constant — a
// module with no composite at all spells only its signature.
TEST(HirTextTypeSpellingCost, ControlAModuleWithNoCompositeSpellsOnlyItsSignature) {
    TypeInterner in{CompilationUnitId{96}};
    std::array<TypeId, 1> const params{in.primitive(TypeKind::I32)};
    TypeId const sig = in.fnSig(params, in.primitive(TypeKind::Void), CallConv::CcSysV);
    HirBuilder b{"toy"};
    HirNodeId const fn = b.makeFunction(sig, 1, {}, b.makeBlock(std::vector<HirNodeId>{}));
    Hir hir = std::move(b).finish(b.makeModule(std::vector<HirNodeId>{fn}));
    std::vector<std::string> names{"", "f"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    DiagnosticReporter r;
    (void)hirTextTypeNodesSpelledTake();
    std::string const text = emitHir(hir, ctx, r);
    EXPECT_EQ(hirTextTypeNodesSpelledTake(), 3u) << text;   // fn, i32, void
    EXPECT_EQ(text.find("types {"), std::string::npos) << text;
}

// ── JOB 2: A VALUE-LESS RETURN IS SPELLED `return void` ──────────────────────
//
// ★★★ sqlite's `test/speedtest1.c` made `--emit-hir` refuse its OWN artifact
// (`unexpected node kind 'ExprStmt' in expression position`), and the minimal
// construct is six lines of ordinary C: `if (x) return; g();`. v4 wrote the
// value-less return as a bare `return`, the source map wrote the NEXT statement's
// `@loc` block right after it, and the reader took any following `@` as the start
// of an attributed return VALUE — so `g();` became the return's operand. The
// premise the reader stated ("nothing may follow a value-less return") is false
// twice: C permits dead code, and an `if` then-arm is followed by the statement
// after the `if`. A look-ahead cannot settle it either (`error`/`ext_node` begin a
// statement AND render inline as an expression), so the TEXT carries the answer.
//
// Three shapes, all with a span on every node so every statement opens with its
// own `@loc`: the then-arm (the speedtest1 shape), dead code after a return, and a
// label whose body is a return. Pinned as WRITER output (the exact lines) and as
// READER input (the rebuilt tree has each statement where the source had it).
TEST(HirText, AValueLessReturnFollowedByAStatementRoundTripsWithEverySpan) {
    TypeInterner in{CompilationUnitId{97}};
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const boolT = in.primitive(TypeKind::Bool);
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const gSig  = in.fnSig({}, voidT, CallConv::CcSysV);
    std::array<TypeId, 1> const fParams{i32};
    TypeId const fSig  = in.fnSig(fParams, voidT, CallConv::CcSysV);

    HirBuilder b{"toy"};
    auto const callG = [&] {
        return b.makeExprStmt(b.makeCall(b.makeRef(gSig, 1), {}, voidT));
    };
    HirNodeId const param = b.makeVarDecl(i32, 3);
    HirNodeId const cond  = b.makeBinaryOp(HirOpKind::Ne, b.makeRef(i32, 3),
                                           b.makeLiteral(i32, 0), boolT);
    HirNodeId const ifs   = b.makeIfStmt(cond, b.makeReturn(), std::nullopt);  // if (x) return;
    HirNodeId const after = callG();                                           // g();
    HirNodeId const dead  = b.makeReturn();                                    // return;
    HirNodeId const deadG = callG();                                           // g();  (unreachable)
    HirNodeId const label = b.makeLabelStmt(0, b.makeReturn());               // L0: return;
    HirNodeId const tail  = callG();                                           // g();
    HirNodeId const body  = b.makeBlock(std::vector<HirNodeId>{ifs, after, dead, deadG,
                                                               label, tail});
    HirNodeId const g = b.makeExternFunction(gSig, 1, {});
    HirNodeId const f = b.makeFunction(fSig, 2, std::vector<HirNodeId>{param}, body);
    Hir hir = std::move(b).finish(b.makeModule(std::vector<HirNodeId>{g, f}));

    // A span on EVERY node, so the statement after each return opens with `@loc`.
    HirSourceMap src{hir};
    std::uint32_t off = 0;
    for (std::uint32_t i = 1; i < hir.nodeCount(); ++i) {
        src.set(HirNodeId{i, hir.id().v}, HirSourceLoc{BufferId{5}, SourceSpan::of(off, off + 3)});
        off += 4;
    }
    std::vector<HirTextBufferName> const buffers{{5, "t.c", 0}};
    std::vector<std::string> names{"", "g", "f", "x"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    ctx.sourceMap = &src; ctx.bufferNames = &buffers;

    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    ASSERT_FALSE(r.hasErrors()) << text;
    // WRITER OUTPUT: all three value-less returns say so, and the statement after
    // each one opens with its own `@loc` on the next line — the exact byte shape
    // v4 misread.
    EXPECT_EQ(occurrences(text, "return void\n"), 3u) << text;
    EXPECT_EQ(occurrences(text, "return void\n      @loc(buf 1, "), 3u)
        << "each value-less return must be followed by the NEXT statement's span "
           "line — the byte shape v4 misread\n" << text;
    EXPECT_EQ(text.find("return\n"), std::string::npos)
        << "a bare `return` line reached the artifact\n" << text;
    // THE ROUND TRIP, byte for byte, with verify-on-load.
    EXPECT_EQ(expectRoundTrip(hir, ctx), text);

    // READER INPUT: every statement is rebuilt where it stood — six block members,
    // the then-arm's return with NO value, and each `g();` an expression STATEMENT
    // of the block rather than the operand of the return before it.
    DiagnosticReporter pr;
    auto res = parseHir(text, CompilationUnitId{98}, pr);
    ASSERT_TRUE(res->ok) << text;
    Hir const& h = res->hir;
    HirNodeId const fBack = h.moduleDecls(h.root())[1];
    auto const members = h.children(h.functionBody(fBack));
    std::vector<HirKind> kinds;
    for (HirNodeId m : members) kinds.push_back(h.kind(m));
    EXPECT_EQ(kinds, (std::vector<HirKind>{HirKind::IfStmt, HirKind::ExprStmt,
                                           HirKind::ReturnStmt, HirKind::ExprStmt,
                                           HirKind::LabelStmt, HirKind::ExprStmt}));
    ASSERT_EQ(members.size(), 6u);
    HirNodeId const thenArm = h.ifThen(members[0]);
    EXPECT_EQ(h.kind(thenArm), HirKind::ReturnStmt);
    EXPECT_FALSE(h.returnValue(thenArm).has_value())
        << "the then-arm return came back WITH a value — the v4 misread";
    EXPECT_FALSE(h.returnValue(members[2]).has_value());
    EXPECT_FALSE(h.returnValue(h.labelBody(members[4])).has_value());
}

// ★★ AND A BARE `return` IS REFUSED BY NAME ON THE WAY IN — it never takes the next
// node as its value. The two texts below are exactly what the v4 writer produced
// for `if (x) return; g();` and for a function ending in a value-less return; a v5
// reader must refuse both, name the v5 spelling, and read the following statement
// as the statement it is.
TEST(HirText, ABareReturnIsRefusedByNameAndNeverTakesTheNextNodeAsItsValue) {
    std::string const head =
        "dsshir 5\nproducer \"\"\nbuffers {\n  buf 1 \"t.c\"\n}\n"
        "symbols {\n  %1 \"g\"\n  %2 \"f\"\n}\nmodule \"toy\" {\n"
        "  extern_function %1 : fn() -> void {\n  }\n"
        "  function %2 : fn() -> void {\n    block {\n";
    std::string const tail = "    }\n  }\n}\n";
    std::string const nextStatement =
        "      @loc(buf 1, 10..13)\n"
        "      expr @loc(buf 1, 10..13) call : void (@loc(buf 1, 10..11) ref %1 : fn() -> void)\n";
    struct Arm { char const* what; std::string text; char const* found; };
    std::array<Arm, 2> const arms{{
        {"a bare return followed by the next statement",
         head + "      @loc(buf 1, 1..8)\n      return\n" + nextStatement + tail,
         "the `expr` that follows"},
        {"a bare return at the end of its block",
         head + "      return\n" + tail, "what follows here"},
    }};
    for (Arm const& arm : arms) {
        SCOPED_TRACE(arm.what);
        DiagnosticReporter r;
        auto res = parseHir(arm.text, CompilationUnitId{99}, r);
        EXPECT_FALSE(res->ok) << "a bare `return` was ACCEPTED:\n" << arm.text;
        bool named = false;
        for (auto const& d : r.all()) {
            if (d.actual.find("return void") != std::string::npos
                && d.actual.find(arm.found) != std::string::npos) named = true;
        }
        EXPECT_TRUE(named) << "the refusal did not name `return void`; first: "
                           << (r.all().empty() ? std::string{} : r.all()[0].actual);
        // NEVER THE NEXT NODE AS ITS VALUE: the rebuilt return carries no operand,
        // and the statement after it is still a member of the block.
        Hir const& h = res->hir;
        auto const decls = h.moduleDecls(h.root());
        ASSERT_EQ(decls.size(), 2u);
        auto const members = h.children(h.functionBody(decls[1]));
        ASSERT_FALSE(members.empty());
        EXPECT_EQ(h.kind(members[0]), HirKind::ReturnStmt);
        EXPECT_FALSE(h.returnValue(members[0]).has_value())
            << "the reader took the next node as the bare return's value";
    }
    // CONTROL: the same module spelled the v5 way loads clean.
    std::string const good = head + "      @loc(buf 1, 1..8)\n      return void\n" +
                             nextStatement + tail;
    DiagnosticReporter cr;
    auto ok = parseHir(good, CompilationUnitId{100}, cr);
    EXPECT_TRUE(ok->ok) << (cr.all().empty() ? std::string{} : cr.all()[0].actual);
    auto const members = ok->hir.children(ok->hir.functionBody(
        ok->hir.moduleDecls(ok->hir.root())[1]));
    ASSERT_EQ(members.size(), 2u);
    EXPECT_EQ(ok->hir.kind(members[1]), HirKind::ExprStmt);
}

// The general half of the same fact: a STATEMENT node is refused BY NAME in every
// slot the writer only ever fills with an expression. v4 built `ExprStmt` into a
// return value and reported `ok`; only the re-emission noticed.
TEST(HirText, AStatementInAnExpressionSlotIsRefusedByName) {
    struct Arm { char const* what; char const* line; char const* slot; };
    std::array<Arm, 4> const arms{{
        {"a statement as an initializer", "var %1 : i32 = unreachable", "an initializer"},
        {"a statement as an `expr` operand", "expr expr lit #0 : i32",
         "the operand of an `expr` statement"},
        {"a statement as an operand",
         "expr binop Add : i32 (lit #0 : i32, var %1 : i32)", "an operand"},
        {"a statement as an `if` condition", "if (unreachable)\n        return void",
         "an `if` condition"},
    }};
    for (Arm const& arm : arms) {
        SCOPED_TRACE(arm.what);
        std::string const text =
            std::string{"dsshir 5\nproducer \"\"\nsymbols {\n  %1 \"f\"\n}\n"
                        "module \"toy\" {\n  function %1 : fn() -> void {\n    block {\n      "}
            + arm.line + "\n      return void\n    }\n  }\n}\n";
        DiagnosticReporter r;
        auto res = parseHir(text, CompilationUnitId{101}, r);
        EXPECT_FALSE(res->ok) << text;
        bool named = false;
        for (auto const& d : r.all()) {
            if (d.actual.find(arm.slot) != std::string::npos
                && d.actual.find("takes an EXPRESSION") != std::string::npos) named = true;
        }
        EXPECT_TRUE(named) << "the refusal did not name the slot; first: "
                           << (r.all().empty() ? std::string{} : r.all()[0].actual);
    }
}

// ── DEPTH MUST COST HEAP, NOT HOST CALL FRAMES — THE `.dsshir` WRITER ────────
//
// The operator's standing ruling of 2026-09-02: *"it's well known to not use
// recursive structures in the compiler because big projects like sqlite will for
// sure explode the stack"*. The MIR tier's pins for it live in
// `tests/mir/test_deep_nesting_costs_heap.cpp` and the front end's in
// `tests/hir/test_frontend_deep_nesting_costs_heap.cpp`; these are the HIR TEXT
// WRITER's, and they sit in this file rather than either of those because the
// arms above pin the FORMAT and so do these — what the writer does at the
// format's declared depth limit is a property of the format.
//
// ⚠⚠ THE ROW THAT CONVERTED THIS FILE'S TYPE AND LITERAL PRINTERS
// (D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED)
// READS ✅ CLOSED, AND THE NODE WALK BESIDE THEM SHIPPED RECURSIVE ANYWAY.
// ✔MEASURED 2026-09-08 before the conversion, gdb 14.2 on the mingw-w64 Debug
// build: `dsscp --emit-hir` on a flat 2000-term `x +1 +1 …` chain died with
// STATUS_STACK_OVERFLOW (0xC00000FD), ZERO bytes on stderr and no diagnostic —
// 3333 frames closing a two-frame `emitExpr` → `operands` lambda → `emitExpr`
// cycle at 616 bytes per frame, i.e. ~1232 bytes per level of nesting against
// the 2 MiB reserve `dsscp.exe` declares. `--compile` took the same file at rc 0.
//
// ★★ THE DEEP PINS RUN ON THE BOUNDED STACK (`tests/core/bounded_stack.hpp`,
// 256 KiB), NOT ON THE gtest MAIN THREAD, and that is what makes them a proof
// rather than a margin. A per-level host frame that costs 1232 bytes on this leg
// costs something else on MSVC and something else again on AppleClang; a pin that
// merely COMPLETES on a ~1 MiB thread measures the toolchain it was built with.
// At the 8000 levels below, a recursion costing even 32 bytes per level — less
// than a bare return address plus saved frame pointer — needs the whole 256 KiB,
// so ANY reintroduced host frame overflows this thread on EVERY leg.
//
// ⚠ AND WHAT A RED LOOKS LIKE: a stack overflow kills the process with no
// `[  FAILED  ]` line, so `ctest` reports this whole executable as
// SegFault/Exception rather than naming a case. That is a louder red than an
// assertion, and it is why the CONTROL is declared FIRST — a green control line
// in the log is what says the bounded thread itself is not the thing failing.

#include "../core/bounded_stack.hpp"   // runOnBoundedStack — the BOUND (see there)

namespace {

// `((#0 + #1) + #1) + …` — `depth` binary operators, LEFT-associative, wrapped in
// `function { block { return <chain> } }`. This is the HIR a flat, parenthesis-free
// `return x +1 +1 …;` lowers to: the parser CLIMBS such a chain iteratively, so it
// never counts against `parser.maxExpressionDepth`, and the tree it hands the
// writer is nonetheless `depth` levels deep. That asymmetry is why this shape, and
// not a nested-parenthesis one, is what found the defect.
[[nodiscard]] Hir deepChainModule(TypeInterner& in, std::size_t depth) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const sig = in.fnSig({}, i32, CallConv::CcSysV);
    HirBuilder b{"toy"};
    HirNodeId acc = b.makeLiteral(i32, 0);
    for (std::size_t i = 0; i < depth; ++i)
        acc = b.makeBinaryOp(HirOpKind::Add, acc, b.makeLiteral(i32, 1), i32);
    HirNodeId const body = b.makeBlock(std::vector<HirNodeId>{b.makeReturn(acc)});
    HirNodeId const fn   = b.makeFunction(sig, /*symbol=*/1, {}, body);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{fn});
    return std::move(b).finish(root);
}

// `if (#0) { if (#0) { … return } }` — `depth` nested IfStmts. The STATEMENT half
// of the same ring: `emitNodeLine` ↔ `emitStmtLike` deepened once per level
// exactly as `emitExpr` did, and converting only the expression half would have
// left the ring intact.
[[nodiscard]] Hir deepIfModule(TypeInterner& in, std::size_t depth) {
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const sig   = in.fnSig({}, voidT, CallConv::CcSysV);
    HirBuilder b{"toy"};
    HirNodeId inner = b.makeBlock(std::vector<HirNodeId>{b.makeReturn()});
    for (std::size_t i = 0; i < depth; ++i)
        inner = b.makeBlock(std::vector<HirNodeId>{
            b.makeIfStmt(b.makeLiteral(i32, 0), inner)});
    HirNodeId const fn   = b.makeFunction(sig, /*symbol=*/1, {}, inner);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{fn});
    return std::move(b).finish(root);
}

[[nodiscard]] std::size_t countOccurrences(std::string const& hay, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

} // namespace

// THE CONTROL, DECLARED FIRST AND NAMED. A shallow module emitted on the SAME
// 256 KiB thread. If this is green and the deep pins below are red, the bound is
// discriminating; if this one dies too, the thread is simply too small for
// anything and the deep reds mean nothing.
TEST(HirTextDeepNesting, ControlAShallowModuleEmitsOnTheSameBoundedStack) {
    TypeInterner in{CompilationUnitId{1}};
    Hir hir = deepChainModule(in, /*depth=*/8);
    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;

    DiagnosticReporter r;
    std::string text;
    dss::test::runOnBoundedStack([&] { text = emitHir(hir, ctx, r); });
    EXPECT_FALSE(r.hasErrors());
    EXPECT_EQ(countOccurrences(text, "binop Add"), 8u);
    EXPECT_EQ(text.find('?'), std::string::npos) << text;
}

TEST(HirTextDeepNesting, ADeepExpressionChainCostsHeapNotHostCallFrames) {
    // 8000 levels: a reintroduced host frame of even 32 bytes needs the whole
    // 256 KiB reserve, and the smallest frame any supported toolchain emits for a
    // call with arguments is several times that. The converted walk carries O(1)
    // host stack per level, so it completes with the reserve barely touched.
    constexpr std::size_t kChain = 8000;
    TypeInterner in{CompilationUnitId{1}};
    Hir hir = deepChainModule(in, kChain);
    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;

    // gtest assertions belong on the CALLING thread (`bounded_stack.hpp`), so the
    // callable only collects.
    DiagnosticReporter r;
    std::string text;
    dss::test::runOnBoundedStack([&] { text = emitHir(hir, ctx, r); });

    EXPECT_FALSE(r.hasErrors())
        << "a chain the front end accepts must not make the writer report";
    // NOT `EXPECT_FALSE(text.empty())`: a truncating writer would also pass that.
    // Every level must be in the bytes.
    EXPECT_EQ(countOccurrences(text, "binop Add"), kChain);
    EXPECT_EQ(text.find('?'), std::string::npos)
        << "the poison token appeared below the format's depth limit";
}

TEST(HirTextDeepNesting, DeeplyNestedStatementsCostHeapNotHostCallFrames) {
    // The statement half of the same ring. ⓘ The indent string grows one level per
    // nesting level here, so the TEXT is quadratic in `kNest` where the expression
    // axis is linear — which is why this axis is pinned shallower. The host-stack
    // arithmetic is unchanged: at 2000 levels a 128-byte frame already needs the
    // whole 256 KiB reserve.
    constexpr std::size_t kNest = 2000;
    TypeInterner in{CompilationUnitId{1}};
    Hir hir = deepIfModule(in, kNest);
    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;

    DiagnosticReporter r;
    std::string text;
    dss::test::runOnBoundedStack([&] { text = emitHir(hir, ctx, r); });

    EXPECT_FALSE(r.hasErrors());
    EXPECT_EQ(countOccurrences(text, "if "), kNest);
    EXPECT_EQ(text.find('?'), std::string::npos) << "the poison token appeared";
}

// ── THE COUNTER THAT REPLACED THE HOST STACK ────────────────────────────────
//
// ★★★ THE LIMIT DID NOT DISAPPEAR WITH THE RECURSION, and this is what says so.
// `kHirTextMaxNodeDepth` is a declared property of the format, so a module past
// it is REFUSED — an Error diagnostic that names the limit, plus the `?` poison
// token in the text. ⚠ NOT an `error` node: `error` is a LEGAL construct, so
// truncating to one would hand a consumer a smaller program that parses cleanly.
// The `?` cannot be read back, which is the whole point of using it.
//
// ⓘ EACH ARM IS 16 LEVELS CLEAR OF THE BOUNDARY, deliberately: the fixture adds
// its own wrapper levels (module decl → block → return → the chain), so an
// exact-boundary assertion would pin the fixture's shape rather than the limit.
// What is pinned is the only thing that matters — which side of the declared
// number a module falls on decides whether it is spelled or refused.
TEST(HirTextDeepNesting, ControlJustUnderTheFormatDepthLimitTheModuleIsSpelled) {
    TypeInterner in{CompilationUnitId{1}};
    Hir hir = deepChainModule(in, kHirTextMaxNodeDepth - 16);
    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;

    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);
    EXPECT_FALSE(r.hasErrors())
        << "a module UNDER the declared limit was refused: "
        << (r.all().empty() ? std::string{} : r.all()[0].actual);
    EXPECT_EQ(text.find('?'), std::string::npos);
    EXPECT_EQ(countOccurrences(text, "binop Add"),
              static_cast<std::size_t>(kHirTextMaxNodeDepth) - 16u);
}

TEST(HirTextDeepNesting, PastTheFormatDepthLimitTheWriterRefusesByNameAndPoisonsTheText) {
    TypeInterner in{CompilationUnitId{1}};
    Hir hir = deepChainModule(in, kHirTextMaxNodeDepth + 16);
    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;

    DiagnosticReporter r;
    std::string const text = emitHir(hir, ctx, r);

    // (1) LOUD — an Error, which is what makes `--emit-hir` exit non-zero rather
    // than write the file and claim success.
    EXPECT_TRUE(r.hasErrors()) << "a module past the declared depth limit was "
                                 "spelled with nothing reported";
    // (2) BY NAME — the refusal states the limit it enforced and the format it
    // belongs to, so a reader can act on it without reading this file.
    bool named = false;
    for (auto const& d : r.all()) {
        if (d.actual.find(std::to_string(kHirTextMaxNodeDepth)) != std::string::npos
            && d.actual.find(".dsshir") != std::string::npos
            && d.actual.find("depth") != std::string::npos)
            named = true;
    }
    EXPECT_TRUE(named) << "the refusal did not name the limit it enforced; first: "
                       << (r.all().empty() ? std::string{} : r.all()[0].actual);
    // (3) NOT A SILENT TRUNCATION — the poison token is in the bytes, so the file
    // cannot be read back as a smaller, valid program.
    EXPECT_NE(text.find('?'), std::string::npos)
        << "the text past the limit carries no poison token — a consumer would "
           "read it as a complete module";
}

// The poison token really is poison: `?` where a statement goes is REFUSED on the
// way back in. Pinned on a two-line artifact rather than by re-parsing the
// multi-megabyte one the arm above produces — the property is the reader's
// treatment of `?`, and it does not need the depth to be exercised.
TEST(HirTextDeepNesting, ThePoisonTokenTheDepthRefusalWritesIsRefusedOnTheWayBackIn) {
    std::string const text =
        "dsshir 5\nproducer \"\"\nsymbols {\n  %1 \"main\"\n}\n"
        "module \"toy\" {\n  ?\n}\n";
    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{91}, r);
    EXPECT_FALSE(res->ok) << "the `?` a depth refusal writes was ACCEPTED";

    // CONTROL: the identical artifact with a real statement in that slot loads.
    std::string const ok =
        "dsshir 5\nproducer \"\"\nsymbols {\n  %1 \"main\"\n}\n"
        "module \"toy\" {\n  unreachable\n}\n";
    DiagnosticReporter cr;
    auto good = parseHir(ok, CompilationUnitId{92}, cr);
    EXPECT_TRUE(good->ok) << "the well-formed control was refused: "
                          << (cr.all().empty() ? std::string{} : cr.all()[0].actual);
}

// ── THE READING HALF OF THE SAME RING ────────────────────────────────────────
//
// ⚠⚠ A CODEC IS ONE THING, AND HALF A CONVERSION IS THE SHAPE THIS ROW KEEPS
// TAKING. `parseHir` is a `DSS_EXPORT`ed entry point that consumes UNTRUSTED
// text, and it called itself once per nested node. ✔MEASURED 2026-09-08 through
// `ctest` on the ordinary gtest main thread (~1 MiB, mingw-w64 g++ 13.2 Debug),
// bisected on writer-produced artifacts: **700 levels parsed clean, 800
// SEGFAULTed** — against a writer that, once flattened, will spell 262144. So
// `emitHir` would hand a consumer a file that killed the process on the way back
// in, and the round trip is this format's whole contract.
//
// The artifacts below are HAND-BUILT rather than taken from `emitHir`, because
// the over-limit arm needs text the writer will not produce (it refuses at the
// same number). The shallow CONTROL uses the identical generator, so a green
// control is what says the hand-built grammar is the writer's grammar and the
// deep arm differs from it in nothing but depth.
namespace {

// `function %1 : fn() -> i32 { block { return <depth-deep left-assoc chain> } }`,
// spelled exactly as `emitHir` spells it for a pool-less module.
[[nodiscard]] std::string deepChainArtifact(std::size_t depth) {
    std::string s;
    s.reserve(depth * 32 + 256);
    s += "dsshir 5\nproducer \"\"\nsymbols {\n  %1 \"main\"\n}\n"
         "module \"toy\" {\n  function %1 : fn() -> i32 {\n    block {\n      return ";
    for (std::size_t i = 0; i < depth; ++i) s += "binop Add : i32 (";
    s += "lit #0 : i32";
    for (std::size_t i = 0; i < depth; ++i) s += ", lit #0 : i32)";
    s += "\n    }\n  }\n}\n";
    return s;
}

} // namespace

// THE CONTROL, DECLARED FIRST: the same generator, shallow, on the same bounded
// thread. Green here is what makes a red below attributable to depth.
TEST(HirTextDeepNesting, ControlAShallowArtifactIsReadBackOnTheSameBoundedStack) {
    std::string const text = deepChainArtifact(8);
    bool ok = false;
    std::string firstDiag;
    dss::test::runOnBoundedStack([&] {
        DiagnosticReporter r;
        auto res = parseHir(text, CompilationUnitId{93}, r);
        ok = res && res->ok;
        if (!ok && !r.all().empty()) firstDiag = r.all()[0].actual;
    });
    EXPECT_TRUE(ok) << "the hand-built grammar is not the writer's grammar: " << firstDiag;
}

TEST(HirTextDeepNesting, ADeepArtifactIsReadBackOnABoundedStackAndReEmitsByteForByte) {
    // 4000 levels on the 256 KiB thread. The recursive reader's ceiling was ~700
    // on FOUR times this reserve, so this arm is red on every leg the moment a
    // host frame per level comes back — and it pins the ROUND TRIP, not merely
    // that a parse returned: the rebuilt module must re-emit the same bytes, which
    // is the property a truncating or reordering reader would break while still
    // reporting `ok`.
    constexpr std::size_t kDepth = 4000;
    std::string const text = deepChainArtifact(kDepth);

    bool        ok = false;
    bool        identical = false;
    std::string firstDiag;
    dss::test::runOnBoundedStack([&] {
        DiagnosticReporter r;
        auto res = parseHir(text, CompilationUnitId{94}, r);
        ok = res && res->ok;
        if (!ok) {
            if (!r.all().empty()) firstDiag = r.all()[0].actual;
            return;
        }
        HirTextContext ctx2;
        ctx2.interner      = &res->interner;
        ctx2.symbolNames   = &res->symbolNames;
        ctx2.producer      = res->producer;
        ctx2.bufferNames   = &res->bufferNames;
        ctx2.sourceMap     = &res->sourceMap;
        ctx2.ffiMap        = &res->ffiMap;
        ctx2.shaderMap     = &res->shaderMap;
        ctx2.transpileMap  = &res->transpileMap;
        ctx2.diagnosticMap = &res->diagnosticMap;
        ctx2.literalPool   = &res->literalPool;
        ctx2.inlineAsmPool = &res->inlineAsmPool;
        DiagnosticReporter r2;
        identical = (emitHir(res->hir, ctx2, r2) == text);
    });
    ASSERT_TRUE(ok) << "a " << kDepth << "-level artifact was refused: " << firstDiag;
    EXPECT_TRUE(identical) << "the round trip changed the bytes at depth " << kDepth;
}

// ★★★ ONE NUMBER, BOTH HALVES. `kHirTextMaxNodeDepth` is the FORMAT's limit, so
// the reader enforces exactly what the writer enforces — a writer that spelled
// deeper than its own reader accepts would ship artifacts that fail at the
// consumer instead of here, which is the asymmetry this arm exists to forbid.
TEST(HirTextDeepNesting, PastTheFormatDepthLimitTheReaderRefusesByNameRatherThanReading) {
    std::string const text = deepChainArtifact(kHirTextMaxNodeDepth + 16);
    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{95}, r);
    EXPECT_FALSE(res && res->ok)
        << "a file past the declared depth limit was READ rather than refused";
    bool named = false;
    for (auto const& d : r.all()) {
        if (d.actual.find(std::to_string(kHirTextMaxNodeDepth)) != std::string::npos
            && d.actual.find(".dsshir") != std::string::npos
            && d.actual.find("depth") != std::string::npos)
            named = true;
    }
    EXPECT_TRUE(named) << "the reader's refusal did not name the limit it enforced; first: "
                       << (r.all().empty() ? std::string{} : r.all()[0].actual);
}

// ── THREE SPELLINGS THE WRITER PRODUCED AND THE READER COULD NOT READ ────────
//
// ⚠⚠ FOUND BY SWEEPING THE CLASS, NOT BY LOOKING FOR THEM. Round-tripping every
// artifact `emitHir` produces for the whole `examples/c` corpus — 720 files —
// through `parseHir` and back out found three, and each fails DIFFERENTLY, which
// is the point: a codec's two halves drift apart one spelling at a time, and only
// a corpus-wide comparison sees it.
//
//   (a) `arr<T, -2>`  — a C99 VLA's interned bound. The reader read the dimension
//       with the unsigned `takeInt()`, which cannot see the `-`, so it built
//       `arr<T, 0>` and then choked on the `2` still in the stream. ✔MEASURED
//       2026-09-08: `c99_vla_fixed_array_arg` did not fail to load, it ABORTED
//       the process — `dss::substrate fatal: TypeInterner::get: TypeId out of
//       range`. Fixed by `takeSignedInt`, used by every arm that reads a
//       verbatim-printed `scalars()` value.
//   (b) `goto *<expr>` — the computed-goto form. The LEXER had no `*` token at
//       all, so the byte became `Tk::Unknown` and the reader's `goto` arm, which
//       knew only `L<ord>`, refused four shipped artifacts by name.
//   (c) `lit float 18446744073709551616` — 2^64, which `std::format` renders with
//       neither point nor exponent, so it lexes as an INTEGER that does not fit a
//       `std::uint64_t`. The float reader took the wrapped accumulator and
//       returned **0.0 with a clean reporter** — the only one of the three that
//       was SILENT, and therefore the worst.
//
// ⓘ Each pin below is the smallest module that carries its spelling, and each
// goes through `expectRoundTrip`, which asserts emit → parse → re-emit is byte
// identity AND that the parse (verify-on-load included) is clean. A pin that only
// asserted "it parses" would have passed for (c).
TEST(HirTextRoundTripGaps, AVlaBoundArrayTypeSurvivesTheTextTier) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    // -2 is the interner's VLA-bound sentinel — the shape
    // D-CSUBSET-VLA-FIXED-ARRAY-ARG-COMPAT is about, and the one the corpus hit.
    TypeId const vla = in.array(i32, -2);

    HirBuilder b{"toy"};
    HirNodeId const decl = b.makeTypeDecl(vla, /*symbol=*/1);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{decl});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "vla"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("arr<i32, -2>"), std::string::npos)
        << "the sentinel bound must be SPELLED, not normalised away:\n" << text;
}

TEST(HirTextRoundTripGaps, AComputedGotoTargetSurvivesTheTextTier) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const pvoid = in.pointer(voidT);
    TypeId const sig   = in.fnSig({}, voidT, CallConv::CcSysV);

    HirBuilder b{"toy"};
    // `L0: goto *&&L0;` — the smallest program that carries both halves of
    // D-CSUBSET-COMPUTED-GOTO through the text tier.
    HirNodeId const target = b.makeLabelAddressOf(/*labelOrdinal=*/0, pvoid);
    HirNodeId const jump   = b.makeIndirectGotoStmt(target);
    HirNodeId const label  = b.makeLabelStmt(/*ordinal=*/0, jump);
    HirNodeId const body   = b.makeBlock(std::vector<HirNodeId>{label});
    HirNodeId const fn     = b.makeFunction(sig, /*symbol=*/1, {}, body);
    HirNodeId const root   = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("goto *"), std::string::npos)
        << "the computed-goto form must be spelled:\n" << text;
}

TEST(HirTextRoundTripGaps, AFloatLiteralPastUint64DoesNotSilentlyBecomeZero) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const f64   = in.primitive(TypeKind::F64);
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const sig   = in.fnSig({}, voidT, CallConv::CcSysV);

    // 2^64 exactly — `(double)UINT64_MAX + 1`, which `std::format` renders as
    // `18446744073709551616`: all digits, no point, no exponent, one past what a
    // `std::uint64_t` accumulator can hold.
    constexpr double kPastUint64 = 18446744073709551616.0;
    HirLiteralPool pool;
    HirBuilder b{"toy"};
    HirNodeId const lit  = b.makeLiteral(f64, pool.add(HirLiteralValue{kPastUint64, TypeKind::F64}));
    HirNodeId const body = b.makeBlock(std::vector<HirNodeId>{b.makeExprStmt(lit), b.makeReturn()});
    HirNodeId const fn   = b.makeFunction(sig, /*symbol=*/1, {}, body);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx; ctx.interner = &in; ctx.symbolNames = &names; ctx.literalPool = &pool;
    std::string const text = expectRoundTrip(hir, ctx);
    EXPECT_NE(text.find("lit float 18446744073709551616"), std::string::npos)
        << "the writer's spelling for this value moved:\n" << text;

    // ⚠ THE VALUE, NOT ONLY THE BYTES. The re-emit above already catches this,
    // but naming the rebuilt double is what says WHAT went wrong when it breaks:
    // the defect this pins returned 0.0 and reported nothing.
    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{96}, r);
    ASSERT_TRUE(res && res->ok);
    ASSERT_EQ(res->literalPool.size(), 1u);
    EXPECT_EQ(std::get<double>(res->literalPool.at(0).value), kPastUint64);
}
