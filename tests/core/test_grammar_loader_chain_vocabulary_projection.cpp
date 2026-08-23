// ── D-CONFIG-GRAMMAR-LOADER-INLINE-CHAIN-VOCABULARIES-REMAIN ─────────────
//    The SWEEP of the owner shape the census could not see, in the one file
//    that held all of it: every inline `==` chain in the grammar loader.
// ─────────────────────────────────────────────────────────────────────────
//
// THE CLASS. Something decides acceptance — here an inline `v == "a"` cascade
// in a loader body, owned by no function at all — and the message beside it
// states the accepted set again as a string literal. Two owners of one fact.
// The decision keeps working while the sentence becomes a lie, and the
// sentence is the half a schema author reads.
//
// ★★★ WHY A WHOLE FILE FOR THIRTEEN VOCABULARIES, AND WHY THE SWEEP HAD TO BE
// COMPLETE RATHER THAN ONE-PER-CYCLE. `compositeKind` — the first member of
// this shape anybody converted — had ALREADY DRIFTED when it was found, to 2
// of its 3 spellings, telling a schema author BY NAME that `enum` was not
// allowed. It sat in THIS FILE'S subject undetected for exactly as long as the
// census had no arm that could see an inline chain. The remaining chains were
// not drifted on the day they were converted; every one of them was a live
// retype waiting to, and the value is in there being none left.
//
// ★★ THE TRAP THIS FILE IS BUILT AROUND, AND IT IS THE REASON THE EXPECTATIONS
// ARE HAND-WRITTEN. A pin whose expectation comes off the SAME TABLE the code
// projects moves BOTH HALVES OF THE COMPARISON TOGETHER: rename a row and the
// message follows it, so "the message names every table row" stays green while
// the vocabulary silently changed under every config in the tree. ✔MEASURED as
// a live weakness of that shape in cycle P23. So `kVocabularies` below states
// each accepted set DIRECTLY, as literals, with a LITERAL count — and every
// arm compares against THAT, never against `allNames(...)`. The table is one
// side of the comparison; this file is the other, and they cannot move
// together.
//
// WHAT THE FOUR ARMS ASSERT, and it takes four because "the message and the
// check name the same set" is not one claim:
//   (A) THE TABLE IS WHAT THIS FILE SAYS IT IS — `allNames(table)` equals the
//       hand-written list, in order, at the hand-written count. The arm a
//       table-row mutant reds, and the only one that cannot be satisfied by
//       moving the code.
//   (B) CORPUS EXERCISE — the shipped documents really write these keys, and
//       every spelling they write is in the list and resolves through the
//       table's `…FromName`. Without it a mutant could rename a row nothing
//       reads and nothing would notice.
//   (C) COMPLETENESS — the refusal AT THE KEY'S OWN JSON POINTER names every
//       spelling in the list. This is the direction the live drift was on.
//   (D) HONESTY — the refusal quotes no vocabulary token outside the list, so
//       a message WIDER than its check cannot send an author to write a value
//       that is then refused.
//
// ⚠ (C) and (D) are keyed on the JSON POINTER the loader reports, not on "some
// diagnostic in the load". Several of these keys are refused by two or three
// different arms (missing / wrong type / unknown value), and an honest sibling
// arm would otherwise cover for a drifted one on every run — which is exactly
// how `compositeKind`'s drift survived.
//
// ⚠ MUST run through ctest, never a bare `.exe`: `findShippedConfig` walks the
// cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it.

#include "core/types/compiled_shape.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/hir_lowering_config.hpp"
#include "core/types/import_config.hpp"
#include "core/types/lexer_mode.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/string_style.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "vocabulary_projection_probe.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

using ::dss::test_support::at;
using ::dss::test_support::quotedTokens;
using ::dss::test_support::shippedLanguageDoc;
using ::dss::test_support::summarize;

// A spelling no vocabulary in this tree claims. Deliberately ugly: a probe that
// collided with a real name would make the negative arms pass for the wrong
// reason.
constexpr char const* kBadSpelling = "zzNotAnySpellingAtAll";

