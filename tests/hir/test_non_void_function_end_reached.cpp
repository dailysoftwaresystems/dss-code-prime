// ── A VALUE-RETURNING FUNCTION WHOSE END IS REACHED ──────────────────────────
// (D-C-A-NON-VOID-FUNCTION-WHOSE-END-IS-REACHABLE-IS-REFUSED)
//
// C23 6.9.2p13: "Unless otherwise specified, if the } that terminates the
// function body is reached, and the value of the function call is used by the
// caller, the behavior is undefined." Reaching the brace is therefore DEFINED;
// only using the value nobody returned is not.
// DSS refused every such function (the HIR verifier's return-completeness rule);
// gcc, clang, Apple clang, mingw-w64 gcc and MSVC all compile it and return.
//
// The rule is LANGUAGE CONFIG — `semantics.declarations[].nonVoidFunctionEndReached`,
// `refused` (the default) or `returnsUnspecifiedValue` (what C states) — and the
// CST->HIR lowering reads it: under the second it COMPLETES the body with
//     T unspecified;  return unspecified;
// (a synthetic local nothing stores to) and reports the function ONCE, as a
// warning, at the body's last token.
//
// WHAT THIS FILE PINS, at the tier that decides it:
//   * the completion's shape, its result type for every result class a function
//     of the fixture's language can return here, and the ONE warning — its code,
//     severity, the function it names and where it points;
//   * NOTHING ELSE is reported: the synthetic local is no user object, so no
//     diagnostic of any kind may name it (the reporter holds exactly the one
//     warning — an assertion on the whole set, which a future "read of an
//     uninitialized object" advisory would have to answer too);
//   * `main` keeps its implicit `return 0` and is NOT reported — the list
//     `implicitReturnZeroForFunctionNames` is read first, and that order is the
//     rule;
//   * a body that returns on every path is neither completed nor reported;
//   * a language document that does NOT state the key, or states `refused`,
//     keeps the verifier's refusal — measured on the shipped C document with
//     that one key taken out, so the two arms differ in nothing else.
//
// The run-time half — the returning paths keep their values under the release
// pipeline, the caller goes on running, every result class through its own
// return convention — is `examples/c/non_void_function_end_reached`, in both
// example runners.
//
// ⚠ MUST run through ctest: the shipped documents are found through
// `DSS_CONFIG_ROOT`, which only `dss_add_test` sets.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "repo_root.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

constexpr DiagnosticCode kReached = DiagnosticCode::H_NonVoidFunctionEndReachable;
constexpr DiagnosticCode kRefused = DiagnosticCode::H_VerifierFailure;

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == c) ++n;
    return n;
}

[[nodiscard]] std::string dump(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all()) {
        out += "\n    ";
        out += diagnosticCodeName(d.code);
        out += ": ";
        out += d.actual;
    }
    return out.empty() ? std::string{" <no diagnostics>"} : out;
}

// The shipped C document with the rule's key REWRITTEN on the one row that
// states it: erased when `spelling` is empty, set to `spelling` otherwise.
//
// ★ THE ROW IS FOUND BY SEARCH AND THE KEY MUST BE THERE. An index would rot into
// another row, and a fixture that "removed" a key the document never stated
// would hand back the shipped rule under a name that says otherwise — every
// refusal arm below would then be measuring the relaxed rule. Both are thrown,
// not skipped.
[[nodiscard]] std::shared_ptr<GrammarSchema const>
cDocumentWithTheRule(std::optional<std::string_view> spelling) {
    std::filesystem::path const path =
        dss::test::configRoot() / "sources" / "c.lang.json";
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        throw std::runtime_error("cannot open the shipped C document: " + path.string());
    }
    nlohmann::json doc = nlohmann::json::parse(in);
    std::size_t rewritten = 0;
    for (auto& row : doc.at("semantics").at("declarations")) {
        if (!row.contains("nonVoidFunctionEndReached")) continue;
        ++rewritten;
        if (spelling.has_value()) {
            row["nonVoidFunctionEndReached"] = std::string{*spelling};
        } else {
            row.erase("nonVoidFunctionEndReached");
        }
    }
    if (rewritten != 1) {
        throw std::runtime_error(
            "expected exactly one declaration row of the shipped C document to "
            "state nonVoidFunctionEndReached, found "
            + std::to_string(rewritten));
    }
    auto loaded = GrammarSchema::loadFromText(doc.dump(), "<c-end-rule-rewritten>");
    if (!loaded) {
        std::string message = "the rewritten C document did not load";
        for (auto const& d : loaded.error()) {
            message += "\n    ";
            message += d.path;
            message += ": ";
            message += d.message;
        }
        throw std::runtime_error(std::move(message));
    }
    return *loaded;
}

// One translation unit taken from source to verified HIR under `schema`. Owns
// the model, because the HIR's TypeIds and the function names both live in it.
struct Lowered {
    explicit Lowered(SemanticModel m) : model(std::move(m)) {}

    SemanticModel                   model;
    DiagnosticReporter              reporter;
    std::unique_ptr<CstToHirResult> result;

    [[nodiscard]] Hir const& hir() const { return result->hir; }

