// HR7 `.dsshir` text-format tests: in-memory byte-identical round-trip across the
// node/type/attribute surface, verify-on-load, parse-error reporting, and a
// golden corpus (DSS_REFRESH_GOLDENS=1 to regenerate, mirroring test_corpus.cpp).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
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

    HirNodeId armV = b.makeLiteral(i32, 5);
    HirNodeId arm0 = b.makeCaseArm(armV, std::vector<HirNodeId>{b.makeBreak(0)});
    HirNodeId armD = b.makeCaseArm(std::nullopt, std::vector<HirNodeId>{b.makeReturn()});
    HirNodeId sw = b.makeSwitchStmt(b.makeLiteral(i32, 0), std::vector<HirNodeId>{arm0, armD});

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
    EXPECT_NE(text.find("default {"), std::string::npos);
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
    std::ostringstream buf; buf << in.rdbuf(); return std::move(buf).str();
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
        DiagnosticReporter r4;
        EXPECT_EQ(out, emitHir(res2->hir, ctx2, r4));
    }
    EXPECT_TRUE(sawAny) << "no .dsshir corpus files found under " << root.string();
}
