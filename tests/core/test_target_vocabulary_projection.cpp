// ── D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET ──────────
//    The `.target.json` loader's half, plus the shared predefined-macro
//    parser every one of the three config families inherits.
// ─────────────────────────────────────────────────────────────────────────
//
// THE CLASS. Acceptance is decided by a TABLE — an `EnumNameTable`, or a
// `std::array<string_view, N>` closed-key vocabulary — and the diagnostic
// beside the check states the accepted set as a STRING LITERAL. Two owners of
// one fact. The rows keep working while the SENTENCE becomes a lie, and the
// sentence is the half a config author reads: they are told BY NAME that a
// spelling the loader accepts is not allowed.
//
// ★★★ THIS FILE EXISTS BECAUSE FOUR OF ITS SUBJECTS HAD ALREADY DRIFTED.
// ✔MEASURED 2026-08-20 by comparing each message against the check standing one
// line below it:
//   * `aggregateLayout/bitFieldStrategy` — the refusal named `"gnu_packed"`,
//     ONE of the THREE spellings `bitFieldStrategyFromName` accepts. A target
//     author declaring the Microsoft straddling rule was told by name that the
//     spelling the loader takes is not allowed.
//   * `opcodes[]/implicitRegisters` — the shape sentence named THREE of the
//     FIVE keys `kImplicitRegisterKeys` accepts; `inputRoles` and `outputRoles`
//     were read by the parse arm and advertised by nothing.
//   * `aggregateLayout` itself — the shape sentence named TWO of its THREE
//     keys, `bitFieldStrategy` silently absent.
//   * `predefinedMacros[]/availableObjectFormats` — the refusal named
//     `"pe"/"elf"/"macho"`, THREE of the FIVE spellings
//     `kSelectableObjectFormatKindNames` contains.
// And two more were the DEGENERATE case — a refusal naming NO accepted set at
// all (`aggregateClassification`, both arms), which leaves an author knowing
// only that what they wrote is wrong.
//
// ★★ WHAT EACH PIN ASSERTS, and why it takes three per site. The claim is "the
// message and the check name the SAME set", which no single assertion carries:
//   (A) CORPUS EXERCISE — the shipped documents really declare these
//       vocabularies. Without it the table mutant deletes a row nothing reads
//       and the whole red-on-disable is theatre.
//   (B) COMPLETENESS — every name in the table appears in the refusal message.
//       This is the direction all four live defects were on, and the only thing
//       that catches them.
//   (C) HONESTY — every vocabulary token the message quotes is one the loader
//       actually ACCEPTS. Catches the mirror defect: a message WIDER than its
//       check, which sends an author to write a value that is then refused.
//
// ★★ AND THE EXPECTATION IS DRIVEN FROM THE TABLE, NEVER FROM TODAY'S SENTENCE.
// A pin spelling out the expected message is the same retyping one level up —
// it would go green over a message that had stopped matching the check, because
// both halves of the comparison would be literals nobody re-derives. Here the
// expectation IS `allNames(kSomeTable)`, so adding a row to the table without
// touching the message reds this file.
//
// ⚠ MUST run through ctest, never a bare `.exe`: `findShippedConfig` walks the
// cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it. A bare
// binary reads whichever config tree the shell happens to stand in.

#include "core/types/aggregate_layout.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/enum_name_table.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/preprocess_config.hpp"
#include "core/types/target_schema.hpp"
#include "mutate_target_schema.hpp"
#include "vocabulary_projection_probe.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ::dss::allNames;
using ::dss::TargetSchema;
using ::dss::test_support::findVocabularyMessage;
using ::dss::test_support::mutateShippedTargetSchemaDoc;
using ::dss::test_support::namesContain;
using ::dss::test_support::quotedTokens;
using ::dss::test_support::summarize;

// The two shipped CPU targets. Every pin below runs against BOTH — a message
// reached only through one target's declarations is a message the other
// target's authors never see, and this project has already had a mutation aimed
// at a container arm64 does not declare pass for the wrong reason.
constexpr char const* kTargets[] = {"x86_64", "arm64"};

// A spelling no vocabulary in this tree claims. Deliberately ugly: a probe that
// collided with a real name would make the refusal arms pass for the wrong
// reason.
constexpr char const* kBadSpelling = "zzNotAnyVocabularySpelling";

