// ── D-CONFIG-GRAMMAR-LOADER-KEY-SHAPE-SENTENCES-RETYPE-THEIR-VOCABULARIES ──
//    D-TEXT-TIER-READERS-KEEP-HAND-WRITTEN-FROMNAME-IF-CHAINS
// ─────────────────────────────────────────────────────────────────────────
//
// THE CLASS, ON ITS TWO HALVES. A closed vocabulary is decided by a table (a
// `kXxxKeys` array, an `EnumNameTable`, a `…FromName` if-chain) and the sentence
// beside the check states the set as a STRING LITERAL. Two owners of one fact.
// The rows keep working while the sentence quietly becomes a lie, and the
// sentence is the half a config author reads: they are told BY NAME that a key
// the loader ACCEPTS is not allowed, or that one it refuses is fine. A test that
// asserts the sentence CONTAINS a name stays green the entire way down, which is
// why neither pin below ever compares against a written-down set.
//
// ★★★ WHERE THE EXPECTED SET COMES FROM, BECAUSE THAT IS THE WHOLE DESIGN.
// Nothing here retypes a vocabulary. Both halves take the set OUT OF THE LOADER
// and compare it against ANOTHER THING THE LOADER SAID:
//
//   PART 1 (the KEY half) — a block's SHAPE sentence (emitted when the block is
//   not an object) is compared against the ALLOWED-KEY LIST that
//   `dss::detail::rejectUnknownKeys` renders directly off the block's closed-key
//   table (emitted when the block carries a bogus key). Add a key to the table
//   and the second list grows; if the shape sentence is a literal rather than a
//   projection it does NOT, and the pin reds. That is the property — "the
//   sentence names every key the check accepts" — decided per site, with no
//   third copy of the set anywhere.
//
//   PART 2 (the VALUE half) — a refusal advertises a set; every spelling it
//   advertises is then FED BACK to the same reader and must be ACCEPTED.
//   ★★ THIS IS THE HALF A ROUND-TRIP TEST CANNOT DO. `.dsshir` and `.dssir` are
//   write-then-read surfaces, so the obvious pin is emit→parse→compare — and it
//   is blind by construction to any spelling the WRITER never emits. ✔MEASURED
//   in this tree: `fmtShader` emits `builtin` only when the value is not `None`,
//   `fmtTranspile` emits `idiom` only when not `Default`, and `fmtDiag` emits
//   `recovery` only when not `None`, so three sentinel spellings the READER
//   accepts are never produced by the writer at all. Driving the reader from the
//   MESSAGE reaches them; driving it from the writer's output never can.
//
// ✔MEASURED LIVE DRIFTS THIS FILE WAS WRITTEN FOR (all four found by reading the
// check beside the sentence, before either was changed):
//   * `checkReferencedDocumentBlocks` said a referenced document "may declare
//     only" the SEVEN merged/reference keys — while the same loop also lets
//     through the EIGHT standalone-surface keys (`tokens`, `lexerModes`,
//     `assembly`, …). 7 advertised, 15 accepted.
//   * the linkage-effect shape sentence named `'binding'`/`'visibility'` as the
//     only fields — 2 of the 4 keys its own closed-key table accepts, and the
//     two it omitted are BOOLEANS, so "string fields" was wrong for them too.
//   * the `semantics.builtinTypes` refusal retyped the core set as
//     `Bool/I*/U*/F*/Char/Byte/Void`: it omits `NullptrT`, which the check
//     accepts, and `I*`/`U*`/`F*` name no spelling at all.
//   * every `orMalformed` refusal in the `.dsshir` reader, and the `lowering`
//     refusal in the grammar loader, named NO accepted set whatsoever.
//
// ⚠ MUST run through ctest, never a bare `.exe`: the shipped-config resolver
// walks the cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets
// it, so a bare run reads whichever tree the shell happens to stand in.

#include "core/types/config_path_walk.hpp"  // findShippedConfig / ShippedConfigLocator
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_registry.hpp"  // TypeRegistry (parseTypeFromText)
#include "hir/hir_text.hpp"
#include "mir/mir_text.hpp"

#include "vocabulary_message_probe.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ::dss::CompilationUnitId;
using ::dss::DiagnosticCode;
using ::dss::DiagnosticReporter;
using ::dss::findShippedConfig;
using ::dss::GrammarSchema;
using ::dss::ShippedConfigLocator;

// A spelling no vocabulary in this tree claims. Deliberately unlikely: a name
// that collided with a real one would make every negative arm below pass for the
// wrong reason.
constexpr char const* kBadSpelling = "zzNotAnyVocabularySpelling";

// A fixture fault — never a verdict about the subject. THROWN, never returned:
// a fixture fault reported through the loader's own diagnostic envelope is
// indistinguishable from the refusal these rows exist to read, and every one of
// them would go green over a document that was never mutated.
class ProbeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] nlohmann::json shippedLanguageDoc(std::string_view name) {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{name, "sources", ".lang.json", "language",
                             DiagnosticCode::C_InvalidLanguageName});
    if (!pathR.has_value()) {
        throw ProbeError{std::string{"cannot resolve shipped language document '"} +
                         std::string{name} + "'"};
    }
    std::ifstream in{*pathR};
    if (!in.is_open()) {
        throw ProbeError{std::string{"cannot open "} + pathR->string()};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return nlohmann::json::parse(buf.str());
}

// Resolve `pointer` and REQUIRE that it currently holds an object. Fail-closed
// on every way the probe could silently do nothing: an unresolvable pointer (the
// block moved, an index shifted) or a node that was never an object, either of
// which would make the shape probe below assert nothing.
[[nodiscard]] nlohmann::json* objectAt(nlohmann::json& doc,
                                       std::string_view pointer,
                                       std::string_view where,
                                       bool             created = false) {
    nlohmann::json* node = nullptr;
    if (created) {
        auto const ptr = nlohmann::json::json_pointer{std::string{pointer}};
        if (doc.contains(ptr)) {
            throw ProbeError{std::string{"["} + std::string{where} + "] '" +
                             std::string{pointer} +
                             "' already exists, so the row's `created` flag is "
                             "wrong — point it at the shipped block instead of "
                             "manufacturing a second one."};
        }
        doc[ptr] = nlohmann::json::object();
        return &doc[ptr];
    }
    try {
        node = &doc.at(nlohmann::json::json_pointer{std::string{pointer}});
    } catch (std::exception const& e) {
        throw ProbeError{std::string{"["} + std::string{where} +
                         "] the JSON pointer '" + std::string{pointer} +
                         "' does not resolve in the shipped document (" + e.what() +
                         "). A row whose pointer went stale asserts NOTHING — "
                         "re-aim it at the block it names."};
    }
    if (!node->is_object()) {
        throw ProbeError{std::string{"["} + std::string{where} + "] '" +
                         std::string{pointer} + "' is a " + node->type_name() +
                         ", not an object — this row is aimed at the wrong node."};
    }
    return node;
}

// ★ `quotedTokens` used to be a file-local copy here, byte-identical to the one
// `tests/core/vocabulary_projection_probe.hpp` already owned — and therefore
// invisible to the mutant that closed
// D-TEST-VOCABULARY-PROJECTION-PROBE-HELPERS-ARE-COPIED-PER-FILE. It has ONE
// owner now, `tests/test_support/vocabulary_message_probe.hpp`, which is
// json-free and on every test target's include path; see
// D-TEST-VOCABULARY-PROBE-MESSAGE-HALF-IS-UNREACHABLE-AND-JSON-COUPLED.
using ::dss::test_support::quotedTokens;

template <typename Diags>
[[nodiscard]] std::string summarize(Diags const& diags) {
    std::string s;
    for (auto const& d : diags) s += "\n  " + d.path + ": " + d.message;
    return s.empty() ? std::string{"<no diagnostics>"} : s;
}

// The first diagnostic whose message carries `needle`, or empty.
template <typename Diags>
[[nodiscard]] std::string messageContaining(Diags const& diags,
                                            std::string_view needle) {
    for (auto const& d : diags) {
        if (d.message.find(needle) != std::string::npos) return d.message;
    }
    return {};
}

// Load a mutated document and hand back its diagnostics. The load MUST fail:
// every probe below is a deliberate malformation, and a clean load would mean
// the mutation never reached the loader.
template <typename Fn>
[[nodiscard]] std::vector<::dss::ConfigDiagnostic>
loadMutated(std::string_view language, std::string_view where, Fn&& mutate) {
    auto doc = shippedLanguageDoc(language);
    mutate(doc);
    auto r = GrammarSchema::loadFromText(doc.dump(), language);
    if (r.has_value()) {
        throw ProbeError{std::string{"["} + std::string{where} +
                         "] the mutated document LOADED CLEAN — the probe never "
                         "reached the check it is aimed at, so nothing below is "
                         "evidence about the message."};
    }
    return r.error();
}

// ── PART 1: the KEY half ────────────────────────────────────────────────

