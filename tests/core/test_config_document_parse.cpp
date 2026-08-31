// D-CONFIG-A-DUPLICATE-JSON-KEY-IS-DROPPED-WITHOUT-A-DIAGNOSTIC
//
// THREE tiers, and each answers a question the others cannot:
//
//   1. `ConfigDocumentParse`          the owner itself — is a repeat NOTICED,
//                                      and does the JSON pointer it reports
//                                      ADDRESS the offending key?
//   2. `ConfigDuplicateKeyRefusal`    the loaders — does a repeat REFUSE the
//                                      load, in the families the operator
//                                      measured compiling a different program?
//   3. `ConfigDocumentParseIsTheOneOwner`
//                                      the tree — is there any OTHER way to
//                                      turn config bytes into a `json`?
//
// ★ Tier 3 is the one that makes tiers 1 and 2 durable. A duplicate check that
// lives in the loaders is a check the NEXT loader does not get; a check that
// lives in one function is only as good as the guarantee that nothing bypasses
// it, and that guarantee is a property of the TREE, not of any one file.

#include "core/types/config_document_parse.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/project_config.hpp"
#include "core/types/target_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::detail::ConfigDocumentParseFailure;
using dss::detail::escapeJsonPointerSegment;
using dss::detail::parseConfigDocument;

namespace {

std::string readWholeFile(std::filesystem::path const& p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) return {};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

} // namespace

// ═══ TIER 1 — THE OWNER ═════════════════════════════════════════════════════

TEST(ConfigDocumentParse, AcceptsADocumentWithNoRepeatedKey) {
    auto const parsed = parseConfigDocument(
        R"({"a": 1, "b": {"a": 2}, "c": [{"a": 3}, {"a": 4}]})");
    ASSERT_TRUE(parsed.has_value())
        << "a key repeated in DIFFERENT objects is not a repeat; only a repeat "
           "inside ONE object loses a declaration";
    EXPECT_EQ(parsed->at("a").get<int>(), 1);
    EXPECT_EQ(parsed->at("b").at("a").get<int>(), 2);
    EXPECT_EQ(parsed->at("c")[1].at("a").get<int>(), 4);
}

TEST(ConfigDocumentParse, RefusesARepeatAtTheRootAndPointsAtIt) {
    auto const parsed =
        parseConfigDocument(R"({"charIsUnsigned": true, "charIsUnsigned": false})");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().kind, ConfigDocumentParseFailure::DuplicateKey);
    EXPECT_EQ(parsed.error().pointer, "/charIsUnsigned");
    EXPECT_NE(parsed.error().message.find("declared more than once"),
              std::string::npos)
        << "the sentence must SAY what happened; a pointer with no explanation "
           "reads as a shape complaint about the value";
}

TEST(ConfigDocumentParse, PointerAddressesARepeatNestedUnderObjectsAndArrays) {
    // The exact shape the brief named: `/opcodes/17/mnemonic`. Built with
    // eighteen entries so the index is a REAL cursor rather than a 0 that any
    // off-by-one would also produce.
    std::string doc = R"({"opcodes": [)";
    for (int i = 0; i < 17; ++i) doc += R"({"mnemonic": "nop"}, )";
    doc += R"({"mnemonic": "add", "mnemonic": "sub"}]})";

    auto const parsed = parseConfigDocument(doc);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().kind, ConfigDocumentParseFailure::DuplicateKey);
    EXPECT_EQ(parsed.error().pointer, "/opcodes/17/mnemonic");
}

TEST(ConfigDocumentParse, ArrayCursorCountsEveryValueShapeNotOnlyObjects) {
    // The array cursor advances for SCALARS and for NESTED ARRAYS too, not only
    // for the objects it is easy to remember. Getting this wrong reports a
    // pointer that addresses a DIFFERENT element — a locator that is confidently
    // wrong, which is worse than none.
    auto const parsed = parseConfigDocument(
        R"({"rows": [1, "two", null, [3, 4], {"k": 1, "k": 2}]})");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().pointer, "/rows/4/k");
}

