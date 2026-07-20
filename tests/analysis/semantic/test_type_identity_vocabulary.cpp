// D-LANG-TYPE-IDENTITY-VOCABULARY — type IDENTITY comes from the language
// VOCABULARY entry; REPRESENTATION comes from the target core. The two axes are
// INDEPENDENT, and identity is NEVER derived from representation.
//
// Before this split the interner keyed a primitive's identity on its `TypeKind`
// ALONE, so any target axis that gave two DISTINCT named C types the same core
// COLLAPSED them into ONE TypeId. Every pin below is RED-on-disable against that
// collapse:
//
//   * `_Generic(int:, long:)`              — S002B ambiguous under LLP64.
//   * `_Generic(long:, long long:)`        — S002B under LP64 (UNIVERSAL, not
//                                            target-gated).
//   * `_Generic(float:, double:, long double:)`
//                                          — S002B on an f64 long-double axis
//                                            (pe64 / apple-arm64) while CLEAN on
//                                            x87 — the same source answered
//                                            differently by TARGET FORMAT.
//   * unsigned siblings of the above.
//   * `int *p; long *q = p;`               — SILENTLY ACCEPTED under LLP64. The
//                                            collapse was not merely fail-loud;
//                                            it accepted invalid C with no
//                                            constraint diagnostic at all.
//
// The pins in the other direction are just as load-bearing — a naive split
// breaks them: two `long` declarations must still DEDUP to one TypeId, anonymous
// primitives must stay the anonymous representative of their core (a promoted
// `char + char` must still match a declared `int`), the four implicit
// conversions between same-representation distinct types must stay CLEAN in both
// directions, and `_Generic` must keep selecting the RIGHT branch (asserted by
// the selected arm's distinct result TYPE, never by "it compiled").

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/data_model.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SemanticModel analyzeCSubset(
    std::string src, DataModel dm,
    LongDoubleFormat ldf = LongDoubleFormat::X87_80) {
    auto cu = buildShippedUnit("c-subset", {std::move(src)});
    assertNoBuilderErrors(*cu);
    return analyze(cu, dm, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                   ldf);
}

// The resolved TypeId of the named symbol (fails the test if absent/untyped).
[[nodiscard]] TypeId typeOf(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i) {
        if (m.symbols()[i].name != name) continue;
        if (!m.symbols()[i].type.valid()) {
            ADD_FAILURE() << "symbol '" << name << "' has no resolved type";
            return InvalidType;
        }
        return m.symbols()[i].type;
    }
    ADD_FAILURE() << "symbol '" << name << "' not found";
    return InvalidType;
}

[[nodiscard]] TypeKind kindOf(SemanticModel const& m, std::string_view name) {
    TypeId const t = typeOf(m, name);
    return t.valid() ? m.lattice().interner().kind(t) : TypeKind::Void;
}

// The vocabulary tag on the named symbol's type ("" when anonymous).
[[nodiscard]] std::string vocabOf(SemanticModel const& m, std::string_view name) {
    TypeId const t = typeOf(m, name);
    return t.valid() ? std::string{m.lattice().interner().vocabularyName(t)}
                     : std::string{};
}

// Every pin asserts BOTH failure modes are absent — a naive "did it compile"
// check would pass while the selection silently fell through to `default`.
void expectGenericClean(SemanticModel const& m) {
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_GenericSelectionAmbiguous), 0u)
        << "the associations name DISTINCT types — never ambiguous";
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_GenericSelectionNoMatch), 0u)
        << "the controlling type must match exactly one association";
    EXPECT_FALSE(m.hasErrors());
}

// The SOURCE TEXT of each `_Generic`'s SELECTED result expression, in source
// order. This is the direct observation of which association won — far stricter
// than watching a downstream type, which can coincide across arms.
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

// The REAL shipped-descriptor directory (`src/dss-config/shippedLibs`), so a
// test can drive `#include <stdint.h>` against the descriptors that actually
// ship rather than a scratch stand-in — the point of the size_t/uint64_t pins is
// that the SHIPPED spelling resolves to the right vocabulary entry.
[[nodiscard]] std::filesystem::path findShippedLibDir() {
    return findShippedSourceConfig().parent_path().parent_path() / "shippedLibs";
}

// Analyze through the FULL front end with the shipped descriptors on the system
// include path — `analyzeCSubset` above never runs `#include`.
[[nodiscard]] SemanticModel analyzeWithShippedHeaders(
    std::string src, DataModel dm, ObjectFormatKind fmt, std::string_view arch) {
    auto schema = loadShippedSchema("c-subset");
    UnitBuilder builder{schema};
    builder.addSystemDir(findShippedLibDir());
    builder.setActiveFormat(fmt);
    builder.addInMemory(std::move(src), "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    return analyze(cu, dm, std::nullopt, std::nullopt, fmt, arch);
}

// The (dataModel, objectFormat, arch) triples the shipped descriptors are
// actually built for — LP64 rides elf, LLP64 rides pe (the data model is a
// property of the FORMAT, never of the CPU).
struct ModelAxis {
    DataModel        dm;
    ObjectFormatKind fmt;
    char const*      arch;
    char const*      label;
    // The vocabulary entry C's `size_t` / `ptrdiff_t` IS on this model.
    char const*      sizeName;
    char const*      ptrdiffName;
};
constexpr ModelAxis kLp64{DataModel::Lp64, ObjectFormatKind::Elf, "x86_64",
                          "LP64/elf", "unsigned long", "long"};
constexpr ModelAxis kLlp64{DataModel::Llp64, ObjectFormatKind::Pe, "x86_64",
                           "LLP64/pe", "unsigned long long", "long long"};

[[nodiscard]] nlohmann::json loadShippedCSubsetJson() {
    std::ifstream in{findShippedSourceConfig(), std::ios::binary};
    EXPECT_TRUE(in.good());
    return nlohmann::json::parse(in);
}

// Perturb the shipped `typeSpecifiers` rows and report whether the schema still
// loads. Every loader pin below proves a knob CANNOT lie.
[[nodiscard]] bool typeSpecifiersLoad(std::function<void(nlohmann::json&)> mutate) {
    nlohmann::json doc = loadShippedCSubsetJson();
    mutate(doc["semantics"]["typeSpecifiers"]);
    return GrammarSchema::loadFromText(doc.dump(), "<vocab-perturbed>").has_value();
}

// The index of the first shipped row whose token multiset is exactly `tokens`.
[[nodiscard]] std::size_t rowIndexFor(nlohmann::json const& rows,
                                      std::vector<std::string> const& tokens) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].value("tokens", nlohmann::json::array()) == tokens) return i;
    }
    ADD_FAILURE() << "no typeSpecifiers row matches the requested token multiset";
    return 0;
}

} // namespace

// ── The confirmed `_Generic` collapses, one pin per instance ────────────────

// LLP64 gives `long` and `int` the SAME core (I32). They are still two types.
TEST(TypeIdentityVocabulary, GenericIntVsLongDistinctUnderLlp64) {
    // Each association's result expression is a DISTINCT literal, so the winner
    // is read back verbatim — a pin that only checked "no diagnostics" would
    // pass on a wrong selection.
    std::string const src =
        "int f(long x){ return _Generic((x), int: 11, long: 22, default: 33); }\n";
    auto m = analyzeCSubset(src, DataModel::Llp64);
    expectGenericClean(m);
    EXPECT_EQ(selectedGenericArms(m), (std::vector<std::string>{"22"}))
        << "the `long:` arm must win — 11 means it matched `int:`, 33 means it "
           "fell through to `default:`";
}

