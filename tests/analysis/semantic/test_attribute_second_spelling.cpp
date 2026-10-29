// ═══════════════════════════════════════════════════════════════════════════
//  THE SECOND SPELLING OF THE ATTRIBUTE SPECIFIER — `__declspec(...)`
//  cycle P69, lane `cs`
//
//  Until this cycle `__declspec(x)` was a function-like predefined macro of the
//  pe pairs that expanded to NOTHING. Every request written in it was erased
//  before the parser saw it: `__declspec(align(32)) int v;` was a 4-aligned
//  object, `__declspec(thread) int t;` was ONE object shared by every thread,
//  and neither said a word. It is now a second frame of the attribute specifier
//  — a keyword, a spelling row of the language document, and rows of the same
//  tables every other attribute is read from.
// ═══════════════════════════════════════════════════════════════════════════
//
// ── THE REFERENCE MATRIX (each cell compiled and RUN through `dssharness run
//    probe-reference-cc`, 2026-10-08): cl 19.51 | mingw-w64 gcc 13 ───────────
//
//  WHERE IT EXISTS. Both compile it for Windows. Off Windows gcc has no such
//  word (a syntax error) and clang refuses it by name. Here it is a spelling of
//  the pe object format only (`attributeSemantics.spellings`), and anywhere
//  else — and where no format is in scope — a specifier written in it is
//  refused by name, once.
//
//  THE MODIFIER LIST. `__declspec()` runs on both. An unknown modifier: cl
//  error | MinGW warns and runs — warned here. Several modifiers in one
//  specifier, separated by white space or by commas: cl accepts both forms,
//  MinGW neither (its `__declspec` is a one-argument macro) — both accepted
//  here. A GNU name inside (`__declspec(aligned(16))`): cl error | MinGW 16 —
//  so a name with no row of its own under this spelling is looked up under its
//  plain name.
//
//  `align`, cl's own rule (MinGW does not know the word and ignores it):
//      __declspec(align(32)) int v;                 the object, 32
//      __declspec(align(2))  int v;                 4 — a request can only RAISE
//      typedef __declspec(align(32)) int t;         the alias, 32
//      typedef __declspec(align(1))  int t;         4
//      __declspec(align(32)) __declspec(align(16))  16 — the LAST one stands, and
//                                                   the other is warned (C4141)
//      __declspec(align(32)) struct S {…} s;        the TYPE: _Alignof(struct S) 32,
//                                                   a later `struct S t;` is 32 too
//      __declspec(align(32)) struct S {…};          the type, with no declarator
//      __declspec(align(32)) union U {…} u;         the type
//      __declspec(align(32)) enum E {…} e;          the OBJECT; the enum stays 4
//      __declspec(align(32)) struct R r;            (R only referred to) the object
//      struct A {…} __declspec(align(32)) a;        the object; struct A stays 4
//      struct M { char c; __declspec(align(32)) int m; }   m at 32, sizeof 64
//      _Alignas(16) __declspec(align(32)) int v;    32 — beside `_Alignas` the
//      _Alignas(32) __declspec(align(16)) int v;    32   larger of the two stands
//      __declspec(align) int v;                     cl error | MinGW: GNU's bare
//                                                   `aligned`. Ignored and warned
//                                                   here: neither meaning is cl's.
//      __declspec(align(32)) void f(void);          cl error | MinGW ignores —
//                                                   ignored and warned here
//
//  `thread`: cl gives the object THREAD storage (a second thread's write is not
//  seen by the first), MinGW ignores the word with a warning. cl's meaning is
//  taken: a program written for it is wrong without it. On a function or a
//  typedef: cl error | MinGW ignores and warns — ignored and warned here.
//  TWO DECLARATIONS OF ONE OBJECT THAT DISAGREE, `extern __declspec(thread) int
//  e; int e = 1;`: both compile it and both run it with ONE object shared by
//  every thread (a second thread's write IS seen); the opposite order is a cl
//  error and runs on MinGW. So the request YIELDS here, in either order, and is
//  warned — which the keyword forms (`_Thread_local`) never do: for them the
//  disagreement stays the constraint violation of C 6.7.1p3.
//
//  THE REST. `noreturn`, `noinline`, `deprecated` (with or without a message)
//  and `selectany` mean what their plain names mean. `dllimport` / `dllexport`
//  on a function or a datum of an executable: both run; inert here. `restrict`
//  / `noalias` on a function: both run; inert. `naked` and `allocate` ask for
//  something this compiler does not do and are refused by name.
//
// Every pin asserts what a reference printed or said, never merely "no error":
// the defect this file exists for was a clean compile that dropped the request.

#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include "semantic_test_fixture.hpp"
#include "test_support/repo_root.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

// Layout parameters are REQUIRED: without them no alignment is applied at all, so
// every "is it 32" pin would be red for the wrong reason and every "it stayed 4"
// pin green for the wrong one.
constexpr AggregateLayoutParams kLayout{
    .scalarAlignment       = ScalarAlignmentRule::Natural,
    .maxAlignment          = 16,
    .maxRequestedAlignment = 268435456};

// The analysis as the pipeline runs it for one object format — or for none, the
// editor's analysis, where a spelling that names its formats cannot be judged.
[[nodiscard]] SemanticModel analyzeOn(std::optional<ObjectFormatKind> format,
                                      std::initializer_list<std::string> srcs) {
    auto cu = buildShippedUnit("c", srcs);
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kLayout,
                   std::nullopt,
                   format.has_value()
                       ? std::optional<SelectableObjectFormatKind>{
                             SelectableObjectFormatKind::of(*format)}
                       : std::nullopt);
}

[[nodiscard]] SemanticModel analyzePe(std::initializer_list<std::string> srcs) {
    return analyzeOn(ObjectFormatKind::Pe, srcs);
}

[[nodiscard]] SymbolRecord const* findSym(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return &m.symbols()[i];
    return nullptr;
}

// EVERY record of a name: a redeclared object has one per declaration, and the
// thread-storage pins must hold on all of them — a later tier may read either.
[[nodiscard]] std::vector<SymbolRecord const*> symsNamed(SemanticModel const& m,
                                                        std::string_view name) {
    std::vector<SymbolRecord const*> out;
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) out.push_back(&m.symbols()[i]);
    return out;
}

struct SizeAlign {
    std::uint64_t size  = 0;
    std::uint32_t align = 0;
    friend bool operator==(SizeAlign const&, SizeAlign const&) = default;
};
std::ostream& operator<<(std::ostream& os, SizeAlign const& sa) {
    return os << "{size " << sa.size << ", align " << sa.align << "}";
}

[[nodiscard]] SizeAlign layoutOfType(SemanticModel const& m, TypeId t) {
    auto const l = computeLayout(t, m.lattice().interner(), kLayout, DataModel::Lp64);
    EXPECT_TRUE(l.has_value()) << "the type has no layout";
    return l ? SizeAlign{static_cast<std::uint64_t>(l->size), l->align.bytes()}
             : SizeAlign{};
}

// What a NAME's TYPE lays out at. An alignment requested for the object, not for
// its type, is `SymbolRecord::explicitAlignment` and is asserted separately: the
// references keep the two apart, and so does every pin below.
[[nodiscard]] SizeAlign layoutOf(SemanticModel const& m, char const* name) {
    SymbolRecord const* s = findSym(m, name);
    EXPECT_NE(s, nullptr) << "no symbol named '" << name << "'";
    return s == nullptr ? SizeAlign{} : layoutOfType(m, s->type);
}

[[nodiscard]] std::optional<std::uint32_t> objectAlignment(SemanticModel const& m,
                                                           char const* name) {
    SymbolRecord const* s = findSym(m, name);
    EXPECT_NE(s, nullptr) << "no symbol named '" << name << "'";
    return s == nullptr ? std::nullopt : s->explicitAlignment;
}

// How many diagnostics carry `code` and say `needle`.
[[nodiscard]] std::size_t countSaying(SemanticModel const& m, DiagnosticCode code,
                                      std::string_view needle) {
    std::size_t n = 0;
    for (auto const& d : m.diagnostics().all())
        if (d.code == code && d.actual.find(needle) != std::string::npos) ++n;
    return n;
}

