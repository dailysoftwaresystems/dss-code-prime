// ===========================================================================
// P69 L2b2 (lane `lm`) — THE SPELLINGS OF THE LIST TYPE ARE DECLARED, ONCE EACH.
//
// THE PROPERTY THIS FILE OWNS: `semantics.variadic.vaListTypeNames` — the spellings a
// source names the va_list TYPE by with no declaration of its own — is REQUIRED with the
// variadic block, a NON-EMPTY array of non-empty strings, NONE TWICE; and the shipped c
// document declares exactly gcc and clang's builtin spelling, `__builtin_va_list`. The
// plain `va_list` is <stdarg.h>'s typedef (the descriptor word), never a builtin:
// ✔MEASURED 2026-11-04/05 (probes l2b2v c01/c04): with no header gcc 13.3.0, clang
// 18.1.3, cl and Apple clang compile a program's own `va_list` and refuse the plain name
// as the list's.
//
// ── RED-ON-DISABLE ─────────────────────────────────────────────────────────
//   * the key not required → the "removed" cell loads;
//   * an empty array accepted → the "empty" cell loads;
//   * a spelling written twice accepted → the "twice" cell loads.
// The shipped document, unmutated, is the control: it loads and binds one spelling.
// ===========================================================================

#include "core/types/grammar_schema.hpp"
#include "core/types/semantic_config.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::string errorDiags(std::vector<ConfigDiagnostic> const& diags) {
    std::string out;
    for (auto const& d : diags) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += std::format("\n  [{}] {}", d.path, d.message);
    }
    return out.empty() ? std::string{"<no error-severity diagnostics>"} : out;
}

[[nodiscard]] std::string shippedC() {
    std::ifstream in{dss::test::configRoot() / "sources" / "c.lang.json", std::ios::binary};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

constexpr char const* kShippedKey = R"("vaListTypeNames": [ "__builtin_va_list" ])";

}  // namespace

TEST(VaListTypeNamesBlock, TheShippedCDocumentBindsTheBuiltinSpellingOnly) {
    std::string const text = shippedC();
    ASSERT_FALSE(text.empty()) << "cannot open the shipped c.lang.json";
    ASSERT_NE(text.find(kShippedKey), std::string::npos)
        << "the shipped c document declares the key as this file reads it";
    auto const result = GrammarSchema::loadFromText(text);
    ASSERT_TRUE(result.has_value()) << errorDiags(result.error());
    EXPECT_EQ((*result)->semantics().vaListTypeNames, std::vector<std::string>{"__builtin_va_list"})
        << "the c grammar's one builtin spelling of the list type; `va_list` is <stdarg.h>'s";
}

TEST(VaListTypeNamesBlock, TheKeyIsRequiredNonEmptyAndNamesEachSpellingOnce) {
    struct Cell { char const* what; char const* replacement; char const* why; };
    std::string const text = shippedC();
    auto const at = text.find(kShippedKey);
    ASSERT_NE(at, std::string::npos) << "the shipped c document declares the key as this file reads it";
    for (Cell const& c : {
             Cell{"removed", R"("$vaListTypeNamesRemoved": "the key is gone")",
                  "'vaListTypeNames' is required"},
             Cell{"empty", R"("vaListTypeNames": [ ])", "'vaListTypeNames' is required"},
             Cell{"twice", R"("vaListTypeNames": [ "__builtin_va_list", "__builtin_va_list" ])",
                  "'vaListTypeNames' names '__builtin_va_list' twice"},
             Cell{"an empty spelling", R"("vaListTypeNames": [ "__builtin_va_list", "" ])",
                  "each 'vaListTypeNames' entry must be a non-empty string"},
         }) {
        std::string mutated = text;
        mutated.replace(at, std::string{kShippedKey}.size(), c.replacement);
        auto const result = GrammarSchema::loadFromText(mutated);
        ASSERT_FALSE(result.has_value()) << c.what << ": the document must be refused";
        auto const msg = errorDiags(result.error());
        EXPECT_NE(msg.find(c.why), std::string::npos) << c.what << ", refused for its reason:" << msg;
        EXPECT_NE(msg.find("/semantics/variadic/vaListTypeNames"), std::string::npos)
            << c.what << ", at the key:" << msg;
    }
}