// The control: the SAME source on LP64, where the cores already differ.
TEST(TypeIdentityVocabulary, GenericIntVsLongDistinctUnderLp64) {
    std::string const src =
        "int f(long x){ return _Generic((x), int: 11, long: 22, default: 33); }\n";
    auto m = analyzeCSubset(src, DataModel::Lp64);
    expectGenericClean(m);
    EXPECT_EQ(selectedGenericArms(m), (std::vector<std::string>{"22"}));
}

// UNIVERSAL (not target-gated): `long` and `long long` are both I64 on LP64.
TEST(TypeIdentityVocabulary, GenericLongVsLongLongDistinctUnderLp64) {
    std::string const src =
        "int f(long a, long long b){\n"
        "  return _Generic((a), long: 11, long long: 22, default: 33)\n"
        "       + _Generic((b), long: 44, long long: 55, default: 66); }\n";
    for (DataModel const dm : {DataModel::Lp64, DataModel::Llp64}) {
        auto m = analyzeCSubset(src, dm);
        SCOPED_TRACE(dm == DataModel::Lp64 ? "LP64" : "LLP64");
        expectGenericClean(m);
        EXPECT_EQ(selectedGenericArms(m), (std::vector<std::string>{"11", "55"}))
            << "`long` takes the long arm, `long long` takes its own — on BOTH "
               "models (this collapse was never target-gated)";
    }
}

TEST(TypeIdentityVocabulary, GenericUnsignedLongVsUnsignedIntDistinctUnderLlp64) {
    std::string const src =
        "int f(unsigned int a, unsigned long b, unsigned long long c){\n"
        "  return _Generic((a), unsigned int: 11, unsigned long: 22,\n"
        "                       unsigned long long: 33, default: 44)\n"
        "       + _Generic((b), unsigned int: 55, unsigned long: 66,\n"
        "                       unsigned long long: 77, default: 88)\n"
        "       + _Generic((c), unsigned int: 91, unsigned long: 92,\n"
        "                       unsigned long long: 93, default: 94); }\n";
    for (DataModel const dm : {DataModel::Lp64, DataModel::Llp64}) {
        auto m = analyzeCSubset(src, dm);
        SCOPED_TRACE(dm == DataModel::Lp64 ? "LP64" : "LLP64");
        expectGenericClean(m);
        EXPECT_EQ(selectedGenericArms(m),
                  (std::vector<std::string>{"11", "66", "93"}))
            << "all three unsigned entries are distinct types on BOTH models — "
               "LLP64 collapses unsigned long onto unsigned int's core, LP64 "
               "onto unsigned long long's; neither may collapse its IDENTITY";
    }
}

// The float axis: on an f64 long-double axis `long double` IS F64, exactly like
// `double`. The pre-split engine answered this source DIFFERENTLY depending on
// the TARGET OBJECT FORMAT — clean on elf64 (x87), ambiguous on pe64/macho-arm64.
TEST(TypeIdentityVocabulary, GenericLongDoubleDistinctOnEveryAxis) {
    struct Row { LongDoubleFormat axis; char const* label; };
    for (Row const row : {Row{LongDoubleFormat::F64,     "f64"},
                          Row{LongDoubleFormat::X87_80,  "x87-80"},
                          Row{LongDoubleFormat::Ieee128, "ieee128"}}) {
        std::string const src =
            "int f(float a, double b, long double c){\n"
            "  return _Generic((a), float: 11, double: 22, long double: 33)\n"
            "       + _Generic((b), float: 44, double: 55, long double: 66)\n"
            "       + _Generic((c), float: 77, double: 88, long double: 99); }\n";
        auto m = analyzeCSubset(src, DataModel::Lp64, row.axis);
        SCOPED_TRACE(row.label);
        expectGenericClean(m);
        EXPECT_EQ(selectedGenericArms(m),
                  (std::vector<std::string>{"11", "55", "99"}))
            << "float / double / long double each select their OWN association "
               "on EVERY axis — the answer must not depend on the target format";
    }
}

// ── Same-name identity: dedup must survive the split ───────────────────────

TEST(TypeIdentityVocabulary, TwoLongDeclarationsShareOneTypeId) {
    // Every spelling of `long` (bare / `long int` / `signed long` / `signed long
    // int`) is ONE vocabulary entry, so all four must intern to ONE TypeId —
    // otherwise the split would have multiplied types instead of separating them.
    std::string const src =
        "int f(void){ long a; long int b; signed long c; signed long int d;\n"
        "  a = 0; b = 0; c = 0; d = 0; return 0; }\n";
    for (DataModel const dm : {DataModel::Lp64, DataModel::Llp64}) {
        auto m = analyzeCSubset(src, dm);
        ASSERT_FALSE(m.hasErrors());
        TypeId const ta = typeOf(m, "a");
        EXPECT_EQ(typeOf(m, "b").v, ta.v);
        EXPECT_EQ(typeOf(m, "c").v, ta.v);
        EXPECT_EQ(typeOf(m, "d").v, ta.v);
        EXPECT_EQ(vocabOf(m, "a"), "long");
    }
}

// ── Anonymous primitives stay anonymous ────────────────────────────────────

TEST(TypeIdentityVocabulary, PromotedCharMatchesDeclaredInt) {
    // Integer promotion re-mints an ANONYMOUS `int`. `int` must therefore stay
    // UNNAMED — naming it would make a promoted `char + char` a different type
    // from a declared `int`, which `_Generic` would then fail to match.
    std::string const src =
        "int f(char c1, char c2){ int i = 0;\n"
        "  auto s = c1 + c2;\n"
        "  return i + _Generic((s), int: 11, long: 22, default: 33); }\n";
    for (DataModel const dm : {DataModel::Lp64, DataModel::Llp64}) {
        auto m = analyzeCSubset(src, dm);
        SCOPED_TRACE(dm == DataModel::Lp64 ? "LP64" : "LLP64");
        expectGenericClean(m);
        EXPECT_EQ(typeOf(m, "s").v, typeOf(m, "i").v)
            << "a promoted char+char IS the declared `int` type, same TypeId";
        EXPECT_EQ(vocabOf(m, "i"), "")
            << "`int` must remain the ANONYMOUS representative of I32";
        EXPECT_EQ(selectedGenericArms(m), (std::vector<std::string>{"11"}))
            << "and the promoted sum still matches the `int:` association";
    }
}

TEST(TypeIdentityVocabulary, UnnamedVocabularyEntriesStayAnonymous) {
    std::string const src =
        "int f(int a, short b, unsigned int c, unsigned short d,\n"
        "      float e, double g, char h, bool k){ return 0; }\n";
    auto m = analyzeCSubset(src, DataModel::Lp64);
    ASSERT_FALSE(m.hasErrors());
    for (char const* name : {"a", "b", "c", "d", "e", "g", "h", "k"}) {
        EXPECT_EQ(vocabOf(m, name), "")
            << "'" << name << "' must carry NO vocabulary tag — naming it would "
               "break promotion / enum-underlying synthesis, which re-mint the "
               "anonymous primitive of that kind";
    }
    // ... while the entries that CAN collide all carry one.
    auto named = analyzeCSubset(
        "int f(long a, unsigned long b, long long c, unsigned long long d,\n"
        "      long double e){ return 0; }\n",
        DataModel::Lp64);
    ASSERT_FALSE(named.hasErrors());
    EXPECT_EQ(vocabOf(named, "a"), "long");
    EXPECT_EQ(vocabOf(named, "b"), "unsigned long");
    EXPECT_EQ(vocabOf(named, "c"), "long long");
    EXPECT_EQ(vocabOf(named, "d"), "unsigned long long");
    EXPECT_EQ(vocabOf(named, "e"), "long double");
}

