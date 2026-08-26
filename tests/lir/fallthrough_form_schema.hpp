#pragma once

// D-OPT-JCC-FALLTHROUGH — the ONE fixture that gives a test a target which
// does NOT spell the fallthrough form of its conditional branch, so the pass
// and the verifier can both be shown refusing to use one.
//
// ★★★ WHY THE MUTATION RUNS IN THIS DIRECTION, AND WHY IT USED TO RUN IN THE
// OTHER ONE. `lir_peephole`'s rule R2 elides a branch's trailing fallthrough
// operand only where the target declares an encoding variant for the shorter
// operand tuple, and `lir_verifier`'s Rule 1b accepts the shorter list only
// under the same condition. Both halves therefore need a target that HAS the
// form and a target that HASN'T — and exactly one of those two can be the
// shipped document, so the OTHER one has to be built in memory.
//
// Until the shipped `.target.json` documents grew the variant, the shipped
// document was the one that HADN'T, and this fixture synthesized the one that
// HAD. That is now backwards: `x86_64.target.json` and `arm64.target.json`
// both declare the one-blockref `jcc` variant, so the shipped document is the
// POSITIVE case and the negative is what must be synthesized — by REMOVAL.
//
// ⚠ THE INVERSION IS NOT COSMETIC, AND SKIPPING IT IS NOT AN OPTION. ✔MEASURED
// 2026-08-26, before the config edit landed: applying the two `.target.json`
// edits with this fixture still adding a variant turned ELEVEN tests red across
// `test_lir_peephole.cpp` (6) and `test_lir_text.cpp` (5) — not on their own
// assertions but on a LOAD REFUSAL, because appending a second one-blockref
// variant to a document that already had one produced a schema the loader
// rejects. Every test that proves R2 and Rule 1b work would have been disabled
// by the very edit that makes them live.
//
// ⚠ THE MUTATION IS TARGET-BLIND ON PURPOSE. It filters the opcode's OWN
// variants by guard arity and restores the operand floor; nothing here knows
// that x86_64's long form bridges two `rel32` wires with `prefixOpcodeBytes`
// while arm64's collapses a two-word `fixedWords` macro. That is the same
// property the production rule claims, exercised by the fixture that tests it.
//
// ★ THE NO-OP CONTRACT NOW GUARDS THE CONFIG EDIT ITSELF, which is the reason
// the removal direction is strictly safer than the addition it replaces.
// `mutateShippedTargetSchemaDoc` compares the canonical dump before and after
// and THROWS when they match, so if the one-blockref variant is ever dropped
// from a shipped document, the removal becomes a no-op and every consumer of
// this fixture fails loudly — instead of quietly testing the schema against
// itself. Under the old direction that same accident was INVISIBLE: an add
// always changes the document, so the fixture kept reporting green over a
// config tree that had lost the feature.
//
// It is shared by `test_lir_peephole.cpp` (the transform) and
// `test_lir_text.cpp` (the verifier pin) so the two cannot drift into
// disagreeing about what "a target with the fallthrough form" means.

#include "core/types/target_schema.hpp"
#include "lir/lir_pass_util.hpp"
#include "mutate_target_schema.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace dss::test_support {

// Strip the FALLTHROUGH FORM of `jcc` from a parsed target document: drop
// every encoding variant guarded on a single operand, and put the operand
// floor back where the two-operand form alone would leave it. The inverse,
// key for key, of the edit the shipped `.target.json` documents carry.
inline void removeFallthroughJccVariant(nlohmann::json& doc) {
    for (auto& o : doc.at("opcodes")) {
        if (!o.is_object()) continue;
        if (o.value("mnemonic", std::string{}) != "jcc") continue;
        auto& variants = o.at("encoding").at("variants");
        nlohmann::json kept = nlohmann::json::array();
        for (auto const& v : variants) {
            // The fallthrough form is the one that names ONE target block.
            // Arity is the whole selector — the same key the encoder's
            // variant walker uses — so nothing here has to recognize an
            // opcode byte or a fixed word.
            if (v.at("guard").at("operandKinds").size() == 1) continue;
            kept.push_back(v);
        }
        variants = std::move(kept);
        o["minOperands"] = 2;
    }
}

// ⚠ THROWS rather than `abort()`s — a throw fails the ONE test that hit it,
// an abort loses every sibling result in the executable.
[[nodiscard]] inline std::shared_ptr<TargetSchema>
schemaWithoutFallthroughForm(std::string_view target) {
    auto r = mutateShippedTargetSchemaDoc(target, removeFallthroughJccVariant);
    if (!r.has_value()) {
        throw std::runtime_error(
            std::string("a target with the D-OPT-JCC-FALLTHROUGH fallthrough "
                        "encoding form REMOVED must still LOAD; ")
            + std::string(target) + " did not");
    }
    return *r;
}

// The POSITIVE case is the shipped document itself — but only as long as it
// really carries the form, which is a property of a `.target.json` file and
// therefore a property that can be edited away. Ask the ONE shared predicate
// (`lir_pass_util::declaresFallthroughBranchForm`, which is also what the
// pass and the verifier consult) rather than trusting the file, so a reverted
// or mis-merged config edit is a loud failure here instead of a silent
// nothing-to-elide everywhere downstream.
[[nodiscard]] inline std::shared_ptr<TargetSchema>
schemaWithFallthroughForm(std::string_view target) {
    auto r = TargetSchema::loadShipped(target);
    if (!r.has_value()) {
        throw std::runtime_error(
            std::string("the shipped target must LOAD; ")
            + std::string(target) + " did not");
    }
    auto const jcc = (*r)->opcodeByMnemonic("jcc");
    if (!jcc.has_value()
        || !lir_pass_util::declaresFallthroughBranchForm(**r, *jcc, 2)) {
        throw std::runtime_error(
            std::string("the SHIPPED ") + std::string(target)
            + " target no longer declares the D-OPT-JCC-FALLTHROUGH "
              "fallthrough encoding form of `jcc`. The one-blockref variant "
              "in its `.target.json` is what rule R2 elides TO — without it "
              "R2 is a no-op and this fixture would be testing the schema "
              "against itself.");
    }
    return *r;
}

}  // namespace dss::test_support