[[nodiscard]] std::string everyDiagnostic(SemanticModel const& m) {
    std::string out;
    for (auto const& d : m.diagnostics().all()) {
        out += "\n  ";
        out += diagnosticCodeName(d.code);
        out += ": ";
        out += d.actual;
    }
    return out.empty() ? std::string{" <none>"} : out;
}

// The shipped C document with every occurrence of ONE piece of its text replaced,
// loaded as a language of its own: how a pin asks what the ENGINE does under a row
// the shipped language does not have. `expected` is how many occurrences the edit
// must find — a count that moved means the document changed under the pin, and a
// pin that then edited nothing would be asking the shipped language again.
[[nodiscard]] std::shared_ptr<GrammarSchema const>
shippedCWith(std::string_view from, std::string_view to, std::size_t expected) {
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return nullptr;
    }
    std::ifstream in(*root / "sources" / "c.lang.json", std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot read the shipped c document";
        return nullptr;
    }
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    std::size_t edits = 0;
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
        ++edits;
    }
    EXPECT_EQ(edits, expected) << "occurrences of the text this pin edits";
    auto loaded = GrammarSchema::loadFromText(text, "<shipped c, edited>");
    if (!loaded.has_value()) {
        ADD_FAILURE() << "the edited document must still load: "
                      << (loaded.error().empty() ? "<no diagnostics>"
                                                 : loaded.error().back().message);
        return nullptr;
    }
    return *loaded;
}

[[nodiscard]] SemanticModel analyzePeUnder(std::shared_ptr<GrammarSchema const> schema,
                                           std::string src) {
    UnitBuilder builder{std::move(schema), DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), "<mem0>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kLayout,
                   std::nullopt, SelectableObjectFormatKind::of(ObjectFormatKind::Pe));
}

} // namespace

// ── WHERE THE SPELLING EXISTS ──────────────────────────────────────────────

// A specifier written in a spelling that the unit's object format does not have is
// refused BY NAME, ONCE PER SPECIFIER, and nothing in it is judged a second time
// (no "unknown attribute" beside the refusal). With NO format in scope the question
// cannot be answered, and "unknown" must not read as "available".
//
// RED-ON-DISABLE: drop the refusal in `judgeAttributeSpecifier` and the three
// off-format rows analyze clean; make `AttributeSpelling::availableFor` answer
// true for an absent format and the no-format row does.
TEST(AttributeSecondSpelling, IsRefusedByNameOffTheFormatsItsRowNames) {
    constexpr char const* kSrc = "__declspec(noinline) int f(void);\n"
                                 "__declspec(deprecated) int g;\n";
    {
        auto m = analyzePe({kSrc});
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 0u);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_UnknownAttribute), 0u);
        ASSERT_NE(findSym(m, "f"), nullptr);
        EXPECT_TRUE(findSym(m, "f")->isNoInline);
        ASSERT_NE(findSym(m, "g"), nullptr);
        EXPECT_TRUE(findSym(m, "g")->isDeprecated);
    }
    struct Off {
        ObjectFormatKind format;
        char const*      name;
    };
    for (Off const off : {Off{ObjectFormatKind::Elf, "'elf'"},
                          Off{ObjectFormatKind::MachO, "'macho'"}}) {
        auto m = analyzeOn(off.format, {kSrc});
        EXPECT_TRUE(m.hasErrors()) << off.name;
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 2u)
            << off.name << " — once per specifier:" << everyDiagnostic(m);
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeNotHonoured, "'__declspec'"),
                  2u)
            << off.name << " — the refusal names the spelling";
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeNotHonoured, off.name), 2u)
            << "…the format the unit is compiled for";
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeNotHonoured, "'pe'"), 2u)
            << off.name << " — …and the one the spelling exists for";
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_UnknownAttribute), 0u)
            << off.name << " — a refused specifier's clauses are not judged again";
    }
    {
        auto m = analyzeOn(std::nullopt, {kSrc});
        EXPECT_TRUE(m.hasErrors());
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeNotHonoured,
                              "no object format in scope"),
                  2u)
            << everyDiagnostic(m);
    }
    // CONTROL: the plain spelling of the same two requests exists everywhere, with
    // or without a format in scope.
    for (std::optional<ObjectFormatKind> const format :
         {std::optional<ObjectFormatKind>{ObjectFormatKind::Pe},
          std::optional<ObjectFormatKind>{ObjectFormatKind::Elf},
          std::optional<ObjectFormatKind>{ObjectFormatKind::MachO},
          std::optional<ObjectFormatKind>{}}) {
        auto m = analyzeOn(format, {"__attribute__((noinline)) int f(void);\n"
                                    "__attribute__((deprecated)) int g;\n"});
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 0u);
    }
}

// ── THE MODIFIER LIST ──────────────────────────────────────────────────────

// An EMPTY specifier says nothing in either spelling (`__declspec()` runs on cl and
// on MinGW; `__attribute__(())` is GNU's own empty form), and the introducer
// itself is never read as a modifier's name. An UNKNOWN modifier in a
// declaration's prefix is this tier's business under neither spelling: the word is
// reported once by the lowering's linkage scan, which is where the plain
// spelling's unknown word is reported too (`HirLoweringC.AModelledModifierOfThe
// SecondSpellingIsNotAnUnknownLinkageSpecifier` pins the pair); in a TYPE NAME,
// where no such scan runs, it is this tier that says it — once, in each spelling.
//
// RED-ON-DISABLE: read a specifier's opening token as a clause name
// (`attrClauseNameToken`) and the two empty specifiers written in a type name are
// each an unknown attribute named after its own introducer.
TEST(AttributeSecondSpelling, AnEmptySpecifierIsSilentAndAnUnknownModifierIsSaidOnce) {
    for (char const* src : {"__declspec() int a;\n", "__attribute__(()) int a;\n",
                            "char probe[sizeof(__declspec() int)];\n",
                            "char probe[sizeof(__attribute__(()) int)];\n"}) {
        auto m = analyzePe({src});
        EXPECT_FALSE(m.hasErrors()) << src << everyDiagnostic(m);
        EXPECT_TRUE(m.diagnostics().all().empty()) << src << everyDiagnostic(m);
    }
    for (char const* src : {"__declspec(frobnicate) int a;\n",
                            "__attribute__((frobnicate)) int a;\n"}) {
        auto m = analyzePe({src});
        EXPECT_FALSE(m.hasErrors()) << src << everyDiagnostic(m);
        EXPECT_TRUE(m.diagnostics().all().empty())
            << src << " — reported by the lowering, not here:" << everyDiagnostic(m);
    }
    for (char const* src : {"char probe[sizeof(__declspec(frobnicate) int)];\n",
                            "char probe[sizeof(__attribute__((frobnicate)) int)];\n"}) {
        auto m = analyzePe({src});
        EXPECT_FALSE(m.hasErrors()) << src << everyDiagnostic(m);
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_UnknownAttribute, "frobnicate"), 1u)
            << src << everyDiagnostic(m);
        EXPECT_EQ(m.diagnostics().all().size(), 1u) << src << everyDiagnostic(m);
    }
}

// SEVERAL MODIFIERS IN ONE SPECIFIER, in both of cl's forms, are each honoured; and
// a name that has no row of its own under this spelling is looked up under its
// plain one — so the GNU word inside the frame means what MinGW makes it mean.
//
// RED-ON-DISABLE: drop the optional comma of the grammar's second frame and the
// white-space row is a parse error; drop the plain-name fallback of
// `extractOneAttrClause` and `aligned` inside the frame is an unknown attribute.
TEST(AttributeSecondSpelling, SeveralModifiersInOneSpecifierAreEachHonoured) {
    for (char const* src :
         {"__declspec(noinline noreturn) void f(void);\n",
          "__declspec(noinline, noreturn) void f(void);\n",
          "__declspec(noinline) __declspec(noreturn) void f(void);\n",
          "__declspec(noreturn) void __declspec(noinline) f(void);\n"}) {
        auto m = analyzePe({src});
        EXPECT_FALSE(m.hasErrors()) << src << everyDiagnostic(m);
        EXPECT_TRUE(m.diagnostics().all().empty()) << src << everyDiagnostic(m);
        ASSERT_NE(findSym(m, "f"), nullptr) << src;
        EXPECT_TRUE(findSym(m, "f")->isNoInline) << src;
        EXPECT_TRUE(findSym(m, "f")->isNoreturn) << src;
    }
    auto m = analyzePe({"__declspec(aligned(16)) int g;\n"});
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    EXPECT_TRUE(m.diagnostics().all().empty()) << everyDiagnostic(m);
    EXPECT_EQ(objectAlignment(m, "g"), std::optional<std::uint32_t>{16u});
}

