// FC3 c1 acceptance — the complete width/signedness FRONT-END semantics
// (plan 23): the C 6.7.2 type-specifier multiset table, the format-schema
// dataModel (LP64/LLP64/ILP32) threading, the C 6.4.4.1 integer-literal
// ladder, the bool keyword literals, the loader fail-louds for every new
// config block, the shipped-lib descriptor signatureByDataModel
// resolution, and the toy/tsql typing-unchanged pins.
//
// Discipline: every engine behavior here is driven by the SHIPPED
// c-subset config (the vocabulary lives in JSON; the tests perturb the
// JSON to prove the loader rejects), and the dataModel differential is
// asserted by analyzing the SAME source under BOTH models — the
// signature-threading makes that directly testable.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_visitor.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "link/object_format_schema.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

// Analyze one c-subset source under an explicit data model.
[[nodiscard]] SemanticModel analyzeCSubset(std::string src,
                                           DataModel dm = DataModel::Lp64) {
    auto cu = buildShippedUnit("c-subset", {std::move(src)});
    assertNoBuilderErrors(*cu);
    return analyze(cu, dm);
}

// The SOURCE TEXT of each `_Generic`'s SELECTED result expression, in source
// order — the DIRECT observation of which association won. Strictly stronger
// than watching a downstream type: two associations can coincide in result width
// (and DSS still types the selection from its CONTROLLING expression today —
// D-CSUBSET-GENERIC-RESULT-TYPE-DEDUCTION), so a type-based check can be green
// for the wrong arm.
[[nodiscard]] std::vector<std::string> selectedGenericArms(SemanticModel const& m) {
    std::vector<std::string> out;
    for (auto const& tree : m.unit().trees()) {
        RuleId const gid = tree.schema().rules().find("genericExpr");
        if (!gid.valid()) continue;
        for (std::uint32_t i = 1; i < tree.nodeCount(); ++i) {
            NodeId const node{i};
            if (tree.kind(node) != NodeKind::Internal) continue;
            if (tree.rule(node).v != gid.v) continue;
            NodeId const sel = m.selectedGenericExpr(node);
            out.push_back(sel.valid() ? std::string{tree.text(sel)}
                                      : std::string{"<none>"});
        }
    }
    return out;
}

// The TypeKind of the named symbol (test fails when absent/untyped).
[[nodiscard]] TypeKind kindOf(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i) {
        if (m.symbols()[i].name == name) {
            if (!m.symbols()[i].type.valid()) {
                ADD_FAILURE() << "symbol '" << name << "' has no resolved type";
                return TypeKind::Void;
            }
            return m.lattice().interner().kind(m.symbols()[i].type);
        }
    }
    ADD_FAILURE() << "symbol '" << name << "' not found";
    return TypeKind::Void;
}

// The shipped c-subset JSON text (for loader perturbation tests). Found
// the same way loadShipped finds it — walk up from cwd.
[[nodiscard]] std::filesystem::path findShippedSourceConfig() {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 12; ++i) {
        fs::path const candidate =
            dir / "src" / "dss-config" / "sources" / "c-subset.lang.json";
        if (fs::exists(candidate)) return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    ADD_FAILURE() << "could not locate shipped c-subset.lang.json above cwd";
    return {};
}

[[nodiscard]] nlohmann::json loadShippedCSubsetJson() {
    auto const path = findShippedSourceConfig();
    std::ifstream in{path, std::ios::binary};
    EXPECT_TRUE(in.good());
    return nlohmann::json::parse(in);
}

// Load a (perturbed) schema text; returns whether it loaded cleanly.
[[nodiscard]] bool schemaLoads(nlohmann::json const& doc) {
    auto r = GrammarSchema::loadFromText(doc.dump(), "<fc3-perturbed>");
    return r.has_value();
}