TEST(ConfigDocumentParse, PointerSegmentsAreRfc6901Escaped) {
    EXPECT_EQ(escapeJsonPointerSegment("plain"), "plain");
    EXPECT_EQ(escapeJsonPointerSegment("a/b"), "a~1b");
    EXPECT_EQ(escapeJsonPointerSegment("a~b"), "a~0b");
    // ORDER: `~` first. Escaping `/` first would make `~1` out of the slash and
    // the second pass would escape ITS tilde into `~01`, addressing a key that
    // does not exist.
    EXPECT_EQ(escapeJsonPointerSegment("a~/b"), "a~0~1b");

    auto const parsed =
        parseConfigDocument(R"({"outer": {"a/b~c": 1, "a/b~c": 2}})");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().pointer, "/outer/a~1b~0c");
}

TEST(ConfigDocumentParse, ReportsTheFirstRepeatInDocumentOrder) {
    auto const parsed =
        parseConfigDocument(R"({"x": {"a": 1, "a": 2}, "y": {"b": 1, "b": 2}})");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().pointer, "/x/a")
        << "deterministic on every host: the scan aborts at the first repeat, "
           "so which one is named cannot depend on iteration order";
}

TEST(ConfigDocumentParse, MalformedBytesKeepTheirOwnFailureKindAndNoPointer) {
    auto const parsed = parseConfigDocument(R"({"a": )");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().kind, ConfigDocumentParseFailure::NotJson);
    EXPECT_TRUE(parsed.error().pointer.empty())
        << "bytes that are not JSON have no position INSIDE a document";
    EXPECT_NE(parsed.error().message.find("parse error"), std::string::npos)
        << "nlohmann's own what(), verbatim — routing a reader through the "
           "owner must move no existing message";
}

TEST(ConfigDocumentParse, RenderersProduceTheTwoShapesTheCallersEmit) {
    auto const dup = parseConfigDocument(R"({"k": 1, "k": 2})");
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().locus("some.target.json"), "/k")
        << "a duplicate is located by POINTER, like every shape diagnostic";
    EXPECT_EQ(dup.error().detailText("JSON parse error: "), dup.error().message)
        << "the duplicate sentence is complete; it must not take a "
           "parse-error prefix that would misdescribe it";
    EXPECT_EQ(dup.error().detailTextWithLocus("JSON parse error: "),
              "at /k: " + dup.error().message);

    auto const bad = parseConfigDocument("{");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().locus("some.target.json"), "some.target.json")
        << "malformed bytes keep the caller's document label";
    EXPECT_EQ(bad.error().detailText("JSON parse error: "),
              "JSON parse error: " + bad.error().message);
    EXPECT_EQ(bad.error().detailTextWithLocus("JSON parse error: "),
              "JSON parse error: " + bad.error().message)
        << "with no pointer, the with-locus renderer must degrade to the plain "
           "one rather than emit a dangling `at : `";
}

// ═══ TIER 2 — THE LOADERS ═══════════════════════════════════════════════════

// ★ THE ORIGINAL FINDING, PINNED.
//
// ✔The operator MEASURED, through the shipped CLI on a COPY of the config tree:
// appending a SHAPE-VALID second `charIsUnsigned` to `arm64.target.json` with
// `default` flipped compiled at rc=0, emitted ZERO diagnostics, and MOVED the
// emitted bytes (`0a18d96c…` → `d6efabca…`, control returned). This is that
// document class, that key, that shape — refused.
//
// ⚠ THE SHIPPED DOCUMENT IS READ, NEVER WRITTEN. The bytes are mutated in
// MEMORY and handed to `loadFromText`; nothing under `src/dss-config/` is
// touched, so a sibling lane building against this tree cannot see this test.
TEST(ConfigDuplicateKeyRefusal, TargetDocumentWithAShapeValidRepeatIsRefused) {
    // ⚠ `dss::test::configRoot()` IS the `src/dss-config` directory, whereas
    // the `DSS_CONFIG_ROOT` env var names the directory that CONTAINS it. Two
    // meanings for one word; appending `src/dss-config` here once produced a
    // path with the segment twice, and the miss reads as "no such document"
    // rather than "wrong path shape".
    auto const path = dss::test::configRoot() / "targets" / "arm64.target.json";
    std::string const pristine = readWholeFile(path);
    ASSERT_FALSE(pristine.empty()) << "no target document at " << path.string();

    // CONTROL — the unmutated bytes must LOAD. Without this the arm below is
    // vacuous: a document that never loaded would "refuse" for any reason.
    auto const control = TargetSchema::loadFromText(pristine, "arm64.target.json");
    ASSERT_TRUE(control.has_value())
        << "the CONTROL did not load; the mutant arm would prove nothing";

    // Append a SECOND `charIsUnsigned` as the last root member — shape-valid, so
    // the shape validator has nothing to say and anything that moves is
    // attributable to the repeat alone. (Injecting a bare `false` instead
    // produces `'charIsUnsigned' must be an OBJECT`, which is the SHAPE
    // validator objecting to the WINNING VALUE — it proves the late key won and
    // proves nothing about noticing a repeat.)
    std::string body = pristine;
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r'
                             || body.back() == ' ' || body.back() == '\t')) {
        body.pop_back();
    }
    ASSERT_EQ(body.back(), '}') << "the root object does not end where expected";
    body.pop_back();
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r'
                             || body.back() == ' ' || body.back() == '\t')) {
        body.pop_back();
    }
    body += R"(,"charIsUnsigned": {"default": false,)"
            R"( "byObjectFormat": {"macho": false, "pe": false}}})";

    auto const mutated = TargetSchema::loadFromText(body, "arm64.target.json");
    ASSERT_FALSE(mutated.has_value())
        << "a target document with a repeated key still loads — the repeat is "
           "still silent, and it still decides what gets emitted";

    bool named = false;
    for (auto const& d : mutated.error()) {
        if (d.code == DiagnosticCode::C_MalformedJson
            && d.path == "/charIsUnsigned"
            && d.message.find("declared more than once") != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named)
        << "the refusal must NAME the offending key by its JSON pointer; a "
           "generic 'malformed' sends the reader to re-read a 264 KB document";
}