// ── `align` ────────────────────────────────────────────────────────────────

// ON AN OBJECT AND ON A TYPE ALIAS the request can only RAISE: below the natural
// alignment it changes nothing and says nothing (cl). The plain spelling's
// `aligned` on an alias takes exactly what it asks, a weaker alignment included —
// which is the whole difference the qualified row's `onTypeAlias` key carries.
//
// RED-ON-DISABLE: drop `onTypeAlias: raises` from the shipped row and `t1` lays
// out at 1; send the raise-only slot to the exact one in the fold and the same.
TEST(AttributeSecondSpelling, AlignRaisesAnObjectOrAnAliasAndNeverLowersOne) {
    auto m = analyzePe({"__declspec(align(32)) int v32;\n"
                        "__declspec(align(2)) int v2;\n"
                        "int __declspec(align(32)) mid32;\n"
                        "typedef __declspec(align(32)) int t32;\n"
                        "__declspec(align(32)) typedef int lead32;\n"
                        "typedef __declspec(align(1)) int t1;\n"
                        "t32 o32;\n"
                        "struct H { char c; t32 m; };\n"
                        "struct H h;\n"});
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    EXPECT_TRUE(m.diagnostics().all().empty()) << everyDiagnostic(m);
    EXPECT_EQ(objectAlignment(m, "v32"), std::optional<std::uint32_t>{32u});
    EXPECT_EQ(layoutOf(m, "v32"), (SizeAlign{4, 4})) << "the object's TYPE is still int";
    EXPECT_EQ(objectAlignment(m, "mid32"), std::optional<std::uint32_t>{32u});
    // A request below the natural alignment leaves the object at its type's.
    if (auto const a = objectAlignment(m, "v2"); a.has_value()) {
        EXPECT_LE(*a, 4u);
    }
    EXPECT_EQ(layoutOf(m, "t32"), (SizeAlign{4, 32}));
    EXPECT_EQ(layoutOf(m, "lead32"), (SizeAlign{4, 32}));
    EXPECT_EQ(layoutOf(m, "t1"), (SizeAlign{4, 4})) << "the alias is not lowered";
    EXPECT_EQ(layoutOf(m, "o32"), (SizeAlign{4, 32}));
    EXPECT_EQ(layoutOf(m, "h"), (SizeAlign{64, 32}))
        << "a member of the raised alias sits at 32 and the structure is 64 bytes";
    // CONTROL: the plain spelling's alias takes exactly what it asks.
    auto g = analyzePe({"typedef int g1 __attribute__((aligned(1)));\n"});
    EXPECT_FALSE(g.hasErrors()) << everyDiagnostic(g);
    EXPECT_EQ(layoutOf(g, "g1"), (SizeAlign{4, 1}));
}

// OF SEVERAL REQUESTS ON ONE DECLARATION THE LAST ONE STANDS, and each earlier one
// is warned as discarded (cl: 32 then 16 gives 16, with its "used more than once"
// warning). The plain spelling keeps the LARGEST and says nothing.
//
// RED-ON-DISABLE: drop `repeated: last` from the shipped row and `a` reads 32 with
// no warning; make `foldAlignmentRequest` order by value and the same.
TEST(AttributeSecondSpelling, OfSeveralAlignRequestsTheLastOneStandsAndTheRestAreSaid) {
    for (char const* src : {"__declspec(align(32)) __declspec(align(16)) int a;\n",
                            "__declspec(align(32) align(16)) int a;\n",
                            "__declspec(align(32)) int __declspec(align(16)) a;\n"}) {
        auto m = analyzePe({src});
        EXPECT_FALSE(m.hasErrors()) << src << everyDiagnostic(m);
        EXPECT_EQ(objectAlignment(m, "a"), std::optional<std::uint32_t>{16u}) << src;
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeIgnoredForDeclarationKind,
                              "written more than once"),
                  1u)
            << src << everyDiagnostic(m);
        EXPECT_EQ(m.diagnostics().all().size(), 1u) << src << everyDiagnostic(m);
    }
    {
        auto m = analyzePe({"__declspec(align(16)) __declspec(align(32)) int a;\n"});
        EXPECT_EQ(objectAlignment(m, "a"), std::optional<std::uint32_t>{32u});
        EXPECT_EQ(m.diagnostics().all().size(), 1u)
            << "the earlier request is discarded and said so even when it is the "
               "smaller one:" << everyDiagnostic(m);
    }
    // CONTROL: the plain spelling keeps the largest, in either order, silently.
    for (char const* src :
         {"__attribute__((aligned(32))) __attribute__((aligned(16))) int a;\n",
          "__attribute__((aligned(16))) __attribute__((aligned(32))) int a;\n"}) {
        auto m = analyzePe({src});
        EXPECT_EQ(objectAlignment(m, "a"), std::optional<std::uint32_t>{32u}) << src;
        EXPECT_TRUE(m.diagnostics().all().empty()) << src << everyDiagnostic(m);
    }
}

// WRITTEN BEFORE A DEFINITION OF A STRUCTURE OR A UNION, the request aligns the
// TYPE being defined — every object of it, the ones declared later included (cl).
// A tag that is only REFERRED TO, one defined deeper inside the body, and an
// enumeration are not the declaration's own structure or union definition: there
// the request is the declared object's alone.
//
// RED-ON-DISABLE: make the shipped row's `leadingDecoratesDefinitionOf` name the
// enumeration shape instead of the structure and union shapes and every "the type"
// row reads {4, 4} while the enumeration reads 32; stop the composite scan from
// asking for the leading requests (`leadingDefinitionAlignmentRequests`) and the
// same rows read {4, 4}; stop it from setting the after-body specifiers aside
// (`appendDeclarationSpecifiersAfterBody`) and `struct A` reads {32, 32}.
TEST(AttributeSecondSpelling, ALeadingAlignOnAStructureOrUnionDefinitionAlignsTheType) {
    auto m = analyzePe({
        "__declspec(align(32)) struct S { int a; } s;\n"
        "struct S later;\n"
        "__declspec(align(32)) struct T { int a; };\n"
        "struct T t;\n"
        "__declspec(align(32)) union U { int a; char c; } u;\n"
        "__declspec(align(32)) struct O { struct I { int a; } i; int b; } o;\n"
        "struct I inner;\n"
        "typedef __declspec(align(32)) struct F { int a; } F_t;\n"
        "struct F f;\n"
        "F_t ft;\n"
        "__declspec(align(32)) typedef struct E { int a; } E_t;\n"
        "struct E e;\n"
        "E_t et;\n"
        "__declspec(align(32)) struct P { int a; } p1, p2;\n"});
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    EXPECT_TRUE(m.diagnostics().all().empty()) << everyDiagnostic(m);
    EXPECT_EQ(layoutOf(m, "s"), (SizeAlign{32, 32}));
    EXPECT_EQ(layoutOf(m, "later"), (SizeAlign{32, 32}))
        << "the TYPE was aligned, so a later object of it is too";
    EXPECT_EQ(layoutOf(m, "t"), (SizeAlign{32, 32}))
        << "a declaration that declares no object still aligns the type it defines";
    EXPECT_EQ(layoutOf(m, "u"), (SizeAlign{32, 32}));
    EXPECT_EQ(layoutOf(m, "o"), (SizeAlign{32, 32}));
    EXPECT_EQ(layoutOf(m, "inner"), (SizeAlign{4, 4}))
        << "a structure defined INSIDE the body is not the declaration's own";
    EXPECT_EQ(layoutOf(m, "f"), (SizeAlign{32, 32}));
    EXPECT_EQ(layoutOf(m, "ft"), (SizeAlign{32, 32}));
    EXPECT_EQ(layoutOf(m, "e"), (SizeAlign{32, 32}));
    EXPECT_EQ(layoutOf(m, "et"), (SizeAlign{32, 32}));
    EXPECT_EQ(layoutOf(m, "p1"), (SizeAlign{32, 32}));
    EXPECT_EQ(layoutOf(m, "p2"), (SizeAlign{32, 32}));

    auto n = analyzePe({
        "struct R { int a; };\n"
        "__declspec(align(32)) struct R r;\n"
        "struct R plain;\n"
        "__declspec(align(32)) enum En { en0 } en;\n"
        "enum En en2;\n"
        "struct A { int a; } __declspec(align(32)) ab;\n"
        "struct A a2;\n"});
    EXPECT_FALSE(n.hasErrors()) << everyDiagnostic(n);
    EXPECT_TRUE(n.diagnostics().all().empty()) << everyDiagnostic(n);
    EXPECT_EQ(layoutOf(n, "r"), (SizeAlign{4, 4})) << "struct R is only referred to";
    EXPECT_EQ(objectAlignment(n, "r"), std::optional<std::uint32_t>{32u});
    EXPECT_FALSE(objectAlignment(n, "plain").has_value());
    EXPECT_EQ(layoutOf(n, "en"), (SizeAlign{4, 4})) << "an enumeration is not aligned";
    EXPECT_EQ(objectAlignment(n, "en"), std::optional<std::uint32_t>{32u});
    EXPECT_FALSE(objectAlignment(n, "en2").has_value());
    EXPECT_EQ(layoutOf(n, "ab"), (SizeAlign{4, 4}))
        << "after the BODY the request is the declared object's, not the type's";
    EXPECT_EQ(objectAlignment(n, "ab"), std::optional<std::uint32_t>{32u});
    EXPECT_EQ(layoutOf(n, "a2"), (SizeAlign{4, 4}));
    EXPECT_FALSE(objectAlignment(n, "a2").has_value());

    // CONTROL: the plain spelling says the opposite at both places, and keeps
    // saying it — a leading run is the declared object's, a run after the body is
    // the type's.
    auto g = analyzePe({
        "__attribute__((aligned(32))) struct GS { int a; } gs;\n"
        "struct GS gs2;\n"
        "struct GA { int a; } __attribute__((aligned(32))) ga;\n"
        "struct GA ga2;\n"});
    EXPECT_FALSE(g.hasErrors()) << everyDiagnostic(g);
    EXPECT_EQ(layoutOf(g, "gs2"), (SizeAlign{4, 4}));
    EXPECT_EQ(objectAlignment(g, "gs"), std::optional<std::uint32_t>{32u});
    EXPECT_EQ(layoutOf(g, "ga2"), (SizeAlign{32, 32}));
}

