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
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_name_resolve.hpp"   // resolveLanguageTypeName: each rung is the spelled type
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::sem_test;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

[[nodiscard]] SemanticModel analyzeC(
    std::string src, DataModel dm,
    LongDoubleFormat ldf = LongDoubleFormat::X87_80) {
    auto cu = buildShippedUnit("c", {std::move(src)});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), dm, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
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

// The shipped c config, located by the ONE test-side resolver
// (`repo_root.hpp`: $DSS_CONFIG_ROOT → the repo root CMake bakes in → a cwd
// ancestor walk). It was a private 12-hop cwd walk that read neither of the
// first two sources, so an out-of-tree build — whose cwd has no `src/dss-config`
// in its ancestry — missed on every hop. It THROWS rather than returning `{}`:
// GoogleTest turns a throw into a failure of the ONE running test, and an empty
// path here used to propagate SILENTLY through `findShippedLibDir()` below.
[[nodiscard]] std::filesystem::path findShippedSourceConfig() {
    std::filesystem::path const path =
        dss::test::configRoot() / "sources" / "c.lang.json";
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("shipped c.lang.json is missing: " +
                                 path.string());
    }
    return path;
}

// The REAL shipped-descriptor directory (`src/dss-config/shippedLibs`), so a
// test can drive `#include <stdint.h>` against the descriptors that actually
// ship rather than a scratch stand-in — the point of the size_t/uint64_t pins is
// that the SHIPPED spelling resolves to the right vocabulary entry.
//
// Asked of the resolver directly instead of being derived by walking two levels
// UP from the lang.json above: on a miss that derivation produced the RELATIVE
// path `shippedLibs` from an empty base and handed it to `addSystemDir`, so the
// system-include path silently pointed at nothing.
[[nodiscard]] std::filesystem::path findShippedLibDir() {
    return dss::test::configRoot() / "shippedLibs";
}

// Analyze through the FULL front end with the shipped descriptors on the system
// include path — `analyzeC` above never runs `#include`.
[[nodiscard]] SemanticModel analyzeWithShippedHeaders(
    std::string src, DataModel dm, ObjectFormatKind fmt, std::string_view arch) {
    auto schema = loadShippedSchema("c");
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addSystemDir(findShippedLibDir());
    builder.setActiveFormat(fmt);
    builder.addInMemory(std::move(src), "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), dm, std::nullopt, std::nullopt,
                   SelectableObjectFormatKind::of(fmt), arch);
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

[[nodiscard]] nlohmann::json loadShippedCJson() {
    std::ifstream in{findShippedSourceConfig(), std::ios::binary};
    EXPECT_TRUE(in.good());
    return nlohmann::json::parse(in);
}

// D-LANG-DIRECT-CALL-INT-POINTEE-COMPAT: a PORTABLE scratch shipped descriptor
// modeling the `Tcl_GetWideIntFromObj` shape — a function whose pointer parameter
// is the abstract width-based `ptr<i64>` (NOT `long long*`; the descriptor `i64`
// is the ANONYMOUS I64, distinct-IDENTITY from every named C `long`/`long long`),
// plus a `fn() -> ptr<i64>` so a `ptr<i64>` rvalue can be produced for the
// init/assign/return boundary pins. Written into a caller-owned scratch system dir
// and reached via `#include <ffiwide.h>`.
constexpr char const* kFfiWideDescriptorJson = R"JSON({
  "header": "ffiwide.h",
  "library": { "pe": "scratchffi.dll", "elf": "libscratchffi.so.1", "macho": "/usr/lib/libscratchffi.dylib" },
  "symbols": [
    { "name": "ffi_take_wide", "signature": "fn(ptr<i64>) -> void", "kind": "function", "linkage": "external" },
    { "name": "ffi_wide_ptr",  "signature": "fn() -> ptr<i64>",     "kind": "function", "linkage": "external" }
  ]
})JSON";

// Analyze `mainSrc` against `kFfiWideDescriptorJson` (written into `sysDir`) under
// the axis `ax`. `flagOn` selects the SHIPPED schema (the diagnosed conversion
// enabled) vs a perturbed copy with
// `pointerConversions.incompatiblePointerConvertsDiagnosed=false` — the config
// red-on-disable axis (the `analyzeWithOverride` perturbation idiom). P68 round 9
// retired `directCallIntPointeeCompat` into that key: the same-representation
// integer pointee is one instance of the class it admits.
// The ScratchDir must outlive the returned model (the semantic phase reads the
// descriptor file), so the caller owns it.
[[nodiscard]] SemanticModel analyzeFfiWide(ScratchDir const& sysDir,
                                           std::string const& mainSrc,
                                           ModelAxis ax, bool flagOn = true) {
    std::ofstream(sysDir.path() / "ffiwide.json", std::ios::binary)
        << kFfiWideDescriptorJson;
    auto build = [&](auto const& schema) {
        UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
        builder.addSystemDir(sysDir.path());
        builder.setActiveFormat(ax.fmt);
        builder.addInMemory(mainSrc, "main.c");
        auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
        assertNoBuilderErrors(*cu);
        return analyze(cu, DiagnosticBudget::libraryDefault(), ax.dm, std::nullopt, std::nullopt,
                       SelectableObjectFormatKind::of(ax.fmt), ax.arch);
    };
    if (flagOn) return build(loadShippedSchema("c"));
    nlohmann::json doc = loadShippedCJson();
    doc["semantics"]["pointerConversions"]["incompatiblePointerConvertsDiagnosed"] = false;
    auto schema = GrammarSchema::loadFromText(doc.dump(), "<ffi-wide-flag-off>");
    // ★ FAIL-CLOSED, TF-C135: this was `EXPECT_TRUE`, which is NON-FATAL — so when
    // the key was renamed and the perturbed schema stopped loading, the helper walked
    // straight on to `*schema` and SEGFAULTED. A crash is a far worse verdict than a
    // failed assertion: it takes the whole binary down, so every LATER test in this
    // file reported nothing at all. An assertion that guards a dereference must stop
    // the dereference; `ASSERT_*` cannot be used here (non-void return), so throw.
    if (!schema.has_value()) {
        throw std::runtime_error(
            "perturbed c schema failed to load — the "
            "`incompatiblePointerConvertsDiagnosed` key was renamed or removed, so "
            "this red-on-disable axis is testing nothing");
    }
    return build(*schema);
}

// The pointee TypeId of the injected `ffi_take_wide`'s first parameter (the
// descriptor `i64`) — for the identity witness (it must NEVER be the same TypeId
// as a named C `long`/`long long`, only the same REPRESENTATION).
[[nodiscard]] TypeId ffiTakeWideParamPointee(SemanticModel const& m) {
    auto const& in = m.lattice().interner();
    for (std::size_t i = 1; i < m.symbols().size(); ++i) {
        if (m.symbols()[i].name != "ffi_take_wide") continue;
        TypeId const fnTy = m.symbols()[i].type;
        if (!fnTy.valid() || in.kind(fnTy) != TypeKind::FnSig) break;
        if (in.fnParams(fnTy).empty()) break;
        TypeId const p0 = in.fnParams(fnTy)[0];
        if (in.kind(p0) != TypeKind::Ptr) break;
        return in.operands(p0)[0];
    }
    ADD_FAILURE() << "ffi_take_wide not injected as fn(ptr<i64>)";
    return InvalidType;
}

// Perturb the shipped `typeSpecifiers` rows and report whether the schema still
// loads. Every loader pin below proves a knob CANNOT lie.
[[nodiscard]] bool typeSpecifiersLoad(std::function<void(nlohmann::json&)> mutate) {
    nlohmann::json doc = loadShippedCJson();
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
    auto m = analyzeC(src, DataModel::Llp64);
    expectGenericClean(m);
    EXPECT_EQ(selectedGenericArms(m), (std::vector<std::string>{"22"}))
        << "the `long:` arm must win — 11 means it matched `int:`, 33 means it "
           "fell through to `default:`";
}