// ★ `summarize`, `quotedTokens`, `namesContain` and `findVocabularyMessage`
// used to be file-local copies here. They now have ONE owner —
// `vocabulary_projection_probe.hpp` — for the reason this whole file exists:
// D-TEST-VOCABULARY-PROJECTION-PROBE-HELPERS-ARE-COPIED-PER-FILE. The header
// records which of the copies had already drifted apart, and in which
// direction each merge resolved.

// The two directions together: the pair of assertions that make "the message
// and the check name the SAME set" a thing a test can fail.
//
// `ignoreQuoted` carries the non-vocabulary quotes the sentence legitimately
// contains — the offending spelling itself, and the JSON field it points at.
// It is a per-site list rather than a blanket "ignore anything unknown",
// because "ignore anything unknown" is precisely what would let a message
// advertise a spelling the check refuses.
void expectRefusalNamesExactly(auto const&                       diags,
                               std::span<std::string_view const> names,
                               std::span<char const* const>      ignoreQuoted,
                               std::string_view                  label) {
    std::string const* msg = findVocabularyMessage(diags, names);
    if (msg == nullptr) {
        ADD_FAILURE()
            << label
            << ": NO diagnostic in the refused load names every spelling the "
               "vocabulary table accepts. A message NARROWER than its check "
               "tells a config author, by name, that a spelling the loader "
               "takes is not allowed — measured live at four sites in this "
               "loader before these pins existed (bitFieldStrategy 1-of-3, "
               "implicitRegisters 3-of-5, aggregateLayout keys 2-of-3, "
               "availableObjectFormats 3-of-5). Render the list through "
               "`dss::detail::renderAllowedList` over the table's projection, "
               "never as a literal.\ndiagnostics were:"
            << summarize(diags);
        return;
    }
    for (auto const& q : quotedTokens(*msg)) {
        bool ok = namesContain(names, q);
        for (char const* const g : ignoreQuoted) {
            if (q == g) { ok = true; break; }
        }
        EXPECT_TRUE(ok)
            << label << ": the refusal quotes '" << q
            << "', which is neither a spelling the vocabulary accepts nor a "
               "declared non-vocabulary quote for this site. A message WIDER "
               "than its check sends an author to write a value that will then "
               "be refused.\nmessage was:\n"
            << *msg;
    }
}

} // namespace

// ── BASELINE ────────────────────────────────────────────────────────────
//
// The positive control for everything below, and the arm every TABLE mutant
// reds: rename a row of `kBitFieldStrategyTable` and this fails, because both
// shipped targets declare `aggregateLayout.bitFieldStrategy` explicitly.
TEST(TargetVocabularyProjection, ShippedTargetsLoadCleanly) {
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        auto r = TargetSchema::loadShipped(t);
        ASSERT_TRUE(r.has_value())
            << t << " must load clean before any mutant means anything: "
            << summarize(r.error());
    }
}

