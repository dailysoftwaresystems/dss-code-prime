// ===========================================================================
// P68 round 9 (lane `cs`) — `sizeof ( NAME )` WHERE NAME IS A VALUE THE PARSER'S
// BINDER SKETCH NEVER SAW DECLARED.
//
// THE PROPERTY THIS FILE OWNS: `sizeof ( X )` and `_Alignof ( X )` take the
// TYPE-NAME reading only when X names a type. The parser decides it with its
// binder sketch (`src/analysis/syntactic/binder_sketch.hpp`): a name it knows as
// a value rolls back to the expression reading, a name it knows as a type
// commits, and a name it does not know at all reaches the Unknown arm — which in
// `sizeof ( X )` has no following operand to read, so it COMMITS AS A TYPE.
//
// Two kinds of value never reached the sketch, so they took that arm and the
// semantic tier refused them S_UnknownType:
//   * the language's PREDEFINED identifiers (`semantics.predefinedFunctionNames`
//     — `__func__`, `__FUNCTION__`), which the source never declares at all;
//   * ENUMERATION CONSTANTS, which the sketch bound inside their enum's own body
//     scope, where they died when the enum closed — the analyzer republishes them
//     in the enclosing scope (`fieldChildren.liftToEnclosingScope`).
//
// ✔REFERENCE VOTES, 2026-09-23, each case one translation unit probed SEPARATELY
// (the lane's `.temp/probe/r2b/`, `r2c/`): gcc 13.3.0 and clang 18.1.3 (WSL) at
// `-std=c17 -pedantic-errors` and `-std=c2x`, mingw-w64 gcc 13.2.0 at both, MSVC
// 19.51.36260 at `/std:c17` and `/std:clatest`, every build RUN: `sizeof(__func__)`
// and `sizeof(E)` for an anonymous, a named and a block-scope enumerator build and
// exit 42 on all four (gcc's -pedantic-errors refuses only the non-ISO spelling
// `__FUNCTION__`, which clang and MSVC accept). DSS refused every one.
//
// ★ THE FALLBACK STAYS: a name the sketch does not know is still read as a type,
// because a type it cannot see (a typedef from another tree of the compilation
// unit, before the unit's oracle reparse) must keep working. The last test pins
// that the fallback still fires.
//
// ── RED-ON-DISABLE (the lane's transcript carries each build and its names) ──
//   * the predefined-identifier seed removed from the sketch → the `__func__` /
//     `__FUNCTION__` arms;
//   * the enumerator lift removed → the enumerator arms;
//   * the typedef controls and the unknown-name pin stay green under both.
// ===========================================================================

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/tree.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

// How many nodes of rule `ruleName` the unit's trees hold, counting only nodes
// REACHABLE from each root: a speculative probe the parser rolled back may leave
// its nodes in the arena, and they are not part of the parse.
[[nodiscard]] std::size_t countRule(CompilationUnit const& cu,
                                    std::string_view ruleName) {
    std::size_t n = 0;
    for (auto const& t : cu.trees()) {
        if (!t.hasSchema() || !t.root().valid()) continue;
        auto const r = t.schema().rules().find(ruleName);
        if (!r.valid()) continue;
        std::vector<NodeId> stack{t.root()};
        while (!stack.empty()) {
            NodeId const id = stack.back();
            stack.pop_back();
            if (t.kind(id) != NodeKind::Internal) continue;
            if (t.rule(id).v == r.v) ++n;
            for (NodeId c : t.children(id)) stack.push_back(c);
        }
    }
    return n;
}

// One translation unit: the reading its single `sizeof ( … )` / `_Alignof ( … )`
// must take, and that it analyzes with no Error.
struct Case {
    char const* what;
    char const* src;
    char const* valueRule;   // the operator's EXPRESSION-reading rule
    char const* typeRule;    // its TYPE-NAME-reading rule
    bool        typeReading;
};

void expectReadings(std::initializer_list<Case> cases) {
    for (Case const& c : cases) {
        auto cu = buildShippedUnit("c", {std::string{c.src}});
        auto model = analyze(cu, DiagnosticBudget::libraryDefault());
        EXPECT_EQ(countRule(*cu, c.typeRule), c.typeReading ? 1u : 0u)
            << c.what << " — the TYPE-NAME reading\n" << c.src;
        EXPECT_EQ(countRule(*cu, c.valueRule), c.typeReading ? 0u : 1u)
            << c.what << " — the EXPRESSION reading\n" << c.src;
        for (auto const& t : cu->trees()) {
            for (auto const& d : t.diagnostics().all()) {
                EXPECT_NE(d.severity, DiagnosticSeverity::Error)
                    << c.what << " — parse " << diagnosticCodeName(d.code) << ": "
                    << d.actual;
            }
        }
        for (auto const& d : model.diagnostics().all()) {
            EXPECT_NE(d.severity, DiagnosticSeverity::Error)
                << c.what << " — semantic " << diagnosticCodeName(d.code) << ": "
                << d.actual << "\n" << c.src;
        }
    }
}

