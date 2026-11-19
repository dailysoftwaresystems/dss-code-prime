// ════════════════════════════════════════════════════════════════════════════
//  THE RUN AFTER A DECLARATOR, AND THE SEQUENCE THAT ENDS A DECLARATION'S
//  SPECIFIERS — cycle P69, lane `cs`, fold f26e
//  [[D-C-A-C23-SEQUENCE-AFTER-A-WIDTH-OR-AFTER-A-GNU-RUN-IS-ACCEPTED-OTHERWISE-THAN-THE-REFERENCES]]
// ════════════════════════════════════════════════════════════════════════════
//
// The row named three positions; main ruled it a CLASS over every slot (2026-11-19) and
// the probes measured that universe. Each cell below is one probe's declaration, and its
// expected verdict is the union's, read off the measurement — never off this compiler:
//
//  * n5o — an init-declarator's C23 sequence (S), asm label (L) and GNU run (G), on an
//    object and a function declaration;
//  * n5t — the same run on every declarator kind and slot: a function, a function
//    returning a pointer, a parenthesized one, a pointer to a function, an array, a later
//    declarator, a definition, a typedef, a parameter (`[[vnd::p]]` is an UNKNOWN vendor
//    attribute, so acceptance reads the grammar and never the attribute's meaning);
//  * n5u — a C23 sequence at the END of a headed declaration's specifiers (C23 6.7p9) in
//    every slot, and a GNU type attribute in that spelling, whose meaning is read;
//  * n5r — the member positions (f26d took the first two; the width cell is decided here).
//
//  ✔MEASURED 2026-11-19: gcc 15.2.0 and clang 21.1.8 on the native Linux host, MinGW-w64
//  gcc 13.2.0, Apple clang, cl 19.52.37029 (probe runs in the row's cells).
//
//  ACCEPT  = at least one reference builds it (one working reference makes it required);
//  REFUSE  = every reference refuses it — a PARSE refusal where the grammar has no place
//            for it, a SEMANTIC one (named code) where the grammar parses a superset.
//  Decided, veto-able: a sequence AFTER A BIT-FIELD WIDTH stays refused (C23 has no place
//  there, every reference refuses `int x : 3 [[vnd::p]];`, and cl's one acceptance is of a
//  `[[deprecated]]` it does not recognise and drops — P66's own ground); a STANDARD
//  attribute at the end of the specifiers is accepted and said ignored, as gcc says it
//  (clang refuses it); `[[gnu::aligned(N)]]` there is gcc's reading (clang refuses it).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include "semantic_test_fixture.hpp"
#include "test_support/repo_root.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

constexpr AggregateLayoutParams kLayout{
    .scalarAlignment       = ScalarAlignmentRule::Natural,
    .maxAlignment          = 16,
    .maxRequestedAlignment = 268435456};

enum class Verdict : std::uint8_t { Accept, ParseRefused, Refused };

struct Cell {
    char const*                   probe;    // the probe cell it reproduces
    char const*                   source;   // one declaration (plus what uses it)
    Verdict                       verdict;
    std::optional<DiagnosticCode> code{};   // the refusal's code, for Refused
};

struct Outcome {
    Verdict     verdict;
    std::string detail;   // the first error, for the failure message
};

[[nodiscard]] Outcome judge(std::string const& source) {
    auto cu = buildShippedUnit("c", {source});
    for (auto const& t : cu->trees()) {
        for (auto const& d : t.diagnostics().all()) {
            if (d.severity == DiagnosticSeverity::Error) {
                return {Verdict::ParseRefused,
                        std::string{diagnosticCodeName(d.code)} + ": " + d.actual};
            }
        }
    }
    auto m = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kLayout);
    for (auto const& d : m.diagnostics().all()) {
        if (d.severity == DiagnosticSeverity::Error) {
            return {Verdict::Refused, std::string{diagnosticCodeName(d.code)} + ": " + d.actual};
        }
    }
    return {Verdict::Accept, {}};
}