// One closed-key block, named by where it lives in a SHIPPED document. Both
// probes hit the SAME pointer, so the two messages are guaranteed to be about
// the same block — a pair aimed at two different blocks would compare two
// unrelated sets and read as green.
struct KeyShapeBlock {
    char const* language;
    char const* pointer;
    char const* label;        // the prose label the unknown-key refusal names
    char const* shapeNeedle;  // an invariant fragment of the SHAPE sentence
    // ★ THE ONE NON-VOCABULARY QUOTE. Every shape sentence opens by naming the
    // BLOCK — `'wrapperRules' must be an object …` — and that quote is the
    // block's IDENTITY, not one of its keys, so the honesty half below must not
    // read it as an advertised key. Carried EXACTLY rather than filtered by a
    // "looks like a block name" heuristic: a heuristic that swallowed a real key
    // would weaken the honesty check in precisely the direction it exists to
    // check. This is the `kIgnoreFormatKey` precedent from
    // `test_vocabulary_projection_ffi_and_lir.cpp`.
    char const* selfQuote;
    // ★ `requires` is OPTIONAL and no shipped document that can be loaded
    // STANDALONE declares one — the only document that does is `asm.lang.json`,
    // which is reachable only THROUGH a host, so its blocks cannot be mutated
    // via `loadFromText`. The block is therefore created rather than found. It
    // is still the real loader on a real document; what it is not is a block
    // some shipped file already carries, which is why the flag is explicit
    // rather than "create when missing" — a silently-created block would hide a
    // pointer that had gone stale.
    bool created = false;
};

// ✔MEASURED 2026-08-20 by resolving each pointer against the shipped documents;
// `objectAt` re-validates every one on every run, so a row that goes stale FAILS
// rather than passing vacuously.
constexpr KeyShapeBlock kKeyShapeBlocks[] = {
    {"c-subset", "/shapes/expression/expr/wrapperRules",
     "a 'wrapperRules' object", "'wrapperRules' must be an object",
     "wrapperRules"},
    {"c-subset", "/numberStyle/integerPrefixes/0/float",
     "a prefix's 'float' block", "'float' must be an object", "float"},
    {"asm-arm64-gas", "/numberStyle/emitKind",
     "the 'emitKind' block", "'numberStyle.emitKind' is required",
     "numberStyle.emitKind"},
    // The linkage effect is the one block whose sentence carries NO self-quote:
    // the object is keyed by specifier SOURCE TEXT, so it has no fixed name to
    // open with. The empty string here is a measured fact, not a placeholder.
    {"c-subset", "/semantics/declarations/0/linkageSpecifiers/static",
     "a linkage effect", "linkage effect must be an object", ""},
    {"c-subset", "/requires", "the 'requires' block",
     "'requires' must be an object", "requires", /*created=*/true},
};

// The allowed-key list `rejectUnknownKeys` renders off the block's table. It is
// bracketed by two fixed fragments of the SHARED sentence, so this reads the
// projection itself rather than guessing at the message's shape.
[[nodiscard]] std::vector<std::string> advertisedKeys(std::string const& msg) {
    constexpr std::string_view kOpen  = "allowed keys are ";
    constexpr std::string_view kClose = " (plus any";
    auto const a = msg.find(kOpen);
    if (a == std::string::npos) return {};
    auto const b = msg.find(kClose, a);
    if (b == std::string::npos) return {};
    return quotedTokens(msg.substr(a + kOpen.size(), b - a - kOpen.size()));
}

}  // namespace

// ── BASELINE ────────────────────────────────────────────────────────────
//
// The positive control. Without it, a loader that refused EVERYTHING would turn
// every probe below green.
TEST(KeyShapeSentences, EveryProbedShippedDocumentLoadsCleanly) {
    for (char const* lang : {"c-subset", "asm-arm64-gas"}) {
        auto const doc = shippedLanguageDoc(lang);
        auto       r   = GrammarSchema::loadFromText(doc.dump(), lang);
        ASSERT_TRUE(r.has_value())
            << lang << " must load clean before any probe means anything: "
            << summarize(r.error());
    }
}

// ★ THE PROPERTY: a block's SHAPE sentence names every key its closed-key check
// ACCEPTS. Both sets come out of the loader; neither is written down here.
TEST(KeyShapeSentences, ShapeSentenceNamesEveryKeyTheCheckAccepts) {
    for (auto const& b : kKeyShapeBlocks) {
        SCOPED_TRACE(std::string{b.language} + b.pointer);

        // (a) the ALLOWED-KEY LIST, rendered by `rejectUnknownKeys` from the
        //     block's own table.
        auto const keyDiags = loadMutated(
            b.language, b.pointer, [&](nlohmann::json& doc) {
                auto* node = objectAt(doc, b.pointer, b.pointer, b.created);
                if (node->contains(kBadSpelling)) {
                    throw ProbeError{"the probe key already exists — the "
                                     "injection would be a no-op"};
                }
                (*node)[kBadSpelling] = "probe";
            });
        auto const keyMsg = messageContaining(keyDiags, b.label);
        ASSERT_FALSE(keyMsg.empty())
            << "no unknown-key refusal named " << b.label
            << " — the probe hit some other check, which proves nothing about "
               "this block's vocabulary:"
            << summarize(keyDiags);
        auto const accepted = advertisedKeys(keyMsg);
        ASSERT_FALSE(accepted.empty())
            << "the unknown-key refusal for " << b.label
            << " carries no rendered allow-list, so this pin has nothing to "
               "compare against:\n"
            << keyMsg;

        // (b) the SHAPE sentence, reached by making the block a non-object.
        auto const shapeDiags = loadMutated(
            b.language, b.pointer, [&](nlohmann::json& doc) {
                auto* node = objectAt(doc, b.pointer, b.pointer, b.created);
                *node      = "probe-not-an-object";
            });
        auto const shapeMsg = messageContaining(shapeDiags, b.shapeNeedle);
        ASSERT_FALSE(shapeMsg.empty())
            << "the shape sentence carrying \"" << b.shapeNeedle
            << "\" never fired:" << summarize(shapeDiags);
        auto const named = quotedTokens(shapeMsg);

        // THE ASSERTION. Every key the check takes must be named by the shape
        // sentence — the direction all four measured drifts were wrong on.
        for (auto const& key : accepted) {
            EXPECT_NE(std::find(named.begin(), named.end(), key), named.end())
                << "the shape sentence for " << b.label << " does not name '"
                << key
                << "', a key the closed-key check ACCEPTS. A sentence narrower "
                   "than its check tells a config author, by name, that a key "
                   "the loader takes is not allowed.\nshape sentence:\n  "
                << shapeMsg << "\nallowed-key list:\n  " << keyMsg;
        }
        // ...and the mirror: it advertises nothing the check refuses. A key
        // named here but absent from the table would send an author to write a
        // key the very next check calls a typo.
        for (auto const& key : named) {
            if (key == b.selfQuote) continue;  // the block's own name
            EXPECT_NE(std::find(accepted.begin(), accepted.end(), key),
                      accepted.end())
                << "the shape sentence for " << b.label << " names '" << key
                << "', which the closed-key check REFUSES.\nshape sentence:\n  "
                << shapeMsg << "\nallowed-key list:\n  " << keyMsg;
        }
    }
}

// ── PART 2: the VALUE half ──────────────────────────────────────────────