// ── FINDING THE KEY, NEVER INDEXING TO IT ────────────────────────────────
//
// Every pointer below is found by SEARCH. An index rots into a different row
// and the pin then probes something else while staying green — and unlike a
// stale index, an empty result is caught by the emptiness guard in every arm.
void collectObjectsWithKey(nlohmann::json const& node, std::string const& ptr,
                           std::string_view key, std::vector<std::string>& out) {
    if (node.is_object()) {
        if (node.contains(std::string{key})) out.push_back(ptr);
        for (auto const& [k, v] : node.items()) {
            std::string esc;
            for (char const c : k) {
                if (c == '~')      esc += "~0";
                else if (c == '/') esc += "~1";
                else               esc += c;
            }
            collectObjectsWithKey(v, ptr + "/" + esc, key, out);
        }
        return;
    }
    if (node.is_array()) {
        for (std::size_t i = 0; i < node.size(); ++i) {
            collectObjectsWithKey(node[i], ptr + "/" + std::to_string(i), key, out);
        }
    }
}

[[nodiscard]] std::vector<std::string>
objectsWithKey(nlohmann::json const& doc, std::string_view key) {
    std::vector<std::string> out;
    collectObjectsWithKey(doc, "", key, out);
    return out;
}

// The pointers whose OWN pointer text contains `needle` — for the keys whose
// spelling is overloaded across the schema. `kind` names a token kind at 245
// pointers in `c-subset` and a type-extension parameter kind at two in
// `tsql-subset`; without this the probe would perturb a token declaration and
// assert against a diagnostic from an unrelated subsystem.
[[nodiscard]] std::vector<std::string>
objectsWithKeyUnder(nlohmann::json const& doc, std::string_view key,
                    std::string_view needle) {
    std::vector<std::string> out;
    for (auto const& p : objectsWithKey(doc, key)) {
        if (p.find(needle) != std::string::npos) out.push_back(p);
    }
    return out;
}

// Where the loader reports the refusal, relative to the object we perturbed.
enum class RefusalPath {
    KeyChild,      // `<object pointer>/<key>` — the common case
    ObjectItself,  // the loader reports the enclosing object's pointer
};

// A document surgery: take the shipped doc, return the pointer of the object
// carrying the key. Some vocabularies are not written by ANY shipped document
// (`typeShapes[].constructor`); those mutate the doc to introduce one, which is
// why this takes a mutable reference.
using Locator = std::function<std::vector<std::string>(nlohmann::json&)>;

struct VocabularyCase {
    std::string_view              label;
    std::string_view              language;
    std::string_view              key;
    // ★ THE ACCEPTED SET, STATED HERE AND NOWHERE ELSE IN THIS FILE. Not
    // derived from the table — that is the whole point (see this file's
    // header). Order matters: arm (A) compares in declaration order.
    std::vector<std::string_view> allowed;
    // Written as a literal so a table that loses a row and a `<N>` that loses a
    // row cannot cancel out. `allowed.size()` is NOT this number's source.
    std::size_t                   allowedCount;
    // Tokens the refusal legitimately quotes that are not vocabulary members:
    // the key it points at, a dotted spelling of it, a sibling key it names.
    std::vector<std::string_view> extraQuotes;
    RefusalPath                   refusalPath;
    Locator                       locate;
    // The spellings the SHIPPED corpus is expected to write at this key. Empty
    // means "no shipped document writes this key" — declared per case so the
    // corpus arm can tell an unexercised vocabulary from a broken probe.
    bool                          corpusWritesIt;
};

[[nodiscard]] Locator plainKey(std::string_view key) {
    return [key](nlohmann::json& doc) { return objectsWithKey(doc, key); };
}

[[nodiscard]] Locator keyUnder(std::string_view key, std::string_view needle) {
    return [key, needle](nlohmann::json& doc) {
        return objectsWithKeyUnder(doc, key, needle);
    };
}

