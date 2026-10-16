// ══ A `__try` FILTER IS LEFT ONLY BY ITS VALUE, AND GUARDS NOTHING ══════════════════
// (cycle P69 round 1, lane `lm`; a review of checkpoint 21, its N3.)
//
// A filter is an expression, and a GNU statement expression lets it hold statements.
// The filter is compiled as a function of its own, called by the dispatcher while it
// searches for a handler, with no scope table of its own — so a statement that leaves
// the filter otherwise than by its value (a `return`, a `goto` to a label outside it, a
// `break` or `continue` of a loop around it) and a `__try` inside it have nothing to be
// compiled to. They were refused by the MIR verifier, as `I_SehStructure` naming MIR
// block numbers (✔MEASURED 2026-10-15, pe64, both pipelines, all five shapes). They are
// refused by the HIR verifier now, by the construct the source wrote: a jump out as
// `H_SehEarlyExit` — the guarded body's own rule, a second way out — and a region inside
// as `H_SehRegionInsideAFilter`.
//
// The references: cl has no statement expression; clang 18 for x86_64-pc-windows-msvc
// COMPILES the `return` and the `__try` and its frontend CRASHES (exit 139) on the other
// three, and nothing of its image can be run here — a compile, not a working reference.
// What stays inside the filter — a loop broken inside it, a `goto` to a label inside it, a
// `switch`, a trap — is accepted, and is the funclet's (`mir/test_seh_filter_with_a_branch`).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

using namespace dss;
using namespace dss::sem_test;

namespace {

struct Lowered {
    bool        analyzed = false;   // the semantic tier accepted the source
    bool        ok       = false;   // the lowering (and its verifier) accepted it
    std::size_t earlyExits = 0, regionsInsideAFilter = 0, structure = 0, errors = 0;
    std::string words;
};

[[nodiscard]] Lowered lowerC(std::string const& src) {
    auto cu    = buildShippedUnit("c", {src});
    auto model = analyze(cu, DiagnosticBudget::libraryDefault());
    Lowered out;
    out.analyzed = !model.hasErrors();
    for (auto const& d : model.diagnostics().all()) out.words += "\n    " + d.actual;
    if (!out.analyzed) return out;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    out.ok                   = res != nullptr && res->ok;
    out.earlyExits           = countCode(r, DiagnosticCode::H_SehEarlyExit);
    out.regionsInsideAFilter = countCode(r, DiagnosticCode::H_SehRegionInsideAFilter);
    out.structure            = countCode(r, DiagnosticCode::I_SehStructure);
    out.errors               = r.errorCount();
    for (auto const& d : r.all()) out.words += "\n    " + d.actual;
    return out;
}

constexpr char const* kPrelude =
    "int g;\n"
    "void touch(volatile int *p) { *p = 1; }\n";

} // namespace

// Each way out of a filter is refused once, by its own construct, in words that name the
// filter — and by nothing else.
TEST(SehFilterStatements, AJumpOutOfAFilterIsRefusedByItsConstruct) {
    struct Shape {
        char const* what;
        char const* body;
        char const* words;
    };
    constexpr Shape kShapes[] = {
        {"return",
         "int f(volatile int *p) {\n    __try { touch(p); }\n"
         "    __except (({ if (g) return 3; 1; })) { return 2; }\n    return 0;\n}\n",
         "'return' inside a __except filter expression"},
        {"goto out",
         "int f(volatile int *p) {\n    __try { touch(p); }\n"
         "    __except (({ if (g) goto out; 1; })) { return 2; }\n    return 0;\nout:\n    return 3;\n}\n",
         "goto out of a __except filter expression"},
        {"break out",
         "int f(volatile int *p) {\n    for (;;) {\n        __try { touch(p); }\n"
         "        __except (({ if (g) break; 1; })) { return 2; }\n        return 0;\n    }\n"
         "    return 3;\n}\n",
         "'break' leaving a __except filter expression"},
        {"continue out",
         "int f(volatile int *p) {\n    for (int i = 0; i < 2; ++i) {\n        __try { touch(p); }\n"
         "        __except (({ if (g) continue; 1; })) { return 2; }\n    }\n    return 0;\n}\n",
         "'continue' leaving a __except filter expression"},
    };
    for (Shape const& s : kShapes) {
        SCOPED_TRACE(s.what);
        Lowered const L = lowerC(std::string{kPrelude} + s.body);
        ASSERT_TRUE(L.analyzed) << L.words;
        EXPECT_FALSE(L.ok);
        EXPECT_EQ(L.earlyExits, 1u) << L.words;
        EXPECT_EQ(L.errors, 1u) << L.words;
        EXPECT_EQ(L.structure, 0u) << "refused by the construct, not by MIR block numbers";
        EXPECT_NE(L.words.find(s.words), std::string::npos) << L.words;
    }
}