// ── (A) CORPUS EXERCISE ─────────────────────────────────────────────────
//
// Every vocabulary these pins mutate is really DECLARED by the shipped
// documents, and the spelling declared is one the projection contains. Without
// this arm a table-row rename would break nothing and every red-on-disable
// below would be theatre.
TEST(TargetVocabularyProjection, ShippedTargetsDeclareTheProbedVocabularies) {
    static constexpr auto kBitFieldNames = allNames(dss::kBitFieldStrategyTable);
    static constexpr auto kScalarNames   =
        allNames(dss::kScalarAlignmentRuleTable);
    static constexpr auto kTlsNames      = allNames(dss::kTlsVariantTable);
    static constexpr auto kVaListNames   = allNames(dss::kVaListStrategyTable);
    static constexpr auto kAggClassNames =
        allNames(dss::kAggregateClassKindTable);

    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        auto r = TargetSchema::loadShipped(t);
        ASSERT_TRUE(r.has_value()) << summarize(r.error());
        auto const& ts = **r;

        ASSERT_TRUE(ts.aggregateLayoutLoaded())
            << t << " declares no `aggregateLayout`, so the bitFieldStrategy / "
                    "scalarAlignment pins below would assert nothing";
        EXPECT_TRUE(namesContain(kBitFieldNames,
                             dss::bitFieldStrategyName(
                                 ts.aggregateLayout().bitFieldStrategy)))
            << t << ": the shipped `bitFieldStrategy` resolves to a value the "
                    "projection does not contain — the corpus and the "
                    "vocabulary disagree";
        EXPECT_TRUE(namesContain(kScalarNames,
                             dss::scalarAlignmentRuleName(
                                 ts.aggregateLayout().scalarAlignment)))
            << t << ": the shipped `scalarAlignment` resolves outside the "
                    "projection";
        ASSERT_TRUE(ts.tlsIdentity().has_value())
            << t << " declares no `tls` block, so the tls.variant pins below "
                    "would assert nothing";
        EXPECT_TRUE(namesContain(kTlsNames,
                             dss::tlsVariantName(ts.tlsIdentity()->variant)))
            << t << ": the shipped `tls.variant` resolves outside the "
                    "projection";

        // The two per-CC vocabularies. At least one calling convention must
        // exercise each, or the message pins below guard a path the shipped
        // corpus never takes.
        std::size_t vaListSeen = 0;
        std::size_t aggClassSeen = 0;
        for (auto const& cc : ts.callingConventions()) {
            if (cc.vaListLayout.has_value()) {
                EXPECT_TRUE(namesContain(
                    kVaListNames,
                    dss::vaListStrategyName(cc.vaListLayout->strategy)))
                    << t << "/" << cc.name
                    << ": the shipped `vaListLayout.strategy` resolves outside "
                       "the projection";
                ++vaListSeen;
            }
            if (namesContain(kAggClassNames,
                         dss::aggregateClassKindName(
                             cc.aggregateClassification))) {
                ++aggClassSeen;
            }
        }
        EXPECT_GT(vaListSeen, 0u)
            << t << " declares no `vaListLayout.strategy` anywhere, so its "
                    "refusal message guards nothing the corpus reaches";
        EXPECT_GT(aggClassSeen, 0u)
            << t << " declares no resolvable `aggregateClassification`, so its "
                    "refusal message guards nothing the corpus reaches";
    }
}

// ── (B)+(C) THE VALUE VOCABULARIES ──────────────────────────────────────

TEST(TargetVocabularyProjection, BitFieldStrategyRefusalNamesTheWholeTable) {
    // ★ THE WHOLE TABLE, sentinel INCLUDED, and that is the point of stating
    // this pin against the target rather than the format. A `.target.json` may
    // write `none`; a `.format.json` may not, because the format's value falls
    // back to this one. The two checks differ, so the two messages must — and a
    // pin that asserted the SELECTABLE projection here would be asserting the
    // other site's contract at this one.
    static constexpr auto kNames = allNames(dss::kBitFieldStrategyTable);
    constexpr char const* kIgnore[] = {kBadSpelling, "bitFieldStrategy"};
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        auto r = mutateShippedTargetSchemaDoc(t, [](nlohmann::json& doc) {
            doc["aggregateLayout"]["bitFieldStrategy"] = kBadSpelling;
        });
        ASSERT_FALSE(r.has_value())
            << "an unknown bitFieldStrategy must be REFUSED — a silent "
               "fallback bakes a wrong bit placement into every bit-field";
        expectRefusalNamesExactly(r.error(), kNames, kIgnore,
                                  "aggregateLayout/bitFieldStrategy");
    }
}

TEST(TargetVocabularyProjection, ScalarAlignmentRefusalNamesTheWholeTable) {
    static constexpr auto kNames = allNames(dss::kScalarAlignmentRuleTable);
    constexpr char const* kIgnore[] = {kBadSpelling, "scalarAlignment",
                                       "maxAlignment", "bitFieldStrategy"};
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        auto r = mutateShippedTargetSchemaDoc(t, [](nlohmann::json& doc) {
            doc["aggregateLayout"]["scalarAlignment"] = kBadSpelling;
        });
        ASSERT_FALSE(r.has_value())
            << "an unknown scalarAlignment must be REFUSED";
        expectRefusalNamesExactly(r.error(), kNames, kIgnore,
                                  "aggregateLayout/scalarAlignment");
    }
}