// ── THE THIRTEEN ─────────────────────────────────────────────────────────
//
// One row per vocabulary that was an inline `==` chain in
// `grammar_schema_json.cpp`. `modeOp` is listed at its DECLARABLE set (three of
// its table's four rows); the sentinel it excludes gets its own test below,
// because "the loader refuses the spelling its own table resolves" is a
// different claim from "the message names the set".
[[nodiscard]] std::vector<VocabularyCase> vocabularies() {
    std::vector<VocabularyCase> v;
    v.push_back({"modeOp", "c-subset", "modeOp",
                 {"pushMode", "popMode", "replaceMode"}, 3,
                 {"modeOp"}, RefusalPath::KeyChild, plainKey("modeOp"), true});
    v.push_back({"escapeKind", "c-subset", "escapeKind",
                 {"none", "char", "doubled-delimiter"}, 3,
                 {"escapeKind"}, RefusalPath::KeyChild, plainKey("escapeKind"), true});
    v.push_back({"unterminatedAs", "c-subset", "unterminatedAs",
                 {"string", "comment", "generic"}, 3,
                 {"unterminatedAs"}, RefusalPath::KeyChild,
                 plainKey("unterminatedAs"), true});
    v.push_back({"reservedWordPolicy", "tsql-subset", "reservedWordPolicy",
                 {"strict", "contextual"}, 2,
                 {"reservedWordPolicy"}, RefusalPath::KeyChild,
                 plainKey("reservedWordPolicy"), true});
    v.push_back({"commitRequiresTypeName.polarity", "c-subset", "polarity",
                 {"preferType", "requireKnownType"}, 2,
                 {"polarity", "commitRequiresTypeName.polarity"},
                 RefusalPath::KeyChild, plainKey("polarity"), true});
    v.push_back({"imports.strategy", "c-subset", "strategy",
                 {"none", "include-following", "name-matching"}, 3,
                 {"strategy", "imports.strategy"}, RefusalPath::KeyChild,
                 keyUnder("strategy", "/imports"), true});
    v.push_back({"nameMatch", "c-subset", "nameMatch",
                 {"self", "lastIdentifier"}, 2,
                 {"nameMatch"}, RefusalPath::KeyChild, plainKey("nameMatch"), true});
    v.push_back({"typeShapes[].constructor", "c-subset", "constructor",
                 {"pointer", "reference", "nullable", "optional", "slice"}, 5,
                 {"constructor"}, RefusalPath::KeyChild,
                 // No shipped document declares a `typeShapes` block, so this
                 // one is INTRODUCED. The `rule` names a shape the document
                 // really declares, so the only thing left to refuse is the
                 // constructor spelling.
                 [](nlohmann::json& doc) -> std::vector<std::string> {
                     if (!doc.contains("shapes") || doc.at("shapes").empty()) return {};
                     auto const ruleName = doc.at("shapes").items().begin().key();
                     doc["semantics"]["typeShapes"] = nlohmann::json::array(
                         {{{"rule", ruleName}, {"constructor", "pointer"}}});
                     return {"/semantics/typeShapes/0"};
                 },
                 false});
    v.push_back({"arithmeticConversions.shiftResult", "c-subset", "shiftResult",
                 {"promotedLeft", "commonType"}, 2,
                 {"shiftResult"}, RefusalPath::KeyChild, plainKey("shiftResult"), true});
    v.push_back({"childGathering[].lower", "tsql-subset", "lower",
                 {"expr", "flatExpr", "ext", "ref", "varDecl"}, 5,
                 {"lower"}, RefusalPath::KeyChild, plainKey("lower"), true});
    v.push_back({"operators[].associativity", "c-subset", "associativity",
                 {"none", "left", "right"}, 3,
                 {"associativity"}, RefusalPath::ObjectItself,
                 plainKey("associativity"), true});
    v.push_back({"operators[].arity", "c-subset", "arity",
                 {"infix", "prefix", "postfix", "ternary"}, 4,
                 {"arity"}, RefusalPath::ObjectItself, plainKey("arity"), true});
    v.push_back({"typeExtensions[].parameters[].kind", "tsql-subset", "kind",
                 {"Integer", "Type"}, 2,
                 {"kind", "name"}, RefusalPath::ObjectItself,
                 keyUnder("kind", "/typeExtensions/"), true});
    return v;
}