[[nodiscard]] char const* verdictName(Verdict v) {
    switch (v) {
        case Verdict::Accept:       return "accepted";
        case Verdict::ParseRefused: return "refused by the parser";
        case Verdict::Refused:      return "refused by the semantic tier";
    }
    return "?";
}

void expectCells(std::span<Cell const> cells) {
    for (Cell const& c : cells) {
        std::string const src = std::string{c.source} + "\nint main(void) { return 0; }\n";
        Outcome const got = judge(src);
        EXPECT_EQ(got.verdict, c.verdict)
            << c.probe << " `" << c.source << "` is " << verdictName(got.verdict)
            << ", the references' union says " << verdictName(c.verdict)
            << (got.detail.empty() ? "" : " — ") << got.detail;
        if (c.verdict == Verdict::Refused && c.code.has_value()
            && got.verdict == Verdict::Refused) {
            auto cu = buildShippedUnit("c", {src});
            auto m = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kLayout);
            EXPECT_EQ(countCode(m.diagnostics(), *c.code), 1u)
                << c.probe << " `" << c.source << "` is refused, but not once by "
                << diagnosticCodeName(*c.code) << ": " << got.detail;
        }
    }
}

[[nodiscard]] SemanticModel analyzeC(std::string source) {
    auto cu = buildShippedUnit("c", {std::move(source)});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kLayout);
}

[[nodiscard]] SymbolRecord const* findSym(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return &m.symbols()[i];
    return nullptr;
}

[[nodiscard]] std::string asmNameOf(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name && !m.symbols()[i].asmName.empty())
            return m.symbols()[i].asmName;
    return {};
}

[[nodiscard]] std::uint32_t alignOf(SemanticModel const& m, char const* name) {
    SymbolRecord const* s = findSym(m, name);
    EXPECT_NE(s, nullptr) << "no symbol named '" << name << "'";
    if (s == nullptr) return 0;
    auto const l = computeLayout(s->type, m.lattice().interner(), kLayout, DataModel::Lp64);
    EXPECT_TRUE(l.has_value()) << "no layout for '" << name << "'";
    return l ? l->align.bytes() : 0;
}

[[nodiscard]] std::uint64_t sizeOf(SemanticModel const& m, char const* name) {
    SymbolRecord const* s = findSym(m, name);
    EXPECT_NE(s, nullptr) << "no symbol named '" << name << "'";
    if (s == nullptr) return 0;
    auto const l = computeLayout(s->type, m.lattice().interner(), kLayout, DataModel::Lp64);
    EXPECT_TRUE(l.has_value()) << "no layout for '" << name << "'";
    return l ? l->size : 0;
}

constexpr auto A  = Verdict::Accept;
constexpr auto PR = Verdict::ParseRefused;
constexpr auto R  = Verdict::Refused;
constexpr DiagnosticCode kMisplaced = DiagnosticCode::S_AsmLabelMisplaced;

} // namespace

// ── THE INIT-DECLARATOR'S RUN: S, then G, then labels each followed by G ───────
// n5o: every reference refuses S after G or after L, and G before L on an object;
// all accept L G, S L, S G, S L G. RED-ON-DISABLE: the any-order run (`{repeat {alt
// attrSpec stdAttr asmLabel}}`) accepts the three parse refusals; the reader's place
// check disabled accepts the object's G L.
TEST(DeclaratorRunOrder, AnObjectsRunIsTakenInTheReferencesOrder) {
    static constexpr Cell kCells[] = {
        {"n5o.p1", "int x __asm__(\"y\") __attribute__((unused)) = 1;", A},
        {"n5o.p2", "int x __attribute__((unused)) __asm__(\"y\") = 1;", R, kMisplaced},
        {"n5o.p3", "int x __asm__(\"y\") [[deprecated]] = 1;", PR},
        {"n5o.p4", "int x [[deprecated]] __asm__(\"y\") = 1;", A},
        {"n5o.p5", "int x [[deprecated]] __attribute__((unused)) = 1;", A},
        {"n5o.p6", "int x __attribute__((unused)) [[deprecated]] = 1;", PR},
        {"n5o.p7", "int x [[deprecated]] __asm__(\"y\") __attribute__((unused)) = 1;", A},
        {"n5o.p8", "int x __asm__(\"y\") __attribute__((unused)) [[deprecated]] = 1;", PR},
        {"n5r.p7", "int g __attribute__((aligned(8))) [[deprecated]];", PR},
        {"n5t.p14", "int v [[deprecated]] __attribute__((unused)) __asm__(\"y\") = 1;", R, kMisplaced},
        {"n5t.p15", "int v __attribute__((unused)) __asm__(\"y\") __attribute__((unused)) = 1;", R, kMisplaced},
        {"n5t.p16", "int v __asm__(\"y\") __asm__(\"z\") = 1;", R, DiagnosticCode::S_AsmLabelDuplicate},
    };
    expectCells(kCells);
}