    // The function declared under `name`; an invalid id when there is none.
    [[nodiscard]] HirNodeId function(std::string_view name) const {
        for (HirNodeId d : hir().moduleDecls(hir().root())) {
            if (hir().kind(d) != HirKind::Function) continue;
            auto const* rec = model.recordFor(hir().functionSymbol(d));
            if (rec != nullptr && rec->name == name) return d;
        }
        return HirNodeId{};
    }
};

// What the analysis is told about the machine: the data model, and the layout
// parameters every compile of a real target hands `analyze()`.
//
// ★ THE DEFAULT CARRIES A LAYOUT, because the compiler always does: a fixture that
// analyzed with none would be measuring a configuration only the LSP and the FFI
// header parser run in, and in it `sizeof` is not a constant at the lowering tier
// (`NoLayoutMeansALayoutQuestionIsNotAConstant` pins exactly that, on purpose).
struct Machine {
    DataModel                            dataModel = DataModel::Lp64;
    std::optional<AggregateLayoutParams> layout =
        AggregateLayoutParams{ScalarAlignmentRule::Natural, 16};
};
const Machine kLp64{};
const Machine kLlp64{DataModel::Llp64,
                     AggregateLayoutParams{ScalarAlignmentRule::Natural, 16}};
const Machine kNoLayout{DataModel::Lp64, std::nullopt};

[[nodiscard]] std::unique_ptr<Lowered>
lowerUnder(std::shared_ptr<GrammarSchema const> schema, std::string src,
           Machine const& machine = kLp64) {
    UnitBuilder builder{std::move(schema), DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    auto out = std::make_unique<Lowered>(analyze(cu, DiagnosticBudget::libraryDefault(),
                                                 machine.dataModel, machine.layout));
    EXPECT_FALSE(out->model.hasErrors())
        << "the front end must be clean, or the lowering verdict below is about "
           "something else:" << dump(out->model.diagnostics());
    out->result = lowerToHir(out->model, out->reporter);
    return out;
}

[[nodiscard]] std::unique_ptr<Lowered> lowerC(std::string src, Machine const& machine = kLp64) {
    return lowerUnder(dss::test_support::shippedSchemaOrThrow("c"), std::move(src), machine);
}

// The offset of the LAST character of `fragment`'s first occurrence, in the text
// the unit's tree was parsed from — the buffer a diagnostic's span indexes.
//
// ★ NOT an offset into the string the test handed in. ✔MEASURED (the first run of
// this file, macos-arm64-debug): an in-memory unit's tree text is that string
// behind a prefix the front end composes (420 bytes there), so `src.find('}')`
// named a position 420 short of the span and both position pins failed for a
// reason that was the fixture's. Searching the tree's own text needs no such
// constant.
[[nodiscard]] std::size_t lastCharOf(Lowered const& l, std::string_view fragment) {
    for (auto const& tree : l.model.unit().trees()) {
        std::string_view const text = tree.source().text();
        std::size_t const at = text.find(fragment);
        if (at == std::string_view::npos) continue;
        EXPECT_EQ(text.find(fragment, at + 1), std::string_view::npos)
            << "`" << fragment << "` must occur once, or the position asked for is ambiguous";
        return at + fragment.size() - 1;
    }
    ADD_FAILURE() << "no tree of the unit holds `" << fragment << "`";
    return std::string_view::npos;
}

// The completed body's three children, checked: `{ <body>  T t;  return t; }`.
// Returns the synthetic local's declared type, or an invalid id when the shape
// is not the completion's.
[[nodiscard]] TypeId completionLocalType(Lowered const& l, HirNodeId fn) {
    Hir const& hir = l.hir();
    HirNodeId const outer = hir.functionBody(fn);
    if (hir.kind(outer) != HirKind::Block) return InvalidType;
    if (!has(hir.flags(outer), HirFlags::Synthetic)) return InvalidType;
    auto const kids = hir.children(outer);
    if (kids.size() != 3) return InvalidType;
    if (hir.kind(kids[0]) != HirKind::Block) return InvalidType;
    HirNodeId const local = kids[1];
    HirNodeId const ret   = kids[2];
    if (hir.kind(local) != HirKind::VarDecl) return InvalidType;
    if (!has(hir.flags(local), HirFlags::Synthetic)) return InvalidType;
    if (hir.varDeclInit(local).has_value()) return InvalidType;   // nothing stores to it
    if (hir.kind(ret) != HirKind::ReturnStmt) return InvalidType;
    if (!has(hir.flags(ret), HirFlags::Synthetic)) return InvalidType;
    auto const value = hir.returnValue(ret);
    if (!value.has_value() || hir.kind(*value) != HirKind::Ref) return InvalidType;
    if (hir.payload(*value) != hir.varDeclSymbol(local).v) return InvalidType;
    if (hir.typeId(*value) != hir.varDeclType(local)) return InvalidType;
    return hir.varDeclType(local);
}

}  // namespace