constexpr char const* kSizeofValue = "sizeofValue";
constexpr char const* kSizeofType  = "sizeofType";

}  // namespace

// ── the language's predefined identifiers are VALUES ────────────────────────
TEST(SizeofValueNames, APredefinedIdentifierTakesTheExpressionReading) {
    expectReadings({
        {"`sizeof(__func__)`",
         "int main(void) { return sizeof(__func__) == 5 ? 0 : 1; }\n",
         kSizeofValue, kSizeofType, false},
        {"`sizeof(__FUNCTION__)` (the config's GNU alias)",
         "int main(void) { return sizeof(__FUNCTION__) == 5 ? 0 : 1; }\n",
         kSizeofValue, kSizeofType, false},
        {"`sizeof(__func__)` in a helper whose name is one character",
         "static int g(void) { return (int)sizeof(__func__); }\n"
         "int main(void) { return g() == 2 ? 0 : 1; }\n",
         kSizeofValue, kSizeofType, false},
        {"`_Alignof(__func__)` (the GNU expression operand gcc and clang accept)",
         "int main(void) { return _Alignof(__func__) == 1 ? 0 : 1; }\n",
         "alignofValue", "alignofType", false},
        // THE CONTROL the brace-less spelling already had right.
        {"`sizeof __func__` (no parentheses: an expression by the grammar alone)",
         "int main(void) { return sizeof __func__ == 5 ? 0 : 1; }\n",
         kSizeofValue, kSizeofType, false},
    });
}

// ── an enumeration constant is a VALUE of the scope its enum appears in ─────
TEST(SizeofValueNames, AnEnumerationConstantTakesTheExpressionReading) {
    expectReadings({
        {"an anonymous file-scope enum",
         "enum { A = 1 };\nint main(void) { return (int)sizeof(A); }\n",
         kSizeofValue, kSizeofType, false},
        {"a named file-scope enum",
         "enum E { B = 2 };\nint main(void) { return (int)sizeof(B); }\n",
         kSizeofValue, kSizeofType, false},
        {"a block-scope enum",
         "int main(void) { enum { C = 3 }; return (int)sizeof(C); }\n",
         kSizeofValue, kSizeofType, false},
        {"an enum declared with an object (`enum { D } v;` — past the declaration's "
         "own scope)",
         "enum { D = 4 } v;\nint main(void) { return (int)sizeof(D); }\n",
         kSizeofValue, kSizeofType, false},
        {"an enum nested in a struct body (C 6.2.1p7: the scope the struct "
         "appears in)",
         "struct S { enum { F = 5 } k; };\n"
         "int main(void) { return (int)sizeof(F); }\n",
         kSizeofValue, kSizeofType, false},
        {"a block-scope enumerator that shadows a file-scope typedef of the same "
         "name",
         "typedef long long U;\n"
         "int main(void) { enum { U = 1 }; return (int)sizeof(U); }\n",
         kSizeofValue, kSizeofType, false},
    });
}

// ── the controls: a TYPE stays a type, and an unknown name keeps the fallback ─
TEST(SizeofValueNames, ATypeNameAndAnUnknownNameStillTakeTheTypeReading) {
    expectReadings({
        {"a typedef name",
         "typedef char T5[5];\nint main(void) { return sizeof(T5) == 5 ? 0 : 1; }\n",
         kSizeofValue, kSizeofType, true},
        {"a block-scope typedef that shadows a file-scope enumerator of the same "
         "name",
         "enum { T = 1 };\n"
         "int main(void) { typedef long long T; return sizeof(T) == 8 ? 0 : 1; }\n",
         kSizeofValue, kSizeofType, true},
    });
    // An identifier NOTHING declares still reaches the Unknown arm and commits as
    // a type — the fallback a type the sketch cannot see relies on — and the
    // semantic tier refuses the name there.
    auto cu = buildShippedUnit(
        "c", {std::string{"int main(void) { return (int)sizeof(zzz); }\n"}});
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    EXPECT_EQ(countRule(*cu, kSizeofType), 1u)
        << "an undeclared name must still take the type-name reading";
    EXPECT_TRUE(hasCode(model.diagnostics(), DiagnosticCode::S_UnknownType))
        << "and the semantic tier must still refuse it as an unknown type";
}
