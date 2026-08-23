// ── D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET ──────────
//    The member of the class that the CENSUS COULD NOT SEE: a closed
//    vocabulary whose only owner was an INLINE `==` chain in a loader body.
// ─────────────────────────────────────────────────────────────────────────
//
// THE CLASS. Something decides acceptance, and the message beside it states the
// accepted set again as a string literal. Two owners of one fact; the decision
// keeps working while the sentence becomes a lie, and the sentence is the half a
// schema author reads.
//
// ★★★ WHY THIS FILE EXISTS SEPARATELY FROM `test_config_enum_vocabulary_
// projection.cpp`, AND IT IS NOT THE VOCABULARY — IT IS THE GRANULARITY.
// `CompositeKind` had no `EnumNameTable` and no `…FromName`: its three spellings
// lived in TWO inline `k == "struct"/"union"/"enum"` chains in
// `grammar_schema_json.cpp`, with FIVE sentences restating them. ✔MEASURED
// 2026-08-21: ONE of the five had already drifted —
//
//     "'compositeKind' must be a string 'struct' or 'union'"
//
// on the `fieldChildren` TYPE-ERROR arm, three lines above a chain that accepts
// `enum` as well. Its two sibling arms named all three.
//
// ★★ AND THAT IS EXACTLY WHAT A PER-LOAD PIN CANNOT CATCH. The established
// helper in the sibling file searches a refused load for SOME diagnostic naming
// the whole set — the right shape there, because a bad enum key produces one
// vocabulary sentence plus cascade noise. Here the three arms are three
// DIFFERENT refusals of one key, and the two honest ones would have covered for
// the drifted one on every run. So these pins are keyed on the JSON PATH the
// loader reports: the diagnostic AT `…/fieldChildren/compositeKind` must itself
// name the whole vocabulary, whichever arm produced it. That is the assertion
// the historical defect fails.
//
// WHAT THE FOUR PINS ASSERT, and it takes four because "the message and the
// check name the same set" is not one claim:
//   (A) CORPUS EXERCISE — the shipped `c-subset.lang.json` really declares every
//       spelling, on BOTH axes. Without it, deleting a table row would break
//       nothing and the red-on-disable would be theatre.
//   (B) COMPLETENESS — every spelling the table owns appears in each arm's
//       refusal. This is the direction the live drift was on.
//   (C) HONESTY — the refusal quotes no vocabulary token the loader does not
//       accept, proven by loading a document that uses each one.
//   (D) THE PARSE FOLLOWS THE TABLE — each advertised spelling actually reaches
//       the enum, so message and behaviour cannot part company.
//
// ⚠ MUST run through ctest, never a bare `.exe`: `findShippedConfig` walks the
// cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it.

#include "core/types/config_path_walk.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"
#include "vocabulary_projection_probe.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

// ⓘ `<fstream>` / `<sstream>` / `<stdexcept>` are GONE with the probe helpers:
// nothing in this file opens a stream or throws any more — the shared
// `vocabulary_projection_probe.hpp` does both.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// A spelling no vocabulary in this tree claims. Deliberately ugly: a probe that
// collided with a real name would make the negative arms pass for the wrong
// reason.
constexpr char const* kBadSpelling = "zzNotAnyCompositeKind";
constexpr char const* kLanguage    = "c-subset";

using ::dss::test_support::at;
using ::dss::test_support::quotedTokens;
using ::dss::test_support::shippedLanguageDoc;
using ::dss::test_support::summarize;

// ★ `shippedLanguageDoc`, `summarize`, `quotedTokens` and the `at()` pointer
// guard used to be file-local copies here, and the local `shippedLanguageDoc`
// took NO argument — it could only ever read `kLanguage`. They now have ONE
// owner, `vocabulary_projection_probe.hpp`, for the reason this whole file
// exists: D-TEST-VOCABULARY-PROJECTION-PROBE-HELPERS-ARE-COPIED-PER-FILE. The
// header records which copies had already drifted and how each merge resolved.

