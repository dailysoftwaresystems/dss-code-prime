// ★★★ WHAT THE END OF A LANGUAGE'S INPUT MEANS — `endOfInputImplies` and a lexer
// mode's `atEndOfInput` (P68 round 8).
//
// Two declarations, two different facts, and neither is a special case in code:
//
//   • `endOfInputImplies` (top level): the lexeme the end of input IMPLIES. The
//     gas dialects declare `"\n"`: a line-oriented language's last line ends at
//     the end of the input. ✔MEASURED 2026-09-23: GNU as 2.42 (x86_64, aarch64,
//     mingw-w64) assembles a `.s` whose last line has no newline ("end of file
//     not at end of a line; newline inserted") and clang 18.1.3 accepts it
//     silently. The tokenizer reads such a text AS IF it ended in the lexeme;
//     the buffer is never rewritten and the implied token is zero-width at the
//     end. ⓘ History: DSS refused such a file (`P_MissingRequiredChild ... got
//     '<eof>'`) until P68 round 8 declared the lexeme
//     (D-ASM-LAST-LINE-WITHOUT-A-NEWLINE-REFUSED).
//
//   • `atEndOfInput` (per lexer mode): what the end of input does to a frame of
//     that mode left unclosed there — `unterminated` (the default), `closes`,
//     or `closesWithWarning`. C's `//` comment `closes`: every C reference
//     accepts a last-line `//` with no newline. The gas dialects' `#`/`//`
//     comments `close` too, and their `/*` `closesWithWarning`: gas accepts an
//     unterminated one with a warning, clang refuses it. ⓘ History: DSS refused
//     both until P68 round 8 made the verdict the mode's declared policy
//     (D-C-LINE-COMMENT-AT-END-OF-FILE-REFUSED,
//     D-ASM-UNTERMINATED-BLOCK-COMMENT-AT-END-OF-FILE-REFUSED).
//
// ★ THE ORDER BETWEEN THE TWO: the verdicts are taken at the end of the BUFFER,
// and only then is an implied tail lexed — gas's order, which is what lets
// `ret /* never closed` keep its `ret`. A body never reads into the tail.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "asm/asm_template_to_lir.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/lexer_mode.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"
#include "core/types/tree_node.hpp"
#include "shipped_schema_or_throw.hpp"
#include "tokenizer/source_reader.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace dss;

namespace {

using test_support::shippedSchemaOrThrow;

constexpr std::array<std::string_view, 2> kGasDialects{"asm-x86_64-att",
                                                       "asm-arm64-gas"};

struct Lexed {
    std::vector<Token>           tokens;
    std::vector<ParseDiagnostic> diagnostics;
};

[[nodiscard]] Lexed lex(std::shared_ptr<GrammarSchema const> const& schema,
                        std::string text, std::string name = "<mem>") {
    auto src = SourceBuffer::fromString(std::move(text), std::move(name));
    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, diags] = std::move(tk).tokenize();
    Lexed out;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        out.tokens.push_back(stream.peek(i));
    }
    for (auto const& d : diags->all()) out.diagnostics.push_back(d);
    return out;
}

// A text with its control bytes spelled out, for a trace line.
[[nodiscard]] std::string visible(std::string_view text) {
    std::string out;
    for (char const ch : text) {
        switch (ch) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += ch;     break;
        }
    }
    return out;
}

[[nodiscard]] std::size_t countKind(Lexed const& l, SchemaTokenId kind) {
    return static_cast<std::size_t>(std::ranges::count_if(
        l.tokens, [&](Token const& t) { return t.schemaKind == kind; }));
}

[[nodiscard]] std::string describe(std::vector<ParseDiagnostic> const& ds) {
    std::string out;
    for (auto const& d : ds) {
        out += std::format("[{} {}..{}] {}\n", diagnosticCodeName(d.code),
                           d.span.start(), d.span.end(), d.actual);
    }
    return out.empty() ? std::string{"(none)"} : out;
}

// The whole-file path a `.s` takes (`UnitBuilder::addInMemory` → tokenize →
// parse): the parse diagnostics of the one tree.
[[nodiscard]] std::vector<ParseDiagnostic>
parseDiagnostics(std::shared_ptr<GrammarSchema const> const& schema,
                 std::string text) {
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(text), "<mem>.s");
    CompilationUnit unit = std::move(builder).finish();
    std::vector<ParseDiagnostic> out;
    if (unit.trees().empty()) {
        ADD_FAILURE() << "the text produced no tree";
        return out;
    }
    for (auto const& d : unit.trees()[0].diagnostics().all()) out.push_back(d);
    return out;
}

[[nodiscard]] bool anyError(std::vector<ParseDiagnostic> const& ds) {
    return std::ranges::any_of(ds, [](ParseDiagnostic const& d) {
        return d.severity == DiagnosticSeverity::Error;
    });
}