TEST(ConfigDuplicateKeyRefusal, ProjectManifestWithARepeatIsRefused) {
    // Fully SYNTHESISED — no shipped document is read at all.
    constexpr std::string_view kClean =
        R"({"language": "c", "artifactProfile": "exe",)"
        R"( "targets": ["x86_64:elf64-x86_64-linux-exec"],)"
        R"( "sources": ["main.c"]})";
    constexpr std::string_view kRepeated =
        R"({"language": "c", "artifactProfile": "exe",)"
        R"( "targets": ["x86_64:elf64-x86_64-linux-exec"],)"
        R"( "sources": ["main.c"], "language": "asm"})";

    DiagnosticReporter cleanRep;
    auto const clean = parseProjectConfig(kClean, ".dss-project.json", cleanRep);
    ASSERT_TRUE(clean.has_value())
        << "the CONTROL manifest did not load; the arm below would be vacuous";
    EXPECT_EQ(clean->language, "c");

    DiagnosticReporter repeatedRep;
    auto const repeated =
        parseProjectConfig(kRepeated, ".dss-project.json", repeatedRep);
    EXPECT_FALSE(repeated.has_value())
        << "the LAST `language` wins silently — the manifest says `c` at the "
           "top and the build compiles `asm`";

    // `report(rep, code, severity, text)` puts the sentence in `actual` — this
    // channel has no separate path field, which is exactly why the manifest
    // reader uses `detailTextWithLocus` and the pointer arrives INSIDE the
    // sentence.
    bool named = false;
    for (auto const& d : repeatedRep.all()) {
        if (d.code == DiagnosticCode::C_MalformedJson
            && d.actual.find("/language") != std::string::npos
            && d.actual.find("declared more than once") != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named) << "the refusal must name `/language`";
}

// ═══ TIER 3 — THE TREE ══════════════════════════════════════════════════════
//
// ⚠ WHAT THIS SCAN CAN AND CANNOT DECIDE, STATED RATHER THAN ASSUMED.
// It reads TEXT. It can decide the two spellings that name a function
// (`json::parse`, `sax_parse`) and it can decide `operator>>` into a variable
// whose declaration it saw IN THE SAME FILE. It cannot decide `operator>>` into
// a `json` reached through a typedef, a template parameter, or another
// translation unit. Its own fixtures are SYNTHESISED sources carrying each
// spelling, so a scan that has stopped seeing anything reds instead of
// reporting a clean tree — the failure direction that matters here, since every
// text guard fails toward *clean*.

namespace {

bool isIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9') || c == '_';
}

bool isDigitChar(char c) { return c >= '0' && c <= '9'; }

// Replace every comment body and string/char literal with spaces, preserving
// length so nothing downstream has to care that it happened. A prose mention of
// `json::parse` (there are several, including one in the owner's own header) is
// not a call, and a guard that cannot tell them apart is a guard nobody can
// keep green.
//
// ⚠ TWO CONSTRUCTS THAT LOOK LIKE STRING SYNTAX AND ARE NOT, BOTH ✔MEASURED
// PRESENT IN `src/` (38 files and 4 files respectively). The first cut of this
// function mishandled both, and one of them fails toward *clean* — the
// direction that would make this whole guard a decoration:
//
//   RAW STRINGS `R"tag(...)tag"` — 38 files. The content may contain a bare
//   `"`, at which a naive scan ends the literal early and resumes "code"
//   scanning inside the text. The delimiter is therefore parsed properly.
//
//   ★ DIGIT SEPARATORS `1'000'000` — 4 files, and THIS is the dangerous one. A
//   naive scan reads the first `'` as a char-literal opener and blanks
//   everything up to the next apostrophe ANYWHERE later in the file. With an
//   odd number of them, that silently erases a large region — and a region
//   containing a real `json::parse` is then reported CLEAN. So an apostrophe
//   sitting between two digits is a separator, never a literal; the prefixed
//   forms (`L'a'`, `u8'a'`) still open literals because the character before
//   them is not a digit.
std::string blankCommentsAndLiterals(std::string_view src) {
    std::string out{src};
    std::size_t i = 0;
    auto blankTo = [&](std::size_t from, std::size_t to) {
        for (std::size_t k = from; k < to && k < out.size(); ++k) {
            if (out[k] != '\n') out[k] = ' ';
        }
    };
    while (i < out.size()) {
        if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '/') {
            std::size_t const end = out.find('\n', i);
            blankTo(i, end == std::string::npos ? out.size() : end);
            i = (end == std::string::npos) ? out.size() : end;
        } else if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '*') {
            std::size_t const end = out.find("*/", i + 2);
            std::size_t const stop =
                (end == std::string::npos) ? out.size() : end + 2;
            blankTo(i, stop);
            i = stop;
        } else if (out[i] == 'R' && i + 1 < out.size() && out[i + 1] == '"') {
            // R"tag( ... )tag" — the tag is everything up to the first '('.
            std::size_t const open = out.find('(', i + 2);
            if (open == std::string::npos) { ++i; continue; }
            std::string const closer = ")" + out.substr(i + 2, open - (i + 2)) + "\"";
            std::size_t const end = out.find(closer, open + 1);
            std::size_t const stop =
                (end == std::string::npos) ? out.size() : end + closer.size();
            blankTo(i, stop);
            i = stop;
        } else if (out[i] == '\'' && i > 0 && isDigitChar(out[i - 1])
                   && i + 1 < out.size() && isIdentChar(out[i + 1])) {
            ++i;  // a digit separator, not a literal — see the note above
        } else if (out[i] == '"' || out[i] == '\'') {
            char const quote = out[i];
            std::size_t j = i + 1;
            while (j < out.size() && out[j] != quote) {
                if (out[j] == '\\') ++j;
                ++j;
            }
            blankTo(i, std::min(j + 1, out.size()));
            i = std::min(j + 1, out.size());
        } else {
            ++i;
        }
    }
    return out;
}