namespace {

// Drive the grammar loader with a bogus value at `pointer`, then feed every
// spelling the refusal advertises back through the SAME field and require each
// one to be accepted.
//
// `pointer` addresses a STRING-valued field (a `core` name, a `lowering` name),
// not an object — so this uses its own resolver rather than `objectAt`.
void expectEveryAdvertisedValueIsAccepted(std::string_view language,
                                          std::string_view pointer,
                                          std::string_view needle,
                                          std::size_t      expectedCount,
                                          std::string_view label) {
    auto const setValue = [&](nlohmann::json& doc, std::string_view value) {
        nlohmann::json* node = nullptr;
        try {
            node = &doc.at(nlohmann::json::json_pointer{std::string{pointer}});
        } catch (std::exception const& e) {
            throw ProbeError{std::string{"["} + std::string{label} +
                             "] the JSON pointer '" + std::string{pointer} +
                             "' does not resolve (" + e.what() + ")"};
        }
        if (!node->is_string()) {
            throw ProbeError{std::string{"["} + std::string{label} + "] '" +
                             std::string{pointer} + "' is a " + node->type_name() +
                             ", not a string — this row is aimed at the wrong "
                             "field."};
        }
        *node = std::string{value};
    };

    auto const diags = loadMutated(
        language, label,
        [&](nlohmann::json& doc) { setValue(doc, kBadSpelling); });
    auto const msg = messageContaining(diags, needle);
    ASSERT_FALSE(msg.empty())
        << label << ": the refusal carrying \"" << needle << "\" never fired:"
        << summarize(diags);

    auto advertised = quotedTokens(msg);
    // The bogus spelling is echoed by the message; it is not vocabulary.
    std::erase(advertised, std::string{kBadSpelling});
    // ⚠ THE COUNT IS STATED, NOT DERIVED, AND IT GUARDS ONE THING: a SILENT
    // SHRINK. Everything else below walks whatever the message says, so a
    // projection that lost half its rows would still satisfy every loop.
    EXPECT_EQ(advertised.size(), expectedCount)
        << label << ": the advertised set changed size. This pin walks the set, "
                    "so it needs no edit when a row is ADDED — but a set that "
                    "got smaller means a spelling stopped being advertised, "
                    "which is the direction this class fails in.\nmessage:\n  "
        << msg;

    for (auto const& value : advertised) {
        SCOPED_TRACE(value);
        auto doc = shippedLanguageDoc(language);
        setValue(doc, value);
        auto r = GrammarSchema::loadFromText(doc.dump(), language);
        if (!r.has_value()) {
            EXPECT_TRUE(messageContaining(r.error(), needle).empty())
                << label << ": the refusal advertises '" << value
                << "' and then REFUSES it — a message wider than its check sends "
                   "a config author to write a value the loader will reject:"
                << summarize(r.error());
        }
    }
}

// Parse `.dsshir` text and hand back the first diagnostic carrying `needle`.
[[nodiscard]] std::string hirRefusal(std::string const& text,
                                     std::string_view   needle) {
    DiagnosticReporter r;
    auto const         res = dss::parseHir(text, CompilationUnitId{1}, r);
    (void)res;
    for (auto const& d : r.all()) {
        if (d.actual.find(needle) != std::string::npos) return d.actual;
    }
    return {};
}

[[nodiscard]] bool hirTextIsClean(std::string const& text) {
    DiagnosticReporter r;
    auto const         res = dss::parseHir(text, CompilationUnitId{1}, r);
    return res->ok && r.all().empty();
}

// One `.dsshir` attribute vocabulary: a text template with `{}` where the
// spelling goes, the `what` word its refusal uses, and the number of spellings
// its table holds.
struct HirAttrVocabulary {
    char const* label;
    char const* prefix;   // text before the spelling
    char const* suffix;   // text after it
    char const* what;     // the `unknown <what> '<name>'` word
    std::size_t rows;
};

[[nodiscard]] std::string hirModule(char const* prefix, std::string_view name,
                                    char const* suffix) {
    return std::string{prefix} + std::string{name} + suffix;
}

// Parse `.dssir` text and hand back the first diagnostic carrying `needle`.
[[nodiscard]] std::string mirRefusal(std::string const& text,
                                     std::string_view   needle) {
    DiagnosticReporter r;
    auto const         res = dss::parseMir(text, CompilationUnitId{1}, r);
    (void)res;
    for (auto const& d : r.all()) {
        if (d.actual.find(needle) != std::string::npos) return d.actual;
    }
    return {};
}

// A `.dssir` module carrying ONE global whose type/initializer the caller writes.
// Everything else is the minimum a module needs, so a diagnostic can only be
// about the hole.
[[nodiscard]] std::string mirGlobalModule(std::string_view globalTail) {
    return std::string{"dssir 1\n"
                       "symbols { %1 \"f\" %2 \"g\" }\n"
                       "module {\n"
                       "  global %2 : "} + std::string{globalTail} + "\n"
           "  function %1 : fn() -> void {\n"
           "    block %b0 [entry] {\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
}

// Every diagnostic a `.dssir` parse produced, for a failure message. A control
// that reds without saying what the reader objected to costs a rebuild to read.
[[nodiscard]] std::string mirDiagnostics(std::string const& text) {
    DiagnosticReporter r;
    auto const         res = dss::parseMir(text, CompilationUnitId{1}, r);
    (void)res;
    std::string out;
    for (auto const& d : r.all()) out += "\n  " + d.actual;
    return out.empty() ? std::string{"<no diagnostics>"} : out;
}

}  // namespace

// The `semantics.builtinTypes[].core` refusal — the one that retyped its set as
// `Bool/I*/U*/F*/Char/Byte/Void` and omitted `NullptrT`.
TEST(TextTierVocabulary, CoreTypeRefusalNamesEverySpellingTheLoaderAccepts) {
    expectEveryAdvertisedValueIsAccepted(
        "c-subset", "/semantics/builtinTypes/0/core", "unknown core TypeKind",
        20, "semantics.builtinTypes[0].core");
}

// The `builtinFunctions[].lowering` refusal — thirty verbs, and it named none.
TEST(TextTierVocabulary, BuiltinLoweringRefusalNamesEveryVerbTheLoaderAccepts) {
    expectEveryAdvertisedValueIsAccepted(
        "c-subset", "/semantics/builtinFunctions/0/lowering",
        "unknown builtin lowering", 30, "semantics.builtinFunctions[0].lowering");
}

// ★★ THE `.dsshir` ATTRIBUTE VOCABULARIES. Six sets that each used to exist
// twice in `hir_text.cpp` — a writer switch and a reader if-chain. The refusals
// named no accepted set at all, so this pin could not have been written before
// the projection existed: there was nothing to read.
TEST(TextTierVocabulary, EveryHirAttributeRefusalAdvertisesExactlyWhatItAccepts) {
    constexpr HirAttrVocabulary kVocabularies[] = {
        {"ffi linkage",
         "dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n  @ffi(link ",
         ")\n  extern_global %1 : i32\n}\n", "unknown ffi linkage", 3},
        {"ffi visibility",
         "dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n  @ffi(vis ",
         ")\n  extern_global %1 : i32\n}\n", "unknown ffi visibility", 3},
        {"shader stage",
         "dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n  @shader(stage ",
         ")\n  extern_global %1 : i32\n}\n", "unknown shader stage", 7},
        {"shader builtin",
         "dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n  @shader(builtin ",
         ")\n  extern_global %1 : i32\n}\n", "unknown shader builtin", 12},
        {"transpile idiom",
         "dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n  @transpile(idiom ",
         ")\n  extern_global %1 : i32\n}\n", "unknown transpile idiom", 6},
        {"diag recovery",
         "dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n  @diag(code 0, recovery ",
         ")\n  extern_global %1 : i32\n}\n", "unknown diag recovery", 4},
    };

    for (auto const& v : kVocabularies) {
        SCOPED_TRACE(v.label);
        auto const msg =
            hirRefusal(hirModule(v.prefix, kBadSpelling, v.suffix), v.what);
        ASSERT_FALSE(msg.empty())
            << v.label
            << ": the reader did not refuse an unrecognized spelling by name — "
               "an attribute that silently coerces to a default loses the "
               "attribute on every round trip.";
        auto advertised = quotedTokens(msg);
        std::erase(advertised, std::string{kBadSpelling});
        EXPECT_EQ(advertised.size(), v.rows)
            << v.label
            << ": the advertised set changed size. A SHRINK means a spelling the "
               "reader takes stopped being advertised.\nmessage:\n  "
            << msg;

        // ★★ THE HALF A ROUND-TRIP PIN CANNOT REACH: every advertised spelling
        // is fed back to the reader, INCLUDING the sentinels the writer never
        // emits (`builtin none`, `idiom default`, `recovery none`).
        for (auto const& name : advertised) {
            SCOPED_TRACE(name);
            EXPECT_TRUE(hirTextIsClean(hirModule(v.prefix, name, v.suffix)))
                << v.label << ": the refusal advertises '" << name
                << "' and the reader then REJECTS it — a message wider than its "
                   "check hands an author a spelling that does not parse.";
        }
    }
}

// ★★ THE `.dssir` BLOCK MARKER, and it is the fail-loud half rather than only
// the projection half. This arm used to have NO `else`: an unrecognized marker
// left the block at `Linear` and the parse SUCCEEDED, so a `.dssir` carrying a
// typo'd — or simply newer — marker loaded with the block's structural role
// silently erased, and the round trip returned a module that DIFFERED from the
// one written.
TEST(TextTierVocabulary, UnknownMirBlockMarkerIsRefusedAndNamesTheAcceptedSet) {
    // ⚠ THE MARKER GOES ON A SECOND BLOCK, never the entry. A function's FIRST
    // block is its entry block, so a single-block fixture can only ever carry
    // `entry` — it would prove the vocabulary claim for one of the twelve
    // spellings while reading as though it covered all of them.
    //
    // ★★★ AND THE TERMINATOR MUST BE A REAL MNEMONIC, WHICH IT WAS NOT.
    // ✔MEASURED 2026-08-21: this fixture spelled it `ret`. The mnemonic is
    // `return` (`mir_opcode.hpp`), so EVERY module this template built was refused
    // for `unknown opcode 'ret'` — and the `EXPECT_FALSE(res->ok)` below therefore
    // held no matter what the marker arm did. A refusal assertion whose fixture
    // never parsed asserts NOTHING, and it reads exactly like the strongest line
    // in the test. Found only because a sibling pin added a POSITIVE control and
    // the control reddened; the negative arm alone can never see this.
    auto const module = [](std::string_view marker) {
        return std::string{"dssir 1\n"
                           "symbols { %1 \"f\" }\n"
                           "module {\n"
                           "  function %1 : fn() -> void {\n"
                           "    block %b0 [entry] {\n"
                           "      br %b1\n"
                           "    }\n"
                           "    block %b1 ["} +
               std::string{marker} +
               "] {\n"
               "      return\n"
               "    }\n"
               "  }\n"
               "}\n";
    };

    // ★ THE POSITIVE CONTROL, which is what the `ret` typo above had left this
    // test without. `linear` is the marker the CFG derivation gives a block
    // reached by one unconditional `br`, and verify-on-load refuses any stored
    // marker that disagrees — so this is the one advertised spelling this fixture
    // can carry and still parse clean.
    {
        DiagnosticReporter ctl;
        auto const ok = dss::parseMir(module("linear"), CompilationUnitId{1}, ctl);
        EXPECT_TRUE(ok->ok)
            << "the fixture must parse clean with a REAL marker, or the refusal "
               "arm below is green for a reason that is not the marker:"
            << mirDiagnostics(module("linear"));
    }

    DiagnosticReporter r;
    auto const         res = dss::parseMir(module(kBadSpelling),
                                           CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok)
        << "an unrecognized block marker must FAIL the parse. Accepting it and "
           "keeping `Linear` drops the block's structural role with no "
           "diagnostic — a round trip that returns a different module.";
    std::string msg;
    for (auto const& d : r.all()) {
        if (d.actual.find("unknown block marker") != std::string::npos) {
            msg = d.actual;
            break;
        }
    }
    ASSERT_FALSE(msg.empty())
        << "the refusal did not name the marker vocabulary.";
    auto advertised = quotedTokens(msg);
    std::erase(advertised, std::string{kBadSpelling});
    EXPECT_EQ(advertised.size(), 12u)
        << "the advertised marker set changed size — a SHRINK means a marker the "
           "reader takes stopped being advertised.\nmessage:\n  "
        << msg;

    // ★ THE CLAIM IS ABOUT THE VOCABULARY AND IS SCOPED TO IT. Asserting the
    // whole module VERIFIES would entangle this pin with MIR's structural rules
    // — whether a `loopheader` on a block with no back-edge is well-formed is a
    // real question, and it is not this one. What must hold is that the reader
    // never calls an advertised spelling unknown.
    for (auto const& name : advertised) {
        SCOPED_TRACE(name);
        DiagnosticReporter r2;
        auto const res2 = dss::parseMir(module(name), CompilationUnitId{1}, r2);
        (void)res2;
        bool refusedByName = false;
        for (auto const& d : r2.all()) {
            if (d.actual.find("unknown block marker") != std::string::npos) {
                refusedByName = true;
                break;
            }
        }
        EXPECT_FALSE(refusedByName)
            << "the refusal advertises marker '" << name
            << "' and the reader then calls it UNKNOWN — a message wider than "
               "its check hands an author a spelling that does not parse.";
    }
}

