// ── D-CONFIG-UNKNOWN-KEY-CHECK-HAND-ROLLED-SITES-REMAIN ──────────────────
//
// The grammar and predefined-macro loaders used to carry ~40 independently
// written copies of ONE check — "is this key part of the block's closed
// vocabulary?" — plus twelve blocks that had no vocabulary TABLE at all, only a
// chained `key != "a" && key != "b"` condition. Every copy had to remember the
// `$`-documentation carve-out for itself, and the class had already shipped two
// live defects in sibling loaders that forgot it (they REFUSED valid `$…Comment`
// prose keys). Every one of those sites now routes through the shared
// `dss::detail::rejectUnknownKeys`.
//
// ★★★ WHY THIS FILE MUTATES THE SHIPPED DOCUMENTS RATHER THAN AUTHORING
// FIXTURES. A hand-written stand-in document proves the loader refuses a shape
// the loader never actually receives, and it rots against the real config
// silently. Every row below names a JSON POINTER INTO A SHIPPED
// `.lang.json`, and `injectAt` THROWS if that pointer does not resolve to an
// object — so a row that goes stale (the block moves, the index shifts, the
// document is rewritten) fails the test rather than passing vacuously. This is
// the `mutateShippedTargetSchemaDoc` contract, applied to language documents.
//
// ★★ AND THE BASELINE ARM IS NOT DECORATION. Without it, a mutant loader that
// refused EVERY key would turn every negative row below green.

#include "core/types/config_path_walk.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ::dss::DiagnosticCode;
using ::dss::findShippedConfig;
using ::dss::GrammarSchema;
using ::dss::ShippedConfigLocator;

// The probe key names. Deliberately unlikely spellings: a name that collided
// with a real key would make the negative arm pass for the wrong reason.
constexpr char const* kUnknownProbe = "zzClosedKeyVocabularyProbe";
constexpr char const* kDocProbe     = "$zzClosedKeyVocabularyComment";

// A fixture fault — never a verdict about the config. Thrown, never returned,
// for the reason `mutate_target_schema.hpp` records at length: a fixture fault
// reported through the loader's own envelope is indistinguishable from the
// refusal the negative rows exist to prove, and every one of them would go
// green over a document that was never mutated.
class VocabularyProbeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] nlohmann::json shippedLanguageDoc(std::string_view name) {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{name, "sources", ".lang.json", "language",
                             DiagnosticCode::C_InvalidLanguageName});
    if (!pathR.has_value()) {
        throw VocabularyProbeError{std::string{"cannot resolve shipped language "
                                               "document '"} +
                                   std::string{name} + "'"};
    }
    std::ifstream in{*pathR};
    if (!in.is_open()) {
        throw VocabularyProbeError{std::string{"cannot open "} +
                                   pathR->string()};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return nlohmann::json::parse(buf.str());
}

// Insert `key` at `pointer`. FAIL-CLOSED on every way the injection could
// silently do nothing: an unresolvable pointer, a non-object target, or a key
// that is already there (which would make the "mutant" byte-identical).
void injectAt(nlohmann::json& doc, std::string_view pointer,
              std::string_view key, std::string_view where) {
    nlohmann::json* node = nullptr;
    try {
        node = &doc.at(nlohmann::json::json_pointer{std::string{pointer}});
    } catch (std::exception const& e) {
        throw VocabularyProbeError{
            std::string{"["} + std::string{where} + "] the JSON pointer '" +
            std::string{pointer} + "' does not resolve in the shipped document (" +
            e.what() +
            "). A row whose pointer went stale asserts NOTHING — re-aim it at "
            "the block it names."};
    }
    if (!node->is_object()) {
        throw VocabularyProbeError{
            std::string{"["} + std::string{where} + "] '" + std::string{pointer} +
            "' is a " + node->type_name() +
            ", not an object — a closed-key vocabulary can only be checked on an "
            "OBJECT, so this row is aimed at the wrong node."};
    }
    if (node->contains(std::string{key})) {
        throw VocabularyProbeError{
            std::string{"["} + std::string{where} + "] '" + std::string{pointer} +
            "' already declares '" + std::string{key} +
            "' — the injection would be a no-op and the pin would assert "
            "nothing."};
    }
    (*node)[std::string{key}] = "probe";
}

[[nodiscard]] std::string summarize(auto const& diags) {
    std::string s;
    for (auto const& d : diags) s += "\n  " + d.path + ": " + d.message;
    return s.empty() ? std::string{"<no diagnostics>"} : s;
}