// A shipped language DOCUMENT, as JSON — the census reads what each document
// DECLARES, which is a statement about the file rather than about a load.
[[nodiscard]] nlohmann::json shippedDocument(std::string_view name) {
    auto path = findShippedConfig(ShippedConfigLocator{
        name, "sources", ".lang.json", "language",
        DiagnosticCode::C_InvalidTargetName});
    if (!path.has_value()) {
        ADD_FAILURE() << "cannot locate shipped language " << name;
        return nlohmann::json::object();
    }
    std::ifstream in{*path, std::ios::binary};
    std::ostringstream buf;
    buf << in.rdbuf();
    return nlohmann::json::parse(buf.str());
}

[[nodiscard]] std::optional<std::string>
loadRefusal(nlohmann::json const& doc) {
    auto r = GrammarSchema::loadFromText(doc.dump(), "<end-of-input-probe>");
    if (r.has_value()) return std::nullopt;
    std::string out;
    for (auto const& e : r.error()) {
        out += std::format("{}: {}\n", e.path, e.message);
    }
    return out;
}

}  // namespace

// ─── the reader: what is READ versus what is a COORDINATE ─────────────────────

TEST(EndOfInputReader, TheImpliedTailIsReadButNeverHandedOutAsACoordinate) {
    auto src = SourceBuffer::fromString("ret", "<mem>");
    SourceReader r{*src, "\n"};
    EXPECT_EQ(r.remaining(), "ret\n") << "the implied tail must be READABLE";
    EXPECT_EQ(r.peek(3), '\n');
    EXPECT_EQ(r.size(), 3u) << "size() is the BUFFER's size";
    EXPECT_EQ(r.remainingInBuffer(), "ret") << "a body sees only the buffer";
    r.advance(3);
    EXPECT_FALSE(r.isAtEnd()) << "the implied byte is still unread";
    EXPECT_TRUE(r.atBufferEnd()) << "…but nothing the author wrote is";
    EXPECT_TRUE(r.remainingInBuffer().empty());
    EXPECT_EQ(r.position(), 3u);
    r.advance(1);
    EXPECT_TRUE(r.isAtEnd());
    EXPECT_EQ(r.position(), 3u)
        << "a position past the buffer would put a span past it";
    EXPECT_EQ(r.slice(0, 4), "ret") << "a slice never includes the implied tail";
}

TEST(EndOfInputReader, NothingIsImpliedWhenTheTextIsEmptyOrAlreadyEndsWithIt) {
    auto empty = SourceBuffer::fromString("", "<mem>");
    SourceReader e{*empty, "\n"};
    EXPECT_TRUE(e.isAtEnd()) << "an empty text must stay empty";

    auto ended = SourceBuffer::fromString("ret\n", "<mem>");
    SourceReader n{*ended, "\n"};
    EXPECT_EQ(n.remaining(), "ret\n") << "a text ending in the lexeme gets no second";

    auto plain = SourceBuffer::fromString("ret", "<mem>");
    SourceReader p{*plain, ""};
    EXPECT_EQ(p.remaining(), "ret") << "an empty tail is the one-argument reader";
}

// ─── the gas dialects: a last line without its newline ────────────────────────

TEST(EndOfInputImplies, ALastLineWithoutANewlineEndsInAZeroWidthLineEnd) {
    for (auto const name : kGasDialects) {
        SCOPED_TRACE(name);
        auto const schema = shippedSchemaOrThrow(name);
        ASSERT_EQ(schema->endOfInputImplies(), "\n");
        SchemaTokenId const lineEnd = schema->schemaTokens().find("LineEnd");
        ASSERT_TRUE(lineEnd.valid());

        Lexed const l = lex(schema, "ret");
        EXPECT_TRUE(l.diagnostics.empty()) << describe(l.diagnostics);
        ASSERT_EQ(countKind(l, lineEnd), 1u)
            << "the end of input must supply the last line's LineEnd";
        auto const it = std::ranges::find_if(
            l.tokens, [&](Token const& t) { return t.schemaKind == lineEnd; });
        EXPECT_EQ(it->span.start(), 3u);
        EXPECT_EQ(it->span.end(), 3u)
            << "the implied LineEnd is ZERO-WIDTH at the end, like Eof";
        EXPECT_EQ(l.tokens.back().coreKind, CoreTokenKind::Eof);
    }
}

TEST(EndOfInputImplies, ATextThatEndsInItsNewlineGetsNoSecondOne) {
    for (auto const name : kGasDialects) {
        SCOPED_TRACE(name);
        auto const schema = shippedSchemaOrThrow(name);
        SchemaTokenId const lineEnd = schema->schemaTokens().find("LineEnd");
        EXPECT_EQ(countKind(lex(schema, "ret\n"), lineEnd), 1u);
        EXPECT_EQ(countKind(lex(schema, "ret\nret"), lineEnd), 2u);
    }
}