// The shape, the one warning, and nothing else reported.
//
// RED-ON-DISABLE: make `maybeCompleteReachableNonVoidEnd` return `body` at once
// and `f` is refused by the verifier (`ok` false, H_VerifierFailure, no warning).
TEST(NonVoidFunctionEndReached, TheBodyIsCompletedAndTheFunctionReportedOnce) {
    std::string const src =
        "int f(int x) { if (x) return 5; }\n"
        "int main(void) { return 0; }\n";
    auto const l = lowerC(src);
    ASSERT_TRUE(l->result->ok) << dump(l->reporter);

    // Exactly ONE diagnostic of ANY code: the warning. The synthetic local is
    // not the user's object and nothing may be said about it.
    ASSERT_EQ(l->reporter.all().size(), 1u) << dump(l->reporter);
    auto const& d = l->reporter.all()[0];
    EXPECT_EQ(d.code, kReached);
    EXPECT_EQ(d.severity, DiagnosticSeverity::Warning);
    EXPECT_NE(d.actual.find("'f'"), std::string::npos)
        << "the warning names the function: " << d.actual;
    // It points at the body's last token — `f`'s closing brace.
    EXPECT_EQ(d.span.start(), lastCharOf(*l, "int f(int x) { if (x) return 5; }"))
        << "the warning belongs at the closing brace the control reaches";

    HirNodeId const f = l->function("f");
    ASSERT_TRUE(f.valid());
    TypeId const local = completionLocalType(*l, f);
    ASSERT_TRUE(local.valid())
        << "f's body must be { <body>  T t;  return t; } with t synthetic, never "
           "stored to, and returned by a read of its own symbol";
    auto const& interner = l->model.lattice().interner();
    EXPECT_EQ(local, interner.fnResult(l->hir().functionSignature(f)))
        << "the local has the function's own result type";

    // `main` returned on every path: untouched.
    HirNodeId const m = l->function("main");
    ASSERT_TRUE(m.valid());
    EXPECT_FALSE(has(l->hir().flags(l->hir().functionBody(m)), HirFlags::Synthetic));
}

// Every result class gets a local of ITS OWN type — no zero form, no per-class
// arm. The classes are the ones a function can return under this fixture (no
// target in scope, so the long-double axis is left to the runnable example).
TEST(NonVoidFunctionEndReached, EveryResultClassIsCompletedWithALocalOfItsOwnType) {
    constexpr std::string_view kPrelude =
        "struct wide { char bytes[64]; };\n"
        "struct pair { long long a; long long b; };\n"
        "union either { int i; double d; };\n"
        "enum colour { RED, GREEN };\n";
    for (std::string_view const type :
         {"int", "char", "short", "long long", "unsigned long long", "_Bool",
          "float", "double", "void *", "const char *", "enum colour",
          "struct wide", "struct pair", "union either", "double _Complex",
          "__int128"}) {
        std::string src{kPrelude};
        src += "static ";
        src += type;
        src += " held;\n";
        src += type;
        src += " f(int x) { if (x) return held; }\n";
        src += type;
        src += " g(void) { }\n";
        src += "int main(void) { return 0; }\n";
        auto const l = lowerC(src);
        ASSERT_TRUE(l->result->ok) << type << ":" << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kReached), 2u) << type << dump(l->reporter);
        EXPECT_EQ(l->reporter.all().size(), 2u)
            << type << ": nothing but the two warnings" << dump(l->reporter);
        auto const& interner = l->model.lattice().interner();
        for (std::string_view const name : {"f", "g"}) {
            HirNodeId const fn = l->function(name);
            ASSERT_TRUE(fn.valid()) << type << " " << name;
            TypeId const local = completionLocalType(*l, fn);
            ASSERT_TRUE(local.valid()) << type << " " << name;
            EXPECT_EQ(local, interner.fnResult(l->hir().functionSignature(fn)))
                << type << " " << name;
        }
    }
}

// ONE warning per FUNCTION, however many of its paths reach the end; each at its
// own function's closing brace, each naming its own function.
TEST(NonVoidFunctionEndReached, OneWarningPerFunctionAtItsOwnClosingBrace) {
    std::string const src =
        "int many(int x) { if (x == 1) return 1; if (x == 2) { } else if (x == 3) { x = 0; } }\n"
        "int none(void) { }\n"
        "int main(void) { return 0; }\n";
    auto const l = lowerC(src);
    ASSERT_TRUE(l->result->ok) << dump(l->reporter);
    ASSERT_EQ(l->reporter.all().size(), 2u) << dump(l->reporter);

    std::size_t const manyEnd = lastCharOf(*l, "{ x = 0; } }");   // `many`'s last brace
    std::size_t const noneEnd = lastCharOf(*l, "int none(void) { }");
    ASSERT_NE(manyEnd, std::string_view::npos);
    ASSERT_NE(noneEnd, std::string_view::npos);
    bool sawMany = false;
    bool sawNone = false;
    for (auto const& d : l->reporter.all()) {
        EXPECT_EQ(d.code, kReached);
        EXPECT_EQ(d.severity, DiagnosticSeverity::Warning);
        if (d.actual.find("'many'") != std::string::npos) {
            sawMany = true;
            EXPECT_EQ(d.span.start(), manyEnd) << d.actual;
        } else if (d.actual.find("'none'") != std::string::npos) {
            sawNone = true;
            EXPECT_EQ(d.span.start(), noneEnd) << d.actual;
        }
    }
    EXPECT_TRUE(sawMany) << dump(l->reporter);
    EXPECT_TRUE(sawNone) << dump(l->reporter);
}