// clang and Apple clang take a GNU run BEFORE the label on the FIRST declarator of a
// declaration that declares a FUNCTION (gcc and MinGW-w64 gcc refuse it): one working
// reference, so taken there and nowhere else — not on a pointer to a function, an
// array, or a later declarator, which all four refuse.
TEST(DeclaratorRunOrder, AGnuRunBeforeTheLabelIsTakenOnlyOnALeadingFunctionDeclarator) {
    static constexpr Cell kCells[] = {
        {"n5o.p9", "int f(void) __asm__(\"g\") __attribute__((unused));", A},
        {"n5o.p10", "int f(void) __attribute__((unused)) __asm__(\"g\");", A},
        {"n5t.p1", "int f(void) [[vnd::p]] __asm__(\"g\") __attribute__((unused));", A},
        {"n5t.p2", "int f(void) __attribute__((unused)) [[vnd::p]];", PR},
        {"n5t.p3", "int f(void) __attribute__((unused)) __asm__(\"g\") __attribute__((cold));", A},
        {"n5t.p4", "int f(void) [[vnd::p]] __attribute__((unused)) __asm__(\"g\");", A},
        {"n5t.p5", "int f(void) __asm__(\"g\") [[vnd::p]];", PR},
        {"n5t.p6", "int f(void) __attribute__((unused)) __attribute__((cold)) __asm__(\"g\");", A},
        {"n5t.p7", "int *f(void) __attribute__((unused)) __asm__(\"g\");", A},
        {"n5t.p8", "int (f)(void) __attribute__((unused)) __asm__(\"g\");", A},
        {"n5t.p9", "int (*fp)(void) __attribute__((unused)) __asm__(\"gp\") = 0;", R, kMisplaced},
        {"n5t.p10", "int (*fp)(void) __asm__(\"gp\") __attribute__((unused)) = 0;", A},
        {"n5t.p11", "int a[2] __attribute__((unused)) __asm__(\"ga\") = {0};", R, kMisplaced},
        {"n5t.p12", "int a[2] __asm__(\"ga\") __attribute__((unused)) = {0};", A},
        {"n5t.p13", "int y0 = 1, f(void) __attribute__((unused)) __asm__(\"g\");", R, kMisplaced},
    };
    expectCells(kCells);
}

// ...and the label it takes there is the symbol's name (the place check passes the
// label through, it does not merely stop refusing).
TEST(DeclaratorRunOrder, TheLeadingFunctionDeclaratorsLabelRenamesItsSymbol) {
    auto m = analyzeC("int f(void) __attribute__((unused)) __asm__(\"g_renamed\");\n"
                      "int main(void) { return 0; }\n");
    EXPECT_FALSE(m.diagnostics().hasErrors());
    EXPECT_EQ(asmNameOf(m, "f"), "g_renamed");
}

