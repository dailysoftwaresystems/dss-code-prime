// analysis/semantic/test_diagnostic_type_spelling — EVERY ANALYZER SENTENCE THAT NAMES A TYPE NAMES IT AS THE
// LANGUAGE SPELLS IT, AND SAYS ITSELF WITHOUT ONE WHERE THE LANGUAGE SPELLS NONE (cycle P69 f26d, lane `cs`,
// rulings 617 and 621).
//
// The analyzer had its own renderers — a lambda printing core kind names (`deduces 'I32' … and 'F64'`), a
// structure speller that knew two tag words — and two sentences that named no type where a reference names it
// (clang names a conditional's two arms, MEASURED by probe n507c: "('int *' and 'int')"; gcc a later declaration's
// type: "have 'double(void)'"). Every one now asks the ONE renderer (`spellType`, core/types/type_spelling.hpp)
// over the language document's `semantics.typeSpelling` table. The table is optional (ruling 621): a language that
// states none names no types, so each site has TWO sentences. The table below is the universe of the migrated
// sites, each read under BOTH documents — the shipped c, and the shipped c with its table turned into a
// documentation key — and each form must hold its own words and not the other's.
//
// RED-ON-DISABLE: a site reverted to a renderer of its own prints a core kind name or loses the table's words (its
// `withTable` cell); a site that spells a type whatever the document says loses its `withoutTable` cell.

#include "semantic_test_fixture.hpp"

#include "repo_root.hpp"