// ── THE TWO AXES, FOUND BY SEARCHING ─────────────────────────────────────
//
// Never by index: an index rots into a different row and the pin then probes
// something else while staying green. Each returns the JSON pointer of every row
// that carries a `compositeKind`, so a row added later is covered without an
// edit here, and a row REMOVED makes the emptiness guard below fire.
[[nodiscard]] std::vector<std::string>
fieldChildrenPointers(nlohmann::json const& doc) {
    std::vector<std::string> out;
    auto const&              sem = doc.at("semantics");
    if (!sem.contains("declarations")) return out;
    auto const& arr = sem.at("declarations");
    for (std::size_t i = 0; i < arr.size(); ++i) {
        if (arr[i].is_object() && arr[i].contains("fieldChildren")) {
            out.push_back("/semantics/declarations/" + std::to_string(i)
                          + "/fieldChildren");
        }
    }
    return out;
}

[[nodiscard]] std::vector<std::string>
tagReferencePointers(nlohmann::json const& doc) {
    std::vector<std::string> out;
    auto const&              sem = doc.at("semantics");
    if (!sem.contains("references")) return out;
    auto const& arr = sem.at("references");
    for (std::size_t i = 0; i < arr.size(); ++i) {
        if (arr[i].is_object() && arr[i].contains("compositeKind")) {
            out.push_back("/semantics/references/" + std::to_string(i));
        }
    }
    return out;
}

// THE PATH-KEYED ASSERTION — the whole reason this file is not three more rows
// in the sibling pin. `path` is the JSON pointer the loader reports, so the arm
// under test answers for ITSELF and a sibling arm that happens to be correct
// cannot satisfy it.
void expectRefusalAtPathNamesExactly(auto const&        diags,
                                     std::string const& path,
                                     std::string_view   label) {
    constexpr auto kNames = allNames(kCompositeKindTable);

    std::optional<std::string> found;
    for (auto const& d : diags) {
        if (d.path == path) { found = d.message; break; }
    }
    if (!found.has_value()) {
        ADD_FAILURE()
            << label << ": no diagnostic was reported AT '" << path
            << "'. Either the loader stopped refusing this, or it moved the "
               "pointer — and a pin that cannot find its arm asserts nothing."
            << summarize(diags);
        return;
    }

    // (B) COMPLETENESS.
    for (std::string_view const n : kNames) {
        bool named = false;
        for (auto const& q : quotedTokens(*found)) {
            if (q == n) { named = true; break; }
        }
        EXPECT_TRUE(named)
            << label << ": the refusal at '" << path << "' does NOT name '" << n
            << "', which `kCompositeKindTable` accepts. This is a schema author "
               "being told BY NAME that a spelling this very loader takes is "
               "not allowed — measured live on this exact arm, which read "
               "\"must be a string 'struct' or 'union'\" while the parse three "
               "lines below took `enum` too. Render the set through "
               "`compositeKindAllowedList()`, never as a literal.\nmessage was:\n"
            << *found;
    }

    // (C) HONESTY — no quoted token that is neither a spelling nor one of the
    // non-vocabulary quotes this sentence legitimately carries (the key it
    // points at, the offending value, the sibling key named by the
    // tag-reference-only rule).
    for (auto const& q : quotedTokens(*found)) {
        bool ok = (q == "compositeKind") || (q == "fieldChildren.compositeKind")
                  || (q == kBadSpelling) || (q == "isTagReference");
        for (std::string_view const n : kNames) {
            if (q == n) { ok = true; break; }
        }
        EXPECT_TRUE(ok)
            << label << ": the refusal at '" << path << "' quotes '" << q
            << "', which is neither a spelling the table accepts nor a declared "
               "non-vocabulary quote. A message WIDER than its check sends an "
               "author to write a value that will then be refused.\nmessage "
               "was:\n"
            << *found;
    }
}

} // namespace

// ── BASELINE ────────────────────────────────────────────────────────────
//
// The positive control for everything below, and the arm that a TABLE mutant
// reds first: delete a row from `kCompositeKindTable` and the shipped document
// stops loading, because it declares all three spellings.
TEST(CompositeKindVocabularyProjection, ShippedLanguageDocumentLoadsCleanly) {
    auto const doc  = shippedLanguageDoc(kLanguage);
    auto const text = doc.dump();
    auto       r    = GrammarSchema::loadFromText(text, kLanguage);
    ASSERT_TRUE(r.has_value())
        << "the shipped document must load clean before any mutant means "
           "anything: "
        << summarize(r.error());
}

