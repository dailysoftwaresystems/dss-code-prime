// P69 (lane `cs`) — A SECOND NAME OF A DEFINITION (`alias("target")`), AND WHAT A TENTATIVE DEFINITION
// MAY BE (`common`, `nocommon`), AS THE SEMANTIC TIER STATES THEM.
//
// `__attribute__((alias("g"))) int f(void);` makes `f` one more name of the definition `g` in this
// unit — never a definition of its own. gcc, clang and mingw-w64 gcc build it on ELF and PE; every
// Apple clang refuses it ("aliases are not supported on darwin"), and so a Mach-O target refuses it
// by name with the format document's own reason. The tier resolves each alias to the DEFINITION it
// ends at (through a chain of aliases), refuses by name an alias that names no definition of the
// unit, a cycle, an inline definition, a common, or a definition whose thread storage differs, and
// binds an internal alias's uses to its target. A tentative definition an alias names is a
// definition of the unit: it is never a common (`tentativeNeverCommon`).
//
// `nocommon` states that a tentative definition is never written as a common; `common` states the
// default where the format has commons and is refused by name where it has none.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

struct Format {
    std::optional<SymbolAliasUnsupportedReason> aliasRefused;
    bool                                        statesCommons = true;
};

[[nodiscard]] SemanticModel analyzeC(std::string const& src, Format format = {}) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                   /*vaListStrategy=*/std::nullopt, /*activeFormat=*/std::nullopt,
                   /*activeTarget=*/std::nullopt, LongDoubleFormat::None, /*target=*/nullptr,
                   /*deepRecursionReserveBytes=*/0, /*roleResolver=*/nullptr, EnumCompatibleTypeRule::Gnu,
                   /*activeCallingConvention=*/std::nullopt, format.aliasRefused, format.statesCommons);
}

[[nodiscard]] std::string everyDiagnostic(SemanticModel const& m) {
    std::string s;
    for (auto const& d : m.diagnostics().all()) {
        s += "\n  ";
        s += diagnosticCodeName(d.code);
        s += ": ";
        s += d.actual;
    }
    return s.empty() ? std::string{"\n  (no diagnostics)"} : s;
}

// The name of the definition the alias `name` was resolved to, or "" when no record of `name` is one.
[[nodiscard]] std::string aliasTargetOf(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i) {
        SymbolRecord const& r = m.symbols()[i];
        if (r.name != name || !r.aliasTarget.valid()) continue;
        SymbolRecord const* t = m.recordFor(r.aliasTarget);
        return t == nullptr ? std::string{"(no record)"} : std::string{t->name};
    }
    return {};
}

// Whether any record of `name` is a tentative definition that may never be a common.
[[nodiscard]] bool neverCommon(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i) {
        SymbolRecord const& r = m.symbols()[i];
        if (r.name == name && r.tentativeNeverCommon) return true;
    }
    return false;
}

}  // namespace

TEST(SymbolAlias, ASecondNameIsResolvedToTheDefinitionItEndsAt) {
    struct Case {
        char const* src;
        char const* alias;
        char const* target;
    };
    for (Case const c : {
             Case{"int g(void) { return 41; }\nint f(void) __attribute__((alias(\"g\")));\n", "f", "g"},
             Case{"int f(void) __attribute__((alias(\"g\")));\nint g(void) { return 41; }\n", "f", "g"},
             Case{"int v = 3;\nextern int w __attribute__((alias(\"v\")));\n", "w", "v"},
             Case{"int g(void) { return 41; }\nint f(void) __attribute__((alias(\"g\")));\n"
                  "int h(void) __attribute__((alias(\"f\")));\n",
                  "h", "g"},
             Case{"int g(void) { return 41; }\nint f(void) __attribute__((weak, alias(\"g\")));\n", "f", "g"},
             Case{"static int g(void) { return 41; }\nstatic int f(void) __attribute__((alias(\"g\")));\n"
                  "int use(void) { return f(); }\n",
                  "f", "g"},
         }) {
        SCOPED_TRACE(c.src);
        SemanticModel const m = analyzeC(c.src);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(aliasTargetOf(m, c.alias), c.target);
    }
    // CONTROL: a declaration with no alias names nothing.
    SemanticModel const m = analyzeC("int g(void) { return 41; }\nint f(void);\n");
    EXPECT_EQ(aliasTargetOf(m, "f"), "");
}

TEST(SymbolAlias, AnAliasThatNamesNoDefinitionItCanBeIsRefusedByName) {
    for (char const* src : {
             "int f(void) __attribute__((alias(\"nope\")));\n",
             "int g(void);\nint f(void) __attribute__((alias(\"g\")));\n",
             "int f(void) __attribute__((alias(1)));\n",
             "int a(void) __attribute__((alias(\"b\")));\nint b(void) __attribute__((alias(\"a\")));\n",
             "inline int g(void) { return 1; }\nint f(void) __attribute__((alias(\"g\")));\n",
             "int t __attribute__((common));\nextern int a __attribute__((alias(\"t\")));\n",
             "_Thread_local int t = 1;\nextern int a __attribute__((alias(\"t\")));\n",
         }) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeC(src);
        EXPECT_TRUE(m.hasErrors());
        EXPECT_GE(countCode(m.diagnostics(), DiagnosticCode::S_SymbolAliasTargetInvalid), 1u) << everyDiagnostic(m);
    }
}

