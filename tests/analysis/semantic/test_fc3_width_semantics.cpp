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
#include "core/types/data_model.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "link/object_format_schema.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

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
    // `unsigned float` / `short long` / `long double` are NOT rows in the
    // typeSpecifiers table — they reject by ABSENCE, with EXACTLY ONE
    // diagnostic (the precise S_InvalidTypeSpecifierCombination; the
    // generic outer S_UnknownType is suppressed for the same resolution).
    for (auto const* src : {
             "int main() { unsigned float x; return 0; }\n",
             "int main() { short long x; return 0; }\n",
             "int main() { long double x; return 0; }\n",
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

TEST(Fc3WidthSemantics, NarrowingLongLiteralRejectsOnLlp64AcceptsOnLp64) {
    // 2147483648l passed to a `long` parameter: LP64 → the l-ladder's
    // first candidate `long` (I64) holds it → I64 arg into I64 param,
    // clean. LLP64 → `long` is I32 (does not hold 2^31) → the literal
    // climbs to `long long` (I64) → an I64 arg into the I32 `long`
    // param is a narrowing mismatch → S_TypeMismatch (the call-arg
    // assignability check). The dataModel DIFFERENTIAL is the witness —
    // the SAME source, two verdicts.
    char const* src =
        "long pick(long v) { return v; }\n"
        "int main() { long r; r = pick(2147483648l); return 0; }\n";
    auto lp = analyzeCSubset(src, DataModel::Lp64);
    EXPECT_EQ(countCode(lp.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_FALSE(lp.hasErrors());
    auto llp = analyzeCSubset(src, DataModel::Llp64);
    EXPECT_EQ(countCode(llp.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u);
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
// Pin the literal's ladder type through the CALL-ARG assignability check
// (the semantic surface that consumes literal types — c-subset declares
// no decl-init child, so the call check is the strict observable):
// passing the literal to a SAME-kind parameter must be clean, and to the
// CROSS-SIGNEDNESS sibling at the same width must MISMATCH (assignability
// is same-signedness rank-based) — proving the ladder picked the exact
// (width × signedness) type, not merely "something assignable".
void expectLiteralTypes(std::string const& literal, TypeKind want,
                        DataModel dm = DataModel::Lp64) {
    char const* binder = nullptr;
    switch (want) {
        case TypeKind::I32: binder = "int";                 break;
        case TypeKind::I64: binder = "long long";           break;
        case TypeKind::U32: binder = "unsigned int";        break;
        case TypeKind::U64: binder = "unsigned long long";  break;
        default: FAIL() << "unsupported want kind"; return;
    }
    auto const probe = [&](char const* paramType) {
        return std::string{"int take("} + paramType
            + " v) { return 0; }\n"
              "int main() { int r; r = take(" + literal + "); return 0; }\n";
    };
    auto clean = analyzeCSubset(probe(binder), dm);
    EXPECT_EQ(countCode(clean.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << literal << " should pass cleanly into a " << binder << " param";
    char const* crossBinder = nullptr;
    switch (want) {
        case TypeKind::I32: crossBinder = "unsigned int";       break;
        case TypeKind::I64: crossBinder = "unsigned long long"; break;
        case TypeKind::U32: crossBinder = "int";                break;
        case TypeKind::U64: crossBinder = "long long";          break;
        default: return;
    }
    auto cross = analyzeCSubset(probe(crossBinder), dm);
    EXPECT_EQ(countCode(cross.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
        << literal << " must NOT pass into a " << crossBinder << " param";
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

TEST(Fc3WidthSemantics, FloatSuffixedLiteralStaysF64ThisCycle) {
    // `1.5f` deliberately types F64 in c1 (the f→F32 literal typing
    // rides the F32 arithmetic surface — D-CSUBSET-F32-CODEGEN); pin the
    // current behavior so a future flip is a CONSCIOUS config change.
    auto m = analyzeCSubset("int main() { double d; d = 1.5f; return 0; }\n");
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_FALSE(m.hasErrors());
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

TEST(Fc3LoaderRejects, UnknownMixedSignednessVerbRejects) {
    auto doc = loadShippedCSubsetJson();
    doc["semantics"]["arithmeticConversions"]["mixedSignedness"] =
        "rank-prefer-signed";
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
        auto d = ffi::readShippedLibDescriptor(desc, interner, registry, rep, dm);
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
