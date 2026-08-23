#pragma once

#include "core/types/config_path_walk.hpp"
#include "core/types/target_schema.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// D-CSUBSET-LOCAL-INT-CODEGEN-NEGATIVE-PIN substrate (cycle 10k,
// 2026-06-04): test-tier helper that loads a shipped target schema,
// strips named opcode rows, and returns a freshly-constructed
// TargetSchema built from the mutated JSON.
//
// Why this is test-tier: shipped configs are always-correct by
// construction (a misconfiguration would have been caught by CI long
// ago); negative-pin tests that exercise schema misconfiguration code
// paths need a way to *synthesize* a misconfiguration. The cleanest
// long-term shape is: don't author a parallel "broken" JSON file
// (would rot vs. shipped + couples tests to one specific shape);
// instead mutate the shipped file in-memory so the test pins the
// SUBSTRATE'S handling of an absent opcode, not a particular JSON
// shape.
//
// **Long-term design**: returns a `LoadResult<std::shared_ptr<...>>`
// — same envelope as `TargetSchema::loadFromText` so callers consume
// it with `ASSERT_TRUE(result.has_value())`. The helper does NOT
// silently degrade on a malformed shipped JSON (caller can inspect
// the error vector).
//
// **Agnosticism**: target-name-driven via `findShippedConfig`; works
// for any target (x86_64, arm64, etc.) by JSON-only addition. No
// hardcoded target string in this helper.
//
// ─────────────────────────────────────────────────────────────────
// ★★★ D-TEST-SCHEMA-MUTATION-HELPER-FAILS-OPEN — THE MUTATION
// CONTRACT (2026-08-14). THIS HELPER IS THE PROJECT'S PRIMARY
// DEFENCE AGAINST VACUOUS TESTS, AND IT USED TO BE FAIL-OPEN.
//
// Until this cycle the helper could mutate NOTHING and report
// success, by THREE separate routes:
//
//   1. A `removeMnemonics` entry that matched no opcode row was
//      SILENTLY ACCEPTED — the old docblock argued the caller's
//      intent ("ensure this mnemonic is absent") was satisfied by an
//      already-absent mnemonic. That argument is wrong, and the way
//      it is wrong is the whole point of this header: the CALLER is
//      not asserting absence, it is building a MUTANT whose only
//      job is to differ from the shipped schema. An unmutated mutant
//      makes the pin downstream of it assert nothing at all.
//   2. `erase(remove_if(...), end())` returned `end()` on no match —
//      no count, no error, byte-for-byte indistinguishable from a
//      successful removal.
//   3. An UNDOCUMENTED bare `return;` when the document carried no
//      `opcodes` array. A missing `opcodes` array is not a tolerable
//      shape, it means the caller pointed at the WRONG DOCUMENT.
//
// This is not theoretical. `tests/core/test_target_schema.cpp` (see
// its `⚠ THESE TWO USE x86_64, NOT arm64` note) records a null
// experiment this project already walked into: arm64 declares ZERO
// `implicitRegisters` blocks, so an arm64 mutation aimed at one was
// a no-op, and the "accepts prose" pin PASSED for the wrong reason —
// "it asserted nothing at all". That pin had to grow a hand-rolled
// `injected` flag to notice. Every consumer now gets that check for
// free, and gets it whether or not its author thought to write it.
//
// ★★ WHY A THROW AND NOT A DIAGNOSTIC IN THE `LoadResult`. This is
// the load-bearing design decision here, and getting it wrong would
// have rebuilt the defect inside its own fix. TWELVE call sites
// assert `EXPECT_FALSE(mutated.has_value())` — they are pins whose
// subject is the LOADER REJECTING the mutant (an ambiguous width
// axis, a typo'd key, a dangling cond-code). If a
// "mutation matched nothing" were reported as `std::unexpected`,
// every one of those pins would go GREEN over a document that was
// never mutated, and the failure would be indistinguishable from
// the rejection they exist to prove. The contract violation must
// therefore be signalled on a channel NO consumer can confuse with
// the schema's own verdict, and must be non-continuable.
//
// A throw is exactly that channel, and it is this repo's established
// test-tier convention for "the fixture itself is broken" — see
// `repo_root.hpp` ("GoogleTest reports a throw as a failure of that
// ONE test") and `scratch_dir.hpp`. The type is DEDICATED rather
// than a bare `std::runtime_error` so the self-tests can catch
// precisely what they aimed at: a self-test that caught
// `std::runtime_error` could pass on an unrelated failure from
// nlohmann or the filesystem, which is the same "green for the
// wrong reason" bug one layer up.
//
// ★★ WHY THERE IS NO OPT-OUT. There is deliberately no
// `mustExist=false` / `allowMissing` escape hatch. All 54 shipped
// call sites were audited when this contract landed and NONE needs
// one (every requested mnemonic resolves in the target it mutates).
// An unwitnessed opt-out would be a speculative build whose only
// effect is to hand a future author a one-token way back to the
// exact fail-open being removed here — and the cheapest thing to
// reach for when a mutation unexpectedly throws is precisely the
// flag that silences it. If a genuinely optional mutation ever
// appears, it arrives WITH its witness and the contract is widened
// then, on purpose, with the reasoning recorded.