// The control: the SAME source on LP64, where the cores already differ.
TEST(TypeIdentityVocabulary, GenericIntVsLongDistinctUnderLp64) {
    std::string const src =
        "int f(long x){ return _Generic((x), int: 11, long: 22, default: 33); }\n";
    auto m = analyzeC(src, DataModel::Lp64);
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
        auto m = analyzeC(src, dm);
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
        auto m = analyzeC(src, dm);
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
        auto m = analyzeC(src, DataModel::Lp64, row.axis);
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
        auto m = analyzeC(src, dm);
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
        auto m = analyzeC(src, dm);
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
    auto m = analyzeC(src, DataModel::Lp64);
    ASSERT_FALSE(m.hasErrors());
    for (char const* name : {"a", "b", "c", "d", "e", "g", "h", "k"}) {
        EXPECT_EQ(vocabOf(m, name), "")
            << "'" << name << "' must carry NO vocabulary tag — naming it would "
               "break promotion / enum-underlying synthesis, which re-mint the "
               "anonymous primitive of that kind";
    }
    // ... while the entries that CAN collide all carry one.
    auto named = analyzeC(
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
    auto llp = analyzeC(
        "int f(int i, long l){ long a = i; int b = l; return a == b; }\n",
        DataModel::Llp64);
    EXPECT_FALSE(llp.hasErrors())
        << "LLP64 int<->long (both I32) must convert cleanly both ways";
    expectSplitPair(llp, "i", "l");
    // ... and the DESTINATIONS keep their own identity (a conversion that
    // silently retyped `a` as `int` would also be "clean").
    EXPECT_EQ(vocabOf(llp, "a"), "long");
    EXPECT_EQ(vocabOf(llp, "b"), "");

    auto lp = analyzeC(
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
    auto f64 = analyzeC(
        "int f(double d, long double ld){ double a = ld; long double b = d;\n"
        "  return a == 0.0 && b == 0.0L; }\n",
        DataModel::Lp64, LongDoubleFormat::F64);
    EXPECT_FALSE(f64.hasErrors())
        << "f64 axis: double <-> long double must convert cleanly BOTH ways";
    expectSplitPair(f64, "d", "ld");
    EXPECT_EQ(vocabOf(f64, "a"), "");
    EXPECT_EQ(vocabOf(f64, "b"), "long double");

    // On a WIDER long-double axis the two have genuinely different representations, so
    // the conversion is governed by the float RANK rule, not identity. BOTH directions
    // are now implicit assignment conversions: WIDENING (`floatRank(rhs) <= floatRank
    // (lhs)`, unconditional) and — since D-CSUBSET-FLOAT-FROM-DOUBLE-NARROWING —
    // NARROWING too (F80/F128 -> F64, gated on `floatSameKindNarrows`, coerce emits the
    // FPTrunc). This is still a RANK rule (kind-keyed), not an identity rule; the
    // identity distinction is the vocab split checked on the f64 axis above.
    for (LongDoubleFormat const axis : {LongDoubleFormat::X87_80,
                                        LongDoubleFormat::Ieee128}) {
        SCOPED_TRACE(static_cast<int>(axis));
        auto widen = analyzeC(
            "int f(double d){ long double b = d; return b == 0.0L; }\n",
            DataModel::Lp64, axis);
        EXPECT_FALSE(widen.hasErrors())
            << "double -> long double widening stays clean on a wider axis";
        auto narrow = analyzeC(
            "int f(long double ld){ double a = ld; return a == 0.0; }\n",
            DataModel::Lp64, axis);
        EXPECT_EQ(countCode(narrow.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
            << "and the NARROWING direction (F80/F128 -> F64) is now an admitted "
               "implicit conversion too (D-CSUBSET-FLOAT-FROM-DOUBLE-NARROWING) — the "
               "float rank rule admits narrowing when floatSameKindNarrows is on";
    }
}

// ── The CORRECT tightening (defect 5) ──────────────────────────────────────

TEST(TypeIdentityVocabulary, IncompatiblePointerTypesNowDiagnose) {
    // C requires a constraint diagnostic here. Under the collapse `int` and
    // `long` were ONE TypeId on LLP64, so this compiled SILENTLY — the collapse
    // did not merely fail loud, it ACCEPTED invalid code.
    // ★ P68 round 9 (lane `cs`): the diagnostic is now the WARNING every pinned
    // reference gives (the program builds and keeps its pointer), not a refusal —
    // [[D-C-INCOMPATIBLE-POINTER-CONVERSION-REFUSED-WHERE-EVERY-REFERENCE-WARNS]].
    // What this test exists for is unchanged and still asserted: the two types are
    // DIFFERENT, so the conversion is DIAGNOSED, never silent.
    auto llp = analyzeC(
        "int f(void){ int x = 0; int *p = &x; long *q = p; return *q != 0; }\n",
        DataModel::Llp64);
    EXPECT_FALSE(llp.hasErrors())
        << "`long *q = p;` from an `int *` is admitted with the diagnostic C requires";
    EXPECT_EQ(countCode(llp.diagnostics(),
                        DiagnosticCode::S_IncompatiblePointerIntegerPointee), 1u)
        << "`long *q = p;` from an `int *` is a C constraint violation: DIAGNOSED";

    // The same diagnostic on LP64's OTHER same-representation pair.
    auto lp = analyzeC(
        "int f(void){ long x = 0; long *p = &x; long long *q = p;\n"
        "  return *q != 0; }\n",
        DataModel::Lp64);
    EXPECT_FALSE(lp.hasErrors());
    EXPECT_EQ(countCode(lp.diagnostics(),
                        DiagnosticCode::S_IncompatiblePointerIntegerPointee), 1u)
        << "`long long *q = (long*)…` is a C constraint violation on LP64 too";

    // ... and the matching-type control stays CLEAN (proves the tightening is
    // not a blanket pointer reject).
    auto ok = analyzeC(
        "int f(void){ long x = 0; long *p = &x; long *q = p; return *q != 0; }\n",
        DataModel::Llp64);
    EXPECT_FALSE(ok.hasErrors()) << "same-vocabulary pointers stay assignable";
}

TEST(TypeIdentityVocabulary, CharFamilyStaysThreeDistinctTypes) {
    // Pre-existing behavior that must be PRESERVED: char / signed char /
    // unsigned char are three distinct CORES (Char/I8/U8), so their pointers
    // were already incompatible. Unrelated to the vocabulary split — pinned so a
    // future identity change cannot quietly merge them. (P68 round 9: the
    // incompatibility is DIAGNOSED with the warning every reference gives rather
    // than refused; a merged identity would make it silent, which is what this
    // pin catches.)
    auto m = analyzeC(
        "int f(void){ char c = 0; char *p = &c; signed char *q = p;\n"
        "  return *q != 0; }\n",
        DataModel::Lp64);
    EXPECT_FALSE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_IncompatiblePointerConversion), 1u);
}

// ── Conversion RANK is keyed on the NAME, not the width ────────────────────

TEST(TypeIdentityVocabulary, ArithmeticResultTakesHigherRankedVocabularyName) {
    // C 6.3.1.1 defines rank by type NAME. With a width-derived rank
    // `someLong + someLongLong` on LP64 (both I64) yields the WRONG NAME —
    // observable exactly here.
    std::string const lpSrc =
        "int f(long a, long long b){ auto s = a + b;\n"
        "  return _Generic((s), long: 11, long long: 22, default: 33); }\n";
    auto lp = analyzeC(lpSrc, DataModel::Lp64);
    expectGenericClean(lp);
    EXPECT_EQ(vocabOf(lp, "s"), "long long")
        << "long + long long is `long long` (rank 4 > 3), even at equal width";
    EXPECT_EQ(selectedGenericArms(lp), (std::vector<std::string>{"22"}));

    // The int/long pair at equal width under LLP64.
    std::string const llpSrc =
        "int f(int a, long b){ auto s = a + b;\n"
        "  return _Generic((s), int: 11, long: 22, default: 33); }\n";
    auto llp = analyzeC(llpSrc, DataModel::Llp64);
    expectGenericClean(llp);
    EXPECT_EQ(vocabOf(llp, "s"), "long")
        << "int + long is `long` (rank 3 > int's 0), even at equal width";
    EXPECT_EQ(selectedGenericArms(llp), (std::vector<std::string>{"22"}));

    // The float axis sibling: `double + long double` is `long double` even where
    // both are F64.
    std::string const fSrc =
        "int f(double a, long double b){ auto s = a + b;\n"
        "  return _Generic((s), double: 11, long double: 22, default: 33); }\n";
    auto f64 = analyzeC(fSrc, DataModel::Lp64, LongDoubleFormat::F64);
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
    auto m = analyzeC(src, DataModel::Lp64);
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
    auto lp = analyzeC(src, DataModel::Lp64);
    expectGenericClean(lp);
    EXPECT_EQ(selectedGenericArms(lp), (std::vector<std::string>{"1", "10", "100"}))
        << "LP64 size_t IS `unsigned long` — a `default:` hit here is the SILENT "
           "wrong-arm selection an anonymous U64 causes";
    // LLP64: the SAME 64-bit representation, the OTHER vocabulary entry.
    auto llp = analyzeC(src, DataModel::Llp64);
    expectGenericClean(llp);
    EXPECT_EQ(selectedGenericArms(llp), (std::vector<std::string>{"2", "20", "200"}))
        << "LLP64 size_t IS `unsigned long long` — same core, different NAME, so "
           "identity cannot be derived from representation";
}

TEST(TypeIdentityVocabulary, PointerDifferenceYieldsTheDeclaredPtrdiffTEntry) {
    std::string const src =
        "int f(int *a, int *b){\n"
        "  return _Generic((a - b), long: 1, long long: 2, default: 0); }\n";
    auto lp = analyzeC(src, DataModel::Lp64);
    expectGenericClean(lp);
    EXPECT_EQ(selectedGenericArms(lp), (std::vector<std::string>{"1"}))
        << "LP64 ptrdiff_t IS `long`";
    auto llp = analyzeC(src, DataModel::Llp64);
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
    auto lp = analyzeC(src, DataModel::Lp64);
    expectGenericClean(lp);
    EXPECT_EQ(selectedGenericArms(lp), (std::vector<std::string>{"4"}))
        << "LP64 size_t IS `unsigned long`";
    auto llp = analyzeC(src, DataModel::Llp64);
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
// shows up as a DIAGNOSED pointer conversion on code C says is correct. (P68
// round 9, lane `cs`: an incompatible pointer conversion is admitted with the
// warning every pinned reference gives, so the witness is the diagnosed-conversion
// code, never `hasErrors()` alone — that would pass over a silent accept too.)
TEST(TypeIdentityVocabulary, ShippedAliasPointersMatchTheirNamedStandardType) {
    for (ModelAxis const ax : {kLp64, kLlp64}) {
        SCOPED_TRACE(ax.label);
        std::string const ok =
            // The pointer flows through an INTERMEDIATE variable of the alias's
            // own pointer type. A direct `T *p = &x;` initializer runs the same
            // pointer-conversion classifier, so either spelling witnesses the
            // identity; the intermediate keeps the pin's source unchanged.
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
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_IncompatiblePointerIntegerPointee), 0u)
            << "the alias and its named type are ONE type: nothing to diagnose";
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_IncompatiblePointerConversion), 0u)
            << "the alias and its named type are ONE type: nothing to diagnose";

        // ... and the OTHER model's name is a genuinely different type, so it
        // must still DIAGNOSE (proving the pin above is not a blanket accept).
        // On LP64 the wrong name (`unsigned long long`) shares `uint64_t`'s
        // representation, so it is the narrower same-representation code; on
        // LLP64 the wrong name (`unsigned long`) is 32-bit, the general one.
        bool const lp = ax.dm == DataModel::Lp64;
        std::string const wrongName = lp ? "unsigned long long" : "unsigned long";
        std::string const bad =
            std::string{"#include <stdint.h>\n"}
            + "int f(void){ uint64_t x = 0; uint64_t *px = &x;\n"
            + "  " + wrongName + " *p = px;\n"
            + "  return *p != 0; }\n";
        auto n = analyzeWithShippedHeaders(bad, ax.dm, ax.fmt, ax.arch);
        EXPECT_FALSE(n.hasErrors())
            << "an incompatible pointer conversion builds with its diagnostic";
        EXPECT_EQ(countCode(n.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
        EXPECT_EQ(countCode(n.diagnostics(),
                            DiagnosticCode::S_IncompatiblePointerIntegerPointee),
                  lp ? 1u : 0u)
            << "the other model's vocabulary entry is a DIFFERENT type here";
        EXPECT_EQ(countCode(n.diagnostics(),
                            DiagnosticCode::S_IncompatiblePointerConversion),
                  lp ? 0u : 1u)
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
    EXPECT_FALSE(hasDiagnosedPointerConversion(m.diagnostics()))
        << "a compatible pointer pair must not be DIAGNOSED (the vacuity sweep)";

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
    //
    // ★ CONTRACT CHANGED TF-C135 (D-LANG-DIRECT-CALL-INT-POINTEE-COMPAT), AND THE
    // CHANGE IS STATED HERE RATHER THAN ABSORBED. This used to assert `hasErrors()`.
    // Widening the integer-pointee relaxation from "shipped-descriptor callee" to
    // "any DIRECT callee" reaches this intrinsic too: on LLP64 `long` is I32 and so
    // is `int`, so the two pointees now pass `sameRepresentation` and the call is
    // admitted. That is NOT a silent collapse and the assertion below is what proves
    // it: the compiler still says the types differ, at WARNING severity, which is
    // exactly what MSVC (C4133) and clang (`-Wincompatible-pointer-types`) do for
    // `int*` into `long*`. The identity claim this test exists for is untouched — the
    // pointee is still the NAMED `long`, asserted above, and `_Generic` still splits
    // them. Under `--warnings-as-errors` this is an error again.
    auto bad = analyzeWithShippedHeaders(
        "int f(void){ int v = 0;\n"
        "  return (int)_InterlockedCompareExchange(&v, 1, 0); }\n",
        kLlp64.dm, kLlp64.fmt, kLlp64.arch);
    EXPECT_FALSE(bad.hasErrors())
        << "same-representation integer pointees are admitted at a direct call arg";
    EXPECT_EQ(countCode(bad.diagnostics(),
                  DiagnosticCode::S_IncompatiblePointerIntegerPointee), 1u)
        << "`int*` and `long*` are DIFFERENT types even at one representation — "
           "admitting one for the other SILENTLY would be the blanket collapse this "
           "test exists to forbid; the warning is the diagnostic that keeps it honest";
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
    // P68 round 9: DIAGNOSED with the warning the references give, not refused.
    EXPECT_EQ(countCode(bad.diagnostics(),
                        DiagnosticCode::S_IncompatiblePointerIntegerPointee), 1u)
        << "`unsigned int *` is NOT `DWORD *` — both are u32, and that is "
           "exactly the collapse this change undoes";
    EXPECT_EQ(countCode(bad.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}

// ── D-LANG-DIRECT-CALL-INT-POINTEE-COMPAT ────────────────────────────────
//
// A shipped-FFI-descriptor `ptr<i64>` parameter accepts a real C integer pointer
// of the SAME representation (size ∧ signedness ∧ integer-base-kind) with the
// narrower S_IncompatiblePointerIntegerPointee warning, and NEVER merges the
// distinct type identities.
// ★ P68 round 9 (lane `cs`,
// [[D-C-INCOMPATIBLE-POINTER-CONVERSION-REFUSED-WHERE-EVERY-REFERENCE-WARNS]]): this
// admission used to be SCOPED — a direct call's argument only, a same-representation
// integer pointee only — and every other incompatible pointer pair was refused. It
// was one instance of a class every pinned reference builds with a warning at EVERY
// site, and the class is now admitted as such: a different width or signedness
// reports the general S_IncompatiblePointerConversion, the same pair at init /
// assignment / return / an indirect call reports the same narrower code, and the
// switch is `incompatiblePointerConvertsDiagnosed`. The pins below keep what the row
// was for — the identity witness, the per-target answer, the diagnostic never going
// silent, the config key being the switch — and state the moved expectations.

// ★★ TF-C135 — THE CASE THE OLD `isShippedDescriptorFn` GATE COULD NOT REACH, AND
// THE REASON THE GATE WAS WRONG. The callee here is an ORDINARY C PROTOTYPE, not a
// shipped descriptor: the pointee types are what decide, so a `long long*` into a
// `long*` parameter is admitted on LP64 exactly as it is for a descriptor's
// `ptr<i64>`. This is upstream sqlite's Darwin shape — `tcl.h` declares
// `Tcl_WideInt` as `long` there, and `sqlite3_int64` is `long long` — MEASURED to be
// a `-Wincompatible-pointer-types` WARNING under Apple clang 21.0.0 and a hard
// S0003 here before this change, which cost both mach-o legs their testfixture.
//
// THE ADMISSION IS DIAGNOSED, NOT SILENT: exactly one
// S_IncompatiblePointerIntegerPointee. Asserting only `!hasErrors()` would pass just
// as well over an implementation that accepted it QUIETLY, which is the failure this
// project calls a silent accept — so the warning count is the load-bearing assertion,
// not the error count.
TEST(TypeIdentityVocabulary, DirectCallIntPointeeAdmitsAtAPlainCPrototypeAndWarns) {
    std::string const src =
        "void plain_take_long(long *p);\n"
        "typedef long long sqlite3_int64;\n"
        "int f(void){ sqlite3_int64 v = 0; plain_take_long(&v); return (int)v; }\n";
    auto m = analyzeWithShippedHeaders(src, kLp64.dm, kLp64.fmt, kLp64.arch);
    EXPECT_FALSE(m.hasErrors())
        << "on LP64 `long` and `long long` are one representation — a direct call "
           "arg admits it, whatever declared the callee";
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(m.diagnostics(),
                  DiagnosticCode::S_IncompatiblePointerIntegerPointee), 1u)
        << "the conversion must be DIAGNOSED — silence here is the whole failure "
           "mode, and it is what distinguishes this from a blanket collapse";

    // PER-TARGET, BY CONSTRUCTION AND WITH NO FORMAT BRANCH: on LLP64 `long` is I32
    // while `long long` is I64, so `sameRepresentation` fails on the width axis and
    // the SAME source is the GENERAL incompatible-pointer class (P68 round 9: admitted
    // with S_IncompatiblePointerConversion, as MSVC's C4133 and gcc 13 admit it). The
    // negative control for the pin above is therefore the CODE: without it, "the
    // narrower code on LP64" could equally describe a predicate that answers it
    // everywhere.
    auto llp = analyzeWithShippedHeaders(src, kLlp64.dm, kLlp64.fmt, kLlp64.arch);
    EXPECT_FALSE(llp.hasErrors());
    EXPECT_EQ(countCode(llp.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(llp.diagnostics(),
                  DiagnosticCode::S_IncompatiblePointerIntegerPointee), 0u)
        << "`long long*` into `long*` is a REAL width mismatch on LLP64";
    EXPECT_EQ(countCode(llp.diagnostics(),
                  DiagnosticCode::S_IncompatiblePointerConversion), 1u);
}

// POSITIVE: `ptr<i64>` accepts `long long*`, a `typedef long long` (the
// sqlite3_int64 shape), AND `long*` on LP64 (where `long` is I64) — no S0003. And
// the IDENTITY WITNESS: the admission is a COMPAT match, never a TypeId merge.
TEST(TypeIdentityVocabulary,
     DirectCallIntPointeeAdmitsWideIntPointerAtShippedCallArg) {
    ScratchDir sysDir{Location::Temp, "ffi-wide-pos"};
    std::string const src =
        "#include <ffiwide.h>\n"
        "typedef long long sqlite3_int64;\n"
        "int f(void){\n"
        "  long long a = 0;     ffi_take_wide(&a);\n"   // long long*  -> ptr<i64>
        "  sqlite3_int64 b = 0; ffi_take_wide(&b);\n"   // (typedef)*  -> ptr<i64>
        "  long c = 0;          ffi_take_wide(&c);\n"   // long* (I64 on LP64)
        "  return (int)(a + b + c); }\n";
    auto m = analyzeFfiWide(sysDir, src, kLp64);
    EXPECT_FALSE(m.hasErrors())
        << "a same-representation integer pointer IS the `ptr<i64>` parameter on LP64";
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);

    // Condition 1: the descriptor `i64` pointee is a DISTINCT TypeId from the
    // named C `long long` — same REPRESENTATION only (never a merge).
    auto const& in = m.lattice().interner();
    TypeId const paramPointee = ffiTakeWideParamPointee(m);
    TypeId const argPointee   = typeOf(m, "a");   // `long long`
    ASSERT_TRUE(paramPointee.valid() && argPointee.valid());
    EXPECT_NE(paramPointee.v, argPointee.v)
        << "the descriptor `ptr<i64>` pointee must NOT be merged with named `long long`";
    EXPECT_TRUE(in.sameRepresentation(paramPointee, argPointee))
        << "... it is admitted purely because the REPRESENTATION matches";
    EXPECT_EQ(std::string{in.vocabularyName(paramPointee)}, "")
        << "the descriptor pointee is the ANONYMOUS i64, never a named vocabulary entry";
    EXPECT_EQ(vocabOf(m, "a"), "long long");
}

// PER-TARGET (Condition 6): on LLP64/pe (where `long` is I32) the SAME `long*` is
// NOT the `ptr<i64>` parameter's representation — emergent from the data model's
// `kind`, with no format branch — so it takes the GENERAL incompatible-pointer code
// (P68 round 9: admitted with S_IncompatiblePointerConversion, no longer S0003).
// `long long*` (I64 on both models) keeps the narrower same-representation code.
TEST(TypeIdentityVocabulary, DirectCallIntPointeePerTargetUnderLlp64LongIsAnotherWidth) {
    ScratchDir sysDir{Location::Temp, "ffi-wide-llp64"};
    auto bad = analyzeFfiWide(sysDir,
        "#include <ffiwide.h>\n"
        "int f(void){ long c = 0; ffi_take_wide(&c); return 0; }\n",
        kLlp64);
    EXPECT_EQ(countCode(bad.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(bad.diagnostics(),
                        DiagnosticCode::S_IncompatiblePointerIntegerPointee), 0u)
        << "`long` is 32-bit under LLP64, so `long*` is NOT `ptr<i64>`'s "
           "representation — with no format branch (sameRepresentation's kind axis "
           "decides)";
    EXPECT_EQ(countCode(bad.diagnostics(),
                        DiagnosticCode::S_IncompatiblePointerConversion), 1u);

    auto ok = analyzeFfiWide(sysDir,
        "#include <ffiwide.h>\n"
        "int f(void){ long long a = 0; ffi_take_wide(&a); return 0; }\n",
        kLlp64);
    EXPECT_FALSE(ok.hasErrors())
        << "`long long` is 64-bit on every model — its pointer is the parameter";
    EXPECT_EQ(countCode(ok.diagnostics(),
                        DiagnosticCode::S_IncompatiblePointerIntegerPointee), 1u);
}

// PREDICATE NEGATIVES at the shipped boundary: the NARROWER code answers ONLY a
// same-(size ∧ signedness ∧ integer-base-kind) pointee — every other integer /
// non-integer pointer takes the general S_IncompatiblePointerConversion (P68 round
// 9: admitted with a warning, no longer S0003), proving the predicate still
// discriminates exactly where it is asked.
TEST(TypeIdentityVocabulary,
     DirectCallIntPointeePredicateNegativesTakeTheGeneralCode) {
    ScratchDir sysDir{Location::Temp, "ffi-wide-neg"};
    std::string const src =
        "#include <ffiwide.h>\n"
        "enum E { X };\n"
        "int f(void){\n"
        "  int p = 0;                ffi_take_wide(&p);\n"    // size: I32 != I64
        "  unsigned long long q = 0; ffi_take_wide(&q);\n"    // signedness: U64 != I64
        "  double d = 0;             ffi_take_wide(&d);\n"    // base-kind: F64
        "  enum E e = X;             ffi_take_wide(&e);\n"    // not an integer kind
        "  _BitInt(64) w = 0;        ffi_take_wide(&w);\n"    // extensionKind (BitInt)
        "  return p; }\n";
    auto m = analyzeFfiWide(sysDir, src, kLp64);
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_IncompatiblePointerIntegerPointee), 0u)
        << "each arg differs on exactly one axis (size / signedness / base-kind / "
           "kind / extensionKind) — none is a same-representation integer pointer";
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_IncompatiblePointerConversion), 5u);
}