// Analyze a c-subset source under a schema whose `arithmeticConversions`
// block is `mutate`d in place first — the perturbation that proves a config
// verb is LIVE (the engine reads it, so flipping it changes a typed result).
[[nodiscard]] SemanticModel analyzeWithArithMutation(
    std::string src, std::function<void(nlohmann::json&)> mutate,
    DataModel dm = DataModel::Lp64) {
    nlohmann::json doc = loadShippedCSubsetJson();
    mutate(doc["semantics"]["arithmeticConversions"]);
    auto schema = GrammarSchema::loadFromText(doc.dump(), "<arith-perturbed>");
    if (!schema) {
        ADD_FAILURE() << "perturbed schema failed to load";
        std::abort();
    }
    UnitBuilder builder{*schema};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    // aggregateLayout MUST be present for an array-dim sizeof to fold (nullopt
    // ⇒ deliberate fail-loud) — the probe folds `sizeof(EXPR)` into a dimension.
    return analyze(cu, dm,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

// The folded dimension of the first array symbol named `name`. The established
// sizeof-folding probe (`<T> <name>[sizeof(EXPR)]` → the array's scalars[0]).
[[nodiscard]] std::int64_t arrayDimOf(SemanticModel const& m,
                                      std::string_view name) {
    auto const& ti = m.lattice().interner();
    for (std::size_t i = 1; i < m.symbols().size(); ++i) {
        if (m.symbols()[i].name != name) continue;
        TypeId const t = m.symbols()[i].type;
        if (!t.valid() || ti.kind(t) != TypeKind::Array
            || ti.scalars(t).empty()) {
            ADD_FAILURE() << "symbol '" << name << "' is not a sized array";
            return -1;
        }
        return ti.scalars(t)[0];
    }
    ADD_FAILURE() << "array symbol '" << name << "' not found";
    return -1;
}

} // namespace

// ── P1: the specifier multiset table ────────────────────────────────────

TEST(Fc3WidthSemantics, UnsignedIntTypesU32) {
    auto m = analyzeCSubset("int main() { unsigned int x; x = 1u; return 0; }\n");
    EXPECT_EQ(kindOf(m, "x"), TypeKind::U32);
}

TEST(Fc3WidthSemantics, SpecifierMultisetIsOrderFree) {
    // C 6.7.2: `unsigned long long int` == `long long unsigned int` ==
    // `long unsigned long` — THREE orderings, ONE type. The engine
    // canonicalizes the keyword-token multiset; order can never matter.
    for (auto const* src : {
             "int main() { unsigned long long int x; x = 1; return 0; }\n",
             "int main() { long long unsigned int x; x = 1; return 0; }\n",
             "int main() { long unsigned long x; x = 1; return 0; }\n",
         }) {
        auto m = analyzeCSubset(src);
        EXPECT_EQ(kindOf(m, "x"), TypeKind::U64) << src;
        EXPECT_FALSE(m.hasErrors()) << src;
    }
}

TEST(Fc3WidthSemantics, FullSpecifierSurfaceResolvesDeclaredCores) {
    struct Row { char const* decl; TypeKind want; };
    Row const rows[] = {
        {"short s;",              TypeKind::I16},
        {"short int s;",          TypeKind::I16},
        {"unsigned short s;",     TypeKind::U16},
        {"signed char s;",        TypeKind::I8},
        {"unsigned char s;",      TypeKind::U8},
        {"signed s;",             TypeKind::I32},
        {"unsigned s;",           TypeKind::U32},
        {"long long s;",          TypeKind::I64},
        {"unsigned long long s;", TypeKind::U64},
        {"bool s;",               TypeKind::Bool},
        {"_Bool s;",              TypeKind::Bool},
        {"float s;",              TypeKind::F32},
        {"double s;",             TypeKind::F64},
    };
    for (auto const& r : rows) {
        auto m = analyzeCSubset(std::string{"int main() { "} + r.decl
                                + " return 0; }\n");
        EXPECT_EQ(kindOf(m, "s"), r.want) << r.decl;
    }
}

TEST(Fc3WidthSemantics, InvalidSpecifierComboFailsLoudOnceByAbsence) {
    // `unsigned float` / `short long` are NOT rows in the typeSpecifiers
    // table — they reject by ABSENCE, with EXACTLY ONE diagnostic (the
    // precise S_InvalidTypeSpecifierCombination; the generic outer
    // S_UnknownType is suppressed for the same resolution). `long double`
    // LEFT this list with FC17.9(e) (D-CSUBSET-LONG-DOUBLE): it is now a
    // VALID row carrying the per-format axis — under an undeclared axis it
    // takes the precise S_LongDoubleFormatUndeclared instead (pinned by
    // SemanticAnalyzerCSubset.LongDoubleUndeclaredAxisFailsLoud).
    for (auto const* src : {
             "int main() { unsigned float x; return 0; }\n",
             "int main() { short long x; return 0; }\n",
         }) {
        auto m = analyzeCSubset(src);
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_InvalidTypeSpecifierCombination),
                  1u)
            << src;
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_UnknownType), 0u)
            << src;
    }
}

TEST(Fc3WidthSemantics, BoolKeywordLiteralsTypeBool) {
    auto m = analyzeCSubset(
        "int main() { bool t; t = true; bool f; f = false; return 0; }\n");
    EXPECT_EQ(kindOf(m, "t"), TypeKind::Bool);
    EXPECT_EQ(kindOf(m, "f"), TypeKind::Bool);
    EXPECT_FALSE(m.hasErrors());
}

// ── P2: dataModel threading (the SAME source under BOTH models) ─────────

TEST(Fc3WidthSemantics, LongIsI64OnLp64AndI32OnLlp64) {
    char const* src = "int main() { long x; x = 1l; return 0; }\n";
    auto lp = analyzeCSubset(src, DataModel::Lp64);
    EXPECT_EQ(kindOf(lp, "x"), TypeKind::I64);
    auto llp = analyzeCSubset(src, DataModel::Llp64);
    EXPECT_EQ(kindOf(llp, "x"), TypeKind::I32);
}

TEST(Fc3WidthSemantics, UnsignedLongFollowsDataModel) {
    char const* src = "int main() { unsigned long x; x = 1ul; return 0; }\n";
    EXPECT_EQ(kindOf(analyzeCSubset(src, DataModel::Lp64), "x"), TypeKind::U64);
    EXPECT_EQ(kindOf(analyzeCSubset(src, DataModel::Llp64), "x"), TypeKind::U32);
}

