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

    // The only post-construction mutator — called by the loader's
    // `computeNullableTails` fixed-point. `canEndSource(cursor)` reads
    // this directly.
    void setNullableTail(bool v) noexcept { nullableTail_ = v; }

private:
    SlotKind        slotKind_    = SlotKind::End;
    SchemaTokenId   tokenId_;
    RuleId          ruleId_;
    std::uint32_t   nextPos_     = 0;
    std::vector<std::uint32_t> branches_;
    std::vector<SchemaTokenId> expectedSet_;
    bool            nullableTail_ = false;
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