// `main` is answered by `implicitReturnZeroForFunctionNames`, which is read
// FIRST: it gets `return 0`, not the unspecified value, and it is not reported.
//
// RED-ON-DISABLE: call `maybeCompleteReachableNonVoidEnd` BEFORE
// `maybeAppendImplicitReturnZero` and `main` is completed with the local and
// warned about — a program whose exit status C defines as 0 would then exit
// with whatever the result register held.
TEST(NonVoidFunctionEndReached, MainKeepsItsImplicitReturnZeroAndIsNotReported) {
    auto const l = lowerC(
        "int helper(void) { }\n"
        "int main(void) { int x; x = 1; }\n");
    ASSERT_TRUE(l->result->ok) << dump(l->reporter);
    ASSERT_EQ(l->reporter.all().size(), 1u) << dump(l->reporter);
    EXPECT_EQ(l->reporter.all()[0].code, kReached);
    EXPECT_NE(l->reporter.all()[0].actual.find("'helper'"), std::string::npos)
        << l->reporter.all()[0].actual;

    Hir const& hir = l->hir();
    HirNodeId const m = l->function("main");
    ASSERT_TRUE(m.valid());
    HirNodeId const outer = hir.functionBody(m);
    ASSERT_EQ(hir.kind(outer), HirKind::Block);
    auto const kids = hir.children(outer);
    ASSERT_EQ(kids.size(), 2u) << "main is { <body>  return 0; } — two children, not three";
    ASSERT_EQ(hir.kind(kids[1]), HirKind::ReturnStmt);
    auto const value = hir.returnValue(kids[1]);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(hir.kind(*value), HirKind::Literal) << "main returns the literal zero";
    EXPECT_FALSE(completionLocalType(*l, m).valid());
}

// A body that returns (or cannot continue) on every path is neither completed
// nor reported. These are the shapes the structural predicate answers "the end
// is not reached" for; each would be a false warning otherwise.
TEST(NonVoidFunctionEndReached, ABodyThatLeavesOnEveryPathIsNeitherCompletedNorReported) {
    for (std::string_view const body :
         {"{ if (x) return 1; else return 2; }",
          "{ while (1) { if (x) return x; } }",
          "{ for (;;) { return x; } }",
          "{ switch (x) { case 0: return 1; default: return 2; } }",
          "{ if (x) goto done; return 1; done: return 2; }",
          "{ if (x) return 1; die(); }",
          "{ return x; ; }"}) {
        std::string src = "_Noreturn void die(void);\nint f(int x) ";
        src += body;
        src += "\nvoid v(int x) { if (x) return; }\nint main(void) { return 0; }\n";
        auto const l = lowerC(src);
        ASSERT_TRUE(l->result->ok) << body << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kReached), 0u) << body << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kRefused), 0u) << body << dump(l->reporter);
        HirNodeId const f = l->function("f");
        ASSERT_TRUE(f.valid()) << body;
        EXPECT_FALSE(completionLocalType(*l, f).valid()) << body;
    }
}

namespace {
// True iff `id`'s subtree holds a Synthetic `Unreachable` — the lowering's mark
// for "control never continues past this statement". Heap, not host frames.
[[nodiscard]] bool holdsSyntheticUnreachable(Hir const& hir, HirNodeId root) {
    std::vector<HirNodeId> pending{root};
    while (!pending.empty()) {
        HirNodeId const id = pending.back();
        pending.pop_back();
        if (hir.kind(id) == HirKind::Unreachable && has(hir.flags(id), HirFlags::Synthetic))
            return true;
        for (HirNodeId c : hir.children(id)) pending.push_back(c);
    }
    return false;
}

[[nodiscard]] std::string functionWithBody(std::string_view body) {
    // `struct P`: b at 4, the anonymous member at 8, its `d` at 8 + 4 = 12.
    std::string src =
        "_Noreturn void die(void);\n"
        "struct P { char a; int b; struct { char c; int d; }; };\n"
        "int f(int x) ";
    src += body;
    src += "\nint main(void) { return 0; }\n";
    return src;
}
}  // namespace