// A `__try` inside a filter is refused once, by its own code.
TEST(SehFilterStatements, ARegionInsideAFilterIsRefusedByItsConstruct) {
    Lowered const L = lowerC(std::string{kPrelude} +
        "int f(volatile int *p) {\n    __try { touch(p); }\n"
        "    __except (({ int r; __try { r = g; } __except (1) { r = 0; } r; })) { return 2; }\n"
        "    return 0;\n}\n");
    ASSERT_TRUE(L.analyzed) << L.words;
    EXPECT_FALSE(L.ok);
    EXPECT_EQ(L.regionsInsideAFilter, 1u) << L.words;
    EXPECT_EQ(L.errors, 1u) << L.words;
    EXPECT_NE(L.words.find("a __try statement inside a __except filter expression"),
              std::string::npos) << L.words;
}

// THE CONTROLS: what stays inside the filter is accepted — a loop broken and continued
// inside it, a `goto` to a label inside it, a `switch`, a trap; and a `__try` inside the
// HANDLER, and one inside the guarded body, are no region inside a filter.
TEST(SehFilterStatements, WhatStaysInsideTheFilterIsAccepted) {
    constexpr char const* kAccepted[] = {
        "int f(volatile int *p) {\n    __try { touch(p); }\n"
        "    __except (({ int r = 0; for (int i = 0; i < 3; ++i) { if (i == g) continue; "
        "r = 1; break; } r; })) { return 2; }\n    return 0;\n}\n",
        "int f(volatile int *p) {\n    __try { touch(p); }\n"
        "    __except (({ int r = 0; if (g) goto in; r = 1; in: r = r + 1; r; })) { return 2; }\n"
        "    return 0;\n}\n",
        "int f(volatile int *p) {\n    __try { touch(p); }\n"
        "    __except (({ int r; switch (_exception_code()) { case 5: r = 1; break; "
        "default: r = 0; } r; })) { return 2; }\n    return 0;\n}\n",
        "int f(volatile int *p) {\n    __try { touch(p); }\n"
        "    __except (({ if (g == 7) __builtin_trap(); 1; })) { return 2; }\n    return 0;\n}\n",
        "int f(volatile int *p) {\n    int r = 0;\n    __try { touch(p); }\n"
        "    __except (1) { __try { r = g; } __except (1) { r = 1; } }\n    return r;\n}\n",
        "int f(volatile int *p) {\n    int r = 0;\n    __try { __try { touch(p); } __except (1) { r = 1; } }\n"
        "    __except (1) { r = 2; }\n    return r;\n}\n",
    };
    for (char const* body : kAccepted) {
        SCOPED_TRACE(body);
        Lowered const L = lowerC(std::string{kPrelude} + body);
        ASSERT_TRUE(L.analyzed) << L.words;
        EXPECT_TRUE(L.ok) << L.words;
        EXPECT_EQ(L.earlyExits + L.regionsInsideAFilter, 0u) << L.words;
    }
}
