#pragma once

#include "core/export.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <vector>

namespace dss::detail {

// A position in a rule's compiled shape body. Built once by the loader,
// queried by the schema cursor's `advance` / `expectedSet` walk.
//
// Each Position records what kind of slot occupies the current step,
// where the cursor moves on a successful match, and the precomputed
// FIRST set of tokens that could match here. AltChoice positions also
// record the per-branch starting position so `advance` can route into
// the correct branch given the input token.
//
// `End` positions terminate a rule's body — a cursor here is at end-of-
// rule and can either be popped by the caller (via `leaveRule`) or
// reported as a source-acceptable position when at the root.

enum class SlotKind : std::uint8_t {
    TokenLeaf,   // expects `tokenId`; advance(matching) → nextPos
    RuleLeaf,    // expects FIRST(ruleId); caller enters the rule, then leaveRule resumes at nextPos
    AltChoice,   // multiple branches; advance(tok) routes into the branch whose FIRST contains tok
    End,         // body completed
};

struct Position {
    SlotKind        slotKind     = SlotKind::End;
    SchemaTokenId   tokenId;            // TokenLeaf
    RuleId          ruleId;             // RuleLeaf
    std::uint32_t   nextPos      = 0;   // for TokenLeaf, RuleLeaf — successor after match
    // For AltChoice: each branch's starting position. branches[i] is the
    // position to jump to if the input token belongs to branches[i]'s
    // FIRST set. End-of-branch flows back to this position's continuation
    // (handled by each branch's own `nextPos` chain).
    std::vector<std::uint32_t> branches;
    // Precomputed FIRST set of tokens valid at this position. Cascades
    // through nullable prefixes — e.g. for sequence step k whose head is
    // nullable, expectedSet includes FIRST(step k+1).
    std::vector<SchemaTokenId> expectedSet;
    // True when the rule body can complete from this position without
    // consuming any further token. `canEndSource()` on the root rule
    // returns this flag directly; mid-rule positions use it via parent
    // composition.
    bool nullableTail = false;
};

struct CompiledRule {
    // Index into `positions` for the rule's entry point. Positions[0] is
    // a sentinel used to make 0 mean "invalid position-id" for cursors,
    // mirroring the strong-id pattern where 0 == invalid.
    std::uint32_t              entryPos = 0;
    std::vector<Position>      positions;
    // FIRST set of the rule itself — the union of its body's entry-point
    // expectedSet. Exposed publicly via GrammarSchema::firstSetOf().
    std::vector<SchemaTokenId> firstSet;
    bool                       nullable = false;
};

} // namespace dss::detail