// SHAPES WHOSE END NO EXECUTION REACHES, which a structural "does every path
// return" cannot see without a constant's value or a callee's `noreturn` — so
// the lowering, which has both, says it (a Synthetic Unreachable after the
// statement). ✔MEASURED 2026-10-08 (probe fo3): gcc 13.3.0 and clang 18.1.3 at
// -Wall and Apple clang 21.0.0 report NONE of these as reaching the end. Until
// P69 DSS refused the first, the third and the tenth (valid C); with the
// completion alone it would have warned on all of them.
//
// RED-ON-DISABLE, one per family: `doBodyNeverReachesItsCondition` answering
// false (the `do` rows), `ifNeverCompletes` answering false (the `if` rows),
// the chokepoint asking `isDirectNoreturnCall` again instead of
// `expressionNeverCompletes` (the conditional, comma and cast rows); and one per
// HOOK of the lowering tier's constant evaluator — `resolveSizeof`,
// `resolveAlignof`, `resolveFieldOffset`, `resolveFoldedConstant`,
// `resolveSelectedArm` — for the rows whose condition is a constant only through
// the machine's layout or through an answer the semantic tier recorded
// (✔MEASURED, probe fo5: gcc 13.3.0, clang 18.1.3, Apple clang 21.0.0, mingw-w64
// gcc and cl 14.51 report none of them).
TEST(NonVoidFunctionEndReached, ShapesNoExecutionLeavesAreNotReported) {
    for (std::string_view const body :
         {// a `do` whose body never reaches its condition
          "{ do { return x; } while (0); }",
          "{ do { return x; } while (x); }",
          "{ do { while (x) { break; } return x; } while (0); }",
          "{ do { switch (x) { case 1: break; } return x; } while (0); }",
          // an `if` whose condition is a constant and whose live arm terminates
          "{ if (1) return x; }",
          "{ if (0) x = 1; else return x; }",
          "{ if (1) return x; else x = 2; }",
          "{ if (sizeof(int) >= 1) return x; }",
          "{ enum { ON = 1 }; if (ON) return x; }",
          "{ if (1) { do { return x; } while (0); } }",
          "{ if (1) die(); }",
          // …a constant only through the machine's LAYOUT
          "{ if (sizeof x >= 1) return x; }",
          "{ if (sizeof(struct P) > sizeof(int)) return x; }",
          "{ if (sizeof(long) == 8 ? 1 : 1) return x; }",
          "{ if (_Alignof(double) >= 1) return x; }",
          "{ if ((unsigned long)&((struct P *)0)->b >= 1) return x; }",
          // …a member PROMOTED through an anonymous member, at the SUM of the
          // offsets crossed, and one behind a qualified pointee: the member rule
          // both tiers share (`anon_member_search::memberByteOffset`)
          "{ if ((unsigned long)&((struct P *)0)->d == 12) return x; }",
          "{ if ((unsigned long)&((volatile struct P *)0)->b == 4) return x; }",
          // …or through an answer the semantic tier RECORDED for the node
          "{ if (__builtin_offsetof(struct P, b) >= 1) return x; }",
          "{ if (__builtin_offsetof(struct P, d) == 12) return x; }",
          "{ if (__builtin_types_compatible_p(int, int)) return x; }",
          "{ if (_Generic(x, int: 1, default: 0)) return x; }",
          "{ if (__builtin_choose_expr(1, 1, 0)) return x; }",
          // a loop whose condition is such a constant never exits
          "{ while (sizeof(int)) { if (x) return x; } }",
          "{ for (; sizeof(int); ) { if (x) return x; } }",
          "{ do { if (x) return x; } while (_Alignof(int)); }",
          // an expression statement that cannot finish evaluating
          "{ x ? die() : die(); }",
          "{ x || 1 ? die() : die(); }",
          "{ x ? (x > 1 ? die() : die()) : die(); }",
          "{ (void)x, die(); }",
          "{ (void)x; (void)die(); }"}) {
        auto const l = lowerC(functionWithBody(body));
        ASSERT_TRUE(l->result->ok) << body << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kReached), 0u) << body << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kRefused), 0u) << body << dump(l->reporter);
        HirNodeId const f = l->function("f");
        ASSERT_TRUE(f.valid()) << body;
        EXPECT_FALSE(completionLocalType(*l, f).valid())
            << body << ": a body whose end is not reached is not completed";
        EXPECT_TRUE(holdsSyntheticUnreachable(l->hir(), l->hir().functionBody(f))) << body;
    }
}

// THE OTHER DIRECTION, AND THE ONE A WRONG RULE WOULD MISCOMPILE THROUGH: each of
// these bodies CAN be left by its end, so none may be marked "never continues".
// A wrap here would put an Unreachable where an execution arrives. ✔MEASURED
// (probe fo3): the first four are reported by gcc, clang and Apple clang.
TEST(NonVoidFunctionEndReached, ShapesAnExecutionDoesLeaveAreStillReported) {
    for (std::string_view const body :
         {// `break` and `continue` both leave a `do … while (0)`
          "{ do { if (x) break; return x; } while (0); }",
          "{ do { if (x) continue; return x; } while (0); }",
          // …and a `continue` is not captured by a nested switch
          "{ do { switch (x) { case 1: continue; } return x; } while (0); }",
          // one arm of the conditional returns
          "{ x ? die() : (void)0; }",
          // a breakable loop
          "{ while (1) { break; } }",
          // `if (0) S;` with no else completes at once
          "{ if (0) return x; }",
          // a label in the arm that is never TAKEN: a goto runs it to its end
          "{ if (x) goto in; if (1) return x; else { in: x = 3; } }",
          "{ if (x) goto in; if (0) { in: x = 3; } else return x; }",
          // a case reached by the switch's dispatch after a returning default
          "{ switch (x) { default: return 5; case 1: x = 2; } }",
          // a condition that is not a constant
          "{ if (x) return x; }",
          // `sizeof` of a VARIABLY MODIFIED operand is evaluated (C 6.5.3.4p2): a
          // run-time value, so neither the `if` nor the loop is decided before
          // the program runs. ✔MEASURED (probe fo5): gcc, clang, Apple clang and
          // mingw-w64 gcc report all three as reaching the end.
          "{ if (sizeof(int[x]) > 8) return x; }",
          "{ int v[x]; if (sizeof v > 8) return x; }",
          "{ while (sizeof(int[x]) > 8) { x--; } }",
          // a recorded selection whose WINNER is the constant zero: `if (0) S;`
          "{ if (_Generic(x, long: 1, default: 0)) return x; }",
          "{ if (__builtin_choose_expr(0, 1, 0)) return x; }",
          // a promoted member's offset is the SUM along the anonymous members, not
          // its index inside the innermost one (4): the condition is false
          "{ if ((unsigned long)&((struct P *)0)->d == 4) return x; }",
          "{ if (__builtin_offsetof(struct P, d) != 12) return x; }"}) {
        auto const l = lowerC(functionWithBody(body));
        ASSERT_TRUE(l->result->ok) << body << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kReached), 1u) << body << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kRefused), 0u) << body << dump(l->reporter);
        HirNodeId const f = l->function("f");
        ASSERT_TRUE(f.valid()) << body;
        EXPECT_TRUE(completionLocalType(*l, f).valid())
            << body << ": an end an execution reaches must be completed";
    }
}

