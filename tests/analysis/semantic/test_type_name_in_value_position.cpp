// ===========================================================================
// P69 L2b2 (lane `lm`) — A TYPE NAME IS NO VALUE.
//
// THE PROPERTY THIS FILE OWNS: an identifier that is a primary expression designates
// an object or a function (C 6.5.2p2). A name the ordinary namespace binds to a TYPE —
// a typedef name, or a builtin type name the language binds there (`__builtin_va_list`)
// — standing where an expression is required is refused AT THE NAME, once, with
// S_TypeNameInValuePosition, and it is no undeclared name (it is declared, as a type).
//
// ✔MEASURED 2026-11-05 (the lane's probes l2b2t, x86-64 and AAPCS64 ELF): gcc 13.3.0
// ("expected expression before 'T'"), clang 18.1.3 and Apple clang ("unexpected type
// name 'T': expected expression") and cl 19.51 (C2275) refuse every position below at
// the name, for the typedef; gcc and both clangs for `__builtin_va_list` (cl has no such
// builtin). DSS refused the typedef only by an internal lowering refusal, and typed
// `__builtin_va_list` as a VALUE of the list type (a pointer-to-integer warning, a type
// mismatch) — the analyzer's reference arm bound the type symbol as a value.
//
// ── RED-ON-DISABLE ─────────────────────────────────────────────────────────
//   * the reference arm binding a type symbol as a value again → every refusal cell
//     counts 0 S_TypeNameInValuePosition;
//   * the refusal reported at the ENCLOSING construct (the return, the operator, the
//     call) rather than at the name → the position check;
//   * the refused use counted undeclared too → the S_UndeclaredIdentifier check.
// The controls (an object hiding the typedef, the type in a type position, a cast) pin
// that only a VALUE position refuses.
// ===========================================================================

#include "core/types/parse_diagnostic.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

constexpr char const* kHead = "typedef int T;\nint f(int a) { return a; }\nint main(void) { ";
constexpr char const* kTail = " }\n";

// The five value positions, each with the name spelled once as `%`.
std::vector<std::pair<char const*, std::string>> const kPositions = {
    {"a return",         "return %;"},
    {"an operand",       "return % + 1;"},
    {"an initializer",   "int y = %; return y;"},
    {"a call argument",  "return f(%);"},
    {"a cast's operand", "return (int)%;"},
};

[[nodiscard]] std::string withName(std::string stmt, std::string const& name) {
    auto const at = stmt.find('%');
    stmt.replace(at, 1, name);
    return stmt;
}

[[nodiscard]] std::string dump(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all())
        out += std::string{"\n    "} + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return out;
}

}  // namespace

TEST(TypeNameInValuePosition, ATypeNameWhereAValueIsRequiredIsRefusedAtTheName) {
    for (std::string const name : {"T", "__builtin_va_list"}) {
        for (auto const& [what, stmt] : kPositions) {
            std::string const body = withName(stmt, name);
            std::string const src  = std::string{kHead} + body + kTail;
            auto cu = buildShippedUnit("c", {src});
            assertNoBuilderErrors(*cu);
            auto model = analyze(cu, DiagnosticBudget::libraryDefault());
            auto const& r = model.diagnostics();
            EXPECT_EQ(countCode(r, DiagnosticCode::S_TypeNameInValuePosition), 1u)
                << "`" << name << "` in " << what << " must be refused once, as every "
                << "reference refuses it; it said:" << dump(r) << "\n" << src;
            EXPECT_EQ(countCode(r, DiagnosticCode::S_UndeclaredIdentifier), 0u)
                << "a type name is declared, as a type — never undeclared:" << dump(r);
            std::size_t const nameAt = std::string{kHead}.size() + body.find(name);
            for (auto const& d : r.all()) {
                if (d.code != DiagnosticCode::S_TypeNameInValuePosition) continue;
                EXPECT_EQ(d.span.start(), nameAt)
                    << "the refusal stands at the NAME, where every reference puts it\n" << src;
            }
        }
    }
}

TEST(TypeNameInValuePosition, OnlyAValuePositionRefusesATypeName) {
    struct Control { char const* what; char const* src; };
    for (Control const& c : {
             Control{"an object hiding the typedef is the value",
                     "typedef int T;\nint main(void) { int T = 42; return T; }\n"},
             Control{"the typedef in a type position",
                     "typedef int T;\nint main(void) { return (int)sizeof(T) - 4 + 42; }\n"},
             Control{"a cast through the typedef",
                     "typedef int T;\nint main(void) { return (T)42; }\n"},
             Control{"the builtin list type in a type position",
                     "int main(void) { return (int)sizeof(__builtin_va_list) * 0 + 42; }\n"},
         }) {
        auto cu = buildShippedUnit("c", {std::string{c.src}});
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeNameInValuePosition), 0u)
            << c.what << ":" << dump(model.diagnostics()) << "\n" << c.src;
        EXPECT_FALSE(model.diagnostics().hasErrors()) << c.what << ":" << dump(model.diagnostics());
    }
}