// Identifiers declared in this file as a `json` (or `nlohmann::json`), however
// spelled: `json doc;`, `json const doc =`, `nlohmann::json j{...}`. These are
// the only names an `operator>>` can turn into a parsed document.
std::set<std::string> jsonTypedNames(std::string const& text) {
    std::set<std::string> names;
    for (std::size_t at = text.find("json"); at != std::string::npos;
         at = text.find("json", at + 1)) {
        if (at > 0 && isIdentChar(text[at - 1])) continue;   // `myjson`
        std::size_t i = at + 4;
        if (i < text.size() && isIdentChar(text[i])) continue;  // `jsonText`
        // skip qualifiers between the type and the name
        while (true) {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
            if (text.compare(i, 6, "const ") == 0) { i += 6; continue; }
            if (text.compare(i, 1, "&") == 0 || text.compare(i, 1, "*") == 0) {
                ++i; continue;
            }
            break;
        }
        std::size_t const start = i;
        while (i < text.size() && isIdentChar(text[i])) ++i;
        if (i == start) continue;
        std::size_t j = i;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
        if (j < text.size()
            && (text[j] == ';' || text[j] == '=' || text[j] == '{'
                || text[j] == ',' || text[j] == ')')) {
            names.insert(text.substr(start, i - start));
        }
    }
    return names;
}