// A WRAP IS A CLAIM THE BACK END MAY ACT ON, so a wrong one is a miscompile, not
// a wrong warning. These bodies all RETURN on every path (no warning either way)
// — what is pinned is the claim itself, by where the Synthetic Unreachable is and
// is not. Each also RUNS, debug and release, in
// `examples/c/non_void_function_end_not_reached`.
TEST(NonVoidFunctionEndReached, TheNeverContinuesClaimIsMadeOnlyWhereItHolds) {
    struct Case {
        char const*      what;
        std::string_view body;
        bool             claimed;   // a Synthetic Unreachable somewhere in `f`
    };
    for (Case const& c : {
             // (i) the Duff's-device shape: a `case` of the ENCLOSING switch sits
             // in the arm the constant `if` never takes. The switch jumps into
             // it, the arm runs to its end, and `x += 1` IS the position after
             // the `if` — which must not be claimed unreachable.
             Case{"a case label of the enclosing switch inside the dead arm",
                  "{ switch (x) { case 0: if (0) { case 1: x = 3; } else return 50;"
                  " x += 1; break; default: return 60; } return x; }",
                  false},
             Case{"…and inside the dead ELSE arm",
                  "{ switch (x) { case 0: if (1) return 50; else { case 1: x = 3; }"
                  " x += 1; break; default: return 60; } return x; }",
                  false},
             Case{"a default label of the enclosing switch inside the dead arm",
                  "{ switch (x) { case 0: if (0) { default: x = 3; } else return 50;"
                  " x += 1; break; } return x; }",
                  false},
             // (ii) the only exit is a `goto` to a label LATER in the function.
             // The `do` never reaches its condition and the `if`'s live arm
             // terminates, so both ARE claimed — and the claim is about the
             // position after the statement, never about the label, whose code
             // runs (the example checks it does).
             Case{"a do body leaving only through a goto to a later label",
                  "{ do { if (x) goto out; return 1; } while (0); return 2; out: return 3; }",
                  true},
             Case{"a live arm leaving only through a goto to a later label",
                  "{ if (1) goto out; return 2; out: return 3; }",
                  true},
             // (iii) a callee that is not a direct reference to a function that
             // does not return: a conditional callee, a pointer object.
             Case{"a conditional callee, one of whose operands returns",
                  "{ (x ? die : live)(); return 5; }",
                  false},
             Case{"a call through a pointer object that was not declared noreturn",
                  "{ void (*p)(void) = live; p(); return 5; }",
                  false},
             Case{"a conditional with one arm that returns",
                  "{ x ? die() : live(); return 5; }",
                  false}}) {
        std::string src = "_Noreturn void die(void);\nvoid live(void);\nint f(int x) ";
        src += c.body;
        src += "\nint main(void) { return 0; }\n";
        auto const l = lowerC(src);
        ASSERT_TRUE(l->result->ok) << c.what << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kReached), 0u) << c.what << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kRefused), 0u) << c.what << dump(l->reporter);
        HirNodeId const f = l->function("f");
        ASSERT_TRUE(f.valid()) << c.what;
        EXPECT_EQ(holdsSyntheticUnreachable(l->hir(), l->hir().functionBody(f)), c.claimed)
            << c.what;
    }
}

// (iii), the declaration half: `noreturn` on ONE declaration among several. A
// function declared `_Noreturn` on any declaration of the translation unit shall
// not return (C23 6.7.5p8), and the record is merged over the unit's declarations
// before any body is lowered — so the first declaration alone, and a later
// redeclaration alone, both make a direct call a terminator.
TEST(NonVoidFunctionEndReached, NoreturnOnOneDeclarationAmongSeveralStillTerminates) {
    for (std::string_view const decls :
         {"_Noreturn void end(void); void end(void);",
          "void end(void); _Noreturn void end(void);",
          "void end(void); void end(void) __attribute__((noreturn)); void end(void);"}) {
        std::string src{decls};
        src += "\nint f(int x) { x ? end() : end(); }\nint main(void) { return 0; }\n";
        auto const l = lowerC(src);
        ASSERT_TRUE(l->result->ok) << decls << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kReached), 0u) << decls << dump(l->reporter);
        HirNodeId const f = l->function("f");
        ASSERT_TRUE(f.valid()) << decls;
        EXPECT_TRUE(holdsSyntheticUnreachable(l->hir(), l->hir().functionBody(f))) << decls;
    }
}