TEST(Fc3WidthSemantics, NarrowingLongLiteralNowAdmittedOnBothModels) {
    // D-CSUBSET-INT-SAME-SIGN-NARROW: implicit same-signedness integer narrowing
    // is now C-conformant (C 6.3.1.3), so 2147483648l passed to a `long` param is
    // ADMITTED under BOTH dataModels — LP64 (`long` is I64, holds it; identity)
    // and LLP64 (`long` is I32; the I64 long-long-laddered literal narrows +
    // truncates). Neither raises S_TypeMismatch any more. The dataModel WIDTH
    // differential is still witnessed elsewhere — at the type level by
    // UnsignedLongFollowsDataModel, and at RUNTIME by the datamodel_long_width_llp64
    // corpus (the truncation flips the sign → exit 7 vs the LP64 sibling's 42).
    // RED-ON-DISABLE: turn intSameSignednessNarrows off in c-subset and the LLP64
    // arm flips back to one S_TypeMismatch.
    char const* src =
        "long pick(long v) { return v; }\n"
        "int main() { long r; r = pick(2147483648l); return 0; }\n";
    auto lp = analyzeCSubset(src, DataModel::Lp64);
    EXPECT_EQ(countCode(lp.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_FALSE(lp.hasErrors());
    auto llp = analyzeCSubset(src, DataModel::Llp64);
    EXPECT_EQ(countCode(llp.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_FALSE(llp.hasErrors());
}

TEST(Fc3WidthSemantics, Ilp32SelectionFailsLoud) {
    // ILP32 is declared-only (wasm/spirv skeletons) — selecting it must
    // fail loud rather than silently run an untested width path.
    auto m = analyzeCSubset("int main() { return 0; }\n", DataModel::Ilp32);
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_UnsupportedDataModel), 1u);
    EXPECT_TRUE(m.hasErrors());
}

TEST(Fc3WidthSemantics, ModelCarriesTheAnalysisDataModel) {
    EXPECT_EQ(analyzeCSubset("int main() { return 0; }\n",
                             DataModel::Llp64).dataModel(),
              DataModel::Llp64);
}

// ── P3: the integer-literal ladder (C 6.4.4.1) ──────────────────────────

namespace {
// Pin the literal's EXACT ladder type (C 6.4.4.1) by reading the TypeId the
// semantic analyzer STAMPED on the IntLiteral leaf — the strongest possible
// assertion: the literal's *actual decoded type* (width AND signedness), not
// an assignability proxy.
//
// This supersedes the prior cross-signedness-mismatch proxy. The c-subset
// `intCrossSignednessConverts` opt-in (D-CSUBSET-INT-CROSS-SIGNEDNESS-CONVERT,
// C 6.3.1.3) admits signed<->unsigned ASSIGNMENT, so an opposite-signedness
// parameter no longer mismatches — signedness became unobservable through
// assignability. Reading the stamped type observes both axes directly, and is
// the stronger pin regardless of the gate.
void expectLiteralTypes(std::string const& literal, TypeKind want,
                        DataModel dm = DataModel::Lp64) {
    auto cu = buildShippedUnit(
        "c-subset", {std::string{"int main() { "} + literal + "; return 0; }\n"});
    assertNoBuilderErrors(*cu);
    auto m = analyze(cu, dm);
    EXPECT_FALSE(m.hasErrors()) << literal << " should analyze without error";
    // Locate the IntLiteral leaf by its token spelling. The `return 0;`
    // epilogue uses a distinct `0` token, so the match is unique for every
    // laddered literal under test (none is the bare "0"). Take the FIRST hit.
    Tree const& tree = cu->trees()[0];
    NodeId lit{};
    walkPreOrder(tree, [&](TreeCursor const& cursor) {
        NodeId const n = cursor.current();
        if (lit.valid()) return;
        if (tree.kind(n) == NodeKind::Token && tree.text(n) == literal) lit = n;
    });
    ASSERT_TRUE(lit.valid())
        << "IntLiteral leaf '" << literal << "' not found in the tree";
    TypeId const got = m.typeAt(lit);
    ASSERT_TRUE(got.valid()) << literal << " leaf carries no stamped type";
    EXPECT_EQ(m.lattice().interner().kind(got), want)
        << literal << " must type kind " << static_cast<int>(want) << " — got "
        << static_cast<int>(m.lattice().interner().kind(got));
}
} // namespace

TEST(Fc3WidthSemantics, DecimalLadderPicksFirstFittingCandidate) {
    expectLiteralTypes("2147483647", TypeKind::I32);          // INT_MAX → int
    expectLiteralTypes("2147483648", TypeKind::I64);          // → long (LP64)
}

TEST(Fc3WidthSemantics, DecimalAboveIntIsStillSignedOnLlp64ViaLongLong) {
    // LLP64: long is I32 (2^31 does not fit) → the next decimal
    // candidate `long long` (I64). Decimal constants NEVER go unsigned
    // without a suffix (C 6.4.4.1).
    expectLiteralTypes("2147483648", TypeKind::I64, DataModel::Llp64);
}

TEST(Fc3WidthSemantics, NondecimalLadderAdmitsUnsignedCandidates) {
    // 0x80000000 does not fit int; hex constants try unsigned int NEXT
    // (the nondecimal ladder) → U32. The decimal spelling of the same
    // value went I64 — the radix-class differential.
    expectLiteralTypes("0x80000000", TypeKind::U32);
}

TEST(Fc3WidthSemantics, SuffixesSelectTheirLadderRules) {
    expectLiteralTypes("42u", TypeKind::U32);
    expectLiteralTypes("42ul", TypeKind::U64);                 // LP64 long
    expectLiteralTypes("42ul", TypeKind::U32, DataModel::Llp64);
    expectLiteralTypes("42ll", TypeKind::I64);
    expectLiteralTypes("42ull", TypeKind::U64);
    // Suffix × magnitude interplay: a u-suffixed value above U32 climbs
    // to unsigned long (U64 on LP64).
    expectLiteralTypes("4294967296u", TypeKind::U64);
}

TEST(Fc3WidthSemantics, LiteralBeyondEveryCandidateFailsLoud) {
    // 2^64 exceeds unsigned long long — decode-tier overflow → the same
    // user-facing S_IntegerLiteralTooLarge as ladder exhaustion.
    auto m = analyzeCSubset(
        "int main() { unsigned long long x; x = 18446744073709551616ull; "
        "return 0; }\n");
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_IntegerLiteralTooLarge), 1u);
    // Ladder exhaustion (decodable, but beyond every SIGNED decimal
    // candidate): 2^63 has no decimal-unsuffixed candidate in C.
    auto m2 = analyzeCSubset(
        "int main() { long long x; x = 9223372036854775808; return 0; }\n");
    EXPECT_EQ(countCode(m2.diagnostics(),
                        DiagnosticCode::S_IntegerLiteralTooLarge), 1u);
}