// ── The four regression pins a naive split breaks ──────────────────────────

TEST(TypeIdentityVocabulary, SameRepresentationConversionsStayClean) {
    // "Converts cleanly" is only EVIDENCE when the two sides are genuinely
    // DISTINCT types at ONE representation — pre-change they were the same
    // TypeId, so a no-error check passed for the wrong reason. Every pair below
    // asserts that precondition explicitly before claiming anything about the
    // conversion.
    auto const expectSplitPair = [](SemanticModel const& m, char const* lhs,
                                    char const* rhs) {
        auto const& in = m.lattice().interner();
        TypeId const a = typeOf(m, lhs);
        TypeId const b = typeOf(m, rhs);
        ASSERT_TRUE(a.valid() && b.valid());
        EXPECT_NE(a.v, b.v)
            << "'" << lhs << "' and '" << rhs << "' must be DISTINCT TypeIds — "
               "otherwise the conversion under test does not exist";
        EXPECT_TRUE(in.sameRepresentation(a, b))
            << "... at ONE representation, which is why the conversion must be "
               "clean AND must cost nothing";
    };

    // Distinct types that share a representation still convert IMPLICITLY, in
    // BOTH directions — the conversion is C 6.3.1.3p1's identity, not an error.
    auto llp = analyzeCSubset(
        "int f(int i, long l){ long a = i; int b = l; return a == b; }\n",
        DataModel::Llp64);
    EXPECT_FALSE(llp.hasErrors())
        << "LLP64 int<->long (both I32) must convert cleanly both ways";
    expectSplitPair(llp, "i", "l");
    // ... and the DESTINATIONS keep their own identity (a conversion that
    // silently retyped `a` as `int` would also be "clean").
    EXPECT_EQ(vocabOf(llp, "a"), "long");
    EXPECT_EQ(vocabOf(llp, "b"), "");

    auto lp = analyzeCSubset(
        "int f(long l, long long q){ long long a = l; long b = q;\n"
        "  return a == b; }\n",
        DataModel::Lp64);
    EXPECT_FALSE(lp.hasErrors())
        << "LP64 long<->long long (both I64) must convert cleanly both ways";
    expectSplitPair(lp, "l", "q");
    EXPECT_EQ(vocabOf(lp, "a"), "long long");
    EXPECT_EQ(vocabOf(lp, "b"), "long");

    // The float pair on the f64 axis — where `long double` and `double` share a
    // representation and were ONE TypeId before the split. BOTH directions.
    auto f64 = analyzeCSubset(
        "int f(double d, long double ld){ double a = ld; long double b = d;\n"
        "  return a == 0.0 && b == 0.0L; }\n",
        DataModel::Lp64, LongDoubleFormat::F64);
    EXPECT_FALSE(f64.hasErrors())
        << "f64 axis: double <-> long double must convert cleanly BOTH ways";
    expectSplitPair(f64, "d", "ld");
    EXPECT_EQ(vocabOf(f64, "a"), "");
    EXPECT_EQ(vocabOf(f64, "b"), "long double");

    // On a WIDER long-double axis the two have genuinely different
    // representations, so only the WIDENING direction is an implicit conversion
    // (`isAssignable`'s float rule is `floatRank(rhs) <= floatRank(lhs)`, a
    // PRE-EXISTING, purely kind-keyed rule this change does not touch).
    for (LongDoubleFormat const axis : {LongDoubleFormat::X87_80,
                                        LongDoubleFormat::Ieee128}) {
        SCOPED_TRACE(static_cast<int>(axis));
        auto widen = analyzeCSubset(
            "int f(double d){ long double b = d; return b == 0.0L; }\n",
            DataModel::Lp64, axis);
        EXPECT_FALSE(widen.hasErrors())
            << "double -> long double widening stays clean on a wider axis";
        auto narrow = analyzeCSubset(
            "int f(long double ld){ double a = ld; return a == 0.0; }\n",
            DataModel::Lp64, axis);
        EXPECT_EQ(countCode(narrow.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
            << "and the NARROWING direction keeps its pre-existing rejection — "
               "a rank rule, not an identity rule";
    }
}

// ── The CORRECT tightening (defect 5) ──────────────────────────────────────

TEST(TypeIdentityVocabulary, IncompatiblePointerTypesNowDiagnose) {
    // C requires a constraint diagnostic here. Under the collapse `int` and
    // `long` were ONE TypeId on LLP64, so this compiled SILENTLY — the collapse
    // did not merely fail loud, it ACCEPTED invalid code.
    auto llp = analyzeCSubset(
        "int f(void){ int x = 0; int *p = &x; long *q = p; return *q != 0; }\n",
        DataModel::Llp64);
    EXPECT_TRUE(llp.hasErrors())
        << "`long *q = p;` from an `int *` is a C constraint violation";
    EXPECT_EQ(countCode(llp.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u);

    // The same tightening on LP64's OTHER same-representation pair.
    auto lp = analyzeCSubset(
        "int f(void){ long x = 0; long *p = &x; long long *q = p;\n"
        "  return *q != 0; }\n",
        DataModel::Lp64);
    EXPECT_TRUE(lp.hasErrors())
        << "`long long *q = (long*)…` is a C constraint violation on LP64 too";
    EXPECT_EQ(countCode(lp.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u);

    // ... and the matching-type control stays CLEAN (proves the tightening is
    // not a blanket pointer reject).
    auto ok = analyzeCSubset(
        "int f(void){ long x = 0; long *p = &x; long *q = p; return *q != 0; }\n",
        DataModel::Llp64);
    EXPECT_FALSE(ok.hasErrors()) << "same-vocabulary pointers stay assignable";
}

TEST(TypeIdentityVocabulary, CharFamilyStaysThreeDistinctTypes) {
    // Pre-existing behavior that must be PRESERVED: char / signed char /
    // unsigned char are three distinct CORES (Char/I8/U8), so their pointers
    // were already incompatible. Unrelated to the vocabulary split — pinned so a
    // future identity change cannot quietly merge them.
    auto m = analyzeCSubset(
        "int f(void){ char c = 0; char *p = &c; signed char *q = p;\n"
        "  return *q != 0; }\n",
        DataModel::Lp64);
    EXPECT_TRUE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u);
}

// ── Conversion RANK is keyed on the NAME, not the width ────────────────────

TEST(TypeIdentityVocabulary, ArithmeticResultTakesHigherRankedVocabularyName) {
    // C 6.3.1.1 defines rank by type NAME. With a width-derived rank
    // `someLong + someLongLong` on LP64 (both I64) yields the WRONG NAME —
    // observable exactly here.
    std::string const lpSrc =
        "int f(long a, long long b){ auto s = a + b;\n"
        "  return _Generic((s), long: 11, long long: 22, default: 33); }\n";
    auto lp = analyzeCSubset(lpSrc, DataModel::Lp64);
    expectGenericClean(lp);
    EXPECT_EQ(vocabOf(lp, "s"), "long long")
        << "long + long long is `long long` (rank 4 > 3), even at equal width";
    EXPECT_EQ(selectedGenericArms(lp), (std::vector<std::string>{"22"}));

    // The int/long pair at equal width under LLP64.
    std::string const llpSrc =
        "int f(int a, long b){ auto s = a + b;\n"
        "  return _Generic((s), int: 11, long: 22, default: 33); }\n";
    auto llp = analyzeCSubset(llpSrc, DataModel::Llp64);
    expectGenericClean(llp);
    EXPECT_EQ(vocabOf(llp, "s"), "long")
        << "int + long is `long` (rank 3 > int's 0), even at equal width";
    EXPECT_EQ(selectedGenericArms(llp), (std::vector<std::string>{"22"}));

    // The float axis sibling: `double + long double` is `long double` even where
    // both are F64.
    std::string const fSrc =
        "int f(double a, long double b){ auto s = a + b;\n"
        "  return _Generic((s), double: 11, long double: 22, default: 33); }\n";
    auto f64 = analyzeCSubset(fSrc, DataModel::Lp64, LongDoubleFormat::F64);
    expectGenericClean(f64);
    EXPECT_EQ(vocabOf(f64, "s"), "long double");
    EXPECT_EQ(selectedGenericArms(f64), (std::vector<std::string>{"22"}));
}

TEST(TypeIdentityVocabulary, SameVocabularySumKeepsItsName) {
    // `long + long` is `long`, not the anonymous primitive of its core — the
    // usual-arithmetic-conversions result must PRESERVE the winner's identity
    // rather than re-synthesize an unnamed type.
    std::string const src =
        "int f(long a, long b){ auto s = a + b;\n"
        "  return _Generic((s), long: 11, default: 22); }\n";
    auto m = analyzeCSubset(src, DataModel::Lp64);
    expectGenericClean(m);
    EXPECT_EQ(vocabOf(m, "s"), "long");
    EXPECT_EQ(selectedGenericArms(m), (std::vector<std::string>{"11"}));
}

// ── The ENGINE-SYNTHESIZED standard types (`semantics.synthesizedTypes`) ───
//
// C says `sizeof`/`_Alignof` yield `size_t` and a same-pointee `p - q` yields
// `ptrdiff_t`, and BOTH are ALIASES of a standard NAMED type whose spelling is
// DATA-MODEL-dependent. Minting a bare ANONYMOUS 64-bit primitive (what the
// engine did) produces a THIRD type matching NEITHER named entry.
//
// RED-ON-DISABLE, and note the `default:` arm: without the fix the selection
// does not FAIL, it silently lands on `default:` — the wrong-arm defect, not a
// diagnostic. Each association's result is a distinct literal, so the pin reads
// the winner back verbatim rather than inferring it.

TEST(TypeIdentityVocabulary, SizeofYieldsTheDeclaredSizeTVocabularyEntry) {
    std::string const src =
        "int f(void){ int x = 0;\n"
        "  return _Generic(sizeof(int), unsigned long: 1, unsigned long long: 2,\n"
        "                  default: 0)\n"
        "       + _Generic(sizeof x,    unsigned long: 10, unsigned long long: 20,\n"
        "                  default: 0)\n"
        "       + _Generic(_Alignof(int), unsigned long: 100,\n"
        "                  unsigned long long: 200, default: 0); }\n";
    // LP64: size_t IS `unsigned long`.
    auto lp = analyzeCSubset(src, DataModel::Lp64);
    expectGenericClean(lp);
    EXPECT_EQ(selectedGenericArms(lp), (std::vector<std::string>{"1", "10", "100"}))
        << "LP64 size_t IS `unsigned long` — a `default:` hit here is the SILENT "
           "wrong-arm selection an anonymous U64 causes";
    // LLP64: the SAME 64-bit representation, the OTHER vocabulary entry.
    auto llp = analyzeCSubset(src, DataModel::Llp64);
    expectGenericClean(llp);
    EXPECT_EQ(selectedGenericArms(llp), (std::vector<std::string>{"2", "20", "200"}))
        << "LLP64 size_t IS `unsigned long long` — same core, different NAME, so "
           "identity cannot be derived from representation";
}

TEST(TypeIdentityVocabulary, PointerDifferenceYieldsTheDeclaredPtrdiffTEntry) {
    std::string const src =
        "int f(int *a, int *b){\n"
        "  return _Generic((a - b), long: 1, long long: 2, default: 0); }\n";
    auto lp = analyzeCSubset(src, DataModel::Lp64);
    expectGenericClean(lp);
    EXPECT_EQ(selectedGenericArms(lp), (std::vector<std::string>{"1"}))
        << "LP64 ptrdiff_t IS `long`";
    auto llp = analyzeCSubset(src, DataModel::Llp64);
    expectGenericClean(llp);
    EXPECT_EQ(selectedGenericArms(llp), (std::vector<std::string>{"2"}))
        << "LLP64 ptrdiff_t IS `long long`";
}

// `sizeof` also has to land inside the SHIPPED <stdbit.h> 5-way association SET
// — the real-world shape the regression was found in. NO `default:` arm here, so
// an anonymous U64 is S_GenericSelectionNoMatch (a hard failure) rather than a
// silent fall-through; the arm results are distinguishable so the WINNER is read
// back verbatim, not inferred.
TEST(TypeIdentityVocabulary, StdbitFiveWayAssociationSetAcceptsASizeofOperand) {
    std::string const src =
        "int f(int x){ return _Generic((sizeof x),\n"
        "    unsigned char: 1, unsigned short: 2, unsigned int: 3,\n"
        "    unsigned long: 4, unsigned long long: 5); }\n";
    auto lp = analyzeCSubset(src, DataModel::Lp64);
    expectGenericClean(lp);
    EXPECT_EQ(selectedGenericArms(lp), (std::vector<std::string>{"4"}))
        << "LP64 size_t IS `unsigned long`";
    auto llp = analyzeCSubset(src, DataModel::Llp64);
    expectGenericClean(llp);
    EXPECT_EQ(selectedGenericArms(llp), (std::vector<std::string>{"5"}))
        << "LLP64 size_t IS `unsigned long long`";
}

// The END-TO-END form of the SAME defect, through the REAL shipped headers: the
// `stdc_count_ones` macro expands to the 5-way `_Generic`, and its operand is a
// shipped `uint64_t` / a `sizeof`. Both were S_GenericSelectionNoMatch.
TEST(TypeIdentityVocabulary, ShippedStdbitGenericAcceptsUint64AndSizeof) {
    for (ModelAxis const ax : {kLp64, kLlp64}) {
        SCOPED_TRACE(ax.label);
        auto m = analyzeWithShippedHeaders(
            "#include <stdbit.h>\n"
            "#include <stdint.h>\n"
            "#include <stddef.h>\n"
            "unsigned f(uint64_t x, size_t n, uintmax_t m){\n"
            "  return stdc_count_ones(x) + stdc_count_ones(sizeof x)\n"
            "       + stdc_count_ones(n) + stdc_count_ones(m); }\n",
            ax.dm, ax.fmt, ax.arch);
        expectGenericClean(m);
        // "No error" alone would be green with the identity split reverted only
        // because the collapsed 4-way still MATCHED something. Read the WINNER of
        // each of the four selections back: every operand here is a 64-bit
        // unsigned alias, so on LP64 all four must take `unsigned long:` and on
        // LLP64 all four must take `unsigned long long:` — and the shipped macro
        // routes those two arms to DIFFERENT builtins per format, so the selected
        // expression text names the arm outright.
        auto const arms = selectedGenericArms(m);
        ASSERT_EQ(arms.size(), 4u)
            << "the macro must expand to exactly four generic selections";
        // Both winners bottom out in the 64-bit builtin, by DIFFERENT routes:
        // on LP64 the `unsigned long:` arm goes through the per-format
        // `stdc_count_ones_ul` variant (elf → `_ull`); on LLP64 the operands are
        // `unsigned long long` and take that arm directly. Either way a `_ui`
        // here would mean a 64-bit operand matched a 32-BIT association — the
        // silent wrong-arm outcome the collapse produced.
        for (auto const& a : arms) {
            EXPECT_NE(a.find("__builtin_stdc_count_ones_ull"), std::string::npos)
                << "selected arm was `" << a << "`, expected the 64-bit builtin "
                   "— a 32-bit `_ui` here means the operand matched the WRONG "
                   "association";
        }
    }
    // ... and the direction that proves the arms are really distinguishable: a
    // 32-bit `unsigned int` operand takes the `unsigned int:` arm (the 32-bit
    // builtin), so the four above are not just "whatever the macro always picks".
    for (ModelAxis const ax : {kLp64, kLlp64}) {
        SCOPED_TRACE(ax.label);
        auto m = analyzeWithShippedHeaders(
            "#include <stdbit.h>\n"
            "unsigned f(unsigned int u){ return stdc_count_ones(u); }\n",
            ax.dm, ax.fmt, ax.arch);
        expectGenericClean(m);
        auto const arms = selectedGenericArms(m);
        ASSERT_EQ(arms.size(), 1u);
        EXPECT_NE(arms[0].find("__builtin_stdc_count_ones_ui"), std::string::npos)
            << "selected arm was `" << arms[0] << "`";
    }
}

// ── The SHIPPED `<stdint.h>` / `<stddef.h>` aliases ───────────────────────
//
// C defines `uint64_t` / `size_t` / `uintptr_t` as ALIASES of a standard NAMED
// type. Both descriptors used to spell them as a bare `u64`, which is the
// ANONYMOUS representative — a THIRD type matching neither `unsigned long:` nor
// `unsigned long long:`. RED-ON-DISABLE through the REAL shipped descriptors.
TEST(TypeIdentityVocabulary, ShippedFixedWidthAliasesAreTheNamedStandardTypes) {
    for (ModelAxis const ax : {kLp64, kLlp64}) {
        SCOPED_TRACE(ax.label);
        std::string const src =
            "#include <stdint.h>\n"
            "#include <stddef.h>\n"
            "int f(uint64_t a, size_t b, uintptr_t c, intmax_t d, ptrdiff_t e){\n"
            "  return _Generic((a), unsigned long: 1, unsigned long long: 2,\n"
            "                  default: 0)\n"
            "       + _Generic((b), unsigned long: 10, unsigned long long: 20,\n"
            "                  default: 0)\n"
            "       + _Generic((c), unsigned long: 100, unsigned long long: 200,\n"
            "                  default: 0)\n"
            "       + _Generic((d), long: 1000, long long: 2000, default: 0)\n"
            "       + _Generic((e), long: 10000, long long: 20000, default: 0); }\n";
        auto m = analyzeWithShippedHeaders(src, ax.dm, ax.fmt, ax.arch);
        expectGenericClean(m);
        bool const lp = (ax.dm == DataModel::Lp64);
        EXPECT_EQ(selectedGenericArms(m),
                  (std::vector<std::string>{lp ? "1" : "2",
                                            lp ? "10" : "20",
                                            lp ? "100" : "200",
                                            lp ? "1000" : "2000",
                                            lp ? "10000" : "20000"}))
            << "each shipped alias must resolve to the data model's NAMED entry";
    }
}

// The pointer direction — strict TypeId identity, so a wrongly-anonymous alias
// shows up as a bare S_TypeMismatch on code C says is correct.
TEST(TypeIdentityVocabulary, ShippedAliasPointersMatchTheirNamedStandardType) {
    for (ModelAxis const ax : {kLp64, kLlp64}) {
        SCOPED_TRACE(ax.label);
        std::string const ok =
            // The pointer flows through an INTERMEDIATE variable of the alias's
            // own pointer type: a direct `T *p = &x;` initializer is not
            // pointee-checked today (a pre-existing gap, unrelated to identity),
            // so it would make this pin vacuous in BOTH directions.
            std::string{"#include <stdint.h>\n#include <stddef.h>\n"}
            + "int f(void){ uint64_t x = 0; uint64_t *px = &x;\n"
            + "  " + ax.sizeName + " *p = px;\n"
            + "  size_t s = 0; size_t *ps = &s;\n"
            + "  " + ax.sizeName + " *q = ps;\n"
            + "  ptrdiff_t d = 0; ptrdiff_t *pd = &d;\n"
            + "  " + ax.ptrdiffName + " *r = pd;\n"
            + "  return (*p != 0) + (*q != 0) + (*r != 0); }\n";
        auto m = analyzeWithShippedHeaders(ok, ax.dm, ax.fmt, ax.arch);
        EXPECT_FALSE(m.hasErrors())
            << "the shipped alias IS that named type on this data model";
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);

        // ... and the OTHER model's name is a genuinely different type, so it
        // must still DIAGNOSE (proving the pin above is not a blanket accept).
        std::string const wrongName =
            ax.dm == DataModel::Lp64 ? "unsigned long long" : "unsigned long";
        std::string const bad =
            std::string{"#include <stdint.h>\n"}
            + "int f(void){ uint64_t x = 0; uint64_t *px = &x;\n"
            + "  " + wrongName + " *p = px;\n"
            + "  return *p != 0; }\n";
        auto n = analyzeWithShippedHeaders(bad, ax.dm, ax.fmt, ax.arch);
        EXPECT_EQ(countCode(n.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
            << "the other model's vocabulary entry is a DIFFERENT type here";
    }
}

// The MSVC atomic intrinsic takes a `LONG volatile*` — i.e. `long*`, and Win32
// `LONG` is a 32-bit `long` (LLP64). A fixed anonymous `ptr<i32>` rejected the
// very C type the intrinsic models.
TEST(TypeIdentityVocabulary, InterlockedCompareExchangeTakesALongPointer) {
    auto m = analyzeWithShippedHeaders(
        "int f(void){ long v = 0;\n"
        "  return (int)_InterlockedCompareExchange(&v, 1, 0); }\n",
        kLlp64.dm, kLlp64.fmt, kLlp64.arch);
    EXPECT_FALSE(m.hasErrors())
        << "`&v` on a `long` IS the intrinsic's `LONG volatile*` parameter";
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);

    // "No error" alone would pass PRE-CHANGE too — back then `long*` and `int*`
    // were literally the same TypeId, so ANY 32-bit integer pointer was accepted.
    // Assert the parameter's actual IDENTITY: `ptr<i32 "long">`, not `ptr<i32>`.
    auto const& in = m.lattice().interner();
    TypeId fnTy = InvalidType;
    for (std::size_t i = 1; i < m.symbols().size(); ++i) {
        if (m.symbols()[i].name == "_InterlockedCompareExchange") {
            fnTy = m.symbols()[i].type;
            break;
        }
    }
    ASSERT_TRUE(fnTy.valid()) << "the intrinsic must be injected as a symbol";
    ASSERT_EQ(in.kind(fnTy), TypeKind::FnSig);
    ASSERT_GE(in.fnParams(fnTy).size(), 1u);
    TypeId const p0 = in.fnParams(fnTy)[0];
    ASSERT_EQ(in.kind(p0), TypeKind::Ptr);
    TypeId const pointee = in.operands(p0)[0];
    EXPECT_EQ(in.kind(pointee), TypeKind::I32)
        << "Win32 `LONG` is 32-bit (LLP64)";
    EXPECT_EQ(std::string{in.vocabularyName(pointee)}, "long")
        << "the pointee must be the NAMED `long` entry — an anonymous `i32` "
           "pointee would reject the very `long*` the intrinsic models";
    // And the negative direction, which is what makes the accept above meaningful:
    // an `int*` is NOT the parameter type, so it must DIAGNOSE.
    auto bad = analyzeWithShippedHeaders(
        "int f(void){ int v = 0;\n"
        "  return (int)_InterlockedCompareExchange(&v, 1, 0); }\n",
        kLlp64.dm, kLlp64.fmt, kLlp64.arch);
    EXPECT_TRUE(bad.hasErrors())
        << "`int*` and `long*` are DIFFERENT types even at one representation — "
           "accepting both would be the pre-change blanket collapse";
}

// Win32 `DWORD` IS `unsigned long`, so `LPDWORD` and `unsigned long*` must be
// ONE type — a user may spell the parameter either way.
TEST(TypeIdentityVocabulary, WindowsDwordPointerIsUnsignedLongPointer) {
    auto m = analyzeWithShippedHeaders(
        "#include <windows.h>\n"
        "int f(void){ unsigned long n = 0; unsigned long *pn = &n; LPDWORD p = pn;\n"
        "  DWORD d = 0; DWORD *pd = &d; unsigned long *q = pd;\n"
        "  return (int)((*p != 0) + (*q != 0)); }\n",
        kLlp64.dm, kLlp64.fmt, kLlp64.arch);
    EXPECT_FALSE(m.hasErrors())
        << "DWORD IS `unsigned long`, so LPDWORD IS `unsigned long *`";
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);

    // "No error" alone would pass PRE-CHANGE, when `unsigned long` and
    // `unsigned int` were ONE TypeId under LLP64. Assert the actual identity:
    // LPDWORD and `unsigned long *` must be the SAME TypeId, and DWORD must
    // carry the `unsigned long` vocabulary tag at a 32-bit representation.
    auto const& in = m.lattice().interner();
    TypeId const p  = typeOf(m, "p");
    TypeId const pn = typeOf(m, "pn");
    ASSERT_TRUE(p.valid() && pn.valid());
    EXPECT_EQ(p.v, pn.v) << "LPDWORD IS `unsigned long *` — one TypeId";
    ASSERT_EQ(in.kind(p), TypeKind::Ptr);
    TypeId const pointee = in.operands(p)[0];
    EXPECT_EQ(in.kind(pointee), TypeKind::U32) << "Win32 DWORD is 32-bit";
    EXPECT_EQ(std::string{in.vocabularyName(pointee)}, "unsigned long");
    EXPECT_EQ(vocabOf(m, "d"), "unsigned long")
        << "the DWORD scalar itself carries the entry, not a bare u32";
    // The negative direction: `unsigned int *` is a DIFFERENT type at the SAME
    // representation, so it must DIAGNOSE — proving the accept above is identity,
    // not width.
    auto bad = analyzeWithShippedHeaders(
        "#include <windows.h>\n"
        "int f(void){ DWORD d = 0; DWORD *pd = &d; unsigned int *q = pd;\n"
        "  return (int)(*q != 0); }\n",
        kLlp64.dm, kLlp64.fmt, kLlp64.arch);
    EXPECT_EQ(countCode(bad.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
        << "`unsigned int *` is NOT `DWORD *` — both are u32, and that is "
           "exactly the collapse this change undoes";
}

// ── The f64 float axis: a QUALIFIED named operand still yields the entry ──
//
// End-to-end companion to `TypeRules.UsualArithmeticCommonTypeDropsQualifiers
// OnTheFloatBranch` (which pins C 6.3.2.1p2's UNQUALIFIED requirement directly
// on the rule — the level where it is observable; a C23 `auto` binding strips
// top-level qualifiers, so it cannot witness that half). What THIS pins is that
// the winning VOCABULARY ENTRY survives a mix with a qualified operand at the
// same core — the shape that made `d + vld` produce a `volatile long double`
// common type and, through it, a spurious Bitcast at the assignment (see
// `MirLoweringCSubset.VolatileLongDoubleArithmeticEmitsNoExtraCast`).
TEST(TypeIdentityVocabulary, QualifiedLongDoubleOperandStillYieldsTheEntry) {
    auto const check = [](std::string const& decl, char const* wantVocab) {
        std::string const src =
            "int f(" + decl + " double d){ auto s = d + q; return s == 0.0; }\n";
        auto m = analyzeCSubset(src, DataModel::Lp64, LongDoubleFormat::F64);
        ASSERT_FALSE(m.hasErrors());
        TypeId const t = typeOf(m, "s");
        ASSERT_TRUE(t.valid());
        auto const& in = m.lattice().interner();
        EXPECT_EQ(std::string{in.vocabularyName(t)}, wantVocab)
            << "the higher-RANKED vocabulary entry wins even at an equal core, "
               "and a qualifier on that operand does not lose its identity";
        EXPECT_EQ(in.qualifierBits(t), 0u);
    };
    check("volatile long double q,", "long double");
    check("_Atomic long double q,",  "long double");
    // The control: an UNNAMED float pair keeps behaving exactly as before.
    check("volatile double q,", "");
}

// ── Loader validation: a vocabulary knob can never lie ─────────────────────

TEST(TypeIdentityVocabularyLoader, ShippedConfigLoadsUnperturbed) {
    EXPECT_TRUE(typeSpecifiersLoad([](nlohmann::json&) {}))
        << "fixture precondition: the SHIPPED config must load";
}

TEST(TypeIdentityVocabularyLoader, EmptyNameRejected) {
    EXPECT_FALSE(typeSpecifiersLoad([](nlohmann::json& rows) {
        rows[rowIndexFor(rows, {"IntKeyword"})]["name"] = "";
    })) << "an empty `name` is indistinguishable from the anonymous default — "
           "the loader must reject it rather than silently accept either reading";
}

TEST(TypeIdentityVocabularyLoader, RankWithoutNameRejected) {
    EXPECT_FALSE(typeSpecifiersLoad([](nlohmann::json& rows) {
        rows[rowIndexFor(rows, {"IntKeyword"})]["rank"] = 2;
    })) << "conversion rank is keyed by the vocabulary entry — a rank on an "
           "anonymous row could never be consulted";
}

TEST(TypeIdentityVocabularyLoader, SameNameDivergentCoreRejected) {
    EXPECT_FALSE(typeSpecifiersLoad([](nlohmann::json& rows) {
        rows[rowIndexFor(rows, {"LongKeyword", "IntKeyword"})]["core"] = "I32";
    })) << "one vocabulary entry is one type: `long int` cannot declare a "
           "different core than `long`";
}

TEST(TypeIdentityVocabularyLoader, SameNameDivergentDataModelRejected) {
    EXPECT_FALSE(typeSpecifiersLoad([](nlohmann::json& rows) {
        rows[rowIndexFor(rows, {"SignedKeyword", "LongKeyword"})]
            ["coreByDataModel"]["LLP64"] = "I64";
    })) << "same name, divergent per-data-model override";
}

TEST(TypeIdentityVocabularyLoader, SameNameDivergentLongDoubleAxisRejected) {
    EXPECT_FALSE(typeSpecifiersLoad([](nlohmann::json& rows) {
        rows[rowIndexFor(rows, {"LongKeyword", "DoubleKeyword", "ComplexKeyword"})]
            ["coreByLongDoubleFormat"]["x87-80"] = "F128";
    })) << "same name, divergent per-long-double-format override";
}

TEST(TypeIdentityVocabularyLoader, SameNameDivergentRankRejected) {
    EXPECT_FALSE(typeSpecifiersLoad([](nlohmann::json& rows) {
        rows[rowIndexFor(rows, {"LongKeyword", "LongKeyword", "IntKeyword"})]
            ["rank"] = 9;
    })) << "same name, divergent rank";
}

TEST(TypeIdentityVocabularyLoader, ComplexMayDifferAcrossRowsSharingAName) {
    // The ONE axis deliberately excluded from the consistency check: plain and
    // `_Complex` `long double` legitimately share the name (the `_Complex` row's
    // core IS the shared element type). The shipped config relies on it.
    nlohmann::json doc = loadShippedCSubsetJson();
    auto const& rows = doc["semantics"]["typeSpecifiers"];
    std::size_t named = 0;
    for (auto const& row : rows) {
        if (row.value("name", std::string{}) == "long double") ++named;
    }
    EXPECT_EQ(named, 2u)
        << "the shipped config must name BOTH long double rows (plain + "
           "_Complex) — the check that lets them coexist is only meaningful if "
           "they actually do";
}

// ── Loader validation: `semantics.synthesizedTypes` ────────────────────────

namespace {
// Perturb the shipped `synthesizedTypes` block and report whether the schema
// still loads.
[[nodiscard]] bool synthesizedTypesLoad(std::function<void(nlohmann::json&)> mutate) {
    nlohmann::json doc = loadShippedCSubsetJson();
    mutate(doc["semantics"]["synthesizedTypes"]);
    return GrammarSchema::loadFromText(doc.dump(), "<synth-perturbed>").has_value();
}
} // namespace

TEST(TypeIdentityVocabularyLoader, ShippedSynthesizedTypesLoadUnperturbed) {
    EXPECT_TRUE(synthesizedTypesLoad([](nlohmann::json&) {}));
}

TEST(TypeIdentityVocabularyLoader, SynthesizedTypeUnknownRoleRejected) {
    EXPECT_FALSE(synthesizedTypesLoad([](nlohmann::json& obj) {
        obj["sizeOfButMisspelled"] = obj["sizeof"];
    })) << "the ROLE key is a CLOSED engine vocabulary — a typo'd role would "
           "silently declare nothing and leave the site on its anonymous core";
}

TEST(TypeIdentityVocabularyLoader, SynthesizedTypeUnknownDataModelRejected) {
    EXPECT_FALSE(synthesizedTypesLoad([](nlohmann::json& obj) {
        obj["sizeof"]["LP62"] = "unsigned long";
    })) << "a typo'd data-model key can never match — fail loud";
}

TEST(TypeIdentityVocabularyLoader, SynthesizedTypeMissingDataModelRejected) {
    EXPECT_FALSE(synthesizedTypesLoad([](nlohmann::json& obj) {
        obj["pointerDifference"].erase("LLP64");
    })) << "a DECLARED role must cover EVERY data model — an uncovered one "
           "silently falls back to the anonymous core on exactly that target";
}

TEST(TypeIdentityVocabularyLoader, SynthesizedTypeUnknownVocabularyNameRejected) {
    EXPECT_FALSE(synthesizedTypesLoad([](nlohmann::json& obj) {
        obj["sizeof"]["LP64"] = "unsigned looong";
    })) << "the type NAME is resolved through `typeSpecifiers` at LOAD — an "
           "unresolvable spelling must never silently no-op";
}

// The mechanism is NOT C-specific: any declared vocabulary entry serves.
TEST(TypeIdentityVocabularyLoader, SynthesizedTypeAcceptsAnyDeclaredEntry) {
    EXPECT_TRUE(synthesizedTypesLoad([](nlohmann::json& obj) {
        obj["alignof"]["LP64"] = "unsigned long long";
    })) << "the role's value is an OPAQUE vocabulary-entry name, resolved "
           "through the same table every other type-name knob uses";
}

// A NEW name on a row whose representation matches an existing entry must still
// load — nothing about the mechanism is C-specific or spelling-aware.
TEST(TypeIdentityVocabularyLoader, ArbitraryOpaqueNameAccepted) {
    EXPECT_TRUE(typeSpecifiersLoad([](nlohmann::json& rows) {
        auto& row = rows[rowIndexFor(rows, {"ShortKeyword"})];
        row["name"] = "zz-opaque-vocabulary-tag";
        row["rank"] = 1;
    })) << "`name` is OPAQUE tag data — the engine must never spell-check it";
}

// ── Loader validation: `builtinFunctions.signatureByDataModel` ─────────────
//
// The NEW language-config surface this cycle added. It is read ONLY inside the
// `signature` branch, so on the `params`/`result` form it was silently ignored —
// and `builtinFunctions` entries had NO closed-key rejection at all, so a typo'd
// key loaded clean and did nothing. Both are the "knob that lies" class the
// neighbouring rejections ('rank' requires a 'name'; 'signature' and
// 'params'/'result' are mutually exclusive) already close.

namespace {
// Perturb the shipped `builtinFunctions` array and report whether the schema
// still loads.
[[nodiscard]] bool builtinFunctionsLoad(std::function<void(nlohmann::json&)> mutate) {
    nlohmann::json doc = loadShippedCSubsetJson();
    mutate(doc["semantics"]["builtinFunctions"]);
    return GrammarSchema::loadFromText(doc.dump(), "<builtins-perturbed>").has_value();
}

// The index of the first shipped builtin declaring `key`.
[[nodiscard]] std::size_t builtinIndexWith(nlohmann::json const& arr,
                                           char const* key) {
    for (std::size_t i = 0; i < arr.size(); ++i) {
        if (arr[i].contains(key)) return i;
    }
    ADD_FAILURE() << "no shipped builtinFunctions entry declares '" << key << "'";
    return 0;
}
} // namespace

TEST(TypeIdentityVocabularyLoader, ShippedBuiltinFunctionsLoadUnperturbed) {
    EXPECT_TRUE(builtinFunctionsLoad([](nlohmann::json&) {}))
        << "fixture precondition: the SHIPPED builtins must load";
    // ... and the surface under test is actually EXERCISED by the shipped config
    // (otherwise every pin below would be testing an unused code path).
    nlohmann::json const doc = loadShippedCSubsetJson();
    auto const& arr = doc["semantics"]["builtinFunctions"];
    std::size_t withOverride = 0;
    for (auto const& e : arr) {
        if (e.contains("signatureByDataModel")) ++withOverride;
    }
    EXPECT_GE(withOverride, 1u)
        << "at least one shipped builtin must carry a per-data-model signature "
           "override (`_InterlockedCompareExchange`'s `LONG*`)";
}

TEST(TypeIdentityVocabularyLoader, BuiltinSignatureByDataModelUnknownKeyRejected) {
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        arr[builtinIndexWith(arr, "signatureByDataModel")]
           ["signatureByDataModel"]["LP62"] = "fn(i32) -> i32";
    })) << "a typo'd data-model key can NEVER match — it would silently leave "
           "the base signature in force on every target";
}

TEST(TypeIdentityVocabularyLoader, BuiltinSignatureByDataModelNonStringRejected) {
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        arr[builtinIndexWith(arr, "signatureByDataModel")]
           ["signatureByDataModel"]["LP64"] = 42;
    })) << "each override must be a non-empty signature STRING";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        arr[builtinIndexWith(arr, "signatureByDataModel")]
           ["signatureByDataModel"]["LP64"] = "";
    })) << "an EMPTY override is indistinguishable from 'no override'";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        arr[builtinIndexWith(arr, "signatureByDataModel")]
           ["signatureByDataModel"] = "fn(i32) -> i32";
    })) << "the value must be an OBJECT keyed by data-model name";
}