// ★ P68 round 9: THE CONVERSION IS ONE RULE AT EVERY SITE. This pin used to assert
// the opposite — that the SAME same-representation pair stayed a hard S0003 at
// INIT / ASSIGNMENT / RETURN because the relaxation was scoped to the call argument
// (TF-C41's "Condition 3"). Every pinned reference builds all four sites with the
// same warning, and C converts an initializer, an argument and a return "as if by
// assignment", so a scope that differed by site was the defect
// [[D-C-INCOMPATIBLE-POINTER-CONVERSION-REFUSED-WHERE-EVERY-REFERENCE-WARNS]]
// closed. What survives is the property that matters: each site DIAGNOSES it, with
// the same narrower code, once.
TEST(TypeIdentityVocabulary, IntPointeeSameRepresentationReportsAlikeAtInitAssignReturn) {
    ScratchDir sysDir{Location::Temp, "ffi-wide-scope"};
    std::string const src =
        "#include <ffiwide.h>\n"
        "long long* g_init(void){ long long *p = ffi_wide_ptr(); return p; }\n"   // INIT
        "void g_assign(void){ long long *p; p = ffi_wide_ptr(); }\n"              // ASSIGN
        "long long* g_return(void){ return ffi_wide_ptr(); }\n";                  // RETURN
    auto m = analyzeFfiWide(sysDir, src, kLp64);
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_ReturnTypeMismatch), 0u);
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_IncompatiblePointerIntegerPointee), 3u)
        << "INIT + ASSIGN + RETURN each report the same narrower code once";
}