TEST(EndOfInputImplies, ControlAnEmptyTextStaysEmpty) {
    for (auto const name : kGasDialects) {
        SCOPED_TRACE(name);
        auto const schema = shippedSchemaOrThrow(name);
        Lexed const l = lex(schema, "");
        ASSERT_EQ(l.tokens.size(), 1u) << "only Eof: nothing is implied by nothing";
        EXPECT_EQ(l.tokens[0].coreKind, CoreTokenKind::Eof);
        EXPECT_TRUE(parseDiagnostics(schema, "").empty());
    }
}

TEST(EndOfInputImplies, ControlAFileThatIsOnlyACommentWithNoNewlineParsesClean) {
    // `#` is x86's line comment and `//` arm64's (`#` is arm64's IMMEDIATE sigil).
    struct Case { std::string_view dialect; std::string_view text; };
    for (Case const c : {Case{"asm-x86_64-att", "# only a comment"},
                         Case{"asm-arm64-gas", "// only a comment"}}) {
        SCOPED_TRACE(c.dialect);
        auto const schema = shippedSchemaOrThrow(c.dialect);
        auto const ds = parseDiagnostics(schema, std::string{c.text});
        EXPECT_FALSE(anyError(ds)) << describe(ds);
    }
}

TEST(EndOfInputImplies, EveryNoNewlineEndingAReferenceAcceptsParsesClean) {
    // The four endings measured against GNU as 2.42 and clang 18.1.3 on
    // 2026-09-23, each a complete function (the dialects require a
    // function-entry marker before a label).
    struct Case { std::string_view dialect; std::string text; };
    std::string const x86 = "\t.text\n\t.globl\tmain\n\t.type\tmain, @function\nmain:\n\tmovl\t$42, %eax\n\tret";
    std::string const a64 = "\t.text\n\t.globl\tmain\n\t.type\tmain, %function\nmain:\n\tmov\tw0, #42\n\tret";
    std::vector<Case> const cases{
        {"asm-x86_64-att", x86},
        {"asm-x86_64-att", x86 + "\t# done"},
        {"asm-x86_64-att", x86 + "\n\t "},
        {"asm-arm64-gas", a64},
        {"asm-arm64-gas", a64 + "\t// done"},
        {"asm-arm64-gas", a64 + "\n\t "},
    };
    for (auto const& c : cases) {
        SCOPED_TRACE(std::format("{}: ...{}", c.dialect, visible(c.text.substr(c.text.size() - 8))));
        auto const ds = parseDiagnostics(shippedSchemaOrThrow(c.dialect), c.text);
        EXPECT_FALSE(anyError(ds)) << describe(ds);
    }
    // A CRLF file missing its final CRLF: `\r` is whitespace in both dialects.
    for (auto const name : kGasDialects) {
        SCOPED_TRACE(std::format("{} CRLF", name));
        std::string crlf = name == "asm-x86_64-att" ? x86 : a64;
        std::string out;
        for (char ch : crlf) {
            if (ch == '\n') out += '\r';
            out += ch;
        }
        auto const ds = parseDiagnostics(shippedSchemaOrThrow(name), out);
        EXPECT_FALSE(anyError(ds)) << describe(ds);
    }
}

TEST(EndOfInputImplies, NoTokenSpanPassesTheEndOfTheBuffer) {
    for (auto const name : kGasDialects) {
        SCOPED_TRACE(name);
        auto const schema = shippedSchemaOrThrow(name);
        for (std::string const text : {"ret", "ret\r", "ret\t", "nop\nret", "x:"}) {
            Lexed const l = lex(schema, text);
            for (auto const& t : l.tokens) {
                EXPECT_LE(t.span.end(), text.size())
                    << "a span past the buffer in " << visible(text);
            }
        }
    }
}

// ─── the template side: ONE rule, no second copy at the mint ─────────────────

TEST(EndOfInputImplies, ATemplatesLastLineIsTerminatedByTheDialectsRule) {
    // ★ THE TEMPLATE PIN. The template mint used to append its own `\n`; it no
    // longer does, so a template's last line is terminated ONLY by the dialect's
    // `endOfInputImplies`. Remove the key and this goes red — which is the proof
    // that templates are read through the declared rule.
    struct Case {
        std::string_view   dialect;
        std::string        text;
        AsmTemplateSurface surface;
    };
    std::vector<Case> const cases{
        {"asm-x86_64-att", "nop\n\tnop", AsmTemplateSurface::Basic},
        {"asm-x86_64-att", "movl %1, %0", AsmTemplateSurface::Extended},
        {"asm-arm64-gas", "nop\n\tnop", AsmTemplateSurface::Basic},
        {"asm-arm64-gas", "add %0, %1, %2", AsmTemplateSurface::Extended},
    };
    for (auto const& c : cases) {
        SCOPED_TRACE(std::format("{}: {}", c.dialect, visible(c.text)));
        DiagnosticReporter reporter{DiagnosticBudget::libraryDefault().asConfig()};
        auto const tree = parseAsmTemplateText(
            c.text, "<inline asm>", shippedSchemaOrThrow(c.dialect), c.surface,
            DiagnosticBudget::libraryDefault(), reporter);
        std::vector<ParseDiagnostic> ds(reporter.all().begin(), reporter.all().end());
        EXPECT_TRUE(tree.has_value()) << describe(ds);
        EXPECT_FALSE(anyError(ds)) << describe(ds);
    }
}