TEST(TargetVocabularyProjection, TlsVariantRefusalNamesTheWholeTable) {
    static constexpr auto kNames = allNames(dss::kTlsVariantTable);
    constexpr char const* kIgnore[] = {kBadSpelling};
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        auto r = mutateShippedTargetSchemaDoc(t, [](nlohmann::json& doc) {
            doc["tls"]["variant"] = kBadSpelling;
        });
        ASSERT_FALSE(r.has_value())
            << "an unknown tls variant must be REFUSED — the two variants "
               "produce opposite-signed tpoffs, so no default is safe";
        expectRefusalNamesExactly(r.error(), kNames, kIgnore, "tls/variant");
    }
}

TEST(TargetVocabularyProjection, VaListStrategyRefusalNamesTheWholeTable) {
    static constexpr auto kNames = allNames(dss::kVaListStrategyTable);
    constexpr char const* kIgnore[] = {kBadSpelling};
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        bool injected = false;
        auto r = mutateShippedTargetSchemaDoc(
            t, [&](nlohmann::json& doc) {
                for (auto& cc : doc.at("callingConventions")) {
                    if (!cc.contains("vaListLayout")) continue;
                    cc["vaListLayout"]["strategy"] = kBadSpelling;
                    injected = true;
                }
            });
        ASSERT_TRUE(injected)
            << t << " declares no `vaListLayout` to aim the mutation at — the "
                    "pin would assert nothing";
        ASSERT_FALSE(r.has_value())
            << "an unknown va_list strategy must be REFUSED";
        expectRefusalNamesExactly(r.error(), kNames, kIgnore,
                                  "vaListLayout/strategy");
    }
}

TEST(TargetVocabularyProjection,
     AggregateClassificationRefusalNamesTheWholeTable) {
    // ⚠ THIS REFUSAL USED TO NAME NO SET AT ALL — the degenerate case of the
    // class. An author was told the spelling was wrong and given nothing to
    // write instead, which is a diagnostic that fails loud and helps nobody.
    static constexpr auto kNames = allNames(dss::kAggregateClassKindTable);
    constexpr char const* kIgnore[] = {kBadSpelling, "aggregateClassification"};
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        bool injected = false;
        auto r = mutateShippedTargetSchemaDoc(
            t, [&](nlohmann::json& doc) {
                for (auto& cc : doc.at("callingConventions")) {
                    cc["aggregateClassification"] = kBadSpelling;
                    injected = true;
                }
            });
        ASSERT_TRUE(injected)
            << t << " declares no calling convention to aim the mutation at";
        ASSERT_FALSE(r.has_value())
            << "an unknown aggregate-classification strategy must be REFUSED — "
               "a silent fallback mis-passes every by-value struct";
        expectRefusalNamesExactly(r.error(), kNames, kIgnore,
                                  "callingConventions/aggregateClassification");
    }
}

// ── (B)+(C) THE TWO SETS NO QUOTED CENSUS COULD SEE ─────────────────────
//
// ⚠ BOTH OF THESE RENDERED THEIR CLOSED SET **UNQUOTED** — `(expected one of
// eq/ne/slt/…)` and `expected one of add/sub/mul/div/to_i32/from_f64`. A
// slash-joined bare run is invisible to every quoted-token instrument this
// project has, and to every test that reads a message back, so the sites
// survived two passes of this class before a `--bare` arm was added to the
// census. They are pinned here in the QUOTED form the shared renderer emits,
// which is what makes them readable at all.

TEST(TargetVocabularyProjection, CondCodeRefusalNamesTheWholeTable) {
    // ★ The cond-code loader kept its OWN 17-spelling copy of this vocabulary,
    // in the same order, because `condCodeEncoding[idx]` indexes by ENUM
    // ORDINAL — so the two copies had to agree on order as well as on content,
    // and nothing checked either. The copy is gone; the order is now a
    // `static_assert` over enumerators.
    static constexpr auto kNames = allNames(dss::kTargetCondCodeTable);
    constexpr char const* kIgnore[] = {kBadSpelling};
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        bool injected = false;
        auto r = mutateShippedTargetSchemaDoc(
            t, [&](nlohmann::json& doc) {
                if (!doc.contains("condCodeEncoding")) return;
                doc["condCodeEncoding"][kBadSpelling] = 0;
                injected = true;
            });
        ASSERT_TRUE(injected)
            << t << " declares no `condCodeEncoding` block to aim the mutation "
                    "at — the pin would assert nothing";
        ASSERT_FALSE(r.has_value())
            << "an unknown cond-code key must be REFUSED — a typo would leave "
               "the encoder defaulting that condition to 0";
        expectRefusalNamesExactly(r.error(), kNames, kIgnore,
                                  "condCodeEncoding key");
    }
}

