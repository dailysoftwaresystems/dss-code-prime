// ── D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET ──────────
//
// The sibling class of the closed-KEY sweep, one level up. A config site whose
// keys (or whose value) must name an ENUM resolves them through a `…FromName`
// lookup and reports the miss — and the report used to state the accepted set
// as a STRING LITERAL typed beside the check. That is a second owner of a fact
// the enum's `EnumNameTable` already owns, and the two drift in silence: the
// rows keep working while the sentence becomes a lie.
//
// ★★★ WHY THIS FILE EXISTS AT ALL, AND WHY "IT IS ONLY A DRIFT HAZARD" WAS
// WRONG. The registry row that opened this work recorded five sites and said of
// every one of them that it was CURRENTLY CORRECT — a hazard, not a defect.
// ✔MEASURED 2026-08-20 by projecting every `EnumNameTable` in the tree and
// comparing its row count against the number of names its loader's message
// advertised: THREE messages had ALREADY drifted.
//   * `operandKinds`      — table has 7 rows, message named 3.
//   * `resultSlot`/`slotKind` — table has 32 rows, message named 8.
//   * `terminatorKind`    — table has 7 rows, message named 6 (`indirect-br`
//                           was accepted by the parse and denied by both
//                           sentences).
// Every one of those is a config author being told, by name, that a spelling
// the loader ACCEPTS is not allowed. The hazard had already fired three times
// before anybody looked.
//
// ★★ WHAT EACH PIN ASSERTS, and why it takes three of them. The claim is
// "the message and the check name the SAME set", which no single assertion can
// carry:
//   (A) CORPUS EXERCISE — the shipped documents really use these vocabularies,
//       at the pointers named below. Without this the red-on-disable is
//       vacuous: deleting a table row would break nothing, because nothing
//       would be reading it.
//   (B) COMPLETENESS — every name in the table appears in the refusal message.
//       This is the direction the three live defects were on, and the only
//       thing that catches them.
//   (C) HONESTY — every vocabulary token the message quotes is one the loader
//       actually ACCEPTS, proven by loading a document that uses it. Catches
//       the mirror defect: a message advertising a spelling the check refuses.
//
// ★★ AND THE MUTANT IS THE TABLE, NOT THE MESSAGE — that is the whole point of
// the change these pins guard. Before it, deleting a name from a message
// reddened nothing anywhere. Now deleting a row from `kDataModelTable` reds
// (A) immediately, because the shipped `c-subset.lang.json` declares
// `coreByDataModel: {"LLP64": …}` at seven pointers and a
// `signatureByDataModel: {"LLP64": …}` at an eighth; the message follows the
// table automatically, so it can no longer be wrong on its own.
//
// ⚠ MUST run through ctest, never a bare `.exe`: `findShippedConfig` walks the
// cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it. A bare
// binary reads whichever config tree the shell happens to stand in.

#include "core/types/config_path_walk.hpp"
#include "core/types/data_model.hpp"
#include "core/types/entry_shape.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "mutate_target_schema.hpp"
#include "vocabulary_projection_probe.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ::dss::allNames;
using ::dss::DiagnosticCode;
using ::dss::findShippedConfig;
using ::dss::GrammarSchema;
using ::dss::ShippedConfigLocator;
using ::dss::TargetSchema;

// A spelling no vocabulary in this tree claims. Deliberately ugly: a probe that
// collided with a real name would make the negative arms pass for the wrong
// reason.
constexpr char const* kBadSpelling = "zzNotAnyEnumSpelling";

// * `ProjectionProbeError`, `shippedLanguageDoc`, `at`, `summarize`,
// `quotedTokens` and `findVocabularyMessage` used to be file-local copies
// here. They now have ONE owner, `vocabulary_projection_probe.hpp`, for the
// reason this whole file exists:
// D-TEST-VOCABULARY-PROJECTION-PROBE-HELPERS-ARE-COPIED-PER-FILE. Two of the
// copies had already drifted from their siblings (`findVocabularyMessage`
// returned a COPY of the message here and a pointer in the target-side file;
// `shippedLanguageDoc` took no argument in the composite-kind file); the
// header records which way each merge resolved.
using ::dss::test_support::at;
using ::dss::test_support::findVocabularyMessage;
using ::dss::test_support::quotedTokens;
using ::dss::test_support::shippedLanguageDoc;
using ::dss::test_support::summarize;
using ProjectionProbeError = ::dss::test_support::VocabularyProbeError;