[[nodiscard]] bool says(auto const& diags, std::string const& needle) {
    return std::ranges::any_of(diags, [&needle](auto const& d) {
        return d.message.find(needle) != std::string::npos;
    });
}

// One closed-key block: which shipped document holds it, where, and the prose
// label its refusal must name. The LABEL is asserted, not just the fact of a
// refusal — otherwise a row could be satisfied by some OTHER block refusing the
// probe, which is exactly how a mis-aimed row would read as green.
struct Block {
    char const* language;
    char const* pointer;
    char const* label;
};

// ✔MEASURED 2026-08-20 by walking every shipped `.lang.json` and recording the
// first document + pointer at which each routed block occurs. Every pointer is
// re-validated on every run by `injectAt`.
constexpr Block kBlocks[] = {
    // ── document / reference surface ──
    {"c-subset", "/language",                        "the 'language' block"},
    {"asm-arm64-gas", "/languageReferences/asm",     "a 'languageReferences' entry"},
    // ── lexical surface ──
    {"c-subset", "/keywords/0",                      "a 'keywords' entry"},
    {"asm-arm64-gas", "/lexerModes/line-comment/defaultToken",
                                                     "a 'defaultToken' object"},
    {"asm-arm64-gas", "/identifierClass",            "the 'identifierClass' block"},
    {"asm-arm64-gas", "/numberStyle",                "the 'numberStyle' block"},
    {"c-subset", "/numberStyle/exponent",            "an 'exponent' block"},
    {"asm-arm64-gas", "/numberStyle/integerPrefixes/0",
                                                     "an 'integerPrefixes' entry"},
    {"c-subset", "/numberStyle/integerPrefixes/0/float",
                                                     "a prefix's 'float' block"},
    {"asm-arm64-gas", "/numberStyle/emitKind",       "the 'emitKind' block"},
    // ── grammar surface ──
    {"c-subset", "/shapes/expression/expr",          "an 'expr' body"},
    {"c-subset", "/shapes/expression/expr/wrapperRules",
                                                     "a 'wrapperRules' object"},
    {"c-subset", "/shapes/enumTypeSpecifier/commitRequiresTypeName",
                                                     "a 'commitRequiresTypeName' guard"},
    // ── preprocess ──
    {"c-subset", "/preprocess/pragmaEffects/0",      "a 'pragmaEffects' row"},
    // ── semantics ──
    {"c-subset", "/semantics",                       "the 'semantics' block"},
    {"c-subset", "/semantics/declarators",           "the 'declarators' block"},
    {"c-subset", "/semantics/declarations/0",        "'declarations[0]'"},
    {"c-subset", "/semantics/declarations/2/gatedMarkers/0",
                                                     "a 'gatedMarkers' entry"},
    {"c-subset", "/semantics/declarations/7/entryFunctions/main/0",
                                                     "an 'entryFunctions' shape"},
    {"c-subset", "/semantics/attributeSemantics",    "the 'attributeSemantics' block"},
    {"c-subset", "/semantics/attributeSemantics/effects/0",
                                                     "an 'effects' row"},
    {"c-subset", "/semantics/typeSpecifiers/0",      "a 'typeSpecifiers' entry"},
    {"c-subset", "/semantics/integerLiteralTyping/0",
                                                     "an 'integerLiteralTyping' entry"},
    {"c-subset", "/semantics/floatLiteralTyping/0",  "a 'floatLiteralTyping' entry"},
    {"c-subset", "/semantics/parameters",            "the 'parameters' block"},
    {"c-subset", "/semantics/arithmeticConversions", "the 'arithmeticConversions' block"},
    {"c-subset", "/semantics/arithmeticConversions/integerPromotion",
                                                     "the 'integerPromotion' block"},
    {"c-subset", "/semantics/synthesizedTypes",      "the 'synthesizedTypes' block"},
    {"c-subset", "/semantics/pointerConversions",    "the 'pointerConversions' block"},
    {"c-subset", "/semantics/pointerAliasing",       "the 'pointerAliasing' block"},
    {"c-subset", "/semantics/builtinFunctions/0",    "a 'builtinFunctions' entry"},
    // ── pipeline entry + assembly dialect surface ──
    {"asm-arm64-gas", "/pipelineEntry",              "the 'pipelineEntry' block"},
    {"asm-arm64-gas", "/pipelineEntry/byRule/0",     "a 'byRule' row"},
    {"asm-arm64-gas", "/assembly",                   "the 'assembly' block"},
    {"asm-arm64-gas", "/assembly/operandForms",      "the 'operandForms' map"},
    {"asm-arm64-gas", "/assembly/instructions/0",    "an 'instructions' row"},
    {"asm-arm64-gas", "/assembly/instructions/36/operandSelectors/0",
                                                     "an 'operandSelectors' entry"},
    {"asm-arm64-gas", "/assembly/directives/0",      "a 'directives' row"},
    // ── the predefined-macro loader, reached through a language document ──
    {"c-subset", "/preprocess/predefinedMacros/0",   "a 'predefinedMacros' entry"},
};