// ── (A) CORPUS EXERCISE ─────────────────────────────────────────────────
//
// Every spelling in the table is DECLARED by the shipped corpus, on both axes,
// and every spelling the corpus declares is in the table. Without this, a table
// mutant could delete a row nothing reads.
TEST(CompositeKindVocabularyProjection, ShippedCorpusDeclaresEverySpelling) {
    constexpr auto kNames = allNames(kCompositeKindTable);
    auto const     doc    = shippedLanguageDoc(kLanguage);

    for (auto const& [label, pointers, key] :
         {std::tuple{"fieldChildren", fieldChildrenPointers(doc),
                     std::string{"compositeKind"}},
          std::tuple{"tag reference", tagReferencePointers(doc),
                     std::string{"compositeKind"}}}) {
        SCOPED_TRACE(label);
        ASSERT_FALSE(pointers.empty())
            << label
            << ": the shipped document carries NO row on this axis, so every "
               "pin below it would pass vacuously";
        std::vector<std::string> declared;
        for (auto const& ptr : pointers) {
            auto        copy = doc;
            auto const& node = at(copy, ptr, label);
            if (!node.contains(key)) continue;
            auto const spelling = node.at(key).get<std::string>();
            declared.push_back(spelling);
            EXPECT_TRUE(compositeKindFromName(spelling).has_value())
                << label << ": the SHIPPED document declares '" << spelling
                << "', which `compositeKindFromName` does not resolve — the "
                   "corpus and the vocabulary disagree";
        }
        for (std::string_view const n : kNames) {
            bool used = false;
            for (auto const& d : declared) {
                if (d == n) { used = true; break; }
            }
            EXPECT_TRUE(used)
                << label << ": no shipped row declares '" << n
                << "', so deleting that row from `kCompositeKindTable` would "
                   "red nothing on this axis and the red-on-disable is vacuous";
        }
    }
}

// ── (D) THE PARSE FOLLOWS THE TABLE ─────────────────────────────────────
//
// Every advertised spelling is accepted by the real loader, and the enum it
// lands on is the one the table names. Proven the only way that is not
// circular: write it into a document and load it.
TEST(CompositeKindVocabularyProjection, EveryAdvertisedSpellingLoads) {
    constexpr auto kNames = allNames(kCompositeKindTable);
    for (std::string_view const name : kNames) {
        SCOPED_TRACE(std::string{"compositeKind <- '"} + std::string{name} + "'");
        auto       doc      = shippedLanguageDoc(kLanguage);
        auto const pointers = fieldChildrenPointers(doc);
        ASSERT_FALSE(pointers.empty());
        for (auto const& ptr : pointers) {
            at(doc, ptr, "fieldChildren row")["compositeKind"] = std::string{name};
        }
        auto const text = doc.dump();
        auto       r    = GrammarSchema::loadFromText(text, kLanguage);
        EXPECT_TRUE(r.has_value())
            << "the refusal advertises '" << name
            << "' but the loader REFUSES it — a message wider than its check "
               "sends an author to write a value that is then rejected: "
            << (r.has_value() ? std::string{} : summarize(r.error()));
    }
}

// ── (B)+(C) ON THE `fieldChildren` AXIS, ARM BY ARM ─────────────────────
//
// Three arms, three separate refusals of one key, each asserted at its own
// path. The TYPE-ERROR arm is the one that had drifted; the other two are here
// because "the sibling arms were right" is precisely why nobody noticed.
TEST(CompositeKindVocabularyProjection, FieldChildrenTypeErrorArmNamesEverySpelling) {
    auto       doc      = shippedLanguageDoc(kLanguage);
    auto const pointers = fieldChildrenPointers(doc);
    ASSERT_FALSE(pointers.empty());
    // A NON-STRING — the exact shape whose sentence named 2 of 3.
    at(doc, pointers.front(), "fieldChildren / not-a-string")["compositeKind"] = 5;
    auto const text = doc.dump();
    auto       r    = GrammarSchema::loadFromText(text, kLanguage);
    ASSERT_FALSE(r.has_value())
        << "a non-string compositeKind must FAIL the load";
    expectRefusalAtPathNamesExactly(r.error(),
                                    pointers.front() + "/compositeKind",
                                    "fieldChildren / not-a-string");
}