// CONFIG RED-ON-DISABLE: the admission is gated on
// `pointerConversions.incompatiblePointerConvertsDiagnosed` (P68 round 9; it was
// `directCallIntPointeeCompat`). Flip it FALSE (schema perturbation) and the very
// admission above reverts to S0003.
TEST(TypeIdentityVocabulary, DirectCallIntPointeeConfigFlagRedOnDisable) {
    ScratchDir sysDir{Location::Temp, "ffi-wide-flagoff"};
    std::string const src =
        "#include <ffiwide.h>\n"
        "int f(void){ long long a = 0; ffi_take_wide(&a); return 0; }\n";
    auto on = analyzeFfiWide(sysDir, src, kLp64, /*flagOn=*/true);
    EXPECT_FALSE(on.hasErrors()) << "flag ON admits the wide-int pointer";
    EXPECT_EQ(countCode(on.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    auto off = analyzeFfiWide(sysDir, src, kLp64, /*flagOn=*/false);
    EXPECT_EQ(countCode(off.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u)
        << "flag OFF reverts to the strict pointer-pointee reject";
}

// FN-POINTER / INDIRECT: ★ P68 round 9 — the same shipped fn reached through a
// NON-direct callee (a designator deref — the vehicle here because a typed C
// fn-pointer cannot spell the descriptor's anonymous `i64` parameter) reports
// EXACTLY what the direct call reports. This pin used to assert the indirect path
// stayed S0003; a conversion that depends on how the callee is SPELLED is the
// provenance-gated rule TF-C135 already called a mistake, one layer further out.
TEST(TypeIdentityVocabulary, IntPointeeSameRepresentationReportsAlikeThroughAnIndirectCall) {
    ScratchDir sysDir{Location::Temp, "ffi-wide-indirect"};
    auto direct = analyzeFfiWide(sysDir,
        "#include <ffiwide.h>\n"
        "int f(void){ long long a = 0; ffi_take_wide(&a); return 0; }\n", kLp64);
    EXPECT_FALSE(direct.hasErrors()) << "the direct bare-name call admits";
    EXPECT_EQ(countCode(direct.diagnostics(),
                        DiagnosticCode::S_IncompatiblePointerIntegerPointee), 1u);
    auto indirect = analyzeFfiWide(sysDir,
        "#include <ffiwide.h>\n"
        "int f(void){ long long a = 0; (*ffi_take_wide)(&a); return 0; }\n", kLp64);
    EXPECT_FALSE(indirect.hasErrors()) << "the indirect call admits it alike";
    EXPECT_EQ(countCode(indirect.diagnostics(),
                        DiagnosticCode::S_IncompatiblePointerIntegerPointee), 1u);
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
// `MirLoweringC.VolatileLongDoubleArithmeticEmitsNoExtraCast`).
TEST(TypeIdentityVocabulary, QualifiedLongDoubleOperandStillYieldsTheEntry) {
    auto const check = [](std::string const& decl, char const* wantVocab) {
        std::string const src =
            "int f(" + decl + " double d){ auto s = d + q; return s == 0.0; }\n";
        auto m = analyzeC(src, DataModel::Lp64, LongDoubleFormat::F64);
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
    nlohmann::json doc = loadShippedCJson();
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
    nlohmann::json doc = loadShippedCJson();
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

// ── P68 round 12 (lane `cs`, the enumeration P1): the `enumerationConstant` role and
//    the `enumerationCompatibleTypes` ladders load, and every malformed shape fails ──

namespace {
// Perturb the shipped semantics block (the whole object, so a test can remove the
// constant role the ladders depend on) and report whether the schema still loads.
[[nodiscard]] bool semanticsLoad(std::function<void(nlohmann::json&)> mutate) {
    nlohmann::json doc = loadShippedCJson();
    mutate(doc["semantics"]);
    return GrammarSchema::loadFromText(doc.dump(), "<semantics-perturbed>").has_value();
}
} // namespace

TEST(TypeIdentityVocabularyLoader, ShippedEnumerationTypingLoadsAndIsDeclared) {
    auto const schema = loadShippedSchema("c");
    ASSERT_NE(schema, nullptr);
    SemanticConfig const& cfg = schema->semantics();
    EXPECT_TRUE(cfg.enumerationConstantType.declared())
        << "C declares the type of an enumeration constant (`int`, C17 6.4.4.3p2)";
    ASSERT_TRUE(cfg.enumerationCompatibleTypes.declared());
    auto const* gnu  = cfg.enumerationCompatibleTypes.ladders(EnumCompatibleTypeRule::Gnu);
    auto const* msvc = cfg.enumerationCompatibleTypes.ladders(EnumCompatibleTypeRule::Msvc);
    ASSERT_NE(gnu, nullptr);
    ASSERT_NE(msvc, nullptr);
    EXPECT_EQ(cfg.enumerationCompatibleTypes.ladders(EnumCompatibleTypeRule::None), nullptr);
    // Each rung is EXACTLY the type the language gives its spelling — its core per data
    // model AND its vocabulary identity (C names only the types that share a kind:
    // `unsigned long` and `unsigned long long` are two U64 types; `int` and `unsigned
    // int` are their kinds' canonical, unnamed ones).
    auto const expectLadder = [&](std::vector<DataModelTypeRef> const& ladder,
                                  std::vector<char const*> const& spelled, char const* what) {
        ASSERT_EQ(ladder.size(), spelled.size()) << what;
        for (std::size_t i = 0; i < spelled.size(); ++i) {
            auto const want = resolveLanguageTypeName(*schema, spelled[i]);
            ASSERT_TRUE(want.has_value()) << spelled[i];
            EXPECT_EQ(ladder[i].core, want->core) << what << " rung " << i << ": " << spelled[i];
            EXPECT_EQ(ladder[i].coreByDataModel, want->coreByDataModel)
                << what << " rung " << i << ": " << spelled[i];
            EXPECT_EQ(ladder[i].vocabularyName, want->vocabularyName)
                << what << " rung " << i << ": " << spelled[i];
        }
    };
    expectLadder(gnu->unsignedLadder, {"unsigned int", "unsigned long", "unsigned long long"},
                 "gnu unsigned (gcc / clang: `unsigned int` first when no value is negative)");
    expectLadder(gnu->signedLadder, {"int", "long", "long long"}, "gnu signed");
    expectLadder(msvc->unsignedLadder, {"int", "unsigned int", "unsigned long long"},
                 "msvc unsigned (the Microsoft x64 ABI's `int` while `int` holds every value)");
    expectLadder(msvc->signedLadder, {"int", "long long"}, "msvc signed");
    EXPECT_EQ(gnu->unsignedLadder[1].vocabularyName, "unsigned long")
        << "a rung that shares its kind keeps its NAME";
}

TEST(TypeIdentityVocabularyLoader, EnumerationCompatibleTypesMalformedShapesRejected) {
    EXPECT_FALSE(semanticsLoad([](nlohmann::json& s) {
        s["enumerationCompatibleTypes"] = nlohmann::json::array();
    })) << "the block is an object keyed by format convention";
    EXPECT_FALSE(semanticsLoad([](nlohmann::json& s) {
        s["enumerationCompatibleTypes"]["mvsc"] = s["enumerationCompatibleTypes"]["msvc"];
    })) << "a key that is no convention spelling would silently serve no format";
    EXPECT_FALSE(semanticsLoad([](nlohmann::json& s) {
        s["enumerationCompatibleTypes"].erase("msvc");
    })) << "every convention must be covered: a pe64 format would find no ladders";
    EXPECT_FALSE(semanticsLoad([](nlohmann::json& s) {
        s["enumerationCompatibleTypes"]["gnu"] = nlohmann::json::array();
    })) << "each convention's entry is an object of two ladders";
    EXPECT_FALSE(semanticsLoad([](nlohmann::json& s) {
        s["enumerationCompatibleTypes"]["gnu"]["unsgined"] =
            s["enumerationCompatibleTypes"]["gnu"]["unsigned"];
    })) << "a typo'd ladder key would silently declare nothing";
    EXPECT_FALSE(semanticsLoad([](nlohmann::json& s) {
        s["enumerationCompatibleTypes"]["gnu"].erase("signed");
    })) << "both ladders are required: a missing one leaves a sign with no answer";
    EXPECT_FALSE(semanticsLoad([](nlohmann::json& s) {
        s["enumerationCompatibleTypes"]["msvc"]["unsigned"] = nlohmann::json::array();
    })) << "an empty ladder chooses nothing";
    EXPECT_FALSE(semanticsLoad([](nlohmann::json& s) {
        s["enumerationCompatibleTypes"]["gnu"]["signed"][1] = "double";
    })) << "a rung must be an integer type under every data model";
    EXPECT_FALSE(semanticsLoad([](nlohmann::json& s) {
        s["enumerationCompatibleTypes"]["gnu"]["signed"][0] = "unsigned int";
    })) << "an unsigned rung on the SIGNED ladder could never hold a negative value";
    EXPECT_FALSE(semanticsLoad([](nlohmann::json& s) {
        s["enumerationCompatibleTypes"]["gnu"]["unsigned"][2] = "unsigned looong";
    })) << "an unresolvable rung never silently no-ops";
    EXPECT_FALSE(semanticsLoad([](nlohmann::json& s) {
        s["synthesizedTypes"].erase("enumerationConstant");
    })) << "the ladders choose a compatible type BESIDE the constants' type, so the "
           "constant role must be declared too";
    // The one sign rule the loader does NOT impose: a SIGNED rung on the unsigned
    // ladder holds the non-negative values it is chosen for (the Microsoft ABI's `int`).
    EXPECT_TRUE(semanticsLoad([](nlohmann::json& s) {
        s["enumerationCompatibleTypes"]["gnu"]["unsigned"][0] = "int";
    }));
}

TEST(TypeIdentityVocabularyLoader, EnumerationTypingIsOptional) {
    EXPECT_TRUE(semanticsLoad([](nlohmann::json& s) {
        s.erase("enumerationCompatibleTypes");
        s["synthesizedTypes"].erase("enumerationConstant");
    })) << "a language that declares neither keeps each constant typed as its "
           "enumeration (the pre-P1 typing) — the mechanism is not C-specific";
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

// ── Loader validation: a builtin's PER-PAIR `signature` ─────────────────────
//
// A builtin whose prototype differs by build pair (`_InterlockedCompareExchange`
// takes a `LONG *`, and `long` is 64-bit on LP64 and 32-bit on LLP64) declares
// its `signature` as the object the shipped descriptors use —
// `{ "variants": [ { "when": {…}, "value": "fn(…)" }, …,
// { "default": true, "value": "fn(…)" } ] }` — decoded at LOAD by the ONE
// `when` decoder (core/types/variant_when.hpp) and SELECTED at injection with
// the pair the CU is compiled for. P68 round 12 (S2a-1 of
// D-C-STDLIB-H-LACKS-THIRTY-FIVE-ISO-NAMES) replaced the data-model-only
// `signatureByDataModel` map with it, and the closed key list now refuses the
// old key. The rules are the descriptor reader's: every arm decodes whether or
// not it is selected, a pair no arm selects is refused unless a `default` arm
// serves it, two arms selecting one pair are refused, and a `default` is
// written at most once.

namespace {
// Perturb the shipped `builtinFunctions` array and report whether the schema
// still loads.
[[nodiscard]] bool builtinFunctionsLoad(std::function<void(nlohmann::json&)> mutate) {
    nlohmann::json doc = loadShippedCJson();
    mutate(doc["semantics"]["builtinFunctions"]);
    return GrammarSchema::loadFromText(doc.dump(), "<builtins-perturbed>").has_value();
}

// The first shipped builtin whose `signature` is PER PAIR (an object).
[[nodiscard]] nlohmann::json& perPairBuiltin(nlohmann::json& arr) {
    for (auto& e : arr) {
        if (e.contains("signature") && e.at("signature").is_object()) return e;
    }
    throw std::runtime_error("no shipped builtinFunctions entry declares a per-pair 'signature'");
}

// Analyze `src` against the shipped C language with its builtins perturbed by
// `mutate`, on the pair (dm, ldf). The perturbation must LOAD — every defect
// these pins exercise is one the loader cannot see, because it needs a pair.
[[nodiscard]] SemanticModel analyzeWithBuiltins(std::function<void(nlohmann::json&)> const& mutate,
                                                std::string const& src, DataModel dm,
                                                LongDoubleFormat ldf = LongDoubleFormat::X87_80) {
    nlohmann::json doc = loadShippedCJson();
    mutate(doc["semantics"]["builtinFunctions"]);
    auto schema = GrammarSchema::loadFromText(doc.dump(), "<builtins-probe>");
    if (!schema.has_value())
        throw std::runtime_error("the perturbed builtins must LOAD for an injection-time pin");
    UnitBuilder builder{*schema, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(src, "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    return analyze(cu, DiagnosticBudget::libraryDefault(), dm, std::nullopt, std::nullopt,
                   std::nullopt, std::nullopt, ldf);
}

// True iff some diagnostic of `m` carries `needle`.
[[nodiscard]] bool mentions(SemanticModel const& m, std::string_view needle) {
    for (auto const& d : m.diagnostics().all()) {
        if (d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Every diagnostic of `m`, one per line — the "what DID it say, then?" payload.
[[nodiscard]] std::string diagnosticsOf(SemanticModel const& m) {
    std::string out;
    for (auto const& d : m.diagnostics().all()) {
        out += "\n  [";
        out += diagnosticCodeName(d.code);
        out += "] ";
        out += d.actual;
    }
    return out.empty() ? std::string{"\n  <no diagnostics>"} : out;
}

constexpr char const* kCasProbe = "int f(void){ return 0; }\n";
} // namespace

TEST(TypeIdentityVocabularyLoader, ShippedBuiltinFunctionsLoadUnperturbed) {
    EXPECT_TRUE(builtinFunctionsLoad([](nlohmann::json&) {}))
        << "fixture precondition: the SHIPPED builtins must load";
    // ... and the surface under test is actually EXERCISED by the shipped config
    // (otherwise every pin below would be testing an unused code path).
    nlohmann::json const doc = loadShippedCJson();
    auto const& arr = doc["semantics"]["builtinFunctions"];
    std::size_t perPair = 0;
    for (auto const& e : arr) {
        if (e.contains("signature") && e.at("signature").is_object()) ++perPair;
        EXPECT_FALSE(e.contains("signatureByDataModel"))
            << "the retired key survives on '" << e.value("name", std::string{}) << "'";
    }
    EXPECT_GE(perPair, 1u)
        << "at least one shipped builtin must carry a per-pair signature "
           "(`_InterlockedCompareExchange`'s `LONG*`)";
}

// The retired key is REFUSED, not ignored — on both builtin forms. Ignored, it
// would load clean and leave the base signature in force on every pair.
TEST(TypeIdentityVocabularyLoader, BuiltinRetiredSignatureByDataModelKeyRejected) {
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signatureByDataModel"] = {{"LLP64", "fn(u64) -> u64"}};
    })) << "the retired key on a per-pair row";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        for (auto& e : arr) {
            if (e.contains("signature")) continue;   // the params/result form
            e["signatureByDataModel"] = {{"LLP64", "fn(u64) -> u64"}};
            return;
        }
        ADD_FAILURE() << "no shipped builtin uses the params/result form";
    })) << "the retired key on a params/result row";
}

TEST(TypeIdentityVocabularyLoader, BuiltinSignatureArmUnknownWhenVocabularyRejected) {
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"][0]["when"] = {{"dataModel", "LP62"}};
    })) << "a typo'd data model can NEVER match — it would silently strand the arm";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"][0]["when"] = {{"longDoubleFormat", "x87-81"}};
    })) << "a typo'd long-double format can NEVER match";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"][0]["when"] = {{"ach", "x86_64"}};
    })) << "an unknown `when` key would widen the arm to every arch";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"][0]["when"] = nlohmann::json::object();
    })) << "an empty `when` would match every pair";
}