namespace dss::test_support {

// ★ A violation of the mutation CONTRACT — never a verdict about the
// schema. Thrown, never returned: see the `LoadResult` rationale
// above. Dedicated type so a `catch` can name exactly this fault.
class TargetSchemaMutationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {

// Shared load+parse half of the two mutation entry points below.
// Returns the parsed shipped JSON document, or the failure envelope.
[[nodiscard]] inline LoadResult<nlohmann::json>
parseShippedTargetJson(std::string_view targetName) {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{targetName, "targets", ".target.json",
                             "target", DiagnosticCode::C_InvalidTargetName});
    if (!pathR.has_value()) {
        return std::unexpected(std::move(pathR).error());
    }

    std::ifstream in{*pathR};
    if (!in.is_open()) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_MissingField, DiagnosticSeverity::Error,
             pathR->string(), "parseShippedTargetJson: cannot open "
                              "shipped target JSON"}});
    }
    std::ostringstream buf;
    buf << in.rdbuf();

    try {
        return nlohmann::json::parse(buf.str());
    } catch (nlohmann::json::parse_error const& e) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_InvalidSemantics, DiagnosticSeverity::Error,
             pathR->string(),
             std::string{"parseShippedTargetJson: JSON parse error in "
                         "shipped schema: "} + e.what()}});
    }
}

// Comma-joined list of the document's top-level keys — the "what the
// document actually contained" half of a missing-container message.
[[nodiscard]] inline std::string topLevelKeys(nlohmann::json const& doc) {
    if (!doc.is_object()) {
        return std::string{"<not a JSON object: "} + doc.type_name() + ">";
    }
    std::string out;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (!out.empty()) {
            out += ", ";
        }
        out += it.key();
    }
    return out.empty() ? std::string{"<none>"} : out;
}

// Spelling neighbourhood of a mnemonic that was NOT found: the rows
// whose mnemonic shares a 3-character prefix with, contains, or is
// contained by the requested one. A typo ("popcnt" for "popcount",
// "bswap32" for "bswap") is the overwhelmingly likely cause of a
// no-match, so the message that reports the miss should carry the
// near-hits rather than making the reader open the JSON.
[[nodiscard]] inline std::string
mnemonicNeighbourhood(nlohmann::json const& opcodes, std::string_view wanted) {
    constexpr std::size_t kMaxShown = 8;
    std::vector<std::string> near;
    std::unordered_set<std::string> seen;
    for (auto const& entry : opcodes) {
        auto it = entry.find("mnemonic");
        if (it == entry.end() || !it->is_string()) {
            continue;
        }
        std::string const m = it->get<std::string>();
        bool const sharesPrefix =
            m.size() >= 3 && wanted.size() >= 3 &&
            std::string_view{m}.substr(0, 3) == wanted.substr(0, 3);
        bool const overlaps = m.find(wanted) != std::string::npos ||
                              wanted.find(m) != std::string_view::npos;
        if ((sharesPrefix || overlaps) && seen.insert(m).second) {
            near.push_back(m);
        }
        if (near.size() >= kMaxShown) {
            break;
        }
    }
    if (near.empty()) {
        return std::string{"no similarly-spelled mnemonic in this target"};
    }
    std::string out{"nearest spellings present: "};
    for (std::size_t i = 0; i < near.size(); ++i) {
        out += (i == 0 ? "" : ", ");
        out += near[i];
    }
    return out;
}