TEST(TargetVocabularyProjection, WideFloatSoftcallOpRefusalNamesTheWholeTable) {
    static constexpr auto kNames = allNames(dss::kWideFloatOpTable);
    constexpr std::span<char const* const> kIgnore{};
    bool anyTargetHadOne = false;
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        bool injected = false;
        auto r = mutateShippedTargetSchemaDoc(
            t, [&](nlohmann::json& doc) {
                if (doc.contains("wideFloatSoftcalls")
                    && !doc.at("wideFloatSoftcalls").empty()) {
                    doc["wideFloatSoftcalls"][0]["op"] = kBadSpelling;
                    injected = true;
                    return;
                }
                // Keep the mutation non-empty so the helper's byte-identity
                // contract holds on a target with no such block; the load then
                // still succeeds and this target is skipped.
                doc["$wideFloatSoftcallsProbeComment"] =
                    "no wideFloatSoftcalls block in this target";
            });
        if (!injected) continue;
        anyTargetHadOne = true;
        ASSERT_FALSE(r.has_value())
            << "an unknown wideFloatSoftcalls op must be REFUSED";
        expectRefusalNamesExactly(r.error(), kNames, kIgnore,
                                  "wideFloatSoftcalls/op");
    }
    EXPECT_TRUE(anyTargetHadOne)
        << "NO shipped target declares `wideFloatSoftcalls`, so this pin "
           "exercised nothing at all";
}

// ── (B)+(C) THE SIBLING CLASS ON THE KEY HALF ───────────────────────────
//
// A SHAPE sentence — `must be an object { … }` — advertises a KEY set exactly
// as a value refusal advertises a spelling set, and drifts the same way. It is
// reached by making the block the WRONG TYPE, which is the one arm that renders
// the shape rather than the block's per-field checks.

TEST(TargetVocabularyProjection, AggregateLayoutShapeNamesEveryKey) {
    // ⚠ ✔MEASURED 2026-08-20: this sentence named `{ scalarAlignment,
    // maxAlignment }` — two of the three keys the block's own
    // `rejectUnknownKeys` accepts.
    static constexpr std::string_view kKeys[] = {
        "scalarAlignment", "maxAlignment", "bitFieldStrategy"};
    // ★ NO declared non-vocabulary quotes at this site, and that is the
    // STRONGEST form of the honesty arm: every `'…'` in the sentence must be a
    // key of the block. An `ignore` entry is a hole, so it is spelled only
    // where the sentence genuinely needs one.
    constexpr std::span<char const* const> kIgnore{};
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        auto r = mutateShippedTargetSchemaDoc(t, [](nlohmann::json& doc) {
            doc["aggregateLayout"] = "not-an-object";
        });
        ASSERT_FALSE(r.has_value())
            << "a non-object aggregateLayout must be REFUSED";
        expectRefusalNamesExactly(r.error(), kKeys, kIgnore,
                                  "aggregateLayout shape");
    }
}

TEST(TargetVocabularyProjection, TlsBlockShapeNamesEveryKey) {
    static constexpr std::string_view kKeys[] = {"variant", "tcbHeaderBytes"};
    // The shape sentence names the BLOCK it describes, and renders the VARIANT
    // vocabulary inside it — both legitimate non-key quotes at this site.
    constexpr char const* kIgnore[] = {"tls", "variant1", "variant2"};
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        auto r = mutateShippedTargetSchemaDoc(t, [](nlohmann::json& doc) {
            doc["tls"] = "not-an-object";
        });
        ASSERT_FALSE(r.has_value()) << "a non-object tls block must be REFUSED";
        expectRefusalNamesExactly(r.error(), kKeys, kIgnore, "tls shape");
    }
}