// A CALL THAT PRECEDES THE DECLARATION WHICH ADDS `noreturn` — DSS'S DECISION,
// pinned as one so nobody "fixes" it toward a reference that answers otherwise.
//
// "Does not return" is a property of the FUNCTION, not of the declaration visible
// at the call: the record is merged over the unit's declarations and the leaf
// (`isDirectNoreturnCall`) reads the merged record, so it is order-blind ON
// PURPOSE. ✔MEASURED (probe fo3, `fo_nr.c`): gcc 13.3.0, mingw-w64 gcc and cl
// 14.51 do NOT report these bodies as reaching the end; clang 18.1.3 reports the
// plain call and the conditional, at -O0 and -O2 — what an analysis run at the
// end of each body, before the later declaration has been seen, would say. The
// claim itself is sound for every defined program ("A function declared with a
// _Noreturn function specifier shall not return to its caller", C23 6.7.5p8), so
// the split is over a warning only, and a warning on an end that cannot be
// reached would be a false one.
//
// RED-ON-DISABLE: an order-aware leaf (or a record no longer merged across the
// declarations) reports all four and wraps none.
TEST(NonVoidFunctionEndReached, ACallBeforeTheDeclarationThatAddsNoreturnIsATerminator) {
    for (std::string_view const body :
         {"{ (void)x; end(); }",           // the plain call
          "{ x ? end() : end(); }",       // both arms of a conditional
          "{ (void)x, end(); }",          // through the comma operator
          "{ (void)x; (void)end(); }"}) { // through a cast
        std::string src = "void end(void);\nint f(int x) ";
        src += body;
        src += "\n_Noreturn void end(void);\nint main(void) { return 0; }\n";
        auto const l = lowerC(src);
        ASSERT_TRUE(l->result->ok) << body << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kReached), 0u) << body << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kRefused), 0u) << body << dump(l->reporter);
        HirNodeId const f = l->function("f");
        ASSERT_TRUE(f.valid()) << body;
        EXPECT_FALSE(completionLocalType(*l, f).valid()) << body;
        EXPECT_TRUE(holdsSyntheticUnreachable(l->hir(), l->hir().functionBody(f))) << body;
    }
}

// THE VALUE IS THE ANALYSIS MACHINE'S, NOT THE HOST'S. `sizeof(long)` is 8 under
// LP64 and 4 under LLP64, so the SAME source ends in a statement that never
// completes under one and in an `if` that does nothing under the other — and the
// lowering must answer each from the parameters the analysis ran under, which the
// model carries (`SemanticModel::aggregateLayout`, `dataModel`). A fold that read
// the host, or a fixed table, would give one answer on every row of a model.
// ✔MEASURED (probe fo5, each platform's own references): gcc 13.3.0, clang 18.1.3
// and Apple clang 21.0.0 (LP64) are silent on `== 8` and report `!= 8`; cl 14.51
// and mingw-w64 gcc (LLP64) report `== 8` and are silent on `!= 8`.
//
// The `--target`-keyed twin of this pin, through the whole pipeline on every
// shipped pair, is `CompilePipeline.AConstantConditionReadsTheTargetsOwnLayout`.
TEST(NonVoidFunctionEndReached, ALayoutConstantIsTheAnalysisMachinesOwn) {
    struct Row {
        char const*      what;
        Machine const*   machine;
        std::string_view body;
        bool             reached;
    };
    for (Row const& r : {
             Row{"LP64, sizeof(long) == 8", &kLp64, "{ if (sizeof(long) == 8) return x; }", false},
             Row{"LP64, sizeof(long) != 8", &kLp64, "{ if (sizeof(long) != 8) return x; }", true},
             Row{"LLP64, sizeof(long) == 8", &kLlp64, "{ if (sizeof(long) == 8) return x; }", true},
             Row{"LLP64, sizeof(long) != 8", &kLlp64, "{ if (sizeof(long) != 8) return x; }", false},
             // the control: a size both models agree on
             Row{"LP64, sizeof(long long) == 8", &kLp64,
                 "{ if (sizeof(long long) == 8) return x; }", false},
             Row{"LLP64, sizeof(long long) == 8", &kLlp64,
                 "{ if (sizeof(long long) == 8) return x; }", false}}) {
        auto const l = lowerC(functionWithBody(r.body), *r.machine);
        ASSERT_TRUE(l->result->ok) << r.what << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kReached), r.reached ? 1u : 0u)
            << r.what << dump(l->reporter);
        HirNodeId const f = l->function("f");
        ASSERT_TRUE(f.valid()) << r.what;
        EXPECT_EQ(completionLocalType(*l, f).valid(), r.reached) << r.what;
        EXPECT_EQ(holdsSyntheticUnreachable(l->hir(), l->hir().functionBody(f)), !r.reached)
            << r.what;
    }
}