// The accepted set as the TABLES project it, keyed by the same label. Arm (A)
// is the comparison of these two lists and nothing else; keeping them in
// separate functions is what stops a future edit from "fixing" a red by
// deriving one from the other.
[[nodiscard]] std::vector<std::string_view>
projectedNames(std::string_view label) {
    auto const copy = [](auto const& arr) {
        return std::vector<std::string_view>(arr.begin(), arr.end());
    };
    if (label == "modeOp")            return copy(kDeclarableModeOpNames);
    if (label == "escapeKind")        return copy(allNames(kEscapeKindTable));
    if (label == "unterminatedAs")    return copy(allNames(kUnterminatedFlavorTable));
    if (label == "reservedWordPolicy")return copy(allNames(kReservedWordPolicyTable));
    if (label == "commitRequiresTypeName.polarity")
        return copy(allNames(kTypeNameCommitPolarityTable));
    if (label == "imports.strategy")  return copy(allNames(kImportStrategyTable));
    if (label == "nameMatch")         return copy(allNames(kNameMatchModeTable));
    if (label == "typeShapes[].constructor")
        return copy(allNames(kTypeConstructorTable));
    if (label == "arithmeticConversions.shiftResult")
        return copy(allNames(kShiftResultRuleTable));
    if (label == "childGathering[].lower")
        return copy(allNames(kChildLowerTable));
    if (label == "operators[].associativity")
        return copy(allNames(kOperatorAssocTable));
    if (label == "operators[].arity") return copy(allNames(kOperatorArityTable));
    if (label == "typeExtensions[].parameters[].kind")
        return copy(allNames(kTypeParamKindTable));
    return {};
}

// Does `…FromName` resolve this spelling? Same keying, same reason.
[[nodiscard]] bool resolves(std::string_view label, std::string const& spelling) {
    if (label == "modeOp") {
        auto const m = modeOpFromName(spelling);
        return m.has_value() && isDeclarableModeOp(*m);
    }
    if (label == "escapeKind")        return escapeKindFromName(spelling).has_value();
    if (label == "unterminatedAs")    return unterminatedFlavorFromName(spelling).has_value();
    if (label == "reservedWordPolicy")return reservedWordPolicyFromName(spelling).has_value();
    if (label == "commitRequiresTypeName.polarity")
        return typeNameCommitPolarityFromName(spelling).has_value();
    if (label == "imports.strategy")  return importStrategyFromName(spelling).has_value();
    if (label == "nameMatch")         return nameMatchModeFromName(spelling).has_value();
    if (label == "typeShapes[].constructor")
        return typeConstructorFromName(spelling).has_value();
    if (label == "arithmeticConversions.shiftResult")
        return shiftResultRuleFromName(spelling).has_value();
    if (label == "childGathering[].lower")
        return childLowerFromName(spelling).has_value();
    if (label == "operators[].associativity")
        return operatorAssocFromName(spelling).has_value();
    if (label == "operators[].arity") return operatorArityFromName(spelling).has_value();
    if (label == "typeExtensions[].parameters[].kind")
        return typeParamKindFromName(spelling).has_value();
    return false;
}

[[nodiscard]] std::optional<std::string>
messageAt(auto const& diags, std::string const& path) {
    for (auto const& d : diags) {
        if (d.path == path) return d.message;
    }
    return std::nullopt;
}

} // namespace

// ── (A) THE TABLE IS WHAT THIS FILE SAYS IT IS ──────────────────────────
//
// The only arm whose expectation does not come off the code. Rename a row in
// any of the thirteen tables and this reds, while every projected sentence
// obediently follows the rename and every "the message names the table" pin
// stays green. That asymmetry is the reason this arm exists.
TEST(GrammarLoaderChainVocabulary, EveryTableEqualsItsHandWrittenSet) {
    auto const cases = vocabularies();
    ASSERT_EQ(cases.size(), 13u)
        << "the sweep is sized here, as a literal: a vocabulary dropped from "
           "this list is a vocabulary nothing in this file checks";
    for (auto const& c : cases) {
        SCOPED_TRACE(std::string{c.label});
        ASSERT_FALSE(c.allowed.empty())
            << "an empty accepted set would make every arm below pass "
               "vacuously";
        ASSERT_EQ(c.allowed.size(), c.allowedCount)
            << "the hand-written list and the hand-written count disagree — "
               "the count is a LITERAL precisely so a row deleted from both "
               "the table and the list cannot cancel out";
        auto const projected = projectedNames(c.label);
        ASSERT_FALSE(projected.empty())
            << "no table is wired to this label, so nothing is being compared";
        ASSERT_EQ(projected.size(), c.allowedCount)
            << "the table projects " << projected.size() << " spellings, this "
               "file states " << c.allowedCount;
        for (std::size_t i = 0; i < projected.size(); ++i) {
            EXPECT_EQ(projected[i], c.allowed[i])
                << "row " << i << ": the table spells '" << projected[i]
                << "', this file states '" << c.allowed[i]
                << "'. Every refusal in the loader renders the TABLE, so a "
                   "renamed row silently re-spells a config key across every "
                   "document in the tree and every message-follows-the-table "
                   "pin stays green through it";
        }
    }
}