// Every JSON-ingestion spelling in one source text. THE instrument tier 3 rests
// on; its own fixtures are below.
std::vector<std::string> jsonIngestionSitesIn(std::string_view rawText) {
    std::string const text = blankCommentsAndLiterals(rawText);
    std::vector<std::string> found;
    for (char const* spelling : {"json::parse", "sax_parse"}) {
        if (text.find(spelling) != std::string::npos) found.emplace_back(spelling);
    }
    auto const names = jsonTypedNames(text);
    for (std::size_t at = text.find(">>"); at != std::string::npos;
         at = text.find(">>", at + 1)) {
        std::size_t i = at + 2;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        std::size_t const start = i;
        while (i < text.size() && isIdentChar(text[i])) ++i;
        if (i > start && names.count(text.substr(start, i - start)) != 0) {
            found.emplace_back(">> " + text.substr(start, i - start));
        }
    }
    return found;
}

} // namespace

TEST(ConfigDocumentParseIsTheOneOwner, TheScannerSeesEachSpelling) {
    EXPECT_FALSE(jsonIngestionSitesIn("doc = json::parse(text);").empty());
    EXPECT_FALSE(
        jsonIngestionSitesIn("nlohmann::json::sax_parse(in, &r);").empty());
    EXPECT_FALSE(jsonIngestionSitesIn("json doc;\n in >> doc;").empty());
    EXPECT_FALSE(
        jsonIngestionSitesIn("nlohmann::json const j = {};\n s >> j;").empty());
}

TEST(ConfigDocumentParseIsTheOneOwner, TheScannerIsNotFooledByProseOrText) {
    EXPECT_TRUE(jsonIngestionSitesIn("// json::parse is mentioned here").empty());
    EXPECT_TRUE(jsonIngestionSitesIn("/* a json::parse in a block */").empty());
    EXPECT_TRUE(jsonIngestionSitesIn(R"(log("json::parse");)").empty());
    EXPECT_TRUE(jsonIngestionSitesIn("int width = 0;\n value >> width;").empty())
        << "a shift into a non-json variable is not JSON ingestion";
    EXPECT_TRUE(jsonIngestionSitesIn("std::string jsonText;\n in >> jsonText;")
                    .empty())
        << "`jsonText` is not a `json`; matching it would make the guard "
           "unkeepable and it would be silenced rather than fixed";
}

// ★ THE TWO CONSTRUCTS THAT LOOK LIKE STRING SYNTAX AND ARE NOT, both present
// in `src/`. The digit-separator arm is the load-bearing one: it fails toward
// CLEAN, so without it a guard that had stopped seeing anything would still
// report a tidy tree.
TEST(ConfigDocumentParseIsTheOneOwner, TheScannerSurvivesRawStringsAndDigitSeparators) {
    // A raw string whose CONTENT carries a bare `"`. A naive scan ends the
    // literal at that quote and resumes reading its text as code.
    EXPECT_TRUE(jsonIngestionSitesIn(
                    "auto s = R\"(he said \"json::parse\" loudly)\";").empty())
        << "text inside a raw string is not a call";
    // ...and code AFTER such a raw string must still be visible.
    EXPECT_FALSE(jsonIngestionSitesIn(
                     "auto s = R\"(a \" b)\";\n doc = json::parse(t);").empty())
        << "the raw string swallowed the code that followed it";

    // An ODD number of digit separators. A naive scan opens a char literal at
    // the first apostrophe and blanks everything to the next one — here, past
    // the real call.
    EXPECT_FALSE(jsonIngestionSitesIn(
                     "constexpr int k = 1'000;\n doc = json::parse(t);\n"
                     "constexpr int m = 2'000'000;").empty())
        << "a digit separator was read as a char literal and blanked the call — "
           "this failure is SILENT and reports a clean tree";
    // A genuine char literal must still be blanked, prefixes included.
    EXPECT_TRUE(jsonIngestionSitesIn("char c = 'x';  // json::parse").empty());
    EXPECT_TRUE(jsonIngestionSitesIn("auto w = L'\\'';").empty());
}

