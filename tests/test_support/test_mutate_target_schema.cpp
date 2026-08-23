// D-TEST-SCHEMA-MUTATION-HELPER-FAILS-OPEN — the self-test for the mutation
// instrument itself.
//
// WHY THIS FILE EXISTS. `mutate_target_schema.hpp` is this project's primary
// defence against vacuous tests: ~54 call sites build a MUTANT schema and pin
// the substrate's behaviour against it. Until 2026-08-14 the helper could
// mutate NOTHING and report success by three routes (an unmatched mnemonic, a
// countless `erase(remove_if(...), end())`, and an undocumented bare `return;`
// when the document had no `opcodes` array). An instrument that can silently
// no-op does not weaken one test — it silently certifies every pin downstream
// of it, and `tests/core/test_target_schema.cpp` already records one pin that
// "asserted nothing at all" for exactly this reason.
//
// So the instrument gets the same posture it enforces: every failure arm is
// EXERCISED here, never read, and every assertion is on message CONTENT, not
// merely on the fact that something failed. A pin that only checks "it threw"
// would stay green if the helper threw the WRONG error — which is the same
// green-for-the-wrong-reason bug one layer up.
//
// ⚠ MUST RUN THROUGH ctest, NEVER AS A BARE .exe. Every case here resolves a
// SHIPPED target through `findShippedConfig`, which prefers `$DSS_CONFIG_ROOT`
// and otherwise walks the cwd. `dss_add_test` sets that variable as a ctest
// ENVIRONMENT property, and a ctest property reaches only a process ctest
// itself launched (tests/CMakeLists.txt ~:61). Run directly from a shell in
// the wrong cwd, these cases fail on config discovery rather than on the
// contract they pin.

#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