TEST(TypeIdentityVocabularyLoader, BuiltinSignatureArmMalformedShapesRejected) {
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"] = 42;
    })) << "neither a type-text string nor a per-pair object";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"] = nlohmann::json::object();
    })) << "a per-pair object with no `variants`";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"] = nlohmann::json::array();
    })) << "an EMPTY `variants`";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["fallback"] = "fn(i32) -> i32";
    })) << "an unknown key beside `variants` — `fallback` is the silent rule the form forbids";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"][0]["value"] = 42;
    })) << "each arm's value must be a non-empty signature STRING";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"][0]["value"] = "";
    })) << "an EMPTY value is indistinguishable from 'no arm'";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"][0].erase("when");
    })) << "an arm with neither `when` nor `default`";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        auto& v = perPairBuiltin(arr)["signature"]["variants"];
        v.push_back({{"default", true}, {"value", "fn(ptr<i32>, i32, i32) -> i32"}});
        v.push_back({{"default", true}, {"value", "fn(ptr<i32>, i32, i32) -> i32"}});
    })) << "at most ONE default arm";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"].push_back(
            {{"default", false}, {"value", "fn(ptr<i32>, i32, i32) -> i32"}});
    })) << "`default` is the literal true or absent";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"].push_back(
            {{"default", true}, {"when", {{"dataModel", "ILP32"}}},
             {"value", "fn(ptr<i32>, i32, i32) -> i32"}});
    })) << "a default arm carries no `when` — a guarded arm is not a default";
    // The control: ONE well-formed default arm loads.
    EXPECT_TRUE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"].push_back(
            {{"default", true}, {"value", "fn(ptr<i32>, i32, i32) -> i32"}});
    })) << "one default arm is the form's own spelling of 'every other pair'";
}