#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/core_type.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dss {
namespace {

// The shipped c document, its `semantics.typeSpelling` table turned into a documentation key (`$typeSpelling`):
// a language whose diagnostics name no types, every other row the shipped one.
[[nodiscard]] std::shared_ptr<GrammarSchema const> cWithoutTypeSpelling() {
    std::ifstream in(test::configRoot() / "sources" / "c.lang.json", std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::string const key = "\"typeSpelling\": {";
    auto const at = text.find(key);
    if (at == std::string::npos || text.find(key, at + 1) != std::string::npos)
        throw std::runtime_error("the shipped c document no longer carries exactly one typeSpelling table");
    text.replace(at, key.size(), "\"$typeSpelling\": {");
    auto loaded = GrammarSchema::loadFromText(text, "<c-without-type-spelling>");
    if (!loaded) {
        std::string why;
        for (auto const& d : loaded.error()) why += "\n  " + d.path + ": " + d.message;
        throw std::runtime_error("the c document without its typeSpelling table did not load:" + why);
    }
    if ((*loaded)->semantics().typeSpelling.declared)
        throw std::runtime_error("the edited c document still declares a typeSpelling table");
    return *loaded;
}

[[nodiscard]] std::vector<std::string> actualsUnder(std::shared_ptr<GrammarSchema const> const& schema,
                                                    std::string const& src, DiagnosticCode code) {
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(src, "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel const model = analyze(cu, DiagnosticBudget::libraryDefault());
    std::vector<std::string> out;
    for (auto const& d : model.diagnostics().all())
        if (d.code == code) out.push_back(d.actual);
    return out;
}

struct Site {
    std::string_view what;
    DiagnosticCode   code;
    std::string      src;
    std::string_view withTable;      // the words under the shipped c document
    std::string_view withoutTable;   // the words under the c document with no table
};

TEST(DiagnosticTypeSpelling, EverySiteNamesTheTypeAsTheLanguageSpellsItOrSaysItsSentenceWithoutOne) {
    auto const shipped = sem_test::loadShippedSchema("c");
    ASSERT_TRUE(shipped->semantics().typeSpelling.declared) << "the shipped c document spells its types";
    auto const bare = cWithoutTypeSpelling();
    std::vector<Site> const sites = {
        {"a definition's incomplete return type", DiagnosticCode::S_IncompleteReturnType,
         "struct S;\nstruct S f(void) { for (;;) {} }\n",
         "(C 6.9.1p3) — 'struct S' is incomplete here", "(C 6.9.1p3) — its result type is incomplete here"},
        {"a definition's incomplete parameter", DiagnosticCode::S_IncompleteTypeObject,
         "struct S;\nvoid f(struct S s) { (void)0; }\n",
         "(C 6.7.6.3p4) — 'struct S' is incomplete here", "(C 6.7.6.3p4) — its type is incomplete here"},
        {"a call through an incomplete result", DiagnosticCode::S_IncompleteReturnType,
         "union U;\nunion U f(void);\nvoid g(void) { f(); }\n",
         "the called function returns 'union U', which is incomplete",
         "the called function returns a type, which is incomplete"},
        {"an incomplete argument", DiagnosticCode::S_IncompleteArgumentType,
         "struct S;\nextern struct S x;\nvoid f(struct S s);\nvoid g(void) { f(x); }\n",
         "argument 1 has type 'struct S', which is incomplete", "argument 1 has a type, which is incomplete"},
        {"a brace element no rule admits", DiagnosticCode::S_TypeMismatch,
         "struct S { int v; };\nint main(void) { struct S s = { 1 }; int a[1] = { s }; return a[0]; }\n",
         "initializes is an integer (an element of type 'struct S' for a slot of type 'int'): an element",
         "initializes is an integer: an element"},
        {"two declarators of one inferred declaration", DiagnosticCode::S_AutoDeclaratorsInferDifferentTypes,
         "int main(void) { auto a = 1, b = 2.5; return (int)b; }\n",
         "deduces 'int' for 'a = 1' and 'double' for 'b = 2.5'",
         "deduces one type for 'a = 1' and another for 'b = 2.5'"},
        {"two pointer deductions (the pointee spelled through)", DiagnosticCode::S_AutoDeclaratorsInferDifferentTypes,
         "int main(void) { int x = 1; double d = 2.0; auto a = &x, b = &d; return *a; }\n",
         "deduces 'int *' for 'a = &x' and 'double *' for 'b = &d'",
         "deduces one type for 'a = &x' and another for 'b = &d'"},
        {"the operand of a unary operator", DiagnosticCode::S_TypeMismatch,
         "int main(void) { int *p = 0; (void)(-p); return 0; }\n",
         "the operand of unary `-` is a pointer ('int *'), and C23 6.5.4.3",
         "the operand of unary `-` is a pointer, and C23 6.5.4.3"},
        {"the operands of a binary operator", DiagnosticCode::S_TypeMismatch,
         "int main(void) { int *p = 0; double *q = 0; (void)(p + q); return 0; }\n",
         "the operands of `+` are a pointer ('int *') and a pointer ('double *'), which",
         "the operands of `+` are a pointer and a pointer, which"},
        {"a conditional's pointer and integer arms", DiagnosticCode::S_ConditionalOperandTypeMismatch,
         "int main(void) { int c = 1; int *p = 0; (void)(c ? p : 1); return 0; }\n",
         "are a pointer and an integer ('int *' and 'int') (the integer",
         "are a pointer and an integer (the integer"},
        {"the same arms the other way round, an array arm decayed", DiagnosticCode::S_ConditionalOperandTypeMismatch,
         "int main(void) { int c = 1; double a[2] = { 0, 0 }; (void)(c ? 1 : a); return 0; }\n",
         "are a pointer and an integer ('int' and 'double *') (the integer",
         "are a pointer and an integer (the integer"},
        {"an implicit declaration a later definition contradicts", DiagnosticCode::S_ImplicitDeclarationConflicts,
         "int b(void) { return (int)f(40) + 2; }\ndouble f(double v) { return v; }\n",
         "its later declaration has the type 'double (double)'", "its later declaration returns another type"},
        {"... a later prototype of no parameters", DiagnosticCode::S_ImplicitDeclarationConflicts,
         "int f(void) { return g(); }\ndouble g(void);\n",
         "its later declaration has the type 'double (void)'", "its later declaration returns another type"},
        {"... a later declaration WITHOUT a prototype",
         DiagnosticCode::S_ImplicitDeclarationConflicts,
         "int f(void) { return g(); }\ndouble g();\n",
         "its later declaration has the type 'double ()'", "its later declaration returns another type"},
    };
    for (Site const& site : sites) {
        SCOPED_TRACE(std::string{site.what} + "\n" + site.src);
        auto const withTable = actualsUnder(shipped, site.src, site.code);
        ASSERT_EQ(withTable.size(), 1u) << "the site must fire once under the shipped c document";
        EXPECT_NE(withTable[0].find(site.withTable), std::string::npos) << "got: " << withTable[0];
        EXPECT_EQ(withTable[0].find(site.withoutTable), std::string::npos) << "got: " << withTable[0];
        auto const withoutTable = actualsUnder(bare, site.src, site.code);
        ASSERT_EQ(withoutTable.size(), 1u) << "the site must fire once under the c document with no table";
        EXPECT_NE(withoutTable[0].find(site.withoutTable), std::string::npos) << "got: " << withoutTable[0];
        EXPECT_EQ(withoutTable[0].find(site.withTable), std::string::npos) << "got: " << withoutTable[0];
        // No core kind's name reaches either sentence as a type (`'I32'`, `'F64 *'`).
        for (std::size_t k = 0; k < static_cast<std::size_t>(TypeKind::Count_); ++k) {
            std::string_view const name = typeKindNameOrEmpty(static_cast<TypeKind>(k));
            if (name.empty()) continue;
            for (std::string const& a : {withTable[0], withoutTable[0]}) {
                EXPECT_EQ(a.find("'" + std::string{name} + "'"), std::string::npos) << "got: " << a;
                EXPECT_EQ(a.find("'" + std::string{name} + " *"), std::string::npos) << "got: " << a;
            }
        }
    }
}

} // namespace
} // namespace dss