// ON A MEMBER the request moves the member and raises the structure (cl: `struct M
// { char c; __declspec(align(32)) int m; }` puts `m` at 32 and is 64 bytes); beside
// `_Alignas` the larger of the two stands, in either order.
TEST(AttributeSecondSpelling, AlignOnAMemberAndBesideAlignas) {
    auto m = analyzePe({"struct M { char c; __declspec(align(32)) int m; };\n"
                        "struct M mo;\n"
                        "_Alignas(16) __declspec(align(32)) int x;\n"
                        "_Alignas(32) __declspec(align(16)) int y;\n"
                        "__declspec(align(16)) _Alignas(32) int z;\n"});
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    EXPECT_TRUE(m.diagnostics().all().empty()) << everyDiagnostic(m);
    EXPECT_EQ(layoutOf(m, "mo"), (SizeAlign{64, 32}));
    EXPECT_EQ(objectAlignment(m, "x"), std::optional<std::uint32_t>{32u});
    EXPECT_EQ(objectAlignment(m, "y"), std::optional<std::uint32_t>{32u});
    EXPECT_EQ(objectAlignment(m, "z"), std::optional<std::uint32_t>{32u});
}

// WHAT IS NOT HONOURED IS SAID. The form with no operand asks for nothing under
// this spelling (cl refuses it; MinGW reads GNU's bare `aligned`, the target's
// largest useful alignment — which is the plain spelling's meaning and stays
// there); on a function the request is ignored (cl refuses, MinGW ignores).
//
// RED-ON-DISABLE: drop `withoutOperand: ignored` from the shipped row and `bare`
// is aligned to 16 in silence; add `function` to the row's `appliesTo` and the
// function row says nothing.
TEST(AttributeSecondSpelling, AnAlignRequestThatAsksNothingOrSitsOnAFunctionIsSaid) {
    {
        auto m = analyzePe({"__declspec(align) int bare;\n"});
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_FALSE(objectAlignment(m, "bare").has_value());
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeIgnoredForDeclarationKind,
                              "names no alignment"),
                  1u)
            << everyDiagnostic(m);
        EXPECT_EQ(m.diagnostics().all().size(), 1u) << everyDiagnostic(m);
    }
    {
        auto m = analyzePe({"__declspec(align(32)) void f(void);\n"});
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_AttributeIgnoredForDeclarationKind),
                  1u)
            << everyDiagnostic(m);
        EXPECT_EQ(m.diagnostics().all().size(), 1u) << everyDiagnostic(m);
    }
    // CONTROL: the plain spelling's bare form is the target's largest useful
    // alignment, silently.
    auto g = analyzePe({"__attribute__((aligned)) int bare;\n"});
    EXPECT_FALSE(g.hasErrors()) << everyDiagnostic(g);
    EXPECT_EQ(objectAlignment(g, "bare"), std::optional<std::uint32_t>{16u});
    EXPECT_TRUE(g.diagnostics().all().empty()) << everyDiagnostic(g);
}

// ── `thread` ───────────────────────────────────────────────────────────────

// THE REQUEST GIVES THE OBJECT THREAD STORAGE at every place cl takes it: before
// the type, after it, beside `static` and `extern`, on a block-scope `static`,
// after a structure's body, and on every declarator of the declaration.
//
// RED-ON-DISABLE: drop `threadStorage` from the qualified key of any one of the
// five shipped rows and the objects that row declares read as shared; drop the
// record-time scan (`declaratorAttributeStorage`) and all of them do.
TEST(AttributeSecondSpelling, ThreadGivesTheObjectThreadStorageWhereverItIsWritten) {
    auto m = analyzePe({"__declspec(thread) int t1;\n"
                        "static __declspec(thread) int t2;\n"
                        "extern __declspec(thread) int t3;\n"
                        "int __declspec(thread) t4;\n"
                        "__declspec(thread) int t5a, t5b;\n"
                        "struct TB { int a; } __declspec(thread) t6;\n"
                        "__declspec(thread) _Thread_local int t7;\n"
                        "int shared;\n"
                        "int f(void) { static __declspec(thread) int t8; return t8; }\n"});
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    EXPECT_TRUE(m.diagnostics().all().empty()) << everyDiagnostic(m);
    for (char const* name : {"t1", "t2", "t3", "t4", "t5a", "t5b", "t6", "t7", "t8"}) {
        ASSERT_NE(findSym(m, name), nullptr) << name;
        EXPECT_TRUE(findSym(m, name)->isThreadLocal) << name;
    }
    // CONTROL: an object that says nothing is shared.
    ASSERT_NE(findSym(m, "shared"), nullptr);
    EXPECT_FALSE(findSym(m, "shared")->isThreadLocal);
}

// ON A FUNCTION OR A TYPE ALIAS the request is ignored and said so (cl refuses
// both; MinGW ignores both with a warning); on a block-scope object with automatic
// storage it is refused, as every thread-storage request there is (C 6.7.1p3; cl
// refuses it too).
TEST(AttributeSecondSpelling, ThreadOnAFunctionOrAnAliasIsIgnoredAndOnAnAutomaticRefused) {
    for (char const* src : {"__declspec(thread) void f(void);\n",
                            "__declspec(thread) typedef int tt;\n"}) {
        auto m = analyzePe({src});
        EXPECT_FALSE(m.hasErrors()) << src << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_AttributeIgnoredForDeclarationKind),
                  1u)
            << src << everyDiagnostic(m);
        EXPECT_EQ(m.diagnostics().all().size(), 1u) << src << everyDiagnostic(m);
    }
    auto m = analyzePe({"void g(void) { __declspec(thread) int a; a = 1; }\n"});
    EXPECT_TRUE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern),
              1u)
        << everyDiagnostic(m);
}