// WITH NO LAYOUT A LAYOUT QUESTION IS NOT A CONSTANT HERE — never a guessed size.
// An analysis that ran with no parameters (the LSP, the FFI header parser, a
// direct-API caller) leaves the lowering nothing to size a type with, so each
// predicate answers "may continue": the body is completed and reported, and no
// statement is claimed never to complete. The second half is the control: a
// condition that needs no layout is still a constant without one.
//
// RED-ON-DISABLE (the carried member): build the model WITHOUT the parameters and
// every layout row of `ShapesNoExecutionLeavesAreNotReported` reads as these do.
TEST(NonVoidFunctionEndReached, NoLayoutMeansALayoutQuestionIsNotAConstant) {
    for (std::string_view const body :
         {"{ if (sizeof(int) >= 1) return x; }",
          "{ if (sizeof x >= 1) return x; }",
          "{ if (_Alignof(double) >= 1) return x; }",
          "{ if ((unsigned long)&((struct P *)0)->b >= 1) return x; }",
          "{ while (sizeof(int)) { if (x) return x; } }"}) {
        auto const l = lowerC(functionWithBody(body), kNoLayout);
        ASSERT_TRUE(l->result->ok) << body << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kReached), 1u) << body << dump(l->reporter);
        HirNodeId const f = l->function("f");
        ASSERT_TRUE(f.valid()) << body;
        EXPECT_TRUE(completionLocalType(*l, f).valid()) << body;
        EXPECT_FALSE(holdsSyntheticUnreachable(l->hir(), l->hir().functionBody(f))) << body;
    }
    for (std::string_view const body :
         {"{ if (1) return x; }",
          "{ enum { ON = 1 }; if (ON) return x; }",
          "{ if (_Generic(x, int: 1, default: 0)) return x; }"}) {
        auto const l = lowerC(functionWithBody(body), kNoLayout);
        ASSERT_TRUE(l->result->ok) << body << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kReached), 0u) << body << dump(l->reporter);
    }
}

// AN INDEX DESIGNATOR IS THE THIRD CONSUMER of the same evaluator, and a layout
// constant is an integer constant expression there too (C 6.6p6: `sizeof` and
// `alignof` results are). ✔MEASURED (probe fo5, `sz_desig.c`): gcc, clang, Apple
// clang, mingw-w64 gcc and cl all compile this and place every element where the
// expression says; the run-time half is in
// `examples/c/non_void_function_end_not_reached`.
//
// RED-ON-DISABLE: without `resolveSizeof` / `resolveAlignof` /
// `resolveFoldedConstant` the designator does not fold and the initializer is
// refused.
TEST(NonVoidFunctionEndReached, AnIndexDesignatorMayBeALayoutConstant) {
    auto const l = lowerC(
        "struct Q { char a; char c; short b; };\n"
        "static int t[8] = { [sizeof(char)] = 5, [sizeof(int)] = 7,\n"
        "                    [__builtin_offsetof(struct Q, b)] = 3, [_Alignof(short) + 4] = 9 };\n"
        "int main(void) {\n"
        "    int u[8] = { [sizeof(short)] = 6, [_Alignof(int)] = 8 };\n"
        "    return t[1] + t[4] + t[2] + t[6] + u[2] + u[4];\n"
        "}\n");
    EXPECT_TRUE(l->result->ok) << dump(l->reporter);
    EXPECT_TRUE(l->reporter.all().empty()) << dump(l->reporter);
}

// THE STRICT RULE IS THE DEFAULT, and it is still a rule: the shipped C document
// with the ONE key taken out — and again with the key spelled `refused` — keeps
// the verifier's refusal and never emits the warning. `main` is unaffected in
// both (its rule is the other key).
//
// RED-ON-DISABLE: drop the rule test at the head of
// `maybeCompleteReachableNonVoidEnd` (complete whatever the document says) and
// both arms compile `f` with a warning.
TEST(NonVoidFunctionEndReached, ADocumentThatDoesNotStateTheRuleKeepsTheRefusal) {
    struct Arm {
        char const*                     what;
        std::optional<std::string_view> spelling;
    };
    for (Arm const& arm : {Arm{"key absent", std::nullopt},
                           Arm{"key spelled refused", std::string_view{"refused"}}}) {
        auto const schema = cDocumentWithTheRule(arm.spelling);
        {
            auto const l = lowerUnder(schema,
                                      "int f(int x) { if (x) return 5; }\n"
                                      "int main(void) { return 0; }\n");
            EXPECT_FALSE(l->result->ok) << arm.what;
            EXPECT_EQ(countCode(l->reporter, kRefused), 1u) << arm.what << dump(l->reporter);
            EXPECT_EQ(countCode(l->reporter, kReached), 0u) << arm.what << dump(l->reporter);
        }
        {
            auto const l = lowerUnder(schema, "int main(void) { int x; x = 1; }\n");
            EXPECT_TRUE(l->result->ok) << arm.what << dump(l->reporter);
            EXPECT_TRUE(l->reporter.all().empty()) << arm.what << dump(l->reporter);
        }
    }
    // The control that makes the two arms above mean something: the SAME
    // rewriting path, writing the relaxed spelling back, compiles `f`.
    auto const relaxed = cDocumentWithTheRule(std::string_view{"returnsUnspecifiedValue"});
    auto const l = lowerUnder(relaxed,
                              "int f(int x) { if (x) return 5; }\n"
                              "int main(void) { return 0; }\n");
    EXPECT_TRUE(l->result->ok) << dump(l->reporter);
    EXPECT_EQ(countCode(l->reporter, kReached), 1u) << dump(l->reporter);
}