TEST(SymbolAlias, AnAliasWhereNoSecondNameCanBeIsSaid) {
    struct Case {
        char const*    src;
        DiagnosticCode code;
        bool           error;
    };
    for (Case const c : {
             Case{"int g(void) { return 1; }\n__attribute__((alias(\"g\"))) int f(void) { return 2; }\n",
                  DiagnosticCode::S_AttributeIgnoredForDeclarationKind, false},
             Case{"int g(void) { return 1; }\nint h(void) { return 2; }\n"
                  "int f(void) __attribute__((alias(\"g\"), alias(\"h\")));\n",
                  DiagnosticCode::S_RedeclaredSymbol, true},
             Case{"int g(void) { return 1; }\nint f(void) __attribute__((alias(\"g\")));\n"
                  "int f(void) { return 2; }\n",
                  DiagnosticCode::S_RedeclaredSymbol, true},
             Case{"int g(void) { return 1; }\n"
                  "void u(void) { extern int f(void) __attribute__((alias(\"g\"))); }\n",
                  DiagnosticCode::S_AttributeNotHonoured, true},
             Case{"int v = 3;\nint w __attribute__((alias(\"v\"))) = 4;\n",
                  DiagnosticCode::S_SymbolAliasInitializerIgnored, false},
         }) {
        SCOPED_TRACE(c.src);
        SemanticModel const m = analyzeC(c.src);
        EXPECT_EQ(m.hasErrors(), c.error) << everyDiagnostic(m);
        EXPECT_GE(countCode(m.diagnostics(), c.code), 1u) << everyDiagnostic(m);
    }
}

TEST(SymbolAlias, AFormatWhosePlatformRefusesAliasesRefusesThemByName) {
    char const* const src = "int g(void) { return 41; }\nint f(void) __attribute__((alias(\"g\")));\n";
    SemanticModel const m = analyzeC(src, Format{SymbolAliasUnsupportedReason::PlatformCompilersRefuse});
    EXPECT_TRUE(m.hasErrors());
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 1u) << everyDiagnostic(m);
    EXPECT_EQ(aliasTargetOf(m, "f"), "") << "a refused alias names nothing";
    // CONTROL: the same unit where the format states no reason.
    SemanticModel const c = analyzeC(src);
    EXPECT_FALSE(c.hasErrors()) << everyDiagnostic(c);
}

TEST(SymbolAlias, CommonAndNocommonStateWhatATentativeDefinitionMayBe) {
    {
        SemanticModel const m = analyzeC("int t __attribute__((nocommon));\n");
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_TRUE(neverCommon(m, "t")) << "`nocommon`: a definition of its unit, never a common";
    }
    {
        SemanticModel const m = analyzeC("int t;\nextern int a __attribute__((alias(\"t\")));\n");
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_TRUE(neverCommon(m, "t")) << "a tentative definition an alias names is a definition";
    }
    {
        SemanticModel const m = analyzeC("int t __attribute__((common, nocommon));\n");
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeIgnoredForDeclarationKind), 1u)
            << "`nocommon` beside `common` is ignored, and said" << everyDiagnostic(m);
        EXPECT_FALSE(neverCommon(m, "t"));
    }
    {
        SemanticModel const m = analyzeC("int t __attribute__((common));\n", Format{std::nullopt, false});
        EXPECT_TRUE(m.hasErrors());
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 1u)
            << "a format with no commons cannot honour `common`" << everyDiagnostic(m);
    }
    // CONTROLS: `common` where the format has commons, and a plain tentative definition, state the default.
    for (char const* src : {"int t __attribute__((common));\n", "int t;\n"}) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeC(src);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_FALSE(neverCommon(m, "t"));
    }
}

TEST(SymbolAlias, ANamedSectionIsRefusedByName) {
    // At file scope and at block scope alike: an error naming the attribute and the reason, never the unknown-name
    // warning a block-scope attribute no document states draws.
    for (char const* src : {"int x __attribute__((section(\"my_data\"))) = 1;\n",
                            "int main(void) {\n    __attribute__((section(\"mysec\"))) static int x = 1;\n"
                            "    return x - 1;\n}\n"}) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeC(src);
        EXPECT_TRUE(m.hasErrors());
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_AttributeNotHonoured), 1u) << everyDiagnostic(m);
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_UnknownAttribute), 0u) << everyDiagnostic(m);
        for (auto const& d : m.diagnostics().all())
            if (d.code == DiagnosticCode::S_AttributeNotHonoured)
                EXPECT_NE(d.actual.find("'section'"), std::string::npos) << d.actual;
    }
}