// TWO DECLARATIONS OF ONE OBJECT THAT DISAGREE. cl and MinGW both compile `extern
// __declspec(thread) int e; int e = 1;` and both run it with ONE object shared by
// every thread, so the request YIELDS, in either order, and is warned; every
// record of the name then says "not thread storage", because a later tier may read
// any of them. The keyword forms never yield: there the disagreement stays the
// constraint violation it is, also when the attribute stands beside the keyword.
//
// RED-ON-DISABLE: drop `yieldsOnMismatch` from the shipped key and both orders are
// refused; clear only one of the two records in `mergeOrCollideRedeclaration` and
// the "every record" loop finds a thread-storage one.
TEST(AttributeSecondSpelling, AThreadRequestYieldsToADeclarationThatDoesNotMakeIt) {
    for (char const* src : {"extern __declspec(thread) int e;\nint e = 1;\n",
                            "int e = 1;\nextern __declspec(thread) int e;\n",
                            "extern int e;\n__declspec(thread) int e = 1;\n"}) {
        auto m = analyzePe({src});
        EXPECT_FALSE(m.hasErrors()) << src << everyDiagnostic(m);
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeIgnoredForDeclarationKind,
                              "thread-storage request"),
                  1u)
            << src << everyDiagnostic(m);
        EXPECT_EQ(m.diagnostics().all().size(), 1u) << src << everyDiagnostic(m);
        auto const records = symsNamed(m, "e");
        ASSERT_EQ(records.size(), 2u) << src;
        for (SymbolRecord const* r : records) EXPECT_FALSE(r->isThreadLocal) << src;
    }
    // CONTROL: two declarations that AGREE keep thread storage and say nothing.
    {
        auto m = analyzePe(
            {"extern __declspec(thread) int e;\n__declspec(thread) int e = 1;\n"});
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_TRUE(m.diagnostics().all().empty()) << everyDiagnostic(m);
        for (SymbolRecord const* r : symsNamed(m, "e")) EXPECT_TRUE(r->isThreadLocal);
    }
    // CONTROL: the keyword does not yield, alone or beside the attribute.
    for (char const* src :
         {"extern _Thread_local int k;\nint k = 1;\n",
          "extern __declspec(thread) _Thread_local int k;\nint k = 1;\n"}) {
        auto m = analyzePe({src});
        EXPECT_TRUE(m.hasErrors()) << src;
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_ThreadLocalRedeclarationMismatch),
                  1u)
            << src << everyDiagnostic(m);
    }
}

// THE REQUEST YIELDS BECAUSE ITS ROW SAYS SO — not because it is written as an
// attribute. The same language with that one answer taken off the qualified key
// makes the disagreeing pair the constraint violation the keyword forms make it, in
// either order, with no word about an ignored request; and two declarations that
// agree are still thread storage. This is the engine's other path, which no row of
// the shipped language reaches: without this pin it would be code no test ran.
//
// RED-ON-DISABLE: copy "yields" from the fact that the request came from an
// attribute instead of from the key (`scanAttributeStorage`) and both orders
// analyze clean with a warning.
TEST(AttributeSecondSpelling, WithoutTheRowsAnswerADisagreeingThreadRequestIsRefused) {
    auto const firm = shippedCWith(
        R"J("__declspec(thread)": { "threadStorage": true, "yieldsOnMismatch": true })J",
        R"J("__declspec(thread)": { "threadStorage": true })J", 5);
    ASSERT_NE(firm, nullptr);
    for (char const* src : {"extern __declspec(thread) int e;\nint e = 1;\n",
                            "int e = 1;\nextern __declspec(thread) int e;\n"}) {
        auto m = analyzePeUnder(firm, src);
        EXPECT_TRUE(m.hasErrors()) << src;
        EXPECT_EQ(countCode(m.diagnostics(),
                            DiagnosticCode::S_ThreadLocalRedeclarationMismatch),
                  1u)
            << src << everyDiagnostic(m);
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeIgnoredForDeclarationKind,
                              "thread-storage request"),
                  0u)
            << src << everyDiagnostic(m);
    }
    // CONTROL: the edited language still gives the object thread storage.
    auto m = analyzePeUnder(
        firm, "extern __declspec(thread) int e;\n__declspec(thread) int e = 1;\n");
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    auto const records = symsNamed(m, "e");
    ASSERT_EQ(records.size(), 2u);
    for (SymbolRecord const* r : records) EXPECT_TRUE(r->isThreadLocal);
}

// ── THE NAMES THAT MEAN WHAT THEIR PLAIN SPELLING MEANS, AND THE INERT ONES ──

TEST(AttributeSecondSpelling, ThePlainNamesKeepTheirMeaningInsideTheFrame) {
    auto m = analyzePe({"__declspec(noreturn) void die(void);\n"
                        "void __declspec(noreturn) die2(void);\n"
                        "__declspec(deprecated) int old1;\n"
                        "__declspec(deprecated(\"use new1\")) int old2;\n"
                        "__declspec(selectany) int pick = 1;\n"
                        "int use(void) { return old1 + old2; }\n"});
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    ASSERT_NE(findSym(m, "die"), nullptr);
    EXPECT_TRUE(findSym(m, "die")->isNoreturn);
    ASSERT_NE(findSym(m, "die2"), nullptr);
    EXPECT_TRUE(findSym(m, "die2")->isNoreturn);
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_DeprecatedSymbolUsed), 2u)
        << everyDiagnostic(m);
    EXPECT_EQ(countSaying(m, DiagnosticCode::S_DeprecatedSymbolUsed, "use new1"), 1u)
        << "the message of the request reaches the warning at the use";
    EXPECT_EQ(m.diagnostics().all().size(), 2u) << everyDiagnostic(m);
}

// `dllimport` / `dllexport` on a datum or a function, and `restrict` / `noalias`
// on a function, are accepted and change nothing here — each reference compiles
// and runs them in an executable (a `dllimport` DEFINITION excepted: the next case). On a type alias the first pair is ignored and
// said so (MinGW warns; cl says nothing).
//
// RED-ON-DISABLE: drop the import/export row and the alias cell loses its warning
// (the kind axis is the row's). An unknown word in a declaration's prefix is not
// this tier's to report, so the silence of the first block is pinned where the
// word WOULD be reported — `HirLoweringC.AModelledModifierOfTheSecondSpellingIs
// NotAnUnknownLinkageSpecifier`, which carries each of these modifiers.
TEST(AttributeSecondSpelling, TheImportExportAndAliasingModifiersAreAcceptedAndInert) {
    auto m = analyzePe({"__declspec(dllexport) int de = 1;\n"
                        "__declspec(dllexport) int fe(void) { return de; }\n"
                        "__declspec(dllimport) extern int di;\n"
                        "__declspec(dllimport) int fi(void);\n"
                        "__declspec(restrict) void* ra(void);\n"
                        "__declspec(noalias) int na(int);\n"});
    EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
    EXPECT_TRUE(m.diagnostics().all().empty()) << everyDiagnostic(m);

    auto t = analyzePe({"__declspec(dllexport) typedef int dt;\n"});
    EXPECT_FALSE(t.hasErrors()) << everyDiagnostic(t);
    EXPECT_EQ(countCode(t.diagnostics(),
                        DiagnosticCode::S_AttributeIgnoredForDeclarationKind),
              1u)
        << everyDiagnostic(t);
    EXPECT_EQ(t.diagnostics().all().size(), 1u) << everyDiagnostic(t);
}