// (C) HONESTY — the vocabulary sentence quotes no token the table does not own.
// `ignore` carries the non-vocabulary quotes that sentence legitimately
// contains (the offending spelling itself, the JSON field it points at).
void expectAdvertisesNothingElse(std::string const&                msg,
                                 std::span<std::string_view const>  names,
                                 std::span<char const* const>       ignore,
                                 std::string_view                   label) {
    for (auto const& q : quotedTokens(msg)) {
        bool ok = false;
        for (std::string_view const n : names) {
            if (q == n) { ok = true; break; }
        }
        for (char const* const g : ignore) {
            if (q == g) { ok = true; break; }
        }
        EXPECT_TRUE(ok)
            << label << ": the refusal message quotes '" << q
            << "', which is neither a spelling the vocabulary accepts nor a "
               "declared non-vocabulary quote for this site. A message WIDER "
               "than its check sends an author to write a key that will then "
               "be refused.\nmessage was:\n"
            << msg;
    }
}

// The two directions together: the pair of assertions that make "the message
// and the check name the SAME set" a thing a test can fail.
void expectRefusalNamesExactly(auto const&                       diags,
                               std::span<std::string_view const> names,
                               std::span<char const* const>      ignore,
                               std::string_view                  label) {
    std::string const* msg = findVocabularyMessage(diags, names);
    if (msg == nullptr) {
        ADD_FAILURE()
            << label
            << ": NO diagnostic in the refused load names every spelling the "
               "vocabulary table accepts. A message narrower than its check "
               "tells a config author, by name, that a spelling the loader "
               "takes is not allowed — measured live THREE times before this "
               "pin existed (operandKinds 3-of-7, resultSlot 8-of-32, "
               "terminatorKind 6-of-7). Render the list through "
               "`dss::detail::renderAllowedList` over the table's projection, "
               "never as a literal.\ndiagnostics were:"
            << summarize(diags);
        return;
    }
    expectAdvertisesNothingElse(*msg, names, ignore, label);
}

// ── the enum-KEYED MAP sites: the row's exact class ──────────────────────
//
// `pointer` names the map itself in the shipped document. `value` is a value
// that is VALID at that site, so injecting `{name: value}` for each accepted
// name is a clean load rather than a value-shaped rejection.
// ⚠ `pointers` IS A LIST, NOT A SINGLE POINTER, and that is a measured
// requirement rather than generality for its own sake. `coreByLongDoubleFormat`
// occurs on TWO `typeSpecifiers` rows that both declare the vocabulary name
// `long double`, and the loader enforces that rows sharing a name declare the
// SAME representation. Injecting a probe key into one of them alone is refused
// by THAT rule, not by the vocabulary — a rejection the pin would have read as
// "the loader refuses a spelling it advertises", which is exactly backwards.
// Every sibling occurrence takes the same mutation, so the cross-row rule stays
// satisfied and the only variable is the vocabulary.
struct KeyedMapSite {
    char const*                       language;
    std::vector<char const*>          pointers;
    char const*                       value;      // JSON text
    char const*                       label;
    std::span<std::string_view const> names;
    std::span<char const* const>      ignoreQuoted;
};

constexpr char const* kIgnoreProbeOnly[]  = {kBadSpelling};

// ✔MEASURED 2026-08-20 by walking `c-subset.lang.json` for every enum-keyed
// map: `coreByDataModel` occurs at 7 pointers, `coreByLongDoubleFormat` at 2,
// `elementCoreByFormat` at 2, `signatureByDataModel` at 1, `synthesizedTypes`
// at 1 (three roles). One representative pointer of each is probed here, and
// `at()` re-validates it on every run.
inline std::vector<KeyedMapSite> keyedMapSites() {
    static constexpr auto kDataModelNames  = allNames(dss::kDataModelTable);
    static constexpr auto kLongDoubleNames = allNames(dss::kLongDoubleFormatTable);
    return {
        {"c-subset", {"/semantics/builtinTypes/1/coreByDataModel"}, "\"I32\"",
         "coreByDataModel", kDataModelNames, kIgnoreProbeOnly},
        {"c-subset",
         {"/semantics/typeSpecifiers/36/coreByLongDoubleFormat",
          "/semantics/typeSpecifiers/39/coreByLongDoubleFormat"},
         "\"F80\"", "coreByLongDoubleFormat", kLongDoubleNames,
         kIgnoreProbeOnly},
        {"c-subset", {"/semantics/synthesizedTypes/pointerDifference"},
         "\"int\"", "synthesizedTypes/pointerDifference", kDataModelNames,
         kIgnoreProbeOnly},
        {"c-subset",
         {"/hirLowering/stringLiteralPrefixes/4/elementCoreByFormat"},
         "\"I32\"", "elementCoreByFormat",
         dss::kSelectableObjectFormatKindNames, kIgnoreProbeOnly},
    };
}

} // namespace