TEST(ConfigDocumentParseIsTheOneOwner, NoOtherWayToTurnConfigBytesIntoAJson) {
    // ── NOT CONFIG DOCUMENTS. Permanent, each with its reason. ──
    static std::set<std::string> const kNotConfigDocuments{
        // LSP wire-protocol messages: a request body from an editor, not a
        // document that declares compiler behaviour. Refusing a repeat there is
        // a protocol decision with a different owner and a different failure
        // mode (a dropped request, not a different program).
        "src/lsp/json_rpc.cpp",
        "src/lsp/lsp_server.cpp",
        // `declaredFileExtensionsOf` — a STREAMING scan that reads a language
        // document's `language` block and deliberately ABORTS there, without
        // building a document at all. Routing it through a whole-document parse
        // would undo the very thing it exists for.
        "src/program/program.cpp",
    };
    // ── CONFIG-CLASS READERS NOT YET ROUTED. Each is a GAP, not a blessing. ──
    // ★★ EMPTY, AND THAT IS THE POINT — every config-class reader in `src/` is
    // routed through the one owner. It held ONE entry when this test was
    // written: `DependencyLockfile::load`, deferred only because
    // `src/program/**` was held by a sibling lane for cycle P46. That lane
    // exited without taking it, so the orchestrator closed it in the same
    // cycle rather than letting the gap outlive the lane that found it, and
    // the loop below then DEMANDED this entry's deletion — a listed gap whose
    // file no longer ingests JSON reds with "delete the entry".
    // ⚠ KEEP THE SET AND THE LOOP even while empty: an empty allowlist is a
    // measured claim that nothing is unrouted, and it is what makes the NEXT
    // unrouted reader a red rather than a silent addition.
    static std::set<std::string> const kUnroutedConfigReaders{};

    auto const src = dss::test::repoRoot() / "src";
    ASSERT_TRUE(std::filesystem::is_directory(src)) << src.string();

    std::vector<std::string> unexpected;
    std::set<std::string>    seenExceptions;
    std::size_t              scanned = 0;

    for (auto const& entry : std::filesystem::recursive_directory_iterator(src)) {
        if (!entry.is_regular_file()) continue;
        auto const ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") continue;
        ++scanned;

        auto const relative =
            std::filesystem::relative(entry.path(), dss::test::repoRoot())
                .generic_string();
        if (relative == "src/core/types/config_document_parse.cpp") continue;

        auto const sites = jsonIngestionSitesIn(readWholeFile(entry.path()));
        if (sites.empty()) continue;

        if (kNotConfigDocuments.count(relative) != 0
            || kUnroutedConfigReaders.count(relative) != 0) {
            seenExceptions.insert(relative);
            continue;
        }
        std::string what = relative + " —";
        for (auto const& s : sites) what += " `" + s + "`";
        unexpected.push_back(std::move(what));
    }

    EXPECT_GT(scanned, 400u)
        << "the scan found almost no sources — it is measuring the wrong "
           "directory, and an empty scan reports a clean tree";

    EXPECT_TRUE(unexpected.empty())
        << "a JSON document is being ingested outside "
           "`dss::detail::parseConfigDocument`. Every config-document reader "
           "routes through that ONE function so a REPEATED KEY is refused with "
           "the JSON pointer of the offending key "
           "(D-CONFIG-A-DUPLICATE-JSON-KEY-IS-DROPPED-WITHOUT-A-DIAGNOSTIC): a "
           "raw parse silently keeps the LAST declaration and compiles a "
           "different program. Route it, or — if it genuinely reads something "
           "that is not a config document — add it to `kNotConfigDocuments` "
           "WITH its reason.\nOffenders:\n"
        << [&] {
               std::string s;
               for (auto const& u : unexpected) s += "  " + u + "\n";
               return s;
           }();

    // Every named exception must still BE one. An entry whose file no longer
    // ingests JSON is a stale licence sitting in a guard, and the next reader
    // to add a parse to that file inherits it silently.
    for (auto const& allowed : kNotConfigDocuments) {
        EXPECT_TRUE(seenExceptions.count(allowed) != 0)
            << allowed << " is listed in `kNotConfigDocuments` but no longer "
                          "ingests JSON — delete the entry";
    }
    for (auto const& gap : kUnroutedConfigReaders) {
        EXPECT_TRUE(seenExceptions.count(gap) != 0)
            << gap << " is listed as an UNROUTED config reader but no longer "
                      "ingests JSON — delete the entry";
    }
}
