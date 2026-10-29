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
// (3) the HIR text (`.dsshir` v9) states each alias in its `aliases` section — target and own
//     linkage — and reads it back as the same alias; a section it cannot read is refused by name.
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
#include "hir/hir_text.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "repo_root.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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

// How many module definitions of STORAGE (`Global` nodes) are bound to a symbol named `name`.
[[nodiscard]] std::size_t storageNamed(SemanticModel const& m, CstToHirResult const& res, std::string_view name) {
    std::size_t n = 0;
    for (HirNodeId d : res.hir.moduleDecls(res.hir.root())) {
        if (res.hir.kind(d) != HirKind::Global) continue;
        if (nameOf(m, res.hir.globalSymbol(d)) == name) ++n;
    }
    return n;
}

// How many module REFERENCES of an object (`ExternGlobal` nodes) are bound to a symbol named `name`.
[[nodiscard]] std::size_t importsNamed(SemanticModel const& m, CstToHirResult const& res, std::string_view name) {
    std::size_t n = 0;
    for (HirNodeId d : res.hir.moduleDecls(res.hir.root())) {
        if (res.hir.kind(d) != HirKind::ExternGlobal) continue;
        if (nameOf(m, res.hir.externGlobalSymbol(d)) == name) ++n;
    }
    return n;
}

[[nodiscard]] std::size_t countKind(Hir const& h, HirKind k) {
    std::size_t         n   = 0;
    std::uint32_t const tag = h.id().v;
    for (std::uint32_t i = 1; i < h.nodeCount(); ++i)
        if (h.kind(HirNodeId{i, tag}) == k) ++n;
    return n;
}

// The `.dsshir` text of a lowered unit, as the CLI writes it (the source-level names, the literal and inline-asm
// pools, the aliases); "" with a failure when the writer reported an error.
[[nodiscard]] std::string textOf(SemanticModel const& m, CstToHirResult const& res) {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < m.symbols().size(); ++i) names.push_back(std::string{m.symbols()[i].name});
    HirTextContext ctx;
    ctx.interner      = &m.lattice().interner();
    ctx.symbolNames   = &names;
    ctx.literalPool   = &res.literalPool;
    ctx.inlineAsmPool = &res.inlineAsmPool;
    ctx.symbolAliases = &res.symbolAliases;
    DiagnosticReporter r;
    std::string text = emitHir(res.hir, ctx, r);
    std::string why;
    appendDiagnostics(why, r);
    EXPECT_EQ(r.errorCount(), 0u) << "the HIR text writer refused the unit:" << why;
    return r.errorCount() == 0 ? text : std::string{};
}