TEST(Fc3WidthSemantics, FloatSuffixedLiteralTypesF32) {
    // FC3.5 sweep-c2 (D-CSUBSET-F32-CODEGEN closed): `1.5f` now types
    // F32 per C 6.4.4.2 — the `floatLiteralTyping` suffix map's flip
    // of the interim c1 F64 pin (the F32 arithmetic surface shipped
    // alongside, so the typing pairs with real codegen). Pin the
    // EXACT type through assignability: `float` param accepts `1.5f`
    // cleanly; an F32 value also implicitly WIDENS into a `double`
    // param (C float→double conversion — same-direction float
    // widening) so the discriminating probe is the REVERSE: an
    // UNSUFFIXED `1.5` (F64) into a `float` param must MISMATCH
    // (narrowing — assignability admits only widening), while `1.5f`
    // into `float` is clean. That asymmetry proves the suffix
    // selected F32, not merely "something float".
    auto cleanF32 = analyzeCSubset(
        "int take(float v) { return 0; }\n"
        "int main() { int r; r = take(1.5f); return 0; }\n");
    EXPECT_EQ(countCode(cleanF32.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "1.5f must pass cleanly into a float param (types F32)";
    EXPECT_FALSE(cleanF32.hasErrors());
    auto widen = analyzeCSubset(
        "int take(double v) { return 0; }\n"
        "int main() { int r; r = take(1.5f); return 0; }\n");
    EXPECT_EQ(countCode(widen.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 0u)
        << "F32 widens implicitly into a double param";
    auto narrow = analyzeCSubset(
        "int take(float v) { return 0; }\n"
        "int main() { int r; r = take(1.5); return 0; }\n");
    EXPECT_EQ(countCode(narrow.diagnostics(),
                        DiagnosticCode::S_TypeMismatch), 1u)
        << "unsuffixed 1.5 stays F64 — narrowing into float must "
           "mismatch, proving the suffix (not the base core) typed "
           "1.5f";
}

// ── FC3.5 sweep-c2: floatLiteralTyping loader validation ────────────────
// (Mirrors the integerLiteralTyping cross-checks: suffix coverage,
// exactly-one-unsuffixed, lexer-known suffixes, float-kind types.)

TEST(Fc3WidthSemantics, FloatTypingUncoveredSuffixRejectsAtLoad) {
    // Remove the f/F rule: numberStyle.floatSuffixes still declares
    // them → the lexer would admit a literal the typing map cannot
    // type. Must reject at LOAD, never silently fall back.
    auto doc = loadShippedCSubsetJson();
    auto& rules = doc["semantics"]["floatLiteralTyping"];
    rules.erase(
        std::remove_if(rules.begin(), rules.end(),
                       [](nlohmann::json const& r) {
                           return r.contains("suffixes")
                               && !r["suffixes"].empty();
                       }),
        rules.end());
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3WidthSemantics, FloatTypingUnknownSuffixRejectsAtLoad) {
    // A rule naming a suffix the lexer does not admit is dead config.
    auto doc = loadShippedCSubsetJson();
    doc["semantics"]["floatLiteralTyping"].push_back(
        {{"suffixes", {"q"}}, {"type", "double"}});
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3WidthSemantics, FloatTypingNonFloatTypeRejectsAtLoad) {
    auto doc = loadShippedCSubsetJson();
    for (auto& r : doc["semantics"]["floatLiteralTyping"]) {
        if (r.contains("suffixes") && r["suffixes"].empty()) {
            r["type"] = "int";
        }
    }
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3WidthSemantics, FloatTypingMissingUnsuffixedRuleRejectsAtLoad) {
    auto doc = loadShippedCSubsetJson();
    auto& rules = doc["semantics"]["floatLiteralTyping"];
    rules.erase(
        std::remove_if(rules.begin(), rules.end(),
                       [](nlohmann::json const& r) {
                           return r.contains("suffixes")
                               && r["suffixes"].empty();
                       }),
        rules.end());
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3WidthSemantics, FloatTypingDuplicateSuffixClaimRejectsAtLoad) {
    auto doc = loadShippedCSubsetJson();
    doc["semantics"]["floatLiteralTyping"].push_back(
        {{"suffixes", {"f"}}, {"type", "double"}});
    EXPECT_FALSE(schemaLoads(doc));
}

// ── toy / tsql: typing-unchanged pins ───────────────────────────────────

TEST(Fc3WidthSemantics, ToyTypingIsDataModelInvariantAndUnchanged) {
    // toy declares NO typeSpecifiers / integerLiteralTyping /
    // arithmeticConversions — its typing must be IDENTICAL under every
    // data model (nothing in its config consumes the model) and match
    // the pre-FC3 shape (int → I32 via the literalTypes token map).
    char const* src = "var x : int = 3;";
    auto cuA = buildShippedUnit("toy", {src});
    auto a = analyze(cuA, DataModel::Lp64);
    auto cuB = buildShippedUnit("toy", {src});
    auto b = analyze(cuB, DataModel::Llp64);
    ASSERT_EQ(a.symbols().size(), b.symbols().size());
    for (std::size_t i = 1; i < a.symbols().size(); ++i) {
        TypeKind const ka = a.symbols()[i].type.valid()
            ? a.lattice().interner().kind(a.symbols()[i].type) : TypeKind::Void;
        TypeKind const kb = b.symbols()[i].type.valid()
            ? b.lattice().interner().kind(b.symbols()[i].type) : TypeKind::Void;
        EXPECT_EQ(ka, kb) << "toy symbol " << a.symbols()[i].name;
    }
    EXPECT_EQ(a.diagnostics().all().size(), b.diagnostics().all().size());
    EXPECT_EQ(kindOf(a, "x"), TypeKind::I32);
}

TEST(Fc3WidthSemantics, TsqlTypingIsDataModelInvariantAndUnchanged) {
    char const* src = "CREATE TABLE t (id INT NOT NULL, flag BIT);";
    auto cuA = buildShippedUnit("tsql-subset", {src});
    auto a = analyze(cuA, DataModel::Lp64);
    auto cuB = buildShippedUnit("tsql-subset", {src});
    auto b = analyze(cuB, DataModel::Llp64);
    ASSERT_EQ(a.symbols().size(), b.symbols().size());
    for (std::size_t i = 1; i < a.symbols().size(); ++i) {
        TypeKind const ka = a.symbols()[i].type.valid()
            ? a.lattice().interner().kind(a.symbols()[i].type) : TypeKind::Void;
        TypeKind const kb = b.symbols()[i].type.valid()
            ? b.lattice().interner().kind(b.symbols()[i].type) : TypeKind::Void;
        EXPECT_EQ(ka, kb) << "tsql symbol " << a.symbols()[i].name;
    }
    EXPECT_EQ(a.diagnostics().all().size(), b.diagnostics().all().size());
    EXPECT_EQ(kindOf(a, "id"), TypeKind::I32);
    EXPECT_EQ(kindOf(a, "flag"), TypeKind::Bool);
}

// ── Loader fail-louds (shipped JSON, surgically perturbed) ──────────────

TEST(Fc3LoaderRejects, ShippedCSubsetLoadsClean) {
    EXPECT_TRUE(schemaLoads(loadShippedCSubsetJson()));
}

TEST(Fc3LoaderRejects, DuplicateSpecifierMultisetRejects) {
    auto doc = loadShippedCSubsetJson();
    // `unsigned int` is already declared; a REORDERED duplicate must
    // reject (resolution would silently shadow one row).
    doc["semantics"]["typeSpecifiers"].push_back(
        {{"tokens", {"IntKeyword", "UnsignedKeyword"}}, {"core", "U32"}});
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3LoaderRejects, UnknownCoreNameRejects) {
    auto doc = loadShippedCSubsetJson();
    doc["semantics"]["typeSpecifiers"][0]["core"] = "I31";
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3LoaderRejects, UnknownTokenKindRejects) {
    auto doc = loadShippedCSubsetJson();
    doc["semantics"]["typeSpecifiers"].push_back(
        {{"tokens", {"NoSuchKeyword"}}, {"core", "I32"}});
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3LoaderRejects, UnknownDataModelKeyRejects) {
    auto doc = loadShippedCSubsetJson();
    // A typo'd model name would otherwise silently never override —
    // the exact knob-that-lies the closed key set forecloses.
    doc["semantics"]["builtinTypes"][1]["coreByDataModel"] =
        {{"LLP65", "I32"}};
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3LoaderRejects, UnknownTypeSpecifierFieldRejects) {
    auto doc = loadShippedCSubsetJson();
    doc["semantics"]["typeSpecifiers"][0]["coar"] = "I32";
    EXPECT_FALSE(schemaLoads(doc));
}

// FC17.9(e) (D-CSUBSET-LONG-DOUBLE): `coreByLongDoubleFormat` is CLOSED both
// ways — a typo'd axis key would silently never override (the long-double row
// would bind base F64 on every walled format = the knob-that-lies), and a
// typo'd core value has no meaning at all.
TEST(Fc3LoaderRejects, UnknownLongDoubleFormatKeyRejects) {
    auto doc = loadShippedCSubsetJson();
    doc["semantics"]["typeSpecifiers"].push_back(
        {{"tokens", {"LongKeyword", "FloatKeyword"}},
         {"core", "F64"},
         {"coreByLongDoubleFormat", {{"x86-80", "F80"}}}});   // typo'd key
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3LoaderRejects, UnknownLongDoubleFormatValueRejects) {
    auto doc = loadShippedCSubsetJson();
    doc["semantics"]["typeSpecifiers"].push_back(
        {{"tokens", {"LongKeyword", "FloatKeyword"}},
         {"core", "F64"},
         {"coreByLongDoubleFormat", {{"x87-80", "F81"}}}});   // typo'd core
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3LoaderRejects, UnknownMixedSignednessVerbRejects) {
    auto doc = loadShippedCSubsetJson();
    doc["semantics"]["arithmeticConversions"]["mixedSignedness"] =
        "rank-prefer-signed";
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3LoaderRejects, UnknownShiftResultVerbRejects) {
    // `shiftResult` (C 6.5.7) is a CLOSED verb — a typo'd spelling must fail
    // loud at load, never silently fall back to a default discipline.
    auto doc = loadShippedCSubsetJson();
    doc["semantics"]["arithmeticConversions"]["shiftResult"] = "promotedRight";
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3LoaderRejects, UnknownLadderTypeNameRejects) {
    auto doc = loadShippedCSubsetJson();
    doc["semantics"]["integerLiteralTyping"][0]["decimal"][0] =
        "no such type";
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3LoaderRejects, LadderMustCoverEveryDeclaredSuffix) {
    // Drop the `u`-group rule: the lexer still admits `42u`, so the
    // ladder could never type it — a silent config hole; reject.
    auto doc = loadShippedCSubsetJson();
    auto& ladder = doc["semantics"]["integerLiteralTyping"];
    for (std::size_t i = 0; i < ladder.size(); ++i) {
        auto const& sfx = ladder[i]["suffixes"];
        if (!sfx.empty() && sfx[0].get<std::string>() == "u") {
            ladder.erase(i);
            break;
        }
    }
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3LoaderRejects, LadderRequiresExactlyOneUnsuffixedRule) {
    auto doc = loadShippedCSubsetJson();
    auto& ladder = doc["semantics"]["integerLiteralTyping"];
    for (std::size_t i = 0; i < ladder.size(); ++i) {
        if (ladder[i]["suffixes"].empty()) {
            ladder.erase(i);
            break;
        }
    }
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(Fc3LoaderRejects, NonIntegerLadderCandidateRejects) {
    auto doc = loadShippedCSubsetJson();
    doc["semantics"]["integerLiteralTyping"][0]["decimal"][0] = "double";
    EXPECT_FALSE(schemaLoads(doc));
}

// ── D-UAC-SHIFT-RESULT-RULE-CONFIG: the shift-result rule is a config verb ──
//
// The closed verb `shiftResult` selects a shift's RESULT TYPE (C 6.5.7). The
// SEMANTIC-tier site (`subtreeType`) is witnessed end-to-end by folding
// `sizeof(a << b)` into an array dimension: `a` is `int` (I32, 4B), `b` is
// `long long` (I64, 8B under EVERY data model). `promotedLeft` → the promoted
// LEFT operand `int` → dim 4; `commonType` → the usual-arithmetic common type
// `long long` → dim 8. The 4↔8 flip when ONLY the verb changes IS the red-on-
// disable proof — a dead knob would peg both arms at 4. (Variable operands, not
// literals: c-subset's `sizeof` of a value expression folds through `subtreeType`
// — which routes the shift through the same `shiftResultType` chokepoint — so
// this is the SEMANTIC tier's behavioral pin; the sibling cst_to_hir site is in
// test_hir_lowering_c_subset.cpp and the chokepoint unit pin in test_type_rules.)

TEST(Fc3ShiftResult, PromotedLeftSizesByLeftOperand) {
    auto m = analyzeWithArithMutation(
        "int a; long long b; char arr[sizeof(a << b)];\n",
        [](nlohmann::json& ac) { ac["shiftResult"] = "promotedLeft"; });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(arrayDimOf(m, "arr"), 4)
        << "promotedLeft (C 6.5.7): (int << long long) types as the promoted "
           "left operand int → sizeof 4";
}

TEST(Fc3ShiftResult, CommonTypeSizesByCommonType) {
    auto m = analyzeWithArithMutation(
        "int a; long long b; char arr[sizeof(a << b)];\n",
        [](nlohmann::json& ac) { ac["shiftResult"] = "commonType"; });
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(arrayDimOf(m, "arr"), 8)
        << "commonType: (int << long long) types like an ordinary binary op → "
           "common(int,long long) = long long → sizeof 8 (the red-on-disable flip)";
}

TEST(Fc3ShiftResult, ShippedAndAbsentDefaultToPromotedLeft) {
    // The shipped config declares promotedLeft; a block written WITHOUT the
    // field keeps C's rule (the struct default is PromotedLeft) — both → 4.
    auto shipped = analyzeWithArithMutation(
        "int a; long long b; char arr[sizeof(a << b)];\n",
        [](nlohmann::json&) { /* no mutation: shipped promotedLeft */ });
    EXPECT_EQ(arrayDimOf(shipped, "arr"), 4) << "shipped promotedLeft → 4";
    auto absent = analyzeWithArithMutation(
        "int a; long long b; char arr[sizeof(a << b)];\n",
        [](nlohmann::json& ac) { ac.erase("shiftResult"); });
    EXPECT_EQ(arrayDimOf(absent, "arr"), 4)
        << "absent shiftResult → default promotedLeft (back-compat) → 4";
}

// ── Format-schema dataModel fail-louds ──────────────────────────────────

namespace {
[[nodiscard]] std::string minimalElfFormatJson(char const* dataModelLine) {
    return std::string{R"({
  "dssObjectFormatVersion": 1,
  )"} + dataModelLine + R"(
  "format": { "name": "fc3-stub", "version": "1.0", "kind": "elf" },
  "elf": { "class": "elf64", "data": "lsb", "machine": 62 }
})";
}
} // namespace

TEST(Fc3FormatDataModel, MissingDataModelRejects) {
    auto r = ObjectFormatSchema::loadFromText(minimalElfFormatJson(""));
    EXPECT_FALSE(r.has_value());
}

TEST(Fc3FormatDataModel, UnknownDataModelRejects) {
    auto r = ObjectFormatSchema::loadFromText(
        minimalElfFormatJson(R"("dataModel": "LP63",)"));
    EXPECT_FALSE(r.has_value());
}

TEST(Fc3FormatDataModel, DeclaredDataModelIsExposed) {
    auto r = ObjectFormatSchema::loadFromText(
        minimalElfFormatJson(R"("dataModel": "LLP64",)"));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->dataModel(), DataModel::Llp64);
}

TEST(Fc3FormatDataModel, ShippedFormatsDeclareTheirOsModels) {
    struct Row { char const* name; DataModel want; };
    Row const rows[] = {
        {"pe64-x86_64-windows-exec",   DataModel::Llp64},
        {"elf64-x86_64-linux-exec",    DataModel::Lp64},
        {"elf64-aarch64-linux-exec",   DataModel::Lp64},
        {"macho64-arm64-darwin-exec",  DataModel::Lp64},
        {"wasm32-v1",                  DataModel::Ilp32},
    };
    for (auto const& row : rows) {
        auto r = ObjectFormatSchema::loadShipped(row.name);
        ASSERT_TRUE(r.has_value()) << row.name;
        EXPECT_EQ((*r)->dataModel(), row.want) << row.name;
    }
}

// ── P5: descriptor signatureByDataModel ─────────────────────────────────

TEST(Fc3Descriptor, FseekOffsetFollowsTheDataModel) {
    // The SAME shipped stdio.json yields the LP64 i64 offset under LP64
    // and the LLP64 i32 offset under LLP64 — the reader resolves the
    // per-symbol signatureByDataModel against the threaded model.
    namespace fs = std::filesystem;
    auto base = findShippedSourceConfig();   // …/src/dss-config/sources/…
    ASSERT_FALSE(base.empty());
    fs::path const desc =
        base.parent_path().parent_path() / "shippedLibs" / "stdio.json";
    ASSERT_TRUE(fs::exists(desc));

    auto const offsetKindUnder = [&](DataModel dm) {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry registry;
        DiagnosticReporter rep;
        // c82: stdio.json's vfprintf spells `va_list` — bind it (the SysV
        // shape) exactly as the analyzer threads it in production; this
        // test's subject (fseek's per-model offset) is unchanged.
        TypeId const voidPtr =
            interner.pointer(interner.primitive(TypeKind::Void));
        std::array<TypeId, 4> vaTagFields{
            interner.primitive(TypeKind::U32),
            interner.primitive(TypeKind::U32), voidPtr, voidPtr};
        TypeId const vaListTy = interner.array(
            interner.structType("__va_list_tag", vaTagFields), 1);
        std::array<NamedTypeBinding, 1> namedTypes{
            NamedTypeBinding{"va_list", vaListTy}};
        auto d = ffi::readShippedLibDescriptor(desc, interner, registry, rep, dm,
                                               std::nullopt, std::nullopt,
                                               namedTypes);
        EXPECT_TRUE(d.has_value());
        EXPECT_EQ(rep.errorCount(), 0u);
        if (!d) return TypeKind::Void;
        for (auto const& s : d->symbols) {
            if (s.name == "fseek") {
                auto params = interner.fnParams(s.signature);
                EXPECT_EQ(params.size(), 3u);
                return interner.kind(params[1]);   // the C `long` offset
            }
        }
        ADD_FAILURE() << "fseek not found in stdio.json";
        return TypeKind::Void;
    };
    EXPECT_EQ(offsetKindUnder(DataModel::Lp64), TypeKind::I64);
    EXPECT_EQ(offsetKindUnder(DataModel::Llp64), TypeKind::I32);
}

TEST(Fc3Descriptor, UnknownSignatureByDataModelKeyFailsLoud) {
    namespace fs = std::filesystem;
    auto const tmp = fs::temp_directory_path() / "fc3_desc_badkey.json";
    {
        std::ofstream out{tmp, std::ios::binary};
        out << R"({"header":"x.h","symbols":[
          {"name":"f","signature":"fn(i32) -> i32",
           "signatureByDataModel":{"LLP65":"fn(i32) -> i32"}}]})";
    }
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry registry;
    DiagnosticReporter rep;
    auto d = ffi::readShippedLibDescriptor(tmp, interner, registry, rep,
                                           DataModel::Lp64);
    EXPECT_FALSE(d.has_value());
    EXPECT_GT(rep.errorCount(), 0u);
    fs::remove(tmp);
}