TEST(CompositeKindVocabularyProjection, FieldChildrenUnknownValueArmNamesEverySpelling) {
    auto       doc      = shippedLanguageDoc(kLanguage);
    auto const pointers = fieldChildrenPointers(doc);
    ASSERT_FALSE(pointers.empty());
    at(doc, pointers.front(),
       "fieldChildren / unknown value")["compositeKind"] = kBadSpelling;
    auto const text = doc.dump();
    auto       r    = GrammarSchema::loadFromText(text, kLanguage);
    ASSERT_FALSE(r.has_value())
        << "an unresolvable compositeKind spelling must FAIL the load";
    expectRefusalAtPathNamesExactly(r.error(),
                                    pointers.front() + "/compositeKind",
                                    "fieldChildren / unknown value");
}

TEST(CompositeKindVocabularyProjection, FieldChildrenMissingKeyArmNamesEverySpelling) {
    auto       doc      = shippedLanguageDoc(kLanguage);
    auto const pointers = fieldChildrenPointers(doc);
    ASSERT_FALSE(pointers.empty());
    at(doc, pointers.front(), "fieldChildren / missing key").erase("compositeKind");
    auto const text = doc.dump();
    auto       r    = GrammarSchema::loadFromText(text, kLanguage);
    ASSERT_FALSE(r.has_value())
        << "a missing compositeKind must FAIL the load — defaulting silently to "
           "Struct is what this key exists to prevent";
    expectRefusalAtPathNamesExactly(r.error(),
                                    pointers.front() + "/compositeKind",
                                    "fieldChildren / missing key");
}

// ── (B)+(C) ON THE TAG-REFERENCE AXIS ───────────────────────────────────
//
// The second chain. Its comment says it "mirrors the fieldChildren.compositeKind
// spelling so the two composite-kind axes read identically" — which, while both
// were hand-written chains, was a statement of intent that nothing checked.
TEST(CompositeKindVocabularyProjection, TagReferenceTypeErrorArmNamesEverySpelling) {
    auto       doc      = shippedLanguageDoc(kLanguage);
    auto const pointers = tagReferencePointers(doc);
    ASSERT_FALSE(pointers.empty());
    at(doc, pointers.front(),
       "tag reference / not-a-string")["compositeKind"] = 5;
    auto const text = doc.dump();
    auto       r    = GrammarSchema::loadFromText(text, kLanguage);
    ASSERT_FALSE(r.has_value())
        << "a non-string tag-reference compositeKind must FAIL the load";
    expectRefusalAtPathNamesExactly(r.error(),
                                    pointers.front() + "/compositeKind",
                                    "tag reference / not-a-string");
}

TEST(CompositeKindVocabularyProjection, TagReferenceUnknownValueArmNamesEverySpelling) {
    auto       doc      = shippedLanguageDoc(kLanguage);
    auto const pointers = tagReferencePointers(doc);
    ASSERT_FALSE(pointers.empty());
    at(doc, pointers.front(),
       "tag reference / unknown value")["compositeKind"] = kBadSpelling;
    auto const text = doc.dump();
    auto       r    = GrammarSchema::loadFromText(text, kLanguage);
    ASSERT_FALSE(r.has_value())
        << "an unresolvable tag-reference compositeKind must FAIL the load";
    expectRefusalAtPathNamesExactly(r.error(),
                                    pointers.front() + "/compositeKind",
                                    "tag reference / unknown value");
}

// ── THE TABLE ITSELF ────────────────────────────────────────────────────
//
// `DSS_CHECK_ENUM_NAME_TABLE` already refuses an under-filled or duplicated
// table at COMPILE time. This is the half a static_assert cannot state: the
// table covers every enumerator, so no `CompositeKind` value can render as
// another one's name. `name()` falls back to row 0, so a missing row is a
// SILENT mis-spelling rather than an empty string.
TEST(CompositeKindVocabularyProjection, EveryEnumeratorHasItsOwnSpelling) {
    constexpr CompositeKind kAll[] = {CompositeKind::Struct, CompositeKind::Union,
                                      CompositeKind::Enum};
    for (CompositeKind const k : kAll) {
        auto const spelled = compositeKindName(k);
        auto const back    = compositeKindFromName(spelled);
        ASSERT_TRUE(back.has_value())
            << "compositeKindName() produced '" << spelled
            << "', which compositeKindFromName() does not resolve";
        EXPECT_EQ(*back, k)
            << "'" << spelled
            << "' round-trips to a DIFFERENT enumerator — a value with no row "
               "renders as row 0's name, which reads as a legitimate "
               "declaration rather than as a miss";
    }
}