// The handle the text's `symbols` section gives the name `name`, or 0.
[[nodiscard]] std::uint32_t handleNamed(HirParseResult const& parsed, std::string_view name) {
    for (std::size_t i = 1; i < parsed.symbolNames.size(); ++i)
        if (parsed.symbolNames[i] == name) return static_cast<std::uint32_t>(i);
    return 0;
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
             // OBJECT aliases written `static` (lane `cs` probe rv cells 1-3, rv2 cells 2-4): module-private
             // when the alias is, and never storage of their own, whichever declaration of the name survived.
             Case{"static int t = 5;\nstatic int a __attribute__((alias(\"t\")));\nint use(void) { return a; }\n",
                  "a", "t", SymbolBinding::Local, WeakDefinitionKind{}},
             Case{"static int t = 5;\nextern int a __attribute__((alias(\"t\")));\nint use(void) { return a; }\n",
                  "a", "t", SymbolBinding::Global, WeakDefinitionKind{}},
             Case{"int t = 5;\nstatic int a __attribute__((alias(\"t\")));\nint use(void) { return a; }\n", "a",
                  "t", SymbolBinding::Local, WeakDefinitionKind{}},
             Case{"static int t = 5;\nstatic int a;\nstatic int a __attribute__((alias(\"t\")));\n"
                  "int use(void) { return a; }\n",
                  "a", "t", SymbolBinding::Local, WeakDefinitionKind{}},
             Case{"static int t = 5;\nstatic int a __attribute__((alias(\"t\")));\nstatic int a;\n"
                  "int use(void) { return a; }\n",
                  "a", "t", SymbolBinding::Local, WeakDefinitionKind{}},
             Case{"int t = 5;\nint a;\nextern int a __attribute__((alias(\"t\")));\nint use(void) { return a; }\n",
                  "a", "t", SymbolBinding::Global, WeakDefinitionKind{}},
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
            EXPECT_EQ(storageNamed(m, res, c.alias), 0u) << "the alias's name was given storage of its own";
            // A module-private alias has no name a reference could reach: its uses are its target's (the semantic
            // tier bound them), so it lowers to its alias row alone — an import of it is a name nothing defines.
            if (c.binding == SymbolBinding::Local)
                EXPECT_EQ(importsNamed(m, res, c.alias), 0u) << "a module-private alias was lowered as a reference to its name";
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

// cs-28 item 11 (`.dsshir` v9): every alias of a C unit — a weak function alias, a hidden one, a module-private
// object alias, an external object alias — travels in the text's `aliases` section with its target and its own
// linkage, and reads back as the same alias; the text written again from what was read is the same text. Before
// v9 the text had no form for an alias, and the unit read back without its second names.
TEST(HirSymbolAliasLowering, EachAliasTravelsInTheTextAndReadsBack) {
    char const* const src = "int g(void) { return 1; }\n"
                            "int f(void) __attribute__((weak, alias(\"g\")));\n"
                            "int h(void) __attribute__((alias(\"g\"), visibility(\"hidden\")));\n"
                            "static int t = 5;\n"
                            "static int a __attribute__((alias(\"t\")));\n"
                            "int v = 3;\n"
                            "extern int w __attribute__((alias(\"v\")));\n"
                            "int use(void) { return f() + h() + a + w; }\n";
    std::string const failed = lowerC(src, [&](SemanticModel const& m, CstToHirResult const& res, DiagnosticReporter const&) {
        ASSERT_EQ(res.symbolAliases.size(), 4u) << "the unit's four aliases were not lowered";
        std::string const text = textOf(m, res);
        ASSERT_FALSE(text.empty());
        EXPECT_NE(text.find("\naliases {\n"), std::string::npos) << "the text states no alias:\n" << text;
        DiagnosticReporter r;
        auto const back = parseHir(text, CompilationUnitId{1}, r);
        std::string why;
        appendDiagnostics(why, r);
        ASSERT_TRUE(back != nullptr && back->ok) << "the text did not read back:" << why << "\n" << text;
        ASSERT_EQ(back->symbolAliases.size(), res.symbolAliases.size()) << "an alias was lost on the way:\n" << text;
        for (std::size_t i = 0; i < res.symbolAliases.size(); ++i) {
            HirSymbolAlias const& was = res.symbolAliases[i];
            HirSymbolAlias const& is  = back->symbolAliases[i];
            std::string const     name = nameOf(m, was.alias);
            SCOPED_TRACE(name);
            ASSERT_LT(is.alias.v, back->symbolNames.size());
            ASSERT_LT(is.target.v, back->symbolNames.size());
            EXPECT_EQ(back->symbolNames[is.alias.v], name) << "the alias read back under another name";
            EXPECT_EQ(back->symbolNames[is.target.v], nameOf(m, was.target)) << "the alias names another target";
            EXPECT_EQ(is.linkage.binding, was.linkage.binding) << "the alias's own binding";
            EXPECT_EQ(is.linkage.visibility, was.linkage.visibility) << "the alias's own visibility";
            EXPECT_EQ(is.linkage.weakKind, was.linkage.weakKind) << "the alias's weak kind";
        }
        HirTextContext again;
        again.interner      = &back->interner;
        again.symbolNames   = &back->symbolNames;
        again.literalPool   = &back->literalPool;
        again.inlineAsmPool = &back->inlineAsmPool;
        again.symbolAliases = &back->symbolAliases;
        DiagnosticReporter r2;
        EXPECT_EQ(emitHir(back->hir, again, r2), text) << "the text written from what was read is another text";
        // The other weak kind (the review of fold f26a, F5): `select-any` is a word, a `-` and a word to the
        // reader's lexer. A C unit states no select-any alias, so the unit's own text is given one — its weak
        // alias's kind spelled `select-any` — and it must read back as that kind and write the same text again.
        std::string selectAny = text;
        auto const  weakAt    = selectAny.find("[weak(overridable)]");
        ASSERT_NE(weakAt, std::string::npos) << "the weak alias is not in the text:\n" << text;
        selectAny.replace(weakAt, std::string_view{"[weak(overridable)]"}.size(), "[weak(select-any)]");
        DiagnosticReporter r3;
        auto const sa = parseHir(selectAny, CompilationUnitId{1}, r3);
        std::string saWhy;
        appendDiagnostics(saWhy, r3);
        ASSERT_TRUE(sa != nullptr && sa->ok) << "a select-any alias did not read back:" << saWhy << "\n" << selectAny;
        bool readSelectAny = false;
        for (HirSymbolAlias const& a : sa->symbolAliases) {
            readSelectAny = readSelectAny || (a.linkage.binding == SymbolBinding::Weak
                                              && a.linkage.weakKind == WeakDefinitionKind::SelectAny);
        }
        EXPECT_TRUE(readSelectAny) << "`weak(select-any)` did not read as the select-any kind";
        HirTextContext saAgain;
        saAgain.interner      = &sa->interner;
        saAgain.symbolNames   = &sa->symbolNames;
        saAgain.literalPool   = &sa->literalPool;
        saAgain.inlineAsmPool = &sa->inlineAsmPool;
        saAgain.symbolAliases = &sa->symbolAliases;
        DiagnosticReporter r4;
        EXPECT_EQ(emitHir(sa->hir, saAgain, r4), selectAny) << "a select-any alias was written back as another text";
    });
    EXPECT_EQ(failed, "");
}

// The `aliases` section a reader cannot take is refused BY NAME — each case is the text of a real unit with its
// section replaced — and nothing may follow the last section (a v8 reader stopped at the module's `}`).
TEST(HirSymbolAliasLowering, AnAliasTheTextCannotStateIsRefusedByName) {
    char const* const src = "int g(void) { return 1; }\n"
                            "int f(void) __attribute__((alias(\"g\")));\n"
                            "int v = 3;\n"
                            "extern int w __attribute__((alias(\"v\")));\n"
                            "int use(void) { return f() + w; }\n";
    std::string const failed = lowerC(src, [&](SemanticModel const& m, CstToHirResult const& res, DiagnosticReporter const&) {
        std::string const text = textOf(m, res);
        auto const        at   = text.find("aliases {\n");
        ASSERT_NE(at, std::string::npos) << text;
        std::string const head = text.substr(0, at);   // the text up to its module's `}`, and its newline
        DiagnosticReporter r0;
        auto const control = parseHir(text, CompilationUnitId{1}, r0);
        ASSERT_TRUE(control != nullptr && control->ok) << "CONTROL: the unit's own text did not read back";
        std::string const g = std::format("%{}", handleNamed(*control, "g"));
        std::string const f = std::format("%{}", handleNamed(*control, "f"));
        std::string const w = std::format("%{}", handleNamed(*control, "w"));
        std::string const u = std::format("%{}", handleNamed(*control, "use"));
        ASSERT_NE(g, "%0");
        ASSERT_NE(f, "%0");
        ASSERT_NE(w, "%0");
        ASSERT_NE(u, "%0");
        struct Case {
            std::string    tail;
            DiagnosticCode code;
            char const*    says;
        };
        auto const section = [](std::string const& items) { return "aliases {\n" + items + "}\n"; };
        for (Case const& c : {
                 Case{section("  alias " + f + " [bogus] = " + g + "\n"), DiagnosticCode::H_TextUnknownName,
                      "unknown word 'bogus'"},
                 Case{section("  alias " + f + " [local, weak(overridable)] = " + g + "\n"),
                      DiagnosticCode::H_TextMalformed, "states its binding twice"},
                 Case{section("  alias " + f + " [hidden, protected] = " + g + "\n"), DiagnosticCode::H_TextMalformed,
                      "states its visibility twice"},
                 Case{section("  alias " + f + " [local(overridable)] = " + g + "\n"), DiagnosticCode::H_TextMalformed,
                      "takes no argument"},
                 Case{section("  alias " + f + " [weak(sometimes)] = " + g + "\n"), DiagnosticCode::H_TextUnknownName,
                      "unknown weak-definition kind 'sometimes'"},
                 Case{section("  alias " + f + " [weak] = " + g + "\n"), DiagnosticCode::H_TextMalformed,
                      "names no weak-definition kind"},
                 // The default binding and visibility are stated by leaving them out (the review of fold f26a, F5):
                 // spelled out, the alias would have two texts.
                 Case{section("  alias " + f + " [global] = " + g + "\n"), DiagnosticCode::H_TextMalformed,
                      "'global' in the attribute list of alias"},
                 Case{section("  alias " + f + " [default] = " + g + "\n"), DiagnosticCode::H_TextMalformed,
                      "'default' in the attribute list of alias"},
                 Case{section("  alias " + f + " [local, default] = " + g + "\n"), DiagnosticCode::H_TextMalformed,
                      "is the visibility an alias has when its list states none"},
                 Case{section("  alias " + f + " = " + f + "\n"), DiagnosticCode::H_TextMalformed, "names itself"},
                 Case{section("  alias " + f + " = " + g + "\n  alias " + f + " = " + g + "\n"),
                      DiagnosticCode::H_TextMalformed, "is stated twice"},
                 Case{section("  alias %999 = " + g + "\n"), DiagnosticCode::H_TextUnknownName, "not declared"},
                 Case{section("  alias " + w + " = " + f + "\n"), DiagnosticCode::H_TextMalformed,
                      "which no `function` or `global` of the module defines"},
                 Case{section("  alias " + u + " = " + g + "\n"), DiagnosticCode::H_TextMalformed,
                      "is also defined by a `function` or `global`"},
                 Case{section("  name " + f + " = " + g + "\n"), DiagnosticCode::H_TextMalformed, "expected 'alias'"},
                 Case{section("  alias " + f + " = " + g + "\n") + "stray\n", DiagnosticCode::H_TextMalformed,
                      "after the module"},
                 Case{std::string{"stray\n"}, DiagnosticCode::H_TextMalformed, "after the module"},
             }) {
            SCOPED_TRACE(c.tail);
            DiagnosticReporter r;
            auto const read = parseHir(head + c.tail, CompilationUnitId{1}, r);
            EXPECT_FALSE(read != nullptr && read->ok) << "the reader took a section it cannot read";
            bool said = false;
            for (auto const& d : r.all()) said = said || (d.code == c.code && d.actual.find(c.says) != std::string::npos);
            std::string why;
            appendDiagnostics(why, r);
            EXPECT_TRUE(said) << "not refused by name (" << diagnosticCodeName(c.code) << ", \"" << c.says << "\"):"
                              << why;
        }
        // The words an unknown word is told a list accepts are the words a list states — never the two defaults.
        DiagnosticReporter r;
        (void)parseHir(head + section("  alias " + f + " [bogus] = " + g + "\n"), CompilationUnitId{1}, r);
        bool listed = false;
        for (auto const& d : r.all()) {
            if (d.code != DiagnosticCode::H_TextUnknownName) continue;
            listed = true;
            EXPECT_EQ(d.actual.find("'global'"), std::string::npos) << "the accepted words name 'global': " << d.actual;
            EXPECT_EQ(d.actual.find("'default'"), std::string::npos) << "the accepted words name 'default': " << d.actual;
            EXPECT_NE(d.actual.find("'weak'"), std::string::npos) << d.actual;
        }
        EXPECT_TRUE(listed) << "the unknown word was not refused by name";
    });
    EXPECT_EQ(failed, "");
}