// ── BASELINE ────────────────────────────────────────────────────────────
//
// The positive control for everything below, and the arm the TABLE mutant
// reds: delete `{DataModel::Llp64, "LLP64"}` from `kDataModelTable` and this
// fails, because the shipped document declares that key at eight pointers.
TEST(EnumVocabularyProjection, ShippedLanguageDocumentsLoadCleanly) {
    for (char const* lang : {"c-subset", "asm-arm64-gas"}) {
        auto const doc = shippedLanguageDoc(lang);
        auto const text = doc.dump();
        auto       r    = GrammarSchema::loadFromText(text, lang);
        ASSERT_TRUE(r.has_value())
            << lang << " must load clean before any mutant means anything: "
            << summarize(r.error());
    }
}

// ── (A) CORPUS EXERCISE ─────────────────────────────────────────────────
//
// Every probed map is non-empty in the shipped document and every key it
// carries resolves through the projection under test. Without this the table
// mutant could delete a row nothing reads, and the whole red-on-disable would
// be theatre.
TEST(EnumVocabularyProjection, ShippedCorpusExercisesEveryProbedVocabulary) {
    for (auto const& s : keyedMapSites()) {
        for (char const* ptr : s.pointers) {
            SCOPED_TRACE(std::string{s.language} + ptr);
            auto        doc  = shippedLanguageDoc(s.language);
            auto const& node = at(doc, ptr, s.label);
            ASSERT_TRUE(node.is_object())
                << s.label << " must be an object for this pin to mean anything";
            std::size_t live = 0;
            for (auto it = node.begin(); it != node.end(); ++it) {
                if (!it.key().empty() && it.key().front() == '$') continue;
                ++live;
                bool known = false;
                for (std::string_view const n : s.names) {
                    if (it.key() == n) { known = true; break; }
                }
                EXPECT_TRUE(known)
                    << s.label << ": the SHIPPED document declares key '"
                    << it.key()
                    << "', which the projection this loader renders does not "
                       "contain — the corpus and the vocabulary disagree";
            }
            EXPECT_GT(live, 0u)
                << s.label
                << ": the shipped document declares NO key here, so deleting a "
                   "row from the vocabulary table would red nothing and the "
                   "red-on-disable for this site is vacuous";
        }
    }
}

// ── (C) HONESTY, proven by LOADING ──────────────────────────────────────
//
// Every spelling the projection advertises is one the loader accepts. Proven
// the only way that is not circular: build a document that uses it and load it.
TEST(EnumVocabularyProjection, EveryAdvertisedSpellingIsAcceptedByTheLoader) {
    for (auto const& s : keyedMapSites()) {
        for (std::string_view const name : s.names) {
            SCOPED_TRACE(std::string{s.label} + " <- '" + std::string{name} + "'");
            auto doc = shippedLanguageDoc(s.language);
            for (char const* ptr : s.pointers) {
                at(doc, ptr, s.label)[std::string{name}] =
                    nlohmann::json::parse(s.value);
            }
            auto const text = doc.dump();
            auto       r    = GrammarSchema::loadFromText(text, s.language);
            EXPECT_TRUE(r.has_value())
                << s.label << " advertises '" << name
                << "' but the loader REFUSES it — the message is wider than "
                   "the check, which sends an author to write a key that will "
                   "then be rejected: "
                << (r.has_value() ? std::string{} : summarize(r.error()));
        }
    }
}