// ★★ THE SAME SILENT ARM, ONE TIER DOWN AND WITH A WORSE CONSEQUENCE. A `.dssir`
// function type may carry `cc <name>`, and an unrecognized name used to leave
// `cc` at `CcSysV` with the parse SUCCEEDING — an ABI CHANGE (different argument
// registers, different stack discipline) applied silently to a signature, on a
// format whose entire contract is lossless round-trip. Found while converting the
// block-marker arm beside it, which is the reason it is pinned here rather than
// anchored for later.
TEST(TextTierVocabulary, UnknownMirCallingConventionIsRefusedAndNamesTheAcceptedSet) {
    auto const module = [](std::string_view cc) {
        return std::string{"dssir 1\n"
                           "symbols { %1 \"f\" }\n"
                           "module {\n"
                           "  function %1 : fn() -> void cc "} +
               std::string{cc} +
               " {\n"
               "    block %b0 [entry] {\n"
               "      return\n"
               "    }\n"
               "  }\n"
               "}\n";
    };

    // ★ THE POSITIVE CONTROL COMES FIRST, and it is what makes the refusal
    // assertion below mean anything at all: without it, a fixture that never
    // parsed for some UNRELATED reason satisfies `EXPECT_FALSE(res->ok)` while
    // proving nothing about the calling-convention arm. That is not hypothetical —
    // ✔MEASURED 2026-08-21, the block-marker fixture beside this one spelled its
    // terminator `ret` instead of `return`, so its refusal assertion had been
    // vacuous since the day it was written.
    {
        DiagnosticReporter ctl;
        auto const ok = dss::parseMir(module("ms64"), CompilationUnitId{1}, ctl);
        EXPECT_TRUE(ok->ok)
            << "the fixture must parse clean with a REAL convention, or the "
               "refusal arm below is green for a reason that is not the "
               "convention:" << mirDiagnostics(module("ms64"));
    }

    DiagnosticReporter r;
    auto const res = dss::parseMir(module(kBadSpelling), CompilationUnitId{1}, r);
    // ★★ THE REFUSAL, NOT MERELY THE SENTENCE. This line was missing and the
    // omission was the whole defect the test names: the arm below reads the
    // message and checks the advertised set, which stays true of a WARNING. Drop
    // the arm to a diagnostic that does not set `errors_` and every remaining
    // assertion here passes while the parse SUCCEEDS and the signature silently
    // reverts to SysV — the exact state this test exists to make impossible.
    EXPECT_FALSE(res->ok)
        << "an unrecognized calling convention must FAIL the parse, not merely "
           "produce a message. A module that loads with a silently-substituted "
           "ABI is the defect; the sentence is only how the author finds out.";
    std::string msg;
    for (auto const& d : r.all()) {
        if (d.actual.find("unknown calling convention") != std::string::npos) {
            msg = d.actual;
            break;
        }
    }
    ASSERT_FALSE(msg.empty())
        << "an unrecognized calling convention must be REFUSED BY NAME. Keeping "
           "the SysV default silently rewrites the function's ABI, and the "
           "reader hands back a module that differs from the one written.";
    auto advertised = quotedTokens(msg);
    std::erase(advertised, std::string{kBadSpelling});
    EXPECT_EQ(advertised.size(), 9u)
        << "the advertised calling-convention set changed size — a SHRINK means "
           "a convention the reader takes stopped being advertised.\nmessage:\n  "
        << msg;

    for (auto const& name : advertised) {
        SCOPED_TRACE(name);
        DiagnosticReporter r2;
        auto const res2 = dss::parseMir(module(name), CompilationUnitId{1}, r2);
        (void)res2;
        bool refusedByName = false;
        for (auto const& d : r2.all()) {
            if (d.actual.find("unknown calling convention") != std::string::npos) {
                refusedByName = true;
                break;
            }
        }
        EXPECT_FALSE(refusedByName)
            << "the refusal advertises calling convention '" << name
            << "' and the reader then calls it UNKNOWN.";
    }
}

// ── PART 2b: THE ARMS THE FIRST SWEEP WALKED PAST ───────────────────────
//
// ★★★ A SILENT DEFAULT IS A SHAPE, AND A FILE THAT CONTAINS ONE IS WORTH
// SEARCHING RATHER THAN A FILE TO FIX ONE LINE OF. The two arms pinned above
// (`.dssir` block marker, `.dssir` calling convention) were found one at a time,
// each while fixing the other. Reading BOTH text tiers end to end for the same
// shape — a chain over a closed vocabulary whose final arm is absent, or a
// comparison whose false branch is an unnamed default — found six more, and
// every one of them let a malformed or newer input parse SUCCESSFULLY with a
// value the text never said:
//
//   * `.dsshir` / `.dssir` `parseType`, the `enum` arm — an enum's underlying
//     TypeKind was serialized as a RAW ORDINAL. The HIR side bounded it by
//     `Count_` and kept `I32` outside that range; the MIR side had no check at
//     all and cast whatever integer it read. Both are now spelled by NAME off the
//     tier's own prim table, which is also what `core_type.hpp` says three times
//     over ("no TypeKind ordinal is serialized") and what the `wfloat` literal arm
//     had already written down.
//   * `.dssir` `parseLiteral`, the `bool` arm — `lv.value = (v.text == "true")`,
//     so EVERY other spelling became `false`. The `.dsshir` twin already refused
//     it: two readers of one format disagreeing about what a boolean is.
//   * `.dssir` `parseLiteral`, the core-kind ladder — no final arm, so an
//     unrecognized core left `Void`. Reachable FROM THIS COMPILER'S OWN WRITER,
//     which renders a bare `?` for any kind it has no spelling for.
//   * `.dssir` `parseInstruction`, the `switch` arm — `if (sawDefault)` with no
//     `else` DROPPED THE TERMINATOR, and a dropped terminator is not a silent
//     wrong answer, it is a process ABORT two steps later in `finish()`.
//   * `.dssir` `parsePercentValue` — a `%` handle whose class letter is followed
//     by no digits (`%b`, `%vx`) fell out as handle ZERO, which is a live slot.
//   * the `.dssir` function-attribute refusal RETYPED both of its closed sets as
//     string literals, in the same file and the same cycle that converted the two
//     arms above it.


// The BASELINE, and it is not decoration: every arm below reads a diagnostic out
// of a module built from these two templates, and a template that never parsed
// would make the negative arms green for the wrong reason and the positive
// controls red for it.
TEST(TextTierVocabulary, TheProbeMirModulesParseCleanly) {
    for (char const* tail : {"i32 = lit int 0 : i32", "bool = lit bool true : bool",
                             "i32 = zero"}) {
        SCOPED_TRACE(tail);
        DiagnosticReporter r;
        auto const res = dss::parseMir(mirGlobalModule(tail), CompilationUnitId{1}, r);
        EXPECT_TRUE(res->ok) << "the probe module must parse clean:"
                             << mirDiagnostics(mirGlobalModule(tail));
    }
}