// Every language document a row above names, once.
constexpr char const* kLanguages[] = {"c-subset", "asm-arm64-gas"};

} // namespace

// ── BASELINE ────────────────────────────────────────────────────────────
//
// The positive control for every negative row below. A loader that refused all
// keys would make each of them green.
TEST(ClosedKeyVocabulary, EveryProbedShippedDocumentLoadsCleanly) {
    for (char const* lang : kLanguages) {
        auto const doc  = shippedLanguageDoc(lang);
        auto const text = doc.dump();
        auto       r    = GrammarSchema::loadFromText(text, lang);
        ASSERT_TRUE(r.has_value())
            << lang << " must load clean before any mutant means anything: "
            << summarize(r.error());
    }
}

// ── NEGATIVE: an unknown key in ANY closed-key block is REFUSED ─────────
TEST(ClosedKeyVocabulary, UnknownKeyIsRefusedInEveryClosedKeyBlock) {
    for (auto const& b : kBlocks) {
        SCOPED_TRACE(std::string{b.language} + b.pointer);
        auto doc = shippedLanguageDoc(b.language);
        injectAt(doc, b.pointer, kUnknownProbe, b.pointer);
        auto const text = doc.dump();
        auto       r    = GrammarSchema::loadFromText(text, b.language);
        ASSERT_FALSE(r.has_value())
            << "an unknown key in " << b.label
            << " must FAIL the load — a silently-dropped key leaves the feature "
               "it names switched off with no diagnostic";
        EXPECT_TRUE(says(r.error(),
                         std::string{"unknown key '"} + kUnknownProbe + "'"))
            << "the refusal must be the closed-key check, not some unrelated "
               "failure the injection happened to trigger:"
            << summarize(r.error());
        // ★ The refusal must name THIS block. Without it, a row aimed at the
        // wrong pointer would be satisfied by a neighbouring block's refusal.
        EXPECT_TRUE(says(r.error(), std::string{b.label}))
            << "the diagnostic must name " << b.label << ':'
            << summarize(r.error());
    }
}

// ── THE `$`-DOCUMENTATION CARVE-OUT, per block ──────────────────────────
//
// This is the axis the class's two known LIVE defects were on — loaders that
// REFUSED a valid `$…Comment` prose key. It is asserted with a `$…Comment`
// spelling rather than a bare `$comment`, because a literal `"$comment"`
// compare (the wrong implementation) passes the bare form and fails this one.
TEST(ClosedKeyVocabulary, DocumentationKeyStaysExemptInEveryClosedKeyBlock) {
    for (auto const& b : kBlocks) {
        SCOPED_TRACE(std::string{b.language} + b.pointer);
        auto doc = shippedLanguageDoc(b.language);
        injectAt(doc, b.pointer, kDocProbe, b.pointer);
        auto const text = doc.dump();
        auto       r    = GrammarSchema::loadFromText(text, b.language);
        EXPECT_TRUE(r.has_value())
            << "a '$'-prefixed documentation key in " << b.label
            << " is PROSE and must never trip the typo discriminator:"
            << summarize(r.error());
    }
}

