// HR8 CST→HIR lowering tests: end-to-end (parse c → semantic → lowerToHir
// → verify) over the covered c slice, a deferred-construct diagnostic, and
// a `.dsshir` golden of a representative program. Genericity (no schema.name()
// dependence) is guaranteed by construction — the engine never inspects the
// language name — and demonstrated here by lowering a real shipped language
// through the single generic engine.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/substrate/large_stack_call.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/target_schema.hpp"   // D-TEST-THE-HIR-LOWERING-FIXTURE-ANALYZES-WITH-NO-TARGET-IN-SCOPE
#include "core/types/wide_string_encode.hpp"   // elementByteWidth (assert unit COUNTS, not format-specific bytes)
#include "hir/const_eval.hpp"
#include "hir/hir.hpp"
#include "hir/hir_intrinsic_registry.hpp"
#include "hir/hir_text.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "repo_root.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include "core/substrate/checked_file_read.hpp"   // the ONE checked whole-file read
#include "shipped_schema_or_throw.hpp"   // the ONE load-or-fail-this-test helper

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace dss;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == c) ++n;
    return n;
}

// D-TEST-THE-HIR-LOWERING-FIXTURE-ANALYZES-WITH-NO-TARGET-IN-SCOPE: the shipped
// `x86_64` target document + the `sysv_amd64` va_list strategy this file's fixtures
// analyze under, OWNED for the whole process.
//
// ★★ WHY A PROCESS-LIFETIME OWNER AND NOT A LOCAL. `analyze` takes the target
// NON-OWNING and the returned `SemanticModel` REPUBLISHES it as `model.target()`
// for the HIR lowering to read. `analyzeC` returns the model BY VALUE, so a
// function-local `shared_ptr` would be destroyed at the `return` and every caller
// would lower against a dangling target. The sibling harness in
// `tests/mir/test_mir_lowering_c.cpp` solves the same problem by holding the target
// in its returned `Lowered` struct, ordered BEFORE `model` so members destroy in
// the right order — it cannot be copied here without changing this helper's
// signature at all 290-odd call sites, and a function-local static is the same
// guarantee with none of that churn. Loaded once, thread-safely (C++11 magic
// statics), and never freed.
//
// ★ WHY THE FIXTURE NEEDS IT AT ALL. `analyze`'s `target` parameter defaults to
// `nullptr` for direct-API callers, which is a DELIBERATE default and correct for
// the LSP — but it means this fixture analyzes a construct with LESS in scope than
// the shipped CLI gives it, and a red here can therefore name a "compiler defect"
// that the CLI does not have. That cost a P42 lane a whole measurement pass. A
// fixture must not be able to fail on a construct the product compiles.
// ⚠ ONLY the target (and the va_list strategy derived FROM it) is threaded. The
// dataModel stays LP64 and the long-double axis stays `None` — those two change
// TYPE WIDTHS, so moving them would silently re-point every one of this file's
// fixtures at a different ABI, which is a separate decision with its own pins, not
// a fixture repair. `analyzeCPe` remains the deliberate LLP64/PE variant.
[[nodiscard]] TargetSchema const* fixtureTarget() {
    static std::shared_ptr<TargetSchema const> const kTarget = [] {
        auto t = TargetSchema::loadShipped("x86_64");
        return t.has_value() ? *t : nullptr;
    }();
    return kTarget.get();
}
[[nodiscard]] std::optional<VaListStrategy> fixtureVaListStrategy() {
    TargetSchema const* t = fixtureTarget();
    if (t == nullptr) return std::nullopt;
    auto const* cc = t->callingConventionByName("sysv_amd64");
    if (cc == nullptr || !cc->vaListLayout.has_value()) return std::nullopt;
    return cc->vaListLayout->strategy;
}

// Drive: c source → CompilationUnit → SemanticModel. Asserts the front
// end (parse + semantic) is clean so a lowering test never chases a phantom.
[[nodiscard]] SemanticModel analyzeC(std::string src) {
    // D-TEST-A-TORN-SHIPPED-CONFIG-CRASHES-A-SUITE-INSTEAD-OF-REDDING-IT:
    // this was `ADD_FAILURE() << "loadShipped(...) failed"; std::abort();`.
    // ✔MEASURED against an emptied shipped config, the abort took the whole
    // binary out at 0xC0000409 with no `[  FAILED  ]` line, no case name and
    // no summary -- every sibling test in this executable lost its verdict.
    auto const loaded = dss::test_support::shippedSchemaOrThrow("c");
    UnitBuilder builder{loaded, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    // D-TEST-THE-HIR-LOWERING-FIXTURE-ANALYZES-WITH-NO-TARGET-IN-SCOPE: the target
    // + its va_list strategy, exactly as `compile_pipeline.cpp` and the sibling MIR
    // harness thread them. See `fixtureTarget` for why they are process-owned.
    // [[D-CSUBSET-CONST-EVAL-CHAR-SIGNEDNESS]]: the OBJECT FORMAT is threaded
    // too, and it is not decoration here. Plain `char`'s signedness is a
    // (processor x object format) fact — the same arm64 CPU is unsigned under
    // GNU/Linux and signed under Darwin — and the accessor requires the format
    // KIND precisely so no caller can take the processor half alone. Without it
    // this fixture analyzed with a target but NO answer for `char`, so a
    // high-byte character constant would refuse here while compiling cleanly
    // through the real pipeline. ELF is what every fixture in this file was
    // implicitly written against.
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   std::nullopt, fixtureVaListStrategy(), ObjectFormatKind::Elf,
                   std::nullopt, LongDoubleFormat::None, fixtureTarget());
}

// As `analyzeC`, but under the PE object format — so `L'…'`/`L"…"` (wchar_t)
// resolves to the 2-byte Windows UTF-16 unit (U16), not the POSIX I32. Used to
// witness the FORMAT-keyed wide-char constraint (an astral `L'😀'` is representable
// under the default I32 but NOT under the pe U16).
[[nodiscard]] SemanticModel analyzeCPe(std::string src) {
    // D-TEST-A-TORN-SHIPPED-CONFIG-CRASHES-A-SUITE-INSTEAD-OF-REDDING-IT:
    // this was `ADD_FAILURE() << "loadShipped(...) failed"; std::abort();`.
    // ✔MEASURED against an emptied shipped config, the abort took the whole
    // binary out at 0xC0000409 with no `[  FAILED  ]` line, no case name and
    // no summary -- every sibling test in this executable lost its verdict.
    auto const loaded = dss::test_support::shippedSchemaOrThrow("c");
    UnitBuilder builder{loaded, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    return analyze(cu, DiagnosticBudget::libraryDefault(),
                   DataModel::Llp64, std::nullopt, std::nullopt,
                   ObjectFormatKind::Pe);
}

// Drive c → SemanticModel with the parser's expression-depth cap RAISED to
// `cap` (the default is 256), so a deep-but-legal nesting beyond the cap parses to
// completion — the DEEP-NEST-RECURSION lowering pins need an input deeper than any
// program the shipped cap admits. The deep CST's parse + the orthogonal recursive
// analyze + the deep tree's teardown all run on the 64 MiB worker (the standard
// pipeline stack); `lowerToHir` is then called by the test ON ITS OWN MAIN STACK,
// which is the flat-property witness for the lowerer. A bare `int main(){…}`
// program (no `#include`) parses via Tokenizer+Parser directly (skipping the PP)
// and is ingested via `UnitBuilder::addTree` — exactly the construct these pins use.
[[nodiscard]] SemanticModel analyzeCRaisedCap(std::string src, std::size_t cap) {
    // D-TEST-A-TORN-SHIPPED-CONFIG-CRASHES-A-SUITE-INSTEAD-OF-REDDING-IT:
    // this was `ADD_FAILURE() << "loadShipped(...) failed"; std::abort();`.
    // ✔MEASURED against an emptied shipped config, the abort took the whole
    // binary out at 0xC0000409 with no `[  FAILED  ]` line, no case name and
    // no summary -- every sibling test in this executable lost its verdict.
    auto const loaded = dss::test_support::shippedSchemaOrThrow("c");
    std::shared_ptr<GrammarSchema const> schema = loaded;
    auto srcBuf = SourceBuffer::fromString(std::move(src), "<deepmem>");
    Tokenizer tk{srcBuf, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    ParserConfig cfg;
    cfg.maxExpressionDepth = cap;
    Parser p{srcBuf, schema, std::move(stream), DiagnosticBudget::libraryDefault(),
             std::move(cfg), std::move(lexDiags)};
    ParseResult result = std::move(p).parse();
    if (result.tree.diagnostics().hasErrors()) {
        ADD_FAILURE() << "raised-cap parse produced errors (cap=" << cap << ")";
    }
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addTree(std::move(result.tree));
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    return analyze(cu, DiagnosticBudget::libraryDefault());
}

[[nodiscard]] std::vector<std::string> symbolNames(SemanticModel const& m) {
    std::vector<std::string> names;
    auto const& syms = m.symbols();
    names.reserve(syms.size());
    for (auto const& s : syms) names.push_back(s.name);
    return names;
}

[[nodiscard]] HirNodeId firstFunction(Hir const& hir) {
    for (HirNodeId d : hir.moduleDecls(hir.root())) {
        if (hir.kind(d) == HirKind::Function) return d;
    }
    return HirNodeId{};
}

// First node of `want` kind in the subtree rooted at `n` (pre-order). Used to
// reach the shift BinaryOp past whatever statement wrapper holds it.
[[nodiscard]] HirNodeId findFirstByKind(Hir const& hir, HirNodeId n, HirKind want) {
    if (!n.valid()) return {};
    if (hir.kind(n) == want) return n;
    for (HirNodeId c : hir.children(n)) {
        if (HirNodeId r = findFirstByKind(hir, c, want); r.valid()) return r;
    }
    return {};
}

// The shipped c JSON text (for shiftResult perturbation), located
// through the ONE test-side resolver (`repo_root.hpp`: $DSS_CONFIG_ROOT → the
// CMake-baked repo root → the cwd ancestor walk). It used to carry a private
// copy of that walk, which resolves nothing in an OUT-OF-TREE build — the cwd
// there has no `src/dss-config` anywhere in its ancestry — and the empty text it
// then returned fell into the caller's `std::abort()`, taking down the whole
// test BINARY and every sibling test's verdict with it. `configRoot()` throws
// instead, which GoogleTest reports as a failure of the one running test.
// Returned as raw text so the perturbation is a surgical textual swap of the
// closed verb value — no JSON library dependency in this target.
[[nodiscard]] std::string shippedCText() {
    fs::path const cand =
        dss::test::configRoot() / "sources" / "c.lang.json";
    // D-TEST-A-TORN-SHIPPED-CONFIG-CRASHES-A-SUITE-INSTEAD-OF-REDDING-IT: this
    // returned an EMPTY string on an unreadable config, and the caller then
    // aborted on the resulting missing needle -- so an I/O fault surfaced as
    // "shiftResult key not found", one frame away from the truth, and killed
    // the binary. THROW at the fault, and read through the ONE checked read
    // (D-CORE-SHIPPED-CONFIG-LOADERS-DRAIN-A-STREAM-WITHOUT-CHECKING-IT) so a
    // TORN read is named as a read failure rather than as a bad document.
    auto text = dss::core::readFileChecked(cand);
    if (!text) {
        throw std::runtime_error("shipped c.lang.json: "
                                 + std::move(text).error().message);
    }
    return *std::move(text);
}

// Lower `void f(int a, long b) { a << b; }` under a schema whose
// arithmeticConversions.shiftResult is `verb`, and return the TypeKind the
// shift BinaryOp carries. The shift is an EXPRESSION STATEMENT (no return/assign
// coercion), so the BinaryOp's own type IS the shift's result type:
// `promotedLeft` (C 6.5.7) → the promoted left operand int (I32); `commonType`
// → the usual-arithmetic common type of (int, long) = long (I64). Exercises the
// cst_to_hir shift arm — the site D-UAC-SHIFT-RESULT-RULE-CONFIG names.
[[nodiscard]] TypeKind shiftResultKind(std::string const& verb) {
    std::string text = shippedCText();
    // The shipped config declares `promotedLeft`; swap ONLY that closed-verb
    // value (unique in the file — the doc comment uses backticks, not quotes).
    std::string const needle = "\"shiftResult\": \"promotedLeft\"";
    auto const pos = text.find(needle);
    if (pos == std::string::npos) {
        throw std::runtime_error(
            "shiftResult key not found in shipped c config");
    }
    text.replace(pos, needle.size(), "\"shiftResult\": \"" + verb + "\"");
    auto schema = GrammarSchema::loadFromText(text,
                                              "<shiftResult-" + verb + ">");
    if (!schema) {
        throw std::runtime_error("perturbed schema (shiftResult=" + verb
                                 + ") failed to load");
    }
    UnitBuilder builder{*schema, DiagnosticBudget::libraryDefault()};
    builder.addInMemory("void f(int a, long b) { a << b; }\n", "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel model = analyze(cu, DiagnosticBudget::libraryDefault());
    if (model.hasErrors()) {
        throw std::runtime_error("front-end errors under shiftResult=" + verb);
    }
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    if (!res->ok) {
        throw std::runtime_error("lowering failed under shiftResult=" + verb);
    }
    HirNodeId const fn = firstFunction(res->hir);
    HirNodeId const shift =
        findFirstByKind(res->hir, res->hir.functionBody(fn), HirKind::BinaryOp);
    if (!shift.valid()) {
        throw std::runtime_error("no BinaryOp in body under shiftResult="
                                 + verb);
    }
    return model.lattice().interner().kind(res->hir.typeId(shift));
}

} // namespace

TEST(HirLoweringC, EmptyVoidFunction) {
    SemanticModel model = analyzeC("void f() {}");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok);
    HirNodeId root = res->hir.root();
    ASSERT_EQ(res->hir.kind(root), HirKind::Module);
    auto decls = res->hir.moduleDecls(root);
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::Function);
    // Function body is a Block.
    EXPECT_EQ(res->hir.kind(res->hir.functionBody(decls[0])), HirKind::Block);
}

TEST(HirLoweringC, ReturnLiteralPopulatesPool) {
    SemanticModel model = analyzeC("int f() { return 42; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(v.value));
    EXPECT_EQ(std::get<std::int64_t>(v.value), 42);
    EXPECT_EQ(v.core, TypeKind::I32);
}

// FC16 C11/C23 6.5.1.1: `_Generic` lowers to the SELECTED association's
// expression — its type + value. `i` is `int`, so the `int:` branch is selected
// and its Literal 5 IS the returned value (result type I32).
TEST(HirLoweringC, GenericLowersSelectedBranchValue) {
    SemanticModel model = analyzeC(
        "int f() { int i = 0; return _Generic(i, int: 5, double: 3, "
        "default: 0); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    HirNodeId const body = res->hir.functionBody(fn);
    // The return value is the selected int-branch's Literal 5 (I32).
    HirNodeId const lit =
        findFirstByKind(res->hir, body, HirKind::Literal);
    ASSERT_TRUE(lit.valid()) << "the selected branch's literal must be lowered";
    EXPECT_EQ(res->hir.kind(lit), HirKind::Literal);
    EXPECT_EQ(model.lattice().interner().kind(res->hir.typeId(lit)),
              TypeKind::I32)
        << "the selected int-branch's value types I32";
}

// FC16 6.5.1.1p3: the NON-selected association expressions are NOT evaluated —
// they must NOT be lowered. The non-selected `double: 999.0` branch's distinctive
// literal 999 must NOT reach the literal pool (only the selected `int: 5` does).
TEST(HirLoweringC, GenericNonSelectedBranchNotLowered) {
    SemanticModel model = analyzeC(
        "int f() { int i = 0; return _Generic(i, int: 5, "
        "double: 999, default: 777); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    // ONLY the selected branch's Literal 5 is lowered; 999 and 777 (the
    // non-selected + default branches) must be absent from the pool.
    bool has5 = false, has999 = false, has777 = false;
    for (std::size_t i = 0; i < res->literalPool.size(); ++i) {
        auto const& v = res->literalPool.at(i);
        if (std::holds_alternative<std::int64_t>(v.value)) {
            auto const iv = std::get<std::int64_t>(v.value);
            if (iv == 5)   has5 = true;
            if (iv == 999) has999 = true;
            if (iv == 777) has777 = true;
        }
    }
    EXPECT_TRUE(has5)    << "the selected int-branch literal 5 must be lowered";
    EXPECT_FALSE(has999) << "the non-selected double-branch literal 999 must NOT "
                            "be lowered (6.5.1.1p3: unevaluated)";
    EXPECT_FALSE(has777) << "the unselected default-branch literal 777 must NOT "
                            "be lowered";
}

TEST(HirLoweringC, ArithmeticAndParams) {
    SemanticModel model = analyzeC("int add(int a, int b) { return a + b; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    ASSERT_EQ(res->hir.kind(fn), HirKind::Function);
    EXPECT_EQ(res->hir.functionParams(fn).size(), 2u);          // a, b
    HirNodeId body = res->hir.functionBody(fn);
    auto stmts = res->hir.children(body);
    ASSERT_EQ(stmts.size(), 1u);
    ASSERT_EQ(res->hir.kind(stmts[0]), HirKind::ReturnStmt);
    HirNodeId ret = *res->hir.returnValue(stmts[0]);
    EXPECT_EQ(res->hir.kind(ret), HirKind::BinaryOp);           // a + b
}

// D-UAC-SHIFT-RESULT-RULE-CONFIG: the C 6.5.7 shift-result rule is the config
// verb `shiftResult`, read by the cst_to_hir shift arm (the site the anchor
// names). `promotedLeft` types `int << long` as the promoted left operand (I32);
// `commonType` types it like an ordinary binary op (common(int,long) = I64). The
// I32↔I64 flip when ONLY the verb changes is the red-on-disable proof the engine
// reads the verb at the HIR-lowering tier (the const-context sibling site is
// pinned in test_fc3_width_semantics.cpp).
TEST(HirLoweringC, ShiftResultPromotedLeftIsLeftType) {
    EXPECT_EQ(shiftResultKind("promotedLeft"), TypeKind::I32)
        << "promotedLeft (C 6.5.7): (int << long) lowers to a BinaryOp typed I32";
}

TEST(HirLoweringC, ShiftResultCommonTypeIsCommonType) {
    EXPECT_EQ(shiftResultKind("commonType"), TypeKind::I64)
        << "commonType: (int << long) lowers to a BinaryOp typed I64 — the "
           "red-on-disable flip at the cst_to_hir site";
}

TEST(HirLoweringC, ControlFlowAndAssignment) {
    SemanticModel model = analyzeC(
        "void f(int x) {\n"
        "  while (x) {\n"
        "    if (x) { x = x + 1; } else { x = 0; }\n"
        "  }\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId body = res->hir.functionBody(fn);
    auto stmts = res->hir.children(body);
    ASSERT_EQ(stmts.size(), 1u);
    EXPECT_EQ(res->hir.kind(stmts[0]), HirKind::WhileStmt);
}

// D-CSUBSET-LOCAL-STATIC: a block-scope `static` lowers to a hidden module
// GLOBAL (static storage duration, C 6.2.4), NOT a function-body VarDecl (a
// stack slot). The name stays block-scoped; the STORAGE is global → the value
// persists across calls. RED-ON-DISABLE: revert the cst_to_hir staticStorage
// arm → `n` lowers to a body VarDecl → moduleDecls carries only the Function
// (zero Globals) and the body's first statement is a VarDecl, not the empty
// Block placeholder. This is the host-independent guard the runtime corpus
// (`local_static`) pairs with.
TEST(HirLoweringC, StaticLocalLowersToModuleGlobal) {
    SemanticModel model = analyzeC(
        "int f(void) { static int n = 0; n = n + 1; return n; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    std::size_t fns = 0, globals = 0;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) == HirKind::Function) ++fns;
        if (res->hir.kind(d) == HirKind::Global)   ++globals;
    }
    EXPECT_EQ(fns, 1u);
    EXPECT_EQ(globals, 1u)
        << "the static local must lower to ONE hidden module Global";
    // No stack VarDecl for `n` survives in the function body.
    HirNodeId fn = firstFunction(res->hir);
    for (HirNodeId s : res->hir.children(res->hir.functionBody(fn)))
        EXPECT_NE(res->hir.kind(s), HirKind::VarDecl)
            << "a static local must not leave a stack VarDecl in the body";
}

// Two SIBLING statics with the SAME source name in distinct blocks get DISTINCT
// module globals (distinct SymbolIds — no mangling needed; internal-linkage
// globals are intra-module by id). RED-ON-DISABLE: the revert collapses both to
// body VarDecls → zero module Globals.
TEST(HirLoweringC, SiblingStaticLocalsGetDistinctGlobals) {
    SemanticModel model = analyzeC(
        "int f(void) { { static int x = 1; x = x + 1; } "
        "{ static int x = 2; x = x + 1; } return 0; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    std::size_t globals = 0;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root()))
        if (res->hir.kind(d) == HirKind::Global) ++globals;
    EXPECT_EQ(globals, 2u)
        << "two sibling statics must mint two DISTINCT module globals";
}

// D-CSUBSET-BLOCK-SCOPE-EXTERN (TF arc C10, C89 6.7.1): a block-scope `extern`
// declaration — a FUNCTION prototype (`extern int ex_fn(int);`) AND an OBJECT
// reference (`extern int ex_obj;`) inside a body — RE-HOMES to the module decls
// as one ExternFunction + one ExternGlobal (the static-local / block-scope-
// prototype accumulator pattern) and lowers the STATEMENT to a no-op (an empty
// Block, the Skip precedent), so the function body carries NO stack VarDecl for
// either extern name. This is the host-independent structural guard the runtime
// corpus (`block_scope_extern`, cross-CU → exit 42) pairs with. RED-ON-DISABLE:
// drop the `k == "ExternDecl"` arm in cst_to_hir.cpp's lowerStmt → the externDecl
// statement hits the terminal default → "statement maps to unsupported HIR kind
// 'ExternDecl'" and res->ok goes false. (lowerToHir is CST→HIR only, so the
// ExternGlobal node is produced here regardless of the MIR-tier data-import
// state — the import resolves at the LK11 cross-CU merge, witnessed by the
// runtime example.)
TEST(HirLoweringC, BlockScopeExternRehomesToModuleDecls) {
    SemanticModel model = analyzeC(
        "int use(void) { extern int ex_fn(int); extern int ex_obj; "
        "return ex_fn(ex_obj); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    std::size_t fns = 0, externFns = 0, externGlobals = 0;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) == HirKind::Function)       ++fns;
        if (res->hir.kind(d) == HirKind::ExternFunction) ++externFns;
        if (res->hir.kind(d) == HirKind::ExternGlobal)   ++externGlobals;
    }
    EXPECT_EQ(fns, 1u);
    EXPECT_EQ(externFns, 1u)
        << "the block-scope extern FUNCTION prototype must re-home to ONE "
           "module ExternFunction";
    EXPECT_EQ(externGlobals, 1u)
        << "the block-scope extern OBJECT must re-home to ONE module ExternGlobal";
    // Both extern statements lower to no-ops: no stack VarDecl for either name.
    HirNodeId fn = firstFunction(res->hir);
    for (HirNodeId s : res->hir.children(res->hir.functionBody(fn)))
        EXPECT_NE(res->hir.kind(s), HirKind::VarDecl)
            << "a block-scope extern must not leave a stack VarDecl in the body";
}

// D-CSUBSET-TENTATIVE-DEFINITION-AFTER-EXTERN-DECL (TF-C61): a file-scope
// TENTATIVE definition (no initializer) that FOLLOWS an `extern` declaration of
// the same name must emit STORAGE — one module Global — NOT an ExternGlobal
// import. C 6.9.2p2: a prior `extern` declaration does not stop a later
// no-initializer declaration from being a tentative definition. Before the fix
// the extern "survived" the merge (it was the prior binding) and the TU emitted
// an ExternGlobal + no Global, so the symbol was UNDEFINED and the link failed —
// the extern-in-header-then-define-in-.c pattern that blocked the full-source
// sqlite build (`undefined reference to sqlite3BuiltinFunctions`).
//
// A SEMANTIC-tier test cannot catch this: extern+tentative merges with zero
// diagnostics and one surviving symbol EITHER WAY. Only the HIR emission
// distinguishes Global (storage) from ExternGlobal (import) — this tier is the
// one that was actually wrong.
//
// RED-ON-DISABLE: revert the defining-rank comparison in
// mergeOrCollideRedeclaration (keep the prior binding whenever the new decl is
// non-defining) → the extern survives → zero Globals + one ExternGlobal → this
// asserts the opposite of what a linkable program needs.
TEST(HirLoweringC, TentativeDefAfterExternEmitsStorageNotImport) {
    SemanticModel model = analyzeC(
        "extern int g;\n"
        "int g;\n"
        "int main(void){ g += 1; return g; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    std::size_t globals = 0, externGlobals = 0;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) == HirKind::Global)       ++globals;
        if (res->hir.kind(d) == HirKind::ExternGlobal) ++externGlobals;
    }
    EXPECT_EQ(globals, 1u)
        << "the tentative definition must emit ONE zero-init module Global — the "
           "prior extern does not suppress the definition (C 6.9.2p2)";
    EXPECT_EQ(externGlobals, 0u)
        << "no ExternGlobal import may survive — the tentative provides the "
           "storage locally; an import would leave the symbol undefined at link";
}

// PRESERVE — a plain `extern int g;` with NO local definition must STILL emit an
// ExternGlobal import (and zero Globals): the storage genuinely lives elsewhere.
// This guards the fix against over-reaching (turning every extern into storage).
// RED-ON-DISABLE: if the rank comparison wrongly promoted a bare extern, this
// flips to one Global / zero ExternGlobals.
TEST(HirLoweringC, PlainExternWithoutDefinitionStaysImport) {
    SemanticModel model = analyzeC(
        "extern int g;\n"
        "int use(void){ return g; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    std::size_t globals = 0, externGlobals = 0;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) == HirKind::Global)       ++globals;
        if (res->hir.kind(d) == HirKind::ExternGlobal) ++externGlobals;
    }
    EXPECT_EQ(globals, 0u)
        << "a bare extern with no local definition must NOT emit storage";
    EXPECT_EQ(externGlobals, 1u)
        << "a bare extern must stay an ExternGlobal import";
}

// D-CSUBSET-GNU-ATTRIBUTE (TF-C62): a GNU `__attribute__((...))` in the
// AFTER-DECLARATOR position, with a widened argument grammar (multi-arg /
// multi-clause / nested / number), must PARSE and lower cleanly — the attributes
// are parse-and-ignore hints, so the declaration lowers exactly as if they were
// absent. Every real C header puts its prototype attributes here (glibc
// `__attribute__((__nothrow__,__leaf__))`, Tcl `TCL_FORMAT_PRINTF(1,2)`).
// RED-ON-DISABLE: revert the after-declarator attrSpec slot in initDeclarator
// (c.lang.json) → the declaration fails P0009 and model.hasErrors().
// TF-C63 (D-CSUBSET-FORM-FEED-VTAB-WHITESPACE): C 6.4p1 counts VERTICAL TAB
// (0x0b) and FORM-FEED (0x0c) as white-space. Older Unix sources (real `tcl.h`)
// use a form-feed at every page boundary; without treating it as whitespace it
// was an illegal-char `P000E` that desynced the parse (tcl.h: 11 such errors).
// RED-ON-DISABLE: drop `isMainScanExtraSpace` from the tokenizer dispatch → the
// form-feed becomes an illegal char and this fails to analyze.
TEST(HirLoweringC, FormFeedAndVerticalTabAreWhitespace) {
    SemanticModel model = analyzeC(
        "int a = 20;\f\n"          // form-feed page break between declarations
        "\vint b = 22;\f\n"        // vertical tab + form-feed
        "int main(void){\v return a + b; }\n");   // vtab as inline whitespace
    ASSERT_FALSE(model.hasErrors())
        << "vertical tab (0x0b) and form-feed (0x0c) are C 6.4 whitespace — they "
           "must not be illegal characters that desync the parse";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

// TF-C73 UPDATED THIS TEST'S SUBJECT, deliberately. It used to include
// `int v __attribute__((aligned(4))) = 20;` and assert the whole program lowered
// CLEAN — which was only true because the after-declarator position was
// parse-and-IGNORE, i.e. the alignment was being silently dropped. `aligned` is
// an ABI FACT and now has a real sink, so "lowers with zero diagnostics" is no
// longer a meaningful thing to assert about it — the interesting question became
// WHAT ALIGNMENT WAS APPLIED, which this test cannot see. It therefore moved to
// the applied-value pin `AfterDeclaratorAlignedAppliesLikeTheLeadingPosition`
// below (and to the runtime witness `examples/c/gnu_aligned_attribute/`).
// The ABI-NEUTRAL hints this test exists to cover — multi-clause, nested-paren,
// number args — are unaffected and stay green, which is the point: the split is
// between "hint with no sink, safely ignorable" (here) and "ABI fact with a
// sink, assert the VALUE" (there). Do not merge them back: a clean-lowering
// assertion is exactly the witness that cannot detect a dropped alignment.
TEST(HirLoweringC, GnuAttributeAfterDeclaratorLowersClean) {
    SemanticModel model = analyzeC(
        "int add(int a, int b) __attribute__((__nothrow__, __leaf__));\n"
        "int add(int a, int b) { return a + b; }\n"
        "int deref(const int *p) __attribute__((__nonnull__((1))));\n"
        "int deref(const int *p) { return *p; }\n"
        // `format(printf, 1, 2)` on a REAL format function: position 1 is the
        // `const char *` and position 2 the first variadic. The obvious-looking
        // `int firstv(int n, ...)` is a HARD clang ERROR ("format argument not a
        // string type") — it shipped in the corpus example for a whole cycle
        // before TF-C73 caught it, so it is spelled correctly here on purpose.
        "int logfmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));\n"
        "int logfmt(const char *fmt, ...) { return fmt[0] - '%'; }\n"
        "int v __attribute__((__unused__)) = 20;\n"
        "int main(void){ int t=22; return add(v, t) + logfmt(\"%d\", 7); }\n");
    ASSERT_FALSE(model.hasErrors())
        << "after-declarator GNU attributes with multi-clause / nested / number "
           "args must parse and lower as parse-and-ignore hints";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "every attribute here is an ABI-neutral hint with no DSS sink — the "
           "now-scanned trailing position must ignore names AND arguments alike";
    // The `= 20` initializer must still be found (not shadowed by the
    // after-declarator attribute): `v` lowers to a Global WITH an initializer.
    std::size_t globalsWithInit = 0;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root()))
        if (res->hir.kind(d) == HirKind::Global) ++globalsWithInit;
    EXPECT_GE(globalsWithInit, 1u)
        << "`int v __attribute__((__unused__)) = 20;` must still lower to a "
           "Global — the attribute must not be mistaken for the initializer";
}

// (The positional-symmetry pin that explains WHY `aligned` left this test —
// `AfterDeclaratorAlignedAppliesLikeTheLeadingPosition` — lives with the rest of
// the TF-C73 battery further down, where the shared `globalAlignment` helper it
// asserts the applied value through is already in scope.)

TEST(HirLoweringC, ForLoop) {
    SemanticModel model = analyzeC(
        "void f() { for (int i = 0; i < 10; i = i + 1) {} }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId body = res->hir.functionBody(fn);
    HirNodeId forS = res->hir.children(body)[0];
    ASSERT_EQ(res->hir.kind(forS), HirKind::ForStmt);
    EXPECT_TRUE(res->hir.forInit(forS).has_value());
    EXPECT_TRUE(res->hir.loopCondition(forS).has_value());
    EXPECT_TRUE(res->hir.forUpdate(forS).has_value());
}

// c60 (Design I-A): the switch lowers to a discriminant + a flat body Block (its
// case/default markers are LabelStmts) + dispatch arms mapping each case value to
// a marker ordinal. Two dispatch arms here (case 1, default); the body Block holds
// the two case markers, each a LabelStmt.
TEST(HirLoweringC, SwitchFlattensToDispatchAndBody) {
    SemanticModel model = analyzeC(
        "void f(int x) { switch (x) { case 1: break; default: break; } }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId sw = res->hir.children(res->hir.functionBody(fn))[0];
    ASSERT_EQ(res->hir.kind(sw), HirKind::SwitchStmt);
    auto arms = res->hir.switchArms(sw);
    ASSERT_EQ(arms.size(), 2u);
    EXPECT_FALSE(res->hir.caseArmIsDefault(arms[0]));   // case 1
    EXPECT_TRUE(res->hir.caseArmIsDefault(arms[1]));     // default
    // The body Block holds the two case markers (LabelStmts) at its top level.
    HirNodeId const body = res->hir.switchBody(sw);
    ASSERT_EQ(res->hir.kind(body), HirKind::Block);
    auto const bodyStmts = res->hir.children(body);
    ASSERT_GE(bodyStmts.size(), 2u);
    EXPECT_EQ(res->hir.kind(bodyStmts[0]), HirKind::LabelStmt);   // case 1 marker
    // Each dispatch arm's ordinal names a LabelStmt marker that exists in the body.
    EXPECT_EQ(res->hir.caseArmLabelOrdinal(arms[0]),
              res->hir.labelOrdinal(bodyStmts[0]));
}

// D-CSUBSET-LABEL-BEFORE-CASE (c60, Design I-A) — a goto-label BEFORE a case
// (`foo: case 1: S`) parses as labelStmt(foo, caseStmt(case 1, S)) and lowers to a
// flat-body marker chain: the body's first statement is LabelStmt(foo, ...) whose
// inner is the case-1 marker LabelStmt (its ordinal = the dispatch arm's ordinal).
// `foo` stays a real LabelStmt (pre-scanned + goto-resolvable); the case dispatches
// to its own marker. (The label AFTER the colon — `case 1: foo: S` — nests the case
// marker OUTSIDE foo; both are valid C, only the relative nesting differs.)
TEST(HirLoweringC, LabelBeforeCaseLowersToMarkerChain) {
    SemanticModel model = analyzeC(
        "void f(int x){ switch(x){ foo: case 1: x=x+1; break; default: break; } }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId sw = res->hir.children(res->hir.functionBody(fn))[0];
    ASSERT_EQ(res->hir.kind(sw), HirKind::SwitchStmt);
    auto arms = res->hir.switchArms(sw);
    ASSERT_EQ(arms.size(), 2u);
    EXPECT_FALSE(res->hir.caseArmIsDefault(arms[0]));   // case 1
    // body[0] = LabelStmt(foo, LabelStmt(caseMarker, …)); the inner case marker's
    // ordinal equals the case-1 dispatch arm's ordinal.
    auto const bodyStmts = res->hir.children(res->hir.switchBody(sw));
    ASSERT_FALSE(bodyStmts.empty());
    ASSERT_EQ(res->hir.kind(bodyStmts[0]), HirKind::LabelStmt);   // the named `foo`
    HirNodeId const inner = res->hir.labelBody(bodyStmts[0]);
    ASSERT_EQ(res->hir.kind(inner), HirKind::LabelStmt);          // the case-1 marker
    EXPECT_EQ(res->hir.labelOrdinal(inner), res->hir.caseArmLabelOrdinal(arms[0]));
}

// c60 (Design I-A) — a BARE case (`case 1: x=…;`) lowers to a case-1 marker
// LabelStmt whose body is the real statement (the AssignStmt/ExprStmt), with NO
// stray nested caseStmt. The marker IS a LabelStmt (every case is a marker now),
// but its single body must reach the case's own statement directly.
TEST(HirLoweringC, BareCaseMarkerWrapsTheBodyStatement) {
    SemanticModel model = analyzeC(
        "void f(int x){ switch(x){ case 1: x=x+1; break; default: break; } }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId sw = res->hir.children(res->hir.functionBody(fn))[0];
    ASSERT_EQ(res->hir.kind(sw), HirKind::SwitchStmt);
    auto const bodyStmts = res->hir.children(res->hir.switchBody(sw));
    ASSERT_GE(bodyStmts.size(), 1u);
    ASSERT_EQ(res->hir.kind(bodyStmts[0]), HirKind::LabelStmt);   // case 1 marker
    // The marker's inner is the case body (an assignment), NOT another caseStmt/
    // label wrapper.
    HirNodeId const inner = res->hir.labelBody(bodyStmts[0]);
    EXPECT_NE(res->hir.kind(inner), HirKind::LabelStmt);
}

// D-CSUBSET-LABEL-BEFORE-CASE guard — a `caseStmt` that is not a direct switch-body
// item (here: outside any switch) fails loud S_CaseLabelNotInSwitch (C 6.8.1),
// never a stray arm-less case. Red-on-disable: drop the lowerStmt CaseStmt guard.
TEST(HirLoweringC, CaseLabelOutsideSwitchFailsLoud) {
    SemanticModel model = analyzeC("int f(void){ case 1: return 0; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GE(countCode(r, DiagnosticCode::S_CaseLabelNotInSwitch), 1u);
}

// c60 (Design I-A) — the multi-label ADJACENT-case chain + a label before
// `default`. `foo: case 1: case 2: S` parses as labelStmt(foo, caseStmt(1,
// caseStmt(2, S))) → body[0] = LabelStmt(foo, LabelStmt(case1, LabelStmt(case2,
// S))); `bar: default: S2` → LabelStmt(bar, LabelStmt(default, S2)). Three dispatch
// arms (case 1, case 2, default), each with a distinct marker ordinal.
TEST(HirLoweringC, LabelBeforeAdjacentCasesAndDefaultMarkerChain) {
    SemanticModel model = analyzeC(
        "void f(int x){ switch(x){ foo: case 1: case 2: x=x+1; break; "
        "bar: default: x=x+9; break; } }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId sw = res->hir.children(res->hir.functionBody(fn))[0];
    ASSERT_EQ(res->hir.kind(sw), HirKind::SwitchStmt);
    auto arms = res->hir.switchArms(sw);
    ASSERT_EQ(arms.size(), 3u);                          // case 1, case 2, default
    EXPECT_FALSE(res->hir.caseArmIsDefault(arms[0]));    // case 1
    EXPECT_FALSE(res->hir.caseArmIsDefault(arms[1]));    // case 2
    EXPECT_TRUE(res->hir.caseArmIsDefault(arms[2]));     // default
    // Distinct ordinals for the three markers.
    EXPECT_NE(res->hir.caseArmLabelOrdinal(arms[0]), res->hir.caseArmLabelOrdinal(arms[1]));
    EXPECT_NE(res->hir.caseArmLabelOrdinal(arms[1]), res->hir.caseArmLabelOrdinal(arms[2]));
    // body[0] = LabelStmt(foo, LabelStmt(case1, LabelStmt(case2, …))).
    auto const bodyStmts = res->hir.children(res->hir.switchBody(sw));
    ASSERT_FALSE(bodyStmts.empty());
    ASSERT_EQ(res->hir.kind(bodyStmts[0]), HirKind::LabelStmt);       // foo
    HirNodeId const c1 = res->hir.labelBody(bodyStmts[0]);
    ASSERT_EQ(res->hir.kind(c1), HirKind::LabelStmt);                 // case 1 marker
    EXPECT_EQ(res->hir.labelOrdinal(c1), res->hir.caseArmLabelOrdinal(arms[0]));
    HirNodeId const c2 = res->hir.labelBody(c1);
    ASSERT_EQ(res->hir.kind(c2), HirKind::LabelStmt);                 // case 2 marker
    EXPECT_EQ(res->hir.labelOrdinal(c2), res->hir.caseArmLabelOrdinal(arms[1]));
}

// D-CSUBSET-LABEL-BUDGET-CLIFF (p19 Cluster G c31) — the `commitAfterPrefix`
// CUT lets `declOrExprStmt`'s `labelStmt` probe COMMIT after its 2-token fixed
// prefix (`Identifier Colon`) is consumed, so the (arbitrarily large) labeled
// `statement` then parses NON-speculatively (off the lookahead*16 = 4096-token
// probe budget). This pin builds a label before a statement whose token count
// is FAR over 4096; with the cut it parses clean, lowers, and runs. RED-ON-
// DISABLE: revert `"commitAfterPrefix": true` on labelStmt (or the parser cut)
// and the labelStmt probe exhausts its budget, rolls back, falls through to
// exprStmt, and emits P0001 ("got ':'") — `model.hasErrors()` then trips the
// ASSERT below. The body (`i = i + vN`, fold-resistant on a parameter) also
// makes the result observable so the labeled block is not DCE'd to nothing.
TEST(HirLoweringC, LabelBeforeOversizeStatementParsesPastProbeBudget) {
    // ~500 statements inside the labeled block ⇒ ~5500 tokens, comfortably
    // over the 4096-token speculative-probe budget (lookahead 256 * 16).
    std::string src = "int f(int i){\n  L: {\n";
    for (int n = 0; n < 500; ++n) {
        src += "    int v" + std::to_string(n) + " = " + std::to_string(n)
             + "; i = i + v" + std::to_string(n) + ";\n";
    }
    src += "  }\n  return i;\n}\n";
    SemanticModel model = analyzeC(std::move(src));
    // The load-bearing assertion: a clean front end. On revert this is the
    // P0001 'got :' from the budget rollback.
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    ASSERT_EQ(res->hir.kind(fn), HirKind::Function);
    auto body = res->hir.children(res->hir.functionBody(fn));
    ASSERT_EQ(body.size(), 2u);                           // labeled block + return
    EXPECT_EQ(res->hir.kind(body[0]), HirKind::LabelStmt);
    EXPECT_EQ(res->hir.kind(res->hir.labelBody(body[0])), HirKind::Block);
}

// ★★ BRACELESS-BODY CORRECTNESS — the silent-miscompile guard for the cut.
// `labelStmt` STAYS `Identifier Colon statement`, so a label in a braceless
// control-flow body keeps its labeled statement AS that body: in
// `if(x) L: g=42;` the `L: g=42` IS the if's then-branch, NOT a sibling that
// runs unconditionally. Were labelStmt ever flattened to a 2-token statement
// (label + a SEPARATE following statement), `g=42` would detach from the `if`
// and execute even when `x==0` — a C-semantics miscompile that runs green on
// any test that only checks the x!=0 path. This pin pins the STRUCTURE: the
// if's then-branch is the LabelStmt, and the assignment hangs UNDER it. The
// `label_before_switch_goto` runtime corpus is the executable companion; the
// gate's `f(0)→5` (the assignment is skipped) is the same property at runtime.
TEST(HirLoweringC, LabelAsBracelessIfBodyStaysInsideTheIf) {
    SemanticModel model = analyzeC(
        "int g; int f(int x){ g = 5; if(x) L: g = 42; return g; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    // f is the SECOND module decl (g is the first — a global var).
    HirNodeId fn{};
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) == HirKind::Function) { fn = d; break; }
    }
    ASSERT_TRUE(fn.valid());
    auto body = res->hir.children(res->hir.functionBody(fn));
    // EXACTLY three statements: `g=5`, the `if`, `return g`. A flattened label
    // would make `g=42` a FOURTH sibling here (and the if's then-branch empty).
    ASSERT_EQ(body.size(), 3u);
    EXPECT_EQ(res->hir.kind(body[0]), HirKind::AssignStmt);   // g = 5
    ASSERT_EQ(res->hir.kind(body[1]), HirKind::IfStmt);       // if (x) ...
    EXPECT_EQ(res->hir.kind(body[2]), HirKind::ReturnStmt);   // return g
    // The if's then-branch IS the label, and `g=42` hangs under it. (The
    // assignment lowers to AssignStmt — the load-bearing structural fact is
    // that it nests UNDER the LabelStmt UNDER the if, not that it is a
    // sibling of the if running unconditionally.)
    HirNodeId thenBranch = res->hir.ifThen(body[1]);
    ASSERT_EQ(res->hir.kind(thenBranch), HirKind::LabelStmt);
    EXPECT_EQ(res->hir.kind(res->hir.labelBody(thenBranch)), HirKind::AssignStmt);
}

TEST(HirLoweringC, CallAndTypedef) {
    SemanticModel model = analyzeC(
        "typedef int myint;\n"
        "int g(int a) { return a; }\n"
        "int f(int x) { return g(x); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 3u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::TypeDecl);     // typedef
    EXPECT_EQ(res->hir.kind(decls[1]), HirKind::Function);     // g
    EXPECT_EQ(res->hir.kind(decls[2]), HirKind::Function);     // f
    // f's body returns a Call.
    HirNodeId fbody = res->hir.functionBody(decls[2]);
    HirNodeId ret = res->hir.children(fbody)[0];
    EXPECT_EQ(res->hir.kind(*res->hir.returnValue(ret)), HirKind::Call);
}

// D-LANG-POINTER-VOID-CONVERT (audit-fold G4, step 13.2, 2026-06-02):
// pins that `cst_to_hir.cpp`'s coerce() arm ACTUALLY emits the
// synthetic `Cast(Ptr<T>→Ptr<Void>)` HIR node when c's
// `pointerConversions.implicitToVoidPtr` admits a T*→void* call
// arg. Pre-G4 only the semantic analyzer's S_TypeMismatch absence
// was pinned — if the lowering arm silently failed to emit the
// Cast (e.g., the `admit` branch returned false unexpectedly), MIR
// would see Ptr<I8> where Ptr<Void> was expected and silently
// type-skew at the HIR/MIR boundary.
TEST(HirLoweringC, CoerceEmitsCastForCharPtrToVoidPtrArg) {
    SemanticModel model = analyzeC(
        "extern int handler(void* p);\n"
        "int main() {\n"
        "    char* s;\n"
        "    return handler(s);\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto const& ti = model.lattice().interner();
    // Navigate to the `handler(s)` call's arg. Decls: [extern handler, main].
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_GE(decls.size(), 2u);
    HirNodeId const mainBody = res->hir.functionBody(decls.back());
    auto const stmts = res->hir.children(mainBody);
    ASSERT_GE(stmts.size(), 1u);
    // Last statement is `return handler(s);` — find its Call.
    HirNodeId const ret = stmts.back();
    HirNodeId const call = *res->hir.returnValue(ret);
    ASSERT_EQ(res->hir.kind(call), HirKind::Call);
    // Call children: [callee, arg0, arg1, ...]
    auto const callKids = res->hir.children(call);
    ASSERT_EQ(callKids.size(), 2u);
    HirNodeId const arg0 = callKids[1];
    // The arg MUST be a synthetic Cast — not a bare Ref/Literal —
    // because the void-pointer conversion is a tracked materialization.
    ASSERT_EQ(res->hir.kind(arg0), HirKind::Cast)
        << "char* → void* arg must lower to an explicit Cast HIR node, "
           "not a bare Ref — otherwise MIR sees the source type and "
           "downstream Bitcast lowering loses the type-skew evidence";
    // Cast's result type IS Ptr<Void>.
    TypeId const argTy = res->hir.typeId(arg0);
    ASSERT_EQ(ti.kind(argTy), TypeKind::Ptr);
    auto const elem = ti.operands(argTy);
    ASSERT_FALSE(elem.empty());
    EXPECT_EQ(ti.kind(elem[0]), TypeKind::Void)
        << "Cast target must be Ptr<Void>, not Ptr<I8>";
}

// D-LANG-POINTER-VOID-CONVERT (audit-fold G4, reverse direction):
// `void* → char*` via call arg also lowers to an explicit Cast
// whose result type is Ptr<Char>. Pins the implicitFromVoidPtr arm.
TEST(HirLoweringC, CoerceEmitsCastForVoidPtrToCharPtrArg) {
    SemanticModel model = analyzeC(
        "extern int handler(char* s);\n"
        "extern void* alloc(int n);\n"
        "int main() {\n"
        "    return handler(alloc(16));\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto const& ti = model.lattice().interner();
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_GE(decls.size(), 3u);
    HirNodeId const mainBody = res->hir.functionBody(decls.back());
    auto const stmts = res->hir.children(mainBody);
    HirNodeId const ret = stmts.back();
    HirNodeId const outerCall = *res->hir.returnValue(ret);
    ASSERT_EQ(res->hir.kind(outerCall), HirKind::Call);
    auto const outerKids = res->hir.children(outerCall);
    ASSERT_EQ(outerKids.size(), 2u);
    HirNodeId const arg0 = outerKids[1];
    ASSERT_EQ(res->hir.kind(arg0), HirKind::Cast)
        << "void* → char* via call arg must materialize as Cast";
    TypeId const argTy = res->hir.typeId(arg0);
    ASSERT_EQ(ti.kind(argTy), TypeKind::Ptr);
    auto const elem = ti.operands(argTy);
    ASSERT_FALSE(elem.empty());
    EXPECT_EQ(ti.kind(elem[0]), TypeKind::Char)
        << "Cast target must be Ptr<Char>, not Ptr<Void>";
}

// D-LANG-NULL-POINTER-CONSTANT (step 13.3, 2026-06-02): pin that
// `cst_to_hir.cpp`'s coerce() arm materializes literal 0 → Ptr<T> as
// an explicit `Cast(IntLiteral(0), Ptr<T>)` HIR node. MIR's mapCast
// routes IntToPtr (literal-0 → pointer-width zero in the dest
// register); without the explicit Cast, MIR would see I32 where
// Ptr was expected → silent type-skew at the HIR/MIR boundary.
TEST(HirLoweringC, NullPointerConstantEmitsCastInCallArg) {
    SemanticModel model = analyzeC(
        "extern void f(void* p);\n"
        "int main() { f(0); return 0; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto const& ti = model.lattice().interner();
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_GE(decls.size(), 2u);
    HirNodeId const mainBody = res->hir.functionBody(decls.back());
    auto const stmts = res->hir.children(mainBody);
    ASSERT_GE(stmts.size(), 1u);
    // First statement is the `f(0)` ExprStmt — find its Call.
    HirNodeId const exprStmt = stmts[0];
    HirNodeId const call = res->hir.exprStmtExpr(exprStmt);
    ASSERT_EQ(res->hir.kind(call), HirKind::Call);
    auto const callKids = res->hir.children(call);
    ASSERT_EQ(callKids.size(), 2u);
    HirNodeId const arg0 = callKids[1];
    ASSERT_EQ(res->hir.kind(arg0), HirKind::Cast)
        << "literal 0 → void* must materialize as an explicit Cast "
           "HIR node; otherwise MIR sees I32 where Ptr is expected";
    TypeId const argTy = res->hir.typeId(arg0);
    ASSERT_EQ(ti.kind(argTy), TypeKind::Ptr);
    auto const elem = ti.operands(argTy);
    ASSERT_FALSE(elem.empty());
    EXPECT_EQ(ti.kind(elem[0]), TypeKind::Void);
}

// D-CSUBSET-CAST-ARRAY-DECAY (FC3.5 sweep-c3): the explicit cast of a
// string literal lowers THROUGH the synthetic array-to-pointer decay
// (C 6.3.2.1p3): `(long)"xy"` is Cast(I64 ← Cast(Ptr<Char> ←
// Array<Char>)) — the inner decay is the SAME synthetic Cast the
// implicit path emits (mapCast materializes the rodata global +
// GlobalAddr), the outer Cast is the programmer's PtrToInt. Without
// the decay the MIR mapCast would see Array directly (no arm — fail).
TEST(HirLoweringC, ExplicitCastOfStringLiteralLowersViaDecay) {
    SemanticModel model = analyzeC(
        "int main() { return (int)(long)\"xy\"; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto const& ti = model.lattice().interner();
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_GE(decls.size(), 1u);
    HirNodeId const mainBody = res->hir.functionBody(decls.back());
    auto const stmts = res->hir.children(mainBody);
    ASSERT_GE(stmts.size(), 1u);
    HirNodeId const ret = stmts.back();
    HirNodeId const outer = *res->hir.returnValue(ret);
    // (int) ← (long) ← decay(Ptr<Char>) ← "xy"
    ASSERT_EQ(res->hir.kind(outer), HirKind::Cast);
    EXPECT_EQ(ti.kind(res->hir.typeId(outer)), TypeKind::I32);
    auto const outerKids = res->hir.children(outer);
    ASSERT_EQ(outerKids.size(), 1u);
    HirNodeId const longCast = outerKids[0];
    ASSERT_EQ(res->hir.kind(longCast), HirKind::Cast);
    EXPECT_EQ(ti.kind(res->hir.typeId(longCast)), TypeKind::I64);
    auto const longKids = res->hir.children(longCast);
    ASSERT_EQ(longKids.size(), 1u);
    HirNodeId const decay = longKids[0];
    ASSERT_EQ(res->hir.kind(decay), HirKind::Cast)
        << "the cast operand must pass through the synthetic "
           "array-to-pointer decay Cast (C 6.3.2.1p3) — mapCast has "
           "no Array→int arm";
    TypeId const decayTy = res->hir.typeId(decay);
    ASSERT_EQ(ti.kind(decayTy), TypeKind::Ptr)
        << "the decay result must be Ptr<Char>";
    auto const decayElem = ti.operands(decayTy);
    ASSERT_FALSE(decayElem.empty());
    EXPECT_EQ(ti.kind(decayElem[0]), TypeKind::Char);
}

// D-CSUBSET-COMPOUND-LITERAL-TYPEDEF (FC3.5 sweep-c3): a typedef'd
// STRUCT compound literal lowers through HIR cleanly — the semantic
// stamping (semantics.compoundLiterals) resolved `MyP` so
// `resolveStampedTypeBelow` finds the struct type and `lowerBraceInit`
// builds the aggregate. HONEST TIER NOTE: this pins the
// parse+semantic+HIR tiers; struct VALUES do not reach LIR codegen yet
// (the aggregate MIR ops are a pre-existing '<deferred>' lowering
// gap), so no runtime corpus arm exists for compound literals.
TEST(HirLoweringC, TypedefStructCompoundLiteralLowersTyped) {
    SemanticModel model = analyzeC(
        "struct P { int x; int y; };\n"
        "typedef struct P MyP;\n"
        "int main() { struct P p = (MyP){40, 2}; return p.x; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok)
        << "typedef'd compound literal must lower through HIR: "
        << (r.all().empty() ? "" : r.all()[0].actual);
}

// FC17.5 (D-CSUBSET-EMPTY-INITIALIZER, C23 6.7.10): SCALAR compound
// literals — `(int){42}`, valid C 6.5.2.5p9 — now lower through the HIR
// scalar brace-init arm (the FLIP of the pre-FC17.5
// `ScalarCompoundLiteralStaysAggregateOnlyFailLoud` pin, which
// anticipated exactly this lift). The single-expression form is
// byte-identical to a plain `= 42` init after the arm's coerce.
TEST(HirLoweringC, ScalarCompoundLiteralLowersViaScalarBraceInit) {
    SemanticModel model = analyzeC(
        "int main() { int x = (int){42}; return x; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "the semantic tier must admit + type the scalar compound "
           "literal (the stamp resolves)";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok)
        << "the FC17.5 scalar brace-init arm must lower `(int){42}` "
           "cleanly: "
        << (r.all().empty() ? "" : r.all()[0].actual);
}

// FC17.5 (D-CSUBSET-EMPTY-INITIALIZER, C23 6.7.10p11): the EMPTY
// initializer `{}` zero-initializes a scalar / a pointer, and the
// single-expression form `{42}` initializes with the expression —
// every route funnels through the ONE lowerBraceInit chokepoint.
TEST(HirLoweringC, ScalarEmptyAndSingleBraceInitLower) {
    SemanticModel model = analyzeC(
        "int main() {\n"
        "    int z = {};\n"
        "    int v = {42};\n"
        "    int *p = {};\n"
        "    return z + v + (p == 0 ? 0 : 1);\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

// FC17.5 (S_InvalidScalarInitializer 0xE03F): the three malformed scalar
// brace shapes stay LOUD — excess elements (`{1,2}`), a designator on a
// scalar (`{.x=1}`), and the audit-N2 nested brace list (`{{42}}` — C23
// 6.7.10p12 requires a SINGLE expression). Each is a distinct arm of the
// scalar lowering's reject path; a silent guess would ship wrong bytes.
TEST(HirLoweringC, ScalarBraceInitMalformedShapesFailLoud) {
    struct Arm { char const* src; char const* what; };
    Arm const arms[] = {
        {"int main() { int v = {1, 2}; return v; }\n",   "excess elements"},
        {"int main() { int v = {.x = 1}; return v; }\n", "designator on scalar"},
        {"int main() { int v = {{42}}; return v; }\n",   "nested brace list (N2)"},
    };
    for (auto const& arm : arms) {
        SemanticModel model = analyzeC(arm.src);
        ASSERT_FALSE(model.hasErrors())
            << arm.what << ": the semantic tier admits the parse (the "
                            "constraint is enforced at HIR lowering)";
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_FALSE(res->ok)
            << arm.what << " must fail loud at the scalar brace-init arm";
        bool sawCode = false;
        for (auto const& d : r.all()) {
            if (d.code == DiagnosticCode::S_InvalidScalarInitializer)
                sawCode = true;
        }
        EXPECT_TRUE(sawCode)
            << arm.what << " must report S_InvalidScalarInitializer (0xE03F)";
    }
}

// FC17.5 F4 (the CLOSED allowlist): `(void){}` stays LOUD — Void is not
// an allowlisted scalar brace-init target (admitting it would mint a
// Void-typed literal and corrupt the type system). The aggregate gate's
// fail-loud reject is the backstop for every non-allowlisted kind.
TEST(HirLoweringC, VoidCompoundLiteralStaysFailLoud) {
    SemanticModel model = analyzeC(
        "int main() { (void){}; return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "the semantic tier stamps the void compound literal; the HIR "
           "gate is the enforcement point";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "`(void){}` must stay fail-loud (F4 closed allowlist)";
}

// FC17.5 (D-CSUBSET-FUNC-PREDEFINED-IDENTIFIER, C99 6.4.2.2): a read of
// `__func__` FOLDS to a string-literal-shaped constant and every string
// consumer (decay to `const char*`, indexing, address-of) rides the
// existing paths — the whole program lowers cleanly through HIR.
TEST(HirLoweringC, FuncNameReadsLowerThroughStringLiteralPaths) {
    SemanticModel model = analyzeC(
        "int helper(void) { return __func__[0] == 'h' ? 1 : 0; }\n"
        "int main() {\n"
        "    const char *fn = __func__;\n"
        "    int a = fn[0] == 'm' ? 1 : 0;\n"
        "    int b = (&__func__ != 0) ? 1 : 0;\n"
        "    int c = (__func__ == __func__) ? 1 : 0;\n"
        "    int d = (int)sizeof __func__;\n"
        "    return a + b + c + d + helper();\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

// FC17.5 F1 (S_PredefinedIdentifierNotAddressable 0xE040): `++__func__`
// reaches the HIR inc/dec classifier (SE4's const check does not model
// inc/dec — the pre-existing D-CSUBSET-INCDEC-CONST-LVALUE class), where
// the simpleLvalue chokepoint now rejects the predefined identifier with
// a REAL diagnostic instead of the engine-level "no storage slot" MIR
// failure it would otherwise dead-end at. Covers `--__func__` and the
// postfix forms by construction (all three ++/-- sites share the
// classifier, and the classifier's simple-lvalue probe IS the guard).
TEST(HirLoweringC, FuncNameIncDecFailsLoudWithRealDiagnostic) {
    SemanticModel model = analyzeC(
        "int main() { ++__func__; return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "inc/dec const-ness is not modelled at semantic — the HIR "
           "guard is the enforcement point";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "++__func__ must fail loud";
    bool sawCode = false;
    for (auto const& d : r.all()) {
        if (d.code == DiagnosticCode::S_PredefinedIdentifierNotAddressable)
            sawCode = true;
    }
    EXPECT_TRUE(sawCode)
        << "++__func__ must report S_PredefinedIdentifierNotAddressable "
           "(0xE040), not an engine-level unsupported-lowering error";
}

// D-CSUBSET-CAST-VOID-DISCARD (FC3.5 sweep-c3): `(void)f()` lowers as
// evaluate-operand-discard — the expression statement's node IS the
// Call itself, with NO Cast wrapping it (mapCast has no void arm by
// design; the discard is a statement effect, not a conversion). The
// operand's presence in the lowered tree is the evaluation guarantee.
TEST(HirLoweringC, VoidDiscardCastLowersOperandWithoutCastNode) {
    SemanticModel model = analyzeC(
        "extern int f();\n"
        "int main() { (void)f(); return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_GE(decls.size(), 2u);
    HirNodeId const mainBody = res->hir.functionBody(decls.back());
    auto const stmts = res->hir.children(mainBody);
    ASSERT_GE(stmts.size(), 2u);
    HirNodeId const exprStmt = stmts[0];
    HirNodeId const inner = res->hir.exprStmtExpr(exprStmt);
    ASSERT_EQ(res->hir.kind(inner), HirKind::Call)
        << "(void)f() must lower to the bare Call (operand evaluated "
           "for effects) — NO Cast node may wrap it (mapCast has no "
           "void arm; a Cast here would fail-loud downstream)";
}

// D5.1: a `struct Foo { int x; int y; };` declaration lowers to a HIR
// `TypeDecl` whose `typeId` is the composed `structType("Foo", {I32, I32})`.
TEST(HirLoweringC, StructDeclarationLowersToTypeDecl) {
    SemanticModel model = analyzeC(
        "struct Point { int x; int y; };\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::TypeDecl);

    // The TypeDecl carries the composed struct type.
    TypeId const t = res->hir.typeDeclType(decls[0]);
    ASSERT_TRUE(t.valid());
    auto const& interner = model.lattice().interner();
    EXPECT_EQ(interner.kind(t), TypeKind::Struct);
    EXPECT_EQ(interner.name(t), "Point");
    // Field ordering: x (I32), y (I32) — declaration order.
    auto fields = interner.operands(t);
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(interner.kind(fields[0]), TypeKind::I32);
    EXPECT_EQ(interner.kind(fields[1]), TypeKind::I32);
}

// C 6.7p2 — a top-level declaration with NEITHER a named declarator NOR a tag
// (`int ;`) declares nothing. This became grammar-parseable when the init-
// declarator-list was made OPTIONAL (to admit the bare `struct P {…};` form,
// D-CSUBSET-STRUCT-BODY-VARDECL-POSITION). The HIR lowering must FAIL LOUD with
// S_DeclarationDeclaresNothing — and must NOT crash: the prior code drove
// `findCompositeSpecifierIn`'s `tree.rule()` over a leaf token (the `int`),
// tripping the `Internal`-node assertion. RED-ON-DISABLE on BOTH halves: revert
// the `findCompositeSpecifierIn` Internal guard → this test CRASHES; drop the
// `emitH(S_DeclarationDeclaresNothing)` → `res->ok` stays true and the count is 0.
// (The sibling `StructDeclarationLowersToTypeDecl` above proves the diagnostic
// does NOT false-fire on a real bare tag-declaring def.)
TEST(HirLoweringC, TopLevelDeclaresNothingFailsLoudNoCrash) {
    SemanticModel model = analyzeC("int ;\nint main(void) { return 0; }\n");
    // The constraint violation is HIR-tier: parse + semantic accept `int ;`.
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "`int ;` declares nothing — lowering must fail loud, not accept";
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 1u)
        << "exactly one S_DeclarationDeclaresNothing for the empty `int ;` decl";
}

// C23 6.7.2.5 (D-CSUBSET-TYPEOF): a bare `typeof(x);` — a typeof type-specifier
// with NO declarator — declares nothing, exactly like `int ;`. The typeof head is
// a type-specifier (not a struct/union/enum composite), so the "declares nothing"
// path fires at HIR lowering (S_DeclarationDeclaresNothing) and must NOT crash on
// the typeof subtree. Semantic + parse accept it (x is a declared global); the
// constraint is HIR-tier.
TEST(HirLoweringC, BareTypeofDeclaresNothingFailsLoud) {
    SemanticModel model = analyzeC(
        "int x;\ntypeof(x);\nint main(void) { return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "`typeof(x);` declares nothing — lowering must fail loud, not accept";
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 1u)
        << "exactly one S_DeclarationDeclaresNothing for the bare `typeof(x);` decl";
}

// c25 D-CSUBSET-UNIFIED-COMPOSITE-SPECIFIER: a body-PRESENT specifier WITH a
// declarator (`struct S { int x; } v;`) is a definition-introducing global — it
// lowers cleanly (the `compositeSpecifierIsDefinition` gate admits it because its
// body child is present). The body-ABSENT counterpart (`struct S;`) is now a
// FORWARD DECLARATION (c35 D-CSUBSET-FORWARD-STRUCT-DECLARATION): it mints an
// opaque tag at the semantic tier and lowers to NOTHING (no TypeDecl, no fail) —
// pinned in TopLevelForwardStructDeclLowersToNothing below. RED-on-disable: if
// the body-present gate were inverted, THIS would spuriously fail-loud.
TEST(HirLoweringC, StructDefinitionWithObjectLowersClean) {
    SemanticModel model = analyzeC(
        "struct S { int x; } v;\nint main(void) { return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 0u)
        << "`struct S { int x; } v;` declares an object — must NOT fail loud";
}

// c35 D-CSUBSET-FORWARD-STRUCT-DECLARATION: a bare top-level FORWARD declaration
// (`struct S;` — a body-ABSENT NAMED composite specifier) is the opaque-tag
// declaration. The semantic tier minted the incomplete tag; the HIR lowering must
// emit NOTHING and must NOT fail loud (it is NOT a declares-nothing constraint
// violation). RED-on-disable: drop the `findForwardCompositeSpecifierIn` no-op arm
// and the bare forward decl re-emits S_DeclarationDeclaresNothing (res->ok false).
// Contrast TopLevelDeclaresNothingFailsLoudNoCrash (`int ;`, NO tag → still loud).
TEST(HirLoweringC, TopLevelForwardStructDeclLowersToNothing) {
    SemanticModel model = analyzeC(
        "struct S;\nint main(void) { return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 0u)
        << "`struct S;` is a forward declaration of an opaque tag — must NOT fail loud";
}

// c35: the LOCAL twin — a bare `struct S;` as a block STATEMENT is a (block-scoped)
// forward declaration; lowers to nothing, no fail-loud. Contrast
// LocalDeclaresNothingFailsLoud (`int;`, NO tag → still loud).
TEST(HirLoweringC, LocalForwardStructDeclLowersToNothing) {
    SemanticModel model = analyzeC(
        "int main(void){ struct S; return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 0u)
        << "a block-scoped `struct S;` forward declaration must NOT fail loud";
}

// D5.1: a struct used as a pointer-typed parameter + member access via `->`
// resolves the field SymbolId AND propagates the field's type to the
// member-access node. Pins the SEMANTIC layer (Pass 1.5 struct composition +
// Pass 2 field-symbol binding + type propagation).
TEST(HirLoweringC, StructFieldAccessResolvesSemantically) {
    SemanticModel model = analyzeC(
        "struct Point { int x; int y; };\n"
        "int read_x(struct Point *p) { return p->x; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);

    // Walk the CU's tree to find the `x` identifier inside `p->x`. The CST has
    // one Identifier token per `x` reference; field `x`'s declaration is the
    // FIRST one (under `struct Point { int x; ... }`) and the USE is the LAST
    // one (under `return p->x;`). The semantic model's reverse use-index makes
    // this robust: every use of the field symbol is recorded.
    auto const& cu = model.unit();
    SymbolId xField = InvalidSymbol;
    for (auto const& rec : model.symbols()) {
        if (rec.name == "x" && rec.kind == DeclarationKind::Variable) {
            xField = SymbolId{static_cast<std::uint32_t>(&rec - model.symbols().data())};
            break;
        }
    }
    ASSERT_TRUE(xField.valid()) << "field symbol 'x' not minted";

    // Pass 1 stamps the field's ordinal index on the symbol.
    auto const* xRec = model.recordFor(xField);
    ASSERT_NE(xRec, nullptr);
    EXPECT_EQ(xRec->fieldIndex, 0u);  // x is the first field

    // Pass 2 should have recorded the field use on the `p->x` access. The
    // reverse-use index gives us every use-site node for the field symbol;
    // for `p->x` inside `read_x`, that's exactly one use.
    auto uses = model.usesOf(xField);
    ASSERT_EQ(uses.size(), 1u) << "field 'x' should have exactly one use site";

    // The use node should carry the field's type (I32) via Pass 2's
    // propagation. The MEMBER-ACCESS node (the parent postfixExpr) should
    // ALSO carry that type so chained access `p->x.y` would resolve the
    // next layer naturally.
    NodeId useNode = uses[0];
    TypeId useType = model.typeAt(useNode);
    auto const& interner = model.lattice().interner();
    ASSERT_TRUE(useType.valid());
    EXPECT_EQ(interner.kind(useType), TypeKind::I32);

    // Find the parent postfixExpr node and verify its type matches.
    Tree const& tree = cu.trees()[0];
    NodeId parent = tree.parent(useNode);
    // memberFollower wraps the Identifier; postfixExpr wraps memberFollower.
    while (parent.valid() && tree.kind(parent) == NodeKind::Internal
           && tree.rule(parent).v != model.unit().schema().rules().find("postfixExpr").v) {
        parent = tree.parent(parent);
    }
    ASSERT_TRUE(parent.valid()) << "no postfixExpr ancestor for field use";
    TypeId accessType = model.typeAt(parent);
    ASSERT_TRUE(accessType.valid()) << "member-access node has no type";
    EXPECT_EQ(interner.kind(accessType), TypeKind::I32);
}

// D5.1 cycle 4: the `MemberAccess` HIR-lowering branch in lowerPostfix. A `.`
// access lowers to a plain `HirKind::MemberAccess` with the field's payload =
// fieldIndex. An arrow `->` access is desugared at HIR level to
// `MemberAccess(Deref(p), idx)` -- same HirKind handles both forms; MIR sees
// uniform GEP-after-load patterns.
TEST(HirLoweringC, MemberAccessLowersToHirMemberAccess) {
    SemanticModel model = analyzeC(
        "struct Point { int x; int y; };\n"
        "int get_y(struct Point *p) { return p->y; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 2u);   // struct Point, function get_y
    HirNodeId const fn = decls[1];
    ASSERT_EQ(res->hir.kind(fn), HirKind::Function);
    HirNodeId const body = res->hir.functionBody(fn);
    // body = Block of [ReturnStmt(MemberAccess(Deref(Ref(p)), 1))]
    auto stmts = res->hir.children(body);
    ASSERT_EQ(stmts.size(), 1u);
    HirNodeId const ret = stmts[0];
    ASSERT_EQ(res->hir.kind(ret), HirKind::ReturnStmt);
    auto const retVal = res->hir.returnValue(ret);
    ASSERT_TRUE(retVal.has_value());
    HirNodeId const access = *retVal;
    ASSERT_EQ(res->hir.kind(access), HirKind::MemberAccess);
    // Field 'y' is the SECOND field (index 1) of Point.
    EXPECT_EQ(res->hir.payload(access), 1u);
    // The MemberAccess's result type is the field type (I32).
    auto const& interner = model.lattice().interner();
    EXPECT_EQ(interner.kind(res->hir.typeId(access)), TypeKind::I32);
    // Arrow form: the access's single child is a Deref of the original LHS.
    auto kids = res->hir.children(access);
    ASSERT_EQ(kids.size(), 1u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::Deref);
    // The Deref's result type is the pointee (struct Point).
    EXPECT_EQ(interner.kind(res->hir.typeId(kids[0])), TypeKind::Struct);
}

// D5.2 cycle 1: adding `Identifier` to `typeBase` lets a typedef-name work
// bare in type position at top level. The engine's `resolveTypeNode` resolves
// Identifier-in-type-position via the SE5 alias path (an ORDINARY Type-kind
// symbol → its `.type`); this cycle's contribution is the schema change that
// lets the parser accept the form. Block-scope alias (`{ Foo x; }`) is
// intentionally deferred — it collides with `exprStmt` at the statement alt
// and needs speculative-alt support (later cycle).
//
// C 6.2.3 tag namespace (now SEPARATED): a bare `Foo` resolves ONLY an
// ordinary typedef-name — a struct TAG `Foo` is reachable only as `struct Foo`
// (see `BareStructTagNotUsableAsTypeName` below). Here the alias `FooT` is a
// genuine typedef (Ordinary), so `FooT origin;` resolves it directly.
TEST(HirLoweringC, TypedefStructAliasAtTopLevel) {
    // The alias name differs from the struct tag here purely for clarity; with
    // the separate tag namespace a SAME-named alias is now also legal (see
    // `TypedefSameNameAsTagCoexistsAcrossNamespaces`).
    SemanticModel model = analyzeC(
        "struct Foo { int x; };\n"
        "typedef struct Foo FooT;\n"
        "FooT origin;\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 3u);   // struct Foo, typedef FooT, origin
    // The third decl is the Global `origin`; its type should be the same
    // composed Struct as the struct decl produced.
    HirNodeId const origin = decls[2];
    ASSERT_EQ(res->hir.kind(origin), HirKind::Global);
    TypeId const originType = res->hir.globalType(origin);
    ASSERT_TRUE(originType.valid());
    auto const& interner = model.lattice().interner();
    EXPECT_EQ(interner.kind(originType), TypeKind::Struct);
    EXPECT_EQ(interner.name(originType), "Foo");
}

// C 6.2.3 tag namespace (closes the tag-namespace residue of
// D-CSUBSET-DECL-GRAMMAR-LOW-RESIDUES): a struct TAG is NOT a bare type name.
// `Foo bare;` (no `struct`, no typedef) must FAIL — the tag `Foo` lives in the
// Tag namespace and an ordinary type-position lookup of the bare identifier
// misses it. `struct Foo bare;` IS the valid spelling and resolves the tag.
// This was previously a DOCUMENTED C DIVERGENCE (the old single-namespace
// resolveTypeNode treated a tag as a bare typedef-name); this cycle is the
// "future cycle that separates the namespaces" the prior pin anticipated.
// RED-ON-DISABLE: with the tag bound Ordinary (pre-change), `Foo bare;`
// resolves and `hasErrors()` is false → the first EXPECT_TRUE fails.
TEST(HirLoweringC, BareStructTagNotUsableAsTypeName) {
    SemanticModel bareModel = analyzeC(
        "struct Foo { int x; };\n"
        "Foo bare;\n");                  // no `struct`, no typedef — invalid in C
    EXPECT_TRUE(bareModel.hasErrors())
        << "a bare struct tag `Foo` is NOT a type name — `struct Foo` is required";
    EXPECT_EQ(countCode(bareModel.diagnostics(), DiagnosticCode::S_UnknownType), 1u)
        << "the bare tag name misses the ordinary namespace → S_UnknownType";

    // The valid spelling — `struct Foo bare;` — resolves the tag and lowers.
    SemanticModel model = analyzeC(
        "struct Foo { int x; };\n"
        "struct Foo bare;\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 2u);   // struct Foo, bare
    HirNodeId const bare = decls[1];
    ASSERT_EQ(res->hir.kind(bare), HirKind::Global);
    TypeId const bareType = res->hir.globalType(bare);
    auto const& interner = model.lattice().interner();
    EXPECT_EQ(interner.kind(bareType), TypeKind::Struct);
    EXPECT_EQ(interner.name(bareType), "Foo");
}

// C 6.2.3 tag namespace: `typedef struct Foo Foo;` is LEGAL — the tag `Foo`
// (Tag namespace) and the typedef alias `Foo` (Ordinary namespace) coexist,
// so NO S_RedeclaredSymbol fires. This INVERTS the prior pin
// (TypedefSameNameAsTagRedeclaresInSingleNamespace), which asserted the old
// single-namespace COLLISION the prior cycle documented as a C divergence.
// RED-ON-DISABLE: route the composite tag BIND Ordinary (drop the
// fieldChildren→Tag gate) and the alias collides with the tag →
// S_RedeclaredSymbol reappears and this count rises above 0.
TEST(HirLoweringC, TypedefSameNameAsTagCoexistsAcrossNamespaces) {
    SemanticModel model = analyzeC(
        "struct Foo { int x; };\n"
        "typedef struct Foo Foo;\n");    // tag Foo (Tag) + typedef Foo (Ordinary)
    EXPECT_FALSE(model.hasErrors())
        << "C 6.2.3: a typedef alias may share a struct tag's name";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "the tag and the typedef are in separate namespaces — no collision";
}

// D5.1 cycle 4 review fix: the DOT form goes through a different lowering
// path than ARROW (no Deref synthesis). The previous test only exercised
// arrow; this one pins the dot path's structural shape.
TEST(HirLoweringC, DotMemberAccessLowersWithoutDeref) {
    SemanticModel model = analyzeC(
        "struct Point { int x; int y; };\n"
        "int by_value(struct Point s) { return s.x; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 2u);
    HirNodeId const fn = decls[1];
    HirNodeId const body = res->hir.functionBody(fn);
    auto stmts = res->hir.children(body);
    ASSERT_EQ(stmts.size(), 1u);
    HirNodeId const ret = stmts[0];
    auto const retVal = res->hir.returnValue(ret);
    ASSERT_TRUE(retVal.has_value());
    HirNodeId const access = *retVal;
    ASSERT_EQ(res->hir.kind(access), HirKind::MemberAccess);
    EXPECT_EQ(res->hir.payload(access), 0u);  // x is field 0
    // Dot form: child is the LHS DIRECTLY, no Deref wrapping.
    auto kids = res->hir.children(access);
    ASSERT_EQ(kids.size(), 1u);
    EXPECT_NE(res->hir.kind(kids[0]), HirKind::Deref);
    // The LHS's type is the struct directly (not Ptr<Struct>).
    auto const& interner = model.lattice().interner();
    EXPECT_EQ(interner.kind(res->hir.typeId(kids[0])), TypeKind::Struct);
}

TEST(HirLoweringC, ExternFunctionAndGlobal) {
    SemanticModel model = analyzeC(
        "extern int puts(int c);\n"
        "extern int errcode;\n"
        "int f() { return puts(0); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 3u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::ExternFunction);   // puts
    EXPECT_EQ(res->hir.externFunctionParams(decls[0]).size(), 1u);  // c
    EXPECT_EQ(res->hir.kind(decls[1]), HirKind::ExternGlobal);     // errcode
    EXPECT_EQ(res->hir.kind(decls[2]), HirKind::Function);         // f
}

// D-CSUBSET-EXTERN-FN-DEFINITION (§B 2026-07-21): an `extern` on a FUNCTION
// DEFINITION lowers to a REAL HirKind::Function with an EMITTED body — NOT an
// ExternFunction import (the pre-cycle mis-lowering, which would DROP the body).
// It reuses the SAME declarator-mode function lowering topLevelDecl's static/plain
// definition arm uses. RED-ON-DISABLE: revert lowerExternDeclInto's isFn branch ->
// addone lowers to a bodyless ExternFunction, this reds.
TEST(HirLoweringC, ExternFunctionDefinitionLowersToFunctionWithBody) {
    SemanticModel model = analyzeC(
        "extern int addone(int x){ return x + 1; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    ASSERT_EQ(res->hir.kind(decls[0]), HirKind::Function)
        << "an extern function DEFINITION lowers to a real Function, not an "
           "ExternFunction import";
    EXPECT_EQ(res->hir.functionParams(decls[0]).size(), 1u);   // x
    // The body is EMITTED (`return x + 1;`), never dropped.
    HirNodeId body = res->hir.functionBody(decls[0]);
    auto stmts = res->hir.children(body);
    ASSERT_EQ(stmts.size(), 1u);
    EXPECT_EQ(res->hir.kind(stmts[0]), HirKind::ReturnStmt)
        << "the extern function definition's body must be emitted";
}

// D-CSUBSET-EXTERN-FN-DEFINITION fail-loud (nested function): an `extern` function
// DEFINITION in BLOCK scope is a nested function (not valid C). It must be rejected
// loud — NEVER silently hoisted to module scope. RED-ON-DISABLE: drop the block-
// scope ExternDecl guard -> f is silently hoisted to a module Function.
TEST(HirLoweringC, NestedExternFunctionDefinitionRejectedLoud) {
    SemanticModel model = analyzeC(
        "int g(void){ extern int f(void){ return 0; } return 0; }\n");
    // Fail-loud at SOME tier (never a silent hoist). If the semantic tier already
    // rejected it, that is fail-loud; otherwise the HIR block-scope guard must.
    if (model.hasErrors()) { SUCCEED(); return; }
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "a block-scope extern function DEFINITION (a nested function) must be "
           "rejected loud, never silently hoisted to module scope";
    EXPECT_FALSE(r.all().empty());
    // And no hoisted module Function `f` leaked out.
    auto decls = res->hir.moduleDecls(res->hir.root());
    std::size_t moduleFns = 0;
    for (HirNodeId d : decls)
        if (res->hir.kind(d) == HirKind::Function) ++moduleFns;
    EXPECT_LE(moduleFns, 1u) << "only g (or none) — f must NOT be hoisted";
}

// D-CSUBSET-EXTERN-FN-DEFINITION fail-loud (override + body): the pathological
// `extern int f(void) "lib" { … }` — a per-declaration library override AND a body
// — must fail loud (H_ExternDeclMalformed), never a silent body-drop. The string
// shifts the kindByChild discriminator off the tail (so it is NOT classified as a
// definition), and the block would otherwise be dropped by the declaration
// lowering. RED-ON-DISABLE: drop the string+block guard -> the body is silently
// dropped and f becomes a bodyless ExternFunction import.
TEST(HirLoweringC, ExternFunctionDefinitionWithLibraryOverrideRejectedLoud) {
    SemanticModel model = analyzeC(
        "extern int f(void) \"lib\" { return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "the form parses + analyzes; the rejection is at lowering";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ExternDeclMalformed), 1u)
        << "a library override on a function definition must fail loud, never "
           "silently drop the body";
}

TEST(HirLoweringC, ExternGlobalWithInitializerRejectedLoud) {
    // D-FF2-3: `extern int x = 5;` is a contradiction — extern means
    // "storage lives elsewhere"; an init would either redefine the
    // symbol locally OR be silently dropped (the prior behavior).
    // Reject loud with H_ExternHasInitializer (remediation-distinct
    // from H_UnsupportedLoweringForKind: remove the init or drop
    // the `extern` keyword — not "extend the engine").
    SemanticModel model = analyzeC("extern int x = 5;\n");
    ASSERT_FALSE(model.hasErrors())
        << "test setup: the c grammar accepts the form; the "
           "rejection is at lowering, not at parse";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ExternHasInitializer), 1u);
    // The pre-fix silent path landed an ExternGlobal at top level; the
    // new path lands an Error sentinel so downstream tooling can't
    // mistake it for a successful extern declaration.
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::Error);
}

// D-CSUBSET-PACKED (F4): a `packed` spelling in the LEADING declaration-specifier
// position (`[[gnu::packed]] struct S {…} v;`) is UNHONORED — packed is honored only
// in the TRAILING composite-attribute slot (`struct S {…} __attribute__((packed))`).
// The linkage scan skips the ignored `stdAttr` wholesale, which would SILENTLY DROP
// packed (leaving the struct padded — a miscompile a program could depend on). It
// fails loud H_UnknownLinkageSpecifier instead, symmetric with the leading
// `__attribute__((packed))` case. Semantically clean (the attribute is a
// declSpecifier); the rejection is at lowering.
TEST(HirLoweringC, LeadingPackedAttributeRejectedLoud) {
    SemanticModel model = analyzeC(
        "[[gnu::packed]] struct S { char c; int v; } gv;\n"
        "int main(void){ return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "test setup: the leading attribute parses + analyzes cleanly; the "
           "rejection is at lowering, not at parse/semantic";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
        << "leading [[gnu::packed]] must fail loud, never silently drop packed";
}

// ★★ P44 (D-CSUBSET-GNU-UNKNOWN-NAME-GATE-ASYMMETRY) — RETARGETED, AND THE
// MEASUREMENT THAT RETARGETED IT REVERSES THE ONE THIS PIN WAS BUILT ON.
//
// It asserted that `__attribute__((packed)) struct S { char c; int v; } gv;` must
// FAIL LOUD, on the reasoning that accepting it would "silently drop packed" and
// leave a struct of the wrong size. ✔RE-MEASURED against the REFERENCES rather
// than against the unpacked control: gcc 13.3.0 (`-std=c2x`) and clang 18.1.3
// (`-std=c23`), probed SEPARATELY, BOTH compile this exact program at rc=0 with
// `'packed' attribute ignored [-Wattributes]` and BOTH report `sizeof(struct S)
// == 8`. A leading `packed` does not reach the composite in ANY of the three
// implementations, so 8 is the CORRECT answer and there was never a layout fact
// to drop — the earlier finding compared DSS to the UNPACKED control and read
// agreement-with-the-references as a miscompile.
//
// So the contract is: ACCEPTED, and NOT HONORED. The refusal half is pinned here
// (a return of the hardcoded Error reds this); the layout half — the part that
// would be a real miscompile — is pinned where `sizeof` is observable, by
// `SemanticAnalyzerC.LeadingGnuPackedDoesNotPackTheComposite`, because this
// fixture analyses with no layout in scope and could not see it.
TEST(HirLoweringC, LeadingGnuPackedAttributeIsAcceptedNotRefused) {
    SemanticModel model = analyzeC(
        "__attribute__((packed)) struct S { char c; int v; } gv;\n"
        "int main(void){ return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "test setup: the leading GNU attribute parses + analyzes cleanly";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "`packed` is attribute vocabulary this language MODELS — the linkage "
           "scan must not adjudicate it as an unrecognized linkage specifier";
    ASSERT_TRUE(res != nullptr);
    EXPECT_TRUE(res->ok) << "every reference compiles this program";
    EXPECT_FALSE(r.hasErrors());
}

// CONTRAST: a leading standard-ignorable attribute (`[[deprecated]]`) STAYS silently
// ignored (C23 6.7.11.1) — ONLY a `packed` spelling fails loud in the leading slot.
// RED-ON-DISABLE for over-broadening: were the F4 hook to fire on any ignored attr,
// this would wrongly report H_UnknownLinkageSpecifier.
TEST(HirLoweringC, LeadingDeprecatedAttributeStillIgnored) {
    SemanticModel model = analyzeC(
        "[[deprecated]] int gv;\n"
        "int main(void){ return 0; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "[[deprecated]] stays standard-ignorable; only packed fails loud";
}

TEST(HirLoweringC, ExternGlobalWithIdentifierInitializerRejectedLoud) {
    // Post-fold #7 PT1a: pin identifier-init `extern int x = y;`
    // (RHS is an operand-rule referencing a prior decl), not just
    // literal-init. Shape-based F4 detector trips on any non-
    // arrayDeclSuffix initValue subtree regardless of RHS shape.
    SemanticModel model = analyzeC(
        "int y = 1;\n"
        "extern int x = y;\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ExternHasInitializer), 1u);
}

TEST(HirLoweringC, ExternGlobalWithEmptyBraceInitializerRejectedLoud) {
    // Post-fold #7 silent-failure F4: pre-fold the init-walk searched
    // for `isExprNode` descendants — `extern int x = {};` has NONE
    // (empty braceInitList), so the silent-accept arm fired. The
    // shape-based detector now keys on the initValue subtree's
    // existence and rejects loud regardless of contents.
    SemanticModel model = analyzeC("extern int x = {};\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ExternHasInitializer), 1u);
}

TEST(HirLoweringC, ExternGlobalWithArraySuffixNoInitStillAccepted) {
    // D-FF2-3 negative: `extern int x[10];` carries an array-size
    // expression (`10`) inside arrayDeclSuffix — that's NOT an init
    // and must NOT trigger H_ExternHasInitializer. The shape-based
    // detector skips the arrayDeclSuffix subtree exactly for this
    // case. Post-fold #7 PT4: also pin that the resulting decl IS
    // an ExternGlobal (not an Error sentinel) — a regression that
    // false-positively rejected `extern int x[10];` would otherwise
    // pass a `res->ok`-only check if no diagnostic landed.
    SemanticModel model = analyzeC("extern int x[10];\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ExternHasInitializer), 0u);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::ExternGlobal);
}

TEST(HirLoweringC, GlobalVariable) {
    SemanticModel model = analyzeC("int counter;\nint f() { return 0; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 2u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::Global);
}

TEST(HirLoweringC, CompoundAssignLowers) {
    // `x += 1` → `x = x + 1`: a simple variable lvalue is read twice safely.
    SemanticModel model = analyzeC("void f(int x) { x += 1; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId stmt = res->hir.children(body)[0];
    ASSERT_EQ(res->hir.kind(stmt), HirKind::AssignStmt);
    EXPECT_EQ(res->hir.kind(res->hir.assignValue(stmt)), HirKind::BinaryOp);  // x + 1
}

TEST(HirLoweringC, IncrementInStatementPositionLowers) {
    // `x++;` (value discarded) → `x = x + 1`.
    SemanticModel model = analyzeC("void f(int x) { x++; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    EXPECT_EQ(res->hir.kind(res->hir.children(body)[0]), HirKind::AssignStmt);
}

TEST(HirLoweringC, ForUpdateIncrement) {
    SemanticModel model = analyzeC("void f() { for (int i = 0; i < 3; i++) {} }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId forS = res->hir.children(res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]))[0];
    ASSERT_EQ(res->hir.kind(forS), HirKind::ForStmt);
    EXPECT_TRUE(res->hir.forUpdate(forS).has_value());
}

TEST(HirLoweringC, ValueYieldingIncrementLowersToSeqExpr) {
    // `return x++;` — postfix yields the OLD value, then mutates. Lowers to a
    // SeqExpr: { var tmp = x; x = x + 1; yield tmp }.
    SemanticModel model = analyzeC("int f(int x) { return x++; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ret  = res->hir.children(body)[0];
    HirNodeId val  = *res->hir.returnValue(ret);
    ASSERT_EQ(res->hir.kind(val), HirKind::SeqExpr);
    // stmts: [var tmp = x, assign x = x+1]; result: ref tmp.
    EXPECT_EQ(res->hir.seqExprStmts(val).size(), 2u);
    EXPECT_EQ(res->hir.kind(res->hir.seqExprStmts(val)[0]), HirKind::VarDecl);
    EXPECT_EQ(res->hir.kind(res->hir.seqExprStmts(val)[1]), HirKind::AssignStmt);
    EXPECT_EQ(res->hir.kind(res->hir.seqExprResult(val)), HirKind::Ref);
}

TEST(HirLoweringC, AssignmentAsSubExpressionLowersToSeqExpr) {
    // `while ((x = x + 1) < 10) {}` — the assignment is used as a value. Lowers
    // to a SeqExpr yielding the assigned value (sound inside a loop condition,
    // where hoisting the store would be wrong).
    SemanticModel model = analyzeC("void f(int x) { while ((x = x + 1) < 10) {} }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body  = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId wh    = res->hir.children(body)[0];
    ASSERT_EQ(res->hir.kind(wh), HirKind::WhileStmt);
    HirNodeId cond  = *res->hir.loopCondition(wh);     // (x = x+1) < 10  → BinaryOp Lt
    ASSERT_EQ(res->hir.kind(cond), HirKind::BinaryOp);
    HirNodeId lhs   = res->hir.children(cond)[0];       // the (x = x+1) sub-expr
    EXPECT_EQ(res->hir.kind(lhs), HirKind::SeqExpr);
}

TEST(HirLoweringC, ComplexLvalueCompoundAssignUsesTempPointer) {
    // `a[i] += 1;` — a complex lvalue. To evaluate `a[i]`'s address once, the
    // lowering binds a temp pointer and reads/writes through it: a Block of
    // { var p = &a[i]; *p = *p + 1; }.
    SemanticModel model = analyzeC("void f(int i) { int a[4]; a[i] += 1; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    // children: [ var a[4], <block for a[i] += 1> ]
    HirNodeId stmt = res->hir.children(body)[1];
    ASSERT_EQ(res->hir.kind(stmt), HirKind::Block);
    auto inner = res->hir.children(stmt);
    ASSERT_EQ(inner.size(), 2u);
    EXPECT_EQ(res->hir.kind(inner[0]), HirKind::VarDecl);     // var p = &a[i]
    EXPECT_EQ(res->hir.kind(inner[1]), HirKind::AssignStmt);  // *p = *p + 1
    // the temp pointer's type is Ptr<I32>
    auto const& ti = model.lattice().interner();
    EXPECT_EQ(ti.kind(res->hir.varDeclType(inner[0])), TypeKind::Ptr);
}

// A COMPOUND assignment USED AS A VALUE (`y = (x += 2)`) — the path the Assign
// frame's `compound` branch handles, which the statement-position compound tests
// above do NOT exercise (those route through the separate `lowerCompoundAssign`).
// `(x += 2)` lowers to `SeqExpr([AssignStmt(Ref x, BinaryOp(Ref x, 2))], yield Ref
// x)`: the stored value is `lvRead(x) + 2` with operand[0] = the lvalue read,
// operand[1] = the rhs (`2`). This pins the flattened frame builds the SAME
// structure (and ordering: the lvRead is emitted before the rhs) as the recursive
// `lowerBinary` Assign arm. Guards the byte-identity of the compound-as-value arm.
TEST(HirLoweringC, CompoundAssignAsSubExpressionLowersToSeqExpr) {
    SemanticModel model = analyzeC(
        "void f(int x) { int y; y = (x += 2); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    // children: [VarDecl y, AssignStmt(y, <value of (x += 2)>)].
    HirNodeId outer = res->hir.children(body)[1];
    ASSERT_EQ(res->hir.kind(outer), HirKind::AssignStmt);
    HirNodeId val = res->hir.assignValue(outer);             // the (x += 2) value
    ASSERT_EQ(res->hir.kind(val), HirKind::SeqExpr);
    auto const stmts = res->hir.seqExprStmts(val);
    ASSERT_EQ(stmts.size(), 1u);
    ASSERT_EQ(res->hir.kind(stmts[0]), HirKind::AssignStmt);  // x = (x + 2)
    HirNodeId stored = res->hir.assignValue(stmts[0]);
    ASSERT_EQ(res->hir.kind(stored), HirKind::BinaryOp);      // x + 2
    auto const ops = res->hir.children(stored);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(res->hir.kind(ops[0]), HirKind::Ref);           // operand[0] = lvRead(x)
    EXPECT_EQ(res->hir.kind(res->hir.seqExprResult(val)), HirKind::Ref);  // yield Ref x
}

TEST(HirLoweringC, ArrayDeclarationLowersToArrayType) {
    // `int a[10]` lowers to a local VarDecl whose type is Array<I32, 10>. (HR9
    // un-deferred arrays: the semantic phase folds the `[10]` declarator suffix
    // into the element type via a constant-length eval.)
    SemanticModel model = analyzeC("void f() { int a[10]; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId vd   = res->hir.children(body)[0];
    ASSERT_EQ(res->hir.kind(vd), HirKind::VarDecl);
    TypeId const ty = res->hir.varDeclType(vd);
    auto const& ti  = model.lattice().interner();
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    ASSERT_EQ(ti.scalars(ty).size(), 1u);
    EXPECT_EQ(ti.scalars(ty)[0], 10);                          // length
    ASSERT_EQ(ti.operands(ty).size(), 1u);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::I32);     // element
}

TEST(HirLoweringC, GlobalArrayLowersToArrayTypeWithNoInit) {
    // A GLOBAL array exercises a different path from the local case: the suffix
    // nests under `topLevelDecl → varDeclTail → arrayDeclSuffix` (a descendant,
    // not a direct child), and `descendantsForInit` must NOT mistake the `[10]`
    // length for the global's initializer.
    SemanticModel model = analyzeC("int g[10];\nint main() { return 0; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId g = res->hir.moduleDecls(res->hir.root())[0];
    ASSERT_EQ(res->hir.kind(g), HirKind::Global);
    auto const& ti = model.lattice().interner();
    TypeId const ty = res->hir.globalType(g);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 10);
    EXPECT_FALSE(res->hir.globalInit(g).has_value()) << "the `[10]` length must not become an initializer";
}

TEST(HirLoweringC, GlobalInitConstEvalIsLeftAssociative) {
    // `int g = 10 - 3 + 1;` — the parse tree is now STRUCTURALLY
    // left-associative, and const-eval follows the tree: (10-3)+1 = 8.
    // The right-recursive mis-shape would fold 10-(3+1) = 6. This pins
    // the full source→parse→semantic→HIR→const-eval pipeline (the
    // hand-built Rig tests in test_const_eval.cpp can't see parser
    // shape bugs).
    SemanticModel model =
        analyzeC("int g = 10 - 3 + 1;\nint main() { return 0; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId g = res->hir.moduleDecls(res->hir.root())[0];
    ASSERT_EQ(res->hir.kind(g), HirKind::Global);
    auto const init = res->hir.globalInit(g);
    ASSERT_TRUE(init.has_value());
    auto cer = evaluateConstant(res->hir, model.lattice().interner(),
                                res->literalPool, *init);
    ASSERT_TRUE(cer.value.has_value())
        << "the global initializer chain must const-fold";
    EXPECT_EQ(std::get<std::int64_t>(cer.value->value), 8)
        << "10 - 3 + 1 must evaluate LEFT-associatively: (10-3)+1 = 8";
}

// D-HIR-INFINITE-LOOP-NOT-TERMINATING × D-LK10-ENTRY-MAIN-IMPLICIT-RETURN
// interaction: `int main() { while (1) { return 5; } }`. The `while (1)` is
// provably-infinite (constant-truthy condition, no break exits its frame), so
// `lowerWhile` wraps it as `Block{ WhileStmt, Synthetic Unreachable }`. That
// wrapper makes the body structurally TERMINATE (`pathTerminates` recurses to
// the wrapper Block's last child, the `Unreachable`). Consequently
// `maybeAppendImplicitReturnZero` correctly sees a `main` that can NEVER fall
// through and appends NO implicit `return 0` (C99 §5.1.2.2.3 only matters when
// `main` can fall off the end — this one provably cannot). The earlier
// double-attach regression (D-LK10 / D-HIR-LOOP-BODY-ONLY-RETURN-DOUBLE-ATTACH)
// stays covered by the straight-line-body pin below, whose body genuinely
// does NOT terminate and so still exercises the implicit-return-0 nest.
//
// Asserts: (1) lowering COMPLETES (no abort), (2) the verifier is clean
// (`res->ok` — the wrapper's `Unreachable` is what satisfies the non-void
// return-completeness check), (3) the loop lowered to the Synthetic-Unreachable
// wrapper, and (4) NO synthetic return was appended (the body is the plain
// lowered block holding exactly the wrapper). The wrapper's `Unreachable` is
// pruned in MIR, so runtime is unchanged (exit 5).
TEST(HirLoweringC, InfiniteLoopMainWrapsLoopAndSkipsImplicitReturn) {
    SemanticModel model = analyzeC("int main() { while (1) { return 5; } }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto const decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    HirNodeId const fn = decls[0];
    ASSERT_EQ(res->hir.kind(fn), HirKind::Function);

    // The body is the plain lowered Block (NOT a synthetic implicit-return-0
    // wrapper — `main` provably terminates, so none was appended). It holds
    // exactly the loop wrapper.
    HirNodeId const body = res->hir.functionBody(fn);
    ASSERT_EQ(res->hir.kind(body), HirKind::Block);
    auto const bodyKids = res->hir.children(body);
    ASSERT_EQ(bodyKids.size(), 1u)
        << "no implicit return-0 appended: body holds only the loop wrapper";

    // The loop wrapper: a Synthetic Block whose children are [WhileStmt,
    // Synthetic Unreachable] — the D-HIR-INFINITE-LOOP-NOT-TERMINATING shape.
    HirNodeId const wrapper = bodyKids[0];
    ASSERT_EQ(res->hir.kind(wrapper), HirKind::Block);
    EXPECT_TRUE(has(res->hir.flags(wrapper), HirFlags::Synthetic))
        << "the infinite-loop wrapper Block must be flagged Synthetic";
    auto const wrapKids = res->hir.children(wrapper);
    ASSERT_EQ(wrapKids.size(), 2u)
        << "wrapper must hold exactly [loop, synthetic-unreachable]";
    EXPECT_EQ(res->hir.kind(wrapKids[0]), HirKind::WhileStmt);
    ASSERT_EQ(res->hir.kind(wrapKids[1]), HirKind::Unreachable);
    EXPECT_TRUE(has(res->hir.flags(wrapKids[1]), HirFlags::Synthetic))
        << "the synthetic Unreachable terminator must be flagged Synthetic";
}

// Breadth pin: the double-attach was NOT loop-specific — ANY non-empty
// `main` body that doesn't structurally terminate hit the re-wrap. A
// straight-line body (`int x; x = 1;`) has no terminator, so the implicit
// return is appended the same way. Same nesting contract; same red-on-
// disable (abort before any EXPECT on the pre-fix children-re-wrap).
TEST(HirLoweringC, NonTerminatingStraightLineMainNestsImplicitReturnZero) {
    SemanticModel model = analyzeC("int main() { int x; x = 1; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto const decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    HirNodeId const outer = res->hir.functionBody(decls[0]);
    ASSERT_EQ(res->hir.kind(outer), HirKind::Block);
    EXPECT_TRUE(has(res->hir.flags(outer), HirFlags::Synthetic));
    auto const outerKids = res->hir.children(outer);
    ASSERT_EQ(outerKids.size(), 2u);

    // The original body is nested as child 0 and still holds its two
    // statements (the VarDecl + the assignment ExprStmt) — not re-parented.
    HirNodeId const inner = outerKids[0];
    ASSERT_EQ(res->hir.kind(inner), HirKind::Block);
    EXPECT_EQ(res->hir.children(inner).size(), 2u);

    HirNodeId const ret = outerKids[1];
    ASSERT_EQ(res->hir.kind(ret), HirKind::ReturnStmt);
    EXPECT_TRUE(has(res->hir.flags(ret), HirFlags::Synthetic));
    ASSERT_TRUE(res->hir.returnValue(ret).has_value());
    EXPECT_EQ(res->hir.kind(*res->hir.returnValue(ret)), HirKind::Literal);
}

// ── D-HIR-INFINITE-LOOP-NOT-TERMINATING HIR-tier pins ───────────────────────
//
// `lowerWhile`/`lowerFor` wrap a PROVABLY-INFINITE loop (constant-truthy/absent
// condition AND no `break` exits its own frame) as `Block{ loop, Synthetic
// Unreachable }`, so the verifier's structural-termination check (which recurses
// to a Block's last child) sees the loop as terminating. This removes the
// over-rejection of a non-`main` non-void function whose terminating tail is
// such a loop, WITHOUT touching the verifier / H0003. The pins below assert the
// wrapper shape (positive) and — critically — that a BREAKABLE / non-constant /
// const-false loop is NOT wrapped (negative, no false positives).

namespace {

// True iff `id`'s subtree contains a Synthetic `Unreachable` leaf — the marker
// the infinite-loop wrapper synthesizes. Used to assert presence (positive pin)
// and ABSENCE (negative pins).
[[nodiscard]] bool subtreeHasSyntheticUnreachable(Hir const& hir, HirNodeId id) {
    if (hir.kind(id) == HirKind::Unreachable
        && has(hir.flags(id), HirFlags::Synthetic))
        return true;
    for (HirNodeId c : hir.children(id))
        if (subtreeHasSyntheticUnreachable(hir, c)) return true;
    return false;
}

// The function declaration whose symbol name is `name` (functions appear in
// source order in `moduleDecls`); InvalidId-shaped HirNodeId if absent.
[[nodiscard]] HirNodeId functionNamed(Hir const& hir, SemanticModel const& m,
                                      std::string_view name) {
    for (HirNodeId d : hir.moduleDecls(hir.root())) {
        if (hir.kind(d) != HirKind::Function) continue;
        SymbolId const sym = hir.functionSymbol(d);
        auto const* rec = m.recordFor(sym);
        if (rec != nullptr && rec->name == name) return d;
    }
    return HirNodeId{};
}

} // namespace

// POSITIVE — the anchor's exact repro: a NON-`main` non-void function whose
// terminating tail is a provably-infinite `while (1)`. Pre-fix this was
// over-rejected H0003 ("non-void function may fall through"); now the loop is
// wrapped, the body structurally terminates, and the verifier is clean.
TEST(HirLoweringC, NonMainInfiniteLoopTailWrapsAndVerifies) {
    SemanticModel model =
        analyzeC("int f(int x){ while(1){ return 5; } } int main(){ return f(0); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    // res->ok folds the verify-on-load pass: TRUE here means the non-void `f`
    // is NO LONGER rejected by checkReturnCompleteness (H0003) — the wrapper's
    // Unreachable made pathTerminates(f-body) true.
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 0u)
        << "the wrapped infinite-loop tail must satisfy non-void return completeness";

    HirNodeId const f = functionNamed(res->hir, model, "f");
    ASSERT_TRUE(f.valid());
    HirNodeId const body = res->hir.functionBody(f);
    ASSERT_EQ(res->hir.kind(body), HirKind::Block);
    auto const bodyKids = res->hir.children(body);
    ASSERT_EQ(bodyKids.size(), 1u);

    // The loop lowered to a Synthetic Block of [WhileStmt, Synthetic Unreachable].
    HirNodeId const wrapper = bodyKids[0];
    ASSERT_EQ(res->hir.kind(wrapper), HirKind::Block);
    EXPECT_TRUE(has(res->hir.flags(wrapper), HirFlags::Synthetic));
    auto const wrapKids = res->hir.children(wrapper);
    ASSERT_EQ(wrapKids.size(), 2u);
    EXPECT_EQ(res->hir.kind(wrapKids[0]), HirKind::WhileStmt);
    ASSERT_EQ(res->hir.kind(wrapKids[1]), HirKind::Unreachable);
    EXPECT_TRUE(has(res->hir.flags(wrapKids[1]), HirFlags::Synthetic));
}

// POSITIVE — the other two provably-infinite shapes also wrap: `for(;;)`
// (absent condition) and `do{...}while(1)` (constant-truthy condition).
TEST(HirLoweringC, ForEverAndDoWhileOneWrapWithUnreachable) {
    SemanticModel forModel =
        analyzeC("int f(int x){ for(;;){ return 9; } } int main(){ return f(0); }");
    ASSERT_FALSE(forModel.hasErrors());
    DiagnosticReporter fr;
    auto forRes = lowerToHir(forModel, fr);
    ASSERT_TRUE(forRes->ok) << (fr.all().empty() ? "" : fr.all()[0].actual);
    HirNodeId const ff = functionNamed(forRes->hir, forModel, "f");
    ASSERT_TRUE(ff.valid());
    auto const fKids = forRes->hir.children(forRes->hir.functionBody(ff));
    ASSERT_EQ(fKids.size(), 1u);
    ASSERT_EQ(forRes->hir.kind(fKids[0]), HirKind::Block);
    auto const fWrap = forRes->hir.children(fKids[0]);
    ASSERT_EQ(fWrap.size(), 2u);
    EXPECT_EQ(forRes->hir.kind(fWrap[0]), HirKind::ForStmt);
    EXPECT_EQ(forRes->hir.kind(fWrap[1]), HirKind::Unreachable);

    SemanticModel doModel =
        analyzeC("int f(int x){ do{ return 7; }while(1); } int main(){ return f(0); }");
    ASSERT_FALSE(doModel.hasErrors());
    DiagnosticReporter dr;
    auto doRes = lowerToHir(doModel, dr);
    ASSERT_TRUE(doRes->ok) << (dr.all().empty() ? "" : dr.all()[0].actual);
    HirNodeId const df = functionNamed(doRes->hir, doModel, "f");
    ASSERT_TRUE(df.valid());
    auto const dKids = doRes->hir.children(doRes->hir.functionBody(df));
    ASSERT_EQ(dKids.size(), 1u);
    ASSERT_EQ(doRes->hir.kind(dKids[0]), HirKind::Block);
    auto const dWrap = doRes->hir.children(dKids[0]);
    ASSERT_EQ(dWrap.size(), 2u);
    EXPECT_EQ(doRes->hir.kind(dWrap[0]), HirKind::DoWhileStmt);
    EXPECT_EQ(doRes->hir.kind(dWrap[1]), HirKind::Unreachable);
}

// NEGATIVE (no false positives) — a BREAKABLE `while(1)` is NOT provably-
// infinite: a `break` reachable in its own frame (through an `if`) exits it.
// The loop must NOT be wrapped (no synthetic Unreachable anywhere in `f`),
// and the bare WhileStmt must sit directly in the body — proof the wrapper
// was not applied. The trailing `return 7` is the real terminator.
TEST(HirLoweringC, BreakableInfiniteLoopIsNotWrapped) {
    SemanticModel model =
        analyzeC("int f(int x){ while(1){ if(x) break; } return 7; } "
                       "int main(){ return f(1); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId const f = functionNamed(res->hir, model, "f");
    ASSERT_TRUE(f.valid());
    EXPECT_FALSE(subtreeHasSyntheticUnreachable(res->hir, f))
        << "a breakable while(1) must NOT be wrapped with a synthetic Unreachable";

    // The body's first statement is the bare WhileStmt (not a wrapper Block).
    auto const bodyKids = res->hir.children(res->hir.functionBody(f));
    ASSERT_GE(bodyKids.size(), 1u);
    EXPECT_EQ(res->hir.kind(bodyKids[0]), HirKind::WhileStmt)
        << "the breakable loop must lower to a bare WhileStmt, not a wrapper Block";
}

// ── FC16 (D-CSUBSET-NORETURN): a direct call to a noreturn function terminates ──
//
// A NON-`main` non-void function whose fall-through tail is a DIRECT call to a
// noreturn function (`_Noreturn void die(int); … die(1);`) is wrapped as
// `Block{ ExprStmt(call), Synthetic Unreachable }` — the direct structural mirror
// of the infinite-loop wrap above — so `f` structurally terminates and the
// verifier is clean. RED-ON-DISABLE (revert detection / the wrap): `f`'s
// fall-through no longer terminates → H_VerifierFailure count 1.
TEST(HirLoweringC, NoreturnCallTailWrapsAndVerifies) {
    SemanticModel model = analyzeC(
        "_Noreturn void die(int); "
        "int f(int x){ if(x>0) return x; die(1); } "
        "int main(){ return f(1); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 0u)
        << "the noreturn-call tail must satisfy non-void return completeness";
    HirNodeId const f = functionNamed(res->hir, model, "f");
    ASSERT_TRUE(f.valid());
    EXPECT_TRUE(subtreeHasSyntheticUnreachable(res->hir, res->hir.functionBody(f)))
        << "a direct call to a noreturn function must be wrapped with a synthetic Unreachable";
}

// All four `noreturn` spellings compile clean (parse + semantic + verify): the
// C11 `_Noreturn` keyword, the C23 `[[noreturn]]`, and both GNU
// `__attribute__((noreturn))` / `__attribute__((__noreturn__))`. Each must wrap
// the `die(1)` tail (no H_VerifierFailure) AND — critically for the GNU forms on
// a file-scope declaration — must NOT trip the linkage scan's
// H_UnknownLinkageSpecifier (the `linkageSpecifierIgnoredNames` / ignoredKinds path).
TEST(HirLoweringC, NoreturnAllFourSpellingsCompileClean) {
    for (char const* proto : {
             "_Noreturn void die(int);",
             "[[noreturn]] void die(int);",
             "__attribute__((noreturn)) void die(int);",
             "__attribute__((__noreturn__)) void die(int);"}) {
        std::string const src = std::string(proto)
            + " int f(int x){ if(x>0) return x; die(1); }"
              " int main(){ return f(1); }";
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors()) << "spelling: " << proto;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << proto << ": "
                             << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 0u) << proto;
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u) << proto;
        HirNodeId const f = functionNamed(res->hir, model, "f");
        ASSERT_TRUE(f.valid()) << proto;
        EXPECT_TRUE(subtreeHasSyntheticUnreachable(res->hir, res->hir.functionBody(f)))
            << proto;
    }
}

// ⚠️ F1 — an INDIRECT / address-takeable callee must NOT be wrapped (the
// conservative direction), else a real return path is elided = MISCOMPILE. Two
// witnesses, both of which `firstNameToken` would have MIS-resolved to a noreturn
// name: (1) a ternary callee `(c ? die : other)(1)` lowers to a NON-Ref node →
// isDirectNoreturnCall false; `other` can return, so f's fall-through does NOT
// terminate → H_VerifierFailure STILL fires (the miscompile witness). (2) a
// function-POINTER object `fp(1)` lowers to Ref(fp) whose record has
// isNoreturn==false → not wrapped. In BOTH the un-relaxed fall-through keeps the
// loud verifier failure — proof we did not silently elide the return path.
TEST(HirLoweringC, NoreturnIndirectCalleeIsNotWrapped) {
    {   // (1) ternary callee — the address-takeable miscompile vector.
        SemanticModel model = analyzeC(
            "_Noreturn void die(int); void other(int); "
            "int f(int x, int c){ if(x>0) return x; (c ? die : other)(1); } "
            "int main(){ return f(1,1); }");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 1u)
            << "an address-takeable ternary callee must NOT be wrapped (F1)";
    }
    {   // (2) function-pointer object callee — Ref, but not a noreturn record.
        SemanticModel model = analyzeC(
            "_Noreturn void die(int); "
            "int f(int x){ if(x>0) return x; void (*fp)(int) = die; fp(1); } "
            "int main(){ return f(1); }");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 1u)
            << "a call through a function-pointer object must NOT be wrapped (F1)";
    }
}

// ── TF-C94: a call through a function POINTER whose DECLARATION spells
//    `noreturn` IS wrapped — the other half of the pair above ────────────────
//
// `NoreturnIndirectCalleeIsNotWrapped` case (2) pins the UNDECORATED pointer:
// Ref(fp), record not noreturn, NOT wrapped, verifier still loud. This pins the
// DECORATED one. The two together say the wrap keys on the RECORD, never on the
// callee's syntactic shape — which is what keeps the F1 miscompile guard true
// while still honoring the attribute GNU actually binds to the pointee type.
//
// This is the CONSUMPTION end of the sink the Tcl 9 `TclStubs` member shape
// feeds (`struct { __attribute__((__noreturn__)) void (*tcl_Panic)(…); }` —
// tclDecls.h). Host clang honors it here too: measured, `int f(struct S
// *s){ s->p(1); }` draws no -Wreturn-type when `p` is decorated and does when
// it is not.
//
// RED-ON-DISABLE: revert the Pass-1 apply gate from `isFnSig ||
// isFnPointerType` back to `isFnSig` → `gp`'s record loses isNoreturn, the tail
// stops terminating, H_VerifierFailure count goes 0 → 1 and the synthetic
// Unreachable disappears.
TEST(HirLoweringC, NoreturnFunctionPointerObjectCallWrapsAndVerifies) {
    SemanticModel model = analyzeC(
        "__attribute__((__noreturn__)) void (*gp)(int); "
        "int f(int x){ if(x>0) return x; gp(1); } "
        "int main(){ return f(1); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 0u)
        << "a call through a pointer DECLARED noreturn must terminate the tail";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "the GNU spelling on a file-scope pointer must not trip the linkage scan";
    HirNodeId const f = functionNamed(res->hir, model, "f");
    ASSERT_TRUE(f.valid());
    EXPECT_TRUE(subtreeHasSyntheticUnreachable(res->hir, res->hir.functionBody(f)))
        << "the decorated function-pointer call must be wrapped with a synthetic "
           "Unreachable, exactly as a direct call to a noreturn function is";
}

// The Tcl 9 `TclStubs` declaration itself, end-to-end through HIR: the leading
// member attribute position must LOWER, not merely parse. Two decorated
// function-pointer members plus a trailing `__format__` run — the literal shape
// at tclDecls.h.
TEST(HirLoweringC, TclStubsLeadingMemberAttributeLowersClean) {
    SemanticModel model = analyzeC(
        "typedef struct TclStubs {\n"
        "  int magic;\n"
        "  __attribute__((__noreturn__)) void (*tcl_Panic)(const char *, ...)\n"
        "      __attribute__((__format__ (__printf__, 1, 2)));\n"
        "  __attribute__((__noreturn__)) void (*tcl_Exit)(int);\n"
        "} TclStubs;\n"
        "int main(){ return 0; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 0u);
}

// ── FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS): the GNU semantic-attribute spellings
//    at FILE scope ride the linkage scan's by-NAME skip end-to-end ────────────

// The FIVE by-name-ignored semantic-attribute spellings (deprecated /
// maybe_unused / unused / nodiscard / warn_unused_result — incl. a
// string-argument form and a dunder form) must lower a FILE-scope declaration
// with ZERO H_UnknownLinkageSpecifier: without the
// `linkageSpecifierIgnoredNames` extension every one hard-fails H000C
// (probe-confirmed at the pre-change HEAD). RED-ON-DISABLE: drop a name from
// the topLevelDecl ignore list → that spelling's H000C count flips to 1.
TEST(HirLoweringC, GnuSemanticAttributeSpellingsFileScopeLowerClean) {
    for (char const* proto : {
             "__attribute__((deprecated)) int f(void) { return 1; }",
             "__attribute__((deprecated(\"use g\"))) int f(void) { return 1; }",
             "__attribute__((__deprecated__)) int f(void) { return 1; }",
             "__attribute__((warn_unused_result)) int f(void) { return 1; }",
             "__attribute__((nodiscard)) int f(void) { return 1; }",
             "__attribute__((unused)) int f(void) { return 1; }",
             "__attribute__((maybe_unused)) int f(void) { return 1; }"}) {
        std::string const src = std::string(proto)
            + " int main(){ return f() - 1; }";
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors()) << "spelling: " << proto;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << proto << ": "
                             << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
            << proto << " — the by-name linkage skip must cover this spelling";
    }
}

// The Fork-2 BOUNDARY regression guard: an UNKNOWN GNU attribute at file scope
// STILL fails loud H_UnknownLinkageSpecifier (the by-name skip covers ONLY the
// declared semantic-attribute names — it must not become a wholesale ignore).
// ★★ P44 ([[D-CSUBSET-GNU-UNKNOWN-NAME-GATE-ASYMMETRY]], THE THIRD TIER) — THIS
// PIN WAS RENAMED AND GREW ITS SECOND HALF, and the reason is worth keeping: it
// was called `GnuUnknownAttributeFileScopeStillFailsLoud` and asserted ONLY the
// diagnostic COUNT, so when the verdict became a warning it stayed GREEN while its
// NAME became a lie. A pin whose name says "FailsLoud" about a program that
// compiles is a misnamed red waiting for the next reader.
//
// It now asserts BOTH halves, which is what makes it able to fail in either
// direction:
//   * the name is still REPORTED — the typo protection this gate exists for. Drop
//     the emit in `linkageFrom` and the count goes to 0.
//   * the program is NOT REFUSED — ✔gcc 13.3.0, ✔clang 18.1.3 and ✔mingw-w64 gcc
//     13.2.0, probed SEPARATELY, all compile this and exit 0 with
//     `'frobnicate' attribute directive ignored [-Wattributes]`. Restore the
//     hardcoded `Error` severity and `res->ok` goes false.
// The severity is the DECLARATION ROW's (`unknownStrictAttributeIsError`, which
// `topLevelDecl` declares false), so this pin also witnesses that the HIR tier
// reads that key rather than overriding it.
TEST(HirLoweringC, GnuUnknownAttributeAtFileScopeIsWarnedNotRefused) {
    SemanticModel model = analyzeC(
        "__attribute__((frobnicate)) int f(void) { return 1; } "
        "int main(){ return f() - 1; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
        << "an unknown GNU attribute must keep the typo-protection diagnostic";
    ASSERT_TRUE(res != nullptr);
    EXPECT_TRUE(res->ok)
        << "…but it must not REFUSE the program: every reference compiles it";
    EXPECT_FALSE(r.hasErrors())
        << "the diagnostic must be a WARNING — a program both references build "
           "must not come out of this tier with an error recorded";
}

// P42 (D-CSUBSET-GNU-UNKNOWN-NAME-GATE-ASYMMETRY) — THE DRIFTED SECOND ROSTER,
// at the tier that owns the file-scope verdict. `linkageSpecifierIgnoredNames`
// is a hand-maintained restatement of the effects table, and the loader's
// drift cross-check DELIBERATELY EXEMPTS the `none` row — so the C23 statement
// and label hints, which THIS language declares KNOWN vocabulary, were never
// added and a leading one on a file-scope object failed loud.
// ✔MEASURED with the shipped CLI before the repair, every one rc=1:
// `__attribute__((fallthrough)) int gv = 7;` → `error[H000C] 'fallthrough' is
// not a recognized linkage specifier`, likewise `likely`, `unlikely`,
// `reproducible`, `unsequenced`. gcc 13.3 (`-std=c2x`) and clang 18
// (`-std=c23`), probed separately, each compile all five with a warning, exit 0.
// RED-ON-DISABLE: delete the five names from `topLevelDecl`'s
// `linkageSpecifierIgnoredNames` in `c.lang.json` → each count returns to 1.
TEST(HirLoweringC, GnuInertHintNamesAreNotUnknownLinkageSpecifiers) {
    for (char const* name : {"fallthrough", "likely", "unlikely",
                             "reproducible", "unsequenced"}) {
        SemanticModel model = analyzeC(
            std::string("__attribute__((") + name + ")) int gv = 7; "
            "int main(){ return gv - 7; }");
        ASSERT_FALSE(model.hasErrors()) << name;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        (void)res;
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
            << "a name the effects table declares KNOWN must not be refused by "
               "the linkage scan's separate roster: " << name;
    }
}

// ★★ P44 (D-CSUBSET-GNU-UNKNOWN-NAME-GATE-ASYMMETRY) — THIS PIN IS INVERTED, AND
// THE INVERSION IS THE FIX. It used to require `packed`, `may_alias` and
// `transparent_union` to STAY LOUD at file scope, on the reasoning that they
// carry layout/aliasing weight DSS does not implement, so the refusal was "the
// only tier still saying so".
// ✔RE-MEASURED, each reference separately: `__attribute__((may_alias)) int gv =
// 7;` is SILENT on gcc 13.3.0 AND clang 18.1.3 — DSS was reporting a name this
// very config models, and refusing the program outright before P44's engine half.
// The layout fear is answered by
// `HirLoweringC.LeadingGnuPackedAttributeIsAcceptedNotRefused` above and by the
// `sizeof` pin in the semantic suite: both references also give `sizeof == 8` for
// the leading `packed`, so nothing is dropped by accepting it.
// The roster these names had drifted out of is now DERIVED from the effects
// table, so their silence is a consequence of the language modelling them rather
// than of anyone remembering to list them.
// RED-ON-DISABLE: delete the derivation loop in `grammar_schema_json.cpp` and
// every count here returns to 1.
TEST(HirLoweringC, GnuModelledInertNamesAreNotUnknownLinkageSpecifiers) {
    for (char const* name : {"packed", "may_alias", "transparent_union"}) {
        SemanticModel model = analyzeC(
            std::string("__attribute__((") + name + ")) int gv = 7; "
            "int main(){ return gv - 7; }");
        ASSERT_FALSE(model.hasErrors()) << name;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res != nullptr) << name;
        EXPECT_TRUE(res->ok) << name;
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
            << "a name the effects table declares KNOWN must not be adjudicated "
               "by the linkage scan's separate roster: " << name;
    }
}

// THE CONTROL that stops the change above from being read as "silence more
// things": a name the language models NOWHERE is still reported. Without this
// pair, deleting the whole unknown-name arm would look like a fix.
TEST(HirLoweringC, GnuGenuinelyUnmodelledNameIsStillReportedAtFileScope) {
    SemanticModel model = analyzeC(
        "__attribute__((frobnicate_xyz)) int gv = 7; "
        "int main(){ return gv - 7; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res != nullptr);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
        << "the typo protection must survive the derivation";
    EXPECT_TRUE(res->ok)
        << "…as a WARNING: both references compile it and exit 0";
}

// ── TfC71 (D-HIR-ADJACENT-CONCAT-WALL-UNTESTED): the linkage-specifier
//    argument's ADJACENT-STRING-CONCAT wall ─────────────────────────────────

namespace {
// The `.actual` text of the FIRST diagnostic carrying `c` (empty if none). The
// adjacent-concat wall and the generic unrecognized-key fall-through share ONE
// DiagnosticCode (H_UnknownLinkageSpecifier), so a count alone cannot tell them
// apart — these pins discriminate on the message.
[[nodiscard]] std::string firstMessageFor(DiagnosticReporter const& r,
                                          DiagnosticCode c) {
    for (auto const& d : r.all()) if (d.code == c) return d.actual;
    return {};
}
} // namespace

// NEGATIVE — C 5.1.1.2 phase 6 makes `visibility("a" "b")` grammatically valid,
// but the composite pairing decodes exactly ONE string body into the
// `<identifier>:<decoded-body>` facet key. The wall fires H_UnknownLinkageSpecifier
// EXACTLY ONCE rather than reading the first piece and walking on.
//
// ★ WHY THIS IS THE ANTI-SILENT-DROP GUARD. Without the wall the scan keeps only
// the FIRST piece and DROPS the rest. That is a WRONG-ATTRIBUTE MISCOMPILE, not a
// crash — the program still builds and still links, just with the linkage facet
// the source did not ask for — therefore it is invisible unless something pins it.
// MEASURED against a wall-relaxed build (`if (false && k2 < toks.size() && …)`),
// the drop lands in two DIFFERENT ways, so both are witnessed here:
//   (1) `"hid" "den"` → first piece forms the key `visibility:hid`, which is NOT
//       in `linkageSpecifiers`, so a relaxed build still errors — but from the
//       generic fall-through arm ("'visibility:hid' is not a recognized linkage
//       specifier"). The COUNT is 1 either way, so a count-only assertion stays
//       GREEN on a relaxed wall. The message is pinned for exactly that reason.
//   (2) `"hidden" "x"` → first piece forms `visibility:hidden`, which DOES
//       resolve. A relaxed build compiles this CLEAN (zero diagnostics) and
//       stamps Hidden visibility while silently discarding `"x"`. THIS is the
//       real miscompile, and this is the witness whose count flips 1 → 0.
TEST(HirLoweringC, AdjacentStringConcatInLinkageArgFailsLoud) {
    {   // (1) neither piece alone is a known key — the wall must still be the
        //     REPORTER, not the downstream unrecognized-key fall-through.
        SemanticModel model = analyzeC(
            "__attribute__((visibility(\"hid\" \"den\"))) int f(int v) "
            "{ return v + 1; }\n"
            "int main(void){ return 0; }\n");
        ASSERT_FALSE(model.hasErrors())
            << "test setup: adjacent concat parses + analyzes cleanly; the "
               "rejection is at lowering, not at parse/semantic";
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        (void)res;
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
            << "the concat wall must fire exactly once — one attribute, one wall";
        EXPECT_NE(firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier)
                      .find("adjacent string concatenation"),
                  std::string::npos)
            << "must be the WALL's diagnostic, not the generic unrecognized-key "
               "fall-through on the silently-truncated key 'visibility:hid'";
    }
    {   // (2) the FIRST piece is a known key — relax the wall and this compiles
        //     clean with `"x"` silently dropped. The miscompile witness.
        SemanticModel model = analyzeC(
            "__attribute__((visibility(\"hidden\" \"x\"))) int f(int v) "
            "{ return v + 1; }\n"
            "int main(void){ return 0; }\n");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        (void)res;
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
            << "a resolvable FIRST piece must not let the trailing piece be "
               "silently dropped — that is the wrong-attribute miscompile";
        EXPECT_NE(firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier)
                      .find("adjacent string concatenation"),
                  std::string::npos);
    }
}

// POSITIVE — the ORDINARY single-string form `visibility("hidden")` still takes
// the normal composite-key path: ZERO H_UnknownLinkageSpecifier *and* the
// `visibility:hidden` facet actually RESOLVES onto the lowered Function's
// linkage side-table entry (Hidden visibility, binding untouched at Global).
//
// ★ WHY THE RESOLVED EFFECT IS ASSERTED, NOT MERELY THE ABSENCE OF A DIAGNOSTIC.
// A negative-only pin is satisfied by an implementation that rejects everything —
// and by one that accepts everything and honors nothing. This is the pin that
// stops the wall from being TIGHTENED into rejecting (or quietly ignoring) the
// single-string form that every real header actually uses: the wall's skip-ahead
// must see PAST the ignored `StringEnd` closer and find NO second opener here.
TEST(HirLoweringC, SingleStringLinkageArgResolvesHiddenVisibility) {
    SemanticModel model = analyzeC(
        "__attribute__((visibility(\"hidden\"))) int f(int v) { return v + 1; }\n"
        "int main(void){ return 0; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "the single-string form is ordinary legal C — the wall must not widen";
    HirNodeId const f = functionNamed(res->hir, model, "f");
    ASSERT_TRUE(f.valid());
    ASSERT_TRUE(res->linkageMap.has(f))
        << "a NON-default visibility must be recorded (the side-table is sparse: "
           "an absent entry means Global/Default — i.e. the facet was DROPPED)";
    EXPECT_EQ(res->linkageMap.get(f).visibility, SymbolVisibility::Hidden);
    EXPECT_EQ(res->linkageMap.get(f).binding, SymbolBinding::Global);
}

// ── TF-C92 (D-CSUBSET-LINKAGE-SPECIFIER-VOCABULARY-INCOMPLETE-VS-REAL-HEADERS):
//    the `visibility:default` twin of the row above ────────────────────────────
//
// ★★ WHAT THESE PINS DO **NOT** CLAIM, STATED FIRST BECAUSE THE OBVIOUS PIN HERE
// IS WRONG. An earlier draft of this cycle justified `visibility:default` by
// claiming that `visibility("hidden")` followed by `visibility("default")` must
// WIDEN back to Default, and proposed that widening as the red-on-disable
// witness. MEASURED with the host `/usr/bin/clang -Wall -Wextra`, BOTH orders:
// clang REJECTS two conflicting visibility attributes on one declaration as a
// HARD ERROR (`error: visibility does not match previous declaration`) and emits
// no object; GCC is documented to warn and keep the EARLIER one. DSS's plain
// last-wins fold is NEITHER — a silent divergence, tracked as
// D-CSUBSET-LINKAGE-SPECIFIER-CONFLICT-SILENT-LAST-WINS — so pinning the
// widening would have enshrined a divergence as this row's contract. There is
// deliberately NO conflicting-visibility test here.
//
// ★ WHAT IS PINNED INSTEAD, AND WHY IT IS THE HONEST FACT. `Default` IS
// `recordLinkage`'s unspecified state (Global+Default stores NOTHING in the
// sparse side-table), so on a non-conflicting declaration this row is
// VALUE-NEUTRAL by construction: its job is to make legal C **compile** with the
// fact threaded, not to change bytes. So the observable contract is exactly two
// things — (1) ZERO H_UnknownLinkageSpecifier in BOTH declaration positions, and
// (2) the visibility that is APPLIED is Default and not some other facet. (2)
// needs a side-table entry to read, and Global+Default has none, so it is read
// off a declaration that carries a co-present `weak` (binding Weak ⇒ the row IS
// stored) — which additionally witnesses that the visibility fold does not
// clobber the binding axis.
//
// PROVENANCE: this arrives from Tcl, not sqlite — tcl.h
// `#define DLLEXPORT __attribute__((visibility("default")))` under
// `__GNUC__ > 3`, reaching tclsqlite-ex.c through TCL_STORAGE_CLASS/EXTERN.

// (1) `topLevelDecl` position — the plain and function forms both lower CLEAN.
TEST(HirLoweringC, VisibilityDefaultTopLevelDeclLowersClean) {
    SemanticModel model = analyzeC(
        "__attribute__((visibility(\"default\"))) int g_vd = 7;\n"
        "__attribute__((visibility(\"default\"))) int f_vd(int v) "
        "{ return v + 1; }\n"
        "int main(void){ return f_vd(g_vd); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "`visibility(\"default\")` is ordinary legal C in both the object and "
           "the function form — delete the topLevelDecl row and this is H000C";
    // The sparse side-table has NOTHING for Global+Default — asserted, not
    // assumed, so nobody later reads an absent entry as a dropped facet.
    HirNodeId const f = functionNamed(res->hir, model, "f_vd");
    ASSERT_TRUE(f.valid());
    EXPECT_FALSE(res->linkageMap.has(f))
        << "Global+Default is recordLinkage's unspecified state — an entry here "
           "would mean the sparsity contract changed, and the APPLIED-fact pin "
           "below (and MirLoweringCLinkage.VisibilityDefaultAppliesDefault"
           "Visibility) would need rewriting";
}

// (2) the APPLIED FACT — read off a decl whose co-present `weak` forces the
// sparse row to exist. Default, not Hidden/Protected: a config typo in the row's
// VALUE turns this red where a diagnostic count alone stays green.
TEST(HirLoweringC, VisibilityDefaultAppliesDefaultVisibilityBesideWeak) {
    SemanticModel model = analyzeC(
        "__attribute__((weak, visibility(\"default\"))) int f_wd(int v) "
        "{ return v + 1; }\n"
        "int main(void){ return 0; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u);
    HirNodeId const f = functionNamed(res->hir, model, "f_wd");
    ASSERT_TRUE(f.valid());
    ASSERT_TRUE(res->linkageMap.has(f))
        << "the co-present `weak` is a NON-default binding, so the sparse row "
           "must exist — without it there is nothing to read the visibility off";
    EXPECT_EQ(res->linkageMap.get(f).visibility, SymbolVisibility::Default)
        << "the row's VALUE must be `default`, not another facet";
    EXPECT_EQ(res->linkageMap.get(f).binding, SymbolBinding::Weak)
        << "the visibility fold must not clobber the binding axis";
}

// (3) `externDecl` position — a DIFFERENT scan root with its own
// `linkageSpecifiers` map, and THE one Tcl actually reaches (tcl.h
// `#define EXTERN extern TCL_STORAGE_CLASS` puts the attribute AFTER `extern`,
// into `externSpecifiers`' TF-C77 repeat rather than `declSpecifiers`).
TEST(HirLoweringC, VisibilityDefaultExternDeclLowersClean) {
    SemanticModel model = analyzeC(
        "extern __attribute__((visibility(\"default\"))) int g_ed;\n"
        "extern __attribute__((visibility(\"default\"))) int f_ed(int);\n"
        "int main(void){ return f_ed(g_ed); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "the externDecl row is independently regressable — deleting IT while "
           "leaving the topLevelDecl twin leaves this form H000C";
}

// (4) THE DUNDER SPELLING **IS** MATCHED — P42, anchor
// D-C-LINKAGE-SPECIFIER-LOOKUP-IS-POSITION-BLIND-AND-NOT-DUNDER-NORMALIZED.
//
// ⚠ WHAT STOOD HERE BEFORE, RECORDED SO IT IS NOT RE-DERIVED AS A REGRESSION:
// `VisibilityDefaultDunderSpellingIsRefusedLoud` pinned
// `__attribute__((__visibility__("default")))` as a LOUD REFUSAL — the TRUE
// behaviour at the time, pinned deliberately so the limit was documented rather
// than discovered. Its own comment named the two ways out: "either two more keys
// per value or dunder-normalizing the lookup — a mechanism change with its own
// design". P42 took the second. That pin is INVERTED here, in the same change as
// the mechanism, because it is the assertion that (correctly) goes red — the
// CLOSING WITNESS, not an obstacle to route around.
//
// ✔THE REFERENCES, PROBED SEPARATELY AND WITH PER-COMPILER STD FLAGS (gcc
// REJECTS `-std=c23`): gcc 13.3.0 under `-std=c2x` AND `-std=gnu2x`, clang
// 18.1.3 under `-std=c23` AND `-std=gnu23`, all with `-Wall -Wextra`. All four
// arms compile EVERY dunder form of `weak` and `visibility` at rc=0 with ZERO
// errors and ZERO warnings, on objects and on functions, in the leading, the
// after-declarator and the post-`extern` positions. DSS refused all eight at
// rc=1 (`error[H000C] '__weak__' is not a recognized linkage specifier`). Under
// `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` that is a conformance defect, not a
// vocabulary preference — and these are the two spellings glibc and the macOS
// SDK actually write.
//
// ★★ THE PIN IS DIFFERENTIAL AND ASSERTS THE EFFECT, NEVER "IT PARSED". A
// count-only pin stays green when the dunder spelling is accepted and then
// DISCARDED, which is the silent drop this project ranks BELOW a loud refusal.
// So each arm requires the dunder form to record the SAME `LinkageAttr` the
// plain form records — a config typo or a dropped facet moves one side only.
// ✔Confirmed end-to-end on the emitted ELF object through the shipped CLI:
// `__attribute__((__weak__)) int gv = 1;` emits `gv V`, byte-for-byte the
// symbol the plain spelling emits, and `__attribute__((__visibility__
// ("hidden")))` emits the TU-local `sym_84 d`, likewise identical to its plain
// twin.
TEST(HirLoweringC, GnuDunderLinkageSpellingsResolveLikeThePlainOnes) {
    struct Folded {
        std::size_t      unknown;
        bool             ok;
        bool             hasRow;
        SymbolBinding    binding;
        SymbolVisibility visibility;
    };
    auto fold = [](char const* spec) -> Folded {
        std::string const src = std::string(spec)
            + " int f_dd(int v) { return v + 1; }\n"
              "int main(void){ return 0; }\n";
        SemanticModel model = analyzeC(src);
        EXPECT_FALSE(model.hasErrors()) << spec;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        Folded out{countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier),
                   res->ok, false, SymbolBinding::Global,
                   SymbolVisibility::Default};
        HirNodeId const f = functionNamed(res->hir, model, "f_dd");
        if (f.valid() && res->linkageMap.has(f)) {
            out.hasRow     = true;
            out.binding    = res->linkageMap.get(f).binding;
            out.visibility = res->linkageMap.get(f).visibility;
        }
        return out;
    };
    struct Case { char const* plain; char const* dunder; };
    for (Case const c : {
             Case{"__attribute__((weak))",
                  "__attribute__((__weak__))"},
             Case{"__attribute__((visibility(\"hidden\")))",
                  "__attribute__((__visibility__(\"hidden\")))"},
             Case{"__attribute__((visibility(\"default\")))",
                  "__attribute__((__visibility__(\"default\")))"},
             // Two clauses in ONE specifier, both dundered: the composite
             // pairing has to survive normalization of the NAME half, and the
             // co-present `weak` forces the sparse row to exist so the
             // visibility is readable at all.
             Case{"__attribute__((weak, visibility(\"default\")))",
                  "__attribute__((__weak__, __visibility__(\"default\")))"}}) {
        Folded const p = fold(c.plain);
        Folded const d = fold(c.dunder);
        EXPECT_EQ(d.unknown, 0u)
            << c.dunder << " — gcc and clang both compile this at rc=0 with no "
                           "diagnostic; a count of 1 is the raw-key lookup back";
        EXPECT_TRUE(d.ok) << c.dunder;
        EXPECT_EQ(d.hasRow, p.hasRow)
            << c.dunder << " — the sparse side-table must agree with the plain "
                           "spelling: a MISSING row where the plain form has one "
                           "is the silent-drop failure, not a fix";
        EXPECT_EQ(d.binding, p.binding)
            << c.dunder << " vs " << c.plain;
        EXPECT_EQ(d.visibility, p.visibility)
            << c.dunder << " vs " << c.plain;
        // The control half: the PLAIN spelling must still be clean, so a green
        // differential can never come from both sides breaking together.
        EXPECT_EQ(p.unknown, 0u) << c.plain << " — the control arm moved";
    }
}

namespace {
// H_UnknownLinkageSpecifier count from lowering `src` under a c schema in
// which the Nth (1-based, file order) `visibility:default` row of
// `semantics.declarations[*].linkageSpecifiers` has been DISABLED — row 1 is
// `topLevelDecl`, row 2 is `externDecl`.
//
// The row is disabled by RENAMING ITS KEY to a spelling no source token can
// produce, while keeping the JSON structurally valid — no trailing-comma surgery,
// and no chance of a malformed-config false green. Surgical textual swap on the
// shipped text, the `shiftResultKind` shape.
// ⚠ UPDATED P42: this used to say the swap is behaviourally identical to a
// delete "because the lookup is by raw key". THE LOOKUP IS NO LONGER BY RAW KEY
// — an attribute-position name is dunder-normalized before the map is asked
// (D-C-LINKAGE-SPECIFIER-LOOKUP-IS-POSITION-BLIND-AND-NOT-DUNDER-NORMALIZED). The
// lever is still valid, and now for a stronger reason: no token text, dundered or
// not, can NORMALIZE to `visibility:default__UNREACHABLE` either, since
// `stripDunder` only ever removes a leading and trailing `__`.
//
// The quoted key occurs EXACTLY TWICE in the shipped file (verified): the two
// `$visibilityDefaultComment` prose blocks spell it in BACKTICKS, so the search
// cannot land in a comment. The count-mismatch guard below makes a future third
// row a loud test failure rather than a silently mis-aimed perturbation.
[[nodiscard]] std::size_t unknownLinkageWithDefaultRowDisabled(
    std::string const& src, int nth) {
    constexpr auto kBad = static_cast<std::size_t>(-1);
    std::string text = shippedCText();
    std::string const needle = "\"visibility:default\"";
    std::size_t pos = std::string::npos;
    std::size_t from = 0;
    for (int i = 0; i < nth; ++i) {
        pos = text.find(needle, from);
        if (pos == std::string::npos) {
            ADD_FAILURE() << "shipped c carries fewer than " << nth
                          << " `visibility:default` rows — this lever is stale";
            return kBad;
        }
        from = pos + needle.size();
    }
    text.replace(pos, needle.size(), "\"visibility:default__UNREACHABLE\"");
    auto schema = GrammarSchema::loadFromText(
        text, "<visibility-default-row-" + std::to_string(nth) + "-disabled>");
    if (!schema) {
        ADD_FAILURE() << "perturbed schema failed to load (row " << nth << ")";
        return kBad;
    }
    UnitBuilder builder{*schema, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(src, "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel model = analyze(cu, DiagnosticBudget::libraryDefault());
    if (model.hasErrors()) {
        ADD_FAILURE() << "front-end errors under the perturbed schema (row "
                      << nth << ")";
        return kBad;
    }
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    (void)res;
    return countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier);
}
} // namespace

// (5) THE TWO ROWS ARE INDEPENDENTLY LOAD-BEARING — the red-on-disable lever run
// BY THE SUITE rather than claimed in prose. Disabling row N must break the form
// that routes through root N and leave the OTHER form clean; that cross-check is
// what proves the two scan roots really do consult separate maps (a single shared
// map would make both forms go red on either perturbation, and one deleted row
// would then look harmless).
TEST(HirLoweringC, VisibilityDefaultRowsAreIndependentlyLoadBearing) {
    std::string const topLevelSrc =
        "__attribute__((visibility(\"default\"))) int g_vd = 7;\n"
        "__attribute__((visibility(\"default\"))) int f_vd(int v) "
        "{ return v + 1; }\n"
        "int main(void){ return f_vd(g_vd); }\n";
    // ⚠ P53 CORRECTION, BY MEASUREMENT
    // (D-C-EXTERN-MUST-LEAD-THE-DECLARATION-SPECIFIERS): this source used to be
    // written at FILE scope, and that spelling no longer routes through
    // `externDecl` at all — the file-scope declaration rules were MERGED, so a
    // file-scope `extern` now reads `topLevelDecl`'s map and the two arms below
    // measured the same row twice (✔MEASURED: row-1 disabled gave 2, not 0).
    // The INVARIANT is unchanged and still worth pinning — two scan roots, two
    // maps, independently load-bearing — but `externDecl` survives only as the
    // BLOCK-scope rule (D-CSUBSET-BLOCK-SCOPE-EXTERN), so that is where its form
    // has to be spelled. A test whose premise moves out from under it while its
    // assertions still pass is the failure this repository names
    // [[feedback-a-rows-premise-has-a-shelf-life]]; here the premise moved and
    // the assertion went red, which is the instrument working.
    std::string const externSrc =
        "int main(void){\n"
        "  extern __attribute__((visibility(\"default\"))) int g_ed;\n"
        "  extern __attribute__((visibility(\"default\"))) int f_ed(int);\n"
        "  return f_ed(g_ed);\n"
        "}\n";

    // Row 1 = topLevelDecl: both attributed top-level declarations go loud.
    EXPECT_EQ(unknownLinkageWithDefaultRowDisabled(topLevelSrc, 1), 2u)
        << "one H000C per attributed topLevelDecl once the row is unreachable";
    EXPECT_EQ(unknownLinkageWithDefaultRowDisabled(externSrc, 1), 0u)
        << "the externDecl form must NOT depend on the topLevelDecl row";

    // Row 2 = externDecl: the mirror image.
    EXPECT_EQ(unknownLinkageWithDefaultRowDisabled(externSrc, 2), 2u)
        << "one H000C per attributed externDecl once the row is unreachable";
    EXPECT_EQ(unknownLinkageWithDefaultRowDisabled(topLevelSrc, 2), 0u)
        << "the topLevelDecl form must NOT depend on the externDecl row";
}

// ── TF-C92 (D-CSUBSET-NO-SANITIZE-THREAD): the STRING-ARGUMENT spelling
//    `no_sanitize("thread")` is REFUSED LOUD ────────────────────────────────────
//
// DSS honors the bare GNU `no_sanitize_thread` (an `attributeSemantics` effect
// that reaches MirFunc.noSanitizeThread and prints as `nosanitizethread` in
// `.dssir`). The clang-style STRING form is a different clause name entirely:
// `linkageFrom`'s by-NAME skip runs on the bare dunder-stripped identifier
// (`no_sanitize`) BEFORE the composite pairing, and `no_sanitize` is in NEITHER
// `linkageSpecifierIgnoredNames` list — so the pairing assembles the composite
// key `no_sanitize:<arg>`, the strict lookup misses, and H000C fires. sqlite
// writes only the bare spelling (src/wal.c), so the loud refusal is the
// correct residue and not a gap.
//
// ★★ WHY THIS PIN EXISTS AT ALL: THE WRONG FIX IS A ONE-TOKEN EDIT. Adding
// `no_sanitize` to either `linkageSpecifierIgnoredNames` array would make
// `no_sanitize("thread")`, `("address")` and `("undefined")` ALL silently
// accepted-and-discarded — a sanitizer exclusion the source asked for and DSS
// threw away, with no diagnostic anywhere. The config loader's attribute-
// vocabulary cross-check constrains effects→names only, never the inverse, so
// NOTHING else in the suite goes red on that edit. These two pins are the whole
// wall. The ARGUMENT is asserted (not just the count) because every unrecognized
// key shares one DiagnosticCode: a count-only pin cannot tell "refused the
// string form" from "refused the clause name and dropped the argument".
TEST(HirLoweringC, NoSanitizeStringArgumentFormIsRefusedLoud) {
    struct Case {
        char const* arg;
        char const* compositeKey;
    };
    // `("thread")` is the one a reader would expect DSS to honor (it names the
    // same sanitizer as the bare spelling DSS DOES honor) — so it is the shape
    // most likely to be "fixed" by an ignore-list entry. `("address")` is the
    // collateral: it names a sanitizer with no DSS spelling at all, and would be
    // swallowed by the same one-token edit.
    for (Case const& c : {Case{"thread", "no_sanitize:thread"},
                          Case{"address", "no_sanitize:address"}}) {
        SemanticModel model = analyzeC(
            std::string{"static __attribute__((no_sanitize(\""} + c.arg
            + "\"))) int f(int k){ return k + 1; }\n"
              "int main(void){ return 0; }\n");
        ASSERT_FALSE(model.hasErrors())
            << "setup: the string form parses and analyzes cleanly — the "
               "refusal is at lowering, not at parse/semantic (arg=" << c.arg
            << ")";
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        (void)res;
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
            << "exactly one refusal — one clause, one wall (arg=" << c.arg << ")";
        EXPECT_NE(firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier)
                      .find(c.compositeKey),
                  std::string::npos)
            << "the message must name the COMPOSITE key `" << c.compositeKey
            << "`, which is what proves the string ARGUMENT reached the lookup "
               "instead of being dropped off a bare `no_sanitize`";
    }
}

// (The `static __attribute__((deprecated))` no-clobber pin — the co-present
// `static` keeping its INTERNAL binding — lives at the MIR tier where the
// binding is observable: MirLoweringCLinkage
// .GnuDeprecatedDoesNotClobberStaticLinkage.)

// The bare attribute-declaration statement lowers to NOTHING observable: the
// `[[fallthrough]];` item maps to Skip (an empty Block) and the enclosing
// declOrAttrStmt wrapper is peeled by the unmapped-statement PassThrough —
// zero H diagnostics, verifier clean.
TEST(HirLoweringC, FallthroughStatementLowersToSkip) {
    SemanticModel model = analyzeC(
        "int main(){ int x = 1; int acc = 0; "
        "switch (x) { case 1: acc += 1; [[fallthrough]]; "
        "case 2: acc += 10; break; default: acc = 99; break; } "
        "return acc - 11; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 0u);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u);
}

// ── TF-C73 (D-CSUBSET-GNU-ATTRIBUTE-LEADING-ARG-SOUP +
//    D-CSUBSET-GNU-ATTRIBUTE's pinned after-declarator follow-up) ──────────────────────
//
// Two defects, ONE fix, because they are one unit: seeding `linkageFrom` with
// the after-declarator roots (defect 2) makes the arg tokens of a real-header
// prototype (`… __attribute__((format(printf,1,2)))`) reachable by the specifier
// scan for the first time, so shipping defect 2 WITHOUT the arg-token flag
// (defect 1) would convert a silent drop into a wall of spurious errors on every
// glibc/tcl header. Neither half is separately shippable.

namespace {
// TF-C73: the APPLIED explicit alignment, in bytes, of the module-level Global
// named `want` — 0 when that declaration carries NO entry in the alignment
// side-table.
//
// ★ 0 IS AN UNAMBIGUOUS "NOTHING WAS APPLIED", not a plausible answer that could
// mask a bug. The side-table is SPARSE and `AlignmentAttr.alignmentBytes` is
// never 0 by construction (an absent override is the ABSENCE of an entry, never
// a zero value — see hir/attributes/alignment_attr.hpp), so this helper's 0
// means exactly "the declaration was lowered with no explicit alignment", which
// is the silent under-alignment the `aligned` sink exists to prevent and the one
// failure a diagnostic-count pin cannot see.
[[nodiscard]] std::uint32_t globalAlignment(CstToHirResult const& res,
                                            SemanticModel const& model,
                                            char const* want) {
    for (HirNodeId d : res.hir.moduleDecls(res.hir.root())) {
        if (res.hir.kind(d) != HirKind::Global) continue;
        auto const* rec = model.recordFor(res.hir.globalSymbol(d));
        if (rec == nullptr || rec->name != want) continue;
        if (auto const* a = res.alignmentMap.tryGet(d)) return a->alignmentBytes;
    }
    return 0u;
}
} // namespace

// DEFECT 1 — a LEADING attribute's ARGUMENTS are no longer read as linkage
// specifier names.
//
// ★ THE TWO CASES ASSERT DIFFERENT THINGS ON PURPOSE, and the difference is the
// point. Both prove the ARG tokens are gone as SPECIFIER NAMES; they differ on
// what happens to the argument afterwards:
//   • `format` has no DSS model, so it is in `linkageSpecifierIgnoredNames` and
//     the whole declaration is clean — count 0 AND an empty message (the message
//     is asserted because a count alone cannot tell "no diagnostic" from "a
//     different diagnostic that happens to total zero here").
//   • `aligned` DOES have a DSS model, so its argument is not merely ignored —
//     it is CONSUMED. The arm below asserts the APPLIED ALIGNMENT (16), which is
//     strictly stronger than either a count-1 or a count-0 pin: it fails if the
//     arg comes back as a specifier name (count 2), it fails if `aligned` were
//     "fixed" by ignoring the name (count 0, alignment 0), and it fails if the
//     argument were parsed but decoded wrong (count 0, alignment ≠ 16). Only an
//     applied-value assertion covers that third case at all.
//
// ★ WHAT THIS PIN USED TO SAY, AND WHY IT CHANGED. Until TF-C73 both arms
// asserted `aligned` was LOUD — "exactly one diagnostic, and it names
// `aligned`" — with a banner forbidding anyone to quiet it by adding `aligned`
// to `linkageSpecifierIgnoredNames`. That prohibition was never about the noise;
// it was about the SINK. DSS sourced alignment only from `alignasSpec`, so
// ignoring the name would have compiled `__attribute__((aligned(16))) int v;`
// clean with the alignment GONE — a silently under-aligned object, a miscompile.
// The banner's own escape clause was "the ignore entry belongs in the same
// commit as a real `aligned` sink, never before it". THAT SINK LANDED in TF-C73
// (`AttributeEffect::Align` → the clause argument is const-evaluated into
// `SymbolRecord.explicitAlignment`, the SAME sink and the same validation path
// `alignas` uses), so the condition the prohibition was waiting on is met and
// the expectation is inverted rather than deleted. What guards the behavior now
// is NOT a diagnostic count — it is the applied `AlignmentAttr` asserted below,
// plus the runtime address check in `examples/c/gnu_aligned_attribute/`.
// A count pin here would now be DEAD: the regression it must catch (a dropped or
// mis-decoded alignment) emits nothing to count.
//
// RED-ON-DISABLE — two independent disables, both recorded with how they were
// measured, because they fail this pin in two DIFFERENT ways:
//   • drop the `fromAttrArg[i]` guard in `linkageFrom` → the argument tokens are
//     read as specifier names again. MEASURED on the pre-sink tree (where the
//     `aligned` clause name was still loud, so its baseline was 1 rather than
//     today's 0): aligned(16) 1 → 2 ("'16' is not a recognized linkage
//     specifier"); format(printf,1,2) 0 → 3 ('printf', '1', '2' — the commas are
//     covered by the `Comma` ignoredKind, so they stay silent). The `format`
//     half is untouched by the sink and still reads exactly as measured; the
//     `aligned` half now starts from 0, so it would go 0 → 1.
//   • MEASURED THIS CYCLE, through a config tree patched under `DSS_CONFIG_ROOT`
//     (the live checkout was never modified): demote the `aligned` row in
//     `semantics.attributeSemantics.effects` from `"effect": "align"` to
//     `"effect": "none"` → the alignment assertion goes 16 → 0 while the
//     diagnostic count STAYS 0. That silence IS the argument for asserting a
//     value here: the regression is completely invisible to a count. Deleting
//     the row outright measures the same 16 → 0 at this position (it also makes
//     `aligned` unknown vocabulary, which fails loud S0031 at the typedef and
//     member positions — shapes this pin does not exercise).
TEST(HirLoweringC, LeadingAttributeArgumentsAreNotLinkageSpecifiers) {
    {   // an ABI-AFFECTING name is HONORED: the clause is clean AND its ARGUMENT
        // is applied — the argument is consumed, not merely silenced
        SemanticModel model = analyzeC(
            "__attribute__((aligned(16))) int v;\nint main(void){ return v; }\n");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
            << "`aligned` has a real sink now, so neither the clause NAME nor "
               "its argument `16` may reach the linkage-specifier fall-through";
        EXPECT_EQ(firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier),
                  std::string{})
            << "asserted beside the count so a future generic fall-through that "
               "re-uses this code cannot hide behind a zero total";
        // THE APPLIED FACT — the half a count can never carry. Silence alone is
        // satisfied by an `aligned` that is ignored by name, which is exactly
        // the under-aligned-object outcome the old loud pin was protecting
        // against; only the 16 distinguishes honored from quietly discarded.
        EXPECT_EQ(globalAlignment(*res, model, "v"), 16u)
            << "the LEADING `aligned(16)` must be APPLIED to `v`: 0 means the "
               "clause was silently dropped (an under-aligned object, the "
               "miscompile the pre-sink loud behavior existed to prevent), and "
               "any other value means the argument was decoded wrong";
    }
    {   // a hint attribute with a multi-arg list is clean end-to-end
        SemanticModel model = analyzeC(
            "__attribute__((format(printf,1,2))) int h(const char*, ...);\n"
            "int main(void){ return 0; }\n");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
            << "`format` is ignored by name and printf/1/2 are argument tokens";
        EXPECT_EQ(firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier),
                  std::string{})
            << "asserted beside the count so a future generic fall-through that "
               "re-uses this code cannot hide behind a zero total";
    }
}

// ★★ DEFECT 1's ANTI-OVER-SKIP PIN — the single most important test here.
//
// The obvious implementation of "ignore attribute arguments" is to add the arg
// rule to `linkageSpecifierIgnoredRules` and skip the subtree WHOLESALE. That is
// REFUTED, and this is the witness: the composite key `<ident>:<string-body>` is
// assembled by a FORWARD SCAN over the flattened token list, and the string lives
// INSIDE the argument group. Remove those tokens and the pairing has nothing left
// to pair with, so the bare `visibility` misses the strict map.
//
// RED-ON-DISABLE (observed, by patching the shipped config's topLevelDecl row to
// `"linkageSpecifierIgnoredRules": ["stdAttr", "alignasSpec", "attrArgs"]`):
//   error[H000C] 'visibility' is not a recognized linkage specifier
// — i.e. this test's count goes 0 → 1 and the Hidden visibility disappears.
// The failure is LOUD rather than silent, which is exactly what makes the WRONG
// fix cheap to detect and the RIGHT fix easy to under-test: a pin that only
// counted diagnostics on `format(...)` would stay green through that regression.
TEST(HirLoweringC, AttrArgFlagDoesNotBreakCompositeVisibilityKey) {
    SemanticModel model = analyzeC(
        "__attribute__((visibility(\"hidden\"))) int g;\n"
        "int main(void){ return g; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "the arg-token flag must MARK the string, never REMOVE it";
    // The applied fact, not merely the absence of a diagnostic: a wholesale skip
    // also produces "no visibility recorded", and only this half distinguishes
    // "resolved to Hidden" from "quietly resolved to nothing".
    bool sawHidden = false;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (!res->linkageMap.has(d)) continue;
        if (res->linkageMap.get(d).visibility == SymbolVisibility::Hidden)
            sawHidden = true;
    }
    EXPECT_TRUE(sawHidden)
        << "the composite key `visibility:hidden` must still RESOLVE — an absent "
           "entry means the facet was dropped, which is the wholesale-skip bug";
}

// DEFECT 2 — an AFTER-DECLARATOR attribute now reaches the linkage fold.
//
// ★ THIS ASSERTS THE APPLIED FACT, NOT A DIAGNOSTIC COUNT, and that is
// deliberate: the bug was SILENT. Measured at the pre-change HEAD,
// `int f(void) __attribute__((weak));` compiled with ZERO diagnostics and simply
// lost the weak binding — the attribute run was consumed by the init-detection
// skip and `linkageFrom` never saw it. A count-based pin would have been dead on
// arrival because the broken behavior emits nothing to count.
//
// ★ THE WITNESS IS AN OBJECT DEFINITION, NOT A FUNCTION PROTOTYPE, and the reason
// is worth stating so nobody "improves" it back. A function's after-declarator
// attribute can only sit on a PROTOTYPE (`int f(void) __attribute__((weak)) {…}`
// is rejected upstream — S_TypeMismatch/S0018, a separate semantic-tier gap), and
// a weak prototype correctly DECLINES `prototypeSynthesizesExtern`, so it emits NO
// HIR node at all and therefore has no linkage entry to assert on. An object
// declaration IS its own definition, so the fold is observable exactly where the
// attribute sits — no cross-declaration merge in the way.
//
// RED-ON-DISABLE (observed): make `declaratorAttrRoots` (cst_to_hir.cpp) return
// an empty vector — deleting its after-declarator collection loop, so
// `declaratorLinkage` hands back the `linkagePrefixRoots` fold untouched → both
// `sawWeak` and `sawHidden` go true → false, i.e. straight back to the silent
// drop. (The two shipped root builders are `linkagePrefixRoots` and
// `declaratorAttrRoots`; there has never been a `linkageScanRoots`.)
TEST(HirLoweringC, AfterDeclaratorAttributeReachesLinkage) {
    SemanticModel model = analyzeC(
        "int gv __attribute__((weak)) = 5;\n"
        "int gh __attribute__((visibility(\"hidden\"))) = 7;\n"
        "int main(void){ return gv + gh - 12; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "`weak` / `visibility` are DECLARED linkage specifiers — they must "
           "resolve from the trailing position, not fail";
    bool sawWeak = false, sawHidden = false;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (!res->linkageMap.has(d)) continue;
        LinkageAttr const a = res->linkageMap.get(d);
        if (a.binding == SymbolBinding::Weak)            sawWeak = true;
        if (a.visibility == SymbolVisibility::Hidden)    sawHidden = true;
    }
    EXPECT_TRUE(sawWeak)
        << "the after-declarator `__attribute__((weak))` must produce a WEAK "
           "binding — the linkage side-table is sparse, so no entry at all is "
           "precisely the silent drop this fixes";
    // The COMPOSITE key resolving from a trailing root proves the two halves of
    // this cycle compose: the arg-token flag keeps the string visible to the
    // forward scan (defect 1) *and* the trailing subtree is now scanned at all
    // (defect 2). Either half missing and this assertion fails.
    EXPECT_TRUE(sawHidden)
        << "`visibility(\"hidden\")` must resolve its composite key from the "
           "AFTER-DECLARATOR position exactly as it does from the leading one";
}

// DEFECT 2's fail-loud half: an UNKNOWN attribute in the after-declarator
// position must now be as loud as the same attribute in the leading position.
// Measured at the pre-change HEAD this compiled SILENTLY CLEAN.
//
// ★ THE MESSAGE TEXT IS ASSERTED, NOT THE COUNT. `linkageFrom` has THREE
// producers of H_UnknownLinkageSpecifier (the unrecognized-key fall-through, the
// adjacent-string-concat wall, and the leading-`packed` guard), so "count == 1"
// does not identify WHICH one fired — a future change that made this shape trip
// the concat wall instead would keep a count pin green while reporting nonsense.
//
// RED-ON-DISABLE (observed): make `declaratorAttrRoots` (cst_to_hir.cpp) return
// an empty vector, so only the `linkagePrefixRoots` fold survives → the message
// goes from the text below to EMPTY (count 1 → 0), i.e. straight back to silent
// acceptance. (The two shipped root builders are `linkagePrefixRoots` and
// `declaratorAttrRoots`; there has never been a `linkageScanRoots`.)
TEST(HirLoweringC, AfterDeclaratorUnknownAttributeFailsLoud) {
    SemanticModel model = analyzeC(
        "int f(void) __attribute__((frobnicate));\n"
        "int f(void){ return 1; }\n"
        "int main(void){ return f() - 1; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    (void)res;
    EXPECT_EQ(firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier),
              "'frobnicate' is not a recognized linkage specifier")
        << "an unknown attribute AFTER the declarator must fail loud with the "
           "unrecognized-key message — an empty string here means the trailing "
           "position went back to parse-and-ignore";
}

// TF-C73 — the after-declarator attribute is PER-DECLARATOR, not per-declaration.
//
// C attaches an after-declarator attribute to the ONE declarator it follows, so
// `int a __attribute__((weak)) = 3, b = 4;` weakens `a` and leaves `b` alone.
// The first cut of this feature folded the trailing run at DECLARATION level and
// pushed `a`'s binding onto `b` too — an over-application: still a wrong answer,
// even though it was less wrong than the silent drop that preceded it.
//
// ★ GROUND TRUTH IS REAL CLANG, not my reading of the standard. The same program
// built with `clang -Wall -Wextra` (zero warnings) gives, via `nm -m`:
//     __DATA,__data  weak external  _a
//     __DATA,__data  external       _b
// so "a weak, b default" is the answer a real toolchain produces.
//
// ★ ASSERTS APPLIED linkageMap FACTS, not diagnostic counts — the failure mode is
// a WRONG APPLIED VALUE (`b` silently weak), and the broken version emits no
// diagnostic at all, so a count pin would be dead on arrival.
//
// RED-ON-DISABLE (observed): make the globals arm of `lowerTopLevelInto` record
// one declaration-level attr again (fold the trailing run into the shared base
// and drop the `origins` mapping) → `b` comes back Weak and this test fails on
// the `bWeak` assertion.
TEST(HirLoweringC, AfterDeclaratorAttributeIsPerDeclaratorNotPerDeclaration) {
    SemanticModel model = analyzeC(
        "int a __attribute__((weak)) = 3, b = 4;\n"
        "int main(void){ return a + b - 7; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u);
    bool sawA = false, sawB = false, aWeak = false, bWeak = false;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) != HirKind::Global) continue;
        auto const* rec = model.recordFor(res->hir.globalSymbol(d));
        if (rec == nullptr) continue;
        // The side-table is SPARSE: no entry means Global/Default binding, which
        // is exactly what `b` must have.
        bool const weak = res->linkageMap.has(d)
                       && res->linkageMap.get(d).binding == SymbolBinding::Weak;
        if (rec->name == "a") { sawA = true; aWeak = weak; }
        if (rec->name == "b") { sawB = true; bWeak = weak; }
    }
    ASSERT_TRUE(sawA) << "test setup: `a` must lower to a Global";
    ASSERT_TRUE(sawB) << "test setup: `b` must lower to a Global";
    EXPECT_TRUE(aWeak)
        << "`a` carries the __attribute__((weak)) — it must be WEAK";
    EXPECT_FALSE(bWeak)
        << "`b` carries NO attribute — a sibling declarator's weak binding must "
           "not leak onto it (clang emits `b` as a plain external symbol)";
}

// TF-C73 — the POSITIONAL SYMMETRY pin, and the reason
// `GnuAttributeAfterDeclaratorLowersClean` above no longer carries an `aligned`.
//
// `aligned` is ABI-AFFECTING, and before this cycle the two positions gave two
// DIFFERENT answers for the same attribute on the same program: the LEADING
// position failed it loud (no sink existed, and going quiet would have produced
// an under-aligned object), while the TRAILING position silently ACCEPTED it and
// dropped the alignment. That asymmetry IS the bug — an attribute must not mean
// two different things depending on which side of the declarator it sits.
//
// ★ THE SYMMETRY IS UNCHANGED; THE SHARED ANSWER IS. This pin was written when
// the only honest shared answer was "loud in BOTH positions", carrying a banner
// that forbade quieting it by adding `aligned` to `linkageSpecifierIgnoredNames`
// — because that would have made BOTH positions silently drop the alignment, a
// miscompile strictly worse than the noise. That banner named its own expiry:
// "the ignore entry belongs in the same commit as a real `aligned` sink, never
// before it". THE SINK LANDED in TF-C73 (`AttributeEffect::Align` — the clause
// argument is const-evaluated into `SymbolRecord.explicitAlignment`, the same
// sink and same validation path `alignas` already used), so the shared answer is
// now "APPLIED, identically, in BOTH positions". Same intent, inverted
// expectation — the test was not deleted, because the asymmetry it guards can
// regress in either direction.
//
// ★ ASSERTS THE APPLIED ALIGNMENT, NOT A DIAGNOSTIC COUNT, and that is now
// forced rather than stylistic. With a sink present, BOTH the correct behavior
// and the regression are silent: an `aligned` that is dropped, ignored by name,
// or decoded to the wrong number all emit ZERO diagnostics, so a count pin would
// be dead on arrival — it would read 0 in every one of those cases and stay
// green through the exact miscompile this exists to prevent. 32 is a genuine
// OVER-alignment for `int` (natural 4), so the value is load-bearing: a sink
// that silently fell back to natural alignment reads 4 or 0, never 32.
//
// WHAT GUARDS THIS NOW: the applied `AlignmentAttr` below (both positions), and
// the runtime address check in `examples/c/gnu_aligned_attribute/`, which
// re-witnesses the same two positions as a masked ADDRESS in a running program
// on every target — including the shipped `release` pipeline, so an optimizer
// that re-laid-out the globals would be caught too.
//
// RED-ON-DISABLE (MEASURED THIS CYCLE, through a config tree patched under
// `DSS_CONFIG_ROOT` — the live checkout was never modified): demote the
// `aligned` row in `semantics.attributeSemantics.effects` from
// `"effect": "align"` to `"effect": "none"` → BOTH loop arms fail with the
// alignment 32 → 0, symmetrically, and with ZERO diagnostics of any code.
// Deleting the row outright measures the same 32 → 0 at both positions. The
// runtime witness `examples/c/gnu_aligned_attribute/` catches that same
// demotion as `baseline exit-code mismatch (expected=42; OS=1)` — the program
// still compiles and links and simply returns the wrong number.
//
// The ASYMMETRIC regression (one position honoring, the other dropping) is what
// this pin's two-arm shape exists to catch: it reports the failing `decl` string,
// so a per-position break names itself instead of collapsing into one failure.
TEST(HirLoweringC, AfterDeclaratorAlignedAppliesLikeTheLeadingPosition) {
    for (char const* decl : {
             "__attribute__((aligned(32))) int v = 20;\n",     // leading
             "int v __attribute__((aligned(32))) = 20;\n"}) {  // trailing
        std::string const full = std::string(decl) + "int main(void){ return v; }\n";
        SemanticModel model = analyzeC(full);
        ASSERT_FALSE(model.hasErrors()) << decl;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok) << decl << ": "
                             << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
            << decl << " — `aligned` has a sink, so the clause must resolve "
                       "cleanly in BOTH positions and its argument `32` must not "
                       "be read as a linkage specifier name";
        EXPECT_EQ(globalAlignment(*res, model, "v"), 32u)
            << decl << " — the SAME attribute must APPLY the SAME alignment from "
                       "either side of the declarator; 0 means this position "
                       "silently dropped it (the original asymmetry, back), and "
                       "4 means it fell back to the natural alignment";
    }
}

// The newly-ignored ABI-NEUTRAL hint names, in the real spellings glibc / tcl.h /
// the Apple SDK actually use: dunder forms, a multi-clause comma run, nested
// double-paren args, and a keyword-argument `availability`. Every one must lower
// with ZERO H_UnknownLinkageSpecifier.
//
// This is the pin that covers BOTH new config rows at once — the `Comma`
// ignoredKind (the clause separator is a DIRECT child of `attrSpec`, so the arg
// flag does not reach it) and the added names.
//
// ★★ EVERY DECLARATION BELOW IS VALID C THAT REAL CLANG ACCEPTS, verified with
// `clang -fsyntax-only -Wall -Wextra`: zero errors, zero warnings on each. This
// matters because an attribute asserted on a shape clang REJECTS would make the
// pin claim DSS handles real C while actually witnessing the opposite. The first
// draft of this test got exactly that wrong in four rows and is recorded here so
// the shapes are not "simplified" back:
//   * `malloc` / `alloc_size(N)` require a POINTER return    (else -Wignored-attributes)
//   * `__nonnull__((1))` requires a POINTER PARAMETER at 1   (else a hard error:
//     "'__nonnull__' attribute parameter 1 is out of bounds" on `int q(void)`)
//   * `cold` and `hot` are MUTUALLY EXCLUSIVE — `((cold, hot))` is a hard error
//     ("'hot' and 'cold' attributes are not compatible"), so they get one row each
//   * `sentinel` requires a VARIADIC function            (else -Wignored-attributes)
//
// ★ `access(read_only, N)` is deliberately ABSENT even though it IS in the ignore
// list. It is a GCC-only attribute — clang answers `-Wunknown-attributes`, and no
// real GCC is available on this machine to witness it — so there is no shape that
// can be validated here. Its ignore-list entry stays justified by the GCC-
// flattened glibc headers DSS actually meets it in; it simply has no clang-clean
// witness, and a witness clang flags is worse than none.
//
// RED-ON-DISABLE (observed): remove `"Comma"` from the topLevelDecl
// `linkageSpecifierIgnoredKinds` → the multi-clause row fails with
// "',' is not a recognized linkage specifier". Remove a NAME (e.g. `pure`) →
// that row fails with "'pure' is not a recognized linkage specifier".
TEST(HirLoweringC, AbiNeutralHintAttributeNamesLowerClean) {
    for (char const* decl : {
             "__attribute__((pure)) int q(void);",
             "__attribute__((__nothrow__, __leaf__)) int q(void);",
             "__attribute__((malloc)) void *q(unsigned long n);",
             "__attribute__((alloc_size(1))) void *q(unsigned long n);",
             "__attribute__((__nonnull__((1)))) int q(const int *p);",
             "__attribute__((cold)) int q(void);",
             "__attribute__((hot)) int q(void);",
             "__attribute__((sentinel)) void q(const char *first, ...);",
             "__attribute__((availability(macos,introduced=10.12))) int q(void);"}) {
        std::string const src = std::string(decl) + "\nint main(void){ return 0; }\n";
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors()) << decl;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << decl << ": "
                             << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
            << decl << " — an ABI-neutral hint with no DSS sink must be ignored, "
                       "name and arguments alike";
        EXPECT_EQ(firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier),
                  std::string{}) << decl;
    }
}

namespace {
// The declared LINKAGE for the module decl whose bound symbol is `name`, read
// from the SPARSE linkage side-table: absent ⇒ the implicit (`Global`,
// `Default`) state — that is exactly what `recordLinkage` means by storing
// nothing, and a default-constructed `LinkageAttr` spells it — so this reports
// the APPLIED linkage for every node, annotated or not. `std::nullopt` means no
// such module decl exists at all, a distinct answer from "exists with Global
// binding", and telling those two apart is the whole point at a synthesis gate.
//
// TF-C93: this was `declaredBinding` alone; the axis-specific readers below are
// now thin projections of it so the two axes can never drift in HOW they read
// the sparse table (the absent ⇒ implicit-default rule is stated once).
[[nodiscard]] std::optional<LinkageAttr>
declaredLinkage(CstToHirResult const& res, SemanticModel const& model,
                std::string_view name) {
    for (HirNodeId d : res.hir.moduleDecls(res.hir.root())) {
        SymbolId sym{};
        switch (res.hir.kind(d)) {
            case HirKind::Global:         sym = res.hir.globalSymbol(d);         break;
            case HirKind::Function:       sym = res.hir.functionSymbol(d);       break;
            case HirKind::ExternGlobal:   sym = res.hir.externGlobalSymbol(d);   break;
            case HirKind::ExternFunction: sym = res.hir.externFunctionSymbol(d); break;
            default: continue;
        }
        auto const* rec = sym.valid() ? model.recordFor(sym) : nullptr;
        if (rec == nullptr || rec->name != name) continue;
        return res.linkageMap.has(d) ? res.linkageMap.get(d) : LinkageAttr{};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<SymbolBinding>
declaredBinding(CstToHirResult const& res, SemanticModel const& model,
                std::string_view name) {
    auto const a = declaredLinkage(res, model, name);
    if (!a.has_value()) return std::nullopt;
    return a->binding;
}

// TF-C93 (D-CSUBSET-LINKAGE-SPECIFIER-CONFLICT-SILENT-LAST-WINS, visibility
// half): the `declaredBinding` twin, and it is REQUIRED rather than convenient.
// Without it the conflict pins below could only match a message SUBSTRING — and
// a message-only assertion CANNOT SEE A WRONG RESIDUE, which is the precise bug
// the binding half's first cut was caught on (it emitted the right diagnostic
// and left the declaration marked as escaping the TU).
[[nodiscard]] std::optional<SymbolVisibility>
declaredVisibility(CstToHirResult const& res, SemanticModel const& model,
                   std::string_view name) {
    auto const a = declaredLinkage(res, model, name);
    if (!a.has_value()) return std::nullopt;
    return a->visibility;
}
}  // namespace

// ★★ TF-C73 REGRESSION PIN — THE LITERAL glibc IDIOM ON AN `extern`.
//
// TF-C73 made the after-declarator attribute run a linkage SCAN ROOT for every
// declarator-mode row. `externDecl` is one, and it carried only
// `{_Thread_local, thread_local}` + an `ExternKeyword` ignore — no
// `AttributeKeyword`, no parens, no `Comma`, no ignored names — so every raw
// token of a trailing run hit the strict lookup.
//
// MEASURED, before the config row grew its vocabulary: the first line below
// produced EIGHT H_UnknownLinkageSpecifier errors and NO object —
// `'__attribute__'`, `'('`, `'('`, `'__nothrow__'`, `','`, `'__leaf__'`, `')'`,
// `')'`. That is the expansion of glibc's `__THROW`, so it is on essentially
// every function declaration in a real libc header.
//
// GROUND TRUTH IS REAL CLANG: this exact program under
// `clang -fsyntax-only -Wall -Wextra -isysroot $(xcrun --show-sdk-path)` gives
// ZERO errors and ZERO warnings, and this compiler accepted it before TF-C73.
//
// ★ ASSERTS THE APPLIED BINDING, not "no diagnostics". A count pin would be
// satisfied by a row that went quiet the WRONG way — adding `weak` to
// `linkageSpecifierIgnoredNames` would silence all eight errors and silently
// drop `ea`'s weak binding, which is the same silent-wrong-linkage bug wearing a
// green test. Only the applied `Weak` on `ea` — beside the applied `Global` on
// its sibling `eb`, which shares the declaration and carries no attribute —
// distinguishes "resolved" from "quietly ignored".
//
// RED-ON-DISABLE (observed): strip the `externDecl` row in c.lang.json
// back to `"linkageSpecifierIgnoredKinds": ["ExternKeyword"]` with no
// `linkageSpecifierIgnoredNames` → the `gg` line fails with
// "'__attribute__' is not a recognized linkage specifier" (8 errors) and both
// EXPECTs on `gg` fail. Drop ONLY the `weak` key from that row's
// `linkageSpecifiers` → `ea` fails with "'weak' is not a recognized linkage
// specifier"; move `weak` into that row's ignored NAMES instead → the errors
// vanish and `eaBinding` silently reads Global, which is the assertion below
// that catches the wrong-way fix.
TEST(HirLoweringC, ExternDeclGlibcAttributeRunResolvesAndApplies) {
    SemanticModel model = analyzeC(
        "extern int gg(void) __attribute__((__nothrow__, __leaf__));\n"
        "extern int ea __attribute__((weak)), eb;\n"
        "int main(void){ return gg() + ea + eb; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier),
              std::string{})
        << "the glibc `__THROW` expansion must resolve on an extern declaration "
           "— any message here means a token of the attribute run reached the "
           "strict lookup";

    // The hint-decorated extern still LOWERS — `gg` must exist as an import, not
    // be dropped along with the diagnostics.
    EXPECT_EQ(declaredBinding(*res, model, "gg"),
              std::optional{SymbolBinding::Global})
        << "`extern int gg(void) __attribute__((__nothrow__, __leaf__));` must "
           "lower to an ordinary external import — nullopt means the whole "
           "declaration was lost, not merely mis-annotated";
    EXPECT_EQ(declaredBinding(*res, model, "ea"),
              std::optional{SymbolBinding::Weak})
        << "`extern int ea __attribute__((weak))` must APPLY the weak binding "
           "(clang emits `_ea` as `undefined weak external`) — Global here means "
           "the row went quiet by ignoring `weak` instead of honoring it";
    EXPECT_EQ(declaredBinding(*res, model, "eb"),
              std::optional{SymbolBinding::Global})
        << "`eb` shares the declaration but carries NO attribute — its sibling's "
           "weak binding must not leak onto it";
}

// ★★ TF-C73 REGRESSION PIN — A WEAK PROTOTYPE STILL SYNTHESIZES ITS EXTERN.
//
// The HIR-tier half of `examples/c/weak_proto_crosscu/`, which is the
// real two-TU link-and-run witness. This half exists because the runtime witness
// can only say "the program failed to build"; this one names WHY, at the exact
// node the gate decides.
//
// The `prototypeSynthesizesExtern` gate asked `binding == Global`, on the stated
// grounds that "a `static` (Local) or weak proto must never bind another TU's
// public symbol (C 6.2.2p3)". That is correct for `static` and FACTUALLY WRONG
// for `weak`: C 6.2.2 has no `weak` at all — it is a GNU attribute that changes
// LINKER RESOLUTION, not a linkage class, and a weak DECLARATION still has
// external linkage. GROUND TRUTH, real clang, two TUs: links and exits 42.
//
// ★ ASSERTS THE APPLIED SYNTHESIS, not a diagnostic count. The broken gate
// emitted NOTHING at this tier — it simply declined to create the node, and the
// program died much later at HIR→MIR with H0009 "Ref to unbound symbol". So the
// fact to pin is that the ExternFunction EXISTS (`declaredBinding` returns a
// value rather than `nullopt`) and that it carries the WEAK binding it was
// declared with — a node that existed but had lost its weakness would be the
// other half of the same bug.
//
// ★ BOTH SPELLINGS. The trailing one regressed in TF-C73; the LEADING one
// reached the same gate through the specifier prefix and was broken identically
// since the gate was written (MEASURED at the pre-TF-C73 HEAD: the leading
// spelling failed H0009 there while the trailing one ran to 42). One gate, two
// spellings — pinning only the regressed spelling leaves the older half open.
//
// RED-ON-DISABLE (observed): restore the gate to
// `protoLinkage().binding == SymbolBinding::Global` → BOTH `declaredBinding`
// calls return `std::nullopt` (no ExternFunction node is created at all) and
// both EXPECTs fail.
TEST(HirLoweringC, WeakPrototypeSynthesizesExternInBothPositions) {
    SemanticModel model = analyzeC(
        "int wtrail(int) __attribute__((weak));\n"
        "__attribute__((weak)) int wlead(int);\n"
        "int main(void){ return wtrail(30) + wlead(1); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(declaredBinding(*res, model, "wtrail"),
              std::optional{SymbolBinding::Weak})
        << "the TRAILING `__attribute__((weak))` proto must still synthesize its "
           "extern (nullopt = no node, so nothing for a sibling TU's definition "
           "to bind to) AND keep the weak binding it declared";
    EXPECT_EQ(declaredBinding(*res, model, "wlead"),
              std::optional{SymbolBinding::Weak})
        << "the LEADING `__attribute__((weak))` proto must behave identically — "
           "the fix is to the RULE, not to one spelling of it";
}

// ★★ TF-C73 REGRESSION PIN — `static` + `weak` IS A CONFLICT, NOT LAST-WINS.
//
// MEASURED, before the fix, on `static int x __attribute__((weak)) = 7;`:
// the ELF object carried `V x` — a WEAK GLOBAL under its own unmangled name,
// escaping the TU — where the same declaration without the attribute emits
// `d sym_N`, a TU-local. ZERO diagnostics, exit code 0. Silent wrong linkage:
// the program links against a different definition than the source asked for.
//
// GROUND TRUTH IS REAL CLANG, which HARD-ERRORS
// `weak declaration cannot have internal linkage` — so the fix follows clang and
// fails loud rather than picking either binding.
//
// ★ THE RULE IS SYMMETRIC, and both orders are pinned because clang errors on
// both: `static __attribute__((weak)) int x;` AND
// `__attribute__((weak)) static int x;`. A "trailing must not clobber leading"
// precedence rule would pass the first and silently accept the second.
//
// ★ ASSERTS THE APPLIED BINDING BESIDE THE MESSAGE, not a count. The binding is
// the fact that was silently wrong (`Weak` where the source said `static`), and
// `linkageFrom` has several producers of H_UnknownLinkageSpecifier, so a count
// says nothing about WHICH fired or what binding survived. `Local` is the only
// answer that means "the `static` was not overwritten".
//
// ★★ AND `Local` IN *BOTH* ARMS IS WHY THIS PIN IS A LOOP RATHER THAN ONE CASE.
// The first cut of the guard rejected the conflict and simply left the binding
// already in force standing — plain first-wins. MEASURED, that passed the
// trailing arm (Local was first) and FAILED this one on the leading arm, which
// read `Weak`: a rejected declaration still marked as escaping the TU. So the
// guard now resolves a rejected conflict to the CONFINING binding, and pinning
// both orders to the same value is what keeps that symmetric.
//
// RED-ON-DISABLE (observed): delete the conflict guard in `linkageFrom` (restore
// the bare `attr.binding = *it->second.binding;` last-wins assignment) → the
// message goes to EMPTY and the binding reads `Weak` in both arms, i.e. straight
// back to the silent escape.
TEST(HirLoweringC, WeakOnInternalLinkageFailsLoudAndKeepsStatic) {
    struct Case { char const* decl; char const* conflicting; };
    for (Case const c : {
             Case{"static int x __attribute__((weak)) = 7;\n", "weak"},
             Case{"__attribute__((weak)) static int x = 7;\n", "static"}}) {
        std::string const src = std::string(c.decl)
                              + "int *px = &x;\n"
                                "int main(void){ return *px; }\n";
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors()) << c.decl;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        // The message names the SECOND-folded specifier and the binding already
        // in force, so the two arms report different (both correct) texts —
        // matching on those two substrings keeps the pin order-symmetric without
        // going vague about WHICH of `linkageFrom`'s several
        // H_UnknownLinkageSpecifier producers fired.
        std::string const msg =
            firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier);
        EXPECT_NE(msg.find(std::string{"'"} + c.conflicting + "' specifies '"),
                  std::string::npos)
            << c.decl << " — got: \"" << msg << "\"; an EMPTY message means the "
                         "conflict went back to last-wins, which is the "
                         "silent-escape bug";
        EXPECT_NE(msg.find("conflicts with"), std::string::npos)
            << c.decl << " — got: \"" << msg << "\"; must be the CONFLICT "
                         "diagnostic, not the generic unrecognized-key "
                         "fall-through";
        EXPECT_EQ(declaredBinding(*res, model, "x"),
                  std::optional{SymbolBinding::Local})
            << c.decl << " — the `static` must SURVIVE the rejected `weak`: "
                         "Weak here means the symbol escaped the TU (measured as "
                         "`V x` in the ELF object), Global means both were lost";
    }
}

// ★★★ P42 — A KEYWORD-SPELLED ATTRIBUTE CLAUSE NAME IS NEVER A LINKAGE
// SPECIFIER. Anchor:
// D-C-LINKAGE-SPECIFIER-LOOKUP-IS-POSITION-BLIND-AND-NOT-DUNDER-NORMALIZED
//
// ⚠ THIS PIN GUARDS A SILENT MISCOMPILE THAT WAS LIVE AT THE PRE-CHANGE TREE.
// ✔MEASURED through the shipped CLI, reading the emitted ELF object with `nm`:
//     `int gv = 1;`                         → `gv D`      (exported)
//     `static int gv = 1;`                  → `sym_84 d`  (TU-local)
//     `__attribute__((static)) int gv = 1;` → `sym_84 d`  ← rc=0, ZERO diagnostics
// The third line is the defect, and it is the worst outcome this project
// recognises: the name `gv` simply LEFT the object, so another TU links against
// a different definition or fails to link, and nothing was said. The cause is
// that ONE `linkageSpecifiers` map holds two vocabularies — declaration-specifier
// KEYWORDS and attribute NAMES — and the lookup was position-blind, so a keyword
// worn as an attribute clause name reached the keyword's own entry.
// ✔BOTH REFERENCES PROBED SEPARATELY (gcc 13.3 `-std=c2x`/`-std=gnu2x`, clang
// 18.1.3 `-std=c23`/`-std=gnu23`, `-Wall -Wextra`): each merely WARNS and IGNORES
// the attribute — gcc `'static' attribute directive ignored`, clang
// `unknown attribute 'static' ignored` — so both keep `gv` exported.
//
// ★ WHY THE RESIDUE IS A LOUD REFUSAL RATHER THAN THE REFERENCES' WARN-AND-
// IGNORE: this tier has no Warning-severity route at all (`emitH` hardcodes
// `DiagnosticSeverity::Error`, and there are ZERO `DiagnosticSeverity::Warning`
// uses in cst_to_hir.cpp), so warn-and-ignore needs a diagnostic code this file
// set does not own. It is the SAME missing tier the whole file-scope
// unknown-name gate needs. Until that lands, LOUD is the correct residue and
// SILENT WRONG LINKAGE is not — the refusal is visible, the rebinding was not.
//
// ★ THE THREE DUNDER ARMS ARE THE NEGATIVE DIRECTION OF THE SAME MECHANISM, and
// they are why the keyword test asks about the DERIVED name and not the raw
// spelling: normalization must reach `__weak__`→`weak` WITHOUT reaching
// `__static__`→`static`. A first cut that tested the raw text was BUILT AND
// MEASURED to let all three of these through — `__attribute__((__static__))`
// went rc=1 → rc=0, silently internal.
TEST(HirLoweringC, KeywordSpelledAttributeNameIsNeverALinkageSpecifier) {
    for (char const* spec : {"__attribute__((static))",
                             "__attribute__((__static__))",
                             "__attribute__((__thread_local__))",
                             "__attribute__((__constexpr__))"}) {
        std::string const src = std::string(spec)
            + " int gv = 1;\n"
              "int *pgv = &gv;\n"
              "int main(void){ return *pgv - 1; }\n";
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors()) << spec;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
            << spec << " — exactly one loud refusal. ZERO means a keyword bound "
                       "through the attribute position again";
        EXPECT_EQ(declaredBinding(*res, model, "gv"),
                  std::optional{SymbolBinding::Global})
            << spec << " — THE assertion of this pin, because a diagnostic count "
                       "alone cannot see a wrong RESIDUE. `Local` is exactly the "
                       "`sym_84 d` that made the symbol vanish; `nullopt` would "
                       "mean the declaration was lost entirely";
    }
}

// The control for the pin above, and it is required rather than tidy: the same
// mechanism must leave the REAL keyword alone. `static int gv = 1;` is a BARE
// declaration specifier, not an attribute name, so it must still bind Local and
// stay silent. Had the keyword denial been written on the SPELLING instead of on
// the POSITION, this goes red — which is the whole reason the position mark
// exists.
TEST(HirLoweringC, BareStorageClassKeywordStillBindsThroughLinkageSpecifiers) {
    SemanticModel model = analyzeC(
        "static int gv = 1;\n"
        "int *pgv = &gv;\n"
        "int main(void){ return *pgv - 1; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u);
    EXPECT_EQ(declaredBinding(*res, model, "gv"),
              std::optional{SymbolBinding::Local})
        << "the bare `static` keyword must keep resolving in `linkageSpecifiers`";
}

// P42 — the dunder spellings in the OBJECT and after-`extern` positions, read
// through the linkage side-table rather than through a diagnostic count. The
// differential pin earlier in this file covers the leading-on-a-function shape;
// these are the two shapes REAL headers write — glibc puts the attribute after
// `extern`, tcl.h puts it in the declaration head.
TEST(HirLoweringC, GnuDunderLinkageSpellingsApplyInObjectAndExternPositions) {
    SemanticModel model = analyzeC(
        "__attribute__((__weak__)) int g_w = 1;\n"
        "extern int e_w __attribute__((__weak__));\n"
        "extern int e_v __attribute__((__visibility__(\"hidden\")));\n"
        "int main(void){ return g_w + e_w + e_v - 1; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "all three are rc=0 with zero diagnostics on gcc 13.3 and clang "
           "18.1.3, probed separately with per-compiler std flags";
    EXPECT_EQ(declaredBinding(*res, model, "g_w"),
              std::optional{SymbolBinding::Weak})
        << "`__weak__` must APPLY the weak binding, not merely parse — `Global` "
           "here is the SILENT DROP, which this project ranks below the loud "
           "refusal it replaced (measured: the plain spelling emits `gv V`)";
    EXPECT_EQ(declaredBinding(*res, model, "e_w"),
              std::optional{SymbolBinding::Weak})
        << "the after-`extern` run is a DIFFERENT scan root with its own map";
    EXPECT_EQ(declaredVisibility(*res, model, "e_v"),
              std::optional{SymbolVisibility::Hidden})
        << "the COMPOSITE key survives normalization of its name half: the "
           "derived key is `visibility:hidden`, not `__visibility__:hidden`";
}

// The other side of the same seam: an attribute that touches NO axis the prefix
// set must leave the prefix's binding alone. `aligned` is axis-free for linkage
// (its sink is the alignment side-table), so `static int sa
// __attribute__((aligned(32)));` must stay LOCAL and stay SILENT — the conflict
// guard above must not have turned "a trailing attribute is present" into a
// rejection. Clang accepts this exact declaration with zero warnings.
//
// RED-ON-DISABLE: widen the conflict guard to fire whenever a trailing run is
// folded at all (rather than only on a differing BINDING) → this reds with a
// spurious H_UnknownLinkageSpecifier on legal C.
TEST(HirLoweringC, AxisFreeTrailingAttributeLeavesStaticBindingIntact) {
    SemanticModel model = analyzeC(
        "static int sa __attribute__((aligned(32))) = 3;\n"
        "int *psa = &sa;\n"
        "int main(void){ return *psa; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier),
              std::string{})
        << "an axis-free trailing attribute on a `static` is legal C";
    EXPECT_EQ(declaredBinding(*res, model, "sa"),
              std::optional{SymbolBinding::Local})
        << "`static` must survive an attribute that sets no binding";
    EXPECT_EQ(globalAlignment(*res, model, "sa"), 32u)
        << "and the alignment must still APPLY — a guard that rejected the "
           "clause would also have dropped its effect";
}

// ★★ TF-C93 REGRESSION PIN — TWO CONFLICTING `visibility` VALUES ON ONE
// DECLARATION IS A CONFLICT, NOT LAST-WINS. Closes the VISIBILITY half of
// D-CSUBSET-LINKAGE-SPECIFIER-CONFLICT-SILENT-LAST-WINS; the BINDING half above
// (TF-C73) is the precedent, down to reusing H_UnknownLinkageSpecifier.
//
// MEASURED at HEAD 199fe7d, `--target arm64:macho64-arm64-darwin-dylib`, and the
// defect is BYTE-OBSERVABLE rather than merely theoretical — RC=0 and ZERO
// diagnostics in BOTH orders, while the shipped image's dynamic export table
// FLIPS on source order:
//   `visibility("hidden")` then `visibility("default")` on `int dv4`
//       → `dyld_info -exports` lists `_dv4`  (EXPORTED)
//   the same two specifiers SWAPPED
//       → `_dv4` absent from the export trie (HIDDEN)
// (`nm -m` on that dylib shows only `_main`; the DATA symbol is visible in the
// export trie, so `dyld_info -exports` is the tool that sees this — noted because
// an earlier write-up cited `nm -m` and it does not show it.)
//
// GROUND TRUTH IS REAL CLANG on this Mac, which HARD-ERRORS BOTH orders with
// `error: visibility does not match previous declaration` and emits no object.
// GCC is documented to warn and keep the EARLIER value. Plain last-wins is
// NEITHER behaviour, so failing loud follows the stricter reference.
//
// ★★★ THE LOOP IS OVER BOTH ORDERS × FIVE SYNTACTIC POSITIONS, AND THE ORDER
// SYMMETRY IS THE POINT. The binding guard's trick — `attr.binding != Global`
// meaning "already specified" — does NOT transfer: `SymbolVisibility::Default`
// is BOTH the unspecified sentinel AND a writable config value (TF-C92 added
// `visibility:default`). A naive `!= Default` mirror fires on `hidden`→`default`
// and SILENTLY FOLDS `default`→`hidden`, so the `default`-FIRST arms are the ones
// that actually pin the explicit `visibilitySpecified` bit.
//
// ★ EACH ARM ASSERTS THE RESIDUE VIA `declaredVisibility` BESIDE THE MESSAGE.
// A message-only assertion cannot see a wrong residue — exactly the bug the
// binding half's first cut was caught on. The residue is `Hidden` in every arm
// (the CONFINING value, derived from the shipped `isExternallyVisible`
// predicate), which is what makes the rule symmetric in VALUE and not only in
// when it fires.
//
// ★ NO COUNT ASSERTION HERE (see the binding pin's note): `linkageFrom` has
// several H_UnknownLinkageSpecifier producers, so a count says nothing about
// WHICH fired. The arm-specific `'<second>' specifies '` substring identifies the
// producer AND the arm; `conflicts with` separates it from the generic
// unrecognized-key fall-through. The count-of-one property has its own test.
//
// ★ NO DUNDER SPELLINGS. MEASURED: the strict lookup uses RAW token text, and
// `stripDunder` applies only to the ignored-NAMES check, so
// `__attribute__((__visibility__("hidden")))` reports `'__visibility__:hidden' is
// not a recognized linkage specifier`. A dunder arm would be red for the wrong
// reason (that loud rejection of legal GNU C is a separate, separately-anchored
// gap — deliberately NOT fixed here).
//
// RED-ON-DISABLE — THREE variants, each BUILT AND RUN, results as OBSERVED (not
// predicted; the second prediction below was wrong and is corrected here):
//
//   (i) restore the bare `attr.visibility = *it->second.visibility;` → all 8
//       arms report an EMPTY message, and the 4 `hidden`-FIRST arms ALSO read the
//       wrong residue (`Default`, i.e. still marked EXPORTED). The
//       `default`-FIRST arms keep residue `Hidden` by last-wins COINCIDENCE —
//       which is exactly why this pin asserts the MESSAGE as well as the residue.
//
//  (ii) delete ONLY the `attr.visibilitySpecified = true;` write → ALL 8 arms go
//       silent. ★ THIS IS NOT THE ASYMMETRY DEMO, and an earlier write-up
//       predicted that it was. With the write gone the flag is never true, so the
//       guard never fires at all — a strict subset of (i). Recorded because the
//       wrong prediction is easy to re-derive.
//
// (iii) ★★ THE ASYMMETRY DEMO, and the one that justifies the design. Keep
//       everything and swap ONLY the guard KEY to the naive mirror of the binding
//       half, `attr.visibility != SymbolVisibility::Default` → all four
//       `hidden`→`default` arms STILL FIRE and PASS, while all four
//       `default`→`hidden` arms go SILENT (`two specifiers`, `one clause list`,
//       `prefix + after-declarator`, and `extern`). `...ReportsExactlyOnce` and
//       the negative-control test both stay GREEN under it, because their
//       conflicts are `hidden`-first. So ONLY a both-orders loop can see this
//       half-fix — and the half it leaves silent is the half that REMOVES a
//       symbol from the export table.
TEST(HirLoweringC, VisibilityConflictFailsLoudAndKeepsConfiningVisibility) {
    // `second` is the composite KEY of the later-folded specifier, so the two
    // orders report different (both correct) texts and neither arm can pass on
    // the other's message.
    struct Case { char const* what; char const* decl; char const* second; };
    for (Case const c : {
             // (1) two separate `__attribute__` specifiers, file scope.
             Case{"two specifiers, hidden→default",
                  "__attribute__((visibility(\"hidden\")))"
                  " __attribute__((visibility(\"default\"))) int x = 7;\n",
                  "visibility:default"},
             Case{"two specifiers, default→hidden",
                  "__attribute__((visibility(\"default\")))"
                  " __attribute__((visibility(\"hidden\"))) int x = 7;\n",
                  "visibility:hidden"},
             // (2) ONE clause LIST inside ONE `__attribute__`. This form is NOT
             // vacuous: `linkageFrom` DOES walk clause 2 and DOES form its
             // composite key from there — MEASURED three ways at HEAD
             // (`visibility("hidden"),bogus_xyz` → H000C on `bogus_xyz`;
             // `visibility("hidden"),visibility("protected")` → H000C
             // `'visibility:protected'`; and the shipped BINDING guard already
             // fires from this exact position via
             // `static int q __attribute__((visibility("hidden"),weak))`).
             Case{"one clause list, hidden→default",
                  "__attribute__((visibility(\"hidden\"),"
                  "visibility(\"default\"))) int x = 7;\n",
                  "visibility:default"},
             Case{"one clause list, default→hidden",
                  "__attribute__((visibility(\"default\"),"
                  "visibility(\"hidden\"))) int x = 7;\n",
                  "visibility:hidden"},
             // (3) THE CROSS-SEED PATH — prefix specifier + after-declarator
             // specifier on the SAME declarator. This is what justifies
             // `visibilitySpecified` being a `LinkageAttr` FIELD rather than a
             // local: `declaratorLinkage` seeds the trailing fold with the
             // prefix's already-folded attribute, so the bit must survive that
             // boundary. Without these two arms the field's necessity is
             // untested.
             Case{"prefix + after-declarator, hidden→default",
                  "__attribute__((visibility(\"hidden\"))) int x"
                  " __attribute__((visibility(\"default\"))) = 7;\n",
                  "visibility:default"},
             Case{"prefix + after-declarator, default→hidden",
                  "__attribute__((visibility(\"default\"))) int x"
                  " __attribute__((visibility(\"hidden\"))) = 7;\n",
                  "visibility:hidden"}}) {
        std::string const src = std::string(c.decl)
                              + "int *px = &x;\n"
                                "int main(void){ return *px; }\n";
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors()) << c.what;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        std::string const msg =
            firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier);
        EXPECT_NE(msg.find(std::string{"'"} + c.second + "' specifies '"),
                  std::string::npos)
            << c.what << " — got: \"" << msg << "\"; an EMPTY message means the "
                         "conflict went back to last-wins, which is the "
                         "silent export-table flip this closes";
        EXPECT_NE(msg.find("conflicts with"), std::string::npos)
            << c.what << " — got: \"" << msg << "\"; must be the CONFLICT "
                         "diagnostic, not the generic unrecognized-key "
                         "fall-through";
        EXPECT_EQ(declaredVisibility(*res, model, "x"),
                  std::optional{SymbolVisibility::Hidden})
            << c.what << " — the CONFINING visibility must be the residue in "
                         "BOTH orders: `Default` here means the rejected "
                         "declaration is still marked EXPORTED (measured as "
                         "`_dv4` present in the Mach-O export trie)";
    }
}

// The `externDecl` position of the same conflict — a SEPARATE row in
// `c.lang.json` with its own `linkageSpecifiers` map (TF-C92 added
// `visibility:default` to BOTH rows, so BOTH became silently last-wins). One row
// is not evidence for the other: they are independent config, and the `extern`
// row is the one tcl.h actually reaches (`#define EXTERN extern
// TCL_STORAGE_CLASS` puts the attribute after `extern`, into `externSpecifiers`).
// MEASURED silent (RC=0) at HEAD in both orders.
TEST(HirLoweringC, ExternDeclVisibilityConflictFailsLoudBothOrders) {
    struct Case { char const* what; char const* decl; char const* second; };
    for (Case const c : {
             Case{"extern, hidden→default",
                  "extern int e __attribute__((visibility(\"hidden\")))"
                  " __attribute__((visibility(\"default\")));\n",
                  "visibility:default"},
             Case{"extern, default→hidden",
                  "extern int e __attribute__((visibility(\"default\")))"
                  " __attribute__((visibility(\"hidden\")));\n",
                  "visibility:hidden"}}) {
        std::string const src = std::string(c.decl)
                              + "int *pe = &e;\n"
                                "int main(void){ return *pe; }\n";
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors()) << c.what;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        std::string const msg =
            firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier);
        EXPECT_NE(msg.find(std::string{"'"} + c.second + "' specifies '"),
                  std::string::npos)
            << c.what << " — got: \"" << msg << "\"";
        EXPECT_NE(msg.find("conflicts with"), std::string::npos)
            << c.what << " — got: \"" << msg << "\"";
        EXPECT_EQ(declaredVisibility(*res, model, "e"),
                  std::optional{SymbolVisibility::Hidden})
            << c.what << " — the confining residue must hold on the extern row "
                         "too; nullopt would mean no ExternGlobal node exists "
                         "at all, so there is nothing carrying the linkage";
    }
}

// ★ THE ONE PLACE A COUNT *IS* THE PROPERTY. `linkageFrom` folds the shared
// declaration PREFIX once and each declarator's TRAILING run separately — a
// design whose stated purpose is that one typo in
// `__attribute__((frobnicate)) int a, b, c;` reports ONCE, not three times. The
// conflict guard must not break that: the prefix `hidden` is seeded into EVERY
// declarator's fold, so a guard that re-emitted per declarator would report the
// conflict on `a` (which carries no trailing attribute at all) as well as on `b`.
// EXACTLY ONE diagnostic, and it must name the declarator that really conflicts.
TEST(HirLoweringC, VisibilityConflictOnSecondDeclaratorReportsExactlyOnce) {
    SemanticModel model = analyzeC(
        "__attribute__((visibility(\"hidden\"))) int a = 1,"
        " b __attribute__((visibility(\"default\"))) = 2;\n"
        "int *pa = &a; int *pb = &b;\n"
        "int main(void){ return *pa + *pb; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
        << "the conflict lives on the SECOND declarator only — the prefix fold "
           "is shared, so a per-declarator re-emission would report it on `a` "
           "too; got: \""
        << firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier) << "\"";
    EXPECT_NE(firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier)
                  .find("'visibility:default' specifies '"),
              std::string::npos);
    EXPECT_EQ(declaredVisibility(*res, model, "a"),
              std::optional{SymbolVisibility::Hidden})
        << "`a` takes the prefix `hidden` cleanly — it is not part of the "
           "conflict and must not be collateral";
    EXPECT_EQ(declaredVisibility(*res, model, "b"),
              std::optional{SymbolVisibility::Hidden})
        << "`b`'s rejected `default` leaves the confining `hidden` standing";
}

// ★★ TF-C93 — THE `static` ARM, AND THE ONLY THING THAT PINS THE
// `SymbolBinding::Global` ARGUMENT.
//
// The residue rule asks the SHIPPED `isExternallyVisible` predicate which of two
// conflicting candidates is CONFINING, and it deliberately passes
// `SymbolBinding::Global` rather than `attr.binding`. The reasoning lives beside the
// call in `cst_to_hir.cpp`: the predicate SHORT-CIRCUITS to false on `Local`, so
// under a co-present `static` the REAL binding makes both candidates compare EQUAL,
// which collapses the rule to "keep the incumbent" — and the incumbent is whichever
// specifier happened to be folded FIRST. That is order dependence reintroduced
// through an axis (storage class) that has nothing to do with visibility, inside the
// one guard whose whole purpose is order SYMMETRY.
//
// ⚠ EVERY OTHER ARM IN THIS FILE IS BLIND TO IT, WHICH IS WHY THIS TEST EXISTS. All
// nine arms of the three pins above declare NO storage class, so `attr.binding` is
// already `Global` there and the literal is indistinguishable from the field.
// MEASURED: swapping the literal to `attr.binding` leaves all nine GREEN.
//
// ★ THE DIAGNOSTIC FIRES IN BOTH ORDERS EITHER WAY (MEASURED), so a message-only
// assertion is blind here too — the error is emitted BEFORE the residue is computed.
// Only `declaredVisibility` discriminates; the message and binding checks are here to
// stop the arm passing vacuously (wrong producer, or a `static` that never applied).
//
// RED-ON-DISABLE, BUILT AND RUN: swap both `SymbolBinding::Global` literals to
// `attr.binding` → the `default`→`hidden` arm reads residue `Default` (both
// candidates non-exporting under `Local` ⇒ keep the incumbent ⇒ keep `default`,
// i.e. still marked EXPORTED) while the `hidden`→`default` arm stays GREEN by
// coincidence. Half-green under the wrong argument is exactly why this needs BOTH
// orders and not one probe.
TEST(HirLoweringC, StaticVisibilityConflictResidueIsBindingIndependent) {
    struct Case { char const* what; char const* decl; char const* second; };
    for (Case const c : {
             Case{"static, hidden→default",
                  "static __attribute__((visibility(\"hidden\")))"
                  " __attribute__((visibility(\"default\"))) int x = 7;\n",
                  "visibility:default"},
             Case{"static, default→hidden",
                  "static __attribute__((visibility(\"default\")))"
                  " __attribute__((visibility(\"hidden\"))) int x = 7;\n",
                  "visibility:hidden"}}) {
        SCOPED_TRACE(c.what);
        std::string const src = std::string(c.decl)
                              + "int *px = &x;\n"
                                "int main(void){ return *px; }\n";
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors()) << c.what;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        std::string const msg =
            firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier);
        EXPECT_NE(msg.find(std::string{"'"} + c.second + "' specifies '"),
                  std::string::npos)
            << c.what << " — got: \"" << msg << "\"; the conflict must still be "
                         "REPORTED under a co-present storage class, or this arm "
                         "is measuring a different code path than its siblings";
        EXPECT_EQ(declaredBinding(*res, model, "x"),
                  std::optional{SymbolBinding::Local})
            << c.what << " — the `static` must actually have taken effect; without "
                         "`Local` on the record this arm is vacuous and pins "
                         "nothing about the predicate's short-circuit";
        EXPECT_EQ(declaredVisibility(*res, model, "x"),
                  std::optional{SymbolVisibility::Hidden})
            << c.what << " — ★ THE ASSERTION THIS TEST EXISTS FOR. `Default` here "
                         "means the residue rule consulted the REAL binding, "
                         "short-circuited on `Local`, found both candidates equal, "
                         "and kept whichever specifier came first — order "
                         "dependence through the storage-class axis";
    }
}

// NEGATIVE CONTROLS — the guard must be IDEMPOTENT and PER-AXIS, or it turns
// legal C loud. Both shapes below are accepted by real clang with zero warnings
// under -Wall -Wextra, and both were MEASURED silent at HEAD (so a regression
// here is caused by the fix, never inherited).
//
//   • the SAME visibility twice is a re-fold, not a conflict — this is the arm
//     that forbids keying the guard on "a trailing run was folded at all";
//   • `visibility("hidden")` + `weak` touch DIFFERENT axes, so the visibility
//     guard must stay silent while the binding is applied. Asserting the applied
//     `Weak` beside the silence is what distinguishes "per-axis" from "the whole
//     attribute was quietly dropped".
TEST(HirLoweringC, RepeatedAndCrossAxisVisibilitySpecifiersStaySilent) {
    struct Case { char const* what; char const* decl; };
    for (Case const c : {
             Case{"hidden twice",
                  "__attribute__((visibility(\"hidden\")))"
                  " __attribute__((visibility(\"hidden\"))) int x = 7;\n"},
             Case{"default twice",
                  "__attribute__((visibility(\"default\")))"
                  " __attribute__((visibility(\"default\"))) int x = 7;\n"}}) {
        std::string const src = std::string(c.decl)
                              + "int *px = &x;\n"
                                "int main(void){ return *px; }\n";
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors()) << c.what;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << c.what << ": "
                             << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier),
                  std::string{})
            << c.what << " — re-folding the SAME value is idempotent, not a "
                         "conflict";
    }
    // Per-axis: one visibility + one binding on one declaration.
    for (char const* decl : {
             "__attribute__((visibility(\"hidden\")))"
             " __attribute__((weak)) int x = 7;\n",
             "__attribute__((weak))"
             " __attribute__((visibility(\"hidden\"))) int x = 7;\n"}) {
        std::string const src = std::string(decl)
                              + "int *px = &x;\n"
                                "int main(void){ return *px; }\n";
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors()) << decl;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << decl << ": "
                             << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(firstMessageFor(r, DiagnosticCode::H_UnknownLinkageSpecifier),
                  std::string{})
            << decl << " — `visibility` and `weak` are different axes; a guard "
                       "that fired here would reject legal C";
        EXPECT_EQ(declaredVisibility(*res, model, "x"),
                  std::optional{SymbolVisibility::Hidden}) << decl;
        EXPECT_EQ(declaredBinding(*res, model, "x"),
                  std::optional{SymbolBinding::Weak})
            << decl << " — the silence must come from the axes being "
                       "independent, NOT from the attribute being dropped";
    }
}

// ── FC5: goto / labels ──────────────────────────────────────────────────────

namespace {
[[nodiscard]] std::size_t countKind(Hir const& h, HirKind k) {
    std::size_t n = 0;
    std::uint32_t const tag = h.id().v;
    for (std::uint32_t i = 1; i < h.nodeCount(); ++i)
        if (h.kind(HirNodeId{i, tag}) == k) ++n;
    return n;
}
}  // namespace

// FC5 — `goto`/labels lower to the new GotoStmt/LabelStmt kinds, and a function
// whose ONLY return is reached through a forward goto + a label still lowers
// clean (pathTerminates treats `goto` as a terminator and a LabelStmt as
// transparent — a labeled `return` terminates). Red-on-disable: if LabelStmt
// transparency in `pathTerminates` were reverted, the body would look like it
// falls through and `lowerToHir` would fail H_VerifierFailure.
TEST(HirLoweringC, GotoAndLabelLowerCleanAndTerminateViaLabel) {
    SemanticModel model = analyzeC(
        "int f(int c){ if(c) goto a; return 1; a: return 2; } "
        "int main(){ return f(0); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_VerifierFailure), 0u)
        << "a goto+labeled-return function must not look like it falls through";
    EXPECT_EQ(countKind(res->hir, HirKind::GotoStmt), 1u);
    EXPECT_EQ(countKind(res->hir, HirKind::LabelStmt), 1u);

    // A function that ONLY terminates via a labeled return (`goto a; a: ...`).
    SemanticModel m2 = analyzeC(
        "int g(){ goto a; a: return 5; } int main(){ return g(); }");
    ASSERT_FALSE(m2.hasErrors());
    DiagnosticReporter r2;
    auto res2 = lowerToHir(m2, r2);
    EXPECT_TRUE(res2->ok) << (r2.all().empty() ? "" : r2.all()[0].actual);
    EXPECT_EQ(countCode(r2, DiagnosticCode::H_VerifierFailure), 0u);
}

// ── D-CSUBSET-BLOCK-TERMINATION-LAST-REACHABLE ───────────────────────────────
//
// `pathTerminates` decides a block's termination by its LAST REACHABLE statement,
// not its literal last child. A trailing dead statement AFTER a real terminator
// (the `MACRO(...);` null-statement idiom that lowers `;` to an empty Block placed
// AFTER the `return`) previously made the body read as fall-through → a spurious
// H_VerifierFailure (H0003) that gcc/clang never emit. These pins assert the four
// CLEAN shapes emit ZERO H_VerifierFailure. Red-on-disable: revert the Block case
// to `return !kids.empty() && pathTerminates(src, kids.back())` and every case
// here goes RED (the empty trailing Block reads as non-terminating).
[[nodiscard]] std::size_t hirVerifierFailures(std::string src) {
    SemanticModel model = analyzeC(std::move(src));
    EXPECT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    (void)res;
    return countCode(r, DiagnosticCode::H_VerifierFailure);
}

TEST(HirLoweringC, DeadTailAfterTerminatorDoesNotReadAsFallThrough) {
    // The minimal trailing null statement — `int m(void){ return 0; ; }`.
    EXPECT_EQ(hirVerifierFailures("int m(void){ return 0; ; }"), 0u)
        << "a `;` after `return 0;` (empty trailing Block) must NOT read as "
           "fall-through — the block terminates at its last REACHABLE statement";
    // The RECOVER_VFS_WRAPPER shape: every path returns via the if/else, then a
    // trailing `;` (the `MACRO(...);` null statement) follows the `return rc;`.
    EXPECT_EQ(hirVerifierFailures(
                  "int w(int x){ int rc=0; if(x){rc=1;}else{rc=2;} return rc; ; }"),
              0u)
        << "trailing `;` after the real terminator must not spuriously fall through";
    // General precision: a dead NON-label call after a return — `return 0; d2();`.
    EXPECT_EQ(hirVerifierFailures("int d2(void); int d(void){ return 0; d2(); }"), 0u)
        << "dead non-label code after a terminator makes later positions "
           "unreachable — the block still terminates";
    // Both if/else arms return, then a dead trailing `;`.
    EXPECT_EQ(hirVerifierFailures("int e(int x){ if(x){return 1;}else{return 2;} ; }"), 0u)
        << "a both-arms-return if terminates; a trailing `;` does not undo that";
}

// The negative miscompile-pins: genuine fall-through MUST still fail loud
// (H_VerifierFailure). These stay RED when the fix is present (they are the
// behavior that must NOT change) — and are how a WRONG fix is caught.
TEST(HirLoweringC, GenuineFallThroughStillFailsLoudIncludingNestedLabel) {
    // A non-void function that just calls a void fn and falls off the end.
    EXPECT_GE(hirVerifierFailures("void foo(void); int g(int x){ foo(); }"), 1u)
        << "a genuine fall-through must still emit H_VerifierFailure";
    // An `if` with no `else` — the then-arm may be skipped, so control falls off.
    EXPECT_GE(hirVerifierFailures("int h(int x){ if(x) return 1; }"), 1u)
        << "if-without-else does not terminate on the fall-through path";
    // A direct fall-through THROUGH a label: `goto L` may reach `L:` which then
    // falls off the end. The label re-establishes reachability at the dead tail.
    EXPECT_GE(hirVerifierFailures("int lbl(int x){ if(x) goto L; return 0; L: ; }"), 1u)
        << "a labeled statement after the terminator is a goto re-entry point — "
           "the block can still fall through via the label";
    // ★ THE NESTED-LABEL SOUNDNESS GUARD (the audit's mandatory addition). The
    // dead tail is an `if` whose BODY contains the label `L:`. `goto L` can re-
    // enter that nested label and then fall off the end — a GENUINE fall-through.
    // Only the `subtreeContainsLabel` (deep) reachability reset catches this; the
    // UNSOUND version (reset only at a DIRECT LabelStmt child) would see the `if`
    // as a plain dead child, keep `reachable=false`, and WRONGLY accept the body
    // (a silent miscompile). This pin goes RED against the unsound version.
    EXPECT_GE(hirVerifierFailures(
                  "int nested(int x){ if(x) goto L; return 0; if(x){ L: ; } }"),
              1u)
        << "a label nested inside a dead-tail statement is still a goto re-entry "
           "point — the block genuinely falls through and must fail loud";
}

// VLA C5 (D-CSUBSET-VLA, C99 6.8.6.1p1): a `goto` that jumps INTO the scope of a
// variable-length array, bypassing its declaration, is FAILED LOUD at HIR verify
// (H_VlaJumpIntoScope). `goto L` sits BEFORE `int a[n]`; `L:` sits AFTER it, so
// arriving at L skips the array's runtime allocation — undefined storage. This is
// ALSO the dominance guarantor for the C5 teardown. Red-on-disable: drop
// checkVlaJumpScoping and this compiles (a silent jump into unallocated dynamic
// stack). A LEGAL goto OUT of a VLA scope stays clean (asserted second).
TEST(HirLoweringC, GotoIntoVlaScopeFailsLoud) {
    SemanticModel model = analyzeC(
        "int main(void){ volatile int vn = 4; int n = vn;\n"
        "  goto L;\n"
        "  int a[n];\n"
        "  L: a[0] = 1;\n"
        "  return a[0]; }");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "a goto into a VLA scope past its decl must fail HIR verification";
    EXPECT_GE(countCode(r, DiagnosticCode::H_VlaJumpIntoScope), 1u)
        << "the C99 6.8.6.1 jump-into-VLA-scope ban must fire";

    // A LEGAL goto OUT of a VLA scope (the array is declared BEFORE the goto and the
    // label) does NOT trip the ban — it is torn down, not entered.
    SemanticModel ok = analyzeC(
        "int main(void){ volatile int vn = 4; int n = vn;\n"
        "  int a[n];\n"
        "  a[0] = 1;\n"
        "  goto L;\n"
        "  L: return a[0]; }");
    ASSERT_FALSE(ok.hasErrors())
        << (ok.diagnostics().all().empty() ? "" : ok.diagnostics().all()[0].actual);
    DiagnosticReporter r2;
    auto res2 = lowerToHir(ok, r2);
    EXPECT_EQ(countCode(r2, DiagnosticCode::H_VlaJumpIntoScope), 0u)
        << "a goto that stays within (or exits) a VLA scope is legal";
}

// VLA C5 (D-CSUBSET-VLA): a computed `goto *expr` (GNU IndirectGotoStmt) LEXICALLY
// inside a VLA scope has a runtime target set — no single SP-restore watermark is
// provable — so it FAILS LOUD at HIR verify (H_VlaComputedGotoInScope). The label
// `L` is OUTSIDE any VLA scope, so `&&L` is fine; only the `goto *p` inside the VLA
// block trips. Red-on-disable: drop the IndirectGotoStmt arm and it compiles.
TEST(HirLoweringC, ComputedGotoInsideVlaScopeFailsLoud) {
    SemanticModel model = analyzeC(
        "int main(void){ volatile int vn = 4; int n = vn;\n"
        "  void *p = &&L;\n"
        "  {\n"
        "    int a[n];\n"
        "    a[0] = 1;\n"
        "    goto *p;\n"
        "  }\n"
        "  L: return 0; }");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "a computed goto inside a VLA scope must fail HIR verification";
    EXPECT_GE(countCode(r, DiagnosticCode::H_VlaComputedGotoInScope), 1u)
        << "the computed-goto-in-VLA-scope ban must fire";
}

// FC5 (audit MUST-FIX 2) — the dead-code scan must NOT flag a goto's TARGET label
// as unreachable: `goto X; X: …` is the universal cleanup idiom and the label is
// manifestly reachable. But a genuinely-dead NON-label statement after a goto
// still warns. This is the carve-out's red-on-disable lever: reverting it makes
// the first case emit a spurious H_UnreachableCode on the label.
TEST(HirLoweringC, GotoTargetLabelIsNotFlaggedUnreachable) {
    // `goto skip; skip: return 1;` — the label directly follows the goto: ZERO.
    SemanticModel clean = analyzeC(
        "int f(){ goto skip; skip: return 1; } int main(){ return f(); }");
    ASSERT_FALSE(clean.hasErrors());
    DiagnosticReporter cr;
    auto cres = lowerToHir(clean, cr);
    ASSERT_TRUE(cres->ok) << (cr.all().empty() ? "" : cr.all()[0].actual);
    EXPECT_EQ(countCode(cr, DiagnosticCode::H_UnreachableCode), 0u)
        << "a goto's target label must NOT be flagged as dead code";

    // `goto skip; r = 1; skip: return r;` — the `r = 1` between the goto and the
    // label IS dead: EXACTLY ONE warning (on the assignment, not the label).
    SemanticModel dead = analyzeC(
        "int f(){ int r; goto skip; r = 1; skip: return r; } "
        "int main(){ return f(); }");
    ASSERT_FALSE(dead.hasErrors());
    DiagnosticReporter dr;
    auto dres = lowerToHir(dead, dr);
    ASSERT_TRUE(dres->ok) << (dr.all().empty() ? "" : dr.all()[0].actual);
    EXPECT_EQ(countCode(dr, DiagnosticCode::H_UnreachableCode), 1u)
        << "a genuinely-dead non-label statement after a goto must still warn";
}

// FC5 — the comma operator lowers to a SeqExpr (the existing sequencing
// substrate); a programmer comma is built non-Synthetic. A multi-declarator with
// a comma SEPARATOR stays two VarDecls (the comma-gate), never one SeqExpr-typed
// declarator — the structural side of the multi-site contract.
TEST(HirLoweringC, CommaOperatorLowersToSeqExprAndSeparatorStaysTwoDecls) {
    SemanticModel op = analyzeC(
        "int f(int x){ return (x = x + 1, x + 1); } int main(){ return f(40); }");
    ASSERT_FALSE(op.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(op, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_GE(countKind(res->hir, HirKind::SeqExpr), 1u)
        << "the comma operator must lower to a SeqExpr";

    // `int a = 1, b = 2;` — two declarators (the comma is a SEPARATOR). If the
    // comma-gate broke, the initializer would swallow `, b = 2` into one SeqExpr
    // and `b` would never be declared (so there would be ONE VarDecl, and `b`
    // would be undeclared at the return — a semantic error).
    SemanticModel sep = analyzeC(
        "int f(){ int a = 1, b = 2; return a + b; } int main(){ return f(); }");
    ASSERT_FALSE(sep.hasErrors())
        << "int a=1,b=2 must declare BOTH a and b (comma is a separator here)";
    DiagnosticReporter sr;
    auto sres = lowerToHir(sep, sr);
    ASSERT_TRUE(sres->ok) << (sr.all().empty() ? "" : sr.all()[0].actual);
    // BOTH a and b are declared — neither f nor main has params, so the module's
    // VarDecl count is exactly 2 (a, b). If the comma-gate broke, the initializer
    // would swallow `, b = 2` into one declarator (1 VarDecl) and `b` would be
    // undeclared (already caught by the hasErrors assertion above; this pins the
    // count too).
    EXPECT_EQ(countKind(sres->hir, HirKind::VarDecl), 2u)
        << "int a=1,b=2 must lower to TWO VarDecls (comma is a separator here)";
}

// NEGATIVE — a const-FALSE condition (`while(0)`) and a NON-constant condition
// (`while(x)`) are not provably-infinite and must not be wrapped. (Both rely on
// the trailing `return 7` to terminate; neither loop is touched by the wrapper.)
TEST(HirLoweringC, FalseAndNonConstConditionsAreNotWrapped) {
    SemanticModel zeroModel =
        analyzeC("int f(int x){ while(0){ return 5; } return 7; } "
                       "int main(){ return f(0); }");
    ASSERT_FALSE(zeroModel.hasErrors());
    DiagnosticReporter zr;
    auto zeroRes = lowerToHir(zeroModel, zr);
    ASSERT_TRUE(zeroRes->ok) << (zr.all().empty() ? "" : zr.all()[0].actual);
    HirNodeId const zf = functionNamed(zeroRes->hir, zeroModel, "f");
    ASSERT_TRUE(zf.valid());
    EXPECT_FALSE(subtreeHasSyntheticUnreachable(zeroRes->hir, zf))
        << "while(0) is provably FINITE — must not be wrapped";

    SemanticModel varModel =
        analyzeC("int f(int x){ while(x){ x = x - 1; } return 7; } "
                       "int main(){ return f(3); }");
    ASSERT_FALSE(varModel.hasErrors());
    DiagnosticReporter vr;
    auto varRes = lowerToHir(varModel, vr);
    ASSERT_TRUE(varRes->ok) << (vr.all().empty() ? "" : vr.all()[0].actual);
    HirNodeId const vf = functionNamed(varRes->hir, varModel, "f");
    ASSERT_TRUE(vf.valid());
    EXPECT_FALSE(subtreeHasSyntheticUnreachable(varRes->hir, vf))
        << "while(x) is not a constant-truthy condition — must not be wrapped";
}

// NEGATIVE (nesting) — a `break` inside a NESTED loop targets the INNER loop,
// not the outer. So `while(1){ while(1){ break; } }` has an OUTER loop that is
// provably-infinite (no break in ITS frame) and an INNER loop that is breakable
// (NOT infinite). The break-scan must respect that frame boundary: the outer
// loop IS wrapped; the inner loop is NOT. Exactly one synthetic Unreachable in
// the function (the outer wrapper's), and the inner WhileStmt is bare.
TEST(HirLoweringC, NestedInnerBreakDoesNotDeInfiniteOuterLoop) {
    SemanticModel model =
        analyzeC("int f(int x){ while(1){ while(1){ break; } return 5; } } "
                       "int main(){ return f(0); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId const f = functionNamed(res->hir, model, "f");
    ASSERT_TRUE(f.valid());

    // Outer loop wrapped: body holds exactly the Synthetic wrapper Block whose
    // first child is the OUTER WhileStmt.
    auto const bodyKids = res->hir.children(res->hir.functionBody(f));
    ASSERT_EQ(bodyKids.size(), 1u);
    HirNodeId const wrapper = bodyKids[0];
    ASSERT_EQ(res->hir.kind(wrapper), HirKind::Block);
    EXPECT_TRUE(has(res->hir.flags(wrapper), HirFlags::Synthetic));
    auto const wrapKids = res->hir.children(wrapper);
    ASSERT_EQ(wrapKids.size(), 2u);
    HirNodeId const outerWhile = wrapKids[0];
    ASSERT_EQ(res->hir.kind(outerWhile), HirKind::WhileStmt);
    ASSERT_EQ(res->hir.kind(wrapKids[1]), HirKind::Unreachable);

    // The inner loop (inside the outer loop's body) is a BARE WhileStmt — NOT
    // wrapped — because its `break` exits it. Find it under the outer while's
    // body and confirm its own subtree carries no synthetic Unreachable.
    HirNodeId const outerBody = res->hir.loopBody(outerWhile);
    HirNodeId innerWhile{};
    for (HirNodeId c : res->hir.children(outerBody))
        if (res->hir.kind(c) == HirKind::WhileStmt) { innerWhile = c; break; }
    ASSERT_TRUE(innerWhile.valid())
        << "the inner WhileStmt must sit bare in the outer loop's body";
    EXPECT_FALSE(subtreeHasSyntheticUnreachable(res->hir, innerWhile))
        << "the breakable INNER loop must not be wrapped";
}

TEST(HirLoweringC, NonConstantArrayLengthFailsLoud) {
    // VLA C1a (D-CSUBSET-VLA): a FILE-scope non-constant length must NOT silently
    // decay or assume a length — the semantic phase emits S_NonConstantArrayLength (a
    // file-scope array needs a constant bound; it is NOT a VLA). A BLOCK-scope
    // `int a[n]` is now a VLA (accepted at semantic, fails loud at the MIR->LIR C1b
    // boundary — see the mir/lir VLA pins).
    SemanticModel model = analyzeC("int n;\nint g[n];");
    EXPECT_TRUE(model.hasErrors());
}

TEST(HirLoweringC, IntegerOverflowReported) {
    // A literal too large for the 64-bit decoder must be reported, not silently
    // wrapped modulo 2^64.
    SemanticModel model = analyzeC("int f() { return 99999999999999999999999; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GT(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 0u);
}

TEST(HirLoweringC, IncludeDirectiveIsSkippedNotFailed) {
    // An `#include` directive contributes NO HIR node (its declarations arrive
    // via the CU import resolver's cross-refs). Lowering must SKIP it cleanly,
    // not emit H_UnsupportedLoweringForKind. The include target is unresolved
    // here (single in-memory buffer), but the directive node still parses and
    // reaches the top-level lowering loop — which is exactly what we pin.
    SemanticModel model = analyzeC("#include \"x.h\"\nint f() { return 0; }\n");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 0u)
        << "the #include directive must be skipped, not fail loud";
    // Exactly one decl: the function. The directive added nothing.
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::Function);
}

// The mutation-lvalue RECONSTRUCTION shape: a `MemberAccess` chain of exactly
// `hops` links standing on the `Deref` of the temp pointer `classifyMemberLvalue`
// binds. `hops == 1` is an ordinary named member; `hops == 2` is a field behind ONE
// anonymous struct/union member (`s.<anon>.f`), and so on. Counting the chain — not
// merely "a MemberAccess exists" — is what makes this test fail in the REMOVE
// direction: the generic via-ptr path this fix replaces emits a bare `Deref(Ref)`
// with NO MemberAccess above it, so the count drops to zero.
[[nodiscard]] std::size_t countReconstructedChains(Hir const& hir, HirNodeId n,
                                                   std::size_t hops) {
    if (!n.valid()) return 0;
    std::size_t total = 0;
    if (hir.kind(n) == HirKind::MemberAccess) {
        HirNodeId cur = n;
        std::size_t depth = 0;
        while (cur.valid() && hir.kind(cur) == HirKind::MemberAccess) {
            ++depth;
            auto const kids = hir.children(cur);
            cur = kids.empty() ? HirNodeId{} : kids.front();
        }
        // The chain must END on the Deref of the bound temp pointer. A source-level
        // `s.anon.f` rvalue also stacks MemberAccess nodes, but over a `Ref`, not a
        // `Deref` — so this counts reconstructions only.
        if (depth == hops && cur.valid() && hir.kind(cur) == HirKind::Deref) {
            auto const dk = hir.children(cur);
            if (!dk.empty() && hir.kind(dk.front()) == HirKind::Ref) ++total;
        }
    }
    for (HirNodeId c : hir.children(n))
        total += countReconstructedChains(hir, c, hops);
    return total;
}

// D-CSUBSET-BITFIELD-ANON-ARROW-MUTATION-RESIDUAL: the bit-field-safe reconstruction
// (D-CSUBSET-BITFIELD-ASSIGN-VALUE-POSITION) once handled NAMED `.`/`->` bit-field
// mutation ONLY. A bit-field reached through an ANONYMOUS-member hop chain, or
// through an ARRAY-arrow decay base (`sarr->bf`, C 6.3.2.1p3), could not be named by
// the single-field `Lvalue`, so a compound / inc-dec / value mutation there was
// REFUSED (S_BitfieldMutationUnsupportedBase) — loud, never a miscompile, but a
// capability gap: ✔MEASURED 2026-08-31, gcc 13.3.0, clang 18.1.3 and mingw-w64 gcc
// 13.2.0 all COMPILE AND RUN both shapes correctly and MSVC 19.51 accepts both, so
// DSS must. The `Lvalue` now carries an ordered member-hop CHAIN (one hop per
// anonymous ancestor, then the field) and the arrow base runs through the same
// `coerce` Array→Ptr decay arm `combineMember` uses — so both shapes reconstruct
// the very `MemberAccess` chain the MIR bit-field read-modify-write chokepoint keys
// on. RED-ON-DISABLE: collapse the chain back to a single index (or drop the decay)
// → these mutations either refuse again or fall to the generic via-ptr path, and the
// reconstruction-chain counts below go to zero.
TEST(HirLoweringC, AnonAndArrowBitfieldMutationReconstructsTheMemberChain) {
    // (1) compound `+=` on an ANON-struct bit-field: compiles, and rebuilds the
    //     TWO-hop chain `Deref(p).<anon>.a` the RMW chokepoint needs.
    {
        SemanticModel model = analyzeC(
            "struct O { struct { unsigned a : 4; unsigned b : 4; }; };\n"
            "void f(struct O* o) { o->a += 1; }\n");
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok) << "anon-member bit-field `+=` must compile";
        EXPECT_EQ(countCode(r, DiagnosticCode::S_BitfieldMutationUnsupportedBase), 0u)
            << "the anon-member bit-field mutation is supported — nothing to refuse";
        EXPECT_GE(countReconstructedChains(res->hir, res->hir.root(), 2), 2u)
            << "the read AND the write-back must each rebuild the 2-hop anon chain, "
               "or the mutation is a neighbour-clobbering full-unit store";
    }
    // (2) inc-dec on an anon bit-field (statement + value) both reconstruct.
    {
        SemanticModel model = analyzeC(
            "struct O { struct { unsigned a : 4; unsigned b : 4; }; };\n"
            "void f(struct O* o) { o->a++; }\n"
            "unsigned g(struct O* o) { return (++o->a); }\n");
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok) << "anon-member bit-field inc/dec must compile";
        EXPECT_EQ(countCode(r, DiagnosticCode::S_BitfieldMutationUnsupportedBase), 0u);
        EXPECT_GE(countReconstructedChains(res->hir, res->hir.root(), 2), 4u)
            << "statement `o->a++` (read+write) and value `++o->a` (read+write+yield) "
               "must all rebuild the anon chain";
    }
    // (3) NESTED anonymous members — TWO anon levels, a THREE-hop chain. The chain
    //     is a loop over `anonAncestorPath`, so one level working is no evidence
    //     that two do; this is the assertion a single-hop special case fails.
    {
        SemanticModel model = analyzeC(
            "struct O { struct { struct { unsigned a : 4; unsigned b : 4; }; }; };\n"
            "void f(struct O* o) { o->a += 1; }\n");
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok) << "a doubly-nested anon bit-field `+=` must compile";
        EXPECT_EQ(countCode(r, DiagnosticCode::S_BitfieldMutationUnsupportedBase), 0u);
        EXPECT_GE(countReconstructedChains(res->hir, res->hir.root(), 3), 2u)
            << "two anonymous levels must produce a 3-hop chain, not a truncated one";
    }
    // (4) STATEMENT plain-`=` on the SAME anon bit-field stays CORRECT — it routes
    //     through lowerAssign, not classifyMemberLvalue, and always did.
    {
        SemanticModel model = analyzeC(
            "struct O { struct { unsigned a : 4; unsigned b : 4; }; };\n"
            "void f(struct O* o) { o->a = 3; }\n");
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << "statement plain-`=` on an anon bit-field must compile";
        EXPECT_EQ(countCode(r, DiagnosticCode::S_BitfieldMutationUnsupportedBase), 0u)
            << "statement plain-`=` on an anon bit-field is correct — no refusal";
    }
    // (5) a NON-bit-field anon member compound-assign compiles. It now takes the
    //     reconstruction too (it used to fall through to the generic path); the
    //     chain it rebuilds is the SAME node a plain `o->x = v` builds, so the store
    //     stays an ordinary scalar store. This is the blast-radius guard.
    {
        SemanticModel model = analyzeC(
            "struct O { struct { int x; int y; }; };\n"
            "void f(struct O* o) { o->x += 1; }\n");
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok) << "non-bit-field anon member compound-assign must compile";
        EXPECT_EQ(countCode(r, DiagnosticCode::S_BitfieldMutationUnsupportedBase), 0u);
        EXPECT_GE(countReconstructedChains(res->hir, res->hir.root(), 2), 2u)
            << "a non-bit-field anon member reconstructs the same 2-hop chain";
    }
    // (6) an ARRAY-arrow bit-field base (`sarr->a`, C 6.3.2.1p3 decay): compiles,
    //     and rebuilds a ONE-hop chain over the DECAYED pointer.
    {
        SemanticModel model = analyzeC(
            "struct S { unsigned a : 4; unsigned b : 4; };\n"
            "void f(void) { struct S sarr[2]; sarr->a += 1; }\n");
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok) << "array-arrow bit-field `+=` must compile";
        EXPECT_EQ(countCode(r, DiagnosticCode::S_BitfieldMutationUnsupportedBase), 0u);
        EXPECT_GE(countReconstructedChains(res->hir, res->hir.root(), 1), 2u)
            << "the array-arrow base must decay and reconstruct, not full-unit-store";
    }
    // (7) a NON-bit-field array-arrow member is unaffected (and now reconstructs).
    {
        SemanticModel model = analyzeC(
            "struct S { int a; int b; };\n"
            "void f(void) { struct S sarr[2]; sarr->a += 1; }\n");
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok) << "a non-bit-field array-arrow member must compile";
        EXPECT_EQ(countCode(r, DiagnosticCode::S_BitfieldMutationUnsupportedBase), 0u);
    }
    // (8) CONTROL: a plain NAMED member keeps its ONE-hop reconstruction — the
    //     shape this fix generalised must not have moved for the case that already
    //     worked. Without this arm a chain-walk bug that added a phantom hop
    //     everywhere would still satisfy every assertion above.
    {
        SemanticModel model = analyzeC(
            "struct S { unsigned a : 4; unsigned b : 4; };\n"
            "void f(struct S* s) { s->a += 1; }\n");
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok);
        EXPECT_GE(countReconstructedChains(res->hir, res->hir.root(), 1), 2u)
            << "a named member is a ONE-hop chain — unchanged by the generalisation";
        EXPECT_EQ(countReconstructedChains(res->hir, res->hir.root(), 2), 0u)
            << "a named member must NOT grow a phantom anonymous hop";
    }
}

// HR cycle C: an int-typed `return expr;` whose expr type is non-int (e.g.
// a comparison that produces Bool) gets a `Cast(_, int)` wrapper inserted
// by the coercion pass. Pins the return-type-threading + coerce mechanism.
TEST(HirLoweringC, ReturnOfBoolFromIntFunctionEmitsCast) {
    SemanticModel model = analyzeC(
        "int gt(int a, int b) { return a > b; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ret  = res->hir.children(body)[0];
    HirNodeId val  = *res->hir.returnValue(ret);
    // The returned expression is now `Cast(BinaryOp(Gt, ...), int)`.
    EXPECT_EQ(res->hir.kind(val), HirKind::Cast);
    auto castKids = res->hir.children(val);
    ASSERT_EQ(castKids.size(), 1u);
    EXPECT_EQ(res->hir.kind(castKids[0]), HirKind::BinaryOp);
}

// HR cycle C: an `if` condition that's already Bool-typed (from a
// comparison) does NOT get a redundant Cast — coerce(child, target) is
// a no-op when child.type == target.
TEST(HirLoweringC, IfConditionAlreadyBoolStaysUncasted) {
    SemanticModel model = analyzeC(
        "void f(int x) { if (x > 0) { return; } }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ifs  = res->hir.children(body)[0];
    HirNodeId cond = res->hir.ifCondition(ifs);
    // `x > 0` is already Bool — no Cast wrapper needed.
    EXPECT_EQ(res->hir.kind(cond), HirKind::BinaryOp);
}

TEST(HirLoweringC, TernaryLowersToTernaryNode) {
    // `cond ? a : b` lowers to a HIR Ternary [cond, then, else]. A
    // non-Bool ARITHMETIC cond takes the truthiness test: a synthetic
    // `BinaryOp Ne(cond, 0-of-cond's-type)` typed Bool (C99 6.5.15p4
    // "compares unequal to 0") — NOT a `Cast(_, Bool)`, whose MIR
    // lowering (Trunc) would keep only the low bit (`x = 2` would
    // select the WRONG arm). Pins the coerceCondition shape at the
    // ternary site.
    SemanticModel model = analyzeC("int f(int x) { return x ? 1 : 2; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ret  = res->hir.children(body)[0];
    HirNodeId tern = *res->hir.returnValue(ret);
    ASSERT_EQ(res->hir.kind(tern), HirKind::Ternary);
    auto kids = res->hir.children(tern);
    ASSERT_EQ(kids.size(), 3u);
    // cond is `Ne(Ref(x), Literal 0)` typed Bool after coerceCondition.
    ASSERT_EQ(res->hir.kind(kids[0]), HirKind::BinaryOp)
        << "non-Bool ternary cond must lower as a truthiness comparison, "
           "not a Cast";
    ASSERT_TRUE(isCoreOp(res->hir.payload(kids[0])));
    EXPECT_EQ(decodeCoreOp(res->hir.payload(kids[0])), HirOpKind::Ne);
    EXPECT_EQ(model.lattice().interner().kind(res->hir.typeId(kids[0])),
              TypeKind::Bool);
    auto neKids = res->hir.children(kids[0]);
    ASSERT_EQ(neKids.size(), 2u);
    EXPECT_EQ(res->hir.kind(neKids[0]), HirKind::Ref);     // cond operand: x
    ASSERT_EQ(res->hir.kind(neKids[1]), HirKind::Literal); // synthetic 0
    // The synthetic zero keeps the cond's OWN type (I32) — no promotion
    // is needed for an unequal-to-zero test — and its pool value is 0.
    EXPECT_EQ(model.lattice().interner().kind(res->hir.typeId(neKids[1])),
              TypeKind::I32);
    auto zeroLit = res->literalPool.at(res->hir.payload(neKids[1]));
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(zeroLit.value));
    EXPECT_EQ(std::get<std::int64_t>(zeroLit.value), 0);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::Literal);   // then: 1
    EXPECT_EQ(res->hir.kind(kids[2]), HirKind::Literal);   // else: 2
}

// LogicalAnd/Or operands are CONDITION positions (C99 6.5.13p3/6.5.14p3:
// each operand "compares unequal to 0"). A non-Bool int operand must take
// the same truthiness `Ne(operand, 0)` wrap as an if/while condition —
// pinned at HIR tier because the MIR tier's LogicalAnd lowering currently
// trips an unrelated pre-existing StructCf marker mismatch (orthogonal:
// it reproduces with genuine Bool comparison operands too).
TEST(HirLoweringC, LogicalAndIntOperandsTakeTruthinessNe) {
    SemanticModel model = analyzeC(
        "int f(int x, int y) { return x && y ? 1 : 2; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ret  = res->hir.children(body)[0];
    HirNodeId tern = *res->hir.returnValue(ret);
    ASSERT_EQ(res->hir.kind(tern), HirKind::Ternary);
    // The ternary cond is the LogicalAnd itself — already Bool, so
    // coerceCondition adds NO extra node on top of it.
    HirNodeId land = res->hir.children(tern)[0];
    ASSERT_EQ(res->hir.kind(land), HirKind::LogicalAnd)
        << "Bool-typed LogicalAnd cond must NOT be re-wrapped";
    auto ops = res->hir.children(land);
    ASSERT_EQ(ops.size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        ASSERT_EQ(res->hir.kind(ops[i]), HirKind::BinaryOp)
            << "int operand " << i << " of && must take the truthiness Ne";
        ASSERT_TRUE(isCoreOp(res->hir.payload(ops[i])));
        EXPECT_EQ(decodeCoreOp(res->hir.payload(ops[i])), HirOpKind::Ne);
        EXPECT_EQ(model.lattice().interner().kind(res->hir.typeId(ops[i])),
                  TypeKind::Bool);
    }
}

TEST(HirLoweringC, PointerDerefAndAddressOfLower) {
    // `*p = x` (deref-assign through a pointer) and `p = &x` (address-of into a
    // pointer) lower with correct Ptr / pointee result types.
    SemanticModel model = analyzeC(
        "void f(int x) {\n"
        "  int *p;\n"
        "  p = &x;\n"
        "  *p = x;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    auto stmts = res->hir.children(body);
    // [ var p, assign p = &x, assign *p = x ]
    ASSERT_EQ(stmts.size(), 3u);
    // `p = &x`: value is AddressOf typed Ptr<I32>.
    HirNodeId addr = res->hir.assignValue(stmts[1]);
    ASSERT_EQ(res->hir.kind(addr), HirKind::AddressOf);
    ASSERT_EQ(ti.kind(res->hir.typeId(addr)), TypeKind::Ptr);
    // `*p = x`: target is Deref typed I32 (the pointee).
    HirNodeId deref = res->hir.assignTarget(stmts[2]);
    ASSERT_EQ(res->hir.kind(deref), HirKind::Deref);
    EXPECT_EQ(ti.kind(res->hir.typeId(deref)), TypeKind::I32);
}

// [[D-CSUBSET-CONST-EVAL-CHAR-SIGNEDNESS]]: THE PAYLOAD IS THE CONSTANT'S VALUE,
// NOT THE CODE UNIT — and it now rides the SIGNED arm. This used to assert
// `std::uint64_t`, which was the arm the old lowering used because it stored the
// raw byte the decoder hands back. C 6.4.4.4p10 makes a narrow character
// constant's value that byte read as a plain `char`, and this fixture's target
// declares plain `char` SIGNED, so the value is a signed integer and belongs in
// the signed arm. For `'a'` the NUMBER is 97 either way; the arm moved because
// the literal is now honest about its own type, and a Char-cored payload outside
// a signed `char`'s range was exactly the lie that let five consumers of one
// declaration disagree.
//
// ⓘ Nothing downstream reads signedness off the ARM — `const_eval_arith.hpp` says
// so in as many words ("THE SOURCE'S SIGNEDNESS IS CARRIED BY ITS CORE, NEVER BY
// WHICH VARIANT ARM HOLDS IT") and `asInt64`/`asIntBits` bridge both arms — so
// this is a representation change with one observable consequence: `'\xff'` is
// finally -1 where the target says plain `char` is signed.
TEST(HirLoweringC, CharLiteralLowersToCharValue) {
    // `'a'` — coalesced body token, decoded to a Char codepoint.
    SemanticModel model = analyzeC("char f() { return 'a'; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::Char);
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(v.value));
    EXPECT_EQ(std::get<std::int64_t>(v.value), static_cast<std::int64_t>('a'));
}

// The half of the pair that is NOT sign-neutral, and the reason the arm moved.
// `'\xff'` is -1 on this fixture's target (x86_64 declares plain `char` SIGNED)
// and would be +255 on the one unsigned-`char` leg DSS ships. ✔MEASURED at
// b1f31420 the lowering stored 255 on EVERY target, and the runtime path hid it:
// MIR materializes the low byte and extends per target, so 255 and -1 produce
// identical machine code — which is precisely why only the COMPILE-TIME consumers
// ever saw the defect, and why a green runtime witness proved nothing about them.
TEST(HirLoweringC, HighByteCharLiteralLowersToItsSignedValueOnASignedCharTarget) {
    SemanticModel model = analyzeC("char f() { return '\\xff'; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::Char);
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(v.value));
    EXPECT_EQ(std::get<std::int64_t>(v.value), -1)
        << "C 6.4.4.4p10: the constant's value is the code unit read as a plain "
           "`char`, and this target declares plain `char` SIGNED — 255 is the "
           "code unit, not the value";
}

TEST(HirLoweringC, CharEscapeLowersToControlCodepoint) {
    SemanticModel model = analyzeC("char f() { return '\\n'; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    // Signed arm — see `CharLiteralLowersToCharValue`. 10 is sign-neutral.
    EXPECT_EQ(std::get<std::int64_t>(res->literalPool.at(0).value), 10);  // '\n'
}

TEST(HirLoweringC, EmptyCharLiteralFailsLoud) {
    // `''` has no body char — fail loud, never a garbage codepoint.
    SemanticModel model = analyzeC("char f() { return ''; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GT(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 0u);
}

TEST(HirLoweringC, MultiCharCharLiteralFailsLoud) {
    // `'ab'` — a multi-character char body must fail loud, not silently take one
    // byte. (Symmetric to the empty case; the one a user hits by accident.)
    SemanticModel model = analyzeC("char f() { return 'ab'; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_GT(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 0u);
}

// ── C11/C23 6.4.4.4: wide / UTF CHARACTER constants (L'x'/u'x'/U'x'/u8'x') ───────
// A prefixed char constant is a SCALAR: its element core is typed by the prefix and
// its value is the single decoded code point (uint64 arm). The narrow path is
// UNCHANGED (the four tests above are the byte-identity guard).

TEST(HirLoweringC, WideCharConstantElementAndValue) {
    // Each prefixed char → its C23 core + the decoded code point. `L'x'` under the
    // default format is wchar_t = I32 (the POSIX width); value is the code point 120.
    struct Case { char const* src; TypeKind core; std::uint64_t value; };
    for (auto const& tc : {Case{"void f() { L'x'; }",  TypeKind::I32, 120},
                           Case{"void f() { u'A'; }",  TypeKind::U16, 65},
                           Case{"void f() { U'A'; }",  TypeKind::U32, 65},
                           Case{"void f() { u8'A'; }", TypeKind::U8,  65}}) {
        SemanticModel model = analyzeC(tc.src);
        ASSERT_FALSE(model.hasErrors()) << tc.src;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << tc.src << " : " << (r.all().empty() ? "" : r.all()[0].actual);
        ASSERT_EQ(res->literalPool.size(), 1u) << tc.src;
        auto const& v = res->literalPool.at(0);
        EXPECT_EQ(v.core, tc.core) << tc.src;
        ASSERT_TRUE(std::holds_alternative<std::uint64_t>(v.value)) << tc.src;
        EXPECT_EQ(std::get<std::uint64_t>(v.value), tc.value) << tc.src;
        auto const& ti = model.lattice().interner();
        HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
        HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
        EXPECT_EQ(ti.kind(res->hir.typeId(lit)), tc.core) << tc.src;
    }
}

TEST(HirLoweringC, WideCharBmpMultibyteDecodesToCodepoint) {
    // `U'€'` — U+20AC, source bytes E2 82 AC — UTF-8-decodes those THREE source
    // bytes to the SINGLE code point 0x20AC (NOT a byte-passthrough). The witness
    // that the char body is UTF-8-decoded exactly like a wide string.
    SemanticModel model = analyzeC("void f() { U'\xe2\x82\xac'; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U32);
    ASSERT_TRUE(std::holds_alternative<std::uint64_t>(v.value));
    EXPECT_EQ(std::get<std::uint64_t>(v.value), 0x20ACu) << "U+20AC is ONE code point";
}

TEST(HirLoweringC, Utf8CharOutOfRangeFailsLoud) {
    // `u8'β'` — U+03B2 (CE B2) exceeds the single-UTF-8-code-unit range (0x7F).
    // C23 char8_t constant constraint → fail loud (H_Utf8CharLiteralOutOfRange),
    // NEVER a silently truncated low byte.
    SemanticModel model = analyzeC("void f() { u8'\xce\xb2'; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "an out-of-range u8 char must fail lowering";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_Utf8CharLiteralOutOfRange), 1u)
        << "exactly the u8-out-of-range code, never a silent truncated byte";
}

TEST(HirLoweringC, Utf16CharAstralFailsLoud) {
    // `u'😀'` — U+1F600, a supplementary-plane cp under a 16-bit char16_t. One
    // char16_t holds ONE code unit; an astral cp needs a surrogate PAIR → fail loud
    // (H_WideCharValueUnrepresentable), NEVER a silent wrong unit. Format-invariant
    // (u' is always U16), so the reject is target-independent.
    SemanticModel model = analyzeC("void f() { u'\xf0\x9f\x98\x80'; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "an astral char16_t constant must fail lowering";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_WideCharValueUnrepresentable), 1u);
}

TEST(HirLoweringC, WideCharMultiCharAndEmptyFailLoud) {
    // A wide/UTF char must denote EXACTLY ONE code point. `L'ab'` (multi) and `L''`
    // (empty) both fail loud (H_WideCharValueUnrepresentable) — the strict
    // single-code-point rule the wide path enforces (unlike narrow impl-defined).
    for (char const* src : {"void f() { L'ab'; }", "void f() { L''; }"}) {
        SemanticModel model = analyzeC(src);
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_FALSE(res->ok) << src;
        EXPECT_EQ(countCode(r, DiagnosticCode::H_WideCharValueUnrepresentable), 1u) << src;
    }
}

// SHOULD-FIX #6 — the DEFINITIVE per-format / agnostic witness. `L'😀'` (U+1F600)
// is a wchar_t constant, and wchar_t is FORMAT-keyed: on pe it is the 16-bit UTF-16
// unit (U16) → the astral cp is UNREPRESENTABLE → fail loud; on the POSIX default it
// is I32 → the astral cp fits → lowers to value 0x1F600. ONE source, opposite
// outcomes, decided purely by the config `elementCoreByFormat` map — no format
// branch in shared substrate. Red-on-disable of the format-keying flips one arm.
TEST(HirLoweringC, WideCharAstralIsFormatKeyed) {
    char const* src = "void f() { L'\xf0\x9f\x98\x80'; }";
    // PE (u16 wchar_t) → fail loud.
    {
        SemanticModel model = analyzeCPe(src);
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_FALSE(res->ok) << "astral L' under pe (u16 wchar_t) must fail loud";
        EXPECT_EQ(countCode(r, DiagnosticCode::H_WideCharValueUnrepresentable), 1u);
    }
    // POSIX default (i32 wchar_t) → the astral cp fits → value 0x1F600.
    {
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
        ASSERT_EQ(res->literalPool.size(), 1u);
        auto const& v = res->literalPool.at(0);
        EXPECT_EQ(v.core, TypeKind::I32);
        ASSERT_TRUE(std::holds_alternative<std::uint64_t>(v.value));
        EXPECT_EQ(std::get<std::uint64_t>(v.value), 0x1F600u);
    }
}

TEST(HirLoweringC, LoweredSeqExprRoundTrips) {
    // A REAL lowering-produced SeqExpr (from `x++` in value position) must
    // survive the .dsshir emit→parse→verify round-trip — the seam where the
    // synthetic-temp `%sN` handle fallback meets the text writer.
    SemanticModel model = analyzeC("int f(int x) { return x++; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    std::vector<std::string> names = symbolNames(model);
    HirTextContext ctx;
    ctx.interner    = &model.lattice().interner();
    ctx.symbolNames = &names;
    ctx.literalPool = &res->literalPool;
    DiagnosticReporter er;
    std::string const out = emitHir(res->hir, ctx, er);
    EXPECT_NE(out.find("seq "), std::string::npos) << "expected a seq expr in:\n" << out;

    DiagnosticReporter pr;
    auto parsed = parseHir(out, CompilationUnitId{1}, pr);
    std::string diags;
    for (auto const& d : pr.all()) diags += std::string{diagnosticCodeName(d.code)} + ": " + d.actual + "\n";
    EXPECT_TRUE(parsed->ok) << "lowered SeqExpr did not round-trip/verify\n" << diags;
}

TEST(HirLoweringC, StringLiteralLowersToCharArray) {
    // `"hello"` — coalesced body, decoded bytes in the pool, typed Array<Char,6>
    // (5 chars + implied NUL).
    SemanticModel model = analyzeC("void f() { \"hello\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::Char);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), "hello");

    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    auto const& ti = model.lattice().interner();
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 6);                                  // "hello" + NUL
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::Char);
}

TEST(HirLoweringC, StringEscapeDecodes) {
    SemanticModel model = analyzeC("void f() { \"a\\tb\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    EXPECT_EQ(std::get<std::string>(res->literalPool.at(0).value), std::string("a\tb"));
}

// ── C 5.1.1.2 phase 6: adjacent string-literal concatenation ────────────────
// (D-CSUBSET-ADJACENT-STRING-CONCAT). `"a" "b"` ≡ `"ab"`. The HIR lowers the
// WHOLE stringLiteralExpr through the `decodeAdjacentStringBodies` chokepoint:
// every body decoded (phase 5) then byte-joined (phase 6). N = total decoded
// bytes; the literal's type is Array<Char, N+1>.

TEST(HirLoweringC, AdjacentStringsConcatTwoWay) {
    // `"a" "b"` → "ab", Array<Char,3> (2 bytes + NUL).
    SemanticModel model = analyzeC("void f() { \"a\" \"b\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u) << "the two pieces fold into ONE literal";
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::Char);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), "ab");

    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    auto const& ti = model.lattice().interner();
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3) << "\"ab\" + NUL";
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::Char);
}

TEST(HirLoweringC, AdjacentStringsConcatThreeWay) {
    // `"a" "b" "c"` → "abc", Array<Char,4>.
    SemanticModel model = analyzeC("void f() { \"a\" \"b\" \"c\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(std::get<std::string>(v.value), "abc");

    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    auto const& ti = model.lattice().interner();
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 4) << "\"abc\" + NUL";
}

// THE byte-level pin (mandatory): concatenation is at the DECODED-byte level —
// each body decoded by phase 5 FIRST, THEN joined. `"\x41" "1"` must be 'A'+'1'
// = "A1" (2 bytes), NOT a raw-token merge `\x411` (which would parse the hex
// escape across the boundary into the single byte 0x11). RED if a consumer
// concatenated raw bodies before decoding.
TEST(HirLoweringC, AdjacentStringsConcatEscapeBoundary) {
    SemanticModel model = analyzeC("void f() { \"\\x41\" \"1\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), "A1")
        << "per-segment escape decode THEN byte-join: \\x41→'A', then '1' → \"A1\" "
           "(a raw-token merge would decode \\x411 as one byte 0x11)";
    ASSERT_EQ(std::get<std::string>(v.value).size(), 2u);

    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    auto const& ti = model.lattice().interner();
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3) << "\"A1\" (2 bytes) + NUL";
}

// ── C11/C23 6.4.5: wide / UTF string literals (L"…"/u"…"/U"…"/u8"…") ─────────
// A prefixed string types as Array<elementCore, codeUnits+1> and its literal pool
// value carries the element-width-encoded (LE) code units. The narrow path is
// unchanged; these assert the wide element core, the encoded byte blob, and the
// astral fail-loud (surrogate pairs are a later cycle).

TEST(HirLoweringC, Utf16StringElementAndBytes) {
    // `u"AB"` → Array<U16,3>; bytes = 41 00 42 00 (2 LE U16 units), NUL implied.
    SemanticModel model = analyzeC("void f() { u\"AB\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U16);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), std::string({0x41, 0x00, 0x42, 0x00}))
        << "u\"AB\" = two LE 16-bit units 0x0041 0x0042";
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3) << "2 code units + wide NUL";
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::U16);
}

TEST(HirLoweringC, Utf32StringElementAndBytes) {
    // `U"AB"` → Array<U32,3>; bytes = 41 00 00 00 42 00 00 00.
    SemanticModel model = analyzeC("void f() { U\"AB\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U32);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value),
              std::string({0x41, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00}));
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::U32);
}

TEST(HirLoweringC, Utf8StringElementAndBytes) {
    // `u8"AB"` → Array<U8,3> (1 byte/ASCII unit); bytes = 41 42.
    SemanticModel model = analyzeC("void f() { u8\"AB\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U8);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), "AB");
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3) << "2 u8 units + NUL";
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::U8);
}

TEST(HirLoweringC, Utf16BmpMultibyteDecodesToOneUnit) {
    // `u"€"` — U+20AC, source bytes E2 82 AC — UTF-8-decodes to ONE U16 unit.
    // THE witness that the tokenizer's raw bytes are UTF-8-decoded (not passed
    // through byte-for-byte): 3 source bytes → 1 code unit → Array<U16,2>.
    SemanticModel model = analyzeC("void f() { u\"\xe2\x82\xac\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U16);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), std::string({static_cast<char>(0xAC), 0x20}))
        << "U+20AC as one LE 16-bit unit";
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 2) << "ONE code unit + NUL (NOT 3 bytes + NUL)";
}

TEST(HirLoweringC, Utf16AstralEncodesSurrogatePair) {
    // Cycle C: `u"😀"` — U+1F600 (F0 9F 98 80), a supplementary-plane cp under a
    // 16-bit element — now encodes as a UTF-16 SURROGATE PAIR: high 0xD83D then
    // low 0xDE00, i.e. the LE bytes 3D D8 00 DE (TWO code units), NEVER a silent
    // truncation. The array is Array<U16,3> (2 units + wide NUL). Red-on-disable:
    // revert the encodeCodepoint U16 astral branch and this fails to compile.
    SemanticModel model = analyzeC("void f() { u\"\xf0\x9f\x98\x80\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U16);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value),
              std::string({0x3D, static_cast<char>(0xD8), 0x00, static_cast<char>(0xDE)}))
        << "U+1F600 as a UTF-16 surrogate pair: high 0xD83D then low 0xDE00 (LE)";
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3) << "2 code units (surrogate pair) + wide NUL";
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::U16);
}

TEST(HirLoweringC, UcnAstralStringEncodesSurrogatePair) {
    // The `\U` universal-character-name form of the astral case: `u"\U0001F600"`
    // decodes (in the shared byte decoder) to U+1F600 and encodes to the SAME
    // surrogate pair 3D D8 00 DE as the raw `u"😀"`. Proves the UCN escape path.
    SemanticModel model = analyzeC("void f() { u\"\\U0001F600\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U16);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value),
              std::string({0x3D, static_cast<char>(0xD8), 0x00, static_cast<char>(0xDE)}))
        << "\\U0001F600 → surrogate pair 0xD83D 0xDE00 (LE)";
}

TEST(HirLoweringC, UcnBmpU32String) {
    // `U"é"` — the BMP UCN é (U+00E9) under a 32-bit element → one LE u32
    // unit 0x000000E9. The array is Array<U32,2> (1 unit + wide NUL).
    SemanticModel model = analyzeC("void f() { U\"\\u00e9\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U32);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value),
              std::string({static_cast<char>(0xE9), 0x00, 0x00, 0x00}))
        << "\\u00e9 → one LE u32 unit 0x000000E9";
}

TEST(HirLoweringC, UcnSurrogateHalfStringFailsLoud) {
    // FF1/FF2: `U"\uD800"` names a UTF-16 surrogate half — not a Unicode scalar
    // value. It fails loud with the dedicated H_InvalidUniversalCharacterName
    // (6.4.3), NEVER a silent CESU-8 / wrong unit.
    SemanticModel model = analyzeC("void f() { U\"\\uD800\"; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "a surrogate-half UCN must fail lowering";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_InvalidUniversalCharacterName), 1u);
}

TEST(HirLoweringC, WideStringByteEscapeAssemblesOneUnitEach) {
    // ✅ THE CLAIM THIS TEST MAKES IS INVERTED, AND THE INVERSION IS THE FIX (P55,
    // D-CSUBSET-WIDE-HEX-OCTAL-ESCAPE-VALUE). It used to assert that `u"\xC3\xA9"`
    // is REFUSED, because DSS could not express an escape's value as a code unit.
    // ⚠ IT MUST NOT BE READ AS "the guard was deleted": the behaviour BEFORE that
    // refusal was a SILENT COLLAPSE of these exact two escapes into ONE wrong
    // 0x00E9 unit, so the literal is pinned here BY VALUE. A regression to the
    // collapse gives one unit and reddens arm two; a regression to the blanket
    // refusal reddens arm one.
    // ✔MEASURED: gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0 and MSVC 19.51 all
    // emit the two units 0xC3, 0xA9 for this literal.
    SemanticModel model = analyzeC("void f() { u\"\\xC3\\xA9\"; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << "u\"\\xC3\\xA9\" is two code units and must lower";
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U16);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    // Two 16-bit LE units: C3 00 A9 00 — NOT the one 0x00E9 the collapse produced.
    EXPECT_EQ(std::get<std::string>(v.value),
              std::string({static_cast<char>(0xC3), 0, static_cast<char>(0xA9), 0}))
        << "each byte escape is its OWN code unit; the UTF-8 re-decode is bypassed";
}

TEST(HirLoweringC, WideStringByteEscapeAgreesWithTheSemanticArrayLength) {
    // ★★ THE TIER-AGREEMENT PIN, and the reason the semantic tier had to move with
    // the encoder rather than after it. HIR emits the code units and the semantic
    // typer derives `Array<char16_t, N+1>` from the SAME buffer through the SAME
    // encoder; if only one of them were given the escape values, one would count a
    // byte escape as one unit and the other would re-read its placeholder byte as
    // UTF-8. `sizeof` reads the semantic answer and the literal pool holds HIR's,
    // so asserting both in one test is what makes a divergence impossible to miss.
    SemanticModel model = analyzeC("void f() { u\"\\xC3\\xA9\"; }");
    EXPECT_FALSE(model.hasErrors()) << "the wide literal must TYPE, not merely lower";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    // HIR's answer: the emitted byte string, divided by the element width.
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    std::uint32_t const w        = elementByteWidth(v.core);
    std::size_t const   hirUnits = std::get<std::string>(v.value).size() / w;

    // The semantic tier's answer: the Array bound stamped on the same node, which
    // is `codeUnits + 1` for the NUL. Reading BOTH in one test is the point — a
    // tier that were given the escape values while the other was not would still
    // pass every single-tier assertion in this file.
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(hirUnits, 2u) << "two escapes, two code units";
    EXPECT_EQ(ti.scalars(ty)[0], static_cast<std::int64_t>(hirUnits + 1))
        << "the semantic array length and the HIR unit count must agree";
}

TEST(HirLoweringC, WideStringEscapeTooWideForElementFailsLoud) {
    // ★ THE FAIL-LOUD CLAIM THE OLD WideStringByteEscapeFailsLoud CARRIED LIVES
    // HERE NOW — moved to the case where the references actually split, not
    // deleted. gcc 13.3.0 and mingw-w64 gcc 13.2.0 truncate `u"\x1FFFF"` to 0xFFFF
    // with a warning; clang 18.1.3 and MSVC 19.51 refuse. The union over what WORKS
    // is a refusal, because a truncation is the same silent wrong answer this
    // anchor pair exists to end.
    SemanticModel model = analyzeC("void f() { u\"\\x1FFFF\"; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "an escape wider than char16_t must fail, never truncate";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_EscapeValueExceedsCodeUnit), 1u);

    // The SAME value under a 32-bit element is valid — the bound is the ELEMENT
    // WIDTH, not Unicode and not a ban on byte escapes.
    SemanticModel wide = analyzeC("void f() { U\"\\x1FFFF\"; }");
    DiagnosticReporter r2;
    auto res2 = lowerToHir(wide, r2);
    EXPECT_TRUE(res2->ok) << "U\"\\x1FFFF\" is one char32_t unit on all four references";
}

TEST(HirLoweringC, WideStringConcatRebasesEscapeOffsets) {
    // ★★ THE OFFSET REBASE, pinned from the direction it fails. Phase 5 decodes
    // each segment independently and phase 6 joins the bytes, so an escape recorded
    // at offset 0 of its OWN segment sits at offset 1 of the joined buffer here.
    // Without the rebase in `decodeAdjacentStringBodies` the encoder splices the
    // unit at the wrong place and re-reads a real byte as text — silently.
    // ✔MEASURED: all four references give `u"\xF" "F"` the units 0x000F, 0x0046.
    SemanticModel model = analyzeC("void f() { u\"\\xF\" \"F\"; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value),
              std::string({0x0F, 0, 0x46, 0}))
        << "the escape stays in the FIRST segment's position after the join";
}

TEST(HirLoweringC, WideStringEscapeIsARawUnitNotACodePoint) {
    // ★★★ THE PROPERTY MOST LIKELY TO BE "TIDIED" AWAY by a later refactor that
    // routes byte escapes back through the code-point validator. ✔MEASURED,
    // unanimous in BOTH directions on all four references: `u"\xD800"` assembles a
    // LONE SURROGATE unit and `U"\xFFFFFFFF"` a unit PAST U+10FFFF, while the UCN
    // spellings of those same numbers are refused by all four. If a refactor makes
    // these two fail, it has re-imposed Unicode rules on something that is not a
    // code point.
    SemanticModel surro = analyzeC("void f() { u\"\\xD800\"; }");
    DiagnosticReporter r;
    auto res = lowerToHir(surro, r);
    EXPECT_TRUE(res->ok) << "a lone-surrogate VALUE is a legal code unit";

    SemanticModel past = analyzeC("void f() { U\"\\xFFFFFFFF\"; }");
    DiagnosticReporter r2;
    auto res2 = lowerToHir(past, r2);
    EXPECT_TRUE(res2->ok) << "0xFFFFFFFF fills a char32_t unit exactly";

    // ...and the UCN spelling of the surrogate stays refused.
    SemanticModel ucn = analyzeC("void f() { u\"\\uD800\"; }");
    DiagnosticReporter r3;
    auto res3 = lowerToHir(ucn, r3);
    EXPECT_FALSE(res3->ok) << "\\uD800 names a surrogate CODE POINT and must fail";
    EXPECT_EQ(countCode(r3, DiagnosticCode::H_InvalidUniversalCharacterName), 1u);
}

TEST(HirLoweringC, WideCharByteEscapeAssemblesOneUnit) {
    // ✅ THE CLAIM THIS TEST MAKES IS INVERTED, AND THAT IS THE POINT (P55,
    // D-CSUBSET-WIDE-HEX-OCTAL-ESCAPE-VALUE, character half). It used to assert that
    // `u'\xC3\xA9'` fails loud; the reason it failed was that DSS could not express
    // an escape's VALUE, and the reason it must not simply be deleted is that the
    // behaviour BEFORE that refusal was a silent collapse to one 0x00E9 unit.
    //
    // Both hazards are now pinned positively. `u'\xFFFF'` is ONE 0xFFFF code unit —
    // ✔MEASURED identical on gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0 and MSVC
    // 19.51 — and a regression to the collapse would give 0xFFFD or a refusal, both
    // caught here. `u'\xC3\xA9'` keeps a REFUSAL, but for the honest reason: a
    // character constant denotes ONE code unit and that body names two.
    SemanticModel model = analyzeC("void f() { u'\\xFFFF'; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << "u'\\xFFFF' names one 0xFFFF code unit and must lower";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_EscapeValueExceedsCodeUnit), 0u);

    // The two-escape body is not one unit — refused, and NOT as a width failure.
    SemanticModel two = analyzeC("void f() { u'\\xC3\\xA9'; }");
    DiagnosticReporter r2;
    auto res2 = lowerToHir(two, r2);
    EXPECT_FALSE(res2->ok) << "u'\\xC3\\xA9' is two units, not one — must fail loud";
    EXPECT_EQ(countCode(r2, DiagnosticCode::H_WideCharValueUnrepresentable), 1u);
}

TEST(HirLoweringC, WideCharByteEscapeTooWideForElementFailsLoud) {
    // ★ THE FAIL-LOUD CLAIM MOVED HERE rather than being dropped (P55). The
    // references SPLIT on an escape that overflows the element: gcc 13.3.0 and
    // mingw-w64 gcc 13.2.0 truncate `u'\x1FFFF'` to 0xFFFF WITH A WARNING, clang
    // 18.1.3 and MSVC 19.51 REFUSE. A reference that only accepts by narrowing the
    // value away is not a working reference, so DSS refuses — a truncation here is
    // the same silent wrong answer the whole P55 pair exists to end.
    SemanticModel model = analyzeC("void f() { u'\\x1FFFF'; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "an escape wider than char16_t must fail, never truncate";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_EscapeValueExceedsCodeUnit), 1u);

    // U'\xFFFFFFFF' EXACTLY fills a 32-bit unit and is valid on all four references
    // — including past U+10FFFF, which is the proof that a byte escape is a raw
    // code UNIT and not a code POINT.
    SemanticModel wide = analyzeC("void f() { U'\\xFFFFFFFF'; }");
    DiagnosticReporter r2;
    auto res2 = lowerToHir(wide, r2);
    EXPECT_TRUE(res2->ok) << "U'\\xFFFFFFFF' fills a char32_t unit exactly";
}

TEST(HirLoweringC, WideCharSurrogateEscapeIsAUnitButTheUcnIsNot) {
    // ★★ THE SHARPEST MEASURED ASYMMETRY, and the one a later refactor is most
    // likely to "tidy" away by routing byte escapes back through the code-point
    // validator. ✔MEASURED, unanimous in BOTH directions on gcc 13.3.0, clang
    // 18.1.3, mingw-w64 gcc 13.2.0 and MSVC 19.51: `u'\xD800'` assembles to one
    // 0xD800 unit, while `u'\uD800'` — the same number spelled as a universal
    // character name — is REFUSED by all four. A raw code unit may be a lone
    // surrogate; a code point may not.
    SemanticModel esc = analyzeC("void f() { u'\\xD800'; }");
    DiagnosticReporter r;
    auto res = lowerToHir(esc, r);
    EXPECT_TRUE(res->ok) << "u'\\xD800' is a raw code unit and must lower";

    SemanticModel ucn = analyzeC("void f() { u'\\uD800'; }");
    DiagnosticReporter r2;
    auto res2 = lowerToHir(ucn, r2);
    EXPECT_FALSE(res2->ok) << "u'\\uD800' names a surrogate CODE POINT and must fail";
    EXPECT_EQ(countCode(r2, DiagnosticCode::H_InvalidUniversalCharacterName), 1u);
}

TEST(HirLoweringC, WideStringIllFormedUtf8FailsLoud) {
    // MEDIUM-2 (code-audit): after Cycle C, H_WideCharSurrogateUnsupported's surviving
    // trigger is a RAW ill-formed UTF-8 byte in a wide string body (astral-under-U16 now
    // surrogate-encodes; the `\x` route is shadowed by the wide-string escape guard).
    // A lone 0x80 (an invalid UTF-8 lead byte) must fail loud, not emit a garbage code
    // unit — this is the sole red-on-disable for that still-live diagnostic.
    std::string src = "void f() { u\"";
    src += static_cast<char>(0x80);
    src += "\"; }";
    SemanticModel model = analyzeC(src);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok) << "a raw ill-formed UTF-8 byte in a wide string must fail";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_WideCharSurrogateUnsupported), 1u);
}

TEST(HirLoweringC, NarrowStringByteEscapeStillWorks) {
    // FF3 boundary: the NARROW `"\xC3\xA9"` keeps `\x` escapes (byte-producing) —
    // Array<Char,3> with the two raw bytes C3 A9. Proves FF3 did not regress the
    // narrow path.
    SemanticModel model = analyzeC("void f() { \"\\xC3\\xA9\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::Char);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value),
              std::string({static_cast<char>(0xC3), static_cast<char>(0xA9)}))
        << "narrow \\xC3\\xA9 = the two raw bytes, unchanged";
}

TEST(HirLoweringC, NarrowStringUnchangedUnderPrefixTable) {
    // Regression: a bare `"AB"` still types Array<Char,3> with the raw bytes —
    // the prefix table's auto-seeded narrow row is byte-identical to before.
    SemanticModel model = analyzeC("void f() { \"AB\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::Char);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), "AB");
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::Char);
}

TEST(HirLoweringC, WideStringRoundTripsThroughDsshirText) {
    // F6: a `u"AB"` literal's element core (U16) must survive the .dsshir
    // emit→parse round-trip. `literalCoreFor` reads the core off the Array element
    // (NOT a hardcoded Char), so the re-parsed pool value carries U16, not Char.
    SemanticModel model = analyzeC("void f() { u\"AB\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    std::vector<std::string> names = symbolNames(model);
    HirTextContext ctx;
    ctx.interner    = &model.lattice().interner();
    ctx.symbolNames = &names;
    ctx.literalPool = &res->literalPool;
    DiagnosticReporter er;
    std::string const out = emitHir(res->hir, ctx, er);

    DiagnosticReporter pr;
    auto parsed = parseHir(out, CompilationUnitId{1}, pr);
    std::string diags;
    for (auto const& d : pr.all())
        diags += std::string{diagnosticCodeName(d.code)} + ": " + d.actual + "\n";
    ASSERT_TRUE(parsed->ok) << "u\"AB\" did not round-trip/verify\n" << diags;
    ASSERT_EQ(parsed->literalPool.size(), 1u);
    EXPECT_EQ(parsed->literalPool.at(0).core, TypeKind::U16)
        << "the re-parsed wide-string core must be U16 (read off the Array element, "
           "NOT hardcoded Char)";
}

// ── Cycle D — C11/C23 6.4.5p5: adjacent-concat prefix MIXING ────────────────
// A run of adjacent string literals takes the SINGLE distinct non-narrow prefix
// as its effective prefix (narrow segments widen to it, position-independent);
// TWO DIFFERENT non-narrow prefixes fail loud (impl-defined reject). These pin
// the two silent defects Cycle A left (mistype + miscompile) and the FF3-mixed hole.

TEST(HirLoweringC, ConcatNarrowWidensLeadingWidePrefix) {
    // `L"a" "b"` — the L segment leads; the NARROW "b" widens to wchar_t. Under the
    // POSIX default wchar_t is I32 → Array<I32,3>. The two units are 'a','b'.
    SemanticModel model = analyzeC("void f() { L\"a\" \"b\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u) << "the two pieces fold into ONE literal";
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::I32) << "the run is wide (wchar_t=I32 on POSIX)";
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3) << "'a' + widened 'b' + wide NUL";
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::I32);
}

TEST(HirLoweringC, ConcatNarrowWidensTrailingWidePrefix) {
    // THE defect fix: `"a" L"b"` — the FIRST opener is NARROW, so pre-Cycle-D keyed
    // the run's core on `"` and DROPPED the `L` → a silent narrow mistype. The run's
    // effective prefix is L (position-independent), so this is Array<wchar_t,3> and
    // the narrow "a" widens. RED-ON-DISABLE: revert to first-opener keying → Char.
    SemanticModel model = analyzeC("void f() { \"a\" L\"b\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::I32)
        << "`\"a\" L\"b\"` is WIDE — the trailing L prefix wins (was silently dropped)";
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::I32);
}

TEST(HirLoweringC, ConcatNarrowWidenByteBlobPosixVsPe) {
    // THE byte-blob pin proving the NARROW segment WIDENED to the run's wide element
    // width. `"a" L"b"` = {'a','b'} as wchar_t. On POSIX (I32) each unit is 4 LE
    // bytes → 61 00 00 00 62 00 00 00. On pe (U16) each is 2 LE bytes → 61 00 62 00.
    // A first-opener-narrow regression would emit the raw bytes `61 62` (Char) — a
    // different length AND width on BOTH formats.
    {
        SemanticModel model = analyzeC("void f() { \"a\" L\"b\"; }");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
        ASSERT_EQ(res->literalPool.size(), 1u);
        auto const& v = res->literalPool.at(0);
        EXPECT_EQ(v.core, TypeKind::I32);
        ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
        EXPECT_EQ(std::get<std::string>(v.value),
                  std::string({0x61, 0x00, 0x00, 0x00, 0x62, 0x00, 0x00, 0x00}))
            << "POSIX wchar_t=I32: 'a' and widened 'b' as two LE 4-byte units";
    }
    {
        SemanticModel model = analyzeCPe("void f() { \"a\" L\"b\"; }");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
        ASSERT_EQ(res->literalPool.size(), 1u);
        auto const& v = res->literalPool.at(0);
        EXPECT_EQ(v.core, TypeKind::U16) << "pe wchar_t is the U16 UTF-16 unit";
        ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
        EXPECT_EQ(std::get<std::string>(v.value),
                  std::string({0x61, 0x00, 0x62, 0x00}))
            << "pe wchar_t=U16: 'a' and widened 'b' as two LE 2-byte units";
    }
}

TEST(HirLoweringC, ConcatSamePrefixPreserved) {
    // `u"a" u"b"` — the SAME non-narrow prefix on both segments is NOT a conflict
    // (one distinct kind). Array<U16,3>, existing behavior preserved.
    SemanticModel model = analyzeC("void f() { u\"a\" u\"b\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::U16);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), std::string({0x61, 0x00, 0x62, 0x00}))
        << "u\"a\" u\"b\" = two LE 16-bit units 0x0061 0x0062";
}

TEST(HirLoweringC, ConcatConflictingNonNarrowPrefixesFailLoud) {
    // Each ordered pair of two DIFFERENT non-narrow prefixes is 6.4.5p5's impl-
    // defined case → fail loud with H_ConflictingStringLiteralPrefixes (NEVER a
    // silent resolve to one prefix, which drops the other's element width). Also a
    // 3-segment run with a LEADING NARROW piece, to pin the fold across positions.
    for (char const* src : {"void f() { u\"a\" U\"b\"; }",     // u16 vs u32
                            "void f() { u8\"a\" u\"b\"; }",    // char8_t vs char16_t
                            "void f() { L\"a\" u\"b\"; }",     // wchar_t vs char16_t
                            "void f() { \"a\" L\"b\" u\"c\"; }"}) {  // leading narrow, then L vs u
        SemanticModel model = analyzeC(src);
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_FALSE(res->ok) << src;
        EXPECT_EQ(countCode(r, DiagnosticCode::H_ConflictingStringLiteralPrefixes), 1u) << src;
    }
}

TEST(HirLoweringC, ConcatConflictPlainStatementFailsLoud) {
    // MF1 (red-on-disable via the SPECIFIC code): a PLAIN statement `u"a" U"b";` re-derives
    // its type at lowering (the semantic tier left it untyped). The EXPLICIT early conflict
    // branch reports the RIGHT reason — H_ConflictingStringLiteralPrefixes. Without it the
    // conflict is NOT silent (the type-drop guard still fires as a backstop: a Char stamp
    // under a wide effective opener) but with the WRONG reason (H_WideCharSurrogateUnsupported
    // "not well-formed UTF-8"). So this pins the branch via the EXACT code (countCode below),
    // and res->ok stays FALSE either way — defense in depth, correct-reason on top.
    SemanticModel model = analyzeC("void f() { u\"a\" U\"b\"; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "a mixed-prefix concat as a plain statement must fail lowering, not "
           "silently type Array<Char,3>";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ConflictingStringLiteralPrefixes), 1u);
    EXPECT_EQ(res->literalPool.size(), 0u) << "no literal is minted on the conflict path";
}

TEST(HirLoweringC, ConcatFF3MixedNarrowWidePrefixFailsLoud) {
    // The FF3-mixed hole (now CLOSED): `"a" L"\xC3"` — the run is WIDE (effective
    // prefix L) but the FIRST opener is narrow. Pre-Cycle-D the FF3 byte-escape guard
    // keyed on the first opener → narrow → MISS → the old silent UTF-8 collapse. Now
    // the guard keys on the run's effective prefix.
    // ⏳ P55 INVERTS THE OUTCOME AND KEEPS THE PROPERTY. `"a" L"\xC3"` now LOWERS,
    // because a byte escape in a wide run is a code unit rather than a refusal —
    // but what this test exists for is unchanged and still load-bearing: the run's
    // element must come from the EFFECTIVE prefix, never the FIRST opener. Keying
    // on the first opener would make this run NARROW, and `\xC3` would be emitted
    // as one raw byte inside what must be a 16-bit array — the original silent
    // miscompile, reached by a different road. Pinning the UNITS is what detects
    // that; a bare "it compiles" would not.
    SemanticModel model = analyzeC("void f() { \"a\" L\"\\xC3\"; }");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ConflictingStringLiteralPrefixes), 0u)
        << "a SINGLE non-narrow prefix is not a conflict";
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_NE(v.core, TypeKind::Char)
        << "the run is WIDE via its later prefix — a narrow core here is the "
           "first-opener bug returning as a silent miscompile";
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    // 'a' then the 0xC3 unit, each in the element's width (2 on pe, 4 on elf/macho
    // for L") — so assert the UNIT COUNT rather than a format-specific byte string.
    std::uint32_t const w = elementByteWidth(v.core);
    EXPECT_EQ(std::get<std::string>(v.value).size(), 2u * w)
        << "two code units: 'a' and the escape's 0xC3";
}

TEST(HirLoweringC, ConcatAllNarrowUnchanged) {
    // RED-ON-DISABLE guard: the effective-prefix change must NOT alter the all-narrow
    // path. `"a" "b"` stays byte-identical Array<Char,3> with the raw bytes "ab".
    SemanticModel model = analyzeC("void f() { \"a\" \"b\"; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const& v = res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::Char);
    ASSERT_TRUE(std::holds_alternative<std::string>(v.value));
    EXPECT_EQ(std::get<std::string>(v.value), "ab");
    auto const& ti = model.lattice().interner();
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId lit  = res->hir.exprStmtExpr(res->hir.children(body)[0]);
    TypeId const ty = res->hir.typeId(lit);
    ASSERT_EQ(ti.kind(ty), TypeKind::Array);
    EXPECT_EQ(ti.scalars(ty)[0], 3);
    EXPECT_EQ(ti.kind(ti.operands(ty)[0]), TypeKind::Char);
}

// ── golden ────────────────────────────────────────────────────────────────

namespace {

// The goldens tree, via the ONE test-side resolver (`repo_root.hpp`:
// $DSS_CONFIG_ROOT → the CMake-baked repo root → the cwd ancestor walk). The
// private walk this replaces resolved nothing in an OUT-OF-TREE build and then
// called `std::abort()` — which kills the whole test BINARY, so one unresolvable
// goldens path cost every sibling test in this executable its verdict.
// `repoRoot()` throws, and GoogleTest reports a throw as a failure of the one
// running test.
[[nodiscard]] fs::path findLoweringGoldens() {
    return dss::test::repoRoot() / "tests" / "hir" / "lowering_goldens";
}

[[nodiscard]] bool goldenRefreshRequested() {
    char const* raw = std::getenv("DSS_REFRESH_GOLDENS");
    if (raw == nullptr) return false;
    std::string_view const v{raw};
    return v == "1" || v == "true" || v == "TRUE" || v == "yes";
}

[[nodiscard]] std::string readFile(fs::path const& p) {
    // D-TEST-A-TORN-SHIPPED-CONFIG-CRASHES-A-SUITE-INSTEAD-OF-REDDING-IT:
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
    std::erase(s, '\r');   // CRLF→LF: golden compare is line-ending agnostic (Windows autocrlf)
    return s;
}

} // namespace

// D-DIAG-BRACE-INIT-AGGREGATE-SOURCE-SPAN: the TOP-LEVEL `ConstructAggregate` a
// brace initializer lowers to MUST carry a source span. It is the node every later
// tier reports AGAINST — the MIR brace-init refusals name it — and before this pin
// `lowerBraceInit` returned it WITHOUT `track(...)`, so those diagnostics printed
// with no `--> file:line` and the offending construct had to be found by grepping
// the source by hand. MEASURED cost: that is exactly how sqlite's `shell.c`
// `struct stat x = {0};` had to be located.
//
// RED-ON-DISABLE: drop the `track(...)` wrapper on `finishBraceLevel`'s returned
// aggregate and the matching arm below goes red. ⚠ THAT USED TO NAME TWO SITES,
// `lowerBraceInit`'s and `lowerUnionBraceInit`'s. There is ONE now: the brace-init
// work-stack conversion folded the union arm in as a one-slot level, so every
// level — struct, array and union alike — is `track`ed in one place.
TEST(HirLoweringC, BraceInitAggregateCarriesASourceSpan) {
    SemanticModel model = analyzeC(
        "struct S { int a; int b; };\n"
        "union U { int i; };\n"
        "int f(void) {\n"
        "  struct S s = {1, 2};\n"
        "  int arr[3] = {7};\n"
        "  union U u = {5};\n"
        "  return s.a + arr[0] + u.i;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    // The three top-level aggregates are the ones whose TYPE is the declared
    // struct / array / union — i.e. the direct init child of a VarDecl. Nested
    // zero-fill children are C's implicit defaults and stay span-less on purpose.
    unsigned checked = 0;
    // Arena slot 0 is the reserved sentinel; real ids run [1, nodeCount()).
    for (std::uint32_t i = 1; i < res->hir.nodeCount(); ++i) {
        HirNodeId const n{i};
        if (res->hir.kind(n) != HirKind::VarDecl) continue;
        auto const kids = res->hir.children(n);
        if (kids.empty()) continue;
        HirNodeId const init = kids.back();
        if (res->hir.kind(init) != HirKind::ConstructAggregate) continue;
        auto const* loc = res->sourceMap.tryGet(init);
        ASSERT_NE(loc, nullptr)
            << "a brace-init aggregate with NO source-map entry makes every "
               "diagnostic reported against it unlocatable";
        EXPECT_TRUE(loc->isPresent())
            << "the aggregate's span must name a real buffer";
        EXPECT_TRUE(loc->spansText())
            << "the span must cover the `{...}` text, not be an empty caret";
        ++checked;
    }
    EXPECT_EQ(checked, 3u)
        << "expected the struct, array, and union brace inits to be checked";
}

TEST(HirLoweringC, GoldenRepresentativeProgram) {
    SemanticModel model = analyzeC(
        "int add(int a, int b) {\n"
        "  return a + b;\n"
        "}\n"
        "int main() {\n"
        "  int x = add(1, 2);\n"
        "  if (x) {\n"
        "    x = x + 1;\n"
        "  }\n"
        "  return x;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    // Source provenance IS populated (asserted below), but the golden omits it:
    // BufferId is a process-global monotonic id, so `@loc(buf N, …)` would depend
    // on how many buffers earlier tests minted. The structure + types + symbols
    // are fully deterministic.
    EXPECT_GT(res->sourceMap.size(), 0u) << "lowering did not populate source provenance";

    std::vector<std::string> names = symbolNames(model);
    HirTextContext ctx;
    ctx.interner    = &model.lattice().interner();
    ctx.symbolNames = &names;
    ctx.literalPool = &res->literalPool;
    DiagnosticReporter er;
    std::string const out = emitHir(res->hir, ctx, er);

    fs::path golden = findLoweringGoldens() / "c_add_main.dsshir";
    if (goldenRefreshRequested()) {
        std::ofstream o{golden, std::ios::binary}; o << out;
        ADD_FAILURE() << "Refreshed " << golden.string()
                      << " — refresh is developer-only; the test fails by design.";
        return;
    }
    if (!fs::exists(golden)) {
        ADD_FAILURE() << "missing golden " << golden.string()
                      << " — generate via DSS_REFRESH_GOLDENS=1";
        return;
    }
    EXPECT_EQ(out, readFile(golden));

    // The emitted lowering must itself be a valid, parseable .dsshir module.
    DiagnosticReporter pr;
    auto parsed = parseHir(out, CompilationUnitId{1}, pr);
    std::string diags;
    for (auto const& d : pr.all()) diags += std::string{diagnosticCodeName(d.code)} + ": " + d.actual + "\n";
    EXPECT_TRUE(parsed->ok) << "lowered .dsshir did not round-trip/verify\n" << diags;
}

// ── D5.3 cycle 1a: brace-init lowering ───────────────────────────────────
//
// Each test lowers a VarDecl whose initializer is a `braceInitList` and
// pins that the lowered HIR contains a `ConstructAggregate` node with the
// expected slot count and that the lowering reports clean (`res->ok`).

namespace {
[[nodiscard]] HirNodeId firstVarInitOfFn(Hir const& hir, HirNodeId fn) {
    HirNodeId body = hir.functionBody(fn);
    for (HirNodeId s : hir.children(body)) {
        if (hir.kind(s) == HirKind::VarDecl) {
            if (auto init = hir.varDeclInit(s)) return *init;
        }
    }
    return HirNodeId{};
}
} // namespace

TEST(HirLoweringC, D5_3_PositionalStructInit) {
    SemanticModel model = analyzeC(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = {1, 2}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 2u);
}

// SP3.c LANDED 2026-05-28: single-level field designator now resolves
// via the TYPE-AWARE path (look up name in `compositeScopeFor(context)`
// rather than the lexical scope Pass 2 stamps). Pins value ORDERING:
// `.y = 7, .x = 3` must produce slot 0 = 3 (.x), slot 1 = 7 (.y).
// A regression that swapped lexical-order resolution for declaration-
// order would pass a count-only assertion silently.
TEST(HirLoweringC, D5_3_FieldDesignatorInit) {
    SemanticModel model = analyzeC(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = {.y = 7, .x = 3}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    // Slot 0 is .x = 3, slot 1 is .y = 7 (declaration order).
    ASSERT_EQ(res->hir.kind(kids[0]), HirKind::Literal);
    ASSERT_EQ(res->hir.kind(kids[1]), HirKind::Literal);
    auto slot0Lit = res->literalPool.at(res->hir.payload(kids[0]));
    auto slot1Lit = res->literalPool.at(res->hir.payload(kids[1]));
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(slot0Lit.value));
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(slot1Lit.value));
    EXPECT_EQ(std::get<std::int64_t>(slot0Lit.value), 3);
    EXPECT_EQ(std::get<std::int64_t>(slot1Lit.value), 7);
}

// C99 §6.7.8p19: a later designator OVERRIDES an earlier value at the
// same subobject. `{.x = 1, .x = 2}` → slot 0 = 2 (last wins).
TEST(HirLoweringC, D5_3_LaterDesignatorOverridesEarlier) {
    SemanticModel model = analyzeC(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = {.x = 1, .x = 2}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    ASSERT_EQ(res->hir.kind(kids[0]), HirKind::Literal);
    auto slot0Lit = res->literalPool.at(res->hir.payload(kids[0]));
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(slot0Lit.value));
    EXPECT_EQ(std::get<std::int64_t>(slot0Lit.value), 2)
        << "later .x = 2 must override earlier .x = 1 per C99 §6.7.8p19";
}

// Dot-chained designator coexists with a sibling brace-init under a
// different outer slot. Exercises the InitSlot tree's nested-merge
// substrate when a chained write and a positional write both land at
// the same outer-aggregate level.
TEST(HirLoweringC, D5_3_DotChainedDesignatorWithSibling) {
    SemanticModel model = analyzeC(
        "struct Inner { int v; };\n"
        "struct Outer { struct Inner a; struct Inner b; };\n"
        "void f() { struct Outer o = {.a.v = 1, .b = {.v = 9}}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::ConstructAggregate);
}

// Restored: zero-fill with designators
TEST(HirLoweringC, D5_3_ZeroFillsOmittedField) {
    SemanticModel model = analyzeC(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = {.y = 7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::Literal);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::Literal);
}

// Restored: chained-brace with field designators
TEST(HirLoweringC, D5_3_ChainedBraceNesting) {
    SemanticModel model = analyzeC(
        "struct Inner { int v; };\n"
        "struct Outer { struct Inner a; struct Inner b; };\n"
        "void f() { struct Outer o = {.a = {.v = 1}, .b = {.v = 2}}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::ConstructAggregate);
}

// ── [[D-C-FLOAT-CAST-DOES-NOT-FOLD-IN-A-CONSTANT-EXPRESSION]] ───────────────
//
// An index designator is the ONE integer-constant-expression consumer that lives
// at this tier, and its const-expr surface had quietly fallen behind the semantic
// tier's: `evalCstConstInt` carried no cast-target resolver at all, so
// `{ [(int)1] = 7 }` was refused with "index designator must be an integer
// literal" while the byte-identical expression folded fine as an array bound.
// ✔MEASURED: gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0 and MSVC 19.51 all
// accept `[(int)1]` AND `[(int)1.5]`.
//
// ⚠ THE ASSERTION IS WHERE THE 7 LANDED, not that lowering succeeded. A resolver
// that folded `(int)1.5` to 0 or 2 would still lower cleanly and would still be
// wrong; the slot is the only thing that says which.
TEST(HirLoweringC, IndexDesignatorFoldsACastToAnInteger) {
    struct Case { char const* src; char const* what; };
    Case const cases[] = {
        {"int a[3] = { [(int)1] = 7 };\n",   "an integer cast"},
        {"int a[3] = { [(int)1.5] = 7 };\n", "a float cast, truncated toward zero"},
        {"int a[3] = { [(int)1.9] = 7 };\n", "and truncation, not rounding"},
    };
    for (Case const& c : cases) {
        SemanticModel model = analyzeC(std::string{"void f() { "} + c.src + "}\n");
        ASSERT_FALSE(model.hasErrors()) << c.what;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok)
            << c.what << ": " << (r.all().empty() ? "" : r.all()[0].actual);
        HirNodeId fn   = firstFunction(res->hir);
        HirNodeId init = firstVarInitOfFn(res->hir, fn);
        ASSERT_TRUE(init.valid()) << c.what;
        EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate) << c.what;
        auto kids = res->hir.children(init);
        ASSERT_EQ(kids.size(), 3u) << c.what;
        // Slot 1 carries the 7; the designator resolved to index 1 and nowhere
        // else. Slots 0 and 2 are the zero fill.
        std::int64_t got[3] = {-1, -1, -1};
        for (std::size_t i = 0; i < 3; ++i) {
            ASSERT_EQ(res->hir.kind(kids[i]), HirKind::Literal) << c.what;
            auto const lit = res->literalPool.at(res->hir.payload(kids[i]));
            ASSERT_TRUE(std::holds_alternative<std::int64_t>(lit.value)) << c.what;
            got[i] = std::get<std::int64_t>(lit.value);
        }
        EXPECT_EQ(got[0], 0) << c.what;
        EXPECT_EQ(got[1], 7) << c.what;
        EXPECT_EQ(got[2], 0) << c.what;
    }
}

// The wall this tier keeps: a float-VALUED index is refused, exactly as all four
// references refuse `{ [1.5] = 7 }`. Without this control, "casts fold here now"
// would be indistinguishable from "floats are integers here now".
TEST(HirLoweringC, IndexDesignatorStillRefusesAFloatVALUE) {
    SemanticModel model = analyzeC("void f() { int a[3] = { [1.5] = 7 }; }\n");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    bool const refused = model.hasErrors() || !res->ok;
    EXPECT_TRUE(refused) << "a float-valued index designator must fail loud";
}

// Lock-in: field designator naming a NON-EXISTENT field emits a
// diagnostic that the field doesn't belong to the target struct.
TEST(HirLoweringC, D5_3_UnknownFieldDesignatorEmitsDiag) {
    SemanticModel model = analyzeC(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = {.bogus = 7}; }\n");
    if (model.hasErrors()) return;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("doesn't belong") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "unknown field designator must be diagnosed";
    EXPECT_FALSE(res->ok);
}

TEST(HirLoweringC, D5_3_OmittedFieldZeroFillStructureWithoutDesignator) {
    // Without field designators, `struct Point p = {7}` lands `7` at
    // slot 0 (positional) + zero-fills slot 1. This exercises the
    // zero-fill path WITHOUT depending on the substrate-blocked
    // designator-name resolution.
    SemanticModel model = analyzeC(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = {7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::Literal);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::Literal);
}

TEST(HirLoweringC, D5_3_ChainedBraceNestingPositional) {
    // The positional form `struct Outer o = {{1}, {2}}` exercises the
    // chained-brace nesting recursion without depending on designator
    // resolution.
    SemanticModel model = analyzeC(
        "struct Inner { int v; };\n"
        "struct Outer { struct Inner a; struct Inner b; };\n"
        "void f() { struct Outer o = {{1}, {2}}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::ConstructAggregate);
}

TEST(HirLoweringC, D5_3_PositionalArrayInit) {
    SemanticModel model = analyzeC(
        "void f() { int xs[3] = {10, 20, 30}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 3u);
}

// Under-filled array: explicit elements at slots 0..k-1, synth-zero at k..N-1.
TEST(HirLoweringC, D5_3_ArrayUnderfillZeroFillsTail) {
    SemanticModel model = analyzeC(
        "void f() { int xs[5] = {1, 2}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 5u);
    for (auto k : kids) EXPECT_EQ(res->hir.kind(k), HirKind::Literal);
}

// C99 §6.7.8p17: a designator restarts the fill cursor at the designated
// position; the immediately following positional element resumes from
// `designated + 1`. Exercised here with INDEX designators (array).
TEST(HirLoweringC, D5_3_CursorRestartAfterIndexDesignator) {
    SemanticModel model = analyzeC(
        "void f() { int xs[5] = {[1] = 9, 7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 5u);
    for (auto k : kids) EXPECT_EQ(res->hir.kind(k), HirKind::Literal);
}

// Same C99 §6.7.8p17 invariant exercised with FIELD designators
// (struct) — now that SP3.c lifted the field-designator substrate
// blocker. `{.b = 9, 7}` puts 9 at .b (slot 1), then the positional 7
// at slot 2 (.c) per cursor restart.
TEST(HirLoweringC, D5_3_CursorRestartAfterFieldDesignator) {
    SemanticModel model = analyzeC(
        "struct Trip { int a; int b; int c; };\n"
        "void f() { struct Trip t = {.b = 9, 7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 3u);
    for (auto k : kids) EXPECT_EQ(res->hir.kind(k), HirKind::Literal);
}

// Empty brace `T x = {};` — fully zero-fills via synthZero recursion. The
// existing zero-fill test only exercises synthZero on scalar fields; this
// hits the recursive aggregate-arm (struct of struct).
TEST(HirLoweringC, D5_3_EmptyBraceZeroFillsNestedAggregate) {
    SemanticModel model = analyzeC(
        "struct Inner { int v; };\n"
        "struct Outer { struct Inner a; struct Inner b; };\n"
        "void f() { struct Outer o = {}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::ConstructAggregate);
}

// ── D5.3 cycle 1b: index designator + compound literal + return / call /
// assign context sites + locked-in substrate-blocked diagnostics ────────

// 1b.2: `int xs[3] = {[2] = 7};` — integer-literal index designator lands
// at slot 2; slots 0 and 1 zero-fill.
TEST(HirLoweringC, D5_3_IndexDesignatorLiteral) {
    SemanticModel model = analyzeC(
        "void f() { int xs[3] = {[2] = 7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 3u);
}

// 1b.2: multi-index designator with cursor jump — `{[0] = 1, [4] = 5}`
// against a 5-slot array. Pins the cursor-restart behavior past one
// jump (the test-analyzer's #2 rating-8 gap).
TEST(HirLoweringC, D5_3_MultiIndexDesignatorWithCursorJump) {
    SemanticModel model = analyzeC(
        "void f() { int xs[5] = {[0] = 1, [4] = 5}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 5u);
    // Slots 0 and 4 are explicit Literals; 1, 2, 3 are zero-fill Literals.
    for (auto k : kids) EXPECT_EQ(res->hir.kind(k), HirKind::Literal);
}

// SP3.c LANDED 2026-05-28: dot-chained `.a.v = 1` now resolves via the
// type-aware designator walker that threads a current-type cursor
// through each step. The InitSlot tree's `nested` substrate (sleeping
// since cycle 1b) makes the multi-step write semantically right (.a
// becomes a sub-aggregate; .v inside it is the explicit value; the
// rest of .a + the entire .b zero-fill).
TEST(HirLoweringC, D5_3_DotChainedDesignator) {
    SemanticModel model = analyzeC(
        "struct Inner { int v; };\n"
        "struct Outer { struct Inner a; struct Inner b; };\n"
        "void f() { struct Outer o = {.a.v = 1}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    // .a is a sub-aggregate (the dot-chained write created it).
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::ConstructAggregate);
    // .b is the synth-zero sub-aggregate (omitted).
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::ConstructAggregate);
}

// 1b.4: ordinary `return 7;` still coerces — the return-site refactor
// (lowerExprOrBraceInit consolidating brace-init detection across all
// context sites) must not regress non-brace-init returns.
TEST(HirLoweringC, D5_3_OrdinaryReturnStillCoerces) {
    SemanticModel model = analyzeC("int f() { return 7; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

// 1b.3 SUBSTRATE-BLOCKED LOCK-IN: a compound literal `(T){...}` reaches
// `lowerCompoundLiteral`; the typeRef child's type can't be resolved
// from Pass 2 (which only stamps types on identifier references inside
// `operand` rules — not type-position references). Until plan 08.5 §SP3
// substrate stamps types on typeRef nodes, the lowering fails LOUD with
// a `compound literal type-ref did not resolve` diagnostic. This test
// STRICTLY asserts the diagnostic — when SP3 lands, this test must be
// inverted to assert `res->ok && children.size() == 2`. The strict form
// matters: a `found || res->ok` short-circuit would let a future
// regression silently set ok=true with InvalidType and still pass.
// SP3.b LANDED 2026-05-28: with `structTypeRef` added to the c
// `references` config (with `nameMatch: "lastIdentifier"`), Pass 2
// resolves the struct's Identifier + stamps `nodeToType` on it; the
// recursive `resolveStampedTypeBelow` then finds the stamp and the
// compound-literal lowers cleanly.
TEST(HirLoweringC, D5_3_CompoundLiteralInVarDeclInit) {
    SemanticModel model = analyzeC(
        "struct Point { int x; int y; };\n"
        "void f() { struct Point p = (struct Point){.x = 1, .y = 2}; }\n");
    if (model.hasErrors()) {
        for (auto const& d : model.diagnostics().all())
            ADD_FAILURE() << diagnosticCodeName(d.code) << " actual=" << d.actual;
        return;
    }
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 2u);
}

// A NON-CONSTANT index designator (`[n] = 7`, `n` a mutable local) must be
// diagnosed. Strict form: when semantic accepts the input, the lowering MUST
// emit the diagnostic (silent acceptance would be a regression the looser
// `found || res->ok` form would have missed).
//
// ⚠ REPAIRED, NOT DELETED, under
// [[D-C-FLOAT-CAST-DOES-NOT-FOLD-IN-A-CONSTANT-EXPRESSION]]. Two things about
// this pin had gone stale while the assertion it makes stayed exactly right.
// (1) It matched the diagnostic by the words "integer literal", which described
// a designator surface that had already grown past literals (`[1+0]` folds) and
// has now grown a cast too — the message names C 6.7.10p4's actual requirement,
// an integer constant EXPRESSION, so the match is on that. (2) Its comment
// called this "substrate-blocked ... pending CST-side const-eval", which stopped
// being the reason long ago: the substrate is here, and `n` is refused because
// it is genuinely not a constant, which is what all four references also say.
TEST(HirLoweringC, D5_3_NonLiteralIndexDesignatorEmitsDiag) {
    SemanticModel model = analyzeC(
        "void f() { int n = 1; int xs[3] = {[n] = 7}; }\n");
    if (model.hasErrors()) return;   // tolerated: semantic may reject too
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("integer constant") != std::string::npos
         || d.actual.find("const-eval") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found)
        << "a non-constant index designator MUST be diagnosed";
    EXPECT_FALSE(res->ok)
        << "lowering must fail when a non-constant index designator appears";
}

// ── plan 12.5 §0.2 D6: CST-side const-eval ──────────────────────────
// The shared CST const-eval engine folds literal arithmetic, ternary,
// LogicalAnd/Or, and `const`-bound identifier refs at all 3 consumer
// sites: array length, enumerator value, and index designator.

// Helper: extract the Array length from a VAR symbol's declared type
// via the TypeInterner. Returns -1 if the symbol isn't found or its
// type isn't an Array; the caller asserts the expected length.
static std::int64_t arrayLengthOfVar(SemanticModel const& model,
                                     std::string_view name) {
    auto const& interner = model.lattice().interner();
    for (auto const& sym : model.symbols()) {
        if (sym.name != name) continue;
        if (!sym.type.valid()) return -1;
        if (interner.kind(sym.type) != TypeKind::Array) return -1;
        auto scals = interner.scalars(sym.type);
        if (scals.empty()) return -1;
        return scals[0];
    }
    return -1;
}

// Array-length const-expr fold: `int a[1+2];` produces Array<int, 3>.
// Previously the hand-rolled "literal-only" check refused anything but
// a single integer literal token; the new engine folds the BinaryOp.
TEST(HirLoweringC, CstConstEval_ArrayLengthConstExpr) {
    SemanticModel model = analyzeC(
        "void f() { int xs[1+2]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 3);
}

// Array-length with `const`-bound identifier ref. The resolver walks
// the scope chain from the declaration site, finds `N` as `isConst`,
// and folds `N + 1` to 4. Mutable refs still refuse — covered by the
// locked-in NonLiteral test that uses `int n = 1;` (not `const`).
TEST(HirLoweringC, CstConstEval_ArrayLengthConstRef) {
    SemanticModel model = analyzeC(
        "const int N = 3;\n"
        "void f() { int xs[N + 1]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 4);
}

// FC4 c1 (audit F1) — the PER-DECLARATOR const-init name match.
// `const int A = 100, B = 2;` carries TWO initDeclarators under ONE
// decl node; findInitExprInDecl must resolve each symbol's init by
// its OWN name node. The silent failure this pins: degrading the
// match to "the FIRST declarator's init" keeps every single-
// declarator test green while B folds as 100 — so `xs[B + 1]`
// becomes Array<101>, not Array<3>. The A-fed sibling stays green
// under that degrade (A IS the first declarator); the B assertion
// is the discriminating lever (red-on-disable demonstrated by
// short-circuiting the name match to the list head).
TEST(HirLoweringC, CstConstEval_MultiDeclaratorConstInitsResolvePerName) {
    SemanticModel model = analyzeC(
        "const int A = 100, B = 2;\n"
        "void f() { int xs[B + 1]; int ys[A - 97]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 3);
    EXPECT_EQ(arrayLengthOfVar(model, "ys"), 3);
}

// UnaryOp fold: `-(-3)` → 3. Pins the UnaryExprRule branch
// (previously test-coverage gap).
TEST(HirLoweringC, CstConstEval_UnaryFolds) {
    SemanticModel model = analyzeC(
        "void f() { int xs[-(-3)]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 3);
}

// LogicalAnd short-circuit: `1 && 2` is 1; combine with arithmetic to
// land on a non-trivial length. Pins the LogicalAnd/Or branch.
TEST(HirLoweringC, CstConstEval_LogicalAndFolds) {
    SemanticModel model = analyzeC(
        "void f() { int xs[(1 && 2) + 4]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 5);
}

// LogicalOr short-circuit: `1 || (some_runtime)` should fold to 1
// regardless of the rhs being non-foldable. The rhs is a mutable
// reference that would refuse to fold if reached.
TEST(HirLoweringC, CstConstEval_LogicalOrShortCircuits) {
    SemanticModel model = analyzeC(
        "void f() { int n = 1; int xs[(1 || n) + 2]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 3);
}

// Cycle detection: `const int a = b + 0; const int b = a + 0;` is a
// genuine cycle. The engine refuses with NotAConstantExpression at
// the second encounter; caller emits S_NonConstantArrayLength.
// VLA C1a (D-CSUBSET-VLA): the array is pinned at FILE scope so the non-foldable
// (cyclic) bound stays S_NonConstantArrayLength — the const-eval-cycle refusal is
// preserved. A block-scope `int xs[a + 1]` would become a VLA (fails at the LIR C1b
// boundary); this keeps the cycle-detection intent.
TEST(HirLoweringC, CstConstEval_CycleRefuses) {
    SemanticModel model = analyzeC(
        "const int a = b + 0;\n"
        "const int b = a + 0;\n"
        "int xs[a + 1];\n");
    bool found = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_NonConstantArrayLength) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found)
        << "cyclic const-init chain must refuse to fold";
}

// D7 scope-context tracking: shadowing across scopes is NOT a cycle.
// Outer `const X=1; const Y=X+1;` + inner `const X=Y;` — the inner X
// shadows the outer X at use-site, but Y's init `X+1` must be
// evaluated in MODULE scope (Y's scope), where it correctly resolves
// to the OUTER X (= 1). Result: inner X = Y = 2; `xs[X+1]` = 3.
//
// Without scope-context tracking, evaluating Y's `X+1` would re-use
// the function scope (the original use-site) and find the inner X
// → false cycle or wrong value. The engine threads
// `resolved->initScopeOpaque` through recursion to fix this.
TEST(HirLoweringC, CstConstEval_ShadowingResolvesCorrectScope) {
    SemanticModel model = analyzeC(
        "const int X = 1;\n"
        "const int Y = X + 1;\n"
        "void f() { const int X = Y; int xs[X + 1]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 3);
}

// Cross-scope ref chain (no shadowing): outer module consts referenced
// from a function body. Two-deep const-ref chain. The engine resolves
// `xs[N + 1]` → folds N (which is itself `M+1`=3) → fold to 4.
TEST(HirLoweringC, CstConstEval_TransitiveConstRef) {
    SemanticModel model = analyzeC(
        "const int M = 2;\n"
        "const int N = M + 1;\n"
        "void f() { int xs[N + 1]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    EXPECT_EQ(arrayLengthOfVar(model, "xs"), 4);
}

// Division by zero in a const-expr: `int xs[1/0];` — the engine
// refuses with DivisionByZero (caller maps to S_NonConstantArrayLength
// since array length doesn't have a dedicated div-by-zero diagnostic).
// VLA C1a (D-CSUBSET-VLA): pinned at FILE scope so the div-by-zero const-expr stays
// S_NonConstantArrayLength (a block-scope `int xs[1/0]` would become a VLA that
// fails at the LIR C1b boundary; the div-by-zero-refusal intent is preserved here).
TEST(HirLoweringC, CstConstEval_DivByZeroRefuses) {
    SemanticModel model = analyzeC(
        "int xs[1/0];\n");
    bool found = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_NonConstantArrayLength) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found)
        << "div-by-zero in const-expr must refuse to fold";
}

// Index designator const-expr fold: `{[1+1] = 7}` lowers to the same
// ConstructAggregate shape as `{[2] = 7}` (slot 2 gets 7, slots 0/1
// zero-fill).
TEST(HirLoweringC, CstConstEval_IndexDesignatorConstExpr) {
    SemanticModel model = analyzeC(
        "void f() { int xs[3] = {[1+1] = 7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 3u);
}

// Index designator with `const`-bound ref: `[N+1]` where `const N=1`
// folds to slot 2.
TEST(HirLoweringC, CstConstEval_IndexDesignatorConstRef) {
    SemanticModel model = analyzeC(
        "const int N = 1;\n"
        "void f() { int xs[3] = {[N + 1] = 7}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

// Enumerator value const-expr fold: `A = 1+1` ⇒ A=2; subsequent
// `B` auto-increments to 3.
TEST(HirLoweringC, CstConstEval_EnumeratorConstExpr) {
    SemanticModel model = analyzeC(
        "enum E { A = 1 + 1, B };\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    // The two enumerators bind A=2 and B=3; verify via SymbolRecord.
    bool foundA = false, foundB = false;
    for (auto const& sym : model.symbols()) {
        if (sym.name == "A") { EXPECT_EQ(sym.enumValue, 2); foundA = true; }
        if (sym.name == "B") { EXPECT_EQ(sym.enumValue, 3); foundB = true; }
    }
    EXPECT_TRUE(foundA);
    EXPECT_TRUE(foundB);
}

// Enumerator value with `const`-bound ref: `A = X + 1` where const X=5
// resolves to 6.
TEST(HirLoweringC, CstConstEval_EnumeratorConstRef) {
    SemanticModel model = analyzeC(
        "const int X = 5;\n"
        "enum E { A = X + 1 };\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
    for (auto const& sym : model.symbols()) {
        if (sym.name == "A") { EXPECT_EQ(sym.enumValue, 6); return; }
    }
    FAIL() << "enumerator A not found";
}

// Ternary fold: `[1 < 2 ? 3 : 5]` → length 3 (cond true → then arm).
TEST(HirLoweringC, CstConstEval_TernaryFolds) {
    SemanticModel model = analyzeC(
        "void f() { int xs[1 < 2 ? 3 : 5]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? "" : model.diagnostics().all()[0].actual);
}

// VLA C1a (D-CSUBSET-VLA): a BLOCK-scope array whose bound is a mutable runtime
// integer (`int n = 1; int xs[n + 1];`) is EXACTLY a variable-length array — the
// const-eval correctly declines to fold `n` (it is not `isConst`), and at block
// scope that non-constant integer bound is now a VLA (accepted at semantic; the
// runtime alloca fails loud at the MIR->LIR C1b boundary). It no longer emits
// S_NonConstantArrayLength. (Pre-VLA this was a reject; the feature reclassifies a
// runtime-int array bound as a VLA.)
TEST(HirLoweringC, CstConstEval_MutableRefIsVla) {
    SemanticModel model = analyzeC(
        "void f() { int n = 1; int xs[n + 1]; }\n");
    EXPECT_FALSE(model.hasErrors())
        << "a runtime-int array bound is a valid VLA, accepted at semantic";
    bool foundDiag = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_NonConstantArrayLength) {
            foundDiag = true; break;
        }
    }
    EXPECT_FALSE(foundDiag)
        << "a block-scope runtime-int bound is a VLA, not S_NonConstantArrayLength";
}

// VLA C1a (D-CSUBSET-VLA, C 6.7.6.2p1): a `_BitInt(N)` bound is a LEGAL VLA size
// (BitInt is an integer kind) — accepted at semantic, NOT S_VlaSizeNotInteger. It
// fails loud only at the MIR->LIR C1b boundary, like any VLA. RED-ON-DISABLE: drop
// BitInt from `isVlaSizeIntegerType` and this VLA wrongly errors as a non-integer
// size.
TEST(HirLoweringC, VlaBitIntBoundIsAcceptedNotSizeError) {
    SemanticModel model = analyzeC(
        "void f() { unsigned _BitInt(20) n; int a[n]; }\n");
    EXPECT_FALSE(model.hasErrors())
        << "a _BitInt VLA bound is a valid integer size, accepted at semantic";
    for (auto const& d : model.diagnostics().all()) {
        EXPECT_NE(d.code, DiagnosticCode::S_VlaSizeNotInteger)
            << "a _BitInt bound is an integer VLA size, not S_VlaSizeNotInteger";
    }
}

// ── D5.4 unions ──────────────────────────────────────────────────────

// `union U u;` declares + types via the same Pass 1.5 path as struct;
// the difference is `compositeKind: "union"` in the c config →
// `interner.unionType(...)`. Empty init zero-fills the FIRST variant.
TEST(HirLoweringC, D5_4_UnionDeclLowersToTypeDecl) {
    SemanticModel model = analyzeC(
        "union U { int i; char c; };\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::TypeDecl);
}

// Positional union brace-init `{ 5 }` initializes the FIRST variant.
// Aggregate is 1-child (NOT 2 — unions are not zero-fill-all).
TEST(HirLoweringC, D5_4_UnionPositionalInit) {
    SemanticModel model = analyzeC(
        "union U { int i; char c; };\n"
        "void f() { union U u = { 5 }; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 1u)
        << "union aggregate must have exactly 1 child (the active variant)";
}

// Designated union brace-init `{ .c = 'a' }` initializes the named
// variant — second variant (index 1), not first.
TEST(HirLoweringC, D5_4_UnionDesignatedInit) {
    SemanticModel model = analyzeC(
        "union U { int i; char c; };\n"
        "void f() { union U u = { .c = 'a' }; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 1u);
    // The child's HIR type identifies the chosen variant — Char here.
    // (We don't read the type interner from the test directly; the
    // child being Literal is enough to confirm the chosen variant's
    // value was lowered.)
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::Literal);
}

// Empty `{}` union init zero-fills the FIRST variant (1-child aggregate).
TEST(HirLoweringC, D5_4_UnionEmptyBrace) {
    SemanticModel model = analyzeC(
        "union U { int i; char c; };\n"
        "void f() { union U u = {}; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 1u);
}

// Multi-element union brace-init MUST emit a diagnostic.
TEST(HirLoweringC, D5_4_UnionMultiElementEmitsDiag) {
    SemanticModel model = analyzeC(
        "union U { int i; char c; };\n"
        "void f() { union U u = { 1, 2 }; }\n");
    if (model.hasErrors()) return;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("at most one variant") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "multi-element union init must be diagnosed";
    EXPECT_FALSE(res->ok);
}

// Unknown union variant name → diagnostic.
TEST(HirLoweringC, D5_4_UnionUnknownVariantEmitsDiag) {
    SemanticModel model = analyzeC(
        "union U { int i; char c; };\n"
        "void f() { union U u = { .bogus = 1 }; }\n");
    if (model.hasErrors()) return;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("doesn't belong") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "unknown union variant must be diagnosed";
    EXPECT_FALSE(res->ok);
}

// Index designator on a union is nonsensical → diagnostic.
TEST(HirLoweringC, D5_4_UnionIndexDesignatorEmitsDiag) {
    SemanticModel model = analyzeC(
        "union U { int i; char c; };\n"
        "void f() { union U u = { [0] = 1 }; }\n");
    if (model.hasErrors()) return;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("not meaningful on union") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "index designator on union must be diagnosed";
    EXPECT_FALSE(res->ok);
}

// Chained designator `{.a.b = 1}` on a union must be diagnosed (variant
// access has no sub-position semantics in C99). Lock-in for the
// silent-failure-hunter HIGH finding.
TEST(HirLoweringC, D5_4_UnionChainedDesignatorEmitsDiag) {
    SemanticModel model = analyzeC(
        "struct Inner { int v; };\n"
        "union U { struct Inner a; int i; };\n"
        "void f() { union U u = { .a.v = 1 }; }\n");
    if (model.hasErrors()) return;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("chained designator on a union") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "chained designator on union must be diagnosed";
    EXPECT_FALSE(res->ok);
}

// Union nested inside a struct: the InitSlot path lands on the union's own
// level (`prepareUnionBraceInit` picks the variant, the brace-init work stack
// runs it as a one-slot level) correctly + omitted struct slots
// containing unions zero-fill via the corrected `synthZeroOrError`
// Union arm (1-child first-variant).
TEST(HirLoweringC, D5_4_UnionNestedInStruct) {
    SemanticModel model = analyzeC(
        "union U { int i; char c; };\n"
        "struct S { union U u; int x; };\n"
        "void f() { struct S s = { .u = { .c = 'a' }, .x = 7 }; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    // Outer slot 0 is the union → 1-child ConstructAggregate.
    ASSERT_EQ(res->hir.kind(kids[0]), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(kids[0]).size(), 1u);
    // Outer slot 1 is .x = 7 → Literal.
    EXPECT_EQ(res->hir.kind(kids[1]), HirKind::Literal);
}

// Union zero-filled by the containing struct's missing-field path: the
// `synthZeroOrError(unionTy)` produces a 1-child aggregate (not N).
TEST(HirLoweringC, D5_4_UnionZeroFilledByContainingStruct) {
    SemanticModel model = analyzeC(
        "union U { int i; char c; };\n"
        "struct S { int x; union U u; };\n"
        "void f() { struct S s = { .x = 1 }; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    auto kids = res->hir.children(init);
    ASSERT_EQ(kids.size(), 2u);
    // Slot 0 = .x = 1 (Literal). Slot 1 = synth-zero union → 1-child agg.
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::Literal);
    ASSERT_EQ(res->hir.kind(kids[1]), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(kids[1]).size(), 1u);
}

// Compound literal of union type: `(union U){.c='a'}` exercises the
// lowerCompoundLiteral → lowerBraceInit → openBraceLevel → prepareUnionBraceInit
// chain (the union is a one-slot level of the brace-init work stack).
TEST(HirLoweringC, D5_4_UnionCompoundLiteral) {
    SemanticModel model = analyzeC(
        "union U { int i; char c; };\n"
        "void f() { union U u = (union U){ .c = 'a' }; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId fn = firstFunction(res->hir);
    HirNodeId init = firstVarInitOfFn(res->hir, fn);
    ASSERT_TRUE(init.valid());
    EXPECT_EQ(res->hir.kind(init), HirKind::ConstructAggregate);
    EXPECT_EQ(res->hir.children(init).size(), 1u);
}

// Member access on a union: `u.c` must resolve via the existing
// MemberAccess path (same `compositeScopeByType` substrate that
// structs use).
TEST(HirLoweringC, D5_4_UnionMemberAccess) {
    SemanticModel model = analyzeC(
        "union U { int i; char c; };\n"
        "void f() { union U u = { .c = 'a' }; char x = u.c; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

// ── D5.5 enums ───────────────────────────────────────────────────────

// `enum E { A, B, C };` declares a TypeDecl. The enum type is nominal-
// by-name; enumerators are Variable symbols with the enum type, bound
// in the enum's inner scope (accessed as `E.A` via MemberAccess).
TEST(HirLoweringC, D5_5_EnumDeclLowersToTypeDecl) {
    SemanticModel model = analyzeC(
        "enum E { A, B, C };\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_EQ(decls.size(), 1u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::TypeDecl);
}

// C-classic enumerator visibility: `enum E { A, B }` makes `A` and `B`
// visible directly in the enclosing scope (Pass 1.5 lifts the
// enumerator bindings from the enum's inner scope to the parent).
// `enum E e = A;` resolves `A` against the enclosing scope.
TEST(HirLoweringC, D5_5_EnumValueUseViaBareName) {
    SemanticModel model = analyzeC(
        "enum E { A, B, C };\n"
        "void f() { enum E e = A; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

// Top-level enum with a TRAILING COMMA parses cleanly — the RED-ON-DISABLE guard
// for the schema-compiler `recomputeAltExpectedSets` fixpoint
// (D-CSUBSET-STRUCT-BODY-VARDECL-POSITION
// §2d). The enum body `enumerator (Comma enumerator?)*` is
// an `optional` inside a `repeat` before the required `}` closer; without the
// fixpoint the trailing-comma optional never learns `}` can follow, so the
// SPECULATIVE body probe (`topLevelCompositeSpec`) hits P_NoAlternativeMatched
// and rolls back to the ref form → parse error. VERIFIED red-on-disable: toggling
// off the `recomputeAltExpectedSets` orchestration call makes THIS test fail.
// NOTE: the `recomputeAltExpectedSets` fixpoint is GLOBAL (every grammar's
// expectedSets), so it fixes the typedef-position surface (`typedef enum {…,} T;`)
// too — see the parallel pin D5_5_TypedefEnumTrailingCommaParses
// (D-CSUBSET-TYPEDEF-ENUM-TRAILING-COMMA CLOSED; the registry's earlier
// "typedef is a separate gap" scoping was overly conservative).
TEST(HirLoweringC, D5_5_EnumTrailingCommaParses) {
    SemanticModel model = analyzeC(
        "enum E { A, B, C, };\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    // The trailing comma must mint EXACTLY A, B, C — never a phantom 4th
    // enumerator (an over-eager `(Comma enumerator?)*` that consumed the comma
    // as introducing a fourth, empty enumerator would be a silent mis-parse a
    // bare `!hasErrors()` check could miss).
    int enumerators = 0;
    for (auto const& s : model.symbols())
        if (s.name == "A" || s.name == "B" || s.name == "C") ++enumerators;
    EXPECT_EQ(enumerators, 3)
        << "exactly A, B, C — the trailing comma introduces no enumerator";
}

// TYPEDEF-POSITION enum with a TRAILING COMMA parses cleanly — the parallel of
// D5_5_EnumTrailingCommaParses for the typedef surface (closes the now-stale
// D-CSUBSET-TYPEDEF-ENUM-TRAILING-COMMA). The global schema-compiler fixpoint
// `recomputeAltExpectedSets` (D-PARSE-SCHEMA-NESTED-NULLABLE-FOLLOW) fixes EVERY
// grammar's expectedSets, so the typedef speculative surface recovers the
// trailing-comma enum body too — verified parsing AND running across anon /
// named-tag / valued / single-element forms. RED-ON-DISABLE: toggling off the
// `recomputeAltExpectedSets` orchestration call (grammar_schema_json.cpp) reddens
// THIS pin alongside the top-level one — verified by the closing cycle.
TEST(HirLoweringC, D5_5_TypedefEnumTrailingCommaParses) {
    SemanticModel model = analyzeC(
        "typedef enum { A, B, C, } T;\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    // Exactly A, B, C — the trailing comma must mint no phantom 4th enumerator
    // (an over-eager `(Comma enumerator?)*` consuming the comma as a 4th empty
    // enumerator would be a silent mis-parse a bare `!hasErrors()` would miss).
    int enumerators = 0;
    for (auto const& s : model.symbols())
        if (s.name == "A" || s.name == "B" || s.name == "C") ++enumerators;
    EXPECT_EQ(enumerators, 3)
        << "exactly A, B, C — the trailing comma introduces no enumerator";
}

// Enumerator values: implicit auto-increment + explicit integer-literal
// + auto-increment from explicit. C99 §6.7.2.2. Verifies that Pass 1.5
// actually COMPUTES the values (not just parses them).
TEST(HirLoweringC, D5_5_EnumValuesComputed) {
    SemanticModel model = analyzeC(
        "enum E { A, B, C = 5, D };\n");
    ASSERT_FALSE(model.hasErrors());
    // Look up each enumerator's value in the symbol table.
    auto findEnumerator = [&](std::string const& name) -> SymbolRecord const* {
        for (auto const& s : model.symbols()) {
            if (s.name == name) return &s;
        }
        return nullptr;
    };
    auto const* a = findEnumerator("A");
    auto const* b = findEnumerator("B");
    auto const* c = findEnumerator("C");
    auto const* d = findEnumerator("D");
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr); ASSERT_NE(d, nullptr);
    EXPECT_EQ(a->enumValue, 0) << "A implicit → 0";
    EXPECT_EQ(b->enumValue, 1) << "B implicit → A + 1 = 1";
    EXPECT_EQ(c->enumValue, 5) << "C explicit = 5";
    EXPECT_EQ(d->enumValue, 6) << "D implicit → C + 1 = 6";
}

// Enumerator type identity: each enumerator must be typed as the enum
// (not as the underlying int). A regression that left enumerators
// typed as I32 would pass count-only assertions but break downstream
// type-equivalence checks.
TEST(HirLoweringC, D5_5_EnumeratorTypedAsEnum) {
    SemanticModel model = analyzeC("enum E { A };\n");
    ASSERT_FALSE(model.hasErrors());
    auto& interner = model.lattice().interner();
    TypeId const enumTy = interner.enumType("E", TypeKind::I32);
    // The enumerator A must carry the enum TypeId, not raw I32.
    SymbolRecord const* a = nullptr;
    for (auto const& s : model.symbols())
        if (s.name == "A") { a = &s; break; }
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->type.v, enumTy.v)
        << "enumerator must be typed as the enum, not the underlying int";
    EXPECT_NE(a->type.v, interner.primitive(TypeKind::I32).v)
        << "enumerator MUST NOT carry the raw I32 TypeId";
}

// Lift-to-enclosing collision: `int A; enum E { A };` must emit
// S_RedeclaredSymbol pointing at the enumerator decl. Locks the
// otherwise-test-untouched diagnostic branch.
TEST(HirLoweringC, D5_5_EnumeratorCollidesWithEnclosingName) {
    SemanticModel model = analyzeC(
        "int A = 7;\n"
        "enum E { A };\n");
    bool foundRedecl = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_RedeclaredSymbol && d.actual == "A") {
            foundRedecl = true; break;
        }
    }
    EXPECT_TRUE(foundRedecl)
        << "lifting enumerator name into a scope that already binds it "
           "must emit S_RedeclaredSymbol";
}

// Non-literal explicit value emits S_NonConstantEnumeratorValue. v1
// accepts integer-literal explicit values only; arbitrary const-exprs
// require CST-side const-eval (plan 12.5 §0.2 D6).
TEST(HirLoweringC, D5_5_NonLiteralEnumeratorValueEmitsDiag) {
    SemanticModel model = analyzeC(
        "int n = 5;\n"
        "enum E { A = n };\n");
    bool found = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_NonConstantEnumeratorValue) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found)
        << "non-literal enumerator value must emit S_NonConstantEnumeratorValue";
}

// D5.5-FU2: prove the `liftToEnclosingScope` gate is wired by toggling
// the c config's flag to `false` and verifying `A` no longer
// resolves at the use site. Pins the otherwise-test-untouched
// opt-OUT branch — without this, removing the `&&
// decl.fieldChildren->liftToEnclosingScope` guard would silently
// keep all tests green.
TEST(HirLoweringC, D5_5_LiftOptOutRespected) {
    // Read the shipped c config text and flip the enumDecl's
    // `liftToEnclosingScope` from true to false. The rest of the
    // schema (incl. `compositeKind: "enum"`) stays untouched.
    // The path comes from the ONE test-side resolver (`repo_root.hpp`), not a
    // private cwd walk: the walk that stood here found nothing in an
    // OUT-OF-TREE build, whose cwd has no `src/dss-config` in its ancestry.
    fs::path const schemaPath =
        dss::test::configRoot() / "sources" / "c.lang.json";
    std::ifstream in{schemaPath, std::ios::binary};
    ASSERT_TRUE(in.is_open()) << "cannot open " << schemaPath.string();
    std::ostringstream buf; buf << in.rdbuf();
    std::string text = std::move(buf).str();
    std::string const target =
        "\"compositeKind\": \"enum\",\n"
        "                           \"liftToEnclosingScope\": true";
    // The enum-composite lift flag now rides ONE row — the unified `enumSpec`
    // (c25 D-CSUBSET-UNIFIED-COMPOSITE-SPECIFIER REPLACED the `enumSpecifierBody`
    // row; the dead statement-position `enumDecl` row was deleted earlier in
    // D-CSUBSET-STRUCT-BODY-VARDECL-POSITION). Flip EVERY occurrence so the
    // opt-out is total — the bare-name `A` below (its enum is parsed via the
    // unified `enumSpec`) must then resolve through NO lift.
    std::size_t flipped = 0;
    for (auto pos = text.find(target); pos != std::string::npos;
         pos = text.find(target, pos)) {
        text.replace(pos, target.size(),
            "\"compositeKind\": \"enum\",\n"
            "                           \"liftToEnclosingScope\": false");
        ++flipped;
    }
    ASSERT_GE(flipped, 1u)
        << "c config no longer carries the expected enum lift flag";

    auto loaded = GrammarSchema::loadFromText(text, "<c-no-lift>");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "" : loaded.error()[0].message);

    UnitBuilder builder{*loaded, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(
        "enum E { A, B, C };\n"
        "void f() { enum E e = A; }\n",
        "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel model = analyze(cu, DiagnosticBudget::libraryDefault());

    bool foundUndecl = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_UndeclaredIdentifier && d.actual == "A") {
            foundUndecl = true; break;
        }
    }
    EXPECT_TRUE(foundUndecl)
        << "with liftToEnclosingScope=false, bare-name `A` MUST emit "
           "S_UndeclaredIdentifier — the gate's opt-out branch is otherwise "
           "test-untouched";
}

// D5.5-FU4 + FU5: enum-typed program emits + re-parses cleanly (the
// HIR text format `enum "E"` round-trip is locked in by parse re-verify).
TEST(HirLoweringC, D5_5_EnumHirTextRoundTrip) {
    SemanticModel model = analyzeC(
        "enum E { A, B, C };\n"
        "void f() { enum E e = A; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    std::vector<std::string> names = symbolNames(model);
    HirTextContext ctx;
    ctx.interner    = &model.lattice().interner();
    ctx.symbolNames = &names;
    ctx.literalPool = &res->literalPool;
    DiagnosticReporter er;
    std::string const out = emitHir(res->hir, ctx, er);
    EXPECT_NE(out.find("enum"), std::string::npos);

    DiagnosticReporter pr;
    auto parsed = parseHir(out, CompilationUnitId{42}, pr);
    EXPECT_TRUE(parsed->ok)
        << "re-parsed enum program must verify cleanly";
}

// ── TRIPWIRE: no shipped C construct lowers to a HIR intrinsic ───────
// D-OPT7-INLINE-FRAME-SENSITIVE-INTRINSIC
// This pin's JOB CHANGED, and the change is worth stating. It used to be
// load-bearing for CORRECTNESS: OPT7 cycle 6 made the MIR inliner
// blanket-admit IntrinsicCall-bearing callees, and that admission was safe
// only while no frame-sensitive intrinsic (va_start / frameaddress /
// stacksave / setjmp-class) could reach the inliner — a precondition
// nothing but this test enforced. The inliner now REFUSES an
// IntrinsicCall-bearing callee outright (it cannot resolve a bare MIR
// intrinsic id, so it cannot prove frame-insensitivity), which moves that
// correctness guarantee into the gate itself where it belongs.
// So this is now a DESIGN tripwire, not a safety net: it records that the
// C frontend reaches its 30 `lowering:` builtins (umulh, bswap, popcount,
// the SEH pair, …) through DEDICATED MIR opcodes and never through the
// HIR intrinsic registry, which stays empty through every real compile.
// The day a frontend does mint one, this goes RED and points whoever did
// it at the anchor — where the relaxation path (a registration-driven
// frame-safety attribute threaded to the MIR IntrinsicCall) is written
// down. Being red then costs them a read, not a miscompile.
TEST(HirLoweringC, NoShippedConstructLowersToIntrinsic) {
    // Breadth of constructs (all in the shipped corpus): params +
    // arithmetic, subtraction, a cross-function call, a conditional, a
    // loop, a comparison. If any future lowering arm emits an intrinsic,
    // at least one of these exercises the path that would register it.
    char const* const programs[] = {
        "int add(int a, int b) { return a + b; }",
        "int sub(int a, int b) { return a - b; }",
        "int callee(int x) { return x + 1; } "
            "int caller(int y) { return callee(y); }",
        "int pick(int c) { if (c) return 7; return 9; }",
        "int loop(int n) { int s = 0; while (n) { s = s + n; n = n - 1; } "
            "return s; }",
        "int cmp(int a, int b) { return a < b; }",
    };
    for (char const* src : programs) {
        SCOPED_TRACE(src);
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok);
        EXPECT_TRUE(res->hir.intrinsicRegistry().intrinsics().empty())
            << "a c program lowered to a HIR with a NON-empty intrinsic "
               "registry — a frontend now emits intrinsics. The inliner "
               "REFUSES every IntrinsicCall-bearing callee, so this is not a "
               "miscompile; it does mean such a callee silently stops "
               "inlining. To let the frame-INSENSITIVE ones inline again, "
               "build the registration-driven frame-safety attribute "
               "described at D-OPT7-INLINE-FRAME-SENSITIVE-INTRINSIC.";
    }
}

// ── plan 24 (cst_to_hir residuals): the Assign + Switch arms are now flattened
// onto the existing ExprFrame/StmtFrame work-stack drivers. These two strict pins
// witness the FLAT-on-the-main-stack property (RED-on-disable: revert the
// flattening → host recursion overflows the test's normal stack at the chain
// depth → crash) AND the exact byte-identical HIR shape (a flattening that parsed
// without crashing but produced the WRONG structure fails the shape assertions).
// They run the LOWERING on the default test main stack (NO large-stack wrapper) —
// that is the whole point — while the orthogonal recursive parse/analyze of the
// deep tree runs on the 64 MiB worker (the parser's assign-RHS arm is flat since
// plan-24 Stage 5; `analyze` wraps itself in `callOnLargeStack`). The Tree/Hir
// arenas tear down FLATLY (dense ArenaContainer, not a recursive node graph), so
// the deep tree's destruction never overflows the main stack. The cap is raised
// above the depth so the SEMANTIC P_ExpressionTooDeep does not fire (the separate
// too-deep pins prove the cap still fires at its configured point).

// A ~2000-deep RIGHT-assoc assignment chain `a=a=…=a;` lowers (CST→HIR) flat on the
// normal stack to the nested-SeqExpr backbone. Each `=` USED AS A VALUE lowers to
// `SeqExpr([AssignStmt(Ref a, ·)], yield = Ref a)`; the outermost `=` is in
// statement position (an AssignStmt whose value is the (N-1)-op sub-chain). The
// Assign frame turns the deep RHS re-entry into a heap work-stack push, so the
// descent carries flat O(1) host-stack cost. RED-on-disable: restore the recursive
// `lowerBinary` Assign arm → the deep RHS recurses ~2000 host frames → overflow.
TEST(HirLoweringC, DeepRightAssocAssignChainLowersFlatOnNormalStack) {
    constexpr int kOps = 2000;   // 2000 `=` ops → a 2000-deep right-assoc chain

    // `int main(void){ int a; a=a=…=a; return 0; }` — `a` is a simple int lvalue,
    // so the chain recurses PURELY through the value-yielding Assign RHS arm (no
    // prep, no other nesting). Built once; parsed with the cap raised.
    std::string src = "int main(void){ int a; a";
    for (int i = 0; i < kOps; ++i) src += "=a";
    src += "; return 0; }";

    SemanticModel model = analyzeCRaisedCap(std::move(src), kOps + 1000);
    ASSERT_FALSE(model.hasErrors());

    DiagnosticReporter r;
    // Lower on THIS (main) stack — the flat-property witness. A revert recurses
    // here and overflows.
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    // Shape walk: the function body's first statement is the outermost AssignStmt;
    // its value is the (N-1)-op sub-chain = exactly kOps-1 nested SeqExprs, each
    // holding one AssignStmt whose stored value is the next-deeper SeqExpr, the
    // innermost storing a bare Ref. A left-nested or level-dropping mis-lowering
    // breaks the count/shape here (not merely "didn't crash").
    HirNodeId const fnBody =
        res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    // children: [VarDecl a, AssignStmt(...)] (the ExprStmt-position assign lowers
    // to a bare AssignStmt statement).
    auto const bodyStmts = res->hir.children(fnBody);
    ASSERT_GE(bodyStmts.size(), 2u);
    HirNodeId const outerAssign = bodyStmts[1];
    ASSERT_EQ(res->hir.kind(outerAssign), HirKind::AssignStmt);
    EXPECT_EQ(res->hir.kind(res->hir.assignTarget(outerAssign)), HirKind::Ref);

    int seqLevels = 0;
    HirNodeId cur = res->hir.assignValue(outerAssign);   // the (N-1)-op value chain
    while (res->hir.kind(cur) == HirKind::SeqExpr) {
        ++seqLevels;
        // Each value-`=` is `SeqExpr([AssignStmt(Ref a, stored)], yield Ref a)`.
        auto const stmts = res->hir.seqExprStmts(cur);
        ASSERT_EQ(stmts.size(), 1u) << "value-assign SeqExpr has exactly one stmt";
        ASSERT_EQ(res->hir.kind(stmts[0]), HirKind::AssignStmt);
        EXPECT_EQ(res->hir.kind(res->hir.seqExprResult(cur)), HirKind::Ref)
            << "the SeqExpr yields the re-read lvalue";
        cur = res->hir.assignValue(stmts[0]);            // descend to the next level
    }
    // The innermost stored value is the bare `a` Ref (the chain's tail operand).
    EXPECT_EQ(res->hir.kind(cur), HirKind::Ref);
    EXPECT_EQ(seqLevels, kOps - 1)
        << "exactly one nested value-SeqExpr per `=` below the statement-position "
           "outermost assign";
}

// A deeply-NESTED switch (`switch(x){ case 1: switch(x){ case 1: … default: …} … }`)
// lowers (CST→HIR) flat on the normal stack to a SwitchStmt whose single case-arm
// body is the next-inner SwitchStmt, nested kDepth deep. The Switch frame turns the
// per-arm-body `lowerStmt` re-entry into a heap work-stack push, so a switch nested
// in a switch-arm body carries flat O(1) host-stack cost (the recursive form
// recursed `lowerStmt → lowerSwitch → lowerStmt` once per level). RED-on-disable:
// restore the recursive `lowerSwitch` body re-entries → ~kDepth host frames →
// overflow. The shape walk pins the exact innermost-SwitchStmt backbone.
TEST(HirLoweringC, DeepNestedSwitchLowersFlatOnNormalStack) {
    constexpr int kDepth = 1200;   // nesting levels of switch-in-case-body

    // `int main(void){ int x=0; switch(x){case 1: switch(x){case 1: … case 0:
    // return 0; …} default: break;} … return 0; }` — each level is a switch whose
    // `case 1:` body is the next-inner switch; the innermost returns. Each switch
    // also has a `default: break;` so every level is a real multi-arm switch.
    std::string src = "int main(void){ int x=0; ";
    for (int i = 0; i < kDepth; ++i) src += "switch(x){ case 1: ";
    src += "return 0;";                          // innermost case-1 body
    for (int i = 0; i < kDepth; ++i) src += " default: break; }";
    src += " return 0; }";

    SemanticModel model = analyzeCRaisedCap(std::move(src), kDepth + 1000);
    ASSERT_FALSE(model.hasErrors());

    DiagnosticReporter r;
    auto res = lowerToHir(model, r);   // lower on THIS (main) stack — flat witness
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    // Shape walk (c60, Design I-A): descend the SwitchStmt backbone. The body's
    // switch is level 0; each level's body Block holds the case-1 marker
    // (LabelStmt, ordinal = arms[0]) whose inner statement is the next switch,
    // kDepth deep, the innermost case-1 marker wrapping the `return`. A dropped
    // level or mis-built dispatch breaks the count here.
    HirNodeId const fnBody =
        res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    auto const bodyStmts = res->hir.children(fnBody);
    // children: [VarDecl x, SwitchStmt, ReturnStmt].
    ASSERT_GE(bodyStmts.size(), 2u);
    // `case 1: switch(...)` parses as a BARE caseLabel item + the switch as the NEXT
    // switchBodyItem (the speculative switchBodyItem takes the caseLabel first), so
    // each level's body Block is [case-1 marker(Skip), <next switch or return>,
    // default marker]. Descend by locating the case-1 marker then the FIRST
    // SwitchStmt/ReturnStmt sibling after it.
    auto descendCase1 = [&](HirNodeId sw) -> HirNodeId {
        auto const arms = res->hir.switchArms(sw);
        if (arms.size() < 2u || res->hir.caseArmIsDefault(arms[0])
            || !res->hir.caseArmIsDefault(arms[1])) return HirNodeId{};
        std::uint32_t const c1Ord = res->hir.caseArmLabelOrdinal(arms[0]);
        auto const kids = res->hir.children(res->hir.switchBody(sw));
        bool seenMarker = false;
        for (HirNodeId s : kids) {
            if (!seenMarker) {
                if (res->hir.kind(s) == HirKind::LabelStmt
                    && res->hir.labelOrdinal(s) == c1Ord) {
                    seenMarker = true;
                    // The marker may directly wrap the next construct (caseStmt form)
                    // — if so, descend into it.
                    HirNodeId const inner = res->hir.labelBody(s);
                    if (res->hir.kind(inner) == HirKind::SwitchStmt
                        || res->hir.kind(inner) == HirKind::ReturnStmt)
                        return inner;
                }
                continue;
            }
            if (res->hir.kind(s) == HirKind::SwitchStmt
                || res->hir.kind(s) == HirKind::ReturnStmt)
                return s;
        }
        return HirNodeId{};
    };
    HirNodeId cur = bodyStmts[1];
    int switchLevels = 0;
    while (res->hir.kind(cur) == HirKind::SwitchStmt) {
        ++switchLevels;
        HirNodeId const next = descendCase1(cur);
        ASSERT_TRUE(next.valid()) << "case-1 descent failed at level " << switchLevels;
        cur = next;
    }
    // The innermost case-1 body is the `return 0;`.
    EXPECT_EQ(res->hir.kind(cur), HirKind::ReturnStmt);
    EXPECT_EQ(switchLevels, kDepth)
        << "exactly one nested SwitchStmt per source `switch`";
}

// The SEMANTIC-ANALYSIS companion to the lowering pins above, and the host- AND
// sanitizer-INDEPENDENT witness that Pass 1.5's declaration-type walk
// (`resolveDeclTypes`) is FLAT: an explicit heap work-stack, O(1) host stack per
// nesting level (D-PARSE-DEEP-NEST-RECURSION-MEMORY plan-24 Stage 2, the
// resolveDeclTypes MISSED SITE). It analyzes a deeply-NESTED switch on a
// deliberately BOUNDED analysis-worker reserve and asserts the analysis completes
// cleanly.
//
// WHY A RESERVE PARAMETER (not the test's own stack): unlike `lowerToHir` — which
// the pins above call directly on the test's main stack — the semantic analysis
// is reachable ONLY through `analyze()`, which runs `analyzeImpl` on a dedicated
// worker (D-PARSE-DEEP-FRONTEND-STACK). Production uses a 64 MiB reserve; there,
// the recursive form overflowed ONLY under ASan's inflated frames (that is the
// existing `...LowersFlatOnNormalStack` CI failure). To pin the flatness WITHOUT
// depending on a sanitizer, this drives that worker at a small, FIXED reserve via
// `analyze()`'s `deepRecursionReserveBytes` parameter: the FLAT walk uses O(1)
// host frames (one `resolveDeclTypesPost` frame at a time) and fits; a per-level
// RECURSION does not.
//
// RED-ON-DISABLE (every leg, no sanitizer needed): restore the recursive
// `resolveDeclTypes` (one host frame PER switch level) → analyzing kDepth levels
// overflows the kAnalyzeReserveBytes reserve and hard-crashes the worker. The
// margins are large on both sides at the gate's Debug frame sizes: the flat walk's
// O(1) peak (~0.5 MiB even under ASan — the analyzeImpl → driver →
// resolveDeclTypesPost chain, NOT multiplied by depth) sits ~4x under the 2 MiB
// reserve, while kDepth (1500) recursive frames of the (huge) resolveDeclTypes
// function (≥ ~8 KiB each in Debug ⇒ ≥ 12 MiB) sit ~6x over it.
//
// The deep tree's PARSE + teardown run on the standard 64 MiB worker — they are
// ORTHOGONAL deep recursions (the parser's residual paren arm; the tree teardown)
// and must not themselves overflow — so ONLY the analysis runs on the bounded
// reserve. maxExpressionDepth is raised above kDepth so the (statement) nesting is
// admitted without a P_ExpressionTooDeep. (Depth is kept moderate because the
// front end is ~quadratic in nesting depth; the reserve, not the depth, is what
// makes the recursion overflow — so a modest depth on a small reserve is a strict
// pin that still runs fast.)
TEST(HirLoweringC, DeepNestedSwitchAnalyzesFlatOnNormalStack) {
    constexpr int kDepth = 1500;   // switch-in-case-body nesting levels
    // Bounded analysis reserve: ample for the FLAT walk's O(1) host frames (even
    // under ASan), far too small for kDepth RECURSIVE frames of the (huge)
    // resolveDeclTypes function.
    constexpr std::size_t kAnalyzeReserveBytes = std::size_t{2} * 1024 * 1024;

    // `int main(void){ int x=0; switch(x){case 1: … return 0; … default: break;} }`
    // — each level is a switch whose `case 1:` body is the next-inner switch; the
    // innermost returns. Every level also has a `default: break;` (a real multi-arm
    // switch, matching the lowering pin's shape).
    std::string src = "int main(void){ int x=0; ";
    for (int i = 0; i < kDepth; ++i) src += "switch(x){ case 1: ";
    src += "return 0;";
    for (int i = 0; i < kDepth; ++i) src += " default: break; }";
    src += " return 0; }";

    // Parse + build the CU on the 64 MiB worker (orthogonal deep recursion kept
    // off the bounded reserve), then ANALYZE on the bounded reserve — the flatness
    // witness. A bare `int main(){…}` program parses via Tokenizer+Parser directly
    // (no PP) and is ingested via addTree — exactly as analyzeCRaisedCap does.
    // D-TEST-A-TORN-SHIPPED-CONFIG-CRASHES-A-SUITE-INSTEAD-OF-REDDING-IT:
    // this was `ADD_FAILURE() << "loadShipped(...) failed"; std::abort();`.
    // ✔MEASURED against an emptied shipped config, the abort took the whole
    // binary out at 0xC0000409 with no `[  FAILED  ]` line, no case name and
    // no summary -- every sibling test in this executable lost its verdict.
    auto const loaded = dss::test_support::shippedSchemaOrThrow("c");
    std::shared_ptr<GrammarSchema const> schema = loaded;
    auto cu = dss::substrate::callOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes,
        [&]() -> std::shared_ptr<CompilationUnit const> {
            auto srcBuf = SourceBuffer::fromString(std::move(src), "<deepanalyze>");
            Tokenizer tk{srcBuf, schema, DiagnosticBudget::libraryDefault()};
            auto [stream, lexDiags] = std::move(tk).tokenize();
            ParserConfig pcfg;
            pcfg.maxExpressionDepth = static_cast<std::size_t>(kDepth) + 1000;
            Parser p{srcBuf, schema, std::move(stream),
                     DiagnosticBudget::libraryDefault(), std::move(pcfg),
                     std::move(lexDiags)};
            ParseResult result = std::move(p).parse();
            if (result.tree.diagnostics().hasErrors())
                ADD_FAILURE() << "deep nested-switch parse produced errors";
            UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
            builder.addTree(std::move(result.tree));
            return std::make_shared<CompilationUnit>(std::move(builder).finish());
        });

    // Pre-fix (recursive resolveDeclTypes): overflows the bounded reserve at kDepth
    // → crash. Post-fix (flat driver): O(1) host stack → clean, error-free model.
    SemanticModel model =
        analyze(cu, DiagnosticBudget::libraryDefault(),
                DataModel::Lp64, std::nullopt, std::nullopt, std::nullopt,
                std::nullopt, LongDoubleFormat::None,
                /*target=*/nullptr,   // inline-asm P5: no asm here, no target needed
                /*deepRecursionReserveBytes=*/kAnalyzeReserveBytes);
    EXPECT_FALSE(model.hasErrors())
        << "deep nested-switch analysis must complete cleanly on a bounded stack "
           "— Pass 1.5 resolveDeclTypes is a flat heap work-stack, not recursion";
}

// ─────────────────────────── FC-F1: ++/-- (Cluster F item F1) ───────────────
// Prefix `++`/`--` (pre + post, integer + pointer). Postfix-int already worked
// (IncrementInStatementPositionLowers / ValueYieldingIncrementLowersToSeqExpr);
// these pin the NEW behavior: prefix parses+lowers, prefix yields the NEW value
// (vs postfix's OLD), and a POINTER ++/-- scales by sizeof(*p) via the Index→Gep
// path (NOT a bare 1-byte BinaryOp Add). Strict + red-on-disable.

TEST(HirLoweringC, PrefixIncrementStatementLowersToAssign) {
    // `++x;` in statement position → a clean AssignStmt + BinaryOp (NOT a value-
    // position SeqExpr). Proves MF-2 (the unaryExprRule arm of lowerStmtExprCore).
    SemanticModel model = analyzeC("void f(int x) { ++x; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId stmt = res->hir.children(body)[0];
    ASSERT_EQ(res->hir.kind(stmt), HirKind::AssignStmt)
        << "prefix ++x; must lower to an AssignStmt, not a SeqExpr";
    // The stored value is `x + 1` (a BinaryOp), not a SeqExpr.
    EXPECT_EQ(res->hir.kind(res->hir.assignValue(stmt)), HirKind::BinaryOp);
}

TEST(HirLoweringC, PrefixIncrementValueYieldsNewValue) {
    // `return ++x;` — prefix yields the NEW value: lowers to a SeqExpr that
    // STORES then yields a fresh READ of the lvalue (the post-store value), with
    // NO leading temp VarDecl (the distinguishing mark vs postfix). For a simple
    // local, that read is a Ref to `x` ITSELF — and crucially it names the SAME
    // symbol the store writes to (so the yield is the post-store value of x).
    SemanticModel model = analyzeC("int f(int x) { return ++x; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
    HirNodeId ret  = res->hir.children(body)[0];
    HirNodeId val  = *res->hir.returnValue(ret);
    ASSERT_EQ(res->hir.kind(val), HirKind::SeqExpr);
    // stmts: [assign x = x+1] ONLY (no tmp save).
    ASSERT_EQ(res->hir.seqExprStmts(val).size(), 1u)
        << "prefix has NO leading temp VarDecl (postfix saves the old value first)";
    HirNodeId store = res->hir.seqExprStmts(val)[0];
    ASSERT_EQ(res->hir.kind(store), HirKind::AssignStmt);
    // The yielded value is a fresh Ref to the SAME symbol the store targets (x) —
    // i.e. the post-store value of x, NOT a saved temp.
    HirNodeId yield = res->hir.seqExprResult(val);
    ASSERT_EQ(res->hir.kind(yield), HirKind::Ref);
    EXPECT_EQ(res->hir.payload(yield), res->hir.payload(res->hir.assignTarget(store)))
        << "prefix yields a read of x itself (the new value), not a temp";
}

TEST(HirLoweringC, PrefixVsPostfixDistinctReturn) {
    // The make-or-break: prefix and postfix value-position lowerings DIFFER.
    // Postfix `x++`: SeqExpr stmts = [tmp = x (VarDecl), x = x+1], result = Ref(tmp)
    //   → yields the OLD value (2 stmts; the yielded Ref names the TEMP, a symbol
    //   distinct from x — the store target).
    // Prefix `++x`: SeqExpr stmts = [x = x+1], result = Ref(x)
    //   → yields the NEW value (1 stmt; the yielded Ref names x = the store target).
    DiagnosticReporter rPost, rPre;
    SemanticModel mPost = analyzeC("int f(int x) { return x++; }");
    SemanticModel mPre  = analyzeC("int f(int x) { return ++x; }");
    ASSERT_FALSE(mPost.hasErrors());
    ASSERT_FALSE(mPre.hasErrors());
    auto post = lowerToHir(mPost, rPost);
    auto pre  = lowerToHir(mPre, rPre);
    ASSERT_TRUE(post->ok);
    ASSERT_TRUE(pre->ok);
    auto seqOf = [](auto const& res) {
        HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
        return *res->hir.returnValue(res->hir.children(body)[0]);
    };
    HirNodeId postSeq = seqOf(post);
    HirNodeId preSeq  = seqOf(pre);
    ASSERT_EQ(post->hir.kind(postSeq), HirKind::SeqExpr);
    ASSERT_EQ(pre->hir.kind(preSeq), HirKind::SeqExpr);
    // Distinct stmt counts: postfix saves the old value first (2), prefix does not (1).
    EXPECT_EQ(post->hir.seqExprStmts(postSeq).size(), 2u);
    EXPECT_EQ(pre->hir.seqExprStmts(preSeq).size(), 1u);
    // The make-or-break value distinction, symbol-id-internals-free: in BOTH the
    // last stmt is the store `x = x ± 1`. Prefix yields a read of the SAME symbol
    // the store targets (x = the NEW value); postfix yields a DIFFERENT symbol
    // (the saved temp = the OLD value).
    auto storeTargetSym = [](auto const& res, HirNodeId seq) {
        auto s = res->hir.seqExprStmts(seq);
        return res->hir.payload(res->hir.assignTarget(s.back()));
    };
    HirNodeId postYield = post->hir.seqExprResult(postSeq);
    HirNodeId preYield  = pre->hir.seqExprResult(preSeq);
    ASSERT_EQ(post->hir.kind(postYield), HirKind::Ref);
    ASSERT_EQ(pre->hir.kind(preYield), HirKind::Ref);
    EXPECT_NE(post->hir.payload(postYield), storeTargetSym(post, postSeq))
        << "postfix yields the OLD value (a temp), distinct from the store target";
    EXPECT_EQ(pre->hir.payload(preYield), storeTargetSym(pre, preSeq))
        << "prefix yields the NEW value (a read of x itself = the store target)";
}

TEST(HirLoweringC, PointerPostfixIncrementScalesViaGep) {
    // `*(p++)` on `int* p` — the pointer step must route through the Index→Gep
    // element-scaling path (`AddressOf(Index(lvRead(p), ±1, int))`), NOT a bare
    // `BinaryOp Add(ptr, 1)` (which would step 1 BYTE, not sizeof(int)). The
    // lowered HIR therefore contains an Index + AddressOf for the step, and the
    // pointer SeqExpr's stored value is NOT a BinaryOp.
    SemanticModel model = analyzeC("int f(int* p) { return *(p++); }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    // The scaled-step nodes are present.
    HirNodeId idx  = findFirstByKind(res->hir, res->hir.root(), HirKind::Index);
    EXPECT_TRUE(idx.valid())
        << "pointer ++ must lower through an Index node (the sizeof-scaled Gep path)";
    // Find the SeqExpr (the p++ value) and assert its STORED value is the scaled
    // AddressOf(Index(...)) pointer, never a bare BinaryOp Add on the pointer.
    HirNodeId seq = findFirstByKind(res->hir, res->hir.root(), HirKind::SeqExpr);
    ASSERT_TRUE(seq.valid());
    auto stmts = res->hir.seqExprStmts(seq);
    ASSERT_FALSE(stmts.empty());
    HirNodeId store = stmts.back();   // the lvWrite (AssignStmt)
    ASSERT_EQ(res->hir.kind(store), HirKind::AssignStmt);
    EXPECT_EQ(res->hir.kind(res->hir.assignValue(store)), HirKind::AddressOf)
        << "pointer step value must be AddressOf(Index(...)), not BinaryOp Add";
    EXPECT_NE(res->hir.kind(res->hir.assignValue(store)), HirKind::BinaryOp);
}

TEST(HirLoweringC, PointerIncDecStatementPositionScalesViaGep) {
    // The plan-lock's explicit call-out: cover the OTHER two MF-1 sites
    // (statement position) too, not only value position — `p++;` and `++p;` must
    // ALSO route through the Index→Gep scaling, so the shared-incidental-coverage
    // trap (only value-position exercised) cannot hide a stmt-site regression.
    for (char const* src : {"void f(int* p) { p++; }", "void f(int* p) { ++p; }"}) {
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors()) << src;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << src << ": " << (r.all().empty() ? "" : r.all()[0].actual);
        HirNodeId body = res->hir.functionBody(res->hir.moduleDecls(res->hir.root())[0]);
        HirNodeId stmt = res->hir.children(body)[0];
        ASSERT_EQ(res->hir.kind(stmt), HirKind::AssignStmt) << src;
        // The stored value is the scaled pointer AddressOf(Index(...)), NOT a
        // BinaryOp Add stepping a single byte.
        EXPECT_EQ(res->hir.kind(res->hir.assignValue(stmt)), HirKind::AddressOf) << src;
        EXPECT_TRUE(findFirstByKind(res->hir, stmt, HirKind::Index).valid()) << src;
    }
}

TEST(HirLoweringC, NonLvalueIncDecFailsLoud) {
    // A manifest rvalue operand (`5++` / `++5`) is not a modifiable lvalue —
    // fail loud with S_IncDecNeedsModifiableLvalue (MF-4), never a silent
    // write-back to a non-object.
    for (char const* src : {"int f() { return 5++; }", "int f() { return ++5; }"}) {
        SemanticModel model = analyzeC(src);
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_FALSE(res->ok) << src;
        EXPECT_EQ(countCode(r, DiagnosticCode::S_IncDecNeedsModifiableLvalue), 1u) << src;
    }
}

// c28 D-CSUBSET-LOCAL-TYPE-DEFINITION: a BLOCK-SCOPED struct/union/enum
// DEFINITION with NO declarator (`struct S { … };` as a statement — sqlite3.c:
// 68508 walMergesort) lowers CLEANLY: the type is minted + interned at the
// SEMANTIC tier (the unified c25 define path), so the no-declarator statement
// needs no runtime HIR node, and a later `struct S v; v.a` resolves through the
// interned type. RED-on-disable: revert the optional-list grammar tweak → P0009
// at parse (the front-end gate in analyzeC fails first).
TEST(HirLoweringC, LocalStructDefinitionLowersClean) {
    SemanticModel model = analyzeC(
        "int main(void){ struct S { int a; int b; }; struct S v; v.a = 1; "
        "return v.a; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 0u)
        << "a block-scoped struct DEFINITION declares a type — must NOT fail loud";
}

// c28: a local define+declare (`struct S { … } v;`) still lowers cleanly (the
// declarator IS present — the ordinary path), and a local REFERENCE to an
// outer-defined tag (`struct S v;`) resolves. RED-on-disable: if the optional
// list mis-routed the declarator-present form, these regress.
TEST(HirLoweringC, LocalStructDefineAndDeclareAndRefLowerClean) {
    SemanticModel m1 = analyzeC(
        "int main(void){ struct S { int a; } v; v.a = 7; return v.a; }\n");
    ASSERT_FALSE(m1.hasErrors())
        << (m1.diagnostics().all().empty() ? std::string{}
            : m1.diagnostics().all()[0].actual);
    DiagnosticReporter r1;
    auto res1 = lowerToHir(m1, r1);
    EXPECT_TRUE(res1->ok) << (r1.all().empty() ? "" : r1.all()[0].actual);

    SemanticModel m2 = analyzeC(
        "struct S { int a; };\n"
        "int main(void){ struct S v; v.a = 3; return v.a; }\n");
    ASSERT_FALSE(m2.hasErrors())
        << (m2.diagnostics().all().empty() ? std::string{}
            : m2.diagnostics().all()[0].actual);
    DiagnosticReporter r2;
    auto res2 = lowerToHir(m2, r2);
    EXPECT_TRUE(res2->ok) << (r2.all().empty() ? "" : r2.all()[0].actual);
}

// c28: a NON-defining no-declarator LOCAL (`int;`) declares nothing (C 6.7p2).
// Mirroring the top-level `int ;` (TopLevelDeclaresNothingFailsLoudNoCrash), the
// front end (parse + semantic) ACCEPTS it, and the HIR lowering FAILS LOUD with
// S_DeclarationDeclaresNothing — the local twin of the top-level no-object path
// (lowerVarLikeInto: an empty list-mode declarator carrier with NO composite
// specifier in the head). Must NOT crash and must NOT silently accept.
// RED-on-disable: drop the lowerVarLikeInto declares-nothing guard → res->ok
// stays true and the count is 0 (a silent accept).
TEST(HirLoweringC, LocalDeclaresNothingFailsLoud) {
    SemanticModel model = analyzeC("int main(void){ int; return 0; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "parse + semantic accept `int;` — the constraint is HIR-tier";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok)
        << "`int;` (no declarator, no tag) declares nothing — lowering must fail loud";
    EXPECT_EQ(countCode(r, DiagnosticCode::S_DeclarationDeclaresNothing), 1u)
        << "exactly one S_DeclarationDeclaresNothing for the empty local `int;`";
}

// c89 (D-CSUBSET-SIZEOF-VALUE-OPERAND-TYPE): the VALUE-form sizeof operand is
// sized by its full EXPRESSION type (C 6.5.3.4) — the tier-boundary pin for the
// Pass-2 sizeofValueRule operand stamp at its exact consumption point: the
// HirKind::SizeOf node's TypeRef child. Pre-c89, Pass 2 left operator nodes
// unstamped and lowerSizeof's resolveStampedTypeBelow DFS sailed past the
// unstamped `*p`/`tab[0]` into the base IDENTIFIER leaf: sizeof(*p) carried
// Ptr<Big> (folding 8, not 48), sizeof(tab[0]) carried the WHOLE Array (336),
// and sizeof(&tab) carried the Array — sqlite's pthreadMutexAlloc
// `sqlite3MallocZero(sizeof(*p))` under-allocated 8 for the 40-byte recursive
// mutex → glibc's own mutex-init writes clobbered the malloc top chunk →
// deterministic sysmalloc SIGABRT on every invocation (the c88 smoke wall).
// The corpus witness (examples/c/sizeof_value_expression) proves the
// folded VALUES end-to-end; THIS pin names the tier, so a future Pass-2
// refactor that drops the operand stamp fails HERE, not three tiers later.
// RED-ON-DISABLE: revert the Pass-2 stamp → [0] kind flips Struct→Ptr,
// [1] Struct→Array, [2] Struct→Ptr, [3] pointee flips Array→Big(Struct);
// every EXPECT below flips.
TEST(HirLoweringC, SizeofValueOperandCarriesExpressionType) {
    SemanticModel model = analyzeC(
        "struct Big { double a; double b; double c; double d; double e; "
        "double f; };\n"
        "static struct Big tab[7];\n"
        "unsigned long long f(struct Big *p) {\n"
        "    return sizeof(*p) + sizeof(tab[0]) + sizeof(p[0]) + sizeof(&tab);\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    ASSERT_TRUE(fn.valid());
    // Collect every SizeOf in the body, pre-order = source order (the `+`
    // chain associates left, so the DFS meets them left-to-right).
    std::vector<HirNodeId> sizeofs;
    auto const collect = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::SizeOf) sizeofs.push_back(n);
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    collect(collect, res->hir.functionBody(fn));
    ASSERT_EQ(sizeofs.size(), 4u) << "four value-form sizeof sites expected";
    auto const& ti = model.lattice().interner();
    auto const sizedType = [&](HirNodeId szNode) -> TypeId {
        auto const kids = res->hir.children(szNode);
        EXPECT_EQ(kids.size(), 1u) << "SizeOf carries exactly [TypeRef]";
        return kids.empty() ? TypeId{} : res->hir.typeId(kids.front());
    };
    // [0] sizeof(*p): the POINTEE struct (the sqlite pthreadMutexAlloc shape).
    TypeId const t0 = sizedType(sizeofs[0]);
    ASSERT_TRUE(t0.valid());
    EXPECT_EQ(ti.kind(t0), TypeKind::Struct)
        << "sizeof(*p) must size the pointee STRUCT, not the pointer";
    EXPECT_EQ(ti.operands(t0).size(), 6u) << "the 6-double Big, 48 bytes";
    // [1] sizeof(tab[0]): the array ELEMENT, never the whole array.
    TypeId const t1 = sizedType(sizeofs[1]);
    ASSERT_TRUE(t1.valid());
    EXPECT_EQ(ti.kind(t1), TypeKind::Struct)
        << "sizeof(tab[0]) must size the ELEMENT, not the whole Array "
           "(the ArraySize idiom's denominator — pre-c89 it folded to the "
           "numerator and ArraySize collapsed to 1)";
    // [2] sizeof(p[0]): index through a pointer — the element again.
    TypeId const t2 = sizedType(sizeofs[2]);
    ASSERT_TRUE(t2.valid());
    EXPECT_EQ(ti.kind(t2), TypeKind::Struct)
        << "sizeof(p[0]) must size the pointee STRUCT, not the pointer";
    // [3] sizeof(&tab): address-of yields a POINTER (8), never the array (336).
    TypeId const t3 = sizedType(sizeofs[3]);
    ASSERT_TRUE(t3.valid());
    ASSERT_EQ(ti.kind(t3), TypeKind::Ptr)
        << "sizeof(&tab) must size a POINTER-to-array";
    ASSERT_EQ(ti.operands(t3).size(), 1u);
    EXPECT_EQ(ti.kind(ti.operands(t3)[0]), TypeKind::Array)
        << "…whose pointee is the Array itself";
}

// C11/C23 6.5.3.4: `_Alignof(T)` / `alignof(T)` lower to a core HirKind::AlignOf
// node carrying the QUERIED type on its single [TypeRef] child (mirroring SizeOf).
// Covers both spellings AND a struct type-name. RED-ON-DISABLE: drop the
// lowerAlignof dispatch → the operand alt tries to type `_Alignof`/`alignof` as an
// expression and the front end fails (no AlignOf node reaches the body).
TEST(HirLoweringC, AlignofLowersToAlignOfNodeCarryingQueriedType) {
    SemanticModel model = analyzeC(
        "struct CharDouble { char c; double d; };\n"
        "unsigned long long f(void) {\n"
        "    return _Alignof(double) + alignof(char) "
        "+ _Alignof(struct CharDouble);\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    ASSERT_TRUE(fn.valid());
    // Collect every AlignOf in the body, pre-order = source order.
    std::vector<HirNodeId> alignofs;
    auto const collect = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::AlignOf) alignofs.push_back(n);
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    collect(collect, res->hir.functionBody(fn));
    ASSERT_EQ(alignofs.size(), 3u) << "three _Alignof/alignof sites expected";
    auto const& ti = model.lattice().interner();
    auto const queriedType = [&](HirNodeId n) -> TypeId {
        auto const kids = res->hir.children(n);
        EXPECT_EQ(kids.size(), 1u) << "AlignOf carries exactly [TypeRef]";
        // The AlignOf node itself is size_t (U64) — its result type.
        EXPECT_EQ(ti.kind(res->hir.typeId(n)), TypeKind::U64)
            << "_Alignof yields size_t";
        return kids.empty() ? TypeId{} : res->hir.typeId(kids.front());
    };
    // [0] _Alignof(double): the queried type is the primitive double (F64).
    TypeId const t0 = queriedType(alignofs[0]);
    ASSERT_TRUE(t0.valid());
    EXPECT_EQ(ti.kind(t0), TypeKind::F64) << "_Alignof(double) queries F64";
    // [1] alignof(char): the C23 spelling, queried type char (TypeKind::Char).
    TypeId const t1 = queriedType(alignofs[1]);
    ASSERT_TRUE(t1.valid());
    EXPECT_EQ(ti.kind(t1), TypeKind::Char) << "alignof(char) queries char";
    // [2] _Alignof(struct CharDouble): the whole struct type.
    TypeId const t2 = queriedType(alignofs[2]);
    ASSERT_TRUE(t2.valid());
    EXPECT_EQ(ti.kind(t2), TypeKind::Struct)
        << "_Alignof(struct CharDouble) queries the STRUCT type";
}

// c90 (D-CSUBSET-ASSIGN-VALUE-RHS-COERCE): a plain `=` used as a VALUE stores
// the RHS COERCED to the lvalue's type (C 6.5.16p3) — the tier-boundary pin
// for `finishAssign`'s plain arm (and its `lowerBinary` Assign-arm mirror) at
// the exact node the property lives on: the SeqExpr's AssignStmt VALUE child
// must be a Cast carrying the LVALUE's type whenever the RHS type differs.
// Pre-c90 the raw RHS node was stored un-coerced and the MIR store executed
// at the RHS's width: an i16 comma-assign from a wider RHS partial-stored
// (sqlite estimateTableWidth's `for(i=pTab->nCol, ...)` left i's upper half
// stale → the 3822-element overrun → the every-SQL-statement SIGSEGV), and a
// wider RHS over-stored past a sub-int lvalue (neighbor corruption). The
// corpus witness (examples/c/assign_value_coerce) proves the VALUES
// end-to-end on all run legs; THIS pin names the tier, so a refactor that
// drops either coerce fails HERE, not three tiers later at runtime.
// RED-ON-DISABLE: revert `stored = coerce(result, lv.type).id` → the stored
// value's kind flips Cast→Ref (the raw I32/F64 RHS) and its type flips
// I16→I32/F64; the Cast asserts below flip. The SeqExpr node type + yield-Ref
// type (I16) are the pre-existing yield thread and stay green.
TEST(HirLoweringC, PlainAssignAsValueStoresRhsCoercedToLvalueType) {
    SemanticModel model = analyzeC(
        "long long g(int v, double d) {\n"
        "    short s; int y; long long L;\n"
        "    y = (s = v);\n"    // int RHS -> i16 lvalue (the sqlite shape)
        "    L = (s = d);\n"    // double RHS -> i16 lvalue (the float leg)
        "    return y + L + s;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    ASSERT_TRUE(fn.valid());
    // Collect the value-position assigns: every SeqExpr in the body, pre-order
    // = source order → [0] = (s = v), [1] = (s = d).
    std::vector<HirNodeId> seqs;
    auto const collect = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::SeqExpr) seqs.push_back(n);
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    collect(collect, res->hir.functionBody(fn));
    ASSERT_EQ(seqs.size(), 2u) << "two value-position plain assigns expected";
    auto const& ti = model.lattice().interner();
    TypeKind const rhsKinds[2] = {TypeKind::I32, TypeKind::F64};
    for (std::size_t k = 0; k < 2; ++k) {
        HirNodeId const seq = seqs[k];
        // The SeqExpr (the assignment-as-value) carries the LVALUE's type.
        TypeId const seqTy = res->hir.typeId(seq);
        ASSERT_TRUE(seqTy.valid());
        EXPECT_EQ(ti.kind(seqTy), TypeKind::I16)
            << "[" << k << "] the assign-as-value expression is lvalue-typed";
        auto const stmts = res->hir.seqExprStmts(seq);
        ASSERT_EQ(stmts.size(), 1u) << "[" << k << "] simple lvalue: no prep";
        ASSERT_EQ(res->hir.kind(stmts[0]), HirKind::AssignStmt);
        // THE c90 PROPERTY: the stored value is the RHS wrapped in a Cast to
        // the LVALUE's type — never the raw RHS at its own width.
        HirNodeId const stored = res->hir.assignValue(stmts[0]);
        ASSERT_EQ(res->hir.kind(stored), HirKind::Cast)
            << "[" << k << "] plain-=-as-value must COERCE the RHS to the "
               "lvalue type (D-CSUBSET-ASSIGN-VALUE-RHS-COERCE)";
        TypeId const storedTy = res->hir.typeId(stored);
        ASSERT_TRUE(storedTy.valid());
        EXPECT_EQ(ti.kind(storedTy), TypeKind::I16)
            << "[" << k << "] the stored Cast carries the lvalue's I16 type";
        auto const castKids = res->hir.children(stored);
        ASSERT_EQ(castKids.size(), 1u);
        TypeId const rawTy = res->hir.typeId(castKids[0]);
        ASSERT_TRUE(rawTy.valid());
        EXPECT_EQ(ti.kind(rawTy), rhsKinds[k])
            << "[" << k << "] …over the raw RHS at its own type";
        // The 6.5.16p3 yield thread: the expression's value is the lvalue
        // re-read, typed by the lvalue (pre-existing, kept pinned).
        HirNodeId const yield = res->hir.seqExprResult(seq);
        ASSERT_EQ(res->hir.kind(yield), HirKind::Ref);
        TypeId const yieldTy = res->hir.typeId(yield);
        ASSERT_TRUE(yieldTy.valid());
        EXPECT_EQ(ti.kind(yieldTy), TypeKind::I16)
            << "[" << k << "] the assignment's VALUE is the post-conversion "
               "lvalue read (C 6.5.16p3)";
    }
}

// c91 (D-CSUBSET-ARRAY-DECAY-IN-COMPARISON + D-CSUBSET-ARRAY-DECAY-IN-CONDITION,
// closing the D-CSUBSET-ARRAY-DECAY-POINTER-IDENTITY HIR surface):
// an ARRAY operand of a comparison, a condition, or `!` decays to Ptr<elem>
// (C 6.3.2.1p3) THROUGH THE ONE coerce funnel — the tier-boundary pin at the
// exact nodes the property lives on. Pre-c91 the operand kept its Array type
// and was VALUE-lowered at MIR: a member/global array operand emitted an
// aggregate Load, so the compare read the array's first BYTES as a "pointer"
// (sqlite sqlite3ParserFinalize `pParser->yystack != pParser->yystk0` →
// always-unequal → freed the on-stack parser → the every-SQL-statement
// SIGABRT), and an Array condition reached the CondBr raw
// (I_TerminatorTypeMismatch). The corpus witness
// (examples/c/array_decay_pointer_identity) proves the VALUES
// end-to-end on all run legs; THIS pin names the HIR tier, so a refactor
// that drops any of the three decay arms fails HERE even while the MIR
// value-read backstop (the c63-twin arms) keeps the end-to-end behavior
// correct. RED-ON-DISABLE (each arm independently):
//   - combineBinary comparison arm reverted → the Ne's rhs stays a raw
//     Array-typed MemberAccess (the Cast asserts flip);
//   - coerceCondition Array arm reverted → `if (g)` synthesizes no Ne
//     (the two-Ne count flips);
//   - combineUnaryOp `!` arm reverted → Not's operand stays a raw
//     Array-typed Ref (the Cast asserts flip).
TEST(HirLoweringC, ArrayComparisonConditionOperandsDecayToPointer) {
    SemanticModel model = analyzeC(
        "struct P { int *stack; int stk0[4]; };\n"
        "int g[4];\n"
        "int f(struct P *p) {\n"
        "    int r = 0;\n"
        "    if (p->stack != p->stk0) r = 1;\n"   // the sqlite ParserFinalize shape
        "    if (g) r = r + 2;\n"                 // Array condition
        "    if (!g) r = r + 4;\n"                // `!array`
        "    return r;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    ASSERT_TRUE(fn.valid());
    auto const& ti = model.lattice().interner();
    // Collect the Ne BinaryOps and the Not UnaryOps, pre-order = source order.
    std::vector<HirNodeId> nes;
    std::vector<HirNodeId> nots;
    auto const collect = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::BinaryOp
            && isCoreOp(res->hir.payload(n))
            && decodeCoreOp(res->hir.payload(n)) == HirOpKind::Ne)
            nes.push_back(n);
        if (res->hir.kind(n) == HirKind::UnaryOp
            && isCoreOp(res->hir.payload(n))
            && decodeCoreOp(res->hir.payload(n)) == HirOpKind::Not)
            nots.push_back(n);
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    collect(collect, res->hir.functionBody(fn));
    // [0] the source `!=`; [1] the Ne coerceCondition SYNTHESIZES for `if (g)`
    // (the `!g` condition is already Bool — Not — so no third Ne).
    ASSERT_EQ(nes.size(), 2u)
        << "the member compare + the SYNTHESIZED `if (g)` truth test "
           "(D-CSUBSET-ARRAY-DECAY-IN-CONDITION: a raw Array cond emits none)";
    // THE c91 comparison property: the Array-typed member operand is wrapped
    // in a decay Cast to Ptr<elem>; the raw MemberAccess keeps its Array type
    // underneath.
    auto const expectDecayCast = [&](HirNodeId operand, HirKind rawKind,
                                     char const* what) {
        ASSERT_EQ(res->hir.kind(operand), HirKind::Cast)
            << what << ": the Array operand must be wrapped in the coerce "
                       "decay Cast (C 6.3.2.1p3)";
        TypeId const ct = res->hir.typeId(operand);
        ASSERT_TRUE(ct.valid());
        ASSERT_EQ(ti.kind(ct), TypeKind::Ptr)
            << what << ": the decay Cast carries Ptr<elem>";
        EXPECT_EQ(ti.kind(ti.operands(ct)[0]), TypeKind::I32)
            << what << ": …whose pointee is the ELEMENT type (int)";
        auto const kids = res->hir.children(operand);
        ASSERT_EQ(kids.size(), 1u);
        EXPECT_EQ(res->hir.kind(kids[0]), rawKind)
            << what << ": …over the raw lvalue node";
        TypeId const rt = res->hir.typeId(kids[0]);
        ASSERT_TRUE(rt.valid());
        EXPECT_EQ(ti.kind(rt), TypeKind::Array)
            << what << ": …which keeps its Array type underneath";
    };
    {   // [0] `p->stack != p->stk0`: lhs is the Ptr member (no cast), rhs
        // is the DECAYED Array member.
        auto const kids = res->hir.children(nes[0]);
        ASSERT_EQ(kids.size(), 2u);
        TypeId const lt = res->hir.typeId(kids[0]);
        ASSERT_TRUE(lt.valid());
        EXPECT_EQ(ti.kind(lt), TypeKind::Ptr)
            << "lhs (p->stack) is already a pointer — never wrapped";
        expectDecayCast(kids[1], HirKind::MemberAccess,
                        "comparison rhs (p->stk0)");
    }
    {   // [1] `if (g)`: coerceCondition decays the Array Ref then re-enters
        // its own Ptr arm → Ne(decayCast(g), nullPtrCast), typed Bool.
        TypeId const bt = res->hir.typeId(nes[1]);
        ASSERT_TRUE(bt.valid());
        EXPECT_EQ(ti.kind(bt), TypeKind::Bool)
            << "the synthesized truth test is Bool-typed";
        auto const kids = res->hir.children(nes[1]);
        ASSERT_EQ(kids.size(), 2u);
        expectDecayCast(kids[0], HirKind::Ref, "condition operand (g)");
        EXPECT_EQ(res->hir.kind(kids[1]), HirKind::Cast)
            << "…compared against the synthetic null-pointer Cast";
    }
    {   // `!g`: the Not's operand is the decayed Array Ref.
        ASSERT_EQ(nots.size(), 1u);
        auto const kids = res->hir.children(nots[0]);
        ASSERT_EQ(kids.size(), 1u);
        expectDecayCast(kids[0], HirKind::Ref, "`!` operand (g)");
    }
}

// c-TF (D-CSUBSET-ARRAY-DECAY-IN-DEREF): unary `*` applied to an ARRAY operand
// decays the array to Ptr<elem> (C 6.3.2.1p3 — the SAME law as c59's additive
// decay) BEFORE the Deref types its result. `derefResultType` is Ptr-only (the
// law SHARED with the semantic-tier typer), so pre-fix `*(arrayName)` reached it
// as a raw Array → InvalidType → a TYPELESS Deref → H0001 (the sqlite
// getVarint32(zBuf,…) test3.c blocker; `zBuf` is `unsigned char zBuf[100]`).
// `arrayName[0]` (Index → indexResultType types an Array base) and `*(arrayName
// + 0)` (c59) already lowered — this is the DIRECT-deref-of-an-array hole. This
// pin names the HIR tier: the Deref is TYPED as the element and its operand is
// the synthetic Array→Ptr decay Cast. The corpus witness (examples/c/
// array_decay_deref) proves the VALUES end-to-end on every run leg. RED-ON-
// DISABLE: revert the combineUnaryOp Deref decay arm → the operand stays a raw
// Array Ref and the Deref is typeless (both asserts flip; the file no longer
// lowers clean).
TEST(HirLoweringC, DerefOfArrayOperandDecaysToPointer) {
    SemanticModel model = analyzeC(
        "int main() { int a[4]; a[0] = 7; return *(a); }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto const& ti = model.lattice().interner();
    auto decls = res->hir.moduleDecls(res->hir.root());
    ASSERT_GE(decls.size(), 1u);
    HirNodeId const mainBody = res->hir.functionBody(decls.back());
    auto const stmts = res->hir.children(mainBody);
    ASSERT_GE(stmts.size(), 1u);
    HirNodeId const ret = stmts.back();
    auto const rv = res->hir.returnValue(ret);
    ASSERT_TRUE(rv.has_value()) << "the last statement is `return *(a);`";
    HirNodeId const deref = *rv;
    // `*(a)` is a Deref TYPED as the element (int → I32), NOT the pre-fix
    // TYPELESS node the HirVerifier reported as H0001.
    ASSERT_EQ(res->hir.kind(deref), HirKind::Deref) << "`*(a)` lowers to a Deref";
    TypeId const dt = res->hir.typeId(deref);
    ASSERT_TRUE(dt.valid())
        << "the Deref MUST be typed (pre-fix a Deref of an Array was TYPELESS → H0001)";
    EXPECT_EQ(ti.kind(dt), TypeKind::I32) << "`*(int[4])` yields the element type int";
    // Its single operand is the synthetic Array→Ptr decay Cast (C 6.3.2.1p3),
    // Ptr<int>, over the raw Array Ref underneath.
    auto const kids = res->hir.children(deref);
    ASSERT_EQ(kids.size(), 1u);
    HirNodeId const operand = kids[0];
    ASSERT_EQ(res->hir.kind(operand), HirKind::Cast)
        << "the Array operand of unary `*` must be wrapped in the coerce decay Cast";
    TypeId const ct = res->hir.typeId(operand);
    ASSERT_TRUE(ct.valid());
    ASSERT_EQ(ti.kind(ct), TypeKind::Ptr) << "the decay Cast carries Ptr<elem>";
    auto const elem = ti.operands(ct);
    ASSERT_FALSE(elem.empty());
    EXPECT_EQ(ti.kind(elem[0]), TypeKind::I32)
        << "…whose pointee is the ELEMENT type (int)";
}

// D-CSUBSET-VARIADIC-DEFAULT-ARG-PROMOTION: C 6.5.2.2p6 default ARGUMENT
// promotions for a SCALAR variadic-tail arg are applied in `coerceCallArg` — a
// sub-int integer promotes to `int` (a Cast typed I32), a `float` widens to
// `double` (a Cast typed F64), and an arg already at/above the promotion floor
// (`int`, `double`, …) passes through with NO cast. The signedness-keyed
// SExt/ZExt of the integer promotion is mapCast's downstream concern (proven
// end-to-end — signed -1 vs unsigned 65535 — by the corpus witness
// examples/c/varargs_default_arg_promotion). RED-ON-DISABLE: revert the
// scalar promotion in coerceCallArg → the narrow/float arg reaches the Call as
// its raw I16/I8/F32 (the Cast + type asserts flip); pre-fix this was the
// silent miscompile that made sqlite's `sqlite3_expert_new` see nArg=-1 as
// 65535.
TEST(HirLoweringC, VariadicTailScalarArgGetsDefaultArgPromotion) {
    // Lower `int main() { <decls> return va(1, x); }` against a variadic `va`,
    // and report the (nodeKind, typeKind) of the variadic-tail arg `x` (the
    // Call's LAST child — [callee, fixed `1`, tail `x`]).
    auto tailArg = [](std::string decls) -> std::pair<HirKind, TypeKind> {
        SemanticModel model = analyzeC(
            "int va(int n, ...);\n"
            "int main() { " + decls + " return va(1, x); }\n");
        EXPECT_FALSE(model.hasErrors())
            << (model.diagnostics().all().empty() ? std::string{}
                : model.diagnostics().all()[0].actual);
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
        auto const& ti = model.lattice().interner();
        auto ds = res->hir.moduleDecls(res->hir.root());
        HirNodeId const call = findFirstByKind(
            res->hir, res->hir.functionBody(ds.back()), HirKind::Call);
        EXPECT_TRUE(call.valid()) << "main ends in a Call `va(1, x)`";
        auto const kids = res->hir.children(call);
        EXPECT_GE(kids.size(), 3u) << "[callee, `1`, `x`]";
        HirNodeId const arg = kids.back();
        return {res->hir.kind(arg), ti.kind(res->hir.typeId(arg))};
    };

    // signed short -1 → integer-promoted to int: a Cast typed I32.
    {
        auto [k, t] = tailArg("short x; x = -1;");
        EXPECT_EQ(k, HirKind::Cast) << "a `short` variadic arg must be promotion-cast";
        EXPECT_EQ(t, TypeKind::I32) << "…to int (I32)";
    }
    // signed char -1 → promoted to int.
    {
        auto [k, t] = tailArg("signed char x; x = -1;");
        EXPECT_EQ(k, HirKind::Cast) << "a `signed char` variadic arg must be promotion-cast";
        EXPECT_EQ(t, TypeKind::I32) << "…to int (I32)";
    }
    // unsigned short → promoted to int (mapCast picks ZExt for the unsigned source).
    {
        auto [k, t] = tailArg("unsigned short x; x = 5;");
        EXPECT_EQ(k, HirKind::Cast) << "an `unsigned short` variadic arg must be promotion-cast";
        EXPECT_EQ(t, TypeKind::I32) << "…to int (I32)";
    }
    // float → widened to double (FPExt): a Cast typed F64.
    {
        auto [k, t] = tailArg("float x; x = 1.5f;");
        EXPECT_EQ(k, HirKind::Cast) << "a `float` variadic arg must widen";
        EXPECT_EQ(t, TypeKind::F64) << "…to double (F64)";
    }
    // int is already at the promotion floor → NO cast (passes through unchanged).
    {
        auto [k, t] = tailArg("int x; x = 5;");
        EXPECT_NE(k, HirKind::Cast) << "an `int` variadic arg must NOT be re-cast";
        EXPECT_EQ(t, TypeKind::I32);
    }
    // double is already double → NO widen.
    {
        auto [k, t] = tailArg("double x; x = 1.5;");
        EXPECT_NE(k, HirKind::Cast) << "a `double` variadic arg must NOT be re-cast";
        EXPECT_EQ(t, TypeKind::F64);
    }
}

// ── c115 SEH (D-WIN64-SEH-FUNCLETS): the __try/__except frontend ──────────────

// The guarded body, filter expression, and handler body lower to a core
// SehTryExcept node {tryBody Block, filterExpr, handlerBody Block}. This is the
// structural shape the c116 x64 funclet lowering consumes.
TEST(HirLoweringC, SehTryExceptLowersToCoreNode) {
    SemanticModel model = analyzeC(
        "int f(int *p) { int rc = 0; __try { rc = *p; } "
        "__except (1) { rc = 42; } return rc; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    HirNodeId const seh =
        findFirstByKind(res->hir, res->hir.functionBody(fn), HirKind::SehTryExcept);
    ASSERT_TRUE(seh.valid()) << "a SehTryExcept node must be emitted";
    auto const kids = res->hir.children(seh);
    ASSERT_EQ(kids.size(), 3u) << "[tryBody, filterExpr, handlerBody]";
    EXPECT_EQ(res->hir.kind(kids[0]), HirKind::Block)   << "guarded body is a Block";
    EXPECT_EQ(res->hir.kind(kids[2]), HirKind::Block)   << "handler body is a Block";
    // The filter (child 1) is an expression, not a statement Block.
    EXPECT_NE(res->hir.kind(kids[1]), HirKind::Block)   << "filter is an expression";
}

// `_exception_code()` in the filter expression is LEGAL (the canonical use).
TEST(HirLoweringC, SehExceptionCodeInFilterIsLegal) {
    SemanticModel model = analyzeC(
        "int f(int *p) { int rc = 0; __try { rc = *p; } "
        "__except (_exception_code() == 0) { rc = 42; } return rc; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_SehBuiltinContext), 0u);
    // The builtin lowered to a BuiltinCall inside the filter subtree.
    HirNodeId const fn = firstFunction(res->hir);
    HirNodeId const seh =
        findFirstByKind(res->hir, res->hir.functionBody(fn), HirKind::SehTryExcept);
    ASSERT_TRUE(seh.valid());
    EXPECT_TRUE(findFirstByKind(res->hir, res->hir.children(seh)[1],
                                HirKind::BuiltinCall).valid())
        << "_exception_code lowers to a BuiltinCall in the filter";
}

// RED: `_exception_code()` with NO enclosing __try → H_SehBuiltinContext.
TEST(HirLoweringC, SehExceptionCodeOutsideTryIsRejected) {
    SemanticModel model = analyzeC(
        "int f(void) { return (int)_exception_code(); }\n");
    ASSERT_FALSE(model.hasErrors());   // resolves as a builtin call; HIR verifier rejects
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_SehBuiltinContext), 1u);
}

// RED: `_exception_info()` in the HANDLER body (filter-only) → H_SehBuiltinContext.
// (_exception_code IS legal in the handler; _exception_info is filter-only —
// the asymmetry is the point.)
TEST(HirLoweringC, SehExceptionInfoInHandlerIsRejected) {
    SemanticModel model = analyzeC(
        "int f(int *p) { int rc = 0; __try { rc = *p; } "
        "__except (1) { rc = (int)(long long)_exception_info(); } return rc; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_SehBuiltinContext), 1u);
}

// RED (option (C), D-CSUBSET-SEH-EARLY-EXIT): `return` inside the guarded body.
TEST(HirLoweringC, SehReturnInTryBodyIsRejected) {
    SemanticModel model = analyzeC(
        "int f(int *p) { __try { return *p; } __except (1) { return 42; } }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_SehEarlyExit), 1u);
}

// FAIL-LOUD (D-CSUBSET-SEH-FINALLY): `__finally` parses but has no lowering.
TEST(HirLoweringC, SehFinallyFailsLoud) {
    SemanticModel model = analyzeC(
        "int f(int *p) { int rc = 0; __try { rc = *p; } "
        "__finally { rc = 1; } return rc; }\n");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 1u);
}

// FAIL-LOUD (D-CSUBSET-SEH-LEAVE): `__leave` parses but has no lowering.
TEST(HirLoweringC, SehLeaveFailsLoud) {
    SemanticModel model = analyzeC(
        "int f(int *p) { int rc = 0; __try { __leave; rc = *p; } "
        "__except (1) { rc = 42; } return rc; }\n");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 1u);
}

// ── C11/C23 6.7.10 static_assert (D-CSUBSET-STATIC-ASSERT) ──────────────────
//
// The condition is const-evaluated at the SEMANTIC tier (the point that folds
// sizeof / enum / arithmetic); a zero / non-constant fold fails loud with
// S_StaticAssertFailed. A passing assertion produces NO HIR (lowers to nothing —
// its hirLowering row maps to Skip) and the module is left with just its real
// declarations.

// Count the top-level Function decls in a lowered module — the witness that a
// passing static_assert added nothing at module scope.
[[nodiscard]] std::size_t moduleFunctionCount(Hir const& hir) {
    std::size_t n = 0;
    for (HirNodeId d : hir.moduleDecls(hir.root()))
        if (hir.kind(d) == HirKind::Function) ++n;
    return n;
}

// NOTE on sizeof: an array-dim / static_assert `sizeof` folds ONLY when
// `analyze()` is given the target's aggregateLayout. The direct-API
// `analyzeC` helper here passes nullopt (the documented direct-API
// default), so the sizeof-in-condition FOLDING pins live in
// test_semantic_analyzer_c.cpp (which passes AggregateLayoutParams) and
// end-to-end in examples/c/static_assert_true. The pins BELOW exercise
// the parse / peel / 1-arg / spelling / block-scope / enum / non-const paths,
// which need no layout.

// POSITIVE — the canonical passing idiom: an arithmetic condition FOLDS true,
// the assertion passes, and NOTHING is emitted for it.
TEST(HirLoweringC, StaticAssertArithmeticTrueCompilesToNothing) {
    SemanticModel model = analyzeC(
        "_Static_assert(2 + 2 == 4, \"math works\");\n"
        "int main(void){ return 42; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u)
        << "2+2==4 must FOLD true in the static_assert condition";
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    // The assertion contributed no module node — only `main` survives.
    EXPECT_EQ(moduleFunctionCount(res->hir), 1u);
}

// NEGATIVE — a false arithmetic condition fails loud.
TEST(HirLoweringC, StaticAssertArithmeticFalseFailsLoud) {
    SemanticModel model = analyzeC(
        "_Static_assert(1 == 2, \"one is not two\");\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u);
}

// C23 1-ARG PASSING — `_Static_assert(1);` (no message) compiles to nothing. The
// critical peelToCore case: the 1-arg node has a SINGLE meaningful child, so a
// naive peel would descend past it → H0009; the Skip-mapped rule stops the peel.
TEST(HirLoweringC, StaticAssert1ArgTrueCompilesToNothing) {
    SemanticModel model = analyzeC(
        "_Static_assert(1);\n"
        "int main(void){ return 42; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u);
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    // No H0009 (Ref-to-unbound / unsupported lowering) — the 1-arg form must NOT
    // fall through the wrapper peel into its condition child.
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnsupportedLoweringForKind), 0u);
    EXPECT_EQ(moduleFunctionCount(res->hir), 1u);
}

// C23 1-ARG FAILING — `_Static_assert(0);` fails loud with S_StaticAssertFailed
// (NOT H0009). Pins that the 1-arg form is reached by the semantic check.
TEST(HirLoweringC, StaticAssert1ArgFalseFailsLoud) {
    SemanticModel model = analyzeC(
        "_Static_assert(0);\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::P_NoAlternativeMatched), 0u)
        << "the 1-arg form must PARSE (no parse fallthrough)";
}

// C23 `static_assert` SPELLING — behaves identically to `_Static_assert`.
TEST(HirLoweringC, StaticAssertC23SpellingTrue) {
    SemanticModel model = analyzeC(
        "static_assert(1 + 1 == 2, \"addition works\");\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u);
    EXPECT_FALSE(model.hasErrors());
}

TEST(HirLoweringC, StaticAssertC23SpellingFalseFailsLoud) {
    SemanticModel model = analyzeC(
        "static_assert(0, \"nope\");\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u);
}

// BLOCK SCOPE — a static_assert is a valid statement-level declaration. Both a
// passing and a failing one are checked at the same tier.
TEST(HirLoweringC, StaticAssertBlockScopeTrueCompilesToNothing) {
    SemanticModel model = analyzeC(
        "int main(void){ _Static_assert(3 > 1, \"three beats one\"); return 42; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u);
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
}

TEST(HirLoweringC, StaticAssertBlockScopeFalseFailsLoud) {
    SemanticModel model = analyzeC(
        "int main(void){ _Static_assert(1 == 0, \"impossible\"); return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u);
}

// BLOCK SCOPE 1-ARG — the message-less form at statement scope (both the parse
// and the peel must handle the single-child node here too).
TEST(HirLoweringC, StaticAssertBlockScope1ArgFalseFailsLoud) {
    SemanticModel model = analyzeC(
        "int main(void){ static_assert(1 > 2); return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u);
}

// ENUM CONSTANT in the condition folds (same evaluator that folds enum constants
// in an array dimension).
TEST(HirLoweringC, StaticAssertEnumConstantFolds) {
    SemanticModel model = analyzeC(
        "enum { KVAL = 7 };\n"
        "_Static_assert(KVAL == 7, \"kval is 7\");\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u);
    EXPECT_FALSE(model.hasErrors());
}

// A FLOAT condition is decided by its TRUTH VALUE, not refused for being a float
// — [[D-C-STATIC-ASSERT-REFUSES-A-LONG-DOUBLE-COMPARISON]].
//
// ⚠⚠ THIS TEST USED TO ASSERT THE OPPOSITE, and it was wrong. It required
// `_Static_assert(3.5, "")` to FAIL "because C 6.7.10 requires an integer
// constant expression". ✔MEASURED at close time, probed SEPARATELY: clang 18.1.3
// and MSVC 19.51 ACCEPT it; gcc 13.3.0 and mingw-w64 gcc 13.2.0 refuse it as
// "not an integer". `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` governs an
// accept-vs-refuse split, so DSS accepts. The REFUSAL the test was really
// guarding — that a float condition cannot silently pass whatever its value —
// is kept below and is stronger: a float that is FALSE still fails loud, which
// all four references agree on.
TEST(HirLoweringC, StaticAssertFloatConditionIsDecidedByItsTruthValue) {
    SemanticModel accepted = analyzeC(
        "_Static_assert(3.5, \"a non-zero float is true\");\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(accepted.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u);
    SemanticModel refused = analyzeC(
        "_Static_assert(0.0, \"a zero float is false\");\n"
        "int main(void){ return 0; }\n");
    EXPECT_EQ(countCode(refused.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 1u)
        << "a FALSE floating condition must still fail loud";
}

// ════════════════════════════════════════════════════════════════════════════
// TF-C77 — D-CSUBSET-ATTRIBUTE-LEADING-WITH-STORAGE-CLASS, modes 1 and 2.
//
// TWO new attribute POSITIONS opened this cycle, each previously a hard P0009:
//   mode 1  AFTER the `extern` keyword   `extern __attribute__((weak)) int wk;`
//   mode 2  BETWEEN the type head and the declarator list — the `declAttrRun`
//           slot — `static int SQLITE_NOINLINE f(…)` / `int __attribute__((weak)) gv;`
//
// EVERY pin below asserts an APPLIED FACT (a binding, a visibility, an
// alignment, a resolved sibling-CU value) or an exact diagnostic SET. That is
// forced, not stylistic: the failure mode of a dropped attribute is SILENCE, so
// a "compiles clean" or bare-count pin reads green through the exact regression
// it exists to catch.
// ════════════════════════════════════════════════════════════════════════════

// ── MODE 1 ──────────────────────────────────────────────────────────────────

// MODE 1, the `weak` binding. `externSpecifiers` gained `attrSpec`, and because
// that wrapper IS the row's `specifierPrefix` the attribute reaches `linkageFrom`
// with ZERO new wiring. This pin is what proves the "zero wiring" claim is real
// rather than assumed.
//
// ★ ASSERTS THE APPLIED BINDING. A count pin would pass if the attribute were
// parsed and silently dropped — which is precisely what the position did before
// the grammar landed, one tier earlier, as P0009.
//
// RED-ON-DISABLE (MEASURED): remove `attrSpec` from `externSpecifiers`'s repeat
// alt → P0009 'expected Identifier, VoidKeyword, … — got __attribute__' and the
// model never reaches lowering. Drop the `weak` key from the externDecl row's
// `linkageSpecifiers` → H_UnknownLinkageSpecifier. Move `weak` into that row's
// ignored NAMES instead → zero diagnostics and `wkBinding` silently reads
// Global, which is the wrong-way "fix" this assertion exists to catch.
TEST(HirLoweringC, ExternHeadAttributeWeakApplies) {
    SemanticModel model = analyzeC(
        "extern __attribute__((weak)) int wk;\n"
        "int main(void){ return wk; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u);
    EXPECT_EQ(declaredBinding(*res, model, "wk"), SymbolBinding::Weak)
        << "an attribute AFTER the `extern` keyword must bind exactly as the "
           "same attribute after the declarator does";
}

// MODE 1, the COMPOSITE `visibility("hidden")` key. Separate from the `weak` pin
// because it exercises a different machine: the `<identifier>:<string-body>`
// pairing, which requires the attribute's string argument to survive the scan as
// a token rather than being skipped wholesale.
TEST(HirLoweringC, ExternHeadAttributeVisibilityHiddenApplies) {
    SemanticModel model = analyzeC(
        "extern __attribute__((visibility(\"hidden\"))) int ev;\n"
        "int ev = 7;\n"
        "int main(void){ return ev; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "the composite `visibility:hidden` key must resolve from the "
           "after-keyword position — a bare `visibility` would fail loud";
}

// MODE 1 × THREAD-STORAGE, BOTH ORDERS. `externSpecifiers`'s slot is a REPEAT
// over an alt, so `extern _Thread_local __attribute__((…)) int` and
// `extern __attribute__((…)) _Thread_local int` are both legal orderings and
// must agree. An order-sensitive fold is a real hazard here — the linkage merge
// is last-wins — so the two orders are pinned against EACH OTHER, not merely
// each against "compiles".
TEST(HirLoweringC, ExternHeadAttributeAndThreadLocalAgreeInBothOrders) {
    for (char const* decl : {
             "extern _Thread_local __attribute__((weak)) int tv;\n",
             "extern __attribute__((weak)) _Thread_local int tv;\n"}) {
        std::string const full = std::string(decl)
                               + "int main(void){ return tv; }\n";
        SemanticModel model = analyzeC(full);
        ASSERT_FALSE(model.hasErrors()) << decl;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok) << decl << ": "
                             << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
            << decl;
        EXPECT_EQ(declaredBinding(*res, model, "tv"), SymbolBinding::Weak)
            << decl << " — the attribute must bind identically whichever side of "
                       "the thread-storage keyword it is written on";
    }
}

// MODE 1, MULTI-CLAUSE. `__attribute__((__nothrow__, __leaf__))` is glibc's
// `__THROW`; the clause comma is a DIRECT child of `attrSpec` and so is NOT
// covered by the attribute-ARGUMENT token flag. This pin asserts the exact
// diagnostic SET is EMPTY for the whole declaration, which is the only way to
// distinguish "resolved" from "one of the six tokens quietly matched something".
TEST(HirLoweringC, ExternHeadMultiClauseAttributeResolvesClean) {
    SemanticModel model = analyzeC(
        "extern __attribute__((__nothrow__, __leaf__)) int gg(void);\n"
        "int gg(void){ return 10; }\n"
        "int main(void){ return gg(); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u);
    EXPECT_EQ(declaredBinding(*res, model, "gg"), SymbolBinding::Global)
        << "an ABI-neutral hint run must leave the binding untouched — a fold "
           "that clobbered it to a default would read as a silent relinking";
}

// MODE 1, THE UNKNOWN-NAME GATE. The diagnostic SET must be exactly
// {H_UnknownLinkageSpecifier} — not "at least one error", which would also be
// satisfied by a parse failure and would therefore keep passing if the grammar
// were reverted.
TEST(HirLoweringC, ExternHeadUnknownAttributeIsExactlyTheLinkageDiagnostic) {
    SemanticModel model = analyzeC(
        "extern __attribute__((frobnicate_xyz)) int uz;\n"
        "int uz = 1;\n"
        "int main(void){ return uz; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "the shape must PARSE — the rejection belongs to the linkage tier, "
           "so a parse error here would mean this pin is guarding the wrong thing";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
        << "an unrecognized GNU name in the newly-honored after-keyword position "
           "must fail LOUD — silence here is the whole bug class this cycle "
           "exists to close";
    std::size_t others = 0;
    for (auto const& d : r.all())
        if (d.code != DiagnosticCode::H_UnknownLinkageSpecifier
            && d.severity == DiagnosticSeverity::Error) ++others;
    EXPECT_EQ(others, 0u) << "the SET must be exactly {H_UnknownLinkageSpecifier}";
}

// ── MODE 2 ──────────────────────────────────────────────────────────────────

// ★★ MODE 2, THE LOAD-BEARING PIN — AND THE H2 HALFWAY-STATE DEMONSTRATION.
//
// The grammar alone makes `int __attribute__((weak)) gv = 3;` PARSE. Only the
// `linkagePrefixRoots` extension — pushing every `declarationAttrSlotRules`
// child as a scan root — makes the binding APPLY. With the grammar and the
// config key but WITHOUT that engine change, this program compiles perfectly
// cleanly, emits ZERO diagnostics, and links `gv` as a plain strong global: a
// silent wrong-linkage miscompile.
//
// RED-ON-DISABLE (H2, MEASURED): revert `linkagePrefixRoots` to returning only
// `specifierPrefixChild` → this EXPECT flips Weak → Global with ZERO
// diagnostics of any code anywhere in the program. That is the halfway state,
// and it is why the grammar and the engine change are ONE unit.
TEST(HirLoweringC, MidPositionAttributeWeakApplies) {
    SemanticModel model = analyzeC(
        "int __attribute__((weak)) gv = 3;\n"
        "int main(void){ return gv; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u);
    EXPECT_EQ(declaredBinding(*res, model, "gv"), SymbolBinding::Weak)
        << "the mid-position attribute must BIND, not merely parse — without the "
           "linkagePrefixRoots slot roots this reads Global, silently";
}

// MODE 2 IS A DECLARATION-LEVEL SLOT, NOT A PER-DECLARATOR ONE — the exact
// OPPOSITE of the after-declarator run, and the distinction is C semantics, not
// a convention. `int __attribute__((weak)) a = 1, b = 2;` writes the attribute
// in the DECL-SPECIFIER region, so it applies to EVERY declarator.
//
// GROUND TRUTH IS REAL CLANG, not DSS agreeing with itself: MEASURED with
// `clang -c` + `nm -m`, that exact line emits BOTH `_ma` and `_mb` as
// "weak external". Contrast `int a __attribute__((weak)) = 1, b = 2;` — the
// trailing form — where only `a` is weak (pinned by
// `AfterDeclaratorAttributeIsPerDeclaratorNotPerDeclaration` above). Two
// positions, two different scopes of application, both correct; this pin exists
// so a future refactor cannot collapse them into one rule.
TEST(HirLoweringC, MidPositionAttributeAppliesToEveryDeclarator) {
    SemanticModel model = analyzeC(
        "int __attribute__((weak)) ma = 1, mb = 2;\n"
        "int main(void){ return ma + mb; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(declaredBinding(*res, model, "ma"), SymbolBinding::Weak);
    EXPECT_EQ(declaredBinding(*res, model, "mb"), SymbolBinding::Weak)
        << "a DECL-SPECIFIER-position attribute applies to the whole "
           "declaration — clang emits both as `weak external` (MEASURED)";
}

// MODE 2 ON A FUNCTION DEFINITION — the literal sqlite shape
// (`static T SQLITE_NOINLINE f(…) { … }`) and the one that moves the
// `kindByChild` index. `declaratorList` 1 → 2 and `childPath` [2,0] → [3,0]
// had to move with the grammar; this pin is what fails if either is left behind.
//
// RED-ON-DISABLE (H3, MEASURED): leave `kindByChild.childPath` at [2,0] → the
// tail is never recognized as a `block`, the definition is not lowered as a
// Function, and this ASSERT on the Function's presence fails.
TEST(HirLoweringC, MidPositionAttributeOnFunctionDefinitionLowersAsFunction) {
    SemanticModel model = analyzeC(
        "static int __attribute__((cold)) sf(int x){ return x + 1; }\n"
        "int main(void){ return sf(41); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    std::size_t fns = 0;
    bool sawSf = false;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) != HirKind::Function) continue;
        ++fns;
        auto const* rec = model.recordFor(res->hir.functionSymbol(d));
        if (rec != nullptr && rec->name == "sf") sawSf = true;
    }
    EXPECT_TRUE(sawSf)
        << "the decorated DEFINITION must lower as a Function with a body — if "
           "kindByChild still points at [2,0] it is mis-lowered as a variable";
    EXPECT_EQ(fns, 2u) << "exactly `sf` and `main`";
    EXPECT_EQ(declaredBinding(*res, model, "sf"), SymbolBinding::Local)
        << "the co-present `static` must still bind internal — an attribute in "
           "the new slot must not clobber the specifier prefix's binding";
}

// MODE 2 ON A PROTOTYPE (no body) — the same position, the other tail arm. Kept
// separate from the definition pin because the two take different lowering
// paths and an asymmetric break would otherwise hide.
TEST(HirLoweringC, MidPositionAttributeOnPrototypeApplies) {
    SemanticModel model = analyzeC(
        "int __attribute__((weak)) wp(void);\n"
        "int wp(void){ return 42; }\n"
        "int main(void){ return wp(); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u);
}

// ★ MODE 2, THE ALIGNMENT SINK — AND THE H1 HALFWAY-STATE DEMONSTRATION.
//
// `declarationAttrSlotRules` is what routes the slot to `scanAttributeSemantics`.
// With the grammar but WITHOUT that key the declaration parses perfectly and the
// alignment is SILENTLY GONE — no diagnostic, no wrong number to notice, just an
// under-aligned object.
//
// 32 is a genuine OVER-alignment for `int` (natural 4), so the value is
// load-bearing: a sink that fell back to natural alignment reads 4 or 0.
//
// RED-ON-DISABLE (H1, MEASURED): drop `declarationAttrSlotRules` from the
// topLevelDecl row → the program still compiles clean with ZERO diagnostics and
// this EXPECT reads 0 instead of 32.
TEST(HirLoweringC, MidPositionAlignedApplies) {
    SemanticModel model = analyzeC(
        "int __attribute__((aligned(32))) av = 20;\n"
        "int main(void){ return av; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u);
    EXPECT_EQ(globalAlignment(*res, model, "av"), 32u)
        << "the mid-position `aligned(32)` must APPLY — a dropped alignment "
           "emits nothing at all, so only the applied value can catch it";
}

// MODE 2 × the LEADING position: the SAME attribute must mean the SAME thing in
// both, on the same program. An asymmetry here is the bug class TF-C73 named
// (an attribute that means two different things depending on where it sits), so
// the two arms are pinned against each other rather than each against a constant.
TEST(HirLoweringC, MidPositionAlignedAgreesWithTheLeadingPosition) {
    for (char const* decl : {
             "__attribute__((aligned(32))) int av = 20;\n",     // leading
             "int __attribute__((aligned(32))) av = 20;\n"}) {  // mid (TF-C77)
        std::string const full = std::string(decl)
                               + "int main(void){ return av; }\n";
        SemanticModel model = analyzeC(full);
        ASSERT_FALSE(model.hasErrors()) << decl;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok) << decl;
        EXPECT_EQ(globalAlignment(*res, model, "av"), 32u) << decl;
    }
}

// MODE 2, THE STATIC/WEAK CONFLICT. `static int __attribute__((weak)) x;` asks
// for internal AND weak binding at once. It must be REFUSED, and the fact that
// it can be refused at all is itself evidence: the prefix root and the slot root
// are folded in the SAME `linkageFrom` pass, so the conflict is visible. Two
// independent folds could not see it and would silently last-wins one of them.
TEST(HirLoweringC, MidPositionWeakConflictsWithStaticLoudly) {
    SemanticModel model = analyzeC(
        "static int __attribute__((weak)) sx = 1;\n"
        "int main(void){ return sx; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
        << "the conflicting binding must be reported, never silently resolved "
           "by whichever root happened to be folded last";
}

// MODE 2, THE UNKNOWN-NAME GATE at file scope. Exact diagnostic SET.
TEST(HirLoweringC, MidPositionUnknownAttributeIsExactlyTheLinkageDiagnostic) {
    SemanticModel model = analyzeC(
        "int __attribute__((frobnicate_xyz)) uz = 1;\n"
        "int main(void){ return uz; }\n");
    ASSERT_FALSE(model.hasErrors())
        << "the shape must PARSE — the rejection belongs to the linkage tier";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
        << "without the linkagePrefixRoots slot roots this reads 0 — the "
           "unknown name is swallowed along with the weak binding";
    std::size_t others = 0;
    for (auto const& d : r.all())
        if (d.code != DiagnosticCode::H_UnknownLinkageSpecifier
            && d.severity == DiagnosticSeverity::Error) ++others;
    EXPECT_EQ(others, 0u) << "the SET must be exactly {H_UnknownLinkageSpecifier}";
}

// ★★ MODE 2, THE TYPE-HIJACK ANTI-PIN — the reason `declAttrRun` is a SIBLING of
// the head and never a child of it.
//
// `resolveTypeNodeImpl` is first-child-that-resolves-wins and its token arm
// resolves ANY identifier through the scope chain. Had the run been nested
// inside `topLevelHead`, the attribute identifier `weak` — which this program
// deliberately also makes a real typedef name — would be tried as a TYPE and
// `gv` would silently resolve to it. Since `typedef int weak;` and the real type
// are both `int` here, the hijack would be invisible to a type check alone —
// so this pin asserts BOTH halves: `gv` is still `int` (4 bytes) AND still weak.
// A hijack that also dropped the binding, or a binding that survived a hijacked
// type, each fails exactly one half.
TEST(HirLoweringC, MidPositionAttributeDoesNotHijackTheHeadType) {
    SemanticModel model = analyzeC(
        "typedef int weak;\n"
        "int __attribute__((weak)) gv = 3;\n"
        "int main(void){ return gv + (int)sizeof(gv); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(declaredBinding(*res, model, "gv"), SymbolBinding::Weak)
        << "a typedef named `weak` must not stop the attribute from binding";
    bool sawGv = false;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) != HirKind::Global) continue;
        auto const* rec = model.recordFor(res->hir.globalSymbol(d));
        if (rec == nullptr || rec->name != "gv") continue;
        sawGv = true;
    }
    EXPECT_TRUE(sawGv) << "`gv` must lower as a Global, not be swallowed";
}

// ★ MODE 2, THE INDEX NON-REGRESSION — the DECLARATOR-LESS form.
//
// `struct P { int x; };` has NO `initDeclaratorList`, so its post-strip role
// children are [topLevelHead, declAttrRun, topLevelDeclTail] — one SHORTER than
// the decorated case. This is exactly where an index that moved by hand rather
// than by construction goes wrong, so the TU carries a no-declarator type
// declaration AND a function definition AND a plain global together: all three
// index shapes in one lowering.
//
// RED-ON-DISABLE (H3, MEASURED): leave `declaratorList` at 1 → the row addresses
// the (empty) `declAttrRun` as its declarator list and the global is lost.
TEST(HirLoweringC, DeclaratorLessDeclarationKeepsTheRowIndicesStable) {
    SemanticModel model = analyzeC(
        "struct P { int x; };\n"
        "int plain = 1;\n"
        "int helper(int a){ return a + 1; }\n"
        "int main(void){ struct P p; p.x = 40; return helper(p.x) + plain; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    std::size_t fns = 0, globals = 0;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) == HirKind::Function) ++fns;
        if (res->hir.kind(d) == HirKind::Global)   ++globals;
    }
    EXPECT_EQ(fns, 2u)
        << "`helper` and `main` must both lower as Functions — a stale "
           "kindByChild path mis-reads a definition as a variable";
    EXPECT_EQ(globals, 1u)
        << "`plain` must lower as exactly one Global — a stale declaratorList "
           "index reads the empty declAttrRun and loses it";
}

// ── TRAILING-POSITION NON-REGRESSION ────────────────────────────────────────
//
// The glibc idiom in the AFTER-DECLARATOR position, unchanged by this cycle.
// It shares `externDecl` with mode 1's new head slot, and the two runs are now
// folded into ONE `linkageFrom` pass, so a mistake in the new roots would show
// up here as a spurious diagnostic on code that has worked for two cycles.
TEST(HirLoweringC, TrailingAttributeRunStillResolvesAfterTheHeadSlotOpened) {
    SemanticModel model = analyzeC(
        "extern int gg(void) __attribute__((__nothrow__));\n"
        "int gg(void){ return 42; }\n"
        "int main(void){ return gg(); }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// TF-C88 (D-CSUBSET-TYPEDEF-MULTI-DECLARATOR) — the HIR-SHAPE pin.
//
// ★ THIS TEST EXISTS BECAUSE THE SEMANTIC TESTS CANNOT SEE THE DEFECT IT GUARDS.
// Pass-1 mints one SYMBOL per declarator independently of the HIR lowering, so
// every "all three aliases bind / each keeps its own suffix" assertion in
// test_semantic_analyzer_c.cpp stays GREEN if `lowerTypeDeclInto` emits
// only the first node — the exact vacuity the removed `break; // typedefs
// declare a single declarator` would reintroduce. A plain alias emits no code,
// so an exit-code corpus example cannot see it either. Counting the NODES is the
// only place the drop is observable.
//
// It matters because a VLA typedef's TypeDecl is where HIR→MIR FREEZES that
// alias's runtime size slots (C99 6.7.7p2 evaluates the bound once, when the
// typedef is REACHED) — a dropped node there is a real wrong-size miscompile.
//
// RED-ON-DISABLE: re-add the `break` after the first `out.push_back` in
// `lowerTypeDeclInto` and this drops 5 → 3 while every other test stays green.
TEST(HirLoweringC, TypedefMultiDeclaratorEmitsOneTypeDeclPerAlias) {
    SemanticModel model = analyzeC(
        "typedef unsigned int mach_port_t;\n"
        "typedef mach_port_t vm_map_t, vm_map_read_t, vm_map_inspect_t;\n"
        "vm_map_t origin;\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    auto decls = res->hir.moduleDecls(res->hir.root());
    std::size_t typeDecls = 0;
    for (HirNodeId d : decls)
        if (res->hir.kind(d) == HirKind::TypeDecl) ++typeDecls;
    EXPECT_EQ(typeDecls, 4u)
        << "one TypeDecl for `mach_port_t` plus ONE PER ALIAS of the "
           "three-declarator typedef — a lowering that stops after the first "
           "declarator reports 2";
    EXPECT_EQ(decls.size(), 5u) << "4 TypeDecls + the Global `origin`";

    // Each alias's TypeDecl must carry its OWN symbol — four DISTINCT ids, not
    // the same one repeated (which is what a loop that re-pushes the first
    // declarator's symbol would produce, and which counts identically).
    std::vector<std::uint32_t> syms;
    for (HirNodeId d : decls)
        if (res->hir.kind(d) == HirKind::TypeDecl)
            syms.push_back(res->hir.typeDeclSymbol(d).v);
    std::ranges::sort(syms);
    EXPECT_EQ(std::ranges::unique(syms).begin() - syms.begin(),
              static_cast<std::ptrdiff_t>(4))
        << "the four TypeDecls must name four DIFFERENT symbols";
}

// ─────────────────────────────────────────────────────────────────────────────
// D-CSUBSET-PARAM-FN-TYPE-ADJUSTMENT (C 6.7.6.3p8) — the HIR PARAM-SLOT pins.
//
// ★ THESE TESTS EXIST BECAUSE THE SEMANTIC TESTS CANNOT SEE THE DEFECT THEY
// GUARD. p8 is implemented as a TYPE adjustment, and every semantic pin reads
// the adjusted TYPE — which was already correct (Ptr<FnSig>) while the bug was
// live. What was wrong was the symbol's PROTOTYPE flag: the INLINE spelling
// `int g(int)` writes a name carrying a `()` suffix, the same syntax a function
// prototype writes, so Pass 1's purely syntactic `isProto` test called the
// parameter a prototype. CST→HIR's D-CSUBSET-FN-PROTOTYPE gate emits NO VarDecl
// for a prototype, so the parameter SLOT silently vanished while its FnSig kept
// the entry — MEASURED as `H_UnsupportedLoweringForKind: Function param count 1
// mismatches FnSig param count 2` at HIR→MIR, on a program whose semantic tier
// was entirely clean. Counting the emitted param NODES is the only place the
// drop is observable, which is exactly why it lives here and not one tier up.
//
// The TYPEDEF spelling (`void f(Fn g)`) never had the defect — its declarator
// carries no `()` suffix at all — so a fixture must use the INLINE form to see it.
//
// RED-ON-DISABLE (both tests): drop `&& !decl.paramAdjustments` from the
// `isProto` derivation in semantic_analyzer.cpp's Pass-1 declarator mint. The
// first test then reports 1 param instead of 2; the second fails its
// `hasErrors()` assert with S0022 (the two parameters re-home onto the FILE
// scope and collide as incompatible redeclarations).
namespace {
// The pointee FnSig of a param slot that C 6.7.6.3p8 adjusted, or InvalidType.
// Reading THROUGH this makes a bare-FnSig (un-adjusted) slot impossible to pass
// by accident, and keeps each structural pin one line.
[[nodiscard]] TypeId adjustedParamPointee(SemanticModel const& m, TypeId t) {
    TypeInterner const& in = m.lattice().interner();
    if (!t.valid() || in.kind(t) != TypeKind::Ptr) return InvalidType;
    auto const ops = in.operands(t);
    if (ops.empty() || in.kind(ops[0]) != TypeKind::FnSig) return InvalidType;
    return ops[0];
}
}  // namespace

// PIN 1 — the param slot EXISTS, in the right POSITION, with the adjusted TYPE.
// Every one of the three facts is load-bearing: HIR→MIR reads the Function's
// param NODES to emit `Arg` instructions and its FnSig for their types, so a
// count mismatch is a hard error and an order/type mismatch would be a silent
// wrong-register miscompile.
TEST(HirLoweringC, InlineFunctionTypedParamEmitsItsOwnParamSlot) {
    SemanticModel model = analyzeC(
        "int twice(int x) { return x + x; }\n"
        "int apply(int g(int), int v) { return g(v); }\n"
        "int main(void) { return apply(twice, 21); }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    HirNodeId apply{};
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) != HirKind::Function) continue;
        auto const* rec = model.recordFor(res->hir.functionSymbol(d));
        if (rec != nullptr && rec->name == "apply") { apply = d; break; }
    }
    ASSERT_TRUE(apply.valid());

    auto const params = res->hir.functionParams(apply);
    ASSERT_EQ(params.size(), 2u)
        << "the INLINE function-typed parameter must emit its OWN slot — a "
           "prototype-classified parameter emits none and this reports 1";

    TypeInterner const& in = model.lattice().interner();
    // The Function's own FnSig is the OTHER half of the equality HIR→MIR
    // checks; pinning both here is what makes the two provably agree.
    auto const sigParams = in.fnParams(res->hir.functionSignature(apply));
    ASSERT_EQ(sigParams.size(), 2u);

    // Slot 0 — `int g(int)` adjusted to `int (*)(int)`.
    ASSERT_EQ(res->hir.kind(params[0]), HirKind::VarDecl);
    TypeId const slot0 = res->hir.varDeclType(params[0]);
    TypeId const pointee = adjustedParamPointee(model, slot0);
    ASSERT_TRUE(pointee.valid())
        << "slot 0 must be Ptr<FnSig> — a bare FnSig is a function VALUE in a "
           "parameter slot, the silent miscompile p8 exists to prevent";
    auto const inner = in.fnParams(pointee);
    ASSERT_EQ(inner.size(), 1u);
    EXPECT_EQ(in.kind(inner[0]), TypeKind::I32);
    EXPECT_EQ(in.kind(in.fnResult(pointee)), TypeKind::I32);
    EXPECT_EQ(slot0, sigParams[0])
        << "the emitted slot's type and the FnSig's entry must be the SAME "
           "interned type — a drift here is a wrong-width argument";

    // Slot 1 — the ordinary `int v` that followed it. Pinned because a dropped
    // slot 0 would silently SHIFT this parameter into argument register 0.
    ASSERT_EQ(res->hir.kind(params[1]), HirKind::VarDecl);
    EXPECT_EQ(in.kind(res->hir.varDeclType(params[1])), TypeKind::I32);
    EXPECT_EQ(res->hir.varDeclType(params[1]), sigParams[1]);
}

// PIN 2 — C 6.2.1p4: a parameter name is scoped to its own declarator and has
// NO linkage, so two functions may reuse one freely. Classifying the inline
// spelling as a prototype ALSO re-homed the parameter symbol onto the FILE
// scope (that is what the prototype path does, per D-CSUBSET-BLOCK-SCOPE-PROTOTYPE),
// where two same-named parameters met as one symbol. MEASURED
// before the fix: S0022 S_IncompatibleRedeclaration on `g`, pointing at the
// OTHER function's parameter — legal C rejected outright.
TEST(HirLoweringC, SameNamedInlineFunctionTypedParamsStayFunctionLocal) {
    SemanticModel model = analyzeC(
        "int  twice(int x)   { return x * 2; }\n"
        "long addOne(long x) { return x + 1; }\n"
        "int a(int  g(int),  int  v) { return g(v); }\n"
        "int b(long g(long), long v) { return (int)g(v); }\n"
        "int main(void) { return a(twice, 20) + b(addOne, 1); }\n");
    ASSERT_FALSE(model.hasErrors())
        << "two functions whose parameters merely share a NAME must not "
           "collide — the parameter has no linkage (C 6.2.1p4): "
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);

    TypeInterner const& in = model.lattice().interner();
    // Each function keeps two slots, and each `g` keeps its OWN signature —
    // a merged symbol would give both the same type (whichever won the bind).
    std::size_t checked = 0;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) != HirKind::Function) continue;
        auto const* rec = model.recordFor(res->hir.functionSymbol(d));
        if (rec == nullptr || (rec->name != "a" && rec->name != "b")) continue;
        auto const params = res->hir.functionParams(d);
        ASSERT_EQ(params.size(), 2u) << "in function " << rec->name;
        TypeId const pointee =
            adjustedParamPointee(model, res->hir.varDeclType(params[0]));
        ASSERT_TRUE(pointee.valid()) << "in function " << rec->name;
        auto const inner = in.fnParams(pointee);
        ASSERT_EQ(inner.size(), 1u) << "in function " << rec->name;
        TypeKind const want = (rec->name == "a") ? TypeKind::I32 : TypeKind::I64;
        EXPECT_EQ(in.kind(inner[0]), want)
            << "`g` in function " << rec->name << " must keep ITS OWN "
               "signature — a shared file-scope symbol gives both the same one";
        EXPECT_EQ(in.kind(in.fnResult(pointee)), want);
        ++checked;
    }
    EXPECT_EQ(checked, 2u) << "both functions must be present and checked";
}

// ── TF-C112 (D-FFI-PE-CRT-UCRT-MIGRATION): a user re-declaration of a
//    SYNTHESIZED shipped row inherits the SHIM, never a raw import ───────────
//
// THE DEFECT THESE PIN, stated as it was measured rather than as it was
// theorised. `#include <stdio.h>` followed by the legal, ubiquitous
// `int printf(const char *, ...);` makes goal-2 suppress the descriptor's own
// printf row, and the bare-prototype extern synthesis then re-exported the name
// as an ordinary import bound to the suppressed row's library. Post-UCRT-flip
// that library is `ucrtbase.dll`, which exports NO bare printf/fprintf/sprintf/
// vfprintf/sscanf at all — only the `__stdio_common_v*` cores those five names
// are SHIMMED over. Since DSS eager-imports every declared shipped extern
// (D-FFI-DESCRIPTOR-EAGER-IMPORT), the result compiled rc=0 with no diagnostic
// at any stage and then failed to LOAD with 0xC0000139 at process start.
// MEASURED at the TF-C111 HEAD on exactly the three lines below: `objdump -p`
// showed `printf` sitting in the ucrtbase import table beside the four
// correctly-shimmed siblings it should have joined.
//
// Pre-flip the identical code path bound `msvcrt.dll`, which DOES export all
// five, so the channel was always open and merely inert. That is why these pins
// are structural (recipe map / import rows) and not "does it still build".
//
// ★ NOT the same defect as D-CSUBSET-PE-BARE-EXTERN-STILL-MSVCRT-AFTER-UCRT-FLIP.
//   That one is the `extern`-KEYWORD path, which never consults
//   `suppressedShippedSymbolFor` at all, takes the language default library,
//   and yields a CRT split that still loads. Different mechanism, different
//   symptom; do not merge these pins with that one's.

namespace {

// Analyze `src` against the real shippedLibs under a GIVEN object format. The
// format is threaded to BOTH the UnitBuilder (the macro `variants` splice) and
// `analyze` (the per-SYMBOL availability gate), exactly as the driver does —
// load-bearing here, because stdio.json carries two `printf` rows and only the
// pe one is tagged `synthesize`.
[[nodiscard]] SemanticModel analyzeRealStdioAt(std::string src,
                                               ObjectFormatKind format,
                                               DataModel dataModel) {
    // The REAL shipped system-include dir — the descriptors the production
    // driver ships — so a regression in stdio.json itself turns these pins red
    // instead of being mirrored green by a scratch copy. Resolved through the
    // ONE test-side resolver (`repo_root.hpp`: $DSS_CONFIG_ROOT → the
    // CMake-baked repo root → the cwd ancestor walk); the private walk that
    // used to stand here found nothing in an OUT-OF-TREE build and the miss
    // ended in `std::abort()`, which kills the whole test BINARY and costs
    // every sibling test its verdict. `configRoot()` throws instead.
    fs::path const shipped = dss::test::configRoot() / "shippedLibs";
    // D-TEST-A-TORN-SHIPPED-CONFIG-CRASHES-A-SUITE-INSTEAD-OF-REDDING-IT:
    // this was `ADD_FAILURE() << "loadShipped(...) failed"; std::abort();`.
    // ✔MEASURED against an emptied shipped config, the abort took the whole
    // binary out at 0xC0000409 with no `[  FAILED  ]` line, no case name and
    // no summary -- every sibling test in this executable lost its verdict.
    auto const loaded = dss::test_support::shippedSchemaOrThrow("c");
    UnitBuilder builder{loaded, DiagnosticBudget::libraryDefault()};
    builder.addSystemDir(shipped);
    builder.setActiveFormat(format);
    builder.addInMemory(std::move(src), "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    return analyze(cu, DiagnosticBudget::libraryDefault(),
                   dataModel, std::nullopt, std::nullopt, format, "x86_64");
}

// How many `externDecls` rows carry `name` — i.e. how many IMPORTS the lowering
// planted for it. For a shim symbol the only correct answer is zero.
[[nodiscard]] std::size_t externRowsNamed(CstToHirResult const& res,
                                          std::string_view name) {
    std::size_t n = 0;
    for (auto const& e : res.externDecls) if (e.canonicalName == name) ++n;
    return n;
}

// The FIRST symbol spelled `name`, or an invalid id.
[[nodiscard]] SymbolId symbolIdNamed(SemanticModel const& model,
                                     std::string_view name) {
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == name)
            return SymbolId{static_cast<std::uint32_t>(i)};
    return SymbolId{};
}

// The reproducer, verbatim.
constexpr char const* kPrintfRedeclSrc =
    "#include <stdio.h>\n"
    "int printf(const char *fmt, ...);\n"
    "int main(void) { return printf(\"hi\\n\"); }\n";

} // namespace

// THE PRIMARY PIN. Under pe the re-declared `printf` must reach the synth
// recipe map — keyed by the USER prototype's symbol, since goal-2 deleted the
// descriptor's — and must plant NO import row. Both halves are asserted: a
// recipe entry alongside a surviving import would still fail the load, and no
// import with no recipe would be an undefined symbol.
//
// RED BEFORE TF-C112 on BOTH assertions: the map was empty and the import row
// was present.
TEST(HirLoweringC, TFC112RedeclaredPeShimSymbolLowersToARecipeNotAnImport) {
    SemanticModel model = analyzeRealStdioAt(kPrintfRedeclSrc,
                                             ObjectFormatKind::Pe,
                                             DataModel::Llp64);
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok);

    SymbolId const printfSym = symbolIdNamed(model, "printf");
    ASSERT_TRUE(printfSym.valid());
    auto const it = res->synthRecipeBySymbol.find(printfSym.v);
    ASSERT_NE(it, res->synthRecipeBySymbol.end())
        << "the user's prototype must inherit the suppressed row's REALIZATION: "
           "without a recipe entry HIR->MIR never seeds the symbol and no shim "
           "body is ever emitted";
    EXPECT_EQ(it->second, "printf");

    EXPECT_EQ(externRowsNamed(*res, "printf"), 0u)
        << "ucrtbase.dll exports no bare `printf` — an import row here is a "
           "binary that fails to LOAD (0xC0000139) with no diagnostic anywhere";

    // The four SIBLINGS this TU did not re-declare take the injected path and
    // must be unaffected — the fix must not have moved the whole family.
    for (char const* sibling : {"fprintf", "sprintf", "vfprintf", "sscanf"})
        EXPECT_EQ(externRowsNamed(*res, sibling), 0u) << sibling;
    // ...while the UCRT cores the shims call ARE ordinary imports.
    EXPECT_EQ(externRowsNamed(*res, "__stdio_common_vfprintf"), 1u);
}

// THE AGNOSTICISM PIN. The SAME source under elf: glibc exports a real
// `printf`, the elf row carries no `synthesize`, so the prototype must still
// synthesize an ordinary libc import and NOTHING may reach the recipe map. If
// the shim arm ever keys on something other than the descriptor's own tag, this
// goes red by dropping a working import.
TEST(HirLoweringC, TFC112RedeclaredElfStdioSymbolStaysAnOrdinaryImport) {
    SemanticModel model = analyzeRealStdioAt(kPrintfRedeclSrc,
                                             ObjectFormatKind::Elf,
                                             DataModel::Lp64);
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok);

    SymbolId const printfSym = symbolIdNamed(model, "printf");
    ASSERT_TRUE(printfSym.valid());
    EXPECT_EQ(res->synthRecipeBySymbol.count(printfSym.v), 0u)
        << "no elf stdio row declares a synthesize recipe";
    ASSERT_EQ(externRowsNamed(*res, "printf"), 1u)
        << "the c86 bare-proto synthesis must still re-bind the suppressed "
           "descriptor's library — dropping it is an undefined symbol at link";
    for (auto const& e : res->externDecls) {
        if (e.canonicalName != "printf") continue;
        ASSERT_TRUE(e.libraryOverride.contains("elf"));
        EXPECT_EQ(e.libraryOverride.at("elf"), "libc.so.6");
    }
}

// THE REFUSAL. A prototype that suppresses a recipe row but does NOT agree with
// its declared signature cannot be reconciled: the synth pass emits ONE fixed
// body per recipe id, built against the descriptor's signature, so binding a
// divergent prototype to it would marshal the call under one ABI and answer it
// under another — silently. Refusing is the only honest answer, and the refused
// set is essentially clang's own "conflicting types for 'printf'".
//
// Asserted THREE-SIDED: the diagnostic fires, the lowering is NOT ok, and
// neither escape hatch is taken — no recipe entry (which would emit a wrong-ABI
// shim) and no import row (which would not load).
TEST(HirLoweringC, TFC112IncompatibleRedeclarationOfAShimSymbolFailsLoud) {
    SemanticModel model = analyzeRealStdioAt(
        "#include <stdio.h>\n"
        "int printf(const char *fmt);\n"   // NOT variadic — cannot be the shim
        "int main(void) { return printf(\"hi\\n\"); }\n",
        ObjectFormatKind::Pe, DataModel::Llp64);
    // ★ P44 ([[D-CSUBSET-INCOMPATIBLE-REDECL-DIAGNOSED-AT-CALL-SITE-NOT-DECLARATION]]):
    // this used to `ASSERT_FALSE(model.hasErrors())` — the semantic tier said
    // NOTHING about a declaration that contradicts the platform's own, which is
    // the wrong-TIER half of that row and is why the identical source on an ELF
    // target (where `printf` is an ordinary import, so this gate never runs)
    // compiled completely clean. The declaration-site diagnostic now fires, and
    // this gate is the BACKSTOP rather than the only voice. Both must speak: a
    // shim gate that went quiet once the analyzer complained would leave the
    // wrong-ABI binding unrefused for any caller that lowers anyway.
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 1u)
        << "C23 6.7p4 wants the diagnostic AT THE DECLARATION";
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ShippedShimSignatureMismatch), 1u)
        << "a silently wrong-ABI call is exactly the class this project refuses "
           "to ship; the mismatch must be named, not absorbed";
    SymbolId const printfSym = symbolIdNamed(model, "printf");
    ASSERT_TRUE(printfSym.valid());
    EXPECT_EQ(res->synthRecipeBySymbol.count(printfSym.v), 0u);
    EXPECT_EQ(externRowsNamed(*res, "printf"), 0u);
}

// THE ARITY ARM of the same refusal, on a DIFFERENT recipe — so the gate is
// pinned as a rule over descriptor data rather than as one hard-coded shape,
// and so the "which axis diverged" reporting has a second witness.
TEST(HirLoweringC, TFC112WrongArityRedeclarationOfAShimSymbolFailsLoud) {
    SemanticModel model = analyzeRealStdioAt(
        "#include <stdio.h>\n"
        "int sprintf(char *b, const char *f, int extra, ...);\n"
        "int main(void) { char b[8]; return sprintf(b, \"%d\", 1, 2); }\n",
        ObjectFormatKind::Pe, DataModel::Llp64);
    // P44: the declaration-site diagnostic now fires here too — see the
    // arity-arm's sibling above for why both tiers must speak.
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 1u);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ShippedShimSignatureMismatch), 1u);
    EXPECT_EQ(externRowsNamed(*res, "sprintf"), 0u);
}

// THE NON-SHIM SUPPRESSED ROW IS UNTOUCHED (the selectivity guard). `puts` is a
// real ucrtbase export with no `synthesize` tag, and re-declaring it must still
// take the c86 import path — the shim arm keys on the ROW's recipe, not on "is
// this a suppressed stdio symbol". RED if the arm is ever widened to fire on
// every suppressed row.
TEST(HirLoweringC, TFC112RedeclaredNonRecipePeRowStillSynthesizesTheImport) {
    SemanticModel model = analyzeRealStdioAt(
        "#include <stdio.h>\n"
        "int puts(const char *s);\n"
        "int main(void) { return puts(\"hi\"); }\n",
        ObjectFormatKind::Pe, DataModel::Llp64);
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok);
    SymbolId const putsSym = symbolIdNamed(model, "puts");
    ASSERT_TRUE(putsSym.valid());
    EXPECT_EQ(res->synthRecipeBySymbol.count(putsSym.v), 0u);
    ASSERT_EQ(externRowsNamed(*res, "puts"), 1u);
    for (auto const& e : res->externDecls) {
        if (e.canonicalName != "puts") continue;
        ASSERT_TRUE(e.libraryOverride.contains("pe"));
        EXPECT_EQ(e.libraryOverride.at("pe"), "ucrtbase.dll");
    }
}

// THE SECOND SITE, and it is why the fix is a SHARED helper rather than an
// inline arm. C99 6.7.4p7: a file-scope function every one of whose
// declarations spelled `inline` without `extern` provides NO external
// definition, so TF-C79's `lowerInlineDefinitionAsDeclaration` suppresses the
// body and leaves a DECLARATION behind — a second place a user declaration
// displaces a shipped row and then synthesizes an extern for it. It carried the
// identical defect, and it was found only by tracing every reader of
// `suppressedShippedSymbolFor` instead of only the site the defect report
// named. MEASURED before the refactor, on exactly this source: the same
// `ExternImport{printf, ucrtbase.dll}`, rc=0, no diagnostic, and the same
// 0xC0000139 at process start.
//
// Handing the shim over is not a workaround for the suppression — the external
// definition 6.7.4p7 sends the call to IS the shim on such a target.
TEST(HirLoweringC, TFC112InlineDefinitionOfAShimSymbolAlsoInheritsTheShim) {
    SemanticModel model = analyzeRealStdioAt(
        "#include <stdio.h>\n"
        "inline int printf(const char *fmt, ...) { return -1; }\n"
        "int main(void) { return printf(\"inl\\n\"); }\n",
        ObjectFormatKind::Pe, DataModel::Llp64);
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok);
    SymbolId const printfSym = symbolIdNamed(model, "printf");
    ASSERT_TRUE(printfSym.valid());
    auto const it = res->synthRecipeBySymbol.find(printfSym.v);
    ASSERT_NE(it, res->synthRecipeBySymbol.end());
    EXPECT_EQ(it->second, "printf");
    EXPECT_EQ(externRowsNamed(*res, "printf"), 0u)
        << "the suppressed inline definition must inherit the shim, not plant a "
           "ucrtbase import that cannot load";
}

// ...and its refusal arm, so the signature gate is pinned at BOTH sites rather
// than only at the one the shared helper was written for.
TEST(HirLoweringC, TFC112IncompatibleInlineDefinitionOfAShimSymbolFailsLoud) {
    SemanticModel model = analyzeRealStdioAt(
        "#include <stdio.h>\n"
        "inline int printf(const char *fmt) { return -1; }\n"   // not variadic
        "int main(void) { return printf(\"inl\\n\"); }\n",
        ObjectFormatKind::Pe, DataModel::Llp64);
    // P44: the declaration-site diagnostic now fires for the inline-definition
    // spelling as well — the C99 6.7.4p7 arm is a DECLARATION for compatibility
    // purposes, so it must be judged like one.
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_IncompatibleRedeclaration), 1u);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_FALSE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_ShippedShimSignatureMismatch), 1u);
    EXPECT_EQ(externRowsNamed(*res, "printf"), 0u);
}

// ★★ D-CSUBSET-ENUM-FNSIG-NULLPTR-CONDITIONS-SKIP-THE-TRUTHINESS-CHOKEPOINT
//
// C 6.8.4.1 / 6.8.5 / 6.5.15 / 6.5.13 / 6.5.14 all state ONE rule — a
// controlling (or logical-operand) expression "shall have scalar type" and is
// compared unequal to 0 — and `coerceCondition` is the ONE place every such
// site funnels through. Scalar is arithmetic (which INCLUDES the enumerated
// types, C 6.2.5p17) union pointer (C 6.2.5p21); a function designator joins by
// C 6.3.2.1p4 and C23 `nullptr_t` by C 6.3.2.4p2. Before this closed, those
// three kinds fell through that function UNCHANGED and reached the MIR CondBr
// terminator still carrying their source type, where the verifier fired
// I_TerminatorTypeMismatch (I_NullptrTypeInMir for nullptr) on LEGAL input —
// while `int y = e; if (y)`, `!e` and `e != 0` all worked.
//
// This pin names the HIR tier: the SHAPE the conversion must produce. The MIR
// sibling (MirLoweringC.ScalarFamilyConditionsLowerToABoolCondBr) pins the
// consequence, and examples/c/scalar_condition_conversions proves the VALUES on
// every run leg.
//
// RED-ON-DISABLE (REMOVE direction): delete the `ck == TypeKind::Enum` arm in
// `coerceCondition` and an enum condition synthesizes NO Ne at all — the count
// below goes 7 -> 0. The count is per-SITE on purpose: reverting the arm cannot
// be masked by any one site keeping a private conversion of its own.
TEST(HirLoweringC, EnumConditionTakesTheTruthinessChokepointAtEverySite) {
    SemanticModel model = analyzeC(
        "enum Color { NONE = 0, EVEN = 4 };\n"
        "int f(enum Color c) {\n"
        "    int r = 0;\n"
        "    if (c) r = 1;\n"                       // 6.8.4.1  selection
        "    while (c) { r = 2; c = NONE; }\n"      // 6.8.5    iteration
        "    for (; c; c = NONE) r = 3;\n"          // 6.8.5    iteration
        "    r = c ? 4 : 5;\n"                      // 6.5.15   conditional
        "    r = (c && 1) ? 6 : 7;\n"               // 6.5.13   logical AND
        "    r = (c || 0) ? 8 : 9;\n"               // 6.5.14   logical OR
        "    _Bool b = c; r = b ? 10 : 11;\n"       // 6.3.1.2  assignment form
        "    return r;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    ASSERT_TRUE(fn.valid());
    auto const& ti = model.lattice().interner();

    // Count the SYNTHESIZED truth tests whose left operand is the enum→int
    // projection: a `Ne` BinaryOp typed Bool whose child[0] is a Cast FROM an
    // Enum-typed node. That is exactly the shape this arm owes, and nothing
    // else in the program produces it.
    std::size_t enumTruthTests = 0;
    auto const walkEnum = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::BinaryOp
            && isCoreOp(res->hir.payload(n))
            && decodeCoreOp(res->hir.payload(n)) == HirOpKind::Ne) {
            auto const kids = res->hir.children(n);
            if (kids.size() == 2 && res->hir.kind(kids[0]) == HirKind::Cast) {
                auto const inner = res->hir.children(kids[0]);
                if (!inner.empty() && res->hir.typeId(inner[0]).valid()
                    && ti.kind(res->hir.typeId(inner[0])) == TypeKind::Enum) {
                    ++enumTruthTests;
                    // The projection target is the enum's UNDERLYING integer
                    // (C 6.7.2.2), and the test itself is Bool — the two
                    // properties the MIR CondBr invariant depends on.
                    EXPECT_EQ(ti.kind(res->hir.typeId(kids[0])), TypeKind::I32)
                        << "the enum must project to its underlying integer, "
                           "not to Bool: a Cast-to-Bool lowers as Trunc and "
                           "would report the EVEN=4 enumerator as false";
                    EXPECT_EQ(ti.kind(res->hir.typeId(n)), TypeKind::Bool)
                        << "the synthesized truth test is Bool-typed";
                }
            }
        }
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    walkEnum(walkEnum, res->hir.functionBody(fn));

    EXPECT_EQ(enumTruthTests, 7u)
        << "one enum->int truth test per controlling-expression site: if, "
           "while, for, ternary, &&, ||, and the `_Bool b = c` assignment form "
           "-- before the Enum arm, every one of these emitted NONE";
}

// The FUNCTION DESIGNATOR member of the same class.
//
// C 6.3.2.1p4: a function designator used anywhere but as the operand of
// `sizeof`/`_Alignof`/unary `&` converts to pointer-to-function — so `if (fn)`
// and `fn ? a : b` are legal (gcc's -Waddress warns that the address is never
// null; the code compiles and the always-true truth value is CORRECT). It
// decays through the SAME `coerce` FnSig→Ptr funnel the assignment and argument
// paths use, then re-enters so the pointer takes the EXISTING null-pointer `Ne`
// arm — the Array arm's shape exactly.
//
// RED-ON-DISABLE (REMOVE direction): delete the FnSig arm in `coerceCondition`
// and `fnDecayTests` goes 2 -> 0 (and the MIR sibling reds with
// I_TerminatorTypeMismatch, which is what the defect looked like).
TEST(HirLoweringC, FunctionDesignatorConditionDecaysAndTakesTheChokepoint) {
    SemanticModel model = analyzeC(
        "static int helper(int v) { return v + 1; }\n"
        "int f(void) {\n"
        "    int r = 0;\n"
        "    if (helper) r = 1;\n"          // function designator, `if`
        "    r = helper ? 2 : 3;\n"         // function designator, ternary
        "    return r;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    auto const& ti = model.lattice().interner();

    std::size_t fnDecayTests = 0;       // Ne(Cast(FnSig -> Ptr), null)
    auto const walkScalar = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::BinaryOp
            && isCoreOp(res->hir.payload(n))
            && decodeCoreOp(res->hir.payload(n)) == HirOpKind::Ne) {
            auto const kids = res->hir.children(n);
            if (kids.size() == 2 && res->hir.kind(kids[0]) == HirKind::Cast) {
                auto const inner = res->hir.children(kids[0]);
                if (!inner.empty() && res->hir.typeId(inner[0]).valid()
                    && ti.kind(res->hir.typeId(inner[0])) == TypeKind::FnSig) {
                    ++fnDecayTests;
                    EXPECT_EQ(ti.kind(res->hir.typeId(kids[0])), TypeKind::Ptr)
                        << "a function designator decays to pointer-to-function "
                           "(C 6.3.2.1p4) before the truth test";
                }
            }
        }
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    // ⚠ EVERY function, not `firstFunction`: `helper` is declared first here, so
    // scanning only the first body would count nothing and the test would be
    // vacuously satisfiable by deleting the conditions from the fixture.
    std::size_t bodies = 0;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) != HirKind::Function) continue;
        ++bodies;
        walkScalar(walkScalar, res->hir.functionBody(d));
    }
    ASSERT_EQ(bodies, 2u) << "both `helper` and `f` must have been lowered";

    EXPECT_EQ(fnDecayTests, 2u)
        << "one FnSig->Ptr decay + truth test per designator condition";
}

// ★★ D-CSUBSET-COMPLEX — the `_Complex` member of the same scalar family, and the
// same ONE chokepoint.
//
// C 6.2.5 makes the complex types floating (p11), the floating types arithmetic
// (p18) and the arithmetic types scalar (p21), so `if (z)` is governed by the very
// sentence the enum/designator siblings above quote: the controlling expression
// "shall have scalar type" and is compared unequal to 0. `isArithmeticCore` is the
// int+float CORE roster and deliberately excludes Complex — it is also `coerce`'s
// implicit-conversion gate, where a complex↔real conversion is NOT a plain Cast —
// so the truthiness arm admits Complex explicitly instead.
// ✔MEASURED at 301e2a63 (x86_64:pe64-x86_64-windows-exec): five
// `I_TerminatorTypeMismatch` on one probe covering `if` / `&&` / `||` / `? :` /
// `while` / `for`, against gcc 13.3.0 (`-std=c2x`) and clang 18.1.3 (`-std=c23`),
// probed SEPARATELY, both compiling and running every one of them.
//
// This pin names the HIR tier — the SHAPE the conversion must produce. The MIR
// siblings (MirLoweringC.ComplexConditionTestsBothComponentsAgainstZero and its
// `!` / `(_Bool)` / `==` / `!=` neighbours) pin the componentwise lowering, and
// examples/c/c99_complex_truth proves the VALUES on every run leg.
//
// ✔ SEVEN SITES SINCE P42, AND THE SEVENTH IS THE ONE THIS COMMENT USED TO RECORD
// AS MISSING. It read: *"SIX SITES, NOT SEVEN … the `_Bool b = z;` assignment form
// the enum sibling counts is still refused one tier UP, by `isAssignable`'s
// `scalarConvertsToBool` roster … The realize side is READY for it … so admitting
// Complex there is the only remaining step."* That step landed (P42 lane H): the
// roster now names `TypeKind::Complex` beside `Char` / `Enum` / `Ptr` / `NullptrT`,
// for the reason C 6.2.5p11/p18/p21 gives and the same reason those are named —
// `Complex` is a NOMINAL kind in DSS, so widening `isArithmetic` instead would
// re-answer the usual-arithmetic-conversion question everywhere it is asked.
// ✔MEASURED through the shipped CLI on x86_64:pe64-x86_64-windows-exec: before,
// `_Bool b = z;` was `error[S0003]` while `if (z)` on the SAME type ran correctly —
// the assignment site and the condition site disagreeing about one type's truth
// value; after, the probe compiles and RUNS exit 2, agreeing with gcc 13.3.0
// (`-std=c2x`) and clang 18.1.3 (`-std=c23`), which both accept it.
// ★ Admit ⟺ realize needed no second change and could not have needed one:
// `coerce`'s `_Bool`-target arm names NO kinds — it defers to `coerceCondition`,
// whose Complex arm is what this test counts.
//
// RED-ON-DISABLE (REMOVE direction): drop `|| ck == TypeKind::Complex` from
// `coerceCondition`'s arithmetic guard and a complex condition synthesizes NO Ne at
// all — the count below goes 7 -> 0. Per-SITE on purpose: the revert cannot be
// masked by any one site keeping a private conversion. ⚠ The SEVENTH site has a
// SECOND revert that reds it alone: drop `|| rk == TypeKind::Complex` from
// `scalarConvertsToBool` in `analysis/semantic/type_rules.hpp` and the count goes
// 7 -> 6 while `ASSERT_FALSE(model.hasErrors())` fires first with S0003.
// ★★ D-TEST-THE-HIR-LOWERING-FIXTURE-ANALYZES-WITH-NO-TARGET-IN-SCOPE — this
// file's fixture must see what the shipped CLI sees.
//
// `analyzeC` called `analyze(cu, budget)` and nothing more, so its 290-odd
// fixtures analyzed with `target == nullptr`. That default is CORRECT for the
// direct API (the LSP and the FFI header parser have no target), which is exactly
// what makes it dangerous HERE: a construct the CLI compiles can still red in this
// binary, and the red is indistinguishable from a compiler defect. ✔MEASURED in
// P42: a lane spent a full measurement pass proving that a red in this file "is not
// a compiler defect — the exact source compiles and runs correctly through the
// CLI". A fixture that can fail on working code is a fixture that costs a lane a
// cycle every time it does.
//
// ★ WHAT IS ASSERTED, and it is the strongest property available: not that the
// pointer is non-null, but that the target is REACHED and ANSWERS — the model
// republishes it, it resolves the `"r"` letter through `asmConstraint` (a
// question-answering object; the analyzer never learns WHICH processor it is), and
// the lowered descriptor's operands carry a RESOLVED binding. With no target in
// scope `HirInlineAsmOperand::regClassResolved` and `operandKindResolved` are BOTH
// false — the header's own "the letter resolved to nothing" condition — and
// `lowerInlineAsm` refuses rather than guessing a register bank. So this is a
// property no other construct in this file can supply and none can fake.
//
// RED-ON-DISABLE: make `fixtureTarget()` return `nullptr` (the pre-fix state) and
// the operand assertions go from 2 resolved to 0 while the `model.target()` assert
// fires first. ⚠ The pin is deliberately NOT the `_Complex` truthiness test the
// defect surfaced under — that test measures the semantic tier's roster and passes
// either way, so it could never have witnessed this.
TEST(HirLoweringC, TheFixtureAnalyzesWithTheSameTargetInScopeAsTheCli) {
    SemanticModel model = analyzeC(
        "int f(int a) {\n"
        "    int r;\n"
        "    __asm__ (\"movl %1, %0\" : \"=r\"(r) : \"r\"(a));\n"
        "    return r;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << "the inline-asm fixture must analyze clean";

    // 1. The model republishes the target the fixture threaded — the `dataModel`
    //    two-tier discipline, which is what lets the HIR lowering read the SAME
    //    target the semantic tier resolved against.
    ASSERT_NE(model.target(), nullptr)
        << "analyzeC must thread the shipped target, as compile_pipeline.cpp does";
    // 2. It ANSWERS. `"r"` is a letter every shipped processor declares; asking
    //    through `asmConstraint` is the question-answering form (never an arch
    //    identity string), so this assertion is target-agnostic by construction.
    EXPECT_NE(model.target()->asmConstraint("r"), nullptr)
        << "the threaded target must resolve the `r` constraint letter";

    // 3. The RESOLUTION reached the lowered operands — the property that is false
    //    with no target and that every downstream tier consumes.
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    std::size_t asmStmts = 0, resolvedOperands = 0;
    auto const walkAsm = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::InlineAsm) {
            std::uint32_t const h = res->hir.payload(n);
            if (res->inlineAsmPool.contains(h)) {
                ++asmStmts;
                for (auto const& op : res->inlineAsmPool.at(h).operands)
                    if (op.regClassResolved || op.operandKindResolved)
                        ++resolvedOperands;
            }
        }
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) != HirKind::Function) continue;
        walkAsm(walkAsm, res->hir.functionBody(d));
    }
    ASSERT_EQ(asmStmts, 1u) << "one `__asm__` statement was lowered";
    EXPECT_EQ(resolvedOperands, 2u)
        << "BOTH operands (`=r` out, `r` in) must carry a binding resolved against "
           "the target -- with no target in scope regClassResolved and "
           "operandKindResolved are both false and lowerInlineAsm refuses";
}

TEST(HirLoweringC, ComplexConditionTakesTheTruthinessChokepointAtEverySite) {
    SemanticModel model = analyzeC(
        "int f(double _Complex z) {\n"
        "    int r = 0;\n"
        "    if (z) r = 1;\n"                       // 6.8.4.1  selection
        "    while (z) { r = 2; break; }\n"         // 6.8.5    iteration
        "    for (; z; ) { r = 3; break; }\n"       // 6.8.5    iteration
        "    r = z ? 4 : 5;\n"                      // 6.5.15   conditional
        "    r = (z && 1) ? 6 : 7;\n"               // 6.5.13   logical AND
        "    r = (z || 0) ? 8 : 9;\n"               // 6.5.14   logical OR
        "    _Bool b = z;\n"                        // 6.5.16.1 assignment/init
        "    r = b ? 10 : 11;\n"                    // (uses `b`; its condition is
        "    return r;\n"                           //  already Bool, so it adds no
        "}\n");                                     //  second complex truth test)
    // ⚠ THE FAILURE MESSAGE NAMES THE DIAGNOSTIC CODE, not just the offending
    // token. ✔MEASURED the hard way: this assert once fired printing only `z`,
    // which says nothing about WHICH rule rejected it — the code is the whole
    // diagnostic value of the message.
    if (model.hasErrors()) {
        std::string why;
        for (auto const& d : model.diagnostics().all()) {
            why += "[" + std::to_string(static_cast<int>(d.code)) + "] " + d.actual + " | ";
        }
        FAIL() << "the fixture must analyze clean; diagnostics: " << why;
    }
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    ASSERT_TRUE(fn.valid());
    auto const& ti = model.lattice().interner();

    // The shape this arm owes: a `Ne` BinaryOp typed Bool whose LEFT operand is
    // Complex-typed and whose RIGHT operand is the synthesized complex ZERO — a
    // Cast of a real zero INTO the complex type, which is the same node
    // `materializeComplexCast` already realizes as `(0, 0)`. Asserting the zero's
    // SHAPE (not merely "some second operand") is what makes the pin catch a zero
    // minted as a Complex-typed integer literal, which has no MIR realization.
    std::size_t complexTruthTests = 0;
    auto const walkComplex = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::BinaryOp
            && isCoreOp(res->hir.payload(n))
            && decodeCoreOp(res->hir.payload(n)) == HirOpKind::Ne) {
            auto const kids = res->hir.children(n);
            if (kids.size() == 2 && res->hir.typeId(kids[0]).valid()
                && ti.kind(res->hir.typeId(kids[0])) == TypeKind::Complex) {
                ++complexTruthTests;
                EXPECT_EQ(ti.kind(res->hir.typeId(n)), TypeKind::Bool)
                    << "the synthesized truth test is Bool-typed";
                EXPECT_EQ(res->hir.kind(kids[1]), HirKind::Cast)
                    << "the complex zero is a Cast of a real zero INTO the "
                       "complex type -- a Complex-typed LITERAL is memory-"
                       "resident with no MIR realization at all";
                ASSERT_TRUE(res->hir.typeId(kids[1]).valid());
                EXPECT_EQ(ti.kind(res->hir.typeId(kids[1])), TypeKind::Complex);
                auto const inner = res->hir.children(kids[1]);
                ASSERT_EQ(inner.size(), 1u);
                EXPECT_EQ(res->hir.kind(inner[0]), HirKind::Literal);
                ASSERT_TRUE(res->hir.typeId(inner[0]).valid());
                EXPECT_EQ(ti.kind(res->hir.typeId(inner[0])), TypeKind::F64)
                    << "the zero is built at the complex's ELEMENT type";
            }
        }
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    walkComplex(walkComplex, res->hir.functionBody(fn));

    EXPECT_EQ(complexTruthTests, 7u)
        << "one complex truth test per site that asks a complex value for a truth "
           "value: if, while, for, ternary, && and || (the controlling-expression "
           "six -- before the Complex arm every one of these reached the MIR "
           "CondBr terminator carrying its raw complex type) PLUS the `_Bool b = z` "
           "assignment/init, which this pin recorded as missing until the semantic "
           "tier's scalarConvertsToBool roster admitted Complex (P42)";
}

// ★ D-CSUBSET-COMPLEX-UNARY-PLUS-REFUSED (P42 lane AI) — `+z` on a `_Complex`.
//
// C 6.2.5p11 puts the complex types inside the FLOATING types and p18 therefore
// makes them arithmetic, so `+z` satisfies C 6.5.3.3p1 exactly as `-z` does.
// ✔MEASURED at the pre-fix tree through the shipped CLI
// (x86_64:pe64-x86_64-windows-exec, binaries RUN): `+z` was
// `error[H0009] operand of unary '+' must have arithmetic type` while `-z` on the
// SAME value compiled and ran; gcc 13.3.0 (`-std=c2x`) and clang 18.1.3
// (`-std=c23`), probed SEPARATELY, both compile and run `+z`. The gate read
// `isArithmeticCore`, the int+float CORE roster, which excludes Complex — the
// same roster mismatch that produced the `!z` silent miscompile one operator over
// ([[D-CSUBSET-COMPLEX-LOGICAL-NOT-AND-BOOL-CAST-SILENT-MISCOMPILE]]).
//
// ⚠ THE NEGATIVE IS IN THE SAME TEST ON PURPOSE. `+` is the ONE unary operator
// carrying an operand-type gate (`-` has none), so a fix that simply deleted the
// gate would pass every positive assertion. The pointer arm below is what makes
// this a widening of one kind rather than a removal.
TEST(HirLoweringC, ComplexUnaryPlusIsAdmittedAndAPointerOperandStaysRefused) {
    {
        SemanticModel model = analyzeC(
            "int f(double _Complex z) {\n"
            "    double _Complex w = +z;\n"
            "    return (int)__builtin_creal(w);\n"
            "}\n");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_TRUE(res->ok)
            << "`+z` must lower; got: "
            << (r.all().empty() ? std::string{"<no diagnostic>"} : r.all()[0].actual);
    }
    {
        // The gate still fires for an operand C does NOT call arithmetic.
        SemanticModel model = analyzeC(
            "int f(int *p) { return (int)(long long)+p; }\n");
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        EXPECT_FALSE(res->ok)
            << "unary `+` on a POINTER must stay a loud refusal — without this "
               "arm the Complex widening is indistinguishable from deleting the "
               "operand-type gate entirely";
    }
}

// ★★ D-CSUBSET-ASSIGNMENT-TO-A-NON-LVALUE-REFUSED-BY-THE-LOWERING-TIER
// (P42 lane AI) — C 6.5.16p2.
//
// Neither assignment path asked whether its left operand was an lvalue.
// `lowerAssign` (statement position) lowered the LHS as an ordinary expression
// and wrapped an `AssignStmt` around whatever came back; `classifyLvalue` (value
// position) took `AddressOf` of anything its two pre-filters had not claimed. The
// refusal then happened three tiers down, in MIR's `lowerLvalueAddress`, as a
// message about the NODE KIND that arrived.
// ✔MEASURED 2026-08-27 through the shipped CLI on x86_64:pe64-x86_64-windows-exec
// — every shape refused, so never a miscompile, and every one wrong about WHY:
// `8 = 5;` reported `H_UnsupportedLoweringForKind: string-literal materialization:
// the Literal pool entry is not a string arm` — a message about STRING literals,
// for an integer one — while `(a+b) = 5;` and `f() = 5;` reported `lvalue kind
// 'BinaryOp' (ordinal 30)` and `'Call' (ordinal 27)`, naming raw internal
// ordinals. gcc 13.3.0 (`-std=c2x`) and clang 18.1.3 (`-std=c23`), probed
// SEPARATELY, both refuse at the user's own token.
//
// ⚠ THE POSITIVE ARMS ARE HALF THE TEST. A guard that rejected every assignment
// would satisfy the three negatives, so the four lvalue SHAPES C actually admits
// — a plain variable (`Ref`), a deref, an index and a member — are asserted to
// still lower clean in the same fixture.
TEST(HirLoweringC, AssignmentRequiresAModifiableLvalue) {
    auto refusalCount = [](char const* src) {
        SemanticModel model = analyzeC(src);
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        std::size_t n = 0;
        for (auto const& d : r.all()) {
            if (d.code == DiagnosticCode::S_AssignNeedsModifiableLvalue) ++n;
        }
        return n;
    };
    EXPECT_EQ(refusalCount("int f(void) { 8 = 5; return 0; }\n"), 1u)
        << "an integer literal has no object — this used to report a STRING "
           "literal materialization failure";
    EXPECT_EQ(refusalCount("int f(int a, int b) { (a + b) = 5; return a; }\n"), 1u)
        << "an arithmetic result is an rvalue";
    EXPECT_EQ(refusalCount("int g(void) { return 1; }\n"
                           "int f(void) { g() = 5; return 0; }\n"), 1u)
        << "a call result is an rvalue";
    EXPECT_EQ(refusalCount("int f(int a) { (int)a = 5; return a; }\n"), 1u)
        << "a cast result is an rvalue (C 6.5.4 yields a value, not an object)";
    // ── the four shapes C DOES admit ──
    EXPECT_EQ(refusalCount(
                  "struct S { int m; };\n"
                  "int f(int *p, int *arr, struct S *s, struct S v) {\n"
                  "    int a = 0;\n"
                  "    a = 1;\n"          // Ref — a plain variable
                  "    *p = 2;\n"         // Deref
                  "    arr[0] = 3;\n"     // Index
                  "    s->m = 4;\n"       // MemberAccess through a pointer
                  "    v.m = 5;\n"        // MemberAccess on a value
                  "    return a + v.m;\n"
                  "}\n"), 0u)
        << "every lvalue shape C admits must still assign — without this arm the "
           "guard is satisfiable by refusing all assignment";
    // ★★ THE PARENTHESIZED FORM, AND IT IS THE ARM THAT WAS PAID FOR. C 6.5.1p5:
    // a parenthesized expression is an lvalue if the unparenthesized one is. The
    // first draft of the guard left `Ref` out of the addressable roster on the
    // reasoning that `simpleLvalue` claims every plain variable first — and
    // `examples/c/array_decay_deref` reddened in one run on `((out) = (u32)*(zBuf))`,
    // because it does NOT claim a PARENTHESIZED one.
    EXPECT_EQ(refusalCount(
                  "int f(void) { int x = 1; int y; y = ((x) = 5); return y; }\n"), 0u)
        << "a parenthesized variable is an lvalue (C 6.5.1p5) — the corpus refuted "
           "the first draft here";
}

// ★ D-CSUBSET-ASSIGNMENT-TO-A-NON-LVALUE-REFUSED-BY-THE-LOWERING-TIER, the
// SIBLING divergence the same roster was already shipping (P42 lane AI).
//
// `classifyIncDecLvalue` carried the addressable roster inline and it too omitted
// `Ref`, so `++(x)` and `(x)++` on a PARENTHESIZED variable were refused
// `S_IncDecNeedsModifiableLvalue`. ✔MEASURED: gcc 13.3.0 (`-std=c2x`) compiles and
// RUNS both (exit 0); DSS refused. One roster, three callers, two bugs — which is
// the argument for extracting `loweredNodeIsAddressable` rather than adding a
// second inline copy at the assignment sites.
TEST(HirLoweringC, IncDecOnAParenthesizedVariableIsAnLvalue) {
    SemanticModel model = analyzeC(
        "int f(void) { int x = 1; ++(x); (x)++; return x; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    EXPECT_TRUE(res->ok)
        << "`++(x)` / `(x)++` must lower; got: "
        << (r.all().empty() ? std::string{"<no diagnostic>"} : r.all()[0].actual);
    // The negative that keeps the widening bounded: a manifest rvalue operand is
    // still refused, so `Ref` joining the roster did not delete the ++/-- guard.
    SemanticModel bad = analyzeC("int f(void) { return (5)++; }\n");
    DiagnosticReporter r2;
    auto res2 = lowerToHir(bad, r2);
    EXPECT_FALSE(res2->ok)
        << "`(5)++` has no object — the ++/-- lvalue guard must still fire";
}

// ★★ D-CSUBSET-SWITCH-ON-A-NON-INTEGER-DISCRIMINANT-ACCEPTED (P42 lane AI).
//
// C 6.8.4.2p1: "The controlling expression of a switch statement shall have
// integer type." There was NO type constraint on it at any tier.
// ✔MEASURED at the pre-fix tree through the shipped CLI
// (x86_64:pe64-x86_64-windows-exec): `switch (z)` on a `double _Complex`
// COMPILED and RAN, while gcc 13.3.0 (`-std=c2x`) — *switch quantity not an
// integer* — and clang 18.1.3 (`-std=c23`) — *statement requires expression of
// integer type* — probed SEPARATELY, both REFUSE. That is direction B of the bar:
// accepting what NO reference accepts and the standard forbids is an invented
// extension.
// ⚠ AND IT WAS NOT INERT. A `_Complex` is memory-resident and reaches the
// discriminant position as its ADDRESS, so a `case 1:` would have compared an
// object address against 1 and silently taken `default` forever — an accepted
// program with a wrong answer.
//
// ⚠ THE POSITIVE CONTROLS ARE THE POINT OF THE THIRD AND FOURTH ARMS: the roster
// is C's INTEGER one, not `isArithmeticCore` (which admits the FLOAT kinds), so
// the FLOAT arm must refuse while `int` and `enum` must still pass. A guard that
// reddened every switch would satisfy the two negatives alone.
TEST(HirLoweringC, SwitchDiscriminantMustHaveIntegerType) {
    auto lowers = [](char const* src) {
        SemanticModel model = analyzeC(src);
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        return res->ok;
    };
    EXPECT_FALSE(lowers(
        "int f(double _Complex z) { switch (z) { default: return 1; } }\n"))
        << "a complex controlling expression must be refused (C 6.8.4.2p1)";
    EXPECT_FALSE(lowers(
        "int f(double d) { switch (d) { default: return 1; } }\n"))
        << "a FLOATING controlling expression must be refused too — both "
           "references reject it under the same clause, and this is what "
           "separates C's integer roster from `isArithmeticCore`";
    EXPECT_TRUE(lowers(
        "int f(int n) { switch (n) { case 1: return 2; default: return 1; } }\n"))
        << "the ordinary integer switch must still lower";
    EXPECT_TRUE(lowers(
        "enum E { A, B };\n"
        "int f(enum E e) { switch (e) { case A: return 2; default: return 1; } }\n"))
        << "C 6.2.5p17 makes an enumerated type an INTEGER type; DSS interns Enum "
           "as its own nominal kind, so it must be named in the roster explicitly";
}

// ★★★ D-HIR-VLA-PROBE-ABORTS-ON-AN-UNRESOLVED-DECLARED-TYPE — a declaration whose
// type the semantic tier could not resolve must FAIL LOUD, not kill the compiler.
//
// ✔MEASURED at 301e2a63 on x86_64:pe64-x86_64-windows-exec, with a gdb backtrace:
// `lowerVarLikeInto`'s pointer-to-VLA probe called the RAW `interner.kind(type)` on
// `rec->type`, which is `InvalidType` whenever the declared type is unresolved, and
// the lattice's arena guard aborted the process —
//     dss::substrate fatal: TypeInterner::get: TypeId out of range
// rc=127, no diagnostic, no source location. Its two siblings on the same line
// (`isVlaArray`, `typeContainsVla`) both guard against exactly this and say so in
// their own comments; the probe added later did not.
// SIX shapes reached it, every one legal C that gcc 13.3.0 (`-std=c2x`) and clang
// 18.1.3 (`-std=c23`) compile and run: a `typeof(<literal>)` parameter (in a
// definition AND in a prototype), a file-scope `static typeof(1) g;`,
// `typeof(1) a[3]`, `typeof(1) *p`, and `typeof(nullptr) p`.
//
// ⚠⚠ THE FIXTURE CHANGED AT P42 BECAUSE ITS ORIGINAL PREMISE WAS A DEFECT, AND
// THE DEFECT IS FIXED. This test used to reach the probe with
// `static int f(typeof(1) p) { return p; }` — legal C whose declared type the
// semantic tier could not resolve, which is what
// [[D-CSUBSET-TYPEOF-VALUE-FORM-RESOLVES-ONLY-FOR-AN-OBJECT-OPERAND]] was. That
// row is CLOSED (P42 lane H): `typeof(1)` now resolves in every specifier
// position, so the old fixture compiles and RUNS (✔MEASURED exit 7), leaves no
// typeless declaration node, and the pin went green-by-vacuity — `typelessDecls`
// was 0 and the EXPECT_GE below caught it.
// ★ THE LESSON, AND IT IS WHY THE FIXTURE WAS REPLACED RATHER THAN THE ASSERTION
// WEAKENED: a fixture that synthesizes its precondition out of a BUG has a
// shelf life ending the day the bug is fixed, and it takes the pin with it.
// The replacement synthesizes the unresolved type out of something that can
// never become valid — an UNDECLARED name inside the `typeof` operand. The
// semantic tier reports S_Undeclared (✔MEASURED through the shipped CLI on
// x86_64:pe64-x86_64-windows-exec: `error[S0001] nosuchsymbol`) and the
// parameter's type stays InvalidType, which is exactly the state the probe used
// to abort on. Nothing can "fix" an undeclared name into a resolved type, so
// this fixture cannot rot the same way.
//
// What this tier owes is that an unresolved type produces a DIAGNOSTIC, and it does:
// `requiresValidType(HirKind::VarDecl)` is true, so the HIR verifier raises
// H_TypeUnresolved with the declarator's own source span.
//
// ★ THE ASSERTION IS PARTLY THAT THIS TEST RETURNS AT ALL. An abort inside
// `lowerToHir` takes the whole executable out at 0xC0000409 with no `[ FAILED ]`
// line — the failure mode `analyzeC`'s own comment records — so ctest reports the
// crash rather than a case. That is the red; the EXPECTs below are what stops it
// from being satisfied by a silent accept instead.
//
// RED-ON-DISABLE (REMOVE direction): change `} else if (type.valid()) {` back to
// `} else {` in `lowerVarLikeInto` — this test's process aborts and ctest reports
// hir/test_hir_lowering_c as crashed rather than failed.
TEST(HirLoweringC, AnUnresolvedDeclaredTypeFailsLoudInsteadOfAbortingTheCompiler) {
    // A parameter typed `typeof(<undeclared name>)`. The semantic tier reports
    // S_Undeclared for the operand and leaves the parameter symbol's type
    // UNRESOLVED — the state the pointer-to-VLA probe used to abort on. An
    // undeclared name cannot later become resolvable, so this precondition does
    // not depend on any open defect (see the docblock above).
    SemanticModel model = analyzeC(
        "static int f(typeof(nosuchsymbol) p) { return p; }\n"
        "int main(void) { return f(0); }\n");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);          // must RETURN, not abort
    ASSERT_TRUE(res != nullptr);

    // The unresolved type must still be visible as a TYPELESS declaration node,
    // which is what makes the HIR verifier's H_TypeUnresolved fire. Asserting the
    // node EXISTS and is typeless is stronger than asserting "no crash": a
    // lowering that silently dropped the declarator would also not crash.
    std::size_t typelessDecls = 0;
    auto const walk = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        HirKind const k = res->hir.kind(n);
        if ((k == HirKind::VarDecl || k == HirKind::Global)
            && !res->hir.typeId(n).valid())
            ++typelessDecls;
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    walk(walk, res->hir.root());
    EXPECT_GE(typelessDecls, 1u)
        << "the unresolved parameter must survive as a TYPELESS declaration node "
           "so the HIR verifier can report H_TypeUnresolved against its span -- a "
           "silently dropped declarator would be a quiet accept, which is worse "
           "than the abort this replaced";
}

// D-CSUBSET-NULLPTR-T-DECLARABLE — the C23 `nullptr_t` OBJECT, and the property
// that replaced the per-site condition arm this test used to pin.
//
// ★ WHAT CHANGED AND WHY THE PIN GOT STRONGER RATHER THAN JUST DIFFERENT.
// `nullptr_t` used to be a SEMANTIC-TIER-ONLY kind: legal only as the type of the
// `nullptr` literal, which lowers to an integer zero before HIR ever sees it. An
// OBJECT of the type therefore could not compile at all — ✔MEASURED at 301e2a63,
// `typeof(nullptr) o = nullptr;` failed with `I_NullptrTypeInMir` +
// `I_StoreValueTypeMismatch`, while gcc 13.3.0 (`-std=c2x`) and clang 18.1.3
// (`-std=c23`) both compile and RUN it. `coerceCondition` carried a NullptrT arm
// that patched exactly ONE position (the condition) of a construct that could not
// compile in any position.
// ⇒ The type gap is now closed at its source: C23 §6.2.5 gives `nullptr_t` the
// size, alignment and representation of `void *`, so
// `TypeInterner::representationType` PROJECTS the kind to `Ptr<Void>` at the
// semantic→HIR boundary while the semantic tier keeps the distinct identity
// `_Generic` and the one-way conversion rules need. The condition arm became
// unreachable and was removed.
// ⇒ So this test no longer asks "did the condition slot get a Bool literal?" (a
// claim about one position) but "can ANY node in the module carry the kind?" —
// which is the invariant the projection actually establishes, and it covers the
// four positions the old pin could not reach: the initializing store, the
// declaration's own type, a same-type copy, and the conversion to `void *`.
//
// The runtime half is `examples/c/nullptr_t_object` (debug AND release, four
// targets); this is the structural half, red on every leg.
//
// RED-ON-DISABLE (REMOVE direction): make `representationType`'s `NullptrT` arm
// return `id` instead of `pointer(primitive(Void))` — the projection becomes the
// identity, `nullptrTypedNodes` goes 0 -> nonzero and the VarDecl's type reverts
// to NullptrT, so BOTH assertions red. (The mutant also reds the MIR pin and the
// corpus example, which is the point: one projection, three tiers.)
TEST(HirLoweringC, NullptrTObjectIsProjectedToItsPointerRepresentation) {
    SemanticModel model = analyzeC(
        "int f(void) {\n"
        "    int r = 0;\n"
        "    typeof(nullptr) v = nullptr;\n"   // the OBJECT + its initializing store
        "    typeof(nullptr) w = v;\n"         // a same-type copy (load + store)
        "    void *p = w;\n"                   // the one-way conversion to a pointer
        "    if (v) r = 1;\n"                  // nullptr_t value, `if`
        "    r = v ? 2 : 3;\n"                 // nullptr_t value, ternary
        "    return r + (p == nullptr ? 0 : 9);\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
            : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    ASSERT_TRUE(fn.valid());
    auto const& ti = model.lattice().interner();

    // (1) TOTALITY — walk EVERY node in the module, not a chosen position. A
    // per-position walk is what let the previous version of this pin be true of
    // the condition while the store one tier down was still refused.
    std::size_t nullptrTypedNodes = 0;
    std::size_t varDeclsSeen      = 0;
    std::size_t voidPtrVarDecls   = 0;
    auto const walkAll = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        TypeId const t = res->hir.typeId(n);
        if (t.valid() && ti.kind(t) == TypeKind::NullptrT) ++nullptrTypedNodes;
        if (res->hir.kind(n) == HirKind::VarDecl) {
            ++varDeclsSeen;
            // (2) IDENTITY OF THE PROJECTION, not merely "not NullptrT": the two
            // `nullptr_t` locals must be `Ptr<Void>` EXACTLY — the representation
            // C23 §6.2.5 assigns the type. Asserting only "not NullptrT" would
            // stay green if the projection picked, say, a bare `u64`.
            if (t.valid() && ti.kind(t) == TypeKind::Ptr) {
                auto const ops = ti.operands(t);
                if (!ops.empty() && ti.kind(ops[0]) == TypeKind::Void)
                    ++voidPtrVarDecls;
            }
        }
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    walkAll(walkAll, res->hir.functionBody(fn));

    EXPECT_EQ(nullptrTypedNodes, 0u)
        << "no HIR node may carry TypeKind::NullptrT in ANY position -- the "
           "semantic->HIR boundary projects the kind to its object "
           "representation, and the I_NullptrTypeInMir tripwire one tier down "
           "reports any site that projection missed";
    // `r`, `v`, `w`, `p` — four locals; `v` and `w` are the nullptr_t pair and
    // `p` is a real `void *`, so exactly three carry `Ptr<Void>`. Pinning the
    // COUNT (not "at least one") is what makes a projection that fires on only
    // the first declarator red.
    EXPECT_EQ(varDeclsSeen, 4u) << "r, v, w, p";
    EXPECT_EQ(voidPtrVarDecls, 3u)
        << "both nullptr_t locals must be typed Ptr<Void> (C23 6.2.5: the size, "
           "alignment and representation of `void *`), alongside the real void* "
           "local";
}

// ── D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN ─────────────
//
// The STATIC-INITIALIZER SCHEDULE, at the tier where the front-end fact becomes
// a carried one. Everything below the HIR reads `LinkageAttr::staticInit`, so a
// fact missing here is a program whose initializer never runs — the exact defect
// this anchor was filed for.
//
// ⚠⚠ THE PAIR OF PINS BELOW IS THE POINT, NOT EITHER ONE ALONE. Adding
// `constructor` to the language's `attributeEffects` table SILENCES the
// `H_UnknownLinkageSpecifier` warning BY CONSTRUCTION — the ignored-names roster
// is DERIVED from that table and the derivation is verb-blind. So "the warning
// stopped" is NOT evidence the attribute is honoured; it is equally consistent
// with the attribute having become silently inert, which is STRICTLY WORSE than
// the announced divergence it replaced. The schedule assertion is what tells the
// two apart, and the `frobnicate` control is what stops the silencing from
// widening to names the language really does not model.
TEST(HirLoweringC, GnuConstructorAttributeRecordsTheBeforeEntrySchedule) {
    SemanticModel model = analyzeC(
        "static int g = 0; "
        "__attribute__((constructor)) static void init(void){ g = 42; } "
        "int main(void){ return g; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res != nullptr);
    EXPECT_TRUE(res->ok);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "`constructor` is vocabulary this language now MODELS, so the linkage "
           "scan must not adjudicate it";
    HirNodeId const f = functionNamed(res->hir, model, "init");
    ASSERT_TRUE(f.valid());
    ASSERT_TRUE(res->linkageMap.has(f))
        << "the side-table is sparse, and `recordLinkage`'s sparseness test had "
           "to GROW to ask about the schedule — an absent entry here means a "
           "non-`static` constructor would have been dropped while the `static` "
           "spelling worked";
    auto const sched = res->linkageMap.get(f).staticInit;
    ASSERT_TRUE(sched.beforeEntry().has_value())
        << "the function must be recorded in the BEFORE-entry channel";
    EXPECT_EQ(*sched.beforeEntry(), kUnprioritizedStaticInit)
        << "the bare spelling is the UNPRIORITIZED priority, which sorts LAST "
           "among constructors — a zero here would sort it first, the opposite "
           "of what all three references do";
    EXPECT_FALSE(sched.afterEntry().has_value())
        << "a `constructor` joins ONE channel";
}

// THE CONTROL for the pin above: silencing `constructor` must not silence a name
// the language models NOWHERE. Without this pair, deleting the unknown-name arm
// outright would look like the feature working.
TEST(HirLoweringC, GnuConstructorSilencingDoesNotWidenToUnmodelledNames) {
    SemanticModel model = analyzeC(
        "__attribute__((frobnicate)) int gv = 11; "
        "__attribute__((constructor)) static void init(void){ gv = 42; } "
        "int main(void){ return gv; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res != nullptr);
    EXPECT_TRUE(res->ok) << "…as a WARNING: every reference compiles this";
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 1u)
        << "EXACTLY one — `frobnicate` still reported, `constructor` no longer. "
           "A count of 2 means the schedule landed without the config row; a "
           "count of 0 means the config row silenced the typo protection too, "
           "which is the announced-to-silent regression this anchor forbids";
}

// The PRIORITY argument reaches the schedule. An implementation that parsed the
// attribute and dropped `(101)` runs a program's initializers in an order all
// three references disagree with, and nothing above this tier could tell.
TEST(HirLoweringC, GnuConstructorPriorityArgumentReachesTheSchedule) {
    SemanticModel model = analyzeC(
        "__attribute__((constructor(101))) static void early(void){} "
        "__attribute__((destructor(102)))  static void late(void){} "
        "int main(void){ return 0; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res != nullptr);
    EXPECT_TRUE(res->ok);
    HirNodeId const e = functionNamed(res->hir, model, "early");
    HirNodeId const l = functionNamed(res->hir, model, "late");
    ASSERT_TRUE(e.valid() && l.valid());
    ASSERT_TRUE(res->linkageMap.has(e) && res->linkageMap.has(l));
    auto const es = res->linkageMap.get(e).staticInit;
    auto const ls = res->linkageMap.get(l).staticInit;
    ASSERT_TRUE(es.beforeEntry().has_value());
    EXPECT_EQ(*es.beforeEntry(), 101u);
    EXPECT_FALSE(es.afterEntry().has_value());
    ASSERT_TRUE(ls.afterEntry().has_value());
    EXPECT_EQ(*ls.afterEntry(), 102u);
    EXPECT_FALSE(ls.beforeEntry().has_value())
        << "`destructor` joins the AFTER-entry channel only — a schedule that "
           "filed it in both would run it twice";
}

// ✔MEASURED (mingw-w64 gcc 13.2.0): `__attribute__((constructor,destructor))`
// compiles clean under `-Wall -Wextra` and runs at BOTH ends. One function, two
// channels, and each may carry its own priority — which is why the schedule is
// two independent optionals rather than a phase plus a priority. A design that
// collapsed them would silently drop one half of every such declaration.
TEST(HirLoweringC, GnuConstructorAndDestructorOnOneFunctionOccupyBothChannels) {
    SemanticModel model = analyzeC(
        "__attribute__((constructor(101))) __attribute__((destructor(102))) "
        "static void both(void){} "
        "int main(void){ return 0; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res != nullptr);
    EXPECT_TRUE(res->ok);
    HirNodeId const f = functionNamed(res->hir, model, "both");
    ASSERT_TRUE(f.valid());
    ASSERT_TRUE(res->linkageMap.has(f));
    auto const s = res->linkageMap.get(f).staticInit;
    ASSERT_TRUE(s.beforeEntry().has_value());
    ASSERT_TRUE(s.afterEntry().has_value());
    EXPECT_EQ(*s.beforeEntry(), 101u);
    EXPECT_EQ(*s.afterEntry(), 102u);
}

// The ordinary C header/impl split: annotate the PROTOTYPE, define plainly. MIR
// is stamped from the DEFINITION's symbol, so without the redeclaration merge the
// schedule would be lost between two declarations of one function.
TEST(HirLoweringC, GnuConstructorOnThePrototypeSurvivesToTheDefinition) {
    SemanticModel model = analyzeC(
        "__attribute__((constructor(101))) static void init(void); "
        "static void init(void){} "
        "int main(void){ return 0; }");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res != nullptr);
    EXPECT_TRUE(res->ok);
    HirNodeId const f = functionNamed(res->hir, model, "init");
    ASSERT_TRUE(f.valid());
    ASSERT_TRUE(res->linkageMap.has(f))
        << "the prototype's schedule must reach the definition's node";
    auto const s = res->linkageMap.get(f).staticInit;
    ASSERT_TRUE(s.beforeEntry().has_value());
    EXPECT_EQ(*s.beforeEntry(), 101u) << "…with its PRIORITY intact, not merely "
                                         "the fact that a schedule exists";
}

// ★★ D-CSUBSET-LINKAGE-INHERITED-INTERNAL-EMITS-GLOBAL (C 6.2.2p4/p5).
//
// A plain definition whose linkage is INHERITED from a visible prior `static`
// declaration is INTERNAL, and until P51 the HIR linkage fold read only the
// definition's OWN specifier tokens — a bare `int f(void){…}` carries no
// `static`, so it emitted `Global`. Single-TU programs could not see it (nothing
// contradicts a binding inside one image); the two-TU corpus example
// `examples/c/linkage_inherited_internal_crosscu` is where it became a REFUSAL
// of a legal program. THIS pin is the unit-tier statement of the same fact, and
// it is the one that names the axis directly rather than through an exit code.
//
// Both halves of the shape are pinned because they reach the emission arm by
// DIFFERENT routes: the function definition outranks a PROTOTYPE in the
// redeclaration merge, the initialized object definition outranks a TENTATIVE.
TEST(HirLoweringC, InheritedInternalLinkageReachesTheEmittedBinding) {
    struct Case {
        char const* what;
        char const* src;
        char const* name;
        SymbolBinding want;
    };
    for (Case const c : {
             // THE DEFECT, function half: the definition has no `static` token.
             Case{"function inheritance",
                  "static int f(void);\n"
                  "int f(void){ return 21; }\n"
                  "int main(void){ return f(); }\n",
                  "f", SymbolBinding::Local},
             // THE DEFECT, object half: the INITIALIZED definition outranks the
             // `static` tentative and becomes the emitting declaration.
             Case{"object inheritance",
                  "static int g;\n"
                  "int g = 3;\n"
                  "int main(void){ return g; }\n",
                  "g", SymbolBinding::Local},
             // CONTROL, and the one that would catch an over-reach: with NO
             // prior `static` the identical definition shapes must stay
             // externally visible. A fix that read the record unconditionally —
             // or that confused "has a record" with "is internal" — makes these
             // Local and hides every symbol from the linker.
             Case{"external function control",
                  "int h(void);\n"
                  "int h(void){ return 21; }\n"
                  "int main(void){ return h(); }\n",
                  "h", SymbolBinding::Global},
             Case{"external object control",
                  "int gg;\n"
                  "int gg = 3;\n"
                  "int main(void){ return gg; }\n",
                  "gg", SymbolBinding::Global},
             // CONTROL on the other side: the path that ALREADY worked (the
             // definition spells `static` itself) must be unchanged, so a green
             // first arm cannot be the fix having simply forced Local everywhere.
             Case{"explicit static definition",
                  "static int s(void){ return 21; }\n"
                  "int main(void){ return s(); }\n",
                  "s", SymbolBinding::Local},
             // CONTROL for the FALLBACK-not-override half. `weak` is a linker
             // binding, not a 6.2.2 linkage class, and the record consult is
             // guarded on `binding == Global` (= "the tokens specified none")
             // precisely so a specified binding survives. Drop that guard and
             // this arm is the only one that moves.
             Case{"weak is not overridden",
                  "__attribute__((weak)) int w(void){ return 21; }\n"
                  "int main(void){ return w(); }\n",
                  "w", SymbolBinding::Weak},
         }) {
        SemanticModel model = analyzeC(c.src);
        ASSERT_FALSE(model.hasErrors()) << c.what;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res != nullptr) << c.what;
        ASSERT_TRUE(res->ok) << c.what << ": "
                             << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(declaredBinding(*res, model, c.name), std::optional{c.want})
            << c.what << " — `" << c.name << "`. `std::nullopt` would mean the "
               "declaration emitted no module decl at all, which is a different "
               "failure from a wrong binding and must not read as one";
    }
}

// The INHERITANCE is a property of the merge SURVIVOR, not of adjacency, so a
// third declaration between the `static` and the definition must not lose it —
// and the `static` may equally arrive AFTER the definition (the unanimously
// accepted `static int f(void); int f(void){…} static int f(void);` triple, plus
// the ordering where the plain PROTOTYPE comes first). Each of these folds a
// different pair through `mergeOrCollideRedeclaration`; all must land Local,
// because the OR-propagation is what carries the fact and this pin is what
// notices if the emission tier starts reading the LAST declaration's tokens
// instead of the record.
TEST(HirLoweringC, InheritedInternalLinkageSurvivesEveryDeclarationOrdering) {
    for (char const* prelude : {
             "static int f(void);\nint f(void){ return 21; }\n",
             "static int f(void);\nstatic int f(void);\nint f(void){ return 21; }\n",
             "static int f(void);\nint f(void);\nint f(void){ return 21; }\n",
             "static int f(void);\nint f(void){ return 21; }\nstatic int f(void);\n",
         }) {
        std::string const src =
            std::string(prelude) + "int main(void){ return f(); }\n";
        SemanticModel model = analyzeC(src);
        ASSERT_FALSE(model.hasErrors()) << prelude;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res != nullptr) << prelude;
        ASSERT_TRUE(res->ok) << prelude << ": "
                             << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(declaredBinding(*res, model, "f"),
                  std::optional{SymbolBinding::Local})
            << prelude << " — the definition inherits internal linkage from the "
               "`static` declaration wherever it sits in the chain; `Global` is "
               "the emission tier reading tokens again";
    }
}

// ★★★ THE SEMANTIC-TIER TWIN OF
// D-C-LINKAGE-SPECIFIER-LOOKUP-IS-POSITION-BLIND-AND-NOT-DUNDER-NORMALIZED —
// the SEMANTIC tier's
// `scanSpecifierPrefixStorage` resolved a declaration-specifier KEYWORD worn as
// an attribute CLAUSE NAME against that keyword's own `linkageSpecifiers` entry.
// P42 fixed exactly this class in the HIR twin `linkageFrom`
// (D-C-LINKAGE-SPECIFIER-LOOKUP-IS-POSITION-BLIND-AND-NOT-DUNDER-NORMALIZED);
// the semantic scan kept the defect for four cycles because its ONE consumer
// was a diagnostic predicate, where a wrong answer is a missed message rather
// than wrong bytes.
//
// ★ THESE PINS LIVE IN THE HIR FILE ON PURPOSE, NOT FOR CONVENIENCE. Two of the
// three consequences are only visible from here: the BINDING half is a HIR
// residue, and `analyzeC` hands back the SemanticModel with its own reporter, so
// the semantic half is readable through `model.diagnostics()` at the same seam.
// A third consequence — the missing 6.2.2p7 mismatch on
// `__attribute__((static)) int gv = 1; static int gv;` — needs a
// `tests/analysis/semantic/` pin and is NOT written here; see the row.
//
// gcc 13.3.0 and clang 18.1.3 were probed SEPARATELY on every arm below and
// WARN-AND-IGNORE all of them (`'static' attribute directive ignored` /
// `unknown attribute 'static' ignored`), building and running each program.
TEST(HirLoweringC, KeywordSpelledAttributeNameConfersNoStorageClass) {
    // THE BINDING HALF. `Local` here is the symbol LEAVING the object: with the
    // P51 emission consult reading `isInternalLinkage`, a mis-mint stops a
    // sibling TU from linking `extern int gv;` at all.
    for (char const* spec : {"__attribute__((static))",
                             "__attribute__((__static__))",
                             "__attribute__((constexpr))",
                             "__attribute__((__constexpr__))"}) {
        std::string const src = std::string(spec)
            + " int gv = 1;\n"
              "int main(void){ return gv - 1; }\n";
        SemanticModel model = analyzeC(src);
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res != nullptr) << spec;
        EXPECT_EQ(declaredBinding(*res, model, "gv"),
                  std::optional{SymbolBinding::Global})
            << spec << " — a storage class is a DECLARATION SPECIFIER; C has no "
                       "attribute form of one, and both references keep `gv` "
                       "exported (nm: `D gv`). `Local` is the symbol vanishing "
                       "from the object";
    }
    // THE THREAD-STORAGE HALF, and it was the loudest: a mis-minted
    // `isThreadLocal` reached Pass 2's 6.7.1 constraint check, so DSS REFUSED
    // (rc=1, S_ThreadLocalOnFunction) a program both references build and run.
    for (char const* spec : {"__attribute__((thread_local))",
                             "__attribute__((__thread_local__))",
                             "__attribute__((_Thread_local))"}) {
        std::string const src = std::string(spec)
            + " int tf(void) { return 0; }\n"
              "int main(void){ return tf(); }\n";
        SemanticModel model = analyzeC(src);
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_ThreadLocalOnFunction), 0u)
            << spec << " — the attribute confers no thread storage, so the "
                       "6.7.1 constraint check has nothing to fire on. A count "
                       "of 1 is a legal program REFUSED";
    }
    // THE MISSING-DIAGNOSTIC HALF, and it is the arm that fails in the OTHER
    // direction — everything above is DSS saying too much, this is DSS saying
    // nothing. The mis-mint made the attribute-spelled declaration look
    // INTERNAL, so `mergeOrCollideRedeclaration`'s `priorPlainDefining` was
    // false and the 6.2.2p7 conflict went unreported. gcc: `error: static
    // declaration of 'gv' follows non-static declaration`; clang: the same
    // error; DSS: rc=0 and silence (✔MEASURED before the fix).
    //
    // ⓘ This is a SEMANTIC-tier diagnostic pinned from the HIR suite on
    // purpose: `analyzeC` returns the model with its own reporter, so the seam
    // is reachable here and no `tests/analysis/**` pin is needed for it.
    {
        SemanticModel model = analyzeC(
            "__attribute__((static)) int gv = 1;\n"
            "static int gv;\n"
            "int main(void){ return gv; }\n");
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_LinkageRedeclarationMismatch), 1u)
            << "the attribute leaves `gv` EXTERNAL, so the later `static` is the "
               "6.2.2p7 conflict both references reject. 0 is the missed "
               "diagnostic the position-blind mint caused";
    }
    // …and the CONTROL for it, because a count of 1 above could equally come
    // from a check that fires on everything: two REAL `static` declarations are
    // a legal redeclaration of an internal identifier and must stay silent.
    {
        SemanticModel model = analyzeC(
            "static int gv = 1;\n"
            "static int gv;\n"
            "int main(void){ return gv - 1; }\n");
        EXPECT_EQ(countCode(model.diagnostics(),
                            DiagnosticCode::S_LinkageRedeclarationMismatch), 0u)
            << "two `static` declarations agree about linkage; a diagnostic here "
               "would be a legal program refused";
    }
}

// ★★ THE CONTROLS, AND THEY CARRY THE WHOLE RISK OF THE FIX ABOVE. The scan now
// skips the language's attribute-specifier shapes wholesale, and the cheap wrong
// version of that — skipping attributes everywhere the linkage vocabulary is
// read — would SILENTLY LOSE `weak` and `visibility`, which are attribute names
// and legitimately live inside `attrSpec`. Only these arms can tell the two
// apart: they assert the attribute vocabulary still APPLIES while the keyword
// vocabulary is denied, which is the exact line the fix had to draw.
TEST(HirLoweringC, AttributeVocabularyStillAppliesAfterTheKeywordDenial) {
    struct Case { char const* src; char const* name; };
    // `weak` and `visibility` reach the HIR fold through the SAME `attrSpec`
    // subtree the storage scan now refuses to enter, so a fix applied one tier
    // too wide reds here and nowhere else.
    {
        SemanticModel model = analyzeC(
            "__attribute__((weak)) int wv = 1;\n"
            "int main(void){ return wv - 1; }\n");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res != nullptr);
        EXPECT_EQ(declaredBinding(*res, model, "wv"),
                  std::optional{SymbolBinding::Weak})
            << "`weak` is an ATTRIBUTE NAME and must still bind; `Global` means "
               "the attribute subtree stopped being read at all";
    }
    {
        SemanticModel model = analyzeC(
            "__attribute__((visibility(\"hidden\"))) int hv = 1;\n"
            "int main(void){ return hv - 1; }\n");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res != nullptr);
        EXPECT_EQ(declaredVisibility(*res, model, "hv"),
                  std::optional{SymbolVisibility::Hidden})
            << "the COMPOSITE attribute key must still resolve through the same "
               "subtree";
    }
    // THE ALREADY-IMMUNE ROWS. Three of the four shipped `linkageSpecifiers`
    // rows list `attrSpec` in `linkageSpecifierIgnoredRules` and were never
    // exposed; the new skip must be a byte-for-byte no-op on them. A block-scope
    // declaration routes through one of those rows, so these two arms are the
    // control that the change did not reach where it had no business.
    {
        // A REAL block-scope `static` still confers static storage: the local is
        // promoted to a hidden module global, so it retains its value.
        SemanticModel model = analyzeC(
            "int bump(void){ static int x = 0; x = x + 1; return x; }\n"
            "int main(void){ bump(); return bump(); }\n");
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res != nullptr);
        EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
        EXPECT_EQ(declaredBinding(*res, model, "x"),
                  std::optional{SymbolBinding::Local})
            << "the static local is still emitted as an internal module global; "
               "`nullopt` means the promotion stopped happening";
    }
    {
        // …and the attribute spelling still confers NONE, at block scope too.
        SemanticModel model = analyzeC(
            "int bump(void){ __attribute__((static)) int x = 0; x = x + 1; "
            "return x; }\n"
            "int main(void){ bump(); return bump(); }\n");
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res != nullptr);
        EXPECT_EQ(declaredBinding(*res, model, "x"), std::nullopt)
            << "an attribute-spelled `static` promotes nothing, so there is no "
               "module-level declaration for `x` at all — it stays an automatic "
               "local, which is what both references do";
    }
}

// ── P51 (D-CSUBSET-INLINE-FUNCTION-SPECIFIER): the LINKAGE-tier truth table
//    over the four baseline spellings ─────────────────────────────────────────
//
// That row's closing witness is "the 4 baseline spellings compile AND the
// emitted symbol's LINKAGE is asserted for each (red-on-disable at the linkage
// tier, never 'it parses')". ✔MEASURED P51: what the tree already carried was
// the SEMANTIC bit (`SemanticAnalyzerC.InlineDefinitionFollowsC99Quantifier`
// asserts `SymbolRecord::isInline`) and the OBSERVABLE consequence (the
// `examples/c/inline_c99_*` exit codes). Neither states what this tier decides —
// which of {an ExternFunction declaration and no emitted body}, {an emitted
// definition with internal binding}, {an emitted definition with external
// binding} the declaration produces. That decision is `lowerInlineDefinitionAsDeclaration`,
// and this is the tier at which it is nameable.
//
// ★ EVERY EXPECTATION IS GROUND TRUTH FROM A REAL TOOLCHAIN. Each row was
// MEASURED with `gcc 13.3.0` and `clang 18.1.3`, `-std=c99` AND `-std=gnu17`,
// `-O0 -c` + `nm`, on this exact source shape; mingw-w64 gcc 13.2.0 agrees on
// all four. The `nm` column each row names is what it is asserting a DSS
// equivalent of:
//   `inline`          -> `U p`  — no external definition in this TU, and the
//                                 reference survives, so the linker sees an
//                                 undefined symbol (it does: DSS reports
//                                 K_SymbolUndefined on this program at debug).
//   `static inline`   -> `t p`  — internal linkage, emitted.
//   `extern inline`   -> `T p`  — 6.7.4p7's with-extern exemption, emitted.
//   `extern __inline` -> `T p`  — the same, through the GNU spelling.
//
// ★★ THE THREE COLUMNS ARE NOT REDUNDANT, and that is why this is a table
// rather than three assertions. `externDecl` alone cannot tell a suppressed
// definition from one that was never lowered; `marked` alone cannot tell an
// unmarked body from an absent one; `binding` alone is Global for two rows that
// differ in everything else. Only the triple names one cell of the truth table.
//
// ⚠ WHAT THIS DELIBERATELY DOES NOT CLAIM. `static inline`'s `t` column is
// about INTERNAL LINKAGE, which this tier records as `SymbolBinding::Local`.
// ✔MEASURED P51 at the object tier: a DSS-emitted `elf64-x86_64-linux`
// relocatable carries NO symbol-table entry at all for an internal-linkage
// function — for `static inline int p` and for a plain `static int p` alike —
// where gcc and clang both emit `t p`. That divergence is a property of `static`,
// not of `inline` (the plain-`static` control shows it identically), so it is
// reported rather than pinned here; this row asserts the linkage DSS records,
// which is the fact `inline` is responsible for.
//
// RED-ON-DISABLE (REMOVE direction), ✔EXERCISED P51: delete the `__inline` and
// `__inline__` rows from the `keywords` table in `c.lang.json` — the C99 word
// left alone — and this test FAILS by name, on the `extern __inline` row, while
// `HirLoweringC` keeps its other 312 passes. The control on the other side is
// the same run's five pure-`inline` corpus examples, which stay green.
//
// ⚠ AND THE MUTANT THAT DOES **NOT** REACH THIS TIER, ✔MEASURED P51 RATHER THAN
// ASSUMED — the first draft of this comment named it and was WRONG. Deleting
// `externSpecifierTokens` from `semantics.inline` flips `SymbolRecord::isInline`
// to TRUE for all three `extern` spellings (`SemanticAnalyzerC`'s rows redden),
// and NOTHING here moves: no row of this table changes, the corpus examples stay
// green, and a DSS-emitted `elf64-x86_64-linux` object still shows `T p` for
// `extern inline`. A definition whose `extern` is the DECLARATION RULE'S HEAD
// does not reach `lowerInlineDefinitionAsDeclaration` at all, so on that route
// the semantic bit is not what suppresses a body. Both facts are true and this
// tier is the one that decides the artifact — which is precisely why this table
// is not a restatement of the semantic truth table.
//   ~~ P53 CORRECTION, BY MEASUREMENT
// (D-C-EXTERN-MUST-LEAD-THE-DECLARATION-SPECIFIERS): the sentence above about
// "a definition whose `extern` is the DECLARATION RULE'S HEAD" describes a tree
// that no longer exists. At FILE scope `extern` is now an ordinary
// `singleDeclSpecifier` and every declaration routes through `topLevelDecl`, so
// these definitions DO reach `lowerInlineDefinitionAsDeclaration` — and reach it
// carrying the keyword, which is why `specifierPrefixHasInline`'s `extern` test
// (`semantics.inline.externSpecifierTokens`) is now load-bearing on this route
// rather than moot. That is the SECOND symptom this row named, closed BY the
// merge rather than beside it. The paragraph is kept because its P51 measurement
// was true of the P51 tree and the `externSpecifierTokens` mutant's reach is a
// fact worth not re-deriving; do not read it as describing the current one.
TEST(HirLoweringC, InlineBaselineSpellingsCarryTheReferenceLinkageState) {
    struct Row {
        char const*   spelling;
        std::size_t   externRows;   // an ExternFunction declaration was planted
        bool          marked;       // ...and the body carries the 6.7.4p7 mark
        SymbolBinding binding;
        char const*   referenceNm;
    };
    static constexpr Row kRows[] = {
        {"inline",          1u, true,  SymbolBinding::Global, "U p"},
        {"static inline",   0u, false, SymbolBinding::Local,  "t p"},
        {"extern inline",   0u, false, SymbolBinding::Global, "T p"},
        {"extern __inline", 0u, false, SymbolBinding::Global, "T p"},
        // ★★★ P53 (D-C-EXTERN-MUST-LEAD-THE-DECLARATION-SPECIFIERS): THE SAME
        // SPELLINGS WITH THE SPECIFIERS IN THE OTHER ORDER. Every one of these
        // was `error[P_NoAlternativeMatched]` before the file-scope declaration-
        // rule merge, because `extern` was the HEAD of its own rule — so the
        // table above could only ever measure half of C 6.7.1's unordered set.
        // They carry the SAME expectations as their mirrors, and that identity
        // is the assertion: `nm` on gcc 13.3.0 and clang 18.1.3 objects
        // (probed separately 2026-09-02, this exact shape) gives `T p` for BOTH
        // `extern inline` and `inline extern`, so the two orders do not merely
        // both compile — they MEAN the same external definition.
        {"inline extern",   0u, false, SymbolBinding::Global, "T p"},
        {"__inline extern", 0u, false, SymbolBinding::Global, "T p"},
        {"inline static",   0u, false, SymbolBinding::Local,  "t p"},
    };
    for (Row const& row : kRows) {
        SCOPED_TRACE(row.spelling);
        SemanticModel model = analyzeC(
            std::string(row.spelling) + " int p(int x) { return x + 1; }\n"
                                        "int main(void) { return p(41); }\n");
        ASSERT_FALSE(model.hasErrors()) << row.spelling;
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res != nullptr);
        EXPECT_TRUE(res->ok) << row.spelling;

        EXPECT_EQ(externRowsNamed(*res, "p"), row.externRows)
            << row.spelling << " — reference `nm` column " << row.referenceNm
            << "; an ExternFunction row is DSS's spelling of `U p`, and exactly "
               "one row must exist iff this TU provides no external definition";

        HirNodeId const f = functionNamed(res->hir, model, "p");
        ASSERT_TRUE(f.valid())
            << row.spelling << " — the body is lowered in EVERY row (the "
               "optimizer needs it even where it is never emitted); an absent "
               "Function node means it was dropped, not suppressed";
        bool const marked = res->inlineDefinitionMap.has(f)
                            && res->inlineDefinitionMap.get(f).isInlineDefinition;
        EXPECT_EQ(marked, row.marked)
            << row.spelling << " — the 6.7.4p7 mark is what makes the optimizer's "
               "strip epilogue drop the body; losing it on the first row emits a "
               "second external definition, and setting it on the others deletes "
               "a definition the program needs";

        EXPECT_EQ(declaredBinding(*res, model, "p"),
                  std::optional{row.binding})
            << row.spelling << " — reference `nm` column " << row.referenceNm;
    }
}

// ── D-C-SUBSCRIPT-OPERANDS-ARE-NOT-COMMUTATIVE (C 6.5.3.2p1) ────────────────
//
// `E1[E2]` is `*((E1)+(E2))` and the constraint is stated symmetrically, so the
// lowering must ASK which operand is the container rather than assume the base
// is. The corpus example `subscript_commuted_operands` pins the VALUES a running
// program reads; these pin the two REFUSALS, which a running program cannot
// witness, and the positive/negative pair in one place.
//
// ⚠ THE TWO REFUSALS ARE WHY A "just admit the reversed spelling" FIX WOULD BE
// WRONG: if the lowering merely stopped asking, `p[q]` and `a[b]` would go quiet
// too. Both are refused by gcc 13.3.0 and clang 18.1.3 at the user's own token
// ("array subscript is not an integer" / "subscripted value is neither array nor
// pointer nor vector") — ✔MEASURED 2026-09-02, each probed SEPARATELY.
TEST(HirLoweringC, CommutedSubscriptLowersCleanBothDirections) {
    SemanticModel model = analyzeC(
        "static int d[4] = {0,1,2,3};\n"
        "int main(void) { int *p = d; int i = 2; return i[p] + p[i]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res != nullptr);
    EXPECT_TRUE(res->ok)
        << "`i[p]` is `p[i]` (C 6.5.3.2p1) — the lowering must type both; "
        << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::S_SubscriptOperandsNotPointerAndInteger),
              0u)
        << "one pointer and one integer satisfies the constraint in EITHER order";
}
// RED-ON-DISABLE: delete the `Ambiguous` arm from `indexContainerOperand` and
// this reds — the law answers `Base`, the integer promotion is then handed a
// pointer, and the process ABORTS inside the type lattice instead of reporting
// (`TypeInterner::primitive: TypeKind Ptr is not a LEAF kind`, exit 0xC0000409,
// ✔MEASURED at P53's base through the shipped CLI).
TEST(HirLoweringC, SubscriptOfTwoPointersIsRefusedNotResolved) {
    SemanticModel model = analyzeC(
        "static int d[4];\n"
        "int main(void) { int *p = d; int *q = d; return p[q]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res != nullptr);
    EXPECT_FALSE(res->ok) << "`p[q]` satisfies 6.5.3.2p1 in NEITHER direction";
    EXPECT_EQ(countCode(r, DiagnosticCode::S_SubscriptOperandsNotPointerAndInteger),
              1u)
        << "exactly one refusal, naming the construct rather than a node ordinal";
}
TEST(HirLoweringC, SubscriptOfTwoIntegersIsRefusedNamingTheConstruct) {
    SemanticModel model = analyzeC(
        "int main(void) { int a = 1, b = 2; return a[b]; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res != nullptr);
    EXPECT_FALSE(res->ok) << "neither operand of `a[b]` can be the pointer";
    EXPECT_EQ(countCode(r, DiagnosticCode::S_SubscriptOperandsNotPointerAndInteger),
              1u);
    // ★ THE DIAGNOSTIC-QUALITY HALF THE ROW CARRIES. At P53's base this shape
    // reached the HIR verifier as `hir node #8 (HirKind ordinal 34)` — an
    // internal enumerator number shown to a user who wrote `a[b]`. The refusal
    // must name the CONSTRUCT and the standard clause, and no message anywhere
    // in the run may spell a raw kind ordinal.
    bool sawSubscriptText = false;
    for (auto const& d : r.all()) {
        EXPECT_EQ(d.actual.find("HirKind ordinal"), std::string::npos)
            << "a user-facing message must not name an internal kind ordinal: "
            << d.actual;
        if (d.code == DiagnosticCode::S_SubscriptOperandsNotPointerAndInteger
            && d.actual.find("6.5.3.2p1") != std::string::npos
            && d.actual.find("subscript") != std::string::npos) {
            sawSubscriptText = true;
        }
    }
    EXPECT_TRUE(sawSubscriptText)
        << "the refusal must name the construct and the constraint it violates";
}

// The OTHER half of 6.5.3.2p1 — "the other shall have INTEGER type". Both
// shapes below ABORTED THE PROCESS at P53's base (exit 0xC0000409, no
// `error[…]` line at all), and a float index instead travelled to the ASSEMBLER
// and was refused there as a register-CLASS mismatch naming xmm3 — a machine
// fact standing in for a source constraint. gcc 13.3.0 and clang 18.1.3 refuse
// all three at the user's own token with "array subscript is not an integer"
// (✔MEASURED 2026-09-02, each probed SEPARATELY).
//
// ⚠ THE CONTROL IS THE POINT AND IT IS BELOW: the check judges only kinds the
// lattice can PROVE are not integers, so `char`, `_Bool`, `enum`, `long` and
// `unsigned char` indices must stay clean. An accept-list would have had to
// enumerate them and one omission REFUSES A CORRECT PROGRAM.
TEST(HirLoweringC, FloatSubscriptIsRefusedAtTheSourceTier) {
    SemanticModel model = analyzeC(
        "static int d[4];\n"
        "int main(void) { int *p = d; double x = 1.0; return p[x]; }\n");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res != nullptr);
    EXPECT_FALSE(res->ok) << "a double cannot be a subscript (C 6.5.3.2p1)";
    EXPECT_EQ(countCode(r, DiagnosticCode::S_SubscriptOperandsNotPointerAndInteger),
              1u);
}
TEST(HirLoweringC, StructSubscriptIsRefusedNotAborted) {
    SemanticModel model = analyzeC(
        "struct S { int a; };\nstatic int d[4];\n"
        "int main(void) { int *p = d; struct S s = {0}; return p[s]; }\n");
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res != nullptr);
    EXPECT_FALSE(res->ok) << "a struct cannot be a subscript (C 6.5.3.2p1)";
    EXPECT_EQ(countCode(r, DiagnosticCode::S_SubscriptOperandsNotPointerAndInteger),
              1u);
}
TEST(HirLoweringC, NarrowAndNominalIntegerSubscriptsStayClean) {
    // char / _Bool / enum / long / unsigned char, forward AND commuted. These
    // are the accept-list this check deliberately does NOT enumerate.
    SemanticModel model = analyzeC(
        "enum E { E_TWO = 2 };\nstatic int d[8];\n"
        "int main(void) {\n"
        "  int *p = d; char c = 1; _Bool b = 1; enum E e = E_TWO;\n"
        "  long l = 3; unsigned char u = 2;\n"
        "  return p[c] + c[p] + p[b] + b[p] + p[e] + e[p]\n"
        "       + p[l] + l[p] + p[u] + u[p];\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
              ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res != nullptr);
    EXPECT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::S_SubscriptOperandsNotPointerAndInteger),
              0u)
        << "every C integer type is a legal subscript in EITHER position";
}

// ═══ P53 — [[D-C-EXTERN-MUST-LEAD-THE-DECLARATION-SPECIFIERS]], the HIR half ══
//
// THE PROPERTY: after the file-scope declaration-rule merge, the CST->HIR
// EXTERN ROUTE is selected by the FOLDED SPECIFIER, not by which grammar rule
// matched. `externDecl` no longer appears in /shapes/topLevel at all, so a
// file-scope `extern` reaches `lowerExternDeclInto` only because
// `topLevelDecl`'s `linkageSpecifiers` maps the keyword to {nonDefining:true}
// and `lowerTopLevelInto` reads that fold.
//
// ★★ THIS IS THE TIER WHERE THE SILENT MISCOMPILE LIVED. With the grammar merge
// alone -- no `extern` entry on the map -- dsscp reported
// warning[H_UnknownLinkageSpecifier] and EXITED 0: the extern-ness was dropped,
// every extern OBJECT became a tentative definition and emitted its own
// storage. A SEMANTIC-tier test cannot see that (it merges either way with zero
// diagnostics); only the Global-vs-ExternGlobal split here can, exactly as
// TentativeDefAfterExternEmitsStorageNotImport records for its own axis.
//
// RED-ON-DISABLE (REMOVE direction): delete the `"extern"` entry from
// topLevelDecl's `linkageSpecifiers` in c.lang.json -> these arms go red (one
// Global, zero ExternGlobals, plus H_UnknownLinkageSpecifier) while
// BlockScopeExternRehomesToModuleDecls stays GREEN -- the control that says the
// mutant hit the FILE-scope specifier fact and not the extern machinery, since
// the block-scope rule still carries its own per-row nonDefiningDeclaration.

TEST(HirLoweringC, ExternSpecifierRoutesToTheImportLoweringInEveryOrder) {
    for (std::string_view const decl : {"extern int g;\n",
                                        "extern __attribute__((weak)) int g;\n",
                                        "extern int g, h;\n"}) {
        SemanticModel model = analyzeC(std::string{decl}
                                       + "int use(void){ return g; }\n");
        ASSERT_FALSE(model.hasErrors())
            << decl
            << (model.diagnostics().all().empty()
                    ? "" : model.diagnostics().all()[0].actual);
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        ASSERT_TRUE(res->ok) << decl << (r.all().empty() ? "" : r.all()[0].actual);
        std::size_t globals = 0, externGlobals = 0;
        for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
            if (res->hir.kind(d) == HirKind::Global)       ++globals;
            if (res->hir.kind(d) == HirKind::ExternGlobal) ++externGlobals;
        }
        EXPECT_EQ(globals, 0u)
            << decl << " -- an `extern` OBJECT must emit NO storage in this TU";
        EXPECT_GE(externGlobals, 1u)
            << decl << " -- it must emit an ExternGlobal IMPORT";
        EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
            << decl
            << " -- `extern` must RESOLVE in the linkage map. An unresolved one "
               "is a WARNING, so the extern-ness would be dropped at rc 0.";
    }
}

// A file-scope `extern` FUNCTION prototype must still lower to an
// ExternFunction import through the merged rule, and its per-declaration
// import-library override (D-CSUBSET-EXTERN-LIBRARY-SYNTAX) must still be
// decoded -- that trailing `stringLiteralExpr` slot moved from `externDecl`'s
// sequence into `topLevelDecl`'s, where a wrong index would shift the
// kindByChild discriminator and mis-lower every function definition in the TU.
TEST(HirLoweringC, MergedRuleKeepsExternFunctionImportAndLibraryOverride) {
    SemanticModel model = analyzeC(
        "extern void* GetStdHandle(int) \"kernel32.dll\";\n"
        "int use(void){ return GetStdHandle(0) != 0; }\n"
        "int def(int x){ return x + 1; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    std::size_t externFns = 0, fns = 0;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root())) {
        if (res->hir.kind(d) == HirKind::ExternFunction) ++externFns;
        if (res->hir.kind(d) == HirKind::Function)       ++fns;
    }
    EXPECT_EQ(externFns, 1u)
        << "the extern prototype must lower to ONE ExternFunction import";
    EXPECT_EQ(fns, 2u)
        << "the two ordinary definitions must still lower as Functions -- a "
           "shifted kindByChild index would turn a definition into a "
           "declaration silently";
    bool sawLibrary = false;
    for (auto const& row : res->externDecls) {
        for (auto const& [format, lib] : row.libraryOverride) {
            (void)format;
            if (lib == "kernel32.dll") sawLibrary = true;
        }
    }
    EXPECT_TRUE(sawLibrary)
        << "the trailing per-declaration library override must still be decoded "
           "after the slot moved into topLevelDecl's sequence";
}

// ⓘ C99 6.7.4p7 THROUGH THE REVERSED ORDER — the row's SECOND symptom — is
// pinned by EXTENDING `InlineBaselineSpellingsCarryTheReferenceLinkageState`'s
// table with the `inline extern` / `__inline extern` / `inline static` rows,
// NOT by a second test here. That table already asserts the triple
// (externRows, 6.7.4p7 mark, binding) against measured `nm` ground truth, and a
// function-COUNT assertion beside it would have been a weaker instrument that
// disagreed with it: the body is lowered in EVERY row (the optimizer needs it),
// so counting Functions cannot tell a suppressed definition from an emitted one.
// That is not a hypothetical — it was written that way first and went red
// against its own subject.

// ★★★ [[D-CSUBSET-ATTRIBUTE-BEFORE-EXTERN-KEYWORD]] — CLOSED BY THIS MERGE, AND
// ITS OWN CLOSING CELL DEMANDS THE EFFECT, NOT THE PARSE ("both spellings compile
// clean AND the attribute's EFFECT is asserted — a binding read back, or an
// address — never that the line parses").
//
// That row prescribed escape (ii), the shared-prefix factoring, as its closing
// work and rejected escape (i) outright. This is escape (ii): with ONE top-level
// declaration rule there is no second FIRST set for `AttributeKeyword` to
// collide with, so the `C_AmbiguousAlternatives`-at-LOAD wall it recorded has
// nothing left to detect. ✔MEASURED 2026-09-02, gcc 13.3.0 `-std=c2x` and clang
// 18.1.3 `-std=c23` probed SEPARATELY: both ACCEPT
// `__attribute__((weak)) extern int g;` and `__attribute__((weak)) extern int
// f(int);` at rc 0, so the previous DSS refusal was BELOW the reference union.
//
// The BINDING is what is asserted here — read back off the HIR linkage map for
// the emitted import — because a leading attribute that parses and silently
// drops its binding is the exact failure TF-C73 measured for the trailing
// position, and it is invisible until link time.
TEST(HirLoweringC, AttributeBeforeExternKeywordBindsItsAttribute) {
    SemanticModel model = analyzeC(
        "__attribute__((weak)) extern int wf(int);\n"
        "__attribute__((visibility(\"hidden\"))) extern int hg;\n"
        "int use(void){ return wf(1) + hg; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    EXPECT_EQ(countCode(r, DiagnosticCode::H_UnknownLinkageSpecifier), 0u)
        << "a leading attribute must RESOLVE, not fall through the unknown gate";
    EXPECT_EQ(declaredBinding(*res, model, "wf"),
              std::optional{SymbolBinding::Weak})
        << "an attribute BEFORE the storage-class keyword must bind, not merely "
           "parse — a silently dropped `weak` is wrong linkage at link time with "
           "no diagnostic anywhere";
    auto const hl = declaredLinkage(*res, model, "hg");
    ASSERT_TRUE(hl.has_value());
    EXPECT_EQ(hl->visibility, SymbolVisibility::Hidden);
}

// An `extern` declaration that declares ONLY A TAG (`extern struct S { int a; };`)
// must take the TypeDecl path, not the import path: gcc accepts it with
// "useless storage class specifier in empty declaration". The route test is
// ordered AFTER the no-named-declarator arm precisely so this shape never
// reaches lowerExternDeclInto, and this pin is what says so.
TEST(HirLoweringC, ExternOnATagOnlyDeclarationEmitsNoImport) {
    SemanticModel model = analyzeC(
        "extern struct S { int a; };\n"
        "int use(void){ struct S s = {1}; return s.a; }\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty()
                ? "" : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    std::size_t externGlobals = 0;
    for (HirNodeId d : res->hir.moduleDecls(res->hir.root()))
        if (res->hir.kind(d) == HirKind::ExternGlobal) ++externGlobals;
    EXPECT_EQ(externGlobals, 0u)
        << "a tag-only declaration declares no object, so there is nothing to "
           "import";
}

// ── [[D-CSUBSET-VLA-SIZEOF-TYPEFORM]] part (1): `sizeof(int[n])` ────────────────
//
// C 6.5.3.4p2: *"If the type of the operand is a variable length array type, the
// operand is evaluated"* — the ONE `sizeof` whose operand is evaluated, so the
// bound is RE-READ at every `sizeof` and its side effects HAPPEN.
// ✔MEASURED 2026-09-04, references probed SEPARATELY: gcc 13.3.0 and clang
// 18.1.3 both compile and RUN it at `-std=c17` AND `-std=c2x`, both give two
// DIFFERENT answers when `n` changes between two `sizeof(int[n])`, and both
// execute a side effect in the bound; MSVC 19.44.35228 ABSTAINS (`C2057` + `C4034`).
//
// These two pins name the TIER and the MECHANISM. The corpus witness
// (`examples/c/c99_vla_sizeof_type_name`) proves the VALUES end to end; what
// cannot be seen from the values alone is that the lowering happens HERE, and
// why it must: a `vlaArray` TypeId carries NO length operand (every VLA of one
// element interns to a single TypeId), so a `SizeOf` node whose TypeRef is a VLA
// has already lost `n` before MIR — there would be nothing left downstream to
// re-evaluate. Pin 1 therefore asserts that NO VLA-typed `SizeOf` node is minted
// at all and that a Mul over a live `Ref` to the bound stands in its place.

// Pin 1 — THE SHAPE AND THE FRESHNESS. RED-ON-DISABLE: drop the
// `lowerVlaTypeNameSizeof` dispatch in `lowerSizeof` and the single SizeOf here
// carries the VLA array type again (the `isVlaArray` EXPECT flips) while the Mul
// and the Ref disappear entirely.
TEST(HirLoweringC, SizeofOfVlaTypeNameLowersToFreshBoundArithmeticNotAVlaSizeOf) {
    SemanticModel model = analyzeC(
        "int main(void) {\n"
        "  int n;\n"
        "  n = 3;\n"
        "  unsigned long a = sizeof(int[n]);\n"
        "  return (int)a;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
                                              : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    ASSERT_TRUE(fn.valid());
    auto const& ti = model.lattice().interner();

    std::vector<HirNodeId> sizeofs;
    std::vector<HirNodeId> muls;
    auto const collect = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::SizeOf) sizeofs.push_back(n);
        if (res->hir.kind(n) == HirKind::BinaryOp
            && isCoreOp(res->hir.payload(n))
            && decodeCoreOp(res->hir.payload(n)) == HirOpKind::Mul)
            muls.push_back(n);
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    collect(collect, res->hir.functionBody(fn));

    // (1) EXACTLY ONE SizeOf, and it sizes the ELEMENT — never the VLA type. A
    // VLA-typed SizeOf is precisely what HIR→MIR refuses (H_UnsupportedLowering-
    // ForKind, "sizeof of an incomplete or un-sizeable type"), so its ABSENCE is
    // the property, not an implementation detail.
    ASSERT_EQ(sizeofs.size(), 1u)
        << "the VLA type-name lowers to ONE static SizeOf (the element's), never "
           "a SizeOf of the array itself";
    auto const skids = res->hir.children(sizeofs[0]);
    ASSERT_EQ(skids.size(), 1u) << "SizeOf carries exactly [TypeRef]";
    TypeId const sizedT = res->hir.typeId(skids.front());
    ASSERT_TRUE(sizedT.valid());
    EXPECT_FALSE(ti.isVlaArray(sizedT))
        << "no SizeOf node may carry a variable-length array type — its TypeId "
           "has no length operand, so MIR could not size it and refuses";
    EXPECT_FALSE(ti.typeContainsVla(sizedT));
    EXPECT_EQ(ti.kind(sizedT), TypeKind::I32)
        << "`int[n]`'s element is `int`, and its size is the static half of the "
           "product";

    // (2) A Mul typed size_t stands where the VLA SizeOf used to be.
    ASSERT_GE(muls.size(), 1u)
        << "`sizeof(int[n])` is `n * sizeof(int)` — a runtime Mul, not a fold";
    HirNodeId chosen{};
    for (HirNodeId m : muls)
        if (res->hir.typeId(m).valid()
            && ti.kind(res->hir.typeId(m)) == TypeKind::U64)
            chosen = m;
    ASSERT_TRUE(chosen.valid()) << "the product is typed size_t (C 6.5.3.4p5)";

    // (3) FRESHNESS AT THIS TIER: the product's non-SizeOf operand subtree must
    // reach a live `Ref` to the bound. A constant-folded implementation would put
    // a Literal there and would pass every value test that only ever reads one
    // `n` — which is why the Ref, not the number, is what this pins.
    auto const kids = res->hir.children(chosen);
    ASSERT_EQ(kids.size(), 2u);
    HirNodeId const boundSide =
        (res->hir.kind(kids[1]) == HirKind::SizeOf) ? kids[0] : kids[1];
    bool sawRef = false;
    auto const hunt = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::Ref) sawRef = true;
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    hunt(hunt, boundSide);
    EXPECT_TRUE(sawRef)
        << "the bound must be lowered as a live read of `n` (C 6.5.3.4p2 makes "
           "the operand EVALUATED), never folded to a constant";
}

// Pin 2 — SUFFIX↔LEVEL PAIRING, the half a single-dimension test cannot see.
// `typedef int Row[3]; sizeof(Row[n])` has ONE array suffix but TWO array levels,
// and its value is `n * sizeof(int[3])` (✔MEASURED: gcc 13.3.0 and clang 18.1.3
// both exit 42 on the 60-byte witness). An implementation that descended to the
// FIRST NON-ARRAY element would multiply by `sizeof(int)` and under-report by 3×
// — a plausible wrong number, never a refusal, which is the failure direction
// this project ranks worst. RED-ON-DISABLE: change the element walk to descend
// past every Array level and this EXPECT flips Array→I32.
TEST(HirLoweringC, SizeofOfVlaTypeNamePairsOneSuffixPerArrayLevelNotToTheLeaf) {
    SemanticModel model = analyzeC(
        "typedef int Row[3];\n"
        "int main(void) {\n"
        "  int n;\n"
        "  n = 5;\n"
        "  unsigned long a = sizeof(Row[n]);\n"
        "  return (int)a;\n"
        "}\n");
    ASSERT_FALSE(model.hasErrors())
        << (model.diagnostics().all().empty() ? std::string{}
                                              : model.diagnostics().all()[0].actual);
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << (r.all().empty() ? "" : r.all()[0].actual);
    HirNodeId const fn = firstFunction(res->hir);
    ASSERT_TRUE(fn.valid());
    auto const& ti = model.lattice().interner();

    std::vector<HirNodeId> sizeofs;
    auto const collect = [&](auto&& self, HirNodeId n) -> void {
        if (!n.valid()) return;
        if (res->hir.kind(n) == HirKind::SizeOf) sizeofs.push_back(n);
        for (HirNodeId c : res->hir.children(n)) self(self, c);
    };
    collect(collect, res->hir.functionBody(fn));
    ASSERT_EQ(sizeofs.size(), 1u);
    auto const skids = res->hir.children(sizeofs[0]);
    ASSERT_EQ(skids.size(), 1u);
    TypeId const sizedT = res->hir.typeId(skids.front());
    ASSERT_TRUE(sizedT.valid());
    ASSERT_EQ(ti.kind(sizedT), TypeKind::Array)
        << "ONE suffix consumes ONE array level, so `Row[n]`'s element is the "
           "whole `int[3]` — descending to the leaf would size `int` and "
           "under-report the answer by a factor of 3";
    EXPECT_FALSE(ti.isVlaArray(sizedT))
        << "…and that element is the FIXED `int[3]`, statically sizeable";
}

// Pin 3 — ★★ THE FAIL-LOUD ARM, AND THE REASON IT EXISTS IS A DEFECT THIS CHANGE
// INTRODUCED AND THEN REMOVED. C 6.7.6.2p1 requires a variable-length array's size
// expression to have INTEGER type. `validateVlaDeclarator` enforces that
// (`S_VlaSizeNotInteger`) for a DECLARATOR, but an ABSTRACT type-name has no
// declarator and never reaches that validator — an omission that was invisible
// while the whole construct was refused at MIR.
// ✔MEASURED 2026-09-04 on the first working build of part (1): with the type-name
// form lowered but WITHOUT this guard, `double x; sizeof(int[x])` compiled rc=0
// and answered 12 (the widening Cast silently truncated 3.5) and
// `int *p; sizeof(int[p])` compiled and answered from the pointer's numeric value
// — two SILENT WRONG ANSWERS where the base (b1f31420) had refused loudly.
// ✔gcc 13.3.0 and clang 18.1.3, probed SEPARATELY, both REFUSE both shapes
// ("size of array has non-integer type"). So this is not a defensive extra: it is
// the difference between the feature being shippable and being a miscompile.
// RED-ON-DISABLE: delete the integer arm in `lowerVlaTypeNameSizeof` and both
// ASSERT_TRUEs below flip — the lowering succeeds and produces a number.
TEST(HirLoweringC, SizeofOfVlaTypeNameWithNonIntegerBoundFailsLoud) {
    auto const mustRefuse = [](char const* what, std::string src) {
        SemanticModel model = analyzeC(std::move(src));
        DiagnosticReporter r;
        auto res = lowerToHir(model, r);
        bool const refused = model.hasErrors() || !res->ok;
        EXPECT_TRUE(refused)
            << what
            << " must be REFUSED (C 6.7.6.2p1 — the size expression of a "
               "variable-length array must have integer type), never silently "
               "converted into a size; gcc and clang both refuse it";
    };
    mustRefuse("a floating bound",
               "int main(void) {\n"
               "  double x;\n"
               "  x = 3.5;\n"
               "  unsigned long s = sizeof(int[x]);\n"
               "  return (int)s;\n"
               "}\n");
    mustRefuse("a pointer bound",
               "int main(void) {\n"
               "  int v;\n"
               "  v = 3;\n"
               "  int *p = &v;\n"
               "  unsigned long s = sizeof(int[p]);\n"
               "  return (int)s;\n"
               "}\n");
    // CONTROL, and the reason the refusal above is not a blanket one: an
    // ENUM-typed variable IS an integer type (C 6.7.6.2p1) and must still be
    // ACCEPTED. ✔gcc 13.3.0 and clang 18.1.3 both compile and run it (20).
    SemanticModel ok = analyzeC(
        "enum E { A = 3, B = 5 };\n"
        "int main(void) {\n"
        "  enum E e;\n"
        "  e = B;\n"
        "  unsigned long s = sizeof(int[e]);\n"
        "  return (int)s;\n"
        "}\n");
    ASSERT_FALSE(ok.hasErrors())
        << (ok.diagnostics().all().empty() ? std::string{}
                                           : ok.diagnostics().all()[0].actual);
    DiagnosticReporter okr;
    auto okres = lowerToHir(ok, okr);
    EXPECT_TRUE(okres->ok)
        << "an enum-typed bound is an INTEGER bound and must not be caught by the "
           "non-integer refusal: "
        << (okr.all().empty() ? "" : okr.all()[0].actual);
}