TEST(TargetVocabularyProjection, ImplicitRegistersShapeNamesEveryKey) {
    // ⚠ ✔MEASURED 2026-08-20: this sentence named `inputs`/`outputs`/
    // `clobbered` — three of the FIVE keys the block accepts. `inputRoles` and
    // `outputRoles` were read by the parse arm and advertised by nothing.
    static constexpr std::string_view kKeys[] = {
        "inputs", "outputs", "clobbered", "inputRoles", "outputRoles"};
    constexpr char const* kIgnore[] = {"implicitRegisters"};
    bool anyTargetHadOne = false;
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        bool injected = false;
        // Not every target declares an `implicitRegisters` block — arm64
        // declares none — so the mutation is aimed only where one exists, and
        // the ACROSS-TARGETS witness below is what refuses a vacuous run.
        auto r = mutateShippedTargetSchemaDoc(
            t, [&](nlohmann::json& doc) {
                if (!doc.contains("opcodes")) return;
                for (auto& op : doc.at("opcodes")) {
                    if (!op.contains("implicitRegisters")) continue;
                    op["implicitRegisters"] = "not-an-object";
                    injected = true;
                }
                if (!injected) {
                    // Keep the mutation non-empty so the helper's byte-identity
                    // contract is satisfied even on a target with no block; the
                    // load then still succeeds and this target is skipped.
                    doc["$implicitRegistersProbeComment"] =
                        "no implicitRegisters block in this target";
                }
            });
        if (!injected) continue;
        anyTargetHadOne = true;
        ASSERT_FALSE(r.has_value())
            << "a non-object implicitRegisters must be REFUSED";
        expectRefusalNamesExactly(r.error(), kKeys, kIgnore,
                                  "implicitRegisters shape");
    }
    EXPECT_TRUE(anyTargetHadOne)
        << "NO shipped target declares an `implicitRegisters` block, so this "
           "pin exercised nothing at all — the vacuous-run trap this project "
           "already walked into once with an arm64-aimed mutation";
}

// ── (B)+(C) THE SHARED PREDEFINED-MACRO PARSER ──────────────────────────
//
// One parser, inherited by all three config families (language, target,
// format), so its refusals are pinned once — here, through the target family,
// which is the one this file already has a mutation vehicle for.

TEST(TargetVocabularyProjection,
     AvailableObjectFormatsRefusalNamesEverySelectableKind) {
    // ⚠ ✔MEASURED 2026-08-20: this refusal named `"pe"/"elf"/"macho"` — THREE
    // of the FIVE spellings the check accepts. `wasm` and `spirv` are declared
    // enumerators with shipped skeleton formats.
    // ★ THE SELECTABLE PROJECTION, NOT `allNames`: the `unknown` sentinel
    // spells correctly and passes the name lookup, and the very next check
    // refuses it. Advertising it would send an author to write the one value
    // that passes one check and fails the next.
    constexpr char const* kIgnore[] = {kBadSpelling};
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        bool injected = false;
        auto r = mutateShippedTargetSchemaDoc(
            t, [&](nlohmann::json& doc) {
                if (!doc.contains("predefinedMacros")) return;
                for (auto& pm : doc.at("predefinedMacros")) {
                    pm["availableObjectFormats"] =
                        nlohmann::json::array({kBadSpelling});
                    injected = true;
                    break;
                }
            });
        ASSERT_TRUE(injected)
            << t << " declares no `predefinedMacros` entry to aim the mutation "
                    "at — the pin would assert nothing";
        ASSERT_FALSE(r.has_value())
            << "an unknown object-format name must be REFUSED — a typo would "
               "gate the macro to nothing, silently";
        expectRefusalNamesExactly(r.error(),
                                  dss::kSelectableObjectFormatKindNames,
                                  kIgnore, "availableObjectFormats");
    }
}

TEST(TargetVocabularyProjection,
     AvailableObjectFormatsRefusesTheSentinelSeparately) {
    // The mirror direction of the pin above, and the reason it must project the
    // SELECTABLE set: the sentinel resolves through the name lookup and is then
    // refused by its own arm. If the advertised list ever grew to include it,
    // this pair would disagree — one message inviting the value the other
    // rejects.
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        bool injected = false;
        auto r = mutateShippedTargetSchemaDoc(
            t, [&](nlohmann::json& doc) {
                if (!doc.contains("predefinedMacros")) return;
                for (auto& pm : doc.at("predefinedMacros")) {
                    pm["availableObjectFormats"] = nlohmann::json::array(
                        {std::string{dss::kObjectFormatKindSentinelName}});
                    injected = true;
                    break;
                }
            });
        ASSERT_TRUE(injected);
        ASSERT_FALSE(r.has_value())
            << "the reserved sentinel spelling must be REFUSED even though the "
               "name lookup resolves it";
        EXPECT_FALSE(namesContain(dss::kSelectableObjectFormatKindNames,
                              dss::kObjectFormatKindSentinelName))
            << "the SELECTABLE projection must not contain the sentinel — the "
               "message would then advertise the one spelling this arm rejects";
    }
}