// ★★ THE ENUM UNDERLYING TYPE, ON BOTH TIERS. The MIR reader cast an arbitrary
// integer to `TypeKind` with NO range check; the HIR reader had a range check and
// no refusal, which is the more dangerous of the two because it LOOKS validated.
TEST(TextTierVocabulary, UnknownMirEnumUnderlyingTypeIsRefusedAndNamesTheAcceptedSet) {
    DiagnosticReporter r;
    auto const         res = dss::parseMir(
        mirGlobalModule(std::string{"enum \"E\" : "} + kBadSpelling + " = zero"),
        CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok)
        << "an unrecognized enum underlying type must FAIL the parse. Keeping the "
           "I32 default silently resizes every enumerator of that type.";

    auto const msg = mirRefusal(
        mirGlobalModule(std::string{"enum \"E\" : "} + kBadSpelling + " = zero"),
        "unknown enum underlying type");
    ASSERT_FALSE(msg.empty()) << "the refusal did not name the type vocabulary.";
    auto advertised = quotedTokens(msg);
    std::erase(advertised, std::string{kBadSpelling});
    EXPECT_EQ(advertised.size(), 19u)
        << "the advertised .dssir primitive set changed size — a SHRINK means a "
           "spelling the reader takes stopped being advertised.\nmessage:\n  "
        << msg;

    // HONESTY: every spelling the message advertises must be one the reader then
    // accepts. Scoped to the vocabulary claim, exactly as the marker pin is: what
    // must hold is that the reader never calls an advertised spelling unknown.
    for (auto const& name : advertised) {
        SCOPED_TRACE(name);
        EXPECT_TRUE(mirRefusal(
                        mirGlobalModule(std::string{"enum \"E\" : "} + name + " = zero"),
                        "unknown enum underlying type")
                        .empty())
            << "the refusal advertises '" << name
            << "' and the reader then calls it UNKNOWN.";
    }
}

// The HIR twin, driven through `parseTypeFromText` rather than the module parser
// — that is the SHIPPED entry point (the FFI shipped-descriptor reader and the
// builtin `signatureText` decoder both call it), so this arm pins the decoder a
// config author actually reaches.
TEST(TextTierVocabulary, UnknownHirEnumUnderlyingTypeIsRefusedByTheShippedTypeDecoder) {
    auto const decode = [](std::string const& typeText) {
        DiagnosticReporter r;
        dss::TypeInterner  interner{CompilationUnitId{1}};
        dss::TypeRegistry  reg;
        dss::TypeId const  t = dss::parseTypeFromText(typeText, interner, reg, r);
        std::string        msg;
        for (auto const& d : r.all()) {
            if (d.actual.find("unknown enum underlying type") != std::string::npos) {
                msg = d.actual;
                break;
            }
        }
        return std::pair{t, msg};
    };

    auto const [bad, msg] =
        decode(std::string{"enum \"E\" : "} + kBadSpelling);
    EXPECT_FALSE(bad.valid())
        << "an undecodable type must come back InvalidType. Returning an enum "
           "whose underlying kind was silently defaulted hands the FFI layer a "
           "type the descriptor never named.";
    ASSERT_FALSE(msg.empty()) << "the refusal did not name the type vocabulary.";
    auto advertised = quotedTokens(msg);
    std::erase(advertised, std::string{kBadSpelling});
    EXPECT_EQ(advertised.size(), 20u)
        << "the advertised .dsshir primitive set changed size — a SHRINK means a "
           "spelling the reader takes stopped being advertised.\nmessage:\n  "
        << msg;

    for (auto const& name : advertised) {
        SCOPED_TRACE(name);
        EXPECT_TRUE(decode(std::string{"enum \"E\" : "} + name).second.empty())
            << "the refusal advertises '" << name
            << "' and the reader then calls it UNKNOWN.";
    }
}

// ★ THE BOOLEAN, AND THE POINT IS THAT `false` IS A LEGAL VALUE. Nothing
// downstream can tell a literal that WAS `false` from one that was a garbled
// token, so this arm could never be caught anywhere but here.
TEST(TextTierVocabulary, UnknownMirBoolLiteralSpellingIsRefused) {
    DiagnosticReporter r;
    auto const         res = dss::parseMir(
        mirGlobalModule(std::string{"bool = lit bool "} + kBadSpelling + " : bool"),
        CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok)
        << "a bool literal spelled anything but 'true'/'false' must FAIL the "
           "parse. Silently taking `false` is indistinguishable downstream from a "
           "literal that really was false.";
    EXPECT_FALSE(mirRefusal(
                     mirGlobalModule(std::string{"bool = lit bool "} + kBadSpelling
                                     + " : bool"),
                     "expected 'true' or 'false'")
                     .empty())
        << "the refusal did not name what it wanted.";

    // The CONTROL: both real spellings still parse, so the arm above refuses a
    // spelling rather than refusing the construct.
    for (char const* good : {"true", "false"}) {
        SCOPED_TRACE(good);
        DiagnosticReporter r2;
        auto const         ok = dss::parseMir(
            mirGlobalModule(std::string{"bool = lit bool "} + good + " : bool"),
            CompilationUnitId{1}, r2);
        EXPECT_TRUE(ok->ok) << "a well-formed bool literal must still parse:"
                            << mirDiagnostics(mirGlobalModule(
                                   std::string{"bool = lit bool "} + good + " : bool"));
    }
}

// ★★ THE LITERAL CORE, AND IT IS REACHABLE FROM THE WRITER. `appendLiteral`
// renders a bare `?` for any core kind this format has no spelling for (a
// `_BitInt` literal core is one — `hir_to_mir` sets it), so the reader's missing
// final arm meant this compiler's OWN output read back with the core changed to
// `Void` and nothing said so.
TEST(TextTierVocabulary, UnknownMirLiteralCoreIsRefusedAndNamesTheAcceptedSet) {
    auto const text = mirGlobalModule(std::string{"i32 = lit int 0 : "} + kBadSpelling);
    DiagnosticReporter r;
    auto const         res = dss::parseMir(text, CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok)
        << "an unrecognized literal core must FAIL the parse. Leaving `Void` "
           "changes the literal's core on a format whose contract is a lossless "
           "round trip.";

    auto const msg = mirRefusal(text, "unknown literal core type");
    ASSERT_FALSE(msg.empty()) << "the refusal did not name the core vocabulary.";
    auto advertised = quotedTokens(msg);
    std::erase(advertised, std::string{kBadSpelling});
    // 19 primitive spellings + the 6 STRUCTURAL cores a literal may carry
    // (struct/union/array/ptr/ref/enum). The two sets live in two tables because
    // the structural kinds must NOT be reachable as bare TYPE keywords; the
    // message projects both, so this count notices either one shrinking.
    EXPECT_EQ(advertised.size(), 25u)
        << "the advertised literal-core set changed size — a SHRINK means a core "
           "spelling the reader takes stopped being advertised.\nmessage:\n  "
        << msg;

    for (auto const& name : advertised) {
        SCOPED_TRACE(name);
        EXPECT_TRUE(mirRefusal(mirGlobalModule(std::string{"i32 = lit int 0 : "} + name),
                               "unknown literal core type")
                        .empty())
            << "the refusal advertises core '" << name
            << "' and the reader then calls it UNKNOWN.";
    }

    // ★★★ AND THE SPELLING THE WRITER ACTUALLY EMITS, which is the one that made
    // this defect REACHABLE rather than merely possible. `appendLiteral` renders a
    // bare `?` for a core it has no keyword for, and `hir_to_mir` sets
    // `lit.core = TypeKind::BitInt` — a kind `kMirTextPrimTable` deliberately does
    // not carry. So `?` is not a typo an author might make; it is this compiler's
    // own output, and the reader used to take it as `Void` without a word.
    auto const fromTheWriter = mirGlobalModule("i32 = lit int 0 : ?");
    DiagnosticReporter wr;
    auto const         wres = dss::parseMir(fromTheWriter, CompilationUnitId{1}, wr);
    EXPECT_FALSE(wres->ok)
        << "the reader must refuse the '?' its own writer emits for an unspelled "
           "core — silently reading it as `Void` is how a round trip returns a "
           "module that differs from the one written.";
    EXPECT_FALSE(mirRefusal(fromTheWriter, "unknown literal core type").empty())
        << "the '?' placeholder must be refused BY NAME.";
}

// ★★ A DROPPED TERMINATOR IS NOT A SILENT WRONG ANSWER — IT IS AN ABORT TWO
// STEPS LATER. With no `default ->` arm the switch instruction was never built,
// `errors_` stayed false, and the unterminated block reached `MirBuilder::
// finish()`, which aborts the process. `a refusal that crashes is not a refusal`
// is this file's own rule, written beside the inline-asm arm.
TEST(TextTierVocabulary, MirSwitchWithoutADefaultArmIsRefused) {
    auto const module = [](char const* arms, char const* extraBlock) {
        return std::string{"dssir 1\n"
                           "symbols { %1 \"f\" }\n"
                           "module {\n"
                           "  function %1 : fn() -> void {\n"
                           "    block %b0 [entry] {\n"
                           "      %v1 = const : i32 (lit int 0 : i32)\n"
                           "      switch %v1 {"} + arms + "}\n"
               "    }\n" + extraBlock +
               "  }\n"
               "}\n";
    };

    DiagnosticReporter r;
    auto const res = dss::parseMir(module("", ""), CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok)
        << "a switch with no default arm must be REFUSED, not silently dropped.";
    EXPECT_FALSE(mirRefusal(module("", ""), "has no 'default -> %b<n>' arm").empty())
        << "the refusal did not say what was missing.";

    // The CONTROL, and it is what makes the arm above a claim about the DEFAULT
    // rather than about switches: the same shape WITH a default parses clean.
    // ⚠ THE MARKER ON `%b1` IS `switchjoin`, NOT `exit`, AND THE VERIFIER IS THE
    // ONE THAT SAYS SO: `mir_struct_markers.hpp` derives every block's marker from
    // the CFG and verify-on-load refuses a stored marker that disagrees. Writing
    // `exit` here reds this control for a reason that has nothing to do with the
    // default arm — which is precisely the entanglement a positive control must
    // not have.
    auto const withDefault =
        module(" default -> %b1 ", "    block %b1 [switchjoin] {\n      return\n    }\n");
    DiagnosticReporter r2;
    auto const         ok = dss::parseMir(withDefault, CompilationUnitId{1}, r2);
    EXPECT_TRUE(ok->ok) << "a switch WITH a default must still parse:"
                        << mirDiagnostics(withDefault);
}

