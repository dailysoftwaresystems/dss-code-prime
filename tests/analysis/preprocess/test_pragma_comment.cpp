// P69 fold 3f (lane `xa`) -- `#pragma comment(kind[, "text"])`: A REQUEST TO THE PROGRAM'S LINK, READ BY THE
// PREPROCESSOR AS ITS LANGUAGE'S ROW SAYS.
//
// The C document's `comment` row (`linkerComment`) states each kind the pragma reads and what it asks
// (`PragmaCommentRequest`): `lib` a library to search, `linker` a directive the link reads as the unit's own,
// `exestr` / `compiler` / `user` nothing. A reached pragma whose kind asks something, stated with its text, is
// the TU's product (`PreprocessResult::pragmaLinkRequests`), in reach order. ✔MEASURED (P69 fold 3f; cl 19.52,
// clang 18.1.3 msvc and ELF, gcc 13.3): the operand is macro-expanded, kind and text alike; adjacent literals
// concatenate; a kind with no text asks nothing and says nothing (cl); a form cl cannot read -- no parenthesized
// kind, a kind the row does not list (the kind is case-sensitive), an operand that is not `kind` or
// `kind, literals`, a prefixed literal -- writes nothing anywhere, and cl warns: DSS accepts it and warns. The
// loader requires the kinds on a `linkerComment` row, refuses them on any other, and refuses a request no verb
// names.

#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/header_name_matching.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/preprocess_config.hpp"
#include "core/types/source_buffer.hpp"
#include "test_support/repo_root.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dss;
namespace fs = std::filesystem;

using Lib    = PragmaLinkRequest;
constexpr auto kLib    = PragmaCommentRequest::DependentLibrary;
constexpr auto kLinker = PragmaCommentRequest::LinkerDirective;

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cSchema() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded.has_value()) throw std::runtime_error{"loadShipped(c) failed"};
        return *loaded;
    }();
    return schema;
}