// ── (B) COMPLETENESS + (C) HONESTY, on the MESSAGE ──────────────────────
//
// The unknown key is refused, and the refusal names EXACTLY the vocabulary —
// every spelling the table owns, and no token that is not one.
TEST(EnumVocabularyProjection, RefusalNamesExactlyTheVocabulary) {
    for (auto const& s : keyedMapSites()) {
        SCOPED_TRACE(std::string{s.language} + " " + s.label);
        auto doc = shippedLanguageDoc(s.language);
        for (char const* ptr : s.pointers) {
            auto& node = at(doc, ptr, s.label);
            ASSERT_FALSE(node.contains(kBadSpelling))
                << "the probe spelling already exists — the injection would be "
                   "a no-op and this pin would assert nothing";
            node[kBadSpelling] = nlohmann::json::parse(s.value);
        }
        auto const text = doc.dump();
        auto       r    = GrammarSchema::loadFromText(text, s.language);
        ASSERT_FALSE(r.has_value())
            << s.label
            << ": an unresolvable enum key must FAIL the load — a silently "
               "dropped key leaves the row it names switched off";
        expectRefusalNamesExactly(r.error(), s.names, s.ignoreQuoted, s.label);
    }
}

// ── the SCALAR enum-VALUED sites in the language loader ─────────────────
//
// Same class, different shape: the enum name is a VALUE rather than a key.
// `pointer` addresses the string; the probe replaces it.
namespace {

struct ScalarSite {
    char const*                       language;
    char const*                       pointer;
    char const*                       label;
    std::span<std::string_view const> names;
    std::span<char const* const>      ignoreQuoted;
};

// The non-vocabulary quotes each sentence legitimately carries — the offending
// spelling, and the JSON field the sentence points at.
constexpr char const* kIgnoreBinding[]  = {kBadSpelling, "binding"};
constexpr char const* kIgnoreVis[]      = {kBadSpelling, "visibility"};
constexpr char const* kIgnoreVerb[]     = {kBadSpelling, "verb"};
constexpr char const* kIgnoreSection[]  = {kBadSpelling, "section", "sectionData"};

inline std::vector<ScalarSite> scalarSites() {
    static constexpr auto kBindingNames = allNames(dss::kSymbolBindingTable);
    static constexpr auto kVisNames     = allNames(dss::kSymbolVisibilityTable);
    static constexpr auto kVerbNames    = allNames(dss::kEntryMaterializationTable);
    return {
        {"c-subset",
         "/semantics/declarations/7/linkageSpecifiers/weak/binding",
         "linkageSpecifiers binding", kBindingNames, kIgnoreBinding},
        {"c-subset",
         "/semantics/declarations/7/linkageSpecifiers/visibility:hidden/visibility",
         "linkageSpecifiers visibility", kVisNames, kIgnoreVis},
        {"c-subset", "/semantics/declarations/7/entryFunctions/main/1/params/0",
         "entryFunctions param shape", dss::kDeclarableEntryParamShapeNames,
         kIgnoreProbeOnly},
        {"c-subset", "/semantics/declarations/7/entryFunctions/main/0/verb",
         "entryFunctions verb", kVerbNames, kIgnoreVerb},
    };
}

} // namespace

TEST(EnumVocabularyProjection, ScalarRefusalNamesExactlyTheVocabulary) {
    for (auto const& s : scalarSites()) {
        SCOPED_TRACE(std::string{s.language} + s.pointer);
        auto  doc  = shippedLanguageDoc(s.language);
        auto& node = at(doc, s.pointer, s.label);
        ASSERT_TRUE(node.is_string())
            << s.label << ": the pointer must address a string spelling";
        ASSERT_NE(node.get<std::string>(), kBadSpelling);
        node = kBadSpelling;
        auto const text = doc.dump();
        auto       r    = GrammarSchema::loadFromText(text, s.language);
        ASSERT_FALSE(r.has_value())
            << s.label << ": an unresolvable enum spelling must FAIL the load";
        expectRefusalNamesExactly(r.error(), s.names, s.ignoreQuoted, s.label);
    }
}

// The data-section vocabulary lives in the assembly dialect document, and its
// directive row is found by SEARCHING rather than by index — an index would rot
// into a different row and the pin would quietly probe the wrong thing.
TEST(EnumVocabularyProjection, DataSectionRefusalNamesExactlyTheVocabulary) {
    auto  doc  = shippedLanguageDoc("asm-arm64-gas");
    auto& dirs = at(doc, "/assembly/directives", "assembly directives");
    ASSERT_TRUE(dirs.is_array());
    nlohmann::json* target = nullptr;
    for (auto& row : dirs) {
        if (row.is_object() && row.contains("section")
            && row.at("section").is_string()) {
            target = &row;
            break;
        }
    }
    ASSERT_NE(target, nullptr)
        << "no shipped directive row declares a 'section' — the data-section "
           "vocabulary is unexercised and this pin would assert nothing";
    (*target)["section"] = kBadSpelling;
    auto const text = doc.dump();
    auto       r    = GrammarSchema::loadFromText(text, "asm-arm64-gas");
    ASSERT_FALSE(r.has_value())
        << "an unknown data section must FAIL the load";
    expectRefusalNamesExactly(r.error(), dss::kDataSectionKindNames,
                              kIgnoreSection, "data section");
}