// A label before a function BODY is refused by all four (GCC's manual: declare the
// function with the label, then define it); a GNU run on a definition is clang's (gcc
// refuses it), and the C23 sequence after its declarator is everyone's.
TEST(DeclaratorRunOrder, AFunctionDefinitionCarriesNoAsmLabel) {
    static constexpr Cell kCells[] = {
        {"n5t.p17", "int f(void) __attribute__((unused)) { return 0; }", A},
        {"n5t.p18", "int f(void) __asm__(\"g\") { return 0; }", R, kMisplaced},
        {"n5t.p19", "int f(void) [[vnd::p]] { return 0; }", A},
        {"n5t.p20", "int f(void) __asm__(\"g\") __attribute__((unused)) { return 0; }", R, kMisplaced},
        {"n5t.p21", "int f(void) __attribute__((unused)) __asm__(\"g\") { return 0; }", R, kMisplaced},
    };
    expectCells(kCells);
}

// ── THE TYPEDEF SLOT: the same order, per declarator, and a label said ignored ──
TEST(DeclaratorRunOrder, ATypedefSlotTakesTheSameRunPerDeclarator) {
    static constexpr Cell kCells[] = {
        {"n5t.p22", "typedef int T [[deprecated]];", A},
        {"n5t.p23", "typedef int T __attribute__((aligned(8))) [[deprecated]];", PR},
        {"n5t.p24", "typedef int T [[deprecated]] __attribute__((aligned(8)));", A},
        {"n5t.p25", "typedef int T __asm__(\"t\");", A},
        {"n5u.p8", "typedef int A [[deprecated]], B;", A},
        {"n5u.p9", "typedef int A __asm__(\"t\"), B;", A},
        // n5v v9: a typedef's GNU run before its label -- gcc 15.2.0, clang 21.1.8, Apple clang and
        // MinGW-w64 gcc 13.2.0 all refuse it, as they refuse an object's.
        {"n5v.p9", "typedef int T __attribute__((unused)) __asm__(\"t\");", R, kMisplaced},
    };
    expectCells(kCells);
}

// The sequence after a NON-LAST typedef declarator confers on that declarator alone
// (all four references warn at a use of A and not of B), and after the last on the last.
TEST(DeclaratorRunOrder, ATypedefSequenceConfersOnItsOwnDeclaratorAlone) {
    auto first = analyzeC("typedef int A [[deprecated]], B;\nB b0 = 1;\nA a0 = 1;\n");
    EXPECT_EQ(countCode(first.diagnostics(), DiagnosticCode::S_DeprecatedSymbolUsed), 1u)
        << "one use of A warns, the use of B does not";
    auto last = analyzeC("typedef int A, B [[deprecated]];\nA a0 = 1;\nB b0 = 1;\n");
    EXPECT_EQ(countCode(last.diagnostics(), DiagnosticCode::S_DeprecatedSymbolUsed), 1u)
        << "the last declarator's sequence stays its own";
}

// A typedef names a type, which has no symbol: gcc and clang take the label and ignore it
// in silence; it is accepted here and the drop SAID, and the typedef keeps no asm name.
TEST(DeclaratorRunOrder, ATypedefsAsmLabelIsAcceptedAndSaidIgnored) {
    auto m = analyzeC("typedef int T __asm__(\"t\");\nT x = 1;\n");
    EXPECT_FALSE(m.diagnostics().hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AsmLabelOnTypedef), 1u);
    EXPECT_EQ(asmNameOf(m, "T"), "") << "the ignored label must not stay on the typedef";
    auto plain = analyzeC("typedef int T;\nT x = 1;\n");
    EXPECT_EQ(countCode(plain.diagnostics(), DiagnosticCode::S_AsmLabelOnTypedef), 0u);
}

// ── THE PARAMETER: the C23 sequence after its declarator ───────────────────────
TEST(DeclaratorRunOrder, AParameterTakesTheSequenceAfterItsDeclarator) {
    static constexpr Cell kCells[] = {
        {"n5t.p26", "int h(int p [[maybe_unused]]);", A},
        {"n5t.p27", "int h(int p __attribute__((unused)));", A},
        {"n5t.p28", "int h(int p [[maybe_unused]] __attribute__((unused)));", A},
        {"n5t.p29", "int h(int p __attribute__((unused)) [[maybe_unused]]);", PR},
        {"n5t.p30", "int h(int p __asm__(\"q\"));", PR},
    };
    expectCells(kCells);
}