// ── (B) CORPUS EXERCISE ─────────────────────────────────────────────────
//
// The shipped documents really write these keys, and every spelling they write
// resolves. Without this a table mutant could rename a row nothing reads.
TEST(GrammarLoaderChainVocabulary, ShippedCorpusWritesEveryKeyItIsExpectedTo) {
    for (auto const& c : vocabularies()) {
        SCOPED_TRACE(std::string{c.label});
        auto       doc      = shippedLanguageDoc(std::string{c.language});
        auto const pointers = c.locate(doc);
        ASSERT_FALSE(pointers.empty())
            << "the probe found NO object carrying '" << c.key << "' in "
            << c.language
            << " — a locator that finds nothing makes every arm below vacuous";
        if (!c.corpusWritesIt) continue;
        std::size_t seen = 0;
        for (auto const& ptr : pointers) {
            auto        copy = doc;
            auto const& node = at(copy, ptr, std::string{c.label});
            if (!node.contains(std::string{c.key})) continue;
            if (!node.at(std::string{c.key}).is_string()) continue;
            auto const spelling = node.at(std::string{c.key}).get<std::string>();
            ++seen;
            EXPECT_TRUE(resolves(c.label, spelling))
                << "the SHIPPED document writes '" << spelling << "' at " << ptr
                << ", and the vocabulary's `…FromName` does not resolve it — "
                   "the corpus and the table disagree";
            bool listed = false;
            for (auto const& a : c.allowed) {
                if (a == spelling) { listed = true; break; }
            }
            EXPECT_TRUE(listed)
                << "the SHIPPED document writes '" << spelling << "' at " << ptr
                << ", which this file does not list as accepted";
        }
        EXPECT_GT(seen, 0u)
            << "no shipped object actually carries a STRING at '" << c.key
            << "', so the corpus exercises nothing here";
    }
}

// ── (C)+(D) THE REFUSAL AT THE KEY'S OWN POINTER ────────────────────────
//
// Write a spelling no vocabulary claims, then read the diagnostic the loader
// reports AT THAT POINTER — not "some diagnostic in the load". Every one of
// these keys has two or three refusal arms, and an honest sibling arm covering
// for a drifted one is precisely how the `compositeKind` drift survived.
TEST(GrammarLoaderChainVocabulary, EveryRefusalNamesItsWholeAcceptedSet) {
    for (auto const& c : vocabularies()) {
        SCOPED_TRACE(std::string{c.label});
        auto       doc      = shippedLanguageDoc(std::string{c.language});
        auto const pointers = c.locate(doc);
        ASSERT_FALSE(pointers.empty());
        auto const& ptr = pointers.front();
        at(doc, ptr, std::string{c.label})[std::string{c.key}] = kBadSpelling;

        auto const text = doc.dump();
        auto       r    = GrammarSchema::loadFromText(text, std::string{c.language});
        ASSERT_FALSE(r.has_value())
            << "an unresolvable '" << c.key << "' must FAIL the load";

        std::string const expected =
            c.refusalPath == RefusalPath::KeyChild ? ptr + "/" + std::string{c.key}
                                                   : ptr;
        auto const found = messageAt(r.error(), expected);
        ASSERT_TRUE(found.has_value())
            << "no diagnostic was reported AT '" << expected
            << "'. Either the loader stopped refusing this, or it moved the "
               "pointer — and a pin that cannot find its arm asserts nothing."
            << summarize(r.error());

        // (C) COMPLETENESS — against the HAND-WRITTEN list.
        for (auto const& n : c.allowed) {
            bool named = false;
            for (auto const& q : quotedTokens(*found)) {
                if (q == n) { named = true; break; }
            }
            EXPECT_TRUE(named)
                << "the refusal at '" << expected << "' does NOT name '" << n
                << "', which this loader accepts. That is a schema author being "
                   "told BY NAME that a spelling the loader takes is not "
                   "allowed — the defect measured live on `compositeKind`. "
                   "Render the set through `renderAllowedList`, never as a "
                   "literal.\nmessage was:\n"
                << *found;
        }

        // (D) HONESTY — no quoted token that is neither a spelling nor one of
        // the non-vocabulary quotes this sentence legitimately carries.
        for (auto const& q : quotedTokens(*found)) {
            bool ok = (q == kBadSpelling);
            for (auto const& n : c.allowed) {
                if (q == n) { ok = true; break; }
            }
            for (auto const& e : c.extraQuotes) {
                if (q == e) { ok = true; break; }
            }
            EXPECT_TRUE(ok)
                << "the refusal at '" << expected << "' quotes '" << q
                << "', which is neither a spelling this loader accepts nor a "
                   "declared non-vocabulary quote. A message WIDER than its "
                   "check sends an author to write a value that will then be "
                   "refused.\nmessage was:\n"
                << *found;
        }
    }
}