// ── the TARGET loader's vocabularies ────────────────────────────────────
//
// Where two of the three LIVE drifts were. Each row mutates the shipped target
// document through `mutateShippedTargetSchemaDoc`, which THROWS if the mutation
// left the document byte-identical — so a navigator that reached nothing fails
// the test instead of passing over an unmutated schema.
namespace {

using MutateFn = std::function<void(nlohmann::json&)>;

struct TargetSite {
    char const*                       target;
    char const*                       label;
    std::span<std::string_view const> names;
    std::span<char const* const>      ignoreQuoted;
    MutateFn                          mutate;
};

constexpr char const* kIgnoreTerminator[] = {kBadSpelling, "terminatorKind"};
constexpr char const* kIgnoreFormat[]     = {kBadSpelling, "format", "encoding"};

// Find the first opcode row satisfying `pred`, or throw — never an index.
nlohmann::json& firstOpcodeWhere(nlohmann::json& doc, auto pred,
                                 std::string_view what) {
    if (!doc.contains("opcodes") || !doc.at("opcodes").is_array()) {
        throw ProjectionProbeError{"target document carries no 'opcodes' array"};
    }
    for (auto& op : doc.at("opcodes")) {
        if (op.is_object() && pred(op)) return op;
    }
    throw ProjectionProbeError{
        std::string{"no shipped opcode row has "} + std::string{what} +
        " — this pin would probe nothing"};
}

inline std::vector<TargetSite> targetSites() {
    static constexpr auto kResultNames     = allNames(dss::kTargetResultRuleTable);
    static constexpr auto kTerminatorNames = allNames(dss::kTargetTerminatorKindTable);
    static constexpr auto kShapeNames      = allNames(dss::kTargetEncodingShapeTable);
    static constexpr auto kFilterNames     = allNames(dss::kOperandKindFilterTable);
    static constexpr auto kSlotNames       = allNames(dss::kEncodingSlotKindTable);
    static constexpr auto kRegClassNames   = allNames(dss::kTargetRegClassTable);
    return {
        {"x86_64", "opcode result rule", kResultNames, kIgnoreProbeOnly,
         [](nlohmann::json& d) {
             firstOpcodeWhere(d, [](auto const& o) {
                 return o.contains("result") && o.at("result").is_string();
             }, "a string 'result'")["result"] = kBadSpelling;
         }},
        // ⚠ THE ROW THAT WAS ALREADY WRONG: `indirect-br` is a table row and
        // was absent from both terminatorKind sentences.
        {"x86_64", "opcode terminatorKind", kTerminatorNames, kIgnoreTerminator,
         [](nlohmann::json& d) {
             firstOpcodeWhere(d, [](auto const& o) {
                 return o.contains("terminatorKind")
                        && o.at("terminatorKind").is_string();
             }, "a string 'terminatorKind'")["terminatorKind"] = kBadSpelling;
         }},
        {"x86_64", "encoding format", kShapeNames, kIgnoreFormat,
         [](nlohmann::json& d) {
             firstOpcodeWhere(d, [](auto const& o) {
                 return o.contains("encoding") && o.at("encoding").is_object()
                        && o.at("encoding").contains("format");
             }, "an 'encoding.format'")["encoding"]["format"] = kBadSpelling;
         }},
        // ⚠ ALREADY WRONG: table has 7 rows, the sentence named 3.
        {"x86_64", "variant guard operandKinds", kFilterNames, kIgnoreProbeOnly,
         [](nlohmann::json& d) {
             auto& op = firstOpcodeWhere(d, [](auto const& o) {
                 if (!o.contains("encoding") || !o.at("encoding").is_object())
                     return false;
                 auto const& e = o.at("encoding");
                 if (!e.contains("variants") || !e.at("variants").is_array())
                     return false;
                 for (auto const& v : e.at("variants")) {
                     if (v.is_object() && v.contains("guard")
                         && v.at("guard").is_object()
                         && v.at("guard").contains("operandKinds")
                         && v.at("guard").at("operandKinds").is_array()
                         && !v.at("guard").at("operandKinds").empty()) {
                         return true;
                     }
                 }
                 return false;
             }, "a variant guard with operandKinds");
             for (auto& v : op["encoding"]["variants"]) {
                 if (v.is_object() && v.contains("guard")
                     && v.at("guard").contains("operandKinds")
                     && !v.at("guard").at("operandKinds").empty()) {
                     v["guard"]["operandKinds"][0] = kBadSpelling;
                     return;
                 }
             }
         }},
        // ⚠ ALREADY WRONG, and by the widest margin: table has 32 rows, the
        // sentence named 8, grouped by an encoding shape no table owns.
        {"x86_64", "encoding slot kind", kSlotNames, kIgnoreProbeOnly,
         [](nlohmann::json& d) {
             auto& op = firstOpcodeWhere(d, [](auto const& o) {
                 if (!o.contains("encoding") || !o.at("encoding").is_object())
                     return false;
                 auto const& e = o.at("encoding");
                 return e.contains("variants") && e.at("variants").is_array()
                        && !e.at("variants").empty();
             }, "an encoding with variants");
             // ⚠ `resultSlot` IS A KEY OF THE VARIANT, NOT OF ITS `template`.
             // The first draft aimed it at `template` and got the closed-KEY
             // refusal instead of the vocabulary one — a pin probing the
             // wrong node, caught only because the vocabulary assertion had
             // nothing to read.
             op["encoding"]["variants"][0]["resultSlot"] = kBadSpelling;
         }},
        {"x86_64", "register class", kRegClassNames, kIgnoreProbeOnly,
         [](nlohmann::json& d) {
             d.at("registers")[0]["class"] = kBadSpelling;
         }},
    };
}

} // namespace

