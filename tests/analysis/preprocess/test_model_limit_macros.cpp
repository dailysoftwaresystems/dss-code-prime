// `__BITINT_MAXWIDTH__` — a predefined macro whose value is a LIMIT OF DSS's OWN
// MODEL, named in the language document and written by the loader.
//
// ═══ WHY THIS FILE EXISTS (P68 round 8, D-C-BITINT-MAXWIDTH-MACRO-RESTATES-THE-MODEL-BOUND) ═
//
// `c.lang.json` stated `"value": "8388608"` while `kBitIntMaxWidth`
// (`core/types/bit_int_value.hpp`) enforced the same number at four tiers: the
// `_BitInt(N)` specifier, the wb/uwb literal typing, both text tiers and the
// static-data encoder. Two owners of one fact, agreeing only because nobody had
// moved either — and the macro is exactly what a program reads to decide how
// wide a `_BitInt` it may declare. The row now NAMES the limit (`kind:
// model-limit`, `limit: bitIntMaxWidth`) and the loader writes the value.
//
// ✔MEASURED 2026-09-23 (`-dM -E -std=c2x`): clang 18.1.3 defines 8388608 for
// x86_64 and 128 for aarch64 (its aarch64 backend refuses `_BitInt(4096)`);
// gcc 13.3.0 defines none. DSS states its OWN bound on every target — its
// multi-limb `_BitInt` is the same on every one.
//
// Pins:
//   (1) the shipped document names the limit and states NO number, and the
//       loaded macro's value is the model's constant;
//   (2) the macro and the width check read ONE number: `_BitInt` of exactly
//       `__BITINT_MAXWIDTH__` is accepted and one wider is refused, through the
//       driver's own preprocess + analysis halves;
//   (3) the loader refuses an unknown limit, a `value` beside the limit, a
//       missing limit, and a `limit` on any other kind.
// RED-ON-DISABLE (✔ transcripts in the row): move the bound (`kBitIntMaxWidth`)
// and (1)(2) stay GREEN — the macro moved with it; state the old number in the
// document and (1) reds; do both and (2) reds too, because a literal cannot move.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/target_format_analysis.hpp"
#include "core/types/bit_int_value.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/preprocess_config.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

constexpr std::string_view kMacro = "__BITINT_MAXWIDTH__";