// ── THE SENTINEL, WHICH IS A CLAIM NO PROJECTION CAN CARRY ──────────────
//
// `kModeOpTable` lists `none` so `modeOpName` has a spelling for the
// no-mode-effect value; the LOADER must still refuse it, because a lexeme with
// no mode effect declares no `modeOp` at all. That is the one place where
// "resolves through the table" and "accepted by the loader" deliberately
// disagree, and a projection of either alone cannot state it.
TEST(GrammarLoaderChainVocabulary, ModeOpSentinelResolvesButIsRefused) {
    ASSERT_EQ(allNames(kModeOpTable).size(), 4u)
        << "the full table carries the sentinel plus the three declarable ops";
    ASSERT_EQ(kDeclarableModeOpNames.size(), 3u);

    auto const sentinel = modeOpFromName("none");
    ASSERT_TRUE(sentinel.has_value())
        << "'none' must resolve — `modeOpName(ModeOp::None)` is what renders it";
    EXPECT_FALSE(isDeclarableModeOp(*sentinel));
    EXPECT_EQ(modeOpName(ModeOp::None), "none");

    auto       doc      = shippedLanguageDoc("c-subset");
    auto const pointers = objectsWithKey(doc, "modeOp");
    ASSERT_FALSE(pointers.empty());
    at(doc, pointers.front(), "modeOp / sentinel")["modeOp"] = "none";
    auto const text = doc.dump();
    auto       r    = GrammarSchema::loadFromText(text, "c-subset");
    ASSERT_FALSE(r.has_value())
        << "`modeOp: \"none\"` must FAIL the load — the table resolves the "
           "spelling and the loader refuses it, which is the whole reason "
           "`kDeclarableModeOpNames` exists";
    auto const found = messageAt(r.error(), pointers.front() + "/modeOp");
    ASSERT_TRUE(found.has_value()) << summarize(r.error());

    // ⚠ SCOPED TO THE ACCEPTED-SET HALF OF THE SENTENCE. The refusal quotes the
    // OFFENDING VALUE too, and that occurrence of `'none'` is correct — it is
    // what the author wrote. A blanket "the message never says 'none'" asserts
    // the wrong thing and fails on a message that is right; ✔MEASURED here on
    // the first run of this pin.
    auto const marker = found->find("(expected ");
    ASSERT_NE(marker, std::string::npos)
        << "the refusal no longer carries an '(expected …)' clause, so there is "
           "no accepted-set half to check.\nmessage was:\n"
        << *found;
    auto const advertised = quotedTokens(found->substr(marker));
    for (auto const& q : advertised) {
        EXPECT_NE(q, "none")
            << "the refusal ADVERTISES 'none', which it then refuses — the "
               "accepted set must render `kDeclarableModeOpNames`, not "
               "`allNames(kModeOpTable)`.\nmessage was:\n"
            << *found;
    }
    EXPECT_EQ(advertised.size(), 3u)
        << "the accepted-set clause names " << advertised.size()
        << " spellings; the declarable set has three";
}