// ─── the census, and the loader ──────────────────────────────────────────────

TEST(EndOfInputImplies, ExactlyTheTwoGasDialectsDeclareIt) {
    // ★ THE CENSUS OF THE SIX SHIPPED LANGUAGE DOCUMENTS, with each "no" a reason:
    //   c           — its preprocessor re-lexes token-paste products and `#if`
    //                 scratch text through the same tokenizer; an implied newline
    //                 at the end of each would be a line boundary nobody wrote.
    //                 C states the narrower true fact on its `//` mode instead.
    //   asm         — the shared line grammar has no tokenizer of its own: a
    //                 host reads its text with the HOST's token table, and the
    //                 key is standalone-only, so it could not reach one anyway.
    //   toy         — no line-oriented statement: its newline is whitespace.
    //   tsql-subset — its newline is whitespace too; a batch ends at the end of
    //                 input with no terminator to imply.
    struct Row { std::string_view name; bool declares; };
    for (Row const row : {Row{"asm-x86_64-att", true}, Row{"asm-arm64-gas", true},
                          Row{"c", false}, Row{"asm", false}, Row{"toy", false},
                          Row{"tsql-subset", false}}) {
        SCOPED_TRACE(row.name);
        nlohmann::json const doc = shippedDocument(row.name);
        EXPECT_EQ(doc.contains("endOfInputImplies"), row.declares);
        if (row.declares) EXPECT_EQ(doc.at("endOfInputImplies"), "\n");
    }
}

TEST(EndOfInputImplies, TheLoaderRefusesALexemeTheTokenTableDoesNotDeclare) {
    nlohmann::json doc = shippedDocument("asm-x86_64-att");
    doc["endOfInputImplies"] = "\r\n";   // `\r` and `\n` are declared; `\r\n` is not
    auto const refusal = loadRefusal(doc);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("/endOfInputImplies"), std::string::npos) << *refusal;
    EXPECT_NE(refusal->find("does not declare"), std::string::npos) << *refusal;
}

TEST(EndOfInputImplies, TheLoaderRefusesAnEmptyOrNonStringValue) {
    for (nlohmann::json const bad : {nlohmann::json(""), nlohmann::json(10),
                                     nlohmann::json::array({"\n"})}) {
        SCOPED_TRACE(bad.dump());
        nlohmann::json doc = shippedDocument("asm-x86_64-att");
        doc["endOfInputImplies"] = bad;
        auto const refusal = loadRefusal(doc);
        ASSERT_TRUE(refusal.has_value());
        EXPECT_NE(refusal->find("/endOfInputImplies"), std::string::npos) << *refusal;
    }
}

TEST(EndOfInputImplies, AMisspelledKeyIsRefusedRatherThanIgnored) {
    nlohmann::json doc = shippedDocument("asm-x86_64-att");
    doc.erase("endOfInputImplies");
    doc["endOfInputImplied"] = "\n";
    auto const refusal = loadRefusal(doc);
    ASSERT_TRUE(refusal.has_value())
        << "a typo'd key would silently restore the refusal of every newline-less .s";
    EXPECT_NE(refusal->find("endOfInputImplied"), std::string::npos) << *refusal;
}

namespace {

// A throwaway config root holding ONE referenced document, entered as the cwd
// for the test's duration (the same technique as test_language_references.cpp's
// `ProbeConfigRoot`: `findShippedConfig` falls through `$DSS_CONFIG_ROOT` to a
// cwd walk, so the probe is ADDED without anything shipped being removed).
class ProbeRoot {
public:
    explicit ProbeRoot(nlohmann::json const& referenced) {
        namespace fs = std::filesystem;
        std::error_code ec;
        root_ = fs::temp_directory_path() / "dss-end-of-input-probe";
        fs::remove_all(root_, ec);
        fs::path const sources = root_ / "src" / "dss-config" / "sources";
        fs::create_directories(sources, ec);
        if (ec) {
            ADD_FAILURE() << "cannot create the probe root: " << ec.message();
            return;
        }
        {
            std::ofstream out(sources / "eoiprobe.lang.json", std::ios::binary);
            out << referenced.dump(2);
        }
        previous_ = fs::current_path(ec);
        fs::current_path(root_, ec);
        if (ec) ADD_FAILURE() << "cannot enter the probe root: " << ec.message();
    }
    ~ProbeRoot() {
        std::error_code ec;
        if (!previous_.empty()) std::filesystem::current_path(previous_, ec);
        std::filesystem::remove_all(root_, ec);
    }
    ProbeRoot(ProbeRoot const&)            = delete;
    ProbeRoot& operator=(ProbeRoot const&) = delete;

private:
    std::filesystem::path root_;
    std::filesystem::path previous_;
};

}  // namespace