// ★ The opcode-row removal transform, factored out of the entry
// point below so it can be aimed at a HAND-BUILT document. That is
// not a convenience: the "document carries no `opcodes` array" arm is
// unreachable through the public entry point (every shipped target
// has one), so without this seam that arm could only be READ, never
// EXERCISED — and this repo has been bitten by a failure arm that was
// read rather than run ("a suite printing failed=0 had been exiting 2
// for weeks"). `tests/test_support/test_mutate_target_schema.cpp`
// drives it directly.
//
// `targetLabel` is for diagnostics only; it never steers behaviour.
inline void eraseOpcodeRows(nlohmann::json& doc,
                            std::vector<std::string> const& removeMnemonics,
                            std::string_view targetLabel) {
    if (removeMnemonics.empty()) {
        throw TargetSchemaMutationError{
            std::string{"mutateShippedTargetSchemaJson(\""} +
            std::string{targetLabel} +
            "\"): the removal list is EMPTY, so the mutant would be "
            "byte-identical to the shipped schema and the pin consuming "
            "it would assert nothing. Name at least one mnemonic."};
    }

    // ★ A missing `opcodes` array means the caller pointed at the
    // WRONG DOCUMENT — it is never a shape to tolerate. This used to
    // be an undocumented bare `return;`, i.e. the quietest of the
    // three fail-open routes: no removal, no error, a perfectly
    // loadable schema handed back.
    auto const opcodesIt = doc.find("opcodes");
    if (opcodesIt == doc.end() || !opcodesIt->is_array()) {
        throw TargetSchemaMutationError{
            std::string{"mutateShippedTargetSchemaJson(\""} +
            std::string{targetLabel} + "\"): the document has no `opcodes` "
            "ARRAY to remove rows from (found: " +
            (opcodesIt == doc.end()
                 ? std::string{"no `opcodes` key at all"}
                 : std::string{"`opcodes` is a "} + opcodesIt->type_name()) +
            "). Top-level keys present: " + topLevelKeys(doc) +
            ". This is a wrong-document error, not a tolerable shape."};
    }

    auto& opcodes = *opcodesIt;

    // Count removals PER REQUESTED MNEMONIC. The old code kept no
    // count, and `erase(remove_if(...), end())` is `end()` on both a
    // successful and an empty removal — the two outcomes were
    // literally the same value.
    std::unordered_map<std::string, std::size_t> removed;
    for (std::string const& m : removeMnemonics) {
        removed.emplace(m, 0U);
    }

    auto const newEnd = std::remove_if(
        opcodes.begin(), opcodes.end(),
        [&removed](nlohmann::json const& entry) {
            auto it = entry.find("mnemonic");
            if (it == entry.end() || !it->is_string()) {
                return false;
            }
            auto hit = removed.find(it->get<std::string>());
            if (hit == removed.end()) {
                return false;
            }
            ++hit->second;
            return true;
        });
    opcodes.erase(newEnd, opcodes.end());

    // Report EVERY mnemonic that matched nothing in one message —
    // fixing them one round-trip at a time is a waste of the reader's
    // build.
    std::vector<std::string> missed;
    for (std::string const& m : removeMnemonics) {
        if (removed[m] == 0U) {
            missed.push_back(m);
        }
    }
    if (missed.empty()) {
        return;
    }

    std::string msg{"mutateShippedTargetSchemaJson(\""};
    msg += std::string{targetLabel};
    msg += "\"): the mutation matched NOTHING — the mutant would be "
           "byte-identical to the shipped schema.\n";
    for (std::string const& m : missed) {
        msg += "  requested removal of mnemonic \"" + m +
               "\", which this target does not declare (" +
               mnemonicNeighbourhood(opcodes, m) + ")\n";
    }
    msg += "  the document declares " + std::to_string(opcodes.size()) +
           " opcode row(s) after the removal walk.\n"
           "A mutation that matches nothing leaves the schema UNCHANGED, so "
           "the pin consuming this mutant asserts nothing at all. Name a "
           "mnemonic this target really has, or aim the test at the target "
           "that has it.";
    throw TargetSchemaMutationError{msg};
}

}  // namespace detail