// ★ HANDLE ZERO IS A LIVE SLOT, WHICH IS WHY THE DIGIT-LESS CASE COULD NOT STAY
// SILENT. `%b`, `%vx`, `%b3x` all fell out of the prefix-stripping loop as 0.
TEST(TextTierVocabulary, MalformedMirPercentHandleIsRefused) {
    auto const module = [](char const* target) {
        return std::string{"dssir 1\n"
                           "symbols { %1 \"f\" }\n"
                           "module {\n"
                           "  function %1 : fn() -> void {\n"
                           "    block %b0 [entry] {\n"
                           "      br %"} + target + "\n"
               "    }\n"
               // No marker: an unconditional `br` into a single successor derives
               // `Linear`, and verify-on-load refuses a STORED marker that
               // disagrees with the CFG derivation (`mir_struct_markers.hpp`).
               "    block %b1 {\n      return\n    }\n"
               "  }\n"
               "}\n";
    };

    for (char const* bad : {"b", "bx", "b1x"}) {
        SCOPED_TRACE(bad);
        DiagnosticReporter r;
        auto const         res = dss::parseMir(module(bad), CompilationUnitId{1}, r);
        EXPECT_FALSE(res->ok)
            << "a '%' handle with no digits (or trailing junk) must be REFUSED — "
               "falling out as handle 0 aliases a live slot.";
        EXPECT_FALSE(mirRefusal(module(bad), "malformed handle").empty())
            << "the refusal did not name the handle.";
    }

    DiagnosticReporter r2;
    auto const         ok = dss::parseMir(module("b1"), CompilationUnitId{1}, r2);
    EXPECT_TRUE(ok->ok) << "a well-formed handle must still parse:"
                        << mirDiagnostics(module("b1"));
}

// ★★★ THE REFUSAL THAT WAS DELEGATED TO A PASS THAT CANNOT RUN. A `br`/`condbr`/
// `switch` naming a block no `block %bN` header declared used to mint a silent
// `Linear` placeholder, on a comment asserting that "the verify-on-load pass will
// flag the orphan". ✔MEASURED 2026-08-21: it cannot. Verify-on-load runs AFTER
// `MirBuilder::finish()`, and `closeFunction_` aborts the process on any block
// created but never filled — the sibling switch-default mutant produced exactly
// that (`dss::MirBuilder fatal: block MirBlockId=1 has no terminator`, ctest
// `Exit code 0xc0000409`). A guard whose enforcement lives downstream of the
// crash is not a guard.
TEST(TextTierVocabulary, MirBranchToAnUndeclaredBlockIsRefused) {
    auto const module = [](char const* target) {
        return std::string{"dssir 1\n"
                           "symbols { %1 \"f\" }\n"
                           "module {\n"
                           "  function %1 : fn() -> void {\n"
                           "    block %b0 [entry] {\n"
                           "      br %"} + target + "\n"
               "    }\n"
               "    block %b1 {\n      return\n    }\n"
               "  }\n"
               "}\n";
    };

    DiagnosticReporter r;
    auto const         res = dss::parseMir(module("b7"), CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok)
        << "a branch to a block that was never declared must be REFUSED here — "
           "the placeholder it used to mint silently reaches the builder, which "
           "aborts the process rather than returning a diagnostic.";
    EXPECT_FALSE(mirRefusal(module("b7"), "was never declared as a").empty())
        << "the refusal did not name the missing block header.";

    // The CONTROL: the same shape branching to a DECLARED block parses clean, so
    // the arm above refuses an undeclared target rather than refusing `br`.
    DiagnosticReporter r2;
    auto const         ok = dss::parseMir(module("b1"), CompilationUnitId{1}, r2);
    EXPECT_TRUE(ok->ok) << "a branch to a declared block must still parse:"
                        << mirDiagnostics(module("b1"));
}

// ★ THE FUNCTION-ATTRIBUTE REFUSAL, which retyped BOTH of its closed sets while
// the two arms pinned above were being converted in the same file.
TEST(TextTierVocabulary, MirFunctionAttributeRefusalProjectsBindingAndVisibility) {
    auto const module = [](std::string_view attr) {
        return std::string{"dssir 1\n"
                           "symbols { %1 \"f\" }\n"
                           "module {\n"
                           "  function %1 : fn() -> void ["} + std::string{attr} + "] {\n"
               "    block %b0 [entry] {\n"
               "      return\n"
               "    }\n"
               "  }\n"
               "}\n";
    };

    auto const msg = mirRefusal(module(kBadSpelling), "unknown function attribute");
    ASSERT_FALSE(msg.empty()) << "an unknown function attribute must be refused.";
    auto advertised = quotedTokens(msg);
    std::erase(advertised, std::string{kBadSpelling});
    // 3 bindings + 4 visibilities + the 4 text-format keywords this file owns.
    EXPECT_EQ(advertised.size(), 11u)
        << "the advertised function-attribute set changed size — a SHRINK means "
           "an attribute the reader takes stopped being advertised.\nmessage:\n  "
        << msg;

    for (auto const& name : advertised) {
        SCOPED_TRACE(name);
        EXPECT_TRUE(mirRefusal(module(name), "unknown function attribute").empty())
            << "the refusal advertises attribute '" << name
            << "' and the reader then calls it UNKNOWN.";
    }
}

// ★★★ THE TWO VARIANT ARMS THE `.dssir` WRITER RENDERED AS NOTHING. Its
// `if constexpr` ladder covered eight of `MirLiteralValue`'s ten arms and had no
// final `else`, so a `_BitInt` constant and a folded F80/F128 constant — both of
// which `toMirLiteral` copies STRAIGHT ACROSS from the HIR pool — came out as
// `lit  : <core>` with the value gone and no diagnostic on either side. The
// spellings are the `.dsshir` tier's, verbatim: the two pools hold the SAME host
// types, so a second syntax would be a second owner of one serialization.
//
// ⚠ WHAT PROTECTS THE WRITER NOW IS A COMPILE ERROR, NOT THIS TEST. The ladder
// ends in a `static_assert` on a dependent false, so an arm added to the variant
// FAILS THE BUILD naming the writer. This pin covers what that cannot: that the
// spelling chosen is one the reader accepts, and that the value survives.
TEST(TextTierVocabulary, MirBitIntAndWideFloatLiteralsRoundTripThroughText) {
    struct Row { char const* value; char const* core; };
    for (Row const& row : {Row{"bitint 40 1 1 42", "i64"},
                           Row{"wfloat 80 16383 9223372036854775808", "f80"}}) {
        SCOPED_TRACE(row.value);
        auto const text = mirGlobalModule(std::string{row.core} + " = lit "
                                          + row.value + " : " + row.core);
        DiagnosticReporter r;
        auto const         res = dss::parseMir(text, CompilationUnitId{1}, r);
        ASSERT_TRUE(res->ok) << "the reader must accept the tag its own writer "
                                "emits:" << mirDiagnostics(text);

        dss::MirTextContext ctx;
        ctx.interner    = &res->interner;
        ctx.symbolNames = &res->symbolNames;
        DiagnosticReporter wr;
        std::string const  emitted = dss::emitMir(res->mir, ctx, wr);
        EXPECT_NE(emitted.find(row.value), std::string::npos)
            << "the value did not survive the round trip — this is the exact "
               "shape of the defect: a writer arm that renders NOTHING leaves a "
               "syntactically valid file with the constant deleted.\nemitted:\n"
            << emitted;

        // The format's own stated contract, and the half that notices a READER
        // that stopped accepting what the writer produces.
        DiagnosticReporter r2;
        auto const         again = dss::parseMir(emitted, CompilationUnitId{1}, r2);
        ASSERT_TRUE(again->ok) << "re-reading this compiler's own output must "
                                  "succeed:" << mirDiagnostics(emitted);
        dss::MirTextContext ctx2;
        ctx2.interner    = &again->interner;
        ctx2.symbolNames = &again->symbolNames;
        DiagnosticReporter wr2;
        EXPECT_EQ(dss::emitMir(again->mir, ctx2, wr2), emitted)
            << "emitMir(parseMir(emitMir(m))) must equal emitMir(m).";
    }
}

// ★★ THE MARKER THE `.dsshir` WRITER LEAVES WHERE A VALUE COULD NOT BE
// SERIALIZED. `appendLiteralValue` spells nine of `HirLiteralValue`'s ten variant
// arms; the tenth (the D5.3 folded aggregate) had no arm AND no final `else`, so
// the whole constant rendered as NOTHING. The marker and this refusal share ONE
// constant in the reader's translation unit, so the two ends cannot drift.
TEST(TextTierVocabulary, TheUnspelledAggregateLiteralMarkerIsRefusedByName) {
    auto const text =
        std::string{"dsshir 1\n"
                    "symbols { %1 \"g\" }\n"
                    "module [] \"probe\" {\n"
                    "  global %1 : i32 = lit unspelled_aggregate : i32\n"
                    "}\n"};
    EXPECT_FALSE(hirRefusal(text, "had no spelling for").empty())
        << "the writer's own unserializable-value marker must be refused BY NAME "
           "on the way back in. Falling into the generic 'unknown tag' arm sends "
           "the author looking for a typo that is not there.";
}