// ── THE END OF A HEADED DECLARATION'S SPECIFIERS, EVERY SLOT ───────────────────
// An unrecognised attribute is IGNORED (C23 6.7.13.1), so each of these is valid C; gcc,
// clang, Apple clang and MinGW-w64 gcc accept every one. Also the GNU spelling of the
// slot on a parameter (gcc and clang accept it), and the typedef slot as the control.
TEST(DeclaratorRunOrder, TheEndOfSpecifiersSlotIsTakenOnEveryHeadedDeclaration) {
    static constexpr Cell kCells[] = {
        {"n5u.p1", "int [[vnd::p]] g = 1;", A},
        {"n5u.p2", "int u(void) { int [[vnd::p]] l = 1; return l; }", A},
        {"n5u.p3", "struct S { int [[vnd::p]] x; int y; };", A},
        {"n5u.p4", "struct S { int [[vnd::p]] : 3; int y; };", A},
        {"n5u.p5", "int h(int [[vnd::p]] p);", A},
        {"n5u.p6", "typedef int [[vnd::p]] T;", A},
        {"n5r.p8", "struct S { int [[deprecated]] : 3; int y; };", A},
        {"n5u.p13", "static int [[deprecated]] m = 1;", A},
        {"extern", "extern int [[vnd::p]] g;", A},
        {"a typedef-name head", "typedef int T;\nT [[vnd::p]] g;", A},
        {"a for-loop head", "int u(void) { for (int [[vnd::p]] i = 0; i < 1; ++i) {} return 0; }", A},
        {"a GNU run after a parameter's head", "int h(int __attribute__((unused)) p);", A},
    };
    expectCells(kCells);
}

// What the slot means: a STANDARD attribute appertains to the type and is said ignored
// (gcc's `'deprecated' attribute ignored`), never conferred on the object; an unknown one
// is said unknown, once.
TEST(DeclaratorRunOrder, AStandardAttributeAtTheEndOfTheSpecifiersIsSaidIgnored) {
    auto m = analyzeC("static int [[deprecated]] m = 1;\nint um(void) { return m; }\n");
    EXPECT_FALSE(m.diagnostics().hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeIgnoredForDeclarationKind), 1u);
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_DeprecatedSymbolUsed), 0u)
        << "the attribute decorates the type; no reference warns at the use";
    auto u = analyzeC("int [[vnd::p]] g = 1;\n");
    EXPECT_EQ(countCode(u.diagnostics(), DiagnosticCode::S_UnknownAttribute), 1u);
}

// `[[gnu::aligned(16)]]` there decorates the HEAD type: gcc and MinGW-w64 gcc make the
// object, the typedef and the member 16-aligned (clang refuses the spelling — gcc's
// reading). RED-ON-DISABLE: the head step returning the head unchanged leaves all three
// at 4, and the typedef slot (accepted before this fold) is the one that DISCARDED it.
TEST(DeclaratorRunOrder, AGnuAlignedAtTheEndOfTheSpecifiersAlignsTheHeadType) {
    auto obj = analyzeC("int [[gnu::aligned(16)]] g = 1;\nint [[gnu::aligned(16)]] *p;\n");
    EXPECT_FALSE(obj.diagnostics().hasErrors());
    EXPECT_EQ(alignOf(obj, "g"), 16u);
    EXPECT_EQ(alignOf(obj, "p"), 8u) << "the pointer is not aligned, its pointee is";
    EXPECT_EQ(countCode(obj.diagnostics(), DiagnosticCode::S_AttributeIgnoredForDeclarationKind), 0u)
        << "an alignment the type carries is not said ignored";
    auto td = analyzeC("typedef int [[gnu::aligned(16)]] TA;\nTA t0;\n");
    EXPECT_EQ(alignOf(td, "TA"), 16u);
    EXPECT_EQ(alignOf(td, "t0"), 16u);
    auto mem = analyzeC("struct S { char c; int [[gnu::aligned(16)]] x; };\nstruct S s0;\n");
    EXPECT_EQ(alignOf(mem, "s0"), 16u);
    auto control = analyzeC("int g = 1;\n");
    EXPECT_EQ(alignOf(control, "g"), 4u);
}