// Generalized in-memory schema mutation (FC1 V2-4.X, 2026-06-10):
// load the shipped target JSON, hand the parsed document to `mutate`
// (any in-place transform), re-construct a `TargetSchema` from the
// mutated text. The remove-mnemonics helper below is the common
// special case; tests needing finer-grained surgery (e.g. stripping
// ONE sub-key off one opcode's `implicitRegisters` to exercise a
// lowering fail-loud arm) pass a lambda instead of authoring a
// parallel broken JSON file (which would rot against the shipped
// one — the cycle-10k rationale above applies unchanged).
//
// ★★ THROWS `TargetSchemaMutationError` if `mutate` left the document
// BYTE-IDENTICAL. This is the "mutant DIFFERS byte-wise" fail-closed
// check, applied at the one place every consumer passes through, so
// no consumer has to remember to write it.
//
// ⚠ It is a BYTE comparison of the exact text handed to
// `loadFromText`, never a line/size/row count: a same-length
// replacement (re-pointing a wire at a different slot of equal name
// length, flipping a width 32→64) walks straight past a length or
// count check while being precisely the mutation the pin needs. The
// two dumps are the canonical serialization of the document, so key
// insertion ORDER cannot manufacture a false difference either — an
// add-then-remove round trip correctly reports NO-OP.
template <typename MutateFn>
[[nodiscard]] inline LoadResult<std::shared_ptr<TargetSchema>>
mutateShippedTargetSchemaDoc(std::string_view targetName,
                             MutateFn&& mutate) {
    auto docR = detail::parseShippedTargetJson(targetName);
    if (!docR.has_value()) {
        return std::unexpected(std::move(docR).error());
    }
    nlohmann::json doc = *std::move(docR);

    std::string const before = doc.dump();
    std::forward<MutateFn>(mutate)(doc);
    std::string after = doc.dump();

    if (after == before) {
        throw TargetSchemaMutationError{
            std::string{"mutateShippedTargetSchemaDoc(\""} +
            std::string{targetName} +
            "\"): the mutation was a NO-OP — the document is byte-identical "
            "(" + std::to_string(before.size()) +
            " bytes) before and after `mutate` ran, so the \"mutant\" IS the "
            "shipped schema and the pin consuming it asserts nothing at all.\n"
            "The usual cause is a navigator that never reached its container: "
            "the key, opcode or variant the lambda searched for does not "
            "exist in THIS target (containers differ per target — e.g. arm64 "
            "declares no `implicitRegisters` at all). Aim the lambda at a "
            "container this target really has, or aim the test at the target "
            "that has it."};
    }

    // ⚠ NOT `std::move(after)`: `loadFromText` takes a `std::string_view`, so
    // a move would move nothing and merely read as though it had. `after` is
    // a named local precisely so the viewed buffer outlives the call — the
    // previous `loadFromText(doc.dump(), ...)` relied on a temporary living
    // to the end of the full-expression, which is correct but far more
    // fragile than it looks.
    return TargetSchema::loadFromText(after,
        std::string{"<mutated "} + std::string{targetName} + ">");
}

// Load the shipped target schema for `targetName`, parse its JSON,
// remove every opcode row whose `mnemonic` field matches any entry
// in `removeMnemonics`, and re-construct a `TargetSchema` from the
// resulting JSON text. Returns the schema on success or a
// `ConfigDiagnostic` vector on failure (mirrors
// `TargetSchema::loadFromText`'s envelope).
//
// ★★ THROWS `TargetSchemaMutationError` — never returns a diagnostic
// — if any requested mnemonic matched no opcode row, if the list is
// empty, or if the document carries no `opcodes` array. A mutation
// that removes nothing is the fail-open this header exists to
// prevent; see the contract note at the top for why this cannot ride
// in the `LoadResult`.
//
// Use cases:
//   - Negative-pin tests for `materializeCallingConvention`'s
//     "schema declares X but not Y" fail-loud arms.
//   - Future cycle's regression guards against silent-failure
//     classes triggered by absent opcodes.
[[nodiscard]] inline LoadResult<std::shared_ptr<TargetSchema>>
mutateShippedTargetSchemaJson(
    std::string_view targetName,
    std::initializer_list<std::string_view> removeMnemonics) {

    // Caller order is preserved (not a set) so the "matched nothing"
    // message lists the misses in the order they were written.
    std::vector<std::string> drop;
    drop.reserve(removeMnemonics.size());
    for (std::string_view const m : removeMnemonics) {
        drop.emplace_back(m);
    }

    return mutateShippedTargetSchemaDoc(targetName,
        [&drop, targetName](nlohmann::json& doc) {
            detail::eraseOpcodeRows(doc, drop, targetName);
        });
}

} // namespace dss::test_support