[[nodiscard]] std::string shippedCText() {
    std::ifstream in(dss::test::configRoot() / "sources" / "c.lang.json",
                     std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The shipped text with `from` → `to` at the first `from` after `anchor`, or
// empty (with an ADD_FAILURE) when either is gone — so an edit that finds
// nothing is LOUD, never a vacuous pass over a document that changed shape.
[[nodiscard]] std::string shippedCEdited(std::string_view anchor,
                                         std::string_view from,
                                         std::string_view to) {
    std::string text = shippedCText();
    auto const at = text.find(anchor);
    if (at == std::string::npos) {
        ADD_FAILURE() << "the shipped c config no longer carries " << anchor;
        return {};
    }
    auto const pos = text.find(from, at);
    if (pos == std::string::npos) {
        ADD_FAILURE() << "no '" << from << "' after " << anchor;
        return {};
    }
    text.replace(pos, from.size(), to);
    return text;
}

// The load's diagnostics when it FAILS as it must, naming `needle`; empty (with
// an ADD_FAILURE) when it loads.
[[nodiscard]] std::vector<ConfigDiagnostic>
refusedNaming(std::string const& text, std::string_view needle) {
    if (text.empty()) return {};
    auto r = GrammarSchema::loadFromText(text, "<edited-c>");
    if (r.has_value()) {
        ADD_FAILURE() << "the edited document LOADED — it had to be refused";
        return {};
    }
    bool named = false;
    for (auto const& d : r.error()) {
        if (d.message.find(needle) != std::string::npos
            || d.path.find(needle) != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named) << "no diagnostic names '" << needle << "'; first: "
                       << (r.error().empty() ? "<none>" : r.error()[0].message);
    return r.error();
}

[[nodiscard]] std::string allErrors(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += "  ";
        out += d.actual;
        out += '\n';
    }
    return out;
}

// Preprocess and analyze `source` for x86_64 ELF through the driver's own two
// halves; the diagnostics of every stage, and whether any is an error.
struct Analyzed {
    bool                        errors = false;
    std::vector<DiagnosticCode> codes;
    std::string                 text;
};
[[nodiscard]] Analyzed analyze(std::string const& source) {
    Analyzed out;
    auto grammarR = GrammarSchema::loadShipped("c");
    auto targetR  = TargetSchema::loadShipped("x86_64");
    auto formatR  = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    if (!grammarR || !targetR || !formatR) {
        ADD_FAILURE() << "the shipped c / x86_64 / elf64-x86_64-linux-exec did not load";
        out.errors = true;
        return out;
    }
    std::shared_ptr<GrammarSchema const> const grammar = *grammarR;
    UnitBuilder builder{grammar, DiagnosticBudget::libraryDefault()};
    applySystemDirs(builder, *grammar);
    applyTargetFormatPair(builder, **targetR, **formatR);
    builder.addInMemory(source, "limit.c");
    auto const cu =
        std::make_shared<CompilationUnit const>(std::move(builder).finish());
    auto collect = [&](DiagnosticReporter const& rep) {
        for (auto const& d : rep.all()) {
            if (d.severity != DiagnosticSeverity::Error) continue;
            out.errors = true;
            out.codes.push_back(d.code);
        }
        out.text += allErrors(rep);
    };
    collect(cu->driverDiagnostics());
    for (auto const& tree : cu->trees()) collect(tree.diagnostics());
    auto const analysis = analyzeForTargetFormat(
        cu, DiagnosticBudget::libraryDefault(), **targetR, **formatR, nullptr);
    collect(analysis.model.diagnostics());
    return out;
}

}  // namespace

// ── (1) THE DOCUMENT NAMES THE LIMIT; THE LOADER WRITES THE NUMBER ───────────
TEST(ModelLimitMacro, TheDocumentNamesTheLimitAndStatesNoNumber) {
    auto const doc = nlohmann::json::parse(shippedCText());
    auto const& rows = doc.at("preprocess").at("predefinedMacros");
    std::size_t seen = 0;
    for (auto const& r : rows) {
        if (!r.is_object() || r.value("name", std::string{}) != kMacro) continue;
        ++seen;
        EXPECT_EQ(r.value("kind", std::string{}), "model-limit")
            << kMacro << " must NAME the model's limit, never restate it";
        EXPECT_EQ(r.value("limit", std::string{}), "bitIntMaxWidth");
        EXPECT_FALSE(r.contains("value"))
            << kMacro << ": a number in the document is a second owner of the "
                         "model's bound";
    }
    EXPECT_EQ(seen, 1u) << kMacro << " must be declared exactly once";
}

TEST(ModelLimitMacro, TheLoadedMacroIsTheModelsBound) {
    auto grammarR = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(grammarR.has_value());
    std::size_t seen = 0;
    for (auto const& pm : (*grammarR)->preprocess().predefinedMacros) {
        if (pm.name != kMacro) continue;
        ++seen;
        EXPECT_EQ(pm.kind, PredefinedMacroKind::Constant)
            << "a model-limit row LOWERS to a constant at load";
        EXPECT_EQ(pm.value, std::format("{}", kBitIntMaxWidth))
            << kMacro << " must be the value of kBitIntMaxWidth";
    }
    EXPECT_EQ(seen, 1u);
    // `model-limit` is a LOAD-time lowering, not a runtime kind — the same
    // one-directional invariant `version` keeps (test_dump_predefined_macros).
    EXPECT_FALSE(predefinedMacroKindFromName("model-limit").has_value())
        << "a table row for `model-limit` would claim a kind the engine never holds";
}

// ── (2) THE MACRO AND THE WIDTH CHECK READ ONE NUMBER ───────────────────────
TEST(ModelLimitMacro, TheMacroAndTheWidthCheckReadOneNumber) {
    // The widest `_BitInt` the macro promises is accepted — named through a
    // pointer, so no megabyte object is laid out.
    auto const widest = analyze(
        "#ifndef __BITINT_MAXWIDTH__\n#error \"__BITINT_MAXWIDTH__ is not defined\"\n#endif\n"
        "_Static_assert(__BITINT_MAXWIDTH__ >= 64, \"C23: at least ULLONG_WIDTH\");\n"
        "unsigned _BitInt(__BITINT_MAXWIDTH__) *widest;\n"
        "int ok;\n");
    EXPECT_FALSE(widest.errors)
        << "`_BitInt(__BITINT_MAXWIDTH__)` must be accepted:\n" << widest.text;

    // One wider is refused, by the width check, naming the bound.
    auto const over = analyze(
        "unsigned _BitInt(__BITINT_MAXWIDTH__ + 1) *over;\n"
        "int ok;\n");
    ASSERT_TRUE(over.errors) << "`_BitInt(__BITINT_MAXWIDTH__ + 1)` must be refused";
    bool widthRefusal = false;
    for (auto const c : over.codes) {
        widthRefusal |= (c == DiagnosticCode::S_BitIntWidthExceedsMax);
    }
    EXPECT_TRUE(widthRefusal)
        << "the refusal must be the width check's own:\n" << over.text;
}

// ══ (3) THE KIND'S LOAD-TIME RULES ═══════════════════════════════════════════
// The control: the shipped document, unedited, loads — every refusal below is
// therefore the edit's, not the document's.
TEST(ModelLimitMacroLoader, TheShippedDocumentLoads) {
    auto r = GrammarSchema::loadFromText(shippedCText(), "<shipped-c>");
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<none>" : r.error()[0].message);
}

TEST(ModelLimitMacroLoader, AnUnknownLimitIsRefusedNamingTheOnesThatExist) {
    auto const ds = refusedNaming(
        shippedCEdited("\"__BITINT_MAXWIDTH__\"", "\"limit\": \"bitIntMaxWidth\"",
                       "\"limit\": \"bitIntMaxWidht\""),
        "bitIntMaxWidht");
    bool listsAccepted = false;
    for (auto const& d : ds) {
        listsAccepted |= d.message.find("'bitIntMaxWidth'") != std::string::npos;
    }
    EXPECT_TRUE(listsAccepted) << "the refusal must name the limits that exist";
}

TEST(ModelLimitMacroLoader, AValueBesideTheLimitIsRefused) {
    (void)refusedNaming(
        shippedCEdited("\"__BITINT_MAXWIDTH__\"", "\"limit\": \"bitIntMaxWidth\"",
                       "\"limit\": \"bitIntMaxWidth\", \"value\": \"8388608\""),
        "never its number");
}

TEST(ModelLimitMacroLoader, AMissingLimitIsRefused) {
    auto const ds = refusedNaming(
        shippedCEdited("\"__BITINT_MAXWIDTH__\"", "\"limit\": \"bitIntMaxWidth\", ",
                       ""),
        "requires 'limit'");
    bool missing = false;
    for (auto const& d : ds) missing |= (d.code == DiagnosticCode::C_MissingField);
    EXPECT_TRUE(missing) << "a missing required key is C_MissingField";
}

TEST(ModelLimitMacroLoader, ALimitOnAnotherKindIsRefused) {
    (void)refusedNaming(
        shippedCEdited("\"__STDC_HOSTED__\"", "\"value\": \"1\"",
                       "\"value\": \"1\", \"limit\": \"bitIntMaxWidth\""),
        "valid only on a 'model-limit'");
}