// ── THE TYPE GRAIN'S OTHER MEANINGS ──────────────────────────────────────────────────
// n5v (gcc 15.2.0, MinGW-w64 gcc 13.2.0; clang 21.1.8 and Apple clang refuse both spellings
// there, their reading and the cost in the row): a machine mode at the end of the specifiers
// REPLACES the head type -- `int [[gnu::mode(QI)]] q;` and its typedef are one byte, as the GNU
// spelling at the same slot already was; an `aligned` after an ARRAY declarator decorates the
// array type that suffix forms (an object, a member, a typedef: 16-aligned) where the suffix is
// the direct declarator's only one -- with two the suffix forms an element type, whose size gcc
// refuses when the alignment exceeds it, and the clause is said ignored. RED-ON-DISABLE: the
// mode's substitution or the array type's step dropped, or either clause said ignored.
TEST(DeclaratorRunOrder, AGnuModeAtTheEndOfTheSpecifiersReplacesTheHeadType) {
    auto obj = analyzeC("int [[gnu::mode(QI)]] q, r;\ntypedef int [[gnu::mode(QI)]] TQ;\nTQ t;\n"
                        "int __attribute__((mode(QI))) g;\n");
    EXPECT_FALSE(obj.diagnostics().hasErrors());
    EXPECT_EQ(sizeOf(obj, "q"), 1u) << "the head type is the one-byte integer the mode names";
    EXPECT_EQ(sizeOf(obj, "r"), 1u) << "every declarator of the head takes it";
    EXPECT_EQ(sizeOf(obj, "TQ"), 1u) << "a typedef's head takes it";
    EXPECT_EQ(sizeOf(obj, "t"), 1u);
    EXPECT_EQ(sizeOf(obj, "g"), 1u) << "CONTROL: the GNU spelling at the same slot";
    EXPECT_EQ(countCode(obj.diagnostics(), DiagnosticCode::S_AttributeIgnoredForDeclarationKind), 0u)
        << "a mode the type carries is not said ignored";
    auto control = analyzeC("int q;\n");
    EXPECT_EQ(sizeOf(control, "q"), 4u);
}

TEST(DeclaratorRunOrder, AGnuAlignedAfterAnArrayDeclaratorAlignsTheArrayType) {
    auto obj = analyzeC("int a[2] [[gnu::aligned(16)]];\n"
                        "struct S { char c; int m[4] [[gnu::aligned(16)]]; };\nstruct S s0;\n"
                        "typedef int TA[4] [[gnu::aligned(16)]];\nTA t0;\n");
    EXPECT_FALSE(obj.diagnostics().hasErrors());
    EXPECT_EQ(alignOf(obj, "a"), 16u) << "the array type the suffix forms takes the alignment";
    EXPECT_EQ(alignOf(obj, "s0"), 16u) << "a member array's";
    EXPECT_EQ(alignOf(obj, "TA"), 16u) << "a typedef's";
    EXPECT_EQ(alignOf(obj, "t0"), 16u);
    EXPECT_EQ(countCode(obj.diagnostics(), DiagnosticCode::S_AttributeIgnoredForDeclarationKind), 0u)
        << "an alignment the array type carries is not said ignored";
    auto two = analyzeC("int b[2][3] [[gnu::aligned(16)]];\n");
    EXPECT_FALSE(two.diagnostics().hasErrors());
    EXPECT_EQ(alignOf(two, "b"), 4u)
        << "with two suffixes the clause would decorate an element: said ignored, not applied";
    EXPECT_EQ(countCode(two.diagnostics(), DiagnosticCode::S_AttributeIgnoredForDeclarationKind), 1u);
}