// `dllimport` ON A DEFINITION — a function with its body, an object with its
// initializer — is refused by name (the row's `refusedOnDefinition`), and on a mere
// declaration of either it is accepted as before. ✔MEASURED (lane `cs` probe di,
// 2026-10-20, windows-x86_64): cl 19.51 refuses both definitions (C2491); mingw-w64
// gcc 13.2.0 refuses the object and compiles the function into a program that does
// not link (`__imp_f` undefined). Before fold f26b both were accepted in silence (the
// re-review of fold A2, finding 2). `dllexport` on a definition is its ordinary use.
//
// RED-ON-DISABLE: drop the row's `refusedOnDefinition`, or the gate that reads it,
// and both definitions compile in silence.
TEST(AttributeSecondSpelling, DllimportOnADefinitionIsRefusedByName) {
    struct Case {
        char const* what;
        char const* src;
        bool        refused;
    };
    for (Case const c : {
             Case{"an object definition", "__declspec(dllimport) int x = 7;\n", true},
             Case{"a function definition", "__declspec(dllimport) int f(void) { return 7; }\n", true},
             Case{"after the type", "int __declspec(dllimport) y = 1;\n", true},
             Case{"CONTROL: an object declaration", "__declspec(dllimport) extern int x;\n", false},
             Case{"CONTROL: a function declaration", "__declspec(dllimport) int f(void);\n", false},
             Case{"CONTROL: dllexport on a definition",
                  "__declspec(dllexport) int f(void) { return 7; }\n", false},
         }) {
        SCOPED_TRACE(c.what);
        auto m = analyzePe({c.src});
        if (c.refused) {
            EXPECT_TRUE(m.hasErrors()) << c.src << everyDiagnostic(m);
            EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeNotHonoured,
                                  "cannot be honoured: this declaration defines"),
                      1u)
                << c.src << everyDiagnostic(m);
            EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeNotHonoured,
                                  "the definition is another image's"),
                      1u)
                << c.src << everyDiagnostic(m);
        } else {
            EXPECT_FALSE(m.hasErrors()) << c.src << everyDiagnostic(m);
            EXPECT_TRUE(m.diagnostics().all().empty()) << c.src << everyDiagnostic(m);
        }
    }
}

// `selectany` ON AN ALIAS is ignored and said (the row's `ignoredOnAlias`): an alias
// is one more name on its target's storage, with no copy of its own for the link to
// select, and it stays the name its own binding makes it. ✔MEASURED (lane `cs` probe
// sw, 2026-10-20, windows-x86_64): mingw-w64 gcc 13.2.0 builds every alias cell below
// and runs it (exit 42), writing the alias as a plain EXTERNAL label — the select-any
// dropped in silence; clang 18.1.3 for x86_64-pc-windows-msvc refuses them. Before
// fold f26b the alias kept the select-any kind, which the pe64 object writer then
// refused beside a `static` body and wrote as an OVERRIDABLE weak external beside an
// external one.
//
// RED-ON-DISABLE: drop the row's `ignoredOnAlias`, or the gate that reads it, and
// every alias cell analyzes in silence.
TEST(AttributeSecondSpelling, SelectanyOnAnAliasIsIgnoredAndSaid) {
    struct Case {
        char const* what;
        char const* src;
        bool        said;
    };
    for (Case const c : {
             Case{"an alias of a static object",
                  "static int s = 40;\n"
                  "__declspec(selectany) extern int a __attribute__((alias(\"s\")));\n",
                  true},
             Case{"an alias of an external object",
                  "int s = 40;\n"
                  "__declspec(selectany) extern int a __attribute__((alias(\"s\")));\n",
                  true},
             Case{"the specifier after the type",
                  "static int s = 40;\n"
                  "extern int __declspec(selectany) a __attribute__((alias(\"s\")));\n",
                  true},
             Case{"CONTROL: a select-any definition", "__declspec(selectany) int w = 7;\n", false},
             Case{"CONTROL: a weak alias",
                  "static int s = 40;\n"
                  "extern int a __attribute__((weak, alias(\"s\")));\n",
                  false},
         }) {
        SCOPED_TRACE(c.what);
        auto m = analyzePe({c.src});
        EXPECT_FALSE(m.hasErrors()) << c.src << everyDiagnostic(m);
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeIgnoredForDeclarationKind,
                              "'a' is an alias, and an alias is one more name on its target's storage"),
                  c.said ? 1u : 0u)
            << c.src << everyDiagnostic(m);
        EXPECT_EQ(m.diagnostics().all().size(), c.said ? 1u : 0u) << c.src << everyDiagnostic(m);
    }
}

// A REQUEST THIS COMPILER CANNOT HONOUR IS REFUSED BY NAME, with its reason: a
// function with no prologue and no epilogue (`naked`, under either spelling — its
// body is the whole of its code, and compiling it as an ordinary function would
// run a prologue the body never asked for), and an object placed in a named
// section (`allocate`).
//
// RED-ON-DISABLE: drop either `unsupported` row and its name is an unknown
// attribute, warned, on an analysis that succeeds.
TEST(AttributeSecondSpelling, ARequestThatCannotBeHonouredIsRefusedByName) {
    struct Case {
        char const* src;
        char const* name;
    };
    for (Case const c :
         {Case{"__declspec(naked) void nk(void) { }\n", "'naked'"},
          Case{"__attribute__((naked)) void nk(void) { }\n", "'naked'"},
          Case{"__declspec(allocate(\"seg\")) int al = 1;\n", "'allocate'"}}) {
        auto m = analyzePe({c.src});
        EXPECT_TRUE(m.hasErrors()) << c.src;
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeNotHonoured, c.name), 1u)
            << c.src << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_UnknownAttribute), 0u)
            << c.src;
    }
}

// ── `dllimport` ON A DECLARATION THAT DEFINES NOTHING (P69 lane `cs`, fold f26b) ─────

// AN IMPORT NAMES A DEFINITION ANOTHER IMAGE HOLDS: a declaration with no initializer that
// carries `dllimport` — at file scope or inside a body, with or without `extern`, before or
// after the type, beside the name inside a pointer declarator, or after the declarator — is
// a DECLARATION of that name: never a tentative definition of this unit, never an automatic
// local. PER DECLARATOR: `int a, * __declspec(dllimport) p;` defines `a` and imports `p`.
// ✔MEASURED (lane `cs` probes di, di2, di3, di4, di5, 2026-10-20/21, windows-x86_64): cl
// 19.51 and mingw-w64 gcc 13.2.0 write `__imp_x` and no `x` for each declaration below
// that they accept (cl has no attribute inside or after a declarator and refuses those;
// mingw-w64 gcc takes them). Before fold f26b this compiler made the file-scope ones a
// common of their own and the block ones locals read uninitialised — silently another
// program against a DLL that exports the name.
//
// RED-ON-DISABLE: drop the `imported` entries of the declaration rows, or the record-time
// scan's reading of them, and every cell is a tentative definition or a local again.
TEST(AttributeSecondSpelling, ADllimportDeclarationNamesAnotherImagesDefinition) {
    struct Case {
        char const* what;
        char const* src;
        char const* name;
    };
    for (Case const c : {
             Case{"a file-scope datum", "__declspec(dllimport) int x;\nint g(void) { return x; }\n",
                  "x"},
             Case{"after the type", "int __declspec(dllimport) x;\nint g(void) { return x; }\n", "x"},
             Case{"both declarators of a list",
                  "__declspec(dllimport) int a, x;\nint g(void) { return a + x; }\n", "x"},
             Case{"beside the name, inside a pointer declarator",
                  "int * __declspec(dllimport) x;\nint g(void) { return *x; }\n", "x"},
             Case{"after the declarator",
                  "int x __declspec(dllimport);\nint g(void) { return x; }\n", "x"},
             Case{"with `extern` as well",
                  "__declspec(dllimport) extern int x;\nint g(void) { return x; }\n", "x"},
             Case{"inside a body", "int g(void) { __declspec(dllimport) int x; return x; }\n", "x"},
             Case{"inside a body, with `extern`",
                  "int g(void) { __declspec(dllimport) extern int x; return x; }\n", "x"},
             Case{"inside a body, shadowing a local",
                  "int g(void) { int x = 1; { __declspec(dllimport) int x; return x; } }\n", "x"},
             Case{"the GNU spelling",
                  "__attribute__((dllimport)) int x;\nint g(void) { return x; }\n", "x"},
             Case{"the GNU spelling, written with underscores",
                  "__attribute__((__dllimport__)) int x;\nint g(void) { return x; }\n", "x"},
             Case{"the GNU spelling inside a body",
                  "int g(void) { __attribute__((dllimport)) int x; return x; }\n", "x"},
             Case{"the GNU spelling after the declarator",
                  "int x __attribute__((dllimport));\nint g(void) { return x; }\n", "x"},
             Case{"a function", "__declspec(dllimport) int f(void);\nint g(void) { return f(); }\n",
                  "f"},
         }) {
        SCOPED_TRACE(c.what);
        auto m = analyzePe({c.src});
        EXPECT_FALSE(m.hasErrors()) << c.src << everyDiagnostic(m);
        for (auto const& d : m.diagnostics().all())
            EXPECT_EQ(d.code, DiagnosticCode::S_UnusedVariable)
                << c.src << ": only the shadowed local may be warned (unused)"
                << everyDiagnostic(m);
        bool sawImport = false;
        for (SymbolRecord const* r : symsNamed(m, c.name)) {
            if (!r->isImportedDeclaration) continue;   // the shadowed local of one cell
            sawImport = true;
            EXPECT_FALSE(r->isTentativeDefinition)
                << c.src << ": an import is no tentative definition";
            EXPECT_TRUE(r->isExternDeclaration || r->isProtoDeclaration)
                << c.src << ": an import declares a name another image defines";
        }
        EXPECT_TRUE(sawImport) << c.src << everyDiagnostic(m);
    }
    // PER DECLARATOR: the import beside one name does not reach the other.
    auto mixed = analyzePe({"int a, * __declspec(dllimport) p;\nint g(void) { return a + *p; }\n"});
    EXPECT_FALSE(mixed.hasErrors()) << everyDiagnostic(mixed);
    ASSERT_NE(findSym(mixed, "a"), nullptr);
    ASSERT_NE(findSym(mixed, "p"), nullptr);
    EXPECT_FALSE(findSym(mixed, "a")->isImportedDeclaration);
    EXPECT_TRUE(findSym(mixed, "a")->isTentativeDefinition) << "`a` stays this unit's definition";
    EXPECT_TRUE(findSym(mixed, "p")->isImportedDeclaration);
    EXPECT_FALSE(findSym(mixed, "p")->isTentativeDefinition);
    auto trailing = analyzePe({"int a __declspec(dllimport), b;\nint g(void) { return a + b; }\n"});
    EXPECT_FALSE(trailing.hasErrors()) << everyDiagnostic(trailing);
    ASSERT_NE(findSym(trailing, "a"), nullptr);
    ASSERT_NE(findSym(trailing, "b"), nullptr);
    EXPECT_TRUE(findSym(trailing, "a")->isImportedDeclaration);
    EXPECT_FALSE(findSym(trailing, "b")->isImportedDeclaration);
    EXPECT_TRUE(findSym(trailing, "b")->isTentativeDefinition);
    // CONTROLS: the same declaration without the modifier, and with `dllexport`, is this
    // unit's tentative definition.
    for (char const* src : {"int x;\nint g(void) { return x; }\n",
                            "__declspec(dllexport) int x;\n"}) {
        auto m = analyzePe({src});
        EXPECT_FALSE(m.hasErrors()) << src << everyDiagnostic(m);
        SymbolRecord const* r = findSym(m, "x");
        ASSERT_NE(r, nullptr) << src;
        EXPECT_FALSE(r->isImportedDeclaration) << src;
        EXPECT_TRUE(r->isTentativeDefinition) << src;
    }
}