TEST(TargetVocabularyProjection, PredefinedMacroKindRefusalNamesTheWholeTable) {
    // ⚠ The acceptance here is the enum table PLUS one config-only spelling:
    // `version` LOWERS to `Constant` at load, so it has no enum row. The pin
    // therefore has two halves — the table projection must be complete, AND the
    // extra spelling must still be named — because a message that dropped
    // either would be narrower than the check in a different direction.
    // ⚠ This set was UNQUOTED (`expected line/file/constant/date/time/version`)
    // and the acceptance was a hand-written six-way `kind ==` chain duplicating
    // the table; neither was visible to a quoted-token census.
    static constexpr auto kNames = allNames(dss::kPredefinedMacroKindTable);
    constexpr char const* kIgnore[] = {kBadSpelling, "version"};
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        bool injected = false;
        auto r = mutateShippedTargetSchemaDoc(
            t, [&](nlohmann::json& doc) {
                if (!doc.contains("predefinedMacros")) return;
                for (auto& pm : doc.at("predefinedMacros")) {
                    pm["kind"] = kBadSpelling;
                    injected = true;
                    break;
                }
            });
        ASSERT_TRUE(injected);
        ASSERT_FALSE(r.has_value())
            << "an unknown predefinedMacros kind must be REFUSED";
        expectRefusalNamesExactly(r.error(), kNames, kIgnore,
                                  "predefinedMacros/kind");
        bool namesVersion = false;
        for (auto const& d : r.error()) {
            auto const q = quotedTokens(d.message);
            if (std::find(q.begin(), q.end(), std::string{"version"})
                != q.end()) {
                namesVersion = true;
            }
        }
        EXPECT_TRUE(namesVersion)
            << "the refusal must also name the config-only 'version' spelling, "
               "which the loader accepts and the enum table does not carry — "
               "dropping it makes the message narrower than its own check";
    }
}

TEST(TargetVocabularyProjection, ImpliedSurfaceKindRefusalNamesTheWholeTable) {
    static constexpr auto kNames = allNames(dss::kImpliedSurfaceKindTable);
    constexpr char const* kIgnore[] = {kBadSpelling, "impliedSurface.kind",
                                       "kind"};
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        bool injected = false;
        auto r = mutateShippedTargetSchemaDoc(
            t, [&](nlohmann::json& doc) {
                if (!doc.contains("predefinedMacros")) return;
                for (auto& pm : doc.at("predefinedMacros")) {
                    pm["impliedSurface"] = {{"kind", kBadSpelling}};
                    injected = true;
                    break;
                }
            });
        ASSERT_TRUE(injected);
        ASSERT_FALSE(r.has_value())
            << "an unknown impliedSurface.kind must be REFUSED";
        expectRefusalNamesExactly(r.error(), kNames, kIgnore,
                                  "impliedSurface/kind");
    }
}

TEST(TargetVocabularyProjection,
     ClaimsNothingReasonRefusalNamesTheWholeTable) {
    static constexpr auto kNames = allNames(dss::kClaimsNothingReasonTable);
    constexpr char const* kIgnore[] = {kBadSpelling, "reason"};
    for (char const* t : kTargets) {
        SCOPED_TRACE(t);
        bool injected = false;
        auto r = mutateShippedTargetSchemaDoc(
            t, [&](nlohmann::json& doc) {
                if (!doc.contains("predefinedMacros")) return;
                for (auto& pm : doc.at("predefinedMacros")) {
                    pm["impliedSurface"] = {
                        {"kind", std::string{dss::impliedSurfaceKindName(
                                     dss::ImpliedSurfaceKind::ClaimsNothing)}},
                        {"reason", kBadSpelling}};
                    injected = true;
                    break;
                }
            });
        ASSERT_TRUE(injected);
        ASSERT_FALSE(r.has_value())
            << "an unknown claims-nothing reason must be REFUSED — a free-text "
               "reason cannot be audited across dozens of rows";
        expectRefusalNamesExactly(r.error(), kNames, kIgnore,
                                  "impliedSurface/reason");
    }
}