// ── THE ROLE TABLE IS THE DISPATCH KEY ──────────────────────────────────
//
// `semantics.synthesizedTypes` is the member of this class where the second
// owner is a ROUTER rather than a sentence: a name array decided acceptance and
// an `if (key == …)` cascade decided what each accepted key DID. A role in the
// array with no arm in the cascade loads clean and is silently dropped. The
// two are now one table of (name, member), so this pin asserts what that buys:
// the refusal names exactly the roles the loader actually fills.
TEST(GrammarLoaderChainVocabulary, SynthesizedTypeRolesAreNamedAndDispatched) {
    constexpr std::string_view kRoles[] = {"sizeof", "alignof",
                                           "pointerDifference"};
    auto doc = shippedLanguageDoc("c-subset");
    ASSERT_TRUE(doc.at("semantics").contains("synthesizedTypes"))
        << "the shipped document must declare the block, or this pin probes "
           "nothing";

    // Each declared role must actually REACH a config member. Proven the only
    // non-circular way: load the shipped document and read the results back.
    auto const clean = GrammarSchema::loadFromText(doc.dump(), "c-subset");
    ASSERT_TRUE(clean.has_value()) << summarize(clean.error());
    auto const& sem = (*clean)->semantics();
    EXPECT_FALSE(sem.sizeofResultType.byDataModel.empty())
        << "the 'sizeof' role declared by the shipped document did not reach "
           "`sizeofResultType` — an accepted-then-dropped role is exactly the "
           "silent failure the (name, member) table exists to make impossible";
    EXPECT_FALSE(sem.alignofResultType.byDataModel.empty())
        << "the 'alignof' role did not reach `alignofResultType`";
    EXPECT_FALSE(sem.pointerDifferenceType.byDataModel.empty())
        << "the 'pointerDifference' role did not reach `pointerDifferenceType`";

    // And the unknown-key refusal names every role, projected from the same
    // table the dispatch walks.
    doc["semantics"]["synthesizedTypes"][kBadSpelling] = nlohmann::json::object();
    auto r = GrammarSchema::loadFromText(doc.dump(), "c-subset");
    ASSERT_FALSE(r.has_value()) << "an unknown synthesizedTypes role must FAIL";
    auto const found = messageAt(
        r.error(), std::string{"/semantics/synthesizedTypes/"} + kBadSpelling);
    ASSERT_TRUE(found.has_value()) << summarize(r.error());
    for (auto const& role : kRoles) {
        bool named = false;
        for (auto const& q : quotedTokens(*found)) {
            if (q == role) { named = true; break; }
        }
        EXPECT_TRUE(named)
            << "the refusal does not name the role '" << role
            << "', which the loader dispatches.\nmessage was:\n"
            << *found;
    }
}