// ── PART 2c: THE `.dsshir` KEYWORD VOCABULARIES ─────────────────────────
//
// ★★★ D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET, and it is the WEAKER cousin of
// everything above: those refusals advertised a set that had DRIFTED from a
// table; these advertised NO SET AT ALL. `unknown flag 'x'`, `unknown attribute
// '@x'`, `unknown for-clause 'x'`, `unknown expression 'x'`, `unknown statement
// 'x'` — five closed keyword sets, five refusals that told an author their word
// was wrong and never what this reader would have taken. There was no table to
// project, so closing the row meant MINTING one per set from the parse arm.
//
// ★★ AND THE FEED-BACK HALF IS NOT CEREMONY HERE. ✔MEASURED 2026-08-23 while
// minting the expression table: `isExprKeyword` — the ROUTER `parseNode` uses to
// decide whether a line is an expression at all — was a hand-retyped THIRD copy
// carrying twenty of the twenty-three keywords. The three it omitted are
// `va_start`, `va_arg` and `va_end`, ALL of which `emitNodeLine` writes, so a
// `.dsshir` holding a variadic-access node routed to the STATEMENT parser and
// came back `unknown statement`. A round-trip pin could not see it (the writer's
// output was never read back through a module that had one) and a
// message-content pin could not see it (the message was correct about a keyword
// the router had already misrouted). Feeding every advertised keyword back
// through the reader is what sees it.

namespace {

// Every advertised spelling in `msg`, minus the bogus one the message echoes.
[[nodiscard]] std::vector<std::string> advertisedIn(std::string const& msg) {
    auto out = quotedTokens(msg);
    std::erase(out, std::string{kBadSpelling});
    return out;
}

// A `.dsshir` module whose single function body is `bodyLine`.
[[nodiscard]] std::string hirBody(std::string_view bodyLine) {
    return std::string{"dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
                       "  function %1 : fn() -> void {\n    block {\n      "}
         + std::string{bodyLine} + "\n      return\n    }\n  }\n}\n";
}

}  // namespace

TEST(TextTierVocabulary, EveryAdvertisedNodeFlagIsAcceptedByTheReader) {
    auto const msg = hirRefusal(hirBody(std::string{"expr ["} + kBadSpelling
                                        + "] lit int 1 : i32"),
                                "unknown node flag");
    ASSERT_FALSE(msg.empty())
        << "the flag arm must refuse an unrecognized spelling BY NAME.";
    auto const advertised = advertisedIn(msg);
    EXPECT_EQ(advertised.size(), 4u)
        << "the advertised flag set changed size — a SHRINK means a spelling the "
           "reader takes stopped being advertised.\nmessage:\n  " << msg;
    for (auto const& name : advertised) {
        SCOPED_TRACE(name);
        EXPECT_TRUE(hirTextIsClean(hirBody("expr [" + name + "] lit int 1 : i32")))
            << "the refusal advertises '" << name
            << "' and the reader then REJECTS it.";
    }
}

TEST(TextTierVocabulary, EveryAdvertisedForClauseIsAcceptedByTheReader) {
    auto const msg = hirRefusal(
        hirBody(std::string{"for { "} + kBadSpelling
                + ": expr lit int 1 : i32 body: block { } }"),
        "unknown for-clause");
    ASSERT_FALSE(msg.empty()) << "the for-clause arm must refuse by name.";
    auto const advertised = advertisedIn(msg);
    EXPECT_EQ(advertised.size(), 4u)
        << "the advertised for-clause set changed size.\nmessage:\n  " << msg;
    for (auto const& role : advertised) {
        SCOPED_TRACE(role);
        // `body` is mandatory, so every probe carries one; a non-`body` role
        // gets its own clause in addition.
        std::string const line = (role == "body")
            ? std::string{"for { body: block { } }"}
            : "for { " + role + ": expr lit int 1 : i32 body: block { } }";
        EXPECT_TRUE(hirTextIsClean(hirBody(line)))
            << "the refusal advertises '" << role
            << "' and the reader then REJECTS it.";
    }
}

TEST(TextTierVocabulary, EveryAdvertisedAggregateFieldCoreIsAcceptedByTheReader) {
    // D-HIR-TEXT-WRITER-DROPS-THE-AGGREGATE-LITERAL-ARM: the per-field core
    // position, whose accepted set is the union of the primitive table and the
    // structural literal-core table — the same two-table join the `.dssir` tier
    // makes at its own literal-core position.
    auto const msg = hirRefusal(
        hirBody(std::string{"expr lit agg {int 1 : "} + kBadSpelling + "} : i32"),
        "unknown aggregate literal field core");
    ASSERT_FALSE(msg.empty())
        << "an unrecognized field core must be refused BY NAME — a silent default "
           "would give every element of a folded aggregate the wrong core while "
           "the re-emitted text still matched byte for byte.";
    auto const advertised = advertisedIn(msg);
    EXPECT_EQ(advertised.size(), 26u)
        << "the advertised field-core set changed size.\nmessage:\n  " << msg;
    for (auto const& core : advertised) {
        SCOPED_TRACE(core);
        EXPECT_TRUE(
            hirRefusal(hirBody("expr lit agg {int 1 : " + core + "} : i32"),
                       "unknown aggregate literal field core").empty())
            << "the refusal advertises '" << core
            << "' and the reader then REJECTS it.";
    }
}

// ★★★ THE ARM THAT WOULD HAVE CAUGHT THE `va_*` ROUTING HOLE.
//
// The refusal at a node position advertises BOTH halves of the accepted set —
// statements and expressions — because `parseNode` routes on the expression
// table first and falls through to the statement parser. Feeding each advertised
// keyword back and requiring that it is not refused AS A KEYWORD is exactly the
// property that broke: `va_arg` was emitted, was a real expression keyword, and
// the router did not know it.
//
// ⚠ THE ASSERTION IS DELIBERATELY NOT "PARSES CLEANLY". Almost every one of these
// keywords needs operands, a type annotation or a body, so a bare keyword is
// legitimately malformed for reasons that have nothing to do with the
// vocabulary. What must NOT happen is the reader failing to RECOGNIZE it.
TEST(TextTierVocabulary, EveryAdvertisedNodeKeywordIsRecognizedByTheReader) {
    auto const msg = hirRefusal(hirBody(kBadSpelling), "unknown node keyword");
    ASSERT_FALSE(msg.empty())
        << "an unrecognized node keyword must be refused BY NAME.";
    auto const advertised = advertisedIn(msg);
    // 28 statement keywords + 25 expression keywords.
    // ⓘ 23 → 25 on 2026-08-23 (cycle P28, lane Z): `builtincall` and `labeladdr`
    // were WRITTEN by `emitExpr` and advertised by neither table — the same
    // write-only shape as the `va_*` hole this arm was built for, two arms over
    // (D-HIR-TEXT-WRITER-SPELLS-KEYWORDS-THE-READER-HAS-NO-ROW-FOR). This is a
    // GROWTH, which is the legitimate direction; the loop below already proved
    // both new spellings are recognized before this number moved.
    EXPECT_EQ(advertised.size(), 53u)
        << "the advertised node-keyword set changed size. A SHRINK means a "
           "keyword the reader dispatches stopped being advertised — or, worse, "
           "stopped being routed.\nmessage:\n  " << msg;
    for (auto const& kw : advertised) {
        SCOPED_TRACE(kw);
        EXPECT_TRUE(hirRefusal(hirBody(kw), "unknown node keyword").empty())
            << "the refusal advertises '" << kw
            << "' and the reader then does not recognize it as a node keyword.";
    }
}

// ★★★ THE TYPE-KEYWORD POSITION IN BOTH TIERS, AND ITS FEED-BACK PIN IS THE
// PAIRING GUARD.
//
// The keyword ladders in `parseType` are NOT converted into a `switch` over
// their table the way the flag / attribute / for-clause / node-keyword ladders
// were: their arms are structurally unlike each other, several share a body,
// `unsigned` is a PREFIX rather than a head keyword, and the `.dsshir` one is on
// the SHIPPED path that decodes every FFI descriptor's type strings. So the
// table owns the SET only — and what stops the set drifting from the ladder is
// this arm, which drives every advertised spelling back through the reader and
// requires that none of them comes back as `unknown type`.
//
// ⚠ NOT "PARSES CLEANLY". Every structural keyword needs an operand (`ptr<T>`,
// `arr<T,N>`, `struct "N" { … }`), so a bare keyword is legitimately malformed —
// for a reason that has nothing to do with the vocabulary. What must not happen
// is the reader failing to RECOGNIZE it.
TEST(TextTierVocabulary, EveryAdvertisedHirTypeKeywordIsRecognizedByTheReader) {
    dss::TypeInterner in{dss::CompilationUnitId{1}};
    dss::TypeRegistry reg;
    auto refusalFor = [&](std::string_view text) {
        DiagnosticReporter r;
        (void)dss::parseTypeFromText(text, in, reg, r, {});
        std::string out;
        for (auto const& d : r.all()) {
            if (d.actual.find("unknown type") != std::string::npos) out = d.actual;
        }
        return out;
    };
    auto const msg = refusalFor(kBadSpelling);
    ASSERT_FALSE(msg.empty()) << "the type arm must refuse an unknown keyword BY NAME";
    auto const advertised = advertisedIn(msg);
    // 20 primitive spellings + 21 structural keywords.
    EXPECT_EQ(advertised.size(), 41u)
        << "the advertised type-keyword set changed size.\nmessage:\n  " << msg;
    for (auto const& kw : advertised) {
        SCOPED_TRACE(kw);
        EXPECT_TRUE(refusalFor(kw).empty())
            << "the refusal advertises '" << kw
            << "' and the reader then does not recognize it as a type keyword.";
    }
}