TEST(Fc3Descriptor, MalformedOverrideFailsEvenWhenNotSelected) {
    // A broken LLP64 override must fail the read under LP64 too — it
    // would otherwise lurk until the first Windows compile.
    namespace fs = std::filesystem;
    auto const tmp = fs::temp_directory_path() / "fc3_desc_badsig.json";
    {
        std::ofstream out{tmp, std::ios::binary};
        out << R"({"header":"x.h","symbols":[
          {"name":"f","signature":"fn(i32) -> i32",
           "signatureByDataModel":{"LLP64":"fn(notatype) -> i32"}}]})";
    }
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry registry;
    DiagnosticReporter rep;
    auto d = ffi::readShippedLibDescriptor(tmp, interner, registry, rep,
                                           DataModel::Lp64);
    EXPECT_FALSE(d.has_value());
    EXPECT_GT(rep.errorCount(), 0u);
    fs::remove(tmp);
}

// ── FC17.9(b) C23 <stdbit.h> (D-FULLC-STDBIT): the 5-way _Generic routing ──
// The generic `stdc_<op>(x)` macro (shippedLibs/stdbit.json) expands to a 5-WAY
// _Generic over {unsigned char, unsigned short, unsigned int, unsigned long,
// unsigned long long} — the full C23 §7.18 association set. It was a 4-way with
// `unsigned long` DROPPED until D-LANG-TYPE-IDENTITY-VOCABULARY: identity was
// derived from REPRESENTATION, so `unsigned long` interned to the same TypeId as
// `unsigned int` (LLP64) / `unsigned long long` (LP64) and a 5-way was
// S_GenericSelectionAmbiguous. Identity now comes from the language vocabulary
// entry, so all five are distinct types on BOTH data models. These pins lock that
// in — the exact source the macro produces (built here, since the semantic
// harness does not run #include).

