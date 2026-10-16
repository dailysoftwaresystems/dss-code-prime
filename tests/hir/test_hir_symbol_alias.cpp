// P69 (lane `cs`) — A SECOND NAME OF A DEFINITION, AND AN ARRAY OF UNKNOWN SIZE, AS THE HIR TIER
// HANDS THEM ON.
//
// (1) `alias("target")`: the semantic tier resolves each alias to the definition it ends at
//     (`SymbolRecord::aliasTarget`); the lowering states it as a fact of the module
//     (`CstToHirResult::symbolAliases`: the alias, its target, the alias's OWN linkage), and lowers
//     the alias's declaration as a reference to its name, never as a body or storage of its own.
//     A `static` alias is module-private; a weak alias carries the weak kind C's `weak` states.
// (2) `sizeof` of an array of unknown size is the language document's statement
//     (`sizeof.incompleteArraySize`, cl's 0, warned by the semantic tier): the lowering folds it in
//     a constant initializer and lowers it to that literal in a function body — never a `SizeOf`
//     node, whose type has no size.
//
// ⚠ MUST run through ctest: the shipped documents are found through `DSS_CONFIG_ROOT`, which only
// `dss_add_test` sets.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "repo_root.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

using namespace dss;

namespace {

void appendDiagnostics(std::string& out, DiagnosticReporter const& r) {
    for (auto const& d : r.all()) out += std::string{"\n  "} + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
}

[[nodiscard]] TargetSchema const* fixtureTarget() {
    static std::shared_ptr<TargetSchema const> const kTarget = [] {
        auto t = TargetSchema::loadShipped("x86_64");
        return t.has_value() ? *t : nullptr;
    }();
    return kTarget.get();
}

using Inspect = std::function<void(SemanticModel const&, CstToHirResult const&, DiagnosticReporter const&)>;

// c source → semantic model → HIR, analyzed with the shipped x86_64 target and an ELF format in
// scope (what the CLI gives an ELF build). Returns every diagnostic of both tiers when the unit does
// not lower; `inspect` reads the lowered module while its model is alive.
[[nodiscard]] std::string lowerC(std::string src, Inspect const& inspect) {
    auto const loaded = dss::test_support::shippedSchemaOrThrow("c");
    UnitBuilder builder{loaded, DiagnosticBudget::libraryDefault()};
    builder.setActiveFormat(ObjectFormatKind::Elf);
    builder.addInMemory(std::move(src), "<mem>");
    auto        cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    std::string out;
    TargetSchema const* const target = fixtureTarget();
    if (target == nullptr) throw std::runtime_error("the shipped x86_64 target did not load");
    std::optional<AggregateLayoutParams> layout;
    if (target->aggregateLayoutLoaded()) layout = target->aggregateLayout();
    std::optional<VaListStrategy> vaList;
    if (auto const* cc = target->callingConventionByName("sysv_amd64"); cc != nullptr && cc->vaListLayout.has_value())
        vaList = cc->vaListLayout->strategy;
    SemanticModel model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, layout, vaList,
                                  SelectableObjectFormatKind::of(ObjectFormatKind::Elf), target->name(),
                                  LongDoubleFormat::None, target);
    if (model.hasErrors()) {
        appendDiagnostics(out, model.diagnostics());
        return "the unit was refused by the semantic tier:" + out;
    }
    DiagnosticReporter r;
    auto               res = lowerToHir(model, r);
    if (res == nullptr || !res->ok) {
        appendDiagnostics(out, r);
        return "the unit did not lower:" + out;
    }
    inspect(model, *res, r);
    return {};
}

[[nodiscard]] std::string nameOf(SemanticModel const& m, SymbolId s) {
    SymbolRecord const* r = m.recordFor(s);
    return r == nullptr ? std::string{"(no record)"} : std::string{r->name};
}

// The one alias of the module named `alias`, or nullptr.
[[nodiscard]] HirSymbolAlias const* aliasNamed(SemanticModel const& m, CstToHirResult const& res,
                                               std::string_view alias) {
    HirSymbolAlias const* found = nullptr;
    for (HirSymbolAlias const& a : res.symbolAliases) {
        if (nameOf(m, a.alias) != alias) continue;
        if (found != nullptr) return nullptr;   // stated twice: no one alias
        found = &a;
    }
    return found;
}

// The value of a lowered integer literal, signed or unsigned, or nullopt for any other node.
[[nodiscard]] std::optional<std::uint64_t> literalValue(CstToHirResult const& res, HirNodeId n) {
    if (!n.valid() || res.hir.kind(n) != HirKind::Literal) return std::nullopt;
    std::uint32_t const at = res.hir.payload(n);
    if (at >= res.literalPool.size()) return std::nullopt;
    if (auto const* v = std::get_if<std::uint64_t>(&res.literalPool.at(at).value)) return *v;
    if (auto const* v = std::get_if<std::int64_t>(&res.literalPool.at(at).value)) return static_cast<std::uint64_t>(*v);
    return std::nullopt;
}

[[nodiscard]] std::size_t countKind(Hir const& h, HirKind k) {
    std::size_t         n   = 0;
    std::uint32_t const tag = h.id().v;
    for (std::uint32_t i = 1; i < h.nodeCount(); ++i)
        if (h.kind(HirNodeId{i, tag}) == k) ++n;
    return n;
}

}  // namespace