// ★ The silent-ignore hole: the `params`/`result` form never READS the key, so
// declaring it there loaded clean and did exactly nothing.
TEST(TypeIdentityVocabularyLoader, BuiltinSignatureByDataModelWithParamsRejected) {
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        for (auto& e : arr) {
            if (e.contains("signature")) continue;   // the other form
            e["signatureByDataModel"] = {{"LLP64", "fn(u64) -> u64"}};
            return;
        }
        ADD_FAILURE() << "no shipped builtin uses the params/result form";
    })) << "'signatureByDataModel' overrides 'signature'; on the params/result "
           "form there is nothing to override, so it must FAIL LOUD rather than "
           "load clean and silently do nothing";
}

// The closed-key discriminator itself — the general fix, not just this one key.
TEST(TypeIdentityVocabularyLoader, BuiltinFunctionUnknownKeyRejected) {
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        arr[0]["signatureByDataModl"] = {{"LLP64", "fn(u64) -> u64"}};
    })) << "a mis-spelled key must be rejected, not silently ignored";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        arr[0]["varadic"] = true;
    })) << "the typo discriminator covers every key, not just the new one";
    // `$`-prefixed documentation keys stay legal (the codebase-wide convention).
    EXPECT_TRUE(builtinFunctionsLoad([](nlohmann::json& arr) {
        arr[0]["$note"] = "documentation";
    }));
}