namespace {

using dss::test_support::TargetSchemaMutationError;
using dss::test_support::mutateShippedTargetSchemaDoc;
using dss::test_support::mutateShippedTargetSchemaJson;

// Run `fn` and return the contract-violation message it threw, or "" if it did
// not throw one.
//
// ★ The `std::exception` arm is not defensive padding — it is the point. If
// this helper caught the base type and returned its message, a case here could
// pass on an unrelated nlohmann or filesystem failure while the contract check
// it aims at was never reached. Tagging the wrong type poisons the string so
// the content assertions below fail loudly instead.
template <typename Fn>
[[nodiscard]] std::string contractErrorFrom(Fn&& fn) {
    try {
        fn();
    } catch (TargetSchemaMutationError const& e) {
        return e.what();
    } catch (std::exception const& e) {
        return std::string{"<WRONG EXCEPTION TYPE, NOT TargetSchemaMutation"
                           "Error> "} + e.what();
    }
    return {};
}

void expectMentions(std::string_view haystack, std::string_view needle,
                    std::string_view why) {
    EXPECT_NE(haystack.find(needle), std::string_view::npos)
        << "the contract message must name " << why << " (\"" << needle
        << "\"), otherwise the reader cannot act on it.\nmessage was:\n"
        << haystack;
}

// Mnemonics MEASURED in the shipped targets (2026-08-14): `lea` and `bswap`
// are declared by x86_64; `msub` by arm64 only; `popcount` by x86_64 only.
// The two one-sided ones are the load-bearing fixtures below — they let one
// spelling be simultaneously valid and invalid depending only on the document,
// which is what proves the check reads the DOCUMENT and not a static list.
constexpr char kX86[] = "x86_64";
constexpr char kArm[] = "arm64";

// ── The SUCCESS arm: a mutation that matches must still work ──────────────

TEST(MutateTargetSchema, MatchingRemovalSucceedsAndRemovesOnlyTheNamedRow) {
    // Baseline: the witness is PRESENT in the unmutated shipped schema. Without
    // this, "absent from the mutant" would be satisfied by a target that never
    // had it — the vacuous success this whole header is about.
    auto shipped = dss::TargetSchema::loadShipped(kX86);
    ASSERT_TRUE(shipped.has_value()) << "shipped x86_64 must load";
    ASSERT_TRUE((*shipped)->opcodeByMnemonic("lea").has_value())
        << "fixture drift: x86_64 no longer declares `lea`";
    ASSERT_TRUE((*shipped)->opcodeByMnemonic("bswap").has_value())
        << "fixture drift: x86_64 no longer declares `bswap`";

    auto mutated = mutateShippedTargetSchemaJson(kX86, {"lea"});

    // Fail-closed check: THE MUTANT STILL PARSES. A mutation that produced an
    // unloadable schema would red every consumer for a reason that has nothing
    // to do with the property under test.
    ASSERT_TRUE(mutated.has_value())
        << "removing `lea` must still yield a loadable schema";

    // Fail-closed check: the witness is ABSENT FROM THE MUTANT BY THE SAME
    // MATCHER THE PIN USES. `opcodeByMnemonic` is the accessor every consumer
    // reaches for — deliberately NOT a grep over the JSON text, which would
    // also hit the spelling inside another row's encoding or inside a
    // `$comment`, and has kept a pin green here before.
    EXPECT_FALSE((*mutated)->opcodeByMnemonic("lea").has_value())
        << "`lea` survived the removal";

    // Fail-closed check: the witness is UNIQUE — the mutation took the row it
    // named and nothing else. A helper that emptied `opcodes` would pass the
    // absence check above while destroying the schema's meaning.
    EXPECT_TRUE((*mutated)->opcodeByMnemonic("bswap").has_value())
        << "the removal was not surgical: `bswap` disappeared too";
}

// ── The failure arms, EXERCISED ───────────────────────────────────────────

TEST(MutateTargetSchema, UnmatchedMnemonicThrowsAndNamesTheMissAndTheDocument) {
    std::string const msg = contractErrorFrom([] {
        auto r = mutateShippedTargetSchemaJson(kX86, {"zzNotARealMnemonic"});
        (void)r;
    });
    ASSERT_FALSE(msg.empty())
        << "a mnemonic that matches NO opcode row must fail the test loudly — "
           "this is the original fail-open: the helper accepted it, returned "
           "an unmutated schema, and the pin consuming it asserted nothing";
    expectMentions(msg, "zzNotARealMnemonic", "what it looked for");
    expectMentions(msg, kX86, "which target it looked in");
    expectMentions(msg, "matched NOTHING", "that the mutation was empty");
    expectMentions(msg, "opcode row", "what the document actually contained");
}

TEST(MutateTargetSchema, TheVerdictFollowsTheDOCUMENTNotTheSpelling) {
    // ★ THE CENTRAL PIN. `msub` is a real, correctly-spelled mnemonic — arm64
    // declares it and x86_64 does not. One spelling, two targets, opposite
    // verdicts: a check keyed on a static allowlist, or one that merely looked
    // plausible, cannot produce this pair. `popcount` runs the same experiment
    // with the targets swapped, so neither direction is a fluke of which
    // target happens to be richer.
    auto armOk = mutateShippedTargetSchemaJson(kArm, {"msub"});
    EXPECT_TRUE(armOk.has_value())
        << "arm64 declares `msub`; removing it must succeed";

    std::string const x86Msg = contractErrorFrom([] {
        auto r = mutateShippedTargetSchemaJson(kX86, {"msub"});
        (void)r;
    });
    ASSERT_FALSE(x86Msg.empty())
        << "x86_64 does not declare `msub` — removing it there mutates "
           "nothing and must throw";
    expectMentions(x86Msg, "msub", "the mnemonic that missed");

    auto x86Ok = mutateShippedTargetSchemaJson(kX86, {"popcount"});
    EXPECT_TRUE(x86Ok.has_value())
        << "x86_64 declares `popcount`; removing it must succeed";

    std::string const armMsg = contractErrorFrom([] {
        auto r = mutateShippedTargetSchemaJson(kArm, {"popcount"});
        (void)r;
    });
    ASSERT_FALSE(armMsg.empty())
        << "arm64 does not declare `popcount` — removing it there mutates "
           "nothing and must throw";
    expectMentions(armMsg, "popcount", "the mnemonic that missed");
}

TEST(MutateTargetSchema, EveryMissIsReportedInOneMessage) {
    // Two misses alongside one hit: the hit must not mask the misses, and the
    // reader must not have to rebuild once per typo.
    std::string const msg = contractErrorFrom([] {
        auto r = mutateShippedTargetSchemaJson(
            kX86, {"lea", "zzFirstTypo", "zzSecondTypo"});
        (void)r;
    });
    ASSERT_FALSE(msg.empty()) << "two unmatched mnemonics must throw";
    expectMentions(msg, "zzFirstTypo", "the first miss");
    expectMentions(msg, "zzSecondTypo", "the second miss");
}

TEST(MutateTargetSchema, ANearMissSpellingIsOfferedBackToTheReader) {
    // A no-match is nearly always a typo, so the message carries the
    // neighbourhood rather than making the reader open the JSON.
    std::string const msg = contractErrorFrom([] {
        auto r = mutateShippedTargetSchemaJson(kX86, {"popcnt"});
        (void)r;
    });
    ASSERT_FALSE(msg.empty()) << "`popcnt` is not the x86_64 spelling";
    expectMentions(msg, "popcount", "the near-miss spelling that DOES exist");
}

TEST(MutateTargetSchema, AnEmptyRemovalListThrows) {
    std::string const msg = contractErrorFrom([] {
        auto r = mutateShippedTargetSchemaJson(kX86, {});
        (void)r;
    });
    ASSERT_FALSE(msg.empty())
        << "removing nothing yields a mutant identical to the shipped schema";
    expectMentions(msg, "EMPTY", "that the removal list was empty");
}

TEST(MutateTargetSchema, ADocumentWithNoOpcodesArrayThrows) {
    // ★ REACHED THROUGH THE `detail::` SEAM ON PURPOSE. Every shipped target
    // has an `opcodes` array, so this arm is unreachable through the public
    // entry point — without the seam it could only be READ, and this repo has
    // been bitten by a failure arm that was read rather than run (a suite
    // printing `failed=0` had been exiting 2 for weeks).
    std::string const msg = contractErrorFrom([] {
        nlohmann::json doc = {{"name", "fake"}, {"registers", nlohmann::json::array()}};
        dss::test_support::detail::eraseOpcodeRows(doc, {"lea"}, "fakeTarget");
    });
    ASSERT_FALSE(msg.empty())
        << "a document with no `opcodes` array means the caller aimed at the "
           "WRONG DOCUMENT — this was the undocumented bare `return;`";
    expectMentions(msg, "no `opcodes` key at all", "what was missing");
    expectMentions(msg, "registers", "what the document DID contain");
    expectMentions(msg, "fakeTarget", "which document it was aimed at");
}

TEST(MutateTargetSchema, AnOpcodesKeyOfTheWrongTypeThrows) {
    // The sibling of the case above: the key exists but is not an array. The
    // old guard (`!contains || !is_array`) collapsed both into one silent
    // `return`, so neither was distinguishable from a successful removal.
    std::string const msg = contractErrorFrom([] {
        nlohmann::json doc = {{"opcodes", 42}};
        dss::test_support::detail::eraseOpcodeRows(doc, {"lea"}, "fakeTarget");
    });
    ASSERT_FALSE(msg.empty()) << "`opcodes` present but not an array must throw";
    expectMentions(msg, "`opcodes` is a", "the type it actually found");
}

// ── The byte-difference contract on the general Doc form ──────────────────

TEST(MutateTargetSchema, ADocMutationThatChangesNothingThrows) {
    std::string const msg = contractErrorFrom([] {
        auto r = mutateShippedTargetSchemaDoc(kArm, [](nlohmann::json&) {});
        (void)r;
    });
    ASSERT_FALSE(msg.empty())
        << "a lambda that mutates nothing produces a \"mutant\" that IS the "
           "shipped schema";
    expectMentions(msg, "NO-OP", "that nothing changed");
    expectMentions(msg, "byte-identical", "how it knows");
    expectMentions(msg, kArm, "which target was untouched");
}

TEST(MutateTargetSchema, ANavigatorThatMissesItsContainerThrows) {
    // ★ THE REGRESSION THIS HELPER WAS BITTEN BY, RE-RUN AS A PIN. The note in
    // tests/core/test_target_schema.cpp records it: arm64 declares ZERO
    // `implicitRegisters` blocks, so a lambda that walked to one on arm64
    // silently did nothing and its pin "asserted nothing at all". The lambda
    // below is that walk. It must now throw instead of quietly succeeding, and
    // no consumer has to hand-roll an `injected` flag to notice.
    std::string const msg = contractErrorFrom([] {
        auto r = mutateShippedTargetSchemaDoc(kArm, [](nlohmann::json& doc) {
            for (auto& op : doc["opcodes"]) {
                if (!op.contains("implicitRegisters")) {
                    continue;
                }
                op["implicitRegisters"]["$zzProseComment"] = "prose";
            }
        });
        (void)r;
    });
    ASSERT_FALSE(msg.empty())
        << "arm64 declares no `implicitRegisters`, so this walk mutates "
           "nothing — the exact null experiment that produced a pin asserting "
           "nothing at all";
    expectMentions(msg, "NO-OP", "that the navigator never reached anything");
}

TEST(MutateTargetSchema, AnAddThenRemoveRoundTripIsStillANoOp) {
    // The byte check compares the CANONICAL serialization, so key insertion
    // order cannot manufacture a false difference: a lambda that adds a key
    // and takes it away again has mutated nothing, and must be reported as
    // nothing. A check that compared, say, an insertion-ordered dump could
    // report "changed" here and hand the caller a vacuous mutant anyway.
    std::string const msg = contractErrorFrom([] {
        auto r = mutateShippedTargetSchemaDoc(kArm, [](nlohmann::json& doc) {
            doc["$zzTemporary"] = "here and gone";
            doc.erase("$zzTemporary");
        });
        (void)r;
    });
    ASSERT_FALSE(msg.empty())
        << "add-then-remove leaves the document byte-identical";
    expectMentions(msg, "NO-OP", "that the round trip changed nothing");
}

TEST(MutateTargetSchema, ASameLengthReplacementCountsAsAChange) {
    // ★★ THE "NEVER A LINE COUNT" PIN. A same-length edit — re-pointing a wire
    // at an equally-named slot, flipping a guard width 32→64 — is precisely
    // the mutation many pins here need, and it is invisible to a line count, a
    // byte-LENGTH check, or an opcode-row count. Only a byte-wise comparison
    // sees it. This case proves the instrument sees it, by making a mutation
    // whose before/after LENGTHS are asserted equal.
    std::size_t beforeLen = 0;
    std::size_t afterLen = 0;
    bool edited = false;

    std::string const msg = contractErrorFrom([&] {
        auto r = mutateShippedTargetSchemaDoc(
            kArm, [&](nlohmann::json& doc) {
                beforeLen = doc.dump().size();
                // Rename one opcode in place, same character count. `msub` and
                // `zsub` are both 4 bytes, so the serialized document changes
                // byte-wise while its LENGTH does not move at all.
                for (auto& op : doc["opcodes"]) {
                    if (op.value("mnemonic", std::string{}) != "msub") {
                        continue;
                    }
                    op["mnemonic"] = "zsub";
                    edited = true;
                    break;
                }
                afterLen = doc.dump().size();
            });
        (void)r;
    });

    ASSERT_TRUE(edited) << "fixture drift: arm64 no longer declares `msub`";
    EXPECT_EQ(beforeLen, afterLen)
        << "this case is only meaningful if the edit is LENGTH-NEUTRAL — a "
           "length check must be unable to see it";
    EXPECT_TRUE(msg.empty())
        << "a same-length byte change is a REAL mutation and must be "
           "accepted; the instrument rejected it, so its difference check is "
           "keyed on size, not bytes.\nmessage was:\n"
        << msg;
}

}  // namespace