TEST(HirSymbolAliasLowering, EachAliasIsStatedOnceWithItsTargetAndItsOwnLinkage) {
    struct Case {
        char const*        src;
        char const*        alias;
        char const*        target;
        SymbolBinding      binding;
        WeakDefinitionKind kind;
    };
    for (Case const c : {
             Case{"int g(void) { return 1; }\nint f(void) __attribute__((alias(\"g\")));\n"
                  "int use(void) { return f(); }\n",
                  "f", "g", SymbolBinding::Global, WeakDefinitionKind{}},
             Case{"int g(void) { return 1; }\nint f(void) __attribute__((weak, alias(\"g\")));\n"
                  "int use(void) { return f(); }\n",
                  "f", "g", SymbolBinding::Weak, WeakDefinitionKind::Overridable},
             Case{"static int g(void) { return 1; }\nstatic int f(void) __attribute__((alias(\"g\")));\n"
                  "int use(void) { return f(); }\n",
                  "f", "g", SymbolBinding::Local, WeakDefinitionKind{}},
             Case{"int v = 3;\nextern int w __attribute__((alias(\"v\")));\nint use(void) { return w; }\n", "w",
                  "v", SymbolBinding::Global, WeakDefinitionKind{}},
             Case{"int t;\nextern int w __attribute__((alias(\"t\")));\nint use(void) { return w; }\n", "w", "t",
                  SymbolBinding::Global, WeakDefinitionKind{}},
         }) {
        SCOPED_TRACE(c.src);
        std::string const failed = lowerC(c.src, [&](SemanticModel const& m, CstToHirResult const& res, DiagnosticReporter const&) {
            EXPECT_EQ(res.symbolAliases.size(), 1u) << "one alias, stated once";
            HirSymbolAlias const* a = aliasNamed(m, res, c.alias);
            ASSERT_NE(a, nullptr) << "the lowering states no alias of that name";
            EXPECT_EQ(nameOf(m, a->target), c.target);
            EXPECT_EQ(a->linkage.binding, c.binding) << "the alias's own binding";
            if (c.binding == SymbolBinding::Weak) EXPECT_EQ(a->linkage.weakKind, c.kind);
            EXPECT_FALSE(a->linkage.tentative) << "an alias has no storage, so it is never a common";
        });
        EXPECT_EQ(failed, "");
    }
    // CONTROL: a unit with no alias states none.
    std::string const failed = lowerC("int g(void) { return 1; }\nint f(void);\n",
                                      [&](SemanticModel const&, CstToHirResult const& res, DiagnosticReporter const&) {
                                          EXPECT_TRUE(res.symbolAliases.empty());
                                      });
    EXPECT_EQ(failed, "");
}

TEST(HirSymbolAliasLowering, SizeofAnArrayOfUnknownSizeIsTheLanguagesStatedValue) {
    // In a function body and in a constant initializer: the literal the language states, never a `SizeOf`
    // of a type that has no size.
    std::string failed;
    for (char const* src : {"extern int a[];\nunsigned long f(void) { return sizeof a; }\n",
                            "extern int a[];\nunsigned long n = sizeof a + 5;\n",
                            // Written before a later declaration completes the array: still of unknown size
                            // there — cl gives 0, not the completed 12 (✔MEASURED, lane `cs` probe sz cell 2).
                            "extern int a[];\nunsigned long f(void) { return sizeof a; }\nint a[3];\n"}) {
        SCOPED_TRACE(src);
        failed = lowerC(src, [&](SemanticModel const&, CstToHirResult const& res, DiagnosticReporter const&) {
            EXPECT_EQ(countKind(res.hir, HirKind::SizeOf), 0u)
                << "sizeof of an array of unknown size lowered to a SizeOf node";
            std::size_t zeros = 0;
            std::size_t twelves = 0;
            std::uint32_t const tag = res.hir.id().v;
            for (std::uint32_t i = 1; i < res.hir.nodeCount(); ++i) {
                auto const v = literalValue(res, HirNodeId{i, tag});
                zeros += v == std::optional<std::uint64_t>{0} ? 1u : 0u;
                twelves += v == std::optional<std::uint64_t>{12} ? 1u : 0u;
            }
            if (std::string_view{src}.find("return") != std::string_view::npos)
                EXPECT_GE(zeros, 1u) << "the body returns the literal the language states for the size";
            EXPECT_EQ(twelves, 0u) << "the size of the array as completed LATER was read where it is of unknown size";
        });
        EXPECT_EQ(failed, "") << "sizeof of an array of unknown size did not lower";
    }
    // As a constant THIS tier folds (its evaluator's layout hook): `if (sizeof a == 0) return 42;` is a
    // constant condition, so the function's end is not reached and nothing is warned; the CONTROL with a
    // sized operand is a constant false condition, so its end is reached and warned once.
    for (auto const& [src, endWarnings] :
         {std::pair<char const*, std::size_t>{"extern int a[];\nint f(void) { if (sizeof a == 0) return 42; }\n", 0},
          std::pair<char const*, std::size_t>{"extern int a[4];\nint f(void) { if (sizeof a == 0) return 42; }\n", 1}}) {
        SCOPED_TRACE(src);
        failed = lowerC(src, [&](SemanticModel const&, CstToHirResult const&, DiagnosticReporter const& r) {
            std::size_t n = 0;
            for (auto const& d : r.all()) n += d.code == DiagnosticCode::H_NonVoidFunctionEndReachable ? 1u : 0u;
            EXPECT_EQ(n, endWarnings) << "the lowering's own constant evaluator folds sizeof of an array of unknown "
                                         "size to the language's value";
        });
        EXPECT_EQ(failed, "");
    }
}