TEST(TextTierVocabulary, EveryAdvertisedMirTypeKeywordIsRecognizedByTheReader) {
    auto refusalFor = [](std::string_view kw) {
        return mirRefusal(mirGlobalModule(std::string{kw} + " = zero"),
                          "unknown type");
    };
    auto const msg = refusalFor(kBadSpelling);
    ASSERT_FALSE(msg.empty()) << "the type arm must refuse an unknown keyword BY NAME";
    auto const advertised = advertisedIn(msg);
    // 19 primitive spellings + 16 structural keywords.
    EXPECT_EQ(advertised.size(), 35u)
        << "the advertised type-keyword set changed size.\nmessage:\n  " << msg;
    for (auto const& kw : advertised) {
        SCOPED_TRACE(kw);
        EXPECT_TRUE(refusalFor(kw).empty())
            << "the refusal advertises '" << kw
            << "' and the reader then does not recognize it as a type keyword.";
    }
}

// ── PART 3: the REFERENCED-DOCUMENT BLOCK SURFACE ───────────────────────
//
// ★★★ THE MEASURED DRIFT THAT COST THE MOST, AND THE ONE THIS FILE EXISTS FOR.
// `checkReferencedDocumentBlocks` decides what a referenced document may declare
// by walking TWO tables in sequence — the blocks the merge folds into the host,
// and the blocks the document reads for ITSELF and the merge deliberately leaves
// alone — plus a `languageReferences` arm with its own, more specific refusal.
// Its sentence stated only the FIRST table: "A referenced document may declare
// only the blocks the merge folds in ('shapes', 'semantics', 'hirLowering',
// 'pipelineEntry') plus 'dssSchemaVersion', 'language' and 'requires'."
// SEVEN advertised. FIFTEEN accepted. Every standalone-surface block — `tokens`,
// `lexerModes`, `assembly`, `numberStyle`, `keywords`, `syncTokens`,
// `artifactProfiles`, `identifierClass` — is legal in a referenced document, has
// a paragraph in the loader explaining exactly why, and was named by that
// sentence as forbidden. `asm.lang.json` declares four of them.
//
// ⚠ THE PROBE MOVES THE CWD, IT DOES NOT WRITE THE ENVIRONMENT. A referenced
// document is resolved off the FILESYSTEM by logical name — there is no text
// entry point for the referenced side — and `findShippedConfig` consults
// `$DSS_CONFIG_ROOT` first, falling through to a cwd walk. Entering a scratch
// root therefore ADDS `probe` without REMOVING anything, so the shipped
// documents the rest of this file mutates still resolve. Restored in the
// destructor, before `remove_all`, because Windows refuses to delete the current
// directory. (The mechanism is `test_language_references.cpp`'s
// `ProbeConfigRoot`; it lives in that file's anonymous namespace, so this is a
// second instance of it rather than a second design.)

namespace {

class ProbeConfigRoot {
public:
    explicit ProbeConfigRoot(nlohmann::json const& referencedDoc) {
        namespace fs = std::filesystem;
        static int      counter = 0;
        std::error_code ec;
        root_ = fs::temp_directory_path()
              / ("dss-keyshape-refdoc-" + std::to_string(++counter));
        fs::remove_all(root_, ec);
        fs::path const sources = root_ / "src" / "dss-config" / "sources";
        fs::create_directories(sources, ec);
        if (ec) {
            ADD_FAILURE() << "could not create the probe config root: "
                          << ec.message();
            return;
        }
        {
            std::ofstream out(sources / "probe.lang.json", std::ios::binary);
            out << referencedDoc.dump(2);
        }
        previous_ = fs::current_path(ec);
        fs::current_path(root_, ec);
        if (ec) {
            ADD_FAILURE() << "could not enter the probe config root: "
                          << ec.message();
        }
    }
    ~ProbeConfigRoot() {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!previous_.empty()) fs::current_path(previous_, ec);
        fs::remove_all(root_, ec);
    }
    ProbeConfigRoot(ProbeConfigRoot const&)            = delete;
    ProbeConfigRoot& operator=(ProbeConfigRoot const&) = delete;

private:
    std::filesystem::path root_;
    std::filesystem::path previous_;
};

// The synthetic REFERENCED document: two holes, one shape, nothing else. Every
// arm below is this document plus ONE key, so a diagnostic can only be about the
// key that was added.
[[nodiscard]] nlohmann::json probeReferencedDoc() {
    return nlohmann::json::parse(R"({
      "dssSchemaVersion": 4,
      "language": { "name": "probe", "version": "0.0.1" },
      "requires": { "rules": ["probeValue"], "tokens": ["probeMark"] },
      "shapes": {
        "probeStmt": { "sequence": ["probeMark", "probeValue"] }
      }
    })");
}

[[nodiscard]] nlohmann::json probeHostJson() {
    return nlohmann::json::parse(R"({
      "dssSchemaVersion": 4,
      "language": { "name": "KeyShapeRefHost", "version": "0.0.1" },
      "languageReferences": {
        "probe": {
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
}

// Load the probe host against a referenced document carrying `extraKey`, and
// return the "does NOT consume" refusal if one fired.
[[nodiscard]] std::string refusalForReferencedKey(std::string const& extraKey,
                                                  nlohmann::json     value) {
    auto refDoc = probeReferencedDoc();
    if (!extraKey.empty()) refDoc[extraKey] = std::move(value);
    ProbeConfigRoot root{refDoc};
    auto r = GrammarSchema::loadFromText(probeHostJson().dump(2),
                                         "<keyshape-refdoc-probe>");
    if (r.has_value()) return {};
    return messageContaining(r.error(), "does NOT consume");
}

}  // namespace

// The BASELINE, and it is not decoration: without it a loader that refused every
// referenced document would turn the honesty arm below green.
TEST(ReferencedDocumentSurface, TheProbeReferencedDocumentLoadsCleanly) {
    auto refDoc = probeReferencedDoc();
    ProbeConfigRoot root{refDoc};
    auto r = GrammarSchema::loadFromText(probeHostJson().dump(2),
                                         "<keyshape-refdoc-probe>");
    EXPECT_TRUE(r.has_value())
        << "the synthetic referenced document must load through the probe host, "
           "or every arm below is green for the wrong reason:"
        << (r.has_value() ? std::string{} : summarize(r.error()));
}

TEST(ReferencedDocumentSurface, TheRefusalNamesEveryBlockTheLoopLetsThrough) {
    // The POSITIVE CONTROL: `operators` is in neither table and has no consumer
    // on either side, so it must still be refused. If this stops firing, the
    // guard has been widened and the arms below stop meaning anything.
    auto const msg = refusalForReferencedKey(
        "operators",
        nlohmann::json::parse(R"({"binary":[{"token":"BangEnd","precedence":1}]})"));
    ASSERT_FALSE(msg.empty())
        << "a referenced document declaring a block the merge does not consume "
           "must be REFUSED — it would load clean and then vanish.";

    auto advertised = quotedTokens(msg);
    // The message's own non-vocabulary quotes: the two document labels and the
    // offending key, all three of which THIS TEST supplied, so they are removed
    // by identity rather than by a heuristic.
    for (char const* own : {"probe.lang.json", "<keyshape-refdoc-probe>",
                            "operators"}) {
        std::erase(advertised, std::string{own});
    }
    // ⚠ THE COUNT IS STATED AND IT IS THE SHRINK GUARD. Everything else walks
    // whatever the message says; only this notices a list that got shorter —
    // which is the exact direction this site was measured wrong in (7 of 15).
    EXPECT_EQ(advertised.size(), 15u)
        << "the advertised referenced-document surface changed size. 15 = the 7 "
           "keys the merge folds in or reads itself, plus the 8 standalone "
           "blocks the merge deliberately leaves alone. A SHRINK means a block "
           "the loop lets through stopped being advertised.\nmessage:\n  "
        << msg;

    // HONESTY: nothing advertised is actually refused. This is the arm that
    // reads the message back to the loader, key by key.
    for (auto const& key : advertised) {
        SCOPED_TRACE(key);
        if (key == "dssSchemaVersion" || key == "language"
            || key == "requires" || key == "shapes") {
            continue;  // already declared by the probe document itself
        }
        EXPECT_TRUE(refusalForReferencedKey(key, nlohmann::json::object()).empty())
            << "the refusal advertises '" << key
            << "' as a block a referenced document may declare, and the loader "
               "then refuses it as unconsumed.";
    }
}