// The EAGER anti-lurking decode at the injection site (semantic_analyzer.cpp):
// EVERY declared override is decoded regardless of which model is active, so a
// malformed INACTIVE override fails on EVERY target rather than lurking until
// that model is first compiled. Observed end-to-end: the perturbed schema is
// analyzed under the OTHER data model and must still error.
TEST(TypeIdentityVocabulary, MalformedInactiveSignatureOverrideFailsOnEveryTarget) {
    auto const analyzeWithOverride = [](std::string const& text, DataModel dm) {
        nlohmann::json doc = loadShippedCSubsetJson();
        auto& arr = doc["semantics"]["builtinFunctions"];
        bool patched = false;
        for (auto& e : arr) {
            if (!e.contains("signatureByDataModel")) continue;
            e["signatureByDataModel"]["LLP64"] = text;
            patched = true;
            break;
        }
        EXPECT_TRUE(patched);
        auto schema = GrammarSchema::loadFromText(doc.dump(), "<override-probe>");
        EXPECT_TRUE(schema.has_value());
        UnitBuilder builder{*schema};
        builder.addInMemory("int f(void){ return 0; }\n", "<mem>");
        auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
        return analyze(cu, dm);
    };
    // The LLP64 override is malformed. Under LLP64 it is the ACTIVE one...
    EXPECT_TRUE(analyzeWithOverride("fn(ptr<", DataModel::Llp64).hasErrors());
    // ... and under LP64 it is INACTIVE, yet must STILL fail: an override that
    // only breaks on the target nobody built yet is the lurking-config defect.
    EXPECT_TRUE(analyzeWithOverride("fn(ptr<", DataModel::Lp64).hasErrors())
        << "an INACTIVE malformed override must fail on EVERY target";
    // A well-formed override under BOTH models is the clean control.
    EXPECT_FALSE(analyzeWithOverride("fn(ptr<i32>, i32, i32) -> i32",
                                     DataModel::Lp64).hasErrors());
    // ... and a well-formed override that is not a FUNCTION type fails too
    // (the decode must land an FnSig, never any type that happens to parse).
    EXPECT_TRUE(analyzeWithOverride("i32", DataModel::Lp64).hasErrors())
        << "an override must decode to a FUNCTION type";
}