// ── THE MEMBER POSITIONS ─────────────────────────────────────────────────────────
// After a width C23 has no place: every reference refuses the vendor spelling, and cl's
// acceptance of `[[deprecated]]` there is of an attribute it does not recognise (C5030).
TEST(DeclaratorRunOrder, ASequenceAfterABitFieldWidthStaysRefused) {
    static constexpr Cell kCells[] = {
        {"n5u.p7", "struct S { int x : 3 [[vnd::p]]; int y; };", PR},
        {"n5r.p3", "struct S { int x : 3 [[deprecated]]; int y; };", PR},
        {"n5r.p1", "struct S { int x [[deprecated]] : 3; int y; };", A},
        {"n5r.p5", "struct S { int x __attribute__((aligned(8))) [[deprecated]]; int y; };", PR},
    };
    expectCells(kCells);
}

// ── THE TWO DECLARATOR KEYS, FAIL-LOUD ───────────────────────────────────────────
namespace {
[[nodiscard]] std::string shippedC() {
    std::ifstream in{dss::test::configRoot() / "sources" / "c.lang.json", std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}
[[nodiscard]] std::string replaced(std::string s, std::string_view from, std::string_view to) {
    auto const at = s.find(from);
    if (at == std::string::npos) return {};
    s.replace(at, from.size(), to);
    return s;
}
} // namespace

TEST(DeclaratorRunOrder, TheAsmLabelPlaceKeysAreReadAndRefusedLoud) {
    std::string const c = shippedC();
    ASSERT_FALSE(c.empty()) << "cannot read the shipped c.lang.json";
    auto shipped = GrammarSchema::loadFromText(c);
    ASSERT_TRUE(shipped.has_value());
    ASSERT_TRUE((*shipped)->semantics().declarators.has_value());
    auto const& dc = *(*shipped)->semantics().declarators;
    ASSERT_EQ(dc.asmLabelLeadingFunctionOnlyRules.size(), 1u);
    EXPECT_EQ(dc.asmLabelLeadingFunctionOnlyRuleNames[0], "attrSpec");
    EXPECT_TRUE(dc.asmLabelRefusedOnDefinition);
    std::string_view const runs = R"("asmLabelLeadingFunctionOnlyRules": [ "attrSpec" ])";
    std::string_view const def  = R"("asmLabelRefusedOnDefinition": true)";
    struct Bad { char const* why; std::string doc; DiagnosticCode code; };
    Bad const bad[] = {
        {"an unknown rule", replaced(c, runs, R"("asmLabelLeadingFunctionOnlyRules": [ "noSuchRule" ])"),
         DiagnosticCode::C_UnknownShape},
        {"not an array", replaced(c, runs, R"("asmLabelLeadingFunctionOnlyRules": "attrSpec")"),
         DiagnosticCode::C_InvalidSemantics},
        {"an empty array", replaced(c, runs, R"("asmLabelLeadingFunctionOnlyRules": [ ])"),
         DiagnosticCode::C_InvalidSemantics},
        {"not a boolean", replaced(c, def, R"("asmLabelRefusedOnDefinition": 1)"),
         DiagnosticCode::C_InvalidSemantics},
        {"no asmLabelRule", replaced(c, R"("asmLabelRule":         "asmLabel",)", ""),
         DiagnosticCode::C_InvalidSemantics},
    };
    for (Bad const& b : bad) {
        ASSERT_FALSE(b.doc.empty()) << b.why << ": the shipped key moved";
        auto r = GrammarSchema::loadFromText(b.doc);
        ASSERT_FALSE(r.has_value()) << b.why << " must fail the load";
        bool named = false;
        for (auto const& d : r.error()) named = named || d.code == b.code;
        EXPECT_TRUE(named) << b.why << ": refused, but not with " << diagnosticCodeName(b.code);
    }
    // Both keys absent (each dropped WITH its comma): the load succeeds and judges nothing.
    std::string noKeys = replaced(replaced(c, std::string{runs} + ",", ""), std::string{def} + ",", "");
    ASSERT_FALSE(noKeys.empty());
    auto none = GrammarSchema::loadFromText(noKeys);
    ASSERT_TRUE(none.has_value()) << "both keys are optional";
    EXPECT_TRUE((*none)->semantics().declarators->asmLabelLeadingFunctionOnlyRules.empty());
    EXPECT_FALSE((*none)->semantics().declarators->asmLabelRefusedOnDefinition);
}
