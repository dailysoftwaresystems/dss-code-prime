// ═══════════════════════════════════════════════════════════════════════════
//  EVERY "IS A WRITTEN BEFORE B" QUESTION READS THE TOKEN STREAM
//  cycle P69 round 1, fold f26c, lane `cs` (main's ledger 474 (2))
//
//  The C front end asks one kind of question at many places: is this
//  declaration, definition, initializer or clause written BEFORE that use? The
//  answer is the token-stream order — `streamRank`, the pre-order rank of a node
//  in its tree (semantic_analyzer.cpp) — and never a byte offset: a token the
//  preprocessor MATERIALIZES (the product of a paste, `##`) lives in the synth
//  buffer's product tail, past every source byte (preprocessor.cpp
//  `materializeSignificant`, READ), so a byte offset ordered it after
//  everything written later.
// ═══════════════════════════════════════════════════════════════════════════
//
// ── THE MEASUREMENT (lane `cs`'s probes fp3–fp6, 2026-11-04) ─────────────────
//  DSS (the f26c compiler before this fix, and the one of 2026-10-28 alike):
//    * a struct whose TAG is pasted was INCOMPLETE as the parameter and the
//      return type of a later definition, and as a later call's argument —
//      S_IncompleteTypeObject / S_IncompleteReturnType (fp3, fp4, fp6 cells 2, 4);
//    * a declaration whose NAME is pasted, completing `extern int arr1[]`, was not
//      seen by a later `sizeof`: 0 with a warning, the program's value WRONG
//      (fp5, fp6 cell 6);
//    * a const object whose INITIALIZER is a pasted number was refused at a later
//      static initializer, "whose initializer is not visible here" (fp6 cell 8).
//    The same programs written without the paste, or through a macro that pastes
//    nothing, compiled and ran 42 (fp4 cells 1-4, fp6 odd cells).
//  THE REFERENCES (probe fp6, every cell RUN, -std=c17 / /std:c11): gcc 13.3.0
//    and clang 18.1.3 (Linux x86_64), mingw-w64 gcc 13.2.0, Apple clang 21
//    (arm64): every cell 42, the pasted alike the plain. cl 19.51: the same
//    answers, except that it refuses a const object read in a static initializer
//    plain and pasted alike (an extension cl does not have). A paste yields ONE
//    preprocessing token (C 6.10.3.3p3): the translation phases after 4 see the
//    same program, which is why no reference can tell the two apart.
//
// ── THE UNIVERSE: every order question the front end asks (the class's site
//    list, searched by data — every `span(…).start()` / `.end()` in a comparison
//    and every position stored as a byte offset, over src/analysis/{semantic,
//    syntactic}, src/hir, src/core/types) × {plain, pasted} × the role the
//    materialized token plays (the declaration, the definition, the reference).
//    Each cell asserts the ANSWER — no diagnostic where every reference compiles
//    it, the value a `_Static_assert` reads, the alignment that stands, the
//    survivor's mark, the order the diagnostic names — never merely "no error".

#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

constexpr AggregateLayoutParams kLayout{
    .scalarAlignment       = ScalarAlignmentRule::Natural,
    .maxAlignment          = 16,
    .maxRequestedAlignment = 268435456};

// The analysis as the pipeline runs it for one object format.
[[nodiscard]] SemanticModel analyzeOn(ObjectFormatKind format, std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kLayout, std::nullopt,
                   std::optional<SelectableObjectFormatKind>{SelectableObjectFormatKind::of(format)});
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

// A name as the form spells it: plain, or the product of a paste of its two halves.
enum class Form { Plain, Pasted };

[[nodiscard]] std::string spelled(Form f, std::string_view left, std::string_view right) {
    if (f == Form::Plain) return std::string{left} + std::string{right};
    return "CAT(" + std::string{left} + ", " + std::string{right} + ")";
}

[[nodiscard]] char const* formName(Form f) { return f == Form::Plain ? "plain" : "pasted"; }

