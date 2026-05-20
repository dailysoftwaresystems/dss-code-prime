#pragma once

#include "core/export.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace dss {

// Loader-built / cursor-read taxonomy for one slot in a rule's compiled
// shape body. Exposed in `dss::` because it appears in the public cursor
// API (`GrammarSchema::slotKind`); the per-slot `Position` storage stays
// in `detail::`.
enum class SlotKind : std::uint8_t {
    TokenLeaf,   // expects `tokenId()`; advance(matching) → nextPos()
    RuleLeaf,    // expects FIRST(ruleId()); caller enters the rule then leaveRule resumes at nextPos()
    AltChoice,   // multiple branches; advance(tok) routes into the branch whose FIRST contains tok
    End,         // body completed
};

namespace detail {

// One position in a rule's compiled shape body. Built once by the loader
// via the named factories below; cursor reads through the const accessors.
// `tokenId`, `ruleId`, `nextPos`, and `branches` are slot-kind-dependent:
// they're only meaningful for the slot kind the factory targeted, and the
// factory enforces that pairing.
//
// `nullableTail` is the only field updated post-construction — the loader's
// fixed-point pass for canEndSource semantics flips it false→true.
class DSS_EXPORT Position {
public:
    Position() noexcept = default;   // default = End slot

    [[nodiscard]] static Position makeTokenLeaf(SchemaTokenId tok, std::uint32_t nextPos) {
        Position p;
        p.slotKind_ = SlotKind::TokenLeaf;
        p.tokenId_  = tok;
        p.nextPos_  = nextPos;
        p.expectedSet_.push_back(tok);
        return p;
    }

    [[nodiscard]] static Position makeRuleLeaf(RuleId rule, std::uint32_t nextPos,
                                               std::vector<SchemaTokenId> firstSet) {
        Position p;
        p.slotKind_    = SlotKind::RuleLeaf;
        p.ruleId_      = rule;
        p.nextPos_     = nextPos;
        p.expectedSet_ = std::move(firstSet);
        return p;
    }

    [[nodiscard]] static Position makeAltChoice(std::vector<std::uint32_t> branches,
                                                std::vector<SchemaTokenId> expectedSet) {
        Position p;
        p.slotKind_    = SlotKind::AltChoice;
        p.branches_    = std::move(branches);
        p.expectedSet_ = std::move(expectedSet);
        return p;
    }

    [[nodiscard]] static Position makeEnd() { return Position{}; }

    [[nodiscard]] SlotKind        slotKind()    const noexcept { return slotKind_; }
    [[nodiscard]] SchemaTokenId   tokenId()     const noexcept { return tokenId_; }
    [[nodiscard]] RuleId          ruleId()      const noexcept { return ruleId_; }
    [[nodiscard]] std::uint32_t   nextPos()     const noexcept { return nextPos_; }
    [[nodiscard]] std::span<std::uint32_t const> branches()   const noexcept { return branches_; }
    [[nodiscard]] std::span<SchemaTokenId const> expectedSet() const noexcept { return expectedSet_; }
    [[nodiscard]] bool            nullableTail() const noexcept { return nullableTail_; }

    // Speculative-alt attributes (only meaningful on AltChoice slots
    // built from an `"alt"` shape carrying `"speculative": true`). The
    // loader populates these; the cursor walker does NOT act on them.
    // The future parser/builder will read them to decide whether to
    // take a `TreeBuilder::Checkpoint` before exploring the branch.
    //
    // `speculative()` defaults to false (no speculation requested).
    // `lookahead()` is the per-branch token budget (default 8 when
    // `speculative` is set but `lookahead` is omitted).
    [[nodiscard]] bool            speculative() const noexcept { return speculative_; }
    [[nodiscard]] std::uint16_t   lookahead()   const noexcept { return lookahead_; }

    // The only post-construction mutators. `setNullableTail` is the
    // loader's `computeNullableTails` fixed-point; the speculative
    // setters land at AltChoice construction (kept separate from the
    // factory so the factory signature doesn't balloon with optional
    // fields most positions don't use).
    void setNullableTail(bool v) noexcept { nullableTail_ = v; }
    void setSpeculative(bool spec, std::uint16_t lookahead) noexcept {
        speculative_ = spec;
        lookahead_   = lookahead;
    }

private:
    SlotKind        slotKind_    = SlotKind::End;
    SchemaTokenId   tokenId_;
    RuleId          ruleId_;
    std::uint32_t   nextPos_     = 0;
    std::vector<std::uint32_t> branches_;
    std::vector<SchemaTokenId> expectedSet_;
    bool            nullableTail_ = false;
    bool            speculative_  = false;
    std::uint16_t   lookahead_    = 0;
};

struct CompiledRule {
    // Index into `positions` for the rule's entry point. `positions[0]`
    // is reserved as a sentinel so cursor `posId == 0` means "invalid",
    // matching the strong-id zero-is-invalid pattern.
    std::uint32_t              entryPos = 0;
    std::vector<Position>      positions;
    std::vector<SchemaTokenId> firstSet;
    bool                       nullable = false;
};

} // namespace dss::detail

} // namespace dss
