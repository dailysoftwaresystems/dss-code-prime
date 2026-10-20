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
// An OBJECT alias's declaration is written `extern` (an initializer of its own ignored, warned), or
// `static` with no initializer: gcc 13.3.0, clang 18.1.3 and mingw-w64 gcc 13.2.0 run a `static` alias
// of a `static` object, an `extern` one of a `static` object, a `static` one of an external object and
// an external alias of a `static` alias, -O0 and -O2 (✔MEASURED, lane `cs` probes rv cells 1-3, rv3
// cells 3 and 5). Written with external linkage and no `extern`, or `static` with an initializer, it is
// a definition of its own, refused by all (probes al2 cell 10, rv cell 10, rv2 cell 1).
//
// `nocommon` states that a tentative definition is never written as a common; `common` states the
// default where the format has commons and is refused by name where it has none. Of several tentative
// declarations of one name, the LAST one's `nocommon` is the definition's (gcc's reading of a split the
// references disagree on, `settleNoCommon`).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

// The MERGE SURVIVOR's answer — the one record of the object `name` no other absorbed, which is the
// record the lowering reads — or nullopt (with a failure) when there is not exactly one.
[[nodiscard]] std::optional<bool> survivorNeverCommon(SemanticModel const& m, std::string_view name) {
    SymbolRecord const* survivor = nullptr;
    std::size_t         count    = 0;
    for (std::size_t i = 1; i < m.symbols().size(); ++i) {
        SymbolRecord const& r = m.symbols()[i];
        if (r.name != name || r.kind != DeclarationKind::Variable || r.isAbsorbedProto) continue;
        survivor = &r;
        ++count;
    }
    if (count != 1) {
        ADD_FAILURE() << "the object '" << name << "' has " << count << " surviving records, not one";
        return std::nullopt;
    }
    return survivor->tentativeNeverCommon;
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
             // OBJECT aliases of each linkage pair (probe rv cells 1-3).
             Case{"static int t = 5;\nstatic int a __attribute__((alias(\"t\")));\nint use(void) { return a; }\n",
                  "a", "t"},
             Case{"static int t = 5;\nextern int a __attribute__((alias(\"t\")));\nint use(void) { return a; }\n",
                  "a", "t"},
             Case{"int t = 5;\nstatic int a __attribute__((alias(\"t\")));\nint use(void) { return a; }\n", "a",
                  "t"},
             // A tentative declaration of the alias's name, before it (gcc runs it, clang refuses it: probe rv2
             // cells 2 and 3) and after it (both run it: rv2 cell 4) — no definition beside the alias.
             Case{"static int t = 5;\nstatic int a;\nstatic int a __attribute__((alias(\"t\")));\n"
                  "int use(void) { return a; }\n",
                  "a", "t"},
             Case{"static int t = 5;\nstatic int a __attribute__((alias(\"t\")));\nstatic int a;\n"
                  "int use(void) { return a; }\n",
                  "a", "t"},
             Case{"int t = 5;\nint a;\nextern int a __attribute__((alias(\"t\")));\nint use(void) { return a; }\n",
                  "a", "t"},
             // An external alias of a `static` alias ends at the storage (probe rv3 cell 3): a name an alias
             // defines is reached as an alias, never as a definition of its own.
             Case{"static int t = 42;\nstatic int a __attribute__((alias(\"t\")));\n"
                  "extern int b __attribute__((alias(\"a\")));\nint use(void) { return b; }\n",
                  "b", "t"},
             // `common` on an object the unit INITIALIZES makes no common, so the object has a second name
             // (probe rv cells 11 and 12).
             Case{"int g __attribute__((common)) = 5;\nextern int a __attribute__((alias(\"g\")));\n", "a", "g"},
             Case{"int g __attribute__((common));\nint g = 5;\nextern int a __attribute__((alias(\"g\")));\n", "a",
                  "g"},
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
             // A `static` object alias that names itself: a cycle (probe rv3 cell 4, refused by all).
             "static int a __attribute__((alias(\"a\")));\nint use(void) { return a; }\n",
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
        char const*    says = "";   // the reason the diagnostic must state, where the case names one
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
             // Written `extern`, the object alias's own initializer is ignored and said (gcc's reading: gcc and
             // mingw-w64 gcc warn and run it, clang refuses; probe rv cell 16) ...
             Case{"int v = 3;\nextern int w __attribute__((alias(\"v\"))) = 4;\n",
                  DiagnosticCode::S_SymbolAliasInitializerIgnored, false},
             // ... and without `extern` the declaration is a definition of its own, refused by all: with an
             // initializer (probe rv cell 10), `static` with one (rv2 cell 1), or external with none (al2 cell 10).
             Case{"int v = 3;\nint w __attribute__((alias(\"v\"))) = 4;\n", DiagnosticCode::S_RedeclaredSymbol, true,
                  "is written `extern`, and the initializer is ignored"},
             // The `static` form with an initializer: the TRUE reason is that it takes none (an alias has no
             // storage of its own), never "write it `extern`", which would change the alias's linkage (cs-28 item 9).
             Case{"static int v = 3;\nstatic int w __attribute__((alias(\"v\"))) = 4;\n",
                  DiagnosticCode::S_RedeclaredSymbol, true, "a `static` object alias takes no initializer"},
             Case{"int v = 3;\nint w __attribute__((alias(\"v\")));\n", DiagnosticCode::S_RedeclaredSymbol, true,
                  "with external linkage is written `extern`"},
         }) {
        SCOPED_TRACE(c.src);
        SemanticModel const m = analyzeC(c.src);
        EXPECT_EQ(m.hasErrors(), c.error) << everyDiagnostic(m);
        EXPECT_GE(countCode(m.diagnostics(), c.code), 1u) << everyDiagnostic(m);
        if (*c.says != '\0') {
            bool said = false;
            for (auto const& d : m.diagnostics().all())
                said = said || (d.code == c.code && d.actual.find(c.says) != std::string::npos);
            EXPECT_TRUE(said) << "the refusal does not state its reason (\"" << c.says << "\"):" << everyDiagnostic(m);
        }
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
    // Two tentative declarations of one object, `nocommon` on one of them: the MERGE SURVIVOR — the record the
    // lowering reads — holds the LAST one's answer. ✔MEASURED (probe rv cells 14 and 15, nm under -fcommon):
    // `int t; int t __attribute__((nocommon));` is a `.bss` definition on gcc 13.3.0, clang 18.1.3 and mingw-w64
    // gcc 13.2.0; `int t __attribute__((nocommon)); int t;` is a COMMON on gcc and mingw-w64 gcc and a `.bss`
    // definition on clang — gcc's reading, decided (`settleNoCommon`).
    for (auto const& [src, never] : {std::pair<char const*, bool>{"int t;\nint t __attribute__((nocommon));\n", true},
                                     std::pair<char const*, bool>{"int t __attribute__((nocommon));\nint t;\n", false}}) {
        SCOPED_TRACE(src);
        SemanticModel const m = analyzeC(src);
        EXPECT_FALSE(m.hasErrors()) << everyDiagnostic(m);
        EXPECT_EQ(survivorNeverCommon(m, "t"), std::optional<bool>{never})
            << "the survivor holds the last tentative declaration's `nocommon`";
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