// The per-pair object is still a `signature`: the rule that forbids declaring
// both `signature` and `params`/`result` holds for it, and a type-generic
// `genericPointee` — which needs ONE exemplar signature — refuses several.
TEST(TypeIdentityVocabularyLoader, BuiltinPerPairSignatureExclusivityRules) {
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        for (auto& e : arr) {
            if (e.contains("signature")) continue;   // the params/result form
            e["signature"] = {{"variants", nlohmann::json::array({
                {{"when", {{"dataModel", "LP64"}}}, {"value", "fn(i32) -> i32"}}})}};
            return;
        }
        ADD_FAILURE() << "no shipped builtin uses the params/result form";
    })) << "a per-pair `signature` beside `params`/`result` is two declarations";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        perPairBuiltin(arr)["genericPointee"] = {
            {"bindFromParam", 0}, {"applyToParams", nlohmann::json::array({1, 2})},
            {"applyToResult", true}};
    })) << "`genericPointee` binds from ONE exemplar; a per-pair signature has several";
}

// The closed-key discriminator itself — the general fix, not just one key.
TEST(TypeIdentityVocabularyLoader, BuiltinFunctionUnknownKeyRejected) {
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        arr[0]["signatureByDataModl"] = {{"LLP64", "fn(u64) -> u64"}};
    })) << "a mis-spelled key must be rejected, not silently ignored";
    EXPECT_FALSE(builtinFunctionsLoad([](nlohmann::json& arr) {
        arr[0]["varadic"] = true;
    })) << "the typo discriminator covers every key";
    // `$`-prefixed documentation keys stay legal (the codebase-wide convention).
    EXPECT_TRUE(builtinFunctionsLoad([](nlohmann::json& arr) {
        arr[0]["$note"] = "documentation";
    }));
}