[[nodiscard]] PreprocessResult pp(std::string text) {
    auto schema = cSchema();
    auto buf = SourceBuffer::fromString(std::move(text), std::string{"comment.c"});
    std::vector<fs::path> const noDirs;
    return preprocess(buf, schema, noDirs, kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
}

[[nodiscard]] std::size_t pragmaDiagsAt(PreprocessResult const& r, DiagnosticSeverity sev) {
    std::size_t n = 0;
    for (auto const& d : r.diagnostics->all())
        if (d.code == DiagnosticCode::P_PreprocessorPragma && d.severity == sev) ++n;
    return n;
}

[[nodiscard]] std::string everyDiagnostic(PreprocessResult const& r) {
    std::string s;
    for (auto const& d : r.diagnostics->all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  (no diagnostics)"} : s;
}

[[nodiscard]] std::string cDocumentText() {
    auto const root = dss::test::findConfigRoot();
    if (!root.has_value()) return {};
    std::ifstream in{*root / "sources" / "c.lang.json", std::ios::binary};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(PragmaComment, ALibraryAndADirectiveAreRecordedInReachOrder) {
    PreprocessResult const r = pp(
        "#pragma comment(lib, \"m\")\n"
        "int x;\n"
        "#pragma comment(linker, \"/STACK:0x200000\")\n"
        "#pragma comment(lib, \"user32.lib\")\n");
    EXPECT_EQ(r.pragmaLinkRequests,
              (std::vector<Lib>{{kLib, "m"}, {kLinker, "/STACK:0x200000"}, {kLib, "user32.lib"}}))
        << everyDiagnostic(r);
    EXPECT_EQ(pragmaDiagsAt(r, DiagnosticSeverity::Warning) + pragmaDiagsAt(r, DiagnosticSeverity::Error), 0u)
        << everyDiagnostic(r);
}

TEST(PragmaComment, TheOperandIsMacroExpandedAndAdjacentLiteralsConcatenate) {
    PreprocessResult const r = pp(
        "#define KIND lib\n"
        "#define NAME \"us\" \"er32\"\n"
        "#pragma comment(KIND, NAME)\n"
        "#pragma comment(linker, \"/STACK:\" \"0x2000\")\n"
        "#pragma comment(linker, \"/manifestdependency:\\\"type='win32' name='A.B'\\\"\")\n");
    EXPECT_EQ(r.pragmaLinkRequests,
              (std::vector<Lib>{{kLib, "user32"},
                                {kLinker, "/STACK:0x2000"},
                                {kLinker, "/manifestdependency:\"type='win32' name='A.B'\""}}))
        << everyDiagnostic(r);
}

TEST(PragmaComment, ThePragmaOperatorAndAMacroBorneOneAreRecorded) {
    PreprocessResult const r = pp(
        "_Pragma(\"comment(lib, \\\"a\\\")\")\n"
        "#define LINK_B _Pragma(\"comment(lib, \\\"b\\\")\")\n"
        "LINK_B\n"
        "int x;\n");
    EXPECT_EQ(r.pragmaLinkRequests, (std::vector<Lib>{{kLib, "a"}, {kLib, "b"}})) << everyDiagnostic(r);
}

TEST(PragmaComment, AKindThatAsksNothingOrStatesNoTextWritesNothingAndSaysNothing) {
    for (char const* line : {"#pragma comment(exestr, \"built today\")\n", "#pragma comment(compiler)\n",
                             "#pragma comment(user, \"someone\")\n", "#pragma comment(lib)\n",
                             "#pragma comment(linker)\n"}) {
        PreprocessResult const r = pp(std::string{line} + "int x;\n");
        EXPECT_TRUE(r.pragmaLinkRequests.empty()) << line;
        EXPECT_EQ(r.diagnostics->all().size(), 0u) << line << everyDiagnostic(r);
    }
}

TEST(PragmaComment, AFormClCannotReadIsWarnedAndAsksNothing) {
    for (char const* line : {"#pragma comment\n", "#pragma comment lib\n", "#pragma comment(LIB, \"m\")\n",
                             "#pragma comment(nosuchkind, \"m\")\n", "#pragma comment(lib, 42)\n",
                             "#pragma comment(lib, \"a\", \"b\")\n", "#pragma comment(lib \"m\")\n",
                             "#pragma comment(lib, L\"m\")\n", "#pragma comment(lib, u8\"m\")\n",
                             "#pragma comment(lib, \"m\" L\"n\")\n"}) {
        PreprocessResult const r = pp(std::string{line} + "int x;\n");
        EXPECT_TRUE(r.pragmaLinkRequests.empty()) << line;
        EXPECT_EQ(pragmaDiagsAt(r, DiagnosticSeverity::Warning), 1u) << line << everyDiagnostic(r);
        EXPECT_FALSE(r.diagnostics->hasErrors()) << line << everyDiagnostic(r);
    }
}

TEST(PragmaComment, APragmaInADeadBranchAsksNothing) {
    PreprocessResult const r = pp(
        "#if 0\n"
        "#pragma comment(lib, \"m\")\n"
        "#pragma comment(nosuchkind)\n"
        "#endif\n"
        "int x;\n");
    EXPECT_TRUE(r.pragmaLinkRequests.empty());
    EXPECT_EQ(r.diagnostics->all().size(), 0u) << everyDiagnostic(r);
}

TEST(PragmaComment, TheDocumentStatesEachKindAndTheLoaderRefusesARowThatDoesNot) {
    auto const& kinds = [] {
        for (PragmaEffectRow const& row : cSchema()->preprocess().pragmaEffects)
            if (row.effect == PragmaEffect::LinkerComment) return row.commentKinds;
        return std::vector<PragmaCommentKind>{};
    }();
    auto const requestOf = [&](std::string_view name) -> std::optional<PragmaCommentRequest> {
        for (auto const& k : kinds)
            if (k.name == name) return k.request;
        return std::nullopt;
    };
    EXPECT_EQ(kinds.size(), 5u);
    EXPECT_EQ(requestOf("lib"), kLib);
    EXPECT_EQ(requestOf("linker"), kLinker);
    for (char const* nothing : {"exestr", "compiler", "user"})
        EXPECT_EQ(requestOf(nothing), PragmaCommentRequest::Nothing) << nothing;

    std::string const text = cDocumentText();
    std::string const row  = "\"effect\": \"linkerComment\",";
    ASSERT_NE(text.find(row), std::string::npos) << "the shipped C document no longer states the comment row";
    struct Case {
        char const* what;
        std::string from;
        std::string to;
    };
    for (Case const& c : {
             Case{"a linkerComment row stating no kinds",
                  "\"kinds\": { \"lib\": \"dependentLibrary\", \"linker\": \"linkerDirective\", \"exestr\": "
                  "\"nothing\", \"compiler\": \"nothing\", \"user\": \"nothing\" }",
                  "\"$noKinds\": 0"},
             Case{"a kind mapped to a request no verb names", "\"lib\": \"dependentLibrary\"",
                  "\"lib\": \"library\""},
             Case{"kinds on another row's verb", "\"effect\": \"linkerComment\",",
                  "\"effect\": \"annotationOnly\","},
         }) {
        std::string edited = text;
        auto const at = edited.find(c.from);
        ASSERT_NE(at, std::string::npos) << c.what;
        edited.replace(at, c.from.size(), c.to);
        auto const loaded = GrammarSchema::loadFromText(edited, "<pragma-comment-c>");
        EXPECT_FALSE(loaded.has_value()) << c.what << " loaded";
        bool named = false;
        if (!loaded.has_value())
            for (auto const& e : loaded.error()) named = named || e.message.find("kinds") != std::string::npos;
        EXPECT_TRUE(named) << c.what << ": no refusal names 'kinds'";
    }
}