// The record of the object `name` no other absorbed (the one the lowering reads).
[[nodiscard]] SymbolRecord const* survivorOf(SemanticModel const& m, std::string_view name) {
    SymbolRecord const* survivor = nullptr;
    for (std::size_t i = 1; i < m.symbols().size(); ++i) {
        SymbolRecord const& r = m.symbols()[i];
        if (r.name != name || r.kind != DeclarationKind::Variable || r.isAbsorbedProto) continue;
        if (survivor != nullptr) return nullptr;   // not exactly one
        survivor = &r;
    }
    return survivor;
}

[[nodiscard]] SymbolRecord const* firstNamed(SemanticModel const& m, std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return &m.symbols()[i];
    return nullptr;
}

struct Cell {
    std::string      question;   // the order question, and where the materialized token stands
    ObjectFormatKind format;
    // The cell's source after `#define CAT(a, b) a##b`, and what must hold of its analysis.
    std::function<std::string(Form)>                  source;
    std::function<void(SemanticModel const&, Form)>   holds;
};

void noError(SemanticModel const& m, Form) {
    EXPECT_FALSE(m.hasErrors()) << "every reference compiles it, the pasted form alike the plain:"
                                << everyDiagnostic(m);
}

[[nodiscard]] std::vector<Cell> cells() {
    ObjectFormatKind const elf = ObjectFormatKind::Elf;
    ObjectFormatKind const pe  = ObjectFormatKind::Pe;
    std::vector<Cell> out;
    // ── A COMPOSITE IS COMPLETE AFTER ITS `}` (C 6.7.2.1p8) — `compositeIncompleteAt`.
    auto const s1 = [](Form f) { return "struct " + spelled(f, "S", "1") + " { int v; };\n"; };
    out.push_back({"a composite completed before a definition's parameter (the tag)", elf,
                   [=](Form f) { return s1(f) + "int get(struct S1 s) { return s.v; }\n"; }, noError});
    out.push_back({"a composite completed before a call's argument (the tag)", elf,
                   [=](Form f) {
                       return s1(f) + "int get(struct S1 s);\n"
                                      "int use(void) { struct S1 x = {42}; return get(x); }\n";
                   },
                   noError});
    out.push_back({"a composite completed before a declaration's parameter (the tag; a declaration asks nothing)",
                   elf, [=](Form f) { return s1(f) + "int get(struct S1 s);\n"; }, noError});
    out.push_back({"a composite completed before a definition's result (the tag)", elf,
                   [=](Form f) { return s1(f) + "struct S1 mk(void) { struct S1 r = {42}; return r; }\n"; },
                   noError});
    out.push_back({"a composite completed before a call's result (the tag)", elf,
                   [=](Form f) { return s1(f) + "struct S1 mk(void);\nint use(void) { return mk().v; }\n"; },
                   noError});
    // ── THE COMPOSITE TYPE VISIBLE AT A USE (C 6.2.7p4) — `ownDeclaredRank`, `typeVisibleAt`.
    auto const arr = [](char const* incomplete, char const* complete, char const* use) {
        return std::string{"extern int "} + incomplete + "[];\nint " + complete + "[3];\n"
               "_Static_assert(sizeof " + use + " == 3 * sizeof(int), \"the composite of both\");\n";
    };
    out.push_back({"an array completed by a later declaration (that declaration's name)", elf,
                   [=](Form f) { return arr("arr1", spelled(f, "arr", "1").c_str(), "arr1"); }, noError});
    out.push_back({"an array completed by a later declaration (the first declaration's name)", elf,
                   [=](Form f) { return arr(spelled(f, "arr", "1").c_str(), "arr1", "arr1"); }, noError});
    out.push_back({"an array completed by a later declaration (the use's name)", elf,
                   [=](Form f) { return arr("arr1", "arr1", spelled(f, "arr", "1").c_str()); }, noError});
    // ── A CONST OBJECT'S INITIALIZER IS VISIBLE AT A LATER READ — the static initializer's walk.
    auto const kread = [](std::string const& name, std::string const& init, std::string const& read) {
        return "static const int " + name + " = " + init + ";\nstatic int g = " + read + ";\n"
               "_Static_assert(sizeof g == sizeof(int), \"g\");\n";
    };
    out.push_back({"a const object's initializer read later (the initializer)", elf,
                   [=](Form f) { return kread("k1", spelled(f, "4", "2"), "k1"); }, noError});
    out.push_back({"a const object's initializer read later (the object's name)", elf,
                   [=](Form f) { return kread(spelled(f, "k", "1"), "42", "k1"); }, noError});
    out.push_back({"a const object's initializer read later (the read's name)", elf,
                   [=](Form f) { return kread("k1", "42", spelled(f, "k", "1")); }, noError});
    // ── OF TWO `__declspec(align)` ON ONE DECLARATION THE LATER STANDS (cl) — `foldAlignmentRequest`.
    for (bool const pastedFirst : {true, false}) {
        out.push_back({std::string{"the later of two repeated alignment requests stands (the "}
                           + (pastedFirst ? "first" : "second") + " clause's name)",
                       pe,
                       [=](Form f) {
                           std::string const named = "__declspec(" + spelled(f, "al", "ign") + "(32))";
                           return pastedFirst ? named + " __declspec(align(16)) int a;\n"
                                              : "__declspec(align(16)) " + named + " int a;\n";
                       },
                       [=](SemanticModel const& m, Form) {
                           SymbolRecord const* a = firstNamed(m, "a");
                           ASSERT_NE(a, nullptr);
                           EXPECT_EQ(a->explicitAlignment, std::optional<std::uint32_t>{pastedFirst ? 16u : 32u})
                               << "the clause written LATER stands" << everyDiagnostic(m);
                       }});
    }
    // ── A LEADING `__declspec(align)` BEFORE A DEFINITION ALIGNS THE TYPE (cl) — the leading requests.
    out.push_back({"a leading alignment request written before a struct's definition (the request's keyword)", pe,
                   [=](Form f) {
                       return spelled(f, "__decl", "spec")
                              + "(align(32)) struct S { int a; } s;\n"
                                "_Static_assert(_Alignof(struct S) == 32, \"the TYPE is aligned\");\n";
                   },
                   noError});
    // ── THE LAST TENTATIVE DECLARATION'S `nocommon` DECIDES (gcc's reading) — settleNoCommon.
    for (bool const markedFirst : {true, false}) {
        out.push_back({std::string{"the last tentative declaration's mark decides (the "}
                           + (markedFirst ? "first" : "second") + " declaration's name)",
                       elf,
                       [=](Form f) {
                           std::string const marked = "int " + spelled(f, "t", "1") + " __attribute__((nocommon));\n";
                           return markedFirst ? marked + "int t1;\n" : "int t1;\n" + marked;
                       },
                       [=](SemanticModel const& m, Form) {
                           SymbolRecord const* t = survivorOf(m, "t1");
                           ASSERT_NE(t, nullptr) << "the object 't1' has no single survivor";
                           EXPECT_EQ(t->tentativeNeverCommon, !markedFirst)
                               << "the LAST tentative declaration's mark" << everyDiagnostic(m);
                       }});
    }
    // ── TWO STORAGE CLASSES ARE NAMED IN THE ORDER WRITTEN (C 6.7.1p2) — the specifier walk.
    out.push_back({"two storage-class specifiers are named in the order written (the first one)", elf,
                   [=](Form f) { return spelled(f, "ext", "ern") + " static int x;\n"; },
                   [=](SemanticModel const& m, Form) {
                       std::size_t said = 0;
                       for (auto const& d : m.diagnostics().all())
                           if (d.code == DiagnosticCode::S_ConflictingStorageClassSpecifiers
                               && d.actual.find("'extern' and 'static'") != std::string::npos)
                               ++said;
                       EXPECT_EQ(said, 1u) << "refused once, naming `extern` first" << everyDiagnostic(m);
                   }});
    return out;
}

}  // namespace

TEST(OrderReadsTheTokenStream, EveryOrderQuestionAnswersAlikePlainAndThroughAPastedToken) {
    for (Cell const& c : cells()) {
        for (Form const f : {Form::Plain, Form::Pasted}) {
            std::string const src = "#define CAT(a, b) a##b\n" + c.source(f);
            SCOPED_TRACE(c.question + ", " + formName(f) + ":\n" + src);
            auto const m = analyzeOn(c.format, src);
            c.holds(m, f);
        }
    }
}