// The EAGER anti-lurking decode at the injection site (semantic_analyzer.cpp):
// EVERY declared text — each `when` arm AND the `default` — is decoded whatever
// the pair, so a malformed INACTIVE one fails on EVERY pair rather than lurking
// until its pair is first compiled. Observed end-to-end: the perturbed schema
// is analyzed on the OTHER data model and must still error.
TEST(TypeIdentityVocabulary, MalformedInactiveSignatureArmFailsOnEveryPair) {
    auto const withLlp64Arm = [](std::string text) {
        return [text](nlohmann::json& arr) {
            for (auto& arm : perPairBuiltin(arr)["signature"]["variants"]) {
                if (arm.contains("when") && arm["when"].value("dataModel", "") == "LLP64")
                    arm["value"] = text;
            }
        };
    };
    // The LLP64 arm is malformed. Under LLP64 it is the ACTIVE one...
    EXPECT_TRUE(analyzeWithBuiltins(withLlp64Arm("fn(ptr<"), kCasProbe, DataModel::Llp64).hasErrors());
    // ... and under LP64 it is INACTIVE, yet must STILL fail: an arm that only
    // breaks on the pair nobody built yet is the lurking-config defect.
    EXPECT_TRUE(analyzeWithBuiltins(withLlp64Arm("fn(ptr<"), kCasProbe, DataModel::Lp64).hasErrors())
        << "an INACTIVE malformed arm must fail on EVERY pair";
    // A well-formed arm under BOTH models is the clean control.
    EXPECT_FALSE(analyzeWithBuiltins(withLlp64Arm("fn(ptr<i32>, i32, i32) -> i32"), kCasProbe,
                                     DataModel::Lp64).hasErrors());
    // ... and a well-formed arm that is not a FUNCTION type fails too (the decode
    // must land an FnSig, never any type that happens to parse).
    EXPECT_TRUE(analyzeWithBuiltins(withLlp64Arm("i32"), kCasProbe, DataModel::Lp64).hasErrors())
        << "an arm must decode to a FUNCTION type";
    // ★ The DEFAULT arm is decoded eagerly too. Here an arm SELECTS the pair, so
    // the default is never chosen — and it must still fail, or a malformed
    // default would lurk until a pair with no arm of its own compiled.
    auto const withBadDefault = [](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"].push_back(
            {{"default", true}, {"value", "fn(ptr<"}});
    };
    EXPECT_TRUE(analyzeWithBuiltins(withBadDefault, kCasProbe, DataModel::Lp64).hasErrors())
        << "an unselected malformed DEFAULT must fail on every pair";
}

// NO SILENT FALLBACK at injection. A pair no arm selects, with no `default`, is
// refused and NAMED — the retired map handed such a pair its base (LP64) text.
// A `default` arm is what makes the same pair legal; two arms selecting one
// pair are refused rather than resolved by order.
//
// ⚠ The unselected pair is made with the LONG-DOUBLE axis, not a third data
// model: ILP32 is refused by the analyzer on its own
// (`Fc3WidthSemantics.Ilp32SelectionFailsLoud`), so a pair that errors there
// could not tell a builtin refusal from the data-model one.
TEST(TypeIdentityVocabulary, PerPairBuiltinSignatureSelectionIsExact) {
    auto const noChange = [](nlohmann::json&) {};
    // The shipped row declares LP64 and LLP64 arms and no default: both select.
    EXPECT_FALSE(analyzeWithBuiltins(noChange, kCasProbe, DataModel::Lp64).hasErrors());
    EXPECT_FALSE(analyzeWithBuiltins(noChange, kCasProbe, DataModel::Llp64).hasErrors());
    // One arm, for x87-80 alone, and no default.
    auto const x87Only = [](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"] = nlohmann::json::array({
            {{"when", {{"longDoubleFormat", "x87-80"}}},
             {"value", "fn(ptr<i32>, i32, i32) -> i32"}}});
    };
    EXPECT_FALSE(analyzeWithBuiltins(x87Only, kCasProbe, DataModel::Lp64, LongDoubleFormat::X87_80)
                     .hasErrors()) << "control: the arm selects x87-80";
    {
        auto const m =
            analyzeWithBuiltins(x87Only, kCasProbe, DataModel::Lp64, LongDoubleFormat::F64);
        EXPECT_TRUE(m.hasErrors()) << "f64 selects no arm and the row has no default";
        EXPECT_TRUE(mentions(m, "no 'signature' variant matches this pair"));
        EXPECT_TRUE(mentions(m, "long-double format 'f64'")) << "the refusal names the pair";
    }
    auto const x87AndDefault = [&](nlohmann::json& arr) {
        x87Only(arr);
        perPairBuiltin(arr)["signature"]["variants"].push_back(
            {{"default", true}, {"value", "fn(ptr<i32>, i32, i32) -> i32"}});
    };
    EXPECT_FALSE(analyzeWithBuiltins(x87AndDefault, kCasProbe, DataModel::Lp64,
                                     LongDoubleFormat::F64).hasErrors())
        << "the default serves the pair no arm selects";
    auto const withSecondLp64Arm = [](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"].push_back(
            {{"when", {{"dataModel", "LP64"}}}, {"value", "fn(ptr<i32>, i32, i32) -> i32"}});
    };
    {
        auto const m = analyzeWithBuiltins(withSecondLp64Arm, kCasProbe, DataModel::Lp64);
        EXPECT_TRUE(m.hasErrors()) << "two arms select LP64";
        EXPECT_TRUE(mentions(m, "2 'signature' variants match this pair"));
    }
    EXPECT_FALSE(analyzeWithBuiltins(withSecondLp64Arm, kCasProbe, DataModel::Llp64).hasErrors())
        << "control: LLP64 still selects exactly one";
}

// The long-double format selects too — the axis S2a-1 added, observed through
// the TYPE the builtin's call gets: the x87-80 arm returns `long long` and the
// default `int`, so a `_Generic` over the call says which arm the pair chose.
// (`_Generic`, not `sizeof`: with no target layout in hand — this analysis has
// none — `sizeof` of a call is not folded to a constant, ✔MEASURED at S2a-1:
// S_StaticAssertFailed "not an integer constant expression".)
TEST(TypeIdentityVocabulary, PerPairBuiltinSignatureSelectsByLongDoubleFormat) {
    auto const byLongDouble = [](nlohmann::json& arr) {
        perPairBuiltin(arr)["signature"]["variants"] = nlohmann::json::array({
            {{"when", {{"longDoubleFormat", "x87-80"}}},
             {"value", "fn(ptr<i32>, i32, i32) -> i64 \"long long\""}},
            {{"default", true}, {"value", "fn(ptr<i32>, i32, i32) -> i32"}}});
    };
    std::string const wide =
        "int x;\n_Static_assert(_Generic(_InterlockedCompareExchange(&x, 0, 0), long long: 1, "
        "default: 0), \"x87 arm\");\n";
    std::string const narrow =
        "int x;\n_Static_assert(_Generic(_InterlockedCompareExchange(&x, 0, 0), int: 1, "
        "default: 0), \"default\");\n";
    {
        auto const m = analyzeWithBuiltins(byLongDouble, wide, DataModel::Lp64, LongDoubleFormat::X87_80);
        EXPECT_FALSE(m.hasErrors()) << "x87-80 selects its arm" << diagnosticsOf(m);
    }
    {
        auto const m = analyzeWithBuiltins(byLongDouble, narrow, DataModel::Lp64, LongDoubleFormat::F64);
        EXPECT_FALSE(m.hasErrors()) << "f64 selects no arm: the default serves it" << diagnosticsOf(m);
    }
    // The negative arms: each pair REFUSES the other's size.
    EXPECT_TRUE(analyzeWithBuiltins(byLongDouble, narrow, DataModel::Lp64, LongDoubleFormat::X87_80)
                    .hasErrors());
    EXPECT_TRUE(analyzeWithBuiltins(byLongDouble, wide, DataModel::Lp64, LongDoubleFormat::Ieee128)
                    .hasErrors());
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
        auto m = analyzeC(src, dm);
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
        auto m = analyzeC(src, DataModel::Lp64, axis);
        expectGenericClean(m);
        EXPECT_EQ(selectedGenericArms(m),
                  (std::vector<std::string>{"1", "2", "3"}))
            << "`1.0L` IS `long double`, `1.0` IS the anonymous `double`, "
               "`1.0f` IS `float` — on EVERY long-double axis";
    }
}