// The 5-way _Generic text for `op` over operand expression `arg`. The
// `unsigned long` arm routes to the width the ACTIVE data model gives it, which
// is what the shipped per-format `stdc_<op>_ul` variant macro resolves to.
[[nodiscard]] static std::string stdbitGeneric5Way(std::string const& op,
                                                   std::string const& arg,
                                                   DataModel dm) {
    auto call = [&](char const* w) {
        return "__builtin_stdc_" + op + "_" + w + "(" + arg + ")";
    };
    return "_Generic((" + arg + "), "
           "unsigned char: "      + call("uc")  + ", "
           "unsigned short: "     + call("us")  + ", "
           "unsigned int: "       + call("ui")  + ", "
           "unsigned long: "      + call(dm == DataModel::Llp64 ? "ui" : "ull") + ", "
           "unsigned long long: " + call("ull") + ")";
}

// All five associations compile with NO ambiguity + NO no-match on both models —
// the red-on-disable guard for the identity split (collapse `unsigned long` back
// onto a width core and this goes S_GenericSelectionAmbiguous again).
static void expectStdbit5WayCleanUnder(DataModel dm) {
    std::string const src =
        "unsigned f(unsigned char a, unsigned short b, unsigned int c,\n"
        "           unsigned long d, unsigned long long e){\n"
        "  return " + stdbitGeneric5Way("count_ones", "a", dm)
              + "\n       + " + stdbitGeneric5Way("count_ones", "b", dm)
              + "\n       + " + stdbitGeneric5Way("count_ones", "c", dm)
              + "\n       + " + stdbitGeneric5Way("count_ones", "d", dm)
              + "\n       + " + stdbitGeneric5Way("count_ones", "e", dm) + ";\n}\n";
    auto m = analyzeCSubset(src, dm);
    EXPECT_FALSE(m.hasErrors()) << "the 5-way _Generic must compile clean";
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_GenericSelectionAmbiguous), 0u)
        << "all five unsigned vocabulary entries are DISTINCT types — never ambiguous";
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_GenericSelectionNoMatch), 0u)
        << "every unsigned width matches exactly one association";
}