TEST(EndOfInputImplies, AReferencedDocumentsDeclarationIsNeverMergedIntoItsHost) {
    // ★ STANDALONE-ONLY, AS INTENDED: a referenced document may declare it (it
    // describes that document's OWN input), and the host's end of input is NOT
    // changed by it — a host reads its file with its own tokenizer.
    nlohmann::json const referenced = nlohmann::json::parse(R"({
      "dssSchemaVersion": 4,
      "language": { "name": "eoiprobe", "version": "0.0.1" },
      "requires": { "rules": ["probeValue"], "tokens": ["probeMark"] },
      "tokens": { "\n": [{ "kind": "ProbeLineEnd" }] },
      "endOfInputImplies": "\n",
      "shapes": { "probeStmt": { "sequence": ["probeMark", "probeValue"] } }
    })");
    ProbeRoot root{referenced};
    nlohmann::json const host = nlohmann::json::parse(R"({
      "dssSchemaVersion": 4,
      "language": { "name": "EoiHost", "version": "0.0.1" },
      "languageReferences": {
        "eoiprobe": {
          "entry": "probeStmt",
          "bindRules":  { "probeValue": "hostValue" },
          "bindTokens": { "probeMark":  "BraceOpen" }
        }
      },
      "tokens": {
        " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        "{": [{ "kind": "BraceOpen" }],
        "!": [{ "kind": "BangEnd" }]
      },
      "shapes": {
        "root":          { "sequence": [{ "repeat": "hostStmt" }] },
        "hostStmt":      { "alt": ["probeStmt", "hostPlainStmt"] },
        "hostPlainStmt": { "sequence": ["hostValue", "BangEnd"] },
        "hostValue":     { "sequence": ["Identifier"] }
      }
    })");
    auto const loaded = GrammarSchema::loadFromText(host.dump(), "<eoi-host>");
    ASSERT_TRUE(loaded.has_value()) << [&] {
        std::string s;
        for (auto const& e : loaded.error()) s += e.path + ": " + e.message + "\n";
        return s;
    }();
    EXPECT_TRUE((*loaded)->endOfInputImplies().empty())
        << "a referenced document's end-of-input lexeme reached the HOST";
}

// ─── the per-mode end-of-input policy (schema half) ──────────────────────────

namespace {

[[nodiscard]] LexerMode const& modeOf(GrammarSchema const& s, std::string_view name) {
    LexerModeId const id = s.findLexerMode(name);
    if (!id.valid()) throw std::runtime_error{std::string{"no mode "} + std::string{name}};
    return s.lexerMode(id);
}

}  // namespace

TEST(AtEndOfInput, TheShippedModesDeclareWhatTheReferencesDo) {
    auto const c = shippedSchemaOrThrow("c");
    EXPECT_EQ(modeOf(*c, "line-comment").atEndOfInput, EndOfInputPolicy::Closes);
    EXPECT_EQ(modeOf(*c, "block-comment").atEndOfInput, EndOfInputPolicy::Unterminated)
        << "every C reference refuses a `/*` the file never closes";
    EXPECT_EQ(modeOf(*c, "directive").atEndOfInput, EndOfInputPolicy::Closes)
        << "a popAtNewline mode's verdict is `closes` by construction";
    for (auto const name : kGasDialects) {
        SCOPED_TRACE(name);
        auto const s = shippedSchemaOrThrow(name);
        EXPECT_EQ(modeOf(*s, "block-comment").atEndOfInput,
                  EndOfInputPolicy::ClosesWithWarning);
        // ⚠ A gas line comment NEEDS its own verdict even though the dialect
        // implies a final newline: the verdict is taken at the end of the
        // BUFFER, before the implied line end is lexed (gas's order — it is
        // what lets `ret /* never closed` keep its `ret`), so a `#`/`//`
        // comment is left unclosed when the sweep runs.
        EXPECT_EQ(modeOf(*s, "line-comment").atEndOfInput, EndOfInputPolicy::Closes);
    }
}

TEST(AtEndOfInput, TheLoaderRefusesAnUnknownValue) {
    nlohmann::json doc = shippedDocument("asm-x86_64-att");
    doc["lexerModes"]["block-comment"]["atEndOfInput"] = "closesQuietly";
    auto const refusal = loadRefusal(doc);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("/lexerModes/block-comment/atEndOfInput"), std::string::npos)
        << *refusal;
    EXPECT_NE(refusal->find("closesWithWarning"), std::string::npos)
        << "the refusal must list the allowed values: " << *refusal;
}