// ── P68 round 10 (lane `cs`, D-LANG-UAC-UNSIGNED-COUNTERPART-OF-SIGNED): C 6.3.1.8's
// FIFTH CONVERSION ─────────────────────────────────────────────────────────────────
// When the signed operand ranks higher (C 6.3.1.1, by NAME) but has the SAME width as
// the unsigned one, it cannot represent every unsigned value, and both convert to the
// UNSIGNED COUNTERPART of the signed type: LP64 `long long + unsigned long` is `unsigned
// long long`, LLP64 `long + unsigned int` is `unsigned long`. The same VALUE and width
// as the unsigned operand's own type — only `_Generic` / `typeof` can tell, which is
// why this was a NAME-only divergence. ✔MEASURED 2026-09-24 (the lane's
// `.temp/probe/uac`, each case one translation unit, every build RUN): gcc 13.3.0 and
// clang 18.1.3 at `-std=c17 -pedantic-errors` and `-std=c2x` (LP64), mingw-w64 13.2.0
// and MSVC 19.51 at both of their modes (LLP64) select exactly the arms below; DSS
// selected the unsigned operand's own type. The widths that DIFFER are the controls:
// a wider signed type represents every value (the fourth conversion).
TEST(TypeIdentityVocabulary, ASameWidthMixedSignednessPairTakesTheSignedTypesUnsignedCounterpart) {
    std::string const src =
        "int f(long long a, unsigned long b, long c, unsigned int d, int k){\n"
        "  return _Generic(a + b, unsigned long long: 11, unsigned long: 12, long long: 13, default: 14)\n"
        "       + _Generic(b + a, unsigned long long: 21, unsigned long: 22, long long: 23, default: 24)\n"
        "       + _Generic(c + d, unsigned long: 31, unsigned int: 32, long: 33, default: 34)\n"
        "       + _Generic(d + c, unsigned long: 41, unsigned int: 42, long: 43, default: 44)\n"
        "       + _Generic(k ? a : b, unsigned long long: 51, unsigned long: 52, long long: 53, default: 54);\n"
        "}\n";
    {
        SCOPED_TRACE("LP64: long long / unsigned long are both 64-bit, long / unsigned int are not");
        auto m = analyzeC(src, DataModel::Lp64);
        expectGenericClean(m);
        EXPECT_EQ(selectedGenericArms(m), (std::vector<std::string>{"11", "21", "33", "43", "51"}));
    }
    {
        SCOPED_TRACE("LLP64: long / unsigned int are both 32-bit, long long / unsigned long are not");
        auto m = analyzeC(src, DataModel::Llp64);
        expectGenericClean(m);
        EXPECT_EQ(selectedGenericArms(m), (std::vector<std::string>{"13", "23", "31", "41", "53"}));
    }
}

// P68 round 12 (lane `cs`): an enumeration with a FIXED underlying type (C23 6.7.2.2) has
// that type's RANK (C 6.3.1.1p1), and rank is by NAME — so `enum E : long` meets an
// `unsigned int` as `long` does: the fifth conversion on LLP64 (`unsigned long`), the
// fourth on LP64 (`long`). The enum record keeps its underlying type AS DECLARED; it
// kept only the kind, and an anonymous I32 / I64 took the unsigned operand's type
// (LLP64 `unsigned int`) or no named type at all (LP64). The enumeration CONSTANT has the
// enumerated type too, and an enum WITHOUT a fixed type is the `int` control.
// ✔MEASURED 2026-09-24 (lane `cs`'s `.temp/probe/uacx`): gcc 13.3.0, clang 18.1.3 and
// mingw-w64 13.2.0 at -std=c2x build and run every shape to C's answer; MSVC 19.51 has no
// fixed underlying types and abstains.
TEST(TypeIdentityVocabulary, AnEnumWithAFixedUnderlyingTypeHasThatTypesRank) {
    std::string const src =
        "enum E : long { A = 1 };\n"
        "enum F : long long { B = 1 };\n"
        "enum G { C = 1 };\n"
        "int f(enum E e, enum F g, enum G h, unsigned int u, unsigned long ul) {\n"
        "  return _Generic(e + u, unsigned long: 11, long: 12, unsigned int: 13, default: 14)\n"
        "       + _Generic(A + u, unsigned long: 21, long: 22, unsigned int: 23, default: 24)\n"
        "       + _Generic(g + ul, unsigned long long: 31, long long: 32, unsigned long: 33, default: 34)\n"
        "       + _Generic(h + u, unsigned int: 41, int: 42, default: 43);\n"
        "}\n";
    {
        SCOPED_TRACE("LLP64: `long` / `unsigned int` share a width — the fifth conversion");
        auto m = analyzeC(src, DataModel::Llp64);
        expectGenericClean(m);
        EXPECT_EQ(selectedGenericArms(m), (std::vector<std::string>{"11", "21", "32", "41"}));
    }
    {
        SCOPED_TRACE("LP64: `long` is wider than `unsigned int`; `long long` / `unsigned long` share one");
        auto m = analyzeC(src, DataModel::Lp64);
        expectGenericClean(m);
        EXPECT_EQ(selectedGenericArms(m), (std::vector<std::string>{"12", "22", "31", "41"}));
    }
}

// P68 round 12 (lane `cs`): C23's enum-type-specifier is a specifier-qualifier-LIST, and
// the underlying type is "the unqualified, non-atomic version" of what it names (C23
// 6.7.2.2) — so `enum E : const long`, `: volatile long` and `: _Atomic long` are `long`
// enums, ranked as `long`. ✔MEASURED 2026-09-24 (lane `cs`'s `.temp/probe/enq`): gcc 13.3.0
// and mingw-w64 13.2.0 at -std=c2x build and run all three to that answer, clang 18.1.3 the
// first two (it refuses `_Atomic long` as "non-integral"; gcc's acceptance is the
// disjunction's), MSVC 19.51 abstains; DSS refused all three at PARSE — the clause took a
// qualifier-free base only.
TEST(TypeIdentityVocabulary, AQualifiedFixedUnderlyingTypeIsItsUnqualifiedType) {
    std::string const src =
        "enum E : const long { A = 1 };\n"
        "enum F : volatile long { B = 1 };\n"
        "enum G : _Atomic long { C = 1 };\n"
        "int f(enum E e, enum F g, enum G h, unsigned int u) {\n"
        "  return _Generic(e + u, unsigned long: 11, long: 12, default: 13)\n"
        "       + _Generic(g + u, unsigned long: 21, long: 22, default: 23)\n"
        "       + _Generic(h + u, unsigned long: 31, long: 32, default: 33);\n"
        "}\n";
    {
        SCOPED_TRACE("LLP64: the fifth conversion, as for an unqualified `long`");
        auto m = analyzeC(src, DataModel::Llp64);
        expectGenericClean(m);
        EXPECT_EQ(selectedGenericArms(m), (std::vector<std::string>{"11", "21", "31"}));
    }
    {
        SCOPED_TRACE("LP64: the fourth conversion, as for an unqualified `long`");
        auto m = analyzeC(src, DataModel::Lp64);
        expectGenericClean(m);
        EXPECT_EQ(selectedGenericArms(m), (std::vector<std::string>{"12", "22", "32"}));
    }
}

// The loader's half: under `rank-prefer-unsigned` every NAMED signed entry must have
// ONE unsigned counterpart — a named entry of the same `rank` whose core is the
// unsigned twin — under every data model. Both perturbations below keep every row
// loadable on its own (no name disappears — `synthesizedTypes` still resolves), so
// the refusal is this check's and no other; the message is read, not just the fact.
namespace {
[[nodiscard]] std::string typeSpecifiersLoadErrors(std::function<void(nlohmann::json&)> mutate) {
    nlohmann::json doc = loadShippedCJson();
    mutate(doc["semantics"]["typeSpecifiers"]);
    auto const loaded = GrammarSchema::loadFromText(doc.dump(), "<vocab-perturbed>");
    if (loaded.has_value()) return {};
    std::string all;
    for (auto const& d : loaded.error()) all += d.message + "\n";
    return all.empty() ? std::string{"<refused with no message>"} : all;
}
}  // namespace

TEST(TypeIdentityVocabularyLoader, ASignedEntryWithoutAnUnsignedCounterpartIsRefused) {
    // `unsigned long long` re-ranked 5: no rank-4 unsigned entry is left for `long long`
    // (and `unsigned __int128`, U128 at rank 5, is no twin of an I64). THE CONTROL is
    // ArbitraryOpaqueNameAccepted above: it names the 16-bit `short` with no named
    // unsigned twin, and must still LOAD — `short` is promoted to `int` before any
    // conversion decision, so only an entry that survives promotion needs a counterpart.
    std::string const errors = typeSpecifiersLoadErrors([](nlohmann::json& rows) {
        for (auto& r : rows)
            if (r.value("name", std::string{}) == "unsigned long long") r["rank"] = 5;
    });
    EXPECT_NE(errors.find("'long long' has no unsigned counterpart"), std::string::npos)
        << "refused for its OWN reason:\n" << errors;
}

TEST(TypeIdentityVocabularyLoader, ASignedEntryWithTwoUnsignedCounterpartsIsRefused) {
    // One spelling of `unsigned long long` renamed: a SECOND rank-4 U64 entry, so
    // `long long` has two candidate counterparts and "the" counterpart is no one type.
    std::string const errors = typeSpecifiersLoadErrors([](nlohmann::json& rows) {
        rows[rowIndexFor(rows, {"UnsignedKeyword", "LongKeyword", "LongKeyword", "IntKeyword"})]["name"] =
            "unsigned long long int";
    });
    EXPECT_NE(errors.find("'long long' has more than one unsigned counterpart"), std::string::npos)
        << "refused for its OWN reason:\n" << errors;
}