TEST(Fc3Stdbit, GenericFiveWayNoAmbiguityLp64)  { expectStdbit5WayCleanUnder(DataModel::Lp64); }
TEST(Fc3Stdbit, GenericFiveWayNoAmbiguityLlp64) { expectStdbit5WayCleanUnder(DataModel::Llp64); }

// Positive routing of an `unsigned long` operand through the 5-way: it takes its
// OWN `unsigned long:` association rather than width-collapsing onto a neighbour.
//
// ★ WHY THE WINNER IS OBSERVED DIRECTLY. The earlier form of this pin read
// `kindOf(r)` off an `auto r = _Generic(...)`, which is NOT evidence of arm
// selection for TWO independent reasons:
//   * the `unsigned long:` arm DELIBERATELY calls the same builtin as its
//     same-width neighbour (that is the whole point — the shipped `stdc_<op>_ul`
//     variant resolves to the model's width), so the RESULT WIDTH is identical
//     whether `unsigned long:` or that neighbour won; and
//   * DSS types a generic selection from its CONTROLLING expression today, not
//     from the selected association (D-CSUBSET-GENERIC-RESULT-TYPE-DEDUCTION), so
//     `kindOf(r)` was reporting `x`'s own width regardless of the outcome.
// `SemanticModel::selectedGenericExpr` names the winning association's expression
// node outright — the only observation that cannot be faked by a coincidence of
// widths. The two same-builtin arms are made textually distinguishable by an
// extra pair of parentheses around the argument, which changes neither the callee
// nor the operand type.
TEST(Fc3Stdbit, GenericUnsignedLongRoutesByDataModel) {
    // The 5-way with the `unsigned long` arm and its same-width neighbour calling
    // the SAME builtin, spelled differently so the winner is readable.
    auto const srcFor = [](DataModel dm) {
        auto call = [](char const* w, char const* arg) {
            return "__builtin_stdc_bit_floor_" + std::string{w} + "(" + arg + ")";
        };
        bool const llp = (dm == DataModel::Llp64);
        std::string const gen =
            std::string{"_Generic((x), "}
            + "unsigned char: "      + call("uc", "x")  + ", "
            + "unsigned short: "     + call("us", "x")  + ", "
            // The neighbour that SHARES the model's `unsigned long` width spells
            // its argument `(x)`; the `unsigned long` arm spells it `x`.
            + "unsigned int: "       + call("ui",  llp ? "(x)" : "x")  + ", "
            + "unsigned long: "      + call(llp ? "ui" : "ull", "x") + ", "
            + "unsigned long long: " + call("ull", llp ? "x" : "(x)") + ")";
        return "unsigned long f(unsigned long x){ auto r = " + gen
             + "; return r; }\n";
    };
    for (DataModel const dm : {DataModel::Lp64, DataModel::Llp64}) {
        SCOPED_TRACE(dm == DataModel::Lp64 ? "LP64" : "LLP64");
        auto m = analyzeCSubset(srcFor(dm), dm);
        ASSERT_FALSE(m.hasErrors());
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_GenericSelectionAmbiguous), 0u);
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_GenericSelectionNoMatch), 0u);
        // The `unsigned long` arm is the ONLY one spelling its argument bare `x`
        // at the model's own width, so its text pins the selection exactly.
        std::string const want =
            std::string{"__builtin_stdc_bit_floor_"}
            + (dm == DataModel::Llp64 ? "ui" : "ull") + "(x)";
        EXPECT_EQ(selectedGenericArms(m), (std::vector<std::string>{want}))
            << "an `unsigned long` operand must select the `unsigned long:` "
               "association — not the same-width neighbour it collapsed onto "
               "before identity was split off representation";
    }
}