// ── Literal SUFFIXES carry vocabulary identity, not just a width ────────────
//
// `IntegerLadderResult`/`FloatLadderResult::vocabularyName`: the ladder resolves
// a suffixed constant through a `DataModelTypeRef`, which now carries the
// resolved row's tag. WITHOUT it the ladder minted the ANONYMOUS primitive of the
// core, so on LP64 `1L` (I64) and `1LL` (I64) were ONE type — and both were the
// anonymous I64, matching NEITHER `long:` nor `long long:`.
//
// RED-ON-DISABLE: drop the tag and every arm below falls to `default` (0) — or,
// with a `default`-less association set, to S_GenericSelectionNoMatch. The arms
// are read back through `selectedGenericExpr`, so the WINNER is observed
// directly rather than inferred from a downstream type (which would coincide
// across arms at equal width).
TEST(TypeIdentityVocabulary, IntegerLiteralSuffixesSelectTheirOwnGenericArm) {
    std::string const src =
        "int f(void){ return\n"
        "    _Generic((1L),   long: 1, long long: 2, int: 3)\n"
        "  + _Generic((1LL),  long: 1, long long: 2, int: 3)\n"
        "  + _Generic((1UL),  unsigned long: 1, unsigned long long: 2,\n"
        "                     unsigned int: 3)\n"
        "  + _Generic((1ULL), unsigned long: 1, unsigned long long: 2,\n"
        "                     unsigned int: 3)\n"
        "  + _Generic((1),    long: 1, long long: 2, int: 3); }\n";
    // No `default:` arm anywhere — an anonymous core is a HARD
    // S_GenericSelectionNoMatch rather than a silent fall-through.
    for (DataModel const dm : {DataModel::Lp64, DataModel::Llp64}) {
        SCOPED_TRACE(dm == DataModel::Lp64 ? "LP64" : "LLP64");
        auto m = analyzeCSubset(src, dm);
        expectGenericClean(m);
        EXPECT_EQ(selectedGenericArms(m),
                  (std::vector<std::string>{"1", "2", "1", "2", "3"}))
            << "each suffix selects ITS OWN vocabulary entry — `L`/`LL` and "
               "`UL`/`ULL` are distinct types even where the data model gives "
               "them one core, and an UNSUFFIXED literal is the anonymous `int`";
    }
}