TEST(AtEndOfInput, ThePolicyIsNotDeclarableOnAPopAtNewlineMode) {
    for (std::string_view const value : {"closes", "unterminated"}) {
        SCOPED_TRACE(value);
        nlohmann::json doc = shippedDocument("c");
        doc["lexerModes"]["directive"]["atEndOfInput"] = value;
        auto const refusal = loadRefusal(doc);
        ASSERT_TRUE(refusal.has_value())
            << "one way to say one thing: the line scope already decides";
        EXPECT_NE(refusal->find("/lexerModes/directive/atEndOfInput"), std::string::npos)
            << *refusal;
    }
}

TEST(AtEndOfInput, ThePolicyIsNotDeclarableOnMain) {
    nlohmann::json doc = shippedDocument("asm-x86_64-att");
    doc["lexerModes"]["main"] = nlohmann::json::object({{"atEndOfInput", "closes"}});
    auto const refusal = loadRefusal(doc);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("/lexerModes/main/atEndOfInput"), std::string::npos)
        << *refusal;
}

TEST(AtEndOfInput, AMisspelledModeKeyIsRefusedRatherThanIgnored) {
    nlohmann::json doc = shippedDocument("c");
    doc["lexerModes"]["line-comment"].erase("atEndOfInput");
    doc["lexerModes"]["line-comment"]["atEndOfInputs"] = "closes";
    auto const refusal = loadRefusal(doc);
    ASSERT_TRUE(refusal.has_value())
        << "a typo'd policy key would silently keep refusing a last-line `//`";
    EXPECT_NE(refusal->find("atEndOfInputs"), std::string::npos) << *refusal;
}

// ─── TOKENIZER HALF: the policy's behaviour ──────────────────────────────────

TEST(AtEndOfInput, ACLineCommentOnTheLastLineClosesSilently) {
    // ✔MEASURED 2026-09-23: gcc 13.3.0, clang 18.1.3, aarch64 gcc, mingw-w64 gcc
    // 13.2.0 and MSVC 19.51 accept all three; DSS refused all three with
    // `P_UnterminatedComment` before the policy.
    auto const c = shippedSchemaOrThrow("c");
    for (std::string const text : {"int x; // c", "#define X 1 // c",
                                   "int x;\n// c \\"}) {
        SCOPED_TRACE(visible(text));
        Lexed const l = lex(c, text, "<mem>.c");
        EXPECT_TRUE(l.diagnostics.empty()) << describe(l.diagnostics);
    }
}

TEST(AtEndOfInput, ControlACBlockCommentTheFileNeverClosesIsStillRefused) {
    // Every C reference refuses it: gcc/clang "unterminated comment", MSVC C1071.
    auto const c = shippedSchemaOrThrow("c");
    Lexed const l = lex(c, "int x;\n/* never closed", "<mem>.c");
    ASSERT_EQ(l.diagnostics.size(), 1u) << describe(l.diagnostics);
    EXPECT_EQ(l.diagnostics[0].code, DiagnosticCode::P_UnterminatedComment);
    EXPECT_EQ(l.diagnostics[0].severity, DiagnosticSeverity::Error);
}

TEST(AtEndOfInput, AGasBlockCommentTheFileNeverClosesIsClosedWithAWarningAtItsOpener) {
    // ✔MEASURED 2026-09-23: GNU as 2.42 assembles `ret /* never closed` with no
    // newline — "end of file in comment" — and the program runs; clang refuses.
    // So the COMMENT closes with a warning at its opener, AND the `ret` line is
    // still terminated by the implied newline.
    for (auto const name : kGasDialects) {
        SCOPED_TRACE(name);
        auto const schema = shippedSchemaOrThrow(name);
        std::string const text = "nop\nret /* never closed";
        Lexed const l = lex(schema, text);
        ASSERT_EQ(l.diagnostics.size(), 1u) << describe(l.diagnostics);
        ParseDiagnostic const& d = l.diagnostics[0];
        EXPECT_EQ(d.code, DiagnosticCode::P_ClosedByEndOfInput);
        EXPECT_EQ(d.severity, DiagnosticSeverity::Warning);
        EXPECT_EQ(d.span.start(), text.find("/*")) << "reported at the OPENER";
        EXPECT_EQ(d.span.end(), text.size()) << "spanning to the end of the input";
        auto const ds = parseDiagnostics(schema, text);
        EXPECT_FALSE(anyError(ds)) << describe(ds);
    }
}

// ─── a BODY never reads into the implied tail ────────────────────────────────
//
// The tail (`endOfInputImplies`) is a line end the LANGUAGE implies; no byte of
// it is text inside a literal or a comment the author left open. Three code
// paths could read it anyway, and each is pinned here on a synthetic language,
// because no shipped dialect has a string escape or a two-byte closer:
//   • the COALESCED body loop (it would swallow the tail into the literal);
//   • an escape lead as the last byte the author wrote (it would escape the tail);
//   • a multi-byte closer (its second byte would match the tail).