TEST(EnumVocabularyProjection, TargetRefusalNamesExactlyTheVocabulary) {
    for (auto const& s : targetSites()) {
        SCOPED_TRACE(std::string{s.target} + " / " + s.label);
        auto r = dss::test_support::mutateShippedTargetSchemaDoc(
            s.target, [&s](nlohmann::json& d) { s.mutate(d); });
        ASSERT_FALSE(r.has_value())
            << s.label << ": an unresolvable enum spelling must FAIL the load";
        expectRefusalNamesExactly(r.error(), s.names, s.ignoreQuoted, s.label);
    }
}

// ── the SENTINEL narrowing on `registerClassOps` ────────────────────────
//
// `TargetRegClass::None` is a real enumerator and `targetRegClassFromName`
// resolves it, so the name lookup ALONE accepted `"class": "none"` here while
// the sentence beside it said the accepted set was gpr/fpr/vr/flags. The check
// now refuses it explicitly, and the message projects the same narrowed set.
TEST(EnumVocabularyProjection, RegisterClassOpsRefusesTheNoClassSentinel) {
    auto r = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& d) {
            ASSERT_TRUE(d.contains("registerClassOps"));
            d.at("registerClassOps")[0]["class"] = "none";
        });
    ASSERT_FALSE(r.has_value())
        << "'none' is the no-class sentinel: a registerClassOps row under it "
           "declares move/load/store mnemonics for registers that do not "
           "exist, and it used to LOAD";
    // `none` appears in the sentence as the OFFENDING spelling, not as an
    // advertised one — the site's legitimate non-vocabulary quote.
    static constexpr char const* kIgnoreSentinel[] = {kBadSpelling, "none"};
    expectRefusalNamesExactly(r.error(), dss::kOperableTargetRegClassNames,
                              kIgnoreSentinel, "registerClassOps class");
}

// The shipped target documents themselves stay clean — the positive control
// for every target row above, and the arm a narrowing that went too far would
// red.
TEST(EnumVocabularyProjection, ShippedTargetDocumentsLoadCleanly) {
    for (char const* t : {"x86_64", "arm64"}) {
        auto pathR = findShippedConfig(
            ShippedConfigLocator{t, "targets", ".target.json", "target",
                                 DiagnosticCode::C_InvalidTargetName});
        ASSERT_TRUE(pathR.has_value()) << "cannot resolve shipped target " << t;
        std::ifstream in{*pathR};
        ASSERT_TRUE(in.is_open());
        std::ostringstream buf;
        buf << in.rdbuf();
        auto const text = buf.str();
        auto       r    = TargetSchema::loadFromText(text, t);
        ASSERT_TRUE(r.has_value())
            << t << " must load clean: " << summarize(r.error());
    }
}