// THE GNU SPELLING IS AN IMPORT ON pe ONLY. Off pe the word is no attribute: warned as
// unknown, ignored, and the declaration is the tentative definition it is without it
// (`__has_attribute` answers 0 there — pinned with the preprocessor's feature queries).
// ✔MEASURED (lane `cs` probe di5): gcc 13.3.0 ("attribute directive ignored"), clang 18.1.3
// and Apple clang ("unknown attribute 'dllimport' ignored") at every position, the program
// built as if the word were not written; mingw-w64 gcc 13.2.0 imports it.
//
// RED-ON-DISABLE: drop the row's `availableObjectFormats` and the ELF, Mach-O and
// no-format cells import; drop `dllimport` from the row's names and the pe cell defines.
TEST(AttributeSecondSpelling, TheGnuSpellingOfDllimportIsAnImportOnPeAndAnUnknownWordElsewhere) {
    char const* const src = "__attribute__((dllimport)) int x;\nint g(void) { return x; }\n";
    auto pe = analyzeOn(ObjectFormatKind::Pe, {src});
    EXPECT_FALSE(pe.hasErrors()) << everyDiagnostic(pe);
    EXPECT_TRUE(pe.diagnostics().all().empty()) << everyDiagnostic(pe);
    ASSERT_NE(findSym(pe, "x"), nullptr);
    EXPECT_TRUE(findSym(pe, "x")->isImportedDeclaration);
    for (std::optional<ObjectFormatKind> const format :
         {std::optional<ObjectFormatKind>{ObjectFormatKind::Elf},
          std::optional<ObjectFormatKind>{ObjectFormatKind::MachO},
          std::optional<ObjectFormatKind>{}}) {
        SCOPED_TRACE(format.has_value() ? std::string{objectFormatKindName(*format)}
                                        : std::string{"no format"});
        auto m = analyzeOn(format, {src});
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_UnknownAttribute,
                              "attribute 'dllimport' is ignored: it is an attribute of the "
                              "object format(s) 'pe' only"),
                  1u)
            << everyDiagnostic(m);
        EXPECT_EQ(m.diagnostics().all().size(), 1u) << everyDiagnostic(m);
        ASSERT_NE(findSym(m, "x"), nullptr);
        EXPECT_FALSE(findSym(m, "x")->isImportedDeclaration);
        EXPECT_TRUE(findSym(m, "x")->isTentativeDefinition);
    }
}

// AN IMPORT THAT CANNOT NAME ANOTHER IMAGE'S DEFINITION IS REFUSED BY NAME: one with no
// external linkage (`static`, at file scope or inside a body, an object or a function), one
// with thread storage, and one after a declaration of this unit that DEFINES the name — a
// definition, a tentative definition, a function's body, at file scope, and a block-scope
// import of a name the file scope already defines. ✔MEASURED (lane `cs` probes di, di2,
// di3, di4, di5, di6; cl 19.51 and mingw-w64 gcc 13.2.0): cl refuses each (C2201, C2492,
// C2370, C2375); mingw-w64 gcc refuses the `static` ones ("external linkage required") and
// compiles the redeclared ones into programs that do not link (`__imp_x` undefined). Of
// thread storage cl says C2492 for both spellings; mingw-w64 gcc drops `__declspec(thread)`
// (a plain import) and imports `_Thread_local`'s emulated-TLS control object
// (`__imp___emutls_v.x`) — its own thread-storage model, not the native one this compiler
// keeps on pe with cl — so cl's answer is taken, one rule for every spelling of the
// storage. CONTROLS: an import after a plain `extern` or prototype holds (mingw-w64 gcc
// imports; cl refuses — one working reference); a block import before a LATER file-scope
// definition runs on both (the definition's value); a block-scope plain `extern` of a
// file-scope import compiles on both (cl keeps the import, mingw-w64 gcc warns and drops
// it; this compiler writes the plain reference either way, which the link routes).
//
// RED-ON-DISABLE: drop the record-time refusal and the storage cells compile; drop the
// merge's refusal and the file-scope redeclaration cells do; drop the block-scope check and
// the two block cells do.
TEST(AttributeSecondSpelling, AnImportIsRefusedWhereNoOtherImageCanHoldTheDefinition) {
    struct Case {
        char const* what;
        char const* src;
        char const* says;   // empty: a CONTROL, accepted in silence
    };
    for (Case const c : {
             Case{"`static` at file scope", "__declspec(dllimport) static int x;\n",
                  "only a name with external linkage"},
             Case{"`static` inside a body",
                  "int g(void) { static __declspec(dllimport) int x; return x; }\n",
                  "only a name with external linkage"},
             Case{"a `static` function", "static __declspec(dllimport) int f(void);\n",
                  "only a name with external linkage"},
             Case{"the GNU spelling, `static`", "__attribute__((dllimport)) static int x;\n",
                  "only a name with external linkage"},
             Case{"thread storage", "__declspec(dllimport) __declspec(thread) int x;\n",
                  "thread storage"},
             Case{"thread storage by the keyword", "__declspec(dllimport) _Thread_local int x;\n",
                  "thread storage"},
             Case{"after a definition", "int x = 7;\n__declspec(dllimport) int x;\n",
                  "a prior declaration of this unit defines 'x'"},
             Case{"after a tentative definition", "int x;\n__declspec(dllimport) int x;\n",
                  "a prior declaration of this unit defines 'x'"},
             Case{"after a function's body",
                  "int f(void) { return 7; }\n__declspec(dllimport) int f(void);\n",
                  "a prior declaration of this unit defines 'f'"},
             Case{"a block import of a defined name",
                  "int x = 5;\nint main(void) { __declspec(dllimport) int x; return x; }\n",
                  "a prior declaration of this unit defines 'x'"},
             Case{"a block import of a tentatively defined name",
                  "int x;\nint main(void) { __declspec(dllimport) int x; return x + 6; }\n",
                  "a prior declaration of this unit defines 'x'"},
             Case{"CONTROL: `extern`, then an import",
                  "extern int x;\n__declspec(dllimport) int x;\n", ""},
             Case{"CONTROL: a prototype, then an imported one",
                  "int f(void);\n__declspec(dllimport) int f(void);\n", ""},
             Case{"CONTROL: two imports",
                  "__declspec(dllimport) int x;\n__declspec(dllimport) int x;\n", ""},
             Case{"CONTROL: a file-scope `extern`, then a block import",
                  "extern int x;\nint g(void) { __declspec(dllimport) int x; return x; }\n", ""},
             Case{"CONTROL: a block import, then a later definition",
                  "int main(void) { __declspec(dllimport) int x; return x; }\nint x = 5;\n", ""},
             Case{"CONTROL: a file-scope import, then a block-scope plain `extern`",
                  "__declspec(dllimport) int x;\nint g(void) { extern int x; return x; }\n", ""},
         }) {
        SCOPED_TRACE(c.what);
        auto m = analyzePe({c.src});
        if (*c.says == '\0') {
            EXPECT_FALSE(m.hasErrors()) << c.src << everyDiagnostic(m);
            EXPECT_TRUE(m.diagnostics().all().empty()) << c.src << everyDiagnostic(m);
            continue;
        }
        EXPECT_TRUE(m.hasErrors()) << c.src << everyDiagnostic(m);
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeNotHonoured, c.says), 1u)
            << c.src << everyDiagnostic(m);
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeNotHonoured, "dllimport"), 1u)
            << c.src << everyDiagnostic(m);
        for (SymbolRecord const* r : symsNamed(m, "x"))
            EXPECT_FALSE(r->isImportedDeclaration) << c.src << ": a refused import imports nothing";
    }
    // The import after a plain non-defining declaration HOLDS, on both records.
    auto held = analyzePe({"extern int x;\n__declspec(dllimport) int x;\n"});
    ASSERT_EQ(symsNamed(held, "x").size(), 2u) << everyDiagnostic(held);
    for (SymbolRecord const* r : symsNamed(held, "x")) EXPECT_TRUE(r->isImportedDeclaration);
}