TEST(TypeIdentityVocabulary, FloatLiteralSuffixSelectsLongDouble) {
    std::string const src =
        "int f(void){ return _Generic((1.0L), long double: 1, double: 2,\n"
        "                             float: 3)\n"
        "                  + _Generic((1.0),  long double: 1, double: 2,\n"
        "                             float: 3)\n"
        "                  + _Generic((1.0f), long double: 1, double: 2,\n"
        "                             float: 3); }\n";
    // Every long-double axis: on x87/ieee128 the cores already differ, but on the
    // f64 axis `long double` IS `double` in representation, so ONLY the identity
    // tag can tell the `L` suffix apart — that is the axis that used to collapse.
    for (LongDoubleFormat const axis : {LongDoubleFormat::F64,
                                        LongDoubleFormat::X87_80,
                                        LongDoubleFormat::Ieee128}) {
        SCOPED_TRACE(static_cast<int>(axis));
        auto m = analyzeCSubset(src, DataModel::Lp64, axis);
        expectGenericClean(m);
        EXPECT_EQ(selectedGenericArms(m),
                  (std::vector<std::string>{"1", "2", "3"}))
            << "`1.0L` IS `long double`, `1.0` IS the anonymous `double`, "
               "`1.0f` IS `float` — on EVERY long-double axis";
    }
}