// ── THE FLAG LIST, WHICH USED TO IGNORE WHAT IT DID NOT KNOW ───────────
//
// D-CONFIG-GRAMMAR-LOADER-FLAG-LIST-SILENTLY-DROPS-AN-UNKNOWN-SPELLING. The
// `flags` chain was the one member of this family whose second owner was not a
// sentence but the ABSENCE of one: its `else` arm ignored an unknown spelling
// outright, so a misspelled `"EmtpySpace"` left the token unflagged and the
// parse failed somewhere else entirely, with nothing pointing at the typo.
// These pins assert the refusal exists, names its whole set, and still accepts
// every spelling it advertises.
TEST(GrammarLoaderChainVocabulary, FlagListRefusesAnUnknownSpelling) {
    // Stated here, as literals, for the reason the whole file states its sets
    // here: an expectation read off the loader's own table cannot catch the
    // loader's own table changing.
    constexpr std::string_view kFlags[] = {"EmptySpace", "Missing", "Synthetic",
                                           "HasError"};
    ASSERT_EQ(std::size(kFlags), 4u);

    auto const pointerOfSomeFlagArray = [](nlohmann::json const& doc) {
        for (auto const& p : objectsWithKey(doc, "flags")) {
            auto const& node = at(const_cast<nlohmann::json&>(doc), p, "flags");
            if (node.at("flags").is_array()) return p;
        }
        return std::string{};
    };

    auto const doc = shippedLanguageDoc("c-subset");
    auto const ptr = pointerOfSomeFlagArray(doc);
    ASSERT_FALSE(ptr.empty())
        << "no shipped object carries a 'flags' array, so this pin probes "
           "nothing";

    // THE REFUSAL. The diagnostic is keyed on the ARRAY ELEMENT's pointer — the
    // bad spelling is written at index 1 and nothing else in the document is
    // perturbed, so `…/flags/1` names this element and no other.
    {
        auto bad = doc;
        at(bad, ptr, "flags / unknown")["flags"] =
            nlohmann::json::array({"EmptySpace", kBadSpelling});
        auto r = GrammarSchema::loadFromText(bad.dump(), "c-subset");
        ASSERT_FALSE(r.has_value())
            << "an unknown node flag must FAIL the load — ignoring it leaves "
               "the behaviour it names switched off with no diagnostic, which "
               "is the defect this refusal exists to end";
        std::optional<std::string> found;
        for (auto const& d : r.error()) {
            if (d.path.size() >= 8
                && d.path.compare(d.path.size() - 8, 8, "/flags/1") == 0) {
                found = d.message;
                break;
            }
        }
        ASSERT_TRUE(found.has_value())
            << "no diagnostic was reported at the offending flag's own array "
               "index — a refusal that cannot point at the element asserts "
               "nothing about which one was wrong."
            << summarize(r.error());
        for (auto const& n : kFlags) {
            bool named = false;
            for (auto const& q : quotedTokens(*found)) {
                if (q == n) { named = true; break; }
            }
            EXPECT_TRUE(named)
                << "the refusal does not name '" << n
                << "', which the loader accepts.\nmessage was:\n"
                << *found;
        }
        for (auto const& q : quotedTokens(*found)) {
            bool ok = (q == kBadSpelling) || (q == "flags");
            for (auto const& n : kFlags) {
                if (q == n) { ok = true; break; }
            }
            EXPECT_TRUE(ok)
                << "the refusal quotes '" << q
                << "', which it does not accept.\nmessage was:\n"
                << *found;
        }
    }

    // AND THE OTHER DIRECTION — every advertised spelling still loads.
    for (auto const& n : kFlags) {
        SCOPED_TRACE(std::string{"flags <- ["} + std::string{n} + "]");
        auto ok = doc;
        at(ok, ptr, "flags / accepted")["flags"] =
            nlohmann::json::array({std::string{n}});
        auto r = GrammarSchema::loadFromText(ok.dump(), "c-subset");
        if (r.has_value()) continue;
        for (auto const& d : r.error()) {
            EXPECT_TRUE(d.path.find("/flags") == std::string::npos)
                << "the refusal advertises '" << n
                << "' and the loader then rejects it: " << d.message;
        }
    }
}

// ── THE OTHER DIRECTION: THE ADVERTISED SPELLINGS REALLY LOAD ───────────
//
// A message wider than its check is the mirror defect, and the tables carry a
// value the shipped corpus does not write for several of these vocabularies —
// so "the corpus exercises it" cannot answer this. Writing each advertised
// spelling into a real document and loading it can. The assertion is scoped to
// the KEY'S OWN POINTER: some spellings legitimately trigger a diagnostic
// ELSEWHERE (`escapeKind: char` requires a sibling `escapeChar`), and that is
// the loader being right, not the message being wide.
TEST(GrammarLoaderChainVocabulary, EveryAdvertisedSpellingIsAcceptedAtItsKey) {
    for (auto const& c : vocabularies()) {
        SCOPED_TRACE(std::string{c.label});
        for (auto const& spelling : c.allowed) {
            SCOPED_TRACE(std::string{c.key} + " <- '" + std::string{spelling} + "'");
            auto       doc      = shippedLanguageDoc(std::string{c.language});
            auto const pointers = c.locate(doc);
            ASSERT_FALSE(pointers.empty());
            auto const& ptr = pointers.front();
            at(doc, ptr, std::string{c.label})[std::string{c.key}] =
                std::string{spelling};

            auto r = GrammarSchema::loadFromText(doc.dump(),
                                                 std::string{c.language});
            if (r.has_value()) continue;
            std::string const expected =
                c.refusalPath == RefusalPath::KeyChild
                    ? ptr + "/" + std::string{c.key}
                    : ptr;
            auto const found = messageAt(r.error(), expected);
            if (!found.has_value()) continue;
            bool namesTheValue = false;
            for (auto const& q : quotedTokens(*found)) {
                if (q == spelling) { namesTheValue = true; break; }
            }
            EXPECT_FALSE(namesTheValue)
                << "the refusal advertises '" << spelling
                << "' and the loader then rejects it AT ITS OWN POINTER — a "
                   "message wider than its check sends an author to write a "
                   "value that is refused.\nmessage was:\n"
                << *found;
        }
    }
}
