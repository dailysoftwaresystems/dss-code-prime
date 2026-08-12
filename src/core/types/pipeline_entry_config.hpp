#pragma once

#include "core/export.hpp"
#include "core/types/rule_id.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Per-RULE pipeline entry tier (schema v4 `pipelineEntry` block; plan 29).
//
// ★★★ WHAT IT DECLARES: which tier of the CST→HIR→MIR→LIR pipeline a given
// construct ENTERS at. Not every construct starts at the top. A `.s`
// translation unit is already the post-register-allocation, physical-register
// machine tier that LIR models — running it through HIR, MIR and the optimizer
// would be re-deriving what the programmer already wrote, and OPTIMIZING it
// would be a miscompile of intent: you cannot know why a human chose a given
// instruction, order or register.
//
// ★★★ WHY IT IS PER-RULE AND NOT PER-LANGUAGE, which is the whole design.
// Plan 29 §1 rejected a language-level `skipToAssembler` flag and stated the
// reason precisely: *"the pipeline entry point is a property of the CONSTRUCT,
// never of the language."* An embedded `__asm__(…)` inside C is the proof — its
// template text is opaque and belongs to the assembler, but its operand
// bindings and clobber set are C lvalues that MUST survive as IR the register
// allocator can see. The tier boundary runs THROUGH that construct, so a
// language-wide switch would be a silent miscompile (the allocator keeps a
// value live in a register the asm block clobbers, with no diagnostic). Now
// that the asm language has TWO entry points — `asmStmt` embedded in a host,
// and eventually an `asmUnit` root for a standalone `.s` — a per-ENTRY-POINT
// declaration is exactly what that sentence prescribes.
//
// ⚠ THERE IS DELIBERATELY NO `optimize` SIBLING KEY, AND ADDING ONE WOULD BE A
// REGRESSION. Entering at LIR means no MIR module exists, so MIR passes cannot
// run BY CONSTRUCTION — "no optimization" is a CONSEQUENCE of the tier, not a
// second switch. Two sources of truth for one property is how they drift
// apart, and the drift would be silent in the dangerous direction (a config
// saying `optimize: true` at an LIR entry would read as a promise the pipeline
// shape cannot keep).
//
// ★ NO IDENTITY BRANCH ANYWHERE. The tier is a closed enum resolved at load;
// the engine dispatches on the enum and never reads the language name. Any
// language may declare any tier — this is generic pipeline vocabulary that
// happens to have asm as its first consumer.
//
// ★★ THE DEFAULT IS THE FULL PIPELINE, AND IT IS A SILENT DEFAULT THAT ONLY
// CLOSED KEY SETS MAKE SAFE (operator decision 2026-08-12: *"a language
// without this specification (like C or any other) follows the original
// pipeline"*). The block is OPTIONAL and the override is SPARSE — a language
// that declares nothing, and a rule absent from `byRule` in a language that
// does, both take CST→HIR→MIR→LIR exactly as before. This project normally
// refuses "absent means default", and the ONLY thing that makes it acceptable
// here is that a misspelling cannot reach the default silently:
//   1. `pipelineEntry` is in the `.lang.json` closed ROOT key set, so
//      `pipelineEntries` is a load error rather than a whole facet dropped;
//   2. the facet's own keys are closed, so a typo'd `byRule` is a load error
//      rather than "no overrides";
//   3. each row requires `rule` and `tier`, and `tier` comes from the closed
//      set above.
// ⚠ DO NOT "SIMPLIFY" ANY OF THE THREE. Drop one and a typo makes hand-written
// assembly take the full pipeline — through MIR, through the optimizer — which
// is a SILENT MISCOMPILE of the programmer's intent and the exact failure this
// facet exists to prevent.
//
// LAYERING: lives in `core` (like `hir_lowering_config.hpp` and
// `semantic_config.hpp`) so `GrammarSchema` can own it, and names no
// `hir`/`mir`/`lir`-layer type — only the tier enum and a RuleId.

namespace dss {

// The closed tier vocabulary. Kept an `enum class` with no catch-all member on
// purpose: a consumer `switch` without a `default:` is then a COMPILE error
// when a tier is added, which is a stronger fail-loud than any runtime check
// this header could offer. A tier whose entry path a given build has not
// implemented must be refused AT USE with a precise diagnostic — never
// silently demoted to another tier, which would compile the wrong thing.
enum class PipelineTier : std::uint8_t {
    Hir,   // CST → HIR → MIR → LIR (the ordinary path every language takes)
    Mir,   // enters at MIR: no CST→HIR lowering for this construct
    Lir,   // enters at LIR: no HIR, no MIR, and therefore no MIR optimizer
};

// The config spelling of each tier, in ONE table so the parser and any
// diagnostic that has to list the legal values cannot disagree.
inline constexpr std::array<std::pair<std::string_view, PipelineTier>, 3>
    kPipelineTierNames{{
        {"hir", PipelineTier::Hir},
        {"mir", PipelineTier::Mir},
        {"lir", PipelineTier::Lir},
    }};

[[nodiscard]] inline std::optional<PipelineTier>
pipelineTierFromName(std::string_view name) {
    for (auto const& [text, tier] : kPipelineTierNames) {
        if (text == name) return tier;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string_view pipelineTierName(PipelineTier tier) {
    for (auto const& [text, t] : kPipelineTierNames) {
        if (t == tier) return text;
    }
    return "<unknown>";   // unreachable for a value produced by the parser
}

// The legal spellings, comma-joined — for the load diagnostic that rejects an
// unknown one. Built from the same table, so it can never list a stale set.
[[nodiscard]] inline std::string pipelineTierNameList() {
    std::string out;
    for (auto const& [text, tier] : kPipelineTierNames) {
        (void)tier;
        if (!out.empty()) out += ", ";
        out += '\'';
        out += text;
        out += '\'';
    }
    return out;
}

// One `pipelineEntry.byRule` row: this rule's construct enters at this tier.
struct PipelineEntryRow {
    RuleId       rule{};
    std::string  ruleName;    // kept for diagnostics that must name the rule
    PipelineTier tier{PipelineTier::Hir};
};

struct DSS_EXPORT PipelineEntryConfig {
    std::vector<PipelineEntryRow> byRule;

    // The declared tier for a rule, or nullopt when the rule declares none.
    // ⚠ NULLOPT IS NOT "Hir" — the two are different statements and the
    // difference is load-bearing. "This rule declared nothing" is a question
    // for the caller's own default; returning `Hir` here would silently
    // answer it, and a caller that genuinely needs to know whether a
    // declaration EXISTS (e.g. to refuse an unimplemented entry path) could
    // no longer tell. Small vector (one row per entry point), linear scan.
    [[nodiscard]] std::optional<PipelineTier> tierForRule(RuleId rule) const {
        auto const it = std::ranges::find_if(
            byRule, [&](PipelineEntryRow const& r) { return r.rule.v == rule.v; });
        if (it == byRule.end()) return std::nullopt;
        return it->tier;
    }
};

} // namespace dss