// A LATER DECLARATION WITHOUT THE IMPORT, AFTER ONE WITH IT, ENDS IT AND IS WARNED: a
// definition, a tentative definition, a function's body, a plain `extern`, a plain
// prototype. ✔MEASURED (lane `cs` probes di, di2, di3; cl 19.51 and mingw-w64 gcc 13.2.0):
// both warn — cl C4273 "inconsistent dll linkage", gcc "redeclared without dllimport
// attribute: previous dllimport ignored" — and the later declaration's meaning stands (a
// definition runs to its value; a plain declaration writes a plain reference). Before
// fold f26b this compiler said nothing.
//
// RED-ON-DISABLE: drop the merge's warning and every cell analyzes in silence.
TEST(AttributeSecondSpelling, ADeclarationWithoutTheImportAfterOneWithItEndsItAndIsWarned) {
    struct Case {
        char const* what;
        char const* src;
        char const* name;
        char const* says;
    };
    for (Case const c : {
             Case{"a datum's definition", "__declspec(dllimport) int x;\nint x = 7;\n", "x",
                  "this declaration defines 'x'"},
             Case{"a tentative definition", "__declspec(dllimport) int x;\nint x;\n", "x",
                  "this declaration defines 'x'"},
             Case{"a function's body",
                  "__declspec(dllimport) int f(void);\nint f(void) { return 7; }\n", "f",
                  "this declaration defines 'f'"},
             Case{"a plain `extern`", "__declspec(dllimport) int x;\nextern int x;\n", "x",
                  "does not import it"},
             Case{"a plain prototype", "__declspec(dllimport) int f(void);\nint f(void);\n", "f",
                  "does not import it"},
             Case{"the GNU spelling, then a definition",
                  "__attribute__((dllimport)) int x;\nint x = 5;\n", "x",
                  "this declaration defines 'x'"},
         }) {
        SCOPED_TRACE(c.what);
        auto m = analyzePe({c.src});
        EXPECT_FALSE(m.hasErrors()) << c.src << everyDiagnostic(m);
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeIgnoredForDeclarationKind,
                              "the import on a prior declaration of"),
                  1u)
            << c.src << everyDiagnostic(m);
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_AttributeIgnoredForDeclarationKind, c.says),
                  1u)
            << c.src << everyDiagnostic(m);
        EXPECT_EQ(m.diagnostics().all().size(), 1u) << c.src << everyDiagnostic(m);
        for (SymbolRecord const* r : symsNamed(m, c.name))
            EXPECT_FALSE(r->isImportedDeclaration) << c.src << ": the import is ended";
    }
}

// AN IMPORTED OBJECT'S ADDRESS IS NO ADDRESS CONSTANT — where the import holds AT THAT POINT
// of the unit. ✔MEASURED (lane `cs` probes di3, di4, di5, di6; cl 19.51 and mingw-w64 gcc
// 13.2.0): both refuse `&x` of an imported `x` in a file-scope initializer (C2099;
// "initializer element is not constant") and in a block `static`'s, of a file-scope or a
// block-scope import, and refuse it also when a LATER definition ends the import; both
// take an imported FUNCTION's address there (its thunk), with `&` or without, and an
// imported object's address taken at run time. CONTROLS: those, a plain `extern`'s
// address, and the address of an object whose import a definition ended before the
// initializer.
//
// RED-ON-DISABLE: drop the import arm of the static-initializer judge and the refused cells
// compile; make it read the end-of-unit flag instead of the history and the ordering cell
// compiles; drop its function test and the function cells are refused.
TEST(AttributeSecondSpelling, AnImportedObjectsAddressIsNoAddressConstant) {
    struct Case {
        char const* what;
        char const* src;
        bool        refused;
    };
    for (Case const c : {
             Case{"a file-scope initializer", "__declspec(dllimport) int x;\nint *p = &x;\n", true},
             Case{"a block static's",
                  "__declspec(dllimport) int x;\n"
                  "int *g(void) { static int *p = &x; return p; }\n",
                  true},
             Case{"a block static's, of a block import",
                  "int *g(void) { __declspec(dllimport) int x; static int *p = &x; return p; }\n",
                  true},
             Case{"the GNU spelling", "__attribute__((dllimport)) int x;\nint *p = &x;\n", true},
             Case{"a later definition ends the import after the use",
                  "__declspec(dllimport) int x;\nint *p = &x;\nint x = 5;\n", true},
             Case{"CONTROL: an imported function",
                  "__declspec(dllimport) int f(void);\nint (*p)(void) = f;\n", false},
             Case{"CONTROL: an imported function, with `&`",
                  "__declspec(dllimport) int f(void);\nint (*p)(void) = &f;\n", false},
             Case{"CONTROL: at run time",
                  "__declspec(dllimport) int x;\nint *g(void) { return &x; }\n", false},
             Case{"CONTROL: a plain `extern`", "extern int x;\nint *p = &x;\n", false},
         }) {
        SCOPED_TRACE(c.what);
        auto m = analyzePe({c.src});
        EXPECT_EQ(countSaying(m, DiagnosticCode::S_StaticInitializerNotConstant,
                              "which another image defines"),
                  c.refused ? 1u : 0u)
            << c.src << everyDiagnostic(m);
        if (c.refused) {
            EXPECT_TRUE(m.hasErrors()) << c.src << everyDiagnostic(m);
        } else {
            EXPECT_FALSE(m.hasErrors()) << c.src << everyDiagnostic(m);
            EXPECT_TRUE(m.diagnostics().all().empty()) << c.src << everyDiagnostic(m);
        }
    }
    // The import ENDED before the initializer: the address is a constant (the merge's
    // warning is the only diagnostic).
    auto ended = analyzePe({"__declspec(dllimport) int x;\nint x = 5;\nint *p = &x;\n"});
    EXPECT_FALSE(ended.hasErrors()) << everyDiagnostic(ended);
    EXPECT_EQ(countCode(ended.diagnostics(), DiagnosticCode::S_StaticInitializerNotConstant), 0u)
        << everyDiagnostic(ended);
}