// ── THE CARVE-OUT, EVERYWHERE AT ONCE ───────────────────────────────────
//
// The per-block rows above can only cover blocks somebody remembered to list.
// This one injects a `$…Comment` into EVERY object of every shipped language
// document in one pass, so a site that forgets the carve-out is caught whether
// or not it appears in `kBlocks` — the whole point being that the sites which
// forget are never the ones being read at the time.
namespace {

// ★★★ THE CARVE-OUT IS NOT UNIVERSAL, AND THAT IS DELIBERATE — this list is the
// difference between a strong pin and a wrong one. ✔MEASURED 2026-08-20 by
// running the sweep WITHOUT it: it reported four map key-spaces as refusing a
// `$…Comment`, which reads exactly like the live defect this class was opened
// for (`D-CONFIG-UNKNOWN-KEY-CHECKS-REJECT-VALID-DOCUMENTATION-KEYS`). It is
// not. In each of these maps the KEY IS SOURCE TEXT the configured language
// defines, not an identifier from a vocabulary the loader owns:
//
//   `tokens` (global and per-mode) — the key is a LEXEME. `$` is an ordinary
//       operator character (a bare `$`, C#-style `$"…"` interpolation, `$$`)
//       and this repo's own fixtures declare all three; the loader's comment
//       records that carving `$` out here broke `core/test_operator_table` and
//       seven `core/test_lexer_modes` cases.
//   `linkageSpecifiers` — the key is specifier SOURCE TEXT matched against
//       `tree().text()`, the same arbitrary key space.
//   `entryFunctions` — the key is an entry function NAME in the configured
//       source language, and `$` is a legal identifier character in the
//       references DSS follows.
//
// Reserving `$` in any of these would silently DELETE a legal spelling from the
// configured language, which is a worse failure than the missing annotation:
// prose for such a map goes on a `$`-prefixed sibling one level UP, where the
// keys really are the loader's own.
[[nodiscard]] bool keysAreSourceTextNotLoaderVocabulary(std::string_view path) {
    for (std::string_view const owner :
         {"/tokens", "/linkageSpecifiers", "/entryFunctions"}) {
        if (path.size() >= owner.size() &&
            path.substr(path.size() - owner.size()) == owner) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(ClosedKeyVocabulary, DocumentationKeyStaysExemptInEveryObjectOfEveryDocument) {
    for (char const* lang : kLanguages) {
        SCOPED_TRACE(lang);
        auto        doc      = shippedLanguageDoc(lang);
        std::size_t injected = 0;
        std::size_t skipped  = 0;
        auto const  inject =
            [&](nlohmann::json& node, std::string const& path, auto&& self) -> void {
            if (node.is_object()) {
                for (auto& [k, v] : node.items()) self(v, path + "/" + k, self);
                if (keysAreSourceTextNotLoaderVocabulary(path)) { ++skipped; return; }
                node[kDocProbe] = "probe";
                ++injected;
            } else if (node.is_array()) {
                std::size_t i = 0;
                for (auto& v : node) self(v, path + "/" + std::to_string(i++), self);
            }
        };
        inject(doc, std::string{}, inject);
        // Fail-closed, both ways. A walk that reached nothing would make the
        // load assertion green over an unmutated document; and a skip list that
        // stopped matching anything would silently turn back into the wrong
        // premise measured above.
        ASSERT_GT(injected, 100U)
            << "the walk injected into only " << injected
            << " object(s) — that is not a whole language document, so this "
               "pin would be asserting nothing";
        EXPECT_GT(skipped, 0U)
            << "no source-text-keyed map was skipped, so the skip list has gone "
               "stale — re-measure before trusting the green";
        auto const text = doc.dump();
        auto       r    = GrammarSchema::loadFromText(text, lang);
        EXPECT_TRUE(r.has_value())
            << lang << ": a '$'-prefixed key is PROSE at every site whose keys "
                       "are the LOADER's vocabulary (" << injected
            << " objects injected, " << skipped << " source-text maps skipped):"
            << summarize(r.error());
    }
}

// The other half of the skip list above, so the deliberate NON-carve-out is
// pinned rather than merely commented: a `$`-led key in the `tokens` map is a
// LEXEME, and it loads.
TEST(ClosedKeyVocabulary, ADollarLedKeyInTheTokensMapIsALexemeNotProse) {
    auto doc = shippedLanguageDoc("c-subset");
    ASSERT_FALSE(doc["tokens"].contains("$$"))
        << "the probe lexeme must not already be declared, or this asserts "
           "nothing";
    doc["tokens"]["$$"] = nlohmann::json::parse(
        R"([{ "kind": "DollarDollarProbe" }])");
    auto const text = doc.dump();
    auto       r    = GrammarSchema::loadFromText(text, "c-subset");
    EXPECT_TRUE(r.has_value())
        << "`$` is an ordinary operator character in a configured language; "
           "reserving it in the lexeme key space would DELETE a legal spelling:"
        << summarize(r.error());
}

// ── THE ARMS: `impliedSurface`'s per-state key vocabulary ────────────────
//
// D-CONFIG-VALISTLAYOUT-INERT-CROSS-STRATEGY-KEY in its second home. The three
// `kind` states read three different field sets, so their union is NOT the
// vocabulary: a sibling-arm key is spelled correctly, loads clean under a union,
// and is read by nothing — indistinguishable in config from a key that works.
namespace {

// Index of the first `predefinedMacros` entry whose `impliedSurface.kind` is
// `kind`. Throws rather than returning a sentinel: a row that silently found
// nothing would make its pin vacuous.
[[nodiscard]] std::size_t firstMacroWithKind(nlohmann::json const& doc,
                                             std::string_view      kind) {
    auto const& arr = doc.at("preprocess").at("predefinedMacros");
    for (std::size_t i = 0; i < arr.size(); ++i) {
        auto const& e = arr[i];
        if (!e.is_object() || !e.contains("impliedSurface")) continue;
        auto const& is = e.at("impliedSurface");
        if (is.is_object() && is.value("kind", std::string{}) == kind) return i;
    }
    throw VocabularyProbeError{
        std::string{"no shipped 'predefinedMacros' entry declares "
                    "impliedSurface.kind = '"} +
        std::string{kind} + "' — this cross-arm pin has no subject."};
}

} // namespace

TEST(ClosedKeyVocabulary, ImpliedSurfaceRefusesASiblingArmsKeyByNamingThatArm) {
    // `reason` is a REAL key — of the `claims-nothing` arm. Under `surface` the
    // parse code never reads it, so accepting it would ship a claim nobody
    // checks.
    auto        doc = shippedLanguageDoc("c-subset");
    auto const  i   = firstMacroWithKind(doc, "surface");
    doc["preprocess"]["predefinedMacros"][i]["impliedSurface"]["reason"] =
        "arch-property";
    auto const text = doc.dump();
    auto       r    = GrammarSchema::loadFromText(text, "c-subset");
    ASSERT_FALSE(r.has_value())
        << "a 'claims-nothing' key on a 'surface' impliedSurface is UNREACHABLE "
           "and must be refused, not ignored";
    EXPECT_TRUE(says(r.error(), "unknown key 'reason'")) << summarize(r.error());
    EXPECT_TRUE(says(r.error(), "of the 'claims-nothing' arm"))
        << "the diagnostic must name the arm the key belongs to — the author "
           "did not make a typo, they pasted from the wrong mechanism:"
        << summarize(r.error());
}

TEST(ClosedKeyVocabulary, ImpliedSurfaceRefusesTheOtherDirectionToo) {
    // The mirror, so the pin asserts the PER-ARM RULE rather than one
    // special-cased key: `headers` belongs to `surface`.
    auto        doc = shippedLanguageDoc("c-subset");
    auto const  i   = firstMacroWithKind(doc, "claims-nothing");
    doc["preprocess"]["predefinedMacros"][i]["impliedSurface"]["headers"] =
        nlohmann::json::array();
    auto const text = doc.dump();
    auto       r    = GrammarSchema::loadFromText(text, "c-subset");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(says(r.error(), "unknown key 'headers'")) << summarize(r.error());
    EXPECT_TRUE(says(r.error(), "of the 'surface' arm")) << summarize(r.error());
}

TEST(ClosedKeyVocabulary, ImpliedSurfaceStillRefusesAPlainTypoWithItsOwnArmsList) {
    // A key belonging to NO arm gets the ordinary refusal — the cross-arm
    // sentence must not be the only way to fail, and the allowed list rendered
    // must be the DECLARED arm's, never the union.
    auto        doc = shippedLanguageDoc("c-subset");
    auto const  i   = firstMacroWithKind(doc, "claims-nothing");
    doc["preprocess"]["predefinedMacros"][i]["impliedSurface"]["raeson"] = "x";
    auto const text = doc.dump();
    auto       r    = GrammarSchema::loadFromText(text, "c-subset");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(says(r.error(), "unknown key 'raeson'")) << summarize(r.error());
    EXPECT_FALSE(says(r.error(), "'headers'"))
        << "the allowed list must be the DECLARED arm's, never the union of all "
           "three — a union is the defect this block's per-arm tables exist to "
           "prevent:"
        << summarize(r.error());
}