namespace {

[[nodiscard]] std::shared_ptr<GrammarSchema const> impliedTailProbeSchema() {
    nlohmann::json const doc = nlohmann::json::parse(R"({
      "dssSchemaVersion": 4,
      "language": { "name": "EoiBodyProbe", "version": "0.0.1" },
      "tokens": {
        " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        "\n": [{ "kind": "LineEnd" }],
        "\"": [{ "kind": "StrOpen", "modeOp": "pushMode", "modeArg": "str",
                 "stringStyle": { "escapeKind": "char", "escapeChar": "\\",
                                  "endsAt": "\"", "multiline": true } }],
        "'":  [{ "kind": "ChrOpen", "modeOp": "pushMode", "modeArg": "chr",
                 "stringStyle": { "escapeKind": "char", "escapeChar": "\\",
                                  "endsAt": "'", "multiline": true } }],
        "{":  [{ "kind": "NoteOpen", "flags": ["EmptySpace"], "modeOp": "pushMode",
                 "modeArg": "note",
                 "stringStyle": { "escapeKind": "none", "endsAt": "}\n",
                                  "multiline": true } }]
      },
      "endOfInputImplies": "\n",
      "lexerModes": {
        "str":  { "defaultToken": { "kind": "StrBody", "coalesce": true,
                                    "closeToken": "StrClose" },
                  "unterminatedAs": "string" },
        "chr":  { "defaultToken": { "kind": "ChrChar" }, "unterminatedAs": "string" },
        "note": { "defaultToken": { "kind": "NoteChar", "flags": ["EmptySpace"] },
                  "unterminatedAs": "comment" }
      },
      "shapes": {
        "root": { "sequence": [{ "repeat": "LineEnd" }] }
      }
    })");
    auto loaded = GrammarSchema::loadFromText(doc.dump(), "<eoi-body-probe>");
    if (!loaded.has_value()) {
        std::string s = "the probe language did not load:\n";
        for (auto const& e : loaded.error()) s += e.path + ": " + e.message + "\n";
        throw std::runtime_error{s};
    }
    return *loaded;
}

[[nodiscard]] bool hasCode(Lexed const& l, DiagnosticCode code) {
    return std::ranges::any_of(l.diagnostics, [&](ParseDiagnostic const& d) {
        return d.code == code;
    });
}

}  // namespace

TEST(ABodyNeverReadsIntoTheImpliedTail, ACoalescedLiteralLeavesTheTailToTheLine) {
    auto const schema = impliedTailProbeSchema();
    SchemaTokenId const lineEnd = schema->schemaTokens().find("LineEnd");
    Lexed const l = lex(schema, "\"abc");
    EXPECT_TRUE(hasCode(l, DiagnosticCode::P_UnterminatedString)) << describe(l.diagnostics);
    EXPECT_EQ(countKind(l, lineEnd), 1u)
        << "the implied line end was read INTO the literal instead of after it";
}

TEST(ABodyNeverReadsIntoTheImpliedTail, AnEscapeLeadAtTheEndEscapesNothing) {
    auto const schema = impliedTailProbeSchema();
    for (std::string const text : {"\"abc\\", "'abc\\"}) {
        SCOPED_TRACE(visible(text));
        Lexed const l = lex(schema, text);
        EXPECT_TRUE(hasCode(l, DiagnosticCode::P_InvalidEscape))
            << "the escape took the IMPLIED line end as its operand: "
            << describe(l.diagnostics);
    }
}

TEST(ABodyNeverReadsIntoTheImpliedTail, AMultiByteCloserCannotEndInTheTail) {
    // `}` + an implied `\n` would spell the closer `}\n`; the author wrote only
    // the `}`, so the body is left unclosed at the end of the input.
    auto const schema = impliedTailProbeSchema();
    Lexed const l = lex(schema, "{note}");
    EXPECT_TRUE(hasCode(l, DiagnosticCode::P_UnterminatedComment))
        << "a closer's second byte matched the implied tail: " << describe(l.diagnostics);
    // Control: the same closer the author DID write closes the body.
    Lexed const closed = lex(schema, "{note}\n");
    EXPECT_TRUE(closed.diagnostics.empty()) << describe(closed.diagnostics);
}

// ─── `NodeFlags::Implied`: the tokenizer SAYS which token it implied ────────
//
// The builder does not infer "implied" from a position; the tokenizer records
// the provenance on the one emission it read from the implied tail, the flag
// rides into the tree's node, and a flagged token under a schema that declares
// no implied lexeme is a fatal drift.

namespace {

// Every LineEnd leaf of the one tree `text` parses to, with its flags.
[[nodiscard]] std::vector<NodeFlags>
lineEndLeafFlags(std::shared_ptr<GrammarSchema const> const& schema, std::string text) {
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(text), "<mem>.s");
    CompilationUnit unit = std::move(builder).finish();
    std::vector<NodeFlags> out;
    if (unit.trees().empty()) {
        ADD_FAILURE() << "the text produced no tree";
        return out;
    }
    Tree const& t = unit.trees()[0];
    SchemaTokenId const lineEnd = schema->schemaTokens().find("LineEnd");
    // An explicit work stack, not recursion (no input-proportional recursion).
    std::vector<NodeId> work{t.root()};
    while (!work.empty()) {
        NodeId const n = work.back();
        work.pop_back();
        if (t.kind(n) == NodeKind::Token) {
            if (t.tokenKind(n) == lineEnd) out.push_back(t.flags(n));
            continue;
        }
        for (NodeId const c : t.children(n)) work.push_back(c);
    }
    return out;
}

}  // namespace

TEST(ImpliedFlag, OnlyTheImpliedLineEndCarriesItToken) {
    for (auto const name : kGasDialects) {
        SCOPED_TRACE(name);
        auto const schema = shippedSchemaOrThrow(name);
        SchemaTokenId const lineEnd = schema->schemaTokens().find("LineEnd");
        Lexed const implied = lex(schema, "nop\nret");
        std::size_t flagged = 0, plain = 0;
        for (auto const& t : implied.tokens) {
            if (t.schemaKind != lineEnd) {
                EXPECT_FALSE(has(t.flags, NodeFlags::Implied))
                    << "a token that is not the implied lexeme carries the flag";
                continue;
            }
            (has(t.flags, NodeFlags::Implied) ? flagged : plain) += 1;
        }
        EXPECT_EQ(flagged, 1u) << "the implied LineEnd must say it is implied";
        EXPECT_EQ(plain, 1u) << "the REAL newline after `nop` must not";
        EXPECT_FALSE(has(implied.tokens.back().flags, NodeFlags::Implied))
            << "Eof is the end itself, never the implied lexeme";
        for (auto const& t : lex(schema, "nop\nret\n").tokens) {
            EXPECT_FALSE(has(t.flags, NodeFlags::Implied))
                << "a text that ends in its newline implies nothing";
        }
    }
}

TEST(ImpliedFlag, TheImpliedLineEndsTreeNodeCarriesItAndARealOneNever) {
    for (auto const name : kGasDialects) {
        SCOPED_TRACE(name);
        auto const schema = shippedSchemaOrThrow(name);
        auto const implied = lineEndLeafFlags(schema, "nop\nret");
        ASSERT_EQ(implied.size(), 2u);
        EXPECT_EQ(std::ranges::count_if(implied, [](NodeFlags f) {
                      return has(f, NodeFlags::Implied);
                  }),
                  1)
            << "exactly the implied LineEnd's node carries the flag";
        for (NodeFlags const f : lineEndLeafFlags(schema, "nop\nret\n")) {
            EXPECT_FALSE(has(f, NodeFlags::Implied)) << "a real `\\n` never does";
        }
    }
}

TEST(ImpliedFlagDeathTest, AFlaggedTokenUnderASchemaDeclaringNoImpliedLexemeIsAFatalDrift) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto const schema = shippedSchemaOrThrow("toy");
    ASSERT_TRUE(schema->endOfInputImplies().empty());
    auto src = SourceBuffer::fromString("x", "<mem>");
    TreeBuilder b{src, schema, DiagnosticBudget::libraryDefault()};
    auto root = b.open(schema->rules().find("root"));
    Token fabricated{};
    fabricated.coreKind   = CoreTokenKind::Word;
    fabricated.flags      = NodeFlags::Implied;
    fabricated.schemaKind = schema->schemaTokens().find("Identifier");
    fabricated.span       = SourceSpan::empty(1);
    EXPECT_DEATH({ b.pushToken(fabricated); }, "endOfInputImplies");
}

TEST(EndOfInputImplies, TheImpliedLexemeMayNotEndALongerOne) {
    // `\r\n` declared beside an implied `\n`: a text ending in `\r` would be
    // lexed ACROSS the end of the buffer into one token, half written and half
    // implied — no single flag could say which of its bytes the source holds.
    nlohmann::json doc = shippedDocument("asm-x86_64-att");
    doc["tokens"]["\r\n"] = nlohmann::json::parse(R"([{ "kind": "LineEnd" }])");
    auto const refusal = loadRefusal(doc);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("/endOfInputImplies"), std::string::npos) << *refusal;
    EXPECT_NE(refusal->find("part written and part implied"), std::string::npos)
        << *refusal;
}

TEST(EndOfInputImplies, ControlALexemeWhoseHeadAlreadyEndsInTheImpliedOneIsAllowed) {
    // `\n\n` cannot meet the implied bytes: a buffer ending in its head `\n`
    // already ends with the lexeme, so nothing is implied after it.
    nlohmann::json doc = shippedDocument("asm-x86_64-att");
    doc["tokens"]["\n\n"] = nlohmann::json::parse(R"([{ "kind": "LineEnd" }])");
    auto const refusal = loadRefusal(doc);
    EXPECT_FALSE(refusal.has_value()) << *refusal;
}
